#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <hidsdi.h>
#include <hidusage.h>
#include <shellapi.h>
#include <commctrl.h>
#include <strsafe.h>
#include <initguid.h>
#include <sensorsapi.h>
#include <sensors.h>
#include <wrl/client.h>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <dwmapi.h> // for layered attributes
#pragma comment(lib,"dwmapi.lib")
#include "resource.h"
#include "hid.h"

#pragma comment(lib,"hid.lib")
#pragma comment(lib,"sensorsapi.lib")
#pragma comment(lib,"ole32.lib")
#pragma comment(lib,"setupapi.lib")

using Microsoft::WRL::ComPtr;

/* ---------- globals ---------- */
static HINSTANCE g_hInst = nullptr;
constexpr UINT   WMAPP_NOTIFYCALLBACK = WM_APP + 1;
constexpr wchar_t kWndClass[] = L"StudioBrightnessClass";

/* ---------- GUID de l’icône systray ---------- */
DEFINE_GUID(GUID_PrinterIcon,
    0x9d0b8b92,0x4e1c,0x488e,0xa1,0xe1,0x23,0x31,0xaf,0xce,0x2c,0xb5);

/* ---------- variables luminosité ---------- */
static float  baseLux = 100.f;
static ULONG  baseBrightness = 30000;
static ULONG  currentBrightness = 30000;
static ULONG  previousUserBrightness = 30000;
static ULONG  minBrightness = 1000;
static ULONG  maxBrightness = 60000;

/* ---------- répétition touche luminosité ---------- */
static const UINT TIMER_ID_BRIGHTNESS_REPEAT = 1; // ID du timer pour l’auto-repeat
static bool      g_repeatActive  = false;         // Indique si un appui est maintenu
static USHORT    g_repeatUsage   = 0;             // 0x006F (up) ou 0x0070 (down)

/* ---------- fenêtre OSD slider ---------- */
static HWND   g_hSlider              = nullptr;
static const UINT TIMER_ID_SLIDER_HIDE = 3;
static const UINT SLIDER_HIDE_DELAY_MS = 1500;
static int    g_sliderValue          = 0; // 0-100%

void ShowSlider();


/* ---------- prototypes ---------- */
void detectBrightnessRange();   // définition plus bas
float getAmbientLux();
void CreateSliderWindow();

/* ---------- systray helpers ---------- */
BOOL AddNotificationIcon(HWND h)
{
    NOTIFYICONDATA nid{ sizeof(nid) };
    nid.hWnd             = h;
    nid.uID              = 1;
    nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE | NIF_GUID;
    nid.guidItem         = GUID_PrinterIcon;
    nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
    nid.hIcon            = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_MYICON));
    wcscpy_s(nid.szTip, L"Studio Brightness ++");
    Shell_NotifyIcon(NIM_ADD, &nid);
    nid.uVersion = NOTIFYICON_VERSION_4;
    return Shell_NotifyIcon(NIM_SETVERSION, &nid);
}
BOOL DeleteNotificationIcon()
{
    NOTIFYICONDATA nid{ sizeof(nid) };
    nid.uFlags = NIF_GUID;
    nid.guidItem = GUID_PrinterIcon;
    return Shell_NotifyIcon(NIM_DELETE, &nid);
}

/* ---------- mapping lux → nits ---------- */
ULONG mapLuxToBrightness(float lux)
{
    float scale = std::clamp(lux, 2.f, 5000.f) / baseLux;
    float tgt   = baseBrightness * scale;
    return static_cast<ULONG>(std::clamp(tgt, float(minBrightness), float(maxBrightness)));
}

/* ---------- lecture capteur ALS ---------- */
float getAmbientLux()
{
    float lux = 100.f;
    ComPtr<ISensorManager> mgr;
    if (FAILED(CoCreateInstance(CLSID_SensorManager, nullptr, CLSCTX_INPROC_SERVER,
                                IID_PPV_ARGS(&mgr))))
        return lux;

    ComPtr<ISensorCollection> col;
    if (FAILED(mgr->GetSensorsByType(SENSOR_TYPE_AMBIENT_LIGHT, &col)))
        return lux;

    ULONG count = 0; col->GetCount(&count);
    if (!count) return lux;

    ComPtr<ISensor> sensor;
    if (FAILED(col->GetAt(0, &sensor))) return lux;

    ComPtr<ISensorDataReport> rpt;
    if (FAILED(sensor->GetData(&rpt))) return lux;

    PROPVARIANT v; PropVariantInit(&v);
    if (SUCCEEDED(rpt->GetSensorValue(SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX,&v))) {
        if (v.vt==VT_R4) lux = v.fltVal;
        else if (v.vt==VT_R8) lux = static_cast<float>(v.dblVal);
    }
    PropVariantClear(&v);
    return lux;
}

/* ---------- maj automatique de la référence user ---------- */
void updateUserReferenceIfChanged()
{
    ULONG cur;
    if (hid_getBrightness(&cur)==0 &&
        std::abs((long)cur-(long)previousUserBrightness) > 1000)
    {
        baseBrightness = cur;
        baseLux        = getAmbientLux();
        previousUserBrightness = cur;
    }
}

/* ---------- helpers OSD ---------- */
LRESULT CALLBACK SliderWndProc(HWND h, UINT m, WPARAM wParam, LPARAM lParam)
{
    switch (m) {
    case WM_PAINT: {
        PAINTSTRUCT ps; HDC hdc = BeginPaint(h, &ps);
        RECT rc; GetClientRect(h, &rc);
        // fond gris sombre
        HBRUSH hBg = CreateSolidBrush(RGB(60, 60, 60));
        FillRect(hdc, &rc, hBg); DeleteObject(hBg);
        // barre blanche (ou bleue ?)
        RECT fill = rc;
        fill.right = rc.left + (LONG)((rc.right - rc.left) * g_sliderValue / 100.0);
        HBRUSH hFg = CreateSolidBrush(RGB(250, 250, 250));
        FillRect(hdc, &fill, hFg); DeleteObject(hFg);
        EndPaint(h, &ps);
        return 0;
    }
    case WM_TIMER:
        if (wParam == TIMER_ID_SLIDER_HIDE) {
            ShowWindow(h, SW_HIDE);
            KillTimer(h, TIMER_ID_SLIDER_HIDE);
            return 0;
        }
        break;
    }
    return DefWindowProc(h, m, wParam, lParam);
}

void CreateSliderWindow()
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc   = SliderWndProc;
    wc.hInstance     = g_hInst;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"StudioBrightnessSliderClass";
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    RegisterClassExW(&wc);

    g_hSlider = CreateWindowExW(WS_EX_LAYERED | WS_EX_TOPMOST | WS_EX_TOOLWINDOW,
        wc.lpszClassName, L"", WS_POPUP,
        0, 0, 10, 10, nullptr, nullptr, g_hInst, nullptr);

    // alpha 220/255 (~86 %)
    SetLayeredWindowAttributes(g_hSlider, 0, 220, LWA_ALPHA);
    ShowWindow(g_hSlider, SW_HIDE);
}

void ShowSlider()
{
    if (!g_hSlider) return;
    // convert brightness to 0-100
    if (maxBrightness > minBrightness) {
        g_sliderValue = (int)((currentBrightness - minBrightness) * 100.0 / (maxBrightness - minBrightness));
        g_sliderValue = std::clamp(g_sliderValue, 0, 100);
    }
    // ---------- positionnement OSD ----------
    // Modifiez width/height ou le calcul de x/y pour ajuster la taille/position.
    const int width = 300, height = 24;           // <- largeur/hauteur du slider
    int sx = GetSystemMetrics(SM_CXSCREEN);
    int sy = GetSystemMetrics(SM_CYSCREEN);
    int x  = (sx - width) / 2;                    // centré horizontalement
    int y  = sy - height - 110;                    // 110 px au-dessus du bas

    // Affiche puis amène au sommet de la bande TOPMOST (au-dessus de l’OSD Windows)
    ShowWindow(g_hSlider, SW_SHOWNOACTIVATE);
    SetWindowPos(g_hSlider, HWND_TOPMOST, x, y, width, height, SWP_NOACTIVATE | SWP_SHOWWINDOW);

    InvalidateRect(g_hSlider, nullptr, TRUE);
    SetTimer(g_hSlider, TIMER_ID_SLIDER_HIDE, SLIDER_HIDE_DELAY_MS, nullptr);
}

/* ---------- fenêtre fantôme & WndProc ---------- */
LRESULT CALLBACK HiddenWndProc(HWND h, UINT m, WPARAM wParam, LPARAM lParam)
{
    if (m == WM_INPUT) {
        UINT dwSize = 0;
        GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
        if (dwSize) {
            std::vector<BYTE> lpb(dwSize);
            if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
                RAWINPUT* raw = (RAWINPUT*)lpb.data();
                if (raw->header.dwType == RIM_TYPEHID) {
                    const RAWHID& hid = raw->data.hid;
                    for (ULONG i = 0; i < hid.dwCount; ++i) {
                        const BYTE* report = hid.bRawData + i * hid.dwSizeHid;
                        if (hid.dwSizeHid >= 3) {
                            USHORT usage = report[1] | (report[2] << 8);
                            ULONG steps = 16;
                            ULONG step = (maxBrightness - minBrightness) / steps;
                            if (step < 1) step = 1;

                            ULONG newBrightness = currentBrightness;
                            if (usage == 0x006F) { // Brightness Up
                                if (currentBrightness < maxBrightness) {
                                    ULONG actualStep = std::min(step, maxBrightness - currentBrightness);
                                    newBrightness = currentBrightness + actualStep;
                                }
                            } else if (usage == 0x0070) { // Brightness Down
                                if (currentBrightness > minBrightness) {
                                    ULONG actualStep = std::min(step, currentBrightness - minBrightness);
                                    newBrightness = currentBrightness - actualStep;
                                }
                            }
                            if (newBrightness != currentBrightness) {
                                hid_setBrightness(newBrightness);
                                baseBrightness = newBrightness;
                                currentBrightness = newBrightness;
                                previousUserBrightness = newBrightness;
                                ShowSlider();
                                // Ne recale la référence que si PAS en butée
                                if (newBrightness != minBrightness && newBrightness != maxBrightness) {
                                    baseLux = getAmbientLux();
                                }
                            }
                            // ---------- gestion du maintien de la touche ----------
                            if (usage == 0x006F || usage == 0x0070) {
                                // Démarre ou met à jour le timer si différent
                                if (!g_repeatActive || g_repeatUsage != usage) {
                                    g_repeatUsage  = usage;
                                    g_repeatActive = true;
                                    SetTimer(h, TIMER_ID_BRIGHTNESS_REPEAT, 80, nullptr); // ~12 Hz
                                }
                            } else if (g_repeatActive) {
                                // Relâchement de la touche
                                KillTimer(h, TIMER_ID_BRIGHTNESS_REPEAT);
                                g_repeatActive = false;
                            }
                        }
                    }
                }
            }
        }
        return 0;
    }
    if (m == WM_TIMER && wParam == TIMER_ID_BRIGHTNESS_REPEAT) {
        // ---------- répétition automatique ----------
        ULONG steps = 16;
        ULONG step  = (maxBrightness - minBrightness) / steps;
        if (step < 1) step = 1;

        ULONG newBrightness = currentBrightness;
        if (g_repeatUsage == 0x006F) { // Brightness Up
            if (currentBrightness < maxBrightness) {
                ULONG actualStep = std::min(step, maxBrightness - currentBrightness);
                newBrightness = currentBrightness + actualStep;
            }
        } else if (g_repeatUsage == 0x0070) { // Brightness Down
            if (currentBrightness > minBrightness) {
                ULONG actualStep = std::min(step, currentBrightness - minBrightness);
                newBrightness = currentBrightness - actualStep;
            }
        }
        if (newBrightness != currentBrightness) {
            hid_setBrightness(newBrightness);
            baseBrightness         = newBrightness;
            currentBrightness      = newBrightness;
            previousUserBrightness = newBrightness;
            ShowSlider();
            if (newBrightness != minBrightness && newBrightness != maxBrightness) {
                baseLux = getAmbientLux();
            }
        } else {
            // Arrêt si l’on atteint la butée
            KillTimer(h, TIMER_ID_BRIGHTNESS_REPEAT);
            g_repeatActive = false;
        }
        return 0;
    }

    if (m == WMAPP_NOTIFYCALLBACK) {
        if (LOWORD(lParam) == WM_RBUTTONUP) {
            HMENU hMenu = CreatePopupMenu();
            AppendMenuW(hMenu, MF_STRING, 1, L"Quit");
            POINT pt; GetCursorPos(&pt);
            SetForegroundWindow(h);
            int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
            DestroyMenu(hMenu);
            if (cmd == 1) {
                PostMessage(h, WM_CLOSE, 0, 0);
            }
            return 0;
        }
    }
    if (m==WM_DESTROY) {
        if (g_repeatActive) KillTimer(h, TIMER_ID_BRIGHTNESS_REPEAT);
        hid_deinit();
        DeleteNotificationIcon();
        PostQuitMessage(0);
        return 0;
    }
    // Passer les vrais wParam et lParam à DefWindowProc
    return DefWindowProc(h, m, wParam, lParam);
}

bool RegisterHiddenClass()
{
    WNDCLASSEXW wc{ sizeof(wc) };
    wc.lpfnWndProc  = HiddenWndProc;
    wc.hInstance    = g_hInst;
    wc.hIcon        = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_MYICON));
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);  
    wc.lpszClassName= kWndClass;
    ATOM a = RegisterClassExW(&wc);
    if (!a) {
        wchar_t buf[128]; wsprintf(buf,L"RegisterClassExW failed (%lu)",GetLastError());
        MessageBoxW(nullptr,buf,L"StudioBrightnessPlusPlus",MB_ICONERROR|MB_TOPMOST);
        return false;
    }
    return true;
}

/* ---------- worker ---------- */
void startWorker()
{
    std::thread([] {
        for (;;) {
            /* ---------- tentative de (re)connexion ---------- */
            {
                ULONG tmp;
                if (hid_getBrightness(&tmp) != 0) {
                    hid_deinit();                      // ferme proprement l‘ancien handle
                    if (hid_init() == 0) {             // nouvel essai de connexion
                        detectBrightnessRange();
                        if (hid_getBrightness(&currentBrightness) == 0) {
                            baseBrightness = previousUserBrightness = currentBrightness;
                            baseLux = getAmbientLux();
                        }
                    }
                }
            }

            ULONG tgt = mapLuxToBrightness(getAmbientLux());
            long diff = (long)tgt - (long)currentBrightness;
            if (diff) {
                long step  = std::max((long)(maxBrightness * 0.02f), 500L);
                long delta = std::clamp(diff, -step, step);
                currentBrightness += delta;
                hid_setBrightness(currentBrightness);
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
        }
    }).detach();
}

/* ---------- fonction manquante ---------- */
void detectBrightnessRange()
{
    hid_getBrightnessRange(&minBrightness,&maxBrightness);
}

/* ---------- WinMain ---------- */
int APIENTRY wWinMain(HINSTANCE hInst,HINSTANCE, PWSTR, int)
{
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
    g_hInst = hInst;

    int hid_res = hid_init();
    if (hid_res < 0 && hid_res != -10) {
        MessageBoxW(nullptr,L"hid_init failed",L"StudioBrightnessPlusPlus",MB_ICONERROR);
        return 1;
    }

    if (!RegisterHiddenClass()) return 1;

    SetLastError(0);   
    HWND h = CreateWindowW(kWndClass,L"",WS_OVERLAPPEDWINDOW,
                           CW_USEDEFAULT,CW_USEDEFAULT,400,200,
                           nullptr,nullptr,hInst,nullptr);
    DWORD err = GetLastError();
    if (!h){
        wchar_t buf[128]; wsprintf(buf,L"CreateWindowW failed (%lu)",GetLastError());
        MessageBoxW(nullptr,buf,L"StudioBrightnessPlusPlus",MB_ICONERROR|MB_TOPMOST);
        return 1;
    }
    ShowWindow(h,SW_HIDE);           // fenêtre invisible
    CreateSliderWindow();

    detectBrightnessRange();
    hid_getBrightness(&currentBrightness);
    baseBrightness = previousUserBrightness = currentBrightness;
    baseLux = getAmbientLux();

    // ----------- AJOUTER L'ENREGISTREMENT RAW INPUT ICI -------------
    RAWINPUTDEVICE rid;
    rid.usUsagePage = 0x0C; // Consumer Page
    rid.usUsage     = 0x01; // Consumer Control
    rid.dwFlags     = RIDEV_INPUTSINK;
    rid.hwndTarget  = h;
    if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
        MessageBoxW(nullptr, L"RegisterRawInputDevices failed", L"StudioBrightnessPlusPlus", MB_ICONERROR);
        return 1;
    }
    // ---------------------------------------------------------------

    AddNotificationIcon(h);
    startWorker();

    MSG msg;
    while (GetMessage(&msg,nullptr,0,0)) {
        TranslateMessage(&msg);
        DispatchMessage(&msg);
    }
    CoUninitialize();
    return 0;
}
