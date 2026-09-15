#include "xlive/xlive_steamworks.h"
#include "api/xsession.h"

#include "core/config.h"
#include "core/net.h"
#include "core/notify.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/utils.h"

#include <mutex>

namespace {

std::mutex g_mutex;
std::map<DWORD, std::string> g_achievementOverrides;
std::map<DWORD, std::string> g_leaderboardOverrides;
// Returned pointers stay valid until the next call for the same id.
std::map<DWORD, std::string> g_achievementNames;
std::map<DWORD, std::string> g_leaderboardNames;

}

namespace xls {

// Consulted by config.cpp before its own tables.
bool ExtAchievementName(uint32_t achievementId, std::string* out)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_achievementOverrides.find(achievementId);
	if (it == g_achievementOverrides.end()) {
		return false;
	}
	*out = it->second;
	return true;
}

bool ExtLeaderboardName(uint32_t viewId, std::string* out)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_leaderboardOverrides.find(viewId);
	if (it == g_leaderboardOverrides.end()) {
		return false;
	}
	*out = it->second;
	return true;
}

}

const char* WINAPI XlsVersion()
{
	static const std::string version = xls::FormatA("%u.%u.%u", XLS_VERSION_MAJOR, XLS_VERSION_MINOR, XLS_VERSION_PATCH);
	return version.c_str();
}

void WINAPI XlsSetSteamOwnedByTitle(BOOL ownedByTitle)
{
	xls::SteamSetOwnedByTitle(ownedByTitle != FALSE);
}

void WINAPI XlsSetRunSteamCallbacks(BOOL run)
{
	xls::SteamSetRunCallbacks(run != FALSE);
}

void WINAPI XlsRunFrame()
{
	xls::SteamPumpForce();
}

BOOL WINAPI XlsLoadSpaFromModule(HMODULE hModule, uint8_t language)
{
	return xls::spa::Load(language ? language : xls::SteamLanguageAsXLanguage(), hModule) ? TRUE : FALSE;
}

XUID WINAPI XlsXuidFromSteamId(uint64_t steamId64)
{
	return xls::XuidFromSteamId(CSteamID(steamId64));
}

uint64_t WINAPI XlsSteamIdFromXuid(XUID xuid)
{
	return xls::SteamIdFromXuid(xuid).ConvertToUint64();
}

uint64_t WINAPI XlsLobbyIdFromSession(HANDLE hSession)
{
	return xls::SessionLobbyId(hSession);
}

uint64_t WINAPI XlsLobbyIdFromXnkid(const XNKID* pxnkid)
{
	if (!pxnkid) {
		return 0;
	}
	uint64_t id;
	memcpy(&id, pxnkid->ab, sizeof(id));
	return id;
}

void WINAPI XlsXnkidFromLobbyId(uint64_t lobbyId, XNKID* pxnkid)
{
	if (pxnkid) {
		memcpy(pxnkid->ab, &lobbyId, sizeof(lobbyId));
	}
}

BOOL WINAPI XlsSessionInfoFromLobby(uint64_t lobbyId, XSESSION_INFO* pSessionInfo)
{
	if (!pSessionInfo) {
		return FALSE;
	}
	return xls::SessionInfoFromLobby(CSteamID(lobbyId), pSessionInfo) ? TRUE : FALSE;
}

IN_ADDR WINAPI XlsSecureAddrFromSteamId(uint64_t steamId64)
{
	return xls::NetSecureAddrFor(CSteamID(steamId64));
}

BOOL WINAPI XlsSteamIdFromSecureAddr(IN_ADDR secureAddr, uint64_t* pSteamId64)
{
	CSteamID steamId;
	if (!pSteamId64 || !xls::NetSecureAddrToSteamId(secureAddr, &steamId)) {
		return FALSE;
	}
	*pSteamId64 = steamId.ConvertToUint64();
	return TRUE;
}

void WINAPI XlsXnaddrFromSteamId(uint64_t steamId64, XNADDR* pxnaddr)
{
	if (pxnaddr) {
		xls::NetXnaddrForSteamId(CSteamID(steamId64), pxnaddr);
	}
}

void WINAPI XlsSetAchievementName(DWORD achievementId, const char* apiName)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (apiName && *apiName) {
		g_achievementOverrides[achievementId] = apiName;
	}
	else {
		g_achievementOverrides.erase(achievementId);
	}
}

void WINAPI XlsSetLeaderboardName(DWORD viewId, const char* leaderboardName)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (leaderboardName && *leaderboardName) {
		g_leaderboardOverrides[viewId] = leaderboardName;
	}
	else {
		g_leaderboardOverrides.erase(viewId);
	}
}

const char* WINAPI XlsGetAchievementName(DWORD achievementId)
{
	std::string name = xls::AchievementApiName(achievementId);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_achievementNames[achievementId] = name;
	return g_achievementNames[achievementId].c_str();
}

const char* WINAPI XlsGetLeaderboardName(DWORD viewId)
{
	std::string name = xls::LeaderboardName(viewId);
	std::lock_guard<std::mutex> lock(g_mutex);
	g_leaderboardNames[viewId] = name;
	return g_leaderboardNames[viewId].c_str();
}

void WINAPI XlsPostNotification(DWORD notificationId, ULONG_PTR parameter)
{
	xls::NotifyPost(notificationId, parameter);
}
