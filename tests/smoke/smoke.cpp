// Drives xlive.dll through the paths a title uses in its first minutes, against a live Steam
// client. Run with steam_appid.txt next to the exe, app 480 (Spacewar) is for testing only.
//
//   xlive_smoke.exe [--probe] [--spa Game.exe]
//
// --probe reports what the app on Steam offers and touches nothing that persists on the app:
// no leaderboard is created and no achievement is written.
#include "xlive/xfuncs.h"
#include "xlive/xlive_steamworks.h"

#include <steam/steam_api.h>

#include <stdio.h>
#include <string>
#include <vector>

static int g_failures = 0;
static bool g_probe = false;

#define CHECK(condition, ...) \
	do { \
		bool ok_ = (condition); \
		printf("%s ", ok_ ? "[ ok ]" : "[FAIL]"); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		if (!ok_) g_failures++; \
	} while (0)

#define INFO(...) \
	do { \
		printf("[info] "); \
		printf(__VA_ARGS__); \
		printf("\n"); \
	} while (0)

static void Pump(int frames)
{
	for (int i = 0; i < frames; i++) {
		XLiveRender();
		Sleep(10);
	}
}

static DWORD Wait(XOVERLAPPED* overlapped, DWORD timeoutMs = 20000)
{
	DWORD started = GetTickCount();
	while (!XHasOverlappedIoCompleted(overlapped)) {
		XLiveRender();
		Sleep(5);
		if (GetTickCount() - started > timeoutMs) {
			return ERROR_TIMEOUT;
		}
	}
	DWORD result = 0;
	XGetOverlappedResult(overlapped, &result, FALSE);
	return (DWORD)overlapped->InternalLow;
}

// Waits for a Steam call from the wrapper's pump since the test has no callback loop of its own.
template <typename T>
static bool WaitSteamCall(SteamAPICall_t call, T* result)
{
	bool failed = false;
	for (int i = 0; i < 1500; i++) {
		XLiveRender();
		if (SteamUtils()->IsAPICallCompleted(call, &failed)) {
			if (failed) {
				return false;
			}
			return SteamUtils()->GetAPICallResult(call, result, sizeof(T), T::k_iCallback, &failed) && !failed;
		}
		Sleep(10);
	}
	return false;
}

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
	INFO("overlay enabled %d, big picture %d, steam hardware %d", (int)SteamUtils()->IsOverlayEnabled(), (int)SteamUtils()->IsSteamInBigPictureMode(), (int)SteamUtils()->IsRunningOnSteamHardware());
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
		XUID xuid = 0;
		XUserGetXUID(0, &xuid);
		result = XSessionWriteStats(session, xuid, 1, &view, nullptr);
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
		if (result == ERROR_SUCCESS && stats->dwNumViews == 1 && stats->pViews[0].dwNumRows == 1) {
			XUSER_STATS_ROW& row = stats->pViews[0].pRows[0];
			printf("       rank %u rating %lld gamertag %s columns %u: %d, %lld\n", row.dwRank, row.i64Rating, row.szGamertag, row.dwNumColumns,
				row.dwNumColumns > 0 ? row.pColumns[0].Value.nData : 0, row.dwNumColumns > 1 ? row.pColumns[1].Value.i64Data : 0);
			CHECK(row.dwRank >= 1 && row.i64Rating >= 1234, "leaderboard LB_1 holds our score");
		}
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
	for (int i = 1; i < argc; i++) {
		if (strcmp(argv[i], "--probe") == 0) {
			g_probe = true;
		}
		else if (strcmp(argv[i], "--spa") == 0 && i + 1 < argc) {
			spaModule = argv[++i];
		}
	}

	printf("xlive-steamworks %s %s\n", g_probe ? "probe" : "smoke test", XlsVersion());
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
