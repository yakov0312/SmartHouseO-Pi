// Created by yakov on 30/07/2026.

#include "ProtocolManager.h"

#include <sys/epoll.h>

#include "Factories/Modules/ModuleFactory.h"
#include "Logging/Logger.h"

constexpr uint32_t DEFAULT_STREAM_MAX_BUFFER = 1024 * 16;
constexpr uint32_t DEFAULT_COMMAND_MAX_BUFFER = 1024 * 4;
constexpr uint32_t DEFAULT_MAX_QUEUED_CHUNKS = 4;
constexpr uint32_t DEFAULT_MAX_QUEUED_EVENTS = 16;
constexpr uint32_t DEFAULT_COMMAND_THREADS = 2;
constexpr uint32_t DEFAULT_STREAM_THREADS = 4;

constexpr auto STREAM_SECTION = "Stream";
constexpr auto COMMAND_SECTION = "Command";

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
	m_streamPool(0), m_commandPool(0), m_epollFd(epollFd),
	m_streamChannel()
{
	// Load protocol settings and initialize worker pools.
	ConfigFile* config = configManager.getConfig("Protocol");

	// Command
	m_commandPool.increaseThreads(config->getInt(COMMAND_SECTION, "CommandThreads", DEFAULT_COMMAND_THREADS));

	//Stream
	m_streamPool.increaseThreads(config->getInt(STREAM_SECTION, "StreamThreads", DEFAULT_STREAM_THREADS));

	m_baseInputMax = config->getInt(SETTINGS_SECTION, "MaxInputBuffe", DEFAULT_STREAM_MAX_BUFFER);
	m_baseOutputMax = config->getInt(SETTINGS_SECTION, "MaxOutputBuffer", DEFAULT_STREAM_MAX_BUFFER);

	m_maxStreamChunks = config->getInt(STREAM_SECTION, "MaxQueuedStreamChunks", DEFAULT_MAX_QUEUED_EVENTS);
	uint32_t maxQueued = config->getInt(STREAM_SECTION, "MaxQueuedStreamEvents", DEFAULT_MAX_QUEUED_EVENTS);

	m_streamChannel = std::make_shared<StreamChannel>(maxQueued);

	configManager.removeConfig("Protocol");

	// Create all configured modules.
	for (auto& [moduleName, moduleConfig]: configManager.getConfigs())
	{
		auto module = ModuleFactory::create(moduleName, moduleConfig, m_streamChannel);

		if (!module)
			throw std::runtime_error("Failed to initialize module: " + moduleName);

		Logger::get().setup("Loaded module: " + moduleName);

		auto* stream = dynamic_cast<StreamModule*>(module.get());
		m_modules[moduleName] = {std::move(module), stream};
	}

	// Start thread responsible for forwarding async module events to clients.
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
	// Wake the handler thread so it can exit cleanly.
	m_running.store(false);
	m_streamChannel->wakeConsumers();

	if (m_streamHandler.joinable())
		m_streamHandler.join();
}

/**
 * Registers a newly connected client.
 *
 * @param client Client context.
 */
void ProtocolManager::createClient(const std::shared_ptr<Connection>& client)
{
	std::lock_guard lock(m_clientsMtx);

	// Store connection by connection ID. This ID represents the socket lifetime,
	// Not the authenticated user.
	m_clients.emplace(client->connectionId, client);

	m_streamChannel->initializeConfiguration(client->connectionId, {m_baseInputMax, m_baseOutputMax});
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

	// Removing a connection also removes all temporary state
	// associated with that TCP session.

	const auto node = m_clients.extract(id);

	if (node)
	{
		const std::shared_ptr<Connection> client = std::move(node.mapped());

		if (client->stream.isStream)
		{
			const auto it = m_modules.find(client->stream.service);
			StreamModule* module = it->second.streamModule;

			if (module)
				 module->terminateStream(id);
		}
	}

	m_streamChannel->removeConfiguration(id);
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
	std::shared_ptr<Connection> client;

	{
		std::shared_lock lock(m_clientsMtx);
		const auto it = m_clients.find(id);
		if (it == m_clients.end())
			return 0;

		client = it->second;
	}

	std::lock_guard lock(client->io.inputMutex);

	const StreamConfiguration configuration = m_streamChannel->getStreamConfiguration(id);

	if (client->io.inputBuffer.size() >= configuration.maxInputChunk)
		return 0;

	return configuration.maxInputChunk - client->io.inputBuffer.size(); // Stream header
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
	const StreamConfiguration configuration = m_streamChannel->getStreamConfiguration(id);

	std::shared_ptr<Connection> client;
	{
		std::shared_lock lock(m_clientsMtx);
		const auto it = m_clients.find(id);
		if (it == m_clients.end())
			return;

		client = it->second;
	}

	std::lock_guard lock(client->io.inputMutex);

	ConnectionIO& io = client->io;

	uint32_t offset = 0;

	if (io.discardBytes != 0)
	{
		// Skip remaining bytes from an invalid oversized packet.
		const size_t consumed = std::min(io.inputBuffer.size(), io.discardBytes);
		offset = consumed;
		io.discardBytes -= consumed;
	}

	while (true)
	{
		const uint16_t availableBytes = io.inputBuffer.size() - offset;
		if (availableBytes < sizeof(PacketHeader)) // Wait for more data
			break;

		const auto* header = reinterpret_cast<PacketHeader*>(io.inputBuffer.data() + offset);

		const Control control = header->control;
		const uint16_t payloadSize = header->len;

		if (control >= std::size(CONTROL_TO_HEADER_SIZE))
		{
			client->keepAlive.store(false);
			return;
		}

		const uint16_t packetHeaderSize = CONTROL_TO_HEADER_SIZE[control];

		// Wait until the complete packet arrives.
		if (availableBytes - packetHeaderSize < payloadSize)
			break;

		// Protect against clients sending huge packet sizes.
		if (payloadSize + packetHeaderSize > configuration.maxInputChunk)
		{
			const size_t available = io.inputBuffer.size() - offset - packetHeaderSize;
			io.discardBytes = payloadSize - available;
			io.inputBuffer.clear();
			return;
		}

		const uint32_t previousOffset = offset;
		if (control == COMMAND || control == AUTH)
			offset = processCommand(client, offset);
		else if (control == STREAM)
			offset = processStream(client, offset);
		else if (control == CONFIG)
			offset = processConfiguration(client, offset);

		if (previousOffset == offset || offset >= io.inputBuffer.size()) // Data cant be processed right now
			break;
	}

	// Remove processed packets while keeping incomplete data.
	io.inputBuffer.erase(0, offset);
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
uint32_t ProtocolManager::processCommand(const std::shared_ptr<Connection>& client, const uint32_t offset)
{
	ConnectionIO& io = client->io;

	const auto* packet = reinterpret_cast<CommandPacket*>(io.inputBuffer.data() + offset);
	const uint16_t payloadSize = packet->header.len;
	const Control packetControl = packet->header.control;

	std::stringstream commandStream(io.inputBuffer.substr(offset + sizeof(CommandPacket), payloadSize)); // Performance can be improved with string_view

	Command cmd;
	cmd.commandRequest.connectionId = client->connectionId;
	cmd.commandRequest.username = client->auth.username;
	cmd.control = packetControl;

	// Expected format:
	// Service Action Arg1 Arg2 ...
	if (!(commandStream >> cmd.service >> cmd.commandRequest.action))
		return offset + payloadSize + sizeof(CommandPacket); // Skip this invalid command

	std::string arg;
	while (commandStream >> arg)
		cmd.commandRequest.args.push_back(arg);

	auto task = [this, cmd = std::move(cmd), client = std::weak_ptr(client)]() mutable
	{
		executeCommand(cmd, client);
	};

	// Schedule execution outside the epoll thread.
	m_commandPool.schedule(client->connectionId, task);

	return offset + payloadSize + sizeof(CommandPacket);
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
uint32_t ProtocolManager::processStream(const std::shared_ptr<Connection>& client, const uint32_t offset)
{
	ConnectionIO& io = client->io;

	const StreamPacket* packet = reinterpret_cast<StreamPacket*>(io.inputBuffer.data() + offset);
	const size_t payloadSize = packet->header.len;

	// Limit queued stream work per connection to avoid memory growth.
	if (m_streamPool.getCurrentTasksCount(client->connectionId) >= m_maxStreamChunks)
		return offset;

	StreamEvent streamEvent{client->connectionId, io.inputBuffer.substr(offset + sizeof(StreamPacket), payloadSize)};

	{
		std::lock_guard sLock(client->stream.mutex);
		streamEvent.service = client->stream.service;
	}

	auto task = [this, sc = std::move(streamEvent), client = std::weak_ptr(client)]() mutable
	{
		runStream(sc, client);
	};

	// Execute stream processing in a worker thread.
	m_streamPool.schedule(client->connectionId, task);

	return offset + payloadSize + sizeof(StreamPacket);
}

uint32_t ProtocolManager::processConfiguration(const std::shared_ptr<Connection>& client, const uint32_t offset) const
{
	ConnectionIO& io = client->io;

	const ConfigurationPacket* packet = reinterpret_cast<ConfigurationPacket*>(io.inputBuffer.data() + offset);

	StreamConfiguration configuration{};
	configuration.maxInputChunk = std::min(m_baseInputMax, packet->maxInputChunk);
	configuration.maxOutputChunk = std::min(m_baseOutputMax, packet->maxOutputChunk);

	if (configuration.maxInputChunk < sizeof(ConfigurationPacket))
		configuration.maxInputChunk = sizeof(ConfigurationPacket);

	m_streamChannel->writeConfiguration(client->connectionId, configuration);

	ConfigurationPacket response;
	response.maxInputChunk = configuration.maxInputChunk;
	response.maxOutputChunk = configuration.maxOutputChunk;

	std::lock_guard lock(client->io.outputMutex);
	client->io.outputBuffer.reserve(sizeof(ConfigurationPacket));
	client->io.outputBuffer.append(reinterpret_cast<const char*>(&response), sizeof(ConfigurationPacket));

	notifyEpoll(client->connectionId, client->io.fd);

	return offset + sizeof(ConfigurationPacket);
}

/**
 * Executes a previously parsed command.
 *
 * The command is forwarded to the corresponding module.
 * Any generated response is appended to the client's
 * Runtime buffer.
 *
 * @param cmd Parsed command.
 * @param wClient Weak reference to the client.
 */
	void ProtocolManager::executeCommand(const Command& cmd, const std::weak_ptr<Connection>& wClient)
{
	LOG_DEBUG("Executing command: " + cmd.service + " " + cmd.commandRequest.action);

	CommandResult res{};

	const std::shared_ptr client = wClient.lock();
	if (!client)
		return;

	// Account commands are handled before modules.
	// Unauthenticated connections are only allowed to access Account.
	if (!client->auth.authenticated || cmd.control == AUTH)
		res.message = m_authManager.processAuth({cmd.commandRequest.action, cmd.commandRequest.args}, client->auth);
	else
	{
		// Forward command to the requested module.
		const auto it = m_modules.find(cmd.service);

		if (it == m_modules.end())
		{
			Logger::get().error("Received command for unknown service: " + cmd.service);
			res = {"Unknown command"};
		}
		else
			res = it->second.module->execute(cmd.commandRequest, !client->stream.isStream);
	}

	if (client->io.fd.load() != -1)
	{
		// A module can request switching this connection into stream mode.
		if (res.createStream)
		{
			{
				client->stream.isStream = true;

				std::lock_guard lock(client->stream.mutex);
				client->stream.service = cmd.service;
			}

			const StreamConfiguration configuration = m_streamChannel->getStreamConfiguration(client->connectionId);
			ConfigurationPacket response;
			response.maxInputChunk = configuration.maxInputChunk;
			response.maxOutputChunk = configuration.maxOutputChunk;

			StreamPacket packet;
			packet.header.len = res.message.size();

			std::lock_guard lock(client->io.outputMutex);
			client->io.outputBuffer.reserve(sizeof(StreamPacket) + packet.header.len + sizeof(ConfigurationPacket));

			// Initialize the stream packet
			client->io.outputBuffer.append(reinterpret_cast<const char*>(&packet), sizeof(StreamPacket));
			client->io.outputBuffer += res.message;

			// Initialize the configuration packet for the stream
			client->io.outputBuffer.append(reinterpret_cast<const char*>(&configuration), sizeof(ConfigurationPacket));
		}
		else
		{
			CommandPacket packet;
			packet.header.len = res.message.size();

			std::lock_guard lock(client->io.outputMutex);
			client->io.outputBuffer.reserve(sizeof(CommandPacket) + packet.header.len);
			client->io.outputBuffer.append(reinterpret_cast<const char*>(&packet), sizeof(CommandPacket));
			client->io.outputBuffer += res.message;
		}

		notifyEpoll(client->connectionId, client->io.fd.load());
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
void ProtocolManager::runStream(const StreamEvent& streamEvent, const std::weak_ptr<Connection>& wClient)
{
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

	if (client && client->io.fd.load() != -1 && // Connection is valid
		(!err.empty() || !keepStream)) // There is an update
	{
		CommandPacket packet{};
		packet.header.len = 0;

		if (!err.empty())
		{
			packet.header.len = err.size();
			packet.header.control = Error;
		}

		if (!keepStream)
		{
			{
				// Remove stream mode
				client->stream.isStream = false;

				std::lock_guard lock(client->stream.mutex);
				client->stream.service.clear();
			}
		}

		std::lock_guard lock(client->io.outputMutex);
		client->io.outputBuffer.reserve(sizeof(CommandPacket) + packet.header.len);
		client->io.outputBuffer.append(reinterpret_cast<const char*>(&packet), sizeof(CommandPacket));
		if (!err.empty())
			client->io.outputBuffer += err;

		notifyEpoll(client->connectionId, client->io.fd.load());
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

		m_streamChannel->swapQueues(events, m_running);

		while (!events.empty())
		{
			auto streamEvent = events.front();
			events.pop();

			std::shared_ptr<Connection> client;

			{
				std::shared_lock lock(m_clientsMtx);

				auto it = m_clients.find(streamEvent.connectionId);

				if (it == m_clients.end())
					continue;

				client = it->second;
			}

			if (client->io.fd.load() != -1)
			{
				{
					if (streamEvent.data.empty())
					{
						{
							client->stream.isStream = false;

							std::lock_guard sLock(client->stream.mutex);
							client->stream.service.clear();
						}

						CommandPacket packet;
						packet.header.len = 0;

						std::lock_guard lock(client->io.outputMutex);
						client->io.outputBuffer.reserve(sizeof(CommandPacket));
						client->io.outputBuffer.append(reinterpret_cast<const char*>(&packet), sizeof(CommandPacket));
					}
					else
					{
						StreamPacket packet;
						packet.header.len = streamEvent.data.size();

						std::lock_guard lock(client->io.outputMutex);
						client->io.outputBuffer.reserve(sizeof(StreamPacket) + packet.header.len);
						client->io.outputBuffer.append(reinterpret_cast<const char*>(&packet), sizeof(StreamPacket));
						client->io.outputBuffer += streamEvent.data;
					}
				}

				notifyEpoll(client->connectionId, client->io.fd.load());
			}
		}
	}
}

void ProtocolManager::notifyEpoll(const uint64_t connectionId, const uint64_t fd) const
{
	epoll_event event{};
	event.events = EPOLLIN | EPOLLOUT;
	event.data.u64 = connectionId;
	epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &event);
}
