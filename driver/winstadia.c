// UMDF2 HID transport minidriver that presents a Stadia controller as an Xbox
// Wireless Controller, in the HID format those use over Bluetooth.
//
// The inbox xinputhid filter sits above this stack and turns the HID reports
// into XInput, as it does for the real thing. Input arrives from the
// controller through stadia.c and the transport below it, USB or Bluetooth;
// rumble written by games goes back the same way.

#include "winstadia.h"
#include <hidport.h>

#define XBOX_VID 0x045E
#define XBOX_PID 0x02FD
#define XBOX_VERSION 0x0903

#define REPORT_ID_GAMEPAD 0x01
#define REPORT_ID_GUIDE 0x02
#define REPORT_ID_RUMBLE 0x03
#define REPORT_ID_BATTERY 0x04
// Not part of a real Xbox controller: driver state for diagnostics.
#define REPORT_ID_STATUS 0xE1

#define GAMEPAD_REPORT_LEN (1 + PAD_GAMEPAD_LEN)
#define GUIDE_REPORT_LEN 2
#define BATTERY_REPORT_LEN 2
#define MAX_INPUT_REPORT_LEN GAMEPAD_REPORT_LEN
#define STATUS_REPORT_LEN 64

// Rumble output report: [1] motor enable mask, [2] left trigger, [3] right
// trigger, [4] strong, [5] weak, each 0..100, [6..9] timing, which the Stadia
// has no use for.
#define RUMBLE_REPORT_MIN_LEN 6
#define RUMBLE_ENABLE_WEAK 0x01
#define RUMBLE_ENABLE_STRONG 0x02
#define RUMBLE_MAGNITUDE_MAX 100

// Taken from an Xbox One S controller connected over Bluetooth.
static const UCHAR ReportDescriptor[] = {
    0x05, 0x01,        // Usage Page (Generic Desktop)
    0x09, 0x05,        // Usage (Game Pad)
    0xA1, 0x01,        // Collection (Application)
    0x85, 0x01,        //   Report ID (1)
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x30,        //     Usage (X)
    0x09, 0x31,        //     Usage (Y)
    0x15, 0x00,        //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65534)
    0x95, 0x02,        //     Report Count (2)
    0x75, 0x10,        //     Report Size (16)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0x09, 0x01,        //   Usage (Pointer)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x33,        //     Usage (Rx)
    0x09, 0x34,        //     Usage (Ry)
    0x15, 0x00,        //     Logical Minimum (0)
    0x27, 0xFF, 0xFF, 0x00, 0x00,  //     Logical Maximum (65534)
    0x95, 0x02,        //     Report Count (2)
    0x75, 0x10,        //     Report Size (16)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0xC0,              //   End Collection
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x32,        //   Usage (Z)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x0A,        //   Report Size (10)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x35,        //   Usage (Rz)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x03,  //   Logical Maximum (1023)
    0x95, 0x01,        //   Report Count (1)
    0x75, 0x0A,        //   Report Size (10)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x39,        //   Usage (Hat switch)
    0x15, 0x01,        //   Logical Minimum (1)
    0x25, 0x08,        //   Logical Maximum (8)
    0x35, 0x00,        //   Physical Minimum (0)
    0x46, 0x3B, 0x01,  //   Physical Maximum (315)
    0x66, 0x14, 0x00,  //   Unit (System: English Rotation, Length: Centimeter)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x42,        //   Input (Data,Var,Abs,Null)
    0x75, 0x04,        //   Report Size (4)
    0x95, 0x01,        //   Report Count (1)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x35, 0x00,        //   Physical Minimum (0)
    0x45, 0x00,        //   Physical Maximum (0)
    0x65, 0x00,        //   Unit (None)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x05, 0x09,        //   Usage Page (Button)
    0x19, 0x01,        //   Usage Minimum (0x01)
    0x29, 0x0A,        //   Usage Maximum (0x0A)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x01,        //   Logical Maximum (1)
    0x75, 0x01,        //   Report Size (1)
    0x95, 0x0A,        //   Report Count (10)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    0x15, 0x00,        //   Logical Minimum (0)
    0x25, 0x00,        //   Logical Maximum (0)
    0x75, 0x06,        //   Report Size (6)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x03,        //   Input (Const,Var,Abs)
    0x05, 0x01,        //   Usage Page (Generic Desktop)
    0x09, 0x80,        //   Usage (Sys Control)
    0x85, 0x02,        //   Report ID (2)
    0xA1, 0x00,        //   Collection (Physical)
    0x09, 0x85,        //     Usage (Sys Main Menu)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x95, 0x01,        //     Report Count (1)
    0x75, 0x01,        //     Report Size (1)
    0x81, 0x02,        //     Input (Data,Var,Abs)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x00,        //     Logical Maximum (0)
    0x75, 0x07,        //     Report Size (7)
    0x95, 0x01,        //     Report Count (1)
    0x81, 0x03,        //     Input (Const,Var,Abs)
    0xC0,              //   End Collection
    0x05, 0x0F,        //   Usage Page (PID Page)
    0x09, 0x21,        //   Usage (0x21)
    0x85, 0x03,        //   Report ID (3)
    0xA1, 0x02,        //   Collection (Logical)
    0x09, 0x97,        //     Usage (0x97)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x01,        //     Logical Maximum (1)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x91, 0x02,        //     Output (Data,Var,Abs)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x00,        //     Logical Maximum (0)
    0x75, 0x04,        //     Report Size (4)
    0x95, 0x01,        //     Report Count (1)
    0x91, 0x03,        //     Output (Const,Var,Abs)
    0x09, 0x70,        //     Usage (0x70)
    0x15, 0x00,        //     Logical Minimum (0)
    0x25, 0x64,        //     Logical Maximum (100)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x04,        //     Report Count (4)
    0x91, 0x02,        //     Output (Data,Var,Abs)
    0x09, 0x50,        //     Usage (0x50)
    0x66, 0x01, 0x10,  //     Unit (System: SI Linear, Time: Seconds)
    0x55, 0x0E,        //     Unit Exponent (-2)
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x00,  //     Logical Maximum (255)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x91, 0x02,        //     Output (Data,Var,Abs)
    0x09, 0xA7,        //     Usage (0xA7)
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x00,  //     Logical Maximum (255)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x91, 0x02,        //     Output (Data,Var,Abs)
    0x65, 0x00,        //     Unit (None)
    0x55, 0x00,        //     Unit Exponent (0)
    0x09, 0x7C,        //     Usage (0x7C)
    0x15, 0x00,        //     Logical Minimum (0)
    0x26, 0xFF, 0x00,  //     Logical Maximum (255)
    0x75, 0x08,        //     Report Size (8)
    0x95, 0x01,        //     Report Count (1)
    0x91, 0x02,        //     Output (Data,Var,Abs)
    0xC0,              //   End Collection
    0x85, 0x04,        //   Report ID (4)
    0x05, 0x06,        //   Usage Page (Generic Dev)
    0x09, 0x20,        //   Usage (Battery Strength)
    0x15, 0x00,        //   Logical Minimum (0)
    0x26, 0xFF, 0x00,  //   Logical Maximum (255)
    0x75, 0x08,        //   Report Size (8)
    0x95, 0x01,        //   Report Count (1)
    0x81, 0x02,        //   Input (Data,Var,Abs)
    // Not part of the original: the status report, see GetFeature.
    0x85, 0xE1,        //   Report ID (225)
    0x06, 0x00, 0xFF,  //   Usage Page (Vendor 0xFF00)
    0x09, 0x01,        //   Usage (0x01)
    0x95, 0x3F,        //   Report Count (63)
    0xB1, 0x02,        //   Feature (Data,Var,Abs)
    0xC0,              // End Collection
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
    XBOX_VID,
    XBOX_PID,
    XBOX_VERSION,
};

// Sticks centered, triggers and buttons released.
static const UCHAR IdleControls[PAD_CONTROLS_LEN] = {0x00, 0x80, 0x00, 0x80, 0x00, 0x80, 0x00, 0x80};

DRIVER_INITIALIZE DriverEntry;
EVT_WDF_DRIVER_DEVICE_ADD EvtDeviceAdd;
EVT_WDF_IO_QUEUE_IO_DEVICE_CONTROL EvtIoDeviceControl;

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

// Builds the input report with the given ID from the current controls and
// returns its length, 0 for unknown IDs. Caller holds the lock.
static size_t
BuildInputReport(PDEVICE_CONTEXT Context, UCHAR ReportId, PUCHAR Report)
{
    Report[0] = ReportId;
    switch (ReportId) {
    case REPORT_ID_GAMEPAD:
        RtlCopyMemory(Report + 1, Context->Controls, PAD_GAMEPAD_LEN);
        return GAMEPAD_REPORT_LEN;
    case REPORT_ID_GUIDE:
        Report[1] = Context->Controls[PAD_GUIDE_INDEX];
        return GUIDE_REPORT_LEN;
    case REPORT_ID_BATTERY:
        Report[1] = 0xFF;
        return BATTERY_REPORT_LEN;
    default:
        return 0;
    }
}

// Builds the next input report that has unsent changes and returns its
// length, 0 when there is none. Caller holds the lock.
static size_t
NextChangedReport(PDEVICE_CONTEXT Context, PUCHAR Report)
{
    if (Context->GamepadChanged) {
        Context->GamepadChanged = FALSE;
        return BuildInputReport(Context, REPORT_ID_GAMEPAD, Report);
    }
    if (Context->GuideChanged) {
        Context->GuideChanged = FALSE;
        return BuildInputReport(Context, REPORT_ID_GUIDE, Report);
    }
    return 0;
}

VOID
PadPublishControls(PDEVICE_CONTEXT Context, const UCHAR *Controls)
{
    AcquireSRWLockExclusive(&Context->Lock);
    if (!RtlEqualMemory(Context->Controls, Controls, PAD_GAMEPAD_LEN)) {
        Context->GamepadChanged = TRUE;
    }
    if (Context->Controls[PAD_GUIDE_INDEX] != Controls[PAD_GUIDE_INDEX]) {
        Context->GuideChanged = TRUE;
    }
    RtlCopyMemory(Context->Controls, Controls, PAD_CONTROLS_LEN);

    // Taking pending reads under the lock keeps ReadReport from parking a
    // request right after this update, which would deliver it late.
    while (Context->GamepadChanged || Context->GuideChanged) {
        UCHAR report[MAX_INPUT_REPORT_LEN];
        WDFREQUEST read;
        size_t length;

        if (!NT_SUCCESS(WdfIoQueueRetrieveNextRequest(Context->ReadQueue, &read))) {
            break;
        }
        length = NextChangedReport(Context, report);
        WdfRequestComplete(read, CopyToRequest(read, report, length));
    }
    ReleaseSRWLockExclusive(&Context->Lock);
}

static NTSTATUS
ReadReport(PDEVICE_CONTEXT Context, WDFREQUEST Request, BOOLEAN *Complete)
{
    UCHAR report[MAX_INPUT_REPORT_LEN];
    size_t length;
    NTSTATUS status;

    AcquireSRWLockExclusive(&Context->Lock);
    length = NextChangedReport(Context, report);
    if (length != 0) {
        ReleaseSRWLockExclusive(&Context->Lock);
        return CopyToRequest(Request, report, length);
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
    NTSTATUS status = GetInputBuffer(Request, RUMBLE_REPORT_MIN_LEN, &report, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (report[0] != REPORT_ID_RUMBLE) {
        return STATUS_INVALID_PARAMETER;
    }

    UCHAR strong = (report[1] & RUMBLE_ENABLE_STRONG) ? min(report[4], RUMBLE_MAGNITUDE_MAX) : 0;
    UCHAR weak = (report[1] & RUMBLE_ENABLE_WEAK) ? min(report[5], RUMBLE_MAGNITUDE_MAX) : 0;
    StadiaSetRumble(Context, (UCHAR)(strong * 255 / RUMBLE_MAGNITUDE_MAX),
                    (UCHAR)(weak * 255 / RUMBLE_MAGNITUDE_MAX));
    WdfRequestSetInformation(Request, length);
    return STATUS_SUCCESS;
}

// The input buffer of GET_INPUT_REPORT holds the report ID, the output buffer
// receives the whole report including the ID.
static NTSTATUS
GetInputReport(PDEVICE_CONTEXT Context, WDFREQUEST Request)
{
    UCHAR report[MAX_INPUT_REPORT_LEN];
    PUCHAR id;
    size_t length;
    NTSTATUS status = GetInputBuffer(Request, 1, &id, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    AcquireSRWLockShared(&Context->Lock);
    length = BuildInputReport(Context, *id, report);
    ReleaseSRWLockShared(&Context->Lock);
    return length == 0 ? STATUS_INVALID_PARAMETER : CopyToRequest(Request, report, length);
}

// Same buffer conventions as GET_INPUT_REPORT.
static NTSTATUS
GetFeature(PDEVICE_CONTEXT Context, WDFREQUEST Request)
{
    UCHAR report[STATUS_REPORT_LEN] = {REPORT_ID_STATUS};
    PUCHAR id;
    size_t length;
    NTSTATUS status = GetInputBuffer(Request, 1, &id, &length);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    if (*id != REPORT_ID_STATUS) {
        return STATUS_INVALID_PARAMETER;
    }

    // [1] rumble failed, [2..6] last error NTSTATUS (LE), [6] length of the
    // last raw Stadia input report, [7..] that report
    AcquireSRWLockShared(&Context->Lock);
    report[1] = Context->RumbleFailed;
    RtlCopyMemory(report + 2, &Context->LastError, sizeof(NTSTATUS));
    report[6] = Context->RawReportLength;
    RtlCopyMemory(report + 7, Context->RawReport, Context->RawReportLength);
    ReleaseSRWLockShared(&Context->Lock);
    return CopyToRequest(Request, report, sizeof(report));
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
        static const WCHAR manufacturer[] = L"Microsoft";
        return CopyToRequest(Request, manufacturer, sizeof(manufacturer));
    }
    case HID_STRING_ID_IPRODUCT: {
        static const WCHAR product[] = L"Xbox Wireless Controller";
        return CopyToRequest(Request, product, sizeof(product));
    }
    case HID_STRING_ID_ISERIALNUMBER: {
        static const WCHAR serial[] = L"WinStadia";
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

// The INF puts the driver on the controller's USB HID interface and on its
// HID service on Bluetooth LE.
static BOOLEAN
IsBluetooth(PWDFDEVICE_INIT DeviceInit)
{
    WDFMEMORY memory;
    BOOLEAN bluetooth = FALSE;

    if (NT_SUCCESS(WdfFdoInitAllocAndQueryProperty(DeviceInit, DevicePropertyEnumeratorName, NonPagedPoolNx,
                                                   WDF_NO_OBJECT_ATTRIBUTES, &memory))) {
        bluetooth = _wcsicmp(WdfMemoryGetBuffer(memory, NULL), L"BTHLEDevice") == 0;
        WdfObjectDelete(memory);
    }
    return bluetooth;
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
    BOOLEAN bluetooth;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(Driver);

    // mshidumdf.sys is the function driver, this driver sits below it.
    WdfFdoInitSetFilter(DeviceInit);
    bluetooth = IsBluetooth(DeviceInit);

    WDF_PNPPOWER_EVENT_CALLBACKS_INIT(&pnpCallbacks);
    pnpCallbacks.EvtDevicePrepareHardware = StadiaPrepareHardware;
    pnpCallbacks.EvtDeviceD0Entry = StadiaD0Entry;
    pnpCallbacks.EvtDeviceD0Exit = StadiaD0Exit;
    WdfDeviceInitSetPnpPowerEventCallbacks(DeviceInit, &pnpCallbacks);

    WDF_OBJECT_ATTRIBUTES_INIT_CONTEXT_TYPE(&attributes, DEVICE_CONTEXT);
    status = WdfDeviceCreate(&DeviceInit, &attributes, &device);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    context = GetDeviceContext(device);
    context->Transport = bluetooth ? &BluetoothTransport : &UsbTransport;
    InitializeSRWLock(&context->Lock);
    InitializeSRWLock(&context->RumbleLock);
    RtlCopyMemory(context->Controls, IdleControls, PAD_CONTROLS_LEN);

    // Parallel, so that a rumble write on the wire does not hold up reads.
    WDF_IO_QUEUE_CONFIG_INIT_DEFAULT_QUEUE(&queueConfig, WdfIoQueueDispatchParallel);
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
