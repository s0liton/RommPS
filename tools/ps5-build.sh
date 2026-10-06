#!/usr/bin/env bash
# Build romm-sync for PS5 inside the Docker SDK image. Same options as
# tools/console-build.sh: --rebuild, --shell, --ca [DEST], --test, or make args.
exec "$(dirname "${BASH_SOURCE[0]}")/console-build.sh" ps5 "$@"
