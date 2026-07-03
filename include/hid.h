#ifndef HID_INCLUDED
#define HID_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <vector>
#include <string>
#include <cstdint>

/* ---------- Display types ---------- */
enum class DisplayType {
	None,
	StudioDisplay,    // PID 0x1114
	StudioDisplay2,   // PID 0x1118  (Gen 2)
	StudioXDR,        // PID 0x1116  (Studio Display XDR)
	ProXDR,           // PID 0x9243  (Pro Display XDR)
	AppleGeneric      // Unknown PID, but valid HID brightness caps
};

/* ---------- Display profile ---------- */
struct DisplayProfile {
	uint16_t     pid;
	DisplayType  type;
	const wchar_t *name;
	float        maxNits;
};

/* ---------- Per-device state ---------- */
struct HidCaps {
	USHORT len   = 0;
	USAGE  page  = 0;
	USAGE  usage = 0;
	UCHAR  id    = 0;
};

/* ---------- Color preset (Apple Reference Mode) ---------- */
struct ColorPreset {
	uint32_t     index;   // hardware index written to 0xFF20/0x03 to select
	std::wstring name;    // UTF-16 name read from 0xFF20/0x08
	std::wstring desc;    // UTF-16 secondary string read from 0xFF20/0x09 (Boot Camp reads it too)

	// The factory "Apple ..." presets are the general-use modes: brightness stays adjustable and,
	// on the XDR panels, they are the only presets compatible with Windows HDR (anything else can
	// blank the panel). Everything else is a fixed-calibration reference mode; macOS locks
	// brightness on those as well. The HID protocol carries no per-preset flag (verified against
	// Boot Camp, which only reads name and desc), so classify by name.
	bool isHdrCompatible() const { return name.rfind(L"Apple XDR Display", 0) == 0; }
	bool allowsBrightness() const {
		return name.rfind(L"Apple XDR Display", 0) == 0 || name.rfind(L"Apple Display", 0) == 0;
	}
};

struct DisplayDevice {
	HANDLE               hDev  = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA prep  = nullptr;
	HidCaps              featCaps;
	DisplayType          type  = DisplayType::None;
	std::wstring         name;
	std::wstring         devicePath;
	GUID                 containerId = {};

	// Color preset (0xFF20) interface: same physical display, different HID interface
	HANDLE                   hPreset           = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA     presetPrep        = nullptr;
	USHORT                   presetReportLen   = 0;
	long                     presetCursorMax   = 0;
	std::vector<ColorPreset> presets;
	int                      activePresetIndex = -1;

	// Per-device brightness state
	ULONG currentBrightness = 30000;
	ULONG baseBrightness    = 30000;
	ULONG minBrightness     = 1000;
	ULONG maxBrightness          = 60000;

	// Per-device ALS state
	float baseLux = 100.f;

	// Auto-brightness ramp/hysteresis (Apple-style)
	float  lastTargetLux  = 0.f;
	ULONG  rampStart      = 0;
	ULONG  rampGoal       = 0;
	double rampStartMs    = 0.0;
	double rampDurationMs = 0.0;

	// Nit calibration for proportional brightness matching
	float maxNits = 600.f;

	DisplayDevice() = default;
	~DisplayDevice() { close(); }

	// Move only (handles are not copyable)
	DisplayDevice(const DisplayDevice &)            = delete;
	DisplayDevice &operator=(const DisplayDevice &) = delete;
	DisplayDevice(DisplayDevice &&o) noexcept
	    : hDev(o.hDev), prep(o.prep), featCaps(o.featCaps),
	      type(o.type), name(std::move(o.name)), devicePath(std::move(o.devicePath)),
	      containerId(o.containerId),
	      hPreset(o.hPreset), presetPrep(o.presetPrep), presetReportLen(o.presetReportLen),
	      presetCursorMax(o.presetCursorMax), presets(std::move(o.presets)),
	      activePresetIndex(o.activePresetIndex),
	      currentBrightness(o.currentBrightness), baseBrightness(o.baseBrightness),
	      minBrightness(o.minBrightness), maxBrightness(o.maxBrightness), baseLux(o.baseLux),
	      lastTargetLux(o.lastTargetLux), rampStart(o.rampStart), rampGoal(o.rampGoal),
	      rampStartMs(o.rampStartMs), rampDurationMs(o.rampDurationMs),
	      maxNits(o.maxNits) {
		o.hDev = INVALID_HANDLE_VALUE;
		o.prep = nullptr;
		o.hPreset = INVALID_HANDLE_VALUE;
		o.presetPrep = nullptr;
	}
	DisplayDevice &operator=(DisplayDevice &&o) noexcept {
		if (this != &o) {
			close();
			hDev = o.hDev; prep = o.prep;
			featCaps = o.featCaps;
			type = o.type; name = std::move(o.name); devicePath = std::move(o.devicePath);
			containerId = o.containerId;
			hPreset = o.hPreset; presetPrep = o.presetPrep;
			presetReportLen = o.presetReportLen; presetCursorMax = o.presetCursorMax;
			presets = std::move(o.presets); activePresetIndex = o.activePresetIndex;
			currentBrightness = o.currentBrightness; baseBrightness = o.baseBrightness;
			minBrightness = o.minBrightness; maxBrightness = o.maxBrightness;
			baseLux = o.baseLux; maxNits = o.maxNits;
			lastTargetLux = o.lastTargetLux; rampStart = o.rampStart; rampGoal = o.rampGoal;
			rampStartMs = o.rampStartMs; rampDurationMs = o.rampDurationMs;
			o.hDev = INVALID_HANDLE_VALUE;
			o.prep = nullptr;
			o.hPreset = INVALID_HANDLE_VALUE;
			o.presetPrep = nullptr;
		}
		return *this;
	}

	void  close();
	int   getBrightness(ULONG *val);
	int   setBrightness(ULONG val);
	int   getBrightnessRange(ULONG *mn, ULONG *mx);
	bool  isOpen() const { return hDev != INVALID_HANDLE_VALUE; }

	// Color presets (0xFF20 interface)
	bool  hasPresets() const { return hPreset != INVALID_HANDLE_VALUE && !presets.empty(); }
	int   enumeratePresets();
	int   getActivePreset(int *outIdx);
	int   setActivePreset(int idx);

	// Name-based preset classification (see ColorPreset). All of these fail open: when the
	// naming scheme is unknown, nothing gets locked or filtered.
	const ColorPreset *activePreset() const;          // nullptr when unknown
	bool  presetsClassifiable() const;                // any preset matches the known Apple naming
	bool  activePresetLocksBrightness() const;        // macOS parity: reference modes fix brightness
	int   firstHdrCompatiblePreset() const;           // rescue target when HDR turns on (-1 = none)
};

/* ---------- Enumeration ---------- */
// Discovers all Apple displays with valid brightness HID caps.
// Returns a vector of opened, ready-to-use devices.
std::vector<DisplayDevice> hid_enumerate();

#endif
