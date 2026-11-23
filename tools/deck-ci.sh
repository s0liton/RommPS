#!/usr/bin/env bash
# Builds, tests and packages RommDeck and RomM Sync for the Deck, in the Deck
# build image (tools/deck-build.sh). One script for both repositories: CI here
# runs it on a copy (tools/rommdeck-copy.sh), s0liton/RommDeck runs it for its
# own CI and for every release.
#
#   tools/deck-ci.sh VERSION      1.5.0 (a leading v is dropped), or ci
#
#   tests/steamdeck*.sh  the sync service on RetroDECK, EmuDeck and standalone layouts, against the mock RomM
#   package.sh           rommdeck-<version>-<arch>.tar.gz and RommDeck-<version>-<arch>.AppImage
#                        (with its .zsync, for AppImageUpdate and Gear Lever),
#                        RommDeck-Installer.desktop and rommdeck-installer.sh, plus
#                        romm-sync-steamdeck (the service alone, for in-app updates;
#                        romm-sync-steamdeck-aarch64 from the ARM build)
#   tests/steamdeck_install.sh  the installer, with that AppImage, against a fake release
#
# Everything a release publishes ends up in build/steamdeck/release.
set -euo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.."
VERSION="${1:?usage: deck-ci.sh VERSION}"
VERSION="${VERSION#v}"
OUT=build/steamdeck/release

# Folded sections in an Actions log, plain headings anywhere else.
step() {
    [[ -z "${open:-}" || -z "${GITHUB_ACTIONS:-}" ]] || echo "::endgroup::"
    if [[ -n "${GITHUB_ACTIONS:-}" ]]; then echo "::group::$1"; else echo "== $1"; fi
    open=1
}

step "Build"
tools/deck-build.sh make steamdeck VERSION="$VERSION"
ROMMPS_PAYLOAD_VERSION="$VERSION" tools/deck-build.sh bash -c 'cmake -S platform/steamdeck/app -B build/steamdeck/app -G Ninja \
    && cmake --build build/steamdeck/app'

for t in steamdeck steamdeck_emus steamdeck_x360_vita steamdeck_saves2 steamdeck_duckstation; do
    step "Test: $t"
    tools/deck-build.sh "tests/$t.sh" || { echo "::error::tests/$t.sh failed"; exit 1; }
done

step "Package"
rm -rf "$OUT"
tools/deck-build.sh platform/steamdeck/package.sh "$VERSION" "$OUT"
# The service on its own too, for its in-app updates (src/update.c), under the
# name the service of that kind of machine asks for (platform.c).
arch="$(tools/deck-build.sh uname -m | tr -d '\r')"
case "$arch" in
    x86_64) service=romm-sync-steamdeck ;;
    aarch64) service=romm-sync-steamdeck-aarch64 ;;
    *) echo "deck-ci: no release name for a $arch service" >&2; exit 1 ;;
esac
cp build/steamdeck/romm-sync "$OUT/$service"

# Install, update and uninstall with the AppImage just packaged, served as a fake release.
step "Test: the installer"
tools/deck-build.sh tests/steamdeck_install.sh "$OUT"
