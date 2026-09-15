#include "core/users.h"

#include "core/config.h"
#include "core/log.h"
#include "core/spa.h"
#include "core/utils.h"

#include <mutex>

namespace xls {

namespace {

struct LocalUser {
	XUSER_SIGNIN_STATE state = eXUserSigninState_NotSignedIn;
	XUID xuid = INVALID_XUID;
	char name[XUSER_NAME_SIZE] = {};
	std::map<DWORD, DWORD> contexts;
	std::map<DWORD, StoredProperty> properties;
	std::wstring presenceText;
	bool presenceDirty = true;
};

std::recursive_mutex g_mutex;
LocalUser g_users[XUSER_MAX_COUNT];
std::map<XUID, std::string> g_gamertags;

void PushRichPresence(DWORD userIndex)
{
	if (userIndex != 0 || !SteamReady() || !SteamFriends()) {
		return;
	}
	std::wstring text = UserPresenceText(userIndex);
	std::string utf8 = WideToUtf8(text.c_str());
	SteamFriends()->SetRichPresence(Cfg().richPresenceKey.c_str(), utf8.c_str());
	if (Cfg().richPresenceSteamDisplay) {
		// A localisation token the studio defines in Steamworks as "%status%" (or whatever
		// presence.key is) shows the same line in the friends list.
		std::string token = "#" + Cfg().richPresenceKey;
		SteamFriends()->SetRichPresence("steam_display", token.c_str());
	}
	XLS_LOG_DEBUG("presence: \"%s\".", utf8.c_str());
}

std::wstring ResolvePresence(const LocalUser& user)
{
	if (!spa::Loaded() || !spa::HasPresence()) {
		return std::wstring();
	}

	uint32_t presenceValue = 0;
	auto presence = user.contexts.find(X_CONTEXT_PRESENCE);
	if (presence != user.contexts.end()) {
		presenceValue = presence->second;
	}
	else if (!spa::ContextDefault(X_CONTEXT_PRESENCE, &presenceValue)) {
		return std::wstring();
	}

	uint16_t formatId = 0;
	if (!spa::PresenceStringId(presenceValue, &formatId)) {
		return std::wstring();
	}
	const wchar_t* format = spa::String(formatId);
	if (!format || !*format) {
		return std::wstring();
	}

	// One element per line of the format. An element that names a context or property the title
	// never set is dropped, so a stale default never shows.
	std::wstring resolved;
	std::wstring element;
	bool complete = true;
	for (const wchar_t* cursor = format;; ) {
		if (!*cursor || *cursor == L'\n') {
			while (!element.empty() && (element.back() == L' ' || element.back() == L'\r')) {
				element.pop_back();
			}
			if (complete && !element.empty()) {
				if (!resolved.empty()) {
					resolved.append(L" - ");
				}
				resolved.append(element);
			}
			if (!*cursor) {
				break;
			}
			element.clear();
			complete = true;
			cursor++;
			continue;
		}
		if (*cursor == L'{' && (cursor[1] == L'c' || cursor[1] == L'p')) {
			const wchar_t* idBegin = cursor + 2;
			wchar_t* idEnd = nullptr;
			unsigned long id = wcstoul(idBegin, &idEnd, 0);
			if (idEnd && idEnd != idBegin && *idEnd == L'}') {
				bool substituted = false;
				if (cursor[1] == L'c') {
					auto context = user.contexts.find((DWORD)id);
					uint16_t valueStringId = 0;
					if (context != user.contexts.end() && spa::ContextValueStringId((uint32_t)id, context->second, &valueStringId)) {
						element.append(spa::String(valueStringId));
						substituted = true;
					}
					else if (context != user.contexts.end()) {
						element.append(std::to_wstring(context->second));
						substituted = true;
					}
				}
				else {
					auto property = user.properties.find((DWORD)id);
					if (property != user.properties.end()) {
						element.append(Utf8ToWide(property->second.ToString().c_str()));
						substituted = true;
					}
				}
				if (!substituted) {
					complete = false;
				}
				cursor = idEnd + 1;
				continue;
			}
		}
		element.push_back(*cursor);
		cursor++;
	}
	return resolved;
}

}

void StoredProperty::Set(uint8_t dataType, const void* data, size_t size)
{
	type = dataType;
	raw.assign((const uint8_t*)data, (const uint8_t*)data + size);
	if (dataType == XUSER_DATA_TYPE_UNICODE) {
		// Always terminated, whatever the title passed.
		if (raw.size() < sizeof(wchar_t) || *(const wchar_t*)&raw[raw.size() - sizeof(wchar_t)] != 0) {
			raw.push_back(0);
			raw.push_back(0);
		}
	}
}

void StoredProperty::Fill(XUSER_DATA& out) const
{
	memset(&out, 0, sizeof(out));
	out.type = type;
	switch (type) {
		case XUSER_DATA_TYPE_INT32:
		case XUSER_DATA_TYPE_CONTEXT:
			if (raw.size() >= 4) memcpy(&out.nData, raw.data(), 4);
			break;
		case XUSER_DATA_TYPE_INT64:
			if (raw.size() >= 8) memcpy(&out.i64Data, raw.data(), 8);
			break;
		case XUSER_DATA_TYPE_DOUBLE:
			if (raw.size() >= 8) memcpy(&out.dblData, raw.data(), 8);
			break;
		case XUSER_DATA_TYPE_FLOAT:
			if (raw.size() >= 4) memcpy(&out.fData, raw.data(), 4);
			break;
		case XUSER_DATA_TYPE_DATETIME:
			if (raw.size() >= 8) memcpy(&out.ftData, raw.data(), 8);
			break;
		case XUSER_DATA_TYPE_UNICODE:
			out.string.cbData = (DWORD)raw.size();
			out.string.pwszData = (LPWSTR)raw.data();
			break;
		case XUSER_DATA_TYPE_BINARY:
			out.binary.cbData = (DWORD)raw.size();
			out.binary.pbData = (PBYTE)raw.data();
			break;
		default:
			break;
	}
}

std::string StoredProperty::ToString() const
{
	XUSER_DATA data;
	Fill(data);
	switch (type) {
		case XUSER_DATA_TYPE_INT32:
		case XUSER_DATA_TYPE_CONTEXT:
			return std::to_string(data.nData);
		case XUSER_DATA_TYPE_INT64:
			return std::to_string(data.i64Data);
		case XUSER_DATA_TYPE_DOUBLE:
			return FormatA("%.17g", data.dblData);
		case XUSER_DATA_TYPE_FLOAT:
			return FormatA("%.9g", data.fData);
		case XUSER_DATA_TYPE_DATETIME:
			return std::to_string(((uint64_t)data.ftData.dwHighDateTime << 32) | data.ftData.dwLowDateTime);
		case XUSER_DATA_TYPE_UNICODE:
			return WideToUtf8(data.string.pwszData);
		case XUSER_DATA_TYPE_BINARY:
			return HexEncode(raw.data(), raw.size());
		default:
			return std::string();
	}
}

bool StoredProperty::FromString(uint8_t dataType, const std::string& text)
{
	switch (dataType) {
		case XUSER_DATA_TYPE_INT32:
		case XUSER_DATA_TYPE_CONTEXT: {
			LONG value = (LONG)strtol(text.c_str(), nullptr, 10);
			Set(dataType, &value, sizeof(value));
			return true;
		}
		case XUSER_DATA_TYPE_INT64: {
			LONGLONG value = _strtoi64(text.c_str(), nullptr, 10);
			Set(dataType, &value, sizeof(value));
			return true;
		}
		case XUSER_DATA_TYPE_DOUBLE: {
			double value = strtod(text.c_str(), nullptr);
			Set(dataType, &value, sizeof(value));
			return true;
		}
		case XUSER_DATA_TYPE_FLOAT: {
			float value = (float)strtod(text.c_str(), nullptr);
			Set(dataType, &value, sizeof(value));
			return true;
		}
		case XUSER_DATA_TYPE_DATETIME: {
			uint64_t ticks = _strtoui64(text.c_str(), nullptr, 10);
			FILETIME value;
			value.dwLowDateTime = (DWORD)ticks;
			value.dwHighDateTime = (DWORD)(ticks >> 32);
			Set(dataType, &value, sizeof(value));
			return true;
		}
		case XUSER_DATA_TYPE_UNICODE: {
			std::wstring value = Utf8ToWide(text.c_str());
			Set(dataType, value.c_str(), (value.size() + 1) * sizeof(wchar_t));
			return true;
		}
		case XUSER_DATA_TYPE_BINARY: {
			std::vector<uint8_t> bytes(text.size() / 2);
			if (!HexDecode(text, bytes.data(), bytes.size())) {
				return false;
			}
			Set(dataType, bytes.data(), bytes.size());
			return true;
		}
		default:
			return false;
	}
}

void UsersInit()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (LocalUser& user : g_users) {
		user = LocalUser();
	}
	if (SteamReady()) {
		LocalUser& user = g_users[0];
		user.state = eXUserSigninState_SignedInToLive;
		user.xuid = XuidFromSteamId(SteamLocalId());
		std::string tag = GamertagFromPersona(SteamFriends()->GetPersonaName(), Cfg().asciiGamertags);
		CopyStringA(user.name, sizeof(user.name), tag.c_str());
		g_gamertags[user.xuid] = tag;
		XLS_LOG_INFO("users: user 0 is %s (xuid 0x%016llx).", user.name, user.xuid);
	}
	if (Cfg().localUsersAllSignedIn) {
		for (DWORD i = 1; i < XUSER_MAX_COUNT; i++) {
			LocalUser& user = g_users[i];
			user.state = eXUserSigninState_SignedInLocally;
			user.xuid = XUID_OFFLINE_FLAG | (0x1000 + i);
			std::string name = FormatA("Player%u", i + 1);
			CopyStringA(user.name, sizeof(user.name), name.c_str());
		}
	}
}

void UsersShutdown()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (LocalUser& user : g_users) {
		user = LocalUser();
	}
	g_gamertags.clear();
}

bool UserIndexValid(DWORD userIndex)
{
	return userIndex < XUSER_MAX_COUNT;
}

XUSER_SIGNIN_STATE UserSigninState(DWORD userIndex)
{
	if (!UserIndexValid(userIndex)) {
		return eXUserSigninState_NotSignedIn;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return g_users[userIndex].state;
}

bool UserSignedIn(DWORD userIndex)
{
	return UserSigninState(userIndex) != eXUserSigninState_NotSignedIn;
}

bool UserOnline(DWORD userIndex)
{
	return UserSigninState(userIndex) == eXUserSigninState_SignedInToLive;
}

XUID UserXuid(DWORD userIndex)
{
	if (!UserIndexValid(userIndex)) {
		return INVALID_XUID;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return g_users[userIndex].xuid;
}

const char* UserName(DWORD userIndex)
{
	if (!UserIndexValid(userIndex)) {
		return "";
	}
	return g_users[userIndex].name;
}

DWORD UserIndexForXuid(XUID xuid)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (DWORD i = 0; i < XUSER_MAX_COUNT; i++) {
		if (g_users[i].state != eXUserSigninState_NotSignedIn && g_users[i].xuid == xuid) {
			return i;
		}
	}
	return XUSER_INDEX_NONE;
}

std::string GamertagForSteamId(CSteamID steamId)
{
	return GamertagForXuid(XuidFromSteamId(steamId));
}

std::string GamertagForXuid(XUID xuid)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	DWORD local = UserIndexForXuid(xuid);
	if (local != XUSER_INDEX_NONE) {
		return g_users[local].name;
	}
	if (!XuidIsSteamUser(xuid)) {
		return std::string();
	}
	CSteamID steamId = SteamIdFromXuid(xuid);
	if (SteamReady() && SteamFriends()) {
		// RequestUserInformation returns false when the name is already cached.
		if (!SteamFriends()->RequestUserInformation(steamId, true)) {
			std::string tag = GamertagFromPersona(SteamFriends()->GetFriendPersonaName(steamId), Cfg().asciiGamertags);
			g_gamertags[xuid] = tag;
			return tag;
		}
	}
	auto cached = g_gamertags.find(xuid);
	if (cached != g_gamertags.end()) {
		return cached->second;
	}
	return FormatA("[%u]", (uint32_t)steamId.GetAccountID());
}

void UserSetContext(DWORD userIndex, DWORD contextId, DWORD value)
{
	if (!UserIndexValid(userIndex)) {
		return;
	}
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_users[userIndex].contexts[contextId] = value;
		g_users[userIndex].presenceDirty = true;
	}
	PushRichPresence(userIndex);
}

bool UserGetContext(DWORD userIndex, DWORD contextId, DWORD* value)
{
	if (!UserIndexValid(userIndex)) {
		return false;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_users[userIndex].contexts.find(contextId);
	if (it == g_users[userIndex].contexts.end()) {
		uint32_t fallback = 0;
		if (spa::Loaded() && spa::ContextDefault(contextId, &fallback)) {
			*value = fallback;
			return true;
		}
		return false;
	}
	*value = it->second;
	return true;
}

std::map<DWORD, DWORD> UserContexts(DWORD userIndex)
{
	if (!UserIndexValid(userIndex)) {
		return std::map<DWORD, DWORD>();
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return g_users[userIndex].contexts;
}

void UserSetProperty(DWORD userIndex, DWORD propertyId, const void* data, DWORD size)
{
	if (!UserIndexValid(userIndex) || !data) {
		return;
	}
	{
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_users[userIndex].properties[propertyId].Set((uint8_t)XPROPERTYTYPEFROMID(propertyId), data, size);
		g_users[userIndex].presenceDirty = true;
	}
	PushRichPresence(userIndex);
}

bool UserGetProperty(DWORD userIndex, DWORD propertyId, StoredProperty* out)
{
	if (!UserIndexValid(userIndex)) {
		return false;
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	auto it = g_users[userIndex].properties.find(propertyId);
	if (it == g_users[userIndex].properties.end()) {
		return false;
	}
	*out = it->second;
	return true;
}

std::map<DWORD, StoredProperty> UserProperties(DWORD userIndex)
{
	if (!UserIndexValid(userIndex)) {
		return std::map<DWORD, StoredProperty>();
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	return g_users[userIndex].properties;
}

std::wstring UserPresenceText(DWORD userIndex)
{
	if (!UserIndexValid(userIndex)) {
		return std::wstring();
	}
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	LocalUser& user = g_users[userIndex];
	if (user.presenceDirty) {
		user.presenceText = ResolvePresence(user);
		user.presenceDirty = false;
	}
	return user.presenceText;
}

}
