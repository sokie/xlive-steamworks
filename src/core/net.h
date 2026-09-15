// XNet and XSocket on Steam networking: a secure IN_ADDR is a local alias for a Steam id, a UDP
// port is a SteamNetworkingMessages channel, a TCP port is a P2P virtual port. Title server
// addresses keep their real IP and go through Winsock.
#pragma once

#include "xlive/xdefs.h"
#include "core/steam.h"

namespace xls {

void NetInit();
void NetShutdown();
// Receives pending Steam messages into socket queues and services QoS and connection state.
void NetPump();

// --- Addresses -----------------------------------------------------------------------------------

void NetLocalXnaddr(XNADDR* out);
void NetXnaddrForSteamId(CSteamID steamId, XNADDR* out);
bool NetSteamIdFromXnaddr(const XNADDR& xnaddr, CSteamID* out);

// Secure address aliases. Registering the same peer twice returns the same alias.
IN_ADDR NetSecureAddrFor(CSteamID steamId);
IN_ADDR NetSecureAddrForServer(IN_ADDR realAddress, DWORD serviceId);
bool NetSecureAddrIsKnown(IN_ADDR alias);
bool NetSecureAddrToSteamId(IN_ADDR alias, CSteamID* out);
bool NetSecureAddrToServer(IN_ADDR alias, IN_ADDR* realAddress);
void NetSecureAddrRelease(IN_ADDR alias);
DWORD NetConnectStatus(IN_ADDR alias);
void NetConnectStart(IN_ADDR alias);
bool NetIsLocalAlias(IN_ADDR alias);

// --- Keys ----------------------------------------------------------------------------------------

void NetCreateKey(XNKID* xnkid, XNKEY* xnkey);
bool NetRegisterKey(const XNKID& xnkid, const XNKEY& xnkey);
bool NetUnregisterKey(const XNKID& xnkid);
bool NetReplaceKey(const XNKID& oldKey, const XNKID& newKey);
bool NetKeyRegistered(const XNKID& xnkid);

// --- QoS -----------------------------------------------------------------------------------------

INT NetQosListen(const XNKID& xnkid, const uint8_t* data, UINT size, DWORD bitsPerSecond, DWORD flags);
INT NetQosLookup(UINT count, const XNADDR* xnaddrs[], const XNKID* xnkids[], const XNKEY* xnkeys[], UINT serviceCount, const IN_ADDR services[], const DWORD serviceIds[], UINT probes, DWORD bitsPerSecond, DWORD flags, WSAEVENT event, XNQOS** out);
INT NetQosRelease(XNQOS* qos);
INT NetQosListenStats(const XNKID& xnkid, XNQOSLISTENSTATS* stats);

// --- Sockets -------------------------------------------------------------------------------------

SOCKET NetSocketCreate(int af, int type, int protocol);
int NetSocketClose(SOCKET s);
int NetSocketShutdown(SOCKET s, int how);
int NetSocketIoctl(SOCKET s, long command, u_long* argument);
int NetSocketSetOpt(SOCKET s, int level, int name, const char* value, int length);
int NetSocketGetOpt(SOCKET s, int level, int name, char* value, int* length);
int NetSocketGetSockName(SOCKET s, sockaddr* name, int* length);
int NetSocketGetPeerName(SOCKET s, sockaddr* name, int* length);
int NetSocketBind(SOCKET s, const sockaddr* name, int length);
int NetSocketConnect(SOCKET s, const sockaddr* name, int length);
int NetSocketListen(SOCKET s, int backlog);
SOCKET NetSocketAccept(SOCKET s, sockaddr* address, int* length);
int NetSocketSelect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const timeval* timeout);
int NetSocketRecv(SOCKET s, char* buffer, int length, int flags);
int NetSocketRecvFrom(SOCKET s, char* buffer, int length, int flags, sockaddr* from, int* fromLength);
int NetSocketSend(SOCKET s, const char* buffer, int length, int flags);
int NetSocketSendTo(SOCKET s, const char* buffer, int length, int flags, const sockaddr* to, int toLength);
int NetSocketEventSelect(SOCKET s, WSAEVENT event, long events);
bool NetSocketIsValid(SOCKET s);

}
