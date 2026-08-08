// Created by yakov on 24/07/2026.

#pragma once

#include "Networking/Protocol/Commands/Commands.h"
#include "Networking/Protocol/Streams/Streams.h"

class Module
{
public:
	virtual ~Module() = default;

	virtual CommandResult execute(const CommandRequest& cmd, bool streamAllowed) = 0;
};


class StreamModule : public Module
{
public:
	explicit StreamModule(const std::shared_ptr<StreamChannel>& streamOutput);

	virtual bool handleStream(const StreamEvent& streamCtx) = 0;

	virtual void terminateStream(uint64_t clientId) = 0;

protected:
	std::shared_ptr<StreamChannel> m_streamChannel;

	void sendData(StreamEvent& event) const;
};