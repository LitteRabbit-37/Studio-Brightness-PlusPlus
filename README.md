<div align="center">
  <img src="studio-brightness-plusplus.ico" />
</div>
<br>
<br>
<div align="center">
  <h1>Studio brightness ++</h1>
  <p>Control your Apple studio display on Windows as if you were on macOS!</p>
</div>
<br>
<div align="center">
  <table>
		<th><a href=https://litterabbit-37.github.io/litterabbit.github.io/StudioBrightnessPlusPlus.html>Website&nbsp;↗</a></th>
		<td><a href=https://github.com/litterabbit-37/Studio-Brightness-PlusPlus/issues/new/choose>Help&nbsp;&&nbsp;Feedback</a></td>
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

**Studio brightness ++** is a small Windows utility for controlling the brightness of an Apple Studio Display:

- **Automatic brightness adjustment** based on ambient light (based on ALS sensor)
- **Manual brightness control** via the native keyboard brightness keys (the “sun” keys, Fn+F1/F2, or QMK/VIA custom keys)
- Runs in the system tray, lightweight and easy to use

### What's new in this version?

Compared to the original, this fork adds:

- **Automatic brightness (ALS)** with a built‑in deadband/hysteresis to avoid micro‑adjustments for tiny room‑light changes. You can toggle this from the tray.
- **Native brightness key support** (sun/F1–F2, or your QMK/VIA keys).
- **Custom global shortcuts** via an Options dialog (tray menu). Includes a "Reset to Defaults" that reverts to the native Windows brightness keys only.
- **Settings persistence** — all options (auto-brightness toggle, custom shortcuts, run-at-startup) are saved to Windows Registry and restored on app restart.
- **Run at Windows startup** — optional setting to launch the application automatically when Windows starts.
- **Revised Studio Display detection** (`hid.cpp` rewritten) for more robust device matching.

### Credits

- **Original author:** [Sam Johnson (sfjohnson)](https://github.com/sfjohnson) ([studio-brightness](https://github.com/sfjohnson/studio-brightness))
- **Modifications, HID improvements, ALS/autobrightness, native key support:** @LitteRabbit-37

### Prerequisites

- Windows 10 or later
- **Apple Studio Display** connected via **USB-C/Thunderbolt** (not HDMI/Display Port to USB-C)
- (Sometimes required) Administrator rights for HID access

## Usage

- **Increase brightness:** Use your keyboard's native brightness up key (sun/F2) or a custom shortcut (if enabled in Options).
- **Decrease brightness:** Use your keyboard's native brightness down key (sun/F1) or a custom shortcut (if enabled).
- **Automatic brightness:** If you have a compatible ambient light sensor, the app auto‑adjusts brightness. Right‑click the tray icon to toggle "Automatic Brightness".
- **Options…:** Right‑click the tray icon → "Options…". You can:
  - Toggle automatic brightness
  - Enable "Run at Windows startup" to launch the app automatically when Windows starts
  - Enable custom shortcuts and set Increase/Decrease keys
  - Click "Reset to Defaults" to disable custom shortcuts and rely only on the native Windows brightness keys
  - All settings are automatically saved to Windows Registry and persist across restarts
- **Quit:** Right‑click the tray icon and select "Quit".

## Building

### Prerequisites

- Visual Studio tools (Developer Command Prompt with `cl.exe` and `rc.exe` in PATH), or CMake

### Using build.bat

1. Open a **x64 Developer Command Prompt for VS**
2. Go to the project directory
3. Run:

```bash
build.bat
```

The output will be `bin\studio-brightness-plusplus.exe`.

## Technical notes

- The file **`hid.cpp`** was rewritten to more reliably detect the Apple Studio Display (VID 05ac / PID 1114). The detection logic may differ from the original upstream.
- Brightness step changes default to **10 steps** across the detected brightness range.
- ALS auto‑adjust uses a deadband to prevent flicker: max(≈3% of device range, a small absolute floor) and smooths toward target in small increments.
- Brightness key events are captured via HID RawInput (Consumer Control page). Optional custom global hotkeys are registered with `RegisterHotKey`.
- **Settings persistence:** All options are stored in `HKEY_CURRENT_USER\Software\StudioBrightnessPlusPlus` and automatically loaded on startup. The "Run at startup" feature uses the standard Windows Registry key `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`.

## Known limitations

- If no ambient light sensor is present or accessible(ALS sensor), only manual brightness control is available.
- Only tested with official Apple Studio Display. Other Apple monitors are not supported.
- The application does **not** adjust color temperature or other display parameters.

---

## License

MPL-2.0 License, following the upstream project’s terms.

---

## Thanks

<!--START_SECTION:buy-me-a-coffee--><div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Someone</b> </div>  <div><i>null</i></div><br>
<div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>The Naked Dev</b> </div>  <div><i>Absolutely fantastic job, I needed this for years and now its here!</i></div><br><!--END_SECTION:buy-me-a-coffe-->

Many thanks to Sam Johnson for the original work!
Feel free to submit issues, pull requests, or feedback.
