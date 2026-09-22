// #5273, #5274, #5282, #5331, #5337, #5339: profile settings live in one per-user cloud file.
#include "xlive/xfuncs.h"
#include "api/xachievements.h"
#include "api/xlive.h"

#include "core/cloud.h"
#include "core/config.h"
#include "core/enumerator.h"
#include "core/image.h"
#include "core/log.h"
#include "core/notify.h"
#include "core/overlapped.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <map>
#include <memory>
#include <mutex>

namespace {

const char* kProfileFile = "xlive/profile.bin";
const uint32_t kProfileMagic = 0x50534C58; // 'XLSP'

std::mutex g_mutex;
bool g_profileLoaded = false;
std::map<DWORD, std::vector<uint8_t>> g_profile;

void LoadProfile()
{
	if (g_profileLoaded) {
		return;
	}
	g_profileLoaded = true;
	std::vector<uint8_t> data;
	if (!xls::CloudRead(kProfileFile, data) || data.size() < 8) {
		return;
	}
	uint32_t magic;
	memcpy(&magic, data.data(), 4);
	if (magic != kProfileMagic) {
		return;
	}
	size_t offset = 8;
	while (offset + 8 <= data.size()) {
		DWORD id;
		DWORD size;
		memcpy(&id, data.data() + offset, 4);
		memcpy(&size, data.data() + offset + 4, 4);
		offset += 8;
		if (offset + size > data.size()) {
			break;
		}
		g_profile[id].assign(data.begin() + offset, data.begin() + offset + size);
		offset += size;
	}
	XLS_LOG_DEBUG("profile: loaded %zu settings.", g_profile.size());
}

bool SaveProfile()
{
	std::vector<uint8_t> data;
	uint32_t magic = kProfileMagic;
	uint32_t version = 1;
	data.insert(data.end(), (uint8_t*)&magic, (uint8_t*)&magic + 4);
	data.insert(data.end(), (uint8_t*)&version, (uint8_t*)&version + 4);
	for (const auto& entry : g_profile) {
		DWORD id = entry.first;
		DWORD size = (DWORD)entry.second.size();
		data.insert(data.end(), (uint8_t*)&id, (uint8_t*)&id + 4);
		data.insert(data.end(), (uint8_t*)&size, (uint8_t*)&size + 4);
		data.insert(data.end(), entry.second.begin(), entry.second.end());
	}
	return xls::CloudWrite(kProfileFile, data.data(), data.size());
}

DWORD CountryFromSteam()
{
	// XONLINE_COUNTRY_* values for the regions Steam reports most, rest reads as United States.
	static const struct { const char* code; DWORD value; } table[] = {
		{ "AE", 1 }, { "AR", 4 }, { "AT", 5 }, { "AU", 6 }, { "BE", 8 }, { "BG", 9 }, { "BR", 13 }, { "CA", 16 },
		{ "CH", 18 }, { "CL", 19 }, { "CN", 20 }, { "CO", 21 }, { "CZ", 23 }, { "DE", 24 }, { "DK", 25 }, { "ES", 31 },
		{ "FI", 32 }, { "FR", 34 }, { "GB", 35 }, { "GR", 37 }, { "HK", 39 }, { "HR", 41 }, { "HU", 42 }, { "ID", 43 },
		{ "IE", 44 }, { "IL", 45 }, { "IN", 46 }, { "IT", 50 }, { "JP", 53 }, { "KR", 56 }, { "MX", 71 }, { "MY", 72 },
		{ "NL", 74 }, { "NO", 75 }, { "NZ", 76 }, { "PE", 79 }, { "PH", 80 }, { "PL", 82 }, { "PT", 84 }, { "RO", 87 },
		{ "RU", 88 }, { "SA", 89 }, { "SE", 90 }, { "SG", 91 }, { "SI", 92 }, { "SK", 93 }, { "TH", 97 }, { "TR", 99 },
		{ "TW", 101 }, { "UA", 102 }, { "US", 103 }, { "VN", 107 }, { "ZA", 109 },
	};
	if (xls::SteamReady() && xls::SteamUtils()) {
		const char* code = xls::SteamUtils()->GetIPCountry();
		for (const auto& entry : table) {
			if (code && _stricmp(code, entry.code) == 0) {
				return entry.value;
			}
		}
	}
	return 103;
}

void TitleCredSummary(DWORD* cred, DWORD* earned)
{
	*cred = 0;
	*earned = 0;
	if (!xls::spa::Loaded()) {
		return;
	}
	for (const xls::spa::Achievement& achievement : xls::spa::Achievements()) {
		if (xls::AchievementUnlocked(achievement.id, nullptr)) {
			*cred += achievement.cred;
			(*earned)++;
		}
	}
}

// Fills one setting for a user. Returns false when the buffer for variable data ran out.
bool FillSetting(XUSER_PROFILE_SETTING& out, DWORD settingId, XUID xuid, bool local, DWORD userIndex, xls::BufferPacker& packer)
{
	memset(&out, 0, sizeof(out));
	out.dwSettingId = settingId;
	if (local) {
		out.user.dwUserIndex = userIndex;
	}
	else {
		out.user.xuid = xuid;
	}
	uint8_t type = XUserGetProfileSettingType(settingId);
	out.data.type = type;
	out.source = XSOURCE_DEFAULT;

	auto stored = local ? g_profile.find(settingId) : g_profile.end();
	if (stored != g_profile.end()) {
		out.source = XSOURCE_TITLE;
		const std::vector<uint8_t>& raw = stored->second;
		switch (type) {
			case XUSER_DATA_TYPE_INT32: if (raw.size() >= 4) memcpy(&out.data.nData, raw.data(), 4); return true;
			case XUSER_DATA_TYPE_INT64: if (raw.size() >= 8) memcpy(&out.data.i64Data, raw.data(), 8); return true;
			case XUSER_DATA_TYPE_DOUBLE: if (raw.size() >= 8) memcpy(&out.data.dblData, raw.data(), 8); return true;
			case XUSER_DATA_TYPE_FLOAT: if (raw.size() >= 4) memcpy(&out.data.fData, raw.data(), 4); return true;
			case XUSER_DATA_TYPE_DATETIME: if (raw.size() >= 8) memcpy(&out.data.ftData, raw.data(), 8); return true;
			case XUSER_DATA_TYPE_UNICODE:
			case XUSER_DATA_TYPE_BINARY: {
				uint8_t* slot = (uint8_t*)packer.Back(raw.size());
				if (!slot) {
					return false;
				}
				memcpy(slot, raw.data(), raw.size());
				if (type == XUSER_DATA_TYPE_UNICODE) {
					out.data.string.cbData = (DWORD)raw.size();
					out.data.string.pwszData = (LPWSTR)slot;
				}
				else {
					out.data.binary.cbData = (DWORD)raw.size();
					out.data.binary.pbData = slot;
				}
				return true;
			}
			default:
				return true;
		}
	}

	switch (settingId) {
		case XPROFILE_GAMERCARD_ZONE: out.data.nData = XPROFILE_GAMERCARD_ZONE_RR; break;
		case XPROFILE_GAMERCARD_REGION: out.data.nData = (LONG)CountryFromSteam(); break;
		case XPROFILE_GAMERCARD_CRED:
		case XPROFILE_GAMERCARD_TITLE_CRED_EARNED: {
			DWORD cred, earned;
			TitleCredSummary(&cred, &earned);
			out.data.nData = (LONG)cred;
			break;
		}
		case XPROFILE_GAMERCARD_ACHIEVEMENTS_EARNED:
		case XPROFILE_GAMERCARD_TITLE_ACHIEVEMENTS_EARNED: {
			DWORD cred, earned;
			TitleCredSummary(&cred, &earned);
			out.data.nData = (LONG)earned;
			break;
		}
		case XPROFILE_GAMERCARD_REP: out.data.fData = 100.0f; break;
		case XPROFILE_GAMERCARD_TITLES_PLAYED: out.data.nData = 1; break;
		case XPROFILE_OPTION_CONTROLLER_VIBRATION: out.data.nData = 3; break;
		case XPROFILE_OPTION_VOICE_VOLUME: out.data.nData = 100; break;
		case XPROFILE_GAMERCARD_PICTURE_KEY: {
			wchar_t key[17];
			swprintf_s(key, L"%016llX", (unsigned long long)(local ? xls::UserXuid(userIndex) : xuid));
			wchar_t* slot = packer.BackString(key);
			if (!slot) {
				return false;
			}
			out.data.string.cbData = (DWORD)((wcslen(key) + 1) * sizeof(wchar_t));
			out.data.string.pwszData = slot;
			break;
		}
		case XPROFILE_GAMERCARD_MOTTO: {
			wchar_t* slot = packer.BackString(L"");
			if (!slot) {
				return false;
			}
			out.data.string.cbData = sizeof(wchar_t);
			out.data.string.pwszData = slot;
			break;
		}
		case XPROFILE_TITLE_SPECIFIC1:
		case XPROFILE_TITLE_SPECIFIC2:
		case XPROFILE_TITLE_SPECIFIC3:
			out.source = XSOURCE_NO_VALUE;
			out.data.binary.cbData = 0;
			out.data.binary.pbData = nullptr;
			break;
		default:
			if (type == XUSER_DATA_TYPE_UNICODE) {
				wchar_t* slot = packer.BackString(L"");
				if (!slot) {
					return false;
				}
				out.data.string.cbData = sizeof(wchar_t);
				out.data.string.pwszData = slot;
			}
			else if (type == XUSER_DATA_TYPE_BINARY) {
				out.source = XSOURCE_NO_VALUE;
			}
			break;
	}
	return true;
}

DWORD ReadSettings(DWORD userIndex, DWORD xuidCount, const XUID* xuids, DWORD settingCount, const DWORD* settingIds, DWORD* resultSize, XUSER_READ_PROFILE_SETTING_RESULT* results, XOVERLAPPED* overlapped)
{
	DWORD required = sizeof(XUSER_READ_PROFILE_SETTING_RESULT) + xuidCount * settingCount * sizeof(XUSER_PROFILE_SETTING);
	for (DWORD i = 0; i < settingCount; i++) {
		uint8_t type = XUserGetProfileSettingType(settingIds[i]);
		if (type == XUSER_DATA_TYPE_UNICODE || type == XUSER_DATA_TYPE_BINARY) {
			required += xuidCount * (XUserGetProfileSettingMaxSize(settingIds[i]) + sizeof(wchar_t));
		}
	}
	if (!results || *resultSize < required) {
		*resultSize = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	LoadProfile();
	xls::BufferPacker packer(results, *resultSize);
	packer.Front(sizeof(XUSER_READ_PROFILE_SETTING_RESULT));
	results->dwSettingsLen = xuidCount * settingCount;
	results->pSettings = (XUSER_PROFILE_SETTING*)packer.Front(results->dwSettingsLen * sizeof(XUSER_PROFILE_SETTING));
	if (!results->pSettings) {
		return ERROR_INSUFFICIENT_BUFFER;
	}
	DWORD localXuidIndex = xls::UserXuid(userIndex) ? 0 : XUSER_INDEX_NONE;
	DWORD written = 0;
	for (DWORD u = 0; u < xuidCount; u++) {
		bool local = xuids[u] == xls::UserXuid(userIndex) || xuids[u] == INVALID_XUID;
		for (DWORD s = 0; s < settingCount; s++) {
			if (!FillSetting(results->pSettings[written], settingIds[s], xuids[u], local, userIndex, packer)) {
				return ERROR_INSUFFICIENT_BUFFER;
			}
			written++;
		}
	}
	(void)localXuidIndex;
	return xls::OverlappedReturn(overlapped, ERROR_SUCCESS);
}

// Avatar reads wait for Steam to load the image. The poll runs from the async pump.
struct AvatarRead {
	CSteamID steamId;
	bool smallPicture;
	uint8_t* texture;
	DWORD pitch;
	DWORD height;
	bool requested = false;
};

bool TryReadAvatar(AvatarRead& read)
{
	int handle = read.smallPicture ? xls::SteamFriends()->GetSmallFriendAvatar(read.steamId) : xls::SteamFriends()->GetMediumFriendAvatar(read.steamId);
	if (handle == -1) {
		return false;
	}
	if (handle == 0) {
		if (!read.requested) {
			read.requested = true;
			if (xls::SteamFriends()->RequestUserInformation(read.steamId, false)) {
				return false;
			}
		}
		xls::FillTexture(read.texture, read.pitch, read.height, 0xFF606060);
		return true;
	}
	xls::RgbaImage image;
	uint32 width = 0;
	uint32 height = 0;
	if (!xls::SteamUtils()->GetImageSize(handle, &width, &height)) {
		xls::FillTexture(read.texture, read.pitch, read.height, 0xFF606060);
		return true;
	}
	image.width = width;
	image.height = height;
	image.pixels.resize((size_t)width * height * 4);
	if (xls::SteamUtils()->GetImageRGBA(handle, image.pixels.data(), (int)image.pixels.size())) {
		xls::BlitToTexture(image, read.texture, read.pitch, read.height);
	}
	else {
		xls::FillTexture(read.texture, read.pitch, read.height, 0xFF606060);
	}
	return true;
}

DWORD ReadAvatar(CSteamID steamId, BOOL smallPicture, uint8_t* texture, DWORD pitch, DWORD height, XOVERLAPPED* overlapped)
{
	if (!texture || !pitch || !height) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!xls::SteamReady() || !steamId.IsValid()) {
		xls::FillTexture(texture, pitch, height, 0xFF606060);
		return xls::OverlappedReturn(overlapped, ERROR_SUCCESS);
	}
	auto read = std::make_shared<AvatarRead>();
	read->steamId = steamId;
	read->smallPicture = smallPicture != FALSE;
	read->texture = texture;
	read->pitch = pitch;
	read->height = height;
	return xls::RunAsync(overlapped, [read](XOVERLAPPED* pending) {
		if (!TryReadAvatar(*read)) {
			return false;
		}
		xls::OverlappedComplete(pending, ERROR_SUCCESS);
		return true;
	});
}

}

namespace xls {
namespace events {

void OnAvatarLoaded(const AvatarImageLoaded_t& loaded)
{
	XLS_LOG_DEBUG("profile: avatar of %llu loaded (%dx%d).", loaded.m_steamID.ConvertToUint64(), loaded.m_iWide, loaded.m_iTall);
}

}
}

// #5331
DWORD WINAPI XUserReadProfileSettings(DWORD dwTitleId, DWORD dwUserIndex, DWORD dwNumSettingIds, const DWORD* pdwSettingIds, DWORD* pcbResults, XUSER_READ_PROFILE_SETTING_RESULT* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!dwNumSettingIds || dwNumSettingIds > 32 || !pdwSettingIds || !pcbResults) {
		return ERROR_INVALID_PARAMETER;
	}
	XUID xuid = xls::UserXuid(dwUserIndex);
	return ReadSettings(dwUserIndex, 1, &xuid, dwNumSettingIds, pdwSettingIds, pcbResults, pResults, pOverlapped);
}

// #5339
DWORD WINAPI XUserReadProfileSettingsByXuid(DWORD dwTitleId, DWORD dwUserIndexRequester, DWORD dwNumFor, const XUID* pxuidFor, DWORD dwNumSettingIds, const DWORD* pdwSettingIds, DWORD* pcbResults, XUSER_READ_PROFILE_SETTING_RESULT* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndexRequester)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndexRequester)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!dwNumFor || dwNumFor > 32 || !pxuidFor || !dwNumSettingIds || dwNumSettingIds > 32 || !pdwSettingIds || !pcbResults) {
		return ERROR_INVALID_PARAMETER;
	}
	return ReadSettings(dwUserIndexRequester, dwNumFor, pxuidFor, dwNumSettingIds, pdwSettingIds, pcbResults, pResults, pOverlapped);
}

// #5337
DWORD WINAPI XUserWriteProfileSettings(DWORD dwUserIndex, DWORD dwNumSettings, const XUSER_PROFILE_SETTING* pSettings, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!dwNumSettings || dwNumSettings > 32 || !pSettings) {
		return ERROR_INVALID_PARAMETER;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	LoadProfile();
	for (DWORD i = 0; i < dwNumSettings; i++) {
		const XUSER_PROFILE_SETTING& setting = pSettings[i];
		DWORD maxSize = XUserGetProfileSettingMaxSize(setting.dwSettingId);
		std::vector<uint8_t>& raw = g_profile[setting.dwSettingId];
		switch (setting.data.type) {
			case XUSER_DATA_TYPE_INT32: raw.assign((const uint8_t*)&setting.data.nData, (const uint8_t*)&setting.data.nData + 4); break;
			case XUSER_DATA_TYPE_INT64: raw.assign((const uint8_t*)&setting.data.i64Data, (const uint8_t*)&setting.data.i64Data + 8); break;
			case XUSER_DATA_TYPE_DOUBLE: raw.assign((const uint8_t*)&setting.data.dblData, (const uint8_t*)&setting.data.dblData + 8); break;
			case XUSER_DATA_TYPE_FLOAT: raw.assign((const uint8_t*)&setting.data.fData, (const uint8_t*)&setting.data.fData + 4); break;
			case XUSER_DATA_TYPE_DATETIME: raw.assign((const uint8_t*)&setting.data.ftData, (const uint8_t*)&setting.data.ftData + 8); break;
			case XUSER_DATA_TYPE_UNICODE: {
				DWORD size = setting.data.string.cbData > maxSize ? maxSize : setting.data.string.cbData;
				raw.assign((const uint8_t*)setting.data.string.pwszData, (const uint8_t*)setting.data.string.pwszData + size);
				break;
			}
			case XUSER_DATA_TYPE_BINARY: {
				DWORD size = setting.data.binary.cbData > maxSize ? maxSize : setting.data.binary.cbData;
				raw.assign(setting.data.binary.pbData, setting.data.binary.pbData + size);
				break;
			}
			default:
				g_profile.erase(setting.dwSettingId);
				break;
		}
	}
	bool saved = SaveProfile();
	xls::NotifyPost(XN_SYS_PROFILESETTINGCHANGED, 1u << dwUserIndex);
	return xls::OverlappedReturn(pOverlapped, saved ? ERROR_SUCCESS : ERROR_WRITE_FAULT);
}

// #5282
DWORD WINAPI XUserReadGamerPicture(DWORD dwUserIndex, BOOL fSmall, uint8_t* pbTextureBuffer, DWORD dwPitch, DWORD dwHeight, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	return ReadAvatar(dwUserIndex == 0 ? xls::SteamLocalId() : k_steamIDNil, fSmall, pbTextureBuffer, dwPitch, dwHeight, pOverlapped);
}

// #5273
DWORD WINAPI XUserReadGamerPictureByKey(const XUSER_DATA* pPictureKey, BOOL fSmall, uint8_t* pbTextureBuffer, DWORD dwPitch, DWORD dwHeight, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!pPictureKey) {
		return ERROR_INVALID_PARAMETER;
	}
	// The key is the XUID as hex text, handed out in XPROFILE_GAMERCARD_PICTURE_KEY. Some titles pass the XUID itself.
	CSteamID steamId = k_steamIDNil;
	if (pPictureKey->type == XUSER_DATA_TYPE_UNICODE && pPictureKey->string.pwszData) {
		XUID xuid = _wcstoui64(pPictureKey->string.pwszData, nullptr, 16);
		steamId = xls::SteamIdFromXuid(xuid);
	}
	else if (pPictureKey->type == XUSER_DATA_TYPE_INT64) {
		steamId = xls::SteamIdFromXuid((XUID)pPictureKey->i64Data);
	}
	return ReadAvatar(steamId, fSmall, pbTextureBuffer, dwPitch, dwHeight, pOverlapped);
}

// #5274
DWORD WINAPI XUserAwardGamerPicture(DWORD dwUserIndex, DWORD dwPictureId, DWORD dwReserved, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}
