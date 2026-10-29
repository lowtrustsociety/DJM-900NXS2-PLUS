#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""The panel's compiler calls (m32c-elf, for the M16C) and their safety checks."""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import check_flags  # noqa: E402

TOOLS = os.path.expanduser("~/tools/m32c/bin/m32c-elf-")
CFLAGS = ["-Os", "-mcpu=m16c", "-ffreestanding", "-fno-builtin",
          "-fno-jump-tables", "-memregs=0", "-Wall", "-Wextra", "-Werror"]


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def compile_c(src, obj, flags):
    """C -> assembly -> flag-safety check (tools/check_flags.py) -> object.
    Used for the mod and for every simulator harness."""
    asm = obj[:-2] + ".s"
    run([TOOLS + "gcc", *flags, "-S", src, "-o", asm])
    problems = check_flags.check(asm)
    if problems:
        sys.exit("conditional jump not fed by a compare (GCC m32c flags bug):\n  "
                 + "\n  ".join(problems))
    run([TOOLS + "gcc", "-mcpu=m16c", "-c", asm, "-o", obj])
    return obj


def check_far_rodata(objs):
    """On the M16C a near pointer only reaches 0x0000-0xFFFF, but the mod's
    constants live in flash at 0xEFxxx. Every access to constant data must
    therefore be a far access (LO16/HI16 relocations via LDE). Reject any
    other relocation that points into a read-only data section."""
    for obj in objs:
        syms = run([TOOLS + "objdump", "-t", obj])
        ro = {l.split()[-1] for l in syms.splitlines()
              if " .rodata" in l or " .frodata" in l}
        section = None
        for line in run([TOOLS + "objdump", "-r", obj]).splitlines():
            if line.startswith("RELOCATION RECORDS FOR"):
                section = line.split("[")[1].split("]")[0]
                continue
            f = line.split()
            if len(f) != 3 or not f[0][0].isdigit():
                continue
            rtype, target = f[1], f[2].split("+")[0]
            if (target in ro or target.startswith((".rodata", ".frodata"))) \
                    and rtype not in ("R_M32C_LO16", "R_M32C_HI16"):
                sys.exit(f"{os.path.basename(obj)}: near access to constant "
                         f"'{f[2]}' ({rtype} in {section}); declare it __far")


def link(objs, script, out):
    run([TOOLS + "ld", "-T", script, "-Map", out + ".map", "-o", out, *objs])
    return out


def symbols(elf):
    syms = {}
    for line in run([TOOLS + "nm", elf]).splitlines():
        addr, _, name = line.split()
        syms[name] = int(addr, 16)
    return syms

