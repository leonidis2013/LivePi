#!/usr/bin/env bash
# Choose networks that can use LivePi WITHOUT a password (everyone else still needs one, if set).
#   sudo /opt/livepi/trusted-network.sh add 192.168.10.0/24     # a whole network
#   sudo /opt/livepi/trusted-network.sh add 192.168.10.25       # a single computer
#   sudo /opt/livepi/trusted-network.sh remove 192.168.10.0/24
#   sudo /opt/livepi/trusted-network.sh list
#   sudo /opt/livepi/trusted-network.sh clear
#   sudo /opt/livepi/trusted-network.sh on      # trusted access ON: these networks skip the password
#   sudo /opt/livepi/trusted-network.sh off     # trusted access OFF: everyone must log in (default)
# Trusted access only matters while a password is set and protection is on.
# Only use a network you control (for example a dedicated AV VLAN). Anyone on it can upload
# plugins (native code) and reboot the Pi. IPv4 only; networks wider than /8 are refused.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "Run with sudo"; exit 1; fi

CMD="${1:-list}"; ARG="${2:-}"
USER_NAME="${LIVEPI_USER:-}"
if [[ -z "$USER_NAME" && -f /etc/systemd/system/livepi.service ]]; then
  USER_NAME="$(sed -n 's/^User=//p' /etc/systemd/system/livepi.service | head -n1)"
fi
USER_NAME="${USER_NAME:-pi}"
id "$USER_NAME" >/dev/null 2>&1 || { echo "User '$USER_NAME' does not exist (set LIVEPI_USER=...)."; exit 1; }

HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"
GROUP_NAME="$(id -gn "$USER_NAME")"
DIR="${LIVEPI_DIR:-$HOME_DIR/.livepi}"
FILE="$DIR/trusted.txt"
install -d -o "$USER_NAME" -g "$GROUP_NAME" -m 700 "$DIR"
touch "$FILE"

valid() {
  local a="${1%%/*}" p=32
  [[ "$1" == */* ]] && p="${1#*/}"
  [[ "$a" =~ ^([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})\.([0-9]{1,3})$ ]] || return 1
  for i in 1 2 3 4; do (( 10#${BASH_REMATCH[$i]} <= 255 )) || return 1; done
  [[ "$p" =~ ^[0-9]{1,2}$ ]] && (( 10#$p >= 8 && 10#$p <= 32 ))
}

case "$CMD" in
  add)
    valid "$ARG" || { echo "Give an IPv4 address or network like 192.168.10.0/24 (prefix 8 to 32)."; exit 1; }
    grep -qxF "$ARG" "$FILE" || echo "$ARG" >> "$FILE"
    ;;
  remove)
    [[ -n "$ARG" ]] || { echo "Say which entry to remove."; exit 1; }
    grep -vxF "$ARG" "$FILE" > "$FILE.new" || true
    mv "$FILE.new" "$FILE"
    ;;
  clear) : > "$FILE" ;;
  on|off|lock|open)
    echo "$( [[ $CMD == on || $CMD == lock ]] && echo on || echo off )" > "$DIR/access.txt"
    chown "$USER_NAME:$GROUP_NAME" "$DIR/access.txt"; chmod 600 "$DIR/access.txt"
    ;;
  list) ;;
  *) echo "usage: $0 add|remove|list|clear|on|off [network]"; exit 2 ;;
esac

chown "$USER_NAME:$GROUP_NAME" "$FILE"; chmod 600 "$FILE"
A=$(cat "$DIR/access.txt" 2>/dev/null || echo off)
echo "Trusted access: $( [[ "$A" == on || "$A" == locked ]] && echo "ON (these networks skip the password)" || echo "OFF (everyone must log in when a password is on)" )"
echo "Trusted networks (no password needed from these):"
if [[ -s "$FILE" ]]; then sed 's/^/  /' "$FILE"; else echo "  (none)"; fi
[[ "$CMD" == list ]] || { systemctl try-restart livepi.service 2>/dev/null || true; echo "LivePi restarted."; }
