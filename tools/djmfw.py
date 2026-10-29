#!/usr/bin/env python3
"""Read and write DJM-900NXS2 update files.

  djmfw.py unpack FILE.upd OUTDIR
  djmfw.py pack   OUTDIR   FILE.upd

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import json
import os
import struct
import sys

KEY = bytes.fromhex("aab1abacb5b5a8b6b0b6b0ada6abaaacb5bca6ff")


def xor(buf):
    return bytes(b ^ KEY[i % len(KEY)] ^ 0xFF for i, b in enumerate(buf))


def _crc_table():
    tbl = []
    for i in range(256):
        c = i << 8
        for _ in range(8):
            c = ((c << 1) ^ 0x1021) & 0xFFFF if c & 0x8000 else (c << 1) & 0xFFFF
        tbl.append(c)
    return tbl


CRC_TABLE = _crc_table()


def crc16_xmodem(data):
    crc = 0
    for b in data:
        crc = ((crc << 8) & 0xFFFF) ^ CRC_TABLE[((crc >> 8) ^ b) & 0xFF]
    return crc


def parse(raw):
    d = xor(raw)
    top = d[:0x20]
    total = struct.unpack_from("<I", d, 0x20)[0]
    segs, o = [], 0x24
    while o < len(d):
        hdr = d[o:o + 32]
        ln = struct.unpack_from("<I", d, o + 32)[0]
        data = d[o + 36:o + 36 + ln]
        crc = struct.unpack_from("<H", d, o + 36 + ln)[0]
        if crc != crc16_xmodem(data):
            raise ValueError(f"CRC mismatch in segment {hdr!r}")
        segs.append((hdr, data))
        o += 38 + ln
    if total != sum(len(s[1]) for s in segs):
        raise ValueError("top-level length field does not match segments")
    return top, segs


def build(top, segs):
    out = bytearray(top) + struct.pack("<I", sum(len(s[1]) for s in segs))
    for hdr, data in segs:
        out += hdr + struct.pack("<I", len(data)) + data
        out += struct.pack("<H", crc16_xmodem(data))
    return xor(bytes(out))


def set_top_version(top, tag):
    if len(tag) != 3 or not tag.isdigit():
        raise ValueError("tag must be 3 digits, e.g. 209")
    if top[16:20] != b"Ver.":
        raise ValueError(f"unexpected top header {top!r}")
    return top[:20] + tag.encode() + top[23:]


def top_version(top):
    return top[20:23].decode()


def seg_name(hdr):
    return hdr[10:16].decode("ascii").strip()


def unpack(path, outdir):
    top, segs = parse(open(path, "rb").read())
    os.makedirs(outdir, exist_ok=True)
    manifest = {"top": top.decode("latin-1"), "segments": []}
    for hdr, data in segs:
        fn = seg_name(hdr) + ".bin"
        open(os.path.join(outdir, fn), "wb").write(data)
        manifest["segments"].append({"header": hdr.decode("latin-1"), "file": fn})
        print(f"{hdr.decode('latin-1')!r}  {len(data):#9x} bytes -> {fn}")
    with open(os.path.join(outdir, "manifest.json"), "w") as f:
        json.dump(manifest, f, indent=2)


def pack(indir, path):
    m = json.load(open(os.path.join(indir, "manifest.json")))
    segs = [(s["header"].encode("latin-1"),
             open(os.path.join(indir, s["file"]), "rb").read())
            for s in m["segments"]]
    open(path, "wb").write(build(m["top"].encode("latin-1"), segs))
    print(f"wrote {path}")


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] not in ("unpack", "pack"):
        sys.exit(__doc__)
    (unpack if sys.argv[1] == "unpack" else pack)(sys.argv[2], sys.argv[3])
