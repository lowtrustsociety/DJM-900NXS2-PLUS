#!/usr/bin/env python3
"""Tests of the packing, on the computer and in the SH simulator.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import glob
import os
import re
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.dirname(HERE)
ROOT = os.path.dirname(PROBE)
sys.path.insert(0, PROBE)
import build_stream as build

BLOCKS = 20000
failed = []


def check(value, label):
    print(("PASS " if value else "FAIL ") + label)
    if not value:
        failed.append(label)


def run(command, **kw):
    result = subprocess.run(command, capture_output=True, text=True, **kw)
    if result.returncode not in kw.get("ok", (0,)) and "ok" not in kw:
        raise RuntimeError(" ".join(command) + "\n" + result.stdout[-2000:] + result.stderr[-2000:])
    return result


def sample_bytes(path):
    with open(path, "rb") as f:
        f.seek(34)
        return struct.unpack("<H", f.read(2))[0] // 8


def audio(path, start_seconds, gain):
    width = sample_bytes(path)
    rate = 96000 * width
    with open(path, "rb") as f:
        f.seek(508)
        length = struct.unpack("<I", f.read(4))[0]
        f.seek(512 + int(start_seconds * rate) // (2 * width) * (2 * width))
        data = f.read(min(BLOCKS * 24 * width, max(0, length - int(start_seconds * rate))))
    if width == 2:
        seed, samples = 1, []
        for s in struct.unpack("<%dh" % (len(data) // 2), data):
            seed = (seed * 1664525 + 1013904223) & 0xFFFFFFFF
            samples.append(s * 256 + (seed >> 24))
    else:
        samples = [int.from_bytes(data[i:i + 3], "little", signed=True)
                   for i in range(0, len(data) - 2, 3)]
    return [max(-8388608, min(8388607, s * gain)) for s in samples]


def main():
    os.makedirs(build.BUILD, exist_ok=True)
    native = os.path.join(build.BUILD, "pack_native")
    run(["cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-o", native,
         os.path.join(HERE, "pack_native.c"), os.path.join(PROBE, "src", "pack.c")])
    recordings = sorted(glob.glob(os.path.join(HERE, "recordings", "*.WAV")))
    out = subprocess.run([native], capture_output=True, text=True)
    check(out.returncode == 0 and "wrong 0" in out.stdout and "largest block 74" in out.stdout
          and len(re.findall(r" \d+:(\d+)", out.stdout)) == 25
          and all(int(n) > 0 for n in re.findall(r" \d+:(\d+)", out.stdout)),
          "Mac: generated signals of all widths 0 to 23 and plain come back bit for bit; no block over 74 bytes")
    raw = os.path.join(build.BUILD, "pack_test.raw")
    for path in recordings:
        with open(path, "rb") as f, open(raw, "wb") as g:
            f.seek(512)
            g.write(f.read())
        out = subprocess.run([native, raw, str(sample_bytes(path))], capture_output=True, text=True)
        ratio = re.search(r"file: ([\d.]+)", out.stdout)
        check(out.returncode == 0 and "wrong 0" in out.stdout and ratio is not None,
              f"Mac: {os.path.relpath(path, PROBE)} "
              f"({'24 bit' if sample_bytes(path) == 3 else '16 bit widened with noise'}) comes back "
              f"bit for bit ({ratio.group(1) if ratio else '?'} of plain)")
    if os.path.exists(raw):
        os.remove(raw)

    crt = os.path.join(build.BUILD, "pack_crt.o")
    run([build.TOOL_PREFIX + "gcc", "-m2a-nofpu", "-mb", "-c",
         os.path.join(HERE, "crt_sim.S"), "-o", crt])
    longest = max(recordings, key=os.path.getsize) if recordings else None
    sources = [(longest, 60, 1), (longest, 60, 4)] if recordings else []
    for path, start, gain in sources:
        samples = audio(path, start, gain)
        blocks = len(samples) // 24
        with open(os.path.join(build.BUILD, "audio.bin"), "wb") as f:
            f.write(struct.pack(">%di" % (blocks * 24), *samples[:blocks * 24]))
        run([build.TOOL_PREFIX + "objcopy", "-I", "binary", "-O", "elf32-sh", "-B", "sh",
             "--rename-section", ".data=.rodata", "--set-section-alignment", ".data=4",
             "audio.bin", "pack_audio.o"], cwd=build.BUILD)
        counts, text = {}, ""
        for mode in (0, 1, 2):
            objects = []
            for source in (os.path.join(PROBE, "src", "pack.c"), os.path.join(HERE, "pack_sim.c")):
                obj = os.path.join(build.BUILD, "pack_sim_%d_%s.o" % (mode, os.path.basename(source)))
                run([build.TOOL_PREFIX + "gcc", *build.CFLAGS, *build.SOURCE_FLAGS.get("pack", []),
                     f"-DMODE={mode}", f"-DBLOCKS={blocks}u", "-c", source, "-o", obj])
                objects.append(obj)
            elf = os.path.join(build.BUILD, "pack_sim_%d.elf" % mode)
            run([build.TOOL_PREFIX + "ld", "-EB", "-T",
                 os.path.join(HERE, "sim.ld"), "-o", elf, crt,
                 *objects, os.path.join(build.BUILD, "pack_audio.o")])
            result = subprocess.run([build.TOOL_PREFIX + "run", "-v", elf], capture_output=True, text=True)
            text = result.stdout + result.stderr
            counts[mode] = int(re.search(r"# instructions executed\s+(\d+)", text).group(1))
        size = int(re.search(r"bytes (\d+)", text).group(1))
        check("wrong 0" in text and size > 0,
              f"SH simulator: {blocks} blocks of {os.path.basename(path)} (level x{gain}) come back "
              f"bit for bit; packed to {size / (blocks * 72):.3f} of plain; instructions per block: "
              f"pack {(counts[1] - counts[0]) / blocks:.0f}, unpack {(counts[2] - counts[1]) / blocks:.0f}")
        os.remove(os.path.join(build.BUILD, "audio.bin"))
    if not recordings:
        print("     (no recordings found: real audio not tested)")
    print(f"\n{len(failed)} failed" if failed else "\nall checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
