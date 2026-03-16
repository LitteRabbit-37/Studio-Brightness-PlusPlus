#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <commctrl.h>
#include <initguid.h>
#include <sensorsapi.h>
#include <sensors.h>
#include <devpkey.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <portabledevicetypes.h>
#include <wrl/client.h>
#include <thread>
#include <chrono>
#include <algorithm>
#include <vector>
#include <cwchar>
#include <atomic>
#include <mutex>
#include <gdiplus.h>

#include "hid.h"
#include "resource.h"
#include "Settings.h"
#include "OSDWindow.h"
#include "TrayPopup.h"
#include "Log.h"
#include "LogWindow.h"

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
constexpr wchar_t kAppVersion[] = L"2.1.1";

/* ---------- system tray icon GUID ---------- */
DEFINE_GUID(GUID_PrinterIcon, 0x9d0b8b92, 0x4e1c, 0x488e, 0xa1, 0xe1, 0x23, 0x31, 0xaf, 0xce, 0x2c, 0xb5);

/* ---------- multi-display state ---------- */
static std::vector<DisplayDevice> g_displays;
static std::mutex                 g_displayMutex;

// GDI+
static ULONG_PTR gdiplusToken;

// Auto-adjust control
static long computeDeadband(ULONG minB, ULONG maxB) {
	long range = (long)std::max<ULONG>(1, maxB - minB);
	long byPct = std::max(1L, (long)(range * 0.03f));
	return std::max(byPct, 1500L);
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

/* ---------- ALS via ISensorEvents ---------- */
class AlsSensorListener : public ISensorEvents {
public:
	AlsSensorListener() : refCount_(1) {}

	// IUnknown
	STDMETHODIMP QueryInterface(REFIID riid, void **ppv) override {
		if (riid == IID_IUnknown || riid == __uuidof(ISensorEvents)) {
			*ppv = static_cast<ISensorEvents *>(this);
			AddRef();
			return S_OK;
		}
		*ppv = nullptr;
		return E_NOINTERFACE;
	}
	STDMETHODIMP_(ULONG) AddRef() override { return InterlockedIncrement(&refCount_); }
	STDMETHODIMP_(ULONG) Release() override {
		ULONG c = InterlockedDecrement(&refCount_);
		if (c == 0) delete this;
		return c;
	}

	// ISensorEvents
	STDMETHODIMP OnStateChanged(ISensor *, SensorState) override { return S_OK; }
	STDMETHODIMP OnDataUpdated(ISensor *, ISensorDataReport *pReport) override {
		if (!pReport) return S_OK;
		PROPVARIANT v;
		PropVariantInit(&v);
		if (SUCCEEDED(pReport->GetSensorValue(SENSOR_DATA_TYPE_LIGHT_LEVEL_LUX, &v))) {
			float lux = 100.f;
			if (v.vt == VT_R4)
				lux = v.fltVal;
			else if (v.vt == VT_R8)
				lux = static_cast<float>(v.dblVal);
			lastLux_.store(lux, std::memory_order_relaxed);
			alive_.store(true, std::memory_order_relaxed);
		}
		PropVariantClear(&v);
		return S_OK;
	}
	STDMETHODIMP OnEvent(ISensor *, REFGUID, IPortableDeviceValues *) override { return S_OK; }
	STDMETHODIMP OnLeave(REFSENSOR_ID) override { return S_OK; }

	float    lux()     const { return lastLux_.load(std::memory_order_relaxed); }
	bool     alive()   const { return alive_.load(std::memory_order_relaxed); }

	GUID     containerId = {};
	ComPtr<ISensor> sensor;

private:
	LONG              refCount_;
	std::atomic<float> lastLux_{100.f};
	std::atomic<bool>  alive_{false};
};

static std::vector<AlsSensorListener *> g_alsListeners;
static std::mutex                       g_alsMutex;

// Extract ContainerId from a sensor device path by looking up its parent chain.
// The sensor path typically contains the HID instance ID which shares a ContainerId
// with the display's USB composite device.
static GUID getContainerIdFromDevicePath(const std::wstring &devPath) {
	GUID cid = {};

	if (!StrStrIW(devPath.c_str(), L"VID_05AC"))
		return cid;

	// Sensor device path: \\?\HID#VID_05AC&PID_xxxx&MI_08#inst#{guid}\{sub}
	// Instance ID:         HID\VID_05AC&PID_xxxx&MI_08\inst
	// Match by converting instId separators (\->#) and checking if it appears in devPath.
	static const GUID GUID_DEVCLASS_SENSOR = {0x5175D334, 0xC371, 0x4806,
	                                           {0xB3, 0xBA, 0x71, 0xFD, 0x53, 0xC9, 0x25, 0x8D}};

	HDEVINFO set = SetupDiGetClassDevsW(&GUID_DEVCLASS_SENSOR, nullptr, 0, DIGCF_PRESENT);
	if (set == INVALID_HANDLE_VALUE) return cid;

	SP_DEVINFO_DATA devInfo{sizeof(devInfo)};
	for (DWORD i = 0; SetupDiEnumDeviceInfo(set, i, &devInfo); ++i) {
		wchar_t instId[512] = {};
		if (!SetupDiGetDeviceInstanceIdW(set, &devInfo, instId, 512, nullptr))
			continue;

		// Convert backslashes to # to match device path format
		std::wstring normalized = instId;
		for (auto &ch : normalized)
			if (ch == L'\\') ch = L'#';

		// Check if this instance ID matches the sensor device path
		if (!StrStrIW(devPath.c_str(), normalized.c_str()))
			continue;

		DWORD type = 0, size = 0;
		SetupDiGetDevicePropertyW(set, &devInfo, &DEVPKEY_Device_ContainerId,
		                          &type, nullptr, 0, &size, 0);
		if (size == sizeof(GUID)) {
			SetupDiGetDevicePropertyW(set, &devInfo, &DEVPKEY_Device_ContainerId,
			                          &type, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
			break;
		}
	}

	SetupDiDestroyDeviceInfoList(set);
	return cid;
}

static void initAlsSensors() {
	ComPtr<ISensorManager> mgr;
	if (FAILED(CoCreateInstance(CLSID_SensorManager, nullptr, CLSCTX_INPROC_SERVER,
	                            IID_PPV_ARGS(&mgr)))) {
		Log::Warn(L"ALS: Failed to create SensorManager");
		return;
	}

	ComPtr<ISensorCollection> col;
	if (FAILED(mgr->GetSensorsByType(SENSOR_TYPE_AMBIENT_LIGHT, &col))) {
		Log::Info(L"ALS: No ambient light sensors found");
		return;
	}

	ULONG count = 0;
	col->GetCount(&count);
	Log::Info(L"ALS: Found %lu ambient light sensor(s)", count);

	std::lock_guard<std::mutex> lock(g_alsMutex);

	for (ULONG i = 0; i < count; ++i) {
		ComPtr<ISensor> sensor;
		if (FAILED(col->GetAt(i, &sensor)))
			continue;

		// Get sensor ID for logging
		SENSOR_ID sid = {};
		sensor->GetID(&sid);

		// Get device path for ContainerId correlation
		PROPVARIANT pvPath;
		PropVariantInit(&pvPath);

		std::wstring sensorInfo = L"(unknown)";
		if (SUCCEEDED(sensor->GetProperty(SENSOR_PROPERTY_FRIENDLY_NAME, &pvPath))) {
			if (pvPath.vt == VT_LPWSTR && pvPath.pwszVal)
				sensorInfo = pvPath.pwszVal;
		}
		PropVariantClear(&pvPath);

		// Try to get device path for ContainerId matching
		GUID cid = {};
		PropVariantInit(&pvPath);
		if (SUCCEEDED(sensor->GetProperty(SENSOR_PROPERTY_DEVICE_PATH, &pvPath))) {
			if (pvPath.vt == VT_LPWSTR && pvPath.pwszVal) {
				std::wstring devPath = pvPath.pwszVal;
				cid = getContainerIdFromDevicePath(devPath);
				Log::Info(L"ALS: Sensor %lu device path: %s", i, devPath.c_str());
			}
		}
		PropVariantClear(&pvPath);

		auto *listener       = new AlsSensorListener();
		listener->sensor     = sensor;
		listener->containerId = cid;

		HRESULT hr = sensor->SetEventSink(listener);
		if (SUCCEEDED(hr)) {
			// Request ~500ms update interval
			ComPtr<IPortableDeviceValues> params;
			if (SUCCEEDED(CoCreateInstance(CLSID_PortableDeviceValues, nullptr,
			                               CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&params)))) {
				params->SetUnsignedIntegerValue(SENSOR_PROPERTY_CURRENT_REPORT_INTERVAL, 500);
				sensor->SetProperties(params.Get(), nullptr);
			}
			Log::Info(L"ALS: Registered listener for sensor %lu: %s", i, sensorInfo.c_str());
			g_alsListeners.push_back(listener);
		} else {
			Log::Warn(L"ALS: SetEventSink failed for sensor %lu (0x%08X)", i, hr);
			listener->Release();
		}
	}
}

static void cleanupAlsSensors() {
	std::lock_guard<std::mutex> lock(g_alsMutex);
	for (auto *l : g_alsListeners) {
		if (l->sensor)
			l->sensor->SetEventSink(nullptr);
		l->Release();
	}
	g_alsListeners.clear();
}

// Get lux from the best available sensor for a given display.
// If the display has an ALS (matched by ContainerId), use it.
// Otherwise use the master sensor (index 0 or user-selected).
static float getAmbientLux(const DisplayDevice &dev) {
	std::lock_guard<std::mutex> lock(g_alsMutex);
	if (g_alsListeners.empty())
		return 100.f;

	// Try to find a sensor matching this display's ContainerId
	GUID zero = {};
	if (memcmp(&dev.containerId, &zero, sizeof(GUID)) != 0) {
		for (auto *l : g_alsListeners) {
			if (memcmp(&l->containerId, &dev.containerId, sizeof(GUID)) == 0 && l->alive())
				return l->lux();
		}
	}

	// Fallback: use first alive sensor (master)
	for (auto *l : g_alsListeners) {
		if (l->alive())
			return l->lux();
	}

	return 100.f;
}

/* ---------- mapping lux to brightness ---------- */
static ULONG mapLuxToBrightness(float lux, const DisplayDevice &dev) {
	float scale = std::clamp(lux, 2.f, 5000.f) / dev.baseLux;
	float tgt   = dev.baseBrightness * scale;
	return static_cast<ULONG>(std::clamp(tgt, (float)dev.minBrightness, (float)dev.maxBrightness));
}

/* ---------- Central Brightness Setter ---------- */
static void SetBrightness(DisplayDevice &dev, ULONG val, bool isUserAction, bool showOSD) {
	ULONG safeVal = std::clamp(val, dev.minBrightness, dev.maxBrightness);
	if (safeVal != dev.currentBrightness) {
		int rc = dev.setBrightness(safeVal);
		if (rc != 0) {
			Log::Warn(L"setBrightness failed on %s (rc=%d)", dev.name.c_str(), rc);
			return;
		}
		dev.currentBrightness = safeVal;

		if (isUserAction) {
			dev.baseBrightness = safeVal;
			if (safeVal != dev.minBrightness && safeVal != dev.maxBrightness)
				dev.baseLux = getAmbientLux(dev);
		}

		if (showOSD && g_settings.showOSD)
			OSDWindow::Show((int)dev.currentBrightness, (int)dev.maxBrightness);
	}
}

// Apply brightness to all connected displays (linked mode)
static void SetBrightnessAll(ULONG val, bool isUserAction, bool showOSD) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	for (auto &dev : g_displays) {
		// Map val from first display's range to this display's range
		SetBrightness(dev, val, isUserAction, showOSD);
	}
}

/* ---------- helpers: brightness step ---------- */
static void adjustBrightnessByStep(int direction) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty()) return;

	// Use first display as reference
	auto &ref = g_displays[0];
	ULONG step = (ref.maxBrightness - ref.minBrightness) / g_settings.brightnessSteps;
	if (step < 1) step = 1;

	ULONG newBrightness = ref.currentBrightness;
	if (direction > 0) {
		if (ref.currentBrightness < ref.maxBrightness) {
			ULONG actualStep = std::min(step, ref.maxBrightness - ref.currentBrightness);
			newBrightness    = ref.currentBrightness + actualStep;
		}
	} else if (direction < 0) {
		if (ref.currentBrightness > ref.minBrightness) {
			ULONG actualStep = std::min(step, ref.currentBrightness - ref.minBrightness);
			newBrightness    = ref.currentBrightness - actualStep;
		}
	}

	if (newBrightness != ref.currentBrightness) {
		for (auto &dev : g_displays)
			SetBrightness(dev, newBrightness, true, true);
	}
}

/* ---------- Options Dialog ---------- */
static void setDlgHotkey(HWND d, int ctrlId, const HotkeySpec &hk) {
	BYTE f = 0;
	if (hk.mods & MOD_CONTROL) f |= HOTKEYF_CONTROL;
	if (hk.mods & MOD_SHIFT)   f |= HOTKEYF_SHIFT;
	if (hk.mods & MOD_ALT)     f |= HOTKEYF_ALT;
	WORD w = MAKEWORD((BYTE)hk.vk, f);
	SendDlgItemMessageW(d, ctrlId, HKM_SETHOTKEY, w, 0);
}
static void getDlgHotkey(HWND d, int ctrlId, HotkeySpec &out) {
	WORD w    = (WORD)SendDlgItemMessageW(d, ctrlId, HKM_GETHOTKEY, 0, 0);
	UINT vk   = LOBYTE(w);
	BYTE f    = HIBYTE(w);
	UINT mods = 0;
	if (f & HOTKEYF_CONTROL) mods |= MOD_CONTROL;
	if (f & HOTKEYF_SHIFT)   mods |= MOD_SHIFT;
	if (f & HOTKEYF_ALT)     mods |= MOD_ALT;
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
			SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_SETPOS32, 0, kDefaultBrightnessSteps);
			return TRUE;
		}
		if (id == IDOK) {
			g_settings.autoAdjustEnabled.store(IsDlgButtonChecked(d, IDC_AUTO_BRIGHTNESS) == BST_CHECKED);
			g_settings.showOSD            = (IsDlgButtonChecked(d, IDC_SHOW_OSD) == BST_CHECKED);
			g_settings.runAtStartup        = (IsDlgButtonChecked(d, IDC_RUN_AT_STARTUP) == BST_CHECKED);
			g_settings.enableCustomHotkeys = (IsDlgButtonChecked(d, IDC_ENABLE_HOTKEYS) == BST_CHECKED);
			if (g_settings.enableCustomHotkeys) {
				getDlgHotkey(d, IDC_HOTKEY_UP, g_settings.hkUp);
				getDlgHotkey(d, IDC_HOTKEY_DOWN, g_settings.hkDown);
			} else {
				g_settings.hkUp   = {0, 0};
				g_settings.hkDown = {0, 0};
			}
			if (!registerHotkeys(g_hMain))
				MessageBoxW(d, L"Failed to register one or more hotkeys.", L"StudioBrightnessPlusPlus", MB_ICONERROR);

			ULONG steps            = (ULONG)SendDlgItemMessageW(d, IDC_BRIGHTNESS_STEPS_SPIN, UDM_GETPOS32, 0, 0);
			g_settings.brightnessSteps = std::clamp(steps, kMinBrightnessSteps, kMaxBrightnessSteps);
			g_settings.Save();
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

/* ---------- helpers for tray menu display list ---------- */
static std::wstring buildDisplayStatusLine() {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty())
		return L"\U0001F534 No Display Detected";

	if (g_displays.size() == 1)
		return std::wstring(L"\U0001F7E2 ") + g_displays[0].name + L" Connected";

	// Multiple displays
	std::wstring line = L"\U0001F7E2 ";
	line += std::to_wstring(g_displays.size());
	line += L" Displays Connected";
	return line;
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
		GetRawInputData((HRAWINPUT)lParam, RID_INPUT, nullptr, &dwSize, sizeof(RAWINPUTHEADER));
		if (dwSize) {
			std::vector<BYTE> lpb(dwSize);
			if (GetRawInputData((HRAWINPUT)lParam, RID_INPUT, lpb.data(), &dwSize, sizeof(RAWINPUTHEADER)) == dwSize) {
				RAWINPUT *raw = (RAWINPUT *)lpb.data();
				if (raw->header.dwType == RIM_TYPEHID) {
					const RAWHID &hid = raw->data.hid;
					for (ULONG i = 0; i < hid.dwCount; ++i) {
						const BYTE *report = hid.bRawData + i * hid.dwSizeHid;
						if (hid.dwSizeHid >= 3) {
							USHORT usage = report[1] | (report[2] << 8);
							if (usage == 0x006F)
								adjustBrightnessByStep(+1);
							else if (usage == 0x0070)
								adjustBrightnessByStep(-1);
						}
					}
				}
			}
		}
		return 0;
	}
	if (m == WMAPP_NOTIFYCALLBACK) {
		if (LOWORD(lParam) == WM_LBUTTONUP) {
			int   pct    = 50;
			ULONG refMin = 1000;
			ULONG refMax = 60000;
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);
				if (g_displays.empty()) return 0;
				auto &ref = g_displays[0];
				int range = ref.maxBrightness - ref.minBrightness;
				if (range <= 0) range = 1;
				pct    = (int)((float)(ref.currentBrightness - ref.minBrightness) / (float)range * 100.0f);
				refMin = ref.minBrightness;
				refMax = ref.maxBrightness;
			}

			TrayPopup::Show(h, pct, [refMin, refMax](int newPct) {
				ULONG r   = refMax - refMin;
				ULONG val = refMin + (ULONG)((float)r * (float)newPct / 100.0f);
				SetBrightnessAll(val, true, false);
			});
			return 0;
		}

		if (LOWORD(lParam) == WM_RBUTTONUP) {
			HMENU hMenu = CreatePopupMenu();

			std::wstring statusLine = buildDisplayStatusLine();
			AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, statusLine.c_str());

			// Sub-list of individual displays (only when multiple)
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);
				if (g_displays.size() > 1) {
					for (size_t i = 0; i < g_displays.size(); ++i) {
						std::wstring item = L"    " + g_displays[i].name;
						AppendMenuW(hMenu, MF_STRING | MF_DISABLED | MF_GRAYED, 0, item.c_str());
					}
				}
			}

			AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(hMenu, MF_STRING | (g_settings.autoAdjustEnabled.load() ? MF_CHECKED : 0), IDM_TOGGLE_AUTO,
			            L"Automatic Brightness");
			AppendMenuW(hMenu, MF_STRING, IDM_OPTIONS, L"Options...");
			AppendMenuW(hMenu, MF_STRING, IDM_SHOW_LOGS, L"Logs...");
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
			} else if (cmd == IDM_SHOW_LOGS) {
				LogWindow::Show();
			} else if (cmd == IDM_CHECK_UPDATE) {
				auto r = ShellExecuteW(h, L"open", kReleaseUrl, nullptr, nullptr, SW_SHOWNORMAL);
				if ((UINT_PTR)r <= 32)
					MessageBoxW(h, L"Unable to open releases page.", L"StudioBrightnessPlusPlus", MB_ICONERROR);
			} else if (cmd == IDM_EXIT) {
				PostMessage(h, WM_CLOSE, 0, 0);
			}
			return 0;
		}
	}
	if (m == WM_DESTROY) {
		cleanupAlsSensors();
		{
			std::lock_guard<std::mutex> lock(g_displayMutex);
			g_displays.clear(); // destructors close handles
		}
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
	wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);
	wc.lpszClassName = kWndClass;
	ATOM a = RegisterClassExW(&wc);
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
		CoInitializeEx(nullptr, COINIT_MULTITHREADED);

		// Initialize ALS sensors
		initAlsSensors();

		DWORD lastEnumerateTick = 0;
		constexpr DWORD kEnumerateCooldownMs = 3000; // re-scan every 3s at most

		for (;;) {
			/* ---------- device (re)connection attempt ---------- */
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);

				// Check existing devices are still alive
				bool anyDead = false;
				for (auto &dev : g_displays) {
					ULONG tmp;
					if (dev.getBrightness(&tmp) != 0) {
						Log::Warn(L"Device %s disconnected", dev.name.c_str());
						dev.close();
						anyDead = true;
					}
				}

				// Remove dead devices
				if (anyDead) {
					g_displays.erase(
					    std::remove_if(g_displays.begin(), g_displays.end(),
					                   [](const DisplayDevice &d) { return !d.isOpen(); }),
					    g_displays.end());
				}

				// Re-scan for new devices only when needed (empty or device lost)
				DWORD now = GetTickCount();
				bool needScan = g_displays.empty() || anyDead;
				if (needScan && now - lastEnumerateTick >= kEnumerateCooldownMs) {
					lastEnumerateTick = now;

					auto found = hid_enumerate();
					for (auto &newDev : found) {
						bool exists = false;
						for (const auto &existing : g_displays) {
							if (existing.devicePath == newDev.devicePath) {
								exists = true;
								break;
							}
						}
						if (exists) {
							newDev.close();
							continue;
						}

						// Initialize new device
						newDev.getBrightnessRange(&newDev.minBrightness, &newDev.maxBrightness);
						if (newDev.getBrightness(&newDev.currentBrightness) == 0) {
							newDev.baseBrightness = newDev.currentBrightness;
							newDev.baseLux = getAmbientLux(newDev);
						}
						Log::Info(L"Device %s ready [range %lu-%lu, current %lu]",
						          newDev.name.c_str(), newDev.minBrightness, newDev.maxBrightness,
						          newDev.currentBrightness);
						g_displays.push_back(std::move(newDev));
					}
				}
			}

			/* ---------- auto-brightness (linked mode) ---------- */
			if (g_settings.autoAdjustEnabled.load()) {
				std::lock_guard<std::mutex> lock(g_displayMutex);
				if (!g_displays.empty()) {
					// Use first display's sensor for master lux
					float lux = getAmbientLux(g_displays[0]);

					for (auto &dev : g_displays) {
						ULONG tgt  = mapLuxToBrightness(lux, dev);
						long  diff = (long)tgt - (long)dev.currentBrightness;
						long  db   = computeDeadband(dev.minBrightness, dev.maxBrightness);

						if (diff > db)
							diff -= db;
						else if (diff < -db)
							diff += db;
						else
							diff = 0;

						if (diff) {
							long step  = std::max((long)((dev.maxBrightness - dev.minBrightness) * 0.02f), 500L);
							long delta = std::clamp(diff, -step, step);
							SetBrightness(dev, dev.currentBrightness + delta, false, false);
						}
					}
				}
			}
			std::this_thread::sleep_for(std::chrono::milliseconds(100));
		}
	}).detach();
}

/* ---------- WinMain ---------- */
int APIENTRY wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
	HANDLE hSingleInstance = CreateMutexW(nullptr, TRUE, L"StudioBrightnessPlusPlus_SingleInstance");
	if (GetLastError() == ERROR_ALREADY_EXISTS) {
		if (hSingleInstance) CloseHandle(hSingleInstance);
		return 0;
	}

	GdiplusStartupInput gdiplusStartupInput;
	GdiplusStartup(&gdiplusToken, &gdiplusStartupInput, NULL);

	CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED | COINIT_DISABLE_OLE1DDE);
	g_hInst = hInst;
	INITCOMMONCONTROLSEX icc{sizeof(icc), ICC_WIN95_CLASSES};
	InitCommonControlsEx(&icc);

	g_settings.Load();
	bool realStartup = g_settings.IsStartupEnabled();
	if (g_settings.runAtStartup != realStartup) {
		if (g_settings.runAtStartup) g_settings.SetStartup(true);
	}

	Log::Info(L"Studio Brightness++ v%s starting", kAppVersion);

	if (!RegisterHiddenClass()) {
		CloseHandle(hSingleInstance);
		return 1;
	}

	SetLastError(0);
	HWND h  = CreateWindowW(kWndClass, L"", WS_OVERLAPPEDWINDOW, CW_USEDEFAULT, CW_USEDEFAULT,
	                        400, 200, nullptr, nullptr, hInst, nullptr);
	g_hMain = h;
	if (!h) {
		wchar_t buf[128];
		wsprintf(buf, L"CreateWindowW failed (%lu)", GetLastError());
		MessageBoxW(nullptr, buf, L"StudioBrightnessPlusPlus", MB_ICONERROR | MB_TOPMOST);
		CloseHandle(hSingleInstance);
		return 1;
	}
	ShowWindow(h, SW_HIDE);

	RAWINPUTDEVICE rid;
	rid.usUsagePage = 0x0C;
	rid.usUsage     = 0x01;
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
	}
	registerHotkeys(h);
	startWorker();

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	GdiplusShutdown(gdiplusToken);
	CoUninitialize();
	CloseHandle(hSingleInstance);
	return 0;
}
