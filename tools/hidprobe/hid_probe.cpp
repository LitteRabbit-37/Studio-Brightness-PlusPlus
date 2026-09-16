// hid_probe.cpp
//
// Read-only diagnostic for Apple displays on Windows. Opens every HID interface with Apple's
// vendor ID and prints what each one exposes: top level collection, every Input and Feature
// value cap with its usage, report ID, logical and physical ranges and unit exponent, every
// button cap, and one Input report fetched with HidD_GetInputReport.
//
// This is how docs/hid-map.md was built. The app's own log only lists Feature value caps, so
// this is the tool to run when a new model shows up or when a sensor is not where we expect it.
// It writes nothing to the display.
//
// Build from a Visual Studio developer prompt:
//     cl /nologo /EHsc /std:c++20 hid_probe.cpp
// Then run hid_probe.exe and paste the whole output. Interfaces that Windows has failed
// (Code 10) do not appear at all, that is expected.

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <setupapi.h>
#include <cstdio>
#include <vector>
#include <string>
#include <set>
#pragma comment(lib, "hid.lib")
#pragma comment(lib, "setupapi.lib")

// Bytes of an Input report shown before truncating. MI_06 col04 carries a 2001-byte report.
static const size_t kMaxDumpBytes = 64;

// The HID unit exponent is a 4-bit signed nibble: 0..7 positive, 8..15 mean -8..-1.
static int unitExponent(ULONG raw) {
    int n = (int)(raw & 0x0Fu);
    return n > 7 ? n - 16 : n;
}

static void dumpValueCaps(PHIDP_PREPARSED_DATA prep, HIDP_REPORT_TYPE rt, const char* tag,
                          std::set<UCHAR>& reportIds) {
    HIDP_CAPS caps{};
    if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) return;
    USHORT n = (rt == HidP_Input) ? caps.NumberInputValueCaps
             : (rt == HidP_Feature) ? caps.NumberFeatureValueCaps
             : caps.NumberOutputValueCaps;
    if (!n) return;
    std::vector<HIDP_VALUE_CAPS> v(n);
    if (HidP_GetValueCaps(rt, v.data(), &n, prep) != HIDP_STATUS_SUCCESS) return;
    for (USHORT i = 0; i < n; i++) {
        HIDP_VALUE_CAPS& c = v[i];
        USAGE umin = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
        USAGE umax = c.IsRange ? c.Range.UsageMax : c.NotRange.Usage;
        printf("    [%s VAL ] Page=0x%04X Usage=0x%04X..0x%04X ReportID=0x%02X Bits=%u Count=%u "
               "Log=%ld..%ld Phys=%ld..%ld UnitExp=%d Units=0x%08lX\n",
               tag, c.UsagePage, umin, umax, c.ReportID, c.BitSize, c.ReportCount,
               (long)c.LogicalMin, (long)c.LogicalMax, (long)c.PhysicalMin, (long)c.PhysicalMax,
               unitExponent(c.UnitsExp), (unsigned long)c.Units);
        if (rt == HidP_Input) reportIds.insert(c.ReportID);
    }
}

static void dumpButtonCaps(PHIDP_PREPARSED_DATA prep, HIDP_REPORT_TYPE rt, const char* tag,
                           std::set<UCHAR>& reportIds) {
    HIDP_CAPS caps{};
    if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS) return;
    USHORT n = (rt == HidP_Input) ? caps.NumberInputButtonCaps
             : (rt == HidP_Feature) ? caps.NumberFeatureButtonCaps
             : caps.NumberOutputButtonCaps;
    if (!n) return;
    std::vector<HIDP_BUTTON_CAPS> v(n);
    if (HidP_GetButtonCaps(rt, v.data(), &n, prep) != HIDP_STATUS_SUCCESS) return;
    for (USHORT i = 0; i < n; i++) {
        HIDP_BUTTON_CAPS& c = v[i];
        USAGE umin = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
        USAGE umax = c.IsRange ? c.Range.UsageMax : c.NotRange.Usage;
        printf("    [%s BTN ] Page=0x%04X Usage=0x%04X..0x%04X ReportID=0x%02X\n",
               tag, c.UsagePage, umin, umax, c.ReportID);
        if (rt == HidP_Input) reportIds.insert(c.ReportID);
    }
}

int main() {
    GUID g; HidD_GetHidGuid(&g);
    HDEVINFO set = SetupDiGetClassDevsW(&g, nullptr, nullptr, DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
    if (set == INVALID_HANDLE_VALUE) { printf("SetupDiGetClassDevs failed\n"); return 1; }

    SP_DEVICE_INTERFACE_DATA ifd{ sizeof(ifd) };
    for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &g, i, &ifd); i++) {
        DWORD need = 0;
        SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
        if (!need) continue;
        std::vector<BYTE> buf(need);
        auto det = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
        det->cbSize = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);
        if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, nullptr)) continue;

        std::wstring path = det->DevicePath;
        std::wstring lower = path;
        for (auto& ch : lower) ch = towlower(ch);
        if (lower.find(L"vid_05ac") == std::wstring::npos) continue;

        printf("\n== %ls\n", path.c_str());
        HANDLE h = CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE,
                               FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr, OPEN_EXISTING, 0, nullptr);
        bool rw = (h != INVALID_HANDLE_VALUE);
        if (!rw) h = CreateFileW(path.c_str(), 0, FILE_SHARE_READ | FILE_SHARE_WRITE,
                                 nullptr, OPEN_EXISTING, 0, nullptr);
        if (h == INVALID_HANDLE_VALUE) { printf("   OPEN FAILED err=%lu\n", GetLastError()); continue; }
        printf("   opened %s\n", rw ? "RW" : "query-only");

        HIDD_ATTRIBUTES at{ sizeof(at) };
        if (HidD_GetAttributes(h, &at))
            printf("   VID=0x%04X PID=0x%04X Ver=0x%04X\n", at.VendorID, at.ProductID, at.VersionNumber);

        PHIDP_PREPARSED_DATA prep = nullptr;
        if (HidD_GetPreparsedData(h, &prep)) {
            HIDP_CAPS caps{};
            if (HidP_GetCaps(prep, &caps) == HIDP_STATUS_SUCCESS) {
                printf("   TLC UsagePage=0x%04X Usage=0x%04X  ReportLen In=%u Out=%u Feat=%u\n",
                       caps.UsagePage, caps.Usage,
                       caps.InputReportByteLength, caps.OutputReportByteLength, caps.FeatureReportByteLength);
                std::set<UCHAR> inIds;
                dumpValueCaps(prep, HidP_Input, "IN", inIds);
                dumpButtonCaps(prep, HidP_Input, "IN", inIds);
                std::set<UCHAR> dummy;
                dumpValueCaps(prep, HidP_Feature, "FEAT", dummy);
                dumpButtonCaps(prep, HidP_Feature, "FEAT", dummy);

                // One Input report per report ID, fetched on demand (a control transfer, so it
                // does not wait for the device to push anything).
                if (caps.InputReportByteLength && rw) {
                    if (inIds.empty()) inIds.insert(0);
                    for (UCHAR id : inIds) {
                        std::vector<BYTE> rep(caps.InputReportByteLength, 0);
                        rep[0] = id;
                        if (HidD_GetInputReport(h, rep.data(), (ULONG)rep.size())) {
                            printf("   INPUT report id=0x%02X (%zu bytes):", id, rep.size());
                            size_t shown = rep.size() < kMaxDumpBytes ? rep.size() : kMaxDumpBytes;
                            for (size_t k = 0; k < shown; k++) printf(" %02X", rep[k]);
                            if (shown < rep.size()) printf(" ... (%zu more)", rep.size() - shown);
                            printf("\n");
                        } else {
                            printf("   HidD_GetInputReport id=0x%02X failed err=%lu\n", id, GetLastError());
                        }
                    }
                }
            }
            HidD_FreePreparsedData(prep);
        } else {
            printf("   HidD_GetPreparsedData failed err=%lu\n", GetLastError());
        }
        CloseHandle(h);
    }
    SetupDiDestroyDeviceInfoList(set);
    return 0;
}
