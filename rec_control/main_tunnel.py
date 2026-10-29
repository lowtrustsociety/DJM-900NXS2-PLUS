#!/usr/bin/env python3
"""The patch of the main processor's application: the recorder's status to the panel.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""

import hashlib
import struct

SEGMENT_BASE = 0x80000000 - 0x50000

BUILDER = 0x80024238
MEMCPY = 0x8008273C
CALL_SITE = 0x80024EA0
RETURN_TO = 0x80024EA4
LINK_COMMAND = 0x80027E60
PATCH = LINK_COMMAND + 8
SH_LINK_STATE = 0x8020016C

PANEL_RX = 0x80208040
PANEL_TX = 0x80207FC0
SH_RX = 0x80200A00
SH_FRAME = 0x80200C40

TO_SH_SOURCE, TO_SH_TARGET, TO_SH_BYTES = PANEL_RX + 0x04, SH_FRAME + 0x66, 12
TO_PANEL_SOURCE, TO_PANEL_TARGET, TO_PANEL_BYTES = SH_RX + 0x66, PANEL_TX + 0x44, 24

R0, R1, R2 = 0, 1, 2
ALWAYS, NOT_EQUAL = 0xE, 0x1


def branch(at, target, link=False, condition=ALWAYS):
    offset = (target - (at + 8)) >> 2
    if (target - at) & 3 or not -0x800000 <= offset < 0x800000:
        raise ValueError("branch out of reach")
    return condition << 28 | (0x0B if link else 0x0A) << 24 | offset & 0xFFFFFF


def movw(register, value):
    return 0xE3000000 | (value >> 12 & 0xF) << 16 | register << 12 | value & 0xFFF


def movt(register, value):
    return 0xE3400000 | (value >> 12 & 0xF) << 16 | register << 12 | value & 0xFFF


def load_address(register, address):
    return [movw(register, address & 0xFFFF), movt(register, address >> 16)]


def mov_small(register, value):
    if not 0 <= value < 256:
        raise ValueError("immediate too large")
    return 0xE3A00000 | register << 12 | value


MVN_R0_0 = 0xE3E00000
BX_LR = 0xE12FFF1E
LDRH_R0_R0 = 0xE1D000B0
CMP_R0_2 = 0xE3500002


def patch_words():
    words = [MVN_R0_0, BX_LR]
    at = PATCH

    def add(*new):
        nonlocal at
        words.extend(new)
        at += 4 * len(new)

    add(branch(at, BUILDER, link=True))
    add(*load_address(R0, SH_LINK_STATE), LDRH_R0_R0, CMP_R0_2)
    add(branch(at, RETURN_TO, condition=NOT_EQUAL))
    add(*load_address(R0, TO_SH_TARGET), *load_address(R1, TO_SH_SOURCE), mov_small(R2, TO_SH_BYTES))
    add(branch(at, MEMCPY, link=True))
    add(*load_address(R0, TO_PANEL_TARGET), *load_address(R1, TO_PANEL_SOURCE),
        mov_small(R2, TO_PANEL_BYTES))
    add(branch(at, MEMCPY, link=True))
    add(branch(at, RETURN_TO))
    return words


def changes():
    hook = struct.pack("<I", branch(CALL_SITE, PATCH))
    stock_hook = struct.pack("<I", branch(CALL_SITE, BUILDER, link=True))
    body = b"".join(struct.pack("<I", word) for word in patch_words())
    return [(CALL_SITE - SEGMENT_BASE, stock_hook, hook),
            (LINK_COMMAND - SEGMENT_BASE, None, body)]


STOCK_COMMAND_WINDOW = 32
STOCK_COMMAND_SHA256 = "b60bfac8be3136df7a0b22250b5523540564b1d8cfd8964e8eef43b95dff920e"
ALLOWED = [(offset, offset + len(new)) for offset, _, new in changes()]


def self_check(segment):
    def word(address):
        return struct.unpack_from("<I", segment, address - SEGMENT_BASE)[0]

    if segment[0x50000:0x5000D] != b"DJM-TOP-NEXT!":
        raise SystemExit("main segment: application header not found")
    checks = [
        (word(CALL_SITE), branch(CALL_SITE, BUILDER, link=True), "bl"),
        (word(0x80024E70), movw(R0, 0x6558), "movw"),
        (word(0x80024E74), movt(R0, 0x8020), "movt"),
        (word(0x80024E90), branch(0x80024E90, RETURN_TO), "b"),
        (word(0x8003C56C), branch(0x8003C56C, 0x8003C698, condition=NOT_EQUAL), "bne"),
        (word(0x80024D88), mov_small(R1, 0x20), "mov"),
        (word(0x80029260), MVN_R0_0, "mvn"),
        (word(0x80082740) | 0xE0000000, BX_LR, "bx"),
        (word(0x8008273C), 0xE3520000, "memcpy entry"),
        (word(BUILDER) & 0xFFFF4000, 0xE92D4000, "builder entry"),
    ]
    for found, made, what in checks:
        if found != made:
            raise SystemExit(f"main segment: {what} does not match the stock firmware")
    start = LINK_COMMAND - SEGMENT_BASE
    if hashlib.sha256(segment[start:start + STOCK_COMMAND_WINDOW]).hexdigest() != STOCK_COMMAND_SHA256:
        raise SystemExit("main segment: the shell command is not where it is expected")


def apply(segment):
    self_check(segment)
    out = bytearray(segment)
    for offset, stock, new in changes():
        if stock is not None and bytes(out[offset:offset + len(stock)]) != stock:
            raise SystemExit("main segment: call site does not hold the stock instruction")
        out[offset:offset + len(new)] = new
    return bytes(out)


def listing():
    lines = [f"{CALL_SITE:08x}: {branch(CALL_SITE, PATCH):08x}   b {PATCH:#x}"
             f"   (was {branch(CALL_SITE, BUILDER, link=True):08x}, bl {BUILDER:#x})"]
    for index, word in enumerate(patch_words()):
        lines.append(f"{LINK_COMMAND + 4 * index:08x}: {word:08x}")
    return "\n".join(lines)


if __name__ == "__main__":
    print(listing())
    print(f"{sum(len(new) for _, _, new in changes())} bytes change, main segment offsets "
          + ", ".join(f"{a:#x}-{b - 1:#x}" for a, b in ALLOWED))
