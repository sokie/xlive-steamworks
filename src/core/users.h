// Local users, their contexts and properties, and gamertags of remote Steam users.
#pragma once

#include "xlive/xdefs.h"
#include "core/steam.h"

#include <map>
#include <string>
#include <vector>

namespace xls {

// A context/property value the title set, stored by value so the XUSER_DATA a title reads back
// points at memory that lives as long as the setting.
struct StoredProperty {
	uint8_t type = XUSER_DATA_TYPE_NULL;
	std::vector<uint8_t> raw;

	void Set(uint8_t dataType, const void* data, size_t size);
	// Points the union at raw, the result is valid while this object is unchanged.
	void Fill(XUSER_DATA& out) const;
	DWORD DataSize() const { return (DWORD)raw.size(); }
	// Renders the value as text for lobby data and presence strings.
	std::string ToString() const;
	// Parses text produced by ToString, given the type the property id carries.
	bool FromString(uint8_t dataType, const std::string& text);
};

void UsersInit();
void UsersShutdown();

bool UserIndexValid(DWORD userIndex);
XUSER_SIGNIN_STATE UserSigninState(DWORD userIndex);
bool UserSignedIn(DWORD userIndex);
bool UserOnline(DWORD userIndex);
XUID UserXuid(DWORD userIndex);
const char* UserName(DWORD userIndex);
DWORD UserIndexForXuid(XUID xuid);

// Gamertag for any XUID: a local user, a Steam friend or a peer met in a session. Unknown Steam
// users are looked up through Steam and get their account id as a name until it arrives.
std::string GamertagForXuid(XUID xuid);
std::string GamertagForSteamId(CSteamID steamId);

void UserSetContext(DWORD userIndex, DWORD contextId, DWORD value);
bool UserGetContext(DWORD userIndex, DWORD contextId, DWORD* value);
std::map<DWORD, DWORD> UserContexts(DWORD userIndex);

void UserSetProperty(DWORD userIndex, DWORD propertyId, const void* data, DWORD size);
bool UserGetProperty(DWORD userIndex, DWORD propertyId, StoredProperty* out);
std::map<DWORD, StoredProperty> UserProperties(DWORD userIndex);

// The rich presence line built from the SPA presence format and the user's contexts.
std::wstring UserPresenceText(DWORD userIndex);

}
