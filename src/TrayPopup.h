#pragma once
#include <windows.h>
#include <functional>
#include <string>

class TrayPopup {
public:
    // Callback type: returns brightness percentage (0-100)
    using OnChangeCallback = std::function<void(int percent)>;
    
    // Shows the popup near the current mouse cursor. When `disabled` is true the slider is
    // replaced by `note` text and ignores input (used to signal that brightness is owned by
    // Windows under HDR).
    static void Show(HWND hParent, int currentPercent, OnChangeCallback cb,
                     bool disabled = false, const wchar_t* note = nullptr);
    
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
    static bool m_disabled;
    static std::wstring m_note;
};
