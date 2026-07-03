#pragma once

// Diagnostic only: logs the NVIDIA-driver-level HDR state of every desktop display, next to our
// DXGI verdict. Some setups (locked HDR toggle, driver-forced HDR) may diverge from what DXGI
// reports; the two lines side by side in a log make that visible.
//
// No-op without an NVIDIA driver (nvapi64.dll absent). Never taken as an input for decisions.
void NvapiLogHdrState(const wchar_t *context);
