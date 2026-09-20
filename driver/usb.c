// USB transport: the driver takes the place of hidusb on the controller's HID
// interface and talks to the endpoints itself. WinUSB sits below and carries
// the transfers.

#include "winstadia.h"

#define WRITE_TIMEOUT_MS 500

// HID class request and report type, for output reports sent through the
// control endpoint.
#define HID_REQUEST_SET_REPORT 0x09
#define HID_REPORT_TYPE_OUTPUT 0x02

EVT_WDF_USB_READER_COMPLETION_ROUTINE EvtUsbInputReport;
EVT_WDF_USB_READERS_FAILED EvtUsbReadersFailed;

VOID
EvtUsbInputReport(WDFUSBPIPE Pipe, WDFMEMORY Buffer, size_t NumBytesTransferred, WDFCONTEXT Context)
{
    UNREFERENCED_PARAMETER(Pipe);

    StadiaInputReport(Context, WdfMemoryGetBuffer(Buffer, NULL), NumBytesTransferred);
}

BOOLEAN
EvtUsbReadersFailed(WDFUSBPIPE Pipe, NTSTATUS Status, USBD_STATUS UsbdStatus)
{
    UNREFERENCED_PARAMETER(UsbdStatus);

    StadiaRecordError(GetDeviceContext(WdfIoTargetGetDevice(WdfUsbTargetPipeGetIoTarget(Pipe))), Status);
    // Have the framework reset the pipe and restart the reader.
    return TRUE;
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

    WDF_USB_CONTINUOUS_READER_CONFIG_INIT(&readerConfig, EvtUsbInputReport, context, inputPacketSize);
    readerConfig.EvtUsbTargetPipeReadersFailed = EvtUsbReadersFailed;
    return WdfUsbTargetPipeConfigContinuousReader(context->InputPipe, &readerConfig);
}

static NTSTATUS
UsbStart(PDEVICE_CONTEXT Context)
{
    return WdfIoTargetStart(WdfUsbTargetPipeGetIoTarget(Context->InputPipe));
}

static VOID
UsbStop(PDEVICE_CONTEXT Context)
{
    WdfIoTargetStop(WdfUsbTargetPipeGetIoTarget(Context->InputPipe), WdfIoTargetCancelSentIo);
}

const TRANSPORT UsbTransport = {UsbPrepareHardware, UsbStart, UsbStop, UsbSendOutputReport};
