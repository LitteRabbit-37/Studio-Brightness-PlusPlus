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
};

/* ---------- Per-device state ---------- */
struct HidCaps {
	USHORT len   = 0;
	USAGE  page  = 0;
	USAGE  usage = 0;
	UCHAR  id    = 0;
};

struct DisplayDevice {
	HANDLE               hDev  = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA prep  = nullptr;
	HidCaps              featCaps;
	DisplayType          type  = DisplayType::None;
	std::wstring         name;
	std::wstring         devicePath;
	GUID                 containerId = {};

	// Per-device brightness state
	ULONG currentBrightness = 30000;
	ULONG baseBrightness    = 30000;
	ULONG minBrightness     = 1000;
	ULONG maxBrightness          = 60000;

	// Per-device ALS state
	float baseLux = 100.f;

	DisplayDevice() = default;
	~DisplayDevice() { close(); }

	// Move only (handles are not copyable)
	DisplayDevice(const DisplayDevice &)            = delete;
	DisplayDevice &operator=(const DisplayDevice &) = delete;
	DisplayDevice(DisplayDevice &&o) noexcept
	    : hDev(o.hDev), prep(o.prep), featCaps(o.featCaps),
	      type(o.type), name(std::move(o.name)), devicePath(std::move(o.devicePath)),
	      containerId(o.containerId), currentBrightness(o.currentBrightness),
	      baseBrightness(o.baseBrightness),
	      minBrightness(o.minBrightness), maxBrightness(o.maxBrightness), baseLux(o.baseLux) {
		o.hDev = INVALID_HANDLE_VALUE;
		o.prep = nullptr;
	}
	DisplayDevice &operator=(DisplayDevice &&o) noexcept {
		if (this != &o) {
			close();
			hDev = o.hDev; prep = o.prep;
			featCaps = o.featCaps;
			type = o.type; name = std::move(o.name); devicePath = std::move(o.devicePath);
			containerId = o.containerId;
			currentBrightness = o.currentBrightness; baseBrightness = o.baseBrightness;
			minBrightness = o.minBrightness; maxBrightness = o.maxBrightness;
			baseLux = o.baseLux;
			o.hDev = INVALID_HANDLE_VALUE;
			o.prep = nullptr;
		}
		return *this;
	}

	void  close();
	int   getBrightness(ULONG *val);
	int   setBrightness(ULONG val);
	int   getBrightnessRange(ULONG *mn, ULONG *mx);
	bool  isOpen() const { return hDev != INVALID_HANDLE_VALUE; }
};

/* ---------- Enumeration ---------- */
// Discovers all Apple displays with valid brightness HID caps.
// Returns a vector of opened, ready-to-use devices.
std::vector<DisplayDevice> hid_enumerate();

#endif
