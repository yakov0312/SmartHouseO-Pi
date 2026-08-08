// Created by yakov on 02/08/2026.

#include "AuthManager.h"

#include <algorithm>

AuthManager::AuthManager() :
	m_commands{
		{"Login", &AuthManager::loginUser},
		{"Register", &AuthManager::registerUser},
		{"Logout", &AuthManager::logoutUser}}
{
}

std::string AuthManager::processAuth(const AuthCommand& command, AuthenticationState& client)
{
	auto it = m_commands.find(command.command);
	if (it == m_commands.end())
		return "Command not found";

	return (this->*(it->second))(command, client);
}

std::string AuthManager::loginUser(const AuthCommand& command, AuthenticationState& client)
{
	if (command.args.size() < 2)
		return "Invalid command call";

	const std::string& username = command.args[0];
	const std::string& password = command.args[1];

	uint64_t id = m_database.validateUser(username, password);
	if (id != 0)
	{
		client.authenticated = true;
		client.userID = id;
		client.username = username;
		return "Logged in successfully";
	}

	return "Invalid username or password";
}

std::string AuthManager::registerUser(const AuthCommand& command, AuthenticationState& client)
{
	if (command.args.size() < 2)
		return "Invalid command call";

	const std::string& username = command.args[0];
	const std::string& password = command.args[1];

	auto cond = [](const char c){ return std::isalnum(c) || c == '_'; };
	if (!std::ranges::all_of(username, cond))
		return {"Invalid username"};

	const uint64_t id = m_database.createUser(command.args[0], command.args[1]);
	if (id != 0)
	{
		client.authenticated = true;
		client.userID = id;
		client.username = username;
		return "Registered user successfully";
	}

	return "Failed to register user";
}

std::string AuthManager::logoutUser(const AuthCommand& command, AuthenticationState& client)
{
	client.authenticated = false;
	client.userID = 0;

	return "";
}