"""Tests of the build tree handling (issue #35): files with a modification time in the future are
set to now, and a CMake build tree made by another launcher version is deleted
(python3 -m unittest launcher/test_build_tree.py from the repository root)."""
import os
import sys
import tempfile
import time
import unittest
from pathlib import Path

HERE = Path(__file__).resolve().parent
sys.path.insert(0, str(HERE))

from forza_launcher import builder  # noqa: E402


class Quiet(builder.Reporter):
    def __init__(self):
        self.lines = []

    def log(self, text):
        self.lines.append(text)


class Tools:
    cc = cxx = cmake = ninja = "x"
    source = {"cmake": "downloaded"}


class ClampTest(unittest.TestCase):
    def test_future_times_are_set_to_now(self):
        with tempfile.TemporaryDirectory() as d:
            d = Path(d)
            (d / "sub").mkdir()
            old, fut, fdir = d / "old.cmake", d / "sub" / "future.cmake", d / "sub"
            old.write_text("a")
            fut.write_text("b")
            t = time.time()
            os.utime(old, (t - 3600, t - 3600))
            os.utime(fut, (t + 7 * 3600, t + 7 * 3600))
            os.utime(fdir, (t + 3600, t + 3600))
            n = builder.clamp_future_times([d], now=t)
            self.assertEqual(n, 2)
            self.assertLessEqual(fut.stat().st_mtime, t + 1)
            self.assertLessEqual(fdir.stat().st_mtime, t + 1)
            self.assertAlmostEqual(old.stat().st_mtime, t - 3600, delta=1)   # the past is left alone
            self.assertEqual(builder.clamp_future_times([d, d / "missing"], now=t), 0)


class StampTest(unittest.TestCase):
    def test_tree_of_another_stamp_is_deleted(self):
        with tempfile.TemporaryDirectory() as d:
            b = Path(d) / "out" / "build" / "x"
            (b / "CMakeFiles").mkdir(parents=True)
            (b / "build.ninja").write_text("old")
            rep = Quiet()
            stamp = builder.configure_stamp(Tools, "sdk", False)
            builder.reset_stale_build_tree(b, stamp, rep)             # no stamp: made by an old launcher
            self.assertFalse(b.exists())
            b.mkdir(parents=True)
            builder.write_configure_stamp(b, stamp)
            (b / "build.ninja").write_text("new")
            builder.reset_stale_build_tree(b, stamp, rep)             # same stamp: resumed
            self.assertTrue((b / "build.ninja").is_file())
            builder.reset_stale_build_tree(b, builder.configure_stamp(Tools, "sdk2", False), rep)
            self.assertFalse(b.exists())
            builder.reset_stale_build_tree(b, stamp, rep)             # nothing there: nothing to do


if __name__ == "__main__":
    unittest.main()
