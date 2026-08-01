// Created by yakov on 30/07/2026.

#include "ProtocolManager.h"

#include <cstring>
#include <sys/epoll.h>

#include "Logger/Logger.h"
#include "ModuleFactory/ModuleFactory.h"
#include "Server/Server.h"

constexpr uint32_t DEFAULT_MAX_BUFFER = 1024 * 16;
constexpr uint32_t DEFAULT_MAX_QUEUED_CHUNKS = 4;
constexpr uint32_t DEFAULT_COMMAND_THREADS = 2;
constexpr uint32_t DEFAULT_STREAM_THREADS = 4;


/**
 * Initializes the protocol manager.
 *
 * Loads protocol configuration, creates worker thread pools,
 * loads all available modules and starts the asynchronous
 * stream event handler thread.
 *
 * @param configManager Configuration manager containing protocol
 *                      and module configurations.
 * @param epollFd Epoll instance used to notify writable sockets.
 */
ProtocolManager::ProtocolManager(ConfigManager& configManager, const int epollFd) :
	m_streamPool(0), m_commandPool(0), m_maxBuffer(),
	m_maxStreamChunks(0), m_epollFd(epollFd), m_streamChannel(std::make_shared<StreamChannel>())
{

	// Config parsing
	ConfigFile* config = configManager.getConfig("Protocol");
	m_maxBuffer = config->getInt(SETTINGS_SECTION, "MaxBufferSize", DEFAULT_MAX_BUFFER);
	m_maxStreamChunks = config->getInt(SETTINGS_SECTION, "MaxQueuedStreamChunks", 4);
	m_commandPool.increaseThreads(config->getInt(SETTINGS_SECTION, "CommandThreads", DEFAULT_COMMAND_THREADS));
	m_streamPool.increaseThreads(config->getInt(SETTINGS_SECTION, "StreamThreads", DEFAULT_STREAM_THREADS));


	// Debug
	LOG_DEBUG("Protocol MaxBufferSize: " + std::to_string(m_maxBuffer));
	LOG_DEBUG("Protocol CommandThreads: " + std::to_string(commandThreads));
	LOG_DEBUG("Protocol StreamThreads: " + std::to_string(streamThreads));
	LOG_DEBUG("Protocol MaxQueuedStreamChunks: " + std::to_string(m_maxStreamChunks));

	configManager.removeConfig("Protocol");

	// Module creation
	auto& configs = configManager.getConfigs();

	for (auto& [moduleName, moduleConfig]: configs)
	{
		auto module = ModuleFactory::create(moduleName, moduleConfig, m_streamChannel);

		if (!module)
			throw std::runtime_error("Failed to initialize module: " + moduleName);

		Logger::get().setup("Loaded module: " + moduleName);

		auto* stream = dynamic_cast<StreamModule*>(module.get());
		m_modules[moduleName] = {std::move(module), stream};
	}

	m_running.store(true);
	m_streamHandler = std::thread(&ProtocolManager::streamEventHandler, this);

	Logger::get().setup("Protocol manager initialized");
}

/**
 * Stops the stream handler thread and shuts down
 * the protocol manager.
 */
ProtocolManager::~ProtocolManager()
{
	m_running.store(false);
	m_streamChannel->condition.notify_all();

	if (m_streamHandler.joinable())
		m_streamHandler.join();
}

/**
 * Registers a newly connected client.
 *
 * @param client Client context.
 */
void ProtocolManager::createClient(const std::shared_ptr<ClientContext>& client)
{
	std::lock_guard lock(m_clientsMtx);
	m_clients.emplace(client->clientID, client);
}

/**
 * Removes a client from the protocol manager.
 *
 * This should be called after the connection has been closed.
 *
 * @param id Client identifier.
 */
void ProtocolManager::removeUser(const uint64_t id)
{
	std::lock_guard lock(m_clientsMtx);
	m_clients.erase(id);
}

/**
 * Returns the remaining writable space in the client's
 * input buffer.
 *
 * Returns zero if the client no longer exists or the
 * input buffer is already full.
 *
 * @param id Client identifier.
 * @return Remaining writable bytes.
 */
uint16_t ProtocolManager::getAvailableInputSpace(const uint64_t id)
{
	std::shared_ptr<ClientContext> client;

	{
		std::shared_lock lock(m_clientsMtx);
		const auto it = m_clients.find(id);
		if (it == m_clients.end())
			return 0;

		client = it->second;
	}

	std::lock_guard lock(client->io.inputMutex);
	if (client->io.inputBuffer.size() >= m_maxBuffer)
		return 0;

	return m_maxBuffer - client->io.inputBuffer.size();
}

/**
 * Processes newly received data for a client.
 *
 * Automatically dispatches the client to either command
 * parsing or stream processing depending on the current
 * protocol state.
 *
 * @param id Client identifier.
 */
void ProtocolManager::process(const uint64_t id)
{
	std::shared_ptr<ClientContext> client;
	{
		std::shared_lock lock(m_clientsMtx);
		const auto it = m_clients.find(id);
		if (it == m_clients.end())
			return;

		client = it->second;
	}

	bool isCommand;
	{
		std::lock_guard lock(client->stream.streamMutex);
		isCommand = client->stream.streamService.empty();
	}

	if (isCommand)
		processCommand(client);
	else
		processStream(client);
}

/**
 * Parses complete command packets from the client's
 * input buffer and schedules them for execution.
 *
 * Incomplete packets remain buffered until more data
 * arrives.
 *
 * @param client Client context.
 */
void ProtocolManager::processCommand(const std::shared_ptr<ClientContext>& client)
{
	std::lock_guard lock(client->io.inputMutex);

	ClientIO& io = client->io;

	size_t start = 0;

	if (io.discardBytes != 0)
	{
		const size_t consumed = std::min(io.inputBuffer.size(), io.discardBytes);
		start = consumed;
		io.discardBytes -= consumed;
	}

	while (true)
	{
		if (io.inputBuffer.size() - start < sizeof(uint16_t))
			break;

		uint16_t size;
		std::memcpy(&size, io.inputBuffer.data() + start, sizeof(size));
		if (size > m_maxBuffer) // Current packet size is bigger than max buffer size - drop packet and mark the rest invalid
		{
			const size_t available = io.inputBuffer.size() - start - sizeof(size);
			io.discardBytes = size - available;
			io.inputBuffer.clear();
			return;
		}

		start += sizeof(size);

		if (io.inputBuffer.size() - start < size) // Not enough data
			break;

		std::stringstream ss(io.inputBuffer.substr(start, size)); // Extract the command

		start += size;

		CommandRequest cmd{};
		if (!(ss >> cmd.service >> cmd.action))
			continue;

		std::string arg;
		while (ss >> arg)
			cmd.args.push_back(arg);

		cmd.clientID = client->clientID;

		auto task = [this, cmd = std::move(cmd), client = std::weak_ptr(client)]() mutable
		{
			executeCommand(cmd, client);
		};

		m_commandPool.schedule(client->clientID, task, nullptr);
	}

	io.inputBuffer.erase(0, start);
}

/**
 * Parses stream packets from the client's input buffer
 * and schedules stream processing tasks.
 *
 * Limits the number of queued stream chunks for each
 * client to avoid unbounded memory usage.
 *
 * @param client Client context.
 */
void ProtocolManager::processStream(const std::shared_ptr<ClientContext>& client)
{
	std::lock_guard lock(client->io.inputMutex);

	ClientIO& io = client->io;

	size_t start = 0;
	while (true)
	{
		if (m_streamPool.getCurrentTasksCount(client->clientID) >= m_maxStreamChunks)
			break;

		if (io.inputBuffer.size() - start < sizeof(uint16_t))
			break;


		StreamEvent streamEvent{client->clientID};
		{
			std::lock_guard sLock(client->stream.streamMutex);
			streamEvent.service = client->stream.streamService;
		}

		uint16_t size;
		std::memcpy(&size, io.inputBuffer.data() + start, sizeof(size));
		if (size > m_maxBuffer) // Current packet size is bigger than max buffer size - drop packet and mark the rest invalid
		{
			const size_t available = io.inputBuffer.size() - start - sizeof(size);
			io.discardBytes = size - available;
			io.inputBuffer.clear();
			return;
		}

		start += sizeof(size);

		if (io.inputBuffer.size() - start < size) // Not enough data
			break;

		if (io.inputBuffer.size() - start >= size)
			streamEvent.data = io.inputBuffer.substr(start, size);

		auto task = [this, sc = std::move(streamEvent), client = std::weak_ptr(client)]() mutable
		{
			runStream(sc, client);
		};

		auto callback = [this, client]() mutable
		{
			bool isStream;
			{
				std::lock_guard sLock(client->stream.streamMutex);
				isStream = !client->stream.streamService.empty();
			}
			if (isStream)
				processStream(client);
		};

		m_streamPool.schedule(client->clientID, task, callback);

		start += size;
	}

	io.inputBuffer.erase(0, start);
}

/**
 * Executes a previously parsed command.
 *
 * The command is forwarded to the corresponding module.
 * Any generated response is appended to the client's
 * output buffer.
 *
 * @param cmd Parsed command.
 * @param wClient Weak reference to the client.
 */
void ProtocolManager::executeCommand(const CommandRequest& cmd, const std::weak_ptr<ClientContext>& wClient)
{
	LOG_DEBUG("Executing command: " + cmd.service + " " + cmd.action);

	CommandResult res;
	const auto it = m_modules.find(cmd.service);
	if (it == m_modules.end())
	{
		Logger::get().error("Received command for unknown service: " + cmd.service);
		res = {cmd.clientID, "Unknown command", false};
	}
	else
	{
		res = it->second.module->execute(cmd);
		for (const auto& work : res.work)
			m_commandPool.schedule(res.clientID, work, nullptr);

	}

	const std::shared_ptr client = wClient.lock();
	if (client && client->io.fd.load() != -1)
	{
		{
			std::lock_guard lock(client->io.outputMutex);
			client->io.outputBuffer += res.message + '\n';
		}

		if (res.streamTransfer)
		{
			std::lock_guard lock(client->stream.streamMutex);
			client->stream.streamService = cmd.service;
		}

		epoll_event event{};
		event.events = EPOLLIN | EPOLLOUT;
		event.data.u64 = client->clientID;
		epoll_ctl(m_epollFd, EPOLL_CTL_MOD, client->io.fd.load(), &event);
	}
}

/**
 * Passes one stream chunk to the owning module.
 *
 * The module decides whether the stream should remain
 * active after processing the chunk.
 *
 * @param streamEvent Stream chunk.
 * @param wClient Weak reference to the client.
 */
void ProtocolManager::runStream(const StreamEvent& streamEvent, const std::weak_ptr<ClientContext>& wClient)
{
	LOG_DEBUG("Feeding stream: " + streamEvent.service);

	std::string err;
	bool keepStream = false;
	const auto it = m_modules.find(streamEvent.service);
	if (it == m_modules.end())
	{
		Logger::get().error("Received stream data for unknown service: " + streamEvent.service);
		err = "Unknown stream service";
	}
	else
	{
		StreamModule* module = it->second.streamModule;
		if (module)
			keepStream = module->handleStream(streamEvent);
		else
			err = "Service does not accept streams";
	}

	const std::shared_ptr client = wClient.lock();
	if (client && client->io.fd.load() != -1)
	{
		if (!err.empty())
		{
			std::lock_guard lock(client->io.outputMutex);
			client->io.outputBuffer += err + '\n';
		}

		if (!keepStream)
		{
			std::lock_guard lock(client->stream.streamMutex);
			client->stream.streamService.clear();
		}

		epoll_event event{};
		event.events = EPOLLIN | EPOLLOUT;
		event.data.u64 = client->clientID;
		epoll_ctl(m_epollFd, EPOLL_CTL_MOD, client->io.fd.load(), &event);
	}
}

/**
 * Background thread responsible for forwarding
 * asynchronous stream events produced by modules
 * to connected clients.
 */
void ProtocolManager::streamEventHandler()
{
	while (m_running.load())
	{
		std::queue<StreamEvent> events;

		{
			std::unique_lock lock(m_streamChannel->mutex);

			auto cond = [this]
			{
				return !m_running.load() || !m_streamChannel->events.empty();
			};
			m_streamChannel->condition.wait(lock, cond);

			std::swap(events, m_streamChannel->events);
		}

		while (!events.empty())
		{
			auto streamEvent = events.front();
			events.pop();

			std::shared_ptr<ClientContext> client;
			{
				std::shared_lock lock(m_clientsMtx);
				auto it = m_clients.find(streamEvent.clientID);
				if (it == m_clients.end())
					continue;

				client = it->second;
			}

			if (client->io.fd.load() != -1)
			{
				if (streamEvent.data.empty())
				{
					std::lock_guard lock(client->stream.streamMutex);
					client->stream.streamService.clear();
				}

				{
					std::lock_guard lock(client->io.outputMutex);
					client->io.outputBuffer += streamEvent.data;
				}

				epoll_event event{};
				event.events = EPOLLIN | EPOLLOUT;
				event.data.u64 = client->clientID;
				epoll_ctl(m_epollFd, EPOLL_CTL_MOD, client->io.fd.load(), &event);
			}
		}
	}
}
