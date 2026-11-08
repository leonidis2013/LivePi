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
check "package has rt-tune.sh" "[ -x rt-tune.sh ] && bash -n rt-tune.sh"
check "rt-tune knows the button item" "grep -q 'ALL=.*button' rt-tune.sh"
check "package has livepi-net" "[ -x livepi-net ] && bash -n livepi-net"
check "livepi-net status works without NetworkManager (says not available)" "./livepi-net status | jq -e '.available==false' >/dev/null"
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
  sleep 0.3
  M=$(api /api/meters); say "meters right after the device start: $(echo "$M" | jq -c '{in:.in,out:.out}')"
  check "$label: soft start keeps meters quiet while settling" "echo '$M' | jq -e '.in[0] < -90 and .out[0] < -90' >/dev/null"
  check "$label: interface levels answer" "api /api/hwmixer | jq -e '.ok==true and (.cards|type==\"array\")' >/dev/null"
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
  post /api/xruns/reset '{}' >/dev/null
  check "$label: xrun count resets" "api /api/state | jq -e '.xruns <= 1' >/dev/null"
  post /api/monitor '{"channel":100,"post":true}' >/dev/null
  B=$(curl -s -m 2 http://127.0.0.1:8080/api/monitor/stream | wc -c); say "stream bytes in 2 s: $B (48 kHz mono 16-bit would be about 192000)"
  check "$label: monitor stream delivers audio" "[ '$B' -gt 100000 ]"
  sleep 1
  post /api/presets/save '{"name":"Smoke test"}' >/dev/null
  check "$label: preset saved" "api /api/state | jq -e '.presets | index(\"Smoke test\")' >/dev/null"
  check "$label: preset deleted" "post /api/presets/delete '{\"name\":\"Smoke test\"}' | jq -e .ok >/dev/null && api /api/state | jq -e '(.presets | index(\"Smoke test\")) == null' >/dev/null"
  check "$label: deleting a missing preset is refused" "post /api/presets/delete '{\"name\":\"../last\"}' | jq -e .error >/dev/null"
  check "$label: moving a missing plugin is refused" "post /api/chain/move '{\"channel\":0,\"from\":7,\"to\":0}' | jq -e .error >/dev/null"
  # ---- Pisound button (no Pisound in CI: settings still work, presses are simulated nowhere)
  check "$label: button settings answer (5 presses, 10 actions, no Pisound here)" "api /api/button | jq -e '(.gestures|length)==5 and (.actions|length)==10 and .present==false' >/dev/null"
  check "$label: button action saved" "post /api/button '{\"actions\":{\"click1\":{\"action\":\"mute\",\"channel\":1}}}' | jq -e .ok >/dev/null && api /api/button | jq -e '.gestures[0].action==\"mute\" and .gestures[0].channel==1' >/dev/null"
  check "$label: shut down refused on a single click" "post /api/button '{\"actions\":{\"click1\":{\"action\":\"shutdown\"}}}' | jq -e .error >/dev/null"
  check "$label: unknown button action refused" "post /api/button '{\"actions\":{\"hold\":{\"action\":\"rm\"}}}' | jq -e .error >/dev/null"
  check "$label: shut down allowed on the 5 s hold" "post /api/button '{\"actions\":{\"longHold\":{\"action\":\"shutdown\"}}}' | jq -e .ok >/dev/null"
  # ---- mixer mode: built-in LiveVoice channels
  post /api/channels '{"count":2}' >/dev/null
  post /api/mode '{"mode":"mixer"}' >/dev/null; sleep 2
  check "$label: mixer mode switches on" "api /api/state | jq -e '.mode==\"mixer\" and (.chains[0].voice.modules|length)==9' >/dev/null"
  V=$(api '/api/voice?channel=0')
  check "$label: mixer channel has 9 modules and zero latency" "echo '$V' | jq -e '(.order|length)==9 and .latency==0 and (.modules.eq.params|length) > 50' >/dev/null"
  check "$label: mixer setting changes" "post /api/voice/param '{\"channel\":0,\"id\":\"compThr\",\"value\":-31}' | jq -e '.value < -30.9 and .value > -31.1' >/dev/null"
  check "$label: mixer modules reorder" "post /api/voice/order '{\"channel\":0,\"order\":[\"comp\",\"hpf\",\"pse\",\"feedback\",\"eq\",\"deess\",\"prox\",\"leveler\",\"warmth\"]}' | jq -e .ok >/dev/null && api '/api/voice?channel=0&live=1' | jq -e '.order[0]==\"comp\"' >/dev/null"
  check "$label: a bad order is refused" "post /api/voice/order '{\"channel\":0,\"order\":[\"comp\",\"comp\"]}' | jq -e .error >/dev/null"
  sleep 2
  check "$label: audio flows through the mixer channel" "api /api/meters | jq -e '.out[0] > -60' >/dev/null && api '/api/voice?channel=0&live=1' | jq -e '.live.outPeak > -60 or .live.inPeak > -60' >/dev/null"
  check "$label: mixer RTA and EQ curve" "api '/api/voice/spectrum?channel=0&module=eq&out=1' | jq -e '(.rta|length)==120 and (.curve|length)==120' >/dev/null"
  check "$label: voice preset loads" "post /api/voice/preset '{\"channel\":0,\"index\":1}' | jq -e .ok >/dev/null"
  check "$label: channel settings copy" "post /api/voice/copy '{\"from\":0,\"to\":1}' | jq -e .ok >/dev/null && api '/api/voice?channel=1&live=1' | jq -e '.order[0]==\"comp\"' >/dev/null"
  check "$label: plugins refused in mixer mode" "post /api/chain/add '{\"channel\":0,\"id\":\"x\"}' | jq -e '.error | test(\"mixer mode\")' >/dev/null"
  check "$label: mixer has the Input module and the parametric EQ" "api '/api/voice?channel=0' | jq -e '.modules.input.fixed==true and .modules.output.fixed==true and (.modules.output.params[0].id==\"outGain\") and (.modules.peq.params|length)==39 and .modules.peq.inFlow==false' >/dev/null"
  check "$label: modules can be taken out and added" "post /api/voice/order '{\"channel\":0,\"order\":[\"comp\",\"peq\",\"hpf\",\"pse\"]}' | jq -e .ok >/dev/null && api '/api/voice?channel=0&live=1' | jq -e '(.order|length)==4 and .order[1]==\"peq\"' >/dev/null"
  check "$label: parametric EQ curve" "post /api/voice/param '{\"channel\":0,\"id\":\"peq3_gain\",\"value\":6}' | jq -e .ok==null >/dev/null; api '/api/voice/spectrum?channel=0&module=peq&out=1' | jq -e '([.curve[]]|max) > 4' >/dev/null"
  post /api/voice/order '{"channel":0,"order":["comp","peq","hpf","pse","feedback"]}' >/dev/null   # notches need the feedback module in the flow
  check "$label: a notch set by hand" "post /api/voice/notch '{\"channel\":0,\"cmd\":\"addFixed\",\"freq\":1250,\"depth\":-9}' | jq -e .ok >/dev/null && sleep 1 && api '/api/voice?channel=0&live=1' | jq -e '[.live.notches[] | select(.fixed and (.freq > 1200) and (.freq < 1300))] | length == 1' >/dev/null"
  check "$label: a hand-set notch out of range is refused" "post /api/voice/notch '{\"channel\":0,\"cmd\":\"addFixed\",\"freq\":5}' | jq -e .error >/dev/null"
  check "$label: more than one of a module (two compressors, two parametric EQs)" "post /api/voice/order '{\"channel\":0,\"order\":[\"comp\",\"peq\",\"hpf\",\"pse\",\"feedback\",\"comp2\",\"peq3\"]}' | jq -e .ok >/dev/null && api '/api/voice?channel=0' | jq -e '.modules.comp2.name==\"Compressor 2\" and .modules.comp2.instance==2 and (.modules.comp2.params|map(.id)|index(\"compThr_2\")) != null and (.live.comps|length)==3' >/dev/null"
  check "$label: second compressor has its own threshold" "post /api/voice/param '{\"channel\":0,\"id\":\"compThr_2\",\"value\":-37}' | jq -e '.value < -36.9' >/dev/null && api '/api/voice?channel=0' | jq -e '([.modules.comp.params[]|select(.id==\"compThr\")][0].value) > -36' >/dev/null"
  check "$label: HPF shown behind the other graphs" "api '/api/voice/spectrum?channel=0&module=peq3&out=1' | jq -e '(.hpf|length)==120' >/dev/null"
  check "$label: a starting point sets the Parametric EQ when it is in the flow" "post /api/voice/preset '{\"channel\":0,\"index\":1}' | jq -e .ok >/dev/null && api '/api/voice?channel=0' | jq -e '([.modules.peq.params[]|select(.id==\"peq5_gain\")][0].value) > 1' >/dev/null"
  check "$label: save your own preset" "post /api/voice/param '{\"channel\":0,\"id\":\"compRatio\",\"value\":6}' >/dev/null; post /api/voice/preset/save '{\"channel\":0,\"name\":\"Smoke voice\"}' | jq -e .ok >/dev/null && api '/api/voice?channel=0' | jq -e '(.userPresets|index(\"Smoke voice\")) != null and .preset==\"Smoke voice\"' >/dev/null"
  check "$label: load your own preset" "post /api/voice/param '{\"channel\":0,\"id\":\"compRatio\",\"value\":2}' >/dev/null; post /api/voice/preset/user '{\"channel\":0,\"name\":\"Smoke voice\"}' | jq -e .ok >/dev/null && api '/api/voice?channel=0' | jq -e '([.modules.comp.params[]|select(.id==\"compRatio\")][0].value) > 5.9' >/dev/null"
  check "$label: delete your own preset" "post /api/voice/preset/delete '{\"name\":\"Smoke voice\"}' | jq -e .ok >/dev/null && api '/api/voice?channel=0' | jq -e '(.userPresets|index(\"Smoke voice\")) == null' >/dev/null"
  check "$label: lapel voice preset" "api '/api/voice?channel=0' | jq -e '.presets | map(test(\"Lapel\")) | any' >/dev/null"
  post /api/mode '{"mode":"vst"}' >/dev/null; sleep 1
  check "$label: back to VST mode" "api /api/state | jq -e '.mode==\"vst\" and (.chains[0].voice==null)' >/dev/null"
  post /api/mode '{"mode":"mixer"}' >/dev/null; sleep 1
  check "$label: mixer settings kept across a mode switch" "api '/api/voice?channel=0&live=1' | jq -e '.order[0]==\"comp\"' >/dev/null"
  post /api/mode '{"mode":"vst"}' >/dev/null
  post /api/channels '{"count":1}' >/dev/null
  check "$label: setup assistant shows until done" "post /api/setup/done '{\"done\":false}' >/dev/null && api /api/state | jq -e '.setupDone==false' >/dev/null && post /api/setup/done '{\"done\":true}' | jq -e .ok >/dev/null && api /api/state | jq -e '.setupDone==true' >/dev/null"
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

# ---- reliability: status, crash-loop safe mode, frozen-audio watchdog
start_lp() {   # start_lp [ENV=value ...]
  rm -f /tmp/lh.rc
  bash -c "cd /tmp/lh/livepi && HOME=/root $* ./LivePi > /tmp/lh.log 2>&1; echo \$? > /tmp/lh.rc" &
  for i in $(seq 1 40); do api /api/info | jq -e .version >/dev/null 2>&1 && break; sleep 0.5; done
}
reliability_case() {
  say "-- case: reliability"
  pkill -9 -x LivePi 2>/dev/null; sleep 1
  rm -rf /root/.livepi; mkdir -p /root/.livepi && echo "127.0.0.0/8" > /root/.livepi/trusted.txt
  echo '{"numChannels":2,"chains":[{"name":"Alpha","slots":[]},{"name":"Beta","slots":[]}]}' > /root/.livepi/last.json
  start_lp
  R=$(api /api/reliability); say "reliability: $(echo "$R" | jq -c .)"
  check "reliability: status answers" "echo '$R' | jq -e 'has(\"realtime\") and has(\"memoryLocked\") and has(\"watchdog\")' >/dev/null"
  check "reliability: says whether the page can fix things" "echo '$R' | jq -e 'has(\"fixAvailable\") and has(\"rebootNeeded\")' >/dev/null"
  check "reliability: fix without the installer's helper asks for the one-time setup" "post /api/reliability/fix '{}' | jq -e '.error==\"setup\"' >/dev/null"
  check "reliability: fixing one item without the helper also asks for the setup" "post /api/reliability/fix '{\"item\":\"core\"}' | jq -e '.error==\"setup\"' >/dev/null"
  check "reliability: unknown fix items are refused" "post /api/reliability/fix '{\"item\":\"rm -rf\"}' | jq -e '.error==\"Unknown item\"' >/dev/null"
  check "network: page is told the one-time setup is needed" "api /api/network | jq -e '.available==false and .setup==true' >/dev/null"
  check "network: change without the helper asks for the setup" "post /api/network '{\"device\":\"eth0\",\"mode\":\"auto\"}' | jq -e '.error==\"setup\"' >/dev/null"
  check "network: confirm refuses a bad token" "post /api/network/confirm '{\"token\":\"nope\"}' | jq -e '.error==\"Bad token\"' >/dev/null"
  check "reliability: state lists notices" "api /api/state | jq -e '(.notices|type==\"array\") and .safeMode==false' >/dev/null"
  check "reliability: session loaded" "api /api/state | jq -e '.chains[1].name==\"Beta\"' >/dev/null"
  # two crashes in a row while starting -> the third start is in safe mode
  pkill -9 -x LivePi; sleep 1; start_lp
  check "reliability: one early crash is not safe mode yet" "api /api/state | jq -e '.safeMode==false' >/dev/null"
  pkill -9 -x LivePi; sleep 1; start_lp
  check "reliability: after two early crashes LivePi starts in safe mode" "api /api/state | jq -e '.safeMode==true and any(.notices[]; .id==\"safe\")' >/dev/null"
  check "reliability: safe mode keeps the channels" "api /api/state | jq -e '.chains[0].name==\"Alpha\"' >/dev/null"
  post /api/mixer '{"channel":0,"gainDb":-7}' >/dev/null; sleep 3
  check "reliability: changes in safe mode are saved apart (last.json keeps the plugins)" "[ -s /root/.livepi/last-safe.json ] && jq -e '.chains[0].gainDb != -7' /root/.livepi/last.json >/dev/null"
  pkill -TERM -x LivePi; for i in $(seq 1 50); do pgrep -x LivePi >/dev/null || break; sleep 0.1; done; start_lp
  check "reliability: safe mode survives a restart until you choose" "api /api/state | jq -e '.safeMode==true and .chains[0].gainDb==-7' >/dev/null"
  check "reliability: restore leaves safe mode" "post /api/safemode/restore '{}' | jq -e .ok >/dev/null && api /api/state | jq -e '.safeMode==false and (any(.notices[]; .id==\"safe\")|not)' >/dev/null"
  pkill -TERM -x LivePi; for i in $(seq 1 50); do pgrep -x LivePi >/dev/null || break; sleep 0.1; done
  check "reliability: a clean stop resets the crash count" "[ \"\$(cat /root/.livepi/starts.txt)\" = 0 ]"
  check "reliability: safe mode flag cleared after restore" "[ ! -e /root/.livepi/safe-mode ]"
  # frozen audio (a hung plugin): the watchdog restarts LivePi and the page is told why
  start_lp LIVEPI_TEST_HANG_AFTER=3
  for i in $(seq 1 60); do pgrep -x LivePi >/dev/null || break; sleep 0.5; done
  sleep 1
  say "after the freeze: rc=$(cat /tmp/lh.rc 2>/dev/null) note=$(cat /root/.livepi/restart-note.txt 2>/dev/null)"
  check "reliability: watchdog ends a frozen LivePi (exit 70) within 30 s" "[ \"\$(cat /tmp/lh.rc 2>/dev/null)\" = 70 ]"
  check "reliability: watchdog leaves a note" "grep -q 'stopped responding' /root/.livepi/restart-note.txt"
  start_lp
  check "reliability: next start shows the note on the page" "api /api/state | jq -e 'any(.notices[]; .id==\"restart\")' >/dev/null"
  check "reliability: audio running normally again" "sleep 2; api /api/meters | jq -e '.in|type==\"array\"' >/dev/null && api /api/state | jq -e '.xruns <= 2' >/dev/null"
  pkill -TERM -x LivePi; sleep 2
  say "log tail:"; tail -n 6 /tmp/lh.log | tee -a "$RES"
}
reliability_case

say "RESULT: $([ $FAIL -eq 0 ] && echo ALL PASSED || echo SOME FAILED)"
