#!/usr/bin/env python3
"""CI helper: download one of the launcher's pinned archives (checked against its sha256) and
unpack it. The same pins the launcher uses.   ci_fetch.py NAME DEST [--strip N]"""
import argparse
import sys
import tempfile
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from forza_launcher import fetch  # noqa: E402

ap = argparse.ArgumentParser()
ap.add_argument("name")
ap.add_argument("dest")
ap.add_argument("--strip", type=int, default=0)
a = ap.parse_args()
cache = Path(tempfile.gettempdir()) / "forza-ci-downloads"
archive = fetch.download(a.name, cache, lambda d, t: None)
fetch.unpack(a.name, archive, Path(a.dest), strip=a.strip)
print(f"{a.name} -> {a.dest}")
