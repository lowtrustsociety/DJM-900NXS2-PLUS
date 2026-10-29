#!/usr/bin/env python3
"""Draw the panel's screens from the simulator into pictures.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PANEL = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(PANEL))
BUILD = os.path.join(PANEL, "build")
TOOLS = os.path.expanduser("~/tools/m32c/bin/m32c-elf-")
STOCK = os.path.join(ROOT, "firmware", "unpacked_208", "panel.bin")
PANEL_BASE = 0xC0000
W, H = 192, 64


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, text=True, **kw)
    if r.returncode:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def simulate(fx, setter, frames=12, panel_file=STOCK, name="screen"):
    os.makedirs(BUILD, exist_ok=True)
    obj, crt, elf = (os.path.join(BUILD, name + e) for e in (".o", "_crt.o", ".elf"))
    flags = ["-Os", "-mcpu=m16c", "-ffreestanding", "-fno-builtin", "-memregs=0", "-Wall", "-Wextra",
             f"-DFX_TYPE={fx}", f"-DFRAMES={frames}"]
    if setter:
        flags.append(f"-DSET={setter}")
    run([TOOLS + "gcc", *flags, "-c", os.path.join(HERE, "screen_dump.c"), "-o", obj])
    run([TOOLS + "gcc", "-mcpu=m16c", f'-DSIM_PANEL_FILE="{panel_file}"', "-DMOD_INIT_ADDR=0xC6836",
         "-c", os.path.join(HERE, "crt_sim.S"), "-o", crt])
    run([TOOLS + "ld", "-T", os.path.join(HERE, "sim.ld"), "-o", elf, crt, obj])
    out = subprocess.run([TOOLS + "run", "-m", "m16c", elf], capture_output=True, text=True, timeout=300).stdout
    if "D" not in out.splitlines()[-1:]:
        sys.exit("the simulator did not finish:\n" + out[-500:])
    entries = []
    for line in out.splitlines():
        if line.startswith("Q "):
            _, frame, disp, slot, data = line.split()
            b = bytes.fromhex(data)
            pointer = int.from_bytes(b[0:4], "little")
            entries.append((int(frame, 16), int(disp, 16), int(slot, 16), pointer, b[4], b[5], b[6], b[7]))
    return entries


def image(panel, pointer):
    o = pointer - PANEL_BASE
    w, h = panel[o], panel[o + 1]
    return w, h, panel[o + 2:o + 2 + (w // 8) * h]


class Canvas:

    def __init__(self, display=1):
        self.display = display
        self.rows = [bytearray(24) for _ in range(H)]
        self.seen = [bytearray(24) for _ in range(H)]
        self.owner = [[None] * 24 for _ in range(H)]

    def apply(self, panel, slot, pointer, c0, r0, c1, r1, ram=None):
        if not (c0 <= c1 < 24 and r0 <= r1 < H):
            return f"odd window {c0},{r0}-{c1},{r1}"
        if pointer == 0xFFFFFFFF:
            data = None
        elif pointer < 0x10000:
            data = ram(pointer) if ram else None
            if data is None:
                return "picture in RAM"
            data = data[2:]
        else:
            w, h, data = image(panel, pointer)
            if w // 8 != c1 - c0 + 1 or h != r1 - r0 + 1:
                return f"picture {w}x{h} in window {c0},{r0}-{c1},{r1}"
        i = 0
        mirror = self.display == 0
        for r in range(r0, r1 + 1):
            for c in range(c0, c1 + 1):
                value = data[i] if data is not None else 0
                self.rows[r][c] = value
                self.owner[r][c] = slot
                if mirror:
                    self.seen[H - 1 - r1 + (r - r0)][23 - c1 + (c - c0)] = value
                else:
                    self.seen[r][c] = value
                i += 1
        return None


def view(canvas, display=None):
    return [[canvas.seen[y][x // 8] >> (7 - x % 8) & 1 for x in range(W)] for y in range(H)]


def write_png(path, width, height, rgb):
    import struct
    import zlib

    def chunk(kind, data):
        return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))
    raw = b"".join(b"\0" + rgb[y * width * 3:(y + 1) * width * 3] for y in range(height))
    with open(path, "wb") as f:
        f.write(b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", struct.pack(">IIBBBBB", width, height, 8, 2, 0, 0, 0))
                + chunk(b"IDAT", zlib.compress(raw, 9)) + chunk(b"IEND", b""))


def save(pixels, path, scale=4, marks=()):
    marked = set()
    for x0, y0, x1, y1 in marks:
        marked |= {(x, y) for x in range(x0, x1 + 1) for y in range(y0, y1 + 1)}
    out = bytearray()
    for y in range(H * scale):
        for x in range(W * scale):
            sx, sy = x // scale, y // scale
            out += (b"\xeb\xf0\xff" if pixels[sy][sx] else
                    b"\x5a\x14\x14" if (sx, sy) in marked else b"\x08\x08\x0c")
    write_png(path, W * scale, H * scale, bytes(out))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--fx", type=int, default=3)
    ap.add_argument("--set", default="")
    ap.add_argument("--name", default="screen")
    ap.add_argument("--frames", type=int, default=12)
    a = ap.parse_args()
    panel = open(STOCK, "rb").read()
    entries = simulate(a.fx, a.set, a.frames)
    canvases = [Canvas(0), Canvas(1)]
    used = {}
    for frame, disp, slot, pointer, c0, r0, c1, r1 in entries:
        problem = canvases[disp].apply(panel, slot, pointer, c0, r0, c1, r1)
        key = (disp, slot)
        used.setdefault(key, set()).add((pointer, c0, r0, c1, r1, problem))
    for (disp, slot), items in sorted(used.items()):
        for pointer, c0, r0, c1, r1, problem in sorted(items):
            what = "clear" if pointer == 0xFFFFFFFF else f"picture {pointer:#07x}"
            print(f"display {disp} slot {slot:2d}: columns {c0:2d}-{c1:2d} rows {r0:2d}-{r1:2d}  {what}"
                  + (f"  ({problem})" if problem else ""))
    for disp in (0, 1):
        save(view(canvases[disp], disp), os.path.join(BUILD, f"{a.name}_{disp}.png"))
        free = [(c, r) for r in range(H) for c in range(24) if canvases[disp].owner[r][c] in (None, 0)]
        print(f"display {disp}: {len(free)} of {24 * H} cells never drawn by a slot other than 0")
    print("pictures:", os.path.join(BUILD, a.name + "_0.png"), "and _1.png")


if __name__ == "__main__":
    main()
