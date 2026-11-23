# SbppMonFilter - Windows brightness for the Apple Studio Display

**Status: experimental, test-signed.** An optional kernel filter driver. The
app does not install or need it, and works normally without it.

With it, Windows treats the Studio Display like a laptop panel:

- the **Settings > System > Display brightness slider** and the **Quick
  Settings slider** appear and drive the panel,
- Windows' brightness API works (`WmiSetBrightness`, legacy
  `IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS`) - so do callers of it, such as Logi
  Options+'s brightness keys on MX Keys keyboards,
- changes made in the app (auto-brightness, tray slider, keys, shortcuts)
  show up on Windows' sliders.

Each brightness Windows sets is handed to Studio Brightness++, which writes it
to the panel over HID as it does for its own controls. **The app has to be
running** for Windows' controls to do anything.

## How Windows decides a monitor has brightness

`monitor.sys`, the function driver of every monitor devnode, can control
brightness two ways, chosen by the `BrightnessControl` DWORD in the monitor's
software key (`HKLM\SYSTEM\CurrentControlSet\Control\Class\{4d36e96e-...}\NNNN`).
Value 3 means: this external-connector display is the system's brightness
target (bit 0), controlled through ACPI (bit 1). In ACPI mode monitor.sys sends
`IOCTL_ACPI_ASYNC_EVAL_METHOD` **down its own device stack** for the standard
ACPI brightness methods, and asks the stack for `GUID_ACPI_INTERFACE_STANDARD2`
to register for notifications.

On a laptop ACPI.sys answers from the firmware. On a desktop monitor nothing
does, so this filter sits **below** monitor.sys and answers them:

| Method | Answer |
|---|---|
| `_BCL` | 103 integers: AC default 100, DC default 100, then 0..100 |
| `_BQC` | the level the app last reported |
| `_BCM(n)` | handed to the app; succeeds once queued |
| anything else | passed down untouched |

plus an `ACPI_INTERFACE_STANDARD2` that accepts the notification registration
(general-purpose-event calls are declined). monitor.sys then publishes
`WmiMonitorBrightness` and serves the requests of the **Display Enhancement
Service** (DES), which is what the Settings and Quick Settings sliders talk to.

**DES only looks once.** It decides which displays have a brightness when it
starts. After a first install the slider shows greyed out until DES restarts;
`install-dev.ps1` restarts it for you. From then on DES picks the display up
again across monitor reconnects on its own.

With the driver in place, DES also applies the power plan's **dim display
after** setting to the display, and re-applies its brightness on resume and
display-on.

## Design

```
Settings / Quick Settings          Logi Options+, scripts      legacy \\.\LCD
            |                               |                        |
  Display Enhancement Service        root\wmi WmiSetBrightness       |
            |                               |                        |
            +---------------------> monitor.sys <--------------------+
                                        |  IOCTL_ACPI_ASYNC_EVAL_METHOD: _BCL / _BQC / _BCM
                    SbppMonFilter.sys  (KMDF lower filter on DISPLAY\APPAE3A)
                                        |
            \\.\SbppBridge  control device: the app pends IOCTL_SBPP_WAIT_REQUEST,
                            sends IOCTL_SBPP_REPORT_LEVEL (include/sbpp_bridge.h)
                                        |
            Studio Brightness++  src/BrightnessBridge.cpp: applies each request like
                                 a slider move, reports every level back, and tells
                                 Windows about its own changes
```

- **Sequencing.** Requests carry a sequence number; the app echoes the one it
  has applied, and the driver takes a reported level only once the app has
  caught up, so a late report never rewinds a newer request (Options+ sends a
  new level every ~200 ms while a key is held).
- **App -> Windows.** When the level changes inside the app, the bridge waits
  for it to settle (600 ms, so a ramp is one update) and sends the current level
  to Windows with `IOCTL_VIDEO_SET_DISPLAY_BRIGHTNESS` on `\\.\LCD` (no
  elevation needed). Windows applies it, so it comes back as a `_BCM`; the app
  recognises that echo and acknowledges without re-applying, which would
  otherwise re-anchor auto-brightness. A request from Windows cancels any local
  change still settling.
- **Requests the service drops.** `WmiSetBrightness` (and a power-plan write)
  changes the plan's *Display brightness* setting, and the Display Enhancement
  Service applies the change - unless the value equals one it applied itself in
  the last 1-3 s, which it takes for the echo of its own write and drops
  (`des.dll`, the power-setting callback in the manual brightness manager).
  Reversing direction with brightness keys hits exactly that: 42 -> 32 -> 42
  loses the second 42, and Windows then reports a level the panel is not at.
  Laptop Fn keys and the Settings slider reach the service by other routes, so
  it is not seen there. The plan setting itself always changes, so the app also
  watches it (`PowerSettingRegisterNotification`) and applies what the service
  dropped; when the service does apply a request, the driver's `_BCM` and the
  plan change carry the same level and the second is a no-op.
- **Monitor reconnects.** The filter fails the app's pended wait when its
  monitor goes away, so the app reconnects; the new filter instance retries
  creating `\\.\SbppBridge` until the old one's name is free. The app is back
  within about 2 s.
- **Fail open.** If any part of the setup fails, the filter stays attached as a
  pure pass-through. It never fails the monitor devnode.
- **One display.** The first matching monitor is bridged; a second Studio
  Display stays pass-through. The app reports and applies the active display's
  level (linked mode carries it to the others).

The app keeps sole ownership of the HID device, and the kernel side has no HID
code at all. The app also does the matching the driver cannot: the monitor
devnode and the display's USB interfaces sit in different PnP containers.

## Use

Needs the [Enterprise WDK](https://learn.microsoft.com/windows-hardware/drivers/download-the-wdk)
ISO (the build scripts look for `Downloads\EWDK*.iso`, or pass `-EwdkIso`).

```powershell
# 1. build and test-sign (unelevated). The test certificate is created once in
#    your CurrentUser store, private key non-exportable.
pwsh -File driver\build-driver.ps1

# 2. once, elevated, then reboot (Secure Boot must be off):
bcdedit /set testsigning on

# 3. elevated: restore point, trust the test certificate, install, restart DES
pwsh -File driver\install-dev.ps1

# 4. start Studio Brightness++, then check (no elevation needed)
pwsh -File driver\driver-status.ps1
pwsh -File driver\driver-status.ps1 -SetTo 60

# 5. undo, elevated - then bcdedit /set testsigning off and reboot
pwsh -File driver\uninstall-dev.ps1 -RemoveCertificate
```

`capture-trace.ps1` (elevated) records an ETW trace of the driver
(TraceLogging provider `StudioBrightnessPlusPlus.SbppMonFilter`,
`{c43e7bfe-0d43-589f-3d47-c8b80e7a7919}`) together with monitor.sys and the
power and display providers, for working out what Windows asked and when.

| File | |
|---|---|
| `SbppMonFilter/SbppMonFilter.c` | The filter. |
| `SbppMonFilter/SbppMonFilter.inf` | Extension INF: adds the lower filter via `[DDInstall.Filters]` and sets `BrightnessControl`; `monitor.inf` stays the function driver. |
| `SbppMonFilter/SbppMonFilter.vcxproj` | KMDF 1.15, x64. |
| `build-driver.ps1` | EWDK build, test-signs the `.sys` and `.cat`. |
| `install-dev.ps1` | Elevated install. |
| `uninstall-dev.ps1` | Elevated removal: packages, filter, service, `BrightnessControl`; `-RemoveCertificate`. |
| `driver-status.ps1` | Stack position, setup results, app connection, Windows' level; `-SetTo`. |
| `capture-trace.ps1` | ETW capture. |

## Tested

Studio Display (2022), Windows 11 26300, NVIDIA RTX 3080 over a Thunderbolt
add-in card:

- The Settings and Quick Settings sliders drive the panel, fast drags included.
- `WmiSetBrightness` and the legacy `\\.\LCD` set each produced one `_BCM`,
  applied by the app; Windows' level matched afterwards.
- Logi Options+ MX Keys brightness keys: +-10 per press, ~216 ms repeat while
  held, fast direction reversals included - every press reached the panel in
  order (71 requests in one run).
- App-side changes (shortcuts, an auto-brightness ramp) reached Windows as one
  settled level; the echo was not re-applied.
- Monitor devnode restarts with the app connected: the app reconnected in about
  2 s each time, and Windows, the app and the panel agreed after the next set.

Not yet tested: other Apple displays, a reboot cycle with the driver already
installed, and PCs with a built-in display.

## Limitations and risks

- **Test-signing mode** is required until the driver is properly signed
  (attestation signing needs an EV code-signing certificate). It puts a "Test
  Mode" watermark on the desktop and makes some kernel anti-cheat refuse to run.
  `install-dev.ps1` also adds the test certificate to the machine's trusted
  root and trusted publisher stores; `uninstall-dev.ps1 -RemoveCertificate`
  takes it out.
- **Uninstall before turning test-signing off.** With test-signing off Windows
  refuses the test-signed filter, and a filter that fails to load takes the
  monitor devnode down with it - which is where colour profiles, HDR
  capabilities and EDID hang. Order: `uninstall-dev.ps1`, then
  `bcdedit /set testsigning off`, then reboot.
- **Studio Display (2022) only** (`MONITOR\APPAE3A`). Other Apple displays need
  their monitor hardware ID added to the INF, and testing.
- **Desktops only.** `BrightnessControl` makes this display the system's one
  brightness target; on a PC with a built-in display it would compete with it.
- **The app must be running.** Without it a `_BCM` is only stored, and Windows'
  slider moves without the panel following.
- **Requests from Windows count as user actions** in the app, so they re-anchor
  auto-brightness - including DES's dim-on-idle and its re-apply on resume.
- **Kernel code.** The filter sits on the monitor devnode, which carries EDID,
  brightness and WMI - the GPU drives the picture - so a filter that fails to
  start should leave the screen working. A bug in it is still a bugcheck. If the
  local display is unusable, `uninstall-dev.ps1` can be run from an elevated
  remote shell.
