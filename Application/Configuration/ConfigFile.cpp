// Created by yakov on 24/07/2026.

#include "ConfigFile.h"

#include <fstream>
#include <thread>

constexpr auto DEFAULT_CONFIG = "./config.co";

constexpr uint16_t SLEEP_TIME = 15;

ConfigFile::ConfigFile(const std::filesystem::path& configPath) : m_writeBackRunning(false), m_configPath(configPath)
{
	if (configPath == "Enable")
		return;

	std::ifstream config(configPath);
	if (!config)
		throw std::runtime_error("Error: Unable to parse config. Cannot find config.");

	std::string line;
	std::unordered_map<std::string, std::string>* section = nullptr;

	while (getline(config, line))
	{
		if (line.empty() || line[0] == COMMENT_SYM)
			continue;

		if (line[0] == '[')
		{
			if (line.back() != ']' && line.size() <= 2)
				continue;

			std::string sectionName = line.substr(1, line.size() - 2);
			section = &m_config.try_emplace(sectionName).first->second;
		}
		else if (section != nullptr)
		{
			size_t keyPos = line.find_first_of('=');
			if (keyPos == std::string::npos)
				continue;

			std::string key = line.substr(0, keyPos);

			std::string value = line.substr(keyPos + 1);
			if (value.empty())
				continue;

			section->emplace(key, value);
		}
	}

	recoverJournal();
}

ConfigFile::~ConfigFile()
{
	m_writeBackRunning.store(false);
	m_writeBackCondition.notify_one();

	if (m_writeBackThread.joinable())
		m_writeBackThread.join();
}

std::string ConfigFile::getString(const std::string& section, const std::string& key, const std::string& defaultValue)
{
	std::shared_lock lock(m_configMutex);
	const auto sectionIt = m_config.find(section);
	if (sectionIt == m_config.end())
		return defaultValue;

	const auto it = sectionIt->second.find(key);
		if (it == sectionIt->second.end())
			return defaultValue;

	return it->second;
}

int ConfigFile::getInt(const std::string& section, const std::string& key, const int defaultValue)
{
	std::shared_lock lock(m_configMutex);

	const auto sectionIt = m_config.find(section);
	if (sectionIt == m_config.end())
		return defaultValue;

	const auto it = sectionIt->second.find(key);
	if (it == sectionIt->second.end())
		return defaultValue;

	try
	{
		return std::stoi(it->second);
	}
	catch ([[maybe_unused]] const std::exception& e)
	{
		return defaultValue;
	}
}

void ConfigFile::add(const std::string& section, const std::string& key, const std::string& value)
{
	bool shouldWrite = false;
	{
		std::lock_guard lock(m_configMutex);

		const auto sectionIt = m_config.find(section);
		if (sectionIt == m_config.end())
		{
			m_config[section][key] = value;
			shouldWrite = true;
		}
		else
		{
			const auto keyIt = sectionIt->second.find(key);
			if (keyIt == sectionIt->second.end() || keyIt->second != value)
			{
				sectionIt->second[key] = value;
				shouldWrite = true;
			}
		}
	}

	if (shouldWrite)
		queueWrite(section, key, value);
}

void ConfigFile::remove(const std::string& section, const std::string& key)
{
	bool removed = false;
	{
		std::lock_guard lock(m_configMutex);

		const auto sectionIt = m_config.find(section);
		if (sectionIt != m_config.end())
		{
			const auto keyIt = sectionIt->second.find(key);
			if (keyIt != sectionIt->second.end())
			{
				sectionIt->second.erase(keyIt);
				removed = true;
			}

			if (sectionIt->second.empty())
				m_config.erase(sectionIt);
		}
	}

	if (removed)
		queueWrite(section, key, "");
}

std::unordered_map<std::string, std::string> ConfigFile::getConfig(const std::string& section)
{
	std::shared_lock lock(m_configMutex);
	const auto it = m_config.find(section);
	if (it == m_config.end())
		return {};
	else
		return it->second;
}

void ConfigFile::queueWrite(const std::string& section, const std::string& key, const std::string& value)
{
	{
		std::lock_guard lock(m_pendingWritesMutex);
		m_pendingWrites.push({section, key, value});
	}

	m_writeBackCondition.notify_one();

	bool expected = false;

	if (m_writeBackRunning.compare_exchange_strong(expected, true)) // Check if Writer exists
	{
		if (m_writeBackThread.joinable())
			m_writeBackThread.join();
		m_writeBackThread = std::thread(&ConfigFile::writeBackWorker, this);
	}
}

void ConfigFile::writeBackWorker()
{
	// Create the journal
	std::filesystem::path journalPath = m_configPath;
	journalPath.replace_extension(".journal");
	Journal journal(journalPath);

	while (m_writeBackRunning.load())
	{
		// Swap the queues to process locally
		std::queue<ConfigChange> pendingChanges;
		{
			std::lock_guard lock(m_pendingWritesMutex);
			std::swap(pendingChanges, m_pendingWrites);
		}

		// Process the changes
		while (!pendingChanges.empty())
		{
			const ConfigChange& write = pendingChanges.front();
			journal.write(write);
			pendingChanges.pop();
		}

		// Wait for timeout or more work
		std::unique_lock lock(m_pendingWritesMutex);
		auto cond = [&]
		{
			return !m_pendingWrites.empty() || !m_writeBackRunning.load();
		};
		m_writeBackCondition.wait_for(lock, std::chrono::seconds(SLEEP_TIME), cond);

		if (m_pendingWrites.empty()) // Timeout check
		{
			lock.unlock();
			if (!flushConfig()) // Rewrite the file from the map
				continue;

			lock.lock();
			if (m_pendingWrites.empty()) // More work check
			{
				std::filesystem::remove(journalPath); // Delete the journal
				m_writeBackRunning.store(false); // Self terminate
			}
			else
				journal.clear(); // Clear the journal and process further

			// Lock self unlocks
		}
	}
}

bool ConfigFile::flushConfig()
{
	std::filesystem::path tmpPath = m_configPath;
	tmpPath += ".tmp";

	std::ofstream config(tmpPath);
	if (!config)
		return false;

	// Reconstruct the file from the map
	std::shared_lock lock(m_configMutex);
	for (const auto& [section, values] : m_config)
	{
		config << '[' << section << "]\n";
		for (const auto& [key, value] : values)
			config << key << '=' << value << '\n';

		config << '\n';
	}

	config.flush();

	std::error_code ec;
	std::filesystem::rename(tmpPath, m_configPath, ec);

	return !ec;
}

void ConfigFile::recoverJournal()
{
	std::filesystem::path journalPath = m_configPath;
	journalPath.replace_extension(".journal");
	if (std::filesystem::exists(journalPath))
	{
		Journal journal(journalPath);
		auto changes = journal.getJournal();
		for (const auto& [section, values] : changes)
		{
			auto& sectionToUpdate = m_config[section];

			for (const auto& [key, value] : values)
				if (!value.empty())
					sectionToUpdate[key] = value;
				else
					sectionToUpdate.erase(key);

			if (sectionToUpdate.empty())
				m_config.erase(section);
		}

		if (!flushConfig())
			throw std::runtime_error("Error: Unable to recover journal.");

		std::filesystem::remove(journalPath);
	}
}
