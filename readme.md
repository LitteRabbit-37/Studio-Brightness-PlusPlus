<p align="center">
  <img src="studio-brightness-plusplus.ico" />
</p>
<br>
<br>

<div align="center">
  <img src="https://img.shields.io/github/downloads/LitteRabbit-37/Studio-Brightness-PlusPlus/total?color=%23CAAA3A">
</div>
<br>
<br>

# Studio brightness ++

> **Note: This project is a fork and extension of [studio-brightness](https://github.com/sfjohnson/studio-brightness) by Sam Johnson (sfjohnson).**

## What is it?

**Studio brightness ++** is a small Windows utility for controlling the brightness of an Apple Studio Display:

-   **Automatic brightness adjustment** based on ambient light (based on ALS sensor)
-   **Manual brightness control** via the native keyboard brightness keys (the “sun” keys, Fn+F1/F2, or QMK/VIA custom keys)
-   Runs in the system tray, lightweight and easy to use

## What's new in this version?

Compared to the original, this fork adds:

-   **Automatic brightness (ALS)** with a built‑in deadband/hysteresis to avoid micro‑adjustments for tiny room‑light changes. You can toggle this from the tray.
-   **Native brightness key support** (sun/F1–F2, or your QMK/VIA keys).
-   **Custom global shortcuts** via an Options dialog (tray menu). Includes a "Reset to Defaults" that reverts to the native Windows brightness keys only.
-   **Settings persistence** — all options (auto-brightness toggle, custom shortcuts, run-at-startup) are saved to Windows Registry and restored on app restart.
-   **Run at Windows startup** — optional setting to launch the application automatically when Windows starts.
-   **Revised Studio Display detection** (`hid.cpp` rewritten) for more robust device matching.

## Credits

-   **Original author:** [Sam Johnson (sfjohnson)](https://github.com/sfjohnson) ([studio-brightness](https://github.com/sfjohnson/studio-brightness))
-   **Modifications, HID improvements, ALS/autobrightness, native key support:** @LitteRabbit-37

## Prerequisites

-   Windows 10 or later
-   **Apple Studio Display** connected via USB-C/Thunderbolt
-   Visual Studio tools (Developer Command Prompt with `cl.exe` and `rc.exe` in PATH), or CMake
-   (Sometimes required) Administrator rights for HID access

## Building

### Using build.bat

1. Open a **x64 Developer Command Prompt for VS**
2. Go to the project directory
3. Run:

```bash
build.bat
```

The output will be `bin\studio-brightness-plusplus.exe`.

## Usage

-   **Increase brightness:** Use your keyboard's native brightness up key (sun/F2) or a custom shortcut (if enabled in Options).
-   **Decrease brightness:** Use your keyboard's native brightness down key (sun/F1) or a custom shortcut (if enabled).
-   **Automatic brightness:** If you have a compatible ambient light sensor, the app auto‑adjusts brightness. Right‑click the tray icon to toggle "Automatic Brightness".
-   **Options…:** Right‑click the tray icon → "Options…". You can:
    -   Toggle automatic brightness
    -   Enable "Run at Windows startup" to launch the app automatically when Windows starts
    -   Enable custom shortcuts and set Increase/Decrease keys
    -   Click "Reset to Defaults" to disable custom shortcuts and rely only on the native Windows brightness keys
    -   All settings are automatically saved to Windows Registry and persist across restarts
-   **Quit:** Right‑click the tray icon and select "Quit".

## Technical notes

-   The file **`hid.cpp`** was rewritten to more reliably detect the Apple Studio Display (VID 05ac / PID 1114). The detection logic may differ from the original upstream.
-   Brightness step changes default to **10 steps** across the detected brightness range.
-   ALS auto‑adjust uses a deadband to prevent flicker: max(≈3% of device range, a small absolute floor) and smooths toward target in small increments.
-   Brightness key events are captured via HID RawInput (Consumer Control page). Optional custom global hotkeys are registered with `RegisterHotKey`.
-   **Settings persistence:** All options are stored in `HKEY_CURRENT_USER\Software\StudioBrightnessPlusPlus` and automatically loaded on startup. The "Run at startup" feature uses the standard Windows Registry key `HKEY_CURRENT_USER\Software\Microsoft\Windows\CurrentVersion\Run`.

## Known limitations

-   If no ambient light sensor is present or accessible(ALS sensor), only manual brightness control is available.
-   Only tested with official Apple Studio Display. Other Apple monitors are not supported.
-   The application does **not** adjust color temperature or other display parameters.

---

## License

MPL-2.0 License, following the upstream project’s terms.

---

## Thanks

Many thanks to Sam Johnson for the original work!
Feel free to submit issues, pull requests, or feedback.
