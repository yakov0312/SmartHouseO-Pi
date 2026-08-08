// Created by yakov on 02/08/2026.

#pragma once

#include <atomic>
#include <unordered_map>
#include <vector>

#include "Storage/Database/DataBase.h"

struct AuthenticationState
{
	std::string username;

	std::atomic_bool authenticated = false;
	std::atomic_uint64_t userID = 0;
};

struct AuthCommand
{
	std::string command;
	std::vector<std::string> args;
};

class AuthManager
{
public:
	AuthManager();

	std::string processAuth(const AuthCommand& command, AuthenticationState& client);

private:
	std::string loginUser(const AuthCommand& command, AuthenticationState& client);
	std::string registerUser(const AuthCommand& command, AuthenticationState& client);
	std::string logoutUser(const AuthCommand& command, AuthenticationState& client);

	using CommandHandler = std::string(AuthManager::*)(const AuthCommand&, AuthenticationState&);

	std::unordered_map<std::string, CommandHandler> m_commands;

	DataBase m_database;
};