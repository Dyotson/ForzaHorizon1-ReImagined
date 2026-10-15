"""Visual Studio selection (issue #37): python3 -m unittest launcher/test_msvc.py"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import build_env as be   # noqa: E402

VSWHERE = json.dumps([
    {"installationPath": r"C:\VS\2017\Professional", "installationVersion": "15.9.28307.1000",
     "displayName": "Visual Studio Professional 2017", "catalog": {"productDisplayVersion": "15.9.36"}},
    {"installationPath": r"C:\VS\2022\BuildTools", "installationVersion": "17.14.36811.4",
     "displayName": "Visual Studio Build Tools 2022", "catalog": {"productDisplayVersion": "17.14.5"}},
    {"installationPath": r"C:\VS\2019\Community", "installationVersion": "16.11.40",
     "displayName": "Visual Studio Community 2019"},
    {"installationPath": r"C:\VS\2022\Old", "installationVersion": "17.2.5",
     "displayName": "Visual Studio Community 2022", "catalog": {"productDisplayVersion": "17.2.5"}},
])


class Fake:
    """A toolset on disk: include dirs holding the named headers."""

    def __init__(self, tmp, headers, tools_version, windows_h=True):
        self.inc = Path(tmp) / f"inc{tools_version}"
        self.inc.mkdir(parents=True)
        for h in headers + (["windows.h"] if windows_h else []):
            (self.inc / h).write_text("")
        self.env = {"INCLUDE": str(self.inc), "LIB": "x", "VCToolsVersion": tools_version}


class MsvcSelection(unittest.TestCase):
    def test_parse_sorted_newest_first(self):
        v = be.parse_vswhere(VSWHERE)
        self.assertEqual([x.version[:2] for x in v], [(17, 14), (17, 2), (16, 11), (15, 9)])
        self.assertIn("2022", v[0].name)

    def test_garbage_vswhere(self):
        self.assertEqual(be.parse_vswhere(""), [])
        self.assertEqual(be.parse_vswhere("not json"), [])

    def test_2017_only_falls_back(self):
        inst = be.parse_vswhere(json.dumps([json.loads(VSWHERE)[0]]))
        logs = []
        env, path = be.find_msvc(logs.append, inst, load=lambda p: self.fail("vcvars must not run for VS 2017"))
        self.assertIsNone(env)
        self.assertTrue(any("skipping" in l and "2017" in l and "<expected>" in l for l in logs), logs)
        self.assertIn("downloaded instead", logs[-1])

    def test_picks_newest_compatible(self):
        with tempfile.TemporaryDirectory() as tmp:
            new = Fake(tmp, ["expected", "span", "bit"], "14.44.35207")
            logs = []
            env, path = be.find_msvc(logs.append, be.parse_vswhere(VSWHERE), load=lambda p: new.env)
            self.assertEqual(path, r"C:\VS\2022\BuildTools")
            self.assertIn("2017", logs[0])        # the list of what was found
            self.assertEqual(len(logs), 2)        # newest first: the older ones are never tried
            self.assertIn("using Visual Studio Build Tools 2022 17.14.5", logs[-1])

    def test_newest_unusable_goes_on_to_older(self):
        with tempfile.TemporaryDirectory() as tmp:
            good = Fake(tmp, ["expected", "span", "bit"], "14.38.1")
            envs = {r"C:\VS\2022\BuildTools": {}, r"C:\VS\2022\Old": good.env}
            inst = be.parse_vswhere(VSWHERE)
            inst[1].version = (17, 8)                    # an older but compatible 2022
            logs = []
            env, path = be.find_msvc(logs.append, inst, load=lambda p: envs.get(p, {}))
            self.assertEqual(path, r"C:\VS\2022\Old")
            self.assertTrue(any("skipping" in l and "BuildTools" in l and "INCLUDE" in l for l in logs), logs)

    def test_old_toolset_inside_2022(self):
        # a 2022 install that only holds the 2019 toolset (14.29): version says yes, the toolset says no
        with tempfile.TemporaryDirectory() as tmp:
            old = Fake(tmp, ["span", "bit"], "14.29.30133")
            logs = []
            env, _ = be.find_msvc(logs.append, be.parse_vswhere(VSWHERE)[:1], load=lambda p: old.env)
            self.assertIsNone(env)
            self.assertTrue(any("14.29" in l for l in logs), logs)

    def test_missing_header_and_sdk(self):
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn("<expected>", be.msvc_env_problem(Fake(tmp, ["span", "bit"], "14.40.1").env))
        with tempfile.TemporaryDirectory() as tmp:
            self.assertIn("Windows SDK", be.msvc_env_problem(Fake(tmp, ["expected", "span", "bit"], "14.40.1", False).env))
        self.assertIn("no INCLUDE", be.msvc_env_problem({}))

    def test_no_vs_at_all(self):
        logs = []
        self.assertEqual(be.find_msvc(logs.append, []), (None, ""))
        self.assertIn("no Visual Studio", logs[0])


if __name__ == "__main__":
    unittest.main()
