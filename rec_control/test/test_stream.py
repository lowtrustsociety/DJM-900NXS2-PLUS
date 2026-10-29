#!/usr/bin/env python3
"""Tests of the recorder in the SH simulator.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""

import os
import re
import struct
import subprocess
import sys
import tempfile
from concurrent.futures import ThreadPoolExecutor

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.dirname(HERE)
ROOT = os.path.dirname(PROBE)
sys.path.insert(0, PROBE)
sys.path.insert(0, HERE)
import analyze_wav
import build_stream as build
import test_fat

PAGE = 23_040
RATE = 288_000
BLOCK = 72
PAGE_BLOCKS = 320
NOISE_BLOCK = 74
RING_FULL, RING_NO_EXTRA, RING_NO_POOL = 45, 15, 36
EXTRA_PIECES = (20, 2, 8)
SETTLE_US = 5_000_000
PHASES = {name: i for i, name in enumerate(analyze_wav.PHASES)}
KEEP, DISCARD, ABORT, FAIL, SWITCH, CUT = 3, 4, 5, 6, 8, 0
SPLIT = 10
REBOOT, STICK_OUT, STICK_IN, SWAP, CORRUPT_MAP, DIRTY_EXTRA, DMA_COUNT, MIDI, COMMAND = 1, 2, 3, 4, 5, 6, 7, 8, 9
FEED_REFUSE, NOTE = 10, 11
GUARD = 12
TOUCH = 13
C_START, C_STOP, C_MARK, C_CUT, C_BITS = 1, 2, 3, 4, 5
START_KEY, STOP_KEY, TOGGLE_KEY, MARK_KEY, CUT_KEY = 0xB020, 0xB021, 0xB022, 0xB023, 0xB024
PRESS = lambda key: (key << 8) | 0x7F
RELEASE = lambda key: key << 8
ST = {name: bit for bit, name in analyze_wav.STATUS.items()}
SPC, DATA_START, CLUSTER = 32, 1058, 16_384


def blocks(seconds):
    return int(round(seconds * 4000))


def array(rows):
    return "{" + ",".join("{" + ",".join("%du" % v for v in row) + "}" for row in rows) + "}"


BURST = int(os.environ.get("BURST", "1"))
CUE_RESERVE = 4096


def cap(seconds):
    return 512 + seconds * RATE // PAGE * PAGE


def case(schedule, events=(), stalls=(), cap_seconds=300, minimum=1_000_000, refill=0,
         margin=1 << 20, layout=0, ahead=0, extra=(), noise=1, count=0, deep=1):
    flags = ["-DSCHEDULE=" + array([(blocks(s), a) for s, a in schedule]),
             f"-DUSER_LAYOUT={layout}u", f"-DDEPTH_AT_POWER_ON={deep}u"]
    if cap_seconds:
        flags.append(f"-DFILE_CAP_BYTES={cap(cap_seconds)}u")
    flags += [f"-DAUDIO_NOISE={noise}u", f"-DBURST_PAGES={BURST}u", f"-DFILE_MIN_BYTES={minimum}u", f"-DFILE_REFILL_BYTES={refill}u",
              f"-DFILE_MARGIN_BYTES={margin}u", f"-DNEXT_AHEAD_WORDS={ahead * 96_000}u",
              f"-DCOUNTDOWN_WORDS={count * 96_000}u"]
    if events:
        flags.append("-DEVENTS=" + array([(blocks(s), a, v) for s, a, v in events]))
    if stalls:
        flags.append("-DSTALLS=" + array([(n, blocks(s)) for n, s in stalls]))
    return flags + list(extra)


PANEL_REST = bytes.fromhex("08 04 02 02 04 00 14 60 00 00 00 00")
NORMAL_BUILD = ["-DAUTO_START=0u"]
LONG_START = (1 << 32) // 24 - 80_000
ONE_TAKE = [(10, 0), (70, 1), (40, 0)]
TWO_TAKES = [(10, 0), (70, 1), (40, 0), (70, 1), (40, 0)]

cases = {
    "two_takes": case(TWO_TAKES),
    "sound_check": case([(10, 0), (30, 1), (30, 0), (70, 1), (40, 0)]),
    "restart": case(TWO_TAKES, events=[(105, REBOOT, 0)]),
    "power_cut_early": case([(10, 0), (190, 1), (40, 0)], events=[(60, REBOOT, 0)]),
    "power_cut_named": case([(10, 0), (230, 1), (40, 0)], events=[(110, REBOOT, 0)]),
    "file_full": case([(10, 0), (150, 1), (40, 0)], cap_seconds=64),
    "file_full_exact": case([(10, 0), (150, 1), (40, 0)], cap_seconds=62),
    "refill": case(TWO_TAKES, refill=cap(250)),
    "user_file": case(ONE_TAKE, layout=1),
    "fragmented": case(ONE_TAKE, layout=2, cap_seconds=0),
    "fragmented_full": case([(10, 0), (900, 1), (40, 0)], layout=2, cap_seconds=0),
    "no_space": case(ONE_TAKE, layout=3, minimum=64 << 20),
    "foreign_tmp": case(ONE_TAKE, layout=4),
    "fat16": case(ONE_TAKE, layout=5),
    "geometry_lie": case(ONE_TAKE, extra=["-DGEOMETRY_LIE=32u"]),
    "no_log": case(ONE_TAKE, extra=["-DLOG_FILE=0u"]),
    "small_log": case(ONE_TAKE, extra=["-DLOG_FILE=2u"]),
    "medium_log": case(ONE_TAKE, extra=["-DLOG_FILE=3u"]),
    "no_stick": case(ONE_TAKE, extra=["-DSTICK_INITIAL=0u"]),
    "stick_late": case([(40, 0), (70, 1), (40, 0)], events=[(20, STICK_IN, 0)],
                       extra=["-DSTICK_INITIAL=0u"]),
    "stick_swap": case([(30, 0), (100, 1), (40, 0)], events=[(20, SWAP, 0)]),
    "stick_pulled": case([(10, 0), (190, 1), (40, 0)],
                         events=[(40, STICK_OUT, 0), (60, STICK_IN, 0)]),
    "dirty": case([(10, 0), (30, 1)], extra=["-DTEST_DIRTY"]),
    "feed_refused": case(ONE_TAKE, events=[(0, FEED_REFUSE, 1)]),
    "feed_late": case(ONE_TAKE, events=[(0, FEED_REFUSE, 1), (30, FEED_REFUSE, 0)]),
    "feed_notes": case(ONE_TAKE, events=[(20, NOTE, 1), (25, NOTE, 6), (50, NOTE, 8)]),
    "guard_changed": case(ONE_TAKE, events=[(40, GUARD, 0)]),
    "map_corrupt": case([(10, 0), (30, 1), (10, 0)], events=[(8, CORRUPT_MAP, 0)]),
    "extra_changed": case(ONE_TAKE, events=[(1, DIRTY_EXTRA, PHASES["LOG"])]),
    "extra_changed_armed": case(ONE_TAKE, events=[(8, DIRTY_EXTRA, 2 << 8)]),
    "extra_changed_twice": case(ONE_TAKE, events=[(1, DIRTY_EXTRA, PHASES["LOG"] | 1 << 8),
                                                  (8, DIRTY_EXTRA, 2 << 8)]),
    "extra_changed_all": case(ONE_TAKE, events=[(1, DIRTY_EXTRA, PHASES["LOG"] | n << 8) for n in range(3)]),
    "pool_fail": case(ONE_TAKE, extra=["-DPOOL_FAIL_AT=3u"]),
    "map_alloc_fail": case([(20, 0), (70, 1), (40, 0)], extra=["-DMAP_ALLOC_FAILS=2u"]),
    "release_fail": case(ONE_TAKE, extra=["-DRELEASE_FAIL_AT=2u"]),
    "write_fail": case([(10, 0), (190, 1), (30, 0)], extra=["-DWRITE_FAIL_AT=40u"]),
    "invalid_armed": case([(8, 0), (72, 1), (40, 0)],
                          events=[(7.5, DMA_COUNT, 22), (12, DMA_COUNT, 24)]),
    "invalid_active": case([(10, 0), (90, 1), (40, 0)],
                           events=[(30, DMA_COUNT, 22), (31, DMA_COUNT, 24)]),
    "spike": case(ONE_TAKE, stalls=[(50, 0.8)]),
    "stall_3s": case(ONE_TAKE, stalls=[(50, 3.0)]),
    "stall_5s5": case(ONE_TAKE, stalls=[(50, 5.5), (600, 5.5)]),
    "stall_no_extra": case(ONE_TAKE, events=[(1, DIRTY_EXTRA, PHASES["LOG"] | n << 8) for n in range(3)],
                           stalls=[(50, 3.0)]),
    "full_45": case([(10, 0), (2700, 1), (40, 0)], cap_seconds=0),
}
cases.update({
    "pack_ramp": case(TWO_TAKES, noise=0),
    "pack_music": case(ONE_TAKE, noise=2),
    "pack_music_stall": case(ONE_TAKE, stalls=[(50, 5.5), (600, 5.5)], noise=2),
    "pack_music_chain": case([(10, 0), (200, 1), (40, 0)], cap_seconds=90, ahead=30, noise=2),
    "pack_music_cut": case([(10, 0), (230, 1), (40, 0)], events=[(110, REBOOT, 0)], noise=2),
})
KEYS = [f"-DCONTROL_START_KEY={START_KEY}u", f"-DCONTROL_STOP_KEY={STOP_KEY}u",
        "-DCONTROL_EVERY_MESSAGE=0u"]
HAND = KEYS + ["-DAUTO_START=0u"]
cases.update({
    "hand_start": case([(10, 0), (60, 1), (60, 0), (40, 1), (40, 0)], extra=HAND,
                       events=[(5, MIDI, PRESS(STOP_KEY)), (20, MIDI, PRESS(START_KEY)),
                               (20.5, MIDI, RELEASE(START_KEY)), (60, MIDI, PRESS(START_KEY)),
                               (150, MIDI, PRESS(STOP_KEY)), (150.5, MIDI, RELEASE(STOP_KEY))]),
    "hand_early": case([(10, 0), (150, 1), (40, 0)], extra=HAND,
                       events=[(2, MIDI, PRESS(START_KEY)), (100, MIDI, PRESS(STOP_KEY))]),
    "hand_short": case([(10, 0), (150, 1), (40, 0)], extra=HAND,
                       events=[(20, MIDI, PRESS(START_KEY)), (25, MIDI, PRESS(STOP_KEY)),
                               (40, MIDI, PRESS(START_KEY)), (100, MIDI, PRESS(STOP_KEY))]),
    "hand_stop_auto": case([(10, 0), (100, 1), (10, 0), (60, 1), (40, 0)], extra=KEYS,
                           events=[(40, MIDI, PRESS(STOP_KEY))]),
    "hand_toggle": case([(10, 0), (150, 1), (40, 0)],
                        extra=[f"-DCONTROL_START_KEY={TOGGLE_KEY}u", f"-DCONTROL_STOP_KEY={TOGGLE_KEY}u",
                               "-DAUTO_START=0u", "-DCONTROL_EVERY_MESSAGE=0u"],
                        events=[(20, MIDI, PRESS(TOGGLE_KEY)), (20.3, MIDI, RELEASE(TOGGLE_KEY)),
                                (90, MIDI, PRESS(TOGGLE_KEY)), (90.3, MIDI, RELEASE(TOGGLE_KEY))]),
})
MARKS = KEYS + [f"-DCONTROL_MARK_KEY={MARK_KEY}u"]
cases.update({
    "mark_take": case(ONE_TAKE, extra=MARKS,
                      events=[(5, MIDI, PRESS(MARK_KEY)), (20, MIDI, PRESS(MARK_KEY)),
                              (20.2, MIDI, RELEASE(MARK_KEY)), (35.5, MIDI, PRESS(MARK_KEY)),
                              (60, MIDI, PRESS(MARK_KEY))]),
    "mark_chain": case([(10, 0), (200, 1), (40, 0)], cap_seconds=90, ahead=30, extra=MARKS,
                       events=[(40, MIDI, PRESS(MARK_KEY)), (110, MIDI, PRESS(MARK_KEY)),
                               (195, MIDI, PRESS(MARK_KEY))]),
    "mark_restart": case([(10, 0), (70, 1), (60, 0)], extra=MARKS,
                         events=[(20, MIDI, PRESS(MARK_KEY)), (35.5, MIDI, PRESS(MARK_KEY)),
                                 (115, REBOOT, 0)]),
    "mark_hand": case([(10, 0), (150, 1), (40, 0)], extra=MARKS + ["-DAUTO_START=0u"],
                      events=[(20, MIDI, PRESS(START_KEY)), (30, MIDI, PRESS(MARK_KEY)),
                              (61.23, MIDI, PRESS(MARK_KEY)), (75.37, MIDI, PRESS(STOP_KEY))]),
})
CUTS = MARKS + [f"-DCONTROL_CUT_KEY={CUT_KEY}u"]
import build_stream as _variants
cases["hand_mixer"] = case([(30, 0), (130, 1), (10, 0), (70, 1), (40, 0)],
                           extra=_variants.VARIANTS["handtest"],
                           events=[(20, MIDI, 0xB0497F), (40, MIDI, 0xB0487F), (80, MIDI, 0xB0477F),
                                   (100, MIDI, 0xB04800), (140, MIDI, 0xB04900)])
cases["menu_mixer"] = case([(30, 0), (130, 1), (10, 0), (70, 1), (40, 0)],
                           events=[(20, COMMAND, C_START), (40, COMMAND, C_MARK), (80, COMMAND, C_CUT),
                                   (100, COMMAND, C_MARK), (101, COMMAND, C_MARK), (140, COMMAND, C_STOP)])
cases["menu_idle"] = case([(10, 0), (150, 1), (40, 0)], extra=_variants.VARIANTS["normal"],
                          events=[(12, COMMAND, C_STOP), (14, COMMAND, C_MARK), (16, COMMAND, C_CUT),
                                  (30, COMMAND, C_START), (50, COMMAND, C_START), (100, COMMAND, C_STOP)])
cases["ready_stick_out"] = case([(80, 0)], extra=_variants.VARIANTS["normal"],
                                events=[(20, STICK_OUT, 0), (40, STICK_IN, 0)])
cases.update({
    "cut_take": case([(10, 0), (200, 1), (40, 0)], extra=CUTS,
                     events=[(5, MIDI, PRESS(CUT_KEY)), (70, MIDI, PRESS(CUT_KEY)),
                             (70.2, MIDI, RELEASE(CUT_KEY)), (130, MIDI, PRESS(CUT_KEY))]),
    "cut_marks": case([(10, 0), (200, 1), (40, 0)], extra=CUTS, stalls=[(1010, 1.5)],
                      events=[(40, MIDI, PRESS(MARK_KEY)), (70, MIDI, PRESS(CUT_KEY)),
                              (100, MIDI, PRESS(MARK_KEY)), (130, MIDI, PRESS(CUT_KEY))]),
    "cut_refused": case(ONE_TAKE, extra=CUTS, minimum=260 * RATE,
                        events=[(60, MIDI, PRESS(CUT_KEY))]),
})
MAKE = 2
for n in range(6 + MAKE, 16 + MAKE):
    cases[f"hand_cut_{n}"] = case([(10, 0), (200, 1), (40, 0)], extra=CUTS + [f"-DMETA_CUT_AT={n}u"],
                                  events=[(70, MIDI, PRESS(CUT_KEY)), (130, MIDI, PRESS(CUT_KEY))])
CHAIN = dict(cap_seconds=90, ahead=30)
cases.update({
    "chain": case([(10, 0), (200, 1), (40, 0)], **CHAIN),
    "chain_fragmented": case([(10, 0), (200, 1), (40, 0)], layout=2, **CHAIN),
    "chain_stall": case([(10, 0), (200, 1), (40, 0)], stalls=[(1785, 1.7), (1800, 1.7)], **CHAIN),
    "chain_ends_early": case([(10, 0), (55, 1), (40, 0)], **CHAIN),
    "chain_stick_full": case([(10, 0), (300, 1), (40, 0)], layout=3, **CHAIN),
    "count_stick_full": case([(10, 0), (300, 1), (40, 0)], layout=3, cap_seconds=90, ahead=31, count=30),
    "count_last_file": case([(10, 0), (300, 1), (40, 0)], layout=3, ahead=31, count=30),
    "count_room": case([(10, 0), (200, 1), (40, 0)], cap_seconds=90, ahead=31, count=30),
    "chain_power_cut": case([(10, 0), (250, 1), (40, 0)], events=[(120, REBOOT, 0)], **CHAIN),
    "chain_power_cut_ready": case([(10, 0), (250, 1), (40, 0)], events=[(85, REBOOT, 0)], **CHAIN),
})
TEST_PAGES = 1125
TEST_RESERVE = 512 + TEST_PAGES * PAGE + 65536
TEST_BUILD = dict(cap_seconds=0, minimum=1_048_576, refill=TEST_RESERVE, margin=32 << 20, ahead=30,
                  extra=[f"-DFILE_AUDIO_PAGES={TEST_PAGES}u"])
cases["test_build"] = case([(10, 0), (230, 1), (40, 0), (100, 1), (40, 0)], **TEST_BUILD)
cases["test_build_edge"] = case([(10, 0), (250, 1), (40, 0), (70, 1), (40, 0)], **TEST_BUILD)
for n in (5, 6, 7, 9, 10, 12, 14, 15, 16, 17, 18, 20, 23, 24, 25, 26, 27, 28):
    cases[f"chain_cut_{n}"] = case([(10, 0), (200, 1), (40, 0), (70, 1), (40, 0)],
                                   extra=[f"-DMETA_CUT_AT={n}u"], **CHAIN)

cases.update({
    "depth16": case(ONE_TAKE, deep=0),
    "depth16_chain": case([(10, 0), (280, 1), (40, 0)], deep=0, **CHAIN),
    "depth16_stall": case([(10, 0), (100, 1), (40, 0)], deep=0, stalls=[(300, 3.8)]),
    "depth_switch": case([(10, 0), (70, 1), (40, 0), (150, 1), (40, 0)], deep=0, cap_seconds=64, ahead=30,
                         refill=cap(64),
                         events=[(2, COMMAND, C_BITS), (110, COMMAND, C_BITS), (150, COMMAND, C_BITS)]),
    "depth_power": case([(10, 0), (190, 1), (40, 0)], deep=0, extra=NORMAL_BUILD,
                        events=[(3, COMMAND, C_BITS), (20, COMMAND, C_START), (100, REBOOT, 0),
                                (150, COMMAND, C_START), (190, COMMAND, C_STOP)]),
})

for n in (0, 1, 2, 3, 4, 5, 6, 7, 8, 10, 12, 14, 16):
    cases[f"meta_cut_{n}"] = case([(10, 0), (70, 1), (40, 0), (70, 1), (40, 0)],
                                  extra=[f"-DMETA_CUT_AT={n}u"])


NORMAL = _variants.VARIANTS["normal"]
cases.update({
    "coarse_cut": case([(10, 0), (200, 1), (40, 0)], extra=CUTS, stalls=[(1010, 3.0)],
                       events=[(52, MIDI, PRESS(CUT_KEY))]),
    "coarse_power": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL_BUILD, stalls=[(250, 3.0)],
                         events=[(20, COMMAND, C_START), (90, REBOOT, 0)]),
    "long_take": case([(10, 0), (200, 1), (40, 0)], extra=NORMAL_BUILD + [f"-DSIM_START_BLOCKS={LONG_START}u"],
                      events=[(20, COMMAND, C_START), (30, COMMAND, C_MARK), (50, COMMAND, C_MARK),
                              (60, COMMAND, C_CUT), (75, COMMAND, C_MARK), (90, COMMAND, C_STOP)]),
    "refill_no_room": case(TWO_TAKES, layout=3, refill=cap(300)),
})
cases.update({
    "cut_next_ready": case([(10, 0), (200, 1), (40, 0)], extra=CUTS,
                           events=[(85, MIDI, PRESS(CUT_KEY))], **CHAIN),
    "start_power_5": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                          events=[(20, COMMAND, C_START), (25, REBOOT, 0), (60, COMMAND, C_START),
                                  (100, COMMAND, C_STOP)]),
    "start_power_40": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                           events=[(20, COMMAND, C_START), (60, REBOOT, 0)]),
    "cut_power_5": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                        events=[(20, COMMAND, C_START), (60, COMMAND, C_CUT), (65, REBOOT, 0),
                                (100, COMMAND, C_START), (130, COMMAND, C_STOP)]),
    "mark_power": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                       events=[(20, COMMAND, C_START), (30, COMMAND, C_MARK), (45.5, COMMAND, C_MARK),
                               (70, COMMAND, C_MARK), (85, COMMAND, C_MARK), (89.6, COMMAND, C_MARK),
                               (90, REBOOT, 0)]),
    "lost_many": case([(10, 0), (280, 1), (40, 0)], extra=MARKS,
                      stalls=[(100 + 220 * k, 4.5) for k in range(18)],
                      events=[(40, MIDI, PRESS(MARK_KEY))]),
    "start_pull_5": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                         events=[(20, COMMAND, C_START), (25, STICK_OUT, 0), (40, STICK_IN, 0),
                                 (60, COMMAND, C_START), (80, COMMAND, C_STOP)]),
    "start_pull_40": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                          events=[(20, COMMAND, C_START), (60, STICK_OUT, 0), (80, STICK_IN, 0),
                                  (100, COMMAND, C_START), (120, COMMAND, C_STOP)]),
    "reserve": case(ONE_TAKE, layout=3, cap_seconds=0, minimum=10 << 20, margin=5 << 20),
    "reserve_too_small": case(ONE_TAKE, layout=3, minimum=10 << 20, margin=28 << 20),
})
TEXT_LAYOUTS = {"new": 0, "foreign": 1, "ours": 2, "full_cluster": 3, "many_foreign": 4, "readonly": 5,
                "binary": 6, "empty": 7}
for _tag, _layout in TEXT_LAYOUTS.items():
    cases[f"log_{_tag}"] = case([(10, 0), (230, 1), (40, 0)], events=[(110, REBOOT, 0)],
                                extra=[f"-DTEXT_LAYOUT={_layout}u"])
cases["next_fail_6"] = cases["chain"] + ["-DMETA_FAIL_AT=6u"]
cases["next_fail_7"] = cases["chain"] + ["-DMETA_FAIL_AT=7u"]
cases["short_build"] = case([(10, 0), (400, 1), (40, 0)], cap_seconds=0, minimum=1_048_576,
                            refill=TEST_RESERVE, margin=5 << 20, ahead=30,
                            extra=[f"-DFILE_AUDIO_PAGES={TEST_PAGES}u", "-DAUTO_START=0u"],
                            events=[(20, COMMAND, C_START), (90, COMMAND, C_CUT), (130, COMMAND, C_STOP),
                                    (150, COMMAND, C_START), (270, COMMAND, C_STOP),
                                    (290, COMMAND, C_START), (365, COMMAND, C_STOP)])
for _n, _at in enumerate((5.36, 5.40, 5.44, 5.48, 5.52)):
    cases[f"prepare_pull_{_n}"] = case([(30, 0), (70, 1), (40, 0)],
                                       events=[(_at, STICK_OUT, 0), (20, STICK_IN, 0)])
def _with_cut(name, n):
    return cases[name] + [f"-DMETA_CUT_AT={n}u"]
for _n in range(9, 20):
    cases[f"ready_cut_{_n}"] = _with_cut("cut_next_ready", _n)
for _n in range(9, 19):
    cases[f"early_cut_{_n}"] = _with_cut("chain_ends_early", _n)
for _n in (8, 9):
    cases[f"start_cut_{_n}"] = _with_cut("start_power_5", _n)
cases["reuse_cut_14"] = _with_cut("cut_power_5", 14)
for _n in range(9, 17):
    cases[f"mark_cut_{_n}"] = _with_cut("mark_power", _n)
for _n in range(13, 19):
    cases[f"grow_cut_{_n}"] = _with_cut("log_full_cluster", _n)
cases["append_cut_13"] = _with_cut("log_ours", 13)
cases["late_cut"] = case([(10, 0), (300, 1), (40, 0)], layout=3, ahead=100, count=30,
                         events=[(40, COMMAND, C_CUT), (75, COMMAND, C_CUT)])
for _n in range(6, 11):
    cases[f"late_cut_{_n}"] = cases["late_cut"] + [f"-DMETA_CUT_AT={_n}u"]
cases.update({
    "touched_out": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                        events=[(20, STICK_OUT, 0), (22, TOUCH, 0), (25, STICK_IN, 0),
                                (40, COMMAND, C_START), (100, COMMAND, C_STOP)]),
    "touched_armed": case([(10, 0), (150, 1), (40, 0)], extra=NORMAL,
                          events=[(20, TOUCH, 0), (40, COMMAND, C_START), (60, COMMAND, C_START),
                                  (100, COMMAND, C_STOP)]),
    "coarse_many_chain": case([(10, 0), (200, 1), (40, 0)],
                              stalls=[(30 + 100 * k, 2.8) for k in range(10)] + [(2180, 3.3)],
                              **CHAIN),
})


def run(command):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode:
        raise RuntimeError(" ".join(command) + "\n" + result.stdout[-2000:] + result.stderr[-4000:])
    return result.stdout


def pairs(line):
    return {key: int(value, 16) for key, value in
            (pair.split("=") for pair in line.split()[1:] if "=" in pair)}


def write_image(path, total_sectors, lines):
    with open(path, "wb") as f:
        f.truncate(total_sectors * 512)
        for line in lines:
            _, sector, data = line.split()
            f.seek(int(sector.split("=")[1], 16) * 512)
            f.write(bytes.fromhex(data))


def build_and_run(tag, flags, tmp):
    firmware_flags = [f for f in flags if f.startswith(("-DFILE_", "-DNEXT_", "-DBURST_",
                                                        "-DCONTROL_", "-DAUTO_", "-DCOUNTDOWN_",
                                                        "-DSIM_START_", "-DDEPTH_"))]
    objects = []
    for source, extra in ((os.path.join(PROBE, "src", "stream.c"), ["-DSIM", *firmware_flags]),
                          (os.path.join(PROBE, "src", "fat.c"), []),
                          (os.path.join(PROBE, "src", "recfile.c"), []),
                          (os.path.join(PROBE, "src", "pack.c"), build.SOURCE_FLAGS["pack"]),
                          (os.path.join(PROBE, "src", "listen.c"), []),
                          (os.path.join(PROBE, "src", "reclog.c"), []),
                          (os.path.join(HERE, "stream_check.c"), ["-DSIM", *flags])):
        obj = os.path.join(build.BUILD, "sim_" + tag + "_" + os.path.basename(source) + ".o")
        run([build.TOOL_PREFIX + "gcc", *build.CFLAGS, *extra, "-c", source, "-o", obj])
        objects.append(obj)
    crt = os.path.join(build.BUILD, "sim_" + tag + "_crt.o")
    run([build.TOOL_PREFIX + "gcc", "-m2a-nofpu", "-mb", "-c",
         os.path.join(HERE, "crt_sim.S"), "-o", crt])
    elf = os.path.join(build.BUILD, "sim_" + tag + ".elf")
    run([build.TOOL_PREFIX + "ld", "-EB", "-T",
         os.path.join(HERE, "sim.ld"), "-o", elf, crt, *objects])
    lines = run([build.TOOL_PREFIX + "run", elf]).splitlines()
    if "DONE" not in lines:
        return None
    result = pairs(next(item for item in lines if item.startswith("R ")))
    result["takes"] = [pairs(item) for item in lines if item.startswith("T ")]
    result["views"] = [(v["block"] / 4000.0, v["state"], v["reason"], v["cut"])
                       for v in (pairs(item) for item in lines if item.startswith("V "))]
    last = pairs(next(item for item in lines if item.startswith("W ")))
    result["view_lost"] = last["view_lost"]
    result["counts"] = [(c["block"] / 4000.0, c["left"] - 1 if c["left"] else None)
                        for c in (pairs(item) for item in lines if item.startswith("C "))]
    result["count_lost"], result["count_bad"] = last["count_lost"], last["count_bad"]
    for key in ("left_seen", "left_ready", "left_count_diff", "left_sum_min", "left_sum_max",
                "space_seen", "space_bad", "space_first", "cut_phase"):
        result[key] = last[key]
    result["coarse_stored"] = last["coarse_stored"]
    total = pairs(next(item for item in lines if item.startswith("G ")))["total_sectors"]
    for letter, name in (("B", "before"), ("A", "after")):
        path = os.path.join(tmp, f"{tag}_{name}.img")
        write_image(path, total, [item for item in lines if item.startswith(letter + " ")])
        result[name] = path
    if os.environ.get("DEBUG"):
        print(tag, {k: v for k, v in result.items() if isinstance(v, int)})
        print("  views", result["views"])
        for t in result["takes"]:
            print("  take", t)
    return result


failed = []


def check(value, label):
    print(("PASS " if value else "FAIL ") + label)
    if not value:
        failed.append(label)


class Stick:
    def __init__(self, r):
        self.before = test_fat.Volume(r["before"])
        self.volume = test_fat.Volume(r["after"])
        self.files = self.volume.root_files()
        self.problems = self.volume.audit()
        self.fsck_clean, self.fsck_text = test_fat.fsck(r["after"])
        self.path = r["after"]

    def others_unchanged(self):
        def view(volume):
            return {name: (attr, size, tuple(volume.chain(first)[0]) if first else ())
                    for name, (attr, first, size) in volume.root_files().items()
                    if not name.startswith("REC") and not name.startswith("DJMREC")}
        before = view(self.before)
        after = view(self.volume)
        if "DJMREC  TMP" in self.before.root_files():
            name = "DJMREC  TMP"
            b, a = self.before.root_files()[name], self.volume.root_files().get(name)
            if a != b or self.before.chain(b[1])[0] != self.volume.chain(a[1])[0]:
                return False
        return before == after

    def header(self, first_cluster):
        return self.volume.sector(DATA_START + (first_cluster - 2) * SPC)

    @staticmethod
    def content(volume, first, offset, length):
        chain = volume.chain(first)[0]
        out = bytearray()
        while length > 0:
            cluster = chain[offset // CLUSTER]
            data = volume.sector(DATA_START + (cluster - 2) * SPC + offset % CLUSTER // 512)
            take = min(length, 512 - offset % 512)
            out += data[offset % 512:offset % 512 + take]
            offset += take
            length -= take
        return bytes(out)

    def tail(self, number):
        rec = self.recording(number)
        if not rec or "data_bytes" not in rec:
            return [], [], False
        start = 512 + rec["data_bytes"]
        raw = self.content(self.volume, rec["first"], start, rec["size"] - start)
        points, labels, at, good = [], [], 0, True
        while at + 8 <= len(raw):
            tag, size = raw[at:at + 4], struct.unpack_from("<I", raw, at + 4)[0]
            body = raw[at + 8:at + 8 + size]
            if tag == b"cue " and len(body) == size and size == 4 + 24 * struct.unpack_from("<I", body)[0]:
                for k in range(struct.unpack_from("<I", body)[0]):
                    ident, position, chunk, start_, block, offset_ = struct.unpack_from("<II4sIII", body, 4 + 24 * k)
                    good &= ident == k + 1 and chunk == b"data" and start_ == 0 and block == 0 and offset_ == position
                    points.append(position / 48000)
            elif tag == b"LIST" and body[:4] == b"adtl" and len(body) == size:
                inner = 4
                while inner + 12 <= len(body):
                    length = struct.unpack_from("<I", body, inner + 4)[0]
                    good &= body[inner:inner + 4] == b"labl"
                    labels.append((struct.unpack_from("<I", body, inner + 8)[0],
                                   body[inner + 12:inner + 8 + length].rstrip(b"\0").decode("latin-1")))
                    inner += 8 + length + (length & 1)
                good &= inner == len(body)
            else:
                good = False
                break
            at += 8 + size + (size & 1)
        return points, labels, good and at == len(raw)

    def text(self, name="REC-LOG TXT", before=False, raw=False):
        volume = self.before if before else self.volume
        entry = volume.root_files().get(name)
        if entry is None:
            return None
        attr, first, size = entry
        text = self.content(volume, first, 0, size).decode("latin-1") if first else ""
        if raw:
            return text
        rest = re.sub(r"REC\d{4}\.WAV (16|24) BIT\r\n", "", text)
        if rest == "DJM RECORDER LOG\r\n" and rest != text and name not in self.before.root_files():
            return None
        return rest

    def legacy_files(self):
        return {k: v for k, v in self.files.items() if not (k == "REC-LOG TXT" and self.text() is None)}

    def waiting(self):
        entry = self.files.get("DJMREC  TMP")
        if entry is None:
            return None
        attr, first, size = entry
        head = self.header(first)
        idle = head[:4] == b"RIFF" and head[44:52] == b"DJMREC44" and head[60:64] == bytes(4) \
            and head[508:512] == bytes(4)
        return {"attr": attr, "first": first, "size": size, "idle": idle,
                "clusters": len(self.volume.chain(first)[0])}

    def recording(self, number):
        entry = self.files.get("REC%04d WAV" % number)
        if entry is None:
            return Missing()
        attr, first, size = entry
        head = self.header(first)
        out = {"attr": attr, "first": first, "size": size, "header": head,
               "first_sector": DATA_START + (first - 2) * SPC}
        if head[:4] == b"RIFF" and head[44:52] == b"DJMREC44":
            out["session"], out["take"], out["capture"] = analyze_wav.decode(head)
            out["data_bytes"] = struct.unpack_from("<I", head, 508)[0]
            out["state"] = struct.unpack_from("<I", head, 60)[0]
            out["flags"] = struct.unpack_from("<I", head, 56)[0]
        return out


def sound(r, name, stick, io=True, leaks=1, clean=True):
    check(r["data_ok"] and r["bounds_ok"] and r["header_ok"] and r["cue_ok"] and r["max_write_sectors"] <= 32
          and r["max_read_sectors"] <= 16,
          f"{name}: every write is bookkeeping in its place or audio inside the take's file, in "
          f"order and exact; no header ahead of the audio")
    if io:
        check(r["first_io_us"] >= SETTLE_US and r["first_alloc_us"] >= SETTLE_US,
              f"{name}: no stick or pool access before the quiet period ended")
    check(r["leaks"] == leaks, f"{name}: pool blocks held at the end: {r['leaks']} (expected {leaks})")
    if clean:
        check(stick.fsck_clean and not stick.problems,
              f"{name}: volume clean for macOS's checker and for the independent audit"
              + ("" if stick.fsck_clean and not stick.problems else f": {stick.problems} {stick.fsck_text[-300:]}"))
    check(stick.others_unchanged(), f"{name}: the other files on the stick are untouched")
    odd = []
    for number in range(1, 40):
        rec = stick.recording(number)
        if rec and rec.get("state") == 2 and not rec["take"]["continued"] and \
                rec["size"] != struct.unpack_from("<I", rec["header"], 4)[0] + 8:
            odd.append(f"REC{number:04d} {rec['size']} != {struct.unpack_from('<I', rec['header'], 4)[0] + 8}")
    check(not odd, f"{name}: every closed recording has the length its header gives"
          + (f": NOT {odd}" if odd else ""))


def kept(r, name, stick, take, number, seconds, ring=RING_FULL, dropped=None, flags=0, reason=2,
         exact=True, kind=KEEP, before=0, index=0, cue=0, lost=None, coarse=0, width=3):
    BLOCK = 24 * width
    t = r["takes"][take] if take < len(r["takes"]) else None
    rec = stick.recording(number)
    if t is None or not rec or "take" not in rec:
        check(False, f"{name}: REC{number:04d}.WAV or its record is missing")
        return
    stored = blocks(seconds) if exact else t["audio_bytes"] // BLOCK
    if dropped is not None:
        stored -= t["dropped"]
    total = blocks(before) + stored
    check(t["kind"] == kind and t["audio_bytes"] == stored * BLOCK
          and t["sectors"] == -(-stored * BLOCK // 512) and t["header_bytes"] == stored * BLOCK
          and t["first_sector"] == rec["first_sector"] and t["take_audio"] == total * BLOCK
          and (kind in (SWITCH, SPLIT) or (t["stored"] == total and t["fifo_left"] == 0)),
          f"{name}: file {take + 1}: {stored / 4000:.2f} s"
          + (f" after {before:.2f} s" if before else "") + ", every stored byte on the stick in order"
          + (", take continues" if kind in (SWITCH, SPLIT) else ""))
    c, k = rec["capture"], rec["take"]
    chain = stick.volume.chain(rec["first"])[0]
    if lost is None:
        lost = min(dropped[0], 16) if dropped is not None else 0
    named = lost + coarse
    behind = (12 + 24 * (cue + named) if cue + named else 0) + (12 + 18 * named if named else 0)
    size_ok = (rec["size"] == len(chain) * CLUSTER) if kind == SWITCH else \
        (rec["size"] == 512 + stored * BLOCK + behind)
    size_ok = size_ok and t["cue_marks"] == cue + named and t["cue_lost"] == lost and \
        t["cue_coarse"] == coarse and c["cue_marks"] == cue and c["cue_lost"] == lost | coarse << 16 and \
        (t["coarse_blocks"] > 0) == (coarse > 0 or bool(rec["flags"] & ST["STORED_16_BIT_IN_FREEZES"])) and \
        (coarse == 0 or t["cue_coarse_first"] == t["coarse_first"])
    if coarse:
        flags |= ST["STORED_16_BIT_IN_FREEZES"]
    size_ok = size_ok and t["bits"] == 8 * width and struct.unpack_from("<IHH", rec["header"], 28) == \
        (96_000 * width, 2 * width, 8 * width)
    check(size_ok and rec["attr"] == 0x20 and rec["state"] == 2
          and rec["data_bytes"] == stored * BLOCK and len(chain) == -(-rec["size"] // CLUSTER)
          and k["actual_words"] == stored * 24 and rec["flags"] == flags and k["number"] == number
          and k["continued"] == (kind in (SWITCH, SPLIT)) and k["chain_index"] == index
          and k["blocks_before"] == blocks(before)
          and (kind in (SWITCH, SPLIT) or c["end_reason"] == reason),
          f"{name}: REC{number:04d}.WAV: {rec['size']} bytes in {len(chain)} clusters, header "
          f"complete, file {index + 1} of its take, {8 * width} bit, flags {rec['flags']:#x}")
    check(c["ring_pages"] == ring and c["maximum_pending_bytes"] <= ring * PAGE,
          f"{name}: ring of {c['ring_pages']} pages, peak {c['maximum_pending_bytes']} bytes")
    if dropped is None:
        check(t["dropped"] == 0 and c["dropped_blocks"] == 0 and c["gap_count"] == 0,
              f"{name}: nothing dropped")
    else:
        gaps, low, high = dropped
        check(c["gap_count"] == gaps and c["dropped_blocks"] == t["dropped"]
              and low <= c["dropped_blocks"] <= high,
              f"{name}: {gaps} gap(s), {c['dropped_blocks']} blocks dropped, exactly the blocks "
              f"the interrupt did not store")


def waiting_ok(name, stick, minimum_bytes=1_000_000):
    w = stick.waiting()
    check(w is not None and w["attr"] == 0x22 and w["idle"] and w["size"] == w["clusters"] * CLUSTER
          and w["size"] >= minimum_bytes,
          f"{name}: a hidden waiting file with a no-audio header is ready "
          f"({(w or {}).get('size', 0) // 1_000_000} MB)")


def status_has(r, *names):
    return all(r["status"] & ST[n] for n in names)


def main():
    selected = dict(cases)
    only = os.environ.get("CASES")
    if only:
        selected = {name: flags for name, flags in cases.items() if name in only.split(",")}
    os.makedirs(build.BUILD, exist_ok=True)
    with tempfile.TemporaryDirectory(prefix="simvol") as tmp:
        with ThreadPoolExecutor(max_workers=int(os.environ.get("JOBS", "6"))) as pool:
            futures = {name: pool.submit(build_and_run, name, flags, tmp)
                       for name, flags in selected.items()}
            results = {}
            for name, future in futures.items():
                try:
                    results[name] = future.result()
                except RuntimeError as error:
                    print(error)
                    results[name] = None
        for name, r in results.items():
            check(r is not None, f"{name}: harness ran to completion")
        results = {name: r for name, r in results.items() if r is not None}
        sticks = {name: Stick(r) for name, r in results.items()}
        evaluate(results, sticks)
    print(f"\n{len(failed)} failed" if failed else "\nall checks passed")
    sys.exit(1 if failed else 0)


class Missing(dict):
    def __missing__(self, key):
        return Missing()

    def __bool__(self):
        return False


HEAD_LOG = "DJM RECORDER LOG\r\n"


def evaluate(results, sticks):
    def get(name):
        return results.get(name), sticks.get(name)

    ran = [(n, r) for n, r in sorted(results.items()) if r]
    torn = [n for n, r in ran if r["link_tx_torn"] or r["status_bad"]]
    silent = [n for n, r in ran if r["status_seen"] == 0]
    check(ran and not torn and not silent,
          f"all {len(ran)} cases: the packet for main never changed while it was being sent, and "
          f"every status in it checked out" + (f"; NOT in {torn[:6]}, none seen in {silent[:6]}" if torn or silent else ""))

    r, s = get("two_takes")
    if r:
        name = "two_takes"
        sound(r, name, s)
        check(r["keeps"] == 2 and r["arms"] == 3 and r["discards"] == 0 and r["fails"] == 0
              and r["status"] == 0 and r["phase"] == PHASES["ARMED"],
              f"{name}: two takes on an empty stick, armed again afterwards")
        kept(r, name, s, 0, 1, 90)
        kept(r, name, s, 1, 2, 90)
        waiting_ok(name, s)
        session = (s.recording(2) or {"session": {}})["session"]
        first = (s.recording(1) or {"session": {}})["session"]
        check(session.get("files_made") == 1 and session.get("files_cut") == 1
              and session.get("files_reused") == 0
              and first.get("prepare_duration_us", 1 << 30) < 1_000_000 and len(s.legacy_files()) == 4,
              f"{name}: the waiting file was made once ({first.get('prepare_duration_us', 0) / 1000:.0f} ms); "
              f"after each take the rest of it became the next waiting file")
        check(r["log_writes"] >= 1 and r["meta_writes"] < 40,
              f"{name}: {r['meta_writes']} bookkeeping writes in all, log written {r['log_writes']} times")

    r, s = get("sound_check")
    if r:
        name = "sound_check"
        sound(r, name, s)
        t = r["takes"]
        check(r["discards"] == 1 and r["keeps"] == 1 and t[0]["kind"] == DISCARD
              and t[0]["stored"] == blocks(50) and t[0]["first_sector"] == t[1]["first_sector"],
              f"{name}: a 30 s sound check is recorded and dropped; the same file takes the next take")
        kept(r, name, s, 1, 1, 90)
        waiting_ok(name, s)
        check(len(s.legacy_files()) == 3 and s.recording(1)["session"]["takes_discarded"] == 1,
              f"{name}: the stick holds one recording and the waiting file, nothing else new")

    r, s = get("restart")
    if r:
        name = "restart"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90)
        kept(r, name, s, 1, 2, 90)
        waiting_ok(name, s)
        session = s.recording(2)["session"]
        check(r["reboots"] == 1 and session["files_reused"] == 1 and session["files_made"] == 0
              and session["takes_kept"] == 1,
              f"{name}: after a power cycle the waiting file is found again, numbering continues")

    r, s = get("power_cut_early")
    if r:
        name = "power_cut_early"
        sound(r, name, s)
        t = r["takes"]
        check(len(t) == 2 and t[0]["kind"] == CUT and t[0]["first_sector"] == t[1]["first_sector"]
              and r["reboots"] == 1,
              f"{name}: power lost 50 s into a take that had no name yet")
        kept(r, name, s, 1, 1, r["takes"][1]["stored"] / 4000, exact=False)
        check(s.recording(1)["session"]["files_reused"] == 1 and not s.recording(2),
              f"{name}: its file is the waiting file again and takes the next recording")
        waiting_ok(name, s)

    r, s = get("power_cut_named")
    if r:
        name = "power_cut_named"
        sound(r, name, s)
        t = r["takes"]
        rec = s.recording(1)
        check(len(t) == 2 and t[0]["kind"] == CUT and bool(rec) and rec["state"] == 2
              and rec["size"] == 512 + t[0]["header_bytes"]
              and 0 < t[0]["audio_bytes"] - t[0]["header_bytes"] <= 15.5 * RATE
              and t[0]["header_bytes"] >= 80 * RATE,
              f"{name}: power lost 100 s into a take: at the next start REC0001.WAV is closed and "
              f"cut to the {t[0]['header_bytes'] / RATE:.1f} s its last header covered")
        kept(r, name, s, 1, 2, r["takes"][1]["stored"] / 4000, exact=False)
        check(s.recording(2)["session"]["files_recovered"] == 1,
              f"{name}: the rest of that file became the waiting file and took the next take")
        waiting_ok(name, s)

    for name, leftover in (("file_full", True), ("file_full_exact", False)):
        r, s = get(name)
        if r:
            sound(r, name, s)
            check(r["keeps"] >= 2 and r["fails"] == 0, f"{name}: {r['keeps']} takes")
            first, second = s.recording(1), s.recording(2)
            if not first or not second:
                check(False, f"{name}: REC0001.WAV and REC0002.WAV exist")
                continue
            full_seconds = (first["session"]["file_bytes"] - 512 - CUE_RESERVE) // PAGE * PAGE / RATE
            kept(r, name, s, 0, 1, full_seconds, reason=1)
            kept(r, name, s, 1, 2, full_seconds, reason=1)
            session = second["session"]
            check((session["files_refilled"] >= 1 and session["files_made"] == 1) if leftover
                  else (session["files_made"] == 2 and session["files_refilled"] == 0),
                  f"{name}: a file of {full_seconds:.2f} s is full: the take ends, "
                  + ("the cluster left over is topped up" if leftover else "new space is found")
                  + " and the music continues in a new take")

    r, s = get("refill")
    if r:
        name = "refill"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90)
        kept(r, name, s, 1, 2, 90)
        session = s.recording(2)["session"]
        w = s.waiting()
        check(session["files_refilled"] == 1 and w["size"] == cap(300) // CLUSTER * CLUSTER,
              f"{name}: after each take the waiting file is topped up to its full size again")

    r, s = get("user_file")
    if r:
        name = "user_file"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90)
        waiting_ok(name, s)

    for name in ("fragmented", "fragmented_full"):
        r, s = get(name)
        if r:
            sound(r, name, s)
            first = s.recording(1)
            extents = first["session"]["extent_count"] if first else 0
            if name == "fragmented":
                kept(r, name, s, 0, 1, 90)
                check(1000 <= extents <= 1024 and first["session"]["file_bytes"] > 150_000_000,
                      f"{name}: waiting file made of {extents} pieces of free space "
                      f"({first['session']['file_bytes'] // 1_000_000} MB)")
            else:
                seconds = (first["session"]["file_bytes"] - 512 - CUE_RESERVE) // PAGE * PAGE / RATE
                kept(r, name, s, 0, 1, seconds, reason=1)
                check(extents >= 1000 and r["keeps"] >= 1 and seconds > 550,
                      f"{name}: a take of {seconds:.0f} s fills a file of {extents} pieces: every "
                      f"piece boundary crossed, every byte in place")
            waiting_ok(name, s)

    for name, flag, text in (("no_space", "NO_SPACE", "a stick without enough room"),
                             ("foreign_tmp", "FS_CHANGED", "a file named DJMREC.TMP that is not the recorder's"),
                             ("fat16", "NOT_FAT32", "a FAT16 stick"),
                             ("geometry_lie", "GEOMETRY", "a boot sector the mixer reads differently")):
        r, s = get(name)
        if r:
            sound(r, name, s, leaks=0, clean=name != "fat16")
            check(status_has(r, flag) and r["meta_writes"] == 0 and r["idle_header_writes"] == 0
                  and r["arms"] == 0 and r["block_writes"] == r["log_writes"],
                  f"{name}: {text}: not armed, nothing written to it"
                  + (f" (reason logged {r['log_writes']} times)" if r["log_writes"] else ""))

    r, s = get("two_takes")
    if r:
        name = "two_takes"
        first = s.volume.root_files()["DJMLOG  BIN"][1]
        at = s.volume.data_start + (first - 2) * SPC + 1
        l = analyze_wav.decode_listen(s.volume.sector(at))
        check(r["listen_writes"] >= 1 and l is not None and l["packets"] > 100000
              and l["missed"] == 0 and l["checksum_bad"] >= l["packets"] and l["polls"] > 8 * 100000,
              f"{name}: the listening post took {(l or {}).get('packets', 0)} packets from main, "
              f"each only once it had arrived whole (it arrives in two parts here: "
              f"{(l or {}).get('checksum_bad', 0)} early looks were put off); none missed")
        asked, answered, last = (l["feed"] >> 16, l["feed"] >> 8 & 0xFF, l["feed"] & 0xFF) if l else (0, 0, 0)
        check(l is not None and l["pipe_packets"] == 2 + answered and l["pipe_bytes"] == 12 + 16 * answered
              and l["clock_flags"] == 1
              and l["seen"] == [(0xB0, 0x11, 0x41, 2), (0x90, 0x4E, 0x7F, 1)]
              and l["tail"] == PANEL_REST + bytes(12),
              f"{name}: the three MIDI messages of the pipe are in the record, with their values "
              f"and counts; the clock flag was counted; main's {answered} answers about the "
              f"recording feed came through the pipe and are not taken for controls")
        check(l is not None and 10 < answered < 60 and asked == answered + 1 and last == 1
              and r["feed_on"] >= asked and r["feed_off"] == 0 and r["feed_changes"] == 1 and r["feed_now"] == 1,
              f"{name}: the recording feed: the recorder asked main for it once it was ready, "
              f"main set its flag at the first request and answered every later one ({asked} requests, "
              f"{answered} answers in the record: every second until the first answer, then every 8 s)")
        t = analyze_wav.decode_tunnel(b"".join(s.volume.sector(at + 1 + i) for i in range(4)))
        pressed_1 = PANEL_REST[:8] + b"\x40" + PANEL_REST[9:]
        pressed_2 = PANEL_REST[:7] + b"\x62" + PANEL_REST[8:]
        check(r["tunnel_writes"] >= 1 and t is not None and t["changes"] == 5
              and t["nonzero"] == l["packets"] and t["first"] == PANEL_REST and t["last"] == PANEL_REST
              and [h[1] for h in t["history"]] == [PANEL_REST, pressed_1, PANEL_REST, pressed_2, PANEL_REST]
              and [h[0] for h in t["history"]] == [1, 2501, 2601, 3001, 3051],
              f"{name}: the panel's bytes come through the tunnel: two buttons pressed and let go "
              f"are in the record, each at its packet ({(t or {}).get('changes', 0) - 1} changes)")
        status = analyze_wav.decode_status(t["status"]) if t else None
        if os.environ.get("DEBUG"):
            print("DEBUG tunnel", t and {k: v for k, v in t.items() if k != "history"}, t and t["history"],
                  status, l["polls"], l["packets"], r["tunnel_writes"])
        check(t is not None and status is not None and status["state"] == 3 and status["seconds"] == 90
              and status["space"] == 0 and status["deep"] and abs(t["sent"] - l["polls"] // 1000) <= 2,
              f"{name}: the status in the record (written while the second take was closed) says "
              f"closing, 90 s, 24 bit, no room given while closing (the next file's number up to "
              f"); {(t or {}).get('sent', 0)} statuses went out, four a second")
        check(r["status_seen"] > 100000 and r["status_states"] == 0b1111 and r["status_seconds"] == 90
              and r["status_steps"] >= t["sent"] - 2 and r["status_flags"] == 0x30 and r["status_marks"] == 0,
              f"{name}: main got a status with every packet ({r['status_seen']}), every one checked "
              f"out, none skipped; they said not ready, ready, recording up to 90 s, closing, and "
              f" that the recording feed is on")

    SEEN = [(0xB0, 0x11, 0x41, 2), (0x90, 0x4E, 0x7F, 1)]
    bad = [n for n, r in ran if r["feed_bad"]]
    idle = [n for n, r in ran if not r["status_states"] & 0b1110 and (r["feed_on"] or r["feed_off"])]
    never = [n for n, r in ran if r["status_states"] & 0b0100 and not r["feed_on"]]
    check(ran and not bad and not idle and not never,
          f"recording feed, all {len(ran)} cases: every request the recorder put into its packet is "
          f"exactly the message main looks for, with the pipe's counter one up; \"on\" only while it "
          f"works with a stick, \"off\" only after; none where no stick was ever ready"
          + (f"; NOT: malformed or at the wrong time in {bad[:6]}, sent without a stick in {idle[:6]}, "
             f"never asked in {never[:6]}" if bad or idle or never else ""))
    r, s = get("no_stick")
    if r:
        check(r["feed_on"] == 0 and r["feed_off"] == 0 and r["status_flags"] & 0x10 == 0,
              "no_stick: without a stick the recorder never writes into the MIDI pipe of its packet")
    r, s = get("stick_pulled")
    if r:
        check(r["feed_off"] == 2 and r["feed_changes"] == 3 and r["feed_now"] == 1,
              f"stick_pulled: when the stick is pulled the recorder asks for \"feed off\" until main has "
              f"answered ({r['feed_off']} requests), and for the feed again when the stick is back")
    r, s = get("feed_refused")
    if r:
        first = s.volume.root_files()["DJMLOG  BIN"][1]
        l = analyze_wav.decode_listen(s.volume.sector(s.volume.data_start + (first - 2) * SPC + 1))
        check(r["feed_refusals"] == r["feed_on"] > 100 and r["feed_now"] == 0 and r["status_flags"] & 0x10 == 0
              and l is not None and l["feed"] & 0xFF == 0x0F and l["seen"] == SEEN,
              f"feed_refused: main refuses the feed all the time: the recorder keeps asking every "
              f"second ({r['feed_on']} requests), never says that the feed is on, the record names the "
              f"refusal; the take is recorded as always")
        take_ok = [t for t in r["takes"]]
        check(len(take_ok) == 1 and r["cue_ok"] == 1,
              "feed_refused: one take, complete, as in the same case without refusals")
    r, s = get("feed_late")
    if r:
        check(20 < r["feed_refusals"] < 35 and r["feed_changes"] == 1 and r["feed_now"] == 1
              and r["status_flags"] & 0x10,
              f"feed_late: main refuses for the first 30 s ({r['feed_refusals']} refusals), then takes the "
              f"request: the feed comes on by itself and the status says so")
    r, s = get("feed_notes")
    if r:
        first = s.volume.root_files()["DJMLOG  BIN"][1]
        l = analyze_wav.decode_listen(s.volume.sector(s.volume.data_start + (first - 2) * SPC + 1))
        answered = l["feed"] >> 8 & 0xFF if l else 0
        if os.environ.get("DEBUG"):
            print("DEBUG feed_notes", l and {k: v for k, v in l.items() if k in ("pipe_packets", "pipe_bytes", "seen", "feed")})
        check(l is not None and l["seen"] == SEEN and l["pipe_packets"] == 2 + answered + 3
              and l["pipe_bytes"] == 12 + 16 * answered + 60 and r["feed_now"] == 1 and r["feed_bad"] == 0,
              "feed_notes: three of main's notes \"a channel was brought in\" during the take: counted "
              "as pipe data, not taken for controls, and not taken for answers")

    told = sorted(n for n, r in ran if r["status"] & ST["GUARD_CHANGED"])
    check(told == (["guard_changed"] if "guard_changed" in results else []),
          f"all {len(ran)} cases: the rest of the variables' piece of memory is reported as changed "
          f"in the one case where something writes there, and in no other" + (f"; reported in {told}" if told else ""))
    r, s = get("guard_changed")
    if r:
        rec = s.recording(1)
        check(len(r["takes"]) == 1 and rec and rec["session"]["status_flags"] & ST["GUARD_CHANGED"]
              and rec["flags"] == 0 and r["cue_ok"] == 1
              and r["takes"][0]["dropped"] == 0 and r["takes"][0]["audio_bytes"] == 90 * RATE
              and r["status"] & ST["GUARD_CHANGED"],
              "guard_changed: a write next to the variables 30 s into a take: the take goes on and is "
              "complete; the report in its header and the log carry the flag GUARD_CHANGED among the "
              "flags since power-on")

    r, s = get("small_log")
    if r:
        sound(r, "small_log", s)
        kept(r, "small_log", s, 0, 1, 90)
        check(r["listen_writes"] == 0 and r["log_writes"] >= 1,
              "small_log: a log file of one sector gets the report and nothing behind it")

    r, s = get("medium_log")
    if r:
        sound(r, "medium_log", s)
        kept(r, "medium_log", s, 0, 1, 90)
        check(r["listen_writes"] >= 1 and r["tunnel_writes"] == 0,
              "medium_log: a log file of four sectors gets the listening record and nothing behind it")

    r, s = get("no_log")
    if r:
        sound(r, "no_log", s)
        kept(r, "no_log", s, 0, 1, 90)
        check(r["log_writes"] == 0, "no_log: works without a DJMLOG.BIN on the stick")

    r, s = get("no_stick")
    if r:
        sound(r, "no_stick", s, io=False, leaks=0)
        check(r["block_reads"] == 0 and r["block_writes"] == 0 and r["alloc"] == 0 and r["status"] == 0,
              "no_stick: without a stick nothing is read, written or allocated")

    r, s = get("stick_late")
    if r:
        sound(r, "stick_late", s)
        kept(r, "stick_late", s, 0, 1, 90)
        check(r["first_io_us"] >= 20_000_000, "stick_late: a stick inserted 20 s after power-on is found and used")

    r, s = get("stick_swap")
    if r:
        name = "stick_swap"
        sound(r, name, s)
        t = r["takes"]
        check(r["aborts"] == 1 and t[0]["kind"] == ABORT and t[0]["sectors"] == 0
              and t[0]["headers"] == 0 and status_has(r, "REVALIDATION"),
              f"{name}: another stick while armed: the take is dropped before any write")
        kept(r, name, s, 1, 1, r["takes"][1]["stored"] / 4000, exact=False)

    r, s = get("stick_pulled")
    if r:
        name = "stick_pulled"
        sound(r, name, s)
        t = r["takes"]
        check(r["fails"] == 1 and t[0]["kind"] == FAIL and status_has(r, "DIRECT_WRITE"),
              f"{name}: stick pulled 30 s into a take: the take stops")
        kept(r, name, s, 1, 1, r["takes"][1]["stored"] / 4000, exact=False)
        waiting_ok(name, s)

    r, s = get("dirty")
    if r:
        sound(r, "dirty", s, io=False, leaks=0)
        check(status_has(r, "MEMORY_DIRTY") and r["block_reads"] == 0 and r["alloc"] == 0
              and r["block_writes"] == 0 and r["phase"] == PHASES["FAILED"],
              "dirty: unexpected memory content stops before any stick or pool access")

    r, s = get("map_corrupt")
    if r:
        name = "map_corrupt"
        sound(r, name, s, leaks=0)
        check(status_has(r, "MAP_MISMATCH") and r["fails"] == 1 and r["phase"] == PHASES["FAILED"]
              and r["takes"][0]["sectors"] == 0 and r["takes"][0]["headers"] == 0,
              f"{name}: checksum caught the changed map before the first write; recorder stopped")

    for name, before, armed in (("extra_changed", (0,), ()), ("extra_changed_armed", (), (2,)),
                                ("extra_changed_twice", (1,), (2,)), ("extra_changed_all", (0, 1, 2), ())):
        r, s = get(name)
        if r:
            out = before + armed
            ring = RING_FULL - sum(EXTRA_PIECES[n] for n in out)
            sound(r, name, s)
            kept(r, name, s, 0, 1, 90, ring=ring)
            session = s.recording(1)["session"]
            state = (2 if len(out) == 3 else 1) | sum(1 << n for n in out) << 8
            check(r["status"] == ST["EXTRA_RAM_CHANGED"] and session["extra_state"] == state
                  and session["ring_base_pages"] == ring - 9,
                  f"{name}: piece {' and '.join(str(n) for n in out)} of the extra RAM changed"
                  f"{' before arming' if before else ''}{' and' if before and armed else ''}"
                  f"{' while armed' if armed else ''}: {'they are' if len(out) > 1 else 'it is'} out of "
                  f"the ring ({ring} pages), the other pieces stay in")

    r, s = get("pool_fail")
    if r:
        sound(r, "pool_fail", s)
        kept(r, "pool_fail", s, 0, 1, 90, ring=RING_NO_POOL, flags=ST["ALLOC_FAILED"] | ST["SMALL_RING"])

    r, s = get("map_alloc_fail")
    if r:
        sound(r, "map_alloc_fail", s)
        kept(r, "map_alloc_fail", s, 0, 1, 90)
        check(status_has(r, "ALLOC_FAILED"), "map_alloc_fail: no memory for the map twice: tried again, then recorded")

    r, s = get("release_fail")
    if r:
        sound(r, "release_fail", s, leaks=r["leaks"])
        check(status_has(r, "RELEASE_FAILED") and r["keeps"] == 1, "release_fail: pool release error is reported")

    r, s = get("write_fail")
    if r:
        name = "write_fail"
        sound(r, name, s, leaks=0)
        check(r["fails"] == 3 and r["keeps"] == 0 and r["phase"] == PHASES["FAILED"]
              and status_has(r, "DIRECT_WRITE"),
              f"{name}: three takes in a row stopped by write errors: the recorder gives up")

    r, s = get("invalid_armed")
    if r:
        sound(r, "invalid_armed", s)
        kept(r, "invalid_armed", s, 0, 1, r["takes"][0]["stored"] / 4000, exact=False)
        check(87.9 * 4000 <= r["takes"][0]["stored"] <= 88 * 4000, "invalid_armed: the take starts when the format is right again")
        check(s.recording(1)["capture"]["invalid_blocks"] >= blocks(3.9) and r["status"] == 0,
              "invalid_armed: blocks of an unknown format while armed are ignored, not recorded")

    r, s = get("invalid_active")
    if r:
        name = "invalid_active"
        sound(r, name, s)
        t = r["takes"]
        kept(r, name, s, 0, 1, t[0]["stored"] / 4000, exact=False, reason=3, flags=ST["INVALID_BLOCK"])
        check(len(t) == 2 and 19.9 * 4000 <= t[0]["stored"] <= 20.1 * 4000,
              f"{name}: an invalid block ends the take after 20 s; it is kept, named when closed, "
              f"and flagged")
        kept(r, name, s, 1, 2, t[1]["stored"] / 4000, exact=False)

    r, s = get("spike")
    if r:
        sound(r, "spike", s)
        kept(r, "spike", s, 0, 1, 90)
        k = s.recording(1)["take"]
        check(k["slow_writes[0]"] == 1 and k["slow_writes[1]"] == 1 and k["slow_writes[2]"] == 0
              and k["slow_count"] == 1 and 790 <= analyze_wav.slow_log(k)[0][1] <= 830,
              f"spike: 0.8 s stall absorbed and logged {analyze_wav.slow_log(k)}")

    r, s = get("stall_3s")
    if r:
        sound(r, "stall_3s", s)
        kept(r, "stall_3s", s, 0, 1, 90, coarse=1)
        c, t0 = s.recording(1)["capture"], r["takes"][0]
        check(c["maximum_pending_bytes"] <= RING_FULL * PAGE and c["maximum_pending_blocks"] >= 2.9 * 4000
              and c["coarse"] >> 24 == 1 and c["coarse"] & 0xFFFFFF == t0["coarse_blocks"] == r["coarse_stored"]
              and 2.4 * 4000 < t0["coarse_first"] / 12 - 50 * 160 < 2.9 * 4000,
              f"stall_3s: a 3.0 s stall fits the ring (most that waited: "
              f"{c['maximum_pending_blocks'] / 4000:.2f} s); from the moment the ring was three quarters full, "
              f"{(t0['coarse_first'] / 12 - 50 * 160) / 4000:.2f} s into the stall, the sound was stored with "
              f"16 bits, {t0['coarse_blocks'] / 4000:.2f} s of it, until the ring had emptied; the report "
              f"counts one stretch of exactly that length")

    r, s = get("stall_5s5")
    if r:
        sound(r, "stall_5s5", s)
        most = 14500 + 2 * (BURST - 1) * PAGE_BLOCKS
        kept(r, "stall_5s5", s, 0, 1, 90, dropped=(2, 11000, most), flags=ST["AUDIO_DROPPED"], coarse=2)

    for name in ("pack_ramp", "pack_music"):
        r, s = get(name)
        if r:
            sound(r, name, s)
            kept(r, name, s, 0, 1, 90)
            if name == "pack_ramp":
                kept(r, name, s, 1, 2, 90)
            c = s.recording(1)["capture"]
            check(r["status"] == 0 and c["maximum_pending_bytes"] <= 2 * PAGE + (BURST - 1) * PAGE
                  and c["maximum_pending_blocks"] <= 2 * 1280 + BURST * PAGE_BLOCKS,
                  f"{name}: audio that packs: bit-exact in the file; the ring never held more "
                  f"than {c['maximum_pending_bytes']} bytes "
                  f"({c['maximum_pending_blocks'] / 4000:.2f} s of audio)")

    r, s = get("pack_music_stall")
    if r:
        name = "pack_music_stall"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90, coarse=2)
        c = s.recording(1)["capture"]
        check(r["status"] == ST["STORED_16_BIT_IN_FREEZES"] and c["maximum_pending_blocks"] >= 5.4 * 4000
              and c["maximum_pending_bytes"] < RING_FULL * PAGE and c["coarse"] >> 24 == 2,
              f"{name}: two stalls of 5.5 s, a fifth longer than the ring holds of this sound at "
              f"24 bit: nothing is lost, the last quarter of the ring took it with 16 bits "
              f"({(c['coarse'] & 0xFFFFFF) / 4000:.2f} s in two stretches): "
              f"{c['maximum_pending_blocks'] / 4000:.2f} s waited in "
              f"{c['maximum_pending_bytes']} of {RING_FULL * PAGE} bytes, nothing dropped")

    r, s = get("pack_music_chain")
    if r:
        name = "pack_music_chain"
        sound(r, name, s)
        full = (cap(90) // CLUSTER * CLUSTER - 512 - CUE_RESERVE) // PAGE * PAGE / RATE
        kept(r, name, s, 0, 1, full, kind=SWITCH)
        kept(r, name, s, 1, 2, full, kind=SWITCH, before=full, index=1)
        kept(r, name, s, 2, 3, 220 - 2 * full, before=2 * full, index=2)
        check(r["switches"] == 2 and r["status"] == 0,
              f"{name}: a take of packed audio through three files, every byte in order")

    r, s = get("pack_music_cut")
    if r:
        name = "pack_music_cut"
        sound(r, name, s)
        check(r["status"] == 0 and len(s.legacy_files()) >= 3 and s.recording(1)["state"] == 2
              and s.recording(2)["state"] == 2,
              f"{name}: power cut during a take of packed audio: the recording is closed at "
              f"the next start and the next take recorded")

    def by_hand(name, r, s, lengths, flags, reasons=None, cue=0):
        sound(r, name, s)
        ok = len(r["takes"]) == len(lengths) and r["status"] == 0 and r["discards"] == 0
        seen = []
        for i, (length, flag) in enumerate(zip(lengths, flags)):
            rec = s.recording(i + 1)
            if not rec or "capture" not in rec:
                ok = False
                break
            seconds = rec["data_bytes"] / RATE
            seen.append(round(seconds, 2))
            reason = (reasons or [4] * len(lengths))[i]
            ok = ok and abs(seconds - length) < 0.05 and rec["capture"]["by_hand"] == flag \
                and rec["capture"]["end_reason"] == reason and rec["state"] == 2 \
                and rec["size"] == 512 + rec["data_bytes"] + (12 + 24 * cue if cue else 0) \
                and rec["capture"]["dropped_blocks"] == 0
        return ok, seen

    r, s = get("hand_start")
    if r:
        ok, seen = by_hand("hand_start", r, s, [130], [3])
        check(ok and len(s.legacy_files()) == 3,
              f"hand_start: automatic start off: music alone starts nothing, a stop while idle and "
              f"a second start are ignored, the take runs from the start key to the stop key "
              f"through 60 s of silence ({seen} s) and nothing starts afterwards")
        waiting_ok("hand_start", s)

    r, s = get("hand_early")
    if r:
        rec = s.recording(1)
        seconds = rec["data_bytes"] / RATE if rec and "capture" in rec else 0
        sound(r, "hand_early", s)
        check(len(r["takes"]) == 1 and 92.5 < seconds < 95.5 and rec["capture"]["by_hand"] == 3,
              f"hand_early: a start pressed before the recorder is armed is not lost: the take "
              f"begins when it arms and runs to the stop ({seconds:.2f} s)")

    r, s = get("hand_short")
    if r:
        ok, seen = by_hand("hand_short", r, s, [5, 60], [3, 3])
        check(ok, f"hand_short: takes by hand of {seen} s: the short one is kept too")

    r, s = get("hand_stop_auto")
    if r:
        ok, seen = by_hand("hand_stop_auto", r, s, [30, 80], [2, 0], reasons=[4, 2])
        check(ok, f"hand_stop_auto: a take that began by itself is stopped by hand after 30 s and "
                  f"kept; the music that goes on starts nothing; after quiet the next music "
                  f"starts a take as usual ({seen} s)")

    r, s = get("hand_toggle")
    if r:
        ok, seen = by_hand("hand_toggle", r, s, [70], [3])
        check(ok, f"hand_toggle: one key starts and stops ({seen} s); releases do nothing")

    def cues(t, seconds):
        got = [t[f"cue{i}"] / 48000 for i in range(min(t["cue_marks"], 4))]
        tolerance = 0.02 if BURST == 1 else 0.12
        return len(got) == len(seconds) and all(abs(a - b) < tolerance for a, b in zip(got, seconds)), \
            [round(g, 3) for g in got]

    r, s = get("mark_take")
    if r:
        name = "mark_take"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90, cue=3)
        ok, got = cues(r["takes"][0], [10, 25.5, 50])
        check(ok and s.recording(1)["capture"]["marks_set"] == 3,
              f"{name}: three markers are behind the audio as a cue chunk at {got} s of the file; "
              f"the one pressed before the take is not")
        waiting_ok(name, s)

    r, s = get("mark_restart")
    if r:
        name = "mark_restart"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90, cue=2)
        rec = s.recording(1)
        check(r["reboots"] == 1 and r["cue_ok"] and rec["size"] == 512 + rec["data_bytes"] + 12 + 24 * 2
              and r["keeps"] == 1 and r["fails"] == 0,
              f"{name}: after the mixer was switched off and on, the newest recording still has its "
              f"two markers behind the audio (the recorder looks at it at every start and used to "
              f"cut them off)")

    r, s = get("mark_chain")
    if r:
        name = "mark_chain"
        sound(r, name, s)
        full = (cap(90) // CLUSTER * CLUSTER - 512 - CUE_RESERVE) // PAGE * PAGE / RATE
        kept(r, name, s, 0, 1, full, kind=SWITCH, cue=1)
        kept(r, name, s, 1, 2, full, kind=SWITCH, before=full, index=1, cue=1)
        kept(r, name, s, 2, 3, 220 - 2 * full, before=2 * full, index=2, cue=1)
        oks = [cues(r["takes"][i], [p]) for i, p in enumerate((30, 100 - full, 185 - 2 * full))]
        check(all(o for o, _ in oks),
              f"{name}: a marker in each of three files of one take, each at its place in its "
              f"own file ({[g for _, g in oks]} s)")

    r, s = get("mark_hand")
    if r:
        name = "mark_hand"
        ok, seen = by_hand(name, r, s, [55.37], [3], cue=2)
        okc, got = cues(r["takes"][0], [10, 41.23])
        check(ok and okc, f"{name}: a take by hand of {seen} s that ends inside a sector: the "
                          f"sector is written again with the two markers behind it ({got} s)")

    for name in ("cut_take", "cut_marks"):
        r, s = get(name)
        if r:
            sound(r, name, s)
            t = r["takes"]
            lengths = [x["audio_bytes"] / RATE for x in t]
            marks = (1, 1, 0) if name == "cut_marks" else (0, 0, 0)
            if len(t) == 3:
                kept(r, name, s, 0, 1, lengths[0], kind=SPLIT, exact=False, cue=marks[0])
                kept(r, name, s, 1, 2, lengths[1], kind=SPLIT, exact=False, before=lengths[0], index=1,
                     cue=marks[1])
                kept(r, name, s, 2, 3, lengths[2], exact=False, before=lengths[0] + lengths[1], index=2)
            check(len(t) == 3 and r["splits"] == 2 and r["switches"] == 0 and r["status"] == 0
                  and 59.7 < lengths[0] <= 60.15 and 59.7 < lengths[1] < 60.3
                  and abs(sum(lengths) - 220) < 1e-9 and t[2]["stored"] == blocks(220)
                  and all(x["audio_bytes"] % PAGE == 0 for x in t[:2]),
                  f"{name}: one take of 220 s cut twice by hand: files of "
                  f"{[round(x, 2) for x in lengths]} s, the first two end on a page, together "
                  f"they hold every block of the take in order")
            if name == "cut_marks" and len(t) == 3:
                a, got_a = cues(t[0], [30])
                b, got_b = cues(t[1], [90 - lengths[0]])
                check(a and b, f"{name}: each cut file carries its marker ({got_a}, {got_b} s); a "
                               f"1.5 s stall of the stick during the first cut lost nothing")
            waiting_ok(name, s)

    r, s = get("cut_refused")
    if r:
        name = "cut_refused"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90)
        check(r["splits"] == 0 and len(r["takes"]) == 1,
              f"{name}: a cut that would leave less than the smallest reserve is not taken; "
              f"the take stays in its file")

    r, s = get("hand_mixer")
    if r:
        name = "hand_mixer"
        sound(r, name, s)
        t = r["takes"]
        lengths = [x["audio_bytes"] / RATE for x in t]
        ok = len(t) == 3 and r["splits"] == 1 and r["status"] == 0
        a, got_a = cues(t[0], [20]) if ok else (False, [])
        b, got_b = cues(t[1], [80 - lengths[0]]) if ok else (False, [])
        check(ok and a and b and abs(lengths[0] - 60) < 0.2 and abs(lengths[0] + lengths[1] - 120) < 0.05
              and abs(lengths[2] - 90) < 1e-9
              and s.recording(2)["capture"]["by_hand"] == 3 and s.recording(3)["capture"]["by_hand"] == 0,
              f"{name}: with the test variant's keys and one message per press: by hand "
              f"{[round(x, 2) for x in lengths[:2]]} s in two files (cut), a marker in each "
              f"({got_a}, {got_b} s), stopped by hand; the music still playing starts nothing; "
              f"the next music starts a take by itself ({round(lengths[2], 2) if ok else '?'} s)")
        check(r["status_marks"] == 1 and r["status_flags"] & 1 and r["status_bad"] == 0,
              f"{name}: the status told main of the markers (never more than {r['status_marks']}: one "
              f"in each of the two files) and of the start by hand")

    r, s = get("menu_mixer")
    if r:
        name = "menu_mixer"
        sound(r, name, s)
        t = r["takes"]
        lengths = [x["audio_bytes"] / RATE for x in t]
        ok = len(t) == 3 and r["splits"] == 1 and r["status"] == 0
        a, got_a = cues(t[0], [20]) if ok else (False, [])
        b, got_b = cues(t[1], [80 - lengths[0], 81 - lengths[0]]) if ok else (False, [])
        check(ok and a and b and abs(lengths[0] - 60) < 0.2 and abs(lengths[0] + lengths[1] - 120) < 0.05
              and abs(lengths[2] - 90) < 1e-9
              and s.recording(2)["capture"]["by_hand"] == 3 and s.recording(3)["capture"]["by_hand"] == 0,
              f"{name}: driven by the panel's menu commands: by hand "
              f"{[round(x, 2) for x in lengths[:2]]} s in two files (cut), a marker in the first, two "
              f"in the second ({got_a}, {got_b} s), stopped; the music still playing starts nothing; "
              f"the next music starts a take by itself ({round(lengths[2], 2) if ok else '?'} s)")
        first = s.volume.root_files()["DJMLOG  BIN"][1]
        at = s.volume.data_start + (first - 2) * SPC + 2
        tl = analyze_wav.decode_tunnel(b"".join(s.volume.sector(at + i) for i in range(4)))
        check(tl is not None and tl["commands"] == 6 and tl["last_command"] == (6 << 4 | C_STOP)
              and r["status_files"] == 2 and r["status_marks"] == 2 and r["status_file2_marks_first"] == 0,
              f"{name}: six commands taken, each once; the status named the second file of the take; "
              f"its marker count starts again with the file (0, then up to 2; the first file had 1)"
              + (f" GOT {tl and (tl['commands'], hex(tl['last_command']))} {r['status_files']} {r['status_marks']}"
                 if os.environ.get("DEBUG") else ""))

    r, s = get("menu_mixer")
    if r:
        check(r["status_file2_first"] <= 2 and 58 <= r["status_file2_most"] <= 60
              and 58 <= r["status_seconds"] <= 90,
              f"menu_mixer: the time in the status starts again with the cut: "
              f"{r['status_file2_first']} s when the second file is first named, "
              f"{r['status_file2_most']} s at its end")

    r, s = get("menu_idle")
    if r:
        ok, seen = by_hand("menu_idle", r, s, [70], [3])
        check(ok and len(s.legacy_files()) == 3 and r["starts"] == 1,
              f"menu_idle: the build as given out: music alone starts nothing, before the take and "
              f"after it; stop, marker and cut with no take running do nothing; start begins a "
              f"take, a second start changes nothing; one take of {seen} s")

    r, s = get("stall_no_extra")
    if r:
        sound(r, "stall_no_extra", s)
        kept(r, "stall_no_extra", s, 0, 1, 90, ring=RING_NO_EXTRA,
             dropped=(1, 6000, 8500 + (BURST - 1) * PAGE_BLOCKS), flags=ST["AUDIO_DROPPED"], coarse=1)

    r, s = get("full_45")
    if r:
        name = "full_45"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 2720)
        session = s.recording(1)["session"]
        check(session["file_bytes"] > 1_000_000_000 and session["prepare_duration_us"] < 5_000_000,
              f"{name}: 45 minutes into a file made from the whole free space "
              f"({session['file_bytes'] // 1_000_000} MB, prepared in "
              f"{session['prepare_duration_us'] / 1e6:.2f} s)")
        waiting_ok(name, s)

    full = (cap(90) // CLUSTER * CLUSTER - 512 - CUE_RESERVE) // PAGE * PAGE / RATE
    for name in ("chain", "chain_fragmented", "chain_stall"):
        r, s = get(name)
        if r:
            sound(r, name, s)
            check(r["arms"] == 2 and r["starts"] == 1 and r["switches"] == 2 and r["keeps"] == 1
                  and len(r["takes"]) == 3 and r["fails"] == 0
                  and r["status"] == (ST["STORED_16_BIT_IN_FREEZES"] if name == "chain_stall" else 0),
                  f"{name}: one take of 220 s through three files of {full:.2f} s")
            marks = [x["cue_coarse"] for x in r["takes"]] + [0, 0, 0]
            kept(r, name, s, 0, 1, full, kind=SWITCH, coarse=marks[0])
            kept(r, name, s, 1, 2, full, kind=SWITCH, before=full, index=1, coarse=marks[1])
            kept(r, name, s, 2, 3, 220 - 2 * full, before=2 * full, index=2, coarse=marks[2])
            check(sum(marks) == (1 if name == "chain_stall" else 0),
                  f"{name}: stretches stored with 16 bits: {sum(marks)}")
            last = r["takes"][-1] if r["takes"] else {"dropped": 1, "stored": 0, "take_audio": 0}
            check(last["dropped"] == 0 and last["stored"] == blocks(220) and last["take_audio"] == blocks(220) * BLOCK,
                  f"{name}: every block from the trigger to the end is on the stick: nothing lost "
                  f"at the two file changes")
            session = s.recording(3)["session"]
            check(session["files_switched"] == 2 and session["files_made"] >= 3,
                  f"{name}: report: 2 file changes, {session['files_made']} files made on the stick")
            waiting_ok(name, s)
            if name == "chain_stall":
                slow = sum(s.recording(i)["take"]["slow_writes[2]"] for i in (1, 2, 3))
                check(slow == 2, f"{name}: two 1.5 s stalls around the first file change absorbed")

    r, s = get("test_build")
    if r:
        name = "test_build"
        sound(r, name, s)
        t = r["takes"]
        kept(r, name, s, 0, 1, 90, kind=SWITCH)
        kept(r, name, s, 1, 2, 90, kind=SWITCH, before=90, index=1)
        kept(r, name, s, 2, 3, 70, before=180, index=2)
        kept(r, name, s, 3, 4, 90, kind=SWITCH)
        kept(r, name, s, 4, 5, 30, before=90, index=1)
        w = s.waiting()
        check(r["switches"] == 3 and r["status"] == 0 and w is not None
              and w["size"] == TEST_RESERVE // CLUSTER * CLUSTER
              and s.recording(1)["size"] == TEST_RESERVE // CLUSTER * CLUSTER,
              f"{name}: with the constants of the test build every full file holds 90.000 s "
              f"exactly, although its reserve is larger; a 250 s take is in three files and a "
              f"120 s take in two; afterwards a full waiting file is ready")

    r, s = get("test_build_edge")
    if r:
        name = "test_build_edge"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90, kind=SWITCH)
        kept(r, name, s, 1, 2, 90, kind=SWITCH, before=90, index=1)
        kept(r, name, s, 2, 3, 90, before=180, index=2)
        kept(r, name, s, 3, 4, 90)
        check(r["switches"] == 2 and r["status"] == 0 and len(r["takes"]) == 4
              and s.waiting() is not None,
              f"{name}: a take that ends by silence at the very block that fills its file "
              f"ends in that file: no empty file follows")

    r, s = get("chain_ends_early")
    if r:
        name = "chain_ends_early"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 75)
        session_log, _, _ = analyze_wav.decode(s.volume.sector(s.volume.data_start + (s.volume.root_files()["DJMLOG  BIN"][1] - 2) * SPC))
        w = s.waiting()
        check(r["switches"] == 0 and session_log["files_made"] == 2 and session_log["files_cut"] == 1
              and w is not None and w["size"] == cap(90) // CLUSTER * CLUSTER and len(s.legacy_files()) == 3,
              f"{name}: the take ended in its first file after the next one had been made: the "
              f"unused part was given back, the new file is the waiting file")

    r, s = get("chain_stick_full")
    if r:
        name = "chain_stick_full"
        sound(r, name, s, leaks=0)
        t = r["takes"]
        lengths = [x["audio_bytes"] / RATE for x in t]
        before = 0.0
        for i, x in enumerate(t):
            last = i == len(t) - 1
            kept(r, name, s, i, i + 1, lengths[i], before=before, index=i, reason=1, exact=False,
                 kind=KEEP if last else SWITCH)
            before += lengths[i]
        check(r["switches"] == len(t) - 1 >= 1 and status_has(r, "NO_SPACE") and t[-1]["dropped"] == 0
              and lengths[0] == full and all(n < full for n in lengths[1:]),
              f"{name}: the take runs on through the last free space in files of "
              f"{', '.join('%.1f' % n for n in lengths)} s, then the stick is full: it ends "
              f"there, nothing lost before")

    def countdown(r):
        shown = [(t_, left) for t_, left in r["counts"] if left is not None]
        after = [v[0] for v in r["views"] if shown and v[0] > shown[0][0] and v[1] != 2]
        return [left for _, left in shown], [t_ for t_, _ in shown], after[0] if after else None

    for name, files_ in (("count_stick_full", 2), ("count_last_file", 1)):
        r, s = get(name)
        if r:
            sound(r, name, s, leaks=0)
            values, times, end = countdown(r)
            t = r["takes"]
            lengths = [x["audio_bytes"] / RATE for x in t]
            ends = [a + b + 1 for a, b in zip(times, values)]
            check(len(t) == files_ and r["count_bad"] == 0 and r["count_lost"] == 0
                  and values == list(range(29, -1, -1)) and max(ends) - min(ends) <= 0.3
                  and end is not None and abs(end - max(ends)) <= 0.6
                  and r["counts"][-1][1] is None and t[-1]["dropped"] == 0
                  and abs(times[0] - (10 + sum(lengths) - 30)) <= 0.6
                  and (files_ == 1 or lengths[-1] < 30),
                  f"{name}: the stick fills up during a recording of {' + '.join('%.1f' % n for n in lengths)} s: "
                  f"the countdown appears 30 s before the end ({times[0] if times else 0:.2f} s into the run), "
                  f"counts 29, 28, ... 0 in step with the audio"
                  + (", straight through the change into the short last file" if files_ == 2 else "")
                  + f", and the recording ends when it is through (at {end if end else 0:.2f} s)")
    r, s = get("late_cut")
    if r:
        name = "late_cut"
        sound(r, name, s, leaks=0)
        values, times, end = countdown(r)
        t = r["takes"]
        lengths = [x["audio_bytes"] / RATE for x in t]
        hints = [(round(second, 1), cut) for second, state, _, cut in r["views"] if state == 2]
        low = [second for second, cut in hints if cut == 2]
        check(len(t) == 2 and r["splits"] == 1 and abs(lengths[0] - 29.75) <= 0.3
              and abs(sum(lengths) - 110) <= 0.3 and t[-1]["dropped"] == 0
              and values == list(range(29, -1, -1)) and end is not None and abs(end - 120) <= 0.6
              and [cut for _, cut in hints][-1] == 2 and low and abs(low[0] - 60) <= 0.5,
              f"{name}: the stick has room for one file only and the next one was looked for in "
              f"vain: CUT 30 s in is made all the same ({' + '.join('%.1f' % n for n in lengths)} s), the "
              f"second file is the rest of the first one's room; the menu says LOW SPACE from the moment "
              f"under a minute would be left ({low[0] if low else 0:.1f} s), the cut asked for at 75 s is "
              f"not made, and the countdown runs to the end ({end if end else 0:.2f} s): {hints}")
    r, s = get("count_room")
    if r:
        sound(r, "count_room", s)
        values, _, _ = countdown(r)
        check(not values and r["count_bad"] == 0 and r["switches"] >= 2,
              f"count_room: room for a next file every time ({r['switches']} changes of file): no "
              f"countdown at any moment, also not in the last 30 s of a file")
    odd = [n for n, r in ran if r["count_bad"] or (not n.startswith(("count_", "late_cut")) and r["counts"])]
    check(not odd, f"all {len(ran)} cases: the countdown bytes of every status are as listen.h says "
          f"(in range, only while recording, the bytes behind them zero), and no case built without a "
          f"countdown shows one" + (f": NOT {odd}" if odd else ""))

    for name in ("count_stick_full", "count_last_file"):
        r, s = get(name)
        if r:
            check(r["left_seen"] > 0 and r["left_count_diff"] <= 1,
                  f"{name}: while the countdown shows, the time left says the same to the second "
                  f"(at most {r['left_count_diff']} s apart)")
    steady = ("chain", "chain_stall", "chain_fragmented", "count_room", "file_full", "pack_music_chain",
              "stall_3s", "count_stick_full")
    got = {n: r for n, r in ran if n in steady}
    bad = [n for n, r in got.items() if r["left_seen"] == 0 or r["left_sum_max"] - r["left_sum_min"] > 2]
    check(len(got) == len(steady) and not bad,
          f"{len(got)} cases of one take without a pause: time left plus time recorded stays the same "
          f"within 2 s from start to end, through the changes of file and the stalls"
          + (f": NOT {bad}" if bad else ""))
    r, s = get("long_take")
    plain = dict(ran).get("chain")
    if r and plain:
        check(abs(r["left_ready"] - plain["left_ready"]) <= 1,
              f"long_take, a take 12 h 25 min in, where the counts of samples run over: the same time "
              f"left when ready as a fresh take ({r['left_ready']} and {plain['left_ready']} s)")
    print("time left, per case: " + ", ".join(
        f"{n} ready {r['left_ready'] if r['left_ready'] != 0xFFFFFFFF else '-'} s, "
        f"left + recorded {r['left_sum_min'] if r['left_sum_min'] != 0xFFFFFFFF else '-'}.."
        f"{r['left_sum_max']}" for n, r in ran if r["left_seen"]))

    r, s = get("chain_power_cut")
    if r:
        name = "chain_power_cut"
        sound(r, name, s)
        t = r["takes"]
        kept(r, name, s, 0, 1, full, kind=SWITCH)
        rec = s.recording(2)
        check(len(t) >= 3 and t[1]["kind"] == CUT and bool(rec) and rec["state"] == 2
              and rec["size"] == 512 + t[1]["header_bytes"] and rec["take"]["chain_index"] == 1
              and 0 < t[1]["audio_bytes"] - t[1]["header_bytes"] <= 15.5 * RATE,
              f"{name}: power lost 20 s into the second file: first file complete, second closed "
              f"at the next start at {t[1]['header_bytes'] / RATE:.1f} s")
        check(bool(s.recording(3)) and r["keeps"] >= 1,
              f"{name}: the music is recorded again after power-on")

    r, s = get("chain_power_cut_ready")
    if r:
        name = "chain_power_cut_ready"
        sound(r, name, s)
        t = r["takes"]
        rec = s.recording(1)
        check(t[0]["kind"] == CUT and bool(rec) and rec["state"] == 2
              and rec["size"] == 512 + t[0]["header_bytes"] and t[0]["header_bytes"] >= 60 * RATE,
              f"{name}: power lost in the first file after the next one was made: at the next "
              f"start REC0001.WAV is closed at {t[0]['header_bytes'] / RATE:.1f} s and its unused "
              f"part given back")
        second = s.recording(2)
        check(bool(second) and second["session"]["files_reused"] == 1
              and second["session"]["files_recovered"] == 1 and r["keeps"] >= 1,
              f"{name}: the waiting file made before the cut takes the next recording")

    for name in sorted((n for n in results if n.startswith("chain_cut_")), key=lambda n: int(n.split("_")[2])):
        r, s = get(name)
        bad = test_fat.harmful_after_cut(s.path)
        check(r["cuts"] == 1 and r["reboots"] == 1 and not bad and s.others_unchanged()
              and r["data_ok"] and r["bounds_ok"] and r["header_ok"] and r["keeps"] >= 1
              and r["fails"] == 0,
              f"{name}: power lost at bookkeeping write {int(name.split('_')[2]) + 1} of a take that "
              f"runs through files: nothing shares space, other files untouched, recorded again "
              f"after power-on ({r['keeps']} take(s) closed, {r['switches']} file changes); "
              f"leftovers: {sorted(set(p.split(':')[0] for p in s.problems)) or 'none'}"
              + (f" HARMFUL {bad}" if bad else ""))

    cut_names = sorted((n for n in results if n.startswith(("meta_cut_", "hand_cut_"))),
                       key=lambda n: (n.split("_")[0], int(n.split("_")[2])))
    for name in cut_names:
        r, s = get(name)
        bad = test_fat.harmful_after_cut(s.path)
        recordings = [n for n in s.files if n.startswith("REC")]
        check(r["cuts"] == 1 and r["reboots"] == 1 and not bad and s.others_unchanged()
              and r["data_ok"] and r["bounds_ok"] and r["keeps"] >= 1 and len(recordings) >= 1
              and r["fails"] <= (1 if name.startswith("hand_cut_") else 0),
              f"{name}: power lost at bookkeeping write {int(name.split('_')[2]) + 1}: nothing "
              f"shares space, other files untouched, and after power-on the recorder recorded "
              f"{r['keeps']} take(s); leftovers: "
              f"{sorted(set(p.split(':')[0] for p in s.problems)) or 'none'}")

    INIT, NO_USB, NOT_FAT32, FULL, ERROR, OTHER = 1, 2, 3, 4, 5, 6
    NOT_READY, READY, RECORDING, CLOSING = 0, 1, 2, 3

    def shown(r):
        out = []
        for _, state, reason, _ in r["views"]:
            if not out or out[-1] != (state, reason):
                out.append((state, reason))
        return out

    def when(r, state, reason, after=0.0):
        for second, s_, r_, _ in r["views"]:
            if second >= after and (s_, r_) == (state, reason):
                return second
        return None

    odd = [name for name, r in results.items()
           if r["view_lost"] or not r["views"] or r["views"][0][1:3] != (NOT_READY, INIT)
           or any((state != NOT_READY and reason) or (state == NOT_READY and not 1 <= reason <= 6)
                  or (state != RECORDING and cut) or cut > 2 for _, state, reason, cut in r["views"])]
    check(not odd, f"status for the menu, all {len(results)} cases: it begins with \"starting up\"; a "
          f"reason is given exactly while not ready, a cut hint only while recording"
          + (f": NOT in {odd}" if odd else ""))

    r, s = get("two_takes")
    if r:
        check(shown(r) == [(NOT_READY, INIT), (READY, 0), (RECORDING, 0), (CLOSING, 0), (READY, 0),
                           (RECORDING, 0), (CLOSING, 0), (READY, 0)],
              f"two_takes: starting up, ready, and for each take recording, closing, ready: "
              f"\"closing\" lasts until the recorder is ready again, nothing else shows in between")
        hints = [(round(second, 1), cut) for second, state, _, cut in r["views"] if state == 2]
        per_take = [[h for h in hints if lo <= h[0] < lo + 100] for lo in (10, 120)]
        check(all(t and [cut for _, cut in t] in ([0], [1, 0]) and t[-1][0] - t[0][0] <= 0.5 for t in per_take),
              f"two_takes: a cut has to wait at most for the first quarter second of a take, then it is "
              f"possible: {hints}")

    for name, reason, text in (("no_stick", NO_USB, "no stick"), ("fat16", NOT_FAT32, "not FAT32"),
                               ("geometry_lie", NOT_FAT32, "not FAT32"), ("no_space", FULL, "no room")):
        r, s = get(name)
        if r:
            check(shown(r) == [(NOT_READY, INIT), (NOT_READY, reason)] and r["views"][-1][0] < 7,
                  f"{name}: the menu is told \"{text}\" {r['views'][-1][0]:.2f} s after the "
                  f"recorder's start, and that stays")

    r, s = get("stick_late")
    if r:
        seen, ready = when(r, NOT_READY, INIT, 20), when(r, READY, 0, 20)
        check(shown(r)[:4] == [(NOT_READY, INIT), (NOT_READY, NO_USB), (NOT_READY, INIT), (READY, 0)]
              and seen is not None and seen <= 21.3 and ready is not None and ready <= 23.5,
              f"stick_late: no stick, then a stick at 20 s: \"looking at a stick\" from {seen} s, "
              f"ready at {ready} s")

    r, s = get("ready_stick_out")
    if r:
        name = "ready_stick_out"
        sound(r, name, s)
        gone, back, ready = when(r, NOT_READY, NO_USB, 20), when(r, NOT_READY, INIT, 40), when(r, READY, 0, 40)
        check(shown(r) == [(NOT_READY, INIT), (READY, 0), (NOT_READY, NO_USB), (NOT_READY, INIT), (READY, 0)]
              and gone is not None and gone <= 21.5 and back is not None and back <= 41.3
              and ready is not None and ready <= 43.5,
              f"{name}: stick pulled at 20 s while ready: \"no stick\" at {gone} s; put back at "
              f"40 s: looked at from {back} s, ready at {ready} s")
        check(r["starts"] == 0 and r["fails"] == 0 and r["aborts"] == 0 and r["arms"] == 2
              and (r["status"] & ~analyze_wav.INFORMATIONAL) == 0 and not [n for n in s.files if n.startswith("REC")],
              f"{name}: no take, no failure, no flag raised, no recording on the stick; armed twice")

    r, s = get("stick_pulled")
    if r:
        error, gone = when(r, NOT_READY, ERROR, 40), when(r, NOT_READY, NO_USB, 40)
        check(gone is not None and error is not None and error <= 40.6 and 1.9 <= gone - error <= 3.1
              and when(r, NOT_READY, INIT, 60) is not None and when(r, RECORDING, 0, 60) is not None,
              f"stick_pulled: stick pulled at 40 s during a take: "
              + (f"an error at {error} s, " if error is not None else "")
              + f"\"no stick\" at {gone} s; put back at 60 s: looked at, and the music is recorded again")

    r, s = get("write_fail")
    if r:
        check((NOT_READY, ERROR) in shown(r) and shown(r)[-1] == (NOT_READY, ERROR),
              f"write_fail: writes that fail show as an error after each take, and for good after "
              f"the third: {shown(r)[2:]}")

    r, s = get("map_corrupt")
    if r:
        check(shown(r)[-1] == (NOT_READY, OTHER),
              f"map_corrupt: memory that changed: not ready, without a stick's fault being named")

    r, s = get("chain_stick_full")
    if r:
        check(shown(r)[-2:] == [(CLOSING, 0), (NOT_READY, FULL)] and r["views"][-3][1:] == (RECORDING, 0, 2),
              f"chain_stick_full: no room for another file: the cut hint says \"too little room\", "
              f"and after the take the menu is told \"no room\"")

    r, s = get("cut_refused")
    if r:
        hints = [(round(second), cut) for second, state, _, cut in r["views"] if state == RECORDING]
        check([cut for _, cut in hints] in ([1, 0, 2], [0, 2]),
              f"cut_refused: the cut hint turns to \"too little room\" once the rest would be "
              f"smaller than the smallest file: {hints}")

    r, s = get("chain")
    if r:
        hints = [(round(second), cut) for second, state, _, cut in r["views"] if state == RECORDING]
        waits = [b[0] - a[0] for a, b in zip(hints, hints[1:]) if a[1] == 1]
        check(0 in [cut for _, cut in hints][:2] and all(w <= 1 for w in waits)
              and all(cut != 2 for _, cut in hints),
              f"chain: files of 90 s, the next one made 30 s ahead: a cut stays possible while the "
              f"next file waits: {hints}")

    HEAD = "DJM RECORDER LOG\r\n"
    REPAIRED = "REC%04d.WAV NOT CLOSED (POWER OR USB LOST), REPAIRED\r\n"

    def free_clusters(stick):
        v = stick.volume
        return sum(1 for c in range(2, v.end) if v.fat[c] == 0)

    r, s = get("cut_next_ready")
    if r:
        name = "cut_next_ready"
        sound(r, name, s)
        t = r["takes"]
        lengths = [x["audio_bytes"] / RATE for x in t]
        full = (cap(90) // CLUSTER * CLUSTER - 512 - CUE_RESERVE) // PAGE * PAGE / RATE
        if len(t) == 3:
            kept(r, name, s, 0, 1, lengths[0], kind=SPLIT, exact=False)
            kept(r, name, s, 1, 2, full, kind=SWITCH, before=lengths[0], index=1)
            kept(r, name, s, 2, 3, 220 - lengths[0] - full, before=lengths[0] + full, index=2)
        hints = [cut for second, state, _, cut in r["views"] if state == 2 and second > 11]
        check(len(t) == 3 and r["splits"] == 1 and r["switches"] == 1 and r["status"] == 0 and r["fails"] == 0
              and 74.7 < lengths[0] <= 75.15 and t[0]["audio_bytes"] % PAGE == 0
              and abs(sum(lengths) - 220) < 1e-9 and t[2]["stored"] == blocks(220) and not any(hints),
              f"{name}: a cut 75 s into a file of 90 s, when the next file exists already: the "
              f"file is closed there, its rest given back, and the take goes on in the waiting file "
              f"and through the next change of file: {[round(x, 2) for x in lengths]} s, every block "
              f"of the take in order; the menu never had to say WAIT")
        waiting_ok(name, s)

    r, s = get("start_power_5")
    if r:
        name = "start_power_5"
        sound(r, name, s)
        t = r["takes"]
        check(len(t) == 2 and t[0]["kind"] == CUT and t[0]["header_bytes"] == 0 and r["reboots"] == 1
              and r["closed_headers"] == 0 and not s.recording(2) and s.text() is None
              and t[1]["first_sector"] == t[0]["first_sector"] and r["fails"] == 0,
              f"{name}: power lost 5 s after START (the file had its name, its header no audio yet): "
              f"at the next start it is the waiting file again, in the same place; no empty "
              f"recording stays, and nothing is written to an errors file")
        if len(t) == 2:
            kept(r, name, s, 1, 1, t[1]["audio_bytes"] / RATE, exact=False, reason=4)
            check(abs(t[1]["audio_bytes"] / RATE - 40) < 0.05,
                  f"{name}: the next recording gets the number that became free, REC0001.WAV, 40 s")
        waiting_ok(name, s)

    r, s = get("start_power_40")
    if r:
        name = "start_power_40"
        sound(r, name, s)
        t = r["takes"]
        rec = s.recording(1)
        check(len(t) == 1 and t[0]["kind"] == CUT and bool(rec) and rec["state"] == 2
              and rec["size"] == 512 + t[0]["header_bytes"] and t[0]["header_bytes"] >= 25 * RATE
              and r["closed_headers"] == 1 and rec["take"]["number"] == 1,
              f"{name}: power lost 40 s after START: REC0001.WAV is kept with the "
              f"{t[0]['header_bytes'] / RATE:.1f} s its last header covered (a recording "
              f"under one minute old was lost whole: it had no name yet)")
        check(s.text() == HEAD + REPAIRED % 1,
              f"{name}: the errors file REC-LOG.TXT is made and says so: {s.text()!r}")
        waiting_ok(name, s)

    r, s = get("start_pull_5")
    if r:
        name = "start_pull_5"
        sound(r, name, s)
        first = s.recording(1)
        stopped = [second for second, state, reason, _ in r["views"] if state == NOT_READY and reason == NO_USB]
        check(r["fails"] == 1 and r["keeps"] == 1 and not s.recording(2) and bool(first)
              and first.get("state") == 2 and abs(first["data_bytes"] / RATE - 20) < 0.05
              and s.text() == HEAD + "RECORDING STOPPED, USB ERROR\r\n" and r["status_flags"] & 8
              and stopped and 26.9 <= stopped[0] <= 28.5,
              f"{name}: the stick pulled 5 s after START and put back: no empty file stays, the "
              f"next recording is REC0001.WAV (20 s), and the errors file has the line without a "
              f"file name (that file is gone): {s.text()!r}; the menu said NO USB at {stopped[:1]} s")

    r, s = get("start_pull_40")
    if r:
        name = "start_pull_40"
        sound(r, name, s)
        first, second = s.recording(1), s.recording(2)
        check(r["fails"] == 1 and r["keeps"] == 1 and bool(first) and bool(second)
              and first.get("state") == 2 and first["data_bytes"] >= 25 * RATE
              and first["size"] == 512 + first["data_bytes"]
              and abs(second["data_bytes"] / RATE - 20) < 0.05
              and s.text() == HEAD + "REC0001.WAV STOPPED, USB ERROR\r\n" + REPAIRED % 1,
              f"{name}: the stick pulled 40 s after START: when it is back REC0001.WAV is closed "
              f"with the {first.get('data_bytes', 0) / RATE:.1f} s its last header covered, the "
              f"next recording is REC0002.WAV, and the errors file tells both: {s.text()!r}")

    r, s = get("cut_power_5")
    if r:
        name = "cut_power_5"
        sound(r, name, s)
        t = r["takes"]
        if len(t) == 3:
            kept(r, name, s, 0, 1, t[0]["audio_bytes"] / RATE, kind=SPLIT, exact=False)
            kept(r, name, s, 2, 2, t[2]["audio_bytes"] / RATE, exact=False, reason=4)
        check(len(t) == 3 and t[1]["kind"] == CUT and t[1]["header_bytes"] == 0
              and t[2]["first_sector"] == t[1]["first_sector"] and not s.recording(3)
              and s.text() is None and r["fails"] == 0 and abs(t[2]["audio_bytes"] / RATE - 30) < 0.05,
              f"{name}: power lost 5 s after a cut: the first file is complete, the second (named, "
              f"no audio in its header) is the waiting file again, and the next recording is "
              f"REC0002.WAV")
        waiting_ok(name, s)

    r, s = get("mark_power")
    if r:
        name = "mark_power"
        sound(r, name, s)
        t = r["takes"]
        rec = s.recording(1)
        points, labels, good = s.tail(1)
        wanted = [10, 25.5, 50, 65]
        check(len(t) == 1 and bool(rec) and rec["state"] == 2 and r["recover_tails"] == 1
              and rec["size"] == 512 + t[0]["header_bytes"] + 12 + 24 * 4 and good and not labels
              and len(points) == 4 and all(abs(a - b) < 0.02 for a, b in zip(points, wanted))
              and t[0]["header_bytes"] / RATE > 65.1 and r["sheet_writes"] == 4,
              f"{name}: power lost 70 s into a recording with five markers: at the next start the "
              f"four that were saved with the header updates are behind the "
              f"{t[0]['header_bytes'] / RATE:.1f} s of audio that are kept, at {points} s; the one "
              f"set 0.4 s before the cut is lost with that audio")
        check(s.text() == HEAD + REPAIRED % 1, f"{name}: and the errors file says: {s.text()!r}")

    def lost_lines(text, number):
        places, rest = [], []
        for line in (text or "").split("\r\n"):
            m = re.fullmatch(r"REC%04d\.WAV (?:(\d+):)?(\d\d):(\d\d) LOST (\d+\.\d\d) S" % number, line)
            if m:
                places.append((int(m.group(1) or 0) * 3600 + int(m.group(2)) * 60 + int(m.group(3)),
                               float(m.group(4))))
            elif line:
                rest.append(line)
        return places, rest

    def coarse_lines(text, number):
        out = []
        for line in (text or "").split("\r\n"):
            m = re.fullmatch(r"REC%04d\.WAV (?:(\d+):)?(\d\d):(\d\d) 16 BIT (\d+\.\d\d) S" % number, line)
            if m:
                out.append((int(m.group(1) or 0) * 3600 + int(m.group(2)) * 60 + int(m.group(3)), float(m.group(4))))
        return out

    r, s = get("lost_many")
    if r:
        name = "lost_many"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 300, dropped=(18, 18 * 2500, 18 * 3800), flags=ST["AUDIO_DROPPED"],
             cue=1, lost=16, coarse=8)
        points, labels, good = s.tail(1)
        named = [i for i, text_ in labels if text_ == "LOST"]
        stretched = [i for i, text_ in labels if text_ == "16BIT"]
        own = [p for i, p in enumerate(points, 1) if i not in named and i not in stretched]
        places, rest = lost_lines(s.text(), 1)
        stretches = coarse_lines(s.text(), 1)
        rest = [x for x in rest if " 16 BIT " not in x or "MORE" in x]
        more = re.fullmatch(r"REC0001\.WAV LOST 2 MORE TIMES, (\d+\.\d\d) S", rest[1]) if len(rest) == 3 else None
        more16 = re.fullmatch(r"REC0001\.WAV 16 BIT 10 MORE TIMES, (\d+\.\d\d) S", rest[2]) if len(rest) == 3 else None
        dropped = r["takes"][0]["dropped"] / 4000
        check(good and len(points) == 25 and len(named) == 16 and len(stretched) == 8 and len(labels) == 24
              and points == sorted(points) and len(own) == 1 and 25 < own[0] < 30,
              f"{name}: eighteen stalls of the stick in one take: sixteen places where audio was "
              f"lost have a marker named LOST and eight stretches one named 16BIT (the allowances), in "
              f"the order of time among the one marker ({own} s), which is not named")
        check(len(places) == 16 and rest[:1] == [HEAD.strip()] and more is not None
              and [p for p, _ in places] == [int(points[i - 1]) for i in named]
              and abs(sum(x for _, x in places) + float(more.group(1)) - dropped) < 0.2
              and r["status_flags"] & 4,
              f"{name}: the errors file has a line for each of the sixteen, with the place and how "
              f"much is missing, and one for the two further places; together "
              f"{sum(x for _, x in places) + (float(more.group(1)) if more else 0):.2f} s of the "
              f"{dropped:.2f} s that were lost; the menu was told (the ! before the time)")
        in16 = r["takes"][0]["coarse_blocks"] / 4000
        check(len(stretches) == 8 and more16 is not None
              and [p for p, _ in stretches] == [int(points[i - 1]) for i in stretched]
              and abs(sum(x for _, x in stretches) + float(more16.group(1)) - in16) < 0.3,
              f"{name}: ... and a line for each of the eight stretches stored with 16 bits, with the "
              f"place and the length, and one for the ten further ones; together "
              f"{sum(x for _, x in stretches) + (float(more16.group(1)) if more16 else 0):.2f} s of the "
              f"{in16:.2f} s the file holds in that form")

    r, s = get("stall_5s5")
    if r:
        points, labels, good = s.tail(1)
        places, rest = lost_lines(s.text(), 1)
        stretches = coarse_lines(s.text(), 1)
        lost_at = [points[i - 1] for i, text_ in labels if text_ == "LOST"]
        from16 = [points[i - 1] for i, text_ in labels if text_ == "16BIT"]
        check(good and len(points) == 4 and len(lost_at) == 2 and len(from16) == 2 and len(places) == 2
              and len(rest) == 3 and rest[0] == HEAD.strip() and [p for p, _ in places] == [int(x) for x in lost_at]
              and [p for p, _ in stretches] == [int(x) for x in from16]
              and all(a < b for a, b in zip(from16, lost_at)),
              f"stall_5s5: two stalls longer than the ring holds: each first a stretch stored with 16 bits "
              f"(markers named 16BIT at {[round(x, 2) for x in from16]} s), then audio lost (markers named "
              f"LOST at {[round(x, 2) for x in lost_at]} s); the errors file has the four lines: "
              f"lost {places}, 16 bit {stretches}")

    FIXED = "DJMREC.TMP CHANGED BY A COMPUTER, REPAIRED\r\n"
    r, s = get("touched_out")
    if r:
        name = "touched_out"
        sound(r, name, s)
        first = s.recording(1)
        check(r["keeps"] == 1 and r["aborts"] == 0 and r["fails"] == 0 and bool(first)
              and first.get("state") == 2 and abs(first["data_bytes"] / RATE - 60) < 0.05
              and not r["status"] & ST["REVALIDATION"] and s.text() == HEAD + FIXED,
              f"{name}: the stick back from a computer that changed the time stamp of DJMREC.TMP: "
              f"the look at the stick writes its entry back, START records (REC0001.WAV, "
              f"{first.get('data_bytes', 0) / RATE:.1f} s), and REC-LOG.TXT says so: {s.text()!r} "
              f"(2.74 refused every start here)")
        waiting_ok(name, s)

    r, s = get("touched_armed")
    if r:
        name = "touched_armed"
        sound(r, name, s)
        first = s.recording(1)
        error, ready = when(r, NOT_READY, ERROR, 40), when(r, READY, 0, 40.5)
        check(r["keeps"] == 1 and r["aborts"] == 1 and r["fails"] == 0 and bool(first)
              and first.get("state") == 2 and abs(first["data_bytes"] / RATE - 40) < 0.05
              and r["status"] & ST["REVALIDATION"]
              and s.text() == HEAD + "RECORDING START REFUSED, WAITING FILE CHANGED\r\n" + FIXED
              and error is not None and error < 40.6 and ready is not None and 2.0 <= ready - error <= 4.0,
              f"{name}: DJMREC.TMP changed while armed: START at 40 s is refused with a line in "
              f"REC-LOG.TXT and USB ERROR from {error} s, ready again at {ready} s with the entry "
              f"written back; START at 60 s records 40 s: {s.text()!r}")
        waiting_ok(name, s)

    r, s = get("coarse_many_chain")
    if r:
        name = "coarse_many_chain"
        sound(r, name, s)
        t = r["takes"]
        text_ = s.text() or ""
        sums, more_lines = [], []
        for i in range(len(t)):
            listed = coarse_lines(text_, i + 1)
            more = re.findall(r"REC%04d\.WAV 16 BIT (\d+) MORE TIMES, (\d+\.\d\d) S" % (i + 1), text_)
            more_lines.append([(int(a), float(b)) for a, b in more])
            sums.append(sum(x for _, x in listed) + sum(float(b) for _, b in more))
        files = [x["coarse_blocks"] / 4000 for x in t]
        spans = [i for i in range(1, len(t)) if t[i]["coarse_blocks"] and t[i]["coarse_first"] == 0]
        check(len(t) >= 2 and r["takes"][-1]["dropped"] == 0 and "0 MORE TIMES" not in text_
              and all(len(m) <= 1 and all(n >= 1 for n, _ in m) for m in more_lines)
              and all(abs(a - b) < 0.3 for a, b in zip(sums, files))
              and len(coarse_lines(text_, 1)) == 8 and more_lines[0] and spans,
              f"{name}: more than eight stretches in a file of 90 s, one still under way at a "
              f"change of file (into file {[i + 1 for i in spans]}): every file's 16 BIT lines add "
              f"up to what it holds in that form ({[round(x, 2) for x in sums]} s against "
              f"{[round(x, 2) for x in files]} s), and no line says 0 MORE TIMES (audit of 2.73); "
              f"lines for those not listed, by file: {more_lines}")

    r, s = get("coarse_cut")
    if r:
        name = "coarse_cut"
        sound(r, name, s)
        t = r["takes"]
        lengths = [x["audio_bytes"] / RATE for x in t]
        if len(t) == 2:
            kept(r, name, s, 0, 1, lengths[0], kind=SPLIT, exact=False, coarse=t[0]["cue_coarse"])
            kept(r, name, s, 1, 2, lengths[1], exact=False, before=lengths[0], index=1, coarse=t[1]["cue_coarse"])
        check(len(t) == 2 and r["status"] == ST["STORED_16_BIT_IN_FREEZES"] and abs(sum(lengths) - 220) < 0.01
              and t[-1]["dropped"] == 0 and t[0]["cue_coarse"] == 1
              and (t[1]["cue_coarse"] == 1) == (t[1]["coarse_blocks"] > 0)
              and (t[1]["coarse_blocks"] == 0 or t[1]["coarse_first"] == 0)
              and t[0]["coarse_blocks"] + t[1]["coarse_blocks"] == r["coarse_stored"] > 4000,
              f"{name}: a stall of 3 s with a cut asked for in the middle of it: the take is in two files of "
              f"{lengths[0] if t else 0:.2f} and {lengths[1] if len(t) > 1 else 0:.2f} s without a gap, nothing "
              f"lost; the {r['coarse_stored'] / 4000:.2f} s stored with 16 bits are all in the files, "
              f"{[x['coarse_blocks'] for x in t]} blocks, with their markers")
    r, s = get("coarse_power")
    if r:
        name = "coarse_power"
        sound(r, name, s, clean=False)
        rec = s.recording(1)
        points, labels, good = s.tail(1)
        check(r["reboots"] == 1 and bool(rec) and rec.get("state") == 2 and good and len(points) == 1
              and [text_ for _, text_ in labels] == ["16BIT"] and 12.3 < points[0] < 13.0
              and rec["flags"] & ST["STORED_16_BIT_IN_FREEZES"],
              f"{name}: a stall of 3 s 10 s into a recording, power lost a minute later: at the next start "
              f"the file is closed and the marker of the 16-bit stretch, saved with the header updates, is "
              f"behind its audio, named 16BIT, at {points[0] if points else 0:.2f} s")

    r, s = get("long_take")
    if r:
        name = "long_take"
        sound(r, name, s)
        t = r["takes"]
        first, second = s.recording(1), s.recording(2)
        ok = len(t) == 2 and bool(first) and bool(second) and "take" in first and "take" in second
        lengths = [x["audio_bytes"] / RATE for x in t]
        tails = [s.tail(1), s.tail(2)] if ok else [([], [], False)] * 2
        wrap_at = ((1 << 32) - LONG_START * 24) / 96000
        check(ok and abs(lengths[0] - 40) < 0.2 and abs(sum(lengths) - 70) < 0.05 and t[1]["dropped"] == 0
              and first["take"]["blocks_before"] == LONG_START
              and second["take"]["blocks_before"] == LONG_START + t[0]["audio_bytes"] // BLOCK
              and all(x[2] for x in tails) and not tails[0][1] and not tails[1][1]
              and len(tails[0][0]) == 2 and abs(tails[0][0][0] - 10) < 0.02 and abs(tails[0][0][1] - 30) < 0.02
              and len(tails[1][0]) == 1 and abs(tails[1][0][0] - (55 - lengths[0])) < 0.02
              and r["status_seconds"] <= 41 and r["status_file2_most"] <= 31 and r["status_file2_first"] <= 1,
              f"{name}: a file that begins 12 h 25 min 16 s into its take; the counts of samples run over "
              f"{wrap_at:.0f} s later. Markers 10 s and 30 s in (before and after that moment), a cut at "
              f"{lengths[0] if t else 0:.2f} s, a marker in the second file: all at their places "
              f"({tails[0][0]} and {tails[1][0]} s), both files whole, and the time for the display never "
              f"above the file's own ({r['status_seconds']} s at most, starting anew after the cut)")
    r, s = get("refill_no_room")
    if r:
        name = "refill_no_room"
        sound(r, name, s, leaks=0)
        t = r["takes"]
        session = (s.recording(2) or s.recording(1) or {}).get("session", {})
        check(len(t) >= 2 and r["fails"] == 0 and abs(t[0]["audio_bytes"] / RATE - 90) < 0.01
              and t[0]["dropped"] == 0 and session.get("files_refilled") == 0,
              f"{name}: a waiting file smaller than a full file on a stick with no more room: the "
              f"recorder looks for more every time it prepares, finds none and goes on with what there is: "
              f"takes of {[round(x['audio_bytes'] / RATE, 1) for x in t]} s, nothing topped up, no error")

    r, s = get("reserve")
    if r:
        name = "reserve"
        sound(r, name, s)
        kept(r, name, s, 0, 1, 90)
        w = s.waiting()
        was_free = sum(1 for c in range(2, s.before.end) if s.before.fat[c] == 0)
        entry = s.files.get("REC-LOG TXT")
        log = len(s.volume.chain(entry[1])[0]) if entry else 0
        check(free_clusters(s) == (5 << 20) // CLUSTER - log and log == 1 and w is not None and was_free == 1999
              and w["clusters"] == was_free - (5 << 20) // CLUSTER - s.recording(1)["size"] // CLUSTER - 1
              and s.text(raw=True) == HEAD_LOG + "REC0001.WAV 24 BIT\r\n",
              f"{name}: {was_free} clusters free and a reserve of 5 MB: the recorder's files take all "
              f"but {free_clusters(s)} clusters, the reserve less the cluster of the errors file ("
              f"it gets the recording's line; the reserve is what it is kept for), also after a take")

    r, s = get("reserve_too_small")
    if r:
        name = "reserve_too_small"
        check(r["meta_writes"] == 0 and r["arms"] == 0 and status_has(r, "NO_SPACE")
              and shown(r)[-1] == (NOT_READY, FULL) and free_clusters(s) == 1999,
              f"{name}: what is free beyond the reserve is smaller than the smallest file: nothing "
              f"is written and the menu is told \"no room\"")

    line = REPAIRED % 1
    for tag in TEXT_LAYOUTS:
        name = f"log_{tag}"
        r, s = get(name)
        if not r:
            continue
        sound(r, name, s)
        old = s.text(before=True)
        files_before = s.before.root_files()
        same = all(s.files.get(n) == e and s.text(n) == s.text(n, before=True)
                   for n, e in files_before.items() if n.startswith("REC-L"))
        if tag == "new":
            ok = s.text() == HEAD + line
            what = "no errors file on the stick: REC-LOG.TXT is made, the heading and the line"
        elif tag == "ours":
            ok = s.text() == old + line and old.startswith(HEAD)
            what = "the recorder's own REC-LOG.TXT is there: the line is added at its end, nothing else changes"
        elif tag == "full_cluster":
            entry = s.files.get("REC-LOG TXT")
            ok = s.text() == old + line and len(old) == CLUSTER and \
                len(s.volume.chain(entry[1])[0]) == 2 and \
                s.text(raw=True) == old + line + "REC0002.WAV 24 BIT\r\n" and entry[2] == len(s.text(raw=True))
            what = "its REC-LOG.TXT fills its cluster to the last byte: a second cluster is linked on and takes the line"
        elif tag == "many_foreign":
            ok = same and s.text("REC-LG10TXT") == HEAD + line
            what = "REC-LOG.TXT and REC-LOG2.TXT to REC-LOG9.TXT are somebody else's: all left alone, REC-LG10.TXT is made"
        else:
            ok = same and s.text("REC-LOG2TXT") == HEAD + line
            what = {"foreign": "a REC-LOG.TXT that starts differently", "readonly": "the recorder's file, read-only",
                    "binary": "the recorder's heading followed by something that is not plain text",
                    "empty": "an empty REC-LOG.TXT"}[tag] + ": left exactly as it is, REC-LOG2.TXT is made"
        check(ok and r["text_writes"] >= 1 and r["keeps"] == 1 and s.recording(1).get("state") == 2,
              f"{name}: {what}" + ("" if ok else f" GOT {s.text()!r} / {s.text('REC-LOG2TXT')!r}"))

    r, s = get("short_build")
    if r:
        name = "short_build"
        sound(r, name, s)
        t = r["takes"]
        lengths = [round(x["audio_bytes"] / RATE, 2) for x in t]
        kinds = [x["kind"] for x in t]
        recs = [s.recording(i) for i in range(1, 6)]
        check(kinds == [SPLIT, KEEP, SWITCH, KEEP, KEEP] and all(rec.get("state") == 2 for rec in recs)
              and not s.recording(6) and r["status"] == 0 and r["fails"] == 0
              and abs(lengths[0] - 70) < 0.2 and abs(lengths[0] + lengths[1] - 110) < 0.05
              and lengths[2] == 90.0 and abs(lengths[2] + lengths[3] - 120) < 0.05
              and abs(lengths[4] - 75) < 0.05
              and all(rec["capture"]["dropped_blocks"] == 0 for rec in recs),
              f"{name}: the test variant with files of 90 s, driven from the menu: a cut in the last "
              f"30 s of a file, a change of file, and a stop in the last 30 s: five files of "
              f"{lengths} s, nothing dropped, the volume clean")
        waiting_ok(name, s)

    ends = [n for n in results if n.startswith(("meta_cut_", "hand_cut_", "chain_cut_"))]
    in_log = [n for n in ends if results[n]["cut_phase"] == PHASES["LOG"]]
    odd = {n: [p for p in sticks[n].problems if "shares" in p or
               ("orphans" in p and not (n in in_log and p.startswith("orphans: 1 clusters")))] for n in ends}
    odd = {n: p for n, p in odd.items() if p}
    check(len(ends) >= 35 and not odd,
          f"power lost at the writes of making a file, naming, ending a take and cutting "
          f"({len(ends)} cases): in the end no space is without an owner and none is shared (up to "
          f"a cut between the end mark and the entry of the rest left the rest unowned); "
          f"{len(in_log)} of the cuts hit the errors file (at most one cluster without an owner "
          f"allowed there)" + (f": NOT {odd}" if odd else ""))

    odd = {n: r["dead_calls"] for n, r in results.items() if r["dead_calls"]}
    check(not odd, f"all {len(results)} cases: no transfer is ever asked for while the mixer "
          f"says that no stick is mounted (a failed transfer was followed by more: the "
          f"log, the next audio page)" + (f": NOT {odd}" if odd else ""))
    pulls = sorted(n for n in results if n.startswith("prepare_pull_"))
    seen = []
    for name in pulls:
        r, s = get(name)
        sound(r, name, s, clean=False)
        left = [p for p in s.problems if "free count says" not in p]
        back = when(r, READY, 0, 20)
        made = s.recording(1)["session"]
        seen.append((made["files_made"], made["files_reused"], made["files_refilled"],
                     made["direct_read_errors"] + made["direct_write_errors"]))
        check(r["keeps"] == 1 and not left and s.fsck_clean and back is not None and back <= 23.5
              and when(r, NOT_READY, NO_USB, 5) is not None,
              f"{name}: the stick pulled while it is being prepared and put back at 20 s: \"no "
              f"stick\", then ready again at {back} s, the music is recorded, nothing unowned on "
              f"the stick (waiting files made {seen[-1][0]}, found {seen[-1][1]}, made longer "
              f"{seen[-1][2]})")
    check(len(pulls) == 5 and len(set(seen)) >= 2,
          f"prepare_pull: the five pulls hit different moments of the preparation: {seen}")

    made = [n for n in results if n.startswith("meta_cut_") and int(n.split("_")[2]) <= 5 + MAKE]
    odd = {n: sticks[n].problems for n in made if any("orphans" in p or "table copies" in p for p in sticks[n].problems)}
    check(len(made) == 6 + MAKE and not odd,
          f"power lost at each of the eight writes that make a waiting file (directory entry, "
          f"table entries in two copies, free count): at the next start the recorder goes on with "
          f"what is linked or makes the file anew; no space is left that no file owns, and the "
          f"table copies agree" + (f": NOT {odd}" if odd else ""))
    for name, what in (("next_fail_6", "to the first table copy: nothing of the file is linked, its "
                                       "entry is taken away again and a waiting file made anew"),
                       ("next_fail_7", "to the second table copy: the copies are made to agree and the "
                                       "file is used")):
        r, s = get(name)
        if r:
            sound(r, name, s, clean=False)
            waiting_names = [n for n in s.files if n.startswith("DJMREC")]
            left = [p for p in s.problems if "free count says" not in p]
            first = s.recording(1)
            check(r["keeps"] >= 2 and waiting_names == ["DJMREC  TMP"] and not left and s.fsck_clean
                  and r["takes"][0]["kind"] == KEEP and first["capture"]["end_reason"] == 1
                  and (s.text() or "").startswith(HEAD + "REC0001.WAV STOPPED, USB ERROR\r\n"),
                  f"{name}: a write fails while the next file is made during a take, {what}; the "
                  f"take ends when its file is full (the errors file says why: a failed write, not a "
                  f"full stick) and the music is recorded again ({r['keeps']} takes); one waiting "
                  f"file, nothing unowned, only the free-count note is {'stale' if s.problems else 'right'}")
            waiting_ok(name, s)

    families = ("ready_cut_", "early_cut_", "start_cut_", "reuse_cut_", "mark_cut_", "grow_cut_", "append_cut_",
                "late_cut_")
    for name in sorted((n for n in results if n.startswith(families)),
                       key=lambda n: (n.split("_")[0], int(n.split("_")[2]))):
        r, s = get(name)
        family = name.split("_")[0]
        bad = test_fat.harmful_after_cut(s.path)
        recs = sorted(n for n in s.files if n.startswith("REC") and n.endswith("WAV"))
        first = s.recording(1)
        if family in ("ready", "early", "late"):
            ok = r["keeps"] >= 1 and bool(first) and first.get("state") == 2 and r["fails"] <= 1 \
                and not any("orphans" in p or "shares" in p for p in s.problems)
            what = "the take went on, or was recorded again after power-on; no space without an owner"
        elif family == "start":
            ok = recs == ["REC0001 WAV"] and first.get("state") == 2 and abs(first["data_bytes"] / RATE - 40) < 0.05
            what = "one recording in the end, REC0001.WAV, the 40 s made after power-on"
        elif family == "reuse":
            second = s.recording(2)
            ok = recs == ["REC0001 WAV", "REC0002 WAV"] and first.get("state") == 2 and second.get("state") == 2 \
                and abs(second["data_bytes"] / RATE - 30) < 0.05
            what = "the first file of the cut take and the 30 s made after power-on, nothing else"
        elif family == "mark":
            points, labels, good = s.tail(1)
            ok = bool(first) and first.get("state") == 2 and good and len(points) == 4 and \
                all(abs(a - b) < 0.02 for a, b in zip(points, [10, 25.5, 50, 65])) and \
                s.text() in (None, HEAD + REPAIRED % 1)
            what = f"REC0001.WAV is closed with its four saved markers ({points} s); errors file: {s.text()!r}"
        else:
            old = s.text(before=True)
            ok = s.text() in (old, old + REPAIRED % 1) and r["keeps"] == 1
            what = "the errors file is as it was" + ("" if s.text() == old else " plus the line")
        check(r["cuts"] == 1 and r["reboots"] >= 1 and not bad and s.others_unchanged() and ok
              and r["data_ok"] and r["bounds_ok"] and r["header_ok"] and r["cue_ok"],
              f"{name}: power lost at bookkeeping write {int(name.split('_')[2]) + 1}: nothing shares "
              f"space, other files untouched; {what}; leftovers: "
              f"{sorted(set(p.split(':')[0] for p in s.problems)) or 'none'}"
              + (f" HARMFUL {bad}" if bad else ""))

    bad = [n for n, r in ran if r["space_bad"] or (r["left_seen"] and not r["space_seen"])]
    check(not bad, f"all {len(ran)} cases: SPACE (status bytes 9-10, ) came with every time left "
          f"and agrees with it at the depth in force (192,000 or 288,000 bytes a second)"
          + (f"; NOT in {bad[:6]}" if bad else ""))
    deep = [n for n, r in ran if n.startswith("depth") and r["status_flags"] & 0x20 and
            n in ("depth16", "depth16_chain", "depth16_stall")]
    check(not deep, "the 16-bit cases never said 24 bit (status byte 4 bit 5)" + (f": NOT {deep}" if deep else ""))
    D16, D24 = "REC%04d.WAV 16 BIT\r\n", "REC%04d.WAV 24 BIT\r\n"
    r, s = get("depth16")
    if r:
        name = "depth16"
        sound(r, name, s)
        check(r["keeps"] == 1 and r["fails"] == 0 and r["coarse_stored"] == 0 and r["space_seen"] > 0,
              f"{name}: one take at 16 bit, the default; no 16-bit stretches counted (all of it is)")
        kept(r, name, s, 0, 1, 90, width=2)
        check(s.text(raw=True) == HEAD_LOG + D16 % 1,
              f"{name}: REC-LOG.TXT says REC0001.WAV 16 BIT: {s.text(raw=True)!r}")
        check(r["space_first"] & 0x8000 == 0 and 9 <= r["space_first"] <= 11,
              f"{name}: SPACE when first ready: about 1 GB of the simulated stick, in tenths "
              f"({r['space_first']})")
        waiting_ok(name, s)
    r, s = get("depth16_chain")
    if r:
        name = "depth16_chain"
        sound(r, name, s)
        full16 = (cap(90) // CLUSTER * CLUSTER - 512 - CUE_RESERVE) // PAGE * PAGE / 192_000
        check(r["switches"] == 2 and r["keeps"] == 1 and len(r["takes"]) == 3 and r["fails"] == 0,
              f"{name}: one take of 300 s at 16 bit through three files of {full16:.2f} s")
        kept(r, name, s, 0, 1, full16, kind=SWITCH, width=2)
        kept(r, name, s, 1, 2, full16, kind=SWITCH, before=full16, index=1, width=2)
        kept(r, name, s, 2, 3, 300 - 2 * full16, before=2 * full16, index=2, width=2)
        check(s.text(raw=True) == HEAD_LOG + D16 % 1,
              f"{name}: one line for the take, in the file it began in: {s.text(raw=True)!r}")
        waiting_ok(name, s)
    r, s = get("depth16_stall")
    if r:
        name = "depth16_stall"
        sound(r, name, s)
        c = s.recording(1)["capture"]
        check(r["keeps"] == 1 and r["coarse_stored"] == 0 and c["dropped_blocks"] == 0 and
              3.7 * 4000 < c["maximum_pending_blocks"] < 4.4 * 4000,
              f"{name}: a stall of 3.8 s on noise at 16 bit: carried, nothing dropped, no stretches; "
              f"most audio waiting {c['maximum_pending_blocks'] / 4000:.2f} s, "
              f"{c['maximum_pending_bytes']} bytes of the ring's {RING_FULL * PAGE}")
        kept(r, name, s, 0, 1, 120, width=2)
    r, s = get("depth_switch")
    if r:
        name = "depth_switch"
        sound(r, name, s)
        full24 = (cap(64) // CLUSTER * CLUSTER - 512 - CUE_RESERVE) // PAGE * PAGE / RATE
        full16 = (cap(64) // CLUSTER * CLUSTER - 512 - CUE_RESERVE) // PAGE * PAGE / 192_000
        check(r["keeps"] == 2 and r["switches"] == 2 and len(r["takes"]) == 4 and r["fails"] == 0
              and r["status_flags"] & 0x20,
              f"{name}: two takes, each through two files")
        kept(r, name, s, 0, 1, full24, kind=SWITCH)
        kept(r, name, s, 1, 2, 90 - full24, before=full24, index=1)
        kept(r, name, s, 2, 3, full16, kind=SWITCH, width=2)
        kept(r, name, s, 3, 4, 170 - full16, before=full16, index=1, width=2)
        check(s.text(raw=True) == HEAD_LOG + D24 % 1 + D16 % 3,
              f"{name}: 24 bit chosen before the stick was ready, 16 again while armed, the TAP "
              f"during the second take not taken: {s.text(raw=True)!r}")
    r, s = get("depth_power")
    if r:
        name = "depth_power"
        rec1, rec2 = s.recording(1), s.recording(2)
        check(rec1 and rec2 and struct.unpack_from("<H", rec1["header"], 34)[0] == 24
              and rec1["state"] == 2 and r["closed_headers"] == 1
              and struct.unpack_from("<H", rec2["header"], 34)[0] == 16
              and abs(rec2.get("data_bytes", 0) / 192_000 - 40) < 0.2
              and s.fsck_clean and not s.problems and s.others_unchanged()
              and r["data_ok"] and r["bounds_ok"] and r["header_ok"],
              f"{name}: power lost in a 24-bit recording: the start-up closes REC0001.WAV as 24 "
              f"bit; after the power-on the next one, REC0002.WAV, is 16 bit "
              f"({rec2.get('data_bytes', 0) / 192_000:.1f} s)")
        check(s.text(raw=True) == HEAD_LOG + "REC0001.WAV NOT CLOSED (POWER OR USB LOST), REPAIRED\r\n" + D16 % 2,
              f"{name}: REC0001's line was still in memory when the power went; then the repair "
              f"and REC0002.WAV 16 BIT: {s.text(raw=True)!r}")

    told = {n: bool(r["status_flags"] & 8) for n, r in results.items()}
    should = {"chain_stick_full": True, "write_fail": True, "stick_pulled": True, "two_takes": False,
              "menu_mixer": False, "cut_next_ready": False, "hand_short": False, "lost_many": False}
    odd = {n: told[n] for n, want in should.items() if n in told and told[n] != want}
    check(not odd, "the menu is told that the last recording ended by itself when the stick was full, "
          "a write failed or the stick was pulled, and not after a stop by hand or a normal end"
          + (f": NOT {odd}" if odd else ""))
    r, s = get("chain_stick_full")
    if r:
        n = max(i for i in range(1, 40) if s.recording(i))
        check((s.text() or "").endswith("REC%04d.WAV STOPPED, USB FULL\r\n" % n),
              f"chain_stick_full: the errors file says that REC{n:04d}.WAV stopped because the stick "
              f"was full: {s.text()!r}")
    r, s = get("write_fail")
    if r:
        check("STOPPED, USB ERROR\r\n" in (s.text() or ""),
              f"write_fail: a recording that stops on a failed write gets its line: {s.text()!r}")


if __name__ == "__main__":
    main()
