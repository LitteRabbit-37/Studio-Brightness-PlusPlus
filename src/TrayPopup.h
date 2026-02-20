#pragma once
#include <windows.h>
#include <functional>

class TrayPopup {
public:
    // Callback type: returns brightness percentage (0-100)
    using OnChangeCallback = std::function<void(int percent)>;
    
    // Shows the popup near the current mouse cursor.
    static void Show(HWND hParent, int currentPercent, OnChangeCallback cb);
    
    static void Hide();
    static bool IsVisible();

private:
    static void Create();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void Draw();
    static void UpdateFromMouse(int x);
    
    static HWND hWnd;
    static int m_currentPercent;
    static OnChangeCallback m_callback;
    static bool m_isDragging;
};
