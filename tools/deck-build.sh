#!/usr/bin/env bash
# Runs a command in the Steam Deck build image (platform/steamdeck/app/docker),
# with this repository at /repo.
#
#   tools/deck-build.sh                  a shell
#   tools/deck-build.sh <command...>     e.g. tools/deck-build.sh make steamdeck-app
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
IMAGE="${DECK_IMAGE:-romm-sync-deck:4}"
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$ROOT/platform/steamdeck/app/docker"
TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)
[[ $# -gt 0 ]] || set -- bash
exec docker run --rm ${TTY[@]+"${TTY[@]}"} -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e BUILD_ID="$(git -C "$ROOT" describe --always --dirty 2>/dev/null)" \
    -v "$ROOT:/repo" -w /repo "$IMAGE" "$@"
