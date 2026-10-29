#!/usr/bin/env python3
"""Read a recording and the recorder's report inside it.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""

import argparse
import hashlib
import math
import mmap
import os
import re
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
STATUS = {
    0x1: "AUDIO_DROPPED", 0x2: "INVALID_BLOCK", 0x4: "DIRECT_WRITE", 0x8: "FS_IO",
    0x10: "NOT_FAT32", 0x20: "HEADER_FAILED", 0x40: "MEMORY_DIRTY",
    0x80: "FS_VERIFY", 0x100: "MAP_MISMATCH", 0x200: "REVALIDATION",
    0x400: "NO_SPACE", 0x800: "FS_DIR", 0x1000: "NAME_FAILED",
    0x2000: "BOUNDARY", 0x4000: "ALLOC_FAILED", 0x8000: "RELEASE_FAILED",
    0x10000: "FS_CHANGED", 0x20000: "GEOMETRY", 0x40000: "FS_CHAIN",
    0x80000: "EXTRA_RAM_CHANGED", 0x100000: "SMALL_RING", 0x200000: "GUARD_CHANGED",
    0x400000: "WATCH_CHANGED", 0x800000: "STORED_16_BIT_IN_FREEZES",
}
INFORMATIONAL = 0x80000 | 0x100000 | 0x4000 | 0x1000 | 0x400000 | 0x800000
PHASES = ["VERIFY_MEMORY", "SETTLE", "SNAPSHOT", "PREPARE", "LOG", "AUDIT", "ARMED", "STREAM",
          "CLOSE", "FAILED"]
FS_ERRORS = {0: "none", -1: "sector transfer failed", -2: "not a usable FAT32 volume",
             -3: "root directory", -4: "no space", -5: "chain does not fit size",
             -6: "sector not as expected", -7: "read-back differs",
             -8: "chain points to a free cluster"}
FS_PHASES = ["IDLE", "MOUNT", "DIR", "RECOVER_HEAD", "RECOVER_MAP", "RECOVER_CUT", "TMP_MAP",
             "TMP_HEAD", "SCAN", "NEW_HEAD", "NEW_DIR", "LINK", "COMMIT", "ADDED", "FSINFO", "CUT_DIR",
             "TAIL_SLOT", "TAIL_HEAD", "TAIL_DIR", "CUT_FAT", "FREE_TAIL", "FREE_FAT", "FREE_INFO",
             "READY"]
NEXT_STATES = ["not started", "looking for a directory entry", "searching free space", "header",
               "directory entry", "table", "free count", "ready", "none (no room, or failed)"]
END_REASONS = {0: "still running", 1: "file full", 2: "20 s of silence", 3: "invalid audio block",
               4: "stopped by hand"}
REPORT_OFFSET = 64
PAGE_BYTES = 23040
SCHEMAS = (50, 51, 52, 53)
EXTRA_PIECES = ((20, "0x1C0AD800"), (2, "0x1C130E00"), (8, "0x1C077000"))


def byte_rate(schema):
    return 288000 if schema >= 51 else 192000


def constants():
    text = open(os.path.join(HERE, "src", "stream.h")).read()
    return {m.group(1): int(m.group(2))
            for m in re.finditer(r"#define (\w+)\s+(\d+)u\b", text)}


def struct_fields(name):
    text = open(os.path.join(HERE, "src", "stream.h")).read()
    sizes = constants()
    body = re.search(r"struct %s \{(.*?)\n\};" % name, text, re.S).group(1)
    body = re.sub(r"/\*.*?\*/", "", body, flags=re.S)
    names = []
    for line in body.splitlines():
        line = line.strip().rstrip(";")
        if not line:
            continue
        m = re.match(r".*?[\s\*](\w+)(?:\[(\w+)\])?$", line)
        field, count = m.group(1), m.group(2)
        if count:
            n = sizes.get(count) or int(count.rstrip("u"))
            names += [f"{field}[{i}]" for i in range(n)]
        else:
            names.append(field)
    return names


def decode(header):
    parts = [struct_fields(n) for n in ("session_state", "take_state", "capture_state")]
    words = struct.unpack_from("<%dI" % sum(len(p) for p in parts), header, REPORT_OFFSET)
    result, position = [], 0
    for names in parts:
        result.append(dict(zip(names, words[position:position + len(names)])))
        position += len(names)
    if struct.unpack_from("<I", header, 52)[0] < 52:
        result[1]["blocks_before"] //= 24
        result[2]["coarse"] = 0
    return tuple(result)


def decode_listen(raw):
    if len(raw) < 512 or raw[:8] != b"DJMLSN01":
        return None
    names = ("polls", "packets", "missed", "torn", "checksum_bad", "pipe_packets", "pipe_bytes",
             "clock_flags", "seen_count", "feed", "head_changes")
    out = dict(zip(names, struct.unpack_from(">11I", raw, 8)))
    out["first_head"], out["last_head"], out["tail"] = raw[52:80], raw[80:108], raw[108:132]
    out["seen"] = [tuple(raw[132 + 4 * i:136 + 4 * i]) for i in range(min(out["seen_count"], 95))]
    return out


def decode_tunnel(raw):
    if len(raw) < 2048 or raw[:8] not in (b"DJMTUN01", b"DJMTUN02"):
        return None
    history = 123 if raw[:8] == b"DJMTUN01" else 121
    out = dict(zip(("changes", "nonzero", "sent", "busy"), struct.unpack_from(">4I", raw, 8)))
    out["first"], out["last"], out["status"] = raw[24:36], raw[36:48], raw[48:72]
    out["history"] = [(struct.unpack_from(">I", raw, 72 + 16 * i)[0], raw[76 + 16 * i:88 + 16 * i])
                      for i in range(min(out["changes"], history))]
    out["commands"], out["last_command"] = struct.unpack_from(">IB", raw, 72 + 16 * history)
    out["watch"] = None
    if history == 121:
        names = ("passes", "changes", "first_address", "first_pass", "first_area",
                 "last_address", "last_pass", "last_area")
        out["watch"] = dict(zip(names, struct.unpack_from(">8I", raw, 2016)))
    return out


WATCH_AREAS = {1: "below the extra pages (0x1C0AD000, 48 kB)",
               2: "the rest of the stock heap (0x1C130E00, 62 kB)",
               3: "the unused array (0x1C076174, 196 kB)"}


def watch_text(w):
    if w is None:
        return "not in this build"
    if w["passes"] == 0 and w["changes"] == 0:
        return "none"
    text = f"{w['passes']} passes over the three areas (307 kB, read only)"
    if w["changes"] == 0:
        return text + ("; nothing ever changed" if w["passes"] > 1 else "; too few to say anything yet")
    text += (f"; CHANGED {w['changes']} times: first a piece at {w['first_address']:#010x} in area "
             f"{w['first_area']} ({WATCH_AREAS.get(w['first_area'], '?')}), found by pass {w['first_pass']}")
    if w["changes"] > 1:
        text += (f"; latest at {w['last_address']:#010x} in area {w['last_area']}, pass {w['last_pass']}")
    return text


def decode_status(status):
    if len(status) != 24 or status[0] != 0x52 or status[1] not in (1, 2, 3) or \
            status[23] != (~sum(status[:23]) & 0xFF):
        return None
    seconds = int.from_bytes(status[5:8], "big") if status[1] == 1 else \
        status[5] * 3600 + status[6] * 60 + status[7]
    return {"counter": status[2], "state": status[3], "flags": status[4],
            "seconds": seconds, "marks": status[8],
            "space": int.from_bytes(status[9:11], "big"),
            "deep": bool(status[4] & 0x20),
            "status_flags": int.from_bytes(status[11:15], "big"),
            "file": status[15],
            "reason": status[16], "cut": status[17],
            "left": (status[18] - 1) * 60 + status[19] if status[18] else None}


STATE_NAMES = {0: "not ready", 1: "ready", 2: "recording", 3: "closing"}
REASON_NAMES = {0: "", 1: "starting up or looking at a stick", 2: "no stick", 3: "not FAT32",
                4: "no room", 5: "transfers or file system", 6: "other"}
CUT_NAMES = {0: "a cut is possible", 1: "a cut has to wait", 2: "too little room for a cut"}


def print_tunnel(t, packets):
    print("tunnel, from the panel: " + ("NOTHING arrived (all zero in every packet)" if t["nonzero"] == 0 else
          f"its bytes 0x04-0x0F were in {t['nonzero']} of {packets} packets; they changed "
          f"{max(0, t['changes'] - 1)} times"))
    if t["nonzero"]:
        print(f"                       first {t['first'].hex(' ')}")
        print(f"                       last  {t['last'].hex(' ')}")
        previous = None
        for packet, data in t["history"]:
            what = ""
            if previous is not None:
                what = "  " + ", ".join(f"byte 0x{4 + i:02X} {previous[i]:02X}->{data[i]:02X}"
                                        for i in range(12) if previous[i] != data[i])
            print(f"                       packet {packet:8d} ({packet / 500.0:7.2f} s)  {data.hex(' ')}{what}")
            previous = data
    print("memory watch:          " + watch_text(t.get("watch")))
    names = {1: "start", 2: "stop", 3: "marker", 4: "cut"}
    print(f"commands from the panel's menu: {t['commands']}"
          + (f" (the last one: {names.get(t['last_command'] & 15, '?')}, sequence "
             f"{t['last_command'] >> 4})" if t["commands"] else ""))
    status = decode_status(t["status"])
    print(f"tunnel, to the panel:  {t['sent']} statuses put into the packet for main "
          f"({t['busy']} looks waited for a transfer to end)")
    if status:
        print(f"                       last: {STATE_NAMES.get(status['state'], status['state'])}, "
              f"{status['seconds']} s, {status['marks']} markers, next file number {status['next_number']}, "
              f"flags {status['flags']:#04x}, session flags {status['status_flags']:#010x}, "
              f"counter {status['counter']}"
              + (f", countdown -{status['left'] // 60:02d}:{status['left'] % 60:02d}"
                 if status["left"] is not None else ""))
    else:
        print(f"                       last status does not check out: {t['status'].hex(' ')}")


MIDI_KINDS = {0x80: "note off", 0x90: "note on", 0xA0: "poly pressure", 0xB0: "controller",
              0xC0: "program", 0xD0: "pressure", 0xE0: "pitch bend"}


def feed_text(feed):
    requests, answers, last = feed >> 16, feed >> 8 & 0xFF, feed & 0xFF
    if requests == 0:
        return "never asked for (no stick was ready)"
    code = {0: "none", 1: "done (0)", 0x0F: "REFUSED (0x0E)"}.get(last, f"code {last - 1:#04x}")
    return (f"{requests}{'+' if requests == 0xFFFF else ''} requests sent, "
            f"{answers}{'+' if answers == 255 else ''} answers from main, the last one: {code}")


def print_listen(l):
    print("link from main:        "
          f"{l['packets']} packets seen in {l['polls']} looks ({l['polls'] / 4000.0:.0f} s), "
          f"{l['missed']} missed; looked at again because still arriving: {l['torn']} (changed "
          f"while copied) and {l['checksum_bad']} (checksum did not fit yet)")
    print(f"                       first packet starts {l['first_head'].hex(' ')}")
    print(f"                       last packet starts  {l['last_head'].hex(' ')}"
          f" (changed {l['head_changes']} times, sequence aside)")
    print(f"                       unused part of the last packet: "
          + ("all zero" if not any(l["tail"]) else l["tail"].hex(" ")))
    print(f"MIDI pipe:             {l['pipe_packets']} packets with data, {l['pipe_bytes']} bytes, "
          f"{l['clock_flags']} MIDI clock ticks")
    print("recording feed:        " + feed_text(l["feed"]))
    for status, data1, last, count in l["seen"]:
        kind = MIDI_KINDS.get(status & 0xF0, "system" if status >= 0xF0 else "?")
        print(f"                       {status:02X} {data1:02X}  {kind:10s} channel {(status & 15) + 1:2d} "
              f"number {data1:3d}: seen {count}{'+' if count == 255 else ''} times, last value {last}")


def slow_log(t):
    return [(t[f"slow_log[{i}]"] >> 16, t[f"slow_log[{i}]"] & 0xFFFF)
            for i in range(t["slow_count"])]


def flag_names(flags):
    return " ".join(name for bit, name in STATUS.items() if flags & bit) or "none"


def ms(us):
    return f"{us / 1000.0:.1f} ms"


def signed(value):
    return struct.unpack("<i", struct.pack("<I", value))[0]


def extra_text(state):
    text = {0: "pending", 1: "accepted", 2: "REJECTED"}.get(state & 0xFF, "?")
    out = [n for n in range(len(EXTRA_PIECES)) if state >> 8 >> n & 1]
    if out and state & 0xFF != 2:
        text += "; OUT for this session: " + ", ".join(
            f"piece {n} ({EXTRA_PIECES[n][0]} pages at {EXTRA_PIECES[n][1]})" for n in out)
    return text


def print_session(s, rate):
    phase = PHASES[s["phase"]] if s["phase"] < len(PHASES) else str(s["phase"])
    fs_phase = FS_PHASES[s["fs_phase"]] if s["fs_phase"] < len(FS_PHASES) else str(s["fs_phase"])
    print(f"recorder state:        {phase}, flags since power-on "
          f"{s['status_flags']:#x} ({flag_names(s['status_flags'])})")
    print(f"takes since power-on:  {s['takes_started']} started, {s['takes_kept']} kept, "
          f"{s['takes_discarded']} discarded (sound checks), {s['takes_failed']} failed")
    print(f"files:                 {s['files_made']} waiting files made, {s['files_reused']} found, "
          f"{s['files_refilled']} topped up, {s['files_cut']} cut to length, "
          f"{s['files_recovered']} closed after a power cut; next number {s['next_number']}")
    print(f"bookkeeping:           state {fs_phase}, {s['sector_reads']} reads, {s['sector_writes']} "
          f"writes, {s['prepare_attempts']} looks at the stick, last one {s['prepare_duration_us'] / 1e6:.2f} s"
          + (f"; last error: {FS_ERRORS.get(signed(s['fs_error']), s['fs_error'])} in "
             f"{FS_PHASES[s['fs_error_phase']] if s['fs_error_phase'] < len(FS_PHASES) else s['fs_error_phase']}"
             if s["fs_error"] else ""))
    print(f"next file:             {NEXT_STATES[s['next_state']] if s['next_state'] < len(NEXT_STATES) else s['next_state']}; "
          f"{s['files_switched']} file changes without a gap since power-on")
    print(f"volume:                {s['sectors_per_cluster']} sectors/cluster, table at sector "
          f"{s['fat_start']} ({s['fat_sectors']} sectors), data from sector {s['data_start']}, "
          f"{s['end_cluster'] - 2 if s['end_cluster'] else 0} clusters")
    print(f"file in use:           {s['file_bytes']} bytes reserved ({(s['file_bytes'] - 512) / rate:.0f} s) "
          f"in {s['extent_count']} pieces (limit 1024), first sector {s['first_sector']}, "
          f"checksum {s['map_sum']:#010x}")
    print(f"ring:                  {s['ring_base_pages']} pages before the pool pages, extra RAM "
          f"{extra_text(s['extra_state'])} ({s['extra_passes']} checksum passes agreed)")
    print(f"errors:                {s['direct_read_errors']} read, {s['direct_write_errors']} write, "
          f"pool release {signed(s['first_release_error'])}, log writes {s['log_writes']}")
    print(f"errors file:           {s['text_lines']} lines written to REC-LOG.TXT since power-on"
          + (f"; last attempt given up: {FS_ERRORS.get(signed(s['text_error']), signed(s['text_error']))}"
             if s["text_error"] else ""))
    if s["status_flags"] & 0x40:
        print(f"memory check:          {s['first_bad_address']:#010x} held {s['first_bad_value']:#010x}")


def file_dropped(t, c):
    return ((c["dropped_blocks"] - t["dropped_before"]) & 0xFFFFFFFF,
            (c["gap_count"] - t["gaps_before"]) & 0xFFFFFFFF)


def print_take(t, c, rate):
    ring = c["ring_pages"] * PAGE_BYTES
    dropped, gaps = file_dropped(t, c)
    place = f"file {t['chain_index'] + 1} of its take"
    if t["chain_index"]:
        place += f", begins {t['blocks_before'] / 4000.0:.3f} s into the take"
    if t["continued"]:
        place += "; the take continues in the next file without a gap"
    print(f"position:              {place}")
    print(f"captured:              {c['captured_blocks']} blocks ({c['captured_blocks'] / 4000.0:.3f} s) "
          f"in the take so far, ended by: "
          + ("file full, next file ready" if t["continued"] else str(END_REASONS.get(c["end_reason"], c["end_reason"]))))
    print(f"dropped:               {dropped} blocks ({dropped * 0.25:.1f} ms) in {gaps} gap(s) "
          f"during this file, {c['invalid_blocks']} invalid blocks")
    if c["coarse"]:
        stretches, blocks_ = c["coarse"] >> 24, c["coarse"] & 0xFFFFFF
        print(f"stored with 16 bits:   {blocks_ / 4000.0:.2f} s in {stretches}{'+' if stretches == 255 else ''} "
              f"stretch(es) in the take so far (the stick froze and the ring had filled to the mark)")
    if c["maximum_pending_blocks"]:
        print(f"ring:                  {c['ring_pages']} pages = {ring} bytes; fullest "
              f"{c['maximum_pending_bytes']} bytes ({100.0 * c['maximum_pending_bytes'] / ring if ring else 0:.0f} %); "
              f"most audio waiting {c['maximum_pending_blocks'] / 4000.0:.2f} s")
    else:
        print(f"ring:                  {c['ring_pages']} pages ({ring / 192000.0:.2f} s), peak "
              f"{c['maximum_pending_bytes']} bytes = {c['maximum_pending_bytes'] / 192.0:.0f} ms "
              f"({100.0 * c['maximum_pending_bytes'] / ring if ring else 0:.0f} %)")
    if t["write_count"]:
        print(f"page writes:           {t['write_count']}, {ms(t['data_write_min_us'])} to "
              f"{ms(t['data_write_max_us'])}, total {t['total_data_write_us'] / 1e6:.1f} s, "
              f"{t['direct_block_writes']} block writes")
        print(f"slowest page write:    at file offset {t['max_write_offset_bytes']} "
              f"({t['max_write_offset_bytes'] / rate:.1f} s into the recording)")
    print(f"slow page writes:      {t['slow_writes[0]']} over 250 ms, {t['slow_writes[1]']} over "
          f"500 ms, {t['slow_writes[2]']} over 1000 ms")
    for page, duration in slow_log(t):
        print(f"                       page {page} ({page * PAGE_BYTES / rate:.1f} s): {duration} ms")
    print(f"header:                written {t['header_writes']} times, slowest "
          f"{ms(t['header_write_max_us'])}; take start took {ms(t['start_duration_us'])}")
    print(f"service gap:           up to {ms(t['service_gap_max_us'])}; pool pages {t['pool_pages']}")
    if t["number"]:
        print(f"name:                  REC{t['number']:04d}.WAV, naming took {ms(t['name_duration_us'])}")


def markers(tail):
    points, lost, coarse, at = [], set(), set(), 0
    while at + 8 <= len(tail):
        tag, size = tail[at:at + 4], struct.unpack_from("<I", tail, at + 4)[0]
        body = tail[at + 8:at + 8 + size]
        if tag == b"cue " and len(body) >= 4:
            for k in range(min(struct.unpack_from("<I", body)[0], (len(body) - 4) // 24)):
                points.append(struct.unpack_from("<I", body, 4 + 24 * k + 4)[0] / 48000.0)
        elif tag == b"LIST" and body[:4] == b"adtl":
            inner = 4
            while inner + 12 <= len(body):
                length = struct.unpack_from("<I", body, inner + 4)[0]
                if body[inner:inner + 4] == b"labl" and body[inner + 12:inner + 16] == b"LOST":
                    lost.add(struct.unpack_from("<I", body, inner + 8)[0])
                if body[inner:inner + 4] == b"labl" and body[inner + 12:inner + 17] == b"16BIT":
                    coarse.add(struct.unpack_from("<I", body, inner + 8)[0])
                inner += 8 + length + (length & 1)
        else:
            break
        at += 8 + size + (size & 1)
    return points, lost, coarse


def coarse_stretches(raw, start, length):
    runs, chunk = [], 3 * (1 << 22)
    for at in range(0, length - length % 6, chunk):
        low = raw[start + at:start + min(at + chunk, length - length % 6):3]
        for m in re.finditer(rb"\x00{24,}", low):
            a, b = at // 3 + m.start(), at // 3 + m.end()
            if runs and runs[-1][1] == a:
                runs[-1][1] = b
            else:
                runs.append([a, b])
    out = []
    for a, b in runs:
        a += a % 2
        b -= b % 2
        if b - a >= 24 and any(raw[start + 3 * a + 1:start + 3 * b:3]):
            out.append((a // 2, (b - a) // 2))
    return out


def analyze(path, continuity):
    size = os.path.getsize(path)
    with open(path, "rb") as f:
        raw = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
    digest = hashlib.sha256()
    for off in range(0, size, 1 << 24):
        digest.update(raw[off:off + (1 << 24)])
    print(f"file:                  {os.path.abspath(path)}")
    print(f"sha256:                {digest.hexdigest()}")
    print(f"physical bytes:        {size}")

    header = raw[:512]
    if header[:8] == b"DJMLOG00":
        print("result:                empty log: the recorder has not written it")
        return False
    if header[:8] == b"DJMLOG01":
        if header[44:52] != b"DJMREC44":
            sys.exit("log of another firmware version")
        s, t, c = decode(header)
        rate = byte_rate(struct.unpack_from("<I", header, 52)[0])
        print("kind:                  log (state when the recorder last prepared or looked for a slot)")
        print_session(s, rate)
        listen = decode_listen(raw[512:1024])
        if listen:
            print_listen(listen)
        tunnel = decode_tunnel(raw[1024:3072])
        if tunnel:
            print_tunnel(tunnel, listen["packets"] if listen else 0)
        if s["takes_started"]:
            print("last take:")
            print_take(t, c, rate)
        return True
    if header[:4] != b"RIFF" or header[8:12] != b"WAVE" or header[504:508] != b"data":
        sys.exit("not a recorder WAV file")
    if header[44:52] != b"DJMREC44":
        sys.exit("not a recording of this recorder")

    schema = struct.unpack_from("<I", header, 52)[0]
    if schema not in SCHEMAS:
        sys.exit(f"this file has report schema {schema}; this analyze_wav.py reads the schemas "
                 f"{SCHEMAS}")
    flags, state = struct.unpack_from("<II", header, 56)
    data_bytes = struct.unpack_from("<I", header, 508)[0]
    s, t, c = decode(header)
    rate, frame, bits = struct.unpack_from("<IHH", header, 28)
    width = bits // 8
    if (rate, frame) != (48000 * 2 * width, 2 * width) or width not in (2, 3):
        sys.exit(f"unexpected audio format: {bits} bit, {frame} bytes a frame, {rate} bytes a second")
    print(f"take:                  {data_bytes / rate:.3f} s of audio ({data_bytes} bytes), {bits} bit")
    print(f"take flags:            {flags:#x} ({flag_names(flags)})")
    print_take(t, c, rate)
    print_session(s, rate)

    if state == 0:
        print("result:                a waiting file: no take in it")
        return False
    left = 0
    if size < 512 + data_bytes:
        print("file is shorter than its header says")
        left = 1
    points, lost_ids, coarse_ids = markers(raw[512 + data_bytes:size])

    def clock(seconds):
        return f"{int(seconds) // 60:02d}:{seconds % 60:05.2f}"
    if points:
        own = [clock(p) for i, p in enumerate(points, 1) if i not in lost_ids and i not in coarse_ids]
        lost = [clock(p) for i, p in enumerate(points, 1) if i in lost_ids]
        coarse = [clock(p) for i, p in enumerate(points, 1) if i in coarse_ids]
        print(f"markers:               {len(own)}" + (": " + ", ".join(own) if own else ""))
        if lost:
            print(f"audio lost at:         {', '.join(lost)} (markers named LOST)")
        if coarse:
            print(f"16 bits from:          {', '.join(coarse)} (markers named 16BIT)")

    peak = [0, 0]
    available = min(data_bytes, size - 512)
    low = [set(), set()]
    for i in range(512, 512 + available - frame + 1, frame * 64):
        for ch in (0, 1):
            v = int.from_bytes(raw[i + ch * width:i + (ch + 1) * width], "little", signed=True)
            if abs(v) > peak[ch]:
                peak[ch] = abs(v)
            low[ch].add(v & 0xFF)
    db = [20 * math.log10(p / float(1 << (bits - 1))) if p else float("-inf") for p in peak]
    print(f"peak (sampled):        L {db[0]:.1f} dBFS, R {db[1]:.1f} dBFS")
    found = coarse_stretches(raw, 512, min(data_bytes, size - 512)) if width == 3 else []
    if found:
        total = sum(n for _, n in found)
        print(f"16 bits in the audio:  {len(found)} stretch(es), {total / 48000.0:.2f} s in all: "
              + ", ".join(f"{clock(a / 48000.0)} for {n / 48000.0:.2f} s" for a, n in found[:12])
              + (" ..." if len(found) > 12 else ""))
    if width == 3:
        print(f"lowest 8 of the 24 bits (sampled): {len(low[0])} and {len(low[1])} of 256 values occur, left and right"
              + ("" if max(len(low[0]), len(low[1])) > 16 else ": NOT USED (silence, or audio of fewer bits)"))

    if continuity:
        frames = available // frame
        run = best = 0
        prev = None
        for i in range(frames):
            cur = raw[512 + i * frame:512 + (i + 1) * frame]
            if cur == prev:
                run += 1
                best = max(best, run)
            else:
                run = 0
            prev = cur
        print(f"longest run of identical frames: {best + 1}")

    consistent = (data_bytes == t["actual_words"] * width and left == 0
                  and (flags & ~INFORMATIONAL & ~0x1) == 0)
    if state == 1:
        print("result:                NOT CLOSED (power cut or stick removed): playable up to the "
              "last header update, at most 15 s before the end")
        return False
    dropped, gaps = file_dropped(t, c)
    in_file = ((c["produced_words"] - t["blocks_before"] * 24) & 0xFFFFFFFF) * width
    if consistent and state == 2 and t["continued"]:
        print("result:                " + ("PASS (complete, nothing dropped, the take continues in the "
              "next file)" if dropped == 0 else f"COMPLETE WITH GAPS ({dropped * 0.25:.1f} ms missing)"))
        return dropped == 0
    if consistent and state == 2 and c["end_reason"] == 0 and data_bytes <= in_file:
        print("result:                CLOSED AFTER A POWER CUT: holds the audio up to its last header "
              "update, cut to that length at the next start"
              + ("" if dropped == 0 else f"; {dropped * 0.25:.1f} ms dropped before that"))
        return dropped == 0
    complete = consistent and state == 2 and data_bytes == in_file
    if complete and dropped == 0:
        print("result:                PASS (complete, nothing dropped"
              + (f"; {sum(n for _, n in found) / 48000.0:.2f} s of it stored with 16 bits during freezes of the stick)"
                 if found else ")"))
        return True
    if complete:
        print(f"result:                COMPLETE WITH GAPS ({dropped * 0.25:.1f} ms missing "
              f"in {gaps} places)")
        return False
    print("result:                NOT a clean recording")
    return False


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("file")
    parser.add_argument("--continuity", action="store_true",
                        help="also look for runs of identical frames (slow on long files)")
    args = parser.parse_args()
    sys.exit(0 if analyze(args.file, args.continuity) else 1)


if __name__ == "__main__":
    main()
