// Created by yakov on 29/07/2026.

#pragma once

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <string_view>

#include "ConfigParser/ConfigFile.h"

constexpr auto LOGGER_CONFIG = "Settings";

//#define ENABLE_DEBUG_LOG 1

class Logger
{
public:
	enum class Level
	{
		Debug,
		Setup,
		Runtime,
		Error
	};

	enum Option : uint8_t
	{
		None      = 0,
		Timestamp = 1 << 0,
		FileLog   = 1 << 1
	};

	enum LevelOption : uint8_t
	{
		SetupLog   = 1 << 1,
		RuntimeLog = 1 << 2,
		ErrorLog   = 1 << 3
	};

	static Logger& get();

	void init(ConfigFile* config);

	void log(Level level, std::string_view message);

	void debug(std::string_view message);
	void setup(std::string_view message);
	void runtime(std::string_view message);
	void error(std::string_view message);

	Logger(const Logger&) = delete;
	Logger& operator=(const Logger&) = delete;
	Logger(Logger&&) = delete;
	Logger& operator=(Logger&&) = delete;

private:
	Logger() = default;

	void write(Level level, std::string_view message);

	const char* levelToString(Level level) const;
	const char* levelColor(Level level) const;

	std::string buildMessage(Level level, std::string_view message) const;

	uint8_t m_options = None;
	uint8_t m_levels = 0;

	std::ofstream m_file;
	std::mutex m_mutex;
};

#ifdef ENABLE_DEBUG_LOG
#define LOG_DEBUG(msg) Logger::get().debug(msg)
#else
#define LOG_DEBUG(msg) ((void)0)
#endif