#!/usr/bin/env bash
# Build romm-sync for a console inside its Docker SDK image (platform/<console>/docker).
# tools/ps5-build.sh and tools/ps4-build.sh call this with the console filled in.
#
#   tools/console-build.sh ps4                   # make ps4
#   tools/console-build.sh ps4 clean ps4 V=1     # any make args/targets
#   tools/console-build.sh ps4 --rebuild         # force (re)build of the image, then make ps4
#   tools/console-build.sh ps4 --shell           # interactive shell in the container
#   tools/console-build.sh ps4 --ca [DEST]       # copy the image's ca-bundle.crt (default ./cacert.pem)
#   tools/console-build.sh ps4 --test            # build platform/<console>/docker/test, if it has one
set -euo pipefail

CONSOLE="${1:?usage: console-build.sh ps4|ps5 [--rebuild|--shell|--ca|--test] [make args]}"
shift
# The image tag follows the SDK version in the Dockerfile, so a new SDK gets a new image.
case "${CONSOLE}" in
    ps5) IMAGE="${PS5_IMAGE:-romm-sync-ps5sdk:v0.43}" ;;
    ps4) IMAGE="${PS4_IMAGE:-romm-sync-ps4sdk:v0.9-oo0.5.4}" ;;
    *)   echo "unknown console '${CONSOLE}', expected ps4 or ps5" >&2; exit 2 ;;
esac
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DOCKER_DIR="${ROOT}/platform/${CONSOLE}/docker"

# The PS4 image is amd64 (its Dockerfile says so, for OpenOrbis), so it runs
# emulated on arm64 hosts.
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
    --test)  exec "${RUN[@]}" make -C "platform/${CONSOLE}/docker/test" clean all ;;
    *)       exec "${RUN[@]}" make "${CONSOLE}" BUILD_ID="$(git -C "${ROOT}" describe --always --dirty 2>/dev/null)" "$@" ;;
esac
