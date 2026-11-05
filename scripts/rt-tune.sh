#!/usr/bin/env bash
# Tunes Raspberry Pi OS for live audio with LivePi (the installer runs this).
#   sudo /opt/livepi/rt-tune.sh            apply everything (default)
#   sudo /opt/livepi/rt-tune.sh apply --only=core,updates     apply only these items
#   sudo /opt/livepi/rt-tune.sh apply --skip=wifi             apply everything except these
#     items: core preempt usb updates cpu wifi service
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
#  - CPU fixed at full speed.
#  - Wi-Fi power saving off, so the web page stays responsive.
#  - ModemManager off (it probes serial/USB devices at random times).
#  - The LivePi service: never stop restarting it, restart within 1 s, let LivePi use the systemd
#    watchdog, and never pick LivePi when memory runs out (a drop-in, takes effect when LivePi restarts).
# Boot settings take effect after a reboot. A backup of cmdline.txt is kept next to it.
set -euo pipefail

CMD="${1:-apply}"
ALL="core preempt usb updates cpu wifi service"
ITEMS="$ALL"
for arg in "${@:2}"; do
  case "$arg" in
    --only=*) ITEMS="$(echo "${arg#--only=}" | tr ',' ' ')" ;;
    --skip=*) for i in $(echo "${arg#--skip=}" | tr ',' ' '); do ITEMS="$(echo " $ITEMS " | sed "s/ $i / /g")"; done ;;
    *) echo "Unknown option: $arg"; exit 2 ;;
  esac
done
ITEMS="$(echo $ITEMS)"
for i in $ITEMS; do [[ " $ALL " == *" $i "* ]] || { echo "Unknown item: $i (items: $ALL)"; exit 2; }; done
has() { [[ " $ITEMS " == *" $1 "* ]]; }
CMDLINE="${LIVEPI_CMDLINE:-/boot/firmware/cmdline.txt}"; [[ -f $CMDLINE ]] || CMDLINE=/boot/cmdline.txt
MARK=/etc/livepi-tuning
PREV=/etc/livepi-tuning.before   # what was set before LivePi changed anything (for undo)
NM_CONF=/etc/NetworkManager/conf.d/livepi-wifi-powersave.conf
DROPIN=/etc/systemd/system/livepi.service.d/10-reliability.conf
TIMERS="apt-daily.timer apt-daily-upgrade.timer man-db.timer"

ncpu=$(nproc --all 2>/dev/null || echo 1)
preempt_supported() { local v; v="$(uname -v)"; [[ "$v" == *PREEMPT_DYNAMIC* && "$v" != *PREEMPT_RT* ]]; }
# Boot options for the chosen items, and the keys they replace
want_tokens() {
  local t=""
  has usb && t="$t usbcore.autosuspend=-1"
  has core && (( ncpu >= 4 )) && t="$t isolcpus=$((ncpu - 1))"
  has preempt && preempt_supported && t="$t preempt=full"
  echo "${t# }"
}
want_keys() {
  local k=""
  has usb && k="$k usbcore.autosuspend"
  has core && (( ncpu >= 4 )) && k="$k isolcpus"
  has preempt && preempt_supported && k="$k preempt"
  echo "${k# }"
}
# LivePi's boot options currently in cmdline.txt (for the page's "reboot to finish" check)
ours_in_cmdline() { local t=""; for tok in $(head -n1 "$CMDLINE" 2>/dev/null); do case "$tok" in usbcore.autosuspend=-1|isolcpus=*|preempt=full) t="$t $tok";; esac; done; echo "${t# }"; }

status() {
  echo "Kernel: $(uname -r) $(uname -v)"
  echo "Boot options ($CMDLINE): $(cat "$CMDLINE" 2>/dev/null)"
  echo "Running with: $(cat /proc/cmdline)"
  echo "Reserved CPU core(s): $(cat /sys/devices/system/cpu/isolated 2>/dev/null || true)"
  echo "USB autosuspend: $(cat /sys/module/usbcore/parameters/autosuspend 2>/dev/null || echo '?')"
  echo "CPU governor: $(cat /sys/devices/system/cpu/cpu0/cpufreq/scaling_governor 2>/dev/null || echo '?')"
  for t in $TIMERS; do st="$(systemctl is-enabled "$t" 2>/dev/null | head -n1 || true)"; echo "$t: ${st:-absent}"; done
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
    # Write a new file, check it reads back exactly, and only then replace the original
    # (a full /boot partition must never leave an empty cmdline.txt behind)
    if printf '%s\n' "$new" > "$CMDLINE.livepi-new" && sync && [[ "$(head -n1 "$CMDLINE.livepi-new")" == "$new" ]]; then
      mv "$CMDLINE.livepi-new" "$CMDLINE" && sync && REBOOT=1
    else
      rm -f "$CMDLINE.livepi-new"
      echo "Could not write $CMDLINE (is /boot full?). Boot options not changed."
      return 1
    fi
  fi
}

[[ $CMD == status ]] && { status; exit 0; }
[[ $CMD == apply || $CMD == undo ]] || { echo "usage: sudo $0 [apply|status|undo]"; exit 2; }
[[ $EUID -eq 0 ]] || { echo "Run with sudo"; exit 1; }
REBOOT=0

UNITS="$TIMERS unattended-upgrades.service ModemManager.service"
if [[ $CMD == apply ]]; then
  # Remember the original state once, so undo can put back exactly that
  if [[ ! -f $PREV ]]; then
    {
      echo "cmdline_keys=$(for tok in $(head -n1 "$CMDLINE" 2>/dev/null); do case "${tok%%=*}" in usbcore.autosuspend|isolcpus|preempt) printf '%s ' "$tok";; esac; done)"
      for u in $UNITS; do st="$(systemctl is-enabled "$u" 2>/dev/null | head -n1 || true)"; echo "unit $u ${st:-absent}"; done
    } > "$PREV"
  fi
  TOK="$(want_tokens)"; KEYS="$(want_keys)"
  if [[ -n $KEYS ]]; then
    if [[ -f $CMDLINE ]]; then
      set_cmdline "$TOK" "$KEYS" || true
    else
      echo "No cmdline.txt found: boot options not changed."
    fi
  fi
  if has cpu; then
    # CPU at full speed now and at every boot
    for g in /sys/devices/system/cpu/cpu*/cpufreq/scaling_governor; do [[ -e $g ]] && { echo performance > "$g" 2>/dev/null || true; }; done
    systemctl enable livepi-governor.service >/dev/null 2>&1 || true
  fi
  if has updates; then
    for t in $TIMERS; do systemctl disable --now "$t" >/dev/null 2>&1 || true; done
    systemctl disable --now unattended-upgrades.service >/dev/null 2>&1 || true
    systemctl disable --now ModemManager.service >/dev/null 2>&1 || true
  fi
  if has wifi && [[ -d /etc/NetworkManager/conf.d ]]; then
    printf '[connection]\n# LivePi: keep Wi-Fi awake so the web page stays responsive\nwifi.powersave = 2\n' > "$NM_CONF"
    systemctl reload NetworkManager >/dev/null 2>&1 || true
  fi
  if has service && [[ -f /etc/systemd/system/livepi.service ]]; then
    mkdir -p "$(dirname "$DROPIN")"
    cat > "$DROPIN" <<'UNIT'
# Added by LivePi rt-tune.sh
[Unit]
StartLimitIntervalSec=0

[Service]
Restart=always
RestartSec=1
NotifyAccess=main
TimeoutStopSec=15
OOMScoreAdjust=-900
LimitRTPRIO=95
LimitMEMLOCK=infinity
UNIT
    systemctl daemon-reload >/dev/null 2>&1 || true
    systemctl enable livepi.service >/dev/null 2>&1 || true     # and start at every boot
    REBOOT=1
  fi
  echo "applied $(date -Iseconds); boot options: $(ours_in_cmdline)" > "$MARK"
  echo "LivePi tuning applied: $ITEMS"
else
  ORIG=""; [[ -f $PREV ]] && ORIG="$(sed -n 's/^cmdline_keys=//p' "$PREV")"
  [[ -f $CMDLINE ]] && { set_cmdline "$ORIG" "usbcore.autosuspend isolcpus preempt" || true; }
  if [[ -f $PREV ]]; then
    while read -r kw u state; do
      [[ $kw == unit && $state == enabled ]] && systemctl enable --now "$u" >/dev/null 2>&1 || true
    done < "$PREV"
  else
    for t in $TIMERS; do systemctl enable --now "$t" >/dev/null 2>&1 || true; done
  fi
  rm -f "$NM_CONF" "$MARK" "$PREV" "$DROPIN"
  systemctl daemon-reload >/dev/null 2>&1 || true
  echo "LivePi tuning removed."
fi
(( REBOOT )) && echo "Reboot to finish: sudo reboot"
exit 0
