// Created by yakov on 24/07/2026.

#include "WakeOnLanManager.h"

#include <ranges>

#include <ifaddrs.h>
#include <unistd.h>
#include <arpa/inet.h>
#include <net/if.h>
#include <netinet/in.h>
#include <sys/socket.h>


/**
 * @brief Creates a WakeOnLanManager instance.
 *
 * Initializes command handlers and loads optional settings from the
 * configuration file. Commands can be disabled through the Settings section.
 *
 * @param config Configuration source containing settings and device data.
 */
WakeOnLanManager::WakeOnLanManager(const ConfigFile& config) : m_config(config),
		m_commands{{"Devices", &WakeOnLanManager::handleDevices}, {"Wake", &WakeOnLanManager::handleWake}}

{
	const auto conf = config.getConfig("Settings");
	if (conf == nullptr)
		return;

	m_interface = config.getString("Settings", "Interface", "");

	for (const auto& [command, State] : *conf)
	{
		if (State == "Disable")
		{
			auto it = m_commands.find(command);
			if (it == m_commands.end())
				continue;

			it->second = nullptr; // Disabled the command
		}
	}
}

/**
 * @brief Executes a command received by the manager.
 *
 * Looks up the command action in the registered command table and calls
 * the corresponding handler if it exists and is enabled.
 *
 * @param cmd Command information including action, arguments and client fd.
 * @return Result containing the response message.
 */
Result WakeOnLanManager::execute(const Command& cmd)
{
	const auto it = m_commands.find(cmd.action);
	if (it == m_commands.end())
		return {cmd.clientFd, "Command not found"};

	if (it->second == nullptr)
		return {cmd.clientFd, "Command is not enabled"};

	return (this->*(it->second))(cmd);
}

/**
 * @brief Handles device management commands.
 *
 * Currently supports listing configured devices. Future implementations
 * may add adding/removing devices.
 *
 * @param cmd Device command and arguments.
 * @return Result containing the operation status.
 */
Result WakeOnLanManager::handleDevices(const Command& cmd)
{
	if (cmd.args.empty())
		return {cmd.clientFd, "Invalid command call. usage WakeOnLan Devices Get/Add/Remove"};

	if (cmd.args[0] == "Get")
	{
		const auto devices = m_config.getConfig("Devices");
		if (devices == nullptr || devices->empty())
			return {cmd.clientFd, "No devices found"};

		Result ret = {cmd.clientFd, ""};

		for (const auto &device: *devices | std::views::keys)
			ret.message += device + ", ";

		ret.message.pop_back(); // remove ','
		ret.message.pop_back(); // remove space

		return ret;
	}

	return {cmd.clientFd, "Arg is invalid or unsupported now"};
}

/**
 * @brief Handles Wake-on-LAN commands.
 *
 * Looks up the requested device in the configured device list and sends
 * a Wake-on-LAN magic packet to the device's MAC address.
 *
 * The command expects a device name as its argument:
 *
 *   Wake <DeviceName>
 *
 * @param cmd Wake command containing the target device name.
 * @return Result containing the operation status and response message.
 */
Result WakeOnLanManager::handleWake(const Command& cmd)
{
	if (cmd.args.empty())
		return {cmd.clientFd, "Invalid command call. usage WakeOnLan Wake <DeviceName>"};

	const auto devices = m_config.getConfig("Devices");
	if (devices == nullptr)
		return {cmd.clientFd, "No devices found"};

	const auto it = devices->find(cmd.args[0]);
	if (it == devices->end())
		return {cmd.clientFd, "Device " + cmd.args[0] + " Does not exist"};

	if (!sendWakePacket(it->second))
		return {cmd.clientFd, "Failed to send WoL packet"};

	return {cmd.clientFd, "Sent WoL packet to " + it->first};
}

/**
 * @brief Sends a Wake-on-LAN magic packet to a device.
 *
 * Retrieves the device MAC address, creates the WoL packet format:
 *
 *   6 bytes of 0xFF followed by the MAC address repeated 16 times.
 *
 * The packet is sent through UDP broadcast on port 9.
 *
 * @param macAddress Target device MAC address.
 * @return True if the packet was successfully sent, false otherwise.
 */
bool WakeOnLanManager::sendWakePacket(const std::string& macAddress) const
{
	constexpr uint16_t WOL_PACKET_LEN = 102;
	constexpr uint16_t WOL_PORT = 9;

	std::array<uint8_t, WOL_PACKET_LEN> packet{};
	std::fill_n(packet.begin(), MAC_SIZE, 0xFF);

	std::array<uint8_t, MAC_SIZE> mac;
	try
	{
		mac = parseMac(macAddress);
	}
	catch ([[maybe_unused]] const std::exception& e)
	{
		return false;
	}

	for (size_t offset = MAC_SIZE; offset < packet.size(); offset += mac.size())
		std::copy(mac.begin(), mac.end(), packet.begin() + offset);


	const int sock = socket(AF_INET, SOCK_DGRAM, 0);
	if (sock < 0)
		return false;

	constexpr int enable = 1;
	if (setsockopt(sock, SOL_SOCKET, SO_BROADCAST, &enable, sizeof(enable)) < 0)
	{
		close(sock);
		return false;
	}

	sockaddr_in addr{};
	addr.sin_family = AF_INET;
	addr.sin_port = htons(WOL_PORT);

	const auto broadcast = getBroadcastAddress();
	if (broadcast.empty() || inet_pton(AF_INET, broadcast.c_str(), &addr.sin_addr) <= 0)
	{
		close(sock);
		return false;
	}

	const bool success = sendto(sock, packet.data(), packet.size(), 0,
								reinterpret_cast<sockaddr*>(&addr), sizeof(addr)) == packet.size();

	close(sock);
	return success;
}

/**
 * @brief Converts a MAC address string into raw bytes.
 *
 * Converts a MAC address in the format:
 *
 *   AA:BB:CC:DD:EE:FF
 *
 * into a 6-byte representation suitable for network packets.
 *
 * @param mac MAC address string.
 * @return Array containing the 6 MAC bytes.
 *
 * @throws std::invalid_argument if the MAC format is invalid.
 */
std::array<uint8_t, MAC_SIZE> WakeOnLanManager::parseMac(const std::string& mac)
{
	constexpr uint16_t MAC_SIZE_ASCII = 17;
	if (mac.size() != MAC_SIZE_ASCII)
		throw std::invalid_argument("Invalid MAC");

	std::array<uint8_t, MAC_SIZE> result{};

	for (int i = 0; i < MAC_SIZE; i++)
	{
		result[i] = static_cast<uint8_t>(
			std::stoi(mac.substr(i * 3, 2), nullptr, 16)
		);
	}

	return result;
}

/**
 * @brief Finds the current IPv4 broadcast address for a usable interface.
 *
 * The broadcast address is intentionally not cached.
 *
 * Network interfaces are not guaranteed to stay the same:
 * - DHCP can change the assigned network/subnet.
 * - Interfaces can go down or become unavailable.
 * - Interface names may change after hardware or OS changes.
 *
 * This function is only called when sending a WoL packet, which is an
 * infrequent operation, so recalculating the broadcast address has
 * negligible overhead compared to the benefit of always using current
 * network information.
 *
 * If a preferred interface was configured, it is used when available.
 * Otherwise, the first valid active non-loopback interface is selected.
 *
 * @return IPv4 broadcast address as a string, or an empty string on failure.
 */
std::string WakeOnLanManager::getBroadcastAddress() const
{
	ifaddrs* interfaces = nullptr;

	if (getifaddrs(&interfaces) == -1)
		return {};

	std::string fallback;

	for (const auto* iface = interfaces; iface; iface = iface->ifa_next)
	{
		if (!iface->ifa_addr || !iface->ifa_broadaddr)
			continue;

		if (iface->ifa_addr->sa_family != AF_INET)
			continue;

		if (iface->ifa_flags & IFF_LOOPBACK || !(iface->ifa_flags & IFF_UP))
			continue;

		const auto* broadcast = reinterpret_cast<sockaddr_in*>(iface->ifa_broadaddr);

		char buffer[INET_ADDRSTRLEN]{};
		inet_ntop(AF_INET, &broadcast->sin_addr, buffer, sizeof(buffer));

		// Preferred interface
		if (!m_interface.empty() && m_interface == iface->ifa_name)
		{
			fallback = buffer;
			break;
		}

		// Save first valid interface as fallback
		if (fallback.empty())
			fallback = buffer;
	}

	freeifaddrs(interfaces);

	return fallback;
}
