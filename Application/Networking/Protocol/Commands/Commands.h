// Created by yakov on 07/08/2026.

#pragma once

#include <cstdint>
#include <string>
#include <vector>

struct CommandRequest
{
	uint64_t connectionId;

	std::string action;
	std::vector<std::string> args;

	std::string username;
};

struct CommandResult
{
	std::string message;
	bool createStream;
};