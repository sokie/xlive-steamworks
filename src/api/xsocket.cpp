// #1 - #40 Winsock surface.
#include "xlive/xfuncs.h"

#include "core/log.h"
#include "core/net.h"
#include "core/steam.h"

#include <map>
#include <mutex>
#include <vector>

namespace {

bool g_wsaStarted = false;

// Overlapped Winsock calls complete immediately, the results wait here for
// XWSAGetOverlappedResult.
std::mutex g_overlappedMutex;
std::map<WSAOVERLAPPED*, std::pair<DWORD, DWORD>> g_overlappedResults; // bytes, error

void StoreOverlapped(WSAOVERLAPPED* overlapped, DWORD bytes, DWORD error)
{
	if (!overlapped) {
		return;
	}
	{
		std::lock_guard<std::mutex> lock(g_overlappedMutex);
		g_overlappedResults[overlapped] = std::make_pair(bytes, error);
	}
	overlapped->Internal = error;
	overlapped->InternalHigh = bytes;
	if (overlapped->hEvent) {
		SetEvent(overlapped->hEvent);
	}
}

}

// #1
int WINAPI XWSAStartup(WORD wVersionRequested, WSADATA* lpWSAData)
{
	XLS_TRACE_FN();
	int result = WSAStartup(wVersionRequested, lpWSAData);
	if (result == 0) {
		g_wsaStarted = true;
		xls::NetInit();
	}
	return result;
}

// #2
int WINAPI XWSACleanup()
{
	XLS_TRACE_FN();
	g_wsaStarted = false;
	return WSACleanup();
}

// #3
SOCKET WINAPI XSocketCreate(int af, int type, int protocol)
{
	XLS_TRACE_FN();
	return xls::NetSocketCreate(af, type, protocol);
}

// #4
int WINAPI XSocketClose(SOCKET s)
{
	XLS_TRACE_FN();
	return xls::NetSocketClose(s);
}

// #5
int WINAPI XSocketShutdown(SOCKET s, int how)
{
	XLS_TRACE_FN();
	return xls::NetSocketShutdown(s, how);
}

// #6
int WINAPI XSocketIOCTLSocket(SOCKET s, long cmd, u_long* argp)
{
	XLS_TRACE_FN();
	return xls::NetSocketIoctl(s, cmd, argp);
}

// #7
int WINAPI XSocketSetSockOpt(SOCKET s, int level, int optname, const char* optval, int optlen)
{
	XLS_TRACE_FN();
	return xls::NetSocketSetOpt(s, level, optname, optval, optlen);
}

// #8
int WINAPI XSocketGetSockOpt(SOCKET s, int level, int optname, char* optval, int* optlen)
{
	XLS_TRACE_FN();
	return xls::NetSocketGetOpt(s, level, optname, optval, optlen);
}

// #9
int WINAPI XSocketGetSockName(SOCKET s, sockaddr* name, int* namelen)
{
	XLS_TRACE_FN();
	return xls::NetSocketGetSockName(s, name, namelen);
}

// #10
int WINAPI XSocketGetPeerName(SOCKET s, sockaddr* name, int* namelen)
{
	XLS_TRACE_FN();
	return xls::NetSocketGetPeerName(s, name, namelen);
}

// #11
int WINAPI XSocketBind(SOCKET s, const sockaddr* name, int namelen)
{
	XLS_TRACE_FN();
	return xls::NetSocketBind(s, name, namelen);
}

// #12
int WINAPI XSocketConnect(SOCKET s, const sockaddr* name, int namelen)
{
	XLS_TRACE_FN();
	return xls::NetSocketConnect(s, name, namelen);
}

// #13
int WINAPI XSocketListen(SOCKET s, int backlog)
{
	XLS_TRACE_FN();
	return xls::NetSocketListen(s, backlog);
}

// #14
SOCKET WINAPI XSocketAccept(SOCKET s, sockaddr* addr, int* addrlen)
{
	XLS_TRACE_FN();
	return xls::NetSocketAccept(s, addr, addrlen);
}

// #15
int WINAPI XSocketSelect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const timeval* timeout)
{
	XLS_TRACE_FN();
	return xls::NetSocketSelect(nfds, readfds, writefds, exceptfds, timeout);
}

// #16
BOOL WINAPI XWSAGetOverlappedResult(SOCKET s, WSAOVERLAPPED* lpOverlapped, DWORD* lpcbTransfer, BOOL fWait, DWORD* lpdwFlags)
{
	XLS_TRACE_FN();
	if (!lpOverlapped) {
		WSASetLastError(WSAEFAULT);
		return FALSE;
	}
	std::lock_guard<std::mutex> lock(g_overlappedMutex);
	auto it = g_overlappedResults.find(lpOverlapped);
	if (it == g_overlappedResults.end()) {
		WSASetLastError(WSA_IO_INCOMPLETE);
		return FALSE;
	}
	if (lpcbTransfer) {
		*lpcbTransfer = it->second.first;
	}
	if (lpdwFlags) {
		*lpdwFlags = 0;
	}
	DWORD error = it->second.second;
	g_overlappedResults.erase(it);
	if (error) {
		WSASetLastError((int)error);
		return FALSE;
	}
	return TRUE;
}

// #17
int WINAPI XWSACancelOverlappedIO(SOCKET s)
{
	XLS_TRACE_FN();
	return 0;
}

// #18
int WINAPI XSocketRecv(SOCKET s, char* buf, int len, int flags)
{
	XLS_TRACE_FN();
	return xls::NetSocketRecv(s, buf, len, flags);
}

// #19
int WINAPI XWSARecv(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesRecvd, DWORD* lpFlags, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	XLS_TRACE_FN();
	if (!lpBuffers || !dwBufferCount) {
		WSASetLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}
	int received = xls::NetSocketRecv(s, lpBuffers[0].buf, (int)lpBuffers[0].len, lpFlags ? (int)*lpFlags : 0);
	if (received == SOCKET_ERROR) {
		int error = WSAGetLastError();
		if (lpOverlapped && error == WSAEWOULDBLOCK) {
			// The overlapped form has no would-block, so report pending and let the title poll.
			WSASetLastError(WSA_IO_PENDING);
		}
		return SOCKET_ERROR;
	}
	if (lpNumberOfBytesRecvd) {
		*lpNumberOfBytesRecvd = (DWORD)received;
	}
	if (lpFlags) {
		*lpFlags = 0;
	}
	StoreOverlapped(lpOverlapped, (DWORD)received, 0);
	if (lpOverlapped && lpCompletionRoutine) {
		lpCompletionRoutine(0, (DWORD)received, lpOverlapped, 0);
	}
	return 0;
}

// #20
int WINAPI XSocketRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen)
{
	XLS_TRACE_FN();
	return xls::NetSocketRecvFrom(s, buf, len, flags, from, fromlen);
}

// #21
int WINAPI XWSARecvFrom(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesRecvd, DWORD* lpFlags, sockaddr* lpFrom, int* lpFromlen, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	XLS_TRACE_FN();
	if (!lpBuffers || !dwBufferCount) {
		WSASetLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}
	int received = xls::NetSocketRecvFrom(s, lpBuffers[0].buf, (int)lpBuffers[0].len, lpFlags ? (int)*lpFlags : 0, lpFrom, lpFromlen);
	if (received == SOCKET_ERROR) {
		int error = WSAGetLastError();
		if (lpOverlapped && error == WSAEWOULDBLOCK) {
			WSASetLastError(WSA_IO_PENDING);
		}
		return SOCKET_ERROR;
	}
	if (lpNumberOfBytesRecvd) {
		*lpNumberOfBytesRecvd = (DWORD)received;
	}
	if (lpFlags) {
		*lpFlags = 0;
	}
	StoreOverlapped(lpOverlapped, (DWORD)received, 0);
	if (lpOverlapped && lpCompletionRoutine) {
		lpCompletionRoutine(0, (DWORD)received, lpOverlapped, 0);
	}
	return 0;
}

// #22
int WINAPI XSocketSend(SOCKET s, const char* buf, int len, int flags)
{
	XLS_TRACE_FN();
	return xls::NetSocketSend(s, buf, len, flags);
}

// #23
int WINAPI XWSASend(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesSent, DWORD dwFlags, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	XLS_TRACE_FN();
	if (!lpBuffers || !dwBufferCount) {
		WSASetLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}
	DWORD total = 0;
	for (DWORD i = 0; i < dwBufferCount; i++) {
		int sent = xls::NetSocketSend(s, lpBuffers[i].buf, (int)lpBuffers[i].len, (int)dwFlags);
		if (sent == SOCKET_ERROR) {
			return SOCKET_ERROR;
		}
		total += (DWORD)sent;
	}
	if (lpNumberOfBytesSent) {
		*lpNumberOfBytesSent = total;
	}
	StoreOverlapped(lpOverlapped, total, 0);
	if (lpOverlapped && lpCompletionRoutine) {
		lpCompletionRoutine(0, total, lpOverlapped, 0);
	}
	return 0;
}

// #24
int WINAPI XSocketSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen)
{
	XLS_TRACE_FN();
	return xls::NetSocketSendTo(s, buf, len, flags, to, tolen);
}

// #25
int WINAPI XWSASendTo(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesSent, DWORD dwFlags, const sockaddr* lpTo, int iTolen, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine)
{
	XLS_TRACE_FN();
	if (!lpBuffers || !dwBufferCount) {
		WSASetLastError(WSAEFAULT);
		return SOCKET_ERROR;
	}
	// Scattered buffers are one datagram.
	DWORD total = 0;
	for (DWORD i = 0; i < dwBufferCount; i++) {
		total += lpBuffers[i].len;
	}
	std::vector<char> joined(total);
	DWORD offset = 0;
	for (DWORD i = 0; i < dwBufferCount; i++) {
		memcpy(joined.data() + offset, lpBuffers[i].buf, lpBuffers[i].len);
		offset += lpBuffers[i].len;
	}
	int sent = xls::NetSocketSendTo(s, joined.data(), (int)total, (int)dwFlags, lpTo, iTolen);
	if (sent == SOCKET_ERROR) {
		return SOCKET_ERROR;
	}
	if (lpNumberOfBytesSent) {
		*lpNumberOfBytesSent = (DWORD)sent;
	}
	StoreOverlapped(lpOverlapped, (DWORD)sent, 0);
	if (lpOverlapped && lpCompletionRoutine) {
		lpCompletionRoutine(0, (DWORD)sent, lpOverlapped, 0);
	}
	return 0;
}

// #26
unsigned long WINAPI XSocketInet_Addr(const char* cp)
{
	XLS_TRACE_FN();
	return inet_addr(cp);
}

// #27
int WINAPI XSocketWSAGetLastError()
{
	XLS_TRACE_FN();
	return WSAGetLastError();
}

// #28
void WINAPI XWSASetLastError(int iError)
{
	XLS_TRACE_FN();
	WSASetLastError(iError);
}

// #29
WSAEVENT WINAPI XWSACreateEvent()
{
	XLS_TRACE_FN();
	return WSACreateEvent();
}

// #30
BOOL WINAPI XWSACloseEvent(WSAEVENT hEvent)
{
	XLS_TRACE_FN();
	return WSACloseEvent(hEvent);
}

// #31
BOOL WINAPI XWSASetEvent(WSAEVENT hEvent)
{
	XLS_TRACE_FN();
	return WSASetEvent(hEvent);
}

// #32
BOOL WINAPI XWSAResetEvent(WSAEVENT hEvent)
{
	XLS_TRACE_FN();
	return WSAResetEvent(hEvent);
}

// #33
DWORD WINAPI XWSAWaitForMultipleEvents(DWORD cEvents, const WSAEVENT* lphEvents, BOOL fWaitAll, DWORD dwTimeout, BOOL fAlertable)
{
	XLS_TRACE_FN();
	// Data only arrives while the pump runs, so wait in slices and pump between them.
	DWORD started = GetTickCount();
	while (true) {
		DWORD slice = dwTimeout == WSA_INFINITE ? 5 : std::min<DWORD>(5, dwTimeout - std::min<DWORD>(dwTimeout, GetTickCount() - started));
		DWORD result = WSAWaitForMultipleEvents(cEvents, lphEvents, fWaitAll, slice, fAlertable);
		if (result != WSA_WAIT_TIMEOUT) {
			return result;
		}
		if (dwTimeout != WSA_INFINITE && GetTickCount() - started >= dwTimeout) {
			return WSA_WAIT_TIMEOUT;
		}
		xls::SteamPumpForce();
	}
}

// #34
int WINAPI XWSAFDIsSet(SOCKET fd, fd_set* set)
{
	XLS_TRACE_FN();
	return __WSAFDIsSet(fd, set);
}

// #35
int WINAPI XWSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents)
{
	XLS_TRACE_FN();
	return xls::NetSocketEventSelect(s, hEventObject, lNetworkEvents);
}

// #37
u_long WINAPI XSocketHTONL(u_long hostlong)
{
	return htonl(hostlong);
}

// #38
u_short WINAPI XSocketNTOHS(u_short netshort)
{
	return ntohs(netshort);
}

// #39
u_long WINAPI XSocketNTOHL(u_long netlong)
{
	return ntohl(netlong);
}

// #40
u_short WINAPI XSocketHTONS(u_short hostshort)
{
	return htons(hostshort);
}
