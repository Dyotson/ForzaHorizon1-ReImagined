"""The CPU the game is built for (#220): x86-64-v3 detection, the build's CPU target, the rebuild it asks for
and the message for a build made for a newer CPU. Run: python3 -m unittest launcher/test_cpu_target.py"""
import json
import os
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import builder, game, specs  # noqa: E402

ZEN = "processor\t: 0\nmodel name\t: AMD Custom APU 0405\nflags\t\t: fpu sse sse2 pni ssse3 fma cx16 sse4_1 " \
      "sse4_2 movbe popcnt xsave avx f16c lahf_lm abm bmi1 avx2 bmi2\n\n"
IVY = "processor\t: 0\nmodel name\t: Intel(R) Xeon(R) CPU E5-2670 v2\nflags\t\t: fpu sse sse2 pni ssse3 cx16 " \
      "sse4_1 sse4_2 popcnt xsave avx f16c lahf_lm\n\n"


def cpuid(leaf1_ecx=None, leaf7_ebx=None, ext_ecx=None, xcr0=0x7, max_leaf=0xD):
    """Fake CPUID/XGETBV of a Haswell / Zen class CPU, with the words given replaced."""
    ecx1 = sum(1 << b for b in (0, 9, 12, 13, 19, 20, 22, 23, 26, 27, 28, 29)) if leaf1_ecx is None else leaf1_ecx
    ebx7 = sum(1 << b for b in (3, 5, 8)) if leaf7_ebx is None else leaf7_ebx
    ext = 0b100001 if ext_ecx is None else ext_ecx
    table = {0: (max_leaf, 0, 0, 0), 1: (0, 0, ecx1, 0), 7: (0, ebx7, 0, 0),
             0x80000000: (0x80000008, 0, 0, 0), 0x80000001: (0, 0, ext, 0)}
    calls = []

    def xgetbv(index):
        calls.append(index)
        return xcr0
    return (lambda leaf, sub: table.get(leaf, (0, 0, 0, 0))), xgetbv, calls


class Detection(unittest.TestCase):
    def test_cpuinfo_flags(self):
        self.assertEqual(specs.missing_v3_flags(ZEN), [])
        self.assertEqual(specs.missing_v3_flags(IVY), ["avx2", "bmi1", "bmi2", "fma", "movbe", "abm"])
        self.assertIsNone(specs.missing_v3_flags("processor : 0\nFeatures : fp asimd\n"))   # an ARM CPU
        self.assertIsNone(specs.missing_v3_flags(""))

    def test_cpuid(self):
        regs, xgetbv, _ = cpuid()
        self.assertTrue(specs.v3_from_cpuid(regs, xgetbv))
        regs, xgetbv, _ = cpuid(leaf1_ecx=cpuid()[0](1, 0)[2] & ~(1 << 22))   # a virtual CPU without MOVBE
        self.assertFalse(specs.v3_from_cpuid(regs, xgetbv))
        regs, xgetbv, _ = cpuid(leaf7_ebx=1 << 5)                            # AVX2 without BMI1/2
        self.assertFalse(specs.v3_from_cpuid(regs, xgetbv))
        regs, xgetbv, _ = cpuid(ext_ecx=1)                                   # no LZCNT
        self.assertFalse(specs.v3_from_cpuid(regs, xgetbv))
        regs, xgetbv, _ = cpuid(max_leaf=6)                                  # leaf 7 does not exist
        self.assertFalse(specs.v3_from_cpuid(regs, xgetbv))

    def test_the_os_must_save_the_avx_registers(self):
        regs, xgetbv, calls = cpuid(xcr0=0x3)
        self.assertFalse(specs.v3_from_cpuid(regs, xgetbv))
        regs, xgetbv, calls = cpuid(leaf1_ecx=cpuid()[0](1, 0)[2] & ~(1 << 27))   # no OSXSAVE: XGETBV would fault
        self.assertFalse(specs.v3_from_cpuid(regs, xgetbv))
        self.assertEqual(calls, [])

    def test_detection_never_raises_and_is_asked_once(self):
        with mock.patch.object(specs, "_V3", []), mock.patch.object(specs, "IS_WINDOWS", False), \
                mock.patch.object(specs.gpu, "_read", side_effect=OSError("no /proc")) as read:
            self.assertIsNone(specs.x86_64_v3())
            self.assertIsNone(specs.x86_64_v3())
            self.assertEqual(read.call_count, 1)
        with mock.patch.object(specs, "_V3", []), mock.patch.object(specs, "IS_WINDOWS", False), \
                mock.patch.object(specs.gpu, "_read", return_value=ZEN):
            self.assertTrue(specs.x86_64_v3())
        with mock.patch.object(specs, "_V3", []), mock.patch.object(specs, "IS_WINDOWS", False), \
                mock.patch.object(specs.gpu, "_read", return_value=IVY):
            self.assertFalse(specs.x86_64_v3())


def env_without_override():
    return {k: v for k, v in os.environ.items() if k != "FORZA_CPU_TARGET"}


class Target(unittest.TestCase):
    def test_v3_only_when_the_cpu_surely_has_it(self):
        with mock.patch.dict(os.environ, env_without_override(), clear=True):
            for v3, want in ((True, builder.CPU_V3), (False, builder.CPU_BASELINE), (None, builder.CPU_BASELINE)):
                with mock.patch.object(specs, "x86_64_v3", return_value=v3):
                    self.assertEqual(builder.cpu_target(), want)

    def test_environment_overrides(self):
        with mock.patch.object(specs, "x86_64_v3", return_value=True):
            with mock.patch.dict(os.environ, {"FORZA_CPU_TARGET": "baseline"}):
                self.assertEqual(builder.cpu_target(), builder.CPU_BASELINE)
            with mock.patch.dict(os.environ, {"FORZA_CPU_TARGET": "nonsense"}):
                self.assertEqual(builder.cpu_target(), builder.CPU_V3)
        with mock.patch.object(specs, "x86_64_v3", return_value=False), \
                mock.patch.dict(os.environ, {"FORZA_CPU_TARGET": "X86-64-V3"}):
            self.assertEqual(builder.cpu_target(), builder.CPU_V3)


class Status(unittest.TestCase):
    """A data folder built on one PC and used on another (or a new CPU) asks for a rebuild."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.root = Path(self.tmp.name)
        rt = builder.runtime_dir(self.root)
        rt.mkdir(parents=True)
        (rt / ("forza" + builder.EXE)).write_text("")
        patches = [mock.patch.object(builder, "project_dir", return_value=self.root),
                   mock.patch.object(builder, "sources_hash", return_value="sources"),
                   mock.patch.dict(os.environ, env_without_override(), clear=True)]
        for p in patches:
            p.start()
            self.addCleanup(p.stop)

    def tearDown(self):
        self.tmp.cleanup()

    def built_on(self, v3: bool):
        with mock.patch.object(specs, "x86_64_v3", return_value=v3):
            builder.write_state(self.root, {"complete": True, "id": builder.current_build_id(self.root),
                                            "cpu_target": builder.cpu_target(), "when": "today"})

    def status_on(self, v3):
        with mock.patch.object(specs, "x86_64_v3", return_value=v3):
            return builder.build_status(self.root)

    def test_same_cpu_is_built(self):
        self.built_on(True)
        self.assertEqual(self.status_on(True)[0], "built")
        self.built_on(False)
        self.assertEqual(self.status_on(False)[0], "built")
        self.assertEqual(self.status_on(None)[0], "built")   # unknown CPU: baseline, as built

    def test_v3_build_on_an_older_cpu(self):
        self.built_on(True)
        self.assertEqual(self.status_on(False), ("stale", builder.CPU_TOO_OLD))
        with mock.patch.object(specs, "x86_64_v3", return_value=False):
            self.assertEqual(builder.cpu_blocker(self.root), builder.CPU_TOO_OLD)
            self.assertEqual(builder.cpu_blocker(self.root, builder.runtime_dir(self.root)), builder.CPU_TOO_OLD)
            self.assertEqual(builder.cpu_blocker(self.root, self.root / "elsewhere"), "")   # not the launcher's build
        with mock.patch.object(specs, "x86_64_v3", return_value=None):
            self.assertEqual(builder.cpu_blocker(self.root), "")   # the game checks again at start

    def test_baseline_build_on_a_v3_cpu_offers_the_faster_one(self):
        self.built_on(False)
        state, detail = self.status_on(True)
        self.assertEqual(state, "stale")
        self.assertIn("faster", detail)
        with mock.patch.object(specs, "x86_64_v3", return_value=True):
            self.assertEqual(builder.cpu_blocker(self.root), "")

    def test_state_of_an_older_launcher_counts_as_baseline(self):
        builder.write_state(self.root, {"complete": True, "id": "old", "when": "today"})
        self.assertIn("faster", self.status_on(True)[1])
        self.assertEqual(self.status_on(False), ("stale", "A new version of the project needs a rebuild."))

    def test_the_state_records_the_target(self):
        self.built_on(True)
        st = json.loads((builder.build_dir(self.root) / "state.json").read_text())
        self.assertEqual(st["cpu_target"], "x86-64-v3")


class Exit(unittest.TestCase):
    def test_the_games_cpu_exit_code_explains_itself(self):
        self.assertEqual(game.CPU_EXIT_CODE, 76)
        self.assertEqual(game.crash_reason(None, game.CPU_EXIT_CODE), builder.CPU_TOO_OLD)
        self.assertIn("Setup > Build", builder.CPU_TOO_OLD)
        self.assertNotEqual(game.CPU_EXIT_CODE, game.RELAUNCH_EXIT_CODE)


if __name__ == "__main__":
    unittest.main()
