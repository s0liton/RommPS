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
head -c 40000 /dev/urandom > "$T/junk.iso"
expect "$T/junk.iso" "- -"
[[ $("$CLI" xbox "$T/junk.iso")/$("$CLI" x360 "$T/junk.iso") == -/- ]] || fail "junk read as an Xbox game"
echo "gameid: ok"
