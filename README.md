# winstadia

Makes a Google Stadia controller (with the Bluetooth firmware) show up as an Xbox controller on Windows, over USB and over Bluetooth. It was built for miHoYo games, but since XInput is what PC games expect, pretty much everything benefits. Nothing runs in the background.

It is a user-mode driver. There is no kernel code of its own and no need for test signing mode.

## Why

With the Bluetooth firmware the controller is a plain HID gamepad. Windows accepts it, many games do not. Zenless Zone Zero ignores it and Genshin Impact gets the buttons wrong, because they only know Xbox and PlayStation controllers.

The usual way to get XInput is a virtual Xbox controller from a kernel bus driver. Individuals cannot sign kernel drivers, and ViGEmBus, the one everybody used, is no longer maintained.

## How it works

Windows already knows how to turn a HID gamepad into an XInput device, because that is what Xbox controllers are over Bluetooth. An inbox filter driver called xinputhid sits on top of their HID stack and does the translation.

winstadia sits at the bottom of the controller's HID stack. It takes in the Stadia input reports, and towards the HID stack above it looks like an Xbox One S controller on Bluetooth, with the same IDs, report descriptor and report format. Its INF puts xinputhid on top, the same way Microsoft's own INF does for the real thing. From there on Windows treats it like any Xbox controller. Rumble travels the other way.

How the Stadia reports reach the driver depends on the connection. On USB it takes the place of the generic USB HID driver and talks to the endpoints itself, with the inbox WinUSB driver below it carrying the transfers. On Bluetooth the Windows driver for Bluetooth LE HID devices stays in place. winstadia sits right on top of it and asks it for input reports the way the HID class driver normally would, so it does not have to speak Bluetooth itself.

The driver runs in the system's user-mode driver host. A crash there cannot bluescreen the machine, and Windows restarts the host.

The face buttons carry the same letters in the same places on both controllers, so button prompts in games are right.

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

## Trade-offs

**It leans on undocumented behavior.** xinputhid is meant for Microsoft's own controllers and nothing documents the reports it expects. winstadia copies what a real controller sends. A Windows update could change the rules.

**No rumble over Bluetooth.** Everything else works there, rumble needs the cable. The controller declares its rumble output report over Bluetooth too and the HID class driver accepts it, but the Windows Bluetooth LE HID driver below refuses to write it and fails the request as an invalid parameter. That happens with the plain Windows stack as well, and SDL lists the same limitation. Why that driver refuses is not known. A likely place to look is how the controller describes the report's characteristic in its GATT table. Apps cannot go around the driver, because Windows keeps the HID service of a Bluetooth LE device to itself. A driver on that service is not locked out, so a possible way forward is to have winstadia find the characteristic of the output report and write to it through the Bluetooth GATT API, leaving input with the Windows driver. That is untested.

**The Stadia controller disappears for everything else.** Steam Input, the browser Gamepad API and any software with native Stadia support will see an Xbox controller instead. That includes Google's web tool for switching firmware, so uninstall first if you ever need it.

**Two buttons are lost.** XInput has no place for Capture and Assistant.

**You trust a self-signed certificate.** The build script generates a code signing certificate on your machine and the installer adds it to the machine's trusted roots. Whoever gets its private key, which stays in your user certificate store, can sign code this machine trusts. That is why no prebuilt drivers are distributed. Uninstalling removes the trust, and you can delete the certificate from your personal store as well.

**Anti-cheat.** There is no kernel code, no injection and no modified game files. Games see what they would see with a real Xbox controller. That is an observation and not a guarantee.

**Limits.** 64-bit Windows 11 only.

## Build and install

You need the Visual Studio C++ build tools and Windows SDK 10.0.26100. Rust is only needed for the diagnostic tool. The WDK is not required, the build script downloads the NuGet WDK (about 110 MB) into `.wdk` on first run.

```powershell
powershell -ExecutionPolicy Bypass -File driver\build.ps1
powershell -ExecutionPolicy Bypass -File driver\install.ps1
```

The installer elevates itself and no reboot is needed. A connected controller switches over right away, unless a game or some other program is holding it open. The installer says so, and reconnecting the controller finishes the job. A controller that gets plugged in or paired later picks the driver up by itself.

`driver\install.ps1 -Uninstall` hands the controller back to the Windows HID driver and removes the certificate trust.

## Troubleshooting

The driver keeps no log. A small tool asks it instead. It needs the controller connected, because without it there is no driver instance to ask.

```powershell
cargo run --release             # what is the driver doing
cargo run --release -- dump     # print raw Stadia input reports
```

The status tells whether input reports arrive from the controller, whether the last rumble write failed, and the last error code the side facing the controller ran into. To make it rumble, use any game or gamepad tester. Windows only lets rumble through by way of XInput.

## License

GPLv3, see [LICENSE](LICENSE).
