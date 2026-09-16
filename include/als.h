//----------------  als.h  ----------------
// Raw-HID ambient light sensor for Apple displays, reimplemented from the path used
// by BootCampService.exe (class HidAlsSensor): open the HID interface, poll
// HidD_GetInputReport, read HID Usage Page 0x20 (Sensors) / Usage 0x04D1 (Illuminance).
//
// This is the same technique as orientation.cpp, and is the source Boot Camp uses for
// its "Automatic Brightness". It is independent of the Windows Sensor API (ISensor),
// which never instantiates for displays whose ALS interface is claimed by Apple's null
// driver or rejected by hidclass.
#ifndef ALS_INCLUDED
#define ALS_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <hidsdi.h>
#include <hidpi.h>
#include <vector>
#include <string>
#include <climits>
#include <cstdint>

constexpr USAGE HID_UP_SENSOR_PAGE  = 0x0020; // Sensors
constexpr USAGE HID_USG_ILLUMINANCE = 0x04D1; // Data Field: Illuminance
constexpr USAGE HID_USG_COLOR_TEMP  = 0x04D2; // Data Field: Light Color Temperature (Kelvin)
constexpr USAGE HID_USG_CHROMA_X    = 0x04D4; // Data Field: Light Chromaticity X
constexpr USAGE HID_USG_CHROMA_Y    = 0x04D5; // Data Field: Light Chromaticity Y

/* One ambient sample. Illuminance, CCT and CIE chromaticity all live in the same input
   report. hasColour is false on a panel that only exposes illuminance. */
struct AmbientReading {
	float lux        = 0.f;   // lux, unit exponent applied
	float colorTemp  = 0.f;   // Kelvin
	float chromaX    = 0.f;   // CIE 1931 x
	float chromaY    = 0.f;   // CIE 1931 y
	bool  hasColour  = false;
};

struct AlsDevice {
	HANDLE               hDev        = INVALID_HANDLE_VALUE;
	PHIDP_PREPARSED_DATA prep        = nullptr;
	USHORT               inputLen    = 0;
	UCHAR                reportId    = 0;
	USAGE                uIllum = 0, uTemp = 0, uChromaX = 0, uChromaY = 0;
	// HID unit exponents as declared by the descriptor, in raw 4-bit form (see unitScale).
	ULONG                eIllum = 0, eTemp = 0, eChromaX = 0, eChromaY = 0;
	bool                 overlapped  = false;
	GUID                 containerId = {};
	std::wstring         devicePath;
	std::wstring         label;                 // short "pid_xxxx&mi_xx" tag for logging
	LONG                 lastRaw     = LONG_MIN;
	DWORD                lastLogTick = 0;
	int                  failures    = 0;   // consecutive failed reads; drives re-enumeration

	AlsDevice() = default;
	~AlsDevice() { close(); }
	AlsDevice(const AlsDevice &)            = delete;
	AlsDevice &operator=(const AlsDevice &) = delete;
	AlsDevice(AlsDevice &&o) noexcept { moveFrom(o); }
	AlsDevice &operator=(AlsDevice &&o) noexcept { if (this != &o) { close(); moveFrom(o); } return *this; }

	bool isOpen() const { return hDev != INVALID_HANDLE_VALUE; }
	void close();
	bool readReport(std::vector<uint8_t> &buf);
	// rawOut gets the logical illuminance (change detection, logging); out gets the values
	// with unit exponents applied. False if no report could be read.
	bool readAmbient(LONG *rawOut, AmbientReading *out);

private:
	void moveFrom(AlsDevice &o);
};

/* Discover + open every Apple display interface exposing an Illuminance input.
   Logs every page-0x20 input cap it sees (so a hidden/absent ALS is visible). */
std::vector<AlsDevice> als_enumerate();

/* Watcher: init once (UI thread), tick from a ~250 ms WM_TIMER, shutdown at exit.
   Logs raw lux only when it changes. */
void als_watch_init();
void als_watch_tick();
void als_watch_shutdown();

/* Thread-safe accessor for the auto-brightness path (worker thread). containerId picks a
   display, null takes the first available. False when there is no reading or the last one is
   older than kAlsMaxAgeMs, which is what lets the Sensor-API path take back over. */
constexpr DWORD kAlsMaxAgeMs = 3000;
bool als_get_lux(const GUID *containerId, float *lux);

/* Same, but returns the full sample including the colour fields. */
bool als_get_ambient(const GUID *containerId, AmbientReading *out);

#endif // ALS_INCLUDED
