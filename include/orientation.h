//----------------  orientation.h  ----------------
// Studio Display XDR (PID 0x1116) MI_09 = HID Device Orientation sensor
// (top-level Usage Page 0x20 / Usage 0x8A). It exposes three inclinometer tilt
// angles as Input values, each 0..360 degrees:
// Usage 0x047F = Tilt X, 0x0480 = Tilt Y, 0x0481 = Tilt Z
// We read all three (for diagnostics) and drive rotation from Tilt Y (0x0480).
#ifndef ORIENTATION_INCLUDED
#define ORIENTATION_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <vector>
#include <string>
#include <climits>
#include <cstdint>

/* HID Sensors usage constants (USB-IF HID Sensor Usage Tables) */
constexpr USAGE HID_UP_SENSOR         = 0x0020;
constexpr USAGE HID_USG_ORIENT_TILT_X = 0x047F;
constexpr USAGE HID_USG_ORIENT_TILT_Y = 0x0480;
constexpr USAGE HID_USG_ORIENT_TILT_Z = 0x0481;

struct OrientationDevice {
	HANDLE               hDev        = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA prep        = nullptr;
	USHORT               inputLen    = 0;
	UCHAR                reportId    = 0;
	USAGE                uX = 0, uY = 0, uZ = 0; // tilt usages present (0 if absent)
	bool                 overlapped  = false;
	GUID                 containerId = {};
	std::wstring         devicePath;
	std::wstring         gdiName; // \\.\DISPLAYx (resolved lazily)
	LONG                 lastX = LONG_MIN, lastY = LONG_MIN, lastZ = LONG_MIN;

	OrientationDevice() = default;
	~OrientationDevice() { close(); }
	OrientationDevice(const OrientationDevice &)            = delete;
	OrientationDevice &operator=(const OrientationDevice &) = delete;
	OrientationDevice(OrientationDevice &&o) noexcept { moveFrom(o); }
	OrientationDevice &operator=(OrientationDevice &&o) noexcept { if (this != &o) { close(); moveFrom(o); } return *this; }

	bool isOpen() const { return hDev != INVALID_HANDLE_VALUE; }
	void close();

	// Fetch one input report (GetInputReport, else overlapped ReadFile).
	bool readReport(std::vector<uint8_t> &buf);
	// Read the three tilt angles from one report. Missing axes come back as LONG_MIN.
	// Returns true if the report was read.
	bool readTilt(LONG *x, LONG *y, LONG *z);

private:
	void moveFrom(OrientationDevice &o);
};

/* Discover + open every Apple display interface carrying a Device-Orientation sensor.
   Logs every Input value cap it sees (diagnostic). */
std::vector<OrientationDevice> orient_enumerate();

/* Map a Tilt-Y angle (0..360 deg) to a Windows DMDO_* rotation. See the calibration
   constants in the .cpp if rotation goes the wrong way. */
int orient_angle_to_dmdo(LONG tiltYdeg);

/* Apply a rotation to a GDI display ("\\.\DISPLAYx"). */
bool orient_apply_rotation(const std::wstring &gdiName, int dmdo);

/* Resolve "\\.\DISPLAYx" for this sensor: by ContainerId first, then by Apple EDID
   (first active non-internal Apple display). Empty on failure. */
std::wstring orient_resolve_display(const OrientationDevice &d);

/* Enable/disable auto-rotation at runtime (driven by the Options / tray setting).
   Enabling re-applies the current physical orientation on the next tick. */
void orient_set_enabled(bool enabled);

/* Convenience watcher: init once (UI thread), tick from a ~250 ms WM_TIMER, shutdown at exit. */
void orient_watch_init();
void orient_watch_tick();
void orient_watch_shutdown();

#endif // ORIENTATION_INCLUDED
