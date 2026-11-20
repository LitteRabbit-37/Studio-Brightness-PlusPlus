#include "BrightnessBridge.h"

#include <winioctl.h>
#include <powrprof.h>
#include <algorithm>
#include <atomic>
#include <thread>

#include "sbpp_bridge.h"
#include "Log.h"

#pragma comment(lib, "powrprof.lib")

// One thread owns the driver handle. It pends IOCTL_SBPP_WAIT_REQUEST and posts each
// request to the UI thread, which applies it like a slider move. Reports go the other way:
// ReportLevel only stores the latest level and sets an event, so it is safe to call with
// g_displayMutex held from any thread; the bridge thread sends whatever is newest.

namespace {

std::thread                g_thread;
HANDLE                     g_stop  = nullptr;  // manual reset
HANDLE                     g_dirty = nullptr;  // auto reset: a report is waiting
HWND                       g_wnd   = nullptr;
UINT                       g_msg   = 0;
std::atomic<long>          g_percent{-1};      // -1: nothing known yet
std::atomic<bool>          g_adjustable{true};
std::atomic<unsigned long> g_applied{0};

// Telling Windows about local changes. With the driver in ACPI mode, Windows' own brightness
// stack (monitor.sys + the Display Enhancement Service) owns the level its slider shows; a
// legacy IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS on \\.\LCD (open to any user) updates it. Windows
// then applies it - which comes back here as a request, recognised as an echo.
constexpr DWORD     kIoctlVideoSetDisplayBrightness = 0x0023049C; // CTL_CODE(FILE_DEVICE_VIDEO, 0x127, METHOD_BUFFERED, FILE_ANY_ACCESS)
constexpr ULONGLONG kSyncSettleMs  = 600;    // one update per settled change, not per ramp step
constexpr ULONGLONG kEchoWindowMs  = 3000;
HANDLE                     g_sync  = nullptr;  // auto reset: a local change was noted
std::atomic<long>          g_syncPct{-1};      // level waiting to be told to Windows
std::atomic<ULONGLONG>     g_syncDue{0};
std::atomic<long>          g_windowsPct{-1};   // last level Windows is known to hold
std::atomic<long>          g_sentPct{-1};      // last level we told Windows
std::atomic<ULONGLONG>     g_sentAt{0};
std::atomic<bool>          g_connected{false};

// Requests the driver never sees. A brightness set through WmiSetBrightness (Logi Options+,
// scripts) or the power plan changes the plan's display brightness; the Display Enhancement
// Service then applies it - unless the value equals one it applied itself in the last 1-3 s,
// which it takes for the echo of its own write and drops. Reversing direction with brightness
// keys hits exactly that (42 -> 32 -> 42), so the press is lost and Windows and the panel
// disagree. The plan setting itself always changes, so the bridge watches it too and applies
// what the service dropped. When the service does apply it, the driver's request and this one
// carry the same level, and the second finds the panel already there.
// GUID_VIDEO_NORMAL_LEVEL, the "Display brightness" power setting.
constexpr GUID   kPlanBrightness = {0xaded5e82, 0xb909, 0x4619, {0x99, 0x49, 0xf5, 0xd7, 0x1d, 0xac, 0x0b, 0xcb}};
HPOWERNOTIFY             g_planNotify = nullptr;
std::atomic<long>        g_planPct{-1};        // last plan value seen; -1 until the first report

ULONG CALLBACK onPlanBrightness(PVOID, ULONG type, PVOID setting) {
	if (type != PBT_POWERSETTINGCHANGE || !setting) return 0;
	const auto *s = static_cast<const POWERBROADCAST_SETTING *>(setting);
	if (s->DataLength < sizeof(DWORD) || !IsEqualGUID(s->PowerSetting, kPlanBrightness)) return 0;
	long pct  = (long)std::min<DWORD>(*reinterpret_cast<const DWORD *>(s->Data), 100);
	long prev = g_planPct.exchange(pct);
	if (prev < 0 || pct == prev) return 0;   // the report on registering, or no change
	if (!g_connected.load()) return 0;       // without the driver Windows does not own the level
	if (pct == g_sentPct.load() && GetTickCount64() - g_sentAt.load() < kEchoWindowMs)
		return 0;                            // our own tellWindows coming back
	g_windowsPct.store(pct);
	g_syncPct.store(-1);                     // newer than any local change still settling
	PostMessageW(g_wnd, g_msg, (WPARAM)pct, 0);   // sequence 0: not a driver request
	return 0;
}

bool tellWindows(long pct) {
	HANDLE lcd = CreateFileW(L"\\\\.\\LCD", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
	                         nullptr, OPEN_EXISTING, 0, nullptr);
	if (lcd == INVALID_HANDLE_VALUE) return false;
	UCHAR in[3] = {3 /* DISPLAYPOLICY_BOTH */, (UCHAR)pct, (UCHAR)pct};
	DWORD got   = 0;
	BOOL  ok    = DeviceIoControl(lcd, kIoctlVideoSetDisplayBrightness, in, sizeof(in), nullptr, 0, &got, nullptr);
	CloseHandle(lcd);
	if (ok) {
		g_sentAt.store(GetTickCount64());
		g_sentPct.store(pct);
		g_windowsPct.store(pct);
	}
	return ok != FALSE;
}

// Sends a settled local change, if one is due. Returns the ms until the next one is due,
// or INFINITE when nothing is pending.
DWORD syncIfDue() {
	long pct = g_syncPct.load();
	if (pct < 0) return INFINITE;
	ULONGLONG now = GetTickCount64(), due = g_syncDue.load();
	if (now < due) return (DWORD)(due - now);
	if (!g_syncPct.compare_exchange_strong(pct, -1)) return 0;   // changed meanwhile: re-check
	pct = g_percent.load();   // the level now, not the one noted (it may have moved since)
	if (pct >= 0 && pct != g_windowsPct.load()) {
		if (!tellWindows(pct))
			Log::Warn(L"Brightness bridge: telling Windows %ld%% failed (%lu)", pct, GetLastError());
	}
	return INFINITE;
}

bool sendReport(HANDLE dev) {
	long pct = g_percent.load();
	if (pct < 0) return true;
	SBPP_REPORT r{};
	r.Version         = SBPP_BRIDGE_VERSION;
	r.AppliedSequence = g_applied.load();
	r.Percent         = (unsigned long)pct;
	r.Flags           = g_adjustable.load() ? SBPP_REPORT_ADJUSTABLE : 0u;

	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent) return false;
	DWORD got = 0;
	BOOL  ok  = DeviceIoControl(dev, IOCTL_SBPP_REPORT_LEVEL, &r, sizeof(r), nullptr, 0, &got, &ov);
	if (!ok && GetLastError() == ERROR_IO_PENDING) ok = GetOverlappedResult(dev, &ov, &got, TRUE);
	CloseHandle(ov.hEvent);
	return ok != FALSE;
}

void deliver(const SBPP_REQUEST &req, DWORD got) {
	if (got < sizeof(SBPP_REQUEST) || req.Version != SBPP_BRIDGE_VERSION) return;
	g_windowsPct.store((long)req.Percent);   // whatever Windows asks for, it now holds
	g_syncPct.store(-1);                     // and it is newer than any local change still settling
	PostMessageW(g_wnd, g_msg, (WPARAM)req.Percent, (LPARAM)req.Sequence);
}

// One connected session: returns when the driver goes away or Stop() is called.
void session(HANDLE dev) {
	OVERLAPPED ov{};
	ov.hEvent = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	if (!ov.hEvent) return;

	SBPP_REQUEST req{};
	bool waiting = false;
	for (;;) {
		if (!waiting) {
			ResetEvent(ov.hEvent);
			DWORD got = 0;
			if (DeviceIoControl(dev, IOCTL_SBPP_WAIT_REQUEST, nullptr, 0, &req, sizeof(req), &got, &ov)) {
				deliver(req, got);   // a request was already waiting
				continue;
			}
			if (GetLastError() != ERROR_IO_PENDING) break;
			waiting = true;
		}

		HANDLE hs[4] = {g_stop, g_dirty, ov.hEvent, g_sync};
		DWORD  w     = WaitForMultipleObjects(4, hs, FALSE, syncIfDue());
		if (w == WAIT_OBJECT_0) break;
		if (w == WAIT_OBJECT_0 + 1) {
			if (!sendReport(dev)) break;
			continue;
		}
		if (w == WAIT_OBJECT_0 + 2) {
			DWORD got = 0;
			waiting   = false;
			if (!GetOverlappedResult(dev, &ov, &got, FALSE)) break;
			deliver(req, got);
			continue;
		}
		if (w == WAIT_OBJECT_0 + 3 || w == WAIT_TIMEOUT) continue;   // syncIfDue runs on the next pass
		break;
	}

	if (waiting) {
		CancelIoEx(dev, &ov);
		DWORD got = 0;
		GetOverlappedResult(dev, &ov, &got, TRUE);
	}
	CloseHandle(ov.hEvent);
}

void run() {
	bool announcedMissing = false;
	while (WaitForSingleObject(g_stop, 0) == WAIT_TIMEOUT) {
		HANDLE dev = CreateFileW(SBPP_BRIDGE_USER_PATH, GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		                         OPEN_EXISTING, FILE_FLAG_OVERLAPPED, nullptr);
		if (dev == INVALID_HANDLE_VALUE) {
			if (!announcedMissing) {
				Log::Info(L"Brightness bridge: SbppMonFilter not available (%lu); Windows brightness API not linked",
				          GetLastError());
				announcedMissing = true;
			}
			WaitForSingleObject(g_stop, 5000);
			continue;
		}

		announcedMissing = false;
		Log::Info(L"Brightness bridge: connected to SbppMonFilter");
		g_applied.store(0);   // the driver restarts its sequence on every connect
		sendReport(dev);      // so the driver's _BQC starts from the real level
		g_windowsPct.store(-1);
		if (long pct = g_percent.load(); pct >= 0) Bridge::NoteLocalChange((int)pct);   // and Windows' slider too
		g_connected.store(true);

		session(dev);
		g_connected.store(false);
		CloseHandle(dev);
		Log::Info(L"Brightness bridge: disconnected");
		WaitForSingleObject(g_stop, 2000);   // back off before reconnecting
	}
}

} // namespace

namespace Bridge {

void Start(HWND wnd, UINT msg) {
	if (g_thread.joinable()) return;
	g_wnd   = wnd;
	g_msg   = msg;
	g_stop  = CreateEventW(nullptr, TRUE, FALSE, nullptr);
	g_dirty = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	g_sync  = CreateEventW(nullptr, FALSE, FALSE, nullptr);
	if (!g_stop || !g_dirty || !g_sync) {
		Log::Warn(L"Brightness bridge not started: CreateEvent failed (%lu)", GetLastError());
		return;
	}
	DEVICE_NOTIFY_SUBSCRIBE_PARAMETERS sub{onPlanBrightness, nullptr};
	GUID planGuid = kPlanBrightness;
	DWORD rc = PowerSettingRegisterNotification(&planGuid, DEVICE_NOTIFY_CALLBACK, &sub, &g_planNotify);
	if (rc != ERROR_SUCCESS) {
		g_planNotify = nullptr;
		Log::Warn(L"Brightness bridge: watching the plan brightness failed (%lu)", rc);
	}
	g_thread = std::thread(run);
}

void Stop() {
	if (g_planNotify) {
		PowerSettingUnregisterNotification(g_planNotify);
		g_planNotify = nullptr;
	}
	if (g_stop) SetEvent(g_stop);
	if (g_thread.joinable()) g_thread.join();
	if (g_stop)  { CloseHandle(g_stop);  g_stop  = nullptr; }
	if (g_dirty) { CloseHandle(g_dirty); g_dirty = nullptr; }
	if (g_sync)  { CloseHandle(g_sync);  g_sync  = nullptr; }
}

void NoteLocalChange(int percent) {
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	g_syncDue.store(GetTickCount64() + kSyncSettleMs);
	g_syncPct.store(percent);
	if (g_sync) SetEvent(g_sync);
}

bool IsSyncEcho(int percent) {
	return percent == g_sentPct.load() && GetTickCount64() - g_sentAt.load() < kEchoWindowMs;
}

void ReportLevel(int percent, bool adjustable) {
	if (percent < 0) percent = 0;
	if (percent > 100) percent = 100;
	g_percent.store(percent);
	g_adjustable.store(adjustable);
	if (g_dirty) SetEvent(g_dirty);
}

void MarkApplied(unsigned long sequence) {
	if (sequence != 0) g_applied.store(sequence);   // 0: a plan change, not a driver request
}

} // namespace Bridge
