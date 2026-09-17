// #51 - #84 XNet, #5023, #5296, #5310, #5311, #5324, #5334, #5335, #5359.
#include "xlive/xfuncs.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/net.h"
#include "core/steam.h"
#include "core/utils.h"

#include <ws2tcpip.h>
#include <thread>

namespace {

bool g_xnetStarted = false;
XNetStartupParams g_startupParams = {};

// Seeds the parameters the secure network library applies to every field a title leaves at zero.
// A title sizes its QoS listen data from cfgQosDataLimitDiv4, so a zero there cuts that data to
// nothing.
void ResetStartupParams()
{
	memset(&g_startupParams, 0, sizeof(g_startupParams));
	g_startupParams.cfgSizeOfStruct = sizeof(XNetStartupParams);
	g_startupParams.cfgSockMaxDgramSockets = 8;
	g_startupParams.cfgSockMaxStreamSockets = 32;
	g_startupParams.cfgSockDefaultRecvBufsizeInK = 16;
	g_startupParams.cfgSockDefaultSendBufsizeInK = 16;
	g_startupParams.cfgKeyRegMax = 8;
	g_startupParams.cfgSecRegMax = 32;
	g_startupParams.cfgQosDataLimitDiv4 = 64;
	g_startupParams.cfgQosProbeTimeoutInSeconds = 2;
	g_startupParams.cfgQosProbeRetries = 3;
	g_startupParams.cfgQosSrvMaxSimultaneousResponses = 8;
	g_startupParams.cfgQosPairWaitTimeInSeconds = 2;
}

WORD g_systemLinkPort = 0;
WORD g_onlinePort = 0;
bool g_onlineStarted = false;

// XNetDnsLookup resolves on a thread and signals the title's event when done.
struct DnsRequest {
	XNDNS* result;
	std::string host;
	WSAEVENT event;
};

void DnsWorker(DnsRequest* request)
{
	addrinfo hints = {};
	hints.ai_family = AF_INET;
	addrinfo* info = nullptr;
	int rc = getaddrinfo(request->host.c_str(), nullptr, &hints, &info);
	if (rc == 0 && info) {
		UINT count = 0;
		for (addrinfo* entry = info; entry && count < 8; entry = entry->ai_next) {
			request->result->aina[count++] = ((sockaddr_in*)entry->ai_addr)->sin_addr;
		}
		request->result->cina = count;
		freeaddrinfo(info);
		MemoryBarrier();
		request->result->iStatus = count ? 0 : WSAHOST_NOT_FOUND;
	}
	else {
		MemoryBarrier();
		request->result->iStatus = rc ? rc : WSAHOST_NOT_FOUND;
	}
	if (request->event) {
		SetEvent(request->event);
	}
	delete request;
}

}

// #51
INT WINAPI XNetStartup(const XNetStartupParams* pxnsp)
{
	XLS_TRACE_FN();
	return XNetStartupEx(pxnsp, 0);
}

// #80
INT WINAPI XNetStartupEx(const XNetStartupParams* pxnsp, DWORD dwVersionReq)
{
	XLS_TRACE_FN();
	ResetStartupParams();
	if (pxnsp && pxnsp->cfgSizeOfStruct == sizeof(XNetStartupParams)) {
		const uint8_t* given = (const uint8_t*)pxnsp;
		uint8_t* effective = (uint8_t*)&g_startupParams;
		for (size_t i = 1; i < sizeof(XNetStartupParams); i++) {
			if (given[i]) {
				effective[i] = given[i];
			}
		}
	}
	XLS_LOG_DEBUG("xnet: startup params: flags 0x%02x, %u dgram %u stream sockets, %uK recv %uK send, %u keys %u sec, qos data %u bytes.",
		g_startupParams.cfgFlags, g_startupParams.cfgSockMaxDgramSockets, g_startupParams.cfgSockMaxStreamSockets,
		g_startupParams.cfgSockDefaultRecvBufsizeInK, g_startupParams.cfgSockDefaultSendBufsizeInK,
		g_startupParams.cfgKeyRegMax, g_startupParams.cfgSecRegMax, g_startupParams.cfgQosDataLimitDiv4 * 4u);
	xls::NetInit();
	g_xnetStarted = true;
	return 0;
}

// #52
INT WINAPI XNetCleanup()
{
	XLS_TRACE_FN();
	ResetStartupParams();
	g_xnetStarted = false;
	return 0;
}

// #53
INT WINAPI XNetRandom(uint8_t* pb, UINT cb)
{
	XLS_TRACE_FN();
	if (!pb) {
		return WSAEFAULT;
	}
	for (UINT i = 0; i < cb; i++) {
		pb[i] = (uint8_t)rand();
	}
	return 0;
}

// #54
INT WINAPI XNetCreateKey(XNKID* pxnkid, XNKEY* pxnkey)
{
	XLS_TRACE_FN();
	if (!pxnkid || !pxnkey) {
		return WSAEFAULT;
	}
	xls::NetCreateKey(pxnkid, pxnkey);
	return 0;
}

// #55
INT WINAPI XNetRegisterKey(const XNKID* pxnkid, const XNKEY* pxnkey)
{
	XLS_TRACE_FN();
	if (!pxnkid || !pxnkey) {
		return WSAEFAULT;
	}
	if (!xls::NetRegisterKey(*pxnkid, *pxnkey)) {
		return WSAEALREADY;
	}
	return 0;
}

// #56
INT WINAPI XNetUnregisterKey(const XNKID* pxnkid)
{
	XLS_TRACE_FN();
	if (!pxnkid) {
		return WSAEFAULT;
	}
	return xls::NetUnregisterKey(*pxnkid) ? 0 : WSAEINVAL;
}

// #81
INT WINAPI XNetReplaceKey(const XNKID* pxnkidUnregister, const XNKID* pxnkidReplace)
{
	XLS_TRACE_FN();
	if (!pxnkidUnregister || !pxnkidReplace) {
		return WSAEFAULT;
	}
	return xls::NetReplaceKey(*pxnkidUnregister, *pxnkidReplace) ? 0 : WSAEINVAL;
}

// #57
INT WINAPI XNetXnAddrToInAddr(const XNADDR* pxna, const XNKID* pxnkid, IN_ADDR* pina)
{
	XLS_TRACE_FN();
	if (!pxna || !pina) {
		return WSAEFAULT;
	}
	CSteamID steamId;
	if (!xls::NetSteamIdFromXnaddr(*pxna, &steamId)) {
		XLS_LOG_WARN("XNetXnAddrToInAddr: the XNADDR does not carry a Steam id, using its online address as-is.");
		*pina = pxna->inaOnline.S_un.S_addr ? pxna->inaOnline : pxna->ina;
		return 0;
	}
	*pina = xls::NetSecureAddrFor(steamId);
	return 0;
}

// #58
INT WINAPI XNetServerToInAddr(const IN_ADDR ina, DWORD dwServiceId, IN_ADDR* pina)
{
	XLS_TRACE_FN();
	if (!pina) {
		return WSAEFAULT;
	}
	*pina = xls::NetSecureAddrForServer(ina, dwServiceId);
	return 0;
}

// #59
INT WINAPI XNetTsAddrToInAddr(const TSADDR* ptsa, DWORD dwServiceId, const XNKID* pxnkid, IN_ADDR* pina)
{
	XLS_TRACE_FN();
	if (!ptsa || !pina) {
		return WSAEFAULT;
	}
	CSteamID steamId;
	if (xls::NetSteamIdFromXnaddr(*ptsa, &steamId)) {
		*pina = xls::NetSecureAddrFor(steamId);
		return 0;
	}
	*pina = xls::NetSecureAddrForServer(ptsa->ina, dwServiceId);
	return 0;
}

// #60
INT WINAPI XNetInAddrToXnAddr(const IN_ADDR ina, XNADDR* pxna, XNKID* pxnkid)
{
	XLS_TRACE_FN();
	if (!pxna) {
		return WSAEFAULT;
	}
	CSteamID steamId;
	if (xls::NetIsLocalAlias(ina)) {
		xls::NetLocalXnaddr(pxna);
	}
	else if (xls::NetSecureAddrToSteamId(ina, &steamId)) {
		xls::NetXnaddrForSteamId(steamId, pxna);
	}
	else {
		IN_ADDR real;
		memset(pxna, 0, sizeof(*pxna));
		if (xls::NetSecureAddrToServer(ina, &real)) {
			pxna->ina = real;
			pxna->inaOnline = real;
		}
		else {
			return WSAEINVAL;
		}
	}
	if (pxnkid) {
		memset(pxnkid, 0, sizeof(*pxnkid));
	}
	return 0;
}

// #61
INT WINAPI XNetInAddrToServer(const IN_ADDR ina, IN_ADDR* pina)
{
	XLS_TRACE_FN();
	if (!pina) {
		return WSAEFAULT;
	}
	if (xls::NetSecureAddrToServer(ina, pina)) {
		return 0;
	}
	*pina = ina;
	return 0;
}

// #62
INT WINAPI XNetInAddrToString(const IN_ADDR ina, char* pchBuf, INT cchBuf)
{
	XLS_TRACE_FN();
	if (!pchBuf || cchBuf < 16) {
		return WSAEFAULT;
	}
	const uint8_t* bytes = (const uint8_t*)&ina.S_un.S_addr;
	snprintf(pchBuf, (size_t)cchBuf, "%u.%u.%u.%u", bytes[0], bytes[1], bytes[2], bytes[3]);
	return 0;
}

// #63
INT WINAPI XNetUnregisterInAddr(const IN_ADDR ina)
{
	XLS_TRACE_FN();
	xls::NetSecureAddrRelease(ina);
	return 0;
}

// #64
INT WINAPI XNetXnAddrToMachineId(const XNADDR* pxnaddr, uint64_t* pqwMachineId)
{
	XLS_TRACE_FN();
	if (!pxnaddr || !pqwMachineId) {
		return WSAEFAULT;
	}
	CSteamID steamId;
	if (xls::NetSteamIdFromXnaddr(*pxnaddr, &steamId)) {
		*pqwMachineId = xls::MachineIdFromSteamId(steamId);
		return 0;
	}
	*pqwMachineId = 0xFA00000000000000ULL | xls::Fnv1a32(pxnaddr->abEnet, sizeof(pxnaddr->abEnet));
	return 0;
}

// #65
INT WINAPI XNetConnect(const IN_ADDR ina)
{
	XLS_TRACE_FN();
	xls::NetConnectStart(ina);
	return 0;
}

// #66
INT WINAPI XNetGetConnectStatus(const IN_ADDR ina)
{
	XLS_TRACE_FN();
	xls::SteamPump();
	return (INT)xls::NetConnectStatus(ina);
}

// #67
INT WINAPI XNetDnsLookup(const char* pszHost, WSAEVENT hEvent, XNDNS** ppxndns)
{
	XLS_TRACE_FN();
	if (!pszHost || !ppxndns) {
		return WSAEFAULT;
	}
	XNDNS* result = new XNDNS();
	memset(result, 0, sizeof(*result));
	result->iStatus = WSAEINPROGRESS;
	*ppxndns = result;
	DnsRequest* request = new DnsRequest{ result, pszHost, hEvent };
	std::thread(DnsWorker, request).detach();
	return 0;
}

// #68
INT WINAPI XNetDnsRelease(XNDNS* pxndns)
{
	XLS_TRACE_FN();
	if (!pxndns) {
		return WSAEFAULT;
	}
	if (pxndns->iStatus == WSAEINPROGRESS) {
		// The worker still writes into it, so the block is leaked on purpose.
		return 0;
	}
	delete pxndns;
	return 0;
}

// #69
INT WINAPI XNetQosListen(const XNKID* pxnkid, const uint8_t* pb, UINT cb, DWORD dwBitsPerSec, DWORD dwFlags)
{
	XLS_TRACE_FN();
	if (!pxnkid) {
		return WSAEFAULT;
	}
	return xls::NetQosListen(*pxnkid, pb, cb, dwBitsPerSec, dwFlags);
}

// #70
INT WINAPI XNetQosLookup(UINT cxnqos, const XNADDR* apxna[], const XNKID* apxnkid[], const XNKEY* apxnkey[], UINT cina, const IN_ADDR aina[], const DWORD adwServiceId[], UINT cProbes, DWORD dwBitsPerSec, DWORD dwFlags, WSAEVENT hEvent, XNQOS** ppxnqos)
{
	XLS_TRACE_FN();
	return xls::NetQosLookup(cxnqos, apxna, apxnkid, apxnkey, cina, aina, adwServiceId, cProbes, dwBitsPerSec, dwFlags, hEvent, ppxnqos);
}

// #71
INT WINAPI XNetQosServiceLookup(DWORD dwFlags, WSAEVENT hEvent, XNQOS** ppxnqos)
{
	XLS_TRACE_FN();
	if (!ppxnqos) {
		return WSAEFAULT;
	}
	IN_ADDR none = {};
	DWORD service = 0;
	return xls::NetQosLookup(0, nullptr, nullptr, nullptr, 1, &none, &service, 1, 0, dwFlags, hEvent, ppxnqos);
}

// #72
INT WINAPI XNetQosRelease(XNQOS* pxnqos)
{
	XLS_TRACE_FN();
	if (!pxnqos) {
		return WSAEFAULT;
	}
	return xls::NetQosRelease(pxnqos);
}

// #73
DWORD WINAPI XNetGetTitleXnAddr(XNADDR* pxna)
{
	XLS_TRACE_FN();
	if (!pxna) {
		return XNET_GET_XNADDR_NONE;
	}
	xls::NetLocalXnaddr(pxna);
	DWORD status = XNET_GET_XNADDR_ETHERNET | XNET_GET_XNADDR_DHCP | XNET_GET_XNADDR_GATEWAY | XNET_GET_XNADDR_DNS;
	if (xls::SteamOnline()) {
		status |= XNET_GET_XNADDR_ONLINE;
	}
	return status;
}

// #74
DWORD WINAPI XNetGetDebugXnAddr(XNADDR* pxna)
{
	XLS_TRACE_FN();
	return XNetGetTitleXnAddr(pxna);
}

// #75
DWORD WINAPI XNetGetEthernetLinkStatus()
{
	XLS_TRACE_FN();
	return XNET_ETHERNET_LINK_ACTIVE | XNET_ETHERNET_LINK_100MBPS | XNET_ETHERNET_LINK_FULL_DUPLEX;
}

// #76
DWORD WINAPI XNetGetBroadcastVersionStatus(BOOL fReset)
{
	XLS_TRACE_FN();
	return 0;
}

// #77
INT WINAPI XNetQosGetListenStats(const XNKID* pxnkid, XNQOSLISTENSTATS* pQosListenStats)
{
	XLS_TRACE_FN();
	if (!pxnkid || !pQosListenStats || pQosListenStats->dwSizeOfStruct != sizeof(XNQOSLISTENSTATS)) {
		return WSAEFAULT;
	}
	return xls::NetQosListenStats(*pxnkid, pQosListenStats);
}

// #78
INT WINAPI XNetGetOpt(DWORD dwOptId, uint8_t* pbValue, DWORD* pdwValueSize)
{
	XLS_TRACE_FN();
	if (!pbValue || !pdwValueSize) {
		return WSAEFAULT;
	}
	switch (dwOptId) {
		case XNET_OPTID_STARTUP_PARAMS:
			if (*pdwValueSize < sizeof(XNetStartupParams)) {
				*pdwValueSize = sizeof(XNetStartupParams);
				return WSAEMSGSIZE;
			}
			memcpy(pbValue, &g_startupParams, sizeof(XNetStartupParams));
			*pdwValueSize = sizeof(XNetStartupParams);
			return 0;
		case XNET_OPTID_NIC_XMIT_BYTES:
		case XNET_OPTID_NIC_RECV_BYTES:
		case XNET_OPTID_CALLER_XMIT_BYTES:
		case XNET_OPTID_CALLER_RECV_BYTES:
			if (*pdwValueSize < sizeof(uint64_t)) {
				*pdwValueSize = sizeof(uint64_t);
				return WSAEMSGSIZE;
			}
			memset(pbValue, 0, sizeof(uint64_t));
			*pdwValueSize = sizeof(uint64_t);
			return 0;
		case XNET_OPTID_NIC_XMIT_FRAMES:
		case XNET_OPTID_NIC_RECV_FRAMES:
		case XNET_OPTID_CALLER_XMIT_FRAMES:
		case XNET_OPTID_CALLER_RECV_FRAMES:
			if (*pdwValueSize < sizeof(DWORD)) {
				*pdwValueSize = sizeof(DWORD);
				return WSAEMSGSIZE;
			}
			memset(pbValue, 0, sizeof(DWORD));
			*pdwValueSize = sizeof(DWORD);
			return 0;
		default:
			return WSAEINVAL;
	}
}

// #79
INT WINAPI XNetSetOpt(DWORD dwOptId, const uint8_t* pbValue, DWORD dwValueSize)
{
	XLS_TRACE_FN();
	return WSAEINVAL;
}

// #82
INT WINAPI XNetGetXnAddrPlatform(const XNADDR* pxnaddr, DWORD* pdwPlatform)
{
	XLS_TRACE_FN();
	if (!pxnaddr || !pdwPlatform) {
		return WSAEFAULT;
	}
	*pdwPlatform = XNET_XNADDR_PLATFORM_WINPC;
	return 0;
}

// #83
INT WINAPI XNetGetSystemLinkPort(WORD* pwSystemLinkPort)
{
	XLS_TRACE_FN();
	if (!pwSystemLinkPort) {
		return WSAEFAULT;
	}
	*pwSystemLinkPort = g_systemLinkPort;
	return 0;
}

// #84
INT WINAPI XNetSetSystemLinkPort(WORD wSystemLinkPort)
{
	XLS_TRACE_FN();
	g_systemLinkPort = wSystemLinkPort;
	return 0;
}

// #5023
HRESULT WINAPI XNetGetCurrentAdapter(char* pszAdapter, DWORD* pdwSize)
{
	XLS_TRACE_FN();
	if (!pdwSize) {
		return E_POINTER;
	}
	static const char name[] = "Steam";
	if (!pszAdapter || *pdwSize < sizeof(name)) {
		*pdwSize = sizeof(name);
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(pszAdapter, name, sizeof(name));
	*pdwSize = sizeof(name);
	return S_OK;
}

// #5296
HRESULT WINAPI XLiveGetLocalOnlinePort(WORD* pwPort)
{
	XLS_TRACE_FN();
	if (!pwPort) {
		return E_POINTER;
	}
	*pwPort = htons(g_onlinePort);
	return S_OK;
}

// #5310
DWORD WINAPI XOnlineStartup()
{
	XLS_TRACE_FN();
	g_onlineStarted = true;
	return ERROR_SUCCESS;
}

// #5311
DWORD WINAPI XOnlineCleanup()
{
	XLS_TRACE_FN();
	g_onlineStarted = false;
	return ERROR_SUCCESS;
}

// #5324
XONLINE_NAT_TYPE WINAPI XOnlineGetNatType()
{
	XLS_TRACE_FN();
	// Steam Datagram Relay reaches every peer, so the NAT never restricts a session.
	return XONLINE_NAT_OPEN;
}

// #5359
HRESULT WINAPI XLiveGetUPnPState(XONLINE_NAT_TYPE* pNatType)
{
	XLS_TRACE_FN();
	if (!pNatType) {
		return E_POINTER;
	}
	*pNatType = XONLINE_NAT_OPEN;
	return S_OK;
}

// #5334
DWORD WINAPI XOnlineGetServiceInfo(DWORD dwServiceId, XONLINE_SERVICE_INFO* pServiceInfo)
{
	XLS_TRACE_FN();
	if (!pServiceInfo) {
		return ERROR_INVALID_PARAMETER;
	}
	for (const xls::TitleServerConfig& server : xls::Cfg().titleServers) {
		if (server.serviceId == dwServiceId) {
			pServiceInfo->dwServiceID = dwServiceId;
			inet_pton(AF_INET, server.ip.c_str(), &pServiceInfo->serviceIP);
			pServiceInfo->wServicePort = server.port;
			pServiceInfo->wReserved = 0;
			return ERROR_SUCCESS;
		}
	}
	return (DWORD)XONLINE_E_LOGON_SERVICE_NOT_REQUESTED;
}

namespace {

class TitleServerEnumerator : public xls::Enumerator {
public:
	std::vector<XTITLE_SERVER_INFO> servers;
	size_t index = 0;

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		DWORD count = 0;
		XTITLE_SERVER_INFO* out = (XTITLE_SERVER_INFO*)buffer;
		while (index < servers.size() && (count + 1) * sizeof(XTITLE_SERVER_INFO) <= bufferSize) {
			out[count++] = servers[index++];
		}
		*itemsReturned = count;
		return count ? ERROR_SUCCESS : ERROR_NO_MORE_FILES;
	}
};

}

// #5335
DWORD WINAPI XTitleServerCreateEnumerator(const char* pszServerInfo, DWORD cItem, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	auto enumerator = std::make_unique<TitleServerEnumerator>();
	for (const xls::TitleServerConfig& server : xls::Cfg().titleServers) {
		if (pszServerInfo && *pszServerInfo && server.info.find(pszServerInfo) == std::string::npos && server.name.find(pszServerInfo) == std::string::npos) {
			continue;
		}
		XTITLE_SERVER_INFO info = {};
		inet_pton(AF_INET, server.ip.c_str(), &info.inaServer);
		info.dwFlags = 0;
		xls::CopyStringA(info.szServerInfo, sizeof(info.szServerInfo), server.info.c_str());
		enumerator->servers.push_back(info);
	}
	*pcbBuffer = (cItem ? cItem : 1) * sizeof(XTITLE_SERVER_INFO);
	enumerator->requiredBufferSize = *pcbBuffer;
	*phEnum = xls::EnumeratorRegister(std::move(enumerator));
	return *phEnum ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}
