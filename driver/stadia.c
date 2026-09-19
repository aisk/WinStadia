// USB side: reads Stadia input reports from the interrupt endpoint, translates
// them to DS4 controls and sends rumble the other way. WinUSB sits below this
// driver and carries the transfers.

#include "winstadia.h"

#define WRITE_TIMEOUT_MS 500

// HID class request and report type, for output reports sent through the
// control endpoint.
#define HID_REQUEST_SET_REPORT 0x09
#define HID_REPORT_TYPE_OUTPUT 0x02

// Stadia input report layout:
//   [0] report ID (0x03)
//   [1] d-pad hat: 0 = up, clockwise to 7 = up-left, 8 = released
//   [2] RS click, Options, Menu, Stadia, R2, L2, Assistant, Capture (bit 7..0)
//   [3] -, A, B, X, Y, L1, R1, LS click (bit 7..0)
//   [4..8] left X, left Y, right X, right Y: centered at 128, Y grows downwards
//   [8..10] L2, R2 analog
#define STADIA_INPUT_REPORT_ID 0x03
#define STADIA_INPUT_REPORT_LEN 10
#define STADIA_RUMBLE_REPORT_ID 0x05
#define STADIA_HAT_RELEASED 8

#define STADIA_B2_RS 0x80
#define STADIA_B2_OPTIONS 0x40
#define STADIA_B2_MENU 0x20
#define STADIA_B2_STADIA 0x10
#define STADIA_B2_CAPTURE 0x01

#define STADIA_B3_A 0x40
#define STADIA_B3_B 0x20
#define STADIA_B3_X 0x10
#define STADIA_B3_Y 0x08
#define STADIA_B3_L1 0x04
#define STADIA_B3_R1 0x02
#define STADIA_B3_LS 0x01

// DS4 controls, indexed as in PAD_CONTROLS_LEN (input report offset minus 1):
//   [0..4] left X, left Y, right X, right Y, same conventions as the Stadia
//   [4] triangle, circle, cross, square (bit 7..4), hat (bit 3..0)
//   [5] R3, L3, Options, Share, R2, L2, R1, L1 (bit 7..0)
//   [6] touchpad click, PS (bit 1..0)
//   [7..9] L2, R2 analog
#define DS4_B4_TRIANGLE 0x80
#define DS4_B4_CIRCLE 0x40
#define DS4_B4_CROSS 0x20
#define DS4_B4_SQUARE 0x10

#define DS4_B5_R3 0x80
#define DS4_B5_L3 0x40
#define DS4_B5_OPTIONS 0x20
#define DS4_B5_SHARE 0x10
#define DS4_B5_R2 0x08
#define DS4_B5_L2 0x04
#define DS4_B5_R1 0x02
#define DS4_B5_L1 0x01

#define DS4_B6_TOUCHPAD 0x02
#define DS4_B6_PS 0x01

#define BIT(source, mask, target) (((source) & (mask)) ? (target) : 0)

EVT_WDF_USB_READER_COMPLETION_ROUTINE EvtInputReport;
EVT_WDF_USB_READERS_FAILED EvtReadersFailed;

// Buttons map by position: A/B/X/Y to cross/circle/square/triangle, Options
// and Menu to Share and Options, Stadia to PS, Capture to the touchpad click.
static VOID
MapControls(const UCHAR *Stadia, UCHAR *Controls)
{
    UCHAR b2 = Stadia[2];
    UCHAR b3 = Stadia[3];

    RtlCopyMemory(Controls, Stadia + 4, 4);
    Controls[4] = (UCHAR)(min(Stadia[1], STADIA_HAT_RELEASED) |
                          BIT(b3, STADIA_B3_Y, DS4_B4_TRIANGLE) |
                          BIT(b3, STADIA_B3_B, DS4_B4_CIRCLE) |
                          BIT(b3, STADIA_B3_A, DS4_B4_CROSS) |
                          BIT(b3, STADIA_B3_X, DS4_B4_SQUARE));
    Controls[5] = (UCHAR)(BIT(b2, STADIA_B2_RS, DS4_B5_R3) |
                          BIT(b3, STADIA_B3_LS, DS4_B5_L3) |
                          BIT(b2, STADIA_B2_MENU, DS4_B5_OPTIONS) |
                          BIT(b2, STADIA_B2_OPTIONS, DS4_B5_SHARE) |
                          (Stadia[9] ? DS4_B5_R2 : 0) |
                          (Stadia[8] ? DS4_B5_L2 : 0) |
                          BIT(b3, STADIA_B3_R1, DS4_B5_R1) |
                          BIT(b3, STADIA_B3_L1, DS4_B5_L1));
    Controls[6] = (UCHAR)(BIT(b2, STADIA_B2_CAPTURE, DS4_B6_TOUCHPAD) |
                          BIT(b2, STADIA_B2_STADIA, DS4_B6_PS));
    Controls[7] = Stadia[8];
    Controls[8] = Stadia[9];
}

static VOID
RecordError(PDEVICE_CONTEXT Context, NTSTATUS Status)
{
    AcquireSRWLockExclusive(&Context->Lock);
    Context->LastError = Status;
    ReleaseSRWLockExclusive(&Context->Lock);
}

VOID
EvtInputReport(WDFUSBPIPE Pipe, WDFMEMORY Buffer, size_t NumBytesTransferred, WDFCONTEXT Context)
{
    PDEVICE_CONTEXT context = Context;
    const UCHAR *report = WdfMemoryGetBuffer(Buffer, NULL);
    UCHAR controls[PAD_CONTROLS_LEN];

    UNREFERENCED_PARAMETER(Pipe);

    AcquireSRWLockExclusive(&context->Lock);
    context->RawReportLength = (UCHAR)min(NumBytesTransferred, RAW_REPORT_MAX);
    RtlCopyMemory(context->RawReport, report, context->RawReportLength);
    ReleaseSRWLockExclusive(&context->Lock);

    if (NumBytesTransferred >= STADIA_INPUT_REPORT_LEN && report[0] == STADIA_INPUT_REPORT_ID) {
        MapControls(report, controls);
        PadPublishControls(context, controls);
    }
}

BOOLEAN
EvtReadersFailed(WDFUSBPIPE Pipe, NTSTATUS Status, USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(UsbdStatus);

    RecordError(GetDeviceContext(WdfIoTargetGetDevice(WdfUsbTargetPipeGetIoTarget(Pipe))), Status);
    // Have the framework reset the pipe and restart the reader.
    return TRUE;
}

static NTSTATUS
SendRumble(PDEVICE_CONTEXT Context, UCHAR Strong, UCHAR Weak)
{
    // Motor speeds are 16 bit little endian; x * 257 scales 8 to 16 bits.
    UCHAR report[] = {STADIA_RUMBLE_REPORT_ID, Strong, Strong, Weak, Weak};
    WDF_MEMORY_DESCRIPTOR memory;
    WDF_REQUEST_SEND_OPTIONS options;
    WDF_USB_CONTROL_SETUP_PACKET setup;

    WDF_MEMORY_DESCRIPTOR_INIT_BUFFER(&memory, report, sizeof(report));
    WDF_REQUEST_SEND_OPTIONS_INIT(&options, WDF_REQUEST_SEND_OPTION_TIMEOUT);
    WDF_REQUEST_SEND_OPTIONS_SET_TIMEOUT(&options, WDF_REL_TIMEOUT_IN_MS(WRITE_TIMEOUT_MS));

    if (Context->OutputPipe != NULL) {
        return WdfUsbTargetPipeWriteSynchronously(Context->OutputPipe, NULL, &options, &memory, NULL);
    }
    WDF_USB_CONTROL_SETUP_PACKET_INIT_CLASS(&setup, BmRequestHostToDevice, BmRequestToInterface,
                                            HID_REQUEST_SET_REPORT,
                                            (HID_REPORT_TYPE_OUTPUT << 8) | STADIA_RUMBLE_REPORT_ID,
                                            Context->InterfaceNumber);
    return WdfUsbTargetDeviceSendControlTransferSynchronously(Context->UsbDevice, NULL, &options, &setup, &memory,
                                                              NULL);
}

VOID
StadiaSetRumble(PDEVICE_CONTEXT Context, UCHAR Strong, UCHAR Weak)
{
    NTSTATUS status = STATUS_SUCCESS;

    // Games repeat the same output report a lot; only changes reach the wire.
    AcquireSRWLockExclusive(&Context->RumbleLock);
    if (Strong != Context->RumbleStrong || Weak != Context->RumbleWeak) {
        status = SendRumble(Context, Strong, Weak);
        if (NT_SUCCESS(status)) {
            Context->RumbleStrong = Strong;
            Context->RumbleWeak = Weak;
        }
    }
    ReleaseSRWLockExclusive(&Context->RumbleLock);

    AcquireSRWLockExclusive(&Context->Lock);
    Context->RumbleFailed = !NT_SUCCESS(status);
    if (!NT_SUCCESS(status)) {
        Context->LastError = status;
    }
    ReleaseSRWLockExclusive(&Context->Lock);
}

NTSTATUS
StadiaPrepareHardware(WDFDEVICE Device, WDFCMRESLIST ResourcesRaw, WDFCMRESLIST ResourcesTranslated)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);
    WDF_USB_DEVICE_CREATE_CONFIG createConfig;
    WDF_USB_DEVICE_SELECT_CONFIG_PARAMS selectParams;
    WDF_USB_CONTINUOUS_READER_CONFIG readerConfig;
    WDF_USB_PIPE_INFORMATION pipeInfo;
    WDFUSBINTERFACE usbInterface;
    ULONG inputPacketSize = 0;
    NTSTATUS status;

    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

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

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&readerConfig, EvtInputReport, context, inputPacketSize);
    readerConfig.EvtUsbTargetPipeReadersFailed = EvtReadersFailed;
    return WdfUsbTargetPipeConfigContinuousReader(context->InputPipe, &readerConfig);
}

NTSTATUS
StadiaD0Entry(WDFDEVICE Device, WDF_POWER_DEVICE_STATE PreviousState)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(PreviousState);

    // The motors are off after power up, whatever was requested before.
    AcquireSRWLockExclusive(&context->RumbleLock);
    context->RumbleStrong = 0;
    context->RumbleWeak = 0;
    ReleaseSRWLockExclusive(&context->RumbleLock);

    return WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(context->InputPipe));
}

NTSTATUS
StadiaD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(context->InputPipe), WdfIoTargetCancelSentIo);
    // Fails harmlessly when the controller is already gone.
    StadiaSetRumble(context, 0, 0);
    return STATUS_SUCCESS;
}
