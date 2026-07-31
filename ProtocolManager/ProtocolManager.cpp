// Created by yakov on 30/07/2026.

#include "ProtocolManager.h"

#include <cstring>
#include <sys/epoll.h>

#include "Logger/Logger.h"
#include "ModuleFactory/ModuleFactory.h"
#include "Server/Server.h"

// Todo: add config parsing for the protocol manager. and create the thread that drain stream events

ProtocolManager::ProtocolManager(ConfigManager& configManager, const int epollFd) :
	m_streamPool(4), m_commandPool(1), m_maxPacket(4096),
	m_maxStreamChunks(4), m_epollFd(epollFd), m_streamChannel(std::make_shared<StreamChannel>())
{
	auto& configs = configManager.getConfigs();

	for (auto& [moduleName, moduleConfig]: configs)
	{
		if (moduleName == "Server")
			continue;

		auto module = ModuleFactory::create(moduleName, moduleConfig, m_streamChannel);

		if (!module)
			throw std::runtime_error("Failed to initialize module: " + moduleName);

		Logger::get().setup("Loaded module: " + moduleName);

		StreamModule* stream = dynamic_cast<StreamModule*>(module.get());
		m_modules[moduleName] = {std::move(module), stream};
	}
}

void ProtocolManager::createClient(const std::shared_ptr<ClientContext>& client)
{
	std::lock_guard lock(m_clientsMtx);
	m_clients.emplace(client->clientID, client);
}

void ProtocolManager::removeUser(const uint64_t id)
{
	std::lock_guard lock(m_clientsMtx);
	m_clients.erase(id);
}

uint16_t ProtocolManager::getMaxPacket(const uint64_t id)
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
	if (client->io.inputBuffer.size() >= m_maxPacket)
		return 0;

	return m_maxPacket - client->io.inputBuffer.size();
}

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
		if (size > m_maxPacket) // current packet size is bigger than max packet size - drop packet and mark the rest invalid
		{
			const size_t available = io.inputBuffer.size() - start - sizeof(size);
			io.discardBytes = size - available;
			io.inputBuffer.clear();
			return;
		}

		start += sizeof(size);

		if (io.inputBuffer.size() - start < size) // Not enough data
			break;

		std::stringstream ss(io.inputBuffer.substr(start, size)); // extract the command

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
		if (size > m_maxPacket) // current packet size is bigger than max packet size - drop packet and mark the rest invalid
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

		if (streamEvent.data.empty())
			break;

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
		res = it->second.module->execute(cmd);

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
