// Created by yakov on 24/07/2026.

#include "Server.h"

#include "ConfigParser/ConfigManager.h"
#include "ModuleFactory/ModuleFactory.h"

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
	// Get server config
	ConfigFile* serverConf = config.getConfig("Server");
	if (serverConf == nullptr)
		throw std::runtime_error("Server service is not enabled");

	// Init data
	m_maxClients = serverConf->getInt("Settings", "MaxClients", DEFAULT_MAX_CLIENTS);
	const int backlog = serverConf->getInt("Settings", "Backlog", DEFAULT_BACKLOG);

	// Construct the needed modules
	auto& configs = config.getConfigs();
	for (auto& [moduleName, moduleConfig] : configs)
	{
		std::unique_ptr<Module> module = ModuleFactory::create(moduleName, moduleConfig);
		if (module)
			m_modules[moduleName] = std::move(module);
	}

	// Setup the server
	startServer(*serverConf);
	config.removeConfig("Server"); // Delete unused configs
	m_worker = std::thread(&Server::commandWorker, this);

	run(backlog);
}

void Server::startServer(ConfigFile& config)
{
	// Create server fd
	m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_serverFd == -1)
		throw std::runtime_error("Failed to create socket");

	constexpr int reuse = 1;
	setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;

	const uint16_t port = config.getInt("Settings", "Port", DEFAULT_PORT);
	addr.sin_port = htons(port);

	addr.sin_addr.s_addr = INADDR_ANY;

	// Bind server fd
	int status = bind(m_serverFd, reinterpret_cast<sockaddr*>(&addr), sizeof(addr));
	if (status == -1)
		throw std::runtime_error("Failed to bind socket");

	// Make non blocking
	const int flags = fcntl(m_serverFd, F_GETFL, 0);
	fcntl(m_serverFd, F_SETFL, flags | O_NONBLOCK);

	// Create epoll fd
	m_epollFd = epoll_create1(0);
	if (m_epollFd == -1)
		throw std::runtime_error("Failed to create epoll");

	// Add server sock to epoll
	epoll_event event{};
	event.events = EPOLLIN;
	event.data.fd = m_serverFd;

	status = epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_serverFd, &event);
	if (status == -1)
		throw std::runtime_error("Failed to add socket to epoll");
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

		auto it = m_modules.find(cmd.service);
		if (it == m_modules.end())
			continue;

		const Result res = it->second->execute(cmd);
		Client& client = *m_clients[cmd.clientFd];

		{
			std::lock_guard<std::mutex> lock(client.oBuffer.mtx);
			client.oBuffer.buf += res.message;
		}

		epoll_event event{};
		event.events = EPOLLIN | EPOLLOUT;
		event.data.fd = cmd.clientFd;

		epoll_ctl(m_epollFd, EPOLL_CTL_MOD, cmd.clientFd, &event);
	}
}

void Server::run(const int backlog)
{
	// Start listening
	if (listen(m_serverFd, backlog) == -1)
		throw std::runtime_error("Failed to listen");

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

			throw std::runtime_error("accept failed");
		}

		if (m_clients.size() >= m_maxClients)
		{
			close(clientFd);
			return;
		}

		// Make client non-blocking
		const int flags = fcntl(clientFd, F_GETFL, 0);
		fcntl(clientFd, F_SETFL, flags | O_NONBLOCK);

		epoll_event event{};
		event.events = EPOLLIN;
		event.data.fd = clientFd;

		epoll_ctl(m_epollFd, EPOLL_CTL_ADD, clientFd, &event);

		m_clients.emplace(clientFd, std::make_unique<Client>());
	}
}

void Server::handleClient(const int fd)
{
	Client& client = *m_clients[fd];
	char buffer[256];
	const size_t bytes = recv(fd, buffer, sizeof(buffer), 0);
	if (bytes == 0)
	{
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

		std::stringstream ss(client.iBuffer.buf.substr(endCom, pos - endCom));

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

		endCom = pos;
		endCom++;

		uLock.lock();
	}

	client.iBuffer.buf.erase(0, endCom);
	uLock.unlock();
}

void Server::handleResponses(const int fd)
{
	Client& client = *m_clients[fd];
	std::unique_lock lock(client.oBuffer.mtx);
 	const size_t bytes = send(fd, client.oBuffer.buf.data(), client.oBuffer.buf.size(), 0);
	if (bytes == 0)
		return; // lock self destructs and unlocks itself

	if (bytes < client.oBuffer.buf.size())
	{
		client.oBuffer.buf.erase(0, bytes);
		lock.unlock();
	}
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
