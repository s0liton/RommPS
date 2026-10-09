#!/usr/bin/env bash
# Builds the app's API client for the Mac/Linux host and tests it against the
# host build of RomM Sync.   make host && platform/ps5/app/tests/api-test.sh
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../../../.." && pwd)"
T="$(mktemp -d)"; PORT=8783
trap 'kill $(jobs -p) 2>/dev/null || true; rm -rf "$T"' EXIT
mkdir -p "$T/data"; echo "{\"web_port\": $PORT, \"sync_interval_min\": 0, \"update_check\": false}" > "$T/data/config.json"
ROMM_SYNC_DATA="$T/data" ROMM_SYNC_HOMEBREW="$T/none" "$ROOT/build/host/romm-sync" > "$T/daemon.log" 2>&1 &
for _ in $(seq 1 40); do curl -sf "http://127.0.0.1:$PORT/api/status" >/dev/null && break; sleep 0.25; done
cc -O2 -c -o "$T/cJSON.o" "$ROOT/third_party/cJSON.c"
c++ -std=c++17 -O2 -Wall -Wextra -I"$ROOT/platform/ps5/app/src" -I"$ROOT/third_party" -o "$T/api_test" \
    "$ROOT/platform/ps5/app/tests/api_test.cpp" "$ROOT/platform/ps5/app/src/rommps_api.cpp" "$T/cJSON.o" -lpthread
"$T/api_test" $PORT
