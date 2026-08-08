// Created by yakov on 01/08/2026.

#include "Module.h"

StreamModule::StreamModule(const std::shared_ptr<StreamChannel>& streamOutput) : m_streamChannel(streamOutput)
{
}

void StreamModule::sendData(StreamEvent& event) const
{
	m_streamChannel->queueEvent(event);
}
