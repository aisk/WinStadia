# winstadia

Makes a Google Stadia controller (with the Bluetooth firmware) usable in Windows games by presenting it as a DualShock 4. It was built for miHoYo games, but anything that supports a DS4 benefits. Once installed it works whenever the controller is plugged in over USB, with nothing running in the background.

It is a single user-mode driver. There is no kernel code of its own and no need for test signing mode.

## Why

With the Bluetooth firmware the controller is a plain HID gamepad. Windows accepts it, many games do not. Zenless Zone Zero ignores it and Genshin Impact gets the buttons wrong, because they only know Xbox and PlayStation controllers.

The usual fix is to emulate an Xbox controller, but an XInput device can only be created by a kernel driver. Individuals cannot sign those, and ViGEmBus is no longer maintained. A DualShock 4 is an ordinary HID device that a user-mode driver can present, and these games support it natively, rumble included.

## How it works

Windows normally puts its generic USB HID driver on the controller. winstadia takes that place. It reads the Stadia input reports from the USB endpoint itself, and when the HID stack above asks what kind of device this is, it answers with the descriptor and IDs of a DualShock 4. Rumble travels the other way. Sticks and d-pad are encoded the same way on both controllers, so translating is mostly moving button bits around.

Because the controller itself becomes the DS4, there is no second virtual device. Nothing needs to be hidden, no press registers twice, and the DS4 exists exactly as long as the controller is plugged in.

The driver runs in the system's user-mode driver host, with the inbox WinUSB driver below it carrying the transfers. A crash there cannot bluescreen the machine, and Windows restarts the host.

Buttons map by position, not by letter.

| Stadia | DualShock 4 |
|---|---|
| A, B, X, Y | Cross, Circle, Square, Triangle |
| L1, R1, L2, R2, stick clicks | same |
| Options (left) | Share |
| Menu (right) | Options |
| Stadia | PS |
| Capture | touchpad click |
| Assistant | unmapped |

## Trade-offs

**USB only.** Over Bluetooth the driver is not involved and the controller stays the plain gamepad it was. Doing the same there means replacing the Windows Bluetooth HID driver, which is a much bigger job.

**The Stadia controller disappears for everything else.** Steam Input, the browser Gamepad API and any software with native Stadia support will see a DS4 instead. That includes Google's web tool for switching firmware, so uninstall first if you ever need it.

**Button prompts show PlayStation symbols.** Positions match, glyphs do not.

**The device path still says Google.** The HID stack reports Sony IDs, which is what games and SDL ask for. Software that parses the vendor ID out of the device path instead will not be fooled.

**You trust a self-signed certificate.** The build script generates a code signing certificate on your machine and the installer adds it to the machine's trusted roots. Whoever gets its private key, which stays in your user certificate store, can sign code this machine trusts. That is why no prebuilt drivers are distributed. Uninstalling removes the trust, and you can delete the certificate from your personal store as well.

**Anti-cheat.** There is no kernel code, no injection and no modified game files. Games just see a HID gamepad. It works with Genshin Impact and Zenless Zone Zero, which is an observation and not a guarantee.

**Limits.** No gyro or touchpad data. 64-bit Windows 11 only.

## Build and install

You need the Visual Studio C++ build tools and Windows SDK 10.0.26100. Rust is only needed for the diagnostic tool. The WDK is not required, the build script downloads the NuGet WDK (about 110 MB) into `.wdk` on first run.

```powershell
powershell -ExecutionPolicy Bypass -File driver\build.ps1
powershell -ExecutionPolicy Bypass -File driver\install.ps1
```

The installer elevates itself and no reboot is needed. A connected controller switches over right away.

`driver\install.ps1 -Uninstall` hands the controller back to the Windows HID driver and removes the certificate trust.

## Troubleshooting

The driver keeps no log. A small tool asks it instead. It needs the controller plugged in, because without it there is no driver instance to ask.

```powershell
cargo run --release             # what is the driver doing
cargo run --release -- dump     # print raw Stadia input reports
cargo run --release -- rumble   # pulse the motors for half a second
```

The status tells whether input reports arrive from the controller, whether the last rumble write failed, and the last error code the USB side ran into.

## License

GPLv3, see [LICENSE](LICENSE).
