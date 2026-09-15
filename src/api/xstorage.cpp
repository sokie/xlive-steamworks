// #5304 - #5309, #5344, #5345: per-user files go to Steam Cloud, per-title files are shipped read-only.
#include "xlive/xfuncs.h"
#include "api/xlive.h"

#include "core/cloud.h"
#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/overlapped.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

namespace {

const wchar_t* kUserPrefix = L"user/";
const wchar_t* kTitlePrefix = L"title/";
const wchar_t* kClipPrefix = L"clip/";
const char* kCloudPrefix = "xlive/tms/";

enum class Facility { User, Title, Clip, Invalid };

struct ParsedPath {
	Facility facility = Facility::Invalid;
	XUID xuid = INVALID_XUID;
	std::wstring item; // Relative item name, forward slashes.
};

ParsedPath ParseServerPath(const wchar_t* serverPath)
{
	ParsedPath parsed;
	if (!serverPath) {
		return parsed;
	}
	std::wstring path = serverPath;
	if (path.compare(0, wcslen(kUserPrefix), kUserPrefix) == 0) {
		size_t slash = path.find(L'/', wcslen(kUserPrefix));
		if (slash == std::wstring::npos) {
			return parsed;
		}
		parsed.facility = Facility::User;
		parsed.xuid = _wcstoui64(path.c_str() + wcslen(kUserPrefix), nullptr, 16);
		parsed.item = path.substr(slash + 1);
	}
	else if (path.compare(0, wcslen(kTitlePrefix), kTitlePrefix) == 0) {
		parsed.facility = Facility::Title;
		parsed.item = path.substr(wcslen(kTitlePrefix));
	}
	else if (path.compare(0, wcslen(kClipPrefix), kClipPrefix) == 0) {
		parsed.facility = Facility::Clip;
		parsed.item = path.substr(wcslen(kClipPrefix));
	}
	return parsed;
}

std::string CloudName(const std::wstring& item)
{
	return kCloudPrefix + xls::WideToUtf8(item.c_str());
}

std::wstring TitleFilePath(const std::wstring& item)
{
	std::wstring relative = item;
	for (wchar_t& c : relative) {
		if (c == L'/') {
			c = L'\\';
		}
	}
	return xls::Cfg().titleStorageDir + relative;
}

bool WildcardMatch(const std::wstring& pattern, const std::wstring& text)
{
	size_t p = 0;
	size_t t = 0;
	size_t star = std::wstring::npos;
	size_t match = 0;
	while (t < text.size()) {
		if (p < pattern.size() && (pattern[p] == L'?' || towlower(pattern[p]) == towlower(text[t]))) {
			p++;
			t++;
		}
		else if (p < pattern.size() && pattern[p] == L'*') {
			star = p++;
			match = t;
		}
		else if (star != std::wstring::npos) {
			p = star + 1;
			t = ++match;
		}
		else {
			return false;
		}
	}
	while (p < pattern.size() && pattern[p] == L'*') {
		p++;
	}
	return p == pattern.size();
}

DWORD BuildPath(XUID xuid, XSTORAGE_FACILITY facility, const void* facilityInfo, DWORD facilityInfoSize, LPCWSTR itemName, LPWSTR serverPath, DWORD* serverPathLength)
{
	if (!itemName || !*itemName || !serverPathLength) {
		return ERROR_INVALID_PARAMETER;
	}
	std::wstring item = itemName;
	for (wchar_t& c : item) {
		if (c == L'\\') {
			c = L'/';
		}
	}
	std::wstring path;
	switch (facility) {
		case XSTORAGE_FACILITY_PER_USER_TITLE:
			path = xls::FormatW(L"%s%016llx/%s", kUserPrefix, xuid, item.c_str());
			break;
		case XSTORAGE_FACILITY_PER_TITLE:
			path = std::wstring(kTitlePrefix) + item;
			break;
		case XSTORAGE_FACILITY_GAME_CLIP: {
			if (!facilityInfo || facilityInfoSize < sizeof(XSTORAGE_FACILITY_INFO_GAME_CLIP)) {
				return ERROR_INVALID_PARAMETER;
			}
			const XSTORAGE_FACILITY_INFO_GAME_CLIP* clip = (const XSTORAGE_FACILITY_INFO_GAME_CLIP*)facilityInfo;
			path = xls::FormatW(L"%s%u/%s", kClipPrefix, clip->dwLeaderboardID, item.c_str());
			break;
		}
		default:
			return ERROR_INVALID_PARAMETER;
	}
	// The length is in characters and includes the terminator.
	DWORD needed = (DWORD)path.size() + 1;
	if (!serverPath || *serverPathLength < needed) {
		*serverPathLength = needed;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	memcpy(serverPath, path.c_str(), needed * sizeof(wchar_t));
	*serverPathLength = needed;
	return ERROR_SUCCESS;
}

}

// #5344
DWORD WINAPI XStorageBuildServerPath(DWORD dwUserIndex, XSTORAGE_FACILITY storageFacility, const void* pvStorageFacilityInfo, DWORD dwStorageFacilityInfoSize, LPCWSTR pwszItemName, LPWSTR pwszServerPath, DWORD* pdwServerPathLength)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	return BuildPath(xls::UserXuid(dwUserIndex), storageFacility, pvStorageFacilityInfo, dwStorageFacilityInfoSize, pwszItemName, pwszServerPath, pdwServerPathLength);
}

// #5309
DWORD WINAPI XStorageBuildServerPathByXuid(XUID xuidUser, XSTORAGE_FACILITY storageFacility, const void* pvStorageFacilityInfo, DWORD dwStorageFacilityInfoSize, LPCWSTR pwszItemName, LPWSTR pwszServerPath, DWORD* pdwServerPathLength)
{
	XLS_TRACE_FN();
	if (!xuidUser) {
		return ERROR_INVALID_PARAMETER;
	}
	return BuildPath(xuidUser, storageFacility, pvStorageFacilityInfo, dwStorageFacilityInfoSize, pwszItemName, pwszServerPath, pdwServerPathLength);
}

// #5305
DWORD WINAPI XStorageUploadFromMemory(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwBufferSize, const uint8_t* pbBuffer, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!wszServerPath || !pbBuffer || dwBufferSize > XSTORAGE_MAX_MEMORY_BUFFER_SIZE) {
		return ERROR_INVALID_PARAMETER;
	}
	ParsedPath parsed = ParseServerPath(wszServerPath);
	DWORD result;
	switch (parsed.facility) {
		case Facility::User:
			if (parsed.xuid != xls::UserXuid(dwUserIndex)) {
				result = (DWORD)XONLINE_E_STORAGE_ACCESS_DENIED;
			}
			else {
				result = xls::CloudWrite(CloudName(parsed.item), pbBuffer, dwBufferSize) ? ERROR_SUCCESS : (DWORD)XONLINE_E_STORAGE_QUOTA_EXCEEDED;
			}
			break;
		case Facility::Title:
			// Only the publisher could write here on LIVE
			result = (DWORD)XONLINE_E_STORAGE_ACCESS_DENIED;
			break;
		default:
			result = (DWORD)XONLINE_E_STORAGE_INVALID_STORAGE_PATH;
			break;
	}
	XLS_LOG_DEBUG("XStorageUploadFromMemory: %ls (%u bytes) -> 0x%08x.", wszServerPath, dwBufferSize, result);
	return xls::OverlappedReturn(pOverlapped, result, dwBufferSize);
}

// #5304
DWORD WINAPI XStorageUploadFromMemoryGetProgress(XOVERLAPPED* pOverlapped, DWORD* pdwPercentComplete, uint64_t* pqwNumerator, uint64_t* pqwDenominator)
{
	XLS_TRACE_FN();
	if (!pOverlapped) {
		return ERROR_INVALID_PARAMETER;
	}
	bool done = XHasOverlappedIoCompleted(pOverlapped);
	if (pdwPercentComplete) *pdwPercentComplete = done ? 100 : 0;
	if (pqwNumerator) *pqwNumerator = done ? 1 : 0;
	if (pqwDenominator) *pqwDenominator = 1;
	return ERROR_SUCCESS;
}

// #5345
DWORD WINAPI XStorageDownloadToMemory(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwBufferSize, uint8_t* pbBuffer, DWORD cbResults, XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!wszServerPath || !*wszServerPath || !pbBuffer || !dwBufferSize || cbResults != sizeof(XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS) || !pResults) {
		return ERROR_INVALID_PARAMETER;
	}
	memset(pResults, 0, sizeof(*pResults));
	ParsedPath parsed = ParseServerPath(wszServerPath);
	std::vector<uint8_t> data;
	DWORD result = ERROR_SUCCESS;
	switch (parsed.facility) {
		case Facility::User:
			if (parsed.xuid != xls::UserXuid(dwUserIndex)) {
				// Another player's cloud files are not reachable.
				result = (DWORD)XONLINE_E_STORAGE_FILE_NOT_FOUND;
			}
			else if (!xls::CloudRead(CloudName(parsed.item), data)) {
				result = (DWORD)XONLINE_E_STORAGE_FILE_NOT_FOUND;
			}
			else {
				pResults->ftCreated = xls::UnixTimeToFileTime((uint64_t)xls::CloudTimestamp(CloudName(parsed.item)));
			}
			break;
		case Facility::Title: {
			std::wstring path = TitleFilePath(parsed.item);
			if (!xls::ReadFileBytes(path, data)) {
				result = (DWORD)XONLINE_E_STORAGE_FILE_NOT_FOUND;
			}
			else {
				WIN32_FILE_ATTRIBUTE_DATA attributes;
				if (GetFileAttributesExW(path.c_str(), GetFileExInfoStandard, &attributes)) {
					pResults->ftCreated = attributes.ftCreationTime;
				}
			}
			break;
		}
		default:
			result = (DWORD)XONLINE_E_STORAGE_INVALID_STORAGE_PATH;
			break;
	}
	if (result == ERROR_SUCCESS) {
		pResults->dwBytesTotal = (DWORD)data.size();
		pResults->xuidOwner = parsed.facility == Facility::User ? parsed.xuid : INVALID_XUID;
		if (data.size() > dwBufferSize) {
			result = ERROR_INSUFFICIENT_BUFFER;
		}
		else {
			memcpy(pbBuffer, data.data(), data.size());
		}
	}
	XLS_LOG_DEBUG("XStorageDownloadToMemory: %ls -> 0x%08x (%u bytes).", wszServerPath, result, pResults->dwBytesTotal);
	return xls::OverlappedReturn(pOverlapped, result, pResults->dwBytesTotal);
}

// #5307
DWORD WINAPI XStorageDownloadToMemoryGetProgress(XOVERLAPPED* pOverlapped, DWORD* pdwPercentComplete, uint64_t* pqwNumerator, uint64_t* pqwDenominator)
{
	XLS_TRACE_FN();
	return XStorageUploadFromMemoryGetProgress(pOverlapped, pdwPercentComplete, pqwNumerator, pqwDenominator);
}

// #5308
DWORD WINAPI XStorageDelete(DWORD dwUserIndex, LPCWSTR wszServerPath, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!wszServerPath) {
		return ERROR_INVALID_PARAMETER;
	}
	ParsedPath parsed = ParseServerPath(wszServerPath);
	DWORD result;
	if (parsed.facility == Facility::User && parsed.xuid == xls::UserXuid(dwUserIndex)) {
		result = xls::CloudDelete(CloudName(parsed.item)) ? ERROR_SUCCESS : (DWORD)XONLINE_E_STORAGE_FILE_NOT_FOUND;
	}
	else if (parsed.facility == Facility::Title) {
		result = (DWORD)XONLINE_E_STORAGE_ACCESS_DENIED;
	}
	else {
		result = (DWORD)XONLINE_E_STORAGE_INVALID_STORAGE_PATH;
	}
	return xls::OverlappedReturn(pOverlapped, result);
}

// #5306
DWORD WINAPI XStorageEnumerate(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwStartingIndex, DWORD dwMaxResultsToReturn, DWORD cbResults, XSTORAGE_ENUMERATE_RESULTS* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!wszServerPath || !pResults || dwMaxResultsToReturn > XSTORAGE_MAX_RESULTS_TO_RETURN || dwStartingIndex + dwMaxResultsToReturn > XSTORAGE_MAX_RESULTS_TO_RETURN) {
		return ERROR_INVALID_PARAMETER;
	}
	DWORD required = sizeof(XSTORAGE_ENUMERATE_RESULTS) + dwMaxResultsToReturn * (sizeof(XSTORAGE_FILE_INFO) + XONLINE_MAX_PATHNAME_LENGTH * sizeof(wchar_t));
	if (cbResults < required) {
		return ERROR_INVALID_PARAMETER;
	}

	ParsedPath parsed = ParseServerPath(wszServerPath);
	struct Entry {
		std::wstring serverPath;
		DWORD size;
		FILETIME created;
		XUID owner;
	};
	std::vector<Entry> entries;
	if (parsed.facility == Facility::User && parsed.xuid == xls::UserXuid(dwUserIndex)) {
		std::wstring pattern = parsed.item;
		size_t slash = pattern.find_last_of(L'/');
		std::string prefix = kCloudPrefix + (slash == std::wstring::npos ? std::string() : xls::WideToUtf8(pattern.substr(0, slash + 1).c_str()));
		for (const xls::CloudFileInfo& file : xls::CloudList(prefix)) {
			std::wstring item = xls::Utf8ToWide(file.name.c_str() + strlen(kCloudPrefix));
			if (!WildcardMatch(pattern, item)) {
				continue;
			}
			Entry entry;
			entry.serverPath = xls::FormatW(L"%s%016llx/%s", kUserPrefix, parsed.xuid, item.c_str());
			entry.size = (DWORD)file.size;
			entry.created = xls::UnixTimeToFileTime((uint64_t)file.timestamp);
			entry.owner = parsed.xuid;
			entries.push_back(entry);
		}
	}
	else if (parsed.facility == Facility::Title) {
		std::wstring directory = TitleFilePath(parsed.item);
		size_t slash = directory.find_last_of(L'\\');
		std::wstring folder = slash == std::wstring::npos ? xls::Cfg().titleStorageDir : directory.substr(0, slash + 1);
		std::wstring pattern = slash == std::wstring::npos ? directory : directory.substr(slash + 1);
		WIN32_FIND_DATAW found;
		HANDLE handle = FindFirstFileW((folder + pattern).c_str(), &found);
		if (handle != INVALID_HANDLE_VALUE) {
			do {
				if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
					continue;
				}
				Entry entry;
				std::wstring relativeFolder = folder.substr(xls::Cfg().titleStorageDir.size());
				for (wchar_t& c : relativeFolder) {
					if (c == L'\\') c = L'/';
				}
				entry.serverPath = std::wstring(kTitlePrefix) + relativeFolder + found.cFileName;
				entry.size = found.nFileSizeLow;
				entry.created = found.ftCreationTime;
				entry.owner = INVALID_XUID;
				entries.push_back(entry);
			} while (FindNextFileW(handle, &found));
			FindClose(handle);
		}
	}

	xls::BufferPacker packer(pResults, cbResults);
	packer.Front(sizeof(XSTORAGE_ENUMERATE_RESULTS));
	pResults->dwTotalNumItems = (DWORD)entries.size();
	pResults->dwNumItemsReturned = 0;
	pResults->pItems = (XSTORAGE_FILE_INFO*)packer.FrontPointer();
	for (DWORD i = dwStartingIndex; i < entries.size() && pResults->dwNumItemsReturned < dwMaxResultsToReturn; i++) {
		XSTORAGE_FILE_INFO* info = (XSTORAGE_FILE_INFO*)packer.Front(sizeof(XSTORAGE_FILE_INFO));
		wchar_t* path = info ? packer.BackString(entries[i].serverPath.c_str()) : nullptr;
		if (!info || !path) {
			break;
		}
		memset(info, 0, sizeof(*info));
		info->dwTitleID = xls::TitleId();
		info->dwTitleVersion = xls::TitleVersion();
		info->qwOwnerPUID = entries[i].owner;
		info->dwContentType = 1;
		info->dwStorageSize = entries[i].size;
		info->dwInstalledSize = entries[i].size;
		info->ftCreated = entries[i].created;
		info->ftLastModified = entries[i].created;
		info->cchPathName = (uint16_t)(entries[i].serverPath.size() + 1);
		info->pwszPathName = path;
		pResults->dwNumItemsReturned++;
	}
	XLS_LOG_DEBUG("XStorageEnumerate: %ls -> %u of %u.", wszServerPath, pResults->dwNumItemsReturned, pResults->dwTotalNumItems);
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS, pResults->dwNumItemsReturned);
}
