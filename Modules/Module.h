// Created by yakov on 24/07/2026.

#pragma once

#include <string>
#include <vector>

struct Command
{
	int clientFd;

	std::string service;
	std::string action;
	std::vector<std::string> args;
};

struct Result
{
	int clientFd;

	std::string message;
};

class Module
{
public:
	virtual ~Module() = default;

	virtual Result execute(const Command& cmd) = 0;
};