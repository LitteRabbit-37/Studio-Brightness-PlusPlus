//----------------  hid.cpp  ----------------
#include "hid.h"
#define _WIN32_DCOM
#include <hidsdi.h>
#include <setupapi.h>
#include <shlwapi.h> // StrStrIW
#include <vector>
#include <cwchar>
#include <cstdint>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shlwapi.lib")

/* ---------- Apple Studio Display ---------- */
static const wchar_t vidStr[]           = L"vid_05ac";
static const wchar_t pidStudioDisplay[] = L"pid_1114"; // Studio Display
static const wchar_t pidXDR[]           = L"pid_9243"; // Pro Display XDR
static const wchar_t interfaceStr[]     = L"mi_07";
static const wchar_t collectionStr[]    = L"&col"; // pour exclure les COLxx

inline bool icontains(const wchar_t *hay, const wchar_t *needle) {
	return StrStrIW(hay, needle) != nullptr; // insensible à la casse
}

/* ---------- globals ---------- */
static HANDLE               hDev = INVALID_HANDLE_VALUE;
static PHIDP_PREPARSED_DATA prep = nullptr;

static struct {
	USHORT len;
	USAGE  page;
	USAGE  usage;
	UCHAR  id;
} inCaps, featCaps;

static void closeCurrent() {
	if (prep) {
		HidD_FreePreparsedData(prep);
		prep = nullptr;
	}
	if (hDev != INVALID_HANDLE_VALUE) {
		CloseHandle(hDev);
		hDev = INVALID_HANDLE_VALUE;
	}
}

/* ============================================================ */
int hid_init(DisplayType *outType) {
	if (hDev != INVALID_HANDLE_VALUE)
		return -1;

	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);
	HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, 0, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE)
		return -2;

	SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		if (!need)
			continue;

		std::vector<BYTE> buf(need);
		auto              det = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
		det->cbSize           = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr))
			continue;

		const wchar_t *path = det->DevicePath;

		/*** filtre : VID/PID & MI_07 sans &COLxx ***/
		if (!icontains(path, vidStr))
			continue;
		if (icontains(path, collectionStr))
			continue;

		// Check for specific PIDs
		DisplayType foundType = DisplayType::None;
		if (icontains(path, pidStudioDisplay)) {
			foundType = DisplayType::StudioDisplay;
		} else if (icontains(path, pidXDR)) {
			foundType = DisplayType::ProXDR;
		}

		if (foundType == DisplayType::None)
			continue;

		hDev = CreateFileW(path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                   OPEN_EXISTING, 0, nullptr);
		if (hDev == INVALID_HANDLE_VALUE) {
			closeCurrent();
			continue;
		}

		if (!HidD_GetPreparsedData(hDev, &prep)) {
			closeCurrent();
			continue;
		}

		HIDP_CAPS caps{};
		if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) {
			closeCurrent();
			continue;
		}

		inCaps.len   = caps.InputReportByteLength;
		featCaps.len = caps.FeatureReportByteLength;

		HIDP_VALUE_CAPS v[4]{};
		USHORT          len = 4;

		/* Input */
		if (HidP_GetValueCaps(HidP_Input, v, &len, prep) != HIDP_STATUS_SUCCESS || len == 0 || v[0].ReportCount != 1) {
			closeCurrent();
			continue;
		}

		inCaps.id    = v[0].ReportID;
		inCaps.page  = v[0].UsagePage;
		inCaps.usage = v[0].NotRange.Usage;

		/* Feature */
		len = 4;
		if (HidP_GetValueCaps(HidP_Feature, v, &len, prep) != HIDP_STATUS_SUCCESS || len == 0 ||
		    v[0].ReportCount != 1) {
			closeCurrent();
			continue;
		}

		featCaps.id    = v[0].ReportID;
		featCaps.page  = v[0].UsagePage;
		featCaps.usage = v[0].NotRange.Usage;

		SetupDiDestroyDeviceInfoList(set);
		if (outType)
			*outType = foundType;
		return 0; // SUCCESS
	}

	SetupDiDestroyDeviceInfoList(set);
	return -10; // rien trouvé
}

/* ============================================================ */
/* -------------------- GET / SET / RANGE  -------------------  */
int hid_getBrightness(ULONG *val) {
	if (hDev == INVALID_HANDLE_VALUE)
		return -1;
	uint8_t buf[100]{};
	buf[0] = inCaps.id;
	if (!HidD_GetInputReport(hDev, buf, sizeof(buf)))
		return -2;
	NTSTATUS s = HidP_GetUsageValue(HidP_Input, inCaps.page, 0, inCaps.usage, val, prep, reinterpret_cast<PCHAR>(buf),
	                                inCaps.len);
	return (s == HIDP_STATUS_SUCCESS) ? 0 : -3;
}
int hid_setBrightness(ULONG v) {
	if (hDev == INVALID_HANDLE_VALUE)
		return -1;
	uint8_t buf[100]{};
	buf[0] = featCaps.id;
	if (!HidD_GetFeature(hDev, buf, sizeof(buf)))
		return -2;
	NTSTATUS s = HidP_SetUsageValue(HidP_Feature, featCaps.page, 0, featCaps.usage, v, prep,
	                                reinterpret_cast<PCHAR>(buf), featCaps.len);
	if (s != HIDP_STATUS_SUCCESS)
		return -3;
	return HidD_SetFeature(hDev, buf, featCaps.len) ? 0 : -4;
}
int hid_getBrightnessRange(ULONG *mn, ULONG *mx) {
	if (!prep || hDev == INVALID_HANDLE_VALUE)
		return -1;
	HIDP_VALUE_CAPS v{};
	USHORT          len = 1;
	if (HidP_GetValueCaps(HidP_Feature, &v, &len, prep) != HIDP_STATUS_SUCCESS || len == 0)
		return -2;
	*mn = v.LogicalMin;
	*mx = v.LogicalMax;
	return 0;
}
void hid_deinit() {
	closeCurrent();
}
