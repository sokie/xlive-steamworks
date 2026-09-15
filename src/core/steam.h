// Steam client lifetime, the callback pump and identity conversions.
#pragma once

#pragma warning(push)
#pragma warning(disable : 4996)
#include <steam/steam_api.h>
#pragma warning(pop)

#include "xlive/xdefs.h"

#include <string>

namespace xls {

// The Steam accessors are global, modules reach them through this namespace.
using ::SteamClient;
using ::SteamUser;
using ::SteamFriends;
using ::SteamUtils;
using ::SteamMatchmaking;
using ::SteamUserStats;
using ::SteamApps;
using ::SteamRemoteStorage;
using ::SteamNetworkingMessages;
using ::SteamNetworkingSockets;
using ::SteamNetworkingUtils;

bool SteamStart();
void SteamStop();
// A title that calls SteamAPI_Init itself sets this before XLiveInitialize.
void SteamSetOwnedByTitle(bool ownedByTitle);
// A title that calls SteamAPI_RunCallbacks itself sets this to false before XLiveInitialize.
void SteamSetRunCallbacks(bool run);
// True once SteamAPI_Init succeeded. A title still runs, signed out, when it did not.
bool SteamReady();
// True while the client reports a connection to the Steam servers.
bool SteamOnline();

// Runs Steam callbacks, pending async jobs and the network receive path. Rate limited so the
// many call sites that poll (XNotifyGetNext, XSocketRecvFrom, XLiveRender) stay cheap.
void SteamPump();
void SteamPumpForce();

CSteamID SteamLocalId();
uint32_t SteamAppId();
uint8_t SteamLanguageAsXLanguage();

// A LIVE-enabled XUID that carries the Steam account id. Titles test IsOnlineXUID on it.
XUID XuidFromSteamId(CSteamID steamId);
CSteamID SteamIdFromXuid(XUID xuid);
bool XuidIsSteamUser(XUID xuid);
uint64_t MachineIdFromSteamId(CSteamID steamId);

// Polls one SteamAPICall_t from the pump instead of a CCallResult, so a result can be waited
// for from any thread.
template <typename T>
struct SteamCall {
	SteamAPICall_t call = k_uAPICallInvalid;
	T result = {};
	bool failed = false;
	bool done = false;

	void Start(SteamAPICall_t handle)
	{
		call = handle;
		failed = handle == k_uAPICallInvalid;
		done = failed;
	}

	bool Poll()
	{
		if (done) {
			return true;
		}
		bool callFailed = false;
		if (!SteamUtils() || !SteamUtils()->IsAPICallCompleted(call, &callFailed)) {
			return false;
		}
		if (callFailed) {
			failed = true;
		}
		else if (!SteamUtils()->GetAPICallResult(call, &result, sizeof(T), T::k_iCallback, &callFailed)) {
			failed = true;
		}
		else {
			failed = callFailed;
		}
		done = true;
		return true;
	}
};

// Steam callbacks that modules act on. Each is implemented by the module that owns the state.
namespace events {
void OnPersonaStateChange(CSteamID user, int changeFlags);
void OnFriendRichPresenceUpdate(CSteamID user);
void OnLobbyJoinRequested(CSteamID lobby, CSteamID inviter);
void OnRichPresenceJoinRequested(CSteamID inviter, const char* connect);
void OnLobbyChatUpdate(const LobbyChatUpdate_t& update);
void OnLobbyDataUpdate(const LobbyDataUpdate_t& update);
void OnLobbyKicked(const LobbyKicked_t& kicked);
void OnNewLaunchParameters();
void OnDlcInstalled(AppId_t appId);
void OnNetSessionRequest(const SteamNetworkingIdentity& remote);
void OnNetSessionFailed(const SteamNetConnectionInfo_t& info);
void OnNetConnectionStatusChanged(const SteamNetConnectionStatusChangedCallback_t& change);
void OnUserStatsReceived(const UserStatsReceived_t& received);
void OnGamepadTextDismissed(bool submitted, uint32_t length);
void OnAvatarLoaded(const AvatarImageLoaded_t& loaded);
}

}
