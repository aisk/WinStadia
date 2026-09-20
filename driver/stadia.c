// Stadia protocol: translates the controller's input reports to Xbox controls
// and rumble to its output report. The transports (usb.c, bluetooth.c) carry
// the reports.

#include "winstadia.h"

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

#define STADIA_B3_A 0x40
#define STADIA_B3_B 0x20
#define STADIA_B3_X 0x10
#define STADIA_B3_Y 0x08
#define STADIA_B3_L1 0x04
#define STADIA_B3_R1 0x02
#define STADIA_B3_LS 0x01

// Xbox controls, as laid out in PAD_CONTROLS_LEN:
//   [0..8] left X, left Y, right X, right Y: 16 bit little endian, centered
//          at 0x8000, Y grows downwards
//   [8..12] left, right trigger: 10 bit little endian
//   [12] d-pad hat: 1 = up, clockwise to 8 = up-left, 0 = released
//   [13] Menu, View, RB, LB, Y, X, B, A (bit 7..0)
//   [14] RS click, LS click (bit 1..0)
//   [15] Xbox button
#define XBOX_B13_MENU 0x80
#define XBOX_B13_VIEW 0x40
#define XBOX_B13_RB 0x20
#define XBOX_B13_LB 0x10
#define XBOX_B13_Y 0x08
#define XBOX_B13_X 0x04
#define XBOX_B13_B 0x02
#define XBOX_B13_A 0x01

#define XBOX_B14_RS 0x02
#define XBOX_B14_LS 0x01

#define BIT(source, mask, target) (((source) & (mask)) ? (target) : 0)

static VOID
PutUshort(UCHAR *Target, ULONG Value)
{
    Target[0] = (UCHAR)Value;
    Target[1] = (UCHAR)(Value >> 8);
}

// The buttons carry the same letters in the same places. Options and Menu
// become View and Menu, Stadia the Xbox button. Capture and Assistant have no
// counterpart.
static VOID
MapControls(const UCHAR *Stadia, UCHAR *Controls)
{
    UCHAR b2 = Stadia[2];
    UCHAR b3 = Stadia[3];

    for (int i = 0; i < 4; i++) {
        // 1..255 centered at 128 becomes 2..65534 centered at 0x8000.
        PutUshort(Controls + 2 * i, (ULONG)(0x8000 + (max(Stadia[4 + i], 1) - 0x80) * 258));
    }
    for (int i = 0; i < 2; i++) {
        // 0..255 becomes 0..1023.
        PutUshort(Controls + 8 + 2 * i, (ULONG)(Stadia[8 + i] * 4 + (Stadia[8 + i] >> 6)));
    }
    Controls[12] = Stadia[1] < STADIA_HAT_RELEASED ? Stadia[1] + 1 : 0;
    Controls[13] = (UCHAR)(BIT(b2, STADIA_B2_MENU, XBOX_B13_MENU) |
                           BIT(b2, STADIA_B2_OPTIONS, XBOX_B13_VIEW) |
                           BIT(b3, STADIA_B3_R1, XBOX_B13_RB) |
                           BIT(b3, STADIA_B3_L1, XBOX_B13_LB) |
                           BIT(b3, STADIA_B3_Y, XBOX_B13_Y) |
                           BIT(b3, STADIA_B3_X, XBOX_B13_X) |
                           BIT(b3, STADIA_B3_B, XBOX_B13_B) |
                           BIT(b3, STADIA_B3_A, XBOX_B13_A));
    Controls[14] = (UCHAR)(BIT(b2, STADIA_B2_RS, XBOX_B14_RS) | BIT(b3, STADIA_B3_LS, XBOX_B14_LS));
    Controls[PAD_GUIDE_INDEX] = BIT(b2, STADIA_B2_STADIA, 1);
}

VOID
StadiaRecordError(PDEVICE_CONTEXT Context, NTSTATUS Status)
{
    AcquireSRWLockExclusive(&Context->Lock);
    Context->LastError = Status;
    ReleaseSRWLockExclusive(&Context->Lock);
}

VOID
StadiaInputReport(PDEVICE_CONTEXT Context, const UCHAR *Report, size_t Length)
{
    UCHAR controls[PAD_CONTROLS_LEN];

    AcquireSRWLockExclusive(&Context->Lock);
    Context->RawReportLength = (UCHAR)min(Length, RAW_REPORT_MAX);
    RtlCopyMemory(Context->RawReport, Report, Context->RawReportLength);
    ReleaseSRWLockExclusive(&Context->Lock);

    if (Length >= STADIA_INPUT_REPORT_LEN && Report[0] == STADIA_INPUT_REPORT_ID) {
        MapControls(Report, controls);
        PadPublishControls(Context, controls);
    }
}

VOID
StadiaSetRumble(PDEVICE_CONTEXT Context, UCHAR Strong, UCHAR Weak)
{
    // Motor speeds are 16 bit little endian; x * 257 scales 8 to 16 bits.
    UCHAR report[] = {STADIA_RUMBLE_REPORT_ID, Strong, Strong, Weak, Weak};
    NTSTATUS status;

    // Games repeat the same output report a lot; only changes reach the wire.
    AcquireSRWLockExclusive(&Context->RumbleLock);
    if (Strong == Context->RumbleStrong && Weak == Context->RumbleWeak) {
        ReleaseSRWLockExclusive(&Context->RumbleLock);
        return;
    }
    status = Context->Transport->SendOutputReport(Context, report, sizeof(report));
    if (NT_SUCCESS(status)) {
        Context->RumbleStrong = Strong;
        Context->RumbleWeak = Weak;
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
    UNREFERENCED_PARAMETER(ResourcesRaw);
    UNREFERENCED_PARAMETER(ResourcesTranslated);

    return GetDeviceContext(Device)->Transport->PrepareHardware(Device);
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

    return context->Transport->Start(context);
}

NTSTATUS
StadiaD0Exit(WDFDEVICE Device, WDF_POWER_DEVICE_STATE TargetState)
{
    PDEVICE_CONTEXT context = GetDeviceContext(Device);

    UNREFERENCED_PARAMETER(TargetState);

    // Fails harmlessly when the controller is already gone.
    StadiaSetRumble(context, 0, 0);
    context->Transport->Stop(context);
    return STATUS_SUCCESS;
}
