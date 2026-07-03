#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <cstdint>
#include "NvHdr.h"
#include "Log.h"

// Minimal dynamic NVAPI binding. nvapi64.dll ships with the NVIDIA driver and exposes everything
// through nvapi_QueryInterface(id); the ids below are the public, ABI-stable values used by
// open-source HDR tools. Everything fails soft: missing dll, missing entry point or an error
// status just means no log line (or an rc line), never a crash.
namespace {

typedef int NvStatus; // 0 = NVAPI_OK, -9 = NVAPI_INCOMPATIBLE_STRUCT_VERSION
typedef void *(__cdecl *PfnQueryInterface)(unsigned int id);
typedef NvStatus (__cdecl *PfnInitialize)();
typedef NvStatus (__cdecl *PfnDispIdByName)(const char *gdiName, unsigned int *dispId);
typedef NvStatus (__cdecl *PfnHdrColorControl)(unsigned int dispId, void *pHdrColorData);

constexpr unsigned int kIdInitialize     = 0x0150E828u; // NvAPI_Initialize
constexpr unsigned int kIdDispIdByName   = 0xFF09EF30u; // NvAPI_DISP_GetDisplayIdByDisplayName
constexpr unsigned int kIdHdrColorControl = 0x351DA224u; // NvAPI_Disp_HdrColorControl

// NV_HDR_COLOR_DATA, laid out to the largest (V2) shape. hdrMode sits at offset 8 in every
// version, and NVAPI validates the version field, so a size mismatch returns -9 cleanly and we
// retry with the smaller layouts.
#pragma pack(push, 8)
struct NvHdrColorData {
	uint32_t version;
	uint32_t cmd;     // 0 = GET
	uint32_t hdrMode; // 0 = off, anything else = an HDR/EDR mode is active in the driver
	uint32_t staticMetadataDescriptorId;
	uint16_t masteringDisplayData[12];
	uint32_t hdrColorFormat;
	uint32_t hdrDynamicRange;
	uint32_t hdrBpc;
};
#pragma pack(pop)

} // namespace

void NvapiLogHdrState(const wchar_t *context) {
	static PfnQueryInterface qi = [] {
		HMODULE m = LoadLibraryW(L"nvapi64.dll");
		return m ? (PfnQueryInterface)GetProcAddress(m, "nvapi_QueryInterface") : nullptr;
	}();
	if (!qi)
		return; // no NVIDIA driver: stay silent
	static PfnInitialize     init   = (PfnInitialize)qi(kIdInitialize);
	static PfnDispIdByName   byName = (PfnDispIdByName)qi(kIdDispIdByName);
	static PfnHdrColorControl hdrCtl = (PfnHdrColorControl)qi(kIdHdrColorControl);
	if (!init || !byName || !hdrCtl)
		return;
	static bool inited = (init() == 0);
	if (!inited)
		return;

	DISPLAY_DEVICEW dd{};
	dd.cb = sizeof(dd);
	for (DWORD i = 0; EnumDisplayDevicesW(nullptr, i, &dd, 0); ++i) {
		if (dd.StateFlags & DISPLAY_DEVICE_ATTACHED_TO_DESKTOP) {
			char gdiName[64] = {};
			WideCharToMultiByte(CP_ACP, 0, dd.DeviceName, -1, gdiName, sizeof(gdiName) - 1, nullptr, nullptr);
			unsigned int dispId = 0;
			if (byName(gdiName, &dispId) == 0) { // fails for displays not on the NVIDIA GPU
				NvHdrColorData d{};
				NvStatus rc = -9;
				// V2 full (52), V2 packed-enum variant (44), then V1 (40)
				const uint32_t versions[] = {52u | (2u << 16), 44u | (2u << 16), 40u | (1u << 16)};
				for (uint32_t ver : versions) {
					d         = {};
					d.version = ver;
					d.cmd     = 0; // GET
					rc        = hdrCtl(dispId, &d);
					if (rc != -9)
						break;
				}
				if (rc == 0)
					Log::Info(L"NVAPI [%s] %s: driver HDR mode=%u", context, dd.DeviceName, d.hdrMode);
				else
					Log::Info(L"NVAPI [%s] %s: HdrColorControl rc=%d", context, dd.DeviceName, rc);
			}
		}
		dd = {};
		dd.cb = sizeof(dd);
	}
}
