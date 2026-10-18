import hashlib
import json
import os
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
        self.cfg = SimpleNamespace(data_home=lambda: self.root, gpu_vendor=lambda: "nvidia")
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
        cache = self.root / "downloads" / "nvidia-dlss" / dlss.SDK_REVISION
        cache.mkdir(parents=True, exist_ok=True)
        target = cache / ("LICENSE.txt" if key == "dlss-license" else "nvngx_dlss.dll")
        target.write_bytes(self.license if key == "dlss-license" else self.dll)
        return target

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

    def test_play_pins_every_setting_prepare_reads(self):
        # #107: Play hands prepare() a pinned SimpleNamespace; it must carry gpu_vendor too.
        src = (Path(__file__).resolve().parents[1] / "forza_launcher/ui/window.py").read_text()
        line = next(l for l in src.splitlines() if "download_cfg = SimpleNamespace(" in l)
        for attr in ("data_home", "gpu_vendor"):
            self.assertIn(attr + "=", line)

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
        self.assertIn("DLSS Performance", notes[0])
        self.assertFalse(dlss.runtime_dir(self.cfg).exists())

    def test_saved_dlss_modes_are_inactive_on_other_vendors(self):
        for vendor in ("amd", "intel", "deck", ""):
            for mode in ("quality", "balanced", "dlss", "dlaa"):
                for write in (False, True):
                    with self.subTest(vendor=vendor, mode=mode, write=write):
                        self.cfg.gpu_vendor = lambda: vendor
                        self.cfg.settings = {"fh1_upscaler": mode, "fh1_output_resolution": "4k"}
                        env, args, notes = {"KEEP": "yes"}, ["--gpu_backend=d3d12"], []
                        with patch.object(dlss, "prepare") as prepare:
                            self.assertFalse(dlss.enabled(self.cfg))
                            dlss.configure(self.cfg, env, args, notes, write=write)
                            prepare.assert_not_called()
                        self.assertEqual(env, {"KEEP": "yes"})
                        self.assertEqual(args, ["--gpu_backend=d3d12"])
                        self.assertEqual(notes, [])

    def test_direct_setup_rejects_other_vendors_before_downloading(self):
        self.cfg.gpu_vendor = lambda: "amd"
        with patch.object(dlss.fetch, "download") as fetch:
            with self.assertRaisesRegex(RuntimeError, "NVIDIA"):
                dlss.prepare(self.cfg)
            fetch.assert_not_called()

    def test_dlaa_launch_prepares_runtime_and_describes_native_input(self):
        self.cfg.settings = {"fh1_upscaler": "dlaa"}
        notes = []
        with patch.object(dlss, "prepare") as prepare:
            dlss.configure(self.cfg, {}, [], notes)
            prepare.assert_called_once_with(self.cfg)
        self.assertIn("Output-sized NGX input", notes[0])

    def test_output_and_quality_matrix_uses_independent_dimensions(self):
        for output, width, height, scale in (("1080p", 1920, 1080, 2),
                                             ("1440p", 2560, 1440, 2), ("4k", 3840, 2160, 3)):
            for mode in ("quality", "balanced", "dlss", "dlaa"):
                with self.subTest(output=output, mode=mode):
                    self.cfg.settings = {"fh1_upscaler": mode, "fh1_output_resolution": output}
                    args, notes = [], []
                    dlss.configure(self.cfg, {}, args, notes, write=False)
                    self.assertIn(f"--resolution={output}", args)
                    self.assertIn(f"--resolution_scale={scale}", args)
                    self.assertIn(f"--fh1_output_width={width}", args)
                    self.assertIn(f"--fh1_output_height={height}", args)
                    self.assertIn(f"native {output} UI", notes[0])

    def test_output_selector_is_launcher_only(self):
        from forza_launcher.config import Config, render_toml
        cfg = Config(self.root / "launcher.json")
        cfg.settings["fh1_output_resolution"] = "4k"
        self.assertNotIn("fh1_output_resolution", render_toml(cfg))

    def test_old_renderer_api_is_rejected_even_with_a_matching_hash(self):
        binary = self.root / "rexgpu-xenos.dll"
        binary.write_bytes(b"1440p-only renderer")
        metadata = {"api": 1, "renderer_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
        (self.root / "fh1-native-dlss.json").write_text(json.dumps(metadata))
        with self.assertRaisesRegex(RuntimeError, "updated SDK"):
            dlss.check_renderer(self.root)

    def test_invalid_output_is_rejected_before_preparing_runtime(self):
        self.cfg.settings = {"fh1_upscaler": "quality", "fh1_output_resolution": "720p"}
        with patch.object(dlss, "prepare") as prepare:
            with self.assertRaisesRegex(RuntimeError, "Unsupported NVIDIA output"):
                dlss.configure(self.cfg, {}, [], [])
            prepare.assert_not_called()

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
        metadata = {"api": 2, "renderer_sha256": hashlib.sha256(binary.read_bytes()).hexdigest()}
        (self.root / "fh1-native-dlss.json").write_text(json.dumps(metadata))
        dlss.check_renderer(self.root)
        binary.write_bytes(b"older SDK overlay")
        with self.assertRaisesRegex(RuntimeError, "updated SDK"):
            dlss.check_renderer(self.root)

    def test_runtime_install_keeps_only_one_copy(self):
        with patch.object(dlss.fetch, "download", side_effect=self.download):
            dest = dlss.prepare(self.cfg)
        copies = list(self.root.rglob("nvngx_dlss.dll"))
        self.assertEqual(copies, [dest / "nvngx_dlss.dll"])

    def test_hash_cache_reuses_unchanged_file_and_detects_changes(self):
        binary = self.root / "runtime"
        with patch.object(dlss.fetch, "sha256_of", wraps=dlss.fetch.sha256_of) as sha:
            original = dlss._sha256(binary)
            self.assertEqual(dlss._sha256(binary), original)
            self.assertEqual(sha.call_count, 1)
            binary.write_bytes(b"replacement runtime")
            # Same-size writes can share a filesystem timestamp tick. Model an
            # observable modification explicitly rather than depending on speed.
            stat = binary.stat()
            os.utime(binary, ns=(stat.st_atime_ns, stat.st_mtime_ns + 1_000_000_000))
            self.assertNotEqual(dlss._sha256(binary), original)
            self.assertEqual(sha.call_count, 2)

    def test_settings_preview_does_not_hash_or_verify_runtime(self):
        from forza_launcher.config import Config
        from forza_launcher import game
        cfg = Config(self.root / "launcher.json")
        cfg.advanced["data_root"] = str(self.root)
        cfg.online["mode"] = "offline"
        cfg.settings["fh1_upscaler"] = "dlss"
        cfg.gpu["vendor"] = "nvidia"
        install = game.GameInstall(self.root, self.root / "forza.exe")
        with patch.object(dlss, "check_renderer", side_effect=AssertionError("verified during preview")), \
             patch.object(dlss, "_sha256", side_effect=AssertionError("hashed during preview")):
            game.build_plan(cfg, install, write=False)

    def test_session_cleanup_removes_generated_status_only(self):
        status = self.root / ("nvidia-dlss-" + "a" * 32 + ".status")
        status.write_text("ready: DLSS active.")
        plan = SimpleNamespace(env={"FORZA_DLSS_STATUS_FILE": str(status)}, dlss_status_file=status)
        dlss.cleanup_status(plan)
        self.assertFalse(status.exists())
        other = self.root / "user-status.txt"
        other.write_text("keep")
        plan.env["FORZA_DLSS_STATUS_FILE"] = str(other)
        plan.dlss_status_file = None
        dlss.cleanup_status(plan)
        self.assertTrue(other.exists())

    def test_windows_package_carries_renderer_metadata_and_notice(self):
        import shutil
        import subprocess
        bash = shutil.which("bash")
        if dlss.paths.IS_WINDOWS:
            git_bash = Path("C:/Program Files/Git/bin/bash.exe")
            bash = str(git_bash) if git_bash.is_file() else None
        if not bash:
            self.skipTest("bash is unavailable")
        prefix, dest = self.root / "prefix", self.root / "dist"
        (prefix / "bin").mkdir(parents=True)
        (prefix / "share/licenses/nvidia-dlss").mkdir(parents=True)
        (dest / "licenses").mkdir(parents=True)
        (prefix / "bin/fh1-native-dlss.json").write_text('{"api":1}')
        (prefix / "share/licenses/nvidia-dlss/LICENSE.txt").write_text("notice")
        script = (Path(__file__).resolve().parents[2] / "tools/build_windows.sh").read_text()
        begin = script.index("# Native NGX capability")
        end = script.index('cp "$ROOT/tools/xcontent_extract.py"', begin)
        subprocess.run([bash, "-c", 'set -e; PREFIX="$1"; DIST="$2";\n' + script[begin:end],
                        "--", prefix.as_posix(), dest.as_posix()], check=True)
        self.assertEqual((dest / "fh1-native-dlss.json").read_text(), '{"api":1}')
        self.assertEqual((dest / "licenses/NVIDIA-DLSS.txt").read_text(), "notice")


if __name__ == "__main__":
    unittest.main()
