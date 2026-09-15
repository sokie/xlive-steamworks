#include "core/config.h"

#include "core/json.h"
#include "core/utils.h"

#include <stdio.h>

namespace xls {

namespace {

Config g_config;
std::wstring g_configPath;

LogLevel ParseLogLevel(const std::string& text, LogLevel fallback)
{
	if (text == "off") return LogLevel::Off;
	if (text == "error") return LogLevel::Error;
	if (text == "warn" || text == "warning") return LogLevel::Warn;
	if (text == "info") return LogLevel::Info;
	if (text == "debug") return LogLevel::Debug;
	if (text == "trace") return LogLevel::Trace;
	return fallback;
}

uint32_t KeyToId(const std::string& key)
{
	return (uint32_t)strtoul(key.c_str(), nullptr, 0);
}

std::wstring PathSetting(const Json& value, const std::wstring& fallback)
{
	std::string text = value.GetString(std::string());
	if (text.empty()) {
		return fallback;
	}
	std::wstring wide = Utf8ToWide(text.c_str());
	// A relative path is relative to the exe.
	if (wide.size() < 2 || (wide[1] != L':' && wide[0] != L'\\')) {
		wide = ExeDirectory() + wide;
	}
	if (!wide.empty() && wide.back() != L'\\' && wide.back() != L'/') {
		wide.push_back(L'\\');
	}
	return wide;
}

void LoadLeaderboards(const Json& node, Config& config)
{
	for (const auto& item : node.Items()) {
		LeaderboardMapping mapping;
		mapping.viewId = KeyToId(item.first);
		const Json& entry = item.second;
		if (entry.IsString()) {
			mapping.name = entry.GetString(std::string());
		}
		else {
			mapping.name = entry["name"].GetString(std::string());
			mapping.ratingColumn = (int32_t)entry["rating_column"].GetInt(-1);
			for (const Json& column : entry["detail_columns"].Elements()) {
				mapping.detailColumns.push_back((uint16_t)column.GetInt(0));
			}
			mapping.keepBest = entry["keep_best"].GetBool(true);
			mapping.createIfMissing = entry["create_if_missing"].GetBool(config.leaderboardsCreateIfMissing);
			mapping.ascending = entry["ascending"].GetBool(false);
			mapping.displayType = (int)entry["display_type"].GetInt(1);
		}
		if (mapping.name.empty()) {
			mapping.name = FormatA(config.leaderboardNameFormat.c_str(), mapping.viewId);
		}
		config.leaderboards[mapping.viewId] = mapping;
	}
}

void LoadDlc(const Json& node, Config& config)
{
	for (const Json& entry : node.Elements()) {
		DlcMapping dlc;
		dlc.appId = entry["app_id"].GetUint32(0);
		std::string contentId = entry["content_id"].GetString(std::string());
		if (!contentId.empty() && !HexDecode(contentId, dlc.contentId, sizeof(dlc.contentId))) {
			XLS_LOG_WARN("config: dlc app %u has a content_id that is not 40 hex digits, a hash of the app id is used instead.", dlc.appId);
			contentId.clear();
		}
		if (contentId.empty()) {
			uint64_t hash = Fnv1a64(&dlc.appId, sizeof(dlc.appId));
			memcpy(dlc.contentId, &hash, sizeof(hash));
			uint32_t hash32 = Fnv1a32(&hash, sizeof(hash));
			memcpy(dlc.contentId + 8, &hash32, sizeof(hash32));
			memcpy(dlc.contentId + 12, &dlc.appId, sizeof(dlc.appId));
		}
		dlc.contentType = entry["content_type"].GetUint32(2);
		dlc.offerId = (uint64_t)entry["offer_id"].GetInt((int64_t)dlc.appId);
		dlc.displayName = Utf8ToWide(entry["display_name"].GetString(std::string()).c_str());
		dlc.path = PathSetting(entry["path"], std::wstring());
		dlc.licensePath = PathSetting(entry["license_path"], std::wstring());
		dlc.licenseMask = entry["license_mask"].GetUint32(0xFFFFFFFF);
		config.dlc.push_back(dlc);
	}
}

void LoadTitleServers(const Json& node, Config& config)
{
	for (const Json& entry : node.Elements()) {
		TitleServerConfig server;
		server.name = entry["name"].GetString(std::string());
		server.ip = entry["ip"].GetString(std::string());
		server.port = (uint16_t)entry["port"].GetInt(0);
		server.serviceId = entry["service_id"].GetUint32(0);
		server.info = entry["info"].GetString(server.name);
		if (!server.ip.empty()) {
			config.titleServers.push_back(server);
		}
	}
}

}

bool ConfigLoad()
{
	Config config;
	std::wstring moduleDir = ModuleDirectory();
	std::wstring exeDir = ExeDirectory();
	config.logPath = moduleDir + L"xlive_steamworks.log";
	config.titleStorageDir = exeDir + L"xlive-title-storage\\";
	config.localStorageDir = exeDir + L"xlive-storage\\";
	config.dlcRootDir = exeDir + L"DLC\\";

	g_configPath = moduleDir + L"xlive_steamworks.json";
	if (!FileExists(g_configPath)) {
		g_configPath = exeDir + L"xlive_steamworks.json";
	}

	std::vector<uint8_t> bytes;
	bool loaded = false;
	if (ReadFileBytes(g_configPath, bytes)) {
		std::string text(bytes.begin(), bytes.end());
		Json root;
		std::string error;
		if (Json::Parse(text, root, &error)) {
			loaded = true;

			const Json& log = root["log"];
			config.logLevel = ParseLogLevel(log["level"].GetString("warn"), LogLevel::Warn);
			config.logPath = PathSetting(log["path"], config.logPath);
			if (log["path"].IsString()) {
				// PathSetting appends a separator for directories, the log is a file.
				if (!config.logPath.empty() && config.logPath.back() == L'\\') {
					config.logPath.pop_back();
				}
			}
			config.logToDebugger = log["debugger"].GetBool(false);

			const Json& steam = root["steam"];
			config.appId = steam["app_id"].GetUint32(0);
			config.restartAppIfNecessary = steam["restart_app_if_necessary"].GetBool(false);
			config.requireSteam = steam["required"].GetBool(true);
			config.asyncTimeoutMs = steam["async_timeout_ms"].GetUint32(15000);

			const Json& title = root["title"];
			config.titleId = title["title_id"].GetUint32(0);
			config.titleVersion = title["title_version"].GetUint32(0);
			config.language = (uint8_t)title["language"].GetInt(0);

			const Json& users = root["users"];
			config.asciiGamertags = users["ascii_gamertags"].GetBool(true);
			config.localUsersAllSignedIn = users["extra_local_users_signed_in"].GetBool(false);

			const Json& achievements = root["achievements"];
			config.achievementNameFormat = achievements["name_format"].GetString("ACH_%u");
			config.achievementsFromSteamWhenNoSpa = achievements["from_steam_when_no_spa"].GetBool(true);
			for (const auto& item : achievements["map"].Items()) {
				config.achievements[KeyToId(item.first)] = item.second.GetString(std::string());
			}

			const Json& leaderboards = root["leaderboards"];
			config.leaderboardNameFormat = leaderboards["name_format"].GetString("LB_%u");
			config.leaderboardsCreateIfMissing = leaderboards["create_if_missing"].GetBool(true);
			LoadLeaderboards(leaderboards["map"], config);

			for (const auto& item : root["stats"].Items()) {
				config.stats[item.first] = item.second.GetString(std::string());
			}

			const Json& sessions = root["sessions"];
			config.lobbyKeyPrefix = sessions["lobby_key_prefix"].GetString("xl_");
			config.lobbyDistanceFilter = (int)sessions["lobby_distance_filter"].GetInt(1);
			config.sessionsAcceptAnyPeer = sessions["accept_any_peer"].GetBool(true);

			const Json& presence = root["presence"];
			config.richPresenceKey = presence["key"].GetString("status");
			config.richPresenceSteamDisplay = presence["steam_display"].GetBool(true);

			const Json& storage = root["storage"];
			config.cloudEnabled = storage["cloud"].GetBool(true);
			config.titleStorageDir = PathSetting(storage["title_dir"], config.titleStorageDir);
			config.localStorageDir = PathSetting(storage["local_dir"], config.localStorageDir);

			const Json& content = root["content"];
			config.dlcAutoDiscover = content["auto_discover"].GetBool(true);
			config.dlcRootDir = PathSetting(content["dlc_dir"], config.dlcRootDir);
			LoadDlc(content["dlc"], config);

			LoadTitleServers(root["title_servers"], config);

			config.voiceEnabled = root["voice"]["enabled"].GetBool(true);
			config.nativeDialogs = root["ui"]["native_dialogs"].GetBool(true);
		}
		else {
			g_config = config;
			LogInit(config.logPath, LogLevel::Warn, false);
			XLS_LOG_ERROR("config: %ls failed to parse: %s. Defaults are in use.", g_configPath.c_str(), error.c_str());
			return false;
		}
	}

	g_config = config;
	LogInit(config.logPath, config.logLevel, config.logToDebugger);
	if (loaded) {
		XLS_LOG_INFO("config: loaded %ls.", g_configPath.c_str());
	}
	else {
		XLS_LOG_INFO("config: no xlive_steamworks.json found, defaults are in use.");
	}
	return true;
}

const Config& Cfg()
{
	return g_config;
}

const std::wstring& ConfigPath()
{
	return g_configPath;
}

std::string AchievementApiName(uint32_t achievementId)
{
	std::string override;
	if (ExtAchievementName(achievementId, &override)) {
		return override;
	}
	auto it = g_config.achievements.find(achievementId);
	if (it != g_config.achievements.end() && !it->second.empty()) {
		return it->second;
	}
	return FormatA(g_config.achievementNameFormat.c_str(), achievementId);
}

const LeaderboardMapping* LeaderboardFor(uint32_t viewId)
{
	auto it = g_config.leaderboards.find(viewId);
	if (it != g_config.leaderboards.end()) {
		return &it->second;
	}
	return nullptr;
}

std::string LeaderboardName(uint32_t viewId)
{
	std::string override;
	if (ExtLeaderboardName(viewId, &override)) {
		return override;
	}
	const LeaderboardMapping* mapping = LeaderboardFor(viewId);
	if (mapping && !mapping->name.empty()) {
		return mapping->name;
	}
	return FormatA(g_config.leaderboardNameFormat.c_str(), viewId);
}

const std::string* StatNameFor(uint32_t viewId, uint16_t columnId)
{
	std::string key = FormatA("%u:%u", viewId, columnId);
	auto it = g_config.stats.find(key);
	if (it != g_config.stats.end() && !it->second.empty()) {
		return &it->second;
	}
	return nullptr;
}

}
