// Extension API for titles that link the wrapper as an SDK and move parts to Steamworks directly.
// Exported at ordinals 46000 and up. Nothing here is required by a title that only swaps the dll.
#pragma once

#include "xlive/xdefs.h"

#ifdef __cplusplus
extern "C" {
#endif

// --- Lifetime ----------------------------------------------------------------------------------

// Version of the wrapper as "major.minor.patch".
const char* WINAPI XlsVersion();

// Tell the wrapper that the title initialises and shuts down the Steam API itself. Call before
// XLiveInitialize. The wrapper then only verifies that Steam is available.
void WINAPI XlsSetSteamOwnedByTitle(BOOL ownedByTitle);

// Tell the wrapper whether it should call SteamAPI_RunCallbacks. A title that runs callbacks
// itself sets this to FALSE before XLiveInitialize, the wrapper still services its own state.
void WINAPI XlsSetRunSteamCallbacks(BOOL run);

// Services the wrapper (pending XOVERLAPPED jobs, network receive, notifications). XLiveRender,
// XLiveInput and XNotifyGetNext do this already, a title that calls none of them per frame calls
// this instead.
void WINAPI XlsRunFrame();

// Reads the SPAFILE resource from another module (a launcher hosting the wrapper while the SPA
// lives in the game dll). Language 0 follows the Steam game language.
BOOL WINAPI XlsLoadSpaFromModule(HMODULE hModule, uint8_t language);

// --- Identity ----------------------------------------------------------------------------------

// The XUID the wrapper hands out for a Steam user, and the way back. A XUID from the wrapper is
// XUID_LIVE_ENABLED_FLAG | account id, so IsOnlineXUID holds and no two Steam users collide.
XUID WINAPI XlsXuidFromSteamId(uint64_t steamId64);
uint64_t WINAPI XlsSteamIdFromXuid(XUID xuid);

// --- Sessions ----------------------------------------------------------------------------------

// The Steam lobby behind a session handle, or 0 when the session has no lobby yet.
uint64_t WINAPI XlsLobbyIdFromSession(HANDLE hSession);
// An XNKID is the lobby id so these convert without any lookup.
uint64_t WINAPI XlsLobbyIdFromXnkid(const XNKID* pxnkid);
void WINAPI XlsXnkidFromLobbyId(uint64_t lobbyId, XNKID* pxnkid);
// Builds an XSESSION_INFO from a lobby this client can see (after RequestLobbyData), so a title
// that finds lobbies through ISteamMatchmaking can still join them through XSessionCreate.
BOOL WINAPI XlsSessionInfoFromLobby(uint64_t lobbyId, XSESSION_INFO* pSessionInfo);

// --- Networking --------------------------------------------------------------------------------

// The secure address alias the XSocket layer uses for a Steam user, and the way back. A title
// that keeps XSocket* for its traffic but learns peers from Steam uses these.
IN_ADDR WINAPI XlsSecureAddrFromSteamId(uint64_t steamId64);
BOOL WINAPI XlsSteamIdFromSecureAddr(IN_ADDR secureAddr, uint64_t* pSteamId64);
// XNADDR for a Steam user, as XNetGetTitleXnAddr would produce it on that machine.
void WINAPI XlsXnaddrFromSteamId(uint64_t steamId64, XNADDR* pxnaddr);

typedef struct _XLS_CONNECTION_INFO {
	DWORD dwState;          // ESteamNetworkingConnectionState
	BOOL fRelayed;          // Traffic goes through a Steam relay (SDR) or a TURN server.
	BOOL fDirect;           // A direct peer-to-peer path is in use.
	DWORD dwPingMs;
	float flQualityLocal;   // 0..1, packets delivered in order, measured here.
	float flQualityRemote;  // 0..1, as measured by the peer.
	DWORD dwFlags;          // k_nSteamNetworkConnectionInfoFlags_*
	char szRelayPop[8];     // Relay data centre code, empty when direct.
	char szRemotePop[8];    // Data centre nearest to the peer, when known.
	char szRemoteAddr[48];  // Remote IP:port on a direct path, else empty.
	char szDescription[128];
} XLS_CONNECTION_INFO;

// State of the datagram (UDP/VDP) session with the peer behind a secure address.
BOOL WINAPI XlsPeerConnectionInfo(IN_ADDR secureAddr, XLS_CONNECTION_INFO* pInfo);
// State of the connection behind a connected or accepted stream (TCP) socket.
BOOL WINAPI XlsSocketConnectionInfo(SOCKET s, XLS_CONNECTION_INFO* pInfo);

#define XLS_P2P_TRANSPORT_AUTO        0   // Direct when the NATs allow it, relay otherwise.
#define XLS_P2P_TRANSPORT_RELAY_ONLY  1   // Never share IP addresses, every connection goes through a relay.
// Selects how peer connections are made. Call before any XNet or XSocket traffic. The config
// key "network": { "relay_only": true } does the same.
void WINAPI XlsSetP2PTransport(DWORD dwMode);

// --- Mappings ----------------------------------------------------------------------------------

// Override the Steam API name of an achievement id or the leaderboard name of a stats view at run
// time. These take precedence over xlive_steamworks.json.
void WINAPI XlsSetAchievementName(DWORD achievementId, const char* apiName);
void WINAPI XlsSetLeaderboardName(DWORD viewId, const char* leaderboardName);
const char* WINAPI XlsGetAchievementName(DWORD achievementId);
const char* WINAPI XlsGetLeaderboardName(DWORD viewId);

// --- Notifications -----------------------------------------------------------------------------

// Queues an XN_* notification for the title's listeners. A title that handles a Steam callback
// itself can still drive its GFWL-era notification code with it.
void WINAPI XlsPostNotification(DWORD notificationId, ULONG_PTR parameter);

#ifdef __cplusplus
}
#endif
