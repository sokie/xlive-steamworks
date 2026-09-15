#include "core/overlapped.h"

#include "core/config.h"
#include "core/log.h"
#include "core/steam.h"

#include <list>
#include <mutex>

namespace xls {

namespace {

struct AsyncJob {
	XOVERLAPPED* overlapped;
	AsyncStep step;
	DWORD startedAt;
	bool running;
};

std::recursive_mutex g_mutex;
std::list<AsyncJob> g_jobs;

}

void OverlappedBegin(XOVERLAPPED* overlapped)
{
	if (!overlapped) {
		return;
	}
	overlapped->InternalLow = ERROR_IO_PENDING;
	overlapped->InternalHigh = 0;
	overlapped->dwExtendedError = 0;
	if (overlapped->hEvent) {
		ResetEvent(overlapped->hEvent);
	}
}

void OverlappedComplete(XOVERLAPPED* overlapped, DWORD result, DWORD_PTR internalHigh, DWORD extendedError)
{
	if (!overlapped) {
		return;
	}
	overlapped->InternalHigh = internalHigh;
	overlapped->dwExtendedError = extendedError ? extendedError : result;
	// InternalLow is what XHasOverlappedIoCompleted polls, so it is the last field written.
	MemoryBarrier();
	overlapped->InternalLow = result;
	if (overlapped->hEvent) {
		SetEvent(overlapped->hEvent);
	}
	if (overlapped->pCompletionRoutine) {
		overlapped->pCompletionRoutine(result, (DWORD)internalHigh, overlapped);
	}
}

DWORD OverlappedReturn(XOVERLAPPED* overlapped, DWORD result, DWORD_PTR internalHigh, DWORD extendedError)
{
	if (overlapped) {
		OverlappedBegin(overlapped);
		OverlappedComplete(overlapped, result, internalHigh, extendedError);
		return ERROR_IO_PENDING;
	}
	return result;
}

DWORD RunAsync(XOVERLAPPED* overlapped, AsyncStep step)
{
	if (overlapped) {
		OverlappedBegin(overlapped);
		std::lock_guard<std::recursive_mutex> lock(g_mutex);
		g_jobs.push_back(AsyncJob{ overlapped, std::move(step), GetTickCount(), false });
		return ERROR_IO_PENDING;
	}

	XOVERLAPPED local = {};
	OverlappedBegin(&local);
	DWORD deadline = GetTickCount() + Cfg().asyncTimeoutMs;
	while (true) {
		if (step(&local)) {
			break;
		}
		if ((int32_t)(GetTickCount() - deadline) > 0) {
			XLS_LOG_WARN("RunAsync: synchronous call timed out after %u ms.", Cfg().asyncTimeoutMs);
			OverlappedComplete(&local, ERROR_TIMEOUT);
			break;
		}
		SteamPumpForce();
		Sleep(1);
	}
	return (DWORD)local.InternalLow;
}

void AsyncPump()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	DWORD now = GetTickCount();
	for (auto it = g_jobs.begin(); it != g_jobs.end();) {
		AsyncJob& job = *it;
		if (job.running) {
			// A step re-entered the pump, leave it to the outer frame.
			++it;
			continue;
		}
		job.running = true;
		bool done = false;
		if (job.step(job.overlapped)) {
			done = true;
		}
		else if (now - job.startedAt > Cfg().asyncTimeoutMs) {
			XLS_LOG_WARN("AsyncPump: overlapped %p timed out after %u ms.", job.overlapped, Cfg().asyncTimeoutMs);
			OverlappedComplete(job.overlapped, ERROR_TIMEOUT);
			done = true;
		}
		if (done) {
			it = g_jobs.erase(it);
		}
		else {
			job.running = false;
			++it;
		}
	}
}

bool AsyncCancel(XOVERLAPPED* overlapped)
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (auto it = g_jobs.begin(); it != g_jobs.end(); ++it) {
		if (it->overlapped == overlapped) {
			g_jobs.erase(it);
			OverlappedComplete(overlapped, ERROR_CANCELLED);
			return true;
		}
	}
	return false;
}

bool AsyncWait(XOVERLAPPED* overlapped, DWORD timeoutMs)
{
	DWORD deadline = GetTickCount() + timeoutMs;
	while (!XHasOverlappedIoCompleted(overlapped)) {
		if (timeoutMs != INFINITE && (int32_t)(GetTickCount() - deadline) > 0) {
			return false;
		}
		SteamPumpForce();
		Sleep(1);
	}
	return true;
}

void AsyncShutdown()
{
	std::lock_guard<std::recursive_mutex> lock(g_mutex);
	for (AsyncJob& job : g_jobs) {
		OverlappedComplete(job.overlapped, ERROR_CANCELLED);
	}
	g_jobs.clear();
}

}
