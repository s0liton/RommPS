#!/usr/bin/env bash
# Static checks, run in the Deck build image (make lint, or tools/deck-build.sh tools/lint.sh):
#   formatting   clang-format on the files already in the repository's style (FORMATTED below)
#   C            cppcheck over src/ and the platform layers (tools/cppcheck-suppressions.txt)
#   shell        shellcheck on the Deck's scripts
#   Python       a syntax check of the Deck's tools
#   --tidy       also clang-tidy (.clang-tidy) on the Deck app and the C core: slower, and
#                advisory (it reports, it doesn't fail the run)
# Exits non-zero when a required check fails.
set -uo pipefail
cd "$(dirname "${BASH_SOURCE[0]}")/.." || exit 1
FORMATTED=(platform/steamdeck/platform.c platform/steamdeck/app/src/*.cpp platform/steamdeck/app/src/*.hpp
           platform/steamdeck/app/src/*.h)
SCRIPTS=(platform/steamdeck/*.sh tools/deck-build.sh tools/lint.sh tests/steamdeck.sh tests/steamdeck_duckstation.sh)
PYTHON=(platform/steamdeck/add-to-steam.py platform/steamdeck/tools/make-steam-art.py)
fail=0

echo "== formatting"
clang-format-16 --dry-run --Werror "${FORMATTED[@]}" || fail=1

echo "== cppcheck"
cppcheck --quiet --enable=warning,portability --std=c11 -D__linux__ --error-exitcode=1 \
    --suppressions-list=tools/cppcheck-suppressions.txt \
    -Isrc -Ithird_party -Ibuild/gen -Ibuild/steamdeck/gen \
    src platform/steamdeck/platform.c platform/host/platform.c || fail=1

echo "== shellcheck"
shellcheck -S warning "${SCRIPTS[@]}" || fail=1

echo "== python"
python3 -m py_compile "${PYTHON[@]}" || fail=1

if [[ "${1:-}" == "--tidy" ]]; then
    echo "== clang-tidy (advisory)"
    cmake -S platform/steamdeck/app -B build/steamdeck/app -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null
    for f in platform/steamdeck/app/src/*.cpp platform/ps5/app/rommps/examples/rommps/kit/*.cpp; do
        clang-tidy-16 -p build/steamdeck/app --quiet "$f" 2>/dev/null
    done
    for f in src/*.c platform/steamdeck/platform.c; do
        clang-tidy-16 --quiet "$f" -- -std=gnu11 -Isrc -Ithird_party -Ibuild/gen -Ibuild/steamdeck/gen \
            -Ithird_party/libchdr/include -Ithird_party/sigil/include -Ithird_party/sigil/src -DCHDR_SYSTEM_ZLIB 2>/dev/null
    done
fi

[[ $fail == 0 ]] && echo "lint: clean" || echo "lint: problems above"
exit $fail
