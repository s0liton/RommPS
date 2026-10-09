#!/usr/bin/env bash
# End-to-end test of the host build against tests/mock_romm.py.
#   make host tools && tests/e2e.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
MOCK_PORT=8899 WEB_PORT=8781
API="http://127.0.0.1:${WEB_PORT}"
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT

fail() { echo "FAIL: $*"; echo "--- daemon log"; tail -30 "$T/daemon.log"; exit 1; }
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
wait_sync() { # waits for the history length to reach $1
  for _ in $(seq 1 40); do
    local cur
    cur=$(curl -sf "$API/api/status" | python3 -c "import json,sys; s=json.load(sys.stdin)['sync']; print(s['sync_count'] if not s['running'] else -1)")
    [[ "$cur" -ge "$1" ]] && return 0; sleep 0.5
  done; fail "sync #$1 did not finish"
}
last() { curl -sf "$API/api/status" | python3 -c "import json,sys; h=json.load(sys.stdin)['sync']['history'][-1]; print(h.get('$1', h.get('error','')))"; }

REL="$T/release"; mkdir -p "$REL"
MOCK_RELEASE_DIR="$REL" python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" &
# The same mock over HTTPS with a self-signed certificate.
TLS_PORT=8898
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=127.0.0.1 -keyout "$T/tls.pem" -out "$T/tls.crt" 2>/dev/null
cat "$T/tls.crt" >> "$T/tls.pem"
python3 "$ROOT/tests/mock_romm.py" $TLS_PORT "$T/tls.pem" 2>"$T/mock-tls.log" &
RA="$T/RetroArch"; mkdir -p "$RA/roms/snes" "$RA/saves/Snes9x" "$T/data"
# RetroArch defaults: saves sorted into a folder per core name.
printf 'savefile_directory = ":/saves"\nsort_savefiles_enable = "true"\n' > "$RA/retroarch.cfg"
cat > "$T/data/config.json" <<EOF
{"web_port": $WEB_PORT, "sync_interval_min": 0, "exit_delay_sec": 1, "profiles_custom": true,
 "profiles": [{"id":"retroarch","name":"RetroArch","enabled":true,"root":"$RA",
   "rom_dir":"{root}/roms/{platform}","save_dir":"{root}/saves","state_dir":"{root}/states",
   "bios_dir":"{root}/system","save_exts":".srm","state_exts":".state*","retroarch_cfg":"{root}/retroarch.cfg",
   "platforms":[{"romm":["snes"],"dir":"snes","emulator":"snes9x","core_name":"Snes9x"},
     {"romm":["ps2"],"dir":"ps2","emulator":"pcsx2","core_name":"LRPS2","save_exts":".ps2",
      "sync_requires_option":{"key":"pcsx2_shared_memory_cards","value":"disabled","default":"enabled"}},
     {"romm":["psp"],"dir":"psp","emulator":"ppsspp","core_name":"PPSSPP","save_layout":"psp"},
     {"romm":["ngc"],"dir":"ngc","emulator":"dolphin","core_name":"Dolphin","save_layout":"gci"}]}]}
EOF
echo "ROM" > "$RA/roms/snes/Chrono Trigger (USA).sfc"     # present locally, matched by name
echo "local-save-v1" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
# A second, native-app style RetroArch install for the detection test.
HB="$T/homebrew/PPSA12345"; mkdir -p "$HB/config" "$HB/cores" "$HB/sce_sys"
echo 'savefile_directory = ":/savefiles"' > "$HB/config/retroarch.cfg"
printf 'corename = "Snes9x"\ndatabase = "Nintendo - Super Nintendo Entertainment System|Nintendo - Satellaview"\n' > "$HB/cores/snes9x_libretro.info"
printf 'corename = "LRPS2"\ndatabase = "Sony - PlayStation 2"\n' > "$HB/cores/pcsx2_libretro.info"
echo '{"titleId":"PPSA12345","localizedParameters":{"defaultLanguage":"en-US","en-US":{"titleName":"RetroArch"}}}' > "$HB/sce_sys/param.json"
# A standalone SNES emulator from a test catalog, "installed" as TEST00001.
mkdir -p "$T/apps/TEST00001" "$T/snesemu"
cat > "$T/catalog.json" <<EOF
[{"id": "testsnes", "name": "Test SNES", "title_ids": ["TEST00001"],
  "profile": {"root": "$T/snesemu", "rom_dir": "{root}/roms", "save_dir": "{root}/saves", "state_dir": "{root}/states",
              "save_exts": ".srm", "platforms": [{"romm": ["snes", "sfam"], "dir": "snes"}]}}]
EOF
# An installed payload in a stand-in etaHEN folder, for the update test.
mkdir -p "$T/etaHEN/payloads"; echo "old-payload" > "$T/etaHEN/payloads/romm-sync.elf"
ROMM_SYNC_HOMEBREW="$T/homebrew" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_AUTOSTART="$T/etaHEN/payloads" \
  ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_APP_DIRS="$T/apps" ROMM_SYNC_FS_ROOTS="$T" ROMM_SYNC_COVERS_TICK=1 \
  ROMM_SYNC_UPDATE_URL="http://127.0.0.1:$MOCK_PORT/_github/release" "$ROOT/build/host/romm-sync" >"$T/daemon.log" 2>&1 &
sleep 6   # let the (unpaired, skipped) startup sync pass

echo "0. self-signed HTTPS needs certificate checks turned off"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *"doesn't trust"* ]] || fail "untrusted certificate not reported: $r"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *"doesn't trust"* ]] || fail "an address without a scheme must not fall back to http on a certificate error: $r"
post /api/config '{"tls_verify":false}' >/dev/null
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *pending* ]] || fail "pairing with checks off failed: $r"
post /api/pair/forget >/dev/null; post /api/config '{"tls_verify":true}' >/dev/null

echo "1. setup: pair, detect emulators, preview, first sync"
post /api/pair/start "{\"server_url\":\"http://127.0.0.1:$MOCK_PORT\"}" >/dev/null
for _ in $(seq 1 20); do [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break; sleep 0.5; done
d=$(curl -sf "$API/api/setup/detect" | python3 -c "
import json,sys; c=json.load(sys.stdin)[0]; p=c['profile']['platforms']
print(c['title_id'], c['profile']['root'].endswith('PPSA12345'), [(x['dir'], x['core_name'], x.get('sync_saves', True)) for x in p])")
[[ "$d" == "PPSA12345 True [('ps2', 'LRPS2', True), ('snes', 'Snes9x', True), ('satellaview', 'Snes9x', True)]" ]] || fail "detection: $d"
sleep 1; [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")" == 0 ]] || fail "no sync may run before setup is finished"
pv=$(curl -sf "$API/api/setup/preview" | python3 -c "import json,sys; p=json.load(sys.stdin); print(len(p['uploads']), len(p['downloads']), p['local_games'])")
[[ "$pv" == "1 0 1" ]] || fail "preview: $pv"
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
wait_sync 1
[[ "$(last reason)" == "first sync" && "$(last uploaded)" == 1 ]] || fail "expected the first sync to upload the local save"

echo "2. game download, then its server save"
post /api/download '{"rom_id":11,"name":"Super Metroid"}' >/dev/null
[[ "$(curl -sf "$API/api/downloads" | python3 -c "import json,sys; print([d['name'] for d in json.load(sys.stdin) if d.get('rom_id') == 11][-1])")" == "Super Metroid" ]] \
  || fail "a download must show its name from the start"
sleep 1
[[ -f "$RA/roms/snes/Super Metroid (USA).sfc" ]] || fail "ROM not downloaded"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":11,"slot":"autosave","file_name":"Super Metroid (USA).srm","content":"server-save"}' >/dev/null
post /api/sync >/dev/null; wait_sync 2
[[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "server-save" ]] || fail "server save not downloaded"

ci=$(curl -sf "$API/api/roms?platform_id=1" | python3 -c "import json,sys; print(sorted(json.load(sys.stdin)['char_index'].items()))")
[[ "$ci" == "[('c', 0), ('e', 1), ('m', 2), ('s', 3)]" ]] || fail "library char_index for the A-Z ribbon: $ci"
inst=$(curl -sf "$API/api/installed" | python3 -c "import json,sys; print([(g['id'], g['name'], g['platform_id'] > 0) for g in json.load(sys.stdin)])")
[[ "$inst" == "[(11, 'Super Metroid', True), (10, 'Chrono Trigger', True)]" ]] || fail "the games on this console, for the app: $inst"
all=$(curl -sf "$API/api/roms?platform_id=0&search=metroid" | python3 -c "import json,sys; print([(g['name'], g['platform_id']) for g in json.load(sys.stdin)['items']])")
[[ "$all" == "[('Super Metroid', 1)]" ]] || fail "a search across every platform: $all"

echo "3. nothing changed, nothing transferred"
post /api/sync >/dev/null; wait_sync 3
[[ "$(last uploaded)$(last downloaded)" == "00" ]] || fail "expected nothing to transfer"

echo "4. save uploads while a game runs, downloads wait for it to close"
echo "PPSA00000" > "$T/data/foreground"; sleep 4
echo "local-save-v2" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
for _ in $(seq 1 16); do [[ "$(last reason)" == "save changed" && "$(last uploaded)" == 1 ]] && break; sleep 0.5; done
[[ "$(last reason)" == "save changed" && "$(last uploaded)" == 1 ]] || fail "change was not uploaded while playing"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":11,"slot":"autosave","file_name":"SM.srm","content":"from-other-device"}' >/dev/null
post /api/sync >/dev/null; sleep 2
[[ "$(last deferred)" == 1 && "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "server-save" ]] || fail "download must wait while a game runs"
rm "$T/data/foreground"
for _ in $(seq 1 30); do [[ "$(last reason)" == "game exited" ]] && break; sleep 0.5; done
[[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "from-other-device" ]] || fail "deferred download not applied after exit"
sleep 3 # let the watcher see our own download settle (no-op sync)
n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")

echo "5. conflict, keep the server copy"
sleep 1.1; curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":10,"slot":"autosave","file_name":"Chrono.srm","content":"other-device"}' >/dev/null
echo "local-save-v3" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"
post /api/sync >/dev/null; wait_sync $((n+1))
[[ "$(last conflicts)" == 1 ]] || fail "conflict not detected"
post /api/conflicts/resolve "{\"path\":\"$RA/saves/Snes9x/Chrono Trigger (USA).srm\",\"keep\":\"server\"}" >/dev/null || fail "resolve failed"
[[ "$(cat "$RA/saves/Snes9x/Chrono Trigger (USA).srm")" == "other-device" ]] || fail "server copy not restored"
ls "$T/data/backups/10/" | grep -q "Chrono Trigger (USA).srm" || fail "no backup of the local save"


echo "6. stray save is left alone until moved"
echo "ROM" > "$RA/roms/snes/EarthBound (USA).sfc"
mkdir -p "$RA/saves/snes"; echo "old-layout" > "$RA/saves/snes/EarthBound (USA).srm"
sleep 3; n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")
post /api/sync >/dev/null; wait_sync $((n+1))
[[ "$(last uploaded)" == 0 ]] || fail "stray save must not be uploaded"
n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; s=json.load(sys.stdin)['sync']['strays']; print(len(s), s[0]['expected_path'] if s else '')")
[[ "$n" == "1 $RA/saves/Snes9x/EarthBound (USA).srm" ]] || fail "stray not reported correctly: $n"
post /api/strays/move "{\"path\":\"$RA/saves/snes/EarthBound (USA).srm\"}" >/dev/null || fail "move failed"
for _ in $(seq 1 20); do [[ "$(last uploaded)" == 1 ]] && break; sleep 0.5; done
[[ -f "$RA/saves/Snes9x/EarthBound (USA).srm" && "$(last uploaded)" == 1 ]] || fail "moved save not synced"

echo "7. PS2 per-game card syncs only with shared cards off"
mkdir -p "$RA/roms/ps2" "$RA/saves/LRPS2"; echo "ISO" > "$RA/roms/ps2/Okami.iso"; echo "card" > "$RA/saves/LRPS2/Okami.ps2"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['sync_note'] for m in json.load(sys.stdin) if 'ps2' in m['platforms']][0])")
[[ "$note" == *pcsx2_shared_memory_cards* ]] || fail "expected a sync note for shared cards: $note"
sleep 3; n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")
post /api/sync >/dev/null; wait_sync $((n+1))
[[ "$(last uploaded)" == 0 ]] || fail "shared-card mode must not upload"
mkdir -p "$RA/LRPS2"; echo 'pcsx2_shared_memory_cards = "disabled"' > "$RA/LRPS2/LRPS2.opt"
post /api/sync >/dev/null; wait_sync $((n+2))
[[ "$(last uploaded)" == 1 ]] || fail "per-game card was not uploaded"

echo "8. PS2 shared card backup and restore"
mkdir -p "$RA/system/pcsx2/memcards"; echo "card-v1" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
post /api/config '{"ps2_cards":"backup"}' >/dev/null
post /api/sync >/dev/null; sleep 3
cards() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump" | python3 -c "import json,sys; c=json.load(sys.stdin)['cards']; print(c[0]['versions'] if c else 0)"; }
[[ "$(cards)" == 1 ]] || fail "card was not backed up"
echo "card-v2" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
post /api/sync >/dev/null; sleep 3
[[ "$(cards)" == 2 ]] || fail "changed card was not backed up again"
post /api/sync >/dev/null; sleep 2
[[ "$(cards)" == 2 ]] || fail "unchanged card must not be uploaded"
echo "broken" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
post /api/memcards/restore '{"slot":1}' >/dev/null || fail "restore failed"
[[ "$(cat "$RA/system/pcsx2/memcards/Mcd001.ps2")" == "card-v2" ]] || fail "restored card has the wrong content"
ls "$T/data/backups/memcards/" | grep -q Mcd001.ps2 || fail "no local backup before restore"

dump() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump"; }
syncnow() { local n; n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])"); post /api/sync >/dev/null; wait_sync $((n+1)); }

echo "9. RomM keeps only the newest versions per save"
post /api/config '{"server_versions":2}' >/dev/null
for v in v4 v5 v6; do sleep 1.1; echo "local-save-$v" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"; syncnow; done
n=$(dump | python3 -c "import json,sys; print(sum(1 for x in json.load(sys.stdin)['saves'] if x['rom_id']==10 and x['slot']=='autosave'))")
[[ "$n" == 2 ]] || fail "expected 2 versions on the server, found $n"

echo "10. states download, save history and restore"
post /api/config '{"states":"sync"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"file_name":"Super Metroid (USA).state1","content":"srv-state"}' >/dev/null
syncnow
[[ "$(cat "$RA/states/Super Metroid (USA).state1" 2>/dev/null)" == "srv-state" ]] || fail "server state not downloaded"
old=$(curl -sf "$API/api/history?rom_id=10" | python3 -c "import json,sys; s=sorted(json.load(sys.stdin)['saves'], key=lambda x: x['updated_at']); print(s[0]['id'])")
want="$(curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/save_content?id=$old")"
post /api/restore "{\"kind\":\"save\",\"id\":$old}" >/dev/null || fail "restore failed"
[[ "$(cat "$RA/saves/Snes9x/Chrono Trigger (USA).srm")" == "$want" ]] || fail "restored save has the wrong content"
newest() { dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==10], key=lambda x: x['updated_at']); print(s[-1]['content'].strip())"; }
for _ in $(seq 1 20); do [[ "$(newest)" == "$want" ]] && break; sleep 0.5; done
[[ "$(newest)" == "$want" ]] || fail "restored save should upload as the newest version"

echo "11. per-game RetroArch overrides"
echo "ISO" > "$RA/roms/ps2/Other.iso"; echo "other-card" > "$RA/saves/LRPS2/Other.ps2"
echo 'pcsx2_shared_memory_cards = "enabled"' > "$RA/LRPS2/Other.opt"
mkdir -p "$RA/special" "$RA/Snes9x"; printf 'savefile_directory = ":/special"\nsort_savefiles_enable = "false"\n' > "$RA/Snes9x/EarthBound (USA).cfg"
echo "special-save" > "$RA/special/EarthBound (USA).srm"
syncnow
n=$(dump | python3 -c "import json,sys; print(sum(1 for x in json.load(sys.stdin)['saves'] if x['rom_id']==21))")
[[ "$n" == 0 ]] || fail "game with shared cards in its own options must not sync"
c=$(dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==12], key=lambda x: x['updated_at']); print(s[-1]['content'].strip())")
[[ "$c" == "special-save" ]] || fail "per-game save folder not used (got $c)"

echo "12. PSP and GameCube saves as zips"
mkdir -p "$RA/roms/psp" "$RA/roms/ngc"
python3 "$ROOT/tests/make_disc.py" cso "$RA/roms/psp/Crisis Core (USA).iso" ULUS10336
python3 "$ROOT/tests/make_disc.py" psp "$RA/roms/psp/Crisis Core (USA).iso" ULUS10336
SD="$RA/saves/PPSSPP/PSP/SAVEDATA"; mkdir -p "$SD/ULUS10336DATA00" "$SD/ULUS10336GAMEDATA"
python3 "$ROOT/tests/make_disc.py" sfo "$SD/ULUS10336DATA00/PARAM.SFO" TITLE="Crisis Core" SAVEDATA_PARAMS=x
echo "psp-save-1" > "$SD/ULUS10336DATA00/DATA.BIN"
python3 "$ROOT/tests/make_disc.py" sfo "$SD/ULUS10336GAMEDATA/PARAM.SFO" TITLE="Crisis Core"
echo "install" > "$SD/ULUS10336GAMEDATA/BIG.BIN"
python3 "$ROOT/tests/make_disc.py" gc "$RA/roms/ngc/Wind Waker (USA).iso" GZLE01
CARD="$RA/saves/dolphin-emu/User/GC/USA/Card A"; mkdir -p "$CARD"; printf 'GZLE01zelda' > "$CARD/01-GZLE-zelda.gci"
syncnow
entries() { dump | python3 -c "
import json,sys,io,zipfile
s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==$1], key=lambda x: x['updated_at'])
import re; name=re.sub(r' \[[^]]*\]', '', s[-1]['file_name']) if s else ''
print(name, sorted(zipfile.ZipFile(io.BytesIO(s[-1]['content'].encode('latin1'))).namelist()) if s else 'none')"; }
[[ "$(entries 30)" == "ULUS10336.zip ['ULUS10336DATA00/DATA.BIN', 'ULUS10336DATA00/PARAM.SFO']" ]] || fail "PSP zip: $(entries 30)"
[[ "$(entries 40)" == "Wind Waker (USA).zip ['01-GZLE-zelda.gci']" ]] || fail "GC zip: $(entries 40)"
syncnow
[[ "$(last uploaded)" == 0 ]] || fail "unchanged PSP and GC saves must not upload again"
python3 - "$SD" > "$T/new.zip.b64" <<'PY'
import base64, io, sys, zipfile
buf = io.BytesIO()
with zipfile.ZipFile(buf, "w", zipfile.ZIP_DEFLATED) as z:
    z.writestr("ULUS10336DATA00/DATA.BIN", "psp-save-2\n")
    z.writestr("ULUS10336DATA00/PARAM.SFO", open(sys.argv[1] + "/ULUS10336DATA00/PARAM.SFO", "rb").read())
print(base64.b64encode(buf.getvalue()).decode())
PY
sleep 1.1
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d "{\"rom_id\":30,\"slot\":\"autosave\",\"file_name\":\"ULUS10336.zip\",\"content_b64\":\"$(cat "$T/new.zip.b64")\"}" >/dev/null
syncnow
[[ "$(cat "$SD/ULUS10336DATA00/DATA.BIN")" == "psp-save-2" ]] || fail "PSP save from the server not unpacked"
[[ -f "$SD/ULUS10336GAMEDATA/BIG.BIN" ]] || fail "game data folder must be left alone"
ls "$T/data/backups/30/" | grep -q "ULUS10336.zip" || fail "no backup of the PSP save"

echo "13. in-app update: only a correctly signed release installs"
SIGN="$ROOT/build/host/release-sign"
upd() { curl -sf "$API/api/update" | python3 -c "import json,sys; u=json.load(sys.stdin); print(u['$1'])"; }
# Publishes a release; $1 says what to break: tamper, wrongkey, unsigned, ps4 or nothing.
publish() {
  rm -f "$REL"/*; printf '\x7fELF new-payload %s' "$RANDOM" > "$REL/romm-sync.elf"
  "$SIGN" manifest v9.9.9 "$REL/romm-sync.elf" > "$REL/manifest.json"
  # ps4: a correctly signed manifest, but for the PS4 payload
  [[ "$1" == ps4 ]] && cp "$REL/romm-sync.elf" "$REL/romm-sync-ps4.elf" && "$SIGN" manifest v9.9.9 "$REL/romm-sync-ps4.elf" > "$REL/manifest.json"
  local key; key="$(cat "$ROOT/tests/update-test.key")"
  [[ "$1" == wrongkey ]] && key="$("$SIGN" keygen | sed -n 's/^secret: //p')"
  RELEASE_SIGNING_KEY="$key" "$SIGN" sign "$REL/manifest.json" > "$REL/manifest.sig" 2>/dev/null
  [[ "$1" == tamper ]] && python3 -c "import sys; p=sys.argv[1]; b=bytearray(open(p,'rb').read()); b[-1]^=1; open(p,'wb').write(b)" "$REL/romm-sync.elf"
  local names='"romm-sync.elf", "manifest.json", "manifest.sig"'; [[ "$1" == unsigned ]] && names='"romm-sync.elf"'
  python3 - "$REL" "http://127.0.0.1:$MOCK_PORT/_github/assets" "$names" <<'PY'
import json, sys
d, base, names = sys.argv[1], sys.argv[2], json.loads("[" + sys.argv[3] + "]")
json.dump({"tag_name": "v9.9.9", "body": "Test notes", "html_url": "https://example.invalid/r",
           "assets": [{"name": n, "browser_download_url": base + "/" + n} for n in names]}, open(d + "/release.json", "w"))
PY
  post /api/update/check >/dev/null || fail "update check refused"
  for _ in $(seq 1 20); do [[ "$(upd state)" != checking ]] && break; sleep 0.5; done
  [[ "$(upd available)" == True && "$(upd latest)" == 9.9.9 ]] || fail "release 9.9.9 not found: $(upd error)"
}
install_err() { # installs and prints the error it ends with, or the state it reached
  post /api/update/install >/dev/null || { echo refused; return; }
  for _ in $(seq 1 40); do local s; s="$(upd state)"; [[ "$s" == error ]] && { upd error; return; }; [[ "$s" == restarting ]] && { echo restarting; return; }; sleep 0.5; done
  echo "timeout: $(upd state)"
}
publish tamper;   r="$(install_err)"; [[ "$r" == *"signed checksum"* ]] || fail "tampered payload: $r"
publish wrongkey; r="$(install_err)"; [[ "$r" == *"signature doesn't match"* ]] || fail "untrusted key: $r"
publish unsigned; r="$(install_err)"; [[ "$r" == refused ]] || fail "unsigned release must be refused: $r"
publish ps4;      r="$(install_err)"; [[ "$r" == *"manifest is for romm-sync-ps4.elf"* ]] || fail "another console's payload: $r"
[[ "$(cat "$T/etaHEN/payloads/romm-sync.elf")" == old-payload && ! -f "$T/data/launched.elf" ]] || fail "a rejected update touched the payload"
publish ok;       r="$(install_err)"; [[ "$r" == restarting ]] || fail "good update: $r"
for _ in $(seq 1 10); do [[ -f "$T/data/launched.elf" ]] && break; sleep 0.5; done
cmp -s "$REL/romm-sync.elf" "$T/data/launched.elf" || fail "the new payload was not launched"
cmp -s "$REL/romm-sync.elf" "$T/etaHEN/payloads/romm-sync.elf" || fail "the installed payload was not replaced"
[[ "$(cat "$T/etaHEN/payloads/romm-sync.elf.bak")" == old-payload ]] || fail "no backup of the old payload"

echo "14. diagnostics report leaves out the token"
curl -sf "$API/api/diagnostics" > "$T/diag.txt" || fail "no diagnostics report"
grep -q "RomM Sync diagnostics" "$T/diag.txt" || fail "diagnostics report is empty"
tok="$(python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['token'])" "$T/data/config.json")"
[[ -n "$tok" ]] && ! grep -qF "$tok" "$T/diag.txt" || fail "the diagnostics report contains the token"

echo "15. one emulator per system when several play it"
kinds=$(curl -sf "$API/api/setup/detect" | python3 -c "import json,sys; print([(c['kind'], c['title_id']) for c in json.load(sys.stdin)])")
[[ "$kinds" == *"('standalone', 'TEST00001')"* ]] || fail "the installed standalone emulator wasn't offered: $kinds"
snes_dirs() { curl -sf "$API/api/paths" | python3 -c "import json,sys; print(sorted(m['rom_dir'] for m in json.load(sys.stdin) if m['platforms'].split(',')[0] == 'snes'))"; }
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/snesemu\"],\"choices\":{\"snes\":\"$T/snesemu\"}}" >/dev/null || fail "setup with choices refused"
[[ "$(snes_dirs)" == "['$T/snesemu/roms']" ]] || fail "SNES should only go to the standalone emulator: $(snes_dirs)"
sel=$(curl -sf "$API/api/systems" | python3 -c "import json,sys; print([(s['system'], s['selected'], len(s['options'])) for s in json.load(sys.stdin)])")
[[ "$sel" == "[('snes', 'testsnes', 2)]" ]] || fail "systems: $sel"
post /api/systems '{"system":"snes","profile":"retroarch-PPSA12345"}' >/dev/null || fail "switching SNES refused"
[[ "$(snes_dirs)" == "['$HB/content/snes']" ]] || fail "SNES should be back with RetroArch: $(snes_dirs)"
curl -s -X POST "$API/api/systems" -d '{"system":"snes","profile":"nope"}' | grep -q error || fail "an unknown emulator must be refused"
[[ "$(snes_dirs)" == "['$HB/content/snes']" ]] || fail "a refused choice changed something: $(snes_dirs)"

echo "16. standalone emulators: saves by serial and hash, one save for two emulators, extras, shared cards"
# Like PSXS5 (cards named by disc serial), PS5N64 (saves by the game file's
# FNV-1a hash), a second N64 emulator keeping the same format (RetroArch-like
# names), PS5SX2 (two shared cards) and one found but not supported yet.
mkdir -p "$T/psxemu/games" "$T/psxemu/saves" "$T/psxemu/states" "$T/n64emu/games" "$T/n64emu/saves" \
         "$T/n64b/roms" "$T/n64b/saves" "$T/ps2emu/games" "$T/nope"
cat > "$T/catalog.json" <<EOF
[{"id": "testsnes", "name": "Test SNES", "title_ids": ["TEST00001"],
  "profile": {"root": "$T/snesemu", "rom_dir": "{root}/roms", "save_dir": "{root}/saves", "state_dir": "{root}/states",
              "save_exts": ".srm", "platforms": [{"romm": ["snes", "sfam"], "dir": "snes"}]}},
 {"id": "psxlike", "name": "PSX-like", "detect": ["$T/psxemu"],
  "profile": {"root": "$T/psxemu", "rom_dir": "{root}/games", "save_dir": "{root}/saves", "state_dir": "{root}/states",
              "save_exts": ".mcd", "save_name": "{serial}_1{ext}", "save_stems": "{serial}_1",
              "state_exts": ".state*", "state_stems": "{rom_stem_safe}", "state_name": "{rom_stem_safe}{ext}",
              "platforms": [{"romm": ["psx", "ps1"], "dir": "psx", "save_format": "psx-card"}]}},
 {"id": "n64like", "name": "N64-like", "detect": ["$T/n64emu"],
  "profile": {"root": "$T/n64emu", "rom_dir": "{root}/games", "save_dir": "{root}/saves", "state_dir": "{root}/saves",
              "save_exts": ".sav", "save_name": "{rom_fnv64}{ext}", "save_stems": "{rom_fnv64}",
              "platforms": [{"romm": ["n64"], "dir": "n64", "save_format": "n64-srm"}]}},
 {"id": "n64b", "name": "Second N64", "detect": ["$T/n64b"],
  "profile": {"root": "$T/n64b", "rom_dir": "{root}/roms", "save_dir": "{root}/saves", "save_exts": ".srm",
              "platforms": [{"romm": ["n64"], "dir": "n64", "save_format": "n64-srm"}]}},
 {"id": "ps2like", "name": "PS2-like", "detect": ["$T/ps2emu"],
  "profile": {"root": "$T/ps2emu", "rom_dir": "{root}/games", "sync_saves": false, "memcards": "{root}/Mcd001.ps2,{root}/Mcd002.ps2",
              "platforms": [{"romm": ["ps2"], "dir": "ps2"}]}},
 {"id": "nope", "name": "Not yet", "detect": ["$T/nope"], "ready": false, "note": "not supported yet",
  "profile": {"root": "$T/nope", "rom_dir": "{root}/games", "platforms": [{"romm": ["n64"], "dir": "n64"}]}}]
EOF
python3 "$ROOT/tests/make_disc.py" psxbin "$T/psxemu/games/Test Quest (USA).bin" SLUS_123.45
printf 'card-from-psx' > "$T/psxemu/saves/SLUS-12345_1.mcd"
printf 'state-from-psx' > "$T/psxemu/states/Test_Quest__USA_.state0"
printf 'ROM:Test Racer (U).z64' > "$T/n64emu/games/Test Racer (U).z64"
cp "$T/n64emu/games/Test Racer (U).z64" "$T/n64b/roms/Test Racer (U).z64"
fnv=$(python3 -c "
h=0xcbf29ce484222325
for b in open('$T/n64emu/games/Test Racer (U).z64','rb').read(): h=((h^b)*0x100000001b3)&0xFFFFFFFFFFFFFFFF
print(f'{h:016x}')")
printf 'n64-save' > "$T/n64emu/saves/$fnv.sav"
printf 'card-ps2' > "$T/ps2emu/Mcd001.ps2"
printf 'ROM:Old Racer (U).z64' > "$T/n64emu/games/Old Racer (U).z64"   # already there before the extra is turned on
det=$(curl -sf "$API/api/setup/detect" | python3 -c "import json,sys; print(sorted((c['name'], c.get('ready', True)) for c in json.load(sys.stdin) if c['kind'] == 'standalone'))")
[[ "$det" == *"('Not yet', False)"* && "$det" == *"('PSX-like', True)"* ]] || fail "detection of the new kinds: $det"
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\"],
  \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\"},\"also\":{\"n64\":[\"$T/n64b\"]}}" >/dev/null \
  || fail "setup with extras refused"
for _ in $(seq 1 20); do [[ -f "$T/n64b/roms/Old Racer (U).z64" ]] && break; sleep 0.25; done
[[ -f "$T/n64b/roms/Old Racer (U).z64" ]] || fail "turning an extra on must give it the games already there"
for _ in $(seq 1 20); do grep -q "NOTIFY.*now also sync with" "$T/daemon.log" && break; sleep 0.25; done
grep -q "NOTIFY\] RomM Sync: [0-9]* N64 games\{0,1\} now also sync with" "$T/daemon.log" || fail "no notice that the games now also sync with the extra"
dirs=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print(sorted(m['rom_dir'] for m in json.load(sys.stdin)))")
[[ "$dirs" != *"$T/nope"* ]] || fail "an emulator not supported yet got a profile: $dirs"
post /api/config '{"states":"upload"}' >/dev/null
post /api/sync >/dev/null; wait_sync 16
dump() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump"; }
saves=$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(sorted((s['rom_id'], s['file_name'], s['slot']) for s in (d['saves'].values() if isinstance(d['saves'], dict) else d['saves']) if s['rom_id'] in (50, 60)))")
# (RomM puts the upload's time in the name: "SLUS-12345_1 [2026-...].mcd".)
[[ "$saves" == *"(50, 'SLUS-12345_1 ["*"].mcd', 'autosave')"* ]] || fail "the card named by serial didn't sync: $saves"
[[ "$saves" == *"(60, '$fnv ["*"].sav', 'autosave')"* ]] || fail "the save named by hash didn't sync: $saves"
[[ $(grep -o "(60," <<<"$saves" | wc -l) -eq 1 ]] || fail "two emulators sharing a save must upload one: $saves"
[[ "$(cat "$T/n64b/saves/Test Racer (U).srm" 2>/dev/null)" == "n64-save" ]] || fail "the shared save didn't reach the second emulator"
states=$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(sorted(s['file_name'] for s in (d['states'].values() if isinstance(d['states'], dict) else d['states']) if s['rom_id'] == 50))")
[[ "$states" == "['Test_Quest__USA_"*".state0']" ]] || fail "the state named by the safe name didn't sync: $states"
c2=$(dump | python3 -c "import json,sys; print([c['versions'] for c in json.load(sys.stdin)['cards'] if c['emulator'] == 'ps2like' and c['name'].endswith('PS2-like slot 1')])")
[[ "$c2" == "[1]" ]] || fail "the shared PS2 card wasn't backed up: $c2"
# A save from another device reaches both N64 emulators.
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":60,"slot":"autosave","file_name":"Test Racer (U).srm","content":"n64-from-elsewhere","emulator":"other"}' >/dev/null
post /api/sync >/dev/null; wait_sync 17
[[ "$(cat "$T/n64emu/saves/$fnv.sav")" == "n64-from-elsewhere" && "$(cat "$T/n64b/saves/Test Racer (U).srm")" == "n64-from-elsewhere" ]] \
  || fail "a downloaded save must reach every emulator that shares it"
# A download goes to the main emulator and is linked into the extra.
post /api/download '{"rom_id":61}' >/dev/null; sleep 2
[[ -f "$T/n64emu/games/Other Racer (U).z64" && -f "$T/n64b/roms/Other Racer (U).z64" ]] || fail "the download didn't reach the extra emulator"
[[ "$(stat -f %i "$T/n64emu/games/Other Racer (U).z64" 2>/dev/null || stat -c %i "$T/n64emu/games/Other Racer (U).z64")" == \
   "$(stat -f %i "$T/n64b/roms/Other Racer (U).z64" 2>/dev/null || stat -c %i "$T/n64b/roms/Other Racer (U).z64")" ]] || fail "the extra copy should be a hard link"

echo "17. paired with another server, the old server's game and save ids are forgotten"
hist() { curl -sf "$API/api/status" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['sync']['history']))"; }
n=$(hist)
post /api/pair/forget >/dev/null
post /api/config '{"tls_verify":false}' >/dev/null
post /api/pair/start "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}" >/dev/null
for _ in $(seq 1 20); do [[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['paired'])")" == True ]] && break; sleep 0.5; done
grep -q "new pairing: the games are matched again" "$T/daemon.log" || fail "pairing again didn't forget the old server's records"
# Pairing syncs by itself: everything goes up to the new server, nothing fails.
wait_sync $((n + 1))
[[ "$(last reason)" == "paired" ]] || fail "expected the pairing's own sync: $(last reason)"
[[ "$(last failed)" == 0 ]] || fail "after pairing with another server nothing may fail: $(last failed) failed"
[[ "$(last uploaded)" -ge 1 ]] || fail "the saves must go up to the new server: $(last uploaded)"

echo "18. a game's details, the folder picker, and folders picked for a system no emulator plays"
det=$(curl -sf "$API/api/roms/11/details" | python3 -c "
import json,sys; d=json.load(sys.stdin)
print(d.get('year'), d.get('publishers'), d.get('genres'), d.get('rating'), int(d.get('main_story_s',0)/3600), d.get('status'), d.get('play_ms'), d.get('sessions'))")
[[ "$det" == "1994 Nintendo Platform, Adventure 92.5 8 finished 5400000 2" ]] || fail "game details: $det"
root=$(curl -sf "$API/api/fs" | python3 -c "import json,sys; print(json.load(sys.stdin)['folders'])")
[[ "$root" == "['$T']" ]] || fail "the picker's roots: $root"
mkdir -p "$T/mine/games" "$T/mine/saves"
sub=$(curl -sf "$API/api/fs?path=$T/mine" | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['folders'], j['parent'] == '$T')")
[[ "$sub" == "['games', 'saves'] True" ]] || fail "listing a folder: $sub"
curl -s "$API/api/fs?path=/etc" | grep -q error || fail "the picker must stay inside its roots"
curl -s "$API/api/fs?path=$T/../.." | grep -q error || fail "the picker must refuse .."
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"custom\":[{\"slugs\":[\"psp\"],\"name\":\"PSP\",\"rom_dir\":\"$T/mine/games\",\"save_dir\":\"$T/mine/saves\"}]}" >/dev/null \
  || fail "setup with picked folders refused"
mine=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([(m['rom_dir'], m['save_dir']) for m in json.load(sys.stdin) if m['rom_dir'].startswith('$T/mine')])")
[[ "$mine" == "[('$T/mine/games', '$T/mine/saves')]" ]] || fail "the picked folders weren't used: $mine"
# An emulator with no preset, set up by its folders, for a system RetroArch plays too: an extra.
mkdir -p "$T/porp/games" "$T/porp/saves"
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"custom\":[{\"id\":\"newemu-snes\",\"slugs\":[\"snes\",\"sfam\"],\"name\":\"NewEmu (SNES)\",\"rom_dir\":\"$T/porp/games\",\"save_dir\":\"$T/porp/saves\"}]}" >/dev/null \
  || fail "setup with an emulator's own folders refused"
porp=$(curl -sf "$API/api/config" | python3 -c "
import json,sys
for p in json.load(sys.stdin)['profiles']:
    if p['id'].startswith('custom-'): print(p['id'], p.get('extra'), p['rom_dir'] == '$T/porp/games')")
[[ "$porp" == "custom-newemu-snes ['snes'] True" ]] || fail "an emulator with no preset should be an extra next to RetroArch: $porp"

echo "19. every cover kept on the console, then only the missing and changed ones fetched"
# Paired with the HTTPS mock since scenario 17.
TM="https://127.0.0.1:$TLS_PORT"
hits() { curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; print(json.load(sys.stdin)['asset_hits'])"; }
covers_done() { # waits for a full pass to finish; prints done/total
  for _ in $(seq 1 120); do
    r=$(curl -sf "$API/api/status" | python3 -c "import json,sys; c=json.load(sys.stdin)['covers']; print(c['state'], c['done'], c['total'])")
    set -- $r; [[ "$1" == done && "$2" == "$3" && "$3" -gt 0 ]] && { echo "$2/$3"; return; }; sleep 0.5
  done; echo "timeout: $r"
}
post /api/covers/refresh >/dev/null
first=$(covers_done); [[ "$first" != timeout* ]] || fail "the full cover pass didn't finish: $first"
cached=$(ls "$T/data/cache/assets" | grep -vc '\.missing$')
[[ "$cached" -ge "${first#*/}" ]] || fail "every game's cover should be on the console: $cached files for $first"
before=$(hits)
p=$(curl -sf "$API/api/roms?platform_id=1&limit=1" | python3 -c "import json,sys; print(json.load(sys.stdin)['items'][0]['cover'])")
curl -sf -o /dev/null "$API/cover?p=$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$p")" || fail "a cached cover wasn't served"
post /api/covers/refresh >/dev/null; sleep 3; covers_done >/dev/null
[[ "$(hits)" == "$before" ]] || fail "a second pass must not download covers it has: $before then $(hits)"
curl -sfk -X POST "$TM/_admin/cover_ts" -d '{"rom_id":11}' >/dev/null
post /api/covers/refresh >/dev/null; sleep 3; covers_done >/dev/null
[[ "$(hits)" == "$((before + 1))" ]] || fail "only the changed cover should be fetched again: $before then $(hits)"

echo "20. files that aren't games are hidden and never downloaded; versions say what sets them apart"
lib=$(curl -sf "$API/api/roms?platform_id=1&limit=50" | python3 -c "
import json,sys
for g in json.load(sys.stdin)['items']: print(g['id'], g.get('not_game', False), g.get('version', ''), g.get('versions', 0))")
grep -q "^13 True" <<<"$lib" || fail "systeminfo.txt must be marked as not a game: $lib"
grep -q "^14 False" <<<"$lib" || fail "a Mega Drive .md ROM is a game: $lib"
grep -q "^12 False USA · Rev 1 · Beta 2$" <<<"$lib" || fail "the version label: $lib"
grep -q "^10 False  0$" <<<"$lib" || fail "a game with one version has no label: $lib"
post /api/download '{"rom_id":13}' >/dev/null; sleep 1
grep -q "systeminfo.txt isn't a game" "$T/daemon.log" || fail "a file that isn't a game was downloaded"
[[ -z "$(find "$T" -name systeminfo.txt -not -path '*/data/*')" ]] || fail "systeminfo.txt reached an emulator's folder"

echo "PASS"
