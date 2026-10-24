#!/usr/bin/env bash
# Runs inside a clean Debian container (see .github/workflows/compat.yml):
# installs LivePi's runtime packages exactly like install-pi.sh, starts it with no sound card
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
if apt-get install -y -qq xvfb libxtst6 fonts-dejavu-core libgl1 >/tmp/apt-viewer.log 2>&1; then say "PASS  plugin window packages install"; else say "FAIL  plugin window packages install"; tail -n 5 /tmp/apt-viewer.log | tee -a "$RES"; FAIL=1; fi

rm -rf /tmp/lh && mkdir /tmp/lh && cd /tmp/lh
ls /work/*.zip | tee -a "$RES"
unzip -q /work/livepi-build-*.zip || say "FAIL  unzip"
cd livepi || { say "FAIL  no livepi directory in the zip"; exit 1; }
check "package has pisound-setup.sh" "[ -x pisound-setup.sh ] && bash -n pisound-setup.sh"
check "binary has no missing libraries" "! ldd ./LivePi | grep -q 'not found'"
[ -s manifest.json ] && say "manifest: $(cat manifest.json)"

api()  { curl -s -m 5 "http://127.0.0.1:8080$1"; }
post() { curl -s -m 5 -X POST -H 'X-LivePi: 1' -H 'Content-Type: application/json' -d "$2" "http://127.0.0.1:8080$1"; }

run_case() {
  local label="$1"
  say "-- case: $label"
  rm -rf /root/.livepi
  mkdir -p /root/.livepi && echo "127.0.0.0/8" > /root/.livepi/trusted.txt   # the test client is local
  rm -f /tmp/lh.rc /tmp/lh.pid
  # Plain background start so the pid really is LivePi (not a wrapper shell) and its exit code is recorded
  bash -c 'cd /tmp/lh/livepi && HOME=/root ./LivePi > /tmp/lh.log 2>&1; echo $? > /tmp/lh.rc' &
  for i in $(seq 1 30); do pgrep -x LivePi > /tmp/lh.pid 2>/dev/null && break; sleep 0.2; done
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
  post /api/mixer '{"paflMode":2}' >/dev/null; sleep 1
  check "$label: global PAFL mode AFL works" "api /api/state | jq -e '.paflMode==2 and .chains[0].sendTap==2' >/dev/null && api /api/meters | jq -e '.mon.in > -60' >/dev/null"
  post /api/mixer '{"paflMode":1}' >/dev/null; sleep 1
  check "$label: global PAFL mode input works" "api /api/meters | jq -e '.mon.in > -60' >/dev/null"
  post /api/mixer '{"paflMode":0}' >/dev/null
  X=$(api /api/state | jq '.xruns // 0'); say "xruns after 3 s: $X"
  check "$label: no more than 2 xruns" "[ '$X' -le 2 ]"
  post /api/monitor '{"channel":100,"post":true}' >/dev/null
  B=$(curl -s -m 2 http://127.0.0.1:8080/api/monitor/stream | wc -c); say "stream bytes in 2 s: $B (48 kHz mono 16-bit would be about 192000)"
  check "$label: monitor stream delivers audio" "[ '$B' -gt 100000 ]"
  sleep 1
  check "$label: update status endpoint" "api /api/update/status | jq -e .state >/dev/null"
  check "$label: OS status endpoint" "api /api/os/status | jq -e .state >/dev/null"
  check "$label: still running" "kill -0 \$(cat /tmp/lh.pid)"
  V=$(api /api/editor/viewer); say "viewer: $(echo "$V" | jq -c '{available,displaySource,input,problem}')"
  check "$label: plugin view ready (virtual screen + touch input)" "echo '$V' | jq -e '.available and .input and (.problem==\"\")' >/dev/null"
  T=$(api /api/editor/selftest); say "screen self-test: $T"
  check "$label: plugin view self-test (window drawn on the virtual screen and read back)" "echo '$T' | jq -e '.ok' >/dev/null"
  check "$label: no plugin open -> frame says closed (410)" "[ \"\$(curl -s -m 5 -o /dev/null -w '%{http_code}' 'http://127.0.0.1:8080/api/editor/frame?channel=0&slot=0')\" = 410 ]"
  check "$label: state reports editorAvailable" "api /api/state | jq -e '.editorAvailable==true' >/dev/null"
  # one device at a time
  cget() { curl -s -m 5 -o /dev/null -w '%{http_code}' -H "X-LivePi-Client: $1" "http://127.0.0.1:8080$2"; }
  check "$label: first device gets control" "[ \"\$(cget devA /api/state)\" = 200 ]"
  check "$label: second device is told a session is in progress (409)" "[ \"\$(cget devB /api/state)\" = 409 ] && curl -s -m 5 -H 'X-LivePi-Client: devB' http://127.0.0.1:8080/api/state | jq -e '.busy' >/dev/null"
  curl -s -m 5 -X POST -H 'X-LivePi: 1' -H 'X-LivePi-Client: devB' http://127.0.0.1:8080/api/control/claim >/dev/null
  check "$label: take over works, the first device is now locked out" "[ \"\$(cget devB /api/state)\" = 200 ] && [ \"\$(cget devA /api/state)\" = 409 ]"
  check "$label: page, icons and manifest load" "[ \"\$(curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/manifest.json)\" = 200 ] && [ \"\$(curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/icon-192.png)\" = 200 ]"
  # password: none -> open; set one -> login needed (no user name); cookie works; trusted access; turn off
  check "$label: no password -> open" "api /api/session | jq -e '.passwordSet==false and .authed' >/dev/null"
  J=/tmp/lh.jar; rm -f $J
  curl -s -m 5 -c $J -X POST -H 'X-LivePi: 1' -H 'Content-Type: application/json' -d '{"password":"test1234"}' http://127.0.0.1:8080/api/password >/dev/null
  check "$label: after setting a password, the API needs a login (401)" "[ \"\$(curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/api/state)\" = 401 ]"
  check "$label: the device that set it stays logged in (cookie)" "curl -s -m 5 -b $J http://127.0.0.1:8080/api/state | jq -e '.chains' >/dev/null"
  check "$label: page itself still loads (login screen)" "[ \"\$(curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/)\" = 200 ]"
  check "$label: wrong password refused, tries left shown" "post /api/login '{\"password\":\"nope\"}' | jq -e '.error | test(\"9 tries left\")' >/dev/null"
  J2=/tmp/lh.jar2; rm -f $J2
  curl -s -m 5 -c $J2 -X POST -H 'X-LivePi: 1' -H 'Content-Type: application/json' -d '{"password":"test1234"}' http://127.0.0.1:8080/api/login >/dev/null
  check "$label: password-only login works" "curl -s -m 5 -b $J2 http://127.0.0.1:8080/api/session | jq -e '.authed and .loggedIn' >/dev/null"
  check "$label: old Basic auth still works for scripts" "curl -s -m 5 -u x:test1234 http://127.0.0.1:8080/api/state | jq -e '.chains' >/dev/null"
  curl -s -m 5 -b $J -X POST -H 'X-LivePi: 1' -H 'Content-Type: application/json' -d '{"trustedAccess":true}' http://127.0.0.1:8080/api/access >/dev/null
  check "$label: trusted access lets the trusted network in without a login" "api /api/state | jq -e '.chains' >/dev/null"
  curl -s -m 5 -b $J -X POST -H 'X-LivePi: 1' -H 'Content-Type: application/json' -d '{"trustedAccess":false}' http://127.0.0.1:8080/api/access >/dev/null
  check "$label: trusted access off -> login needed again" "[ \"\$(curl -s -m 5 -o /dev/null -w '%{http_code}' http://127.0.0.1:8080/api/state)\" = 401 ]"
  curl -s -m 5 -b $J -X POST -H 'X-LivePi: 1' -H 'Content-Type: application/json' -d '{"enabled":false}' http://127.0.0.1:8080/api/password/protection >/dev/null
  check "$label: protection off -> open, no login" "api /api/state | jq -e '.chains' >/dev/null"
  check "$label: protection off -> trusted access unavailable" "post /api/access '{\"trustedAccess\":true}' | jq -e '.error' >/dev/null"
  check "$label: turning protection back on needs the password typed" "post /api/password/protection '{\"enabled\":true}' | jq -e '.error' >/dev/null"
  check "$label: mixer works" "api /api/state | jq -e '.chains' >/dev/null"
  for n in $(seq 1 10); do post /api/login '{"password":"nope"}' > /tmp/lh.lock; done
  say "after 10 wrong: $(cat /tmp/lh.lock)"
  check "$label: 10 wrong passwords -> lockout with a countdown" "jq -e '.retryAfter > 500 and .retryAfter <= 600' /tmp/lh.lock >/dev/null"
  check "$label: lockout refuses even the right password" "post /api/login '{\"password\":\"test1234\"}' | jq -e '.retryAfter > 0' >/dev/null"
  check "$label: page learns the countdown on reload" "api /api/session | jq -e '.retryAfter > 0' >/dev/null"
  local pid; pid=$(cat /tmp/lh.pid)
  say "signals: $(grep -E 'SigCgt|SigBlk|SigIgn' /proc/$pid/status | tr '\n' ' ') (SIGTERM caught = bit 15 of SigCgt, i.e. 0x4000)"
  say "threads: $(ls /proc/$pid/task | wc -l)"
  kill -TERM "$pid" 2>/dev/null
  for i in $(seq 1 50); do kill -0 "$pid" 2>/dev/null || break; sleep 0.1; done
  check "$label: stops cleanly on SIGTERM within 5 s (what systemd sends)" "! kill -0 $pid 2>/dev/null"
  sleep 1; say "exit code: $(cat /tmp/lh.rc 2>/dev/null)"
  check "$label: exit code 0" "[ \"$(cat /tmp/lh.rc 2>/dev/null)\" = 0 ]"
  say "viewer log: $(grep -v 'keysym\|xkbcomp\|XKEYBOARD' /root/.livepi/viewer.log 2>/dev/null | tail -n 4 | tr '\n' '|')"
  check "$label: log shows the clean shutdown path" "grep -q 'Shutdown complete' /tmp/lh.log"
  pkill -9 LivePi 2>/dev/null; sleep 1
  say "log tail:"; tail -n 6 /tmp/lh.log | tee -a "$RES"
}

unset DISPLAY
run_case "no DISPLAY given (LivePi starts its own virtual screen)"
check "self-started virtual screen ended with LivePi" "! ps -eo stat=,comm= | grep -v '^Z' | grep -q Xvfb"
Xvfb :99 -screen 0 1024x768x24 >/dev/null 2>&1 &
sleep 2
export DISPLAY=:99
run_case "DISPLAY given (:99)"

say "RESULT: $([ $FAIL -eq 0 ] && echo ALL PASSED || echo SOME FAILED)"
