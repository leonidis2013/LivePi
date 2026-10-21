#!/usr/bin/env bash
# Install LiveHost on a Raspberry Pi and tune it for low-latency audio.
# Run from the unpacked livehost folder:
#   sudo ./install-pi.sh [username] [--no-editor]
#
# By default this also sets up a virtual screen + browser viewer (port 6080) so plugin windows
# can be opened from the web page. Use --no-editor to skip that.
# You will be asked to choose a password for the web page. Set LIVEHOST_PASSWORD to skip the prompt.
set -euo pipefail

USER_NAME="pi"
WITH_EDITOR=1
for a in "$@"; do
  case "$a" in
    --no-editor) WITH_EDITOR=0 ;;
    *) USER_NAME="$a" ;;
  esac
done
HERE="$(cd "$(dirname "$0")" && pwd)"

if [[ $EUID -ne 0 ]]; then echo "Run with sudo"; exit 1; fi
id "$USER_NAME" >/dev/null 2>&1 || { echo "User '$USER_NAME' does not exist. Usage: sudo ./install-pi.sh <your-username>"; exit 1; }

echo "== Runtime libraries"
apt-get update
apt-get install -y libfreetype6 libfontconfig1 libx11-6 libxext6 libxrandr2 libxinerama1 libxcursor1 libjack-jackd2-0 unzip
apt-get install -y libasound2 2>/dev/null || apt-get install -y libasound2t64

echo "== Installing to /opt/livehost"
mkdir -p /opt/livehost
cp -r "$HERE/LiveHost" "$HERE/web" "$HERE/set-password.sh" /opt/livehost/
chmod +x /opt/livehost/LiveHost /opt/livehost/set-password.sh

echo "== Real-time limits for the audio group"
usermod -aG audio "$USER_NAME"
cat > /etc/security/limits.d/99-audio.conf <<EOF
@audio - rtprio 95
@audio - memlock unlimited
@audio - nice -10
EOF

echo "== CPU governor: performance"
apt-get install -y cpufrequtils >/dev/null 2>&1 || true
echo 'GOVERNOR="performance"' > /etc/default/cpufrequtils
for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do
  echo performance > "$g" 2>/dev/null || true
done

echo "== Stopping services that fight for the audio device"
systemctl disable --now pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl --global disable pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl disable --now bluetooth.service 2>/dev/null || true

echo "== Allow the web page to reboot / shut down the Pi"
cat > /etc/sudoers.d/livehost <<EOF
${USER_NAME} ALL=(root) NOPASSWD: /usr/bin/systemctl reboot, /usr/bin/systemctl poweroff
EOF
chmod 440 /etc/sudoers.d/livehost
visudo -c -f /etc/sudoers.d/livehost >/dev/null || { echo "sudoers check failed, removing rule"; rm -f /etc/sudoers.d/livehost; }

if [[ $WITH_EDITOR -eq 1 ]]; then
  echo "== Plugin window support (virtual display + browser viewer)"
  apt-get install -y xvfb x11vnc novnc websockify

  HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"

  cat > /etc/systemd/system/livehost-display.service <<EOF
[Unit]
Description=LiveHost virtual display

[Service]
User=${USER_NAME}
ExecStart=/usr/bin/Xvfb :99 -screen 0 1280x800x24 -nolisten tcp
Restart=always

[Install]
WantedBy=multi-user.target
EOF

  cat > /etc/systemd/system/livehost-vnc.service <<EOF
[Unit]
Description=LiveHost VNC (local only, password protected)
After=livehost-display.service
Requires=livehost-display.service

[Service]
User=${USER_NAME}
ExecStartPre=/bin/sleep 2
ExecStart=/usr/bin/x11vnc -display :99 -localhost -rfbauth ${HOME_DIR}/.livehost/vnc.pass -forever -shared -noxdamage -quiet
Restart=always

[Install]
WantedBy=multi-user.target
EOF

  cat > /etc/systemd/system/livehost-novnc.service <<EOF
[Unit]
Description=LiveHost browser viewer
After=livehost-vnc.service
Requires=livehost-vnc.service

[Service]
User=${USER_NAME}
ExecStart=/usr/bin/websockify --web=/usr/share/novnc 6080 localhost:5900
Restart=always

[Install]
WantedBy=multi-user.target
EOF
  DISPLAY_LINES="Environment=DISPLAY=:99
Wants=livehost-display.service
After=livehost-display.service
ExecStartPre=/bin/sleep 3"
else
  DISPLAY_LINES=""
fi

echo "== LiveHost service"
cat > /etc/systemd/system/livehost.service <<EOF
[Unit]
Description=LiveHost VST3 host
After=sound.target network-online.target
Wants=network-online.target

[Service]
User=${USER_NAME}
Environment=HOME=/home/${USER_NAME}
${DISPLAY_LINES}
WorkingDirectory=/opt/livehost
ExecStart=/opt/livehost/LiveHost --port 8080
Restart=always
RestartSec=2
LimitRTPRIO=95
LimitMEMLOCK=infinity
Nice=-10

[Install]
WantedBy=multi-user.target
EOF

systemctl daemon-reload

echo "== Web password"
if [[ -t 0 || -n "${LIVEHOST_PASSWORD:-}" ]]; then
  /opt/livehost/set-password.sh "$USER_NAME"
else
  echo "No terminal available, so no password was set. Run: sudo /opt/livehost/set-password.sh"
fi

if [[ $WITH_EDITOR -eq 1 ]]; then
  systemctl enable --now livehost-display.service
fi
systemctl enable --now livehost.service
systemctl restart livehost.service

echo
echo "Done. Open http://$(hostname).local:8080 from any device on the network."
echo "Upload your Linux ARM64 plugins from the Setup tab (zip each .vst3 folder)."
echo "Reboot once so the audio-group limits take effect."
