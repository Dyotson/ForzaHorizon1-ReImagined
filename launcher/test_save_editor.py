"""Synthetic authenticated profiles; no real game keys or save data in fixtures."""
from pathlib import Path
import hmac
import os
import struct
import tempfile
import unittest
from unittest.mock import patch

from forza_launcher import save_editor as editor, saves


KEYS = (bytes(range(16)), bytes(range(32)))


def field(name, kind, value):
    name = name.encode()
    data = value if kind == 15 else struct.pack('>'+editor.FORMATS[kind], value)
    return struct.pack('>I', len(name))+name+struct.pack('>IIB', 0x20, 0, kind)+data


def body():
    main = struct.pack('>I', 6) + field('Credits', 3, 500) + field('XP', 7, 100) + field('Level', 3, 2)
    main += field('WristbandLevel', 7, 1)+field('UnrelatedFlag', 0, True)+field('UnknownCounter', 4, 2**40)
    return struct.pack('>I', 1)+field('Main', 15, main)+b'opaque class state\0\xff\xbb'


def profile_bytes(data=None):
    data = body() if data is None else data
    padding = b'\xbb'*(-len(data)%16)
    iv = bytes(range(16))
    seed = struct.pack('>I', len(data) ^ editor.reduce(editor.operator.xor, struct.unpack('>4I', iv)))+iv
    payload = seed+editor._encrypt_cbc(KEYS[0], data+padding, iv)
    return b'cmss'+struct.pack('>I', 32+len(payload))+hmac.digest(KEYS[1], payload, 'sha256')+payload+b'untouched file capacity'


class EditorTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        root = Path(self.temp.name).resolve()
        self.profile = saves.Profile(root, root/'data', 'local', 'Fixture')
        self.file = self.profile.folder/'ForzaProfile/ForzaProfile'
        self.file.parent.mkdir(parents=True)
        self.original = profile_bytes()
        self.file.write_bytes(self.original)
        (self.file.parent/'PlayerDatabase').write_bytes(b'untouched companion')
        self.doc = editor.decode(self.profile, self.original, (KEYS,))

    def test_nist_aes_cbc_vector_with_standard_library_fallback(self):
        key = bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c')
        iv = bytes.fromhex('000102030405060708090a0b0c0d0e0f')
        data = bytes.fromhex('6bc1bee22e409f96e93d7e117393172aae2d8a571e03ac9c9eb76fac45af8e51')
        expected = bytes.fromhex('7649abac8119b246cee98e9b12e9197d5086cb9b507219ee95db113a917678b2')
        with patch.dict('sys.modules', {'cryptography': None}):
            self.assertEqual(editor._encrypt_cbc(key, data, iv), expected)

    def test_no_edit_round_trip_is_byte_identical(self):
        self.assertEqual(editor.encode(self.doc, {}), self.original)
        result = editor.apply(self.doc, {'Main/Credits': 500})
        self.assertIsNone(result.backup)
        self.assertEqual(saves.snapshots(self.profile), ([], 0))

    def test_key_derivation_uses_separate_aes_and_hmac_table_blocks(self):
        from forza_launcher.config import Config
        cfg = Config(self.profile.home/'launcher.json')
        cfg['game_root'] = str(self.profile.home/'game')
        cfg.game_files().mkdir()
        (cfg.game_files()/'default.xex').write_bytes(b'fake')
        table = bytes(range(64))
        owner = bytes.fromhex('B13EBABEBABEBABE')
        with patch.object(editor, '_tables', return_value=(table,)):
            aes, mac = editor._keys(cfg)[0]
        self.assertEqual(aes, bytes(((i ^ owner[(i-2)%8])+2*i)%256 for i in range(16)))
        self.assertEqual(mac, bytes((((i+32) ^ owner[(i-4)%8])+2*i)%256 for i in range(32)))

    def test_edit_changes_only_allowed_body_bytes_and_preserves_capacity(self):
        changed = editor.encode(self.doc, {'Main/Credits': 1_000_000, 'Main/XP': 450_000})
        after = editor.decode(self.profile, changed, (KEYS,))
        self.assertEqual(after.values['Main/Credits'].value, 1_000_000)
        self.assertEqual(after.values['Main/XP'].value, 450_000)
        allowed = {offset+i for path in editor.EDITABLE for offset in (self.doc.values[path].offset,) for i in range(4)}
        self.assertTrue(all(a == b or i in allowed for i, (a, b) in enumerate(zip(self.doc.body, after.body))))
        self.assertEqual(after.padding, self.doc.padding)
        self.assertTrue(changed.endswith(b'untouched file capacity'))
        self.assertEqual(len(changed), len(self.original))

    def test_apply_keeps_full_before_edit_backup_and_can_restore_it(self):
        result = editor.apply(self.doc, {'Main/Credits': 1000})
        self.assertEqual(result.backup.kind, 'before-edit')
        self.assertEqual(result.document.values['Main/Credits'].value, 1000)
        self.assertEqual((result.backup.path/'save/ForzaProfile/ForzaProfile').read_bytes(), self.original)
        self.assertEqual((self.file.parent/'PlayerDatabase').read_bytes(), b'untouched companion')
        saves.restore(self.profile, result.backup)
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_stale_document_cannot_overwrite_newer_save(self):
        self.file.write_bytes(b'newer save')
        with self.assertRaisesRegex(saves.SaveError, 'changed'): editor.apply(self.doc, {'Main/Credits': 999})
        self.assertEqual(self.file.read_bytes(), b'newer save')

    def test_game_lock_blocks_load_and_edit(self):
        with saves.locked(self.profile):
            with self.assertRaises(saves.SaveError): editor.apply(self.doc, {'Main/Credits': 999})
            with self.assertRaises(saves.SaveError): editor.load(None, self.profile)

    def test_tampered_ciphertext_and_wrong_owner_keys_fail_authentication(self):
        bad = bytearray(self.original)
        bad[65] ^= 1
        with self.assertRaisesRegex(saves.SaveError, 'integrity'): editor.decode(self.profile, bytes(bad), (KEYS,))
        with self.assertRaises(saves.SaveError): editor.decode(self.profile, self.original, ((bytes(16), bytes(32)),))

    def test_bad_stream_lengths_and_field_layouts_are_rejected(self):
        for length in (0, 67, len(self.original), 0xffffffff):
            bad = bytearray(self.original)
            struct.pack_into('>I', bad, 4, length)
            with self.assertRaises(saves.SaveError): editor.decode(self.profile, bytes(bad), (KEYS,))
        for data in (b'\xff'*4, struct.pack('>I',1)+field('Bad',1,0), body().replace(b'Credits', b'Creditx')):
            with self.assertRaises(saves.SaveError): editor.decode(self.profile, profile_bytes(data), (KEYS,))

    def test_edit_limits_and_unknown_fields_are_rejected(self):
        for path, value in (('Main/Credits', -1), ('Main/Credits', 1_000_000_000), ('Main/XP', 2**31),
                            ('Main/XP', True), ('Main/Level', 50), ('Main/Credits', 1.5)):
            with self.subTest(path=path, value=value):
                with self.assertRaises(saves.SaveError): editor.apply(self.doc, {path:value})
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_cancel_and_backup_failure_do_not_replace_the_profile(self):
        with self.assertRaises(saves.Cancelled):
            editor.apply(self.doc, {'Main/Credits': 1000}, cancelled=lambda: True)
        with patch.object(saves, '_create', side_effect=OSError('disk full')):
            with self.assertRaises(OSError): editor.apply(self.doc, {'Main/Credits': 1000})
        self.assertEqual(self.file.read_bytes(), self.original)

    def test_failed_atomic_replace_leaves_current_save_and_safety_backup(self):
        real_replace = os.replace
        def fail(source, target):
            if Path(source).name.startswith('.profile-edit-'):
                raise PermissionError('fixture failure')
            return real_replace(source, target)
        with patch.object(editor.os, 'replace', side_effect=fail):
            with self.assertRaises(PermissionError): editor.apply(self.doc, {'Main/Credits': 1000})
        self.assertEqual(self.file.read_bytes(), self.original)
        self.assertEqual(len(saves.snapshots(self.profile)[0]), 1)
        self.assertFalse(list(self.file.parent.glob('.profile-edit-*')))

    def test_game_save_changes_during_backup_are_not_overwritten(self):
        create = saves._create
        def changed(*args):
            result = create(*args)
            self.file.write_bytes(b'written by another process')
            return result
        with patch.object(saves, '_create', side_effect=changed):
            with self.assertRaisesRegex(saves.SaveError, 'changed'): editor.apply(self.doc, {'Main/Credits': 1000})
        self.assertEqual(self.file.read_bytes(), b'written by another process')


if __name__ == '__main__': unittest.main()
