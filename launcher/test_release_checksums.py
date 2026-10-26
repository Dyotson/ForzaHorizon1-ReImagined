"""Release manifests hash the final bytes, use download-relative names and survive reruns."""
import hashlib
import importlib.util
from pathlib import Path
import subprocess
import sys
import tempfile
import unittest


SCRIPT = Path(__file__).parent / "packaging" / "release_checksums.py"
spec = importlib.util.spec_from_file_location("release_checksums", SCRIPT)
checksums = importlib.util.module_from_spec(spec)
spec.loader.exec_module(checksums)


class ReleaseChecksumsTests(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.assets = Path(self.temp.name)

    def test_cli_hashes_final_bytes_and_emits_portable_sorted_filenames(self):
        (self.assets / "Setup.exe").write_bytes(b"abc")
        (self.assets / "Linux package.tar.gz").write_bytes(bytes(range(256)) * 8193)
        (self.assets / "Game.flatpak").write_bytes(b"")
        subprocess.run([sys.executable, str(SCRIPT), str(self.assets)], check=True,
                       capture_output=True, text=True)
        lines = (self.assets / "SHA256SUMS").read_bytes().decode("utf-8").splitlines()
        entries = [line.split("  ", 1) for line in lines]
        self.assertEqual([name for _, name in entries],
                         ["Game.flatpak", "Linux package.tar.gz", "Setup.exe"])
        self.assertEqual(entries[0][0], "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855")
        self.assertEqual(entries[2][0], "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad")
        for digest, name in entries:
            self.assertEqual(digest, hashlib.sha256((self.assets / name).read_bytes()).hexdigest())
        self.assertNotIn(b"\r", (self.assets / "SHA256SUMS").read_bytes())

    def test_rerun_replaces_stale_manifest_without_hashing_itself(self):
        package = self.assets / "Setup.exe"
        package.write_bytes(b"old bytes")
        manifest = checksums.write_checksums(self.assets)
        before = manifest.read_bytes()
        self.assertEqual(checksums.write_checksums(self.assets).read_bytes(), before)
        package.write_bytes(b"abc")
        checksums.write_checksums(self.assets)
        self.assertEqual(manifest.read_text(),
                         "ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad  Setup.exe\n")

    def test_missing_assets_fail_instead_of_publishing_empty_checksums(self):
        for stale in (False, True):
            with self.subTest(stale=stale):
                if stale:
                    (self.assets / "SHA256SUMS").write_text("old manifest")
                with self.assertRaisesRegex(ValueError, "No release assets"):
                    checksums.write_checksums(self.assets)

    def test_nested_artifact_directory_fails_before_replacing_manifest(self):
        (self.assets / "nested").mkdir()
        manifest = self.assets / "SHA256SUMS"
        manifest.write_text("old manifest")
        with self.assertRaisesRegex(ValueError, "Not a supported release asset"):
            checksums.write_checksums(self.assets)
        self.assertEqual(manifest.read_text(), "old manifest")


if __name__ == "__main__":
    unittest.main()
