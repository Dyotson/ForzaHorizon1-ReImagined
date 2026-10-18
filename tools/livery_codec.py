#!/usr/bin/env python3
"""Authenticate FH1 livery streams using key material from the user's own XEX.

Read-only audit by default. No game keys or artwork are distributed with this tool.
This decodes the outer stream; it does not interpret or modify the crlv layer format.
"""
import argparse
from dataclasses import dataclass, field
import hashlib
import hmac
import json
from pathlib import Path
import struct

import apply_title_update as aes

MAX_LIVERY = 16 * 1024**2
MAX_XEX = 128 * 1024**2
# Disc and TU4 addresses, not key bytes. An authenticated MAC selects the candidate.
TABLES = ((0x8200D643, 0x820173E4), (0x82013A13, 0x8201DFE4))


def derive(image, load_address, owner):
    if len(owner) != 8:
        raise ValueError('The save profile identity must contain eight bytes.')
    result = []
    for aes_address, mac_address in TABLES:
        a, m = aes_address-load_address, mac_address-load_address
        if min(a, m) < 0 or a+16 > len(image) or m+32 > len(image):
            continue
        result.append((bytes(((image[a+i] ^ owner[(i-7) % 8])+2*i) % 256
                             for i in range(16)),
                       bytes(((image[m+i] ^ owner[(i-3) % 8])+2*i) % 256
                             for i in range(32))))
    return tuple(result)


def executable_keys(path, owner):
    path = Path(path)
    if path.stat().st_size > MAX_XEX:
        raise ValueError('Executable exceeds the size limit.')
    xex = aes.Xex(path.read_bytes())
    info = xex.opts.get(aes.HDR_EXECUTION_INFO)
    if info is None or struct.unpack_from('>I', xex.raw, info+12)[0] != 0x4D5309C9:
        raise ValueError('Choose a Forza Horizon executable.')
    if xex.image_size > MAX_XEX or xex.pages_size() > MAX_XEX:
        raise ValueError('Executable image exceeds the size limit.')
    return derive(xex.image(), xex.load_address, owner)


@dataclass(frozen=True)
class Livery:
    original: bytes = field(repr=False)
    body: bytes = field(repr=False)
    padding: bytes = field(repr=False)
    aes_key: bytes = field(repr=False)
    mac_key: bytes = field(repr=False)


def decode(raw, candidates):
    if not 68 <= len(raw) <= MAX_LIVERY or (len(raw)-52) % 16:
        raise ValueError('Invalid livery stream size.')
    words = struct.unpack_from('>5I', raw, 32)
    length = words[0] ^ words[1] ^ words[2] ^ words[3] ^ words[4]
    if length < 12 or 52 + ((length+15)//16)*16 != len(raw):
        raise ValueError('Invalid encrypted livery length.')
    for aes_key, mac_key in candidates:
        if len(aes_key) != 16 or len(mac_key) != 32:
            raise ValueError('Invalid key sizes.')
        if not hmac.compare_digest(hmac.digest(mac_key, raw[32:], 'sha256'), raw[:32]):
            continue
        cipher = raw[52:]
        ecb = aes.aes_decrypt(aes_key, cipher, cbc=False)
        plain = bytes(a ^ b for a, b in zip(ecb, raw[36:52]+cipher[:-16]))
        if plain[:4] != b'crlv':
            raise ValueError('Authenticated stream is not a supported crlv livery.')
        return Livery(bytes(raw), plain[:length], plain[length:], aes_key, mac_key)
    raise ValueError('Livery authentication failed for this executable and profile identity.')


def _encrypt(key, data, iv):
    """Portable AES-128 CBC without padding; adapted from the save editor in PR #139."""
    schedule, rcon = list(key), 1
    while len(schedule) < 176:
        word = schedule[-4:]
        if len(schedule) % 16 == 0:
            word = [aes._SBOX[b] for b in word[1:]+word[:1]]
            word[0] ^= rcon
            rcon = aes._xt(rcon)
        for byte in word:
            schedule.append(schedule[-16] ^ byte)
    out, previous = bytearray(), iv
    for start in range(0, len(data), 16):
        state = [a ^ b ^ c for a, b, c in zip(data[start:start+16], previous, schedule[:16])]
        for round_number in range(1, 11):
            state = [aes._SBOX[state[4*((col+row) % 4)+row]]
                     for col in range(4) for row in range(4)]
            if round_number < 10:
                for col in range(0, 16, 4):
                    a, b, c, d = state[col:col+4]
                    xor = a ^ b ^ c ^ d
                    state[col:col+4] = [a ^ xor ^ aes._xt(a ^ b), b ^ xor ^ aes._xt(b ^ c),
                                        c ^ xor ^ aes._xt(c ^ d), d ^ xor ^ aes._xt(d ^ a)]
            state = [b ^ k for b, k in zip(state, schedule[16*round_number:16*(round_number+1)])]
        previous = bytes(state)
        out.extend(previous)
    return bytes(out)


def round_trip(document):
    """Preserve the original seed and padding for an exact, no-edit round trip."""
    seed = document.original[32:52]
    cipher = _encrypt(document.aes_key, document.body+document.padding, seed[4:])
    payload = seed+cipher
    return hmac.digest(document.mac_key, payload, 'sha256')+payload


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('livery', type=Path)
    parser.add_argument('--xex', required=True, type=Path)
    parser.add_argument('--profile-id', required=True, help='16 hex digits of the save profile identity')
    args = parser.parse_args()
    try:
        owner = bytes.fromhex(args.profile_id)
        if len(args.profile_id) != 16 or len(owner) != 8:
            raise ValueError('Profile identity must be 16 hex digits.')
        if args.livery.stat().st_size > MAX_LIVERY:
            raise ValueError('Livery exceeds the size limit.')
        document = decode(args.livery.read_bytes(), executable_keys(args.xex, owner))
        exact = round_trip(document) == document.original
        if not exact:
            raise ValueError('The no-edit round trip failed.')
        print(json.dumps(dict(authenticated=True, format='crlv', plaintext_bytes=len(document.body),
                              plaintext_sha256=hashlib.sha256(document.body).hexdigest(),
                              byte_identical_round_trip=exact), indent=2))
    except (OSError, ValueError, struct.error) as exc:
        parser.exit(1, f'{exc}\n')


if __name__ == '__main__':
    main()
