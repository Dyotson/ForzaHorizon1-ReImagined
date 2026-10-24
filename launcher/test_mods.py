"""Mods (#149): the mods folder, the load order kept in launcher.json, what the game is told,
and the Mods page. Disposable folders only."""
import os
import sys
import tempfile
import unittest
from pathlib import Path

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import game, mods  # noqa: E402
from forza_launcher.config import Config  # noqa: E402

try:
    from PySide6.QtCore import Qt
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None


def make_mod(root: Path, folder: str, manifest: str | None = None, files=("media/gamemodes/a.xml",)):
    d = root / folder
    d.mkdir(parents=True)
    if manifest is not None:
        (d / "mod.toml").write_text(manifest, encoding="utf-8")
    for rel in files:
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(b"data")
    return d


class ModFolders(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        t = Path(self.tmp.name)
        self.cfg = Config(t / "launcher.json")
        self.cfg.advanced["data_root"] = str(t / "home")
        self.cfg.online["mode"] = "offline"
        self.dir = mods.mods_dir(self.cfg)
        self.dir.mkdir(parents=True)
        self.install = game.GameInstall(t / "rt", t / "rt" / game.EXE)

    def test_the_folder_is_in_the_data_folder(self):
        self.assertEqual(self.dir, Path(self.cfg.data_home()) / "mods")

    def test_manifest_files_and_problems(self):
        make_mod(self.dir, "Good", 'name = "Good mod"\nversion = "1.2"\nauthor = "Me"\ndescription = "Text"\n',
                 files=("media/gamemodes/a.xml", "media/db/gamedb.slt"))
        make_mod(self.dir, "Plain")
        make_mod(self.dir, "NoMedia", files=("gamemodes/a.xml",))
        (make_mod(self.dir, "Empty", files=()) / "media" / "sub").mkdir(parents=True)
        make_mod(self.dir, "BadToml", "name = \n")
        (self.dir / ".hidden").mkdir()
        found = {m.folder: m for m in mods.scan(self.dir)}
        self.assertEqual(sorted(found), ["BadToml", "Empty", "Good", "NoMedia", "Plain"])
        good = found["Good"]
        self.assertEqual((good.title, good.version, good.author, good.description, good.files, good.size),
                         ("Good mod", "1.2", "Me", "Text", 2, 8))
        self.assertEqual(good.summary(), "Good mod 1.2 by Me")
        self.assertEqual((found["Plain"].title, found["Plain"].problem), ("Plain", ""))
        self.assertIn("media", found["NoMedia"].problem)
        self.assertIn("no files", found["Empty"].problem)
        self.assertEqual(found["BadToml"].problem, "")
        self.assertIn("mod.toml", found["BadToml"].note)

    def test_unreadable_files_and_archives(self):
        d = make_mod(self.dir, "Locked", files=("media/a.xml", "media/cars/x.zip"))
        found = mods.scan(self.dir)[0]
        self.assertEqual((found.files, found.problem), (1, ""))
        self.assertIn(".zip", found.note)
        if os.name != "nt" and os.geteuid() != 0:
            (d / "media" / "a.xml").chmod(0)
            self.addCleanup((d / "media" / "a.xml").chmod, 0o644)
            self.assertEqual(mods.scan(self.dir)[0].problem, "cannot read media/a.xml")

    def test_a_bar_in_the_name_is_refused(self):
        if os.name == "nt":
            self.skipTest("Windows file names cannot hold |")
        make_mod(self.dir, "A|B")
        self.assertIn("|", mods.scan(self.dir)[0].problem)

    def test_order_new_mods_and_turned_off(self):
        for name in ("Alpha", "Beta", "Gamma"):
            make_mod(self.dir, name)
        self.cfg.d["mods"] = {"order": ["Gone", "Gamma", "Alpha"], "disabled": ["Alpha"]}
        listed = mods.ordered(self.cfg, mods.scan(self.dir))
        self.assertEqual([(m.folder, m.enabled) for m in listed],
                         [("Gamma", True), ("Alpha", False), ("Beta", True)])
        # Saving keeps a mod that is not there now, after the shown ones.
        listed[0], listed[2] = listed[2], listed[0]
        mods.remember(self.cfg, listed)
        self.assertEqual(self.cfg.d["mods"], {"order": ["Beta", "Alpha", "Gamma", "Gone"], "disabled": ["Alpha"]})
        self.assertEqual(Config(self.cfg.path).d["mods"], self.cfg.d["mods"])

    def test_the_game_gets_the_turned_on_mods_in_order(self):
        def plan_args():
            return game.build_plan(self.cfg, self.install, write=False, log=lambda _t: None).argv

        args = plan_args()
        self.assertIn(f"--forza_mods_dir={self.dir}", args)
        self.assertIn("--forza_mods=|", args)  # none
        for name in ("Alpha", "Beta", "Broken"):
            make_mod(self.dir, name, files=() if name == "Broken" else ("media/x.xml",))
        self.cfg.d["mods"] = {"order": ["Beta", "Alpha"], "disabled": []}
        self.assertIn("--forza_mods=Beta|Alpha", plan_args())
        self.cfg.d["mods"]["disabled"] = ["Beta"]
        self.assertIn("--forza_mods=Alpha", plan_args())
        # The player's own extra arguments win.
        self.cfg.advanced["extra_args"] = "--forza_mods=Beta"
        self.assertEqual([a for a in plan_args() if a.startswith("--forza_mods")], ["--forza_mods=Beta"])

    def test_a_config_without_mods_loads(self):
        self.cfg.path.write_text('{"mods": "bad"}', encoding="utf-8")
        cfg = Config(self.cfg.path)
        self.assertEqual(mods.state(cfg), {"order": [], "disabled": []})


@unittest.skipIf(QApplication is None, "PySide6 is not installed")
class ModsPageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from forza_launcher.ui.mods_page import ModsPage
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        t = Path(self.tmp.name)
        self.cfg = Config(t / "launcher.json")
        self.cfg.advanced["data_root"] = str(t / "home")
        self.dir = mods.mods_dir(self.cfg)
        self.dir.mkdir(parents=True)
        make_mod(self.dir, "Alpha", 'name = "Alpha mod"\n')
        make_mod(self.dir, "Beta")
        self.page = ModsPage(self.cfg, window=None)

    def names(self):
        return [self.page.table.item(r, 0).text() for r in range(self.page.table.rowCount())]

    def test_lists_turns_off_and_reorders(self):
        self.assertEqual(self.names(), ["Alpha mod", "Beta"])
        self.page.table.item(0, 0).setCheckState(Qt.Unchecked)
        self.assertEqual(self.cfg.d["mods"]["disabled"], ["Alpha"])
        self.assertEqual([m.folder for m in mods.enabled(self.cfg)], ["Beta"])
        self.page.table.selectRow(1)
        self.page.move(-1)
        self.assertEqual(self.names(), ["Beta", "Alpha mod"])
        self.assertEqual(self.cfg.d["mods"]["order"], ["Beta", "Alpha"])
        self.assertFalse(self.page.up_button.isEnabled())
        self.assertIn("1 of 2", self.page.message.text())

    def test_new_folders_appear_on_refresh(self):
        make_mod(self.dir, "Gamma", files=())
        self.page.refresh()
        self.assertEqual(self.names(), ["Alpha mod", "Beta", "Gamma  (will not load)"])
        self.page.table.selectRow(2)
        self.assertIn("Will not load", self.page.details.text())


if __name__ == "__main__":
    unittest.main()
