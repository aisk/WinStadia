// Shared between the HID side presenting an Xbox controller (winstadia.c) and the
// USB side talking to the Stadia controller (stadia.c).

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

typedef struct _DEVICE_CONTEXT {
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

    WDFUSBDEVICE UsbDevice;
    WDFUSBPIPE InputPipe;
    // NULL when the interface has no interrupt OUT endpoint; output reports
    // then go through the control endpoint.
    WDFUSBPIPE OutputPipe;
    UCHAR InterfaceNumber;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

// Publishes new control values as input reports, if they changed.
VOID PadPublishControls(PDEVICE_CONTEXT Context, const UCHAR *Controls);

EVT_WDF_DEVICE_PREPARE_HARDWARE StadiaPrepareHardware;
EVT_WDF_DEVICE_D0_ENTRY StadiaD0Entry;
EVT_WDF_DEVICE_D0_EXIT StadiaD0Exit;

VOID StadiaSetRumble(PDEVICE_CONTEXT Context, UCHAR Strong, UCHAR Weak);
