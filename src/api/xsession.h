#pragma once

#include "xlive/xdefs.h"
#include "core/steam.h"

namespace xls {

void SessionsInit();
void SessionsShutdown();
bool SessionHandleValid(HANDLE session);
bool SessionCloseHandle(HANDLE session);
CSteamID SessionPresenceLobby();
// Handles a +connect_lobby launch parameter as an accepted invite.
void SessionCheckLaunchInvite();
uint64_t SessionLobbyId(HANDLE session);
bool SessionInfoFromLobby(CSteamID lobby, XSESSION_INFO* info);
// A title may set contexts and properties after XSessionCreate so this updates the lobby data
void OnUserContextChanged(DWORD userIndex, DWORD contextId, DWORD value);
void OnUserPropertyChanged(DWORD userIndex, DWORD propertyId);

}
