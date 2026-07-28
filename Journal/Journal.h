// Created by yakov on 28/07/2026.

#pragma once

#include <filesystem>
#include <fstream>
#include <unordered_map>


struct ConfigChange
{
	std::string section;
	std::string key;
	std::string value;
};

class Journal
{
public:
	explicit Journal(const std::filesystem::path& path);

	void clear();
	void write(const ConfigChange& write);

	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> getJournal();

private:

	std::filesystem::path m_path;
	std::fstream m_journalFile;
};
