#!/usr/bin/env bash
# Runs inside a clean Debian container (see .github/workflows/compat.yml):
# installs LiveHost's runtime packages exactly like install-pi.sh, starts it with no sound card
# (so it uses the virtual one) and checks the web API, meters, monitor mix and streaming.
set -u
cd /work
RES=/work/result.txt; : > "$RES"
say()  { echo "$*" | tee -a "$RES"; }
FAIL=0
check() { if eval "$2"; then say "PASS  $1"; else say "FAIL  $1"; FAIL=1; fi; }

. /etc/os-release; say "== $PRETTY_NAME ($(uname -m)), glibc $(ldd --version | head -n1 | awk '{print $NF}')"
export DEBIAN_FRONTEND=noninteractive
if apt-get update -qq >/tmp/apt-update.log 2>&1; then say "PASS  apt-get update"; else say "FAIL  apt-get update"; tail -n 5 /tmp/apt-update.log | tee -a "$RES"; fi
# same package line as scripts/install-pi.sh
PKGS=$(sed -n 's/^apt-get install -y \(libfreetype6.*\)$/\1/p' scripts/install-pi.sh | head -n1)
check "installer package list found" "[ -n '$PKGS' ]"
if apt-get install -y -qq $PKGS jq procps xvfb unzip >/tmp/apt-install.log 2>&1; then say "PASS  runtime packages install"; else say "FAIL  runtime packages install"; tail -n 5 /tmp/apt-install.log | tee -a "$RES"; FAIL=1; fi
if apt-get install -y -qq libasound2 >/dev/null 2>&1; then say "PASS  libasound2 installs"
elif apt-get install -y -qq libasound2t64 >/dev/null 2>&1; then say "PASS  libasound2t64 installs (renamed package)"
else say "FAIL  libasound install"; FAIL=1; fi
if apt-get install -y -qq xvfb x11vnc novnc websockify fonts-dejavu-core libgl1 >/tmp/apt-viewer.log 2>&1; then say "PASS  plugin window packages install"; else say "FAIL  plugin window packages install"; tail -n 5 /tmp/apt-viewer.log | tee -a "$RES"; FAIL=1; fi

rm -rf /tmp/lh && mkdir /tmp/lh && cd /tmp/lh
ls /work/*.zip | tee -a "$RES"
unzip -q /work/livehost-build-*.zip || say "FAIL  unzip"
cd livehost || { say "FAIL  no livehost directory in the zip"; exit 1; }
check "binary has no missing libraries" "! ldd ./LiveHost | grep -q 'not found'"
[ -s manifest.json ] && say "manifest: $(cat manifest.json)"

api()  { curl -s -m 5 "http://127.0.0.1:8080$1"; }
post() { curl -s -m 5 -X POST -H 'X-LiveHost: 1' -H 'Content-Type: application/json' -d "$2" "http://127.0.0.1:8080$1"; }

run_case() {
  local label="$1"
  say "-- case: $label"
  rm -rf /root/.livehost
  mkdir -p /root/.livehost && echo "127.0.0.0/8" > /root/.livehost/trusted.txt   # the test client is local
  rm -f /tmp/lh.rc
  ( cd /tmp/lh/livehost && HOME=/root ./LiveHost > /tmp/lh.log 2>&1 & echo $! > /tmp/lh.pid; wait $!; echo $? > /tmp/lh.rc ) &
  for i in $(seq 1 30); do api /api/info | jq -e .version >/dev/null 2>&1 && break; sleep 1; done
  check "$label: web API answers" "api /api/info | jq -e .version >/dev/null"
  if ! api /api/info | jq -e .version >/dev/null 2>&1; then say "---- log"; tail -n 25 /tmp/lh.log | tee -a "$RES"; return; fi
  sleep 2
  say "state: $(api /api/state | jq -c '{deviceName,sampleRate,bufferSize,virtualDevice,xruns,cpu}')"
  check "$label: falls back to the virtual sound card" "api /api/state | jq -e '.virtualDevice==true' >/dev/null"
  post /api/device '{"type":"Virtual (no audio hardware)","input":"Pink noise","output":"Discard (no output)","sampleRate":48000,"bufferSize":128}' >/dev/null
  post /api/mixer '{"channel":0,"sendDb":0}' >/dev/null
  post /api/monitorbus '{"outputs":[2]}' >/dev/null
  sleep 3
  M=$(api /api/meters); say "meters: $(echo "$M" | jq -c '{in:.in,out:.out,mon:.mon}')"
  check "$label: channel input meter moves" "echo '$M' | jq -e '.in[0] > -60' >/dev/null"
  check "$label: channel output meter moves" "echo '$M' | jq -e '.out[0] > -60' >/dev/null"
  check "$label: monitor mix receives the send" "echo '$M' | jq -e '.mon.in > -60' >/dev/null"
  X=$(api /api/state | jq '.xruns // 0'); say "xruns after 3 s: $X"
  check "$label: no more than 2 xruns" "[ '$X' -le 2 ]"
  post /api/monitor '{"channel":100,"post":true}' >/dev/null
  B=$(curl -s -m 2 http://127.0.0.1:8080/api/monitor/stream | wc -c); say "stream bytes in 2 s: $B (48 kHz mono 16-bit would be about 192000)"
  check "$label: monitor stream delivers audio" "[ '$B' -gt 100000 ]"
  sleep 1
  check "$label: update status endpoint" "api /api/update/status | jq -e .state >/dev/null"
  check "$label: OS status endpoint" "api /api/os/status | jq -e .state >/dev/null"
  check "$label: still running" "kill -0 \$(cat /tmp/lh.pid)"
  V=$(api /api/editor/viewer); say "viewer: $(echo "$V" | jq -c '{available,displaySource,vnc,web,webReachable,problem}')"
  check "$label: plugin window viewer running (Xvfb + x11vnc + websockify)" "echo '$V' | jq -e '.available and .vnc==\"running\" and .web==\"running\" and .webReachable' >/dev/null"
  check "$label: noVNC page served on port 6080" "curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:6080/vnc.html | grep -q 200"
  check "$label: state reports editorAvailable" "api /api/state | jq -e '.editorAvailable==true' >/dev/null"
  local pid; pid=$(cat /tmp/lh.pid)
  say "signals: $(grep -E 'SigCgt|SigBlk|SigIgn' /proc/$pid/status | tr '\n' ' ') (SIGTERM caught = bit 15 of SigCgt, i.e. 0x4000)"
  say "threads: $(ls /proc/$pid/task | wc -l)"
  kill -TERM "$pid" 2>/dev/null
  for i in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  check "$label: stops cleanly on SIGTERM within 5 s (what systemd sends)" "! kill -0 $pid 2>/dev/null"
  sleep 1; say "exit code: $(cat /tmp/lh.rc 2>/dev/null)"
  check "$label: exit code 0" "[ \"$(cat /tmp/lh.rc 2>/dev/null)\" = 0 ]"
  check "$label: viewer helpers stopped with it (ports 5900/6080 closed)" "! (exec 3<>/dev/tcp/127.0.0.1/6080) 2>/dev/null && ! (exec 3<>/dev/tcp/127.0.0.1/5900) 2>/dev/null"
  say "viewer log: $(grep -v 'keysym\|xkbcomp\|XKEYBOARD' /root/.livehost/viewer.log 2>/dev/null | tail -n 4 | tr '\n' '|')"
  pkill -9 LiveHost 2>/dev/null; sleep 1
  say "log tail:"; tail -n 6 /tmp/lh.log | tee -a "$RES"
}

unset DISPLAY
run_case "no DISPLAY given (LiveHost starts its own virtual screen)"
check "self-started Xvfb :99 is up" "[ -S /tmp/.X11-unix/X99 ]"
export DISPLAY=:99
run_case "DISPLAY given (:99)"

say "RESULT: $([ $FAIL -eq 0 ] && echo ALL PASSED || echo SOME FAILED)"
