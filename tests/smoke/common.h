// Shared by the smoke test and the two-machine pair test.
#pragma once

#include "xlive/xfuncs.h"
#include "xlive/xlive_steamworks.h"

#include <steam/steam_api.h>

#include <stdio.h>
#include <string>
#include <vector>

inline int g_failures = 0;
inline bool g_probe = false;

#define CHECK(condition, ...) \
	do { \
		bool ok_ = (condition); \
		printf("%s ", ok_ ? "[ ok ]" : "[FAIL]"); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		fflush(stdout); \
		if (!ok_) g_failures++; \
	} while (0)

#define INFO(...) \
	do { \
		printf("[info] "); \
		printf(__VA_ARGS__); \
		printf("\n"); \
		fflush(stdout); \
	} while (0)

inline void Pump(int frames)
{
	for (int i = 0; i < frames; i++) {
		XLiveRender();
		Sleep(10);
	}
}

inline DWORD Wait(XOVERLAPPED* overlapped, DWORD timeoutMs = 20000)
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

// Waits for a Steam call from the wrapper's pump since the tests have no callback loop of their own.
template <typename T>
bool WaitSteamCall(SteamAPICall_t call, T* result)
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

inline std::string Format(const char* format, ...)
{
	char buffer[1024];
	va_list args;
	va_start(args, format);
	vsnprintf(buffer, sizeof(buffer), format, args);
	va_end(args);
	return buffer;
}

inline std::string HexOf(const void* data, size_t size)
{
	static const char digits[] = "0123456789abcdef";
	std::string text;
	for (size_t i = 0; i < size; i++) {
		uint8_t byte = ((const uint8_t*)data)[i];
		text += digits[byte >> 4];
		text += digits[byte & 15];
	}
	return text;
}

inline bool UnhexTo(const std::string& text, void* out, size_t size)
{
	if (text.size() < size * 2) {
		return false;
	}
	for (size_t i = 0; i < size; i++) {
		((uint8_t*)out)[i] = (uint8_t)strtoul(text.substr(i * 2, 2).c_str(), nullptr, 16);
	}
	return true;
}

// Prints where a peer connection runs: direct or through which Steam relay.
inline void ReportTransport(const char* label, const XLS_CONNECTION_INFO& c, bool relayOnly)
{
	const char* path = c.fRelayed ? "RELAYED" : (c.fDirect ? "DIRECT" : "unknown");
	INFO("%s transport: %s%s%s%s%s, ping %u ms, quality %.2f local / %.2f remote, flags 0x%x", label, path,
		c.szRelayPop[0] ? " via relay " : "", c.szRelayPop, c.szRemoteAddr[0] ? " to " : "", c.szRemoteAddr,
		c.dwPingMs, c.flQualityLocal, c.flQualityRemote, c.dwFlags);
	if (relayOnly) {
		CHECK(c.fRelayed, "%s goes through a Steam relay in relay-only mode", label);
	}
	else {
		CHECK(c.fRelayed || c.fDirect, "%s has an established transport path", label);
	}
}

int RunPair(bool host, DWORD code, bool relayOnly, DWORD discoveryTimeoutSeconds);
