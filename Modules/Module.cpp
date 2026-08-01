// Created by yakov on 01/08/2026.

#include "Module.h"

StreamModule::StreamModule(const std::shared_ptr<StreamChannel>& streamOutput) : m_streamChannel(streamOutput)
{
}

void StreamModule::sendData(const StreamEvent& event) const
{
	{
		std::lock_guard lock(m_streamChannel->mutex);
		m_streamChannel->events.emplace(std::move(event));
	}

	m_streamChannel->condition.notify_one();
}
