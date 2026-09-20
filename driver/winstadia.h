// Shared between the HID side presenting an Xbox controller (winstadia.c), the
// Stadia protocol (stadia.c) and the transports that reach the controller
// (usb.c, bluetooth.c).

#pragma once

#include <windows.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>

// Controls of the presented pad: the payload of the gamepad input report,
// followed by the guide button, which travels in a report of its own.
#define PAD_GAMEPAD_LEN 15
#define PAD_GUIDE_INDEX PAD_GAMEPAD_LEN
#define PAD_CONTROLS_LEN (PAD_GAMEPAD_LEN + 1)

#define RAW_REPORT_MAX 16

// Input reports the Bluetooth transport keeps pending below, like the HID
// class driver does.
#define BLUETOOTH_READER_COUNT 2

typedef struct _DEVICE_CONTEXT DEVICE_CONTEXT, *PDEVICE_CONTEXT;

// How Stadia reports travel to and from the controller. Start and Stop follow
// the power state; input reports go to StadiaInputReport.
typedef struct _TRANSPORT {
    NTSTATUS (*PrepareHardware)(WDFDEVICE Device);
    NTSTATUS (*Start)(PDEVICE_CONTEXT Context);
    VOID (*Stop)(PDEVICE_CONTEXT Context);
    // The report starts with its ID.
    NTSTATUS (*SendOutputReport)(PDEVICE_CONTEXT Context, PUCHAR Report, size_t Length);
} TRANSPORT;

extern const TRANSPORT UsbTransport;
extern const TRANSPORT BluetoothTransport;

typedef struct _BLUETOOTH_READER {
    PDEVICE_CONTEXT Context;
    WDFREQUEST Request;
    WDFMEMORY Buffer;
    // Guarded by the context's lock.
    BOOLEAN Sent;
} BLUETOOTH_READER, *PBLUETOOTH_READER;

struct _DEVICE_CONTEXT {
    const TRANSPORT *Transport;

    // Guards the input and diagnostics state below.
    SRWLOCK Lock;

    WDFQUEUE ReadQueue;
    UCHAR Controls[PAD_CONTROLS_LEN];
    // Set while the report holds changes no read has picked up yet.
    BOOLEAN GamepadChanged;
    BOOLEAN GuideChanged;

    // Diagnostics, reported through the status feature report.
    UCHAR RawReport[RAW_REPORT_MAX];
    UCHAR RawReportLength;
    BOOLEAN RumbleFailed;
    NTSTATUS LastError;

    // Serializes rumble writes so that the last value written wins.
    SRWLOCK RumbleLock;
    UCHAR RumbleStrong;
    UCHAR RumbleWeak;

    // USB transport.
    WDFUSBDEVICE UsbDevice;
    WDFUSBPIPE InputPipe;
    // NULL when the interface has no interrupt OUT endpoint; output reports
    // then go through the control endpoint.
    WDFUSBPIPE OutputPipe;
    UCHAR InterfaceNumber;

    // Bluetooth transport.
    WDFIOTARGET LowerDriver;
    BLUETOOTH_READER Readers[BLUETOOTH_READER_COUNT];
    WDFTIMER RetryTimer;
    // Guarded by the lock.
    BOOLEAN Reading;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// Publishes new control values as input reports, if they changed.
VOID PadPublishControls(PDEVICE_CONTEXT Context, const UCHAR *Controls);

EVT_WDF_DEVICE_PREPARE_HARDWARE StadiaPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY StadiaD0Entry;
EVT_WDF_DEVICE_D0_EXIT StadiaD0Exit;

// Takes an input report from the transport.
VOID StadiaInputReport(PDEVICE_CONTEXT Context, const UCHAR *Report, size_t Length);
VOID StadiaSetRumble(PDEVICE_CONTEXT Context, UCHAR Strong, UCHAR Weak);
VOID StadiaRecordError(PDEVICE_CONTEXT Context, NTSTATUS Status);
