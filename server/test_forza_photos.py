"""Photo uploads (forza_photos.py): the calls photo mode's upload makes, as the game's type
descriptors encode them (forza_xtsw_all.py).

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

import tempfile
import unittest

import forza_clubs as clubs
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
        clubs.configure(self.dir.name, lambda xuid: {"gamertag": {ALICE: "Alice", BOB: "Bob"}.get(xuid, "")})

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
        r = call(ALICE, "Forza.WebServices.StorefrontPhoto", "Create", A.ForzaStorefrontPhotoCreate_Params,
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


SF = "Forza.WebServices.Storefront"


def share(xuid, title="Sunset", data=b"abc", **header):
    """A photo shared the way the game does it: StorefrontPhoto.Create, then the picture."""
    r = call(xuid, "Forza.WebServices.StorefrontPhoto", "Create", A.ForzaStorefrontPhotoCreate_Params,
             {"fileHeader": dict(Title=title, Type=5, PayloadSize=len(data), **header),
              "photoDesc": {"base": {"EnvironmentId": 63, "TrackId": 77, "ForStorefrontUse": True}},
              "payloadSize": len(data), "previewData": b"thumb-" + title.encode()},
             A.ForzaStorefrontPhotoCreate_Results)
    if r is None:
        return None
    upload(xuid, r["contentFileId"], 0, len(data), data)
    return r["contentFileId"]


def pages(owner):
    return call(ALICE, SF, "GetPhotoPageList", A.ForzaStorefrontGetPhotoPageList_Params,
                {"ownerXuid": owner, "maxPages": 10}, A.ForzaStorefrontGetPhotoPageList_Results)["Pages"]


def files(owner, start=0, count=20, page=photos.PHOTO_PAGE):
    return call(ALICE, SF, "GetFileListForPage", A.ForzaStorefrontGetFileListForPage_Params,
                {"ownerXuid": owner, "startAt": start, "maxFiles": count, "page": page},
                A.ForzaStorefrontGetFileListForPage_Results)


class StorefrontTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        limits.configure(self.dir.name)
        photos.configure(self.dir.name)
        clubs.configure(self.dir.name, lambda xuid: {"gamertag": {ALICE: "Alice", BOB: "Bob"}.get(xuid, "")})

    def tearDown(self):
        self.dir.cleanup()

    def test_empty_storefront_has_all_slots_free(self):
        self.assertEqual(pages(ALICE), [{"Page": photos.PHOTO_PAGE, "TotalSlots": photos.STOREFRONT_SLOTS,
                                         "UsedSlots": 0}])
        self.assertEqual(files(ALICE)["Files"], [])

    def test_shared_photo_is_listed_with_slot_used(self):
        fid = share(ALICE, "Sunset", b"abc", Description="golden hour")
        self.assertGreater(fid, 0)
        self.assertEqual(pages(ALICE)[0]["UsedSlots"], 1)
        self.assertEqual(pages(BOB)[0]["UsedSlots"], 0)
        got = files(ALICE)
        self.assertEqual(got["totalFiles"], 1)
        f = got["Files"][0]
        self.assertEqual((f["Id"], f["Title"], f["Description"], f["Type"], f["PayloadSize"], f["Ordinal"]),
                         (fid, "Sunset", "golden hour", 5, 3, 63))
        self.assertEqual(f["PreviewPayload"], b"thumb-Sunset")
        self.assertEqual(f["Owner"]["qwXuid"], ALICE)
        self.assertEqual(f["Owner"]["wzGamerTag"], "Alice")
        self.assertEqual(files(BOB)["Files"], [])                     # per player
        self.assertEqual(files(ALICE, page=photos.PHOTO_PAGE + 1)["Files"], [])

    def test_unfinished_upload_is_not_listed_but_holds_its_slot(self):
        r = call(ALICE, "Forza.WebServices.StorefrontPhoto", "Create", A.ForzaStorefrontPhotoCreate_Params,
                 {"fileHeader": {"Title": "x", "PayloadSize": 9}, "payloadSize": 9},
                 A.ForzaStorefrontPhotoCreate_Results)
        self.assertEqual(files(ALICE)["Files"], [])
        self.assertEqual(pages(ALICE)[0]["UsedSlots"], 1)
        photos.load()["files"][str(r["contentFileId"])]["created"] -= photos.STALE_UPLOAD + 1
        self.assertEqual(pages(ALICE)[0]["UsedSlots"], 0)             # abandoned: slot is back

    def test_paging_newest_first(self):
        ids = [share(ALICE, "p%d" % i) for i in range(5)]
        for i, fid in enumerate(ids):                                  # same-second creation
            photos.load()["files"][str(fid)]["created"] += i
        got = files(ALICE, start=1, count=2)
        self.assertEqual([f["Id"] for f in got["Files"]], [ids[3], ids[2]])
        self.assertEqual(got["totalFiles"], 5)

    def test_slots_run_out_and_remove_file_frees_one(self):
        ids = [share(ALICE, "p%d" % i, b"x") for i in range(photos.STOREFRONT_SLOTS)]
        self.assertTrue(all(ids))
        self.assertEqual(pages(ALICE)[0]["UsedSlots"], photos.STOREFRONT_SLOTS)
        self.assertIsNone(share(ALICE, "one too many"))
        self.assertIsNotNone(share(BOB, "bob's own slots"))
        self.assertIsNone(call(BOB, SF, "RemoveFile", A.ForzaStorefrontRemoveFile_Params, {"fileId": ids[0]}))
        self.assertEqual(pages(ALICE)[0]["UsedSlots"], photos.STOREFRONT_SLOTS)
        call(ALICE, SF, "RemoveFile", A.ForzaStorefrontRemoveFile_Params, {"fileId": ids[0]})
        self.assertEqual(pages(ALICE)[0]["UsedSlots"], photos.STOREFRONT_SLOTS - 1)
        self.assertIsNotNone(share(ALICE, "fits again"))

    def test_others_can_download_shared_photos_only(self):
        shared, private = share(ALICE, "s", b"shared"), large_create(ALICE, 4)
        upload(ALICE, private, 0, 4, b"priv")
        self.assertEqual(download(BOB, shared)["downloadData"], b"shared")
        self.assertEqual(download(BOB, private)["downloadData"], b"")

    def test_others_can_fetch_a_photo_and_downloads_are_counted(self):
        fid = share(ALICE, "Sunset", b"picture")
        got = call(BOB, SF, "Purchase", A.ForzaStorefrontPurchase_Params,
                   {"fileId": fid, "maxPayloadSize": 100, "maxPreviewSize": 100}, A.ForzaStorefrontPurchase_Results)
        self.assertEqual(got["payload"], b"picture")
        self.assertEqual(got["purchasedFileInfo"]["Title"], "Sunset")
        self.assertIsNone(call(BOB, SF, "Purchase", A.ForzaStorefrontPurchase_Params,
                               {"fileId": fid, "maxPayloadSize": 3}, A.ForzaStorefrontPurchase_Results))
        self.assertEqual(files(ALICE)["Files"][0]["DownloadCount"], 1)
        stats = call(BOB, SF, "GetStats", A.ForzaStorefrontGetStats_Params, {"xuid": ALICE},
                     A.ForzaStorefrontGetStats_Results)["stats"]
        self.assertEqual((stats["photosUploaded"], stats["photosDownloaded"]), (1, 1))

    def test_featured_most_downloaded_and_top_rated_cover_everybody(self):
        a, b = share(ALICE, "a"), share(BOB, "b")
        photos.load()["files"][str(b)]["created"] += 1
        call(ALICE, SF, "SubmitFileRating", A.ForzaStorefrontSubmitFileRating_Params, {"fileId": b, "rating": 5})
        call(ALICE, SF, "SubmitFileRating", A.ForzaStorefrontSubmitFileRating_Params, {"fileId": a, "rating": 5})  # own
        call(BOB, SF, "Purchase", A.ForzaStorefrontPurchase_Params, {"fileId": a, "maxPayloadSize": 100},
             A.ForzaStorefrontPurchase_Results)

        def ids(method, ptype, rtype, field):
            return [f["Id"] for f in call(ALICE, SF, method, ptype, {"maxFiles": 10}, rtype)[field]]
        self.assertEqual(ids("GetFeaturedPhotos", A.ForzaStorefrontGetFeaturedPhotos_Params,
                             A.ForzaStorefrontGetFeaturedPhotos_Results, "FeaturedFiles"), [b, a])
        self.assertEqual(ids("GetMostDownloadedPhotos", A.ForzaStorefrontGetMostDownloadedPhotos_Params,
                             A.ForzaStorefrontGetMostDownloadedPhotos_Results, "MostDownloadedPhotos"), [a, b])
        self.assertEqual(ids("GetTopRatedPhotos", A.ForzaStorefrontGetTopRatedPhotos_Params,
                             A.ForzaStorefrontGetTopRatedPhotos_Results, "TopRatedPhotos"), [b, a])

    def test_search_filters_shared_photos(self):
        a = share(ALICE, "a", KeywordOne=2)
        b = share(BOB, "b", KeywordOne=3)

        def search(**q):
            any_ = {k: -1 for k in ("PriceMax", "MakeId", "ModelId", "ClassId", "KeywordOneId", "KeywordTwoId",
                                    "EnvironmentId", "TrackId", "NumLayers")}
            return [r["Id"] for r in call(
                ALICE, SF, "SearchPhotoFiles", A.ForzaStorefrontSearchPhotoFiles_Params,
                {"searchParams": {**any_, **q}, "maxFiles": 100}, A.ForzaStorefrontSearchPhotoFiles_Results)["searchResults"]]
        self.assertEqual(sorted(search()), [a, b])
        self.assertEqual(search(KeywordOneId=3), [b])
        self.assertEqual(search(GamerTag="ali"), [a])
        self.assertEqual(search(TrackId=77), sorted([a, b], reverse=True))  # the tests' photos are all at track 77
        self.assertEqual(search(TrackId=78), [])

    def test_owner_changes_title_and_description(self):
        fid = share(ALICE, "old")
        call(BOB, SF, "ChangeFileTitle", A.ForzaStorefrontChangeFileTitle_Params, {"fileId": fid, "newTitle": "bob"})
        call(ALICE, SF, "ChangeFileTitle", A.ForzaStorefrontChangeFileTitle_Params, {"fileId": fid, "newTitle": "new"})
        call(ALICE, SF, "ChangeFileDescription", A.ForzaStorefrontChangeFileDescription_Params,
             {"fileId": fid, "newDescription": "d"})
        f = files(ALICE)["Files"][0]
        self.assertEqual((f["Title"], f["Description"]), ("new", "d"))

    def test_large_uploads_are_not_on_the_storefront(self):
        fid = large_create(ALICE, 4)
        upload(ALICE, fid, 0, 4, b"data")
        self.assertEqual(pages(ALICE)[0]["UsedSlots"], 0)
        self.assertEqual(files(ALICE)["Files"], [])

    def test_shared_photos_survive_a_restart(self):
        fid = share(ALICE, "Sunset")
        photos.configure(self.dir.name)
        self.assertEqual(files(ALICE)["Files"][0]["Id"], fid)


if __name__ == "__main__":
    unittest.main()
