#pragma once
#include <windows.h>

class LogWindow {
public:
	static void Show();
	static void Hide();
	static bool IsVisible();

private:
	static void            Create();
	static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
	static void            Refresh();
	static void            UpdateRecordButton();
	static void            ApplyFonts();  // (re)create both fonts for dpi_ and set them on the controls
	static void            Layout();      // place the buttons and the edit control for dpi_

	static HWND     hWnd_;
	static HWND     hEdit_;
	static HWND     hBtnCopy_;
	static HWND     hBtnRecord_;
	static HWND     hBtnFolder_;
	static HFONT    hFont_;     // monospace font of the log text
	static HFONT    hUiFont_;   // font of the buttons
	static int      dpi_;       // DPI of the monitor the window is on
	static UINT_PTR timerId_;
	static size_t   nextIndex_;
};
