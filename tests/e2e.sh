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
  ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_APP_DIRS="$T/apps" \
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
post /api/download '{"rom_id":11}' >/dev/null; sleep 1
[[ -f "$RA/roms/snes/Super Metroid (USA).sfc" ]] || fail "ROM not downloaded"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":11,"slot":"autosave","file_name":"Super Metroid (USA).srm","content":"server-save"}' >/dev/null
post /api/sync >/dev/null; wait_sync 2
[[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "server-save" ]] || fail "server save not downloaded"

ci=$(curl -sf "$API/api/roms?platform_id=1" | python3 -c "import json,sys; print(sorted(json.load(sys.stdin)['char_index'].items()))")
[[ "$ci" == "[('c', 0), ('e', 1), ('s', 2)]" ]] || fail "library char_index for the A-Z ribbon: $ci"

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

echo "PASS"
