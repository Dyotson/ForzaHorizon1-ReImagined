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
    from PySide6.QtCore import QCoreApplication, QEvent, Qt
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
        # The player's own extra arguments win, in either form, and only for the option named.
        def mod_args():
            return [a for a in plan_args() if a.startswith("--forza_mods")]

        self.cfg.advanced["extra_args"] = "--forza_mods=Beta"
        self.assertEqual(mod_args(), [f"--forza_mods_dir={self.dir}", "--forza_mods=Beta"])
        self.cfg.advanced["extra_args"] = "--forza_mods Beta"
        self.assertEqual(mod_args(), [f"--forza_mods_dir={self.dir}", "--forza_mods"])
        self.cfg.advanced["extra_args"] = "--forza_mods_dir /elsewhere"
        self.assertEqual(mod_args(), ["--forza_mods=Alpha", "--forza_mods_dir"])

    def test_a_config_without_mods_loads(self):
        for text in ('{"mods": "bad"}', '{"mods": {"order": null, "disabled": "Alpha"}}',
                     '{"mods": {"order": 5, "disabled": [1, "B"]}}'):
            self.cfg.path.write_text(text, encoding="utf-8")
            cfg = Config(self.cfg.path)
            s = mods.state(cfg)
            self.assertEqual(s["order"], [])
            self.assertIn(s["disabled"], ([], ["B"]))
            mods.launch_args(cfg)

    def test_the_launcher_agrees_with_the_game(self):
        """The rules of the game's ScanMod (mod_archive.h): the launcher says "will not load"
        for the same mods."""
        if os.name != "nt":
            make_mod(self.dir, "Case", files=("media/A.xml", "media/a.xml"))
            make_mod(self.dir, "*")
            work = self.dir.parent / "work" / "stringtables"
            (work / "en").mkdir(parents=True)
            (work / "en" / "main.str").write_bytes(b"x")
            (make_mod(self.dir, "Linked", files=()) / "media").mkdir()
            (self.dir / "Linked" / "media" / "stringtables").symlink_to(work, target_is_directory=True)
        make_mod(self.dir, "Accents", files=("media/caf\u00e9.xml",))
        make_mod(self.dir, " Spaced", files=("media/a.xml",))
        make_mod(self.dir, "Long", files=("media/" + "d/" * 130 + "x.xml", "media/ok.xml"))
        found = {m.folder: m for m in mods.scan(self.dir)}
        self.assertIn("no files", found["Accents"].problem)
        self.assertIn("plain ASCII", found["Accents"].note)
        self.assertEqual((found[" Spaced"].problem, found[" Spaced"].files), ("", 1))
        self.assertEqual((found["Long"].problem, found["Long"].files), ("", 1))
        self.assertIn("255", found["Long"].note)
        if os.name != "nt":
            self.assertIn("differ only in case", found["Case"].problem)
            self.assertIn("*", found["*"].problem)
            self.assertEqual((found["Linked"].problem, found["Linked"].files), ("", 1))

    def test_big_files_only_replace_loose_files(self):
        big = make_mod(self.dir, "Big", files=()) / "media" / "audio" / "radio" / "radio_music.fsb"
        big.parent.mkdir(parents=True)
        with open(big, "wb") as f:
            f.truncate(mods.MAX_MEMBER + 1)  # sparse
        game = Path(self.tmp.name) / "game" / "media"
        self.assertIn("over 64 MB", mods.scan(self.dir, game)[0].problem)
        (game / "audio" / "radio").mkdir(parents=True)
        (game / "audio" / "radio" / "Radio_Music.fsb").write_bytes(b"x")
        found = mods.scan(self.dir, game)[0]
        self.assertEqual((found.problem, found.files), ("", 1))

    def test_an_unreadable_folder_is_a_problem_not_a_crash(self):
        make_mod(self.dir, "Locked")
        real = Path.is_dir

        def is_dir(p):
            if p.name == "media" and p.parent.name == "Locked":
                raise PermissionError(13, "Permission denied", str(p))
            return real(p)

        from unittest import mock
        with mock.patch.object(Path, "is_dir", is_dir):
            found = mods.scan(self.dir)
        self.assertEqual(len(found), 1)
        self.assertIn("could not be read", found[0].problem)


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

    def tearDown(self):
        # Delete the page now, while the QApplication exists: a top-level widget left to the
        # interpreter's exit is destroyed after it, and PySide6 6.12 then crashes (exit 139).
        self.page.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)

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

    def test_return_turns_a_mod_on_and_off(self):
        """A controller's A arrives as Return (ui/gamepad.py)."""
        from PySide6.QtGui import QKeyEvent
        self.page.table.selectRow(1)
        for key, want in ((Qt.Key_Return, ["Beta"]), (Qt.Key_Enter, [])):
            QApplication.sendEvent(self.page.table, QKeyEvent(QEvent.KeyPress, key, Qt.NoModifier))
            self.assertEqual(self.cfg.d["mods"]["disabled"], want)
        self.assertEqual(self.page.table.currentRow(), 1)

    def test_new_folders_appear_on_refresh(self):
        make_mod(self.dir, "Gamma", files=())
        self.page.refresh()
        self.assertEqual(self.names(), ["Alpha mod", "Beta", "Gamma  (will not load)"])
        self.page.table.selectRow(2)
        self.assertIn("Will not load", self.page.details.text())


if __name__ == "__main__":
    unittest.main()
