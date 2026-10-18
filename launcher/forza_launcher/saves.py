"""Local save snapshots. Originals from Xenia and DLC are never snapshot targets."""
from contextlib import contextmanager
from dataclasses import dataclass
from datetime import datetime, timezone
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import uuid

from . import TITLE_ID, paths
from .config import _atomic_write

MAX_FILES = 20000
MAX_BYTES = 32 * 1024**3
CHUNK = 1024**2
ID = re.compile(r'\d{8}T\d{12}Z-[a-f0-9]{12}')


class SaveError(RuntimeError):
    pass


class Cancelled(SaveError):
    pass


@dataclass(frozen=True)
class Profile:
    home: Path
    root: Path
    key: str
    label: str

    @property
    def folder(self):
        from .game import LOCAL_PROFILE
        return self.root / LOCAL_PROFILE / TITLE_ID / '00000001'

    @property
    def backups(self):
        return self.home / 'save-backups' / self.key


@dataclass(frozen=True)
class Snapshot:
    path: Path
    label: str
    created: str
    kind: str
    files: int
    size: int
    fingerprint: str


def _xuid(cfg):
    from . import game
    xuid = cfg.profile.get('xenia_xuid', '').strip().upper()
    if not xuid:
        candidates = game.xenia_candidates(Path(cfg.profile.get('xenia_content') or game.default_xenia_content()))
        if len(candidates) != 1:
            raise SaveError('Choose a Xenia profile in Settings > Profile first.')
        xuid = candidates[0].upper()
    if not re.fullmatch('[0-9A-F]{16}', xuid):
        raise SaveError('Choose a valid Xenia profile in Settings > Profile.')
    return xuid


def data_root_arg(args, default):
    value = str(default)
    for i, arg in enumerate(args):
        if arg.startswith('--user_data_root='):
            value = arg.split('=', 1)[1]
        elif arg == '--user_data_root' and i+1 < len(args):
            value = args[i+1]
    if not value or '"' in value:
        raise SaveError('Use an absolute save folder. For a Windows path with spaces, quote the entire '
                        'advanced argument: "--user_data_root=C:\\My Saves".')
    return Path(value)


def profile_for(cfg, root=None):
    """Resolve without creating/copying saves. A launch may pin its actual migrated root."""
    from . import game
    home = cfg.data_home().expanduser().resolve()
    key, label = 'local', 'My local profile'
    expected = home
    if cfg.profile['mode'] == 'xenia':
        xuid = _xuid(cfg)
        key, label = 'xenia-' + xuid, 'Xenia copy · ' + xuid
        expected = home / key
    if root is None:
        # Honour the same explicit command-line override as the game. Never
        # derive a save path from a profile display name or snapshot metadata.
        args = game.split_args(cfg.advanced.get('extra_args', ''))
        root = data_root_arg(args, expected)
        override = any(a == '--user_data_root' or a.startswith('--user_data_root=') for a in args)
        if not override and root == home and paths.IS_WINDOWS and not (home / game.LOCAL_PROFILE / TITLE_ID / '00000001').exists():
            from . import datafolder
            legacy = datafolder.game_default_data_home().resolve()
            if (legacy / game.LOCAL_PROFILE / TITLE_ID / '00000001').is_dir():
                root = legacy
    root = Path(root).expanduser()
    if not root.is_absolute():
        install, _ = game.find_game(cfg)
        if install is None:
            raise SaveError('Choose an absolute save folder in the advanced launch arguments.')
        root = install.dir / root
    root = root.resolve()
    if root != expected.resolve():
        # The standard legacy Windows location still belongs to the local profile.
        legacy = None
        if paths.IS_WINDOWS and key == 'local':
            from . import datafolder
            legacy = datafolder.game_default_data_home().resolve()
        if root != legacy:
            key = 'custom-' + hashlib.sha256(os.path.normcase(str(root)).encode()).hexdigest()[:16]
            label = 'Custom save folder'
    return Profile(home, root, key, label)


def _check(cancelled):
    if cancelled():
        raise Cancelled('Cancelled. Your current save was not replaced.')


def _inside(path, root):
    """Check lexical containment and reject links/reparse points below a chosen root."""
    path, root = Path(os.path.abspath(path)), Path(os.path.abspath(root))
    try:
        parts = path.relative_to(root).parts
    except ValueError as exc:
        raise SaveError('Save path is outside its selected folder.') from exc
    cursor = root
    for part in ('',) + parts:
        cursor = cursor / part
        try:
            info = cursor.lstat()
        except FileNotFoundError:
            continue
        if stat.S_ISLNK(info.st_mode) or getattr(info, 'st_file_attributes', 0) & 0x400:
            raise SaveError(f'Linked folders or files cannot be managed: {cursor.name}')
    return path


def _remove(path, root):
    path = _inside(path, root)
    if path == Path(root).absolute() or not path.resolve().is_relative_to(Path(root).resolve()):
        raise SaveError('Refusing to remove a folder outside save storage.')
    if path.exists():
        shutil.rmtree(path)


@contextmanager
def locked(profile):
    """An OS lock released on process exit; shared by snapshots and game sessions."""
    storage = _inside(profile.home / 'save-backups', profile.home)
    storage.mkdir(parents=True, exist_ok=True)
    lock = _inside(storage / '.manager.lock', profile.home).open('a+b')
    acquired = False
    try:
        lock.seek(0, os.SEEK_END)
        if not lock.tell():
            lock.write(b'1')
            lock.flush()
        lock.seek(0)
        try:
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_NBLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(), fcntl.LOCK_EX | fcntl.LOCK_NB)
            acquired = True
        except OSError as exc:
            raise SaveError('Close the game or wait for the other save operation to finish.') from exc
        yield
    finally:
        if acquired:
            lock.seek(0)
            if os.name == 'nt':
                import msvcrt
                msvcrt.locking(lock.fileno(), msvcrt.LK_UNLCK, 1)
            else:
                import fcntl
                fcntl.flock(lock.fileno(), fcntl.LOCK_UN)
        lock.close()


def _files(folder, cancelled):
    out = []
    if not folder.exists():
        return out
    _inside(folder, folder)
    pending, total = [folder], 0
    while pending:
        _check(cancelled)
        current = pending.pop()
        for entry in current.iterdir():
            _inside(entry, folder)
            info = entry.lstat()
            if stat.S_ISDIR(info.st_mode):
                pending.append(entry)
            elif stat.S_ISREG(info.st_mode):
                total += info.st_size
                out.append((entry.relative_to(folder).as_posix(), info.st_size))
                if total > MAX_BYTES or len(out) > MAX_FILES:
                    raise SaveError('This save exceeds the snapshot size or file-count limit.')
            else:
                raise SaveError(f'Unsupported file in the save: {entry.name}')
    return sorted(out)


def _copy(source, destination, entries, cancelled, progress):
    total = sum(size for _, size in entries)
    done, records = 0, []
    for name, size in entries:
        _check(cancelled)
        src = _inside(source / name, source)
        dst = _inside(destination / name, destination)
        dst.parent.mkdir(parents=True, exist_ok=True)
        digest = hashlib.sha256()
        before = src.stat()
        with src.open('rb') as reader, dst.open('xb') as writer:
            while data := reader.read(CHUNK):
                _check(cancelled)
                writer.write(data)
                digest.update(data)
                done += len(data)
                if done > MAX_BYTES:
                    raise SaveError('The save grew while it was being backed up. Close the game and retry.')
                progress(done, total)
            after = os.fstat(reader.fileno())
        if (before.st_size, before.st_mtime_ns, before.st_ino) != (after.st_size, after.st_mtime_ns, after.st_ino) or after.st_size != size:
            raise SaveError('The save changed during the operation. Close the game and retry.')
        shutil.copystat(src, dst, follow_symlinks=False)
        records.append(dict(path=name, size=size, sha256=digest.hexdigest()))
    return records


def _manifest(path, profile):
    _inside(path, profile.backups)
    if not ID.fullmatch(path.name):
        raise SaveError('Invalid backup identifier.')
    file = _inside(path / 'snapshot.json', profile.backups)
    if file.stat().st_size > 8 * CHUNK:
        raise SaveError('Invalid backup metadata.')
    data = json.loads(file.read_text(encoding='utf-8'))
    if not isinstance(data, dict) or data.get('version') != 1 or data.get('profile') != profile.key:
        raise SaveError('This backup belongs to a different profile or format.')
    files = data.get('files')
    if not isinstance(files, list) or not files or len(files) > MAX_FILES:
        raise SaveError('Invalid backup file list.')
    seen, size = set(), 0
    for entry in files:
        if not isinstance(entry, dict):
            raise SaveError('Invalid backup file entry.')
        name, count, digest = entry.get('path'), entry.get('size'), entry.get('sha256')
        if (not isinstance(name, str) or not name or '\\' in name or ':' in name or '\0' in name or
                PurePosixPath(name).is_absolute() or any(p in ('', '.', '..') for p in name.split('/')) or
                name.casefold() in seen or type(count) is not int or count < 0 or
                not isinstance(digest, str) or not re.fullmatch('[a-f0-9]{64}', digest)):
            raise SaveError('Invalid or unsafe backup file entry.')
        seen.add(name.casefold())
        size += count
    if size > MAX_BYTES or data.get('kind') not in ('manual', 'automatic', 'before-restore'):
        raise SaveError('Invalid backup metadata.')
    if not isinstance(data.get('label'), str) or len(data['label']) > 80:
        raise SaveError('Invalid backup name.')
    try:
        datetime.fromisoformat(data['created'])
    except (KeyError, TypeError, ValueError) as exc:
        raise SaveError('Invalid backup date.') from exc
    fingerprint = hashlib.sha256(json.dumps(files, sort_keys=True).encode()).hexdigest()
    if data.get('fingerprint') != fingerprint:
        raise SaveError('The backup file list has changed.')
    return data, Snapshot(path, data['label'], data['created'], data['kind'], len(files), size, fingerprint)


def _verified(snapshot, profile, cancelled):
    data, _ = _manifest(snapshot.path, profile)
    source = _inside(snapshot.path / 'save', profile.backups)
    entries = _files(source, cancelled)
    if entries != sorted((entry['path'], entry['size']) for entry in data['files']):
        return False
    for entry in data['files']:
        digest = hashlib.sha256()
        with (source / entry['path']).open('rb') as reader:
            while chunk := reader.read(CHUNK):
                _check(cancelled)
                digest.update(chunk)
        if digest.hexdigest() != entry['sha256']:
            return False
    return True


def snapshots(profile):
    _inside(profile.backups, profile.home)
    found, errors = [], 0
    if profile.backups.is_dir():
        for path in profile.backups.iterdir():
            if path.name.startswith('.'):
                continue
            try:
                _, snapshot = _manifest(path, profile)
                found.append(snapshot)
            except (OSError, ValueError, SaveError):
                errors += 1
    return sorted(found, key=lambda snap: snap.created, reverse=True), errors


def _recover(profile):
    """Finish or roll back an interrupted directory replacement before using saves."""
    parent = profile.folder.parent
    journal = _inside(parent / '.save-manager-restore.json', profile.root)
    if not journal.exists():
        return
    if journal.stat().st_size > 4096:
        raise SaveError('Invalid restore recovery record.')
    data = json.loads(journal.read_text(encoding='utf-8'))
    if not isinstance(data, dict):
        raise SaveError('Invalid restore recovery record.')
    old_name, stage_name = data.get('old'), data.get('stage')
    if not isinstance(old_name, str) or not isinstance(stage_name, str) or not re.fullmatch(r'\.save-old-[a-f0-9]{32}', old_name) or not re.fullmatch(r'\.save-restore-[a-f0-9]{32}', stage_name):
        raise SaveError('Invalid restore recovery record.')
    old = _inside(parent / old_name, profile.root)
    stage = _inside(parent / stage_name, profile.root)
    target = _inside(profile.folder, profile.root)
    if not target.exists() and old.exists():
        os.replace(old, target)
    elif not target.exists() and data.get('had_save') is not False:
        raise SaveError('Restore recovery needs attention. Your backup is still available.')
    for path in (old, stage):
        _remove(path, parent)
    journal.unlink()


def recover(profile):
    with locked(profile):
        _recover(profile)


def _create(profile, label, kind, cancelled, progress, keep):
    _inside(profile.folder, profile.root)
    entries = _files(profile.folder, cancelled)
    if not entries:
        return None
    storage = _inside(profile.backups, profile.home)
    storage.mkdir(parents=True, exist_ok=True)
    if shutil.disk_usage(storage).free < sum(size for _, size in entries) + CHUNK:
        raise SaveError('There is not enough space to back up this save.')
    now = datetime.now(timezone.utc)
    identifier = now.strftime('%Y%m%dT%H%M%S%fZ') + '-' + uuid.uuid4().hex[:12]
    temporary = storage / ('.pending-' + identifier)
    target = storage / identifier
    temporary.mkdir()
    try:
        (temporary / 'save').mkdir()
        records = _copy(profile.folder, temporary / 'save', entries, cancelled, progress)
        fingerprint = hashlib.sha256(json.dumps(records, sort_keys=True).encode()).hexdigest()
        existing, _ = snapshots(profile)
        if kind == 'automatic' and existing and existing[0].fingerprint == fingerprint:
            try:
                if _verified(existing[0], profile, cancelled):
                    return existing[0]
            except (OSError, ValueError, SaveError) as exc:
                if isinstance(exc, Cancelled):
                    raise
        data = dict(version=1, profile=profile.key, created=now.isoformat(), kind=kind,
                    label=' '.join(label.split())[:80] or now.strftime('Backup · %d %b %Y, %H:%M UTC'),
                    fingerprint=fingerprint, files=records)
        _atomic_write(temporary / 'snapshot.json', json.dumps(data, indent=2)+'\n')
        _check(cancelled)
        os.replace(temporary, target)
        if kind == 'automatic':
            automatic = [s for s in snapshots(profile)[0] if s.kind == 'automatic']
            for expired in automatic[max(1, min(50, keep)):]:
                _remove(expired.path, storage)
        return _manifest(target, profile)[1]
    finally:
        _remove(temporary, storage)


def create(profile, label='', kind='manual', cancelled=lambda: False, progress=lambda *_: None, keep=10):
    if kind not in ('manual', 'automatic', 'before-restore'):
        raise SaveError('Unknown backup type.')
    with locked(profile):
        _recover(profile)
        return _create(profile, label, kind, cancelled, progress, keep)


def restore(profile, snapshot, cancelled=lambda: False, progress=lambda *_: None):
    with locked(profile):
        _recover(profile)
        data, _ = _manifest(snapshot.path, profile)
        source = _inside(snapshot.path / 'save', profile.backups)
        entries = _files(source, cancelled)
        if entries != sorted((entry['path'], entry['size']) for entry in data['files']):
            raise SaveError('The backup is incomplete or contains unexpected files.')
        parent = _inside(profile.folder.parent, profile.root)
        parent.mkdir(parents=True, exist_ok=True)
        stage, old = parent / ('.save-restore-' + uuid.uuid4().hex), parent / ('.save-old-' + uuid.uuid4().hex)
        journal = parent / '.save-manager-restore.json'
        stage.mkdir()
        committed = False
        try:
            copied = _copy(source, stage, entries, cancelled, progress)
            if copied != data['files']:
                raise SaveError('The backup failed its integrity check. Your current save was not replaced.')
            safety = _create(profile, 'Before restore · ' + snapshot.label, 'before-restore', cancelled, progress, 10)
            _check(cancelled)
            _atomic_write(journal, json.dumps(dict(old=old.name, stage=stage.name, had_save=profile.folder.exists())))
            try:
                if profile.folder.exists():
                    os.replace(profile.folder, old)
                os.replace(stage, profile.folder)
                committed = True
            except OSError:
                if old.exists() and not profile.folder.exists():
                    os.replace(old, profile.folder)
                raise
            _recover(profile)
            return safety
        finally:
            if not committed and not old.exists():
                _remove(stage, parent)
                if journal.exists():
                    journal.unlink()
