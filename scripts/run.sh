#!/bin/sh
# Starts LiveHost. If an update was just installed and the new version keeps failing
# to start, this puts the previous version back automatically.
D="$(cd "$(dirname "$0")" && pwd)"
DATA="$HOME/.livehost"
M="$DATA/update-pending"

if [ -f "$M" ]; then
  n=$(cat "$M" 2>/dev/null)
  n=$((n + 1))
  if [ "$n" -gt 3 ] && [ -f "$D/LiveHost.prev" ]; then
    echo "LiveHost: the update failed to start $((n - 1)) times, rolling back"
    mv -f "$D/LiveHost" "$D/LiveHost.failed"
    mv -f "$D/LiveHost.prev" "$D/LiveHost"
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

exec "$D/LiveHost" "$@"
