// Created by yakov on 29/07/2026.

#pragma once
#include <condition_variable>
#include <functional>
#include <queue>
#include <thread>
#include <vector>

struct Task
{
	std::function<void()> execute;
};

struct ClientWorkQueue
{
	std::queue<Task> queue;
	std::mutex mutex;
	bool freeAfterDone = false;
};

class ThreadPool
{
public:
	explicit ThreadPool(uint8_t threadCount);
	~ThreadPool();

	void increaseThreads(uint8_t threadCount);

	void schedule(int clientID, const std::function<void()>& task);
	void remove(int ClientID);

	size_t getCurrentTasksCount(int ClientID);

private:
	void worker();

	std::vector<std::thread> m_workers;

	std::unordered_map<int, std::shared_ptr<ClientWorkQueue>> m_clientQueues;
	std::mutex m_clientQueuesMtx;

	std::queue<int> m_readyClients;
	std::mutex m_readyClientsMtx;

	std::condition_variable m_cv;
	bool m_stop = false;
};
