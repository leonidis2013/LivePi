#!/usr/bin/env bash
# Install LivePi on a Raspberry Pi and tune it for low-latency audio.
# Run from the unpacked livepi folder:
#   sudo ./install-pi.sh [username] [--no-editor] [--password | --no-password]
#
# By default this also installs a virtual screen (Xvfb) so plugin windows
# can be opened from the web page. Use --no-editor to skip that.
# You will be asked whether you want a password for the web page (Y/N). Use --password or --no-password to skip
# the question, or set LIVEPI_PASSWORD to choose the password without a prompt.
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

HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"
GROUP_NAME="$(id -gn "$USER_NAME")"

# ---- Earlier versions were called LiveHost: move everything over to LivePi
if [[ -f /etc/systemd/system/livehost.service || -d /opt/livehost || -d "$HOME_DIR/.livehost" ]]; then
  echo "== Moving the old LiveHost install over to LivePi"
  for u in livehost livehost-governor livehost-novnc livehost-vnc livehost-display; do
    systemctl disable --now "$u.service" 2>/dev/null || true
    rm -f "/etc/systemd/system/$u.service"
  done
  systemctl daemon-reload
  rm -f /etc/sudoers.d/livehost /usr/local/sbin/livehost-os /usr/local/sbin/livehost-os-job
  if [[ -d /var/lib/livehost-os && ! -d /var/lib/livepi-os ]]; then mv /var/lib/livehost-os /var/lib/livepi-os; else rm -rf /var/lib/livehost-os; fi
  # Settings, presets, uploaded plugins, password and trusted networks are kept
  if [[ -d "$HOME_DIR/.livehost" && ! -d "$HOME_DIR/.livepi" ]]; then
    mv "$HOME_DIR/.livehost" "$HOME_DIR/.livepi"
    # saved plugin paths point into the old folder
    find "$HOME_DIR/.livepi" -path "$HOME_DIR/.livepi/plugins" -prune -o -type f \( -name '*.xml' -o -name '*.json' \) -print0 \
      | xargs -0 -r sed -i "s#/\.livehost/#/.livepi/#g"
    rm -f "$HOME_DIR/.livepi/vnc.txt" "$HOME_DIR/.livepi/vnc.pass" "$HOME_DIR/.livepi/update-pending"
    chown -R "$USER_NAME:$GROUP_NAME" "$HOME_DIR/.livepi"
    echo "   settings moved to $HOME_DIR/.livepi"
  fi
  rm -rf /opt/livehost
fi

echo "== Runtime libraries"
apt-get update
apt-get install -y libfreetype6 libfontconfig1 libx11-6 libxext6 libxrandr2 libxinerama1 libxcursor1 libjack-jackd2-0 unzip curl ca-certificates
apt-get install -y libasound2 2>/dev/null || apt-get install -y libasound2t64

echo "== Installing to /opt/livepi"
mkdir -p /opt/livepi
cp -r "$HERE/LivePi" "$HERE/web" "$HERE/set-password.sh" "$HERE/trusted-network.sh" "$HERE/run.sh" "$HERE/INSTALLER_LEVEL" /opt/livepi/
chmod +x /opt/livepi/LivePi /opt/livepi/set-password.sh /opt/livepi/trusted-network.sh /opt/livepi/run.sh
# The app owns its folder so it can install offline updates from the web page
chown -R "$USER_NAME:$(id -gn "$USER_NAME")" /opt/livepi

echo "== Real-time limits for the audio group"
usermod -aG audio "$USER_NAME"
cat > /etc/security/limits.d/99-audio.conf <<EOF
@audio - rtprio 95
@audio - memlock unlimited
@audio - nice -10
EOF

echo "== CPU governor: performance"
# A tiny boot service works on every Raspberry Pi OS release (cpufrequtils is gone in Debian 13)
cat > /etc/systemd/system/livepi-governor.service <<'EOF'
[Unit]
Description=LivePi: CPU governor performance

[Service]
Type=oneshot
ExecStart=/bin/sh -c 'for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do echo performance > "$g" 2>/dev/null || true; done'
RemainAfterExit=yes

[Install]
WantedBy=multi-user.target
EOF
systemctl daemon-reload
systemctl enable --now livepi-governor.service >/dev/null 2>&1 || true

echo "== Stopping services that fight for the audio device"
systemctl disable --now pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl --global disable pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl disable --now bluetooth.service 2>/dev/null || true

echo "== OS update helper (apt) for the web page"
install -m 755 -o root -g root "$HERE/livepi-os" "$HERE/livepi-os-job" /usr/local/sbin/
mkdir -p /var/lib/livepi-os
chmod 755 /var/lib/livepi-os

echo "== Allow the web page to reboot / shut down the Pi and run OS updates"
cat > /etc/sudoers.d/livepi <<EOF
${USER_NAME} ALL=(root) NOPASSWD: /usr/bin/systemctl reboot, /usr/bin/systemctl poweroff, /usr/local/sbin/livepi-os check, /usr/local/sbin/livepi-os upgrade
EOF
chmod 440 /etc/sudoers.d/livepi
visudo -c -f /etc/sudoers.d/livepi >/dev/null || { echo "sudoers check failed, removing rule"; rm -f /etc/sudoers.d/livepi; }

if [[ $WITH_EDITOR -eq 1 ]]; then
  echo "== Plugin window support (virtual screen, started by LivePi itself)"
  apt-get install -y xvfb libxtst6 fonts-dejavu-core libgl1
  EDITOR_ARG=""
else
  EDITOR_ARG=" --no-editor"
fi

echo "== LivePi service"
cat > /etc/systemd/system/livepi.service <<EOF
[Unit]
Description=LivePi VST3 host
After=sound.target network-online.target
Wants=network-online.target

[Service]
User=${USER_NAME}
Environment=HOME=/home/${USER_NAME}
WorkingDirectory=/opt/livepi
ExecStart=/opt/livepi/run.sh --port 8080${EDITOR_ARG}
Restart=always
RestartSec=2
# SIGTERM goes to LivePi only; its helpers (virtual screen, viewer) are cleaned up after it has exited
KillMode=mixed
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

if [[ -n "${LIVEPI_PASSWORD:-}" ]]; then PW_MODE=yes; fi
if [[ "$PW_MODE" == ask ]]; then
  if [[ -t 0 ]]; then
    echo "A password protects the web page. Without one, anyone on your network can use LivePi."
    echo "(You can also set, change or turn off the password later in Setup > Settings.)"
    if yesno "Do you want to set a password? [Y/n]" Y; then PW_MODE=yes; else PW_MODE=no; fi
  else
    PW_MODE=no
  fi
fi

if [[ "$PW_MODE" == yes ]]; then
  if /opt/livepi/set-password.sh "$USER_NAME"; then
    # Offer to skip the login on the Pi's own network ("trusted access")
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
    if [[ -t 0 && -n "$NET" ]] && yesno "Skip the password for devices on this network ($NET)? [y/N]" N; then
      /opt/livepi/trusted-network.sh add "$NET" >/dev/null || true
      /opt/livepi/trusted-network.sh on || true
    fi
  else
    echo "No password was set. LivePi will still start; set one later in Setup > Settings."
  fi
else
  echo "Continuing without a password: everyone on your network can use LivePi. Set one any time in Setup > Settings."
fi

systemctl enable --now livepi.service
systemctl restart livepi.service

echo
echo "Done. Open http://$(hostname).local:8080 from any device on the network."
echo "Upload your Linux ARM64 plugins from the Setup tab (zip each .vst3 folder)."
echo "Reboot once so the audio-group limits take effect."
