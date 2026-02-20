#pragma once
#include <windows.h>

class OSDWindow {
public:
    // Shows the OSD with the current brightness level.
    // Automatically hides after a short delay.
    static void Show(int currentLevel, int maxLevel);
    
    // Hides the OSD immediately.
    static void Hide();

private:
    static void Create();
    static LRESULT CALLBACK WndProc(HWND, UINT, WPARAM, LPARAM);
    static void UpdateVisuals();
    
    static HWND hWnd;
    static int m_currentLevel;
    static int m_maxLevel;
    static UINT_PTR m_timerId;
    static int m_posX; // top-left position on the target monitor
    static int m_posY;
};
