#include "LogWindow.h"
#include "Log.h"
#include "resource.h"
#include <vector>
#include <string>

HWND     LogWindow::hWnd_     = nullptr;
HWND     LogWindow::hEdit_    = nullptr;
HWND     LogWindow::hBtnCopy_ = nullptr;
HFONT    LogWindow::hFont_    = nullptr;
UINT_PTR LogWindow::timerId_  = 0;
size_t   LogWindow::nextIndex_ = 0;

static const wchar_t *kLogWndClass = L"StudioBrightnessLogWindow";
static constexpr int   kWndW       = 700;
static constexpr int   kWndH       = 460;
static constexpr int   kBtnH       = 28;
static constexpr int   kBtnW       = 140;
static constexpr int   kPad        = 6;
static constexpr UINT_PTR kTimerRefresh = 100;
static constexpr UINT  kRefreshMs      = 200;
static constexpr int   kBtnCopyId      = 5001;

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

	// Copy button
	hBtnCopy_ = CreateWindowExW(0, L"BUTTON", L"Copy to Clipboard",
	                            WS_CHILD | WS_VISIBLE | BS_PUSHBUTTON,
	                            kPad, kPad, kBtnW, kBtnH,
	                            hWnd_, (HMENU)(INT_PTR)kBtnCopyId, hInst, nullptr);

	// Read-only multiline edit
	int editTop = kPad + kBtnH + kPad;
	RECT rc;
	GetClientRect(hWnd_, &rc);
	hEdit_ = CreateWindowExW(WS_EX_CLIENTEDGE, L"EDIT", L"",
	                         WS_CHILD | WS_VISIBLE | WS_VSCROLL | WS_HSCROLL |
	                             ES_MULTILINE | ES_READONLY | ES_AUTOVSCROLL | ES_AUTOHSCROLL,
	                         kPad, editTop,
	                         rc.right - kPad * 2,
	                         rc.bottom - editTop - kPad,
	                         hWnd_, nullptr, hInst, nullptr);

	// Set monospace font
	hFont_ = CreateFontW(-16, 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                      DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                      CLEARTYPE_QUALITY, FIXED_PITCH | FF_MODERN, L"Consolas");
	if (hFont_)
		SendMessage(hEdit_, WM_SETFONT, (WPARAM)hFont_, TRUE);
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

LRESULT CALLBACK LogWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_TIMER:
		if (w == kTimerRefresh)
			Refresh();
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
		break;

	case WM_SIZE: {
		RECT rc;
		GetClientRect(h, &rc);
		int editTop = kPad + kBtnH + kPad;
		if (hEdit_)
			MoveWindow(hEdit_, kPad, editTop, rc.right - kPad * 2, rc.bottom - editTop - kPad, TRUE);
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
		hWnd_ = nullptr;
		hEdit_ = nullptr;
		hBtnCopy_ = nullptr;
		return 0;
	}
	return DefWindowProc(h, m, w, l);
}
