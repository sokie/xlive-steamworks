// Drives xlive.dll through the paths a title uses in its first minutes, against a live Steam
// client. Run with steam_appid.txt next to the exe, app 480 (Spacewar) is for testing only.
//
//   xlive_smoke.exe [--probe] [--spa Game.exe] [--filters] [--voice] [--query "N:id=value,..."]
//   xlive_smoke.exe --pair-host CODE | --pair-join CODE [--relay-only] [--timeout SECONDS]
//
// --probe reports what the app on Steam offers and touches nothing that persists on the app:
// no leaderboard is created and no achievement is written. --filters, --voice and --query run
// only that test. The pair modes need a second machine running the other side with the same code.
#include "common.h"

#include <algorithm>
#include <functional>

// steam_api sets SteamAppId itself when it reads steam_appid.txt, so this is sampled before init.
static bool g_launchedBySteam = false;

static void SteamSummary()
{
	// Stats and the relay configuration arrive a moment after init so give them a few seconds.
	for (int i = 0; i < 500; i++) {
		SteamRelayNetworkStatus_t relay = {};
		bool relayReady = SteamNetworkingUtils()->GetRelayNetworkStatus(&relay) == k_ESteamNetworkingAvailability_Current;
		bool statsReady = SteamUserStats()->GetNumAchievements() > 0;
		if (relayReady && (statsReady || i >= 300)) {
			break;
		}
		XLiveRender();
		Sleep(10);
	}
	AppId_t appId = SteamUtils()->GetAppID();
	INFO("app id %u, Steam user %llu \"%s\", logged on %d, language %s", appId, SteamUser()->GetSteamID().ConvertToUint64(), SteamFriends()->GetPersonaName(), (int)SteamUser()->BLoggedOn(), SteamApps()->GetCurrentGameLanguage());
	INFO("owns app: subscribed %d, subscribed app %d, build id %d", (int)SteamApps()->BIsSubscribed(), (int)SteamApps()->BIsSubscribedApp(appId), SteamApps()->GetAppBuildId());
	INFO("overlay: enabled in Steam %d, GameOverlayRenderer.dll loaded in this process %d, launched by Steam %d, big picture %d, steam hardware %d", (int)SteamUtils()->IsOverlayEnabled(), GetModuleHandleW(L"GameOverlayRenderer.dll") != nullptr, (int)g_launchedBySteam, (int)SteamUtils()->IsSteamInBigPictureMode(), (int)SteamUtils()->IsRunningOnSteamHardware());
	INFO("cloud: account %d, app %d", (int)SteamRemoteStorage()->IsCloudEnabledForAccount(), (int)SteamRemoteStorage()->IsCloudEnabledForApp());
	uint64 quotaTotal = 0;
	uint64 quotaFree = 0;
	if (SteamRemoteStorage()->GetQuota(&quotaTotal, &quotaFree)) {
		INFO("cloud quota: %llu bytes total, %llu free", quotaTotal, quotaFree);
	}
	else {
		INFO("cloud quota: not reported, API writes are likely refused and the wrapper keeps files next to the exe");
	}
	uint32 achievements = SteamUserStats()->GetNumAchievements();
	INFO("steam achievements defined for this app: %u", achievements);
	for (uint32 i = 0; i < achievements && i < 5; i++) {
		const char* name = SteamUserStats()->GetAchievementName(i);
		INFO("   %s = \"%s\"", name, SteamUserStats()->GetAchievementDisplayAttribute(name, "name"));
	}
	int dlcCount = SteamApps()->GetDLCCount();
	INFO("dlc apps listed for this app: %d", dlcCount);
	for (int i = 0; i < dlcCount && i < 70; i++) {
		AppId_t dlc = 0;
		bool available = false;
		char name[128] = {};
		if (SteamApps()->BGetDLCDataByIndex(i, &dlc, &available, name, sizeof(name))) {
			INFO("   %u \"%s\" available %d installed %d owned %d", dlc, name, (int)available, (int)SteamApps()->BIsDlcInstalled(dlc), (int)SteamApps()->BIsSubscribedApp(dlc));
		}
	}
	SteamRelayNetworkStatus_t relay = {};
	ESteamNetworkingAvailability availability = SteamNetworkingUtils()->GetRelayNetworkStatus(&relay);
	INFO("relay network: availability %d, config %d, any relay %d (%s)", (int)availability, (int)relay.m_eAvailNetworkConfig, (int)relay.m_eAvailAnyRelay, relay.m_debugMsg);
	SteamNetworkPingLocation_t location = {};
	float age = SteamNetworkingUtils()->GetLocalPingLocation(location);
	char locationText[k_cchMaxSteamNetworkingPingLocationString] = {};
	if (age >= 0) {
		SteamNetworkingUtils()->ConvertPingLocationToString(location, locationText, sizeof(locationText));
	}
	INFO("ping location: %s (measured %.0f s ago)", age >= 0 ? locationText : "not yet measured", age);
	int popCount = SteamNetworkingUtils()->GetPOPCount();
	std::vector<SteamNetworkingPOPID> pops(popCount > 0 ? popCount : 0);
	int listed = popCount > 0 ? SteamNetworkingUtils()->GetPOPList(pops.data(), popCount) : 0;
	struct PopPing { SteamNetworkingPOPID id; int ping; SteamNetworkingPOPID via; };
	std::vector<PopPing> pings;
	for (int i = 0; i < listed; i++) {
		SteamNetworkingPOPID via = 0;
		int ping = SteamNetworkingUtils()->GetPingToDataCenter(pops[i], &via);
		if (ping >= 0) {
			pings.push_back({ pops[i], ping, via });
		}
	}
	std::sort(pings.begin(), pings.end(), [](const PopPing& a, const PopPing& b) { return a.ping < b.ping; });
	std::string nearest;
	for (size_t i = 0; i < pings.size() && i < 6; i++) {
		char code[8] = {};
		GetSteamNetworkingLocationPOPStringFromID(pings[i].id, code);
		char viaCode[8] = {};
		if (pings[i].via) {
			GetSteamNetworkingLocationPOPStringFromID(pings[i].via, viaCode);
		}
		nearest += Format("%s %d ms%s%s  ", code, pings[i].ping, pings[i].via ? " via " : "", viaCode);
	}
	INFO("relay data centres: %d known, %zu reachable, nearest %s", listed, pings.size(), nearest.c_str());

	SteamAPICall_t call = SteamUserStats()->FindLeaderboard("LB_1");
	LeaderboardFindResult_t found = {};
	if (WaitSteamCall(call, &found) && found.m_bLeaderboardFound) {
		INFO("leaderboard LB_1 exists on this app (%d entries)", SteamUserStats()->GetLeaderboardEntryCount(found.m_hSteamLeaderboard));
	}
	else {
		INFO("leaderboard LB_1 does not exist on this app, the first XSessionWriteStats creates it when leaderboards.create_if_missing is on");
	}
}

static void TestNotifications(HANDLE listener)
{
	bool signin = false;
	bool connection = false;
	for (int i = 0; i < 50 && !(signin && connection); i++) {
		DWORD id = 0;
		ULONG_PTR param = 0;
		while (XNotifyGetNext(listener, 0, &id, &param)) {
			printf("       notification 0x%08x param 0x%p\n", id, (void*)param);
			if (id == XN_SYS_SIGNINCHANGED) signin = true;
			if (id == XN_LIVE_CONNECTIONCHANGED) connection = true;
		}
		Sleep(10);
	}
	CHECK(signin, "XN_SYS_SIGNINCHANGED delivered");
	CHECK(connection, "XN_LIVE_CONNECTIONCHANGED delivered");
}

static void TestUser()
{
	XUSER_SIGNIN_STATE state = XUserGetSigninState(0);
	CHECK(state == eXUserSigninState_SignedInToLive, "user 0 signed in to LIVE (state %u)", state);
	XUID xuid = 0;
	CHECK(XUserGetXUID(0, &xuid) == ERROR_SUCCESS && IsOnlineXUID(xuid), "XUID 0x%016llx is online", xuid);
	char name[XUSER_NAME_SIZE] = {};
	CHECK(XUserGetName(0, name, sizeof(name)) == ERROR_SUCCESS && name[0], "gamertag \"%s\"", name);
	CHECK(XlsSteamIdFromXuid(xuid) != 0 && XlsXuidFromSteamId(XlsSteamIdFromXuid(xuid)) == xuid, "XUID <-> SteamID round trip");
	BOOL privileged = FALSE;
	CHECK(XUserCheckPrivilege(0, XPRIVILEGE_MULTIPLAYER_SESSIONS, &privileged) == ERROR_SUCCESS && privileged, "multiplayer privilege");
	CHECK(XUserGetSigninState(1) == eXUserSigninState_NotSignedIn, "user 1 not signed in");
}

static void TestAchievements()
{
	DWORD bufferSize = 0;
	HANDLE enumerator = nullptr;
	DWORD result = XUserCreateAchievementEnumerator(0, 0, INVALID_XUID, XACHIEVEMENT_DETAILS_ALL, 0, 32, &bufferSize, &enumerator);
	CHECK(result == ERROR_SUCCESS && enumerator, "achievement enumerator (buffer %u)", bufferSize);
	if (result != ERROR_SUCCESS) {
		return;
	}
	std::vector<uint8_t> buffer(bufferSize);
	DWORD count = 0;
	result = XEnumerate(enumerator, buffer.data(), bufferSize, &count, nullptr);
	CHECK(result == ERROR_SUCCESS || result == ERROR_NO_MORE_FILES, "XEnumerate achievements -> %u (%u items)", result, count);
	XACHIEVEMENT_DETAILS* details = (XACHIEVEMENT_DETAILS*)buffer.data();
	for (DWORD i = 0; i < count && i < 5; i++) {
		printf("       %u: %ls (%u pts) %s\n", details[i].dwId, details[i].pwszLabel ? details[i].pwszLabel : L"", details[i].dwCred, (details[i].dwFlags & XACHIEVEMENT_DETAILS_ACHIEVED) ? "unlocked" : "locked");
	}
	XCloseHandle(enumerator);
	printf("       (achievement %u maps to Steam API name %s)\n", 1, XlsGetAchievementName(1));
	if (g_probe) {
		return;
	}

	// An id the app has no Steam achievement for still unlocks through the local record.
	XUSER_ACHIEVEMENT unlock = { 0, 1 };
	XOVERLAPPED overlapped = {};
	result = XUserWriteAchievements(1, &unlock, &overlapped);
	CHECK(result == ERROR_IO_PENDING && Wait(&overlapped) == ERROR_SUCCESS, "XUserWriteAchievements id 1");
	result = XUserCreateAchievementEnumerator(0, 0, INVALID_XUID, XACHIEVEMENT_DETAILS_ALL, 0, 32, &bufferSize, &enumerator);
	if (result == ERROR_SUCCESS) {
		std::vector<uint8_t> again(bufferSize);
		DWORD countAgain = 0;
		XEnumerate(enumerator, again.data(), bufferSize, &countAgain, nullptr);
		XACHIEVEMENT_DETAILS* list = (XACHIEVEMENT_DETAILS*)again.data();
		bool unlocked = false;
		for (DWORD i = 0; i < countAgain; i++) {
			if (list[i].dwId == 1 && (list[i].dwFlags & XACHIEVEMENT_DETAILS_ACHIEVED)) {
				unlocked = true;
			}
		}
		CHECK(unlocked || countAgain == 0, "achievement 1 reads back as unlocked (%u listed)", countAgain);
		XCloseHandle(enumerator);
	}
}

static void TestProfile()
{
	DWORD ids[] = { XPROFILE_GAMERCARD_REGION, XPROFILE_GAMERCARD_CRED, XPROFILE_TITLE_SPECIFIC1 };
	DWORD size = 0;
	DWORD result = XUserReadProfileSettings(0, 0, 3, ids, &size, nullptr, nullptr);
	CHECK(result == ERROR_INSUFFICIENT_BUFFER && size, "profile read size query -> %u bytes", size);
	std::vector<uint8_t> buffer(size);
	XUSER_READ_PROFILE_SETTING_RESULT* settings = (XUSER_READ_PROFILE_SETTING_RESULT*)buffer.data();
	result = XUserReadProfileSettings(0, 0, 3, ids, &size, settings, nullptr);
	CHECK(result == ERROR_SUCCESS && settings->dwSettingsLen == 3, "profile read -> %u settings", settings->dwSettingsLen);
	if (result == ERROR_SUCCESS) {
		printf("       region %d cred %d title1 %u bytes (source %d)\n", settings->pSettings[0].data.nData, settings->pSettings[1].data.nData, settings->pSettings[2].data.binary.cbData, settings->pSettings[2].source);
	}

	uint8_t blob[16] = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15, 16 };
	XUSER_PROFILE_SETTING write = {};
	write.dwSettingId = XPROFILE_TITLE_SPECIFIC1;
	write.data.type = XUSER_DATA_TYPE_BINARY;
	write.data.binary.cbData = sizeof(blob);
	write.data.binary.pbData = blob;
	XOVERLAPPED overlapped = {};
	result = XUserWriteProfileSettings(0, 1, &write, &overlapped);
	CHECK(result == ERROR_IO_PENDING && Wait(&overlapped) == ERROR_SUCCESS, "profile write TITLE_SPECIFIC1");

	result = XUserReadProfileSettings(0, 0, 3, ids, &size, settings, nullptr);
	CHECK(result == ERROR_SUCCESS && settings->pSettings[2].data.binary.cbData == sizeof(blob) && memcmp(settings->pSettings[2].data.binary.pbData, blob, sizeof(blob)) == 0, "profile read back TITLE_SPECIFIC1");
}

// Where the last upload landed: Steam Cloud (and whether it is persisted) or the local folder.
static void ReportCloudResidency(const char* suffix)
{
	if (!SteamRemoteStorage()->IsCloudEnabledForApp()) {
		INFO("Steam Cloud is off for this app, the wrapper keeps files in its local folder");
		return;
	}
	int32 count = SteamRemoteStorage()->GetFileCount();
	for (int32 i = 0; i < count; i++) {
		int32 size = 0;
		const char* name = SteamRemoteStorage()->GetFileNameAndSize(i, &size);
		size_t length = name ? strlen(name) : 0;
		size_t suffixLength = strlen(suffix);
		if (name && length >= suffixLength && strcmp(name + length - suffixLength, suffix) == 0) {
			INFO("Steam Cloud holds \"%s\" (%d bytes, persisted %d, %d file(s) in the app's cloud)", name, size, (int)SteamRemoteStorage()->FilePersisted(name), count);
			return;
		}
	}
	INFO("Steam Cloud does not hold a file ending in \"%s\" (%d cloud file(s)), the Cloud API refused the write and the wrapper kept it in its local folder", suffix, count);
}

static void TestStorage()
{
	wchar_t path[256] = {};
	DWORD length = 256;
	DWORD result = XStorageBuildServerPath(0, XSTORAGE_FACILITY_PER_USER_TITLE, nullptr, 0, L"smoke/test.bin", path, &length);
	CHECK(result == ERROR_SUCCESS, "XStorageBuildServerPath -> %ls", path);

	const char payload[] = "hello from xlive-steamworks";
	XOVERLAPPED overlapped = {};
	result = XStorageUploadFromMemory(0, path, sizeof(payload), (const uint8_t*)payload, &overlapped);
	CHECK(result == ERROR_IO_PENDING && Wait(&overlapped) == ERROR_SUCCESS, "XStorageUploadFromMemory");
	ReportCloudResidency("smoke/test.bin");

	uint8_t buffer[64] = {};
	XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS info = {};
	result = XStorageDownloadToMemory(0, path, sizeof(buffer), buffer, sizeof(info), &info, nullptr);
	CHECK(result == ERROR_SUCCESS && info.dwBytesTotal == sizeof(payload) && memcmp(buffer, payload, sizeof(payload)) == 0, "XStorageDownloadToMemory (%u bytes)", info.dwBytesTotal);

	wchar_t pattern[256] = {};
	length = 256;
	XStorageBuildServerPath(0, XSTORAGE_FACILITY_PER_USER_TITLE, nullptr, 0, L"smoke/*", pattern, &length);
	std::vector<uint8_t> results(sizeof(XSTORAGE_ENUMERATE_RESULTS) + 4 * (sizeof(XSTORAGE_FILE_INFO) + XONLINE_MAX_PATHNAME_LENGTH * sizeof(wchar_t)));
	XSTORAGE_ENUMERATE_RESULTS* enumerated = (XSTORAGE_ENUMERATE_RESULTS*)results.data();
	result = XStorageEnumerate(0, pattern, 0, 4, (DWORD)results.size(), enumerated, nullptr);
	CHECK(result == ERROR_SUCCESS && enumerated->dwNumItemsReturned >= 1, "XStorageEnumerate -> %u item(s)", enumerated->dwNumItemsReturned);

	result = XStorageDelete(0, path, nullptr);
	CHECK(result == ERROR_SUCCESS, "XStorageDelete");
}

// XEnumerate on a stats enumerator reports ERROR_IO_INCOMPLETE until the download lands.
static DWORD EnumerateSync(HANDLE enumerator, void* buffer, DWORD size, DWORD* count)
{
	DWORD result = ERROR_IO_INCOMPLETE;
	for (int i = 0; i < 1000 && result == ERROR_IO_INCOMPLETE; i++) {
		result = XEnumerate(enumerator, buffer, size, count, nullptr);
		if (result == ERROR_IO_INCOMPLETE) {
			Pump(1);
		}
	}
	return result;
}

static void PrintRows(const XUSER_STATS_VIEW& view, int limit)
{
	for (DWORD r = 0; r < view.dwNumRows && (int)r < limit; r++) {
		const XUSER_STATS_ROW& row = view.pRows[r];
		printf("       rank %u %s rating %lld columns %u: %d, %lld\n", row.dwRank, row.szGamertag, row.i64Rating, row.dwNumColumns,
			row.dwNumColumns > 0 ? row.pColumns[0].Value.nData : 0, row.dwNumColumns > 1 ? row.pColumns[1].Value.i64Data : 0);
	}
}

static void TestStats(HANDLE session, XUID xuid)
{
	XSESSION_VIEW_PROPERTIES view = {};
	XUSER_PROPERTY properties[2] = {};
	properties[0].dwPropertyId = XPROPERTYID(0, XUSER_DATA_TYPE_INT32, 1);
	properties[0].value.type = XUSER_DATA_TYPE_INT32;
	properties[0].value.nData = 1234;
	properties[1].dwPropertyId = XPROPERTYID(0, XUSER_DATA_TYPE_INT64, 2);
	properties[1].value.type = XUSER_DATA_TYPE_INT64;
	properties[1].value.i64Data = 5678;
	view.dwViewId = 1;
	view.dwNumProperties = 2;
	view.pProperties = properties;
	DWORD result = XSessionWriteStats(session, xuid, 1, &view, nullptr);
	CHECK(result == ERROR_SUCCESS, "XSessionWriteStats view 1 (score 1234, detail 5678)");
	Pump(300);

	XUSER_STATS_SPEC spec = {};
	spec.dwViewId = 1;
	spec.dwNumColumnIds = 2;
	spec.rgwColumnIds[0] = 1;
	spec.rgwColumnIds[1] = 2;
	DWORD statsSize = 0;
	result = XUserReadStats(0, 1, &xuid, 1, &spec, &statsSize, nullptr, nullptr);
	std::vector<uint8_t> statsBuffer(statsSize);
	XUSER_STATS_READ_RESULTS* stats = (XUSER_STATS_READ_RESULTS*)statsBuffer.data();
	result = XUserReadStats(0, 1, &xuid, 1, &spec, &statsSize, stats, nullptr);
	CHECK(result == ERROR_SUCCESS && stats->dwNumViews == 1 && stats->pViews[0].dwNumRows == 1, "XUserReadStats -> %u", result);
	if (result != ERROR_SUCCESS || stats->dwNumViews != 1 || stats->pViews[0].dwNumRows != 1) {
		return;
	}
	XUSER_STATS_ROW& row = stats->pViews[0].pRows[0];
	PrintRows(stats->pViews[0], 1);
	if (row.dwRank == 0) {
		INFO("leaderboard LB_1 does not exist on this app and create_if_missing is off, the rank reads are skipped");
		return;
	}
	CHECK(row.dwRank >= 1 && row.i64Rating >= 1234, "leaderboard LB_1 holds our score");

	// The two reads a title's leaderboard screen makes: the top of the board and the rows around us.
	DWORD bufferSize = 0;
	HANDLE enumerator = nullptr;
	result = XUserCreateStatsEnumeratorByRank(0, 1, 10, 1, &spec, &bufferSize, &enumerator);
	CHECK(result == ERROR_SUCCESS && enumerator, "XUserCreateStatsEnumeratorByRank(1..10)");
	if (result == ERROR_SUCCESS) {
		std::vector<uint8_t> buffer(bufferSize);
		DWORD items = 0;
		result = EnumerateSync(enumerator, buffer.data(), bufferSize, &items);
		XUSER_STATS_READ_RESULTS* top = (XUSER_STATS_READ_RESULTS*)buffer.data();
		bool ok = result == ERROR_SUCCESS && top->dwNumViews == 1 && top->pViews[0].dwNumRows >= 1;
		CHECK(ok, "top-10 read -> %u row(s) of %u on the board (result %u)", ok ? top->pViews[0].dwNumRows : 0, ok ? top->pViews[0].dwTotalViewRows : 0, result);
		if (ok) {
			PrintRows(top->pViews[0], 3);
			bool ranked = false;
			for (DWORD r = 0; r < top->pViews[0].dwNumRows; r++) {
				if (top->pViews[0].pRows[r].xuid == xuid && top->pViews[0].pRows[r].dwRank == r + 1) {
					ranked = true;
				}
			}
			CHECK(ranked || top->pViews[0].dwNumRows == 10, "our entry appears at its rank in the top 10");
		}
		XCloseHandle(enumerator);
	}
	result = XUserCreateStatsEnumeratorByXuid(0, xuid, 5, 1, &spec, &bufferSize, &enumerator);
	CHECK(result == ERROR_SUCCESS && enumerator, "XUserCreateStatsEnumeratorByXuid(around us, 5 rows)");
	if (result == ERROR_SUCCESS) {
		std::vector<uint8_t> buffer(bufferSize);
		DWORD items = 0;
		result = EnumerateSync(enumerator, buffer.data(), bufferSize, &items);
		XUSER_STATS_READ_RESULTS* around = (XUSER_STATS_READ_RESULTS*)buffer.data();
		bool ok = result == ERROR_SUCCESS && around->dwNumViews == 1 && around->pViews[0].dwNumRows >= 1;
		bool present = false;
		for (DWORD r = 0; ok && r < around->pViews[0].dwNumRows; r++) {
			present = present || around->pViews[0].pRows[r].xuid == xuid;
		}
		CHECK(ok && present, "around-user read -> %u row(s), ours included %d (result %u)", ok ? around->pViews[0].dwNumRows : 0, (int)present, result);
		if (ok) {
			PrintRows(around->pViews[0], 3);
		}
		XCloseHandle(enumerator);
	}
}

static void TestNetworkAndSession()
{
	XNetStartupParams params = {};
	params.cfgSizeOfStruct = sizeof(params);
	CHECK(XNetStartup(&params) == 0, "XNetStartup");
	XNADDR local = {};
	DWORD status = XNetGetTitleXnAddr(&local);
	CHECK(status & XNET_GET_XNADDR_ONLINE, "XNetGetTitleXnAddr online (status 0x%x)", status);
	uint64_t machine = 0;
	CHECK(XNetXnAddrToMachineId(&local, &machine) == 0 && (machine >> 56) == 0xFA, "machine id 0x%016llx", machine);

	IN_ADDR alias = {};
	XNKID anyKey = {};
	CHECK(XNetXnAddrToInAddr(&local, &anyKey, &alias) == 0, "XNetXnAddrToInAddr(local) -> %u.%u.%u.%u", alias.S_un.S_un_b.s_b1, alias.S_un.S_un_b.s_b2, alias.S_un.S_un_b.s_b3, alias.S_un.S_un_b.s_b4);

	SOCKET socket = XSocketCreate(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
	CHECK(socket != INVALID_SOCKET, "XSocketCreate");
	sockaddr_in bind = {};
	bind.sin_family = AF_INET;
	bind.sin_port = htons(1000);
	CHECK(XSocketBind(socket, (sockaddr*)&bind, sizeof(bind)) == 0, "XSocketBind 1000");
	u_long nonBlocking = 1;
	XSocketIOCTLSocket(socket, FIONBIO, &nonBlocking);
	sockaddr_in to = {};
	to.sin_family = AF_INET;
	to.sin_addr = alias;
	to.sin_port = htons(1000);
	const char datagram[] = "ping";
	CHECK(XSocketSendTo(socket, datagram, sizeof(datagram), 0, (sockaddr*)&to, sizeof(to)) == sizeof(datagram), "XSocketSendTo self");
	char received[32] = {};
	sockaddr_in from = {};
	int fromLength = sizeof(from);
	int got = XSocketRecvFrom(socket, received, sizeof(received), 0, (sockaddr*)&from, &fromLength);
	CHECK(got == sizeof(datagram) && memcmp(received, datagram, sizeof(datagram)) == 0 && ntohs(from.sin_port) == 1000, "XSocketRecvFrom loopback (%d bytes)", got);
	XSocketClose(socket);

	XUserSetContext(0, X_CONTEXT_GAME_TYPE, X_CONTEXT_GAME_TYPE_STANDARD);
	XUserSetContext(0, X_CONTEXT_GAME_MODE, 3);
	uint64_t nonce = 0;
	XSESSION_INFO info = {};
	HANDLE session = nullptr;
	XOVERLAPPED overlapped = {};
	DWORD result = XSessionCreate(XSESSION_CREATE_HOST | XSESSION_CREATE_USES_PRESENCE | XSESSION_CREATE_USES_MATCHMAKING | XSESSION_CREATE_USES_PEER_NETWORK | XSESSION_CREATE_USES_STATS, 0, 4, 2, &nonce, &info, &overlapped, &session);
	DWORD created = result == ERROR_IO_PENDING ? Wait(&overlapped) : result;
	CHECK(created == ERROR_SUCCESS && session, "XSessionCreate host -> lobby %llu", XlsLobbyIdFromXnkid(&info.sessionID));
	if (created != ERROR_SUCCESS) {
		return;
	}
	CHECK(XNetRegisterKey(&info.sessionID, &info.keyExchangeKey) == 0, "XNetRegisterKey(session)");
	CHECK(XlsLobbyIdFromSession(session) == XlsLobbyIdFromXnkid(&info.sessionID), "XlsLobbyIdFromSession matches");

	DWORD userIndex = 0;
	BOOL privateSlot = FALSE;
	CHECK(XSessionJoinLocal(session, 1, &userIndex, &privateSlot, nullptr) == ERROR_SUCCESS, "XSessionJoinLocal");
	Pump(30);

	DWORD detailsSize = 0;
	result = XSessionGetDetails(session, &detailsSize, nullptr, nullptr);
	std::vector<uint8_t> detailsBuffer(detailsSize);
	XSESSION_LOCAL_DETAILS* details = (XSESSION_LOCAL_DETAILS*)detailsBuffer.data();
	result = XSessionGetDetails(session, &detailsSize, details, nullptr);
	CHECK(result == ERROR_SUCCESS && details->dwActualMemberCount >= 1 && details->dwMaxPublicSlots == 4, "XSessionGetDetails: %u member(s), state %d, host index %u", details->dwActualMemberCount, details->eState, details->dwUserIndexHost);

	DWORD searchSize = 0;
	result = XSessionSearchByID(info.sessionID, 0, &searchSize, nullptr, nullptr);
	std::vector<uint8_t> searchBuffer(searchSize);
	XSESSION_SEARCHRESULT_HEADER* header = (XSESSION_SEARCHRESULT_HEADER*)searchBuffer.data();
	result = XSessionSearchByID(info.sessionID, 0, &searchSize, header, nullptr);
	CHECK(result == ERROR_SUCCESS && header->dwSearchResults == 1, "XSessionSearchByID -> %u result(s)", header->dwSearchResults);
	if (result == ERROR_SUCCESS && header->dwSearchResults == 1) {
		XSESSION_SEARCHRESULT& found = header->pResults[0];
		CHECK(memcmp(&found.info.keyExchangeKey, &info.keyExchangeKey, sizeof(XNKEY)) == 0, "search result carries the XNKEY");
		CHECK(found.cContexts >= 2, "search result carries %u context(s), %u propert(ies)", found.cContexts, found.cProperties);
	}

	// A public search from the same client hides its own lobby, this checks the request itself.
	DWORD listSize = 0;
	result = XSessionSearch(0, 0, 10, 0, 0, nullptr, nullptr, &listSize, nullptr, nullptr);
	std::vector<uint8_t> listBuffer(listSize);
	XSESSION_SEARCHRESULT_HEADER* list = (XSESSION_SEARCHRESULT_HEADER*)listBuffer.data();
	result = XSessionSearch(0, 0, 10, 0, 0, nullptr, nullptr, &listSize, list, nullptr);
	CHECK(result == ERROR_SUCCESS, "XSessionSearch -> %u other session(s) of this title", list->dwSearchResults);

	if (!g_probe) {
		XUID xuid = 0;
		XUserGetXUID(0, &xuid);
		TestStats(session, xuid);
	}

	CHECK(XSessionDelete(session, nullptr) == ERROR_SUCCESS, "XSessionDelete");
	CHECK(XCloseHandle(session), "XCloseHandle(session)");
	XNetUnregisterKey(&info.sessionID);
}

static void TestFriends()
{
	DWORD bufferSize = 0;
	HANDLE enumerator = nullptr;
	DWORD result = XFriendsCreateEnumerator(0, 0, 20, &bufferSize, &enumerator);
	CHECK(result == ERROR_SUCCESS, "XFriendsCreateEnumerator");
	if (result != ERROR_SUCCESS) {
		return;
	}
	std::vector<uint8_t> buffer(bufferSize);
	DWORD count = 0;
	result = XEnumerate(enumerator, buffer.data(), bufferSize, &count, nullptr);
	CHECK(result == ERROR_SUCCESS || result == ERROR_NO_MORE_FILES, "XEnumerate friends -> %u friend(s)", count);
	XONLINE_FRIEND* friends = (XONLINE_FRIEND*)buffer.data();
	for (DWORD i = 0; i < count && i < 3 && !g_probe; i++) {
		printf("       %s state 0x%08x \"%ls\"\n", friends[i].szGamertag, friends[i].dwFriendState, friends[i].wszRichPresence);
	}
	XCloseHandle(enumerator);
}

// Lists lobbies straight from Steam with the wrapper's keys, so the filters can be checked against
// this client's own lobby (the wrapper's XSessionSearch hides it). Returns 1 when our lobby is
// listed, 0 when not, -1 when the request failed.
static int OwnLobbyListed(uint64_t lobby, const std::function<void()>& addFilters)
{
	SteamMatchmaking()->AddRequestLobbyListStringFilter("xl_kind", "session", k_ELobbyComparisonEqual);
	SteamMatchmaking()->AddRequestLobbyListDistanceFilter(k_ELobbyDistanceFilterWorldwide);
	SteamMatchmaking()->AddRequestLobbyListResultCountFilter(50);
	addFilters();
	LobbyMatchList_t list = {};
	if (!WaitSteamCall(SteamMatchmaking()->RequestLobbyList(), &list)) {
		return -1;
	}
	for (uint32 i = 0; i < list.m_nLobbiesMatching; i++) {
		if (SteamMatchmaking()->GetLobbyByIndex((int)i).ConvertToUint64() == lobby) {
			return 1;
		}
	}
	return 0;
}

// Publishes a lobby shaped like GTA IV's ranked search (a context plus 26 numeric properties and
// a 64-bit one) and checks that Steam's server-side filters match and exclude it as expected.
static void TestFilters()
{
	XUserSetContext(0, X_CONTEXT_GAME_TYPE, X_CONTEXT_GAME_TYPE_STANDARD);
	XUserSetContext(0, X_CONTEXT_GAME_MODE, 4242);
	for (DWORD i = 0; i < 26; i++) {
		LONG value = 100 + (LONG)i;
		XUserSetProperty(0, XPROPERTYID(0, XUSER_DATA_TYPE_INT32, 0x40 + i), sizeof(value), &value);
	}
	LONGLONG wide = 5000000000LL;
	XUserSetProperty(0, XPROPERTYID(0, XUSER_DATA_TYPE_INT64, 0x60), sizeof(wide), &wide);

	uint64_t nonce = 0;
	XSESSION_INFO info = {};
	HANDLE session = nullptr;
	XOVERLAPPED overlapped = {};
	DWORD result = XSessionCreate(XSESSION_CREATE_HOST | XSESSION_CREATE_USES_MATCHMAKING | XSESSION_CREATE_USES_PEER_NETWORK, 0, 4, 0, &nonce, &info, &overlapped, &session);
	DWORD created = result == ERROR_IO_PENDING ? Wait(&overlapped) : result;
	CHECK(created == ERROR_SUCCESS && session, "XSessionCreate host with 27 properties -> lobby %llu", XlsLobbyIdFromXnkid(&info.sessionID));
	if (created != ERROR_SUCCESS) {
		return;
	}
	uint64_t lobby = XlsLobbyIdFromSession(session);
	Pump(100);
	int keys = SteamMatchmaking()->GetLobbyDataCount(CSteamID(lobby));
	INFO("lobby carries %d data keys: xl_c0000800b=%s xl_p10000040=%s xl_p20000060=%s", keys, SteamMatchmaking()->GetLobbyData(CSteamID(lobby), "xl_c0000800b"), SteamMatchmaking()->GetLobbyData(CSteamID(lobby), "xl_p10000040"), SteamMatchmaking()->GetLobbyData(CSteamID(lobby), "xl_p20000060"));

	int control = OwnLobbyListed(lobby, [] {});
	INFO("control listing without extra filters: own lobby listed %d", control);
	if (control != 1) {
		INFO("Steam does not list this client's own lobby, so the filter checks below need a second machine (the pair test covers them)");
	}
	auto numeric = [](const char* key, int value, ELobbyComparison compare) {
		SteamMatchmaking()->AddRequestLobbyListNumericalFilter(key, value, compare);
	};
	auto allMatching = [&] {
		numeric("xl_c0000800b", 4242, k_ELobbyComparisonEqual);
		for (int i = 0; i < 26; i++) {
			numeric(Format("xl_p%08x", XPROPERTYID(0, XUSER_DATA_TYPE_INT32, 0x40 + i)).c_str(), 100 + i, k_ELobbyComparisonEqual);
		}
	};
	int positive = OwnLobbyListed(lobby, allMatching);
	int ranged = OwnLobbyListed(lobby, [&] {
		numeric("xl_c0000800b", 4242, k_ELobbyComparisonEqual);
		numeric("xl_p10000040", 100, k_ELobbyComparisonEqualToOrLessThan);
		numeric("xl_p10000040", 100, k_ELobbyComparisonEqualToOrGreaterThan);
		numeric("xl_p10000041", 200, k_ELobbyComparisonLessThan);
		numeric("xl_p10000042", 50, k_ELobbyComparisonGreaterThan);
		numeric("xl_p10000043", 999, k_ELobbyComparisonNotEqual);
	});
	int text = OwnLobbyListed(lobby, [&] {
		SteamMatchmaking()->AddRequestLobbyListStringFilter("xl_p20000060", "5000000000", k_ELobbyComparisonEqual);
	});
	int wrongMode = OwnLobbyListed(lobby, [&] { numeric("xl_c0000800b", 4243, k_ELobbyComparisonEqual); });
	int wrongValue = OwnLobbyListed(lobby, [&] { numeric("xl_p10000040", 100, k_ELobbyComparisonGreaterThan); });
	int wrongOneOf26 = OwnLobbyListed(lobby, [&] {
		allMatching();
		numeric("xl_p10000059", 126, k_ELobbyComparisonEqual);
	});
	if (control == 1) {
		CHECK(positive == 1, "context + 26 numeric equality filters match our lobby");
		CHECK(ranged == 1, "<=, >=, <, > and != numeric filters match our lobby");
		CHECK(text == 1, "64-bit property matches as text");
		CHECK(wrongMode == 0, "a different game mode excludes our lobby");
		CHECK(wrongValue == 0, "a failing range filter excludes our lobby");
		CHECK(wrongOneOf26 == 0, "one wrong filter out of 27 excludes our lobby");
	}
	else {
		INFO("filter results (1 listed, 0 not, -1 failed): all-match %d, ranges %d, text %d, wrong mode %d, wrong value %d, one-of-27 wrong %d", positive, ranged, text, wrongMode, wrongValue, wrongOneOf26);
	}

	// Through the wrapper: the XLAST-free path filters on every context and property passed.
	XUSER_CONTEXT context = { X_CONTEXT_GAME_MODE, 4242 };
	XUSER_PROPERTY property = {};
	property.dwPropertyId = XPROPERTYID(0, XUSER_DATA_TYPE_INT32, 0x40);
	property.value.type = XUSER_DATA_TYPE_INT32;
	property.value.nData = 100;
	DWORD size = 0;
	XSessionSearch(0, 0, 10, 1, 1, &property, &context, &size, nullptr, nullptr);
	std::vector<uint8_t> buffer(size);
	XSESSION_SEARCHRESULT_HEADER* header = (XSESSION_SEARCHRESULT_HEADER*)buffer.data();
	result = XSessionSearch(0, 0, 10, 1, 1, &property, &context, &size, header, nullptr);
	CHECK(result == ERROR_SUCCESS, "XSessionSearch with a context and a property -> %u other session(s) (the log lists the filters sent)", header->dwSearchResults);

	XSessionDelete(session, nullptr);
	XCloseHandle(session);
}

// Runs one XLAST query of the loaded SPA with the parameters a title would pass, given as
// "procedure:id=value,id=value". The debug log then lists the filters sent to Steam.
static void TestQuery(const char* spec)
{
	DWORD procedure = (DWORD)strtoul(spec, nullptr, 10);
	const char* cursor = strchr(spec, ':');
	std::vector<XUSER_CONTEXT> contexts;
	std::vector<XUSER_PROPERTY> properties;
	while (cursor && *cursor) {
		cursor++;
		char* end = nullptr;
		DWORD id = (DWORD)strtoul(cursor, &end, 0);
		if (!end || *end != '=') {
			break;
		}
		LONGLONG value = _strtoi64(end + 1, &end, 0);
		if (XPROPERTYTYPEFROMID(id) == XUSER_DATA_TYPE_CONTEXT) {
			contexts.push_back({ id, (DWORD)value });
		}
		else {
			XUSER_PROPERTY property = {};
			property.dwPropertyId = id;
			property.value.type = (uint8_t)XPROPERTYTYPEFROMID(id);
			if (property.value.type == XUSER_DATA_TYPE_INT64) {
				property.value.i64Data = value;
			}
			else {
				property.value.nData = (LONG)value;
			}
			properties.push_back(property);
		}
		cursor = end && *end == ',' ? end : nullptr;
	}
	DWORD size = 0;
	XSessionSearch(procedure, 0, 10, (WORD)properties.size(), (WORD)contexts.size(), properties.data(), contexts.data(), &size, nullptr, nullptr);
	std::vector<uint8_t> buffer(size);
	XSESSION_SEARCHRESULT_HEADER* header = (XSESSION_SEARCHRESULT_HEADER*)buffer.data();
	DWORD result = XSessionSearch(procedure, 0, 10, (WORD)properties.size(), (WORD)contexts.size(), properties.data(), contexts.data(), &size, header, nullptr);
	CHECK(result == ERROR_SUCCESS, "XSessionSearch query %u with %zu propert(ies), %zu context(s) -> %u session(s), the log lists the filters", procedure, properties.size(), contexts.size(), header->dwSearchResults);
}

// Captures through Steam's voice codec and feeds the frames back into the engine as if a remote
// talker sent them, which is the whole in-game voice path minus the network.
static void TestVoice()
{
	XHV_PROCESSING_MODE modes[] = { XHV_VOICECHAT_MODE };
	XHV_INIT_PARAMS params = {};
	params.dwMaxRemoteTalkers = 4;
	params.dwMaxLocalTalkers = 1;
	params.localTalkerEnabledModes = modes;
	params.dwNumLocalTalkerEnabledModes = 1;
	params.remoteTalkerEnabledModes = modes;
	params.dwNumRemoteTalkerEnabledModes = 1;
	IXHVEngine* engine = nullptr;
	HANDLE worker = nullptr;
	HRESULT hr = XHVCreateEngine(&params, &worker, &engine);
	CHECK(hr == S_OK && engine, "XHVCreateEngine -> 0x%08x", hr);
	if (hr != S_OK || !engine) {
		return;
	}
	XUID self = 0;
	XUserGetXUID(0, &self);
	CHECK(engine->RegisterLocalTalker(0) == S_OK, "RegisterLocalTalker(0)");
	CHECK(engine->RegisterRemoteTalker(self, nullptr, nullptr, nullptr) == S_OK, "RegisterRemoteTalker(self as remote)");
	CHECK(engine->StartLocalProcessingModes(0, modes, 1) == S_OK, "StartLocalProcessingModes(VOICECHAT)");
	INFO("headset present %d", (int)engine->IsHeadsetPresent(0));
	DWORD frames = 0;
	DWORD bytes = 0;
	DWORD batches = 0;
	DWORD errors = 0;
	bool talking = false;
	DWORD started = GetTickCount();
	while (GetTickCount() - started < 5000) {
		Pump(2);
		if (!(engine->GetDataReadyFlags() & 1)) {
			continue;
		}
		uint8_t buffer[4096];
		DWORD size = sizeof(buffer);
		DWORD packets = 0;
		if (engine->GetLocalChatData(0, buffer, &size, &packets) != S_OK || !size) {
			continue;
		}
		frames += packets;
		bytes += size;
		DWORD consumed = size;
		if (engine->SubmitIncomingChatData(self, buffer, &consumed) == S_OK && consumed == size) {
			batches++;
			talking = talking || engine->IsRemoteTalking(self);
		}
		else {
			errors++;
		}
	}
	INFO("voice: %u frame(s), %u bytes captured through Steam in 5 s, %u batch(es) decoded and played, %u error(s), remote talking seen %d", frames, bytes, batches, errors, (int)talking);
	if (!frames) {
		INFO("voice: nothing captured, Steam voice records from the device Steam has selected, so a microphone must be present and enabled in Steam");
	}
	CHECK(errors == 0, "every captured batch decoded");
	engine->StopLocalProcessingModes(0, modes, 1);
	engine->UnregisterRemoteTalker(self);
	engine->UnregisterLocalTalker(0);
	engine->Release();
	if (worker) {
		CloseHandle(worker);
	}
}

// The probe leaves nothing behind in the user's cloud for the app.
static void ProbeCleanup()
{
	if (SteamRemoteStorage()->IsCloudEnabledForApp()) {
		SteamRemoteStorage()->FileDelete("xlive/profile.bin");
		SteamRemoteStorage()->FileDelete("xlive/achievements.bin");
	}
}

int main(int argc, char** argv)
{
	const char* spaModule = nullptr;
	const char* querySpec = nullptr;
	bool filtersOnly = false;
	bool voiceOnly = false;
	int pairRole = 0;
	DWORD pairCode = 0;
	bool relayOnly = false;
	DWORD timeoutSeconds = 300;
	g_launchedBySteam = GetEnvironmentVariableW(L"SteamAppId", nullptr, 0) != 0 || GetEnvironmentVariableW(L"SteamGameId", nullptr, 0) != 0;
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--probe") == 0) {
			g_probe = true;
		}
		else if (strcmp(argv[i], "--spa") == 0 && i + 1 < argc) {
			spaModule = argv[++i];
		}
		else if (strcmp(argv[i], "--query") == 0 && i + 1 < argc) {
			querySpec = argv[++i];
		}
		else if (strcmp(argv[i], "--filters") == 0) {
			filtersOnly = true;
		}
		else if (strcmp(argv[i], "--voice") == 0) {
			voiceOnly = true;
		}
		else if ((strcmp(argv[i], "--pair-host") == 0 || strcmp(argv[i], "--pair-join") == 0) && i + 1 < argc) {
			pairRole = strcmp(argv[i], "--pair-host") == 0 ? 1 : 2;
			pairCode = (DWORD)strtoul(argv[++i], nullptr, 10);
		}
		else if (strcmp(argv[i], "--relay-only") == 0) {
			relayOnly = true;
		}
		else if (strcmp(argv[i], "--timeout") == 0 && i + 1 < argc) {
			timeoutSeconds = (DWORD)strtoul(argv[++i], nullptr, 10);
		}
	}

	printf("xlive-steamworks %s %s\n", g_probe ? "probe" : (pairRole ? "pair test" : "smoke test"), XlsVersion());
	XLIVE_INITIALIZE_INFO init = {};
	init.cbSize = sizeof(init);
	HRESULT hr = XLiveInitialize(&init);
	CHECK(hr == S_OK, "XLiveInitialize -> 0x%08x", hr);
	if (spaModule) {
		// A GFWL exe's SPA, so the achievement list below comes from a real title.
		HMODULE module = LoadLibraryExA(spaModule, nullptr, LOAD_LIBRARY_AS_DATAFILE | LOAD_LIBRARY_AS_IMAGE_RESOURCE);
		CHECK(module && XlsLoadSpaFromModule(module, 0), "XlsLoadSpaFromModule(%s)", spaModule);
	}
	HANDLE listener = XNotifyCreateListener(XNOTIFY_ALL);
	CHECK(listener != nullptr, "XNotifyCreateListener");
	Pump(20);

	if (XUserGetSigninState(0) != eXUserSigninState_SignedInToLive) {
		printf("[FAIL] Steam did not initialise for this app id, see xlive_steamworks.log (Steam not running, not logged in, or this account does not own the app).\n");
		XLiveUninitialize();
		return 1;
	}

	if (pairRole) {
		int result = RunPair(pairRole == 1, pairCode, relayOnly, timeoutSeconds);
		XCloseHandle(listener);
		XLiveUninitialize();
		return result;
	}
	if (filtersOnly || voiceOnly || querySpec) {
		XNetStartupParams params = {};
		params.cfgSizeOfStruct = sizeof(params);
		XNetStartup(&params);
		if (filtersOnly) {
			TestFilters();
		}
		if (querySpec) {
			TestQuery(querySpec);
		}
		if (voiceOnly) {
			TestVoice();
		}
		XCloseHandle(listener);
		XLiveUninitialize();
		printf("%d failure(s)\n", g_failures);
		return g_failures ? 1 : 0;
	}

	if (g_probe) {
		SteamSummary();
	}
	TestNotifications(listener);
	TestUser();
	TestAchievements();
	TestProfile();
	TestStorage();
	TestFriends();
	TestNetworkAndSession();
	if (g_probe) {
		ProbeCleanup();
	}

	XCloseHandle(listener);
	XLiveUninitialize();
	printf("%d failure(s)\n", g_failures);
	return g_failures ? 1 : 0;
}
