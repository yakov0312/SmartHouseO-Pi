// Created by yakov on 24/07/2026.

#include "ConfigParser/ConfigManager.h"
#include "Server/Server.h"

int main(const int argc, char* argv[])
{
	static ConfigManager config(argc > 1 ? argv[1] : DEFAULT_CONFIG);

	Server server(config);

	return 0;
};