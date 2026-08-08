// Created by yakov on 27/07/2026.

#include "Cloud.h"

#include "Logging/Logger.h"

constexpr auto DEFAULT_SAVE_DIR = "Cloud";

Cloud::Cloud(ConfigFile& configFile, const std::shared_ptr<StreamChannel>& StreamChannel) : StreamModule(StreamChannel),
	m_commands{
		{"Upload", {&Cloud::uploadFile, true}},
		{"Download", {&Cloud::downloadFile, true}}
	}
{
	m_cloudDir = configFile.getString(SETTINGS_SECTION, "Saves", DEFAULT_SAVE_DIR);

	if (!std::filesystem::exists(m_cloudDir))
	{
		std::error_code ec;
		std::filesystem::create_directories(m_cloudDir, ec);
		if (ec)
			throw std::runtime_error("Failed to create the cloud dir");
	}
	else if (!std::filesystem::is_directory(m_cloudDir))
		throw std::runtime_error("Provided path for the cloud is not a directory");

	m_downloadThread = std::thread( &Cloud::downloadStreamer, this);
	m_running = true;
}

Cloud::~Cloud()
{
	m_running = false;
	m_downloadCond.notify_all();

	if (m_downloadThread.joinable())
		m_downloadThread.join();
}

CommandResult Cloud::execute(const CommandRequest& cmd, const bool streamAllowed)
{
	LOG_DEBUG("Executing Cloud command: " + cmd.action);


	const auto it = m_commands.find(cmd.action);
	if (it == m_commands.end())
	{
		LOG_DEBUG("Command not found: " + cmd.action);
		return {"Command not found"};
	}

	if (!streamAllowed && it->second.streamCapable)
		return {"Command is not allowed to be executed while having an open stream", false};

	return (this->*(it->second.handler))(cmd);
}

bool Cloud::handleStream(const StreamEvent& streamCtx)
{
	FileStream* file;
	{
		std::lock_guard lock(m_filesMutex);
		const auto it = m_files.find(streamCtx.connectionId);
		if (it == m_files.end())
			return false;
		file = &it->second;
	}

	if (file->type != StreamType::Upload)
		return true; // Client is not allowed to send data

	if (streamCtx.data.empty())
	{
		file->file.close();
		std::lock_guard lock(m_filesMutex);
		m_files.erase(streamCtx.connectionId);
		return false;
	}

	file->file.write(streamCtx.data.data(), streamCtx.data.size());

	if (!file->file)
	{
		file->file.close();
		std::lock_guard lock(m_filesMutex);
		m_files.erase(streamCtx.connectionId);
		return false;
	}

	return true;
}

void Cloud::terminateStream(const uint64_t clientId)
{
	std::lock_guard lock(m_filesMutex);
	m_files.erase(clientId); // Will close the files as well
}

CommandResult Cloud::uploadFile(const CommandRequest& cmd)
{
	if (cmd.args.empty())
		return {"No file name is provided"};

	const std::filesystem::path userDir = m_cloudDir / cmd.username; // Username can include only a-Z, numbers and '_'
	std::filesystem::create_directories(userDir);

	const std::filesystem::path requested(cmd.args.at(0));
	const std::filesystem::path path = userDir / requested.filename();

	CommandResult result{};

	std::unique_lock<std::mutex> lock(m_filesMutex);
	auto [it, inserted] = m_files.try_emplace(
		cmd.connectionId,
		FileStream{
			StreamType::Upload,
			std::fstream(path, std::ios::binary | std::ios::app | std::ios::out)
		}
	);
	lock.unlock();

	if (inserted && it->second.file.is_open())
	{
		result.message = "File have been created";
		result.createStream = true; // Make the connection a stream

		Logger::get().runtime("User " + cmd.username + " has started a upload stream");
	}
	else
		result.message = "Failed to create the file";

	return result;
}

CommandResult Cloud::downloadFile(const CommandRequest& cmd)
{
	if (cmd.args.empty())
		return {"No file name is provided"};

	const std::filesystem::path requested(cmd.args.at(0));
	const std::filesystem::path path = m_cloudDir / cmd.username / requested.filename();

	CommandResult result{};

	std::unique_lock lock(m_filesMutex);
	auto [it, inserted] = m_files.try_emplace(
		cmd.connectionId,
		FileStream{
			StreamType::Download,
			std::fstream(path, std::ios::binary | std::ios::in)
		}
	);
	lock.unlock();

	if (inserted && it->second.file.is_open())
	{
		result.message = "File have been opened for reading";
		result.createStream = true; // Make the connection a stream

		Logger::get().runtime("User " + cmd.username + " has started a download stream");
		{
			std::lock_guard dLock(m_downloadsMutex);
			m_clientsDownloads.push(cmd.connectionId);
		}

		m_downloadCond.notify_all();
	}
	else
		result.message = "Failed to open the file";

	return result;
}

CommandResult Cloud::deleteFile(const CommandRequest& cmd)
{
	if (cmd.args.empty())
		return {"No file name is provided"};

	const std::filesystem::path requested(cmd.args.at(0));
	const std::filesystem::path path = m_cloudDir / cmd.username / requested.filename();

	CommandResult result{};
	const bool wasRemoved = std::filesystem::remove(path);
	if (wasRemoved)
		result.message = "Successfully deleted the file";
	else
		result.message = "Failed to delete the file";

	return result;
}

CommandResult Cloud::listDir(const CommandRequest& cmd)
{
	CommandResult result{};

	const std::filesystem::path userDir = m_cloudDir / cmd.username;

	for (const auto& entry : std::filesystem::directory_iterator(userDir))
	{
		result.message += entry.path().filename().string();
		if (entry.is_directory())
			result.message += '/';
		result.message += "\n";
	}

	return result;
}

void Cloud::downloadStreamer()
{
	while (m_running)
	{
		std::unique_lock lock(m_downloadsMutex);

		auto cond = [this]()
		{
			return !m_clientsDownloads.empty() || !m_running;
		};

		m_downloadCond.wait(lock, cond);

		while (!m_clientsDownloads.empty())
		{
			uint64_t clientID = m_clientsDownloads.front();
			m_clientsDownloads.pop();

			lock.unlock();

			const StreamConfiguration configuration = m_streamChannel->getStreamConfiguration(clientID);

			FileStream* file;
			{
				std::lock_guard fLock(m_filesMutex);
				auto it = m_files.find(clientID);
				if (it == m_files.end())
					continue;

				file = &it->second;
			}

			if (file->finished)
			{
				StreamEvent event = {clientID, "", "Cloud"};
				sendData(event);

				{
					std::lock_guard fLock(m_filesMutex);
					m_files.erase(clientID);
				}

				lock.lock();
				continue;
			}

			std::string buffer(configuration.maxOutputChunk, '\0');

			file->file.read(buffer.data(), configuration.maxOutputChunk);

			const auto bytesRead = file->file.gcount();

			if (bytesRead <= 0)
				file->finished = true;
			else
			{
				buffer.resize(bytesRead);

				StreamEvent event = {clientID, std::move(buffer), "Cloud"};
				sendData(event);
			}

			lock.lock();
			m_clientsDownloads.push(clientID);
		}
	}
}
