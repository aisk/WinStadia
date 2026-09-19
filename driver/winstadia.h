// Shared between the HID side presenting a DualShock 4 (winstadia.c) and the
// USB side talking to the Stadia controller (stadia.c).

#pragma once

#include <windows.h>
#include <wdf.h>
#include <usb.h>
#include <wdfusb.h>

#define INPUT_REPORT_LEN 64

// Bytes [1..10) of the DS4 input report: sticks, hat and buttons, triggers.
// The report counter bits in byte [7] are left clear.
#define PAD_CONTROLS_LEN 9

#define RAW_REPORT_MAX 16

typedef struct _DEVICE_CONTEXT {
    // Guards the input and diagnostics state below.
    SRWLOCK Lock;

    WDFQUEUE ReadQueue;
    UCHAR InputReport[INPUT_REPORT_LEN];
    UCHAR Controls[PAD_CONTROLS_LEN];
    UCHAR ReportCounter;
    BOOLEAN InputChanged;

    // Diagnostics, reported through the status feature report.
    UCHAR RawReport[RAW_REPORT_MAX];
    UCHAR RawReportLength;
    BOOLEAN RumbleFailed;
    NTSTATUS LastError;

    // Serializes rumble writes so that the last value written wins.
    SRWLOCK RumbleLock;
    UCHAR RumbleStrong;
    UCHAR RumbleWeak;

    WDFUSBDEVICE UsbDevice;
    WDFUSBPIPE InputPipe;
    // NULL when the interface has no interrupt OUT endpoint; output reports
    // then go through the control endpoint.
    WDFUSBPIPE OutputPipe;
    UCHAR InterfaceNumber;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// Publishes new control values as a DS4 input report, if they changed.
VOID PadPublishControls(PDEVICE_CONTEXT Context, const UCHAR *Controls);

EVT_WDF_DEVICE_PREPARE_HARDWARE StadiaPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY StadiaD0Entry;
EVT_WDF_DEVICE_D0_EXIT StadiaD0Exit;

VOID StadiaSetRumble(PDEVICE_CONTEXT Context, UCHAR Strong, UCHAR Weak);
