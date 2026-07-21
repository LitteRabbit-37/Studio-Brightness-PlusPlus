//----------------  orientation.cpp  ----------------
#include "orientation.h"
#include "Log.h"

#define _WIN32_DCOM
#include <initguid.h>
#include <devpropdef.h>
#include <setupapi.h>
#include <shlwapi.h>
#include <utility>
#include <cstring>
#include <atomic>

#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shlwapi.lib")
#pragma comment(lib, "user32.lib")

// DEVPKEY_Device_ContainerId : {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2
DEFINE_DEVPROPKEY(ORI_DEVPKEY_Device_ContainerId,
                  0x8c7ed206, 0x3f8a, 0x4827,
                  0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);

static const wchar_t kAppleVid[] = L"vid_05ac";

static inline bool icontains(const wchar_t *hay, const wchar_t *needle) {
	return StrStrIW(hay, needle) != nullptr;
}

/* ============================ OrientationDevice ============================ */

void OrientationDevice::moveFrom(OrientationDevice &o) {
	hDev = o.hDev; prep = o.prep; inputLen = o.inputLen; reportId = o.reportId;
	uX = o.uX; uY = o.uY; uZ = o.uZ; overlapped = o.overlapped; containerId = o.containerId;
	devicePath = std::move(o.devicePath); gdiName = std::move(o.gdiName);
	lastX = o.lastX; lastY = o.lastY; lastZ = o.lastZ;
	o.hDev = INVALID_HANDLE_VALUE; o.prep = nullptr;
}

void OrientationDevice::close() {
	if (prep) { HidD_FreePreparsedData(prep); prep = nullptr; }
	if (hDev != INVALID_HANDLE_VALUE) { CloseHandle(hDev); hDev = INVALID_HANDLE_VALUE; }
}

bool OrientationDevice::readReport(std::vector<uint8_t> &buf) {
	if (hDev == INVALID_HANDLE_VALUE || inputLen == 0) return false;
	buf.assign(inputLen, 0);
	buf[0] = reportId;
	// Preferred path (what BootCampService uses): GET_REPORT on the Input report.
	if (HidD_GetInputReport(hDev, buf.data(), (ULONG)buf.size()))
		return true;
	// Fallback: the panel only pushes reports -> overlapped ReadFile with a short wait.
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
			CancelIo(hDev);
		}
	}
	CloseHandle(ov.hEvent);
	return ok;
}

static LONG getUsage(PHIDP_PREPARSED_DATA prep, USAGE u, std::vector<uint8_t> &buf) {
	if (!u) return LONG_MIN;
	ULONG v = 0;
	NTSTATUS s = HidP_GetUsageValue(HidP_Input, HID_UP_SENSOR, 0, u, &v, prep,
	                                reinterpret_cast<PCHAR>(buf.data()), (ULONG)buf.size());
	return (s == HIDP_STATUS_SUCCESS) ? (LONG)v : LONG_MIN;
}

bool OrientationDevice::readTilt(LONG *x, LONG *y, LONG *z) {
	if (!prep) return false;
	std::vector<uint8_t> buf;
	if (!readReport(buf)) return false;
	if (x) *x = getUsage(prep, uX, buf);
	if (y) *y = getUsage(prep, uY, buf);
	if (z) *z = getUsage(prep, uZ, buf);
	return true;
}

/* ============================ Display resolution ============================ */

static GUID queryContainerIdFromDevinfo(HDEVINFO set, PSP_DEVINFO_DATA devInfo) {
	GUID cid = {}; DEVPROPTYPE type = 0; DWORD size = 0;
	SetupDiGetDevicePropertyW(set, devInfo, &ORI_DEVPKEY_Device_ContainerId, &type, nullptr, 0, &size, 0);
	if (size == sizeof(GUID))
		SetupDiGetDevicePropertyW(set, devInfo, &ORI_DEVPKEY_Device_ContainerId, &type, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
	return cid;
}

static GUID containerIdOfMonitorPath(const wchar_t *ifacePath) {
	GUID cid = {};
	HDEVINFO di = SetupDiCreateDeviceInfoList(nullptr, nullptr);
	if (di == INVALID_HANDLE_VALUE) return cid;
	SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};
	if (SetupDiOpenDeviceInterfaceW(di, ifacePath, 0, &ifd)) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(di, &ifd, nullptr, 0, &need, nullptr);
		if (need) {
			std::vector<BYTE> buf(need);
			auto det = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
			det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
			SP_DEVINFO_DATA dev{sizeof(dev)};
			if (SetupDiGetDeviceInterfaceDetailW(di, &ifd, det, need, nullptr, &dev))
				cid = queryContainerIdFromDevinfo(di, &dev);
		}
		SetupDiDeleteDeviceInterfaceData(di, &ifd);
	}
	SetupDiDestroyDeviceInfoList(di);
	return cid;
}

static bool edidIsApple(USHORT edidMfg) {
	// edidManufactureId is stored big-endian; unpack 3x 5-bit letters (1=A..26=Z).
	USHORT id = (USHORT)((edidMfg >> 8) | (edidMfg << 8));
	char a = (char)('A' + (((id >> 10) & 0x1f) - 1));
	char b = (char)('A' + (((id >> 5)  & 0x1f) - 1));
	char c = (char)('A' + (( id        & 0x1f) - 1));
	return a == 'A' && b == 'P' && c == 'P';
}

// Walk active display paths; `wantApple` picks the first non-internal Apple monitor,
// otherwise match by ContainerId. Returns the GDI name ("\\.\DISPLAYx").
static std::wstring resolveGdi(const GUID *containerId, bool wantApple) {
	UINT32 nPath = 0, nMode = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &nPath, &nMode) != ERROR_SUCCESS) return L"";
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(nPath);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(nMode);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &nPath, paths.data(), &nMode, modes.data(), nullptr) != ERROR_SUCCESS)
		return L"";
	for (UINT32 i = 0; i < nPath; ++i) {
		DISPLAYCONFIG_TARGET_DEVICE_NAME tn{};
		tn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tn.header.size = sizeof(tn);
		tn.header.adapterId = paths[i].targetInfo.adapterId;
		tn.header.id        = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tn.header) != ERROR_SUCCESS) continue;

		bool match = false;
		if (wantApple) {
			bool internal = (tn.outputTechnology == DISPLAYCONFIG_OUTPUT_TECHNOLOGY_INTERNAL);
			match = !internal && edidIsApple(tn.edidManufactureId);
		} else if (containerId) {
			GUID cid = containerIdOfMonitorPath(tn.monitorDevicePath);
			match = (memcmp(&cid, containerId, sizeof(GUID)) == 0);
		}
		if (!match) continue;

		DISPLAYCONFIG_SOURCE_DEVICE_NAME sn{};
		sn.header.type = DISPLAYCONFIG_DEVICE_INFO_GET_SOURCE_NAME;
		sn.header.size = sizeof(sn);
		sn.header.adapterId = paths[i].sourceInfo.adapterId;
		sn.header.id        = paths[i].sourceInfo.id;
		if (DisplayConfigGetDeviceInfo(&sn.header) == ERROR_SUCCESS)
			return sn.viewGdiDeviceName;
	}
	return L"";
}

std::wstring orient_resolve_display(const OrientationDevice &d) {
	static const GUID empty = {};
	std::wstring name;
	if (memcmp(&d.containerId, &empty, sizeof(GUID)) != 0)
		name = resolveGdi(&d.containerId, false); // by ContainerId
	if (name.empty())
		name = resolveGdi(nullptr, true); // fallback: first Apple external display
	return name;
}

/* ============================ Enumeration ============================ */

std::vector<OrientationDevice> orient_enumerate() {
	std::vector<OrientationDevice> result;

	GUID hidGuid; HidD_GetHidGuid(&hidGuid);
	HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		Log::Error(L"orient: SetupDiGetClassDevsW failed (%lu)", GetLastError());
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

		// Find the orientation tilt axes among the Input value caps (page 0x20, usages 0x047F/80/81).
		USAGE uX = 0, uY = 0, uZ = 0; UCHAR reportId = 0;
		USHORT n = caps.NumberInputValueCaps;
		if (n) {
			std::vector<HIDP_VALUE_CAPS> v(n);
			if (HidP_GetValueCaps(HidP_Input, v.data(), &n, prep) == HIDP_STATUS_SUCCESS) {
				for (USHORT k = 0; k < n; ++k) {
					if (v[k].UsagePage != HID_UP_SENSOR || v[k].IsRange) continue;
					USAGE u = v[k].NotRange.Usage;
					if (u == HID_USG_ORIENT_TILT_X) { uX = u; reportId = v[k].ReportID; }
					else if (u == HID_USG_ORIENT_TILT_Y) { uY = u; reportId = v[k].ReportID; }
					else if (u == HID_USG_ORIENT_TILT_Z) { uZ = u; reportId = v[k].ReportID; }
				}
			}
		}

		// Accept only interfaces exposing Tilt Y (Boot Camp's rotation axis).
		if (!uY) { HidD_FreePreparsedData(prep); CloseHandle(h); continue; }

		OrientationDevice d;
		d.hDev = h; d.prep = prep; d.inputLen = caps.InputReportByteLength;
		d.reportId = reportId; d.uX = uX; d.uY = uY; d.uZ = uZ; d.overlapped = true;
		d.devicePath = path;
		d.containerId = queryContainerIdFromDevinfo(set, &devInfo);
		d.gdiName = orient_resolve_display(d);

		Log::Info(L"orient: orientation sensor -> %s (reportId=0x%02X)",
		          d.gdiName.empty() ? L"(display unresolved)" : d.gdiName.c_str(), reportId);
		result.push_back(std::move(d));
	}
	SetupDiDestroyDeviceInfoList(set);
	Log::Info(L"orient: %zu orientation sensor(s) found", result.size());
	return result;
}

/* ============================ Angle -> rotation ============================ */

// Studio Display reports only two orientations via Tilt Y: 0 = landscape,
// 270 = portrait (90 deg clockwise). g_dirSign=-1 gives the correct handedness
// (0->DEFAULT, 270->DMDO_90). The 90/180 branches are kept for completeness in case
// a future panel reports the other positions.
static int  g_dirSign   = -1;
static LONG g_angleZero = 0;

int orient_angle_to_dmdo(LONG a) {
	if (a == LONG_MIN) return -1;
	long d = ((long)a - g_angleZero);
	d = ((d % 360) + 360) % 360;
	if (g_dirSign < 0) d = (360 - d) % 360;
	if (d < 45 || d >= 315) return DMDO_DEFAULT;
	if (d < 135)            return DMDO_90;
	if (d < 225)            return DMDO_180;
	return DMDO_270;
}

bool orient_apply_rotation(const std::wstring &gdiName, int dmdo) {
	if (gdiName.empty() || dmdo < 0) return false;
	DEVMODEW dm{}; dm.dmSize = sizeof(dm);
	if (!EnumDisplaySettingsW(gdiName.c_str(), ENUM_CURRENT_SETTINGS, &dm)) return false;
	if ((int)dm.dmDisplayOrientation == dmdo) return true;
	bool curPortrait = (dm.dmDisplayOrientation == DMDO_90 || dm.dmDisplayOrientation == DMDO_270);
	bool newPortrait = (dmdo == DMDO_90 || dmdo == DMDO_270);
	if (curPortrait != newPortrait) std::swap(dm.dmPelsWidth, dm.dmPelsHeight);
	dm.dmDisplayOrientation = (DWORD)dmdo;
	dm.dmFields = DM_DISPLAYORIENTATION | DM_PELSWIDTH | DM_PELSHEIGHT;
	LONG r = ChangeDisplaySettingsExW(gdiName.c_str(), &dm, nullptr, CDS_UPDATEREGISTRY, nullptr);
	if (r != DISP_CHANGE_SUCCESSFUL)
		Log::Warn(L"orient: ChangeDisplaySettingsEx(%s) returned %ld", gdiName.c_str(), r);
	return r == DISP_CHANGE_SUCCESSFUL;
}

/* ============================ Watcher ============================ */

static std::vector<OrientationDevice> g_orient;
static std::atomic<bool>              g_orientEnabled{true};

void orient_set_enabled(bool enabled) {
	bool was = g_orientEnabled.exchange(enabled);
	if (enabled && !was) {
		// Re-apply the current physical orientation on the next tick.
		for (auto &d : g_orient) { d.lastX = d.lastY = d.lastZ = LONG_MIN; }
	}
}

void orient_watch_init() {
	g_orient = orient_enumerate();
}

void orient_watch_tick() {
	if (!g_orientEnabled.load()) return;
	for (auto &d : g_orient) {
		LONG x = LONG_MIN, y = LONG_MIN, z = LONG_MIN;
		if (!d.readTilt(&x, &y, &z)) continue;
		if (x == d.lastX && y == d.lastY && z == d.lastZ) continue; // log only on real change
		Log::Info(L"orient: tilt X=%ld Y=%ld Z=%ld (%s)", x, y, z,
		          d.gdiName.empty() ? L"?" : d.gdiName.c_str());
		d.lastX = x; d.lastY = y; d.lastZ = z;

		int dmdo = orient_angle_to_dmdo(y); // Tilt Y drives rotation (Boot Camp parity)
		if (dmdo < 0) continue;

		if (d.gdiName.empty()) d.gdiName = orient_resolve_display(d);
		if (d.gdiName.empty()) { Log::Warn(L"orient: no target display resolved; not rotating"); continue; }

		if (orient_apply_rotation(d.gdiName, dmdo))
			Log::Info(L"orient: rotated %s to DMDO %d (tiltY=%ld)", d.gdiName.c_str(), dmdo, y);
	}
}

void orient_watch_shutdown() {
	g_orient.clear();
}
