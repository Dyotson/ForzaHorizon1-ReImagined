import hashlib
import json
import tempfile
import unittest
from pathlib import Path
from types import SimpleNamespace
from unittest.mock import patch

from forza_launcher import dlss


class RuntimeSetup(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cfg = SimpleNamespace(data_home=lambda: self.root)
        self.dll, self.license = b"test NVIDIA runtime", b"test license"
        for name, content in (("runtime", self.dll), ("license", self.license)):
            (self.root / name).write_bytes(content)
        for name, value in (("RUNTIME_SHA256", hashlib.sha256(self.dll).hexdigest()),
                            ("LICENSE_SHA256", hashlib.sha256(self.license).hexdigest())):
            p = patch.object(dlss, name, value)
            p.start()
            self.addCleanup(p.stop)
        p = patch.object(dlss.paths, "IS_WINDOWS", True)
        p.start()
        self.addCleanup(p.stop)

    def download(self, key, *_args):
        return self.root / ("license" if key == "dlss-license" else "runtime")

    def test_installs_both_assets_and_reuses_them_offline(self):
        with patch.object(dlss.fetch, "download", side_effect=self.download) as fetch:
            dest = dlss.prepare(self.cfg)
            self.assertEqual(fetch.call_count, 2)
        self.assertEqual((dest / "nvngx_dlss.dll").read_bytes(), self.dll)
        self.assertEqual((dest / "LICENSE.txt").read_bytes(), self.license)
        with patch.object(dlss.fetch, "download", side_effect=AssertionError("went online")):
            self.assertEqual(dlss.prepare(self.cfg), dest)

    def test_detects_and_repairs_a_changed_runtime(self):
        with patch.object(dlss.fetch, "download", side_effect=self.download):
            dest = dlss.prepare(self.cfg)
            (dest / "nvngx_dlss.dll").write_bytes(b"changed")
            self.assertFalse(dlss.ready(self.cfg))
            dlss.prepare(self.cfg)
        self.assertTrue(dlss.ready(self.cfg))

    def test_failed_download_does_not_replace_installed_files(self):
        dest = dlss.runtime_dir(self.cfg)
        dest.mkdir(parents=True)
        (dest / "nvngx_dlss.dll").write_bytes(b"existing version")
        with patch.object(dlss.fetch, "download", side_effect=[self.root / "runtime", OSError("offline")]):
            with self.assertRaises(OSError):
                dlss.prepare(self.cfg)
        self.assertEqual((dest / "nvngx_dlss.dll").read_bytes(), b"existing version")
        self.assertFalse((dest / "LICENSE.txt").exists())

    def test_cancellation_leaves_no_partial_install(self):
        with patch.object(dlss.fetch, "download", side_effect=self.download):
            with self.assertRaises(dlss.fetch.Cancelled):
                dlss.prepare(self.cfg, cancelled=lambda: True)
        self.assertFalse((dlss.runtime_dir(self.cfg) / "nvngx_dlss.dll").exists())

    def test_does_not_attempt_windows_install_on_other_platforms(self):
        with patch.object(dlss.paths, "IS_WINDOWS", False), patch.object(dlss.fetch, "download") as fetch:
            with self.assertRaisesRegex(RuntimeError, "Windows"):
                dlss.prepare(self.cfg)
            fetch.assert_not_called()

    def test_preview_configures_native_ui_without_downloading(self):
        self.cfg.settings = {"fh1_upscaler": "dlss"}
        env, args, notes = {}, [], []
        with patch.object(dlss, "prepare", side_effect=AssertionError("downloaded in preview")):
            dlss.configure(self.cfg, env, args, notes, write=False)
        self.assertEqual(env["FORZA_DLSS_RUNTIME"], str(dlss.runtime_dir(self.cfg)))
        self.assertIn("--resolution=1440p", args)
        self.assertIn("--resolution_scale=2", args)
        self.assertIn("720p", notes[0])
        self.assertFalse(dlss.runtime_dir(self.cfg).exists())

    def test_dlaa_launch_prepares_runtime_and_describes_native_input(self):
        self.cfg.settings = {"fh1_upscaler": "dlaa"}
        notes = []
        with patch.object(dlss, "prepare") as prepare:
            dlss.configure(self.cfg, {}, [], notes)
            prepare.assert_called_once_with(self.cfg)
        self.assertEqual(notes, ["DLAA: native 1440p world and UI."])

    def test_off_leaves_launch_unchanged(self):
        self.cfg.settings = {"fh1_upscaler": "off"}
        env, args, notes = {}, [], []
        with patch.object(dlss, "prepare") as prepare:
            dlss.configure(self.cfg, env, args, notes)
            prepare.assert_not_called()
        self.assertEqual((env, args, notes), ({}, [], []))

    def test_unknown_mode_is_rejected_before_downloading(self):
        self.cfg.settings = {"fh1_upscaler": "made-up"}
        with patch.object(dlss, "prepare") as prepare:
            with self.assertRaisesRegex(RuntimeError, "Unknown NVIDIA"):
                dlss.configure(self.cfg, {}, [], [])
            prepare.assert_not_called()

    def test_session_status_requires_renderer_confirmation(self):
        path = self.root / "session.status"
        plan = SimpleNamespace(env={"FORZA_DLSS_STATUS_FILE": str(path)})
        self.assertIsNone(dlss.session_status(plan))
        path.write_text("ready: DLAA is active.")
        self.assertEqual(dlss.session_status(plan), (False, "DLAA is active."))
        path.write_text("error: Driver unavailable; using native 1440p.")
        self.assertEqual(dlss.session_status(plan), (True, "Driver unavailable; using native 1440p."))
        path.write_text("ready:")
        self.assertIsNone(dlss.session_status(plan))

    def test_renderer_capability_is_bound_to_the_built_dll(self):
        with self.assertRaisesRegex(RuntimeError, "does not include"):
            dlss.check_renderer(self.root)
        binary = self.root / "rexgpu-xenos.dll"
        binary.write_bytes(b"native renderer")
        metadata = {"api": 1, "renderer_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
        (self.root / "fh1-native-dlss.json").write_text(json.dumps(metadata))
        dlss.check_renderer(self.root)
        binary.write_bytes(b"older SDK overlay")
        with self.assertRaisesRegex(RuntimeError, "updated SDK"):
            dlss.check_renderer(self.root)


if __name__ == "__main__":
    unittest.main()
