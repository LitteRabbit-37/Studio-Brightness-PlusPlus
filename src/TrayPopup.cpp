#include "TrayPopup.h"
#include "Dpi.h"
#include <gdiplus.h>
#include <algorithm>

using namespace Gdiplus;

HWND TrayPopup::hWnd = nullptr;
int TrayPopup::m_currentPercent = 50;
TrayPopup::OnChangeCallback TrayPopup::m_callback = nullptr;
bool TrayPopup::m_isDragging = false;
bool TrayPopup::m_disabled = false;
std::wstring TrayPopup::m_note;

static const wchar_t* kTrayPopupClass = L"StudioBrightnessTrayPopup";
static int s_dpi = 96;  // DPI of the monitor the popup was last shown on

// Layout at 96 DPI, scaled to the monitor the popup opens on (see Dpi.h).
static const int kPopupWidth = 200;
static const int kPopupHeight = 40;
static const int kPopupMargin = 10; 
static const int kTrackPadding = 16;  // room for the thumb at both ends of the track
static const int kTrackHeight = 4;
static const int kThumbDiameter = 16;
static const float kNoteFontPt = 8.5f;

static int S(int v) { return DpiScale(v, s_dpi); }

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

void TrayPopup::Show(HWND, int currentPercent, OnChangeCallback cb, bool disabled, const wchar_t* note) {
    if (!hWnd) Create();

    m_currentPercent = std::clamp(currentPercent, 0, 100);
    m_callback = cb;
    m_isDragging = false;
    m_disabled = disabled;
    m_note = (disabled && note) ? note : L"";
    
    POINT pt;
    GetCursorPos(&pt);

    // Use the monitor that contains the cursor so the popup appears on the
    // correct screen in multi-monitor setups (e.g. tray on a secondary display).
    HMONITOR hMon = MonitorFromPoint(pt, MONITOR_DEFAULTTONEAREST);
    MONITORINFO mi = { sizeof(mi) };
    GetMonitorInfo(hMon, &mi);
    const RECT& work = mi.rcWork;

    // Size for that monitor before placing the popup on it.
    s_dpi = DpiOfMonitor(hMon);
    const int width = S(kPopupWidth);
    const int height = S(kPopupHeight);
    const int margin = S(kPopupMargin);

    int x = pt.x - (width / 2);
    int y = pt.y - height - margin;

    if (y < work.top)
        y = pt.y + margin;

    if (x < work.left) x = work.left;
    if (x + width > work.right) x = work.right - width;
    
    SetWindowPos(hWnd, HWND_TOPMOST, x, y, width, height, SWP_SHOWWINDOW);
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
    RECT rc;
    GetClientRect(hWnd, &rc);
    int padding = S(kTrackPadding);
    int w = rc.right - (padding * 2);
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
    
    RECT rc;
    GetClientRect(hWnd, &rc);
    const int width = rc.right;
    const int height = rc.bottom;

    // Background
    SolidBrush bgBrush(Color(255, 30, 30, 30));
    g.FillRectangle(&bgBrush, 0, 0, width, height);
    
    // Border, inset by half its width so it is not clipped
    const REAL borderW = (REAL)S(1);
    Pen borderPen(Color(100, 255, 255, 255), borderW);
    g.DrawRectangle(&borderPen, borderW / 2, borderW / 2, (REAL)width - borderW, (REAL)height - borderW);

    // Disabled (HDR) mode: show a note instead of an interactive slider.
    if (m_disabled) {
        FontFamily ff(L"Segoe UI");
        // In pixels at the monitor DPI: a size in points would follow the system DPI instead.
        Font font(&ff, kNoteFontPt * (REAL)s_dpi / 72.0f, FontStyleRegular, UnitPixel);
        SolidBrush textBrush(Color(255, 185, 185, 185));
        StringFormat fmt;
        fmt.SetAlignment(StringAlignmentCenter);
        fmt.SetLineAlignment(StringAlignmentCenter);
        RectF rect((REAL)S(6), (REAL)S(2), (REAL)(width - S(12)), (REAL)(height - S(4)));
        g.DrawString(m_note.c_str(), -1, &font, rect, &fmt, &textBrush);
        EndPaint(hWnd, &ps);
        return;
    }

    int padding = S(kTrackPadding);
    int trackH = S(kTrackHeight);
    int trackY = (height - trackH) / 2;
    int trackW = width - (padding * 2);
    
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
    int thumbDia = S(kThumbDiameter);
    int thumbX = padding + fillW - (thumbDia / 2);
    int thumbY = (height - thumbDia) / 2;
    
    g.FillEllipse(&fillBrush, thumbX, thumbY, thumbDia, thumbDia);
    
    EndPaint(hWnd, &ps);
}

LRESULT CALLBACK TrayPopup::WndProc(HWND h, UINT m, WPARAM w, LPARAM l) {
    switch (m) {
    case WM_PAINT:
        Draw();
        return 0;
    case WM_LBUTTONDOWN:
        if (!m_disabled) {
            m_isDragging = true;
            SetCapture(h);
            UpdateFromMouse(LOWORD(l));
        }
        return 0;
    case WM_MOUSEMOVE:
        if (!m_disabled && m_isDragging) {
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
    case WM_DPICHANGED:
        // Show() already sized the popup for the monitor it moves to, so the suggested
        // rectangle is not applied. Only keep drawing and hit testing in step.
        s_dpi = LOWORD(w);
        InvalidateRect(h, nullptr, FALSE);
        return 0;
    }
    return DefWindowProc(h, m, w, l);
}
