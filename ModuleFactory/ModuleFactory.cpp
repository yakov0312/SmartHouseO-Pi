// Created by yakov on 24/07/2026.

#include "ModuleFactory.h"

#include "Modules/Cloud/Cloud.h"
#include "Modules/WakeOnLan/WakeOnLanManager.h"

std::unordered_map<std::string, ModuleFactory::Creator> ModuleFactory::s_modules =
{
	{"WakeOnLan", ModuleFactory::createWakeOnLan},
	{"Cloud", ModuleFactory::createCloud},
};

std::unique_ptr<Module> ModuleFactory::create(const std::string& name, ConfigFile& config, const std::shared_ptr<StreamChannel>& sChannel)
{
	const auto creator = s_modules.find(name);
	if (creator == s_modules.end())
		return nullptr;

	return creator->second(config, sChannel);
}

std::unique_ptr<Module> ModuleFactory::createWakeOnLan(ConfigFile& config, const std::shared_ptr<StreamChannel>&)
{
	return std::make_unique<WakeOnLanManager>(config);
}

std::unique_ptr<Module> ModuleFactory::createCloud(ConfigFile& config, const std::shared_ptr<StreamChannel>& sChannel)
{
	return std::make_unique<Cloud>(config, sChannel);
}
