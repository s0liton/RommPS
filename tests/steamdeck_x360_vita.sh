#!/usr/bin/env bash
# The Steam Deck build (make steamdeck) with Xbox 360 and PS Vita emulators of
# its own, against tests/mock_romm.py: EmuDeck's Xenia (the Windows build it
# runs through Proton, portable beside its .exe), the Linux Xenia in
# ~/.local/share/Xenia, and Vita3K as EmuDeck sets it up (its pref-path in
# ~/.config/Vita3K/config.yml, a copy of a real Deck's). Checks that each one's
# folders come from its own config, that a 360 save goes to and from the
# player signed in to Xenia, and that a Vita game and save land where Vita3K
# looks for them.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMDECK_BIN:-$ROOT/build/steamdeck/romm-sync}"
T="$(mktemp -d)"
MOCK_PORT=18805
API_PORT=18806
cleanup() { kill "${DAEMON:-}" "${MOCK:-}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; echo "--- daemon log" >&2; tail -40 "$T/daemon.log" >&2; exit 1; }
API="http://127.0.0.1:$API_PORT"
MURL="http://127.0.0.1:$MOCK_PORT"
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
wait_file() { for _ in $(seq 1 60); do [[ -e "$1" ]] && return 0; sleep 0.5; done; fail "missing: ${1#"$T"/}"; }
syncs() { grep -c "sync done" "$T/daemon.log"; }
sync_wait() { local s; s=$(syncs); post /api/sync >/dev/null; for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && return 0; sleep 0.5; done; }
start() { # HOME
    env HOME="$1" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 ROMM_SYNC_RDAPP="$T/no-retrodeck" \
        ROMM_SYNC_FLATPAK="$T/no-flatpaks" ROMM_SYNC_MEDIA="$T/no-media" "$BIN" >> "$T/daemon.log" 2>&1 & DAEMON=$!
    local up=0
    for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
    [[ $up == 1 ]] || fail "the service didn't start for $1"
}
# One candidate's value, by name and a Python expression on it ("c": the candidate).
found() { curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
d=json.load(sys.stdin)
c=[c for c in d if c["name"]==sys.argv[1]]
print(eval(sys.argv[2]) if c else "(no %s; found: %s)" % (sys.argv[1], ", ".join(x["name"] for x in d)))' "$1" "$2"; }
# A save's zip on the mock: "<name>.zip <emulator> <entry>=<content>,..." for each of a game's saves.
units() { curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,io,zipfile,urllib.request
for s in json.load(sys.stdin)['saves']:
    if s['rom_id'] != int(sys.argv[1]): continue
    z = zipfile.ZipFile(io.BytesIO(urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()))
    print(s['file_name'].split(' [')[0] + '.zip', s['emulator'], ','.join('%s=%s' % (n, z.read(n).decode()) for n in sorted(z.namelist())))" "$1"; }
zip_b64() { python3 -c "
import base64,io,sys,zipfile
b=io.BytesIO(); z=zipfile.ZipFile(b,'w')
for a in sys.argv[1:]: n,_,c=a.partition('='); z.writestr(n,c)
z.close(); print(base64.b64encode(b.getvalue()).decode())" "$@"; }
b64_of() { python3 -c "import base64,sys; print(base64.b64encode(open(sys.argv[1],'rb').read()).decode())" "$1"; }
mkdir -p "$T/proc" "$T/data" "$T/no-flatpaks" "$T/no-media"
echo "{\"web_port\": $API_PORT}" > "$T/data/config.json"

# EmuDeck, as on the user's Deck: its Xenia is the Windows build, run by Proton beside
# portable.txt, so its config and content/ are in roms/xbox360; games in roms/xbox360/roms.
E="$T/ed/home"; EM="$T/card/Emulation"; X="$EM/roms/xbox360"
mkdir -p "$E/.config/EmuDeck" "$E/.config/Vita3K" "$E/Applications/Vita3K" "$X/roms/xbla" "$X/content" "$X/patches" \
         "$EM/roms/psvita" "$EM/saves/xenia" "$EM/bios" "$EM/storage/Vita3K/ux0/user/00/savedata" "$EM/storage/Vita3K/ux0/app"
cat > "$E/.config/EmuDeck/settings.sh" <<EOF
emulationPath=$EM
romsPath=$EM/roms
toolsPath=$EM/tools
biosPath=$EM/bios
savesPath=$EM/saves
storagePath=$EM/storage
EOF
: > "$X/xenia_canary.exe"; : > "$X/portable.txt"; : > "$E/Applications/Vita3K/Vita3K"
ln -s "$X/content" "$EM/saves/xenia/saves"
XU=E03000000000AB12; XO=E0300000000000CD
# Xenia Canary's own config: [Profiles] as it writes it (aligned, a comment after each value).
pad() { printf '%-50s\t# %s\n' "$1" "$2"; }
{
    echo '[Content]'
    pad 'license_mask = 0' 'Set license mask for activated content.'
    echo; echo '[Profiles]'
    pad 'logged_profile_slot_0_xuid = ""' 'XUID of the profile to load on boot in slot 0'
    pad "logged_profile_slot_1_xuid = \"$XU\"" 'XUID of the profile to load on boot in slot 1'
    pad "logged_profile_slot_2_xuid = \"$XO\"" 'XUID of the profile to load on boot in slot 2'
    pad 'logged_profile_slot_3_xuid = ""' 'XUID of the profile to load on boot in slot 3'
    echo; echo '[Storage]'
    pad 'cache_root = ""' 'Root path for files used to speed up certain parts of the emulator or the game.'
    pad 'content_root = ""' 'Root path for guest content storage (saves, etc.), or empty to use the content folder under the storage root.'
    pad 'storage_root = ""' 'Root path for persistent internal data storage (config, etc.).'
} > "$X/xenia-canary.config.toml"
mkdir -p "$X/content/$XU/FFFE07D1/00010000/$XU" "$X/content/$XO/4D5307E6/00000001/Other" "$X/content/$XU/4D5307E6/00000002"
printf 'other-player' > "$X/content/$XO/4D5307E6/00000001/Other/x.bin"; printf 'dlc' > "$X/content/$XU/4D5307E6/00000002/dlc.bin"
# Vita3K's config.yml from a Deck, its pref-path pointed here.
sed "s|^pref-path: .*|pref-path: $EM/storage/Vita3K/|" "$ROOT/tests/fixtures/vita3k-config.yml" > "$E/.config/Vita3K/config.yml"

python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" & MOCK=$!
start "$E"
post /api/pair/start "{\"server_url\":\"$MURL\"}" >/dev/null
paired=0
for _ in $(seq 1 60); do curl -sf "$API/api/status" | grep -q '"paired":true' && { paired=1; break; }; sleep 0.5; done
[[ $paired == 1 ]] || fail "pairing with the mock RomM didn't finish"

echo "1. EmuDeck's Xenia and Vita3K found, their folders from their own configs"
[[ "$(found 'Xenia (EmuDeck, Proton)' 'c[0]["profile"]["platforms"][0]["bundle_dirs"]')" == "$X/content/{xuid}" ]] \
  || fail "EmuDeck's Xenia content: $(found 'Xenia (EmuDeck, Proton)' 'c[0]["profile"]')"
[[ "$(found 'Xenia (EmuDeck, Proton)' 'c[0]["dirs"]["xbox360"]["rom_dir"]')" == "$X/roms" ]] \
  || fail "EmuDeck's Xenia games: $(found 'Xenia (EmuDeck, Proton)' 'c[0]["dirs"]')"
[[ "$(found Vita3K 'c[0]["profile"]["root"]')" == "$EM/storage/Vita3K" ]] || fail "Vita3K's pref-path: $(found Vita3K 'c[0]["profile"]["root"]')"
[[ "$(found Vita3K 'c[0]["profile"]["bundle_dirs"]')" == "$EM/storage/Vita3K/ux0/user/00/savedata" ]] \
  || fail "Vita3K's savedata, user-id 00: $(found Vita3K 'c[0]["profile"]["bundle_dirs"]')"
[[ "$(found Vita3K 'c[0]["dirs"]["psvita"]["rom_dir"]')" == "$EM/roms/psvita" ]] || fail "Vita3K's games: $(found Vita3K 'c[0]["dirs"]')"
[[ "$(found Vita3K 'c[0]["profile"]["firmware"]["dir"]')" == "$EM/bios/psvita" ]] || fail "Vita3K's PUPs: $(found Vita3K 'c[0]["profile"]["firmware"]')"
[[ "$(found Xenia 'c')" == "(no Xenia;"* ]] || fail "the Linux Xenia was found without its folder"
body=$(curl -sf "$API/api/setup/detect" | python3 -c 'import json,sys; print(json.dumps({"roots":[c["root"] for c in json.load(sys.stdin)]}))')
post /api/setup/emulators "$body" >/dev/null
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null

echo "2. a 360 save: RomM's comes down to the player in Xenia's lowest signed-in slot, one made there goes up as <title id>.zip"
curl -sf -X POST "$MURL/_admin/platform" -d '{"id":61,"slug":"xbox360","name":"Xbox 360"}' >/dev/null
python3 "$ROOT/tests/make_disc.py" x360iso "$T/gears.iso" 4D5307E6
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":610,\"platform_id\":61,\"name\":\"Gears\",\"fs_name\":\"Gears (USA).iso\",\"files\":[{\"id\":6101,\"file_name\":\"Gears (USA).iso\",\"content_b64\":\"$(b64_of "$T/gears.iso")\"}]}" >/dev/null
curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":610,\"slot\":\"autosave\",\"file_name\":\"4D5307E6.zip\",\"emulator\":\"xenia\",\"content_b64\":\"$(zip_b64 4D5307E6/00000001/SaveGame/data.bin=from-pc 4D5307E6/Headers/00000001/SaveGame.header=hdr-pc)\"}" >/dev/null
post /api/download '{"rom_id":610,"name":"Gears"}' >/dev/null
wait_file "$X/roms/Gears (USA).iso"
[[ ! -d "$X/roms/Gears (USA)" ]] || fail "a 360 game went in a folder of its own: ES-DE wants the file"
S="$X/content/$XU/4D5307E6"
wait_file "$S/00000001/SaveGame/data.bin"
[[ "$(cat "$S/00000001/SaveGame/data.bin")/$(cat "$S/Headers/00000001/SaveGame.header")" == "from-pc/hdr-pc" ]] \
  || fail "RomM's 360 save isn't in slot 1's player's folder: $(find "$X/content" -type f)"
[[ "$(cat "$X/content/$XO/4D5307E6/00000001/Other/x.bin")/$(cat "$S/00000002/dlc.bin")" == "other-player/dlc" ]] \
  || fail "another player's saves or the title's DLC changed"
sleep 1.1; printf 'played-here' > "$S/00000001/SaveGame/data.bin"
sync_wait
up=$(units 610)
[[ "$up" == *"4D5307E6.zip xenia 4D5307E6/00000001/SaveGame/data.bin=played-here,4D5307E6/Headers/00000001/SaveGame.header=hdr-pc"* ]] \
  || fail "the 360 save changed in Xenia didn't go up as 4D5307E6.zip: $up"

echo "3. a Vita game and save, where EmuDeck's Vita3K has them"
curl -sf -X POST "$MURL/_admin/platform" -d '{"id":62,"slug":"psvita","name":"PlayStation Vita"}' >/dev/null
vpk=$(python3 -c "import base64,sys; sys.path.insert(0,sys.argv[1]); import make_disc; print(base64.b64encode(make_disc.vita_zip('PCSE00001')).decode())" "$ROOT/tests")
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":620,\"platform_id\":62,\"name\":\"Vita Quest\",\"fs_name\":\"Vita Quest (USA).vpk\",\"files\":[{\"id\":6201,\"file_name\":\"Vita Quest (USA).vpk\",\"content_b64\":\"$vpk\"}]}" >/dev/null
curl -sf -X POST "$MURL/_admin/save" -d "{\"rom_id\":620,\"slot\":\"autosave\",\"file_name\":\"PCSE00001.zip\",\"emulator\":\"vita3k\",\"content_b64\":\"$(zip_b64 PCSE00001/SAVE.DAT=vita-from-server)\"}" >/dev/null
curl -sf -X POST "$MURL/_admin/firmware" -d '{"platform_id":62,"file_name":"PSVUPDAT.PUP","content":"SCEUF-PSV"}' >/dev/null
post /api/download '{"rom_id":620,"name":"Vita Quest"}' >/dev/null
wait_file "$EM/roms/psvita/Vita Quest (USA).psvita"
[[ "$(cat "$EM/roms/psvita/Vita Quest (USA).psvita")" == PCSE00001 ]] || fail "the .psvita must hold the title id"
wait_file "$EM/storage/Vita3K/ux0/user/00/savedata/PCSE00001/SAVE.DAT"
[[ "$(cat "$EM/storage/Vita3K/ux0/user/00/savedata/PCSE00001/SAVE.DAT")" == vita-from-server ]] || fail "RomM's Vita save didn't come down whole"
wait_file "$EM/bios/psvita/PSVUPDAT.PUP"
for _ in $(seq 1 20); do grep -q "notify: RommDeck: Vita Quest needs installing in Vita3K" "$T/daemon.log" && break; sleep 0.5; done
grep -q "notify: RommDeck: Vita Quest needs installing in Vita3K" "$T/daemon.log" || fail "no install notice for the Vita game"
grep -A1 "Vita3K needs its firmware installed" "$T/daemon.log" | grep -q "then pick $EM/bios/psvita/PSVUPDAT.PUP" \
  || fail "the Vita firmware notice: $(grep -A1 "Vita3K needs its firmware" "$T/daemon.log" | tail -2)"

echo "4. the Linux Xenia: with nobody signed in, its only profile; none at all, and it says to sign in"
kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
L="$T/linux/home"; XL="$L/.local/share/Xenia"; XP=E0300000000000EF
mkdir -p "$XL/content/$XP/FFFE07D1/00010000/$XP" "$XL/content/0000000000000000/4D5307E6/00000002"
{
    echo '[Profiles]'
    for s in 0 1 2 3; do pad "logged_profile_slot_${s}_xuid = \"\"" "XUID of the profile to load on boot in slot $s"; done
    echo; echo '[Storage]'
    pad 'content_root = ""' 'Root path for guest content storage (saves, etc.), or empty to use the content folder under the storage root.'
} > "$XL/xenia-edge.config.toml"
start "$L"
[[ "$(found Xenia 'c[0]["profile"]["platforms"][0]["bundle_dirs"]')" == "$XL/content/{xuid}" ]] \
  || fail "the Linux Xenia's content: $(found Xenia 'c[0]["profile"]')"
[[ "$(found Xenia 'c[0]["dirs"]["xbox360"]["rom_dir"]')" == "$L/ROMs/xbox360" ]] || fail "the Linux Xenia's games: $(found Xenia 'c[0]["dirs"]')"
curl -sf "$API/api/config" | python3 -c '
import json,sys,urllib.request
x=[c["profile"] for c in json.load(urllib.request.urlopen(sys.argv[1] + "/api/setup/detect")) if c["name"] == "Xenia"]
req=urllib.request.Request(sys.argv[1] + "/api/config", json.dumps({"profiles": x}).encode(), {"Content-Type": "application/json"})
urllib.request.urlopen(req)' "$API"
mkdir -p "$L/ROMs/xbox360"; cp "$T/gears.iso" "$L/ROMs/xbox360/Gears (USA).iso"
sync_wait
wait_file "$XL/content/$XP/4D5307E6/00000001/SaveGame/data.bin"
[[ "$(cat "$XL/content/$XP/4D5307E6/00000001/SaveGame/data.bin")" == played-here ]] || fail "the 360 save didn't come down to the only profile"
[[ -d "$XL/content/0000000000000000/4D5307E6/00000002" && ! -e "$XL/content/0000000000000000/4D5307E6/00000001" ]] \
  || fail "the all-zero XUID's folder (DLC) was taken for a player"
rm -rf "$XL/content/$XP/FFFE07D1"
note=$(curl -sf "$API/api/paths" | python3 -c "import json,sys; print([m['sync_note'] for m in json.load(sys.stdin) if m['profile'] == 'Xenia'])")
[[ "$note" == *"sign in to a profile in it"* ]] || fail "with no profile, it must say to sign in: $note"
# content_root set (relative to Xenia's folder, its comment after it as Xenia writes it): the saves move with it.
{ echo '[Storage]'; pad 'content_root = "moved"' 'Root path for guest content storage (saves, etc.), or empty to use the content folder under the storage root.'; } \
    > "$XL/xenia-edge.config.toml"
touch -d '+5 seconds' "$XL/xenia-edge.config.toml"
[[ "$(found Xenia 'c[0]["profile"]["platforms"][0]["bundle_dirs"]')" == "$XL/moved/{xuid}" ]] \
  || fail "Xenia's content_root: $(found Xenia 'c[0]["profile"]["platforms"][0]["bundle_dirs"]')"

echo "PASS"
