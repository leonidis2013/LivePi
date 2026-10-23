# LivePi

**Upgrading from LiveHost** (the old name): download and run the installer as below. It stops and removes the old `livehost` service and moves your settings, presets, uploaded plugins, password and trusted networks from `~/.livehost` to `~/.livepi`.

Headless VST3 host for live voice on a Raspberry Pi, managed entirely from a web page.
Built with JUCE. Runs your own Linux ARM64 VST3 plugins, one chain per channel.

## Quick start

1. **Flash the SD card.** In [Raspberry Pi Imager](https://www.raspberrypi.com/software/) choose your Pi model and **Raspberry Pi OS Lite (64-bit)**. Before writing, open the OS settings and set a hostname (for example `livepi`), a username and password, turn on **SSH**, and enter Wi-Fi if you need it. A network cable is better for live use.
2. **Fit your audio interface** (HAT, USB card) and follow its own setup steps if it has any. Boot the Pi and wait about a minute.
3. **Connect from your computer** with `ssh your-user@livepi.local` and download and install LivePi:

   ```bash
   curl -L -o livepi.tar.gz https://github.com/leonidis2013/LivePi/releases/latest/download/livepi-linux-arm64.tar.gz
   tar xzf livepi.tar.gz
   cd livepi
   sudo ./install-pi.sh your-user
   ```

   The installer installs everything, then asks **"Do you want to set a password? [Y/n]"**. Answer **Y** to choose a web password (at least 4 characters; nothing is shown while you type); it then asks whether to turn on trusted access and which networks to trust (the Pi's own network is suggested). Answer **n** to run without one (everyone on your network can use it). You can change all of this later in Setup > Settings. Use `--password` or `--no-password` after the user name to skip the question. When it finishes, run `sudo reboot`.
4. **Open the page** at `http://livepi.local:8080` from any computer, tablet or phone on the same network. If you set a password, the page asks for it (no user name).
5. **Set up audio** on the **Setup** tab: pick your audio device, sample rate 48000 and buffer 64 (raise to 128 if the xrun counter climbs), then Apply.
6. **Add plugins**: on the Setup tab > Plugins upload each plugin as a `.zip`, a `.vst3` folder, or a single `.vst3`/`.so` file (Linux ARM64), press Scan, then add them to a channel on the **Mixer** tab.

No Pi yet, or no audio interface? You can try everything with the virtual sound card, see "Testing without an audio interface" below.

If the Pi has no internet, download `livepi-linux-arm64.tar.gz` from the Releases page on your computer, copy it over with `scp livepi-linux-arm64.tar.gz your-user@livepi.local:` and continue from `tar xzf`.

## Requirements

- A Raspberry Pi 4 or 5 is recommended (a Pi 3 runs it with less headroom), running **Raspberry Pi OS Lite 64-bit**, Debian 12 "Bookworm" or Debian 13 "Trixie". Both are tested automatically in clean ARM64 containers (see `.github/workflows/compat.yml`); a 32-bit OS will not work.
- An audio interface that Linux (ALSA) supports, or none at all for testing.
- Your own plugins built as **VST3 for Linux aarch64**.

## How releases are built

Pushing to `main` starts GitHub Actions, which builds LivePi on an ARM64 runner and publishes a **Release** (repo page > Releases) with:
- `livepi-build-N.zip` - the update package (for the Setup tab's offline update, or unzip it for a fresh install)
- `livepi-linux-arm64.tar.gz` - the same files as a tarball (the file the quick start downloads)

GitHub's free ARM64 runners are only available for **public** repositories.

## What the installer does

It copies the app to `/opt/livepi`, installs the runtime libraries, sets real-time limits for the audio group, switches the CPU governor to performance (at every boot), stops PulseAudio and Bluetooth, allows the page to reboot/shut down the Pi and run OS updates, and enables boot services. Unless you pass `--no-editor` it also installs a virtual screen (Xvfb) for plugin windows; LivePi starts it itself and shows each plugin's own screen inside the web page, so nothing else (no VNC, no second port) is needed. To add or change the password later: `sudo /opt/livepi/set-password.sh`; see "Trusted networks" for unlocking uploads and updates without one.

## Troubleshooting

- **Page does not open:** use the Pi's IP address instead of `livepi.local` (find it with `hostname -I` on the Pi, or in your router), and check `sudo systemctl status livepi`.
- **See what LivePi is doing:** `sudo journalctl -u livepi -n 50`.
- **No audio device in the list:** the Setup tab lists what Linux sees; `aplay -l` on the Pi shows the same. Choose the **Virtual** type to keep working without hardware.
- **Crackles or xruns:** raise the buffer size (64, then 128), use a wired network, and avoid the per-plugin CPU test and plugin windows during a show.
- **Forgot the password:** run `sudo /opt/livepi/set-password.sh --off` on the Pi.
- **An update did not start:** LivePi restores the previous version by itself after 3 failed starts and shows a notice on the page.

## Testing without an audio interface

On the Setup tab choose the device type **Virtual (no audio hardware)**. It behaves like a sound card (same timing, CPU and xrun counters), but its input is a test tone, pink noise, a sweep, silence, or **a looping audio file you upload** (a spoken voice recording works well), and its output is discarded. If no sound card is found at all, LivePi starts on the virtual one automatically (this is not remembered, so a real card is used as soon as it is plugged in).

To hear the result, use **Listen** on the Mixer tab: pick any channel (before or after its plugins) or the Monitor mix and press *Listen in this browser*. The audio is streamed to your browser; the delay is shown on the page and depends on the setting (Lowest / Low / Safe) and your network, typically 100 ms or more, so it is for checking and adjusting, not for performing. This also works with a real interface.

The main audio path always has priority: the audio thread only copies the chosen signal into a small buffer (nothing at all is copied while nobody is listening), and the streaming runs on web threads at lower priority. At most two browsers can listen at once, and listening stops by itself when the page is closed.

## Monitor mix (mappable monitoring output)

Every channel has a **Mon** send (pre-fader, after the channel's plugins; off by default). The sends are summed into the **Monitor mix** strip, which has its own plugin chain (for example LiveVoice for feedback control on the monitors), a fader and a mute. Tick the **Out** boxes on that strip to choose which output channels of the audio device carry it (outputs marked * also carry a channel, so the two would add together). The mapping and the monitor mix are saved in presets.

## Updating online (from GitHub)

On the **Setup** tab under **System**, **Check for updates** looks at this repo's latest GitHub Release. If a newer build exists, **Install latest** downloads it, verifies it, installs it and restarts LivePi (the old version is restored automatically if the new one fails to start). Both buttons are grayed out with "No internet access" when the Pi cannot reach GitHub. The Pi needs `curl` (the installer adds it). Development builds ("dev") never update themselves.

## CPU and temperature

- The header shows the Pi's SoC temperature (red above 75 °C).
- Each plugin has a **CPU** button: it runs a separate copy of the plugin (same settings) on a noise test signal at the current buffer size and reports average and peak load as a percentage of one core, plus a per-channel sum. It is an estimate, not a live meter, and it uses some CPU while it runs, so don't use it during a show.

## Updating (offline, from the web page)

1. Download `livepi-build-N.zip` from the repo's Releases page on any computer.
2. Open the Setup tab > System > **Install update** and upload the zip.

LivePi checks the zip (ARM64 program, checksum, compatible installer), keeps the old version as
`LivePi.prev`, installs the new one and restarts. Presets, uploaded plugins and the password are kept.
If the new version fails to start 3 times in a row, the old one is restored automatically and the page
shows a notice. If an update says it needs the newer installer, unzip it on the Pi and run
`sudo ./install-pi.sh <user>` again.

## Using it (everything from the web page)

**Mixer tab**
- One strip per channel: name, IN meter (before the plugins), fader, OUT meter (after the plugins and fader) with a dB scale, mute, and the plugin chain.
- Master fader on the right; hide it under Setup > Settings if you don't use it. Double-click (or double-tap) a fader to reset it to 0 dB.
- **PAFL** (the round button on each channel, as on an Allen & Heath SQ) sends that channel to the Monitor mix; one channel at a time for now. The source menu on the Monitor mix strip picks the channel and its tap point (before or after its plugins). Everything you monitor, on the outputs or with Listen here, goes through the Monitor mix fader. The Monitor mix strip has its own fader and mute, goes to the outputs you tick, and carries no plugins. **Listen here** plays the Monitor mix on your phone or computer (after its fader); the listen delay is set in Setup > Settings.
- Per plugin: **Controls** (big touch knobs, switches and lists, grouped by the plugin's own sections; double-tap a knob for its default), **Plugin** (the plugin's own screen, full screen in the page, with touch, mouse and keyboard; on a phone the ⌨ button opens the keyboard),
  **Bypass**, **x** (remove).
- Presets (chains, plugin settings, faders, names) are saved/loaded from the top bar.
  The last session is restored automatically on boot.

**Setup tab**
- Audio device, sample rate and buffer size (start at 48000 Hz / 64) and channel count.
- **Plugins**: one Choose… button takes a `.zip`, a `.vst3` file or a `.so` file (a bare file is wrapped into a bundle); on a computer you can also drop a whole `.vst3` folder on the drop area. LivePi unpacks it, checks it is a
  Linux ARM64 build (`Contents/aarch64-linux/*.so`), and scans it. Delete uploaded plugins here too.
- **System**: restart LivePi, reboot or shut down the Pi, install LivePi updates, and update the Pi's operating system (apt-get update / upgrade).
  The OS update buttons are grayed out with "No internet access" when the Pi cannot reach its package servers.

With one channel the output feeds both L and R. With more, channel N goes to output N.
Data lives in `~/.livepi` (audio setup, plugin list, `plugins/`, `last.json`, `presets/`, `auth.json`).

## Security

- **Password** (Setup > Settings > Password): set or change it there, no user name. With **password protection on**, every device logs in once on the page (it stays logged in for 30 days); with it **off**, or with no password, everyone on your network can use LivePi. Turning protection off never asks for the password; turning it back on always asks you to type the password to use (the old one or a new one), so you can't switch it on and then not know it. Changing the password logs out all other devices. Ten wrong passwords lock that device out for 10 minutes.
- **Forgot the password:** on the Pi run `sudo /opt/livepi/set-password.sh --off` (protection off) or `sudo /opt/livepi/set-password.sh` (new password).
- **Trusted access** (Setup > Settings, only while a password is set and protection is on): devices on the trusted networks you list skip the login. Only trust a network you control, such as a dedicated AV VLAN. From the Pi: `sudo /opt/livepi/trusted-network.sh add 192.168.10.0/24` and `... on` / `off` / `list`.
- Uploaded plugins are native code that runs on the Pi, so only upload plugins you trust.
- Traffic is plain HTTP, so the password is not encrypted on the network. Keep the Pi on a trusted network or VLAN, not exposed to the internet.
- Zip uploads are checked for path tricks, symbolic links and Windows-style paths before unpacking.
- Plugin windows are drawn in software on a virtual screen, so use them for setup, not during a show. If one shows no picture, Setup > Plugins > **Plugin windows** says what is missing.

## Notes

- Your plugins must be built for Linux aarch64. Add an ARM64 Linux target to your plugin CI.
- The fader is a gain stage only: no pan or solo (outputs are direct, one per channel).
- Status shows reported device latency, CPU and xruns. If xruns climb, raise the buffer to 128.
