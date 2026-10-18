"""Qt save lifecycle checks using only disposable data and fake game processes."""
import os
os.environ.setdefault('QT_QPA_PLATFORM', 'offscreen')
from pathlib import Path
from types import SimpleNamespace
import tempfile
import threading
import time
import unittest
from unittest.mock import Mock, patch

try:
    from PySide6.QtWidgets import QApplication, QMessageBox
except ImportError:
    QApplication = None

from forza_launcher import game, saves
from forza_launcher.config import Config


@unittest.skipIf(QApplication is None, 'PySide6 is not installed')
class SaveUITests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.app = QApplication.instance() or QApplication([])

    def setUp(self):
        from forza_launcher.ui.window import MainWindow
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.patches = [patch('forza_launcher.paths.config_dir', return_value=self.root),
                        patch('forza_launcher.datafolder.game_default_data_home', return_value=self.root/'legacy')]
        for p in self.patches: p.start()
        cfg = Config(self.root/'launcher.json')
        cfg.advanced['data_root'] = str(self.root/'data')
        cfg.gpu['checked'] = True
        cfg.loaded = True
        cfg.d['check_updates'] = False
        self.win = MainWindow(cfg)
        self.page = self.win.saves
        self.profile = saves.profile_for(cfg)
        self.profile.folder.mkdir(parents=True)
        self.file = self.profile.folder/'career'
        self.file.write_bytes(b'career one')

    def tearDown(self):
        self.page.cancel()
        self.wait_done()
        self.win.close()
        self.app.processEvents()
        for p in reversed(self.patches): p.stop()
        self.temp.cleanup()

    def wait_done(self):
        deadline = time.monotonic()+8
        while self.page.busy() and time.monotonic() < deadline:
            self.app.processEvents()
            time.sleep(.005)
        self.app.processEvents()
        self.assertFalse(self.page.busy(), 'Save thread did not finish')

    def test_named_backup_restore_and_profile_change_guard(self):
        self.win.show_page(4)
        self.wait_done()
        self.page.name.setText('Before the finale')
        self.page.create_button.click()
        self.wait_done()
        self.assertEqual(self.page.table.rowCount(), 1)
        self.assertEqual(self.page.entries[0].label, 'Before the finale')
        self.file.write_bytes(b'career two')
        self.page.table.selectRow(0)
        with patch.object(QMessageBox, 'question', return_value=QMessageBox.Yes):
            self.page.restore_button.click()
            self.wait_done()
        self.assertEqual(self.file.read_bytes(), b'career one')
        self.assertEqual(self.page.table.rowCount(), 2)
        self.win.cfg.profile.update(mode='xenia', xenia_xuid='0000000000000001')
        self.page._enable()
        self.assertFalse(self.page.create_button.isEnabled())
        self.assertFalse(self.page.restore_button.isEnabled())

    def test_automatic_backup_finishes_before_launch_and_failure_blocks_launch(self):
        launched = []
        main_thread = threading.get_ident()
        copy = saves._copy
        workers = []
        def record(*args):
            workers.append(threading.get_ident())
            return copy(*args)
        def launch():
            self.assertEqual(len(saves.snapshots(self.profile)[0]), 1)
            launched.append(True)
        with patch.object(saves, '_copy', side_effect=record):
            self.page.before_launch(SimpleNamespace(save_profile=self.profile), launch)
            self.wait_done()
        self.assertEqual(launched, [True])
        self.assertTrue(workers)
        self.assertNotIn(main_thread, workers)
        with patch.object(saves, 'create', side_effect=saves.SaveError('Disk full')):
            self.page.before_launch(SimpleNamespace(save_profile=self.profile), lambda: launched.append(False))
            self.wait_done()
        self.assertEqual(launched, [True])
        self.assertIn('Disk full', self.win.play.warn.text())

    def test_cancel_and_close_wait_for_worker_and_never_launch(self):
        gate, started = threading.Event(), threading.Event()
        launched = []
        def slow(*args, **kwargs):
            started.set()
            gate.wait(3)
        with patch.object(saves, 'create', side_effect=slow):
            self.page.before_launch(SimpleNamespace(save_profile=self.profile), lambda: launched.append(True))
            self.assertTrue(started.wait(2))
            self.page.cancel()
            self.win.close()
            self.assertTrue(self.page.closing)
            gate.set()
            self.wait_done()
        self.assertFalse(launched)

    def test_running_game_blocks_write_buttons_and_preferences_persist(self):
        self.page.refresh()
        self.wait_done()
        self.win.session = Mock()
        self.win.session.poll.return_value = None
        self.page._enable()
        self.assertFalse(self.page.create_button.isEnabled())
        self.assertFalse(self.page.restore_button.isEnabled())
        self.win.session = None
        self.page.automatic.setChecked(False)
        self.page.retention.setValue(3)
        prefs = Config(self.win.cfg.path).d['save_manager']
        self.assertEqual(prefs, dict(automatic=False, keep_automatic=3))

    def test_closing_waits_for_the_game_to_release_its_save_lock(self):
        session = Mock()
        session.poll.return_value = None
        self.win.session = session
        self.win.close()
        self.assertTrue(self.win._close_after_game)
        session.stop.assert_called_once()
        session.cleanup.assert_not_called()
        session.poll.return_value = 0
        self.win._poll()
        session.cleanup.assert_called_once()
        self.assertIsNone(self.win.session)

    def test_editor_load_apply_and_backup_restore_from_the_saves_page(self):
        from forza_launcher import save_editor
        from test_save_editor import KEYS, profile_bytes
        path = self.profile.folder/'ForzaProfile/ForzaProfile'
        path.parent.mkdir()
        original = profile_bytes()
        path.write_bytes(original)
        with patch.object(save_editor, '_keys', return_value=(KEYS,)):
            self.page.refresh()
            self.wait_done()
            self.page.edit_button.click()
            self.wait_done()
            dialog = self.page.editor
            self.assertIsNotNone(dialog.document)
            self.assertEqual(dialog.controls['Main/Credits'].value(), 500)
            dialog.controls['Main/Credits'].setValue(1500)
            dialog.controls['Main/XP'].setValue(5000)
            dialog.apply_button.click()
            self.wait_done()
            self.assertIn('Changes saved', dialog.message.text())
            self.assertEqual(dialog.document.values['Main/XP'].value, 5000)
            backup = saves.snapshots(self.profile)[0][0]
            self.assertEqual(backup.kind, 'before-edit')
            self.assertEqual((backup.path/'save/ForzaProfile/ForzaProfile').read_bytes(), original)
            dialog.close()
            self.wait_done()
            self.assertEqual(self.page.table.item(0, 2).text(), 'Before edit')
            saves.restore(self.profile, backup)
            self.assertEqual(path.read_bytes(), original)

    def test_editor_failure_is_visible_and_leaves_no_enabled_apply_action(self):
        self.page.refresh()
        self.wait_done()
        self.page.edit_button.click()
        self.wait_done()
        dialog = self.page.editor
        self.assertIsNone(dialog.document)
        self.assertFalse(dialog.apply_button.isEnabled())
        self.assertTrue(dialog.reload_button.isEnabled())
        self.assertTrue(dialog.message.text())
        dialog.close()
        self.wait_done()


class SaveSessionTests(unittest.TestCase):
    def test_session_lock_survives_relaunch_and_releases_on_cleanup_or_start_failure(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            profile = saves.Profile(root, root, 'local', 'Test')
            plan = game.LaunchPlan(None, root, ['fake-game'], {}, [], root/'forza.toml', '', save_profile=profile)
            session = game.Session(plan)
            with patch.object(game.subprocess, 'Popen'):
                session.start()
                with self.assertRaises(saves.SaveError): saves.create(profile)
                session.relaunch()
                with self.assertRaises(saves.SaveError): saves.create(profile)
                session.cleanup()
                self.assertIsNone(saves.create(profile))
            with patch.object(game.subprocess, 'Popen', side_effect=OSError('failed')):
                with self.assertRaises(OSError): session.start()
            self.assertIsNone(saves.create(profile))


if __name__ == '__main__': unittest.main()
