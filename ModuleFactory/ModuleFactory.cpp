// Created by yakov on 24/07/2026.

#include "ModuleFactory.h"

#include "../WakeOnLan/WakeOnLanManager.h"

std::unordered_map<std::string, ModuleFactory::Creator> ModuleFactory::s_modules =
{
	{"WakeOnLan", ModuleFactory::createWakeOnLan}
};

std::unique_ptr<Module> ModuleFactory::create(const std::string& name, const ConfigFile& config)
{
	const auto creator = ModuleFactory::s_modules.find(name);
	if (creator == ModuleFactory::s_modules.end())
		return nullptr;

	return creator->second(name, config);
}

std::unique_ptr<Module> ModuleFactory::createWakeOnLan(const std::string& name, const ConfigFile& config)
{
	return std::make_unique<WakeOnLanManager>(config);
}
