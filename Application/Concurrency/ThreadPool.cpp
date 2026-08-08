// Created by yakov on 29/07/2026.

#include "ThreadPool.h"

ThreadPool::ThreadPool(const uint8_t threadCount)
{
	for (uint8_t i = 0; i < threadCount; i++)
		m_workers.emplace_back(&ThreadPool::worker, this);
}

ThreadPool::~ThreadPool()
{
	{
		std::lock_guard lock(m_clientQueuesMtx);
		m_stop = true;
	}

	m_cv.notify_all();
	for (auto& t : m_workers)
		t.join();
}

void ThreadPool::increaseThreads(uint8_t threadCount)
{
	for (uint8_t i = 0; i < threadCount; i++)
		m_workers.emplace_back(&ThreadPool::worker, this);
}

void ThreadPool::schedule(const int clientID, const std::function<void()>& task)
{
	std::shared_ptr<ClientWorkQueue> clientQueue;

	{
		std::lock_guard lock(m_clientQueuesMtx);

		auto it = m_clientQueues.find(clientID);
		if (it == m_clientQueues.end())
		{
			it = m_clientQueues.emplace(clientID, std::make_shared<ClientWorkQueue>()).first;
		}

		clientQueue = it->second;
	}

	{
		std::lock_guard lock(clientQueue->mutex);

		if (clientQueue->freeAfterDone)
			return;

		clientQueue->queue.emplace(task);

		if (clientQueue->queue.size() == 1)
		{
			{
				std::lock_guard readyLock(m_readyClientsMtx);
				m_readyClients.push(clientID);
			}

			m_cv.notify_one();
		}
	}
}

void ThreadPool::remove(const int ClientID)
{
	std::shared_ptr<ClientWorkQueue> clientQueue;

	{
		std::lock_guard lock(m_clientQueuesMtx);
		clientQueue = m_clientQueues[ClientID];
	}

	{
		std::lock_guard lock(clientQueue->mutex);

		clientQueue->freeAfterDone = true;

		if (clientQueue->queue.empty())
		{
			{
				std::lock_guard readyLock(m_readyClientsMtx);
				m_readyClients.push(ClientID);
			}

			m_cv.notify_one();
		}
	}
}

size_t ThreadPool::getCurrentTasksCount(const int ClientID)
{
	const auto it = m_clientQueues.find(ClientID);
	if (it == m_clientQueues.end())
		return 0;

	const std::shared_ptr<ClientWorkQueue> clientQueue = it->second;

	std::lock_guard lock(clientQueue->mutex);

	return clientQueue->queue.size();
}

void ThreadPool::worker()
{
	while (true)
	{
		std::shared_ptr<ClientWorkQueue> clientQueue;
		int fd = 0;
		Task task;

		{
			std::unique_lock lock(m_readyClientsMtx);
			m_cv.wait(lock, [this] { return m_stop || !m_readyClients.empty(); });

			if (m_stop && m_readyClients.empty())
				return;

			fd = m_readyClients.front();
			m_readyClients.pop();
		}

		{
			std::lock_guard lock(m_clientQueuesMtx);
			clientQueue = m_clientQueues[fd];
		}

		std::unique_lock lock(clientQueue->mutex);
		while (!clientQueue->queue.empty())
		{
			task = std::move(clientQueue->queue.front());
			lock.unlock();

			task.execute();

			lock.lock();
			clientQueue->queue.pop();
		}

		if (clientQueue->freeAfterDone)
		{
			std::lock_guard clientLock(m_clientQueuesMtx);
			m_clientQueues.erase(fd);
		}

	}
}
