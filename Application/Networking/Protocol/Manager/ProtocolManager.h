// Created by yakov on 30/07/2026.

#pragma once

#include "../Packets.h"
#include "Concurrency/ThreadPool.h"
#include "Configuration/ConfigManager.h"
#include "Security/Authentication/AuthManager.h"
#include "Services/Modules/Module.h"

struct ConnectionIO
{
	std::mutex outputMutex;
	std::string outputBuffer;

	std::mutex inputMutex;
	std::string inputBuffer;

	size_t discardBytes = 0;

	std::atomic_int fd;
};

struct StreamState
{
	std::mutex mutex;
	std::string service;
	std::atomic_bool isStream;
};

struct Connection
{
	uint64_t connectionId;

	ConnectionIO io;
	StreamState stream;
	AuthenticationState auth;

	std::atomic_bool keepAlive = true;
};

struct ModuleEntry
{
	std::unique_ptr<Module> module;
	StreamModule* streamModule;
};

struct Command
{
	std::string service;
	Control control;

	CommandRequest commandRequest;
};

class ProtocolManager
{
public:
	explicit ProtocolManager(ConfigManager& configManager, int epollFd);
	~ProtocolManager();

	void createClient(const std::shared_ptr<Connection>& client);
	void removeUser(uint64_t id);

	uint16_t getAvailableInputSpace(uint64_t id);
	void process(uint64_t id);

private:
	uint32_t processCommand(const std::shared_ptr<Connection>& client, uint32_t offset);

	uint32_t processStream(const std::shared_ptr<Connection>& client, uint32_t offset);

	uint32_t processConfiguration(const std::shared_ptr<Connection>& client, uint32_t offset) const;

	void executeCommand(const Command& cmd, const std::weak_ptr<Connection>& wClient);
	void runStream(const StreamEvent& streamEvent, const std::weak_ptr<Connection>& wClient);

	void streamEventHandler();

	void notifyEpoll(uint64_t connectionId, uint64_t fd) const;

	std::unordered_map<std::string, ModuleEntry> m_modules;

	std::unordered_map<uint64_t, std::shared_ptr<Connection>> m_clients;
	std::shared_mutex m_clientsMtx;

	ThreadPool m_streamPool;
	ThreadPool m_commandPool;
	uint32_t m_maxStreamChunks;

	int m_epollFd;

	std::thread m_streamHandler;
	std::atomic_bool m_running;
	std::shared_ptr<StreamChannel> m_streamChannel;

	AuthManager m_authManager;

	uint32_t m_baseOutputMax;
	uint32_t m_baseInputMax;
};
