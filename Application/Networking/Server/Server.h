// Created by yakov on 24/07/2026.

#pragma once

#include "Configuration/ConfigManager.h"
#include "../Protocol/Manager/ProtocolManager.h"

class Server
{
public:
	explicit Server(ConfigManager& config);
	~Server() = default;

	[[noreturn]] void run();

private:
	void startServer(ConfigFile* config);
	void handleNewConnection();
	void handleClient(const std::shared_ptr<Connection>& client);
	void handleResponses(const std::shared_ptr<Connection>& client) const;

	void closeConnection(const std::shared_ptr<Connection>& client);

	uint32_t m_maxClients;
	uint32_t m_backlog;

	std::unordered_map<int, std::shared_ptr<Connection>> m_clients;

	std::unique_ptr<ProtocolManager> m_protocolManager;

	std::atomic_uint64_t m_nextClientId{1};

	int m_serverFd;
	int m_epollFd;
};
