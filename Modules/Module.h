// Created by yakov on 24/07/2026.

#pragma once

#include <string>
#include <vector>

struct CommandRequest
{
	uint64_t clientID;

	std::string service;
	std::string action;
	std::vector<std::string> args;
};

struct CommandResult
{
	uint64_t clientID;

	std::string message;
	bool streamTransfer;
};

class Module
{
public:
	virtual ~Module() = default;

	virtual CommandResult execute(const CommandRequest& cmd) = 0;
};


struct StreamEvent
{
	uint64_t clientID;

	std::string data;
	std::string service;
};

struct StreamChannel
{
	std::queue<StreamEvent> events;
	std::condition_variable condition;
	std::mutex mutex;
};

class StreamModule : public Module
{
public:
	explicit StreamModule(const std::shared_ptr<StreamChannel>& streamOutput) :
		m_streamChannel(streamOutput)
	{

	};

	virtual bool handleStream(const StreamEvent& streamCtx) = 0;

protected:
	std::shared_ptr<StreamChannel> m_streamChannel;
};