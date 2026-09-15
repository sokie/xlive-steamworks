// #472 - #479 Guide custom actions: the Steam overlay has no gamer card hook, so none is ever pressed.
#include "xlive/xfuncs.h"

#include "core/log.h"

// #472
void WINAPI XCustomSetAction(DWORD dwActionIndex, LPCWSTR lpszActionText, DWORD dwFlags)
{
	XLS_TRACE_FN();
}

// #473
BOOL WINAPI XCustomGetLastActionPress(DWORD* pdwUserIndex, DWORD* pdwActionIndex, XUID* pXuid)
{
	XLS_TRACE_FN();
	return FALSE;
}

// #474
DWORD WINAPI XCustomSetDynamicActions(DWORD dwUserIndex, XUID xuid, const XCUSTOMACTION* pCustomActions, WORD cCustomActions)
{
	XLS_TRACE_FN();
	return ERROR_SUCCESS;
}

// #476
DWORD WINAPI XCustomGetLastActionPressEx(DWORD* pdwUserIndex, DWORD* pdwActionId, XUID* pXuid, uint8_t* pbPayload, WORD* pwPayloadSize)
{
	XLS_TRACE_FN();
	if (pwPayloadSize) {
		*pwPayloadSize = 0;
	}
	return ERROR_NO_MORE_ITEMS;
}

// #477
void WINAPI XCustomRegisterDynamicActions()
{
	XLS_TRACE_FN();
}

// #478
void WINAPI XCustomUnregisterDynamicActions()
{
	XLS_TRACE_FN();
}

// #479
BOOL WINAPI XCustomGetCurrentGamercard(DWORD* pdwUserIndex, XUID* pXuid)
{
	XLS_TRACE_FN();
	return FALSE;
}
