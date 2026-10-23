#!/usr/bin/env bash
# Set or change the LivePi web password.
#   sudo /opt/livepi/set-password.sh [username]
# For scripted installs you can pass it as LIVEPI_PASSWORD=... in the environment.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "Run with sudo"; exit 1; fi

USER_NAME="${1:-}"
if [[ -z "$USER_NAME" && -f /etc/systemd/system/livepi.service ]]; then
  USER_NAME="$(sed -n 's/^User=//p' /etc/systemd/system/livepi.service | head -n1)"
fi
USER_NAME="${USER_NAME:-pi}"
id "$USER_NAME" >/dev/null 2>&1 || { echo "User '$USER_NAME' does not exist. Usage: sudo $0 <username>"; exit 1; }

PASS="${LIVEPI_PASSWORD:-}"
if [[ -z "$PASS" ]]; then
  echo "Choose the password for the LivePi web page. Nothing is shown while you type."
  for attempt in 1 2 3; do
    read -r -s -p "New password (at least 8 characters): " PASS; echo
    read -r -s -p "Repeat it: " PASS2; echo
    if (( ${#PASS} < 8 )); then echo "That password is too short - use at least 8 characters."; PASS=""; continue; fi
    if [[ "$PASS" != "$PASS2" ]]; then echo "The two entries do not match."; PASS=""; continue; fi
    break
  done
  [[ -n "$PASS" ]] || { echo "No password was set."; exit 1; }
fi
if (( ${#PASS} < 8 )); then echo "Password must be at least 8 characters."; exit 1; fi

HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"
GROUP_NAME="$(id -gn "$USER_NAME")"
DIR="$HOME_DIR/.livepi"
install -d -o "$USER_NAME" -g "$GROUP_NAME" -m 700 "$DIR"

SALT="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
HASH="$(printf '%s%s' "$SALT" "$PASS" | sha256sum | cut -d' ' -f1)"
( umask 077; printf '{ "salt": "%s", "hash": "%s" }\n' "$SALT" "$HASH" > "$DIR/auth.json" )
chown "$USER_NAME:$GROUP_NAME" "$DIR/auth.json"

systemctl try-restart livepi.service 2>/dev/null || true

echo "Password set. When the browser asks for a login, leave the user name blank (or type anything) and enter the password."
