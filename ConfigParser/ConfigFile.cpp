// Created by yakov on 24/07/2026.

#include "ConfigFile.h"
#include <fstream>

constexpr auto DEFAULT_CONFIG = "./config.co";

ConfigFile::ConfigFile(const std::filesystem::path& configPath)
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
}

std::string ConfigFile::getString(const std::string& section, const std::string& key, const std::string& defaultValue) const
{
	const auto sectionIt = m_config.find(section);
	if (sectionIt == m_config.end())
		return defaultValue;

	const auto it = sectionIt->second.find(key);
		if (it == sectionIt->second.end())
			return defaultValue;

	return it->second;
}

int ConfigFile::getInt(const std::string& section, const std::string& key, const int defaultValue) const
{
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
	catch (const std::exception& e)
	{
		return defaultValue;
	}
}

const std::unordered_map<std::string, std::string>* ConfigFile::getConfig(const std::string& section) const
{
	const auto it = m_config.find(section);
	if (it == m_config.end())
		return nullptr;

	return &it->second;
}
