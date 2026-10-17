"""AMD FSR 3 (temporal) settings: the game arguments and the toml it writes."""
import sys
import tempfile
import unittest
from pathlib import Path
sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import fsr3, gpu
from forza_launcher.config import Config, render_toml


class Fsr3Settings(unittest.TestCase):
    def config(self, **settings):
        folder = tempfile.TemporaryDirectory()
        self.addCleanup(folder.cleanup)
        cfg = Config(Path(folder.name) / 'launcher.json')
        cfg.settings.update(settings)
        return cfg

    def test_off_by_default(self):
        cfg = self.config()
        self.assertFalse(fsr3.enabled(cfg))
        args = ['--present_effect=fsr']
        fsr3.configure(cfg, args, [])
        self.assertEqual(args, ['--present_effect=fsr'])

    def test_arguments(self):
        cfg = self.config(present_effect='fsr3', fsr3_mode='quality', fsr3_output_resolution='4k')
        args, notes = ['--present_effect=fsr3'], []
        fsr3.configure(cfg, args, notes)
        for arg in ('--resolution=4k', '--resolution_scale=3', '--fh1_output_width=3840',
                    '--fh1_output_height=2160', '--fh1_upscaler=fsr3_quality'):
            self.assertIn(arg, args)
        # FSR 1 stays the presenter filter (and the fallback)
        self.assertEqual([a for a in args if a.startswith('--present_effect=')], ['--present_effect=fsr'])
        self.assertTrue(notes)

    def test_launcher_only_keys_stay_out_of_the_toml(self):
        text = render_toml(self.config(present_effect='fsr3', fsr3_mode='native',
                                       fsr3_output_resolution='1080p'))
        self.assertIn('present_effect = "fsr3"', text)
        self.assertNotIn('fsr3_mode', text)
        self.assertNotIn('fsr3_output_resolution', text)

    def test_dlss_wins_on_nvidia(self):
        cfg = self.config(present_effect='fsr3', fh1_upscaler='quality')
        cfg.gpu_vendor = lambda: gpu.NVIDIA
        self.assertFalse(fsr3.enabled(cfg))
        cfg.gpu_vendor = lambda: gpu.AMD
        self.assertTrue(fsr3.enabled(cfg))


if __name__ == '__main__':
    unittest.main()
