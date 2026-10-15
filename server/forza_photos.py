"""Forza Live: photo uploads (Forza.WebServices.LargeFileUpload / Storefront.PhotoCreate and
Xls.WebServices.Content).

Photo mode's SAVE and "choose large photo" end at "uploading to forzamotorsport.net". The game
first asks for a content file (LargeFileUpload.Create(payloadSize, overwriteOldest, name), or
Storefront.PhotoCreate with the photo's header and preview, both answer a contentFileId), then
sends the picture with Content.UploadContent(contentId, payloadOffset, payloadTotalSize,
uploadData) and carries on when the answer's payloadUploadedSize reaches the total. A server
that answers neither leaves the game at the upload screen (empty answers read as id 0 and
0 bytes uploaded).

A player's files are kept as sent, under DATA_DIR/photos/ (counted in the server's disk quota),
at most MAX_PER_PLAYER of them, each up to limits.MAX_BLOB bytes; with overwriteOldest the oldest
of the player's files makes room, without it a full player gets a failure. DATA_DIR/photos.json
is the index. Content.DownloadContent gives a player's own file back (nobody else's; anything else gets
the empty answer the game has always had for it).
"""

import json
import logging
import os
import time

import forza_limits as limits
import forza_xtsw as x
import forza_xtsw_all as A

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"
MAX_PER_PLAYER = 100
MAX_PREVIEW = 256 << 10  # a photo's preview (thumbnail) in PhotoCreate
MAX_CHUNK = 1 << 20      # bytes handed back by one DownloadContent
INDEX = {"nextId": 1, "files": {}}
_loaded = [False]


def configure(data_dir):
    global DATA_DIR
    DATA_DIR = data_dir
    INDEX.update({"nextId": 1, "files": {}})
    _loaded[0] = False


def index_path():
    return os.path.join(DATA_DIR, "photos.json")


def file_path(fid, suffix=".bin"):
    return os.path.join(DATA_DIR, "photos", "%d%s" % (fid, suffix))


def load():
    if not _loaded[0]:
        _loaded[0] = True
        try:
            with open(index_path()) as f:
                INDEX.update(json.load(f))
        except (OSError, ValueError):
            pass
    return INDEX


def save():
    os.makedirs(DATA_DIR, exist_ok=True)
    tmp = index_path() + ".tmp"
    with open(tmp, "w") as f:
        json.dump(INDEX, f, indent=1)
    os.replace(tmp, index_path())


def remove(fid):
    load()["files"].pop(str(fid), None)
    for suffix in (".bin", ".prev"):
        try:
            os.remove(file_path(fid, suffix))
        except OSError:
            pass


def create(xuid, size, overwrite, name="", kind="photo", preview=b""):
    """A new content file of `size` bytes for the player; its id. ValueError: refused."""
    d = load()
    if not 0 < size <= limits.MAX_BLOB:
        raise ValueError("file of %d bytes refused" % size)
    if len(preview) > MAX_PREVIEW:
        raise ValueError("preview of %d bytes refused" % len(preview))
    mine = sorted((f["created"], int(i)) for i, f in d["files"].items() if f["owner"] == xuid)
    while len(mine) >= MAX_PER_PLAYER:
        if not overwrite:
            raise ValueError("storage full")
        remove(mine.pop(0)[1])
    limits.accept(size + len(preview))
    fid = d["nextId"]
    d["nextId"] += 1
    os.makedirs(os.path.dirname(file_path(fid)), exist_ok=True)
    with open(file_path(fid), "wb") as f:
        f.truncate(size)  # sparse: the quota counts the whole file from the start
    if preview:
        with open(file_path(fid, ".prev"), "wb") as f:
            f.write(preview)
    d["files"][str(fid)] = {"owner": xuid, "size": size, "got": 0, "kind": kind,
                            "name": name[:64], "created": time.time(), "done": False}
    save()
    log.info("photos: %016X: file %d (%s, %d bytes)", xuid, fid, kind, size)
    return fid


def large_file_create(xuid, body):
    p, _ = x.decode(A.ForzaLargeFileUploadCreate_Params, body)
    fid = create(xuid, p["payloadSize"], p["overwriteOldest"], p["userFileName"], "large")
    return x.reply(A.ForzaLargeFileUploadCreate_Results, {"contentFileId": fid})


def photo_create(xuid, body):
    p, _ = x.decode(A.ForzaStorefrontPhotoCreate_Params, body)
    size = p["payloadSize"] or p["fileHeader"]["PayloadSize"]
    fid = create(xuid, size, True, p["fileHeader"]["Title"], "photo", p["previewData"])
    return x.reply(A.ForzaStorefrontPhotoCreate_Results, {"contentFileId": fid})


def upload_content(xuid, body):
    p, _ = x.decode(A.ContentUploadContent_Params, body)
    f = load()["files"].get(str(p["contentId"]))
    if f is None or f["owner"] != xuid:
        return x.failure(1, "no such file")
    data, offset = p["uploadData"], p["payloadOffset"]
    if offset > f["got"] or offset + len(data) > f["size"]:
        return x.failure(2, "bad range")  # a gap, or more than the size it announced
    if data:
        with open(file_path(p["contentId"]), "r+b") as out:
            out.seek(offset)
            out.write(data)
    f["got"] = max(f["got"], offset + len(data))
    if f["got"] >= f["size"] and not f["done"]:
        f["done"] = True
        save()
        log.info("photos: %016X: file %d uploaded (%d bytes)", xuid, p["contentId"], f["size"])
    return x.reply(A.ContentUploadContent_Results, {"payloadUploadedSize": f["got"]})


def download_content(xuid, body):
    p, _ = x.decode(A.ContentDownloadContent_Params, body)
    f = load()["files"].get(str(p["contentId"]))
    if f is None or f["owner"] != xuid or not f["done"]:
        # What every other content id got before: the game asks for id 0 after sign-in and
        # a failure there would be retried; nothing (isLastBuffer false, 0 bytes) is final.
        return x.reply(A.ContentDownloadContent_Results, {})
    offset, want = p["payloadOffset"], min(p["requestedSize"], MAX_CHUNK)
    with open(file_path(p["contentId"]), "rb") as src:
        src.seek(offset)
        data = src.read(want)
    return x.reply(A.ContentDownloadContent_Results, {
        "isLastBuffer": offset + len(data) >= f["size"], "downloadData": data,
        "totalSize": f["size"]})


def services():
    return {
        ("LargeFileUpload", "Create"): large_file_create,
        ("Storefront", "PhotoCreate"): photo_create,
        ("Xls.Content", "UploadContent"): upload_content,
        ("Xls.Content", "DownloadContent"): download_content,
    }
