// Created by yakov on 30/07/2026.

#pragma once

#include "ConfigParser/ConfigManager.h"
#include "Modules/Module.h"
#include "ThreadPool/ThreadPool.h"

struct ClientIO
{
	std::mutex outputMutex;
	std::string outputBuffer;

	std::mutex inputMutex;
	std::string inputBuffer;

	size_t discardBytes = 0;

	std::atomic_int fd;
};

struct ClientStream
{
	std::mutex streamMutex;
	std::string streamService;
};

struct ClientContext
{
	ClientIO io;

	ClientStream stream;

	uint64_t clientID;
};

struct ModuleEntry
{
	std::unique_ptr<Module> module;
	StreamModule* streamModule;
};

class ProtocolManager
{
public:
	explicit ProtocolManager(ConfigManager& configManager, int epollFd);

	void createClient(const std::shared_ptr<ClientContext>& client);
	void removeUser(uint64_t id);

	uint16_t getMaxPacket(uint64_t id);
	void process(uint64_t id);

private:
	void processCommand(const std::shared_ptr<ClientContext>& client);
	void processStream(const std::shared_ptr<ClientContext>& client);

	void executeCommand(const CommandRequest& cmd, const std::weak_ptr<ClientContext>& wClient);
	void runStream(const StreamEvent& streamEvent, const std::weak_ptr<ClientContext>& wClient);

	std::unordered_map<std::string, ModuleEntry> m_modules;
	std::unordered_map<uint64_t, std::shared_ptr<ClientContext>> m_clients;
	std::shared_mutex m_clientsMtx;

	ThreadPool m_streamPool;
	ThreadPool m_commandPool;

	uint16_t m_maxPacket;
	uint16_t m_maxStreamChunks;

	int m_epollFd;

	std::shared_ptr<StreamChannel> m_streamChannel;
};
