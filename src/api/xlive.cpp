// #5000 - #5039 lifetime and misc, #1082, #1083, #5251, #5254 - #5259.
#include "xlive/xfuncs.h"
#include "api/xlive.h"
#include "api/xsession.h"

#include "core/config.h"
#include "core/enumerator.h"
#include "core/log.h"
#include "core/net.h"
#include "core/notify.h"
#include "core/overlapped.h"
#include "core/spa.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

namespace {

bool g_initialised = false;
bool g_configLoaded = false;
uint32_t g_titleId = 0;
uint32_t g_titleVersion = 0;
XLIVE_DEBUG_LEVEL g_debugLevel = XLIVE_DEBUG_LEVEL_DEFAULT;

}

namespace xls {

bool Initialised() { return g_initialised; }
uint32_t TitleId() { return g_titleId; }
uint32_t TitleVersion() { return g_titleVersion; }

}

// #5000
HRESULT WINAPI XLiveInitialize(XLIVE_INITIALIZE_INFO* pXii)
{
	return XLiveInitializeEx(pXii, 0);
}

// #5297
HRESULT WINAPI XLiveInitializeEx(XLIVE_INITIALIZE_INFO* pXii, DWORD dwTitleXLiveVersion)
{
	if (g_initialised) {
		return S_OK;
	}
	if (!g_configLoaded) {
		xls::ConfigLoad();
		g_configLoaded = true;
	}
	XLS_LOG_INFO("xlive-steamworks %u.%u.%u (built %s %s) initialising (title xlive version 0x%08x, flags 0x%08x).", XLS_VERSION_MAJOR, XLS_VERSION_MINOR, XLS_VERSION_PATCH, __DATE__, __TIME__, dwTitleXLiveVersion, pXii ? pXii->dwFlags : 0);

	xls::SteamStart();

	uint8_t language = xls::Cfg().language ? xls::Cfg().language : xls::SteamLanguageAsXLanguage();
	xls::spa::Load(language);
	g_titleId = xls::Cfg().titleId ? xls::Cfg().titleId : xls::spa::TitleId();
	g_titleVersion = xls::Cfg().titleVersion;
	XLS_LOG_INFO("title id 0x%08x, language %u.", g_titleId, language);

	xls::NotifyInit();
	xls::UsersInit();
	xls::NetInit();
	xls::SessionsInit();

	if (xls::SteamReady()) {
		xls::NotifyPost(XN_SYS_SIGNINCHANGED, 1);
		xls::NotifyPost(XN_LIVE_CONNECTIONCHANGED, (ULONG_PTR)(xls::SteamOnline() ? XONLINE_S_LOGON_CONNECTION_ESTABLISHED : XONLINE_E_LOGON_NO_NETWORK_CONNECTION));
	}
	else {
		xls::NotifyPost(XN_LIVE_CONNECTIONCHANGED, (ULONG_PTR)XONLINE_E_LOGON_NO_NETWORK_CONNECTION);
	}

	g_initialised = true;
	xls::SessionCheckLaunchInvite();
	return S_OK;
}

// #5001
HRESULT WINAPI XLiveInput(XLIVE_INPUT_INFO* pXii)
{
	if (!pXii) {
		return E_POINTER;
	}
	pXii->fHandled = FALSE;
	xls::SteamPump();
	return S_OK;
}

// #5002
HRESULT WINAPI XLiveRender()
{
	xls::SteamPump();
	return S_OK;
}

// #5003
void WINAPI XLiveUninitialize()
{
	XLS_TRACE_FN();
	if (!g_initialised) {
		return;
	}
	g_initialised = false;
	xls::SessionsShutdown();
	xls::AsyncShutdown();
	xls::EnumeratorShutdown();
	xls::NetShutdown();
	xls::NotifyShutdown();
	xls::UsersShutdown();
	xls::SteamStop();
	XLS_LOG_INFO("uninitialised.");
	xls::LogShutdown();
}

// #5005
HRESULT WINAPI XLiveOnCreateDevice(IUnknown* pD3D, void* pD3DPP)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5006
HRESULT WINAPI XLiveOnDestroyDevice()
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5007
HRESULT WINAPI XLiveOnResetDevice(void* pD3DPP)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5010
HRESULT WINAPI XLiveRegisterDataSection(LPCWSTR lpszName, uint8_t* pbData, DWORD cbData)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5011
HRESULT WINAPI XLiveUnregisterDataSection(LPCWSTR lpszName)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5012
HRESULT WINAPI XLiveUpdateHashes(DWORD dwUnknown1, DWORD dwUnknown2)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5022
HRESULT WINAPI XLiveGetUpdateInformation(XLIVEUPDATE_INFORMATION* pInfo)
{
	XLS_TRACE_FN();
	if (!pInfo || pInfo->cbSize != sizeof(XLIVEUPDATE_INFORMATION)) {
		return E_INVALIDARG;
	}
	// Steam delivers updates before launch, so there is never one pending.
	return S_FALSE;
}

// #5024
HRESULT WINAPI XLiveUpdateSystem(LPCWSTR lpszRelaunchCmdLine)
{
	XLS_TRACE_FN();
	return S_FALSE;
}

// #5025
HRESULT WINAPI XLiveGetLiveIdError(HRESULT* phrAuthState, HRESULT* phrRequestState, LPWSTR lpszUrl, DWORD* pcchUrl)
{
	XLS_TRACE_FN();
	if (phrAuthState) *phrAuthState = S_OK;
	if (phrRequestState) *phrRequestState = S_OK;
	if (pcchUrl) {
		if (lpszUrl && *pcchUrl) {
			lpszUrl[0] = 0;
		}
		*pcchUrl = 0;
	}
	return S_OK;
}

// #5026
HRESULT WINAPI XLiveSetSponsorToken(LPCWSTR lpszToken, DWORD dwTitleId)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5027
HRESULT WINAPI XLiveUninstallTitle(DWORD dwTitleId)
{
	XLS_TRACE_FN();
	return S_OK;
}

// #5028
DWORD WINAPI XLiveLoadLibraryEx(LPCWSTR lpszModuleFileName, HINSTANCE* phModule, DWORD dwFlags)
{
	XLS_TRACE_FN();
	if (!lpszModuleFileName || !phModule) {
		return ERROR_INVALID_PARAMETER;
	}
	*phModule = LoadLibraryExW(lpszModuleFileName, nullptr, dwFlags);
	return *phModule ? ERROR_SUCCESS : GetLastError();
}

// #5029
HRESULT WINAPI XLiveFreeLibrary(HMODULE hModule)
{
	XLS_TRACE_FN();
	if (!hModule) {
		return E_HANDLE;
	}
	return FreeLibrary(hModule) ? S_OK : HRESULT_FROM_WIN32(GetLastError());
}

// #5030
BOOL WINAPI XLivePreTranslateMessage(const MSG* pMsg)
{
	xls::SteamPump();
	return FALSE;
}

// #5031
HRESULT WINAPI XLiveSetDebugLevel(XLIVE_DEBUG_LEVEL xdlLevel, XLIVE_DEBUG_LEVEL* pxdlOldLevel)
{
	XLS_TRACE_FN();
	if (pxdlOldLevel) {
		*pxdlOldLevel = g_debugLevel;
	}
	g_debugLevel = xdlLevel;
	return S_OK;
}

// #5032
HRESULT WINAPI XLiveVerifyArcadeLicense(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset)
{
	XLS_TRACE_FN();
	// Steam owns the licence check
	return S_OK;
}

// #1082
DWORD WINAPI XGetOverlappedExtendedError(XOVERLAPPED* pOverlapped)
{
	if (!pOverlapped) {
		return GetLastError();
	}
	if (pOverlapped->InternalLow != ERROR_IO_PENDING) {
		return pOverlapped->dwExtendedError;
	}
	return ERROR_IO_INCOMPLETE;
}

// #1083
DWORD WINAPI XGetOverlappedResult(XOVERLAPPED* pOverlapped, DWORD* pResult, BOOL bWait)
{
	if (!pOverlapped) {
		return ERROR_INVALID_PARAMETER;
	}
	if (pOverlapped->InternalLow == ERROR_IO_PENDING) {
		xls::SteamPump();
	}
	if (pOverlapped->InternalLow == ERROR_IO_PENDING) {
		if (!bWait) {
			return ERROR_IO_INCOMPLETE;
		}
		if (!xls::AsyncWait(pOverlapped, INFINITE)) {
			return ERROR_IO_INCOMPLETE;
		}
	}
	if (pResult) {
		*pResult = (DWORD)pOverlapped->InternalHigh;
	}
	return (DWORD)pOverlapped->InternalLow;
}

// #5254
DWORD WINAPI XCancelOverlapped(XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!pOverlapped) {
		return ERROR_INVALID_PARAMETER;
	}
	xls::AsyncCancel(pOverlapped);
	return ERROR_SUCCESS;
}

// #5251
BOOL WINAPI XCloseHandle(HANDLE hObject)
{
	XLS_TRACE_FN();
	if (!hObject || hObject == INVALID_HANDLE_VALUE) {
		SetLastError(ERROR_INVALID_HANDLE);
		return FALSE;
	}
	if (xls::EnumeratorClose(hObject)) {
		return TRUE;
	}
	if (xls::NotifyCloseListener(hObject)) {
		return TRUE;
	}
	if (xls::SessionCloseHandle(hObject)) {
		return TRUE;
	}
	return CloseHandle(hObject);
}

// #5256
DWORD WINAPI XEnumerate(HANDLE hEnum, void* pvBuffer, DWORD cbBuffer, DWORD* pcItemsReturned, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!hEnum || !pvBuffer || !cbBuffer) {
		return ERROR_INVALID_PARAMETER;
	}
	if ((pcItemsReturned && pOverlapped) || (!pcItemsReturned && !pOverlapped)) {
		return ERROR_INVALID_PARAMETER;
	}
	xls::Enumerator* enumerator = xls::EnumeratorFind(hEnum);
	if (!enumerator) {
		return ERROR_INVALID_HANDLE;
	}
	memset(pvBuffer, 0, cbBuffer);
	return xls::RunAsync(pOverlapped, [hEnum, pvBuffer, cbBuffer, pcItemsReturned](XOVERLAPPED* overlapped) {
		xls::Enumerator* current = xls::EnumeratorFind(hEnum);
		if (!current) {
			xls::OverlappedComplete(overlapped, ERROR_INVALID_HANDLE);
			return true;
		}
		if (!current->Ready()) {
			return false;
		}
		DWORD count = 0;
		DWORD result = current->Next(pvBuffer, cbBuffer, &count);
		if (pcItemsReturned) {
			*pcItemsReturned = count;
		}
		xls::OverlappedComplete(overlapped, result, count);
		return true;
	});
}

// #5255
DWORD WINAPI XEnumerateBack(HANDLE hEnum, void* pvBuffer, DWORD cbBuffer, DWORD* pcItemsReturned, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	return XEnumerate(hEnum, pvBuffer, cbBuffer, pcItemsReturned, pOverlapped);
}

// #5257
HRESULT WINAPI XLiveManageCredentials(LPCWSTR lpszLiveIdName, LPCWSTR lpszLiveIdPassword, DWORD dwFlags, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS) == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : S_OK;
}

// #5258
HRESULT WINAPI XLiveSignout(XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	// The Steam user cannot sign out of the game so this is basically a NOP
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS) == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : S_OK;
}

// #5259
HRESULT WINAPI XLiveSignin(LPCWSTR lpszLiveIdName, LPCWSTR lpszLiveIdPassword, DWORD dwFlags, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (xls::SteamReady()) {
		xls::NotifyPost(XN_SYS_SIGNINCHANGED, 1);
	}
	return xls::OverlappedReturn(pOverlapped, xls::SteamReady() ? ERROR_SUCCESS : (DWORD)XONLINE_E_LOGON_NO_NETWORK_CONNECTION) == ERROR_IO_PENDING ? HRESULT_FROM_WIN32(ERROR_IO_PENDING) : (xls::SteamReady() ? S_OK : XONLINE_E_LOGON_NO_NETWORK_CONNECTION);
}
