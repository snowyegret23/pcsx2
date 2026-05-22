// SPDX-FileCopyrightText: 2002-2026 PCSX2 Dev Team
// SPDX-License-Identifier: GPL-3.0+

#include "DebugServer.h"

#include "common/Console.h"
#include "common/Threading.h"

#include <cstdlib>
#include <cstring>

#ifdef _WIN32
#include <WinSock2.h>
#include <WS2tcpip.h>
#include "common/RedtapeWindows.h"

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

using debug_socket_t = SOCKET;
static constexpr debug_socket_t INVALID_DEBUG_SOCKET = INVALID_SOCKET;

static debug_socket_t to_socket(void* value)
{
	if (value == nullptr)
		return INVALID_DEBUG_SOCKET;
	return static_cast<debug_socket_t>(reinterpret_cast<uintptr_t>(value));
}

static void* from_socket(debug_socket_t value)
{
	if (value == INVALID_DEBUG_SOCKET)
		return nullptr;
	return reinterpret_cast<void*>(static_cast<uintptr_t>(value));
}

static bool initialize_winsock()
{
	static bool initialized = false;
	if (initialized)
		return true;

	WSADATA wsa = {};
	if (WSAStartup(MAKEWORD(2, 2), &wsa) != 0)
		return false;

	initialized = true;
	std::atexit([]() { WSACleanup(); });
	return true;
}

static bool would_block()
{
	const int error = WSAGetLastError();
	return error == WSAEWOULDBLOCK || error == WSAEINPROGRESS || error == WSAEINTR;
}

static void close_socket(debug_socket_t socket)
{
	if (socket != INVALID_DEBUG_SOCKET)
		closesocket(socket);
}

static int read_socket(debug_socket_t socket, void* data, int size)
{
	return recv(socket, static_cast<char*>(data), size, 0);
}

static int write_socket(debug_socket_t socket, const void* data, int size)
{
	return send(socket, static_cast<const char*>(data), size, 0);
}

static void set_nonblocking(debug_socket_t socket)
{
	u_long mode = 1;
	ioctlsocket(socket, FIONBIO, &mode);
}

#else
#include <arpa/inet.h>
#include <cerrno>
#include <fcntl.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>

using debug_socket_t = int;
static constexpr debug_socket_t INVALID_DEBUG_SOCKET = -1;

static debug_socket_t to_socket(int value)
{
	return value;
}

static int from_socket(debug_socket_t value)
{
	return value;
}

static bool would_block()
{
	return errno == EWOULDBLOCK || errno == EAGAIN || errno == EINTR;
}

static void close_socket(debug_socket_t socket)
{
	if (socket != INVALID_DEBUG_SOCKET)
		close(socket);
}

static int read_socket(debug_socket_t socket, void* data, int size)
{
	return read(socket, data, size);
}

static int write_socket(debug_socket_t socket, const void* data, int size)
{
#ifdef MSG_NOSIGNAL
	return send(socket, data, size, MSG_NOSIGNAL);
#else
	return send(socket, data, size, 0);
#endif
}

static void set_nonblocking(debug_socket_t socket)
{
	fcntl(socket, F_SETFL, fcntl(socket, F_GETFL) | O_NONBLOCK);
}
#endif

DebugNetworkServer EEDebugNetworkServer;
DebugNetworkServer IOPDebugNetworkServer;

DebugNetworkServer::DebugNetworkServer() = default;

DebugNetworkServer::~DebugNetworkServer()
{
	shutdown();
}

bool DebugNetworkServer::init(
	std::string_view name,
	std::unique_ptr<DebugServerInterface> debugServerInterface,
	u16 port,
	const char* address)
{
	shutdown();

	if (!debugServerInterface)
	{
		Console.Error("DebugNetworkServer: missing debug server backend.");
		return false;
	}

	m_name = std::string(name);
	m_address = address ? std::string(address) : std::string();
	m_debugServerInterface = std::move(debugServerInterface);
	m_port = port;
	m_end.store(false, std::memory_order_release);
	m_recvBuffer.resize(MAX_DEBUG_PACKET_SIZE);
	m_pendingBuffer.clear();
	m_sendBuffer.resize(MAX_DEBUG_PACKET_SIZE);

	m_thread = std::thread(&DebugNetworkServer::serverLoop, this);
	return true;
}

void DebugNetworkServer::shutdown()
{
	m_end.store(true, std::memory_order_release);
	closeListenSocket();
	closeClient();

	if (m_thread.joinable() && std::this_thread::get_id() != m_thread.get_id())
		m_thread.join();

	if (!m_thread.joinable())
		m_debugServerInterface.reset();

	m_port = -1;
}

bool DebugNetworkServer::isConnected() const
{
	return m_connected.load(std::memory_order_acquire);
}

bool DebugNetworkServer::isRunning() const
{
	return m_thread.joinable() && !m_end.load(std::memory_order_acquire);
}

int DebugNetworkServer::getPort() const
{
	return m_port;
}

bool DebugNetworkServer::setupSocket()
{
#ifdef _WIN32
	if (!initialize_winsock())
	{
		Console.Error("DebugNetworkServer: WSAStartup failed.");
		return false;
	}
#endif

	const debug_socket_t sock = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (sock == INVALID_DEBUG_SOCKET)
	{
		Console.Error("DebugNetworkServer: failed to create socket for %s.", m_name.c_str());
		return false;
	}

	int enable = 1;
	setsockopt(sock, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&enable), sizeof(enable));

	sockaddr_in server = {};
	server.sin_family = AF_INET;
	server.sin_port = htons(static_cast<u16>(m_port));
	if (m_address.empty())
		server.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	else
		inet_pton(AF_INET, m_address.c_str(), &server.sin_addr);

	if (bind(sock, reinterpret_cast<sockaddr*>(&server), sizeof(server)) != 0)
	{
		Console.Error("DebugNetworkServer: failed to bind %s GDB server on port %d.", m_name.c_str(), m_port);
		close_socket(sock);
		return false;
	}

	if (listen(sock, 1) != 0)
	{
		Console.Error("DebugNetworkServer: failed to listen for %s GDB server.", m_name.c_str());
		close_socket(sock);
		return false;
	}

	std::lock_guard lock(m_socketMutex);
	m_sock = from_socket(sock);
	return true;
}

bool DebugNetworkServer::acceptClient()
{
	const debug_socket_t sock = to_socket(m_sock);
	if (sock == INVALID_DEBUG_SOCKET)
		return false;

	const debug_socket_t client = accept(sock, nullptr, nullptr);
	if (client == INVALID_DEBUG_SOCKET)
	{
		if (!m_end.load(std::memory_order_acquire))
			Threading::Sleep(1);
		return false;
	}

	set_nonblocking(client);
	{
		std::lock_guard lock(m_socketMutex);
		m_msgsock = from_socket(client);
	}

	m_connected.store(true, std::memory_order_release);
	m_pendingBuffer.clear();
	if (m_debugServerInterface)
		m_debugServerInterface->onClientConnected();

	Console.WriteLn(Color_Green, "DebugNetworkServer: %s GDB client connected on port %d.", m_name.c_str(), m_port);
	return true;
}

void DebugNetworkServer::closeClient()
{
	debug_socket_t client = INVALID_DEBUG_SOCKET;
	{
		std::lock_guard lock(m_socketMutex);
		client = to_socket(m_msgsock);
		m_msgsock = from_socket(INVALID_DEBUG_SOCKET);
	}

	if (client != INVALID_DEBUG_SOCKET)
	{
#ifdef _WIN32
		::shutdown(client, SD_BOTH);
#else
		::shutdown(client, SHUT_RDWR);
#endif
		close_socket(client);
	}

	if (m_connected.exchange(false, std::memory_order_acq_rel) && m_debugServerInterface)
		m_debugServerInterface->onClientDisconnected();
}

void DebugNetworkServer::closeListenSocket()
{
	debug_socket_t sock = INVALID_DEBUG_SOCKET;
	{
		std::lock_guard lock(m_socketMutex);
		sock = to_socket(m_sock);
		m_sock = from_socket(INVALID_DEBUG_SOCKET);
	}

	if (sock != INVALID_DEBUG_SOCKET)
	{
#ifdef _WIN32
		::shutdown(sock, SD_BOTH);
#else
		::shutdown(sock, SHUT_RDWR);
#endif
		close_socket(sock);
	}
}

void DebugNetworkServer::serverLoop()
{
	Threading::SetNameOfCurrentThread("GDB Server");

	if (!setupSocket())
	{
		m_end.store(true, std::memory_order_release);
		return;
	}

	Console.WriteLn(Color_Green, "DebugNetworkServer: %s GDB server listening on 127.0.0.1:%d.", m_name.c_str(), m_port);
	while (!m_end.load(std::memory_order_acquire))
	{
		if (!acceptClient())
			continue;

		clientLoop();
		closeClient();
	}

	closeListenSocket();
}

bool DebugNetworkServer::clientLoop()
{
	while (!m_end.load(std::memory_order_acquire))
	{
		std::size_t outSize = 0;
		if (m_debugServerInterface && !m_debugServerInterface->replyPacket(m_sendBuffer.data(), outSize))
			return false;
		if (outSize > 0 && !sendData(m_sendBuffer.data(), outSize))
			return false;

		const debug_socket_t client = to_socket(m_msgsock);
		const int read = read_socket(client, m_recvBuffer.data(), static_cast<int>(m_recvBuffer.size()));
		if (read < 0)
		{
			if (would_block())
			{
				Threading::Sleep(1);
				continue;
			}
			return false;
		}
		if (read == 0)
			return false;

		m_pendingBuffer.insert(m_pendingBuffer.end(), m_recvBuffer.begin(), m_recvBuffer.begin() + read);

		for (;;)
		{
			outSize = 0;
			const std::size_t consumed = m_debugServerInterface->processPacket(
				reinterpret_cast<const char*>(m_pendingBuffer.data()), m_pendingBuffer.size(), m_sendBuffer.data(), outSize);
			if (consumed == static_cast<std::size_t>(-1))
				return false;
			if (consumed == 0)
				break;

			m_pendingBuffer.erase(m_pendingBuffer.begin(), m_pendingBuffer.begin() + consumed);
			if (outSize > 0 && !sendData(m_sendBuffer.data(), outSize))
				return false;
			if (m_pendingBuffer.empty())
				break;
		}
	}

	return true;
}

bool DebugNetworkServer::sendData(const void* data, std::size_t size)
{
	const debug_socket_t client = to_socket(m_msgsock);
	const u8* bytes = static_cast<const u8*>(data);
	std::size_t written = 0;

	while (written < size && !m_end.load(std::memory_order_acquire))
	{
		const int ret = write_socket(client, bytes + written, static_cast<int>(size - written));
		if (ret < 0)
		{
			if (would_block())
			{
				Threading::Sleep(1);
				continue;
			}
			return false;
		}
		if (ret == 0)
			return false;
		written += static_cast<std::size_t>(ret);
	}

	return written == size;
}
