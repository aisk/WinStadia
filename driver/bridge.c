// Bridge between the physical Stadia controller and the virtual DualShock 4.
//
// A worker thread finds the controller (USB or Bluetooth LE), reads its input
// reports, translates them to DS4 controls and forwards rumble the other way.

#include "winstadia.h"
#include <hidsdi.h>
#include <stdlib.h>

// Opens carrying this suffix get past the hide filter (hide.c).
static const WCHAR OpenSuffix[] = L"\\winstadia";

// Hardware ID fragments in the lowercased interface path: USB, Bluetooth LE.
static const PCWSTR PathMarkers[] = {L"vid_18d1&pid_9400", L"vid&0218d1_pid&9400"};

#define RETRY_INTERVAL_MS 1000
#define WRITE_TIMEOUT_MS 500

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
SetBridgeState(PDEVICE_CONTEXT Context, UCHAR State, ULONG Error)
{
    AcquireSRWLockExclusive(&Context->Lock);
    Context->BridgeState = State;
    Context->BridgeError = Error;
    if (State == BRIDGE_CONNECTED) {
        Context->RumbleFailed = FALSE;
    }
    ReleaseSRWLockExclusive(&Context->Lock);
}

// Writes the open path of the first connected controller to Path.
static BOOLEAN
FindController(PWSTR Path, size_t PathLength)
{
    GUID hidGuid;
    PWSTR list = NULL;
    ULONG length = 0;
    BOOLEAN found = FALSE;
    CONFIGRET result;

    HidD_GetHidGuid(&hidGuid);
    // The list can grow between the two calls, hence the retry.
    do {
        free(list);
        list = NULL;
        result = CM_Get_Device_Interface_List_SizeW(&length, &hidGuid, NULL, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
        if (result != CR_SUCCESS) {
            return FALSE;
        }
        list = calloc(length, sizeof(WCHAR));
        if (list == NULL) {
            return FALSE;
        }
        result = CM_Get_Device_Interface_ListW(&hidGuid, NULL, list, length, CM_GET_DEVICE_INTERFACE_LIST_PRESENT);
    } while (result == CR_BUFFER_SMALL);

    for (PWSTR entry = list; result == CR_SUCCESS && *entry != L'\0'; entry += wcslen(entry) + 1) {
        if (wcslen(entry) + ARRAYSIZE(OpenSuffix) > PathLength) {
            continue;
        }
        wcscpy_s(Path, PathLength, entry);
        _wcslwr_s(Path, PathLength);
        for (size_t i = 0; i < ARRAYSIZE(PathMarkers) && !found; i++) {
            found = wcsstr(Path, PathMarkers[i]) != NULL;
        }
        if (found) {
            wcscat_s(Path, PathLength, OpenSuffix);
            break;
        }
    }
    free(list);
    return found;
}

// Completes an overlapped operation, cancelling it after the timeout.
static BOOL
FinishOverlapped(HANDLE Device, LPOVERLAPPED Overlapped, DWORD TimeoutMs)
{
    DWORD transferred;

    if (WaitForSingleObject(Overlapped->hEvent, TimeoutMs) != WAIT_OBJECT_0) {
        CancelIoEx(Device, Overlapped);
    }
    return GetOverlappedResult(Device, Overlapped, &transferred, TRUE);
}

static BOOL
SendRumble(HANDLE Device, HANDLE WriteEvent, UCHAR Strong, UCHAR Weak)
{
    // Motor speeds are 16 bit little endian; x * 257 scales 8 to 16 bits.
    UCHAR report[] = {STADIA_RUMBLE_REPORT_ID, Strong, Strong, Weak, Weak};
    OVERLAPPED overlapped = {.hEvent = WriteEvent};

    if (WriteFile(Device, report, sizeof(report), NULL, &overlapped) ||
        (GetLastError() == ERROR_IO_PENDING && FinishOverlapped(Device, &overlapped, WRITE_TIMEOUT_MS))) {
        return TRUE;
    }
    return HidD_SetOutputReport(Device, report, sizeof(report));
}

// Returns FALSE when the thread is asked to stop.
static BOOLEAN
RunSession(PDEVICE_CONTEXT Context, PCWSTR Path)
{
    UCHAR report[64];
    UCHAR controls[PAD_CONTROLS_LEN];
    OVERLAPPED read = {0};
    HANDLE writeEvent = NULL;
    BOOLEAN rumbleWorks = TRUE;
    BOOLEAN keepRunning = TRUE;
    BOOLEAN reading = FALSE;
    DWORD length;

    HANDLE device = CreateFileW(Path, GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, NULL,
                                OPEN_EXISTING, FILE_FLAG_OVERLAPPED, NULL);
    if (device == INVALID_HANDLE_VALUE) {
        SetBridgeState(Context, BRIDGE_OPEN_FAILED, GetLastError());
        return TRUE;
    }
    read.hEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    writeEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    if (read.hEvent == NULL || writeEvent == NULL) {
        SetBridgeState(Context, BRIDGE_OPEN_FAILED, GetLastError());
        goto cleanup;
    }
    // Rumble requested while no controller was connected is stale by now.
    ResetEvent(Context->RumbleEvent);
    SetBridgeState(Context, BRIDGE_CONNECTED, ERROR_SUCCESS);

    for (;;) {
        HANDLE events[] = {Context->StopEvent, Context->RumbleEvent, read.hEvent};
        DWORD signaled;

        if (!reading) {
            if (!ReadFile(device, report, sizeof(report), NULL, &read) && GetLastError() != ERROR_IO_PENDING) {
                break;
            }
            reading = TRUE;
        }

        signaled = WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, INFINITE);
        if (signaled == WAIT_OBJECT_0) {
            keepRunning = FALSE;
            break;
        }
        if (signaled == WAIT_OBJECT_0 + 1) {
            UCHAR strong, weak;

            AcquireSRWLockShared(&Context->Lock);
            strong = Context->RumbleStrong;
            weak = Context->RumbleWeak;
            ReleaseSRWLockShared(&Context->Lock);
            // Windows rejects output reports to this controller over
            // Bluetooth LE; stop trying once that shows.
            if (rumbleWorks && !SendRumble(device, writeEvent, strong, weak)) {
                rumbleWorks = FALSE;
                AcquireSRWLockExclusive(&Context->Lock);
                Context->RumbleFailed = TRUE;
                Context->BridgeError = GetLastError();
                ReleaseSRWLockExclusive(&Context->Lock);
            }
            continue;
        }

        reading = FALSE;
        if (!GetOverlappedResult(device, &read, &length, FALSE)) {
            break;
        }
        if (length >= STADIA_INPUT_REPORT_LEN && report[0] == STADIA_INPUT_REPORT_ID) {
            MapControls(report, controls);
            PadPublishControls(Context, controls);
        }
    }

    if (reading) {
        CancelIoEx(device, &read);
        GetOverlappedResult(device, &read, &length, TRUE);
    }
    if (rumbleWorks) {
        SendRumble(device, writeEvent, 0, 0);
    }
    PadPublishControls(Context, IdleControls);
    SetBridgeState(Context, BRIDGE_SEARCHING, ERROR_SUCCESS);

cleanup:
    if (read.hEvent != NULL) {
        CloseHandle(read.hEvent);
    }
    if (writeEvent != NULL) {
        CloseHandle(writeEvent);
    }
    CloseHandle(device);
    return keepRunning;
}

static DWORD WINAPI
BridgeThread(PVOID Parameter)
{
    PDEVICE_CONTEXT context = Parameter;
    HANDLE events[] = {context->StopEvent, context->ScanEvent};
    WCHAR path[512];

    for (;;) {
        BOOLEAN found = FindController(path, ARRAYSIZE(path));
        if (found && !RunSession(context, path)) {
            break;
        }
        // Without a controller, sleep until a HID interface arrives. After a
        // session the controller can linger in the list, so pace the retries.
        if (WaitForMultipleObjects(ARRAYSIZE(events), events, FALSE, found ? RETRY_INTERVAL_MS : INFINITE) ==
            WAIT_OBJECT_0) {
            break;
        }
    }
    return 0;
}

static DWORD CALLBACK
OnInterfaceChange(
    HCMNOTIFICATION Notification,
    PVOID Parameter,
    CM_NOTIFY_ACTION Action,
    PCM_NOTIFY_EVENT_DATA EventData,
    DWORD EventDataSize)
{
    PDEVICE_CONTEXT context = Parameter;

    UNREFERENCED_PARAMETER(Notification);
    UNREFERENCED_PARAMETER(EventData);
    UNREFERENCED_PARAMETER(EventDataSize);

    if (Action == CM_NOTIFY_ACTION_DEVICEINTERFACEARRIVAL) {
        SetEvent(context->ScanEvent);
    }
    return ERROR_SUCCESS;
}

NTSTATUS
BridgeStart(PDEVICE_CONTEXT Context)
{
    CM_NOTIFY_FILTER filter = {0};

    // Stop stays signaled for both loops to see it, the others auto-reset.
    Context->StopEvent = CreateEventW(NULL, TRUE, FALSE, NULL);
    Context->ScanEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    Context->RumbleEvent = CreateEventW(NULL, FALSE, FALSE, NULL);
    if (Context->StopEvent == NULL || Context->ScanEvent == NULL || Context->RumbleEvent == NULL) {
        BridgeStop(Context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }

    filter.cbSize = sizeof(filter);
    filter.FilterType = CM_NOTIFY_FILTER_TYPE_DEVICEINTERFACE;
    HidD_GetHidGuid(&filter.u.DeviceInterface.ClassGuid);
    if (CM_Register_Notification(&filter, Context, OnInterfaceChange, &Context->Notification) != CR_SUCCESS) {
        Context->Notification = NULL;
        BridgeStop(Context);
        return STATUS_UNSUCCESSFUL;
    }

    Context->Thread = CreateThread(NULL, 0, BridgeThread, Context, 0, NULL);
    if (Context->Thread == NULL) {
        BridgeStop(Context);
        return STATUS_INSUFFICIENT_RESOURCES;
    }
    return STATUS_SUCCESS;
}

VOID
BridgeStop(PDEVICE_CONTEXT Context)
{
    PHANDLE events[] = {&Context->StopEvent, &Context->ScanEvent, &Context->RumbleEvent};

    if (Context->Notification != NULL) {
        CM_Unregister_Notification(Context->Notification);
        Context->Notification = NULL;
    }
    if (Context->Thread != NULL) {
        SetEvent(Context->StopEvent);
        WaitForSingleObject(Context->Thread, INFINITE);
        CloseHandle(Context->Thread);
        Context->Thread = NULL;
    }
    for (size_t i = 0; i < ARRAYSIZE(events); i++) {
        if (*events[i] != NULL) {
            CloseHandle(*events[i]);
            *events[i] = NULL;
        }
    }
}
