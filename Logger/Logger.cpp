// Created by yakov on 29/07/2026.

#include "Logger.h"

#include <chrono>
#include <iomanip>
#include <iostream>
#include <sstream>

Logger& Logger::get()
{
	static Logger instance;
	return instance;
}

void Logger::init(ConfigFile* config)
{
	std::lock_guard lock(m_mutex);

	m_options = None;
	m_levels = 0;

	if (config == nullptr)
		return;

	if (config->getString(LOGGER_CONFIG, "Timestamp", "Disable") == "Enable")
		m_options |= Timestamp;

	const auto fileLogging = config->getString(LOGGER_CONFIG, "File", "");

	if (!fileLogging.empty())
	{
		m_options |= FileLog;
		m_file.open(fileLogging, std::ios::app);
	}

	if (config->getString(LOGGER_CONFIG, "Setup", "Disable") == "Enable")
		m_levels |= SetupLog;

	if (config->getString(LOGGER_CONFIG, "Runtime", "Disable") == "Enable")
		m_levels |= RuntimeLog;

	if (config->getString(LOGGER_CONFIG, "Error", "Disable") == "Enable")
		m_levels |= ErrorLog;
}

void Logger::log(const Level level, const std::string_view message)
{
	uint8_t levelFlag = 0;

	switch (level)
	{
		case Level::Debug:
			write(level, message);
			return;

		case Level::Setup:
			levelFlag = SetupLog;
			break;

		case Level::Runtime:
			levelFlag = RuntimeLog;
			break;

		case Level::Error:
			levelFlag = ErrorLog;
			break;
	}

	if (!(m_levels & levelFlag))
		return;

	write(level, message);
}

void Logger::debug(const std::string_view message)
{
	log(Level::Debug, message);
}

void Logger::setup(const std::string_view message)
{
	log(Level::Setup, message);
}

void Logger::runtime(const std::string_view message)
{
	log(Level::Runtime, message);
}

void Logger::error(const std::string_view message)
{
	log(Level::Error, message);
}

void Logger::write(const Level level, const std::string_view message)
{
	const std::string formatted = buildMessage(level, message);

	std::lock_guard lock(m_mutex);

	// Colored console output
	std::cout << levelColor(level)
			  << formatted
			  << "\033[0m\n";

	// Plain file output
	if ((m_options & FileLog) && m_file)
	{
		m_file << formatted << '\n';
		m_file.flush();
	}
}

std::string Logger::buildMessage(const Level level, const std::string_view message) const
{
	std::ostringstream stream;

	if (m_options & Timestamp)
	{
		const auto now = std::chrono::system_clock::now();
		const auto time = std::chrono::system_clock::to_time_t(now);

		stream << '['
			   << std::put_time(std::localtime(&time), "%Y-%m-%d %H:%M:%S")
			   << "] ";
	}

	stream << '[' << levelToString(level) << "] ";
	stream << message;

	return stream.str();
}

const char* Logger::levelToString(const Level level) const
{
	switch (level)
	{
		case Level::Debug:
			return "DEBUG";

		case Level::Setup:
			return "SETUP";

		case Level::Runtime:
			return "RUNTIME";

		case Level::Error:
			return "ERROR";
	}

	return "UNKNOWN";
}

const char* Logger::levelColor(const Level level) const
{
	switch (level)
	{
		case Level::Debug:
			return "\033[90m"; // Gray

		case Level::Setup:
			return "\033[36m"; // Cyan

		case Level::Runtime:
			return "\033[32m"; // Green

		case Level::Error:
			return "\033[31m"; // Red
	}

	return "\033[0m";
}