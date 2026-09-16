//----------------  als.cpp  ----------------
#include "als.h"
#include "Log.h"

#define _WIN32_DCOM
#include <initguid.h>
#include <devpropdef.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <mutex>
#include <cstring>
#include <cmath>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shlwapi.lib")

// DEVPKEY_Device_ContainerId : {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2
DEFINE_DEVPROPKEY(ALS_DEVPKEY_Device_ContainerId,
                  0x8c7ed206, 0x3f8a, 0x4827,
                  0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);

static const wchar_t kAppleVid[] = L"vid_05ac";

static inline bool icontains(const wchar_t *hay, const wchar_t *needle) {
	return StrStrIW(hay, needle) != nullptr;
}

// "vid_05ac&pid_1116&mi_08" from a device interface path, for readable logs.
static std::wstring shortLabel(const wchar_t *path) {
	const wchar_t *p = StrStrIW(path, L"vid_");
	if (!p) p = path;
	std::wstring s;
	for (const wchar_t *q = p; *q && *q != L'#'; ++q) s += *q;
	return s;
}

/* ============================ AlsDevice ============================ */

void AlsDevice::moveFrom(AlsDevice &o) {
	hDev = o.hDev; prep = o.prep; inputLen = o.inputLen; reportId = o.reportId;
	uIllum = o.uIllum; uTemp = o.uTemp; uChromaX = o.uChromaX; uChromaY = o.uChromaY;
	eIllum = o.eIllum; eTemp = o.eTemp; eChromaX = o.eChromaX; eChromaY = o.eChromaY;
	overlapped = o.overlapped; containerId = o.containerId;
	devicePath = std::move(o.devicePath); label = std::move(o.label);
	lastRaw = o.lastRaw; lastLogTick = o.lastLogTick; failures = o.failures;
	o.hDev = INVALID_HANDLE_VALUE; o.prep = nullptr;
}

void AlsDevice::close() {
	if (prep) { HidD_FreePreparsedData(prep); prep = nullptr; }
	if (hDev != INVALID_HANDLE_VALUE) { CloseHandle(hDev); hDev = INVALID_HANDLE_VALUE; }
}

bool AlsDevice::readReport(std::vector<uint8_t> &buf) {
	if (hDev == INVALID_HANDLE_VALUE || inputLen == 0) return false;
	buf.assign(inputLen, 0);
	buf[0] = reportId;
	if (HidD_GetInputReport(hDev, buf.data(), (ULONG)buf.size()))
		return true;
	if (!overlapped) return false;
	std::vector<uint8_t> rb(inputLen, 0);
	OVERLAPPED ov{}; ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent) return false;
	bool ok = false; DWORD got = 0;
	BOOL r = ReadFile(hDev, rb.data(), inputLen, &got, &ov);
	if (r || GetLastError() == ERROR_IO_PENDING) {
		if (WaitForSingleObject(ov.hEvent, 150) == WAIT_OBJECT_0 &&
		    GetOverlappedResult(hDev, &ov, &got, FALSE) && got > 0) {
			buf.assign(rb.begin(), rb.end());
			ok = true;
		} else {
			// CancelIo does not wait. rb and ov are on this stack frame, so we have to block
			// until the I/O is really done before unwinding.
			CancelIo(hDev);
			GetOverlappedResult(hDev, &ov, &got, TRUE);
		}
	}
	CloseHandle(ov.hEvent);
	return ok;
}

// Unit exponent is a 4-bit signed nibble: 0..7 positive, 8..15 mean -8..-1.
// Do NOT use HidP_GetScaledUsageValue here. It maps logical onto physical, and Apple leaves
// PhysicalMin/Max at 0, which the spec reads as physical == logical, so it returns the raw
// value and the exponent is silently dropped. Gen 1: 0x04D1 UnitExp 13, raw 35264 = 35.264 lux.
static float unitScale(ULONG unitsExp) {
	int n = (int)(unitsExp & 0x0Fu);
	return std::pow(10.0f, (float)((n > 7) ? n - 16 : n));
}

static bool readField(PHIDP_PREPARSED_DATA prep, std::vector<uint8_t> &buf, USAGE u, ULONG *out) {
	if (!u) return false;
	return HidP_GetUsageValue(HidP_Input, HID_UP_SENSOR_PAGE, 0, u, out, prep,
	                          reinterpret_cast<PCHAR>(buf.data()), (ULONG)buf.size()) == HIDP_STATUS_SUCCESS;
}

bool AlsDevice::readAmbient(LONG *rawOut, AmbientReading *out) {
	if (!prep || !uIllum) return false;
	std::vector<uint8_t> buf;
	if (!readReport(buf)) return false;
	ULONG v = 0;
	if (!readField(prep, buf, uIllum, &v)) return false;
	if (rawOut) *rawOut = (LONG)v;
	if (!out) return true;

	*out = AmbientReading{};
	out->lux = (float)v * unitScale(eIllum);
	ULONG t = 0, cx = 0, cy = 0;
	const bool okT = readField(prep, buf, uTemp, &t);
	const bool okX = readField(prep, buf, uChromaX, &cx);
	const bool okY = readField(prep, buf, uChromaY, &cy);
	if (okT) out->colorTemp = (float)t * unitScale(eTemp);
	if (okX) out->chromaX   = (float)cx * unitScale(eChromaX);
	if (okY) out->chromaY   = (float)cy * unitScale(eChromaY);
	// Below a few photons the panel zeroes the colour fields (the XDR does it at 0 lux). That is
	// "no colour estimate", not 0 K, so do not hand a temperature that does not exist to the log or
	// to a True Tone consumer. The illuminance stays a valid reading of darkness.
	out->hasColour = (okT && t != 0) || (okX && okY && (cx != 0 || cy != 0));
	return true;
}

/* ============================ ContainerId ============================ */

static GUID queryContainerIdFromDevinfo(HDEVINFO set, PSP_DEVINFO_DATA devInfo) {
	GUID cid = {}; DEVPROPTYPE type = 0; DWORD size = 0;
	SetupDiGetDevicePropertyW(set, devInfo, &ALS_DEVPKEY_Device_ContainerId, &type, nullptr, 0, &size, 0);
	if (size == sizeof(GUID))
		SetupDiGetDevicePropertyW(set, devInfo, &ALS_DEVPKEY_Device_ContainerId, &type, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
	return cid;
}

/* ============================ Enumeration ============================ */

std::vector<AlsDevice> als_enumerate() {
	std::vector<AlsDevice> result;

	GUID hidGuid; HidD_GetHidGuid(&hidGuid);
	HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		Log::Error(L"als: SetupDiGetClassDevsW failed (%lu)", GetLastError());
		return result;
	}

	SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		if (!need) continue;
		std::vector<BYTE> ibuf(need);
		auto det = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(ibuf.data());
		det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
		SP_DEVINFO_DATA devInfo{sizeof(devInfo)};
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &devInfo)) continue;

		const wchar_t *path = det->DevicePath;
		if (!icontains(path, kAppleVid)) continue;

		HANDLE h = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                       OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (h == INVALID_HANDLE_VALUE) continue;

		PHIDP_PREPARSED_DATA prep = nullptr;
		if (!HidD_GetPreparsedData(h, &prep)) { CloseHandle(h); continue; }

		HIDP_CAPS caps{};
		if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) { HidD_FreePreparsedData(prep); CloseHandle(h); continue; }

		// Illuminance is the one we need; CCT and chromaticity share its report. Each field
		// carries its own unit exponent.
		USAGE uIllum = 0, uTemp = 0, uChromaX = 0, uChromaY = 0;
		ULONG eIllum = 0, eTemp = 0, eChromaX = 0, eChromaY = 0;
		UCHAR reportId = 0;
		USHORT n = caps.NumberInputValueCaps;
		if (n) {
			std::vector<HIDP_VALUE_CAPS> v(n);
			if (HidP_GetValueCaps(HidP_Input, v.data(), &n, prep) == HIDP_STATUS_SUCCESS) {
				for (USHORT k = 0; k < n; ++k) {
					if (v[k].UsagePage != HID_UP_SENSOR_PAGE || v[k].IsRange) continue;
					switch (v[k].NotRange.Usage) {
					case HID_USG_ILLUMINANCE:
						uIllum = HID_USG_ILLUMINANCE; eIllum = v[k].UnitsExp; reportId = v[k].ReportID; break;
					case HID_USG_COLOR_TEMP:
						uTemp = HID_USG_COLOR_TEMP; eTemp = v[k].UnitsExp; break;
					case HID_USG_CHROMA_X:
						uChromaX = HID_USG_CHROMA_X; eChromaX = v[k].UnitsExp; break;
					case HID_USG_CHROMA_Y:
						uChromaY = HID_USG_CHROMA_Y; eChromaY = v[k].UnitsExp; break;
					default: break;
					}
				}
			}
		}

		if (!uIllum) { HidD_FreePreparsedData(prep); CloseHandle(h); continue; }

		AlsDevice d;
		d.hDev = h; d.prep = prep; d.inputLen = caps.InputReportByteLength;
		d.reportId = reportId; d.overlapped = true;
		d.uIllum = uIllum; d.uTemp = uTemp; d.uChromaX = uChromaX; d.uChromaY = uChromaY;
		d.eIllum = eIllum; d.eTemp = eTemp; d.eChromaX = eChromaX; d.eChromaY = eChromaY;
		d.devicePath = path;
		d.label = shortLabel(path);
		d.containerId = queryContainerIdFromDevinfo(set, &devInfo);

		// Log the raw exponents: on an unfamiliar model this line is what tells us how to
		// scale that panel.
		LONG raw = 0; AmbientReading amb;
		const bool okRead = d.readAmbient(&raw, &amb);
		Log::Info(L"als: illuminance sensor %s (reportId=0x%02X) testRead=%s raw=%ld lux=%.2f "
		          L"unitExp[lux=%lu temp=%lu x=%lu y=%lu]",
		          d.label.c_str(), reportId, okRead ? L"ok" : L"FAILED", okRead ? raw : -1,
		          okRead ? amb.lux : -1.f, eIllum, eTemp, eChromaX, eChromaY);
		if (okRead && amb.hasColour)
			Log::Info(L"als: ambient colour %s: %.0f K, CIE x=%.4f y=%.4f (True Tone input)",
			          d.label.c_str(), amb.colorTemp, amb.chromaX, amb.chromaY);
		result.push_back(std::move(d));
	}
	SetupDiDestroyDeviceInfoList(set);
	Log::Info(L"ALS: %zu ambient light sensor(s) via raw HID", result.size());
	return result;
}

/* ============================ Watcher + snapshot ============================ */

// Snapshot published to the worker thread. `tick` is when the sample was taken, so a
// display that stops answering ages out instead of pinning auto-brightness forever.
struct AlsSnap { GUID container; AmbientReading amb; DWORD tick; bool valid; };

static std::vector<AlsDevice> g_als;
static std::vector<AlsSnap>   g_alsSnap;      // parallel to g_als; read by als_get_lux
static std::mutex             g_alsSnapMtx;

// Do not add a Reporting State (0x0316) write back here. It is a named array on this panel,
// so it sits in the Feature BUTTON caps as selectors 0x0840/0x0841 and a value-caps lookup
// never finds it. Unnecessary anyway: HidD_GetInputReport answers whatever the reporting
// state, same as Boot Camp. Keeps us from writing anything to the sensor interface.

// Same as the orientation watcher: a preset switch or a sleep cycle re-enumerates the HID
// interfaces and kills these handles. Re-scan once everything has stopped answering; back off
// to 30 s when there has never been a sensor, so a machine without the null driver is not
// running a SetupDi sweep every 3 s forever.
constexpr int   kAlsMaxFailures  = 8;      // ~2 s at the 250 ms poll interval
constexpr DWORD kAlsRescanFastMs = 3000;
constexpr DWORD kAlsRescanSlowMs = 30000;

static bool  g_alsEverFound = false;
static DWORD g_alsLastScan  = 0;

// Rebuilds the snapshot alongside the device list; the two are matched by index.
static void alsPublishFresh() {
	const DWORD now = GetTickCount();
	std::lock_guard<std::mutex> lk(g_alsSnapMtx);
	g_alsSnap.assign(g_als.size(), AlsSnap{});
	for (size_t i = 0; i < g_als.size(); ++i) {
		g_alsSnap[i].container = g_als[i].containerId;
		AmbientReading amb;
		if (g_als[i].readAmbient(nullptr, &amb)) {
			g_alsSnap[i].amb = amb; g_alsSnap[i].tick = now; g_alsSnap[i].valid = true;
		}
	}
}

void als_watch_init() {
	g_als = als_enumerate();
	// Seed with a real sample now: the worker anchors baseLux when it opens the device, which
	// is well before the first 250 ms tick. An empty snapshot there means it anchors on the
	// 100 lux placeholder instead.
	alsPublishFresh();
	g_alsEverFound = !g_als.empty();
	g_alsLastScan  = GetTickCount();
}

static void alsRescanIfNeeded() {
	bool anyAlive = false;
	for (const auto &d : g_als)
		if (d.isOpen() && d.failures < kAlsMaxFailures) { anyAlive = true; break; }
	if (anyAlive) return;

	const DWORD now  = GetTickCount();
	const DWORD wait = g_alsEverFound ? kAlsRescanFastMs : kAlsRescanSlowMs;
	if (now - g_alsLastScan < wait) return;
	g_alsLastScan = now;

	const bool had = !g_als.empty();
	g_als.clear();                  // destructors close the dead handles
	g_als = als_enumerate();
	alsPublishFresh();
	if (!g_als.empty()) {
		g_alsEverFound = true;
		if (had) Log::Info(L"als: sensor re-acquired after the interface went away");
	}
}

void als_watch_tick() {
	alsRescanIfNeeded();
	for (size_t i = 0; i < g_als.size(); ++i) {
		LONG raw = 0; AmbientReading amb;
		if (!g_als[i].readAmbient(&raw, &amb)) { ++g_als[i].failures; continue; }
		g_als[i].failures = 0;
		const DWORD now = GetTickCount();
		{
			std::lock_guard<std::mutex> lk(g_alsSnapMtx);
			if (i < g_alsSnap.size()) {
				g_alsSnap[i].amb = amb; g_alsSnap[i].tick = now; g_alsSnap[i].valid = true;
			}
		}
		// Noisy sensor: every change is several lines a second, and one fsync each with file
		// logging on. Only log a 5% move, 2 s apart at most.
		const LONG last = g_als[i].lastRaw;
		const LONG delta = (last == LONG_MIN) ? raw : (raw > last ? raw - last : last - raw);
		const LONG threshold = (last == LONG_MIN) ? 0 : (last / 20 + 1);
		if (last != LONG_MIN && (delta < threshold || now - g_als[i].lastLogTick < 2000)) continue;
		if (amb.hasColour)
			Log::Info(L"als: %s raw=%ld lux=%.2f %.0fK x=%.4f y=%.4f",
			          g_als[i].label.c_str(), raw, amb.lux, amb.colorTemp, amb.chromaX, amb.chromaY);
		else
			Log::Info(L"als: %s raw=%ld lux=%.2f", g_als[i].label.c_str(), raw, amb.lux);
		g_als[i].lastRaw = raw;
		g_als[i].lastLogTick = now;
	}
}

void als_watch_shutdown() {
	{
		std::lock_guard<std::mutex> lk(g_alsSnapMtx);
		g_alsSnap.clear();
	}
	g_als.clear();
}

bool als_get_ambient(const GUID *containerId, AmbientReading *out) {
	if (!out) return false;
	std::lock_guard<std::mutex> lk(g_alsSnapMtx);
	static const GUID zero = {};
	const DWORD now = GetTickCount();
	const bool wantMatch = containerId && memcmp(containerId, &zero, sizeof(GUID)) != 0;
	// A display can carry more than one sensor (the XDRs have a front and a rear one). Take
	// the brightest fresh reading, like Boot Camp does: a covered or shadowed sensor can only
	// under-read, so the max is the one that still tracks the room.
	bool found = false;
	for (const auto &s : g_alsSnap) {
		if (!s.valid || now - s.tick > kAlsMaxAgeMs) continue;   // stale: let the caller fall back
		if (wantMatch && memcmp(&s.container, containerId, sizeof(GUID)) != 0) continue;
		if (!found || s.amb.lux > out->lux) { *out = s.amb; found = true; }
	}
	return found;   // false: no fresh raw-HID reading for this display
}

bool als_get_lux(const GUID *containerId, float *lux) {
	if (!lux) return false;
	AmbientReading a;
	if (!als_get_ambient(containerId, &a)) return false;
	*lux = a.lux;
	return true;
}
