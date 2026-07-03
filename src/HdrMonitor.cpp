#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <shlwapi.h>
#include <vector>
#include "HdrMonitor.h"
#include "Log.h"

#pragma comment(lib, "dxgi.lib")

namespace {

// DXGI gives us each output's GDI device name (e.g. L"\\.\DISPLAY1"). EnumDisplayDevices on it
// yields the monitor's PnP id, which starts with "MONITOR\APP" for Apple (EDID vendor code "APP").
bool isAppleOutput(const wchar_t *gdiDeviceName) {
	DISPLAY_DEVICEW dd{};
	dd.cb = sizeof(dd);
	if (!EnumDisplayDevicesW(gdiDeviceName, 0, &dd, 0))
		return false;
	return StrCmpNIW(dd.DeviceID, L"MONITOR\\APP", 11) == 0;
}

} // namespace

bool HdrAnyAppleDisplayActive() {
	IDXGIFactory1 *factory = nullptr;
	if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&factory))) || !factory)
		return false;

	bool hdr = false;
	IDXGIAdapter1 *adapter = nullptr;
	for (UINT a = 0; !hdr && factory->EnumAdapters1(a, &adapter) != DXGI_ERROR_NOT_FOUND; ++a) {
		IDXGIOutput *output = nullptr;
		for (UINT o = 0; !hdr && adapter->EnumOutputs(o, &output) != DXGI_ERROR_NOT_FOUND; ++o) {
			IDXGIOutput6 *output6 = nullptr;
			if (SUCCEEDED(output->QueryInterface(IID_PPV_ARGS(&output6))) && output6) {
				DXGI_OUTPUT_DESC1 desc{};
				if (SUCCEEDED(output6->GetDesc1(&desc))
				    && desc.ColorSpace == DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020
				    && isAppleOutput(desc.DeviceName)) {
					hdr = true;
				}
				output6->Release();
			}
			output->Release();
			output = nullptr;
		}
		adapter->Release();
		adapter = nullptr;
	}
	factory->Release();
	return hdr;
}

int HdrTurnOffForAppleDisplays() {
	UINT32 numPaths = 0, numModes = 0;
	if (GetDisplayConfigBufferSizes(QDC_ONLY_ACTIVE_PATHS, &numPaths, &numModes) != ERROR_SUCCESS || !numPaths)
		return 0;
	std::vector<DISPLAYCONFIG_PATH_INFO> paths(numPaths);
	std::vector<DISPLAYCONFIG_MODE_INFO> modes(numModes ? numModes : 1);
	if (QueryDisplayConfig(QDC_ONLY_ACTIVE_PATHS, &numPaths, paths.data(), &numModes, modes.data(), nullptr) != ERROR_SUCCESS)
		return 0;

	int done = 0;
	for (UINT32 i = 0; i < numPaths; ++i) {
		DISPLAYCONFIG_TARGET_DEVICE_NAME tname{};
		tname.header.type      = DISPLAYCONFIG_DEVICE_INFO_GET_TARGET_NAME;
		tname.header.size      = sizeof(tname);
		tname.header.adapterId = paths[i].targetInfo.adapterId;
		tname.header.id        = paths[i].targetInfo.id;
		if (DisplayConfigGetDeviceInfo(&tname.header) != ERROR_SUCCESS)
			continue;
		// Apple monitor device paths carry the EDID vendor code, e.g. \\?\DISPLAY#APP1114#...
		if (!StrStrIW(tname.monitorDevicePath, L"#APP"))
			continue;

		// Both SET packets are a header plus one UINT32 whose bit 0 is the enable flag. Use the
		// raw type values so this builds the same on every SDK: 16 = SET_HDR_STATE (Win11 24H2+),
		// 10 = SET_ADVANCED_COLOR_STATE (the pre-24H2 HDR toggle).
		struct {
			DISPLAYCONFIG_DEVICE_INFO_HEADER header;
			UINT32 value;
		} pkt{};
		pkt.header.size      = sizeof(pkt);
		pkt.header.adapterId = paths[i].targetInfo.adapterId;
		pkt.header.id        = paths[i].targetInfo.id;
		pkt.value            = 0; // HDR / advanced color off

		pkt.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)16;
		LONG rc = DisplayConfigSetDeviceInfo(&pkt.header);
		if (rc != ERROR_SUCCESS) {
			pkt.header.type = (DISPLAYCONFIG_DEVICE_INFO_TYPE)10;
			rc = DisplayConfigSetDeviceInfo(&pkt.header);
		}
		if (rc == ERROR_SUCCESS) {
			Log::Info(L"HDR off accepted for %s", tname.monitorFriendlyDeviceName);
			++done;
		} else {
			Log::Warn(L"HDR off refused for %s (rc=%ld)", tname.monitorFriendlyDeviceName, rc);
		}
	}
	return done;
}
