"""Photo discovery, decoding, export, and Qt lifecycle tests with disposable saves."""
import os
os.environ.setdefault("QT_QPA_PLATFORM", "offscreen")
from pathlib import Path
import sys
import tempfile
import threading
import time
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import gallery, game
from forza_launcher.config import Config

try:
    from PySide6.QtCore import QByteArray, QBuffer, QCoreApplication, QEvent, QIODevice, Qt
    from PySide6.QtGui import QImage, QColor
    from PySide6.QtWidgets import QApplication
except ImportError:
    QApplication = None


@unittest.skipIf(QApplication is None, "PySide6 is not installed")
class GalleryTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.cfg = Config(self.root / "launcher.json")
        self.cfg.advanced["data_root"] = str(self.root / "data")
        self.profile = gallery.profile_folder(self.cfg)
        self.page = None
        self.log_patch = patch("forza_launcher.paths.config_dir", return_value=self.root)
        self.log_patch.start()

    def tearDown(self):
        if self.page:
            self.page.shutdown()
            self.wait(lambda: self.page.task is None and self.page.preview is None)
            self.page.close()
            self.page.deleteLater()
        # processEvents alone does not run deferred deletion without an exec()
        # event loop. Dispose Qt workers/windows before Python tears Qt down.
        QCoreApplication.sendPostedEvents(None, QEvent.DeferredDelete)
        self.log_patch.stop()
        self.temp.cleanup()

    def wait(self, condition):
        end = time.monotonic() + 5
        while not condition() and time.monotonic() < end:
            self.app.processEvents()
            time.sleep(.005)
        self.app.processEvents()
        self.assertTrue(condition(), "Photo task did not finish")

    def encoded(self, width=1280, height=720, fmt="JPEG"):
        image = QImage(width, height, QImage.Format_RGB32)
        image.fill(QColor("#fc9b3e"))
        data = QByteArray()
        buffer = QBuffer(data)
        buffer.open(QIODevice.WriteOnly)
        self.assertTrue(image.save(buffer, fmt))
        return bytes(data)

    def photo(self, title="Colorado", data=None, profile=None):
        path = (profile or self.profile) / "photo" / title / "image"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_bytes(data if data is not None else self.encoded())
        return path

    def page_ready(self):
        from forza_launcher.ui.gallery_page import GalleryPage
        self.page = GalleryPage(self.cfg, None)
        self.page.resize(800, 720)
        self.page.show()
        self.page.refresh()
        self.wait(lambda: self.page.task is None)
        return self.page

    def test_empty_profile_is_not_created(self):
        self.assertEqual(list(gallery.photo_groups(self.profile)), [])
        self.assertFalse(self.profile.exists())

    def test_xenia_uses_local_copy_and_validates_identity(self):
        self.cfg.profile.update(mode="xenia", xenia_xuid="e030000004b490fa")
        result = gallery.profile_folder(self.cfg)
        self.assertEqual(result, self.cfg.data_home() / "xenia-E030000004B490FA" /
                         game.LOCAL_PROFILE / "4D5309C9" / "00000001")
        self.assertFalse(result.exists())
        self.cfg.profile["xenia_xuid"] = "../wrong"
        with self.assertRaises(ValueError):
            gallery.profile_folder(self.cfg)
        self.cfg.profile["xenia_xuid"] = ""
        with patch.object(game, "xenia_candidates", return_value=["A" * 16]):
            self.assertIn("xenia-" + "A" * 16, str(gallery.profile_folder(self.cfg)))
        with patch.object(game, "xenia_candidates", return_value=["A" * 16, "B" * 16]):
            with self.assertRaises(ValueError):
                gallery.profile_folder(self.cfg)

    def test_discovery_excludes_car_thumbnails_and_other_titles(self):
        wanted = self.photo()
        for path in (self.profile / "ForzaProfile" / "Thumbnails" / "car.jpg",
                     self.profile.parent.parent / "OTHERGAME" / "00000001" / "photo" / "image"):
            path.parent.mkdir(parents=True, exist_ok=True)
            path.write_bytes(self.encoded())
        groups = list(gallery.photo_groups(self.profile))
        self.assertEqual(groups, [(wanted.parent, [wanted])])
        self.assertEqual(list(gallery.photo_groups(self.profile, lambda: True)), [])

    def test_extensionless_and_wrapped_jpeg_choose_full_size(self):
        path = self.photo(data=b"Save header" + self.encoded(160, 90) + b"payload" + self.encoded())
        image, w, h = gallery.read_photo(path, (400, 225))
        self.assertEqual((w, h), (1280, 720))
        self.assertEqual((image.width(), image.height()), (400, 225))
        (path.parent / "thumbnail").write_bytes(self.encoded(80, 45))
        result = gallery.load_group(path.parent, list(path.parent.iterdir()))
        self.assertEqual(result[0].path, path)

    def test_corrupt_empty_oversized_and_large_dimensions(self):
        path = self.photo(data=b"incomplete")
        with self.assertRaises(ValueError):
            gallery.read_photo(path)
        path.write_bytes(b"")
        with self.assertRaises(ValueError):
            gallery.read_photo(path)
        path.write_bytes(self.encoded())
        with patch.object(gallery, "MAX_FILE_BYTES", 8):
            with self.assertRaises(ValueError):
                gallery.read_photo(path)
        with patch.object(gallery, "MAX_PIXELS", 100):
            with self.assertRaises(ValueError):
                gallery.read_photo(path)

    def test_export_roundtrip_preserves_save_and_failed_destination(self):
        path = self.photo()
        before = path.read_bytes()
        image, _, _ = gallery.read_photo(path)
        for suffix in (".png", ".jpg"):
            dest = self.root / ("export" + suffix)
            gallery.export_photo(image, dest, self.profile)
            decoded, w, h = gallery.read_photo(dest)
            self.assertEqual((w, h), (1280, 720))
            self.assertEqual(decoded.pixelColor(20, 20), image.pixelColor(20, 20))
        with self.assertRaises(ValueError):
            gallery.export_photo(image, path.parent / "new.png", self.profile)
        destination = self.root / "previous.png"
        destination.write_bytes(b"keep this")
        with self.assertRaises(OSError):
            gallery.export_photo(QImage(), destination, self.profile)
        self.assertEqual(destination.read_bytes(), b"keep this")
        self.assertEqual(path.read_bytes(), before)
        self.assertEqual(list(self.root.glob(".forza-photo-*")), [])

    def test_caption_free_grid_sort_and_preview_navigation(self):
        first = self.photo("Alpine")
        second = self.photo("Sunset", self.encoded(800, 600))
        os.utime(first, (1000, 1000))
        os.utime(second, (2000, 2000))
        page = self.page_ready()
        self.assertEqual(page.grid.count(), 2)
        self.assertEqual(page.grid.item(0).data(Qt.UserRole).path, second)
        self.assertEqual(page.grid.item(0).text(), "")
        self.assertIn("Photo 1", page.grid.item(0).data(Qt.AccessibleTextRole))
        page.sort.setCurrentIndex(1)
        self.assertEqual(page.grid.item(0).data(Qt.UserRole).path, first)
        page.open_photo(page.grid.item(1))
        self.wait(lambda: page.preview.task is None)
        self.assertEqual(page.preview.image.width(), 800)
        self.assertFalse(page.preview.next.isEnabled())
        page.preview.reject()
        self.app.processEvents()
        page.open_photo(page.grid.item(0))
        self.wait(lambda: page.preview.task is None)
        page.preview.navigate(1)
        self.wait(lambda: page.preview.task is None)
        self.assertEqual(page.preview.heading.text(), "Photo 2 of 2")
        self.assertNotIn("Sunset", page.preview.windowTitle())

    def test_refresh_switches_profiles_and_reports_bad_photo(self):
        self.photo()
        self.photo("Still saving", b"unfinished")
        page = self.page_ready()
        self.assertIn("1 folder(s)", page.message.text())
        self.cfg.advanced["data_root"] = str(self.root / "other")
        page.refresh()
        self.wait(lambda: page.task is None)
        self.assertEqual(page.grid.count(), 0)
        self.assertIn("No saved photos", page.empty.text())
        self.assertFalse((self.root / "other").exists())

    def test_profile_change_during_scan_discards_previous_results(self):
        self.photo()
        page = self.page_ready()
        started, release = threading.Event(), threading.Event()
        original = gallery.load_group

        def slow(*args, **kwargs):
            started.set()
            release.wait(3)
            return original(*args, **kwargs)

        with patch.object(gallery, "load_group", side_effect=slow):
            page.refresh()
            self.assertTrue(started.wait(1))
            self.cfg.advanced["data_root"] = str(self.root / "different-profile")
            page.refresh()
            release.set()
            self.wait(lambda: page.task is None)
        self.assertEqual(page.grid.count(), 0)
        self.assertIn("different-profile", str(page.profile))

    def test_main_window_navigation_and_close_wait_for_gallery(self):
        from forza_launcher.ui.window import MainWindow
        self.cfg.loaded = True
        self.cfg.gpu["checked"] = True
        self.cfg.d["check_updates"] = False
        started, release = threading.Event(), threading.Event()

        def slow_groups(*args):
            started.set()
            release.wait(3)
            return iter(())

        with patch.object(game, "find_game", return_value=(None, [])), \
                patch.object(gallery, "photo_groups", side_effect=slow_groups):
            window = MainWindow(self.cfg)
            window.show()
            window.show_page(4)
            self.assertTrue(started.wait(1))
            self.assertIs(window.stack.currentWidget(), window.gallery)
            window.show_page(3)
            self.assertIs(window.stack.currentWidget(), window.about)
            window.close()
            self.assertTrue(window.isVisible())
            release.set()
            self.wait(lambda: not window.isVisible())
            self.assertIsNone(window.gallery.task)
            window.deleteLater()
            self.app.processEvents()

    def test_close_during_scan_and_preview_does_not_destroy_worker(self):
        self.photo()
        page = self.page_ready()
        started, release = threading.Event(), threading.Event()
        original = gallery.read_photo

        def slow(*args, **kwargs):
            started.set()
            release.wait(3)
            return original(*args, **kwargs)

        with patch.object(gallery, "read_photo", side_effect=slow):
            page.open_photo(page.grid.item(0))
            self.assertTrue(started.wait(1))
            self.assertFalse(page.shutdown())
            self.assertIsNotNone(page.preview.task)
            release.set()
            self.wait(lambda: page.preview is None)


if __name__ == "__main__":
    unittest.main()
