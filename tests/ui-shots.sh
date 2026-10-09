#!/usr/bin/env bash
# Screenshots of every screen of the web UI, from the host build against
# tests/mock_romm.py, for checking a restyle or a theme.
#   make host && tests/ui-shots.sh build/shots/before
#   ... change the UI ...
#   make host && tests/ui-shots.sh build/shots/after
#   node tests/ui_shots.mjs compare build/shots/before build/shots/after build/shots/diff
# THEME=<id> picks a theme (default classic). Needs Google Chrome (CHROME=<binary>).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
mkdir -p "${1:?usage: ui-shots.sh <out-dir>}"; OUT="$(cd "$1" && pwd)"
# A fixed folder: its path shows on screen, so it must be the same every run.
T="${TMPDIR:-/tmp}/romm-ui-shots"; rm -rf "$T"; mkdir -p "$T"
MOCK_PORT=8897 WEB_PORT=8782
API="http://127.0.0.1:${WEB_PORT}"
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }

python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" &
RA="$T/RetroArch"; mkdir -p "$RA/roms/snes" "$RA/saves" "$T/data"
printf 'savefile_directory = ":/saves"\n' > "$RA/retroarch.cfg"
echo "ROM" > "$RA/roms/snes/Chrono Trigger (USA).sfc"
cat > "$T/data/config.json" <<EOF
{"web_port": $WEB_PORT, "sync_interval_min": 0, "update_check": false, "profiles_custom": true, "ui_theme": "${THEME:-classic}",
 "profiles": [{"id":"retroarch","name":"RetroArch","enabled":true,"root":"$RA",
   "rom_dir":"{root}/roms/{platform}","save_dir":"{root}/saves","state_dir":"{root}/states",
   "bios_dir":"{root}/system","save_exts":".srm","retroarch_cfg":"{root}/retroarch.cfg",
   "platforms":[{"romm":["snes"],"dir":"snes","emulator":"snes9x","core_name":"Snes9x"},
     {"romm":["psp"],"dir":"psp","emulator":"ppsspp","core_name":"PPSSPP","save_layout":"psp"}]}]}
EOF
ROMM_SYNC_DATA="$T/data" ROMM_SYNC_HOMEBREW="$T/none" "$ROOT/build/host/romm-sync" >"$T/daemon.log" 2>&1 &
for _ in $(seq 1 40); do curl -sf "$API/api/status" >/dev/null && break; sleep 0.25; done

rm -f "$OUT"/*.png
node "$ROOT/tests/ui_shots.mjs" shoot "$API/" "$OUT" pair
post /api/pair/start "{\"server_url\":\"http://127.0.0.1:$MOCK_PORT\"}" >/dev/null
for _ in $(seq 1 40); do
  [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break
  sleep 0.25
done
node "$ROOT/tests/ui_shots.mjs" shoot "$API/" "$OUT" setup
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
post /api/sync >/dev/null
for _ in $(seq 1 40); do
  [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; s=json.load(sys.stdin)['sync']; print(s['sync_count'] >= 1 and not s['running'])")" == True ]] && break
  sleep 0.25
done
node "$ROOT/tests/ui_shots.mjs" shoot "$API/" "$OUT" app
echo "screenshots in $OUT"
