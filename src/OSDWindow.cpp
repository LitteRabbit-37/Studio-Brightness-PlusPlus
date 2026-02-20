#include "OSDWindow.h"
#include <gdiplus.h>
#include <algorithm>

using namespace Gdiplus;

HWND OSDWindow::hWnd = nullptr;
int OSDWindow::m_currentLevel = 0;
int OSDWindow::m_maxLevel = 100;
UINT_PTR OSDWindow::m_timerId = 0;
int OSDWindow::m_posX = 0; // set in Show() based on active monitor
int OSDWindow::m_posY = 0;

static const wchar_t* kOSDClass = L"StudioBrightnessOSD";
static const int kOSDWidth = 300;
static const int kOSDHeight = 40;
static const int kMargin = 40;
static const UINT_PTR kTimerID_Hide = 1;
static const UINT kHideDelay = 2500; // ms

void OSDWindow::Create() {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kOSDClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    RegisterClassExW(&wc);

    // WS_EX_LAYERED is key for transparency
    // WS_EX_TOOLWINDOW hides it from Alt-Tab
    // WS_EX_NOACTIVATE prevents it from stealing focus
    // WS_EX_TRANSPARENT allows clicks to pass through (optional, but good for OSD)
    hWnd = CreateWindowExW(
        WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE | WS_EX_TRANSPARENT,
        kOSDClass, L"",
        WS_POPUP,
        kMargin, kMargin, kOSDWidth, kOSDHeight,
        nullptr, nullptr, wc.hInstance, nullptr
    );
}

void OSDWindow::Show(int current, int max) {
    if (!hWnd) {
        Create();
    }

    m_currentLevel = current;
    m_maxLevel = max;

    // Position the OSD on the monitor that contains the cursor (best proxy
    // for the active display when Apple display is a secondary monitor).
    POINT cursorPt;
    GetCursorPos(&cursorPt);
    HMONITOR hMon = MonitorFromPoint(cursorPt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    if (GetMonitorInfo(hMon, &mi)) {
        m_posX = mi.rcMonitor.left + kMargin;
        m_posY = mi.rcMonitor.top  + kMargin;
    }

    UpdateVisuals();
    
    // Ensure window is visible
    ShowWindow(hWnd, SW_SHOWNOACTIVATE);
    
    // Reset timer
    if (m_timerId) {
        KillTimer(hWnd, kTimerID_Hide);
    }
    m_timerId = SetTimer(hWnd, kTimerID_Hide, kHideDelay, nullptr);
}

void OSDWindow::Hide() {
    if (hWnd) {
        ShowWindow(hWnd, SW_HIDE);
        if (m_timerId) {
            KillTimer(hWnd, kTimerID_Hide);
            m_timerId = 0;
        }
    }
}

LRESULT CALLBACK OSDWindow::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    if (m == WM_TIMER && w == kTimerID_Hide) {
        Hide();
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}

void OSDWindow::UpdateVisuals() {
    if (!hWnd) return;

    // Create a bitmap for off-screen drawing
    Bitmap bitmap(kOSDWidth, kOSDHeight, PixelFormat32bppARGB);
    Graphics g(&bitmap);
    
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    g.Clear(Color(0, 0, 0, 0)); // Transparent background
    
    // Draw background (Rounded Rectangle)
    Rect rect(0, 0, kOSDWidth, kOSDHeight);
    GraphicsPath path;
    int cornerRadius = 8;
    int dia = cornerRadius * 2;
    
    // Create rounded rect path
    path.AddArc(rect.X, rect.Y, dia, dia, 180, 90);
    path.AddArc(rect.X + rect.Width - dia, rect.Y, dia, dia, 270, 90);
    path.AddArc(rect.X + rect.Width - dia, rect.Y + rect.Height - dia, dia, dia, 0, 90);
    path.AddArc(rect.X, rect.Y + rect.Height - dia, dia, dia, 90, 90);
    path.CloseFigure();
    
    // Fill background: Dark grey with transparency
    SolidBrush bgBrush(Color(220, 30, 30, 30));
    g.FillPath(&bgBrush, &path);
    
    // Draw progress bar
    float pct = (float)m_currentLevel / (float)m_maxLevel;
    if (pct < 0) pct = 0;
    if (pct > 1) pct = 1;
    
    int barMargin = 8;
    int barH = kOSDHeight - (barMargin * 2);
    int maxBarW = kOSDWidth - (barMargin * 2);
    int barW = (int)(maxBarW * pct);
    
    if (barW > 0) {
        Rect barRect(barMargin, barMargin, barW, barH);
        
        // Rounded bar logic (simplified)
        GraphicsPath barPath;
        int barCorner = 4;
        int barDia = barCorner * 2;
        
        // If bar is too small for rounded corners, just draw rect
        if (barW < barDia) {
             SolidBrush barBrush(Color(255, 255, 255, 255));
             g.FillRectangle(&barBrush, barRect);
        } else {
            barPath.AddArc(barRect.X, barRect.Y, barDia, barDia, 180, 90);
            barPath.AddArc(barRect.X + barRect.Width - barDia, barRect.Y, barDia, barDia, 270, 90);
            barPath.AddArc(barRect.X + barRect.Width - barDia, barRect.Y + barRect.Height - barDia, barDia, barDia, 0, 90);
            barPath.AddArc(barRect.X, barRect.Y + barRect.Height - barDia, barDia, barDia, 90, 90);
            barPath.CloseFigure();
            
            SolidBrush barBrush(Color(255, 255, 255, 255));
            g.FillPath(&barBrush, &barPath);
        }
    }
    
    // Update Layered Window
    HDC screenDC = GetDC(NULL);
    HDC memDC = CreateCompatibleDC(screenDC);
    HBITMAP hBitmap;
    bitmap.GetHBITMAP(Color(0, 0, 0, 0), &hBitmap);
    HBITMAP hOldBitmap = (HBITMAP)SelectObject(memDC, hBitmap);
    
    SIZE size = { kOSDWidth, kOSDHeight };
    POINT ptSrc = { 0, 0 };
    POINT ptDst = { m_posX, m_posY };
    
    BLENDFUNCTION blend = { 0 };
    blend.BlendOp = AC_SRC_OVER;
    blend.SourceConstantAlpha = 255;
    blend.AlphaFormat = AC_SRC_ALPHA;
    
    UpdateLayeredWindow(hWnd, screenDC, &ptDst, &size, memDC, &ptSrc, 0, &blend, ULW_ALPHA);
    
    SelectObject(memDC, hOldBitmap);
    DeleteObject(hBitmap);
    DeleteDC(memDC);
    ReleaseDC(NULL, screenDC);
}
