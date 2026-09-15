#include "core/utils.h"

#include <shlwapi.h>
#include <stdarg.h>
#include <stdio.h>

extern "C" IMAGE_DOS_HEADER __ImageBase;

namespace xls {

std::string WideToUtf8(const wchar_t* text, int length)
{
	if (!text) {
		return std::string();
	}
	int needed = WideCharToMultiByte(CP_UTF8, 0, text, length, nullptr, 0, nullptr, nullptr);
	if (needed <= 0) {
		return std::string();
	}
	std::string result((size_t)needed, 0);
	WideCharToMultiByte(CP_UTF8, 0, text, length, &result[0], needed, nullptr, nullptr);
	if (length == -1 && !result.empty() && result.back() == 0) {
		result.pop_back();
	}
	return result;
}

std::wstring Utf8ToWide(const char* text, int length)
{
	if (!text) {
		return std::wstring();
	}
	int needed = MultiByteToWideChar(CP_UTF8, 0, text, length, nullptr, 0);
	if (needed <= 0) {
		return std::wstring();
	}
	std::wstring result((size_t)needed, 0);
	MultiByteToWideChar(CP_UTF8, 0, text, length, &result[0], needed);
	if (length == -1 && !result.empty() && result.back() == 0) {
		result.pop_back();
	}
	return result;
}

std::string FormatA(const char* format, ...)
{
	va_list args;
	va_start(args, format);
	int needed = _vscprintf(format, args);
	va_end(args);
	if (needed < 0) {
		return std::string();
	}
	std::string result((size_t)needed + 1, 0);
	va_start(args, format);
	vsnprintf(&result[0], result.size(), format, args);
	va_end(args);
	result.resize((size_t)needed);
	return result;
}

std::wstring FormatW(const wchar_t* format, ...)
{
	va_list args;
	va_start(args, format);
	int needed = _vscwprintf(format, args);
	va_end(args);
	if (needed < 0) {
		return std::wstring();
	}
	std::wstring result((size_t)needed + 1, 0);
	va_start(args, format);
	_vsnwprintf(&result[0], result.size(), format, args);
	va_end(args);
	result.resize((size_t)needed);
	return result;
}

static std::wstring DirectoryOfModule(HMODULE module)
{
	wchar_t path[MAX_PATH * 2] = {};
	DWORD length = GetModuleFileNameW(module, path, (DWORD)(sizeof(path) / sizeof(path[0])));
	if (!length) {
		return std::wstring();
	}
	std::wstring result(path, length);
	size_t slash = result.find_last_of(L"\\/");
	if (slash == std::wstring::npos) {
		return std::wstring();
	}
	return result.substr(0, slash + 1);
}

std::wstring ModuleDirectory()
{
	return DirectoryOfModule((HMODULE)&__ImageBase);
}

std::wstring ExeDirectory()
{
	return DirectoryOfModule(nullptr);
}

bool FileExists(const std::wstring& path)
{
	DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && !(attributes & FILE_ATTRIBUTE_DIRECTORY);
}

bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& data)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	LARGE_INTEGER size = {};
	if (!GetFileSizeEx(file, &size) || size.QuadPart > 0x7FFFFFFF) {
		CloseHandle(file);
		return false;
	}
	data.resize((size_t)size.QuadPart);
	DWORD read = 0;
	bool ok = data.empty() || (ReadFile(file, data.data(), (DWORD)data.size(), &read, nullptr) && read == data.size());
	CloseHandle(file);
	return ok;
}

bool WriteFileBytes(const std::wstring& path, const void* data, size_t size)
{
	HANDLE file = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (file == INVALID_HANDLE_VALUE) {
		return false;
	}
	DWORD written = 0;
	bool ok = size == 0 || (WriteFile(file, data, (DWORD)size, &written, nullptr) && written == size);
	CloseHandle(file);
	return ok;
}

bool EnsureDirectory(const std::wstring& path)
{
	std::wstring current;
	for (size_t i = 0; i < path.size(); i++) {
		wchar_t c = path[i];
		current.push_back(c);
		if ((c == L'\\' || c == L'/') && current.size() > 3) {
			CreateDirectoryW(current.c_str(), nullptr);
		}
	}
	if (!current.empty() && current.back() != L'\\' && current.back() != L'/') {
		CreateDirectoryW(current.c_str(), nullptr);
	}
	DWORD attributes = GetFileAttributesW(path.c_str());
	return attributes != INVALID_FILE_ATTRIBUTES && (attributes & FILE_ATTRIBUTE_DIRECTORY);
}

FILETIME UnixTimeToFileTime(uint64_t unixSeconds)
{
	uint64_t ticks = unixSeconds * 10000000ULL + 116444736000000000ULL;
	FILETIME result;
	result.dwLowDateTime = (DWORD)ticks;
	result.dwHighDateTime = (DWORD)(ticks >> 32);
	return result;
}

uint64_t FileTimeToUnixTime(const FILETIME& fileTime)
{
	uint64_t ticks = ((uint64_t)fileTime.dwHighDateTime << 32) | fileTime.dwLowDateTime;
	if (ticks < 116444736000000000ULL) {
		return 0;
	}
	return (ticks - 116444736000000000ULL) / 10000000ULL;
}

FILETIME NowFileTime()
{
	FILETIME now;
	GetSystemTimeAsFileTime(&now);
	return now;
}

std::string HexEncode(const void* data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	const uint8_t* bytes = (const uint8_t*)data;
	std::string result;
	result.reserve(size * 2);
	for (size_t i = 0; i < size; i++) {
		result.push_back(digits[bytes[i] >> 4]);
		result.push_back(digits[bytes[i] & 0xF]);
	}
	return result;
}

static int HexValue(char c)
{
	if (c >= '0' && c <= '9') return c - '0';
	if (c >= 'a' && c <= 'f') return c - 'a' + 10;
	if (c >= 'A' && c <= 'F') return c - 'A' + 10;
	return -1;
}

bool HexDecode(const std::string& hex, void* data, size_t size)
{
	if (hex.size() != size * 2) {
		return false;
	}
	uint8_t* bytes = (uint8_t*)data;
	for (size_t i = 0; i < size; i++) {
		int high = HexValue(hex[i * 2]);
		int low = HexValue(hex[i * 2 + 1]);
		if (high < 0 || low < 0) {
			return false;
		}
		bytes[i] = (uint8_t)((high << 4) | low);
	}
	return true;
}

uint32_t Fnv1a32(const void* data, size_t size)
{
	const uint8_t* bytes = (const uint8_t*)data;
	uint32_t hash = 2166136261u;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 16777619u;
	}
	return hash;
}

uint64_t Fnv1a64(const void* data, size_t size)
{
	const uint8_t* bytes = (const uint8_t*)data;
	uint64_t hash = 14695981039346656037ULL;
	for (size_t i = 0; i < size; i++) {
		hash ^= bytes[i];
		hash *= 1099511628211ULL;
	}
	return hash;
}

void CopyStringW(wchar_t* destination, size_t destinationCount, const wchar_t* source)
{
	if (!destination || !destinationCount) {
		return;
	}
	if (!source) {
		destination[0] = 0;
		return;
	}
	size_t i = 0;
	for (; i + 1 < destinationCount && source[i]; i++) {
		destination[i] = source[i];
	}
	destination[i] = 0;
}

void CopyStringA(char* destination, size_t destinationCount, const char* source)
{
	if (!destination || !destinationCount) {
		return;
	}
	if (!source) {
		destination[0] = 0;
		return;
	}
	size_t i = 0;
	for (; i + 1 < destinationCount && source[i]; i++) {
		destination[i] = source[i];
	}
	destination[i] = 0;
}

std::string GamertagFromPersona(const char* personaUtf8, bool asciiOnly)
{
	std::wstring wide = Utf8ToWide(personaUtf8);
	std::string result;
	for (wchar_t c : wide) {
		if (result.size() >= 15) {
			break;
		}
		if (c < 0x20) {
			continue;
		}
		if (c < 0x7F) {
			result.push_back((char)c);
		}
		else if (asciiOnly) {
			result.push_back('_');
		}
		else {
			std::string utf8 = WideToUtf8(&c, 1);
			if (result.size() + utf8.size() > 15) {
				break;
			}
			result += utf8;
		}
	}
	while (!result.empty() && result.back() == ' ') {
		result.pop_back();
	}
	if (result.empty()) {
		result = "Player";
	}
	return result;
}

std::wstring SanitizeFileName(const std::wstring& name)
{
	std::wstring result = name;
	for (wchar_t& c : result) {
		if (c == L'<' || c == L'>' || c == L':' || c == L'"' || c == L'/' || c == L'\\' || c == L'|' || c == L'?' || c == L'*' || c < 0x20) {
			c = L'_';
		}
	}
	return result;
}

}
