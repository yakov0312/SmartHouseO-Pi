// Created by yakov on 24/07/2026.

#include "ConfigFile.h"
#include <fstream>
#include <iostream>

constexpr auto DEFAULT_CONFIG = "./config.co";

ConfigFile::ConfigFile(const std::filesystem::path& configPath)
{
	if (configPath == "Enable")
		return;

	std::ifstream config(configPath);
	if (!config)
		throw std::runtime_error("Error: Unable to parse config. Cannot find config.");

	std::string line;
	while (getline(config, line))
	{
		if (line[0] == COMMENT_SYM)
			continue;

		size_t keyPos = line.find_first_of('=');
		if (keyPos == std::string::npos)
			continue;

		std::string key = line.substr(0, keyPos);
		if (m_config.contains(key))
			continue;

		std::string value = line.substr(keyPos + 1);
		if (value.empty())
			continue;

		m_config.emplace(key, value);
	}
}

std::string ConfigFile::getString(const std::string& key, const std::string& defaultValue) const
{
	const auto it = m_config.find(key);
	if (it == m_config.end())
		return defaultValue;

	return it->second;
}

int ConfigFile::getInt(const std::string& key, const int defaultValue) const
{
	const auto it = m_config.find(key);
	if (it == m_config.end())
		return defaultValue;

	try
	{
		return std::stoi(it->second);
	}
	catch (const std::exception& e)
	{
		return defaultValue;
	}
}
