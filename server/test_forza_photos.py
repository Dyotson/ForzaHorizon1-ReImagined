"""Photo uploads (forza_photos.py): the calls photo mode's upload makes, as the game's type
descriptors encode them (forza_xtsw_all.py).

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

import tempfile
import unittest

import forza_limits as limits
import forza_live_lsp as lsp
import forza_photos as photos
import forza_xtsw as x
import forza_xtsw_all as A

ALICE, BOB = 0x0009000000000A01, 0x0009000000000B02


def call(xuid, cls, method, ptype, params, rtype=None):
    """A service call through the server's dispatch; the decoded results."""
    headers = {"x-classname": cls, "x-method": method, "x-userid": "%016X" % xuid}
    reply = lsp.service_call(headers, bytes(x.encode(ptype, params)))
    if reply[:4] != bytes(4):  # not a success
        return None
    return x.decode(rtype, reply, 12)[0] if rtype else {}


def large_create(xuid, size, overwrite=False):
    r = call(xuid, "Forza.WebServices.LargeFileUpload", "Create", A.ForzaLargeFileUploadCreate_Params,
             {"payloadSize": size, "overwriteOldest": overwrite, "userFileName": "photo.jpg"},
             A.ForzaLargeFileUploadCreate_Results)
    return r and r["contentFileId"]


def upload(xuid, fid, offset, total, data):
    r = call(xuid, "Xls.WebServices.Content", "UploadContent", A.ContentUploadContent_Params,
             {"contentId": fid, "payloadOffset": offset, "payloadTotalSize": total, "uploadData": data},
             A.ContentUploadContent_Results)
    return r and r["payloadUploadedSize"]


def download(xuid, fid, offset=0, size=1 << 20):
    return call(xuid, "Xls.WebServices.Content", "DownloadContent", A.ContentDownloadContent_Params,
                {"contentId": fid, "payloadOffset": offset, "requestedSize": size, "userInitiated": True},
                A.ContentDownloadContent_Results)


class PhotoTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        limits.configure(self.dir.name)
        photos.configure(self.dir.name)

    def tearDown(self):
        self.dir.cleanup()

    def test_upload_in_chunks_completes(self):
        data = bytes(range(256)) * 1000  # 256,000 bytes
        fid = large_create(ALICE, len(data))
        self.assertGreater(fid, 0)
        done = 0
        while done < len(data):
            chunk = data[done:done + 65536]
            done = upload(ALICE, fid, done, len(data), chunk)
        self.assertEqual(done, len(data))
        got = download(ALICE, fid)
        self.assertEqual(got["downloadData"], data)
        self.assertTrue(got["isLastBuffer"])
        self.assertEqual(got["totalSize"], len(data))
        photos.configure(self.dir.name)  # a restart: the index is on disk
        self.assertEqual(download(ALICE, fid)["downloadData"], data)

    def test_photo_create_stores_header_and_preview(self):
        r = call(ALICE, "Forza.WebServices.Storefront", "PhotoCreate", A.ForzaStorefrontPhotoCreate_Params,
                 {"fileHeader": {"Title": "Sunset", "PayloadSize": 3}, "payloadSize": 3,
                  "previewData": b"thumb"}, A.ForzaStorefrontPhotoCreate_Results)
        fid = r["contentFileId"]
        self.assertEqual(upload(ALICE, fid, 0, 3, b"abc"), 3)
        self.assertEqual(download(ALICE, fid)["downloadData"], b"abc")
        with open(photos.file_path(fid, ".prev"), "rb") as f:
            self.assertEqual(f.read(), b"thumb")

    def test_other_players_have_no_access(self):
        fid = large_create(ALICE, 4)
        self.assertIsNone(upload(BOB, fid, 0, 4, b"evil"))
        self.assertEqual(upload(ALICE, fid, 0, 4, b"good"), 4)
        empty = {"isLastBuffer": False, "downloadData": b"", "totalSize": 0}
        self.assertEqual(download(BOB, fid), empty)    # nobody else's file, no failure
        self.assertEqual(download(ALICE, 999), empty)
        self.assertEqual(download(ALICE, 0), empty)

    def test_ranges_are_checked(self):
        fid = large_create(ALICE, 10)
        self.assertIsNone(upload(ALICE, fid, 5, 10, b"12345"))     # a gap
        self.assertIsNone(upload(ALICE, fid, 0, 10, b"x" * 11))    # more than announced
        self.assertEqual(upload(ALICE, fid, 0, 10, b"12345"), 5)
        self.assertEqual(upload(ALICE, fid, 0, 10, b"12345"), 5)   # a resent chunk
        self.assertEqual(download(ALICE, fid)["totalSize"], 0)     # not complete yet
        self.assertEqual(upload(ALICE, fid, 5, 10, b"67890"), 10)

    def test_size_limits(self):
        self.assertIsNone(large_create(ALICE, 0))
        self.assertIsNone(large_create(ALICE, limits.MAX_BLOB + 1))
        self.assertIsNotNone(large_create(ALICE, limits.MAX_BLOB))

    def test_per_player_cap_and_overwrite_oldest(self):
        ids = [large_create(ALICE, 1) for _ in range(photos.MAX_PER_PLAYER)]
        self.assertTrue(all(ids))
        self.assertIsNone(large_create(ALICE, 1))              # full, not allowed to overwrite
        self.assertIsNotNone(large_create(BOB, 1))             # another player is not affected
        newest = large_create(ALICE, 1, overwrite=True)        # the oldest makes room
        self.assertGreater(newest, ids[-1])
        self.assertNotIn(str(ids[0]), photos.load()["files"])
        mine = [f for f in photos.load()["files"].values() if f["owner"] == ALICE]
        self.assertEqual(len(mine), photos.MAX_PER_PLAYER)

    def test_disk_quota(self):
        old = limits.DISK_CAP
        limits.DISK_CAP = 100
        try:
            self.assertIsNotNone(large_create(ALICE, 60))
            limits._cache[0] = 0  # rescan
            self.assertIsNone(large_create(ALICE, 60))
        finally:
            limits.DISK_CAP = old


if __name__ == "__main__":
    unittest.main()
