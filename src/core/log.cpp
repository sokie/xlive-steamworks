#include "core/log.h"

#include <windows.h>
#include <stdarg.h>
#include <stdio.h>
#include <mutex>

namespace xls {

namespace {

std::mutex g_mutex;
FILE* g_file = nullptr;
LogLevel g_level = LogLevel::Off;
bool g_toDebugger = false;

const char* LevelTag(LogLevel level)
{
	switch (level) {
		case LogLevel::Error: return "ERROR";
		case LogLevel::Warn: return "WARN ";
		case LogLevel::Info: return "INFO ";
		case LogLevel::Debug: return "DEBUG";
		case LogLevel::Trace: return "TRACE";
		default: return "     ";
	}
}

}

void LogInit(const std::wstring& path, LogLevel level, bool toDebugger)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_level = level;
	g_toDebugger = toDebugger;
	if (g_file) {
		fclose(g_file);
		g_file = nullptr;
	}
	if (level != LogLevel::Off && !path.empty()) {
		g_file = _wfopen(path.c_str(), L"w");
	}
}

void LogShutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file) {
		fclose(g_file);
		g_file = nullptr;
	}
}

void LogSetLevel(LogLevel level)
{
	g_level = level;
}

LogLevel LogGetLevel()
{
	return g_level;
}

void LogWrite(LogLevel level, const char* format, ...)
{
	if (level > g_level || level == LogLevel::Off) {
		return;
	}
	if (!g_file && !g_toDebugger) {
		return;
	}

	char message[4096];
	va_list args;
	va_start(args, format);
	int written = vsnprintf(message, sizeof(message), format, args);
	va_end(args);
	if (written < 0) {
		return;
	}

	SYSTEMTIME now;
	GetLocalTime(&now);

	std::lock_guard<std::mutex> lock(g_mutex);
	if (g_file) {
		fprintf(g_file, "%02u:%02u:%02u.%03u [%s] [%05lu] %s\n", now.wHour, now.wMinute, now.wSecond, now.wMilliseconds, LevelTag(level), GetCurrentThreadId(), message);
		fflush(g_file);
	}
	if (g_toDebugger) {
		char line[4200];
		snprintf(line, sizeof(line), "xlive-steamworks [%s] %s\n", LevelTag(level), message);
		OutputDebugStringA(line);
	}
}

}
