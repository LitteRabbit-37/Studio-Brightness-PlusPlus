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

- **Automatic brightness adjustment** based on ambient light, from the display's own sensor or the Windows Sensor API
- **Automatic screen rotation** when the display is physically turned, using its built-in orientation sensor (needs the sensor driver, see Display sensors below)
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

| Display | PID | Ambient light | Rotation |
|---|---|---|---|
| Apple Studio Display | 0x1114 | Built in, or read directly with the sensor driver | With the sensor driver |
| Apple Studio Display (Gen 2) | 0x1118 | Built in, or read directly with the sensor driver | With the sensor driver |
| Apple Studio Display XDR | 0x1116 | Only with the sensor driver and its descriptor fix | With the sensor driver |
| Apple Pro Display XDR | 0x9243 | With the sensor driver, untested | With the sensor driver, untested |
| Other Apple displays (VID 05AC) | Auto-detected | Depends | No |

See Display sensors below for what the sensor driver is and where things stand with it.

### Features

Compared to the original [studio-brightness](https://github.com/sfjohnson/studio-brightness), this fork adds:

- **Multi-display support** to detect and control all connected Apple displays together (linked brightness).
- **Automatic brightness (ALS)** from the display's own sensor, read directly when a sensor driver is installed and through `ISensorEvents` otherwise, with an Apple-style relative-lux hysteresis and an asymmetric perceptual ramp. The direct path also reads the ambient colour temperature and chromaticity. Toggle from the tray menu.
- **Automatic screen rotation** driven by the display's orientation sensor: turn the panel 90 degrees and Windows follows. It only reacts to a physical move, never at startup. Toggle from the tray menu.
- **ALS sensor correlation** matching sensors to displays via ContainerId for accurate per-display ambient light readings. When a display has more than one sensor, the brightest reading wins.
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
- **Orientation and ambient light sensors read directly over HID, XDR testing and driver work:** @FluorescentHallucinogen
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
- **Automatic rotation:** With the sensor driver installed, the display follows when you physically rotate it. Right-click the tray icon to toggle "Automatic Rotate".
- **Options...:** Right-click the tray icon > "Options...". You can:
  - Toggle automatic brightness
  - Toggle automatic rotation
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

## Display sensors

Apple displays carry an ambient light sensor and an orientation sensor, but on Windows their HID interfaces do not start out of the box. The orientation sensor fails with a Code 10 on every model, and the ambient light sensor is only reachable through the Windows Sensor API on the Studio Display and the Studio Display (Gen 2). The app works without any driver: automatic brightness then uses the Windows Sensor API, or whatever ambient light sensor the host machine has, and there is no rotation.

Unlocking the sensors takes a small null driver package, which tells Windows to start those interfaces without a driver so the app can read them directly. Apple ships one with the Boot Camp 6.1.17 support software (`AppleDisplayNull64.inf`, signed by Apple) that covers the 2022 Studio Display and the Pro Display XDR; if you have it, `pnputil /add-driver AppleDisplayNull64.inf /install` from an elevated prompt is all it takes. The 2026 models are not in it. A community package maintained by @FluorescentHallucinogen covers all models, and the Studio Display XDR additionally needs a filter driver that fixes its ambient light sensor descriptor, which Windows otherwise rejects.

That community package is not bundled with or recommended by this app yet. Its current builds are signed with a self signed certificate that changes on every build, and the install script adds that certificate to the Windows trusted root store. The plan, once the package carries a license and a stable signing identity, is to install it from inside the app. Issue #16 tracks where this stands.

Once a sensor driver is in place the app picks it up on its own: ambient light moves from the Sensor API to the display's sensor, and "Automatic Rotate" starts working. Rotation only ever reacts to a physical move of the panel; the app never changes your orientation at startup. Everything known about the interfaces is in `docs/hid-map.md`, and `tools/hidprobe` is the read-only diagnostic used to map them, attached to each release as `hid_probe.exe`.

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
- **ALS:** Two sources. With a sensor driver installed, the display's ambient light collection is read directly as HID from a dedicated polling thread: illuminance, colour temperature and CIE chromaticity, with the HID unit exponents applied. Otherwise `ISensorEvents` async callbacks. Sensors are correlated to displays via `DEVPKEY_Device_ContainerId`, and the brightest of a display's sensors wins.
- **Orientation:** The orientation sensor's tilt angle is polled on the same thread and mapped to a Windows display orientation, applied with `ChangeDisplaySettingsEx` only when the panel has actually moved.
- Brightness step changes default to **10 steps** across the detected range (configurable 10-50).
- ALS auto-adjust follows an Apple-style response: it reacts only to ambient changes above a relative threshold (20%), then ramps to the new target over a fixed asymmetric duration (about 1.5s to brighten, 5s to dim), stepping in perceptual (log2) space.
- Brightness key events are captured via HID RawInput (Consumer Control page). Custom global hotkeys use `RegisterHotKey`.
- **OSD** is rendered via GDI+ as a layered (per-pixel alpha) window -- no focus theft, passes clicks through.
- **Settings persistence:** All options stored in `HKEY_CURRENT_USER\Software\StudioBrightnessPlusPlus`. Run-at-startup uses `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`.
- **Log viewer:** Ring buffer (2000 entries) with SRWLOCK, refreshed every 200ms via timer.

## Known limitations

- If no ambient light sensor is present or accessible, only manual brightness control is available.
- Only tested with the Apple Studio Display and Apple Pro Display XDR. Other Apple monitors may work via generic fallback.
- Color control is limited to switching between the display's built-in Apple Reference Mode presets. The app does not set an arbitrary color temperature or white point. True Tone is read only: the app gets the ambient colour temperature from the sensor, but writing the display's white point is not decoded yet.
- Screen rotation and direct ambient light readings need a sensor driver, see Display sensors.

---

## License

MPL-2.0 License, following the upstream project's terms.

---

## Thanks

<!--START_SECTION:buy-me-a-coffee--><div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Em</b> </div>  <div><i>I used to unplug my laptop and connect my iPad , lower brightness and reconnect throughout the day. Your software fixes this problem perfectly. Thanks for using your talent and skill for this application!</i></div><br>
<div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Jon M</b> </div>  <div><i>Thank you for brightening my day!</i></div><br>
<div><img src="https://github.com/akosbalasko/coffee-to-file/blob/main/assets/bmc-logo.png?raw=true" width="30"> from <b>Miguel M.</b> </div>  <div><i>Hey man, I really appreciate what you did here! I wish I had more to give, but I'm really working on getting a better-paying job, and this has really helped in my journey. When I get a better job, I'll support more!</i></div><br><!--END_SECTION:buy-me-a-coffe-->

Many thanks to Sam Johnson for the original work!
Feel free to submit issues, pull requests, or feedback.

## Support

If you find Studio brightness ++ useful and want to support the development, you can buy me a coffee:

<a href="https://www.buymeacoffee.com/litterabbit" target="_blank"><img src="https://cdn.buymeacoffee.com/buttons/v2/default-yellow.png" alt="Buy Me A Coffee" height="40" /></a>
