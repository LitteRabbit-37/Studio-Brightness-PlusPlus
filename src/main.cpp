#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <initguid.h>
#include <sensorsapi.h>
#include <sensors.h>
#include <wrl/client.h>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <cwchar>
#include <atomic>
#include <gdiplus.h>

#include "hid.h"
#include "resource.h"
#include "Settings.h"
#include "OSDWindow.h"
#include "TrayPopup.h"

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "sensorsapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "gdiplus.lib")

using Microsoft::WRL::ComPtr;
using namespace Gdiplus;

/* ---------- globals ---------- */
static HINSTANCE  g_hInst              = nullptr;
static HWND       g_hMain              = nullptr;
constexpr UINT    WMAPP_NOTIFYCALLBACK = WM_APP + 1;
constexpr wchar_t kWndClass[]          = L"StudioBrightnessClass";

constexpr wchar_t kReleaseUrl[] = L"https://github.com/LitteRabbit-37/Studio-Brightness-PlusPlus/releases";
constexpr wchar_t kAppVersion[] = L"2.0.0";

/* ---------- system tray icon GUID ---------- */
DEFINE_GUID(GUID_PrinterIcon, 0x9d0b8b92, 0x4e1c, 0x488e, 0xa1, 0xe1, 0x23, 0x31, 0xaf, 0xce, 0x2c, 0xb5);

/* ---------- brightness variables ---------- */
static float baseLux                = 100.f;
static ULONG baseBrightness         = 30000;
static ULONG currentBrightness      = 30000;
static ULONG previousUserBrightness = 30000;
static ULONG minBrightness          = 1000;
static ULONG maxBrightness          = 60000;

// GDI+
static ULONG_PTR gdiplusToken;

static DisplayType g_currentDisplayType = DisplayType::None;

// Auto-adjust control & deadband
static long computeDeadband() {
    long range  = (long) std::max<ULONG>(1, maxBrightness - minBrightness);
    long byPct  = (long) std::max(1L, (long) (range * 0.03f)); // 3% of range
    long minAbs = 1500L;                                       // absolute floor
    return std::max(byPct, minAbs);
}

static void unregisterHotkeys(HWND h) {
	UnregisterHotKey(h, ID_HOTKEY_UP);
	UnregisterHotKey(h, ID_HOTKEY_DOWN);
}
static bool registerHotkeys(HWND h) {
	unregisterHotkeys(h);
	if (!g_settings.enableCustomHotkeys)
		return true;
	bool ok = true;
	if (g_settings.hkUp.vk)
		ok = ok && RegisterHotKey(h, ID_HOTKEY_UP, g_settings.hkUp.mods, g_settings.hkUp.vk);
	if (g_settings.hkDown.vk)
		ok = ok && RegisterHotKey(h, ID_HOTKEY_DOWN, g_settings.hkDown.mods, g_settings.hkDown.vk);
	return ok;
}

/* ---------- prototypes ---------- */
void             detectBrightnessRange(); // defined below
float            getAmbientLux();
INT_PTR CALLBACK OptionsDlgProc(HWND, UINT, WPARAM, LPARAM);

/* ---------- systray helpers ---------- */
BOOL AddNotificationIcon(HWND h) {
	NOTIFYICONDATA nid{sizeof(nid)};
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
BOOL DeleteNotificationIcon() {
	NOTIFYICONDATA nid{sizeof(nid)};
	nid.uFlags   = NIF_GUID;
	nid.guidItem = GUID_PrinterIcon;
	return Shell_NotifyIcon(NIM_DELETE, &nid);
}

/* ---------- mapping lux → nits ---------- */
ULONG mapLuxToBrightness(float lux) {
	float scale = std::clamp(lux, 2.f, 5000.f) / baseLux;
	float tgt   = baseBrightness * scale;
	return static_cast<ULONG>(std::clamp(tgt, float(minBrightness), float(maxBrightness)));
}

/* ---------- ambient light sensor (ALS) ---------- */
float getAmbientLux() {
	// Pro Display XDR does not support ALS via Windows Sensor API (causes hang)
	if (g_currentDisplayType == DisplayType::ProXDR)
		return 100.f;

	// Cache the reading for 500 ms to avoid creating COM objects on every
	// worker tick (~10/sec). Ambient light changes slowly enough that a
	// half-second stale reading is acceptable. These statics are written from
	// the worker thread and rarely from the main thread; on x86, aligned
	// float/DWORD writes are hardware-atomic so the benign race is safe.
	static float s_lux      = 100.f;
	static DWORD s_lastTick = 0;
	DWORD        now        = GetTickCount();
	if (now - s_lastTick < 500)
		return s_lux;
	s_lastTick = now;

	float                  lux = 100.f;
	ComPtr<ISensorManager> mgr;
	if (FAILED(CoCreateInstance(CLSID_SensorManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr))))
		return s_lux; // return cached value on COM failure

	ComPtr<ISensorCollection> col;
	if (FAILED(mgr->GetSensorsByType(SENSOR_TYPE_AMBIENT_LIGHT, &col)))
		return s_lux;

	ULONG count = 0;
	col->GetCount(&count);
	if (!count)
		return s_lux;

	ComPtr<ISensor> sensor;
	if (FAILED(col->GetAt(0, &sensor)))
		return s_lux;

	ComPtr<ISensorDataReport> rpt;
	if (FAILED(sensor->GetData(&rpt)))
		return s_lux;

	PROPVARIANT v;
	PropVariantInit(&v);
	if (SUCCEEDED(rpt->GetSensorValue(SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, &v))) {
		if (v.vt == VT_R4)
			lux = v.fltVal;
		else if (v.vt == VT_R8)
			lux = static_cast<float>(v.dblVal);
	}
	PropVariantClear(&v);
	s_lux = lux;
	return lux;
}

/* ---------- Central Brightness Setter ---------- */
// isUserAction: true if the change was initiated by the user (shortcut, slider)
//               This resets the auto-brightness baseline.
// showOSD:      true if the OSD should be displayed.
void SetBrightness(ULONG val, bool isUserAction, bool showOSD) {
    ULONG safeVal = std::clamp(val, minBrightness, maxBrightness);
    if (safeVal != currentBrightness) {
        hid_setBrightness(safeVal);
        currentBrightness = safeVal;
        
        if (isUserAction) {
            baseBrightness = safeVal;
            previousUserBrightness = safeVal;
            if (safeVal != minBrightness && safeVal != maxBrightness) {
                 baseLux = getAmbientLux();
            }
        }
        
        if (showOSD && g_settings.showOSD) {
            OSDWindow::Show((int)currentBrightness, (int)maxBrightness);
        }
    }
}

/* ---------- helpers: brightness step ---------- */
void adjustBrightnessByStep(int direction) {
	ULONG step = (maxBrightness - minBrightness) / g_settings.brightnessSteps;
	if (step < 1)
		step = 1;

	ULONG newBrightness = currentBrightness;
	if (direction > 0) {
		if (currentBrightness < maxBrightness) {
			ULONG actualStep = std::min(step, maxBrightness - currentBrightness);
			newBrightness    = currentBrightness + actualStep;
		}
	} else if (direction < 0) {
		if (currentBrightness > minBrightness) {
			ULONG actualStep = std::min(step, currentBrightness - minBrightness);
			newBrightness    = currentBrightness - actualStep;
		}
	}
    
    // Manual Step -> User Action = true, OSD = true
	if (newBrightness != currentBrightness) {
        SetBrightness(newBrightness, true, true);
	}
}

/* ---------- Options Dialog ---------- */
static void setDlgHotkey(HWND d, int ctrlId, const HotkeySpec &hk) {
	BYTE f = 0;
	if (hk.mods & MOD_CONTROL)
		f |= HOTKEYF_CONTROL;
	if (hk.mods & MOD_SHIFT)
		f |= HOTKEYF_SHIFT;
	if (hk.mods & MOD_ALT)
		f |= HOTKEYF_ALT;
	WORD w = MAKEWORD((BYTE) hk.vk, f);
	SendDlgItemMessageW(d, ctrlId, HKM_SETHOTKEY, w, 0);
}
static void getDlgHotkey(HWND d, int ctrlId, HotkeySpec &out) {
	WORD w    = (WORD) SendDlgItemMessageW(d, ctrlId, HKM_GETHOTKEY, 0, 0);
	UINT vk   = LOBYTE(w);
	BYTE f    = HIBYTE(w);
	UINT mods = 0;
	if (f & HOTKEYF_CONTROL)
		mods |= MOD_CONTROL;
	if (f & HOTKEYF_SHIFT)
		mods |= MOD_SHIFT;
	if (f & HOTKEYF_ALT)
		mods |= MOD_ALT;
	out = {mods, vk};
}

INT_PTR CALLBACK OptionsDlgProc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
	UNREFERENCED_PARAMETER(lp);
	switch (msg) {
	case WM_INITDIALOG: {
		CheckDlgButton(d, IDC_AUTO_BRIGHTNESS, g_settings.autoAdjustEnabled.load() ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_SHOW_OSD, g_settings.showOSD ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_RUN_AT_STARTUP, g_settings.runAtStartup ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_ENABLE_HOTKEYS, g_settings.enableCustomHotkeys ? BST_CHECKED : BST_UNCHECKED);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), g_settings.enableCustomHotkeys);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), g_settings.enableCustomHotkeys);
		setDlgHotkey(d, IDC_HOTKEY_UP, g_settings.hkUp);
		setDlgHotkey(d, IDC_HOTKEY_DOWN, g_settings.hkDown);
		// Initialize brightness steps spin control
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETRANGE32, kMinBrightnessSteps, kMaxBrightnessSteps);
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETPOS32, 0, g_settings.brightnessSteps);
		return TRUE;
	}
	case WM_COMMAND: {
		WORD id   = LOWORD(wp);
		WORD code = HIWORD(wp);
		if (id == IDC_ENABLE_HOTKEYS && code == BN_CLICKED) {
			BOOL en = IsDlgButtonChecked(d, IDC_ENABLE_HOTKEYS) == BST_CHECKED;
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), en);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), en);
			return TRUE;
		}
		if (id == IDC_RESET_SHORTCUTS && code == BN_CLICKED) {
			CheckDlgButton(d, IDC_ENABLE_HOTKEYS, BST_UNCHECKED);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), FALSE);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), FALSE);
			SendDlgItemMessageW(d, IDC_HOTKEY_UP, HKM_SETHOTKEY, 0, 0);
			SendDlgItemMessageW(d, IDC_HOTKEY_DOWN, HKM_SETHOTKEY, 0, 0);
			// Reset brightness steps to default
			SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETPOS32, 0, kDefaultBrightnessSteps);
			return TRUE;
		}
		if (id == IDOK) {
			g_settings.autoAdjustEnabled.store(IsDlgButtonChecked(d, IDC_AUTO_BRIGHTNESS) == BST_CHECKED);
			g_settings.showOSD = (IsDlgButtonChecked(d, IDC_SHOW_OSD) == BST_CHECKED);
			g_settings.runAtStartup        = (IsDlgButtonChecked(d, IDC_RUN_AT_STARTUP) == BST_CHECKED);
			g_settings.enableCustomHotkeys = (IsDlgButtonChecked(d, IDC_ENABLE_HOTKEYS) == BST_CHECKED);
			if (g_settings.enableCustomHotkeys) {
				getDlgHotkey(d, IDC_HOTKEY_UP, g_settings.hkUp);
				getDlgHotkey(d, IDC_HOTKEY_DOWN, g_settings.hkDown);
			} else {
				g_settings.hkUp   = {0, 0};
				g_settings.hkDown = {0, 0};
			}
			if (!registerHotkeys(g_hMain)) {
				MessageBoxW(d, L"Failed to register one or more hotkeys.", L"StudioBrightnessPlusPlus", MB_ICONERROR);
			}

			// Get brightness steps value with validation
			ULONG steps       = (ULONG) SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_GETPOS32, 0, 0);
			g_settings.brightnessSteps = std::clamp(steps, kMinBrightnessSteps, kMaxBrightnessSteps);

			// Save settings to registry
			g_settings.Save();

			// Manage startup registry key
			g_settings.SetStartup(g_settings.runAtStartup);

			EndDialog(d, IDOK);
			return TRUE;
		}
		if (id == IDCANCEL) {
			EndDialog(d, IDCANCEL);
			return TRUE;
		}
		break;
	}
	}
	return FALSE;
}

/* ---------- hidden window & WndProc ---------- */
LRESULT CALLBACK HiddenWndProc(HWND h, UINT m, WPARAM wParam, LPARAM lParam) {
	if (m == WM_HOTKEY) {
		if (wParam == ID_HOTKEY_UP) {
			adjustBrightnessByStep(+1);
			return 0;
		}
		if (wParam == ID_HOTKEY_DOWN) {
			adjustBrightnessByStep(-1);
			return 0;
		}
	}
	if (m == WM_INPUT) {
		UINT dwSize = 0;
		GetRawInputData((HRAWINPUT) lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
		if (dwSize) {
			std::vector<BYTE> lpb(dwSize);
			if (GetRawInputData((HRAWINPUT) lParam, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
				RAWINPUT *raw = (RAWINPUT *) lpb.data();
				if (raw->header.dwType == RIM_TYPEHID) {
					const RAWHID &hid = raw->data.hid;
					for (ULONG i = 0; i < hid.dwCount; ++i) {
						const BYTE *report = hid.bRawData + i * hid.dwSizeHid;
						if (hid.dwSizeHid >= 3) {
							USHORT usage = report[1] | (report[2] << 8);
							if (usage == 0x006F) { // Brightness Up
								adjustBrightnessByStep(+1);
							} else if (usage == 0x0070) { // Brightness Down
								adjustBrightnessByStep(-1);
							}
						}
					}
				}
			}
		}
		return 0;
	}
	if (m == WMAPP_NOTIFYCALLBACK) {
        // Handle Tray Click
        if (LOWORD(lParam) == WM_LBUTTONUP) {
            // Calculate current percentage
            int range = maxBrightness - minBrightness;
            if (range <= 0) range = 1;
            int pct = (int)((float)(currentBrightness - minBrightness) / (float)range * 100.0f);
            
            TrayPopup::Show(h, pct, [](int newPct) {
                // Callback from slider -> User Action = true, OSD = false (Silent)
                ULONG r = maxBrightness - minBrightness;
                ULONG val = minBrightness + (ULONG)((float)r * (float)newPct / 100.0f);
                SetBrightness(val, true, false); 
            });
            return 0;
        }
        
		if (LOWORD(lParam) == WM_RBUTTONUP) {
			HMENU hMenu = CreatePopupMenu();
            
            // Display connection status
            if (g_currentDisplayType == DisplayType::StudioDisplay) {
                AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"\U0001F7E2 Studio Display Connected");
            } else if (g_currentDisplayType == DisplayType::ProXDR) {
                AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"\U0001F7E2 Pro Display XDR Connected");
            } else {
                AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, L"\U0001F534 No Display Detected");
            }
            AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);

			AppendMenuW(hMenu, MF_STRING | (g_settings.autoAdjustEnabled.load() ? MF_CHECKED : 0), IDM_TOGGLE_AUTO,
			            L"Automatic Brightness");
			AppendMenuW(hMenu, MF_STRING, IDM_OPTIONS, L"Options...");
			AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE, L"Check update");
			wchar_t versionLabel[32];
			swprintf_s(versionLabel, L"Version %s", kAppVersion);
			AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, IDM_VERSION_INFO, versionLabel);
			AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(hMenu, MF_STRING, IDM_EXIT, L"Quit");
			POINT pt;
			GetCursorPos(&pt);
			SetForegroundWindow(h);
			int cmd = TrackPopupMenu(hMenu, TPM_RETURNCMD | TPM_NONOTIFY, pt.x, pt.y, 0, h, nullptr);
			DestroyMenu(hMenu);
			if (cmd == IDM_TOGGLE_AUTO) {
				g_settings.autoAdjustEnabled.store(!g_settings.autoAdjustEnabled.load());
				g_settings.Save();
			} else if (cmd == IDM_OPTIONS) {
				DialogBoxParamW(g_hInst, MAKEINTRESOURCE(IDD_OPTIONS), h, OptionsDlgProc, 0);
			} else if (cmd == IDM_CHECK_UPDATE) {
				auto r = ShellExecuteW(h, L"open", kReleaseUrl, nullptr, nullptr, SW_SHOWNORMAL);
				if ((UINT_PTR) r <= 32) {
					MessageBoxW(h, L"Unable to open releases page.", L"StudioBrightnessPlusPlus", MB_ICONERROR);
				}
			} else if (cmd == IDM_EXIT) {
				PostMessage(h, WM_CLOSE, 0, 0);
			}
			return 0;
		}
	}
	if (m == WM_DESTROY) {
		hid_deinit();
		DeleteNotificationIcon();
		unregisterHotkeys(h);
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProc(h, m, wParam, lParam);
}

bool RegisterHiddenClass() {
	WNDCLASSEXW wc{sizeof(wc)};
	wc.lpfnWndProc   = HiddenWndProc;
	wc.hInstance     = g_hInst;
	wc.hIcon         = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_MYICON));
	wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
	wc.hbrBackground = (HBRUSH) (COLOR_WINDOW + 1);
	wc.lpszClassName = kWndClass;
	ATOM a           = RegisterClassExW(&wc);
	if (!a) {
		wchar_t buf[128];
		wsprintf(buf, L"RegisterClassExW failed (%lu)", GetLastError());
		MessageBoxW(nullptr, buf, L"StudioBrightnessPlusPlus", MB_ICONERROR | MB_TOPMOST);
		return false;
	}
	return true;
}

/* ---------- background worker thread ---------- */
void startWorker() {
	std::thread([] {
		// Initialize COM for this thread (needed for getAmbientLux / Sensor API).
		// The thread runs indefinitely so CoUninitialize is never reached, which
		// is acceptable for a thread that lives for the process lifetime.
		CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
		for (;;) {
			/* ---------- device (re)connection attempt ---------- */
			{
				ULONG tmp;
				if (hid_getBrightness(&tmp) != 0) {
					g_currentDisplayType = DisplayType::None; // clear until reconnect succeeds
					hid_deinit();                              // close the stale handle
					if (hid_init(&g_currentDisplayType) == 0) { // attempt reconnection
						detectBrightnessRange();
						if (hid_getBrightness(&currentBrightness) == 0) {
							baseBrightness = previousUserBrightness = currentBrightness;
							baseLux                                 = getAmbientLux();
						}
					}
				}
			}

			if (g_settings.autoAdjustEnabled.load()) {
				ULONG tgt  = mapLuxToBrightness(getAmbientLux());
				long  diff = (long) tgt - (long) currentBrightness;
				long  db   = computeDeadband();
				if (diff > db) {
					diff -= db;
				} else if (diff < -db) {
					diff += db;
				} else {
					diff = 0;
				}
				if (diff) {
					long step  = std::max((long) ((maxBrightness - minBrightness) * 0.02f), 500L);
					long delta = std::clamp(diff, -step, step);
					// Auto Update -> User Action = false, OSD = false
                    SetBrightness(currentBrightness + delta, false, false);
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}).detach();
}

/* ---------- brightness range detection ---------- */
void detectBrightnessRange() {
	hid_getBrightnessRange(&minBrightness, &maxBrightness);
}

/* ---------- WinMain ---------- */
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
	// Prevent multiple instances from running simultaneously.
	// If GetLastError() == ERROR_ALREADY_EXISTS after CreateMutexW, another
	// instance already owns the mutex — just exit silently.
	HANDLE hSingleInstance = CreateMutexW(nullptr, TRUE, L"StudioBrightnessPlusPlus_SingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (hSingleInstance) CloseHandle(hSingleInstance);
		return 0;
	}

    // Initialize GDI+
    GdiplusStartupInput gdiplusStartupInput;
    GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	g_hInst = hInst;
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
	InitCommonControlsEx(&icc);

	// Load settings from registry
	g_settings.Load();
	bool realStartup = g_settings.IsStartupEnabled();
	if (g_settings.runAtStartup != realStartup) {
		if (g_settings.runAtStartup) g_settings.SetStartup(true);
	}

	hid_init(&g_currentDisplayType);
	// Ignore errors, we'll retry in the worker thread

	if (!RegisterHiddenClass()) {
		CloseHandle(hSingleInstance);
		return 1;
	}

	SetLastError(0);
	HWND h  = CreateWindowW(kWndClass, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 200, nullptr,
	                        nullptr, hInst, nullptr);
	g_hMain = h;
	if (!h) {
		wchar_t buf[128];
		wsprintf(buf, L"CreateWindowW failed (%lu)", GetLastError());
		MessageBoxW(nullptr, buf, L"StudioBrightnessPlusPlus", MB_ICONERROR | MB_TOPMOST);
		CloseHandle(hSingleInstance);
		return 1;
	}
	ShowWindow(h, SW_HIDE); // keep the window hidden

	detectBrightnessRange();
	hid_getBrightness(&currentBrightness);
	baseBrightness = previousUserBrightness = currentBrightness;
	baseLux                                 = getAmbientLux();

	RAWINPUTDEVICE rid;
	rid.usUsagePage = 0x0C; // Consumer Page
	rid.usUsage     = 0x01; // Consumer Control
	rid.dwFlags     = RIDEV_INPUTSINK;
	rid.hwndTarget  = h;
	if (!RegisterRawInputDevices(&rid, 1, sizeof(rid))) {
		MessageBoxW(nullptr, L"RegisterRawInputDevices failed", L"StudioBrightnessPlusPlus", MB_ICONERROR);
		CloseHandle(hSingleInstance);
		return 1;
	}

	if (!AddNotificationIcon(h)) {
		MessageBoxW(nullptr,
		            L"Failed to create the system tray icon.\nUse Task Manager to exit if needed.",
		            L"Studio Brightness ++", MB_ICONWARNING);
		// Continue — keyboard shortcuts still work without the tray icon.
	}
	registerHotkeys(h);
	startWorker();

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

    // Shutdown GDI+
    GdiplusShutdown(gdiplusToken);
	CoUninitialize();
	CloseHandle(hSingleInstance);
	return 0;
}
