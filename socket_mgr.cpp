#ifdef _MSC_VER
#include <Winsock2.h>
#include <Ws2tcpip.h>
#include <windows.h>
#endif
#ifdef __linux
#include <sys/epoll.h>
#endif
#ifdef __APPLE__
#include <sys/types.h>
#include <sys/event.h>
#include <sys/time.h>
#endif
#if defined(__linux) || defined(__APPLE__)
#include <sys/socket.h>
#include <netinet/in.h>
#include <arpa/inet.h>
#endif
#include "tools.h"
#include "socket_helper.h"
#include "io_buffer.h"
#include "socket_mgr.h"
#include "socket_listener.h"
#include "socket_stream.h"

#ifdef _MSC_VER
#pragma comment(lib, "Ws2_32.lib")
#endif

XSocketManager::XSocketManager()
{
#ifdef _MSC_VER
	WORD    wVersion = MAKEWORD(2, 2);
	WSADATA wsaData;
	WSAStartup(wVersion, &wsaData);
#endif
	m_szError[0] = '\0';
}

XSocketManager::~XSocketManager()
{
	for (auto pStreamSocket : m_StreamTable)
	{
		delete pStreamSocket;
	}
	m_StreamTable.clear();

	for (auto pListenSocket : m_ListenTable)
	{
		delete pListenSocket;
	}
	m_ListenTable.clear();

	for (auto it : m_ConnectingQueue)
	{
		CloseSocketHandle(it.nSocket);
	}
	m_ConnectingQueue.clear();

#ifdef _MSC_VER
	if (m_hCompletionPort != INVALID_HANDLE_VALUE)
	{
		CloseHandle(m_hCompletionPort);
		m_hCompletionPort = INVALID_HANDLE_VALUE;
	}
	WSACleanup();
#endif

#ifdef __linux
	if (m_nEpoll != -1)
	{
		close(m_nEpoll);
		m_nEpoll = -1;
	}
#endif
}

bool XSocketManager::Setup(int max_connection)
{
#ifdef _MSC_VER
	m_hCompletionPort = CreateIoCompletionPort(INVALID_HANDLE_VALUE, NULL, 0, 0);
	if (m_hCompletionPort == INVALID_HANDLE_VALUE)
		return false;
#endif

#ifdef __linux
	m_nEpoll = epoll_create(max_connection);
	if (m_nEpoll == -1)
		return false;
#endif

	m_max_connection = max_connection;
	m_Events.resize(max_connection);
	return true;
}

// support both ipv6 && ipv4
// http://www.ibm.com/support/knowledgecenter/ssw_i5_54/rzab6/xacceptboth.htm
ISocketListener* XSocketManager::Listen(const char szIP[], int nPort)
{
	int nRetCode = false;
	ISocketListener* pResult = nullptr;
	int nOne = 1;
	socket_t nSocket = INVALID_SOCKET;
	XSocketListener* pSocket = nullptr;
	sockaddr_storage addr;

	nRetCode = make_ip_addr(addr, szIP, nPort);
	FAILED_JUMP(nRetCode);

	nSocket = socket(addr.ss_family, SOCK_STREAM, IPPROTO_IP);
	FAILED_JUMP(nSocket != INVALID_SOCKET);

	SetSocketNoneBlock(nSocket);

	nRetCode = setsockopt(nSocket, SOL_SOCKET, SO_REUSEADDR, (char*)&nOne, sizeof(nOne));
	FAILED_JUMP(nRetCode != SOCKET_ERROR);

	nRetCode = bind(nSocket, (sockaddr*)&addr, sizeof(addr));
	FAILED_JUMP(nRetCode != SOCKET_ERROR);

	nRetCode = listen(nSocket, 16);
	FAILED_JUMP(nRetCode != SOCKET_ERROR);

	pSocket = new XSocketListener(this, nSocket);
	m_ListenTable.push_back(pSocket);

	pResult = pSocket;
Exit0:
	if (pResult == nullptr)
	{
		get_error_string(m_szError, sizeof(m_szError), GetSocketError());
		if (nSocket != INVALID_SOCKET)
		{
			CloseSocketHandle(nSocket);
			nSocket = INVALID_SOCKET;
		}
		SAFE_DELETE(pSocket);
	}
	return pResult;
}

void XSocketManager::ConnectAsync(const char szIP[], int nPort, const connecting_callback_t& callback, int nTimeout, size_t uRecvBufferSize, size_t uSendBufferSize)
{
	XAsyncConnecting cs;

	cs.callback = callback;
	cs.nSocket = INVALID_SOCKET;
	cs.dwBeginTime = get_time_ms();
	cs.nTimeout = nTimeout;
	cs.uRecvBufferSize = uRecvBufferSize;
	cs.uSendBufferSize = uSendBufferSize;
	cs.strRemoteIP = szIP;
	cs.nPort = nPort;

	m_ConnectingQueue.push_back(cs);
}

void XSocketManager::Wait(int nTimeout)
{
	ProcessSocketEvent(nTimeout);

	ProcessAsyncConnect();

	auto stream_it = m_StreamTable.begin();
	while (stream_it != m_StreamTable.end())
	{
		XSocketStream* pStream = *stream_it;

		pStream->Activate();

		if (pStream->m_bUserClosed 
#ifdef _MSC_VER
		&& pStream->m_bRecvComplete && pStream->m_bSendComplete
#endif
        )
		{
			delete pStream;
			stream_it = m_StreamTable.erase(stream_it);
			continue;
		}
		++stream_it;
	}

	auto listen_it = m_ListenTable.begin();
	while (listen_it != m_ListenTable.end())
	{
		XSocketListener* pListenSocket = *listen_it;
		pListenSocket->TryAccept();
		if (pListenSocket->m_bUserClosed)
		{
			delete pListenSocket;
			listen_it = m_ListenTable.erase(listen_it);
			continue;
		}
		++listen_it;
	}
}

void XSocketManager::ProcessAsyncConnect()
{
	int nRetCode = 0;
	int64_t dwTimeNow = get_time_ms();

	auto it = m_ConnectingQueue.begin();
	while (it != m_ConnectingQueue.end())
	{
		if (it->nSocket == INVALID_SOCKET)
		{
			it->nSocket = ConnectSocket(it->strRemoteIP.c_str(), it->nPort);
			if (it->nSocket == INVALID_SOCKET)
			{
				get_error_string(m_szError, _countof(m_szError), GetSocketError());
				it->callback(nullptr, m_szError);
				it = m_ConnectingQueue.erase(it);
				continue;
			}
		}

		if (CheckSocketWriteable(it->nSocket, 0))
		{
			int nError = 0;
			ISocketStream* pSocket = nullptr;
			socklen_t nSockLen = sizeof(nError);
			nRetCode = getsockopt(it->nSocket, SOL_SOCKET, SO_ERROR, (char*)&nError, &nSockLen);
			if (nRetCode == 0 && nError == 0)
			{
				pSocket = CreateStreamSocket(it->nSocket, it->uRecvBufferSize, it->uSendBufferSize, it->strRemoteIP);
			}
			else
			{
				get_error_string(m_szError, _countof(m_szError), nError);
			}

			if (pSocket == nullptr)
			{
				CloseSocketHandle(it->nSocket);
			}

			it->callback(pSocket, m_szError);
			it = m_ConnectingQueue.erase(it);
			continue;
		}

		if (it->nTimeout >= 0 && dwTimeNow - it->dwBeginTime > it->nTimeout)
		{
			it->callback(nullptr, "request_timeout");
			CloseSocketHandle(it->nSocket);
			it = m_ConnectingQueue.erase(it);
			continue;
		}

		++it;
	}
}


void XSocketManager::ProcessSocketEvent(int nTimeout)
{
#ifdef _MSC_VER
	ULONG uEventCount;
	int nRetCode = GetQueuedCompletionStatusEx(m_hCompletionPort, &m_Events[0], (ULONG)m_Events.size(), &uEventCount, (DWORD)nTimeout, false);
	if (!nRetCode)
		return;

	for (ULONG i = 0; i < uEventCount; i++)
	{
		OVERLAPPED_ENTRY& oe = m_Events[i];
		XSocketStream* pStream = (XSocketStream*)oe.lpCompletionKey;
		pStream->OnComplete(oe.lpOverlapped, oe.dwNumberOfBytesTransferred);
	}
#endif

#ifdef __linux
	int nCount = epoll_wait(m_nEpoll, m_Events, _countof(m_Events), nTimeout);
	for (int i = 0; i < nCount; i++)
	{
		epoll_event& ev = m_Events[i];
		auto& pStream = (ISocketStream*)ev.data.ptr;

		if (ev.events & EPOLLIN)
		{
			pStream->OnRecvAble();
		}

		if (ev.events & EPOLLOUT)
		{
			pStream->OnSendAble();
		}
	}
#endif

#ifdef __APPLE__
	timespec timeWait;
	timeWait.tv_sec = nTimeout / 1000;
	timeWait.tv_nsec = (nTimeout % 1000) * 1000000;
	int nCount = kevent(m_nKQ, nullptr, 0, &m_Events[0], (int)m_Events.size(), nTimeout >= 0 ? &timeWait : nullptr);
	for (int i = 0; i < nCount; i++)
	{
		struct kevent& ev = m_Events[i];
		auto pStream = (XSocketStream*)ev.udata;
		assert(ev.filter == EVFILT_READ || ev.filter == EVFILT_WRITE);
		if (ev.filter == EVFILT_READ)
		{
			pStream->OnRecvAble();
		}
		else if (ev.filter == EVFILT_WRITE)
		{
			pStream->OnSendAble();
		}
	}
#endif
}

ISocketStream* XSocketManager::CreateStreamSocket(socket_t nSocket, size_t uRecvBufferSize, size_t uSendBufferSize, const std::string& strRemoteIP)
{
	XSocketStream* pStream = new XSocketStream();

	SetSocketNoneBlock(nSocket);

#ifdef _MSC_VER
	HANDLE hHandle = CreateIoCompletionPort((HANDLE)nSocket, m_hCompletionPort, (ULONG_PTR)pStream, 0);
	if (hHandle != m_hCompletionPort)
	{
		get_error_string(m_szError, _countof(m_szError), GetLastError());
		delete pStream;
		return nullptr;
	}
#endif

#ifdef __linux
	epoll_event ev;
	ev.events = EPOLLIN | EPOLLOUT | EPOLLET;
	ev.data.ptr = pStream;

	int nRetCode = epoll_ctl(m_nEpoll, EPOLL_CTL_ADD, nSocket, &ev);
	if (nRetCode == -1)
	{
		get_error_string(m_szError, _countof(m_szError), GetSocketError());
		delete pStream;
		return nullptr;
	}
#endif

#ifdef __APPLE__
	struct kevent ev[2];
	// EV_CLEAR ���Ա��ش���? ע���д��־���ܰ�λ��
	EV_SET(&ev[0], nSocket, EVFILT_READ, EV_ADD | EV_CLEAR, 0, 0, pStream);
	EV_SET(&ev[1], nSocket, EVFILT_WRITE, EV_ADD | EV_CLEAR, 0, 0, pStream);
	int nRetCode = kevent(m_nKQ, ev, _countof(ev), nullptr, 0, nullptr);
	if (nRetCode == -1)
	{
		get_error_string(m_szError, _countof(m_szError), GetSocketError());
		delete pStream;
		return nullptr;
	}
#endif

	pStream->m_strRemoteIP = strRemoteIP;
	pStream->m_nSocket = nSocket;
	pStream->m_RecvBuffer.SetSize(uRecvBufferSize);
	pStream->m_SendBuffer.SetSize(uSendBufferSize);
	m_StreamTable.push_back(pStream);

#ifdef _MSC_VER
	pStream->AsyncRecv();
#endif

	return pStream;
}

ISocketManager* create_socket_mgr(int max_connection)
{
	XSocketManager* pMgr = new XSocketManager();
	if (!pMgr->Setup(max_connection))
	{
		delete pMgr;
		return nullptr;
	}
	return pMgr;
}
