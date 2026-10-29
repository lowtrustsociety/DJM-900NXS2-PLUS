#!/usr/bin/env python3
"""Compile the panel code and put it into the panel image.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import hashlib
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import m16c as modbuild

BUILD = os.path.join(HERE, "build")
SRC = os.path.join(HERE, "src")
PANEL_BASE = 0xC0000
MOD_BASE, MOD_END = 0xEF200, 0xEFFE0
TRAMP_BASE, TRAMP_END = 0xD10E0, 0xD1100
MENU_BASE, MENU_END = 0xD1100, 0xD2000
HOOKS = [
    ("jsra", 0xC5E3D, 4, "_mod_entry",
     "c22e7dfcc241ded678c8ff1fcf3da2ce2db47640552cbc3b3303bcd1279ebe29"),
    ("jsra", 0xC5D7A, 4, "_mod_init_entry",
     "82848d6d11449392e49463dcbe91496675d7a0edeef31811b17e91dd6457355e"),
    ("jsrw", 0xCB6F6, 3, "_tx_tramp",
     "45b7493dc8d75393faba0b3e01f96ef093f710f9580282aedd60ff8ea145eecb"),
    ("jsrw", 0xCB897, 3, "_tx_tramp",
     "56155424be6861dc873bcf5fd41e78d5dac84193e84eee55deb00a7ee0ba0c74"),
]
VER_PICTURE = 0xD3B6E
VER_STOCK_SHA256 = "3817e663c2304063480dc253bc2b99f5966b4ccdb866d518e60c78fadad17804"
PLUS_PICTURE = bytes([0xE8, 0xAE,
                      0xA8, 0xA8,
                      0xA8, 0xA8,
                      0xE8, 0xAE,
                      0x88, 0xA2,
                      0x88, 0xA2,
                      0x8E, 0xEE,
                      0x00, 0x00])
ALLOWED = [(site, site + length) for _, site, length, _, _ in HOOKS] + \
    [(MOD_BASE, MOD_END), (TRAMP_BASE, MENU_END), (VER_PICTURE + 2, VER_PICTURE + 2 + len(PLUS_PICTURE))]
CFLAGS = modbuild.CFLAGS + ["-Wno-array-bounds", "-I" + SRC]


def version_text(tag):
    if len(tag) != 3 or not tag.isdigit():
        sys.exit("tag must contain exactly three digits")
    return f"{tag[0]}.{tag[1:]}"


def compile_panel(tag="000"):
    os.makedirs(BUILD, exist_ok=True)
    objs = []
    cflags = CFLAGS + [f'-DREC_V{i}={d}u' for i, d in enumerate(version_text(tag).replace(".", ""))]
    for name in ("entry.S", "safety.c", "recstat.c", "menu.c"):
        src = os.path.join(SRC, name)
        obj = os.path.join(BUILD, os.path.splitext(name)[0] + ".o")
        if name.endswith(".S"):
            modbuild.run([modbuild.TOOLS + "gcc", "-mcpu=m16c", "-I" + SRC,
                          "-I" + SRC, "-c", src, "-o", obj])
        else:
            modbuild.compile_c(src, obj, cflags)
        objs.append(obj)
    modbuild.check_far_rodata(objs)
    elf = modbuild.link(objs, os.path.join(HERE, "panel.ld"), os.path.join(BUILD, "panel_rec.elf"))
    binary = os.path.join(BUILD, "panel_rec.bin")
    modbuild.run([modbuild.TOOLS + "objcopy", "-O", "binary", "-j", ".text", elf, binary])
    with open(binary, "rb") as f:
        code = f.read()
    tramp_file = os.path.join(BUILD, "panel_rec_tramp.bin")
    modbuild.run([modbuild.TOOLS + "objcopy", "-O", "binary", "-j", ".tramp", elf, tramp_file])
    with open(tramp_file, "rb") as f:
        tramp = f.read()
    menu_file = os.path.join(BUILD, "panel_rec_menu.bin")
    modbuild.run([modbuild.TOOLS + "objcopy", "-O", "binary", "-j", ".text2", elf, menu_file])
    with open(menu_file, "rb") as f:
        menu = f.read()
    syms = modbuild.symbols(elf)
    if MENU_BASE + len(menu) > MENU_END:
        sys.exit(f"menu code too large: {len(menu)} bytes")
    if syms["_mod_entry"] != MOD_BASE:
        sys.exit("_mod_entry is not at the start of the code area")
    if MOD_BASE + len(code) > MOD_END:
        sys.exit(f"panel code too large: {len(code)} bytes")
    if syms["_tx_tramp"] != TRAMP_BASE or TRAMP_BASE + len(tramp) > TRAMP_END:
        sys.exit("trampoline is not where the builder calls can reach it")
    return code, syms, tramp, menu


def patch(segment, tag="000"):
    code, syms, tramp, menu = compile_panel(tag)
    panel = bytearray(segment)
    for base, end, what in ((MOD_BASE, MOD_END, "code"), (TRAMP_BASE, MENU_END, "second code")):
        region = panel[base - PANEL_BASE:end - PANEL_BASE]
        if region != b"\xff" * len(region):
            sys.exit(f"panel: the {what} area is not erased in the stock image")
    for _, site, _, _, window in HOOKS:
        if hashlib.sha256(bytes(panel[site - PANEL_BASE:site - PANEL_BASE + 32])).hexdigest() != window:
            sys.exit(f"panel: call at {site:#07x} is not the stock one")
    at = VER_PICTURE - PANEL_BASE
    if hashlib.sha256(bytes(panel[at:at + 32])).hexdigest() != VER_STOCK_SHA256 or panel[at:at + 2] != b"\x10\x08":
        sys.exit("panel: the Ver. picture is not the stock one")
    panel[at + 2:at + 2 + len(PLUS_PICTURE)] = PLUS_PICTURE
    panel[MOD_BASE - PANEL_BASE:MOD_BASE - PANEL_BASE + len(code)] = code
    panel[TRAMP_BASE - PANEL_BASE:TRAMP_BASE - PANEL_BASE + len(tramp)] = tramp
    panel[MENU_BASE - PANEL_BASE:MENU_BASE - PANEL_BASE + len(menu)] = menu
    for kind, site, _, symbol, _ in HOOKS:
        at = site - PANEL_BASE
        if kind == "jsra":
            panel[at:at + 4] = b"\xfd" + syms[symbol].to_bytes(3, "little")
        else:
            reach = syms[symbol] - (site + 1)
            if not -0x8000 <= reach <= 0x7FFF:
                sys.exit(f"panel: the call at {site:#07x} cannot reach {symbol}")
            panel[at:at + 3] = b"\xf5" + (reach & 0xFFFF).to_bytes(2, "little")
    return bytes(panel), len(code) + len(tramp) + len(menu)


def stock_panel():
    import djmfw
    import verify_upd
    with open(verify_upd.STOCK, "rb") as f:
        _, segments = djmfw.parse(f.read())
    return dict((djmfw.seg_name(h), d) for h, d in segments)["panel"]


if __name__ == "__main__":
    patched, size = patch(stock_panel())
    with open(os.path.join(BUILD, "panel_rec_image.bin"), "wb") as f:
        f.write(patched)
    code, _, tramp, menu = compile_panel()
    print(f"panel code: {len(code)} of {MOD_END - MOD_BASE} bytes at {MOD_BASE:#07x}, "
          f"{len(menu)} of {MENU_END - MENU_BASE} at {MENU_BASE:#07x}, trampoline {len(tramp)}; "
          f"calls redirected at {', '.join(f'{h[1]:#07x}' for h in HOOKS)}")
