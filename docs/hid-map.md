# Apple Display HID Interface Map

Reverse engineered layout of the HID interfaces Apple displays expose over USB-C on Windows.
Every display uses Apple VID `0x05AC`. Everything here comes from real hardware, either from a
live probe or from a contributor's log, except where a line says otherwise.


## 1. Usage page reference

### Page 0x0082, Monitor

| Usage | Direction | Meaning |
|---|---|---|
| 0x0010 | Feature, read and write | Brightness. 32 bit, logical range 400 to 60000 on every model seen so far. This is the whole brightness protocol. |
| 0x0066 | Feature, write | Present on some models. Boot Camp probes for it to decide whether a display supports automatic brightness, then never writes it. Its two setters are dead code. Not a second brightness control. |

### Page 0x0020, HID Sensors

Two top level collections use this page. Usage 0x0041 is the ambient light sensor, usage 0x008A is
the device orientation sensor.

Ambient light data fields, all Input, all 32 bit:

| Usage | Meaning | Unit exponent, same on Gen 1 and XDR |
|---|---|---|
| 0x04D1 | Illuminance | 13, meaning 10^-3, so the value is milli lux |
| 0x04D2 | Correlated colour temperature in Kelvin | 0, so the value is Kelvin directly |
| 0x04D4 | CIE 1931 chromaticity x | 8, meaning 10^-8 |
| 0x04D5 | CIE 1931 chromaticity y | 8, meaning 10^-8 |

The two models differ in behaviour, not in format. The XDR reports whole lux, every raw value is a
multiple of 1000, where the Gen 1 resolves to a thousandth. And in the dark the XDR reads a plain 0
with the three colour fields at 0 as well, because it does not estimate a colour from a few
photons, where the Gen 1 still reports 0.17 lux and a colour. An all zero report from the XDR is a
real reading of darkness, not a sensor fault. Auto brightness has to act on it, and rejecting it
would leave the app blind in a dark room. The app only treats the colour part as absent.

The colour fields were checked on the XDR by holding a phone showing red, green and blue in front
of each sensor: x=0.63 y=0.34 for red, x=0.29 y=0.71 for green, x=0.16 y=0.06 for blue, which are
the three corners of the CIE diagram.

Orientation data fields, all Input, 9 bit, logical range 0 to 360:

| Usage | Meaning |
|---|---|
| 0x047F | Tilt X. Stays 0 on a Studio Display. |
| 0x0480 | Tilt Y. This is the in plane screen rotation angle, the one that matters. Counter clockwise positive, so a 90 degree clockwise physical rotation reads 270. |
| 0x0481 | Tilt Z. Stays 0 on a Studio Display. |

Sensor properties seen in the ambient light Feature report:

| Usage | Meaning | Notes |
|---|---|---|
| 0x030E | Report Interval | Range 200 to 10000 ms on Gen 1. |
| 0x0304 | Minimum Report Interval | Range 0 to 10000. |
| 0x0318 | Undecoded | Count of 10, range -32767 to 32767. Looks like a calibration array. |
| 0x14D1, 0x14D3 | Change sensitivity, absolute, for illuminance and chromaticity | Standard HID sensor modifier prefix 0x1000. |
| 0xE4D1 | Change sensitivity, percent relative, for illuminance | Modifier prefix 0xE000. |

Named arrays in the same report show up as Feature **button** caps, not value caps. This matters in
code: a lookup through `HidP_GetValueCaps(HidP_Feature, ...)` will never find them. The selectors
present are 0x0840 and 0x0841 for Reporting State, 0x0831 for Power State, 0x0800 to 0x0806 for
Sensor State, and 0x0850 to 0x0855 for Sensor Event. There is also one Apple vendor button on page
`0xFF15` usage 0x0101 inside the sensor collection, which is undocumented.

Writing Reporting State is not necessary. `HidD_GetInputReport` answers regardless of the reporting
state, which is also how Boot Camp reads the sensor.

### Page 0xFF20, Apple vendor, colour presets

This is the Reference Mode surface, fully decoded and in use by the app.

| Usage | Direction | Meaning |
|---|---|---|
| 0x0002 | Feature | Undecoded. Same report as 0x0003, same shape. |
| 0x0003 | Feature, read and write | Active preset index. Write an index here to switch preset. |
| 0x0004 | Feature, write | Enumeration cursor. Write index i to select which preset the read usages describe. |
| 0x0005 | Feature, read | Per preset boolean. Reads 0 for all nine presets on Gen 1 at rest. Meaning unknown, Boot Camp never reads it. Logged per preset so a tester log on another model may reveal it. |
| 0x0006 | Feature, read | Validity flag for the cursor preset. Zero means stop enumerating. |
| 0x0008 | Feature, read | Preset name, UTF-16. |
| 0x0009 | Feature, read | Preset description, UTF-16. |
| 0x000A to 0x000E | Feature, read | Undecoded buffers of 2, 64, 2, 192 and 16 bytes on the same report as the name. |

Two gotchas. The value caps are 4 wide arrays, not scalars, but a scalar `HidP_SetUsageValue`
writes the first field correctly, which is what Boot Camp does. And the cursor and active index
reports are write only on the XDR: a `HidD_GetFeature` on them stalls, so a read modify write aborts
enumeration at index 0. Build a clean zeroed report and write it, never read it back first.

### Page 0xFF28, Apple vendor, undecoded

Three usages on one Feature report, 0x0001 as a single byte with range 0 to 127, then buffers of 764
and 256 bytes. All three read zero at rest on Gen 1, so it is not a read to get data channel.
Probably a write channel for custom colour or calibration data, or a command and response surface
where writing 0x0001 populates the other two. Boot Camp never touches it. Best candidate for the
display side of True Tone.

### Page 0xFF16, Apple vendor, undecoded

Usage 0x0003 as a single byte and usage 0x0004 as 8 bytes, both range 0 to 255, on one Feature
report. Present on every model. Boot Camp never touches it. Likely firmware update or identity.

### Page 0x000F, usage 0x0050

Sits next to brightness on MI_07, 16 bit, range 0 to 20000. Undecoded.

### Page 0xFF15, usage 0x0003, XDR only

On the XDR, MI_07 has a second collection next to the brightness one, top level 0xFF00 / 0x0037.
It carries one 64 bit Input value on page 0xFF15 usage 0x0003 and one Feature value that reuses the
sensor usage 0x030E, both declared with the brightness unit code, exponent -2 and the brightness
range 400 to 60000. The Input report reads `06 D0 07 00 00 58 02 00 00`, which as two 32 bit
values is 2000 and 600. Those look like the panel's peak and SDR nit figures, the XDR preset is
named P3-2000 nits and 600 is the Gen 1 figure, but the declared exponent would make them 20 and
6, so either the firmware ignores the exponent on this field or the guess is wrong. Not present on
Gen 1. Undecoded, see part 4.

Worth noting in passing: brightness itself, range 400 to 60000 with exponent -2, is 4.00 to 600.00
nits. The brightness control is in hundredths of a nit.

## 2. Per model interface map

### Studio Display, PID 0x1114

| Interface | Top level collection | Contents | Windows status |
|---|---|---|---|
| MI_05 | 0xFF00 / 0x0053 | Input report of 26 bytes, no Feature caps | Starts, input read denied |
| MI_06 col01 | 0xFF00 / 0x000B | Page 0xFF16, usages 0x0003 and 0x0004 | Starts |
| MI_06 col02 | 0xFF00 / 0x0002 | Page 0xFF20, colour presets, 12 caps | Starts |
| MI_06 col03 | 0xFF00 / 0x0047 | Page 0xFF28, 3 caps | Starts |
| MI_06 col04 | 0xFF00 / 0x003A | Input stream of 2001 bytes, output of 5 | Starts |
| MI_07 | 0x0080 / 0x0001 | Brightness on 0x0082 / 0x0010, plus 0x000F / 0x0050 | Starts |
| MI_08 | 0x0020 / 0x0041 | Ambient light sensor | Sensor class by default, no Code 10. Needs the null driver to be readable as raw HID. |
| MI_09 | 0x0020 / 0x008A | Orientation sensor | **Code 10** without the null driver |

Nine Reference Modes, index 0 to 8, starting with Apple Display (P3-600 nits) as the factory
default. Only that first one leaves brightness adjustable. Every calibrated mode locks it.

### Studio Display XDR, PID 0x1116

| Interface | Contents | Windows status |
|---|---|---|
| MI_05 | Vendor | Starts |
| MI_06 col01 to col04 | Same vendor pages as Gen 1, with 0xFF20 carrying more presets and 0xFF28 carrying 7 caps instead of 3 | Starts |
| MI_07 col01 | Brightness on 0x0082 / 0x0010 | Starts |
| MI_07 col02 | 0xFF00 / 0x0037: one 64 bit Input on page 0xFF15 and a Feature reusing 0x030E, brightness units, reads 2000 and 600 | Starts. Not a sensor. See page 0xFF15 above. |
| MI_08 | Two ambient light sensors, same fields and exponents as Gen 1. col01 is the front one, in the upper left corner of the bezel, report ID 0x01. col02 is the rear one, report ID 0x02. | **Code 10**, for a different reason than MI_09, see below |
| MI_09 | Orientation sensor | **Code 10** without a null driver |

Under room light the front sensor reads higher than the rear one, 43 against 17 lux in one setup,
so the brightest of the two, which is what the app uses, is normally the front. Either sensor under
a phone flashlight reads tens of thousands of lux, far above the 5000 lux the brightness mapping
clamps to, which is also where Boot Camp clamps.

Sixteen Reference Modes. Only the two named Apple XDR Display leave brightness adjustable, and they
are the only ones compatible with Windows HDR. Switching to any other preset while HDR is on blanks
the panel.

MI_08 on this model is a different problem from every other Code 10 here. The report descriptor
declares two top level collections, an empty vendor collection with no report ID followed by the
ambient light collection with report ID 1:

```
06 00 FF 09 1A A1 01 C0     vendor 0xFF00, empty, no report ID
05 20 09 41 A1 01 85 01     Ambient Light 0x20 / 0x41, report ID 1
```

`hidclass.sys` refuses that mix and fails the device with "a top level collection does not have a
declared report ID or has a report ID that spans multiple collections". macOS tolerates it. The
consequence is that Windows creates no HID child device at all, so there is nothing to configure and
no interface to open. A null driver cannot help. Only kernel code running before that decision can,
which is why this one case needs a filter driver that rewrites the descriptor. Switching USB
configuration does not help either, the descriptor is identical in all three.

### Studio Display Gen 2, PID 0x1118

Contributed by [oskarjiang](https://github.com/LitteRabbit-37/Studio-Brightness-PlusPlus/pull/9).
MI_05 to MI_09 exist at the USB level with the same shape as Gen 1. The HID children of MI_08 and
MI_09 have never been listed, because the dump filtered on the HIDClass class and those two land
under the Sensor class. Whether this model has the Gen 1 descriptor or the XDR one is still unknown,
and that decides whether it needs a null driver only or a filter driver too.

### Pro Display XDR, PID 0x9243

No hardware available. Apple's own INF places the ambient light sensors on MI_00 and MI_01 and the
orientation sensor on MI_03, which is a different layout from the Studio Displays. It also needs a
second Apple driver to switch the display into the right USB configuration.

## 3. Practical notes

### Unit exponents

The sensor values are not in their natural units. HID declares a unit exponent as a 4 bit signed
nibble, where 0 to 7 are positive and 8 to 15 mean -8 to -1, and the reported value has to be
multiplied by 10 to that power.

`HidP_GetScaledUsageValue` does **not** apply it. That function maps the logical range onto the
physical range, and Apple leaves PhysicalMin and PhysicalMax at 0, which the HID specification
defines as physical equals logical. So it hands the raw value straight back. Reading raw with
`HidP_GetUsageValue` and applying the exponent yourself is the only correct path.

Measured on a Studio Display (2022):

| Field | Raw | Unit exponent | True value |
|---|---|---|---|
| Illuminance | 35264 | 13, so 10^-3 | 35.264 lux |
| Colour temperature | 3538 | 0 | 3538 K |
| Chromaticity x | 41714574 | 8, so 10^-8 | 0.41715 |
| Chromaticity y | 39507029 | 8, so 10^-8 | 0.39507 |

Treating milli lux as lux makes the app read an ordinary room as brighter than direct daylight.
Auto brightness then pins the panel near its minimum and stops responding.

### Which driver each model needs

| Model | Orientation | Ambient light | Signed by Apple |
|---|---|---|---|
| Studio Display, 0x1114 | AppleDisplayNull64.inf | AppleDisplayNull64.inf, or leave it on the Windows Sensor API | Yes |
| Pro Display XDR, 0x9243 | AppleDisplayNull64.inf | AppleDisplayNull64.inf, plus the USB composite device INF | Yes |
| LG UltraFine | AppleDisplayNull64.inf | AppleDisplayNull64.inf | Yes |
| Studio Display Gen 2, 0x1118 | Null INF, PIDs absent from Apple's file | Unknown | No |
| Studio Display XDR, 0x1116 | Null INF, PIDs absent from Apple's file | Null INF plus a filter driver carrying a .sys | No |

`AppleDisplayNull64.inf` ships with Boot Camp Update 6.1.17. It is signed by Apple with a Microsoft
trusted certificate, so a single elevated `pnputil /add-driver ... /install` is enough, with no test
mode and no Secure Boot change.

Installing it on a Studio Display moves MI_08 off the Windows Sensor API and onto raw HID. An app
that only knows the Sensor API loses automatic brightness the moment the driver is installed, which
is why raw HID reading and the null driver go together.

Signing. A package with only an INF and no .sys loads nothing into the kernel, so its signature is
checked at install time and never again, and a self signed certificate in the trusted stores is
enough. A package containing a .sys is loaded on every boot and needs a signature Windows accepts at
load time, which in practice means Microsoft attestation signing. Only the XDR ambient light sensor
needs that second kind.

## 4. What is still unknown, and how to get it

**True Tone.** The read half is done: the ambient sensor reports colour temperature and CIE
chromaticity alongside illuminance, in the same input report, and the app reads all of it. The write
half, changing the display white point, is undecoded. Boot Camp does not implement it, a search of
its full decompilation finds no 0xFF28, no 0xFF16 and no white point logic anywhere, so there is
nothing to copy on Windows.

The way to get it is a USB capture on macOS while macOS drives True Tone. Enable True Tone in System
Settings, start the capture, change the room lighting enough to force a reaction, and look at the
Feature SET_REPORT traffic on MI_06. A write to 0xFF28 should stand out, since that page reads zero
at rest and Boot Camp never touches it.

A white point shift can also be approximated host side through the GPU gamma ramp, the way Windows
Night Light does. No driver and no reverse engineering needed, but it breaks under HDR, Windows
clamps the ramp, and it affects colour managed applications.

**Whether the XDR sensors can be reached without the filter driver.** Settled, they cannot. A
probe of an XDR with no third party driver installed lists MI_05, MI_06 and MI_07 only. MI_08 and
MI_09 do not exist as HID devices until the descriptor fix and the null driver are installed.

**MI_07 col02 on the XDR.** The 2000 and 600 it reports look like nit figures but the declared
exponent says otherwise, see page 0xFF15. What a Gen 2 shows there, if anything, and whether the
values change with the active preset, would settle it.

**0xFF28 and 0xFF16.** Both undecoded on every model. 0xFF28 is the interesting one.

**0xFF20 usages 0x0002, 0x0005 and 0x000A to 0x000E.** Present on the preset interface, ignored by
Boot Camp, meaning unknown. The app logs 0x0005 per preset so a tester log on an unfamiliar model
may show it varying.

## Contributing

If you have an Apple display that is not fully described here, run Studio Brightness++ and share
the log. The enumeration phase logs every interface, collection and Feature value cap, and the
ambient sensor line includes the unit exponents, which is what we need to scale a new panel.

For the sensor interfaces, use this listing rather than one filtered on HIDClass, which hides the
devices that land under the Sensor class:

```
Get-PnpDevice | Where-Object { $_.InstanceId -like "*VID_05AC*" } |
  Select-Object Status, Class, InstanceId, FriendlyName
```

## NTSTATUS reference

| Code | Meaning |
|---|---|
| 0x00110000 | HIDP_STATUS_SUCCESS |
| 0xC0110004 | HIDP_STATUS_USAGE_NOT_FOUND |
| 0xC0110007 | HIDP_STATUS_INCOMPATIBLE_REPORT_ID |
