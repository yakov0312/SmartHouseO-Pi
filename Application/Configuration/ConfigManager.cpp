// Created by yakov on 24/07/2026.

#include "ConfigManager.h"
#include <fstream>
#include <memory>

ConfigManager::ConfigManager(const std::filesystem::path& configPath)
{
	std::ifstream config(configPath);
	if (!config && configPath != DEFAULT_CONFIG) // Fallback
	{
		config.clear();
		config.open(DEFAULT_CONFIG);
	}

	if (!config)
		throw std::runtime_error("Error: Unable to find config file.");

	std::string line;
	while (getline(config, line))
	{
		if (line.empty() || line[0] == COMMENT_SYM)
			continue;

		size_t sectionPos = line.find_first_of('=');
		if (sectionPos == std::string::npos)
			continue;

		std::string section = line.substr(0, sectionPos);

		std::filesystem::path sectionPath = line.substr(sectionPos + 1);
		if (sectionPath.empty())
			continue;

		if (sectionPath.is_relative() && sectionPath != "Enable")
			sectionPath = configPath.parent_path() / sectionPath;

		m_configFiles.emplace(section, sectionPath);
	}
}

ConfigFile* ConfigManager::getConfig(const std::string& configName)
{
	const auto it = m_configFiles.find(configName);
	if (it == m_configFiles.end())
		return nullptr;

	return &(it->second);
}

void ConfigManager::removeConfig(const std::string& configName)
{
	m_configFiles.erase(configName);
}
