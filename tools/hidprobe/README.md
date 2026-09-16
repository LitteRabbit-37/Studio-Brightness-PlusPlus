# hid_probe

Read-only diagnostic that prints everything each Apple display HID interface exposes: the
top level collection, every Input and Feature cap with its usage, report ID, ranges and unit
exponent, and one Input report per report ID. It is how `docs/hid-map.md` was built, and it is
the tool to run when a new display model shows up or a sensor is not where we expect it.

Build from a Visual Studio developer prompt:

```
cl /nologo /EHsc /std:c++20 hid_probe.cpp
```

Run `hid_probe.exe` and paste the whole output. Interfaces that Windows has failed, the Code 10
ones, do not appear at all. That is expected and tells us something by itself.

The app's own log lists only the Feature value caps, so it cannot answer questions about Input
data such as where a sensor reports. This tool can.
