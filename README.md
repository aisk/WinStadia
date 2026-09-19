# winstadia

Makes a Google Stadia controller (with the Bluetooth firmware) usable in Windows games by presenting it as a DualShock 4. It was built for miHoYo games, but anything that supports a DS4 benefits. Once installed it works over USB or Bluetooth with nothing running in the background.

It consists of two user-mode drivers. There is no kernel code of its own and no need for test signing mode.

## Why

With the Bluetooth firmware the controller is a plain HID gamepad. Windows accepts it, many games do not. Zenless Zone Zero ignores it and Genshin Impact gets the buttons wrong, because they only know Xbox and PlayStation controllers.

The usual fix is to emulate an Xbox controller, but an XInput device can only be created by a kernel driver. Individuals cannot sign those, and ViGEmBus is no longer maintained. A DualShock 4 is an ordinary HID device that a user-mode driver can create, and these games support it natively, rumble included.

## How it works

The first driver creates a virtual DualShock 4. A thread inside it waits for the Stadia controller, reads its input and republishes it in DS4 format. Rumble travels the other way. Sticks and d-pad are encoded the same way on both controllers, so translating is mostly moving button bits around.

On its own that would make games see two controllers and register every press twice. So the second driver sits on the real Stadia controller and rejects every attempt to open it, unless the device path ends with a specific suffix. The Windows HID stack ignores that suffix, which makes it a password only the first driver knows. Unlike a process whitelist this needs no configuration, and it also stops RawInput, which opens devices from the kernel.

Both drivers run in the system's user-mode driver host. A crash there cannot bluescreen the machine, and Windows restarts the host.

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

**The Stadia controller disappears for everything else.** Steam Input, the browser Gamepad API and any software with native Stadia support will see a DS4 instead. That is usually fine, but think twice if you rely on Stadia-specific support somewhere.

**Button prompts show PlayStation symbols.** Positions match, glyphs do not.

**No rumble over Bluetooth.** The Windows Bluetooth HID driver rejects output reports to this controller. SDL documents the same limitation, and the same firmware rumbles fine on Linux and macOS. Rumble works over USB.

**The virtual DS4 is always present.** With no Stadia controller connected, some games will still show a controller that does nothing.

**You trust a self-signed certificate.** The build script generates a code signing certificate on your machine and the installer adds it to the machine's trusted roots. Whoever gets its private key, which stays in your user certificate store, can sign code this machine trusts. That is why no prebuilt drivers are distributed. Uninstalling removes the trust, and you can delete the certificate from your personal store as well.

**Anti-cheat.** There is no kernel code, no injection and no modified game files. Games just see a HID gamepad. It works with Genshin Impact and Zenless Zone Zero, which is an observation and not a guarantee.

**Limits.** One controller at a time. No gyro or touchpad data. 64-bit Windows 11 only.

## Build and install

You need the Visual Studio C++ build tools and Windows SDK 10.0.26100. Rust is only needed for the diagnostic tool. The WDK is not required, the build script downloads the NuGet WDK (about 110 MB) into `.wdk` on first run.

```powershell
powershell -ExecutionPolicy Bypass -File driver\build.ps1
powershell -ExecutionPolicy Bypass -File driver\install.ps1
```

The installer elevates itself and no reboot is needed. If a button still triggers two actions afterwards, reconnect the controller once.

`driver\install.ps1 -Uninstall` removes both drivers, the virtual device and the certificate trust.

## Troubleshooting

The drivers keep no log. A small tool asks them instead.

```powershell
cargo run --release             # what is the driver doing
cargo run --release -- dump     # print raw Stadia input reports
cargo run --release -- rumble   # pulse the motors for half a second
```

The status is one of three. No controller found, controller found but cannot be opened (with the error code), or connected. A failed rumble write is reported too, which is expected over Bluetooth.

## License

GPLv3, see [LICENSE](LICENSE).
