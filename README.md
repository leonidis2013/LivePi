# LivePi

Headless VST3 host for live voice on a Raspberry Pi, run entirely from a web page on your phone, tablet or computer. Built with JUCE. It runs your own Linux ARM64 VST3 plugins, with one chain per channel.

📖 **[Read the LivePi User Manual](docs/MANUAL.md)**: installing, the mixer, PAFL monitoring, plugin views, setup pages, password and trusted access, updating, latency tips and troubleshooting.

<img src="docs/images/mixer-phone.png" width="260" alt="LivePi on a phone"> <img src="docs/images/plugin-controls.png" width="260" alt="Plugin controls">

## Quick start

1. Flash **Raspberry Pi OS Lite (64-bit)** with [Raspberry Pi Imager](https://www.raspberrypi.com/software/). In its settings, set a hostname (e.g. `livepi`), a user and SSH. Fit your audio interface (e.g. a Blokas Pisound) and boot.
2. On the Pi:

   ```bash
   curl -L -o livepi.tar.gz https://github.com/leonidis2013/LivePi/releases/latest/download/livepi-linux-arm64.tar.gz
   tar xzf livepi.tar.gz && cd livepi
   sudo ./install-pi.sh your-user
   sudo reboot
   ```

   The installer asks about a Pisound, a password, and trusted access (see the [manual](docs/MANUAL.md#22-run-the-installer)).
3. Open **`http://livepi.local:8080`**. Then use **⚙ → Audio** to choose the device at 48 kHz with a buffer of 64, and **⚙ → Plugins** to upload your plugins (`.zip`, `.vst3` or `.so`). Add them to a channel on the mixer.

No audio interface yet? Choose **Virtual (no audio hardware)** under ⚙ → Audio and loop a test file as the input.

**Upgrading from LiveHost** (the old name)? Run the installer as above. It removes the old service and moves your settings, presets, plugins and password over.

## Requirements

- Raspberry Pi 5 or 4 (a Pi 3 works with less headroom), running Raspberry Pi OS Lite 64-bit: Debian 13 "Trixie" or Debian 12 "Bookworm". Both are tested automatically (`.github/workflows/compat.yml`).
- An ALSA-supported audio interface, or none for testing.
- VST3 plugins built for **Linux aarch64**.

## Releases and versions

Versions follow **major.minor.patch** and are set in `CMakeLists.txt` (`project(LivePi VERSION x.y.z)`). Every push to `main` is built and tested on an ARM64 runner. A release titled "LivePi vX.Y.Z" is published only when the version is new. Each release contains:

- `livepi-linux-arm64.tar.gz`: the fresh-install package.
- `livepi-build-N.zip`: the update package, used by ⚙ → System → Update from File.

Pis can also update online from ⚙ → System. If a new version fails to start three times, LivePi rolls back to the previous one by itself.

## Security notes

- Uploaded plugins are native code that runs on the Pi, so only upload plugins you trust.
- LivePi uses plain HTTP. Keep the Pi on a network you control and never expose it to the internet.
- Zip uploads are checked for path tricks and symbolic links before unpacking.
