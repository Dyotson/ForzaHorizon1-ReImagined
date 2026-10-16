"""Built-in ultrawide settings survive persistence without any mod machinery."""
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import schema
from forza_launcher.config import Config, render_toml

class UltrawideSettings(unittest.TestCase):
    def test_defaults_leave_presentation_unchanged(self):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        cfg = Config(Path(folder.name) / 'launcher.json')
        self.assertFalse(cfg.settings['forza_ultrawide'])
        self.assertFalse(cfg.settings['forza_hud_edges'])
        self.assertNotIn('forza_ultrawide', render_toml(cfg))
        self.assertNotIn('Mods', schema.GROUPS)

    def test_settings_roundtrip(self):
        with tempfile.TemporaryDirectory() as folder:
            cfg = Config(Path(folder) / 'launcher.json')
            cfg.settings.update(forza_ultrawide=True, forza_hud_edges=True, forza_hud_layout=True, forza_hud_safe_width=90.0,
                                forza_hud_safe_height=95.0, forza_hud_offset_x=-5.0)
            cfg.save()
            loaded = Config(cfg.path)
            for key in ('forza_ultrawide', 'forza_hud_edges', 'forza_hud_layout', 'forza_hud_safe_width',
                        'forza_hud_safe_height', 'forza_hud_offset_x'):
                self.assertEqual(loaded.settings[key], cfg.settings[key])
            self.assertIn('forza_ultrawide = true', render_toml(loaded))

    def test_safe_area_bounds(self):
        self.assertEqual(schema.coerce(schema.BY_KEY['forza_hud_safe_width'], 0), 50)
        self.assertEqual(schema.coerce(schema.BY_KEY['forza_hud_safe_height'], 200), 100)
        self.assertEqual(schema.coerce(schema.BY_KEY['forza_hud_offset_x'], -100), -20)

if __name__ == '__main__':
    unittest.main()
