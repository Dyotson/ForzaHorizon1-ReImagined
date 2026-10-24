"""Write SHA256SUMS for the final, flat directory uploaded as release assets."""
import argparse
import hashlib
from pathlib import Path


def write_checksums(directory):
    directory = Path(directory)
    assets = sorted((p for p in directory.iterdir() if p.name != "SHA256SUMS"),
                    key=lambda p: p.name)
    if not assets:
        raise ValueError("No release assets to checksum")
    lines = []
    for asset in assets:
        # Keep each line usable by sha256sum -c, with a filename relative to the download folder.
        if not asset.is_file() or any(c in asset.name for c in "\r\n\\"):
            raise ValueError(f"Not a supported release asset: {asset.name!r}")
        with asset.open("rb") as stream:
            digest = hashlib.file_digest(stream, "sha256").hexdigest()
        lines.append(f"{digest}  {asset.name}\n")
    manifest = directory / "SHA256SUMS"
    manifest.write_text("".join(lines), encoding="utf-8", newline="\n")
    return manifest


if __name__ == "__main__":
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("directory", type=Path)
    print(write_checksums(parser.parse_args().directory))
