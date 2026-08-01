// Created by yakov on 27/07/2026.

#pragma once

#include "ConfigParser/ConfigFile.h"
#include "Modules/Module.h"

enum class StreamType
{
	Upload,
	Download
};

struct FileStream
{
	StreamType type;
	std::fstream file;
};

class Cloud final : public StreamModule
{
public:
	explicit Cloud(ConfigFile& configFile, const std::shared_ptr<StreamChannel>& StreamChannel);

	CommandResult execute(const CommandRequest& cmd) override;
	bool handleStream(const StreamEvent& streamCtx) override;

private:

	CommandResult uploadFile(const CommandRequest& cmd);
	CommandResult downloadFile(const CommandRequest& cmd);

	void downloadStreamer(uint64_t clientID);

	std::filesystem::path m_cloudDir;

	std::unordered_map<uint64_t, FileStream> m_files;

	using CommandHandler = CommandResult(Cloud::*)(const CommandRequest& cmd);

	std::unordered_map<std::string, CommandHandler> m_commands;
};
