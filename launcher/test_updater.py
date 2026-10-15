"""Release selection and download integrity, without accessing the network."""
import hashlib
import io
import json
import sys
import tempfile
import unittest
import urllib.error
from pathlib import Path
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parent))
from forza_launcher import GITHUB_URL, fetch, updater


def release_data():
    names = ["ForzaHorizon1-ReImagined-Setup-0.1.8.exe",
             "ForzaHorizon1-ReImagined-linux-x86_64.tar.gz",
             "ForzaHorizon1-ReImagined-x86_64.flatpak"]
    return {"tag_name": "v0.1.8", "assets": [
        {"name": name, "browser_download_url": GITHUB_URL + "/releases/download/v0.1.8/" + name,
         "digest": "sha256:" + hashlib.sha256(b"update").hexdigest(), "size": 6} for name in names]}


class UpdateTests(unittest.TestCase):
    def test_versions_do_not_downgrade_or_compare_development_builds(self):
        r = updater.parse_release(release_data())
        for current in ("0.1.7", "v0.1.7", "0.1.7+build.3"):
            self.assertTrue(r.newer_than(current))
        for current in ("0.1.8", "0.1.9", "0.10.0", "0.0.0-dev.abc", "dev", "0.1.8-rc.1"):
            self.assertFalse(r.newer_than(current))
        for tag in ("nightly", "v0.1.9-beta", "../../tmp", "v01.2.3"):
            with self.subTest(tag=tag), self.assertRaises(updater.UpdateError):
                updater.parse_release({"tag_name": tag})
        for flag in ("draft", "prerelease"):
            with self.assertRaises(updater.UpdateError):
                updater.parse_release({**release_data(), flag: True})

    def test_platform_selection(self):
        r = updater.parse_release(release_data())
        self.assertTrue(r.asset_for("win32", machine="AMD64").name.endswith(".exe"))
        self.assertTrue(r.asset_for("linux").name.endswith(".tar.gz"))
        self.assertTrue(r.asset_for("linux", flatpak=True).name.endswith(".flatpak"))
        self.assertIsNone(r.asset_for("darwin"))
        self.assertIsNone(r.asset_for("linux", machine="aarch64"))

    def test_untrusted_or_unverified_assets_are_not_offered(self):
        for key, value in (("name", "../bad.exe"), ("browser_download_url", "https://evil.test/update"),
                           ("digest", None), ("digest", "sha256:no"), ("size", -1), ("size", True)):
            data = release_data()
            data["assets"][0][key] = value
            with self.subTest(key=key, value=value):
                self.assertIsNone(updater.parse_release(data).asset_for("win32"))

    def test_api_errors_and_timeout(self):
        for code, message in ((403, "limit"), (429, "limit"), (404, "No published"), (500, "HTTP 500")):
            error = urllib.error.HTTPError(updater.API_URL, code, "error", {}, None)
            with patch.object(updater.urllib.request, "urlopen", side_effect=error), \
                    self.assertRaisesRegex(updater.UpdateError, message):
                updater.check()
            error.close()
        with patch.object(updater.urllib.request, "urlopen", side_effect=TimeoutError()), \
                self.assertRaisesRegex(updater.UpdateError, "connection"):
            updater.check()
        with patch.object(updater.urllib.request, "urlopen", return_value=io.BytesIO(json.dumps(release_data()).encode())) as op:
            self.assertEqual(updater.check().tag, "v0.1.8")
            self.assertEqual(op.call_args.kwargs["timeout"], updater.TIMEOUT)
            self.assertIs(op.call_args.kwargs["context"], fetch.ssl_context())

    def test_download_integrity_cancel_and_retry(self):
        asset = updater.parse_release(release_data()).asset_for("win32")
        with tempfile.TemporaryDirectory() as tmp:
            folder = Path(tmp)
            for body in (b"bad", b"longer than expected", b"broken"):
                with patch.object(updater.urllib.request, "urlopen", return_value=io.BytesIO(body)), \
                        self.assertRaises(updater.UpdateError):
                    updater.download(asset, folder)
                self.assertFalse((folder / asset.name).exists())
                self.assertEqual(list(folder.iterdir()), [])
            with patch.object(updater.urllib.request, "urlopen", return_value=io.BytesIO(b"update")), \
                    self.assertRaises(fetch.Cancelled):
                checks = iter([False, False, True])
                updater.download(asset, folder, cancelled=lambda: next(checks))
            self.assertEqual(list(folder.iterdir()), [])
            with patch.object(updater.urllib.request, "urlopen", return_value=io.BytesIO(b"update")) as op:
                path = updater.download(asset, folder)
                self.assertIs(op.call_args.kwargs["context"], fetch.ssl_context())
            self.assertEqual(path.read_bytes(), b"update")
            with patch.object(updater.urllib.request, "urlopen") as op:
                self.assertEqual(updater.download(asset, folder), path)
                op.assert_not_called()
            path.write_bytes(b"broken")
            with patch.object(updater.urllib.request, "urlopen", return_value=io.BytesIO(b"update")):
                self.assertEqual(updater.download(asset, folder).read_bytes(), b"update")


if __name__ == "__main__":
    unittest.main()
