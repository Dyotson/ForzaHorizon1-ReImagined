"""Downloading and unpacking the pinned archives."""
import hashlib
import os
import shutil
import ssl
import stat
import tarfile
import urllib.request
import zipfile
from pathlib import Path

from . import pins


class Cancelled(Exception):
    pass


# CA bundles of the common distributions. A frozen launcher's OpenSSL looks where the build
# machine keeps them (Ubuntu), which Fedora, Bazzite, SteamOS or Arch don't have (#68).
_CA_FILES = (
    "/etc/ssl/certs/ca-certificates.crt",                 # Debian, Ubuntu, Arch, SteamOS
    "/etc/pki/ca-trust/extracted/pem/tls-ca-bundle.pem",  # Fedora, Bazzite, RHEL
    "/etc/pki/tls/certs/ca-bundle.crt",
    "/etc/ssl/ca-bundle.pem",                             # openSUSE
    "/etc/ssl/cert.pem",                                  # Alpine, macOS
)
_SSL_CONTEXT = None


def ssl_context() -> ssl.SSLContext:
    """A verifying context with the system's CA certificates wherever the distribution keeps them."""
    global _SSL_CONTEXT
    if _SSL_CONTEXT is None:
        ctx = ssl.create_default_context()
        for ca in (os.environ.get("SSL_CERT_FILE"),) + _CA_FILES:
            if ca and os.path.isfile(ca):
                try:
                    ctx.load_verify_locations(cafile=ca)
                except (OSError, ssl.SSLError):
                    pass
        try:
            import certifi
            ctx.load_verify_locations(cafile=certifi.where())
        except Exception:
            pass
        _SSL_CONTEXT = ctx
    return _SSL_CONTEXT


def sha256_of(path: Path, progress=None):
    h = hashlib.sha256()
    size = path.stat().st_size
    done = 0
    with open(path, "rb") as f:
        while chunk := f.read(1 << 22):
            h.update(chunk)
            done += len(chunk)
            if progress:
                progress(done, size)
    return h.hexdigest()


def download(name: str, cache: Path, progress=lambda d, t: None, cancelled=lambda: False) -> Path:
    """Download a pinned archive into cache (kept: a second run needs no download); returns its
    path after checking the sha256. Resumes a partial file."""
    url, digest, _kind = pins.DOWNLOADS[name]
    cache.mkdir(parents=True, exist_ok=True)
    final = cache / url.rsplit("/", 1)[1]
    if final.is_file() and sha256_of(final) == digest:
        return final
    part = final.with_name(final.name + ".part")
    have = part.stat().st_size if part.exists() else 0
    req = urllib.request.Request(url, headers={"User-Agent": "forza-reimagined-launcher"})
    if have:
        req.add_header("Range", f"bytes={have}-")
    try:
        resp = urllib.request.urlopen(req, timeout=60, context=ssl_context())
    except urllib.error.HTTPError as e:
        if e.code == 416:  # the partial file is already complete
            resp, have = None, have
        else:
            raise
    if resp is not None:
        if resp.status == 200 and have:  # the server ignored Range: start over
            have = 0
        total = have + int(resp.headers.get("Content-Length") or 0)
        mode = "ab" if have else "wb"
        with open(part, mode) as f:
            done = have
            while chunk := resp.read(1 << 20):
                if cancelled():
                    raise Cancelled()
                f.write(chunk)
                done += len(chunk)
                progress(done, total)
    if sha256_of(part) != digest:
        part.unlink(missing_ok=True)
        raise RuntimeError(f"{final.name}: the download does not match its pinned checksum "
                           "(it was deleted; try again)")
    os.replace(part, final)
    return final


def _safe(dest: Path, name: str) -> Path:
    target = (dest / name).resolve()
    if not str(target).startswith(str(dest.resolve())):
        raise RuntimeError(f"unsafe path in archive: {name}")
    return target


def extract_zip(archive: Path, dest: Path, strip=0, progress=lambda d, t: None, cancelled=lambda: False):
    dest.mkdir(parents=True, exist_ok=True)
    with zipfile.ZipFile(archive) as z:
        infos = z.infolist()
        for i, info in enumerate(infos):
            if cancelled():
                raise Cancelled()
            parts = Path(info.filename).parts[strip:]
            if not parts:
                continue
            target = _safe(dest, str(Path(*parts)))
            mode = info.external_attr >> 16
            if info.is_dir():
                target.mkdir(parents=True, exist_ok=True)
            elif stat.S_ISLNK(mode):
                target.parent.mkdir(parents=True, exist_ok=True)
                target.unlink(missing_ok=True)
                os.symlink(z.read(info).decode(), target)
            else:
                target.parent.mkdir(parents=True, exist_ok=True)
                with z.open(info) as src, open(target, "wb") as out:
                    shutil.copyfileobj(src, out, 1 << 20)
                if mode & 0o111:
                    os.chmod(target, (mode & 0o777) | 0o600)
            if i % 50 == 0:
                progress(i, len(infos))
    progress(len(infos), len(infos))


def extract_tar(archive: Path, dest: Path, strip=0, only=None, progress=lambda d, t: None,
                cancelled=lambda: False):
    """Unpack a tar(.gz/.xz). only: a predicate on the stripped member name (skip what is not needed)."""
    dest.mkdir(parents=True, exist_ok=True)
    total = archive.stat().st_size
    with open(archive, "rb") as raw, tarfile.open(fileobj=raw, mode="r|*") as tar:
        for member in tar:
            if cancelled():
                raise Cancelled()
            parts = Path(member.name).parts[strip:]
            if not parts:
                continue
            name = str(Path(*parts))
            if only and not only(name):
                continue
            member.name = name
            tar.extract(member, dest, filter="tar")
            progress(raw.tell(), total)
    progress(total, total)


def unpack(name: str, archive: Path, dest: Path, strip=0, only=None, progress=lambda d, t: None,
           cancelled=lambda: False):
    kind = pins.DOWNLOADS[name][2]
    if kind == "zip":
        extract_zip(archive, dest, strip, progress, cancelled)
    else:
        extract_tar(archive, dest, strip, only, progress, cancelled)
