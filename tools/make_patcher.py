#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Make the two patchers, patcher/djm900nxs2_plus_patcher.py and .html,
from the official 2.08 file and the build.

The patchers hold only the bytes the build changes, where they go, and the
SHA-256 of the official file and of the build. Each one is tried here on
the official file before it is written.

    python3 tools/make_patcher.py [--build FILE] [--copyright-holder NAME]
"""
import argparse
import base64
import hashlib
import json
import os
import subprocess
import sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(ROOT, "tools"))
import djmfw  # noqa: E402

STOCK = os.path.join(ROOT, "firmware", "original", "DJM-900NXS2_208.upd")
STOCK_SHA256 = "482817cfe5984894db8e198eba8d3dd33e226d523ac5e1555bbccb5cc6c08538"
TAG = "092"
VERSION = "0.92"
MOD_NAME = f"DJM-900NXS2_{TAG}.upd"
BUILDS = [os.path.join(ROOT, "build", MOD_NAME), os.path.join(ROOT, "rec_control", "build", "package", MOD_NAME)]
OUT = os.path.join(ROOT, "patcher")
NAME = "djm900nxs2_plus_patcher"
COPYRIGHT_HOLDER = "low trust society"

NOTICE = {
    "warning": "UNOFFICIAL AND EXPERIMENTAL. UPDATING FIRMWARE IS AT YOUR OWN RISK. "
               "IT CAN LEAVE A MIXER UNUSABLE. THERE IS NO WARRANTY OF ANY KIND, AND NO ONE WHO MADE OR SHARED IT "
               "IS RESPONSIBLE FOR ANY DAMAGE OR LOSS.",
    # The README's "Read this first", word for word.
    "points": [
        "This is not a Pioneer DJ or AlphaTheta product. They have nothing to do with it, do not support it, "
        "and may refuse service on a modified mixer.",
        "It is free software under the GNU General Public License version 3, provided as is, with no warranty "
        "of any kind.",
        "You use it entirely at your own risk. In no case is anyone who made or shared it responsible for any "
        "damage or loss, to your mixer, your media, your recordings, your gig or anything else.",
        "It has been tested at length on the author's mixer, with recordings of up to 10 hours. That is not "
        "proof that it works the same on yours. Treat it as experimental, and do not rely on it for a show "
        "before you have tried it yourself at home.",
        "A failed or interrupted update can leave a mixer unusable, as with any firmware update. Never switch "
        "the mixer off during an update.",
        "The boot and update code of every chip stays as Pioneer made it, so the mixer's own USB update can "
        "always put the official firmware back.",
        "Keep your official 2.08 UPD file, in case you want to go back to stock.",
        "Only for the DJM-900NXS2, and only from your own official DJM-900NXS2 firmware version 2.08 UPD file.",
    ],
}
# The README's "Good to know", word for word.
KNOWN_ISSUES = [
    "24 bit is experimental: how well it works depends on the stick (see 16 bit or 24 bit in the README).",
    "The stick must be FAT32. Other formats show USB ERROR.",
    "One TAP on STOP ends the recording.",
    "Every file is dated 1 January 2026: the mixer has no clock.",
    "The recorder keeps a file of about 2 GB, DJMREC.TMP, on the stick so that a recording starts at once. "
    "Leave it there.",
]
UPDATE_TEXT = ("with the mixer switched off, put the stick in. hold BEAT FX ON/OFF and MID (FX FREQUENCY), "
               "switch the mixer on, and keep holding until the update screen appears. it shows the new "
               "version, " + VERSION + ". wait for COMPLETE, then switch the mixer off and on again. "
               "DO NOT SWITCH IT OFF before COMPLETE.")


def changes(stock_raw, mod_raw):
    """For each segment the build changes: (offset, our bytes) for every run of changed bytes."""
    (stock_top, stock_segments), (mod_top, mod_segments) = djmfw.parse(stock_raw), djmfw.parse(mod_raw)
    if stock_top[:20] != mod_top[:20] or stock_top[23:] != mod_top[23:] or djmfw.top_version(mod_top) != TAG:
        sys.exit("the build's top header is not the official one with the version " + TAG)
    out = {}
    for (stock_header, stock_data), (mod_header, mod_data) in zip(stock_segments, mod_segments):
        if stock_header != mod_header or len(stock_data) != len(mod_data):
            sys.exit("the build's segments do not match the official file's")
        runs, i, n = [], 0, len(stock_data)
        while i < n:
            if stock_data[i] != mod_data[i]:
                j = i
                while j < n and stock_data[j] != mod_data[j]:
                    j += 1
                runs.append([i, base64.b64encode(mod_data[i:j]).decode("ascii")])
                i = j
            else:
                i += 1
        if runs:
            out[djmfw.seg_name(stock_header)] = runs
    return out


def font_css():
    fonts = os.path.join(ROOT, "tools", "fonts")
    css = []
    for weight, name in ((400, "Regular"), (500, "Medium"), (700, "Bold")):
        with open(os.path.join(fonts, f"IoskeleyMono-{name}.woff2"), "rb") as f:
            encoded = base64.b64encode(f.read()).decode("ascii")
        css.append('@font-face { font-family: "IoskeleyMono"; font-style: normal; font-weight: %d; '
                   'font-display: swap; src: url(data:font/woff2;base64,%s) format("woff2"); }' % (weight, encoded))
    return "\n".join(css)


def read(path):
    with open(path, "rb") as f:
        return f.read()


def main():
    parser = argparse.ArgumentParser(description=__doc__.splitlines()[0])
    parser.add_argument("--build", help="the build (default: build/ or rec_control/build/package/)")
    parser.add_argument("--copyright-holder", default=COPYRIGHT_HOLDER)
    parser.add_argument("--out", default=OUT)
    args = parser.parse_args()

    stock = read(STOCK)
    if hashlib.sha256(stock).hexdigest() != STOCK_SHA256:
        sys.exit(f"{STOCK} is not the official 2.08 file")
    build_path = args.build or next((p for p in BUILDS if os.path.exists(p)), None)
    if not build_path:
        sys.exit("no build found: build it first")
    mod = read(build_path)
    data = {
        "build": "PLUS V" + VERSION,
        "tag": TAG,
        "mod_name": MOD_NAME,
        "stock_sha256": STOCK_SHA256,
        "mod_sha256": hashlib.sha256(mod).hexdigest(),
        "changes": changes(stock, mod),
        "notice": NOTICE,
        "known_issues": KNOWN_ISSUES,
        "update_text": UPDATE_TEXT,
    }
    blob = json.dumps(data, separators=(",", ":"))
    os.makedirs(args.out, exist_ok=True)

    with open(os.path.join(ROOT, "tools", "patcher_template.py")) as f:
        template = f.read()
    if template.count("\nDATA = None\n") != 1:
        sys.exit("patcher_template.py has no DATA line")
    python = template.replace("\nDATA = None\n", "\nDATA = " + blob + "\n").replace(
        "low trust society", args.copyright_holder)
    py_path = os.path.join(args.out, NAME + ".py")
    with open(py_path, "w") as f:
        f.write(python)
    os.chmod(py_path, 0o755)

    with open(os.path.join(ROOT, "tools", "web_patcher_template.html")) as f:
        page = f.read()
    with open(os.path.join(ROOT, "tools", "web_patcher_core.js")) as f:
        core = f.read()
    for marker in ("/*FONTS*/", "/*CORE*/", "/*PATCH_DATA*/null"):
        if page.count(marker) != 1:
            sys.exit(f"web_patcher_template.html: {marker} must appear once")
    page = page.replace("/*FONTS*/", font_css()).replace("/*CORE*/", core).replace(
        "/*PATCH_DATA*/null", blob.replace("</", "<\\/"))
    html_path = os.path.join(args.out, NAME + ".html")
    with open(html_path, "w") as f:
        f.write(page)

    # Both patchers, tried on the official file.
    probe = subprocess.run([sys.executable, "-c", (
        "import importlib.util, sys\n"
        f"spec = importlib.util.spec_from_file_location('p', {py_path!r})\n"
        "p = importlib.util.module_from_spec(spec); spec.loader.exec_module(p)\n"
        f"mod = p.build_mod(open({STOCK!r}, 'rb').read())\n"
        "import hashlib; print(hashlib.sha256(mod).hexdigest())\n")], capture_output=True, text=True)
    if probe.stdout.strip() != data["mod_sha256"]:
        sys.exit("the Python patcher does not make the build:\n" + probe.stdout + probe.stderr)
    print(f"{py_path}: makes {MOD_NAME}, sha-256 {data['mod_sha256']}")
    print(f"{html_path}: {len(page) // 1024} KB")
    print(f"changed bytes: " + ", ".join(
        f"{name} {sum(len(base64.b64decode(r[1])) for r in runs)} in {len(runs)} runs"
        for name, runs in data["changes"].items()))


if __name__ == "__main__":
    main()
