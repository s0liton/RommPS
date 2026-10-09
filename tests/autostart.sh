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
ver() { [[ -f "$1" ]] && { grep -ao 'ROMM_SYNC_VERSION=[0-9.]*' "$1" | cut -d= -f2 || echo untagged; } || echo none; }
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

chosen() { curl -sf "$API/api/autostart" | python3 -c "import json,sys; print(json.load(sys.stdin)['chosen'])"; }
expect "a choice before anyone made one" "$(chosen)" False

echo "2. an older payload never replaces a newer one"
upload upload 1.0.9 >/dev/null
expect "etaHEN after an older upload" "$(ver "$ETA/romm-sync.elf")" 1.1.0
expect "onionHEN after an older upload" "$(ver "$ONION/romm-sync.elf")" 1.1.0
upload upload "" >/dev/null
expect "etaHEN after an untagged (1.0.x) upload" "$(ver "$ETA/romm-sync.elf")" 1.1.0
expect "pldmgr after an older upload" "$(ver "$PLD")" 1.1.0

echo "3. install puts the newer payload everywhere and leaves autostart on"
r=$(upload install 1.1.1); [[ "$r" == *'"version":"1.1.1"'* ]] || fail "status after install: $r"
for d in "$ETA" "$ONION"; do expect "$d" "$(ver "$d/romm-sync.elf")/$(on "$d")" 1.1.1/on; done
expect "pldmgr" "$(ver "$PLD")" 1.1.1
n=$(curl -sf "$API/api/autostart" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['copies']))")
expect "copies reported" "$n" 3

echo "4. off removes the payload from every loader, but not the payload manager's copy"
curl -sf -X POST "$API/api/autostart" -d '{"enable":false}' >/dev/null
for d in "$ETA" "$ONION"; do [[ -z "$(ls -A "$d")" ]] || fail "$d not empty after turning autostart off: $(ls "$d")"; done
expect "pldmgr after off" "$(ver "$PLD")" 1.1.1

echo "5. install while off only updates copies that exist; it doesn't turn autostart on"
upload install 1.1.2 >/dev/null
for d in "$ETA" "$ONION"; do [[ -z "$(ls -A "$d")" ]] || fail "install while off wrote to $d"; done
expect "pldmgr" "$(ver "$PLD")" 1.1.2

echo "6. upload turns it on in every installed loader; a loader that isn't installed is left alone"
rm -rf "$T/OnionHEN"
upload upload 1.1.2 >/dev/null
expect "etaHEN" "$(ver "$ETA/romm-sync.elf")/$(on "$ETA")" 1.1.2/on
[[ ! -e "$T/OnionHEN" ]] || fail "a folder was made for a loader that isn't installed"

expect "turning it on is remembered" "$(chosen)" True

echo "7. a loader installed later gets set up at the next start"
stop; mkdir -p "$ONION"; start
expect "the choice after a restart" "$(chosen)" True
expect "onionHEN after restart" "$(ver "$ONION/romm-sync.elf")/$(on "$ONION")" 1.1.2/on

echo "8. while off, the next start adds nothing but still updates what's there"
curl -sf -X POST "$API/api/autostart" -d '{"enable":false}' >/dev/null
stop; elf 1.0.9 > "$ONION/romm-sync.elf"; start   # a stray copy, not turned on
expect "etaHEN while off" "$(ver "$ETA/romm-sync.elf")" none
expect "stray onionHEN copy" "$(ver "$ONION/romm-sync.elf")/$(on "$ONION")" 1.1.2/off

echo "9. on the PS5 the payload doesn't update itself: RommPS brings the new one"
stop; SELF_UPDATE=0 start
r=$(curl -s -X POST "$API/api/update/install"); [[ "$r" == *"update RommPS from ProsperoStore"* ]] || fail "install with self-update off: $r"
r=$(curl -sf "$API/api/update"); [[ "$r" == *'"self_update":false'* ]] || fail "update status: $r"

echo "10. a page on another website can't change anything; our own page and apps can"
code() { curl -s -o /dev/null -w '%{http_code}' -X POST "$API/api/autostart" "$@"; }
expect "post from another site" "$(code -H 'Origin: http://evil.example' -d '{"enable":true}')" 403
expect "cross-site fetch" "$(code -H 'Sec-Fetch-Site: cross-site' -d '{"enable":true}')" 403
expect "opaque origin" "$(code -H 'Origin: null' -d '{"enable":true}')" 403
expect "etaHEN after refused posts" "$(ver "$ETA/romm-sync.elf")" none
expect "our own page" "$(code -H "Origin: http://127.0.0.1:$WEB_PORT" -H 'Sec-Fetch-Site: same-origin' -d '{"enable":false}')" 200
expect "an app (no Origin)" "$(code -d '{"enable":false}')" 200
expect "reading from another site still works" "$(curl -s -o /dev/null -w '%{http_code}' -H 'Origin: http://evil.example' "$API/api/autostart")" 200

echo "11. with RommPS installed RomM Sync keeps quiet; without it, it says to install RommPS"
stop; : > "$T/daemon.log"; SELF_UPDATE=0 APP_INSTALLED=1 start; sleep 1
! grep -q NOTIFY "$T/daemon.log" || fail "a notification with RommPS installed: $(grep NOTIFY "$T/daemon.log")"
stop; : > "$T/daemon.log"; SELF_UPDATE=0 APP_INSTALLED=1 APP_VERSION=1.1.0 start; sleep 1
grep -q "NOTIFY.*RommPS 1.1.0 is out of date" "$T/daemon.log" || fail "no hint to update an older RommPS: $(grep NOTIFY "$T/daemon.log")"
stop; : > "$T/daemon.log"; SELF_UPDATE=0 start; sleep 1
grep -q "NOTIFY.*Install RommPS from ProsperoStore" "$T/daemon.log" || fail "no hint to install RommPS: $(grep NOTIFY "$T/daemon.log")"

echo PASS
