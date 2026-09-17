// #5206 - #5299 Guide UI: the Steam overlay where it has a page, a native dialog where it does not.
#include "xlive/xfuncs.h"
#include "api/xlive.h"
#include "api/xsession.h"

#include "core/config.h"
#include "core/log.h"
#include "core/notify.h"
#include "core/overlapped.h"
#include "core/steam.h"
#include "core/users.h"
#include "core/utils.h"

#include <memory>
#include <mutex>
#include <thread>
#include <vector>

namespace {

// --- In-memory dialog templates ------------------------------------------------------------------

class DialogTemplate {
public:
	void Header(DWORD style, short x, short y, short cx, short cy, const wchar_t* title)
	{
		Write32(style | DS_SETFONT);
		Write32(0);
		m_countOffset = m_data.size();
		Write16(0);
		Write16((uint16_t)x);
		Write16((uint16_t)y);
		Write16((uint16_t)cx);
		Write16((uint16_t)cy);
		Write16(0); // menu
		Write16(0); // class
		WriteString(title);
		Write16(9); // point size
		WriteString(L"Segoe UI");
	}

	void Item(DWORD style, short x, short y, short cx, short cy, uint16_t id, uint16_t classAtom, const wchar_t* text)
	{
		Align();
		Write32(style | WS_CHILD | WS_VISIBLE);
		Write32(0);
		Write16((uint16_t)x);
		Write16((uint16_t)y);
		Write16((uint16_t)cx);
		Write16((uint16_t)cy);
		Write16(id);
		Write16(0xFFFF);
		Write16(classAtom);
		WriteString(text);
		Write16(0);
		m_data[m_countOffset]++;
	}

	const DLGTEMPLATE* Get() { Align(); return (const DLGTEMPLATE*)m_data.data(); }

private:
	void Write16(uint16_t value) { m_data.push_back(value); }
	void Write32(uint32_t value) { Write16((uint16_t)value); Write16((uint16_t)(value >> 16)); }
	void WriteString(const wchar_t* text)
	{
		for (const wchar_t* c = text ? text : L""; *c; c++) {
			Write16((uint16_t)*c);
		}
		Write16(0);
	}
	void Align()
	{
		if (m_data.size() % 2) {
			Write16(0);
		}
	}

	std::vector<uint16_t> m_data;
	size_t m_countOffset = 0;
};

const uint16_t kButtonClass = 0x0080;
const uint16_t kEditClass = 0x0081;
const uint16_t kStaticClass = 0x0082;
const uint16_t kFirstButtonId = 100;
const uint16_t kEditId = 200;

struct DialogRequest {
	bool keyboard = false;
	std::wstring title;
	std::wstring text;
	std::wstring description;
	std::vector<std::wstring> buttons;
	DWORD focusButton = 0;
	DWORD keyboardFlags = 0;
	std::wstring defaultText;
	MESSAGEBOX_RESULT* messageResult = nullptr;
	wchar_t* textResult = nullptr;
	DWORD textResultCount = 0;
	XOVERLAPPED* overlapped = nullptr;
	DWORD outcome = ERROR_CANCELLED;
};

INT_PTR CALLBACK DialogProc(HWND dialog, UINT message, WPARAM wParam, LPARAM lParam)
{
	DialogRequest* request = (DialogRequest*)GetWindowLongPtrW(dialog, GWLP_USERDATA);
	switch (message) {
		case WM_INITDIALOG: {
			request = (DialogRequest*)lParam;
			SetWindowLongPtrW(dialog, GWLP_USERDATA, lParam);
			SetWindowPos(dialog, HWND_TOPMOST, 0, 0, 0, 0, SWP_NOMOVE | SWP_NOSIZE | SWP_SHOWWINDOW);
			if (request->keyboard) {
				SetDlgItemTextW(dialog, kEditId, request->defaultText.c_str());
				SendDlgItemMessageW(dialog, kEditId, EM_SETLIMITTEXT, request->textResultCount ? request->textResultCount - 1 : 0, 0);
				SendDlgItemMessageW(dialog, kEditId, EM_SETSEL, 0, -1);
				SetFocus(GetDlgItem(dialog, kEditId));
			}
			else {
				HWND focus = GetDlgItem(dialog, kFirstButtonId + (int)request->focusButton);
				if (focus) {
					SetFocus(focus);
				}
			}
			return FALSE;
		}
		case WM_COMMAND: {
			WORD id = LOWORD(wParam);
			if (!request) {
				return FALSE;
			}
			if (request->keyboard) {
				if (id == IDOK) {
					if (request->textResult && request->textResultCount) {
						GetDlgItemTextW(dialog, kEditId, request->textResult, (int)request->textResultCount);
					}
					request->outcome = ERROR_SUCCESS;
					EndDialog(dialog, IDOK);
					return TRUE;
				}
				if (id == IDCANCEL) {
					request->outcome = ERROR_CANCELLED;
					EndDialog(dialog, IDCANCEL);
					return TRUE;
				}
				return FALSE;
			}
			if (id >= kFirstButtonId && id < kFirstButtonId + request->buttons.size()) {
				if (request->messageResult) {
					request->messageResult->dwButtonPressed = id - kFirstButtonId;
				}
				request->outcome = ERROR_SUCCESS;
				EndDialog(dialog, id);
				return TRUE;
			}
			if (id == IDCANCEL) {
				if (request->messageResult) {
					request->messageResult->dwButtonPressed = (DWORD)XMB_CANCELID;
				}
				request->outcome = ERROR_CANCELLED;
				EndDialog(dialog, IDCANCEL);
				return TRUE;
			}
			return FALSE;
		}
		case WM_CLOSE:
			if (request) {
				if (request->keyboard) {
					request->outcome = ERROR_CANCELLED;
				}
				else if (request->messageResult) {
					request->messageResult->dwButtonPressed = (DWORD)XMB_CANCELID;
				}
			}
			EndDialog(dialog, IDCANCEL);
			return TRUE;
		default:
			return FALSE;
	}
}

HWND TitleWindow()
{
	HWND foreground = GetForegroundWindow();
	DWORD process = 0;
	if (foreground && GetWindowThreadProcessId(foreground, &process) && process == GetCurrentProcessId()) {
		return foreground;
	}
	return nullptr;
}

void RunDialog(std::shared_ptr<DialogRequest> request)
{
	DialogTemplate dialog;
	const short width = 260;
	short y = 10;
	if (request->keyboard) {
		bool multiline = (request->keyboardFlags & VKBD_MULTILINE) != 0;
		short editHeight = multiline ? 60 : 14;
		short height = (short)(y + 10 + (request->description.empty() ? 0 : 18) + editHeight + 30);
		dialog.Header(DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, width, height, request->title.c_str());
		if (!request->description.empty()) {
			dialog.Item(SS_LEFT, 10, y, width - 20, 16, (uint16_t)-1, kStaticClass, request->description.c_str());
			y += 18;
		}
		DWORD editStyle = WS_BORDER | WS_TABSTOP | ES_AUTOHSCROLL;
		if (multiline) editStyle |= ES_MULTILINE | ES_AUTOVSCROLL | ES_WANTRETURN;
		if (request->keyboardFlags & VKBD_LATIN_PASSWORD) editStyle |= ES_PASSWORD;
		if ((request->keyboardFlags & (VKBD_LATIN_NUMERIC | VKBD_LATIN_PHONE)) && !(request->keyboardFlags & ~(VKBD_LATIN_NUMERIC | VKBD_LATIN_PHONE | VKBD_SELECT_OK | VKBD_HIGHLIGHT_TEXT))) editStyle |= ES_NUMBER;
		dialog.Item(editStyle, 10, y, width - 20, editHeight, kEditId, kEditClass, L"");
		y += editHeight + 8;
		dialog.Item(BS_DEFPUSHBUTTON | WS_TABSTOP, width - 130, y, 55, 14, IDOK, kButtonClass, L"OK");
		dialog.Item(BS_PUSHBUTTON | WS_TABSTOP, width - 65, y, 55, 14, IDCANCEL, kButtonClass, L"Cancel");
	}
	else {
		// Roughly 55 characters per line at this width.
		size_t lines = 1;
		size_t column = 0;
		for (wchar_t c : request->text) {
			if (c == L'\n' || ++column > 55) {
				lines++;
				column = 0;
			}
		}
		short textHeight = (short)(lines * 10 + 6);
		short height = (short)(y + textHeight + 30);
		dialog.Header(DS_MODALFRAME | DS_CENTER | WS_POPUP | WS_CAPTION | WS_SYSMENU, 0, 0, width, height, request->title.c_str());
		dialog.Item(SS_LEFT, 10, y, width - 20, textHeight, (uint16_t)-1, kStaticClass, request->text.c_str());
		y += textHeight + 8;
		short buttonWidth = 60;
		short spacing = 8;
		short total = (short)(request->buttons.size() * buttonWidth + (request->buttons.size() - 1) * spacing);
		short x = (short)((width - total) / 2);
		for (size_t i = 0; i < request->buttons.size(); i++) {
			DWORD style = (i == request->focusButton ? BS_DEFPUSHBUTTON : BS_PUSHBUTTON) | WS_TABSTOP;
			dialog.Item(style, x, y, buttonWidth, 14, (uint16_t)(kFirstButtonId + i), kButtonClass, request->buttons[i].c_str());
			x += buttonWidth + spacing;
		}
	}
	xls::NotifySystemUi(true);
	INT_PTR rc = DialogBoxIndirectParamW(GetModuleHandleW(nullptr), dialog.Get(), TitleWindow(), DialogProc, (LPARAM)request.get());
	if (rc == -1) {
		XLS_LOG_ERROR("ui: DialogBoxIndirectParam failed (%lu).", GetLastError());
		request->outcome = ERROR_FUNCTION_FAILED;
	}
	xls::NotifySystemUi(false);
	if (request->overlapped) {
		xls::OverlappedComplete(request->overlapped, request->outcome);
	}
}

// Shows a dialog: on a worker thread when the title polls an overlapped, on the calling thread
// when it wants the answer now.
DWORD ShowDialog(std::shared_ptr<DialogRequest> request)
{
	if (request->overlapped) {
		xls::OverlappedBegin(request->overlapped);
		std::thread(RunDialog, request).detach();
		return ERROR_IO_PENDING;
	}
	RunDialog(request);
	return request->outcome;
}

// --- Steam gamepad text input --------------------------------------------------------------------

std::mutex g_gamepadMutex;
std::shared_ptr<DialogRequest> g_gamepadRequest;

bool UseGamepadTextInput()
{
	if (!xls::SteamReady() || !xls::SteamUtils()) {
		return false;
	}
#if XLS_STEAM_HARDWARE_API
	bool handheld = xls::SteamUtils()->IsRunningOnSteamHardware() != k_ESteamHardwareTypeNone;
#else
	bool handheld = xls::SteamUtils()->IsSteamRunningOnSteamDeck();
#endif
	return xls::SteamUtils()->IsSteamInBigPictureMode() || handheld;
}

void OverlayPage(const char* page)
{
	if (xls::SteamReady() && xls::SteamFriends()) {
		xls::SteamFriends()->ActivateGameOverlay(page);
	}
}

void OverlayUser(const char* page, XUID xuid)
{
	CSteamID steamId = xls::SteamIdFromXuid(xuid);
	if (xls::SteamReady() && xls::SteamFriends() && steamId.IsValid()) {
		xls::SteamFriends()->ActivateGameOverlayToUser(page, steamId);
	}
}

}

namespace xls {
namespace events {

void OnGamepadTextDismissed(bool submitted, uint32_t length)
{
	std::shared_ptr<DialogRequest> request;
	{
		std::lock_guard<std::mutex> lock(g_gamepadMutex);
		request = g_gamepadRequest;
		g_gamepadRequest.reset();
	}
	if (!request) {
		return;
	}
	if (submitted && request->textResult && request->textResultCount) {
		std::vector<char> utf8(length + 1);
		if (SteamUtils()->GetEnteredGamepadTextInput(utf8.data(), (uint32)utf8.size())) {
			std::wstring wide = Utf8ToWide(utf8.data());
			CopyStringW(request->textResult, request->textResultCount, wide.c_str());
		}
		request->outcome = ERROR_SUCCESS;
	}
	else {
		request->outcome = ERROR_CANCELLED;
	}
	NotifySystemUi(false);
	if (request->overlapped) {
		OverlappedComplete(request->overlapped, request->outcome);
	}
}

}
}

// #5206
DWORD WINAPI XShowMessagesUI(DWORD dwUserIndex)
{
	XLS_TRACE_FN();
	OverlayPage("Friends");
	return ERROR_SUCCESS;
}

// #5208
DWORD WINAPI XShowGameInviteUI(DWORD dwUserIndex, const XUID* pXuidRecipients, DWORD cRecipients, LPCWSTR lpszUnused)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	CSteamID lobby = xls::SessionPresenceLobby();
	if (!xls::SteamReady()) {
		return ERROR_NOT_LOGGED_ON;
	}
	if (!lobby.IsValid()) {
		XLS_LOG_WARN("XShowGameInviteUI: no presence session to invite to, showing the friends list.");
		OverlayPage("Friends");
		return ERROR_SUCCESS;
	}
	if (pXuidRecipients && cRecipients) {
		for (DWORD i = 0; i < cRecipients; i++) {
			CSteamID steamId = xls::SteamIdFromXuid(pXuidRecipients[i]);
			if (steamId.IsValid()) {
				xls::SteamMatchmaking()->InviteUserToLobby(lobby, steamId);
			}
		}
		return ERROR_SUCCESS;
	}
	xls::SteamFriends()->ActivateGameOverlayInviteDialog(lobby);
	return ERROR_SUCCESS;
}

// #5209
DWORD WINAPI XShowMessageComposeUI(DWORD dwUserIndex, const XUID* pXuidRecipients, DWORD cRecipients, LPCWSTR lpszText)
{
	XLS_TRACE_FN();
	if (pXuidRecipients && cRecipients) {
		OverlayUser("chat", pXuidRecipients[0]);
	}
	else {
		OverlayPage("Friends");
	}
	return ERROR_SUCCESS;
}

// #5210
DWORD WINAPI XShowFriendRequestUI(DWORD dwUserIndex, XUID xuidUser)
{
	XLS_TRACE_FN();
	OverlayUser("friendadd", xuidUser);
	return ERROR_SUCCESS;
}

// #5212
DWORD WINAPI XShowCustomPlayerListUI(DWORD dwUserIndex, DWORD dwFlags, LPCWSTR lpszTitle, LPCWSTR lpszDescription, const uint8_t* pbImage, DWORD cbImage, const XPLAYERLIST_USER* pPlayers, DWORD cPlayers, const XPLAYERLIST_BUTTON* pXButton, const XPLAYERLIST_BUTTON* pYButton, XPLAYERLIST_RESULT* pResult, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	// Session members are marked as played with, so the overlay's Players page lists them.
	OverlayPage("Players");
	if (pResult) {
		pResult->xuidSelected = INVALID_XUID;
		pResult->dwKeyCode = 0;
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_CANCELLED);
}

// #5214
DWORD WINAPI XShowPlayerReviewUI(DWORD dwUserIndex, XUID xuidFeedbackTarget)
{
	XLS_TRACE_FN();
	OverlayUser("steamid", xuidFeedbackTarget);
	return ERROR_SUCCESS;
}

// #5215
DWORD WINAPI XShowGuideUI(DWORD dwUserIndex)
{
	XLS_TRACE_FN();
	OverlayPage("Friends");
	return ERROR_SUCCESS;
}

// #5216
DWORD WINAPI XShowKeyboardUI(DWORD dwUserIndex, DWORD dwFlags, LPCWSTR lpszDefaultText, LPCWSTR lpszTitleText, LPCWSTR lpszDescriptionText, LPWSTR lpszResultText, DWORD cchResultText, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex)) {
		return ERROR_NO_SUCH_USER;
	}
	if (!lpszResultText || !cchResultText) {
		return ERROR_INVALID_PARAMETER;
	}
	auto request = std::make_shared<DialogRequest>();
	request->keyboard = true;
	request->title = lpszTitleText ? lpszTitleText : L"";
	request->description = lpszDescriptionText ? lpszDescriptionText : L"";
	request->defaultText = lpszDefaultText ? lpszDefaultText : L"";
	request->keyboardFlags = dwFlags;
	request->textResult = lpszResultText;
	request->textResultCount = cchResultText;
	request->overlapped = pOverlapped;

	if (UseGamepadTextInput()) {
		std::lock_guard<std::mutex> lock(g_gamepadMutex);
		if (!g_gamepadRequest) {
			std::string description = xls::WideToUtf8(request->description.empty() ? request->title.c_str() : request->description.c_str());
			std::string existing = xls::WideToUtf8(request->defaultText.c_str());
			EGamepadTextInputMode mode = (dwFlags & VKBD_LATIN_PASSWORD) ? k_EGamepadTextInputModePassword : k_EGamepadTextInputModeNormal;
			EGamepadTextInputLineMode lines = (dwFlags & VKBD_MULTILINE) ? k_EGamepadTextInputLineModeMultipleLines : k_EGamepadTextInputLineModeSingleLine;
			if (xls::SteamUtils()->ShowGamepadTextInput(mode, lines, description.c_str(), cchResultText - 1, existing.c_str())) {
				g_gamepadRequest = request;
				xls::NotifySystemUi(true);
				if (pOverlapped) {
					xls::OverlappedBegin(pOverlapped);
					return ERROR_IO_PENDING;
				}
				XOVERLAPPED local = {};
				request->overlapped = &local;
				xls::OverlappedBegin(&local);
				xls::AsyncWait(&local, INFINITE);
				return (DWORD)local.InternalLow;
			}
		}
	}
	if (!xls::Cfg().nativeDialogs) {
		return xls::OverlappedReturn(pOverlapped, ERROR_CANCELLED);
	}
	return ShowDialog(request);
}

// #5218
DWORD WINAPI XShowArcadeUI(DWORD dwUserIndex)
{
	XLS_TRACE_FN();
	if (xls::SteamReady()) {
		xls::SteamFriends()->ActivateGameOverlayToStore(xls::SteamAppId(), k_EOverlayToStoreFlag_None);
	}
	return ERROR_SUCCESS;
}

// #5250
DWORD WINAPI XShowAchievementsUI(DWORD dwUserIndex)
{
	XLS_TRACE_FN();
	OverlayPage("Achievements");
	return ERROR_SUCCESS;
}

// #5252
DWORD WINAPI XShowGamerCardUI(DWORD dwUserIndex, XUID xuidPlayer)
{
	XLS_TRACE_FN();
	OverlayUser("steamid", xuidPlayer);
	return ERROR_SUCCESS;
}

// #5260
DWORD WINAPI XShowSigninUI(DWORD cPanes, DWORD dwFlags)
{
	XLS_TRACE_FN();
	// The Steam user is always signed in
	xls::NotifyPost(XN_SYS_UI, TRUE);
	xls::NotifyPost(XN_SYS_SIGNINCHANGED, xls::UserSignedIn(0) ? 1 : 0);
	xls::NotifyPost(XN_SYS_UI, FALSE);
	return ERROR_SUCCESS;
}

// #5266
DWORD WINAPI XShowMessageBoxUI(DWORD dwUserIndex, LPCWSTR lpszTitle, LPCWSTR lpszText, DWORD cButtons, LPCWSTR* pwszButtons, DWORD dwFocusButton, DWORD dwFlags, MESSAGEBOX_RESULT* pResult, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (!xls::UserIndexValid(dwUserIndex) && dwUserIndex != XUSER_INDEX_ANY) {
		return ERROR_NO_SUCH_USER;
	}
	if (!cButtons || cButtons > XMB_MAXBUTTONS || !pwszButtons || dwFocusButton >= cButtons || !pResult) {
		return ERROR_INVALID_PARAMETER;
	}
	if (dwFlags & XMB_MODE_MASK) {
		// Passcode entry belonged to the Guide's parental controls.
		pResult->dwButtonPressed = (DWORD)XMB_CANCELID;
		return xls::OverlappedReturn(pOverlapped, ERROR_CANCELLED);
	}
	auto request = std::make_shared<DialogRequest>();
	request->title = lpszTitle ? lpszTitle : L"";
	request->text = lpszText ? lpszText : L"";
	for (DWORD i = 0; i < cButtons; i++) {
		request->buttons.push_back(pwszButtons[i] ? pwszButtons[i] : L"");
	}
	request->focusButton = dwFocusButton;
	request->messageResult = pResult;
	request->overlapped = pOverlapped;
	if (!xls::Cfg().nativeDialogs) {
		pResult->dwButtonPressed = dwFocusButton;
		return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
	}
	return ShowDialog(request);
}

// #5271
DWORD WINAPI XShowPlayersUI(DWORD dwUserIndex)
{
	XLS_TRACE_FN();
	OverlayPage("Players");
	return ERROR_SUCCESS;
}

// #5275
DWORD WINAPI XShowFriendsUI(DWORD dwUserIndex)
{
	XLS_TRACE_FN();
	// The in-game invite action opens Steam's invite dialog while a session exists, otherwise the
	// friends page.
	CSteamID lobby = xls::SessionPresenceLobby();
	if (lobby.IsValid() && xls::SteamReady() && xls::SteamFriends()) {
		xls::SteamFriends()->ActivateGameOverlayInviteDialog(lobby);
		return ERROR_SUCCESS;
	}
	OverlayPage("Friends");
	return ERROR_SUCCESS;
}

// #5365
DWORD WINAPI XShowMarketplaceUI(DWORD dwUserIndex, DWORD dwEntryPoint, uint64_t qwOfferId, DWORD dwContentCategories)
{
	XLS_TRACE_FN();
	if (!xls::SteamReady()) {
		return ERROR_NOT_LOGGED_ON;
	}
	// Offer ids are Steam DLC app ids in the configuration, so an item entry opens that DLC page.
	AppId_t app = (dwEntryPoint == XSHOWMARKETPLACEUI_ENTRYPOINT_CONTENTITEM || dwEntryPoint == XSHOWMARKETPLACEUI_ENTRYPOINT_CONTENTITEM_BACKGROUND) && qwOfferId && qwOfferId < 0xFFFFFFFF ? (AppId_t)qwOfferId : xls::SteamAppId();
	xls::SteamFriends()->ActivateGameOverlayToStore(app, k_EOverlayToStoreFlag_None);
	return ERROR_SUCCESS;
}

// #5366
DWORD WINAPI XShowMarketplaceDownloadItemsUI(DWORD dwUserIndex, DWORD dwEntryPoint, const uint64_t* pOfferIds, DWORD cOfferIds, HRESULT* phrResult, XOVERLAPPED* pOverlapped)
{
	XLS_TRACE_FN();
	if (xls::SteamReady() && pOfferIds && cOfferIds) {
		xls::SteamFriends()->ActivateGameOverlayToStore(pOfferIds[0] && pOfferIds[0] < 0xFFFFFFFF ? (AppId_t)pOfferIds[0] : xls::SteamAppId(), k_EOverlayToStoreFlag_None);
	}
	if (phrResult) {
		*phrResult = MPDI_E_CANCELLED;
	}
	return xls::OverlappedReturn(pOverlapped, ERROR_SUCCESS);
}

// #5298
DWORD WINAPI XLiveGetGuideKey(XINPUT_KEYSTROKE* pKeystroke)
{
	XLS_TRACE_FN();
	if (!pKeystroke) {
		return ERROR_INVALID_PARAMETER;
	}
	// Shift+Tab, the Steam overlay's default binding.
	memset(pKeystroke, 0, sizeof(*pKeystroke));
	pKeystroke->VirtualKey = VK_TAB;
	pKeystroke->Flags = 0x0004; // XINPUT_KEYSTROKE_SHIFT
	return ERROR_SUCCESS;
}

// #5299
DWORD WINAPI XShowGuideKeyRemapUI(DWORD dwUserIndex)
{
	XLS_TRACE_FN();
	OverlayPage("Settings");
	return ERROR_SUCCESS;
}
