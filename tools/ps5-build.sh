#!/usr/bin/env bash
# Build romm-sync for PS5 inside the Docker SDK image.
#
#   tools/ps5-build.sh                 # make ps5
#   tools/ps5-build.sh clean ps5 V=1   # any make args/targets
#   tools/ps5-build.sh --rebuild       # force (re)build of the image, then make ps5
#   tools/ps5-build.sh --shell         # interactive shell in the container
#   tools/ps5-build.sh --ca [DEST]     # copy the bundle's ca-bundle.crt (default ./cacert.pem)
#   tools/ps5-build.sh --test          # build tools/docker/test (static libcurl link check)
set -euo pipefail

IMAGE="${PS5_IMAGE:-romm-sync-ps5sdk:v0.43}"
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKER_DIR="${ROOT}/tools/docker"

build_image() { docker build -t "${IMAGE}" "${DOCKER_DIR}"; }

if [[ "${1:-}" == "--rebuild" ]]; then
    shift; build_image
elif ! docker image inspect "${IMAGE}" >/dev/null 2>&1; then
    build_image
fi

TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)
RUN=(docker run --rm ${TTY[@]+"${TTY[@]}"} -u "$(id -u):$(id -g)" -e HOME=/tmp
     -v "${ROOT}":/src -w /src "${IMAGE}")

case "${1:-}" in
    --shell) exec "${RUN[@]}" bash ;;
    --ca)    exec "${RUN[@]}" cp /opt/ca-bundle.crt "${2:-cacert.pem}" ;;
    --test)  exec "${RUN[@]}" make -C tools/docker/test clean all ;;
    *)       exec "${RUN[@]}" make ps5 "$@" ;;
esac
