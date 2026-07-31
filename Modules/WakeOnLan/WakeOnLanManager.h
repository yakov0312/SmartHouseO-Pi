// Created by yakov on 24/07/2026.

#pragma once

#include "ConfigParser/ConfigFile.h"
#include "Modules/Module.h"

constexpr uint16_t MAC_SIZE = 6;

class WakeOnLanManager final : public Module
{
public:
	explicit WakeOnLanManager(ConfigFile& config);

	virtual CommandResult execute(const CommandRequest& cmd) override;

private:

	CommandResult handleDevices(const CommandRequest& cmd);
	CommandResult handleWake(const CommandRequest& cmd);
	bool sendWakePacket(const std::string& macAddress) const;


	static std::array<uint8_t, MAC_SIZE> parseMac(const std::string& mac);
	std::string getBroadcastAddress() const;


	std::string m_interface;

	ConfigFile& m_config;

	using CommandHandler = CommandResult(WakeOnLanManager::*)(const CommandRequest& cmd);

	std::unordered_map<std::string, WakeOnLanManager::CommandHandler> m_commands;
};


