#include "core/net.h"

#include "core/config.h"
#include "core/log.h"
#include "core/utils.h"

#include <ws2tcpip.h>
#include <iphlpapi.h>
#include <algorithm>
#include <deque>
#include <map>
#include <mutex>
#include <set>
#include <vector>

namespace xls {

namespace {

// Wire format of every datagram this layer sends over SteamNetworkingMessages.
#pragma pack(push, 1)
struct DatagramHeader {
	uint8_t kind;     // DatagramKind
	uint8_t flags;
	uint16_t sourcePort;
};

struct QosProbe {
	uint8_t kind;
	uint8_t sequence;
	uint16_t reserved;
	uint32_t sentTick;
	XNKID xnkid;
};

struct QosReply {
	uint8_t kind;
	uint8_t sequence;
	uint16_t dataSize;
	uint32_t sentTick;
	uint8_t flags;    // QosReplyFlags
	uint8_t reserved[3];
	// dataSize bytes follow.
};
#pragma pack(pop)

enum DatagramKind : uint8_t {
	kDatagramData = 1,
	kQosProbeKind = 2,
	kQosReplyKind = 3,
};

enum QosReplyFlags : uint8_t {
	kQosReplyDisabled = 1,
};

const int kControlChannel = 0x10000;           // Above every UDP port.
const uint32_t kXnaddrMagic = 0x53544D57;      // 'STMW' in abOnline, after the Steam id.
const size_t kMaxQueuedDatagrams = 512;
const DWORD kQosProbeTimeoutMs = 3000;
const DWORD kQosWarmupMs = 2500;
const uint8_t kQosDefaultProbes = 8;

// --- Secure address table ------------------------------------------------------------------------

struct SecureEntry {
	bool isServer = false;
	CSteamID steamId;
	IN_ADDR realAddress = {};
	DWORD serviceId = 0;
	DWORD connectStatus = XNET_CONNECT_STATUS_IDLE;
};

std::recursive_mutex g_mutex;
std::map<uint32_t, SecureEntry> g_secure;   // Keyed by alias in host order.
std::map<uint64_t, uint32_t> g_aliasBySteamId;
std::map<uint64_t, uint32_t> g_aliasByServer; // (ip << 32 | serviceId)
uint32_t g_nextAlias = 0x0A000002;           // 10.0.0.2, since 10.0.0.1 is the local machine.
const uint32_t kLocalAlias = 0x0A000001;

std::map<uint64_t, XNKEY> g_keys;            // XNKID as 64-bit.

uint64_t KidToInt(const XNKID& xnkid)
{
	uint64_t value;
	memcpy(&value, xnkid.ab, sizeof(value));
	return value;
}

uint32_t AliasOf(IN_ADDR address)
{
	return ntohl(address.S_un.S_addr);
}

IN_ADDR AliasToInAddr(uint32_t alias)
{
	IN_ADDR result;
	result.S_un.S_addr = htonl(alias);
	return result;
}

bool ResolveAlias(IN_ADDR address, SecureEntry* entry)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_secure.find(AliasOf(address));
	if (it == g_secure.end()) {
		return false;
	}
	*entry = it->second;
	return true;
}

// --- QoS -----------------------------------------------------------------------------------------

struct QosListener {
	bool enabled = true;
	std::vector<uint8_t> data;
	DWORD bitsPerSecond = 0;
	XNQOSLISTENSTATS stats = {};
};

struct QosTarget {
	CSteamID steamId;
	XNKID xnkid = {};
	bool isService = false;
	IN_ADDR serviceAddress = {};
	uint8_t probesSent = 0;
	uint8_t probesReceived = 0;
	std::vector<uint16_t> rtts;
	std::vector<uint8_t> data;
	bool dataReceived = false;
	bool disabled = false;
	bool complete = false;
	bool sessionReady = false;
	bool warmSent = false;
};

struct QosLookup {
	XNQOS* result = nullptr;
	std::vector<QosTarget> targets;
	WSAEVENT event = nullptr;
	DWORD startedTick = 0;
	uint8_t probeCount = kQosDefaultProbes;
	uint8_t nextSequence = 0;
	DWORD lastProbeTick = 0;
	DWORD probeStartTick = 0;
};

std::map<uint64_t, QosListener> g_qosListeners;
std::vector<QosLookup*> g_qosLookups;

// --- Sockets -------------------------------------------------------------------------------------

struct Datagram {
	IN_ADDR from;
	uint16_t fromPort;
	std::vector<uint8_t> data;
};

struct XlsSocket {
	SOCKET handle = INVALID_SOCKET;
	int type = SOCK_DGRAM;
	int protocol = IPPROTO_UDP;
	bool bound = false;
	uint16_t port = 0;
	bool nonBlocking = false;
	bool broadcast = false;
	bool noDelay = false;
	DWORD receiveTimeoutMs = 0;
	DWORD sendTimeoutMs = 0;
	DWORD receiveBufferSize = 16 * 1024;
	DWORD sendBufferSize = 16 * 1024;

	std::deque<Datagram> datagrams;

	// A Winsock socket for title servers and other real addresses.
	SOCKET real = INVALID_SOCKET;

	HSteamNetConnection connection = k_HSteamNetConnection_Invalid;
	HSteamListenSocket listenSocket = k_HSteamListenSocket_Invalid;
	std::deque<HSteamNetConnection> acceptQueue;
	std::vector<uint8_t> stream;
	bool connecting = false;
	bool connected = false;
	bool closedByPeer = false;
	int connectError = 0;
	CSteamID peer;
	uint16_t peerPort = 0;
	IN_ADDR peerAlias = {};

	WSAEVENT event = nullptr;
	long eventMask = 0;

	void Signal(long which)
	{
		if (event && (eventMask & which)) {
			SetEvent(event);
		}
	}
};

std::map<SOCKET, XlsSocket*> g_sockets;
std::map<uint16_t, XlsSocket*> g_dgramByPort;
std::map<HSteamNetConnection, XlsSocket*> g_socketByConnection;
std::map<HSteamListenSocket, XlsSocket*> g_socketByListen;
SOCKET g_nextHandle = 0x2000;
uint16_t g_nextEphemeral = 49152;
bool g_started = false;
XNADDR g_localXnaddr = {};
int g_p2pTransport = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Default;
bool g_p2pTransportSet = false;
bool g_hadPeers = false;

void ApplyP2PTransport()
{
	if (!SteamReady() || !SteamNetworkingUtils()) {
		return;
	}
	SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_P2P_Transport_ICE_Enable, g_p2pTransport);
	XLS_LOG_INFO("net: P2P transport ICE mask %d%s.", g_p2pTransport, g_p2pTransport == k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable ? " (relay only)" : "");
}

void FillConnectionInfo(const SteamNetConnectionInfo_t& info, const SteamNetConnectionRealTimeStatus_t* status, XLS_CONNECTION_INFO* out)
{
	memset(out, 0, sizeof(*out));
	out->dwState = (DWORD)info.m_eState;
	out->dwFlags = (DWORD)info.m_nFlags;
	out->fRelayed = ((info.m_nFlags & k_nSteamNetworkConnectionInfoFlags_Relayed) || info.m_idPOPRelay) ? TRUE : FALSE;
	out->fDirect = (!out->fRelayed && !info.m_addrRemote.IsIPv6AllZeros()) ? TRUE : FALSE;
	if (info.m_idPOPRelay) {
		GetSteamNetworkingLocationPOPStringFromID(info.m_idPOPRelay, out->szRelayPop);
	}
	if (info.m_idPOPRemote) {
		GetSteamNetworkingLocationPOPStringFromID(info.m_idPOPRemote, out->szRemotePop);
	}
	if (!info.m_addrRemote.IsIPv6AllZeros()) {
		info.m_addrRemote.ToString(out->szRemoteAddr, sizeof(out->szRemoteAddr), true);
	}
	if (status) {
		out->dwPingMs = status->m_nPing > 0 ? (DWORD)status->m_nPing : 0;
		out->flQualityLocal = status->m_flConnectionQualityLocal;
		out->flQualityRemote = status->m_flConnectionQualityRemote;
	}
	CopyStringA(out->szDescription, sizeof(out->szDescription), info.m_szConnectionDescription);
}

XlsSocket* Find(SOCKET s)
{
	auto it = g_sockets.find(s);
	return it == g_sockets.end() ? nullptr : it->second;
}

int Fail(int error)
{
	WSASetLastError(error);
	return SOCKET_ERROR;
}

IN_ADDR LocalLanAddress()
{
	IN_ADDR result = {};
	result.S_un.S_addr = htonl(0x7F000001);
	char hostname[256] = {};
	if (gethostname(hostname, sizeof(hostname)) != 0) {
		return result;
	}
	addrinfo hints = {};
	hints.ai_family = AF_INET;
	addrinfo* info = nullptr;
	if (getaddrinfo(hostname, nullptr, &hints, &info) != 0 || !info) {
		return result;
	}
	for (addrinfo* entry = info; entry; entry = entry->ai_next) {
		sockaddr_in* address = (sockaddr_in*)entry->ai_addr;
		if (address->sin_addr.S_un.S_addr != htonl(0x7F000001)) {
			result = address->sin_addr;
			break;
		}
	}
	freeaddrinfo(info);
	return result;
}

void BuildXnaddr(CSteamID steamId, XNADDR* out)
{
	memset(out, 0, sizeof(*out));
	uint32_t account = steamId.GetAccountID();
	// A private-range pseudo address, so titles that print or compare it see a plausible value.
	out->ina.S_un.S_addr = htonl(0x0A000000 | (account & 0x00FFFFFF));
	out->inaOnline = out->ina;
	out->wPortOnline = 0;
	// The MAC is what titles compare to tell peers apart, so it must be unique per account.
	out->abEnet[0] = 0x02;
	out->abEnet[1] = 0x53;
	memcpy(out->abEnet + 2, &account, sizeof(account));
	uint64_t id64 = steamId.ConvertToUint64();
	memcpy(out->abOnline, &id64, sizeof(id64));
	memcpy(out->abOnline + 8, &kXnaddrMagic, sizeof(kXnaddrMagic));
}

SteamNetworkingIdentity IdentityOf(CSteamID steamId)
{
	SteamNetworkingIdentity identity;
	identity.SetSteamID(steamId);
	return identity;
}

// Delivers a datagram to the local socket bound to port, if any.
void DeliverLocal(uint16_t port, IN_ADDR from, uint16_t fromPort, const uint8_t* data, size_t size)
{
	auto it = g_dgramByPort.find(port);
	if (it == g_dgramByPort.end()) {
		return;
	}
	XlsSocket* socket = it->second;
	if (socket->datagrams.size() >= kMaxQueuedDatagrams) {
		return;
	}
	Datagram datagram;
	datagram.from = from;
	datagram.fromPort = fromPort;
	datagram.data.assign(data, data + size);
	socket->datagrams.push_back(std::move(datagram));
	socket->Signal(FD_READ);
}

int SendDatagramToUser(CSteamID steamId, uint16_t sourcePort, uint16_t destinationPort, const char* buffer, int length)
{
	if (!SteamReady() || !SteamNetworkingMessages()) {
		return Fail(WSAENETDOWN);
	}
	std::vector<uint8_t> packet(sizeof(DatagramHeader) + length);
	DatagramHeader* header = (DatagramHeader*)packet.data();
	header->kind = kDatagramData;
	header->flags = 0;
	header->sourcePort = sourcePort;
	memcpy(packet.data() + sizeof(DatagramHeader), buffer, length);
	int sendFlags = k_nSteamNetworkingSend_UnreliableNoNagle | k_nSteamNetworkingSend_AutoRestartBrokenSession;
	EResult result = SteamNetworkingMessages()->SendMessageToUser(IdentityOf(steamId), packet.data(), (uint32)packet.size(), sendFlags, destinationPort);
	if (result != k_EResultOK && result != k_EResultNoConnection) {
		XLS_LOG_DEBUG("net: SendMessageToUser to %llu:%u failed (%d).", steamId.ConvertToUint64(), destinationPort, (int)result);
		return Fail(result == k_EResultLimitExceeded ? WSAEMSGSIZE : WSAEHOSTUNREACH);
	}
	return length;
}

void SendControl(CSteamID steamId, const void* data, uint32_t size, bool reliable)
{
	if (!SteamReady() || !SteamNetworkingMessages()) {
		return;
	}
	int flags = (reliable ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_UnreliableNoNagle) | k_nSteamNetworkingSend_AutoRestartBrokenSession;
	SteamNetworkingMessages()->SendMessageToUser(IdentityOf(steamId), data, size, flags, kControlChannel);
}

void HandleQosProbe(CSteamID from, const QosProbe* probe)
{
	QosReply reply = {};
	reply.kind = kQosReplyKind;
	reply.sequence = probe->sequence;
	reply.sentTick = probe->sentTick;
	std::vector<uint8_t> packet(sizeof(QosReply));
	auto listener = g_qosListeners.find(KidToInt(probe->xnkid));
	if (listener == g_qosListeners.end() || !listener->second.enabled) {
		reply.flags = kQosReplyDisabled;
		if (listener != g_qosListeners.end()) {
			listener->second.stats.dwNumProbesReceived++;
		}
		if (probe->sequence == 0) {
			XLS_LOG_DEBUG("qos: %llu probed session %016llx, which has no enabled listener here.", from.ConvertToUint64(), KidToInt(probe->xnkid));
		}
	}
	else {
		QosListener& state = listener->second;
		if (probe->sequence == 0) {
			XLS_LOG_DEBUG("qos: answering %llu for session %016llx with %zu data bytes.", from.ConvertToUint64(), KidToInt(probe->xnkid), state.data.size());
		}
		state.stats.dwNumProbesReceived++;
		state.stats.dwNumDataRequestsReceived++;
		reply.dataSize = (uint16_t)std::min<size_t>(state.data.size(), 0xFFFF);
		packet.resize(sizeof(QosReply) + reply.dataSize);
		memcpy(packet.data() + sizeof(QosReply), state.data.data(), reply.dataSize);
		state.stats.dwNumDataRepliesSent++;
		state.stats.dwNumDataReplyBytesSent += reply.dataSize;
		state.stats.dwNumProbeRepliesSent++;
	}
	memcpy(packet.data(), &reply, sizeof(reply));
	SendControl(from, packet.data(), (uint32_t)packet.size(), false);
}

void HandleQosReply(CSteamID from, const QosReply* reply, size_t size)
{
	DWORD now = GetTickCount();
	for (QosLookup* lookup : g_qosLookups) {
		for (size_t i = 0; i < lookup->targets.size(); i++) {
			QosTarget& target = lookup->targets[i];
			if (target.complete || target.isService || target.steamId != from) {
				continue;
			}
			target.probesReceived++;
			target.rtts.push_back((uint16_t)std::min<DWORD>(now - reply->sentTick, 0xFFFF));
			if (reply->flags & kQosReplyDisabled) {
				target.disabled = true;
			}
			else if (!target.dataReceived && reply->dataSize && size >= sizeof(QosReply) + reply->dataSize) {
				target.data.assign((const uint8_t*)reply + sizeof(QosReply), (const uint8_t*)reply + sizeof(QosReply) + reply->dataSize);
				target.dataReceived = true;
			}
		}
	}
}

void FinishQosTarget(QosLookup* lookup, size_t index)
{
	QosTarget& target = lookup->targets[index];
	if (target.complete) {
		return;
	}
	target.complete = true;
	XNQOSINFO& info = lookup->result->axnqosinfo[index];
	info.bFlags = XNET_XNQOSINFO_COMPLETE;
	info.cProbesXmit = target.probesSent;
	info.cProbesRecv = target.probesReceived;
	if (target.probesReceived || target.isService) {
		info.bFlags |= XNET_XNQOSINFO_TARGET_CONTACTED;
	}
	if (target.disabled) {
		info.bFlags |= XNET_XNQOSINFO_TARGET_DISABLED;
	}
	if (target.dataReceived && !target.data.empty()) {
		info.bFlags |= XNET_XNQOSINFO_DATA_RECEIVED;
		info.cbData = (uint16_t)target.data.size();
		info.pbData = new uint8_t[target.data.size()];
		memcpy(info.pbData, target.data.data(), target.data.size());
	}
	if (!target.rtts.empty()) {
		std::vector<uint16_t> sorted = target.rtts;
		std::sort(sorted.begin(), sorted.end());
		info.wRttMinInMsecs = sorted.front();
		info.wRttMedInMsecs = sorted[sorted.size() / 2];
	}
	else if (target.isService) {
		info.wRttMinInMsecs = 50;
		info.wRttMedInMsecs = 50;
	}
	// Steam relays are not bandwidth probed so report a healthy link.
	info.dwUpBitsPerSec = 4 * 1024 * 1024;
	info.dwDnBitsPerSec = 8 * 1024 * 1024;
	if (!target.isService) {
		XLS_LOG_DEBUG("qos: %llu done, flags 0x%02x, %u of %u probes answered, median %u ms, %u data bytes.", target.steamId.ConvertToUint64(), info.bFlags, info.cProbesRecv, info.cProbesXmit, info.wRttMedInMsecs, info.cbData);
	}
	if (lookup->result->cxnqosPending) {
		lookup->result->cxnqosPending--;
	}
	if (!lookup->result->cxnqosPending && lookup->event) {
		SetEvent(lookup->event);
	}
}

void PumpQos()
{
	DWORD now = GetTickCount();
	ISteamNetworkingMessages* messages = SteamReady() ? SteamNetworkingMessages() : nullptr;
	for (QosLookup* lookup : g_qosLookups) {
		if (!lookup->result->cxnqosPending) {
			continue;
		}
		if (!lookup->probeStartTick) {
			// A probe sent while Steam is still opening the session would time the handshake, not
			// the link, so the first round waits for the sessions, up to kQosWarmupMs.
			bool ready = true;
			for (QosTarget& target : lookup->targets) {
				if (target.isService || target.complete || target.sessionReady) {
					continue;
				}
				SteamNetConnectionInfo_t info = {};
				if (messages && messages->GetSessionConnectionInfo(IdentityOf(target.steamId), &info, nullptr) == k_ESteamNetworkingConnectionState_Connected) {
					target.sessionReady = true;
					continue;
				}
				if (!target.warmSent) {
					DatagramHeader header = { kDatagramData, 1, 0 };
					SendControl(target.steamId, &header, sizeof(header), true);
					target.warmSent = true;
				}
				ready = false;
			}
			if (!ready && now - lookup->startedTick < kQosWarmupMs) {
				continue;
			}
			lookup->probeStartTick = now;
		}
		bool sendRound = lookup->nextSequence < lookup->probeCount && (lookup->nextSequence == 0 || now - lookup->lastProbeTick >= 50);
		for (size_t i = 0; i < lookup->targets.size(); i++) {
			QosTarget& target = lookup->targets[i];
			if (target.complete) {
				continue;
			}
			if (target.isService) {
				FinishQosTarget(lookup, i);
				continue;
			}
			if (sendRound) {
				QosProbe probe = {};
				probe.kind = kQosProbeKind;
				probe.sequence = lookup->nextSequence;
				probe.sentTick = now;
				probe.xnkid = target.xnkid;
				SendControl(target.steamId, &probe, sizeof(probe), false);
				target.probesSent++;
			}
			bool allReplied = target.probesReceived >= lookup->probeCount;
			bool timedOut = now - lookup->probeStartTick > kQosProbeTimeoutMs;
			if (allReplied || timedOut) {
				FinishQosTarget(lookup, i);
			}
		}
		if (sendRound) {
			lookup->nextSequence++;
			lookup->lastProbeTick = now;
		}
	}
}

void PumpControlChannel()
{
	SteamNetworkingMessage_t* messages[32];
	int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(kControlChannel, messages, 32);
	for (int i = 0; i < count; i++) {
		SteamNetworkingMessage_t* message = messages[i];
		CSteamID from = message->m_identityPeer.GetSteamID();
		const uint8_t* data = (const uint8_t*)message->m_pData;
		if (message->m_cbSize >= (int)sizeof(QosProbe) && data[0] == kQosProbeKind) {
			HandleQosProbe(from, (const QosProbe*)data);
		}
		else if (message->m_cbSize >= (int)sizeof(QosReply) && data[0] == kQosReplyKind) {
			HandleQosReply(from, (const QosReply*)data, message->m_cbSize);
		}
		message->Release();
	}
}

void PumpDatagrams()
{
	SteamNetworkingMessage_t* messages[64];
	for (auto& entry : g_dgramByPort) {
		XlsSocket* socket = entry.second;
		int count = SteamNetworkingMessages()->ReceiveMessagesOnChannel(socket->port, messages, 64);
		for (int i = 0; i < count; i++) {
			SteamNetworkingMessage_t* message = messages[i];
			if (message->m_cbSize >= (int)sizeof(DatagramHeader)) {
				const DatagramHeader* header = (const DatagramHeader*)message->m_pData;
				if (header->kind == kDatagramData) {
					CSteamID from = message->m_identityPeer.GetSteamID();
					IN_ADDR alias = NetSecureAddrFor(from);
					DeliverLocal(socket->port, alias, header->sourcePort, (const uint8_t*)message->m_pData + sizeof(DatagramHeader), message->m_cbSize - sizeof(DatagramHeader));
				}
			}
			message->Release();
		}
	}
}

void PumpRealSockets()
{
	for (auto& entry : g_sockets) {
		XlsSocket* socket = entry.second;
		if (socket->real == INVALID_SOCKET || socket->type != SOCK_DGRAM) {
			continue;
		}
		while (true) {
			u_long pending = 0;
			if (ioctlsocket(socket->real, FIONREAD, &pending) != 0 || !pending) {
				break;
			}
			std::vector<uint8_t> buffer(std::max<u_long>(pending, 1500));
			sockaddr_in from = {};
			int fromLength = sizeof(from);
			int received = recvfrom(socket->real, (char*)buffer.data(), (int)buffer.size(), 0, (sockaddr*)&from, &fromLength);
			if (received < 0) {
				break;
			}
			// Map the real sender back to the alias the title knows it by.
			IN_ADDR alias = NetSecureAddrForServer(from.sin_addr, 0);
			buffer.resize(received);
			Datagram datagram;
			datagram.from = alias;
			datagram.fromPort = ntohs(from.sin_port);
			datagram.data = std::move(buffer);
			if (socket->datagrams.size() < kMaxQueuedDatagrams) {
				socket->datagrams.push_back(std::move(datagram));
				socket->Signal(FD_READ);
			}
		}
	}
}

// Moves every message Steam holds for the connection into the socket's stream buffer.
void DrainConnection(HSteamNetConnection connection, XlsSocket* socket)
{
	SteamNetworkingMessage_t* messages[32];
	bool received = false;
	for (;;) {
		int count = SteamNetworkingSockets()->ReceiveMessagesOnConnection(connection, messages, 32);
		for (int i = 0; i < count; i++) {
			SteamNetworkingMessage_t* message = messages[i];
			socket->stream.insert(socket->stream.end(), (const uint8_t*)message->m_pData, (const uint8_t*)message->m_pData + message->m_cbSize);
			message->Release();
		}
		received = received || count > 0;
		if (count < 32) {
			break;
		}
	}
	if (received) {
		socket->Signal(FD_READ);
	}
}

void PumpStreams()
{
	for (auto& entry : g_socketByConnection) {
		if (entry.second->connected) {
			DrainConnection(entry.first, entry.second);
		}
	}
}

bool ParseSockaddr(const sockaddr* name, int length, IN_ADDR* address, uint16_t* port)
{
	if (!name || length < (int)sizeof(sockaddr_in) || name->sa_family != AF_INET) {
		return false;
	}
	const sockaddr_in* in = (const sockaddr_in*)name;
	*address = in->sin_addr;
	*port = ntohs(in->sin_port);
	return true;
}

void FillSockaddr(sockaddr* name, int* length, IN_ADDR address, uint16_t port)
{
	if (!name || !length || *length < (int)sizeof(sockaddr_in)) {
		if (length) {
			*length = sizeof(sockaddr_in);
		}
		return;
	}
	sockaddr_in* in = (sockaddr_in*)name;
	memset(in, 0, sizeof(*in));
	in->sin_family = AF_INET;
	in->sin_addr = address;
	in->sin_port = htons(port);
	*length = sizeof(sockaddr_in);
}

uint16_t AllocateEphemeralPort()
{
	for (int attempts = 0; attempts < 16384; attempts++) {
		uint16_t candidate = g_nextEphemeral++;
		if (g_nextEphemeral == 0) {
			g_nextEphemeral = 49152;
		}
		if (!g_dgramByPort.count(candidate)) {
			return candidate;
		}
	}
	return 0;
}

SOCKET EnsureRealSocket(XlsSocket* socket)
{
	if (socket->real != INVALID_SOCKET) {
		return socket->real;
	}
	socket->real = ::socket(AF_INET, socket->type, socket->type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP);
	if (socket->real == INVALID_SOCKET) {
		return INVALID_SOCKET;
	}
	u_long nonBlocking = 1;
	ioctlsocket(socket->real, FIONBIO, &nonBlocking);
	if (socket->broadcast) {
		BOOL enable = TRUE;
		setsockopt(socket->real, SOL_SOCKET, SO_BROADCAST, (const char*)&enable, sizeof(enable));
	}
	sockaddr_in local = {};
	local.sin_family = AF_INET;
	local.sin_port = htons(socket->bound ? socket->port : 0);
	if (::bind(socket->real, (sockaddr*)&local, sizeof(local)) != 0) {
		// The exact port is taken by another process, any port works for outgoing traffic.
		local.sin_port = 0;
		::bind(socket->real, (sockaddr*)&local, sizeof(local));
	}
	return socket->real;
}

void CloseStream(XlsSocket* socket, bool linger)
{
	if (socket->connection != k_HSteamNetConnection_Invalid) {
		g_socketByConnection.erase(socket->connection);
		if (SteamReady() && SteamNetworkingSockets()) {
			SteamNetworkingSockets()->CloseConnection(socket->connection, 0, "closed", linger);
		}
		socket->connection = k_HSteamNetConnection_Invalid;
	}
	if (socket->listenSocket != k_HSteamListenSocket_Invalid) {
		g_socketByListen.erase(socket->listenSocket);
		if (SteamReady() && SteamNetworkingSockets()) {
			SteamNetworkingSockets()->CloseListenSocket(socket->listenSocket);
		}
		socket->listenSocket = k_HSteamListenSocket_Invalid;
	}
	for (HSteamNetConnection pending : socket->acceptQueue) {
		if (SteamReady() && SteamNetworkingSockets()) {
			SteamNetworkingSockets()->CloseConnection(pending, 0, "listener closed", false);
		}
	}
	socket->acceptQueue.clear();
	socket->connected = false;
	socket->connecting = false;
}

bool WaitCondition(XlsSocket* socket, DWORD timeoutMs, bool (*ready)(XlsSocket*))
{
	DWORD started = GetTickCount();
	while (!ready(socket)) {
		if (timeoutMs && GetTickCount() - started >= timeoutMs) {
			return false;
		}
		if (!Find(socket->handle)) {
			return false;
		}
		SteamPumpForce();
		Sleep(1);
	}
	return true;
}

bool ReadReady(XlsSocket* socket)
{
	if (socket->type == SOCK_DGRAM) {
		return !socket->datagrams.empty();
	}
	if (socket->listenSocket != k_HSteamListenSocket_Invalid) {
		return !socket->acceptQueue.empty();
	}
	if (socket->real != INVALID_SOCKET && socket->type == SOCK_STREAM) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(socket->real, &set);
		timeval zero = {};
		return select(0, &set, nullptr, nullptr, &zero) > 0;
	}
	return !socket->stream.empty() || socket->closedByPeer;
}

bool WriteReady(XlsSocket* socket)
{
	if (socket->type == SOCK_DGRAM) {
		return true;
	}
	if (socket->real != INVALID_SOCKET) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(socket->real, &set);
		timeval zero = {};
		return select(0, nullptr, &set, nullptr, &zero) > 0;
	}
	return socket->connected;
}

bool ExceptReady(XlsSocket* socket)
{
	if (socket->real != INVALID_SOCKET && socket->type == SOCK_STREAM) {
		fd_set set;
		FD_ZERO(&set);
		FD_SET(socket->real, &set);
		timeval zero = {};
		return select(0, nullptr, nullptr, &set, &zero) > 0;
	}
	return socket->connectError != 0;
}

}

// --- Lifecycle -----------------------------------------------------------------------------------

void NetInit()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (g_started) {
		return;
	}
	WSADATA wsaData;
	WSAStartup(MAKEWORD(2, 2), &wsaData);
	if (SteamReady()) {
		BuildXnaddr(SteamLocalId(), &g_localXnaddr);
		g_localXnaddr.ina = LocalLanAddress();
		g_secure[kLocalAlias] = SecureEntry{ false, SteamLocalId(), {}, 0, XNET_CONNECT_STATUS_CONNECTED };
		g_aliasBySteamId[SteamLocalId().ConvertToUint64()] = kLocalAlias;
		if (Cfg().relayOnly && !g_p2pTransportSet) {
			g_p2pTransport = k_nSteamNetworkingConfig_P2P_Transport_ICE_Enable_Disable;
			g_p2pTransportSet = true;
		}
		if (g_p2pTransportSet) {
			ApplyP2PTransport();
		}
		if (Cfg().sendRateKBytes && SteamNetworkingUtils()) {
			SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMin, (int32)(Cfg().sendRateKBytes * 1024));
			SteamNetworkingUtils()->SetGlobalConfigValueInt32(k_ESteamNetworkingConfig_SendRateMax, (int32)(Cfg().sendRateKBytes * 1024));
			XLS_LOG_INFO("net: send rate %u KB/s per peer.", Cfg().sendRateKBytes);
		}
	}
	else {
		memset(&g_localXnaddr, 0, sizeof(g_localXnaddr));
		g_localXnaddr.ina = LocalLanAddress();
		g_localXnaddr.inaOnline = g_localXnaddr.ina;
	}
	g_started = true;
	const uint8_t* lan = (const uint8_t*)&g_localXnaddr.ina.S_un.S_addr;
	XLS_LOG_INFO("net: started with local alias 10.0.0.1 and lan %u.%u.%u.%u.", lan[0], lan[1], lan[2], lan[3]);
}

void NetShutdown()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (!g_started) {
		return;
	}
	for (auto& entry : g_sockets) {
		XlsSocket* socket = entry.second;
		CloseStream(socket, false);
		if (socket->real != INVALID_SOCKET) {
			closesocket(socket->real);
		}
		delete socket;
	}
	g_sockets.clear();
	g_dgramByPort.clear();
	g_socketByConnection.clear();
	g_socketByListen.clear();
	for (QosLookup* lookup : g_qosLookups) {
		delete lookup;
	}
	g_qosLookups.clear();
	g_qosListeners.clear();
	g_keys.clear();
	// Sessions left open would still be signalling through the Steam pipe after SteamAPI_Shutdown.
	if (SteamReady() && SteamNetworkingMessages()) {
		for (const auto& entry : g_aliasBySteamId) {
			if (entry.second != kLocalAlias) {
				SteamNetworkingMessages()->CloseSessionWithUser(IdentityOf(CSteamID(entry.first)));
			}
		}
	}
	g_secure.clear();
	g_aliasBySteamId.clear();
	g_aliasByServer.clear();
	g_started = false;
	WSACleanup();
}

void NetPump()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (!g_started) {
		return;
	}
	if (SteamReady() && SteamNetworkingMessages()) {
		PumpControlChannel();
		PumpDatagrams();
		PumpStreams();
	}
	PumpRealSockets();
	PumpQos();
}

// --- Addresses -----------------------------------------------------------------------------------

void NetLocalXnaddr(XNADDR* out)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	*out = g_localXnaddr;
}

void NetXnaddrForSteamId(CSteamID steamId, XNADDR* out)
{
	if (steamId == SteamLocalId()) {
		NetLocalXnaddr(out);
		return;
	}
	BuildXnaddr(steamId, out);
}

bool NetSteamIdFromXnaddr(const XNADDR& xnaddr, CSteamID* out)
{
	uint32_t magic = 0;
	memcpy(&magic, xnaddr.abOnline + 8, sizeof(magic));
	if (magic != kXnaddrMagic) {
		return false;
	}
	uint64_t id64 = 0;
	memcpy(&id64, xnaddr.abOnline, sizeof(id64));
	CSteamID steamId(id64);
	if (!steamId.IsValid() || !steamId.BIndividualAccount()) {
		return false;
	}
	*out = steamId;
	return true;
}

IN_ADDR NetSecureAddrFor(CSteamID steamId)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto existing = g_aliasBySteamId.find(steamId.ConvertToUint64());
	if (existing != g_aliasBySteamId.end()) {
		return AliasToInAddr(existing->second);
	}
	uint32_t alias = g_nextAlias++;
	SecureEntry entry;
	entry.steamId = steamId;
	g_secure[alias] = entry;
	g_hadPeers = true;
	g_aliasBySteamId[steamId.ConvertToUint64()] = alias;
	XLS_LOG_DEBUG("net: alias %u.%u.%u.%u -> %llu.", alias >> 24, (alias >> 16) & 0xFF, (alias >> 8) & 0xFF, alias & 0xFF, steamId.ConvertToUint64());
	return AliasToInAddr(alias);
}

IN_ADDR NetSecureAddrForServer(IN_ADDR realAddress, DWORD serviceId)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	uint64_t key = ((uint64_t)realAddress.S_un.S_addr << 32) | serviceId;
	auto existing = g_aliasByServer.find(key);
	if (existing != g_aliasByServer.end()) {
		return AliasToInAddr(existing->second);
	}
	uint32_t alias = g_nextAlias++;
	SecureEntry entry;
	entry.isServer = true;
	entry.realAddress = realAddress;
	entry.serviceId = serviceId;
	entry.connectStatus = XNET_CONNECT_STATUS_CONNECTED;
	g_secure[alias] = entry;
	g_aliasByServer[key] = alias;
	return AliasToInAddr(alias);
}

bool NetSecureAddrIsKnown(IN_ADDR alias)
{
	SecureEntry entry;
	return ResolveAlias(alias, &entry);
}

bool NetSecureAddrToSteamId(IN_ADDR alias, CSteamID* out)
{
	SecureEntry entry;
	if (!ResolveAlias(alias, &entry) || entry.isServer) {
		return false;
	}
	*out = entry.steamId;
	return true;
}

bool NetSecureAddrToServer(IN_ADDR alias, IN_ADDR* realAddress)
{
	SecureEntry entry;
	if (!ResolveAlias(alias, &entry) || !entry.isServer) {
		return false;
	}
	*realAddress = entry.realAddress;
	return true;
}

void NetSecureAddrRelease(IN_ADDR alias)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	uint32_t key = AliasOf(alias);
	if (key == kLocalAlias) {
		return;
	}
	auto it = g_secure.find(key);
	if (it == g_secure.end()) {
		return;
	}
	if (it->second.isServer) {
		g_aliasByServer.erase(((uint64_t)it->second.realAddress.S_un.S_addr << 32) | it->second.serviceId);
	}
	else {
		g_aliasBySteamId.erase(it->second.steamId.ConvertToUint64());
		if (SteamReady() && SteamNetworkingMessages()) {
			SteamNetworkingMessages()->CloseSessionWithUser(IdentityOf(it->second.steamId));
		}
	}
	g_secure.erase(it);
}

DWORD NetConnectStatus(IN_ADDR alias)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_secure.find(AliasOf(alias));
	if (it == g_secure.end()) {
		return XNET_CONNECT_STATUS_LOST;
	}
	if (it->second.isServer || AliasOf(alias) == kLocalAlias) {
		return XNET_CONNECT_STATUS_CONNECTED;
	}
	if (!SteamReady() || !SteamNetworkingMessages()) {
		return XNET_CONNECT_STATUS_LOST;
	}
	SteamNetConnectionInfo_t info = {};
	ESteamNetworkingConnectionState state = SteamNetworkingMessages()->GetSessionConnectionInfo(IdentityOf(it->second.steamId), &info, nullptr);
	switch (state) {
		case k_ESteamNetworkingConnectionState_Connected:
			it->second.connectStatus = XNET_CONNECT_STATUS_CONNECTED;
			break;
		case k_ESteamNetworkingConnectionState_Connecting:
		case k_ESteamNetworkingConnectionState_FindingRoute:
			it->second.connectStatus = XNET_CONNECT_STATUS_PENDING;
			break;
		case k_ESteamNetworkingConnectionState_None:
			// No session yet: a title that only checks status before its first send would wait
			// forever, so report it as pending and let the first send open the session.
			if (it->second.connectStatus == XNET_CONNECT_STATUS_IDLE) {
				break;
			}
			if (it->second.connectStatus == XNET_CONNECT_STATUS_CONNECTED) {
				it->second.connectStatus = XNET_CONNECT_STATUS_LOST;
			}
			break;
		default:
			it->second.connectStatus = XNET_CONNECT_STATUS_LOST;
			break;
	}
	return it->second.connectStatus;
}

void NetConnectStart(IN_ADDR alias)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_secure.find(AliasOf(alias));
	if (it == g_secure.end() || it->second.isServer) {
		return;
	}
	if (it->second.connectStatus == XNET_CONNECT_STATUS_IDLE || it->second.connectStatus == XNET_CONNECT_STATUS_LOST) {
		it->second.connectStatus = XNET_CONNECT_STATUS_PENDING;
	}
	// An empty control datagram opens the Steam session. The reply path accepts it on the other
	// side and the connection state then reads as connected.
	DatagramHeader header = { kDatagramData, 1, 0 };
	SendControl(it->second.steamId, &header, sizeof(header), true);
}

bool NetIsLocalAlias(IN_ADDR alias)
{
	uint32_t value = AliasOf(alias);
	return value == kLocalAlias || value == 0x7F000001 || alias.S_un.S_addr == g_localXnaddr.ina.S_un.S_addr;
}

// --- Keys ----------------------------------------------------------------------------------------

void NetCreateKey(XNKID* xnkid, XNKEY* xnkey)
{
	uint8_t random[24];
	for (uint8_t& byte : random) {
		byte = (uint8_t)rand();
	}
	uint64_t time = ((uint64_t)GetTickCount64() << 20) ^ (uint64_t)GetCurrentProcessId();
	memcpy(random, &time, sizeof(time));
	if (xnkid) {
		memcpy(xnkid->ab, random, sizeof(xnkid->ab));
		xnkid->ab[0] = (xnkid->ab[0] & 0x1F) | XNET_XNKID_ONLINE_PEER;
	}
	if (xnkey) {
		memcpy(xnkey->ab, random + 8, sizeof(xnkey->ab));
	}
}

bool NetRegisterKey(const XNKID& xnkid, const XNKEY& xnkey)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	uint64_t key = KidToInt(xnkid);
	if (g_keys.count(key)) {
		return false;
	}
	g_keys[key] = xnkey;
	return true;
}

bool NetUnregisterKey(const XNKID& xnkid)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	uint64_t key = KidToInt(xnkid);
	g_qosListeners.erase(key);
	return g_keys.erase(key) > 0;
}

bool NetReplaceKey(const XNKID& oldKey, const XNKID& newKey)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_keys.find(KidToInt(oldKey));
	if (it == g_keys.end()) {
		return false;
	}
	XNKEY value = it->second;
	g_keys.erase(it);
	g_keys[KidToInt(newKey)] = value;
	auto listener = g_qosListeners.find(KidToInt(oldKey));
	if (listener != g_qosListeners.end()) {
		g_qosListeners[KidToInt(newKey)] = listener->second;
		g_qosListeners.erase(listener);
	}
	return true;
}

bool NetKeyRegistered(const XNKID& xnkid)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return g_keys.count(KidToInt(xnkid)) > 0;
}

// --- QoS -----------------------------------------------------------------------------------------

INT NetQosListen(const XNKID& xnkid, const uint8_t* data, UINT size, DWORD bitsPerSecond, DWORD flags)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	uint64_t key = KidToInt(xnkid);
	if (flags & XNET_QOS_LISTEN_RELEASE) {
		XLS_LOG_DEBUG("qos: listener for session %016llx released.", key);
		g_qosListeners.erase(key);
		return 0;
	}
	QosListener& listener = g_qosListeners[key];
	if ((flags & (XNET_QOS_LISTEN_ENABLE | XNET_QOS_LISTEN_DISABLE)) || ((flags & XNET_QOS_LISTEN_SET_DATA) && listener.data.size() != (data ? size : 0))) {
		XLS_LOG_DEBUG("qos: listener for session %016llx: flags 0x%x, %u data bytes.", key, flags, data ? size : 0);
	}
	if (flags & XNET_QOS_LISTEN_SET_DATA) {
		listener.data.assign(data, data + (data ? size : 0));
	}
	if (flags & XNET_QOS_LISTEN_SET_BITSPERSEC) {
		listener.bitsPerSecond = bitsPerSecond;
	}
	if (flags & XNET_QOS_LISTEN_ENABLE) {
		listener.enabled = true;
	}
	if (flags & XNET_QOS_LISTEN_DISABLE) {
		listener.enabled = false;
	}
	return 0;
}

INT NetQosLookup(UINT count, const XNADDR* xnaddrs[], const XNKID* xnkids[], const XNKEY* xnkeys[], UINT serviceCount, const IN_ADDR services[], const DWORD serviceIds[], UINT probes, DWORD bitsPerSecond, DWORD flags, WSAEVENT event, XNQOS** out)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	UINT total = count + serviceCount;
	if (!total || !out) {
		return WSAEINVAL;
	}
	size_t bytes = sizeof(XNQOS) + (total - 1) * sizeof(XNQOSINFO);
	XNQOS* result = (XNQOS*)calloc(1, bytes);
	if (!result) {
		return WSAENOBUFS;
	}
	result->cxnqos = total;
	result->cxnqosPending = total;

	QosLookup* lookup = new QosLookup();
	lookup->result = result;
	lookup->event = event;
	lookup->startedTick = GetTickCount();
	lookup->probeCount = (uint8_t)std::min<UINT>(probes ? probes : kQosDefaultProbes, 32);
	for (UINT i = 0; i < count; i++) {
		QosTarget target;
		CSteamID steamId;
		if (xnaddrs && xnaddrs[i] && NetSteamIdFromXnaddr(*xnaddrs[i], &steamId)) {
			target.steamId = steamId;
		}
		if (xnkids && xnkids[i]) {
			target.xnkid = *xnkids[i];
		}
		if (!target.steamId.IsValid()) {
			// Not one of ours, finish it as unreachable right away.
			target.complete = false;
			target.disabled = true;
			XLS_LOG_DEBUG("qos: target %u is not a Steam peer, reported as unreachable.", i);
		}
		else {
			XLS_LOG_DEBUG("qos: probing %llu for session %016llx, %u probes.", target.steamId.ConvertToUint64(), KidToInt(target.xnkid), lookup->probeCount);
		}
		lookup->targets.push_back(target);
	}
	for (UINT i = 0; i < serviceCount; i++) {
		QosTarget target;
		target.isService = true;
		target.serviceAddress = services ? services[i] : IN_ADDR{};
		lookup->targets.push_back(target);
	}
	g_qosLookups.push_back(lookup);
	for (size_t i = 0; i < lookup->targets.size(); i++) {
		if (!lookup->targets[i].isService && !lookup->targets[i].steamId.IsValid()) {
			FinishQosTarget(lookup, i);
		}
	}
	*out = result;
	return 0;
}

INT NetQosRelease(XNQOS* qos)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (auto it = g_qosLookups.begin(); it != g_qosLookups.end(); ++it) {
		if ((*it)->result == qos) {
			for (UINT i = 0; i < qos->cxnqos; i++) {
				delete[] qos->axnqosinfo[i].pbData;
			}
			delete *it;
			g_qosLookups.erase(it);
			free(qos);
			return 0;
		}
	}
	return WSAEINVAL;
}

INT NetQosListenStats(const XNKID& xnkid, XNQOSLISTENSTATS* stats)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_qosListeners.find(KidToInt(xnkid));
	if (it == g_qosListeners.end()) {
		return WSAEINVAL;
	}
	DWORD size = stats->dwSizeOfStruct;
	*stats = it->second.stats;
	stats->dwSizeOfStruct = size;
	return 0;
}

// --- Sockets -------------------------------------------------------------------------------------

SOCKET NetSocketCreate(int af, int type, int protocol)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (af != AF_INET || (type != SOCK_DGRAM && type != SOCK_STREAM)) {
		WSASetLastError(WSAEAFNOSUPPORT);
		return INVALID_SOCKET;
	}
	XlsSocket* socket = new XlsSocket();
	socket->handle = g_nextHandle++;
	socket->type = type;
	socket->protocol = protocol == IPPROTO_VDP ? IPPROTO_UDP : (protocol ? protocol : (type == SOCK_DGRAM ? IPPROTO_UDP : IPPROTO_TCP));
	g_sockets[socket->handle] = socket;
	XLS_LOG_DEBUG("net: socket %u created (%s%s).", (unsigned)socket->handle, type == SOCK_DGRAM ? "dgram" : "stream", protocol == IPPROTO_VDP ? ", vdp" : "");
	return socket->handle;
}

int NetSocketClose(SOCKET s)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (socket->bound && socket->type == SOCK_DGRAM) {
		g_dgramByPort.erase(socket->port);
	}
	CloseStream(socket, true);
	if (socket->real != INVALID_SOCKET) {
		closesocket(socket->real);
	}
	g_sockets.erase(s);
	delete socket;
	return 0;
}

int NetSocketShutdown(SOCKET s, int how)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (socket->real != INVALID_SOCKET) {
		shutdown(socket->real, how);
	}
	if (socket->connection != k_HSteamNetConnection_Invalid && how != SD_RECEIVE) {
		SteamNetworkingSockets()->FlushMessagesOnConnection(socket->connection);
	}
	return 0;
}

int NetSocketIoctl(SOCKET s, long command, u_long* argument)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (!argument) {
		return Fail(WSAEFAULT);
	}
	switch (command) {
		case FIONBIO:
			socket->nonBlocking = *argument != 0;
			return 0;
		case FIONREAD:
			if (socket->type == SOCK_DGRAM) {
				*argument = socket->datagrams.empty() ? 0 : (u_long)socket->datagrams.front().data.size();
			}
			else if (socket->real != INVALID_SOCKET) {
				return ioctlsocket(socket->real, command, argument);
			}
			else {
				*argument = (u_long)socket->stream.size();
			}
			return 0;
		default:
			return 0;
	}
}

int NetSocketSetOpt(SOCKET s, int level, int name, const char* value, int length)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (!value || length <= 0) {
		return Fail(WSAEFAULT);
	}
	if (level == SOL_SOCKET) {
		switch (name) {
			case SO_BROADCAST: socket->broadcast = *(const BOOL*)value != 0; break;
			case SO_RCVBUF: socket->receiveBufferSize = *(const DWORD*)value; break;
			case SO_SNDBUF: socket->sendBufferSize = *(const DWORD*)value; break;
			case SO_RCVTIMEO: socket->receiveTimeoutMs = *(const DWORD*)value; break;
			case SO_SNDTIMEO: socket->sendTimeoutMs = *(const DWORD*)value; break;
			default: break;
		}
	}
	else if (level == IPPROTO_TCP && name == TCP_NODELAY) {
		socket->noDelay = *(const BOOL*)value != 0;
	}
	if (socket->real != INVALID_SOCKET) {
		setsockopt(socket->real, level, name, value, length);
	}
	return 0;
}

int NetSocketGetOpt(SOCKET s, int level, int name, char* value, int* length)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (!value || !length || *length < (int)sizeof(DWORD)) {
		return Fail(WSAEFAULT);
	}
	DWORD result = 0;
	if (level == SOL_SOCKET) {
		switch (name) {
			case SO_BROADCAST: result = socket->broadcast; break;
			case SO_RCVBUF: result = socket->receiveBufferSize; break;
			case SO_SNDBUF: result = socket->sendBufferSize; break;
			case SO_RCVTIMEO: result = socket->receiveTimeoutMs; break;
			case SO_SNDTIMEO: result = socket->sendTimeoutMs; break;
			case SO_TYPE: result = socket->type; break;
			case SO_ERROR: result = socket->connectError; socket->connectError = 0; break;
			case SO_ACCEPTCONN: result = socket->listenSocket != k_HSteamListenSocket_Invalid; break;
			default: break;
		}
	}
	memcpy(value, &result, sizeof(result));
	*length = sizeof(result);
	return 0;
}

int NetSocketGetSockName(SOCKET s, sockaddr* name, int* length)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (!name || !length || *length < (int)sizeof(sockaddr_in)) {
		return Fail(WSAEFAULT);
	}
	FillSockaddr(name, length, g_localXnaddr.ina, socket->port);
	return 0;
}

int NetSocketGetPeerName(SOCKET s, sockaddr* name, int* length)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (!socket->connected) {
		return Fail(WSAENOTCONN);
	}
	FillSockaddr(name, length, socket->peerAlias, socket->peerPort);
	return 0;
}

int NetSocketBind(SOCKET s, const sockaddr* name, int length)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	IN_ADDR address;
	uint16_t port;
	if (!ParseSockaddr(name, length, &address, &port)) {
		return Fail(WSAEFAULT);
	}
	if (socket->bound) {
		return Fail(WSAEINVAL);
	}
	if (socket->type == SOCK_DGRAM) {
		if (!port) {
			port = AllocateEphemeralPort();
		}
		if (!port || g_dgramByPort.count(port)) {
			return Fail(WSAEADDRINUSE);
		}
		g_dgramByPort[port] = socket;
	}
	socket->port = port;
	socket->bound = true;
	XLS_LOG_DEBUG("net: socket %u bound to port %u.", (unsigned)s, port);
	return 0;
}

int NetSocketConnect(SOCKET s, const sockaddr* name, int length)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	IN_ADDR address;
	uint16_t port;
	if (!ParseSockaddr(name, length, &address, &port)) {
		return Fail(WSAEFAULT);
	}
	SecureEntry entry;
	bool known = ResolveAlias(address, &entry);

	if (socket->type == SOCK_DGRAM) {
		// A connected datagram socket just remembers its default destination.
		socket->peerAlias = address;
		socket->peerPort = port;
		socket->connected = true;
		if (known && !entry.isServer) {
			socket->peer = entry.steamId;
		}
		return 0;
	}

	if (socket->connected) {
		return Fail(WSAEISCONN);
	}
	if (socket->connecting) {
		return Fail(WSAEALREADY);
	}

	if (!known || entry.isServer) {
		// Real address (title server or LAN): Winsock does the work.
		SOCKET real = EnsureRealSocket(socket);
		if (real == INVALID_SOCKET) {
			return Fail(WSAENETDOWN);
		}
		sockaddr_in target = {};
		target.sin_family = AF_INET;
		target.sin_addr = known ? entry.realAddress : address;
		target.sin_port = htons(port);
		int rc = ::connect(real, (sockaddr*)&target, sizeof(target));
		socket->peerAlias = address;
		socket->peerPort = port;
		if (rc == 0) {
			socket->connected = true;
			return 0;
		}
		int error = WSAGetLastError();
		if (error == WSAEWOULDBLOCK) {
			socket->connecting = true;
			if (socket->nonBlocking) {
				return Fail(WSAEWOULDBLOCK);
			}
			fd_set writeSet;
			FD_ZERO(&writeSet);
			FD_SET(real, &writeSet);
			timeval timeout = { 20, 0 };
			if (select(0, nullptr, &writeSet, nullptr, &timeout) > 0) {
				socket->connecting = false;
				socket->connected = true;
				return 0;
			}
			socket->connecting = false;
			return Fail(WSAETIMEDOUT);
		}
		return Fail(error);
	}

	if (!SteamReady() || !SteamNetworkingSockets()) {
		return Fail(WSAENETDOWN);
	}
	socket->peer = entry.steamId;
	socket->peerAlias = address;
	socket->peerPort = port;
	socket->connection = SteamNetworkingSockets()->ConnectP2P(IdentityOf(entry.steamId), port, 0, nullptr);
	if (socket->connection == k_HSteamNetConnection_Invalid) {
		return Fail(WSAEHOSTUNREACH);
	}
	g_socketByConnection[socket->connection] = socket;
	socket->connecting = true;
	XLS_LOG_DEBUG("net: socket %u connecting to %llu:%u.", (unsigned)s, entry.steamId.ConvertToUint64(), port);
	if (socket->nonBlocking) {
		return Fail(WSAEWOULDBLOCK);
	}
	if (!WaitCondition(socket, 20000, [](XlsSocket* candidate) { return candidate->connected || candidate->connectError != 0; })) {
		return Fail(WSAETIMEDOUT);
	}
	if (socket->connectError) {
		return Fail(socket->connectError);
	}
	return 0;
}

int NetSocketListen(SOCKET s, int backlog)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (socket->type != SOCK_STREAM) {
		return Fail(WSAEOPNOTSUPP);
	}
	if (!socket->bound) {
		return Fail(WSAEINVAL);
	}
	if (!SteamReady() || !SteamNetworkingSockets()) {
		return Fail(WSAENETDOWN);
	}
	if (socket->listenSocket != k_HSteamListenSocket_Invalid) {
		return 0;
	}
	socket->listenSocket = SteamNetworkingSockets()->CreateListenSocketP2P(socket->port, 0, nullptr);
	if (socket->listenSocket == k_HSteamListenSocket_Invalid) {
		return Fail(WSAEADDRINUSE);
	}
	g_socketByListen[socket->listenSocket] = socket;
	XLS_LOG_DEBUG("net: socket %u listening on virtual port %u.", (unsigned)s, socket->port);
	return 0;
}

SOCKET NetSocketAccept(SOCKET s, sockaddr* address, int* length)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		WSASetLastError(WSAENOTSOCK);
		return INVALID_SOCKET;
	}
	if (socket->listenSocket == k_HSteamListenSocket_Invalid) {
		WSASetLastError(WSAEINVAL);
		return INVALID_SOCKET;
	}
	if (socket->acceptQueue.empty()) {
		if (socket->nonBlocking) {
			WSASetLastError(WSAEWOULDBLOCK);
			return INVALID_SOCKET;
		}
		if (!WaitCondition(socket, 0, [](XlsSocket* candidate) { return !candidate->acceptQueue.empty(); })) {
			WSASetLastError(WSAEINTR);
			return INVALID_SOCKET;
		}
	}
	HSteamNetConnection connection = socket->acceptQueue.front();
	socket->acceptQueue.pop_front();

	SteamNetConnectionInfo_t info = {};
	SteamNetworkingSockets()->GetConnectionInfo(connection, &info);

	XlsSocket* accepted = new XlsSocket();
	accepted->handle = g_nextHandle++;
	accepted->type = SOCK_STREAM;
	accepted->protocol = IPPROTO_TCP;
	accepted->bound = true;
	accepted->port = socket->port;
	accepted->nonBlocking = socket->nonBlocking;
	accepted->connection = connection;
	accepted->connected = true;
	accepted->peer = info.m_identityRemote.GetSteamID();
	accepted->peerAlias = NetSecureAddrFor(accepted->peer);
	accepted->peerPort = socket->port;
	g_sockets[accepted->handle] = accepted;
	g_socketByConnection[connection] = accepted;
	FillSockaddr(address, length, accepted->peerAlias, accepted->peerPort);
	XLS_LOG_DEBUG("net: socket %u accepted %llu as socket %u.", (unsigned)s, accepted->peer.ConvertToUint64(), (unsigned)accepted->handle);
	return accepted->handle;
}

int NetSocketSelect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const timeval* timeout)
{
	DWORD limit = timeout ? (DWORD)(timeout->tv_sec * 1000 + timeout->tv_usec / 1000) : INFINITE;
	DWORD started = GetTickCount();
	while (true) {
		int ready = 0;
		{
			std::lock_guard<std::recursive_mutex> lock(g_mutex);
			auto scan = [&](fd_set* set, bool (*test)(XlsSocket*)) {
				if (!set) {
					return;
				}
				fd_set result;
				FD_ZERO(&result);
				for (u_int i = 0; i < set->fd_count; i++) {
					XlsSocket* socket = Find(set->fd_array[i]);
					if (socket && test(socket)) {
						FD_SET(set->fd_array[i], &result);
						ready++;
					}
				}
				*set = result;
			};
			fd_set readCopy = {};
			fd_set writeCopy = {};
			fd_set exceptCopy = {};
			if (readfds) readCopy = *readfds;
			if (writefds) writeCopy = *writefds;
			if (exceptfds) exceptCopy = *exceptfds;
			scan(readfds, ReadReady);
			scan(writefds, WriteReady);
			scan(exceptfds, ExceptReady);
			if (ready || limit == 0) {
				return ready;
			}
			if (readfds) *readfds = readCopy;
			if (writefds) *writefds = writeCopy;
			if (exceptfds) *exceptfds = exceptCopy;
		}
		if (limit != INFINITE && GetTickCount() - started >= limit) {
			std::lock_guard<std::recursive_mutex> lock(g_mutex);
			if (readfds) FD_ZERO(readfds);
			if (writefds) FD_ZERO(writefds);
			if (exceptfds) FD_ZERO(exceptfds);
			return 0;
		}
		SteamPumpForce();
		Sleep(1);
	}
}

int NetSocketRecvFrom(SOCKET s, char* buffer, int length, int flags, sockaddr* from, int* fromLength)
{
	std::unique_lock<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (!buffer || length <= 0) {
		return Fail(WSAEFAULT);
	}
	if (socket->type == SOCK_STREAM) {
		lock.unlock();
		return NetSocketRecv(s, buffer, length, flags);
	}
	if (socket->datagrams.empty()) {
		SteamPump();
	}
	if (socket->datagrams.empty()) {
		if (socket->nonBlocking) {
			return Fail(WSAEWOULDBLOCK);
		}
		lock.unlock();
		if (!WaitCondition(socket, socket->receiveTimeoutMs, [](XlsSocket* candidate) { return !candidate->datagrams.empty(); })) {
			return Fail(WSAETIMEDOUT);
		}
		lock.lock();
		if (!Find(s) || socket->datagrams.empty()) {
			return Fail(WSAEINTR);
		}
	}
	Datagram& datagram = socket->datagrams.front();
	int copied = (int)std::min<size_t>(datagram.data.size(), (size_t)length);
	memcpy(buffer, datagram.data.data(), copied);
	FillSockaddr(from, fromLength, datagram.from, datagram.fromPort);
	bool truncated = datagram.data.size() > (size_t)length;
	if (!(flags & MSG_PEEK)) {
		socket->datagrams.pop_front();
	}
	if (truncated) {
		return Fail(WSAEMSGSIZE);
	}
	return copied;
}

int NetSocketRecv(SOCKET s, char* buffer, int length, int flags)
{
	std::unique_lock<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (socket->type == SOCK_DGRAM) {
		lock.unlock();
		return NetSocketRecvFrom(s, buffer, length, flags, nullptr, nullptr);
	}
	if (socket->real != INVALID_SOCKET) {
		int received = recv(socket->real, buffer, length, flags);
		if (received == SOCKET_ERROR && WSAGetLastError() == WSAEWOULDBLOCK && !socket->nonBlocking) {
			lock.unlock();
			WaitCondition(socket, socket->receiveTimeoutMs, ReadReady);
			lock.lock();
			received = recv(socket->real, buffer, length, flags);
		}
		return received;
	}
	if (!socket->connected && !socket->closedByPeer) {
		return Fail(WSAENOTCONN);
	}
	if (socket->stream.empty()) {
		SteamPump();
	}
	if (socket->stream.empty()) {
		if (socket->closedByPeer) {
			return 0;
		}
		if (socket->nonBlocking) {
			return Fail(WSAEWOULDBLOCK);
		}
		lock.unlock();
		if (!WaitCondition(socket, socket->receiveTimeoutMs, [](XlsSocket* candidate) { return !candidate->stream.empty() || candidate->closedByPeer; })) {
			return Fail(WSAETIMEDOUT);
		}
		lock.lock();
		if (!Find(s)) {
			return Fail(WSAEINTR);
		}
		if (socket->stream.empty()) {
			return 0;
		}
	}
	int copied = (int)std::min<size_t>(socket->stream.size(), (size_t)length);
	memcpy(buffer, socket->stream.data(), copied);
	if (!(flags & MSG_PEEK)) {
		socket->stream.erase(socket->stream.begin(), socket->stream.begin() + copied);
	}
	return copied;
}

int NetSocketSendTo(SOCKET s, const char* buffer, int length, int flags, const sockaddr* to, int toLength)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (!buffer || length < 0) {
		return Fail(WSAEFAULT);
	}
	if (socket->type == SOCK_STREAM) {
		return NetSocketSend(s, buffer, length, flags);
	}
	IN_ADDR address;
	uint16_t port;
	if (!to) {
		if (!socket->connected) {
			return Fail(WSAENOTCONN);
		}
		address = socket->peerAlias;
		port = socket->peerPort;
	}
	else if (!ParseSockaddr(to, toLength, &address, &port)) {
		return Fail(WSAEFAULT);
	}
	if (!socket->bound) {
		// Sending binds to an ephemeral port, as Winsock does.
		sockaddr_in any = {};
		any.sin_family = AF_INET;
		if (NetSocketBind(s, (sockaddr*)&any, sizeof(any)) != 0) {
			return SOCKET_ERROR;
		}
	}

	uint32_t alias = AliasOf(address);
	if (alias == INADDR_BROADCAST || (alias & 0xFF) == 0xFF) {
		// Broadcast reaches every peer this machine has an alias for, and the local machine.
		DeliverLocal(port, AliasToInAddr(kLocalAlias), socket->port, (const uint8_t*)buffer, length);
		for (const auto& entry : g_secure) {
			if (!entry.second.isServer && entry.first != kLocalAlias) {
				SendDatagramToUser(entry.second.steamId, socket->port, port, buffer, length);
			}
		}
		return length;
	}
	if (NetIsLocalAlias(address)) {
		DeliverLocal(port, AliasToInAddr(kLocalAlias), socket->port, (const uint8_t*)buffer, length);
		return length;
	}
	SecureEntry entry;
	if (!ResolveAlias(address, &entry)) {
		// A real IP the title got from somewhere else (a LAN address, a DNS lookup).
		SOCKET real = EnsureRealSocket(socket);
		if (real == INVALID_SOCKET) {
			return Fail(WSAENETDOWN);
		}
		sockaddr_in target = {};
		target.sin_family = AF_INET;
		target.sin_addr = address;
		target.sin_port = htons(port);
		return ::sendto(real, buffer, length, 0, (sockaddr*)&target, sizeof(target));
	}
	if (entry.isServer) {
		SOCKET real = EnsureRealSocket(socket);
		if (real == INVALID_SOCKET) {
			return Fail(WSAENETDOWN);
		}
		sockaddr_in target = {};
		target.sin_family = AF_INET;
		target.sin_addr = entry.realAddress;
		target.sin_port = htons(port);
		return ::sendto(real, buffer, length, 0, (sockaddr*)&target, sizeof(target));
	}
	return SendDatagramToUser(entry.steamId, socket->port, port, buffer, length);
}

int NetSocketSend(SOCKET s, const char* buffer, int length, int flags)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	if (socket->type == SOCK_DGRAM) {
		return NetSocketSendTo(s, buffer, length, flags, nullptr, 0);
	}
	if (socket->real != INVALID_SOCKET) {
		return ::send(socket->real, buffer, length, flags);
	}
	if (!socket->connected) {
		return Fail(socket->connecting ? WSAEWOULDBLOCK : WSAENOTCONN);
	}
	if (length == 0) {
		return 0;
	}
	int sendFlags = socket->noDelay ? k_nSteamNetworkingSend_ReliableNoNagle : k_nSteamNetworkingSend_Reliable;
	EResult result = SteamNetworkingSockets()->SendMessageToConnection(socket->connection, buffer, (uint32)length, sendFlags, nullptr);
	if (result == k_EResultOK) {
		return length;
	}
	if (result == k_EResultLimitExceeded) {
		return Fail(WSAEWOULDBLOCK);
	}
	return Fail(WSAECONNRESET);
}

int NetSocketEventSelect(SOCKET s, WSAEVENT event, long events)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket) {
		return Fail(WSAENOTSOCK);
	}
	socket->event = event;
	socket->eventMask = events;
	socket->nonBlocking = true;
	if (event && ((events & FD_READ) && ReadReady(socket))) {
		SetEvent(event);
	}
	return 0;
}

bool NetSocketIsValid(SOCKET s)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return Find(s) != nullptr;
}

// --- Diagnostics ---------------------------------------------------------------------------------

bool NetHadPeers()
{
	return g_hadPeers;
}

void NetSetP2PTransport(int iceEnable)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_p2pTransport = iceEnable;
	g_p2pTransportSet = true;
	ApplyP2PTransport();
}

bool NetPeerConnectionInfo(IN_ADDR alias, XLS_CONNECTION_INFO* out)
{
	SecureEntry entry;
	if (!ResolveAlias(alias, &entry) || entry.isServer || !SteamReady() || !SteamNetworkingMessages()) {
		return false;
	}
	SteamNetConnectionInfo_t info = {};
	SteamNetConnectionRealTimeStatus_t status = {};
	ESteamNetworkingConnectionState state = SteamNetworkingMessages()->GetSessionConnectionInfo(IdentityOf(entry.steamId), &info, &status);
	if (state == k_ESteamNetworkingConnectionState_None) {
		return false;
	}
	FillConnectionInfo(info, &status, out);
	return true;
}

bool NetSocketConnectionInfo(SOCKET s, XLS_CONNECTION_INFO* out)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	XlsSocket* socket = Find(s);
	if (!socket || socket->connection == k_HSteamNetConnection_Invalid || !SteamReady() || !SteamNetworkingSockets()) {
		return false;
	}
	SteamNetConnectionInfo_t info = {};
	if (!SteamNetworkingSockets()->GetConnectionInfo(socket->connection, &info)) {
		return false;
	}
	SteamNetConnectionRealTimeStatus_t status = {};
	bool haveStatus = SteamNetworkingSockets()->GetConnectionRealTimeStatus(socket->connection, &status, 0, nullptr) == k_EResultOK;
	FillConnectionInfo(info, haveStatus ? &status : nullptr, out);
	char detail[2048] = {};
	if (SteamNetworkingSockets()->GetDetailedConnectionStatus(socket->connection, detail, sizeof(detail)) == 0) {
		XLS_LOG_DEBUG("net: socket %u status:\n%s", (unsigned)s, detail);
	}
	return true;
}

// --- Steam events --------------------------------------------------------------------------------

namespace events {

void OnNetSessionRequest(const SteamNetworkingIdentity& remote)
{
	if (!SteamReady() || !SteamNetworkingMessages()) {
		return;
	}
	CSteamID steamId = remote.GetSteamID();
	bool accept = Cfg().sessionsAcceptAnyPeer;
	if (!accept) {
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		accept = g_aliasBySteamId.count(steamId.ConvertToUint64()) > 0 || (SteamFriends() && SteamFriends()->HasFriend(steamId, k_EFriendFlagImmediate));
	}
	if (accept) {
		SteamNetworkingMessages()->AcceptSessionWithUser(remote);
		NetSecureAddrFor(steamId);
		XLS_LOG_DEBUG("net: accepted session from %llu.", steamId.ConvertToUint64());
	}
	else {
		XLS_LOG_INFO("net: refused session from unknown user %llu.", steamId.ConvertToUint64());
	}
}

void OnNetSessionFailed(const SteamNetConnectionInfo_t& info)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	CSteamID steamId = info.m_identityRemote.GetSteamID();
	auto alias = g_aliasBySteamId.find(steamId.ConvertToUint64());
	if (alias != g_aliasBySteamId.end()) {
		g_secure[alias->second].connectStatus = XNET_CONNECT_STATUS_LOST;
	}
	XLS_LOG_WARN("net: session with %llu failed (%d: %s).", steamId.ConvertToUint64(), (int)info.m_eEndReason, info.m_szEndDebug);
}

void OnNetConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t& change)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	HSteamNetConnection connection = change.m_hConn;
	ESteamNetworkingConnectionState state = change.m_info.m_eState;

	auto owned = g_socketByConnection.find(connection);
	if (owned == g_socketByConnection.end()) {
		// A new incoming connection on one of our listen sockets.
		auto listener = g_socketByListen.find(change.m_info.m_hListenSocket);
		if (listener != g_socketByListen.end() && state == k_ESteamNetworkingConnectionState_Connecting) {
			if (SteamNetworkingSockets()->AcceptConnection(connection) == k_EResultOK) {
				listener->second->acceptQueue.push_back(connection);
				listener->second->Signal(FD_ACCEPT);
			}
		}
		return;
	}

	XlsSocket* socket = owned->second;
	switch (state) {
		case k_ESteamNetworkingConnectionState_Connected:
			socket->connecting = false;
			socket->connected = true;
			socket->Signal(FD_CONNECT | FD_WRITE);
			XLS_LOG_DEBUG("net: socket %u connected.", (unsigned)socket->handle);
			break;
		case k_ESteamNetworkingConnectionState_ClosedByPeer:
		case k_ESteamNetworkingConnectionState_ProblemDetectedLocally:
			// The peer's last messages are still queued on the connection so the title reads them
			// before it sees the close, as with TCP.
			DrainConnection(connection, socket);
			if (socket->connecting) {
				socket->connectError = WSAECONNREFUSED;
			}
			socket->connecting = false;
			socket->connected = false;
			socket->closedByPeer = true;
			socket->Signal(FD_CLOSE);
			SteamNetworkingSockets()->CloseConnection(connection, 0, nullptr, false);
			g_socketByConnection.erase(connection);
			socket->connection = k_HSteamNetConnection_Invalid;
			XLS_LOG_DEBUG("net: socket %u closed (%d: %s).", (unsigned)socket->handle, (int)change.m_info.m_eEndReason, change.m_info.m_szEndDebug);
			break;
		default:
			break;
	}
}

}

}
