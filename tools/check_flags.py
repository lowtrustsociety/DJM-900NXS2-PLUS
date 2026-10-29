#!/usr/bin/env python3
"""Refuse M16C assembly where a conditional jump does not follow its compare (a GCC 14 m32c bug).

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import re
import sys

JCC = re.compile(r"^\s*j(eq|ne|z|nz|c|nc|gt|ge|lt|le|gtu|geu|ltu|leu|n|pz|o|no)\s")
SETTER = re.compile(r"^\s*(cmp|tst|btst)\b")
MARKER = re.compile(r"^\s*;\s*cmp\.[bw]\s+#0\s*,\s*(\w+)\s*$")
MOVSRC = re.compile(r"^\s*mov\.[bw](?::\w)?\s+(\w+)\s*,")


def check(path):
    lines = [l.rstrip("\n") for l in open(path)]
    problems = []
    for i, line in enumerate(lines):
        if not JCC.match(line):
            continue
        j = i - 1
        while j >= 0 and (not lines[j].strip() or lines[j].strip().startswith(".")):
            j -= 1
        prev = lines[j] if j >= 0 else ""
        if SETTER.match(prev):
            continue
        m = MARKER.match(prev)
        if m:
            k = j - 1
            while k >= 0 and (not lines[k].strip() or lines[k].strip().startswith(".")):
                k -= 1
            mm = MOVSRC.match(lines[k]) if k >= 0 else None
            if mm and mm.group(1) == m.group(1):
                continue
        problems.append(f"{path}:{i + 1}: {prev.strip()!r} -> {line.strip()!r}")
    return problems


if __name__ == "__main__":
    bad = [p for f in sys.argv[1:] for p in check(f)]
    for p in bad:
        print("FLAGS:", p)
    sys.exit(1 if bad else 0)
