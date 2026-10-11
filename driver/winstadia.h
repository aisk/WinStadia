// Shared between the HID side presenting an Xbox controller (winstadia.c), the
// Stadia protocol (stadia.c) and the transports that reach the controller
// (usb.c, bluetooth.c).

#pragma once

#include <windows.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>
#include "private-gatt.h"

// Controls of the presented pad: the payload of the gamepad input report,
// followed by the guide button, which travels in a report of its own.
#define PAD_GAMEPAD_LEN 15
#define PAD_GUIDE_INDEX PAD_GAMEPAD_LEN
#define PAD_CONTROLS_LEN (PAD_GAMEPAD_LEN + 1)

#define RAW_REPORT_MAX 16

// The Stadia controller's output report: the speeds of its two motors.
#define STADIA_RUMBLE_REPORT_ID 0x05

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
    // The report starts with its ID. May block; the caller holds RumbleLock.
    NTSTATUS (*SendOutputReport)(PDEVICE_CONTEXT Context, PUCHAR Report, size_t Length);
    // Runs the recovery from a failed input reader; NULL where there is none.
    NTSTATUS (*TestRecovery)(PDEVICE_CONTEXT Context);
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
    ULONG RumbleWrites;
    // Every input report from the transport, changed or not.
    ULONG InputReports;
    // USB input reader failures and what was done about them.
    USBD_STATUS LastUsbdStatus;
    ULONG ReaderFailures;
    ULONG PipeResets;
    ULONG DeviceRestarts;

    // Motor speeds: those asked for and those the controller has, both
    // guarded by the lock. The work item writes the difference, so that a
    // slow write holds up no request and the last value asked for wins.
    UCHAR RumbleStrong;
    UCHAR RumbleWeak;
    UCHAR RumbleSentStrong;
    UCHAR RumbleSentWeak;
    WDFWORKITEM RumbleWorkItem;
    // Serializes the writes, and with them the transport's SendOutputReport.
    SRWLOCK RumbleLock;

    // USB transport.
    WDFUSBDEVICE UsbDevice;
    WDFUSBPIPE InputPipe;
    // NULL when the interface has no interrupt OUT endpoint; output reports
    // then go through the control endpoint.
    WDFUSBPIPE OutputPipe;
    UCHAR InterfaceNumber;
    // Brings the input pipe back after the reader failed. Running is guarded
    // by the lock, the tick of the last recovery belongs to the work item.
    WDFWORKITEM RecoveryWorkItem;
    BOOLEAN UsbRunning;
    ULONGLONG LastRecoveryTick;

    // Bluetooth transport.
    WDFIOTARGET LowerDriver;
    BLUETOOTH_READER Readers[BLUETOOTH_READER_COUNT];
    WDFTIMER RetryTimer;
    // Guarded by the lock.
    BOOLEAN Reading;
    // Rumble goes around the driver below, through a session with bthserv.
    // Guarded by RumbleLock. The tick is that of the last attempt to open.
    UINT64 BluetoothAddress;
    PRIVATE_GATT_SESSION *GattSession;
    HRESULT GattOpenStatus;
    ULONGLONG GattOpenTick;
};

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// Publishes new control values as input reports, if they changed.
VOID PadPublishControls(PDEVICE_CONTEXT Context, const UCHAR *Controls);
// Publishes all controls released and centered, for when input stops
// arriving, so that games do not keep the last buttons held.
VOID PadReleaseControls(PDEVICE_CONTEXT Context);

EVT_WDF_DEVICE_PREPARE_HARDWARE StadiaPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY StadiaD0Entry;
EVT_WDF_DEVICE_D0_EXIT StadiaD0Exit;

// Takes an input report from the transport.
VOID StadiaInputReport(PDEVICE_CONTEXT Context, const UCHAR *Report, size_t Length);
VOID StadiaSetRumble(PDEVICE_CONTEXT Context, UCHAR Strong, UCHAR Weak);
EVT_WDF_WORKITEM StadiaRumbleWorkItem;
VOID StadiaRecordError(PDEVICE_CONTEXT Context, NTSTATUS Status);
