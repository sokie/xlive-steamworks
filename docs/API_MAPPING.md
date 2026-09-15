# API mapping

Kinds:

- **1:1** - one Steam call answers the xlive call.
- **middle** - state or translation lives in the wrapper (a lobby doubles as a session, an
  overlapped is completed from a polled `SteamAPICall_t`, a datagram becomes a message).
- **local** - answered from the wrapper's own state or the SPA, no Steam call.
- **stub** - accepted and ignored, the return value is whatever lets the title continue.

## Lifetime and plumbing

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XLiveInitialize, XLiveInitializeEx | middle | `SteamAPI_InitEx`, SPA load, users, network, sessions. Posts `XN_SYS_SIGNINCHANGED` and `XN_LIVE_CONNECTIONCHANGED` |
| XLiveUninitialize | middle | leaves lobbies, `SteamAPI_Shutdown` |
| XLiveRender, XLiveInput, XLivePreTranslateMessage | middle | runs the pump: `SteamAPI_RunCallbacks`, async jobs, network receive. Home opens the Steam overlay as the Guide key did (`ui.home_key_opens_overlay`) |
| XLiveOnCreateDevice, OnDestroyDevice, OnResetDevice | stub | S_OK (the Steam overlay hooks D3D itself) |
| XLiveRegisterDataSection, XLiveUnregisterDataSection, XLiveUpdateHashes | stub | S_OK |
| XLiveGetUpdateInformation, XLiveUpdateSystem | stub | S_FALSE, Steam updates before launch |
| XLiveGetLiveIdError, XLiveSetSponsorToken, XLiveUninstallTitle | stub | S_OK |
| XLiveLoadLibraryEx, XLiveFreeLibrary | local | LoadLibraryEx / FreeLibrary |
| XLiveSetDebugLevel | local | stored |
| XLiveVerifyArcadeLicense | stub | S_OK, Steam owns licensing |
| XGetOverlappedResult, XGetOverlappedExtendedError, XCancelOverlapped | middle | wrapper's overlapped bridge, a wait pumps Steam |
| XEnumerate, XEnumerateBack, XCloseHandle | middle | enumerator registry. `XCloseHandle` also closes listeners and sessions |
| XNotifyCreateListener, XNotifyGetNext, XNotifyPositionUI, XNotifyDelayUI | middle | per-listener queues. `GameOverlayActivated_t` -> `XN_SYS_UI`, `PersonaStateChange_t` -> `XN_FRIENDS_*`, `GameLobbyJoinRequested_t` -> `XN_LIVE_INVITE_ACCEPTED`, `SteamServersConnected_t/Disconnected_t` -> `XN_LIVE_CONNECTIONCHANGED`, `DlcInstalled_t` -> `XN_LIVE_CONTENT_INSTALLED`. Position -> `SetOverlayNotificationPosition` |
| XLiveSignin, XLiveSignout, XLiveManageCredentials | stub | complete with success, the Steam user is always signed in |

## Users

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XUserGetXUID | 1:1 | `ISteamUser::GetSteamID` -> XUID |
| XUserGetSigninState | local | index 0 signed in to LIVE when Steam is up |
| XUserGetName | 1:1 | `GetPersonaName` squeezed to 15 chars |
| XUserGetSigninInfo | local | XUID, flags, name |
| XUserCheckPrivilege | local | TRUE, multiplayer needs `BLoggedOn` |
| XUserAreUsersFriends | 1:1 | `HasFriend` for each XUID |
| XUserSetContext(Ex), XUserSetProperty(Ex) | middle | stored per user, feeds lobby data and rich presence (`SetRichPresence`) |
| XUserGetContext, XUserGetProperty | local | stored values, SPA defaults for contexts |
| XUserGetReputationStars | local | 0-100 -> 0-5 stars |
| XUserMuteListQuery | 1:1 | `GetFriendRelationship` == ignored |
| XUserFindUsers | 1:1 | XUID -> `GetFriendPersonaName`. Name -> XUID is not possible on Steam |
| XStringVerify | 1:1 | `ISteamUtils::FilterText` |
| XUserReadGamerPicture, XUserReadGamerPictureByKey | middle | `GetSmallFriendAvatar/GetMediumFriendAvatar` + `GetImageRGBA`, waits for `AvatarImageLoaded_t` |
| XUserAwardGamerPicture | stub | success |

## Achievements

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XUserWriteAchievements | 1:1 | `SetAchievement(name)` + `StoreStats` with the name from config or `ACH_<id>`. When the app has no such Steam achievement the unlock is recorded in the per-user file `xlive/achievements.bin` (`achievements.local_fallback`) |
| XUserCreateAchievementEnumerator | middle | list from the SPA (ids, text, cred, image ids), unlock state from `GetAchievementAndUnlockTime`. Other users through `RequestUserStats` + `GetUserAchievementAndUnlockTime` |
| XUserReadAchievementPicture | middle | SPA PNG decoded to BGRA, falls back to `GetAchievementIcon` |

## Profile settings

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XUserReadProfileSettings, XUserReadProfileSettingsByXuid | middle | writable settings from the Cloud file `xlive/profile.bin`, region from `GetIPCountry`, cred and achievement counts computed from Steam achievements. Other users get defaults |
| XUserWriteProfileSettings | middle | Cloud file, posts `XN_SYS_PROFILESETTINGCHANGED` |

## Storage

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XStorageBuildServerPath, XStorageBuildServerPathByXuid | local | `user/<xuid>/<item>`, `title/<item>`, `clip/<lb>/<item>` |
| XStorageUploadFromMemory | 1:1 | `ISteamRemoteStorage::FileWrite` under `xlive/tms/`. The title facility is read-only |
| XStorageDownloadToMemory | 1:1 | `FileRead`. The title facility reads `xlive-title-storage\` |
| XStorageDelete | 1:1 | `FileDelete` |
| XStorageEnumerate | 1:1 | `GetFileCount/GetFileNameAndSize` with wildcard match |
| XStorage*GetProgress | local | 100% once the overlapped completed |

## Friends and presence

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XFriendsCreateEnumerator | 1:1 | `GetFriendCount/GetFriendByIndex`, state from `GetFriendPersonaState`, `GetFriendGamePlayed` (lobby -> `sessionID`), rich presence from `GetFriendRichPresence` |
| XPresenceInitialize | stub | success |
| XPresenceSubscribe, XPresenceUnsubscribe | 1:1 | `RequestFriendRichPresence`, `RequestUserInformation` |
| XPresenceCreateEnumerator | 1:1 | same fields as friends for arbitrary XUIDs |
| XInviteSend | 1:1 | `InviteUserToLobby` on the presence session's lobby |
| XInviteGetAcceptedInfo | middle | `GameLobbyJoinRequested_t`, `GameRichPresenceJoinRequested_t` or `+connect_lobby` -> `RequestLobbyData` -> `XINVITE_INFO` |

## Sessions

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XSessionCreate (host) | middle | `CreateLobby` with the lobby type from the create flags. Lobby data: kind, title, flags, slots, nonce, XNKEY, host XNADDR, state, game type/mode, every context `xl_c<id>` and property `xl_p<id>` |
| XSessionCreate (join) | middle | `JoinLobby(XNKID)`, reads the lobby data back into the session |
| XSessionJoinLocal, XSessionJoinRemote, XSessionLeaveLocal, XSessionLeaveRemote | middle | declared members merged with lobby membership. Local leave -> `LeaveLobby` |
| XSessionGetDetails | middle | lobby members -> `XSESSION_MEMBER[]`, slot counts, state |
| XSessionStart, XSessionEnd | middle | state in lobby data, `SetLobbyJoinable` per `JOIN_IN_PROGRESS_DISABLED` |
| XSessionModify | 1:1 | `SetLobbyMemberLimit`, `SetLobbyType`, lobby data |
| XSessionDelete | 1:1 | `LeaveLobby` |
| XSessionMigrateHost | middle | new host: publishes a host claim in its lobby member data and takes the lobby when the owner hands it over (the owner's wrapper does so on seeing the claim). Client: adopts the info passed in |
| XSessionArbitrationRegister | local | registrants from the member list with machine ids |
| XSessionSearch, XSessionSearchEx | middle | `RequestLobbyList` with kind/title filters plus one filter per XLAST query filter: the attribute's lobby key compared with the parameter, constant or context value the query names (numeric when the value fits an int, text otherwise). Without an XLAST query every context and property passed is an equality filter. Results with contexts and properties unpacked |
| XSessionSearchByID | middle | `RequestLobbyData` -> one result |
| XSessionWriteStats, XSessionFlushStats | middle | see stats |
| XSessionModifySkill | stub | success |
| XSessionCalculateSkill | local | mean of mu, root-sum-square of sigma |

## Stats and leaderboards

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XSessionWriteStats | middle | per view: `FindOrCreateLeaderboard(LB_<viewId>)` then `UploadLeaderboardScore` with the rating column as score and the other numeric columns packed into the details array in SPA column order, plus an optional `SetStat` mirror from config |
| XUserReadStats | middle | `DownloadLeaderboardEntriesForUsers`, details unpacked back into columns |
| XUserCreateStatsEnumeratorByRank | middle | `DownloadLeaderboardEntries(Global, start, end)` |
| XUserCreateStatsEnumeratorByXuid | middle | `GlobalAroundUser` for the local user, single-user download otherwise |
| XUserCreateStatsEnumeratorByRating | middle | top of the board (Steam cannot seek by score) |
| XUserResetStatsView, XUserResetStatsViewAllUsers | stub | success, a client cannot delete its Steam entry |
| XUserEstimateRankForRating | stub | rank 1 |

## Networking

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XNetStartup(Ex), XNetCleanup | middle | network layer init |
| XNetCreateKey, XNetRegisterKey, XNetUnregisterKey, XNetReplaceKey | local | key table, unregister releases the QoS listener |
| XNetXnAddrToInAddr | middle | Steam id out of the XNADDR -> alias 10.0.0.n |
| XNetInAddrToXnAddr | middle | alias -> XNADDR |
| XNetServerToInAddr, XNetTsAddrToInAddr, XNetInAddrToServer | middle | title server alias with the real IP behind it |
| XNetXnAddrToMachineId | local | 0xFA... + account id |
| XNetConnect, XNetGetConnectStatus | 1:1 | opens a SteamNetworkingMessages session, `GetSessionConnectionInfo` |
| XNetDnsLookup, XNetDnsRelease | local | `getaddrinfo` on a thread |
| XNetQosListen, XNetQosLookup, XNetQosRelease, XNetQosGetListenStats | middle | probe/reply datagrams on a control channel, the listener's data blob rides in the reply |
| XNetQosServiceLookup | stub | one completed entry |
| XNetGetTitleXnAddr, XNetGetDebugXnAddr | local | local XNADDR, online flag from `BLoggedOn` |
| XNetGetEthernetLinkStatus, XNetGetBroadcastVersionStatus, XNetGetOpt, XNetSetOpt, XNetGetXnAddrPlatform, XNetGet/SetSystemLinkPort | local | constants |
| XNetGetCurrentAdapter, XLiveGetLocalOnlinePort | local | constants |
| XOnlineStartup, XOnlineCleanup | stub | success |
| XOnlineGetNatType, XLiveGetUPnPState | local | OPEN (relay) |
| XOnlineGetServiceInfo, XTitleServerCreateEnumerator | local | `title_servers` from the config |
| XWSAStartup, XWSACleanup | 1:1 | Winsock |
| XSocketCreate/Close/Shutdown/IOCTLSocket/Set/GetSockOpt/GetSockName/GetPeerName/Bind | middle | wrapper sockets |
| XSocketSendTo, XSocketSend, XWSASend, XWSASendTo | middle | to an alias: `SendMessageToUser(identity, channel = port)`. To a title server or LAN IP: Winsock. Broadcast: every known peer |
| XSocketRecvFrom, XSocketRecv, XWSARecv, XWSARecvFrom | middle | `ReceiveMessagesOnChannel(port)` queued per socket, stream data from `ReceiveMessagesOnConnection` |
| XSocketConnect, XSocketListen, XSocketAccept | middle | `ConnectP2P` / `CreateListenSocketP2P` / `AcceptConnection` on virtual port = port, real addresses through Winsock |
| XSocketSelect, XWSAEventSelect, XWSAWaitForMultipleEvents | middle | polls the wrapper queues, pumps while waiting |
| XWSAGetOverlappedResult, XWSACancelOverlappedIO | middle | immediate completion table |
| XWSA*Event, XWSAFDIsSet, XSocketInet_Addr, XSocketWSAGetLastError, XWSASetLastError, HTONL/NTOHS/NTOHL/HTONS | 1:1 | Winsock |

## Guide UI

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XShowGuideUI, XShowFriendsUI, XShowMessagesUI | 1:1 | `ActivateGameOverlay("Friends")` |
| XShowAchievementsUI | 1:1 | `ActivateGameOverlay("Achievements")` |
| XShowPlayersUI, XShowCustomPlayerListUI | 1:1 / stub | `ActivateGameOverlay("Players")`, the custom list reports cancel |
| XShowGamerCardUI, XShowPlayerReviewUI | 1:1 | `ActivateGameOverlayToUser("steamid")` |
| XShowFriendRequestUI | 1:1 | `ActivateGameOverlayToUser("friendadd")` |
| XShowMessageComposeUI | 1:1 | `ActivateGameOverlayToUser("chat")` |
| XShowGameInviteUI | 1:1 | `ActivateGameOverlayInviteDialog(lobby)` or `InviteUserToLobby` per recipient |
| XShowSigninUI | local | posts `XN_SYS_UI` and `XN_SYS_SIGNINCHANGED` |
| XShowKeyboardUI | middle | `ShowGamepadTextInput` in Big Picture / on Steam hardware, else a native dialog on a worker thread |
| XShowMessageBoxUI | middle | native dialog with up to 3 buttons, passcode modes report cancel |
| XShowArcadeUI, XShowMarketplaceUI, XShowMarketplaceDownloadItemsUI | 1:1 | `ActivateGameOverlayToStore` |
| XLiveGetGuideKey | local | Shift+Tab |
| XShowGuideKeyRemapUI | 1:1 | `ActivateGameOverlay("Settings")` |
| XCustom* | stub | no overlay hook |

## Content and marketplace

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XLiveContentCreateEnumerator | middle | config `content.dlc` plus `GetDLCCount/BGetDLCDataByIndex`. `BIsDlcInstalled` and `BIsSubscribedApp` gate the list |
| XLiveContentGetPath, GetDisplayName, GetLicensePath, GetThumbnail | local | from the mapping, thumbnail.png in the package folder |
| XLiveContentCreateAccessHandle | local | handle for an owned package, license info filled |
| XLiveContentVerifyInstalledPackage | 1:1 | `BIsDlcInstalled` |
| XLiveContentInstallPackage, InstallLicense | stub | S_OK, Steam installs DLC |
| XLiveContentUninstall | 1:1 | `UninstallDLC` |
| XLiveContentRetrieveOffersByDate, XMarketplaceCreateOfferEnumerator(ByOffering), XContentGetMarketplaceCounts | middle | offers = DLC apps not owned |
| XMarketplaceGetDownloadStatus | 1:1 | `GetDlcDownloadProgress` |
| XMarketplaceGetImageUrl | local | Steam CDN header image URL |
| XMarketplaceCreateAssetEnumerator, XMarketplaceConsumeAssets | stub | empty package |
| XLiveMarketplaceDoesContentIdMatch | local | memcmp |
| XLiveProtectedCreateFile, XLiveProtectedLoadLibrary, XLiveProtectedVerifyFile | local | CreateFile / LoadLibraryEx / file exists |

## XLocator

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XLocatorServiceInitialize, UnInitialize, CreateKey | local | handle, key with the server flag |
| XLocatorServerAdvertise | middle | `CreateLobby(Public)` tagged `xl_locator=1` with slots, key and properties in lobby data |
| XLocatorServerUnAdvertise | 1:1 | `LeaveLobby` |
| XLocatorCreateServerEnumerator(ByIDs) | middle | `RequestLobbyList` on the tag -> `XLOCATOR_SEARCHRESULT[]` with properties. Filter groups and sorters are accepted but not applied |
| XLocatorGetServiceProperty | stub | zeros |

## Protected data

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XLivePBuffer* | local | plain heap buffers |
| XLiveProtectData, XLiveUnprotectData, Create/Query/CloseProtectedDataContext | local | pass-through, offline flag from sign-in state |
| XLiveVerifyDataFile | local | file exists |

## Voice

| Export | Kind | Steam side / behaviour |
| --- | --- | --- |
| XHVCreateEngine | middle | `IXHVEngine` on `StartVoiceRecording/GetVoice/DecompressVoice`. Frames are `[u16 size][data]` so the title's own transport carries them. Playback per remote talker through waveOut |

## Extension API (not GFWL)

| Export | Purpose |
| --- | --- |
| XlsVersion | wrapper version |
| XlsSetSteamOwnedByTitle, XlsSetRunSteamCallbacks, XlsRunFrame | share the Steam API with a title that initialises it |
| XlsXuidFromSteamId, XlsSteamIdFromXuid | identity both ways |
| XlsLobbyIdFromSession, XlsLobbyIdFromXnkid, XlsXnkidFromLobbyId, XlsSessionInfoFromLobby | session <-> lobby |
| XlsSecureAddrFromSteamId, XlsSteamIdFromSecureAddr, XlsXnaddrFromSteamId | addresses <-> Steam id |
| XlsPeerConnectionInfo, XlsSocketConnectionInfo | direct or relayed, relay data centre, ping and quality of a peer session or a stream socket |
| XlsSetP2PTransport | force every peer connection through a Steam relay (also `network.relay_only` in the config) |
| XlsSetAchievementName, XlsSetLeaderboardName, XlsGetAchievementName, XlsGetLeaderboardName | run-time mapping |
| XlsPostNotification | inject XN_* notifications |
