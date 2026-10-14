#pragma once
#include <windows.h>
#include <shellscalingapi.h>

#pragma comment(lib, "shcore.lib")

// The manifest declares the app Per-Monitor V2 aware, so Windows never stretches our windows.
// Menus and dialog resources are scaled by the system, but every window we size and draw
// ourselves has to scale itself to the DPI of the monitor it is on. Sizes in the code are
// written for 96 DPI (100 % scaling) and go through DpiScale().

inline int DpiScale(int value, int dpi) {
	return MulDiv(value, dpi, 96);
}

// Effective DPI of a monitor, or 96 when it cannot be read.
inline int DpiOfMonitor(HMONITOR mon) {
	UINT x = 0, y = 0;
	if (mon && SUCCEEDED(GetDpiForMonitor(mon, MDT_EFFECTIVE_DPI, &x, &y)) && x)
		return (int)x;
	return 96;
}

// DPI of the monitor a window currently sits on.
inline int DpiOfWindow(HWND h) {
	UINT d = h ? GetDpiForWindow(h) : 0;
	if (d)
		return (int)d;
	return DpiOfMonitor(MonitorFromWindow(h, MONITOR_DEFAULTTOPRIMARY));
}
