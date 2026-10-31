#!/usr/bin/env bash
# Tunes Raspberry Pi OS for live audio with LivePi (the installer runs this).
#   sudo /opt/livepi/rt-tune.sh            apply (default)
#   sudo /opt/livepi/rt-tune.sh status     show what is set
#   sudo /opt/livepi/rt-tune.sh undo       put the boot settings back and re-enable automatic updates
#
# What it does:
#  - Reserves the last CPU core for the audio (isolcpus): the kernel runs nothing else there, and
#    LivePi pins its real-time audio thread to it.
#  - Full kernel preemption (preempt=full) when the kernel supports switching at boot, so the audio
#    thread gets the CPU faster.
#  - USB power saving off (usbcore.autosuspend=-1): USB audio interfaces never get suspended.
#  - Automatic background OS updates off (apt-daily, man-db timers): they can load the Pi during a
#    service. Update from the LivePi page instead (Settings > System > Operating system).
#  - Wi-Fi power saving off, so the web page stays responsive.
#  - ModemManager off (it probes serial/USB devices at random times).
# Boot settings take effect after a reboot. A backup of cmdline.txt is kept next to it.
set -euo pipefail

CMD="${1:-apply}"
CMDLINE="${LIVEPI_CMDLINE:-/boot/firmware/cmdline.txt}"; [[ -f $CMDLINE ]] || CMDLINE=/boot/cmdline.txt
MARK=/etc/livepi-tuning
NM_CONF=/etc/NetworkManager/conf.d/livepi-wifi-powersave.conf
TIMERS="apt-daily.timer apt-daily-upgrade.timer man-db.timer"

ncpu=$(nproc --all 2>/dev/null || echo 1)
want_tokens() {
  local t="usbcore.autosuspend=-1"
  if (( ncpu >= 4 )); then t="$t isolcpus=$((ncpu - 1))"; fi
  local v; v="$(uname -v)"
  if [[ "$v" == *PREEMPT_DYNAMIC* && "$v" != *PREEMPT_RT* ]]; then t="$t preempt=full"; fi
  echo "$t"
}

status() {
  echo "Kernel: $(uname -r) $(uname -v)"
  echo "Boot options ($CMDLINE): $(cat "$CMDLINE" 2>/dev/null)"
  echo "Running with: $(cat /proc/cmdline)"
  echo "Reserved CPU core(s): $(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)"
  echo "USB autosuspend: $(cat /sys/module/usbcore/parameters/autosuspend 2>/dev/null || echo '?')"
  echo "CPU governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '?')"
  for t in $TIMERS; do echo "$t: $(systemctl is-enabled "$t" 2>/dev/null || echo absent)"; done
  [[ -f $MARK ]] && echo "LivePi tuning: $(cat "$MARK")" || echo "LivePi tuning: not applied"
}

# Rewrites the single line of cmdline.txt: removes any earlier value of each key we set, adds ours.
# Refuses to write anything that does not still look like a bootable line.
set_cmdline() {
  local add="$1" remove_keys="$2" line new
  line="$(head -n1 "$CMDLINE")"
  new=""
  for tok in $line; do
    local key="${tok%%=*}" skip=0
    for k in $remove_keys; do [[ "$key" == "$k" ]] && skip=1; done
    (( skip )) || new="$new $tok"
  done
  for tok in $add; do new="$new $tok"; done
  new="${new# }"
  if [[ -z "$new" || "$new" != *root=* ]]; then
    echo "Refusing to change $CMDLINE (the result would not boot)."; return 1
  fi
  if [[ "$new" != "$line" ]]; then
    cp -n "$CMDLINE" "$CMDLINE.livepi-backup" 2>/dev/null || true
    printf '%s\n' "$new" > "$CMDLINE.livepi-new"
    mv "$CMDLINE.livepi-new" "$CMDLINE"
    sync
    REBOOT=1
  fi
}

[[ $CMD == status ]] && { status; exit 0; }
[[ $CMD == apply || $CMD == undo ]] || { echo "usage: sudo $0 [apply|status|undo]"; exit 2; }
[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
REBOOT=0

if [[ $CMD == apply ]]; then
  TOK="$(want_tokens)"
  if [[ -f $CMDLINE ]]; then
    set_cmdline "$TOK" "usbcore.autosuspend isolcpus preempt" || true
  else
    echo "No cmdline.txt found: boot options not changed."
  fi
  for t in $TIMERS; do systemctl disable --now "$t" >/dev/null 2>&1 || true; done
  systemctl disable --now unattended-upgrades.service >/dev/null 2>&1 || true
  systemctl disable --now ModemManager.service >/dev/null 2>&1 || true
  if [[ -d /etc/NetworkManager/conf.d ]]; then
    printf '[connection]\n# LivePi: keep Wi-Fi awake so the web page stays responsive\nwifi.powersave = 2\n' > "$NM_CONF"
    systemctl reload NetworkManager >/dev/null 2>&1 || true
  fi
  echo "applied $(date -Iseconds); boot options: $TOK" > "$MARK"
  echo "LivePi tuning applied (boot options: $TOK)."
else
  [[ -f $CMDLINE ]] && set_cmdline "" "usbcore.autosuspend isolcpus preempt" || true
  for t in $TIMERS; do systemctl enable --now "$t" >/dev/null 2>&1 || true; done
  rm -f "$NM_CONF" "$MARK"
  echo "LivePi tuning removed."
fi
(( REBOOT )) && echo "Reboot to use the new boot options: sudo reboot"
exit 0
