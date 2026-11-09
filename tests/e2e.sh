#!/usr/bin/env bash
# End-to-end test of the host build against tests/mock_romm.py.
#   make host tools && tests/e2e.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
MOCK_PORT=8899 WEB_PORT=8781
API="http://127.0.0.1:${WEB_PORT}"
trap 'kill $(jobs -p) 2>/dev/null || true; [[ -n "${KEEP_T:-}" ]] || rm -rf "$T"' EXIT

fail() { echo "FAIL: $*"; echo "--- daemon log"; tail -80 "$T/daemon.log"; exit 1; }
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
# And once more behind mTLS: a client certificate from its own CA, as a .p12.
MTLS_PORT=8897
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=client-ca -keyout "$T/cca.key" -out "$T/cca.crt" 2>/dev/null
openssl req -newkey rsa:2048 -nodes -subj /CN=rommps-test -keyout "$T/client.key" -out "$T/client.csr" 2>/dev/null
openssl x509 -req -in "$T/client.csr" -CA "$T/cca.crt" -CAkey "$T/cca.key" -CAcreateserial -days 1 -out "$T/client.crt" 2>/dev/null
openssl pkcs12 -export -inkey "$T/client.key" -in "$T/client.crt" -passout pass:s3cret -out "$T/client.p12"
openssl req -x509 -newkey rsa:2048 -nodes -days 1 -subj /CN=stranger -keyout "$T/other.key" -out "$T/other.crt" 2>/dev/null
openssl pkcs12 -export -inkey "$T/other.key" -in "$T/other.crt" -passout pass: -out "$T/other.p12"
MOCK_CLIENT_CA="$T/cca.crt" python3 "$ROOT/tests/mock_romm.py" $MTLS_PORT "$T/tls.pem" 2>"$T/mock-mtls.log" &
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
# Apps in $MB can have mounts, as on the PS5 (the host build lists them in data/mounts.txt).
MB="$T/mountable"; mkdir -p "$MB"
ROMM_SYNC_HOMEBREW="$T/homebrew" ROMM_SYNC_MOUNTS="$MB" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_AUTOSTART="$T/etaHEN/payloads" \
  ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_APP_DIRS="$T/apps" ROMM_SYNC_FS_ROOTS="$T" ROMM_SYNC_COVERS_TICK=1 ROMM_SYNC_RA2_EVERY=1 ROMM_SYNC_INSTALLED_SIG_SEC=2 \
  ROMM_SYNC_UPDATE_URL="http://127.0.0.1:$MOCK_PORT/_github/release" ROMM_SYNC_PRESETS_URL="http://127.0.0.1:$MOCK_PORT/_github/assets" \
  "$ROOT/build/host/romm-sync" >"$T/daemon.log" 2>&1 &
sleep 6   # let the (unpaired, skipped) startup sync pass

echo "web UI script parses"
if command -v node >/dev/null; then
    python3 -c "import re,sys; print('\n'.join(re.findall(r'<script>(.*?)</script>', open(sys.argv[1]).read(), re.S)))" "$ROOT/ui/index.html" > "$T/ui.js"
    node --check "$T/ui.js" || fail "the web UI's script doesn't parse"
fi

echo "0. self-signed HTTPS needs certificate checks turned off"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *"doesn't trust"* ]] || fail "untrusted certificate not reported: $r"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *"doesn't trust"* ]] || fail "an address without a scheme must not fall back to http on a certificate error: $r"
post /api/config '{"tls_verify":false}' >/dev/null
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$TLS_PORT\"}")
[[ "$r" == *pending* ]] || fail "pairing with checks off failed: $r"
post /api/pair/forget >/dev/null

echo "0b. a server behind mTLS takes a client certificate (.p12)"
cert() { curl -s -X POST "$API/api/client-cert" -d "{\"p12\":\"$(base64 -w0 "$1")\",\"password\":\"$2\"}"; }
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$MTLS_PORT\"}")
[[ "$r" == *"asks for a client certificate"* ]] || fail "missing client certificate not reported: $r"
r=$(cert "$T/client.p12" wrong); [[ "$r" == *"wrong password"* ]] || fail "wrong password not reported: $r"
r=$(cert "$T/tls.crt" ""); [[ "$r" == *"not a PKCS#12"* ]] || fail "a file that isn't a .p12 was taken: $r"
r=$(curl -sf "$API/api/client-cert"); [[ "$r" == *'"active":false'* && "$r" == *'"file":""'* ]] || fail "a refused upload changed the certificate: $r"
r=$(cert "$T/other.p12" ""); [[ "$r" == *'"active":true'* && "$r" == *"CN=stranger"* ]] || fail "a .p12 without a password wasn't taken: $r"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$MTLS_PORT\"}")
[[ "$r" == *"didn't accept"* ]] || fail "a certificate the server won't take not reported: $r"
r=$(cert "$T/client.p12" s3cret); [[ "$r" == *'"active":true'* && "$r" == *"CN=rommps-test"* && "$r" == *"valid until"* ]] || fail "certificate upload: $r"
r=$(curl -s -X POST "$API/api/pair/start" -d "{\"server_url\":\"https://127.0.0.1:$MTLS_PORT\"}")
[[ "$r" == *pending* ]] || fail "pairing through mTLS failed: $r"
[[ -f "$T/data/client.p12" ]] || fail "the certificate wasn't saved in the data folder"
python3 -c "import json,sys; c=json.load(open(sys.argv[1])); assert c['client_cert']=='client.p12' and c['client_cert_password']=='s3cret', c" "$T/data/config.json" \
  || fail "the certificate isn't in config.json"
r=$(curl -sf "$API/api/config"); [[ "$r" != *s3cret* && "$r" == *'"client_cert":"client.p12"'* ]] || fail "the web UI's config shows the certificate's password: $r"
post /api/pair/forget >/dev/null
r=$(curl -sf -X POST "$API/api/client-cert/remove"); [[ "$r" == *'"active":false'* ]] || fail "certificate not removed: $r"
[[ ! -f "$T/data/client.p12" ]] || fail "client.p12 left behind"
grep -q s3cret "$T/data/config.json" && fail "the password stayed in config.json"
post /api/config '{"tls_verify":true}' >/dev/null

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
names=$(curl -sf "$API/api/platforms" | python3 -c "import json,sys; print(sorted(p['name'] for p in json.load(sys.stdin)))")
[[ "$names" == *"'Sony PlayStation 2'"* && "$names" == *"'Super Nintendo Entertainment System'"* && "$names" == *"'Nintendo GameCube'"* ]] \
  || fail "platforms must go by their makers' names: $names"

echo "2. game download brings its server save along"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":11,"slot":"autosave","file_name":"Super Metroid (USA).srm","content":"server-save"}' >/dev/null
post /api/download '{"rom_id":11,"name":"Super Metroid"}' >/dev/null
[[ "$(curl -sf "$API/api/downloads" | python3 -c "import json,sys; print([d['name'] for d in json.load(sys.stdin) if d.get('rom_id') == 11][-1])")" == "Super Metroid" ]] \
  || fail "a download must show its name from the start"
wait_sync 2
[[ -f "$RA/roms/snes/Super Metroid (USA).sfc" ]] || fail "ROM not downloaded"
[[ "$(last reason)" == "game downloaded" ]] || fail "a finished download must sync the game's saves"
[[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "server-save" ]] || fail "server save not downloaded"
for _ in $(seq 1 20); do
  dl=$(curl -sf "$API/api/downloads" | python3 -c "import json,sys; d=[d for d in json.load(sys.stdin) if d.get('rom_id') == 11][-1]; print(d['status'], d.get('saves'))")
  [[ "$dl" == "done 1" ]] && break; sleep 0.25
done
[[ "$dl" == "done 1" ]] || fail "the download must end done, with the save it synced: $dl"

echo "2b. folder roms: one iso per folder, and a cue+bin set"
post /api/download '{"rom_id":22,"name":"Downhill Domination"}' >/dev/null
post /api/download '{"rom_id":25,"name":"War of the Monsters"}' >/dev/null
dl_wait() { # wait for a downloaded path to exist
  for _ in $(seq 1 30); do [[ -e "$1" ]] && return 0; sleep 0.5; done; fail "not downloaded: $1"
}
D="$RA/roms/ps2/Downhill Domination (USA)"
W="$RA/roms/ps2/War of the Monsters (USA)"
dl_wait "$D/Downhill Domination (USA).iso"; dl_wait "$W/War of the Monsters (USA).cue"
[[ -d "$D" && -d "$W" ]] || fail "a folder rom must land in a folder named after its fs_name"
[[ "$(cat "$D/Downhill Domination (USA).iso")" == "ROM:Downhill Domination (USA).iso" ]] || fail "iso content wrong"
[[ -f "$W/War of the Monsters (USA).bin" ]] || fail "cue's bin not downloaded"
[[ "$(cat "$W/War of the Monsters (USA).bin")" == "ROM:War of the Monsters (USA).bin" ]] || fail "bin content wrong"
[[ ! -f "$RA/roms/ps2/Downhill Domination (USA)" ]] || fail "folder rom written as a single extensionless file"

ci=$(curl -sf "$API/api/roms?platform_id=1" | python3 -c "import json,sys; print(sorted(json.load(sys.stdin)['char_index'].items()))")
[[ "$ci" == "[('c', 0), ('e', 1), ('m', 2), ('s', 3)]" ]] || fail "library char_index for the A-Z ribbon: $ci"
inst=$(curl -sf "$API/api/installed" | python3 -c "import json,sys; print(sorted([(g['id'], g['name'], g['platform_id'] > 0) for g in json.load(sys.stdin)]))")
[[ "$inst" == "[(10, 'Chrono Trigger', True), (11, 'Super Metroid', True), (22, 'Downhill Domination', True), (25, 'War of the Monsters', True)]" ]] \
  || fail "the games on this console, for the app: $inst"
# A game deleted by hand: it's no longer on the console, deleting it again
# isn't an error, and it downloads again.
sig() { curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['installed_sig'])"; }
sleep 2.2; before=$(sig); mv "$W" "$T/war.bak"
onc() { curl -sf "$API/api/roms/$1/on_console" | python3 -c "import json,sys; print(json.load(sys.stdin)['installed'])"; }
expect_eq() { [[ "$2" == "$3" ]] || fail "$1: got '$2', expected '$3'"; }
expect_eq "the sheet's check, right away" "$(onc 25)" False
expect_eq "the status looks at the disk only every so often" "$(sig)" "$before"
sleep 2.2
[[ "$(sig)" != "$before" ]] || fail "installed_sig didn't change when a game was deleted by hand"
on=$(curl -sf "$API/api/roms?platform_id=0&search=war" | python3 -c "import json,sys; print([g['installed'] for g in json.load(sys.stdin)['items'] if g['id'] == 25])")
[[ "$on" == "[False]" ]] || fail "a game deleted by hand still shows as downloaded: $on"
curl -sf "$API/api/installed" | grep -q '"id":25' && fail "a game deleted by hand is still on the shelf"
post /api/delete '{"rom_id":25}' >/dev/null || fail "deleting a game already gone failed"
code=$(curl -s -o /dev/null -w '%{http_code}' -X POST "$API/api/delete" -d '{}')
[[ "$code" == 400 ]] || fail "a delete without a game: HTTP $code"
post /api/download '{"rom_id":25,"name":"War of the Monsters"}' >/dev/null
dl_wait "$W/War of the Monsters (USA).cue"; rm -rf "$T/war.bak"
for _ in $(seq 1 20); do [[ "$(onc 25)" == True ]] && break; sleep 0.25; done
expect_eq "the sheet's check after downloading again" "$(onc 25)" True
expect_eq "installed_sig right after our own download" "$(sig)" "$before"
all=$(curl -sf "$API/api/roms?platform_id=0&search=metroid" | python3 -c "import json,sys; print([(g['name'], g['platform_id']) for g in json.load(sys.stdin)['items']])")
[[ "$all" == "[('Super Metroid', 1)]" ]] || fail "a search across every platform: $all"
colls=$(curl -sf "$API/api/collections" | python3 -c "import json,sys; print([(c['kind'], c['id'], c['name'], c['rom_count'], c['owner']) for c in json.load(sys.stdin)])")
[[ "$colls" == "[('regular', '7', 'Favourites', 3, ''), ('regular', '9', 'Arcade Nights', 1, 'anna'), ('smart', '3', 'SNES picks', 2, ''), ('virtual', 'franchise-Metroid', 'Metroid', 1, ''), ('virtual', 'genre-Action', 'Action', 3, '')]" ]] || fail "collections by kind and type, own first, the empty one left out: $colls"
colls=$(curl -sf "$API/api/collections?kind=virtual" | python3 -c "import json,sys; print([(c['name'], c['type']) for c in json.load(sys.stdin)])")
[[ "$colls" == "[('Metroid', 'franchise'), ('Action', 'genre')]" ]] || fail "virtual collections, with their type: $colls"
cg=$(curl -sf "$API/api/roms?collection_kind=virtual&collection_id=genre-Action" | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['total'], [(g['name'], g['installed']) for g in j['items']])")
[[ "$cg" == "3 [('Chrono Trigger', True), ('Crisis Core', False), ('Mega Test', False)]" ]] || fail "a virtual collection's games: $cg"
# A whole collection: what isn't here and has an emulator is queued.
qd=$(post /api/download '{"collection_kind":"virtual","collection_id":"genre-Action"}' | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['queued'], j['skipped'])")
[[ "$qd" == "2 1" ]] || fail "a collection's install: queued and skipped: $qd"
dl_wait "$RA/roms/snes/Mega Test (USA).md"; dl_wait "$RA/roms/psp/Crisis Core (USA).iso"
for _ in $(seq 1 20); do [[ "$(onc 14)$(onc 30)" == TrueTrue ]] && break; sleep 0.25; done
qd=$(post /api/download '{"collection_kind":"virtual","collection_id":"genre-Action"}' | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['queued'], j['skipped'])")
[[ "$qd" == "0 3" ]] || fail "a collection installed again queues nothing: $qd"
post /api/delete '{"rom_id":14}' >/dev/null && post /api/delete '{"rom_id":30}' >/dev/null || fail "couldn't remove the collection's games"
# RomM's ids for virtual collections are base64 of their name and type: long, with +, / and = in them. One holds
# a version RomM's grouping fronts with another (Square USA: Final Fantasy IX's discs, not its .m3u).
VID=$(python3 -c "import base64,json; print(base64.b64encode(json.dumps({'name': 'Disc Sets ~~~ Square USA >>>? xxxxxxxxxxxxxxx', 'type': 'company'}).encode()).decode())")
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d '{"id":964,"platform_id":1,"name":"Disc Two","fs_name":"Disc Two (USA).sfc","grouped_away":true}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/virtual" -d "{\"id\":\"$VID\",\"name\":\"Disc Sets\",\"type\":\"company\",\"rom_ids\":[964,10]}" >/dev/null
listed=$(curl -sf "$API/api/collections?kind=virtual" | python3 -c "import json,sys; print([c['id'] for c in json.load(sys.stdin) if c['name'] == 'Disc Sets'])")
[[ "$listed" == "['$VID']" ]] || fail "a virtual collection's id must reach the app whole (${#VID} characters): $listed"
vg=$(curl -sf "$API/api/roms?platform_id=0&collection_kind=virtual&collection_id=$(python3 -c "import sys,urllib.parse; print(urllib.parse.quote(sys.argv[1], safe=''))" "$VID")" \
     | python3 -c "import json,sys; j=json.load(sys.stdin); print(j.get('error') or (j['total'], sorted(g['name'] for g in j['items'])))")
[[ "$vg" == "(2, ['Chrono Trigger', 'Disc Two'])" ]] || fail "a long-id collection's games, the grouped-away version too: $vg"
pg=$(curl -sf "$API/api/roms?platform_id=1&search=disc" | python3 -c "import json,sys; print(json.load(sys.stdin)['total'])")
[[ "$pg" == 0 ]] || fail "a platform's list still groups a game's versions: $pg"
curl -s -o /dev/null -w "%{http_code}" -X POST "$API/api/download" -d '{"collection_kind":"virtual","collection_id":""}' | grep -qx 400 || fail "a collection install with no id must be refused"
curl -s -o /dev/null -w "%{http_code}" -X POST "$API/api/download" -d '{"collection_kind":"regular","collection_id":"x"}' | grep -qx 400 || fail "a collection install with a bad id must be refused"
colls=$(curl -sf "$API/api/collections?kind=smart" | python3 -c "import json,sys; print([(c['name'], c['description']) for c in json.load(sys.stdin)])")
[[ "$colls" == "[('SNES picks', 'Platform: SNES')]" ]] || fail "smart collections, their filter for a description: $colls"
cg=$(curl -sf "$API/api/roms?collection_kind=smart&collection_id=3" | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['total'], [g['name'] for g in j['items']])")
[[ "$cg" == "2 ['EarthBound', 'Super Metroid']" ]] || fail "a smart collection's games: $cg"
colls=$(curl -sf "$API/api/collections?kind=regular" | python3 -c "import json,sys; print(len(json.load(sys.stdin)))")
[[ "$colls" == 2 ]] || fail "collections of one kind: $colls"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/collections_scope" -d '{"granted":false}' >/dev/null
curl -s -o /dev/null -w "%{http_code}" "$API/api/collections" | grep -qx 403 || fail "collections RomM won't show this pairing: 403, for RommPS to ask for pairing again"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/collections_scope" -d '{"granted":true}' >/dev/null
curl -s -o /dev/null -w "%{http_code}" "$API/api/collections?kind=nope" | grep -qx 400 || fail "listing an unknown kind of collection must be refused"
cg=$(curl -sf "$API/api/roms?collection_id=7" | python3 -c "import json,sys; j=json.load(sys.stdin); print(j['total'], [(g['name'], g['platform_id']) for g in j['items']])")
[[ "$cg" == "3 [('EarthBound', 1), ('Okami', 2), ('Super Metroid', 1)]" ]] || fail "a collection's games, across platforms: $cg"
cg=$(curl -sf "$API/api/roms?collection_kind=regular&collection_id=7&search=oka" | python3 -c "import json,sys; print([g['name'] for g in json.load(sys.stdin)['items']])")
[[ "$cg" == "['Okami']" ]] || fail "a search in a collection: $cg"
curl -s -o /dev/null -w "%{http_code}" "$API/api/roms?collection_kind=nope&collection_id=x" | grep -qx 400 || fail "an unknown kind of collection must be refused"
for bad in abc -1 0 7x 99999999999; do
  curl -s -o /dev/null -w "%{http_code}" "$API/api/roms?collection_id=$bad" | grep -qx 400 || fail "collection id $bad must be refused"
done

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
# The console rests with the game open: nothing in front, its processes still there. It hasn't closed.
echo "PPSA00000" > "$T/data/alive"; rm "$T/data/foreground"; sleep 7
! grep -q "game PPSA00000 closed" "$T/daemon.log" || fail "a console resting with the game open counted as the game closing"
[[ "$(cat "$RA/saves/Snes9x/Super Metroid (USA).srm")" == "server-save" ]] || fail "a download came down while the game rested"
rm "$T/data/alive"
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

echo "8. PS2 shared cards sync both ways, and only while PS2 uses shared cards"
MC="$RA/system/pcsx2/memcards"
mkdir -p "$MC"; echo "card-v1" > "$MC/Mcd001.ps2"
# LRPS2 is on one card per game (step 7): asking for shared cards syncs neither.
post /api/config '{"ps2_cards":"backup"}' >/dev/null   # 1.2's name for shared
post /api/sync >/dev/null; sleep 3
cards() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump" | python3 -c "import json,sys; c=[x for x in json.load(sys.stdin)['cards'] if x['emulator']=='rommps-ps2' and x['name']=='RetroArch Mcd001.ps2']; print(c[0]['versions'] if c else 0)"; }
[[ "$(cards)" == 0 ]] || fail "a shared card synced while LRPS2 keeps one per game"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['sync_note'] for m in json.load(sys.stdin) if 'ps2' in m['platforms']][0])")
[[ "$note" == *"don't sync"* ]] || fail "no note for an emulator on the other card method: $note"
echo 'pcsx2_shared_memory_cards = "enabled"' > "$RA/LRPS2/LRPS2.opt"
post /api/sync >/dev/null; sleep 3
[[ "$(cards)" == 1 ]] || fail "card was not backed up"
echo "card-v2" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
post /api/sync >/dev/null; sleep 3
[[ "$(cards)" == 2 ]] || fail "changed card was not backed up again"
post /api/sync >/dev/null; sleep 2
[[ "$(cards)" == 2 ]] || fail "unchanged card must not be uploaded"
echo "broken" > "$RA/system/pcsx2/memcards/Mcd001.ps2"
r=$(curl -s -X POST "$API/api/memcards/restore" -d '{"slot":1}'); [[ "$r" == *'"ok"'* ]] || fail "restore failed: $r"
[[ "$(cat "$RA/system/pcsx2/memcards/Mcd001.ps2")" == "card-v2" ]] || fail "restored card has the wrong content"
ls "$T/data/backups/memcards/" | grep -q Mcd001.ps2 || fail "no local backup before restore"
# Another console (or emulator) saves on the same card: it comes down here, this one kept.
sleep 1.1
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/card_version" -d '{"emulator":"rommps-ps2","name":"RetroArch Mcd001.ps2","file_name":"Mcd001.ps2","content":"card-from-ps4"}' >/dev/null
n0=$(ls "$T/data/backups/memcards/" | wc -l)
post /api/sync >/dev/null; sleep 3
[[ "$(cat "$MC/Mcd001.ps2")" == "card-from-ps4" ]] || fail "a newer card on RomM didn't come down: $(cat "$MC/Mcd001.ps2")"
[[ "$(ls "$T/data/backups/memcards/" | wc -l)" -gt "$n0" ]] || fail "the card replaced wasn't kept"
# Changed on both sides: the newer wins (here, this console's).
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/card_version" -d '{"emulator":"rommps-ps2","name":"RetroArch Mcd001.ps2","file_name":"Mcd001.ps2","content":"older-remote"}' >/dev/null
sleep 1.1; echo "card-newer-here" > "$MC/Mcd001.ps2"
post /api/sync >/dev/null; sleep 3
latest=$(curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump" | python3 -c "import json,sys; print([x['latest'] for x in json.load(sys.stdin)['cards'] if x['name']=='RetroArch Mcd001.ps2'][0])")
[[ "$latest" == "card-newer-here"* && "$(cat "$MC/Mcd001.ps2")" == "card-newer-here" ]] || fail "both changed: the newer must win: $latest / $(cat "$MC/Mcd001.ps2")"
# Both changed, but RomM's went up later than this one was written: when each
# was last written decides, not when it reached RomM (every card's first sync
# after updating to 1.3 is like this).
cardup() { curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/card_version" -d "{\"emulator\":\"rommps-ps2\",\"name\":\"RetroArch Mcd001.ps2\",\"file_name\":\"Mcd001.ps2\",$1}" >/dev/null; }
cardmd5() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump" | python3 -c "import json,sys; print([x['latest_md5'] for x in json.load(sys.stdin)['cards'] if x['name']=='RetroArch Mcd001.ps2'][0])"; }
# RomM's first: the save watcher syncs as soon as the card here changes.
cardup "\"content\":\"written-2-days-ago\",\"written\":$(( $(date +%s) - 172800 ))"
echo "played-an-hour-ago" > "$MC/Mcd001.ps2"; touch -d '1 hour ago' "$MC/Mcd001.ps2"
post /api/sync >/dev/null; sleep 3
[[ "$(cat "$MC/Mcd001.ps2")" == "played-an-hour-ago" && "$(cardmd5)" == "$(md5sum < "$MC/Mcd001.ps2" | cut -d' ' -f1)" ]] \
  || fail "both changed: the card written last must win, not the one uploaded last: $(cat "$MC/Mcd001.ps2")"
cardup "\"content\":\"written-10-min-ago\",\"written\":$(( $(date +%s) - 600 ))"
echo "played-an-hour-ago-2" > "$MC/Mcd001.ps2"; touch -d '1 hour ago' "$MC/Mcd001.ps2"
post /api/sync >/dev/null; sleep 3
[[ "$(cat "$MC/Mcd001.ps2")" == "written-10-min-ago" ]] || fail "both changed: RomM's, written last, must win: $(cat "$MC/Mcd001.ps2")"
# A blank card never beats one with saves: not a fresh one made here (it
# comes back from RomM instead of going up), not a blank one from elsewhere.
python3 "$ROOT/tests/memcards.py" ps2 "$MC/Mcd001.ps2" 3 mine
post /api/sync >/dev/null; sleep 3
saved=$(md5sum < "$MC/Mcd001.ps2" | cut -d' ' -f1)
[[ "$(cardmd5)" == "$saved" ]] || fail "a card with saves must go up"
sleep 1.1; python3 "$ROOT/tests/memcards.py" ps2 "$MC/Mcd001.ps2" 0 fresh
post /api/sync >/dev/null; sleep 3
[[ "$(md5sum < "$MC/Mcd001.ps2" | cut -d' ' -f1)" == "$saved" && "$(cardmd5)" == "$saved" ]] \
  || fail "a blank card made here must not replace RomM's with saves; RomM's must come back"
# Nor one never formatted: PCSX2 makes a missing card as 8 MB of 0xFF.
sleep 1.1; python3 -c "import sys; open(sys.argv[1], 'wb').write(b'\xff' * 8650752)" "$MC/Mcd001.ps2"
post /api/sync >/dev/null; sleep 3
[[ "$(md5sum < "$MC/Mcd001.ps2" | cut -d' ' -f1)" == "$saved" && "$(cardmd5)" == "$saved" ]] \
  || fail "a never formatted card made here must not replace RomM's with saves; RomM's must come back"
python3 "$ROOT/tests/memcards.py" ps2 "$T/blank.ps2" 0 other-console
cardup "\"path\":\"$T/blank.ps2\""
post /api/sync >/dev/null; sleep 3
[[ "$(md5sum < "$MC/Mcd001.ps2" | cut -d' ' -f1)" == "$saved" && "$(cardmd5)" == "$saved" ]] \
  || fail "a blank card from another console must not replace this one with saves; this one must go back up"
rm -f "$T/blank.ps2"

dump() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump"; }
syncnow() { local n; n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])"); post /api/sync >/dev/null; wait_sync $((n+1)); }

echo "9. RomM keeps only the newest versions per save"
post /api/config '{"server_versions":2}' >/dev/null
for v in v4 v5 v6; do sleep 1.1; echo "local-save-$v" > "$RA/saves/Snes9x/Chrono Trigger (USA).srm"; syncnow; done
n=$(dump | python3 -c "import json,sys; print(sum(1 for x in json.load(sys.stdin)['saves'] if x['rom_id']==10 and x['slot']=='autosave'))")
[[ "$n" == 2 ]] || fail "expected 2 versions on the server, found $n"

echo "10. states download, save history and restore"
post /api/config '{"states":"sync"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"emulator":"snes9x","file_name":"Super Metroid (USA).state1","content":"srv-state"}' >/dev/null
# Made by another core (or with no emulator at all): no emulator here can load it.
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"emulator":"bsnes","file_name":"Super Metroid (USA) [bsnes].state5","content":"other-core"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":11,"file_name":"Super Metroid (USA) [unknown].state6","content":"no-emulator"}' >/dev/null
syncnow
! grep -rqs "other-core\|no-emulator" "$RA/states" || fail "a state from another core came down"
grep -q "skipped: made by bsnes" "$T/daemon.log" || fail "a skipped state must say why"
[[ "$(cat "$RA/states/Super Metroid (USA).state1" 2>/dev/null)" == "srv-state" ]] || fail "server state not downloaded"
# Two web player states for one game: each gets its own file, and they don't
# come down again on the next sync.
for ts in "2026-10-14 01-00-00-000" "2026-10-14 01-00-05-000"; do
  curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d "{\"rom_id\":11,\"emulator\":\"snes9x\",\"file_name\":\"Super Metroid (USA) [$ts].state\",\"content\":\"web-$ts\"}" >/dev/null
done
syncnow
[[ "$(cat "$RA/states/Super Metroid (USA).state")" == "web-2026-10-14 01-00-00-000" ]] || fail "first web state should take slot 0"
[[ "$(cat "$RA/states/Super Metroid (USA).state2")" == "web-2026-10-14 01-00-05-000" ]] || fail "second web state should take the next free slot"
[[ "$(cat "$RA/states/Super Metroid (USA).state1")" == "srv-state" ]] || fail "a downloaded state replaced another one"
syncnow
[[ "$(last states_downloaded)" == 0 ]] || fail "states came down again: $(last states_downloaded)"
# Saving over a state that came from elsewhere sends a new one; RomM's copy
# of the web state stays as it was.
echo "played-on-console" > "$RA/states/Super Metroid (USA).state"
syncnow
web=$(dump | python3 -c "import json,sys; print([s['file_name'] for s in json.load(sys.stdin)['states'] if s['rom_id']==11 and s['file_name'].endswith('.state')])")
[[ "$web" == *"Super Metroid (USA) [2026-10-14 01-00-00-000].state"* && "$web" == *"Super Metroid (USA) [PlayStation 5].state"* ]] \
  || fail "a state saved over a web state must go up as a new one: $web"
orig=$(curl -sf "$API/api/history?rom_id=11" >/dev/null; dump | python3 -c "import json,sys; print([s['file_size_bytes'] for s in json.load(sys.stdin)['states'] if s['file_name']=='Super Metroid (USA) [2026-10-14 01-00-00-000].state'][0])")
[[ "$orig" == 27 ]] || fail "the web state on RomM was changed (size $orig)"
syncnow
[[ "$(last states_downloaded)$(last states_uploaded)" == 00 ]] || fail "an uploaded state came back down or went up again"
# The player's own slot is never taken by a state from elsewhere.
echo "my-own-slot" > "$RA/states/Chrono Trigger (USA).state"
syncnow
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d '{"rom_id":10,"emulator":"snes9x","file_name":"Chrono Trigger (USA) [2026-10-14 02-00-00-000].state","content":"web-ct"}' >/dev/null
syncnow
[[ "$(cat "$RA/states/Chrono Trigger (USA).state")" == "my-own-slot" ]] || fail "a web state replaced the player's own slot"
[[ "$(cat "$RA/states/Chrono Trigger (USA).state1")" == "web-ct" ]] || fail "the web state should land in the next free slot"
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
# The per-game folder's file was never synced and RomM already has EarthBound's
# save: that's a conflict, not a quiet replacement. Keeping this one sends it up.
[[ "$(last conflicts)" == 1 ]] || fail "a never-synced save against RomM's must be a conflict (got $(last conflicts))"
post /api/conflicts/resolve "{\"path\":\"$RA/special/EarthBound (USA).srm\",\"keep\":\"local\"}" >/dev/null || fail "resolve failed"
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
# What a game installs to the memory stick through the save system (Peace Walker's 848 MB): no save.
mkdir -p "$SD/ULUS10336INSTALL"
python3 "$ROOT/tests/make_disc.py" sfo "$SD/ULUS10336INSTALL/PARAM.SFO" TITLE="Crisis Core" SAVEDATA_PARAMS=x
truncate -s 17M "$SD/ULUS10336INSTALL/SLOT.DAT"
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
[[ -f "$SD/ULUS10336INSTALL/SLOT.DAT" ]] || fail "what the game installed must be left alone"
ls "$T/data/backups/30/" | grep -q "ULUS10336.zip" || fail "no backup of the PSP save"

echo "12b. GameCube saves stay in the form RomM has them: one .gci, or a whole memory card"
GC="python3 $ROOT/tests/gccard.py"
admin_save() { # rom_id file_name path
    python3 -c "import base64,json,sys; print(json.dumps({'rom_id':int(sys.argv[1]),'slot':'autosave','file_name':sys.argv[2],'content_b64':base64.b64encode(open(sys.argv[3],'rb').read()).decode()}))" "$1" "$2" "$3" > "$T/admin.json"
    curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d @"$T/admin.json" >/dev/null
}
newest() { # rom_id: writes RomM's newest save for it to $T/newest, prints its name
    dump | python3 -c "
import json,sys,re
s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==$1], key=lambda x: (x['updated_at'], x['id']))
open('$T/newest','wb').write(s[-1]['content'].encode('latin1')); print(re.sub(r' \[[^]]*\]', '', s[-1]['file_name']))"; }
blocks_md5() { python3 -c "import hashlib,sys; print(hashlib.md5(open(sys.argv[1],'rb').read()[0x40:]).hexdigest())" "$1"; }
for g in "41 Sunshine GMSE01" "42 Melee GALE01"; do
    set -- $g
    curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d "{\"id\":$1,\"platform_id\":4,\"name\":\"$2\",\"fs_name\":\"$2 (USA).iso\",\"files\":[]}" >/dev/null
    python3 "$ROOT/tests/make_disc.py" gc "$RA/roms/ngc/$2 (USA).iso" "$3"
done
# One .gci on RomM (another app's): it comes down into the GCI folder, and goes back up as one .gci.
$GC gci "$T/sms.gci" GMSE01 super_mario_sunshine 2 0x11
admin_save 41 "01-GMSE-super_mario_sunshine.gci" "$T/sms.gci"
syncnow
cmp -s "$T/sms.gci" "$CARD/01-GMSE-super_mario_sunshine.gci" || fail "RomM's .gci must land in the GCI folder"
syncnow
[[ "$(last uploaded)" == 0 ]] || fail "an unchanged .gci must not go up again"
sleep 1.1; $GC gci "$CARD/01-GMSE-super_mario_sunshine.gci" GMSE01 super_mario_sunshine 3 0x22
syncnow
[[ "$(newest 41)" == "01-GMSE-super_mario_sunshine.gci" ]] || fail "a .gci on RomM must go back up as a .gci: $(newest 41)"
cmp -s "$T/newest" "$CARD/01-GMSE-super_mario_sunshine.gci" || fail "the .gci RomM got isn't this console's"
# A whole card on RomM with Melee's save and another game's: Melee's comes
# down; this console's change goes back onto RomM's card, the other save untouched.
$GC blank "$T/cardA.raw" 64
$GC gci "$T/melee.gci" GALE01 SuperSmashBros0110290334 3 0x33; $GC put "$T/cardA.raw" "$T/melee.gci"
$GC gci "$T/other.gci" GXXE01 other_game 2 0x44; $GC put "$T/cardA.raw" "$T/other.gci"
admin_save 42 "MemoryCardA.USA.raw" "$T/cardA.raw"
syncnow
cmp -s "$T/melee.gci" "$CARD/01-GALE-SuperSmashBros0110290334.gci" 2>/dev/null ||
    [[ "$(blocks_md5 "$T/melee.gci")" == "$(blocks_md5 "$CARD/01-GALE-SuperSmashBros0110290334.gci" 2>/dev/null)" ]] || fail "Melee's save must come off RomM's card"
[[ ! -e "$CARD/01-GXXE-other_game.gci" ]] || fail "only the game's own saves come off the card"
syncnow
[[ "$(last uploaded)" == 0 ]] || fail "an unchanged save must leave RomM's card alone"
other_md5=$($GC saves "$T/cardA.raw" GXXE01)
sleep 1.1; $GC gci "$CARD/01-GALE-SuperSmashBros0110290334.gci" GALE01 SuperSmashBros0110290334 4 0x55
syncnow
[[ "$(newest 42)" == "MemoryCardA.USA.raw" ]] || fail "a card on RomM must go back up as the card: $(newest 42)"
dump | python3 -c "import json,sys; n=[x['file_name'] for x in json.load(sys.stdin)['saves'] if x['rom_id'] in (41,42)]; sys.exit(any(f.count('[') > 1 for f in n))" || fail "a name sent back must not pile up RomM's stamps"
$GC check "$T/newest" || fail "the card sent to RomM must be one Dolphin reads"
[[ "$($GC saves "$T/newest" GALE01 | cut -d' ' -f2)" == "$(blocks_md5 "$CARD/01-GALE-SuperSmashBros0110290334.gci")" ]] || fail "this console's Melee save must be on the card"
[[ "$($GC saves "$T/newest" GXXE01)" == "$other_md5" ]] || fail "the card's other saves must stay as they were"
# Another device changes the card: it comes down, and this console's file follows.
cp "$T/newest" "$T/cardB.raw"; $GC gci "$T/melee2.gci" GALE01 SuperSmashBros0110290334 2 0x66; $GC put "$T/cardB.raw" "$T/melee2.gci"
sleep 1.1; admin_save 42 "MemoryCardA.USA.raw" "$T/cardB.raw"
syncnow
[[ "$(blocks_md5 "$CARD/01-GALE-SuperSmashBros0110290334.gci")" == "$(blocks_md5 "$T/melee2.gci")" ]] || fail "a newer card on RomM must update the save here"
ls "$T/data/backups/42/" 2>/dev/null | grep -q . || fail "no backup before the card's save replaced this console's"
grep -q "Sunshine (USA).iso: RomM keeps its saves as gci (01-GMSE-super_mario_sunshine.gci)" "$T/daemon.log" &&
    grep -q "Melee (USA).iso: RomM keeps its saves as card (MemoryCardA.USA.raw)" "$T/daemon.log" ||
    fail "each game's form must be the one RomM has"

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
[[ ! -e "$T/etaHEN/payloads/romm-sync.elf.bak" ]] || fail "the update left a .bak of the old payload behind"

echo "14. diagnostic bundle: the logs, nothing that names the person"
echo "connecting to 10.0.16.84 from aa:bb:cc:dd:ee:ff, mail me@example.com, /user/home/1a2b3c4d/x, onlineId=SomePlayer99" >> "$T/data/romm-sync.log"
# A server paired before this one, in every form a log line names it.
cat >> "$T/data/romm-sync.log" <<'EOF2'
[INFO] paired with https://romm.old-server.example.org, device x
[WARN] HTTP https://romm.old-server.example.org/api/platforms: Failed to connect to romm.old-server.example.org port 443 after 15001 ms: Timeout was reached
[WARN] HTTP http://oldnas:8080/api/roms: Could not resolve host: oldnas
[WARN] HTTP http://oldnas:8080/api/roms: Could not connect to server
[INFO] update check: https://api.github.com/repos/s0liton/RommPS/releases/latest
EOF2
curl -sf "$API/api/diagnostics" > "$T/diag.zip" || fail "no diagnostic bundle"
python3 - "$T/diag.zip" "$T/data/config.json" <<'PY' || fail "diagnostic bundle"
import json, sys, zipfile
z = zipfile.ZipFile(sys.argv[1]); cfg = json.load(open(sys.argv[2]))
names = z.namelist()
for want in ("report.txt", "romm-sync.log", "sync-log.json", "emulators-found.json", "emulator-settings.json", "kernel.log"):
    assert want in names, (want, names)
assert "RommPS diagnostics" in z.read("report.txt").decode()
text = "\n".join(z.read(n).decode("utf-8", "replace") for n in names)
host = cfg["server_url"].split("://")[1].split("/")[0].split(":")[0]
for secret in (cfg["token"], cfg["server_url"], host, cfg["device_id"], "10.0.16.84", "aa:bb:cc:dd:ee:ff", "me@example.com", "1a2b3c4d", "SomePlayer99", "old-server", "oldnas"):
    assert secret and secret not in text, secret
assert "<ip>" in text and "<mac>" in text and "<email>" in text and "<server>" in text
for kept in ("connect to <host> port 443", "resolve host: <host>", "http://<host>:8080/api/roms", "Could not connect to server", "https://api.github.com/repos"):
    assert kept in text, kept
PY
out=$(curl -sf -X POST "$API/api/diagnostics") || fail "saving a bundle refused"
[[ -f "$(echo "$out" | python3 -c "import json,sys; print(json.load(sys.stdin)['path'])")" ]] || fail "the saved bundle isn't there: $out"

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
              "save_name_by_core": {"mednafen_psx_hw": "{rom_stem}_1{ext}"},
              "state_format": "raw", "state_slot0": ".state0", "state_cores": "pcsx_rearmed,mednafen_psx_hw",
              "state_core_setting": {"file": "{root}/psxs5.ini", "key": "emulator",
                                     "values": {"0": "mednafen_psx_hw", "2": "mednafen_psx_hw"},
                                     "auto": "0", "log": "{root}/logs/psxs5.log", "log_line": "host: emulator ",
                                     "log_names": {"rearmed": "pcsx_rearmed", "beetle": "mednafen_psx_hw"}},
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
              "platforms": [{"romm": ["ps2"], "dir": "ps2", "cards": {"default": "shared", "modes": "shared"}}]}},
 {"id": "nope", "name": "Not yet", "detect": ["$T/nope"], "ready": false, "note": "not supported yet",
  "profile": {"root": "$T/nope", "rom_dir": "{root}/games", "platforms": [{"romm": ["n64"], "dir": "n64"}]}}]
EOF
python3 "$ROOT/tests/make_disc.py" psxbin "$T/psxemu/games/Test Quest (USA).bin" SLUS_123.45
printf 'card-from-psx' > "$T/psxemu/saves/SLUS-12345_1.mcd"
printf 'STv4 PCSXra test-build' > "$T/psxemu/states/Test_Quest__USA_.state0"   # a bare PCSX-ReARMed state
printf 'emulator=0\n' > "$T/psxemu/psxs5.ini"   # Auto: the log says which core it started
mkdir -p "$T/psxemu/logs"; printf 'host: emulator Beetle PSX\nhost: emulator PCSX-ReARMed\n' > "$T/psxemu/logs/psxs5.log"
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
for _ in $(seq 1 20); do grep -q "NOTIFY.*now also syncs\{0,1\} with" "$T/daemon.log" && break; sleep 0.25; done
grep -q "NOTIFY\] RommPS: [0-9]* N64 games\{0,1\} now also syncs\{0,1\} with" "$T/daemon.log" || fail "no notice that the games now also sync with the extra"
dirs=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print(sorted(m['rom_dir'] for m in json.load(sys.stdin)))")
[[ "$dirs" != *"$T/nope"* ]] || fail "an emulator not supported yet got a profile: $dirs"
post /api/config '{"states":"upload"}' >/dev/null
syncnow
dump() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump"; }
saves=$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(sorted((s['rom_id'], s['file_name'], s['slot']) for s in (d['saves'].values() if isinstance(d['saves'], dict) else d['saves']) if s['rom_id'] in (50, 60)))")
# (RomM puts the upload's time in the name: "SLUS-12345_1 [2026-...].mcd".)
[[ "$saves" == *"(50, 'SLUS-12345_1 ["*"].mcd', 'autosave')"* ]] || fail "the card named by serial didn't sync: $saves"
[[ "$saves" == *"(60, '$fnv ["*"].sav', 'autosave')"* ]] || fail "the save named by hash didn't sync: $saves"
[[ $(grep -o "(60," <<<"$saves" | wc -l) -eq 1 ]] || fail "two emulators sharing a save must upload one: $saves"
[[ "$(cat "$T/n64b/saves/Test Racer (U).srm" 2>/dev/null)" == "n64-save" ]] || fail "the shared save didn't reach the second emulator"
states=$(dump | python3 -c "import json,sys; d=json.load(sys.stdin); print(sorted(s['file_name'] for s in (d['states'].values() if isinstance(d['states'], dict) else d['states']) if s['rom_id'] == 50))")
[[ "$states" == "['Test_Quest__USA_"*".state0']" ]] || fail "the state named by the safe name didn't sync: $states"
# A bare state goes up in RetroArch's container, as the core that made it, so
# RomM's web player can load it.
up=$(dump | python3 -c "import json,sys; print([(s['emulator'], s['file_size_bytes']) for s in json.load(sys.stdin)['states'] if s['rom_id'] == 50])")
[[ "$up" == "[('pcsx_rearmed', 48)]" ]] || fail "a PSXS5 state must go up wrapped, as pcsx_rearmed: $up"
# A standalone emulator's card is its own on RomM, apart from RetroArch's.
c2=$(dump | python3 -c "import json,sys; print([c['latest'] for c in json.load(sys.stdin)['cards'] if c['emulator'] == 'rommps-ps2' and c['name'] == 'Mcd001.ps2'])")
[[ "$c2" == "['card-ps2']" ]] || fail "the shared PS2 card wasn't synced: $c2"
# Web player states come down bare, into a slot of their own, and only for the
# core the emulator runs now.
post /api/config '{"states":"sync"}' >/dev/null
rast() { python3 -c "import json,sys,struct; m=sys.argv[1].encode(); print(json.dumps((b'RASTATE\x01MEM '+struct.pack('<I',len(m))+m+b'\0'*(-len(m)%8)+b'END \0\0\0\0').decode('latin1')))" "$1"; }
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d "{\"rom_id\":50,\"emulator\":\"pcsx_rearmed\",\"file_name\":\"Test Quest (USA) [web].state\",\"content\":$(rast 'STv4 PCSXra web-build')}" >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d "{\"rom_id\":50,\"emulator\":\"mednafen_psx_hw\",\"file_name\":\"Test Quest (USA) [beetle].state\",\"content\":$(rast 'MDFNSVST beetle')}" >/dev/null
syncnow
[[ "$(cat "$T/psxemu/states/Test_Quest__USA_.state1")" == "STv4 PCSXra web-build" ]] || fail "a web state must come down bare, in the next free slot"
[[ ! -e "$T/psxemu/states/Test_Quest__USA_.state2" ]] || fail "a Beetle state came down while the emulator runs PCSX-ReARMed"
printf 'emulator=2\n' > "$T/psxemu/psxs5.ini"
syncnow
[[ "$(cat "$T/psxemu/states/Test_Quest__USA_.state2")" == "MDFNSVST beetle" ]] || fail "switched to Beetle, its state must come down"
# With Beetle the card is named after the game file: still the game's card.
sleep 1; printf 'card-beetle' > "$T/psxemu/saves/Test Quest (USA)_1.mcd"
syncnow
# A card this console never synced, against the one RomM has: asked, not assumed.
[[ "$(last conflicts)" == 1 ]] || fail "Beetle's never-synced card against RomM's must be a conflict (got $(last conflicts))"
post /api/conflicts/resolve "{\"path\":\"$T/psxemu/saves/Test Quest (USA)_1.mcd\",\"keep\":\"local\"}" >/dev/null || fail "resolve failed"
card=$(dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==50], key=lambda x: x['updated_at']); print(s[-1]['content'])")
[[ "$card" == "card-beetle" ]] || fail "the card Beetle names after the game file must sync: $card"
# A blank card an emulator just made under the other name never beats one with a save on it.
mkcard() { python3 -c "import sys; d=bytearray(131072); d[0:2]=b'MC'; d[128:128*16]=bytes([0xA0])*(128*15)
if sys.argv[2]=='save': d[128]=0x51; d[0x2000:0x2004]=b'GAME'
open(sys.argv[1],'wb').write(bytes(d))" "$1" "$2"; }
mkcard "$T/psxemu/saves/SLUS-12345_1.mcd" save; sleep 1; mkcard "$T/psxemu/saves/Test Quest (USA)_1.mcd" blank
syncnow
kind=$(dump | python3 -c "import json,sys; s=sorted([x for x in json.load(sys.stdin)['saves'] if x['rom_id']==50], key=lambda x: x['updated_at']); print('save' if 'GAME' in s[-1]['content'] else 'blank')")
[[ "$kind" == "save" ]] || fail "a blank card was uploaded over a card with a save"
# Nor does a blank card from RomM replace one with saves: that's a conflict to settle.
python3 -c "import base64,json,sys; d=bytearray(131072); d[0:2]=b'MC'; d[128:128*16]=bytes([0xA0])*(128*15)
json.dump({'rom_id': 50, 'slot': 'autosave', 'file_name': 'SLUS-12345_1.mcd', 'content_b64': base64.b64encode(bytes(d)).decode()}, open(sys.argv[1], 'w'))" "$T/blank.json"
sleep 1; curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d @"$T/blank.json" >/dev/null
syncnow
python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.exit(0 if d[128]==0x51 else 1)" "$T/psxemu/saves/SLUS-12345_1.mcd" \
  || fail "a blank card from RomM replaced a card with saves"
[[ "$(last conflicts)" -ge 1 ]] || fail "a blank card from RomM over one with saves must be a conflict"

post /api/config '{"states":"upload"}' >/dev/null
# A save from another device reaches both N64 emulators.
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":60,"slot":"autosave","file_name":"Test Racer (U).srm","content":"n64-from-elsewhere","emulator":"other"}' >/dev/null
syncnow
[[ "$(cat "$T/n64emu/saves/$fnv.sav")" == "n64-from-elsewhere" && "$(cat "$T/n64b/saves/Test Racer (U).srm")" == "n64-from-elsewhere" ]] \
  || fail "a downloaded save must reach every emulator that shares it"
# A download goes to the main emulator and is linked into the extra.
post /api/download '{"rom_id":61}' >/dev/null; sleep 2
[[ -f "$T/n64emu/games/Other Racer (U).z64" && -f "$T/n64b/roms/Other Racer (U).z64" ]] || fail "the download didn't reach the extra emulator"
python3 -c 'import os,sys; sys.exit(not os.path.samefile(sys.argv[1], sys.argv[2]))' \
  "$T/n64emu/games/Other Racer (U).z64" "$T/n64b/roms/Other Racer (U).z64" || fail "the extra copy should be a hard link"

echo "16b. two Dolphins for GameCube keep one save: the main one's .gci files reach the extra's folder"
mkdir -p "$T/gcA/games" "$T/gcB/ngc"
python3 - "$T/catalog.json" "$T" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
# Dolphin A keeps GameCube and Wii games in one folder, like Porpoise;
# Dolphin B a folder for each.
# Their folders for GameCube are named differently ("gamecube", "ngc"), as
# Porpoise's and RetroArch's are.
for gid, name, roms, gc_dir in (("gcA", "Dolphin A", "{root}/games", "gamecube"), ("gcB", "Dolphin B", "{root}/{platform}", "ngc")):
    cat.append({"id": gid, "name": name, "detect": [f"{sys.argv[2]}/{gid}"],
                "profile": {"root": f"{sys.argv[2]}/{gid}", "rom_dir": roms, "save_dir": "{root}/User",
                            "platforms": [{"romm": ["ngc"], "dir": gc_dir, "emulator": "dolphin",
                                           "save_layout": "gci", "bundle_dirs": "{root}/User/GC",
                                           **({"state_dir": "{root}/states/{game_id}", "state_exts": ".state",
                                               "state_format": "raw"} if gid == "gcA" else {})},
                                          {"romm": ["wii"], "dir": "wii", "emulator": "dolphin",
                                           "save_layout": "wii", "bundle_dirs": "{root}/User/Wii"}]}})
json.dump(cat, open(sys.argv[1], "w"))
PY
python3 "$ROOT/tests/make_disc.py" gc "$T/gcA/games/Sunshine (USA).iso" GMSE01
python3 "$ROOT/tests/make_disc.py" gc "$T/gcB/ngc/Sunshine (USA).iso" GMSE01
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\",\"$T/gcA\",\"$T/gcB\"],
  \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\",\"ngc\":\"$T/gcA\",\"wii\":\"$T/gcA\"},
  \"also\":{\"n64\":[\"$T/n64b\"],\"ngc\":[\"$T/gcB\"],\"wii\":[\"$T/gcB\"]}}" >/dev/null \
  || fail "setup with two Dolphins refused"
A="$T/gcA/User/GC/USA/Card A"; Bc="$T/gcB/User/GC/USA/Card A"
syncnow
sleep 1; syncnow
[[ ! -e "$T/gcB/wii/Sunshine (USA).iso" ]] || fail "a GameCube game in a folder shared with Wii must not go to the extra's Wii folder"
cmp -s "$A/01-GMSE-super_mario_sunshine.gci" "$Bc/01-GMSE-super_mario_sunshine.gci" || fail "RomM's save must reach both Dolphins"
sleep 1.1; $GC gci "$Bc/01-GMSE-super_mario_sunshine.gci" GMSE01 super_mario_sunshine 2 0x77
syncnow
cmp -s "$Bc/01-GMSE-super_mario_sunshine.gci" "$A/01-GMSE-super_mario_sunshine.gci" || fail "a save made in the extra must reach the main Dolphin"
[[ "$(last uploaded)" == 1 ]] || fail "two Dolphins sharing a save must upload it once: $(last uploaded)"
[[ "$(newest 41)" == "01-GMSE-super_mario_sunshine.gci" ]] && cmp -s "$T/newest" "$A/01-GMSE-super_mario_sunshine.gci" || fail "RomM must get the newer save, as one .gci"
ls "$T/data/backups/41/" 2>/dev/null | grep -q "GMSE01-shared.zip" || fail "the save replaced in the main Dolphin must be backed up"
# Porpoise's states: a folder per game id holding slot1.state, slot2.state...
post /api/config '{"states":"sync"}' >/dev/null
mkdir -p "$T/gcA/states/GMSE01"; printf 'dolphin-bare-state' > "$T/gcA/states/GMSE01/slot1.state"
syncnow
up=$(dump | python3 -c "import json,sys; print([(s['file_name'].split(' [')[0], s['emulator'], s['file_size_bytes']) for s in json.load(sys.stdin)['states'] if s['rom_id']==41])")
[[ "$up" == "[('slot1', 'dolphin', 48)]" ]] || fail "a state in a folder per game must go up, wrapped: $up"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/state" -d "{\"rom_id\":41,\"emulator\":\"dolphin\",\"file_name\":\"Sunshine (USA).state3\",\"content\":$(rast 'dolphin-from-retroarch')}" >/dev/null
syncnow
[[ "$(cat "$T/gcA/states/GMSE01/slot2.state" 2>/dev/null)" == "dolphin-from-retroarch" ]] || fail "a state from RomM must land unwrapped in the game's next free slot"
[[ "$(cat "$T/gcA/states/GMSE01/slot1.state")" == "dolphin-bare-state" ]] || fail "the state already there must be left alone"
rm "$T/gcA/states/GMSE01/slot1.state"
syncnow
[[ "$(cat "$T/gcA/states/GMSE01/slot1.state" 2>/dev/null)" == "dolphin-bare-state" ]] || fail "a deleted state must come back bare, in its own emulator's form"

echo "16c. an extra that reads its own app folder plays the main emulator's games through a mount, not copies"
XA="$MB/TEST00002"; mkdir -p "$XA/games" "$XA/data/saves"
python3 - "$T/catalog.json" "$XA" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
cat.append({"id": "psxmount", "name": "PSX-mount", "detect": [sys.argv[2]],
            "profile": {"root": sys.argv[2], "rom_dir": "{root}/games", "save_dir": "{root}/data/saves",
                        "state_dir": "{root}/data/states", "mount_games": True,
                        "save_exts": ".mcd", "save_name": "{rom_stem}_1{ext}", "save_stems": "{rom_stem}_1",
                        "platforms": [{"romm": ["psx", "ps1"], "dir": "psx", "save_format": "psx-card"}]}})
json.dump(cat, open(sys.argv[1], "w"))
PY
# A copy from before mounts, as 1.2.1 made them.
cp "$T/psxemu/games/Test Quest (USA).bin" "$XA/games/"
setup16c() {
  post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\",\"$T/gcA\",\"$T/gcB\",\"$XA\"],
    \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\",\"ngc\":\"$T/gcA\",\"wii\":\"$T/gcA\"},
    \"also\":{\"n64\":[\"$T/n64b\"],\"ngc\":[\"$T/gcB\"],\"wii\":[\"$T/gcB\"],\"psx\":[\"$XA\"]}}" >/dev/null || fail "setup with a mounted extra refused"
}
printf 'Put your games here' > "$XA/games/README.txt"   # what an emulator ships there (Crux2) is no game of its own
setup16c
for _ in $(seq 1 20); do grep -q "^TEST00002|" "$T/data/mounts.txt" 2>/dev/null && break; sleep 0.25; done
[[ "$(cat "$T/data/mounts.txt" 2>/dev/null)" == "TEST00002|$T/psxemu/games|app0/games" ]] || fail "the extra must get the main emulator's games mounted on app0/games: $(cat "$T/data/mounts.txt" 2>/dev/null)"
for _ in $(seq 1 20); do [[ ! -e "$XA/games/Test Quest (USA).bin" ]] && break; sleep 0.25; done
[[ ! -e "$XA/games/Test Quest (USA).bin" ]] || fail "the copy from before mounts must go"
for _ in $(seq 1 20); do grep -q "NOTIFY\] RommPS: PSX-mount now plays the same game files" "$T/daemon.log" && break; sleep 0.25; done
grep -q "NOTIFY\] RommPS: PSX-mount now plays the same game files" "$T/daemon.log" || fail "no notice that the copies went"
[[ -f "$T/psxemu/games/Test Quest (USA).bin" ]] || fail "the main emulator's game must stay"
m=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([(m['rom_dir'], m.get('own_rom_dir'), m.get('mount')) for m in json.load(sys.stdin) if m['profile'] == 'PSX-mount'])")
[[ "$m" == "[('$T/psxemu/games', '$XA/games', 'app0/games')]" ]] || fail "the extra must play the main emulator's folder: $m"
syncnow
[[ -s "$XA/data/saves/Test Quest (USA)_1.mcd" ]] || fail "the shared card must reach the mounted extra"
# A blank card the extra just made never beats the main one's with a save on it.
shared=$(for f in "$T/psxemu/saves/"*.mcd; do cmp -s "$f" "$XA/data/saves/Test Quest (USA)_1.mcd" && echo "$f"; done | head -1)
[[ -n "$shared" ]] || fail "the extra's card must be a copy of one of the main emulator's"
mkcard "$shared" save; touch -d '1 minute ago' "$shared"
mkcard "$XA/data/saves/Test Quest (USA)_1.mcd" blank
syncnow
hassave() { python3 -c "import sys; d=open(sys.argv[1],'rb').read(); sys.exit(0 if d[128]==0x51 else 1)" "$1"; }
hassave "$shared" || fail "a blank card an extra just made replaced the main emulator's card with a save"
hassave "$XA/data/saves/Test Quest (USA)_1.mcd" || fail "the extra's blank card must get the main emulator's save"
[[ ! -e "$XA/games/Test Quest (USA).bin" ]] || fail "a sync must not copy the game back"
# Games of its own in its folder would be hidden by the mount: it keeps copies then.
printf 'mine' > "$XA/games/Mine (USA).bin"
setup16c
for _ in $(seq 1 20); do [[ ! -s "$T/data/mounts.txt" ]] && break; sleep 0.25; done
[[ ! -s "$T/data/mounts.txt" ]] || fail "an extra with games of its own must not get a mount"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['mount_note'] for m in json.load(sys.stdin) if m['profile'] == 'PSX-mount'])")
[[ "$note" == *"1 game of its own"* ]] || fail "why it has no mount must be said: $note"
for _ in $(seq 1 20); do [[ -f "$XA/games/Test Quest (USA).bin" ]] && break; sleep 0.25; done
[[ -f "$XA/games/Test Quest (USA).bin" ]] || fail "without a mount the extra must get copies again"
rm "$XA/games/Mine (USA).bin"
# A game RomM Sync downloaded for the extra that the main one doesn't have (the
# extra was the main one before) moves over, and then the mount goes on.
for _ in $(seq 1 20); do grep -q "Test Quest" "$T/data/state.json" 2>/dev/null && break; sleep 0.25; done
mv "$T/psxemu/games/Test Quest (USA).bin" "$T/quest.bak"
setup16c
for _ in $(seq 1 20); do [[ -f "$T/psxemu/games/Test Quest (USA).bin" ]] && break; sleep 0.25; done
[[ -f "$T/psxemu/games/Test Quest (USA).bin" && ! -e "$XA/games/Test Quest (USA).bin" ]] || fail "a game RomM Sync downloaded must move from the extra to the main emulator"
for _ in $(seq 1 20); do grep -q "NOTIFY\] RommPS: 1 PSX-mount game moved to PSX-like" "$T/daemon.log" && break; sleep 0.25; done
grep -q "NOTIFY\] RommPS: 1 PSX-mount game moved to PSX-like" "$T/daemon.log" || fail "no notice of the game moved"
for _ in $(seq 1 20); do grep -q "^TEST00002|" "$T/data/mounts.txt" 2>/dev/null && break; sleep 0.25; done
grep -q "^TEST00002|" "$T/data/mounts.txt" || fail "with its games moved, the extra must get its mount"
rm "$T/quest.bak"
# Another extra playing the games in that folder (SwanStationPS5 in PSXS5's)
# would lose them under the mount: both keep copies, and they stay copies.
mkdir -p "$T/swanlike"
python3 - "$T/catalog.json" "$T/swanlike" "$XA/games" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
cat.append({"id": "swanlike", "name": "Swan-like", "detect": [sys.argv[2]],
            "profile": {"root": sys.argv[2], "rom_dir": sys.argv[3], "save_dir": "{root}/saves",
                        "save_exts": ".mcd", "save_name": "{rom_stem}_1{ext}",
                        "platforms": [{"romm": ["psx", "ps1"], "dir": "psx", "save_format": "psx-card"}]}})
json.dump(cat, open(sys.argv[1], "w"))
PY
setup_shared() {
  post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\",\"$T/gcA\",\"$T/gcB\",\"$XA\",\"$T/swanlike\"],
    \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\",\"ngc\":\"$T/gcA\",\"wii\":\"$T/gcA\"},
    \"also\":{\"n64\":[\"$T/n64b\"],\"ngc\":[\"$T/gcB\"],\"wii\":[\"$T/gcB\"],\"psx\":[\"$XA\",\"$T/swanlike\"]}}" >/dev/null || fail "setup with two extras refused"
}
setup_shared
for _ in $(seq 1 20); do [[ -f "$XA/games/Test Quest (USA).bin" ]] && break; sleep 0.25; done
[[ -f "$XA/games/Test Quest (USA).bin" ]] || fail "an extra playing from another extra's folder must get copies there"
setup_shared; sleep 1; setup_shared; sleep 1
[[ -f "$XA/games/Test Quest (USA).bin" ]] || fail "copies another emulator plays from must never be cleared"
[[ ! -s "$T/data/mounts.txt" ]] || fail "a folder another emulator plays from must not get a mount"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['mount_note'] for m in json.load(sys.stdin) if m['profile'] == 'PSX-mount'])")
[[ "$note" == *"Swan-like plays the games in its folder too"* ]] || fail "why it has no mount must be said: $note"
setup16c   # back to one extra: the mount goes on, the copy goes
for _ in $(seq 1 20); do [[ ! -e "$XA/games/Test Quest (USA).bin" ]] && break; sleep 0.25; done
grep -q "^TEST00002|" "$T/data/mounts.txt" || fail "with the other extra gone, the mount must come back"
# A game of the extra's own blocks its mount until the user moves it (RommPS asks).
printf 'mine' > "$XA/games/Mine (USA).bin"
setup16c
for _ in $(seq 1 20); do [[ ! -s "$T/data/mounts.txt" ]] && break; sleep 0.25; done
own=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([(m['system'], m['extra'], m['own_games']) for m in json.load(sys.stdin) if m['profile'] == 'PSX-mount'])")
[[ "$own" == "[('psx', True, 1)]" ]] || fail "the extra's games of its own must be counted for the app: $own"
# The user chooses to move them to the main emulator: then the mount goes on.
r=$(curl -sf -X POST "$API/api/emulators/own-games" -d "{\"root\":\"$XA\",\"system\":\"psx\",\"action\":\"move\"}")
[[ "$r" == *'"games":1'* ]] || fail "moving the extra's own games: $r"
[[ -f "$T/psxemu/games/Mine (USA).bin" && ! -e "$XA/games/Mine (USA).bin" ]] || fail "the extra's own game must move to the main emulator"
for _ in $(seq 1 20); do grep -q "^TEST00002|" "$T/data/mounts.txt" 2>/dev/null && break; sleep 0.25; done
grep -q "^TEST00002|" "$T/data/mounts.txt" || fail "with its own games moved, the extra must get its mount"
r=$(curl -s -X POST "$API/api/emulators/own-games" -d "{\"root\":\"$XA\",\"system\":\"psx\",\"action\":\"burn\"}")
[[ "$r" == *"move or delete"* ]] || fail "an unknown action must be refused: $r"
rm "$T/psxemu/games/Mine (USA).bin"
# SwanStationPS5 names a card by serial once it knows it, renaming the one
# named after the game file, and takes it in lower case too.
python3 - "$T/catalog.json" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
for e in cat:
    if e["id"] == "psxmount":
        e["profile"].update({"save_name": "{serial}_1{ext},{rom_stem}_1{ext}", "save_stems": "{serial}_1,{rom_stem}_1"})
json.dump(cat, open(sys.argv[1], "w"))
PY
setup16c
syncnow
strays() { curl -sf "$API/api/status" | python3 -c "import json,sys; print(len(json.load(sys.stdin)['sync']['strays']))"; }
for name in "SLUS-12345_1.mcd" "slus-12345_1.mcd"; do
  mv "$XA/data/saves/"*.mcd "$XA/data/saves/$name"
  syncnow
  [[ ! -e "$XA/data/saves/Test Quest (USA)_1.mcd" ]] || fail "a card renamed to $name must not come back under its old name"
  [[ "$(ls "$XA/data/saves")" == "$name" ]] || fail "the card renamed to $name must stay the game's one card: $(ls "$XA/data/saves")"
  [[ "$(strays)" == 0 ]] || fail "a card named $name is the extra's own, not a stray"
done
rm "$XA/data/saves/"*.mcd
syncnow
[[ -s "$XA/data/saves/SLUS-12345_1.mcd" ]] || fail "a missing card must come back named by serial: $(ls "$XA/data/saves")"
# An emulator that resumes the game on its own (PSXS5's quick resume) would put
# back the old progress over a newer card: its resume state is set aside.
python3 - "$T/catalog.json" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
for e in cat:
    if e["id"] == "psxmount":
        e["profile"]["resume_states"] = "{rom_stem_safe}.resume,{rom_stem_safe}.resume.thumb"
json.dump(cat, open(sys.argv[1], "w"))
PY
setup16c
syncnow
mkdir -p "$XA/data/states"
printf 'old-ram' > "$XA/data/states/Test_Quest__USA_.resume"
printf 'thumb' > "$XA/data/states/Test_Quest__USA_.resume.thumb"
printf 'other' > "$XA/data/states/Other_Game.resume"
sleep 1.1; printf 'newer-card' > "$T/psxemu/saves/Test Quest (USA)_1.mcd"   # its card with the core it runs
syncnow
[[ "$(cat "$XA/data/saves/SLUS-12345_1.mcd")" == newer-card ]] || fail "the newer card must reach the extra"
[[ ! -e "$XA/data/states/Test_Quest__USA_.resume" && ! -e "$XA/data/states/Test_Quest__USA_.resume.thumb" ]] ||
  fail "a resume state older than the card that came in must be set aside: $(ls "$XA/data/states")"
[[ -e "$XA/data/states/Other_Game.resume" ]] || fail "another game's resume state must stay"
ls "$T/data/backups/"*/Test_Quest__USA_.resume.* >/dev/null 2>&1 || fail "the resume state set aside must be in the backups"
# The emulator that saved keeps its own.
printf 'new-ram' > "$XA/data/states/Test_Quest__USA_.resume"
sleep 1.1; printf 'extra-card' > "$XA/data/saves/SLUS-12345_1.mcd"
syncnow
[[ "$(cat "$T/psxemu/saves/Test Quest (USA)_1.mcd")" == extra-card ]] || fail "the extra's card must reach the main emulator"
[[ -e "$XA/data/states/Test_Quest__USA_.resume" ]] || fail "the emulator whose save went out must keep its resume state"
rm -f "$XA/data/states/"*.resume

echo "16d. BIOS files reach every emulator of the platform, named as each one needs, and nobody's own files are replaced"
md5s() { printf '%s' "$1" | md5sum | cut -d' ' -f1; }
# PS2 BIOS are 4 MB; the files beside them in a dump (.nvm, .mec, rom1) are small.
ps2bios() { { printf '%s' "$1"; head -c $((4194304 - ${#1})) /dev/zero; } > "$2"; }
md5p() { ps2bios "$1" /dev/stdout | md5sum | cut -d' ' -f1; }
python3 - "$T/catalog.json" "$(md5s psx-us-bios)" "$(md5p ps2B)" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
for e in cat:
    if e["id"] == "psxlike":
        e["profile"]["firmware"] = {"names": {sys.argv[2]: "scph5501.bin"}, "from": {"ps2": ["ps2A.bin"]}}
    if e["id"] == "ps2like":
        e["profile"]["firmware"] = {"pick_one": [sys.argv[3]]}
json.dump(cat, open(sys.argv[1], "w"))
PY
setup16c
fw() { curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/firmware" -d "$1" >/dev/null; }
fw '{"platform_id":5,"file_name":"SCPH-5501.BIN","content":"psx-us-bios","embed":true,"is_verified":true}'
fw '{"platform_id":5,"file_name":"scph5500.bin","content":"psx-jp-bios"}'
fw '{"platform_id":5,"file_name":"gone.bin","content":"x","missing":true}'
fw '{"platform_id":5,"file_name":"scph5501-twin.bin","content":"psx-us-bios"}'   # RomM keeps it twice
fw '{"platform_id":2,"file_name":"ps2A.bin","content":"ps2A","is_verified":true,"pad":4194304}'
fw '{"platform_id":2,"file_name":"ps2B.bin","content":"ps2B","pad":4194304}'
# A dump's companions on RomM too: the BIOS button of 1.2 brought them all into the folder.
fw '{"platform_id":2,"file_name":"rom1.bin","content":"rom1","is_verified":true}'
fw '{"platform_id":2,"file_name":"SCPH-X.NVM","content":"nvm","is_verified":true}'
mkdir -p "$XA/bios"; printf 'mine' > "$XA/bios/SCPH-5501.BIN"   # the user's own, under RomM's name
# A PCSX2-like folder holding several BIOS from before: each one used has its .nvm.
mkdir -p "$T/ps2emu/bios"; ps2bios ps2A "$T/ps2emu/bios/old-a.bin"; printf 'nvm' > "$T/ps2emu/bios/old-a.nvm"
ps2bios ps2B "$T/ps2emu/bios/x-b.bin"; printf 'nvm' > "$T/ps2emu/bios/x-b.nvm"; ps2bios ps2C "$T/ps2emu/bios/mine.bin"
printf 'rom1' > "$T/ps2emu/bios/rom1.bin"   # RomM's, but not a BIOS: it stays, and is no reason to keep one
touch -d '2 days ago' "$T/ps2emu/bios/x-b.nvm"   # used before, but old-a since
fwhits() { curl -sf "http://127.0.0.1:$MOCK_PORT/_admin/dump" | python3 -c "import json,sys; print(json.load(sys.stdin)['firmware_hits'])"; }
before=$(fwhits)
syncnow
[[ "$(cat "$T/psxemu/bios/scph5501.bin" 2>/dev/null)" == "psx-us-bios" ]] || fail "a BIOS must take the name the emulator opens: $(ls "$T/psxemu/bios" 2>&1)"
[[ "$(cat "$T/psxemu/bios/scph5500.bin" 2>/dev/null)" == "psx-jp-bios" ]] || fail "every region's BIOS must come down"
[[ ! -e "$T/psxemu/bios/gone.bin" && ! -e "$T/psxemu/bios/SCPH-5501.BIN" && ! -e "$T/psxemu/bios/scph5501-twin.bin" ]] \
  || fail "a file missing on RomM, or a renamed one under its old name or a twin's, came down: $(ls "$T/psxemu/bios")"
[[ "$(cat "$XA/bios/scph5500.bin" 2>/dev/null)" == "psx-jp-bios" ]] || fail "the extra emulator must get the BIOS files too"
[[ "$(cat "$XA/bios/SCPH-5501.BIN")" == "mine" ]] || fail "a file of the user's own must never be replaced"
[[ "$(ls "$T/ps2emu/bios" | tr '\n' ' ')" == "mine.bin old-a.bin old-a.nvm rom1.bin x-b.nvm " ]] \
  || fail "of RomM's BIOS in a take-the-first folder only the one in use must stay, the user's own and the dump's other files too: $(ls "$T/ps2emu/bios" 2>&1)"
grep -A1 "NOTIFY\] RommPS: 1 extra BIOS file removed$" "$T/daemon.log" | grep -q "^Kept old-a.bin$" || fail "no notice of the BIOS files removed"
[[ "$(head -c4 "$T/psxemu/bios/ps2A.bin" 2>/dev/null)" == "ps2A" ]] || fail "a BIOS RomM keeps under another platform must come down when the rules name it"
[[ "$(cat "$RA/system/bsx.bin" 2>/dev/null)" == "BSXBIOS" ]] || fail "BIOS files from an older RomM (no list in the platform) must come down too"
grep -q "NOTIFY\] RommPS: [0-9]* BIOS files added" "$T/daemon.log" || fail "no notice of the BIOS files"
hits=$(fwhits)
# scph5501 and scph5500 for both PS1 emulators, ps2A for one of them: three downloads.
[[ $((hits - before)) -eq 3 ]] || fail "each BIOS file must download once, the other emulators get a copy: $((hits - before)) downloads
$(grep 'BIOS' "$T/daemon.log" | tail -20)"
syncnow
[[ "$(fwhits)" == "$hits" ]] || fail "BIOS files already there must not download again"
# One of ours the user swapped for a dump of their own is theirs from then on.
printf 'my own dump' > "$T/psxemu/bios/scph5501.bin"
syncnow
[[ "$(cat "$T/psxemu/bios/scph5501.bin")" == "my own dump" && "$(fwhits)" == "$hits" ]] || fail "a BIOS file the user changed must stay as they left it"
[[ "$(ls "$T/ps2emu/bios" | tr '\n' ' ')" == "mine.bin old-a.bin old-a.nvm rom1.bin x-b.nvm " ]] || fail "a second sync must not add another PS2 BIOS"
# With none there, the preferred one comes down, and only it. A dump's other
# files left behind don't count as one.
rm "$T/ps2emu/bios/"*
printf 'rom1' > "$T/ps2emu/bios/rom1.bin"; printf 'nvm' > "$T/ps2emu/bios/x.nvm"
syncnow
[[ "$(ls "$T/ps2emu/bios" | tr '\n' ' ')" == "ps2B.bin rom1.bin x.nvm " ]] \
  || fail "an emulator that takes the first BIOS it finds must get exactly one, the preferred: $(ls "$T/ps2emu/bios" 2>&1)"

echo "16e. 3DS and Wii U saves go up as RomM's save units, and come back into this console's own account"
Z=00000000000000000000000000000000
CE="$T/cemu"
mkdir -p "$CE/games" "$CE/azahar/games" "$CE/mlc01/usr/save/00050000/1010ec00/user/80000001" "$CE/mlc01/usr/save/00050000/1010ec00/meta" \
         "$CE/azahar/sdmc/Nintendo 3DS/$Z/$Z/title/00040000/00066c00/data/00000001/00slot00"
printf 'WUX0-image' > "$CE/games/Kart (USA).wux"
# The 3DS game's id comes from its header (NCSD, media id at 0x108); the Wii U
# one's only from the front end's list.
python3 -c "import struct,sys; h=bytearray(0x4000); h[0x100:0x104]=b'NCSD'; h[0x108:0x110]=struct.pack('<Q',0x0004000000066C00); open(sys.argv[1],'wb').write(bytes(h))" "$CE/azahar/games/Rumble (USA).cci"
printf '{"wiiu":[{"titleId":"000500001010ec00","path":"%s"}]}' "$CE/games/Kart (USA).wux" > "$CE/library.json"
printf '<content><Account><PersistentId>2147483649</PersistentId></Account></content>' > "$CE/settings.xml"
printf 'kart-save' > "$CE/mlc01/usr/save/00050000/1010ec00/user/80000001/userdata.dat"
printf '<meta/>' > "$CE/mlc01/usr/save/00050000/1010ec00/meta/meta.xml"
S3="$CE/azahar/sdmc/Nintendo 3DS/$Z/$Z/title/00040000/00066c00/data"
printf 'rumble-save' > "$S3/00000001/00slot00/00main.dat"; printf 'meta16bytes.....' > "$S3/00000001.metadata"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/platform" -d '{"id":70,"slug":"3ds","name":"Nintendo 3DS"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/platform" -d '{"id":80,"slug":"wiiu","name":"Wii U"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d '{"id":700,"platform_id":70,"name":"Rumble","fs_name":"Rumble (USA).cci"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d '{"id":800,"platform_id":80,"name":"Kart","fs_name":"Kart (USA).wux"}' >/dev/null
python3 - "$T/catalog.json" "$CE" "$Z" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1])); z = sys.argv[3]
cat.append({"id": "cemulike", "name": "Cemu-like", "detect": [sys.argv[2]],
            "profile": {"root": sys.argv[2], "platforms": [
                {"romm": ["wiiu"], "dir": "wiiu", "emulator": "cemu", "rom_dir": "{root}/games",
                 "save_layout": "wiiu", "bundle_dirs": "{root}/mlc01/usr/save"},
                {"romm": ["3ds"], "dir": "3ds", "emulator": "azahar", "rom_dir": "{root}/azahar/games",
                 "save_layout": "3ds", "bundle_dirs": "{root}/azahar/sdmc/Nintendo 3DS/%s/%s/title" % (z, z)}]}})
json.dump(cat, open(sys.argv[1], "w"))
PY
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\",\"$T/gcA\",\"$T/gcB\",\"$XA\",\"$CE\"],
  \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\",\"ngc\":\"$T/gcA\",\"wii\":\"$T/gcA\",\"wiiu\":\"$CE\",\"3ds\":\"$CE\"},
  \"also\":{\"n64\":[\"$T/n64b\"],\"ngc\":[\"$T/gcB\"],\"wii\":[\"$T/gcB\"],\"psx\":[\"$XA\"]}}" >/dev/null || fail "setup with a Cemu-like emulator refused"
syncnow
units=$(dump | python3 -c "
import json,sys,io,zipfile,urllib.request
d=json.load(sys.stdin); out=[]
for s in d['saves']:
    if s['rom_id'] not in (700,800): continue
    raw=urllib.request.urlopen('http://127.0.0.1:$MOCK_PORT/_admin/save_content?id=%d' % s['id']).read()
    out.append((s['rom_id'], s['file_name'].split(' [')[0], s['emulator'], sorted(zipfile.ZipFile(io.BytesIO(raw)).namelist())))
print(sorted(out))")
[[ "$units" == *"(700, '00066c00', 'azahar', ['00040000/00066c00/data/00000001.metadata', '00040000/00066c00/data/00000001/00slot00/00main.dat'])"* ]] \
  || fail "a 3DS save must go up as RomM's save unit: $units"
[[ "$units" == *"(800, '1010ec00', 'cemu', ['1010ec00/meta/meta.xml', '1010ec00/user/account/userdata.dat'])"* ]] \
  || fail "a Wii U save must go up as RomM's save unit, the account as 'account': $units"
# A newer save from a PC whose Cemu account is 80000002: it lands in this console's 80000001.
sleep 1.1
b64=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('1010ec00/user/80000002/userdata.dat','from-pc'); z.writestr('1010ec00/user/common/common.dat','shared'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d "{\"rom_id\":800,\"slot\":\"autosave\",\"file_name\":\"1010ec00.zip\",\"emulator\":\"cemu\",\"content_b64\":\"$b64\"}" >/dev/null
syncnow
W="$CE/mlc01/usr/save/00050000/1010ec00"
[[ "$(cat "$W/user/80000001/userdata.dat" 2>/dev/null)" == "from-pc" && "$(cat "$W/user/common/common.dat" 2>/dev/null)" == "shared" ]] \
  || fail "a Wii U save from another account must land in this console's account: $(find "$W" -type f)"
[[ "$(cat "$W/meta/meta.xml")" == "<meta/>" && ! -e "$W/user/80000002" ]] || fail "the title's meta must stay, and no other account folder appear"
# A 3DS save in Argosy's form ("data/..." with no title path) is taken too.
sleep 1.1
b64=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('data/00000001/00slot00/00main.dat','from-argosy'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d "{\"rom_id\":700,\"slot\":\"autosave\",\"file_name\":\"Rumble.zip\",\"emulator\":\"azahar\",\"content_b64\":\"$b64\"}" >/dev/null
syncnow
[[ "$(cat "$S3/00000001/00slot00/00main.dat")" == "from-argosy" && -f "$S3/00000001.metadata" ]] || fail "a 3DS save in another client's form must land in place, its metadata kept"

echo "16f. Dreamcast (PSFlyCast): each game's own VMU by disc id, from the front end's list or the disc; shared VMUs as memory cards"
FC="$T/flycast"
mkdir -p "$FC/games" "$FC/data"
printf 'CDI-image' > "$FC/games/Taxi (USA).cdi"   # not read: its id comes from the list
printf '/app0/games/Taxi (USA).cdi\tMK-51035\n' > "$FC/data/game-ids.txt"
# Not in the list yet: its id comes from IP.BIN, track 3's first sector.
printf '3\n1 0 4 2352 track01.bin 0\n2 450 0 2352 track02.raw 0\n3 45000 4 2352 "track 03.bin" 0\n' > "$FC/games/Jet (USA).gdi"
python3 -c "
import sys
ip = b'SEGA SEGAKATANA SEGA ENTERPRISES' + b' ' * 32 + b'HDR-0078  '
sec = b'\x00' + b'\xff' * 10 + b'\x00' + b'\x00\x02\x00\x01' + ip.ljust(2048, b'\x00') + b'\x00' * 288
open(sys.argv[1], 'wb').write(sec * 4)" "$FC/games/track 03.bin"
printf 'taxi-vmu' > "$FC/data/MK-51035_vmu_save_A1.bin"
printf 'shared-vmu' > "$FC/data/vmu_save_A1.bin"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/platform" -d '{"id":90,"slug":"dc","name":"Dreamcast"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d '{"id":900,"platform_id":90,"name":"Taxi","fs_name":"Taxi (USA).cdi"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d '{"id":901,"platform_id":90,"name":"Jet","fs_name":"Jet (USA).gdi"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":901,"slot":"autosave","file_name":"HDR-0078_vmu_save_A1.bin","emulator":"flycast","content":"jet-vmu-from-pc"}' >/dev/null
python3 - "$T/catalog.json" "$FC" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
cat.append({"id": "flylike", "name": "Flycast-like", "detect": [sys.argv[2]],
            "profile": {"root": sys.argv[2], "rom_dir": "{root}/games", "save_dir": "{root}/data",
                        "state_dir": "{root}/data/savestates", "save_exts": ".bin", "state_exts": "",
                        "game_ids": "{root}/data/game-ids.txt", "save_stems": "{game_id}_vmu_save_A1",
                        "save_name": "{game_id}_vmu_save_A1{ext}",
                        "memcards": "{root}/data/vmu_save_A1.bin,{root}/data/vmu_save_B1.bin",
                        "platforms": [{"romm": ["dc", "dreamcast"], "dir": "dc", "emulator": "flycast",
                                       "cards": {"default": "per_game", "modes": "per_game,shared", "change": "Turn on Own memory card for each game",
                                                 "setting": {"file": "{root}/emu.cfg", "key": "PerGameVmu", "per_game": "yes,true", "shared": "no,false"}}}]}})
json.dump(cat, open(sys.argv[1], "w"))
PY
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\",\"$T/gcA\",\"$T/gcB\",\"$XA\",\"$CE\",\"$FC\"],
  \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\",\"ngc\":\"$T/gcA\",\"wii\":\"$T/gcA\",\"wiiu\":\"$CE\",\"3ds\":\"$CE\",\"dc\":\"$FC\"},
  \"also\":{\"n64\":[\"$T/n64b\"],\"ngc\":[\"$T/gcB\"],\"wii\":[\"$T/gcB\"],\"psx\":[\"$XA\"]}}" >/dev/null || fail "setup with a Flycast-like emulator refused"
syncnow
vmus=$(dump | python3 -c "
import json,sys; d=json.load(sys.stdin)
print(sorted((s['rom_id'], s['file_name'].split(' [')[0], s['emulator']) for s in d['saves'] if s['rom_id'] in (900, 901)))")
[[ "$vmus" == *"(900, 'MK-51035_vmu_save_A1', 'flycast')"* ]] || fail "a game's own VMU must go up under its disc id: $vmus"
[[ "$(cat "$FC/data/HDR-0078_vmu_save_A1.bin" 2>/dev/null)" == "jet-vmu-from-pc" ]] \
  || fail "a VMU from RomM must land under the id read from the disc: $(ls "$FC/data")"
dccards() { dump | python3 -c "import json,sys; print([(c['name'], c['versions']) for c in json.load(sys.stdin)['cards'] if c['emulator'] == 'rommps-dc'])"; }
[[ "$(dccards)" == "[]" ]] || fail "a shared VMU synced while each game has its own: $(dccards)"
# The emulator's own setting says which cards it keeps; the shared ones here are listed.
fcards() { curl -sf "$API/api/paths" | python3 -c "
import json,sys; c=[p['cards'] for p in json.load(sys.stdin) if p['profile']=='Flycast-like'][0]
print(c['mode'], [x.rsplit('/',1)[1] for x in c['shared_here']])"; }
[[ "$(fcards)" == "per_game ['vmu_save_A1.bin']" ]] || fail "cards with no setting written: $(fcards)"
printf '[config]\nPerGameVmu = no\n' > "$FC/emu.cfg"
[[ "$(fcards)" == "shared ['vmu_save_A1.bin']" ]] || fail "cards with PerGameVmu off: $(fcards)"
# Shared now: the shared VMU syncs (only the one that exists), the game's own doesn't.
sleep 1.1; printf 'taxi-vmu-2' > "$FC/data/MK-51035_vmu_save_A1.bin"
syncnow
[[ "$(dccards)" == "[('vmu_save_A1.bin', 1)]" ]] || fail "the shared VMU must sync once shared: $(dccards)"
n=$(dump | python3 -c "import json,sys; print(sum(1 for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 900))")
[[ "$n" == 1 ]] || fail "a game's own VMU synced while the cards are shared: $n saves"
rm "$FC/emu.cfg"

echo "16g. CD-i (Seedee5): a game's NVRAM, one file with no extension in a folder named after the game; states by slot"
CD="$T/cdiemu"; NV="$CD/saves/same_cdi/nvram"
mkdir -p "$CD/roms" "$CD/saves/states" "$NV/Zelda (USA)/cdimono1"
printf 'ROM:Zelda' > "$CD/roms/Zelda (USA).chd"; printf 'ROM:Link' > "$CD/roms/Link (USA).chd"
printf 'zelda-nvram' > "$NV/Zelda (USA)/cdimono1/mk48t08"
printf 'zelda-slot1' > "$CD/saves/states/Zelda (USA).slot1.state"
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/platform" -d '{"id":95,"slug":"philips-cd-i","name":"Philips CD-i"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d '{"id":950,"platform_id":95,"name":"Zelda","fs_name":"Zelda (USA).chd"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/rom" -d '{"id":951,"platform_id":95,"name":"Link","fs_name":"Link (USA).chd"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" -d '{"rom_id":951,"slot":"autosave","file_name":"mk48t08","content":"link-nvram-from-pc"}' >/dev/null
python3 - "$T/catalog.json" "$CD" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
cat.append({"id": "cdilike", "name": "Seedee5-like", "detect": [sys.argv[2]],
            "profile": {"root": sys.argv[2], "rom_dir": "{root}/roms",
                        "save_dir": "{root}/saves/same_cdi/nvram/{rom_stem}/cdimono1", "save_exts": "mk48t08", "save_name": "mk48t08",
                        "state_dir": "{root}/saves/states", "state_exts": ".state", "state_format": "raw", "state_cores": "seedee5",
                        "platforms": [{"romm": ["philips-cd-i"], "dir": "cdi", "emulator": "same_cdi"}]}})
json.dump(cat, open(sys.argv[1], "w"))
PY
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/nope\",\"$T/gcA\",\"$T/gcB\",\"$XA\",\"$CE\",\"$FC\",\"$CD\"],
  \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\",\"ngc\":\"$T/gcA\",\"wii\":\"$T/gcA\",\"wiiu\":\"$CE\",\"3ds\":\"$CE\",\"dc\":\"$FC\",\"philips-cd-i\":\"$CD\"},
  \"also\":{\"n64\":[\"$T/n64b\"],\"ngc\":[\"$T/gcB\"],\"wii\":[\"$T/gcB\"],\"psx\":[\"$XA\"]}}" >/dev/null || fail "setup with a Seedee5-like emulator refused"
syncnow
cdi=$(dump | python3 -c "
import json,sys; d=json.load(sys.stdin)
print(sorted((s['rom_id'], s['file_name'].split(' [')[0], s.get('content', '')[:20]) for s in d['saves'] + d['states'] if s['rom_id'] == 950))")
[[ "$cdi" == *"'zelda-nvram')"* ]] || fail "a CD-i game's NVRAM must go up: $cdi"
[[ "$cdi" == *"'Zelda (USA).slot1"* ]] || fail "a CD-i state must go up: $cdi"
[[ "$(cat "$NV/Link (USA)/cdimono1/mk48t08" 2>/dev/null)" == "link-nvram-from-pc" ]] \
  || fail "RomM's NVRAM must land in the game's own folder: $(find "$NV" -type f)"

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
# Each system's folders: detection says where an emulator keeps them, and folders
# picked for one system in setup are used for it alone.
found=$(curl -sf "$API/api/setup/detect" | python3 -c "
import json,sys
for c in json.load(sys.stdin):
    if c['root']=='$HB': print(c['dirs']['snes']['rom_dir'] != '', 'fixed' in c)")
[[ "$found" == "True True" ]] || fail "detection must give each system's folders: $found"
mkdir -p "$T/mine/snes" "$T/mine/snessaves"
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"systems\":{\"snes\":{\"main\":\"$HB\",\"on\":[\"$HB\"],
  \"folders\":{\"$HB\":{\"rom_dir\":\"$T/mine/snes\",\"save_dir\":\"$T/mine/snessaves\",\"state_dir\":\"\"}}}}}" >/dev/null \
  || fail "setup with a system's own folders refused"
sn=$(curl -sf "$API/api/paths" | python3 -c "
import json,sys
for m in json.load(sys.stdin):
    if 'snes' in m['platforms'].split(','): print(m['rom_dir'], m['save_dir'])
    elif m['rom_dir'].startswith('$T/mine/snes'): print('leaked', m['platforms'])")
grep -q "^$T/mine/snes $T/mine/snessaves$" <<<"$sn" && ! grep -q "^leaked" <<<"$sn" || fail "a system's picked folders must apply to it alone: $sn"
post /api/setup/emulators "{\"roots\":[\"$HB\"],\"custom\":[{\"id\":\"newemu-snes\",\"slugs\":[\"snes\",\"sfam\"],\"name\":\"NewEmu (SNES)\",\"rom_dir\":\"$T/porp/games\",\"save_dir\":\"$T/porp/saves\"}]}" >/dev/null

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
# RomM not answering isn't "no cover" (RommPS never asks again after a 404).
code=$(curl -s -o /dev/null -w '%{http_code}' "$API/cover?p=%2Fassets%2Fbusy%2Fsmall.png")
[[ "$code" == 502 ]] || fail "a cover RomM didn't answer for: HTTP $code"
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
# RommPS's picker lists the versions RomM can serve: EarthBound's other (id 99) isn't on it, which
# leaves nothing to pick. A folder of discs lists them, each disc's .bin and .cue counted together.
vers() { curl -sf "$API/api/roms/$1/details" | python3 -c "
import json,sys
print([(v['id'], v['label'], [(d['disc'], d['size']) for d in v.get('discs', [])])
       for v in json.load(sys.stdin).get('version_list', [])])"; }
[[ "$(vers 12)" == "[]" ]] || fail "one version left, nothing to pick: $(vers 12)"
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":71,"platform_id":1,"name":"Disc Game","fs_name":"Disc Game (USA)",
  "files":[{"id":711,"file_name":"Disc Game (USA) (Disc 1).bin","content":"1111"},
           {"id":712,"file_name":"Disc Game (USA) (Disc 1).cue","content":"11"},
           {"id":713,"file_name":"Disc Game (USA) (Disc 2).bin","content":"222"},
           {"id":714,"file_name":"Disc Game (USA).m3u","content":"m3u"}]}' >/dev/null
[[ "$(vers 71)" == "[(71, '', [(1, 6), (2, 3)])]" ]] || fail "a folder's discs: $(vers 71)"
# One disc of it comes down alone (with the .m3u no disc names); then the rest.
discs_here() {
  for _ in $(seq 1 40); do here=$(curl -sf "$API/api/roms/71/on_console"); [[ "$here" == *"\"discs\":$1"* ]] && return; sleep 0.25; done
  fail "discs on the console: $here, wanted $1"
}
post /api/download '{"rom_id":71,"disc":2}' >/dev/null
discs_here "[2]"
[[ -n "$(find "$T" -name 'Disc Game (USA).m3u' -not -path '*/data/*')" ]] || fail "a disc comes with the folder's .m3u"
[[ -z "$(find "$T" -name 'Disc Game (USA) (Disc 1).*' -not -path '*/data/*')" ]] || fail "only the disc asked for comes down"
# The rest once that disc's download is over (its saves synced): the game can't come down twice at once.
for _ in $(seq 1 40); do
  curl -sf "$API/api/downloads" | python3 -c "
import json,sys
sys.exit(any(d.get('rom_id') == 71 and d['status'] not in ('done', 'error', 'cancelled') for d in json.load(sys.stdin)))" && break
  sleep 0.25
done
post /api/download '{"rom_id":71}' >/dev/null
discs_here "[1,2]"
# One disc comes off on its own; the game goes with its last.
post /api/delete '{"rom_id":71,"disc":1}' >/dev/null || fail "removing one disc refused"
discs_here "[2]"
[[ -z "$(find "$T" -name 'Disc Game (USA) (Disc 1).*' -not -path '*/data/*')" ]] || fail "the disc's files must go"
post /api/delete '{"rom_id":71,"disc":2}' >/dev/null || fail "removing the last disc refused"
[[ "$(curl -sf "$API/api/roms/71/on_console")" == *'"installed":false'* ]] || fail "with its last disc the game goes: $(curl -sf "$API/api/roms/71/on_console")"

echo "21. one-file folder games: saves named after the file sync; old extensionless downloads are moved into the folder"
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":70,"platform_id":1,"name":"Folder Game","fs_name":"Folder Game",
  "files":[{"id":701,"file_name":"Folder Game (USA).sfc","content":"ROM:Folder Game (USA).sfc"}]}' >/dev/null
post /api/download '{"rom_id":70}' >/dev/null
# The main emulator's copy: RomM's saves come down to it, and the extra's copy
# would only be picked when find happens to list it first (it did, on CI).
for _ in $(seq 1 20); do got=$(find "$T/homebrew" -path "*/Folder Game/Folder Game (USA).sfc" | head -1); [[ -n "$got" ]] && break; sleep 0.25; done
[[ -n "$got" ]] || fail "a one-file folder game must download into its folder"
gdir=$(dirname "$(dirname "$got")")
sdir=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['save_dir'] for m in json.load(sys.stdin) if m['rom_dir']=='$gdir'][0])")
[[ -n "$sdir" ]] || fail "no save folder for $gdir"
mkdir -p "$sdir"; printf 'folder-save' > "$sdir/Folder Game (USA).srm"
syncnow
saved=$(curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; s=[x for x in json.load(sys.stdin)['saves'] if x['rom_id']==70]; print(s[-1]['content'] if s else 'none')")
[[ "$saved" == "folder-save" ]] || fail "a save named after the file inside a one-file folder must sync: $saved"
# What 1.1.1 left behind: the file named after the folder, no extension.
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":71,"platform_id":1,"name":"Bare Game","fs_name":"Bare Game",
  "files":[{"id":711,"file_name":"Bare Game (USA).sfc","content":"ROM:Bare Game (USA).sfc"}]}' >/dev/null
printf 'ROM:Bare Game (USA).sfc' > "$gdir/Bare Game"; printf 'bare-save' > "$sdir/Bare Game.srm"
syncnow
[[ -f "$gdir/Bare Game/Bare Game (USA).sfc" ]] || fail "an extensionless game must be moved into its folder under the file's name"
[[ "$(cat "$sdir/Bare Game (USA).srm" 2>/dev/null)" == "bare-save" && ! -e "$sdir/Bare Game.srm" ]] || fail "its save must take the file's name"

# A one-file folder game's save from RomM is named after the file inside, as the emulator names it.
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":76,"platform_id":1,"name":"Folder Two","fs_name":"Folder Two",
  "files":[{"id":761,"file_name":"Folder Two (USA).sfc","content":"ROM:Folder Two (USA).sfc"}]}' >/dev/null
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":76,"slot":"autosave","file_name":"Folder Two (USA).srm","content":"folder-two-save"}' >/dev/null
post /api/download '{"rom_id":76}' >/dev/null
for _ in $(seq 1 40); do [[ -f "$sdir/Folder Two (USA).srm" ]] && break; sleep 0.25; done
[[ "$(cat "$sdir/Folder Two (USA).srm" 2>/dev/null)" == "folder-two-save" ]] || fail "a one-file folder game's save must take the file's name: $(ls "$sdir" | grep -i 'folder two')"
[[ ! -e "$sdir/Folder Two.srm" ]] || fail "not the folder's name"

# A save from before RomM's sync slots (no slot) comes down with its game.
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":72,"platform_id":1,"name":"Old Save Game","fs_name":"Old Save Game (USA).sfc",
  "files":[{"id":721,"file_name":"Old Save Game (USA).sfc","content":"ROM:Old Save Game (USA).sfc"}]}' >/dev/null
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":72,"slot":null,"file_name":"Old Save Game (USA) [2026-08-01].srm","content":"from-before-slots"}' >/dev/null
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":72,"slot":null,"file_name":"Old Save Game (USA) [2026-08-01].rtc","content":"clock"}' >/dev/null
post /api/download '{"rom_id":72}' >/dev/null
for _ in $(seq 1 40); do [[ -f "$sdir/Old Save Game (USA).srm" ]] && break; sleep 0.25; done
[[ "$(cat "$sdir/Old Save Game (USA).srm" 2>/dev/null)" == "from-before-slots" ]] || fail "a save with no slot must come down with its game"
if [[ "$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m for m in json.load(sys.stdin) if m['rom_dir']=='$gdir'][0].get('save_exts', '.srm,.rtc'))")" == *rtc* ]]; then
    [[ "$(cat "$sdir/Old Save Game (USA).rtc" 2>/dev/null)" == "clock" ]] || fail "the clock file beside it must come down too"
fi
syncnow
up=$(curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; print(sorted(str(s['slot']) for s in json.load(sys.stdin)['saves'] if s['rom_id']==72))")
[[ "$up" == "['None', 'None', 'autosave', 'autosave-rtc']" ]] || fail "they must then sync in this console's slots: $up"

# Another device's slot: never mixed into this console's save. A game with
# none of its own takes the newest from elsewhere, then syncs in its own slot.
for g in "74 Phone Game" "75 Both Game"; do
    set -- $g
    curl -sfk -X POST "$TM/_admin/rom" -d "{\"id\":$1,\"platform_id\":1,\"name\":\"$2 $3\",\"fs_name\":\"$2 $3 (USA).sfc\",
      \"files\":[{\"id\":${1}1,\"file_name\":\"$2 $3 (USA).sfc\",\"content\":\"ROM:$2 $3 (USA).sfc\"}]}" >/dev/null
done
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":74,"slot":"phone","file_name":"Phone Game (USA).srm","content":"from-the-phone"}' >/dev/null
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":75,"slot":"phone","file_name":"Both Game (USA).srm","content":"phone-copy"}' >/dev/null
sleep 1.1
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":75,"slot":"autosave","file_name":"Both Game (USA).srm","content":"this-slot"}' >/dev/null
post /api/download '{"rom_id":74}' >/dev/null; post /api/download '{"rom_id":75}' >/dev/null
for _ in $(seq 1 40); do [[ -f "$sdir/Phone Game (USA).srm" && -f "$sdir/Both Game (USA).srm" ]] && break; sleep 0.25; done
syncnow
[[ "$(cat "$sdir/Phone Game (USA).srm" 2>/dev/null)" == "from-the-phone" ]] || fail "a game with only another device's save must get it"
[[ "$(cat "$sdir/Both Game (USA).srm" 2>/dev/null)" == "this-slot" ]] || fail "another device's slot must never replace this console's save"
curl -sf "$API/api/events?rom_id=74" | grep -q "From another device" || fail "the sync log must say where it came from"

echo "22. the sync log and the tracked saves"
ev=$(curl -sf "$API/api/events?limit=500" | python3 -c "
import json,sys; L=json.load(sys.stdin)
print(sorted({(e['kind'], e['action']) for e in L}))
print(next((e['game'] for e in L if e['rom_id']==70 and e['action']=='uploaded'), 'none'))")
grep -q "('save', 'uploaded')" <<<"$ev" && grep -q "('state', 'downloaded')" <<<"$ev" && grep -q "('state', 'skipped')" <<<"$ev" \
  || fail "the sync log is missing kinds of entries: $ev"
[[ "$(tail -1 <<<"$ev")" == "Folder Game" ]] || fail "a log entry must name its game: $ev"
one=$(curl -sf "$API/api/events?rom_id=70" | python3 -c "import json,sys; print({e['rom_id'] for e in json.load(sys.stdin)})")
[[ "$one" == "{70}" ]] || fail "the log for one game holds only that game: $one"
tr=$(curl -sf "$API/api/tracked" | python3 -c "
import json,sys; t=json.load(sys.stdin)
g={x['rom_id']: x for p in t['platforms'] for x in p['games']}
print(t['saves'] > 0, t['states'] > 0, g[70]['name'], [f['status'] for f in g[70]['files']])")
[[ "$tr" == "True True Folder Game ['synced']" ]] || fail "tracked saves by platform: $tr"
printf 'changed-again' > "$sdir/Folder Game (USA).srm"
st=$(curl -sf "$API/api/tracked?rom_id=70" | python3 -c "import json,sys; print([f['status'] for p in json.load(sys.stdin)['platforms'] for g in p['games'] for f in g['files']])")
[[ "$st" == "['changed']" ]] || fail "a save changed here must say so: $st"

echo "23. save sync off: RomM only as a game source"
post /api/config '{"save_sync":false}' >/dev/null
n=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")
curl -sfk -X POST "$TM/_admin/rom" -d '{"id":73,"platform_id":1,"name":"No Sync Game","fs_name":"No Sync Game (USA).sfc",
  "files":[{"id":731,"file_name":"No Sync Game (USA).sfc","content":"ROM:No Sync Game (USA).sfc"}]}' >/dev/null
curl -sfk -X POST "$TM/_admin/save" -d '{"rom_id":73,"slot":"autosave","file_name":"No Sync Game (USA).srm","content":"stays-on-romm"}' >/dev/null
post /api/download '{"rom_id":73}' >/dev/null; post /api/sync >/dev/null; sleep 3
[[ -n "$(find "$T" -name "No Sync Game (USA).sfc" -not -path '*/data/*')" ]] || fail "games must still download with save sync off"
[[ ! -e "$sdir/No Sync Game (USA).srm" ]] || fail "no save may come down with save sync off"
[[ "$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin)['sync']['sync_count'])")" == "$n" ]] || fail "no sync may run with save sync off"
grep -q "skipped: save sync is off" "$T/daemon.log" || fail "a skipped sync must say why"
post /api/config '{"save_sync":true}' >/dev/null
syncnow
[[ "$(cat "$sdir/No Sync Game (USA).srm" 2>/dev/null)" == "stays-on-romm" ]] || fail "turned back on, the save must come down"
grep -A1 "\[NOTIFY\] RommPS: No Sync Game$" "$T/daemon.log" | grep -qE "^Downloaded for [A-Za-z]" || fail "a download with save sync off must still say it's done, and for what system"
grep -qE "^(\[NOTIFY\] RommPS: )?([0-9]+ (saves?|states?) (up|down)(, )?)+$" "$T/daemon.log" || fail "a sync's notification must say what moved"
! grep -qE "^(\[NOTIFY\] RommPS: )?0 " "$T/daemon.log" || fail "a notification must not count nothing"
grep -B1 -E "^[0-9]+ saves? (up|down)" "$T/daemon.log" | grep -q "\[NOTIFY\] RommPS: .*No Sync Game" || fail "a sync's notification must name the games"
# RommPS's own messages become the console's notifications.
post /api/notify '{"message":"couldn'"'"'t cancel\nIt already finished"}' >/dev/null
grep -A1 "\[NOTIFY\] RommPS: couldn't cancel$" "$T/daemon.log" | grep -q "^It already finished$" \
  || fail "a message from RommPS must reach the notifications: $(grep -A1 NOTIFY "$T/daemon.log" | tail -4)"

echo "24. save sync per emulator and platform: saves only, then off, then everything"
path_of() { curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['$1'] for m in json.load(sys.stdin) if m['rom_dir']=='$gdir'][0])"; }
emu_root=$(path_of root); stdir=$(path_of state_dir)
[[ -n "$emu_root" && -n "$stdir" ]] || fail "no emulator folders for $gdir"
post /api/config '{"states":"sync"}' >/dev/null
post /api/emulators/sync "{\"root\":\"$emu_root\",\"sync\":\"saves\"}" >/dev/null || fail "choosing saves only refused"
[[ -z "$(path_of state_dir)" && -n "$(path_of save_dir)" ]] || fail "saves only: states must show as not synced"
curl -sfk -X POST "$TM/_admin/state" -d '{"rom_id":73,"emulator":"snes9x","file_name":"No Sync Game (USA).state1","content":"emu-state"}' >/dev/null
printf 'saves-only' > "$sdir/No Sync Game (USA).srm"
syncnow
[[ ! -e "$stdir/No Sync Game (USA).state1" ]] || fail "saves only: a state came down"
last73() { curl -sfk "$TM/_admin/dump" | python3 -c "import json,sys; s=[x for x in json.load(sys.stdin)['saves'] if x['rom_id']==73]; print(s[-1]['content'] if s else 'none')"; }
[[ "$(last73)" == "saves-only" ]] || fail "saves only: the save must still go up: $(last73)"
post /api/emulators/sync "{\"root\":\"$emu_root\",\"sync\":\"off\"}" >/dev/null
sleep 1.1; printf 'not-synced' > "$sdir/No Sync Game (USA).srm"
syncnow
[[ "$(last73)" == "saves-only" ]] || fail "off: a save went up"
[[ -z "$(path_of save_dir)" ]] || fail "off: saves must show as not synced"
post /api/setup/emulators "{\"roots\":[\"$emu_root\"]}" >/dev/null || true
[[ "$(curl -sf "$API/api/config" | python3 -c "import json,sys; print([e.get('sync','all') for p in json.load(sys.stdin)['profiles'] if p.get('root')=='$emu_root' for e in p['platforms']])")" == *"'off'"* ]] || fail "setting up again must keep each platform's choice"
post /api/emulators/sync "{\"root\":\"$emu_root\",\"sync\":\"all\"}" >/dev/null
syncnow
[[ "$(last73)" == "not-synced" ]] || fail "back on: the save must go up: $(last73)"
[[ "$(cat "$stdir/No Sync Game (USA).state1" 2>/dev/null)" == "emu-state" ]] || fail "back on: the state must come down"
curl -s -X POST "$API/api/emulators/sync" -d '{"root":"/nope","sync":"off"}' | grep -q error || fail "an unknown emulator must be refused"
# Per platform, the way the app sets it: only this system's saves stop.
post /api/setup/emulators "{\"roots\":[\"$emu_root\"],\"systems\":{\"snes\":{\"main\":\"$emu_root\",\"on\":[\"$emu_root\"],\"sync\":\"off\"}}}" >/dev/null
modes=$(curl -sf "$API/api/config" | python3 -c "import json,sys; print(sorted((e['romm'][0], e.get('sync','all')) for p in json.load(sys.stdin)['profiles'] if p.get('root')=='$emu_root' for e in p['platforms']))")
[[ "$modes" == *"('snes', 'off')"* && "$modes" == *"('ps2', 'all')"* ]] || fail "save sync must be set for one platform only: $modes"
[[ -z "$(path_of save_dir)" ]] || fail "a platform with save sync off must show its saves as not synced"
post /api/setup/emulators "{\"roots\":[\"$emu_root\"],\"systems\":{\"snes\":{\"main\":\"$emu_root\",\"on\":[\"$emu_root\"],\"sync\":\"all\"}}}" >/dev/null

echo "25. a save or state deleted from the console comes back from RomM"
rm -f "$sdir/No Sync Game (USA).srm"
syncnow
[[ "$(cat "$sdir/No Sync Game (USA).srm" 2>/dev/null)" == "not-synced" ]] || fail "a deleted save must come back"
curl -sf "$API/api/events?rom_id=73" | grep -q "Missing here" || fail "the sync log must say why it came back"
rm -f "$stdir/No Sync Game (USA).state1"
syncnow
[[ "$(cat "$stdir/No Sync Game (USA).state1" 2>/dev/null)" == "emu-state" ]] || fail "a deleted state must come back"

echo "26. RetroArch 2.0: games go to the folders named after libretro's databases, and the ones already here move there"
PS2N="$HB/content/Sony - PlayStation 2"; SNESN="$HB/content/Nintendo - Super Nintendo Entertainment System"
post /api/setup/emulators "{\"roots\":[\"$HB\"]}" >/dev/null || fail "setting up RetroArch refused"
dirs_of() { curl -sf "$API/api/paths" | python3 -c "import json,sys; print(sorted(m['rom_dir'] for m in json.load(sys.stdin) if m['platforms'].split(',')[0] == '$1'))"; }
to_move() { curl -sf "$API/api/status" | python3 -c "import json,sys; print(json.load(sys.stdin).get('retroarch_move',{}).get('games',0))"; }
[[ "$(dirs_of ps2)" == "['$HB/content/ps2']" ]] || fail "before 2.0, PS2 games go to content/ps2: $(dirs_of ps2)"
mkdir -p "$HB/content/ps2/Folder Game (USA)" "$HB/content/snes" "$HB/webui" "$PS2N" "$SNESN"
echo iso > "$HB/content/ps2/Gran Turismo 4 (USA).iso"
echo bin > "$HB/content/ps2/Folder Game (USA)/game.bin"
echo old > "$HB/content/ps2/Both (USA).iso"; echo new > "$PS2N/Both (USA).iso"
echo part > "$HB/content/ps2/Arriving (USA).iso.part"
echo sfc > "$HB/content/snes/Super Metroid (USA).sfc"
echo '{"release": "v1.0.0-beta.1"}' > "$HB/webui/version.json"
sleep 1.1
[[ "$(to_move)" == 0 ]] || fail "nothing moves before 2.0"
# 2.0 plays PS2 with PS5SX2 (an upgrade can leave LRPS2 behind), on two shared cards.
printf 'corename = "PS5SX2"\ndatabase = "Sony - PlayStation 2"\n' > "$HB/cores/ps5sx2_libretro.info"
mkdir -p "$HB/system/pcsx2/memcards"; echo card1 > "$HB/system/pcsx2/memcards/Mcd001.ps2"; echo card2 > "$HB/system/pcsx2/memcards/Mcd002.ps2"
echo '{"release": "v2.0.0-beta-2"}' > "$HB/webui/version.json"
sleep 1.1
waiting=$(curl -sf "$API/api/status" | python3 -c "import json,sys; print(sorted((f['from'].split('/')[-1], f['games']) for f in json.load(sys.stdin)['retroarch_move']['folders']))")
snes_games=$(ls "$HB/content/snes" | wc -l)
[[ "$waiting" == "[('ps2', 2), ('snes', $snes_games)]" ]] || fail "waiting to move: $waiting"
[[ "$(dirs_of ps2)" == "['$PS2N']" && "$(dirs_of snes)" == "['$SNESN']" ]] || fail "with 2.0, games go to the new folders: $(dirs_of ps2) $(dirs_of snes)"
ps2core=$(curl -sf "$API/api/config" | python3 -c "import json,sys; print([(e['emulator'], e['core_name'], 'sync_requires_option' in e) for p in json.load(sys.stdin)['profiles'] if p.get('root')=='$HB' for e in p['platforms'] if e['romm'][0]=='ps2'])")
[[ "$ps2core" == "[('ps5sx2', 'PS5SX2', False)]" ]] || fail "with 2.0, PS2 plays with PS5SX2: $ps2core"
cards=$(curl -sf "$API/api/memcards" | python3 -c "import json,sys; print(sorted(s['name'] for s in json.load(sys.stdin)['slots'] if s['path'].startswith('$HB/')))")
[[ "$cards" == "['RetroArch Mcd001.ps2', 'RetroArch Mcd002.ps2']" ]] || fail "PS5SX2's two shared cards sync: $cards"
# RetroArch's history names games as it sees them, under /app0.
mkdir -p "$HB/playlists/builtin"
cat > "$HB/playlists/builtin/content_history.lpl" <<'LPL'
{"version": "1.5", "items": [
 {"path": "/app0/content/snes/Super Metroid (USA).sfc", "label": "Super Metroid", "core_path": "DETECT", "core_name": "DETECT"},
 {"path": "/app0/content/Saturn/Other.chd", "label": "Not ours", "core_path": "DETECT", "core_name": "DETECT"}]}
LPL
r=$(post /api/retroarch/move) || fail "moving refused"
[[ "$r" == *"\"moved\":$((snes_games + 2))"* ]] || fail "moved: $r"
[[ -f "$PS2N/Gran Turismo 4 (USA).iso" && -f "$PS2N/Folder Game (USA)/game.bin" && -f "$SNESN/Super Metroid (USA).sfc" ]] || fail "the games didn't move"
[[ ! -e "$HB/content/snes" ]] || fail "an emptied old folder must go"
[[ "$(cat "$PS2N/Both (USA).iso")" == new && "$(cat "$HB/content/ps2/Both (USA).iso")" == old ]] || fail "a game in both folders must stay as it is"
[[ -f "$HB/content/ps2/Arriving (USA).iso.part" ]] || fail "a download still arriving must stay"
[[ "$(to_move)" == 0 ]] || fail "nothing should be left to move: $(to_move)"
hist=$(python3 -c "import json,sys; print([i['path'] for i in json.load(open(sys.argv[1]))['items']])" "$HB/playlists/builtin/content_history.lpl")
[[ "$hist" == "['/app0/content/Nintendo - Super Nintendo Entertainment System/Super Metroid (USA).sfc', '/app0/content/Saturn/Other.chd']" ]] || fail "the history must follow the moved games, and only them: $hist"
curl -s -X POST "$API/api/retroarch/move" | grep -q error || fail "a second move must say there's nothing to move"

echo "26b. RetroArch 3.0: ES-DE's folders (content/snes, content/ps2), 2.x's renamed by RetroArch itself"
# What 3.0 does on its first start: 2.x's folders renamed, or merged into one already there.
echo '{"release": "v4.0.0-beta-4"}' > "$HB/webui/version.json"
mv "$SNESN" "$HB/content/snes"
for f in "$PS2N"/*; do mv "$f" "$HB/content/ps2/"; done; rmdir "$PS2N"
sleep 1.1; curl -sf "$API/api/status" >/dev/null; sleep 1.1
[[ "$(dirs_of ps2)" == "['$HB/content/ps2']" && "$(dirs_of snes)" == "['$HB/content/snes']" ]] || fail "with 3.0, games go to ES-DE's folders: $(dirs_of ps2) $(dirs_of snes)"
[[ "$(to_move)" == 0 ]] || fail "nothing to move once RetroArch moved its folders: $(to_move)"
[[ "$(curl -sf "$API/api/roms/11/on_console" | python3 -c "import json,sys; print(json.load(sys.stdin)['installed'])")" == True ]] \
  || fail "a game RetroArch's folder rename took along must still be on the console"
grep -q "the games' records follow" "$T/daemon.log" || fail "the records didn't follow RetroArch's rename"

echo "27. the device name: the console's own unless one is set, and RomM is told"
# A name set in the web UI goes to RomM at the next sync; clearing it goes
# back to the console's own (the host build has none: "PlayStation 5").
devname() { curl -sf "$API/api/config" | python3 -c "import json,sys; print(json.load(sys.stdin)['device_name'])"; }
server() { curl -sf "$API/api/config" | python3 -c "import json,sys; print(json.load(sys.stdin)['server_url'])"; }
romm_name() { curl -skf "$(server)/_admin/dump" | python3 -c "import json,sys; print(json.load(sys.stdin)['device_name'])"; }
[[ "$(romm_name)" == "PlayStation 5" ]] || fail "RomM wasn't told the device's name: $(romm_name)"
post /api/config '{"device_name":"Den PS5"}' >/dev/null
post /api/sync >/dev/null
for _ in $(seq 1 40); do [[ "$(romm_name)" == "Den PS5" ]] && break; sleep 0.25; done
[[ "$(devname)/$(romm_name)" == "Den PS5/Den PS5" ]] || fail "a name set in the web UI: $(devname)/$(romm_name)"
post /api/config '{"device_name":""}' >/dev/null
[[ "$(devname)" == "PlayStation 5" ]] || fail "clearing the name: $(devname)"

echo "28. Switch: the library shows the game, its update and DLC come with it; archives are turned down"
SV=$(server)
nxrom() { curl -skf -X POST "$SV/_admin/rom" -d "$1" >/dev/null || fail "mock rom: $1"; }
curl -skf -X POST "$SV/_admin/platform" -d '{"id":90,"slug":"switch","name":"Nintendo Switch"}' >/dev/null
nxrom '{"id":900,"platform_id":90,"name":"Poke","fs_name":"Poke [0100000011D90000][v0].nsp"}'
nxrom '{"id":901,"platform_id":90,"name":"Poke","fs_name":"Poke [0100000011D90800][v65536].nsp"}'
nxrom '{"id":902,"platform_id":90,"name":"Poke","fs_name":"Poke [0100000011D90800][v393216].nsp"}'
nxrom '{"id":903,"platform_id":90,"name":"Poke DLC","fs_name":"Poke DLC [0100000011D91001][v0].nsp"}'
# Another game whose title id starts the same: not Poke's.
nxrom '{"id":904,"platform_id":90,"name":"Poke Spinoff","fs_name":"Poke Spinoff [0100000011D96000][v0].nsp"}'
nxrom '{"id":905,"platform_id":90,"name":"Sword","fs_name":"Sword","files":[
  {"id":9051,"file_name":"Sword [0100ABCD12340000][v0].nsp","content":"ROM:sword"},
  {"id":9052,"file_name":"Sword [0100ABCD12340800][v65536].nsp","content":"ROM:sword-update"},
  {"id":9053,"file_name":"Sword Expansion.nsp","category":"dlc","content":"ROM:sword-dlc"}]}'
nxrom '{"id":906,"platform_id":90,"name":"Mansion","fs_name":"Mansion (NSZ).rar"}'
nxrom '{"id":907,"platform_id":90,"name":"Mystery","fs_name":"Mystery","files":[{"id":9071,"file_name":"Mystery (Base Game).rar","content":"RAR"}]}'
NX="$T/nxemu"; mkdir -p "$NX"
python3 - "$API" "$NX" <<'PY' >/dev/null
import json, sys, urllib.request
api, root = sys.argv[1], sys.argv[2]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [p for p in cfg["profiles"] if p.get("id") != "nxlike"] + [{
    "id": "nxlike", "name": "NX-like", "enabled": True, "root": root, "rom_dir": "{root}/roms",
    "update_dir": "{root}/updates", "dlc_dir": "{root}/updates", "bios_dir": "{root}", "sync_saves": False,
    "firmware": {"by_ext": {".keys": "keys", ".nca": "firmware"}},
    "platforms": [{"romm": ["switch"], "dir": "switch", "emulator": "nxlike"}]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
lib=$(curl -sf "$API/api/roms?platform_id=90&limit=60" | python3 -c "
import json,sys
for g in json.load(sys.stdin)['items']: print(g['id'], bool(g.get('not_game')), g.get('addon',''), bool(g.get('archived')))")
for want in "900 False  False" "901 True update False" "902 True update False" "903 True dlc False" "904 False  False" \
            "905 False  False" "906 False  True"; do
  grep -qx "$want" <<<"$lib" || fail "Switch library listing, '$want': $lib"
done
# One game RomM keeps three times (an .nsp, a .rar and a folder, versions of each other): one card.
nxrom '{"id":910,"platform_id":90,"name":"Dup","fs_name":"Dup [0100CCCC00000000][v0].nsp","sibling_roms":[{"id":911},{"id":912}]}'
nxrom '{"id":911,"platform_id":90,"name":"Dup","fs_name":"Dup (NSZ).rar","sibling_roms":[{"id":910},{"id":912}]}'
nxrom '{"id":912,"platform_id":90,"name":"Dup","fs_name":"Dup [0100CCCC00000000][v0]","sibling_roms":[{"id":910},{"id":911}],
  "files":[{"id":9121,"file_name":"Dup.nsp","content":"x"}]}'
dup=$(curl -sf "$API/api/roms?platform_id=90&limit=60&search=dup" | python3 -c "
import json,sys
for i in json.load(sys.stdin)['items']: print(i['id'], bool(i.get('not_game')), i.get('same_as',''), i.get('versions',''))")
[[ "$dup" == $'910 False  \n911 True 910 \n912 True 910 ' ]] || fail "a Switch game RomM keeps several times must show once, the .nsp: $dup"
[[ "$(curl -sf "$API/api/roms?platform_id=90&limit=60" | python3 -c "import json,sys; print([i['id'] for i in json.load(sys.stdin)['items'] if i.get('version')])")" == "[]" ]] \
  || fail "Switch entries carry no version label (their versions are files)"
det=$(curl -sf "$API/api/roms/900/details" | python3 -c "
import json,sys; d=json.load(sys.stdin); print(d.get('updates'), d.get('dlc'), [(a['kind'], a['name']) for a in d.get('addons', [])])")
[[ "$det" == "1 1 [('update', 'Poke [0100000011D90800][v393216].nsp'), ('dlc', 'Poke DLC [0100000011D91001][v0].nsp')]" ]] \
  || fail "a Switch game's details list its newest update and its DLC: $det"
# Title ids: the game's, and each update's and DLC's.
det=$(curl -sf "$API/api/roms/905/details" | python3 -c "
import json,sys; d=json.load(sys.stdin); print(d.get('title_id'), [(a['kind'], a.get('title_id')) for a in d.get('addons', [])])")
[[ "$det" == "0100ABCD12340000 [('update', '0100ABCD12340800'), ('dlc', '')]" ]] || fail "a Switch game's details carry the title ids: $det"
# A folder holding a collection: each game its own, the others' updates aren't this one's.
nxrom '{"id":910,"platform_id":90,"name":"Collection","fs_name":"Collection [NSP]","files":[
  {"id":9101,"file_name":"First [0100AAAA00010000][v0].nsp","content":"ROM:first"},
  {"id":9102,"file_name":"First [0100AAAA00010800][v65536].nsp","content":"ROM:first-update"},
  {"id":9103,"file_name":"Second [0100BBBB00020000][v0].nsp","content":"ROM:second"},
  {"id":9104,"file_name":"Second [0100BBBB00020800][v131072].nsp","content":"ROM:second-update"}]}'
det=$(curl -sf "$API/api/roms/910/details" | python3 -c "
import json,sys; d=json.load(sys.stdin)
print(d.get('title_id'), d.get('updates'), [a.get('title_id') for a in d.get('addons', [])], [(a['kind'], a.get('title_id')) for a in d.get('bundle', [])])")
[[ "$det" == "0100AAAA00010000 1 ['0100AAAA00010800'] [('game', '0100BBBB00020000'), ('update', '0100BBBB00020800')]" ]] \
  || fail "a collection's other games and their updates must not be listed as this game's updates: $det"
[[ "$(curl -sf "$API/api/roms/906/details" | python3 -c "import json,sys; print(json.load(sys.stdin).get('archived',''))")" == *"archived (.rar)"* ]] \
  || fail "the details say an archive can't be downloaded"
r=$(curl -s -X POST "$API/api/download" -d '{"rom_id":906}')
[[ "$r" == *"archived (.rar). Decompress it"* ]] || fail "an archived game must be turned down at once: $r"
r=$(curl -s -X POST "$API/api/download" -d '{"rom_id":907}')
[[ "$r" == *"archived (.rar)"* ]] || fail "a folder holding an archive too: $r"
nxdone() { for _ in $(seq 1 60); do st=$(curl -sf "$API/api/downloads" | python3 -c "import json,sys; d=[d for d in json.load(sys.stdin) if d.get('rom_id') == $1]; print(d[0]['status'] if d else '')"); [[ "$st" == done || "$st" == error ]] && break; sleep 0.25; done; echo "$st"; }
post /api/download '{"rom_id":900,"name":"Poke"}' >/dev/null
[[ "$(nxdone 900)" == done ]] || fail "the Switch download didn't finish: $(curl -sf "$API/api/downloads")"
[[ -f "$NX/roms/Poke [0100000011D90000][v0].nsp" ]] || fail "the game goes in the games folder: $(find "$NX" -type f)"
[[ -f "$NX/updates/Poke [0100000011D90800][v393216].nsp" && -f "$NX/updates/Poke DLC [0100000011D91001][v0].nsp" ]] \
  || fail "its newest update and its DLC go in updates: $(find "$NX" -type f)"
[[ ! -e "$NX/updates/Poke [0100000011D90800][v65536].nsp" && ! -e "$NX/roms/Poke Spinoff [0100000011D96000][v0].nsp" ]] \
  || fail "not the older update, nor another game: $(find "$NX" -type f)"
inst=$(curl -sf "$API/api/installed?limit=100" | python3 -c "import json,sys; print(sorted(g['id'] for g in json.load(sys.stdin) if g['platform_id'] == 90))")
[[ "$inst" == "[900]" ]] || fail "the update and DLC aren't games of their own on the console: $inst"
post /api/download '{"rom_id":905,"name":"Sword"}' >/dev/null
[[ "$(nxdone 905)" == done ]] || fail "the Switch folder game didn't finish"
[[ -f "$NX/roms/Sword [0100ABCD12340000][v0].nsp" && ! -d "$NX/roms/Sword" ]] || fail "a folder game's own file goes straight in the games folder: $(find "$NX" | sort)"
[[ -f "$NX/updates/Sword [0100ABCD12340800][v65536].nsp" && -f "$NX/updates/Sword Expansion.nsp" ]] \
  || fail "its update, and its DLC by RomM's category, go in updates: $(find "$NX" -type f)"
post /api/delete '{"rom_id":900}' >/dev/null
[[ ! -e "$NX/roms/Poke [0100000011D90000][v0].nsp" && ! -e "$NX/updates/Poke DLC [0100000011D91001][v0].nsp" && \
   ! -e "$NX/updates/Poke [0100000011D90800][v393216].nsp" ]] || fail "removing the game removes its update and DLC: $(find "$NX" -type f)"
[[ -f "$NX/updates/Sword Expansion.nsp" ]] || fail "another game's DLC stays"
# Games an older version downloaded as a folder, matched to RomM by a sync: unfolded once the
# profiles are saved (or the payload starts); an empty one, a download that never finished, goes.
mkdir -p "$NX/roms/Old" "$NX/roms/Empty"
printf 'ROM:old' > "$NX/roms/Old/Old [0100AAAA00000000][v0].nsp"; printf 'ROM:old-up' > "$NX/roms/Old/Old [0100AAAA00000800][v1].nsp"
nxrom '{"id":908,"platform_id":90,"name":"Old","fs_name":"Old","files":[
  {"id":9081,"file_name":"Old [0100AAAA00000000][v0].nsp","content":"ROM:old"},{"id":9082,"file_name":"Old [0100AAAA00000800][v1].nsp","content":"ROM:old-up"}]}'
nxrom '{"id":909,"platform_id":90,"name":"Empty","fs_name":"Empty","files":[{"id":9091,"file_name":"Empty [0100BBBB00000000][v0].nsp","content":"ROM:e"}]}'
syncnow
onc() { curl -sf "$API/api/roms/$1/on_console" | python3 -c "import json,sys; print(json.load(sys.stdin)['installed'])"; }
[[ "$(onc 908)/$(onc 909)" == "True/True" ]] || fail "the old folders must be matched first: $(onc 908)/$(onc 909)"
python3 - "$API" <<'PY' >/dev/null
import json, sys, urllib.request
api = sys.argv[1]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": cfg["profiles"]}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
for _ in $(seq 1 40); do [[ ! -d "$NX/roms/Old" && ! -d "$NX/roms/Empty" ]] && break; sleep 0.25; done
[[ -f "$NX/roms/Old [0100AAAA00000000][v0].nsp" && -f "$NX/updates/Old [0100AAAA00000800][v1].nsp" && ! -d "$NX/roms/Old" ]] \
  || fail "an old folder download must be unfolded: $(find "$NX" | sort)"
[[ ! -d "$NX/roms/Empty" ]] || fail "an empty folder must go"
[[ "$(onc 908)/$(onc 909)" == "True/False" ]] || fail "the unfolded game stays on the console, the empty one doesn't: $(onc 908)/$(onc 909)"


echo "28b. Switch saves: the emulator's first user's, a zip per game; never another user's; left alone when it syncs them itself"
U1=AAAA0000000000000000000000000001; U2=BBBB0000000000000000000000000002; NS="$NX/user/save"
mkdir -p "$NS/$U1/0100ABCD12340000" "$NS/$U2/0100ABCD12340000"
printf 'sword-u1' > "$NS/$U1/0100ABCD12340000/SaveData.bin"
printf 'sword-u2' > "$NS/$U2/0100ABCD12340000/SaveData.bin"
printf '{"profiles": {"first": "%s", "users": {"1": "%s"}}}' "$U1" "$U1" > "$NX/users.json"
curl -skf -X POST "$SV/_admin/save" -d "{\"rom_id\":908,\"slot\":\"autosave\",\"file_name\":\"0100AAAA00000000.zip\",\"content_b64\":\"$(python3 -c "
import base64,io,zipfile
b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('0100AAAA00000000/SaveData.bin','old-from-switch'); z.close()
print(base64.b64encode(b.getvalue()).decode())")\"}" >/dev/null
nxsaves() { python3 - "$API" "$NX" "$1" <<'PY' >/dev/null
import json, sys, urllib.request
api, root, own = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
for p in cfg["profiles"]:
    if p.get("id") == "nxlike":
        p["sync_saves"] = True
        p["platforms"][0].update({"save_layout": "switch", "bundle_dirs": "{root}/user/save/{switch_user}",
                                  "switch_users": "{root}/users.json", "switch_sync": own})
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": cfg["profiles"]}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
}
nxsaves "{root}/sync.json"
syncnow
nx=$(curl -skf "$SV/_admin/dump" | python3 -c "
import json,sys; d=json.load(sys.stdin)
print(sorted((s['rom_id'], s['file_name'].split(' [')[0]) for s in d['saves'] if s['rom_id'] in (905, 908)))")
[[ "$nx" == *"(905, '0100ABCD12340000"* ]] || fail "the first user's Switch save must go up, as a zip named by title id: $nx"
# What went up is the first user's: it comes back from RomM when it's gone here.
rm -r "$NS/$U1/0100ABCD12340000"
syncnow
[[ "$(cat "$NS/$U1/0100ABCD12340000/SaveData.bin" 2>/dev/null)" == "sword-u1" ]] \
  || fail "the first user's save must be the one on RomM: $(find "$NS" -type f)"
[[ "$(cat "$NS/$U2/0100ABCD12340000/SaveData.bin")" == "sword-u2" ]] || fail "another user's saves must stay as they are"
[[ "$(cat "$NS/$U1/0100AAAA00000000/SaveData.bin" 2>/dev/null)" == "old-from-switch" ]] \
  || fail "RomM's Switch save must land in the user's folder: $(find "$NS" -type f)"
# The emulator syncing this user's saves itself (ProsperoEden's save-sync.json): left alone, and said why.
printf '{"profiles": [{"profile": "%s", "url": "https://elsewhere.example", "token": "x"}]}' "$U1" > "$NX/sync.json"
nxsaves "{root}/sync.json"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['sync_note'] for m in json.load(sys.stdin) if m['profile'] == 'NX-like'])")
[[ "$note" == *"syncs this user's saves itself"* ]] || fail "an emulator syncing its own saves must be left alone, and say so: $note"
rm "$NX/sync.json"
echo "    a game RomM gave a new id (a rescan): matched again, by name"
# RomM knows no title id for it either, so each sync asks RomM again (as for a console's Pokemon Mystery Dungeon).
curl -skf -X POST "$SV/_admin/rom" -d '{"id":909,"platform_id":90,"name":"Nameless","fs_name":"Nameless.nsp"}' >/dev/null
printf 'ROM:nameless' > "$NX/roms/Nameless.nsp"
syncnow
nxid() { python3 -c "import json,sys; print(json.load(open(sys.argv[1]))['roms'].get(sys.argv[2], {}).get('rom_id'))" "$T/data/state.json" "$NX/roms/Nameless.nsp"; }
[[ "$(nxid)" == 909 ]] || fail "the game without a title id in its name wasn't matched: $(nxid)"
curl -skf -X POST "$SV/_admin/renumber" -d '{"from":909,"to":919}' >/dev/null
syncnow
grep -q "Nameless.nsp: RomM no longer has game 909, looked for again" "$T/daemon.log" || fail "a game gone from RomM wasn't let go"
syncnow
[[ "$(nxid)" == 919 ]] || fail "the game wasn't matched again under its new id: $(nxid)"
rm "$NX/roms/Nameless.nsp"
echo "    Switch firmware is one set: RomM's goes only where there's none of the user's"
curl -skf -X POST "$SV/_admin/firmware" -d '{"platform_id":90,"file_name":"aaaa.nca","content":"romm-fw-a"}' >/dev/null
curl -skf -X POST "$SV/_admin/firmware" -d '{"platform_id":90,"file_name":"bbbb.cnmt.nca","content":"romm-fw-b"}' >/dev/null
rm -rf "$NX/firmware"
syncnow
[[ "$(ls "$NX/firmware" 2>/dev/null | tr '\n' ' ')" == "aaaa.nca bbbb.cnmt.nca " ]] || fail "RomM's firmware must fill an empty folder: $(ls "$NX/firmware" 2>&1)"
printf 'my-fw' > "$NX/firmware/mine.nca"   # the user puts their own set in
syncnow
[[ "$(ls "$NX/firmware" | tr '\n' ' ')" == "mine.nca " ]] || fail "RomM's firmware beside the user's set must come out: $(ls "$NX/firmware")"
grep -q "NOTIFY\] RommPS: removed RomM's Switch firmware" "$T/daemon.log" || fail "no notice of RomM's firmware removed"
syncnow
[[ "$(ls "$NX/firmware" | tr '\n' ' ')" == "mine.nca " ]] || fail "RomM's firmware must not come back beside the user's: $(ls "$NX/firmware")"

echo "    two Switch emulators, the second an extra: one save on RomM, in both, whichever was saved in"
NX2="$T/nxemu2"; NS2="$NX2/user/save"
mkdir -p "$NX2/roms" "$NS2/$U1/0100ABCD12340000"
printf '{"profiles": {"first": "%s", "users": {"1": "%s"}}}' "$U1" "$U1" > "$NX2/users.json"
# Turned on with a save of its own from before, never synced: it gets the main one's.
printf 'sword-extra-old' > "$NS2/$U1/0100ABCD12340000/SaveData.bin"
touch -d "2 days ago" "$NS2/$U1/0100ABCD12340000/SaveData.bin"
python3 - "$API" "$NX2" <<'PY' >/dev/null
import json, sys, urllib.request
api, root = sys.argv[1], sys.argv[2]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [p for p in cfg["profiles"] if p.get("id") != "nxextra"] + [{
    "id": "nxextra", "name": "NX-extra", "enabled": True, "root": root, "rom_dir": "{root}/roms", "sync_saves": True,
    "extra": ["switch"],
    "platforms": [{"romm": ["switch"], "dir": "switch", "emulator": "nxextra", "save_layout": "switch",
                   "bundle_dirs": "{root}/user/save/{switch_user}", "switch_users": "{root}/users.json"}]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
# RommPS puts the main one's games in the extra's folder (a copy here, a mount on a console that can).
for _ in $(seq 1 40); do [[ -e "$NX2/roms/Sword [0100ABCD12340000][v0].nsp" ]] && break; sleep 0.25; done
[[ -e "$NX2/roms/Sword [0100ABCD12340000][v0].nsp" ]] || fail "the main Switch emulator's game didn't reach the extra"
syncnow
[[ "$(cat "$NS2/$U1/0100ABCD12340000/SaveData.bin")" == sword-u1 ]] \
  || fail "a Switch extra just turned on must get the main emulator's save: $(cat "$NS2/$U1/0100ABCD12340000/SaveData.bin")"
ls "$T/data/backups/905/" 2>/dev/null | grep -q . || fail "the extra's own save must be backed up first"
# Saved in the main one: it goes up, and the extra gets it.
sleep 1.1; printf 'sword-main-2' > "$NS/$U1/0100ABCD12340000/SaveData.bin"
syncnow
[[ "$(cat "$NS2/$U1/0100ABCD12340000/SaveData.bin")" == sword-main-2 ]] \
  || fail "a save made in the main Switch emulator must reach the extra: $(cat "$NS2/$U1/0100ABCD12340000/SaveData.bin")"
# Saved in the extra: it goes up, and the main one gets it.
sleep 1.1; printf 'sword-extra-3' > "$NS2/$U1/0100ABCD12340000/SaveData.bin"
syncnow
[[ "$(cat "$NS/$U1/0100ABCD12340000/SaveData.bin")" == sword-extra-3 ]] \
  || fail "a save made in the Switch extra must reach the main emulator: $(cat "$NS/$U1/0100ABCD12340000/SaveData.bin")"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "two Switch emulators in step kept moving saves: $(last uploaded) up, $(last downloaded) down"

echo "28c. Xbox saves: E:\\UDATA\\<title id> inside xemu's hard disk image, as RomM's save unit; never while xemu runs"
XB="$T/homebrew/PPSA97358"; XS="$T/xbsave"; mkdir -p "$XB/games" "$XB/hdd" "$XS/006412E217B6"
python3 "$ROOT/tests/make_disc.py" xiso "$XB/games/Halo (USA).xiso.iso" 4D530004
head -c 4000 /dev/urandom > "$XB/games/Rally (USA).xiso.iso"   # unreadable: the id comes from RomM's save_target
printf 'halo-save' > "$XS/006412E217B6/savegame.bin"; printf 'meta' > "$XS/TitleMeta.xbx"
python3 "$ROOT/tests/xboxhdd.py" make "$XB/hdd/disk.qcow2" 4D530004 "$XS" --compressed
printf "[sys.files]\nhdd_path = '%s'\n" "$XB/hdd/disk.qcow2" > "$XB/xemu.toml"
curl -skf -X POST "$SV/_admin/platform" -d '{"id":96,"slug":"xbox","name":"Xbox"}' >/dev/null
curl -skf -X POST "$SV/_admin/rom" -d '{"id":960,"platform_id":96,"name":"Halo","fs_name":"Halo (USA).xiso.iso"}' >/dev/null
curl -skf -X POST "$SV/_admin/rom" -d '{"id":961,"platform_id":96,"name":"Rally","fs_name":"Rally (USA).xiso.iso","title_id":"LA-020","save_target":"4C410014"}' >/dev/null
python3 - "$API" "$XB" <<'PY' >/dev/null
import json, sys, urllib.request
api, root = sys.argv[1], sys.argv[2]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [p for p in cfg["profiles"] if p.get("id") != "xboxlike"] + [{
    "id": "xboxlike", "name": "Xbox-like", "enabled": True, "root": root, "rom_dir": "{root}/games", "bios_dir": "{root}",
    "sync_saves": True, "platforms": [{"romm": ["xbox"], "dir": "xbox", "emulator": "xemu", "save_layout": "xbox"}]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
xbunits() { curl -skf "$SV/_admin/dump" | python3 -c "
import json,sys,io,ssl,zipfile,urllib.request
d=json.load(sys.stdin); out=[]; tls=ssl._create_unverified_context()
for s in d['saves']:
    if s['rom_id'] not in (960,961): continue
    z=zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$SV/_admin/save_content?id=%d' % s['id'], context=tls).read()))
    out.append((s['rom_id'], s['file_name'].split(' [')[0], s['emulator'], sorted((n, z.read(n).decode()) for n in z.namelist())))
print(sorted(out))"; }
syncnow
units=$(xbunits)
[[ "$units" == *"(960, '4D530004', 'xemu', [('4d530004/006412E217B6/savegame.bin', 'halo-save'), ('4d530004/TitleMeta.xbx', 'meta')])"* ]] \
  || fail "an Xbox save must go up as RomM's save unit, read out of the hard disk image: $units"
# While xemu runs, its disk is left alone: what changes there waits, and so does RomM's save for the other game.
b64=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('4C410014/SLOT0/rally.sav','rally-from-pc'); z.writestr('4C410014/TitleMeta.xbx','rally'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -skf -X POST "$SV/_admin/save" -d "{\"rom_id\":961,\"slot\":\"autosave\",\"file_name\":\"4C410014.zip\",\"emulator\":\"xemu\",\"content_b64\":\"$b64\"}" >/dev/null
echo "PPSA97358" > "$T/data/alive"
printf 'halo-save-2' > "$XS/006412E217B6/savegame.bin"
python3 "$ROOT/tests/xboxhdd.py" make "$XB/hdd/disk.qcow2" 4D530004 "$XS"
units=$(xbunits)
syncnow
[[ "$(xbunits)" == "$units" ]] || fail "nothing on the disk must go up while xemu has it open: $(xbunits)"
[[ "$(python3 "$ROOT/tests/xboxhdd.py" get "$XB/hdd/disk.qcow2" 4C410014 "$T/xbout")" == none ]] \
  || fail "a save must not go onto the disk while xemu has it open"
rm "$T/data/alive"
syncnow
units=$(xbunits)
[[ "$units" == *"'4d530004/006412E217B6/savegame.bin', 'halo-save-2'"* ]] || fail "the save changed while xemu ran must go up once it's closed: $units"
python3 "$ROOT/tests/xboxhdd.py" get "$XB/hdd/disk.qcow2" 4C410014 "$T/xbout" >/dev/null
[[ "$(cat "$T/xbout/4C410014/SLOT0/rally.sav" 2>/dev/null)" == "rally-from-pc" && "$(cat "$T/xbout/4C410014/TitleMeta.xbx")" == rally ]] \
  || fail "RomM's save (by save_target) must land on the disk once xemu is closed: $(find "$T/xbout" -type f)"
python3 "$ROOT/tests/xboxhdd.py" check "$XB/hdd/disk.qcow2" >/dev/null || fail "the hard disk image must stay consistent: $(python3 "$ROOT/tests/xboxhdd.py" check "$XB/hdd/disk.qcow2" 2>&1)"
python3 "$ROOT/tests/xboxhdd.py" get "$XB/hdd/disk.qcow2" 4D530004 "$T/xbout" >/dev/null
[[ "$(cat "$T/xbout/4d530004/006412E217B6/savegame.bin")" == halo-save-2 ]] || fail "the other game's save on the disk must stay as it was"
[[ ! -e "$XB/hdd/disk.qcow2.romm" ]] || fail "no copy of the image may be left behind"
syncnow
[[ "$(xbunits)" == "$units" ]] || fail "a save just placed must not go up again: $(xbunits)"

echo "28c1. An Xbox image cut short on the server: its save can't be placed, said once, not a failure every sync"
python3 "$ROOT/tests/make_disc.py" xiso "$T/whole.xiso.iso" 4D530099
head -c $((41 * 2048)) "$T/whole.xiso.iso" > "$XB/games/Cut (USA).xiso.iso"   # stops where its default.xbe starts
curl -skf -X POST "$SV/_admin/rom" -d '{"id":962,"platform_id":96,"name":"Cut","fs_name":"Cut (USA).xiso.iso"}' >/dev/null
b64=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('4D530099/SLOT0/cut.sav','cut'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -skf -X POST "$SV/_admin/save" -d "{\"rom_id\":962,\"slot\":\"autosave\",\"file_name\":\"4D530099.zip\",\"emulator\":\"xemu\",\"content_b64\":\"$b64\"}" >/dev/null
syncnow
told() { grep -c "Its saves can't sync: the game file is incomplete" "$T/daemon.log"; }
[[ "$(told)" == 1 ]] || fail "a game file cut short must be said, once: $(told) times"
[[ "$(last failed)" == 0 ]] || fail "a game file cut short isn't a failed sync: $(last failed) failed"
syncnow
[[ "$(told)" == 1 && "$(last failed)" == 0 ]] || fail "said again or failed again on the next sync: $(told) times, $(last failed) failed"
[[ "$(python3 "$ROOT/tests/xboxhdd.py" get "$XB/hdd/disk.qcow2" 4D530099 "$T/xbout")" == none ]] || fail "a save went onto the disk for a game with no id"
rm "$XB/games/Cut (USA).xiso.iso"

echo "28c2. DS saves: melonDS's .sav and the melonDS DS core's .srm are one raw save, shared between a main emulator and an extra"
curl -skf -X POST "$SV/_admin/platform" -d '{"id":98,"slug":"nds","name":"Nintendo DS"}' >/dev/null
curl -skf -X POST "$SV/_admin/rom" -d '{"id":980,"platform_id":98,"name":"Pocket Test","fs_name":"Pocket Test (USA).nds"}' >/dev/null
DM="$T/dsmain"; DX="$T/dsextra"; mkdir -p "$DM/games" "$DM/saves" "$DX/games" "$DX/saves"
printf 'ROM:pocket' > "$DM/games/Pocket Test (USA).nds"
printf 'ds-main-1' > "$DM/saves/Pocket Test (USA).sav"
python3 - "$API" "$DM" "$DX" <<'PY' >/dev/null
import json, sys, urllib.request
api, dm, dx = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
plat = {"romm": ["nds"], "dir": "nds", "save_format": "nds-raw"}
profiles = [p for p in cfg["profiles"] if p.get("id") not in ("dsmain", "dsextra")] + [
    {"id": "dsmain", "name": "DS-main", "enabled": True, "root": dm, "rom_dir": "{root}/games", "save_dir": "{root}/saves",
     "save_exts": ".sav", "sync_saves": True, "platforms": [dict(plat, emulator="melonds")]},
    {"id": "dsextra", "name": "DS-extra", "enabled": True, "root": dx, "rom_dir": "{root}/games", "save_dir": "{root}/saves",
     "save_exts": ".srm", "sync_saves": True, "extra": ["nds"], "platforms": [dict(plat, emulator="melondsds")]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
for _ in $(seq 1 40); do [[ -e "$DX/games/Pocket Test (USA).nds" ]] && break; sleep 0.25; done
[[ -e "$DX/games/Pocket Test (USA).nds" ]] || fail "the main DS emulator's game didn't reach the extra"
syncnow
[[ "$(cat "$DX/saves/Pocket Test (USA).srm" 2>/dev/null)" == ds-main-1 ]] \
  || fail "the main DS emulator's save must reach the extra as its .srm: $(ls "$DX/saves")"
sleep 1.1; printf 'ds-extra-2' > "$DX/saves/Pocket Test (USA).srm"
syncnow
[[ "$(cat "$DM/saves/Pocket Test (USA).sav")" == ds-extra-2 ]] || fail "a save made in the DS extra must reach the main emulator's .sav"
ds=$(curl -skf "$SV/_admin/dump" | python3 -c "
import json,sys; print([s['content'] for s in sorted((s for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 980), key=lambda s: s['updated_at'])][-1:])")
[[ "$ds" == *ds-extra-2* ]] || fail "RomM must have the DS save made last: $ds"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "two DS emulators in step kept moving saves: $(last uploaded) up, $(last downloaded) down"

echo "28c4. RetroArch's other cores for a system (\"cores\") keep the save too; DeSmuME's .dsv carries the same save plus its footer"
RL="$T/ralike"; mkdir -p "$RL/games" "$RL/saves"
python3 - "$API" "$RL" <<'PY' >/dev/null
import json, sys, urllib.request
api, root = sys.argv[1], sys.argv[2]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [p for p in cfg["profiles"] if p.get("id") != "ralike"] + [
    {"id": "ralike", "name": "RA-like", "enabled": True, "root": root, "rom_dir": "{root}/games", "save_dir": "{root}/saves",
     "sync_saves": True, "extra": ["nds"], "platforms": [
        {"romm": ["nds"], "dir": "nds", "emulator": "desmume", "core_name": "DeSmuME", "save_exts": ".dsv", "save_format": "nds-dsv",
         "cores": [{"core_name": "melonDS DS", "emulator": "melondsds"}]}]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
for _ in $(seq 1 40); do [[ -e "$RL/games/Pocket Test (USA).nds" ]] && break; sleep 0.25; done
syncnow
dsv() { python3 -c "
import sys; b=open(sys.argv[1],'rb').read(); c=b'|-DESMUME SAVE-|'
print(b[:-122].decode() if b.endswith(c) and len(b) >= 122 else 'no footer: %r' % b[-20:])" "$1"; }
[[ "$(dsv "$RL/saves/Pocket Test (USA).dsv" 2>&1)" == "$(cat "$DM/saves/Pocket Test (USA).sav")" ]] \
  || fail "DeSmuME must get the main emulator's save with its footer: $(dsv "$RL/saves/Pocket Test (USA).dsv" 2>&1)"
[[ "$(cat "$RL/saves/Pocket Test (USA).srm" 2>/dev/null)" == "$(cat "$DM/saves/Pocket Test (USA).sav")" ]] \
  || fail "melonDS DS, RetroArch's other DS core, must get the save too: $(ls "$RL/saves")"
sleep 1.1
python3 -c "
import sys; p=sys.argv[1]; b=open(p,'rb').read(); open(p,'wb').write(b'ds-dsv-3' + b[-122:])" "$RL/saves/Pocket Test (USA).dsv"
syncnow
[[ "$(cat "$DM/saves/Pocket Test (USA).sav")" == ds-dsv-3 ]] || fail "a save made in DeSmuME must reach the main emulator without the footer: $(head -c 40 "$DM/saves/Pocket Test (USA).sav")"
ds=$(curl -skf "$SV/_admin/dump" | python3 -c "
import json,sys; print([s['content'] for s in sorted((s for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 980), key=lambda s: s['updated_at'])][-1:])")
[[ "$ds" == "['ds-dsv-3']" ]] || fail "RomM must get DeSmuME's save without its footer: $ds"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "DeSmuME's save and the others in step kept moving: $(last uploaded) up, $(last downloaded) down"

echo "28c5. Mupen64Plus standalone: its .eep/.sra/.fla/.mpk, named from its database, go to RomM as one .srm and come back"
curl -skf -X POST "$SV/_admin/rom" -d '{"id":962,"platform_id":6,"name":"Pocket Kart","fs_name":"Pocket Kart (U).v64"}' >/dev/null
M6="$T/m64like"; mkdir -p "$M6/roms" "$M6/saves" "$M6/data"
m64=$(python3 - "$M6" <<'PY'
import hashlib, sys
root = sys.argv[1]
z = bytearray(b"\xff" * 0x1000)
z[0:4] = b"\x80\x37\x12\x40"; z[0x10:0x18] = bytes.fromhex("1122334455667788")
z[0x20:0x34] = b"POCKET KART".ljust(20)
md5 = hashlib.md5(bytes(z)).hexdigest().upper()
v = bytearray(z)  # the .v64 dump: each 16-bit word the other way round
v[0::2], v[1::2] = z[1::2], z[0::2]
open(root + "/roms/Pocket Kart (U).v64", "wb").write(v)
good = "Pocket Kart Deluxe Championship Edition (U) [!]"
open(root + "/data/mupen64plus.ini", "w").write("[OTHER]\nGoodName=Other\n\n[%s]\nGoodName=%s\nCRC=11223344 55667788\n" % (md5, good))
print("%.32s-%.8s" % (good, md5))
PY
)
python3 -c "import sys; open(sys.argv[1], 'wb').write(b'EEP1' + b'\xff' * 0x1fc)" "$M6/saves/$m64.eep"   # a 4 Kbit game's, as it writes it
python3 - "$API" "$M6" <<'PY' >/dev/null
import json, sys, urllib.request
api, root = sys.argv[1], sys.argv[2]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [p for p in cfg["profiles"] if p.get("id") != "m64like"] + [
    {"id": "m64like", "name": "M64-like", "enabled": True, "root": root, "rom_dir": "{root}/roms", "sync_saves": True,
     "save_dir": "{data}/n64/m64like", "save_exts": ".srm", "n64_split_dir": "{root}/saves",
     "n64_ini": "{root}/data/mupen64plus.ini", "extra": ["n64"],
     "platforms": [{"romm": ["n64"], "dir": "n64", "save_format": "n64-srm"}]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
syncnow
srm() { curl -skf "$SV/_admin/dump" | python3 -c "
import json,sys,ssl,urllib.request
s=[s for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 962]
s=sorted(s, key=lambda s: s['updated_at'])[-1] if s else None
b=urllib.request.urlopen('$SV/_admin/save_content?id=%d' % s['id'], context=ssl._create_unverified_context()).read() if s else b''
print(len(b), b[:4], b[0x20800:0x20804], s['slot'] if s else '')"; }
[[ "$(srm)" == "296960 b'EEP1' b'\xff\xff\xff\xff' autosave"* ]] || fail "Mupen64Plus's .eep must go up as a .srm: $(srm)"
# A newer save from another device: its SRAM comes out as Mupen64Plus's .sra, the .eep as it reads it.
curl -skf "$SV/_admin/dump" | python3 -c "
import json,sys,ssl,urllib.request,base64
s=sorted((s for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 962), key=lambda s: s['updated_at'])[-1]
b=bytearray(urllib.request.urlopen('$SV/_admin/save_content?id=%d' % s['id'], context=ssl._create_unverified_context()).read())
b[0x20800:0x20804] = b'SRA1'
print(json.dumps({'rom_id': 962, 'slot': s['slot'], 'file_name': 'Pocket Kart (U).srm', 'content_b64': base64.b64encode(bytes(b)).decode()}))" > "$T/m64-up.json"
sleep 1.1; curl -skf -X POST "$SV/_admin/save" -d @"$T/m64-up.json" >/dev/null
syncnow
parts() { (cd "$M6/saves" && for f in *; do printf '%s:%s:%s ' "$f" "$(stat -c %s "$f")" "$(head -c 4 "$f")"; done); }
[[ "$(parts)" == "$m64.eep:2048:EEP1 $m64.sra:32768:SRA1 " ]] || fail "RomM's newer save must come out as Mupen64Plus's files: $(parts)"
syncnow
[[ "$(last uploaded)" == 0 && "$(last downloaded)" == 0 ]] || fail "Mupen64Plus's files and RomM's .srm in step kept moving: $(last uploaded) up, $(last downloaded) down"
rm -f "$M6/saves/"*
syncnow
[[ "$(parts)" == "$m64.eep:2048:EEP1 $m64.sra:32768:SRA1 " ]] || fail "files gone must come back under the name its database gives: $(parts)"

echo "28c3. A system in a folder of its own (PS5-ECAM 5.1): its \"user_dir\", the first that has \"user_dir_has\""
curl -skf -X POST "$SV/_admin/platform" -d '{"id":94,"slug":"ngp","name":"Neo Geo Pocket"}' >/dev/null
curl -skf -X POST "$SV/_admin/rom" -d '{"id":940,"platform_id":94,"name":"Pocket Own","fs_name":"Pocket Own (USA).ngp"}' >/dev/null
EC="$T/ecamlike"; EN="$T/ngpsys"; mkdir -p "$EC" "$EN/games" "$EN/saves"
printf 'ROM:own' > "$EN/games/Pocket Own (USA).ngp"
printf 'ngp-own-1' > "$EN/saves/Pocket Own (USA).sav"
python3 - "$API" "$EC" "$EN" <<'PY' >/dev/null
import json, sys, urllib.request
api, root, own = sys.argv[1], sys.argv[2], sys.argv[3]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [p for p in cfg["profiles"] if p.get("id") != "ecamlike"] + [
    {"id": "ecamlike", "name": "ECAM-like", "enabled": True, "root": root, "sync_saves": True, "platforms": [
        {"romm": ["ngp"], "dir": "ngp", "user_dir": "{root}," + own, "user_dir_has": "saves",
         "rom_dir": "{user_dir}/games", "save_dir": "{user_dir}/saves", "save_exts": ".sav"}]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
syncnow
own=$(curl -skf "$SV/_admin/dump" | python3 -c "import json,sys; print([s['content'] for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 940])")
[[ "$own" == "['ngp-own-1']" ]] || fail "the save in the system's own folder must go up (the root has no saves/ in it): $own"
[[ ! -e "$EC/games" && ! -e "$EC/saves" ]] || fail "nothing may be made in the folder the system left: $(ls "$EC")"

echo "28d. Xbox 360 saves: the first signed-in player's <title id>/00000001 and its headers, as a zip per game; games in a folder each"
XE="$T/x360emu"; XU="$XE/saves/E0005F7A522743A4"; XO="$XE/saves/E0000000000000AB"
mkdir -p "$XE/games" "$XU/4D5307E6/00000001/SaveGame" "$XU/4D5307E6/Headers/00000001" "$XU/4D5307E6/00000002" "$XO/4D5307E6/00000001"
printf 'e0005f7a522743a4\r\n0\r\n0\r\n0\r\n' > "$XE/saves/local-players.txt"
printf 'gears' > "$XU/4D5307E6/00000001/SaveGame/data.bin"; printf 'hdr' > "$XU/4D5307E6/Headers/00000001/SaveGame.header"
printf 'dlc' > "$XU/4D5307E6/00000002/dlc.bin"; printf 'other-player' > "$XO/4D5307E6/00000001/x.bin"
curl -skf -X POST "$SV/_admin/platform" -d '{"id":97,"slug":"xbox360","name":"Xbox 360"}' >/dev/null
curl -skf -X POST "$SV/_admin/rom" -d '{"id":971,"platform_id":97,"name":"Gears","fs_name":"Gears (USA).iso","save_target":"4D5307E6"}' >/dev/null
python3 - "$API" "$XE" <<'PY' >/dev/null
import json, sys, urllib.request
api, root = sys.argv[1], sys.argv[2]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [p for p in cfg["profiles"] if p.get("id") != "x360like"] + [{
    "id": "x360like", "name": "X360-like", "enabled": True, "root": root, "rom_dir": "{root}/games", "bios_dir": "{root}",
    "sync_saves": True, "platforms": [{"romm": ["xbox360"], "dir": "xbox360", "emulator": "xenia", "game_folders": True,
    "save_layout": "x360", "bundle_dirs": "{root}/saves/{xuid}", "x360_users": "{root}/saves/local-players.txt"}]}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
post /api/download '{"rom_id":971,"name":"Gears"}' >/dev/null
for _ in $(seq 1 80); do [[ -f "$XE/games/Gears (USA)/Gears (USA).iso" ]] && break; sleep 0.25; done
[[ -f "$XE/games/Gears (USA)/Gears (USA).iso" && ! -e "$XE/games/Gears (USA).iso" ]] \
  || fail "a single-file game must land in a folder of its own: $(find "$XE/games")"
# A GOD game: RomM has no title id for it (sigil doesn't read GOD), its header does. It comes down as the tree it is.
god=$(python3 -c "import json; print(json.dumps({'id':972,'platform_id':97,'name':'War','fs_name':'War GOD','files':[
  {'id':9721,'file_name':'2E07093A','sub':'41560741/00007000','content':'LIVE'+chr(0)*0x35C+'AV'+chr(7)+'A'+chr(0)*64},
  {'id':9722,'file_name':'Data0000','sub':'41560741/00007000/2E07093A.data','content':'GODDATA'}]}))")
curl -skf -X POST "$SV/_admin/rom" -d "$god" >/dev/null
post /api/download '{"rom_id":972,"name":"War"}' >/dev/null
G="$XE/games/War GOD/41560741/00007000"
for _ in $(seq 1 80); do [[ -f "$G/2E07093A.data/Data0000" ]] && break; sleep 0.25; done
[[ -f "$G/2E07093A" && "$(cat "$G/2E07093A.data/Data0000" 2>/dev/null)" == GODDATA ]] \
  || fail "a GOD game must come down with its folders: $(find "$XE/games/War GOD")"
mkdir -p "$XU/41560741/00000001/Campaign"; printf 'war-save' > "$XU/41560741/00000001/Campaign/s.bin"
x360units() { curl -skf "$SV/_admin/dump" | python3 -c "
import json,sys,io,ssl,zipfile,urllib.request
d=json.load(sys.stdin); out=[]; tls=ssl._create_unverified_context()
for s in d['saves']:
    if s['rom_id'] != 971: continue
    z=zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$SV/_admin/save_content?id=%d' % s['id'], context=tls).read()))
    out.append((s['rom_id'], s['file_name'].split(' [')[0], s['emulator'], sorted((n, z.read(n).decode()) for n in z.namelist())))
print(sorted(out))"; }
syncnow
units=$(x360units)
[[ "$units" == "[(971, '4D5307E6', 'xenia', [('4D5307E6/00000001/SaveGame/data.bin', 'gears'), ('4D5307E6/Headers/00000001/SaveGame.header', 'hdr')])]" ]] \
  || fail "a 360 save must go up as a zip of the first player's title folder, saves and headers only: $units"
war=$(curl -skf "$SV/_admin/dump" | python3 -c "import json,sys; print([s['file_name'].split(' [')[0] for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 972])")
[[ "$war" == "['41560741']" ]] || fail "a GOD game's save must go up under the title id in its header: $war"
# A newer one from another client, in lowercase: it lands in the same player's folder; DLC and the other player stay.
sleep 1.1
b64=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('4d5307e6/00000001/SaveGame/data.bin','from-pc'); z.writestr('4d5307e6/Headers/00000001/SaveGame.header','hdr-pc'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -skf -X POST "$SV/_admin/save" -d "{\"rom_id\":971,\"slot\":\"autosave\",\"file_name\":\"4D5307E6.zip\",\"emulator\":\"xenia\",\"content_b64\":\"$b64\"}" >/dev/null
syncnow
[[ "$(cat "$XU/4D5307E6/00000001/SaveGame/data.bin")/$(cat "$XU/4D5307E6/Headers/00000001/SaveGame.header")" == "from-pc/hdr-pc" ]] \
  || fail "RomM's 360 save must land in the first player's folder: $(find "$XE/saves" -type f)"
[[ "$(cat "$XU/4D5307E6/00000002/dlc.bin")/$(cat "$XO/4D5307E6/00000001/x.bin")" == "dlc/other-player" ]] \
  || fail "the title's other content and other players' saves must stay as they are"
# Nobody signed in: nothing to sync, and it says why.
printf '0\r\n0\r\n0\r\n0\r\n' > "$XE/saves/local-players.txt"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['sync_note'] for m in json.load(sys.stdin) if m['profile'] == 'X360-like'])")
[[ "$note" == *"No player profile in X360-like yet"* ]] || fail "a 360 emulator with no player must say so: $note"

echo "29. a download that failed with RomM away starts again once it answers; failed ones can be retried"
SV=$(server)
curl -skf -X POST "$SV/_admin/rom" -d '{"id":950,"platform_id":1,"name":"Retry Me","fs_name":"Retry Me (USA).sfc"}' >/dev/null
job() { curl -sf "$API/api/downloads" | python3 -c "import json,sys; d=[d for d in json.load(sys.stdin) if d.get('rom_id') == $1]; print((d[0]['status'] + '|' + str(d[0]['id']) + '|' + d[0].get('error','')) if d else '')"; }
waitjob() { local s; for _ in $(seq 1 120); do s=$(job "$1"); [[ "${s%%|*}" == "$2" ]] && break; sleep 0.25; done; echo "$s"; }
curl -skf -X POST "$SV/_admin/offline" -d '{"seconds":3}' >/dev/null
sleep 0.5
post /api/download '{"rom_id":950,"name":"Retry Me"}' >/dev/null
s=$(waitjob 950 error)
[[ "$s" == error*"starts again once it does"* ]] || fail "a download failing with RomM away must say it starts again: $s"
# RomM is back; the next request that finds it answering puts the download back.
sleep 6; post /api/sync >/dev/null
s=$(waitjob 950 done)
[[ "$s" == done* ]] || fail "the download must start again by itself once RomM answers: $s
$(grep -E 'answers again|Retry Me' "$T/daemon.log" | tail -5)"
grep -q "answers again: restarting 1 download" "$T/daemon.log" || fail "no log of the download starting again"
# One that failed for another reason waits for the user: Retry queues it again.
curl -skf -X POST "$SV/_admin/platform" -d '{"id":91,"slug":"3do","name":"3DO"}' >/dev/null
curl -skf -X POST "$SV/_admin/rom" -d '{"id":951,"platform_id":91,"name":"No Emu","fs_name":"No Emu.iso"}' >/dev/null
post /api/download '{"rom_id":951,"name":"No Emu"}' >/dev/null
s=$(waitjob 951 error); id=$(cut -d'|' -f2 <<< "$s")
[[ "$s" == *"no emulator"* ]] || fail "expected a download with no emulator to fail: $s"
sleep 1; [[ "$(job 951)" == error* ]] || fail "a download that failed for its own reason must not start again by itself"
r=$(curl -s -X POST "$API/api/downloads/retry" -d "{\"id\":$id}")
[[ "$r" == *'"ok"'* ]] || fail "retry of a failed download refused: $r"
s=$(waitjob 951 error); [[ "$(cut -d'|' -f2 <<< "$s")" == "$id" ]] || fail "a retried download keeps its place: $s"
grep -c "download of No Emu failed" "$T/daemon.log" | grep -qx 2 || fail "the retried download must have run again"
# A transfer cut off halfway picks up where it stopped, in the same go.
python3 -c "import json,sys; json.dump({'id': 952, 'platform_id': 1, 'name': 'Cut Short', 'fs_name': 'Cut Short (USA).sfc',
  'files': [{'id': 9521, 'file_name': 'Cut Short (USA).sfc', 'content': 'Z' * 300000}]}, open(sys.argv[1], 'w'))" "$T/cut.json"
curl -skf -X POST "$SV/_admin/rom" -d @"$T/cut.json" >/dev/null
curl -skf -X POST "$SV/_admin/cut" -d '{"bytes":100000}' >/dev/null
post /api/download '{"rom_id":952,"name":"Cut Short"}' >/dev/null
s=$(waitjob 952 done)
[[ "$s" == done* ]] || fail "a download cut off halfway must carry on: $s
$(grep -E 'Cut Short|cut off' "$T/daemon.log" | tail -5)"
grep -q "Cut Short (USA).sfc cut off at 100000 bytes" "$T/daemon.log" || fail "no log of the download carrying on"
cut_file=$(find "$T" -name 'Cut Short (USA).sfc' -not -path '*/data/*' | head -1)
[[ "$(wc -c < "$cut_file")" == 300000 && "$(tr -d Z < "$cut_file" | wc -c)" == 0 ]] || fail "the carried-on download must be whole: $cut_file $(wc -c < "$cut_file")"
# A big one goes over several connections at once, in pieces (#39: the PS5
# holds each connection to 512 KB in flight); one cut off halfway carries on
# from where the file stops being whole (the cut skips the first answer, the
# one that says how big the file is), and every byte lands in its place.
python3 -c "import json,sys; json.dump({'id': 954, 'platform_id': 1, 'name': 'Big One', 'fs_name': 'Big One (USA).sfc',
  'files': [{'id': 9541, 'file_name': 'Big One (USA).sfc', 'content': ''.join('%015d\n' % i for i in range(5 * 2**20))}]},
  open(sys.argv[1], 'w'))" "$T/big.json"
curl -skf -X POST "$SV/_admin/rom" -d @"$T/big.json" >/dev/null
curl -skf -X POST "$SV/_admin/cut" -d '{"bytes":5000000,"after":1}' >/dev/null
post /api/download '{"rom_id":954,"name":"Big One"}' >/dev/null
s=$(waitjob 954 done)
[[ "$s" == done* ]] || fail "a big download cut off halfway must carry on: $s
$(grep -E 'Big One|cut off' "$T/daemon.log" | tail -5)"
grep -q "Big One (USA).sfc cut off at" "$T/daemon.log" || fail "no log of the big download carrying on"
grep -qE "downloaded [0-9.]+ MB in .* over [2-9] connections" "$T/daemon.log" || fail "a big download must go over several connections
$(grep -E 'downloaded' "$T/daemon.log" | tail -3)"
big=$(find "$T" -name 'Big One (USA).sfc' -not -path '*/data/*' | head -1)
python3 -c "import hashlib,sys; want=''.join('%015d\n' % i for i in range(5 * 2**20)).encode()
sys.exit(0 if open(sys.argv[1],'rb').read() == want else 1)" "$big" || fail "the big download must be whole, every piece in its place: $big"
# A file RomM lists but no longer has on its disk says so, and isn't retried.
curl -skf -X POST "$SV/_admin/rom" -d '{"id":953,"platform_id":1,"name":"Moved Away","fs_name":"Moved Away","files":[
  {"id":9531,"file_name":"Moved Away (Disc 1).sfc","content":"ROM:one"},
  {"id":9532,"file_name":"Moved Away (Disc 2).sfc","content":"ROM:two","gone":true}]}' >/dev/null
post /api/download '{"rom_id":953,"name":"Moved Away"}' >/dev/null
s=$(waitjob 953 error)
[[ "$s" == *"Moved Away (Disc 2).sfc is missing from its server. If it was moved or renamed, rescan the platform in RomM"* ]] \
  || fail "a file missing on RomM's disk must say so: $s"
grep -q "Moved Away.*cut off" "$T/daemon.log" && fail "a 404 must not be retried as a cut-off transfer"
done_id=$(job 950 | cut -d'|' -f2)
r=$(curl -s -X POST "$API/api/downloads/retry" -d "{\"id\":$done_id}")
[[ "$r" == *"finished"* ]] || fail "a finished download can't be retried: $r"


echo "30. emulators found after setup are announced once, pointing at Emulator Settings"
D2="$T/data2"; mkdir -p "$D2"
echo '{"web_port": 8785, "sync_interval_min": 0, "profiles_custom": true, "profiles": []}' > "$D2/config.json"
second() {
  ROMM_SYNC_HOMEBREW="$T/homebrew" ROMM_SYNC_MOUNTS="$MB" ROMM_SYNC_DATA="$D2" ROMM_SYNC_AUTOSTART="$T/etaHEN2" \
    ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_APP_DIRS="$T/apps" ROMM_SYNC_FS_ROOTS="$T" \
    "$ROOT/build/host/romm-sync" >>"$D2/daemon.log" 2>&1 &
  local pid=$!; sleep 3; kill $pid; wait $pid 2>/dev/null || true
}
second
n=$(grep -c "NOTIFY\] RommPS: found" "$D2/daemon.log")
[[ "$n" == 1 ]] || fail "new emulators must be announced: $(grep -E 'NOTIFY|new emulators' "$D2/daemon.log")"
grep -q "Turn them on in Emulator Settings" "$D2/daemon.log" || fail "the notice must say where to turn them on"
second
[[ "$(grep -c "NOTIFY\] RommPS: found" "$D2/daemon.log")" == 1 ]] || fail "the same emulators must not be announced twice"

echo "30b. a system a newer preset adds joins the saved profile; excluded when another emulator plays it"
D3="$T/data3"; mkdir -p "$D3"
python3 - "$D3/config.json" "$T" <<'PY'
import json, sys
t = sys.argv[2]
json.dump({"web_port": 8786, "sync_interval_min": 0, "profiles_custom": True, "profiles": [
    {"id": "psxlike", "name": "PSX-like", "enabled": True, "root": t + "/psxemu", "platforms": []},
    {"id": "mine", "name": "My PSX", "enabled": True, "root": t + "/mypsx", "platforms": [{"romm": ["psx"], "dir": "psx"}]},
    {"id": "n64b", "name": "Second N64", "enabled": True, "root": t + "/n64b", "platforms": []}]}, open(sys.argv[1], "w"))
PY
ROMM_SYNC_HOMEBREW="$T/homebrew" ROMM_SYNC_MOUNTS="$MB" ROMM_SYNC_DATA="$D3" ROMM_SYNC_AUTOSTART="$T/etaHEN3" \
  ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_APP_DIRS="$T/apps" ROMM_SYNC_FS_ROOTS="$T" \
  "$ROOT/build/host/romm-sync" >>"$D3/daemon.log" 2>&1 &
pid=$!; sleep 3; kill $pid; wait $pid 2>/dev/null || true
got=$(python3 -c "
import json,sys
for p in json.load(open(sys.argv[1]))['profiles']:
    print(p['id'], [e['romm'][0] for e in p.get('platforms', [])], p.get('exclude', []))" "$D3/config.json")
grep -qx "psxlike \['psx'\] \['psx'\]" <<<"$got" || fail "a new system another emulator plays must come in excluded: $got"
grep -qx "n64b \['n64'\] \[\]" <<<"$got" || fail "a new system nobody plays must come in: $got"
grep -qx "mine \['psx'\] \[\]" <<<"$got" || fail "the emulator playing it keeps it: $got"

echo "30b2. a system's folders the old preset shipped follow the new one; the user's own stay"
D4="$T/data4"; mkdir -p "$D4"
python3 - "$T/catalog.json" "$D4/config.json" "$T" <<'PY'
import json, sys
cat, t = json.load(open(sys.argv[1])), sys.argv[3]
cat.append({"id": "ps5cemu", "name": "PS5-ECAM", "detect": [t + "/ecam2"], "profile": {"root": t + "/ecam2", "platforms": [
    {"romm": ["nds"], "dir": "nds", "user_dir": "{root}/melonds," + t + "/melonds2", "rom_dir": "{user_dir}/games",
     "save_dir": "{user_dir}/saves", "bios_dir": "{user_dir}/bios"}]}})
json.dump(cat, open(sys.argv[1], "w"))
json.dump({"web_port": 8787, "sync_interval_min": 0, "profiles_custom": True, "profiles": [
    {"id": "ps5cemu", "name": "PS5-ECAM", "enabled": True, "root": t + "/ecam2", "platforms": [
        {"romm": ["nds"], "dir": "nds", "rom_dir": "{root}/melonds/games", "save_dir": "{root}/melonds/saves",
         "bios_dir": "/mine/bios"}]},
    {"id": "moved", "name": "Moved", "enabled": True, "root": t + "/moved", "platforms": [
        dict(romm=[s], dir=s, user_dir="{root}/" + s + "," + t + "/moved" + s, user_dir_has="moved.txt", rom_dir="{user_dir}/games",
             save_dir="{user_dir}/saves")
        for s in ("gb", "gbc", "gba")]}]},
    open(sys.argv[2], "w"))
PY
# Where each system was before (an older RommPS put its BIOS back there): copies go, anything else stays.
# GBA's move was put off: both folders are there, unmarked, and stay as they are.
for s in gb gbc gba; do mkdir -p "$T/moved$s/bios" "$T/moved/$s/bios" "$T/moved/$s/games"; printf "bios-$s" > "$T/moved$s/bios/boot.bin"; cp "$T/moved$s/bios/boot.bin" "$T/moved/$s/bios/"; done
printf 'only-here' > "$T/moved/gbc/bios/mine.bin"; : > "$T/movedgb/moved.txt"; : > "$T/movedgbc/moved.txt"
python3 -c "import json,sys; json.dump({'saves': {sys.argv[2] + '/moved/gb/saves/Pocket (USA).sav': {'rom_id': 1, 'hash': 'abc'}}}, open(sys.argv[1], 'w'))" "$D4/state.json" "$T"
ROMM_SYNC_HOMEBREW="$T/homebrew" ROMM_SYNC_MOUNTS="$MB" ROMM_SYNC_DATA="$D4" ROMM_SYNC_AUTOSTART="$T/etaHEN4" \
  ROMM_SYNC_CATALOG="$T/catalog.json" ROMM_SYNC_APP_DIRS="$T/apps" ROMM_SYNC_FS_ROOTS="$T" \
  "$ROOT/build/host/romm-sync" >>"$D4/daemon.log" 2>&1 &
pid=$!; sleep 3; kill $pid; wait $pid 2>/dev/null || true
got=$(python3 -c "
import json,sys
e = [p for p in json.load(open(sys.argv[1]))['profiles'] if p['id'] == 'ps5cemu'][0]['platforms'][0]
print(e.get('user_dir', '').split(',')[0], e['rom_dir'], e['save_dir'], e['bios_dir'])" "$D4/config.json")
[[ "$got" == "{root}/melonds {user_dir}/games {user_dir}/saves /mine/bios" ]] \
  || fail "the shipped DS folders must follow the preset, a folder the user set must stay: $got"
[[ ! -e "$T/moved/gb" ]] || fail "a system's old folder holding only copies must go: $(find "$T/moved/gb")"
keys=$(python3 -c "import json,sys; print(list(json.load(open(sys.argv[1]))['saves']))" "$D4/state.json")
[[ "$keys" == "['$T/movedgb/saves/Pocket (USA).sav']" ]] || fail "a save's record must follow its system to the new folder: $keys"
[[ -f "$T/moved/gba/bios/boot.bin" && -f "$T/movedgba/bios/boot.bin" ]] || fail "a system whose move was put off must keep both its folders"
[[ "$(cd "$T/moved/gbc" && find . -type f)" == "./bios/mine.bin" && -f "$T/movedgbc/bios/boot.bin" ]] \
  || fail "only the copies may go from a system's old folder, never the folder in use: $(find "$T/moved/gbc" "$T/movedgbc")"

echo "30c. Dolphin's saves: an extra keeping them as the main does gets the main's folders mounted on its own"
GM="$MB/TEST00003"; mkdir -p "$GM/games" "$GM/User/GC" "$GM/User/Wii"
python3 - "$T/catalog.json" "$GM" <<'PY'
import json, sys
cat = json.load(open(sys.argv[1]))
cat.append({"id": "gcmount", "name": "Dolphin M", "detect": [sys.argv[2]],
            "profile": {"root": sys.argv[2], "mount_games": True, "rom_dir": "{root}/games", "save_dir": "{root}/User",
                        "platforms": [{"romm": ["ngc"], "dir": "gamecube", "emulator": "dolphin", "save_layout": "gci", "bundle_dirs": "{root}/User/GC"},
                                      {"romm": ["wii"], "dir": "wii", "emulator": "dolphin", "save_layout": "wii", "bundle_dirs": "{root}/User/Wii"}]}})
json.dump(cat, open(sys.argv[1], "w"))
PY
post /api/setup/emulators "{\"roots\":[\"$HB\",\"$T/psxemu\",\"$T/n64emu\",\"$T/n64b\",\"$T/ps2emu\",\"$T/gcA\",\"$T/gcB\",\"$GM\"],
  \"choices\":{\"psx\":\"$T/psxemu\",\"n64\":\"$T/n64emu\",\"ps2\":\"$T/ps2emu\",\"ngc\":\"$T/gcA\",\"wii\":\"$T/gcA\"},
  \"also\":{\"n64\":[\"$T/n64b\"],\"ngc\":[\"$T/gcB\",\"$GM\"],\"wii\":[\"$T/gcB\",\"$GM\"]}}" >/dev/null || fail "setup with a mountable Dolphin refused"
for _ in $(seq 1 20); do grep -q "^TEST00003|" "$T/data/mounts.txt" 2>/dev/null && break; sleep 0.25; done
mt=$(grep "^TEST00003|" "$T/data/mounts.txt" 2>/dev/null | sort)
[[ "$mt" == "TEST00003|$T/gcA/User/GC|app0/User/GC"$'\n'"TEST00003|$T/gcA/User/Wii|app0/User/Wii" ]] \
  || fail "the extra must get the main Dolphin's save folders mounted (its games stay copies: the main keeps two systems in one folder): $mt"

echo "31. presets from the side channel: taken when signed and newer, refused when tampered with"
publish_presets() { # $1: the version; tamper: the file changes after it's signed
  python3 -c "import json,sys; json.dump({'format': 1, 'version': sys.argv[1], 'console': 'host', 'emulators': []}, open(sys.argv[2], 'w'))" \
    "$1" "$REL/presets-host.json"
  RELEASE_SIGNING_KEY="$(cat "$ROOT/tests/update-test.key")" "$SIGN" sign "$REL/presets-host.json" > "$REL/presets-host.sig" 2>/dev/null
  [[ "${2:-}" == tamper ]] && sed -i 's/"emulators": \[\]/"emulators": [{}]/' "$REL/presets-host.json"
  return 0
}
presets() { curl -sf "$API/api/presets" | python3 -c "import json,sys; print(json.load(sys.stdin)['$1'])"; }
check_presets() {
  post /api/presets/check >/dev/null
  for _ in $(seq 1 40); do [[ "$(presets state)" != checking ]] && return; sleep 0.25; done
}
publish_presets 2099.01.01.1; check_presets
[[ "$(presets version) $(presets source)" == "2099.01.01.1 download" ]] || fail "signed presets must be taken: $(curl -sf "$API/api/presets")"
publish_presets 2099.01.01.2 tamper; check_presets
[[ "$(presets version)" == 2099.01.01.1 && "$(presets error)" == *signature* ]] \
  || fail "tampered presets must be refused, the last good ones kept: $(curl -sf "$API/api/presets")"

echo "32. A conflict settled with RomM's save stays settled, though Mupen64Plus wrote its save as the game started"
B6="$T/bootn64"; M7="$T/bootm64"; mkdir -p "$B6/games" "$B6/saves" "$M7/saves" "$M7/data"
m64b=$(python3 - "$B6/games/Boot Kart (U).z64" "$M7/data/mupen64plus.ini" <<'PY'
import hashlib, sys
z = bytearray(b"\xff" * 0x1000)
z[0:4] = b"\x80\x37\x12\x40"; z[0x20:0x34] = b"BOOT KART".ljust(20)
open(sys.argv[1], "wb").write(z)
md5 = hashlib.md5(bytes(z)).hexdigest().upper()
open(sys.argv[2], "w").write("[%s]\nGoodName=Boot Kart (U)\n" % md5)
print("Boot Kart (U)-%.8s" % md5)
PY
)
curl -skf -X POST "$SV/_admin/rom" -d '{"id":966,"platform_id":6,"name":"Boot Kart","fs_name":"Boot Kart (U).z64"}' >/dev/null
srm_with() { python3 -c "import base64,sys; b=bytearray(b'\xff'*0x48800); b[0:4]=sys.argv[1].encode(); print(base64.b64encode(bytes(b)).decode())" "$1"; }
romm_save() { printf '{"rom_id":966,"slot":"autosave","file_name":"Boot Kart (U).srm","content_b64":"%s"}' "$(srm_with "$1")" > "$T/boot.json"
              curl -skf -X POST "$SV/_admin/save" -d @"$T/boot.json" >/dev/null; }
python3 - "$API" "$B6" "$M7" <<'PY' >/dev/null
import json, sys, urllib.request
api, b6, m7 = sys.argv[1:4]
cfg = json.load(urllib.request.urlopen(api + "/api/config"))
profiles = [dict(p, enabled=False) if any("n64" in e.get("romm", []) for e in p.get("platforms", [])) else p
            for p in cfg["profiles"] if p.get("id") not in ("bootn64", "bootm64")]
plat = [{"romm": ["n64"], "dir": "n64", "save_format": "n64-srm"}]
profiles += [{"id": "bootn64", "name": "Boot-N64", "enabled": True, "root": b6, "rom_dir": "{root}/games",
              "save_dir": "{root}/saves", "save_exts": ".srm", "sync_saves": True, "platforms": plat},
             {"id": "bootm64", "name": "Boot-M64", "enabled": True, "root": m7, "rom_dir": b6 + "/games", "sync_saves": True,
              "save_dir": "{data}/n64/bootm64", "save_exts": ".srm", "n64_split_dir": "{root}/saves",
              "n64_ini": "{root}/data/mupen64plus.ini", "extra": ["n64"], "platforms": plat}]
req = urllib.request.Request(api + "/api/config", json.dumps({"profiles": profiles}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)
PY
romm_save S1AA
syncnow
eep() { head -c 4 "$M7/saves/$m64b.eep" 2>/dev/null; }
[[ "$(head -c 4 "$B6/saves/Boot Kart (U).srm")$(eep)" == S1AAS1AA ]] || fail "RomM's save must reach both N64 emulators: $(head -c 4 "$B6/saves/Boot Kart (U).srm") $(eep)"
# Another device saves; then the game, started in Mupen64Plus, writes its old save again as it boots.
sleep 1.1; romm_save S2BB
sleep 1.1; python3 -c "import sys; open(sys.argv[1], 'wb').write(b'JUNK' + b'\xff' * 0x7fc)" "$M7/saves/$m64b.eep"
syncnow
cpath=$(python3 -c "import json,sys; print(next(iter(json.load(open(sys.argv[1])).get('conflicts', {})), ''))" "$T/data/state.json")
[[ -n "$cpath" ]] || fail "a save changed here and on RomM must be a conflict"
post /api/conflicts/resolve "{\"path\":\"$cpath\",\"keep\":\"server\"}" >/dev/null || fail "resolve failed"
[[ "$(head -c 4 "$B6/saves/Boot Kart (U).srm")$(eep)" == S2BBS2BB ]] || fail "RomM's save, kept, must reach every N64 emulator at once: $(head -c 4 "$B6/saves/Boot Kart (U).srm") $(eep)"
syncnow
[[ "$(last conflicts)" == 0 && "$(last uploaded)" == 0 ]] || fail "the conflict came back: $(last conflicts) conflicts, $(last uploaded) up"
[[ "$(head -c 4 "$B6/saves/Boot Kart (U).srm")$(eep)" == S2BBS2BB ]] || fail "RomM's save must stay: $(head -c 4 "$B6/saves/Boot Kart (U).srm") $(eep)"

echo "PASS"
