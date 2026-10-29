#!/usr/bin/env python3
"""Tests of the main processor patch, run in an ARM emulator.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.dirname(HERE))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "tools"))
import djmfw
import main_tunnel as T
import verify_upd

try:
    from unicorn import Uc, UC_ARCH_ARM, UC_MODE_ARM, UC_HOOK_CODE, UC_HOOK_MEM_WRITE
    from unicorn.arm_const import (UC_ARM_REG_SP, UC_ARM_REG_LR, UC_ARM_REG_PC, UC_ARM_REG_R0,
                                   UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7,
                                   UC_ARM_REG_R8, UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11)
except ImportError:
    sys.exit("the Python package unicorn is missing: python3 -m venv V; V/bin/pip install unicorn")

KEPT = [UC_ARM_REG_R4, UC_ARM_REG_R5, UC_ARM_REG_R6, UC_ARM_REG_R7, UC_ARM_REG_R8,
        UC_ARM_REG_R9, UC_ARM_REG_R10, UC_ARM_REG_R11]
RAM, RAM_BYTES = 0x80000000, 0x800000
STACK = 0x80700000
passed = failed = 0


def check(name, ok, detail=""):
    global passed, failed
    if ok:
        passed += 1
    else:
        failed += 1
        print(f"FAIL {name} {detail}")


def machine(segment, stub_builder):
    uc = Uc(UC_ARCH_ARM, UC_MODE_ARM)
    uc.mem_map(RAM, RAM_BYTES)
    uc.mem_write(RAM, segment[0x50000:])
    if stub_builder:
        uc.mem_write(T.BUILDER, struct.pack("<I", T.BX_LR))
    return uc


def run_site(uc, state, limit=200000, mode=0):
    uc.mem_write(0x80206368, struct.pack("<I", T.PANEL_TX))
    uc.mem_write(0x8020636C, struct.pack("<I", T.PANEL_RX))
    uc.mem_write(0x8020612E, bytes([mode]))
    panel_rx = bytes(0x80 + i for i in range(0x60))
    sh_rx = bytes((0x11 * i + 3) & 0xFF for i in range(128))
    uc.mem_write(T.PANEL_RX, panel_rx)
    uc.mem_write(T.SH_RX, sh_rx)
    uc.mem_write(T.SH_LINK_STATE, struct.pack("<H", state))
    before = bytes(uc.mem_read(0x80200000, 0x10000))
    for index, register in enumerate(KEPT):
        uc.reg_write(register, 0xA5A50000 + index)
    uc.reg_write(UC_ARM_REG_SP, STACK)
    lowest = [STACK]
    uc.hook_add(UC_HOOK_MEM_WRITE, lambda u, access, address, size, value, data:
                lowest.__setitem__(0, min(lowest[0], address)) if STACK - 0x10000 <= address < STACK else None)
    uc.emu_start(T.CALL_SITE, T.RETURN_TO, count=limit)
    after = bytes(uc.mem_read(0x80200000, 0x10000))
    registers_kept = all(uc.reg_read(r) == 0xA5A50000 + i for i, r in enumerate(KEPT))
    return (panel_rx, sh_rx, before, after, uc.reg_read(UC_ARM_REG_PC), uc.reg_read(UC_ARM_REG_SP),
            registers_kept, STACK - lowest[0])


def changed(before, after):
    return sorted(0x80200000 + i for i in range(len(before)) if before[i] != after[i])


def main():
    with open(verify_upd.STOCK, "rb") as f:
        _, segments = djmfw.parse(f.read())
    stock = dict((djmfw.seg_name(h), d) for h, d in segments)["main"]
    patched = T.apply(stock)

    panel_rx, sh_rx, before, after, pc, sp, kept, depth = run_site(machine(patched, True), 2)
    check("returns behind the call", pc == T.RETURN_TO, hex(pc))
    check("stack pointer as before", sp == STACK, hex(sp))
    check("kept registers as before", kept)
    want = set(range(T.TO_SH_TARGET, T.TO_SH_TARGET + 12)) | set(range(T.TO_PANEL_TARGET, T.TO_PANEL_TARGET + 24))
    got = changed(before, after)
    check("only the two targets change", set(got) <= want, [hex(a) for a in got if a not in want][:8])
    o = T.TO_SH_TARGET - 0x80200000
    check("panel bytes 0x04-0x0F reach the SH frame", after[o:o + 12] == panel_rx[4:16])
    o = T.TO_PANEL_TARGET - 0x80200000
    check("SH bytes 0x66-0x7D reach the panel packet", after[o:o + 24] == sh_rx[0x66:0x7E])
    check("memcpy stack use small", depth <= 16, depth)

    for state in (0, 1, 3):
        _, _, before, after, pc, sp, kept, _ = run_site(machine(patched, True), state)
        check(f"state {state}: no copy", before == after and pc == T.RETURN_TO and sp == STACK and kept)

    for mode in (4, 5, 0):
        results = []
        for segment in (stock, patched):
            try:
                results.append(run_site(machine(segment, False), 2, limit=2000000, mode=mode))
            except Exception as error:
                print(f"note: mode {mode}: the real builder could not be run in the emulator ({error})")
                results = None
                break
        if not results:
            continue
        (_, _, _, after_stock, pc_s, sp_s, kept_s, depth_s), \
            (_, sh_rx, before_new, after_new, pc_n, sp_n, kept_n, depth_n) = results
        name = f"real builder, mode {mode}: "
        check(name + "both end behind the call", pc_s == T.RETURN_TO and pc_n == T.RETURN_TO)
        check(name + "stack pointer and registers", sp_n == sp_s == STACK and kept_s and kept_n)
        differ = set(changed(after_stock, after_new))
        check(name + "only our bytes differ from stock", differ <= want,
              [hex(a) for a in sorted(differ - want)][:8])
        check(name + "no deeper stack than stock", depth_n <= depth_s, (depth_n, depth_s))
        o = T.TO_PANEL_TARGET - 0x80200000
        check(name + "SH bytes are in the finished packet", after_new[o:o + 24] == sh_rx[0x66:0x7E])
        print(f"real builder ran in mode {mode}: it changed {len(changed(before_new, after_new))} bytes; "
              f"stack use stock {depth_s} bytes, patched {depth_n} bytes")

    uc = machine(patched, False)
    uc.reg_write(UC_ARM_REG_SP, STACK)
    uc.reg_write(UC_ARM_REG_LR, 0x80700100)
    uc.mem_write(0x80700100, struct.pack("<I", 0xEAFFFFFE))
    uc.emu_start(T.LINK_COMMAND, 0x80700100, count=100)
    check("shell command link returns -1", uc.reg_read(UC_ARM_REG_R0) == 0xFFFFFFFF
          and uc.reg_read(UC_ARM_REG_PC) == 0x80700100 and uc.reg_read(UC_ARM_REG_SP) == STACK)

    print(f"main tunnel: {passed} checks passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
