"""Snapshots only touch disposable fixtures; no installed save is discovered."""
import hashlib
import json
from pathlib import Path
import os
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import saves, game, TITLE_ID
from forza_launcher.config import Config


class SaveTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        legacy = patch('forza_launcher.datafolder.game_default_data_home', return_value=self.root/'unused-legacy')
        legacy.start()
        self.addCleanup(legacy.stop)
        self.cfg = Config(self.root / 'launcher.json')
        self.cfg.advanced['data_root'] = str(self.root / 'data')
        self.profile = saves.profile_for(self.cfg)
        self.profile.folder.mkdir(parents=True)
        self.write('ForzaProfile/ProfileData', b'career one')
        self.write('Photo/One', b'photo one')

    def write(self, name, data):
        file = self.profile.folder / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_bytes(data)

    def test_named_backup_and_restore_preserve_previous_save_and_remove_new_files(self):
        first = saves.create(self.profile, 'Before the race')
        self.assertEqual(first.label, 'Before the race')
        self.assertEqual((first.files, first.size), (2, 19))
        self.write('ForzaProfile/ProfileData', b'career two')
        self.write('ForzaProfile/new-file', b'new')
        safety = saves.restore(self.profile, first)
        self.assertEqual((self.profile.folder/'ForzaProfile/ProfileData').read_bytes(), b'career one')
        self.assertFalse((self.profile.folder/'ForzaProfile/new-file').exists())
        self.assertEqual(safety.kind, 'before-restore')
        self.assertEqual((safety.path/'save/ForzaProfile/ProfileData').read_bytes(), b'career two')

    def test_automatic_dedup_and_retention_leave_named_and_safety_backups(self):
        manual = saves.create(self.profile, 'Keep this')
        self.assertEqual(saves.create(self.profile, kind='automatic').path, manual.path)
        for i in range(4):
            self.write('ForzaProfile/ProfileData', str(i).encode())
            saves.create(self.profile, kind='automatic', keep=2)
        entries, errors = saves.snapshots(self.profile)
        self.assertEqual(errors, 0)
        self.assertEqual(sum(s.kind == 'automatic' for s in entries), 2)
        self.assertIn(manual, entries)

    def test_corrupt_existing_backup_does_not_suppress_a_fresh_automatic_backup(self):
        bad = saves.create(self.profile, 'Old')
        (bad.path/'save/ForzaProfile/ProfileData').write_bytes(b'corruption')
        fresh = saves.create(self.profile, kind='automatic')
        self.assertNotEqual(fresh.path, bad.path)
        self.assertEqual((fresh.path/'save/ForzaProfile/ProfileData').read_bytes(), b'career one')

    def test_corruption_is_rejected_before_replacing_current_save(self):
        snap = saves.create(self.profile)
        (snap.path/'save/Photo/One').write_bytes(b'wrong one')
        with self.assertRaisesRegex(saves.SaveError, 'integrity'):
            saves.restore(self.profile, snap)
        self.assertEqual((self.profile.folder/'Photo/One').read_bytes(), b'photo one')
        self.assertEqual(len(saves.snapshots(self.profile)[0]), 1)

    def test_missing_and_extra_files_are_rejected(self):
        snap = saves.create(self.profile)
        (snap.path/'save/extra').write_bytes(b'bad')
        with self.assertRaisesRegex(saves.SaveError, 'unexpected'):
            saves.restore(self.profile, snap)
        (snap.path/'save/extra').unlink()
        (snap.path/'save/Photo/One').unlink()
        with self.assertRaisesRegex(saves.SaveError, 'incomplete'):
            saves.restore(self.profile, snap)

    def test_unsafe_manifest_paths_and_cross_profile_backups_are_rejected(self):
        for name in ('../outside', '/outside', 'C:/outside', 'folder\\outside', 'folder/./file'):
            with self.subTest(name=name):
                snap = saves.create(self.profile)
                path = snap.path/'snapshot.json'
                data = json.loads(path.read_text())
                data['files'][0]['path'] = name
                data['fingerprint'] = hashlib.sha256(json.dumps(data['files'], sort_keys=True).encode()).hexdigest()
                path.write_text(json.dumps(data))
                with self.assertRaises(saves.SaveError): saves.restore(self.profile, snap)
        other = saves.Profile(self.profile.home, self.profile.root, 'xenia-0000000000000001', 'Other')
        with self.assertRaises(saves.SaveError): saves.restore(other, snap)

    def test_lock_prevents_save_operations_during_a_game_session(self):
        with saves.locked(self.profile):
            with self.assertRaisesRegex(saves.SaveError, 'Close the game'):
                saves.create(self.profile)
        self.assertIsNotNone(saves.create(self.profile))

    def test_cancel_never_publishes_a_partial_backup_or_changes_save(self):
        with self.assertRaises(saves.Cancelled):
            saves.create(self.profile, cancelled=lambda: True)
        self.assertEqual(saves.snapshots(self.profile), ([], 0))
        snap = saves.create(self.profile)
        with self.assertRaises(saves.Cancelled): saves.restore(self.profile, snap, cancelled=lambda: True)
        self.assertEqual((self.profile.folder/'ForzaProfile/ProfileData').read_bytes(), b'career one')
        self.assertFalse(list(self.profile.backups.glob('.pending-*')))

    def test_changed_source_is_not_published_as_a_consistent_backup(self):
        called = []
        def change(*_):
            if not called:
                called.append(True)
                self.write('ForzaProfile/ProfileData', b'changed while copying')
        with self.assertRaises(saves.SaveError): saves.create(self.profile, progress=change)
        self.assertEqual(saves.snapshots(self.profile), ([], 0))

    def test_out_of_space_leaves_the_current_save_untouched(self):
        class Full: free = 0
        with patch.object(saves.shutil, 'disk_usage', return_value=Full()):
            with self.assertRaisesRegex(saves.SaveError, 'space'): saves.create(self.profile)
        self.assertEqual((self.profile.folder/'ForzaProfile/ProfileData').read_bytes(), b'career one')

    def test_failed_directory_replacement_rolls_back_and_keeps_safety_backup(self):
        snap = saves.create(self.profile)
        self.write('ForzaProfile/ProfileData', b'latest career')
        real_replace = saves.os.replace
        def fail_stage(source, target):
            if Path(source).name.startswith('.save-restore-') and Path(target) == self.profile.folder:
                raise PermissionError('test replacement failure')
            return real_replace(source, target)
        with patch.object(saves.os, 'replace', side_effect=fail_stage):
            with self.assertRaises(PermissionError): saves.restore(self.profile, snap)
        self.assertEqual((self.profile.folder/'ForzaProfile/ProfileData').read_bytes(), b'latest career')
        self.assertTrue(any(s.kind == 'before-restore' for s in saves.snapshots(self.profile)[0]))
        self.assertFalse((self.profile.folder.parent/'.save-manager-restore.json').exists())

    def test_interrupted_restore_recovers_the_previous_directory(self):
        parent = self.profile.folder.parent
        old = parent / ('.save-old-'+'a'*32)
        stage = parent / ('.save-restore-'+'b'*32)
        stage.mkdir()
        os.replace(self.profile.folder, old)
        (parent/'.save-manager-restore.json').write_text(json.dumps(dict(old=old.name, stage=stage.name, had_save=True)))
        saves.recover(self.profile)
        self.assertEqual((self.profile.folder/'ForzaProfile/ProfileData').read_bytes(), b'career one')
        self.assertFalse(stage.exists())
        self.assertFalse(old.exists())

    def test_links_in_save_or_backup_are_refused(self):
        outside = self.root/'outside'
        outside.mkdir()
        (outside/'private').write_bytes(b'leave alone')
        link = self.profile.folder/'linked'
        try:
            link.symlink_to(outside, target_is_directory=True)
        except OSError:
            self.skipTest('Creating symlinks is not permitted')
        with self.assertRaises(saves.SaveError): saves.create(self.profile)
        self.assertEqual((outside/'private').read_bytes(), b'leave alone')

    def test_recovery_keeps_a_committed_restore_and_removes_only_its_staging_directories(self):
        parent = self.profile.folder.parent
        old = parent / ('.save-old-'+'a'*32)
        stage = parent / ('.save-restore-'+'b'*32)
        old.mkdir()
        (old/'old-save').write_bytes(b'old')
        stage.mkdir()
        unrelated = parent/'leave-alone'
        unrelated.mkdir()
        (parent/'.save-manager-restore.json').write_text(json.dumps(dict(old=old.name, stage=stage.name, had_save=True)))
        saves.recover(self.profile)
        self.assertTrue((self.profile.folder/'ForzaProfile/ProfileData').is_file())
        self.assertFalse(old.exists())
        self.assertFalse(stage.exists())
        self.assertTrue(unrelated.is_dir())

    def test_invalid_recovery_record_cannot_remove_arbitrary_folders(self):
        journal = self.profile.folder.parent/'.save-manager-restore.json'
        journal.write_text(json.dumps(dict(old='../../outside', stage='.save-restore-'+'b'*32)))
        with self.assertRaises(saves.SaveError): saves.recover(self.profile)
        self.assertTrue((self.profile.folder/'ForzaProfile/ProfileData').is_file())

    def test_xenia_copy_selection_and_custom_root(self):
        original = self.root/'xenia-original/0000000000000001'/TITLE_ID/'00000001'
        original.mkdir(parents=True)
        (original/'save').write_bytes(b'original')
        self.cfg.profile.update(mode='xenia', xenia_xuid='0000000000000001', xenia_content=str(self.root/'xenia-original'))
        copy = saves.profile_for(self.cfg)
        self.assertEqual(copy.root.name, 'xenia-0000000000000001')
        self.assertIsNone(saves.create(copy))
        self.assertEqual((original/'save').read_bytes(), b'original')
        self.cfg.profile['mode'] = 'own'
        self.cfg.advanced['extra_args'] = '"--user_data_root='+str(self.root/'custom saves')+'"'
        custom = saves.profile_for(self.cfg)
        self.assertTrue(custom.key.startswith('custom-'))
        self.assertEqual(custom.root, (self.root/'custom saves').resolve())

    def test_explicit_root_does_not_fall_back_to_the_legacy_save(self):
        legacy = self.root/'unused-legacy'/game.LOCAL_PROFILE/TITLE_ID/'00000001'
        legacy.mkdir(parents=True)
        (legacy/'save').write_bytes(b'legacy')
        saves._remove(self.profile.folder, self.profile.root)
        self.cfg.advanced['extra_args'] = '"--user_data_root='+str(self.profile.root)+'"'
        self.assertEqual(saves.profile_for(self.cfg).root, self.profile.root)

    def test_preferences_are_normalized(self):
        self.cfg.d['save_manager'] = {'automatic': 'yes', 'keep_automatic': -3}
        self.cfg.save()
        self.assertEqual(Config(self.cfg.path).d['save_manager'], {'automatic': False, 'keep_automatic': 1})


if __name__ == '__main__': unittest.main()
