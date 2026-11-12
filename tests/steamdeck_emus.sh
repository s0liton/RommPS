#!/usr/bin/env bash
# The Steam Deck build (make steamdeck) with the emulators EmuDeck installs on
# their own (Flatpaks, AppImages, its Windows Model 2 through Proton), against
# tests/mock_romm.py. Their configs are copies of a real Deck's
# (tests/fixtures/deck-emudeck: @HOME@ and @CARD@ stand for its home folder
# and SD card), with EmuDeck's Flatpak overrides as it writes them. Checks that
# every entry's folders come out of those configs, and that the saves of the
# arcade boards (Supermodel's .nv, Model 2's .DAT) go up to RomM.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMDECK_BIN:-$ROOT/build/steamdeck/romm-sync}"
T="$(mktemp -d)"
MOCK_PORT=18807
API_PORT=18808
cleanup() { kill "${DAEMON:-}" "${MOCK:-}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; echo "--- daemon log" >&2; tail -40 "$T/daemon.log" >&2; exit 1; }
API="http://127.0.0.1:$API_PORT"
MURL="http://127.0.0.1:$MOCK_PORT"
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
wait_file() { for _ in $(seq 1 60); do [[ -e "$1" ]] && return 0; sleep 0.5; done; fail "missing: ${1#"$T"/}"; }
# One candidate's value, by name and a Python expression on it ("c": the candidate).
found() { curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
d=json.load(sys.stdin)
c=[c for c in d if c["name"]==sys.argv[1]]
print(eval(sys.argv[2]) if c else "(no %s; found: %s)" % (sys.argv[1], ", ".join(x["name"] for x in d)))' "$1" "$2"; }
is() { # name, expression, expected
    local got; got="$(found "$1" "$2")"
    [[ "$got" == "$3" ]] || fail "$1: $2 is $got, not $3"
}

# The machine: the user's home and SD card, EmuDeck's Emulation folder on it.
H="$T/home"; CARD="$T/media/deck/Emulation"; EM="$CARD/Emulation"
FX="$ROOT/tests/fixtures/deck-emudeck/home"
(cd "$FX" && find . -type f) | while read -r f; do
    mkdir -p "$(dirname "$H/$f")"
    sed -e "s|@HOME@|$H|g" -e "s|@CARD@|$CARD|g" "$FX/$f" > "$H/$f"
done
for d in psx ps2 gc wii primehacks psp ps3 wiiu/roms wiiu/mlc01 xbox nds n3ds dreamcast gba gb gbc arcade scummvm n64 snes model3 \
         model2/roms model2/NVDATA atarijaguar ps4; do mkdir -p "$EM/roms/$d"; done
mkdir -p "$EM/saves/duckstation/saves" "$EM/saves/pcsx2/saves" "$EM/saves/melonds/saves" "$EM/saves/mgba/saves" "$EM/saves/mame/saves" \
         "$EM/saves/scummvm/saves" "$EM/saves/RMG/saves" "$EM/bios" "$EM/storage/xemu" "$EM/storage/azahar/sdmc" \
         "$EM/storage/rpcs3/dev_hdd0" "$EM/tools/launchers" "$H/Applications/BigPEmu/UserData" "$H/.supermodel/NVRAM" \
         "$T/proc" "$T/data" "$T/no-retrodeck" "$T/system-flatpaks" "$T/no-media"
for a in DuckStation pcsx2-Qt rpcs3 Cemu azahar mGBA Shadps4-qt; do : > "$H/Applications/$a.AppImage"; done
: > "$H/Applications/BigPEmu/bigpemu"
: > "$EM/roms/model2/emulator_multicpu.exe"
printf 'NVRAM:scud' > "$H/.supermodel/NVRAM/scud.nv"
printf 'NVDATA:daytona' > "$EM/roms/model2/NVDATA/daytona.DAT"
# User Flatpaks with their Flathub permissions, and the overrides EmuDeck gives them.
flatpak() { # id, filesystems= of its manifest, override (or "")
    local m="$H/.local/share/flatpak/app/$1/current/active"
    mkdir -p "$m" "$H/.local/share/flatpak/overrides"
    printf '[Application]\nname=%s\n\n[Context]\nshared=ipc;network;\nfilesystems=%s\n' "$1" "$2" > "$m/metadata"
    [[ -z "$3" ]] || printf '[Context]\nfilesystems=%s\nshared=network;\n' "$3" > "$H/.local/share/flatpak/overrides/$1"
}
flatpak org.DolphinEmu.dolphin-emu "host:ro;xdg-run/gamescope-0:ro;" "host;"
flatpak io.github.shiiion.primehack "host:ro;" "host;"
flatpak org.ppsspp.PPSSPP "host:ro;" "host;"
flatpak app.xemu.xemu "host:ro;" "$EM/storage/xemu;host;"
flatpak net.kuribo64.melonDS "home;" "host;"
flatpak org.flycast.Flycast "host:ro;" "host;"
flatpak org.mamedev.MAME "home;host:ro;" "host;"
flatpak org.scummvm.ScummVM "home;" "host;"
flatpak com.github.Rosalie241.RMG "xdg-pictures/RMG;" "host;"
flatpak com.supermodel3.Supermodel "home;" "host;"
RMGINI="$H/.local/share/flatpak/app/com.github.Rosalie241.RMG/current/active/files/share/RMG/mupen64plus.ini"
mkdir -p "$(dirname "$RMGINI")"; : > "$RMGINI"
echo "{\"web_port\": $API_PORT}" > "$T/data/config.json"

python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" & MOCK=$!
HOME="$H" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 ROMM_SYNC_RDAPP="$T/no-retrodeck" \
    ROMM_SYNC_FLATPAK="$T/system-flatpaks" ROMM_SYNC_MEDIA="$T/no-media" "$BIN" > "$T/daemon.log" 2>&1 & DAEMON=$!
up=0
for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
[[ $up == 1 ]] || fail "the service didn't start"
post /api/pair/start "{\"server_url\":\"$MURL\"}" >/dev/null
paired=0
for _ in $(seq 1 60); do curl -sf "$API/api/status" | grep -q '"paired":true' && { paired=1; break; }; sleep 0.5; done
[[ $paired == 1 ]] || fail "pairing with the mock RomM didn't finish"

echo "1. every emulator on the Deck found, its folders out of its own config"
grep -q "EmuDeck: games in $EM/roms, saves in $EM/saves$" "$T/daemon.log" || fail "EmuDeck's folders: $(grep EmuDeck "$T/daemon.log")"
names=$(curl -sf "$API/api/setup/detect" | python3 -c 'import json,sys; print(", ".join(sorted(c["name"] for c in json.load(sys.stdin))))')
for n in DuckStation PCSX2 Dolphin PrimeHack PPSSPP RPCS3 Cemu xemu melonDS Azahar Flycast mGBA MAME ScummVM \
         "RMG (Rosalie's Mupen GUI)" Supermodel "Model 2 Emulator" BigPEmu shadPS4; do
    [[ ", $names, " == *", $n, "* ]] || fail "$n wasn't found: $names"
done
is DuckStation 'c[0]["dirs"]["psx"]["save_dir"]' "$EM/saves/duckstation/saves"
is DuckStation 'c[0]["dirs"]["psx"]["rom_dir"]' "$EM/roms/psx"
is DuckStation 'c[0]["profile"]["bios_dir"]' "$EM/bios"
is PCSX2 'c[0]["profile"]["pcsx2_cards"]["dir"]' "$EM/saves/pcsx2/saves"
is PCSX2 'c[0]["profile"]["bios_dir"]' "$EM/bios"
is Dolphin 'c[0]["dirs"]["ngc"]["rom_dir"]' "$EM/roms/gc"
is Dolphin 'c[0]["profile"]["platforms"][0]["bundle_dirs"]' "$H/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu/GC"
is Dolphin 'c[0]["profile"]["platforms"][1]["bundle_dirs"]' "$H/.var/app/org.DolphinEmu.dolphin-emu/data/dolphin-emu/Wii"
is PrimeHack 'c[0]["dirs"]["wii"]["rom_dir"]' "$EM/roms/primehacks"
is PPSSPP 'c[0]["profile"]["bundle_dirs"]' "$H/.var/app/org.ppsspp.PPSSPP/config/ppsspp/PSP/SAVEDATA"
is RPCS3 'c[0]["profile"]["bundle_dirs"]' "$EM/storage/rpcs3/dev_hdd0/home/00000001/savedata"
is RPCS3 'c[0]["dirs"]["ps3"]["rom_dir"]' "$EM/roms/ps3"
is RPCS3 'c[0]["profile"]["firmware"]["installed"]' "$H/.config/rpcs3/dev_flash/vsh/etc/version.txt"
# Cemu: EmuDeck's settings.xml has no mlc_path (Cemu's own) and a game folder of another machine's.
is Cemu 'c[0]["profile"]["platforms"][0]["bundle_dirs"]' "$H/.local/share/Cemu/mlc01/usr/save"
is Cemu 'c[0]["dirs"]["wiiu"]["rom_dir"]' "$EM/roms/wiiu/roms"
is xemu 'c[0]["profile"]["root"]' "$H/.var/app/app.xemu.xemu/data/xemu/xemu"
is xemu 'c[0]["profile"]["bios_dir"]' "$EM/bios"
is melonDS 'c[0]["dirs"]["nds"]["save_dir"]' "$EM/saves/melonds/saves"
is melonDS 'c[0]["profile"]["bios_dir"]' "$EM/bios"
is Azahar 'c[0]["profile"]["platforms"][0]["bundle_dirs"]' \
    "$EM/storage/azahar/sdmc/Nintendo 3DS/00000000000000000000000000000000/00000000000000000000000000000000/title"
is Flycast 'c[0]["dirs"]["dc"]["rom_dir"]' "$EM/roms/dreamcast"
is Flycast 'c[0]["dirs"]["dc"]["save_dir"]' "$H/.var/app/org.flycast.Flycast/data/flycast"
is mGBA 'c[0]["dirs"]["gba"]["save_dir"]' "$EM/saves/mgba/saves"
is MAME 'c[0]["dirs"]["arcade"]["rom_dir"]' "$EM/roms/arcade"
is MAME 'c[0]["dirs"]["arcade"]["save_dir"]' "$EM/saves/mame/saves"
is ScummVM 'c[0]["dirs"]["scummvm"]["save_dir"]' "$EM/saves/scummvm/saves"
is "RMG (Rosalie's Mupen GUI)" 'c[0]["profile"]["n64_split_dir"]' "$EM/saves/RMG/saves"
is "RMG (Rosalie's Mupen GUI)" 'c[0]["profile"]["n64_ini"]' "$RMGINI"
is "RMG (Rosalie's Mupen GUI)" 'c[0]["dirs"]["n64"]["rom_dir"]' "$EM/roms/n64"
is Supermodel 'c[0]["dirs"]["model3"]["save_dir"]' "$H/.supermodel/NVRAM"
is Supermodel 'c[0]["dirs"]["model3"]["rom_dir"]' "$EM/roms/model3"
is "Model 2 Emulator" 'c[0]["dirs"]["model2"]["rom_dir"]' "$EM/roms/model2/roms"
is "Model 2 Emulator" 'c[0]["dirs"]["model2"]["save_dir"]' "$EM/roms/model2/NVDATA"
is BigPEmu 'c[0]["dirs"]["jaguar"]["rom_dir"]' "$EM/roms/atarijaguar"
is shadPS4 'c[0]["dirs"]["ps4"]["rom_dir"]' "$EM/roms/ps4"

echo "2. the arcade boards: games into EmuDeck's folders, their NVRAM up to RomM"
curl -sf -X POST "$MURL/_admin/platform" -d '{"id":31,"slug":"model3","name":"Sega Model 3"}' >/dev/null
curl -sf -X POST "$MURL/_admin/platform" -d '{"id":32,"slug":"model2","name":"Sega Model 2"}' >/dev/null
curl -sf -X POST "$MURL/_admin/rom" -d '{"id":310,"platform_id":31,"name":"Scud Race","fs_name":"scud.zip"}' >/dev/null
curl -sf -X POST "$MURL/_admin/rom" -d '{"id":320,"platform_id":32,"name":"Daytona USA","fs_name":"daytona.zip"}' >/dev/null
body=$(curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
print(json.dumps({"roots":[c["root"] for c in json.load(sys.stdin)]}))')
post /api/setup/emulators "$body" >/dev/null
post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
post /api/download '{"rom_id":310,"name":"Scud Race"}' >/dev/null
post /api/download '{"rom_id":320,"name":"Daytona USA"}' >/dev/null
wait_file "$EM/roms/model3/scud.zip"
wait_file "$EM/roms/model2/roms/daytona.zip"
up=""
for _ in $(seq 1 40); do
    up=$(curl -sf "$MURL/_admin/dump" | python3 -c '
import json,sys
print(" ".join(sorted("%s%s:%s" % (s["file_name"].split(" [")[0], "." + s["file_name"].rsplit(".", 1)[1] if "." in s["file_name"] else "",
                                    s.get("emulator")) for s in json.load(sys.stdin)["saves"] if s["rom_id"] in (310, 320))))')
    [[ "$up" == *daytona*scud* ]] && break
    post /api/sync >/dev/null; sleep 1
done
[[ "$up" == "daytona.DAT:model2 scud.nv:supermodel" ]] || fail "the boards' NVRAM didn't go up: $up"

echo "3. installed later, configured as their sources write it: ares (a folder per system under its Saves), Snes9x, bsnes, DeSmuME"
mkdir -p "$H/.local/share/ares" "$H/.config/snes9x" "$H/.config/bsnes" "$H/.config/desmume"
printf 'Paths\n  Home\n  Saves: %s/saves/ares/\n  Screenshots: %s/storage/ares/screenshots/\n' "$EM" "$EM" > "$H/.local/share/ares/settings.bml"
printf '[Files]\nSRAMDirectory = %s/saves/snes9x\n' "$EM" > "$H/.config/snes9x/snes9x.conf"
printf 'Path\n  Games: %s/roms/snes/\n  Saves:\n' "$EM" > "$H/.config/bsnes/settings.bml"
is ares 'c[0]["dirs"]["snes"]["save_dir"]' "$EM/saves/ares/Super Famicom"
is ares 'c[0]["dirs"]["gbc"]["save_dir"]' "$EM/saves/ares/Game Boy"
is ares 'c[0]["dirs"]["snes"]["rom_dir"]' "$EM/roms/snes"
is Snes9x 'c[0]["dirs"]["snes"]["save_dir"]' "$EM/saves/snes9x"
is bsnes 'c[0]["dirs"]["snes"]["save_dir"]' "$EM/roms/snes"   # no Saves folder: beside the game
is DeSmuME 'c[0]["dirs"]["nds"]["save_dir"]' "$H/.config/desmume"

echo "PASS"
