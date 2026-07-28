// Created by yakov on 24/07/2026.

#pragma once

#include <filesystem>
#include <string>
#include <unordered_map>

#include "ConfigFile.h"


constexpr auto DEFAULT_CONFIG = "../Configs/Config.conf";

class ConfigManager
{
public:
	explicit ConfigManager(const std::filesystem::path& configPath = DEFAULT_CONFIG);
	~ConfigManager() = default;

	inline std::unordered_map<std::string, ConfigFile>& getConfigs()
	{
		return m_configFiles;
	}

	ConfigFile* getConfig(const std::string& configName);
	void removeConfig(const std::string& configName);

private:
	std::unordered_map<std::string, ConfigFile> m_configFiles;
};