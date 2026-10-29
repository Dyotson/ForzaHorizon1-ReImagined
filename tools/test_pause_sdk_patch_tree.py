"""Checks the audited SDK overlay without compiling the renderer or changing a checkout."""
import hashlib
import json
import os
from pathlib import Path
import subprocess
import tempfile
import unittest

from pause_sdk_patch_tree import patch_tree


class PatchTreeTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.root = Path(self.temp.name)
        self.sdk = self.root / "sdk"
        self.sdk.mkdir()
        self.env = dict(os.environ, GIT_AUTHOR_NAME="SDK test", GIT_AUTHOR_EMAIL="sdk@example.invalid",
                        GIT_COMMITTER_NAME="SDK test", GIT_COMMITTER_EMAIL="sdk@example.invalid")
        self.git("init", "-q")
        self.git("config", "core.autocrlf", "false")
        (self.sdk / "source.txt").write_bytes(b"before\n")
        self.git("add", "source.txt")
        tree = self.git("write-tree").decode().strip()
        self.base = self.git("commit-tree", tree, "-m", "fixture").decode().strip()
        self.git("update-ref", "HEAD", self.base)
        patch = b"diff --git a/source.txt b/source.txt\n--- a/source.txt\n+++ b/source.txt\n@@ -1 +1 @@\n-before\n+after\n"
        (self.root / "change.patch").write_bytes(patch)
        self.spec = {"version": 1, "base_commit": self.base,
                     "patches": [{"file": "change.patch", "sha256": hashlib.sha256(patch).hexdigest()}],
                     "files": {"source.txt": {"before_sha256": hashlib.sha256(b"before\n").hexdigest(),
                                                "after_sha256": hashlib.sha256(b"after\n").hexdigest()}}}
        self.manifest = self.root / "manifest.json"
        self.save()

    def git(self, *args):
        return subprocess.check_output(["git", "-C", str(self.sdk), *args], env=self.env, stderr=subprocess.PIPE)

    def save(self):
        self.manifest.write_text(json.dumps(self.spec))

    def tearDown(self):
        self.temp.cleanup()

    def test_repeat_is_identical_and_checkout_and_index_are_untouched(self):
        before = (self.sdk / ".git/index").read_bytes()
        tree = patch_tree(self.sdk, self.manifest, self.base)
        self.assertEqual(tree, patch_tree(self.sdk, self.manifest, self.base))
        self.assertEqual(self.git("show", tree + ":source.txt"), b"after\n")
        self.assertEqual((self.sdk / "source.txt").read_bytes(), b"before\n")
        self.assertEqual((self.sdk / ".git/index").read_bytes(), before)

    def test_new_pin_requires_a_new_audit(self):
        with self.assertRaisesRegex(ValueError, "new SDK pin"):
            patch_tree(self.sdk, self.manifest, "0" * 40)

    def test_modified_patch_is_rejected(self):
        (self.root / "change.patch").write_bytes(b"changed")
        with self.assertRaisesRegex(ValueError, "hash mismatch"):
            patch_tree(self.sdk, self.manifest)

    def test_manifest_cannot_omit_changed_files_or_misstate_source_hash(self):
        self.spec["files"] = {}
        self.save()
        with self.assertRaisesRegex(ValueError, "file set"):
            patch_tree(self.sdk, self.manifest)
        self.spec["files"] = {"source.txt": {"before_sha256": "0" * 64, "after_sha256": "0" * 64}}
        self.save()
        with self.assertRaisesRegex(ValueError, "baseline mismatch"):
            patch_tree(self.sdk, self.manifest)


if __name__ == "__main__":
    unittest.main()
