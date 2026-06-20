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
#include <cmath>
#include <map>
#include <set>
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
#include "version.h"
#include "Updater.h"
#include "HdrMonitor.h"
#include "PresetConfirm.h"

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
constexpr wchar_t kAppVersion[] = SBPP_VERSION_STR;

// Update state, filled by the background update-check thread.
constexpr UINT     WMAPP_UPDATE_READY = WM_APP + 2;
constexpr UINT_PTR ID_UPDATE_TIMER    = 0xA001;
constexpr UINT_PTR ID_HDR_TIMER       = 0xA002;
static std::atomic<bool> g_updateAvailable{false};
static std::atomic<bool> g_updateChecking{false};
static std::mutex        g_updateMutex;
static UpdateInfo        g_updateInfo;

// HDR state of Apple displays, tracked live. Brightness control is unavailable while HDR is on.
static std::atomic<bool> g_hdrActive{false};

/* ---------- system tray icon ---------- */
// Re-add the tray icon when the shell (re)creates the taskbar: Explorer restart, or we
// started before the taskbar was ready at boot. Registered in WinMain, handled in WndProc.
static UINT g_wmTaskbarCreated = 0;

/* ---------- multi-display state ---------- */
static std::vector<DisplayDevice> g_displays;
static std::mutex                 g_displayMutex;

/* ---------- Per-display color-preset persistence (HKCU\...\Presets\{ContainerId}) ---------- */
static std::wstring guidToString(const GUID &g) {
	wchar_t buf[64] = {};
	StringFromGUID2(g, buf, 64);
	return buf;
}
// Revert a display's color preset to a previous index, located by ContainerId so it still works
// after the HID re-enumeration a preset switch triggers (the DisplayDevice object gets replaced).
static void RevertPresetByContainer(const GUID &cid, int prevIdx) {
	if (prevIdx < 0) return;
	std::lock_guard<std::mutex> lock(g_displayMutex);
	for (auto &dev : g_displays)
		if (memcmp(&dev.containerId, &cid, sizeof(GUID)) == 0 && dev.hPreset != INVALID_HANDLE_VALUE) {
			if (dev.setActivePreset(prevIdx) == 0)
				Log::Info(L"Color preset reverted to %d on %s", prevIdx, dev.name.c_str());
			break;
		}
}

[[maybe_unused]] static bool loadPresetForContainer(const GUID &cid, int *outIdx) {
	HKEY hKey;
	bool ok = false;
	if (RegOpenKeyExW(HKEY_CURRENT_USER, L"Software\\StudioBrightnessPlusPlus\\Presets", 0, KEY_READ, &hKey) == ERROR_SUCCESS) {
		DWORD val = 0, size = sizeof(val), type = 0;
		if (RegQueryValueExW(hKey, guidToString(cid).c_str(), nullptr, &type, (BYTE *)&val, &size) == ERROR_SUCCESS && type == REG_DWORD) {
			if (outIdx) *outIdx = (int)val;
			ok = true;
		}
		RegCloseKey(hKey);
	}
	return ok;
}
[[maybe_unused]] static void savePresetForContainer(const GUID &cid, int idx) {
	HKEY hKey;
	DWORD disp;
	if (RegCreateKeyExW(HKEY_CURRENT_USER, L"Software\\StudioBrightnessPlusPlus\\Presets", 0, nullptr, 0, KEY_WRITE, nullptr, &hKey, &disp) == ERROR_SUCCESS) {
		DWORD val = (DWORD)idx;
		RegSetValueExW(hKey, guidToString(cid).c_str(), 0, REG_DWORD, (BYTE *)&val, sizeof(val));
		RegCloseKey(hKey);
	}
}

// Cache of enumerated presets (so we don't re-write the 0xFF20 cursor on every reconnect) and a
// once-per-run default-reset guard. Switching a calibrated preset re-enumerates the display's HID
// descriptor (a disconnect), so re-applying on each reconnect would loop. Keyed by ContainerId.
static std::map<std::wstring, std::vector<ColorPreset>> g_presetCache;
static std::set<std::wstring>                           g_presetRestored;

// GDI+
static ULONG_PTR gdiplusToken;

// --- Apple-style auto-brightness transition (hysteresis + asymmetric perceptual ramp) ---
constexpr float  kRelLuxHysteresis = 0.20f;   // re-target only when |dLux|/lastTargetLux >= 20%
constexpr double kRampBrightenMs   = 1500.0;  // fast brightening
constexpr double kRampDimMs        = 5000.0;  // slow, gentle dimming

static double brightToPerc(ULONG v) { return std::log2((double)v + 1.0); }
static ULONG  percToBright(double l) { return (ULONG)std::llround(std::exp2(l) - 1.0); }
static double nowMs() {
	using namespace std::chrono;
	return duration<double, std::milli>(steady_clock::now().time_since_epoch()).count();
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
	nid.uFlags           = NIF_ICON | NIF_TIP | NIF_MESSAGE;
	nid.uCallbackMessage = WMAPP_NOTIFYCALLBACK;
	nid.hIcon            = LoadIcon(g_hInst, MAKEINTRESOURCE(IDI_MYICON));
	wcscpy_s(nid.szTip, L"Studio Brightness ++");
	Shell_NotifyIcon(NIM_ADD, &nid);
	nid.uVersion = NOTIFYICON_VERSION_4;
	return Shell_NotifyIcon(NIM_SETVERSION, &nid);
}
BOOL DeleteNotificationIcon() {
	NOTIFYICONDATA nid{sizeof(nid)};
	nid.hWnd = g_hMain;
	nid.uID  = 1;
	return Shell_NotifyIcon(NIM_DELETE, &nid);
}

/* ---------- update helpers ---------- */
static void ShowUpdateBalloon(const wchar_t *title, const wchar_t *text) {
	NOTIFYICONDATA nid{sizeof(nid)};
	nid.hWnd        = g_hMain;
	nid.uID         = 1;
	nid.uFlags      = NIF_INFO;
	nid.dwInfoFlags = NIIF_INFO;
	wcscpy_s(nid.szInfoTitle, title);
	wcscpy_s(nid.szInfo, text);
	Shell_NotifyIcon(NIM_MODIFY, &nid);
}

// Re-read the HDR state of Apple displays into g_hdrActive, logging any transition.
static void RefreshHdrState() {
	bool now  = HdrAnyAppleDisplayActive();
	bool prev = g_hdrActive.exchange(now);
	if (now != prev)
		Log::Info(L"HDR %s", now ? L"enabled (brightness is controlled by Windows)" : L"disabled");
}

static void StartUpdateCheck(bool manual) {
	bool expected = false;
	if (!g_updateChecking.compare_exchange_strong(expected, true)) return; // a check is already running
	std::thread([manual]() {
		UpdateInfo info = CheckForUpdate(g_settings.updateChannel, kAppVersion);
		{
			std::lock_guard<std::mutex> lk(g_updateMutex);
			g_updateInfo = info;
		}
		g_updateAvailable.store(info.available);
		g_updateChecking.store(false);
		if (g_hMain) PostMessageW(g_hMain, WMAPP_UPDATE_READY, manual ? 1 : 0, 0);
	}).detach();
}

static void StartUpdateInstall() {
	UpdateInfo info;
	{
		std::lock_guard<std::mutex> lk(g_updateMutex);
		info = g_updateInfo;
	}
	if (!info.available) return;
	ShowUpdateBalloon(L"Studio Brightness++", L"Downloading the update...");
	std::thread([info]() {
		bool ok = BeginInstallUpdate(info);
		if (g_hMain) {
			if (ok) PostMessageW(g_hMain, WM_CLOSE, 0, 0);       // exit so the installer can replace files
			else    PostMessageW(g_hMain, WMAPP_UPDATE_READY, 3, 0);
		}
	}).detach();
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
static std::atomic<float>               g_lastKnownLux{100.f}; // last good lux; survives ALS re-init

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

	// Try to find a sensor matching this display's ContainerId
	GUID zero = {};
	if (memcmp(&dev.containerId, &zero, sizeof(GUID)) != 0) {
		for (auto *l : g_alsListeners) {
			if (memcmp(&l->containerId, &dev.containerId, sizeof(GUID)) == 0 && l->alive()) {
				float lx = l->lux();
				g_lastKnownLux.store(lx, std::memory_order_relaxed);
				return lx;
			}
		}
	}

	// Fallback: use first alive sensor (master)
	for (auto *l : g_alsListeners) {
		if (l->alive()) {
			float lx = l->lux();
			g_lastKnownLux.store(lx, std::memory_order_relaxed);
			return lx;
		}
	}

	// No live sensor yet (e.g. just after an ALS re-init): use the last known value, not a hard default.
	return g_lastKnownLux.load(std::memory_order_relaxed);
}

/* ---------- mapping lux to brightness ---------- */
static ULONG mapLuxToBrightness(float lux, const DisplayDevice &dev) {
	float scale = std::clamp(lux, 2.f, 5000.f) / dev.baseLux;
	float tgt   = dev.baseBrightness * scale;
	return static_cast<ULONG>(std::clamp(tgt, (float)dev.minBrightness, (float)dev.maxBrightness));
}

/* ---------- Central Brightness Setter ---------- */
static void SetBrightness(DisplayDevice &dev, ULONG val, bool isUserAction, bool showOSD) {
	if (g_hdrActive.load()) return;   // brightness writes are no-ops under HDR; Windows owns it
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
			// Stop any auto ramp and drop the hysteresis anchor so auto re-syncs to the user.
			dev.rampDurationMs = 0.0;
			dev.lastTargetLux  = 0.f;
		}

		if (showOSD && g_settings.showOSD)
			OSDWindow::Show((int)dev.currentBrightness, (int)dev.maxBrightness);
	}
}

// Map a brightness value from one display's range to another using nit calibration.
// This ensures perceived brightness matches across displays with different nit ceilings.
static ULONG mapBrightnessAcrossDisplays(ULONG val, const DisplayDevice &from, const DisplayDevice &to) {
	if (from.maxNits == to.maxNits && from.maxBrightness == to.maxBrightness && from.minBrightness == to.minBrightness)
		return val;
	float fromRange = (float)(from.maxBrightness - from.minBrightness);
	if (fromRange <= 0.f) return val;
	float pct  = (float)(val - from.minBrightness) / fromRange;
	float nits = pct * from.maxNits;
	float toPct = std::clamp(nits / to.maxNits, 0.f, 1.f);
	return to.minBrightness + (ULONG)(toPct * (float)(to.maxBrightness - to.minBrightness));
}

// Apply brightness to all connected displays (linked mode, proportional nit mapping)
static void SetBrightnessLinked(ULONG val, const DisplayDevice &refDev, bool isUserAction, bool showOSD) {
	for (auto &dev : g_displays) {
		ULONG mapped = mapBrightnessAcrossDisplays(val, refDev, dev);
		SetBrightness(dev, mapped, isUserAction, showOSD);
	}
}

// Apply brightness change based on current mode (linked or single display)
static void ApplyBrightness(ULONG val, bool isUserAction, bool showOSD) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty()) return;

	if (g_settings.linkedMode || g_displays.size() == 1) {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		SetBrightnessLinked(val, g_displays[idx], isUserAction, showOSD);
	} else {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		SetBrightness(g_displays[idx], val, isUserAction, showOSD);
	}
}

/* ---------- helpers: brightness step ---------- */
static void adjustBrightnessByStep(int direction) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty()) return;

	ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
	auto &ref = g_displays[idx];
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
		if (g_settings.linkedMode || g_displays.size() == 1) {
			SetBrightnessLinked(newBrightness, ref, true, true);
		} else {
			SetBrightness(ref, newBrightness, true, true);
		}
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

		// Color preset combo for the active display
		{
			std::wstring dispName;
			std::vector<ColorPreset> presetsCopy;
			int  active = -1;
			bool have   = false;
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);
				if (!g_displays.empty()) {
					ULONG idx   = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
					dispName    = g_displays[idx].name;
					presetsCopy = g_displays[idx].presets;
					active      = g_displays[idx].activePresetIndex;
					have        = true;
				}
			}
			std::wstring label = have ? (L"Display: " + dispName) : L"(no display detected)";
			SetDlgItemTextW(d, IDC_PRESET_DISPLAY_LABEL, label.c_str());
			HWND combo = GetDlgItem(d, IDC_PRESET_COMBO);
			SendMessageW(combo, CB_RESETCONTENT, 0, 0);
			if (have && !presetsCopy.empty()) {
				int sel = 0;
				for (size_t i = 0; i < presetsCopy.size(); ++i) {
					int pos = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)presetsCopy[i].name.c_str());
					SendMessageW(combo, CB_SETITEMDATA, pos, (LPARAM)presetsCopy[i].index);
					if ((int)presetsCopy[i].index == active)
						sel = pos;
				}
				SendMessageW(combo, CB_SETCURSEL, sel, 0);
				EnableWindow(combo, TRUE);
			} else {
				SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"(no presets)");
				SendMessageW(combo, CB_SETCURSEL, 0, 0);
				EnableWindow(combo, FALSE);
			}
		}
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

			// Apply the chosen color preset to the active display, then prompt to keep or auto-revert.
			// Not persisted: a preset resets to the default at startup, changed only by hand here.
			{
				HWND combo = GetDlgItem(d, IDC_PRESET_COMBO);
				int  sel   = (int)SendMessageW(combo, CB_GETCURSEL, 0, 0);
				if (sel != CB_ERR && IsWindowEnabled(combo)) {
					int  hwIdx    = (int)SendMessageW(combo, CB_GETITEMDATA, sel, 0);
					GUID cid      = {};
					int  prevIdx  = -1;
					bool switched = false;
					{
						std::lock_guard<std::mutex> lock(g_displayMutex);
						if (!g_displays.empty()) {
							ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
							auto &dev = g_displays[idx];
							if (dev.hPreset != INVALID_HANDLE_VALUE && hwIdx != dev.activePresetIndex) {
								prevIdx = dev.activePresetIndex;
								if (dev.setActivePreset(hwIdx) == 0) {
									cid      = dev.containerId;
									switched = true;
								}
							}
						}
					}
					if (switched)
						PresetConfirm::Show(g_hInst, 10, [cid, prevIdx]() { RevertPresetByContainer(cid, prevIdx); });
				}
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
	if (g_wmTaskbarCreated && m == g_wmTaskbarCreated) {
		AddNotificationIcon(h);
		return 0;
	}
	if (m == WM_DISPLAYCHANGE) {
		RefreshHdrState();
		return 0;
	}
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
	if (m == WMAPP_UPDATE_READY) {
		if (wParam == 3) {
			ShowUpdateBalloon(L"Studio Brightness++", L"The update could not be installed.");
		} else if (g_updateAvailable.load()) {
			std::wstring v;
			{
				std::lock_guard<std::mutex> lk(g_updateMutex);
				v = g_updateInfo.version;
			}
			std::wstring text = L"Version " + v + L" is available. Click here to install.";
			ShowUpdateBalloon(L"Update available", text.c_str());
		} else if (wParam == 1) {
			ShowUpdateBalloon(L"Studio Brightness++", L"You are running the latest version.");
		}
		return 0;
	}
	if (m == WM_TIMER && wParam == ID_UPDATE_TIMER) {
		StartUpdateCheck(false);
		return 0;
	}
	if (m == WM_TIMER && wParam == ID_HDR_TIMER) {
		RefreshHdrState();
		return 0;
	}
	if (m == WMAPP_NOTIFYCALLBACK) {
		if (LOWORD(lParam) == WM_LBUTTONUP) {
			if (g_hdrActive.load()) {
				TrayPopup::Show(h, 0, nullptr, true, L"Brightness controlled by Windows (HDR)");
				return 0;
			}
			int   pct    = 50;
			ULONG refMin = 1000;
			ULONG refMax = 60000;
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);
				if (g_displays.empty()) return 0;
				ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
				auto &ref = g_displays[idx];
				int range = ref.maxBrightness - ref.minBrightness;
				if (range <= 0) range = 1;
				pct    = (int)((float)(ref.currentBrightness - ref.minBrightness) / (float)range * 100.0f);
				refMin = ref.minBrightness;
				refMax = ref.maxBrightness;
			}

			TrayPopup::Show(h, pct, [refMin, refMax](int newPct) {
				ULONG r   = refMax - refMin;
				ULONG val = refMin + (ULONG)((float)r * (float)newPct / 100.0f);
				ApplyBrightness(val, true, false);
			});
			return 0;
		}

		if (LOWORD(lParam) == NIN_BALLOONUSERCLICK) {
			if (g_updateAvailable.load()) StartUpdateInstall();
			return 0;
		}

		if (LOWORD(lParam) == WM_RBUTTONUP) {
			HMENU hMenu = CreatePopupMenu();

			std::wstring statusLine = buildDisplayStatusLine();
			AppendMenuW(hMenu, MF_STRING | MF_DISABLED, 0, statusLine.c_str());

			// Display list with selection (only when multiple)
			{
				std::lock_guard<std::mutex> lock(g_displayMutex);
				if (g_displays.size() > 1) {
					ULONG activeIdx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
					for (size_t i = 0; i < g_displays.size(); ++i) {
						std::wstring item = L"    \U0001F7E2 " + g_displays[i].name;
						UINT flags = MF_STRING;
						if (g_settings.linkedMode) {
							flags |= MF_CHECKED | MF_GRAYED;
						} else {
							flags |= (i == activeIdx) ? MF_CHECKED : MF_UNCHECKED;
						}
						AppendMenuW(hMenu, flags, IDM_SELECT_DISPLAY + i, item.c_str());
					}
					AppendMenuW(hMenu, MF_STRING | (g_settings.linkedMode ? MF_CHECKED : 0),
					            IDM_LINKED_MODE, L"Linked Displays");
				}
			}

			AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
			AppendMenuW(hMenu, MF_STRING | (g_settings.autoAdjustEnabled.load() ? MF_CHECKED : 0), IDM_TOGGLE_AUTO,
			            L"Automatic Brightness");
			AppendMenuW(hMenu, MF_STRING, IDM_OPTIONS, L"Options...");
			AppendMenuW(hMenu, MF_STRING, IDM_SHOW_LOGS, L"Logs...");
			if (g_updateAvailable.load()) {
				std::wstring lbl;
				{
					std::lock_guard<std::mutex> lk(g_updateMutex);
					lbl = L"Install update " + g_updateInfo.version;
				}
				AppendMenuW(hMenu, MF_STRING, IDM_INSTALL_UPDATE, lbl.c_str());
			} else {
				AppendMenuW(hMenu, MF_STRING, IDM_CHECK_UPDATE, L"Check update");
			}
			HMENU hChannel = CreatePopupMenu();
			AppendMenuW(hChannel, MF_STRING | (g_settings.updateChannel == 0 ? MF_CHECKED : 0),
			            IDM_CHANNEL_STABLE, L"Stable only");
			AppendMenuW(hChannel, MF_STRING | (g_settings.updateChannel == 1 ? MF_CHECKED : 0),
			            IDM_CHANNEL_BETA, L"Include betas");
			AppendMenuW(hMenu, MF_POPUP, (UINT_PTR)hChannel, L"Update channel");
			wchar_t versionLabel[40];
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
			} else if (cmd == IDM_LINKED_MODE) {
				g_settings.linkedMode = !g_settings.linkedMode;
				g_settings.Save();
			} else if (cmd >= IDM_SELECT_DISPLAY && cmd < IDM_SELECT_DISPLAY + 16) {
				g_settings.activeDisplayIndex = (ULONG)(cmd - IDM_SELECT_DISPLAY);
				g_settings.Save();
			} else if (cmd == IDM_OPTIONS) {
				DialogBoxParamW(g_hInst, MAKEINTRESOURCE(IDD_OPTIONS), h, OptionsDlgProc, 0);
			} else if (cmd == IDM_SHOW_LOGS) {
				LogWindow::Show();
			} else if (cmd == IDM_CHECK_UPDATE) {
				StartUpdateCheck(true);
			} else if (cmd == IDM_INSTALL_UPDATE) {
				StartUpdateInstall();
			} else if (cmd == IDM_CHANNEL_STABLE) {
				g_settings.updateChannel = 0;
				g_settings.Save();
			} else if (cmd == IDM_CHANNEL_BETA) {
				g_settings.updateChannel = 1;
				g_settings.Save();
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
		bool  firstAddDone      = false; // skip the ALS re-bind on the first device add (startup)
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

						// Initialize new device. On a reconnect the old ISensor goes stale across the
						// display reconfiguration, so re-bind the ALS to the freshly settled sensor
						// (skip the first add: initAlsSensors already ran at worker startup).
						if (firstAddDone) {
							cleanupAlsSensors();
							initAlsSensors();
						}
						firstAddDone = true;

						newDev.getBrightnessRange(&newDev.minBrightness, &newDev.maxBrightness);
						if (newDev.getBrightness(&newDev.currentBrightness) == 0) {
							newDev.baseBrightness = newDev.currentBrightness;
							newDev.baseLux = getAmbientLux(newDev);
						}
						Log::Info(L"Device %s ready [range %lu-%lu, current %lu]",
						          newDev.name.c_str(), newDev.minBrightness, newDev.maxBrightness,
						          newDev.currentBrightness);

						// Color presets: enumerate ONCE per physical display (cached), reset to the default ONCE per run.
						// Re-enumerating or re-restoring on every reconnect loops, because switching a
						// calibrated preset re-enumerates the display's HID descriptor (a disconnect).
						if (newDev.hPreset != INVALID_HANDLE_VALUE) {
							std::wstring cidKey = guidToString(newDev.containerId);
							auto cit = g_presetCache.find(cidKey);
							if (cit != g_presetCache.end() && !cit->second.empty()) {
								newDev.presets = cit->second; // reuse cached list; skip cursor writes
							} else {
								newDev.enumeratePresets();
								if (!newDev.presets.empty())
									g_presetCache[cidKey] = newDev.presets;
							}
							newDev.getActivePreset(&newDev.activePresetIndex);
							if (g_presetRestored.insert(cidKey).second) { // once per run, per display
								// Reset to the default reference mode (index 0) at startup if not already
								// there. No persistence/restore of the user's choice; a preset is changed
								// only by manual action, and this stops a saved preset from re-blanking
								// some XDR units on every launch.
								if (!newDev.presets.empty() && newDev.activePresetIndex != 0 &&
								    newDev.setActivePreset(0) == 0)
									Log::Info(L"Reset %s to its default color preset", newDev.name.c_str());
							}
						}

						g_displays.push_back(std::move(newDev));
					}
				}
			}

			/* ---------- auto-brightness (Apple-style hysteresis + asymmetric perceptual ramp) ---------- */
			if (g_settings.autoAdjustEnabled.load()) {
				std::lock_guard<std::mutex> lock(g_displayMutex);
				for (auto &dev : g_displays) {
					if (dev.maxBrightness <= dev.minBrightness)
						continue; // brightness locked (e.g. a calibrated color preset); nothing to adjust
					float lux = getAmbientLux(dev); // per-device, ContainerId-matched sensor

					bool retarget = (dev.lastTargetLux <= 0.f) ||
					                (std::fabs(lux - dev.lastTargetLux) / dev.lastTargetLux >= kRelLuxHysteresis);
					if (retarget) {
						ULONG goal = mapLuxToBrightness(lux, dev);
						if (goal != dev.rampGoal || dev.rampDurationMs == 0.0) {
							dev.rampStart      = dev.currentBrightness;
							dev.rampGoal       = goal;
							dev.rampStartMs    = nowMs();
							dev.rampDurationMs = (goal >= dev.currentBrightness) ? kRampBrightenMs : kRampDimMs;
						}
						dev.lastTargetLux = lux;
					}

					if (dev.rampDurationMs > 0.0) {
						double t = (nowMs() - dev.rampStartMs) / dev.rampDurationMs;
						if (t >= 1.0) {
							if (dev.currentBrightness != dev.rampGoal)
								SetBrightness(dev, dev.rampGoal, false, false);
							dev.rampDurationMs = 0.0;
						} else {
							if (t < 0.0) t = 0.0;
							double a = brightToPerc(dev.rampStart), b = brightToPerc(dev.rampGoal);
							ULONG  next = percToBright(a + t * (b - a));
							next = std::clamp(next, dev.minBrightness, dev.maxBrightness);
							if (next != dev.currentBrightness)
								SetBrightness(dev, next, false, false);
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
		// Sync the Run key with the saved preference: rewrite the (now path-checked) entry if we
		// should auto-start, or remove a stale/leftover entry if we shouldn't.
		g_settings.SetStartup(g_settings.runAtStartup);
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
	g_wmTaskbarCreated = RegisterWindowMessageW(L"TaskbarCreated");
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

	// Check for updates shortly after launch, then once a day.
	SetTimer(h, ID_UPDATE_TIMER, 24 * 60 * 60 * 1000, nullptr);
	SetTimer(h, ID_HDR_TIMER, 2000, nullptr);   // poll HDR; also refreshed on WM_DISPLAYCHANGE
	RefreshHdrState();
	StartUpdateCheck(false);

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
