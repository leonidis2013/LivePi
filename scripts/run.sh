#!/bin/sh
# Starts LivePi. If an update was just installed and the new version keeps failing
# to start, this puts the previous version back automatically.
D="$(cd "$(dirname "$0")" && pwd)"
DATA="$HOME/.livepi"
M="$DATA/update-pending"

if [ -f "$M" ]; then
  n=$(cat "$M" 2>/dev/null)
  n=$((n + 1))
  if [ "$n" -gt 3 ] && [ -f "$D/LivePi.prev" ]; then
    echo "LivePi: the update failed to start $((n - 1)) times, rolling back"
    mv -f "$D/LivePi" "$D/LivePi.failed"
    mv -f "$D/LivePi.prev" "$D/LivePi"
    if [ -d "$D/web.prev" ]; then
      rm -rf "$D/web.failed"
      mv "$D/web" "$D/web.failed"
      mv "$D/web.prev" "$D/web"
    fi
    rm -f "$M"
    : > "$DATA/rolled-back"
  else
    echo "$n" > "$M"
  fi
fi

exec "$D/LivePi" "$@"
