// Created by yakov on 24/07/2026.

#pragma once
#include <atomic>
#include <condition_variable>
#include <filesystem>
#include <mutex>
#include <queue>
#include <shared_mutex>
#include <string>
#include <unordered_map>

#include "Journal/Journal.h"

constexpr char COMMENT_SYM = '#';
constexpr auto SETTINGS_SECTION = "Settings";

class ConfigFile
{
public:
	explicit ConfigFile(const std::filesystem::path& configPath);
	~ConfigFile();

	std::string getString(const std::string& section, const std::string& key, const std::string& defaultValue = "");
	int getInt(const std::string& section, const std::string& key, int defaultValue = 0);

	void add(const std::string& section, const std::string& key, const std::string& value);
	void remove(const std::string& section, const std::string& key);
	std::unordered_map<std::string, std::string> getConfig(const std::string& section);

private:
	void queueWrite(const std::string& section, const std::string& key, const std::string& value);
	void writeBackWorker();

	bool flushConfig();

	void recoverJournal();

	std::queue<ConfigChange> m_pendingWrites;
	std::mutex m_pendingWritesMutex;
	std::atomic<bool> m_writeBackRunning;
	std::condition_variable m_writeBackCondition;
	std::thread m_writeBackThread;

	std::filesystem::path m_configPath;
	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> m_config;
	std::shared_mutex m_configMutex;
};


