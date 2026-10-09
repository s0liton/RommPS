#!/usr/bin/env bash
# Runs a command in the native PS5 app's build image (platform/ps5/app/docker).
# The PS5_VulkanTemplate stack lives in PS5_STACK (default ../ps5-stack, beside
# this repository) and is mounted at /stack; this repository at /repo.
#
#   tools/ps5-app-build.sh                         a shell
#   tools/ps5-app-build.sh <command...>            run a command, e.g.
#   tools/ps5-app-build.sh PS5_VulkanTemplate/ps5/tools/bootstrap.sh --check
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
STACK="${PS5_STACK:-$(cd "$ROOT/.." && pwd)/ps5-stack}"
IMAGE="${PS5_APP_IMAGE:-rommps-ps5app:1}"
mkdir -p "$STACK"
docker image inspect "$IMAGE" >/dev/null 2>&1 || docker build -t "$IMAGE" "$ROOT/platform/ps5/app/docker"
TTY=()
[[ -t 0 && -t 1 ]] && TTY=(-it)
# Scratch builds (Mesa's sources, RADV's build) go on a volume of the
# container's own: extracting large trees into a folder shared from macOS
# fails ("Directory renamed before its status could be extracted"), and is
# slow. The stack's .deps/work folders are symlinks to /work.
if ! docker volume inspect rommps-ps5-work >/dev/null 2>&1; then
    docker volume create rommps-ps5-work >/dev/null
    docker run --rm -u 0 -v rommps-ps5-work:/work "$IMAGE" chown "$(id -u):$(id -g)" /work
fi
# The stack's tools look for each other beside the title; ours lives in this
# repository instead, so they're told where the stack is. Folders shared from
# macOS look like another user's to git, which then refuses them: these are
# our own checkouts, marked safe for this container only.
exec docker run --rm ${TTY[@]+"${TTY[@]}"} -u "$(id -u):$(id -g)" -e HOME=/tmp \
    -e GIT_CONFIG_COUNT=1 -e GIT_CONFIG_KEY_0=safe.directory -e GIT_CONFIG_VALUE_0='*' \
    -e PS5_VULKAN_DIR=/stack/PS5_Vulkan -e PS5_PAYLOAD_SDK_FORK=/stack/PS5_PayloadSDK -e PS5_MESA_FORK=/stack/PS5_Mesa \
    -v rommps-ps5-work:/work -v "$STACK":/stack -v "$ROOT":/repo -w /stack "$IMAGE" "${@:-bash}"
