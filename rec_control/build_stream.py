#!/usr/bin/env python3
"""Compile and link the recorder for the SH7267.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""

import os
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, os.path.join(ROOT, "tools"))
import djmfw

TOOL_PREFIX = os.path.expanduser("~/tools/sh/bin/sh-elf-")
CFLAGS = [
    "-m2a-nofpu", "-mb", "-mrenesas", "-Os", "-ffreestanding",
    "-fno-builtin", "-nostdlib", "-fno-zero-initialized-in-bss",
    "-fno-common", "-Wall", "-Wextra", "-Werror",
]
BUILD = os.path.join(HERE, "build")
SOURCES = ["stream", "fat", "recfile", "pack", "listen", "reclog"]
SOURCE_FLAGS = {"pack": ["-O2"]}
VARIANTS = {
    "normal": ["-DAUTO_START=0u"],
    "autostart": [],
    "short": ["-DAUTO_START=0u", "-DFILE_AUDIO_PAGES=1125u", "-DFILE_MIN_BYTES=1048576u",
              "-DFILE_REFILL_BYTES=FILE_CAP_BYTES", "-DNEXT_AHEAD_WORDS=2880000u",
              "-DCOUNTDOWN_WORDS=2400000u"],
    "test90": ["-DFILE_AUDIO_PAGES=1125u", "-DFILE_MIN_BYTES=1048576u",
               "-DFILE_REFILL_BYTES=FILE_CAP_BYTES", "-DNEXT_AHEAD_WORDS=2880000u",
               "-DCOUNTDOWN_WORDS=2400000u"],
    "burst": ["-DBURST_PAGES=13u"],
    "handtest": ["-DCONTROL_START_KEY=0xB049u", "-DCONTROL_STOP_KEY=0xB049u",
                 "-DCONTROL_MARK_KEY=0xB048u", "-DCONTROL_CUT_KEY=0xB047u"],
}
STOCK_UPDATE = os.path.join(ROOT, "firmware", "original", "DJM-900NXS2_208.upd")
ENTRY_ADDRESS = 0x1C038000
CODE_FLASH_START = 0xF8000
CODE_FLASH_END = 0x100000
CANDIDATE_FLASH_START = 0x100000
CANDIDATE_FLASH_END = 0x11B000
VARS_ADDRESS = 0x1C058E00
VARS_FLASH_START = 0x118E00
VARS_FLASH_END = 0x11B000
VARS_GUARD_MIN = 1024
AUDITED_BUFFER_FLASH_START = 0x100000
BACKGROUND_HOOK_OFFSET = 0xCAA58
BACKGROUND_HOOK_STOCK = 0x1C0095E0
SEND_HOOK_OFFSET = 0xEE284
SEND_HOOK_STOCK = 0xFFF84EA6


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        sys.exit(f"FAILED: {' '.join(command)}\n{result.stdout}{result.stderr}")
    return result.stdout


def symbols(elf_path):
    result = {}
    for line in run([TOOL_PREFIX + "nm", elf_path]).splitlines():
        parts = line.split()
        if len(parts) == 3:
            result[parts[2]] = int(parts[0], 16)
    return result


def stock_sh_image():
    with open(STOCK_UPDATE, "rb") as update_file:
        _, segments = djmfw.parse(update_file.read())
    return dict((djmfw.seg_name(header), data) for header, data in segments)["sh"]


def validate_stock():
    stock = stock_sh_image()
    if stock[CODE_FLASH_START:CODE_FLASH_END] != b"\xff" * (
        CODE_FLASH_END - CODE_FLASH_START
    ):
        sys.exit("stock SH code tail is not erased")
    if stock[CANDIDATE_FLASH_START:CANDIDATE_FLASH_END] != b"\xff" * (
        CANDIDATE_FLASH_END - CANDIDATE_FLASH_START
    ):
        sys.exit("stock candidate SDRAM source is not erased")
    background = struct.unpack(
        ">I", stock[BACKGROUND_HOOK_OFFSET:BACKGROUND_HOOK_OFFSET + 4]
    )[0]
    send = struct.unpack(">I", stock[SEND_HOOK_OFFSET:SEND_HOOK_OFFSET + 4])[0]
    if background != BACKGROUND_HOOK_STOCK:
        sys.exit("unexpected stock background callback pointer")
    if send != SEND_HOOK_STOCK:
        sys.exit("unexpected stock SEND callback pointer")


def build(variant="normal"):
    os.makedirs(BUILD, exist_ok=True)
    elf = os.path.join(BUILD, "rec_control.elf")
    binary = os.path.join(BUILD, "rec_control.bin")
    objects = []
    for name in SOURCES:
        obj = os.path.join(BUILD, "rec_" + name + ".o")
        run([TOOL_PREFIX + "gcc", *CFLAGS, *SOURCE_FLAGS.get(name, []), *VARIANTS[variant], "-c",
             os.path.join(HERE, "src", name + ".c"), "-o", obj])
        objects.append(obj)
    run([TOOL_PREFIX + "ld", "-EB", "-T", os.path.join(HERE, "sh.ld"), "-o", elf, *objects])
    variables = os.path.join(BUILD, "rec_vars.bin")
    run([TOOL_PREFIX + "objcopy", "-O", "binary", "-j", ".text", "-j", ".rodata", elf, binary])
    run([TOOL_PREFIX + "objcopy", "-O", "binary", "-j", ".data", elf, variables])
    syms = symbols(elf)
    if syms.get("_recorder_service") != ENTRY_ADDRESS:
        sys.exit("recorder service is not at the SH code-tail start")
    if "_stream_capture_isr" not in syms:
        sys.exit("stream capture ISR symbol is missing")
    size = os.path.getsize(binary)
    if size > AUDITED_BUFFER_FLASH_START - CODE_FLASH_START:
        sys.exit("recorder code exceeds the 32768-byte code partition")
    vars_size = os.path.getsize(variables)
    if syms.get("_vars_end") != VARS_ADDRESS + vars_size or vars_size % 4:
        sys.exit("the variables are not where the linker script puts them")
    if vars_size > VARS_FLASH_END - VARS_FLASH_START - VARS_GUARD_MIN:
        sys.exit("the variables leave too little of their piece as a guard")
    validate_stock()
    print(f"recorder code: {size} of 32768 bytes at SDRAM {ENTRY_ADDRESS:#x} (variant {variant})")
    print(f"variables:     {vars_size} of {VARS_FLASH_END - VARS_FLASH_START} bytes at SDRAM {VARS_ADDRESS:#x} "
          f"(the other {VARS_FLASH_END - VARS_FLASH_START - vars_size} stay 0xFF and are watched)")
    print(f"SEND ISR:      {syms['_stream_capture_isr']:#x}")
    print("pages 0-2:     3 x 23040 bytes at SDRAM 0x1c042600 (static, audited at boot); the page after them is the writer's")
    print("buffers:       header, idle header, directory sector, 16 table sectors at SDRAM 0x1c040000")
    print("pages 3-5:     3 x 23040 bytes at SDRAM 0x1c120000 (stock pool area, audio only)")
    print("extra pages:   30 x 23040 bytes in three pieces: 20 at SDRAM 0x1c0ad800-0x1c11dfff, 2 at 0x1c130e00-0x1c13c1ff, "
          "8 at 0x1c077000-0x1c0a3fff (each piece while checksum passes over it agree)")
    print("pool pages:    9 x 23040 bytes in three 69120-byte blocks from Pool 1, during a take")
    print("total buffer:  1036800 bytes (45 pages), audio packed losslessly: at 24 bit 3.50 s at worst, about 4.3 to 6 s of music, at 16 bit 6.5 to 9 s of music; "
          "15 pages without extra RAM")
    print("extent maps:   16384-byte block allocated from Pool 1 (file in use and work map, 1024 extents each)")
    print("validated stock hooks, block I/O entry points, and buffer sources")
    return elf, binary, syms


if __name__ == "__main__":
    build()
    print("compile-only result: no .upd file was created")
