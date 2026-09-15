// XOVERLAPPED completion and polled Steam calls. Nothing completes from a Steam callback thread.
#pragma once

#include "xlive/xdefs.h"

#include <functional>

namespace xls {

void OverlappedBegin(XOVERLAPPED* overlapped);

void OverlappedComplete(XOVERLAPPED* overlapped, DWORD result, DWORD_PTR internalHigh = 0, DWORD extendedError = 0);

// The common shape of a call whose work is already done: complete the overlapped and report
// ERROR_IO_PENDING when the title gave one, else hand the result straight back.
DWORD OverlappedReturn(XOVERLAPPED* overlapped, DWORD result, DWORD_PTR internalHigh = 0, DWORD extendedError = 0);

// A step function for work that finishes later. It is called from the pump until it returns true.
// It must call OverlappedComplete on the overlapped it was given before returning true.
using AsyncStep = std::function<bool(XOVERLAPPED*)>;

// Runs an asynchronous job. With an overlapped it is queued and ERROR_IO_PENDING is returned.
// Without one this pumps Steam until the job completes (or the configured timeout passes) and
// returns the job's result, which makes the synchronous form of every X* call work.
DWORD RunAsync(XOVERLAPPED* overlapped, AsyncStep step);

void AsyncPump();

// Fails a queued job without running it further.
bool AsyncCancel(XOVERLAPPED* overlapped);

bool AsyncWait(XOVERLAPPED* overlapped, DWORD timeoutMs);

void AsyncShutdown();

}
