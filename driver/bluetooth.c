// Bluetooth transport: the inbox HID over GATT driver stays in the stack and
// this driver sits on top of it, in the same driver host. It asks the driver
// below for Stadia input reports the way the HID class driver would. Stadia
// output reports go around it, through bthserv; further down is why.

#include "winstadia.h"
#include <hidport.h>

#define RETRY_DELAY_MS 500
#define GATT_REOPEN_DELAY_MS 3000
#define BLUETOOTH_ADDRESS_DIGITS 12
// Room for any input report of the controller.
#define READ_BUFFER_LEN 64

EVT_WDF_REQUEST_COMPLETION_ROUTINE EvtBluetoothReadComplete;
EVT_WDF_TIMER EvtBluetoothRetryTimer;

static VOID
ReadFailed(PBLUETOOTH_READER Reader, NTSTATUS Status)
{
    PDEVICE_CONTEXT context = Reader->Context;
    BOOLEAN retry;

    AcquireSRWLockExclusive(&context->Lock);
    Reader->Sent = FALSE;
    // Stopping cancels the reads, that is no error.
    if (Status != STATUS_CANCELLED) {
        context->LastError = Status;
    }
    retry = context->Reading;
    ReleaseSRWLockExclusive(&context->Lock);

    if (retry) {
        WdfTimerStart(context->RetryTimer, WDF_REL_TIMEOUT_IN_MS(RETRY_DELAY_MS));
    }
}

// Sends the reader's request down, unless it is there already or the
// transport is stopped.
static VOID
SendRead(PBLUETOOTH_READER Reader)
{
    PDEVICE_CONTEXT context = Reader->Context;
    WDF_REQUEST_REUSE_PARAMS reuse;
    BOOLEAN send;
    NTSTATUS status;

    AcquireSRWLockExclusive(&context->Lock);
    send = context->Reading && !Reader->Sent;
    if (send) {
        Reader->Sent = TRUE;
    }
    ReleaseSRWLockExclusive(&context->Lock);
    if (!send) {
        return;
    }

    WDF_REQUEST_REUSE_PARAMS_INIT(&reuse, WDF_REQUEST_REUSE_NO_FLAGS, STATUS_SUCCESS);
    WdfRequestReuse(Reader->Request, &reuse);
    status = WdfIoTargetFormatRequestForIoctl(context->LowerDriver, Reader->Request, IOCTL_HID_READ_REPORT, NULL,
                                              NULL, Reader->Buffer, NULL);
    if (NT_SUCCESS(status)) {
        WdfRequestSetCompletionRoutine(Reader->Request, EvtBluetoothReadComplete, Reader);
        if (WdfRequestSend(Reader->Request, context->LowerDriver, WDF_NO_SEND_OPTIONS)) {
            return;
        }
        status = WdfRequestGetStatus(Reader->Request);
    }
    ReadFailed(Reader, status);
}

VOID
EvtBluetoothReadComplete(WDFREQUEST Request, WDFIOTARGET Target, PWDF_REQUEST_COMPLETION_PARAMS Params,
                         WDFCONTEXT Context)
{
    PBLUETOOTH_READER reader = Context;
    PDEVICE_CONTEXT context = reader->Context;
    size_t capacity;
    const UCHAR *report = WdfMemoryGetBuffer(reader->Buffer, &capacity);

    UNREFERENCED_PARAMETER(Request);
    UNREFERENCED_PARAMETER(Target);

    if (!NT_SUCCESS(Params->IoStatus.Status)) {
        ReadFailed(reader, Params->IoStatus.Status);
        return;
    }
    if (Params->IoStatus.Information == 0) {
        // Reading on right away could spin if the driver below keeps doing this.
        ReadFailed(reader, STATUS_NO_DATA_DETECTED);
        return;
    }
    StadiaInputReport(context, report, min(Params->IoStatus.Information, capacity));

    AcquireSRWLockExclusive(&context->Lock);
    reader->Sent = FALSE;
    ReleaseSRWLockExclusive(&context->Lock);
    SendRead(reader);
}

VOID
EvtBluetoothRetryTimer(WDFTIMER Timer)
{
    PDEVICE_CONTEXT context = GetDeviceContext(WdfTimerGetParentObject(Timer));

    for (int i = 0; i < BLUETOOTH_READER_COUNT; i++) {
        SendRead(&context->Readers[i]);
    }
}

// Rumble does not go down the HID path. The driver below fails the write with
// 0xC0070057 (invalid parameter), as it does for HidD_SetOutputReport on the
// plain inbox stack. The likely reason is in the controller's GATT table: the
// output report's characteristic (0x2A4D, value handle 0x0044) is declared
// with Read and Write only, where the HID over GATT profile demands Write
// Without Response as well. Protocol Mode and the control point lack it too.
//
// Writing the characteristic with the Bluetooth GATT API
// (BluetoothGATTSetCharacteristicValue) was tried and is closed off:
// - The API needs a handle of the service. All it does with the handle is
//   fetch 16 bytes that stand for that open (IOCTL 0x00411490); reads and
//   writes then go to bthserv over RPC. The handle of the whole device lists
//   the table but fails every read and write with ERROR_INVALID_FUNCTION.
// - The service's device interface cannot be opened: the create ends at the
//   HID class driver on top of this stack (error 31).
// - From inside the driver host, WdfIoTargetOpen by file either fails with
//   access denied, or, with UmdfDispatcher=FileHandle, succeeds without
//   giving a handle, and the IOCTL above then fails with STATUS_FILE_CLOSED.
//   A filter as lowest driver of the stack gets the same. BthLEEnum seems to
//   refuse user mode opens of protected services, as it does for 0x1800 and
//   0x1801 (error 5). The reflector's control devices (\\.\UMDFCtrlDev-*)
//   refuse a CreateFile from the host as well.
// The public WinRT API (Windows.Devices.Bluetooth.GenericAttributeProfile,
// what bleak and Web Bluetooth use) is closed off as well: GetGattServices
// lists 0x1812, but RequestAccess on it returns DeniedBySystem and
// GetCharacteristics returns AccessDenied, offline and connected alike,
// while the other services are allowed. Reports of a plain GATT write
// making the motors run come from macOS and Linux, whose stacks let user
// mode at the HID service.
// What works is what the driver below uses itself: the WinRT class
// Microsoft.Bluetooth.Profiles.Gatt.Interface.GattClientDevice, served by
// bthserv, with GattClientCharacteristic and GattClientWriteResult next to
// it. It is private, without metadata; private-gatt.cpp calls it by vtable
// slot and writes the characteristic with response.

static VOID
CloseGattSession(PDEVICE_CONTEXT Context)
{
    PrivateGattClose(Context->GattSession);
    Context->GattSession = NULL;
}

// Opens the session rumble is written through, unless there is one. The
// caller holds RumbleLock.
static HRESULT
OpenGattSession(PDEVICE_CONTEXT Context)
{
    if (Context->GattSession != NULL) {
        return S_OK;
    }
    // An attempt takes seconds to fail, and games write rumble all the time.
    if (Context->GattOpenTick != 0 && GetTickCount64() - Context->GattOpenTick < GATT_REOPEN_DELAY_MS) {
        return Context->GattOpenStatus;
    }
    Context->GattOpenStatus =
        PrivateGattOpen(Context->BluetoothAddress, STADIA_RUMBLE_REPORT_ID, &Context->GattSession);
    Context->GattOpenTick = GetTickCount64();
    return Context->GattOpenStatus;
}

// Waits for the controller's answer, up to the two seconds private-gatt.cpp
// allows. The status is an HRESULT, which tells failure by its sign as well.
static NTSTATUS
BluetoothSendOutputReport(PDEVICE_CONTEXT Context, PUCHAR Report, size_t Length)
{
    BOOLEAN started;
    HRESULT hr;

    AcquireSRWLockShared(&Context->Lock);
    started = Context->Reading;
    ReleaseSRWLockShared(&Context->Lock);
    if (!started) {
        return STATUS_DEVICE_NOT_READY;
    }

    hr = OpenGattSession(Context);
    if (SUCCEEDED(hr)) {
        // The report ID is in the characteristic's Report Reference
        // descriptor, not in its value.
        hr = PrivateGattWrite(Context->GattSession, Report + 1, (ULONG)(Length - 1));
        if (FAILED(hr)) {
            // The session may have gone stale; the next write opens another.
            CloseGattSession(Context);
        }
    }
    return (NTSTATUS)hr;
}

static NTSTATUS
GetBluetoothAddress(WDFDEVICE Device, UINT64 *Address)
{
    // DEVPKEY_Bluetooth_DeviceAddress: twelve hex digits.
    static const DEVPROPKEY addressKey = {
        {0x2bd67d8b, 0x8beb, 0x48d5, {0x87, 0xe0, 0x6c, 0xda, 0x34, 0x28, 0x04, 0x0a}}, 1
    };
    WDF_DEVICE_PROPERTY_DATA property;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFMEMORY memory;
    DEVPROPTYPE type;
    NTSTATUS status;
    size_t size;
    WCHAR *text;
    WCHAR *end;

    WDF_DEVICE_PROPERTY_DATA_INIT(&property, &addressKey);
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfDeviceAllocAndQueryPropertyEx(Device, &property, NonPagedPoolNx, &attributes, &memory, &type);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    status = STATUS_INVALID_PARAMETER;
    text = WdfMemoryGetBuffer(memory, &size);
    if (type == DEVPROP_TYPE_STRING && size == (BLUETOOTH_ADDRESS_DIGITS + 1) * sizeof(WCHAR) &&
        text[BLUETOOTH_ADDRESS_DIGITS] == L'\0') {
        *Address = _wcstoui64(text, &end, 16);
        if (end == text + BLUETOOTH_ADDRESS_DIGITS && *Address != 0) {
            status = STATUS_SUCCESS;
        }
    }
    WdfObjectDelete(memory);
    return status;
}

static NTSTATUS
BluetoothPrepareHardware(WDFDEVICE Device)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    WDF_OBJECT_ATTRIBUTES attributes;
    WDF_TIMER_CONFIG timerConfig;
    NTSTATUS status;

    // Survives a stop and restart of the device, so set it up only once.
    if (context->LowerDriver != NULL) {
        return STATUS_SUCCESS;
    }

    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    for (int i = 0; i < BLUETOOTH_READER_COUNT; i++) {
        PBLUETOOTH_READER reader = &context->Readers[i];

        reader->Context = context;
        status = WdfRequestCreate(&attributes, WdfDeviceGetIoTarget(Device), &reader->Request);
        if (!NT_SUCCESS(status)) {
            return status;
        }
        status = WdfMemoryCreate(&attributes, NonPagedPoolNx, 0, READ_BUFFER_LEN, &reader->Buffer, NULL);
        if (!NT_SUCCESS(status)) {
            return status;
        }
    }

    WDF_TIMER_CONFIG_INIT(&timerConfig, EvtBluetoothRetryTimer);
    timerConfig.AutomaticSerialization = FALSE;
    status = WdfTimerCreate(&timerConfig, &attributes, &context->RetryTimer);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    // Only rumble needs the address, so input works without.
    status = GetBluetoothAddress(Device, &context->BluetoothAddress);
    if (!NT_SUCCESS(status)) {
        StadiaRecordError(context, status);
    }

    context->LowerDriver = WdfDeviceGetIoTarget(Device);
    return STATUS_SUCCESS;
}

static NTSTATUS
BluetoothStart(PDEVICE_CONTEXT Context)
{
    HRESULT hr;
    NTSTATUS status = WdfIoTargetStart(Context->LowerDriver);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    AcquireSRWLockExclusive(&Context->Lock);
    Context->Reading = TRUE;
    ReleaseSRWLockExclusive(&Context->Lock);
    for (int i = 0; i < BLUETOOTH_READER_COUNT; i++) {
        SendRead(&Context->Readers[i]);
    }

    // Now, so that the first rumble of a game does not wait for it. Should it
    // fail, as it may right after the controller connected, the writes try
    // again.
    AcquireSRWLockExclusive(&Context->RumbleLock);
    Context->GattOpenTick = 0;
    hr = OpenGattSession(Context);
    ReleaseSRWLockExclusive(&Context->RumbleLock);
    if (FAILED(hr)) {
        StadiaRecordError(Context, (NTSTATUS)hr);
    }
    return STATUS_SUCCESS;
}

static VOID
BluetoothStop(PDEVICE_CONTEXT Context)
{
    AcquireSRWLockExclusive(&Context->Lock);
    Context->Reading = FALSE;
    ReleaseSRWLockExclusive(&Context->Lock);

    // A write that slips in after this finds Reading cleared and opens none.
    AcquireSRWLockExclusive(&Context->RumbleLock);
    CloseGattSession(Context);
    ReleaseSRWLockExclusive(&Context->RumbleLock);

    // A retry that slips in after this finds Reading cleared and does nothing.
    WdfTimerStop(Context->RetryTimer, TRUE);
    WdfIoTargetStop(Context->LowerDriver, WdfIoTargetCancelSentIo);
}

const TRANSPORT BluetoothTransport = {
    BluetoothPrepareHardware,
    BluetoothStart,
    BluetoothStop,
    BluetoothSendOutputReport,
    NULL,
};
