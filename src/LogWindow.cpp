#include "LogWindow.h"
#include "Log.h"
#include "Dpi.h"
#include "resource.h"
#include <vector>
#include <string>
#include <shellapi.h>

HWND     LogWindow::hWnd_     = nullptr;
HWND     LogWindow::hEdit_    = nullptr;
HWND     LogWindow::hBtnCopy_ = nullptr;
HWND     LogWindow::hBtnRecord_ = nullptr;
HWND     LogWindow::hBtnFolder_ = nullptr;
HFONT    LogWindow::hFont_    = nullptr;
HFONT    LogWindow::hUiFont_  = nullptr;
int      LogWindow::dpi_      = 96;
UINT_PTR LogWindow::timerId_  = 0;
size_t   LogWindow::nextIndex_ = 0;

// Layout at 96 DPI, scaled to the monitor the window is on (see Dpi.h).
static const wchar_t *kLogWndClass = L"StudioBrightnessLogWindow";
static constexpr int   kWndW       = 700;
static constexpr int   kWndH       = 460;
static constexpr int   kBtnH       = 28;
static constexpr int   kBtnCopyW   = 140;
static constexpr int   kBtnRecordW = 190;
static constexpr int   kBtnFolderW = 150;
static constexpr int   kPad        = 6;
static constexpr int   kLogFontPx  = 16;
static constexpr UINT_PTR kTimerRefresh = 100;
static constexpr UINT  kRefreshMs      = 200;
static constexpr int   kBtnCopyId      = 5001;
static constexpr int   kBtnRecordId    = 5002;
static constexpr int   kBtnFolderId    = 5003;

void LogWindow::Create() {
	HINSTANCE hInst = GetModuleHandle(nullptr);

	WNDCLASSEXW wc    = {sizeof(WNDCLASSEXW)};
	wc.lpfnWndProc    = WndProc;
	wc.hInstance       = hInst;
	wc.lpszClassName   = kLogWndClass;
	wc.hCursor         = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground   = (HBRUSH)(COLOR_WINDOW + 1);
	wc.hIcon           = LoadIcon(hInst, MAKEINTRESOURCE(IDI_MYICON));
	ATOM a = RegisterClassExW(&wc);
	if (!a && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
		return;

	hWnd_ = CreateWindowExW(0, kLogWndClass, L"Studio Brightness++ \u2014 Logs",
	                        WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                        kWndW, kWndH, nullptr, nullptr, hInst, nullptr);
	if (!hWnd_)
		return;

	// Now that Windows has picked a monitor, size the window for its DPI.
	dpi_ = DpiOfWindow(hWnd_);
	SetWindowPos(hWnd_, nullptr, 0, 0, DpiScale(kWndW, dpi_), DpiScale(kWndH, dpi_),
	             SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);

	// Controls are created at zero size, Layout() places them.
	hBtnCopy_ = CreateWindowExW(0, L"BUTTON", L"Copy to Clipboard", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
	                            0, 0, 0, 0, hWnd_, (HMENU)(INT_PTR)kBtnCopyId, hInst, nullptr);
	hBtnRecord_ = CreateWindowExW(0, L"BUTTON", L"Record to file", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
	                              0, 0, 0, 0, hWnd_, (HMENU)(INT_PTR)kBtnRecordId, hInst, nullptr);
	hBtnFolder_ = CreateWindowExW(0, L"BUTTON", L"Open logs folder", WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
	                              0, 0, 0, 0, hWnd_, (HMENU)(INT_PTR)kBtnFolderId, hInst, nullptr);
	UpdateRecordButton();

	// Read-only multiline edit
	hEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
	                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
	                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
	                         0, 0, 0, 0, hWnd_, nullptr, hInst, nullptr);

	ApplyFonts();
	Layout();
}

void LogWindow::ApplyFonts() {
	HFONT oldMono = hFont_;
	HFONT oldUi   = hUiFont_;

	hFont_ = CreateFontW(-DpiScale(kLogFontPx, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                     CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");

	// The system message font at this DPI, the one dialogs use. Without a font of their own
	// the buttons fall back to the unscaled bitmap System font.
	NONCLIENTMETRICSW ncm{};
	ncm.cbSize = sizeof(ncm);
	if (SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, (UINT)dpi_))
		hUiFont_ = CreateFontIndirectW(&ncm.lfMessageFont);
	else
		hUiFont_ = CreateFontW(-DpiScale(12, dpi_), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
		                       DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
		                       CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");

	if (hEdit_ && hFont_)
		SendMessage(hEdit_, WM_SETFONT, (WPARAM)hFont_, TRUE);
	for (HWND btn : {hBtnCopy_, hBtnRecord_, hBtnFolder_})
		if (btn && hUiFont_)
			SendMessage(btn, WM_SETFONT, (WPARAM)hUiFont_, TRUE);

	// The controls hold the new fonts now, the old ones can go.
	if (oldMono) DeleteObject(oldMono);
	if (oldUi) DeleteObject(oldUi);
}

void LogWindow::Layout() {
	if (!hWnd_)
		return;
	auto S = [](int v) { return DpiScale(v, dpi_); };
	const int pad = S(kPad), btnH = S(kBtnH);
	int x = pad;
	if (hBtnCopy_)   MoveWindow(hBtnCopy_, x, pad, S(kBtnCopyW), btnH, TRUE);
	x += S(kBtnCopyW) + pad;
	if (hBtnRecord_) MoveWindow(hBtnRecord_, x, pad, S(kBtnRecordW), btnH, TRUE);
	x += S(kBtnRecordW) + pad;
	if (hBtnFolder_) MoveWindow(hBtnFolder_, x, pad, S(kBtnFolderW), btnH, TRUE);

	RECT rc;
	GetClientRect(hWnd_, &rc);
	const int editTop = pad + btnH + pad;
	if (hEdit_)
		MoveWindow(hEdit_, pad, editTop, rc.right - pad * 2, rc.bottom - editTop - pad, TRUE);
}

void LogWindow::Show() {
	if (!hWnd_)
		Create();
	if (!hWnd_)
		return;

	// Reset read position and populate
	nextIndex_ = 0;
	SetWindowTextW(hEdit_, L"");
	Refresh();

	ShowWindow(hWnd_, SW_SHOWNORMAL);
	SetForegroundWindow(hWnd_);

	if (!timerId_)
		timerId_ = SetTimer(hWnd_, kTimerRefresh, kRefreshMs, nullptr);
}

void LogWindow::Hide() {
	if (hWnd_ && IsWindowVisible(hWnd_)) {
		ShowWindow(hWnd_, SW_HIDE);
		if (timerId_) {
			KillTimer(hWnd_, kTimerRefresh);
			timerId_ = 0;
		}
	}
}

bool LogWindow::IsVisible() {
	return hWnd_ && IsWindowVisible(hWnd_);
}

void LogWindow::Refresh() {
	if (!hEdit_)
		return;

	std::vector<LogEntry> newEntries;
	size_t newNext = Log::GetEntries(newEntries, nextIndex_);
	if (newEntries.empty())
		return;
	nextIndex_ = newNext;

	// Build text for new entries
	std::wstring text;
	for (const auto &e : newEntries) {
		DWORD sec = e.tick / 1000;
		DWORD ms  = e.tick % 1000;
		DWORD h   = (sec / 3600) % 24;
		DWORD m   = (sec / 60) % 60;
		DWORD s   = sec % 60;

		const wchar_t *tag = L"INFO ";
		if (e.level == LogLevel::Warn)
			tag = L"WARN ";
		else if (e.level == LogLevel::Error)
			tag = L"ERROR";

		wchar_t line[1200];
		_snwprintf_s(line, _TRUNCATE, L"[%02u:%02u:%02u.%03u] [%s] %s\r\n",
		             h, m, s, ms, tag, e.message.c_str());
		text += line;
	}

	// Append to edit control
	int len = GetWindowTextLengthW(hEdit_);
	SendMessage(hEdit_, EM_SETSEL, len, len);
	SendMessage(hEdit_, EM_REPLACESEL, FALSE, (LPARAM)text.c_str());

	// Auto-scroll to end
	SendMessage(hEdit_, EM_SCROLLCARET, 0, 0);
}

void LogWindow::UpdateRecordButton() {
	if (!hBtnRecord_)
		return;
	static std::wstring last;
	std::wstring text;
	if (Log::FileLogActive()) {
		int mins = (Log::RemainingSeconds() + 59) / 60;
		wchar_t buf[64];
		_snwprintf_s(buf, _TRUNCATE, L"Stop recording (%d min left)", mins);
		text = buf;
	} else {
		text = L"Record to file";
	}
	if (text != last) {
		SetWindowTextW(hBtnRecord_, text.c_str());
		last = text;
	}
}

LRESULT CALLBACK LogWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_TIMER:
		if (w == kTimerRefresh) {
			Refresh();
			UpdateRecordButton();
		}
		return 0;

	case WM_COMMAND:
		if (LOWORD(w) == kBtnCopyId) {
			std::wstring all = Log::FormatRecent(500);
			if (!all.empty() && OpenClipboard(h)) {
				EmptyClipboard();
				size_t bytes = (all.size() + 1) * sizeof(wchar_t);
				HGLOBAL hg   = GlobalAlloc(GMEM_MOVEABLE, bytes);
				if (hg) {
					memcpy(GlobalLock(hg), all.c_str(), bytes);
					GlobalUnlock(hg);
					SetClipboardData(CF_UNICODETEXT, hg);
				}
				CloseClipboard();
			}
			return 0;
		}
		if (LOWORD(w) == kBtnRecordId) {
			if (Log::FileLogActive())
				Log::StopFileLog();
			else
				Log::StartFileLog(30);
			UpdateRecordButton();
			return 0;
		}
		if (LOWORD(w) == kBtnFolderId) {
			ShellExecuteW(h, L"open", Log::LogsFolderPath().c_str(), nullptr, nullptr, SW_SHOWNORMAL);
			return 0;
		}
		break;

	case WM_SIZE:
		Layout();
		return 0;

	case WM_DPICHANGED: {
		// Moved to a monitor with another scale: take the size Windows suggests for it,
		// then rebuild the fonts and the layout at the new DPI.
		dpi_ = LOWORD(w);
		ApplyFonts();
		const RECT *r = reinterpret_cast<const RECT *>(l);
		SetWindowPos(h, nullptr, r->left, r->top, r->right - r->left, r->bottom - r->top,
		             SWP_NOZORDER | SWP_NOACTIVATE);
		Layout();
		return 0;
	}

	case WM_CLOSE:
		Hide();
		return 0;

	case WM_DESTROY:
		if (hFont_) {
			DeleteObject(hFont_);
			hFont_ = nullptr;
		}
		if (hUiFont_) {
			DeleteObject(hUiFont_);
			hUiFont_ = nullptr;
		}
		hWnd_ = nullptr;
		hEdit_ = nullptr;
		hBtnCopy_ = nullptr;
		hBtnRecord_ = nullptr;
		hBtnFolder_ = nullptr;
		return 0;
	}
	return DefWindowProc(h, m, w, l);
}
