#!/usr/bin/env python3
# Copyright (C) 2026 low trust society
# SPDX-License-Identifier: GPL-3.0-only
"""Build DJM-900NXS2 PLUS from the official 2.08 file: build/DJM-900NXS2_092.upd."""
import hashlib
import os
import struct
import sys

ROOT = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(ROOT, "tools"))
sys.path.insert(0, os.path.join(ROOT, "rec_control"))
sys.path.insert(0, os.path.join(ROOT, "rec_control", "panel"))
import build_stream  # noqa: E402
import djmfw  # noqa: E402
import main_tunnel  # noqa: E402
import build_panel  # noqa: E402
import verify_upd  # noqa: E402

TAG = "092"
BUILD_DIR = os.path.join(ROOT, "build")


def read_file(path):
    with open(path, "rb") as input_file:
        return input_file.read()


def package(tag=TAG, output_dir=BUILD_DIR, variant="normal", tunnel=True, panel=True):
    if len(tag) != 3 or not tag.isdigit():
        sys.exit("tag must contain exactly three digits")
    _, binary_path, symbols = build_stream.build(variant)
    stock_raw = read_file(verify_upd.STOCK)
    if hashlib.sha256(stock_raw).hexdigest() != verify_upd.STOCK_SHA256:
        sys.exit("official v2.08 reference SHA-256 does not match")
    top, segments = djmfw.parse(stock_raw)
    code = read_file(binary_path)
    variables = read_file(os.path.join(os.path.dirname(binary_path), "rec_vars.bin"))
    if panel and not tunnel:
        sys.exit("the panel's indicator needs the tunnel in main")
    output_segments = []
    panel_code_size = 0
    for header, data in segments:
        if djmfw.seg_name(header) == "main" and tunnel:
            output_segments.append((header, main_tunnel.apply(data)))
            continue
        if djmfw.seg_name(header) == "panel" and panel:
            patched_panel, panel_code_size = build_panel.patch(data, tag)
            output_segments.append((header, patched_panel))
            continue
        if djmfw.seg_name(header) != "sh":
            output_segments.append((header, data))
            continue
        modified = bytearray(data)
        modified[
            build_stream.BACKGROUND_HOOK_OFFSET:
            build_stream.BACKGROUND_HOOK_OFFSET + 4
        ] = struct.pack(">I", build_stream.ENTRY_ADDRESS)
        modified[
            build_stream.SEND_HOOK_OFFSET:build_stream.SEND_HOOK_OFFSET + 4
        ] = struct.pack(">I", symbols["_stream_capture_isr"])
        modified[
            build_stream.CODE_FLASH_START:
            build_stream.CODE_FLASH_START + len(code)
        ] = code
        modified[
            build_stream.VARS_FLASH_START:
            build_stream.VARS_FLASH_START + len(variables)
        ] = variables
        output_segments.append((header, bytes(modified)))

    os.makedirs(output_dir, exist_ok=True)
    output_path = os.path.join(output_dir, f"DJM-900NXS2_{tag}.upd")
    packaged = djmfw.build(djmfw.set_top_version(top, tag), output_segments)
    with open(output_path, "wb") as output_file:
        output_file.write(packaged)
    if not verify_upd.verify(output_path, "status" if panel else "tunnel" if tunnel else "recorder"):
        os.remove(output_path)
        sys.exit("recorder verification failed, updater removed")

    _, packaged_segments = djmfw.parse(packaged)
    original = dict((djmfw.seg_name(h), d) for h, d in segments)
    result = dict((djmfw.seg_name(h), d) for h, d in packaged_segments)
    for name in original:
        if name == "main" and tunnel:
            # an own look, apart from verify_upd: only the patch's bytes, and exactly those
            differing = [i for i in range(len(original[name])) if result[name][i] != original[name][i]]
            inside = all(any(lo <= i < hi for lo, hi in main_tunnel.ALLOWED) for i in differing)
            exact = all(result[name][offset:offset + len(new)] == new
                        for offset, _, new in main_tunnel.changes())
            if not differing or not inside or not exact or len(result[name]) != len(original[name]):
                os.remove(output_path)
                sys.exit("main segment is not stock plus exactly the tunnel patch")
        elif name == "panel" and panel:
            differing = [i for i in range(len(original[name])) if result[name][i] != original[name][i]]
            inside = all(any(lo <= i + build_panel.PANEL_BASE < hi for lo, hi in build_panel.ALLOWED)
                         for i in differing)
            if not differing or not inside or len(result[name]) != len(original[name]) or \
                    result[name][0x30000:] != original[name][0x30000:]:
                os.remove(output_path)
                sys.exit("panel segment is not stock plus the indicator")
        elif name != "sh" and result[name] != original[name]:
            os.remove(output_path)
            sys.exit(f"unexpected difference in {name}")
    # The recorder's buffers must arrive erased (it checks that at its start): up to the
    # variables' piece the flash is stock; in that piece only the variables differ, and
    # what follows them is erased (the guard).
    if result["sh"][
        build_stream.AUDITED_BUFFER_FLASH_START:build_stream.VARS_FLASH_START
    ] != original["sh"][
        build_stream.AUDITED_BUFFER_FLASH_START:build_stream.VARS_FLASH_START
    ]:
        os.remove(output_path)
        sys.exit("package changed audited audio-buffer source bytes")
    piece = result["sh"][build_stream.VARS_FLASH_START:build_stream.VARS_FLASH_END]
    if piece[:len(variables)] != variables or piece[len(variables):] != b"\xff" * (len(piece) - len(variables)) \
            or result["sh"][build_stream.VARS_FLASH_END:] != original["sh"][build_stream.VARS_FLASH_END:]:
        os.remove(output_path)
        sys.exit("the variables' piece is not the variables followed by erased flash")
    with open(output_path, "rb") as f:
        digest = hashlib.sha256(f.read()).hexdigest()
    print(f"{output_path}")
    print(f"sha-256 {digest}")
    return output_path, digest


if __name__ == "__main__":
    package()
