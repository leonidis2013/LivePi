#!/usr/bin/env bash
# Install LivePi on a Raspberry Pi and tune it for low-latency audio.
# Run from the unpacked livepi folder:
#   sudo ./install-pi.sh [username]    (default: the account running sudo) [--no-editor] [--password | --no-password] [--pisound | --no-pisound] [--no-tuning]
#
# It also tunes the OS for live audio (rt-tune.sh: a CPU core reserved for the audio, full kernel
# preemption, no USB power saving, no automatic background updates). Use --no-tuning to skip that.
#
# By default this also installs a virtual screen (Xvfb) so plugin windows
# can be opened from the web page. Use --no-editor to skip that.
# You will be asked whether you want a password for the web page (Y/N). Use --password or --no-password to skip
# the question, or set LIVEPI_PASSWORD to choose the password without a prompt.
set -euo pipefail

USER_NAME="${SUDO_USER:-pi}"   # the account that ran sudo, unless one is given
WITH_EDITOR=1
PW_MODE=ask      # ask | yes | no
PISOUND=ask      # ask | yes | no
TUNING=1
for a in "$@"; do
  case "$a" in
    --no-editor) WITH_EDITOR=0 ;;
    --password) PW_MODE=yes ;;
    --no-password) PW_MODE=no ;;
    --pisound) PISOUND=yes ;;
    --no-pisound) PISOUND=no ;;
    --no-tuning) TUNING=0 ;;
    *) USER_NAME="$a" ;;
  esac
done
HERE="$(cd "$(dirname "$0")" && pwd)"
yesno_early() { local a; read -r -p "$1 " a || a=""; a="${a:-$2}"; [[ "$a" =~ ^[Yy] ]]; }

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
# Running the installer again: stop LivePi first (a running program can't be overwritten). It starts
# again at the end, and also if the installer stops early because of an error.
STOPPED_LIVEPI=0
if systemctl is-active --quiet livepi.service 2>/dev/null; then STOPPED_LIVEPI=1; fi
on_error() {
  echo
  echo "!! The installer stopped early (line $1). Nothing you had is lost."
  if [[ $STOPPED_LIVEPI -eq 1 ]]; then systemctl start livepi.service 2>/dev/null && echo "   LivePi is running again with the previous setup."; fi
  echo "   Run the installer again, or send the lines above to get help."
}
set -o errtrace
trap 'on_error $LINENO' ERR
systemctl stop livepi.service 2>/dev/null || true
for i in $(seq 1 50); do pgrep -x LivePi >/dev/null || break; sleep 0.2; done
pkill -9 -x LivePi 2>/dev/null || true
mkdir -p /opt/livepi
# Copy the program next to the old one and swap it in, so even a still-running copy can't block it
cp "$HERE/LivePi" /opt/livepi/LivePi.new && mv -f /opt/livepi/LivePi.new /opt/livepi/LivePi
cp -r "$HERE/web" "$HERE/set-password.sh" "$HERE/trusted-network.sh" "$HERE/pisound-setup.sh" "$HERE/rt-tune.sh" "$HERE/run.sh" "$HERE/INSTALLER_LEVEL" /opt/livepi/
chmod +x /opt/livepi/LivePi /opt/livepi/set-password.sh /opt/livepi/trusted-network.sh /opt/livepi/pisound-setup.sh /opt/livepi/rt-tune.sh /opt/livepi/run.sh
# The app owns its folder so it can install offline updates from the web page
chown -R "$USER_NAME:$(id -gn "$USER_NAME")" /opt/livepi

echo "== Real-time limits for the audio group"
usermod -aG audio "$USER_NAME"
getent group gpio >/dev/null 2>&1 && usermod -aG gpio "$USER_NAME"   # the Pisound button
cat > /etc/security/limits.d/99-audio.conf <<EOF
@audio - rtprio 95
@audio - memlock unlimited
@audio - nice -10
EOF

echo
echo "== Live-audio tuning"
echo "   These settings protect the audio from dropouts. You can change any of them later on the"
echo "   LivePi page: Settings > System > Reliability (tap an item marked ! to fix it)."
TUNE_SKIP=""
if [[ $TUNING -eq 1 && -t 0 ]]; then
  if yesno_early "Apply all recommended settings? [Y/n]" Y; then
    echo "   All recommended settings will be applied."
  else
    q() {   # q item "question" "note line" ...
      local item="$1" question="$2"; shift 2
      echo
      for l in "$@"; do echo "   $l"; done
      yesno_early "$question [Y/n]" Y || TUNE_SKIP="$TUNE_SKIP,$item"
    }
    q core "Reserve one CPU core for the audio?" \
      "Nothing else ever runs on that core: the strongest protection against dropouts." \
      "Everything else shares the other cores. Takes effect after a reboot."
    v="$(uname -v)"
    if [[ "$v" == *PREEMPT_DYNAMIC* && "$v" != *PREEMPT_RT* ]]; then
      q preempt "Use the faster-responding kernel mode (full preemption)?" \
        "The audio gets the CPU sooner when it needs it. Costs a little CPU overall." \
        "Takes effect after a reboot."
    fi
    q updates "Turn off automatic background OS updates?" \
      "They can start downloading and installing in the middle of a service." \
      "You can still update any time from Settings > System on the LivePi page."
    q cpu "Keep the CPU at full speed?" \
      "No delays while the CPU speeds up when the plugins need it." \
      "Uses a little more power and makes more heat: use a fan or a cooling case."
    q service "Restart LivePi automatically if it ever freezes (watchdog)?" \
      "LivePi always restarts after a crash; this also catches a freeze." \
      "Also makes sure the system never gives up restarting it."
    q usb "Turn off USB power saving?" \
      "Keeps USB audio interfaces awake. Makes no difference for a Pisound or other HAT." \
      "Takes effect after a reboot."
    q wifi "Turn off Wi-Fi power saving?" \
      "Keeps the LivePi page responsive over Wi-Fi. Makes no difference on a wired network."
    if [[ -e /sys/kernel/pisound/id ]] || grep -qi pisound /proc/asound/cards 2>/dev/null; then
      q button "Let LivePi use the Pisound button?" \
        "You choose what each click or hold does in Setup > Settings > Pisound button." \
        "Turns off Blokas' own button program (pisound-btn), if it is installed."
    fi
  fi
fi
TUNE_SKIP="${TUNE_SKIP#,}"

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
if [[ ",$TUNE_SKIP," == *",cpu,"* ]]; then
  systemctl disable livepi-governor.service >/dev/null 2>&1 || true
  echo "   Skipped: the CPU changes speed as usual."
else
  systemctl enable --now livepi-governor.service >/dev/null 2>&1 || true
fi

echo "== Stopping services that fight for the audio device"
systemctl disable --now pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl --global disable pulseaudio.service pulseaudio.socket 2>/dev/null || true
systemctl disable --now bluetooth.service 2>/dev/null || true

echo "== System tuning for live audio"
# A root-owned copy the web page may run (Settings > System > Reliability > Fix now). The copy in
# /opt/livepi belongs to the LivePi user, so it must never be what runs as root.
install -m 755 -o root -g root "$HERE/rt-tune.sh" /usr/local/sbin/livepi-tune
# Network settings from the page (Settings > Network); every change undoes itself unless confirmed
install -m 755 -o root -g root "$HERE/livepi-net" /usr/local/sbin/livepi-net
if [[ $TUNING -eq 1 ]]; then
  /usr/local/sbin/livepi-tune apply ${TUNE_SKIP:+--skip=$TUNE_SKIP} || echo "Tuning did not finish; run later: sudo /usr/local/sbin/livepi-tune apply"
else
  echo "Skipped (--no-tuning)."
fi

echo "== OS update helper (apt) for the web page"
install -m 755 -o root -g root "$HERE/livepi-os" "$HERE/livepi-os-job" /usr/local/sbin/
mkdir -p /var/lib/livepi-os
chmod 755 /var/lib/livepi-os

echo "== Allow the web page to reboot / shut down the Pi and run OS updates"
cat > /etc/sudoers.d/livepi <<EOF
${USER_NAME} ALL=(root) NOPASSWD: /usr/bin/systemctl reboot, /usr/bin/systemctl poweroff, /usr/local/sbin/livepi-os check, /usr/local/sbin/livepi-os upgrade, /usr/local/sbin/livepi-tune apply, /usr/local/sbin/livepi-tune apply --only\=*, /usr/local/sbin/livepi-net set *, /usr/local/sbin/livepi-net confirm *
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

echo "== Audio interface"
if grep -qi pisound /proc/asound/cards 2>/dev/null; then
  echo "A Pisound is fitted and working."
  [[ $PISOUND == ask ]] && PISOUND=yes
elif [[ $PISOUND == ask ]]; then
  [[ -t 0 ]] && echo "   The Pisound's driver is already part of the OS; this switches it on (needs a reboot). Answer N for other sound cards."
  if [[ -t 0 ]] && yesno_early "Will you use a Blokas Pisound? [y/N]" N; then PISOUND=yes; else PISOUND=no; fi
fi
if [[ $PISOUND == yes ]]; then
  /opt/livepi/pisound-setup.sh enable || echo "Pisound setup did not finish; run later: sudo /opt/livepi/pisound-setup.sh"
fi

echo "== LivePi service"
cat > /etc/systemd/system/livepi.service <<EOF
[Unit]
Description=LivePi VST3 host
After=sound.target network-online.target
Wants=network-online.target
# Never give up restarting it, however often it stops
StartLimitIntervalSec=0

[Service]
User=${USER_NAME}
Environment=HOME=/home/${USER_NAME}
WorkingDirectory=/opt/livepi
ExecStart=/opt/livepi/run.sh --port 8080${EDITOR_ARG}
Restart=always
RestartSec=1
# LivePi tells systemd it is alive; if it ever freezes, systemd restarts it (it enables the timeout itself)
NotifyAccess=main
TimeoutStopSec=15
# Under memory pressure the OS stops something else, never the audio
OOMScoreAdjust=-900
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
# Running the installer again: keep the password (and trusted access) you already have
if [[ "$PW_MODE" == ask && -s "$HOME_DIR/.livepi/auth.json" ]] && grep -q '"hash"' "$HOME_DIR/.livepi/auth.json"; then
  if [[ ! -t 0 ]] || yesno "A web password is already set. Keep it (and your trusted access settings)? [Y/n]" Y; then PW_MODE=keep; fi
fi
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
    if [[ -t 0 ]]; then
      echo
      echo "Trusted access: devices on networks you trust skip the password (for example your AV network)."
      if yesno "Turn on trusted access? [y/N]" N; then
        while :; do
          read -r -p "Trusted network or address${NET:+ [$NET]} (Enter = ${NET:-done}): " ADD || ADD=""
          ADD="${ADD:-$NET}"; NET=""
          [[ -z "$ADD" ]] && break
          /opt/livepi/trusted-network.sh add "$ADD" >/dev/null && echo "   added $ADD" || echo "   '$ADD' is not a valid IPv4 address or network (like 192.168.1.0/24)"
          yesno "Add another? [y/N]" N || break
        done
        /opt/livepi/trusted-network.sh on | sed 's/^/   /' || true
      fi
    fi
  else
    echo "No password was set. LivePi will still start; set one later in Setup > Settings."
  fi
elif [[ "$PW_MODE" == keep ]]; then
  echo "Keeping your password and trusted access settings."
else
  echo "Continuing without a password: everyone on your network can use LivePi. Set one any time in Setup > Settings."
fi

systemctl daemon-reload
systemctl enable livepi.service
systemctl restart livepi.service
trap - ERR

# Check that it really runs and starts at boot
echo
echo "== Checking"
sleep 3
if systemctl is-enabled --quiet livepi.service; then echo "   Starts automatically at boot: yes"; else echo "!! LivePi is not set to start at boot. Run: sudo systemctl enable livepi"; fi
if systemctl is-active --quiet livepi.service; then echo "   Running now: yes"; else
  echo "!! LivePi is not running. Its last messages:"
  journalctl -u livepi.service -n 15 --no-pager 2>/dev/null | sed 's/^/   /' || true
fi

echo
echo "Done. Open http://$(hostname).local:8080 from any device on the network."
echo "Upload your Linux ARM64 plugins from the Setup tab (zip each .vst3 folder)."
echo "Reboot once so the audio-group limits take effect."
