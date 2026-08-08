// Created by yakov on 07/08/2026.

#include "Streams.h"

StreamChannel::StreamChannel(const uint16_t maxQueued) : m_maxQueuedEvents(maxQueued)
{
}

void StreamChannel::swapQueues(std::queue<StreamEvent>& events, std::atomic_bool& running)
{
	std::unique_lock lock(m_eventMutex);

	auto cond = [&running, this]
	{
		return !running.load() || !m_pendingEvents.empty();
	};

	// Sleep until a module generates stream Runtime
	// or shutdown is requested.
	m_dataAvailable.wait(lock, cond);

	std::swap(events, m_pendingEvents);

	// Wake producers waiting because the queue was full
	m_spaceAvailable.notify_all();
}

void StreamChannel::queueEvent(StreamEvent& event)
{
	std::unique_lock lock(m_eventMutex);

	m_spaceAvailable.wait(lock, [&]
	{
		return m_pendingEvents.size() < m_maxQueuedEvents;
	});

	m_pendingEvents.emplace(std::move(event));

	lock.unlock();
	m_dataAvailable.notify_one();
}

void StreamChannel::wakeConsumers()
{
	m_dataAvailable.notify_all();
}

void StreamChannel::initializeConfiguration(const uint64_t id, const StreamConfiguration& config)
{
	std::lock_guard lock(m_configurationsMutex);
	m_streamConfigurations.try_emplace(id, config);
}

void StreamChannel::writeConfiguration(const uint64_t id, const StreamConfiguration& configuration)
{
	std::lock_guard lock(m_configurationsMutex);
	m_streamConfigurations[id] = configuration;
}

void StreamChannel::removeConfiguration(uint64_t id)
{
	std::lock_guard lock(m_configurationsMutex);
	m_streamConfigurations.erase(id);
}

StreamConfiguration StreamChannel::getStreamConfiguration(const uint64_t id)
{
	std::shared_lock lock(m_configurationsMutex);
	const auto it = m_streamConfigurations.find(id);
	if (it == m_streamConfigurations.end())
		return {};

	return it->second;
}
