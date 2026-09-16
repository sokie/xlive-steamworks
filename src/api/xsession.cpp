// #5300, #5315 - #5336: a session is a Steam lobby. The lobby id is the XNKID and the lobby data carries the rest.
#include "xlive/xfuncs.h"
#include "api/xlive.h"
#include "api/xsession.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/net.h"
#include "core/notify.h"
#include "core/overlapped.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <set>

namespace {

struct Member {
	XUID xuid;
	DWORD userIndex;
	DWORD flags;
};

struct Session {
	HANDLE handle = nullptr;
	DWORD flags = 0;
	DWORD userIndex = 0;
	bool host = false;
	CSteamID lobby;
	XSESSION_INFO info = {};
	uint64_t nonce = 0;
	DWORD maxPublic = 0;
	DWORD maxPrivate = 0;
	DWORD gameType = 0;
	DWORD gameMode = 0;
	XSESSION_STATE state = XSESSION_STATE_LOBBY;
	std::vector<Member> members; // Declared through XSessionJoin*, merged with lobby membership.
	bool deleted = false;
	uint64_t hostSeq = 0;        // Ordering of host claims: the highest one names the host.
	bool pendingPublish = false; // Migrated to host before Steam handed this machine the lobby.
};

std::recursive_mutex g_mutex;
std::map<HANDLE, std::shared_ptr<Session>> g_sessions;
std::set<uint64_t> g_lobbyDataArrived;
std::shared_ptr<XINVITE_INFO> g_acceptedInvite;
CSteamID g_pendingInviteLobby;
CSteamID g_pendingInviter;

std::string Key(const char* name)
{
	return xls::Cfg().lobbyKeyPrefix + name;
}

std::string ContextKey(DWORD contextId)
{
	return xls::FormatA("%sc%08x", xls::Cfg().lobbyKeyPrefix.c_str(), contextId);
}

std::string PropertyKey(DWORD propertyId)
{
	return xls::FormatA("%sp%08x", xls::Cfg().lobbyKeyPrefix.c_str(), propertyId);
}

XNKID KidFromLobby(CSteamID lobby)
{
	XNKID xnkid;
	uint64_t id = lobby.ConvertToUint64();
	memcpy(xnkid.ab, &id, sizeof(id));
	return xnkid;
}

CSteamID LobbyFromKid(const XNKID& xnkid)
{
	uint64_t id;
	memcpy(&id, xnkid.ab, sizeof(id));
	CSteamID lobby(id);
	return lobby.IsLobby() ? lobby : k_steamIDNil;
}

std::shared_ptr<Session> Find(HANDLE handle)
{
	auto it = g_sessions.find(handle);
	return it == g_sessions.end() ? nullptr : it->second;
}

std::shared_ptr<Session> FindByLobby(CSteamID lobby)
{
	for (auto& entry : g_sessions) {
		if (entry.second->lobby == lobby && !entry.second->deleted) {
			return entry.second;
		}
	}
	return nullptr;
}

ELobbyType LobbyTypeFor(DWORD flags)
{
	if (flags & XSESSION_CREATE_USES_MATCHMAKING) {
		return k_ELobbyTypePublic;
	}
	if ((flags & XSESSION_CREATE_USES_PRESENCE) && !(flags & XSESSION_CREATE_JOIN_VIA_PRESENCE_DISABLED)) {
		return (flags & XSESSION_CREATE_JOIN_VIA_PRESENCE_FRIENDS_ONLY) ? k_ELobbyTypeFriendsOnly : k_ELobbyTypeFriendsOnly;
	}
	return k_ELobbyTypeInvisible;
}

// Writes what a joiner needs to know about the session into the lobby data.
void PublishSession(Session& session)
{
	ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
	CSteamID lobby = session.lobby;
	matchmaking->SetLobbyData(lobby, Key("kind").c_str(), "session");
	matchmaking->SetLobbyData(lobby, Key("title").c_str(), xls::FormatA("%u", xls::TitleId()).c_str());
	matchmaking->SetLobbyData(lobby, Key("flags").c_str(), xls::FormatA("%u", session.flags).c_str());
	matchmaking->SetLobbyData(lobby, Key("pub").c_str(), xls::FormatA("%u", session.maxPublic).c_str());
	matchmaking->SetLobbyData(lobby, Key("priv").c_str(), xls::FormatA("%u", session.maxPrivate).c_str());
	matchmaking->SetLobbyData(lobby, Key("nonce").c_str(), xls::FormatA("%llu", session.nonce).c_str());
	matchmaking->SetLobbyData(lobby, Key("xnkey").c_str(), xls::HexEncode(session.info.keyExchangeKey.ab, sizeof(session.info.keyExchangeKey.ab)).c_str());
	matchmaking->SetLobbyData(lobby, Key("host").c_str(), xls::HexEncode(&session.info.hostAddress, sizeof(session.info.hostAddress)).c_str());
	matchmaking->SetLobbyData(lobby, Key("hostseq").c_str(), xls::FormatA("%llu", session.hostSeq).c_str());
	matchmaking->SetLobbyData(lobby, Key("state").c_str(), xls::FormatA("%u", (unsigned)session.state).c_str());
	matchmaking->SetLobbyData(lobby, Key("gt").c_str(), xls::FormatA("%u", session.gameType).c_str());
	matchmaking->SetLobbyData(lobby, Key("gm").c_str(), xls::FormatA("%u", session.gameMode).c_str());
	for (const auto& context : xls::UserContexts(session.userIndex)) {
		matchmaking->SetLobbyData(lobby, ContextKey(context.first).c_str(), xls::FormatA("%u", context.second).c_str());
	}
	for (const auto& property : xls::UserProperties(session.userIndex)) {
		if (property.second.type == XUSER_DATA_TYPE_BINARY && property.second.raw.size() > 2048) {
			continue;
		}
		matchmaking->SetLobbyData(lobby, PropertyKey(property.first).c_str(), property.second.ToString().c_str());
	}
	bool joinable = session.state != XSESSION_STATE_INGAME || !(session.flags & XSESSION_CREATE_JOIN_IN_PROGRESS_DISABLED);
	matchmaking->SetLobbyJoinable(lobby, joinable);
}

void PublishRichPresenceConnect(const Session* session)
{
	if (!xls::SteamReady() || !xls::SteamFriends()) {
		return;
	}
	if (session && session->lobby.IsValid() && (session->flags & XSESSION_CREATE_USES_PRESENCE) && !(session->flags & XSESSION_CREATE_JOIN_VIA_PRESENCE_DISABLED)) {
		std::string connect = xls::FormatA("+connect_lobby %llu", session->lobby.ConvertToUint64());
		xls::SteamFriends()->SetRichPresence("connect", connect.c_str());
	}
	else {
		xls::SteamFriends()->SetRichPresence("connect", nullptr);
	}
}

// A member that took over as host publishes "<xnaddr hex>|<seq>" in its member data, which
// can be done without lobby ownership. Returns the member with the newest claim above minSeq, or nil.
CSteamID NewestHostClaim(CSteamID lobby, uint64_t minSeq, XNADDR* hostAddress, uint64_t* seq)
{
	ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
	CSteamID claimant;
	int count = matchmaking->GetNumLobbyMembers(lobby);
	for (int i = 0; i < count; i++) {
		CSteamID member = matchmaking->GetLobbyMemberByIndex(lobby, i);
		const char* claim = matchmaking->GetLobbyMemberData(lobby, member, Key("host").c_str());
		const char* separator = claim ? strchr(claim, '|') : nullptr;
		if (!separator) {
			continue;
		}
		uint64_t claimSeq = _strtoui64(separator + 1, nullptr, 10);
		XNADDR address = {};
		if (claimSeq <= minSeq || !xls::HexDecode(std::string(claim, separator).c_str(), &address, sizeof(address))) {
			continue;
		}
		minSeq = claimSeq;
		claimant = member;
		if (hostAddress) {
			*hostAddress = address;
		}
		if (seq) {
			*seq = claimSeq;
		}
	}
	return claimant;
}

uint64_t LobbyHostSeq(CSteamID lobby)
{
	return _strtoui64(xls::SteamMatchmaking()->GetLobbyData(lobby, Key("hostseq").c_str()), nullptr, 10);
}

// One above every sequence this client can see, so a claim never depends on a wall clock.
uint64_t NextHostSeq(CSteamID lobby)
{
	uint64_t seq = LobbyHostSeq(lobby);
	uint64_t claimSeq = 0;
	if (NewestHostClaim(lobby, 0, nullptr, &claimSeq).IsValid() && claimSeq > seq) {
		seq = claimSeq;
	}
	return seq + 1;
}

bool ReadSessionInfo(CSteamID lobby, XSESSION_INFO* info)
{
	ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
	const char* kind = matchmaking->GetLobbyData(lobby, Key("kind").c_str());
	if (!kind || strcmp(kind, "session") != 0) {
		return false;
	}
	memset(info, 0, sizeof(*info));
	info->sessionID = KidFromLobby(lobby);
	xls::HexDecode(matchmaking->GetLobbyData(lobby, Key("xnkey").c_str()), info->keyExchangeKey.ab, sizeof(info->keyExchangeKey.ab));
	if (!xls::HexDecode(matchmaking->GetLobbyData(lobby, Key("host").c_str()), &info->hostAddress, sizeof(info->hostAddress))) {
		xls::NetXnaddrForSteamId(matchmaking->GetLobbyOwner(lobby), &info->hostAddress);
	}
	// Member data is only current for a lobby this client is in
	if (FindByLobby(lobby)) {
		NewestHostClaim(lobby, LobbyHostSeq(lobby), &info->hostAddress, nullptr);
	}
	return true;
}

struct SearchEntry {
	XSESSION_SEARCHRESULT result;
	std::vector<XUSER_CONTEXT> contexts;
	std::vector<XUSER_PROPERTY> properties;
	std::vector<xls::StoredProperty> values;
};

bool ReadSearchEntry(CSteamID lobby, SearchEntry& entry)
{
	ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
	memset(&entry.result, 0, sizeof(entry.result));
	if (!ReadSessionInfo(lobby, &entry.result.info)) {
		return false;
	}
	DWORD maxPublic = (DWORD)strtoul(matchmaking->GetLobbyData(lobby, Key("pub").c_str()), nullptr, 10);
	DWORD maxPrivate = (DWORD)strtoul(matchmaking->GetLobbyData(lobby, Key("priv").c_str()), nullptr, 10);
	DWORD members = (DWORD)matchmaking->GetNumLobbyMembers(lobby);
	DWORD filledPublic = members > maxPublic ? maxPublic : members;
	DWORD filledPrivate = members > maxPublic ? std::min<DWORD>(members - maxPublic, maxPrivate) : 0;
	entry.result.dwFilledPublicSlots = filledPublic;
	entry.result.dwFilledPrivateSlots = filledPrivate;
	entry.result.dwOpenPublicSlots = maxPublic - filledPublic;
	entry.result.dwOpenPrivateSlots = maxPrivate - filledPrivate;

	int count = matchmaking->GetLobbyDataCount(lobby);
	std::string contextPrefix = xls::Cfg().lobbyKeyPrefix + "c";
	std::string propertyPrefix = xls::Cfg().lobbyKeyPrefix + "p";
	for (int i = 0; i < count; i++) {
		char key[k_nMaxLobbyKeyLength] = {};
		char value[k_cubChatMetadataMax] = {};
		if (!matchmaking->GetLobbyDataByIndex(lobby, i, key, sizeof(key), value, sizeof(value))) {
			continue;
		}
		if (strncmp(key, contextPrefix.c_str(), contextPrefix.size()) == 0 && strlen(key) == contextPrefix.size() + 8) {
			XUSER_CONTEXT context;
			context.dwContextId = (DWORD)strtoul(key + contextPrefix.size(), nullptr, 16);
			context.dwValue = (DWORD)strtoul(value, nullptr, 10);
			entry.contexts.push_back(context);
		}
		else if (strncmp(key, propertyPrefix.c_str(), propertyPrefix.size()) == 0 && strlen(key) == propertyPrefix.size() + 8) {
			DWORD propertyId = (DWORD)strtoul(key + propertyPrefix.size(), nullptr, 16);
			xls::StoredProperty stored;
			if (!stored.FromString((uint8_t)XPROPERTYTYPEFROMID(propertyId), value)) {
				continue;
			}
			XUSER_PROPERTY property = {};
			property.dwPropertyId = propertyId;
			entry.properties.push_back(property);
			entry.values.push_back(stored);
		}
	}
	entry.result.cContexts = (DWORD)entry.contexts.size();
	entry.result.cProperties = (DWORD)entry.properties.size();
	return true;
}

// Packs search entries into the title's buffer. Returns the number written.
DWORD PackSearchResults(std::vector<SearchEntry>& entries, XSESSION_SEARCHRESULT_HEADER* header, DWORD bufferSize)
{
	xls::BufferPacker packer(header, bufferSize);
	packer.Front(sizeof(XSESSION_SEARCHRESULT_HEADER));
	header->dwSearchResults = 0;
	header->pResults = (XSESSION_SEARCHRESULT*)packer.FrontPointer();
	for (SearchEntry& entry : entries) {
		uint8_t* front = packer.FrontPointer();
		uint8_t* back = packer.BackPointer();
		XSESSION_SEARCHRESULT* out = (XSESSION_SEARCHRESULT*)packer.Front(sizeof(XSESSION_SEARCHRESULT));
		if (!out) {
			break;
		}
		*out = entry.result;
		out->pContexts = nullptr;
		out->pProperties = nullptr;
		bool fits = true;
		if (!entry.contexts.empty()) {
			XUSER_CONTEXT* contexts = (XUSER_CONTEXT*)packer.Back(entry.contexts.size() * sizeof(XUSER_CONTEXT));
			fits = contexts != nullptr;
			if (fits) {
				memcpy(contexts, entry.contexts.data(), entry.contexts.size() * sizeof(XUSER_CONTEXT));
				out->pContexts = contexts;
			}
		}
		if (fits && !entry.properties.empty()) {
			XUSER_PROPERTY* properties = (XUSER_PROPERTY*)packer.Back(entry.properties.size() * sizeof(XUSER_PROPERTY));
			fits = properties != nullptr;
			for (size_t i = 0; fits && i < entry.properties.size(); i++) {
				properties[i] = entry.properties[i];
				entry.values[i].Fill(properties[i].value);
				const xls::StoredProperty& stored = entry.values[i];
				if (stored.type == XUSER_DATA_TYPE_UNICODE || stored.type == XUSER_DATA_TYPE_BINARY) {
					uint8_t* data = (uint8_t*)packer.Back(stored.raw.size());
					if (!data) {
						fits = false;
						break;
					}
					memcpy(data, stored.raw.data(), stored.raw.size());
					if (stored.type == XUSER_DATA_TYPE_UNICODE) {
						properties[i].value.string.pwszData = (LPWSTR)data;
					}
					else {
						properties[i].value.binary.pbData = data;
					}
				}
			}
			if (fits) {
				out->pProperties = properties;
			}
		}
		if (!fits) {
			packer.Restore(front, back);
			break;
		}
		header->dwSearchResults++;
	}
	return header->dwSearchResults;
}

DWORD SearchBufferEstimate(DWORD results)
{
	return sizeof(XSESSION_SEARCHRESULT_HEADER) + results * (sizeof(XSESSION_SEARCHRESULT) + 32 * sizeof(XUSER_CONTEXT) + 32 * (sizeof(XUSER_PROPERTY) + 64));
}

ELobbyComparison SteamComparison(xls::spa::Comparison c)
{
	switch (c) {
		case xls::spa::Comparison::NotEqual: return k_ELobbyComparisonNotEqual;
		case xls::spa::Comparison::Less: return k_ELobbyComparisonLessThan;
		case xls::spa::Comparison::LessEqual: return k_ELobbyComparisonEqualToOrLessThan;
		case xls::spa::Comparison::Greater: return k_ELobbyComparisonGreaterThan;
		case xls::spa::Comparison::GreaterEqual: return k_ELobbyComparisonEqualToOrGreaterThan;
		default: return k_ELobbyComparisonEqual;
	}
}

const char* ComparisonText(ELobbyComparison c)
{
	switch (c) {
		case k_ELobbyComparisonNotEqual: return "!=";
		case k_ELobbyComparisonLessThan: return "<";
		case k_ELobbyComparisonEqualToOrLessThan: return "<=";
		case k_ELobbyComparisonGreaterThan: return ">";
		case k_ELobbyComparisonEqualToOrGreaterThan: return ">=";
		default: return "==";
	}
}

// Steam compares lobby data numerically only through int, wider values fall back to text, where
// only equality is meaningful.
void AddFilter(const std::string& key, const XUSER_DATA& value, ELobbyComparison compare)
{
	ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
	int64_t number = 0;
	bool numeric = true;
	switch (value.type) {
		case XUSER_DATA_TYPE_CONTEXT:
		case XUSER_DATA_TYPE_INT32: number = value.nData; break;
		case XUSER_DATA_TYPE_INT64: number = value.i64Data; break;
		case XUSER_DATA_TYPE_FLOAT: number = (int64_t)value.fData; break;
		case XUSER_DATA_TYPE_DOUBLE: number = (int64_t)value.dblData; break;
		default: numeric = false; break;
	}
	if (numeric && number >= INT32_MIN && number <= INT32_MAX) {
		matchmaking->AddRequestLobbyListNumericalFilter(key.c_str(), (int)number, compare);
		XLS_LOG_DEBUG("session: filter %s %s %lld.", key.c_str(), ComparisonText(compare), number);
		return;
	}
	xls::StoredProperty stored;
	switch (value.type) {
		case XUSER_DATA_TYPE_INT64: stored.Set(value.type, &value.i64Data, 8); break;
		case XUSER_DATA_TYPE_UNICODE: stored.Set(value.type, value.string.pwszData, value.string.cbData); break;
		case XUSER_DATA_TYPE_BINARY: stored.Set(value.type, value.binary.pbData, value.binary.cbData); break;
		default: return;
	}
	matchmaking->AddRequestLobbyListStringFilter(key.c_str(), stored.ToString().c_str(), compare);
	XLS_LOG_DEBUG("session: filter %s %s \"%s\" (text).", key.c_str(), ComparisonText(compare), stored.ToString().c_str());
}

void AddSearchFilters(DWORD procedureIndex, WORD propertyCount, WORD contextCount, const XUSER_PROPERTY* properties, const XUSER_CONTEXT* contexts, DWORD resultCount)
{
	ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
	matchmaking->AddRequestLobbyListStringFilter(Key("kind").c_str(), "session", k_ELobbyComparisonEqual);
	matchmaking->AddRequestLobbyListStringFilter(Key("title").c_str(), xls::FormatA("%u", xls::TitleId()).c_str(), k_ELobbyComparisonEqual);
	matchmaking->AddRequestLobbyListDistanceFilter((ELobbyDistanceFilter)xls::Cfg().lobbyDistanceFilter);
	matchmaking->AddRequestLobbyListResultCountFilter((int)std::max<DWORD>(resultCount, 50));

	// The value the title passed for a query parameter id, as a context or a property.
	auto parameter = [&](DWORD id, XUSER_DATA* out) {
		if (XPROPERTYTYPEFROMID(id) == XUSER_DATA_TYPE_CONTEXT) {
			for (WORD i = 0; i < contextCount; i++) {
				if (contexts[i].dwContextId == id) {
					memset(out, 0, sizeof(*out));
					out->type = XUSER_DATA_TYPE_CONTEXT;
					out->nData = (LONG)contexts[i].dwValue;
					return true;
				}
			}
			return false;
		}
		for (WORD i = 0; i < propertyCount; i++) {
			if (properties[i].dwPropertyId == id) {
				*out = properties[i].value;
				return true;
			}
		}
		return false;
	};

	// The XLAST query says which attribute each filter tests and where its operand comes from,
	// without it every context and property the title passes is an equality filter.
	const xls::spa::Query* query = xls::spa::FindQuery(procedureIndex);
	if (!query) {
		for (WORD i = 0; i < contextCount; i++) {
			XUSER_DATA value = {};
			value.type = XUSER_DATA_TYPE_CONTEXT;
			value.nData = (LONG)contexts[i].dwValue;
			AddFilter(ContextKey(contexts[i].dwContextId), value, k_ELobbyComparisonEqual);
		}
		for (WORD i = 0; i < propertyCount; i++) {
			AddFilter(PropertyKey(properties[i].dwPropertyId), properties[i].value, k_ELobbyComparisonEqual);
		}
		return;
	}
	for (const xls::spa::QueryFilter& filter : query->filters) {
		bool isContext = XPROPERTYTYPEFROMID(filter.attributeId) == XUSER_DATA_TYPE_CONTEXT;
		std::string key = isContext ? ContextKey(filter.attributeId) : PropertyKey(filter.attributeId);
		ELobbyComparison compare = SteamComparison(filter.comparison);
		XUSER_DATA value = {};
		value.type = XUSER_DATA_TYPE_INT32;
		switch (filter.operandType) {
			case xls::spa::Operand::Constant: {
				double constant = 0;
				if (!xls::spa::Constant(filter.operand, &constant)) {
					continue;
				}
				value.nData = (LONG)constant;
				break;
			}
			case xls::spa::Operand::ContextValue:
				value.nData = (LONG)filter.operand;
				break;
			default:
				if (!parameter(filter.operand, &value) && !parameter(filter.attributeId, &value)) {
					XLS_LOG_DEBUG("session: query %u filter on 0x%08x skipped, the title passed no parameter 0x%08x.", procedureIndex, filter.attributeId, filter.operand);
					continue;
				}
				break;
		}
		AddFilter(key, value, compare);
	}
}

// Builds the local member view: everyone in the lobby plus anyone the title declared.
std::vector<XSESSION_MEMBER> MemberList(const Session& session)
{
	std::vector<XSESSION_MEMBER> members;
	auto add = [&](XUID xuid, DWORD userIndex, DWORD flags) {
		for (XSESSION_MEMBER& existing : members) {
			if (existing.xuidOnline == xuid) {
				existing.dwFlags |= flags;
				if (userIndex != XUSER_INDEX_NONE) {
					existing.dwUserIndex = userIndex;
				}
				return;
			}
		}
		XSESSION_MEMBER member;
		member.xuidOnline = xuid;
		member.dwUserIndex = userIndex;
		member.dwFlags = flags;
		members.push_back(member);
	};
	if (session.lobby.IsValid() && xls::SteamReady()) {
		int count = xls::SteamMatchmaking()->GetNumLobbyMembers(session.lobby);
		for (int i = 0; i < count; i++) {
			CSteamID steamId = xls::SteamMatchmaking()->GetLobbyMemberByIndex(session.lobby, i);
			XUID xuid = xls::XuidFromSteamId(steamId);
			add(xuid, xls::UserIndexForXuid(xuid), 0);
		}
	}
	for (const Member& declared : session.members) {
		add(declared.xuid, declared.userIndex, declared.flags);
	}
	return members;
}

void StoreInvite(CSteamID lobby, CSteamID inviter)
{
	XSESSION_INFO info;
	if (!ReadSessionInfo(lobby, &info)) {
		XLS_LOG_WARN("invite: lobby %llu carries no session data.", lobby.ConvertToUint64());
		return;
	}
	auto invite = std::make_shared<XINVITE_INFO>();
	memset(invite.get(), 0, sizeof(*invite));
	invite->xuidInvitee = xls::UserXuid(0);
	invite->xuidInviter = xls::XuidFromSteamId(inviter);
	invite->dwTitleID = xls::TitleId();
	invite->hostInfo = info;
	invite->fFromGameInvite = TRUE;
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_acceptedInvite = invite;
	}
	XLS_LOG_INFO("invite: accepted invite to lobby %llu from %llu.", lobby.ConvertToUint64(), inviter.ConvertToUint64());
	xls::NotifyPost(XN_LIVE_INVITE_ACCEPTED, 0);
}

void BeginInvite(CSteamID lobby, CSteamID inviter)
{
	if (!lobby.IsValid() || !xls::SteamReady()) {
		return;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_pendingInviteLobby = lobby;
	g_pendingInviter = inviter;
	g_lobbyDataArrived.erase(lobby.ConvertToUint64());
	xls::SteamMatchmaking()->RequestLobbyData(lobby);
}

}

namespace xls {

void SessionsInit()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	g_sessions.clear();
	g_acceptedInvite.reset();
}

void SessionsShutdown()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (auto& entry : g_sessions) {
		if (entry.second->lobby.IsValid() && SteamReady() && !entry.second->deleted) {
			SteamMatchmaking()->LeaveLobby(entry.second->lobby);
		}
		CloseHandle(entry.first);
	}
	g_sessions.clear();
	PublishRichPresenceConnect(nullptr);
}

bool SessionHandleValid(HANDLE session)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return Find(session) != nullptr;
}

bool SessionCloseHandle(HANDLE session)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_sessions.find(session);
	if (it == g_sessions.end()) {
		return false;
	}
	if (!it->second->deleted && it->second->lobby.IsValid() && SteamReady()) {
		SteamMatchmaking()->LeaveLobby(it->second->lobby);
	}
	g_sessions.erase(it);
	CloseHandle(session);
	return true;
}

CSteamID SessionPresenceLobby()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	CSteamID fallback;
	for (auto& entry : g_sessions) {
		if (entry.second->deleted || !entry.second->lobby.IsValid()) {
			continue;
		}
		if (entry.second->flags & XSESSION_CREATE_USES_PRESENCE) {
			return entry.second->lobby;
		}
		fallback = entry.second->lobby;
	}
	return fallback;
}

uint64_t SessionLobbyId(HANDLE session)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> found = Find(session);
	return found && found->lobby.IsValid() ? found->lobby.ConvertToUint64() : 0;
}

bool SessionInfoFromLobby(CSteamID lobby, XSESSION_INFO* info)
{
	if (!SteamReady() || !lobby.IsLobby()) {
		return false;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return ReadSessionInfo(lobby, info);
}

void SessionCheckLaunchInvite()
{
	if (!SteamReady() || !SteamApps()) {
		return;
	}
	char commandLine[1024] = {};
	int length = SteamApps()->GetLaunchCommandLine(commandLine, sizeof(commandLine));
	const char* source = length > 0 ? commandLine : GetCommandLineA();
	const char* marker = strstr(source, "+connect_lobby");
	if (!marker) {
		return;
	}
	uint64_t lobbyId = _strtoui64(marker + strlen("+connect_lobby"), nullptr, 10);
	CSteamID lobby(lobbyId);
	if (lobby.IsLobby()) {
		XLS_LOG_INFO("invite: launched with +connect_lobby %llu.", lobbyId);
		BeginInvite(lobby, k_steamIDNil);
	}
}

namespace events {

void OnLobbyJoinRequested(CSteamID lobby, CSteamID inviter)
{
	BeginInvite(lobby, inviter);
}

void OnRichPresenceJoinRequested(CSteamID inviter, const char* connect)
{
	const char* marker = connect ? strstr(connect, "+connect_lobby") : nullptr;
	if (!marker) {
		return;
	}
	CSteamID lobby(_strtoui64(marker + strlen("+connect_lobby"), nullptr, 10));
	if (lobby.IsLobby()) {
		BeginInvite(lobby, inviter);
	}
}

void OnNewLaunchParameters()
{
	SessionCheckLaunchInvite();
}

// Steam gives lobby to whoever it wants, but some GFWL games pick the new host themselves.
// So we settle that here by passing the lobby to the member with the newest host claim
void SettleLobbyOwnership(Session& session)
{
	if (!session.lobby.IsValid() || session.deleted || !SteamReady()) {
		return;
	}
	ISteamMatchmaking* matchmaking = SteamMatchmaking();
	if (matchmaking->GetLobbyOwner(session.lobby) != SteamLocalId()) {
		return;
	}
	if (session.host && session.pendingPublish) {
		session.pendingPublish = false;
		PublishSession(session);
		XLS_LOG_INFO("session: lobby %llu is ours, migrated host data published.", session.lobby.ConvertToUint64());
		return;
	}
	CSteamID claimant = NewestHostClaim(session.lobby, LobbyHostSeq(session.lobby), nullptr, nullptr);
	if (claimant.IsValid() && claimant != SteamLocalId()) {
		matchmaking->SetLobbyOwner(session.lobby, claimant);
		XLS_LOG_INFO("session: handed lobby %llu to the migrated host %llu.", session.lobby.ConvertToUint64(), claimant.ConvertToUint64());
	}
}

void OnLobbyChatUpdate(const LobbyChatUpdate_t& update)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = FindByLobby(CSteamID(update.m_ulSteamIDLobby));
	if (!session) {
		return;
	}
	CSteamID changed(update.m_ulSteamIDUserChanged);
	if (update.m_rgfChatMemberStateChange & k_EChatMemberStateChangeEntered) {
		SteamFriends()->SetPlayedWith(changed);
		NetSecureAddrFor(changed);
		XLS_LOG_DEBUG("session: %llu joined lobby %llu.", changed.ConvertToUint64(), update.m_ulSteamIDLobby);
	}
	else if (BChatMemberStateChangeRemoved(update.m_rgfChatMemberStateChange)) {
		XLS_LOG_DEBUG("session: %llu left lobby %llu.", changed.ConvertToUint64(), update.m_ulSteamIDLobby);
		if (!session->host && SteamMatchmaking()->GetLobbyOwner(session->lobby) == SteamLocalId()) {
			XLS_LOG_INFO("session: this machine now owns lobby %llu.", update.m_ulSteamIDLobby);
		}
		SettleLobbyOwnership(*session);
	}
}

void OnLobbyDataUpdate(const LobbyDataUpdate_t& update)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	CSteamID lobby(update.m_ulSteamIDLobby);
	if (update.m_ulSteamIDLobby == update.m_ulSteamIDMember) {
		g_lobbyDataArrived.insert(update.m_ulSteamIDLobby);
		if (g_pendingInviteLobby == lobby) {
			CSteamID inviter = g_pendingInviter;
			g_pendingInviteLobby = k_steamIDNil;
			g_pendingInviter = k_steamIDNil;
			if (update.m_bSuccess) {
				StoreInvite(lobby, inviter);
			}
		}
		std::shared_ptr<Session> session = FindByLobby(lobby);
		if (session && !session->host) {
			// The host may have migrated or changed the slots so uptade the local copy
			XSESSION_INFO info;
			if (ReadSessionInfo(lobby, &info)) {
				session->info = info;
			}
			session->maxPublic = (DWORD)strtoul(SteamMatchmaking()->GetLobbyData(lobby, Key("pub").c_str()), nullptr, 10);
			session->maxPrivate = (DWORD)strtoul(SteamMatchmaking()->GetLobbyData(lobby, Key("priv").c_str()), nullptr, 10);
		}
		if (session) {
			SettleLobbyOwnership(*session);
		}
	}
	else {
		// Member data changed: a host claim may have arrived.
		std::shared_ptr<Session> session = FindByLobby(lobby);
		if (session) {
			if (!session->host) {
				XSESSION_INFO info;
				if (ReadSessionInfo(lobby, &info)) {
					session->info = info;
				}
			}
			SettleLobbyOwnership(*session);
		}
	}
}

void OnLobbyKicked(const LobbyKicked_t& kicked)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = FindByLobby(CSteamID(kicked.m_ulSteamIDLobby));
	if (session) {
		XLS_LOG_WARN("session: removed from lobby %llu.", kicked.m_ulSteamIDLobby);
		session->deleted = true;
		session->state = XSESSION_STATE_DELETED;
	}
}

}
}

// #5300
DWORD WINAPI XSessionCreate(DWORD dwFlags, DWORD dwUserIndex, DWORD dwMaxPublicSlots, DWORD dwMaxPrivateSlots, uint64_t* pqwSessionNonce, XSESSION_INFO* pSessionInfo, XOVERLAPPED* pOverlapped, HANDLE* phSession)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!pqwSessionNonce || !pSessionInfo || !phSession) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!xls::SteamReady()) {
		return (DWORD)XONLINE_E_SESSION_NOT_LOGGED_ON;
	}

	auto session = std::make_shared<Session>();
	session->handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	session->flags = dwFlags;
	session->userIndex = dwUserIndex;
	session->host = (dwFlags & XSESSION_CREATE_HOST) != 0;
	session->maxPublic = dwMaxPublicSlots;
	session->maxPrivate = dwMaxPrivateSlots;
	xls::UserGetContext(dwUserIndex, X_CONTEXT_GAME_TYPE, &session->gameType);
	xls::UserGetContext(dwUserIndex, X_CONTEXT_GAME_MODE, &session->gameMode);
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_sessions[session->handle] = session;
	}
	*phSession = session->handle;

	if (session->host) {
		session->nonce = ((uint64_t)GetTickCount64() << 24) ^ xls::SteamLocalId().ConvertToUint64();
		session->hostSeq = 1;
		xls::NetLocalXnaddr(&session->info.hostAddress);
		xls::NetCreateKey(nullptr, &session->info.keyExchangeKey);
		int maxMembers = (int)std::min<DWORD>(std::max<DWORD>(dwMaxPublicSlots + dwMaxPrivateSlots, 1), 250);
		auto call = std::make_shared<xls::SteamCall<LobbyCreated_t>>();
		call->Start(xls::SteamMatchmaking()->CreateLobby(LobbyTypeFor(dwFlags), maxMembers));
		XLS_LOG_INFO("session: creating lobby (flags 0x%08x, %u public, %u private).", dwFlags, dwMaxPublicSlots, dwMaxPrivateSlots);
		return xls::RunAsync(pOverlapped, [session, call, pqwSessionNonce, pSessionInfo](XOVERLAPPED* overlapped) {
			if (!call->Poll()) {
				return false;
			}
			std::lock_guard<std::recursive_mutex> lock(g_mutex);
			if (call->failed || call->result.m_eResult != k_EResultOK) {
				XLS_LOG_ERROR("session: CreateLobby failed (EResult %d).", (int)call->result.m_eResult);
				session->deleted = true;
				xls::OverlappedComplete(overlapped, (DWORD)XONLINE_E_SESSION_CREATE_KEY_FAILED);
				return true;
			}
			session->lobby = CSteamID(call->result.m_ulSteamIDLobby);
			session->info.sessionID = KidFromLobby(session->lobby);
			PublishSession(*session);
			PublishRichPresenceConnect(session.get());
			*pqwSessionNonce = session->nonce;
			*pSessionInfo = session->info;
			XLS_LOG_INFO("session: hosting lobby %llu.", session->lobby.ConvertToUint64());
			xls::OverlappedComplete(overlapped, ERROR_SUCCESS);
			return true;
		});
	}

	CSteamID lobby = LobbyFromKid(pSessionInfo->sessionID);
	if (!lobby.IsValid()) {
		XLS_LOG_ERROR("session: the session id is not a Steam lobby.");
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_sessions.erase(session->handle);
		CloseHandle(session->handle);
		*phSession = nullptr;
		return (DWORD)XONLINE_E_SESSION_NOT_FOUND;
	}
	session->lobby = lobby;
	session->info = *pSessionInfo;
	auto call = std::make_shared<xls::SteamCall<LobbyEnter_t>>();
	call->Start(xls::SteamMatchmaking()->JoinLobby(lobby));
	XLS_LOG_INFO("session: joining lobby %llu.", lobby.ConvertToUint64());
	return xls::RunAsync(pOverlapped, [session, call, pqwSessionNonce, pSessionInfo](XOVERLAPPED* overlapped) {
		if (!call->Poll()) {
			return false;
		}
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		if (call->failed || call->result.m_EChatRoomEnterResponse != k_EChatRoomEnterResponseSuccess) {
			DWORD result = (DWORD)XONLINE_E_SESSION_NOT_FOUND;
			if (call->result.m_EChatRoomEnterResponse == k_EChatRoomEnterResponseFull) {
				result = (DWORD)XONLINE_E_SESSION_FULL;
			}
			XLS_LOG_ERROR("session: JoinLobby failed (%u).", call->result.m_EChatRoomEnterResponse);
			session->deleted = true;
			xls::OverlappedComplete(overlapped, result);
			return true;
		}
		XSESSION_INFO info;
		if (ReadSessionInfo(session->lobby, &info)) {
			session->info = info;
		}
		ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
		session->flags = (session->flags & XSESSION_CREATE_HOST) | (DWORD)strtoul(matchmaking->GetLobbyData(session->lobby, Key("flags").c_str()), nullptr, 10);
		session->maxPublic = (DWORD)strtoul(matchmaking->GetLobbyData(session->lobby, Key("pub").c_str()), nullptr, 10);
		session->maxPrivate = (DWORD)strtoul(matchmaking->GetLobbyData(session->lobby, Key("priv").c_str()), nullptr, 10);
		session->nonce = _strtoui64(matchmaking->GetLobbyData(session->lobby, Key("nonce").c_str()), nullptr, 10);
		session->gameType = (DWORD)strtoul(matchmaking->GetLobbyData(session->lobby, Key("gt").c_str()), nullptr, 10);
		session->gameMode = (DWORD)strtoul(matchmaking->GetLobbyData(session->lobby, Key("gm").c_str()), nullptr, 10);
		session->state = (XSESSION_STATE)strtoul(matchmaking->GetLobbyData(session->lobby, Key("state").c_str()), nullptr, 10);
		CSteamID host;
		if (xls::NetSteamIdFromXnaddr(session->info.hostAddress, &host)) {
			xls::NetSecureAddrFor(host);
			xls::SteamFriends()->SetPlayedWith(host);
		}
		PublishRichPresenceConnect(session.get());
		*pqwSessionNonce = session->nonce;
		*pSessionInfo = session->info;
		XLS_LOG_INFO("session: joined lobby %llu.", session->lobby.ConvertToUint64());
		xls::OverlappedComplete(overlapped, ERROR_SUCCESS);
		return true;
	});
}

// #5318
DWORD WINAPI XSessionStart(HANDLE hSession, DWORD dwFlags, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	session->state = XSESSION_STATE_INGAME;
	if (session->host && session->lobby.IsValid() && xls::SteamReady()) {
		PublishSession(*session);
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5332
DWORD WINAPI XSessionEnd(HANDLE hSession, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	session->state = XSESSION_STATE_LOBBY;
	if (session->host && session->lobby.IsValid() && xls::SteamReady()) {
		PublishSession(*session);
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5322
DWORD WINAPI XSessionModify(HANDLE hSession, DWORD dwFlags, DWORD dwMaxPublicSlots, DWORD dwMaxPrivateSlots, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	session->flags = (session->flags & XSESSION_CREATE_HOST) | (dwFlags & ~XSESSION_CREATE_HOST);
	session->maxPublic = dwMaxPublicSlots;
	session->maxPrivate = dwMaxPrivateSlots;
	xls::UserGetContext(session->userIndex, X_CONTEXT_GAME_TYPE, &session->gameType);
	xls::UserGetContext(session->userIndex, X_CONTEXT_GAME_MODE, &session->gameMode);
	if (session->host && session->lobby.IsValid() && xls::SteamReady()) {
		xls::SteamMatchmaking()->SetLobbyMemberLimit(session->lobby, (int)std::min<DWORD>(std::max<DWORD>(dwMaxPublicSlots + dwMaxPrivateSlots, 1), 250));
		xls::SteamMatchmaking()->SetLobbyType(session->lobby, LobbyTypeFor(session->flags));
		PublishSession(*session);
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5330
DWORD WINAPI XSessionDelete(HANDLE hSession, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	if (!session->deleted && session->lobby.IsValid() && xls::SteamReady()) {
		xls::SteamMatchmaking()->LeaveLobby(session->lobby);
		XLS_LOG_INFO("session: left lobby %llu.", session->lobby.ConvertToUint64());
	}
	session->deleted = true;
	session->state = XSESSION_STATE_DELETED;
	PublishRichPresenceConnect(nullptr);
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5327
DWORD WINAPI XSessionJoinLocal(HANDLE hSession, DWORD dwUserCount, const DWORD* pdwUserIndexes, const BOOL* pfPrivateSlots, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwUserCount || !pdwUserIndexes) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	for (DWORD i = 0; i < dwUserCount; i++) {
		if (!xls::UserIndexValid(pdwUserIndexes[i])) {
			return ERROR_NO_SUCH_USER;
		}
		Member member;
		member.xuid = xls::UserXuid(pdwUserIndexes[i]);
		member.userIndex = pdwUserIndexes[i];
		member.flags = (pfPrivateSlots && pfPrivateSlots[i]) ? XSESSION_MEMBER_FLAGS_PRIVATE_SLOT : 0;
		session->members.push_back(member);
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5326
DWORD WINAPI XSessionJoinRemote(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids, const BOOL* pfPrivateSlots, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwXuidCount || !pXuids) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	for (DWORD i = 0; i < dwXuidCount; i++) {
		Member member;
		member.xuid = pXuids[i];
		member.userIndex = XUSER_INDEX_NONE;
		member.flags = (pfPrivateSlots && pfPrivateSlots[i]) ? XSESSION_MEMBER_FLAGS_PRIVATE_SLOT : 0;
		session->members.push_back(member);
		CSteamID steamId = xls::SteamIdFromXuid(pXuids[i]);
		if (steamId.IsValid()) {
			xls::NetSecureAddrFor(steamId);
		}
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5325
DWORD WINAPI XSessionLeaveLocal(HANDLE hSession, DWORD dwUserCount, const DWORD* pdwUserIndexes, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwUserCount || !pdwUserIndexes) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	for (DWORD i = 0; i < dwUserCount; i++) {
		XUID xuid = xls::UserXuid(pdwUserIndexes[i]);
		session->members.erase(std::remove_if(session->members.begin(), session->members.end(), [xuid](const Member& m) { return m.xuid == xuid; }), session->members.end());
		if (pdwUserIndexes[i] == 0 && !session->host && !session->deleted && session->lobby.IsValid() && xls::SteamReady()) {
			xls::SteamMatchmaking()->LeaveLobby(session->lobby);
			session->deleted = true;
			PublishRichPresenceConnect(nullptr);
		}
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5336
DWORD WINAPI XSessionLeaveRemote(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwXuidCount || !pXuids) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	for (DWORD i = 0; i < dwXuidCount; i++) {
		XUID xuid = pXuids[i];
		session->members.erase(std::remove_if(session->members.begin(), session->members.end(), [xuid](const Member& m) { return m.xuid == xuid; }), session->members.end());
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5328
DWORD WINAPI XSessionGetDetails(HANDLE hSession, DWORD* pcbResultsBuffer, XSESSION_LOCAL_DETAILS* pSessionDetails, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!pcbResultsBuffer) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	std::vector<XSESSION_MEMBER> members = MemberList(*session);
	DWORD required = sizeof(XSESSION_LOCAL_DETAILS) + (DWORD)members.size() * sizeof(XSESSION_MEMBER);
	if (!pSessionDetails || *pcbResultsBuffer < sizeof(XSESSION_LOCAL_DETAILS)) {
		*pcbResultsBuffer = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	memset(pSessionDetails, 0, sizeof(*pSessionDetails));
	pSessionDetails->dwUserIndexHost = session->host ? session->userIndex : XUSER_INDEX_NONE;
	pSessionDetails->dwGameType = session->gameType;
	pSessionDetails->dwGameMode = session->gameMode;
	pSessionDetails->dwFlags = session->flags;
	pSessionDetails->dwMaxPublicSlots = session->maxPublic;
	pSessionDetails->dwMaxPrivateSlots = session->maxPrivate;
	DWORD privateUsed = 0;
	DWORD publicUsed = 0;
	for (const XSESSION_MEMBER& member : members) {
		if (member.dwFlags & XSESSION_MEMBER_FLAGS_PRIVATE_SLOT) privateUsed++;
		else publicUsed++;
	}
	pSessionDetails->dwAvailablePrivateSlots = privateUsed < session->maxPrivate ? session->maxPrivate - privateUsed : 0;
	pSessionDetails->dwAvailablePublicSlots = publicUsed < session->maxPublic ? session->maxPublic - publicUsed : 0;
	pSessionDetails->dwActualMemberCount = (DWORD)members.size();
	pSessionDetails->eState = session->state;
	pSessionDetails->qwNonce = session->nonce;
	pSessionDetails->sessionInfo = session->info;
	pSessionDetails->xnkidArbitration = session->info.sessionID;
	DWORD room = (*pcbResultsBuffer - sizeof(XSESSION_LOCAL_DETAILS)) / sizeof(XSESSION_MEMBER);
	pSessionDetails->dwReturnedMemberCount = std::min<DWORD>(room, (DWORD)members.size());
	if (pSessionDetails->dwReturnedMemberCount) {
		pSessionDetails->pSessionMembers = (XSESSION_MEMBER*)((uint8_t*)pSessionDetails + sizeof(XSESSION_LOCAL_DETAILS));
		memcpy(pSessionDetails->pSessionMembers, members.data(), pSessionDetails->dwReturnedMemberCount * sizeof(XSESSION_MEMBER));
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5323
DWORD WINAPI XSessionMigrateHost(HANDLE hSession, DWORD dwUserIndex, XSESSION_INFO* pSessionInfo, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!pSessionInfo) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	if (dwUserIndex != XUSER_INDEX_NONE && xls::UserIndexValid(dwUserIndex)) {
		// This machine takes over: the lobby stays, the host address changes.
		session->host = true;
		session->userIndex = dwUserIndex;
		session->flags |= XSESSION_CREATE_HOST;
		session->hostSeq = session->lobby.IsValid() && xls::SteamReady() ? NextHostSeq(session->lobby) : 1;
		xls::NetLocalXnaddr(&session->info.hostAddress);
		if (session->lobby.IsValid() && !session->deleted && xls::SteamReady()) {
			ISteamMatchmaking* matchmaking = xls::SteamMatchmaking();
			std::string claim = xls::HexEncode(&session->info.hostAddress, sizeof(session->info.hostAddress)) + xls::FormatA("|%llu", session->hostSeq);
			matchmaking->SetLobbyMemberData(session->lobby, Key("host").c_str(), claim.c_str());
			if (matchmaking->GetLobbyOwner(session->lobby) == xls::SteamLocalId()) {
				PublishSession(*session);
				XLS_LOG_INFO("session: migrated to host of lobby %llu.", session->lobby.ConvertToUint64());
			}
			else {
				session->pendingPublish = true;
				XLS_LOG_INFO("session: migrated to host of lobby %llu, waiting for the lobby owner to hand it over.", session->lobby.ConvertToUint64());
			}
		}
		*pSessionInfo = session->info;
	}
	else {
		// Another machine took over and the title hands us the info it received from the new host.
		session->host = false;
		session->flags &= ~XSESSION_CREATE_HOST;
		session->info = *pSessionInfo;
		CSteamID host;
		if (xls::NetSteamIdFromXnaddr(session->info.hostAddress, &host)) {
			xls::NetSecureAddrFor(host);
		}
		xls::events::SettleLobbyOwnership(*session);
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5333
DWORD WINAPI XSessionArbitrationRegister(HANDLE hSession, DWORD dwFlags, uint64_t qwSessionNonce, DWORD* pcbResultsBuffer, XSESSION_REGISTRATION_RESULTS* pRegistrationResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!pcbResultsBuffer) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	std::shared_ptr<Session> session = Find(hSession);
	if (!session) {
		return ERROR_INVALID_HANDLE;
	}
	std::vector<XSESSION_MEMBER> members = MemberList(*session);
	DWORD required = sizeof(XSESSION_REGISTRATION_RESULTS) + (DWORD)members.size() * (sizeof(XSESSION_REGISTRANT) + sizeof(XUID));
	if (!pRegistrationResults || *pcbResultsBuffer < required) {
		*pcbResultsBuffer = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	session->state = XSESSION_STATE_REGISTRATION;
	pRegistrationResults->wNumRegistrants = (DWORD)members.size();
	pRegistrationResults->rgRegistrants = (XSESSION_REGISTRANT*)((uint8_t*)pRegistrationResults + sizeof(XSESSION_REGISTRATION_RESULTS));
	XUID* xuids = (XUID*)(pRegistrationResults->rgRegistrants + members.size());
	for (size_t i = 0; i < members.size(); i++) {
		XSESSION_REGISTRANT& registrant = pRegistrationResults->rgRegistrants[i];
		registrant.qwMachineID = xls::MachineIdFromSteamId(xls::SteamIdFromXuid(members[i].xuidOnline));
		registrant.bTrustworthiness = 1;
		registrant.bNumUsers = 1;
		registrant.rgUsers = &xuids[i];
		xuids[i] = members[i].xuidOnline;
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5321
DWORD WINAPI XSessionSearch(DWORD dwProcedureIndex, DWORD dwUserIndex, DWORD dwNumResults, WORD wNumProperties, WORD wNumContexts, XUSER_PROPERTY* pSearchProperties, XUSER_CONTEXT* pSearchContexts, DWORD* pcbResultsBuffer, XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped)
{
	return XSessionSearchEx(dwProcedureIndex, dwUserIndex, dwNumResults, 1, wNumProperties, wNumContexts, pSearchProperties, pSearchContexts, pcbResultsBuffer, pSearchResults, pOverlapped);
}

// #5319
DWORD WINAPI XSessionSearchEx(DWORD dwProcedureIndex, DWORD dwUserIndex, DWORD dwNumResults, DWORD dwNumUsers, WORD wNumProperties, WORD wNumContexts, XUSER_PROPERTY* pSearchProperties, XUSER_CONTEXT* pSearchContexts, DWORD* pcbResultsBuffer, XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!dwNumResults || !pcbResultsBuffer || (wNumProperties && !pSearchProperties) || (wNumContexts && !pSearchContexts)) {
		return ERROR_INVALID_PARAMETER;
	}
	DWORD estimate = SearchBufferEstimate(dwNumResults);
	if (!pSearchResults || *pcbResultsBuffer < sizeof(XSESSION_SEARCHRESULT_HEADER) + sizeof(XSESSION_SEARCHRESULT)) {
		*pcbResultsBuffer = estimate;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	if (!xls::SteamReady()) {
		pSearchResults->dwSearchResults = 0;
		pSearchResults->pResults = nullptr;
		return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
	}
	AddSearchFilters(dwProcedureIndex, wNumProperties, wNumContexts, pSearchProperties, pSearchContexts, dwNumResults);
	auto call = std::make_shared<xls::SteamCall<LobbyMatchList_t>>();
	call->Start(xls::SteamMatchmaking()->RequestLobbyList());
	DWORD bufferSize = *pcbResultsBuffer;
	XLS_LOG_INFO("session: searching (procedure %u, %u contexts, %u properties).", dwProcedureIndex, wNumContexts, wNumProperties);
	return xls::RunAsync(pOverlapped, [call, dwNumResults, pSearchResults, bufferSize](XOVERLAPPED* overlapped) {
		if (!call->Poll()) {
			return false;
		}
		std::vector<SearchEntry> entries;
		if (!call->failed) {
			std::lock_guard<std::recursive_mutex> lock(g_mutex);
			for (uint32 i = 0; i < call->result.m_nLobbiesMatching && entries.size() < dwNumResults; i++) {
				CSteamID lobby = xls::SteamMatchmaking()->GetLobbyByIndex((int)i);
				if (FindByLobby(lobby)) {
					continue;
				}
				SearchEntry entry;
				if (ReadSearchEntry(lobby, entry)) {
					entries.push_back(std::move(entry));
				}
			}
		}
		DWORD written = PackSearchResults(entries, pSearchResults, bufferSize);
		XLS_LOG_INFO("session: search found %zu session(s), returned %u.", entries.size(), written);
		xls::OverlappedComplete(overlapped, ERROR_SUCCESS, written);
		return true;
	});
}

// #5320
DWORD WINAPI XSessionSearchByID(XNKID sessionID, DWORD dwUserIndex, DWORD* pcbResultsBuffer, XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pcbResultsBuffer) {
		return ERROR_INVALID_PARAMETER;
	}
	DWORD estimate = SearchBufferEstimate(1);
	if (!pSearchResults || *pcbResultsBuffer < sizeof(XSESSION_SEARCHRESULT_HEADER) + sizeof(XSESSION_SEARCHRESULT)) {
		*pcbResultsBuffer = estimate;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	CSteamID lobby = LobbyFromKid(sessionID);
	if (!lobby.IsValid() || !xls::SteamReady()) {
		return (DWORD)XONLINE_E_SESSION_NOT_FOUND;
	}
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_lobbyDataArrived.erase(lobby.ConvertToUint64());
	}
	if (!xls::SteamMatchmaking()->RequestLobbyData(lobby)) {
		return (DWORD)XONLINE_E_SESSION_NOT_FOUND;
	}
	DWORD bufferSize = *pcbResultsBuffer;
	return xls::RunAsync(pOverlapped, [lobby, pSearchResults, bufferSize](XOVERLAPPED* overlapped) {
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		if (!g_lobbyDataArrived.count(lobby.ConvertToUint64())) {
			return false;
		}
		std::vector<SearchEntry> entries;
		SearchEntry entry;
		if (ReadSearchEntry(lobby, entry)) {
			entries.push_back(std::move(entry));
		}
		DWORD written = PackSearchResults(entries, pSearchResults, bufferSize);
		xls::OverlappedComplete(overlapped, written ? ERROR_SUCCESS : (DWORD)XONLINE_E_SESSION_NOT_FOUND, written);
		return true;
	});
}

// #5316
DWORD WINAPI XInviteSend(DWORD dwUserIndex, DWORD cInvitees, const XUID* pXuidInvitees, LPCWSTR pszText, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cInvitees || !pXuidInvitees) {
		return ERROR_INVALID_PARAMETER;
	}
	CSteamID lobby = xls::SessionPresenceLobby();
	if (!lobby.IsValid() || !xls::SteamReady()) {
		return xls::OverlappedReturn(pOverlapped, (DWORD)XONLINE_E_SESSION_NOT_FOUND);
	}
	DWORD sent = 0;
	for (DWORD i = 0; i < cInvitees; i++) {
		CSteamID steamId = xls::SteamIdFromXuid(pXuidInvitees[i]);
		if (steamId.IsValid() && xls::SteamMatchmaking()->InviteUserToLobby(lobby, steamId)) {
			sent++;
		}
	}
	XLS_LOG_INFO("invite: sent %u of %u invites to lobby %llu.", sent, cInvitees, lobby.ConvertToUint64());
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS, sent);
}

// #5315
DWORD WINAPI XInviteGetAcceptedInfo(DWORD dwUserIndex, XINVITE_INFO* pInfo)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pInfo) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	if (!g_acceptedInvite) {
		return ERROR_FUNCTION_FAILED;
	}
	*pInfo = *g_acceptedInvite;
	g_acceptedInvite.reset();
	return ERROR_SUCCESS;
}
