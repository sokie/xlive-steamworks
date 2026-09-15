// #651 - #653, #5270 Notifications.
#include "xlive/xfuncs.h"

#include "core/log.h"
#include "core/notify.h"
#include "core/steam.h"

// #651
BOOL WINAPI XNotifyGetNext(HANDLE hNotification, DWORD dwMsgFilter, DWORD* pdwId, ULONG_PTR* pParam)
{
	if (!hNotification || hNotification == INVALID_HANDLE_VALUE || !pdwId) {
		SetLastError(ERROR_INVALID_PARAMETER);
		return FALSE;
	}
	*pdwId = 0;
	// Titles call this every frame, which makes it the natural place to run Steam callbacks.
	xls::SteamPump();
	BOOL result = xls::NotifyGetNext(hNotification, dwMsgFilter, pdwId, pParam);
	if (result) {
		XLS_LOG_DEBUG("XNotifyGetNext: 0x%08x param 0x%p.", *pdwId, pParam ? (void*)*pParam : nullptr);
	}
	return result;
}

// #652
void WINAPI XNotifyPositionUI(DWORD dwPosition)
{
	XLS_TRACE_FN();
	xls::NotifySetUiPosition(dwPosition);
	if (xls::SteamReady() && xls::SteamUtils()) {
		ENotificationPosition position = k_EPositionBottomRight;
		if (dwPosition & XNOTIFYUI_POS_TOPCENTER) {
			position = (dwPosition & XNOTIFYUI_POS_CENTERLEFT) ? k_EPositionTopLeft : k_EPositionTopRight;
		}
		else {
			position = (dwPosition & XNOTIFYUI_POS_CENTERLEFT) ? k_EPositionBottomLeft : k_EPositionBottomRight;
		}
		xls::SteamUtils()->SetOverlayNotificationPosition(position);
	}
}

// #653
DWORD WINAPI XNotifyDelayUI(ULONG ulMilliSeconds)
{
	XLS_TRACE_FN();
	return ERROR_SUCCESS;
}

// #5270
HANDLE WINAPI XNotifyCreateListener(ULONGLONG qwAreas)
{
	XLS_TRACE_FN();
	HANDLE listener = xls::NotifyCreateListener(qwAreas);
	XLS_LOG_DEBUG("XNotifyCreateListener: areas 0x%llx -> %p.", qwAreas, listener);
	return listener;
}
