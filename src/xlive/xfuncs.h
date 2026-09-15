// Prototypes of every export in xlive_exports.def, with the ordinal a title imports it by.
#pragma once

#include "xlive/xdefs.h"

// #1 - #40 Winsock.
int WINAPI XWSAStartup(WORD wVersionRequested, WSADATA* lpWSAData);
int WINAPI XWSACleanup();
SOCKET WINAPI XSocketCreate(int af, int type, int protocol);
int WINAPI XSocketClose(SOCKET s);
int WINAPI XSocketShutdown(SOCKET s, int how);
int WINAPI XSocketIOCTLSocket(SOCKET s, long cmd, u_long* argp);
int WINAPI XSocketSetSockOpt(SOCKET s, int level, int optname, const char* optval, int optlen);
int WINAPI XSocketGetSockOpt(SOCKET s, int level, int optname, char* optval, int* optlen);
int WINAPI XSocketGetSockName(SOCKET s, sockaddr* name, int* namelen);
int WINAPI XSocketGetPeerName(SOCKET s, sockaddr* name, int* namelen);
int WINAPI XSocketBind(SOCKET s, const sockaddr* name, int namelen);
int WINAPI XSocketConnect(SOCKET s, const sockaddr* name, int namelen);
int WINAPI XSocketListen(SOCKET s, int backlog);
SOCKET WINAPI XSocketAccept(SOCKET s, sockaddr* addr, int* addrlen);
int WINAPI XSocketSelect(int nfds, fd_set* readfds, fd_set* writefds, fd_set* exceptfds, const timeval* timeout);
BOOL WINAPI XWSAGetOverlappedResult(SOCKET s, WSAOVERLAPPED* lpOverlapped, DWORD* lpcbTransfer, BOOL fWait, DWORD* lpdwFlags);
int WINAPI XWSACancelOverlappedIO(SOCKET s);
int WINAPI XSocketRecv(SOCKET s, char* buf, int len, int flags);
int WINAPI XWSARecv(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesRecvd, DWORD* lpFlags, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
int WINAPI XSocketRecvFrom(SOCKET s, char* buf, int len, int flags, sockaddr* from, int* fromlen);
int WINAPI XWSARecvFrom(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesRecvd, DWORD* lpFlags, sockaddr* lpFrom, int* lpFromlen, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
int WINAPI XSocketSend(SOCKET s, const char* buf, int len, int flags);
int WINAPI XWSASend(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesSent, DWORD dwFlags, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
int WINAPI XSocketSendTo(SOCKET s, const char* buf, int len, int flags, const sockaddr* to, int tolen);
int WINAPI XWSASendTo(SOCKET s, WSABUF* lpBuffers, DWORD dwBufferCount, DWORD* lpNumberOfBytesSent, DWORD dwFlags, const sockaddr* lpTo, int iTolen, WSAOVERLAPPED* lpOverlapped, LPWSAOVERLAPPED_COMPLETION_ROUTINE lpCompletionRoutine);
unsigned long WINAPI XSocketInet_Addr(const char* cp);
int WINAPI XSocketWSAGetLastError();
void WINAPI XWSASetLastError(int iError);
WSAEVENT WINAPI XWSACreateEvent();
BOOL WINAPI XWSACloseEvent(WSAEVENT hEvent);
BOOL WINAPI XWSASetEvent(WSAEVENT hEvent);
BOOL WINAPI XWSAResetEvent(WSAEVENT hEvent);
DWORD WINAPI XWSAWaitForMultipleEvents(DWORD cEvents, const WSAEVENT* lphEvents, BOOL fWaitAll, DWORD dwTimeout, BOOL fAlertable);
int WINAPI XWSAFDIsSet(SOCKET fd, fd_set* set);
int WINAPI XWSAEventSelect(SOCKET s, WSAEVENT hEventObject, long lNetworkEvents);
u_long WINAPI XSocketHTONL(u_long hostlong);
u_short WINAPI XSocketNTOHS(u_short netshort);
u_long WINAPI XSocketNTOHL(u_long netlong);
u_short WINAPI XSocketHTONS(u_short hostshort);

// #51 - #84 XNet.
INT WINAPI XNetStartup(const XNetStartupParams* pxnsp);
INT WINAPI XNetCleanup();
INT WINAPI XNetRandom(uint8_t* pb, UINT cb);
INT WINAPI XNetCreateKey(XNKID* pxnkid, XNKEY* pxnkey);
INT WINAPI XNetRegisterKey(const XNKID* pxnkid, const XNKEY* pxnkey);
INT WINAPI XNetUnregisterKey(const XNKID* pxnkid);
INT WINAPI XNetXnAddrToInAddr(const XNADDR* pxna, const XNKID* pxnkid, IN_ADDR* pina);
INT WINAPI XNetServerToInAddr(const IN_ADDR ina, DWORD dwServiceId, IN_ADDR* pina);
INT WINAPI XNetTsAddrToInAddr(const TSADDR* ptsa, DWORD dwServiceId, const XNKID* pxnkid, IN_ADDR* pina);
INT WINAPI XNetInAddrToXnAddr(const IN_ADDR ina, XNADDR* pxna, XNKID* pxnkid);
INT WINAPI XNetInAddrToServer(const IN_ADDR ina, IN_ADDR* pina);
INT WINAPI XNetInAddrToString(const IN_ADDR ina, char* pchBuf, INT cchBuf);
INT WINAPI XNetUnregisterInAddr(const IN_ADDR ina);
INT WINAPI XNetXnAddrToMachineId(const XNADDR* pxnaddr, uint64_t* pqwMachineId);
INT WINAPI XNetConnect(const IN_ADDR ina);
INT WINAPI XNetGetConnectStatus(const IN_ADDR ina);
INT WINAPI XNetDnsLookup(const char* pszHost, WSAEVENT hEvent, XNDNS** ppxndns);
INT WINAPI XNetDnsRelease(XNDNS* pxndns);
INT WINAPI XNetQosListen(const XNKID* pxnkid, const uint8_t* pb, UINT cb, DWORD dwBitsPerSec, DWORD dwFlags);
INT WINAPI XNetQosLookup(UINT cxnqos, const XNADDR* apxna[], const XNKID* apxnkid[], const XNKEY* apxnkey[], UINT cina, const IN_ADDR aina[], const DWORD adwServiceId[], UINT cProbes, DWORD dwBitsPerSec, DWORD dwFlags, WSAEVENT hEvent, XNQOS** ppxnqos);
INT WINAPI XNetQosServiceLookup(DWORD dwFlags, WSAEVENT hEvent, XNQOS** ppxnqos);
INT WINAPI XNetQosRelease(XNQOS* pxnqos);
DWORD WINAPI XNetGetTitleXnAddr(XNADDR* pxna);
DWORD WINAPI XNetGetDebugXnAddr(XNADDR* pxna);
DWORD WINAPI XNetGetEthernetLinkStatus();
DWORD WINAPI XNetGetBroadcastVersionStatus(BOOL fReset);
INT WINAPI XNetQosGetListenStats(const XNKID* pxnkid, XNQOSLISTENSTATS* pQosListenStats);
INT WINAPI XNetGetOpt(DWORD dwOptId, uint8_t* pbValue, DWORD* pdwValueSize);
INT WINAPI XNetSetOpt(DWORD dwOptId, const uint8_t* pbValue, DWORD dwValueSize);
INT WINAPI XNetStartupEx(const XNetStartupParams* pxnsp, DWORD dwVersionReq);
INT WINAPI XNetReplaceKey(const XNKID* pxnkidUnregister, const XNKID* pxnkidReplace);
INT WINAPI XNetGetXnAddrPlatform(const XNADDR* pxnaddr, DWORD* pdwPlatform);
INT WINAPI XNetGetSystemLinkPort(WORD* pwSystemLinkPort);
INT WINAPI XNetSetSystemLinkPort(WORD wSystemLinkPort);

// #472 - #479 Guide custom actions.
void WINAPI XCustomSetAction(DWORD dwActionIndex, LPCWSTR lpszActionText, DWORD dwFlags);
BOOL WINAPI XCustomGetLastActionPress(DWORD* pdwUserIndex, DWORD* pdwActionIndex, XUID* pXuid);
DWORD WINAPI XCustomSetDynamicActions(DWORD dwUserIndex, XUID xuid, const XCUSTOMACTION* pCustomActions, WORD cCustomActions);
DWORD WINAPI XCustomGetLastActionPressEx(DWORD* pdwUserIndex, DWORD* pdwActionId, XUID* pXuid, uint8_t* pbPayload, WORD* pwPayloadSize);
void WINAPI XCustomRegisterDynamicActions();
void WINAPI XCustomUnregisterDynamicActions();
BOOL WINAPI XCustomGetCurrentGamercard(DWORD* pdwUserIndex, XUID* pXuid);

// #651 - #653, #5270 Notifications.
BOOL WINAPI XNotifyGetNext(HANDLE hNotification, DWORD dwMsgFilter, DWORD* pdwId, ULONG_PTR* pParam);
void WINAPI XNotifyPositionUI(DWORD dwPosition);
DWORD WINAPI XNotifyDelayUI(ULONG ulMilliSeconds);
HANDLE WINAPI XNotifyCreateListener(ULONGLONG qwAreas);

// #1082, #1083, #5254 Overlapped.
DWORD WINAPI XGetOverlappedExtendedError(XOVERLAPPED* pOverlapped);
DWORD WINAPI XGetOverlappedResult(XOVERLAPPED* pOverlapped, DWORD* pResult, BOOL bWait);
DWORD WINAPI XCancelOverlapped(XOVERLAPPED* pOverlapped);

// #5000 - #5039 XLive core.
HRESULT WINAPI XLiveInitialize(XLIVE_INITIALIZE_INFO* pXii);
HRESULT WINAPI XLiveInput(XLIVE_INPUT_INFO* pXii);
HRESULT WINAPI XLiveRender();
void WINAPI XLiveUninitialize();
HRESULT WINAPI XLiveOnCreateDevice(IUnknown* pD3D, void* pD3DPP);
HRESULT WINAPI XLiveOnDestroyDevice();
HRESULT WINAPI XLiveOnResetDevice(void* pD3DPP);
HRESULT WINAPI XHVCreateEngine(XHV_INIT_PARAMS* pParams, HANDLE* phWorkerThread, IXHVEngine** ppEngine);
HRESULT WINAPI XLiveRegisterDataSection(LPCWSTR lpszName, uint8_t* pbData, DWORD cbData);
HRESULT WINAPI XLiveUnregisterDataSection(LPCWSTR lpszName);
HRESULT WINAPI XLiveUpdateHashes(DWORD dwUnknown1, DWORD dwUnknown2);
HRESULT WINAPI XLivePBufferAllocate(DWORD dwSize, XLIVE_PROTECTED_BUFFER** ppBuffer);
HRESULT WINAPI XLivePBufferFree(XLIVE_PROTECTED_BUFFER* pBuffer);
HRESULT WINAPI XLivePBufferGetByte(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, uint8_t* pbValue);
HRESULT WINAPI XLivePBufferSetByte(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, uint8_t bValue);
HRESULT WINAPI XLivePBufferGetDWORD(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, DWORD* pdwValue);
HRESULT WINAPI XLivePBufferSetDWORD(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, DWORD dwValue);
HRESULT WINAPI XLiveGetUpdateInformation(XLIVEUPDATE_INFORMATION* pInfo);
HRESULT WINAPI XNetGetCurrentAdapter(char* pszAdapter, DWORD* pdwSize);
HRESULT WINAPI XLiveUpdateSystem(LPCWSTR lpszRelaunchCmdLine);
HRESULT WINAPI XLiveGetLiveIdError(HRESULT* phrAuthState, HRESULT* phrRequestState, LPWSTR lpszUrl, DWORD* pcchUrl);
HRESULT WINAPI XLiveSetSponsorToken(LPCWSTR lpszToken, DWORD dwTitleId);
HRESULT WINAPI XLiveUninstallTitle(DWORD dwTitleId);
DWORD WINAPI XLiveLoadLibraryEx(LPCWSTR lpszModuleFileName, HINSTANCE* phModule, DWORD dwFlags);
HRESULT WINAPI XLiveFreeLibrary(HMODULE hModule);
BOOL WINAPI XLivePreTranslateMessage(const MSG* pMsg);
HRESULT WINAPI XLiveSetDebugLevel(XLIVE_DEBUG_LEVEL xdlLevel, XLIVE_DEBUG_LEVEL* pxdlOldLevel);
HRESULT WINAPI XLiveVerifyArcadeLicense(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset);
HRESULT WINAPI XLiveProtectData(const uint8_t* pInBuffer, DWORD dwInDataSize, uint8_t* pOutBuffer, DWORD* pdwOutDataSize, HANDLE hProtectedData);
HRESULT WINAPI XLiveUnprotectData(const uint8_t* pInBuffer, DWORD dwInDataSize, uint8_t* pOutBuffer, DWORD* pdwOutDataSize, HANDLE* phProtectedData);
HRESULT WINAPI XLiveCreateProtectedDataContext(const XLIVE_PROTECTED_DATA_INFORMATION* pInfo, HANDLE* phProtectedData);
HRESULT WINAPI XLiveQueryProtectedDataInformation(HANDLE hProtectedData, XLIVE_PROTECTED_DATA_INFORMATION* pInfo);
HRESULT WINAPI XLiveCloseProtectedDataContext(HANDLE hProtectedData);
HRESULT WINAPI XLiveVerifyDataFile(LPCWSTR lpszFileName);

// #5206 - #5299 UI.
DWORD WINAPI XShowMessagesUI(DWORD dwUserIndex);
DWORD WINAPI XShowGameInviteUI(DWORD dwUserIndex, const XUID* pXuidRecipients, DWORD cRecipients, LPCWSTR lpszUnused);
DWORD WINAPI XShowMessageComposeUI(DWORD dwUserIndex, const XUID* pXuidRecipients, DWORD cRecipients, LPCWSTR lpszText);
DWORD WINAPI XShowFriendRequestUI(DWORD dwUserIndex, XUID xuidUser);
DWORD WINAPI XShowCustomPlayerListUI(DWORD dwUserIndex, DWORD dwFlags, LPCWSTR lpszTitle, LPCWSTR lpszDescription, const uint8_t* pbImage, DWORD cbImage, const XPLAYERLIST_USER* pPlayers, DWORD cPlayers, const XPLAYERLIST_BUTTON* pXButton, const XPLAYERLIST_BUTTON* pYButton, XPLAYERLIST_RESULT* pResult, XOVERLAPPED* pOverlapped);
DWORD WINAPI XShowPlayerReviewUI(DWORD dwUserIndex, XUID xuidFeedbackTarget);
DWORD WINAPI XShowGuideUI(DWORD dwUserIndex);
DWORD WINAPI XShowKeyboardUI(DWORD dwUserIndex, DWORD dwFlags, LPCWSTR lpszDefaultText, LPCWSTR lpszTitleText, LPCWSTR lpszDescriptionText, LPWSTR lpszResultText, DWORD cchResultText, XOVERLAPPED* pOverlapped);
DWORD WINAPI XShowArcadeUI(DWORD dwUserIndex);
DWORD WINAPI XShowAchievementsUI(DWORD dwUserIndex);
DWORD WINAPI XShowGamerCardUI(DWORD dwUserIndex, XUID xuidPlayer);
DWORD WINAPI XShowSigninUI(DWORD cPanes, DWORD dwFlags);
DWORD WINAPI XShowMessageBoxUI(DWORD dwUserIndex, LPCWSTR lpszTitle, LPCWSTR lpszText, DWORD cButtons, LPCWSTR* pwszButtons, DWORD dwFocusButton, DWORD dwFlags, MESSAGEBOX_RESULT* pResult, XOVERLAPPED* pOverlapped);
DWORD WINAPI XShowPlayersUI(DWORD dwUserIndex);
DWORD WINAPI XShowFriendsUI(DWORD dwUserIndex);
DWORD WINAPI XShowMarketplaceUI(DWORD dwUserIndex, DWORD dwEntryPoint, uint64_t qwOfferId, DWORD dwContentCategories);
DWORD WINAPI XShowMarketplaceDownloadItemsUI(DWORD dwUserIndex, DWORD dwEntryPoint, const uint64_t* pOfferIds, DWORD cOfferIds, HRESULT* phrResult, XOVERLAPPED* pOverlapped);
DWORD WINAPI XLiveGetGuideKey(XINPUT_KEYSTROKE* pKeystroke);
DWORD WINAPI XShowGuideKeyRemapUI(DWORD dwUserIndex);

// #5230 - #5238 XLocator.
HRESULT WINAPI XLocatorServerAdvertise(DWORD dwUserIndex, DWORD dwServerType, XNKID xnkid, XNKEY xnkey, DWORD dwMaxPublicSlots, DWORD dwMaxPrivateSlots, DWORD dwFilledPublicSlots, DWORD dwFilledPrivateSlots, DWORD cProperties, XUSER_PROPERTY* pProperties, XOVERLAPPED* pOverlapped);
HRESULT WINAPI XLocatorServerUnAdvertise(DWORD dwUserIndex, XOVERLAPPED* pOverlapped);
HRESULT WINAPI XLocatorGetServiceProperty(DWORD dwUserIndex, DWORD cProperties, XUSER_PROPERTY* pProperties, XOVERLAPPED* pOverlapped);
DWORD WINAPI XLocatorCreateServerEnumerator(DWORD dwUserIndex, DWORD cItems, DWORD cRequiredPropertyIds, const DWORD* pRequiredPropertyIds, DWORD cFilterGroupItems, const XLOCATOR_FILTER_GROUP* pFilterGroups, DWORD cSorters, const XLOCATOR_SORTER* pSorters, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XLocatorCreateServerEnumeratorByIDs(DWORD dwUserIndex, DWORD cItems, DWORD cRequiredPropertyIds, const DWORD* pRequiredPropertyIds, DWORD cIDs, const uint64_t* pIDs, DWORD* pcbBuffer, HANDLE* phEnum);
HRESULT WINAPI XLocatorServiceInitialize(XLOCATOR_INIT_INFO* pInitInfo, HANDLE* phService);
HRESULT WINAPI XLocatorServiceUnInitialize(HANDLE hService);
HRESULT WINAPI XLocatorCreateKey(XNKID* pxnkid, XNKEY* pxnkey);

// #5251, #5255, #5256 Handles and enumeration.
BOOL WINAPI XCloseHandle(HANDLE hObject);
DWORD WINAPI XEnumerateBack(HANDLE hEnum, void* pvBuffer, DWORD cbBuffer, DWORD* pcItemsReturned, XOVERLAPPED* pOverlapped);
DWORD WINAPI XEnumerate(HANDLE hEnum, void* pvBuffer, DWORD cbBuffer, DWORD* pcItemsReturned, XOVERLAPPED* pOverlapped);

// #5257 - #5267, #5273 - #5299 Users.
HRESULT WINAPI XLiveManageCredentials(LPCWSTR lpszLiveIdName, LPCWSTR lpszLiveIdPassword, DWORD dwFlags, XOVERLAPPED* pOverlapped);
HRESULT WINAPI XLiveSignout(XOVERLAPPED* pOverlapped);
HRESULT WINAPI XLiveSignin(LPCWSTR lpszLiveIdName, LPCWSTR lpszLiveIdPassword, DWORD dwFlags, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserGetXUID(DWORD dwUserIndex, XUID* pXuid);
XUSER_SIGNIN_STATE WINAPI XUserGetSigninState(DWORD dwUserIndex);
DWORD WINAPI XUserGetName(DWORD dwUserIndex, char* szUserName, DWORD cchUserName);
DWORD WINAPI XUserAreUsersFriends(DWORD dwUserIndex, const XUID* pXuids, DWORD cXuids, BOOL* pfResult, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserCheckPrivilege(DWORD dwUserIndex, XPRIVILEGE_TYPE privilegeType, BOOL* pfResult);
DWORD WINAPI XUserGetSigninInfo(DWORD dwUserIndex, DWORD dwFlags, XUSER_SIGNIN_INFO* pSigninInfo);
DWORD WINAPI XUserReadGamerPictureByKey(const XUSER_DATA* pPictureKey, BOOL fSmall, uint8_t* pbTextureBuffer, DWORD dwPitch, DWORD dwHeight, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserAwardGamerPicture(DWORD dwUserIndex, DWORD dwPictureId, DWORD dwReserved, XOVERLAPPED* pOverlapped);
void WINAPI XUserSetProperty(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, const void* pvValue);
void WINAPI XUserSetContext(DWORD dwUserIndex, DWORD dwContextId, DWORD dwContextValue);
DWORD WINAPI XUserWriteAchievements(DWORD dwNumAchievements, const XUSER_ACHIEVEMENT* pAchievements, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserReadAchievementPicture(DWORD dwUserIndex, DWORD dwTitleId, DWORD dwImageId, uint8_t* pbTextureBuffer, DWORD dwPitch, DWORD dwHeight, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserCreateAchievementEnumerator(DWORD dwTitleId, DWORD dwUserIndex, XUID xuid, DWORD dwDetailFlags, DWORD dwStartingIndex, DWORD cItem, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XUserReadStats(DWORD dwTitleId, DWORD dwNumXuids, const XUID* pXuids, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbResults, XUSER_STATS_READ_RESULTS* pResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserReadGamerPicture(DWORD dwUserIndex, BOOL fSmall, uint8_t* pbTextureBuffer, DWORD dwPitch, DWORD dwHeight, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserCreateStatsEnumeratorByRank(DWORD dwTitleId, DWORD dwRankStart, DWORD dwNumRows, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XUserCreateStatsEnumeratorByRating(DWORD dwTitleId, LONGLONG i64Rating, DWORD dwNumRows, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XUserCreateStatsEnumeratorByXuid(DWORD dwTitleId, XUID xuidPivot, DWORD dwNumRows, DWORD dwNumStatsSpecs, const XUSER_STATS_SPEC* pSpecs, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XUserResetStatsView(DWORD dwUserIndex, DWORD dwViewId, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserGetProperty(DWORD dwUserIndex, DWORD* pcbActual, XUSER_PROPERTY* pProperty, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserGetContext(DWORD dwUserIndex, XUSER_CONTEXT* pContext, XOVERLAPPED* pOverlapped);
FLOAT WINAPI XUserGetReputationStars(FLOAT fGamerRating);
DWORD WINAPI XUserResetStatsViewAllUsers(DWORD dwViewId, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserSetContextEx(DWORD dwUserIndex, DWORD dwContextId, DWORD dwContextValue, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserSetPropertyEx(DWORD dwUserIndex, DWORD dwPropertyId, DWORD cbValue, const void* pvValue, XOVERLAPPED* pOverlapped);
HRESULT WINAPI XLivePBufferGetByteArray(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, uint8_t* pbData, DWORD cbData);
HRESULT WINAPI XLivePBufferSetByteArray(XLIVE_PROTECTED_BUFFER* pBuffer, DWORD dwOffset, const uint8_t* pbData, DWORD cbData);
HRESULT WINAPI XLiveGetLocalOnlinePort(WORD* pwPort);
HRESULT WINAPI XLiveInitializeEx(XLIVE_INITIALIZE_INFO* pXii, DWORD dwTitleXLiveVersion);

// #5300 - #5343 Sessions, storage, friends, presence.
DWORD WINAPI XSessionCreate(DWORD dwFlags, DWORD dwUserIndex, DWORD dwMaxPublicSlots, DWORD dwMaxPrivateSlots, uint64_t* pqwSessionNonce, XSESSION_INFO* pSessionInfo, XOVERLAPPED* pOverlapped, HANDLE* phSession);
DWORD WINAPI XStringVerify(DWORD dwFlags, const char* szLocale, DWORD dwNumStrings, const STRING_DATA* pStringData, DWORD cbResults, STRING_VERIFY_RESPONSE* pResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XStorageUploadFromMemoryGetProgress(XOVERLAPPED* pOverlapped, DWORD* pdwPercentComplete, uint64_t* pqwNumerator, uint64_t* pqwDenominator);
DWORD WINAPI XStorageUploadFromMemory(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwBufferSize, const uint8_t* pbBuffer, XOVERLAPPED* pOverlapped);
DWORD WINAPI XStorageEnumerate(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwStartingIndex, DWORD dwMaxResultsToReturn, DWORD cbResults, XSTORAGE_ENUMERATE_RESULTS* pResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XStorageDownloadToMemoryGetProgress(XOVERLAPPED* pOverlapped, DWORD* pdwPercentComplete, uint64_t* pqwNumerator, uint64_t* pqwDenominator);
DWORD WINAPI XStorageDelete(DWORD dwUserIndex, LPCWSTR wszServerPath, XOVERLAPPED* pOverlapped);
DWORD WINAPI XStorageBuildServerPathByXuid(XUID xuidUser, XSTORAGE_FACILITY storageFacility, const void* pvStorageFacilityInfo, DWORD dwStorageFacilityInfoSize, LPCWSTR pwszItemName, LPWSTR pwszServerPath, DWORD* pdwServerPathLength);
DWORD WINAPI XOnlineStartup();
DWORD WINAPI XOnlineCleanup();
DWORD WINAPI XFriendsCreateEnumerator(DWORD dwUserIndex, DWORD dwStartingIndex, DWORD dwFriendsToReturn, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XPresenceInitialize(DWORD cPeerSubscriptions);
DWORD WINAPI XUserMuteListQuery(DWORD dwUserIndex, XUID xuidRemoteTalker, BOOL* pfOnMuteList);
DWORD WINAPI XInviteGetAcceptedInfo(DWORD dwUserIndex, XINVITE_INFO* pInfo);
DWORD WINAPI XInviteSend(DWORD dwUserIndex, DWORD cInvitees, const XUID* pXuidInvitees, LPCWSTR pszText, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionWriteStats(HANDLE hSession, XUID xuid, DWORD dwNumViews, const XSESSION_VIEW_PROPERTIES* pViews, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionStart(HANDLE hSession, DWORD dwFlags, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionSearchEx(DWORD dwProcedureIndex, DWORD dwUserIndex, DWORD dwNumResults, DWORD dwNumUsers, WORD wNumProperties, WORD wNumContexts, XUSER_PROPERTY* pSearchProperties, XUSER_CONTEXT* pSearchContexts, DWORD* pcbResultsBuffer, XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionSearchByID(XNKID sessionID, DWORD dwUserIndex, DWORD* pcbResultsBuffer, XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionSearch(DWORD dwProcedureIndex, DWORD dwUserIndex, DWORD dwNumResults, WORD wNumProperties, WORD wNumContexts, XUSER_PROPERTY* pSearchProperties, XUSER_CONTEXT* pSearchContexts, DWORD* pcbResultsBuffer, XSESSION_SEARCHRESULT_HEADER* pSearchResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionModify(HANDLE hSession, DWORD dwFlags, DWORD dwMaxPublicSlots, DWORD dwMaxPrivateSlots, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionMigrateHost(HANDLE hSession, DWORD dwUserIndex, XSESSION_INFO* pSessionInfo, XOVERLAPPED* pOverlapped);
XONLINE_NAT_TYPE WINAPI XOnlineGetNatType();
DWORD WINAPI XSessionLeaveLocal(HANDLE hSession, DWORD dwUserCount, const DWORD* pdwUserIndexes, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionJoinRemote(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids, const BOOL* pfPrivateSlots, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionJoinLocal(HANDLE hSession, DWORD dwUserCount, const DWORD* pdwUserIndexes, const BOOL* pfPrivateSlots, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionGetDetails(HANDLE hSession, DWORD* pcbResultsBuffer, XSESSION_LOCAL_DETAILS* pSessionDetails, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionFlushStats(HANDLE hSession, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionDelete(HANDLE hSession, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserReadProfileSettings(DWORD dwTitleId, DWORD dwUserIndex, DWORD dwNumSettingIds, const DWORD* pdwSettingIds, DWORD* pcbResults, XUSER_READ_PROFILE_SETTING_RESULT* pResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionEnd(HANDLE hSession, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionArbitrationRegister(HANDLE hSession, DWORD dwFlags, uint64_t qwSessionNonce, DWORD* pcbResultsBuffer, XSESSION_REGISTRATION_RESULTS* pRegistrationResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XOnlineGetServiceInfo(DWORD dwServiceId, XONLINE_SERVICE_INFO* pServiceInfo);
DWORD WINAPI XTitleServerCreateEnumerator(const char* pszServerInfo, DWORD cItem, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XSessionLeaveRemote(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserWriteProfileSettings(DWORD dwUserIndex, DWORD dwNumSettings, const XUSER_PROFILE_SETTING* pSettings, XOVERLAPPED* pOverlapped);
DWORD WINAPI XPresenceSubscribe(DWORD dwUserIndex, DWORD cPeers, const XUID* pPeers);
DWORD WINAPI XUserReadProfileSettingsByXuid(DWORD dwTitleId, DWORD dwUserIndexRequester, DWORD dwNumFor, const XUID* pxuidFor, DWORD dwNumSettingIds, const DWORD* pdwSettingIds, DWORD* pcbResults, XUSER_READ_PROFILE_SETTING_RESULT* pResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XPresenceCreateEnumerator(DWORD dwUserIndex, DWORD cPeers, const XUID* pPeers, DWORD dwStartingIndex, DWORD cPeersToReturn, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XPresenceUnsubscribe(DWORD dwUserIndex, DWORD cPeers, const XUID* pPeers);
DWORD WINAPI XSessionModifySkill(HANDLE hSession, DWORD dwXuidCount, const XUID* pXuids, XOVERLAPPED* pOverlapped);
DWORD WINAPI XSessionCalculateSkill(DWORD dwNumSkills, double* rgMu, double* rgSigma, double* pdAggregateMu, double* pdAggregateSigma);
DWORD WINAPI XStorageBuildServerPath(DWORD dwUserIndex, XSTORAGE_FACILITY storageFacility, const void* pvStorageFacilityInfo, DWORD dwStorageFacilityInfoSize, LPCWSTR pwszItemName, LPWSTR pwszServerPath, DWORD* pdwServerPathLength);
DWORD WINAPI XStorageDownloadToMemory(DWORD dwUserIndex, LPCWSTR wszServerPath, DWORD dwBufferSize, uint8_t* pbBuffer, DWORD cbResults, XSTORAGE_DOWNLOAD_TO_MEMORY_RESULTS* pResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XUserEstimateRankForRating(DWORD dwNumRequests, const XUSER_RANK_REQUEST* pRequests, DWORD cbResults, XUSER_ESTIMATE_RANK_RESULTS* pResults, XOVERLAPPED* pOverlapped);

// #5347 - #5377 Content, marketplace, misc.
HRESULT WINAPI XLiveProtectedLoadLibrary(HANDLE hContentAccess, void* pvReserved, LPCWSTR lpszModuleFileName, DWORD dwFlags, HMODULE* phModule);
HRESULT WINAPI XLiveProtectedCreateFile(HANDLE hContentAccess, void* pvReserved, LPCWSTR lpszFileName, DWORD dwDesiredAccess, DWORD dwShareMode, SECURITY_ATTRIBUTES* lpSecurityAttributes, DWORD dwCreationDisposition, DWORD dwFlagsAndAttributes, HANDLE* phFile);
HRESULT WINAPI XLiveProtectedVerifyFile(HANDLE hContentAccess, void* pvReserved, LPCWSTR lpszFileName);
HRESULT WINAPI XLiveContentCreateAccessHandle(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, DWORD dwLicenseInfoVersion, XLIVE_PROTECTED_BUFFER* pLicenseInfo, DWORD dwOffset, HANDLE* phContentAccess, XOVERLAPPED* pOverlapped);
HRESULT WINAPI XLiveContentInstallPackage(const XLIVE_CONTENT_INFO* pContentInfo, LPCWSTR lpszCabFile, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams);
HRESULT WINAPI XLiveContentUninstall(const XLIVE_CONTENT_INFO* pContentInfo, const XUID* pXuid, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams);
HRESULT WINAPI XLiveContentVerifyInstalledPackage(const XLIVE_CONTENT_INFO* pContentInfo, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams);
HRESULT WINAPI XLiveContentGetPath(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, LPWSTR lpszPath, DWORD* pcchPath);
HRESULT WINAPI XLiveContentGetDisplayName(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, LPWSTR lpszDisplayName, DWORD* pcchDisplayName);
HRESULT WINAPI XLiveContentGetThumbnail(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, uint8_t* pbThumbnail, DWORD* pcbThumbnail);
HRESULT WINAPI XLiveContentInstallLicense(const XLIVE_CONTENT_INFO* pContentInfo, LPCWSTR lpszLicenseFile, XLIVE_CONTENT_INSTALL_CALLBACK_PARAMS* pParams);
HRESULT WINAPI XLiveGetUPnPState(XONLINE_NAT_TYPE* pNatType);
DWORD WINAPI XLiveContentCreateEnumerator(DWORD cItems, const XLIVE_CONTENT_RETRIEVAL_INFO* pRetrievalInfo, DWORD* pcbBuffer, HANDLE* phEnum);
HRESULT WINAPI XLiveContentRetrieveOffersByDate(DWORD dwUserIndex, DWORD dwOfferInfoVersion, const SYSTEMTIME* pstStartDate, XLIVE_OFFER_INFO* pOfferInfo, DWORD* pcOfferInfo, XOVERLAPPED* pOverlapped);
BOOL WINAPI XLiveMarketplaceDoesContentIdMatch(const uint8_t* pbContentId, const XLIVE_CONTENT_INFO* pContentInfo);
HRESULT WINAPI XLiveContentGetLicensePath(DWORD dwUserIndex, const XLIVE_CONTENT_INFO* pContentInfo, LPWSTR lpszLicensePath, DWORD* pcchLicensePath);
DWORD WINAPI XContentGetMarketplaceCounts(DWORD dwUserIndex, DWORD dwContentCategories, DWORD cbResults, XOFFERING_CONTENTAVAILABLE_RESULT* pResults, XOVERLAPPED* pOverlapped);
DWORD WINAPI XMarketplaceConsumeAssets(DWORD dwUserIndex, DWORD cAssets, const XMARKETPLACE_ASSET* pAssets, XOVERLAPPED* pOverlapped);
DWORD WINAPI XMarketplaceCreateAssetEnumerator(DWORD dwUserIndex, DWORD cItems, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XMarketplaceCreateOfferEnumerator(DWORD dwUserIndex, DWORD dwOfferType, DWORD dwContentCategories, DWORD cItems, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XMarketplaceGetDownloadStatus(DWORD dwUserIndex, uint64_t qwOfferId, DWORD* pdwResult);
void WINAPI XMarketplaceGetImageUrl(DWORD dwTitleId, uint64_t qwOfferId, DWORD cchUrl, LPWSTR pszUrl);
DWORD WINAPI XMarketplaceCreateOfferEnumeratorByOffering(DWORD dwUserIndex, DWORD cItems, const uint64_t* pOfferIds, WORD cOfferIds, DWORD* pcbBuffer, HANDLE* phEnum);
DWORD WINAPI XUserFindUsers(XUID xuidRequester, DWORD dwUsers, const FIND_USER_INFO* pUsers, DWORD cbResults, FIND_USERS_RESPONSE* pResults, XOVERLAPPED* pOverlapped);
