// Created by yakov on 07/08/2026.

#pragma once


#include <condition_variable>
#include <queue>
#include <shared_mutex>
#include <unordered_map>

struct StreamEvent
{
	uint64_t connectionId;

	std::string data;
	std::string service;
};

struct StreamConfiguration
{
	uint32_t maxInputChunk;
	uint32_t maxOutputChunk;
};

class StreamChannel
{
public:
	StreamChannel(uint16_t maxQueued);

	void swapQueues(std::queue<StreamEvent>& events, std::atomic_bool& running);
	void queueEvent(StreamEvent& event);
	void wakeConsumers();

	void initializeConfiguration(uint64_t id, const StreamConfiguration& config);
	void writeConfiguration(uint64_t id, const StreamConfiguration& configuration);
	void removeConfiguration(uint64_t id);

	StreamConfiguration getStreamConfiguration(uint64_t id);

private:
	std::queue<StreamEvent> m_pendingEvents;

	std::condition_variable m_dataAvailable;
	std::condition_variable m_spaceAvailable;
	std::mutex m_eventMutex;

	std::unordered_map<uint64_t, StreamConfiguration> m_streamConfigurations;
	std::shared_mutex m_configurationsMutex;

	uint16_t m_maxQueuedEvents;
};

