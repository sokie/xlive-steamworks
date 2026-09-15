// Notification listeners. Steam callbacks become XN_* notifications here.
#pragma once

#include "xlive/xdefs.h"

namespace xls {

void NotifyInit();
void NotifyShutdown();

// Queues a notification for every listener of its area.
void NotifyPost(DWORD notificationId, ULONG_PTR parameter);

HANDLE NotifyCreateListener(ULONGLONG areas);
bool NotifyCloseListener(HANDLE listener);
BOOL NotifyGetNext(HANDLE listener, DWORD filter, DWORD* notificationId, ULONG_PTR* parameter);

// XN_SYS_UI bookkeeping: a title pauses while the system UI (here the Steam overlay or one of
// our dialogs) is open.
void NotifySystemUi(bool open);
bool NotifySystemUiOpen();

void NotifySetUiPosition(DWORD position);
DWORD NotifyUiPosition();

}
