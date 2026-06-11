//----------------  hid.cpp  ----------------
#include "hid.h"
#include "Log.h"
#define _WIN32_DCOM
#include <initguid.h>
#include <devpropdef.h>
#include <hidsdi.h>
#include <setupapi.h>
// SBPP_DEVPKEY_Device_ContainerId: {8c7ed206-3f8a-4827-b3ab-ae9e1faefc6c}, 2
DEFINE_DEVPROPKEY(SBPP_DEVPKEY_Device_ContainerId,
                  0x8c7ed206, 0x3f8a, 0x4827,
                  0xb3, 0xab, 0xae, 0x9e, 0x1f, 0xae, 0xfc, 0x6c, 2);
#include <shlwapi.h>
#include <vector>
#include <cstdint>

#pragma comment(lib, "setupapi.lib")
#pragma comment(lib, "shlwapi.lib")

/* ---------- Apple vendor ID ---------- */
static const wchar_t kAppleVid[]     = L"vid_05ac";
// kColFilter removed: some displays (e.g. Studio Display XDR) expose
// brightness via a HID collection, not the top-level interface.

inline bool icontains(const wchar_t *hay, const wchar_t *needle) {
	return StrStrIW(hay, needle) != nullptr;
}

/* ---------- Known display profiles ---------- */
static const DisplayProfile kProfiles[] = {
	{0x1114, DisplayType::StudioDisplay,  L"Studio Display",          600.f},
	{0x1118, DisplayType::StudioDisplay2, L"Studio Display (Gen 2)",  600.f},
	{0x1116, DisplayType::StudioXDR,      L"Studio Display XDR",     1000.f},
	{0x9243, DisplayType::ProXDR,         L"Pro Display XDR",        1000.f},
};

static const DisplayProfile *findProfileByPid(uint16_t pid) {
	for (const auto &p : kProfiles)
		if (p.pid == pid)
			return &p;
	return nullptr;
}

/* ---------- Extract PID from device path ---------- */
static uint16_t extractPid(const wchar_t *path) {
	const wchar_t *p = StrStrIW(path, L"pid_");
	if (!p)
		return 0;
	return (uint16_t)wcstoul(p + 4, nullptr, 16);
}

/* ---------- Query ContainerId via SetupAPI ---------- */
static GUID queryContainerId(HDEVINFO set, PSP_DEVINFO_DATA devInfo) {
	GUID cid   = {};
	DWORD type = 0, size = 0;

	SetupDiGetDevicePropertyW(set, devInfo, &SBPP_DEVPKEY_Device_ContainerId,
	                          &type, nullptr, 0, &size, 0);
	if (size == sizeof(GUID)) {
		SetupDiGetDevicePropertyW(set, devInfo, &SBPP_DEVPKEY_Device_ContainerId,
		                          &type, (PBYTE)&cid, sizeof(GUID), nullptr, 0);
	}
	return cid;
}

/* ============================================================ */
void DisplayDevice::close() {
	if (presetPrep) {
		HidD_FreePreparsedData(presetPrep);
		presetPrep = nullptr;
	}
	if (hPreset != INVALID_HANDLE_VALUE) {
		CloseHandle(hPreset);
		hPreset = INVALID_HANDLE_VALUE;
	}
	if (prep) {
		HidD_FreePreparsedData(prep);
		prep = nullptr;
	}
	if (hDev != INVALID_HANDLE_VALUE) {
		CloseHandle(hDev);
		hDev = INVALID_HANDLE_VALUE;
	}
}

int DisplayDevice::getBrightness(ULONG *val) {
	if (hDev == INVALID_HANDLE_VALUE || featCaps.len == 0)
		return -1;
	std::vector<uint8_t> buf(featCaps.len, 0);
	buf[0] = featCaps.id;
	if (!HidD_GetFeature(hDev, buf.data(), (ULONG)buf.size()))
		return -2;
	NTSTATUS s = HidP_GetUsageValue(HidP_Feature, featCaps.page, 0, featCaps.usage, val, prep,
	                                reinterpret_cast<PCHAR>(buf.data()), featCaps.len);
	return (s == HIDP_STATUS_SUCCESS) ? 0 : -3;
}

int DisplayDevice::setBrightness(ULONG v) {
	if (hDev == INVALID_HANDLE_VALUE || featCaps.len == 0)
		return -1;
	std::vector<uint8_t> buf(featCaps.len, 0);
	buf[0] = featCaps.id;
	if (!HidD_GetFeature(hDev, buf.data(), (ULONG)buf.size()))
		return -2;
	NTSTATUS s = HidP_SetUsageValue(HidP_Feature, featCaps.page, 0, featCaps.usage, v, prep,
	                                reinterpret_cast<PCHAR>(buf.data()), featCaps.len);
	if (s != HIDP_STATUS_SUCCESS)
		return -3;
	return HidD_SetFeature(hDev, buf.data(), featCaps.len) ? 0 : -4;
}

int DisplayDevice::getBrightnessRange(ULONG *mn, ULONG *mx) {
	if (!prep || hDev == INVALID_HANDLE_VALUE)
		return -1;
	// Use the stored featCaps (already resolved to the correct brightness cap)
	HIDP_CAPS caps{};
	if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS)
		return -2;
	USHORT numVals = caps.NumberFeatureValueCaps;
	if (numVals == 0) return -2;
	std::vector<HIDP_VALUE_CAPS> vcaps(numVals);
	if (HidP_GetValueCaps(HidP_Feature, vcaps.data(), &numVals, prep) != HIDP_STATUS_SUCCESS)
		return -2;
	for (USHORT i = 0; i < numVals; ++i) {
		USAGE u = vcaps[i].IsRange ? vcaps[i].Range.UsageMin : vcaps[i].NotRange.Usage;
		if (vcaps[i].UsagePage == featCaps.page && u == featCaps.usage) {
			*mn = vcaps[i].LogicalMin;
			*mx = vcaps[i].LogicalMax;
			return 0;
		}
	}
	return -2;
}

/* ---------- Color presets (0xFF20 vendor interface) ---------- */
static bool presetHasFeatureUsage(PHIDP_PREPARSED_DATA prep, USAGE page, USAGE usage, long *logMax) {
	HIDP_CAPS caps{};
	if (HidP_GetCaps(prep, &caps) != HIDP_STATUS_SUCCESS)
		return false;
	USHORT n = caps.NumberFeatureValueCaps;
	if (!n)
		return false;
	std::vector<HIDP_VALUE_CAPS> v(n);
	if (HidP_GetValueCaps(HidP_Feature, v.data(), &n, prep) != HIDP_STATUS_SUCCESS)
		return false;
	for (auto &c : v) {
		USAGE u = c.IsRange ? c.Range.UsageMin : c.NotRange.Usage;
		if (c.UsagePage == page && u == usage) {
			if (logMax)
				*logMax = c.LogicalMax;
			return true;
		}
	}
	return false;
}

int DisplayDevice::enumeratePresets() {
	presets.clear();
	if (hPreset == INVALID_HANDLE_VALUE || !presetPrep)
		return -1;
	long lm = 0;
	presetHasFeatureUsage(presetPrep, 0xFF20, 0x04, &lm);
	presetCursorMax = lm;
	long bound = (lm > 0 && lm <= 128) ? lm : 64;
	for (long i = 0; i < bound; ++i) {
		// Write the enumeration cursor (0xFF20/0x04). This usage is write-only: build a clean, zeroed
		// report and write it directly, with NO read-modify-write. Boot Camp does the same
		// (sub_140007EC0). The XDR (PID 0x1116) stalls a GET_REPORT on the cursor report, so a prior
		// HidD_GetFeature aborts enumeration there (the Gen 1 tolerated the read). NON-destructive:
		// the active preset (0x03) is untouched.
		std::vector<uint8_t> wr(presetReportLen, 0);
		wr[0] = 0x04;
		if (HidP_SetUsageValue(HidP_Feature, 0xFF20, 0, 0x04, (ULONG)i, presetPrep,
		                       reinterpret_cast<PCHAR>(wr.data()), (ULONG)wr.size()) != HIDP_STATUS_SUCCESS)
			break;
		if (!HidD_SetFeature(hPreset, wr.data(), (ULONG)wr.size()))
			break;
		// Read the cursor preset's validity (0xFF20/0x06) and name (0xFF20/0x08) from report 0x05.
		std::vector<uint8_t> r5(presetReportLen, 0);
		r5[0] = 0x05;
		if (!HidD_GetFeature(hPreset, r5.data(), (ULONG)r5.size()))
			break;
		ULONG valid = 0;
		HidP_GetUsageValue(HidP_Feature, 0xFF20, 0, 0x06, &valid, presetPrep,
		                   reinterpret_cast<PCHAR>(r5.data()), (ULONG)r5.size());
		if (!valid)
			break;
		std::vector<uint8_t> nameBuf(256, 0);
		HidP_GetUsageValueArray(HidP_Feature, 0xFF20, 0, 0x08, reinterpret_cast<PCHAR>(nameBuf.data()),
		                        (USHORT)nameBuf.size(), presetPrep,
		                        reinterpret_cast<PCHAR>(r5.data()), (ULONG)r5.size());
		const wchar_t *wp = reinterpret_cast<const wchar_t *>(nameBuf.data());
		size_t nlen = 0;
		while (nlen < 128 && wp[nlen])
			++nlen;
		presets.push_back({(uint32_t)i, std::wstring(wp, nlen)});
	}
	Log::Info(L"  Presets enumerated for %s: %zu", name.c_str(), presets.size());
	return 0;
}

int DisplayDevice::getActivePreset(int *outIdx) {
	if (hPreset == INVALID_HANDLE_VALUE || !presetPrep)
		return -1;
	std::vector<uint8_t> r3(presetReportLen, 0);
	r3[0] = 0x03;
	if (!HidD_GetFeature(hPreset, r3.data(), (ULONG)r3.size()))
		return -2;
	ULONG v = 0;
	if (HidP_GetUsageValue(HidP_Feature, 0xFF20, 0, 0x03, &v, presetPrep,
	                       reinterpret_cast<PCHAR>(r3.data()), (ULONG)r3.size()) != HIDP_STATUS_SUCCESS)
		return -3;
	activePresetIndex = (int)v;
	if (outIdx)
		*outIdx = (int)v;
	return 0;
}

int DisplayDevice::setActivePreset(int idx) {
	if (hPreset == INVALID_HANDLE_VALUE || !presetPrep)
		return -1;
	// Clean zeroed write-only report, matching Boot Camp's sub_140007EC0 (no read-modify-write,
	// which the XDR's preset reports reject).
	std::vector<uint8_t> r3(presetReportLen, 0);
	r3[0] = 0x03;
	if (HidP_SetUsageValue(HidP_Feature, 0xFF20, 0, 0x03, (ULONG)idx, presetPrep,
	                       reinterpret_cast<PCHAR>(r3.data()), (ULONG)r3.size()) != HIDP_STATUS_SUCCESS)
		return -3;
	if (!HidD_SetFeature(hPreset, r3.data(), (ULONG)r3.size()))
		return -4;
	activePresetIndex = idx;
	return 0;
}

/* ============================================================ */
std::vector<DisplayDevice> hid_enumerate() {
	struct Candidate {
		DisplayDevice dev;
		bool exactMatch;
	};
	std::vector<Candidate> candidates;
	std::vector<DisplayDevice> result;

	// 0xFF20 color-preset interfaces (same display as brightness, matched later by ContainerId)
	struct PresetIface {
		std::wstring         path;
		GUID                 containerId{};
		HANDLE               h         = INVALID_HANDLE_VALUE;
		PHIDP_PREPARSED_DATA prep      = nullptr;
		USHORT               reportLen = 0;
	};
	std::vector<PresetIface> presetIfaces;

	GUID hidGuid;
	HidD_GetHidGuid(&hidGuid);
	HDEVINFO set = SetupDiGetClassDevsW(&hidGuid, nullptr, 0,
	                                    DIGCF_PRESENT | DIGCF_DEVICEINTERFACE);
	if (set == INVALID_HANDLE_VALUE) {
		Log::Error(L"SetupDiGetClassDevsW failed (%lu)", GetLastError());
		return result;
	}

	// Note: this function may be called periodically; keep logs minimal for known-empty scans

	SP_DEVICE_INTERFACE_DATA ifd{sizeof(ifd)};

	for (DWORD i = 0; SetupDiEnumDeviceInterfaces(set, nullptr, &hidGuid, i, &ifd); ++i) {
		DWORD need = 0;
		SetupDiGetDeviceInterfaceDetailW(set, &ifd, nullptr, 0, &need, nullptr);
		if (!need)
			continue;

		std::vector<BYTE> buf(need);
		auto det       = reinterpret_cast<PSP_DEVICE_INTERFACE_DETAIL_DATA_W>(buf.data());
		det->cbSize    = sizeof(SP_DEVICE_INTERFACE_DETAIL_DATA_W);

		SP_DEVINFO_DATA devInfo{sizeof(devInfo)};
		if (!SetupDiGetDeviceInterfaceDetailW(set, &ifd, det, need, nullptr, &devInfo))
			continue;

		const wchar_t *path = det->DevicePath;

		// Filter: Apple VID only
		if (!icontains(path, kAppleVid))
			continue;

		uint16_t pid = extractPid(path);
		const DisplayProfile *profile = findProfileByPid(pid);

		if (profile) {
			Log::Info(L"Found %s (PID 0x%04X): %s", profile->name, pid, path);
		} else {
			Log::Info(L"Found unknown Apple HID (PID 0x%04X): %s", pid, path);
		}

		// Open device
		DisplayDevice dev;
		dev.devicePath = path;
		dev.hDev = CreateFileW(path, GENERIC_READ | GENERIC_WRITE,
		                       FILE_SHARE_READ | FILE_SHARE_WRITE, nullptr,
		                       OPEN_EXISTING, 0, nullptr);
		if (dev.hDev == INVALID_HANDLE_VALUE) {
			Log::Warn(L"  CreateFile failed (%lu), skipping", GetLastError());
			continue;
		}

		if (!HidD_GetPreparsedData(dev.hDev, &dev.prep)) {
			Log::Warn(L"  GetPreparsedData failed, skipping");
			dev.close();
			continue;
		}

		HIDP_CAPS caps{};
		if (HidP_GetCaps(dev.prep, &caps) != HIDP_STATUS_SUCCESS) {
			Log::Warn(L"  GetCaps failed, skipping");
			dev.close();
			continue;
		}

		dev.featCaps.len = caps.FeatureReportByteLength;

		// Enumerate all Feature value caps and find the brightness control
		USHORT numFeatVals = caps.NumberFeatureValueCaps;
		if (numFeatVals == 0) {
			Log::Info(L"  No Feature value caps, skipping");
			dev.close();
			continue;
		}
		std::vector<HIDP_VALUE_CAPS> vcaps(numFeatVals);
		NTSTATUS vcStatus = HidP_GetValueCaps(HidP_Feature, vcaps.data(), &numFeatVals, dev.prep);
		if (vcStatus != HIDP_STATUS_SUCCESS) {
			Log::Warn(L"  HidP_GetValueCaps failed (0x%08X), skipping", (unsigned)vcStatus);
			dev.close();
			continue;
		}

		// Log all Feature value caps for diagnostics
		for (USHORT vi = 0; vi < numFeatVals; ++vi) {
			auto &vc = vcaps[vi];
			USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
			Log::Info(L"  ValueCap[%u]: Page=0x%04X Usage=0x%04X ReportID=0x%02X BitSize=%u ReportCount=%u LogMin=%ld LogMax=%ld",
			          vi, vc.UsagePage, u, vc.ReportID, vc.BitSize, vc.ReportCount,
			          vc.LogicalMin, vc.LogicalMax);
		}

		// Divert the 0xFF20 vendor (color preset) interface: same display, no brightness cap.
		// Capture it here before the brightness-cap check would drop it; attach by ContainerId later.
		bool isPresetIface = false;
		for (USHORT vi = 0; vi < numFeatVals; ++vi)
			if (vcaps[vi].UsagePage == 0xFF20) {
				isPresetIface = true;
				break;
			}
		if (isPresetIface) {
			PresetIface pf;
			pf.path        = path;
			pf.h           = dev.hDev;
			pf.prep        = dev.prep;
			pf.reportLen   = caps.FeatureReportByteLength;
			pf.containerId = queryContainerId(set, &devInfo);
			dev.hDev = INVALID_HANDLE_VALUE; // transfer ownership; keep dev.close() from freeing them
			dev.prep = nullptr;
			Log::Info(L"  FF20 color-preset interface (PID 0x%04X), deferring for ContainerId attach", pid);
			presetIfaces.push_back(std::move(pf));
			continue;
		}

		// Search for brightness: UsagePage 0x0082 (Monitor), Usage 0x0010 (Brightness)
		// Fallback: any cap with ReportCount==1 and reasonable LogicalMax (>= 400)
		int brightIdx = -1;
		int fallbackIdx = -1;
		for (USHORT vi = 0; vi < numFeatVals; ++vi) {
			auto &vc = vcaps[vi];
			USAGE u = vc.IsRange ? vc.Range.UsageMin : vc.NotRange.Usage;
			if (vc.UsagePage == 0x0082 && u == 0x0010) {
				brightIdx = vi;
				break;
			}
			if (fallbackIdx < 0 && vc.ReportCount == 1 && vc.LogicalMax >= 400)
				fallbackIdx = vi;
		}
		int chosen = (brightIdx >= 0) ? brightIdx : fallbackIdx;
		if (chosen < 0) {
			Log::Warn(L"  No brightness value cap found among %u caps, skipping", numFeatVals);
			dev.close();
			continue;
		}
		bool isExact = (brightIdx >= 0);
		if (isExact) {
			Log::Info(L"  Matched brightness cap by UsagePage/Usage (index %d)", chosen);
		} else {
			Log::Info(L"  Using fallback cap (index %d), no exact brightness match", chosen);
		}

		auto &bc = vcaps[chosen];
		dev.featCaps.id    = bc.ReportID;
		dev.featCaps.page  = bc.UsagePage;
		dev.featCaps.usage = bc.IsRange ? bc.Range.UsageMin : bc.NotRange.Usage;

		// Assign type and name
		if (profile) {
			dev.type    = profile->type;
			dev.name    = profile->name;
			dev.maxNits = profile->maxNits;
		} else {
			dev.type    = DisplayType::AppleGeneric;
			dev.name    = L"Apple Display (Unknown)";
			dev.maxNits = 600.f;
			Log::Warn(L"  PID 0x%04X not in profiles, using generic mode", pid);
		}

		// Query ContainerId
		dev.containerId = queryContainerId(set, &devInfo);

		Log::Info(L"  Candidate: %s [Feature ID=0x%02X, %s]",
		          dev.name.c_str(), dev.featCaps.id, isExact ? L"exact" : L"fallback");

		candidates.push_back({std::move(dev), isExact});
	}

	SetupDiDestroyDeviceInfoList(set);

	// Pass 2: per ContainerId, prefer exact match over fallback
	static const GUID emptyGuid = {};
	for (auto &c : candidates) {
		GUID &cid = c.dev.containerId;
		bool hasContainer = (memcmp(&cid, &emptyGuid, sizeof(GUID)) != 0);

		if (hasContainer) {
			// Check if we already accepted a device with this ContainerId
			bool dominated = false;
			for (size_t ri = 0; ri < result.size(); ++ri) {
				if (memcmp(&result[ri].containerId, &cid, sizeof(GUID)) != 0)
					continue;
				// Same ContainerId: keep the exact match
				if (c.exactMatch && !result[ri].featCaps.page) {
					// Shouldn't happen, but safety
				} else if (c.exactMatch) {
					// New candidate is exact, existing is fallback: replace
					Log::Info(L"  Replacing fallback with exact match for %s", c.dev.name.c_str());
					result[ri] = std::move(c.dev);
				} else {
					// Existing is better or equal, skip this one
					Log::Info(L"  Duplicate ContainerId, skipping %s (better candidate already accepted)",
					          c.dev.name.c_str());
					c.dev.close();
				}
				dominated = true;
				break;
			}
			if (dominated) continue;

			// Check if a later candidate for the same ContainerId has an exact match
			bool laterExact = false;
			if (!c.exactMatch) {
				for (auto &other : candidates) {
					if (&other == &c || !other.exactMatch) continue;
					if (memcmp(&other.dev.containerId, &cid, sizeof(GUID)) == 0) {
						laterExact = true;
						break;
					}
				}
			}
			if (laterExact) {
				Log::Info(L"  Skipping fallback for %s (exact match available)", c.dev.name.c_str());
				c.dev.close();
				continue;
			}
		}

		Log::Info(L"  Opened: %s [Feature ID=0x%02X]",
		          c.dev.name.c_str(), c.dev.featCaps.id);
		result.push_back(std::move(c.dev));
	}

	// Pass 3: attach each 0xFF20 preset interface to its display by ContainerId
	static const GUID emptyGuidP = {};
	for (auto &pf : presetIfaces) {
		bool attached = false;
		for (auto &dd : result) {
			if (memcmp(&dd.containerId, &emptyGuidP, sizeof(GUID)) == 0)
				continue;
			if (memcmp(&dd.containerId, &pf.containerId, sizeof(GUID)) != 0)
				continue;
			if (dd.hPreset != INVALID_HANDLE_VALUE)
				continue;
			dd.hPreset         = pf.h;
			dd.presetPrep      = pf.prep;
			dd.presetReportLen = pf.reportLen;
			pf.h    = INVALID_HANDLE_VALUE;
			pf.prep = nullptr;
			Log::Info(L"  Attached FF20 preset interface to %s", dd.name.c_str());
			attached = true;
			break;
		}
		if (!attached) {
			if (pf.prep)
				HidD_FreePreparsedData(pf.prep);
			if (pf.h != INVALID_HANDLE_VALUE)
				CloseHandle(pf.h);
		}
	}

	if (!result.empty())
		Log::Info(L"Enumeration complete: %zu display(s) found", result.size());
	return result;
}
