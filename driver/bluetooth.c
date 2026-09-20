// Bluetooth transport: the inbox HID over GATT driver stays in the stack and
// this driver sits on top of it, in the same driver host. It asks the driver
// below for Stadia input reports the way the HID class driver would, and hands
// it Stadia output reports.

#include "winstadia.h"
#include <hidport.h>

#define WRITE_TIMEOUT_MS 2000
#define RETRY_DELAY_MS 500
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

static NTSTATUS
BluetoothSendOutputReport(PDEVICE_CONTEXT Context, PUCHAR Report, size_t Length)
{
    // A UMDF HID minidriver finds the report in the input buffer and the
    // report ID in the length of the output buffer.
    UCHAR unused[UCHAR_MAX];
    WDF_MEMORY_DESCRIPTOR input;
    WDF_MEMORY_DESCRIPTOR output;
    WDF_REQUEST_SEND_OPTIONS options;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&input, Report, (ULONG)Length);
    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&output, unused, Report[0]);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(WRITE_TIMEOUT_MS));

    return WdfIoTargetSendIoctlSynchronously(Context->LowerDriver, NULL, IOCTL_HID_WRITE_REPORT, &input, &output,
                                             &options, NULL);
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

    context->LowerDriver = WdfDeviceGetIoTarget(Device);
    return STATUS_SUCCESS;
}

static NTSTATUS
BluetoothStart(PDEVICE_CONTEXT Context)
{
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
    return STATUS_SUCCESS;
}

static VOID
BluetoothStop(PDEVICE_CONTEXT Context)
{
    AcquireSRWLockExclusive(&Context->Lock);
    Context->Reading = FALSE;
    ReleaseSRWLockExclusive(&Context->Lock);

    // A retry that slips in after this finds Reading cleared and does nothing.
    WdfTimerStop(Context->RetryTimer, TRUE);
    WdfIoTargetStop(Context->LowerDriver, WdfIoTargetCancelSentIo);
}

const TRANSPORT BluetoothTransport = {
    BluetoothPrepareHardware,
    BluetoothStart,
    BluetoothStop,
    BluetoothSendOutputReport,
};
