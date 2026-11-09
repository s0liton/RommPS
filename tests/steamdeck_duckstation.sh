#!/usr/bin/env bash
# The Steam Deck build (make steamdeck) with DuckStation, against
# tests/mock_romm.py: its memory cards named as DuckStation names them, from
# its settings and the game database of the DuckStation installed. EmuDeck's
# AppImage (a newer build: discsets.yaml) shows its database only while it
# runs, so it's copied then; RetroDECK's older build has it in its Flatpak.
# The databases here are made up (serials, titles), never DuckStation's.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${STEAMDECK_BIN:-$ROOT/build/steamdeck/romm-sync}"
T="$(mktemp -d)"
MOCK_PORT=18811
API_PORT=18812
cleanup() { kill "${DAEMON:-}" "${MOCK:-}" 2>/dev/null || true; rm -rf "$T"; }
trap cleanup EXIT
fail() { echo "FAIL: $*" >&2; echo "--- daemon log" >&2; tail -40 "$T/daemon.log" >&2; exit 1; }
API="http://127.0.0.1:$API_PORT"
MURL="http://127.0.0.1:$MOCK_PORT"
post() { curl -sf -X POST "$API$1" -d "${2:-{\}}"; }
wait_file() { for _ in $(seq 1 60); do [[ -e "$1" ]] && return 0; sleep 0.5; done; fail "missing: ${1#"$T"/}"; }
wait_log() { for _ in $(seq 1 60); do grep -qF "$1" "$T/daemon.log" && return 0; sleep 0.5; done; fail "not in the log: $1"; }
# The log from a mark on: what happened since.
mark() { MARK=$(wc -l < "$T/daemon.log"); }
wait_new() { for _ in $(seq 1 60); do tail -n +$((MARK + 1)) "$T/daemon.log" | grep -qF "$1" && return 0; sleep 0.5; done; fail "not in the log since: $1"; }
syncs() { grep -c "sync done" "$T/daemon.log" || true; }
sync_wait() { local s; s=$(syncs); post /api/sync >/dev/null; for _ in $(seq 1 40); do [[ $(syncs) -gt $s ]] && return 0; sleep 0.5; done; }
start() { # HOME, then more environment
    local home="$1"; shift
    env HOME="$home" ROMM_SYNC_DATA="$T/data" ROMM_SYNC_PROC="$T/proc" ROMM_SYNC_NO_NOTIFY=1 ROMM_SYNC_MEDIA="$T/no-media" \
        "$@" "$BIN" >> "$T/daemon.log" 2>&1 & DAEMON=$!
    local up=0
    for _ in $(seq 1 80); do curl -sf "$API/api/status" >/dev/null && { up=1; break; }; sleep 0.25; done
    [[ $up == 1 ]] || fail "the service didn't start for $home"
    post /api/pair/start "{\"server_url\":\"$MURL\"}" >/dev/null
    local paired=0
    for _ in $(seq 1 60); do curl -sf "$API/api/status" | grep -q '"paired":true' && { paired=1; break; }; sleep 0.5; done
    [[ $paired == 1 ]] || fail "pairing with the mock RomM didn't finish"
}
setup() { # the DuckStation detected whose name starts so
    body=$(curl -sf "$API/api/setup/detect" | python3 -c '
import json,sys
d=json.load(sys.stdin)
duck=[c["root"] for c in d if c["name"]==sys.argv[1]]
if not duck: sys.exit("no %s; found: %s" % (sys.argv[1], ", ".join(c["name"] for c in d)))
print(json.dumps({"roots":duck,"choices":{"psx":duck[0]}}))' "$1") || fail "setup: $body"
    post /api/setup/emulators "$body" >/dev/null
    post /api/setup/finish '{"conflict_policy":"ask"}' >/dev/null
}
# A raw PS1 memory card, base64, with one save of the game whose code is $1 ("SLUS-12345"), its data $2.
card_b64() { python3 -c '
import base64,sys
img=bytearray(128*1024); img[0:2]=b"MC"
code=sys.argv[1].replace("-","-")
for f in range(1,16):
    fr=memoryview(img)[f*128:(f+1)*128]
    fr[0]=0x51 if f==1 else 0xA0
    if f==1:
        fr[4:8]=(8192).to_bytes(4,"little"); name=("BA"+code+"SAVE").encode(); fr[10:10+len(name)]=name
    fr[8]=fr[9]=0xFF
for f in range(16):
    x=0
    for b in img[f*128:f*128+127]: x^=b
    img[f*128+127]=x
data=sys.argv[2].encode(); img[8192:8192+len(data)]=data
print(base64.b64encode(bytes(img)).decode())' "$1" "$2"; }
disc_b64() { python3 "$ROOT/tests/make_disc.py" psxbin "$T/disc.bin" "$1" && base64 -w0 "$T/disc.bin"; }
# The saves RomM has for a rom: "<file_name, untagged> <emulator> <md5>" each.
server_saves() { curl -sf "$MURL/_admin/dump" | python3 -c "
import json,sys,hashlib,urllib.request
for s in json.load(sys.stdin)['saves']:
    if s['rom_id'] != int(sys.argv[1]): continue
    b = urllib.request.urlopen('$MURL/_admin/save_content?id=%d' % s['id']).read()
    print(s['file_name'].split(' [')[0] + '.' + s['file_name'].rsplit('.', 1)[-1], s.get('emulator'), hashlib.md5(b).hexdigest())" "$1"; }
# A card changed here going up: RomM gets it under the other clients' name, <stem of the file booted>_1.mcd.
uploads_as() { # rom id, local card, name on RomM
    sleep 1.1
    printf 'changed-%s' "$RANDOM" | dd of="$2" bs=1 seek=8200 conv=notrunc status=none
    sync_wait
    local want up=""
    want=$(md5sum "$2" | cut -d' ' -f1)
    for _ in $(seq 1 20); do up=$(server_saves "$1"); [[ "$up" == *"$3 duckstation $want"* ]] && return 0; sleep 0.5; done
    fail "${2##*/} didn't go up as $3 (DuckStation's psx card): $up"
}
mkdir -p "$T/proc" "$T/data" "$T/no-media"
echo "{\"web_port\": $API_PORT}" > "$T/data/config.json"
python3 "$ROOT/tests/mock_romm.py" $MOCK_PORT 2>"$T/mock.log" & MOCK=$!
for _ in $(seq 1 40); do curl -sf "$MURL/_admin/dump" >/dev/null && break; sleep 0.25; done

# ---- EmuDeck's AppImage ----
E="$T/ed/home"; EM="$T/card/Emulation"; CARDS="$EM/saves/duckstation/saves"
mkdir -p "$E/.config/EmuDeck" "$E/Applications" "$E/.local/share/duckstation" "$EM/roms/psx" "$CARDS" "$EM/bios"
cat > "$E/.config/EmuDeck/settings.sh" <<EOF
emulationPath=$EM
romsPath=$EM/roms
toolsPath=$EM/tools
biosPath=$EM/bios
savesPath=$EM/saves
storagePath=$EM/storage
EOF
printf 'appimage-v1' > "$E/Applications/DuckStation.AppImage"
printf '[Main]\nSettingsVersion = 3\n\n[MemoryCards]\nCard1Type = PerGameTitle\nCard2Type = None\nUsePlaylistTitle = true\nDirectory = %s\n' \
    "$CARDS" > "$E/.local/share/duckstation/settings.ini"
# Its mount while it runs, the database in it.
MNT="$T/tmp/.mount_DuckSt1/usr/bin"
mkdir -p "$MNT/resources"; : > "$MNT/duckstation-qt"
cat > "$MNT/resources/gamedb.yaml" <<'EOF'
SLUS-12345:
  name: "Test Quest"
  saveName: "Test Quest Title (Testland)"
  codes:
    - SLUS-12345
    - SLUS-92345
SLUS-55501:
  name: "Saga (Disc 1)"
  saveName: "Saga (Testland) (Disc 1)"
SLUS-55502:
  name: "Saga (Disc 2)"
  saveName: "Saga (Testland) (Disc 2)"
EOF
cat > "$MNT/resources/discsets.yaml" <<'EOF'
- name: "Saga"
  saveName: "Saga (Testland)"
  serials:
    - SLUS-55501
    - SLUS-55502
EOF
running() { # pid, the AppImage: a DuckStation running from its mount
    mkdir -p "$T/proc/$1"; echo duckstation-qt > "$T/proc/$1/comm"; ln -sfn "$MNT/duckstation-qt" "$T/proc/$1/exe"
    printf 'HOME=%s\0APPIMAGE=%s\0PATH=/usr/bin\0' "$E" "$2" > "$T/proc/$1/environ"
}

start "$E" ROMM_SYNC_RDAPP="$T/no-retrodeck" ROMM_SYNC_FLATPAK="$T/no-flatpaks"
setup DuckStation

echo "1. the AppImage running: its database copied, with the data folder it uses"
running 5151 "$E/Applications/DuckStation.AppImage"
wait_log "DuckStation's game database, for its memory card names: copied from $E/Applications/DuckStation.AppImage"
rm -rf "$T/proc/5151"
copy=$(ls -d "$T/data/duckstation"/*/)
[[ -f "$copy/gamedb.yaml" && -f "$copy/discsets.yaml" ]] || fail "the copy: $(ls -R "$T/data/duckstation")"
[[ "$(cat "$copy/data_root")" == "$(realpath "$E/.local/share/duckstation")" ]] || fail "the copy's data root: $(cat "$copy/data_root")"

echo "2. RomM's card comes down named by the game's title, one made here goes up as DuckStation's"
printf '{"id":851,"platform_id":5,"name":"Test Quest","fs_name":"tq.bin","files":[{"id":8511,"file_name":"tq.bin","content_b64":"%s"}]}' "$(disc_b64 SLUS_123.45)" | curl -sf -X POST "$MURL/_admin/rom" --data-binary @- >/dev/null
printf '{"rom_id":851,"slot":"autosave","file_name":"seed_1.mcd","emulator":"duckstation","content_b64":"%s"}' "$(card_b64 SLUS-12345 from-server)" | curl -sf -X POST "$MURL/_admin/save" --data-binary @- >/dev/null
post /api/download '{"rom_id":851,"name":"Test Quest"}' >/dev/null
wait_file "$EM/roms/psx/tq.bin"
CARD="$CARDS/Test Quest Title (Testland)_1.mcd"
wait_file "$CARD"
grep -q "from-server" "$CARD" || fail "RomM's card isn't the one that came down"
[[ ! -e "$CARDS/tq_1.mcd" ]] || fail "a card named after the file, which DuckStation doesn't read"
uploads_as 851 "$CARD" "tq_1.mcd"

echo "3. the settings say otherwise: the name follows them"
INI="$E/.local/share/duckstation/settings.ini"
named() { # Card1Type, the name it gives
    mark; sed -i "s/^Card1Type = .*/Card1Type = $1/" "$INI"; sync_wait
    wait_new "tq.bin: DuckStation's memory card for it is $2.mcd"
}
named PerGame SLUS-12345_1
named PerGameFileTitle tq_1
sed -i 's/^Card1Type = .*/Card1Type = None/' "$INI"
sid=$(curl -sf "$MURL/_admin/dump" | python3 -c "import json,sys; print([s['id'] for s in json.load(sys.stdin)['saves'] if s['rom_id']==851][0])")
why=$(curl -s -X POST "$API/api/restore" -d "{\"kind\":\"save\",\"id\":$sid}")
[[ "$why" == *"DuckStation keeps no memory card for this game"* ]] || fail "Card1Type None: its saves must not sync: $why"
# The game's own settings over them.
sed -i 's/^Card1Type = .*/Card1Type = PerGameTitle/' "$INI"
mkdir -p "$E/.local/share/duckstation/gamesettings"
printf '[MemoryCards]\nCard1Type = PerGame\n' > "$E/.local/share/duckstation/gamesettings/SLUS-12345.ini"
mark; sync_wait
wait_new "tq.bin: DuckStation's memory card for it is SLUS-12345_1.mcd"
rm -f "$E/.local/share/duckstation/gamesettings/SLUS-12345.ini"
mark; sync_wait
wait_new "tq.bin: DuckStation's memory card for it is Test Quest Title (Testland)_1.mcd"

echo "4. a newer AppImage: copied again, the older copy gone"
printf 'appimage-v2, longer' > "$E/Applications/DuckStation.AppImage"
running 5152 "$E/Applications/DuckStation.AppImage"
for _ in $(seq 1 40); do [[ "$(ls "$T/data/duckstation" | wc -l)" == 1 && ! -d "$copy" ]] && break; sleep 0.5; done
rm -rf "$T/proc/5152"
[[ "$(ls "$T/data/duckstation" | wc -l)" == 1 && ! -d "$copy" ]] || fail "copies: $(ls "$T/data/duckstation")"

kill "$DAEMON"; wait "$DAEMON" 2>/dev/null || true
rm -rf "$T/data"; mkdir -p "$T/data"; echo "{\"web_port\": $API_PORT}" > "$T/data/config.json"

# ---- RetroDECK: the older build, its database in the Flatpak ----
H="$T/rd/home"; SD="$T/sd/retrodeck"; RD="$H/retrodeck"; APP="$H/.var/app/net.retrodeck.retrodeck"
RCARDS="$SD/saves/psx/duckstation/memcards"
RES="$T/flatpaks/app/net.retrodeck.retrodeck/current/active/files/retrodeck/components/duckstation/bin/resources"
mkdir -p "$SD/roms/psx" "$RD/bios" "$RD/states" "$APP/config/retrodeck" "$APP/config/duckstation" "$RCARDS" "$RES" \
    "$SD/saves/psx/duckstation"
cat > "$APP/config/retrodeck/retrodeck.json" <<EOF
{"version": "0.10.10b", "paths": {"rd_home_path": "$SD", "roms_path": "$SD/roms", "saves_path": "$SD/saves",
 "states_path": "$RD/states", "bios_path": "$RD/bios"}}
EOF
printf '[MemoryCards]\nCard1Type = PerGameTitle\nCard2Type = None\nUsePlaylistTitle = true\nCard1Path = %s/shared_card_1.mcd\nDirectory = %s\n' \
    "$RCARDS" "$RCARDS" > "$APP/config/duckstation/settings.ini"
cat > "$RES/gamedb.yaml" <<'EOF'
SLUS-55501:
  name: "Saga (Testland) (Disc 1)"
  discSet:
    name: "Saga (Testland)"
    serials:
      - SLUS-55501
      - SLUS-55502
SLUS-55502:
  name: "Saga (Testland) (Disc 2)"
  discSet:
    name: "Saga (Testland)"
    serials:
      - SLUS-55501
      - SLUS-55502
EOF
start "$H" ROMM_SYNC_FLATPAK="$T/flatpaks"
setup "DuckStation (RetroDECK)"

echo "5. RetroDECK: a disc of a set gets the set's card; a playlist, its own name"
d1=$(disc_b64 SLUS_555.01); d2=$(disc_b64 SLUS_555.02)
printf '{"id":852,"platform_id":5,"name":"Saga","fs_name":"saga1.bin","files":[{"id":8521,"file_name":"saga1.bin","content_b64":"%s"}]}' "$d1" | curl -sf -X POST "$MURL/_admin/rom" --data-binary @- >/dev/null
printf '{"rom_id":852,"slot":"autosave","file_name":"seed_1.mcd","emulator":"duckstation","content_b64":"%s"}' "$(card_b64 SLUS-55501 saga-server)" | curl -sf -X POST "$MURL/_admin/save" --data-binary @- >/dev/null
post /api/download '{"rom_id":852,"name":"Saga"}' >/dev/null
wait_file "$SD/roms/psx/saga1.bin"
wait_file "$RCARDS/Saga (Testland)_1.mcd"
grep -q "saga-server" "$RCARDS/Saga (Testland)_1.mcd" || fail "RomM's card isn't the set's"
wait_log "DuckStation's game database: $RES"
# A playlist of the set, on the card: RetroDECK's build names its card after the playlist.
mkdir -p "$SD/roms/psx/Saga Set"
base64 -d <<< "$d1" > "$SD/roms/psx/Saga Set/Saga (Disc 1).bin"; base64 -d <<< "$d2" > "$SD/roms/psx/Saga Set/Saga (Disc 2).bin"
printf 'Saga (Disc 1).bin\nSaga (Disc 2).bin\n' > "$SD/roms/psx/Saga Set/Saga Set.m3u"
curl -sf -X POST "$MURL/_admin/rom" -d "{\"id\":853,\"platform_id\":5,\"name\":\"Saga Set\",\"fs_name\":\"Saga Set\",\"files\":[{\"id\":8531,\"file_name\":\"Saga Set.m3u\",\"content\":\"x\"},{\"id\":8532,\"file_name\":\"Saga (Disc 1).bin\",\"content\":\"x\"}]}" >/dev/null
sync_wait
wait_log "Saga Set: DuckStation's memory card for it is Saga Set_1.mcd"
# On RomM: a disc's own stem, a playlist's the playlist's (sigil's content stem).
uploads_as 852 "$RCARDS/Saga (Testland)_1.mcd" "saga1_1.mcd"
card_b64 SLUS-55501 saga-set | base64 -d > "$RCARDS/Saga Set_1.mcd"
uploads_as 853 "$RCARDS/Saga Set_1.mcd" "Saga Set_1.mcd"

echo "PASS"
