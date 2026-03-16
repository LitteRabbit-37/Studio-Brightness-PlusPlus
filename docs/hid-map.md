# Apple Display HID Interface Map

Reverse-engineered HID interface layout for Apple displays connected via USB-C on Windows.
All displays use Apple VID `0x05AC`. Data collected from real hardware.

## Studio Display (Gen 1) — PID 0x1114

| Interface | Usage Page | Usage | Function |
|-----------|-----------|-------|----------|
| MI_05 | — | — | No Feature value caps |
| MI_06&col01 | 0xFF16 | 0x0003, 0x0004 | Apple vendor specific (ReportID 0x01, 8-bit) |
| MI_06&col02 | 0xFF20 | 0x0002 to 0x000E | Apple vendor specific (12 caps) |
| MI_06&col03 | 0xFF28 | 0x0001 to 0x0003 | Apple vendor specific (3 caps) |
| MI_06&col04 | — | — | No Feature value caps |
| MI_07 | **0x0082** | **0x0010** | **Brightness control** (32-bit, range 400-60000, ReportID 0x01) |
| MI_07 | 0x000F | 0x0050 | Sensor related (16-bit, range 0-20000) |
| MI_08 | — | — | **Ambient Light Sensor** (Windows Sensor API) |
| MI_09 | — | — | Orientation Sensor (Code 10, driver incompatible) |

- Brightness on **MI_07** (no `&col` subcollection)
- ALS on **MI_08**: working, exposed via `ISensorEvents` / `sensorshidclassdriver.inf`
- Orientation on **MI_09**: Code 10 (`STATUS_INVALID_PARAMETER`), Apple descriptor incompatible with Windows driver

## Studio Display XDR — PID 0x1116

| Interface | Usage Page | Usage | Function |
|-----------|-----------|-------|----------|
| MI_05 | — | — | No Feature value caps |
| MI_06&col01 | 0xFF16 | 0x0003, 0x0004 | Apple vendor specific (ReportID 0x01, 8-bit) |
| MI_06&col02 | 0xFF20 | 0x0002 to 0x0012 | Apple vendor specific (12 caps) |
| MI_06&col03 | 0xFF28 | 0x0001 to 0x0009 | Apple vendor specific (7 caps) |
| MI_06&col04 | — | — | No Feature value caps |
| MI_07&col01 | **0x0082** | **0x0010** | **Brightness control** (32-bit, range 400-60000, ReportID 0x01) |
| MI_07&col01 | 0x000F | 0x0050 | Sensor related (16-bit, range 0-20000) |
| MI_07&col02 | 0x0020 | 0x030E | Sensor data (32-bit, range 400-60000) |
| MI_08 | — | — | **Ambient Light Sensor** (Code 10, driver fails to load) |
| MI_09 | — | — | Orientation Sensor (Code 10, driver incompatible) |

- Brightness on **MI_07&col01**
- ALS on **MI_08**: Code 10, HID report descriptor validation failed. Error: "A top-level collection does not have a declared report ID or has a report ID that spans multiple collections." This is a firmware incompatibility with the Windows HID class driver (`hidclass.sys`).
- Orientation on **MI_09**: same Code 10 as Gen 1

## Studio Display (Gen 2) — PID 0x1118

No hardware available for testing. Expected to follow the same layout as Gen 1.

## Pro Display XDR — PID 0x9243

No hardware available for testing.

## Comparison

| | Gen 1 (0x1114) | XDR (0x1116) |
|---|---|---|
| Brightness interface | MI_07 | MI_07&col01 |
| UsagePage / Usage | 0x0082 / 0x0010 | 0x0082 / 0x0010 |
| Brightness range | 400-60000 | 400-60000 |
| Feature ReportID | 0x01 | 0x01 |
| Bit size | 32 | 32 |
| ALS (MI_08) | Working | Code 10 (descriptor issue) |
| Orientation (MI_09) | Code 10 | Code 10 |

Both displays share the same brightness protocol. The only structural difference is that the XDR uses HID collections (`&col`) under MI_07, while Gen 1 exposes MI_07 as a single top-level device.

## Key findings

**Brightness is always on Usage Page 0x0082, Usage 0x0010.** The interface and collection
vary between models, so the code scans all HID interfaces (including `&col` subcollections)
and matches by UsagePage/Usage rather than hardcoding an interface number.

**MI_05 has no Feature caps on any tested model.** Previous versions of the app targeted MI_05
using Input reports, which partially worked for setting brightness but failed for reading.
The correct approach is Feature reports on MI_07.

**ALS (Ambient Light Sensor)** is on MI_08 for all models. It is exposed to Windows via the
Sensor API (`ISensorEvents`), not via HID. On Gen 1, the driver loads successfully. On the XDR,
the HID descriptor has a structural issue (shared/missing report IDs across top-level collections)
that causes `hidclass.sys` to reject the device. This is a firmware issue on Apple's side.

**Orientation Sensor** is on MI_09 for all models. Code 10 on all tested hardware.
The Apple descriptor uses non-standard fields that `sensorshidclassdriver.inf` cannot parse.

**Vendor-specific pages (0xFF16, 0xFF20, 0xFF28)** are present on all models under MI_06.
Their purpose is unknown (likely firmware updates, color profiles, True Tone calibration).

## NTSTATUS reference

| Code | Meaning |
|------|---------|
| 0x00110000 | HIDP_STATUS_SUCCESS |
| 0xC0110004 | HIDP_STATUS_USAGE_NOT_FOUND |
| 0xC0110007 | HIDP_STATUS_INCOMPATIBLE_REPORT_ID |

## Contributing

If you have an Apple display not listed here, run Studio Brightness++ and share
the full logs. The enumeration phase logs every HID interface, collection, and
Feature value cap, which is all we need to map a new model.
