#!/usr/bin/env python3
"""Compare an update file with the official 2.08 file and refuse any change outside the allowed places.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import argparse
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import djmfw

STOCK = os.path.join(os.path.dirname(HERE), "firmware/original/DJM-900NXS2_208.upd")
STOCK_SHA256 = "482817cfe5984894db8e198eba8d3dd33e226d523ac5e1555bbccb5cc6c08538"

PANEL_BASE = 0xC0000
ALLOWED_PANEL = [(0xC5E3D, 0xC5E41), (0xC5D7A, 0xC5D7E),
                 (0xCB6F6, 0xCB6F9), (0xCB897, 0xCB89A),
                 (0xD10E0, 0xD1100),
                 (0xEF200, 0xEFFE0)]
ALLOWED_PANEL_STATUS = [(0xC5E3D, 0xC5E41), (0xC5D7A, 0xC5D7E), (0xCB6F6, 0xCB6F9), (0xCB897, 0xCB89A),
                        (0xEF200, 0xEFFE0), (0xD10E0, 0xD2000),
                        (0xD3B70, 0xD3B80)]
PANEL_BOOT = (0xF0000, 0x100000)
ALLOWED_SH_MOD = [(0xEE270, 0xEE274), (0xEE284, 0xEE288),
                  (0xF8000, 0x100000)]
ALLOWED_SH_RECORDER = [(0xCAA58, 0xCAA5C), (0xEE284, 0xEE288),
                       (0xF8000, 0x100000),
                       (0x118E00, 0x11B000)]
SH_PROTECTED = (0x00000, 0xA0000)
ALLOWED_MAIN_TUNNEL = [(0x74EA0, 0x74EA4), (0x77E60, 0x77EB4)]
MAIN_PROTECTED = (0x00000, 0x50000)


def ranges(idx):
    out = []
    for i in idx:
        if out and out[-1][1] == i:
            out[-1][1] = i + 1
        else:
            out.append([i, i + 1])
    return out


def verify(path, profile):
    with open(path, "rb") as input_file:
        raw = input_file.read()
    top, segs = djmfw.parse(raw)
    with open(STOCK, "rb") as stock_file:
        sraw = stock_file.read()
    if hashlib.sha256(sraw).hexdigest() != STOCK_SHA256:
        sys.exit(f"reference {STOCK} is not the official v2.08 file")
    stop, ssegs = djmfw.parse(sraw)
    problems = []
    print(f"{os.path.basename(path)}  sha256 {hashlib.sha256(raw).hexdigest()}")
    print(f"  header {top.decode('latin-1').rstrip()!r} (stock "
          f"{stop.decode('latin-1').rstrip()!r}), all segment CRCs valid")
    if top[:20] != stop[:20] or top[23:] != stop[23:]:
        problems.append("top header differs beyond the version digits")

    if [h for h, _ in segs] != [h for h, _ in ssegs]:
        problems.append("segment headers differ from stock")
    for (hdr, data), (_, sdata) in zip(segs, ssegs):
        name = djmfw.seg_name(hdr)
        if len(data) != len(sdata):
            problems.append(f"{name}: size differs")
            continue
        diff = [i for i in range(len(data)) if data[i] != sdata[i]] if data != sdata else []
        if not diff:
            print(f"  {name:6} identical to stock")
            continue
        rs = ranges(diff)
        desc = ", ".join(f"{a + (PANEL_BASE if name == 'panel' else 0):#07x}"
                         f"-{b - 1 + (PANEL_BASE if name == 'panel' else 0):#07x}"
                         for a, b in rs[:6])
        print(f"  {name:6} {len(diff)} bytes differ: {desc}{' ...' if len(rs) > 6 else ''}")
        if name == "main" and profile in ("tunnel", "status"):
            for a, b in rs:
                if not any(lo <= a and b <= hi for lo, hi in ALLOWED_MAIN_TUNNEL):
                    problems.append(f"main: change at {a:#07x}-{b - 1:#07x} outside the tunnel patch")
                if a < MAIN_PROTECTED[1] and b > MAIN_PROTECTED[0]:
                    problems.append("main: boot loader/updater area modified")
            continue
        if name == "sh" and profile in ("modsh", "recorder", "tunnel", "status"):
            allowed = ALLOWED_SH_MOD if profile == "modsh" else ALLOWED_SH_RECORDER
            for a, b in rs:
                if not any(lo <= a and b <= hi for lo, hi in allowed):
                    problems.append(f"sh: change at {a:#07x}-{b - 1:#07x} outside handler pointers/free area")
                if a < SH_PROTECTED[1] and b > SH_PROTECTED[0]:
                    problems.append("sh: loader/updater area modified")
            continue
        if name != "panel" or profile not in ("mod", "modsh", "status"):
            problems.append(f"{name}: modified")
            continue
        allowed_panel = ALLOWED_PANEL_STATUS if profile == "status" else ALLOWED_PANEL
        for a, b in rs:
            a, b = a + PANEL_BASE, b + PANEL_BASE
            if not any(lo <= a and b <= hi for lo, hi in allowed_panel):
                problems.append(f"panel: change at {a:#07x}-{b - 1:#07x} outside hook/mod region")
            if a < PANEL_BOOT[1] and b > PANEL_BOOT[0]:
                problems.append("panel: boot region modified")

    for p in problems:
        print("  PROBLEM:", p)
    print("  OK" if not problems else "  FAILED")
    return not problems


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("file")
    ap.add_argument("--profile", choices=["mod", "modsh", "recorder", "tunnel", "status", "restore"],
                    default="mod")
    a = ap.parse_args()
    sys.exit(0 if verify(a.file, a.profile) else 1)
