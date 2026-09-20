# WinStadia

Makes a Google Stadia controller (with the Bluetooth firmware) show up as an Xbox controller on Windows, over USB and Bluetooth. It was built for miHoYo games, which ignore the controller or get its buttons wrong, but anything that speaks XInput benefits.

It is a UMDF driver. No kernel code, no test signing mode, no background process. The usual route to XInput is a virtual controller from a kernel bus driver, which individuals cannot sign, and ViGEmBus, the one everybody used, is no longer maintained.

## How it works

Xbox controllers on Bluetooth are HID gamepads, and the inbox filter driver xinputhid on top of their HID stack turns them into XInput devices. WinStadia is a HID transport minidriver at the bottom of the Stadia controller's HID stack. It presents the IDs, report descriptor and reports of an Xbox One S controller on Bluetooth, and its INF puts xinputhid on top, the way Microsoft's INF does for the real thing. Rumble travels the other way.

On USB it replaces hidusb and talks to the interrupt endpoints through WinUSB. On Bluetooth it sits on top of the inbox HID over GATT driver and reads input reports from it the way the HID class driver would.

| Stadia | Xbox |
|---|---|
| A, B, X, Y | same |
| L1, R1 | LB, RB |
| L2, R2 | LT, RT |
| stick clicks | same |
| Options (left) | View |
| Menu (right) | Menu |
| Stadia | Xbox button |
| Capture, Assistant | unmapped |

## Limits

- The report format xinputhid expects is undocumented. WinStadia copies a real controller, and a Windows update could break that.
- No rumble over Bluetooth, use the cable. The inbox HID over GATT driver rejects the controller's output report as an invalid parameter, with or without WinStadia.
- Everything sees an Xbox controller, including Steam Input, browsers and Google's web tool for switching firmware. Uninstall before using that.
- Capture and Assistant have no counterpart in XInput.
- Games see an ordinary Xbox controller, with no kernel code or injection involved. What anti-cheat makes of it is still not guaranteed.
- 64-bit Windows 11 only.

## Install

Download `WinStadia.zip` from the [releases](https://github.com/aisk/WinStadia/releases), unpack it and run the installer.

```powershell
powershell -ExecutionPolicy Bypass -File install.ps1
```

It elevates itself and needs no reboot. The package is signed during the install, with a certificate made on the spot that only this machine trusts and whose private key is deleted right after. If something holds the controller open, the driver cannot take over. Replug the controller then, or over Bluetooth switch Bluetooth off and on.

`install.ps1 -Uninstall` brings back the inbox drivers and removes the certificate.

Releases are built by GitHub Actions, which `gh attestation verify WinStadia.zip --repo aisk/WinStadia` confirms.

## Build

Needs the Visual Studio C++ build tools and Windows SDK 10.0.26100. The build script fetches the NuGet WDK (about 110 MB) into `.wdk` on first run.

```powershell
powershell -ExecutionPolicy Bypass -File driver\build.ps1
powershell -ExecutionPolicy Bypass -File driver\install.ps1
```

## Troubleshooting

`status.ps1` sits next to the installer, in `driver` in a checkout. It reads the driver's state through a vendor feature report. That covers whether input arrives, whether the last rumble write failed and the last NTSTATUS from the side facing the controller. With `-Watch` it prints the raw Stadia input reports. The controller has to be connected.

Rumble can only be tested through XInput, because xinputhid blocks HID output writes from above.

## License

GPLv3, see [LICENSE](LICENSE).
