#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <shellapi.h>
#include <dbt.h>
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
#include <climits>
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
#include "NvHdr.h"
#include "PresetConfirm.h"
#include "orientation.h"
#include "als.h"
#include "Dpi.h"

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
constexpr UINT     WMAPP_OPTIONS_RESCALE = WM_APP + 3; // Options dialog: lay it out for another scaling (see rescaleOptions)
constexpr UINT     WMAPP_OPTIONS_SHOW = WM_APP + 4; // Options dialog: scroll a control (wParam) into view
constexpr UINT_PTR ID_UPDATE_TIMER    = 0xA001;
constexpr UINT_PTR ID_HDR_TIMER       = 0xA002;
constexpr UINT_PTR ID_REAPPLY_TIMER   = 0xA003;
constexpr UINT_PTR ID_OPTIONS_TIMER   = 0xA004; // Options dialog: keeps its brightness controls current
constexpr UINT_PTR ID_TYPING_TIMER    = 0xA005; // Options dialog: a pause in typing into the percent field
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
// Set when an Apple HID interface arrives, so the worker re-scans even while displays are
// already connected: a second display plugged in later, or one whose HID shows up late at boot.
static std::atomic<bool>          g_rescanRequested{false};
// Set by ID_REAPPLY_TIMER once the displays are back on; the worker sends the brightness again.
static std::atomic<bool>          g_reapplyRequested{false};

/* ---------- Per-display preset persistence (HKCU\...\Presets\{ContainerId}) ---------- */
static std::wstring guidToString(const GUID &g) {
	wchar_t buf[64] = {};
	StringFromGUID2(g, buf, 64);
	return buf;
}
// Revert a display's preset to a previous index, located by ContainerId so it still works
// after the HID re-enumeration a preset switch triggers (the DisplayDevice object gets replaced).
static bool tryRevertPreset(const GUID &cid, int prevIdx) {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	for (auto &dev : g_displays)
		if (memcmp(&dev.containerId, &cid, sizeof(GUID)) == 0 && dev.hPreset != INVALID_HANDLE_VALUE) {
			if (dev.setActivePreset(prevIdx) == 0) {
				Log::Info(L"Preset reverted to %d on %s", prevIdx, dev.name.c_str());
				return true;
			}
		}
	return false;
}
static void RevertPresetByContainer(const GUID &cid, int prevIdx) {
	if (prevIdx < 0) return;
	Log::Info(L"Reverting preset to %d", prevIdx);
	if (tryRevertPreset(cid, prevIdx))
		return;
	// The switch re-enumerates the display's HID interface (a disconnect), so the device may not
	// be back in g_displays yet, especially on the early display-dropped revert. Keep trying in
	// the background until the worker has re-added it.
	std::thread([cid, prevIdx]() {
		for (int i = 0; i < 40; ++i) { // ~20 s
			std::this_thread::sleep_for(std::chrono::milliseconds(500));
			if (tryRevertPreset(cid, prevIdx))
				return;
		}
		Log::Warn(L"Preset revert gave up: the display did not come back within 20 s");
	}).detach();
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
static std::map<std::wstring, std::vector<Preset>> g_presetCache;
static std::set<std::wstring>                           g_presetRestored;

// GDI+
static ULONG_PTR gdiplusToken;

// --- Apple-style auto-brightness transition (hysteresis + asymmetric perceptual ramp) ---
// The two ramp durations are settings, edited in Options: g_settings.rampBrightenMs / rampDimMs.
constexpr float  kRelLuxHysteresis = 0.20f;   // re-target only when |dLux|/lastTargetLux >= 20%

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
// On the SDR-to-HDR transition, auto-rescue: a reference-mode preset is incompatible with HDR
// (the panel can blank), so force each display back to a main "Apple XDR Display" preset the
// moment HDR turns on. This closes the "pick a reference mode, then enable HDR" trap, and it is
// the same recovery a user would do by hand from a second machine.
static void RefreshHdrState() {
	bool now  = HdrAnyAppleDisplayActive();
	bool prev = g_hdrActive.exchange(now);
	if (now != prev) {
		Log::Info(L"HDR %s", now ? L"enabled (brightness is controlled by Windows)" : L"disabled");
		NvapiLogHdrState(now ? L"HDR on" : L"HDR off");
		HdrLogAppleOutputs(now ? L"HDR on" : L"HDR off");
		if (now) {
			PresetConfirm::Cancel(); // a pending keep/revert prompt is superseded by the rescue
			std::lock_guard<std::mutex> lock(g_displayMutex);
			for (auto &dev : g_displays) {
				if (!dev.hasPresets() || !dev.presetsClassifiable())
					continue;
				const Preset *ap = dev.activePreset();
				if (!ap || ap->isHdrCompatible())
					continue;
				int tgt = dev.firstHdrCompatiblePreset();
				if (tgt >= 0 && tgt != dev.activePresetIndex) {
					Log::Info(L"HDR enabled with \"%s\" active on %s: switching to preset %d for HDR compatibility",
					          ap->name.c_str(), dev.name.c_str(), tgt);
					if (dev.setActivePreset(tgt) != 0)
						Log::Warn(L"HDR rescue preset switch failed on %s", dev.name.c_str());
				}
			}
		}
	}
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
// Where the ambient light reading used for a display comes from.
enum class AmbientSource {
	None,    // no sensor has reported since the app started
	Native,  // the display's own sensor
	Another, // the sensor of another Apple display, or any other ambient light sensor Windows has
	Last,    // no sensor is reporting now: the last reading there was
};
struct Ambient {
	AmbientReading reading; // color only from an Apple display's sensor, and not in the dark
	AmbientSource  source = AmbientSource::None;
	bool live() const { return source == AmbientSource::Native || source == AmbientSource::Another; }
};
static AmbientReading g_lastAmbient;             // last good reading; survives ALS re-init. Under g_alsMutex.
static bool           g_haveLastAmbient = false; // false until a sensor has reported once

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
		Log::Info(L"ALS: no ambient light sensor via the Windows Sensor API");
		return;
	}

	ULONG count = 0;
	col->GetCount(&count);
	Log::Info(L"ALS: %lu ambient light sensor(s) via the Windows Sensor API", count);

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

// Get the ambient light from the best available sensor for a given display, and say which it was.
// If the display has an ALS (matched by ContainerId), use it.
// Otherwise use the master sensor: another Apple display's, then any sensor Windows has.
// With no sensor reporting, it is the last reading there was.
// The display is given by its ContainerId. All zeros stands for no display at all: there is no
// sensor of its own to look for then, and the rest is the same.
static Ambient getAmbient(const GUID &container) {
	std::lock_guard<std::mutex> lock(g_alsMutex);
	Ambient a;
	auto luxOnly = [&a](float lux) { // a sensor read through Windows: no color
		a.reading     = AmbientReading{};
		a.reading.lux = lux;
	};
	auto from = [&a](AmbientSource source) {
		a.source          = source;
		g_lastAmbient     = a.reading;
		g_haveLastAmbient = true;
		return a;
	};
	static const GUID zero  = {};
	const bool        known = memcmp(&container, &zero, sizeof(GUID)) != 0;

	// Prefer the raw-HID Apple ALS (the same source Boot Camp uses) matched to this display.
	if (known && als_get_ambient(&container, &a.reading))
		return from(AmbientSource::Native);

	// Try to find a sensor matching this display's ContainerId
	if (known) {
		for (auto *l : g_alsListeners) {
			if (memcmp(&l->containerId, &container, sizeof(GUID)) == 0 && l->alive()) {
				luxOnly(l->lux());
				return from(AmbientSource::Native);
			}
		}
	}

	// Raw-HID master (any display's ALS) before the Sensor-API master.
	if (als_get_ambient(nullptr, &a.reading))
		return from(AmbientSource::Another);

	// Fallback: use first alive sensor (master)
	for (auto *l : g_alsListeners) {
		if (l->alive()) {
			luxOnly(l->lux());
			return from(AmbientSource::Another);
		}
	}

	// No live sensor yet (e.g. just after an ALS re-init): use the last known value, not a hard default.
	if (g_haveLastAmbient) {
		a.reading = g_lastAmbient;
		a.source  = AmbientSource::Last;
	} else {
		luxOnly(100.f); // nothing to go by yet
	}
	return a;
}

// Just the lux of it, which is all automatic brightness goes by.
static float getAmbientLux(const DisplayDevice &dev) {
	return getAmbient(dev.containerId).reading.lux;
}

/* ---------- mapping lux to brightness ---------- */
static ULONG mapLuxToBrightness(float lux, const DisplayDevice &dev) {
	float scale = std::clamp(lux, 2.f, 5000.f) / dev.baseLux;
	float tgt   = dev.baseBrightness * scale;
	// The limits from Options: only so far from the level set by hand, as a share of the range.
	float range = (float)(dev.maxBrightness - dev.minBrightness);
	float lo    = (float)dev.baseBrightness - range * (float)g_settings.autoDimLimit.load() / 100.f;
	float hi    = (float)dev.baseBrightness + range * (float)g_settings.autoBrightenLimit.load() / 100.f;
	tgt         = std::clamp(tgt, lo, hi);
	return static_cast<ULONG>(std::clamp(tgt, (float)dev.minBrightness, (float)dev.maxBrightness));
}

/* ---------- Central Brightness Setter ---------- */
static void SetBrightness(DisplayDevice &dev, ULONG val, bool isUserAction, bool showOSD) {
	if (g_hdrActive.load()) return;   // brightness writes are no-ops under HDR; Windows owns it
	if (dev.activePresetLocksBrightness()) return; // reference modes fix brightness (macOS locks it too)
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
				dev.baseLux = std::max(1.f, getAmbientLux(dev));
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

// The value an Apple display reports is only the last one it was sent. After a boot, a restart
// or a sleep its panel can come back at the firmware default while still reporting that value,
// so reading it tells nothing (#28). Send the brightness again regardless: one unit off first,
// so the display sees a real change like a slider move, then the value itself.
static void ReapplyBrightness(const wchar_t *reason) {
	if (g_hdrActive.load() || g_displays.empty()) return;

	if (g_settings.linkedMode && g_displays.size() > 1) {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		const auto &ref = g_displays[idx];
		SetBrightnessLinked(ref.currentBrightness, ref, true, false);
	}

	for (auto &dev : g_displays) {
		if (dev.maxBrightness <= dev.minBrightness || dev.activePresetLocksBrightness())
			continue;
		ULONG val   = std::clamp(dev.currentBrightness, dev.minBrightness, dev.maxBrightness);
		ULONG nudge = (val < dev.maxBrightness) ? val + 1 : val - 1;
		if (dev.setBrightness(nudge) != 0 || dev.setBrightness(val) != 0) {
			Log::Warn(L"Reapplying brightness failed on %s (%s)", dev.name.c_str(), reason);
			continue;
		}
		dev.currentBrightness = val;
		Log::Info(L"Reapplied brightness %lu to %s (%s)", val, dev.name.c_str(), reason);
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

/* ---------- manual brightness slider (tray popup and Options dialog) ---------- */
// What a brightness slider needs to know about the active display. The tray popup and the
// Options dialog both take it from here, so the two sliders show the same level and lock in
// exactly the same situations.
struct SliderState {
	bool  hasDisplay    = false;
	bool  hdr           = false; // Windows owns brightness while HDR is on
	bool  locked        = false; // reference-mode preset: brightness is fixed (macOS locks it there too)
	ULONG minBrightness = 1000;
	ULONG maxBrightness = 60000;
	ULONG brightness    = 0;

	// What automatic brightness works from: the level last set by hand and the ambient light
	// it was set in, and the ambient light now.
	ULONG baseBrightness = 0;
	float baseLux        = 0.f;
	Ambient ambient; // the ambient light now, and where that reading comes from

	bool usable() const { return hasDisplay && !hdr && !locked; }

	// A level as a position on a 0-100 slider. Rounded to the nearest, so that a level set from
	// a position reads back as that position.
	int percentOf(ULONG level) const {
		int range = (int)(maxBrightness - minBrightness);
		if (range <= 0) range = 1;
		float pct = (float)(level - minBrightness) / (float)range * 100.0f;
		return (int)std::lround(std::clamp(pct, 0.0f, 100.0f));
	}
	int percent() const { return percentOf(brightness); }
	int basePercent() const { return percentOf(baseBrightness); }
};

// Caller holds g_displayMutex.
static SliderState sliderStateLocked() {
	SliderState st;
	st.hdr = g_hdrActive.load();
	if (!g_displays.empty()) {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		const auto &ref  = g_displays[idx];
		st.hasDisplay    = true;
		st.locked        = ref.activePresetLocksBrightness();
		st.minBrightness = ref.minBrightness;
		st.maxBrightness = ref.maxBrightness;
		st.brightness    = ref.currentBrightness;
		st.baseBrightness = ref.baseBrightness;
		st.baseLux        = ref.baseLux;
		st.ambient        = getAmbient(ref.containerId);
	} else {
		st.ambient = getAmbient(GUID{}); // no display to ask for: whatever other sensor there is
	}
	return st;
}

static SliderState readSliderState() {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	return sliderStateLocked();
}

// Send a 0-100 slider position to the active display, or to all of them in linked mode.
// Returns the brightness that was asked for.
static ULONG applySliderPercent(int pct, ULONG refMin, ULONG refMax) {
	ULONG r   = refMax - refMin;
	ULONG val = refMin + (ULONG)((float)r * (float)pct / 100.0f);
	ApplyBrightness(val, true, false);
	return val;
}

/* ---------- the displays, as the Options dialog lists them ---------- */
// Which displays there are, which of them is the active one, and that one's presets. The
// dialog fills its display and preset lists from this, and compares it with a fresh one to tell
// when they need filling again: displays come and go, the tray menu can make another one
// active, and HDR changes which presets may be offered.
struct DisplaysView {
	std::vector<std::wstring> names;          // in the order of the tray menu
	std::vector<std::wstring> paths;          // device paths: two displays of one model share a name
	int                       active = -1;    // position of the active display, -1 when there is none
	bool                      hdr    = false;
	bool                      locked = false; // the active display's preset fixes its brightness
	std::vector<Preset>       presets;        // of the active display
	int                       activePreset = -1;

	bool sameAs(const DisplaysView &o) const {
		return names == o.names && paths == o.paths && active == o.active && hdr == o.hdr && locked == o.locked &&
		       activePreset == o.activePreset &&
		       std::equal(presets.begin(), presets.end(), o.presets.begin(), o.presets.end(),
		                  [](const Preset &a, const Preset &b) {
			                  return a.index == b.index && a.name == b.name && a.desc == b.desc;
		                  });
	}
};

// Caller holds g_displayMutex.
static DisplaysView displaysViewLocked() {
	DisplaysView v;
	v.hdr = g_hdrActive.load();
	for (const auto &dev : g_displays) {
		v.names.push_back(dev.name);
		v.paths.push_back(dev.devicePath);
	}
	if (!g_displays.empty()) {
		ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
		const auto &ref = g_displays[idx];
		v.active        = (int)idx;
		v.locked        = ref.activePresetLocksBrightness();
		v.presets       = ref.presets;
		v.activePreset  = ref.activePresetIndex;
	}
	return v;
}

// The displays and the slider state of the active one, read together so that they describe the
// same moment. A UI timer passes wait = false: the worker keeps the display lock across HID
// I/O, which can take seconds on a panel that is waking up, so it is better to give up and
// catch up next time than to stall the message pump.
static bool readDisplays(DisplaysView &v, SliderState &st, bool wait) {
	std::unique_lock<std::mutex> lock(g_displayMutex, std::defer_lock);
	if (wait)
		lock.lock();
	else if (!lock.try_lock())
		return false;
	v  = displaysViewLocked();
	st = sliderStateLocked();
	return true;
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

// Enable or disable a control. A control that is being disabled hands the focus on first, or
// the keyboard would be left on a window that ignores it.
static void enableDlgItem(HWND d, int ctrlId, bool enable) {
	HWND c = GetDlgItem(d, ctrlId);
	if (!c || (IsWindowEnabled(c) != FALSE) == enable)
		return;
	if (!enable && GetFocus() == c)
		SendMessageW(d, WM_NEXTDLGCTL, 0, FALSE);
	EnableWindow(c, enable);
}
// Tick or untick a checkbox, leaving it alone when it already shows that.
static void checkDlgItem(HWND d, int ctrlId, bool on) {
	if ((IsDlgButtonChecked(d, ctrlId) == BST_CHECKED) != on)
		CheckDlgButton(d, ctrlId, on ? BST_CHECKED : BST_UNCHECKED);
}

// The number fields of the dialog: an edit box with a spin control, the range of the number, and
// the setting the field edits.
struct NumberField {
	int   editId, spinId;
	ULONG lo, hi;
	ULONG initial;    // what "Reset to Defaults" puts in the field
	ULONG step;       // what one press of an arrow changes the number by
	ULONG (*saved)(); // the setting as it is saved now
};
static const NumberField kNumberFields[] = {
	{IDC_BRIGHTEN_LIMIT, IDC_BRIGHTEN_LIMIT_SPIN, kMinAutoLimit, kMaxAutoLimit, kDefaultAutoLimit, 1,
	 [] { return g_settings.autoBrightenLimit.load(); }},
	{IDC_DIM_LIMIT, IDC_DIM_LIMIT_SPIN, kMinAutoLimit, kMaxAutoLimit, kDefaultAutoLimit, 1,
	 [] { return g_settings.autoDimLimit.load(); }},
	// The arrows of the ramp times move by a tenth of a second.
	{IDC_RAMP_BRIGHTEN, IDC_RAMP_BRIGHTEN_SPIN, kMinRampMs, kMaxRampMs, kDefaultRampBrightenMs, 100,
	 [] { return g_settings.rampBrightenMs.load(); }},
	{IDC_RAMP_DIM, IDC_RAMP_DIM_SPIN, kMinRampMs, kMaxRampMs, kDefaultRampDimMs, 100,
	 [] { return g_settings.rampDimMs.load(); }},
	{IDC_BRIGHTNESS_STEPS, IDC_BRIGHTNESS_STEPS_SPIN, kMinBrightnessSteps, kMaxBrightnessSteps,
	 kDefaultBrightnessSteps, 1, [] { return g_settings.brightnessSteps; }},
};
static const NumberField *numberField(int editId) {
	for (const auto &f : kNumberFields)
		if (f.editId == editId)
			return &f;
	return nullptr;
}
static void initNumberFields(HWND d) {
	for (const auto &f : kNumberFields) {
		UDACCEL accel{0, (UINT)f.step};
		// No more digits than the maximum has.
		SendDlgItemMessageW(d, f.editId, EM_SETLIMITTEXT, std::to_wstring(f.hi).size(), 0);
		SendDlgItemMessageW(d, f.spinId, UDM_SETRANGE32, f.lo, f.hi);
		SendDlgItemMessageW(d, f.spinId, UDM_SETACCEL, 1, (LPARAM)&accel);
		SendDlgItemMessageW(d, f.spinId, UDM_SETPOS32, 0, f.saved());
	}
}
// The number a field counts as. Read from the edit box rather than the spin control, so that a
// typed number outside the range becomes the nearest one inside it instead of being dropped. An
// empty box counts as the saved setting.
static ULONG readNumberField(HWND d, int editId) {
	const NumberField *f = numberField(editId);
	if (!f)
		return 0;
	BOOL ok    = FALSE;
	UINT typed = GetDlgItemInt(d, editId, &ok, FALSE);
	return ok ? std::clamp((ULONG)typed, f->lo, f->hi) : f->saved();
}
// When a number field is left, put in it the number it counts as: what the box shows is then
// what Save will store.
static void settleNumberField(HWND d, const NumberField &f) {
	SendDlgItemMessageW(d, f.spinId, UDM_SETPOS32, 0, readNumberField(d, f.editId));
}
// Automatic brightness as the dialog shows it: the checkbox available and ticked.
static bool autoBrightnessTicked(HWND d) {
	return IsWindowEnabled(GetDlgItem(d, IDC_AUTO_BRIGHTNESS)) &&
	       IsDlgButtonChecked(d, IDC_AUTO_BRIGHTNESS) == BST_CHECKED;
}
// The limits and the ramp times only shape automatic changes (a manual change is applied at
// once and in full), so they can be edited only while automatic brightness is ticked.
static void updateAutoFields(HWND d) {
	bool on = autoBrightnessTicked(d);
	for (int ctrlId : {IDC_BRIGHTEN_LIMIT, IDC_BRIGHTEN_LIMIT_SPIN, IDC_DIM_LIMIT, IDC_DIM_LIMIT_SPIN,
	                   IDC_RAMP_BRIGHTEN, IDC_RAMP_BRIGHTEN_SPIN, IDC_RAMP_DIM, IDC_RAMP_DIM_SPIN})
		enableDlgItem(d, ctrlId, on);
}

// The percent field next to the slider shows the slider position as a number, for typing an
// exact level. s_fillingPercent tells our own updates of the box from the user's typing;
// s_percentTyped marks a typed value that has not been sent to the display yet.
static bool s_fillingPercent = false;
static bool s_percentTyped   = false;

static void setPercentField(HWND d, int pct) {
	s_fillingPercent = true;
	SendDlgItemMessageW(d, IDC_SLIDER_PERCENT_SPIN, UDM_SETPOS32, 0, pct);
	s_fillingPercent = false;
}

// Send a level from the slider or the percent field to the display, and show it in both.
// Applied at once, like the tray slider: brightness does not wait for Save.
static void applyOptionsPercent(HWND d, int pct) {
	KillTimer(d, ID_TYPING_TIMER);
	s_percentTyped = false;

	// If the display does not take the new level, nothing is remembered as matched, and the
	// timer puts the controls back where the display really is.
	HWND        slider  = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
	ULONG       matched = ~0ul;
	SliderState st      = readSliderState();
	if (st.usable()) {
		ULONG asked = applySliderPercent(pct, st.minBrightness, st.maxBrightness);
		if (readSliderState().brightness == asked)
			matched = asked;
	}
	SetWindowLongPtrW(slider, GWLP_USERDATA, (LONG_PTR)matched);

	if ((int)SendMessageW(slider, TBM_GETPOS, 0, 0) != pct)
		SendMessageW(slider, TBM_SETPOS, TRUE, pct);
	// Rewriting the box moves the caret, so leave it alone when it already says the same.
	BOOL ok    = FALSE;
	UINT shown = GetDlgItemInt(d, IDC_SLIDER_PERCENT, &ok, FALSE);
	if (!ok || (int)shown != pct)
		setPercentField(d, pct);
}

// Apply what was typed into the percent field. A box left empty goes back to the slider's value.
static void commitPercentField(HWND d, bool leaving) {
	KillTimer(d, ID_TYPING_TIMER);
	BOOL ok    = FALSE;
	UINT typed = GetDlgItemInt(d, IDC_SLIDER_PERCENT, &ok, FALSE);
	if (ok && s_percentTyped)
		applyOptionsPercent(d, (int)std::min(typed, 100u));
	else if (!ok && leaving && IsWindowEnabled(GetDlgItem(d, IDC_BRIGHTNESS_SLIDER)))
		setPercentField(d, (int)SendDlgItemMessageW(d, IDC_BRIGHTNESS_SLIDER, TBM_GETPOS, 0, 0));
	s_percentTyped = false;
}

// With automatic brightness on, the level on screen drifts away from the one set by hand. The
// line under the checkbox and the marks on the slider keep the two apart: what was set and in
// what ambient light, where that has led now, and how far it may lead. All of it is blank while
// automatic brightness is off or cannot work, when the two levels are the same thing. (The
// dialog is as wide as it is so that the line fits with six-digit lux values, which is direct
// sunlight.)
static std::wstring luxText(float lux) {
	wchar_t buf[32];
	swprintf_s(buf, lux < 10.f ? L"%.1f" : L"%.0f", lux);
	return buf;
}

// The marks on the slider: a tick at the level that was set and, when the limits leave
// automatic brightness less than the whole slider, a highlight on the part it may use, with a
// tick at each end of it that is a limit.
struct SliderMarks {
	int set = -1;         // slider positions; -1 = not marked
	int lo = -1, hi = -1; // the part automatic brightness may use
	bool sameAs(const SliderMarks &o) const { return set == o.set && lo == o.lo && hi == o.hi; }
};

// What an open Options dialog keeps between messages, in its DWLP_USER.
// One control of the part of the dialog that scrolls: everything but the buttons at the bottom.
struct PageItem {
	HWND ctrl;
	RECT at;      // where the layout puts it: the dialog at its design width, scrolled to the top
	int  open;    // a dropdown list: its height with the list open, which is the height it is sized by
	int  follow;  // the dialog's right edge: 0 not at all, 1 with its own right edge, 2 as a whole
	bool clipped; // has a window region at the moment
};
// Where a control is as the dialog opens (see noteAsOpened).
struct OpenedItem {
	HWND ctrl;
	RECT at;   // in the dialog
	int  open; // a dropdown list: its height with the list open
};
struct OptionsState {
	DisplaysView shown;            // what the display and preset lists were last filled from
	bool         filled = false;   // not before the first fill
	HWND         tip    = nullptr; // the tooltip: the explanations, and why a control is disabled
	SliderMarks  marks;            // what the slider has been given to mark

	// The dialog as it opened: what it is laid out from again for another scaling (see placeAsOpened).
	std::vector<OpenedItem> opened;
	SIZE openedUnits = {}; // the size of the dialog units those places were measured with

	// The scrolling part (see layOutPage).
	std::vector<PageItem> page;
	RECT buttons[3] = {};    // the buttons under it, in the dialog at its design size
	SIZE design     = {};    // the client size the template gives the dialog
	int  pageEnd    = 0;     // at that size, where the scrolling part ends and the row of buttons begins
	int  content    = 0;     // how high everything that scrolls is
	int  scroll     = 0;     // how far it is scrolled
	RECT view       = {};    // the part of the dialog it shows through, as last laid out
	SIZE units      = {};    // the size of the dialog units all of the above is measured with
	SIZE client     = {};    // the client size the dialog had when it was last told of one
	SIZE wanted     = {};    // the client size to keep: the user's or the full one (see sizeDialog)
	int  scrollKept = 0;     // how far it was scrolled at the last change of scaling, in the units of `wanted`...
	int  scrollThen = 0;     // ...and in the pixels that came to
	bool sizing     = false; // the dialog is setting its size itself
	bool dragged    = false; // the user is moving or sizing the dialog with the mouse
	int  wheel      = 0;     // mouse wheel movement not yet turned into scrolling
	bool wheelKept  = false; // a control is using the wheel itself (see PageCtrlProc)
	bool rescaling  = false; // from the first word of another scaling until all is laid out for it
};
static OptionsState *optionsState(HWND d) {
	return (OptionsState *)GetWindowLongPtrW(d, DWLP_USER);
}

// The marks are also given to the slider as its tick list, which is what turns them into
// pixels: the set level alone, or the low end, the set level and the high end. Always in
// ascending order, so that it does not matter whether the control keeps its list as given or
// sorted. They are painted by drawSliderMarks.
static void showSliderMarks(HWND d, const SliderMarks &m) {
	OptionsState *os = optionsState(d);
	if (!os || os->marks.sameAs(m))
		return;
	os->marks   = m;
	HWND slider = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
	SendMessageW(slider, TBM_CLEARTICS, FALSE, 0);
	if (m.lo >= 0)
		SendMessageW(slider, TBM_SETTIC, 0, m.lo);
	if (m.set >= 0)
		SendMessageW(slider, TBM_SETTIC, 0, m.set);
	if (m.lo >= 0)
		SendMessageW(slider, TBM_SETTIC, 0, m.hi);
	InvalidateRect(slider, nullptr, TRUE);
}

// For a line of text that is rewritten on a timer: rewriting the same text would only make it flicker.
static void showText(HWND d, int ctrlId, const std::wstring &text) {
	wchar_t shown[160] = {};
	GetDlgItemTextW(d, ctrlId, shown, 160);
	if (text != shown)
		SetDlgItemTextW(d, ctrlId, text.c_str());
}

// The "Ambient light:" line: the reading automatic brightness goes by for the picked display, and
// in brackets where it comes from, because a sensor that is not the display's own is somewhere
// else and sees other light. Shown whether automatic brightness is on or not.
static std::wstring ambientText(const Ambient &a) {
	if (a.source == AmbientSource::None)
		return L"No ambient light reading";
	const AmbientReading &r    = a.reading;
	std::wstring          text = luxText(r.lux) + L" lx";
	wchar_t               buf[128];
	if (r.hasColour && r.colorTemp > 0.f) {
		swprintf_s(buf, L", %.0fK", r.colorTemp);
		text += buf;
	}
	if (r.hasColour && (r.chromaX > 0.f || r.chromaY > 0.f)) {
		swprintf_s(buf, L", x=%.4f y=%.4f CIE 1931", r.chromaX, r.chromaY);
		text += buf;
	}
	text += a.source == AmbientSource::Native ? L" (native sensor)" : a.source == AmbientSource::Another ? L" (another sensor)" : L" (last reading)";
	return text;
}

// Without a display the line is there all the same: a sensor does not have to be a display's,
// and with none at all it says that there is no reading.
static void showAmbient(HWND d, const SliderState &st) {
	showText(d, IDC_AMBIENT_VALUE, ambientText(st.ambient));
}

static void showAutoStatus(HWND d, const SliderState &st) {
	bool         on = st.usable() && autoBrightnessTicked(d);
	std::wstring text;
	if (on && !st.ambient.live()) {
		text = L"No ambient light reading, so there is nothing to follow";
	} else if (on) {
		wchar_t buf[160];
		swprintf_s(buf, L"Set: %d %% at %s lx of ambient light \u00B7 Now: %d %% at %s lx of ambient light",
		           st.basePercent(), luxText(st.baseLux).c_str(), st.percent(), luxText(st.ambient.reading.lux).c_str());
		text = buf;
	}
	showText(d, IDC_AUTO_STATUS, text);

	// The limits are taken from their fields rather than from the settings, so that the marks
	// show what the numbers in the dialog would do before they are saved.
	SliderMarks m;
	if (on) {
		int brighten = (int)readNumberField(d, IDC_BRIGHTEN_LIMIT);
		int dim      = (int)readNumberField(d, IDC_DIM_LIMIT);
		m.set        = st.basePercent();
		int lo = std::max(m.set - dim, 0), hi = std::min(m.set + brighten, 100);
		if (lo > 0 || hi < 100) { // otherwise the whole slider is in use, and marking all of it says nothing
			m.lo = lo;
			m.hi = hi;
		}
	}
	showSliderMarks(d, m);
}

// NM_CUSTOMDRAW of the slider: paints the marks. A trackbar that draws any tick also draws one
// at each end of its range, and those two would only compete with the ticks that mean something
// here. So the control's own tick drawing is skipped, and the marks are drawn alone.
static LRESULT drawSliderMarks(HWND d, const NMCUSTOMDRAW *cd) {
	if (cd->dwDrawStage == CDDS_PREPAINT)
		return CDRF_NOTIFYITEMDRAW;

	// Where the marks are in the slider's tick list (see showSliderMarks). An end of the part
	// automatic brightness may use is a limit unless it is the end of the slider itself.
	const OptionsState *os = optionsState(d);
	SliderMarks         m  = os ? os->marks : SliderMarks();
	HWND slider  = cd->hdr.hwndFrom;
	bool range   = m.lo >= 0;
	int  setTick = range ? 1 : 0, loTick = 0, hiTick = 2;
	bool loLimit = range && m.lo > (int)SendMessageW(slider, TBM_GETRANGEMIN, 0, 0);
	bool hiLimit = range && m.hi < (int)SendMessageW(slider, TBM_GETRANGEMAX, 0, 0);
	int  dpi     = DpiOfWindow(slider);

	if (cd->dwItemSpec == TBCD_CHANNEL) {
		// The highlight lies on the channel and under the thumb, so it is painted right after
		// the channel. Without a limit on a side it runs to that end of the channel.
		if (cd->dwDrawStage == CDDS_ITEMPREPAINT)
			return range ? CDRF_NOTIFYPOSTPAINT : CDRF_DODEFAULT;
		if (cd->dwDrawStage == CDDS_ITEMPOSTPAINT && range) {
			RECT band;
			SendMessageW(slider, TBM_GETCHANNELRECT, 0, (LPARAM)&band);
			if (loLimit)
				band.left = (LONG)SendMessageW(slider, TBM_GETTICPOS, loTick, 0);
			if (hiLimit)
				band.right = (LONG)SendMessageW(slider, TBM_GETTICPOS, hiTick, 0) + DpiScale(1, dpi);
			if (band.right > band.left)
				FillRect(cd->hdc, &band, GetSysColorBrush(COLOR_HIGHLIGHT));
		}
		return CDRF_DODEFAULT;
	}
	if (cd->dwItemSpec != TBCD_TICS || cd->dwDrawStage != CDDS_ITEMPREPAINT)
		return CDRF_DODEFAULT;

	RECT thumb;
	SendMessageW(slider, TBM_GETTHUMBRECT, 0, (LPARAM)&thumb);
	auto tick = [&](int index, int color) {
		int x = (int)SendMessageW(slider, TBM_GETTICPOS, index, 0); // -1: there is no such tick
		if (x < 0)
			return;
		RECT rc{x, thumb.bottom + DpiScale(2, dpi), x + DpiScale(1, dpi), thumb.bottom + DpiScale(7, dpi)};
		FillRect(cd->hdc, &rc, GetSysColorBrush(color));
	};
	if (loLimit)
		tick(loTick, COLOR_HIGHLIGHT);
	if (hiLimit)
		tick(hiTick, COLOR_HIGHLIGHT);
	if (m.set >= 0)
		tick(setTick, COLOR_BTNTEXT); // last, so that it shows where a limit of 0 puts a limit tick on it
	return CDRF_SKIPDEFAULT;
}

// Bring the brightness controls in line with the active display: which ones are available and
// where the slider sits. Part of syncOptions.
static void showBrightnessState(HWND d, const SliderState &st, bool opening) {
	// The slider locks exactly when the tray slider does. As in Boot Camp, its labels grey out with it.
	for (int ctrlId : {IDC_BRIGHTNESS_SLIDER, IDC_SLIDER_DIM_LABEL, IDC_SLIDER_BRIGHT_LABEL,
	                   IDC_SLIDER_PERCENT, IDC_SLIDER_PERCENT_SPIN, IDC_SLIDER_PERCENT_UNIT})
		enableDlgItem(d, ctrlId, st.usable());

	// Auto-brightness is unavailable while Windows owns brightness (HDR) and while a
	// reference-mode preset fixes it (macOS locks it there too). Its checkbox is the setting
	// itself, in force from the click on, and the tray menu can turn it as well.
	checkDlgItem(d, IDC_AUTO_BRIGHTNESS, g_settings.autoAdjustEnabled.load());
	enableDlgItem(d, IDC_AUTO_BRIGHTNESS, !st.hdr && !st.locked);
	updateAutoFields(d);
	showAmbient(d, st);
	showAutoStatus(d, st);

	// Follow the display, but not mid-drag and not after the slider's own change: a position is
	// coarser than the brightness it was mapped to, and mapping that back can nudge the thumb.
	// GWLP_USERDATA remembers the brightness the position was last matched to (see WM_HSCROLL).
	HWND slider = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
	if (st.hasDisplay && GetCapture() != slider &&
	    (opening || (ULONG)GetWindowLongPtrW(slider, GWLP_USERDATA) != st.brightness)) {
		SendMessageW(slider, TBM_SETPOS, TRUE, st.percent());
		SetWindowLongPtrW(slider, GWLP_USERDATA, (LONG_PTR)st.brightness);
		if (!s_percentTyped) // not over a number the user is in the middle of typing
			setPercentField(d, st.percent());
	}
}

// A tooltip window for longer, wrapped text. Owned by the dialog, so destroyed with it.
static HWND createWrappedTooltip(HWND d) {
	HWND tip = CreateWindowExW(WS_EX_TOPMOST, TOOLTIPS_CLASS, nullptr, WS_POPUP | TTS_ALWAYSTIP | TTS_NOPREFIX,
	                           0, 0, 0, 0, d, nullptr, g_hInst, nullptr);
	if (tip) {
		RECT width{0, 0, 180, 0}; // dialog units, so the wrap width follows the font and the DPI
		MapDialogRect(d, &width);
		SendMessageW(tip, TTM_SETMAXTIPWIDTH, 0, width.right);
	}
	return tip;
}

/* ---------- rows: inputs of one height, labels on the inputs' line of text ---------- */
static RECT rectInDialog(HWND d, HWND ctrl) {
	RECT rc;
	GetWindowRect(ctrl, &rc);
	MapWindowPoints(nullptr, d, (POINT *)&rc, 2);
	return rc;
}
static void moveInDialog(HWND ctrl, const RECT &rc) {
	SetWindowPos(ctrl, nullptr, rc.left, rc.top, rc.right - rc.left, rc.bottom - rc.top, SWP_NOZORDER | SWP_NOACTIVATE);
}
static bool isClass(HWND ctrl, const wchar_t *name) {
	wchar_t cls[64] = {};
	GetClassNameW(ctrl, cls, 64);
	return lstrcmpiW(cls, name) == 0;
}

// Where an input's line of text begins, counted from the top of the dialog.
static int textLineTop(HWND d, HWND input, int lineHeight) {
	POINT at = {0, 0};
	if (isClass(input, WC_COMBOBOXW)) {
		COMBOBOXINFO cbi = {sizeof(cbi)}; // in the middle of the part beside the button
		GetComboBoxInfo(input, &cbi);
		at.y = (cbi.rcItem.top + cbi.rcItem.bottom - lineHeight) / 2;
	} else if (isClass(input, WC_EDITW)) {
		RECT text = {};
		SendMessageW(input, EM_GETRECT, 0, (LPARAM)&text);
		at.y = text.top;
	} else {
		at.y = GetSystemMetrics(SM_CYBORDER); // a hotkey box: one border in
	}
	MapWindowPoints(input, d, &at, 1);
	return at.y;
}

// The template is laid out in dialog units, and two things in it do not go by them. A dropdown
// list ignores the height it is given: it takes the height of the font plus a few pixels that
// do not grow with the scaling. An edit box does take the height it is given, and puts its text
// at the top of it. So the higher the scaling, the lower the lists are next to the other
// inputs, the further the text of those sits above their middle, and the further every label is
// off the text beside it: an offset in the template is right at one scaling only.
// This sets the rows right from what the controls measure:
//  - an edit box or a hotkey box gets the height of a dropdown list, around its own middle;
//  - a dropdown list goes to the middle of the row the template gives it;
//  - a label goes onto the line of text of the input it shares a row with;
//  - the slider goes with its channel on the middle of that line.
// Runs when the dialog opens and again for a monitor with another scaling, each time with the
// controls where they were as the dialog opened (see placeAsOpened): the second step would not
// come out the same from where a previous run has left them.
static void layOutRows(HWND d) {
	RECT rc     = rectInDialog(d, GetDlgItem(d, IDC_PRESET_COMBO));
	int  height = rc.bottom - rc.top; // of a dropdown list, and so of every input
	RECT row    = {0, 0, 0, 14};      // the height of a row of inputs in the template, in dialog units
	MapDialogRect(d, &row);

	TEXTMETRICW tm  = {};
	HDC         dc  = GetDC(d);
	HGDIOBJ     old = SelectObject(dc, (HFONT)SendMessageW(d, WM_GETFONT, 0, 0));
	GetTextMetricsW(dc, &tm);
	SelectObject(dc, old);
	ReleaseDC(d, dc);

	// A spin control sits inside the edit box it belongs to: note where, before the box changes.
	struct Spin {
		HWND spin, edit;
		int  top, bottom; // how far in from the top and the bottom of the edit box
	};
	std::vector<Spin> spins;
	for (HWND c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		HWND edit = isClass(c, UPDOWN_CLASSW) ? (HWND)SendMessageW(c, UDM_GETBUDDY, 0, 0) : nullptr;
		if (!edit)
			continue;
		RECT s = rectInDialog(d, c), e = rectInDialog(d, edit);
		spins.push_back({c, edit, s.top - e.top, e.bottom - s.bottom});
	}

	std::vector<HWND> inputs;
	for (HWND c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		rc = rectInDialog(d, c);
		if (isClass(c, WC_COMBOBOXW)) {
			// Moved, not sized: the size of a dropdown list is the size of its open list.
			SetWindowPos(c, nullptr, rc.left, rc.top + (row.bottom - height) / 2, 0, 0,
			             SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE);
		} else if (isClass(c, WC_EDITW) || isClass(c, HOTKEY_CLASSW)) {
			rc.top    = (rc.top + rc.bottom - height) / 2;
			rc.bottom = rc.top + height;
			moveInDialog(c, rc);
		} else {
			continue;
		}
		inputs.push_back(c);
	}
	for (const Spin &s : spins) {
		RECT e    = rectInDialog(d, s.edit);
		rc        = rectInDialog(d, s.spin);
		rc.top    = e.top + s.top;
		rc.bottom = e.bottom - s.bottom;
		moveInDialog(s.spin, rc);
	}

	// A label shares a row with the input that its middle is level with.
	for (HWND c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		if (!isClass(c, WC_STATICW))
			continue;
		rc         = rectInDialog(d, c);
		int middle = (rc.top + rc.bottom) / 2;
		for (HWND input : inputs) {
			RECT in = rectInDialog(d, input);
			if (middle < in.top || middle >= in.bottom)
				continue;
			OffsetRect(&rc, 0, textLineTop(d, input, tm.tmHeight) - rc.top);
			moveInDialog(c, rc);
			break;
		}
	}

	// The slider's row takes its line from the percent field.
	HWND slider  = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
	RECT channel = {};
	SendMessageW(slider, TBM_GETCHANNELRECT, 0, (LPARAM)&channel);
	POINT middle = {0, (channel.top + channel.bottom) / 2};
	MapWindowPoints(slider, d, &middle, 1);
	rc = rectInDialog(d, slider);
	OffsetRect(&rc, 0, textLineTop(d, GetDlgItem(d, IDC_SLIDER_PERCENT), tm.tmHeight) + tm.tmHeight / 2 - middle.y);
	moveInDialog(slider, rc);
}

// The size of the dialog units at the moment, in pixels: of 4 horizontal ones and of 8 vertical
// ones, which is how they are defined. They follow the font, and the font follows the scaling of
// the monitor.
static SIZE dialogUnits(HWND d) {
	RECT rc = {0, 0, 4, 8};
	MapDialogRect(d, &rc);
	return {std::max(rc.right, 1L), std::max(rc.bottom, 1L)};
}

// Note where everything is as the dialog opens: where the template has put it, and where a spin
// control has then put itself and its edit box.
// On a monitor with another scaling the dialog is laid out from these places again. Windows
// does rescale the controls of a dialog itself, but from wherever they are at that moment, and
// here that is where scrolling and resizing have them; and each such step is rounded, so that
// going back and forth between two monitors would add up.
static void noteAsOpened(HWND d) {
	OptionsState *os = optionsState(d);
	if (!os)
		return;
	os->opened.clear();
	os->openedUnits = dialogUnits(d);
	for (HWND c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		OpenedItem item = {c, rectInDialog(d, c), 0};
		if (isClass(c, WC_COMBOBOXW)) {
			RECT open = {};
			SendMessageW(c, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&open);
			item.open = open.bottom - open.top;
		}
		os->opened.push_back(item);
	}
}

// Put everything where it was as the dialog opened, in the dialog units of the moment: where a
// dialog opened at this scaling has it, to a pixel or so. Edges are converted and not sizes, so
// that what touched still touches.
static void placeAsOpened(HWND d) {
	OptionsState *os = optionsState(d);
	if (!os || os->opened.empty())
		return;
	const SIZE now = dialogUnits(d), then = os->openedUnits;
	HDWP       batch = BeginDeferWindowPos((int)os->opened.size());
	for (const OpenedItem &item : os->opened) {
		SetWindowRgn(item.ctrl, nullptr, FALSE); // scrolling may have left it cut (see layOutPage)
		RECT rc     = {MulDiv(item.at.left, now.cx, then.cx), MulDiv(item.at.top, now.cy, then.cy)};
		rc.right    = MulDiv(item.at.right, now.cx, then.cx);
		rc.bottom   = MulDiv(item.at.bottom, now.cy, then.cy);
		int  height = item.open ? MulDiv(item.open, now.cy, then.cy) : rc.bottom - rc.top;
		if (batch)
			batch = DeferWindowPos(batch, item.ctrl, nullptr, rc.left, rc.top, rc.right - rc.left, height, SWP_NOZORDER | SWP_NOACTIVATE);
	}
	if (batch)
		EndDeferWindowPos(batch);
}

// Longer explanations, each shown as a tooltip when hovering the label it belongs to; for a
// checkbox that is its text, not its box. The same places get the help cursor (see cursorAt).
static const wchar_t kRotateHelp[] =
    L"Only a 90\u00B0 clockwise rotation works. A 90\u00B0 counterclockwise rotation and 180\u00B0 rotations don't work."
    L"\r\n\r\n"
    L"This is a limitation of the display's firmware. It only reports two orientations: landscape (0\u00B0) and "
    L"portrait (90\u00B0 clockwise).";
static const wchar_t kSyncHelp[] =
    L"A brightness change made by hand, with either slider or with the shortcuts, is applied to all connected "
    L"displays. The app also brings the other displays in line with the selected one when a display is detected "
    L"and when the displays come back on after sleep. Displays of different models are matched in nits, so that "
    L"they look equally bright.\r\n\r\n"
    L"Automatic brightness and the preset stay separate for each display.";
static const wchar_t kAmbientHelp[] =
    L"The reading is looked for in this order:\r\n\r\n"
    L"1. The display's own sensor.\r\n"
    L"2. The sensor of any other Apple display.\r\n"
    L"3. Any Windows ambient light sensor, for example the one built into a laptop.\r\n"
    L"4. The last known value.\r\n\r\n"
    L"Please note that another sensor is in a different place and does not measure the light that falls on "
    L"the selected display.";
static const wchar_t kAutoHelp[] =
    L"Adjusts the brightness as the ambient light changes.\r\n\r\n"
    L"Each time you set the brightness level by hand, with a slider or the shortcuts, that value and the "
    L"ambient light value at that moment become the new reference point.\r\n\r\n"
    L"From then on the target point is that level scaled by how much the ambient light has changed.\r\n\r\n"
    L"For example, 40 % set in 200 lx becomes about 20 % in 100 lx and about 80 % in 400 lx.\r\n\r\n"
    L"It reacts only when the light changes by 20 % or more, and moves to the new level gradually.\r\n\r\n"
    L"The limits and times below set how far and how fast it may go.\r\n\r\n"
    L"It works for each display separately, from the sensor shown in \"Ambient light\", and does nothing "
    L"while HDR is on, while a preset fixes the brightness, or while no sensor is reporting.\r\n\r\n"
    L"The line below shows the reference (Set) and the current brightness (Now).";
static const wchar_t kBrightenLimitHelp[] =
    L"How far automatic brightness may raise the brightness above the level you set, in percent of the "
    L"brightness slider.\r\n\r\n"
    L"For example, with the brightness set to 40 % and a limit of 30, it stops at 70 % however bright the room "
    L"gets. With 0 it never goes above the level you set, and with 100 there is no limit.\r\n\r\n"
    L"On the slider, the highlighted part is what the two limits leave to automatic brightness.";
static const wchar_t kDimLimitHelp[] =
    L"How far automatic brightness may lower the brightness below the level you set, in percent of the "
    L"brightness slider.\r\n\r\n"
    L"For example, with the brightness set to 40 % and a limit of 30, it stops at 10 % however dark the room "
    L"gets. With 0 it never goes below the level you set, and with 100 there is no limit.\r\n\r\n"
    L"On the slider, the highlighted part is what the two limits leave to automatic brightness.";
static const wchar_t kStepsHelp[] =
    L"The number of steps the whole brightness range is divided into for the brightness shortcuts: "
    L"the keyboard's native brightness keys and the custom shortcuts set below.\r\n\r\n"
    L"One press changes brightness by (max - min) / steps, so fewer steps means a bigger change per press.";
static const struct {
	int            ctrlId;
	const wchar_t *text;
} kHelpTexts[] = {{IDC_AUTO_ROTATE, kRotateHelp},
                  {IDC_LINK_DISPLAYS, kSyncHelp},
                  {IDC_AMBIENT_LABEL, kAmbientHelp},
                  {IDC_AUTO_BRIGHTNESS, kAutoHelp},
                  {IDC_BRIGHTEN_LIMIT_LABEL, kBrightenLimitHelp},
                  {IDC_DIM_LIMIT_LABEL, kDimLimitHelp},
                  {IDC_STEPS_LABEL, kStepsHelp}};

static bool hasHelpText(int ctrlId) {
	for (const auto &help : kHelpTexts)
		if (help.ctrlId == ctrlId)
			return true;
	return false;
}

// The controls that say why they are disabled, when there is a text for the reason (see
// fillDisplayControls).
static const int kWhyControls[] = {IDC_AUTO_BRIGHTNESS, IDC_BRIGHTNESS_SLIDER, IDC_SLIDER_PERCENT,
                                   IDC_SLIDER_PERCENT_SPIN, IDC_PRESET_COMBO};

/* ---------- the tooltip: explanations, and why a control is disabled ---------- */
// One tooltip for the whole dialog. Its tools are places, and there are two kinds of them:
//  - in a control, for the mouse the control has: a label that has an explanation, and of a
//    checkbox that has one only its label, not its box;
//  - in the dialog, for the mouse the dialog has, which is the mouse over a disabled control:
//    the same explanation on the same label, and on what cannot be operated the reason, if
//    there is a text for it. On a checkbox what cannot be operated is its box.
// A place in the dialog is told by the id of its control, plus kWhyTool for a reason.
constexpr UINT_PTR kLabelTool = 1;
constexpr UINT_PTR kWhyTool   = 0x10000;

static bool isCheckbox(HWND ctrl) {
	return isClass(ctrl, WC_BUTTONW) && (GetWindowLongW(ctrl, GWL_STYLE) & BS_TYPEMASK) == BS_AUTOCHECKBOX;
}

// A checkbox is a box and, beside it, a label. Where the label begins, counted from the left
// edge of the control: what the control needs in all, less what its text takes.
static int checkboxLabelLeft(HWND box) {
	RECT own;
	GetClientRect(box, &own);
	wchar_t text[128] = {};
	int     length    = GetWindowTextW(box, text, 128);
	SIZE    all = {}, label = {};
	HDC     dc  = GetDC(box);
	HGDIOBJ old = SelectObject(dc, (HFONT)SendMessageW(box, WM_GETFONT, 0, 0));
	GetTextExtentPoint32W(dc, text, length, &label);
	SelectObject(dc, old);
	ReleaseDC(box, dc);
	LONG left = SendMessageW(box, BCM_GETIDEALSIZE, 0, (LPARAM)&all) && all.cx > label.cx
	                ? all.cx - label.cx
	                : own.bottom + own.bottom / 4; // no answer: a box is about as wide as the control is high
	return (int)std::min(left, own.right);
}

// A control's place in the dialog: all of it, or one of the two sides of a checkbox.
enum class Part { All, Box, Label };
static RECT partInDialog(HWND d, HWND ctrl, Part part) {
	RECT rc = rectInDialog(d, ctrl);
	if (part != Part::All) {
		LONG split = std::min(rc.right, rc.left + checkboxLabelLeft(ctrl));
		(part == Part::Box ? rc.right : rc.left) = split;
	}
	return rc;
}

static void addTool(HWND tip, HWND in, UINT_PTR id, const wchar_t *text) {
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.uFlags      = TTF_SUBCLASS;
	ti.hwnd        = in;
	ti.uId         = id;
	ti.lpszText    = (LPWSTR)text;
	SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti); // placeTips says where
}
static void moveTool(HWND tip, HWND in, UINT_PTR id, const RECT &rc) {
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.hwnd        = in;
	ti.uId         = id;
	ti.rect        = rc;
	SendMessageW(tip, TTM_NEWTOOLRECTW, 0, (LPARAM)&ti);
}

static HWND addHelpTexts(HWND d) {
	HWND tip = createWrappedTooltip(d);
	if (!tip)
		return nullptr;
	SendMessageW(tip, TTM_SETDELAYTIME, TTDT_AUTOPOP, 30000); // long enough to read
	for (const auto &help : kHelpTexts) {
		addTool(tip, GetDlgItem(d, help.ctrlId), kLabelTool, help.text);
		addTool(tip, d, (UINT_PTR)help.ctrlId, help.text);
	}
	// The reasons keep their places for good; a place with an empty text shows nothing.
	for (int ctrlId : kWhyControls)
		addTool(tip, d, kWhyTool + (UINT_PTR)ctrlId, L"");
	return tip;
}

// Say on a control why it is disabled. With nullptr it says nothing.
static void setWhy(HWND d, int ctrlId, const wchar_t *why) {
	const OptionsState *os = optionsState(d);
	if (!os || !os->tip)
		return;
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.hwnd        = d;
	ti.uId         = kWhyTool + (UINT_PTR)ctrlId;
	ti.lpszText    = (LPWSTR)(why ? why : L"");
	SendMessageW(os->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);
}

// Put every place of the tooltip where its control is now. In the dialog that is no further
// than the scrolling part shows: a control scrolled out of sight explains nothing.
static void placeTips(HWND d) {
	const OptionsState *os = optionsState(d);
	if (!os || !os->tip)
		return;
	auto shown = [os](RECT rc) {
		RECT part;
		if (!IntersectRect(&part, &rc, &os->view))
			SetRectEmpty(&part);
		return part;
	};
	for (const auto &help : kHelpTexts) {
		HWND ctrl  = GetDlgItem(d, help.ctrlId);
		bool label = isCheckbox(ctrl); // only the label of a checkbox explains, not its box
		RECT own;
		GetClientRect(ctrl, &own);
		if (label)
			own.left = checkboxLabelLeft(ctrl);
		moveTool(os->tip, ctrl, kLabelTool, own);
		moveTool(os->tip, d, (UINT_PTR)help.ctrlId, shown(partInDialog(d, ctrl, label ? Part::Label : Part::All)));
	}
	for (int ctrlId : kWhyControls) {
		HWND ctrl = GetDlgItem(d, ctrlId);
		moveTool(os->tip, d, kWhyTool + (UINT_PTR)ctrlId, shown(partInDialog(d, ctrl, isCheckbox(ctrl) ? Part::Box : Part::All)));
	}
}

// The cursor that says what hovering does at a place in the dialog, or nullptr for the usual one:
//  - the help cursor over a label that has an explanation, disabled or not, and over the label
//    of a checkbox that has one;
//  - the "unavailable" cursor over a control that is disabled, and over the box of a disabled
//    checkbox. A label that is greyed with its control is not something to be unavailable.
static const wchar_t *cursorAt(HWND d, POINT at) {
	const OptionsState *os = optionsState(d);
	if (!os || !PtInRect(&os->view, at))
		return nullptr; // not over the scrolling part
	HWND ctrl = ChildWindowFromPointEx(d, at, CWP_SKIPINVISIBLE);
	if (!ctrl || ctrl == d)
		return nullptr;
	bool help = hasHelpText(GetDlgCtrlID(ctrl));
	if (isClass(ctrl, WC_STATICW))
		return help ? IDC_HELP : nullptr;
	if (help && isCheckbox(ctrl) && at.x >= partInDialog(d, ctrl, Part::Label).left)
		return IDC_HELP;
	return IsWindowEnabled(ctrl) ? nullptr : IDC_NO;
}

/* ---------- the scrolling part of the dialog ---------- */
// The dialog can be made lower than what it has to show, and opens lower on a screen that is
// not high enough for it. Everything except the row of buttons at the bottom then scrolls.
// The controls stay direct children of the dialog, so that Windows goes on giving them the font
// of a monitor with another scaling. Scrolling moves them, and none shows, or takes the mouse,
// outside the scrolling part (see layOutPage).
// Made wider, the dialog gives the room to the controls that can use it.
static const int kPageButtons[] = {IDC_RESET_SHORTCUTS, IDOK, IDCANCEL}; // the first keeps left, the others right
// Following the right edge of the dialog: with its own right edge, so that it stretches...
static const int kPageStretch[] = {IDC_DISPLAY_COMBO, IDC_PRESET_COMBO, IDC_PRESET_NOTE, IDC_BRIGHTNESS_SLIDER,
                                   IDC_AMBIENT_VALUE, IDC_AUTO_STATUS,  IDC_HOTKEY_UP,   IDC_HOTKEY_DOWN};
// ...or as a whole: what stands to the right of the slider.
static const int kPageMove[] = {IDC_SLIDER_BRIGHT_LABEL, IDC_SLIDER_PERCENT, IDC_SLIDER_PERCENT_SPIN, IDC_SLIDER_PERCENT_UNIT};
constexpr UINT_PTR kPageCtrlSubclass = 2;

template <size_t N> static bool among(const int (&ids)[N], int id) {
	return std::find(ids, ids + N, id) != ids + N;
}
static int dialogUnitsY(HWND d, int units) {
	RECT rc = {0, 0, 0, units};
	MapDialogRect(d, &rc);
	return rc.bottom;
}

// Note where the layout has put everything. Called with the dialog as the template and
// layOutRows leave it: not scrolled, and with its controls where they are at the design width.
static void capturePage(HWND d) {
	OptionsState *os = optionsState(d);
	if (!os)
		return;
	RECT design = {0, 0, OPTIONS_DLG_W, OPTIONS_DLG_H};
	MapDialogRect(d, &design);
	os->design = {design.right, design.bottom};
	os->units  = dialogUnits(d);
	os->scroll = 0;
	os->page.clear();

	LONG top = design.bottom;
	for (int i = 0; i < 3; ++i) {
		os->buttons[i] = rectInDialog(d, GetDlgItem(d, kPageButtons[i]));
		top            = std::min(top, os->buttons[i].top);
	}
	int margin  = dialogUnitsY(d, 4);
	os->pageEnd = (int)top - margin;

	LONG bottom = 0;
	for (HWND c = GetWindow(d, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
		int id = GetDlgCtrlID(c);
		if (id == IDC_OPTIONS_SCROLL || id == IDC_OPTIONS_EDGE || among(kPageButtons, id))
			continue;
		PageItem item = {c, rectInDialog(d, c), 0, among(kPageStretch, id) ? 1 : among(kPageMove, id) ? 2 : 0, false};
		if (isClass(c, WC_COMBOBOXW)) {
			RECT open = {};
			SendMessageW(c, CB_GETDROPPEDCONTROLRECT, 0, (LPARAM)&open);
			item.open = open.bottom - open.top;
		}
		bottom = std::max(bottom, item.at.bottom);
		os->page.push_back(item);
	}
	// Some room under the last row, but never so much that the dialog as designed would scroll.
	os->content = std::max((int)bottom, std::min((int)bottom + margin, os->pageEnd));
}

// Put everything where the size of the dialog and the scrolling have it.
static void layOutPage(HWND d) {
	OptionsState *os = optionsState(d);
	if (!os || os->page.empty())
		return;
	RECT client;
	GetClientRect(d, &client);
	int  strip = os->design.cy - os->pageEnd; // the row of buttons keeps its height
	int  viewH = std::max(0, (int)client.bottom - strip);
	bool bar   = os->content > viewH;
	int  barW  = bar ? GetSystemMetricsForDpi(SM_CXVSCROLL, (UINT)DpiOfWindow(d)) : 0;
	int  viewW = std::max(0, (int)client.right - barW);
	int  wider = viewW - (int)os->design.cx; // under 0 only by the scroll bar, in a dialog not made wider
	os->scroll = std::clamp(os->scroll, 0, std::max(0, os->content - viewH));
	os->view   = {0, 0, viewW, viewH};

	// A control shows, and takes the mouse, only where the scrolling part does. One that is partly
	// out of it gets a window region for the part that is in it. One that is out of it altogether
	// is put out of the dialog: it stays a control the keyboard can get to, which a hidden one
	// would not be, and getting the keyboard brings it back into view.
	const UINT flags  = SWP_NOZORDER | SWP_NOACTIVATE;
	const int  parked = -30000;
	HWND       bars   = GetDlgItem(d, IDC_OPTIONS_SCROLL), edge = GetDlgItem(d, IDC_OPTIONS_EDGE);
	std::vector<HRGN> parts(os->page.size(), nullptr); // nullptr: all of the control
	HDWP              batch = BeginDeferWindowPos((int)os->page.size() + 5);
	for (size_t i = 0; i < os->page.size(); ++i) {
		const PageItem &item = os->page[i];
		RECT            rc   = item.at, part;
		if (item.follow == 1)
			rc.right = std::max(rc.left, rc.right + wider);
		else if (item.follow == 2)
			OffsetRect(&rc, wider, 0);
		OffsetRect(&rc, 0, -os->scroll);
		if (!IntersectRect(&part, &rc, &os->view)) {
			OffsetRect(&rc, 0, parked - rc.top);
		} else if (!EqualRect(&part, &rc)) {
			OffsetRect(&part, -rc.left, -rc.top);
			parts[i] = CreateRectRgnIndirect(&part);
		}
		if (batch)
			batch = DeferWindowPos(batch, item.ctrl, nullptr, rc.left, rc.top, rc.right - rc.left,
			                       item.open ? item.open : rc.bottom - rc.top, flags);
	}
	for (int i = 0; i < 3 && batch; ++i) {
		RECT rc = os->buttons[i];
		OffsetRect(&rc, i ? client.right - os->design.cx : 0, client.bottom - os->design.cy);
		batch = DeferWindowPos(batch, GetDlgItem(d, kPageButtons[i]), nullptr, rc.left, rc.top, rc.right - rc.left,
		                       rc.bottom - rc.top, flags);
	}
	if (batch)
		batch = DeferWindowPos(batch, bars, nullptr, viewW, 0, barW, viewH, flags);
	if (batch)
		batch = DeferWindowPos(batch, edge, nullptr, 0, viewH, client.right, GetSystemMetrics(SM_CYEDGE), flags);
	if (batch)
		EndDeferWindowPos(batch);

	for (size_t i = 0; i < os->page.size(); ++i) {
		PageItem &item = os->page[i];
		if (!parts[i] && !item.clipped)
			continue;
		SetWindowRgn(item.ctrl, parts[i], TRUE); // the region is the system's from here on
		item.clipped = parts[i] != nullptr;
	}

	// The scroll bar, and a line over the buttons, only while there is something to scroll.
	SCROLLINFO si = {sizeof(si), SIF_RANGE | SIF_PAGE | SIF_POS};
	si.nMax       = std::max(0, os->content - 1);
	si.nPage      = (UINT)viewH;
	si.nPos       = os->scroll;
	SetScrollInfo(bars, SB_CTL, &si, TRUE);
	ShowWindow(bars, bar ? SW_SHOWNA : SW_HIDE);
	ShowWindow(edge, bar ? SW_SHOWNA : SW_HIDE);

	// What a control no longer covers is the dialog's to paint again, and that includes what a
	// region has just cut off: paint all of it rather than work out which parts.
	RedrawWindow(d, nullptr, nullptr, RDW_INVALIDATE | RDW_ERASE | RDW_ALLCHILDREN);
	placeTips(d);
}

static void scrollPageTo(HWND d, int to) {
	OptionsState *os = optionsState(d);
	if (!os)
		return;
	to = std::clamp(to, 0, std::max(0, os->content - (int)os->view.bottom));
	if (to == os->scroll)
		return;
	os->scroll = to;
	if (os->tip)
		SendMessageW(os->tip, TTM_POP, 0, 0); // it is about a place that is moving away
	layOutPage(d);
}

// The scroll bar: a WM_VSCROLL request.
static void scrollPageBy(HWND d, WPARAM request) {
	const OptionsState *os = optionsState(d);
	if (!os)
		return;
	int line = dialogUnitsY(d, 10), page = std::max(line, (int)os->view.bottom - line), to = os->scroll;
	switch (LOWORD(request)) {
	case SB_LINEUP:   to -= line; break;
	case SB_LINEDOWN: to += line; break;
	case SB_PAGEUP:   to -= page; break;
	case SB_PAGEDOWN: to += page; break;
	case SB_TOP:      to = 0; break;
	case SB_BOTTOM:   to = os->content; break;
	case SB_THUMBTRACK:
	case SB_THUMBPOSITION: {
		SCROLLINFO si = {sizeof(si), SIF_TRACKPOS};
		GetScrollInfo(GetDlgItem(d, IDC_OPTIONS_SCROLL), SB_CTL, &si);
		to = si.nTrackPos;
		break;
	}
	default:
		return;
	}
	scrollPageTo(d, to);
}

// Scroll so that a control is in view, with a little room around it.
static void showInPage(HWND d, HWND ctrl) {
	const OptionsState *os = optionsState(d);
	if (!os)
		return;
	for (const PageItem &item : os->page) {
		if (item.ctrl != ctrl)
			continue;
		int margin = dialogUnitsY(d, 6);
		if (item.at.top - margin < os->scroll)
			scrollPageTo(d, item.at.top - margin);
		else if (item.at.bottom + margin > os->scroll + os->view.bottom)
			scrollPageTo(d, item.at.bottom + margin - os->view.bottom);
		return;
	}
}

// How small and how high the client area of the dialog may be: no narrower than designed, no
// lower than a few rows over the buttons, and no higher than everything there is to show.
struct SizeLimits {
	int minWidth, minHeight, maxHeight;
};
static SizeLimits sizeLimits(HWND d, const OptionsState &os) {
	int strip = os.design.cy - os.pageEnd;
	return {(int)os.design.cx, std::min(os.content, dialogUnitsY(d, 120)) + strip, std::max(os.content, os.pageEnd) + strip};
}

// The client size to keep is held in 64ths of a dialog unit: it has to come out as the same
// pixels again at the same scaling, and as the same share of the dialog at another.
constexpr int kSizeUnit = 64;
static SIZE   sizeToKeep(HWND d, SIZE pixels) {
	SIZE units = dialogUnits(d);
	return {MulDiv(pixels.cx, 4 * kSizeUnit, units.cx), MulDiv(pixels.cy, 8 * kSizeUnit, units.cy)};
}
static SIZE sizeKept(HWND d, SIZE kept) {
	SIZE units = dialogUnits(d);
	return {MulDiv(kept.cx, units.cx, 4 * kSizeUnit), MulDiv(kept.cy, units.cy, 8 * kSizeUnit)};
}

// Give the dialog the size it is to have: the one the user gave it, or its full size until then,
// but no higher than the monitor it is on has room for. What the monitor takes off is not
// forgotten: on a monitor with the room the dialog has its height back.
// As it opens, the dialog is also put on that monitor whole. Later it stays where the user has
// put it.
static void sizeDialog(HWND d, bool opening) {
	OptionsState *os = optionsState(d);
	RECT          window, client;
	MONITORINFO   mi = {sizeof(mi)};
	if (!os || os->page.empty() || os->rescaling || !GetWindowRect(d, &window) || !GetClientRect(d, &client))
		return;
	if (!GetMonitorInfoW(MonitorFromWindow(d, MONITOR_DEFAULTTONEAREST), &mi))
		return;
	const RECT      &work   = mi.rcWork;
	const SizeLimits limits = sizeLimits(d, *os);
	const SIZE       kept   = sizeKept(d, os->wanted);
	int frameW = (window.right - window.left) - client.right, frameH = (window.bottom - window.top) - client.bottom;
	int width  = std::max((int)kept.cx, limits.minWidth) + frameW;
	int height = std::clamp((int)kept.cy, limits.minHeight, limits.maxHeight) + frameH;
	height     = std::min(height, (int)(work.bottom - work.top));
	int x      = std::max((int)work.left, std::min((int)window.left, (int)work.right - width));
	int y      = std::max((int)work.top, std::min((int)window.top, (int)work.bottom - height));
	os->sizing = true;
	SetWindowPos(d, nullptr, x, y, width, height, SWP_NOZORDER | SWP_NOACTIVATE | (opening ? 0 : SWP_NOMOVE));
	os->sizing = false;
}

// The dialog is on a monitor with another scaling, and Windows has given it and its controls the
// font for it: lay it out as a dialog opened there is, and keep what the user has done to it,
// its size and how far it is scrolled.
static void rescaleOptions(HWND d) {
	OptionsState *os = optionsState(d);
	if (!os)
		return;
	// How far it is scrolled is kept as the size is, in units that come out the same again:
	// unless the user has scrolled since, the place is the one kept the last time.
	if (os->scroll != os->scrollThen)
		os->scrollKept = MulDiv(os->scroll, 8 * kSizeUnit, std::max(os->units.cy, 1L));
	placeAsOpened(d);
	layOutRows(d);
	capturePage(d);
	os->scroll     = sizeKept(d, {0, os->scrollKept}).cy; // layOutPage sees that it is not too far
	os->scrollThen = os->scroll;
	os->rescaling  = false;
	// While the dialog is being dragged its size is the one Windows keeps for the drag, and any
	// other is undone at the next move of the mouse: its size waits for the end of the drag.
	if (!os->dragged)
		sizeDialog(d, false);
	layOutPage(d);
}

// Every control of the scrolling part is watched for two things.
static LRESULT CALLBACK PageCtrlProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR) {
	HWND d = GetParent(h);
	switch (m) {
	case WM_MOUSEWHEEL: {
		// While the dialog scrolls, the wheel scrolls it from a control that has no use for the
		// wheel. An open list is another window and keeps its wheel. A closed list gives it to
		// the dialog: picking a display acts at once, and a wheel meant for the dialog should not
		// do that on the way.
		OptionsState *os   = optionsState(d);
		bool          open = isClass(h, WC_COMBOBOXW) && SendMessageW(h, CB_GETDROPPEDSTATE, 0, 0);
		if (!os || os->content <= os->view.bottom || open)
			break;
		if (isClass(h, TRACKBAR_CLASSW) || isClass(h, WC_EDITW) || isClass(h, UPDOWN_CLASSW)) {
			// The slider and the number fields, with their arrows, do have a use for the wheel,
			// and keep it: it works on them as it does while nothing scrolls. Whatever such a
			// control leaves of the message goes up to its parent, and the dialog must not
			// scroll on top of what the control has done with it.
			os->wheelKept = true;
			LRESULT res   = DefSubclassProc(h, m, w, l);
			os->wheelKept = false;
			return res;
		}
		return SendMessageW(d, WM_MOUSEWHEEL, w, l);
	}
	case WM_SETFOCUS:
		PostMessageW(d, WMAPP_OPTIONS_SHOW, (WPARAM)h, 0); // the keyboard has got here: have it in view
		break;
	case WM_NCDESTROY:
		RemoveWindowSubclass(h, PageCtrlProc, id);
		break;
	}
	return DefSubclassProc(h, m, w, l);
}

/* ---------- preset descriptions as tooltips on the dropdown items ---------- */
// The display supplies a description with a preset (the text that also goes to the log). It is
// shown next to the open dropdown for the item the mouse is over or the arrow keys have moved
// to. A combo box does not report which item is hovered, so its list window is subclassed; this
// state lives and dies with it.
struct PresetTips {
	HWND                      tip = nullptr;
	std::vector<std::wstring> descs;                        // by position in the dropdown; empty = no description
	int                       shown     = -1;               // position the tooltip is up for, -1 = hidden
	POINT                     lastMouse = {LONG_MIN, LONG_MIN}; // screen position of the last real mouse move
};
constexpr UINT_PTR kPresetTipsSubclass = 1;

static void hidePresetTip(HWND list, PresetTips *pt) {
	if (pt->shown < 0)
		return;
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.hwnd        = list;
	SendMessageW(pt->tip, TTM_TRACKACTIVATE, FALSE, (LPARAM)&ti);
	pt->shown = -1;
}

static void showPresetTip(HWND list, PresetTips *pt, int item) {
	if (item == pt->shown)
		return;
	hidePresetTip(list, pt);
	if (item < 0 || item >= (int)pt->descs.size() || pt->descs[item].empty())
		return;

	TTTOOLINFOW ti = {sizeof(ti)};
	ti.hwnd        = list;
	ti.lpszText    = pt->descs[item].data();
	SendMessageW(pt->tip, TTM_UPDATETIPTEXTW, 0, (LPARAM)&ti);

	// Beside the list and level with the item, so that it covers none of the other items.
	RECT lr, ir, tr;
	GetWindowRect(list, &lr);
	SendMessageW(list, LB_GETITEMRECT, item, (LPARAM)&ir);
	MapWindowPoints(list, nullptr, (POINT *)&ir, 2);
	int gap = 2 * GetSystemMetrics(SM_CXEDGE);
	SendMessageW(pt->tip, TTM_TRACKPOSITION, 0, MAKELPARAM(lr.right + gap, ir.top));
	SendMessageW(pt->tip, TTM_TRACKACTIVATE, TRUE, (LPARAM)&ti);
	pt->shown = item;

	// Only now does it have the size of this text. If the monitor has no room for it on the
	// right, move it to the left of the list, and failing that under the list.
	GetWindowRect(pt->tip, &tr);
	int         width = tr.right - tr.left;
	MONITORINFO mi    = {sizeof(mi)};
	GetMonitorInfoW(MonitorFromWindow(list, MONITOR_DEFAULTTONEAREST), &mi);
	if (lr.right + gap + width > mi.rcWork.right) {
		int x = lr.left - gap - width, y = ir.top;
		if (x < mi.rcWork.left) {
			x = lr.left;
			y = lr.bottom + gap;
		}
		SendMessageW(pt->tip, TTM_TRACKPOSITION, 0, MAKELPARAM(x, y));
	}
}

static LRESULT CALLBACK PresetListProc(HWND h, UINT m, WPARAM w, LPARAM l, UINT_PTR id, DWORD_PTR ref) {
	auto *pt = (PresetTips *)ref;
	switch (m) {
	case WM_MOUSEMOVE: {
		// Windows also sends this when a window appears or goes under a mouse that has not
		// moved, the tooltip included. Only a real move may take over from the arrow keys.
		POINT p{(short)LOWORD(l), (short)HIWORD(l)}, at = p;
		ClientToScreen(h, &at);
		if (at.x == pt->lastMouse.x && at.y == pt->lastMouse.y)
			break;
		pt->lastMouse = at;

		// The open list holds the mouse capture, so moves outside it arrive here as well. And
		// LB_ITEMFROMPOINT answers with the nearest item, so check the point is really on it.
		RECT rc;
		GetClientRect(h, &rc);
		int item = -1;
		if (PtInRect(&rc, p)) {
			int hit = LOWORD(SendMessageW(h, LB_ITEMFROMPOINT, 0, l));
			if (SendMessageW(h, LB_GETITEMRECT, hit, (LPARAM)&rc) != LB_ERR && PtInRect(&rc, p))
				item = hit;
		}
		showPresetTip(h, pt, item);
		break;
	}
	case WM_MOUSEWHEEL:
	case WM_VSCROLL:
		hidePresetTip(h, pt); // the items move under the mouse; the next move picks it up again
		break;
	case WM_SHOWWINDOW:
	case WM_WINDOWPOSCHANGED: {
		// The dropdown opening or closing, by either route.
		UINT flags = m == WM_WINDOWPOSCHANGED ? ((const WINDOWPOS *)l)->flags : (w ? SWP_SHOWWINDOW : SWP_HIDEWINDOW);
		if (flags & SWP_HIDEWINDOW)
			hidePresetTip(h, pt);
		if (flags & SWP_SHOWWINDOW)
			GetCursorPos(&pt->lastMouse); // a mouse that just sits where the list opens is not hovering
		break;
	}
	case WM_NCDESTROY:
		RemoveWindowSubclass(h, PresetListProc, id);
		delete pt;
		break;
	}
	return DefSubclassProc(h, m, w, l);
}

// Keyboard: while the dropdown is open, every arrow-key move comes as CBN_SELCHANGE.
static void showPresetTipForSelection(HWND combo) {
	COMBOBOXINFO cbi = {sizeof(cbi)};
	DWORD_PTR    ref = 0;
	if (!SendMessageW(combo, CB_GETDROPPEDSTATE, 0, 0) || !GetComboBoxInfo(combo, &cbi) || !cbi.hwndList ||
	    !GetWindowSubclass(cbi.hwndList, PresetListProc, kPresetTipsSubclass, &ref))
		return;
	showPresetTip(cbi.hwndList, (PresetTips *)ref, (int)SendMessageW(combo, CB_GETCURSEL, 0, 0));
}

// Give the preset dropdown the descriptions of the list it has just been filled with, one entry
// per dropdown position. The first list that has a description sets the tooltip up.
static void setPresetTips(HWND d, HWND combo, std::vector<std::wstring> descs) {
	COMBOBOXINFO cbi = {sizeof(cbi)};
	if (!GetComboBoxInfo(combo, &cbi) || !cbi.hwndList)
		return;
	DWORD_PTR ref = 0;
	if (GetWindowSubclass(cbi.hwndList, PresetListProc, kPresetTipsSubclass, &ref)) {
		auto *pt = (PresetTips *)ref;
		hidePresetTip(cbi.hwndList, pt);
		pt->descs = std::move(descs);
		return;
	}
	if (std::all_of(descs.begin(), descs.end(), [](const std::wstring &s) { return s.empty(); }))
		return; // this display describes none of its presets
	HWND tip = createWrappedTooltip(d);
	if (!tip)
		return;
	TTTOOLINFOW ti = {sizeof(ti)};
	ti.uFlags      = TTF_TRACK | TTF_ABSOLUTE; // shown and placed by showPresetTip
	ti.hwnd        = cbi.hwndList;
	ti.lpszText    = (LPWSTR)L"";
	SendMessageW(tip, TTM_ADDTOOLW, 0, (LPARAM)&ti);

	auto *pt = new PresetTips{tip, std::move(descs)};
	if (!SetWindowSubclass(cbi.hwndList, PresetListProc, kPresetTipsSubclass, (DWORD_PTR)pt))
		delete pt;
}

/* ---------- the display selector and what follows from the display ---------- */
// Fill the display list, the preset list and the notes that go with them.
static void fillDisplayControls(HWND d, const DisplaysView &v) {
	bool have = v.active >= 0;

	// The displays, in the order of the tray menu. Whether the list can be used is up to
	// showDisplayChoice.
	HWND displays = GetDlgItem(d, IDC_DISPLAY_COMBO);
	SendMessageW(displays, CB_RESETCONTENT, 0, 0);
	for (const auto &name : v.names)
		SendMessageW(displays, CB_ADDSTRING, 0, (LPARAM)name.c_str());
	if (!have)
		SendMessageW(displays, CB_ADDSTRING, 0, (LPARAM)L"(no display detected)");
	SendMessageW(displays, CB_SETCURSEL, have ? v.active : 0, 0);

	// Presets of the active display. Under HDR only the main "Apple XDR Display" presets
	// stay listed (the only ones compatible with HDR): switching between those is valid,
	// anything else can blank the panel.
	std::vector<Preset> shown;
	for (const auto &p : v.presets)
		if (!v.hdr || p.isHdrCompatible())
			shown.push_back(p);
	HWND                      combo = GetDlgItem(d, IDC_PRESET_COMBO);
	std::vector<std::wstring> descs(shown.size()); // by dropdown position, for the tooltips
	SendMessageW(combo, CB_RESETCONTENT, 0, 0);
	if (!shown.empty()) {
		int sel = -1;
		for (size_t i = 0; i < shown.size(); ++i) {
			int pos = (int)SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)shown[i].name.c_str());
			SendMessageW(combo, CB_SETITEMDATA, pos, (LPARAM)shown[i].index);
			if ((int)shown[i].index == v.activePreset)
				sel = pos;
			if (pos >= 0 && pos < (int)descs.size())
				descs[pos] = shown[i].desc;
		}
		SendMessageW(combo, CB_SETCURSEL, sel, 0); // -1 = no selection (active preset filtered out)
	} else {
		SendMessageW(combo, CB_ADDSTRING, 0, (LPARAM)L"(no presets)");
		SendMessageW(combo, CB_SETCURSEL, 0, 0);
	}
	enableDlgItem(d, IDC_PRESET_COMBO, !shown.empty());
	setPresetTips(d, combo, std::move(descs));

	// HDR: what it does to the list and, as an experimental escape hatch, a button that asks
	// Windows to turn it off from here (it may refuse).
	const wchar_t *note = !v.hdr        ? L""
	                    : !have         ? L"HDR is on"
	                    : shown.empty() ? L"Disable HDR to unlock presets and brightness controls"
	                                    : L"HDR is on: only the Apple XDR presets are available";
	SetDlgItemTextW(d, IDC_PRESET_NOTE, note);
	HWND hdrOff = GetDlgItem(d, IDC_HDR_OFF_BTN);
	if (!v.hdr && GetFocus() == hdrOff)
		SendMessageW(d, WM_NEXTDLGCTL, 0, FALSE); // the button is about to go
	ShowWindow(hdrOff, v.hdr ? SW_SHOW : SW_HIDE);
	EnableWindow(hdrOff, TRUE); // a click leaves it disabled until HDR is off

	// Why a control is disabled, as a tooltip on it, where there is a text for the reason. HDR
	// and a preset that fixes the brightness lock the brightness controls; HDR that leaves no
	// preset to offer locks the preset list.
	const wchar_t *why = v.hdr    ? L"Brightness is controlled by Windows while HDR is on"
	                   : v.locked ? L"Brightness is fixed by the current preset"
	                              : nullptr;
	for (int ctrlId : {IDC_AUTO_BRIGHTNESS, IDC_BRIGHTNESS_SLIDER, IDC_SLIDER_PERCENT, IDC_SLIDER_PERCENT_SPIN})
		setWhy(d, ctrlId, why);
	setWhy(d, IDC_PRESET_COMBO, v.hdr && have && shown.empty() ? L"Unavailable while HDR is on" : nullptr);
}

// The display list and "Sync manual brightness across displays". The list is disabled only
// when it has no display in it: one display is still something to open the list and see. The
// checkbox takes two displays, as with one there is nothing to keep in sync. The tray menu has
// the same switch and can turn it while the dialog is open.
static void showDisplayChoice(HWND d, const OptionsState &os) {
	checkDlgItem(d, IDC_LINK_DISPLAYS, g_settings.linkedMode);
	enableDlgItem(d, IDC_DISPLAY_COMBO, !os.shown.names.empty());
	enableDlgItem(d, IDC_LINK_DISPLAYS, os.shown.names.size() > 1);
}

// Bring the dialog in line with the displays: first the lists that follow from which displays
// there are and which one is active, then the brightness controls. Runs when the dialog opens,
// then on a timer, because auto-brightness, the brightness keys, HDR, the tray menu and the
// displays themselves all carry on meanwhile, and after a change in the dialog that the rest of
// it depends on.
static void syncOptions(HWND d, bool wait, bool opening) {
	OptionsState *os = optionsState(d);
	DisplaysView  v;
	SliderState   st;
	if (!os || !readDisplays(v, st, wait))
		return;

	// Fill the lists again only when what they show has changed: a fill drops a preset that was
	// picked but not saved yet.
	bool fill = !os->filled || !os->shown.sameAs(v);
	if (fill) {
		fillDisplayControls(d, v);
		os->shown  = std::move(v);
		os->filled = true;
	}
	showDisplayChoice(d, *os);
	showBrightnessState(d, st, opening || fill); // after a fill the slider starts from that display's level
}

INT_PTR CALLBACK OptionsDlgProc(HWND d, UINT msg, WPARAM wp, LPARAM lp) {
	switch (msg) {
	case WM_INITDIALOG: {
		SetWindowLongPtrW(d, DWLP_USER, (LONG_PTR) new OptionsState);
		CheckDlgButton(d, IDC_AUTO_ROTATE, g_settings.autoRotateEnabled.load() ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_SHOW_OSD, g_settings.showOSD ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_RUN_AT_STARTUP, g_settings.runAtStartup ? BST_CHECKED : BST_UNCHECKED);
		CheckDlgButton(d, IDC_ENABLE_HOTKEYS, g_settings.enableCustomHotkeys ? BST_CHECKED : BST_UNCHECKED);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), g_settings.enableCustomHotkeys);
		EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), g_settings.enableCustomHotkeys);
		setDlgHotkey(d, IDC_HOTKEY_UP, g_settings.hkUp);
		setDlgHotkey(d, IDC_HOTKEY_DOWN, g_settings.hkDown);
		noteAsOpened(d);
		layOutRows(d);
		capturePage(d); // before the tooltips, which go by where the controls are
		if (OptionsState *os = optionsState(d)) {
			RECT client;
			GetClientRect(d, &client);
			os->client = {client.right, client.bottom};
			os->wanted = {OPTIONS_DLG_W * kSizeUnit, OPTIONS_DLG_H * kSizeUnit}; // the full size
			os->tip = addHelpTexts(d);
			for (const PageItem &item : os->page)
				SetWindowSubclass(item.ctrl, PageCtrlProc, kPageCtrlSubclass, 0);
		}
		initNumberFields(d);

		// The brightness slider: the control Boot Camp Control Panel uses, 0 to 100, with Dim and
		// Bright at its sides. Boot Camp's has a tick mark at each end; here the only marks are
		// the ones automatic brightness gives it (see drawSliderMarks).
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_SLIDER, TBM_SETRANGEMIN, FALSE, 0);
		SendDlgItemMessageW(d, IDC_BRIGHTNESS_SLIDER, TBM_SETRANGEMAX, FALSE, 100);
		s_percentTyped = false;
		SendDlgItemMessageW(d, IDC_SLIDER_PERCENT, EM_SETLIMITTEXT, 3, 0);
		SendDlgItemMessageW(d, IDC_SLIDER_PERCENT_SPIN, UDM_SETRANGE32, 0, 100);

		// The displays, the presets of the active one, and where its brightness is.
		syncOptions(d, true, true);
		sizeDialog(d, true);
		layOutPage(d);
		SetTimer(d, ID_OPTIONS_TIMER, 200, nullptr);
		return TRUE;
	}
	case WM_GETDPISCALEDSIZE:
	case WM_DPICHANGED:
		// Moved to a monitor with another scaling. Windows gives the dialog and its controls the
		// font for it and resizes the dialog, and does so between these two messages: the first
		// comes here before any of it, the second after all of it. Until the dialog is laid out
		// for the new scaling, what is known of its layout is for the old one: it must neither be
		// laid out from when the size changes, nor limit that size. The layout is done from a
		// posted message, which comes when Windows is through with its part.
		if (OptionsState *os = optionsState(d); os && !os->rescaling) {
			os->rescaling = true;
			PostMessageW(d, WMAPP_OPTIONS_RESCALE, 0, 0);
		}
		break;
	case WMAPP_OPTIONS_RESCALE:
		rescaleOptions(d);
		return TRUE;
	case WM_ENTERSIZEMOVE:
		if (OptionsState *os = optionsState(d))
			os->dragged = true;
		break;
	case WM_EXITSIZEMOVE:
		// Put down, maybe on another monitor: there it may have to be lower, or have the room to
		// be as high as it was.
		if (OptionsState *os = optionsState(d)) {
			os->dragged = false;
			sizeDialog(d, false);
		}
		break;
	case WM_NOTIFY: {
		const NMHDR *nm = (const NMHDR *)lp;
		if (nm->idFrom != IDC_BRIGHTNESS_SLIDER || nm->code != NM_CUSTOMDRAW)
			break;
		SetWindowLongPtrW(d, DWLP_MSGRESULT, drawSliderMarks(d, (const NMCUSTOMDRAW *)lp));
		return TRUE;
	}
	case WM_HSCROLL: {
		HWND slider = GetDlgItem(d, IDC_BRIGHTNESS_SLIDER);
		if ((HWND)lp != slider)
			break;
		applyOptionsPercent(d, (int)SendMessageW(slider, TBM_GETPOS, 0, 0)); // as the thumb moves
		return TRUE;
	}
	case WM_VSCROLL:
		if ((HWND)lp == GetDlgItem(d, IDC_OPTIONS_SCROLL)) {
			scrollPageBy(d, wp);
			return TRUE;
		}
		// The arrows of the percent field: no need to wait for a pause as with typing.
		if ((HWND)lp != GetDlgItem(d, IDC_SLIDER_PERCENT_SPIN))
			break;
		commitPercentField(d, false);
		return TRUE;
	case WM_TIMER: {
		if (wp == ID_TYPING_TIMER) {
			commitPercentField(d, false);
			return TRUE;
		}
		if (wp != ID_OPTIONS_TIMER)
			break;
		syncOptions(d, false, false);
		return TRUE;
	}
	case WM_SETCURSOR: {
		// The cursor says what hovering here does (see cursorAt). The place is found by where
		// the mouse is and not by who has the mouse: over a disabled control that is the dialog.
		POINT at;
		GetCursorPos(&at);
		ScreenToClient(d, &at);
		const wchar_t *cursor = LOWORD(lp) == HTCLIENT ? cursorAt(d, at) : nullptr;
		if (!cursor)
			break;
		SetCursor(LoadCursorW(nullptr, cursor));
		SetWindowLongPtrW(d, DWLP_MSGRESULT, TRUE);
		return TRUE;
	}
	case WM_SIZE:
		if (OptionsState *os = optionsState(d)) {
			SIZE now = {LOWORD(lp), HIWORD(lp)};
			if (!os->rescaling && !os->sizing && wp == SIZE_RESTORED) {
				// The user's doing: what has changed is the size to keep from now on.
				SIZE keep = sizeToKeep(d, now);
				if (now.cx != os->client.cx)
					os->wanted.cx = keep.cx;
				if (now.cy != os->client.cy)
					os->wanted.cy = keep.cy;
			}
			os->client = now;
			if (!os->rescaling)
				layOutPage(d);
		}
		break;
	case WM_GETMINMAXINFO: {
		// The limits are for the scaling the dialog is laid out for (see WM_DPICHANGED).
		const OptionsState *os = optionsState(d);
		if (!os || os->page.empty() || os->rescaling)
			break;
		RECT window, client;
		GetWindowRect(d, &window);
		GetClientRect(d, &client);
		int              frameW = (window.right - window.left) - client.right;
		int              frameH = (window.bottom - window.top) - client.bottom;
		const SizeLimits limits = sizeLimits(d, *os);
		auto            *sizes  = (MINMAXINFO *)lp;
		sizes->ptMinTrackSize   = {limits.minWidth + frameW, limits.minHeight + frameH};
		sizes->ptMaxTrackSize.y = limits.maxHeight + frameH;
		return TRUE;
	}
	case WM_MOUSEWHEEL: {
		OptionsState *os = optionsState(d);
		if (!os || os->content <= os->view.bottom)
			break; // nothing to scroll
		if (os->wheelKept)
			return TRUE; // passed up by a control that works with the wheel itself
		UINT lines = 3;
		SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &lines, 0);
		int notch = lines == WHEEL_PAGESCROLL ? (int)os->view.bottom : (int)lines * dialogUnitsY(d, 10);
		os->wheel += GET_WHEEL_DELTA_WPARAM(wp) * notch; // a touchpad sends a notch in small parts
		int by    = os->wheel / WHEEL_DELTA;
		os->wheel -= by * WHEEL_DELTA;
		scrollPageTo(d, os->scroll - by);
		return TRUE;
	}
	case WMAPP_OPTIONS_SHOW:
		showInPage(d, (HWND)wp);
		return TRUE;
	case WM_DESTROY:
		KillTimer(d, ID_OPTIONS_TIMER);
		KillTimer(d, ID_TYPING_TIMER);
		delete optionsState(d);
		SetWindowLongPtrW(d, DWLP_USER, 0);
		break;
	case WM_COMMAND: {
		WORD id   = LOWORD(wp);
		WORD code = HIWORD(wp);
		if (id == IDC_AUTO_BRIGHTNESS && code == BN_CLICKED) {
			// In force at once, like "Automatic brightness" in the tray menu. The status line and
			// the marks on the slider are about what automatic brightness is doing, and it would
			// not be doing it yet if this waited for Save.
			g_settings.autoAdjustEnabled.store(IsDlgButtonChecked(d, IDC_AUTO_BRIGHTNESS) == BST_CHECKED);
			g_settings.Save();
			syncOptions(d, true, false); // its fields, the status line, the marks on the slider
			return TRUE;
		}
		if (id == IDC_DISPLAY_COMBO && code == CBN_SELCHANGE) {
			// As in the tray menu, the display is the active one from now on, not from Save: the
			// slider below already acts on it.
			int sel = (int)SendMessageW((HWND)lp, CB_GETCURSEL, 0, 0);
			if (sel != CB_ERR) {
				commitPercentField(d, true); // a level still being typed belongs to the display it was typed for
				g_settings.activeDisplayIndex = (ULONG)sel;
				g_settings.Save();
				syncOptions(d, true, false); // this display's presets and brightness
			}
			return TRUE;
		}
		if (id == IDC_LINK_DISPLAYS && code == BN_CLICKED) {
			// At once, like the tray menu item of the same name: the slider above already acts on it.
			g_settings.linkedMode = IsDlgButtonChecked(d, IDC_LINK_DISPLAYS) == BST_CHECKED;
			g_settings.Save();
			return TRUE;
		}
		if (id == IDC_PRESET_COMBO && code == CBN_SELCHANGE) {
			showPresetTipForSelection((HWND)lp);
			return TRUE;
		}
		if (id == IDC_SLIDER_PERCENT && code == EN_CHANGE && !s_fillingPercent) {
			// Wait for a pause before applying: "50" should not flash through 5 % on the way.
			s_percentTyped = true;
			SetTimer(d, ID_TYPING_TIMER, 600, nullptr);
			return TRUE;
		}
		if (id == IDC_SLIDER_PERCENT && code == EN_KILLFOCUS) {
			commitPercentField(d, true);
			return TRUE;
		}
		if (code == EN_KILLFOCUS) {
			if (const NumberField *f = numberField(id)) {
				settleNumberField(d, *f);
				return TRUE;
			}
		}
		if (id == IDC_ENABLE_HOTKEYS && code == BN_CLICKED) {
			BOOL en = IsDlgButtonChecked(d, IDC_ENABLE_HOTKEYS) == BST_CHECKED;
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), en);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), en);
			return TRUE;
		}
		if (id == IDC_HDR_OFF_BTN && code == BN_CLICKED) {
			Log::Info(L"User asked to turn HDR off from Options");
			if (HdrTurnOffForAppleDisplays() > 0) {
				// The dialog follows once Windows has done it (see syncOptions).
				SetDlgItemTextW(d, IDC_PRESET_NOTE, L"HDR is turning off...");
				enableDlgItem(d, IDC_HDR_OFF_BTN, false);
			} else {
				MessageBoxW(d,
				            L"Windows did not accept the change. Please turn HDR off in Windows Settings > System > Display.",
				            L"Studio Brightness++", MB_ICONINFORMATION);
			}
			return TRUE;
		}
		if (id == IDC_RESET_SHORTCUTS && code == BN_CLICKED) {
			CheckDlgButton(d, IDC_ENABLE_HOTKEYS, BST_UNCHECKED);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_UP), FALSE);
			EnableWindow(GetDlgItem(d, IDC_HOTKEY_DOWN), FALSE);
			SendDlgItemMessageW(d, IDC_HOTKEY_UP, HKM_SETHOTKEY, 0, 0);
			SendDlgItemMessageW(d, IDC_HOTKEY_DOWN, HKM_SETHOTKEY, 0, 0);
			for (const auto &f : kNumberFields)
				SendDlgItemMessageW(d, f.spinId, UDM_SETPOS32, 0, f.initial);
			return TRUE;
		}
		if (id == IDOK) {
			commitPercentField(d, true); // Enter right after typing a level
			g_settings.autoRotateEnabled.store(IsDlgButtonChecked(d, IDC_AUTO_ROTATE) == BST_CHECKED);
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

			g_settings.brightnessSteps = readNumberField(d, IDC_BRIGHTNESS_STEPS);
			g_settings.rampBrightenMs.store(readNumberField(d, IDC_RAMP_BRIGHTEN));
			g_settings.rampDimMs.store(readNumberField(d, IDC_RAMP_DIM));
			ULONG brightenLimit = readNumberField(d, IDC_BRIGHTEN_LIMIT);
			ULONG dimLimit      = readNumberField(d, IDC_DIM_LIMIT);
			bool  limitsChanged = brightenLimit != g_settings.autoBrightenLimit.load() ||
			                      dimLimit != g_settings.autoDimLimit.load();
			g_settings.autoBrightenLimit.store(brightenLimit);
			g_settings.autoDimLimit.store(dimLimit);
			if (limitsChanged) {
				// Have automatic brightness work out its target again now, within the new limits,
				// rather than when the ambient light next changes enough.
				std::lock_guard<std::mutex> lock(g_displayMutex);
				for (auto &dev : g_displays)
					dev.lastTargetLux = 0.f;
			}
			g_settings.Save();
			g_settings.SetStartup(g_settings.runAtStartup);
			orient_set_enabled(g_settings.autoRotateEnabled.load());

			// Apply the chosen preset to the active display, then prompt to keep or auto-revert.
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
							// The list was filled from one display: its entries are not for another
							// one that has taken that display's place since.
							const OptionsState *os      = optionsState(d);
							bool                listed = os && os->shown.active >= 0 &&
							                             os->shown.paths[os->shown.active] == dev.devicePath;
							if (listed && dev.hPreset != INVALID_HANDLE_VALUE && hwIdx != dev.activePresetIndex) {
								prevIdx = dev.activePresetIndex;
								// Log the intent BEFORE the write (the file log flushes per line):
								// if this switch takes the GPU driver down, the log still shows
								// exactly what ran.
								const Preset *from  = dev.activePreset();
								const wchar_t     *toName = L"?";
								for (const auto &p : dev.presets)
									if ((int)p.index == hwIdx) { toName = p.name.c_str(); break; }
								Log::Info(L"Switching preset on %s: %d (%s) -> %d (%s), HDR=%d",
								          dev.name.c_str(), prevIdx, from ? from->name.c_str() : L"?",
								          hwIdx, toName, g_hdrActive.load() ? 1 : 0);
								if (dev.setActivePreset(hwIdx) == 0) {
									cid      = dev.containerId;
									switched = true;
								} else {
									Log::Warn(L"Preset switch failed on %s", dev.name.c_str());
								}
							}
						}
					}
					if (switched) {
						NvapiLogHdrState(L"post preset switch");
						PresetConfirm::Show(g_hInst, 10, [cid, prevIdx]() { RevertPresetByContainer(cid, prevIdx); });
					}
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
static bool activeDisplayPresetLocked() {
	std::lock_guard<std::mutex> lock(g_displayMutex);
	if (g_displays.empty()) return false;
	ULONG idx = std::min((ULONG)(g_displays.size() - 1), g_settings.activeDisplayIndex);
	return g_displays[idx].activePresetLocksBrightness();
}

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
// A panel takes a moment to light up after Windows turns it on, so reapply 3 s and 6 s later
// rather than once right away. A new event restarts the count.
static int g_reapplyPassesLeft = 0;
static void scheduleReapply(HWND h) {
	g_reapplyPassesLeft = 2;
	SetTimer(h, ID_REAPPLY_TIMER, 3000, nullptr);
}

LRESULT CALLBACK HiddenWndProc(HWND h, UINT m, WPARAM wParam, LPARAM lParam) {
	if (g_wmTaskbarCreated && m == g_wmTaskbarCreated) {
		AddNotificationIcon(h);
		return 0;
	}
	if (m == WM_DISPLAYCHANGE) {
		RefreshHdrState();
		scheduleReapply(h);
		return 0;
	}
	if (m == WM_POWERBROADCAST && wParam == PBT_POWERSETTINGCHANGE) {
		// Console display state: 0 off, 1 on, 2 dimmed. Windows sends the current state right
		// after registering, so only a change from off or dimmed to on counts.
		static DWORD lastState = 1;
		auto *ps = (const POWERBROADCAST_SETTING *)lParam;
		if (ps && IsEqualGUID(ps->PowerSetting, GUID_CONSOLE_DISPLAY_STATE) && ps->DataLength >= sizeof(DWORD)) {
			DWORD state = *(const DWORD *)ps->Data;
			if (state == 1 && lastState != 1)
				scheduleReapply(h);
			lastState = state;
		}
		return TRUE;
	}
	if (m == WM_TIMER && wParam == ID_REAPPLY_TIMER) {
		g_reapplyRequested.store(true);
		if (--g_reapplyPassesLeft <= 0)
			KillTimer(h, ID_REAPPLY_TIMER);
		return 0;
	}
	if (m == WM_DEVICECHANGE && wParam == DBT_DEVICEARRIVAL) {
		// Only Apple interfaces: a full scan logs every interface, so a mouse or a USB stick
		// should not trigger one. Removals are caught by the worker's liveness check.
		auto *hdr = (const DEV_BROADCAST_HDR *)lParam;
		if (hdr && hdr->dbch_devicetype == DBT_DEVTYP_DEVICEINTERFACE &&
		    StrStrIW(((const DEV_BROADCAST_DEVICEINTERFACE_W *)hdr)->dbcc_name, L"vid_05ac"))
			g_rescanRequested.store(true);
		return TRUE;
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
			SliderState st = readSliderState();
			if (st.hdr) {
				TrayPopup::Show(h, 0, nullptr, true, L"Brightness controlled by Windows (HDR)");
				return 0;
			}
			if (!st.hasDisplay) return 0;
			if (st.locked) { // reference-mode preset: brightness is fixed (macOS locks it there too)
				TrayPopup::Show(h, 0, nullptr, true, L"Brightness fixed by the preset");
				return 0;
			}

			TrayPopup::Show(h, st.percent(), [refMin = st.minBrightness, refMax = st.maxBrightness](int newPct) {
				applySliderPercent(newPct, refMin, refMax);
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
					            IDM_LINKED_MODE, L"Sync manual brightness across displays");
				}
			}

			AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
			UINT autoFlags = MF_STRING | (g_settings.autoAdjustEnabled.load() ? MF_CHECKED : 0u)
			                 | ((g_hdrActive.load() || activeDisplayPresetLocked())
			                        ? (UINT)(MF_GRAYED | MF_DISABLED) : 0u);
			AppendMenuW(hMenu, autoFlags, IDM_TOGGLE_AUTO, L"Automatic brightness");
			UINT rotFlags = MF_STRING | (g_settings.autoRotateEnabled.load() ? MF_CHECKED : 0u);
			AppendMenuW(hMenu, rotFlags, IDM_TOGGLE_ROTATE, L"Automatic rotate");
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
			} else if (cmd == IDM_TOGGLE_ROTATE) {
				g_settings.autoRotateEnabled.store(!g_settings.autoRotateEnabled.load());
				g_settings.Save();
				orient_set_enabled(g_settings.autoRotateEnabled.load());
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

/* ---------- sensor polling thread ---------- */
// Both watchers read with synchronous HID I/O: HidD_GetInputReport blocks, and the fallback
// waits up to 150 ms per device. On a WM_TIMER that stalls the message pump several times a
// second, and a panel waking from sleep can block a HID call for seconds. Polling goes here
// instead and publishes into the snapshots the UI and the worker read.
// Joined at shutdown, unlike the worker: the watchers free the handles it is still reading.
static std::thread g_sensorThread;
static HANDLE      g_sensorStop = nullptr;

static void startSensorThread() {
	g_sensorStop = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!g_sensorStop) {
		Log::Warn(L"Sensor thread not started: CreateEvent failed (%lu)", GetLastError());
		return;
	}
	g_sensorThread = std::thread([] {
		// Wait, not sleep, so shutdown does not cost a whole poll interval.
		while (WaitForSingleObject(g_sensorStop, 250) == WAIT_TIMEOUT) {
			orient_watch_tick();
			als_watch_tick();
		}
	});
}

static void stopSensorThread() {
	if (g_sensorStop) SetEvent(g_sensorStop);
	if (g_sensorThread.joinable()) g_sensorThread.join();
	if (g_sensorStop) { CloseHandle(g_sensorStop); g_sensorStop = nullptr; }
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

				// Re-scan for new devices only when needed (empty, device lost, or a device change)
				DWORD now = GetTickCount();
				bool needScan = g_displays.empty() || anyDead || g_rescanRequested.load();
				if (needScan && now - lastEnumerateTick >= kEnumerateCooldownMs) {
					lastEnumerateTick = now;
					g_rescanRequested.store(false); // before enumerating, so a change during it re-arms

					auto found = hid_enumerate();
					bool added = false;
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
							// mapLuxToBrightness divides by baseLux, and a sensor reads 0 in a dark room.
							// Unclamped that sends the target to infinity.
							newDev.baseLux = std::max(1.f, getAmbientLux(newDev));
						}
						Log::Info(L"Device %s ready [range %lu-%lu, current %lu]",
						          newDev.name.c_str(), newDev.minBrightness, newDev.maxBrightness,
						          newDev.currentBrightness);

						// Presets: enumerate ONCE per physical display (cached), reset to the default ONCE per run.
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
								// some XDR units on every launch. Log first (the file log flushes per
								// line) so a write that blanks the panel still shows in the log.
								if (!newDev.presets.empty() && newDev.activePresetIndex != 0) {
									Log::Info(L"Startup: resetting %s to its default preset (was %d)",
									          newDev.name.c_str(), newDev.activePresetIndex);
									if (newDev.setActivePreset(0) != 0)
										Log::Warn(L"Default preset reset failed on %s", newDev.name.c_str());
								}
							}
						}

						g_displays.push_back(std::move(newDev));
						added = true;
					}

					// At launch or after a reconnect, the panels may not show what they report.
					if (added)
						ReapplyBrightness(L"display found");
				}

				// Displays powered back on (sleep, monitor standby) or reconfigured: same reason.
				if (g_reapplyRequested.exchange(false))
					ReapplyBrightness(L"displays back on");
			}

			/* ---------- auto-brightness (Apple-style hysteresis + asymmetric perceptual ramp) ---------- */
			if (g_settings.autoAdjustEnabled.load()) {
				std::lock_guard<std::mutex> lock(g_displayMutex);
				for (auto &dev : g_displays) {
					if (dev.maxBrightness <= dev.minBrightness)
						continue; // brightness locked (e.g. a calibrated preset); nothing to adjust
					if (dev.activePresetLocksBrightness())
						continue; // reference mode active: brightness is fixed (macOS parity)
					float lux = getAmbientLux(dev); // per-device, ContainerId-matched sensor

					bool retarget = (dev.lastTargetLux <= 0.f) ||
					                (std::fabs(lux - dev.lastTargetLux) / dev.lastTargetLux >= kRelLuxHysteresis);
					if (retarget) {
						ULONG goal = mapLuxToBrightness(lux, dev);
						if (goal != dev.rampGoal || dev.rampDurationMs == 0.0) {
							ULONG rampMs = (goal >= dev.currentBrightness) ? g_settings.rampBrightenMs.load()
							                                               : g_settings.rampDimMs.load();
							dev.rampStart      = dev.currentBrightness;
							dev.rampGoal       = goal;
							dev.rampStartMs    = nowMs();
							// At least 1 ms: 0 means "no ramp running" here, and the goal would never be applied.
							dev.rampDurationMs = std::max(1.0, (double)rampMs);
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

	Log::ResumeIfPending();   // resume a file-log session that was still running before a restart
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

	// HID interface arrivals reach HiddenWndProc as DBT_DEVICEARRIVAL (re-scan for displays).
	DEV_BROADCAST_DEVICEINTERFACE_W hidFilter{};
	hidFilter.dbcc_size       = sizeof(hidFilter);
	hidFilter.dbcc_devicetype = DBT_DEVTYP_DEVICEINTERFACE;
	HidD_GetHidGuid(&hidFilter.dbcc_classguid);
	if (!RegisterDeviceNotificationW(h, &hidFilter, DEVICE_NOTIFY_WINDOW_HANDLE))
		Log::Warn(L"RegisterDeviceNotification failed (%lu)", GetLastError());
	// Displays turning back on (wake from sleep, monitor standby) reach HiddenWndProc as
	// WM_POWERBROADCAST, so the brightness can be sent again (#28).
	if (!RegisterPowerSettingNotification(h, &GUID_CONSOLE_DISPLAY_STATE, DEVICE_NOTIFY_WINDOW_HANDLE))
		Log::Warn(L"RegisterPowerSettingNotification failed (%lu)", GetLastError());

	if (!AddNotificationIcon(h)) {
		MessageBoxW(nullptr,
		            L"Failed to create the system tray icon.\nUse Task Manager to exit if needed.",
		            L"Studio Brightness ++", MB_ICONWARNING);
	}
	registerHotkeys(h);

	// Before startWorker: the worker anchors baseLux from the first reading it can get, so the
	// raw-HID ALS has to be up and carrying a real sample by then.
	orient_watch_init(g_settings.autoRotateEnabled.load()); // discover Apple orientation sensors (MI_09)
	als_watch_init();                           // discover Apple raw-HID ALS (MI_08 illuminance)
	startSensorThread();                        // poll both off the UI thread
	startWorker();

	// Check for updates shortly after launch, then once a day.
	SetTimer(h, ID_UPDATE_TIMER, 24 * 60 * 60 * 1000, nullptr);
	SetTimer(h, ID_HDR_TIMER, 2000, nullptr);   // poll HDR; also refreshed on WM_DISPLAYCHANGE
	RefreshHdrState();
	NvapiLogHdrState(L"startup");
	HdrLogAppleOutputs(L"startup");
	StartUpdateCheck(false);

	MSG msg;
	while (GetMessage(&msg, nullptr, 0, 0)) {
		TranslateMessage(&msg);
		DispatchMessage(&msg);
	}

	stopSensorThread();   // join first: the watchers below free the handles it reads
	orient_watch_shutdown();
	als_watch_shutdown();
	GdiplusShutdown(gdiplusToken);
	CoUninitialize();
	CloseHandle(hSingleInstance);
	return 0;
}
