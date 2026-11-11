#!/usr/bin/env bash
# Packages RommDeck for release, from the builds in build/steamdeck: run it in
# the Deck build image (tools/deck-build.sh), after `make steamdeck` and the
# app's CMake build.
#
#   platform/steamdeck/package.sh 1.5.0 [OUT_DIR]
#
# Writes to OUT_DIR (build/steamdeck/release by default):
#   rommdeck-<version>-x86_64.tar.gz   the app, RomM Sync and install.sh (--steam adds it to Steam)
#   RommDeck-<version>-x86_64.AppImage one file: each launch installs or updates RomM Sync, the
#                                      background service, then opens the app
# Both carry licenses/ (tools/stage-notices.py): every part's licence, beside the programs.
set -euo pipefail
VERSION="${1:?usage: package.sh VERSION [OUT_DIR]}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/../.." && pwd)"
OUT="${2:-$ROOT/build/steamdeck/release}"
B="$ROOT/build/steamdeck"
HERE="$ROOT/platform/steamdeck"

for f in "$B/romm-sync" "$B/app/rommdeck" "$B/app/assets"; do
    [[ -e "$f" ]] || { echo "package.sh: $f isn't built" >&2; exit 1; }
done
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "$OUT"

python3 "$HERE/tools/make-steam-art.py" "$WORK/steam-art" >/dev/null
python3 "$HERE/tools/stage-notices.py" "$WORK/licenses"

# The tarball: what install.sh expects beside it.
T="$WORK/rommdeck"
mkdir -p "$T"
cp "$B/app/rommdeck" "$B/romm-sync" "$T/"
cp -a "$B/app/assets" "$WORK/steam-art" "$WORK/licenses" "$T/"
cp "$HERE/install.sh" "$HERE/install-service.sh" "$HERE/add-to-steam.py" "$T/"
chmod 755 "$T/rommdeck" "$T/romm-sync" "$T/install.sh" "$T/install-service.sh" "$T/add-to-steam.py"
tar -C "$WORK" -czf "$OUT/rommdeck-$VERSION-x86_64.tar.gz" rommdeck

# The AppImage: the same files under usr/lib/rommdeck (the app finds assets/
# beside itself, install-service.sh the licences), and a launcher that keeps
# the service installed.
A="$WORK/RommDeck.AppDir"
mkdir -p "$A/usr/lib/rommdeck"
cp "$B/app/rommdeck" "$B/romm-sync" "$HERE/install-service.sh" "$A/usr/lib/rommdeck/"
cp -a "$B/app/assets" "$WORK/licenses" "$A/usr/lib/rommdeck/"
cp "$WORK/steam-art/icon.png" "$A/rommdeck.png"
cat > "$A/rommdeck.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=RommDeck
Comment=Your RomM library and save sync, for RetroDECK
Exec=rommdeck
Icon=rommdeck
Categories=Game;
Terminal=false
X-AppImage-Version=$VERSION
EOF
cat > "$A/AppRun" <<'EOF'
#!/bin/sh
# RommDeck's AppImage: RomM Sync, the background service, has to outlive the
# app, so its copy is installed (or updated) outside the AppImage first.
HERE="$(dirname "$(readlink -f "$0")")"
bash "$HERE/usr/lib/rommdeck/install-service.sh" "$HERE/usr/lib/rommdeck/romm-sync" >&2 || \
    echo "RommDeck: couldn't install its background service" >&2
exec "$HERE/usr/lib/rommdeck/rommdeck" "$@"
EOF
chmod 755 "$A/AppRun" "$A/usr/lib/rommdeck/rommdeck" "$A/usr/lib/rommdeck/romm-sync" "$A/usr/lib/rommdeck/install-service.sh"
ARCH=x86_64 appimagetool --no-appstream --runtime-file /opt/appimage-runtime-x86_64 "$A" "$OUT/RommDeck-$VERSION-x86_64.AppImage" >/dev/null
ls -la "$OUT"
