"""Read local photo-mode saves. Nothing here modifies or migrates a profile.

The game mounts a photo content folder and stores named photos beneath it. Image
files need not have extensions. JPEG payloads may follow a save header, so sniff
the bytes as well as trying ordinary image files. Do not scan car thumbnails,
game media, DLC, or the online storefront.
"""
from dataclasses import dataclass
from pathlib import Path
import os
import re
import stat
import tempfile

from . import TITLE_ID, game

MAX_FILE_BYTES = 64 * 1024 * 1024
MAX_PIXELS = 40_000_000


def _linked(path):
    return path.is_symlink() or getattr(path, "is_junction", lambda: False)()


@dataclass(frozen=True)
class Photo:
    path: Path
    modified: float
    width: int
    height: int


def profile_folder(cfg):
    """The active local save, including the launcher's *copy* of a Xenia save."""
    root = cfg.data_home()
    if cfg.profile["mode"] == "xenia":
        xuid = cfg.profile.get("xenia_xuid", "").strip().upper()
        if not xuid:
            content = Path(cfg.profile.get("xenia_content") or game.default_xenia_content())
            candidates = game.xenia_candidates(content)
            if len(candidates) != 1:
                raise ValueError("Choose a Xenia profile in Settings > Profile to see its photos.")
            xuid = candidates[0].upper()
        if not re.fullmatch(r"[0-9A-F]{16}", xuid):
            raise ValueError("Choose a valid Xenia profile in Settings > Profile.")
        root /= f"xenia-{xuid}"
    return root / game.LOCAL_PROFILE / TITLE_ID / "00000001"


def photo_groups(folder, cancelled=lambda: False):
    """Yield photo directories and their files, excluding linked paths and other saves.

    Search the content folders and one level inside ForzaProfile for photo roots.
    Once inside a photo root, each directory with files is a potential photo.
    Errors are surfaced to the page instead of being mistaken for an empty gallery.
    """
    folder = Path(folder)
    if not folder.exists():
        return

    def failed(error):
        raise error

    roots = []
    for directory, dirs, _ in os.walk(folder, onerror=failed, followlinks=False):
        if cancelled():
            return
        directory = Path(directory)
        dirs[:] = [d for d in dirs if not _linked(directory / d)]
        if directory != folder and directory.name.lower() in ("photo", "photos"):
            roots.append(directory)
            dirs[:] = []
        elif len(directory.relative_to(folder).parts) >= 2:
            dirs[:] = []

    for root in roots:
        for directory, dirs, names in os.walk(root, onerror=failed, followlinks=False):
            if cancelled():
                return
            directory = Path(directory)
            dirs[:] = [d for d in dirs if not _linked(directory / d)]
            files = [directory / n for n in sorted(names)
                     if not (directory / n).is_symlink() and not n.lower().endswith(".header")]
            if directory == root:
                for path in files:
                    yield directory, [path]
            elif files:
                yield directory, files


def _image_sources(data):
    """Ordinary images and embedded JPEGs (thumbnail + full photo in a save)."""
    yield data
    start = 0
    # Bound work on corrupt files full of fake JPEG signatures.
    for _ in range(32):
        start = data.find(b"\xff\xd8\xff", start + 1)
        if start < 0:
            break
        yield data[start:]


def read_photo(path, size=None):
    """Decode the largest image in a file, optionally scaled for a thumbnail.

    QImage is safe in a worker; QPixmap is created only by the UI. Limit both file
    size and decoded dimensions before allocation. Content sniffing works without
    extensions and does not add a Pillow dependency to packaged launchers.
    """
    from PySide6.QtCore import QByteArray, QBuffer, QIODevice, QSize, Qt
    from PySide6.QtGui import QImageReader

    path = Path(path)
    info = path.stat()
    if not stat.S_ISREG(info.st_mode) or not 0 < info.st_size <= MAX_FILE_BYTES:
        raise ValueError("Photo is empty, is not a regular file, or exceeds the 64 MB image limit.")
    with path.open("rb") as source:
        data = source.read(MAX_FILE_BYTES + 1)
    if not data or len(data) > MAX_FILE_BYTES:
        raise ValueError("Photo is empty or exceeds the 64 MB image limit.")
    best = None
    for payload in _image_sources(data):
        buffer = QBuffer()
        buffer.setData(QByteArray(payload))
        buffer.open(QIODevice.ReadOnly)
        reader = QImageReader(buffer)
        reader.setDecideFormatFromContent(True)
        reader.setAutoTransform(True)
        dimensions = reader.size()
        pixels = dimensions.width() * dimensions.height()
        if not dimensions.isValid() or not 0 < pixels <= MAX_PIXELS:
            continue
        if best and pixels <= best[0]:
            continue
        if size:
            reader.setScaledSize(dimensions.scaled(QSize(*size), Qt.KeepAspectRatio))
        image = reader.read()
        if image.isNull():
            continue
        best = pixels, image, dimensions.width(), dimensions.height()
    if best is None:
        raise ValueError("No readable image found. The save may be incomplete or use an unsupported format.")
    return best[1:]


def load_group(directory, files, cancelled=lambda: False):
    """One tile per named photo; prefer the full picture over its saved preview."""
    best = None
    for path in files:
        if cancelled():
            return None
        try:
            image, width, height = read_photo(path, (400, 225))
            modified = path.stat().st_mtime
        except (OSError, ValueError):
            continue
        if best is None or width * height > best[0].width * best[0].height:
            best = Photo(path, modified, width, height), image
    return best


def export_photo(image, destination, profile):
    """Atomic PNG/JPEG export; never replace any file inside the source profile."""
    destination = Path(destination)
    if destination.resolve().is_relative_to(Path(profile).resolve()):
        raise ValueError("Choose an export folder outside your game save.")
    fmt = {".png": "PNG", ".jpg": "JPEG", ".jpeg": "JPEG"}.get(destination.suffix.lower())
    if not fmt:
        raise ValueError("Choose a PNG (.png) or JPEG (.jpg) filename.")
    fd, temporary = tempfile.mkstemp(prefix=".forza-photo-", dir=destination.parent)
    os.close(fd)
    try:
        if not image.save(temporary, fmt, 95):
            raise OSError("The photo could not be written. Check the export folder and free space.")
        os.replace(temporary, destination)
    finally:
        Path(temporary).unlink(missing_ok=True)
