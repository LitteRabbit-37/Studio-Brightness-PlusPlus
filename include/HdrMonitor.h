#pragma once

// HDR detection for Apple displays, via DXGI.
//
// We use DXGI (IDXGIOutput6::GetDesc1) rather than the standard Windows
// DisplayConfigGetDeviceInfo(GET_ADVANCED_COLOR_INFO) because Apple displays reject that
// query with ERROR_INVALID_PARAMETER (0x57). An output is in HDR when its color space is
// DXGI_COLOR_SPACE_RGB_FULL_G2084_NONE_P2020 (HDR10 / PQ).

// Returns true if any connected Apple display (EDID manufacturer "APP") output is currently
// in HDR. Returns false on any failure, so callers safely fall back to normal (SDR) behavior.
//
// This is the v1 "any Apple display" detector, correct for single-Apple-display setups (the
// case for the XDR testers). A per-display variant keyed on ContainerId can be layered on later
// for multi-Apple-display machines.
bool HdrAnyAppleDisplayActive();
