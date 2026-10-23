# LiveHost

Headless VST3 host for live voice on a Raspberry Pi, managed entirely from a web page.
Built with JUCE. Runs your own Linux ARM64 VST3 plugins, one chain per channel.

## Quick start

1. **Flash the SD card.** In [Raspberry Pi Imager](https://www.raspberrypi.com/software/) choose your Pi model and **Raspberry Pi OS Lite (64-bit)**. Before writing, open the OS settings and set a hostname (for example `livehost`), a username and password, turn on **SSH**, and enter Wi-Fi if you need it. A network cable is better for live use.
2. **Fit your audio interface** (HAT, USB card) and follow its own setup steps if it has any. Boot the Pi and wait about a minute.
3. **Connect from your computer** with `ssh your-user@livehost.local` and download and install LiveHost:

   ```bash
   curl -L -o livehost.tar.gz https://github.com/leonidis2013/LivePi/releases/latest/download/livehost-linux-arm64.tar.gz
   tar xzf livehost.tar.gz
   cd livehost
   sudo ./install-pi.sh your-user
   ```

   The installer installs everything, then asks **"Do you want to set a password? [Y/n]"**. Answer **Y** to choose a web password (at least 8 characters; nothing is shown while you type). Answer **n** to run without one: it then offers to *trust your network* so uploads, updates and reboot still work without a password. Use `--password` or `--no-password` after the user name to skip the question. When it finishes, run `sudo reboot`.
4. **Open the page** at `http://livehost.local:8080` from any computer, tablet or phone on the same network. If you set a password, leave the user name blank and enter it.
5. **Set up audio** on the **Setup** tab: pick your audio device, sample rate 48000 and buffer 64 (raise to 128 if the xrun counter climbs), then Apply.
6. **Add plugins**: on the Setup tab upload each plugin as a `.zip` of its Linux ARM64 `.vst3` folder, press Scan, then add them to a channel on the **Mixer** tab.

No Pi yet, or no audio interface? You can try everything with the virtual sound card, see "Testing without an audio interface" below.

If the Pi has no internet, download `livehost-linux-arm64.tar.gz` from the Releases page on your computer, copy it over with `scp livehost-linux-arm64.tar.gz your-user@livehost.local:` and continue from `tar xzf`.

## Requirements

- A Raspberry Pi 4 or 5 is recommended (a Pi 3 runs it with less headroom), running **Raspberry Pi OS Lite 64-bit**, Debian 12 "Bookworm" or Debian 13 "Trixie". Both are tested automatically in clean ARM64 containers (see `.github/workflows/compat.yml`); a 32-bit OS will not work.
- An audio interface that Linux (ALSA) supports, or none at all for testing.
- Your own plugins built as **VST3 for Linux aarch64**.

## How releases are built

Pushing to `main` starts GitHub Actions, which builds LiveHost on an ARM64 runner and publishes a **Release** (repo page > Releases) with:
- `livehost-build-N.zip` - the update package (for the Setup tab's offline update, or unzip it for a fresh install)
- `livehost-linux-arm64.tar.gz` - the same files as a tarball (the file the quick start downloads)

GitHub's free ARM64 runners are only available for **public** repositories.

## What the installer does

It copies the app to `/opt/livehost`, installs the runtime libraries, sets real-time limits for the audio group, switches the CPU governor to performance (at every boot), stops PulseAudio and Bluetooth, allows the page to reboot/shut down the Pi and run OS updates, and enables boot services. Unless you pass `--no-editor` it also sets up a virtual screen and browser viewer (port 6080) so plugin windows can be opened from the page. To add or change the password later: `sudo /opt/livehost/set-password.sh`. Without a password the plugin window viewer (Edit button) is unavailable; everything else works, and see "Trusted networks" for unlocking uploads and updates.

## Troubleshooting

- **Page does not open:** use the Pi's IP address instead of `livehost.local` (find it with `hostname -I` on the Pi, or in your router), and check `sudo systemctl status livehost`.
- **See what LiveHost is doing:** `sudo journalctl -u livehost -n 50`.
- **No audio device in the list:** the Setup tab lists what Linux sees; `aplay -l` on the Pi shows the same. Choose the **Virtual** type to keep working without hardware.
- **Crackles or xruns:** raise the buffer size (64, then 128), use a wired network, and avoid the per-plugin CPU test and plugin windows during a show.
- **Forgot the password:** run `sudo /opt/livehost/set-password.sh` on the Pi.
- **An update did not start:** LiveHost restores the previous version by itself after 3 failed starts and shows a notice on the page.

## Testing without an audio interface

On the Setup tab choose the device type **Virtual (no audio hardware)**. It behaves like a sound card (same timing, CPU and xrun counters), but its input is a test tone, pink noise, a sweep, silence, or **a looping audio file you upload** (a spoken voice recording works well), and its output is discarded. If no sound card is found at all, LiveHost starts on the virtual one automatically (this is not remembered, so a real card is used as soon as it is plugged in).

To hear the result, use **Listen** on the Mixer tab: pick any channel (before or after its plugins) or the Monitor mix and press *Listen in this browser*. The audio is streamed to your browser; the delay is shown on the page and depends on the setting (Lowest / Low / Safe) and your network, typically 100 ms or more, so it is for checking and adjusting, not for performing. This also works with a real interface.

The main audio path always has priority: the audio thread only copies the chosen signal into a small buffer (nothing at all is copied while nobody is listening), and the streaming runs on web threads at lower priority. At most two browsers can listen at once, and listening stops by itself when the page is closed.

## Monitor mix (mappable monitoring output)

Every channel has a **Mon** send (pre-fader, after the channel's plugins; off by default). The sends are summed into the **Monitor mix** strip, which has its own plugin chain (for example LiveVoice for feedback control on the monitors), a fader and a mute. Tick the **Out** boxes on that strip to choose which output channels of the audio device carry it (outputs marked * also carry a channel, so the two would add together). The mapping and the monitor mix are saved in presets.

## Updating online (from GitHub)

On the **Setup** tab, **Check for updates** looks at this repo's latest GitHub Release. If a newer build exists, **Install latest** downloads it, verifies it, installs it and restarts LiveHost (the old version is restored automatically if the new one fails to start). Both buttons are grayed out with "No internet access" when the Pi cannot reach GitHub. The Pi needs `curl` (the installer adds it). Development builds ("dev") never update themselves.

## CPU and temperature

- The header shows the Pi's SoC temperature (red above 75 °C).
- Each plugin has a **CPU** button: it runs a separate copy of the plugin (same settings) on a noise test signal at the current buffer size and reports average and peak load as a percentage of one core, plus a per-channel sum. It is an estimate, not a live meter, and it uses some CPU while it runs, so don't use it during a show.

## Updating (offline, from the web page)

1. Download `livehost-build-N.zip` from the repo's Releases page on any computer.
2. Open the Setup tab > System > **Install update** and upload the zip.

LiveHost checks the zip (ARM64 program, checksum, compatible installer), keeps the old version as
`LiveHost.prev`, installs the new one and restarts. Presets, uploaded plugins and the password are kept.
If the new version fails to start 3 times in a row, the old one is restored automatically and the page
shows a notice. If an update says it needs the newer installer, unzip it on the Pi and run
`sudo ./install-pi.sh <user>` again.

## Using it (everything from the web page)

**Mixer tab**
- One strip per channel: name, input meter, fader, output meter, mute, and the plugin chain.
- Master fader on the right. Double-click a fader to reset it to 0 dB.
- Per plugin: **Edit** (the real plugin window in your browser), **Params** (generic sliders),
  **Bypass**, **x** (remove).
- Presets (chains, plugin settings, faders, names) are saved/loaded from the top bar.
  The last session is restored automatically on boot.

**Setup tab**
- Audio device, sample rate and buffer size (start at 48000 Hz / 64) and channel count.
- **Plugins on this Pi**: upload a `.zip` of a plugin's `.vst3` folder. LiveHost unpacks it, checks it is a
  Linux ARM64 build (`Contents/aarch64-linux/*.so`), and scans it. Delete uploaded plugins here too.
- **System**: restart LiveHost, reboot or shut down the Pi, install LiveHost updates, and update the Pi's operating system (apt-get update / upgrade).
  The OS update buttons are grayed out with "No internet access" when the Pi cannot reach its package servers.

With one channel the output feeds both L and R. With more, channel N goes to output N.
Data lives in `~/.livehost` (audio setup, plugin list, `plugins/`, `last.json`, `presets/`, `auth.json`).

## Security

- A password is required for everything once set. Upload, delete and reboot stay disabled until a password exists.
- Uploaded plugins are native code that runs on the Pi, so only upload plugins you trust.
- Traffic is plain HTTP, so the password is not encrypted on the network. Keep the Pi on a trusted
  network or VLAN, not exposed to the internet.
- Zip uploads are checked for path tricks, symbolic links and Windows-style paths before unpacking.
- Plugin windows are drawn in software on a virtual screen, so use them for setup, not during a show.

### Trusted networks (optional, no password from your own network)

```
sudo /opt/livehost/trusted-network.sh add 192.168.10.0/24   # a network, or a single IP
sudo /opt/livehost/trusted-network.sh list | remove <entry> | clear
```

Computers in a trusted network use LiveHost without a password, including uploads, updates and reboot, even if no password is set at all. Everyone else still needs the password (and without one, the dangerous controls stay locked). Only trust a network you control, such as a dedicated AV VLAN: anyone on it can upload plugins, which are native code, and reboot the Pi. The list is stored in `~/.livehost/trusted.txt`; IPv4 only, and networks wider than /8 are ignored. The plugin-window viewer (port 6080) has its own VNC password and is not affected.

## Notes

- Your plugins must be built for Linux aarch64. Add an ARM64 Linux target to your plugin CI.
- The fader is a gain stage only: no pan or solo (outputs are direct, one per channel).
- Status shows reported device latency, CPU and xruns. If xruns climb, raise the buffer to 128.
