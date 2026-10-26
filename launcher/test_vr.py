"""The "VR headset (experimental)" setting (#333): the cvar, the launch arguments and the Flatpak runtime path."""
from pathlib import Path
import re
import sys
import tempfile
import unittest
from unittest import mock

sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import config, paths, schema, vr

XR_SOURCE = Path(__file__).resolve().parent.parent / "forza-recomp" / "src" / "platform" / "xr.cpp"


class VrSettingTest(unittest.TestCase):
    def config(self):
        return config.Config(Path(tempfile.mkdtemp()) / "launcher.toml")

    def test_setting(self):
        setting = schema.BY_KEY["vr_mode"]
        # Marked experimental in its label, its section and the first words of its help.
        self.assertEqual(setting.label, "VR headset (experimental)")
        self.assertEqual((setting.group, setting.section, setting.kind, setting.default),
                         ("Display", "VR (experimental)", "choice", "off"))
        self.assertTrue(setting.help.startswith("Experimental"))
        self.assertEqual(schema.BY_KEY["vr_seat_height_cm"].section, "VR (experimental)")
        self.assertEqual(setting.choices, [("off", "Off"), ("screen", "Virtual screen"), ("cockpit", "Cockpit")])
        self.assertTrue(setting.restart)
        for runtime in ("WiVRn", "Monado", "SteamVR", "Meta Quest Link", "Virtual Desktop"):
            self.assertIn(runtime, setting.help)
        self.assertTrue(setting.applies())

    def test_the_game_knows_the_values(self):
        # The cvar and its allowed values in the game (src/platform/xr.cpp).
        text = XR_SOURCE.read_text()
        m = re.search(r'REXCVAR_DEFINE_STRING\(vr_mode, "(\w+)",.*?\.allowed\(\{([^}]*)\}\)', text, re.S)
        self.assertIsNotNone(m)
        self.assertEqual(m.group(1), schema.BY_KEY["vr_mode"].default)
        self.assertEqual(re.findall(r'"(\w+)"', m.group(2)), [v for v, _ in schema.BY_KEY["vr_mode"].choices])
        seat = re.search(r"REXCVAR_DEFINE_DOUBLE\(vr_seat_height_cm, ([\d.]+),", text)
        self.assertEqual(float(seat.group(1)), schema.BY_KEY["vr_seat_height_cm"].default)

    def test_off_writes_nothing_and_adds_nothing(self):
        cfg = self.config()
        self.assertNotIn("vr_mode", config.render_toml(cfg))
        env, args, notes = {}, ["--a"], []
        vr.configure(cfg, env, args, notes, in_flatpak=True)
        self.assertEqual((env, args, notes), ({}, ["--a"], []))

    def test_on_is_written_and_noted(self):
        cfg = self.config()
        cfg.settings["vr_mode"] = "cockpit"
        self.assertIn('vr_mode = "cockpit"', config.render_toml(cfg).splitlines())
        env, args, notes = {}, [], []
        with mock.patch.object(paths, "IS_WINDOWS", False):
            vr.configure(cfg, env, args, notes, in_flatpak=False, environ={})
        self.assertEqual((env, args), ({}, []))
        self.assertEqual(len(notes), 1)
        self.assertIn("Cockpit", notes[0])

    def test_windows_forces_vulkan(self):
        cfg = self.config()
        cfg.settings["vr_mode"] = "screen"
        args = ["--gpu_backend=d3d12", "--x"]
        with mock.patch.object(paths, "IS_WINDOWS", True):
            vr.configure(cfg, {}, args, [], in_flatpak=False, environ={})
        self.assertEqual(args, ["--x", "--gpu_backend=vulkan"])

    def test_flatpak_names_the_hosts_runtime(self):
        cfg = self.config()
        cfg.settings["vr_mode"] = "screen"
        home = Path(tempfile.mkdtemp())
        manifest = home / ".config" / "openxr" / "1" / "active_runtime.json"
        env = {}
        vr.configure(cfg, env, [], [], in_flatpak=True, home=home, environ={})
        self.assertNotIn("XR_RUNTIME_JSON", env)  # none installed
        manifest.parent.mkdir(parents=True)
        manifest.write_text("{}")
        vr.configure(cfg, env, [], [], in_flatpak=True, home=home, environ={})
        self.assertEqual(env["XR_RUNTIME_JSON"], str(manifest))
        # The player's own choice wins; outside the Flatpak the loader finds it itself.
        env = {}
        vr.configure(cfg, env, [], [], in_flatpak=True, home=home, environ={"XR_RUNTIME_JSON": "/x.json"})
        self.assertNotIn("XR_RUNTIME_JSON", env)
        vr.configure(cfg, env, [], [], in_flatpak=False, home=home, environ={})
        self.assertNotIn("XR_RUNTIME_JSON", env)

    def test_host_config_home(self):
        home, other = Path(tempfile.mkdtemp()), Path(tempfile.mkdtemp())
        manifest = other / "openxr" / "1" / "active_runtime.json"
        manifest.parent.mkdir(parents=True)
        manifest.write_text("{}")
        self.assertEqual(vr.active_runtime_manifest(home, {"HOST_XDG_CONFIG_HOME": str(other)}), manifest)


if __name__ == "__main__":
    unittest.main()
