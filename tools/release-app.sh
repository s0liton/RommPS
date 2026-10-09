#!/usr/bin/env bash
# Builds RommPS for a release and attaches PPSA76677.zip to its GitHub release.
#
#   tools/release-app.sh v1.1.0            build and package it, in build/release/
#   tools/release-app.sh v1.1.0 --upload   and attach it to the release v1.1.0
#
# The release workflow can't build RommPS: it needs the PS5 stack beside this
# repository (tools/ps5-app-build.sh), and the menu sounds aren't in git. So it
# is built here, from the tagged commit, once the workflow has published the
# payloads. The homebrew catalog picks the release up from the asset's name,
# so it stays PPSA76677.zip, and an asset already published is never replaced.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
APP="$ROOT/platform/ps5/app/rommps"
TAG="${1:?usage: tools/release-app.sh vX.Y.Z [--upload]}"
die() { echo "release-app: $*" >&2; exit 1; }

# The catalog tells consoles about an update from contentVersion, so it must
# match the tag: v1.1.0 is 01.001.000.
[[ "$TAG" =~ ^v([0-9]+)\.([0-9]+)\.([0-9]+)$ ]] || die "$TAG isn't a vX.Y.Z tag"
want=$(printf "%02d.%03d.%03d" "${BASH_REMATCH[1]}" "${BASH_REMATCH[2]}" "${BASH_REMATCH[3]}")
have=$(python3 -c 'import json,sys; print(json.load(open(sys.argv[1]))["contentVersion"])' "$APP/ps5/sce_sys/param.json")
[[ "$have" == "$want" ]] || die "contentVersion is $have, $TAG needs $want (ps5/sce_sys/param.json)"

cd "$ROOT"
git diff --quiet && git diff --cached --quiet || die "commit or stash your changes first"
[[ "$(git rev-parse HEAD)" == "$(git rev-parse "$TAG^{commit}" 2>/dev/null)" ]] || die "check out $TAG first"
ls platform/ps5/app/assets/sounds/*.wav >/dev/null 2>&1 || die "the menu sounds aren't in platform/ps5/app/assets/sounds"
for art in pic0 pic1; do # the home screen art, made by platform/ps5/app/tools/title-art.sh
    [[ -s "$APP/ps5/sce_sys/$art.dds" ]] || die "$art.dds is missing: run platform/ps5/app/tools/title-art.sh"
done

# RommPS carries the payload and starts it when it isn't running: the
# release's own romm-sync.elf, the same file the in-app updater installs.
grep -q "kMinPayload = \"${TAG#v}\"" "$APP/examples/rommps/kit/rommps_app.hpp" ||
    die "kMinPayload in rommps_app.hpp isn't ${TAG#v}"
mkdir -p build/ps5
gh release download "$TAG" -p romm-sync.elf -O build/ps5/romm-sync.elf --clobber
(cd build/ps5 && gh release download "$TAG" -p SHA256SUMS -O - | grep " romm-sync.elf$" | shasum -a 256 -c -) ||
    die "build/ps5/romm-sync.elf isn't $TAG's"

tools/ps5-app-build.sh bash -c 'cd /repo/platform/ps5/app/rommps && bash ps5/tools/build.sh rommps'
# The notices name the commit of every part; one built with uncommitted changes
# wouldn't be the source they point to.
dirty=$(python3 -c 'import json,sys; print(" ".join(p["id"] for p in json.load(open(sys.argv[1])) if p["source"].get("dirty")))' \
    "$APP/dist/PPSA76677/licenses/components.json")
[[ -z "$dirty" ]] || die "built from uncommitted changes in: $dirty"

mkdir -p build/release
rm -f build/release/PPSA76677.zip
(cd "$APP/dist" && zip -qrX "$ROOT/build/release/PPSA76677.zip" PPSA76677 -x 'PPSA76677/screenshots/*')
shasum -a 256 build/release/PPSA76677.zip

if [[ "${2:-}" == --upload ]]; then
    gh release view "$TAG" --json assets -q '.assets[].name' | grep -qx PPSA76677.zip &&
        die "$TAG already has PPSA76677.zip; publish a new release instead of replacing it"
    gh release upload "$TAG" build/release/PPSA76677.zip
fi
