import unittest

from forza_launcher import build_env

GB = build_env.GB


class MemoryCheckTest(unittest.TestCase):
    def test_estimate_covers_the_measured_peaks(self):
        # measured: 1.5 GB at -j3 (Windows), 2.0 GB at -j4, 3.2 GB at -j8 (Linux)
        for jobs, peak in ((3, 1.5), (4, 2.0), (8, 3.2)):
            need = build_env.ram_need_gb(jobs)
            self.assertGreaterEqual(need, peak)
            self.assertLess(need, peak * 1.3)

    def test_jobs_for_ram(self):
        self.assertEqual(build_env.jobs_for_ram(0.5), 1)
        self.assertEqual(build_env.jobs_for_ram(5), build_env.jobs_for_ram(5.0))
        for gb in (1.5, 2.5, 5, 12):
            n = build_env.jobs_for_ram(gb)
            self.assertLessEqual(build_env.ram_need_gb(n), max(gb, build_env.ram_need_gb(1)))
            self.assertGreater(build_env.ram_need_gb(n + 1), gb)

    def test_eight_gb_machine_with_five_free_is_fine(self):
        lvl, text, jobs = build_env.memory_check(4, 8 * GB, 5 * GB)
        self.assertEqual((lvl, jobs), ("ok", 4))
        self.assertEqual(build_env.memory_check(8, 8 * GB, 5 * GB)[0], "ok")

    def test_low_free_memory_warns_and_suggests_fewer_jobs(self):
        lvl, text, jobs = build_env.memory_check(8, 8 * GB, 2.5 * GB)
        self.assertEqual(lvl, "warn")
        self.assertLess(jobs, 8)
        self.assertGreaterEqual(jobs, 1)
        self.assertLessEqual(build_env.ram_need_gb(jobs), 2.5)
        self.assertIn(f"{jobs} jobs", text)

    def test_nearly_nothing_free_still_only_warns(self):
        lvl, _text, jobs = build_env.memory_check(4, 8 * GB, 0.5 * GB)
        self.assertEqual((lvl, jobs), ("warn", 1))

    def test_error_only_when_one_job_cannot_fit_in_installed_memory(self):
        self.assertEqual(build_env.memory_check(1, 1 * GB, 0.8 * GB)[0], "error")
        self.assertNotEqual(build_env.memory_check(1, 4 * GB, 1 * GB)[0], "error")

    def test_unknown_memory_is_silent(self):
        self.assertEqual(build_env.memory_check(4, 0, 0), ("ok", "", 4))

    def test_preflight_memory_row_optional(self):
        import tempfile
        from pathlib import Path
        with tempfile.TemporaryDirectory() as d:
            rows = build_env.preflight(Path(d), True, False, 2, memory=False)
            self.assertFalse([t for _l, t in rows if t.startswith("Memory")])


if __name__ == "__main__":
    unittest.main()
