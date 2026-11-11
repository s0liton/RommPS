#!/usr/bin/env python3
"""Made-up Switch dumps for tests, and the prod.keys that opens them.

    make_switch.py xci <out.xci> <TITLE_ID> <prod.keys>
    make_switch.py nsp <out.nsp> <TITLE_ID> <prod.keys> [noticket]

The XCI's secure partition holds a meta and a Program NCA whose headers name
the title id, encrypted as Nintendo's are (AES-128-XTS, the sector number
big-endian in the tweak) under a made-up header_key, written to prod.keys.
The NSP is laid out as an eShop update's: a ticket and certificate named by
the rights id (the title id first), then meta, Program and Control NCAs;
noticket leaves the ticket out, so only the key reads what it is.
AES comes from the openssl command. No Nintendo data, no real keys.
"""
import struct
import subprocess
import sys

HEADER_KEY = b"made-up header_key, not Nintendo"
SECTOR = 0x200


def ecb(key, data):
    return subprocess.run(["openssl", "enc", "-aes-128-ecb", "-nopad", "-K", key.hex()],
                          input=data, capture_output=True, check=True).stdout


def times_x(t):
    v = int.from_bytes(t, "little") << 1
    if v >> 128:
        v ^= (1 << 128) | 0x87
    return v.to_bytes(16, "little")


def xts_encrypt(key, first_sector, data):
    n = len(data) // SECTOR
    starts = ecb(key[16:], b"".join((first_sector + s).to_bytes(16, "big") for s in range(n)))
    tweaks = []
    for s in range(n):
        t = starts[16 * s:16 * s + 16]
        for _ in range(SECTOR // 16):
            tweaks.append(t)
            t = times_x(t)
    pad = b"".join(tweaks)
    mixed = ecb(key[:16], bytes(a ^ b for a, b in zip(data, pad)))
    return bytes(a ^ b for a, b in zip(mixed, pad))


def nca(content_type, title_id):
    h = bytearray(0x1000)
    h[:0x200] = b"\xee" * 0x200  # the signatures
    h[0x200:0x204] = b"NCA3"
    h[0x205] = content_type
    struct.pack_into("<QQ", h, 0x208, len(h), title_id)
    return xts_encrypt(HEADER_KEY, 0, bytes(h[:0xC00])) + bytes(h[0xC00:])


def hfs0(files):
    names = b"".join(name.encode() + b"\0" for name, _ in files)
    names += b"\0" * (-len(names) % 0x20)
    table, data, name_at = b"", b"", 0
    for name, body in files:
        table += struct.pack("<QQII8x32x", len(data), len(body), name_at, 0)
        name_at += len(name) + 1
        data += body
    return b"HFS0" + struct.pack("<III", len(files), len(names), 0) + table + names + data


def pfs0(files):
    names = b"".join(name.encode() + b"\0" for name, _ in files)
    names += b"\0" * (-len(names) % 0x20)
    table, data, name_at = b"", b"", 0
    for name, body in files:
        table += struct.pack("<QQII", len(data), len(body), name_at, 0)
        name_at += len(name) + 1
        data += body
    return b"PFS0" + struct.pack("<III", len(files), len(names), 0) + table + names + data


def nsp(title_id, ticket):
    rights = "%016x%016x" % (title_id, 0xb)  # the title id, then the key generation
    files = [(rights + ".tik", b"made-up ticket"), (rights + ".cert", b"made-up cert")] if ticket else []
    # As real updates are: only the meta NCA names the update; its Program and
    # Control NCAs name the game they patch.
    patched = title_id & ~0xFFF if title_id & 0xFFF == 0x800 else title_id
    return pfs0(files + [("0123456789abcdef0123456789abcdef.cnmt.nca", nca(1, title_id)),
                         ("fedcba9876543210fedcba9876543210.nca", nca(0, patched)),
                         ("00112233445566778899aabbccddeeff.nca", nca(2, patched))])


def xci(title_id):
    secure = hfs0([("0123456789abcdef0123456789abcdef.cnmt.nca", nca(1, title_id)),
                   ("fedcba9876543210fedcba9876543210.nca", nca(0, title_id))])
    root = hfs0([("update", hfs0([])), ("normal", hfs0([])), ("secure", secure)])
    head = bytearray(0xF000)
    head[0x100:0x104] = b"HEAD"
    struct.pack_into("<QQ", head, 0x130, len(head), 0x200)
    return bytes(head) + root


def main():
    if len(sys.argv) not in (5, 6) or sys.argv[1] not in ("xci", "nsp") or sys.argv[5:] not in ([], ["noticket"]):
        sys.exit(__doc__)
    title_id = int(sys.argv[3], 16)
    with open(sys.argv[2], "wb") as f:
        f.write(xci(title_id) if sys.argv[1] == "xci" else nsp(title_id, sys.argv[5:] != ["noticket"]))
    with open(sys.argv[4], "w") as f:
        f.write("master_key_00 = 00112233445566778899aabbccddeeff\nheader_key = %s\n" % HEADER_KEY.hex())


if __name__ == "__main__":
    main()
