// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "GDBServer.h"

#include "Breakpoints.h"
#include "Host.h"
#include "MIPSAnalyst.h"
#include "Patch.h"
#include "VMManager.h"

#include "common/Console.h"
#include "common/Error.h"
#include "common/StringUtil.h"
#include "common/Threading.h"

#include "fmt/format.h"

#include <algorithm>
#include <charconv>
#include <cctype>
#include <cstring>
#include <mutex>
#include <sstream>
#include <vector>

static std::mutex s_cpu_transaction_mutex;

static constexpr std::string_view GDB_FEATURES =
	"PacketSize=47ff"
	";QStartNoAckMode+"
	";qXfer:features:read+"
	";qXfer:threads:read+"
	";qXfer:memory-map:read+"
	";swbreak+"
	";hwbreak+"
	";vContSupported+"
	";multiprocess-";

static constexpr std::string_view TARGET_EE_XML = R"(<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
  <architecture>mips:5900</architecture>
  <feature name="org.gnu.gdb.mips.cpu">
    <reg name="r0" bitsize="32" regnum="0"/>
    <reg name="r1" bitsize="32"/><reg name="r2" bitsize="32"/><reg name="r3" bitsize="32"/>
    <reg name="r4" bitsize="32"/><reg name="r5" bitsize="32"/><reg name="r6" bitsize="32"/><reg name="r7" bitsize="32"/>
    <reg name="r8" bitsize="32"/><reg name="r9" bitsize="32"/><reg name="r10" bitsize="32"/><reg name="r11" bitsize="32"/>
    <reg name="r12" bitsize="32"/><reg name="r13" bitsize="32"/><reg name="r14" bitsize="32"/><reg name="r15" bitsize="32"/>
    <reg name="r16" bitsize="32"/><reg name="r17" bitsize="32"/><reg name="r18" bitsize="32"/><reg name="r19" bitsize="32"/>
    <reg name="r20" bitsize="32"/><reg name="r21" bitsize="32"/><reg name="r22" bitsize="32"/><reg name="r23" bitsize="32"/>
    <reg name="r24" bitsize="32"/><reg name="r25" bitsize="32"/><reg name="r26" bitsize="32"/><reg name="r27" bitsize="32"/>
    <reg name="r28" bitsize="32"/><reg name="r29" bitsize="32"/><reg name="r30" bitsize="32"/><reg name="r31" bitsize="32"/>
    <reg name="status" bitsize="32" regnum="32"/>
    <reg name="lo" bitsize="32" regnum="33"/>
    <reg name="hi" bitsize="32" regnum="34"/>
    <reg name="badvaddr" bitsize="32" regnum="35"/>
    <reg name="cause" bitsize="32" regnum="36"/>
    <reg name="pc" bitsize="32" regnum="37"/>
  </feature>
  <feature name="org.gnu.gdb.mips.fpu">
    <reg name="f0" bitsize="32" type="ieee_single" regnum="38"/><reg name="f1" bitsize="32" type="ieee_single"/>
    <reg name="f2" bitsize="32" type="ieee_single"/><reg name="f3" bitsize="32" type="ieee_single"/>
    <reg name="f4" bitsize="32" type="ieee_single"/><reg name="f5" bitsize="32" type="ieee_single"/>
    <reg name="f6" bitsize="32" type="ieee_single"/><reg name="f7" bitsize="32" type="ieee_single"/>
    <reg name="f8" bitsize="32" type="ieee_single"/><reg name="f9" bitsize="32" type="ieee_single"/>
    <reg name="f10" bitsize="32" type="ieee_single"/><reg name="f11" bitsize="32" type="ieee_single"/>
    <reg name="f12" bitsize="32" type="ieee_single"/><reg name="f13" bitsize="32" type="ieee_single"/>
    <reg name="f14" bitsize="32" type="ieee_single"/><reg name="f15" bitsize="32" type="ieee_single"/>
    <reg name="f16" bitsize="32" type="ieee_single"/><reg name="f17" bitsize="32" type="ieee_single"/>
    <reg name="f18" bitsize="32" type="ieee_single"/><reg name="f19" bitsize="32" type="ieee_single"/>
    <reg name="f20" bitsize="32" type="ieee_single"/><reg name="f21" bitsize="32" type="ieee_single"/>
    <reg name="f22" bitsize="32" type="ieee_single"/><reg name="f23" bitsize="32" type="ieee_single"/>
    <reg name="f24" bitsize="32" type="ieee_single"/><reg name="f25" bitsize="32" type="ieee_single"/>
    <reg name="f26" bitsize="32" type="ieee_single"/><reg name="f27" bitsize="32" type="ieee_single"/>
    <reg name="f28" bitsize="32" type="ieee_single"/><reg name="f29" bitsize="32" type="ieee_single"/>
    <reg name="f30" bitsize="32" type="ieee_single"/><reg name="f31" bitsize="32" type="ieee_single"/>
    <reg name="fcsr" bitsize="32"/><reg name="fir" bitsize="32"/>
  </feature>
</target>
)";

static constexpr std::string_view TARGET_IOP_XML = R"(<?xml version="1.0"?>
<!DOCTYPE target SYSTEM "gdb-target.dtd">
<target version="1.0">
  <architecture>mips:3000</architecture>
  <feature name="org.gnu.gdb.mips.cpu">
    <reg name="r0" bitsize="32" regnum="0"/>
    <reg name="r1" bitsize="32"/><reg name="r2" bitsize="32"/><reg name="r3" bitsize="32"/>
    <reg name="r4" bitsize="32"/><reg name="r5" bitsize="32"/><reg name="r6" bitsize="32"/><reg name="r7" bitsize="32"/>
    <reg name="r8" bitsize="32"/><reg name="r9" bitsize="32"/><reg name="r10" bitsize="32"/><reg name="r11" bitsize="32"/>
    <reg name="r12" bitsize="32"/><reg name="r13" bitsize="32"/><reg name="r14" bitsize="32"/><reg name="r15" bitsize="32"/>
    <reg name="r16" bitsize="32"/><reg name="r17" bitsize="32"/><reg name="r18" bitsize="32"/><reg name="r19" bitsize="32"/>
    <reg name="r20" bitsize="32"/><reg name="r21" bitsize="32"/><reg name="r22" bitsize="32"/><reg name="r23" bitsize="32"/>
    <reg name="r24" bitsize="32"/><reg name="r25" bitsize="32"/><reg name="r26" bitsize="32"/><reg name="r27" bitsize="32"/>
    <reg name="r28" bitsize="32"/><reg name="r29" bitsize="32"/><reg name="r30" bitsize="32"/><reg name="r31" bitsize="32"/>
    <reg name="status" bitsize="32" regnum="32"/>
    <reg name="lo" bitsize="32" regnum="33"/>
    <reg name="hi" bitsize="32" regnum="34"/>
    <reg name="badvaddr" bitsize="32" regnum="35"/>
    <reg name="cause" bitsize="32" regnum="36"/>
    <reg name="pc" bitsize="32" regnum="37"/>
  </feature>
  <feature name="org.gnu.gdb.mips.fpu">
    <reg name="f0" bitsize="32" type="ieee_single" regnum="38"/><reg name="f1" bitsize="32" type="ieee_single"/>
    <reg name="f2" bitsize="32" type="ieee_single"/><reg name="f3" bitsize="32" type="ieee_single"/>
    <reg name="f4" bitsize="32" type="ieee_single"/><reg name="f5" bitsize="32" type="ieee_single"/>
    <reg name="f6" bitsize="32" type="ieee_single"/><reg name="f7" bitsize="32" type="ieee_single"/>
    <reg name="f8" bitsize="32" type="ieee_single"/><reg name="f9" bitsize="32" type="ieee_single"/>
    <reg name="f10" bitsize="32" type="ieee_single"/><reg name="f11" bitsize="32" type="ieee_single"/>
    <reg name="f12" bitsize="32" type="ieee_single"/><reg name="f13" bitsize="32" type="ieee_single"/>
    <reg name="f14" bitsize="32" type="ieee_single"/><reg name="f15" bitsize="32" type="ieee_single"/>
    <reg name="f16" bitsize="32" type="ieee_single"/><reg name="f17" bitsize="32" type="ieee_single"/>
    <reg name="f18" bitsize="32" type="ieee_single"/><reg name="f19" bitsize="32" type="ieee_single"/>
    <reg name="f20" bitsize="32" type="ieee_single"/><reg name="f21" bitsize="32" type="ieee_single"/>
    <reg name="f22" bitsize="32" type="ieee_single"/><reg name="f23" bitsize="32" type="ieee_single"/>
    <reg name="f24" bitsize="32" type="ieee_single"/><reg name="f25" bitsize="32" type="ieee_single"/>
    <reg name="f26" bitsize="32" type="ieee_single"/><reg name="f27" bitsize="32" type="ieee_single"/>
    <reg name="f28" bitsize="32" type="ieee_single"/><reg name="f29" bitsize="32" type="ieee_single"/>
    <reg name="f30" bitsize="32" type="ieee_single"/><reg name="f31" bitsize="32" type="ieee_single"/>
    <reg name="fcsr" bitsize="32"/><reg name="fir" bitsize="32"/>
  </feature>
</target>
)";

static constexpr std::string_view EE_MEMORY_MAP = R"(<?xml version="1.0"?>
<memory-map>
  <memory type="ram" start="0x00000000" length="0x02000000"/>
  <memory type="ram" start="0x10000000" length="0x00010000"/>
  <memory type="ram" start="0x11000000" length="0x00010000"/>
  <memory type="ram" start="0x12000000" length="0x00002000"/>
  <memory type="ram" start="0x1c000000" length="0x00200000"/>
  <memory type="rom" start="0x1fc00000" length="0x00400000"/>
  <memory type="ram" start="0x70000000" length="0x00004000"/>
</memory-map>
)";

static constexpr std::string_view IOP_MEMORY_MAP = R"(<?xml version="1.0"?>
<memory-map>
  <memory type="ram" start="0x00000000" length="0x00200000"/>
  <memory type="ram" start="0x1d000000" length="0x00800000"/>
  <memory type="ram" start="0x1f800000" length="0x00010000"/>
  <memory type="ram" start="0x1f900000" length="0x00000400"/>
  <memory type="rom" start="0x1fc00000" length="0x00400000"/>
</memory-map>
)";

static constexpr int GDB_REGISTER_COUNT = 72;

static char hex_digit(u8 value)
{
	static constexpr char digits[] = "0123456789abcdef";
	return digits[value & 0xf];
}

static std::optional<u8> from_hex(char ch)
{
	if (ch >= '0' && ch <= '9')
		return static_cast<u8>(ch - '0');
	if (ch >= 'a' && ch <= 'f')
		return static_cast<u8>(ch - 'a' + 10);
	if (ch >= 'A' && ch <= 'F')
		return static_cast<u8>(ch - 'A' + 10);
	return std::nullopt;
}

static u8 checksum(std::string_view data)
{
	u8 ret = 0;
	for (char ch : data)
		ret += static_cast<u8>(ch);
	return ret;
}

static bool parse_hex_u32(std::string_view text, u32* value)
{
	if (text.empty())
		return false;

	u32 out = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), out, 16);
	if (result.ec != std::errc())
		return false;

	*value = out;
	return true;
}

static bool starts_with(std::string_view text, std::string_view prefix)
{
	return text.size() >= prefix.size() && text.substr(0, prefix.size()) == prefix;
}

static bool parse_hex_size(std::string_view text, std::size_t* value)
{
	if (text.empty())
		return false;

	std::size_t out = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), out, 16);
	if (result.ec != std::errc())
		return false;

	*value = out;
	return true;
}

static std::string encode_hex(std::string_view text)
{
	std::string out;
	out.reserve(text.size() * 2);
	for (u8 ch : text)
	{
		out.push_back(hex_digit(ch >> 4));
		out.push_back(hex_digit(ch));
	}
	return out;
}

static std::optional<std::string> decode_hex_string(std::string_view text)
{
	if ((text.size() & 1) != 0)
		return std::nullopt;

	std::string out;
	out.resize(text.size() / 2);
	for (std::size_t i = 0; i < out.size(); i++)
	{
		const std::optional<u8> hi = from_hex(text[i * 2]);
		const std::optional<u8> lo = from_hex(text[i * 2 + 1]);
		if (!hi.has_value() || !lo.has_value())
			return std::nullopt;
		out[i] = static_cast<char>((*hi << 4) | *lo);
	}
	return out;
}

static std::optional<std::vector<u8>> decode_hex_bytes(std::string_view text)
{
	if ((text.size() & 1) != 0)
		return std::nullopt;

	std::vector<u8> out(text.size() / 2);
	for (std::size_t i = 0; i < out.size(); i++)
	{
		const std::optional<u8> hi = from_hex(text[i * 2]);
		const std::optional<u8> lo = from_hex(text[i * 2 + 1]);
		if (!hi.has_value() || !lo.has_value())
			return std::nullopt;
		out[i] = static_cast<u8>((*hi << 4) | *lo);
	}
	return out;
}

static std::vector<u8> decode_binary_memory(std::string_view text)
{
	std::vector<u8> out;
	out.reserve(text.size());
	for (std::size_t i = 0; i < text.size(); i++)
	{
		u8 value = static_cast<u8>(text[i]);
		if (value == '}')
		{
			if (++i >= text.size())
				break;
			value = static_cast<u8>(text[i]) ^ 0x20;
		}
		out.push_back(value);
	}
	return out;
}

static void append_le_hex32(std::string& out, u32 value)
{
	for (int i = 0; i < 4; i++)
	{
		const u8 byte = static_cast<u8>((value >> (i * 8)) & 0xff);
		out.push_back(hex_digit(byte >> 4));
		out.push_back(hex_digit(byte));
	}
}

static std::optional<u32> read_le_hex32(std::string_view text)
{
	if (text.size() < 8)
		return std::nullopt;

	u32 out = 0;
	for (int i = 0; i < 4; i++)
	{
		const std::optional<u8> hi = from_hex(text[i * 2]);
		const std::optional<u8> lo = from_hex(text[i * 2 + 1]);
		if (!hi.has_value() || !lo.has_value())
			return std::nullopt;
		out |= static_cast<u32>((*hi << 4) | *lo) << (i * 8);
	}
	return out;
}

static std::string state_to_string(VMState state)
{
	switch (state)
	{
		case VMState::Running:
			return "running";
		case VMState::Paused:
			return "paused";
		case VMState::Shutdown:
			return "shutdown";
		case VMState::Initializing:
			return "initializing";
		case VMState::Resetting:
			return "resetting";
		case VMState::Stopping:
			return "stopping";
		default:
			return "unknown";
	}
}

static std::string trim(std::string_view text)
{
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.front())))
		text.remove_prefix(1);
	while (!text.empty() && std::isspace(static_cast<unsigned char>(text.back())))
		text.remove_suffix(1);
	return std::string(text);
}

static std::pair<std::string, std::string> split_command(std::string_view command)
{
	const std::size_t space = command.find_first_of(" \t\r\n");
	if (space == std::string_view::npos)
		return {std::string(command), {}};

	return {std::string(command.substr(0, space)), trim(command.substr(space + 1))};
}

static bool parse_slot(std::string_view text, s32* slot)
{
	int out = 0;
	const auto result = std::from_chars(text.data(), text.data() + text.size(), out, 10);
	if (result.ec != std::errc() || out < 0 || out >= VMManager::NUM_SAVE_STATE_SLOTS)
		return false;
	*slot = static_cast<s32>(out);
	return true;
}

GDBServer::GDBServer(DebugInterface* debugInterface)
{
	m_debugInterface = debugInterface;
}

void GDBServer::onClientConnected()
{
	pauseExecution();
}

void GDBServer::onClientDisconnected()
{
	m_noAckMode = false;
}

bool GDBServer::writePacketBegin()
{
	std::size_t& outSize = *m_outSize;
	char* out = static_cast<char*>(m_outData);

	if (outSize + 2 >= MAX_DEBUG_PACKET_SIZE)
		return false;

	if (!m_noAckMode)
		out[outSize++] = '+';

	out[outSize++] = '$';
	m_packetPayloadOffset = outSize;
	return true;
}

bool GDBServer::writePacketEnd()
{
	std::size_t& outSize = *m_outSize;
	char* out = static_cast<char*>(m_outData);

	if (outSize + 3 >= MAX_DEBUG_PACKET_SIZE)
		return false;

	const std::string_view payload(out + m_packetPayloadOffset, outSize - m_packetPayloadOffset);
	const u8 sum = checksum(payload);
	out[outSize++] = '#';
	out[outSize++] = hex_digit(sum >> 4);
	out[outSize++] = hex_digit(sum);
	return true;
}

bool GDBServer::writePacketData(std::string_view data)
{
	std::size_t& outSize = *m_outSize;
	if (outSize + data.size() + 3 >= MAX_DEBUG_PACKET_SIZE)
		return false;

	std::memcpy(static_cast<char*>(m_outData) + outSize, data.data(), data.size());
	outSize += data.size();
	return true;
}

bool GDBServer::writePacketByte(char value)
{
	std::size_t& outSize = *m_outSize;
	if (outSize + 4 >= MAX_DEBUG_PACKET_SIZE)
		return false;

	static_cast<char*>(m_outData)[outSize++] = value;
	return true;
}

bool GDBServer::writeResponse(std::string_view data)
{
	return writePacketBegin() && writePacketData(data) && writePacketEnd();
}

bool GDBServer::writeError(std::string_view code)
{
	return writeResponse(code);
}

bool GDBServer::writeStopReply(u8 signal)
{
	return writeResponse(fmt::format("T{:02x}thread:1;", signal));
}

bool GDBServer::writePaged(std::size_t offset, std::size_t length, std::string_view data)
{
	if (offset >= data.size())
		return writeResponse("l");

	const std::size_t remaining = data.size() - offset;
	const std::size_t take = std::min(remaining, length);
	const char prefix = (take < remaining) ? 'm' : 'l';

	return writePacketBegin() &&
		   writePacketByte(prefix) &&
		   writePacketData(data.substr(offset, take)) &&
		   writePacketEnd();
}

void GDBServer::pauseExecution()
{
	std::lock_guard lock(s_cpu_transaction_mutex);
	if (m_debugInterface && m_debugInterface->isAlive() && !m_debugInterface->isCpuPaused())
		m_debugInterface->pauseCpu();
}

void GDBServer::resumeExecution(std::optional<u32> address)
{
	std::lock_guard lock(s_cpu_transaction_mutex);
	if (!m_debugInterface || !m_debugInterface->isAlive())
		return;

	if (address.has_value())
		m_debugInterface->setPc(*address);

	if (m_debugInterface->isCpuPaused())
		m_debugInterface->resumeCpu();
}

void GDBServer::singleStep(std::optional<u32> address)
{
	std::lock_guard lock(s_cpu_transaction_mutex);
	if (!m_debugInterface || !m_debugInterface->isAlive())
		return;

	if (!m_debugInterface->isCpuPaused())
		m_debugInterface->pauseCpu();

	while (!m_debugInterface->isCpuPaused())
		Threading::Sleep(1);

	if (address.has_value())
		m_debugInterface->setPc(*address);

	CBreakPoints::SetSkipFirst(m_debugInterface->getCpuType(), m_debugInterface->getPC());

	const u32 pc = m_debugInterface->getPC();
	const MIPSAnalyst::MipsOpcodeInfo info = MIPSAnalyst::GetOpcodeInfo(m_debugInterface, pc);
	u32 breakAddress = pc + 4;
	if (info.isBranch)
		breakAddress = info.isConditional ? (info.conditionMet ? info.branchTarget : pc + 8) : info.branchTarget;
	if (info.isSyscall)
		breakAddress = info.branchTarget;

	CBreakPoints::AddBreakPoint(m_debugInterface->getCpuType(), breakAddress, true, true, true);
	m_debugInterface->resumeCpu();
}

bool GDBServer::replyPacket(void* outData, std::size_t& outSize)
{
	outSize = 0;
	if (!CBreakPoints::GetBreakpointTriggered() || !m_debugInterface)
		return true;

	const BreakPointCpu triggered_cpu = CBreakPoints::GetBreakpointTriggeredCpu();
	if (triggered_cpu != m_debugInterface->getCpuType() && triggered_cpu != BREAKPOINT_IOP_AND_EE)
		return true;

	CBreakPoints::ClearTemporaryBreakPoints();
	CBreakPoints::SetBreakpointTriggered(false, triggered_cpu);
	CBreakPoints::SetSkipFirst(m_debugInterface->getCpuType(), m_debugInterface->getPC());

	m_outData = outData;
	m_outSize = &outSize;
	return writeStopReply(5);
}

std::size_t GDBServer::processPacket(const char* inData, std::size_t inSize, void* outData, std::size_t& outSize)
{
	outSize = 0;
	if (inSize == 0)
		return 0;

	m_outData = outData;
	m_outSize = &outSize;

	if (inData[0] == '+' || inData[0] == '-')
		return 1;

	if (inData[0] == '\x03')
	{
		pauseExecution();
		return writeStopReply(2) ? 1 : static_cast<std::size_t>(-1);
	}

	std::size_t start = 0;
	while (start < inSize && inData[start] != '$')
		start++;
	if (start >= inSize)
		return inSize;

	const char* hash = static_cast<const char*>(std::memchr(inData + start + 1, '#', inSize - start - 1));
	if (!hash)
		return 0;

	const std::size_t hashOffset = static_cast<std::size_t>(hash - inData);
	if (hashOffset + 2 >= inSize)
		return 0;

	const std::string_view data(inData + start + 1, hashOffset - start - 1);
	const std::optional<u8> hi = from_hex(inData[hashOffset + 1]);
	const std::optional<u8> lo = from_hex(inData[hashOffset + 2]);
	if (!hi.has_value() || !lo.has_value() || static_cast<u8>((*hi << 4) | *lo) != checksum(data))
	{
		static_cast<char*>(m_outData)[outSize++] = '-';
		return hashOffset + 3;
	}

	bool success = false;
	if (data.empty())
		success = writeResponse("");
	else
	{
		switch (data[0])
		{
			case '!':
				success = writeResponse("OK");
				break;
			case '?':
				success = writeStopReply((m_debugInterface && m_debugInterface->isCpuPaused()) ? 5 : 0);
				break;
			case 'q':
				success = processQueryPacket(data);
				break;
			case 'Q':
				success = processSetPacket(data);
				break;
			case 'H':
				success = processThreadPacket(data);
				break;
			case 'T':
				success = writeResponse("OK");
				break;
			case 'v':
				success = processVPacket(data);
				break;
			case 'g':
				success = writePacketBegin() && writeAllRegisters() && writePacketEnd();
				break;
			case 'G':
			{
				success = true;
				for (int reg = 0; reg < GDB_REGISTER_COUNT; reg++)
				{
					const std::string_view chunk = data.substr(1 + reg * 8, 8);
					if (chunk.size() != 8)
					{
						success = false;
						break;
					}
					const std::optional<u32> value = read_le_hex32(chunk);
					if (!value.has_value() || !writeRegister(reg, *value))
					{
						success = false;
						break;
					}
				}
				success = success ? writeResponse("OK") : writeError("E01");
				break;
			}
			case 'p':
				success = processReadRegisterPacket(data);
				break;
			case 'P':
				success = processWriteRegisterPacket(data);
				break;
			case 'm':
				success = processMemoryReadPacket(data);
				break;
			case 'M':
				success = processMemoryWritePacket(data);
				break;
			case 'X':
				success = processBinaryMemoryWritePacket(data);
				break;
			case 'Z':
				success = processBreakpointPacket(data, true);
				break;
			case 'z':
				success = processBreakpointPacket(data, false);
				break;
			case 'C':
			case 'c':
			{
				std::optional<u32> address;
				std::string_view addr_text;
				if (data[0] == 'c')
					addr_text = data.substr(1);
				else if (const std::size_t semicolon = data.find(';'); semicolon != std::string_view::npos)
					addr_text = data.substr(semicolon + 1);
				if (!addr_text.empty())
				{
					u32 parsed = 0;
					if (parse_hex_u32(addr_text, &parsed))
						address = parsed;
				}
				resumeExecution(address);
				success = true;
				break;
			}
			case 'S':
			case 's':
			{
				std::optional<u32> address;
				std::string_view addr_text;
				if (data[0] == 's')
					addr_text = data.substr(1);
				else if (const std::size_t semicolon = data.find(';'); semicolon != std::string_view::npos)
					addr_text = data.substr(semicolon + 1);
				if (!addr_text.empty())
				{
					u32 parsed = 0;
					if (parse_hex_u32(addr_text, &parsed))
						address = parsed;
				}
				singleStep(address);
				success = true;
				break;
			}
			case 'D':
			case 'k':
				pauseExecution();
				success = writeResponse("OK");
				break;
			default:
				success = writeResponse("");
				break;
		}
	}

	if (!success)
	{
		outSize = 0;
		writeError("E00");
	}

	return hashOffset + 3;
}

bool GDBServer::processQueryPacket(std::string_view data)
{
	if (data == "qSupported" || starts_with(data, "qSupported:"))
		return writeResponse(GDB_FEATURES);

	if (data == "qAttached")
		return writeResponse("1");

	if (data == "qC")
		return writeResponse("QC1");

	if (data == "qfThreadInfo")
	{
		std::string response = "m1";
		if (m_debugInterface && m_debugInterface->isAlive())
		{
			const std::vector<std::unique_ptr<BiosThread>> threads = m_debugInterface->GetThreadList();
			for (const auto& thread : threads)
				response += fmt::format(",{:x}", thread->TID() + 1);
		}
		return writeResponse(response);
	}

	if (data == "qsThreadInfo")
		return writeResponse("l");

	if (starts_with(data, "qThreadExtraInfo"))
		return writeResponse(encode_hex(m_debugInterface ? m_debugInterface->longCpuName(m_debugInterface->getCpuType()) : "PCSX2"));

	if (data == "qTStatus")
		return writeResponse(m_debugInterface && m_debugInterface->isAlive() ? "T1" : "T0");

	if (starts_with(data, "qXfer:"))
	{
		const std::size_t read_pos = data.find(":read:");
		if (read_pos == std::string_view::npos)
			return writeResponse("");

		const std::string_view object = data.substr(6, read_pos - 6);
		const std::string_view rest = data.substr(read_pos + 6);
		const std::size_t colon = rest.rfind(':');
		const std::size_t comma = rest.rfind(',');
		if (colon == std::string_view::npos || comma == std::string_view::npos || comma < colon)
			return writeError("E01");

		std::size_t offset = 0;
		std::size_t length = 0;
		if (!parse_hex_size(rest.substr(colon + 1, comma - colon - 1), &offset) ||
			!parse_hex_size(rest.substr(comma + 1), &length))
		{
			return writeError("E01");
		}

		if (object == "features")
			return writePaged(offset, length, (m_debugInterface && m_debugInterface->getCpuType() == BREAKPOINT_IOP) ? TARGET_IOP_XML : TARGET_EE_XML);
		if (object == "memory-map")
			return writePaged(offset, length, (m_debugInterface && m_debugInterface->getCpuType() == BREAKPOINT_IOP) ? IOP_MEMORY_MAP : EE_MEMORY_MAP);
		if (object == "threads")
		{
			std::string threads = "<?xml version=\"1.0\"?><threads><thread id=\"1\" name=\"current\"/>";
			if (m_debugInterface && m_debugInterface->isAlive())
			{
				const std::vector<std::unique_ptr<BiosThread>> list = m_debugInterface->GetThreadList();
				for (const auto& thread : list)
					threads += fmt::format("<thread id=\"{:x}\" core=\"{}\" name=\"tid {}\"/>", thread->TID() + 1, m_debugInterface->cpuName(m_debugInterface->getCpuType()), thread->TID());
			}
			threads += "</threads>";
			return writePaged(offset, length, threads);
		}

		return writeResponse("");
	}

	if (starts_with(data, "qPcsx2:"))
		return processPcsx2CommandPacket(data.substr(7));

	if (starts_with(data, "qSymbol:"))
		return writeResponse("OK");

	return writeResponse("");
}

bool GDBServer::processSetPacket(std::string_view data)
{
	if (data == "QStartNoAckMode")
	{
		const bool old_no_ack = m_noAckMode;
		m_noAckMode = false;
		const bool ok = writeResponse("OK");
		m_noAckMode = old_no_ack || ok;
		return ok;
	}

	if (starts_with(data, "QThreadEvents:"))
		return writeResponse("OK");

	return writeResponse("");
}

bool GDBServer::processThreadPacket(std::string_view data)
{
	if (data.size() >= 2 && (data[1] == 'c' || data[1] == 'g'))
		return writeResponse("OK");

	return writeResponse("");
}

bool GDBServer::processVPacket(std::string_view data)
{
	if (data == "vMustReplyEmpty")
		return writeResponse("");

	if (data == "vCont?")
		return writeResponse("vCont;c;C;s;S");

	if (starts_with(data, "vCont;"))
	{
		if (data.find(";s") != std::string_view::npos || data.find(";S") != std::string_view::npos)
			singleStep(std::nullopt);
		else
			resumeExecution(std::nullopt);
		return true;
	}

	if (data == "vCtrlC")
	{
		pauseExecution();
		return writeStopReply(2);
	}

	return writeResponse("");
}

bool GDBServer::processBreakpointPacket(std::string_view data, bool insert)
{
	const std::size_t first_comma = data.find(',');
	const std::size_t second_comma = data.find(',', first_comma + 1);
	if (first_comma == std::string_view::npos || second_comma == std::string_view::npos)
		return writeError("E01");

	const int type = data[1] - '0';
	u32 address = 0;
	u32 kind = 4;
	if (!parse_hex_u32(data.substr(first_comma + 1, second_comma - first_comma - 1), &address) ||
		!parse_hex_u32(data.substr(second_comma + 1), &kind))
	{
		return writeError("E01");
	}

	if (!m_debugInterface || !m_debugInterface->isAlive())
		return writeError("E01");

	const BreakPointCpu cpu = m_debugInterface->getCpuType();
	{
		std::lock_guard lock(s_cpu_transaction_mutex);
		if (type == 0 || type == 1)
		{
			if (insert)
				CBreakPoints::AddBreakPoint(cpu, address);
			else
				CBreakPoints::RemoveBreakPoint(cpu, address);
		}
		else if (type >= 2 && type <= 4)
		{
			MemCheckCondition cond = MEMCHECK_READWRITE;
			if (type == 2)
				cond = MEMCHECK_WRITE;
			else if (type == 3)
				cond = MEMCHECK_READ;

			const u32 end = address + std::max<u32>(kind, 1);
			if (insert)
				CBreakPoints::AddMemCheck(cpu, address, end, cond, MEMCHECK_BREAK);
			else
				CBreakPoints::RemoveMemCheck(cpu, address, end);
		}
		else
		{
			return writeResponse("");
		}
	}

	return writeResponse("OK");
}

bool GDBServer::processMemoryReadPacket(std::string_view data)
{
	const std::size_t comma = data.find(',');
	if (comma == std::string_view::npos)
		return writeError("E01");

	u32 address = 0;
	std::size_t length = 0;
	if (!parse_hex_u32(data.substr(1, comma - 1), &address) || !parse_hex_size(data.substr(comma + 1), &length))
		return writeError("E01");

	if (!m_debugInterface || !m_debugInterface->isAlive())
		return writeError("E01");

	if (length * 2 + 8 >= MAX_DEBUG_PACKET_SIZE)
		return writeError("E02");

	std::vector<u8> bytes(length);
	if (length > 0 && !m_debugInterface->ReadBytes(address, bytes.data(), static_cast<u32>(bytes.size())))
		return writeError("E03");

	if (!writePacketBegin())
		return false;
	for (u8 byte : bytes)
	{
		if (!writePacketByte(hex_digit(byte >> 4)) || !writePacketByte(hex_digit(byte)))
			return false;
	}
	return writePacketEnd();
}

bool GDBServer::processMemoryWritePacket(std::string_view data)
{
	const std::size_t first_comma = data.find(',');
	const std::size_t colon = data.find(':', first_comma + 1);
	if (first_comma == std::string_view::npos || colon == std::string_view::npos)
		return writeError("E01");

	u32 address = 0;
	std::size_t length = 0;
	if (!parse_hex_u32(data.substr(1, first_comma - 1), &address) ||
		!parse_hex_size(data.substr(first_comma + 1, colon - first_comma - 1), &length))
	{
		return writeError("E01");
	}

	std::optional<std::vector<u8>> bytes = decode_hex_bytes(data.substr(colon + 1));
	if (!bytes.has_value() || bytes->size() != length)
		return writeError("E02");

	if (!m_debugInterface || !m_debugInterface->isAlive())
		return writeError("E01");

	if (length > 0 && !m_debugInterface->WriteBytes(address, bytes->data(), static_cast<u32>(bytes->size())))
		return writeError("E03");

	return writeResponse("OK");
}

bool GDBServer::processBinaryMemoryWritePacket(std::string_view data)
{
	const std::size_t first_comma = data.find(',');
	const std::size_t colon = data.find(':', first_comma + 1);
	if (first_comma == std::string_view::npos || colon == std::string_view::npos)
		return writeError("E01");

	u32 address = 0;
	std::size_t length = 0;
	if (!parse_hex_u32(data.substr(1, first_comma - 1), &address) ||
		!parse_hex_size(data.substr(first_comma + 1, colon - first_comma - 1), &length))
	{
		return writeError("E01");
	}

	std::vector<u8> bytes = decode_binary_memory(data.substr(colon + 1));
	if (bytes.size() != length)
		return writeError("E02");

	if (!m_debugInterface || !m_debugInterface->isAlive())
		return writeError("E01");

	if (length > 0 && !m_debugInterface->WriteBytes(address, bytes.data(), static_cast<u32>(bytes.size())))
		return writeError("E03");

	return writeResponse("OK");
}

bool GDBServer::processReadRegisterPacket(std::string_view data)
{
	u32 reg = 0;
	if (!parse_hex_u32(data.substr(1), &reg))
		return writeError("E01");

	return writePacketBegin() && writeRegisterValue(reg) && writePacketEnd();
}

bool GDBServer::processWriteRegisterPacket(std::string_view data)
{
	const std::size_t equals = data.find('=');
	if (equals == std::string_view::npos)
		return writeError("E01");

	u32 reg = 0;
	if (!parse_hex_u32(data.substr(1, equals - 1), &reg))
		return writeError("E01");

	const std::optional<u32> value = read_le_hex32(data.substr(equals + 1, 8));
	if (!value.has_value() || !writeRegister(reg, *value))
		return writeError("E02");

	return writeResponse("OK");
}

bool GDBServer::readRegister(u32 id, u32* value)
{
	if (!m_debugInterface || !m_debugInterface->isAlive())
		return false;

	const BreakPointCpu cpu = m_debugInterface->getCpuType();
	if (cpu == BREAKPOINT_EE)
	{
		if (id < 32)
		{
			*value = m_debugInterface->getRegister(EECAT_GPR, id)._u32[0];
			return true;
		}
		switch (id)
		{
			case 32:
				*value = m_debugInterface->getRegister(EECAT_CP0, 12)._u32[0];
				return true;
			case 33:
				*value = m_debugInterface->getRegister(EECAT_GPR, 34)._u32[0];
				return true;
			case 34:
				*value = m_debugInterface->getRegister(EECAT_GPR, 33)._u32[0];
				return true;
			case 35:
				*value = m_debugInterface->getRegister(EECAT_CP0, 8)._u32[0];
				return true;
			case 36:
				*value = m_debugInterface->getRegister(EECAT_CP0, 13)._u32[0];
				return true;
			case 37:
				*value = m_debugInterface->getPC();
				return true;
			default:
				break;
		}
		if (id >= 38 && id < 70)
		{
			*value = m_debugInterface->getRegister(EECAT_FPR, id - 38)._u32[0];
			return true;
		}
		if (id == 70)
		{
			*value = m_debugInterface->getRegister(EECAT_FCR, 31)._u32[0];
			return true;
		}
	}
	else
	{
		if (id < 32)
		{
			*value = m_debugInterface->getRegister(IOPCAT_GPR, id)._u32[0];
			return true;
		}
		switch (id)
		{
			case 33:
				*value = m_debugInterface->getRegister(IOPCAT_GPR, 34)._u32[0];
				return true;
			case 34:
				*value = m_debugInterface->getRegister(IOPCAT_GPR, 33)._u32[0];
				return true;
			case 37:
				*value = m_debugInterface->getPC();
				return true;
			default:
				break;
		}
	}

	*value = 0;
	return id < static_cast<u32>(GDB_REGISTER_COUNT);
}

bool GDBServer::writeRegister(u32 id, u32 value)
{
	if (!m_debugInterface || !m_debugInterface->isAlive())
		return false;

	if (id == 0)
		return true;

	const BreakPointCpu cpu = m_debugInterface->getCpuType();
	if (cpu == BREAKPOINT_EE)
	{
		if (id < 32)
		{
			m_debugInterface->setRegister(EECAT_GPR, id, u128::From32(value));
			return true;
		}
		switch (id)
		{
			case 32:
				m_debugInterface->setRegister(EECAT_CP0, 12, u128::From32(value));
				return true;
			case 33:
				m_debugInterface->setRegister(EECAT_GPR, 34, u128::From32(value));
				return true;
			case 34:
				m_debugInterface->setRegister(EECAT_GPR, 33, u128::From32(value));
				return true;
			case 35:
				m_debugInterface->setRegister(EECAT_CP0, 8, u128::From32(value));
				return true;
			case 36:
				m_debugInterface->setRegister(EECAT_CP0, 13, u128::From32(value));
				return true;
			case 37:
				m_debugInterface->setPc(value);
				return true;
			default:
				break;
		}
		if (id >= 38 && id < 70)
		{
			m_debugInterface->setRegister(EECAT_FPR, id - 38, u128::From32(value));
			return true;
		}
		if (id == 70)
		{
			m_debugInterface->setRegister(EECAT_FCR, 31, u128::From32(value));
			return true;
		}
	}
	else
	{
		if (id < 32)
		{
			m_debugInterface->setRegister(IOPCAT_GPR, id, u128::From32(value));
			return true;
		}
		switch (id)
		{
			case 33:
				m_debugInterface->setRegister(IOPCAT_GPR, 34, u128::From32(value));
				return true;
			case 34:
				m_debugInterface->setRegister(IOPCAT_GPR, 33, u128::From32(value));
				return true;
			case 37:
				m_debugInterface->setPc(value);
				return true;
			default:
				break;
		}
	}

	return id < static_cast<u32>(GDB_REGISTER_COUNT);
}

bool GDBServer::writeRegisterValue(u32 id)
{
	u32 value = 0;
	if (!readRegister(id, &value))
		value = 0;

	std::string hex;
	append_le_hex32(hex, value);
	return writePacketData(hex);
}

bool GDBServer::writeAllRegisters()
{
	for (u32 i = 0; i < static_cast<u32>(GDB_REGISTER_COUNT); i++)
	{
		if (!writeRegisterValue(i))
			return false;
	}
	return true;
}

bool GDBServer::processPcsx2CommandPacket(std::string_view data)
{
	const std::optional<std::string> decoded = decode_hex_string(data);
	if (!decoded.has_value())
		return writeError("E01");

	const std::string result = runPcsx2Command(*decoded);
	return writeResponse(encode_hex(result));
}

std::string GDBServer::runPcsx2Command(std::string_view command_view)
{
	const auto [verb, argument] = split_command(command_view);
	if (verb == "status")
		return getStatusString();

	if (verb == "pause")
	{
		Host::RunOnCPUThread([]() { VMManager::SetPaused(true); }, true);
		return "OK paused";
	}

	if (verb == "resume")
	{
		Host::RunOnCPUThread([]() { VMManager::SetPaused(false); }, true);
		return "OK running";
	}

	if (verb == "reset")
	{
		bool ok = false;
		Host::RunOnCPUThread([&ok]() { ok = VMManager::RequestReset(); }, true);
		return ok ? "OK reset" : "ERR reset failed";
	}

	if (verb == "frameadvance")
	{
		u32 frames = 1;
		if (!argument.empty())
			std::from_chars(argument.data(), argument.data() + argument.size(), frames, 10);
		Host::RunOnCPUThread([frames]() { VMManager::FrameAdvance(frames); }, true);
		return fmt::format("OK frameadvance {}", frames);
	}

	if (verb == "save_slot")
	{
		s32 slot = 0;
		if (!parse_slot(argument, &slot))
			return "ERR invalid slot";

		std::string error;
		Host::RunOnCPUThread([slot, &error]() {
			VMManager::SaveStateToSlot(slot, false, [&error](const std::string& message) { error = message; });
		}, true);
		return error.empty() ? fmt::format("OK save_slot {}", slot) : fmt::format("ERR {}", error);
	}

	if (verb == "load_slot")
	{
		s32 slot = 0;
		if (!parse_slot(argument, &slot))
			return "ERR invalid slot";

		bool ok = false;
		Error error;
		Host::RunOnCPUThread([slot, &ok, &error]() { ok = VMManager::LoadStateFromSlot(slot, false, &error); }, true);
		return ok ? fmt::format("OK load_slot {}", slot) : fmt::format("ERR {}", error.GetDescription());
	}

	if (verb == "load_backup_slot")
	{
		s32 slot = 0;
		if (!parse_slot(argument, &slot))
			return "ERR invalid slot";

		bool ok = false;
		Error error;
		Host::RunOnCPUThread([slot, &ok, &error]() { ok = VMManager::LoadStateFromSlot(slot, true, &error); }, true);
		return ok ? fmt::format("OK load_backup_slot {}", slot) : fmt::format("ERR {}", error.GetDescription());
	}

	if (verb == "save_file")
	{
		if (argument.empty())
			return "ERR missing path";

		std::string error;
		Host::RunOnCPUThread([path = argument, &error]() {
			VMManager::SaveState(path.c_str(), false, true, [&error](const std::string& message) { error = message; });
		}, true);
		return error.empty() ? fmt::format("OK save_file {}", argument) : fmt::format("ERR {}", error);
	}

	if (verb == "load_file")
	{
		if (argument.empty())
			return "ERR missing path";

		bool ok = false;
		Error error;
		Host::RunOnCPUThread([path = argument, &ok, &error]() { ok = VMManager::LoadState(path.c_str(), &error); }, true);
		return ok ? fmt::format("OK load_file {}", argument) : fmt::format("ERR {}", error.GetDescription());
	}

	if (verb == "patch_reload")
	{
		bool reload_files = true;
		if (!argument.empty())
			reload_files = (argument != "0" && argument != "false");

		Host::RunOnCPUThread([reload_files]() {
			VMManager::ReloadPatches(reload_files, true, true, true);
		}, true);
		return fmt::format("OK patch_reload reload_files={}", reload_files ? 1 : 0);
	}

	if (verb == "cheat_list")
	{
		bool cheats_enabled = false;
		u32 unlabelled_count = 0;
		std::vector<std::string> enabled_cheats;
		std::vector<Patch::PatchInfo> cheats;
		Host::RunOnCPUThread([&cheats_enabled, &enabled_cheats, &cheats, &unlabelled_count]() {
			cheats_enabled = Patch::GetCheatsGloballyEnabled();
			enabled_cheats = Patch::GetEnabledCheats();
			cheats = Patch::GetPatchInfo(VMManager::GetDiscSerial(), VMManager::GetCurrentCRC(), true, false, &unlabelled_count);
		}, true);

		std::ostringstream ss;
		ss << "OK cheats_enabled=" << (cheats_enabled ? "1" : "0") << " unlabelled=" << unlabelled_count;
		for (const Patch::PatchInfo& cheat : cheats)
		{
			const bool enabled = std::find(enabled_cheats.begin(), enabled_cheats.end(), cheat.name) != enabled_cheats.end();
			ss << '\n' << (enabled ? "1" : "0") << '\t' << cheat.name << '\t'
			   << Patch::PlaceToString(cheat.place) << '\t' << cheat.author << '\t' << cheat.description;
		}
		return ss.str();
	}

	if (verb == "cheat_set")
	{
		const auto [state_text, encoded_name] = split_command(argument);
		if (state_text.empty() || encoded_name.empty())
			return "ERR usage cheat_set <0|1> <hex_utf8_name>";

		const bool enabled = (state_text != "0" && state_text != "false");
		const std::optional<std::string> name = decode_hex_string(encoded_name);
		if (!name.has_value())
			return "ERR invalid cheat name encoding";

		bool ok = false;
		Host::RunOnCPUThread([&ok, &name, enabled]() {
			ok = Patch::SetCheatEnabled(*name, enabled, true, true);
		}, true);
		return ok ? fmt::format("OK cheat_set {} {}", enabled ? 1 : 0, *name) : fmt::format("ERR unknown cheat {}", *name);
	}

	if (verb == "cheats_enable")
	{
		if (argument.empty())
			return "ERR usage cheats_enable <0|1>";

		const bool enabled = (argument != "0" && argument != "false");
		Host::RunOnCPUThread([enabled]() {
			Patch::SetCheatsGloballyEnabled(enabled, true, true);
		}, true);
		return fmt::format("OK cheats_enable {}", enabled ? 1 : 0);
	}

	if (verb == "wait_savestate_flush")
	{
		Host::RunOnCPUThread([]() { VMManager::WaitForSaveStateFlush(); }, true);
		return "OK wait_savestate_flush";
	}

	return fmt::format("ERR unknown command {}", verb);
}

std::string GDBServer::getStatusString() const
{
	std::ostringstream ss;
	const VMState state = VMManager::GetState();
	ss << "OK state=" << state_to_string(state);
	ss << " has_vm=" << (VMManager::HasValidVM() ? "1" : "0");
	if (VMManager::HasValidVM())
	{
		ss << " title=" << VMManager::GetTitle(false);
		ss << " serial=" << VMManager::GetDiscSerial();
		ss << " disc_crc=" << fmt::format("{:08X}", VMManager::GetDiscCRC());
		ss << " current_crc=" << fmt::format("{:08X}", VMManager::GetCurrentCRC());
	}
	return ss.str();
}
