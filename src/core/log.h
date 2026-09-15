#pragma once

#include <string>

namespace xls {

enum class LogLevel {
	Off = 0,
	Error,
	Warn,
	Info,
	Debug,
	Trace,
};

void LogInit(const std::wstring& path, LogLevel level, bool toDebugger);
void LogShutdown();
void LogSetLevel(LogLevel level);
LogLevel LogGetLevel();
void LogWrite(LogLevel level, const char* format, ...);

}

#define XLS_LOG_ERROR(...) ::xls::LogWrite(::xls::LogLevel::Error, __VA_ARGS__)
#define XLS_LOG_WARN(...)  ::xls::LogWrite(::xls::LogLevel::Warn, __VA_ARGS__)
#define XLS_LOG_INFO(...)  ::xls::LogWrite(::xls::LogLevel::Info, __VA_ARGS__)
#define XLS_LOG_DEBUG(...) ::xls::LogWrite(::xls::LogLevel::Debug, __VA_ARGS__)
#define XLS_LOG_TRACE(...) ::xls::LogWrite(::xls::LogLevel::Trace, __VA_ARGS__)
#define XLS_TRACE_FN()     ::xls::LogWrite(::xls::LogLevel::Trace, "%s", __func__)
