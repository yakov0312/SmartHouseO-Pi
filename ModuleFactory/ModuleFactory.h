// Created by yakov on 24/07/2026.

#pragma once

#include <memory>

#include "ConfigParser/ConfigFile.h"
#include "Modules/Module.h"

class ModuleFactory
{
public:
	static std::unique_ptr<Module> create(const std::string& name, const ConfigFile& config);

private:
	static std::unique_ptr<Module> createWakeOnLan(const std::string& name, const ConfigFile& config);

	using Creator = std::unique_ptr<Module>(*)(const std::string&, const ConfigFile&);
	static std::unordered_map<std::string, Creator> s_modules;
};