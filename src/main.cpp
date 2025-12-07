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
#include "hid.h"
#include "resource.h"

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "sensorsapi.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "setupapi.lib")

using Microsoft::WRL::ComPtr;

/* ---------- globals ---------- */
static HINSTANCE  g_hInst              = nullptr;
static HWND       g_hMain              = nullptr;
constexpr UINT    WMAPP_NOTIFYCALLBACK = WM_APP + 1;
constexpr wchar_t kWndClass[]          = L"StudioBrightnessClass";

constexpr wchar_t kReleaseUrl[] = L"https://github.com/LitteRabbit-37/Studio-Brightness-PlusPlus/releases";
constexpr wchar_t kAppVersion[] = L"1.4.0";

/* ---------- GUID de l’icône systray ---------- */
DEFINE_GUID(GUID_PrinterIcon, 0x9d0b8b92, 0x4e1c, 0x488e, 0xa1, 0xe1, 0x23, 0x31, 0xaf, 0xce, 0x2c, 0xb5);

/* ---------- variables luminosité ---------- */
static float baseLux                = 100.f;
static ULONG baseBrightness         = 30000;
static ULONG currentBrightness      = 30000;
static ULONG previousUserBrightness = 30000;
static ULONG minBrightness          = 1000;
static ULONG maxBrightness          = 60000;

// Auto-adjust control & deadband
static std::atomic<bool> g_autoAdjustEnabled{true};
static long              computeDeadband() {
    long range  = (long) std::max<ULONG>(1, maxBrightness - minBrightness);
    long byPct  = (long) std::max(1L, (long) (range * 0.03f)); // 3% of range
    long minAbs = 1500L;                                       // absolute floor
    return std::max(byPct, minAbs);
}

// Custom hotkeys
struct HotkeySpec {
	UINT mods;
	UINT vk;
};
static bool       g_enableCustomHotkeys = false;
static HotkeySpec g_hkUp{0, 0}, g_hkDown{0, 0};
static bool       g_runAtStartup = false;

// Brightness steps (configurable, 10-50)
constexpr ULONG kDefaultBrightnessSteps = 10;
constexpr ULONG kMinBrightnessSteps     = 10;
constexpr ULONG kMaxBrightnessSteps     = 50;
static ULONG    g_brightnessSteps       = kDefaultBrightnessSteps;

static void unregisterHotkeys(HWND h) {
	UnregisterHotKey(h, ID_HOTKEY_UP);
	UnregisterHotKey(h, ID_HOTKEY_DOWN);
}
static bool registerHotkeys(HWND h) {
	unregisterHotkeys(h);
	if (!g_enableCustomHotkeys)
		return true;
	bool ok = true;
	if (g_hkUp.vk)
		ok = ok && RegisterHotKey(h, ID_HOTKEY_UP, g_hkUp.mods, g_hkUp.vk);
	if (g_hkDown.vk)
		ok = ok && RegisterHotKey(h, ID_HOTKEY_DOWN, g_hkDown.mods, g_hkDown.vk);
	return ok;
}

/* ---------- Settings persistence (Registry) ---------- */
static const wchar_t *kRegKeyPath = L"Software\\StudioBrightnessPlusPlus";

static void saveSettings() {
	HKEY  hKey;
	DWORD disp;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) ==
	    ERROR_SUCCESS) {
		DWORD autoEnabled = g_autoAdjustEnabled.load() ? 1 : 0;
		RegSetValueExW(hKey, L"AutoBrightnessEnabled", 0, REG_DWORD, (BYTE *) &autoEnabled, sizeof(DWORD));

		DWORD customEnabled = g_enableCustomHotkeys ? 1 : 0;
		RegSetValueExW(hKey, L"CustomHotkeysEnabled", 0, REG_DWORD, (BYTE *) &customEnabled, sizeof(DWORD));

		DWORD runAtStartup = g_runAtStartup ? 1 : 0;
		RegSetValueExW(hKey, L"RunAtStartup", 0, REG_DWORD, (BYTE *) &runAtStartup, sizeof(DWORD));

		RegSetValueExW(hKey, L"HotkeyUpMods", 0, REG_DWORD, (BYTE *) &g_hkUp.mods, sizeof(DWORD));
		RegSetValueExW(hKey, L"HotkeyUpVK", 0, REG_DWORD, (BYTE *) &g_hkUp.vk, sizeof(DWORD));

		RegSetValueExW(hKey, L"HotkeyDownMods", 0, REG_DWORD, (BYTE *) &g_hkDown.mods, sizeof(DWORD));
		RegSetValueExW(hKey, L"HotkeyDownVK", 0, REG_DWORD, (BYTE *) &g_hkDown.vk, sizeof(DWORD));

		RegSetValueExW(hKey, L"BrightnessSteps", 0, REG_DWORD, (BYTE *) &g_brightnessSteps, sizeof(DWORD));

		RegCloseKey(hKey);
	}
}

static void loadSettings() {
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, kRegKeyPath, 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD val, size = sizeof(DWORD);

		if (RegQueryValueExW(hKey, L"AutoBrightnessEnabled", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_autoAdjustEnabled.store(val != 0);
		}

		if (RegQueryValueExW(hKey, L"CustomHotkeysEnabled", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_enableCustomHotkeys = (val != 0);
		}

		if (RegQueryValueExW(hKey, L"RunAtStartup", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_runAtStartup = (val != 0);
		}

		if (RegQueryValueExW(hKey, L"HotkeyUpMods", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_hkUp.mods = val;
		}

		if (RegQueryValueExW(hKey, L"HotkeyUpVK", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_hkUp.vk = val;
		}

		if (RegQueryValueExW(hKey, L"HotkeyDownMods", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_hkDown.mods = val;
		}

		if (RegQueryValueExW(hKey, L"HotkeyDownVK", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_hkDown.vk = val;
		}

		if (RegQueryValueExW(hKey, L"BrightnessSteps", nullptr, nullptr, (BYTE *) &val, &size) == ERROR_SUCCESS) {
			g_brightnessSteps = std::clamp(val, kMinBrightnessSteps, kMaxBrightnessSteps);
		}

		RegCloseKey(hKey);
	}
}

/* ---------- Run at startup management ---------- */
static bool enableStartup() {
	wchar_t exePath[MAX_PATH];
	GetModuleFileNameW(nullptr, exePath, MAX_PATH);

	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) ==
	    ERROR_SUCCESS) {
		LONG result = RegSetValueExW(hKey, L"StudioBrightnessPlusPlus", 0, REG_SZ, (BYTE *) exePath,
		                             (DWORD) ((wcslen(exePath) + 1) * sizeof(wchar_t)));
		RegCloseKey(hKey);
		return result == ERROR_SUCCESS;
	}
	return false;
}

static bool disableStartup() {
	HKEY hKey;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_WRITE, &hKey) ==
	    ERROR_SUCCESS) {
		RegDeleteValueW(hKey, L"StudioBrightnessPlusPlus");
		RegCloseKey(hKey);
		return true;
	}
	return false;
}

static bool isStartupEnabled() {
	HKEY hKey;
	bool enabled = false;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\Microsoft\\Windows\\CurrentVersion\\Run", 0, KEY_READ, &hKey) ==
	    ERROR_SUCCESS) {
		wchar_t value[MAX_PATH];
		DWORD   size = sizeof(value);
		enabled      = (RegQueryValueExW(hKey, L"StudioBrightnessPlusPlus", nullptr, nullptr, (BYTE *) value, &size) ==
                   ERROR_SUCCESS);
		RegCloseKey(hKey);
	}
	return enabled;
}

/* ---------- prototypes ---------- */
void             detectBrightnessRange(); // définition plus bas
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

/* ---------- lecture capteur ALS ---------- */
float getAmbientLux() {
	float                  lux = 100.f;
	ComPtr<ISensorManager> mgr;
	if (FAILED(CoCreateInstance(CLSID_SensorManager, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&mgr))))
		return lux;

	ComPtr<ISensorCollection> col;
	if (FAILED(mgr->GetSensorsByType(SENSOR_TYPE_AMBIENT_LIGHT, &col)))
		return lux;

	ULONG count = 0;
	col->GetCount(&count);
	if (!count)
		return lux;

	ComPtr<ISensor> sensor;
	if (FAILED(col->GetAt(0, &sensor)))
		return lux;

	ComPtr<ISensorDataReport> rpt;
	if (FAILED(sensor->GetData(&rpt)))
		return lux;

	PROPVARIANT v;
	PropVariantInit(&v);
	if (SUCCEEDED(rpt->GetSensorValue(SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, &v))) {
		if (v.vt == VT_R4)
			lux = v.fltVal;
		else if (v.vt == VT_R8)
			lux = static_cast<float>(v.dblVal);
	}
	PropVariantClear(&v);
	return lux;
}

/* ---------- helpers: brightness step ---------- */
void adjustBrightnessByStep(int direction) {
	ULONG step = (maxBrightness - minBrightness) / g_brightnessSteps;
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
	if (newBrightness != currentBrightness) {
		hid_setBrightness(newBrightness);
		baseBrightness         = newBrightness;
		currentBrightness      = newBrightness;
		previousUserBrightness = newBrightness;
		if (newBrightness != minBrightness && newBrightness != maxBrightness) {
			baseLux = getAmbientLux();
		}
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
		CheckDlgButton(d, IDC_AUTO_BRIGHTNESS, g_autoAdjustEnabled.load() ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_RUN_AT_STARTUP, g_runAtStartup ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_ENABLE_HOTKEYS, g_enableCustomHotkeys ? BST_CHECKED : BST_UNCHECKED);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), g_enableCustomHotkeys);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), g_enableCustomHotkeys);
		setDlgHotkey(d, IDC_HOTKEY_UP, g_hkUp);
		setDlgHotkey(d, IDC_HOTKEY_DOWN, g_hkDown);
		// Initialize brightness steps spin control
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETRANGE32, kMinBrightnessSteps, kMaxBrightnessSteps);
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETPOS32, 0, g_brightnessSteps);
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
			g_autoAdjustEnabled.store(IsDlgButtonChecked(d, IDC_AUTO_BRIGHTNESS) == BST_CHECKED);
			g_runAtStartup        = (IsDlgButtonChecked(d, IDC_RUN_AT_STARTUP) == BST_CHECKED);
			g_enableCustomHotkeys = (IsDlgButtonChecked(d, IDC_ENABLE_HOTKEYS) == BST_CHECKED);
			if (g_enableCustomHotkeys) {
				getDlgHotkey(d, IDC_HOTKEY_UP, g_hkUp);
				getDlgHotkey(d, IDC_HOTKEY_DOWN, g_hkDown);
			} else {
				g_hkUp   = {0, 0};
				g_hkDown = {0, 0};
			}
			if (!registerHotkeys(g_hMain)) {
				MessageBoxW(d, L"Failed to register one or more hotkeys.", L"StudioBrightnessPlusPlus", MB_ICONERROR);
			}

			// Get brightness steps value with validation
			ULONG steps       = (ULONG) SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_GETPOS32, 0, 0);
			g_brightnessSteps = std::clamp(steps, kMinBrightnessSteps, kMaxBrightnessSteps);

			// Save settings to registry
			saveSettings();

			// Manage startup registry key
			if (g_runAtStartup) {
				enableStartup();
			} else {
				disableStartup();
			}

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

/* ---------- fenêtre fantôme & WndProc ---------- */
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
		if (LOWORD(lParam) == WM_RBUTTONUP) {
			HMENU hMenu = CreatePopupMenu();
			AppendMenuW(hMenu, MF_STRING | (g_autoAdjustEnabled.load() ? MF_CHECKED : 0), IDM_TOGGLE_AUTO,
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
				g_autoAdjustEnabled.store(!g_autoAdjustEnabled.load());
				saveSettings();
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
	// Passer les vrais wParam et lParam à DefWindowProc
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

/* ---------- worker ---------- */
void startWorker() {
	std::thread([] {
		for (;;) {
			/* ---------- tentative de (re)connexion ---------- */
			{
				ULONG tmp;
				if (hid_getBrightness(&tmp) != 0) {
					hid_deinit();          // ferme proprement l‘ancien handle
					if (hid_init() == 0) { // nouvel essai de connexion
						detectBrightnessRange();
						if (hid_getBrightness(&currentBrightness) == 0) {
							baseBrightness = previousUserBrightness = currentBrightness;
							baseLux                                 = getAmbientLux();
						}
					}
				}
			}

			if (g_autoAdjustEnabled.load()) {
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
					currentBrightness += delta;
					hid_setBrightness(currentBrightness);
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}).detach();
}

/* ---------- fonction manquante ---------- */
void detectBrightnessRange() {
	hid_getBrightnessRange(&minBrightness, &maxBrightness);
}

/* ---------- WinMain ---------- */
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	g_hInst = hInst;
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
	InitCommonControlsEx(&icc);

	// Load settings from registry
	loadSettings();
	// Sync g_runAtStartup with actual startup registry state
	g_runAtStartup = isStartupEnabled();

	int hid_res = hid_init();
	if (hid_res < 0 && hid_res != -10) {
		MessageBoxW(nullptr, L"hid_init failed", L"StudioBrightnessPlusPlus", MB_ICONERROR);
		return 1;
	}

	if (!RegisterHiddenClass())
		return 1;

	SetLastError(0);
	HWND h  = CreateWindowW(kWndClass, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT, 400, 200, nullptr,
	                        nullptr, hInst, nullptr);
	g_hMain = h;
	if (!h) {
		wchar_t buf[128];
		wsprintf(buf, L"CreateWindowW failed (%lu)", GetLastError());
		MessageBoxW(nullptr, buf, L"StudioBrightnessPlusPlus", MB_ICONERROR | MB_TOPMOST);
		return 1;
	}
	ShowWindow(h, SW_HIDE); // fenêtre invisible

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
		return 1;
	}
	// ---------------------------------------------------------------

	AddNotificationIcon(h);
	registerHotkeys(h);
	startWorker();

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}
	CoUninitialize();
	return 0;
}
