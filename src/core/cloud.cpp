#include "core/cloud.h"

#include "core/config.h"
#include "core/log.h"
#include "core/steam.h"
#include "core/utils.h"

#include <set>

namespace xls {

namespace {

bool g_writeFailureLogged = false;

bool UseSteamCloud()
{
	return Cfg().cloudEnabled && SteamReady() && SteamRemoteStorage() && SteamRemoteStorage()->IsCloudEnabledForAccount() && SteamRemoteStorage()->IsCloudEnabledForApp();
}

std::wstring LocalRoot()
{
	std::wstring path = Cfg().localStorageDir;
	if (SteamReady()) {
		path += FormatW(L"%u\\", SteamLocalId().GetAccountID());
	}
	return path;
}

std::wstring LocalPath(const std::string& name)
{
	std::wstring relative = Utf8ToWide(name.c_str());
	for (wchar_t& c : relative) {
		if (c == L'/') {
			c = L'\\';
		}
	}
	return LocalRoot() + relative;
}

bool LocalWrite(const std::string& name, const void* data, size_t size)
{
	std::wstring path = LocalPath(name);
	size_t slash = path.find_last_of(L'\\');
	if (slash != std::wstring::npos) {
		EnsureDirectory(path.substr(0, slash + 1));
	}
	return WriteFileBytes(path, data, size);
}

void ListLocal(const std::wstring& directory, const std::string& relativePrefix, std::vector<CloudFileInfo>& out)
{
	WIN32_FIND_DATAW found;
	HANDLE handle = FindFirstFileW((directory + L"*").c_str(), &found);
	if (handle == INVALID_HANDLE_VALUE) {
		return;
	}
	do {
		if (wcscmp(found.cFileName, L".") == 0 || wcscmp(found.cFileName, L"..") == 0) {
			continue;
		}
		std::string name = relativePrefix + WideToUtf8(found.cFileName);
		if (found.dwFileAttributes & FILE_ATTRIBUTE_DIRECTORY) {
			ListLocal(directory + found.cFileName + L"\\", name + "/", out);
		}
		else {
			CloudFileInfo info;
			info.name = name;
			info.size = (int32_t)found.nFileSizeLow;
			info.timestamp = (int64_t)FileTimeToUnixTime(found.ftLastWriteTime);
			out.push_back(info);
		}
	} while (FindNextFileW(handle, &found));
	FindClose(handle);
}

bool SteamHas(const std::string& name)
{
	return UseSteamCloud() && SteamRemoteStorage()->FileExists(name.c_str());
}

}

bool CloudAvailable()
{
	return UseSteamCloud();
}

bool CloudExists(const std::string& name)
{
	return SteamHas(name) || FileExists(LocalPath(name));
}

bool CloudRead(const std::string& name, std::vector<uint8_t>& data)
{
	if (SteamHas(name)) {
		int32 size = SteamRemoteStorage()->GetFileSize(name.c_str());
		if (size >= 0) {
			data.resize((size_t)size);
			int32 read = size ? SteamRemoteStorage()->FileRead(name.c_str(), data.data(), size) : 0;
			if (read == size) {
				return true;
			}
			XLS_LOG_WARN("cloud: read %d of %d bytes of %s.", read, size, name.c_str());
		}
	}
	return ReadFileBytes(LocalPath(name), data);
}

bool CloudWrite(const std::string& name, const void* data, size_t size)
{
	if (UseSteamCloud()) {
		if (SteamRemoteStorage()->FileWrite(name.c_str(), data, (int32)size)) {
			return true;
		}
		// An app can have Cloud on for its auto-cloud folders but no API quota, so the file stays local.
		if (!g_writeFailureLogged) {
			uint64 total = 0;
			uint64 available = 0;
			bool quota = SteamRemoteStorage()->GetQuota(&total, &available);
			XLS_LOG_WARN("cloud: FileWrite %s (%zu bytes) refused (quota %s: %llu total, %llu free), files go to %ls from now on.", name.c_str(), size, quota ? "known" : "unavailable", total, available, LocalRoot().c_str());
			g_writeFailureLogged = true;
		}
	}
	return LocalWrite(name, data, size);
}

bool CloudDelete(const std::string& name)
{
	bool deleted = false;
	if (SteamHas(name)) {
		deleted = SteamRemoteStorage()->FileDelete(name.c_str());
	}
	if (DeleteFileW(LocalPath(name).c_str())) {
		deleted = true;
	}
	return deleted;
}

int64_t CloudTimestamp(const std::string& name)
{
	if (SteamHas(name)) {
		return SteamRemoteStorage()->GetFileTimestamp(name.c_str());
	}
	WIN32_FILE_ATTRIBUTE_DATA attributes;
	if (!GetFileAttributesExW(LocalPath(name).c_str(), GetFileExInfoStandard, &attributes)) {
		return 0;
	}
	return (int64_t)FileTimeToUnixTime(attributes.ftLastWriteTime);
}

std::vector<CloudFileInfo> CloudList(const std::string& prefix)
{
	std::vector<CloudFileInfo> result;
	std::set<std::string> seen;
	if (UseSteamCloud()) {
		int32 count = SteamRemoteStorage()->GetFileCount();
		for (int32 i = 0; i < count; i++) {
			int32 size = 0;
			const char* name = SteamRemoteStorage()->GetFileNameAndSize(i, &size);
			if (!name || strncmp(name, prefix.c_str(), prefix.size()) != 0) {
				continue;
			}
			CloudFileInfo info;
			info.name = name;
			info.size = size;
			info.timestamp = SteamRemoteStorage()->GetFileTimestamp(name);
			result.push_back(info);
			seen.insert(info.name);
		}
	}
	std::vector<CloudFileInfo> local;
	ListLocal(LocalRoot(), std::string(), local);
	for (const CloudFileInfo& info : local) {
		if (info.name.compare(0, prefix.size(), prefix) == 0 && !seen.count(info.name)) {
			result.push_back(info);
		}
	}
	return result;
}

}
