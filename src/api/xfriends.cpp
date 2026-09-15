// #5312, #5313, #5338, #5340, #5341: friends and presence.
#include "xlive/xfuncs.h"
#include "api/xlive.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/notify.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <algorithm>
#include <mutex>
#include <set>

namespace {

std::mutex g_mutex;
std::set<uint64_t> g_subscriptions;

DWORD FriendStateOf(CSteamID steamId, XNKID* sessionId, DWORD* titleId)
{
	DWORD state = XONLINE_FRIENDSTATE_ENUM_CONSOLE_WINPC;
	memset(sessionId, 0, sizeof(*sessionId));
	*titleId = 0;
	if (!xls::SteamReady()) {
		return state;
	}
	EPersonaState persona = xls::SteamFriends()->GetFriendPersonaState(steamId);
	if (persona != k_EPersonaStateOffline && persona != k_EPersonaStateInvisible) {
		state |= XONLINE_FRIENDSTATE_FLAG_ONLINE;
	}
	if (persona == k_EPersonaStateAway || persona == k_EPersonaStateSnooze) {
		state |= XONLINE_FRIENDSTATE_ENUM_AWAY;
	}
	else if (persona == k_EPersonaStateBusy) {
		state |= XONLINE_FRIENDSTATE_ENUM_BUSY;
	}
	FriendGameInfo_t game = {};
	if (xls::SteamFriends()->GetFriendGamePlayed(steamId, &game)) {
		state |= XONLINE_FRIENDSTATE_FLAG_PLAYING;
		if (game.m_gameID.AppID() == xls::SteamAppId()) {
			*titleId = xls::TitleId();
			if (game.m_steamIDLobby.IsValid()) {
				uint64_t lobby = game.m_steamIDLobby.ConvertToUint64();
				memcpy(sessionId->ab, &lobby, sizeof(lobby));
				state |= XONLINE_FRIENDSTATE_FLAG_JOINABLE;
			}
		}
	}
	return state;
}

std::wstring RichPresenceOf(CSteamID steamId)
{
	if (!xls::SteamReady()) {
		return std::wstring();
	}
	const char* status = xls::SteamFriends()->GetFriendRichPresence(steamId, xls::Cfg().richPresenceKey.c_str());
	if (!status || !*status) {
		FriendGameInfo_t game = {};
		if (xls::SteamFriends()->GetFriendGamePlayed(steamId, &game) && game.m_gameID.AppID() == xls::SteamAppId()) {
			return xls::spa::Loaded() ? xls::spa::TitleName() : std::wstring();
		}
		return std::wstring();
	}
	std::wstring text = xls::Utf8ToWide(status);
	if (text.size() >= MAX_RICHPRESENCE_SIZE) {
		text.resize(MAX_RICHPRESENCE_SIZE - 1);
	}
	return text;
}

class FriendsEnumerator : public xls::Enumerator {
public:
	std::vector<XONLINE_FRIEND> friends;
	size_t index = 0;
	DWORD batchSize = 0;

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		*itemsReturned = 0;
		if (index >= friends.size()) {
			return ERROR_NO_MORE_FILES;
		}
		XONLINE_FRIEND* out = (XONLINE_FRIEND*)buffer;
		DWORD count = 0;
		while (index < friends.size() && count < batchSize && (count + 1) * sizeof(XONLINE_FRIEND) <= bufferSize) {
			out[count++] = friends[index++];
		}
		*itemsReturned = count;
		return count ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
	}
};

class PresenceEnumerator : public xls::Enumerator {
public:
	std::vector<XONLINE_PRESENCE> peers;
	size_t index = 0;
	DWORD batchSize = 0;

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		*itemsReturned = 0;
		if (index >= peers.size()) {
			return ERROR_NO_MORE_FILES;
		}
		XONLINE_PRESENCE* out = (XONLINE_PRESENCE*)buffer;
		DWORD count = 0;
		while (index < peers.size() && count < batchSize && (count + 1) * sizeof(XONLINE_PRESENCE) <= bufferSize) {
			out[count++] = peers[index++];
		}
		*itemsReturned = count;
		return count ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
	}
};

}

namespace xls {
namespace events {

void OnPersonaStateChange(CSteamID user, int changeFlags)
{
	if (!SteamReady() || user == SteamLocalId()) {
		return;
	}
	bool friendNow = SteamFriends()->HasFriend(user, k_EFriendFlagImmediate);
	if (changeFlags & k_EPersonaChangeRelationshipChanged) {
		NotifyPost(friendNow ? XN_FRIENDS_FRIEND_ADDED : XN_FRIENDS_FRIEND_REMOVED, 1);
	}
	bool subscribed;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		subscribed = g_subscriptions.count(user.ConvertToUint64()) > 0;
	}
	if ((friendNow || subscribed) && (changeFlags & (k_EPersonaChangeStatus | k_EPersonaChangeGamePlayed | k_EPersonaChangeGameServer | k_EPersonaChangeComeOnline | k_EPersonaChangeGoneOffline | k_EPersonaChangeName))) {
		NotifyPost(XN_FRIENDS_PRESENCE_CHANGED, 1);
	}
}

void OnFriendRichPresenceUpdate(CSteamID user)
{
	if (user == SteamLocalId()) {
		return;
	}
	NotifyPost(XN_FRIENDS_PRESENCE_CHANGED, 1);
}

}
}

// #5312
DWORD WINAPI XFriendsCreateEnumerator(DWORD dwUserIndex, DWORD dwStartingIndex, DWORD dwFriendsToReturn, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (dwStartingIndex >= MAX_FRIENDS || !dwFriendsToReturn || dwFriendsToReturn > MAX_FRIENDS || dwStartingIndex + dwFriendsToReturn > MAX_FRIENDS || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	auto enumerator = std::make_unique<FriendsEnumerator>();
	enumerator->batchSize = dwFriendsToReturn;
	if (xls::SteamReady() && dwUserIndex == 0) {
		int count = xls::SteamFriends()->GetFriendCount(k_EFriendFlagImmediate);
		for (int i = 0; i < count && enumerator->friends.size() < MAX_FRIENDS; i++) {
			CSteamID steamId = xls::SteamFriends()->GetFriendByIndex(i, k_EFriendFlagImmediate);
			XONLINE_FRIEND entry = {};
			entry.xuid = xls::XuidFromSteamId(steamId);
			xls::CopyStringA(entry.szGamertag, sizeof(entry.szGamertag), xls::GamertagForSteamId(steamId).c_str());
			entry.dwFriendState = FriendStateOf(steamId, &entry.sessionID, &entry.dwTitleID);
			entry.ftUserTime = xls::NowFileTime();
			std::wstring presence = RichPresenceOf(steamId);
			xls::CopyStringW(entry.wszRichPresence, MAX_RICHPRESENCE_SIZE, presence.c_str());
			entry.cchRichPresence = (DWORD)wcslen(entry.wszRichPresence);
			enumerator->friends.push_back(entry);
		}
		// Online friends first, as the Guide listed them.
		std::stable_sort(enumerator->friends.begin(), enumerator->friends.end(), [](const XONLINE_FRIEND& a, const XONLINE_FRIEND& b) {
			bool aOnline = (a.dwFriendState & XONLINE_FRIENDSTATE_FLAG_ONLINE) != 0;
			bool bOnline = (b.dwFriendState & XONLINE_FRIENDSTATE_FLAG_ONLINE) != 0;
			if (aOnline != bOnline) return aOnline;
			bool aPlaying = a.dwTitleID != 0;
			bool bPlaying = b.dwTitleID != 0;
			return aPlaying && !bPlaying;
		});
	}
	enumerator->index = dwStartingIndex < enumerator->friends.size() ? dwStartingIndex : enumerator->friends.size();
	*pcbBuffer = dwFriendsToReturn * sizeof(XONLINE_FRIEND);
	enumerator->requiredBufferSize = *pcbBuffer;
	*phEnum = xls::EnumeratorRegister(std::move(enumerator));
	return *phEnum ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

// #5313
DWORD WINAPI XPresenceInitialize(DWORD cPeerSubscriptions)
{
	XLS_TRACE_FN();
	if (cPeerSubscriptions > XPRESENCE_MAX_TITLE_SUBS) {
		return ERROR_INVALID_PARAMETER;
	}
	return ERROR_SUCCESS;
}

// #5338
DWORD WINAPI XPresenceSubscribe(DWORD dwUserIndex, DWORD cPeers, const XUID* pPeers)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cPeers || !pPeers) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	for (DWORD i = 0; i < cPeers; i++) {
		CSteamID steamId = xls::SteamIdFromXuid(pPeers[i]);
		if (!steamId.IsValid()) {
			continue;
		}
		g_subscriptions.insert(steamId.ConvertToUint64());
		if (xls::SteamReady()) {
			xls::SteamFriends()->RequestFriendRichPresence(steamId);
			xls::SteamFriends()->RequestUserInformation(steamId, false);
		}
	}
	return ERROR_SUCCESS;
}

// #5341
DWORD WINAPI XPresenceUnsubscribe(DWORD dwUserIndex, DWORD cPeers, const XUID* pPeers)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cPeers || !pPeers) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	for (DWORD i = 0; i < cPeers; i++) {
		CSteamID steamId = xls::SteamIdFromXuid(pPeers[i]);
		g_subscriptions.erase(steamId.ConvertToUint64());
	}
	return ERROR_SUCCESS;
}

// #5340
DWORD WINAPI XPresenceCreateEnumerator(DWORD dwUserIndex, DWORD cPeers, const XUID* pPeers, DWORD dwStartingIndex, DWORD cPeersToReturn, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cPeers || cPeers > MAX_PRESENCE || !pPeers || !cPeersToReturn || cPeersToReturn > MAX_PRESENCE || dwStartingIndex >= cPeers || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	auto enumerator = std::make_unique<PresenceEnumerator>();
	enumerator->batchSize = cPeersToReturn;
	for (DWORD i = 0; i < cPeers; i++) {
		CSteamID steamId = xls::SteamIdFromXuid(pPeers[i]);
		XONLINE_PRESENCE entry = {};
		entry.xuid = pPeers[i];
		if (steamId.IsValid()) {
			entry.dwState = FriendStateOf(steamId, &entry.sessionID, &entry.dwTitleID);
			std::wstring presence = RichPresenceOf(steamId);
			xls::CopyStringW(entry.wszRichPresence, MAX_RICHPRESENCE_SIZE, presence.c_str());
			entry.cchRichPresence = (DWORD)wcslen(entry.wszRichPresence);
		}
		entry.ftUserTime = xls::NowFileTime();
		enumerator->peers.push_back(entry);
	}
	enumerator->index = dwStartingIndex;
	*pcbBuffer = cPeersToReturn * sizeof(XONLINE_PRESENCE);
	enumerator->requiredBufferSize = *pcbBuffer;
	*phEnum = xls::EnumeratorRegister(std::move(enumerator));
	return *phEnum ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}
