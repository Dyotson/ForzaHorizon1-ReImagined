"""Synthetic crypto tests; no game-derived keys, saves or artwork."""
import hmac
import struct
import unittest

import livery_codec as codec


class LiveryCodecTests(unittest.TestCase):
    def setUp(self):
        self.aes = bytes(range(16))
        self.mac = bytes(range(32))
        self.body = b'crlv'+struct.pack('>II', 3, 1)+b'synthetic layer data'
        self.padding = bytes((-len(self.body)) % 16)
        iv = bytes(range(16, 32))
        words = struct.unpack('>4I', iv)
        seed = struct.pack('>I', len(self.body) ^ words[0] ^ words[1] ^ words[2] ^ words[3])+iv
        payload = seed+codec._encrypt(self.aes, self.body+self.padding, iv)
        self.raw = hmac.digest(self.mac, payload, 'sha256')+payload
        self.keys = ((self.aes, self.mac),)

    def test_nist_aes_cbc_vector(self):
        key = bytes.fromhex('2b7e151628aed2a6abf7158809cf4f3c')
        iv = bytes.fromhex('000102030405060708090a0b0c0d0e0f')
        plain = bytes.fromhex('6bc1bee22e409f96e93d7e117393172a')
        expected = bytes.fromhex('7649abac8119b246cee98e9b12e9197d')
        self.assertEqual(codec._encrypt(key, plain, iv), expected)

    def test_authenticated_round_trip(self):
        result = codec.decode(self.raw, self.keys)
        self.assertEqual(result.body, self.body)
        self.assertEqual(result.padding, self.padding)
        self.assertEqual(codec.round_trip(result), self.raw)
        self.assertNotIn(self.aes.hex(), repr(result))

    def test_ciphertext_tampering_rejected(self):
        damaged = bytearray(self.raw)
        damaged[-1] ^= 1
        with self.assertRaisesRegex(ValueError, 'authentication failed'):
            codec.decode(damaged, self.keys)

    def test_wrong_key_rejected(self):
        with self.assertRaisesRegex(ValueError, 'authentication failed'):
            codec.decode(self.raw, ((self.aes, bytes(32)),))

    def test_invalid_length_and_truncation_rejected(self):
        for raw in (b'', self.raw[:-1], self.raw[:32]+bytes(20)+self.raw[52:]):
            with self.subTest(length=len(raw)), self.assertRaises(ValueError):
                codec.decode(raw, self.keys)

    def test_authenticated_wrong_format_rejected(self):
        result = codec.decode(self.raw, self.keys)
        wrong = codec.Livery(result.original, b'nope'+result.body[4:], result.padding,
                             result.aes_key, result.mac_key)
        with self.assertRaisesRegex(ValueError, 'crlv'):
            codec.decode(codec.round_trip(wrong), self.keys)

    def test_key_derivation_uses_profile_identity(self):
        image = bytes(i % 251 for i in range(0x20000))
        a = codec.derive(image, 0x82000000, bytes(8))
        b = codec.derive(image, 0x82000000, bytes(range(8)))
        self.assertEqual(len(a), 2)
        self.assertNotEqual(a, b)
        self.assertEqual(codec.derive(b'', 0x82000000, bytes(8)), ())
        with self.assertRaises(ValueError):
            codec.derive(image, 0x82000000, b'bad')


if __name__ == '__main__':
    unittest.main()
