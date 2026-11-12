#!/usr/bin/env bash
# RommDeck's installer, for the Steam Deck's Desktop Mode (or any x86_64 Linux
# with a desktop). RommDeck-Installer.desktop fetches it from the latest
# release and runs it. It downloads that release's AppImage, checks it against
# the release's SHA256SUMS, and installs it as ~/Applications/RommDeck.AppImage
# with its background service, a menu entry and a Steam shortcut with artwork.
# Run again, it updates RommDeck (settings and pairing stay) or uninstalls it.
#
#   bash rommdeck-installer.sh
#
# The service, the shortcut and the icon come out of the AppImage itself
# (AppRun's --install-service, --add-to-steam and --icon, in package.sh), so
# how each is done ships with the release that does it, and only once.
#
# It talks through kdialog (SteamOS has it), else zenity, else the terminal.
# ROMMDECK_RELEASE_URL points it at another release, in the shape GitHub's API
# gives one, and allows plain HTTP for it (tests/steamdeck_install.sh).
# SHA256SUMS comes from the same place as the AppImage, so it catches a broken
# download, not a compromised release: checking the release's signature
# (manifest-steamdeck.sig) is for later.
set -euo pipefail

RELEASE_URL="${ROMMDECK_RELEASE_URL:-https://api.github.com/repos/s0liton/RommDeck/releases/latest}"
PROTO="=https"
[[ -n "${ROMMDECK_RELEASE_URL:-}" ]] && PROTO="=https,http"
DATA="${XDG_DATA_HOME:-$HOME/.local/share}"
APPIMAGE="$HOME/Applications/RommDeck.AppImage"
PART="$HOME/Applications/.RommDeck.AppImage.part"
DEST="$DATA/rommdeck"        # the service's copy (install-service.sh), and where the tarball installs
SETTINGS="$DATA/romm-sync"
MENU="$DATA/applications/rommdeck.desktop"
UNIT="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user/romm-sync.service"
LOG="${XDG_CACHE_HOME:-$HOME/.cache}/rommdeck-installer.log"

STEP="Starting the installer" # what it's doing, for the progress window and for an error
STEAM_NOTE=""                 # what to do about Steam by hand, for the last message
REPORTED=0
CHOICE=""

UI=terminal
if [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
    if command -v kdialog >/dev/null; then
        UI=kdialog
    elif command -v zenity >/dev/null; then
        UI=zenity
    fi
fi

tilde() { printf '%s' "${1/#"$HOME"/\~}"; }
run() { echo "+ $*" >>"$LOG"; "$@" >>"$LOG" 2>&1; }
fetch() { curl -fsSL --proto "$PROTO" --proto-redir "$PROTO" --connect-timeout 30 --retry 2 "$@"; }
sha256() { sha256sum "$1" | cut -d' ' -f1; }
steam_running() { pgrep -x steam >/dev/null 2>&1; }

# ---------------------------------------------------------------- talking

tell() {
    case $UI in
    kdialog) kdialog --title RommDeck --msgbox "$1" 2>>"$LOG" || true ;;
    zenity) zenity --info --title RommDeck --text "$1" 2>>"$LOG" || true ;;
    *) printf '%s\n' "$1" ;;
    esac
}

fail() {
    echo "failed: $1" >>"$LOG"
    progress_end
    case $UI in
    kdialog) kdialog --title RommDeck --error "$1" 2>>"$LOG" || true ;;
    zenity) zenity --error --title RommDeck --text "$1" 2>>"$LOG" || true ;;
    *) printf '%s\n' "$1" >&2 ;;
    esac
    REPORTED=1
    exit 1
}

# A question with two answers: true for the first.
ask() {
    local answer=""
    case $UI in
    kdialog) kdialog --title RommDeck --yes-label "$2" --no-label "$3" --yesno "$1" 2>>"$LOG" ;;
    zenity) zenity --question --title RommDeck --ok-label "$2" --cancel-label "$3" --text "$1" 2>>"$LOG" ;;
    *)
        printf '%s\n  1) %s\n  2) %s\n> ' "$1" "$2" "$3"
        read -r answer && [[ $answer == 1 ]]
        ;;
    esac
}

# Two answers and Cancel: CHOICE is 1 or 2, or empty for Cancel.
choose() {
    local rc=0 out=""
    CHOICE=""
    case $UI in
    kdialog)
        kdialog --title RommDeck --yes-label "$2" --no-label "$3" --yesnocancel "$1" 2>>"$LOG" || rc=$?
        case $rc in 0) CHOICE=1 ;; 1) CHOICE=2 ;; esac
        ;;
    zenity)
        out="$(zenity --question --title RommDeck --ok-label "$2" --cancel-label Cancel --extra-button "$3" \
            --text "$1" 2>>"$LOG")" || rc=$?
        if [[ $rc == 0 ]]; then CHOICE=1; elif [[ $out == "$3" ]]; then CHOICE=2; fi
        ;;
    *)
        printf '%s\n  1) %s\n  2) %s\n  3) Cancel\n> ' "$1" "$2" "$3"
        read -r out || true
        [[ $out == [12] ]] && CHOICE="$out"
        ;;
    esac
    return 0
}

# The progress window. kdialog's takes its orders over D-Bus, through
# whichever tool this system happens to have; zenity's reads them on stdin.
PROGRESS=""   # kdialog's: its D-Bus service and object
ZENITY=""     # zenity's: the descriptor it reads
DBUS=""
for t in dbus-send qdbus qdbus6 busctl; do
    command -v "$t" >/dev/null && { DBUS="$t"; break; }
done

kd() {
    local svc="${PROGRESS% *}" obj="${PROGRESS#* }" i=org.kde.kdialog.ProgressDialog
    [[ -n $PROGRESS ]] || return 0
    case "$DBUS:$1" in
    # --print-reply: without it dbus-send fires and quits, and on SteamOS the
    # call never arrives, so the window kept its first label and stayed open.
    dbus-send:label) dbus-send --session --print-reply --dest="$svc" "$obj" "$i.setLabelText" "string:$2" ;;
    dbus-send:value) dbus-send --session --print-reply --dest="$svc" "$obj" org.freedesktop.DBus.Properties.Set \
        "string:$i" string:value "variant:int32:$2" ;;
    dbus-send:close) dbus-send --session --print-reply --dest="$svc" "$obj" "$i.close" ;;
    qdbus*:label) "$DBUS" "$svc" "$obj" setLabelText "$2" ;;
    qdbus*:value) "$DBUS" "$svc" "$obj" Set "" value "$2" ;;
    qdbus*:close) "$DBUS" "$svc" "$obj" close ;;
    busctl:label) busctl --user call "$svc" "$obj" "$i" setLabelText s "$2" ;;
    busctl:value) busctl --user set-property "$svc" "$obj" "$i" value i "$2" ;;
    busctl:close) busctl --user call "$svc" "$obj" "$i" close ;;
    esac >/dev/null 2>>"$LOG" || true # the replies are noise; the errors aren't
}

progress_start() {
    progress_end # one window at a time
    STEP="$1"
    case $UI in
    kdialog) PROGRESS="$(kdialog --title RommDeck --progressbar "$1" 100 2>>"$LOG")" || PROGRESS="" ;;
    zenity)
        trap '' PIPE
        exec {ZENITY}> >(zenity --progress --title RommDeck --text "$1" --auto-close --no-cancel 2>>"$LOG" || true)
        ;;
    *) printf '%s\n' "$1" ;;
    esac
}

# progress PERCENT TEXT [DETAIL]: the detail shows beside it in a window only.
progress() {
    local line="$2${3:+ $3}"
    case $UI in
    kdialog) kd value "$1"; kd label "$line" ;;
    zenity) [[ -z $ZENITY ]] || { printf '%s\n# %s\n' "$1" "$line" >&"$ZENITY"; } 2>/dev/null || true ;;
    *) [[ $2 == "$STEP" ]] || printf '%s\n' "$2" ;;
    esac
    STEP="$2"
}

progress_end() {
    local pid
    case $UI in
    kdialog)
        [[ -n $PROGRESS ]] || return 0
        kd close
        # No D-Bus tool that worked: it goes the blunt way.
        pid="${PROGRESS%% *}"
        pid="${pid##*-}"
        sleep 0.2
        # Plasma 6 runs the window as kdialog_progress_helper ("kdialog_progres"
        # in comm), Plasma 5 as kdialog itself.
        [[ "$(cat "/proc/$pid/comm" 2>/dev/null)" != kdialog* ]] || kill "$pid" 2>/dev/null || true
        PROGRESS=""
        ;;
    zenity)
        [[ -n $ZENITY ]] || return 0
        { echo 100 >&"$ZENITY"; } 2>/dev/null || true
        exec {ZENITY}>&-
        ZENITY=""
        ;;
    esac
    return 0
}

finish() {
    local rc=$?
    rm -f "$PART"
    progress_end
    if [[ $rc != 0 && $REPORTED == 0 ]]; then
        REPORTED=1
        fail "Couldn't finish ${STEP,}. Details are in $(tilde "$LOG")."
    fi
}

# ---------------------------------------------------------------- the parts

# The latest release, as GitHub's API has it: the tag, the AppImage's name,
# address and size, and the address of SHA256SUMS.
release() {
    fetch -H "Accept: application/vnd.github+json" "$RELEASE_URL" | python3 -I -c '
import json, re, sys
r = json.load(sys.stdin)
if r.get("draft") or r.get("prerelease"):
    sys.exit("not a published release: " + r.get("tag_name", "?"))
assets = {a["name"]: a for a in r.get("assets", [])}
apps = [n for n in assets if re.fullmatch(r"RommDeck-[0-9A-Za-z.+-]+-x86_64\.AppImage", n)]
if len(apps) != 1 or "SHA256SUMS" not in assets:
    sys.exit("no AppImage, or no SHA256SUMS, in " + r.get("tag_name", "?"))
a = assets[apps[0]]
print(r["tag_name"], a["name"], a["browser_download_url"], a.get("size", 0), assets["SHA256SUMS"]["browser_download_url"])
'
}

download() {   # URL FILE SIZE TEXT
    local pid have=0 total="$3" pct
    (( total > 0 )) || total=1
    progress 10 "$4"
    fetch -o "$2" "$1" 2>>"$LOG" &
    pid=$!
    while kill -0 "$pid" 2>/dev/null; do
        have="$(stat -c %s "$2" 2>/dev/null || echo 0)"
        pct=$((10 + 70 * have / total))
        (( pct <= 80 )) || pct=80
        progress "$pct" "$4" "($((have >> 20)) of $((total >> 20)) MB)"
        sleep 0.5
    done
    wait "$pid"
}

# The app, if it's open. It would start the service again behind an
# uninstall's back, and on an update it would be the old one anyway.
close_app() {
    pgrep -u "$UID" -x rommdeck >/dev/null 2>&1 || return 0
    progress "$1" "Closing RommDeck"
    pkill -u "$UID" -x rommdeck 2>/dev/null || true
    for _ in $(seq 20); do
        pgrep -u "$UID" -x rommdeck >/dev/null 2>&1 || return 0
        sleep 0.25
    done
    pkill -KILL -u "$UID" -x rommdeck 2>/dev/null || true
}

menu_entry() {
    mkdir -p "$DEST" "$(dirname "$MENU")"
    "$APPIMAGE" --icon >"$DEST/rommdeck.png.new" 2>>"$LOG"
    mv -f "$DEST/rommdeck.png.new" "$DEST/rommdeck.png"
    cat >"$MENU" <<EOF
[Desktop Entry]
Type=Application
Name=RommDeck
Comment=Your RomM library and save sync, for RetroDECK, EmuDeck and standalone emulators
Exec="$APPIMAGE"
Icon=$DEST/rommdeck.png
StartupWMClass=io.github.s0liton.RommDeck
Categories=Game;
Terminal=false
EOF
}

# Steam rewrites its shortcuts file when it quits, so it has to be closed
# while add-to-steam.py writes it. Nobody's Steam gets closed without asking.
steam_shortcut() {
    local rc=0 restart=()
    # Game Mode's Steam is the whole session: not from in here, thanks.
    if [[ ${XDG_CURRENT_DESKTOP:-} == gamescope ]]; then
        STEAM_NOTE="To add it to Steam, run this installer again in Desktop Mode."
        return 0
    fi
    run "$APPIMAGE" --add-to-steam --check "$APPIMAGE" || rc=$?
    case $rc in
    0) return 0 ;;
    1) ;;
    2) STEAM_NOTE="To add it to Steam, sign in to Steam, then run this installer again."; return 0 ;;
    *) STEAM_NOTE="Couldn't add it to Steam. Details are in $(tilde "$LOG")."; return 0 ;;
    esac
    if steam_running; then
        if ! ask "Steam needs to restart to add RommDeck." "Restart Steam" "Not now"; then
            STEAM_NOTE="To add it to Steam later, run this installer again."
            return 0
        fi
        restart=(--restart-steam)
    fi
    progress_start "Adding RommDeck to Steam"
    run "$APPIMAGE" --add-to-steam "${restart[@]}" "$APPIMAGE" ||
        STEAM_NOTE="Couldn't add it to Steam. Details are in $(tilde "$LOG")."
    progress_end
}

# Coming from the tarball (install.sh): its copy of the app goes, unless a
# Steam shortcut still starts it (Steam wasn't restarted to point it here).
tidy_tarball() {
    [[ -e $DEST/rommdeck ]] || return 0
    run "$APPIMAGE" --add-to-steam --check "$DEST/rommdeck" && return 0
    rm -rf "$DEST"/{rommdeck,assets,steam-art}{,.new,.old} "$DEST/add-to-steam.py" "$DEST/install-service.sh"
}

# ---------------------------------------------------------------- the jobs

install_app() {
    local info tag name url size sums version expected current=0 was=0 text
    [[ -e $APPIMAGE || -e $UNIT || -e $DEST/rommdeck ]] && was=1
    progress_start "Looking for the latest RommDeck"
    info="$(release 2>>"$LOG")" || fail "Couldn't find the latest RommDeck on GitHub. Check your connection and try again."
    read -r tag name url size sums <<<"$info"
    version="${tag#v}"
    expected="$(fetch "$sums" 2>>"$LOG" | awk -v f="$name" '$2 == f || $2 == "*" f { print $1 }')" || true
    [[ $expected =~ ^[0-9a-f]{64}$ ]] ||
        fail "Couldn't check RommDeck $version against its checksums. Nothing was changed. Try again later."

    mkdir -p "$(dirname "$APPIMAGE")"
    if [[ -f $APPIMAGE && "$(sha256 "$APPIMAGE")" == "$expected" ]]; then
        current=1
    else
        download "$url" "$PART" "$size" "Downloading RommDeck $version" ||
            fail "Couldn't download RommDeck $version. Check your connection and try again."
        progress 82 "Checking the download"
        [[ "$(sha256 "$PART")" == "$expected" ]] ||
            fail "The download is damaged: its checksum doesn't match. Nothing was changed. Try again later."
        chmod 755 "$PART"
        close_app 85
        mv -f "$PART" "$APPIMAGE"
    fi
    progress 88 "Starting RommDeck's background service"
    run "$APPIMAGE" --install-service ||
        fail "RommDeck's background service didn't start. Details are in $(tilde "$LOG")."
    progress 95 "Adding RommDeck to the application menu"
    menu_entry
    progress_end
    steam_shortcut
    tidy_tarball

    if [[ $current == 1 ]]; then
        text="RommDeck $version is up to date."
    elif [[ $was == 1 ]]; then
        text="RommDeck is updated to $version."
    else
        text="RommDeck $version is installed."
    fi
    [[ -z $STEAM_NOTE ]] || text+=$'\n\n'"$STEAM_NOTE"
    if [[ -n "${DISPLAY:-}${WAYLAND_DISPLAY:-}" ]]; then
        ask "$text" "Open RommDeck" "Close" && (setsid "$APPIMAGE" >/dev/null 2>&1 </dev/null &)
    else
        tell "$text"
    fi
    return 0
}

uninstall_app() {
    local settings=keep steam="" tool=() restart=() found=3 text
    choose "Remove your RommDeck settings and RomM pairing too?" "Remove them" "Keep them"
    case $CHOICE in
    1) settings=remove ;;
    2) ;;
    *) return 0 ;;
    esac
    # The shortcut goes first, while the AppImage that knows how is still here.
    if [[ -x $APPIMAGE ]]; then
        tool=("$APPIMAGE" --add-to-steam)
    elif [[ -f $DEST/add-to-steam.py ]]; then
        tool=(python3 "$DEST/add-to-steam.py")
    fi
    if [[ ${#tool[@]} != 0 ]]; then
        found=0
        run "${tool[@]}" --check || found=$?
    fi
    case $found in
    0)
        if [[ ${XDG_CURRENT_DESKTOP:-} == gamescope ]]; then
            STEAM_NOTE="Remove RommDeck from your Steam library by hand."
        elif ! steam_running; then
            steam=now
        elif ask "Steam needs to restart to remove RommDeck." "Restart Steam" "Not now"; then
            steam=restart
            restart=(--restart-steam)
        else
            STEAM_NOTE="Remove RommDeck from your Steam library by hand."
        fi
        ;;
    1 | 2) ;;
    *) STEAM_NOTE="If RommDeck is still in your Steam library, remove it by hand." ;;
    esac

    progress_start "Uninstalling RommDeck"
    close_app 10
    progress 25 "Stopping RommDeck's background service"
    run systemctl --user disable --now romm-sync.service || true
    if [[ -n $steam ]]; then
        progress 45 "Removing RommDeck from Steam"
        run "${tool[@]}" --remove "${restart[@]}" "$APPIMAGE" ||
            STEAM_NOTE="Couldn't remove RommDeck from Steam: remove it from your library by hand."
    fi
    progress 70 "Removing RommDeck's files"
    rm -rf "$APPIMAGE" "$DEST" "$MENU" "$UNIT"
    run systemctl --user daemon-reload || true
    if [[ $settings == remove ]]; then
        progress 90 "Removing your settings and pairing"
        rm -rf "$SETTINGS"
    fi
    progress_end

    text="RommDeck is uninstalled."
    [[ $settings == remove ]] || text+=" Your settings and pairing are kept in $(tilde "$SETTINGS")."
    [[ -z $STEAM_NOTE ]] || text+=$'\n\n'"$STEAM_NOTE"
    tell "$text"
}

mkdir -p "$(dirname "$LOG")"
echo "rommdeck-installer $(date -Is), from $RELEASE_URL" >"$LOG"
trap finish EXIT
[[ $EUID != 0 ]] || fail "Run the RommDeck installer as yourself, not as root."
[[ "$(uname -m)" == x86_64 ]] || fail "RommDeck needs an x86_64 machine, like the Steam Deck."
if [[ -e $APPIMAGE || -e $UNIT || -e $DEST/rommdeck ]]; then
    choose "RommDeck is installed. Update it to the latest release, or uninstall it?" "Update" "Uninstall"
    case $CHOICE in
    1) install_app ;;
    2) uninstall_app ;;
    esac
else
    install_app
fi
