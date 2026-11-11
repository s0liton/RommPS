#!/usr/bin/env bash
# Autostart on a console with two loaders (etaHEN and onionHEN folders) and a
# payload manager's copy, against the host build.
#   make host && tests/autostart.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
WEB_PORT=8783
API="http://127.0.0.1:${WEB_PORT}"
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT

ETA="$T/etaHEN/payloads" ONION="$T/OnionHEN/payloads" PLD="$T/pldmgr/payloads/romm-sync/romm-sync.elf"
fail() { echo "FAIL: $*"; echo "--- daemon log"; tail -30 "$T/daemon.log"; exit 1; }
# A stand-in payload: an ELF header and, from 1.1.0 on, the version tag.
elf() { if [[ -n "$1" ]]; then printf '\177ELF payload ROMM_SYNC_VERSION=%s\n' "$1"; else printf '\177ELF payload\n'; fi; }
ver() { [[ -f "$1" ]] && { grep -ao 'ROMM_SYNC_VERSION=[0-9A-Za-z.+-]*' "$1" | cut -d= -f2 || echo untagged; } || echo none; }
on()  { [[ -f "$1/romm-sync.elf.auto_start" ]] && echo on || echo off; }
start() {
  ROMM_SYNC_APP_INSTALLED="${APP_INSTALLED:-0}" ROMM_SYNC_APP_VERSION="${APP_VERSION:-}" ROMM_SYNC_SELF_UPDATE="${SELF_UPDATE:-1}" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_HOMEBREW="$T/none" ROMM_SYNC_AUTOSTART="$ETA,$ONION" \
    ROMM_SYNC_PAYLOAD_COPIES="$PLD,$T/elsewhere/romm-sync.elf" "$ROOT/build/host/romm-sync" >>"$T/daemon.log" 2>&1 &
  for _ in $(seq 1 40); do curl -sf "$API/api/autostart" >/dev/null 2>&1 && return 0; sleep 0.25; done
  fail "the daemon didn't start"
}
stop() { kill $(jobs -p) 2>/dev/null || true; wait 2>/dev/null || true; }
upload()  { elf "$2" > "$T/up.elf"; curl -s -X POST "$API/api/autostart/$1" --data-binary @"$T/up.elf"; }
expect() { # expect <what> <got> <want>
  [[ "$2" == "$3" ]] || fail "$1: got '$2', expected '$3'"; }

mkdir -p "$T/data" "$ETA" "$ONION" "$(dirname "$PLD")"
echo "{\"web_port\": $WEB_PORT, \"sync_interval_min\": 0}" > "$T/data/config.json"

echo "1. at startup, every copy is brought up to the newest and onionHEN is set up too"
elf 1.1.0 > "$ETA/romm-sync.elf"; : > "$ETA/romm-sync.elf.auto_start"   # 1.1.0 installed it in etaHEN only
elf "" > "$ETA/romm-sync.elf.bak"                                        # an update's leftover
elf "" > "$PLD"                                                          # 1.0.x kept in pldmgr's autoload
start
expect "etaHEN copy" "$(ver "$ETA/romm-sync.elf")" 1.1.0
expect "onionHEN copy" "$(ver "$ONION/romm-sync.elf")" 1.1.0
expect "onionHEN autostart" "$(on "$ONION")" on
expect "pldmgr copy" "$(ver "$PLD")" 1.1.0
[[ ! -e "$ETA/romm-sync.elf.bak" ]] || fail "the .bak wasn't removed"
[[ ! -e "$T/elsewhere" ]] || fail "a copy that didn't exist was created"

echo "2. an older payload never replaces a newer one"
upload upload 1.0.9 >/dev/null
expect "etaHEN after an older upload" "$(ver "$ETA/romm-sync.elf")" 1.1.0
expect "onionHEN after an older upload" "$(ver "$ONION/romm-sync.elf")" 1.1.0
upload upload "" >/dev/null
expect "etaHEN after an untagged (1.0.x) upload" "$(ver "$ETA/romm-sync.elf")" 1.1.0
expect "pldmgr after an older upload" "$(ver "$PLD")" 1.1.0

echo "3. install (1.2's name for upload) puts the newer payload everywhere, autostart on"
r=$(upload install 1.1.1); [[ "$r" == *'"version":"1.1.1"'* ]] || fail "status after install: $r"
for d in "$ETA" "$ONION"; do expect "$d" "$(ver "$d/romm-sync.elf")/$(on "$d")" 1.1.1/on; done
expect "pldmgr" "$(ver "$PLD")" 1.1.1
n=$(curl -sf "$API/api/autostart" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['copies']))")
expect "copies reported" "$n" 3

echo "4. there's no off: asking for it (older apps) leaves autostart on"
curl -sf -X POST "$API/api/autostart" -d '{"enable":false}' >/dev/null
for d in "$ETA" "$ONION"; do expect "$d after asking for off" "$(ver "$d/romm-sync.elf")/$(on "$d")" 1.1.1/on; done

echo "5. upload turns it on in every installed loader; a loader that isn't installed is left alone"
rm -rf "$T/OnionHEN"
upload upload 1.1.2 >/dev/null
expect "etaHEN" "$(ver "$ETA/romm-sync.elf")/$(on "$ETA")" 1.1.2/on
expect "pldmgr" "$(ver "$PLD")" 1.1.2
[[ ! -e "$T/OnionHEN" ]] || fail "a folder was made for a loader that isn't installed"

echo "6. a loader installed later gets set up at the next start"
stop; mkdir -p "$ONION"; start
expect "onionHEN after restart" "$(ver "$ONION/romm-sync.elf")/$(on "$ONION")" 1.1.2/on

echo "7. autostart another tool turned off comes back on at the next start, without its .off"
stop
rm "$ETA/romm-sync.elf.auto_start"                                       # deleted by hand
mv "$ONION/romm-sync.elf.auto_start" "$ONION/romm-sync.elf.auto_start.off" # renamed by a file manager
elf 1.0.9 > "$ONION/romm-sync.elf"                                        # and an older copy
start
for d in "$ETA" "$ONION"; do expect "$d" "$(ver "$d/romm-sync.elf")/$(on "$d")" 1.1.2/on; done
[[ ! -e "$ONION/romm-sync.elf.auto_start.off" ]] || fail "the .off marker wasn't removed"

echo "8. every loader is turned on even when only a payload manager has a copy"
stop; rm -f "$ETA"/* "$ONION"/*; start
for d in "$ETA" "$ONION"; do expect "$d" "$(ver "$d/romm-sync.elf")/$(on "$d")" 1.1.2/on; done

echo "9. on the PS5 the payload doesn't update itself: RommPS brings the new one"
stop; SELF_UPDATE=0 start
r=$(curl -s -X POST "$API/api/update/install"); [[ "$r" == *"update RommPS from your homebrew store"* ]] || fail "install with self-update off: $r"
r=$(curl -sf "$API/api/update"); [[ "$r" == *'"self_update":false'* ]] || fail "update status: $r"

echo "10. a page on another website can't change anything; our own page and apps can"
code() { curl -s -o /dev/null -w '%{http_code}' -X POST "$API/api/autostart" "$@"; }
rm "$ETA/romm-sync.elf.auto_start"
expect "post from another site" "$(code -H 'Origin: http://evil.example' -d '{"enable":true}')" 403
expect "cross-site fetch" "$(code -H 'Sec-Fetch-Site: cross-site' -d '{"enable":true}')" 403
expect "opaque origin" "$(code -H 'Origin: null' -d '{"enable":true}')" 403
expect "etaHEN after refused posts" "$(on "$ETA")" off
expect "our own page" "$(code -H "Origin: http://127.0.0.1:$WEB_PORT" -H 'Sec-Fetch-Site: same-origin' -d '{}')" 200
expect "etaHEN after our own page" "$(on "$ETA")" on
expect "an app (no Origin)" "$(code -d '{}')" 200
expect "reading from another site still works" "$(curl -s -o /dev/null -w '%{http_code}' -H 'Origin: http://evil.example' "$API/api/autostart")" 200

echo "11. with RommPS installed RomM Sync keeps quiet; without it, it says to install RommPS"
stop; : > "$T/daemon.log"; SELF_UPDATE=0 APP_INSTALLED=1 start; sleep 1
! grep -q NOTIFY "$T/daemon.log" || fail "a notification with RommPS installed: $(grep NOTIFY "$T/daemon.log")"
stop; : > "$T/daemon.log"; SELF_UPDATE=0 APP_INSTALLED=1 APP_VERSION=1.1.0 start; sleep 1
grep -q "NOTIFY.*RommPS 1.1.0 is out of date" "$T/daemon.log" || fail "no hint to update an older RommPS: $(grep NOTIFY "$T/daemon.log")"
stop; : > "$T/daemon.log"; SELF_UPDATE=0 start; sleep 1
grep -q "^Install the RommPS app from your homebrew store" "$T/daemon.log" || fail "no hint to install RommPS: $(grep -A1 NOTIFY "$T/daemon.log")"

echo "12. dev builds: over their release, under the next one, and in order among themselves"
stop; start
upload upload 1.4.3-dev.2 >/dev/null
expect "a dev build over its release" "$(ver "$ETA/romm-sync.elf")" 1.4.3-dev.2
upload upload 1.4.3-dev.10 >/dev/null
expect "a newer dev build (10 after 2, not before)" "$(ver "$ETA/romm-sync.elf")" 1.4.3-dev.10
upload upload 1.4.3-dev.9 >/dev/null
expect "an older dev build" "$(ver "$ETA/romm-sync.elf")" 1.4.3-dev.10
upload upload 1.4.2 >/dev/null
expect "the release before it" "$(ver "$ETA/romm-sync.elf")" 1.4.3-dev.10
upload upload 1.4.3 >/dev/null
for d in "$ETA" "$ONION"; do expect "$d: the release over its dev builds" "$(ver "$d/romm-sync.elf")/$(on "$d")" 1.4.3/on; done
expect "pldmgr: the release over its dev builds" "$(ver "$PLD")" 1.4.3
upload upload 1.4.3-dev.11 >/dev/null
expect "a dev build of a release that's out" "$(ver "$ETA/romm-sync.elf")" 1.4.3
# At startup the newest copy wins, numbers as numbers.
stop; elf 1.4.4-dev.9 > "$ETA/romm-sync.elf"; elf 1.4.4-dev.10 > "$ONION/romm-sync.elf"; start
for d in "$ETA" "$ONION"; do expect "$d at startup" "$(ver "$d/romm-sync.elf")" 1.4.4-dev.10; done
expect "pldmgr at startup" "$(ver "$PLD")" 1.4.4-dev.10

echo PASS
