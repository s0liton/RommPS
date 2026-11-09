#!/usr/bin/env bash
# Disc ids from test images: PS1 serials, PS2 serials and PCSX2's CRC (make unit).
set -euo pipefail
CLI=$1
ROOT=$(cd "$(dirname "$0")/.." && pwd)
T=$(mktemp -d)
trap 'rm -rf "$T"' EXIT
fail() { echo "gameid: $*" >&2; exit 1; }
expect() { local got; got=$("$CLI" "$1"); [[ "$got" == "$2" ]] || fail "$(basename "$1"): got '$got', want '$2'"; }

python3 "$ROOT/tests/make_disc.py" psx "$T/ps1.iso" SLUS_123.45
expect "$T/ps1.iso" "SLUS-12345 -"
python3 "$ROOT/tests/make_disc.py" psxbin "$T/ps1.bin" SCUS_941.94
expect "$T/ps1.bin" "SCUS-94194 -"
crc=$(python3 "$ROOT/tests/make_disc.py" ps2 "$T/ps2.iso" SLUS_205.52)
expect "$T/ps2.iso" "SLUS-20552 $crc"
crc=$(python3 "$ROOT/tests/make_disc.py" ps2 "$T/sub.iso" SLES_501.23 DATA)
expect "$T/sub.iso" "SLES-50123 $crc"
python3 "$ROOT/tests/make_disc.py" ps3 "$T/ps3.iso" BLUS30443
[[ $("$CLI" ps3 "$T/ps3.iso") == BLUS30443 ]] || fail "ps3 iso: no BLUS30443"
cp "$T/ps3.iso" "$T/Named (USA)iso"   # a file RomM has with no extension
[[ $("$CLI" ps3 "$T/Named (USA)iso") == BLUS30443 ]] || fail "ps3 disc without an extension: no BLUS30443"
mkdir -p "$T/ps3game.ps3/PS3_GAME" && python3 -c "
import sys; sys.path.insert(0, sys.argv[1]); import make_disc
open(sys.argv[2], 'wb').write(make_disc.sfo({'TITLE_ID': 'NPUB30024', 'TITLE': 'Folder'}))" "$ROOT/tests" "$T/ps3game.ps3/PS3_GAME/PARAM.SFO"
[[ $("$CLI" ps3 "$T/ps3game.ps3") == NPUB30024 ]] || fail "ps3 game folder: no NPUB30024"
[[ $("$CLI" ps3 "$T/ps1.bin") == - ]] || fail "a PS1 disc read as a PS3 one"
python3 "$ROOT/tests/make_disc.py" xiso "$T/halo.xiso.iso" 4D530004
[[ $("$CLI" xbox "$T/halo.xiso.iso") == 4D530004 ]] || fail "xiso: no 4D530004"
[[ $("$CLI" xbox-short "$T/halo.xiso.iso") == - ]] || fail "a whole xiso read as cut short"
head -c $((41 * 2048)) "$T/halo.xiso.iso" > "$T/cut.xiso.iso"   # stops where its default.xbe starts
[[ $("$CLI" xbox "$T/cut.xiso.iso") == - ]] || fail "a cut xiso still gave a title id"
[[ $("$CLI" xbox-short "$T/cut.xiso.iso") == "$(( 41 * 2048 + $(stat -c %s "$T/halo.xiso.iso") - 41 * 2048 )) $(( 41 * 2048 ))" ]] \
  || fail "a cut xiso wasn't seen as short: $("$CLI" xbox-short "$T/cut.xiso.iso")"
[[ $("$CLI" xbox-short "$T/xbox-game") == - ]] || fail "a game folder read as a short image"
mkdir "$T/xbox-game" && python3 "$ROOT/tests/make_disc.py" xbe "$T/xbox-game/default.xbe" 54430003
[[ $("$CLI" xbox "$T/xbox-game") == 54430003 ]] || fail "xbe folder: no 54430003"
python3 "$ROOT/tests/make_disc.py" x360iso "$T/360.iso" 4D5307E6
[[ $("$CLI" x360 "$T/360.iso") == 4D5307E6 ]] || fail "360 iso: no 4D5307E6"
[[ $("$CLI" xbox "$T/360.iso") == - ]] || fail "a 360 disc read as an Xbox one"
mkdir "$T/x360-game" && python3 "$ROOT/tests/make_disc.py" xex "$T/x360-game/default.xex" 415607D2
[[ $("$CLI" x360 "$T/x360-game") == 415607D2 ]] || fail "xex folder: no 415607D2"
mkdir -p "$T/god/415607E1/00007000/ABCDEF.data" && python3 "$ROOT/tests/make_disc.py" god "$T/god/415607E1/00007000/ABCDEF" 415607E1
[[ $("$CLI" x360 "$T/god") == 415607E1 ]] || fail "GOD folder: no 415607E1"
python3 "$ROOT/tests/make_disc.py" god "$T/58410A1F" 58410A1F   # an arcade game: one STFS file, no extension
[[ $("$CLI" x360 "$T/58410A1F") == 58410A1F ]] || fail "STFS file: no 58410A1F"
vita() { local got; got=$("$CLI" vita "$@"); echo "$got"; }
python3 "$ROOT/tests/make_disc.py" vpk "$T/game.vpk" PCSE00001
[[ $(vita "$T/game.vpk") == "PCSE00001 PCSE00001" ]] || fail "vpk: $(vita "$T/game.vpk")"
python3 "$ROOT/tests/make_disc.py" vpk "$T/nonpdrm.zip" PCSB00002 PCSE00001 app/PCSB00002/   # shares PCSE00001's saves
[[ $(vita "$T/nonpdrm.zip") == "PCSB00002 PCSE00001" ]] || fail "NoNpDrm zip: $(vita "$T/nonpdrm.zip")"
python3 "$ROOT/tests/make_disc.py" vpk64 "$T/zip64.vpk" PCSG00009 "" PCSG00009/
[[ $(vita "$T/zip64.vpk") == "PCSG00009 PCSG00009" ]] || fail "zip64 vpk: $(vita "$T/zip64.vpk")"
python3 "$ROOT/tests/make_disc.py" pkg "$T/game.pkg" EP0001-PCSF00007_00-WIPEOUT2048000000
[[ $(vita "$T/game.pkg") == "PCSF00007 PCSF00007 (title id only)" ]] || fail "pkg: $(vita "$T/game.pkg")"
printf 'PCSF00007\n' > "$T/WipEout.psvita"
[[ $(vita "$T/WipEout.psvita") == "PCSF00007 PCSF00007 (title id only)" ]] || fail ".psvita: $(vita "$T/WipEout.psvita")"
mkdir -p "$T/ux0/app/PCSF00007/sce_sys" && python3 "$ROOT/tests/make_disc.py" sfo "$T/ux0/app/PCSF00007/sce_sys/param.sfo" \
    TITLE_ID=PCSF00007 INSTALL_DIR_SAVEDATA=PCSE00003
[[ $(vita "$T/game.pkg" "$T/ux0/app") == "PCSF00007 PCSE00003" ]] || fail "installed copy's save folder: $(vita "$T/game.pkg" "$T/ux0/app")"
mkdir -p "$T/vita-folder/PCSE00004" && python3 -c "
import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$(python3 "$ROOT/tests/make_disc.py" vpk "$T/f.vpk" PCSE00004 >/dev/null; echo "$T/f.vpk")" "$T/vita-folder/PCSE00004"
[[ $(vita "$T/vita-folder") == "PCSE00004 PCSE00004" ]] || fail "game folder: $(vita "$T/vita-folder")"
[[ $(vita "$T/ps3.iso") == - ]] || fail "a PS3 disc read as a Vita game"
python3 "$ROOT/tests/make_disc.py" ps4pkg "$T/game.pkg4" UP9000-CUSA00552_00-THELASTOFUS00000
[[ $("$CLI" ps4 "$T/game.pkg4") == CUSA00552 ]] || fail "ps4 pkg: $("$CLI" ps4 "$T/game.pkg4")"
mkdir -p "$T/ps4game/CUSA07408/sce_sys" && python3 "$ROOT/tests/make_disc.py" sfo "$T/ps4game/CUSA07408/sce_sys/param.sfo" TITLE_ID=CUSA07408
[[ $("$CLI" ps4 "$T/ps4game") == CUSA07408 ]] || fail "ps4 game folder: $("$CLI" ps4 "$T/ps4game")"
[[ $("$CLI" ps4 "$T/game.pkg") == - ]] || fail "a Vita .pkg read as a PS4 one"
mkdir -p "$T/svm/Monkey Island 2.scummvm" "$T/svm/Sky" "$T/svm/Loom"
printf 'scumm:monkey2\n' > "$T/svm/Monkey Island 2.scummvm/Monkey Island 2.scummvm"
[[ $("$CLI" scummvm "$T/svm/Monkey Island 2.scummvm") == monkey2 ]] || fail "scummvm .scummvm in the folder: $("$CLI" scummvm "$T/svm/Monkey Island 2.scummvm")"
printf 'sky\r\n' > "$T/svm/sky.scummvm"
[[ $("$CLI" scummvm "$T/svm/sky.scummvm") == sky ]] || fail "scummvm .scummvm file: $("$CLI" scummvm "$T/svm/sky.scummvm")"
printf '[scummvm]\nsavepath=/saves\n\n[sky]\ngameid=sky\npath=%s/\n\n[loom-fm]\nengineid=scumm\npath=%s\n' "$T/svm/Sky" "$T/svm/Loom" > "$T/svm/scummvm.ini"
[[ $("$CLI" scummvm "$T/svm/Loom" "$T/svm/scummvm.ini")/$("$CLI" scummvm "$T/svm/Sky" "$T/svm/scummvm.ini") == loom-fm/sky ]] \
  || fail "scummvm.ini's targets: $("$CLI" scummvm "$T/svm/Loom" "$T/svm/scummvm.ini")/$("$CLI" scummvm "$T/svm/Sky" "$T/svm/scummvm.ini")"
[[ $("$CLI" scummvm "$T/svm/Loom") == - ]] || fail "a ScummVM game with no .scummvm and no ini"
head -c 40000 /dev/urandom > "$T/junk.iso"
expect "$T/junk.iso" "- -"
[[ $("$CLI" xbox "$T/junk.iso")/$("$CLI" x360 "$T/junk.iso") == -/- ]] || fail "junk read as an Xbox game"
echo "gameid: ok"
