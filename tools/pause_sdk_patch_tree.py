#!/usr/bin/env python3
"""Build the pause SDK overlay tree in a temporary index, without changing a checkout."""
import argparse
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile


def patch_tree(sdk, manifest, expected_base=None):
    sdk, manifest = Path(sdk).resolve(), Path(manifest).resolve()
    spec = json.loads(manifest.read_text(encoding="utf-8"))
    base = spec["base_commit"]
    if expected_base is not None and base != expected_base:
        raise ValueError("Pause SDK patch requires an audit against the new SDK pin")
    subprocess.run(["git", "-C", str(sdk), "cat-file", "-e", base + "^{commit}"], check=True)
    with tempfile.TemporaryDirectory(prefix="fh1-pause-sdk-index-") as temp:
        env = dict(os.environ, GIT_INDEX_FILE=str(Path(temp) / "index"))

        def git(*args):
            return subprocess.check_output(["git", "-C", str(sdk), *args], env=env)

        git("read-tree", base)
        for entry in spec["patches"]:
            patch = manifest.parent / entry["file"]
            if hashlib.sha256(patch.read_bytes()).hexdigest() != entry["sha256"]:
                raise ValueError(f"SDK patch hash mismatch: {patch.name}")
            git("apply", "--cached", "--check", str(patch))
            git("apply", "--cached", str(patch))
        tree = git("write-tree").decode().strip()
        # Validate every changed/new source, including the exact baseline inputs.
        actual = set(git("diff-tree", "--no-commit-id", "--name-only", "-r", base, tree).decode().splitlines())
        if actual != set(spec["files"]):
            raise ValueError("SDK patch file set differs from its audit manifest")
        for name, hashes in spec["files"].items():
            if hashes["before_sha256"] is not None:
                data = git("show", base + ":" + name)
                if hashlib.sha256(data).hexdigest() != hashes["before_sha256"]:
                    raise ValueError(f"SDK baseline mismatch: {name}")
            data = git("show", tree + ":" + name)
            if hashlib.sha256(data).hexdigest() != hashes["after_sha256"]:
                raise ValueError(f"SDK patched source mismatch: {name}")
        return tree


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--sdk", type=Path, required=True)
    parser.add_argument("--base", help="Require this exact pinned ReIGlue commit")
    parser.add_argument("--manifest", type=Path, default=Path(__file__).resolve().parents[1] / "patches/pause-camera-sdk/manifest.json")
    args = parser.parse_args()
    print(patch_tree(args.sdk, args.manifest, args.base))


if __name__ == "__main__":
    main()
