#!/usr/bin/env bash
# The Steam Deck build (make steamdeck) with the emulators whose saves need a
# shape of their own, against tests/mock_romm.py: MAME's NVRAM folder per set,
# ScummVM's saves by target, shadPS4's savedata per title id, BigPEmu's EEPROM
# named by the game's hash and gopher64's N64 files named by its SHA-256.
# Each game's save comes down from RomM to where the emulator reads it, and
# one made there goes up in the shape RomM keeps for it.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMDECK_BIN:-$ROOT/build/steamdeck/romm-sync}"
T="$(mktemp -d)"
MOCK_PORT=18809
API_PORT=18810
cleanup() { kill "${DAEMON:-}" "${MOCK:-}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; echo "--- daemon log" >&2; tail -40 "$T/daemon.log" >&2; exit 1; }
API="http://127.0.0.1:$API_PORT"
MURL="http://127.0.0.1:$MOCK_PORT"
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
wait_file() { for _ in $(seq 1 60); do [[ -e "$1" ]] && return 0; sleep 0.5; done; fail "missing: ${1#"$T"/}"; }
syncs() { grep -c "sync done" "$T/daemon.log"; }
sync_wait() { local s; s=$(syncs); post /api/sync >/dev/null; for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && return 0; sleep 0.5; done; }
found() { curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
d=json.load(sys.stdin)
c=[c for c in d if c["name"]==sys.argv[1]]
print(eval(sys.argv[2]) if c else "(no %s; found: %s)" % (sys.argv[1], ", ".join(x["name"] for x in d)))' "$1" "$2"; }
# A game's saves on the mock, newest last: "<name> <emulator> <entry>=<content>,..." (a zip's) or "<name> <emulator> <md5>".
saves() { curl -sf "$MURL/_admin/dump" | python3 -c "
import hashlib,io,json,sys,urllib.request,zipfile
for s in sorted((s for s in json.load(sys.stdin)['saves'] if s['rom_id'] == int(sys.argv[1])), key=lambda s: s['id']):
    raw = urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()
    name = s['file_name'].split(' [')[0] + '.' + s['file_name'].rsplit('.', 1)[-1]
    if raw[:2] == b'PK':
        z = zipfile.ZipFile(io.BytesIO(raw))
        print(name, s['emulator'], ','.join('%s=%s' % (n, z.read(n).decode()) for n in sorted(z.namelist())))
    else:
        print(name, s['emulator'], hashlib.md5(raw).hexdigest())" "$1"; }
zip_b64() { python3 -c "
import base64,io,sys,zipfile
b=io.BytesIO(); z=zipfile.ZipFile(b,'w')
for a in sys.argv[1:]: n,_,c=a.partition('='); z.writestr(n,c)
z.close(); print(base64.b64encode(b.getvalue()).decode())" "$@"; }
b64_of() { python3 -c "import base64,sys; print(base64.b64encode(open(sys.argv[1],'rb').read()).decode())" "$1"; }
rom() { # id, platform id, name, file, its content (a path)
    printf '{"id":%s,"platform_id":%s,"name":"%s","fs_name":"%s","files":[{"id":%s1,"file_name":"%s","content_b64":"%s"}]}' \
        "$1" "$2" "$3" "$4" "$1" "$4" "$(b64_of "$5")" > "$T/rom.json"
    curl -sf -X POST "$MURL/_admin/rom" --data-binary @"$T/rom.json" >/dev/null
}
save() { # rom id, file name, emulator, content (base64)
    printf '{"rom_id":%s,"slot":"autosave","file_name":"%s","emulator":"%s","content_b64":"%s"}' "$1" "$2" "$3" "$4" > "$T/save.json"
    curl -sf -X POST "$MURL/_admin/save" --data-binary @"$T/save.json" >/dev/null
}

# The machine: each emulator installed on its own, games in ~/ROMs.
H="$T/home"; R="$H/ROMs"
NV="$H/.mame/nvram"; SVS="$H/.local/share/scummvm/saves"; SP="$H/.local/share/shadPS4"
BP="$H/Applications/BigPEmu/UserData"; G64="$H/.var/app/io.github.gopher64.gopher64/data/gopher64"
mkdir -p "$NV" "$R/arcade" "$H/.config/scummvm" "$SVS" "$SP/savedata/1" "$BP" "$G64/saves" "$R" "$T/proc" "$T/data" "$T/no-flatpaks" "$T/no-media"
printf 'rompath                   %s/arcade\nnvram_directory           %s\n' "$R" "$NV" > "$H/.mame/mame.ini"
printf '[scummvm]\nsavepath=%s\nversioninfo=2.9.0\n' "$SVS" > "$H/.config/scummvm/scummvm.ini"
: > "$SP/config.toml"; : > "$H/Applications/BigPEmu/bigpemu"
printf 'sky' > "$SVS/sky.000"
echo "{\"web_port\": $API_PORT}" > "$T/data/config.json"

python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" & MOCK=$!
HOME="$H" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 ROMM_SYNC_RDAPP="$T/no-retrodeck" \
    ROMM_SYNC_FLATPAK="$T/no-flatpaks" ROMM_SYNC_MEDIA="$T/no-media" "$BIN" > "$T/daemon.log" 2>&1 & DAEMON=$!
up=0
for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
[[ $up == 1 ]] || fail "the service didn't start"
post /api/pair/start "{\"server_url\":\"$MURL\"}" >/dev/null
paired=0
for _ in $(seq 1 60); do curl -sf "$API/api/status" | grep -q '"paired":true' && { paired=1; break; }; sleep 0.5; done
[[ $paired == 1 ]] || fail "pairing with the mock RomM didn't finish"

echo "1. found, with their saves synced"
for e in MAME ScummVM shadPS4 BigPEmu gopher64; do
    [[ "$(found "$e" 'c[0]["profile"].get("sync_saves")')" == True ]] || fail "$e: $(found "$e" 'c[0]["profile"]')"
done
body=$(curl -sf "$API/api/setup/detect" | python3 -c 'import json,sys; print(json.dumps({"roots":[c["root"] for c in json.load(sys.stdin)]}))')
post /api/setup/emulators "$body" >/dev/null
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
for p in '71,"slug":"arcade","name":"Arcade"' '72,"slug":"scummvm","name":"ScummVM"' '73,"slug":"ps4","name":"PlayStation 4"' \
         '74,"slug":"jaguar","name":"Jaguar"' '75,"slug":"n64","name":"Nintendo 64"'; do
    curl -sf -X POST "$MURL/_admin/platform" -d "{\"id\":$p}" >/dev/null
done

echo "2. MAME: a set's NVRAM folder, as <set>.zip with <set>/ in it"
printf 'sf2-rom' > "$T/sf2.zip"; rom 710 71 "Street Fighter II" sf2.zip "$T/sf2.zip"
save 710 sf2.zip mame "$(zip_b64 sf2/eeprom=server-eeprom sf2/nvram=server-nvram)"
post /api/download '{"rom_id":710,"name":"Street Fighter II"}' >/dev/null
wait_file "$R/arcade/sf2.zip"
for _ in $(seq 1 40); do [[ "$(cat "$NV/sf2/eeprom" 2>/dev/null)" == server-eeprom ]] && break; sleep 0.5; done
[[ "$(cat "$NV/sf2/eeprom")/$(cat "$NV/sf2/nvram")" == server-eeprom/server-nvram ]] || fail "RomM's NVRAM didn't come down: $(find "$NV" -type f)"
sleep 1.1; printf 'played' > "$NV/sf2/eeprom"
sync_wait
[[ "$(saves 710 | tail -1)" == "sf2.zip mame sf2/eeprom=played,sf2/nvram=server-nvram" ]] || fail "MAME's NVRAM up: $(saves 710)"

echo "3. ScummVM: the target's numbered saves from its save folder, as <target>.zip; the target from the game's .scummvm"
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":720,\"platform_id\":72,\"name\":\"Monkey Island 2\",\"fs_name\":\"Monkey Island 2\",\"files\":[
  {\"id\":7201,\"file_name\":\"Monkey Island 2.scummvm\",\"content\":\"scumm:monkey2\\n\"},{\"id\":7202,\"file_name\":\"MONKEY2.000\",\"content\":\"game-data\"}]}" >/dev/null
save 720 monkey2.zip scummvm "$(zip_b64 monkey2.s01=server-slot-1)"
post /api/download '{"rom_id":720,"name":"Monkey Island 2"}' >/dev/null
wait_file "$R/scummvm/Monkey Island 2/MONKEY2.000"
wait_file "$SVS/monkey2.s01"
[[ "$(cat "$SVS/monkey2.s01")" == server-slot-1 && "$(cat "$SVS/sky.000")" == sky ]] || fail "ScummVM's save down: $(ls "$SVS")"
sleep 1.1; printf 'slot-2' > "$SVS/monkey2.s02"
sync_wait
[[ "$(saves 720 | tail -1)" == "monkey2.zip scummvm monkey2.s01=server-slot-1,monkey2.s02=slot-2" ]] || fail "ScummVM's saves up: $(saves 720)"

echo "4. shadPS4: savedata/<title id>/, the title id from the .pkg, as <title id>.zip"
python3 "$ROOT/tests/make_disc.py" ps4pkg "$T/tlou.pkg" UP9000-CUSA00552_00-THELASTOFUS00000
rom 730 73 "The Last of Us" "The Last of Us.pkg" "$T/tlou.pkg"
save 730 CUSA00552.zip shadps4 "$(zip_b64 CUSA00552/SAVEDATA00/data.bin=server-ps4 CUSA00552/SAVEDATA00/sce_sys/param.sfo=sfo)"
post /api/download '{"rom_id":730,"name":"The Last of Us"}' >/dev/null
wait_file "$SP/savedata/1/CUSA00552/SAVEDATA00/data.bin"
[[ "$(cat "$SP/savedata/1/CUSA00552/SAVEDATA00/data.bin")" == server-ps4 ]] || fail "shadPS4's save down"
sleep 1.1; printf 'ps4-local' > "$SP/savedata/1/CUSA00552/SAVEDATA00/data.bin"
sync_wait
[[ "$(saves 730 | tail -1)" == "CUSA00552.zip shadps4 CUSA00552/SAVEDATA00/data.bin=ps4-local,CUSA00552/SAVEDATA00/sce_sys/param.sfo=sfo" ]] \
  || fail "shadPS4's save up: $(saves 730)"

echo "5. BigPEmu: game<FNV-1a, capitals>_eeprom.bigpeep"
printf 'JAGUAR-CART' > "$T/t2k.j64"; rom 740 74 "Tempest 2000" "Tempest 2000.j64" "$T/t2k.j64"
HASH=$(python3 -c "
h=0xcbf29ce484222325
for b in open('$T/t2k.j64','rb').read(): h=((h^b)*0x100000001b3)&0xffffffffffffffff
print('%016X' % h)")
save 740 "Tempest 2000.bigpeep" bigpemu "$(printf 'eeprom-from-server' | base64 -w0)"
post /api/download '{"rom_id":740,"name":"Tempest 2000"}' >/dev/null
wait_file "$BP/game${HASH}_eeprom.bigpeep"
[[ "$(cat "$BP/game${HASH}_eeprom.bigpeep")" == eeprom-from-server ]] || fail "BigPEmu's EEPROM down"
sleep 1.1; printf 'eeprom-local' > "$BP/game${HASH}_eeprom.bigpeep"
sync_wait
[[ "$(saves 740 | tail -1)" == *" bigpemu $(printf 'eeprom-local' | md5sum | cut -d' ' -f1)" ]] || fail "BigPEmu's EEPROM up: $(saves 740)"

echo "6. gopher64: RomM's .srm out to <name>-<SHA-256>.eep and back"
python3 - "$T/mario.z64" <<'EOF'
import sys
rom = bytearray(b"\x80\x37\x12\x40" + bytes(0x3C) + bytes(range(256)) * 4096)
rom[0x20:0x34] = b"SUPER MARIO 64!     "
rom[0x3B:0x3E] = b"NSM"
open(sys.argv[1], "wb").write(rom)
EOF
NAME="SUPER MARIO 64-$(sha256sum "$T/mario.z64" | cut -d' ' -f1 | tr a-f A-F)"
rom 750 75 "Super Mario 64" "Super Mario 64 (USA).z64" "$T/mario.z64"
srm=$(python3 -c "
import base64
b = bytearray(0x48800); b[0:0x200] = b'E' * 0x200
print(base64.b64encode(bytes(b)).decode())")
save 750 "Super Mario 64 (USA).srm" mupen64plus "$srm"
post /api/download '{"rom_id":750,"name":"Super Mario 64"}' >/dev/null
wait_file "$R/n64/Super Mario 64 (USA).z64"
for _ in $(seq 1 6); do [[ -f "$G64/saves/$NAME.eep" ]] && break; sync_wait; done
[[ -f "$G64/saves/$NAME.eep" && "$(head -c 4 "$G64/saves/$NAME.eep")" == EEEE ]] || fail "gopher64's .eep: $(ls "$G64/saves")"
sleep 1.1; python3 -c "open('$G64/saves/$NAME.eep','r+b').write(b'G'*0x200)"
sync_wait; sync_wait
last=$(curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,urllib.request
s=sorted((s for s in json.load(sys.stdin)['saves'] if s['rom_id'] == 750), key=lambda s: s['id'])[-1]
raw=urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()
print(s['file_name'].split(' [')[0], len(raw), raw[:4].decode('latin-1'))")
[[ "$last" == "Super Mario 64 (USA) $((0x48800)) GGGG" ]] || fail "gopher64's save up as a .srm: $last"

echo "PASS"
