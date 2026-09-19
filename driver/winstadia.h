// Shared between the HID minidriver (winstadia.c) and the bridge to the
// physical controller (bridge.c).

#pragma once

#include <windows.h>
#include <cfgmgr32.h>
#include <wdf.h>

#define INPUT_REPORT_LEN 64

// Bytes [1..10) of the DS4 input report: sticks, hat and buttons, triggers.
// The report counter bits in byte [7] are left clear.
#define PAD_CONTROLS_LEN 9

// Bridge states, reported through the status feature report.
#define BRIDGE_SEARCHING 0
#define BRIDGE_CONNECTED 1
#define BRIDGE_OPEN_FAILED 2

typedef struct _DEVICE_CONTEXT {
    // Guards everything below that both the I/O queue callbacks and the
    // bridge thread touch.
    SRWLOCK Lock;

    WDFQUEUE ReadQueue;
    UCHAR InputReport[INPUT_REPORT_LEN];
    UCHAR Controls[PAD_CONTROLS_LEN];
    UCHAR ReportCounter;
    BOOLEAN InputChanged;

    // Rumble requested by the game, picked up by the bridge thread.
    UCHAR RumbleStrong;
    UCHAR RumbleWeak;

    UCHAR BridgeState;
    BOOLEAN RumbleFailed;
    ULONG BridgeError;

    // Bridge thread plumbing, owned by bridge.c.
    HANDLE Thread;
    HANDLE StopEvent;
    HANDLE ScanEvent;
    HANDLE RumbleEvent;
    HCMNOTIFICATION Notification;
} DEVICE_CONTEXT, *PDEVICE_CONTEXT;

WDF_DECLARE_CONTEXT_TYPE_WITH_NAME(DEVICE_CONTEXT, GetDeviceContext)

extern const UCHAR IdleControls[PAD_CONTROLS_LEN];

// Publishes new control values as a DS4 input report, if they changed.
VOID PadPublishControls(PDEVICE_CONTEXT Context, const UCHAR *Controls);

NTSTATUS BridgeStart(PDEVICE_CONTEXT Context);
VOID BridgeStop(PDEVICE_CONTEXT Context);
