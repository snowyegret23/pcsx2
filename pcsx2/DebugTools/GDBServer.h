// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DebugServer.h"

#include <optional>
#include <string>
#include <string_view>

class GDBServer final : public DebugServerInterface
{
public:
	explicit GDBServer(DebugInterface* debugInterface);
	~GDBServer() override = default;

	void onClientConnected() override;
	void onClientDisconnected() override;
	bool replyPacket(void* outData, std::size_t& outSize) override;
	std::size_t processPacket(const char* inData, std::size_t inSize, void* outData, std::size_t& outSize) override;

private:
	bool writePacketBegin();
	bool writePacketEnd();
	bool writePacketData(std::string_view data);
	bool writePacketByte(char value);
	bool writeResponse(std::string_view data);
	bool writeError(std::string_view code);
	bool writeStopReply(u8 signal);
	bool writePaged(std::size_t offset, std::size_t length, std::string_view data);

	bool processQueryPacket(std::string_view data);
	bool processSetPacket(std::string_view data);
	bool processThreadPacket(std::string_view data);
	bool processVPacket(std::string_view data);
	bool processBreakpointPacket(std::string_view data, bool insert);
	bool processMemoryReadPacket(std::string_view data);
	bool processMemoryWritePacket(std::string_view data);
	bool processBinaryMemoryWritePacket(std::string_view data);
	bool processReadRegisterPacket(std::string_view data);
	bool processWriteRegisterPacket(std::string_view data);
	bool processPcsx2CommandPacket(std::string_view data);

	bool readRegister(u32 id, u32* value);
	bool writeRegister(u32 id, u32 value);
	bool writeRegisterValue(u32 id);
	bool writeAllRegisters();

	void pauseExecution();
	void resumeExecution(std::optional<u32> address);
	void singleStep(std::optional<u32> address);

	std::string runPcsx2Command(std::string_view command);
	std::string getStatusString() const;

	void* m_outData = nullptr;
	std::size_t* m_outSize = nullptr;
	std::size_t m_packetPayloadOffset = 0;
	bool m_noAckMode = false;
};
