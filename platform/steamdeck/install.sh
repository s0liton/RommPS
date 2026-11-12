#!/usr/bin/env bash
# Installs RommDeck on a Steam Deck (or any Linux with RetroDECK), for the
# user running it: the app in ~/.local/share/rommdeck, RomM Sync (its
# background service) as a systemd user unit that starts with the session, in
# Game Mode and Desktop Mode alike, and an entry in the desktop's menu. Run it
# again to update; the service's settings and pairing (~/.local/share/romm-sync)
# stay.
#
#   ./install.sh               from an unpacked release (rommdeck, romm-sync, assets/ beside it)
#   ./install.sh --steam       also add RommDeck to Steam (closes Steam for a moment)
set -euo pipefail
HERE="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/rommdeck"

for f in rommdeck romm-sync assets install-service.sh; do
    [[ -e "$HERE/$f" ]] || { echo "install.sh: $f isn't beside this script" >&2; exit 1; }
done

echo "Installing RommDeck in $DEST"
mkdir -p "$DEST"
# New files beside the old, then swapped in: a running app keeps its own.
rm -rf "$DEST/assets.new" "$DEST/steam-art.new"
cp -a "$HERE/assets" "$DEST/assets.new"
[[ -d "$HERE/steam-art" ]] && cp -a "$HERE/steam-art" "$DEST/steam-art.new"
cp "$HERE/rommdeck" "$DEST/rommdeck.new"
for f in add-to-steam.py install-service.sh; do
    [[ -f "$HERE/$f" ]] && cp "$HERE/$f" "$DEST/$f.new" && mv -f "$DEST/$f.new" "$DEST/$f"
done
rm -rf "$DEST/assets.old" "$DEST/steam-art.old"
[[ -d "$DEST/assets" ]] && mv "$DEST/assets" "$DEST/assets.old"
mv "$DEST/assets.new" "$DEST/assets"
if [[ -d "$DEST/steam-art.new" ]]; then
    [[ -d "$DEST/steam-art" ]] && mv "$DEST/steam-art" "$DEST/steam-art.old"
    mv "$DEST/steam-art.new" "$DEST/steam-art"
fi
mv -f "$DEST/rommdeck.new" "$DEST/rommdeck"
rm -rf "$DEST/assets.old" "$DEST/steam-art.old"

bash "$HERE/install-service.sh" "$HERE/romm-sync"

# Desktop Mode's application menu
APPS="${XDG_DATA_HOME:-$HOME/.local/share}/applications"
mkdir -p "$APPS"
cat > "$APPS/rommdeck.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=RommDeck
Comment=Your RomM library and save sync, for RetroDECK, EmuDeck and standalone emulators
Exec=$DEST/rommdeck
Path=$DEST
Icon=$DEST/steam-art/icon.png
StartupWMClass=io.github.s0liton.RommDeck
Categories=Game;
Terminal=false
EOF

if [[ "${1:-}" == "--steam" ]]; then
    python3 "$DEST/add-to-steam.py" --restart-steam "$DEST/rommdeck"
fi
