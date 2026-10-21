# LiveHost

Headless VST3 host for live voice on a Raspberry Pi, with a mixer you control from a web page.
Built with JUCE. Runs your own Linux ARM64 VST3 plugins, one chain per channel.

## Build (no compiling on the Pi)

Push to `main`. GitHub Actions builds on an ARM64 runner and uploads
`livehost-linux-arm64.tar.gz` (Actions tab > latest run > Artifacts).

Note: GitHub's free ARM64 runners are only available for **public** repositories.

## Install on the Pi

```bash
tar xzf livehost-linux-arm64.tar.gz
cd livehost
sudo ./install-pi.sh your-username        # add --no-editor to skip plugin window support
sudo reboot
```

Then open `http://<pi-hostname>.local:8080`.

The installer copies the app to `/opt/livehost`, sets real-time limits, switches the CPU governor
to performance, stops PulseAudio and Bluetooth, and enables boot services. Unless you pass
`--no-editor` it also sets up a virtual screen and a browser viewer (port 6080) for plugin windows.

## Using it

**Mixer tab**
- One strip per channel: name, input meter, fader, output meter, mute, and the plugin chain.
- Master fader on the right. Double-click a fader to reset it to 0 dB.
- Per plugin: **Edit** (opens the real plugin window in your browser), **Params** (generic sliders),
  **Bypass**, **x** (remove).
- Presets (chains, plugin settings, fader positions, names) are saved and loaded from the top bar.
  The last session is restored automatically on boot.

**Setup tab**
- Pick the audio device, sample rate and buffer size (start at 48000 Hz / 64).
- Set the channel count.
- Scan plugins after copying `.vst3` bundles to `~/.vst3` on the Pi.

With one channel the output feeds both L and R. With more, channel N goes to output N.

Data lives in `~/.livehost` (audio setup, plugin list, `last.json`, `presets/`).

## Notes

- Your plugins must be built for Linux aarch64. Add an ARM64 Linux target to your plugin CI.
- There is no password on the web page or the plugin window viewer. Keep the Pi on a trusted network or VLAN.
- Plugin windows are drawn in software on a virtual screen, so use them for setup, not during a show.
- The fader is a gain stage only: no pan or solo (outputs are direct, one per channel).
- Status shows reported device latency, CPU and xruns. If xruns climb, raise the buffer to 128.
