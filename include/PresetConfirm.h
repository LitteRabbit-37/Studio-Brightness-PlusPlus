#pragma once
#include <windows.h>
#include <functional>

// A small top-most "keep this color preset?" prompt with a countdown and Keep / Revert now buttons.
// onRevert() runs on timeout, on "Revert now", or if the window is closed; "Keep" runs nothing.
//
// The auto-revert is the safety net: it fires even if the display went blank on the preset switch
// (some Studio Display XDR units do), so the screen comes back on its own without needing a Mac.
class PresetConfirm {
public:
	static void Show(HINSTANCE hInst, int seconds, std::function<void()> onRevert);

	// Dismiss a pending prompt WITHOUT reverting (something else took over, e.g. the HDR
	// auto-rescue switched the preset out from under it). No-op when no prompt is open.
	static void Cancel();

private:
	static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
	static void UpdateLabel();

	static HWND  hWnd;
	static HWND  m_label;
	static HFONT m_font;
	static int   m_remaining;
	static int   m_appleMonitors; // Apple monitor count at Show(); a drop means the panel blanked
	static std::function<void()> m_onRevert;
};
