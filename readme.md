<p align="center">
  <img src="studio-brightness-plusplus.ico" />
</p>

# Studio brightness ++

> **Note: This project is a fork and extension of [studio-brightness](https://github.com/sfjohnson/studio-brightness) by Sam Johnson (sfjohnson).**

## What is it?

**Studio brightness ++** is a small Windows utility for controlling the brightness of an Apple Studio Display:

-   **Automatic brightness adjustment** based on ambient light (if your system has a compatible ALS sensor)
-   **Manual brightness control** via the native keyboard brightness keys (the “sun” keys, Fn+F1/F2, or QMK/VIA custom keys)
-   Runs in the system tray, lightweight and easy to use

## What’s new in this version?

Compared to the original, this fork adds:

-   **Automatic brightness:** Adjusts your Studio Display’s brightness to match the ambient light in your room
-   **Native brightness key support:** Use your keyboard’s built-in brightness up/down keys, just like on macOS
-   **16-level brightness scale:** Matches the macOS/Apple standard for number of brightness steps between minimum and maximum
-   **Revised Studio Display detection** (`hid.cpp` has been rewritten for more robust device matching)

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

1. Open a **Developer Command Prompt for VS**
2. Go to the project directory
3. Run:

```bash
build.bat
```

The output will be `bin\studio-brightness-plusplus.exe`.

## Usage

-   **Increase brightness:** Use your keyboard’s native brightness up key (the sun/F2 key, or your QMK/VIA-assigned key)
-   **Decrease brightness:** Use your keyboard’s brightness down key (the sun/F1 key, or your QMK/VIA-assigned key)
-   **Automatic brightness:** If you have a Windows-compatible ambient light sensor, the display will auto-adjust as lighting changes
-   **Quit:** Right-click the tray icon and select “Quit”

## Technical notes

-   The file **`hid.cpp`** was rewritten to more reliably detect the Apple Studio Display (VID 05ac / PID 1114). The detection logic may differ from the original upstream.
-   Brightness levels are split into **10 steps** (to match the windows slider but sadly it is not yet synchronised).
-   Brightness key events are captured through HID RawInput (Consumer Control page), making this compatible with Apple, Windows, and QMK/VIA keyboards with proper key assignments.

## Known limitations

-   If no ambient light sensor is present or accessible, only manual brightness control is available.
-   Only tested with official Apple Studio Display. Other Apple monitors are not supported.
-   The application does **not** adjust color temperature or other display parameters.

---

## License

MPL-2.0 License, following the upstream project’s terms.

---

## Thanks

Many thanks to Sam Johnson for the original work!
Feel free to submit issues, pull requests, or feedback.
