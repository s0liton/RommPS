#!/usr/bin/env bash
# Runs a command in the Steam Deck build image (platform/steamdeck/app/docker),
# with this repository at /repo.
#
#   tools/deck-build.sh                  a shell
#   tools/deck-build.sh <command...>     e.g. tools/deck-build.sh make steamdeck-app
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${DECK_IMAGE:-romm-sync-deck:6}"
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$ROOT/platform/steamdeck/app/docker"
TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)
[[ $# -gt 0 ]] || set -- bash
# RommDeck's copy says which commit of this repository it is (tools/rommdeck-copy.sh).
if [[ -f "$ROOT/SOURCE.txt" ]]; then
    BUILD_ID="$(sed -n 's/^commit \(.\{7\}\).*/\1/p' "$ROOT/SOURCE.txt")"
else
    BUILD_ID="$(git -C "$ROOT" describe --always --dirty 2>/dev/null || true)"
fi
exec docker run --rm ${TTY[@]+"${TTY[@]}"} -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e BUILD_ID="$BUILD_ID" -e ROMMPS_PAYLOAD_VERSION \
    -v "$ROOT:/repo" -w /repo "$IMAGE" "$@"
