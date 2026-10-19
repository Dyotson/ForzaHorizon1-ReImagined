import json
from pathlib import Path
import tempfile
import unittest
from unittest.mock import patch

from forza_launcher import dlss, gpu, antialiasing, schema
from forza_launcher.config import Config


class AntiAliasingTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cfg = Config(self.root / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.root)


    def test_smaa_runtime_is_native_and_never_prepares_nvidia(self):
        self.cfg.settings.update(gpu_backend="vulkan", resolution="1440p",
                                 resolution_scale=1, present_effect="bilinear",
                                 forza_antialiasing="smaa", fh1_upscaler="off")
        args, env = [], {}
        with patch.object(dlss, "ready", return_value=False), patch.object(dlss, "prepare") as prepare:
            antialiasing.configure_runtime(self.cfg, env, args)
        prepare.assert_not_called()
        self.assertNotIn("FORZA_DLSS_RUNTIME", env)
        self.assertIn("--fh1_upscaler=smaa", args)
        self.assertIn("--fh1_output_width=2560", args)
        self.assertIn("--fh1_native_scene_scale=2", args)
        self.cfg.settings.update(present_effect="fsr3", fsr3_mode="quality")
        with self.assertRaisesRegex(RuntimeError, "includes anti-aliasing"):
            antialiasing.configure_runtime(self.cfg, {}, [])

    def test_native_fsr_aa_uses_output_sized_input_without_nvidia(self):
        from forza_launcher import fsr3
        self.cfg.settings.update(forza_antialiasing="fsr3_native", present_effect="bilinear",
                                 resolution="1440p", fh1_upscaler="off")
        args = []
        fsr3.configure(self.cfg, args, [])
        self.assertIn("--fh1_upscaler=fsr3_native", args)
        self.assertFalse(dlss.enabled(self.cfg))

    @unittest.skipUnless("fh1_upscaler" in schema.BY_KEY, "Windows NVIDIA setting")
    def test_legacy_dlaa_migrates_to_separate_aa_selector(self):
        self.cfg.settings.update(fh1_upscaler="dlaa")
        self.cfg.save()
        cfg = Config(self.cfg.path)
        self.assertEqual(cfg.settings["fh1_upscaler"], "off")
        self.assertEqual(cfg.settings["forza_antialiasing"], "dlaa")
        cfg.gpu_vendor = lambda: gpu.NVIDIA
        with patch.object(dlss, "prepare") as prepare:
            args = []
            dlss.configure(cfg, {}, args, [], write=False)
        prepare.assert_not_called()
        self.assertIn("--fh1_upscaler=dlaa", args)

    def test_game_msaa_preserves_existing_render_path_for_every_output_and_scale(self):
        self.cfg.settings.update(gpu_backend="vulkan", forza_antialiasing="game",
                                 fh1_upscaler="off", present_effect="bilinear",
                                 forza_ultrawide=False)
        for vendor in (gpu.AMD, gpu.INTEL, gpu.DECK, gpu.NVIDIA):
            for resolution in ("720p", "1080p", "1440p", "4k"):
                for scale in (1, 2, 3):
                    with self.subTest(vendor=vendor, resolution=resolution, scale=scale):
                        self.cfg.gpu["vendor"] = vendor
                        self.cfg.settings.update(resolution=resolution, resolution_scale=scale)
                        original = [f"--resolution_scale={scale}"]
                        args, env = list(original), {}
                        with patch.object(dlss, "ready", return_value=False):
                            antialiasing.configure_runtime(self.cfg, env, args)
                        self.assertEqual(args, original)
                        self.assertEqual(self.cfg.settings["resolution_scale"], scale)

    def test_upscaling_keeps_post_aa_override_without_native_scene_arguments(self):
        self.cfg.settings.update(gpu_backend="vulkan", resolution="1440p",
                                 resolution_scale=1, forza_antialiasing="game",
                                 forza_ultrawide=False)
        for upscaler, effect in (("quality", "bilinear"), ("off", "fsr3")):
            with self.subTest(upscaler=upscaler, effect=effect):
                self.cfg.settings.update(fh1_upscaler=upscaler, present_effect=effect)
                args = []
                with patch.object(dlss, "ready", return_value=False):
                    antialiasing.configure_runtime(self.cfg, {}, args)
                self.assertEqual(args, ["--swap_post_effect=none"])

    def test_custom_ultrawide_keeps_its_existing_render_path(self):
        self.cfg.settings.update(gpu_backend="vulkan", resolution="1440p",
                                 resolution_scale=2, forza_ultrawide=True)
        args = []
        with patch("forza_launcher.dlss.ready", return_value=False):
            antialiasing.configure_runtime(self.cfg, {}, args)
        self.assertEqual(args, [])
