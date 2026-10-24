#!/usr/bin/env bash
# Sets up a Blokas Pisound for LivePi.
#   sudo /opt/livepi/pisound-setup.sh            enable (default)
#   sudo /opt/livepi/pisound-setup.sh disable    remove what this script added
#   sudo /opt/livepi/pisound-setup.sh status     show whether the Pisound is found
#
# The Pisound driver is part of the Raspberry Pi kernel, so nothing is downloaded.
# The board's EEPROM normally loads it by itself; this adds the overlays explicitly
# (pisound, plus pisound-pi5 on a Raspberry Pi 5) so it also works when that fails.
# The Pisound button/app software (pisound-btn) is not needed for LivePi.
set -euo pipefail

CMD="${1:-enable}"
CFG=/boot/firmware/config.txt; [[ -f $CFG ]] || CFG=/boot/config.txt
OVL="$(dirname "$CFG")/overlays"
BEGIN="# --- LivePi: Pisound (added by pisound-setup.sh) ---"
END="# --- end LivePi: Pisound ---"

found() { grep -qi pisound /proc/asound/cards 2>/dev/null; }

status() {
  if found; then
    echo "Pisound: found"; grep -i -A1 pisound /proc/asound/cards | sed 's/^/  /'
  else
    echo "Pisound: not found (is it fitted? did you reboot after setup?)"
  fi
  if [[ -r /proc/device-tree/hat/product ]]; then echo "HAT EEPROM says: $(tr -d '\0' < /proc/device-tree/hat/product)"; fi
  grep -q "$BEGIN" "$CFG" && echo "LivePi Pisound lines: present in $CFG" || echo "LivePi Pisound lines: not in $CFG"
}

remove_block() {
  if grep -q "$BEGIN" "$CFG"; then
    sed -i "/$(printf '%s' "$BEGIN" | sed 's/[][\/.*^$]/\\&/g')/,/$(printf '%s' "$END" | sed 's/[][\/.*^$]/\\&/g')/d" "$CFG"
  fi
}

case "$CMD" in
  status) status; exit 0 ;;
  enable|disable) ;;
  *) echo "usage: sudo $0 [enable|disable|status]"; exit 2 ;;
esac

[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
[[ -f $CFG ]] || { echo "Cannot find config.txt"; exit 1; }

cp -n "$CFG" "$CFG.livepi-backup" 2>/dev/null || true
remove_block

if [[ $CMD == enable ]]; then
  [[ -f $OVL/pisound.dtbo ]] || { echo "This kernel has no pisound overlay ($OVL/pisound.dtbo). Update the OS (sudo apt full-upgrade) and try again."; exit 1; }
  {
    echo "$BEGIN"
    echo "[all]"
    echo "dtoverlay=pisound"
    if [[ -f $OVL/pisound-pi5.dtbo ]]; then
      echo "[pi5]"
      echo "dtoverlay=pisound-pi5"
      echo "[all]"
    fi
    echo "$END"
  } >> "$CFG"
  echo "Pisound enabled in $CFG. Reboot, then pick it in LivePi: gear > Audio > Driver ALSA, Input/Output pisound."
else
  echo "Pisound lines removed from $CFG. Reboot to apply."
fi
