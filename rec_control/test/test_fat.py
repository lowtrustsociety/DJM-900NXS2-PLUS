#!/usr/bin/env python3
"""Tests of the FAT32 code on disk images.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""

import hashlib
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
PROBE = os.path.dirname(HERE)
NATIVE = os.path.join(PROBE, "build", "fat_native")
RATE = 192_000
failed = []


def check(value, label):
    print(("PASS " if value else "FAIL ") + label)
    if not value:
        failed.append(label)


def sh(*command, ok=(0,)):
    result = subprocess.run(command, capture_output=True, text=True)
    if result.returncode not in ok:
        raise RuntimeError(" ".join(command) + "\n" + result.stdout + result.stderr)
    return result


def build():
    os.makedirs(os.path.join(PROBE, "build"), exist_ok=True)
    sh("cc", "-std=c11", "-O1", "-Wall", "-Wextra", "-Werror", "-o", NATIVE,
       os.path.join(HERE, "fat_native.c"), os.path.join(PROBE, "src", "fat.c"),
       os.path.join(PROBE, "src", "recfile.c"))


def make_image(path, megabytes, spc, fat="32"):
    sh("mkfile", "-n", f"{megabytes}m", path)
    device = sh("hdiutil", "attach", "-nomount", "-imagekey",
                "diskimage-class=CRawDiskImage", path).stdout.split()[0]
    try:
        sh("newfs_msdos", "-F", fat, "-c", str(spc), "-v", "TESTVOL", device)
    finally:
        sh("hdiutil", "detach", device)


class Mounted:
    def __init__(self, path):
        self.path = path

    def __enter__(self):
        self.mount = tempfile.mkdtemp(prefix="fatmnt", dir=os.environ.get("FAT_MOUNT_DIR"))
        out = sh("hdiutil", "attach", "-nobrowse", "-imagekey", "diskimage-class=CRawDiskImage",
                 "-mountpoint", self.mount, self.path).stdout
        self.device = out.split()[0]
        return self.mount

    def __exit__(self, *exc):
        sh("hdiutil", "detach", self.device)
        os.rmdir(self.mount)


class Volume:
    def __init__(self, path, copy=0):
        self.f = open(path, "rb")
        b = self.sector(0)
        self.spc = b[13]
        self.reserved = struct.unpack_from("<H", b, 14)[0]
        self.fats = b[16]
        self.fat_sectors = struct.unpack_from("<I", b, 36)[0]
        total = struct.unpack_from("<H", b, 19)[0] or struct.unpack_from("<I", b, 32)[0]
        self.root = struct.unpack_from("<I", b, 44)[0]
        self.info_sector = struct.unpack_from("<H", b, 48)[0]
        self.data_start = self.reserved + self.fats * self.fat_sectors
        self.end = (total - self.data_start) // self.spc + 2
        self.cb = self.spc * 512
        tables = []
        for n in range(self.fats):
            self.f.seek((self.reserved + n * self.fat_sectors) * 512)
            tables.append(self.f.read(self.fat_sectors * 512))
        raw = tables[min(copy, self.fats - 1)]
        self.fat = [v & 0x0FFFFFFF for v in struct.unpack("<%dI" % (len(raw) // 4), raw)]
        self.fat2_same = all(t == tables[0] for t in tables)

    def sector(self, n, count=1):
        self.f.seek(n * 512)
        return self.f.read(512 * count)

    def chain(self, first, limit=None):
        out, seen = [], set()
        c = first
        while 2 <= c < self.end and c not in seen and (limit is None or len(out) < limit):
            out.append(c)
            seen.add(c)
            c = self.fat[c]
        return out, c

    def read_clusters(self, clusters):
        return b"".join(self.sector(self.data_start + (c - 2) * self.spc, self.spc) for c in clusters)

    def directory(self, first):
        clusters, _ = self.chain(first)
        raw = self.read_clusters(clusters)
        entries = []
        for i in range(0, len(raw), 32):
            e = raw[i:i + 32]
            if e[0] == 0:
                break
            if e[0] == 0xE5 or e[11] & 0x0F == 0x0F or e[11] & 0x08:
                continue
            first_cluster = struct.unpack_from("<H", e, 20)[0] << 16 | struct.unpack_from("<H", e, 26)[0]
            entries.append((bytes(e[:11]), e[11], first_cluster, struct.unpack_from("<I", e, 28)[0]))
        return entries

    def tree(self, first=None, prefix=""):
        out = []
        for name, attr, cluster, size in self.directory(first or self.root):
            if name[:1] == b".":
                continue
            path = prefix + name.decode("latin-1")
            out.append((path, attr, cluster, size))
            if attr & 0x10 and cluster:
                out += self.tree(cluster, path + "/")
        return out

    def audit(self):
        problems = []
        if not self.fat2_same:
            problems.append("table copies differ")
        owner = {}
        owned = 0
        items = [("<root>", 0x10, self.root, 0)] + self.tree()
        for path, attr, first, size in items:
            if first == 0:
                if size:
                    problems.append(f"{path}: size {size} without clusters")
                continue
            clusters, end = self.chain(first)
            if end < 0x0FFFFFF8:
                problems.append(f"{path}: chain ends in {end:#x}")
            if end == 0 and clusters and self.fat[clusters[-1]] == 0:
                clusters = clusters[:-1]
            for c in clusters:
                if c in owner:
                    problems.append(f"{path}: shares cluster {c} with {owner[c]}")
                else:
                    owned += 1
                owner[c] = path
            if not attr & 0x10:
                need = -(-size // self.cb)
                if need > len(clusters):
                    problems.append(f"{path}: size {size} needs {need} clusters, chain has {len(clusters)}")
                elif need < len(clusters):
                    problems.append(f"{path}: chain longer than size ({len(clusters)} > {need})")
        used = sum(1 for c in range(2, self.end) if self.fat[c] != 0)
        if used != owned:
            problems.append(f"orphans: {used - owned} clusters marked used that no file owns")
        info = self.sector(self.info_sector)
        free = struct.unpack_from("<I", info, 488)[0]
        if free != 0xFFFFFFFF and free != self.end - 2 - used:
            problems.append(f"free count says {free}, really {self.end - 2 - used}")
        return problems

    def root_files(self):
        return {name.decode("latin-1"): (attr, first, size) for name, attr, first, size in self.directory(self.root)}

    def file_bytes(self, first, size):
        clusters, _ = self.chain(first)
        return self.read_clusters(clusters)[:size]

    def hashes(self):
        out = {}
        for path, attr, first, size in self.tree():
            if attr & 0x10 or path.startswith("REC") or path.startswith("DJMREC"):
                continue
            out[path] = hashlib.sha256(self.file_bytes(first, size) if first else b"").hexdigest()
        return out


HARMLESS = ("orphans:", "chain longer than size", "free count says")
AFTER_CUT = HARMLESS + ("table copies differ",)


def harmful(problems, allowed=HARMLESS):
    return [p for p in problems if not any(h in p for h in allowed)]


def harmful_after_cut(image, allowed=AFTER_CUT):
    return (harmful(Volume(image, 0).audit(), allowed)
            + ["second table: " + p for p in harmful(Volume(image, 1).audit(), allowed)])


AT_CUT = AFTER_CUT + ("DJMREC  TMP: chain ends in 0x0", "DJMREC  TMP: size ")
HAND_OVER = re.compile(r"^(second table: )?(DJMREC  TMP: shares cluster \d+ with REC\d{4} WAV|"
                       r"REC\d{4} WAV: shares cluster \d+ with DJMREC  TMP)$")
GIVING_BACK = re.compile(r"^(second table: )?REC\d{4} WAV: chain ends in 0x0$")


class CutLog:
    def __init__(self):
        self.worst, self.harmful, self.fsck_bad, self.changed = {}, [], [], []
        self.hand_overs, self.repair_bad = [], []

    def judge(self, tmp, work, before, options, limit):
        v = Volume(work)
        for p in set(re.sub(r"cluster \d+", "cluster N", p) for p in v.audit()):
            kind = next((h for h in AFTER_CUT if h in p), p)
            self.worst[kind] = self.worst.get(kind, 0) + 1
        problems = harmful_after_cut(work, AT_CUT)
        shared = any(HAND_OVER.match(p) for p in problems)
        bad = [p for p in problems if not HAND_OVER.match(p) and not GIVING_BACK.match(p)]
        if bad:
            self.harmful.append((limit, bad[:6]))
        if v.hashes() != before or Volume(work, 1).hashes() != before:
            self.changed.append(limit)
        text = fsck(work)[1]
        if shared:
            self.hand_overs.append(limit)
            repaired = os.path.join(tmp, "crash_repaired.img")
            shutil.copyfile(work, repaired)
            fixed = subprocess.run(["fsck_msdos", "-y", repaired], capture_output=True, text=True)
            rv = Volume(repaired)
            recs = sorted(n for n in rv.root_files() if n.startswith("REC") and n.endswith("WAV"))
            whole = all(recording_ok(rv, n, int(n[3:7])) for n in recs)
            again = subprocess.run([NATIVE, repaired, *[str(o) for o in options], "boot", "take:61"],
                                   capture_output=True, text=True)
            rv = Volume(repaired)
            if not recs or not whole or rv.hashes() != before or again.returncode not in (0, 1) \
                    or harmful_after_cut(repaired):
                self.repair_bad.append((limit, recs, whole, again.returncode, again.stdout[-200:],
                                        harmful_after_cut(repaired)[:4],
                                        (fixed.stdout + fixed.stderr)[-400:]))
            os.remove(repaired)
        elif any(word in text.lower() for word in ("cross", "invalid", "corrupt", "out of range")):
            self.fsck_bad.append((limit, text))

    def report(self, name, total, expect_hand_over=False):
        check(not self.harmful,
              f"{name}: after a cut at any of the {total} writes no file shares space with another "
              f"and no chain is broken, whichever table copy is believed; the one exception is the "
              f"hand-over of the unused rest (the waiting file and the recording it is cut from), "
              f"seen at {len(self.hand_overs)} of the writes: {self.hand_overs}"
              + (f" FAILED at {self.harmful[:3]}" if self.harmful else ""))
        if self.hand_overs or expect_hand_over:
            check(bool(self.hand_overs) and not self.repair_bad,
                  f"{name}: a computer's disk repair (fsck_msdos -y) run on the stick in the "
                  f"hand-over state: every recording comes out whole, the other files are "
                  f"untouched, and the recorder works with what the repair leaves "
                  f"({len(self.hand_overs)} states tried)"
                  + (f" FAILED at {self.repair_bad[:2]}" if self.repair_bad else ""))
        unowned = {k: n for k, n in self.worst.items() if "orphans" in k}
        check(not unowned, f"{name}: at none of the {total} cuts is any space left without an owner "
              f"(a file that is made, made longer, handed over or given back keeps hold of "
              f"its space at every write)" + (f" FAILED: {unowned}" if unowned else ""))
        check(not self.changed, f"{name}: the other files are byte-identical after every cut"
              + (f" FAILED at {self.changed[:5]}" if self.changed else ""))
        check(not self.fsck_bad, f"{name}: outside the hand-over macOS's checker never reports damage"
              + (f" FAILED at {self.fsck_bad[0]}" if self.fsck_bad else ""))


def fsck(path):
    result = subprocess.run(["fsck_msdos", "-n", path], capture_output=True, text=True)
    text = result.stdout + result.stderr
    return result.returncode == 0, text


def run(image, *arguments, ok=(0,)):
    result = subprocess.run([NATIVE, image, *[str(a) for a in arguments]], capture_output=True, text=True)
    if result.returncode not in ok:
        raise RuntimeError(f"fat_native {arguments}: status {result.returncode}\n{result.stdout}{result.stderr}")
    return result


def recording_ok(volume, name, number, seconds=None, state=2):
    files = volume.root_files()
    if name not in files:
        return False
    attr, first, size = files[name]
    data = volume.file_bytes(first, size)
    want = seconds * RATE if seconds is not None else size - 512
    if size != 512 + want or attr != 0x20 or data[:4] != b"RIFF" or data[44:52] != b"DJMREC43":
        return False
    if struct.unpack_from("<I", data, 60)[0] != state or struct.unpack_from("<I", data, 508)[0] != want:
        return False
    for position in (0, 8, want // 2 // 8 * 8, want - 8):
        tag, offset = struct.unpack_from("<II", data, 512 + position)
        if tag != number or offset != position:
            return False
    return True


def waiting(volume):
    return volume.root_files().get("DJMREC  TMP")


def populate(image, files, delete_every=0, seed=1):
    rng = random.Random(seed)
    with Mounted(image) as mount:
        os.makedirs(os.path.join(mount, "Music Folder"))
        names = []
        for i, size in enumerate(files):
            name = os.path.join(mount, "Music Folder" if i % 3 == 0 else "",
                                f"A long file name number {i} with spaces.dat")
            with open(name, "wb") as f:
                f.write(rng.randbytes(size))
            names.append(name)
        if delete_every:
            for name in names[::delete_every]:
                os.remove(name)


def test_fresh(tmp):
    name = "fresh 16K clusters"
    image = os.path.join(tmp, "fresh.img")
    make_image(image, 1100, 32)
    out = run(image, "--cap", 400_000_000, "--min", 1_000_000, "--refill", 100_000_000,
              "boot", "take:90", "take:61", "short:20", "take:200", "boot", "take:75").stdout
    v = Volume(image)
    clean, text = fsck(image)
    check(clean and not v.audit(), f"{name}: macOS checker and independent audit find nothing")
    check(all(recording_ok(v, f"REC000{n} WAV", n, s) for n, s in ((1, 90), (2, 61), (3, 200), (4, 75))),
          f"{name}: REC0001..REC0004 hold exactly their own audio at their exact length")
    w = waiting(v)
    check(w is not None and w[0] == 0x22 and w[2] % v.cb == 0
          and v.file_bytes(w[1], 512)[60:64] == bytes(4) and v.file_bytes(w[1], 512)[44:52] == b"DJMREC43",
          f"{name}: one hidden waiting file with a no-audio header ({w[2] // 1_000_000} MB)")
    check("reused 1" in out and "made 1" in out and len(v.root_files()) == 5,
          f"{name}: the waiting file was made once and found again after the restart")


def test_refill_and_full(tmp):
    name = "refill and full file"
    image = os.path.join(tmp, "refill.img")
    make_image(image, 1100, 32)
    out = run(image, "--cap", 30_000_000, "--min", 1_000_000, "--refill", 20_000_000,
              "boot", "take:70", "take:400", "take:65").stdout
    v = Volume(image)
    clean, text = fsck(image)
    check(clean and not v.audit(), f"{name}: checker and audit find nothing")
    full_seconds = (30_000_000 // v.cb * v.cb - 512) // 512 * 512 / RATE
    files = v.root_files()
    check(recording_ok(v, "REC0001 WAV", 1, 70) and recording_ok(v, "REC0003 WAV", 3, 65)
          and "refilled 1" in out,
          f"{name}: after a 70 s take the waiting file was topped up again")
    size2 = files["REC0002 WAV"][2]
    check(size2 == 30_000_000 // v.cb * v.cb and "made 2" in out,
          f"{name}: a take longer than the file fills it ({size2 / RATE:.0f} s); new space is "
          f"then found for the next waiting file")


def test_fragmented(tmp):
    name = "fragmented and nearly full (512-byte clusters)"
    image = os.path.join(tmp, "frag.img")
    make_image(image, 36, 1)
    populate(image, [140_000] * 220, delete_every=2)
    before = Volume(image).hashes()
    out = run(image, "--cap", 12_000_000, "--min", 500_000, "--refill", 0, "--margin", 1_000_000,
              "boot", "take:20", "take:15").stdout
    v = Volume(image)
    clean, text = fsck(image)
    w = waiting(v)
    pieces = int(out.split(" pieces")[0].split()[-1])
    check(clean and not v.audit(), f"{name}: checker and audit find nothing")
    check(v.hashes() == before and len(before) > 100,
          f"{name}: all {len(before)} other files (long names, folder) are byte-identical")
    check(pieces > 20 and recording_ok(v, "REC0001 WAV", 1, 20) and recording_ok(v, "REC0002 WAV", 2, 15),
          f"{name}: waiting file made from {pieces} pieces of free space; both takes correct")
    free_after = sum(1 for c in range(2, v.end) if v.fat[c] == 0) * v.cb
    check(w is not None and free_after >= 1_000_000,
          f"{name}: {free_after // 1000} kB left free beside the waiting file")


def test_cluster_sizes(tmp):
    for spc, megabytes in ((8, 300), (64, 2300)):
        name = f"{spc // 2} KB clusters"
        image = os.path.join(tmp, f"c{spc}.img")
        make_image(image, megabytes, spc)
        run(image, "--cap", 100_000_000, "--min", 1_000_000, "--refill", 0, "boot", "take:61", "take:100")
        v = Volume(image)
        clean, text = fsck(image)
        check(clean and not v.audit() and recording_ok(v, "REC0001 WAV", 1, 61)
              and recording_ok(v, "REC0002 WAV", 2, 100),
              f"{name}: two takes, checker and audit find nothing")
        os.remove(image)


def test_power_cut_recording(tmp):
    name = "power cut while recording"
    image = os.path.join(tmp, "cut.img")
    make_image(image, 1100, 32)
    run(image, "--cap", 100_000_000, "--min", 1_000_000, "--refill", 0, "boot", "take:61", "cut:100")
    v = Volume(image)
    files = v.root_files()
    reserve = files["REC0002 WAV"][2]
    check("DJMREC  TMP" not in files and reserve > 60_000_000
          and v.file_bytes(files["REC0002 WAV"][1], 512)[60] == 1 and not harmful(v.audit()),
          f"{name}: 100 s into the second take: REC0002.WAV still owns its whole reserve, header "
          f"marked running; volume sound")
    out = run(image, "--cap", 100_000_000, "--min", 1_000_000, "--refill", 0, "boot", "take:61").stdout
    v = Volume(image)
    clean, text = fsck(image)
    check(clean and not v.audit() and "recovered 1" in out,
          f"{name}: at the next start the recording is closed and cut; checker and audit clean")
    check(recording_ok(v, "REC0002 WAV", 2, 90) and recording_ok(v, "REC0003 WAV", 3, 61)
          and waiting(v) is not None,
          f"{name}: REC0002.WAV keeps the 90 s its last header covered; the rest became the "
          f"waiting file and took the next take")

    image = os.path.join(tmp, "cut_short.img")
    make_image(image, 1100, 32)
    run(image, "--cap", 100_000_000, "--min", 1_000_000, "--refill", 0, "boot", "cut:40")
    out = run(image, "--cap", 100_000_000, "--min", 1_000_000, "--refill", 0, "boot", "take:61").stdout
    v = Volume(image)
    clean, text = fsck(image)
    check(clean and not v.audit() and "reused 1" in out and recording_ok(v, "REC0001 WAV", 1, 61)
          and len(v.root_files()) == 2,
          f"{name}: cut within the first minute: the waiting file is simply used again")


def test_refusals(tmp):
    name = "refusals"
    image = os.path.join(tmp, "fat16.img")
    make_image(image, 200, 8, fat="16")
    before = hashlib.sha256(open(image, "rb").read()).hexdigest()
    out = run(image, "boot", ok=(1,)).stdout
    check("NOT_FAT32" in out and hashlib.sha256(open(image, "rb").read()).hexdigest() == before,
          f"{name}: a FAT16 volume is refused and not written")

    for size, reason in ((20_000, "CHANGED"), (32_768, "CHANGED")):
        image = os.path.join(tmp, "foreign.img")
        make_image(image, 1100, 32)
        with Mounted(image) as mount:
            with open(os.path.join(mount, "DJMREC.TMP"), "wb") as f:
                f.write((b"somebody else's file" * 2000)[:size])
        before = hashlib.sha256(open(image, "rb").read()).hexdigest()
        out = run(image, "boot", ok=(1,)).stdout
        check(reason in out and hashlib.sha256(open(image, "rb").read()).hexdigest() == before,
              f"{name}: a file named DJMREC.TMP that is not the recorder's ({size} bytes) is left "
              f"alone, nothing written ({reason})")
        os.remove(image)

    image = os.path.join(tmp, "full.img")
    make_image(image, 36, 1)
    populate(image, [1_000_000] * 33)
    before = hashlib.sha256(open(image, "rb").read()).hexdigest()
    out = run(image, "--min", 8_000_000, "boot", ok=(1,)).stdout
    check("NO_SPACE" in out and hashlib.sha256(open(image, "rb").read()).hexdigest() == before,
          f"{name}: a stick without room is refused and not written")


CONTINUE = ["--cap", 17_281_024, "--min", 1_000_000, "--refill", 17_281_024, "--ahead", 5_760_000]


def test_continue(tmp):
    name = "take through several files"
    image = os.path.join(tmp, "continue.img")
    make_image(image, 1100, 32)
    out = run(image, *CONTINUE, "boot", "take:200", "take:100").stdout
    v = Volume(image)
    clean, text = fsck(image)
    files = v.root_files()
    recs = sorted(n for n in files if n.startswith("REC"))
    check(clean and not v.audit(), f"{name}: checker and audit find nothing")
    check(recs == ["REC000%d WAV" % n for n in range(1, 6)]
          and all(recording_ok(v, n, int(n[3:7])) for n in recs),
          f"{name}: five files, each with its own audio from its first to its last byte")
    first = sum(files[n][2] - 512 for n in recs[:3])
    second = sum(files[n][2] - 512 for n in recs[3:])
    check(first == 200 * RATE and second == 100 * RATE and "switched 3" in out,
          f"{name}: the 200 s take is in files 1 to 3 and the 100 s take in files 4 and 5, to the byte")
    w = waiting(v)
    check(w is not None and len(files) == 6,
          f"{name}: one waiting file left; the part cut off the last file was given back")

    name = "power cut after the next file was made"
    image = os.path.join(tmp, "cutnext.img")
    make_image(image, 1100, 32)
    out = run(image, *CONTINUE, "boot", "cut:80").stdout
    v = Volume(image)
    check("next file state 7" in out and "DJMREC  TMP" in v.root_files()
          and "REC0001 WAV" in v.root_files() and not harmful(v.audit()),
          f"{name}: an open recording and a waiting file exist side by side; volume sound")
    out = run(image, *CONTINUE, "boot", "take:61").stdout
    v = Volume(image)
    clean, text = fsck(image)
    check(clean and not v.audit() and "recovered 1" in out and "reused 1" in out,
          f"{name}: at the next start the recording is closed, what it did not use is given "
          f"back, and the waiting file is used; checker and audit clean")
    check(recording_ok(v, "REC0001 WAV", 1, 75) and recording_ok(v, "REC0002 WAV", 2, 61),
          f"{name}: REC0001.WAV keeps the 75 s its last header covered; REC0002.WAV is the new take")


def test_full_size(tmp):
    full = ["--noaudio", "--ahead", 115_200_000]
    for spc in (8, 32, 64):
        name = f"full-size files, {spc * 512} byte clusters"
        image = os.path.join(tmp, f"full{spc}.img")
        make_image(image, 8192, spc)
        out = run(image, *full, "boot", "take:26000", "take:100").stdout
        v = Volume(image)
        clean, text = fsck(image)
        files = v.root_files()
        recs = sorted(n for n in files if n.startswith("REC"))
        cap = 2_073_666_048 // v.cb * v.cb
        check(clean and not v.audit(), f"{name}: checker and audit find nothing")
        check(recs == ["REC000%d WAV" % n for n in range(1, 5)]
              and files[recs[0]][2] == cap and files[recs[1]][2] == cap and "switched 2" in out,
              f"{name}: a take of 7 h 13 min fills two files of {cap} bytes and ends in a third")
        check(cap >= 512 + 90_000 * 23_040 and cap < 1 << 31,
              f"{name}: such a file has room for 3 h of audio and stays {((1 << 31) - cap) / 1e6:.0f} MB under 2 GB")
        check(sum(files[n][2] - 512 for n in recs[:3]) == 26000 * RATE
              and files[recs[3]][2] - 512 == 100 * RATE,
              f"{name}: the three files hold the take to the byte; the next take is in the fourth")
        w = waiting(v)
        check(w is not None and len(files) == 5, f"{name}: one waiting file left")
        run(image, *full, "boot", "cut:12000")
        out = run(image, *full, "boot", "take:61").stdout
        v = Volume(image)
        clean, text = fsck(image)
        check(clean and not v.audit() and "recovered 1" in out,
              f"{name}: power cut in the second file of a take: closed at the next start, "
              f"checker and audit clean")
        os.remove(image)


def crash_points(tmp, name, base, before, options, script, recover_min_bytes):
    full = os.path.join(tmp, "crash_full.img")
    shutil.copyfile(base, full)
    total = int(run(full, *options, *script).stdout.split("io: ")[1].split(" reads, ")[1].split(" writes")[0])
    clean, _ = fsck(full)
    check(clean and not Volume(full).audit() and total > 20,
          f"{name}: reference run without a cut is clean ({total} bookkeeping writes)")
    log, recover_bad = CutLog(), []
    work = os.path.join(tmp, "crash_work.img")
    for limit in range(total):
        shutil.copyfile(base, work)
        run(work, *options, "--limit", limit, *script, ok=(3,))
        log.judge(tmp, work, before, options, limit)
        result = subprocess.run([NATIVE, work, *[str(o) for o in options], "boot"],
                                capture_output=True, text=True)
        if harmful_after_cut(work) or Volume(work).hashes() != before or result.returncode not in (0, 1):
            recover_bad.append((limit, "after a plain start: " + result.stdout[-200:], harmful_after_cut(work)))
        result = subprocess.run([NATIVE, work, *[str(o) for o in options], "boot", "take:61"],
                                capture_output=True, text=True)
        v = Volume(work)
        if harmful_after_cut(work) or v.hashes() != before or result.returncode not in (0, 1):
            recover_bad.append((limit, result.stdout[-300:], harmful_after_cut(work)))
        elif result.returncode == 0:
            recs = sorted(n for n in v.root_files() if n.startswith("REC"))
            newest = recs[-1]
            if not all(recording_ok(v, n, int(n[3:7])) for n in recs[-2:]) \
                    or sum(v.root_files()[n][2] for n in recs[-2:]) < recover_min_bytes:
                recover_bad.append((limit, "take after the cut is not a correct recording", [newest]))
    log.report(name, total, expect_hand_over=True)
    check(not recover_bad, f"{name}: started again after every cut, the recorder records or declines "
          f"cleanly without harming anything" + (f" FAILED at {recover_bad[:2]}" if recover_bad else ""))
    print("     leftovers seen after cuts:", {k.rstrip(":"): n for k, n in sorted(log.worst.items())})


def test_power_cut_continue(tmp):
    name = "power cut in the bookkeeping of a long take"
    base = os.path.join(tmp, "crash_base.img")
    make_image(base, 300, 8)
    populate(base, [900_000] * 40, delete_every=4, seed=11)
    before = Volume(base).hashes()
    options = ["--cap", 17_281_024, "--min", 1_000_000, "--refill", 17_281_024,
               "--ahead", 5_760_000, "--margin", 1_000_000]
    crash_points(tmp, name, base, before, options, ["boot", "take:200", "take:65"], 61 * RATE)


def test_power_cut_bookkeeping(tmp):
    name = "power cut in the bookkeeping"
    base = os.path.join(tmp, "crash_base.img")
    make_image(base, 36, 1)
    populate(base, [90_000] * 150, delete_every=3, seed=7)
    before = Volume(base).hashes()
    options = ["--cap", 6_000_000, "--min", 500_000, "--refill", 5_000_000, "--margin", 200_000]
    script = ["boot", "take:61", "take:62"]
    full = os.path.join(tmp, "crash_full.img")
    shutil.copyfile(base, full)
    total = int(run(full, *options, *script).stdout.split("io: ")[1].split(" reads, ")[1].split(" writes")[0])
    clean, _ = fsck(full)
    check(clean and not Volume(full).audit() and total > 20,
          f"{name}: reference run without a cut is clean ({total} bookkeeping writes)")
    log, recover_bad = CutLog(), []
    work = os.path.join(tmp, "crash_work.img")
    for limit in range(total):
        shutil.copyfile(base, work)
        run(work, *options, "--limit", limit, *script, ok=(3,))
        log.judge(tmp, work, before, options, limit)
        result = subprocess.run([NATIVE, work, *[str(o) for o in options], "boot"],
                                capture_output=True, text=True)
        if harmful_after_cut(work) or Volume(work).hashes() != before or result.returncode not in (0, 1):
            recover_bad.append((limit, "after a plain start: " + result.stdout[-200:], harmful_after_cut(work)))
        result = subprocess.run([NATIVE, work, *[str(o) for o in options], "boot", "take:61"],
                                capture_output=True, text=True)
        v = Volume(work)
        if harmful_after_cut(work) or v.hashes() != before or result.returncode not in (0, 1):
            recover_bad.append((limit, result.stdout[-300:], harmful_after_cut(work)))
        elif result.returncode == 0:
            newest = sorted(n for n in v.root_files() if n.startswith("REC"))[-1]
            if not recording_ok(v, newest, int(newest[3:7])) or v.root_files()[newest][2] < 5_000_000:
                recover_bad.append((limit, "take after the cut is not a correct recording", []))
    log.report(name, total)
    check(not recover_bad, f"{name}: started again after every cut, the recorder records or declines "
          f"cleanly without harming anything" + (f" FAILED at {recover_bad[:2]}" if recover_bad else ""))
    print("     leftovers seen after cuts:", {k.rstrip(":"): n for k, n in sorted(log.worst.items())})


def main():
    build()
    tests = [test_fresh, test_refill_and_full, test_fragmented, test_cluster_sizes, test_full_size,
             test_power_cut_recording, test_refusals, test_continue, test_power_cut_bookkeeping,
             test_power_cut_continue]
    only = os.environ.get("TESTS")
    with tempfile.TemporaryDirectory(prefix="fatrec") as tmp:
        for test in tests:
            if only and test.__name__ not in only.split(","):
                continue
            try:
                test(tmp)
            except Exception as error:
                check(False, f"{test.__name__}: {type(error).__name__}: {str(error)[:600]}")
            for leftover in os.listdir(tmp):
                os.remove(os.path.join(tmp, leftover))
    print(f"\n{len(failed)} failed" if failed else "\nall checks passed")
    sys.exit(1 if failed else 0)


if __name__ == "__main__":
    main()
