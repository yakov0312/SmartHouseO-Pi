// Created by yakov on 27/07/2026.

#pragma once

#include "ConfigParser/ConfigFile.h"
#include "Modules/Module.h"

class Cloud final : public StreamModule
{
public:
	explicit Cloud(const ConfigFile& configFile, const std::shared_ptr<StreamChannel>& StreamChannel);

	CommandResult execute(const CommandRequest& cmd) override;
	bool handleStream(const StreamEvent& streamCtx) override;

private:

	using CommandHandler = CommandResult(Cloud::*)(const CommandRequest& cmd);

	std::unordered_map<std::string, CommandHandler> m_commands;
};
