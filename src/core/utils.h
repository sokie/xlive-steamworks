#pragma once

#include <windows.h>
#include <stdint.h>
#include <string>
#include <vector>

namespace xls {

std::string WideToUtf8(const wchar_t* text, int length = -1);
std::wstring Utf8ToWide(const char* text, int length = -1);

std::string FormatA(const char* format, ...);
std::wstring FormatW(const wchar_t* format, ...);

// Directory that holds this xlive.dll, with a trailing backslash.
std::wstring ModuleDirectory();
// Directory that holds the title's exe, with a trailing backslash.
std::wstring ExeDirectory();

bool FileExists(const std::wstring& path);
bool ReadFileBytes(const std::wstring& path, std::vector<uint8_t>& data);
bool WriteFileBytes(const std::wstring& path, const void* data, size_t size);
bool EnsureDirectory(const std::wstring& path);

FILETIME UnixTimeToFileTime(uint64_t unixSeconds);
uint64_t FileTimeToUnixTime(const FILETIME& fileTime);
FILETIME NowFileTime();

std::string HexEncode(const void* data, size_t size);
bool HexDecode(const std::string& hex, void* data, size_t size);

uint32_t Fnv1a32(const void* data, size_t size);
uint64_t Fnv1a64(const void* data, size_t size);

// Copies with truncation and always terminates. Count is in characters.
void CopyStringW(wchar_t* destination, size_t destinationCount, const wchar_t* source);
void CopyStringA(char* destination, size_t destinationCount, const char* source);

// Squeezes a persona name into 15 characters. With asciiOnly every non-ASCII character becomes
// an underscore so a latin-1 font still draws a name.
std::string GamertagFromPersona(const char* personaUtf8, bool asciiOnly);

// Replaces characters that are not valid in a file name.
std::wstring SanitizeFileName(const std::wstring& name);

}
