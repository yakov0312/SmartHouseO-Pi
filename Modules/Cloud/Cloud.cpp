// Created by yakov on 27/07/2026.

#include "Cloud.h"

#include "Logger/Logger.h"

constexpr auto DEFAULT_SAVE_DIR = "Cloud";

Cloud::Cloud(ConfigFile& configFile, const std::shared_ptr<StreamChannel>& StreamChannel) : StreamModule(StreamChannel),
	m_commands{{"Upload", &Cloud::uploadFile}, {"Download", &Cloud::downloadFile}}
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
}

CommandResult Cloud::execute(const CommandRequest& cmd)
{
	LOG_DEBUG("Executing Cloud command: " + cmd.action);


	const auto it = m_commands.find(cmd.action);
	if (it == m_commands.end())
	{
		LOG_DEBUG("Command not found: " + cmd.action);
		return {cmd.clientID, "Command not found"};
	}

	return (this->*(it->second))(cmd);
}

bool Cloud::handleStream(const StreamEvent& streamCtx)
{
	const auto it = m_files.find(streamCtx.clientID);
	if (it == m_files.end())
		return false;

	if (it->second.type != StreamType::Upload)
		return true; // Client is not allowed to send data

	if (streamCtx.data.empty())
	{
		it->second.file.close();
		m_files.erase(it);
		return false;
	}

	it->second.file.write(streamCtx.data.data(), streamCtx.data.size());

	if (!it->second.file)
	{
		it->second.file.close();
		m_files.erase(it);
		return false;
	}

	return true;
}

CommandResult Cloud::uploadFile(const CommandRequest& cmd)
{
	if (cmd.args.empty())
		return {cmd.clientID, "No file name is provided"};

	const std::filesystem::path requested(cmd.args.at(0));
	const std::filesystem::path path =  m_cloudDir / requested.filename();

	CommandResult result{cmd.clientID};

	auto [it, inserted] = m_files.try_emplace(
		cmd.clientID,
		FileStream{
			StreamType::Upload,
			std::fstream(path, std::ios::binary | std::ios::app | std::ios::out)
		}
	);

	if (inserted && it->second.file.is_open())
	{
		result.message = "File have been created";
		result.streamTransfer = true; // Make the connection a stream
	}
	else
		result.message = "Failed to create the file";

	return result;
}

CommandResult Cloud::downloadFile(const CommandRequest& cmd)
{
	if (cmd.args.empty())
		return {cmd.clientID, "No file name is provided"};

	const std::filesystem::path requested(cmd.args.at(0));
	const std::filesystem::path path =  m_cloudDir / requested.filename();

	CommandResult result{cmd.clientID};


	auto work = [this, clientID = cmd.clientID]()
	{
		this->downloadStreamer(clientID);
	};

	auto [it, inserted] = m_files.try_emplace(
		cmd.clientID,
		FileStream{
			StreamType::Download,
			std::fstream(path, std::ios::binary | std::ios::in)
		}
	);

	if (inserted && it->second.file.is_open())
	{
		result.message = "File have been opened for reading";
		result.streamTransfer = true; // Make the connection a stream
		result.work.emplace_back(work);
	}
	else
		result.message = "Failed to open the file";

	return result;
}

void Cloud::downloadStreamer(const uint64_t clientID)
{
	constexpr size_t CHUNK_SIZE = 16 * 1024;

	const auto it = m_files.find(clientID);
	if (it == m_files.end())
		return;

	while (true)
	{
		std::string buffer(CHUNK_SIZE, '\0');

		it->second.file.read(buffer.data(), CHUNK_SIZE);

		const auto bytesRead = it->second.file.gcount();
		if (bytesRead <= 0)
			break;

		buffer.resize(bytesRead);

		sendData({
			clientID,
			std::move(buffer),
			"Cloud"
		});
	}

	m_files.erase(it);

	sendData({
		clientID,
		"",
		"Cloud"
	});
}
