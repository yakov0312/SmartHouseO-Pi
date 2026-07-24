// Created by yakov on 24/07/2026.

#pragma once
#include <condition_variable>
#include <memory>
#include <queue>
#include <string>
#include <unordered_map>
#include <vector>

#include "../ConfigParser/ConfigFile.h"
#include "../ConfigParser/ConfigManager.h"
#include "../Modules/Module.h"

enum State : int
{
	NEW,
	AUTH
};

struct Client
{
	State state;
	std::string iBuffer;
	std::string oBuffer;
};

class Server
{
public:
	explicit Server(ConfigManager& config);
	~Server() = default;

private:
	void startServer(const ConfigFile& config);
	[[noreturn]] void commandWorker();
	[[noreturn]] void run(int backlog);
	void handleNewConnection();
	void handleClient(int fd);
	void handleResponses(int fd);

	std::unordered_map<std::string, std::unique_ptr<Module>> m_modules;

	uint32_t m_maxClients;

	std::queue<Command> m_commands;
	std::mutex m_commandMutex;
	std::condition_variable m_commandCV;

	std::unordered_map<int, Client> m_clients;

	int m_serverFd;
	int m_epollFd;

	std::thread m_worker;
};
