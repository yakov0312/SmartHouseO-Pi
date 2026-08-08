// Created by yakov on 24/07/2026.


#include "Configuration/ConfigManager.h"
#include "Logging/Logger.h"
#include "Networking/Server/Server.h"


int main(const int argc, char* argv[])
{
	static ConfigManager config(argc > 1 ? argv[1] : DEFAULT_CONFIG);
	Logger::get().init(config.getConfig(LOGGER_CONFIG));
	config.removeConfig(LOGGER_CONFIG);

	try
	{
		Server server(config);
		server.run();
	}
	catch (const std::exception& e)
	{
		Logger::get().error(e.what());
	}

	return 0;
}
