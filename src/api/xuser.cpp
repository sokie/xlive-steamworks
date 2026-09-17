// #5261 - #5267, #5276, #5277, #5288 - #5293, #5303, #5314, #5377: user identity, contexts and
// properties.
#include "xlive/xfuncs.h"
#include "api/xsession.h"

#include "core/log.h"
#include "core/overlapped.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

// #5261
DWORD WINAPI XUserGetXUID(DWORD dwUserIndex, XUID* pXuid)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pXuid) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		*pXuid = INVALID_XUID;
		return ERROR_NOT_LOGGED_ON;
	}
	*pXuid = xls::UserXuid(dwUserIndex);
	return ERROR_SUCCESS;
}

// #5262
XUSER_SIGNIN_STATE WINAPI XUserGetSigninState(DWORD dwUserIndex)
{
	return xls::UserSigninState(dwUserIndex);
}

// #5263
DWORD WINAPI XUserGetName(DWORD dwUserIndex, char* szUserName, DWORD cchUserName)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!szUserName || !cchUserName) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		szUserName[0] = 0;
		return ERROR_NOT_LOGGED_ON;
	}
	xls::CopyStringA(szUserName, cchUserName, xls::UserName(dwUserIndex));
	return ERROR_SUCCESS;
}

// #5264
DWORD WINAPI XUserAreUsersFriends(DWORD dwUserIndex, const XUID* pXuids, DWORD cXuids, BOOL* pfResult, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pXuids || !cXuids || (!pfResult && !pOverlapped)) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	BOOL allFriends = xls::SteamReady() ? TRUE : FALSE;
	for (DWORD i = 0; i < cXuids && allFriends; i++) {
		CSteamID steamId = xls::SteamIdFromXuid(pXuids[i]);
		if (!steamId.IsValid() || !xls::SteamFriends()->HasFriend(steamId, k_EFriendFlagImmediate)) {
			allFriends = FALSE;
		}
	}
	if (pfResult) {
		*pfResult = allFriends;
	}
	return xls::OverlappedReturn(pOverlapped, allFriends ? ERROR_SUCCESS : ERROR_NOT_FOUND, allFriends);
}

// #5265
DWORD WINAPI XUserCheckPrivilege(DWORD dwUserIndex, XPRIVILEGE_TYPE privilegeType, BOOL* pfResult)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pfResult) {
		return ERROR_INVALID_PARAMETER;
	}
	if (!xls::UserSignedIn(dwUserIndex)) {
		*pfResult = FALSE;
		return ERROR_NOT_LOGGED_ON;
	}
	switch (privilegeType) {
		case XPRIVILEGE_MULTIPLAYER_SESSIONS:
		case XPRIVILEGE_MULTIPLAYER_DEDICATED_SERVER:
			*pfResult = xls::SteamOnline() ? TRUE : FALSE;
			break;
		default:
			*pfResult = TRUE;
			break;
	}
	return ERROR_SUCCESS;
}

// #5267
DWORD WINAPI XUserGetSigninInfo(DWORD dwUserIndex, DWORD dwFlags, XUSER_SIGNIN_INFO* pSigninInfo)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pSigninInfo) {
		return ERROR_INVALID_PARAMETER;
	}
	memset(pSigninInfo, 0, sizeof(*pSigninInfo));
	if (!xls::UserSignedIn(dwUserIndex)) {
		return ERROR_NOT_LOGGED_ON;
	}
	bool online = xls::UserOnline(dwUserIndex);
	XUID xuid = xls::UserXuid(dwUserIndex);
	pSigninInfo->UserSigninState = xls::UserSigninState(dwUserIndex);
	pSigninInfo->dwInfoFlags = online ? XUSER_INFO_FLAG_LIVE_ENABLED : 0;
	if (dwFlags & XUSER_GET_SIGNIN_INFO_ONLINE_XUID_ONLY) {
		pSigninInfo->xuid = online ? xuid : INVALID_XUID;
	}
	else if (dwFlags & XUSER_GET_SIGNIN_INFO_OFFLINE_XUID_ONLY) {
		pSigninInfo->xuid = online ? (XUID_OFFLINE_FLAG | (xuid & 0xFFFFFFFF)) : xuid;
	}
	else {
		pSigninInfo->xuid = xuid;
	}
	xls::CopyStringA(pSigninInfo->szUserName, sizeof(pSigninInfo->szUserName), xls::UserName(dwUserIndex));
	return ERROR_SUCCESS;
}

// #5276
void WINAPI XUserSetProperty(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, const void* pvValue)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex) || !pvValue || !cbValue) {
		return;
	}
	xls::UserSetProperty(dwUserIndex, dwPropertyId, pvValue, cbValue);
	xls::OnUserPropertyChanged(dwUserIndex, dwPropertyId);
}

// #5293
DWORD WINAPI XUserSetPropertyEx(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, const void* pvValue, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pvValue || !cbValue) {
		return ERROR_INVALID_PARAMETER;
	}
	xls::UserSetProperty(dwUserIndex, dwPropertyId, pvValue, cbValue);
	xls::OnUserPropertyChanged(dwUserIndex, dwPropertyId);
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5277
void WINAPI XUserSetContext(DWORD dwUserIndex, DWORD dwContextId, DWORD dwContextValue)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return;
	}
	xls::UserSetContext(dwUserIndex, dwContextId, dwContextValue);
	xls::OnUserContextChanged(dwUserIndex, dwContextId, dwContextValue);
}

// #5292
DWORD WINAPI XUserSetContextEx(DWORD dwUserIndex, DWORD dwContextId, DWORD dwContextValue, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	xls::UserSetContext(dwUserIndex, dwContextId, dwContextValue);
	xls::OnUserContextChanged(dwUserIndex, dwContextId, dwContextValue);
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5288
DWORD WINAPI XUserGetProperty(DWORD dwUserIndex, DWORD* pcbActual, XUSER_PROPERTY* pProperty, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pcbActual || !pProperty) {
		return ERROR_INVALID_PARAMETER;
	}
	xls::StoredProperty stored;
	if (!xls::UserGetProperty(dwUserIndex, pProperty->dwPropertyId, &stored)) {
		pProperty->value.type = XUSER_DATA_TYPE_NULL;
		return xls::OverlappedReturn(pOverlapped, ERROR_NOT_FOUND);
	}
	bool variable = stored.type == XUSER_DATA_TYPE_UNICODE || stored.type == XUSER_DATA_TYPE_BINARY;
	DWORD required = sizeof(XUSER_PROPERTY) + (variable ? stored.DataSize() : 0);
	if (*pcbActual < required) {
		*pcbActual = required;
		return ERROR_INSUFFICIENT_BUFFER;
	}
	*pcbActual = required;
	stored.Fill(pProperty->value);
	if (variable) {
		uint8_t* data = (uint8_t*)pProperty + sizeof(XUSER_PROPERTY);
		memcpy(data, stored.raw.data(), stored.raw.size());
		if (stored.type == XUSER_DATA_TYPE_UNICODE) {
			pProperty->value.string.pwszData = (LPWSTR)data;
		}
		else {
			pProperty->value.binary.pbData = data;
		}
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5289
DWORD WINAPI XUserGetContext(DWORD dwUserIndex, XUSER_CONTEXT* pContext, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pContext) {
		return ERROR_INVALID_PARAMETER;
	}
	DWORD value = 0;
	if (!xls::UserGetContext(dwUserIndex, pContext->dwContextId, &value)) {
		return xls::OverlappedReturn(pOverlapped, ERROR_NOT_FOUND);
	}
	pContext->dwValue = value;
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5290
FLOAT WINAPI XUserGetReputationStars(FLOAT fGamerRating)
{
	if (fGamerRating < 0.0f) return 0.0f;
	if (fGamerRating > 100.0f) return 5.0f;
	// Reputation is 0 to 100, the star display rounds to halves.
	float stars = fGamerRating / 20.0f;
	return (float)((int)(stars * 2.0f + 0.5f)) / 2.0f;
}

// #5314
DWORD WINAPI XUserMuteListQuery(DWORD dwUserIndex, XUID xuidRemoteTalker, BOOL* pfOnMuteList)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!pfOnMuteList) {
		return ERROR_INVALID_PARAMETER;
	}
	*pfOnMuteList = FALSE;
	CSteamID steamId = xls::SteamIdFromXuid(xuidRemoteTalker);
	if (xls::SteamReady() && steamId.IsValid()) {
		EFriendRelationship relationship = xls::SteamFriends()->GetFriendRelationship(steamId);
		*pfOnMuteList = (relationship == k_EFriendRelationshipIgnored || relationship == k_EFriendRelationshipIgnoredFriend) ? TRUE : FALSE;
	}
	return ERROR_SUCCESS;
}

// #5377
DWORD WINAPI XUserFindUsers(XUID xuidRequester, DWORD dwUsers, const FIND_USER_INFO* pUsers, DWORD cbResults, FIND_USERS_RESPONSE* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xuidRequester || !dwUsers || dwUsers > 100 || !pUsers || !pResults) {
		return ERROR_INVALID_PARAMETER;
	}
	DWORD required = sizeof(FIND_USERS_RESPONSE) + dwUsers * sizeof(FIND_USER_INFO);
	if (cbResults < required) {
		return ERROR_INSUFFICIENT_BUFFER;
	}
	pResults->dwResults = dwUsers;
	pResults->pUsers = (FIND_USER_INFO*)((uint8_t*)pResults + sizeof(FIND_USERS_RESPONSE));
	for (DWORD i = 0; i < dwUsers; i++) {
		FIND_USER_INFO& out = pResults->pUsers[i];
		out = pUsers[i];
		if (out.qwUserId) {
			// Steam has no gamertag search: a XUID resolves to a name, a name never resolves to a XUID.
			std::string name = xls::GamertagForXuid(out.qwUserId);
			xls::CopyStringA(out.szGamerTag, sizeof(out.szGamerTag), name.c_str());
		}
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5303
DWORD WINAPI XStringVerify(DWORD dwFlags, const char* szLocale, DWORD dwNumStrings, const STRING_DATA* pStringData, DWORD cbResults, STRING_VERIFY_RESPONSE* pResults, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!dwNumStrings || dwNumStrings > XSTRING_MAX_STRINGS || !pStringData || !pResults) {
		return ERROR_INVALID_PARAMETER;
	}
	DWORD required = sizeof(STRING_VERIFY_RESPONSE) + dwNumStrings * sizeof(HRESULT);
	if (cbResults < required) {
		return ERROR_INSUFFICIENT_BUFFER;
	}
	pResults->wNumStrings = (uint16_t)dwNumStrings;
	pResults->pStringResult = (HRESULT*)((uint8_t*)pResults + sizeof(STRING_VERIFY_RESPONSE));
	static bool filterReady = false;
	if (!filterReady && xls::SteamReady() && xls::SteamUtils()) {
		filterReady = xls::SteamUtils()->InitFilterText(0);
	}
	for (DWORD i = 0; i < dwNumStrings; i++) {
		HRESULT verdict = S_OK;
		if (pStringData[i].wStringSize > XSTRING_MAX_LENGTH) {
			verdict = XONLINE_E_STRING_TOO_LONG;
		}
		else if (filterReady && pStringData[i].pszString) {
			std::string utf8 = xls::WideToUtf8(pStringData[i].pszString);
			char filtered[XSTRING_MAX_LENGTH * 4] = {};
			int changed = xls::SteamUtils()->FilterText(k_ETextFilteringContextName, xls::SteamLocalId(), utf8.c_str(), filtered, sizeof(filtered));
			if (changed > 0) {
				verdict = XONLINE_E_STRING_OFFENSIVE_TEXT;
			}
		}
		pResults->pStringResult[i] = verdict;
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}
