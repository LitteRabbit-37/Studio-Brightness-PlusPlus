#include "PresetConfirm.h"
#include <cstdio>

HWND  PresetConfirm::hWnd = nullptr;
HWND  PresetConfirm::m_label = nullptr;
HFONT PresetConfirm::m_font = nullptr;
int   PresetConfirm::m_remaining = 0;
std::function<void()> PresetConfirm::m_onRevert = nullptr;

static const wchar_t *kClass   = L"StudioBrightnessPresetConfirm";
static const UINT_PTR kTimerId = 1;
enum { ID_KEEP = 101, ID_REVERT = 102 };

// DPI of the monitor the window currently sits on. GetDpiForWindow exists on
// Windows 10 1607+ (this app declares PerMonitorV2); fall back to the device
// caps just in case it returns 0.
static int dpiForWindow(HWND h) {
	UINT d = GetDpiForWindow(h);
	if (d) return (int)d;
	HDC dc = GetDC(h);
	int caps = dc ? GetDeviceCaps(dc, LOGPIXELSX) : 96;
	if (dc) ReleaseDC(h, dc);
	return caps ? caps : 96;
}

void PresetConfirm::UpdateLabel() {
	if (!m_label) return;
	wchar_t buf[160];
	swprintf_s(buf, L"Keep this color preset?   Reverting in %d s...", m_remaining);
	SetWindowTextW(m_label, buf);
}

void PresetConfirm::Show(HINSTANCE hInst, int seconds, std::function<void()> onRevert) {
	// Cancel a previous prompt without reverting (a new switch supersedes it).
	Cancel();

	m_onRevert  = std::move(onRevert);
	m_remaining = seconds;

	WNDCLASSEXW wc = { sizeof(wc) };
	wc.lpfnWndProc   = WndProc;
	wc.hInstance     = hInst;
	wc.lpszClassName = kClass;
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
	RegisterClassExW(&wc); // ERROR_CLASS_ALREADY_EXISTS on later calls is harmless

	const DWORD style   = WS_POPUP | WS_CAPTION | WS_SYSMENU;
	const DWORD exStyle = WS_EX_TOPMOST;

	// Create hidden first so we can read the DPI of the monitor it lands on,
	// then size the CLIENT area and lay out the controls scaled to that DPI.
	// The app is PerMonitorV2, so the old fixed 360x132 *window* size meant the
	// title bar ate the client area on a high-DPI panel (e.g. a 5K Studio
	// Display at 200%) and pushed the buttons off the bottom.
	hWnd = CreateWindowExW(exStyle, kClass, L"Studio Brightness ++",
	                       style, 0, 0, 100, 100, nullptr, nullptr, hInst, nullptr);
	if (!hWnd) {
		auto cb = m_onRevert; m_onRevert = nullptr;
		if (cb) cb();
		return;
	}

	const int dpi = dpiForWindow(hWnd);
	auto S = [dpi](int v) { return MulDiv(v, dpi, 96); };

	// Layout in 96-dpi reference units, scaled by S().
	const int margin  = S(16);
	const int clientW = S(380);
	const int labelY  = S(20);
	const int labelH  = S(22);
	const int btnW    = S(112);
	const int btnH    = S(32);
	const int gap     = S(12);
	const int btnY    = labelY + labelH + S(18);
	const int clientH = btnY + btnH + S(16);

	// Grow the window so its CLIENT area is exactly clientW x clientH, using the
	// measured non-client overhead (avoids any SDK-version-gated DPI helper).
	RECT wr, cr;
	GetWindowRect(hWnd, &wr);
	GetClientRect(hWnd, &cr);
	const int winW = clientW + (wr.right - wr.left) - (cr.right - cr.left);
	const int winH = clientH + (wr.bottom - wr.top) - (cr.bottom - cr.top);

	RECT wa{};
	SystemParametersInfoW(SPI_GETWORKAREA, 0, &wa, 0);
	const int x = (wa.left + wa.right - winW) / 2;
	const int y = (wa.top + wa.bottom - winH) / 2;
	SetWindowPos(hWnd, HWND_TOPMOST, x, y, winW, winH, SWP_NOACTIVATE);

	// A Segoe UI font scaled to the DPI (DEFAULT_GUI_FONT is a fixed bitmap font
	// that would look tiny inside the scaled controls).
	m_font = CreateFontW(-S(15), 0, 0, 0, FW_NORMAL, FALSE, FALSE, FALSE,
	                     DEFAULT_CHARSET, OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS,
	                     CLEARTYPE_QUALITY, VARIABLE_PITCH | FF_SWISS, L"Segoe UI");
	HFONT useFont = m_font ? m_font : (HFONT)GetStockObject(DEFAULT_GUI_FONT);

	m_label = CreateWindowExW(0, L"STATIC", L"", WS_CHILD | WS_VISIBLE | SS_CENTER,
	                          margin, labelY, clientW - 2 * margin, labelH, hWnd, nullptr, hInst, nullptr);

	// Two buttons centered as a group under the label.
	const int groupW  = btnW * 2 + gap;
	const int keepX   = (clientW - groupW) / 2;
	const int revertX = keepX + btnW + gap;
	HWND keep   = CreateWindowExW(0, L"BUTTON", L"Keep",
	                              WS_CHILD | WS_VISIBLE | WS_TABSTOP | BS_DEFPUSHBUTTON,
	                              keepX, btnY, btnW, btnH, hWnd, (HMENU)ID_KEEP, hInst, nullptr);
	HWND revert = CreateWindowExW(0, L"BUTTON", L"Revert now",
	                              WS_CHILD | WS_VISIBLE | WS_TABSTOP,
	                              revertX, btnY, btnW, btnH, hWnd, (HMENU)ID_REVERT, hInst, nullptr);

	SendMessageW(m_label, WM_SETFONT, (WPARAM)useFont, TRUE);
	SendMessageW(keep,    WM_SETFONT, (WPARAM)useFont, TRUE);
	SendMessageW(revert,  WM_SETFONT, (WPARAM)useFont, TRUE);

	UpdateLabel();
	ShowWindow(hWnd, SW_SHOW);
	SetForegroundWindow(hWnd);
	SetFocus(keep);
	SetTimer(hWnd, kTimerId, 1000, nullptr);
}

void PresetConfirm::Cancel() {
	if (hWnd) {
		m_onRevert = nullptr;
		DestroyWindow(hWnd);
		hWnd = nullptr;
	}
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
		if (m_font) { DeleteObject(m_font); m_font = nullptr; }
		if (hWnd == h) hWnd = nullptr;
		return 0;
	}
	return DefWindowProcW(h, m, w, l);
}
