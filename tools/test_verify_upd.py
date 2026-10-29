#!/usr/bin/env python3
"""Tests of verify_upd.py.

Copyright (C) 2026 low trust society
SPDX-License-Identifier: GPL-3.0-only
"""

import contextlib
import io
import os
import tempfile
import unittest

import djmfw
import verify_upd


class RecorderProfileTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(verify_upd.STOCK, "rb") as stock_file:
            raw = stock_file.read()
        cls.top, cls.segs = djmfw.parse(raw)

    def make_update(self, directory, mutation=None):
        segs = list(self.segs)
        if mutation is not None:
            wanted_name, offset = mutation
            changed = False
            output = []
            for header, data in segs:
                if djmfw.seg_name(header) == wanted_name:
                    altered = bytearray(data)
                    altered[offset] ^= 1
                    data = bytes(altered)
                    changed = True
                output.append((header, data))
            self.assertTrue(changed, f"segment {wanted_name!r} was not found")
            segs = output

        path = os.path.join(directory, "test.upd")
        tagged_top = djmfw.set_top_version(self.top, "299")
        with open(path, "wb") as output_file:
            output_file.write(djmfw.build(tagged_top, segs))
        return path

    def verify_quietly(self, path):
        with contextlib.redirect_stdout(io.StringIO()):
            return verify_upd.verify(path, "recorder")

    def test_tagged_stock_passes(self):
        with tempfile.TemporaryDirectory() as directory:
            self.assertTrue(self.verify_quietly(self.make_update(directory)))

    def test_recorder_regions_pass(self):
        allowed = [
            ("sh", 0xCAA58),
            ("sh", 0xCAA5B),
            ("sh", 0xEE284),
            ("sh", 0xEE287),
            ("sh", 0xF8000),
            ("sh", 0xFFFFF),
        ]
        for mutation in allowed:
            with self.subTest(mutation=mutation):
                with tempfile.TemporaryDirectory() as directory:
                    path = self.make_update(directory, mutation)
                    self.assertTrue(self.verify_quietly(path))

    def test_everything_else_fails(self):
        denied = [
            ("sh", 0xEE270),
            ("panel", 0),
            ("sh", 0x100),
            ("sh", 0xCAA54),
            ("sh", 0xCAA5C),
            ("sh", 0xEE288),
            ("sh", 0xF7FFF),
            ("main", 0),
            ("dsp", 0),
            ("sdsp", 0),
        ]
        for mutation in denied:
            with self.subTest(mutation=mutation):
                with tempfile.TemporaryDirectory() as directory:
                    path = self.make_update(directory, mutation)
                    self.assertFalse(self.verify_quietly(path))


if __name__ == "__main__":
    unittest.main()
