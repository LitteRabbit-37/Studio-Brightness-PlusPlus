/*
 * sbpp_bridge.h - the contract between SbppMonFilter.sys (driver/) and the
 * Studio Brightness++ app (src/BrightnessBridge.cpp).
 *
 * A brightness set through Windows (the Settings and Quick Settings sliders,
 * WmiSetBrightness, ...) reaches the driver as an ACPI _BCM; the driver hands
 * each request to the app, which writes the panel over HID as it already does
 * and reports the real level back after every change, from any source.
 *
 * Shared by kernel C and user-mode C++: keep it plain C and ASCII. Include
 * <winioctl.h> (user mode) or <ntddk.h> (kernel) first for CTL_CODE.
 *
 * Sequencing: every request bumps Sequence. A caller such as Logi Options+
 * reads the level, adds 10 and writes it back, every ~200 ms while a key is
 * held, so a report for an older request must not overwrite the level of a
 * newer one. The app echoes the Sequence of the last request it applied, and
 * the driver only takes a reported level once the app has caught up.
 */
#ifndef SBPP_BRIDGE_H
#define SBPP_BRIDGE_H

#define SBPP_BRIDGE_NT_NAME     L"\\Device\\SbppBridge"
#define SBPP_BRIDGE_DOS_NAME    L"\\DosDevices\\SbppBridge"
#define SBPP_BRIDGE_USER_PATH   L"\\\\.\\SbppBridge"

#define SBPP_BRIDGE_VERSION     1
/* Unsigned: vendor device types are >= 0x8000, and CTL_CODE shifts this into
   bit 31, which would overflow a signed int. */
#define SBPP_BRIDGE_DEVICE_TYPE 0x8A11u

/* Pended by the app; completed with an SBPP_REQUEST when a caller sets a level. */
#define IOCTL_SBPP_WAIT_REQUEST \
    CTL_CODE(SBPP_BRIDGE_DEVICE_TYPE, 0x801, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

/* App -> driver: an SBPP_REPORT with the panel's actual level. */
#define IOCTL_SBPP_REPORT_LEVEL \
    CTL_CODE(SBPP_BRIDGE_DEVICE_TYPE, 0x802, METHOD_BUFFERED, FILE_READ_ACCESS | FILE_WRITE_ACCESS)

typedef struct _SBPP_REQUEST {
    unsigned long Version;          /* SBPP_BRIDGE_VERSION */
    unsigned long Sequence;         /* 1, 2, ... since the app connected */
    unsigned long Percent;          /* requested level, 0..100 */
    unsigned long Reserved;
} SBPP_REQUEST;

#define SBPP_REPORT_ADJUSTABLE 0x00000001u  /* not under HDR or a locked colour preset */

typedef struct _SBPP_REPORT {
    unsigned long Version;          /* SBPP_BRIDGE_VERSION */
    unsigned long AppliedSequence;  /* Sequence of the last request applied, 0 for none */
    unsigned long Percent;          /* actual level, 0..100 */
    unsigned long Flags;            /* SBPP_REPORT_* */
} SBPP_REPORT;

#endif /* SBPP_BRIDGE_H */
