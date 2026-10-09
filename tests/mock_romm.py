#!/usr/bin/env python3
"""In-memory stand-in for the RomM endpoints romm-sync uses, for tests.
It is not RomM. Please don't point anything real at it.

Negotiate follows backend/endpoints/sync.py from RomM 5.2.0.

    python3 tests/mock_romm.py 8899 [cert.pem]   (with a cert+key PEM, serves HTTPS)

POST /_admin/save adds a save as if another device uploaded it.
GET /_admin/dump returns everything the mock holds.
GET /_github/release stands in for GitHub's latest-release API: it serves
release.json from $MOCK_RELEASE_DIR, and /_github/assets/<name> the files next to it.
"""
import email.parser
import email.policy
import hashlib
import json
import os
import sys
import threading
from datetime import datetime, timezone
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from urllib.parse import parse_qs, unquote, urlparse

LOCK = threading.Lock()
NOW = lambda: datetime.now(timezone.utc)

DB = {
    "platforms": [
        {"id": 1, "slug": "snes", "fs_slug": "snes", "name": "Super Nintendo", "display_name": "SNES",
         "rom_count": 3, "firmware_count": 1, "url_logo": ""},
        {"id": 2, "slug": "ps2", "fs_slug": "ps2", "name": "PlayStation 2", "display_name": "PS2",
         "rom_count": 2, "firmware_count": 0, "url_logo": ""},
        {"id": 3, "slug": "psp", "fs_slug": "psp", "name": "PlayStation Portable", "display_name": "PSP",
         "rom_count": 1, "firmware_count": 0, "url_logo": ""},
        {"id": 4, "slug": "ngc", "fs_slug": "ngc", "name": "GameCube", "display_name": "GameCube",
         "rom_count": 1, "firmware_count": 0, "url_logo": ""},
        {"id": 5, "slug": "psx", "fs_slug": "psx", "name": "PlayStation", "display_name": "PlayStation",
         "rom_count": 1, "firmware_count": 0, "url_logo": ""},
        {"id": 6, "slug": "n64", "fs_slug": "n64", "name": "Nintendo 64", "display_name": "N64",
         "rom_count": 2, "firmware_count": 0, "url_logo": ""},
    ],
    "roms": {
        10: {"id": 10, "platform_id": 1, "name": "Chrono Trigger", "fs_name": "Chrono Trigger (USA).sfc"},
        11: {"id": 11, "platform_id": 1, "name": "Super Metroid", "fs_name": "Super Metroid (USA).sfc",
             "summary": "A test summary.",
             "metadatum": {"first_release_date": 764553600000, "publishers": ["Nintendo"], "developers": ["Nintendo R&D1"],
                           "genres": ["Platform", "Adventure"], "player_count": "1", "average_rating": 92.5},
             "hltb_metadata": {"main_story": 28800, "release_year": 1994},
             "rom_user": {"last_played": "2026-10-01T20:00:00", "status": "finished", "completion": 100, "rating": 9}},
        # One of two versions RomM has of the game.
        12: {"id": 12, "platform_id": 1, "name": "EarthBound", "fs_name": "EarthBound (USA).sfc",
             "regions": ["USA"], "revision": "1", "tags": ["!", "M3", "Beta"], "fs_extension": "sfc",
             "sibling_roms": [{"id": 99, "name": "EarthBound", "is_main_sibling": True}]},
        # A frontend's file a RomM scan lists as a game.
        13: {"id": 13, "platform_id": 1, "name": "systeminfo", "fs_name": "systeminfo.txt"},
        # .md is a Mega Drive ROM, not a readme.
        14: {"id": 14, "platform_id": 1, "name": "Mega Test", "fs_name": "Mega Test (USA).md"},
        20: {"id": 20, "platform_id": 2, "name": "Okami", "fs_name": "Okami.iso"},
        21: {"id": 21, "platform_id": 2, "name": "Other", "fs_name": "Other.iso"},
        30: {"id": 30, "platform_id": 3, "name": "Crisis Core", "fs_name": "Crisis Core (USA).iso"},
        40: {"id": 40, "platform_id": 4, "name": "Wind Waker", "fs_name": "Wind Waker (USA).iso"},
        50: {"id": 50, "platform_id": 5, "name": "Test Quest", "fs_name": "Test Quest (USA).bin"},
        60: {"id": 60, "platform_id": 6, "name": "Test Racer", "fs_name": "Test Racer (U).z64"},
        61: {"id": 61, "platform_id": 6, "name": "Other Racer", "fs_name": "Other Racer (U).z64"},
    },
    "firmware": [{"id": 1, "platform_id": 1, "file_name": "bsx.bin", "content": b"BSXBIOS"}],
    "saves": {},    # id -> save dict (+ "content")
    "states": {},
    "cards": {},    # id -> {"id", "name", "emulator", "versions": [bytes]}
    "device_syncs": {},  # (device_id, save_id) -> last_synced_at
    "sessions": {},
    "pending": {},  # device_code -> {"polls": n, "user_code":..}
    "play_sessions": [{"id": 1, "rom_id": 11, "duration_ms": 3600000}, {"id": 2, "rom_id": 11, "duration_ms": 1800000}],
    "cover_ts": {},   # rom id -> its cover's change stamp
    "asset_hits": 0,  # cover requests served
    "next_id": 100,
}
TOKEN = "rmm_" + "a" * 64
DEVICE_ID = "3f0e8c1a-0000-4000-8000-000000000001"


def next_id():
    DB["next_id"] += 1
    return DB["next_id"]


def rom_public(r):
    p = next(p for p in DB["platforms"] if p["id"] == r["platform_id"])
    stem = r["fs_name"].rsplit(".", 1)[0]
    content = ("ROM:" + r["fs_name"]).encode()
    return {**r, "fs_name_no_ext": stem, "fs_size_bytes": len(content), "platform_slug": p["slug"],
            "platform_fs_slug": p["fs_slug"], "has_multiple_files": False, "files": [],
            "md5_hash": hashlib.md5(content).hexdigest(),
            # Like RomM's: the cover's path carries a stamp that changes with it.
            "path_cover_small": "/assets/romm/resources/roms/%d/%d/cover/small.png?ts=%s" % (
                r["platform_id"], r["id"], DB["cover_ts"].get(r["id"], "2026-01-01 00:00:00"))}


def save_public(s):
    return {k: (v.isoformat() if isinstance(v, datetime) else v) for k, v in s.items() if k != "content"}


def compare(client_hash, client_ts, server_hash, server_ts, synced):
    if client_hash and server_hash and client_hash == server_hash:
        return "no_op", "Content is identical"
    if synced:
        cc, sc = client_ts > synced, server_ts > synced
        if cc and sc:
            return "conflict", "Both sides changed since last sync"
        if cc:
            return "upload", "Client save is newer than last sync"
        if sc:
            return "download", "Server save is newer than last sync"
        return "no_op", "No changes since last sync"
    if client_ts > server_ts:
        return "upload", "Client save is newer (no sync history)"
    if server_ts > client_ts:
        return "download", "Server save is newer (no sync history)"
    return ("conflict", "Same timestamp but different content") if client_hash != server_hash else ("no_op", "")


def parse_ts(s):
    return datetime.fromisoformat(s.replace("Z", "+00:00")).astimezone(timezone.utc)


def content_hash(content):
    import io, zipfile
    buf = io.BytesIO(content)
    if zipfile.is_zipfile(buf):
        with zipfile.ZipFile(buf) as zf:
            lines = sorted(f"{i.filename}:{hashlib.md5(zf.read(i)).hexdigest()}" for i in zf.infolist() if not i.is_dir())
        return hashlib.md5("\n".join(lines).encode()).hexdigest()
    return hashlib.md5(content).hexdigest()


def new_save(rom_id, slot, emulator, file_name, content, device_id=None):
    sid = next_id()
    now = NOW()
    tagged = file_name
    if slot:
        stem, _, ext = file_name.rpartition(".")
        tagged = f"{stem} [{now.strftime('%Y-%m-%d_%H-%M-%S-%f')}].{ext}"
    s = {"id": sid, "rom_id": rom_id, "slot": slot, "emulator": emulator, "file_name": tagged,
         "file_size_bytes": len(content), "content_hash": content_hash(content),
         "created_at": now, "updated_at": now, "content": content}
    DB["saves"][sid] = s
    if device_id:
        DB["device_syncs"][(device_id, sid)] = now
    return s


class H(BaseHTTPRequestHandler):
    def log_message(self, fmt, *a):
        sys.stderr.write("[mock] " + (fmt % a) + "\n")

    def reply(self, code, obj=None, raw=None, ctype="application/json"):
        body = raw if raw is not None else json.dumps(obj).encode()
        self.send_response(code)
        self.send_header("Content-Type", ctype)
        self.send_header("Content-Length", str(len(body)))
        self.end_headers()
        self.wfile.write(body)

    def body(self):
        n = int(self.headers.get("Content-Length") or 0)
        return self.rfile.read(n) if n else b""

    def json_body(self):
        b = self.body()
        return json.loads(b) if b else {}

    def multipart_file(self, field):
        raw = self.body()
        msg = email.parser.BytesParser(policy=email.policy.default).parsebytes(
            b"Content-Type: " + self.headers["Content-Type"].encode() + b"\r\n\r\n" + raw)
        for part in msg.iter_parts():
            if part.get_param("name", header="content-disposition") == field:
                return part.get_filename(), part.get_payload(decode=True)
        return None, None

    def authed(self):
        if self.headers.get("Authorization") != "Bearer " + TOKEN:
            self.reply(401, {"detail": "Not authenticated"})
            return False
        return True

    def route(self, method):
        u = urlparse(self.path)
        q = {k: v for k, v in parse_qs(u.query).items()}
        qs = lambda k, d=None: q.get(k, [d])[0]
        p = u.path
        parts = [unquote(x) for x in p.strip("/").split("/")]
        with LOCK:
            # no auth needed
            if p.startswith("/assets/"):
                DB["asset_hits"] += 1
                return self.reply(200, raw=b"\x89PNG-test-" + p.encode(), ctype="image/png")
            if p == "/_admin/cover_ts" and method == "POST":
                DB["cover_ts"][int(self.json_body()["rom_id"])] = "2026-02-02 00:00:00"
                return self.reply(200, {"ok": True})
            if p == "/api/heartbeat":
                return self.reply(200, {"SYSTEM": {"VERSION": "5.3.1"}})
            if p == "/api/auth/device/init" and method == "POST":
                b = self.json_body()
                code = "dc-" + str(next_id())
                DB["pending"][code] = {"polls": 0, "user_code": "ABCD2345", "client": b.get("client")}
                return self.reply(201, {"device_code": code, "user_code": "ABCD2345", "verification_path": "/pair/device",
                                        "verification_path_complete": "/pair/device?user_code=ABCD2345",
                                        "expires_in": 600, "interval": 1})
            if p == "/api/auth/device/token" and method == "POST":
                b = self.json_body()
                pend = DB["pending"].get(b.get("device_code"))
                if not pend:
                    return self.reply(400, {"detail": "expired_token"})
                pend["polls"] += 1
                if pend["polls"] < 2:
                    return self.reply(400, {"detail": "authorization_pending"})
                return self.reply(200, {"access_token": TOKEN, "device_id": DEVICE_ID, "scopes": [], "expires_at": None})
            if parts[0] == "_github":
                d = os.environ.get("MOCK_RELEASE_DIR", "")
                name = "release.json" if p == "/_github/release" else parts[-1] if parts[1:2] == ["assets"] else ""
                f = os.path.join(d, os.path.basename(name)) if d and name else ""
                if not f or not os.path.isfile(f):
                    return self.reply(404, {"message": "Not Found"})
                return self.reply(200, raw=open(f, "rb").read(), ctype="application/octet-stream")
            if p == "/_admin/save" and method == "POST":
                b = self.json_body()
                import base64
                raw = base64.b64decode(b["content_b64"]) if "content_b64" in b else b["content"].encode()
                s = new_save(b["rom_id"], b.get("slot"), b.get("emulator", "other-emu"), b.get("file_name", "x.srm"), raw)
                return self.reply(200, save_public(s))
            if p == "/_admin/state" and method == "POST":
                b = self.json_body()
                sid = next_id()
                DB["states"][sid] = {"id": sid, "rom_id": b["rom_id"], "file_name": b["file_name"], "emulator": b.get("emulator"),
                                     "file_size_bytes": len(b["content"]), "updated_at": NOW(), "content": b["content"].encode()}
                return self.reply(200, save_public(DB["states"][sid]))
            if p == "/_admin/save_content":
                sv = DB["saves"][int(qs("id"))]
                return self.reply(200, raw=sv["content"], ctype="application/octet-stream")
            if p == "/_admin/dump":
                return self.reply(200, {"saves": [save_public(s) | {"content": s["content"].decode("latin1")} for s in DB["saves"].values()],
                                        "states": [save_public(s) for s in DB["states"].values()],
                                        "sessions": DB["sessions"],
                                        "asset_hits": DB["asset_hits"],
                                        "cards": [{"name": c["name"], "emulator": c["emulator"], "versions": len(c["versions"])} for c in DB["cards"].values()]})
            if not self.authed():
                return
            dev = qs("device_id")
            # everything below needs the token
            if p == "/api/users/me":
                return self.reply(200, {"id": 1, "username": "tester"})
            if p == "/api/platforms":
                return self.reply(200, DB["platforms"])
            if p == "/api/play-sessions" and method == "GET":
                rid = int(qs("rom_id", 0) or 0)
                return self.reply(200, [x for x in DB["play_sessions"] if not rid or x["rom_id"] == rid])
            if len(parts) == 3 and parts[:2] == ["api", "platforms"]:
                return self.reply(200, next(x for x in DB["platforms"] if x["id"] == int(parts[2])))
            if p == "/api/roms":
                ids = [int(x) for x in q.get("platform_ids", [])]
                term = (qs("search_term") or "").lower()
                items = sorted((rom_public(r) for r in DB["roms"].values()
                                if (not ids or r["platform_id"] in ids) and term in r["name"].lower() + r["fs_name"].lower()),
                               key=lambda r: r["name"].lower())
                char_index = {}
                for i, r in enumerate(items):
                    c = r["name"][:1].lower()
                    char_index.setdefault(c if c.isalpha() else "0", i)
                off, lim = int(qs("offset", 0)), int(qs("limit", 50))
                return self.reply(200, {"items": items[off:off + lim], "total": len(items), "limit": lim, "offset": off,
                                        "char_index": char_index})
            if len(parts) == 3 and parts[:2] == ["api", "roms"]:
                return self.reply(200, rom_public(DB["roms"][int(parts[2])]))
            if len(parts) == 5 and parts[:2] == ["api", "roms"] and parts[3] == "content":
                r = DB["roms"][int(parts[2])]
                return self.reply(200, raw=("ROM:" + r["fs_name"]).encode(), ctype="application/octet-stream")
            if p == "/api/firmware":
                pid = int(qs("platform_id", 0))
                return self.reply(200, [{"id": f["id"], "file_name": f["file_name"], "file_size_bytes": len(f["content"])}
                                        for f in DB["firmware"] if f["platform_id"] == pid])
            if len(parts) == 5 and parts[:2] == ["api", "firmware"]:
                f = next(f for f in DB["firmware"] if f["id"] == int(parts[2]))
                return self.reply(200, raw=f["content"], ctype="application/octet-stream")
            if p == "/api/memory-cards" and method == "GET":
                emu = qs("emulator")
                return self.reply(200, [{k: v for k, v in c.items() if k != "versions"} for c in DB["cards"].values()
                                        if not emu or c["emulator"] == emu])
            if p == "/api/memory-cards" and method == "POST":
                b = self.json_body()
                cid = next_id()
                DB["cards"][cid] = {"id": cid, "name": b["name"], "emulator": b["emulator"], "slot": 1, "versions": []}
                return self.reply(201, {k: v for k, v in DB["cards"][cid].items() if k != "versions"})
            if len(parts) == 4 and parts[:2] == ["api", "memory-cards"] and parts[3] == "versions":
                card = DB["cards"].get(int(parts[2]))
                if not card:
                    return self.reply(404, {"detail": "not found"})
                name, content = self.multipart_file("cardFile")
                import io, zipfile
                if not zipfile.is_zipfile(io.BytesIO(content)):
                    return self.reply(400, {"detail": "Memory card archive rejected, not a readable zip archive"})
                card["versions"].append(content)
                return self.reply(200, {"id": next_id(), "memory_card_id": card["id"], "file_name": name,
                                        "file_size_bytes": len(content)})
            if len(parts) == 4 and parts[:2] == ["api", "memory-cards"] and parts[3] == "content":
                card = DB["cards"].get(int(parts[2]))
                if not card or not card["versions"]:
                    return self.reply(404, {"detail": "Memory card has no stored data yet"})
                return self.reply(200, raw=card["versions"][-1], ctype="application/zip")
            if p == "/api/sync/negotiate":
                return self.negotiate(self.json_body())
            if len(parts) == 5 and parts[:3] == ["api", "sync", "sessions"] and parts[4] == "complete":
                DB["sessions"][parts[3]]["complete"] = self.json_body()
                return self.reply(200, {"session": DB["sessions"][parts[3]]})
            if p == "/api/saves" and method == "POST":
                name, content = self.multipart_file("saveFile")
                rom_id, slot = int(qs("rom_id")), qs("slot")
                if dev and slot and qs("overwrite", "false") != "true":
                    cur = self.latest(rom_id, slot)
                    synced = DB["device_syncs"].get((dev, cur["id"])) if cur else None
                    if cur and (not synced or cur["updated_at"] > synced):
                        return self.reply(409, {"detail": "Slot has a newer save since your last sync"})
                s = new_save(rom_id, slot, qs("emulator"), name, content, dev)
                if slot and qs("autocleanup") == "true":
                    keep = int(qs("autocleanup_limit", 10))
                    rows = sorted((x for x in DB["saves"].values() if x["rom_id"] == rom_id and x["slot"] == slot),
                                  key=lambda x: x["updated_at"], reverse=True)
                    for old in rows[keep:]:
                        del DB["saves"][old["id"]]
                return self.reply(201, save_public(s))
            if p == "/api/saves" and method == "GET":
                rid = qs("rom_id")
                return self.reply(200, [save_public(x) for x in DB["saves"].values() if not rid or x["rom_id"] == int(rid)])
            if len(parts) == 3 and parts[:2] == ["api", "saves"] and method == "GET":
                return self.reply(200, save_public(DB["saves"][int(parts[2])]))
            if p == "/api/states" and method == "GET":
                rid = qs("rom_id")
                return self.reply(200, [save_public(x) for x in DB["states"].values() if not rid or x["rom_id"] == int(rid)])
            if len(parts) == 3 and parts[:2] == ["api", "states"] and method == "GET":
                return self.reply(200, save_public(DB["states"][int(parts[2])]))
            if len(parts) == 4 and parts[:2] == ["api", "states"] and parts[3] == "content":
                return self.reply(200, raw=DB["states"][int(parts[2])]["content"], ctype="application/octet-stream")
            if len(parts) == 4 and parts[:2] == ["api", "saves"] and parts[3] == "content":
                s = DB["saves"][int(parts[2])]
                if dev:
                    DB["device_syncs"][(dev, s["id"])] = NOW()
                return self.reply(200, raw=s["content"], ctype="application/octet-stream")
            if len(parts) == 4 and parts[:2] == ["api", "saves"] and parts[3] == "downloaded":
                return self.reply(200, {"ok": True})
            if p == "/api/states" and method == "POST":
                name, content = self.multipart_file("stateFile")
                same = [x for x in DB["states"].values() if x["rom_id"] == int(qs("rom_id")) and x["file_name"] == name]
                if same:
                    same[0].update(content=content, file_size_bytes=len(content), updated_at=NOW())
                    return self.reply(201, save_public(same[0]))
                sid = next_id()
                DB["states"][sid] = {"id": sid, "rom_id": int(qs("rom_id")), "file_name": name,
                                     "emulator": qs("emulator"), "file_size_bytes": len(content), "updated_at": NOW(), "content": content}
                return self.reply(201, save_public(DB["states"][sid]))
            if len(parts) == 3 and parts[:2] == ["api", "states"] and method == "PUT":
                sid = int(parts[2])
                if sid not in DB["states"]:
                    return self.reply(404, {"detail": "not found"})
                name, content = self.multipart_file("stateFile")
                DB["states"][sid].update(content=content, file_size_bytes=len(content), updated_at=NOW())
                return self.reply(200, save_public(DB["states"][sid]))
        self.reply(404, {"detail": "Not found: " + p})

    def latest(self, rom_id, slot):
        c = [s for s in DB["saves"].values() if s["rom_id"] == rom_id and s["slot"] == slot]
        return max(c, key=lambda s: s["updated_at"]) if c else None

    def negotiate(self, b):
        dev = b.get("device_id")
        sess = str(next_id())
        server_map = {}
        for s in DB["saves"].values():
            if s["slot"] is None:
                continue
            k = (s["rom_id"], s["slot"])
            if k not in server_map or s["updated_at"] > server_map[k]["updated_at"]:
                server_map[k] = s
        ops, matched = [], set()
        for cs in b["saves"]:
            ss = server_map.get((cs["rom_id"], cs.get("slot")))
            if ss is None:
                ops.append({"action": "upload", "rom_id": cs["rom_id"], "save_id": None, "file_name": cs["file_name"],
                            "slot": cs.get("slot"), "emulator": cs.get("emulator"), "reason": "not on server"})
                continue
            matched.add(ss["id"])
            action, reason = compare(cs.get("content_hash"), parse_ts(cs["updated_at"]), ss["content_hash"],
                                     ss["updated_at"], DB["device_syncs"].get((dev, ss["id"])))
            ops.append({"action": action, "rom_id": ss["rom_id"], "save_id": ss["id"], "file_name": ss["file_name"],
                        "slot": ss["slot"], "emulator": ss["emulator"], "reason": reason,
                        "server_updated_at": ss["updated_at"].isoformat(), "server_content_hash": ss["content_hash"]})
        for ss in server_map.values():
            if ss["id"] in matched:
                continue
            synced = DB["device_syncs"].get((dev, ss["id"]))
            if synced and ss["updated_at"] <= synced:
                continue
            ops.append({"action": "download", "rom_id": ss["rom_id"], "save_id": ss["id"], "file_name": ss["file_name"],
                        "slot": ss["slot"], "emulator": ss["emulator"], "reason": "server only",
                        "server_updated_at": ss["updated_at"].isoformat(), "server_content_hash": ss["content_hash"]})
        DB["sessions"][sess] = {"ops": [o["action"] for o in ops]}
        self.reply(200, {"session_id": int(sess), "operations": ops,
                         **{f"total_{a}": sum(o["action"] == a for o in ops) for a in ("upload", "download", "conflict", "no_op")}})

    def do_GET(self):
        self.route("GET")

    def do_POST(self):
        self.route("POST")

    def do_PUT(self):
        self.route("PUT")


if __name__ == "__main__":
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8899
    srv = ThreadingHTTPServer((os.environ.get("MOCK_HOST", "127.0.0.1"), port), H)
    if len(sys.argv) > 2:
        import ssl
        ctx = ssl.SSLContext(ssl.PROTOCOL_TLS_SERVER)
        ctx.load_cert_chain(sys.argv[2])
        srv.socket = ctx.wrap_socket(srv.socket, server_side=True)
    srv.serve_forever()
