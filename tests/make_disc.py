#!/usr/bin/env python3
"""Tiny disc images and PARAM.SFO files for tests.

    make_disc.py psp <out.iso> <DISC_ID>
    make_disc.py sfo <out.sfo> KEY=VALUE ...
    make_disc.py cso <out.cso> <DISC_ID>
    make_disc.py gc <out.iso> <GAMEID>
"""
import struct
import sys

SECTOR = 2048


def sfo(values):
    keys = list(values)
    key_table = b""
    key_offsets = []
    for k in keys:
        key_offsets.append(len(key_table))
        key_table += k.encode() + b"\0"
    key_table += b"\0" * (-len(key_table) % 4)
    data = b""
    index = b""
    for i, k in enumerate(keys):
        v = values[k].encode() + b"\0"
        size = len(v) + (-len(v) % 4)
        index += struct.pack("<HHIII", key_offsets[i], 0x0204, len(v), size, len(data))
        data += v + b"\0" * (size - len(v))
    key_start = 20 + len(index)
    data_start = key_start + len(key_table)
    return b"\0PSF" + struct.pack("<IIII", 0x0101, key_start, data_start, len(keys)) + index + key_table + data


def both16(v):
    return struct.pack("<H", v) + struct.pack(">H", v)


def both32(v):
    return struct.pack("<I", v) + struct.pack(">I", v)


def record(name, lba, size, is_dir):
    rec = bytes([0, 0]) + both32(lba) + both32(size) + bytes(7) + bytes([2 if is_dir else 0, 0, 0]) + both16(1)
    rec += bytes([len(name)]) + name
    if len(rec) % 2:
        rec += b"\0"
    return bytes([len(rec)]) + rec[1:]


def psp_iso(disc_id):
    param = sfo({"DISC_ID": disc_id, "TITLE": "Test Game"})
    img = bytearray(SECTOR * 21)
    pvd = bytearray(SECTOR)
    pvd[0] = 1
    pvd[1:6] = b"CD001"
    pvd[6] = 1
    pvd[156:156 + 34] = record(b"\0", 18, SECTOR, True)
    img[16 * SECTOR:17 * SECTOR] = pvd
    img[17 * SECTOR:17 * SECTOR + 7] = b"\xffCD001\x01"
    root = record(b"\0", 18, SECTOR, True) + record(b"\1", 18, SECTOR, True) + record(b"PSP_GAME", 19, SECTOR, True)
    img[18 * SECTOR:18 * SECTOR + len(root)] = root
    game = record(b"\0", 19, SECTOR, True) + record(b"\1", 18, SECTOR, True) + record(b"PARAM.SFO;1", 20, len(param), False)
    img[19 * SECTOR:19 * SECTOR + len(game)] = game
    img[20 * SECTOR:20 * SECTOR + len(param)] = param
    return bytes(img)


def cso(iso, block=2048):
    import zlib
    blocks = [iso[i:i + block] for i in range(0, len(iso), block)]
    header = b"CISO" + struct.pack("<IQIBBH", 24, len(iso), block, 1, 0, 0)
    body, index = b"", []
    offset = len(header) + 4 * (len(blocks) + 1)
    for i, b in enumerate(blocks):
        index.append(offset + len(body))
        c = zlib.compressobj(9, zlib.DEFLATED, -15)
        z = c.compress(b) + c.flush()
        if i % 2 or len(z) >= len(b):  # store some blocks uncompressed
            index[-1] |= 0x80000000
            z = b
        body += z
    index.append(offset + len(body))
    return header + b"".join(struct.pack("<I", x) for x in index) + body


if __name__ == "__main__":
    kind, out = sys.argv[1], sys.argv[2]
    if kind == "psp":
        data = psp_iso(sys.argv[3])
    elif kind == "cso":
        data = cso(psp_iso(sys.argv[3]))
    elif kind == "sfo":
        data = sfo(dict(a.split("=", 1) for a in sys.argv[3:]))
    elif kind == "gc":
        data = sys.argv[3].encode()[:6] + bytes(0x20) + b"\xc2\x33\x9f\x3d" + bytes(64)
    else:
        sys.exit("unknown kind")
    open(out, "wb").write(data)
