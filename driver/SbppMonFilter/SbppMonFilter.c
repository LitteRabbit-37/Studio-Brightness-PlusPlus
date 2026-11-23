/*
 * SbppMonFilter.c - Windows brightness for the Apple Studio Display
 *
 * KMDF lower filter on the Studio Display's monitor devnode (MONITOR\APPAE3A),
 * between monitor.sys and the display adapter's child PDO.
 *
 * The panel's brightness lives on USB HID, which Windows' brightness stack
 * knows nothing about, so Windows reports the display as having no brightness
 * control. This filter makes monitor.sys treat it like a laptop panel:
 *
 *   - The INF sets "BrightnessControl" = 3 in the monitor's software key. Bit 0
 *     makes this external-connector display the system's brightness target;
 *     bit 1 says to control it through ACPI.
 *   - monitor.sys then evaluates the ACPI brightness methods - _BCL (supported
 *     levels), _BQC (current level), _BCM(level) (set) - with
 *     IOCTL_ACPI_ASYNC_EVAL_METHOD, V1 buffers, sent down its own stack. On a
 *     laptop ACPI.sys answers from the firmware; here this filter answers.
 *   - It also asks for GUID_ACPI_INTERFACE_STANDARD2 to register for brightness
 *     notifications; this filter offers one (without it, _BCM is never sent).
 *
 * monitor.sys then publishes WmiMonitorBrightness and accepts the set requests
 * of the Display Enhancement Service, which drives the Settings and Quick
 * Settings sliders.
 *
 * A _BCM(level) goes to the Studio Brightness++ app through the control device
 * \\.\SbppBridge (include\sbpp_bridge.h). The app writes the panel over HID and
 * reports the real level back, which _BQC returns.
 *
 * Rules this file keeps:
 *   - Fail open. Any setup failure leaves a pass-through filter; the monitor
 *     devnode is never failed.
 *   - Only _BCL, _BQC and _BCM are answered. Everything else, including input
 *     that does not parse, is passed down untouched.
 *   - PASSIVE_LEVEL throughout (devices are ExecutionLevelPassive), one wait
 *     lock. The ACPI notify callbacks may come at DISPATCH_LEVEL and only use
 *     interlocked operations.
 *
 * Diagnostics: TraceLogging provider "StudioBrightnessPlusPlus.SbppMonFilter"
 * {c43e7bfe-0d43-589f-3d47-c8b80e7a7919} (driver\capture-trace.ps1), plus a
 * small status snapshot under
 * HKLM\SYSTEM\CurrentControlSet\Services\SbppMonFilter\Parameters, written
 * only when the state changes (driver\driver-status.ps1).
 */

#include <ntddk.h>
#include <wdf.h>
#include <acpiioct.h>
#include <initguid.h>
#include <TraceLoggingProvider.h>
#include "sbpp_bridge.h"

/* GUID_ACPI_INTERFACE_STANDARD2 (wdmguid.h), defined here to avoid pulling in all of it. */
DEFINE_GUID(SBPP_GUID_ACPI_INTERFACE_STANDARD2,
    0xe8695f63, 0x1831, 0x4870, 0xa8, 0xcf, 0x9c, 0x2f, 0x03, 0xf9, 0xdc, 0xb5);

/* Name-hashed, as EventSource and tracelog derive it from the provider name. */
TRACELOGGING_DEFINE_PROVIDER(g_SbppTrace, "StudioBrightnessPlusPlus.SbppMonFilter",
    (0xc43e7bfe, 0x0d43, 0x589f, 0x3d, 0x47, 0xc8, 0xb8, 0x0e, 0x7a, 0x79, 0x19));

#define SBPP_TAG 'ppbS'

/* Brightness levels, 0..100 percent, as Windows and the app both count them. */
#define SBPP_LEVEL_COUNT        101
#define SBPP_INITIAL_BRIGHTNESS 50

/* ---- ACPI: V1 forms only, as monitor.sys uses them ----------------------- */

#define SBPP_ACPI_NAME(a, b, c, d) \
    ((ULONG)(UCHAR)(a) | ((ULONG)(UCHAR)(b) << 8) | ((ULONG)(UCHAR)(c) << 16) | ((ULONG)(UCHAR)(d) << 24))
#define SBPP_ACPI_BCL SBPP_ACPI_NAME('_', 'B', 'C', 'L')
#define SBPP_ACPI_BQC SBPP_ACPI_NAME('_', 'B', 'Q', 'C')
#define SBPP_ACPI_BCM SBPP_ACPI_NAME('_', 'B', 'C', 'M')

/* acpiioct.h's signatures and codes are int expressions; compare as ULONG. */
#define SBPP_SIG_PLAIN    ((ULONG)ACPI_EVAL_INPUT_BUFFER_SIGNATURE_V1)
#define SBPP_SIG_INTEGER  ((ULONG)ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_SIGNATURE_V1)
#define SBPP_SIG_COMPLEX  ((ULONG)ACPI_EVAL_INPUT_BUFFER_COMPLEX_SIGNATURE_V1)
#define SBPP_SIG_OUTPUT   ((ULONG)ACPI_EVAL_OUTPUT_BUFFER_SIGNATURE_V1)
#define SBPP_IOCTL_EVAL   ((ULONG)IOCTL_ACPI_EVAL_METHOD_V1)
#define SBPP_IOCTL_AEVAL  ((ULONG)IOCTL_ACPI_ASYNC_EVAL_METHOD_V1)

/* The generic ACPI_METHOD_ARGUMENT_LENGTH follows the generic type alias,
   which is V2 on newer targets; size V1 arguments explicitly. */
#define SBPP_ARG_V1_LEN(dataLength) \
    ((ULONG)FIELD_OFFSET(ACPI_METHOD_ARGUMENT_V1, Data) + \
     (((ULONG)(dataLength) > (ULONG)sizeof(ULONG)) ? (ULONG)(dataLength) : (ULONG)sizeof(ULONG)))
#define SBPP_OUT_V1_HEADER ((ULONG)FIELD_OFFSET(ACPI_EVAL_OUTPUT_BUFFER_V1, Argument))
#define SBPP_COMPLEX_V1_MIN \
    ((ULONG)FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX_V1, Argument) + \
     (ULONG)FIELD_OFFSET(ACPI_METHOD_ARGUMENT_V1, Data) + (ULONG)sizeof(ULONG))

/*
 * _BCL: by ACPI convention the level on AC power, the level on battery, then
 * every supported level. 100/100: a desktop display has no battery, and no
 * reason to suggest a different default to whoever reads them.
 */
#define SBPP_BCL_DEFAULT_AC 100
#define SBPP_BCL_DEFAULT_DC 100
#define SBPP_BCL_COUNT      (2 + SBPP_LEVEL_COUNT)

static ULONG g_BclValues[SBPP_BCL_COUNT];

/*
 * When the monitor devnode restarts while the app holds \\.\SbppBridge open,
 * the old control device keeps its name until the app closes its handle, so
 * creating the new one fails with STATUS_OBJECT_NAME_COLLISION. Retry every
 * 500 ms for up to a minute.
 */
#define SBPP_CONTROL_RETRY_MS    500
#define SBPP_CONTROL_RETRY_LIMIT 120

/* ---- State ---------------------------------------------------------------- */

typedef struct _SBPP_BRIDGE {
    WDFWAITLOCK Lock;            /* NULL: bridge disabled, filter is pass-through */
    WDFDEVICE   FilterDevice;    /* the one bridged monitor */
    WDFDEVICE   ControlDevice;   /* \Device\SbppBridge */
    WDFQUEUE    WaitQueue;       /* pended IOCTL_SBPP_WAIT_REQUEST (manual queue) */
    BOOLEAN     ClientConnected;
    BOOLEAN     PendingValid;
    UCHAR       Level;           /* what _BQC returns */
    UCHAR       PendingPercent;
    ULONG       PendingSeq;
    ULONG       RequestSeq;
} SBPP_BRIDGE;

static SBPP_BRIDGE g_Bridge;

/*
 * monitor.sys's ACPI notification callback, registered through our
 * ACPI_INTERFACE_STANDARD2. Stored so the registration succeeds; nothing in
 * this design needs to raise a notification.
 */
static PVOID volatile g_NotifyHandler;
static PVOID volatile g_NotifyContext;

typedef struct _SBPP_CONTROL_CONTEXT {
    WDFQUEUE WaitQueue;
} SBPP_CONTROL_CONTEXT, *PSBPP_CONTROL_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SBPP_CONTROL_CONTEXT, SbppControlGetContext)

typedef struct _SBPP_RETRY_CONTEXT {
    ULONG Tries;
} SBPP_RETRY_CONTEXT, *PSBPP_RETRY_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(SBPP_RETRY_CONTEXT, SbppRetryGetContext)

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_UNLOAD SbppEvtDriverUnload;
EVT_WDF_DRIVER_DEVICE_ADD SbppEvtDeviceAdd;
EVT_WDF_OBJECT_CONTEXT_CLEANUP SbppEvtFilterCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SbppEvtFilterIoDeviceControl;
EVT_WDF_IO_QUEUE_IO_INTERNAL_DEVICE_CONTROL SbppEvtFilterIoInternalDeviceControl;
EVT_WDF_TIMER SbppEvtControlRetry;
EVT_WDF_DEVICE_FILE_CREATE SbppEvtFileCreate;
EVT_WDF_FILE_CLEANUP SbppEvtFileCleanup;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL SbppEvtIoDeviceControl;

#ifdef ALLOC_PRAGMA
#pragma alloc_text(INIT, DriverEntry)
#pragma alloc_text(PAGE, SbppEvtDeviceAdd)
#endif

/* ------------------------------------------------------------------------- */
/* Status snapshot (Parameters key), written on state changes only           */
/* ------------------------------------------------------------------------- */

static const UNICODE_STRING SbppValAttachTime   = RTL_CONSTANT_STRING(L"AttachTime");
static const UNICODE_STRING SbppValQueueStatus  = RTL_CONSTANT_STRING(L"QueueStatus");
static const UNICODE_STRING SbppValAcpiIfStatus = RTL_CONSTANT_STRING(L"AcpiInterfaceStatus");
static const UNICODE_STRING SbppValCtlStatus    = RTL_CONSTANT_STRING(L"ControlStatus");
static const UNICODE_STRING SbppValClient       = RTL_CONSTANT_STRING(L"ClientConnected");
static const UNICODE_STRING SbppValNotify       = RTL_CONSTANT_STRING(L"AcpiNotifyRegistered");

/* Best effort, PASSIVE_LEVEL only: a failure here never matters. */
static VOID
SbppSetStatus(
    _In_ PCUNICODE_STRING Name,
    _In_ ULONG Value)
{
    WDFKEY key;

    if (KeGetCurrentIrql() != PASSIVE_LEVEL ||
        !NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_WRITE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    (VOID)WdfRegistryAssignULong(key, Name, Value);
    WdfRegistryClose(key);
}

static VOID
SbppSetAttachTime(VOID)
{
    WDFKEY key;
    LARGE_INTEGER now;

    if (!NT_SUCCESS(WdfDriverOpenParametersRegistryKey(WdfGetDriver(), KEY_WRITE,
                                                       WDF_NO_OBJECT_ATTRIBUTES, &key))) {
        return;
    }
    KeQuerySystemTimePrecise(&now);
    (VOID)WdfRegistryAssignValue(key, &SbppValAttachTime, REG_QWORD, sizeof(now), &now);
    WdfRegistryClose(key);
}

/* ------------------------------------------------------------------------- */
/* Bridge core                                                               */
/* ------------------------------------------------------------------------- */

/* Hands the pending request to a pended IOCTL_SBPP_WAIT_REQUEST. Lock held. */
static VOID
SbppDeliverLocked(VOID)
{
    WDFREQUEST request;
    SBPP_REQUEST *out;
    NTSTATUS status;

    if (!g_Bridge.PendingValid || g_Bridge.WaitQueue == NULL) {
        return;
    }
    if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(g_Bridge.WaitQueue, &request))) {
        return; /* the app is busy applying the previous one; it collects this next */
    }

    status = WdfRequestRetrieveOutputBuffer(request, sizeof(SBPP_REQUEST), (PVOID *)&out, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(request, status);
        return;
    }

    out->Version  = SBPP_BRIDGE_VERSION;
    out->Sequence = g_Bridge.PendingSeq;
    out->Percent  = g_Bridge.PendingPercent;
    out->Reserved = 0;
    g_Bridge.PendingValid = FALSE;

    WdfRequestCompleteWithInformation(request, STATUS_SUCCESS, sizeof(SBPP_REQUEST));
}

/*
 * A level set through Windows (_BCM). Taken at once, so _BQC right after
 * returns what was set; the app's report confirms or corrects it. With no app
 * connected the level is only stored. Lock held.
 */
static VOID
SbppRequestLevelLocked(
    _In_ ULONG Percent)
{
    g_Bridge.Level = (UCHAR)((Percent > 100) ? 100 : Percent);
    if (!g_Bridge.ClientConnected) {
        return;
    }
    g_Bridge.PendingPercent = g_Bridge.Level;
    g_Bridge.PendingSeq     = ++g_Bridge.RequestSeq;
    g_Bridge.PendingValid   = TRUE;
    SbppDeliverLocked();
}

static VOID
SbppHandleReport(
    _In_ WDFREQUEST Request)
{
    SBPP_REPORT *in;
    NTSTATUS status;
    BOOLEAN accepted = FALSE;
    ULONG percent;
    ULONG applied;

    status = WdfRequestRetrieveInputBuffer(Request, sizeof(SBPP_REPORT), (PVOID *)&in, NULL);
    if (!NT_SUCCESS(status)) {
        WdfRequestComplete(Request, status);
        return;
    }
    if (in->Version != SBPP_BRIDGE_VERSION) {
        WdfRequestComplete(Request, STATUS_REVISION_MISMATCH);
        return;
    }

    percent = (in->Percent > 100) ? 100 : in->Percent;
    applied = in->AppliedSequence;

    WdfWaitLockAcquire(g_Bridge.Lock, NULL);
    /* Only once the app has applied every request: otherwise this level is older
       than the one the last caller set, and that caller may be about to read it. */
    if (applied == g_Bridge.RequestSeq && !g_Bridge.PendingValid) {
        g_Bridge.Level = (UCHAR)percent;
        accepted = TRUE;
    }
    WdfWaitLockRelease(g_Bridge.Lock);

    WdfRequestComplete(Request, STATUS_SUCCESS);
    TraceLoggingWrite(g_SbppTrace, "Report",
                      TraceLoggingUInt32(percent, "Percent"),
                      TraceLoggingUInt32(applied, "AppliedSequence"),
                      TraceLoggingBoolean(accepted, "Accepted"));
}

/* ------------------------------------------------------------------------- */
/* Control device: \Device\SbppBridge                                        */
/* ------------------------------------------------------------------------- */

_Use_decl_annotations_
VOID
SbppEvtFileCreate(
    WDFDEVICE Device,
    WDFREQUEST Request,
    WDFFILEOBJECT FileObject)
{
    UNREFERENCED_PARAMETER(Device);
    UNREFERENCED_PARAMETER(FileObject);

    WdfWaitLockAcquire(g_Bridge.Lock, NULL);
    g_Bridge.ClientConnected = TRUE;
    g_Bridge.RequestSeq      = 0;     /* the app restarts at 0 on every connect too */
    g_Bridge.PendingValid    = FALSE;
    WdfWaitLockRelease(g_Bridge.Lock);

    TraceLoggingWrite(g_SbppTrace, "AppConnected");
    SbppSetStatus(&SbppValClient, 1);
    WdfRequestComplete(Request, STATUS_SUCCESS);
}

_Use_decl_annotations_
VOID
SbppEvtFileCleanup(
    WDFFILEOBJECT FileObject)
{
    WDFDEVICE device = WdfFileObjectGetDevice(FileObject);
    WDFREQUEST request;
    BOOLEAN live;

    /* If the monitor went away while the app was connected, the filter cleanup
       already retired this control device and failed its waits: touch nothing
       of it, and leave the bridge state to whichever device is current. */
    WdfWaitLockAcquire(g_Bridge.Lock, NULL);
    live = (g_Bridge.ControlDevice == device) ? TRUE : FALSE;
    if (live) {
        g_Bridge.ClientConnected = FALSE;
        g_Bridge.PendingValid    = FALSE;
    }
    WdfWaitLockRelease(g_Bridge.Lock);

    if (live) {
        PSBPP_CONTROL_CONTEXT ctx = SbppControlGetContext(device);
        while (NT_SUCCESS(WdfIoQueueRetrieveRequestByFileObject(ctx->WaitQueue, FileObject, &request))) {
            WdfRequestComplete(request, STATUS_CANCELLED);
        }
        SbppSetStatus(&SbppValClient, 0);
    }
    TraceLoggingWrite(g_SbppTrace, "AppDisconnected", TraceLoggingBoolean(live, "CurrentDevice"));
}

_Use_decl_annotations_
VOID
SbppEvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    WDFDEVICE device = WdfIoQueueGetDevice(Queue);
    NTSTATUS status;

    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_SBPP_WAIT_REQUEST:
        if (OutputBufferLength < sizeof(SBPP_REQUEST)) {
            WdfRequestComplete(Request, STATUS_BUFFER_TOO_SMALL);
            return;
        }
        /* A retired control device (its monitor went away): fail at once, so the
           app reconnects to the current one instead of waiting here forever. */
        WdfWaitLockAcquire(g_Bridge.Lock, NULL);
        status = (g_Bridge.ControlDevice == device) ? STATUS_SUCCESS : STATUS_DEVICE_REMOVED;
        WdfWaitLockRelease(g_Bridge.Lock);
        if (NT_SUCCESS(status)) {
            status = WdfRequestForwardToIoQueue(Request, SbppControlGetContext(device)->WaitQueue);
        }
        if (!NT_SUCCESS(status)) {
            WdfRequestComplete(Request, status);
            return;
        }
        WdfWaitLockAcquire(g_Bridge.Lock, NULL);
        SbppDeliverLocked();
        WdfWaitLockRelease(g_Bridge.Lock);
        return;

    case IOCTL_SBPP_REPORT_LEVEL:
        SbppHandleReport(Request);
        return;

    default:
        WdfRequestComplete(Request, STATUS_INVALID_DEVICE_REQUEST);
        return;
    }
}

static NTSTATUS
SbppCreateControlDevice(
    _In_ WDFDRIVER Driver)
{
    /* SYSTEM and Administrators full; the interactive user read/write. */
    static const UNICODE_STRING sddl    = RTL_CONSTANT_STRING(L"D:P(A;;GA;;;SY)(A;;GA;;;BA)(A;;GRGW;;;IU)");
    static const UNICODE_STRING ntName  = RTL_CONSTANT_STRING(SBPP_BRIDGE_NT_NAME);
    static const UNICODE_STRING dosName = RTL_CONSTANT_STRING(SBPP_BRIDGE_DOS_NAME);
    PWDFDEVICE_INIT init;
    WDF_FILEOBJECT_CONFIG fileConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE control;
    WDFQUEUE waitQueue;
    NTSTATUS status;

    init = WdfControlDeviceInitAllocate(Driver, &sddl);
    if (init == NULL) {
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    WdfDeviceInitSetDeviceType(init, SBPP_BRIDGE_DEVICE_TYPE);
    WdfDeviceInitSetExclusive(init, TRUE);          /* one app at a time */
    WdfDeviceInitSetIoType(init, WdfDeviceIoBuffered);

    status = WdfDeviceInitAssignName(init, &ntName);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    WDF_FILEOBJECT_CONFIG_INIT(&fileConfig, SbppEvtFileCreate, WDF_NO_EVENT_CALLBACK, SbppEvtFileCleanup);
    WdfDeviceInitSetFileObjectConfig(init, &fileConfig, WDF_NO_OBJECT_ATTRIBUTES);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SBPP_CONTROL_CONTEXT);
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    status = WdfDeviceCreate(&init, &attributes, &control);
    if (!NT_SUCCESS(status)) {
        WdfDeviceInitFree(init);
        return status;
    }

    status = WdfDeviceCreateSymbolicLink(control, &dosName);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(control);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
    queueConfig.EvtIoDeviceControl = SbppEvtIoDeviceControl;
    status = WdfIoQueueCreate(control, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(control);
        return status;
    }

    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    status = WdfIoQueueCreate(control, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &waitQueue);
    if (!NT_SUCCESS(status)) {
        WdfObjectDelete(control);
        return status;
    }
    SbppControlGetContext(control)->WaitQueue = waitQueue;

    WdfWaitLockAcquire(g_Bridge.Lock, NULL);
    g_Bridge.ControlDevice = control;
    g_Bridge.WaitQueue     = waitQueue;
    WdfWaitLockRelease(g_Bridge.Lock);

    WdfControlFinishInitializing(control);
    return STATUS_SUCCESS;
}

/* A PASSIVE_LEVEL timer cannot be periodic, so this one re-arms itself. */
_Use_decl_annotations_
VOID
SbppEvtControlRetry(
    WDFTIMER Timer)
{
    WDFDEVICE device = (WDFDEVICE)WdfTimerGetParentObject(Timer);
    PSBPP_RETRY_CONTEXT ctx = SbppRetryGetContext(Timer);
    NTSTATUS status;
    BOOLEAN needed;

    WdfWaitLockAcquire(g_Bridge.Lock, NULL);
    needed = (g_Bridge.FilterDevice == device && g_Bridge.ControlDevice == NULL) ? TRUE : FALSE;
    WdfWaitLockRelease(g_Bridge.Lock);
    if (!needed) {
        return;
    }

    ctx->Tries++;
    status = SbppCreateControlDevice(WdfDeviceGetDriver(device));
    TraceLoggingWrite(g_SbppTrace, "ControlDeviceRetry",
                      TraceLoggingUInt32(ctx->Tries, "Try"),
                      TraceLoggingNTStatus(status, "Status"));
    if (NT_SUCCESS(status) || ctx->Tries >= SBPP_CONTROL_RETRY_LIMIT) {
        SbppSetStatus(&SbppValCtlStatus, (ULONG)status);
    } else {
        (VOID)WdfTimerStart(Timer, WDF_REL_TIMEOUT_IN_MS(SBPP_CONTROL_RETRY_MS));
    }
}

static VOID
SbppStartControlRetry(
    _In_ WDFDEVICE Device)
{
    WDF_TIMER_CONFIG config;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFTIMER timer;

    WDF_TIMER_CONFIG_INIT(&config, SbppEvtControlRetry);
    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, SBPP_RETRY_CONTEXT);
    attributes.ParentObject   = Device;               /* stopped and deleted with the filter */
    attributes.ExecutionLevel = WdfExecutionLevelPassive;

    if (NT_SUCCESS(WdfTimerCreate(&config, &attributes, &timer))) {
        SbppRetryGetContext(timer)->Tries = 0;
        (VOID)WdfTimerStart(timer, WDF_REL_TIMEOUT_IN_MS(SBPP_CONTROL_RETRY_MS));
    }
}

/* ------------------------------------------------------------------------- */
/* ACPI brightness methods                                                   */
/* ------------------------------------------------------------------------- */

/* Writes a V1 output buffer of integers, with ACPI.sys's overflow convention. */
static NTSTATUS
SbppWriteIntegers(
    _In_ WDFREQUEST Request,
    _In_ size_t OutLen,
    _In_reads_opt_(Count) const ULONG *Values,
    _In_ ULONG Count,
    _Out_ PULONG Information)
{
    ULONG need = SBPP_OUT_V1_HEADER + Count * SBPP_ARG_V1_LEN(sizeof(ULONG));
    PUCHAR out;
    PUCHAR p;
    size_t outSize;
    ACPI_EVAL_OUTPUT_BUFFER_V1 UNALIGNED *header;
    ULONG i;
    NTSTATUS status;

    *Information = 0;
    if (OutLen < SBPP_OUT_V1_HEADER) {
        return STATUS_BUFFER_TOO_SMALL;
    }
    status = WdfRequestRetrieveOutputBuffer(Request, SBPP_OUT_V1_HEADER, (PVOID *)&out, &outSize);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    header = (ACPI_EVAL_OUTPUT_BUFFER_V1 UNALIGNED *)out;
    header->Signature = SBPP_SIG_OUTPUT;
    header->Length    = need;
    header->Count     = Count;
    if (outSize < need) {
        *Information = SBPP_OUT_V1_HEADER;   /* caller reallocates to Length */
        return STATUS_BUFFER_OVERFLOW;
    }

    p = out + SBPP_OUT_V1_HEADER;
    for (i = 0; i < Count && Values != NULL; i++) {
        PACPI_METHOD_ARGUMENT_V1 arg = (PACPI_METHOD_ARGUMENT_V1)p;
        arg->Type       = ACPI_METHOD_ARGUMENT_INTEGER;
        arg->DataLength = sizeof(ULONG);
        arg->Argument   = Values[i];
        p += SBPP_ARG_V1_LEN(sizeof(ULONG));
    }
    *Information = need;
    return STATUS_SUCCESS;
}

static VOID
SbppForward(
    _In_ WDFREQUEST Request,
    _In_ WDFDEVICE Device)
{
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_SEND_AND_FORGET);
    if (!WdfRequestSend(Request, WdfDeviceGetIoTarget(Device), &options)) {
        WdfRequestComplete(Request, WdfRequestGetStatus(Request));
    }
}

static VOID
SbppFilterIoctl(
    _In_ WDFQUEUE Queue,
    _In_ WDFREQUEST Request,
    _In_ size_t OutLen,
    _In_ size_t InLen,
    _In_ ULONG Code)
{
    PUCHAR in = NULL;
    size_t inSize = 0;
    ULONG sig = 0;
    ULONG name = 0;
    ULONG intArg = 0;
    BOOLEAN haveInt = FALSE;
    BOOLEAN ours = FALSE;
    NTSTATUS status;
    ULONG info = 0;
    ULONG level;

    if (g_Bridge.Lock != NULL &&
        (Code == SBPP_IOCTL_AEVAL || Code == SBPP_IOCTL_EVAL) &&
        InLen >= sizeof(ACPI_EVAL_INPUT_BUFFER_V1) &&
        NT_SUCCESS(WdfRequestRetrieveInputBuffer(Request, sizeof(ACPI_EVAL_INPUT_BUFFER_V1),
                                                 (PVOID *)&in, &inSize))) {
        /* METHOD_BUFFERED: input and output share one buffer. Read it all first. */
        RtlCopyMemory(&sig, in, sizeof(ULONG));
        RtlCopyMemory(&name, in + sizeof(ULONG), sizeof(ULONG));

        /* _BCM's level comes as a simple integer ('IieA'); accept the complex
           form ('CieA') with an integer first argument too. */
        if (sig == SBPP_SIG_INTEGER &&
            inSize >= sizeof(ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_V1)) {
            RtlCopyMemory(&intArg, in + FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_SIMPLE_INTEGER_V1, IntegerArgument),
                          sizeof(ULONG));
            haveInt = TRUE;
        } else if (sig == SBPP_SIG_COMPLEX && inSize >= SBPP_COMPLEX_V1_MIN) {
            PUCHAR a = in + FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX_V1, Argument);
            ULONG argc;
            USHORT type;
            USHORT dataLength;

            RtlCopyMemory(&argc, in + FIELD_OFFSET(ACPI_EVAL_INPUT_BUFFER_COMPLEX_V1, ArgumentCount), sizeof(ULONG));
            RtlCopyMemory(&type, a + FIELD_OFFSET(ACPI_METHOD_ARGUMENT_V1, Type), sizeof(USHORT));
            RtlCopyMemory(&dataLength, a + FIELD_OFFSET(ACPI_METHOD_ARGUMENT_V1, DataLength), sizeof(USHORT));
            if (argc >= 1 && type == ACPI_METHOD_ARGUMENT_INTEGER && dataLength >= sizeof(ULONG)) {
                RtlCopyMemory(&intArg, a + FIELD_OFFSET(ACPI_METHOD_ARGUMENT_V1, Data), sizeof(ULONG));
                haveInt = TRUE;
            }
        }

        if (name == SBPP_ACPI_BCL || name == SBPP_ACPI_BQC) {
            ours = (sig == SBPP_SIG_PLAIN) ? TRUE : FALSE;
        } else if (name == SBPP_ACPI_BCM) {
            ours = haveInt;
        }
    }

    if (!ours) {
        if (name != 0) {
            TraceLoggingWrite(g_SbppTrace, "AcpiMethodPassedDown",
                              TraceLoggingLevel(5 /* WINEVENT_LEVEL_VERBOSE */),
                              TraceLoggingCountedString((PCSTR)&name, 4, "Method"),
                              TraceLoggingHexUInt32(sig, "Signature"));
        }
        SbppForward(Request, WdfIoQueueGetDevice(Queue));
        return;
    }

    WdfWaitLockAcquire(g_Bridge.Lock, NULL);
    if (name == SBPP_ACPI_BCM) {
        SbppRequestLevelLocked(intArg);
    }
    level = g_Bridge.Level;
    WdfWaitLockRelease(g_Bridge.Lock);

    if (name == SBPP_ACPI_BCL) {
        status = SbppWriteIntegers(Request, OutLen, g_BclValues, SBPP_BCL_COUNT, &info);
    } else if (name == SBPP_ACPI_BQC) {
        status = SbppWriteIntegers(Request, OutLen, &level, 1, &info);
    } else {
        /* _BCM returns nothing: an empty output buffer if one was supplied. */
        status = (OutLen >= SBPP_OUT_V1_HEADER) ? SbppWriteIntegers(Request, OutLen, NULL, 0, &info)
                                                : STATUS_SUCCESS;
    }

    WdfRequestCompleteWithInformation(Request, status, info);
    TraceLoggingWrite(g_SbppTrace, "AcpiMethod",
                      TraceLoggingCountedString((PCSTR)&name, 4, "Method"),
                      TraceLoggingUInt32((name == SBPP_ACPI_BCM) ? intArg : level, "Level"),
                      TraceLoggingNTStatus(status, "Status"));
}

_Use_decl_annotations_
VOID
SbppEvtFilterIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    SbppFilterIoctl(Queue, Request, OutputBufferLength, InputBufferLength, IoControlCode);
}

_Use_decl_annotations_
VOID
SbppEvtFilterIoInternalDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    SbppFilterIoctl(Queue, Request, OutputBufferLength, InputBufferLength, IoControlCode);
}

/* ------------------------------------------------------------------------- */
/* ACPI_INTERFACE_STANDARD2, for monitor.sys's notification registration     */
/* ------------------------------------------------------------------------- */

/* No general-purpose events behind this display: every GPE call is declined. */
static NTSTATUS
SbppAcpiGpeConnect(
    _In_ PVOID Context,
    _In_ ULONG GpeNumber,
    _In_ KINTERRUPT_MODE Mode,
    _In_ BOOLEAN Shareable,
    _In_ PGPE_SERVICE_ROUTINE ServiceRoutine,
    _In_opt_ PVOID ServiceContext,
    _Out_ PVOID *ObjectContext)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(GpeNumber);
    UNREFERENCED_PARAMETER(Mode);
    UNREFERENCED_PARAMETER(Shareable);
    UNREFERENCED_PARAMETER(ServiceRoutine);
    UNREFERENCED_PARAMETER(ServiceContext);
    *ObjectContext = NULL;
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
SbppAcpiGpeDecline(
    _In_ PVOID Context,
    _In_ PVOID ObjectContext)
{
    UNREFERENCED_PARAMETER(Context);
    UNREFERENCED_PARAMETER(ObjectContext);
    return STATUS_NOT_SUPPORTED;
}

static NTSTATUS
SbppAcpiRegisterNotify(
    _In_ PVOID Context,
    _In_ PDEVICE_NOTIFY_CALLBACK2 NotificationHandler,
    _In_opt_ PVOID NotificationContext)
{
    UNREFERENCED_PARAMETER(Context);
    if (NotificationHandler == NULL) {
        return STATUS_INVALID_PARAMETER;
    }
    (VOID)InterlockedExchangePointer((PVOID volatile *)&g_NotifyContext, NotificationContext);
    (VOID)InterlockedExchangePointer((PVOID volatile *)&g_NotifyHandler, (PVOID)NotificationHandler);
    TraceLoggingWrite(g_SbppTrace, "AcpiNotifyRegistered");
    SbppSetStatus(&SbppValNotify, 1);
    return STATUS_SUCCESS;
}

static VOID
SbppAcpiUnregisterNotify(
    _In_ PVOID Context)
{
    UNREFERENCED_PARAMETER(Context);
    (VOID)InterlockedExchangePointer((PVOID volatile *)&g_NotifyHandler, NULL);
    (VOID)InterlockedExchangePointer((PVOID volatile *)&g_NotifyContext, NULL);
    TraceLoggingWrite(g_SbppTrace, "AcpiNotifyUnregistered");
    SbppSetStatus(&SbppValNotify, 0);
}

static NTSTATUS
SbppAddAcpiInterface(
    _In_ WDFDEVICE Device)
{
    ACPI_INTERFACE_STANDARD2 acpi;
    WDF_QUERY_INTERFACE_CONFIG config;

    RtlZeroMemory(&acpi, sizeof(acpi));
    acpi.Size                             = sizeof(acpi);
    acpi.Version                          = 1;
    acpi.Context                          = Device;
    acpi.InterfaceReference               = WdfDeviceInterfaceReferenceNoOp;
    acpi.InterfaceDereference             = WdfDeviceInterfaceDereferenceNoOp;
    acpi.GpeConnectVector                 = SbppAcpiGpeConnect;
    acpi.GpeDisconnectVector              = SbppAcpiGpeDecline;
    acpi.GpeEnableEvent                   = SbppAcpiGpeDecline;
    acpi.GpeDisableEvent                  = SbppAcpiGpeDecline;
    acpi.GpeClearStatus                   = SbppAcpiGpeDecline;
    acpi.RegisterForDeviceNotifications   = SbppAcpiRegisterNotify;
    acpi.UnregisterForDeviceNotifications = SbppAcpiUnregisterNotify;

    /* KMDF copies the interface and answers the query itself, so it never
       reaches the display adapter's PDO (which knows nothing of ACPI). */
    WDF_QUERY_INTERFACE_CONFIG_INIT(&config, (PINTERFACE)&acpi, &SBPP_GUID_ACPI_INTERFACE_STANDARD2, NULL);
    return WdfDeviceAddQueryInterface(Device, &config);
}

/* ------------------------------------------------------------------------- */
/* Driver and filter device                                                  */
/* ------------------------------------------------------------------------- */

_Use_decl_annotations_
NTSTATUS
DriverEntry(
    PDRIVER_OBJECT DriverObject,
    PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;
    ULONG i;
    NTSTATUS status;

    (VOID)TraceLoggingRegister(g_SbppTrace);   /* tracing is optional */

    WDF_DRIVER_CONFIG_INIT(&config, SbppEvtDeviceAdd);
    config.DriverPoolTag   = SBPP_TAG;
    config.EvtDriverUnload = SbppEvtDriverUnload;

    status = WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES,
                             &config, WDF_NO_HANDLE);
    if (!NT_SUCCESS(status)) {
        TraceLoggingUnregister(g_SbppTrace);
        return status;
    }

    g_BclValues[0] = SBPP_BCL_DEFAULT_AC;
    g_BclValues[1] = SBPP_BCL_DEFAULT_DC;
    for (i = 0; i < SBPP_LEVEL_COUNT; i++) {
        g_BclValues[2 + i] = i;
    }

    g_Bridge.Level = SBPP_INITIAL_BRIGHTNESS;
    if (!NT_SUCCESS(WdfWaitLockCreate(WDF_NO_OBJECT_ATTRIBUTES, &g_Bridge.Lock))) {
        g_Bridge.Lock = NULL;   /* bridge disabled; the filter still loads as pass-through */
    }
    return STATUS_SUCCESS;
}

_Use_decl_annotations_
VOID
SbppEvtDriverUnload(
    WDFDRIVER Driver)
{
    UNREFERENCED_PARAMETER(Driver);
    TraceLoggingUnregister(g_SbppTrace);
}

_Use_decl_annotations_
VOID
SbppEvtFilterCleanup(
    WDFOBJECT Object)
{
    WDFDEVICE control = NULL;
    WDFREQUEST request;

    if (g_Bridge.Lock == NULL) {
        return;
    }

    WdfWaitLockAcquire(g_Bridge.Lock, NULL);
    if (g_Bridge.FilterDevice == (WDFDEVICE)Object) {
        /* Deleting a control device does not cancel what is queued on it. Fail the
           app's pended wait so it sees the disconnect and closes its handle -
           otherwise the old device, and its name, live on and the next attach
           cannot create its own. */
        if (g_Bridge.WaitQueue != NULL) {
            while (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(g_Bridge.WaitQueue, &request))) {
                WdfRequestComplete(request, STATUS_DEVICE_REMOVED);
            }
        }
        g_Bridge.FilterDevice    = NULL;
        control                  = g_Bridge.ControlDevice;
        g_Bridge.ControlDevice   = NULL;
        g_Bridge.WaitQueue       = NULL;
        g_Bridge.PendingValid    = FALSE;
        g_Bridge.ClientConnected = FALSE;
    }
    WdfWaitLockRelease(g_Bridge.Lock);

    /* The control device holds the driver loaded; it goes with the filter. */
    if (control != NULL) {
        WdfObjectDelete(control);
        SbppSetStatus(&SbppValClient, 0);
    }
}

_Use_decl_annotations_
NTSTATUS
SbppEvtDeviceAdd(
    WDFDRIVER Driver,
    PWDFDEVICE_INIT DeviceInit)
{
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    NTSTATUS status;
    NTSTATUS queueStatus = STATUS_NOT_SUPPORTED;
    NTSTATUS acpiStatus  = STATUS_NOT_SUPPORTED;
    NTSTATUS ctlStatus   = STATUS_NOT_SUPPORTED;
    BOOLEAN first = FALSE;

    PAGED_CODE();

    WdfFdoInitSetFilter(DeviceInit);

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.EvtCleanupCallback = SbppEvtFilterCleanup;
    attributes.ExecutionLevel     = WdfExecutionLevelPassive;

    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    /* One bridge: a second matching monitor stays a plain pass-through. */
    if (g_Bridge.Lock != NULL) {
        WdfWaitLockAcquire(g_Bridge.Lock, NULL);
        if (g_Bridge.FilterDevice == NULL) {
            g_Bridge.FilterDevice = device;
            first = TRUE;
        }
        WdfWaitLockRelease(g_Bridge.Lock);
    }

    if (first) {
        /* No queue means KMDF forwards every request untouched: fail open. */
        WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
        queueConfig.EvtIoDeviceControl         = SbppEvtFilterIoDeviceControl;
        queueConfig.EvtIoInternalDeviceControl = SbppEvtFilterIoInternalDeviceControl;
        queueStatus = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, WDF_NO_HANDLE);

        acpiStatus = SbppAddAcpiInterface(device);

        ctlStatus = SbppCreateControlDevice(Driver);
        if (!NT_SUCCESS(ctlStatus)) {
            /* Most likely the previous instance's control device, still held open
               by the app for a moment: retry until its name is free. */
            SbppStartControlRetry(device);
        }

        SbppSetAttachTime();
        SbppSetStatus(&SbppValQueueStatus, (ULONG)queueStatus);
        SbppSetStatus(&SbppValAcpiIfStatus, (ULONG)acpiStatus);
        SbppSetStatus(&SbppValCtlStatus, (ULONG)ctlStatus);
        SbppSetStatus(&SbppValClient, 0);
    }

    TraceLoggingWrite(g_SbppTrace, "Attached",
                      TraceLoggingBoolean(first, "Bridged"),
                      TraceLoggingNTStatus(queueStatus, "QueueStatus"),
                      TraceLoggingNTStatus(acpiStatus, "AcpiInterfaceStatus"),
                      TraceLoggingNTStatus(ctlStatus, "ControlStatus"));
    return STATUS_SUCCESS;
}
