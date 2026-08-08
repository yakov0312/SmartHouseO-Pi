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
 * Constructs the protocol manager and initializes all subsystems.
 *
 * Loads protocol configuration from the provided ConfigManager, creates
 * and sizes the command and stream thread pools, instantiates all
 * configured modules via ModuleFactory, and launches the asynchronous
 * stream event handler background thread.
 *
 * @param configManager Reference to configuration manager containing
 *                      protocol settings and module configurations.
 * @param epollFd       Epoll file descriptor used for asynchronous
 *                      socket writability notifications.
 * @throws std::runtime_error If a module fails to initialize.
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

	m_baseInputMax = config->getInt(SETTINGS_SECTION, "MaxInputBuffer", DEFAULT_STREAM_MAX_BUFFER);
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
		{
			Logger::get().error("Failed to initialize module: " + moduleName);
			throw std::runtime_error("Failed to initialize module: " + moduleName);
		}

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
 * Registers a newly connected client with the protocol manager.
 *
 * Stores the connection in the client registry by its connection ID
 * and initializes per-connection stream configuration with the
 * default buffer limits.
 *
 * Thread Safety: Thread-safe. Acquires exclusive lock on client registry.
 *
 * @param client Shared pointer to the Connection context. The connectionId
 *               must be unique and valid.
 */
void ProtocolManager::createClient(const std::shared_ptr<Connection>& client)
{
	std::lock_guard lock(m_clientsMtx);

	// Store connection by connection ID. This ID represents the socket lifetime,
	// Not the authenticated user.
	m_clients.emplace(client->connectionId, client);

	m_streamChannel->initializeConfiguration(client->connectionId, {m_baseInputMax, m_baseOutputMax});
	Logger::get().runtime("Initialized configuration for ID " + std::to_string(client->connectionId));
}

/**
 * Removes a client and cleans up all associated resources.
 * Thread Safety: Thread-safe. Acquires exclusive lock on client registry.
 *
 * @param id Unique connection identifier assigned during createClient().
 */
void ProtocolManager::removeUser(const uint64_t id)
{
	std::lock_guard lock(m_clientsMtx);

	Logger::get().runtime("Removing client ID " + std::to_string(id));

	const auto node = m_clients.extract(id);

	if (node)
	{
		const std::shared_ptr<Connection> client = std::move(node.mapped());

		if (client->stream.isStream)
		{
			Logger::get().runtime("Terminating open stream for ID " + std::to_string(client->connectionId));

			const auto it = m_modules.find(client->stream.service);
			StreamModule* module = it->second.streamModule;

			if (module)
				 module->terminateStream(id);
		}
	}

	m_streamChannel->removeConfiguration(id);
}

/**
 * Returns the remaining writable space in the client's input buffer.
 * Used for flow control to prevent sending data faster than the client
 * can process.
 *
 * Thread Safety: Thread-safe. Acquires shared lock on client registry
 * and exclusive lock on the client's input mutex.
 *
 * @param id Client connection identifier.
 * @return Number of writable bytes remaining in input buffer, or 0
 *         if client unknown or buffer full.
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
 * Processes newly received data from a client.
 *
 * Parses the input buffer for complete protocol packets, validates
 * packet headers, and dispatches complete packets to the appropriate
 * handler.
 *
 * Thread Safety: Thread-safe. Acquires shared lock on client registry
 * and exclusive lock on the client's input mutex.
 *
 * @param id Connection identifier of the client with data to process.
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
			Logger::get().error("Invalid control type " + std::to_string(control) + " from client " + std::to_string(id));
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
			Logger::get().error("Oversized packet (" + std::to_string(payloadSize + packetHeaderSize) + " bytes) from client " + std::to_string(id));
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
 * Parses a complete command packet,
 * schedules it for asynchronous execution on the command thread pool.
 * Malformed commands are logged and skipped.
 *
 * Thread Safety: Called with client->io.inputMutex held by caller.
 *
 * @param client Shared pointer to the client connection context.
 * @param offset Byte offset in input buffer where the packet starts.
 * @return New offset after consuming this packet (header + payload).
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
	{
		Logger::get().error("Malformed command from client " + std::to_string(client->connectionId));
		return offset + payloadSize + sizeof(CommandPacket); // Skip this invalid command
	}

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
 * Parses a stream packet and schedules it for processing on the stream thread pool.
 *
 * Implements flow control by checking the number of queued stream chunks
 * for this connection.
 *
 * Thread Safety: Called with client->io.inputMutex held by caller.
 *
 * @param client Shared pointer to the client connection context.
 * @param offset Byte offset in input buffer where the packet starts.
 * @return New offset after consuming this packet, or current offset
 *         if flow control prevented processing (packet remains buffered).
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

/**
 * Processes a configuration packet to negotiate buffer sizes.
 *
 * Validates the requested input/output chunk sizes against system limits,
 * clamps them to acceptable values, stores the configuration for the
 * connection, and sends the accepted values back to the client.
 * Ensures minimum input size can accommodate a ConfigurationPacket.
 *
 * Thread Safety: Called with client->io.inputMutex held by caller.
 *
 * @param client Shared pointer to the client connection context.
 * @param offset Byte offset in input buffer where the packet starts.
 * @return New offset after consuming this packet.
 */
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
 * Executes a parsed command on the appropriate module or auth manager.
 *
 * Authentication commands (AUTH control type) and unauthenticated
 * connections are handled by AuthManager.
 * Authenticated commands are routed to their designated service module.
 *
 * If the module requests stream creation, the connection is switched to stream mode and initial
 * stream configuration is sent.
 *
 * Thread Safety: Called from command thread pool. Acquires outputMutex
 * when appending to output buffer.
 *
 * @param cmd     Parsed command structure with service, action, and args.
 * @param wClient Weak reference to client (may be expired if disconnected).
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
 * Processes a single stream data chunk through the owning module.
 *
 * Forwards the stream event to the appropriate module based on the
 * service name. The module returns true to keep the stream active,
 * false to terminate.
 *
 * Thread Safety: Called from stream thread pool. May acquire stream.mutex
 * and outputMutex.
 *
 * @param streamEvent Stream chunk containing data and service identifier.
 * @param wClient     Weak reference to client (may be expired).
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
 * Background thread function for forwarding async stream events.
 *
 * Continuously polls the StreamChannel for events generated by modules
 * and forwards them to correct client.
 * Runs until m_running is set to false.
 *
 * Thread Safety: Runs on dedicated thread. Uses shared_lock for client
 * lookups and appropriate mutexes for buffer access.
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


				notifyEpoll(client->connectionId, client->io.fd.load());
			}
		}
	}
}

/**
 * Notifies epoll that a socket is ready for writing.
 *
 * Modifies the epoll event mask to include EPOLLOUT for the given
 * file descriptor. The connection ID is stored in epoll_data.u64
 * for correlation when the event fires.
 *
 * @param connectionId Unique connection identifier (stored in epoll_data).
 * @param fd           Socket file descriptor to modify in epoll set.
 */
void ProtocolManager::notifyEpoll(const uint64_t connectionId, const uint64_t fd) const
{
	epoll_event event{};
	event.events = EPOLLIN | EPOLLOUT;
	event.data.u64 = connectionId;
	epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &event);
}