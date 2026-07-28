// Created by yakov on 27/07/2026.

#pragma once

#include "ConfigParser/ConfigFile.h"
#include "Modules/Module.h"

class Cloud final : public Module
{
public:
	explicit Cloud(const ConfigFile& configFile);

	Result execute(const Command& cmd) override;

private:

	using CommandHandler = Result(Cloud::*)(const Command& cmd);

	std::unordered_map<std::string, Cloud::CommandHandler> m_commands;
};
