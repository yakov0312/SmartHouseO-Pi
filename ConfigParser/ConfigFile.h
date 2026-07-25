// Created by yakov on 24/07/2026.

#pragma once
#include <filesystem>
#include <string>
#include <unordered_map>

constexpr char COMMENT_SYM = '#';

class ConfigFile
{
public:
	explicit ConfigFile(const std::filesystem::path& configPath);
	~ConfigFile() = default;

	std::string getString(const std::string& key, const std::string& defaultValue = "") const;
	int getInt(const std::string& key, int defaultValue = 0) const;

private:
	std::unordered_map<std::string, std::filesystem::path> m_config;
};


