// #5350 - #5363, #5367, #5370 - #5376: a GFWL content package is a Steam DLC app.
#include "xlive/xfuncs.h"
#include "api/xlive.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/notify.h"
#include "core/overlapped.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <mutex>

namespace {

struct Content {
	xls::DlcMapping mapping;
	bool installed;
	bool owned;
};

std::mutex g_mutex;
std::vector<Content> g_content;
bool g_scanned = false;

void Scan()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_content.clear();
	const xls::Config& config = xls::Cfg();
	for (const xls::DlcMapping& mapping : config.dlc) {
		Content content;
		content.mapping = mapping;
		if (content.mapping.path.empty()) {
			content.mapping.path = config.dlcRootDir + xls::FormatW(L"%u\\", mapping.appId);
		}
		content.installed = xls::SteamReady() && mapping.appId ? xls::SteamApps()->BIsDlcInstalled(mapping.appId) : true;
		content.owned = xls::SteamReady() && mapping.appId ? xls::SteamApps()->BIsSubscribedApp(mapping.appId) : true;
		g_content.push_back(content);
	}
	if (config.dlcAutoDiscover && xls::SteamReady()) {
		int count = xls::SteamApps()->GetDLCCount();
		for (int i = 0; i < count; i++) {
			AppId_t appId = 0;
			bool available = false;
			char name[128] = {};
			if (!xls::SteamApps()->BGetDLCDataByIndex(i, &appId, &available, name, sizeof(name))) {
				continue;
			}
			bool known = false;
			for (const Content& existing : g_content) {
				if (existing.mapping.appId == appId) {
					known = true;
					break;
				}
			}
			if (known) {
				continue;
			}
			Content content;
			content.mapping.appId = appId;
			uint64_t hash = xls::Fnv1a64(&appId, sizeof(appId));
			memcpy(content.mapping.contentId, &hash, sizeof(hash));
			uint32_t hash32 = xls::Fnv1a32(&hash, sizeof(hash));
			memcpy(content.mapping.contentId + 8, &hash32, sizeof(hash32));
			memcpy(content.mapping.contentId + 12, &appId, sizeof(appId));
			content.mapping.contentType = XCONTENTTYPE_MARKETPLACE;
			content.mapping.offerId = appId;
			content.mapping.displayName = xls::Utf8ToWide(name);
			content.mapping.path = config.dlcRootDir + xls::FormatW(L"%u\\", appId);
			content.installed = xls::SteamApps()->BIsDlcInstalled(appId);
			content.owned = xls::SteamApps()->BIsSubscribedApp(appId);
			g_content.push_back(content);
		}
	}
	g_scanned = true;
	XLS_LOG_INFO("content: %zu content package(s) known.", g_content.size());
	for (const Content& content : g_content) {
		XLS_LOG_DEBUG("content: app %u \"%ls\" installed %d owned %d path %ls.", content.mapping.appId, content.mapping.displayName.c_str(), content.installed, content.owned, content.mapping.path.c_str());
	}
}

void EnsureScanned()
{
	if (!g_scanned) {
		Scan();
	}
}

bool FindContent(const XLIVE_CONTENT_INFO* info, Content* out)
{
	EnsureScanned();
	std::lock_guard<std::mutex> lock(g_mutex);
	for (const Content& content : g_content) {
		if (memcmp(content.mapping.contentId, info->abContentID, XLIVE_CONTENT_ID_SIZE) == 0) {
			*out = content;
			return true;
		}
	}
	return false;
}

void FillInfo(const Content& content, XLIVE_CONTENT_INFO* info)
{
	info->dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
	info->dwTitleID = xls::TitleId();
	info->dwContentType = content.mapping.contentType;
	memcpy(info->abContentID, content.mapping.contentId, XLIVE_CONTENT_ID_SIZE);
}

class ContentEnumerator : public xls::Enumerator {
public:
	std::vector<XLIVE_CONTENT_INFO> items;
	size_t index = 0;
	DWORD batchSize = 0;

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		*itemsReturned = 0;
		if (index >= items.size()) {
			return ERROR_NO_MORE_FILES;
		}
		XLIVE_CONTENT_INFO* out = (XLIVE_CONTENT_INFO*)buffer;
		DWORD count = 0;
		while (index < items.size() && count < batchSize && (count + 1) * sizeof(XLIVE_CONTENT_INFO) <= bufferSize) {
			out[count++] = items[index++];
		}
		*itemsReturned = count;
		return count ? ERROR_SUCCESS : ERROR_INSUFFICIENT_BUFFER;
	}
};

struct OfferRecord {
	XMARKETPLACE_CONTENTOFFER_INFO info;
	std::wstring name;
	std::wstring title;
	std::wstring sellText;
};

class OfferEnumerator : public xls::Enumerator {
public:
	std::vector<OfferRecord> offers;
	size_t index = 0;
	DWORD batchSize = 0;

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		*itemsReturned = 0;
		if (index >= offers.size()) {
			return ERROR_NO_MORE_FILES;
		}
		xls::BufferPacker packer(buffer, bufferSize);
		DWORD count = 0;
		while (index < offers.size() && count < batchSize) {
			OfferRecord& offer = offers[index];
			uint8_t* front = packer.FrontPointer();
			uint8_t* back = packer.BackPointer();
			XMARKETPLACE_CONTENTOFFER_INFO* out = (XMARKETPLACE_CONTENTOFFER_INFO*)packer.Front(sizeof(XMARKETPLACE_CONTENTOFFER_INFO));
			if (!out) {
				break;
			}
			*out = offer.info;
			out->wszOfferName = packer.BackString(offer.name.c_str());
			out->wszTitleName = packer.BackString(offer.title.c_str());
			out->wszSellText = packer.BackString(offer.sellText.c_str());
			if (!out->wszOfferName || !out->wszTitleName || !out->wszSellText) {
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

class AssetEnumerator : public xls::Enumerator {
public:
	bool returned = false;

	DWORD Next(void* buffer, DWORD bufferSize, DWORD* itemsReturned) override
	{
		*itemsReturned = 0;
		if (returned) {
			return ERROR_NO_MORE_FILES;
		}
		if (bufferSize < sizeof(XMARKETPLACE_ASSET_ENUMERATE_REPLY)) {
			return ERROR_INSUFFICIENT_BUFFER;
		}
		// Store assets have no Steam counterpart
		XMARKETPLACE_ASSET_ENUMERATE_REPLY* reply = (XMARKETPLACE_ASSET_ENUMERATE_REPLY*)buffer;
		memset(reply, 0, sizeof(*reply));
		reply->assetPackage.ftEnumerate = xls::NowFileTime();
		returned = true;
		*itemsReturned = 1;
		return ERROR_SUCCESS;
	}
};

OfferRecord OfferFor(const Content& content)
{
	OfferRecord offer = {};
	offer.info.qwOfferID = content.mapping.offerId ? content.mapping.offerId : content.mapping.appId;
	offer.info.dwOfferType = XMARKETPLACE_OFFERING_TYPE_CONTENT;
	memcpy(offer.info.contentId, content.mapping.contentId, XMARKETPLACE_CONTENT_ID_LEN);
	offer.info.fIsUnrestrictedLicense = TRUE;
	offer.info.dwLicenseMask = content.mapping.licenseMask;
	offer.info.dwTitleID = xls::TitleId();
	offer.info.fUserHasPurchased = content.owned ? TRUE : FALSE;
	offer.name = content.mapping.displayName;
	offer.title = xls::spa::Loaded() ? xls::spa::TitleName() : std::wstring();
	offer.sellText = content.mapping.displayName;
	offer.info.dwOfferNameLength = (DWORD)offer.name.size();
	offer.info.dwTitleNameLength = (DWORD)offer.title.size();
	offer.info.dwSellTextLength = (DWORD)offer.sellText.size();
	return offer;
}

}

namespace xls {
namespace events {

void OnDlcInstalled(AppId_t appId)
{
	XLS_LOG_INFO("content: DLC %u installed.", appId);
	Scan();
	NotifyPost(XN_LIVE_CONTENT_INSTALLED, 0);
}

}
}

// #5360
DWORD WINAPI XLiveContentCreateEnumerator(DWORD cItems, const XLIVE_CONTENT_RETRIEVAL_INFO* pRetrievalInfo, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!cItems || !pRetrievalInfo || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	if (pRetrievalInfo->dwContentAPIVersion != XLIVE_CONTENT_API_VERSION) {
		return ERROR_INVALID_PARAMETER;
	}
	EnsureScanned();
	auto enumerator = std::make_unique<ContentEnumerator>();
	enumerator->batchSize = cItems;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (const Content& content : g_content) {
			if (!content.installed || !content.owned) {
				continue;
			}
			if (pRetrievalInfo->dwContentType && content.mapping.contentType != pRetrievalInfo->dwContentType) {
				continue;
			}
			if (pRetrievalInfo->dwTitleID && xls::TitleId() && pRetrievalInfo->dwTitleID != xls::TitleId()) {
				continue;
			}
			XLIVE_CONTENT_INFO info;
			FillInfo(content, &info);
			enumerator->items.push_back(info);
		}
	}
	*pcbBuffer = cItems * sizeof(XLIVE_CONTENT_INFO);
	enumerator->requiredBufferSize = *pcbBuffer;
	*phEnum = xls::EnumeratorRegister(std::move(enumerator));
	return *phEnum ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

// #5350
HRESULT WINAPI XLiveContentCreateAccessHandle(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, DWORD dwLicenseInfoVersion, XLIVE_PROTECTED_BUFFER* pLicenseInfo, DWORD dwOffset, HANDLE* phContentAccess, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return HRESULT_FROM_WIN32(ERROR_NO_SUCH_USER);
	}
	if (!pContentInfo || !phContentAccess) {
		return E_POINTER;
	}
	Content content;
	if (!FindContent(pContentInfo, &content) || !content.owned) {
		*phContentAccess = nullptr;
		DWORD result = xls::OverlappedReturn(pOverlapped, ERROR_NOT_FOUND);
		return result == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	// The handle only has to be closable since the content is already on disk.
	*phContentAccess = CreateEventW(nullptr, TRUE, TRUE, nullptr);
	if (pLicenseInfo && dwLicenseInfoVersion == XLIVE_LICENSE_INFO_VERSION && dwOffset + sizeof(XLIVE_LICENSE_INFO) <= pLicenseInfo->dwSize) {
		XLIVE_LICENSE_INFO license = {};
		license.dwContentAPIVersion = XLIVE_CONTENT_API_VERSION;
		memcpy(license.abContentID, content.mapping.contentId, XLIVE_LICENSE_ID_SIZE);
		license.dwLicenseMask = content.mapping.licenseMask;
		memcpy(&pLicenseInfo->bData + dwOffset, &license, sizeof(license));
	}
	DWORD result = xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
	return result == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : S_OK;
}

// #5351
HRESULT WINAPI XLiveContentInstallPackage(const XLIVE_CONTENT_INFO* pContentInfo, LPCWSTR lpszCabFile, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams)
{
	XLS_TRACE_FN();
	// Steam installs DLC so a title-driven install has nothing to do.
	return S_OK;
}

// #5352
HRESULT WINAPI XLiveContentUninstall(const XLIVE_CONTENT_INFO* pContentInfo, const XUID* pXuid, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams)
{
	XLS_TRACE_FN();
	Content content;
	if (pContentInfo && FindContent(pContentInfo, &content) && content.mapping.appId && xls::SteamReady()) {
		xls::SteamApps()->UninstallDLC(content.mapping.appId);
	}
	return S_OK;
}

// #5354
HRESULT WINAPI XLiveContentVerifyInstalledPackage(const XLIVE_CONTENT_INFO* pContentInfo, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams)
{
	XLS_TRACE_FN();
	if (!pContentInfo) {
		return E_POINTER;
	}
	Content content;
	if (!FindContent(pContentInfo, &content) || !content.installed) {
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	return S_OK;
}

// #5355
HRESULT WINAPI XLiveContentGetPath(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, LPWSTR lpszPath, DWORD* pcchPath)
{
	XLS_TRACE_FN();
	if (!pContentInfo || !pcchPath) {
		return E_POINTER;
	}
	Content content;
	if (!FindContent(pContentInfo, &content)) {
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	// Titles append their own backslash separated names to this, so it ends with one.
	std::wstring path = content.mapping.path;
	if (!path.empty() && path.back() != L'\\') {
		path.push_back(L'\\');
	}
	DWORD needed = (DWORD)path.size() + 1;
	if (!lpszPath || *pcchPath < needed) {
		*pcchPath = needed;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(lpszPath, path.c_str(), needed * sizeof(wchar_t));
	*pcchPath = needed;
	return S_OK;
}

// #5356
HRESULT WINAPI XLiveContentGetDisplayName(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, LPWSTR lpszDisplayName, DWORD* pcchDisplayName)
{
	XLS_TRACE_FN();
	if (!pContentInfo || !pcchDisplayName) {
		return E_POINTER;
	}
	Content content;
	if (!FindContent(pContentInfo, &content)) {
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	DWORD needed = (DWORD)content.mapping.displayName.size() + 1;
	if (!lpszDisplayName || *pcchDisplayName < needed) {
		*pcchDisplayName = needed;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(lpszDisplayName, content.mapping.displayName.c_str(), needed * sizeof(wchar_t));
	*pcchDisplayName = needed;
	return S_OK;
}

// #5357
HRESULT WINAPI XLiveContentGetThumbnail(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, uint8_t* pbThumbnail, DWORD* pcbThumbnail)
{
	XLS_TRACE_FN();
	if (!pContentInfo || !pcbThumbnail) {
		return E_POINTER;
	}
	Content content;
	if (!FindContent(pContentInfo, &content)) {
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	std::vector<uint8_t> thumbnail;
	std::wstring path = content.mapping.path;
	if (!path.empty() && path.back() != L'\\') {
		path.push_back(L'\\');
	}
	if (!xls::ReadFileBytes(path + L"thumbnail.png", thumbnail)) {
		*pcbThumbnail = 0;
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	if (!pbThumbnail || *pcbThumbnail < thumbnail.size()) {
		*pcbThumbnail = (DWORD)thumbnail.size();
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(pbThumbnail, thumbnail.data(), thumbnail.size());
	*pcbThumbnail = (DWORD)thumbnail.size();
	return S_OK;
}

// #5358
HRESULT WINAPI XLiveContentInstallLicense(const XLIVE_CONTENT_INFO* pContentInfo, LPCWSTR lpszLicenseFile, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5363
HRESULT WINAPI XLiveContentGetLicensePath(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, LPWSTR lpszLicensePath, DWORD* pcchLicensePath)
{
	XLS_TRACE_FN();
	if (!pContentInfo || !pcchLicensePath) {
		return E_POINTER;
	}
	Content content;
	if (!FindContent(pContentInfo, &content)) {
		return HRESULT_FROM_WIN32(ERROR_NOT_FOUND);
	}
	std::wstring path = content.mapping.licensePath.empty() ? content.mapping.path : content.mapping.licensePath;
	DWORD needed = (DWORD)path.size() + 1;
	if (!lpszLicensePath || *pcchLicensePath < needed) {
		*pcchLicensePath = needed;
		return HRESULT_FROM_WIN32(ERROR_INSUFFICIENT_BUFFER);
	}
	memcpy(lpszLicensePath, path.c_str(), needed * sizeof(wchar_t));
	*pcchLicensePath = needed;
	return S_OK;
}

// #5361
HRESULT WINAPI XLiveContentRetrieveOffersByDate(DWORD dwUserIndex, DWORD dwOfferInfoVersion, const SYSTEMTIME* pstStartDate, XLIVE_OFFER_INFO* pOfferInfo, DWORD* pcOfferInfo, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!pcOfferInfo) {
		return E_POINTER;
	}
	EnsureScanned();
	DWORD count = 0;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (const Content& content : g_content) {
			if (content.owned) {
				continue;
			}
			if (pOfferInfo && count < *pcOfferInfo) {
				XLIVE_OFFER_INFO& offer = pOfferInfo[count];
				memset(&offer, 0, sizeof(offer));
				offer.qwOfferID = content.mapping.offerId ? content.mapping.offerId : content.mapping.appId;
				xls::CopyStringW(offer.pszName, XLIVE_OFFER_INFO_TITLE_LENGTH, content.mapping.displayName.c_str());
				xls::CopyStringW(offer.pszDescription, XLIVE_OFFER_INFO_DESCRIPTION_LENGTH, content.mapping.displayName.c_str());
				std::wstring url = xls::FormatW(L"https://store.steampowered.com/app/%u", content.mapping.appId);
				xls::CopyStringW(offer.pszImageUrl, XLIVE_OFFER_INFO_IMAGEURL_LENGTH, url.c_str());
				xls::CopyStringW(offer.pszGameTitle, XLIVE_OFFER_INFO_TITLE_LENGTH, xls::spa::Loaded() ? xls::spa::TitleName().c_str() : L"");
				if (dwOfferInfoVersion >= XLIVE_OFFER_INFO_VERSION) {
					xls::CopyStringW(offer.pszMediaType, XLIVE_OFFER_INFO_TITLE_LENGTH, L"DLC");
				}
			}
			count++;
		}
	}
	*pcOfferInfo = count;
	DWORD result = xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS, count);
	return result == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : S_OK;
}

// #5362
BOOL WINAPI XLiveMarketplaceDoesContentIdMatch(const uint8_t* pbContentId, const XLIVE_CONTENT_INFO* pContentInfo)
{
	XLS_TRACE_FN();
	if (!pbContentId || !pContentInfo) {
		return FALSE;
	}
	return memcmp(pbContentId, pContentInfo->abContentID, XLIVE_CONTENT_ID_SIZE) == 0 ? TRUE : FALSE;
}

// #5367
DWORD WINAPI XContentGetMarketplaceCounts(DWORD dwUserIndex, DWORD dwContentCategories, DWORD cbResults, XOFFERING_CONTENTAVAILABLE_RESULT* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pResults || cbResults < sizeof(XOFFERING_CONTENTAVAILABLE_RESULT)) {
		return ERROR_INVALID_PARAMETER;
	}
	EnsureScanned();
	std::lock_guard<std::mutex> lock(g_mutex);
	pResults->dwTotalOffers = (DWORD)g_content.size();
	pResults->dwNewOffers = 0;
	for (const Content& content : g_content) {
		if (!content.owned) {
			pResults->dwNewOffers++;
		}
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5370
DWORD WINAPI XMarketplaceConsumeAssets(DWORD dwUserIndex, DWORD cAssets, const XMARKETPLACE_ASSET* pAssets, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5371
DWORD WINAPI XMarketplaceCreateAssetEnumerator(DWORD dwUserIndex, DWORD cItems, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	auto enumerator = std::make_unique<AssetEnumerator>();
	*pcbBuffer = sizeof(XMARKETPLACE_ASSET_ENUMERATE_REPLY) + cItems * sizeof(XMARKETPLACE_ASSET);
	enumerator->requiredBufferSize = *pcbBuffer;
	*phEnum = xls::EnumeratorRegister(std::move(enumerator));
	return *phEnum ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

namespace {

DWORD CreateOffers(DWORD cItems, const uint64_t* offerIds, DWORD offerIdCount, bool includeOwned, DWORD* bufferSize, HANDLE* handle)
{
	EnsureScanned();
	auto enumerator = std::make_unique<OfferEnumerator>();
	enumerator->batchSize = cItems ? cItems : 1;
	{
		std::lock_guard<std::mutex> lock(g_mutex);
		for (const Content& content : g_content) {
			if (!includeOwned && content.owned) {
				continue;
			}
			if (offerIds && offerIdCount) {
				bool wanted = false;
				uint64_t id = content.mapping.offerId ? content.mapping.offerId : content.mapping.appId;
				for (DWORD i = 0; i < offerIdCount; i++) {
					if (offerIds[i] == id) {
						wanted = true;
					}
				}
				if (!wanted) {
					continue;
				}
			}
			enumerator->offers.push_back(OfferFor(content));
		}
	}
	*bufferSize = enumerator->batchSize * (sizeof(XMARKETPLACE_CONTENTOFFER_INFO) + (XCONTENT_MAX_DISPLAYNAME_LENGTH * 3 + 3) * sizeof(wchar_t));
	enumerator->requiredBufferSize = *bufferSize;
	*handle = xls::EnumeratorRegister(std::move(enumerator));
	return *handle ? ERROR_SUCCESS : ERROR_FUNCTION_FAILED;
}

}

// #5372
DWORD WINAPI XMarketplaceCreateOfferEnumerator(DWORD dwUserIndex, DWORD dwOfferType, DWORD dwContentCategories, DWORD cItems, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	return CreateOffers(cItems, nullptr, 0, true, pcbBuffer, phEnum);
}

// #5376
DWORD WINAPI XMarketplaceCreateOfferEnumeratorByOffering(DWORD dwUserIndex, DWORD cItems, const uint64_t* pOfferIds, WORD cOfferIds, DWORD* pcbBuffer, HANDLE* phEnum)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pOfferIds || !cOfferIds || !pcbBuffer || !phEnum) {
		return ERROR_INVALID_PARAMETER;
	}
	return CreateOffers(cItems, pOfferIds, cOfferIds, true, pcbBuffer, phEnum);
}

// #5374
DWORD WINAPI XMarketplaceGetDownloadStatus(DWORD dwUserIndex, uint64_t qwOfferId, DWORD* pdwResult)
{
	XLS_TRACE_FN();
	if (!pdwResult) {
		return ERROR_INVALID_PARAMETER;
	}
	EnsureScanned();
	std::lock_guard<std::mutex> lock(g_mutex);
	for (const Content& content : g_content) {
		uint64_t id = content.mapping.offerId ? content.mapping.offerId : content.mapping.appId;
		if (id != qwOfferId) {
			continue;
		}
		if (content.installed) {
			*pdwResult = 100;
			return ERROR_SUCCESS;
		}
		uint64 downloaded = 0;
		uint64 total = 0;
		if (xls::SteamReady() && xls::SteamApps()->GetDlcDownloadProgress(content.mapping.appId, &downloaded, &total) && total) {
			*pdwResult = (DWORD)(downloaded * 100 / total);
		}
		else {
			*pdwResult = 0;
		}
		return ERROR_SUCCESS;
	}
	return ERROR_NOT_FOUND;
}

// #5375
void WINAPI XMarketplaceGetImageUrl(DWORD dwTitleId, uint64_t qwOfferId, DWORD cchUrl, LPWSTR pszUrl)
{
	XLS_TRACE_FN();
	if (!pszUrl || !cchUrl) {
		return;
	}
	std::wstring url = xls::FormatW(L"https://cdn.akamai.steamstatic.com/steam/apps/%llu/header.jpg", qwOfferId);
	xls::CopyStringW(pszUrl, cchUrl, url.c_str());
}
