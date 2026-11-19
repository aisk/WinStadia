# Bluetooth reconnects: separate input from private GATT output

This is a diagnostic procedure, not a claim that a particular Windows build
is supported. The Bluetooth rumble implementation uses an undocumented ABI.
An OS build number alone neither proves nor disproves ABI compatibility.

## Reported baseline

A v0.2.0 installation on Windows 11 x64, build 26300.9457, with an Intel AX200
(adapter driver 24.10.0.4) produced repeated Xbox connect/disconnect notifications.
The same Stadia controller supported Bluetooth rumble on macOS. On the Windows
inbox stack, HID output report 5 failed with Win32 error 87, as described in
`driver/bluetooth.c`.

After installing WinStadia:

- The installer returned 0.
- `status.ps1` initially reported a running driver with no input received.
- One `XInputSetState` returned 0; the stop request about 500 ms later returned
  1167 (device not connected). Subsequent attempts found no XInput controller.
- No successful physical rumble was confirmed.
- The DriverFrameworks-UserMode Operational log was disabled; no driver crash
  or failing private interface slot has been established.
- Uninstallation reported that a restart was required. Kernel-PnP events also
  identified applications holding the device open during removal.

Relevant file versions on that system were:

| Module | File version |
| --- | --- |
| Microsoft.Bluetooth.Service.dll | 10.0.26100.9444 |
| Microsoft.Bluetooth.Profiles.HidOverGatt.dll | 10.0.26100.1 |
| xinputhid.sys | 10.0.26100.9278 |

These observations do **not** establish that build 26300 changed a vtable slot,
or that opening GATT in D0 entry caused the reconnects.

## Controlled comparison

1. Finish any pending uninstall and restart Windows if requested. Check that
   the inbox driver gives stable input before installing another build.
2. Exit games, Steam, controller mappers and browser gamepad pages. Other
   applications, including Game Bar, may still hold the device open. Follow
   the installer's reconnect/restart instructions if necessary.
3. In an elevated PowerShell, enable the diagnostic channel before reproducing:

   ```powershell
   wevtutil sl Microsoft-Windows-DriverFrameworks-UserMode/Operational /e:true
   $start = Get-Date
   ```

   Record whether the channel was already enabled. Restore its previous state
   after collecting the logs if desired.
4. Install the candidate and reconnect the controller. Run `status.ps1 -Watch`
   and move the sticks for at least a minute, without requesting rumble.
   Record whether input flows and whether connect/disconnect notifications recur.
   The candidate does not initialize private GATT from D0 entry; a nonzero
   rumble request is needed to exercise that path. Applications can request
   rumble themselves, so an input-only test must avoid those applications.
5. Only after input is stable, send a bounded XInput rumble request followed
   by a zero request in a `finally` block. Record both return codes, physical
   vibration and the status report. If the device disappears, a zero request
   cannot be assumed to have reached it; switch the controller off if necessary.
6. Compare idle input, first rumble, reconnect, and sleep/resume against v0.2.0.
   The intended change is that private GATT initialization no longer delays
   D0 entry; the first rumble may take longer. USB is unchanged.
7. Inspect/export the relevant time range from System, Application and
   DriverFrameworks-UserMode logs. For example:

   ```powershell
   Get-WinEvent -FilterHashtable @{
       LogName = 'Microsoft-Windows-DriverFrameworks-UserMode/Operational'
       StartTime = $start
   } | Select-Object TimeCreated, Id, ProviderName, Message
   ```

   Record exact OS/driver/module versions and installer exit codes. Redact
   Bluetooth addresses, device instance IDs, account names and unrelated
   application details before publishing logs.

If input is stable until the first rumble, investigate the private GATT path
next. If it reconnects before any GATT request, investigate the input transport,
PnP/power lifecycle and driver host instead. Neither outcome alone identifies
an ABI slot change. Do not guess new vtable indices or mark a build compatible
without a live test.
