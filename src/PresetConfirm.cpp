#include "PresetConfirm.h"
#include <cstdio>

HWND PresetConfirm::hWnd = nullptr;
HWND PresetConfirm::m_label = nullptr;
int  PresetConfirm::m_remaining = 0;
std::function<void()> PresetConfirm::m_onRevert = nullptr;

static const wchar_t *kClass   = L"StudioBrightnessPresetConfirm";
static const UINT_PTR kTimerId = 1;
enum { ID_KEEP = 101, ID_REVERT = 102 };

void PresetConfirm::UpdateLabel() {
	if (!m_label) return;
	wchar_t buf[160];
	swprintf_s(buf, L"Keep this color preset?   Reverting in %d s...", m_remaining);
	SetWindowTextW(m_label, buf);
}

void PresetConfirm::Show(HINSTANCE hInst, int seconds, std::function<void()> onRevert) {
	// Cancel a previous prompt without reverting (a new switch supersedes it).
	if (hWnd) { m_onRevert = nullptr; DestroyWindow(hWnd); hWnd = nullptr; }

	m_onRevert  = std::move(onRevert);
	m_remaining = seconds;

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.lpszClassName = kClass;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	RegisterClassExW(&wc); // ERROR_CLASS_ALREADY_EXISTS on later calls is harmless

	const int w = 360, hgt = 132;
	RECT wa{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
	int x = (wa.left + wa.right - w) / 2;
	int y = (wa.top + wa.bottom - hgt) / 2;

	hWnd = CreateWindowExW(WS_EX_TOPMOST, kClass, L"Studio Brightness ++",
	                       WS_POPUP | WS_CAPTION | WS_SYSMENU, x, y, w, hgt, nullptr, nullptr, hInst, nullptr);
	if (!hWnd) {
		auto cb = m_onRevert; m_onRevert = nullptr;
		if (cb) cb();
		return;
	}

	HFONT hFont = (HFONT)GetStockObject(DEFAULT_GUI_FONT);
	m_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
	                          14, 22, w - 28, 24, hWnd, nullptr, hInst, nullptr);
	HWND keep   = CreateWindowExW(0, L"BUTTON", L"Keep",
	                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
	                              w - 236, 66, 104, 30, hWnd, (HMENU)ID_KEEP, hInst, nullptr);
	HWND revert = CreateWindowExW(0, L"BUTTON", L"Revert now",
	                              WS_CHILD | WS_VISIBLE | WS_TABSTOP,
	                              w - 124, 66, 108, 30, hWnd, (HMENU)ID_REVERT, hInst, nullptr);
	SendMessageW(m_label, WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessageW(keep,    WM_SETFONT, (WPARAM)hFont, TRUE);
	SendMessageW(revert,  WM_SETFONT, (WPARAM)hFont, TRUE);

	UpdateLabel();
	ShowWindow(hWnd, SW_SHOW);
	SetForegroundWindow(hWnd);
	SetFocus(keep);
	SetTimer(hWnd, kTimerId, 1000, nullptr);
}

LRESULT CALLBACK PresetConfirm::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
	switch (m) {
	case WM_TIMER:
		if (w == kTimerId) {
			if (--m_remaining <= 0) {
				auto cb = m_onRevert; m_onRevert = nullptr;
				DestroyWindow(h);
				if (cb) cb();
			} else {
				UpdateLabel();
			}
		}
		return 0;
	case WM_COMMAND:
		if (LOWORD(w) == ID_KEEP) {
			m_onRevert = nullptr; // keep: do not revert
			DestroyWindow(h);
			return 0;
		}
		if (LOWORD(w) == ID_REVERT) {
			auto cb = m_onRevert; m_onRevert = nullptr;
			DestroyWindow(h);
			if (cb) cb();
			return 0;
		}
		return 0;
	case WM_CLOSE: { // the X button counts as "not confirmed" -> revert
		auto cb = m_onRevert; m_onRevert = nullptr;
		DestroyWindow(h);
		if (cb) cb();
		return 0;
	}
	case WM_DESTROY:
		KillTimer(h, kTimerId);
		m_label = nullptr;
		if (hWnd == h) hWnd = nullptr;
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}
