"""Read-only preservation of local Forza design/vinyl catalogs.

Preserve encrypted payloads, creator markers and storefront records byte for byte.
An archival export is not a cross-profile or cross-game installer. No keys or artwork
are bundled. This backend can run in a launcher worker independently of the gallery.
"""
from dataclasses import dataclass, field
import hashlib
import json
import os
from pathlib import Path, PurePosixPath
import re
import shutil
import stat
import struct

from . import paths, TITLE_ID

TITLE_NAMES = {TITLE_ID: "Forza Horizon", "4D530910": "Forza Motorsport 4",
               "4D53084D": "Forza Motorsport 3"}
MAX_BYTES = 128 * 1024**2
MAX_MEMBER = 16 * 1024**2
MAX_FILES = 4096


class CatalogError(ValueError):
    pass


def _check(cancelled):
    if cancelled():
        raise CatalogError("Catalog operation cancelled.")


def _safe_name(name):
    parts = PurePosixPath(name).parts
    if (not parts or PurePosixPath(name).is_absolute() or "\\" in name or ":" in name
            or any(part in (".", "..") or any(ord(c) < 32 for c in part) for part in parts)
            or any(part.endswith((".", " ")) or re.fullmatch(
                r"(?i:con|prn|aux|nul|com[0-9]|lpt[0-9])", part.split(".")[0]) for part in parts)
            or str(PurePosixPath(name)) != name):
        raise CatalogError("Unsafe catalog member path.")


def _wanted(name):
    base = PurePosixPath(name).name
    return (base.lower().endswith((".liv", ".lvg", ".owner", ".xuid"))
            or base in ("StorefrontID", "thumb.xdc"))


@dataclass(frozen=True)
class Item:
    path: str
    title: str
    kind: str
    model_id: int | None
    declared_layers: int | None
    creators: tuple[str, ...]
    storefront: bool
    files: dict[str, bytes] = field(repr=False, compare=False)


@dataclass(frozen=True)
class Catalog:
    source: Path
    title_id: str | None
    source_sha256: str | None
    items: tuple[Item, ...]

    @property
    def game(self):
        return TITLE_NAMES.get(self.title_id, "Unverified extracted catalog")


def _package(source, cancelled):
    paths.ensure_tools_importable()
    import xcontent_extract as xc

    size = source.stat().st_size
    if not 0x971A <= size <= MAX_BYTES:
        raise CatalogError("Catalog package is too small or exceeds 128 MB.")

    class Reader(xc.Stfs):
        reads = 0

        def block_hash_info(self, index):
            self.reads += 1
            _check(cancelled)
            if self.reads > MAX_BYTES//4096 + MAX_FILES:
                raise CatalogError("Catalog has an invalid block chain.")
            return super().block_hash_info(index)

        def block_to_offset(self, index):
            if not 0 <= index < self.total_blocks:
                raise CatalogError("Catalog block lies outside its declared volume.")
            return super().block_to_offset(index)

    pkg = None
    try:
        header = xc.Header(str(source))
        title_id = f"{header.title_id:08X}"
        if title_id not in TITLE_NAMES or header.content_type != 1 or header.volume_type != 0:
            raise CatalogError("Choose an FH1, FM3 or FM4 saved-content package.")
        pkg = Reader(str(source), header)
        before = os.fstat(pkg.files[0].fileno())
        if not 0 < pkg.table_count <= MAX_FILES//64 or pkg.total_blocks > size//4096:
            raise CatalogError("Invalid catalog file table or volume size.")
        entries = pkg.entries()
        if len(entries) > MAX_FILES:
            raise CatalogError("Catalog has too many entries.")
        found, seen, total = {}, set(), 0
        for entry in entries:
            _check(cancelled)
            _safe_name(entry.path)
            if entry.path.casefold() in seen:
                raise CatalogError("Duplicate catalog member path.")
            seen.add(entry.path.casefold())
            if entry.is_dir or not _wanted(entry.path):
                continue
            total += entry.size
            if entry.size > MAX_MEMBER or total > MAX_BYTES:
                raise CatalogError("Catalog content exceeds its size limit.")
            data = bytearray()
            for block in entry.blocks:
                reader = pkg.files[0]
                reader.seek(pkg.block_to_offset(block))
                raw = reader.read(pkg.BLOCK)
                if len(raw) != pkg.BLOCK or hashlib.sha1(raw).digest() != pkg.block_sha1(block):
                    raise CatalogError(f"Catalog block hash check failed: {entry.path}")
                data.extend(raw[:min(pkg.BLOCK, entry.size-len(data))])
            if len(data) != entry.size:
                raise CatalogError("Truncated catalog member.")
            found[entry.path] = bytes(data)
        pkg.files[0].seek(0)
        digest = hashlib.file_digest(pkg.files[0], "sha256").hexdigest()
        after = os.fstat(pkg.files[0].fileno())
        if (before.st_size, before.st_mtime_ns) != (after.st_size, after.st_mtime_ns):
            raise CatalogError("Catalog package changed while reading; retry after closing the game.")
        return title_id, digest, found
    except (SystemExit, IndexError, struct.error, OSError) as exc:
        raise CatalogError("Damaged or unreadable catalog package.") from exc
    finally:
        if pkg is not None:
            for reader in pkg.files:
                reader.close()


def _folder(source, cancelled):
    found, seen, total, count = {}, set(), 0, 0

    def failed(error):
        raise error

    for directory, dirs, names in os.walk(source, followlinks=False, onerror=failed):
        _check(cancelled)
        count += len(dirs)+len(names)
        if count > MAX_FILES:
            raise CatalogError("Catalog has too many entries.")
        for name in dirs+names:
            path = Path(directory)/name
            if path.is_symlink() or getattr(path, "is_junction", lambda: False)():
                raise CatalogError("Catalog contains a linked path.")
        if len(Path(directory).relative_to(source).parts) > 8:
            raise CatalogError("Catalog directory is too deeply nested.")
        for name in names:
            path = Path(directory)/name
            relative = path.relative_to(source).as_posix()
            _safe_name(relative)
            if relative.casefold() in seen:
                raise CatalogError("Duplicate catalog member path.")
            seen.add(relative.casefold())
            if not _wanted(relative):
                continue
            info = path.stat()
            if not stat.S_ISREG(info.st_mode) or info.st_size > MAX_MEMBER:
                raise CatalogError("Catalog member is not a regular file or is too large.")
            with path.open("rb") as reader:
                data = reader.read(MAX_MEMBER+1)
            total += len(data)
            if len(data) != info.st_size or len(data) > MAX_MEMBER or total > MAX_BYTES:
                raise CatalogError("Catalog changed while reading or exceeds its size limit.")
            found[relative] = data
    return found


def read_catalog(source, cancelled=lambda: False):
    """Read a saved-content package or extracted catalog without changing it."""
    source = Path(source).expanduser().absolute()
    if source.is_symlink() or getattr(source, "is_junction", lambda: False)():
        raise CatalogError("Choose a catalog directly, not a linked path.")
    try:
        if source.is_dir():
            title_id, digest, files = None, None, _folder(source, cancelled)
        elif source.is_file():
            title_id, digest, files = _package(source, cancelled)
        else:
            raise CatalogError("Catalog does not exist or is not a regular file.")
    except OSError as exc:
        raise CatalogError(f"Could not read catalog: {exc}") from exc
    items = []
    for name in sorted(files):
        if not name.lower().endswith((".liv", ".lvg")):
            continue
        _check(cancelled)
        if not files[name]:
            raise CatalogError("Catalog contains an empty design payload.")
        path = PurePosixPath(name)
        peers = {PurePosixPath(p).name: data for p, data in files.items()
                 if PurePosixPath(p).parent == path.parent}
        if sum(n.lower().endswith((".liv", ".lvg")) for n in peers) != 1:
            raise CatalogError("A catalog item contains multiple design payloads.")
        match = re.fullmatch(r"(\d+)_layers(?:_(\d+))?\.(liv|lvg)", path.name, re.I)
        model = path.parent.parent.name
        model_id = int(model) if model.isascii() and model.isdigit() else None
        if match and match[2]:
            encoded_model = int(match[2])
            if model_id is not None and encoded_model != model_id:
                raise CatalogError("Design filename and model folder disagree.")
            model_id = encoded_model
        items.append(Item(name, path.parent.name, "livery" if path.suffix.lower() == ".liv" else "vinyl-group",
                          model_id, int(match[1]) if match else None,
                          tuple(sorted(n[:-6] for n in peers if n.lower().endswith(".owner"))),
                          bool(peers.get("StorefrontID")), peers))
    return Catalog(source, title_id, digest, tuple(items))


def description(catalog):
    """JSON-ready provenance; payloads remain encrypted and opaque."""
    return {"format": "forza-ugc-archive", "version": 1, "source": catalog.source.name,
            "source_sha256": catalog.source_sha256, "title_id": catalog.title_id, "game": catalog.game,
            "installable": False, "items": [
                {"source_path": item.path, "title": item.title, "kind": item.kind,
                 "model_id": item.model_id, "declared_layers": item.declared_layers,
                 "creators": list(item.creators), "storefront_record": item.storefront,
                 "files": {name: {"bytes": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                           for name, data in sorted(item.files.items())}}
                for item in catalog.items]}


def export_catalog(catalog, destination, cancelled=lambda: False, progress=lambda *_: None):
    """Write a new archival bundle without replacing any source or existing output."""
    _check(cancelled)
    destination = Path(destination).expanduser().absolute()
    source = catalog.source.resolve()
    if destination.resolve() == source or (source.is_dir() and destination.resolve().is_relative_to(source)):
        raise CatalogError("Choose an export folder outside the source save directory.")
    report = description(catalog)
    paths.ensure_tools_importable()
    import xdc_decode
    try:
        destination.mkdir()
    except OSError as exc:
        raise CatalogError(f"Choose a new, writable export folder: {exc}") from exc
    try:
        for i, item in enumerate(catalog.items):
            _check(cancelled)
            folder = destination/f"item-{i+1:04d}"
            folder.mkdir()
            for name, data in item.files.items():
                _safe_name(name)
                with (folder/name).open("xb") as writer:
                    writer.write(data)
            row = report["items"][i]
            row["directory"] = folder.name
            if "thumb.xdc" in item.files:
                try:
                    width, height, rgba = xdc_decode.decode(item.files["thumb.xdc"])
                except ValueError as exc:
                    row["preview_error"] = str(exc)
                else:
                    with (folder/"preview.png").open("xb") as writer:
                        writer.write(xdc_decode.png_bytes(width, height, rgba))
                    row["preview"] = {"path": "preview.png", "width": width, "height": height}
            progress(i+1, len(catalog.items))
        _check(cancelled)
        with (destination/"manifest.json").open("x", encoding="utf-8") as writer:
            json.dump(report, writer, indent=2, ensure_ascii=False)
            writer.write("\n")
    except BaseException:
        shutil.rmtree(destination)
        raise
    return destination/"manifest.json"
