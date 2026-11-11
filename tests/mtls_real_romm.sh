#!/usr/bin/env bash
# RommPS (host build) against a real RomM behind a proxy that asks for a client
# certificate (mTLS): Traefik, as Pangolin uses it, and nginx. Everything runs
# in Docker and is removed afterwards; takes two to three minutes.
#
#   tests/mtls_real_romm.sh                      RomM 5.3.1
#   ROMM_IMAGE=rommapp/romm:5.2.0 tests/mtls_real_romm.sh
#   KEEP=1 tests/mtls_real_romm.sh               leave the containers and files for a look
#
# Checked: the refusal without a certificate (and with a stranger's), the .p12
# upload, pairing, a game download, a cover, a save up and a save down, the
# certificate loaded again at start, and nginx's way of refusing.
set -euo pipefail

ROMM_IMAGE="${ROMM_IMAGE:-rommapp/romm:5.3.1}"
TOOLS_IMAGE=rommps-mtls-test:bookworm
GAME="Super Test Quest (USA)"

# ---- inside the tools container --------------------------------------------

if [[ "${1:-}" == --prepare ]]; then # certificates, a game, a cover
    cd /t
    mkdir -p certs library/roms/snes work
    cd certs
    ca() { openssl req -x509 -newkey rsa:2048 -nodes -days 2 -subj "/CN=$1" -keyout "$1.key" -out "$1.crt" 2>/dev/null; }
    signed() { # name CN ca [extensions]
        openssl req -newkey rsa:2048 -nodes -subj "/CN=$2" ${4:+-addext "$4"} -keyout "$1.key" -out "$1.csr" 2>/dev/null
        openssl x509 -req -in "$1.csr" -CA "$3.crt" -CAkey "$3.key" -CAcreateserial -days 2 -copy_extensions copy -out "$1.crt" 2>/dev/null
    }
    ca server-ca; ca client-ca; ca stranger
    signed server romm.test server-ca "subjectAltName=DNS:romm.test,DNS:romm-nginx.test"
    signed client rommps-test client-ca
    openssl pkcs12 -export -inkey client.key -in client.crt -passout pass:geheim -out client.p12
    openssl pkcs12 -export -inkey stranger.key -in stranger.crt -passout pass: -out stranger.p12
    python3 - <<'EOF'
import os, struct, zlib
open("/t/library/roms/snes/Super Test Quest (USA).sfc", "wb").write(os.urandom(512 * 1024))
def chunk(t, d): return struct.pack(">I", len(d)) + t + d + struct.pack(">I", zlib.crc32(t + d))
w = h = 128
rows = b"".join(b"\0" + b"".join(bytes((x * 2, y * 2, 160)) for x in range(w)) for y in range(h))
png = b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", w, h, 8, 2, 0, 0, 0)) + chunk(b"IDAT", zlib.compress(rows)) + chunk(b"IEND", b"")
open("/t/cover.png", "wb").write(png)
EOF
    exit 0
fi

if [[ "${1:-}" == --test ]]; then
    set +e # every step reports, the first failure doesn't end it
    W=/t/work API=http://127.0.0.1:8780 ROMM=https://romm.test NGINX=https://romm-nginx.test
    RC="--cacert /t/certs/server-ca.crt --cert /t/certs/client.crt --key /t/certs/client.key"
    FAILED=0
    ok() { echo "  ok    $*"; }
    bad() { echo "  FAIL  $*"; FAILED=1; }
    jq_() { python3 -c "import json,sys; d=json.load(sys.stdin); print($1)"; }
    status() { curl -sf $API/api/status; }
    synced() { status | jq_ "d['sync']['sync_count']"; }
    wait_sync() { for _ in $(seq 1 60); do
        [[ "$(status | jq_ "-1 if d['sync']['running'] else d['sync']['sync_count']")" -ge "$1" ]] && return 0; sleep 1; done; return 1; }
    sync_now() { local n; n=$(synced); curl -sf -X POST $API/api/sync -d '{}' >/dev/null; wait_sync $((n + 1)) || bad "the sync didn't finish"; }
    pair() { curl -s -X POST $API/api/pair/start -d "{\"server_url\":\"$1\"}"; }
    cert() { curl -s -X POST $API/api/client-cert -d "{\"p12\":\"$(base64 -w0 "$1")\",\"password\":\"$2\"}"; }
    daemon() { ROMM_SYNC_DATA=$W/data ROMM_SYNC_CA=/t/certs/server-ca.crt /src/build/mtls/romm-sync >>$W/daemon.log 2>&1 & PID=$!; sleep 3; }

    # RomM's first user (an admin), as its setup wizard makes it.
    curl -sS $RC -c /tmp/jar -b /tmp/jar -o /dev/null $ROMM/api/heartbeat
    csrf=$(awk '/csrftoken/ {print $7}' /tmp/jar)
    curl -sSf $RC -c /tmp/jar -b /tmp/jar -o /dev/null -X POST $ROMM/api/users -H "X-CSRFToken: $csrf" -H 'Content-Type: application/json' \
        -d '{"username":"admin","email":"admin@romm.test","password":"adminpass1","role":"admin"}'
    RC="$RC -u admin:adminpass1"
    rom=$(curl -sSf $RC "$ROMM/api/roms?limit=1" | jq_ "d['items'][0]['id']")
    platform=$(curl -sSf $RC "$ROMM/api/roms/$rom" | jq_ "d['platform_id']")

    mkdir -p $W/data $W/RA/roms/snes $W/RA/saves
    printf 'savefile_directory = ":/saves"\nsort_savefiles_enable = "true"\n' >$W/RA/retroarch.cfg
    cat >$W/data/config.json <<EOF
{"web_port": 8780, "sync_interval_min": 0, "exit_delay_sec": 1, "profiles_custom": true,
 "profiles": [{"id":"retroarch","name":"RetroArch","enabled":true,"root":"$W/RA",
   "rom_dir":"{root}/roms/{platform}","save_dir":"{root}/saves","state_dir":"{root}/states",
   "bios_dir":"{root}/system","save_exts":".srm","state_exts":".state*","retroarch_cfg":"{root}/retroarch.cfg",
   "platforms":[{"romm":["snes"],"dir":"snes","emulator":"snes9x","core_name":"Snes9x"}]}]}
EOF
    daemon

    echo "1. Traefik, no certificate: RommPS says it wants one"
    r=$(pair $ROMM)
    [[ "$r" == *"asks for a client certificate"* ]] && ok "$(echo "$r" | jq_ "d.get('error', '')")" || bad "$r"

    echo "2. a stranger's certificate: refused, and said so"
    cert /t/certs/stranger.p12 "" >/dev/null
    r=$(pair $ROMM)
    [[ "$r" == *"didn't accept the client certificate"* ]] && ok "$(echo "$r" | jq_ "d.get('error', '')")" || bad "$r"

    echo "3. the right .p12, through the web UI's API"
    r=$(cert /t/certs/client.p12 geheim)
    [[ "$r" == *'"active":true'* && "$r" == *CN=rommps-test* ]] && ok "in use: $(echo "$r" | jq_ "d['info']")" || bad "$r"

    echo "4. pairing, approved in RomM"
    code=$(pair $ROMM | jq_ "d.get('user_code','')")
    scopes=$(curl -sSf $RC "$ROMM/api/auth/device/pending/$code" | jq_ "json.dumps(d['allowed_scopes'])")
    curl -sSf $RC -o /dev/null -X POST $ROMM/api/auth/device/approve -H 'Content-Type: application/json' \
        -d "{\"user_code\":\"$code\",\"approved_scopes\":$scopes}" || bad "RomM didn't take the approval"
    for _ in $(seq 1 30); do [[ "$(status | jq_ "d['paired']")" == True ]] && break; sleep 1; done
    r=$(status | jq_ "d['paired'], d['username'], d['server_version']")
    [[ "$r" == "True admin "* ]] && ok "paired: $r" || bad "not paired: $r"
    curl -sf -o /dev/null -X POST $API/api/setup/finish -d '{"save_sync":true,"conflict_policy":"newest","sync_on_change":false,
        "sync_on_game_exit":false,"sync_on_game_start":false,"notify":false,"sync_interval_min":0}' || bad "setup didn't finish"
    wait_sync 1 || true

    echo "5. a game downloaded"
    curl -sf -o /dev/null -X POST $API/api/download -d "{\"rom_id\":$rom,\"name\":\"Super Test Quest\"}"
    for _ in $(seq 1 60); do
        d=$(curl -sf $API/api/downloads | jq_ "([x['status'] for x in d if x.get('rom_id') == $rom] or [''])[-1]")
        [[ "$d" == done || "$d" == failed ]] && break; sleep 1
    done
    cmp -s "$W/RA/roms/snes/$GAME.sfc" "/t/library/roms/snes/$GAME.sfc" \
        && ok "$(stat -c %s "$W/RA/roms/snes/$GAME.sfc") bytes, the same as RomM's" || bad "download: $d"

    echo "6. a cover"
    curl -sSf $RC -o /dev/null -X PUT "$ROMM/api/roms/$rom" -F "artwork=@/t/cover.png;type=image/png" || bad "RomM didn't take the cover"
    p=$(curl -sf "$API/api/roms?platform_id=$platform&limit=1" | jq_ "d['items'][0].get('cover','')")
    q=$(python3 -c "import urllib.parse,sys; print(urllib.parse.quote(sys.argv[1]))" "$p")
    curl -sf -o /tmp/via-rommps "$API/cover?p=$q" || bad "RommPS didn't serve the cover ($p)"
    curl -sSf $RC -o /tmp/from-romm "$ROMM${p// /%20}" # RomM's paths have spaces ("?ts=2026-11-04 16:54:29")
    [[ -s /tmp/via-rommps ]] && cmp -s /tmp/via-rommps /tmp/from-romm \
        && ok "$(stat -c %s /tmp/via-rommps) bytes, as RomM serves it ($p)" || bad "cover: '$p'"

    echo "7. a save up to RomM"
    mkdir -p $W/RA/saves/Snes9x
    echo "save-from-the-console" >"$W/RA/saves/Snes9x/$GAME.srm"
    sync_now
    id=$(curl -sSf $RC "$ROMM/api/saves?rom_id=$rom" | jq_ "(d.get('items', d) if isinstance(d, dict) else d)[-1]['id']")
    [[ "$(curl -sSf $RC "$ROMM/api/saves/$id/content")" == save-from-the-console ]] && ok "on RomM as save $id" || bad "save $id on RomM"

    echo "8. a newer save on RomM down to the console"
    sleep 2
    new="save-from-elsewhere-$(date +%s%N)"; echo "$new" >/tmp/new.srm
    curl -sSf $RC -o /dev/null -X POST "$ROMM/api/saves?rom_id=$rom&emulator=snes9x&slot=autosave" -F "saveFile=@/tmp/new.srm;filename=$GAME.srm"
    sync_now
    [[ "$(cat "$W/RA/saves/Snes9x/$GAME.srm")" == "$new" ]] && ok "the console has it" || bad "the console's save: $(cat "$W/RA/saves/Snes9x/$GAME.srm")"

    echo "9. restarted: the certificate comes from config.json"
    kill $PID; wait $PID 2>/dev/null || true
    daemon
    r=$(curl -sf $API/api/client-cert)
    [[ "$r" == *'"active":true'* && "$r" == *'"file":"client.p12"'* ]] && ok "loaded" || bad "$r"
    sync_now
    r=$(status | jq_ "d.get('server_trouble', '') + d['sync']['history'][-1].get('error', '')")
    [[ -z "$r" ]] && ok "and a sync goes through" || bad "$r"

    echo "10. the certificate removed: RommPS says why RomM can't be reached"
    curl -sf -o /dev/null -X POST $API/api/client-cert/remove
    curl -sf -o /dev/null -X POST $API/api/sync -d '{}'; sleep 4
    r=$(status | jq_ "d.get('server_trouble', '')")
    [[ "$r" == *"asks for a client certificate"* ]] && ok "$r" || bad "server_trouble: $r"

    echo "11. nginx, which answers with a 400 page instead of a TLS alert"
    r=$(pair $NGINX); [[ "$r" == *"asks for a client certificate"* ]] && ok "no certificate: said so" || bad "$r"
    cert /t/certs/stranger.p12 "" >/dev/null
    r=$(pair $NGINX); [[ "$r" == *"didn't accept"* ]] && ok "a stranger's: said so" || bad "$r"
    cert /t/certs/client.p12 geheim >/dev/null
    r=$(pair $NGINX); [[ "$r" == *pending* ]] && ok "the right one: pairing starts" || bad "$r"

    kill $PID 2>/dev/null || true
    if [[ $FAILED != 0 ]]; then echo "--- daemon log"; tail -40 $W/daemon.log; echo FAIL; exit 1; fi
    echo PASS
    exit 0
fi

# ---- on the host: Docker ---------------------------------------------------

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
T="$(mktemp -d)"
NET="rommps-mtls-$$"
C=("$NET-db" "$NET-romm" "$NET-traefik" "$NET-nginx")
cleanup() {
    if [[ -n "${KEEP:-}" ]]; then echo "kept: containers ${C[*]}, files in $T"; return; fi
    docker rm -fv "${C[@]}" >/dev/null 2>&1 || true
    docker network rm "$NET" >/dev/null 2>&1 || true
    rm -rf "$T"
}
trap cleanup EXIT
tools() { docker run --rm -u "$(id -u):$(id -g)" -e HOME=/tmp -v "$ROOT":/src -v "$T":/t -w /src "$@"; }

echo "build: tools image and RommPS's host build"
docker build -q -t $TOOLS_IMAGE - >/dev/null <<'EOF'
FROM debian:bookworm
RUN apt-get update -qq && apt-get install -y -qq --no-install-recommends gcc libc6-dev make python3 pkg-config \
        libcurl4-openssl-dev libssl-dev zlib1g-dev curl openssl ca-certificates >/dev/null && rm -rf /var/lib/apt/lists/*
EOF
tools $TOOLS_IMAGE make -s host HOST_BIN=build/mtls/romm-sync >/dev/null 2>&1 || { tools $TOOLS_IMAGE make host HOST_BIN=build/mtls/romm-sync | tail -20; exit 1; }
tools $TOOLS_IMAGE bash tests/mtls_real_romm.sh --prepare

cat >"$T/traefik.yml" <<'EOF'
entryPoints: {websecure: {address: ":443"}}
providers: {file: {filename: /etc/traefik/dynamic.yml}}
EOF
cat >"$T/dynamic.yml" <<'EOF'
tls:
  certificates: [{certFile: /certs/server.crt, keyFile: /certs/server.key}]
  options:
    default:
      clientAuth: {caFiles: [/certs/client-ca.crt], clientAuthType: RequireAndVerifyClientCert}
http:
  routers: {romm: {rule: "Host(`romm.test`)", entryPoints: [websecure], service: romm, tls: {}}}
  services: {romm: {loadBalancer: {servers: [{url: "http://romm:8080"}]}}}
EOF
cat >"$T/nginx.conf" <<'EOF'
events {}
http {
  client_max_body_size 0;
  server {
    listen 443 ssl;
    ssl_certificate /certs/server.crt;
    ssl_certificate_key /certs/server.key;
    ssl_client_certificate /certs/client-ca.crt;
    ssl_verify_client on;
    location / { proxy_pass http://romm:8080; proxy_set_header Host $host; proxy_set_header X-Forwarded-Proto https; }
  }
}
EOF

echo "start: MariaDB, $ROMM_IMAGE, Traefik and nginx with mTLS"
docker network create "$NET" >/dev/null
docker run -d --name "$NET-db" --network "$NET" -e MARIADB_ROOT_PASSWORD=root -e MARIADB_DATABASE=romm \
    -e MARIADB_USER=romm -e MARIADB_PASSWORD=romm mariadb:11 >/dev/null
for _ in $(seq 1 60); do docker exec "$NET-db" mariadb -uromm -promm -e 'select 1' romm >/dev/null 2>&1 && break; sleep 1; done
docker run -d --name "$NET-romm" --network "$NET" --network-alias romm -e DB_HOST="$NET-db" -e DB_NAME=romm -e DB_USER=romm \
    -e DB_PASSWD=romm -e ENABLE_SCHEDULED_RESCAN=true -e ROMM_AUTH_SECRET_KEY="$(head -c 32 /dev/urandom | od -An -tx1 | tr -d ' \n')" \
    -v "$T/library":/romm/library:ro -v /romm/resources -v /romm/assets -v /romm/config "$ROMM_IMAGE" >/dev/null
docker run -d --name "$NET-traefik" --network "$NET" --network-alias romm.test -v "$T/traefik.yml":/etc/traefik/traefik.yml:ro \
    -v "$T/dynamic.yml":/etc/traefik/dynamic.yml:ro -v "$T/certs":/certs:ro traefik:v3.5 >/dev/null
docker run -d --name "$NET-nginx" --network "$NET" --network-alias romm-nginx.test -v "$T/nginx.conf":/etc/nginx/nginx.conf:ro \
    -v "$T/certs":/certs:ro nginx:1.27-alpine >/dev/null
for i in $(seq 1 100); do
    docker exec "$NET-romm" sh -c 'wget -qO- http://127.0.0.1:8080/api/heartbeat' >/dev/null 2>&1 && break
    [[ $i == 100 ]] && { docker logs --tail 30 "$NET-romm"; echo "RomM didn't start"; exit 1; }
    sleep 2
done

echo "scan: RomM finds the game"
docker exec -w /backend "$NET-romm" /src/.venv/bin/python -c "
import asyncio
from tasks.scheduled.scan_library import ScanLibraryTask
s = asyncio.run(ScanLibraryTask().run())
assert s['new_roms'] == 1, s" >/dev/null 2>&1 || { echo "RomM's scan didn't find the game"; exit 1; }

echo "test"
tools --network "$NET" $TOOLS_IMAGE bash tests/mtls_real_romm.sh --test
