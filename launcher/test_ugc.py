"""Synthetic catalogs and texture pixels only; no player saves or game keys."""
import hashlib
import json
from pathlib import Path
import struct
import sys
import tempfile
import unittest
from unittest.mock import patch
import zlib

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import ugc, paths
paths.ensure_tools_importable()
import xdc_decode as xdc


def thumbnail(width=32, height=32):
    pitch = (width+31) & ~31
    count = max(xdc.tiled_offset(x, y, pitch) for y in range(height) for x in range(width))+4
    pixels = bytearray(count)
    expected = bytearray()
    for y in range(height):
        for x in range(width):
            rgb = bytes((x % 256, y % 256, (x+y) % 256))
            pos = xdc.tiled_offset(x, y, pitch)
            pixels[pos:pos+4] = b'\0'+rgb
            expected.extend(rgb+b'\xff')
    header = bytearray(52)
    struct.pack_into('>II', header, 0x20, 0x86, (height-1) << 13 | (width-1))
    body = bytes(header)+pixels
    compressor = zlib.compressobj(wbits=-15)
    packed = compressor.compress(body)+compressor.flush()
    return struct.pack('>4s4I', b'cxds', 2, len(body), len(packed), len(body))+packed, bytes(expected)


def package(title_id=0x4D5309C9, payload_name='0002_layers_1293.liv'):
    """Minimal read-only STFS layout with a table and one opaque data block."""
    data = bytearray(0xD000)
    data[:4] = b'CON '
    struct.pack_into('>III', data, 0x340, 0x971A, 1, 2)
    struct.pack_into('>I', data, 0x360, title_id)
    data[0x37B] = 1  # one copy of the block-hash table
    struct.pack_into('<H', data, 0x37C, 1)
    struct.pack_into('>II', data, 0x395, 2, 0)
    entries = [('1293', True, 0xFFFF, 0, b''), ('Fixture', True, 0, 0, b''),
               (payload_name, False, 1, 1, b'opaque encrypted payload'),
               ('Original Creator.owner', False, 1, 0, b'')]
    for i, (name, directory, parent, block, body) in enumerate(entries):
        at = 0xB000+i*64
        encoded = name.encode()
        data[at:at+len(encoded)] = encoded
        data[at+0x28] = len(encoded) | (0x80 if directory else 0)
        data[at+0x2F:at+0x32] = block.to_bytes(3, 'little')
        struct.pack_into('>HI', data, at+0x32, parent, len(body))
        if body:
            data[0xC000:0xC000+len(body)] = body
    for block in range(2):
        raw = data[0xB000+block*4096:0xC000+block*4096]
        at = 0xA000+block*24
        data[at:at+20] = hashlib.sha1(raw).digest()
        struct.pack_into('>I', data, at+20, 0x80FFFFFF)
    return bytes(data)


class CatalogTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.root = Path(self.temp.name)
        self.source = self.root/'catalog'
        self.item = self.source/'1293'/'Fixture'
        self.item.mkdir(parents=True)
        self.payload = self.item/'0002_layers_1293.liv'
        self.payload.write_bytes(b'opaque encrypted payload')
        (self.item/'Original Creator.owner').touch()
        (self.item/'StorefrontID').write_bytes(b'opaque storefront record')
        (self.item/'thumb.xdc').write_bytes(thumbnail()[0])

    def test_export_preserves_original_payload_creator_and_storefront(self):
        before = {p: p.read_bytes() for p in self.source.rglob('*') if p.is_file()}
        catalog = ugc.read_catalog(self.source)
        self.assertIsNone(catalog.title_id)  # directory names do not prove which game made it
        item, = catalog.items
        self.assertEqual((item.title, item.model_id, item.declared_layers), ('Fixture', 1293, 2))
        self.assertEqual(item.creators, ('Original Creator',))
        self.assertTrue(item.storefront)
        output = self.root/'export'
        manifest = json.loads(ugc.export_catalog(catalog, output).read_text())
        self.assertFalse(manifest['installable'])
        self.assertEqual(manifest['items'][0]['preview']['width'], 32)
        for path, raw in before.items():
            self.assertEqual(path.read_bytes(), raw)
            self.assertEqual((output/'item-0001'/path.name).read_bytes(), raw)
        self.assertTrue((output/'item-0001/preview.png').read_bytes().startswith(b'\x89PNG'))

    def test_snapshot_is_consistent_if_source_later_changes(self):
        catalog = ugc.read_catalog(self.source)
        self.payload.write_bytes(b'newer source')
        out = self.root/'export'
        ugc.export_catalog(catalog, out)
        self.assertEqual((out/'item-0001'/self.payload.name).read_bytes(), b'opaque encrypted payload')
        self.assertEqual(self.payload.read_bytes(), b'newer source')

    def test_refuses_overwrite_and_export_inside_source(self):
        catalog = ugc.read_catalog(self.source)
        for output in [self.source, self.source/'export', self.root]:
            with self.subTest(output=output), self.assertRaises(ugc.CatalogError):
                ugc.export_catalog(catalog, output)
        self.assertTrue(self.payload.exists())

    def test_cancellation_rolls_back_partial_export(self):
        catalog = ugc.read_catalog(self.source)
        cancel = [False]
        out = self.root/'cancelled'
        with self.assertRaisesRegex(ugc.CatalogError, 'cancelled'):
            ugc.export_catalog(catalog, out, lambda: cancel[0], lambda *_: cancel.__setitem__(0, True))
        self.assertFalse(out.exists())
        self.assertTrue(self.payload.exists())

    def test_bad_preview_does_not_destroy_recoverable_payload(self):
        (self.item/'thumb.xdc').write_bytes(b'broken thumbnail')
        out = self.root/'export'
        report = json.loads(ugc.export_catalog(ugc.read_catalog(self.source), out).read_text())
        self.assertIn('preview_error', report['items'][0])
        self.assertEqual((out/'item-0001/thumb.xdc').read_bytes(), b'broken thumbnail')
        self.assertTrue((out/'item-0001'/self.payload.name).is_file())

    def test_rejects_linked_content_and_linked_source(self):
        linked = self.root/'linked'
        try:
            linked.symlink_to(self.source, target_is_directory=True)
        except OSError:
            self.skipTest('Symlinks unavailable')
        with self.assertRaises(ugc.CatalogError): ugc.read_catalog(linked)
        (self.item/'link.owner').symlink_to(self.payload)
        with self.assertRaises(ugc.CatalogError): ugc.read_catalog(self.source)

    def test_rejects_ambiguous_or_inconsistent_model_and_empty_payload(self):
        self.payload.rename(self.item/'0002_layers_9999.liv')
        with self.assertRaisesRegex(ugc.CatalogError, 'disagree'): ugc.read_catalog(self.source)
        (self.item/'0002_layers_9999.liv').rename(self.payload)
        (self.item/'another.liv').write_bytes(b'other')
        with self.assertRaisesRegex(ugc.CatalogError, 'multiple'): ugc.read_catalog(self.source)
        (self.item/'another.liv').unlink()
        self.payload.write_bytes(b'')
        with self.assertRaisesRegex(ugc.CatalogError, 'empty'): ugc.read_catalog(self.source)

    def test_vinyl_group_and_unknown_names_preserve_provenance(self):
        self.payload.rename(self.item/'0002_layers.lvg')
        catalog = ugc.read_catalog(self.source)
        self.assertEqual(catalog.items[0].kind, 'vinyl-group')
        (self.item/'0002_layers.lvg').rename(self.item/'unknown.lvg')
        self.assertIsNone(ugc.read_catalog(self.source).items[0].declared_layers)

    def test_safe_paths_and_member_size_limit(self):
        for name in ['../outside.liv', '/absolute.liv', 'x\\outside.liv', 'x/../evil', 'a//b',
                     'a:stream', 'CON.owner', 'nul', 'trailing./thumb.xdc']:
            with self.subTest(name=name), self.assertRaises(ugc.CatalogError): ugc._safe_name(name)
        with patch.object(ugc, 'MAX_MEMBER', 8):
            with self.assertRaises(ugc.CatalogError): ugc.read_catalog(self.source)

    def test_stfs_header_game_identity_payload_and_block_integrity(self):
        p = self.root/'LivCatalog'
        for title_id in [0x4D5309C9, 0x4D53084D, 0x4D530910]:
            p.write_bytes(package(title_id))
            catalog = ugc.read_catalog(p)
            self.assertEqual(catalog.title_id, f'{title_id:08X}')
            self.assertEqual(catalog.items[0].files[self.payload.name], b'opaque encrypted payload')
            self.assertEqual(catalog.source_sha256, hashlib.sha256(p.read_bytes()).hexdigest())
        damaged = bytearray(package())
        damaged[0xC001] ^= 1
        p.write_bytes(damaged)
        with self.assertRaisesRegex(ugc.CatalogError, 'hash check failed'): ugc.read_catalog(p)

    def test_rejects_wrong_game_truncated_package_and_traversal(self):
        p = self.root/'bad'
        for data in [package(0x12345678), package()[:100], package(payload_name='../escape.liv')]:
            p.write_bytes(data)
            with self.subTest(size=len(data)), self.assertRaises(ugc.CatalogError): ugc.read_catalog(p)


class ThumbnailTests(unittest.TestCase):
    def test_storefront_and_garage_sizes_and_unaligned_width(self):
        for width, height in [(288, 160), (768, 288), (17, 16)]:
            data, expected = thumbnail(width, height)
            self.assertEqual(xdc.decode(data), (width, height, expected))

    def test_png_has_correct_dimensions_and_pixels(self):
        raw = xdc.png_bytes(1, 1, b'\x01\x02\x03\xff')
        self.assertEqual(struct.unpack_from('>II', raw, 16), (1, 1))
        offset = raw.index(b'IDAT')
        length = struct.unpack_from('>I', raw, offset-4)[0]
        self.assertEqual(zlib.decompress(raw[offset+4:offset+4+length]), b'\0\x01\x02\x03\xff')

    def test_rejects_truncated_unsupported_and_expanding_thumbnail(self):
        raw, _ = thumbnail()
        bad_format = bytearray(zlib.decompress(raw[20:], -15))
        struct.pack_into('>I', bad_format, 0x20, 0x82)
        comp = zlib.compressobj(wbits=-15)
        packed = comp.compress(bad_format)+comp.flush()
        unsupported = struct.pack('>4s4I', b'cxds', 2, len(bad_format), len(packed), len(bad_format))+packed
        wrong_size = bytearray(raw)
        struct.pack_into('>I', wrong_size, 8, 64)
        struct.pack_into('>I', wrong_size, 16, 64)
        for value in [b'', raw[:-1], b'bad!'+raw[4:], unsupported, bytes(wrong_size), raw+b'extra']:
            with self.subTest(size=len(value)), self.assertRaises(ValueError): xdc.decode(value)


if __name__ == '__main__':
    unittest.main()
