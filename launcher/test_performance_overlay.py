import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from forza_launcher import dlss, gpu, ingame, schema
from forza_launcher.config import Config


class PerformanceOverlayTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cfg = Config(self.root / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.root)
        self.pending = self.root / ingame.FILENAME

    def request(self, **values):
        self.pending.write_text(json.dumps({"version": 1, "settings": values}))

    def test_performance_overlay_round_trips_each_mode(self):
        for mode in ("off", "fps", "graph"):
            with self.subTest(mode=mode):
                self.request(forza_performance_overlay=mode)
                self.assertTrue(ingame.import_pending(self.cfg, lambda _: None))
                self.assertEqual(Config(self.cfg.path).settings["forza_performance_overlay"], mode)
        previous = dict(self.cfg.settings)
        for bad in ("on", "GPU", "Graph", True):
            self.request(forza_performance_overlay=bad)
            self.assertFalse(ingame.import_pending(self.cfg, lambda _: None))
            self.assertEqual(self.cfg.settings, previous)
