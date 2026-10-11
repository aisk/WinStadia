// USB transport: the driver takes the place of hidusb on the controller's HID
// interface and talks to the endpoints itself. WinUSB sits below and carries
// the transfers.

#include "winstadia.h"

#define WRITE_TIMEOUT_MS 500
// A reader failure this soon after a pipe reset means the reset did not
// help; the device is restarted then.
#define RECOVERY_RETRY_MS 5000

// HID class request and report type, for output reports sent through the
// control endpoint.
#define HID_REQUEST_SET_REPORT 0x09
#define HID_REPORT_TYPE_OUTPUT 0x02

EVT_WDF_USB_READER_COMPLETION_ROUTINE EvtUsbInputReport;
EVT_WDF_USB_READERS_FAILED EvtUsbReadersFailed;
EVT_WDF_WORKITEM EvtUsbRecovery;

VOID
EvtUsbInputReport(WDFUSBPIPE Pipe, WDFMEMORY Buffer, size_t NumBytesTransferred, WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    StadiaInputReport(Context, WdfMemoryGetBuffer(Buffer, NULL), NumBytesTransferred);
}

static VOID
UsbReaderFailed(PDEVICE_CONTEXT Context, NTSTATUS Status, USBD_STATUS UsbdStatus)
{
    AcquireSRWLockExclusive(&Context->Lock);
    Context->LastError = Status;
    Context->LastUsbdStatus = UsbdStatus;
    Context->ReaderFailures++;
    ReleaseSRWLockExclusive(&Context->Lock);

    // No input arrives from here on, so games must not keep seeing the
    // buttons the controller last reported as held.
    PadReleaseControls(Context);

    // Returning TRUE asks the framework to reset the pipe and restart the
    // reader, but in UMDF it never does: it first checks that the port is
    // connected, and FxUsbDevice::IsConnected always fails in user mode. The
    // reader then stayed down until the controller was replugged. The work
    // item recovers instead.
    WdfWorkItemEnqueue(Context->RecoveryWorkItem);
}

BOOLEAN
EvtUsbReadersFailed(WDFUSBPIPE Pipe, NTSTATUS Status, USBD_STATUS UsbdStatus)
{
    UsbReaderFailed(GetDeviceContext(WdfIoTargetGetDevice(WdfUsbTargetPipeGetIoTarget(Pipe))), Status, UsbdStatus);
    return FALSE;
}

static VOID
CountRecovery(PDEVICE_CONTEXT Context, ULONG *Counter, NTSTATUS Status)
{
    AcquireSRWLockExclusive(&Context->Lock);
    (*Counter)++;
    if (!NT_SUCCESS(Status)) {
        Context->LastError = Status;
    }
    ReleaseSRWLockExclusive(&Context->Lock);
}

// Resets the input pipe and restarts the reader. When that fails, or the
// reader fails again soon after, it gives up on the device: asking for a
// restart removes it, and on USB it stays away until it is replugged, but
// with its controls released rather than stuck. A port reset, the step in
// between that the USB documentation recommends, left the controller's
// output pipe hanging on the first rumble write after it.
VOID
EvtUsbRecovery(WDFWORKITEM WorkItem)
{
    WDFDEVICE device = WdfWorkItemGetParentObject(WorkItem);
    PDEVICE_CONTEXT context = GetDeviceContext(device);
    WDFIOTARGET input = WdfUsbTargetPipeGetIoTarget(context->InputPipe);
    ULONGLONG now = GetTickCount64();
    NTSTATUS status = STATUS_UNSUCCESSFUL;
    BOOLEAN running;
    BOOLEAN retry;

    AcquireSRWLockShared(&context->Lock);
    running = context->UsbRunning;
    ReleaseSRWLockShared(&context->Lock);
    if (!running) {
        return;
    }

    retry = context->LastRecoveryTick != 0 && now - context->LastRecoveryTick < RECOVERY_RETRY_MS;
    context->LastRecoveryTick = now;
    if (!retry) {
        // The reset goes through while the target is stopped; stopping it
        // first is what lets Start send the readers again.
        WdfIoTargetStop(input, WdfIoTargetCancelSentIo);
        status = WdfUsbTargetPipeResetSynchronously(context->InputPipe, NULL, NULL);
        if (NT_SUCCESS(status)) {
            status = WdfIoTargetStart(input);
        }
        CountRecovery(context, &context->PipeResets, status);
        if (NT_SUCCESS(status)) {
            return;
        }
    }

    CountRecovery(context, &context->DeviceRestarts, STATUS_SUCCESS);
    WdfDeviceSetFailed(device, WdfDeviceFailedAttemptRestart);
}

static NTSTATUS
UsbSendOutputReport(PDEVICE_CONTEXT Context, PUCHAR Report, size_t Length)
{
    WDF_MEMORY_DESCRIPTOR memory;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_USB_CONTROL_SETUP_PACKET setup;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, Report, (ULONG)Length);
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(WRITE_TIMEOUT_MS));

    if (Context->OutputPipe != NULL) {
        return WdfUsbTargetPipeWriteSynchronously(Context->OutputPipe, NULL, &options, &memory, NULL);
    }
    WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS(&setup, BmRequestHostToDevice, BmRequestToInterface,
                                            HID_REQUEST_SET_REPORT, (HID_REPORT_TYPE_OUTPUT << 8) | Report[0],
                                            Context->InterfaceNumber);
    return WdfUsbTargetDeviceSendControlTransferSynchronously(Context->UsbDevice, NULL, &options, &setup, &memory,
                                                              NULL);
}

static NTSTATUS
UsbPrepareHardware(WDFDEVICE Device)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    WDF_USB_DEVICE_CREATE_CONFIG createConfig;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selectParams;
    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    WDF_USB_PIPE_INFORMATION pipeInfo;
    WDF_WORKITEM_CONFIG workItemConfig;
    WDF_OBJECT_ATTRIBUTES attributes;
    WDFUSBINTERFACE usbInterface;
    ULONG inputPacketSize = 0;
    NTSTATUS status;

    // Survives a stop and restart of the device, so set it up only once.
    if (context->UsbDevice != NULL) {
        return STATUS_SUCCESS;
    }

    WDF_USB_DEVICE_CREATE_CONFIG_INIT(&createConfig, USBD_CLIENT_CONTRACT_VERSION_602);
    status = WdfUsbTargetDeviceCreateWithParameters(Device, &createConfig, WDF_NO_OBJECT_ATTRIBUTES,
                                                    &context->UsbDevice);
    if (!NT_SUCCESS(status)) {
        return status;
    }
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS_INIT_SINGLE_INTERFACE(&selectParams);
    status = WdfUsbTargetDeviceSelectConfig(context->UsbDevice, WDF_NO_OBJECT_ATTRIBUTES, &selectParams);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    usbInterface = selectParams.Types.SingleInterface.ConfiguredUsbInterface;
    context->InterfaceNumber = WdfUsbInterfaceGetInterfaceNumber(usbInterface);
    for (UCHAR i = 0; i < WdfUsbInterfaceGetNumConfiguredPipes(usbInterface); i++) {
        WDFUSBPIPE pipe;

        WDF_USB_PIPE_INFORMATION_INIT(&pipeInfo);
        pipe = WdfUsbInterfaceGetConfiguredPipe(usbInterface, i, &pipeInfo);
        if (pipeInfo.PipeType != WdfUsbPipeTypeInterrupt) {
            continue;
        }
        if (WdfUsbTargetPipeIsInEndpoint(pipe) && context->InputPipe == NULL) {
            context->InputPipe = pipe;
            inputPacketSize = pipeInfo.MaximumPacketSize;
        } else if (WdfUsbTargetPipeIsOutEndpoint(pipe) && context->OutputPipe == NULL) {
            context->OutputPipe = pipe;
        }
    }
    if (context->InputPipe == NULL) {
        return STATUS_DEVICE_CONFIGURATION_ERROR;
    }

    WDF_WORKITEM_CONFIG_INIT(&workItemConfig, EvtUsbRecovery);
    workItemConfig.AutomaticSerialization = FALSE;
    WDF_OBJECT_ATTRIBUTES_INIT(&attributes);
    attributes.ParentObject = Device;
    status = WdfWorkItemCreate(&workItemConfig, &attributes, &context->RecoveryWorkItem);
    if (!NT_SUCCESS(status)) {
        return status;
    }

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&readerConfig, EvtUsbInputReport, context, inputPacketSize);
    readerConfig.EvtUsbTargetPipeReadersFailed = EvtUsbReadersFailed;
    return WdfUsbTargetPipeConfigContinuousReader(context->InputPipe, &readerConfig);
}

static NTSTATUS
UsbStart(PDEVICE_CONTEXT Context)
{
    AcquireSRWLockExclusive(&Context->Lock);
    Context->UsbRunning = TRUE;
    ReleaseSRWLockExclusive(&Context->Lock);
    return WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(Context->InputPipe));
}

static VOID
UsbStop(PDEVICE_CONTEXT Context)
{
    // A recovery still running would start the reader again.
    AcquireSRWLockExclusive(&Context->Lock);
    Context->UsbRunning = FALSE;
    ReleaseSRWLockExclusive(&Context->Lock);
    WdfWorkItemFlush(Context->RecoveryWorkItem);
    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Context->InputPipe), WdfIoTargetCancelSentIo);
}

// For status.ps1 -TestRecovery: goes through a reader failure without one.
static NTSTATUS
UsbTestRecovery(PDEVICE_CONTEXT Context)
{
    UsbReaderFailed(Context, STATUS_UNSUCCESSFUL, 0);
    return STATUS_SUCCESS;
}

const TRANSPORT UsbTransport = {UsbPrepareHardware, UsbStart, UsbStop, UsbSendOutputReport, UsbTestRecovery};
