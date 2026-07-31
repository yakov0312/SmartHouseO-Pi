// Created by yakov on 24/07/2026.

#pragma once

#include <memory>

#include "ConfigParser/ConfigFile.h"
#include "Modules/Module.h"

class ModuleFactory
{
public:
	static std::unique_ptr<Module> create(const std::string& name, ConfigFile& config, const std::shared_ptr<StreamChannel>& sChannel);

private:
	static std::unique_ptr<Module> createWakeOnLan(ConfigFile& config, const std::shared_ptr<StreamChannel>&);
	static std::unique_ptr<Module> createCloud(ConfigFile& config, const std::shared_ptr<StreamChannel>& sChannel);

	using Creator = std::unique_ptr<Module>(*)(ConfigFile&, const std::shared_ptr<StreamChannel>&);
	static std::unordered_map<std::string, Creator> s_modules;
};