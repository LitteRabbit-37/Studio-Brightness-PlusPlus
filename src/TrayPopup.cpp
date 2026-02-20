#include "TrayPopup.h"
#include <gdiplus.h>
#include <algorithm>

using namespace Gdiplus;

HWND TrayPopup::hWnd = nullptr;
int TrayPopup::m_currentPercent = 50;
TrayPopup::OnChangeCallback TrayPopup::m_callback = nullptr;
bool TrayPopup::m_isDragging = false;

static const wchar_t* kTrayPopupClass = L"StudioBrightnessTrayPopup";
static const int kPopupWidth = 200;
static const int kPopupHeight = 40;
static const int kPopupMargin = 10; 

void TrayPopup::Create() {
    WNDCLASSEXW wc = { sizeof(WNDCLASSEXW) };
    wc.lpfnWndProc = WndProc;
    wc.hInstance = GetModuleHandle(nullptr);
    wc.lpszClassName = kTrayPopupClass;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);

    ATOM a = RegisterClassExW(&wc);
    if (!a && GetLastError() != ERROR_CLASS_ALREADY_EXISTS)
        return; // registration failed for an unexpected reason

    hWnd = CreateWindowExW(
        WS_EX_TOOLWINDOW | WS_EX_TOPMOST,
        kTrayPopupClass, L"",
        WS_POPUP,
        0, 0, kPopupWidth, kPopupHeight,
        nullptr, nullptr, wc.hInstance, nullptr
    );
    // hWnd may be nullptr if creation fails; Show() will retry on next call.
}

void TrayPopup::Show(HWND, int currentPercent, OnChangeCallback cb) {
    if (!hWnd) Create();
    
    m_currentPercent = std::clamp(currentPercent, 0, 100);
    m_callback = cb;
    m_isDragging = false;
    
    POINT pt;
    GetCursorPos(&pt);

    // Use the monitor that contains the cursor so the popup appears on the
    // correct screen in multi-monitor setups (e.g. tray on a secondary display).
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    const RECT& work = mi.rcWork;

    int x = pt.x - (kPopupWidth / 2);
    int y = pt.y - kPopupHeight - kPopupMargin;

    if (y < work.top)
        y = pt.y + kPopupMargin;

    if (x < work.left) x = work.left;
    if (x + kPopupWidth > work.right) x = work.right - kPopupWidth;
    
    SetWindowPos(hWnd, HWND_TOPMOST, x, y, kPopupWidth, kPopupHeight, SWP_SHOWWINDOW);
    SetForegroundWindow(hWnd);
    SetFocus(hWnd);
    
    InvalidateRect(hWnd, nullptr, FALSE);
}

void TrayPopup::Hide() {
    if (hWnd && IsWindowVisible(hWnd)) {
        ShowWindow(hWnd, SW_HIDE);
        if (m_isDragging) {
            ReleaseCapture();
            m_isDragging = false;
        }
    }
}

bool TrayPopup::IsVisible() {
    return hWnd && IsWindowVisible(hWnd);
}

void TrayPopup::UpdateFromMouse(int x) {
    int padding = 16; // More padding for thumb
    int w = kPopupWidth - (padding * 2);
    if (w <= 0) return;
    
    int relX = x - padding;
    float pct = (float)relX / (float)w;
    int newPercent = (int)(std::clamp(pct, 0.0f, 1.0f) * 100.0f);
    
    if (newPercent != m_currentPercent) {
        m_currentPercent = newPercent;
        if (m_callback) {
            m_callback(m_currentPercent);
        }
        InvalidateRect(hWnd, nullptr, FALSE);
    }
}

void TrayPopup::Draw() {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hWnd, &ps);
    
    Graphics g(hdc);
    g.SetSmoothingMode(SmoothingModeAntiAlias);
    
    // Background
    SolidBrush bgBrush(Color(255, 30, 30, 30));
    g.FillRectangle(&bgBrush, 0, 0, kPopupWidth, kPopupHeight);
    
    // Border
    Pen borderPen(Color(100, 255, 255, 255), 1.0f);
    g.DrawRectangle(&borderPen, 0, 0, kPopupWidth - 1, kPopupHeight - 1);
    
    int padding = 16;
    int trackH = 4;
    int trackY = (kPopupHeight - trackH) / 2;
    int trackW = kPopupWidth - (padding * 2);
    
    // Track Background
    SolidBrush trackBrush(Color(255, 80, 80, 80));
    g.FillRectangle(&trackBrush, padding, trackY, trackW, trackH);
    
    // Filled Part
    int fillW = (int)((float)trackW * (float)m_currentPercent / 100.0f);
    SolidBrush fillBrush(Color(255, 255, 255, 255));
    if (fillW > 0) {
        g.FillRectangle(&fillBrush, padding, trackY, fillW, trackH);
    }
    
    // Thumb
    int thumbDia = 16;
    int thumbX = padding + fillW - (thumbDia / 2);
    int thumbY = (kPopupHeight - thumbDia) / 2;
    
    g.FillEllipse(&fillBrush, thumbX, thumbY, thumbDia, thumbDia);
    
    EndPaint(hWnd, &ps);
}

LRESULT CALLBACK TrayPopup::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        Draw();
        return 0;
    case WM_LBUTTONDOWN:
        m_isDragging = true;
        SetCapture(h);
        UpdateFromMouse(LOWORD(l));
        return 0;
    case WM_MOUSEMOVE:
        if (m_isDragging) {
            POINTS pts = MAKEPOINTS(l);
            UpdateFromMouse(pts.x);
        }
        return 0;
    case WM_LBUTTONUP:
        if (m_isDragging) {
            ReleaseCapture();
            m_isDragging = false;
        }
        return 0;
    case WM_ACTIVATE:
        if (LOWORD(w) == WA_INACTIVE) {
            Hide();
        }
        return 0;
    case WM_KEYDOWN:
        if (w == VK_ESCAPE) {
            Hide();
        }
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}
