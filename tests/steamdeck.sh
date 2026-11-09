#!/usr/bin/env bash
# The Steam Deck build (make steamdeck) against tests/mock_romm.py, on a
# RetroDECK 0.10 laid out the way a Deck has it: retrodeck.json naming each
# folder, games on the SD card and saves in ~/retrodeck, RetroArch's config in
# its Flatpak folder with its cores behind /var/config. Checks that the service
# finds RetroDECK's emulators (RetroDECK set up after the service started, as
# with a card mounted late), downloads games into roms/<system>, puts
# RetroArch's saves in saves/<system>, and syncs when an emulator closes.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMDECK_BIN:-$ROOT/build/steamdeck/romm-sync}"
T="$(mktemp -d)"
MOCK_PORT=18797
API_PORT=18798
cleanup() { kill "${DAEMON:-}" "${MOCK:-}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; echo "--- daemon log" >&2; tail -40 "$T/daemon.log" >&2; exit 1; }
API="http://127.0.0.1:$API_PORT"
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }

# The machine: home, SD card, the Flatpak's folders, /proc.
H="$T/home"; SD="$T/sd/Emulation/retrodeck"; RD="$H/retrodeck"; APP="$H/.var/app/net.retrodeck.retrodeck"
mkdir -p "$SD/roms" "$RD/bios" "$RD/states" "$APP/config/retrodeck" "$APP/config/retroarch/cores" "$APP/data/xemu/xemu" \
         "$T/proc" "$T/data"
cat > "$APP/config/retrodeck/retrodeck.json" <<EOF
{"version": "0.10.10b", "paths": {"rd_home_path": "$SD", "roms_path": "$SD/roms", "saves_path": "$RD/saves",
 "states_path": "$RD/states", "bios_path": "$RD/bios", "downloaded_media_path": "$SD/ES-DE/downloaded_media"}}
EOF
cat > "$APP/config/retroarch/retroarch.cfg" <<EOF
libretro_directory = "/var/config/retroarch/cores"
libretro_info_path = "/var/config/retroarch/cores"
savefile_directory = "$RD/saves"
savestate_directory = "$RD/states"
system_directory = "$RD/bios"
sort_savefiles_by_content_enable = "true"
sort_savestates_by_content_enable = "true"
sort_savefiles_enable = "false"
sort_savestates_enable = "false"
EOF
for c in "snes9x:Nintendo - Super Nintendo Entertainment System:Snes9x" "swanstation:Sony - PlayStation:SwanStation"; do
    IFS=: read -r id db name <<< "$c"
    : > "$APP/config/retroarch/cores/${id}_libretro.so"
    printf 'corename = "%s"\ndatabase = "%s"\n' "$name" "$db" > "$APP/config/retroarch/cores/${id}_libretro.info"
done
echo "{\"web_port\": $API_PORT}" > "$T/data/config.json"

python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" & MOCK=$!
HOME="$H" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 "$BIN" > "$T/daemon.log" 2>&1 & DAEMON=$!
up=0
for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
[[ $up == 1 ]] || fail "the service didn't start"

echo "1. RetroDECK not set up yet: nothing found"
post /api/pair/start "{\"server_url\":\"http://127.0.0.1:$MOCK_PORT\"}" >/dev/null
paired=0
for _ in $(seq 1 60); do curl -sf "$API/api/status" | grep -q '"paired":true' && { paired=1; break; }; sleep 0.5; done
[[ $paired == 1 ]] || fail "pairing with the mock RomM didn't finish"
names() { curl -sf "$API/api/setup/detect" | python3 -c 'import json,sys; print(" ".join(sorted(c["name"].split(" (")[0] for c in json.load(sys.stdin))))'; }
[[ -z "$(names)" ]] || fail "found emulators before RetroDECK's saves folder existed: $(names)"
profile_ids() { curl -sf "$API/api/config" | python3 -c 'import json,sys; print(" ".join(p["id"] for p in json.load(sys.stdin)["profiles"]))'; }
[[ " $(profile_ids) " != *" mednafen "* && " $(profile_ids) " != *" ps2-classics "* ]] \
  || fail "the Deck offers PlayStation-only profiles: $(profile_ids)"

echo "2. set up since: found without a restart"
mkdir -p "$RD/saves/psx/duckstation/memcards" "$RD/saves/ps2/pcsx2/memcards" "$RD/saves/gc/dolphin/US/Card A" \
         "$RD/saves/wii/dolphin" "$RD/saves/PSP/PPSSPP-SA" "$RD/saves/nds/melonds" "$RD/saves/n3ds/azahar" \
         "$RD/saves/wiiu/cemu" "$RD/saves/xbox/xemu" "$RD/saves/ps3/rpcs3"
got="$(names)"
[[ "$got" == "Azahar Cemu Dolphin DuckStation PCSX2 PPSSPP RPCS3 RetroArch melonDS xemu" ]] || fail "detection: $got"

echo "3. setup, downloads into RetroDECK's folders, RetroArch's save in saves/<system>"
body=$(curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
d=json.load(sys.stdin)
duck=[c["root"] for c in d if c["name"].startswith("DuckStation")][0]
print(json.dumps({"roots":[c["root"] for c in d],"choices":{"psx":duck}}))')
post /api/setup/emulators "$body" >/dev/null
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" \
    -d '{"rom_id":11,"slot":"autosave","file_name":"Super Metroid (USA).srm","content":"server-save"}' >/dev/null
for r in '11,"name":"Super Metroid"' '50,"name":"Test Quest"' '20,"name":"Okami"' '40,"name":"Wind Waker"'; do
    post /api/download "{\"rom_id\":$r}" >/dev/null
done
wait_file() { for _ in $(seq 1 60); do [[ -e "$1" ]] && return 0; sleep 0.5; done; fail "missing: ${1#"$T"/}"; }
wait_file "$SD/roms/snes/Super Metroid (USA).sfc"
wait_file "$SD/roms/psx/Test Quest (USA).bin"
wait_file "$SD/roms/ps2/Okami.iso"
wait_file "$SD/roms/gc/Wind Waker (USA).iso"
wait_file "$RD/saves/snes/Super Metroid (USA).srm"
[[ "$(cat "$RD/saves/snes/Super Metroid (USA).srm")" == server-save ]] || fail "RetroArch's save didn't come down"

echo "4. an emulator closing syncs"
mkdir -p "$T/proc/4242"; echo pcsx2-qt > "$T/proc/4242/comm"
for _ in $(seq 1 20); do grep -q "game pcsx2-qt started" "$T/daemon.log" && break; sleep 0.5; done
grep -q "game pcsx2-qt started" "$T/daemon.log" || fail "pcsx2-qt starting wasn't seen"
rm -rf "$T/proc/4242"
for _ in $(seq 1 30); do grep -q "sync (game exited)" "$T/daemon.log" && break; sleep 0.5; done
grep -q "sync (game exited)" "$T/daemon.log" || fail "no sync after pcsx2-qt closed"

echo "5. its notifications, web UI and diagnostics say RommDeck, not RommPS"
grep -q "notify: RommDeck: Super Metroid" "$T/daemon.log" || fail "the download's notification doesn't say RommDeck"
! grep -q "notify: RommPS" "$T/daemon.log" || fail "a notification says RommPS"
page="$(curl -sf "$API/")"
[[ "${page:0:300}" == *'<html data-theme="'*'" data-product="RommDeck" data-store="GitHub"'* ]] || fail "the web UI isn't told it's RommDeck"
[[ "$(curl -sf "$API/video?v=abc")" == *"<title>RommDeck</title>"* ]] || fail "the video page's title isn't RommDeck"
[[ "${page:0:300}" == *' data-here="Deck" data-peers="devices"'* ]] || fail "the web UI isn't told it's on a Deck"
# Diagnostic bundles are named after RommDeck, and ones named the old way go.
mkdir -p "$T/data/diagnostics"; : > "$T/data/diagnostics/rommps-diagnostics-20250101-000000.zip"
curl -sf -D "$T/diag.h" -o "$T/diag.zip" "$API/api/diagnostics" || fail "no diagnostic bundle"
grep -qi 'filename="rommdeck-diagnostics.zip"' "$T/diag.h" || fail "the bundle isn't named after RommDeck: $(grep -i disposition "$T/diag.h")"
saved="$(ls "$T/data/diagnostics")"
[[ "$saved" =~ ^rommdeck-diagnostics-[0-9-]+\.zip$ ]] || fail "the saved bundles should be only RommDeck's, the old rommps- one gone: $saved"

echo "6. RPCS3: a PS3 save comes down into RetroDECK's savedata, one made there goes up; the firmware: asked for once, then RomM's PUP comes down to install"
MURL="http://127.0.0.1:$MOCK_PORT"
curl -sf -X POST "$MURL/_admin/platform" -d '{"id":19,"slug":"ps3","name":"PlayStation 3"}' >/dev/null
curl -sf -X POST "$MURL/_admin/rom" -d '{"id":90,"platform_id":19,"name":"Rally","fs_name":"Rally (USA).iso"}' >/dev/null
mkdir -p "$SD/roms/ps3"; python3 "$ROOT/tests/make_disc.py" ps3 "$SD/roms/ps3/Rally (USA).iso" BLUS30443
b64=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('BLUS30443-SAVE00/SAVE.DAT','ps3-from-server'); z.writestr('BLUS30443-SAVE00/PARAM.SFO','sfo'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":90,\"slot\":\"autosave\",\"file_name\":\"BLUS30443.zip\",\"emulator\":\"rpcs3\",\"content_b64\":\"$b64\"}" >/dev/null
post /api/sync >/dev/null
wait_file "$RD/saves/ps3/rpcs3/BLUS30443-SAVE00/SAVE.DAT"
[[ "$(cat "$RD/saves/ps3/rpcs3/BLUS30443-SAVE00/SAVE.DAT")" == ps3-from-server ]] || fail "RomM's PS3 save didn't come down whole"
sleep 1.1; printf 'ps3-local' > "$RD/saves/ps3/rpcs3/BLUS30443-SAVE00/SAVE.DAT"
post /api/sync >/dev/null
up=""
for _ in $(seq 1 40); do
    up=$(curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,io,zipfile,urllib.request
for s in json.load(sys.stdin)['saves']:
    if s['rom_id'] != 90: continue
    z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()))
    print(s['file_name'].split(' [')[0], z.read('BLUS30443-SAVE00/SAVE.DAT').decode())")
    [[ "$up" == *"ps3-local"* ]] && break; sleep 0.5
done
[[ "$up" == *"BLUS30443"*"ps3-local"* ]] || fail "the PS3 save changed in RPCS3 didn't go up as BLUS30443.zip: $up"
n=$(grep -c "notify: RommDeck: RPCS3 (RetroDECK) needs its firmware installed" "$T/daemon.log")
[[ "$n" == 1 ]] || fail "the missing PS3 firmware must be said once a run: $n times"
grep -A1 "notify: RommDeck: RPCS3 (RetroDECK) needs its firmware installed" "$T/daemon.log" | grep -q "Add PS3UPDAT.PUP to RomM" \
  || fail "with no PUP on RomM, it must say to add one"
# RomM gets the PUP: it comes down to bios/ps3 under its own name, and that's said once, with where it is.
curl -sf -X POST "$MURL/_admin/firmware" -d '{"platform_id":19,"file_name":"PS3UPDAT.PUP","content":"SCEUF-test-pup"}' >/dev/null
post /api/sync >/dev/null
wait_file "$RD/bios/ps3/PS3UPDAT.PUP"
[[ "$(cat "$RD/bios/ps3/PS3UPDAT.PUP")" == SCEUF-test-pup ]] || fail "RomM's PUP didn't come down whole"
for _ in $(seq 1 30); do grep -q "then pick $RD/bios/ps3/PS3UPDAT.PUP" "$T/daemon.log" && break; sleep 0.5; done
grep -q "In RPCS3: File, Install Firmware, then pick $RD/bios/ps3/PS3UPDAT.PUP" "$T/daemon.log" \
  || fail "the notice must say where the PUP is: $(grep -A1 "needs its firmware" "$T/daemon.log" | tail -2)"
syncs() { grep -c "sync done" "$T/daemon.log"; }
s=$(syncs); post /api/sync >/dev/null
for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && break; sleep 0.5; done
[[ "$(grep -c "needs its firmware installed" "$T/daemon.log")" == 2 ]] || fail "the PUP's notice must not repeat every sync"
# Installed: nothing more is said.
mkdir -p "$SD/storage/rpcs3/dev_flash/vsh/etc"; echo "release:04.92" > "$SD/storage/rpcs3/dev_flash/vsh/etc/version.txt"
rm "$RD/bios/ps3/PS3UPDAT.PUP"   # comes down again, as BIOS files do: news, if it weren't installed
s=$(syncs); post /api/sync >/dev/null
for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && break; sleep 0.5; done
[[ -f "$RD/bios/ps3/PS3UPDAT.PUP" ]] || fail "the PUP didn't come down again"
[[ "$(grep -c "needs its firmware installed" "$T/daemon.log")" == 2 ]] || fail "installed firmware was asked for again"

echo "6b. a re-release saving under the original's title id comes down once and goes up; a save that isn't the game's comes down once, not every sync"
# Midnight Club: LA's Complete Edition (BLUS30442) writes BLUS30190MC4_SAV, and once looped on a real Deck all day.
mkdir -p "$RD/saves/ps3/rpcs3/BLUS30190MC4_SAV"; printf 'played-here' > "$RD/saves/ps3/rpcs3/BLUS30190MC4_SAV/SAVE.DAT"
curl -sf -X POST "$MURL/_admin/rom" -d '{"id":91,"platform_id":19,"name":"Rally Complete","fs_name":"Rally Complete (USA).iso"}' >/dev/null
curl -sf -X POST "$MURL/_admin/rom" -d '{"id":92,"platform_id":19,"name":"Rally Odd","fs_name":"Rally Odd (USA).iso"}' >/dev/null
python3 "$ROOT/tests/make_disc.py" ps3 "$SD/roms/ps3/Rally Complete (USA).iso" BLUS30442
python3 "$ROOT/tests/make_disc.py" ps3 "$SD/roms/ps3/Rally Odd (USA).iso" BLUS30444
zip64() { python3 -c "import base64,io,sys,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr(sys.argv[1]+'/SAVE.DAT',sys.argv[2]); z.writestr(sys.argv[1]+'/PARAM.SFO','sfo'); z.close(); print(base64.b64encode(b.getvalue()).decode())" "$@"; }
mc=$(curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":91,\"slot\":\"autosave\",\"file_name\":\"BLUS30442.zip\",\"emulator\":\"rpcs3\",\"content_b64\":\"$(zip64 BLUS30190MC4_SAV mc-from-server)\"}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
odd=$(curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":92,\"slot\":\"autosave\",\"file_name\":\"BLUS30444.zip\",\"emulator\":\"rpcs3\",\"content_b64\":\"$(zip64 SOMETHING_ELSE odd-from-server)\"}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
for _ in 1 2 3 4; do s=$(syncs); post /api/sync >/dev/null; for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && break; sleep 0.5; done; done
[[ "$(cat "$RD/saves/ps3/rpcs3/BLUS30190MC4_SAV/SAVE.DAT")" == mc-from-server ]] || fail "the re-release's save didn't come down"
[[ "$(grep -c "downloaded save $mc into" "$T/daemon.log")" == 1 ]] || fail "the re-release's save must come down once: $(grep -c "downloaded save $mc into" "$T/daemon.log") times"
grep -q "BLUS30442: its saves also go by BLUS30190" "$T/daemon.log" || fail "the original's title id wasn't learned"
bk=$(ls "$T/data/backups/91/"BLUS30442.zip.* 2>/dev/null | head -1)
[[ -n "$bk" ]] && python3 -c "import sys,zipfile; sys.exit(zipfile.ZipFile(sys.argv[1]).read('BLUS30190MC4_SAV/SAVE.DAT') != b'played-here')" "$bk" \
  || fail "what was under the original's id here wasn't backed up first"
[[ "$(grep -c "save $odd came down, but none of its files are BLUS30444's here" "$T/daemon.log")" == 1 ]] \
  || fail "a save that isn't the game's must come down once: $(grep -c "save $odd came down" "$T/daemon.log") times"
sleep 1.1; printf 'mc-local' > "$RD/saves/ps3/rpcs3/BLUS30190MC4_SAV/SAVE.DAT"
post /api/sync >/dev/null
up=""
for _ in $(seq 1 40); do
    up=$(curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,io,zipfile,urllib.request
for s in json.load(sys.stdin)['saves']:
    if s['rom_id'] != 91: continue
    z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()))
    print(s['file_name'].split(' [')[0], z.read('BLUS30190MC4_SAV/SAVE.DAT').decode())")
    [[ "$up" == *"mc-local"* ]] && break; sleep 0.5
done
[[ "$up" == *"BLUS30442"*"mc-local"* ]] || fail "the re-release's save changed here didn't go up as BLUS30442.zip: $up"

echo "6c. Vita3K: a downloaded .vpk gets its .psvita and one install notice; a save comes down into saves/psvita/vita3k/<save folder> and goes up as <save folder>.zip; the PUPs come down with a notice, quiet once installed"
# The game saves under another title's folder (INSTALL_DIR_SAVEDATA), as sequels and other regions do.
curl -sf -X POST "$MURL/_admin/platform" -d '{"id":47,"slug":"psvita","name":"PlayStation Vita"}' >/dev/null
vpk=$(python3 -c "import base64,sys; sys.path.insert(0,sys.argv[1]); import make_disc; print(base64.b64encode(make_disc.vita_zip('PCSE00001','PCSE00002')).decode())" "$ROOT/tests")
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":97,\"platform_id\":47,\"name\":\"Vita Quest\",\"fs_name\":\"Vita Quest (USA).vpk\",\"files\":[{\"id\":971,\"file_name\":\"Vita Quest (USA).vpk\",\"content_b64\":\"$vpk\"}]}" >/dev/null
vz=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('PCSE00002/SAVE.DAT','vita-from-server'); z.writestr('PCSE00002/SlotParam_1.bin','slot'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":97,\"slot\":\"autosave\",\"file_name\":\"PCSE00002.zip\",\"emulator\":\"vita3k\",\"content_b64\":\"$vz\"}" >/dev/null
for pup in PSVUPDAT PSP2UPDAT; do
    curl -sf -X POST "$MURL/_admin/firmware" -d "{\"platform_id\":47,\"file_name\":\"$pup.PUP\",\"content\":\"SCEUF-$pup\"}" >/dev/null
done
# Vita3K set up since: added to the settings the way the app adds an emulator found later.
mkdir -p "$RD/saves/psvita/vita3k"
curl -sf "$API/api/setup/detect" > "$T/detect.json"
curl -sf "$API/api/config" | python3 -c '
import json,sys
cfg=json.load(sys.stdin)
vita=[c["profile"] for c in json.load(open(sys.argv[1])) if c["name"].startswith("Vita3K")]
print(json.dumps({"profiles": cfg["profiles"] + vita}))' "$T/detect.json" > "$T/profiles.json"
post /api/config "$(cat "$T/profiles.json")" >/dev/null
[[ " $(profile_ids) " == *" rd-vita3k "* ]] || fail "Vita3K wasn't found: $(profile_ids)"
post /api/download '{"rom_id":97,"name":"Vita Quest"}' >/dev/null
VITA="$SD/roms/psvita/Vita Quest (USA)"
wait_file "$VITA.vpk"
wait_file "$VITA.psvita"
[[ "$(cat "$VITA.psvita")" == PCSE00001 ]] || fail "the .psvita must hold the title id: $(cat "$VITA.psvita")"
wait_file "$RD/saves/psvita/vita3k/PCSE00002/SAVE.DAT"
[[ "$(cat "$RD/saves/psvita/vita3k/PCSE00002/SAVE.DAT")" == vita-from-server ]] || fail "RomM's Vita save didn't come down whole"
[[ "$(grep -c "notify: RommDeck: Vita Quest needs installing in Vita3K (RetroDECK)" "$T/daemon.log")" == 1 ]] \
  || fail "the install notice must be said once: $(grep -c "needs installing" "$T/daemon.log") times"
grep -A1 "needs installing in Vita3K" "$T/daemon.log" | grep -qF "In Vita3K: File, Install..., then pick $VITA.vpk" \
  || fail "the install notice must say which file: $(grep -A1 "needs installing" "$T/daemon.log" | tail -1)"
wait_file "$RD/bios/psvita/PSVUPDAT.PUP"
wait_file "$RD/bios/psvita/PSP2UPDAT.PUP"
for _ in $(seq 1 30); do grep -q "Vita3K (RetroDECK) needs its firmware installed" "$T/daemon.log" && break; sleep 0.5; done
grep -A1 "Vita3K (RetroDECK) needs its firmware installed" "$T/daemon.log" | grep -q "In Vita3K: File, Install Firmware, then pick $RD/bios/psvita/PS" \
  || fail "the firmware notice must say where a PUP is: $(grep -A1 "Vita3K (RetroDECK) needs its firmware" "$T/daemon.log" | tail -2)"
# Changed in Vita3K: up as RomM's save unit for the game, <save folder>.zip holding <save folder>/.
sleep 1.1; printf 'vita-local' > "$RD/saves/psvita/vita3k/PCSE00002/SAVE.DAT"
post /api/sync >/dev/null
up=""
for _ in $(seq 1 40); do
    up=$(curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,io,zipfile,urllib.request
for s in json.load(sys.stdin)['saves']:
    if s['rom_id'] != 97: continue
    z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()))
    stem, ext = s['file_name'].split(' [')[0], s['file_name'].rsplit('.', 1)[-1]
    print(stem + '.' + ext, s['emulator'], ','.join(sorted(z.namelist())), z.read('PCSE00002/SAVE.DAT').decode())")
    [[ "$up" == *"vita-local"* ]] && break; sleep 0.5
done
[[ "$up" == *"PCSE00002.zip vita3k PCSE00002/SAVE.DAT,PCSE00002/SlotParam_1.bin vita-local"* ]] \
  || fail "the Vita save changed in Vita3K didn't go up as PCSE00002.zip: $up"
# A newer save on RomM holding only other games' folders: tried once, and the save here stays.
oz=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('PCSE00008/A.DAT','a'); z.writestr('PCSE00009/B.DAT','b'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
other=$(curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":97,\"slot\":\"autosave\",\"file_name\":\"PCSE00002.zip\",\"emulator\":\"vita3k\",\"content_b64\":\"$oz\"}" | python3 -c 'import json,sys; print(json.load(sys.stdin)["id"])')
for _ in 1 2 3; do s=$(syncs); post /api/sync >/dev/null; for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && break; sleep 0.5; done; done
[[ "$(grep -c "save $other came down, but none of its files are PCSE00002's here" "$T/daemon.log")" == 1 ]] \
  || fail "other games' Vita folders must be tried once: $(grep -c "save $other came down" "$T/daemon.log") times"
[[ "$(cat "$RD/saves/psvita/vita3k/PCSE00002/SAVE.DAT")" == vita-local && ! -e "$RD/saves/psvita/vita3k/PCSE00002/PCSE00008" ]] \
  || fail "a zip of other games' folders changed this game's save"
# Installed, game and firmware: downloading it again says neither, and the PUPs come down quietly.
mkdir -p "$SD/storage/psvita/Vita3K/ux0/app/PCSE00001/sce_sys" "$SD/storage/psvita/Vita3K/vs0/sys/external"
fw=$(grep -c "Vita3K (RetroDECK) needs its firmware installed" "$T/daemon.log")
rm "$RD/bios/psvita/PSVUPDAT.PUP" "$RD/bios/psvita/PSP2UPDAT.PUP"
post /api/download '{"rom_id":97,"name":"Vita Quest"}' >/dev/null
for _ in $(seq 1 60); do [[ $(grep -c "downloaded Vita Quest to" "$T/daemon.log") -ge 2 ]] && break; sleep 0.5; done
[[ $(grep -c "downloaded Vita Quest to" "$T/daemon.log") -ge 2 ]] || fail "the second download didn't finish"
wait_file "$RD/bios/psvita/PSVUPDAT.PUP"
s=$(syncs); post /api/sync >/dev/null
for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && break; sleep 0.5; done
[[ "$(grep -c "needs installing in Vita3K" "$T/daemon.log")" == 1 ]] || fail "an installed game was said to need installing"
[[ "$(grep -c "Vita3K (RetroDECK) needs its firmware installed" "$T/daemon.log")" == "$fw" ]] || fail "installed Vita firmware was asked for again"

echo "7. settings saved with the PlayStation-only profiles (an older Deck build): they're gone after a restart"
before="$(profile_ids)"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
python3 - "$T/data/config.json" <<'EOF'
import json, sys
c = json.load(open(sys.argv[1]))
c["profiles_custom"] = True
c["profiles"] += [{"id": "mednafen", "name": "Mednafen", "enabled": False, "root": "", "platforms": []},
                  {"id": "ps2-classics", "name": "PS2 ISOs (for PS2 FPKG tools)", "enabled": False,
                   "root": "/mnt/usb0", "platforms": [{"romm": ["ps2"], "emulator": "ps2-classics"}]}]
json.dump(c, open(sys.argv[1], "w"))
EOF
HOME="$H" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 "$BIN" >> "$T/daemon.log" 2>&1 & DAEMON=$!
up=0
for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
[[ $up == 1 ]] || fail "the service didn't start again"
[[ "$(profile_ids)" == "$before" ]] || fail "the old profiles stayed: $(profile_ids) (was $before)"
! grep -q '"mednafen"\|"ps2-classics"' "$T/data/config.json" || fail "config.json still has the old profiles"

# Emulators installed on their own (Flathub, AppImages, EmuDeck's): the service
# again on other homes, each with what that machine would have.
restart() { # HOME, then more VAR=value for the service
    local home="$1"; shift
    kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
    mark=$(wc -l < "$T/daemon.log")
    env HOME="$home" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 ROMM_SYNC_FLATPAK="$T/no-flatpaks" \
        ROMM_SYNC_MEDIA="$T/no-media" "$@" "$BIN" >> "$T/daemon.log" 2>&1 & DAEMON=$!
    local up=0
    for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
    [[ $up == 1 ]] || fail "the service didn't start for $home"
}
since() { tail -n +"$((mark + 1))" "$T/daemon.log"; }
# One candidate's value, by name and a Python expression on it ("c": the candidate).
found() { curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
d=json.load(sys.stdin)
c=[c for c in d if c["name"]==sys.argv[1]]
print(eval(sys.argv[2]) if c else "(no %s; found: %s)" % (sys.argv[1], ", ".join(x["name"] for x in d)))' "$1" "$2"; }

echo "8. EmuDeck alone: settings.sh read the way bash would (~, quotes, //, the last line wins); its standalones' folders from their own configs"
E="$T/ed/home"; EM="$E/Emulation"
mkdir -p "$EM/roms/psx" "$EM/roms/snes" "$EM/saves/duckstation/saves" "$EM/saves/pcsx2/saves" "$EM/saves/retroarch/saves" \
         "$EM/bios" "$EM/storage" "$EM/tools/launchers" "$E/.config/EmuDeck" "$E/Applications" \
         "$E/.local/share/duckstation" "$E/.config/PCSX2/inis" "$E/.var/app/org.libretro.RetroArch/config/retroarch/cores"
cat > "$E/.config/EmuDeck/settings.sh" <<'EOF'
#!/usr/bin/env bash
romsPath=/somewhere/old/roms
emulationPath=~/Emulation
romsPath="$HOME/Emulation//roms"
savesPath='${HOME}/Emulation/saves/'
biosPath=~/Emulation/bios
cloud_sync_status=true
EOF
: > "$E/Applications/DuckStation.AppImage"
printf '[BIOS]\nSearchDirectory = %s\n\n[MemoryCards]\nCard1Type = PerGameTitle\nDirectory = %s\n' "$EM/bios" "$EM/saves/duckstation/saves" \
    > "$E/.local/share/duckstation/settings.ini"
printf '[Folders]\nBios = %s\nMemoryCards = %s\n' "$EM/bios" "$EM/saves/pcsx2/saves" > "$E/.config/PCSX2/inis/PCSX2.ini"
EDRA="$E/.var/app/org.libretro.RetroArch/config/retroarch"
cat > "$EDRA/retroarch.cfg" <<EOF
libretro_directory = "~/.var/app/org.libretro.RetroArch/config/retroarch/cores"
libretro_info_path = "~/.var/app/org.libretro.RetroArch/config/retroarch/cores"
savefile_directory = "$EM/saves/retroarch/saves"
savestate_directory = "$EM/saves/retroarch/states"
system_directory = "$EM/bios"
sort_savefiles_by_content_enable = "false"
sort_savefiles_enable = "false"
sort_savestates_by_content_enable = "false"
sort_savestates_enable = "false"
EOF
: > "$EDRA/cores/snes9x_libretro.so"
printf 'corename = "Snes9x"\ndatabase = "Nintendo - Super Nintendo Entertainment System"\n' > "$EDRA/cores/snes9x_libretro.info"
restart "$E"
since | grep -q "EmuDeck: games in $EM/roms, saves in $EM/saves$" || fail "EmuDeck's folders: $(since | grep EmuDeck)"
since | grep -q "RetroDECK: not found" || fail "RetroDECK was found on a machine without it"
[[ "$(found DuckStation 'c[0]["dirs"]["psx"]["save_dir"]')" == "$EM/saves/duckstation/saves" ]] \
  || fail "DuckStation's cards aren't where its settings.ini says: $(found DuckStation 'c[0]["dirs"]')"
[[ "$(found DuckStation 'c[0]["dirs"]["psx"]["rom_dir"]')" == "$EM/roms/psx" ]] || fail "DuckStation's games aren't in EmuDeck's roms/psx"
[[ "$(found PCSX2 'c[0]["profile"]["pcsx2_cards"]["dir"]')" == "$EM/saves/pcsx2/saves" ]] \
  || fail "PCSX2's cards aren't where PCSX2.ini says: $(found PCSX2 'c[0]["profile"]')"
[[ "$(found RetroArch 'c[0]["profile"]["id"]')" == retroarch-flatpak ]] || fail "Flathub's RetroArch: $(found RetroArch 'c[0]["profile"]["id"]')"
[[ "$(found RetroArch 'c[0]["dirs"]["snes"]["rom_dir"]')" == "$EM/roms/snes" ]] || fail "RetroArch's games aren't in EmuDeck's roms/snes"
[[ "$(grep -c "notify: RommDeck: EmuDeck's CloudSync is on" "$T/daemon.log")" == 1 ]] || fail "EmuDeck's CloudSync being on must be said once"

echo "9. EmuDeck: games into its roms/<system>, RetroArch's saves in its saves/retroarch/saves (not sorted)"
body=$(curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
d=json.load(sys.stdin)
pick=lambda n: [c["root"] for c in d if c["name"]==n][0]
print(json.dumps({"roots":[c["root"] for c in d],"choices":{"psx":pick("DuckStation"),"snes":pick("RetroArch")}}))')
post /api/setup/emulators "$body" >/dev/null
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
curl -sf -X POST "http://127.0.0.1:$MOCK_PORT/_admin/save" \
    -d '{"rom_id":10,"slot":"autosave","file_name":"Chrono Trigger (USA).srm","content":"ed-save"}' >/dev/null
for r in '10,"name":"Chrono Trigger"' '50,"name":"Test Quest"'; do post /api/download "{\"rom_id\":$r}" >/dev/null; done
wait_file "$EM/roms/psx/Test Quest (USA).bin"
wait_file "$EM/roms/snes/Chrono Trigger (USA).sfc"
wait_file "$EM/saves/retroarch/saves/Chrono Trigger (USA).srm"
[[ "$(cat "$EM/saves/retroarch/saves/Chrono Trigger (USA).srm")" == ed-save ]] || fail "RetroArch's save didn't come down whole"
[[ ! -e "$EM/saves/retroarch/saves/snes" ]] || fail "RetroArch's saves were sorted by system, which EmuDeck's RetroArch doesn't do"
restart "$E"
[[ "$(grep -c "notify: RommDeck: EmuDeck's CloudSync is on" "$T/daemon.log")" == 1 ]] || fail "EmuDeck's CloudSync was said again after a restart"

echo "10. Flathub installs with their own folders: Dolphin's NAND and PPSSPP's SAVEDATA in their Flatpaks, RPCS3 installed system-wide, games in ~/ROMs, then where ES-DE says"
P="$T/plain/home"; DOL="$P/.var/app/org.DolphinEmu.dolphin-emu"
mkdir -p "$DOL/data/dolphin-emu/GC" "$DOL/config/dolphin-emu" "$P/.var/app/org.ppsspp.PPSSPP/config/ppsspp/PSP/SAVEDATA" \
         "$T/flatpaks/app/net.rpcs3.RPCS3" "$P/.config/rpcs3" "$P/Applications" "$T/flatpaks/app/org.desmume.DeSmuME/current/active"
# DeSmuME as Flathub has it: no folder of the machine's to see.
printf '[Application]\nname=org.desmume.DeSmuME\n\n[Context]\nshared=ipc;\nfilesystems=xdg-run/pipewire-0;\n' \
    > "$T/flatpaks/app/org.desmume.DeSmuME/current/active/metadata"
printf '[General]\nISOPaths = 0\n' > "$DOL/config/dolphin-emu/Dolphin.ini"
printf '$(EmulatorDir): ""\n/dev_hdd0/: $(EmulatorDir)dev_hdd0/\n/dev_flash/: $(EmulatorDir)dev_flash/\n' > "$P/.config/rpcs3/vfs.yml"
restart "$P" ROMM_SYNC_FLATPAK="$T/flatpaks"
since | grep -q "EmuDeck: not found" || fail "EmuDeck was found on a machine without it"
[[ "$(found Dolphin 'c[0]["profile"]["platforms"][1]["bundle_dirs"]')" == "$DOL/data/dolphin-emu/Wii" ]] \
  || fail "Dolphin's NAND isn't its Flatpak's: $(found Dolphin 'c[0]["profile"]["platforms"]')"
[[ "$(found Dolphin 'c[0]["dirs"]["ngc"]["rom_dir"]')" == "$P/ROMs/gc" ]] || fail "Dolphin's GameCube games aren't in ~/ROMs/gc: $(found Dolphin 'c[0]["dirs"]')"
[[ "$(found PPSSPP 'c[0]["profile"]["bundle_dirs"]')" == "$P/.var/app/org.ppsspp.PPSSPP/config/ppsspp/PSP/SAVEDATA" ]] \
  || fail "PPSSPP's SAVEDATA: $(found PPSSPP 'c[0]["profile"]["bundle_dirs"]')"
[[ "$(found RPCS3 'c[0]["profile"]["bundle_dirs"]')" == "$P/.config/rpcs3/dev_hdd0/home/00000001/savedata" ]] \
  || fail "RPCS3's savedata, from vfs.yml's \$(EmulatorDir): $(found RPCS3 'c[0]["profile"]["bundle_dirs"]')"
[[ "$(found RPCS3 'c[0]["dirs"]["ps3"]["rom_dir"]')" == "$P/ROMs/ps3" ]] || fail "RPCS3's games without a /games/ in vfs.yml"
mkdir -p "$P/ES-DE/settings"
printf '<?xml version="1.0"?>\n<string name="ROMDirectory" value="~/Games/" />\n' > "$P/ES-DE/settings/es_settings.xml"
[[ "$(found Dolphin 'c[0]["dirs"]["ngc"]["rom_dir"]')" == "$P/Games/gc" ]] || fail "games don't follow ES-DE's ROMDirectory: $(found Dolphin 'c[0]["dirs"]["ngc"]')"
# A Flatpak that can't see the games folder keeps its games in its own; allowed since (flatpak override), the games folder.
[[ "$(found DeSmuME 'c[0]["dirs"]["nds"]["rom_dir"]')" == "$P/.var/app/org.desmume.DeSmuME/data/roms/nds" ]] \
  || fail "a Flatpak without access to the games folder: $(found DeSmuME 'c[0]["dirs"]')"
mkdir -p "$P/.local/share/flatpak/overrides"
printf '[Context]\nfilesystems=home;\n' > "$P/.local/share/flatpak/overrides/org.desmume.DeSmuME"
[[ "$(found DeSmuME 'c[0]["dirs"]["nds"]["rom_dir"]')" == "$P/Games/nds" ]] || fail "with an override for home: $(found DeSmuME 'c[0]["dirs"]')"
# Its config saved since: read again.
printf '[General]\nNANDRootPath = /elsewhere/Wii\n' > "$DOL/config/dolphin-emu/Dolphin.ini"
touch -d '+5 seconds' "$DOL/config/dolphin-emu/Dolphin.ini"
[[ "$(found Dolphin 'c[0]["profile"]["platforms"][1]["bundle_dirs"]')" == /elsewhere/Wii ]] || fail "Dolphin.ini changed, its NAND didn't"

echo "11. an AppImage: found by its file; running, it's seen by the file it runs when its process is named otherwise"
: > "$P/Applications/pcsx2-v2.4.0-linux-appimage-x64-Qt.AppImage"
[[ "$(found PCSX2 'c[0]["profile"]["root"]')" == "$P/.config/PCSX2" ]] || fail "an AppImage alone: $(found PCSX2 'c[0]["profile"]["root"]')"
mkdir -p "$T/proc/4343"; echo "MainThread" > "$T/proc/4343/comm"; ln -s /tmp/.mount_pcsx2ab/usr/bin/pcsx2-qt "$T/proc/4343/exe"
for _ in $(seq 1 20); do since | grep -q "game pcsx2-qt started" && break; sleep 0.5; done
since | grep -q "game pcsx2-qt started" || fail "pcsx2-qt by the file it runs wasn't seen"
rm -rf "$T/proc/4343"
mkdir -p "$T/proc/4344"; echo "dolphin" > "$T/proc/4344/comm"; ln -s /usr/bin/dolphin "$T/proc/4344/exe"
sleep 3
! since | grep -q "game dolphin started" || fail "KDE's file manager (dolphin) was taken for an emulator"
rm -rf "$T/proc/4344"

echo "12. RetroDECK and EmuDeck together: two RetroArchs, each with its own games folder; and EmuDeck found on a card without its settings"
restart "$E" ROMM_SYNC_RDAPP="$APP"
since | grep -q "RetroDECK: games in $SD/roms" || fail "RetroDECK wasn't found next to EmuDeck"
ras=$(curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
print(" ".join(sorted("%s=%s:%s" % (c["profile"]["id"], c["name"], c["dirs"]["snes"]["rom_dir"]) for c in json.load(sys.stdin) if c["kind"]=="retroarch")))')
[[ "$ras" == "retroarch-flatpak=RetroArch (Flatpak):$EM/roms/snes retroarch-retroarch=RetroArch:$SD/roms/snes" ]] || fail "the two RetroArchs: $ras"
CARD="$T/media/deck/Emulation"   # the card, labelled Emulation, RetroDECK's on it and roms/ saves/ at its top
mkdir -p "$CARD/retrodeck" "$CARD/roms" "$CARD/saves" "$CARD/Emulation/roms" "$CARD/Emulation/saves" "$CARD/Emulation/tools/launchers" "$T/sdonly/home"
restart "$T/sdonly/home" ROMM_SYNC_MEDIA="$T/media" USER=deck
since | grep -q "EmuDeck: games in $CARD/Emulation/roms, saves in $CARD/Emulation/saves$" \
  || fail "EmuDeck's folder on the card: $(since | grep EmuDeck)"

# Eden as Flathub has it: no folder of the machine's to see. Its user's
# UUID is a real Deck's (Eden made it, "Eden"); a second user after it.
start_on() { # HOME, then more VAR=value for the service, as restart has them
    local home="$1"; shift
    mark=$(wc -l < "$T/daemon.log")
    env HOME="$home" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 ROMM_SYNC_FLATPAK="$T/no-flatpaks" \
        ROMM_SYNC_MEDIA="$T/no-media" "$@" "$BIN" >> "$T/daemon.log" 2>&1 & DAEMON=$!
    local up=0
    for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
    [[ $up == 1 ]] || fail "the service didn't start for $home"
}
stop() { kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true; }
profiles_dat() { # file, then each user's UUID as hex
    python3 - "$@" <<'EOF'
import sys
d = bytearray(0x650)
for i, u in enumerate(sys.argv[2:]):
    off = 0x10 + i * 0xC8
    d[off:off + 16] = d[off + 16:off + 32] = bytes.fromhex(u)
    d[off + 0x28:off + 0x2C] = b"Eden"
open(sys.argv[1], "wb").write(d)
EOF
}
qt_config() { # file, NAND folder, current user, then the game folders Eden lists (none: no list yet)
    local f="$1" nand="$2" user="$3"; shift 3
    { printf '[Data%%20Storage]\nnand_directory\\default=false\nnand_directory=%s\nsave_directory\\default=true\nsave_directory=\n\n' "$nand"
      printf '[System]\ncurrent_user\\default=false\ncurrent_user=%s\n\n[UI]\ntheme=default\n' "$user"
      if (($#)); then
          printf 'Paths\\gamedirs\\size=%d\nPaths\\external_content_dirs\\size=0\n' $#
          local i=0; for g in "$@"; do i=$((i + 1))
              printf 'Paths\\gamedirs\\%d\\path=%s\nPaths\\gamedirs\\%d\\deep_scan\\default=true\nPaths\\gamedirs\\%d\\deep_scan=false\n' $i "$g" $i $i
              printf 'Paths\\gamedirs\\%d\\expanded\\default=true\nPaths\\gamedirs\\%d\\expanded=true\n' $i $i
          done
      fi
      printf '\n[WebService]\nweb_api_url=https://api.ynet-fun.xyz\n'; } > "$f"
}
ini() { python3 - "$@" <<'EOF'
import sys, re
sec, key = sys.argv[2], sys.argv[3]
cur = None
for l in open(sys.argv[1]):
    l = l.rstrip("\n")
    m = re.match(r"\[(.*)\]$", l)
    if m: cur = m.group(1).replace("%20", " "); continue
    if cur == sec and l.split("=", 1)[0].strip() == key: print(l.split("=", 1)[1].strip()); break
EOF
}
UA=37b76f02d1998f500dc7a9a526a1fd29; UB=00112233445566778899aabbccddeeff
FA=29FDA126A5A9C70D508F99D1026FB737; FB=FFEEDDCCBBAA99887766554433221100

echo "13. Eden's Flatpak, no override: Switch games go into its own folder (one RetroDECK's roms/switch had moves there), its folders into qt-config.ini only while it's closed, keys and firmware into the NAND its config names, the save of its current user down and up"
F="$T/eden/home"; EV="$F/.var/app/dev.eden_emu.eden"; R="$EV/data/eden"; NAND="$R/othernand"; QT="$EV/config/eden/qt-config.ini"
mkdir -p "$NAND/system/save/8000000000000010/su/avators" "$EV/config/eden" "$T/fpe/app/dev.eden_emu.eden/current/active" "$SD/roms/switch"
printf '[Application]\nname=dev.eden_emu.eden\n\n[Context]\nfilesystems=xdg-run/app/com.discordapp.Discord:ro;xdg-config/kdeglobals:ro;\n' \
    > "$T/fpe/app/dev.eden_emu.eden/current/active/metadata"
profiles_dat "$NAND/system/save/8000000000000010/su/avators/profiles.dat" $UA $UB
qt_config "$QT" "$NAND" 0 SDMC UserNAND SysNAND
curl -sf -X POST "$MURL/_admin/platform" -d '{"id":30,"slug":"switch","name":"Nintendo Switch"}' >/dev/null
G1="Test Switch [0100ABCD00010000][v0].nsp"; U1="Test Switch [0100ABCD00010800][v65536].nsp"; G2="Other Switch [0100ABCD00020000][v0].nsp"
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":80,\"platform_id\":30,\"name\":\"Test Switch\",\"fs_name\":\"$G1\"}" >/dev/null
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":81,\"platform_id\":30,\"name\":\"Test Switch Update\",\"fs_name\":\"$U1\"}" >/dev/null
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":82,\"platform_id\":30,\"name\":\"Other Switch\",\"fs_name\":\"$G2\"}" >/dev/null
# The settings an earlier RommDeck saved: Eden's games in RetroDECK's roms/switch, where one came down with its update.
stop
printf 'ROM:%s' "$G1" > "$SD/roms/switch/$G1"; printf 'ROM:%s' "$U1" > "$SD/roms/switch/$U1"
python3 - "$T/data" "$R" "$SD/roms/switch" "$G1" "$U1" <<'EOF'
import json, sys
data, root, old, g1, u1 = sys.argv[1:]
c = json.load(open(data + "/config.json"))
c["profiles_custom"] = True
c["profiles"] = [{"id": "eden", "name": "Eden", "enabled": True, "generated": 3, "root": root, "rom_dir": old,
                  "bios_dir": root, "sync_saves": False, "state_exts": "",
                  "firmware": {"by_ext": {".keys": "keys", ".nca": "nand/system/Contents/registered"}},
                  "platforms": [{"romm": ["switch"], "dir": "switch", "emulator": "eden"}]}]
json.dump(c, open(data + "/config.json", "w"))
s = json.load(open(data + "/state.json"))
roms = s.setdefault("roms", {})
roms[old + "/" + g1] = {"rom_id": 80, "profile": "eden", "platform_dir": "switch", "name": "Test Switch", "fs_name": g1, "platform_id": 30}
roms[old + "/" + u1] = {"rom_id": 81, "profile": "eden", "platform_dir": "switch", "name": "Test Switch Update", "fs_name": u1,
                        "platform_id": 30, "with": 80}
json.dump(s, open(data + "/state.json", "w"))
EOF
# Eden is open: its settings are left alone until it closes.
mkdir -p "$T/proc/4545"; echo eden > "$T/proc/4545/comm"
start_on "$F" ROMM_SYNC_RDAPP="$APP" ROMM_SYNC_FLATPAK="$T/fpe"
wait_file "$R/games/$G1"
wait_file "$R/updates/$U1"
[[ ! -e "$SD/roms/switch/$G1" && ! -e "$SD/roms/switch/$U1" ]] || fail "the game and its update were copied, not moved"
[[ "$(cat "$R/games/$G1")" == "ROM:$G1" ]] || fail "the moved game isn't whole"
for _ in $(seq 1 20); do ! grep -q moved_from "$T/data/config.json" && break; sleep 0.5; done
! grep -q moved_from "$T/data/config.json" || fail "the move's note stayed in the settings"
python3 -c "import json,sys; r=json.load(open(sys.argv[1]))['roms']; sys.exit(not (sys.argv[2] in r and sys.argv[3] in r))" \
    "$T/data/state.json" "$R/games/$G1" "$R/updates/$U1" || fail "the moved files' records didn't follow them"
# A sync follows the move, so the moved games' saves come down before anyone plays (BDSP on the Deck:
# the startup sync ran mid-move, and Eden started a fresh game).
for _ in $(seq 1 30); do grep -q "sync (games moved)" "$T/daemon.log" && break; sleep 0.5; done
grep -q "sync (games moved)" "$T/daemon.log" || fail "no sync after the games moved"
sleep 6
[[ "$(ini "$QT" UI 'Paths\gamedirs\size')" == 3 ]] || fail "qt-config.ini was written while Eden ran"
rm -rf "$T/proc/4545"
for _ in $(seq 1 30); do [[ "$(ini "$QT" UI 'Paths\external_content_dirs\size')" == 1 ]] && break; sleep 0.5; done
[[ "$(ini "$QT" UI 'Paths\gamedirs\size')" == 4 && "$(ini "$QT" UI 'Paths\gamedirs\4\path')" == "$R/games" ]] \
  || fail "the games folder isn't Eden's 4th: $(grep gamedirs "$QT")"
[[ "$(ini "$QT" UI 'Paths\gamedirs\4\deep_scan')" == false && "$(ini "$QT" UI 'Paths\gamedirs\1\path')" == SDMC ]] || fail "Eden's own game folders changed"
[[ "$(ini "$QT" UI 'Paths\external_content_dirs\1\path')" == "$R/updates" ]] || fail "the updates folder isn't Eden's External Content: $(grep external "$QT")"
[[ "$(ini "$QT" "Data Storage" nand_directory)" == "$NAND" && "$(ini "$QT" WebService web_api_url)" == https://api.ynet-fun.xyz ]] \
  || fail "the rest of qt-config.ini changed"
sleep 6
[[ "$(ini "$QT" UI 'Paths\gamedirs\size')" == 4 && "$(grep -c 'Paths\\gamedirs\\[0-9]*\\path=' "$QT")" == 4 ]] || fail "the games folder went in twice"
# A game coming down now goes into Eden's own folder.
post /api/download '{"rom_id":82,"name":"Other Switch"}' >/dev/null
wait_file "$R/games/$G2"
# Keys beside its data, firmware in the NAND its config names.
curl -sf -X POST "$MURL/_admin/firmware" -d '{"platform_id":30,"file_name":"prod.keys","content":"header_key = 00"}' >/dev/null
curl -sf -X POST "$MURL/_admin/firmware" -d '{"platform_id":30,"file_name":"0123456789abcdef0123456789abcdef.nca","content":"nca-1"}' >/dev/null
curl -sf -X POST "$MURL/_admin/firmware" -d '{"platform_id":30,"file_name":"fedcba9876543210fedcba9876543210.cnmt.nca","content":"nca-2"}' >/dev/null
# RomM's save of the first game (sigil's yuzu unit: <title id>/...), for Eden's current user.
nx=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('0100ABCD00010000/save.bin','nx-from-server'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":80,\"slot\":\"autosave\",\"file_name\":\"0100ABCD00010000.zip\",\"emulator\":\"eden\",\"content_b64\":\"$nx\"}" >/dev/null
post /api/sync >/dev/null
wait_file "$R/keys/prod.keys"
wait_file "$NAND/system/Contents/registered/0123456789abcdef0123456789abcdef.nca"
wait_file "$NAND/system/Contents/registered/fedcba9876543210fedcba9876543210.cnmt.nca"
[[ ! -e "$R/nand" ]] || fail "firmware went to the default NAND, not the one qt-config.ini names"
SAVES="$NAND/user/save/0000000000000000"
wait_file "$SAVES/$FA/0100ABCD00010000/save.bin"
[[ "$(cat "$SAVES/$FA/0100ABCD00010000/save.bin")" == nx-from-server ]] || fail "RomM's Switch save didn't come down whole"
sleep 1.1; printf 'nx-local' > "$SAVES/$FA/0100ABCD00010000/save.bin"
# The game's device save (all users'), under the all-zero user: the unit's device/<title id>/.
mkdir -p "$SAVES/00000000000000000000000000000000/0100ABCD00010000"; printf 'nx-device' > "$SAVES/00000000000000000000000000000000/0100ABCD00010000/dev.bin"
post /api/sync >/dev/null
up=""
for _ in $(seq 1 40); do
    up=$(curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,io,zipfile,urllib.request
for s in json.load(sys.stdin)['saves']:
    if s['rom_id'] != 80: continue
    z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()))
    print(s['emulator'], ','.join(sorted(z.namelist())), z.read('0100ABCD00010000/save.bin').decode())")
    [[ "$up" == *"nx-local"* ]] && break; sleep 0.5
done
[[ "$up" == *"eden 0100ABCD00010000/save.bin,device/0100ABCD00010000/dev.bin nx-local"* ]] \
  || fail "the Switch save changed in Eden didn't go up as Eden's <title id>/ and device/<title id>/ unit: $up"

echo "14. Eden's current user is the one its settings have open; no empty user folders left in its save tree"
qt_config "$QT.new" "$NAND" 1 SDMC UserNAND SysNAND "$R/games"; mv "$QT.new" "$QT"
nx2=$(python3 -c "import base64,io,zipfile; b=io.BytesIO(); z=zipfile.ZipFile(b,'w'); z.writestr('0100ABCD00020000/save.bin','nx2-from-server'); z.close(); print(base64.b64encode(b.getvalue()).decode())")
curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":82,\"slot\":\"autosave\",\"file_name\":\"0100ABCD00020000.zip\",\"emulator\":\"eden\",\"content_b64\":\"$nx2\"}" >/dev/null
post /api/sync >/dev/null
wait_file "$SAVES/$FB/0100ABCD00020000/save.bin"
[[ ! -e "$SAVES/$FA/0100ABCD00020000" ]] || fail "the save went to Eden's first user too"
[[ -z "$(find "$SAVES" -type d -empty)" ]] || fail "empty folders in Eden's save tree: $(find "$SAVES" -type d -empty)"

echo "15. Eden's Flatpak allowed to see the games folder (flatpak override): its games go there, and the ones in its own folder follow"
stop
mkdir -p "$F/.local/share/flatpak/overrides"
printf '[Context]\nfilesystems=home;\n' > "$F/.local/share/flatpak/overrides/dev.eden_emu.eden"
start_on "$F" ROMM_SYNC_RDAPP="$APP" ROMM_SYNC_FLATPAK="$T/fpe"
[[ "$(found Eden 'c[0]["dirs"]["switch"]["rom_dir"]')" == "$F/ROMs/switch" ]] || fail "with an override: $(found Eden 'c[0]["dirs"]')"
wait_file "$F/ROMs/switch/$G1"
wait_file "$F/ROMs/switch/$G2"
[[ -e "$R/updates/$U1" ]] || fail "the update left Eden's own folder, which it always sees"
for _ in $(seq 1 30); do [[ "$(ini "$QT" UI 'Paths\gamedirs\size')" == 5 ]] && break; sleep 0.5; done
[[ "$(ini "$QT" UI 'Paths\gamedirs\5\path')" == "$F/ROMs/switch" ]] || fail "the games folder Eden sees now isn't in its list: $(grep gamedirs "$QT")"

echo "16. Eden as an AppImage, EmuDeck's: its NAND where qt-config.ini says, games in EmuDeck's roms/switch, seen running by its file; a list Eden never made is made whole; Citron waits for its first start"
stop
A="$T/ededen/home"; AM="$A/Emulation"; ANAND="$AM/storage/eden/nand"
mkdir -p "$AM/roms/switch" "$AM/saves" "$AM/tools/launchers" "$A/.config/EmuDeck" "$A/Applications" "$A/.local/share/eden/keys" \
         "$A/.config/eden" "$ANAND/system/save/8000000000000010/su/avators" "$A/.local/share/citron"
printf 'emulationPath=%s\nromsPath=%s/roms\nsavesPath=%s/saves\nstoragePath=%s/storage\n' "$AM" "$AM" "$AM" "$AM" > "$A/.config/EmuDeck/settings.sh"
: > "$A/Applications/Eden.AppImage"; : > "$A/Applications/citron.AppImage"
profiles_dat "$ANAND/system/save/8000000000000010/su/avators/profiles.dat" $UA
qt_config "$A/.config/eden/qt-config.ini" "$ANAND" 0
start_on "$A"
[[ "$(found 'Eden (AppImage)' 'c[0]["dirs"]["switch"]["rom_dir"]')" == "$AM/roms/switch" ]] || fail "the AppImage's games: $(found 'Eden (AppImage)' 'c[0]["dirs"]')"
[[ "$(found 'Eden (AppImage)' 'c[0]["profile"]["platforms"][0]["bundle_dirs"]')" == "$ANAND/user/save/0000000000000000/{switch_user}" ]] \
  || fail "the AppImage's saves aren't in the NAND its config names: $(found 'Eden (AppImage)' 'c[0]["profile"]["platforms"]')"
[[ "$(found 'Eden (AppImage)' 'c[0]["profile"]["firmware"]["by_ext"][".nca"]')" == "$ANAND/system/Contents/registered" ]] \
  || fail "the AppImage's firmware: $(found 'Eden (AppImage)' 'c[0]["profile"]["firmware"]')"
curl -sf "$API/api/setup/detect" > "$T/detect.json"
python3 -c '
import json,sys
d=json.load(open(sys.argv[1]))
print(json.dumps({"profiles": [c["profile"] for c in d if c["id"] in ("eden-appimage", "citron")]}))' "$T/detect.json" > "$T/profiles.json"
post /api/config "$(cat "$T/profiles.json")" >/dev/null
for _ in $(seq 1 30); do [[ "$(ini "$A/.config/eden/qt-config.ini" UI 'Paths\gamedirs\size')" == 4 ]] && break; sleep 0.5; done
for i in 1:SDMC 2:UserNAND 3:SysNAND "4:$AM/roms/switch"; do
    [[ "$(ini "$A/.config/eden/qt-config.ini" UI "Paths\\gamedirs\\${i%%:*}\\path")" == "${i#*:}" ]] \
      || fail "Eden's list, made whole: $(grep gamedirs "$A/.config/eden/qt-config.ini")"
done
[[ "$(ini "$A/.config/eden/qt-config.ini" UI 'Paths\external_content_dirs\1\path')" == "$A/.local/share/eden/updates" ]] \
  || fail "the AppImage's updates folder: $(grep external "$A/.config/eden/qt-config.ini")"
for _ in $(seq 1 20); do since | grep -q "Citron: no $A/.config/citron/qt-config.ini yet" && break; sleep 0.5; done
since | grep -q "Citron: no $A/.config/citron/qt-config.ini yet" || fail "Citron's missing settings weren't said"
[[ ! -e "$A/.config/citron" ]] || fail "Citron's settings folder was made before its first start"
mkdir -p "$T/proc/4646"; echo AppRun > "$T/proc/4646/comm"; ln -s "$A/Applications/Eden.AppImage" "$T/proc/4646/exe"
for _ in $(seq 1 20); do since | grep -q "game Eden.AppImage started" && break; sleep 0.5; done
since | grep -q "game Eden.AppImage started" || fail "Eden's AppImage running wasn't seen by its file"
rm -rf "$T/proc/4646"

echo "17. other emulators' game lists: DuckStation's RecursivePaths, RMG's ROM browser, Snes9x's and melonDS's folder, each once and only while unset"
stop
X="$T/lists/home"; XF="$T/fpx"
for id in org.duckstation.DuckStation com.github.Rosalie241.RMG com.snes9x.Snes9x net.kuribo64.melonDS; do
    mkdir -p "$XF/app/$id/current/active"; printf '[Application]\nname=%s\n\n[Context]\nfilesystems=home;\n' "$id" > "$XF/app/$id/current/active/metadata"
done
DS="$X/.var/app/org.duckstation.DuckStation/config/duckstation"; RM="$X/.var/app/com.github.Rosalie241.RMG/config/RMG"
S9="$X/.var/app/com.snes9x.Snes9x/config/snes9x"; MD="$X/.var/app/net.kuribo64.melonDS/config/melonDS"
mkdir -p "$DS" "$RM" "$S9" "$MD"
printf '[Main]\nSettingsVersion = 3\n\n[GameList]\nRecursivePaths = /mnt/other\n\n[BIOS]\nSearchDirectory = bios\n' > "$DS/settings.ini"
printf "[Rosalie's Mupen GUI RomBrowser]\nDirectory = \"\"\nRecursive = True\n" > "$RM/mupen64plus.cfg"
printf '[Files]\nLastDirectory = /picked/by/user\nLastShaderDirectory = \n' > "$S9/snes9x.conf"
printf 'RecentROM = []\n\n[Instance0]\nSaveFilePath = ""\n' > "$MD/melonDS.toml"
start_on "$X" ROMM_SYNC_FLATPAK="$XF"
curl -sf "$API/api/setup/detect" > "$T/detect.json"
python3 -c '
import json,sys
d=json.load(open(sys.argv[1]))
print(json.dumps({"profiles": [c["profile"] for c in d if c["id"] in ("duckstation", "rmg", "snes9x", "melonds")]}))' "$T/detect.json" > "$T/profiles.json"
[[ "$(python3 -c 'import json,sys; print(len(json.load(open(sys.argv[1]))["profiles"]))' "$T/profiles.json")" == 4 ]] \
  || fail "the four emulators weren't found: $(python3 -c 'import json,sys; print([c["name"] for c in json.load(open(sys.argv[1]))])' "$T/detect.json")"
post /api/config "$(cat "$T/profiles.json")" >/dev/null
for _ in $(seq 1 30); do grep -q "LastROMFolder" "$MD/melonDS.toml" && grep -q "^Directory = \"/" "$RM/mupen64plus.cfg" && break; sleep 0.5; done
grep -q "^RecursivePaths = /mnt/other$" "$DS/settings.ini" && grep -q "^RecursivePaths = $X/ROMs/psx$" "$DS/settings.ini" \
  || fail "DuckStation's games folder: $(cat "$DS/settings.ini")"
grep -q "^Directory = \"$X/ROMs/n64\"$" "$RM/mupen64plus.cfg" || fail "RMG's ROM browser: $(cat "$RM/mupen64plus.cfg")"
grep -q "^LastDirectory = /picked/by/user$" "$S9/snes9x.conf" || fail "Snes9x's folder the user picked was replaced: $(cat "$S9/snes9x.conf")"
[[ "$(head -2 "$MD/melonDS.toml")" == "RecentROM = []"$'\n'"LastROMFolder = \"$X/ROMs/nds\"" ]] || fail "melonDS's root table: $(cat "$MD/melonDS.toml")"
sleep 6
[[ "$(grep -c RecursivePaths "$DS/settings.ini")" == 2 ]] || fail "DuckStation's games folder went in twice"

echo "18. Ryujinx, EmuDeck's: games in its roms/switch (already in game_dirs), updates and DLC in its patchesAndDlc (into autoload_dirs), keys in system/, each firmware NCA a folder of its own in registered/"
stop
Y="$T/edryu/home"; YM="$Y/Emulation"; YC="$Y/.config/Ryujinx"
mkdir -p "$YM/roms/switch" "$YM/saves" "$YM/storage/ryujinx/patchesAndDlc" "$Y/.config/EmuDeck" "$Y/Applications/publish" "$YC/system"
printf 'emulationPath=%s\nromsPath=%s/roms\nsavesPath=%s/saves\nstoragePath=%s/storage\n' "$YM" "$YM" "$YM" "$YM" > "$Y/.config/EmuDeck/settings.sh"
: > "$Y/Applications/publish/Ryujinx"
printf '{\n  "version": 73,\n  "game_dirs": [\n    "%s/roms/switch"\n  ],\n  "autoload_dirs": [],\n  "show_dirty_hacks": false\n}\n' "$YM" > "$YC/Config.json"
start_on "$Y"
[[ "$(found Ryujinx 'c[0]["dirs"]["switch"]["rom_dir"]')" == "$YM/roms/switch" ]] || fail "Ryujinx's games: $(found Ryujinx 'c[0]["dirs"]')"
curl -sf "$API/api/setup/detect" > "$T/detect.json"
python3 -c '
import json,sys
print(json.dumps({"profiles": [c["profile"] for c in json.load(open(sys.argv[1])) if c["id"] == "ryujinx"]}))' "$T/detect.json" > "$T/profiles.json"
post /api/config "$(cat "$T/profiles.json")" >/dev/null
for _ in $(seq 1 30); do grep -q patchesAndDlc "$YC/Config.json" && break; sleep 0.5; done
python3 - "$YC/Config.json" "$YM" <<'EOF' || fail "Ryujinx's Config.json: $(cat "$YC/Config.json")"
import json, sys
c = json.load(open(sys.argv[1]))
assert c["game_dirs"] == [sys.argv[2] + "/roms/switch"], c["game_dirs"]
assert c["autoload_dirs"] == [sys.argv[2] + "/storage/ryujinx/patchesAndDlc"], c["autoload_dirs"]
assert c["version"] == 73 and c["show_dirty_hacks"] is False
EOF
post /api/sync >/dev/null
wait_file "$YC/system/prod.keys"
REG="$YC/bis/system/Contents/registered"
wait_file "$REG/fedcba9876543210fedcba9876543210.nca/00"
[[ "$(cat "$REG/0123456789abcdef0123456789abcdef.nca/00")" == nca-1 && "$(cat "$REG/fedcba9876543210fedcba9876543210.nca/00")" == nca-2 ]] \
  || fail "Ryujinx's firmware isn't <id>.nca/00: $(find "$YC/bis" -type f)"

# Ryujinx's save index as LibHac writes it: "IMKV", a reserved word, the count,
# then "IMEN", the key's and value's sizes, a SaveDataAttribute (program id,
# user high, user low, static id, type: 1 a user's, 3 the device's) and a
# SaveDataIndexerValue (folder id; space 1, the user's; state).
imkvdb() { # file, then title:user:type:id ...
    python3 - "$@" <<'EOF'
import struct, sys
out, entries = sys.argv[1], []
for e in sys.argv[2:]:
    title, user, typ, sid = e.split(":")
    hi, lo = (int(user[:16], 16), int(user[16:], 16)) if user else (0, 0)
    key = struct.pack("<QQQQBBH", int(title, 16), hi, lo, 0, int(typ), 0, 0).ljust(0x40, b"\0")
    val = struct.pack("<QQQBB", int(sid, 16), 0x4000, 0, 1, 0).ljust(0x40, b"\0")
    entries.append(b"IMEN" + struct.pack("<II", len(key), len(val)) + key + val)
open(out, "wb").write(b"IMKV" + struct.pack("<II", 0, len(entries)) + b"".join(entries))
EOF
}
RU1=00000000000000010000000000000000; RU2=8a9b2c3d4e5f60718293a4b5c6d7e8f9

echo "19. Ryujinx's saves: the open user's (last_opened), placed where the index says, RomM's Eden save taken as it is, the user's and the device's up as one unit"
cat > "$YC/system/Profiles.json" <<EOF
{
  "profiles": [
    {
      "user_id": "$RU1",
      "name": "RyuPlayer",
      "account_state": "Closed",
      "online_play_state": "Closed",
      "last_modified_timestamp": 1730000000,
      "image": "/9j/4AAQ"
    },
    {
      "user_id": "$RU2",
      "name": "Second",
      "account_state": "Open",
      "online_play_state": "Closed",
      "last_modified_timestamp": 1730000001,
      "image": "/9j/4AAQ"
    }
  ],
  "last_opened": "$RU2"
}
EOF
RS="$YC/bis/user/save"
mkdir -p "$YC/bis/system/save/8000000000000000/0" "$RS/0000000000000010/0" "$RS/0000000000000011/0" "$RS/0000000000000012/0"
printf 'first-user' > "$RS/0000000000000012/0/save.bin"
imkvdb "$YC/bis/system/save/8000000000000000/0/imkvdb.arc" 0100ABCD00010000:$RU1:1:12 0100ABCD00010000:$RU2:1:10 0100ABCD00010000::3:11
post /api/download '{"rom_id":80,"name":"Test Switch"}' >/dev/null
wait_file "$YM/roms/switch/$G1"
post /api/sync >/dev/null
wait_file "$RS/0000000000000010/0/save.bin"
[[ "$(cat "$RS/0000000000000010/0/save.bin")" == nx-local && "$(cat "$RS/0000000000000011/0/dev.bin")" == nx-device ]] \
  || fail "RomM's save didn't come down into the open user's and the device's folders: $(find "$RS" -type f)"
[[ "$(cat "$RS/0000000000000012/0/save.bin")" == first-user ]] || fail "the other user's save changed"
sleep 1.1; printf 'ryu-local' > "$RS/0000000000000010/0/save.bin"
post /api/sync >/dev/null
up=""
for _ in $(seq 1 40); do
    up=$(curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,io,zipfile,urllib.request
for s in json.load(sys.stdin)['saves']:
    if s['rom_id'] != 80 or s['emulator'] != 'ryujinx': continue
    z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()))
    print(s['file_name'].split(' [')[0], ','.join(sorted(z.namelist())), z.read('0100ABCD00010000/save.bin').decode())")
    [[ "$up" == *"ryu-local"* ]] && break; sleep 0.5
done
[[ "$up" == *"0100ABCD00010000 0100ABCD00010000/save.bin,device/0100ABCD00010000/dev.bin ryu-local"* || "$up" == *"0100ABCD00010000.zip 0100ABCD00010000/save.bin,device/0100ABCD00010000/dev.bin ryu-local"* ]] \
  || fail "Ryujinx's save didn't go up as the <title id>/ and device/<title id>/ unit: $up"

echo "20. a game Ryujinx never ran: RomM's save waits, said once; once Ryujinx has made its folder, it comes down"
post /api/download '{"rom_id":82,"name":"Other Switch"}' >/dev/null
wait_file "$YM/roms/switch/$G2"
for _ in 1 2; do s=$(syncs); post /api/sync >/dev/null; for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && break; sleep 0.5; done; done
[[ "$(grep -c "notify: RommDeck: start Other Switch once in Ryujinx" "$T/daemon.log")" == 1 ]] \
  || fail "the wait must be said once: $(grep -c "start Other Switch once" "$T/daemon.log") times"
[[ "$(find "$RS" -mindepth 1 -maxdepth 1 | wc -l)" == 3 ]] || fail "a save folder was made for a game Ryujinx never ran: $(ls "$RS")"
# Ryujinx ran it: its folder and its index entry.
mkdir -p "$RS/0000000000000013/0"
imkvdb "$YC/bis/system/save/8000000000000000/0/imkvdb.arc" 0100ABCD00010000:$RU1:1:12 0100ABCD00010000:$RU2:1:10 0100ABCD00010000::3:11 \
    0100ABCD00020000:$RU2:1:13
post /api/sync >/dev/null
wait_file "$RS/0000000000000013/0/save.bin"
[[ "$(cat "$RS/0000000000000013/0/save.bin")" == nx2-from-server ]] || fail "the held save didn't come down whole"

echo "PASS"
