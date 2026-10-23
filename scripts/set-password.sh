#!/usr/bin/env bash
# Set or change the LivePi web password (also possible on the page: Setup > Settings > Password).
#   sudo /opt/livepi/set-password.sh [username]          set / change it (turns protection on)
#   sudo /opt/livepi/set-password.sh --off [username]    turn password protection off (forgot it? use this)
#   sudo /opt/livepi/set-password.sh --on  [username]    turn it back on (asks for the password to use)
# For scripted installs you can pass it as LIVEPI_PASSWORD=... in the environment.
set -euo pipefail

if [[ $EUID -ne 0 ]]; then echo "Run with sudo"; exit 1; fi

MODE=set
if [[ "${1:-}" == --off ]]; then MODE=off; shift; elif [[ "${1:-}" == --on ]]; then shift; fi   # --on = set it again
USER_NAME="${1:-}"
if [[ -z "$USER_NAME" && -f /etc/systemd/system/livepi.service ]]; then
  USER_NAME="$(sed -n 's/^User=//p' /etc/systemd/system/livepi.service | head -n1)"
fi
USER_NAME="${USER_NAME:-pi}"
id "$USER_NAME" >/dev/null 2>&1 || { echo "User '$USER_NAME' does not exist. Usage: sudo $0 <username>"; exit 1; }

if [[ $MODE != set ]]; then
  HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"
  F="$HOME_DIR/.livepi/auth.json"
  [[ -s "$F" ]] || { echo "No password is set."; exit 0; }
  SALT="$(sed -n 's/.*"salt"[^"]*"\([^"]*\)".*/\1/p' "$F")"
  HASH="$(sed -n 's/.*"hash"[^"]*"\([^"]*\)".*/\1/p' "$F")"
  ( umask 077; printf '{ "salt": "%s", "hash": "%s", "enabled": false }\n' "$SALT" "$HASH" > "$F" )
  chown "$USER_NAME:$(id -gn "$USER_NAME")" "$F"
  systemctl try-restart livepi.service 2>/dev/null || true
  echo "Password protection is now OFF. Turn it back on in Setup > Settings or with: sudo $0 --on"
  exit 0
fi

PASS="${LIVEPI_PASSWORD:-}"
if [[ -z "$PASS" ]]; then
  echo "Choose the password for the LivePi web page. Nothing is shown while you type."
  for attempt in 1 2 3; do
    read -r -s -p "New password (at least 4 characters): " PASS; echo
    read -r -s -p "Repeat it: " PASS2; echo
    if (( ${#PASS} < 4 )); then echo "That password is too short - use at least 4 characters."; PASS=""; continue; fi
    if [[ "$PASS" != "$PASS2" ]]; then echo "The two entries do not match."; PASS=""; continue; fi
    break
  done
  [[ -n "$PASS" ]] || { echo "No password was set."; exit 1; }
fi
if (( ${#PASS} < 4 )); then echo "Password must be at least 4 characters."; exit 1; fi

HOME_DIR="$(getent passwd "$USER_NAME" | cut -d: -f6)"
GROUP_NAME="$(id -gn "$USER_NAME")"
DIR="$HOME_DIR/.livepi"
install -d -o "$USER_NAME" -g "$GROUP_NAME" -m 700 "$DIR"

SALT="$(head -c 16 /dev/urandom | od -An -tx1 | tr -d ' \n')"
HASH="$(printf '%s%s' "$SALT" "$PASS" | sha256sum | cut -d' ' -f1)"
( umask 077; printf '{ "salt": "%s", "hash": "%s", "enabled": true }\n' "$SALT" "$HASH" > "$DIR/auth.json" )
rm -f "$DIR/sessions.json"   # everyone logs in again with the new password
chown "$USER_NAME:$GROUP_NAME" "$DIR/auth.json"

systemctl try-restart livepi.service 2>/dev/null || true

echo "Password set. The page now asks for it (no user name needed)."
