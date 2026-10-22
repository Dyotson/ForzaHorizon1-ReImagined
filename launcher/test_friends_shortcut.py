"""The controller friends shortcut setting (#194) reaches the game's forza.toml."""
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import config, schema


class FriendsShortcutTest(unittest.TestCase):
    def config(self):
        return config.Config(Path(tempfile.mkdtemp()) / "launcher.toml")

    def test_on_by_default(self):
        self.assertTrue(schema.BY_KEY["live_friends_pad_shortcut"].default)
        self.assertTrue(self.config().settings["live_friends_pad_shortcut"])

    def test_off_is_written_for_the_game(self):
        cfg = self.config()
        cfg.settings["live_friends_pad_shortcut"] = False
        self.assertIn("live_friends_pad_shortcut = false", config.render_toml(cfg).splitlines())


if __name__ == "__main__":
    unittest.main()
