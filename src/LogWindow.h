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

	static HWND     hWnd_;
	static HWND     hEdit_;
	static HWND     hBtnCopy_;
	static HWND     hBtnRecord_;
	static HWND     hBtnFolder_;
	static HFONT    hFont_;
	static UINT_PTR timerId_;
	static size_t   nextIndex_;
};
