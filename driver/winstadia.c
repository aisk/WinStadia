// UMDF2 HID minidriver exposing a virtual DualShock 4.
//
// Input comes from the bridge thread (bridge.c), which reads the physical
// Stadia controller; rumble written by games goes back the same way.

#include "winstadia.h"
#include <hidport.h>

#define DS4_VID 0x054C
#define DS4_PID 0x09CC
#define DS4_VERSION 0x0100

#define REPORT_ID_INPUT 0x01
#define REPORT_ID_OUTPUT 0x05
#define REPORT_ID_CALIBRATION 0x02
#define REPORT_ID_SERIAL 0x12
#define REPORT_ID_FIRMWARE 0xA3
// Not part of a real DS4: bridge state for diagnostics, see GetFeature.
#define REPORT_ID_STATUS 0xE1

#define STATUS_REPORT_LEN 64

// DS4 output report: [1] flags, [4] weak motor, [5] strong motor.
#define OUTPUT_FLAG_RUMBLE 0x01

static const UCHAR ReportDescriptor[] = {
    0x05, 0x01,       // Usage Page (Generic Desktop)
    0x09, 0x05,       // Usage (Game Pad)
    0xA1, 0x01,       // Collection (Application)
    0x85, 0x01,       //   Report ID (1)
    0x09, 0x30,       //   Usage (X)
    0x09, 0x31,       //   Usage (Y)
    0x09, 0x32,       //   Usage (Z)
    0x09, 0x35,       //   Usage (Rz)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x04,       //   Report Count (4)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x09, 0x39,       //   Usage (Hat switch)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x07,       //   Logical Maximum (7)
    0x35, 0x00,       //   Physical Minimum (0)
    0x46, 0x3B, 0x01, //   Physical Maximum (315)
    0x65, 0x14,       //   Unit (Degrees)
    0x75, 0x04,       //   Report Size (4)
    0x95, 0x01,       //   Report Count (1)
    0x81, 0x42,       //   Input (Data,Var,Abs,Null)
    0x65, 0x00,       //   Unit (None)
    0x05, 0x09,       //   Usage Page (Button)
    0x19, 0x01,       //   Usage Minimum (1)
    0x29, 0x0E,       //   Usage Maximum (14)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x01,       //   Logical Maximum (1)
    0x75, 0x01,       //   Report Size (1)
    0x95, 0x0E,       //   Report Count (14)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor 0xFF00)
    0x09, 0x20,       //   Usage (0x20), report counter
    0x75, 0x06,       //   Report Size (6)
    0x95, 0x01,       //   Report Count (1)
    0x15, 0x00,       //   Logical Minimum (0)
    0x25, 0x7F,       //   Logical Maximum (127)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x05, 0x01,       //   Usage Page (Generic Desktop)
    0x09, 0x33,       //   Usage (Rx)
    0x09, 0x34,       //   Usage (Ry)
    0x15, 0x00,       //   Logical Minimum (0)
    0x26, 0xFF, 0x00, //   Logical Maximum (255)
    0x75, 0x08,       //   Report Size (8)
    0x95, 0x02,       //   Report Count (2)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x06, 0x00, 0xFF, //   Usage Page (Vendor 0xFF00)
    0x09, 0x21,       //   Usage (0x21), sensors and touchpad
    0x95, 0x36,       //   Report Count (54)
    0x81, 0x02,       //   Input (Data,Var,Abs)
    0x85, 0x05,       //   Report ID (5)
    0x09, 0x22,       //   Usage (0x22)
    0x95, 0x1F,       //   Report Count (31)
    0x91, 0x02,       //   Output (Data,Var,Abs)
    0x85, 0x02,       //   Report ID (2)
    0x09, 0x24,       //   Usage (0x24)
    0x95, 0x24,       //   Report Count (36)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)
    0x85, 0x12,       //   Report ID (18)
    0x09, 0x25,       //   Usage (0x25)
    0x95, 0x0F,       //   Report Count (15)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)
    0x85, 0xA3,       //   Report ID (163)
    0x09, 0x26,       //   Usage (0x26)
    0x95, 0x30,       //   Report Count (48)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)
    0x85, 0xE1,       //   Report ID (225)
    0x09, 0x28,       //   Usage (0x28)
    0x95, 0x3F,       //   Report Count (63)
    0xB1, 0x02,       //   Feature (Data,Var,Abs)
    0xC0,             // End Collection
};

static const HID_DESCRIPTOR HidDescriptor = {
    0x09,   // bLength
    0x21,   // bDescriptorType (HID)
    0x0100, // bcdHID
    0x00,   // bCountry
    0x01,   // bNumDescriptors
    {{0x22, sizeof(ReportDescriptor)}},
};

static const HID_DEVICE_ATTRIBUTES DeviceAttributes = {
    sizeof(HID_DEVICE_ATTRIBUTES),
    DS4_VID,
    DS4_PID,
    DS4_VERSION,
};

// Sticks centered, hat released.
const UCHAR IdleControls[PAD_CONTROLS_LEN] = {0x80, 0x80, 0x80, 0x80, 0x08};

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;
EVT_WDF_DEVICE_SELF_MANAGED_IO_INIT EvtSelfManagedIoInit;
EVT_WDF_DEVICE_SELF_MANAGED_IO_CLEANUP EvtSelfManagedIoCleanup;

static NTSTATUS
CopyToRequest(WDFREQUEST Request, const VOID *Source, size_t Length)
{
    WDFMEMORY memory;
    size_t capacity;
    NTSTATUS status = WdfRequestRetrieveOutputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WdfMemoryGetBuffer(memory, &capacity);
    if (capacity < Length) {
        return STATUS_INVALID_BUFFER_SIZE;
    }
    status = WdfMemoryCopyFromBuffer(memory, 0, (PVOID)Source, Length);
    if (NT_SUCCESS(status)) {
        WdfRequestSetInformation(Request, Length);
    }
    return status;
}

static NTSTATUS
GetInputBuffer(WDFREQUEST Request, size_t MinLength, PUCHAR *Buffer, size_t *Length)
{
    WDFMEMORY memory;
    NTSTATUS status = WdfRequestRetrieveInputMemory(Request, &memory);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    *Buffer = WdfMemoryGetBuffer(memory, Length);
    return *Length < MinLength ? STATUS_INVALID_BUFFER_SIZE : STATUS_SUCCESS;
}

// Rebuilds the input report around Context->Controls. Caller holds the lock.
static VOID
BuildInputReport(PDEVICE_CONTEXT Context)
{
    PUCHAR report = Context->InputReport;
    // The real pad counts in 5.33 ms units at a 4 ms report interval.
    USHORT timestamp = (USHORT)(Context->ReportCounter * 188);

    RtlZeroMemory(report, INPUT_REPORT_LEN);
    report[0] = REPORT_ID_INPUT;
    RtlCopyMemory(report + 1, Context->Controls, PAD_CONTROLS_LEN);
    report[7] |= (UCHAR)(Context->ReportCounter << 2);
    report[10] = (UCHAR)timestamp;
    report[11] = (UCHAR)(timestamp >> 8);
    report[30] = 0x1B; // USB powered, battery full
    report[35] = 0x80; // no touch
    report[39] = 0x80;
    Context->ReportCounter++;
}

VOID
PadPublishControls(PDEVICE_CONTEXT Context, const UCHAR *Controls)
{
    UCHAR report[INPUT_REPORT_LEN];
    WDFREQUEST read = NULL;

    AcquireSRWLockExclusive(&Context->Lock);
    if (RtlEqualMemory(Context->Controls, Controls, PAD_CONTROLS_LEN)) {
        ReleaseSRWLockExclusive(&Context->Lock);
        return;
    }
    RtlCopyMemory(Context->Controls, Controls, PAD_CONTROLS_LEN);
    BuildInputReport(Context);
    // Taking the pending read under the lock keeps ReadReport from parking a
    // request right after this update, which would deliver it late.
    if (NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->ReadQueue, &read))) {
        RtlCopyMemory(report, Context->InputReport, INPUT_REPORT_LEN);
    } else {
        Context->InputChanged = TRUE;
    }
    ReleaseSRWLockExclusive(&Context->Lock);

    if (read != NULL) {
        WdfRequestComplete(read, CopyToRequest(read, report, INPUT_REPORT_LEN));
    }
}

static NTSTATUS
ReadReport(PDEVICE_CONTEXT Context, WDFREQUEST Request, BOOLEAN *Complete)
{
    UCHAR report[INPUT_REPORT_LEN];
    NTSTATUS status;

    AcquireSRWLockExclusive(&Context->Lock);
    if (Context->InputChanged) {
        Context->InputChanged = FALSE;
        RtlCopyMemory(report, Context->InputReport, INPUT_REPORT_LEN);
        ReleaseSRWLockExclusive(&Context->Lock);
        return CopyToRequest(Request, report, INPUT_REPORT_LEN);
    }
    status = WdfRequestForwardToIoQueue(Request, Context->ReadQueue);
    ReleaseSRWLockExclusive(&Context->Lock);

    *Complete = !NT_SUCCESS(status);
    return status;
}

// The report buffer of write-type requests starts with the report ID.
static NTSTATUS
WriteReport(PDEVICE_CONTEXT Context, WDFREQUEST Request)
{
    PUCHAR report;
    size_t length;
    NTSTATUS status = GetInputBuffer(Request, 1, &report, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (report[0] != REPORT_ID_OUTPUT) {
        return STATUS_INVALID_PARAMETER;
    }

    if (length > 5 && (report[1] & OUTPUT_FLAG_RUMBLE)) {
        AcquireSRWLockExclusive(&Context->Lock);
        Context->RumbleWeak = report[4];
        Context->RumbleStrong = report[5];
        ReleaseSRWLockExclusive(&Context->Lock);
        SetEvent(Context->RumbleEvent);
    }
    WdfRequestSetInformation(Request, length);
    return STATUS_SUCCESS;
}

// The input buffer of GET_FEATURE holds the report ID, the output buffer
// receives the whole report including the ID.
static NTSTATUS
GetFeature(PDEVICE_CONTEXT Context, WDFREQUEST Request)
{
    PUCHAR id;
    size_t length;
    NTSTATUS status = GetInputBuffer(Request, 1, &id, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    UCHAR report[STATUS_REPORT_LEN] = {*id};
    switch (*id) {
    case REPORT_ID_CALIBRATION:
        length = 37;
        break;
    case REPORT_ID_SERIAL:
        length = 16;
        break;
    case REPORT_ID_FIRMWARE:
        length = 49;
        break;
    case REPORT_ID_STATUS:
        // [1] bridge state, [2] rumble failed, [3..7] last Win32 error (LE)
        length = STATUS_REPORT_LEN;
        AcquireSRWLockShared(&Context->Lock);
        report[1] = Context->BridgeState;
        report[2] = Context->RumbleFailed;
        RtlCopyMemory(report + 3, &Context->BridgeError, sizeof(ULONG));
        ReleaseSRWLockShared(&Context->Lock);
        break;
    default:
        return STATUS_INVALID_PARAMETER;
    }
    return CopyToRequest(Request, report, length);
}

static NTSTATUS
GetInputReport(PDEVICE_CONTEXT Context, WDFREQUEST Request)
{
    UCHAR report[INPUT_REPORT_LEN];

    AcquireSRWLockShared(&Context->Lock);
    RtlCopyMemory(report, Context->InputReport, INPUT_REPORT_LEN);
    ReleaseSRWLockShared(&Context->Lock);
    return CopyToRequest(Request, report, INPUT_REPORT_LEN);
}

static NTSTATUS
GetString(WDFREQUEST Request)
{
    PUCHAR input;
    size_t length;
    NTSTATUS status = GetInputBuffer(Request, sizeof(ULONG), &input, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // The low word is the string ID, the high word the language ID.
    switch (*(PULONG)input & 0xFFFF) {
    case HID_STRING_ID_IMANUFACTURER: {
        static const WCHAR manufacturer[] = L"Sony Interactive Entertainment";
        return CopyToRequest(Request, manufacturer, sizeof(manufacturer));
    }
    case HID_STRING_ID_IPRODUCT: {
        static const WCHAR product[] = L"Wireless Controller";
        return CopyToRequest(Request, product, sizeof(product));
    }
    case HID_STRING_ID_ISERIALNUMBER: {
        static const WCHAR serial[] = L"winstadia";
        return CopyToRequest(Request, serial, sizeof(serial));
    }
    default:
        return STATUS_INVALID_PARAMETER;
    }
}

VOID
EvtIoDeviceControl(
    WDFQUEUE Queue,
    WDFREQUEST Request,
    size_t OutputBufferLength,
    size_t InputBufferLength,
    ULONG IoControlCode)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfIoQueueGetDevice(Queue));
    BOOLEAN complete = TRUE;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(OutputBufferLength);
    UNREFERENCED_PARAMETER(InputBufferLength);

    switch (IoControlCode) {
    case IOCTL_HID_GET_DEVICE_DESCRIPTOR:
        status = CopyToRequest(Request, &HidDescriptor, HidDescriptor.bLength);
        break;
    case IOCTL_HID_GET_DEVICE_ATTRIBUTES:
        status = CopyToRequest(Request, &DeviceAttributes, sizeof(DeviceAttributes));
        break;
    case IOCTL_HID_GET_REPORT_DESCRIPTOR:
        status = CopyToRequest(Request, ReportDescriptor, sizeof(ReportDescriptor));
        break;
    case IOCTL_HID_READ_REPORT:
        status = ReadReport(context, Request, &complete);
        break;
    case IOCTL_HID_WRITE_REPORT:
    case IOCTL_UMDF_HID_SET_OUTPUT_REPORT:
        status = WriteReport(context, Request);
        break;
    case IOCTL_UMDF_HID_GET_FEATURE:
        status = GetFeature(context, Request);
        break;
    case IOCTL_UMDF_HID_GET_INPUT_REPORT:
        status = GetInputReport(context, Request);
        break;
    case IOCTL_HID_GET_STRING:
        status = GetString(Request);
        break;
    default:
        status = STATUS_NOT_IMPLEMENTED;
        break;
    }

    if (complete) {
        WdfRequestComplete(Request, status);
    }
}

NTSTATUS
EvtSelfManagedIoInit(WDFDEVICE Device)
{
    return BridgeStart(GetDeviceContext(Device));
}

VOID
EvtSelfManagedIoCleanup(WDFDEVICE Device)
{
    BridgeStop(GetDeviceContext(Device));
}

NTSTATUS
EvtDeviceAdd(WDFDRIVER Driver, PWDFDEVICE_INIT DeviceInit)
{
    WDF_PNPPOWER_EVENT_CALLBACKS pnpCallbacks;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_IO_QUEUE_CONFIG queueConfig;
    WDFDEVICE device;
    WDFQUEUE queue;
    PDEVICE_CONTEXT context;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    // mshidumdf.sys is the function driver, this driver sits below it.
    WdfFdoInitSetFilter(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDeviceSelfManagedIoInit = EvtSelfManagedIoInit;
    pnpCallbacks.EvtDeviceSelfManagedIoCleanup = EvtSelfManagedIoCleanup;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    context = GetDeviceContext(device);
    InitializeSRWLock(&context->Lock);
    RtlCopyMemory(context->Controls, IdleControls, PAD_CONTROLS_LEN);
    BuildInputReport(context);

    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchSequential);
    queueConfig.EvtIoDeviceControl = EvtIoDeviceControl;
    status = WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &queue);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Holds IOCTL_HID_READ_REPORT requests until the input state changes.
    WDF_IO_QUEUE_CONFIG_INIT(&queueConfig, WdfIoQueueDispatchManual);
    return WdfIoQueueCreate(device, &queueConfig, WDF_NO_OBJECT_ATTRIBUTES, &context->ReadQueue);
}

NTSTATUS
DriverEntry(PDRIVER_OBJECT DriverObject, PUNICODE_STRING RegistryPath)
{
    WDF_DRIVER_CONFIG config;

    WDF_DRIVER_CONFIG_INIT(&config, EvtDeviceAdd);
    return WdfDriverCreate(DriverObject, RegistryPath, WDF_NO_OBJECT_ATTRIBUTES, &config, WDF_NO_HANDLE);
}
