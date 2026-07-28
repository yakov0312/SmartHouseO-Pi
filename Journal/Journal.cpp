// Created by yakov on 28/07/2026.

#include "Journal.h"

Journal::Journal(const std::filesystem::path& path) : m_path(path), m_journalFile(path, std::ios::out | std::ios::in | std::ios::app)
{
	if (!m_journalFile)
		throw std::runtime_error("Unable to open journal");
}

void Journal::clear()
{
	m_journalFile.close();
	m_journalFile.open(m_path, std::ios::out | std::ios::in | std::ios::trunc);
}

void Journal::write(const ConfigChange& write)
{
	if (write.value.empty())
		m_journalFile << '[' << write.section << "]\n" << write.key << '\n';
	else
		m_journalFile << '[' << write.section << "]\n" << write.key << "=" << write.value << '\n';

	m_journalFile.flush();

}

std::unordered_map<std::string, std::unordered_map<std::string, std::string>> Journal::getJournal()
{
	m_journalFile.clear();
	m_journalFile.seekg(0);

	std::unordered_map<std::string, std::unordered_map<std::string, std::string>> cachedJournal;

	std::string line;
	std::unordered_map<std::string, std::string>* section = nullptr;

	while (getline(m_journalFile, line))
	{
		if (line.empty())
			continue;

		if (line[0] == '[')
		{
			if (line.back() != ']' && line.size() <= 2)
				continue;

			std::string sectionName = line.substr(1, line.size() - 2);
			section = &cachedJournal.try_emplace(sectionName).first->second;
		}
		else if (section != nullptr)
		{
			const size_t keyPos = line.find_first_of('=');
			if (keyPos == std::string::npos)
			{
				(*section)[line] = "";
				continue;
			}

			std::string key = line.substr(0, keyPos);

			std::string value = line.substr(keyPos + 1);
			if (value.empty())
				continue;

			(*section)[key] = value;
		}
	}

	return cachedJournal;
}
