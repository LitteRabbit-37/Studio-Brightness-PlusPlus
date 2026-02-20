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

**Studio brightness ++** is a small Windows utility for controlling the brightness of an Apple Studio Display or Pro Display XDR:

- **Automatic brightness adjustment** based on ambient light (ALS sensor)
- **Manual brightness control** via the native keyboard brightness keys (the "sun" keys, Fn+F1/F2, or QMK/VIA custom keys)
- **On-Screen Display (OSD)** — a modern brightness indicator shown on key presses
- **Tray brightness slider** — left-click the tray icon to instantly adjust brightness
- **Display detection** — the tray menu shows which Apple display is connected
- Runs in the system tray, lightweight and easy to use

### What's new in v2.0.0?

- **On-Screen Display (OSD):** A sleek, Windows 11-style brightness bar appears in the top-left corner whenever you change brightness with a keyboard shortcut. Can be disabled in Options.
- **Tray brightness slider:** Left-click the tray icon to open a mini slider popup for quick manual adjustment. Right-click still opens the context menu.
- **Display detection in tray menu:** The context menu now shows which Apple display is detected (Studio Display or Pro Display XDR) with a live connection indicator.
- **Modernized UI:** Refreshed overall look and feel across all interface components.

### What was added in v1.5.0?

- **Apple Pro Display XDR support:** The app now detects and controls the Pro Display XDR (PID 9243) in addition to the Studio Display. The XDR's ALS sensor is intentionally bypassed (it causes a hang on Windows) — auto-brightness falls back to a fixed lux value.

### Full feature set

Compared to the original [studio-brightness](https://github.com/sfjohnson/studio-brightness), this fork adds:

- **Automatic brightness (ALS)** with deadband/hysteresis to avoid micro-adjustments for tiny ambient light changes. Toggle from the tray menu.
- **Native brightness key support** (Fn+F1/F2 / sun keys, or your QMK/VIA keys).
- **Custom global shortcuts** via an Options dialog. Includes a "Reset to Defaults" button.
- **On-Screen Display (OSD)** — dismisses automatically after 2.5 seconds.
- **Tray brightness slider** — quick adjustment without opening a dialog.
- **Display detection** — tray menu shows the detected display and its connection status.
- **Apple Pro Display XDR support** (PID 9243).
- **Settings persistence** — all options saved to Windows Registry and restored on restart.
- **Run at Windows startup** — optional auto-launch setting.
- **Revised device detection** (`hid.cpp` rewritten) for robust HID matching.

### Credits

- **Original author:** [Sam Johnson (sfjohnson)](https://github.com/sfjohnson) ([studio-brightness](https://github.com/sfjohnson/studio-brightness))
- **Modifications, HID improvements, ALS/auto-brightness, native key support, OSD, tray slider, display detection:** @LitteRabbit-37
- **XDR support and ALS bypass:** @sse1234

### Prerequisites

- Windows 10 or later
- **Apple Studio Display** or **Apple Pro Display XDR** connected via **USB-C/Thunderbolt** (not HDMI/DisplayPort-to-USB-C adapters)
- (Sometimes required) Administrator rights for HID access

## Usage

- **Increase brightness:** Use your keyboard's native brightness up key (sun/F2) or a custom shortcut (if enabled in Options).
- **Decrease brightness:** Use your keyboard's native brightness down key (sun/F1) or a custom shortcut (if enabled).
- **Quick slider:** Left-click the tray icon to open the brightness slider popup.
- **Automatic brightness:** If you have a compatible ambient light sensor, the app auto-adjusts brightness. Right-click the tray icon to toggle "Automatic Brightness".
- **Options…:** Right-click the tray icon → "Options…". You can:
  - Toggle automatic brightness
  - Toggle the On-Screen Display (OSD)
  - Enable "Run at Windows startup"
  - Enable custom shortcuts and set Increase/Decrease keys
  - Set the number of brightness steps (10–50)
  - Click "Reset to Defaults" to restore default settings
  - All settings are automatically saved to the Windows Registry
- **Quit:** Right-click the tray icon and select "Quit".

## Building

### Prerequisites

- Visual Studio tools (Developer Command Prompt with `cl.exe` and `rc.exe` in PATH)

### Using build.bat

1. Open a **x64 Developer Command Prompt for VS**
2. Go to the project directory
3. Run:

```bash
build.bat
```

The output will be `bin\studio-brightness-plusplus.exe`.

## Technical notes

- **`hid.cpp`** was rewritten to reliably detect Apple displays by VID/PID on the `mi_07` interface, excluding HID subcollections (`&col`).
- Brightness step changes default to **10 steps** across the detected range (configurable 10–50).
- ALS auto-adjust uses a deadband of `max(3% of range, 1500 units)` and smooths toward the target in small increments (2%/step, minimum 500 units).
- Brightness key events are captured via HID RawInput (Consumer Control page). Custom global hotkeys use `RegisterHotKey`.
- **OSD** is rendered via GDI+ as a layered (per-pixel alpha) window — no focus theft, passes clicks through.
- **Settings persistence:** All options stored in `HKEY_CURRENT_USER\Software\StudioBrightnessPlusPlus`. Run-at-startup uses `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`.
- **Pro Display XDR:** ALS is disabled for XDR (causes a hang on Windows); auto-brightness defaults to 100 lux when XDR is detected.

## Known limitations

- If no ambient light sensor is present or accessible, only manual brightness control is available.
- ALS auto-brightness is not available on the Pro Display XDR (Windows driver limitation).
- Only tested with the Apple Studio Display and Apple Pro Display XDR. Other monitors are not supported.
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
