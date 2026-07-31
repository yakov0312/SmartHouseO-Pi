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

constexpr uint32_t COMMAND_BUFFER_SIZE = 512;
constexpr uint32_t STREAM_BUFFER_SIZE = 4 * 4096;

constexpr uint64_t CONNECTION_ID = 0;

// seperate command parsing from the server
// seperate the client struct - command parser owns out and server owns in.
// server shouldnt care if the client is in stream or command mode.
// server writes data to in buffer and call the parser with a ref and id. the parser looks up the id and start parsing then forward to the workers.
// when data is available in the out buffer(the worker writes to there) the server looks up the client then ask the command parser for the parsed out buffer to return

Server::Server(ConfigManager& config) : m_clients(0), m_protocolManager(nullptr), m_serverFd(-1), m_epollFd(0)
{
	ConfigFile* serverConf = config.getConfig("Server");

	if (serverConf != nullptr)
	{
		m_maxClients = serverConf->getInt("Settings", "MaxClients", DEFAULT_MAX_CLIENTS);
		m_backlog = serverConf->getInt("Settings", "Backlog", DEFAULT_BACKLOG);
	}
	else
	{
		m_maxClients = DEFAULT_MAX_CLIENTS;
		m_backlog = DEFAULT_BACKLOG;
	}

	LOG_DEBUG("Server MaxClients: " + std::to_string(m_maxClients));
	LOG_DEBUG("Server Backlog: " + std::to_string(m_backlog));

	startServer(serverConf);

	config.removeConfig("Server");

	Logger::get().setup("Server initialized");

	m_protocolManager = std::make_unique<ProtocolManager>(config, m_epollFd);
}

void Server::run()
{
	Logger::get().runtime("Server started");

	if (listen(m_serverFd, m_backlog) == -1)
		throw std::runtime_error("Failed to start listening on server socket");


	Logger::get().runtime("Listening for connections");


	epoll_event events[MAX_EVENTS];

	while (true)
	{
		const int count = epoll_wait(m_epollFd, events, MAX_EVENTS, -1);

		for (int i = 0; i < count; i++)
		{
			const uint64_t id = events[i].data.u64;

			if (id == CONNECTION_ID)
				handleNewConnection();
			else if (m_clients.contains(id))
			{
				if (events[i].events & EPOLLIN)
					handleClient(id);

				if (events[i].events & EPOLLOUT)
					handleResponses(id);
			}
		}
	}
}

void Server::startServer(ConfigFile* config)
{
	m_serverFd = socket(AF_INET, SOCK_STREAM, 0);
	if (m_serverFd == -1)
		throw std::runtime_error("Failed to create server socket");

	constexpr int reuse = 1;
	setsockopt(m_serverFd, SOL_SOCKET, SO_REUSEADDR, &reuse, sizeof(reuse));

	sockaddr_in addr{};
	addr.sin_family = AF_INET;

	const uint16_t port = (config) ? config->getInt("Settings", "Port", DEFAULT_PORT) : DEFAULT_PORT;

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
	event.data.u64 = CONNECTION_ID;

	if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, m_serverFd, &event) == -1)
		throw std::runtime_error("Failed to register server socket with epoll");


	Logger::get().setup("Epoll initialized");
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
			LOG_DEBUG("Failed to initialize client connection");
			return;
		}

		const uint64_t clientID = m_nextClientId++;

		epoll_event event{};
		event.events = EPOLLIN;
		event.data.u64 = clientID;

		if (epoll_ctl(m_epollFd, EPOLL_CTL_ADD, clientFd, &event) == -1)
		{
			close(clientFd);
			throw std::runtime_error("Failed to register client socket with epoll");
		}

		std::shared_ptr<ClientContext> client = std::make_shared<ClientContext>();
		client->clientID = clientID;
		client->io.fd = clientFd;
		m_clients.emplace(clientID, client);
		m_protocolManager->createClient(client);

		Logger::get().runtime("Client connected (id=" + std::to_string(clientID) + ")");
	}
}

void Server::handleClient(const int id)
{
	const std::shared_ptr<ClientContext> client = m_clients[id];
	ClientIO& io = client->io;

	while (true)
	{
		const uint16_t readSize = m_protocolManager->getMaxPacket(id);
		if (readSize == 0)
			return;

		{
			std::lock_guard lock(io.inputMutex);

			const size_t oldSize = io.inputBuffer.size();

			io.inputBuffer.resize(oldSize + readSize);

			const int bytes = read(client->io.fd.load(), io.inputBuffer.data() + oldSize, readSize);

			if (bytes > 0)
				io.inputBuffer.resize(oldSize + bytes);
			else
			{
				io.inputBuffer.resize(oldSize);

				if (bytes == 0)
				{
					close(client->io.fd.load());
					client->io.fd.store(-1);

					m_protocolManager->removeUser(client->clientID);
					m_clients.erase(id);
				}

				break;
			}
		}

		m_protocolManager->process(id);
	}
}

void Server::handleResponses(const int id)
{
	const std::shared_ptr<ClientContext> client = m_clients[id];
	ClientIO& io = client->io;

	std::unique_lock lock(io.outputMutex);

	const size_t bytes = send(client->io.fd.load(), io.outputBuffer.data(), io.outputBuffer.size(), 0);

	if (bytes == -1)
	{
		Logger::get().error("Failed sending response to client id=" + std::to_string(id));
		return;
	}

	if (bytes < io.outputBuffer.size())
		io.outputBuffer.erase(0, bytes);
	else
	{
		io.outputBuffer.clear();

		lock.unlock();

		epoll_event event{};

		event.events = EPOLLIN;
		event.data.u64 = id;

		epoll_ctl(m_epollFd, EPOLL_CTL_MOD, client->io.fd.load(), &event);
	}
}
