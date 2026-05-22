// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#pragma once

#include "DebugInterface.h"

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

static constexpr std::size_t MAX_DEBUG_PACKET_SIZE = 0x47ff;

class DebugServerInterface
{
public:
	virtual ~DebugServerInterface() = default;

	virtual void onClientConnected() {}
	virtual void onClientDisconnected() {}
	virtual bool replyPacket(void* outData, std::size_t& outSize) = 0;
	virtual std::size_t processPacket(const char* inData, std::size_t inSize, void* outData, std::size_t& outSize) = 0;

protected:
	DebugInterface* m_debugInterface = nullptr;
};

class DebugNetworkServer
{
public:
	DebugNetworkServer();
	~DebugNetworkServer();

	bool init(std::string_view name, std::unique_ptr<DebugServerInterface> debugServerInterface, u16 port, const char* address);
	void shutdown();

	bool isConnected() const;
	bool isRunning() const;
	int getPort() const;

private:
	bool setupSocket();
	bool acceptClient();
	void closeClient();
	void closeListenSocket();
	void serverLoop();
	bool clientLoop();
	bool sendData(const void* data, std::size_t size);

	std::string m_name;
	std::string m_address;
	std::unique_ptr<DebugServerInterface> m_debugServerInterface;

	std::atomic_bool m_end{true};
	std::atomic_bool m_connected{false};
	std::thread m_thread;
	std::mutex m_socketMutex;

	int m_port = -1;

#ifdef _WIN32
	void* m_sock = nullptr;
	void* m_msgsock = nullptr;
#else
	int m_sock = -1;
	int m_msgsock = -1;
#endif

	std::vector<u8> m_recvBuffer;
	std::vector<u8> m_pendingBuffer;
	std::vector<u8> m_sendBuffer;
};

extern DebugNetworkServer EEDebugNetworkServer;
extern DebugNetworkServer IOPDebugNetworkServer;
