// xlive_steamworks.json, read once from the xlive.dll directory, then the exe directory. Every entry has a default.
#pragma once

#include "core/log.h"

#include <stdint.h>
#include <map>
#include <string>
#include <vector>

namespace xls {

struct LeaderboardMapping {
	uint32_t viewId = 0;
	std::string name;                   // Steam leaderboard API name.
	int32_t ratingColumn = -1;          // Column id whose value is the Steam score. -1 = first column.
	std::vector<uint16_t> detailColumns; // Columns packed into the Steam details array, in order.
	bool keepBest = true;               // false = force update.
	bool createIfMissing = true;        // FindOrCreateLeaderboard instead of FindLeaderboard.
	bool ascending = false;             // Sort method used when creating.
	int displayType = 1;                // ELeaderboardDisplayType used when creating.
};

struct DlcMapping {
	uint32_t appId = 0;
	uint8_t contentId[20] = {};
	uint32_t contentType = 2;           // XCONTENTTYPE_MARKETPLACE
	uint64_t offerId = 0;
	std::wstring displayName;
	std::wstring path;                  // Absolute install path of the content.
	std::wstring licensePath;
	uint32_t licenseMask = 0xFFFFFFFF;
};

struct TitleServerConfig {
	std::string name;
	std::string ip;
	uint16_t port = 0;
	uint32_t serviceId = 0;
	std::string info;
};

struct Config {
	// Diagnostics.
	LogLevel logLevel = LogLevel::Warn;
	std::wstring logPath;               // Empty = <module dir>\xlive_steamworks.log
	bool logToDebugger = false;

	// Steam.
	uint32_t appId = 0;                 // 0 = whatever steam_api resolves (steam_appid.txt or launcher).
	bool restartAppIfNecessary = false; // SteamAPI_RestartAppIfNecessary(appId) before init.
	bool requireSteam = true;           // false = keep running signed-out when Steam is absent.

	// Title identity.
	uint32_t titleId = 0;               // 0 = from the SPA.
	uint32_t titleVersion = 0;
	uint8_t language = 0;               // XLANGUAGE_*, 0 = from Steam's game language.

	// Users.
	bool asciiGamertags = true;
	bool localUsersAllSignedIn = false; // Report user indexes 1-3 as signed in locally.

	// Achievements.
	std::string achievementNameFormat = "ACH_%u";
	std::map<uint32_t, std::string> achievements;
	bool achievementsFromSteamWhenNoSpa = true;
	bool achievementsLocalFallback = true;  // Record unlocks locally when Steam has no such achievement.

	// Stats and leaderboards.
	std::string leaderboardNameFormat = "LB_%u";
	std::map<uint32_t, LeaderboardMapping> leaderboards;
	std::map<std::string, std::string> stats; // "<viewId>:<columnId>" -> Steam stat API name.
	bool leaderboardsCreateIfMissing = true;

	// Sessions.
	std::string lobbyKeyPrefix = "xl_";
	int lobbyDistanceFilter = 1;        // ELobbyDistanceFilter, 1 = default.
	bool sessionsAcceptAnyPeer = true;  // Accept SteamNetworkingMessages sessions from anyone.
	uint32_t asyncTimeoutMs = 15000;

	// Networking.
	bool relayOnly = false;             // Never share IP addresses, make every peer connection goes through a Steam relay.
	uint32_t sendRateKBytes = 0;        // Per-peer cap on outgoing bytes, 0 keeps Steam's default (256 KB/s).

	// Presence.
	std::string richPresenceKey = "status";
	bool richPresenceSteamDisplay = true;

	// Storage.
	bool cloudEnabled = true;
	std::wstring titleStorageDir;       // Read-only per-title files.. empty = <exe dir>\xlive-title-storage
	std::wstring localStorageDir;       // Fallback when Steam Cloud is off, empty = <exe dir>\xlive-storage

	// Content.
	bool dlcAutoDiscover = true;
	std::wstring dlcRootDir;            // Auto discovered DLC lives in <dlcRootDir>\<appid>, empty = <exe dir>\DLC
	std::vector<DlcMapping> dlc;

	// Title servers (XLSP).
	std::vector<TitleServerConfig> titleServers;

	// Voice.
	bool voiceEnabled = true;

	// UI.
	bool nativeDialogs = true;          // Win32 fallbacks for keyboard and message box UI.
	bool homeKeyOpensOverlay = true;    // GFWL opened the Guide on Home
};

bool ConfigLoad();
const Config& Cfg();
const std::wstring& ConfigPath();

// Run-time overrides from the extension API, consulted before the file.
bool ExtAchievementName(uint32_t achievementId, std::string* out);
bool ExtLeaderboardName(uint32_t viewId, std::string* out);

std::string AchievementApiName(uint32_t achievementId);
const LeaderboardMapping* LeaderboardFor(uint32_t viewId);
std::string LeaderboardName(uint32_t viewId);
const std::string* StatNameFor(uint32_t viewId, uint16_t columnId);

}
