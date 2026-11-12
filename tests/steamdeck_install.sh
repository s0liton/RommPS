#!/usr/bin/env bash
# RommDeck's installer (platform/steamdeck/rommdeck-installer.sh and
# RommDeck-Installer.desktop) against fake releases served over HTTP, with a
# real AppImage from package.sh, a scratch home, and stand-ins for kdialog,
# systemd, Steam and the app's process. Covers a fresh install with the Steam
# shortcut and its artwork, updating, a damaged download refused, uninstalling
# with and without the settings, Steam declined and added later, the .desktop
# fetching the installer itself, and taking over from the tarball's install.sh.
#
#   tests/steamdeck_install.sh [RELEASE_DIR]   (build/steamdeck/release by default)
#
# Runs in the Deck build image (tools/deck-build.sh), where AppImages can't
# mount, so they extract and run (APPIMAGE_EXTRACT_AND_RUN).
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
REL="${1:-$ROOT/build/steamdeck/release}"
apps=("$REL"/RommDeck-*-x86_64.AppImage)
APP_SRC="${apps[0]}"
[[ -f $APP_SRC ]] || { echo "tests/steamdeck_install.sh: no AppImage in $REL (run package.sh first)" >&2; exit 1; }
HERE="$ROOT/platform/steamdeck"
PORT=18813
T="$(mktemp -d)"
H="$T/home"
FAKE_RUN="$T/run"
FAKE_LOG="$T/calls.log"
FAKE_ANSWERS="$T/answers"
cleanup() {
    kill "${SERVER:-}" 2>/dev/null || true
    for p in "$FAKE_RUN"/*.pid; do [[ ! -f $p ]] || kill "$(cat "$p")" 2>/dev/null || true; done
    rm -rf "$T"
}
trap cleanup EXIT
fail() {
    echo "FAIL: $*" >&2
    echo "--- calls" >&2; tail -30 "$FAKE_LOG" >&2 || true
    echo "--- installer log" >&2; cat "$H/.cache/rommdeck-installer.log" >&2 || true
    exit 1
}
mkdir -p "$H" "$FAKE_RUN" "$T/bin" "$T/tmp" "$T/release/latest/download"
: >"$FAKE_LOG"

# ---------------------------------------------------------------- stand-ins

# kdialog: says what it was asked, answers from $FAKE_ANSWERS (yes, no or cancel, one a line).
cat >"$T/bin/kdialog" <<'EOF'
#!/bin/bash
{ printf 'kdialog'; printf ' [%s]' "$@"; echo; } >>"$FAKE_LOG"
for a; do
    case $a in
    --progressbar) echo "org.kde.kdialog-999999 /ProgressDialog"; exit 0 ;;
    --yesno | --yesnocancel)
        answer="$(head -n1 "$FAKE_ANSWERS")"
        sed -i 1d "$FAKE_ANSWERS"
        case $answer in yes) exit 0 ;; no) exit 1 ;; *) exit 2 ;; esac
        ;;
    esac
done
EOF
# The progress window's orders, and systemd's.
for tool in dbus-send systemctl; do
    printf '#!/bin/bash\n{ printf %s; printf " [%%s]" "$@"; echo; } >>"$FAKE_LOG"\n' "$tool" >"$T/bin/$tool"
done
echo '[[ "$*" == *is-active* ]] && echo active; exit 0' >>"$T/bin/systemctl"
# Processes by name, as pidfiles in $FAKE_RUN: pgrep and pkill -x NAME.
cat >"$T/bin/pgrep" <<'EOF'
#!/bin/bash
sig=TERM name=""
while (($#)); do
    case $1 in -x) name=$2; shift ;; -u) shift ;; -KILL) sig=KILL ;; esac
    shift
done
pid="$(cat "$FAKE_RUN/$name.pid" 2>/dev/null)" && kill -0 "$pid" 2>/dev/null || exit 1
if [[ $(basename "$0") == pkill ]]; then
    kill "-$sig" "$pid"
    rm -f "$FAKE_RUN/$name.pid"
    echo "pkill [$name]" >>"$FAKE_LOG"
fi
EOF
cp "$T/bin/pgrep" "$T/bin/pkill"
# Steam: -shutdown closes the running one, anything else starts one.
cat >"$T/bin/steam" <<'EOF'
#!/bin/bash
{ printf 'steam'; printf ' [%s]' "$@"; echo; } >>"$FAKE_LOG"
if [[ "${1:-}" == -shutdown ]]; then
    kill "$(cat "$FAKE_RUN/steam.pid")" 2>/dev/null
    rm -f "$FAKE_RUN/steam.pid"
else
    setsid sleep 600 >/dev/null 2>&1 </dev/null &
    echo $! >"$FAKE_RUN/steam.pid"
fi
EOF
chmod 755 "$T/bin"/*
fake() { setsid sleep 600 >/dev/null 2>&1 </dev/null & echo $! >"$FAKE_RUN/$1.pid"; }
running() { pid="$(cat "$FAKE_RUN/$1.pid" 2>/dev/null)" && kill -0 "$pid" 2>/dev/null; }
stop() { kill "$(cat "$FAKE_RUN/$1.pid")" 2>/dev/null || true; rm -f "$FAKE_RUN/$1.pid"; }
answers() { : >"$FAKE_ANSWERS"; [[ $# == 0 ]] || printf '%s\n' "$@" >"$FAKE_ANSWERS"; }

# ---------------------------------------------------------------- Steam's shortcuts

STEAMCFG="$H/.local/share/Steam/userdata/4242/config"
mkdir -p "$STEAMCFG/grid"
# add-to-steam.py's own VDF code, for the sample file and for reading it back.
vdf() {
    python3 - "$HERE/add-to-steam.py" "$@" <<'EOF'
import importlib.util, json, sys
spec = importlib.util.spec_from_file_location("ats", sys.argv[1])
ats = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ats)
cmd, path = sys.argv[2], sys.argv[3]
if cmd == "write":
    ats.save(path, json.loads(sys.argv[4]))
else:
    print(json.dumps(ats.load(path)))
EOF
}
OTHER='{"appid": -1234567, "AppName": "Some Game", "Exe": "\"/usr/bin/somegame\"", "StartDir": "\"/usr/bin\"",
        "icon": "", "LaunchOptions": "-x", "tags": {"0": "favorite"}}'
# What the user on the Deck had: the AppImage, added by hand from Downloads.
STALE='{"appid": -7654321, "AppName": "RommDeck-1.0.0-dev.8-x86_64.AppImage",
        "Exe": "\"/home/deck/Downloads/RommDeck-1.0.0-dev.8-x86_64.AppImage\"", "StartDir": "\"/home/deck/Downloads\""}'
vdf write "$STEAMCFG/shortcuts.vdf" "[$OTHER, $STALE]"
echo other-art >"$STEAMCFG/grid/$((-1234567 & 0xFFFFFFFF))p.png"
shortcuts() { vdf read "$STEAMCFG/shortcuts.vdf"; }
names() { shortcuts | python3 -c 'import json,sys; print(",".join(e["AppName"] for e in json.load(sys.stdin)))'; }

# ---------------------------------------------------------------- releases

python3 -m http.server "$PORT" --bind 127.0.0.1 --directory "$T/release" >"$T/server.log" 2>&1 &
SERVER=$!
URL="http://127.0.0.1:$PORT"
for _ in $(seq 40); do curl -sf "$URL/" >/dev/null && break; sleep 0.25; done
curl -sf "$URL/" >/dev/null || fail "the fake release server didn't start"
cp "$HERE/rommdeck-installer.sh" "$T/release/latest/download/"

# publish VERSION [bad]: the AppImage as that version's (a byte longer for each
# new one, so each checksum differs), its .zsync (listed first: the installer
# must still see exactly one AppImage), SHA256SUMS of both, and the API's answer.
publish() {
    local v="$1" name="RommDeck-$1-x86_64.AppImage" dir="$T/release/$1"
    mkdir -p "$dir"
    cp "$APP_SRC" "$dir/$name"
    printf '%s' "$v" >>"$dir/$name"
    printf 'zsync: 0.6.2\nFilename: %s\n\n' "$name" >"$dir/$name.zsync"
    (cd "$dir" && sha256sum "$name.zsync" "$name" >SHA256SUMS)
    [[ "${2:-}" != bad ]] || printf '%064d  %s\n' 0 "$name" >"$dir/SHA256SUMS"
    cat >"$T/release/latest/api.json" <<EOF
{"tag_name": "v$v", "draft": false, "prerelease": false, "assets": [
 {"name": "$name.zsync", "size": $(stat -c %s "$dir/$name.zsync"), "browser_download_url": "$URL/$v/$name.zsync"},
 {"name": "$name", "size": $(stat -c %s "$dir/$name"), "browser_download_url": "$URL/$v/$name"},
 {"name": "SHA256SUMS", "size": 100, "browser_download_url": "$URL/$v/SHA256SUMS"},
 {"name": "RommDeck-Installer.desktop", "size": 1, "browser_download_url": "$URL/x"}]}
EOF
    sha256sum "$dir/$name" | cut -d' ' -f1
}

APPIMG="$H/Applications/RommDeck.AppImage"
DEST="$H/.local/share/rommdeck"
UNIT="$H/.config/systemd/user/romm-sync.service"
MENU="$H/.local/share/applications/rommdeck.desktop"
SETTINGS="$H/.local/share/romm-sync"
# The installer, as KDE would start it (DISPLAY set) or from a terminal (unset).
installer() {
    : >"$FAKE_LOG"
    env -i HOME="$H" PATH="$T/bin:/usr/bin:/bin" TMPDIR="$T/tmp" XDG_CURRENT_DESKTOP=KDE \
        FAKE_RUN="$FAKE_RUN" FAKE_LOG="$FAKE_LOG" FAKE_ANSWERS="$FAKE_ANSWERS" APPIMAGE_EXTRACT_AND_RUN=1 \
        ROMMDECK_RELEASE_URL="$URL/latest/api.json" "$@"
}
gui() { installer DISPLAY=:99 bash "$HERE/rommdeck-installer.sh"; }
said() { grep -qF -- "$1" "$FAKE_LOG" || fail "never said: $1"; }
called() { grep -qF -- "$1" "$FAKE_LOG" || fail "never ran: $1"; }
asked_all() { [[ ! -s $FAKE_ANSWERS ]] || fail "questions left unasked: $(tr '\n' ' ' <"$FAKE_ANSWERS")"; }
art_for() { local id="$1"; for f in "${id}p.png" "$id.png" "${id}_hero.png" "${id}_logo.png" "${id}_icon.png"; do echo "$STEAMCFG/grid/$f"; done; }
our_id() { shortcuts | python3 -c 'import json,sys; print([e["appid"] & 0xFFFFFFFF for e in json.load(sys.stdin) if e["AppName"] == "RommDeck"][0])'; }
our_exe() { shortcuts | python3 -c 'import json,sys; print([e["Exe"] for e in json.load(sys.stdin) if e["AppName"] == "RommDeck"][0])'; }

echo "1. fresh install, Steam open: asked, restarted, shortcut with artwork; the hand-added one replaced"
SUM1="$(publish 1.0.0-dev.9)"
fake steam
answers yes no # restart Steam; don't open RommDeck
gui || fail "the installer failed"
asked_all
[[ -x $APPIMG && "$(sha256sum "$APPIMG" | cut -d' ' -f1)" == "$SUM1" ]] || fail "the AppImage isn't installed as released"
[[ ! -e "$H/Applications/.RommDeck.AppImage.part" ]] || fail "the download's leftovers stayed"
[[ -x $DEST/romm-sync && -f $UNIT ]] || fail "the service isn't installed"
grep -q "ExecStart=$DEST/romm-sync" "$UNIT" || fail "the unit runs something else: $(cat "$UNIT")"
called "systemctl [--user] [enable] [romm-sync.service]"
called "systemctl [--user] [restart] [romm-sync.service]"
grep -qx "Exec=\"$APPIMG\"" "$MENU" && grep -qx "Icon=$DEST/rommdeck.png" "$MENU" &&
    grep -qx "StartupWMClass=io.github.s0liton.RommDeck" "$MENU" || fail "menu entry: $(cat "$MENU")"
[[ "$(head -c 4 "$DEST/rommdeck.png" | tail -c 3)" == PNG ]] || fail "the menu's icon isn't a PNG"
said "[Steam needs to restart to add RommDeck.]"
said "Downloading RommDeck 1.0.0-dev.9"
said "[string:Checking the download]"
said "[string:Starting RommDeck's background service]"
said "[RommDeck 1.0.0-dev.9 is installed.]"
called "steam [-shutdown]"
running steam || fail "Steam wasn't started again"
[[ "$(names)" == "Some Game,RommDeck" ]] || fail "shortcuts: $(names)"
ID="$(our_id)"
for f in $(art_for "$ID"); do [[ -s $f ]] || fail "no artwork $f"; done
[[ -L "$STEAMCFG/grid/$(python3 -c "print(($ID << 32) | 0x02000000)").png" ]] || fail "no legacy cover link"
shortcuts | python3 -c '
import json, sys
app, grid, icon = sys.argv[1:]
e = [e for e in json.load(sys.stdin) if e["AppName"] == "RommDeck"][0]
assert e["Exe"] == f"\"{app}\"", e["Exe"]
assert e["StartDir"] == f"\"{grid}\"", e["StartDir"]
assert e["icon"] == icon, e["icon"]
' "$APPIMG" "$H/Applications" "$STEAMCFG/grid/${ID}_icon.png" || fail "the RommDeck shortcut is wrong"
shortcuts | python3 -c '
import json, sys
o = [e for e in json.load(sys.stdin) if e["AppName"] == "Some Game"][0]
assert all(o[k] == v for k, v in json.loads(sys.argv[1]).items()), o
' "$OTHER" || fail "the other game's shortcut changed"
[[ "$(cat "$STEAMCFG/grid/$((-1234567 & 0xFFFFFFFF))p.png")" == other-art ]] || fail "the other game's artwork changed"
HOME="$H" python3 "$HERE/add-to-steam.py" --check "$APPIMG" || fail "--check doesn't see it"

echo "2. again, same release: Update says up to date, the app and Steam left alone"
mkdir -p "$SETTINGS"
echo '{"web_port": 8780, "paired": true}' >"$SETTINGS/config.json"
fake rommdeck
answers yes no # Update; don't open
gui || fail "the second run failed"
asked_all
said "[RommDeck 1.0.0-dev.9 is up to date.]"
grep -q "Downloading" "$FAKE_LOG" && fail "downloaded what it already had"
grep -q "steam \[-shutdown\]" "$FAKE_LOG" && fail "closed Steam with the shortcut already there"
running rommdeck || fail "closed the app with nothing to replace"

echo "3. a new release, the app still open: it's closed, replaced, the settings stay"
SUM2="$(publish 1.0.0-dev.10)"
answers yes no # Update; don't open
gui || fail "the update failed"
asked_all
running rommdeck && fail "the app is still open"
called "pkill [rommdeck]"
[[ "$(sha256sum "$APPIMG" | cut -d' ' -f1)" == "$SUM2" ]] || fail "the AppImage wasn't updated"
said "[RommDeck is updated to 1.0.0-dev.10.]"
grep -q '"paired": true' "$SETTINGS/config.json" || fail "the settings changed"
[[ "$(names)" == "Some Game,RommDeck" ]] || fail "shortcuts after the update: $(names)"

echo "4. a damaged download (wrong checksum), from a terminal: refused, nothing changed"
publish 1.0.0-dev.11 bad >/dev/null
out="$(echo 1 | installer bash "$HERE/rommdeck-installer.sh" 2>&1)" && fail "a bad checksum was accepted"
[[ $out == *"Update it to the latest release, or uninstall it?"* ]] || fail "terminal: no menu: $out"
[[ $out == *"The download is damaged"* ]] || fail "terminal: no word about the checksum: $out"
[[ "$(sha256sum "$APPIMG" | cut -d' ' -f1)" == "$SUM2" ]] || fail "the bad download replaced the AppImage"
[[ ! -e "$H/Applications/.RommDeck.AppImage.part" ]] || fail "the bad download stayed"

echo "5. uninstall, keeping the settings, Steam open, the app open"
fake rommdeck
answers no no yes # Uninstall; keep the settings; restart Steam
gui || fail "the uninstall failed"
asked_all
running rommdeck && fail "the app is still open"
called "systemctl [--user] [disable] [--now] [romm-sync.service]"
called "systemctl [--user] [daemon-reload]"
said "[Steam needs to restart to remove RommDeck.]"
for f in "$APPIMG" "$DEST" "$UNIT" "$MENU"; do [[ ! -e $f ]] || fail "$f is still there"; done
grep -q '"paired": true' "$SETTINGS/config.json" || fail "the settings went"
said "Your settings and pairing are kept in ~/.local/share/romm-sync."
[[ "$(names)" == "Some Game" ]] || fail "shortcuts after the uninstall: $(names)"
for f in $(art_for "$ID"); do [[ ! -e $f ]] || fail "artwork left: $f"; done
[[ "$(cat "$STEAMCFG/grid/$((-1234567 & 0xFFFFFFFF))p.png")" == other-art ]] || fail "the other game's artwork went"
running steam || fail "Steam wasn't started again"

echo "6. install with Steam declined, then again with Steam closed: added without asking"
publish 1.0.0-dev.12 >/dev/null
answers no no # not now; don't open
gui || fail "the install failed"
asked_all
said "To add it to Steam later, run this installer again."
[[ "$(names)" == "Some Game" ]] || fail "added to Steam after Not now: $(names)"
stop steam
answers yes no # Update; don't open
gui || fail "the second install failed"
asked_all
grep -q "Steam needs to restart" "$FAKE_LOG" && fail "asked to restart a Steam that isn't running"
[[ "$(names)" == "Some Game,RommDeck" ]] || fail "not added with Steam closed: $(names)"
running steam && fail "started a Steam that wasn't running"

echo "7. uninstall, settings too, Steam closed"
answers no yes # Uninstall; remove the settings
gui || fail "the uninstall failed"
asked_all
for f in "$APPIMG" "$DEST" "$UNIT" "$MENU" "$SETTINGS"; do [[ ! -e $f ]] || fail "$f is still there"; done
[[ "$(names)" == "Some Game" ]] || fail "shortcuts: $(names)"

echo "8. the .desktop: fetches the installer from the latest release, runs it, cleans up"
if command -v desktop-file-validate >/dev/null; then
    desktop-file-validate "$HERE/RommDeck-Installer.desktop" || fail "the .desktop isn't valid"
fi
script="$(python3 - "$HERE/RommDeck-Installer.desktop" <<'EOF'
import configparser, shlex, sys
c = configparser.ConfigParser(interpolation=None)
c.read(sys.argv[1])
args = shlex.split(c["Desktop Entry"]["Exec"])
assert args[:2] == ["bash", "-c"] and len(args) == 3, args
print(args[2])
EOF
)"
gh="https://github.com/s0liton/RommDeck/releases/latest/download/"
[[ $script == *"$gh"rommdeck-installer.sh* ]] || fail "the .desktop fetches from somewhere else"
answers no # don't open
installer DISPLAY=:99 bash -c "${script//"$gh"/"$URL/latest/download/"}" || fail "the .desktop's command failed"
asked_all
said "[RommDeck 1.0.0-dev.12 is installed.]"
[[ -x $APPIMG ]] || fail "the .desktop didn't install RommDeck"
[[ ! -e "$H/.cache/rommdeck-installer.sh" ]] || fail "the .desktop left the installer in ~/.cache"
rm -rf "$T/release/latest/download/rommdeck-installer.sh"
answers
installer DISPLAY=:99 bash -c "${script//"$gh"/"$URL/latest/download/"}" || fail "the .desktop's command failed offline"
said "[Could not download the RommDeck installer. Check your connection and try again.]"

echo "9. from the tarball (install.sh --steam): the installer takes over, and its copy of the app goes"
tarballs=("$REL"/rommdeck-*-x86_64.tar.gz)
[[ -f ${tarballs[0]} ]] || fail "no tarball in $REL"
rm -rf "$APPIMG" "$DEST" "$UNIT" "$MENU"
vdf write "$STEAMCFG/shortcuts.vdf" "[$OTHER]"
mkdir -p "$T/tarball"
tar -xzf "${tarballs[0]}" -C "$T/tarball"
fake steam
installer bash "$T/tarball/rommdeck/install.sh" --steam >/dev/null || fail "install.sh --steam failed"
[[ -x $DEST/rommdeck && -d $DEST/assets ]] || fail "install.sh didn't install the app"
[[ "$(our_exe)" == "\"$DEST/rommdeck\"" ]] || fail "install.sh's shortcut starts $(our_exe)"
answers yes yes no # Update; restart Steam; don't open
gui || fail "the installer over the tarball failed"
asked_all
said "[RommDeck is updated to 1.0.0-dev.12.]"
[[ "$(names)" == "Some Game,RommDeck" ]] || fail "shortcuts: $(names)"
[[ "$(our_exe)" == "\"$APPIMG\"" ]] || fail "the shortcut starts $(our_exe)"
for f in rommdeck assets steam-art add-to-steam.py install-service.sh; do
    [[ ! -e $DEST/$f ]] || fail "the tarball's $f stayed"
done
[[ -x $DEST/romm-sync && -d $DEST/licenses ]] || fail "the service went with the tarball's copy"

echo "PASS"
