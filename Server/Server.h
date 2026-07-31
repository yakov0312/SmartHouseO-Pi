// Created by yakov on 24/07/2026.

#pragma once

#include <condition_variable>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>

#include "ConfigParser/ConfigFile.h"
#include "ConfigParser/ConfigManager.h"
#include "Modules/Module.h"
#include "ProtocolManager/ProtocolManager.h"
#include "ThreadPool/ThreadPool.h"

class Server
{
public:
	explicit Server(ConfigManager& config);
	~Server() = default;

	[[noreturn]] void run();

private:
	void startServer(ConfigFile* config);
	void handleNewConnection();
	void handleClient(int id);
	void handleResponses(int id);

	uint32_t m_maxClients;
	uint32_t m_backlog;

	std::unordered_map<int, std::shared_ptr<ClientContext>> m_clients;

	std::unique_ptr<ProtocolManager> m_protocolManager;

	std::atomic_uint64_t m_nextClientId{1};

	int m_serverFd;
	int m_epollFd;
};
