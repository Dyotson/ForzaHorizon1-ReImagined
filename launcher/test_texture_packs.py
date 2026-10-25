import os
import struct
import sys
import tempfile
import unittest
import zipfile
from pathlib import Path
from unittest.mock import patch

os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
sys.path.insert(0, str(Path(__file__).resolve().parent))

from forza_launcher import config, crash_report, game, power, schema, texture_packs  # noqa: E402
from forza_launcher.config import Config  # noqa: E402

try:
    from PySide6.QtCore import QCoreApplication, QEvent, Qt
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None

FP = ["%016x" % (0x1111111111111111 * i) for i in range(1, 9)]


def dds(width=8, height=8, four_cc=b"DXT1", caps2=0, data=None, dx10=None):
    """A DDS file: a legacy header with a four CC (or DX10 and its extension)."""
    header = [0] * 31
    header[0:4] = [124, 0x1007, height, width]
    header[6] = 1
    header[18:21] = [32, 0x4, struct.unpack("<I", four_cc)[0]]
    header[26:28] = [0x1000, caps2]
    head = b"DDS " + struct.pack("<31I", *header)
    if dx10 is not None:
        head += struct.pack("<5I", *dx10)
    blocks = ((width + 3) // 4) * ((height + 3) // 4) * (8 if four_cc in (b"DXT1", b"ATI1") else 16)
    return head + (data if data is not None else b"\0" * blocks)


def png(width=8, height=8):
    """A PNG header and IHDR (enough for the launcher's check; the game reads the rest)."""
    return b"\x89PNG\r\n\x1a\n" + struct.pack(">I4sIIBBBBB", 13, b"IHDR", width, height, 8, 6, 0, 0, 0) + b"\0" * 16


def make_pack(root: Path, folder: str, files: dict, manifest: str | None = None) -> Path:
    d = root / folder
    d.mkdir(parents=True)
    if manifest is not None:
        (d / "pack.toml").write_text(manifest, encoding="utf-8")
    for rel, data in files.items():
        p = d / rel
        p.parent.mkdir(parents=True, exist_ok=True)
        p.write_bytes(data)
    return d


def make_zip(path: Path, members: dict) -> Path:
    with zipfile.ZipFile(path, "w") as z:
        for name, data in members.items():
            if isinstance(data, zipfile.ZipInfo):
                z.writestr(data, b"target")
            else:
                z.writestr(name, data)
    return path


class TexturePackTests(unittest.TestCase):
    """Settings > Display > Texture packs (#384, docs/texture_packs.md)."""

    def setUp(self):
        platform = patch.object(power, "IS_WINDOWS", False)
        platform.start()
        self.addCleanup(platform.stop)
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.cfg = Config(self.root / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.root / "home")
        self.cfg.online["mode"] = "offline"
        self.install = game.GameInstall(self.root / "rt", self.root / "rt" / game.EXE)
        (self.root / "rt").mkdir()

    def setting(self, key):
        return next(s for s in schema.S if s.key == key)

    def plan(self):
        return game.build_plan(self.cfg, self.install, write=False, log=lambda _t: None)

    def test_two_options_in_their_own_section(self):
        for key, label in (("texture_dump", "Dump textures"),
                           ("texture_replace", "Use texture packs")):
            with self.subTest(key=key):
                s = self.setting(key)
                self.assertEqual((s.label, s.group, s.section, s.kind),
                                 (label, "Display", "Texture packs", "bool"))
                self.assertIs(s.default, False)
                self.assertTrue(s.restart)
                self.assertIn(key, crash_report.SETTING_KEYS)
        sections = [s.section for s in schema.S if s.group == "Display"]
        start = sections.index("Texture packs")
        self.assertEqual(sections[start:start + 2], ["Texture packs", "Texture packs"])
        self.assertNotIn("Texture packs", sections[start + 2:])

    def test_off_writes_nothing_and_passes_no_folder(self):
        plan = self.plan()
        self.assertNotIn("texture_", plan.toml_text)
        self.assertFalse(any(a.startswith("--texture_pack_root") for a in plan.argv))

    def test_either_option_reaches_forza_toml_with_the_data_folder(self):
        folder = self.root / "home" / "textures"
        self.assertEqual(game.texture_pack_folder(self.cfg), folder)
        for key in ("texture_dump", "texture_replace"):
            with self.subTest(key=key):
                self.cfg.settings.update(texture_dump=False, texture_replace=False)
                self.cfg.settings[key] = True
                plan = self.plan()
                self.assertEqual(config.parse_toml(plan.toml_text)[key], True)
                self.assertIn(f"--texture_pack_root={folder}", plan.argv)

    def test_a_folder_in_the_extra_arguments_is_the_only_one(self):
        self.cfg.settings["texture_replace"] = True
        self.cfg.advanced["extra_args"] = "--texture_pack_root=/elsewhere"
        argv = self.plan().argv
        self.assertEqual([a for a in argv if a.startswith("--texture_pack_root")],
                         ["--texture_pack_root=/elsewhere"])

    def test_settings_page_has_the_folder_button_and_needs_vulkan(self):
        try:
            from PySide6.QtWidgets import QApplication
        except ImportError:
            self.skipTest("PySide6 is not installed")
        from unittest.mock import MagicMock
        from forza_launcher.ui.settings_page import SettingsPage
        app = QApplication.instance() or QApplication([])
        window = MagicMock()
        window.preview_text.return_value = ""
        page = SettingsPage(self.cfg, window)
        self.addCleanup(page.close)
        self.assertEqual(page.texture_folder_button.text(), "Open textures folder")
        self.assertEqual(page.texture_folder_button.toolTip(), str(self.root / "home" / "textures"))
        self.assertTrue(page.rows["texture_replace"].control.isEnabled())
        self.cfg.settings["gpu_backend"] = "d3d12"
        page._apply_dependencies()
        self.assertFalse(page.rows["texture_replace"].control.isEnabled())
        self.assertTrue(page.rows["texture_dump"].control.isEnabled())
        app.processEvents()



class PackFolders(unittest.TestCase):
    """The packs in the textures folder (texture_packs.py) and what the game is told."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.t = Path(self.tmp.name)
        self.cfg = Config(self.t / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.t / "home")
        self.cfg.online["mode"] = "offline"
        self.dir = texture_packs.textures_dir(self.cfg)
        self.dir.mkdir(parents=True)

    def test_files_are_checked_as_the_game_reads_them(self):
        make_pack(self.dir, "Rocks", {
            f"{FP[0]}.dds": dds(),
            f"sub/{FP[1].upper()}.PNG": png(16, 8),
            f"{FP[2]}.dds": dds(caps2=0x200),                       # cube map
            f"{FP[3]}.dds": dds(four_cc=b"DX10", dx10=(2, 3, 0, 1, 0)),  # DXGI 2: float
            f"{FP[4]}.dds": dds(width=64, data=b"\0" * 8),           # shorter than its header
            f"{FP[5]}.dds": dds(four_cc=b"DX10", dx10=(98, 3, 0, 1, 0), data=b"\0" * 64),  # BC7
            f"{FP[6]}.png": png(20000, 4),
            f"{FP[0]}.png": png(),                                   # the .dds wins
            "preview.jpg": b"x", "readme.txt": b"hi",
        }, 'name = "Rock pack"\nauthor = "Me"\nversion = "2"\n')
        pack = texture_packs.read_pack(self.dir / "Rocks")
        self.assertEqual((pack.title, pack.author, pack.version, pack.problem), ("Rock pack", "Me", "2", ""))
        self.assertEqual(pack.names, sorted([FP[0], FP[1], FP[5]]))
        self.assertEqual(pack.files, 3)
        self.assertIn("4 file(s) the game cannot use", pack.note)
        self.assertIn("a cube map", pack.note)
        self.assertIn("1 texture(s) are in it twice", pack.note)
        self.assertIn("1 file(s) are not named by a fingerprint", pack.note)
        # BC1 8x8 (32 bytes), a 16x8 PNG as RGBA8 (512), BC7 8x8 (64); with mips (4/3).
        self.assertEqual(pack.gpu_bytes, 32 * 4 // 3 + 512 * 4 // 3 + 64 * 4 // 3)
        make_pack(self.dir, "Empty", {"notes.txt": b"x"})
        self.assertIn("no texture files", texture_packs.read_pack(self.dir / "Empty").problem)
        if os.name != "nt":  # Windows folders cannot have a | anyway
            make_pack(self.dir, "a|b", {f"{FP[0]}.dds": dds()})
            self.assertIn("|", texture_packs.read_pack(self.dir / "a|b").problem)

    def test_listed_in_order_with_the_made_pack_first(self):
        for name in ("Beta", "Alpha", texture_packs.HD_PACK, ".adding-x", "dump"):
            make_pack(self.dir, name, {f"{FP[0]}.dds": dds()})
        (self.dir / f"{FP[1]}.dds").write_bytes(dds())
        listed = texture_packs.listed(self.cfg)
        self.assertEqual([p.folder for p in listed], [texture_packs.HD_PACK, "Alpha", "Beta"])
        self.assertEqual(listed[0].title, "HD pack (made on this PC)")
        self.assertEqual(texture_packs.loose_files(self.dir), 1)
        listed.reverse()
        listed[0].enabled = False
        texture_packs.remember(self.cfg, listed)
        self.assertEqual(self.cfg.d["texture_packs"], {"order": ["Beta", "Alpha", texture_packs.HD_PACK],
                                                       "disabled": ["Beta"]})
        self.assertEqual([p.folder for p in texture_packs.enabled(self.cfg)], ["Alpha", texture_packs.HD_PACK])
        # All three have FP[0]: the lowest one in the list wins.
        clashes = texture_packs.conflicts(texture_packs.listed(self.cfg))
        self.assertEqual(clashes["Alpha"], [(FP[0], texture_packs.HD_PACK, False)])
        self.assertEqual(clashes[texture_packs.HD_PACK], [(FP[0], "Alpha", True)])
        self.assertNotIn("Beta", clashes)

    def test_the_game_gets_the_packs_in_order(self):
        make_pack(self.dir, "Alpha", {f"{FP[0]}.dds": dds()})
        make_pack(self.dir, "Beta Two", {f"{FP[0]}.png": png()})
        install = game.GameInstall(self.t / "rt", self.t / "rt" / game.EXE)
        (self.t / "rt").mkdir()

        def argv():
            return game.build_plan(self.cfg, install, write=False, log=lambda _t: None).argv

        self.assertFalse(any(a.startswith("--texture_packs") for a in argv()))
        self.cfg.settings["texture_replace"] = True
        self.assertIn("--texture_packs=Alpha|Beta Two", argv())
        self.cfg.d["texture_packs"] = {"order": ["Beta Two", "Alpha"], "disabled": ["Alpha"]}
        self.assertIn("--texture_packs=Beta Two", argv())
        self.cfg.d["texture_packs"]["disabled"] = ["Alpha", "Beta Two"]
        self.assertIn("--texture_packs=|", argv())
        # The player's own folder or list: theirs alone.
        self.cfg.advanced["extra_args"] = "--texture_pack_root=/elsewhere"
        self.assertFalse(any(a.startswith("--texture_packs") for a in argv()))


class AddingPacks(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.t = Path(self.tmp.name)
        self.cfg = Config(self.t / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.t / "home")
        self.dir = texture_packs.textures_dir(self.cfg)
        self.src = self.t / "downloads"
        self.src.mkdir()

    def names(self):
        return sorted(p.name for p in self.dir.iterdir()) if self.dir.exists() else []

    def test_a_zip_in_one_folder_and_only_its_textures(self):
        z = make_zip(self.src / "rocks-v2.zip", {
            "Rocks HD/pack.toml": 'name = "Rocks HD"\n',
            f"Rocks HD/textures/{FP[0]}.dds": dds(),
            "Rocks HD/README.md": "hi",
            "Rocks HD/run-me.exe": "MZ",
        })
        pack = texture_packs.install(z, self.dir)
        self.assertEqual((pack.folder, pack.title, pack.files), ("Rocks HD", "Rocks HD", 1))
        got = sorted(p.relative_to(self.dir / "Rocks HD").as_posix() for p in (self.dir / "Rocks HD").rglob("*")
                     if p.is_file())
        self.assertEqual(got, ["README.md", "pack.toml", f"textures/{FP[0]}.dds"])
        self.assertEqual(self.names(), ["Rocks HD"])  # no staging folder left
        with self.assertRaises(texture_packs.PackExists):
            texture_packs.install(z, self.dir)
        self.assertEqual(texture_packs.install(z, self.dir, replace=True).folder, "Rocks HD")
        texture_packs.remove(self.cfg, "Rocks HD")
        self.assertEqual(self.names(), [])

    def test_refused_in_plain_words(self):
        slip = make_zip(self.src / "slip.zip", {f"../{FP[0]}.dds": dds()})
        absolute = make_zip(self.src / "abs.zip", {f"/tmp/{FP[0]}.dds": dds()})
        link_info = zipfile.ZipInfo(f"{FP[0]}.dds")
        link_info.external_attr = (0o120777 << 16)
        linked = make_zip(self.src / "link.zip", {"x": link_info})
        dump = make_zip(self.src / "mydump.zip", {"dump/index.tsv": "x", f"dump/{FP[0]}.dds": dds()})
        nothing = make_zip(self.src / "nothing.zip", {"readme.txt": "x", "a.dds": dds()})
        unusable = make_zip(self.src / "cube.zip", {f"{FP[0]}.dds": dds(caps2=0x200)})
        bad = self.src / "bad.zip"
        bad.write_bytes(b"nope")
        cases = {slip: "outside its own folder", absolute: "outside its own folder", linked: "link",
                 dump: "texture dump", nothing: "holds no texture pack", unusable: "would not be used",
                 bad: "not a zip", self.src / "gone.zip": "not there"}
        for source, words in cases.items():
            with self.subTest(source=source.name):
                with self.assertRaises(texture_packs.InstallError) as e:
                    texture_packs.install(source, self.dir)
                self.assertIn(words, str(e.exception))
        self.assertFalse((self.t / f"{FP[0]}.dds").exists())
        self.assertEqual([n for n in self.names() if not n.startswith(".")], [])
        with patch.object(texture_packs, "MAX_INSTALL", 10):
            big = make_zip(self.src / "big.zip", {f"{FP[0]}.dds": dds()})
            with self.assertRaises(texture_packs.InstallError) as e:
                texture_packs.install(big, self.dir)
            self.assertIn("32 GB", str(e.exception))

    def test_a_folder_a_made_pack_and_the_dump_check(self):
        make_pack(self.src, "hd-pack", {f"{FP[0]}.png": png(), f"{FP[1]}.png": png()})
        (self.dir / "dump").mkdir(parents=True)
        (self.dir / "dump" / f"{FP[0]}.png").write_bytes(png())
        pack = texture_packs.install(self.src / "hd-pack", self.dir)
        self.assertEqual(pack.folder, "hd-pack (added)")  # Make HD pack's folder stays its own
        self.assertIn("1 file(s) are the game's own textures unchanged", pack.note)
        with self.assertRaises(texture_packs.InstallError) as e:
            texture_packs.install(self.dir / "hd-pack (added)", self.dir)
        self.assertIn("already in the textures folder", str(e.exception))
        with self.assertRaises(texture_packs.InstallError):
            texture_packs.remove(self.cfg, "dump")
        self.assertTrue((self.dir / "dump").is_dir())


@unittest.skipIf(QApplication is None, "PySide6 is not installed")
class TexturePacksPageTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from forza_launcher.ui.texture_packs_page import TexturePacksPage
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.t = Path(self.tmp.name)
        self.cfg = Config(self.t / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.t / "home")
        self.dir = texture_packs.textures_dir(self.cfg)
        make_pack(self.dir, "Alpha", {f"{FP[0]}.dds": dds()}, 'name = "Alpha pack"\n')
        make_pack(self.dir, "Beta", {f"{FP[0]}.png": png(), f"{FP[1]}.png": png()})
        self.page = TexturePacksPage(self.cfg, window=None)

    def tearDown(self):
        self.page.deleteLater()
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)

    def names(self):
        return [self.page.table.item(r, 0).text() for r in range(self.page.table.rowCount())]

    def test_lists_orders_and_shows_the_clashes(self):
        self.assertEqual(self.names(), ["Alpha pack", "Beta"])
        self.assertIn("2 of 2 texture pack(s) will be used", self.page.message.text())
        self.assertIn("Use texture packs is off", self.page.message.text())
        self.page.table.selectRow(0)
        self.assertIn("Same texture(s) as Beta: " + FP[0] + "; Beta's copy is used", self.page.details.text())
        self.page.move(1)
        self.assertEqual(self.cfg.d["texture_packs"]["order"], ["Beta", "Alpha"])
        self.page.table.item(0, 0).setCheckState(Qt.Unchecked)
        self.assertEqual(self.cfg.d["texture_packs"]["disabled"], ["Beta"])

    def test_the_switches_are_the_settings(self):
        self.page.switches["texture_replace"].setChecked(True)
        self.assertTrue(self.cfg.settings["texture_replace"])
        self.page.switches["texture_dump"].setChecked(True)
        self.assertTrue(self.cfg.settings["texture_dump"])
        self.cfg.settings["gpu_backend"] = "d3d12"
        self.page.refresh()
        self.assertFalse(self.page.switches["texture_replace"].isEnabled())
        self.assertFalse(self.page.vulkan_note.isHidden())

    def test_add_a_dropped_zip_and_remove_it(self):
        from PySide6.QtCore import QMimeData, QPointF, QUrl
        from PySide6.QtGui import QDropEvent
        from unittest import mock
        z = make_zip(self.t / "Gamma.zip", {f"{FP[2]}.dds": dds()})
        data = QMimeData()
        data.setUrls([QUrl.fromLocalFile(str(z))])
        self.page.dropEvent(QDropEvent(QPointF(5, 5), Qt.CopyAction, data, Qt.LeftButton, Qt.NoModifier))
        self.assertEqual(self.names(), ["Alpha pack", "Beta", "Gamma"])
        self.assertIn("Added Gamma (1 texture(s))", self.page.message.text())
        self.page.table.selectRow(2)
        with mock.patch.object(self.page, "_confirm", return_value=True):
            self.page.remove_selected()
        self.assertEqual(self.names(), ["Alpha pack", "Beta"])



if __name__ == "__main__":
    unittest.main()
