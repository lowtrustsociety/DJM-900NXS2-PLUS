#!/usr/bin/env python3
"""Tests of the status indicator in the M16C simulator.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PANEL = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(PANEL))
sys.path.insert(0, PANEL)
sys.path.insert(0, HERE)
import build_panel
import screen_dump

BUILD = build_panel.BUILD
TOOLS = build_panel.modbuild.TOOLS
MOD_SRC = os.path.join(ROOT, "rec_control", "panel", "src")
ARM_FRAMES = 30
STALE = 300
SLOT = 30
COLS, ROWS, COL0, ROW0 = 7, 8, 0, 22
X0 = 13
passed = failed = 0


def check(ok, text):
    global passed, failed
    if ok:
        passed += 1
        print("PASS " + text)
    else:
        failed += 1
        print("FAIL " + text)


def run(cmd):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode:
        sys.exit(f"FAILED: {' '.join(cmd)}\n{r.stdout}{r.stderr}")
    return r.stdout


def simulate(name, image, hook, init, defs=()):
    obj, crt, elf = (os.path.join(BUILD, name + e) for e in (".o", "_crt.o", ".elf"))
    flags = ["-Os", "-mcpu=m16c", "-ffreestanding", "-fno-builtin", "-memregs=0", "-Wall", "-Wextra",
             "-Werror", "-Wno-array-bounds", "-I" + MOD_SRC, *defs]
    build_panel.modbuild.compile_c(os.path.join(HERE, "ind_check.c"), obj, flags)
    run([TOOLS + "gcc", "-mcpu=m16c", f'-DSIM_PANEL_FILE="{image}"', f"-DHOOK_ADDR={hook:#x}",
         f"-DINIT_ADDR={init:#x}", "-c", os.path.join(HERE, "crt_ind.S"), "-o", crt])
    run([TOOLS + "ld", "-T", os.path.join(HERE, "sim.ld"), "-o", elf, crt, obj])
    r = subprocess.run([TOOLS + "run", "-v", "-m", "m16c", elf], capture_output=True, text=True, timeout=900)
    out = r.stdout
    if "DONE" not in out:
        sys.exit(f"{name}: the simulator did not finish\n{out[-400:]}{r.stderr[-400:]}")
    steps, queue, ram = [], [], None
    for line in out.splitlines():
        if line.startswith("T "):
            _, tag, at = line.split()
            steps.append((tag, int(at, 16)))
        elif line.startswith("Q "):
            parts = line.split()
            b = bytes.fromhex(parts[4])
            queue.append((int(parts[1], 16), int(parts[2], 16), int(parts[3], 16),
                          int.from_bytes(b[0:4], "little"), b[4], b[5], b[6], b[7],
                          bytes.fromhex(parts[5]) if len(parts) > 5 else None))
        elif line.startswith("RAM "):
            ram = bytes.fromhex(line.split()[1])
    insns = int(out.split("insns:")[1].split()[0].replace(",", "")) if "insns:" in out else 0
    return steps, queue, ram, insns


def font():
    src = open(os.path.join(PANEL, "src", "recstat.c")).read()
    body = src[src.index("font[38][7] = {"):]
    body = body[:body.index("};")]
    rows = re.findall(r"\{((?:0x[0-9A-Fa-f]{2},? ?){7})\}", body)
    return [[int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", r)] for r in rows]


FONT = font()
G_COLON, G_R, G_E, G_A, G_D, G_Y, G_RING, G_DOT, G_SQUARE, G_WARN, G_H, G_M = range(10, 22)
G_F, G_U, G_L, G_O, G_N, G_S, G_B, G_MINUS = range(22, 30)
CAUSE_TEXT = {1: (G_F, G_U, G_L, G_L), 2: (G_E, G_R, G_R, G_O, G_R), 3: (G_N, G_O, None, G_U, G_S, G_B), 4: ()}
CAUSE_OF_REASON = {4: 1, 5: 2, 2: 3}


def compose(kind, hours, minutes, seconds, warn, cause=0, dark=0, count=0):
    px = [[0] * (COLS * 8) for _ in range(ROWS)]

    def glyph(x, g):
        for r in range(7):
            for k in range(5):
                if FONT[g][r] >> (4 - k) & 1:
                    px[r][x + k] = 1

    def two(x, value):
        glyph(x, value // 10)
        glyph(x + 6, value % 10)
        return x + 12

    if warn or not dark:
        glyph(X0, G_WARN if warn else G_RING if kind == 1 else G_DOT if kind == 2 else G_SQUARE)
    if kind == 0:
        for i, g in enumerate(CAUSE_TEXT[cause]):
            if g is not None:
                glyph(X0 + 7 + 6 * i, g)
    elif kind == 1:
        for x, g in ((X0 + 7, G_R), (X0 + 13, G_E), (X0 + 19, G_A), (X0 + 25, G_D), (X0 + 31, G_Y)):
            glyph(x, g)
    else:
        x = X0 + 7
        if count:
            glyph(x, G_MINUS)
            x = two(x + 6, minutes)
            glyph(x, G_COLON)
            two(x + 3, seconds)
        elif hours:
            glyph(x, min(hours, 9))
            glyph(x + 6, G_H)
            x = two(x + 12, minutes)
            glyph(x, G_M)
        else:
            x = two(x, minutes)
            glyph(x, G_COLON)
            two(x + 3, seconds)
    out = bytearray([COLS * 8, ROWS])
    for r in range(ROWS):
        for c in range(COLS):
            out.append(sum(px[r][c * 8 + k] << (7 - k) for k in range(8)))
    return bytes(out)


SCRIPT = [
    ("window", 29, 3, 1, 0, 2, 0, 0, 1, 2), ("ready", 60, 3, 1, 0, 1, 0, 0, 0, 0),
    ("rec7", 60, 3, 1, 0, 2, 0, 0, 0, 7), ("rec0959", 60, 3, 1, 0, 2, 0, 0, 9, 59),
    ("rec5959", 30, 3, 1, 0, 2, 0, 0, 59, 59), ("rec1h", 30, 3, 1, 0, 2, 0, 1, 0, 0),
    ("rec12h", 30, 3, 1, 0, 2, 0, 12, 34, 56), ("warn", 30, 3, 1, 0, 2, 4, 12, 34, 57),
    ("badsum", 30, 3, 1, 1, 2, 0, 1, 1, 1), ("badformat", 30, 3, 1, 2, 2, 0, 1, 1, 1),
    ("back", 30, 3, 1, 0, 2, 4, 12, 34, 58), ("stale", 320, 3, 0, 0, 2, 4, 12, 34, 58),
    ("stillstale", 320, 3, 0, 0, 2, 4, 12, 34, 58), ("again", 30, 3, 1, 0, 2, 0, 0, 0, 1),
    ("utility", 40, 0x80, 1, 0, 2, 0, 0, 0, 2), ("normal", 40, 3, 1, 0, 2, 0, 0, 0, 3),
    ("normal2", 40, 3, 1, 0, 3, 0, 0, 0, 5),
    ("notready", 40, 3, 1, 0, 0, 0, 0, 0, 0), ("nostatus", 40, 3, 0, 3, 0, 0, 0, 0, 0),
    ("ready2", 40, 3, 1, 0, 1, 0, 0, 0, 0), ("fx", 40, 7, 1, 0, 1, 0, 0, 0, 0),
    ("logo", 40, 0xF8, 1, 0, 2, 0, 0, 0, 9), ("afterlogo", 40, 7, 1, 0, 2, 0, 0, 0, 10),
    ("quiet", 40, 7, 0, 3, 0, 0, 0, 0, 0), ("quiet2", 300, 7, 0, 3, 0, 0, 0, 0, 0),
    ("utility2", 10, 0x80, 0, 3, 0, 0, 0, 0, 0), ("normal3", 10, 7, 0, 3, 0, 0, 0, 0, 0),
    ("full", 40, 7, 1, 0, 0, 8, 0, 0, 0), ("error", 40, 7, 1, 0, 0, 8, 0, 0, 0),
    ("nousb", 40, 7, 1, 0, 0, 8, 0, 0, 0), ("looking", 40, 7, 1, 0, 0, 8, 0, 0, 0),
    ("readyagain", 40, 7, 1, 0, 1, 8, 0, 0, 0), ("newtake", 40, 7, 1, 0, 2, 0, 0, 0, 1),
    ("fullnoflag", 40, 7, 1, 0, 0, 0, 0, 0, 0), ("ready3", 40, 7, 1, 0, 1, 0, 0, 0, 0),
    ("blink", 210, 7, 1, 0, 2, 0, 0, 0, 2), ("count", 110, 7, 1, 0, 2, 0, 1, 31, 2),
    ("count59", 40, 7, 1, 0, 2, 0, 1, 1, 0),
    ("count7", 40, 7, 1, 0, 2, 4, 1, 59, 53), ("countbad", 30, 7, 1, 0, 2, 0, 0, 0, 1),
    ("countoff", 40, 7, 1, 0, 2, 0, 1, 59, 55),
    ("text", 20, 0xFF, 1, 0, 2, 0, 0, 0, 4), ("aftertext", 80, 7, 1, 0, 2, 0, 0, 0, 30),
    ("utility3", 20, 0x80, 1, 0, 2, 0, 0, 0, 31), ("normal4", 40, 7, 1, 0, 1, 0, 0, 0, 0),
]
WHY = {"full": 4, "error": 5, "nousb": 2, "looking": 1, "fullnoflag": 4}
LEFT = {"count": (30, 58), "count59": (60, 59), "count7": (1, 7), "countbad": (61, 7)}


def expected(repaints):
    out = {}
    frame = counter = 0
    seen = fresh = False
    last_counter = age = 0
    said = (0, 0, 0, 0, 0, 0, 0, 0, 0)
    shown = (0,) * 8
    normal = False
    off = False
    for tag, n, mode, live, spoil, state, flags, h, m, s in SCRIPT:
        for i in range(n):
            if live and i % 25 == 0:
                counter = (counter + 1) & 0xFF
            if mode == 0xFF:
                off = True
            if frame >= ARM_FRAMES - 1 and not off:
                left, left_seconds = LEFT.get(tag, (0, 0))
                valid = spoil == 0 and left <= 60
                if valid and (not seen or counter != last_counter):
                    last_counter, age, seen, fresh = counter, 0, True, True
                    said = (state if state <= 3 else 0, min(h, 9), m, 0 if h else s, 1 if flags & 4 else 0,
                            1 if flags & 8 else 0, WHY.get(tag, 0), left, left_seconds)
                elif fresh:
                    age += 1
                    if age > STALE:
                        fresh = False
                kind = said[0] if fresh else 0
                stopped = fresh and said[5]
                want = [kind, said[1] if kind >= 2 else 0, said[2] if kind >= 2 else 0,
                        said[3] if kind >= 2 else 0, 1 if stopped else (said[4] if kind else 0),
                        CAUSE_OF_REASON.get(said[6], 4) if stopped and kind == 0 else 0, 0, 0]
                if kind == 2:
                    want[6] = 1 if not want[4] and last_counter & 2 else 0
                    if said[7]:
                        want[1:4] = [0, said[7] - 1, said[8]]
                        want[7] = 1
                want = tuple(want)
                if mode >= 0x80:
                    normal = False
                else:
                    force = not normal
                    if force or frame in repaints:
                        shown = (0,) * 8
                    normal = True
                    if want != shown or force:
                        out[frame] = None if want[0] == 0 and not want[5] else compose(*want)
                    shown = want
            frame += 1
    return out


def main():
    stock_segment = build_panel.stock_panel()
    patched, size = build_panel.patch(stock_segment)
    patched_file = os.path.join(BUILD, "panel_rec_image.bin")
    stock_file = os.path.join(BUILD, "panel_stock_image.bin")
    open(patched_file, "wb").write(patched)
    open(stock_file, "wb").write(stock_segment)
    _, syms, _, _ = build_panel.compile_panel()

    differing = [i for i in range(len(patched)) if patched[i] != stock_segment[i]]
    inside = all(any(lo <= i + 0xC0000 < hi for lo, hi in build_panel.ALLOWED) for i in differing)
    check(inside and patched[0x30000:] == stock_segment[0x30000:] and len(patched) == len(stock_segment),
          f"panel image: {len(differing)} bytes differ from stock, all in the four redirected calls and "
          f"the two code areas; the boot area 0xF0000-0xFFFFF is untouched; code {size} bytes")

    s_steps, s_queue, s_ram, s_insns = simulate("ind_stock", stock_file, 0xC68F0, 0xC6836)
    p_steps, p_queue, p_ram, p_insns = simulate("ind_patched", patched_file, syms["_mod_entry"],
                                                syms["_mod_init_entry"])
    total = sum(x[1] for x in SCRIPT)
    check([t for t, _ in p_steps] == [x[0] for x in SCRIPT] and p_steps == s_steps,
          f"both images ran the same {len(SCRIPT)} steps, {total} packets from main")

    mine = [q for q in p_queue if q[1] == 0 and q[2] == SLOT]
    others = [q for q in p_queue if not (q[1] == 0 and q[2] == SLOT)]
    check(not any(q[2] == SLOT and q[1] == 0 for q in s_queue),
          "the stock image never uses queue slot 30 of the upper display")
    check(others == s_queue,
          f"everything the stock renderer queues ({len(s_queue)} pictures and clearings) is queued "
          f"identically by the patched image, frame for frame")
    repaints = {q[0] for q in s_queue if q[1] == 0 and q[4] <= COL0 + COLS - 1 and q[6] >= COL0
                and q[5] <= ROW0 + ROWS - 1 and q[7] >= ROW0}
    logos = {q[0] for q in s_queue if q[1] == 0 and q[3] == 0xD2000}
    want = expected(repaints)
    got = {}
    windows_ok = True
    for frame, disp, slot, pointer, c0, r0, c1, r1, picture in mine:
        windows_ok &= (c0, r0, c1, r1) == (COL0, ROW0, COL0 + COLS - 1, ROW0 + ROWS - 1)
        windows_ok &= frame not in got
        got[frame] = None if pointer == 0xFFFFFFFF else picture
    check(windows_ok, "the indicator's window is always columns 0-6, rows 22-29; one entry a frame at most")
    check(got == want,
          f"the indicator queues exactly what the model expects: {len(want)} entries "
          f"({sum(1 for v in want.values() if v is None)} clearings), every picture bit for bit"
          + ("" if got == want else f"; frames only expected {sorted(set(want) - set(got))[:8]}, only got "
             f"{sorted(set(got) - set(want))[:8]}, different {[f for f in want if f in got and want[f] != got[f]][:8]}"))

    def at(tag):
        return dict(p_steps)[tag]

    def events(tag):
        lo = at(tag)
        hi = lo + [x[1] for x in SCRIPT if x[0] == tag][0]
        return {f: v for f, v in got.items() if lo <= f < hi}

    check(not events("window") and min(got) == ARM_FRAMES - 1,
          f"power-on interlock: nothing during the first {ARM_FRAMES - 1} packets, the first picture "
          f"with packet {ARM_FRAMES}")
    check(list(events("ready").values()) == [compose(1, 0, 0, 0, 0)], "ready: ring and READY, drawn once")
    def both(*a, **k):
        return {compose(*a, **k), compose(*a, dark=1, **k)}

    def only(tag, *a, **k):
        seen = set(events(tag).values())
        return bool(seen) and seen <= both(*a, **k)

    check(only("rec7", 2, 0, 0, 7, 0) and only("rec0959", 2, 0, 9, 59, 0) and only("rec5959", 2, 0, 59, 59, 0)
          and only("rec1h", 2, 1, 0, 0, 0) and only("rec12h", 2, 12, 34, 56, 0),
          "recording: dot and 00:07, 09:59, 59:59, then 1h00m and (12 h, capped) 9h34m, drawn when "
          "the time changes and when the dot goes dark or comes back, and never otherwise")
    check(list(events("warn").values()) == [compose(2, 12, 34, 57, 1)], "audio dropped: a ! in place of the dot")
    check(not events("badsum") and not events("badformat"),
          "a status with a wrong check byte or another format is ignored: the display stays")
    last_new = at("back") + 25
    check(list(events("stale").items()) == [(last_new + STALE + 1, None)] and not events("stillstale"),
          f"the counter stops: the indicator is cleared {STALE + 1} packets (3 s) after the last new "
          f"status, once")
    check(only("again", 2, 0, 0, 1, 0), "the counter moves again: shown again")
    check(not events("utility"), "UTILITY menu: the indicator queues nothing")
    check(min(events("normal")) == at("normal") and only("normal", 2, 0, 0, 3, 0)
          and at("normal") in repaints
          and list(events("normal2").values()) == [compose(3, 0, 0, 5, 0)],
          "back on the normal screen (which the stock code rebuilds from blank): drawn again in the "
          "same frame; closing shows a square")
    check(list(events("notready").values()) == [None] and not events("nostatus")
          and list(events("ready2").values()) == [compose(1, 0, 0, 0, 0)] and not events("fx"),
          "not ready: cleared; no status at all: nothing; ready again: drawn; another effect: untouched")
    check(at("logo") in logos and not events("logo")
          and min(events("afterlogo")) == at("afterlogo") and only("afterlogo", 2, 0, 0, 10, 0),
          "main shows the full-screen logo: nothing is drawn over it; back on the normal screen the "
          "indicator is drawn again at once")
    check(list(events("normal3").items()) == [(at("normal3"), None)] and len(repaints) >= 4,
          f"back on the normal screen with nothing to show: its place is cleared; the stock code "
          f"repainted the place in {len(repaints)} frames of the run, each one noticed")

    check(list(events("full").values()) == [compose(0, 0, 0, 0, 1, 1)]
          and list(events("error").values()) == [compose(0, 0, 0, 0, 1, 2)]
          and list(events("nousb").values()) == [compose(0, 0, 0, 0, 1, 3)]
          and list(events("looking").values()) == [compose(0, 0, 0, 0, 1, 4)],
          "the last recording ended by itself: ! FULL, ! ERROR, ! NO USB while that is why "
          "the recorder is not ready, the ! alone while a stick is looked at; each drawn once")
    check(list(events("readyagain").values()) == [compose(1, 0, 0, 0, 1)]
          and only("newtake", 2, 0, 0, 1, 0)
          and list(events("fullnoflag").values()) == [None]
          and list(events("ready3").values()) == [compose(1, 0, 0, 0, 0)],
          "... ready again: a ! in place of the ring; the next recording: the dot as always; not "
          "ready without that flag: nothing, as before")
    blink = [v for _, v in sorted(events("blink").items())]
    lit, dark = compose(2, 0, 0, 2, 0), compose(2, 0, 0, 2, 0, dark=1)
    frames = sorted(events("blink"))
    check(len(blink) >= 4 and set(blink) == {lit, dark} and all(a != b for a, b in zip(blink, blink[1:]))
          and all(b - a == 50 for a, b in zip(frames[1:], frames[2:])),
          f"recording: the dot goes dark and comes back in turn, every 50 packets (half a "
          f"second: two of the recorder's four statuses a second each), {len(blink)} changes in 2.1 s; "
          f"the time stays")
    count = [v for _, v in sorted(events("count").items())]
    check(len(count) >= 2 and set(count) == both(2, 0, 29, 58, 0, count=1)
          and only("count59", 2, 0, 59, 59, 0, count=1)
          and list(events("count7").values()) == [compose(2, 0, 0, 7, 1, count=1)],
          "under 30 minutes until the stick is full: -29:58 in place of the file's time (1h31m), "
          "the dot still flashing; the countdown from 60 minutes, -59:59 fits as well; with "
          "audio lost in the file the ! stands still: ! -00:07")
    check(not events("countbad") and only("countoff", 2, 1, 59, 0, 0),
          "... a status whose countdown byte is out of range is not taken (the display stays); "
          "without a countdown the file's time is back: 1h59m")
    check(not events("text") and not events("aftertext") and not events("utility3")
          and not events("normal4"),
          "after main's text screen (its updater) the indicator stays off for the rest of the "
          "session, whenever that screen comes")

    def offset(address):
        o = address - 0x0404
        if address >= 0x4400:
            o -= 0x4400 - 0x2000
        if address >= 0x4F00:
            o -= 0x4F00 - 0x4E00
        return o
    slot = 0x17D2 + SLOT * 9
    diff = [i for i in range(min(len(s_ram), len(p_ram))) if s_ram[i] != p_ram[i]]
    allowed = set(range(offset(slot), offset(slot + 9)))
    check(len(s_ram) == len(p_ram) and set(diff) <= allowed,
          f"panel RAM after all {total} packets: identical to the stock image's except the "
          f"indicator's queue slot ({len(diff)} bytes differ there)"
          + ("" if set(diff) <= allowed else f"; ALSO {[hex(d) for d in diff if d not in allowed][:10]}"))

    n_steps, n_queue, n_ram, _ = simulate("ind_noinit", patched_file, syms["_mod_entry"],
                                          syms["_mod_init_entry"], ["-DNO_INIT"])
    check(not any(q[1] == 0 and q[2] == SLOT for q in n_queue),
          "without the startup hook (RAM as found) the indicator never acts, however long it runs")

    per_frame = (p_insns - s_insns) / total
    print(f"INFO cost: {per_frame:.0f} instructions per packet from main on average "
          f"(stock: {s_insns / total:.0f})")
    check(per_frame < 700, "indicator and menu cost under 700 instructions per packet on average "
          "(the harness calls them eight times a packet)")

    panel = stock_segment
    for name, record in (("ready", compose(1, 0, 0, 0, 0)), ("rec", compose(2, 0, 12, 34, 0)),
                         ("rec_long", compose(2, 12, 34, 56, 1)), ("closing", compose(3, 1, 2, 3, 0)),
                         ("rec_dark", compose(2, 0, 12, 34, 0, dark=1)), ("countdown", compose(2, 0, 29, 58, 0, count=1)),
                         ("stopped_full", compose(0, 0, 0, 0, 1, 1)), ("stopped_error", compose(0, 0, 0, 0, 1, 2)),
                         ("stopped_no_usb", compose(0, 0, 0, 0, 1, 3)), ("stopped_ready", compose(1, 0, 0, 0, 1))):
        canvas = screen_dump.Canvas(0)
        for frame, disp, slot_, pointer, c0, r0, c1, r1 in screen_dump.simulate(
                3, "RAM8(RX_BUF_BASE+0x2D)=1;RAM8(RX_BUF_BASE+0x2E)=0x14;RAM8(RX_BUF_BASE+0x2F)=0x05;"
                   "RAM8(RX_BUF_BASE+0x30)=4;", name="pic"):
            if disp == 0:
                canvas.apply(panel, slot_, pointer, c0, r0, c1, r1)
        canvas.apply(panel, SLOT, 0x2000, COL0, ROW0, COL0 + COLS - 1, ROW0 + ROWS - 1, ram=lambda _: record)
        screen_dump.save(screen_dump.view(canvas, 0), os.path.join(BUILD, f"indicator_{name}.png"))
    print(f"pictures: {os.path.join(BUILD, 'indicator_*.png')}")
    print(f"panel indicator: {passed} checks passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
