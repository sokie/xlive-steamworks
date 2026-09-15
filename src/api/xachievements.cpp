// #5278, #5279, #5280 Achievements: text and art come from the SPA, the unlock state from Steam.
#include "xlive/xfuncs.h"
#include "api/xlive.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/image.h"
#include "core/log.h"
#include "core/overlapped.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <mutex>
#include <set>

namespace {

std::mutex g_mutex;
std::set<uint64_t> g_statsReceived;

struct AchievementRecord {
	XACHIEVEMENT_DETAILS details;
	std::wstring label;
	std::wstring description;
	std::wstring unachieved;
};

// Builds the full list for a user, in SPA order, or in Steam order when the exe has no SPA.
std::vector<AchievementRecord> BuildList(CSteamID user, bool local)
{
	std::vector<AchievementRecord> records;
	ISteamUserStats* stats = xls::SteamReady() ? xls::SteamUserStats() : nullptr;

	auto unlockState = [&](const std::string& apiName, bool* achieved, uint32* unlockTime) {
		*achieved = false;
		*unlockTime = 0;
		if (!stats) {
			return;
		}
		if (local) {
			stats->GetAchievementAndUnlockTime(apiName.c_str(), achieved, unlockTime);
		}
		else {
			stats->GetUserAchievementAndUnlockTime(user, apiName.c_str(), achieved, unlockTime);
		}
	};

	if (xls::spa::Loaded() && !xls::spa::Achievements().empty()) {
		for (const xls::spa::Achievement& spaAchievement : xls::spa::Achievements()) {
			AchievementRecord record = {};
			record.details.dwId = spaAchievement.id;
			record.details.dwImageId = spaAchievement.imageId;
			record.details.dwCred = spaAchievement.cred;
			record.details.dwFlags = spaAchievement.flags & (XACHIEVEMENT_DETAILS_MASK_TYPE | XACHIEVEMENT_DETAILS_SHOWUNACHIEVED);
			record.label = xls::spa::String(spaAchievement.labelId);
			record.description = xls::spa::String(spaAchievement.descriptionId);
			record.unachieved = xls::spa::String(spaAchievement.unachievedId);
			bool achieved = false;
			uint32 unlockTime = 0;
			unlockState(xls::AchievementApiName(spaAchievement.id), &achieved, &unlockTime);
			if (achieved) {
				record.details.dwFlags |= XACHIEVEMENT_DETAILS_ACHIEVED | XACHIEVEMENT_DETAILS_ACHIEVED_ONLINE;
				record.details.ftAchieved = xls::UnixTimeToFileTime(unlockTime);
			}
			records.push_back(std::move(record));
		}
		return records;
	}

	if (!stats || !xls::Cfg().achievementsFromSteamWhenNoSpa) {
		return records;
	}
	// No SPA: Steam's own definitions, numbered in Steam order from 1.
	uint32 count = stats->GetNumAchievements();
	for (uint32 i = 0; i < count; i++) {
		const char* apiName = stats->GetAchievementName(i);
		if (!apiName) {
			continue;
		}
		AchievementRecord record = {};
		record.details.dwId = i + 1;
		record.details.dwImageId = i + 1;
		record.details.dwCred = 0;
		const char* hidden = stats->GetAchievementDisplayAttribute(apiName, "hidden");
		record.details.dwFlags = XACHIEVEMENT_TYPE_COMPLETION | ((hidden && hidden[0] == '1') ? 0 : XACHIEVEMENT_DETAILS_SHOWUNACHIEVED);
		record.label = xls::Utf8ToWide(stats->GetAchievementDisplayAttribute(apiName, "name"));
		record.description = xls::Utf8ToWide(stats->GetAchievementDisplayAttribute(apiName, "desc"));
		record.unachieved = record.description;
		bool achieved = false;
		uint32 unlockTime = 0;
		unlockState(apiName, &achieved, &unlockTime);
		if (achieved) {
			record.details.dwFlags |= XACHIEVEMENT_DETAILS_ACHIEVED | XACHIEVEMENT_DETAILS_ACHIEVED_ONLINE;
			record.details.ftAchieved = xls::UnixTimeToFileTime(unlockTime);
		}
		records.push_back(std::move(record));
	}
	return records;
}

class AchievementEnumerator : public xls::Enumerator {
public:
	CSteamID user;
	bool local = true;
	bool built = false;
	DWORD detailFlags = 0;
	DWORD startingIndex = 0;
	DWORD batchSize = 0;
	size_t index = 0;
	std::vector<AchievementRecord> records;

	bool Ready() override
	{
		if (local) {
			return true;
		}
		std::lock_guard<std::mutex> lock(g_mutex);
		return g_statsReceived.count(user.ConvertToUint64()) > 0;
	}

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		if (!built) {
			records = BuildList(user, local);
			index = startingIndex < records.size() ? startingIndex : records.size();
			built = true;
		}
		*itemsReturned = 0;
		if (index >= records.size()) {
			return ERROR_NO_MORE_FILES;
		}
		xls::BufferPacker packer(buffer, bufferSize);
		DWORD count = 0;
		while (index < records.size() && count < batchSize) {
			const AchievementRecord& record = records[index];
			uint8_t* front = packer.FrontPointer();
			uint8_t* back = packer.BackPointer();
			XACHIEVEMENT_DETAILS* out = (XACHIEVEMENT_DETAILS*)packer.Front(sizeof(XACHIEVEMENT_DETAILS));
			if (!out) {
				break;
			}
			*out = record.details;
			out->pwszLabel = nullptr;
			out->pwszDescription = nullptr;
			out->pwszUnachieved = nullptr;
			bool fits = true;
			if (detailFlags & XACHIEVEMENT_DETAILS_LABEL) {
				out->pwszLabel = packer.BackString(record.label.c_str());
				fits = fits && out->pwszLabel;
			}
			if (fits && (detailFlags & XACHIEVEMENT_DETAILS_DESCRIPTION)) {
				out->pwszDescription = packer.BackString(record.description.c_str());
				fits = fits && out->pwszDescription;
			}
			if (fits && (detailFlags & XACHIEVEMENT_DETAILS_UNACHIEVED)) {
				out->pwszUnachieved = packer.BackString(record.unachieved.c_str());
				fits = fits && out->pwszUnachieved;
			}
			if (!fits) {
				packer.Restore(front, back);
				break;
			}
			count++;
			index++;
		}
		*itemsReturned = count;
		return count ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
	}
};

}

namespace xls {
namespace events {

void OnUserStatsReceived(const UserStatsReceived_t& received)
{
	if (received.m_nGameID != SteamAppId()) {
		return;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	g_statsReceived.insert(received.m_steamIDUser.ConvertToUint64());
	XLS_LOG_DEBUG("achievements: stats for %llu received (EResult %d).", received.m_steamIDUser.ConvertToUint64(), (int)received.m_eResult);
}

}
}

// #5278
DWORD WINAPI XUserWriteAchievements(DWORD dwNumAchievements, const XUSER_ACHIEVEMENT* pAchievements, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwNumAchievements || !pAchievements) {
		return ERROR_INVALID_PARAMETER;
	}
	for (DWORD i = 0; i < dwNumAchievements; i++) {
		if (!xls::UserIndexValid(pAchievements[i].dwUserIndex)) {
			return ERROR_NO_SUCH_USER;
		}
	}
	if (!xls::SteamReady()) {
		return xls::OverlappedReturn(pOverlapped, ERROR_NOT_LOGGED_ON);
	}
	bool anySet = false;
	for (DWORD i = 0; i < dwNumAchievements; i++) {
		if (pAchievements[i].dwUserIndex != 0) {
			// Only the Steam user has an account so extra local players earn nothing.
			continue;
		}
		std::string apiName = xls::AchievementApiName(pAchievements[i].dwAchievementId);
		bool achieved = false;
		xls::SteamUserStats()->GetAchievement(apiName.c_str(), &achieved);
		if (achieved) {
			XLS_LOG_DEBUG("achievements: %u (%s) is already unlocked.", pAchievements[i].dwAchievementId, apiName.c_str());
			continue;
		}
		if (xls::SteamUserStats()->SetAchievement(apiName.c_str())) {
			XLS_LOG_INFO("achievements: unlocked %u as %s.", pAchievements[i].dwAchievementId, apiName.c_str());
			anySet = true;
		}
		else {
			XLS_LOG_ERROR("achievements: SetAchievement(%s) failed for id %u, check that the API name exists on Steamworks.", apiName.c_str(), pAchievements[i].dwAchievementId);
		}
	}
	if (anySet) {
		xls::SteamUserStats()->StoreStats();
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5280
DWORD WINAPI XUserCreateAchievementEnumerator(DWORD dwTitleId, DWORD dwUserIndex, XUID xuid, DWORD dwDetailFlags, DWORD dwStartingIndex, DWORD cItem, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cItem || cItem > XACHIEVEMENT_MAX_COUNT || dwStartingIndex >= XACHIEVEMENT_MAX_COUNT || !dwDetailFlags || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	if (dwTitleId && xls::TitleId() && dwTitleId != xls::TitleId()) {
		XLS_LOG_WARN("XUserCreateAchievementEnumerator: title 0x%08x is not this title, the list is empty.", dwTitleId);
	}
	auto enumerator = std::make_unique<AchievementEnumerator>();
	enumerator->detailFlags = dwDetailFlags;
	enumerator->startingIndex = dwStartingIndex;
	enumerator->batchSize = cItem;
	if (xuid == INVALID_XUID || xuid == xls::UserXuid(dwUserIndex)) {
		if (!xls::UserSignedIn(dwUserIndex)) {
			return ERROR_NOT_LOGGED_ON;
		}
		enumerator->local = true;
		enumerator->user = xls::SteamLocalId();
	}
	else {
		CSteamID steamId = xls::SteamIdFromXuid(xuid);
		if (!steamId.IsValid()) {
			return ERROR_INVALID_PARAMETER;
		}
		enumerator->local = steamId == xls::SteamLocalId();
		enumerator->user = steamId;
		if (!enumerator->local && xls::SteamReady()) {
			bool known;
			{
				std::lock_guard<std::mutex> lock(g_mutex);
				known = g_statsReceived.count(steamId.ConvertToUint64()) > 0;
			}
			if (!known) {
				xls::SteamUserStats()->RequestUserStats(steamId);
			}
		}
	}
	if (dwTitleId && xls::TitleId() && dwTitleId != xls::TitleId()) {
		enumerator->built = true;
	}
	*pcbBuffer = cItem * XACHIEVEMENT_SIZE_FULL;
	enumerator->requiredBufferSize = *pcbBuffer;
	*phEnum = xls::EnumeratorRegister(std::move(enumerator));
	return *phEnum ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

// #5279
DWORD WINAPI XUserReadAchievementPicture(DWORD dwUserIndex, DWORD dwTitleId, DWORD dwImageId, uint8_t* pbTextureBuffer, DWORD dwPitch, DWORD dwHeight, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pbTextureBuffer || !dwPitch || !dwHeight) {
		return ERROR_INVALID_PARAMETER;
	}
	const uint8_t* png = nullptr;
	size_t pngSize = 0;
	xls::RgbaImage image;
	bool decoded = false;
	if (xls::spa::Image(dwImageId, &png, &pngSize)) {
		decoded = xls::DecodePng(png, pngSize, image);
	}
	if (!decoded && xls::SteamReady()) {
		// Without SPA art, the Steam icon of the achievement with this id (Steam order) is used.
		for (const xls::spa::Achievement& achievement : xls::spa::Achievements()) {
			if (achievement.imageId == dwImageId) {
				int icon = xls::SteamUserStats()->GetAchievementIcon(xls::AchievementApiName(achievement.id).c_str());
				uint32 width = 0;
				uint32 height = 0;
				if (icon > 0 && xls::SteamUtils()->GetImageSize(icon, &width, &height)) {
					image.width = width;
					image.height = height;
					image.pixels.resize((size_t)width * height * 4);
					decoded = xls::SteamUtils()->GetImageRGBA(icon, image.pixels.data(), (int)image.pixels.size());
				}
				break;
			}
		}
	}
	if (decoded) {
		xls::BlitToTexture(image, pbTextureBuffer, dwPitch, dwHeight);
	}
	else {
		xls::FillTexture(pbTextureBuffer, dwPitch, dwHeight, 0xFF404040);
		XLS_LOG_WARN("XUserReadAchievementPicture: no image %u.", dwImageId);
	}
	return xls::OverlappedReturn(pOverlapped, decoded ? ERROR_SUCCESS : ERROR_FILE_NOT_FOUND);
}
