#include "core/cloud.h"

#include "core/config.h"
#include "core/log.h"
#include "core/steam.h"
#include "core/utils.h"

namespace xls {

namespace {

bool UseSteamCloud()
{
	return Cfg().cloudEnabled && SteamReady() && SteamRemoteStorage() && SteamRemoteStorage()->IsCloudEnabledForAccount() && SteamRemoteStorage()->IsCloudEnabledForApp();
}

std::wstring LocalPath(const std::string& name)
{
	std::wstring path = Cfg().localStorageDir;
	if (SteamReady()) {
		path += FormatW(L"%u\\", SteamLocalId().GetAccountID());
	}
	std::wstring relative = Utf8ToWide(name.c_str());
	for (wchar_t& c : relative) {
		if (c == L'/') {
			c = L'\\';
		}
	}
	return path + relative;
}

std::wstring LocalRoot()
{
	std::wstring path = Cfg().localStorageDir;
	if (SteamReady()) {
		path += FormatW(L"%u\\", SteamLocalId().GetAccountID());
	}
	return path;
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

}

bool CloudAvailable()
{
	return UseSteamCloud();
}

bool CloudExists(const std::string& name)
{
	if (UseSteamCloud()) {
		return SteamRemoteStorage()->FileExists(name.c_str());
	}
	return FileExists(LocalPath(name));
}

bool CloudRead(const std::string& name, std::vector<uint8_t>& data)
{
	if (UseSteamCloud()) {
		if (!SteamRemoteStorage()->FileExists(name.c_str())) {
			return false;
		}
		int32 size = SteamRemoteStorage()->GetFileSize(name.c_str());
		if (size < 0) {
			return false;
		}
		data.resize((size_t)size);
		if (size == 0) {
			return true;
		}
		int32 read = SteamRemoteStorage()->FileRead(name.c_str(), data.data(), size);
		if (read != size) {
			XLS_LOG_WARN("cloud: read %d of %d bytes of %s.", read, size, name.c_str());
			data.resize(read > 0 ? (size_t)read : 0);
			return read > 0;
		}
		return true;
	}
	return ReadFileBytes(LocalPath(name), data);
}

bool CloudWrite(const std::string& name, const void* data, size_t size)
{
	if (UseSteamCloud()) {
		if (!SteamRemoteStorage()->FileWrite(name.c_str(), data, (int32)size)) {
			XLS_LOG_WARN("cloud: FileWrite %s (%zu bytes) failed, check the app's Cloud quota and file limits.", name.c_str(), size);
			return false;
		}
		return true;
	}
	std::wstring path = LocalPath(name);
	size_t slash = path.find_last_of(L'\\');
	if (slash != std::wstring::npos) {
		EnsureDirectory(path.substr(0, slash + 1));
	}
	return WriteFileBytes(path, data, size);
}

bool CloudDelete(const std::string& name)
{
	if (UseSteamCloud()) {
		return SteamRemoteStorage()->FileDelete(name.c_str());
	}
	return DeleteFileW(LocalPath(name).c_str()) != FALSE;
}

int64_t CloudTimestamp(const std::string& name)
{
	if (UseSteamCloud()) {
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
		}
		return result;
	}
	std::vector<CloudFileInfo> all;
	ListLocal(LocalRoot(), std::string(), all);
	for (const CloudFileInfo& info : all) {
		if (info.name.compare(0, prefix.size(), prefix) == 0) {
			result.push_back(info);
		}
	}
	return result;
}

}
