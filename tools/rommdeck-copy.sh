#!/usr/bin/env bash
# Copies what RommDeck is built from into a checkout of s0liton/RommDeck, which
# builds and releases it from there. The code is written here and only here:
# the copy goes one way, and nobody edits it by hand.
#
#   tools/rommdeck-copy.sh DEST [COMMIT]     COMMIT defaults to HEAD
#
# DEST is either a new or empty folder (ci.yml builds the Deck from one, so a
# file missing from PATHS fails here first) or a RommDeck checkout. In a
# checkout, every tracked file outside KEEP is replaced, so what's deleted
# here is deleted there. Nothing is committed: rommdeck-copy.yml does that.
#
# SOURCE.txt, at the top of the copy, names the commit it was taken from;
# tools/deck-build.sh puts that in the build, so a log says which code ran.
set -euo pipefail
ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DEST="${1:?usage: rommdeck-copy.sh DEST [COMMIT]}"
REV="$(git -C "$ROOT" rev-parse --verify "${2:-HEAD}^{commit}")"

# Everything `tools/deck-ci.sh` reads. RommPS's screens and its API layer are
# in platform/ps5/app, the kit's setup script and the menu sounds with them.
PATHS=(
    .gitignore LICENSE Makefile
    src ui third_party
    platform/retroarch_cores.json platform/steamdeck platform/ps5/app
    tools/deck-build.sh tools/deck-ci.sh tools/embed.py tools/font_css.py tools/release-sign.c
    'tests/steamdeck*.sh' tests/mock_romm.py tests/make_disc.py
    tests/fixtures/deck-emudeck tests/fixtures/vita3k-config.yml
    docs/releases/rommdeck
)
# RommDeck's own: its README and screenshots, its workflows and templates.
KEEP='^(README\.md|assets/|\.github/)'

# Archived before DEST is touched: a path this commit doesn't have stops here.
ARCHIVE="$(mktemp)"
trap 'rm -f "$ARCHIVE"' EXIT
git -C "$ROOT" archive "$REV" -- "${PATHS[@]}" > "$ARCHIVE"

mkdir -p "$DEST"
DEST="$(cd "$DEST" && pwd)"
if [[ -n "$(ls -A "$DEST")" ]]; then
    top="$(git -C "$DEST" rev-parse --show-toplevel 2>/dev/null || true)"
    if [[ "$top" != "$DEST" || "$DEST" == "$ROOT" ]]; then
        echo "rommdeck-copy.sh: $DEST is neither empty nor a RommDeck checkout" >&2
        exit 1
    fi
    git -C "$DEST" ls-files -z | { grep -zvE "$KEEP" || true; } | xargs -0 -r git -C "$DEST" rm -q --
fi

tar -x -C "$DEST" -f "$ARCHIVE"
cat > "$DEST/SOURCE.txt" <<EOF
RommDeck's code is a copy of RommPS's, https://github.com/s0liton/RommPS, where it's written.
Changes and pull requests go there; anything edited here is replaced by the next copy.
commit $REV
EOF
