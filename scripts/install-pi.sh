#!/usr/bin/env bash
# Install LiveHost on a Raspberry Pi and tune it for low-latency audio.
# Run from the unpacked livehost folder:
#   sudo ./install-pi.sh [username] [--no-editor] [--password | --no-password]
#
# By default this also sets up a virtual screen + browser viewer (port 6080) so plugin windows
# can be opened from the web page. Use --no-editor to skip that.
# You will be asked whether you want a password for the web page (Y/N). Use --password or --no-password to skip
# the question, or set LIVEHOST_PASSWORD to choose the password without a prompt.
set -euo pipefail

USER_NAME="pi"
WITH_EDITOR=1
PW_MODE=ask      # ask | yes | no
for a in "$@"; do
  case "$a" in
    --no-editor) WITH_EDITOR=0 ;;
    --password) PW_MODE=yes ;;
    --no-password) PW_MODE=no ;;
    *) USER_NAME="$a" ;;
  esac
done
HERE="$(cd "$(dirname "$0")" && pwd)"

if [[ $EUID -ne 0 ]]; then echo "Run with sudo"; exit 1; fi
id "$USER_NAME" >/dev/null 2>&1 || { echo "User '$USER_NAME' does not exist. Usage: sudo ./install-pi.sh <your-username>"; exit 1; }

echo "== Runtime libraries"
apt-get update
apt-get install -y libfreetype6 libfontconfig1 libx11-6 libxext6 libxrandr2 libxinerama1 libxcursor1 libjack-jackd2-0 unzip curl ca-certificates
apt-get install -y libasound2 2>/dev/null || apt-get install -y libasound2t64

echo "== Installing to /opt/livehost"
mkdir -p /opt/livehost
cp -r "$HERE/LiveHost" "$HERE/web" "$HERE/set-password.sh" "$HERE/trusted-network.sh" "$HERE/run.sh" "$HERE/INSTALLER_LEVEL" /opt/livehost/
chmod +x /opt/livehost/LiveHost /opt/livehost/set-password.sh /opt/livehost/trusted-network.sh /opt/livehost/run.sh
# The app owns its folder so it can install offline updates from the web page
chown -R "$USER_NAME:$(id -gn "$USER_NAME")" /opt/livehost

echo "== Real-time limits for the audio group"
usermod -aG audio "$USER_NAME"
cat > /etc/security/limits.d/99-audio.conf <<EOF
@audio - rtprio 95
@audio - memlock unlimited
@audio - nice -10
EOF

echo "== CPU governor: performance"
# A tiny boot service works on every Raspberry Pi OS release (cpufrequtils is gone in Debian 13)
cat > /etc/systemd/system/livehost-governor.service <<'EOF'
[Unit]
Description=LiveHost: CPU governor performance

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now livehost-governor.service >/dev/null 2>&1 || true

echo "== Stopping services that fight for the audio device"
systemctl disable --now pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl --global disable pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl disable --now bluetooth.service 2>/dev/null || true

echo "== OS update helper (apt) for the web page"
install -m 755 -o root -g root "$HERE/livehost-os" "$HERE/livehost-os-job" /usr/local/sbin/
mkdir -p /var/lib/livehost-os
chmod 755 /var/lib/livehost-os

echo "== Allow the web page to reboot / shut down the Pi and run OS updates"
cat > /etc/sudoers.d/livehost <<EOF
${USER_NAME} ALL=(root) NOPASSWD: /usr/bin/systemctl reboot, /usr/bin/systemctl poweroff, /usr/local/sbin/livehost-os check, /usr/local/sbin/livehost-os upgrade
EOF
chmod 440 /etc/sudoers.d/livehost
visudo -c -f /etc/sudoers.d/livehost >/dev/null || { echo "sudoers check failed, removing rule"; rm -f /etc/sudoers.d/livehost; }

if [[ $WITH_EDITOR -eq 1 ]]; then
  echo "== Plugin window support (virtual display + browser viewer)"
  apt-get install -y xvfb x11vnc novnc websockify

  HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"
  GROUP_NAME="$(id -gn "$USER_NAME")"
  install -d -o "$USER_NAME" -g "$GROUP_NAME" -m 700 "${HOME_DIR}/.livehost"
  # The viewer uses its own random password; the web page fetches it (only for logged-in or trusted
  # clients) and signs in automatically, so nobody has to type it.
  if [[ ! -s "${HOME_DIR}/.livehost/vnc.txt" || ! -s "${HOME_DIR}/.livehost/vnc.pass" ]]; then
    VNCPW="$(head -c 64 /dev/urandom | tr -dc 'A-Za-z0-9' | head -c 8)"
    ( umask 077; printf '%s' "$VNCPW" > "${HOME_DIR}/.livehost/vnc.txt" )
    x11vnc -storepasswd "$VNCPW" "${HOME_DIR}/.livehost/vnc.pass" >/dev/null 2>&1
    chown "$USER_NAME:$GROUP_NAME" "${HOME_DIR}/.livehost/vnc.txt" "${HOME_DIR}/.livehost/vnc.pass"
    chmod 600 "${HOME_DIR}/.livehost/vnc.txt" "${HOME_DIR}/.livehost/vnc.pass"
  fi

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
ExecStart=/opt/livehost/run.sh --port 8080
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
yesno() {   # yesno "question" default(Y|N)  -> returns 0 for yes
  local prompt="$1" def="$2" ans
  read -r -p "$prompt " ans || ans=""
  ans="${ans:-$def}"
  [[ "$ans" =~ ^[Yy] ]]
}

if [[ -n "${LIVEHOST_PASSWORD:-}" ]]; then PW_MODE=yes; fi
if [[ "$PW_MODE" == ask ]]; then
  if [[ -t 0 ]]; then
    echo "A password protects the web page. Without one, anyone on your network can use the mixer,"
    echo "and plugin upload, updates and reboot stay locked unless you trust your network."
    if yesno "Do you want to set a password? [Y/n]" Y; then PW_MODE=yes; else PW_MODE=no; fi
  else
    PW_MODE=no
    echo "No terminal available, so no password was set. Run: sudo /opt/livehost/set-password.sh"
  fi
fi

if [[ "$PW_MODE" == yes ]]; then
  /opt/livehost/set-password.sh "$USER_NAME" || echo "No password was set. LiveHost will still start; set one later with: sudo /opt/livehost/set-password.sh"
else
  echo "Continuing without a password."
  # Offer to trust the Pi's own network so uploads, updates and reboot work without one
  NET=""
  if command -v python3 >/dev/null 2>&1; then
    NET="$(python3 - <<'PY' 2>/dev/null || true
import ipaddress, subprocess
out = subprocess.run(["ip", "-4", "-o", "addr", "show", "scope", "global"], capture_output=True, text=True).stdout.split()
for i, w in enumerate(out):
    if w == "inet":
        print(ipaddress.ip_interface(out[i + 1]).network)
        break
PY
)"
  fi
  if [[ -t 0 && -n "$NET" ]]; then
    if yesno "Trust this network ($NET) so plugin upload, updates and reboot work without a password? [Y/n]" Y; then
      /opt/livehost/trusted-network.sh add "$NET" || true
    fi
  elif [[ -n "$NET" ]]; then
    echo "To unlock uploads and updates without a password, run: sudo /opt/livehost/trusted-network.sh add $NET"
  fi
  fi

if [[ $WITH_EDITOR -eq 1 ]]; then
  systemctl daemon-reload
  systemctl enable --now livehost-display.service livehost-vnc.service livehost-novnc.service
fi
systemctl enable --now livehost.service
systemctl restart livehost.service

echo
echo "Done. Open http://$(hostname).local:8080 from any device on the network."
echo "Upload your Linux ARM64 plugins from the Setup tab (zip each .vst3 folder)."
echo "Reboot once so the audio-group limits take effect."
