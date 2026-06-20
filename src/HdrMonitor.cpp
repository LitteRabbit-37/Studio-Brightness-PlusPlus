#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <dxgi1_6.h>
#include <shlwapi.h>
#include "HdrMonitor.h"

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
