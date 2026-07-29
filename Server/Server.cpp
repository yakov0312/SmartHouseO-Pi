// Created by yakov on 24/07/2026.

#include "Server.h"

#include "ConfigParser/ConfigManager.h"
#include "ModuleFactory/ModuleFactory.h"
#include "Logger/Logger.h"

#include <fcntl.h>
#include <stdexcept>
#include <unistd.h>
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/epoll.h>
#include <sys/socket.h>
#include <sys/types.h>

#include <sstream>

constexpr uint16_t DEFAULT_PORT = 8080;
constexpr uint16_t DEFAULT_BACKLOG = 32;
constexpr uint32_t DEFAULT_MAX_CLIENTS = 32;

constexpr uint32_t MAX_EVENTS = 64;

Server::Server(ConfigManager& config) : m_clients(0), m_serverFd(-1), m_epollFd(0)
{
	ConfigFile* serverConf = config.getConfig("Server");

	if (serverConf == nullptr)
		throw std::runtime_error("Server module is not enabled in configuration");

	m_maxClients = serverConf->getInt("Settings", "MaxClients", DEFAULT_MAX_CLIENTS);
	m_backlog = serverConf->getInt("Settings", "Backlog", DEFAULT_BACKLOG);

	LOG_DEBUG("Server MaxClients: " + std::to_string(m_maxClients));
	LOG_DEBUG("Server Backlog: " + std::to_string(m_backlog));

	auto& configs = config.getConfigs();

	for (auto& [moduleName, moduleConfig] : configs)
	{
		if (moduleName == "Server")
			continue;

		auto module = ModuleFactory::create(moduleName, moduleConfig);

		if (!module)
			throw std::runtime_error("Failed to initialize module: " + moduleName);

		Logger::get().setup("Loaded module: " + moduleName);

		m_modules[moduleName] = std::move(module);
	}

	startServer(*serverConf);

	config.removeConfig("Server");

	Logger::get().setup("Server initialized");
}

void Server::startServer(ConfigFile& config)
{
	m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_serverFd == -1)
		throw std::runtime_error("Failed to create server socket");

	constexpr int reuse = 1;
	setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;

	const uint16_t port = config.getInt("Settings", "Port", DEFAULT_PORT);

	addr.sin_port = htons(port);
	addr.sin_addr.s_addr = INADDR_ANY;
	if (bind(m_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == -1)
		throw std::runtime_error("Failed to bind server socket on port " + std::to_string(port));


	Logger::get().setup("Server bound to port " + std::to_string(port));


	const int flags = fcntl(m_serverFd, F_GETFL, 0);
	if (fcntl(m_serverFd, F_SETFL, flags | O_NONBLOCK) == -1)
		throw std::runtime_error("Failed to set server socket as non-blocking");


	m_epollFd = epoll_create1(0);
	if (m_epollFd == -1)
		throw std::runtime_error("Failed to create epoll instance");


	epoll_event event{};
	event.events = EPOLLIN;
	event.data.fd = m_serverFd;

	if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_serverFd, &event) == -1)
		throw std::runtime_error("Failed to register server socket with epoll");


	Logger::get().setup("Epoll initialized");
}

void Server::commandWorker()
{
	while (true)
	{
		Command cmd;

		{
			std::unique_lock lock(m_commandMutex);

			m_commandCV.wait(lock, [this] {
				return !m_commands.empty();
			});

			cmd = std::move(m_commands.front());
			m_commands.pop();
		}


		LOG_DEBUG("Executing command: " + cmd.service + " " + cmd.action);


		auto it = m_modules.find(cmd.service);
		if (it == m_modules.end())
		{
			Logger::get().error("Received command for unknown service: " + cmd.service);
			continue;
		}


		const Result res = it->second->execute(cmd);

		Client& client = *m_clients[cmd.clientFd];

		{
			std::lock_guard<std::mutex> lock(client.oBuffer.mtx);

			client.oBuffer.buf += res.message + '\n';
		}

		epoll_event event{};

		event.events = EPOLLIN | EPOLLOUT;
		event.data.fd = cmd.clientFd;
		epoll_ctl(m_epollFd, EPOLL_CTL_MOD, cmd.clientFd, &event);
	}
}

void Server::run()
{
	Logger::get().runtime("Server started");


	m_worker = std::thread(&Server::commandWorker, this);


	if (listen(m_serverFd, m_backlog) == -1)
		throw std::runtime_error("Failed to start listening on server socket");


	Logger::get().runtime("Listening for connections");


	epoll_event events[MAX_EVENTS];

	while (true)
	{
		const int count = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);

		for (int i = 0; i < count; i++)
		{
			const int fd = events[i].data.fd;

			if (fd == m_serverFd)
				handleNewConnection();
			else
			{
				if (events[i].events & EPOLLIN)
					handleClient(fd);

				if (events[i].events & EPOLLOUT)
					handleResponses(fd);
			}
		}
	}
}

void Server::handleNewConnection()
{
	while (true)
	{
		sockaddr_in clientAddr{};
		socklen_t size = sizeof(clientAddr);

		const int clientFd = accept(m_serverFd, reinterpret_cast<sockaddr*>(&clientAddr), &size);
		if (clientFd == -1)
		{
			if (errno == EAGAIN || errno == EWOULDBLOCK)
				break;

			LOG_DEBUG("Failed to accept client connection");
			return;
		}

		if (m_clients.size() >= m_maxClients)
		{
			LOG_DEBUG("Rejected connection: maximum clients reached");

			close(clientFd);
			return;
		}

		const int flags = fcntl(clientFd, F_GETFL, 0);
		if (fcntl(clientFd, F_SETFL, flags | O_NONBLOCK) == -1)
		{
			close(clientFd);
			LOG_DEBUG("Failed to set client socket as non-blocking");
			return;
		}

		epoll_event event{};

		event.events = EPOLLIN;
		event.data.fd = clientFd;

		if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, clientFd, &event) == -1)
		{
			close(clientFd);
			throw std::runtime_error("Failed to register client socket with epoll");
		}


		m_clients.emplace(clientFd, std::make_unique<Client>());

		Logger::get().runtime("Client connected (fd=" + std::to_string(clientFd) + ")");
	}
}

void Server::handleClient(const int fd)
{
	Client& client = *m_clients[fd];

	char buffer[256];

	const size_t bytes = recv(fd, buffer, sizeof(buffer), 0);
	if (bytes == 0)
	{
		Logger::get().runtime("Client disconnected (fd=" + std::to_string(fd) + ")");

		close(fd);
		m_clients.erase(fd);

		return;
	}

	std::unique_lock uLock(client.oBuffer.mtx);

	client.iBuffer.buf.append(buffer, bytes);

	size_t endCom = 0;
	while (true)
	{
		Command cmd;

		const size_t pos = client.iBuffer.buf.find('\n', endCom);
		if (pos == std::string::npos)
			break;

		std::stringstream ss(
			client.iBuffer.buf.substr(endCom, pos - endCom)
		);

		uLock.unlock();

		if (!(ss >> cmd.service >> cmd.action))
			continue;

		std::string arg;

		while (ss >> arg)
			cmd.args.push_back(arg);

		cmd.clientFd = fd;

		{
			std::lock_guard lock(m_commandMutex);

			m_commands.push(std::move(cmd));
		}

		m_commandCV.notify_one();

		endCom = pos + 1;

		uLock.lock();
	}

	client.iBuffer.buf.erase(0, endCom);

	uLock.unlock();
}

void Server::handleResponses(const int fd)
{
	Client& client = *m_clients[fd];

	std::unique_lock lock(client.oBuffer.mtx);

	const size_t bytes = send(
		fd,
		client.oBuffer.buf.data(),
		client.oBuffer.buf.size(),
		0
	);

	if (bytes == -1)
	{
		Logger::get().error("Failed sending response to client fd=" + std::to_string(fd));
		return;
	}

	if (bytes < client.oBuffer.buf.size())
		client.oBuffer.buf.erase(0, bytes);
	else
	{
		client.oBuffer.buf.clear();

		lock.unlock();

		epoll_event event{};

		event.events = EPOLLIN;
		event.data.fd = fd;

		epoll_ctl(m_epollFd, EPOLL_CTL_MOD, fd, &event);
	}
}