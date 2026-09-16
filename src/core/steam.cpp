#include "core/steam.h"

#include "core/config.h"
#include "core/log.h"
#include "core/net.h"
#include "core/notify.h"
#include "core/overlapped.h"
#include "core/utils.h"

#include <mutex>

namespace xls {

namespace {

// Every Steam callback lands here and is forwarded to the module that owns the state, so the
// module files stay free of the Steamworks callback plumbing.
class SteamBridge {
public:
	STEAM_CALLBACK(SteamBridge, OnOverlayActivated, GameOverlayActivated_t);
	STEAM_CALLBACK(SteamBridge, OnPersonaStateChange, PersonaStateChange_t);
	STEAM_CALLBACK(SteamBridge, OnFriendRichPresenceUpdate, FriendRichPresenceUpdate_t);
	STEAM_CALLBACK(SteamBridge, OnLobbyJoinRequested, GameLobbyJoinRequested_t);
	STEAM_CALLBACK(SteamBridge, OnRichPresenceJoinRequested, GameRichPresenceJoinRequested_t);
	STEAM_CALLBACK(SteamBridge, OnLobbyChatUpdate, LobbyChatUpdate_t);
	STEAM_CALLBACK(SteamBridge, OnLobbyDataUpdate, LobbyDataUpdate_t);
	STEAM_CALLBACK(SteamBridge, OnLobbyKicked, LobbyKicked_t);
	STEAM_CALLBACK(SteamBridge, OnServersConnected, SteamServersConnected_t);
	STEAM_CALLBACK(SteamBridge, OnServersDisconnected, SteamServersDisconnected_t);
	STEAM_CALLBACK(SteamBridge, OnServerConnectFailure, SteamServerConnectFailure_t);
	STEAM_CALLBACK(SteamBridge, OnNewLaunchParameters, NewUrlLaunchParameters_t);
	STEAM_CALLBACK(SteamBridge, OnDlcInstalled, DlcInstalled_t);
	STEAM_CALLBACK(SteamBridge, OnNetSessionRequest, SteamNetworkingMessagesSessionRequest_t);
	STEAM_CALLBACK(SteamBridge, OnNetSessionFailed, SteamNetworkingMessagesSessionFailed_t);
	STEAM_CALLBACK(SteamBridge, OnNetConnectionStatusChanged, SteamNetConnectionStatusChangedCallback_t);
	STEAM_CALLBACK(SteamBridge, OnUserStatsReceived, UserStatsReceived_t);
	STEAM_CALLBACK(SteamBridge, OnGamepadTextDismissed, GamepadTextInputDismissed_t);
	STEAM_CALLBACK(SteamBridge, OnAvatarLoaded, AvatarImageLoaded_t);
	STEAM_CALLBACK(SteamBridge, OnSteamShutdown, SteamShutdown_t);
};

void SteamBridge::OnOverlayActivated(GameOverlayActivated_t* p)
{
	XLS_LOG_DEBUG("steam: overlay %s.", p->m_bActive ? "opened" : "closed");
	NotifySystemUi(p->m_bActive != 0);
}

void SteamBridge::OnPersonaStateChange(PersonaStateChange_t* p)
{
	events::OnPersonaStateChange(CSteamID(p->m_ulSteamID), p->m_nChangeFlags);
}

void SteamBridge::OnFriendRichPresenceUpdate(FriendRichPresenceUpdate_t* p)
{
	events::OnFriendRichPresenceUpdate(p->m_steamIDFriend);
}

void SteamBridge::OnLobbyJoinRequested(GameLobbyJoinRequested_t* p)
{
	events::OnLobbyJoinRequested(p->m_steamIDLobby, p->m_steamIDFriend);
}

void SteamBridge::OnRichPresenceJoinRequested(GameRichPresenceJoinRequested_t* p)
{
	events::OnRichPresenceJoinRequested(p->m_steamIDFriend, p->m_rgchConnect);
}

void SteamBridge::OnLobbyChatUpdate(LobbyChatUpdate_t* p)
{
	events::OnLobbyChatUpdate(*p);
}

void SteamBridge::OnLobbyDataUpdate(LobbyDataUpdate_t* p)
{
	events::OnLobbyDataUpdate(*p);
}

void SteamBridge::OnLobbyKicked(LobbyKicked_t* p)
{
	events::OnLobbyKicked(*p);
}

void SteamBridge::OnServersConnected(SteamServersConnected_t*)
{
	XLS_LOG_INFO("steam: connected to Steam servers.");
	NotifyPost(XN_LIVE_CONNECTIONCHANGED, (ULONG_PTR)XONLINE_S_LOGON_CONNECTION_ESTABLISHED);
}

void SteamBridge::OnServersDisconnected(SteamServersDisconnected_t* p)
{
	XLS_LOG_WARN("steam: disconnected from Steam servers (EResult %d).", (int)p->m_eResult);
	NotifyPost(XN_LIVE_CONNECTIONCHANGED, (ULONG_PTR)XONLINE_E_LOGON_CONNECTION_LOST);
}

void SteamBridge::OnServerConnectFailure(SteamServerConnectFailure_t* p)
{
	XLS_LOG_WARN("steam: connection to Steam servers failed (EResult %d, still retrying %d).", (int)p->m_eResult, (int)p->m_bStillRetrying);
	if (!p->m_bStillRetrying) {
		NotifyPost(XN_LIVE_CONNECTIONCHANGED, (ULONG_PTR)XONLINE_E_LOGON_CANNOT_ACCESS_SERVICE);
	}
}

void SteamBridge::OnNewLaunchParameters(NewUrlLaunchParameters_t*)
{
	events::OnNewLaunchParameters();
}

void SteamBridge::OnDlcInstalled(DlcInstalled_t* p)
{
	events::OnDlcInstalled(p->m_nAppID);
}

void SteamBridge::OnNetSessionRequest(SteamNetworkingMessagesSessionRequest_t* p)
{
	events::OnNetSessionRequest(p->m_identityRemote);
}

void SteamBridge::OnNetSessionFailed(SteamNetworkingMessagesSessionFailed_t* p)
{
	events::OnNetSessionFailed(p->m_info);
}

void SteamBridge::OnNetConnectionStatusChanged(SteamNetConnectionStatusChangedCallback_t* p)
{
	events::OnNetConnectionStatusChanged(*p);
}

void SteamBridge::OnUserStatsReceived(UserStatsReceived_t* p)
{
	events::OnUserStatsReceived(*p);
}

void SteamBridge::OnGamepadTextDismissed(GamepadTextInputDismissed_t* p)
{
	events::OnGamepadTextDismissed(p->m_bSubmitted, p->m_unSubmittedText);
}

void SteamBridge::OnAvatarLoaded(AvatarImageLoaded_t* p)
{
	events::OnAvatarLoaded(*p);
}

void SteamBridge::OnSteamShutdown(SteamShutdown_t*)
{
	XLS_LOG_WARN("steam: the Steam client is shutting down.");
	NotifyPost(XN_LIVE_CONNECTIONCHANGED, (ULONG_PTR)XONLINE_E_LOGON_CONNECTION_LOST);
}

SteamBridge* g_bridge = nullptr;
bool g_ready = false;
bool g_ownedByTitle = false;
bool g_runCallbacks = true;
std::recursive_mutex g_pumpMutex;
bool g_pumping = false;
DWORD g_lastPumpTick = 0;

extern "C" void __cdecl SteamWarningHook(int severity, const char* message)
{
	if (severity) {
		XLS_LOG_WARN("steam: %s", message);
	}
	else {
		XLS_LOG_INFO("steam: %s", message);
	}
}

}

bool SteamStart()
{
	if (g_ready) {
		return true;
	}

	const Config& config = Cfg();
	if (config.appId) {
		// steam_api reads the app id from the launcher, steam_appid.txt or this variable, in that
		// order of preference. Setting it here covers a title started outside Steam during
		// development.
		char existing[16] = {};
		if (!GetEnvironmentVariableA("SteamAppId", existing, sizeof(existing))) {
			std::string appId = std::to_string(config.appId);
			SetEnvironmentVariableA("SteamAppId", appId.c_str());
			SetEnvironmentVariableA("SteamGameId", appId.c_str());
		}
		if (config.restartAppIfNecessary && SteamAPI_RestartAppIfNecessary(config.appId)) {
			XLS_LOG_INFO("steam: relaunching through Steam.");
			ExitProcess(0);
		}
	}

	SteamErrMsg error = {};
	ESteamAPIInitResult result = k_ESteamAPIInitResult_OK;
	if (!g_ownedByTitle && SteamAPI_GetHSteamPipe() && SteamAPI_GetHSteamUser()) {
		// The exe already called SteamAPI_Init (a Steam build with DLC checks) so share it.
		g_ownedByTitle = true;
		XLS_LOG_INFO("steam: the title initialised the Steam API itself.");
	}
	if (g_ownedByTitle) {
		if (!SteamUser() || !SteamUtils()) {
			XLS_LOG_ERROR("steam: the title owns the Steam API but it is not initialised.");
			return false;
		}
	}
	else {
		result = SteamAPI_InitEx(&error);
	}
	if (result != k_ESteamAPIInitResult_OK) {
		XLS_LOG_ERROR("steam: SteamAPI_InitEx failed (%d): %s", (int)result, error);
		if (config.requireSteam) {
			std::wstring text = L"Steam must be running, logged in, and this account must own the game to play online.\n\nSteam said: " + Utf8ToWide(error) + L"\n\nThe game will continue in offline mode.";
			MessageBoxW(nullptr, text.c_str(), L"Steam not available", MB_OK | MB_ICONWARNING);
		}
		return false;
	}

	SteamClient()->SetWarningMessageHook(&SteamWarningHook);
	g_bridge = new SteamBridge();
	g_ready = true;

	SteamNetworkingUtils()->InitRelayNetworkAccess();

	XLS_LOG_INFO("steam: initialised app %u as %s (%llu).", SteamUtils()->GetAppID(), SteamFriends()->GetPersonaName(), SteamUser()->GetSteamID().ConvertToUint64());
	return true;
}

void SteamStop()
{
	if (!g_ready) {
		return;
	}
	g_ready = false;
	delete g_bridge;
	g_bridge = nullptr;
	if (g_ownedByTitle) {
		return;
	}
	// Closed peer connections keep handshaking on Steam's networking thread for a few seconds,
	// so SteamAPI_Shutdown races that thread and crashes. Process exit ends the thread first.
	if (!Cfg().steamShutdownApi || NetHadPeers()) {
		XLS_LOG_INFO("steam: the Steam API stays up until the process exits.");
		return;
	}
	SteamAPI_Shutdown();
}

void SteamSetOwnedByTitle(bool ownedByTitle)
{
	g_ownedByTitle = ownedByTitle;
}

void SteamSetRunCallbacks(bool run)
{
	g_runCallbacks = run;
}

bool SteamReady()
{
	return g_ready;
}

bool SteamOnline()
{
	return g_ready && SteamUser() && SteamUser()->BLoggedOn();
}

void SteamPumpForce()
{
	std::unique_lock<std::recursive_mutex> lock(g_pumpMutex, std::try_to_lock);
	if (!lock.owns_lock()) {
		return;
	}
	if (g_pumping) {
		// A completion routine or Steam callback re-entered an X* call, the outer pump continues.
		return;
	}
	g_pumping = true;
	g_lastPumpTick = GetTickCount();
	if (g_ready && g_runCallbacks) {
		SteamAPI_RunCallbacks();
	}
	NetPump();
	AsyncPump();
	g_pumping = false;
}

void SteamPump()
{
	DWORD now = GetTickCount();
	if (now == g_lastPumpTick) {
		return;
	}
	SteamPumpForce();
}

CSteamID SteamLocalId()
{
	if (!g_ready || !SteamUser()) {
		return k_steamIDNil;
	}
	return SteamUser()->GetSteamID();
}

uint32_t SteamAppId()
{
	if (!g_ready || !SteamUtils()) {
		return Cfg().appId;
	}
	return SteamUtils()->GetAppID();
}

uint8_t SteamLanguageAsXLanguage()
{
	if (!g_ready || !SteamApps()) {
		return XLANGUAGE_ENGLISH;
	}
	std::string language = SteamApps()->GetCurrentGameLanguage();
	if (language == "japanese") return XLANGUAGE_JAPANESE;
	if (language == "german") return XLANGUAGE_GERMAN;
	if (language == "french") return XLANGUAGE_FRENCH;
	if (language == "spanish" || language == "latam") return XLANGUAGE_SPANISH;
	if (language == "italian") return XLANGUAGE_ITALIAN;
	if (language == "koreana") return XLANGUAGE_KOREAN;
	if (language == "tchinese") return XLANGUAGE_TCHINESE;
	if (language == "portuguese" || language == "brazilian") return XLANGUAGE_PORTUGUESE;
	if (language == "schinese") return XLANGUAGE_SCHINESE;
	if (language == "polish") return XLANGUAGE_POLISH;
	if (language == "russian") return XLANGUAGE_RUSSIAN;
	return XLANGUAGE_ENGLISH;
}

XUID XuidFromSteamId(CSteamID steamId)
{
	if (!steamId.IsValid()) {
		return INVALID_XUID;
	}
	return XUID_LIVE_ENABLED_FLAG | (XUID)steamId.GetAccountID();
}

CSteamID SteamIdFromXuid(XUID xuid)
{
	if (!XuidIsSteamUser(xuid)) {
		return k_steamIDNil;
	}
	return CSteamID((uint32)(xuid & 0xFFFFFFFF), k_EUniversePublic, k_EAccountTypeIndividual);
}

bool XuidIsSteamUser(XUID xuid)
{
	return IsOnlineXUID(xuid) && (xuid & 0xFFFFFFFF) != 0 && !IsGuestXUID(xuid);
}

uint64_t MachineIdFromSteamId(CSteamID steamId)
{
	// GFWL machine ids carry 0xFA in the top byte, the account id keeps them unique per player.
	return 0xFA00000000000000ULL | (uint64_t)steamId.GetAccountID();
}

}
