// Created by yakov on 07/08/2026.

#pragma once
#include <cstdint>

enum Control : uint8_t
{
	AUTH = 0,
	COMMAND = 1,
	STREAM = 2,
	CONFIG = 3,
	Error = 4,
};

struct PacketHeader
{
	Control control;
	uint16_t len;
} __attribute__((packed));

struct CommandPacket
{
	PacketHeader header{COMMAND, 0};
	char msg[];
} __attribute__((packed));

struct StreamPacket
{
	PacketHeader header{STREAM, 0};
	uint8_t data[];
} __attribute__((packed));

struct ConfigurationPacket
{
	PacketHeader header{CONFIG, sizeof(ConfigurationPacket) - sizeof(PacketHeader)};

	uint32_t maxInputChunk = 0;
	uint32_t maxOutputChunk = 0;

} __attribute__((packed));

constexpr uint16_t CONTROL_TO_HEADER_SIZE[] = {sizeof(PacketHeader), sizeof(CommandPacket), sizeof(StreamPacket), sizeof(PacketHeader)};
