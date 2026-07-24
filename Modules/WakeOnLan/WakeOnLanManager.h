// Created by yakov on 24/07/2026.

#pragma once

#include "ConfigParser/ConfigFile.h"
#include "Modules/Module.h"

class WakeOnLanManager : public Module
{
public:
	explicit WakeOnLanManager(const ConfigFile& config);

	virtual Result execute(const Command& cmd) override;

private:
};


