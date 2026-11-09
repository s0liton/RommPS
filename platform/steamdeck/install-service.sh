#!/usr/bin/env bash
# Installs RomM Sync, RommDeck's background service, from a romm-sync binary:
# a copy in ~/.local/share/rommdeck (a path that stays put, unlike an
# AppImage's) and a systemd user unit that starts it with the session.
# Nothing happens when that copy is already this one, so the AppImage runs it
# at every launch to keep the service up to date. Settings and pairing
# (~/.local/share/romm-sync) are never touched.
#
#   install-service.sh /path/to/romm-sync
set -euo pipefail

SRC="${1:?usage: install-service.sh /path/to/romm-sync}"
DEST="${XDG_DATA_HOME:-$HOME/.local/share}/rommdeck"
UNIT_DIR="${XDG_CONFIG_HOME:-$HOME/.config}/systemd/user"
UNIT="$UNIT_DIR/romm-sync.service"

unit_text() {
    cat <<EOF
[Unit]
Description=RommDeck's background service: save sync and downloads
After=network-online.target
Wants=network-online.target

[Service]
ExecStart=$DEST/romm-sync
Restart=on-failure
RestartSec=10

[Install]
WantedBy=default.target
EOF
}

if cmp -s "$SRC" "$DEST/romm-sync" && [[ -f "$UNIT" ]] && [[ "$(unit_text)" == "$(cat "$UNIT")" ]]; then
    exit 0
fi

mkdir -p "$DEST" "$UNIT_DIR"
# Beside it first, then renamed over it: a running service keeps its old file.
cp "$SRC" "$DEST/romm-sync.new"
chmod 755 "$DEST/romm-sync.new"
mv -f "$DEST/romm-sync.new" "$DEST/romm-sync"
unit_text > "$UNIT"
systemctl --user daemon-reload
systemctl --user enable romm-sync.service >/dev/null 2>&1
systemctl --user restart romm-sync.service
echo "RommDeck's background service is running: $(systemctl --user is-active romm-sync.service)"
