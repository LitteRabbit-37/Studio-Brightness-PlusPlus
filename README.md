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
- **Color presets** to switch the display's Apple Reference Mode (color profile) from the Options dialog
- **Automatic updates** from GitHub with selectable stable and beta channels, installed in one click
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
- **Automatic brightness (ALS)** via `ISensorEvents` async callbacks, with an Apple-style relative-lux hysteresis and an asymmetric perceptual ramp. Toggle from the tray menu.
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
- **Color presets** to read and switch the display's Apple Reference Modes (color profiles), discovered per display and remembered.
- **MSI installer** with an optional desktop shortcut and a launch-on-finish option, plus a clean uninstall that removes settings.
- **In-app auto-update** that checks GitHub Releases directly, with stable and beta channels and one-click install.

### Credits

- **Original author:** [Sam Johnson (sfjohnson)](https://github.com/sfjohnson) ([studio-brightness](https://github.com/sfjohnson/studio-brightness))
- **Modifications, HID improvements, ALS/auto-brightness, native key support, OSD, tray slider, display detection, multi-display, log viewer:** @LitteRabbit-37
- **XDR support:** @sse1234
- **Studio Display Gen 2 & Studio Display XDR PID identification:** @oskarjiang

### Prerequisites

- Windows 10 or later
- **Apple Studio Display** or **Apple Pro Display XDR** connected via **USB-C/Thunderbolt** (not HDMI/DisplayPort-to-USB-C adapters)
- (Sometimes required) Administrator rights for HID access

## Installation

1. Download the latest `studio-brightness-plusplus-x.y.z.msi` from the [Releases](https://github.com/LitteRabbit-37/Studio-Brightness-PlusPlus/releases) page.
2. Run it. The installer asks for the usual Windows permission once, adds a Start Menu entry, lets you add an optional desktop shortcut, and can launch the app right away from its last page.
3. The app lives in the system tray. Enable "Run at Windows startup" from Options if you want it to start with Windows.

To remove it, use Windows Settings, Apps, "Installed apps", which also clears your settings. A portable `studio-brightness-plusplus.exe` is attached to every release as well, if you would rather not install.

## Usage

- **Increase brightness:** Use your keyboard's native brightness up key (sun/F2) or a custom shortcut (if enabled in Options).
- **Decrease brightness:** Use your keyboard's native brightness down key (sun/F1) or a custom shortcut (if enabled).
- **Quick slider:** Left-click the tray icon to open the brightness slider popup.
- **Automatic brightness:** If you have a compatible ambient light sensor, the app auto-adjusts brightness. Right-click the tray icon to toggle "Automatic Brightness".
- **Options...:** Right-click the tray icon > "Options...". You can:
  - Toggle automatic brightness
  - Toggle the On-Screen Display (OSD)
  - Choose a color preset (Apple Reference Mode) for the active display
  - Enable "Run at Windows startup"
  - Enable custom shortcuts and set Increase/Decrease keys
  - Set the number of brightness steps (10-50)
  - Click "Reset to Defaults" to restore default settings
  - All settings are automatically saved to the Windows Registry
- **Logs...:** Right-click the tray icon > "Logs..." to open the real-time log viewer. Useful for diagnostics and troubleshooting.
- **Updates:** The app checks for a new version on launch and once a day. When one is available you get a notification and an "Install update" item appears in the tray menu; one click downloads it, installs it, and relaunches. Right-click the tray icon and use "Update channel" to pick "Stable only" or "Include betas", or "Check update" to check right away.
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

The output will be `bin\studio-brightness-plusplus.exe`. The build also generates `include/version.h` from the current git tag (or a `-dev` version when building locally), so the version is never hardcoded.

### Building the installer

The MSI is built with [WiX 5](https://wixtoolset.org/):

```bash
dotnet tool install --global wix --version 5.0.2
wix extension add -g WixToolset.UI.wixext/5.0.2
tools\build-msi.ps1
```

The output is `bin\studio-brightness-plusplus-x.y.z.msi`. Releases are produced automatically by GitHub Actions when a `v*` tag is pushed; a tag with a `-beta` suffix publishes a pre-release.

## Technical notes

- **`hid.cpp`** uses profile-based detection with Apple VID/PID matching, excluding HID subcollections (`&col`). Unknown Apple displays fall back to generic mode if Feature caps are valid.
- **Multi-display:** All detected displays share linked brightness. The worker thread manages device lifecycle with automatic reconnection.
- **ALS:** Uses `ISensorEvents` async callbacks (no polling) for ambient light data. Sensors are correlated to displays via `DEVPKEY_Device_ContainerId`.
- Brightness step changes default to **10 steps** across the detected range (configurable 10-50).
- ALS auto-adjust follows an Apple-style response: it reacts only to ambient changes above a relative threshold (20%), then ramps to the new target over a fixed asymmetric duration (about 1.5s to brighten, 5s to dim), stepping in perceptual (log2) space.
- Brightness key events are captured via HID RawInput (Consumer Control page). Custom global hotkeys use `RegisterHotKey`.
- **OSD** is rendered via GDI+ as a layered (per-pixel alpha) window -- no focus theft, passes clicks through.
- **Settings persistence:** All options stored in `HKEY_CURRENT_USER\Software\StudioBrightnessPlusPlus`. Run-at-startup uses `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`.
- **Log viewer:** Ring buffer (2000 entries) with SRWLOCK, refreshed every 200ms via timer.

## Known limitations

- If no ambient light sensor is present or accessible, only manual brightness control is available.
- Only tested with the Apple Studio Display and Apple Pro Display XDR. Other Apple monitors may work via generic fallback.
- Color control is limited to switching between the display's built-in Apple Reference Mode presets. The app does not set an arbitrary color temperature or white point, and does not yet support True Tone, HDR toggling, or screen rotation.

---

## License

MPL-2.0 License, following the upstream project's terms.

---

## Thanks

<!--START_SECTION:buy-me-a-coffee--><div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Jon M</b> </div>  <div><i>Thank you for brightening my day!</i></div><br>
<div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Miguel M.</b> </div>  <div><i>Hey man, I really appreciate what you did here! I wish I had more to give, but I'm really working on getting a better-paying job, and this has really helped in my journey. When I get a better job, I'll support more!</i></div><br>
<div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Someone</b> </div>  <div><i>{"note":"Worked on studio XDR, but only can change brightness when start app after windows reboot and complete login in, think need option install as service and delay start after 1 min. Thank so much","gif":null,"video":null}</i></div><br><!--END_SECTION:buy-me-a-coffe-->

Many thanks to Sam Johnson for the original work!
Feel free to submit issues, pull requests, or feedback.

## Support

If you find Studio brightness ++ useful and want to support the development, you can buy me a coffee:

<a href="https://www.buymeacoffee.com/litterabbit" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" height="40" /></a>
