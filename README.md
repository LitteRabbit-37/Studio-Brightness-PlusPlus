<div align="center">
  <img src="studio-brightness-plusplus.ico" />
</div>
<br>
<br>
<div align="center">
  <h1>Studio brightness ++</h1>
  <p>Control your Apple Studio Display or Pro Display XDR on Windows as if you were on macOS!</p>
</div>
<br>
<div align="center">
  <table>
		<th><a href=https://litterabbit-37.github.io/litterabbit.github.io/StudioBrightnessPlusPlus.html>Website&nbsp;↗</a></th>
		<td><a href=https://github.com/litterabbit-37/Studio-Brightness-PlusPlus/issues/new/choose>Help&nbsp;&amp;&nbsp;Feedback</a></td>
		<td><a href=https://github.com/LitteRabbit-37/Studio-Brightness-PlusPlus/releases>Releases</a></td>
	</table>
</div>
<div align="center">
  <img src="https://img.shields.io/github/downloads/LitteRabbit-37/Studio-Brightness-PlusPlus/total?color=%23CAAA3A">
  <img alt="GitHub Release" src="https://img.shields.io/github/v/release/LitteRabbit-37/Studio-Brightness-PlusPlus">
  <img alt="GitHub Repo stars" src="https://img.shields.io/github/stars/LitteRabbit-37/Studio-Brightness-PlusPlus?style=social">
</div>

<br>
<br>

> **Note: This project is a fork and extension of [studio-brightness](https://github.com/sfjohnson/studio-brightness) by Sam Johnson (sfjohnson).**

## Overview

### What is it?

**Studio brightness ++** is a small Windows utility for controlling the brightness of Apple displays:

- **Automatic brightness adjustment** based on ambient light (ALS sensor) via async callbacks
- **Multi-display support** to control all connected Apple displays with linked brightness
- **Manual brightness control** via the native keyboard brightness keys (the "sun" keys, Fn+F1/F2, or QMK/VIA custom keys)
- **On-Screen Display (OSD)** showing a modern brightness indicator on key presses
- **Tray brightness slider** for instant adjustment by left-clicking the tray icon
- **Display detection** showing connected Apple display(s) in the tray menu
- **Log viewer** with real-time diagnostics accessible from the tray menu
- **Generic Apple display support** for unknown Apple displays with valid HID brightness caps
- Runs in the system tray, lightweight and easy to use

### Supported displays

| Display | PID | ALS |
|---|---|---|
| Apple Studio Display | 0x1114 | Built-in |
| Apple Studio Display (Gen 2) | 0x1118 | Built-in |
| Apple Studio Display XDR | 0x1116 | No |
| Apple Pro Display XDR | 0x9243 | No |
| Other Apple displays (VID 05AC) | Auto-detected | Depends |

### Features

Compared to the original [studio-brightness](https://github.com/sfjohnson/studio-brightness), this fork adds:

- **Multi-display support** to detect and control all connected Apple displays together (linked brightness).
- **Automatic brightness (ALS)** via `ISensorEvents` async callbacks with deadband/hysteresis. Toggle from the tray menu.
- **ALS sensor correlation** matching sensors to displays via ContainerId for accurate per-display ambient light readings.
- **Native brightness key support** (Fn+F1/F2 / sun keys, or your QMK/VIA keys).
- **Custom global shortcuts** via an Options dialog. Includes a "Reset to Defaults" button.
- **On-Screen Display (OSD)** that dismisses automatically after 2.5 seconds.
- **Tray brightness slider** for quick adjustment without opening a dialog.
- **Display detection** showing connected display(s) and connection status in the tray menu.
- **Log viewer** with real-time log window and copy-to-clipboard, accessible via tray menu "Logs...".
- **Generic fallback** for unknown Apple displays (VID 05AC) with valid HID brightness Feature caps.
- **Apple Pro Display XDR support** (PID 0x9243).
- **Apple Studio Display Gen 2 support** (PID 0x1118).
- **Apple Studio Display XDR support** (PID 0x1116).
- **Settings persistence** with all options saved to Windows Registry and restored on restart.
- **Run at Windows startup** as an optional auto-launch setting.
- **Robust device detection** with profile-based matching, ContainerId correlation, and automatic reconnection.

### Credits

- **Original author:** [Sam Johnson (sfjohnson)](https://github.com/sfjohnson) ([studio-brightness](https://github.com/sfjohnson/studio-brightness))
- **Modifications, HID improvements, ALS/auto-brightness, native key support, OSD, tray slider, display detection, multi-display, log viewer:** @LitteRabbit-37
- **XDR support:** @sse1234
- **Studio Display Gen 2 & Studio Display XDR PID identification:** @oskarjiang

### Prerequisites

- Windows 10 or later
- **Apple Studio Display** or **Apple Pro Display XDR** connected via **USB-C/Thunderbolt** (not HDMI/DisplayPort-to-USB-C adapters)
- (Sometimes required) Administrator rights for HID access

## Usage

- **Increase brightness:** Use your keyboard's native brightness up key (sun/F2) or a custom shortcut (if enabled in Options).
- **Decrease brightness:** Use your keyboard's native brightness down key (sun/F1) or a custom shortcut (if enabled).
- **Quick slider:** Left-click the tray icon to open the brightness slider popup.
- **Automatic brightness:** If you have a compatible ambient light sensor, the app auto-adjusts brightness. Right-click the tray icon to toggle "Automatic Brightness".
- **Options...:** Right-click the tray icon > "Options...". You can:
  - Toggle automatic brightness
  - Toggle the On-Screen Display (OSD)
  - Enable "Run at Windows startup"
  - Enable custom shortcuts and set Increase/Decrease keys
  - Set the number of brightness steps (10-50)
  - Click "Reset to Defaults" to restore default settings
  - All settings are automatically saved to the Windows Registry
- **Logs...:** Right-click the tray icon > "Logs..." to open the real-time log viewer. Useful for diagnostics and troubleshooting.
- **Quit:** Right-click the tray icon and select "Quit".

## Building

### Prerequisites

- Visual Studio 2022 Build Tools (or Community/Professional/Enterprise)

### Using build.bat

1. Open any terminal (the build script auto-detects the VS environment)
2. Go to the project directory
3. Run:

```bash
build.bat
```

The output will be `bin\studio-brightness-plusplus.exe`.

## Technical notes

- **`hid.cpp`** uses profile-based detection with Apple VID/PID matching, excluding HID subcollections (`&col`). Unknown Apple displays fall back to generic mode if Feature caps are valid.
- **Multi-display:** All detected displays share linked brightness. The worker thread manages device lifecycle with automatic reconnection.
- **ALS:** Uses `ISensorEvents` async callbacks (no polling) for ambient light data. Sensors are correlated to displays via `DEVPKEY_Device_ContainerId`.
- Brightness step changes default to **10 steps** across the detected range (configurable 10-50).
- ALS auto-adjust uses a deadband of `max(3% of range, 1500 units)` and smooths toward the target in small increments (2%/step, minimum 500 units).
- Brightness key events are captured via HID RawInput (Consumer Control page). Custom global hotkeys use `RegisterHotKey`.
- **OSD** is rendered via GDI+ as a layered (per-pixel alpha) window -- no focus theft, passes clicks through.
- **Settings persistence:** All options stored in `HKEY_CURRENT_USER\Software\StudioBrightnessPlusPlus`. Run-at-startup uses `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`.
- **Log viewer:** Ring buffer (2000 entries) with SRWLOCK, refreshed every 200ms via timer.

## Known limitations

- If no ambient light sensor is present or accessible, only manual brightness control is available.
- Only tested with the Apple Studio Display and Apple Pro Display XDR. Other Apple monitors may work via generic fallback.
- The application does **not** adjust color temperature or other display parameters.

---

## License

MPL-2.0 License, following the upstream project's terms.

---

## Thanks

<!--START_SECTION:buy-me-a-coffee--><div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Someone</b> </div>  <div><i>null</i></div><br>
<div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>The Naked Dev</b> </div>  <div><i>Absolutely fantastic job, I needed this for years and now its here!</i></div><br><!--END_SECTION:buy-me-a-coffe-->

Many thanks to Sam Johnson for the original work!
Feel free to submit issues, pull requests, or feedback.
