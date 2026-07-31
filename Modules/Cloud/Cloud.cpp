// Created by yakov on 27/07/2026.

#include "Cloud.h"

Cloud::Cloud(const ConfigFile& configFile, const std::shared_ptr<StreamChannel>& StreamChannel) : StreamModule(StreamChannel)
{
}

CommandResult Cloud::execute(const CommandRequest& cmd)
{
	return {};
}

bool Cloud::handleStream(const StreamEvent& streamCtx)
{
	return false;
}
