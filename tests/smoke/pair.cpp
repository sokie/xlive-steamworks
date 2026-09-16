// Two-machine test: one side hosts a session, the other finds it through a filtered search, then
// both exchange UDP and TCP traffic over the GFWL socket API, report the Steam transport (direct
// or relay), swap voice frames and migrate the host in both directions. Both sides run under the
// same app id. App 480 (Spacewar) is for testing only.
#include "common.h"

#include <algorithm>
#include <functional>

namespace {

const DWORD kPairGameMode = 7777;
const DWORD kCodeProperty = XPROPERTYID(0, XUSER_DATA_TYPE_INT32, 0x21);
const DWORD kTagProperty = XPROPERTYID(0, XUSER_DATA_TYPE_INT64, 0x22);
const LONGLONG kTagBase = 5000000000LL;    // Beyond int32, so this property filters as text.
const uint16_t kUdpPort = 3000;
const uint16_t kTcpPortHost = 3001;
const uint16_t kTcpPortJoiner = 3002;
const size_t kVoiceDatagramMax = 1100;

struct Pair {
	bool host = false;
	DWORD code = 0;
	bool relayOnly = false;
	DWORD discoveryMs = 300000;
	HANDLE session = nullptr;
	XSESSION_INFO info = {};
	uint64_t nonce = 0;
	XUID selfXuid = 0;
	XNADDR selfXnaddr = {};
	bool peerKnown = false;
	IN_ADDR peerAlias = {};
	uint64_t peerSteamId = 0;
	XUID peerXuid = 0;
	SOCKET udp = INVALID_SOCKET;
	SOCKET listener = INVALID_SOCKET;
};

using Sink = std::function<void(const std::string&)>;

uint64_t SteamIdOfXnaddr(const XNADDR& address)
{
	uint64_t id;
	memcpy(&id, address.abOnline, sizeof(id));
	return id;
}

uint64_t LocalSteamId()
{
	return SteamUser()->GetSteamID().ConvertToUint64();
}

bool UdpOpen(SOCKET* out, uint16_t port)
{
	SOCKET s = XSocketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	if (s == INVALID_SOCKET) {
		return false;
	}
	sockaddr_in bind = {};
	bind.sin_family = AF_INET;
	bind.sin_port = htons(port);
	if (XSocketBind(s, (sockaddr*)&bind, sizeof(bind)) != 0) {
		XSocketClose(s);
		return false;
	}
	u_long nonBlocking = 1;
	XSocketIOCTLSocket(s, FIONBIO, &nonBlocking);
	*out = s;
	return true;
}

bool ListenOpen(SOCKET* out, uint16_t port)
{
	SOCKET s = XSocketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	if (s == INVALID_SOCKET) {
		return false;
	}
	sockaddr_in bind = {};
	bind.sin_family = AF_INET;
	bind.sin_port = htons(port);
	u_long nonBlocking = 1;
	XSocketIOCTLSocket(s, FIONBIO, &nonBlocking);
	if (XSocketBind(s, (sockaddr*)&bind, sizeof(bind)) != 0 || XSocketListen(s, 4) != 0) {
		XSocketClose(s);
		return false;
	}
	*out = s;
	return true;
}

bool SendRaw(Pair& p, const void* data, int size)
{
	sockaddr_in to = {};
	to.sin_family = AF_INET;
	to.sin_addr = p.peerAlias;
	to.sin_port = htons(kUdpPort);
	return XSocketSendTo(p.udp, (const char*)data, size, 0, (sockaddr*)&to, sizeof(to)) == size;
}

bool Send(Pair& p, const std::string& text)
{
	return SendRaw(p, text.data(), (int)text.size());
}

void SendRepeated(Pair& p, const std::string& text)
{
	for (int i = 0; i < 3; i++) {
		Send(p, text);
		Pump(5);
	}
}

bool Recv(Pair& p, std::string* text, IN_ADDR* from)
{
	char buffer[2048];
	sockaddr_in sender = {};
	int length = sizeof(sender);
	int got = XSocketRecvFrom(p.udp, buffer, sizeof(buffer), 0, (sockaddr*)&sender, &length);
	if (got <= 0) {
		return false;
	}
	text->assign(buffer, got);
	if (from) {
		*from = sender.sin_addr;
	}
	return true;
}

bool StartsWith(const std::string& text, const char* prefix)
{
	return text.compare(0, strlen(prefix), prefix) == 0;
}

// Waits for a message starting with prefix. Pings are answered on the spot so neither side can
// stall the other, anything else goes to sink.
bool WaitFor(Pair& p, const char* prefix, std::string* text, DWORD timeoutMs, const Sink& sink = nullptr)
{
	DWORD started = GetTickCount();
	while (GetTickCount() - started < timeoutMs) {
		std::string message;
		while (Recv(p, &message, nullptr)) {
			if (StartsWith(message, prefix)) {
				*text = message;
				return true;
			}
			if (StartsWith(message, "PING ")) {
				Send(p, "PONG" + message.substr(4));
			}
			else if (sink) {
				sink(message);
			}
		}
		XLiveRender();
		Sleep(1);
	}
	return false;
}

// --- Setup ---------------------------------------------------------------------------------------

bool HostCreate(Pair& p)
{
	XUserSetContext(0, X_CONTEXT_GAME_TYPE, X_CONTEXT_GAME_TYPE_STANDARD);
	XUserSetContext(0, X_CONTEXT_GAME_MODE, kPairGameMode);
	LONG code = (LONG)p.code;
	XUserSetProperty(0, kCodeProperty, sizeof(code), &code);
	LONGLONG tag = kTagBase + p.code;
	XUserSetProperty(0, kTagProperty, sizeof(tag), &tag);

	XOVERLAPPED overlapped = {};
	DWORD result = XSessionCreate(XSESSION_CREATE_HOST | XSESSION_CREATE_USES_MATCHMAKING | XSESSION_CREATE_USES_PEER_NETWORK, 0, 2, 0, &p.nonce, &p.info, &overlapped, &p.session);
	DWORD created = result == ERROR_IO_PENDING ? Wait(&overlapped) : result;
	CHECK(created == ERROR_SUCCESS && p.session, "XSessionCreate host -> lobby %llu", XlsLobbyIdFromXnkid(&p.info.sessionID));
	if (created != ERROR_SUCCESS) {
		return false;
	}
	CHECK(XNetRegisterKey(&p.info.sessionID, &p.info.keyExchangeKey) == 0, "XNetRegisterKey");
	CHECK(XNetQosListen(&p.info.sessionID, nullptr, 0, 0, XNET_QOS_LISTEN_ENABLE) == 0, "XNetQosListen enabled");
	DWORD userIndex = 0;
	BOOL privateSlot = FALSE;
	XSessionJoinLocal(p.session, 1, &userIndex, &privateSlot, nullptr);
	Pump(30);
	INFO("hosting lobby %llu with code %u. Waiting up to %u s for the joiner ...", XlsLobbyIdFromXnkid(&p.info.sessionID), p.code, p.discoveryMs / 1000);
	return true;
}

DWORD Search(DWORD code, DWORD gameMode, bool withTag, XSESSION_SEARCHRESULT_HEADER** header, std::vector<uint8_t>& storage)
{
	XUSER_CONTEXT context = { X_CONTEXT_GAME_MODE, gameMode };
	XUSER_PROPERTY properties[2] = {};
	properties[0].dwPropertyId = kCodeProperty;
	properties[0].value.type = XUSER_DATA_TYPE_INT32;
	properties[0].value.nData = (LONG)code;
	properties[1].dwPropertyId = kTagProperty;
	properties[1].value.type = XUSER_DATA_TYPE_INT64;
	properties[1].value.i64Data = kTagBase + code;
	WORD propertyCount = withTag ? 2 : 1;
	DWORD size = 0;
	XSessionSearch(0, 0, 10, propertyCount, 1, properties, &context, &size, nullptr, nullptr);
	storage.assign(size, 0);
	*header = (XSESSION_SEARCHRESULT_HEADER*)storage.data();
	DWORD result = XSessionSearch(0, 0, 10, propertyCount, 1, properties, &context, &size, *header, nullptr);
	return result == ERROR_SUCCESS ? (*header)->dwSearchResults : 0xFFFFFFFF;
}

bool JoinerFind(Pair& p)
{
	INFO("searching for a host with code %u (game mode %u) for up to %u s ...", p.code, kPairGameMode, p.discoveryMs / 1000);
	DWORD started = GetTickCount();
	std::vector<uint8_t> storage;
	XSESSION_SEARCHRESULT_HEADER* header = nullptr;
	DWORD found = 0;
	int attempts = 0;
	while (GetTickCount() - started < p.discoveryMs) {
		attempts++;
		found = Search(p.code, kPairGameMode, true, &header, storage);
		if (found != 0 && found != 0xFFFFFFFF) {
			break;
		}
		Pump(200);
	}
	CHECK(found == 1, "filtered search (context + int32 property + int64 property) -> %u session(s) after %d attempt(s), %u s", found, attempts, (GetTickCount() - started) / 1000);
	if (found != 1) {
		return false;
	}
	XSESSION_SEARCHRESULT& result = header->pResults[0];
	p.info = result.info;
	bool codeCarried = false;
	for (DWORD i = 0; i < result.cProperties; i++) {
		if (result.pProperties[i].dwPropertyId == kCodeProperty && result.pProperties[i].value.nData == (LONG)p.code) {
			codeCarried = true;
		}
	}
	bool modeCarried = false;
	for (DWORD i = 0; i < result.cContexts; i++) {
		if (result.pContexts[i].dwContextId == X_CONTEXT_GAME_MODE && result.pContexts[i].dwValue == kPairGameMode) {
			modeCarried = true;
		}
	}
	CHECK(codeCarried && modeCarried, "result carries the host's context and properties (%u contexts, %u properties, %u open public slots)", result.cContexts, result.cProperties, result.dwOpenPublicSlots);
	const wchar_t* hostName = nullptr;
	ULONGLONG hostPuid = 0;
	for (DWORD i = 0; i < result.cProperties; i++) {
		const XUSER_PROPERTY& property = result.pProperties[i];
		if (property.dwPropertyId == X_PROPERTY_GAMER_HOSTNAME && property.value.type == XUSER_DATA_TYPE_UNICODE) {
			hostName = property.value.string.pwszData;
		}
		if (property.dwPropertyId == X_PROPERTY_GAMER_PUID && property.value.type == XUSER_DATA_TYPE_INT64) {
			hostPuid = (ULONGLONG)property.value.i64Data;
		}
	}
	CHECK(hostName && hostName[0] && hostPuid, "result carries the host's gamertag \"%ls\" and XUID 0x%016llx as system properties", hostName ? hostName : L"", hostPuid);

	// Filters must also exclude: a wrong code and a wrong game mode find nothing.
	std::vector<uint8_t> other;
	XSESSION_SEARCHRESULT_HEADER* otherHeader = nullptr;
	DWORD wrongCode = Search(p.code + 1, kPairGameMode, false, &otherHeader, other);
	CHECK(wrongCode == 0, "search with a different code -> %u session(s)", wrongCode);
	DWORD wrongMode = Search(p.code, kPairGameMode + 1, false, &otherHeader, other);
	CHECK(wrongMode == 0, "search with a different game mode -> %u session(s)", wrongMode);
	return true;
}

bool JoinerJoin(Pair& p)
{
	// Titles probe the host before joining, the answer travels the wrapper's control channel.
	const XNADDR* address = &p.info.hostAddress;
	const XNKID* kid = &p.info.sessionID;
	const XNKEY* key = &p.info.keyExchangeKey;
	XNQOS* qos = nullptr;
	INT rc = XNetQosLookup(1, &address, &kid, &key, 0, nullptr, nullptr, 8, 0, 0, nullptr, &qos);
	CHECK(rc == 0 && qos, "XNetQosLookup(host) started");
	if (rc == 0 && qos) {
		DWORD started = GetTickCount();
		while (qos->cxnqosPending && GetTickCount() - started < 10000) {
			Pump(2);
		}
		XNQOSINFO& info = qos->axnqosinfo[0];
		CHECK(!qos->cxnqosPending && (info.bFlags & XNET_XNQOSINFO_TARGET_CONTACTED), "QoS: host contacted, rtt %u ms median, flags 0x%x", info.wRttMedInMsecs, info.bFlags);
		XNetQosRelease(qos);
	}

	XOVERLAPPED overlapped = {};
	DWORD result = XSessionCreate(XSESSION_CREATE_USES_MATCHMAKING | XSESSION_CREATE_USES_PEER_NETWORK, 0, 0, 0, &p.nonce, &p.info, &overlapped, &p.session);
	DWORD joined = result == ERROR_IO_PENDING ? Wait(&overlapped) : result;
	CHECK(joined == ERROR_SUCCESS && p.session, "XSessionCreate join -> %u", joined);
	if (joined != ERROR_SUCCESS) {
		return false;
	}
	CHECK(XNetRegisterKey(&p.info.sessionID, &p.info.keyExchangeKey) == 0, "XNetRegisterKey(joined session)");
	DWORD userIndex = 0;
	BOOL privateSlot = FALSE;
	XSessionJoinLocal(p.session, 1, &userIndex, &privateSlot, nullptr);

	CHECK(XNetXnAddrToInAddr(&p.info.hostAddress, &p.info.sessionID, &p.peerAlias) == 0, "XNetXnAddrToInAddr(host) -> %u.%u.%u.%u", p.peerAlias.S_un.S_un_b.s_b1, p.peerAlias.S_un.S_un_b.s_b2, p.peerAlias.S_un.S_un_b.s_b3, p.peerAlias.S_un.S_un_b.s_b4);
	XNetConnect(p.peerAlias);
	DWORD started = GetTickCount();
	INT status = 0;
	while ((status = XNetGetConnectStatus(p.peerAlias)) != XNET_CONNECT_STATUS_CONNECTED && GetTickCount() - started < 15000) {
		Pump(5);
	}
	CHECK(status == XNET_CONNECT_STATUS_CONNECTED, "XNetGetConnectStatus(host) -> %d after %u ms", status, GetTickCount() - started);
	uint64_t steamId = 0;
	XlsSteamIdFromSecureAddr(p.peerAlias, &steamId);
	p.peerSteamId = steamId;
	p.peerXuid = XlsXuidFromSteamId(steamId);
	p.peerKnown = steamId != 0;
	return true;
}

bool JoinerHello(Pair& p)
{
	DWORD started = GetTickCount();
	std::string welcome;
	bool got = false;
	while (!got && GetTickCount() - started < 60000) {
		Send(p, Format("HELLO %llu", p.selfXuid));
		got = WaitFor(p, "WELCOME ", &welcome, 500);
	}
	CHECK(got, "UDP handshake with the host (%u ms)", GetTickCount() - started);
	return got;
}

bool HostWaitHello(Pair& p)
{
	DWORD started = GetTickCount();
	while (GetTickCount() - started < p.discoveryMs) {
		std::string message;
		IN_ADDR from = {};
		while (Recv(p, &message, &from)) {
			if (StartsWith(message, "HELLO ")) {
				p.peerAlias = from;
				p.peerXuid = _strtoui64(message.c_str() + 6, nullptr, 10);
				uint64_t steamId = 0;
				XlsSteamIdFromSecureAddr(from, &steamId);
				p.peerSteamId = steamId;
				p.peerKnown = steamId != 0;
				CHECK(p.peerKnown, "UDP handshake: joiner arrived after %u s from alias %u.%u.%u.%u", (GetTickCount() - started) / 1000, from.S_un.S_un_b.s_b1, from.S_un.S_un_b.s_b2, from.S_un.S_un_b.s_b3, from.S_un.S_un_b.s_b4);
				SendRepeated(p, Format("WELCOME %llu", p.selfXuid));
				return p.peerKnown;
			}
		}
		Pump(5);
	}
	CHECK(false, "no joiner arrived within %u s", p.discoveryMs / 1000);
	return false;
}

// --- Traffic -------------------------------------------------------------------------------------

void PingRound(Pair& p, const char* label)
{
	int replies = 0;
	DWORD total = 0;
	DWORD best = 0xFFFFFFFF;
	DWORD worst = 0;
	for (int i = 0; i < 20; i++) {
		DWORD sent = GetTickCount();
		Send(p, Format("PING %d %u", i, sent));
		std::string reply;
		if (WaitFor(p, Format("PONG %d ", i).c_str(), &reply, 1500)) {
			DWORD rtt = GetTickCount() - sent;
			replies++;
			total += rtt;
			best = std::min(best, rtt);
			worst = std::max(worst, rtt);
		}
	}
	CHECK(replies >= 18, "%s: %d/20 UDP round trips (rtt min %u avg %u max %u ms)", label, replies, replies ? best : 0, replies ? total / replies : 0, worst);
}

void UdpPhase(Pair& p)
{
	std::string ignored;
	if (p.host) {
		CHECK(WaitFor(p, "YOURTURN", &ignored, 60000), "joiner finished its ping round");
		PingRound(p, "host -> joiner");
		SendRepeated(p, "PINGDONE");
	}
	else {
		PingRound(p, "joiner -> host");
		SendRepeated(p, "YOURTURN");
		CHECK(WaitFor(p, "PINGDONE", &ignored, 60000), "host finished its ping round");
	}
	XLS_CONNECTION_INFO info = {};
	if (XlsPeerConnectionInfo(p.peerAlias, &info)) {
		ReportTransport("UDP session", info, p.relayOnly);
	}
	else {
		CHECK(false, "XlsPeerConnectionInfo for the peer");
	}
}

bool SendAll(SOCKET s, const std::string& data, DWORD timeoutMs)
{
	size_t offset = 0;
	DWORD started = GetTickCount();
	while (offset < data.size()) {
		int sent = XSocketSend(s, data.data() + offset, (int)(data.size() - offset), 0);
		if (sent > 0) {
			offset += sent;
			continue;
		}
		if (sent == SOCKET_ERROR && WSAGetLastError() != WSAEWOULDBLOCK) {
			return false;
		}
		if (GetTickCount() - started > timeoutMs) {
			return false;
		}
		Pump(1);
	}
	return true;
}

std::string RecvUntil(SOCKET s, size_t wanted, DWORD timeoutMs)
{
	std::string data;
	DWORD started = GetTickCount();
	while (data.size() < wanted && GetTickCount() - started < timeoutMs) {
		char buffer[1024];
		int got = XSocketRecv(s, buffer, sizeof(buffer), 0);
		if (got > 0) {
			data.append(buffer, got);
		}
		else if (got == 0) {
			break;
		}
		else {
			Pump(1);
		}
	}
	return data;
}

void TcpClient(Pair& p, uint16_t port, const char* label)
{
	SOCKET s = XSocketCreate(AF_INET, SOCK_STREAM, IPPROTO_TCP);
	sockaddr_in to = {};
	to.sin_family = AF_INET;
	to.sin_addr = p.peerAlias;
	to.sin_port = htons(port);
	DWORD started = GetTickCount();
	int rc = XSocketConnect(s, (sockaddr*)&to, sizeof(to));
	CHECK(rc == 0, "%s: XSocketConnect to port %u (%u ms%s%d)", label, port, GetTickCount() - started, rc == 0 ? "" : ", error ", rc == 0 ? 0 : WSAGetLastError());
	if (rc != 0) {
		XSocketClose(s);
		return;
	}
	u_long nonBlocking = 1;
	XSocketIOCTLSocket(s, FIONBIO, &nonBlocking);
	std::string outgoing;
	for (int i = 0; i < 5; i++) {
		outgoing += Format("LINE %d from the %s\n", i, p.host ? "host" : "joiner");
	}
	outgoing += "BYE\n";
	bool sent = SendAll(s, outgoing, 10000);
	std::string echoed = sent ? RecvUntil(s, outgoing.size(), 20000) : std::string();
	CHECK(sent && echoed == outgoing, "%s: %zu of %zu bytes echoed back over the stream", label, echoed.size(), outgoing.size());
	XLS_CONNECTION_INFO info = {};
	if (XlsSocketConnectionInfo(s, &info)) {
		ReportTransport(label, info, p.relayOnly);
	}
	else {
		CHECK(false, "%s: XlsSocketConnectionInfo", label);
	}
	// Sent and closed back to back: the peer must still read it, as over TCP.
	SendAll(s, "LAST\n", 10000);
	XSocketClose(s);
}

void TcpServer(Pair& p, const char* label, DWORD timeoutMs)
{
	SOCKET c = INVALID_SOCKET;
	DWORD started = GetTickCount();
	while (c == INVALID_SOCKET && GetTickCount() - started < timeoutMs) {
		sockaddr_in from = {};
		int length = sizeof(from);
		c = XSocketAccept(p.listener, (sockaddr*)&from, &length);
		if (c == INVALID_SOCKET) {
			Pump(2);
		}
	}
	CHECK(c != INVALID_SOCKET, "%s: accepted the incoming connection (%u ms)", label, GetTickCount() - started);
	if (c == INVALID_SOCKET) {
		return;
	}
	u_long nonBlocking = 1;
	XSocketIOCTLSocket(c, FIONBIO, &nonBlocking);
	std::string seen;
	bool bye = false;
	started = GetTickCount();
	while (!bye && GetTickCount() - started < 30000) {
		char buffer[1024];
		int got = XSocketRecv(c, buffer, sizeof(buffer), 0);
		if (got > 0) {
			SendAll(c, std::string(buffer, got), 10000);
			seen.append(buffer, got);
			bye = seen.find("BYE\n") != std::string::npos;
		}
		else if (got == 0) {
			break;
		}
		else {
			Pump(1);
		}
	}
	CHECK(bye, "%s: echoed %zu bytes until BYE", label, seen.size());
	XLS_CONNECTION_INFO info = {};
	if (XlsSocketConnectionInfo(c, &info)) {
		ReportTransport(label, info, p.relayOnly);
	}
	else {
		CHECK(false, "%s: XlsSocketConnectionInfo", label);
	}
	std::string tail;
	bool closed = false;
	started = GetTickCount();
	while (!closed && GetTickCount() - started < 10000) {
		char buffer[64];
		int got = XSocketRecv(c, buffer, sizeof(buffer), 0);
		if (got > 0) {
			tail.append(buffer, got);
		}
		else if (got == 0) {
			closed = true;
		}
		else {
			Pump(1);
		}
	}
	CHECK(tail == "LAST\n" && closed, "%s: the line sent right before the peer closed arrived (%zu bytes), then the close", label, tail.size());
	XSocketClose(c);
}

void TcpPhase(Pair& p)
{
	if (p.host) {
		TcpServer(p, "TCP joiner -> host", 60000);
		TcpClient(p, kTcpPortJoiner, "TCP host -> joiner");
	}
	else {
		TcpClient(p, kTcpPortHost, "TCP joiner -> host");
		TcpServer(p, "TCP host -> joiner", 60000);
	}
}

// Sends whole voice frames ([u16 size][data] each) in datagrams the unreliable channel accepts.
void SendVoiceFrames(Pair& p, const uint8_t* data, DWORD size, DWORD* datagrams)
{
	std::string chunk = "V:";
	DWORD offset = 0;
	while (offset + 2 <= size) {
		uint16_t frameSize;
		memcpy(&frameSize, data + offset, sizeof(frameSize));
		DWORD needed = 2 + frameSize;
		if (offset + needed > size) {
			break;
		}
		if (chunk.size() > 2 && chunk.size() + needed > kVoiceDatagramMax) {
			SendRaw(p, chunk.data(), (int)chunk.size());
			(*datagrams)++;
			chunk = "V:";
		}
		chunk.append((const char*)data + offset, needed);
		offset += needed;
	}
	if (chunk.size() > 2) {
		SendRaw(p, chunk.data(), (int)chunk.size());
		(*datagrams)++;
	}
}

void VoicePhase(Pair& p)
{
	XHV_PROCESSING_MODE modes[] = { XHV_VOICECHAT_MODE };
	XHV_INIT_PARAMS params = {};
	params.dwMaxRemoteTalkers = 4;
	params.dwMaxLocalTalkers = 1;
	params.localTalkerEnabledModes = modes;
	params.dwNumLocalTalkerEnabledModes = 1;
	params.remoteTalkerEnabledModes = modes;
	params.dwNumRemoteTalkerEnabledModes = 1;
	IXHVEngine* engine = nullptr;
	HANDLE worker = nullptr;
	HRESULT hr = XHVCreateEngine(&params, &worker, &engine);
	CHECK(hr == S_OK && engine, "XHVCreateEngine -> 0x%08x", hr);
	if (hr != S_OK || !engine) {
		return;
	}
	CHECK(engine->RegisterLocalTalker(0) == S_OK, "voice: RegisterLocalTalker");
	CHECK(engine->RegisterRemoteTalker(p.peerXuid, nullptr, nullptr, nullptr) == S_OK, "voice: RegisterRemoteTalker(peer)");
	CHECK(engine->StartLocalProcessingModes(0, modes, 1) == S_OK, "voice: StartLocalProcessingModes(VOICECHAT)");
	engine->StartRemoteProcessingModes(p.peerXuid, modes, 1);
	INFO("voice: headset present %d", (int)engine->IsHeadsetPresent(0));

	DWORD sentFrames = 0;
	DWORD sentBytes = 0;
	DWORD sentDatagrams = 0;
	DWORD gotDatagrams = 0;
	DWORD gotBytes = 0;
	DWORD decodeErrors = 0;
	bool remoteTalked = false;
	Sink sink = [&](const std::string& message) {
		if (!StartsWith(message, "V:")) {
			return;
		}
		DWORD size = (DWORD)message.size() - 2;
		HRESULT submitted = engine->SubmitIncomingChatData(p.peerXuid, (const uint8_t*)message.data() + 2, &size);
		if (submitted == S_OK && size == message.size() - 2) {
			gotDatagrams++;
			gotBytes += size;
			remoteTalked = remoteTalked || engine->IsRemoteTalking(p.peerXuid);
		}
		else {
			decodeErrors++;
		}
	};
	DWORD started = GetTickCount();
	bool peerDone = false;
	std::string ignored;
	while (GetTickCount() - started < 5000) {
		if (engine->GetDataReadyFlags() & 1) {
			uint8_t buffer[4096];
			DWORD size = sizeof(buffer);
			DWORD packets = 0;
			if (engine->GetLocalChatData(0, buffer, &size, &packets) == S_OK && size) {
				SendVoiceFrames(p, buffer, size, &sentDatagrams);
				sentFrames += packets;
				sentBytes += size;
			}
		}
		if (!peerDone && WaitFor(p, "VOICE_DONE", &ignored, 20, sink)) {
			peerDone = true;
		}
	}
	SendRepeated(p, "VOICE_DONE");
	if (!peerDone) {
		peerDone = WaitFor(p, "VOICE_DONE", &ignored, 30000, sink);
	}
	INFO("voice: sent %u frame(s), %u bytes in %u datagram(s), received %u datagram(s), %u bytes, decode errors %u, remote talking seen %d", sentFrames, sentBytes, sentDatagrams, gotDatagrams, gotBytes, decodeErrors, (int)remoteTalked);
	if (!sentFrames) {
		INFO("voice: this side captured nothing (no microphone, or Steam has no recording device), that is not a failure");
	}
	CHECK(peerDone, "voice phase finished on both sides");
	CHECK(decodeErrors == 0, "every received voice datagram was accepted by the engine");
	engine->StopRemoteProcessingModes(p.peerXuid, modes, 1);
	engine->StopLocalProcessingModes(0, modes, 1);
	engine->UnregisterRemoteTalker(p.peerXuid);
	engine->UnregisterLocalTalker(0);
	engine->Release();
	if (worker) {
		CloseHandle(worker);
	}
}

// --- Host migration ------------------------------------------------------------------------------

std::string LobbyHostHex(uint64_t lobby)
{
	const char* data = SteamMatchmaking()->GetLobbyData(CSteamID(lobby), "xl_host");
	return data ? data : "";
}

bool WaitLobbyOwner(uint64_t lobby, uint64_t owner, const std::string& hostHex, DWORD timeoutMs, DWORD* elapsed)
{
	DWORD started = GetTickCount();
	bool ok = false;
	while (!ok && GetTickCount() - started < timeoutMs) {
		ok = SteamMatchmaking()->GetLobbyOwner(CSteamID(lobby)).ConvertToUint64() == owner && (hostHex.empty() || LobbyHostHex(lobby) == hostHex);
		if (!ok) {
			Pump(5);
		}
	}
	*elapsed = GetTickCount() - started;
	return ok;
}

DWORD HostIndexOf(HANDLE session)
{
	DWORD size = 0;
	XSessionGetDetails(session, &size, nullptr, nullptr);
	std::vector<uint8_t> buffer(size);
	XSESSION_LOCAL_DETAILS* details = (XSESSION_LOCAL_DETAILS*)buffer.data();
	if (XSessionGetDetails(session, &size, details, nullptr) != ERROR_SUCCESS) {
		return 0xFFFFFFFE;
	}
	return details->dwUserIndexHost;
}

// The joiner takes over while the old host is still in the lobby: the old host's wrapper must hand
// the Steam lobby over, since only the lobby owner can publish session data.
void MigrationWhileHostPresent(Pair& p)
{
	uint64_t lobby = XlsLobbyIdFromSession(p.session);
	if (!p.host) {
		XSESSION_INFO migrated = {};
		DWORD result = XSessionMigrateHost(p.session, 0, &migrated, nullptr);
		CHECK(result == ERROR_SUCCESS, "XSessionMigrateHost(become host) -> %u", result);
		CHECK(SteamIdOfXnaddr(migrated.hostAddress) == LocalSteamId(), "migrated session info names this machine as host");
		CHECK(memcmp(&migrated.sessionID, &p.info.sessionID, sizeof(XNKID)) == 0, "session id (lobby) unchanged by the migration");
		p.info = migrated;
		SendRepeated(p, "MIGRATED " + HexOf(&migrated, sizeof(migrated)));
		DWORD elapsed = 0;
		bool owner = WaitLobbyOwner(lobby, LocalSteamId(), HexOf(&migrated.hostAddress, sizeof(XNADDR)), 20000, &elapsed);
		CHECK(owner, "Steam lobby handed to the migrated host and its data published (%u ms)", elapsed);
		CHECK(HostIndexOf(p.session) == 0, "XSessionGetDetails: host index is now 0 here");
		std::string reply;
		CHECK(WaitFor(p, "MIGRATE_OK", &reply, 45000), "old host confirmed: %s", reply.c_str());
	}
	else {
		std::string message;
		bool got = WaitFor(p, "MIGRATED ", &message, 90000);
		CHECK(got, "received the new host's session info");
		if (!got) {
			return;
		}
		XSESSION_INFO migrated = {};
		UnhexTo(message.substr(9), &migrated, sizeof(migrated));
		DWORD result = XSessionMigrateHost(p.session, XUSER_INDEX_NONE, &migrated, nullptr);
		CHECK(result == ERROR_SUCCESS, "XSessionMigrateHost(adopt new host) -> %u", result);
		DWORD elapsed = 0;
		bool handed = WaitLobbyOwner(lobby, p.peerSteamId, HexOf(&migrated.hostAddress, sizeof(XNADDR)), 20000, &elapsed);
		CHECK(handed, "lobby ownership and host data moved to the new host (%u ms)", elapsed);
		CHECK(HostIndexOf(p.session) == XUSER_INDEX_NONE, "XSessionGetDetails: host index is now XUSER_INDEX_NONE here");
		p.info = migrated;
		SendRepeated(p, Format("MIGRATE_OK handed=%d in %u ms", (int)handed, elapsed));
	}
}

// The current host leaves and the remaining member takes over an owner-less lobby.
void MigrationAfterHostLeft(Pair& p)
{
	uint64_t lobby = XlsLobbyIdFromXnkid(&p.info.sessionID);
	if (!p.host) {
		SendRepeated(p, "LEAVING");
		Pump(30);
		CHECK(XSessionDelete(p.session, nullptr) == ERROR_SUCCESS, "XSessionDelete (host leaves)");
		XCloseHandle(p.session);
		p.session = nullptr;
		std::string reply;
		CHECK(WaitFor(p, "MIGRATE2_OK", &reply, 90000), "remaining member took the session over: %s", reply.c_str());
		// The new owner's data takes a moment to reach the backend so poll a few times.
		DWORD result = 0;
		DWORD found = 0;
		uint64_t seenHost = 0;
		for (int attempt = 0; attempt < 5 && seenHost != p.peerSteamId; attempt++) {
			Pump(100);
			DWORD size = 0;
			XSessionSearchByID(p.info.sessionID, 0, &size, nullptr, nullptr);
			std::vector<uint8_t> buffer(size);
			XSESSION_SEARCHRESULT_HEADER* header = (XSESSION_SEARCHRESULT_HEADER*)buffer.data();
			result = XSessionSearchByID(p.info.sessionID, 0, &size, header, nullptr);
			found = result == ERROR_SUCCESS ? header->dwSearchResults : 0;
			seenHost = found == 1 ? SteamIdOfXnaddr(header->pResults[0].info.hostAddress) : 0;
		}
		CHECK(result == ERROR_SUCCESS && found == 1, "XSessionSearchByID after leaving -> %u result(s)", found);
		CHECK(seenHost == p.peerSteamId, "the session names the remaining member as host (saw %llu, expected %llu)", seenHost, p.peerSteamId);
		SendRepeated(p, "DONE");
	}
	else {
		std::string message;
		CHECK(WaitFor(p, "LEAVING", &message, 90000), "new host announced that it is leaving");
		DWORD started = GetTickCount();
		int members = 0;
		while ((members = SteamMatchmaking()->GetNumLobbyMembers(CSteamID(lobby))) > 1 && GetTickCount() - started < 30000) {
			Pump(5);
		}
		CHECK(members == 1, "Steam dropped the departed host from the lobby (%u ms)", GetTickCount() - started);
		XSESSION_INFO info = {};
		DWORD result = XSessionMigrateHost(p.session, 0, &info, nullptr);
		CHECK(result == ERROR_SUCCESS, "XSessionMigrateHost(take over) -> %u", result);
		DWORD elapsed = 0;
		bool owner = WaitLobbyOwner(lobby, LocalSteamId(), HexOf(&info.hostAddress, sizeof(XNADDR)), 20000, &elapsed);
		CHECK(owner, "this machine owns the lobby again with its host data published (%u ms)", elapsed);
		p.info = info;
		SendRepeated(p, Format("MIGRATE2_OK owner=%d in %u ms", (int)owner, elapsed));
		CHECK(WaitFor(p, "DONE", &message, 45000), "peer verified the session from outside");
	}
}

}

int RunPair(bool host, DWORD code, bool relayOnly, DWORD discoveryTimeoutSeconds)
{
	Pair p;
	p.host = host;
	p.code = code;
	p.relayOnly = relayOnly;
	p.discoveryMs = discoveryTimeoutSeconds * 1000;
	INFO("pair test: %s, code %u, %s, app %u, Steam user %llu \"%s\"", host ? "HOST" : "JOINER", code, relayOnly ? "RELAY ONLY (direct paths disabled)" : "auto transport", SteamUtils()->GetAppID(), LocalSteamId(), SteamFriends()->GetPersonaName());
	if (relayOnly) {
		XlsSetP2PTransport(XLS_P2P_TRANSPORT_RELAY_ONLY);
	}
	SteamRelayNetworkStatus_t relay = {};
	for (int i = 0; i < 500 && SteamNetworkingUtils()->GetRelayNetworkStatus(&relay) != k_ESteamNetworkingAvailability_Current; i++) {
		Pump(1);
	}
	INFO("relay network: availability %d (%s)", (int)relay.m_eAvail, relay.m_debugMsg);

	XNetStartupParams params = {};
	params.cfgSizeOfStruct = sizeof(params);
	XNetStartup(&params);
	XUserGetXUID(0, &p.selfXuid);
	XNetGetTitleXnAddr(&p.selfXnaddr);
	CHECK(UdpOpen(&p.udp, kUdpPort), "UDP socket on port %u", kUdpPort);
	CHECK(ListenOpen(&p.listener, host ? kTcpPortHost : kTcpPortJoiner), "TCP listen socket on port %u", host ? kTcpPortHost : kTcpPortJoiner);

	bool ready = host ? (HostCreate(p) && HostWaitHello(p)) : (JoinerFind(p) && JoinerJoin(p) && JoinerHello(p));
	if (ready && p.peerKnown) {
		INFO("peer: Steam id %llu, XUID 0x%016llx, alias %u.%u.%u.%u", p.peerSteamId, p.peerXuid, p.peerAlias.S_un.S_un_b.s_b1, p.peerAlias.S_un.S_un_b.s_b2, p.peerAlias.S_un.S_un_b.s_b3, p.peerAlias.S_un.S_un_b.s_b4);
		UdpPhase(p);
		TcpPhase(p);
		VoicePhase(p);
		MigrationWhileHostPresent(p);
		MigrationAfterHostLeft(p);
	}
	else {
		printf("[FAIL] the two sides never met, nothing else was tested\n");
		g_failures++;
	}

	if (p.listener != INVALID_SOCKET) {
		XSocketClose(p.listener);
	}
	if (p.udp != INVALID_SOCKET) {
		XSocketClose(p.udp);
	}
	if (p.session) {
		XSessionDelete(p.session, nullptr);
		XCloseHandle(p.session);
	}
	XNetUnregisterKey(&p.info.sessionID);
	XNetCleanup();
	printf("PAIR RESULT: %d failure(s)\n", g_failures);
	fflush(stdout);
	return g_failures ? 1 : 0;
}
