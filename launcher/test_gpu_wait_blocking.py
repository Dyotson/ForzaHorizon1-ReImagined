"""The gpu_wait_blocking setting: the game's default per platform (src/hooks/gpu_wait.cpp) and the
launcher's Windows-only switch, which writes forza.toml only when turned on."""
import re
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import schema
from forza_launcher.config import Config, render_toml

HOOK = Path(__file__).resolve().parent.parent / "forza-recomp" / "src" / "hooks" / "gpu_wait.cpp"


def game_defaults():
    """{"windows": bool, "other": bool} from the hook's #ifdef _WIN32 default."""
    m = re.search(r"#ifdef _WIN32\s*#define FORZA_GPU_WAIT_BLOCKING_DEFAULT (\w+)\s*#else\s*"
                  r"#define FORZA_GPU_WAIT_BLOCKING_DEFAULT (\w+)\s*#endif", HOOK.read_text())
    assert m, "default not found in gpu_wait.cpp"
    return {"windows": m.group(1) == "true", "other": m.group(2) == "true"}


class GpuWaitBlocking(unittest.TestCase):
    def test_game_default_is_off_on_windows_only(self):
        self.assertIn("REXCVAR_DEFINE_BOOL(gpu_wait_blocking, FORZA_GPU_WAIT_BLOCKING_DEFAULT",
                      HOOK.read_text())
        self.assertEqual(game_defaults(), {"windows": False, "other": True})

    def test_launcher_switch_is_windows_only_and_matches_the_default(self):
        s = schema.BY_KEY["gpu_wait_blocking"]
        self.assertEqual((s.group, s.kind, s.platform), ("Advanced", "bool", "windows"))
        self.assertEqual(s.default, game_defaults()["windows"])
        self.assertTrue(s.restart)

    def test_written_only_when_turned_on_on_windows(self):
        with tempfile.TemporaryDirectory() as folder:
            cfg = Config(Path(folder) / "launcher.json")
            with patch("forza_launcher.schema.IS_WINDOWS", True):
                self.assertNotIn("gpu_wait_blocking", render_toml(cfg))
                cfg.settings["gpu_wait_blocking"] = True
                self.assertIn("gpu_wait_blocking = true", render_toml(cfg))
            with patch("forza_launcher.schema.IS_WINDOWS", False):
                self.assertNotIn("gpu_wait_blocking", render_toml(cfg))  # Linux: the game's default


if __name__ == "__main__":
    unittest.main()
