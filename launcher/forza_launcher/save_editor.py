"""Authenticated FH1 profile edits; game-specific key bytes come from the player's XEX.

The public format research is linked in docs/launcher.md. Only fixed-width values
in the self-describing prefix are edited; class-serialised state remains opaque.
"""
from dataclasses import dataclass, field
from functools import lru_cache, reduce
import hmac
import operator
import os
from pathlib import Path
import re
import struct
import uuid

from . import TITLE_ID, game, paths, saves

MAX_PROFILE = 16 * 1024**2
FORMATS = {0: '?', 1: 'B', 3: 'I', 4: 'Q', 7: 'i', 9: 'f'}
EDITABLE = {'Main/Credits': (3, 0, 999_999_999), 'Main/XP': (7, 0, 2_147_483_647)}


@dataclass(frozen=True)
class Value:
    offset: int
    kind: int
    value: object


def fields(body):
    """Parse the bounded prefix completely; retain the unknown trailing bytes unchanged."""
    position, total, found = 0, 0, {}

    def take(count):
        nonlocal position
        if count < 0 or position + count > len(body):
            raise saves.SaveError('The profile has a truncated field section.')
        data = body[position:position+count]
        position += count
        return data

    def u32():
        return struct.unpack('>I', take(4))[0]

    def section(prefix='', depth=0):
        nonlocal total
        count = u32()
        total += count
        if depth > 8 or total > 10000:
            raise saves.SaveError('The profile field section is too complex.')
        for _ in range(count):
            length = u32()
            if not 0 < length <= 256:
                raise saves.SaveError('Invalid profile field name.')
            try:
                name = take(length).decode('utf-8')
            except UnicodeError as exc:
                raise saves.SaveError('Invalid profile field name.') from exc
            if '/' in name or '\0' in name:
                raise saves.SaveError('Invalid profile field name.')
            path = prefix + name
            if path in found or take(8) != struct.pack('>II', 0x20, 0):
                raise saves.SaveError('Duplicate or unsupported profile field.')
            kind = take(1)[0]
            found[path] = None
            if kind == 15:
                section(path+'/', depth+1)
            elif kind in FORMATS:
                offset = position
                raw = take(struct.calcsize(FORMATS[kind]))
                if kind == 0 and raw not in (b'\0', b'\1'):
                    raise saves.SaveError('Invalid profile boolean.')
                found[path] = Value(offset, kind, struct.unpack('>'+FORMATS[kind], raw)[0])
            else:
                raise saves.SaveError(f'Unsupported profile field type: {kind}.')

    section()
    for path, (kind, _low, _high) in EDITABLE.items():
        if path not in found or found[path] is None or found[path].kind != kind:
            raise saves.SaveError('This is not a supported FH1 career profile.')
    return {path: value for path, value in found.items() if value is not None}


def _aes_tools():
    paths.ensure_tools_importable()
    import apply_title_update
    return apply_title_update


def _encrypt_cbc(key, data, iv):
    """AES-128 CBC, no padding. Same stdlib-only fallback as the XEX decoder."""
    try:
        from cryptography.hazmat.primitives.ciphers import Cipher, algorithms, modes
        cipher = Cipher(algorithms.AES(key), modes.CBC(iv)).encryptor()
        return cipher.update(data) + cipher.finalize()
    except ImportError:
        pass
    aes = _aes_tools()
    sbox, xt = aes._SBOX, aes._xt
    schedule = list(key)
    rcon = 1
    while len(schedule) < 176:
        word = schedule[-4:]
        if len(schedule) % 16 == 0:
            word = [sbox[b] for b in word[1:]+word[:1]]
            word[0] ^= rcon
            rcon = xt(rcon)
        for byte in word:
            schedule.append(schedule[-16] ^ byte)
    out, previous = bytearray(), iv
    for start in range(0, len(data), 16):
        state = [a ^ b ^ c for a, b, c in zip(data[start:start+16], previous, schedule[:16])]
        for round_number in range(1, 11):
            state = [sbox[state[4*((col+row)%4)+row]] for col in range(4) for row in range(4)]
            if round_number < 10:
                for col in range(0, 16, 4):
                    a, b, c, d = state[col:col+4]
                    xor = a ^ b ^ c ^ d
                    state[col:col+4] = [a ^ xor ^ xt(a ^ b), b ^ xor ^ xt(b ^ c),
                                        c ^ xor ^ xt(c ^ d), d ^ xor ^ xt(d ^ a)]
            state = [b ^ k for b, k in zip(state, schedule[16*round_number:16*(round_number+1)])]
        previous = bytes(state)
        out.extend(previous)
    return bytes(out)


@lru_cache(maxsize=4)
def _tables(path, size, modified):
    """Cache key tables in memory only; no game bytes or decrypted saves are persisted."""
    if size > 128 * 1024**2:
        raise saves.SaveError('The game executable is too large to read safely.')
    raw = Path(path).read_bytes()
    from .xexcheck import header_info
    info = header_info(raw)
    if not info or info[3] != int(TITLE_ID, 16):
        raise saves.SaveError('Choose the FH1 game files in Setup before editing a save.')
    try:
        xex = _aes_tools().Xex(raw)
        if xex.image_size > 128 * 1024**2 or xex.pages_size() > 128 * 1024**2:
            raise ValueError('Unsupported executable image size')
        image = xex.image()
    except (SystemExit, ValueError, struct.error) as exc:
        raise saves.SaveError('Could not read the profile key tables from this game executable.') from exc
    # Locations of the profile's AES/HMAC tables in the supported disc and TU4
    # images. These are addresses, not key material. A matching save MAC is required.
    return tuple(bytes(image[address-xex.load_address:address-xex.load_address+64])
                 for address in (0x8200D634, 0x82013A04)
                 if 0 <= address-xex.load_address <= len(image)-64)


def _owner(cfg):
    owner = saves._xuid(cfg) if cfg.profile['mode'] == 'xenia' else game.LOCAL_PROFILE
    args = game.split_args(cfg.advanced.get('extra_args', ''))
    for i, arg in enumerate(args):
        if arg.startswith('--profile_xuid='):
            owner = arg.split('=', 1)[1]
        elif arg == '--profile_xuid' and i+1 < len(args):
            owner = args[i+1]
    if not re.fullmatch('[0-9a-fA-F]{16}', owner):
        raise saves.SaveError('Choose a valid profile XUID before editing a save.')
    return bytes.fromhex(owner)


def _keys(cfg):
    executable = cfg.game_files()/'default.xex'
    try:
        info = executable.stat()
    except OSError as exc:
        raise saves.SaveError('Choose your FH1 game files in Setup before opening the editor.') from exc
    owner = _owner(cfg)
    material = _tables(str(executable.resolve()), info.st_size, info.st_mtime_ns)
    return tuple((bytes(((table[i] ^ owner[(i-2)%8])+2*i) % 256 for i in range(16)),
                  bytes(((table[32+i] ^ owner[(i-4)%8])+2*i) % 256 for i in range(32)))
                 for table in material)


@dataclass(frozen=True)
class Document:
    profile: saves.Profile
    original: bytes = field(repr=False)
    body: bytes = field(repr=False)
    padding: bytes = field(repr=False)
    aes_key: bytes = field(repr=False)
    mac_key: bytes = field(repr=False)
    values: dict


@dataclass(frozen=True)
class EditResult:
    backup: saves.Snapshot | None
    document: Document


def decode(profile, raw, keys):
    if not 76 <= len(raw) <= MAX_PROFILE or raw[:4] != b'cmss':
        raise saves.SaveError('Unsupported or damaged ForzaProfile file.')
    used = struct.unpack_from('>I', raw, 4)[0]
    if not 68 <= used <= len(raw)-8:
        raise saves.SaveError('Invalid profile stream length.')
    payload = raw[40:8+used]
    length = reduce(operator.xor, struct.unpack_from('>5I', payload))
    if not 4 <= length <= MAX_PROFILE or 52 + ((length+15)//16)*16 != used:
        raise saves.SaveError('Invalid encrypted profile length.')
    for aes_key, mac_key in keys:
        if not hmac.compare_digest(hmac.digest(mac_key, payload, 'sha256'), raw[8:40]):
            continue
        cipher = payload[20:]
        ecb = _aes_tools().aes_decrypt(aes_key, cipher, cbc=False)
        plain = bytes(a ^ b for a, b in zip(ecb, payload[4:20]+cipher[:-16]))
        body = plain[:length]
        return Document(profile, raw, body, plain[length:], aes_key, mac_key, fields(body))
    raise saves.SaveError('The profile integrity check failed. It may be damaged, belong to a different '
                          'XUID, or use an unsupported game version. Nothing was changed.')


def _read(profile):
    path = saves._inside(profile.folder/'ForzaProfile/ForzaProfile', profile.root)
    if path.stat().st_size > MAX_PROFILE:
        raise saves.SaveError('The profile is too large to edit safely.')
    return path.read_bytes()


def load(cfg, profile, cancelled=lambda: False):
    with saves.locked(profile):
        saves._recover(profile)
        saves._check(cancelled)
        raw = _read(profile)
        keys = _keys(cfg)
        saves._check(cancelled)
        return decode(profile, raw, keys)


def encode(document, changes):
    body = bytearray(document.body)
    for path, value in changes.items():
        if path not in EDITABLE:
            raise saves.SaveError('This profile value cannot be edited.')
        kind, low, high = EDITABLE[path]
        if type(value) is not int or not low <= value <= high:
            raise saves.SaveError(f'{path.split("/")[-1]} must be between {low:,} and {high:,}.')
        saved = document.values[path]
        if saved.kind != kind:
            raise saves.SaveError('The profile field type changed.')
        struct.pack_into('>'+FORMATS[kind], body, saved.offset, value)
    seed = document.original[40:60]
    cipher = _encrypt_cbc(document.aes_key, bytes(body)+document.padding, seed[4:])
    payload = seed+cipher
    used = len(payload)+32
    raw = document.original[:8]+hmac.digest(document.mac_key, payload, 'sha256')+payload+document.original[8+used:]
    checked = decode(document.profile, raw, ((document.aes_key, document.mac_key),))
    if checked.body != body or len(raw) != len(document.original):
        raise saves.SaveError('The edited profile did not pass verification.')
    return raw


def apply(document, changes, cancelled=lambda: False, progress=lambda *_: None):
    profile = document.profile
    with saves.locked(profile):
        saves._recover(profile)
        saves._check(cancelled)
        raw = _read(profile)
        if raw != document.original:
            raise saves.SaveError('The save changed after the editor was opened. Reload it before editing.')
        edited = encode(document, changes)
        if edited == raw:
            return EditResult(None, document)
        backup = saves._create(profile, 'Before profile edit', 'before-edit', cancelled, progress, 10)
        saves._check(cancelled)
        if _read(profile) != raw:
            raise saves.SaveError('The save changed during backup. Reload it before editing.')
        path = saves._inside(profile.folder/'ForzaProfile/ForzaProfile', profile.root)
        temporary = path.with_name('.profile-edit-'+uuid.uuid4().hex)
        try:
            with temporary.open('xb') as writer:
                writer.write(edited)
                writer.flush()
                os.fsync(writer.fileno())
            saves._check(cancelled)
            if _read(profile) != raw:
                raise saves.SaveError('The save changed before applying the edit. Reload it first.')
            os.replace(temporary, path)
        finally:
            if temporary.exists():
                temporary.unlink()
        return EditResult(backup, decode(profile, edited, ((document.aes_key, document.mac_key),)))
