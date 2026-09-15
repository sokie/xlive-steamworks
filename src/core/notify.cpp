#include "core/notify.h"

#include "core/log.h"

#include <deque>
#include <map>
#include <mutex>

namespace xls {

namespace {

struct Listener {
	DWORD areas;
	// One queue per listener: every listener sees every notification for its areas.
	std::deque<std::pair<DWORD, ULONG_PTR>> pending;
};

std::mutex g_mutex;
std::map<HANDLE, Listener> g_listeners;
// Notifications raised before any listener for their area existed. A title creates its listener
// after XLiveInitialize, so the sign-in and connection notifications from init wait here.
std::map<DWORD, std::pair<DWORD, ULONG_PTR>> g_retained;
bool g_systemUiOpen = false;
DWORD g_uiPosition = XNOTIFYUI_POS_BOTTOMCENTER;

DWORD AreaOf(DWORD notificationId)
{
	switch (XNID_AREA(notificationId)) {
		case _XNAREA_SYSTEM: return XNOTIFY_SYSTEM;
		case _XNAREA_LIVE: return XNOTIFY_LIVE;
		case _XNAREA_FRIENDS: return XNOTIFY_FRIENDS;
		case _XNAREA_CUSTOM: return XNOTIFY_CUSTOM;
		case _XNAREA_XMP: return XNOTIFY_XMP;
		case _XNAREA_MSGR: return XNOTIFY_MSGR;
		case _XNAREA_PARTY: return XNOTIFY_PARTY;
		default: return 0;
	}
}

bool RetainWithoutListener(DWORD notificationId)
{
	switch (notificationId) {
		case XN_SYS_SIGNINCHANGED:
		case XN_SYS_UI:
		case XN_LIVE_CONNECTIONCHANGED:
		case XN_LIVE_INVITE_ACCEPTED:
			return true;
		default:
			return false;
	}
}

// The same notification twice in a row carries no new information, except for invites and
// custom actions where every instance matters.
bool Coalesce(DWORD notificationId)
{
	switch (notificationId) {
		case XN_LIVE_INVITE_ACCEPTED:
		case XN_CUSTOM_ACTIONPRESSED:
		case XN_CUSTOM_GAMERCARD:
			return false;
		default:
			return true;
	}
}

}

void NotifyInit()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	g_listeners.clear();
	g_retained.clear();
	g_systemUiOpen = false;
}

void NotifyShutdown()
{
	std::lock_guard<std::mutex> lock(g_mutex);
	for (auto& entry : g_listeners) {
		CloseHandle(entry.first);
	}
	g_listeners.clear();
	g_retained.clear();
}

void NotifyPost(DWORD notificationId, ULONG_PTR parameter)
{
	DWORD area = AreaOf(notificationId);
	if (!area) {
		return;
	}
	if (notificationId == XN_SYS_UI) {
		g_systemUiOpen = parameter != 0;
	}

	std::lock_guard<std::mutex> lock(g_mutex);
	bool delivered = false;
	for (auto& entry : g_listeners) {
		Listener& listener = entry.second;
		if (!(listener.areas & area)) {
			continue;
		}
		if (Coalesce(notificationId)) {
			bool replaced = false;
			for (auto& queued : listener.pending) {
				if (queued.first == notificationId) {
					queued.second = parameter;
					replaced = true;
					break;
				}
			}
			if (!replaced) {
				listener.pending.emplace_back(notificationId, parameter);
			}
		}
		else {
			listener.pending.emplace_back(notificationId, parameter);
		}
		SetEvent(entry.first);
		delivered = true;
	}
	if (!delivered && RetainWithoutListener(notificationId)) {
		g_retained[notificationId] = std::make_pair(area, parameter);
	}
	XLS_LOG_DEBUG("notify: 0x%08x param 0x%p %s.", notificationId, (void*)parameter, delivered ? "queued" : "no listener");
}

HANDLE NotifyCreateListener(ULONGLONG areas)
{
	DWORD requested = (DWORD)areas;
	if (!requested) {
		requested = XNOTIFY_ALL;
	}
	HANDLE handle = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!handle) {
		return nullptr;
	}
	std::lock_guard<std::mutex> lock(g_mutex);
	Listener& listener = g_listeners[handle];
	listener.areas = requested;
	listener.pending.clear();
	for (auto it = g_retained.begin(); it != g_retained.end();) {
		if (it->second.first & requested) {
			listener.pending.emplace_back(it->first, it->second.second);
			it = g_retained.erase(it);
		}
		else {
			++it;
		}
	}
	if (!listener.pending.empty()) {
		SetEvent(handle);
	}
	return handle;
}

bool NotifyCloseListener(HANDLE listener)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_listeners.find(listener);
	if (it == g_listeners.end()) {
		return false;
	}
	g_listeners.erase(it);
	CloseHandle(listener);
	return true;
}

BOOL NotifyGetNext(HANDLE listener, DWORD filter, DWORD* notificationId, ULONG_PTR* parameter)
{
	std::lock_guard<std::mutex> lock(g_mutex);
	auto it = g_listeners.find(listener);
	if (it == g_listeners.end()) {
		return FALSE;
	}
	Listener& state = it->second;
	for (auto queued = state.pending.begin(); queued != state.pending.end(); ++queued) {
		if (filter && queued->first != filter) {
			continue;
		}
		*notificationId = queued->first;
		if (parameter) {
			*parameter = queued->second;
		}
		state.pending.erase(queued);
		if (state.pending.empty()) {
			ResetEvent(listener);
		}
		return TRUE;
	}
	if (state.pending.empty()) {
		ResetEvent(listener);
	}
	return FALSE;
}

void NotifySystemUi(bool open)
{
	if (g_systemUiOpen == open) {
		return;
	}
	NotifyPost(XN_SYS_UI, open ? TRUE : FALSE);
}

bool NotifySystemUiOpen()
{
	return g_systemUiOpen;
}

void NotifySetUiPosition(DWORD position)
{
	g_uiPosition = position & XNOTIFYUI_POS_MASK;
}

DWORD NotifyUiPosition()
{
	return g_uiPosition;
}

}
