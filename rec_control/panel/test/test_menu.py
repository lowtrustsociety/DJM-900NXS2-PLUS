#!/usr/bin/env python3
"""Tests of the menu in the M16C simulator.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""
import os
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
PANEL = os.path.dirname(HERE)
ROOT = os.path.dirname(os.path.dirname(PANEL))
sys.path.insert(0, PANEL)
sys.path.insert(0, HERE)
import build_panel
import screen_dump
import re

BUILD = build_panel.BUILD
TOOLS = build_panel.modbuild.TOOLS
MOD_SRC = os.path.join(ROOT, "rec_control", "panel", "src")
W, H = 192, 64
LEFT, RIGHT, RULE, HINT1, HINT3, HINT4 = 0xDE522, 0xDE706, 0xDE8EA, 0xDE9AC, 0xDEB30, 0xDECB4
ITEM_PICTURE = [None, 1, 2, 3, 5, 6, 4, 7, 8, 9, 10, 11, 0x45]
KEY_PREV, KEY_NEXT, KEY_ENTER, KEY_TAP7, KEY_QUANTIZE, KEY_BACK = 0x01, 0x02, 0x08, 0x80, 0x04, 0x08


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


def simulate(name, image, hook, init, builder, defines=()):
    obj, crt, elf = (os.path.join(BUILD, name + e) for e in (".o", "_crt.o", ".elf"))
    flags = ["-Os", "-mcpu=m16c", "-ffreestanding", "-fno-builtin", "-memregs=0", "-Wall", "-Wextra",
             "-Werror", "-Wno-array-bounds", "-I" + MOD_SRC, "-I" + os.path.join(PANEL, "src"), *defines]
    build_panel.modbuild.compile_c(os.path.join(HERE, "menu_check.c"), obj, flags)
    run([TOOLS + "gcc", "-mcpu=m16c", f'-DSIM_PANEL_FILE="{image}"', f"-DHOOK_ADDR={hook:#x}",
         f"-DINIT_ADDR={init:#x}", f"-DBUILDER_ADDR={builder:#x}", "-c",
         os.path.join(HERE, "crt_menu.S"), "-o", crt])
    run([TOOLS + "ld", "-T", os.path.join(HERE, "sim.ld"), "-o", elf, crt, obj])
    r = subprocess.run([TOOLS + "run", "-m", "m16c", elf], capture_output=True, text=True, timeout=1800)
    lines = r.stdout.splitlines()
    if not lines or lines[-1] != "D":
        sys.exit(f"{name}: the simulator did not finish\n{r.stdout[-600:]}{r.stderr[-400:]}")
    return lines


def characters(text):
    out = {}
    text = "".join(sorted(set(text)))
    for at in range(0, len(text), 12):
        part = text[at:at + 12].ljust(12)
        setter = ('RAM8(RX_BUF_BASE + 0x2D)=3;{u8 k;const char*t="%s";for(k=0;k<12;k++)'
                  'RAM8(RX_BUF_BASE + 0x2E + k)=(u8)t[k];}' % part)
        for frame, disp, slot, pointer, c0, r0, c1, r1 in screen_dump.simulate(255, setter, frames=3,
                                                                              name="chars"):
            if disp == 1 and c1 - c0 == 1 and r1 - r0 == 23 and slot < 12 and c0 == 2 * slot:
                out[part[slot]] = pointer
    return out


class Expect:

    def __init__(self, panel, chars):
        self.panel, self.chars = panel, chars
        self.rows = [bytearray(24) for _ in range(H)]

    def picture(self, address, column, row):
        w, h, data = screen_dump.image(self.panel, address)
        for y in range(h):
            for x in range(w // 8):
                self.rows[row + y][column + x] = data[y * (w // 8) + x]
        return self

    def text(self, line, places, first_column):
        column = first_column + 2 * ((places - len(line)) // 2) + (places - len(line)) % 2
        for i, c in enumerate(line):
            if c == " ":
                continue
            if c in ":!":
                for y in range(24):
                    dot = y in (7, 8, 9, 15, 16, 17) if c == ":" else (3 <= y <= 14 or 18 <= y <= 20)
                    self.rows[8 + y][column + 2 * i] = 0x01 if dot else 0
                    self.rows[8 + y][column + 2 * i + 1] = 0x80 if dot else 0
            else:
                self.picture(self.chars[c], column + 2 * i, 8)
        return self


MASTER_OUT, BOOTH_OUT, LOCK, PEAK_LIMITER = 0xDF8BC, 0xDFC7E, 0xE2212, 0xE389E
MIDI, OFF, TOGGLE, NO, TALK_OVER, MIC_LOW_CUT, YES = 0xDED76, 0xE9A52, 0xE8004, 0xEA1D6, 0xDF138, 0xE0B86, 0xE9E14
CH = 0xE2996
DIGIT_PICTURES = [0xEA598, 0xE43E4, 0xE47A6, 0xE4B68, 0xE4F2A, 0xE52EC, 0xE56AE, 0xE5A70, 0xE5E32, 0xE61F4]
LETTER_SOURCE = {"M": (MASTER_OUT, 0), "A": (MASTER_OUT, 1), "S": (MASTER_OUT, 2), "T": (MASTER_OUT, 3),
                 "E": (MASTER_OUT, 4), "R": (MASTER_OUT, 5), "O": (MASTER_OUT, 6), "U": (MASTER_OUT, 7),
                 "B": (BOOTH_OUT, 0), "C": (LOCK, 2), "K": (LOCK, 3), "L": (LOCK, 0), "P": (PEAK_LIMITER, 0),
                 "D": (MIDI, 2), "I": (MIDI, 1), "F": (OFF, 1), "G": (TOGGLE, 2), "N": (NO, 0),
                 "V": (TALK_OVER, 5), "W": (MIC_LOW_CUT, 5), "Y": (YES, 0), "-": (0xEBC24, 0), "H": (CH, 1),
                 **{str(d): (a, 0) for d, a in enumerate(DIGIT_PICTURES)}}
LETTER_GAP = {("M", "A"): 4, ("S", "T"): 2, ("T", "A"): 1, ("T", "O"): 2}


def pixel(panel, address, x, y):
    w, h, data = screen_dump.image(panel, address)
    return data[y * (w // 8) + x // 8] >> (7 - x % 8) & 1


def runs(panel, address):
    w, h, _ = screen_dump.image(panel, address)
    lit = [any(pixel(panel, address, x, y) for y in range(h)) for x in range(w)] + [False]
    out, start = [], None
    for x, on in enumerate(lit):
        if on and start is None:
            start = x
        elif not on and start is not None:
            out.append((start, x - start))
            start = None
    return out


def letter(panel, c):
    address, n = LETTER_SOURCE[c]
    first, width = runs(panel, address)[n]
    return address, first, width


def glyph(panel, c):
    def stock(ch, row_of=lambda r: r):
        a, f, w = letter(panel, ch)
        return [[pixel(panel, a, f + k, 11 + row_of(r)) for k in range(w)] for r in range(18)]
    if c == "X":
        return stock("Y", lambda r: r if r < 8 else 9 if r < 10 else 17 - r)
    if c == "!":
        return [row if not 12 <= r <= 14 else [0, 0] for r, row in enumerate(stock("I"))]
    if c == ":":
        return [[1, 1, 1] if r in (4, 5, 6, 11, 12, 13) else [0, 0, 0] for r in range(18)]
    return stock(c)


def stock_text(panel, rows, y0, text):
    def layout():
        x, prev, out = 0, None, []
        for c in text:
            if c == " ":
                x, prev = x + 8, None
                continue
            if prev is not None:
                x += LETTER_GAP.get((prev, c), 3) - 3
            g = glyph(panel, c)
            w = len(g[0])
            lead = (16 - w) // 2 if c.isdigit() else 2 if c == ":" else 0
            out.append((x + lead, g))
            x += 16 if c.isdigit() else lead + w + 3
            prev = c
        return out
    placed = layout()
    end = max((x + len(g[0]) for x, g in placed), default=0)
    x0 = (192 - end + 1) // 2 if end < 192 else 0
    for x, g in placed:
        for r in range(18):
            for k, on in enumerate(g[r]):
                if on and x0 + x + k < 192:
                    rows[y0 + r][(x0 + x + k) // 8] |= 0x80 >> ((x0 + x + k) % 8)


def upper(panel, chars, text=None, item=None, left=False, right=False):
    e = Expect(panel, chars)
    if text is not None:
        stock_text(panel, e.rows, 11, text)
    if item is not None:
        e.picture(item, 0, 0)
    if left:
        e.picture(LEFT, 0, 40)
    if right:
        e.picture(RIGHT, 14, 40)
    return e.rows


def lower(panel, chars, text=None, hint=HINT1):
    e = Expect(panel, chars)
    if text is not None:
        stock_text(panel, e.rows, 11, text)
    e.picture(RULE, 0, 44)
    if hint is not None:
        e.picture(hint, 0, 52)
    return e.rows


def small_font():
    src = open(os.path.join(PANEL, "src", "recstat.c")).read()
    body = src[src.index("font[38][7] = {"):]
    body = body[:body.index("};")]
    rows = re.findall(r"\{((?:0x[0-9A-Fa-f]{2},? ?){7})\}", body)
    return [[int(x, 16) for x in re.findall(r"0x[0-9A-Fa-f]{2}", r)] for r in rows]


SMALL = small_font()
SMALL_CODE = dict({str(d): d for d in range(10)}, **{":": 10, "E": 12, "A": 13, "U": 23, "L": 24, "S": 27, "B": 28,
                                                      "-": 29, "T": 30, "I": 31, "M": 32, "P": 33, "V": 34,
                                                      "C": 35, "G": 36, ".": 37})


def small_width(text):
    return sum(4 if c == " " else 3 if c in ":." else 6 for c in text) - 1


def small_text(rows, x, y, text):
    for c in text:
        if c != " ":
            for r, bits in enumerate(SMALL[SMALL_CODE[c]]):
                for k in range(5):
                    if bits >> (4 - k) & 1:
                        rows[y + r][(x + k) // 8] |= 0x80 >> ((x + k) % 8)
        x += 4 if c == " " else 3 if c in ":." else 6


def about_lower(panel, chars, time, space, hint=HINT4):
    e = Expect(panel, chars)
    small_text(e.rows, 4, 10, "TIME " + time)
    small_text(e.rows, 188 - small_width("SPACE " + space), 10, "SPACE " + space)
    plus = "PLUS V" + build_panel.version_text("000")
    small_text(e.rows, 188 - small_width(plus), 30, plus)
    e.picture(RULE, 0, 44)
    e.picture(hint, 0, 52)
    return e.rows


def show(rows):
    return [[rows[y][x // 8] >> (7 - x % 8) & 1 for x in range(W)] for y in range(H)]


def main():
    stock = build_panel.stock_panel()
    patched, size = build_panel.patch(stock)
    patched_file = os.path.join(BUILD, "panel_rec_image.bin")
    open(patched_file, "wb").write(patched)
    _, syms, _, _ = build_panel.compile_panel()
    lines = simulate("menu_patched", patched_file, syms["_mod_entry"], syms["_mod_init_entry"],
                     syms["_tx_tramp"])
    at = build_panel.VER_PICTURE - build_panel.PANEL_BASE
    differ = [i for i in range(len(stock)) if stock[i] != patched[i]]
    inside = all(any(lo <= build_panel.PANEL_BASE + i < hi for lo, hi in build_panel.ALLOWED) for i in differ)
    rows = ["".join("#" if patched[at + 2 + 2 * y + x // 8] >> (7 - x % 8) & 1 else "." for x in range(16))
            for y in range(8)]
    check(patched[at:at + 2] == stock[at:at + 2] == b"\x10\x08" and rows[0] == "###.#...#.#.###."
          and rows[6] == "#...###.###.###." and inside,
          "the start-up's small Ver. picture (16 x 8) reads PLUS, its size unchanged; every byte "
          "of the panel that differs from stock lies in the places allowed: " + " / ".join(rows[:7]))
    shown = ("REC START STOP MARKER CUT READY SAVING NOT READY INIT NO USB USB FULL USB ERROR "
             "WAIT LOW SPACE MAX 100 ABOUT 0123456789 - MF BIT DEPTH 16 BIT 24 BIT EXPERIMENTAL")
    wanted = set(shown + "!:XW") - set(" ")
    chars = {}
    check(wanted <= set(LETTER_SOURCE) | set("X!:"),
          f"every one of the {len(wanted)} characters the two displays show is a glyph of a "
          f"stock picture, or made from them (X, ! and the colon)")

    def play(lines):
        canvases = [screen_dump.Canvas(0), screen_dump.Canvas(1)]
        snapshots, packets, tags = {}, [], {}
        names = {}
        for line in lines:
            if line.startswith("Q "):
                parts = line.split()
                frame, disp, slot = int(parts[1], 16), int(parts[2], 16), int(parts[3], 16)
                b = bytes.fromhex(parts[4])
                pointer = int.from_bytes(b[0:4], "little")
                record = bytes.fromhex(parts[5]) if len(parts) > 5 else None
                canvases[disp].apply(patched, slot, pointer, b[4], b[5], b[6], b[7], ram=lambda _: record)
                if disp == 0 and slot == 16 and 0xC0000 <= pointer < 0x100000:
                    names[frame] = pointer
            elif line.startswith("X "):
                _, frame, data, keys = line.split()
                packets.append((int(frame, 16), bytes.fromhex(data), bytes.fromhex(keys)))
            elif line.startswith("T "):
                _, number, frame = line.split()
                tags[int(number, 16)] = int(frame, 16)
                snapshots[int(number, 16)] = ([bytes(r) for r in canvases[0].seen],
                                              [bytes(r) for r in canvases[1].seen])
        return snapshots, packets, tags, names

    snapshots, packets, tags, names = play(lines)

    def between(a, b):
        lo = tags[a] if a is not None else 0
        return [p for p in packets if lo <= p[0] < tags[b]]

    def checksum_ok(p):
        return int.from_bytes(p[0x5E:0x60], "little") == (~sum(p[:0x5E])) & 0xFFFF
    check(len(packets) > 900 and all(checksum_ok(p[1]) for p in packets),
          f"all {len(packets)} packets for main that were looked at carry a checksum that fits "
          f"(main's rule: complement of the sum of bytes 0x00-0x5D)")
    real = {bytes([0x60, 0x15, k]) for k in (0, KEY_PREV, KEY_NEXT, KEY_ENTER, KEY_QUANTIZE)} | \
        {bytes([0x60 | KEY_BACK, 0x15, 0])}
    check(all(k in real for _, _, k in packets) and len({k for _, _, k in packets}) == 6,
          "after every packet the button RAM holds what is really pressed again (all six "
          "states of the script seen)")

    def seen_by_main(a, b):
        d = b3 = 0
        for p in between(a, b):
            d |= p[1][0x0D]
            b3 |= p[1][0x0B] & 0x08
        return d, b3

    item = {n: names_for for n, names_for in ()}
    def item_picture(n):
        return item_addresses[n]
    item_addresses = {}
    order = sorted(names.items())
    def name_at(checkpoint):
        frames = [f for f in names if f < tags[checkpoint]]
        return names[max(frames)]
    item_addresses[1] = name_at(2)
    item_addresses[2] = name_at(3)
    item_addresses[3] = name_at(36)
    check(len(set(item_addresses.values())) == 3, "main's entries 1 to 3 are drawn from three stock pictures")

    def screen(checkpoint, up, low, what):
        got_up, got_low = snapshots[checkpoint]
        ok_up, ok_low = [bytes(r) for r in up] == got_up, [bytes(r) for r in low] == got_low
        check(ok_up and ok_low, f"checkpoint {checkpoint}: {what}"
              + ("" if ok_up else " [UPPER display differs]") + ("" if ok_low else " [LOWER display differs]"))
        if not (ok_up and ok_low):
            for name, rows in (("got_upper", got_up), ("want_upper", up), ("got_lower", got_low), ("want_lower", low)):
                screen_dump.save(show(rows), os.path.join(BUILD, f"menu_{checkpoint}_{name}.png"))

    U = lambda **k: upper(stock, chars, **k)
    L = lambda text=None, hint=HINT1: lower(stock, chars, text, hint)
    A = lambda time, space, hint=HINT4: about_lower(stock, chars, time, space, hint)
    m1 = dict(item=item_addresses[1])
    screen(2, U(item=item_addresses[1], right=True), L(), "lock-out: UTILITY opens on main's first entry, no RECORDER")
    d, b = seen_by_main(1, 2)
    check(d & KEY_QUANTIZE and all(p[1][0x0E] == 0 and p[1][0x0F] == 0 for p in between(None, 8)),
          "lock-out: main sees the button; no command bytes")
    screen(8, U(item=item_addresses[1], left=True, right=True), L(),
           "the lock-out ends with the menu open: main's entry stays untouched and gets the arrow back")
    screen(9, U(text="REC", right=True), L(), "... and BEAT < leads to REC, cleanly")
    screen(0x50, U(item=item_addresses[1], left=True, right=True), L(), "... BEAT > back to main's entry")
    screen(3, U(item=item_addresses[2], left=True, right=True), L(), "... BEAT > again: main's second entry")
    screen(4, U(item=item_addresses[1], left=True, right=True), L(), "... BEAT <: the first, with the arrow to RECORDER")

    screen(0xE0, U(item=item_addresses[1], left=True, right=True), L(),
           "UTILITY opens on MASTER OUT as the stock firmware does, with the arrow to the left")
    screen(10, U(text="REC", right=True), L(),
           "BEAT <: REC, in the letters of the stock entry names; the lower display shows only "
           "[TAP]:ENTER, as main's entries")
    def lit_rows(rows, columns=range(24)):
        return [y for y in range(40) if any(rows[y][c] for c in columns)]
    sources = {c: letter(stock, c) for c in LETTER_SOURCE}
    check(all(not any(pixel(stock, a, x, y) for y in range(40) for x in (f - 1, f + w))
              and lit_rows([bytes([pixel(stock, a, x, y) for x in range(f, f + w)]) for y in range(40)],
                           range(w)) == (list(range(11, 29)) if c != "-" else [21, 22])
              for c, (a, f, w) in sources.items()),
          f"/each of the {len(sources)} glyphs is a run of lit columns of its stock picture "
          f"with an empty column on both sides, and stands in rows 11 to 28 of it, 18 rows high (the "
          f"minus in rows 21 and 22)")
    check(lit_rows(snapshots[10][0]) == lit_rows(snapshots[0xE0][0], range(4, 20)) == list(range(11, 29)),
          "... and on the display REC stands in the same rows 11 to 28 as MASTER OUT (22 rows, 9 to 30, "
          ")")
    screen(11, U(text="REC", right=True), L(), "BEAT < on REC: nothing")
    screen(12, U(text="REC", right=True), L(), "AUTO/TAP on REC: nothing")
    screen(13, U(text="START", right=True), L("READY", HINT3), "TAP: the list, on START (BIT DEPTH follows, so an arrow to the right)")
    screen(14, U(text="START", right=True), L("READY", HINT3), "BEAT < on the first entry of the list: stays")
    screen(16, U(text="STOP", right=True), L("00:00:12", HINT3), "recording: the entry reads STOP, the time shows")
    screen(17, U(text="MARKER", left=True, right=True), L("00:00:12 M0", HINT3), "BEAT >: MARKER, with the number of markers")
    screen(19, U(text="MARKER", left=True, right=True), L("00:00:15 M1", HINT3), "after TAP: one marker")
    screen(21, U(text="MARKER", left=True, right=True), L("00:00:17 M2", HINT3), "TAP again without moving: two")
    screen(22, U(text="CUT", left=True, right=True), L("00:00:17 F1", HINT3), "BEAT >: CUT, with the file of the take")
    screen(0xF0, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT4),
           "BEAT >: BIT DEPTH, in the stock letters (the H of CH), 16 BIT; during a take "
           "[AUTO/TAP]:BACK alone, a TAP would do nothing")
    screen(23, U(text="ABOUT", left=True), A("03:25:10", "3.5 GB"),
           "BEAT >: ABOUT, the last entry (no arrow to the right): TIME 03:25:10 at the top left, "
           "SPACE 3.5 GB at the top right, PLUS V and the version at "
           "the bottom right, in the small letters, pixel for pixel as in the layout; the "
           "hint line [AUTO/TAP]:BACK alone")
    screen(0xB1, U(text="ABOUT", left=True), A("03:25:09", "3.5 GB"), "... it follows the recorder, second by second")
    screen(0xB3, U(text="ABOUT", left=True), A("03:25:09", "3.5 GB"), "BEAT > on ABOUT: stays")
    screen(0xB7, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT4), "BEAT <: BIT DEPTH, its line back in place of the page")
    screen(0xF1, U(text="CUT", left=True, right=True), L("00:00:17 F1", HINT3), "BEAT <: CUT again")
    screen(25, U(text="CUT", left=True, right=True), L("00:01:05 F2", HINT3), "after TAP: the second file")
    screen(26, U(text="REC", right=True), L(), "AUTO/TAP: back to REC, nothing below it")
    screen(27, U(text="CUT", left=True, right=True), L("00:01:05 F2", HINT3), "TAP: the list where it was left")
    screen(29, U(text="STOP", right=True), L("00:01:05", HINT3), "BEAT < twice: STOP")
    screen(31, U(text="START", right=True), L("SAVING", HINT4),
           "after TAP: the file is being closed")
    screen(32, U(text="START", right=True), L("READY", HINT3), "ready again")
    screen(34, U(item=item_addresses[1], left=True, right=True), L(),
           "BEAT > from RECORDER: main's first entry, now with an arrow back to RECORDER")
    screen(35, U(item=item_addresses[2], left=True, right=True), L(), "BEAT >: main's second entry")
    screen(36, U(item=item_addresses[3], left=True, right=True), L(), "BEAT >: main's third entry")
    screen(38, U(item=item_addresses[1], left=True, right=True), L(), "BEAT < twice: main's first entry again, arrow back")
    screen(39, U(text="REC", right=True), L(), "BEAT <: REC")
    screen(0xE1, U(item=item_addresses[1], left=True, right=True), L(), "closed and opened again: MASTER OUT")
    screen(42, U(text="REC", right=True), L(), "BEAT <: REC")
    screen(43, U(text="START", right=True), L("READY", HINT3), "TAP: the list where it was left")
    screen(45, U(item=item_addresses[1], left=True, right=True), L(),
           "BEAT > held for 0.6 s on RECORDER: main's first entry, and no further")
    screen(46, U(item=item_addresses[1], left=True, right=True), L(), "... also after it is let go")

    screen(0x52, U(text="REC", right=True), L(), "recording past the hour: REC shows nothing below it")
    screen(0x53, U(text="STOP", right=True), L("02:03:44", HINT3), "STOP: the time reads 02:03:44")
    screen(0x54, U(text="MARKER", left=True, right=True), L("02:03:44 M4", HINT3), "... also with the marker count")

    REC = dict(text="REC", right=True)
    START = dict(text="START", right=True)
    screen(0x60, U(**REC), L(), "no stick: REC as always")
    screen(0x61, U(**START), L("NO USB", HINT4), "TAP: START says NO USB; [AUTO/TAP]:BACK alone, a TAP does nothing")
    screen(0x56, U(**START), L("NO USB", HINT4), "BEAT <: nothing before it")
    screen(0xF2, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3),
           "BEAT >: BIT DEPTH, 16 BIT, and [TAP]:OK: it can be chosen without a stick")
    screen(0x57, U(text="ABOUT", left=True), A("--", "--"), "BEAT >: ABOUT, TIME -- and SPACE --")
    screen(0x58, U(text="ABOUT", left=True), A("--", "--"), "TAP: nothing")
    screen(0x59, U(**REC), L(), "AUTO/TAP: back to REC, the page gone")
    screen(0xE4, U(text="ABOUT", left=True), A("--", "--"), "TAP: the list where it was left, ABOUT")
    screen(0xF3, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "BEAT <: BIT DEPTH")
    screen(0xEA, U(**START), L("NO USB", HINT4), "BEAT <: START, NO USB")
    screen(0x62, U(**START), L("USB ERROR", HINT4), "a stick that is not FAT32: USB ERROR (NOT FAT32 before)")
    screen(0x63, U(**START), L("USB FULL", HINT4), "no room on the stick: USB FULL")
    screen(0x64, U(**START), L("USB ERROR", HINT4), "transfers or the file system fail: USB ERROR")
    screen(0x65, U(**START), L("NOT READY", HINT4), "anything else: NOT READY")

    def flashing(shots, first, last, what):
        on, off = [bytes(r) for r in L("INIT", HINT4)], [bytes(r) for r in L(None, HINT4)]
        up = [bytes(r) for r in U(**START)]
        looks = [shots[c] for c in range(first, last + 1)]
        states = ["on" if low == on else "off" if low == off else "?" for _, low in looks]
        check(all(u == up for u, _ in looks) and "?" not in states and "on" in states and "off" in states,
              f"checkpoints {first:#x}-{last:#x}: {what}: {' '.join(states)}")

    flashing(snapshots, 0x66, 0x69, "the recorder looks at a stick: INIT flashes (0.6 s on, 0.4 s off)")
    screen(0x6A, U(**REC), L(), "AUTO/TAP: REC")
    screen(0xE5, U(item=item_addresses[1], left=True, right=True), L(),
           "BEAT > still leads to main's first entry, which has its [TAP]:ENTER")
    screen(0x6B, U(**REC), L(), "BEAT <: REC")
    screen(0x6C, U(**REC), L(), "the recorder is ready: REC as before")
    screen(0x6D, U(**START), L("READY", HINT3), "TAP: the list, START, READY, [TAP]:OK")
    screen(0x6E, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3),
           "BEAT >: MARKER and CUT are not there while nothing is recorded: BIT DEPTH follows START")
    screen(0xB8, U(**START), L("READY", HINT3), "BEAT <: START again")
    screen(0x6F, U(**START), L("NO USB", HINT4), "the stick is pulled with START open: START stays, NO USB")
    screen(0x70, U(**START), L("READY", HINT3), "stick back: READY")
    screen(0x73, U(text="STOP", right=True), L("00:00:07", HINT3), "recording: STOP, and now there is more to the right")
    screen(0x74, U(text="MARKER", left=True, right=True), L("00:00:07 M0", HINT3), "BEAT >: MARKER")
    screen(0x75, U(text="MARKER", left=True, right=True), L("MAX 100", HINT4), "the file has its hundred markers: MAX 100, [AUTO/TAP]:BACK alone")
    screen(0x77, U(text="CUT", left=True, right=True), L("WAIT", HINT4), "CUT while the recorder cannot cut yet: WAIT")
    screen(0x79, U(text="CUT", left=True, right=True), L("LOW SPACE", HINT4), "... or when too little room is left: LOW SPACE")
    screen(0x7B, U(text="CUT", left=True, right=True), L("00:00:07 F1", HINT3), "a cut is possible again: the time, the file and [TAP]:OK")
    screen(0x7D, U(**START), L("SAVING", HINT4), "the take ends with CUT shown: the list is back on START, SAVING")
    screen(0x7F, U(**START), L("READY", HINT3), "ready again")
    screen(0x80, U(**START), L("NOT READY", HINT4), "the recorder falls silent for 3 s with the list open: START, NOT READY")
    screen(0x81, U(**START), L("READY", HINT3), "it is back: READY")
    d, b = seen_by_main(0x60, 0x69)
    check(d & (KEY_PREV | KEY_NEXT | KEY_ENTER | KEY_TAP7) == 0 and b == 0,
          "while a message shows main still sees none of the navigation buttons")

    screen(0x90, U(**REC), L(), "the file has lost audio: REC shows nothing below it")
    screen(0x91, U(text="STOP", right=True), L("!00:12:34", HINT3), "... STOP shows !00:12:34")
    screen(0x92, U(text="MARKER", left=True, right=True), L("!00:12:34 M7", HINT3), "... with the marker count")
    screen(0x93, U(text="MARKER", left=True, right=True), L("!01:12:34 M12", HINT3), "... and past the hour with a count of two digits")
    screen(0x94, U(text="MARKER", left=True, right=True), L("01:12:34 M12", HINT3), "the next file has lost nothing: no !")

    screen(0xA0, U(**REC), L(), "under 60 minutes until the stick is full: REC shows nothing below it")
    screen(0xA1, U(text="MARKER", left=True, right=True), L("-29:58 M12", HINT3), "... in the list, with the marker count")
    screen(0xA2, U(text="STOP", right=True), L("-29:58", HINT3), "... and on stop")
    screen(0xA3, U(text="MARKER", left=True, right=True), L("!-00:07 M12", HINT3),
           "seven seconds left, and the file has lost audio: !-00:07 M12 fills the eleven places")
    screen(0xA4, U(text="CUT", left=True, right=True), L("!-00:07 F1", HINT3), "... on cut with the file's number")
    screen(0xA5, U(text="CUT", left=True, right=True), L("01:31:02 F1", HINT3), "a next file was found after all: the file's time again")
    screen(0xA6, U(text="CUT", left=True, right=True), L("01:31:02 F1", HINT3),
           "a status whose countdown byte is out of range (61; 31 before) is not taken: the display stays")
    screen(0xA9, U(text="CUT", left=True, right=True), L("-59:59 F1", HINT3), "the countdown from -59:59")

    AB = dict(text="ABOUT", left=True)
    screen(0xF4, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "ready: START, BEAT >: BIT DEPTH")
    screen(0xC3, U(**AB), A("03:25:10", "3.5 GB"), "BEAT >: ABOUT, TIME 03:25:10, SPACE 3.5 GB")
    screen(0xCB, U(**AB), A("124:00:00", "128.5 GB"), "a big stick: 124:00:00 and 128.5 GB (1285 tenths from the recorder)")
    screen(0xCD, U(**AB), A("--", "--"), "the recorder does not know them: -- and --")
    screen(0xCF, U(**AB), A("--", "--"), "a status with 60 minutes left is not taken: the page stays")
    screen(0xD0, U(**AB), A("00:57:52", "999 MB"), "under an hour 00:57:52; under 1 GB in MB: 999")
    screen(0xD1, U(**AB), A("00:57:53", "1.0 GB"), "from 1 GB on in GB with one decimal: 1.0 GB (10 tenths)")
    screen(0xD2, U(**AB), A("00:00:41", "11 MB"), "41 seconds: 00:00:41, 11 MB")
    screen(0xD3, U(**AB), A("--", "--"), "the stick is pulled on ABOUT: the page stays, -- and --")
    screen(0xF5, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "BEAT <: BIT DEPTH, its line back in place of the page")
    screen(0xD4, U(**START), L("NO USB", HINT4), "BEAT <: START, NO USB")
    screen(0xF6, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "BEAT >: BIT DEPTH")
    screen(0xE8, U(**AB), A("--", "--"), "BEAT >: ABOUT")
    screen(0xD5, U(**AB), A("01:02:03", "1.0 GB"), "the stick back: 01:02:03, 1.0 GB")
    screen(0xF7, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "BEAT <: BIT DEPTH")
    screen(0xD6, U(text="START", right=True), L("READY", HINT3), "BEAT <: START")
    screen(0xF8, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "BEAT >: BIT DEPTH")
    screen(0xD7, U(**AB), A("01:02:03", "1.0 GB"), "BEAT >: ABOUT")
    screen(0xD8, U(**REC), L(), "AUTO/TAP: REC, nothing in place of the page")
    screen(0xD9, U(**AB), A("01:02:03", "1.0 GB"), "TAP: the list opens where it was left, on ABOUT")
    screen(0xDC, U(item=item_addresses[1], left=True, right=True), L(), "the menu closed on ABOUT and opened again: MASTER OUT")
    screen(0xE9, U(**REC), L(), "BEAT <: REC")
    screen(0xDD, U(**AB), A("01:02:03", "1.0 GB"), "TAP: ABOUT is drawn again in full")

    screen(0x42, U(**AB), A("01:02:03", "1.0 GB"), "the list opens where it was left, ABOUT")
    screen(0x43, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3),
           "BEAT <: BIT DEPTH just before ABOUT, 16 BIT at power-on, [TAP]:OK")

    def turns(shots, first, last, hint, what):
        up = [bytes(r) for r in U(text="BIT DEPTH", left=True, right=True)]
        a, b = [bytes(r) for r in L("24 BIT", hint)], [bytes(r) for r in L("EXPERIMENTAL", hint)]
        looks = [shots[c] for c in range(first, last + 1)]
        states = ["24" if low == a else "EXP" if low == b else "?" for _, low in looks]
        check(all(u == up for u, _ in looks) and "?" not in states and "24" in states and "EXP" in states,
              f"checkpoints {first:#x}-{last:#x}: {what}: {' '.join(states)}")

    turns(snapshots, 0x45, 0x48, HINT3, "TAP: the recorder takes 24 bit; 24 BIT and EXPERIMENTAL in turn "
          "every two seconds")
    screen(0x4A, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "TAP again: 16 BIT")
    turns(snapshots, 0x31, 0x34, HINT4, "24 bit again and a take runs: shown in turn, [AUTO/TAP]:BACK alone")
    low = snapshots[0x4C][1]
    check(snapshots[0x4C][0] == [bytes(r) for r in U(text="BIT DEPTH", left=True, right=True)]
          and low in ([bytes(r) for r in L("24 BIT", HINT4)], [bytes(r) for r in L("EXPERIMENTAL", HINT4)]),
          "checkpoint 0x4c: the take is being closed: the take's depth, [AUTO/TAP]:BACK alone")
    screen(0x4E, U(text="BIT DEPTH", left=True, right=True), L("16 BIT", HINT3), "ready again at 16 bit: 16 BIT, [TAP]:OK")

    hidden = KEY_PREV | KEY_NEXT | KEY_ENTER | KEY_TAP7
    d, b = seen_by_main(10, 33)
    check(d & hidden == 0 and b == 0,
          "on RECORDER and in its list main sees none of BEAT <, BEAT >, TAP, AUTO/TAP")
    d, _ = seen_by_main(33, 34)
    check(d & hidden == 0, "the BEAT > that leaves RECORDER is not passed on to main")
    d35, _ = seen_by_main(34, 35)
    d37, _ = seen_by_main(36, 37)
    check(d35 & KEY_NEXT and d37 & KEY_PREV, "in main's own entries BEAT < and > reach main")
    d40, _ = seen_by_main(39, 40)
    d42, _ = seen_by_main(41, 42)
    d41, _ = seen_by_main(40, 41)
    check(d40 & KEY_QUANTIZE and d42 & KEY_QUANTIZE and d41 & KEY_NEXT,
          "QUANTIZE always reaches main (the menu closes from RECORDER); with the menu closed BEAT > "
          "is main's again")
    d, _ = seen_by_main(44, 46)
    check(d & hidden == 0, "a button held while RECORDER is left stays hidden until it is let go")

    def command_at(checkpoint):
        p = [x for x in packets if x[0] < tags[checkpoint]][-1][1]
        return p[0x0E], p[0x0F]
    want = {14: (0, 0), 15: (0x11, 0xEE), 17: (0x11, 0xEE), 18: (0x23, 0xDC), 20: (0x33, 0xCC),
            23: (0x33, 0xCC), 0xB4: (0x33, 0xCC), 0xB7: (0x33, 0xCC), 24: (0x44, 0xBB), 29: (0x44, 0xBB),
            30: (0x52, 0xAD), 43: (0x52, 0xAD)}
    got = {c: command_at(c) for c in want}
    check(got == want,
          "commands in packet bytes 0x0E/0x0F: start, marker, marker, cut, stop, each with a new "
          "sequence number and its complement; the bytes stay until the next command (TAP on ABOUT "
          "sends nothing)"
          + ("" if got == want else f" GOT {got}"))
    want = {0x61: (0x52, 0xAD), 0x58: (0x52, 0xAD), 0x59: (0x52, 0xAD), 0x71: (0x52, 0xAD), 0x72: (0x61, 0x9E), 0x76: (0x61, 0x9E),
            0x78: (0x61, 0x9E), 0x7A: (0x61, 0x9E), 0x7C: (0x74, 0x8B), 0x7E: (0x74, 0x8B),
            0x82: (0x74, 0x8B)}
    got = {c: command_at(c) for c in want}
    check(got == want,
          "a TAP that cannot do anything sends no command: on START under a message, "
          "on ABOUT, on "
          "MARKER at 100 markers, on CUT under WAIT and LOW SPACE, on START while SAVING; start and "
          "cut go out when they can" + ("" if got == want else f" GOT {got}"))

    boot = simulate("menu_boot", patched_file, syms["_mod_entry"], syms["_mod_init_entry"],
                    syms["_tx_tramp"], defines=["-DSCENARIO_BOOT"])
    shots, boot_packets, boot_tags, _ = play(boot)

    def boot_screen(checkpoint, up, low, what):
        got_up, got_low = shots[checkpoint]
        ok = [bytes(r) for r in up] == got_up and [bytes(r) for r in low] == got_low
        check(ok, f"after power-on, checkpoint {checkpoint}: {what}")
        if not ok:
            for name, rows in (("got_upper", got_up), ("want_upper", up), ("got_lower", got_low), ("want_lower", low)):
                screen_dump.save(show(rows), os.path.join(BUILD, f"menu_boot_{checkpoint}_{name}.png"))

    boot_screen(0x20, U(item=item_addresses[1], left=True, right=True), L(),
                "UTILITY opened 3 s after the normal screen: MASTER OUT with the arrow to REC")
    boot_screen(0x21, U(**REC), L(), "BEAT <: REC")
    flashing(shots, 1, 4, "TAP: START, and INIT flashes though the recorder has sent nothing yet")
    got_up, got_low = shots[5]
    check(got_up == [bytes(r) for r in U(text="BIT DEPTH", left=True, right=True)]
          and got_low in ([bytes(r) for r in L("INIT", HINT4)], [bytes(r) for r in L(None, HINT4)]),
          "after power-on, checkpoint 5: BEAT >: BIT DEPTH; nothing known yet, so it says why "
          "as START does (INIT), and a TAP would do nothing")
    boot_screen(0x23, U(text="ABOUT", left=True), A("--", "--"), "BEAT >: ABOUT, nothing known yet")
    flashing(shots, 6, 9, "BEAT < back to START; 36 s on, still nothing: INIT")
    for c in (10, 11, 12, 13):
        boot_screen(c, U(**START), L("NOT READY", HINT4), "38 s on and nothing has come: NOT READY, steady")
    flashing(shots, 14, 17, "the recorder appears and says it is starting: INIT")
    boot_screen(18, U(**START), L("READY", HINT3), "ready: READY and [TAP]:OK")
    boot_screen(19, U(**REC), L(), "AUTO/TAP: REC")
    check(all(p[1][0x0E] == 0 and p[1][0x0F] == 0 for p in boot_packets) and len(boot_packets) > 300,
          f"after power-on: no command in any of the {len(boot_packets)} packets looked at (the TAP "
          f"on START under INIT sent none)")

    held = simulate("menu_update", patched_file, syms["_mod_entry"], syms["_mod_init_entry"],
                    syms["_tx_tramp"], defines=["-DSCENARIO_UPDATE"])
    shots, held_packets, held_tags, _ = play(held)
    first = [p for p in held_packets if p[0] < 40]
    check(len(first) == 40 and all(p[1][0x0B] & 0x02 and p[1][0x0C] & 0x40 for p in first),
          "update buttons held at power-on: both are in every one of the first 40 packets for main "
          "(byte 0x0B bit 1, byte 0x0C bit 6), which is what main's updater looks at")
    stock_first = ([bytes(r) for r in U(item=item_addresses[1], right=True)], [bytes(r) for r in L()])
    stock_second = ([bytes(r) for r in U(item=item_addresses[2], left=True, right=True)], [bytes(r) for r in L()])
    check(shots[1] == stock_first and shots[2] == stock_first and shots[3] == stock_second,
          "... and from then on, also long after the lockout: UTILITY is main's alone, exactly "
          "as the stock firmware shows it (no RECORDER, no arrow to it)")
    later = [p for p in held_packets if p[0] >= 400]
    d = 0
    for p in later:
        d |= p[1][0x0D]
    check(d & (KEY_PREV | KEY_NEXT | KEY_ENTER | KEY_QUANTIZE) == KEY_PREV | KEY_NEXT | KEY_ENTER | KEY_QUANTIZE
          and all(p[1][0x0E] == 0 and p[1][0x0F] == 0 for p in held_packets),
          f"... main sees every button and no packet carries a command ({len(later)} packets looked at)")
    stock_file = os.path.join(BUILD, "panel_stock_image.bin")
    open(stock_file, "wb").write(stock)
    plain = simulate("menu_update_stock", stock_file, 0xC68F0, 0xC6836, 0xCAE90,
                     defines=["-DSCENARIO_UPDATE"])
    check(held == plain and sum(l.startswith("Q ") for l in held) > 20
          and sum(l.startswith("X ") for l in held) > 150,
          f"... and the whole session, {sum(l.startswith('X ') for l in held)} packets for main and "
          f"{sum(l.startswith('Q ') for l in held)} drawings, is identical to the same session on the "
          f"stock panel firmware: this code does nothing at all once the update buttons were seen")

    want = {0x43: (0x74, 0x8B), 0x44: (0x85, 0x7A), 0x48: (0x85, 0x7A), 0x49: (0x95, 0x6A),
            0x30: (0xA5, 0x5A), 0x4B: (0xA5, 0x5A), 0x4D: (0xA5, 0x5A), 0x4E: (0xA5, 0x5A),
            0xF0: (0x33, 0xCC), 0xF2: (0x52, 0xAD), 0x57: (0x52, 0xAD)}
    got = {c: command_at(c) for c in want}
    check(got == want,
          "TAP on BIT DEPTH sends command 5 with a new sequence number, while ready (and without "
          "a stick); not while a take runs or is being closed; moving over it sends nothing"
          + ("" if got == want else f" GOT {got}"))
    last = command_at(0xDA)
    check(last == command_at(0xC1),
          "no command from ABOUT (TAP on it at 0xDA)" + ("" if last == command_at(0xC1) else f" GOT {last}"))

    for name, number in (("recorder", 10), ("start", 13), ("stop", 16), ("marker", 21), ("cut", 25), ("main_first", 34),
                         ("about", 0xC3), ("about_big", 0xCB), ("about_mb", 0xD0), ("about_not_ready", 0x57),
                         ("no_usb", 0x61), ("usb_error", 0x64), ("max_100", 0x75), ("main_open", 0xE0),
                         ("wait", 0x77), ("low_space", 0x79), ("saving", 0x7D), ("lost_audio", 0x92),
                         ("long_line", 0x93), ("countdown", 0xA1), ("countdown_end", 0xA3),
                         ("bit_depth", 0x43), ("bit_depth_24", 0x45), ("bit_depth_24b", 0x46)):
        for k, part in enumerate(("upper", "lower")):
            screen_dump.save(show(snapshots[number][k]), os.path.join(BUILD, f"menu_{name}_{part}.png"))
    print(f"pictures: {os.path.join(BUILD, 'menu_*.png')}")
    print(f"panel menu: {passed} checks passed, {failed} failed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
