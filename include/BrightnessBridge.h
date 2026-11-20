#ifndef BRIGHTNESS_BRIDGE_INCLUDED
#define BRIGHTNESS_BRIDGE_INCLUDED

#define WIN32_LEAN_AND_MEAN
#include <windows.h>

// Link to the optional SbppMonFilter driver (driver/), which gives the display a brightness
// in Windows' own stack: the Settings and Quick Settings sliders, and the WMI brightness API
// (Logi Options+ brightness keys among its callers) then drive the panel through the app.
// Without the driver installed this does nothing but retry quietly in the background.
namespace Bridge {

// Starts the bridge thread. Each level requested through Windows is posted to 'wnd' as
// 'msg', wParam = percent (0..100), lParam = request sequence for MarkApplied.
void Start(HWND wnd, UINT msg);
void Stop();

// Non-blocking and thread-safe. Records the active display's real level for the driver,
// after any change from any source.
void ReportLevel(int percent, bool adjustable);

// Called once a posted request has been applied (or refused), before the follow-up report.
void MarkApplied(unsigned long sequence);

// Non-blocking and thread-safe. The active display changed for a reason Windows did not
// cause (auto-brightness, tray slider, hotkeys, a display arriving). Once the change has
// settled, the bridge tells Windows the new level, so its slider - and the level callers
// such as Logi Options+ step from - stay true.
void NoteLocalChange(int percent);

// True when a request from Windows is only the echo of a level the bridge just told it;
// the caller acknowledges it without applying (applying would re-anchor auto-brightness).
bool IsSyncEcho(int percent);

} // namespace Bridge

#endif
