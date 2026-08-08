// Created by yakov on 27/07/2026.

#pragma once
#include "Configuration/ConfigFile.h"
#include "Services/Modules/Module.h"

enum class StreamType
{
	Upload,
	Download
};

struct FileStream
{
	StreamType type;
	std::fstream file;
	bool finished;
};

class Cloud final : public StreamModule
{
public:
	explicit Cloud(ConfigFile& configFile, const std::shared_ptr<StreamChannel>& StreamChannel);
	~Cloud();

	CommandResult execute(const CommandRequest& cmd, bool streamAllowed) override;
	bool handleStream(const StreamEvent& streamCtx) override;

	virtual void terminateStream(uint64_t clientId) override;

private:

	CommandResult uploadFile(const CommandRequest& cmd);
	CommandResult downloadFile(const CommandRequest& cmd);
	CommandResult deleteFile(const CommandRequest& cmd);
	CommandResult listDir(const CommandRequest& cmd);

	void downloadStreamer();

	std::filesystem::path m_cloudDir;

	std::unordered_map<uint64_t, FileStream> m_files;
	std::mutex m_filesMutex;

	std::mutex m_downloadsMutex;
	std::queue<uint64_t> m_clientsDownloads;
	std::condition_variable m_downloadCond;

	struct Handler
	{
		using CommandHandler = CommandResult(Cloud::*)(const CommandRequest&);

		CommandHandler handler;
		bool streamCapable;
	};

	std::unordered_map<std::string, Handler> m_commands;

	std::atomic_bool m_running;
	std::thread m_downloadThread;
};
