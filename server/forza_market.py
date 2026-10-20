"""Forza Live: the Auction House and car gifts, between players.

A seller lists a car (Auction.Create: the car, its parts, tuning, livery and history, an
opening and a buyout price, a duration; the game takes the car out of the garage and the
listing fee from the credits). Other players find it (SearchForAuction, GetAuctions,
GetDetails) and bid (PlaceBid; the game takes the bid from the bidder's credits). When it
ends (the duration is over, or a bid reached the buyout price):

    winner   "WON" (status 5)     ResolveWon (RetrieveCarForAuction): the car goes into the
                                  winner's garage
    seller   "SOLD" (status 2)    ReceiveMoney (RetrievePaymentForAuctionSold): the winning bid
    outbid   "OUTBID"/"LOST" (6/7) ReceiveMoney (RetrievePaymentsForAuctionLost): their bid
                                  back (also while the auction runs)
    no bids  "EXPIRED" (status 1) ResolveFailedAuction (RetrieveCarForAuction: the car back)
                                  or Restart

The server keeps the auctions and every player's held and owed credits; the game adds the
amounts it is given to its own credits (they live in the save). Statuses and the action
bits (allowedActions) are the game's: the action table at 0x8323E528 (label, bit, 1 =
bidder / 2 = seller action) and the status labels in sub_827DC788 (docs/title_services.md).

Car gifts (Gifting.SendCarGift / RetrieveCarGift) are kept per recipient.

DATA_DIR/auctions.json and gifts.json hold the records, DATA_DIR/market/ the blobs (car
parts, tuning, livery, thumbnails) as the game sent them.
"""

import json
import logging
import os
import time
import uuid

import forza_notifications as notifications
import forza_limits as limits
import forza_xtsw as x
import forza_xtsw_all as A

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"

# Status (per viewer) and allowedActions bits, as the game reads them.
ACTIVE, EXPIRED, SOLD, CANCELLED, HIGH_BIDDER, WON, OUTBID, LOST = range(8)
BID, BUYOUT, RESOLVE_WON, RESOLVE_FAILED, RESTART, CANCEL, REPORT, REMOVE_BANNED, \
    RECEIVE_REFUND, RECEIVE_SALE = (1 << i for i in range(10))

# Listing fees by duration (hours, credits) and the search menus' thresholds.
FEES = [(1, 250), (4, 500), (12, 1000), (24, 1500), (48, 2500), (72, 3500)]
DEFAULT_HOURS = 24
PRICE_STEPS = [5000, 10000, 25000, 50000, 100000, 250000, 500000, 1000000, 2500000]
YEAR_STEPS = [1950, 1960, 1970, 1980, 1990, 2000, 2005, 2010]
POWER_STEPS = [100, 200, 300, 400, 500, 600, 800]
REFRESH_SECONDS = 30
BLOBS = ("liveryDetails", "partsInstalled", "partsInTrunk", "tuning")


def configure(data_dir):
    global DATA_DIR
    DATA_DIR = data_dir
    MARKET.__init__()


def jsonable(v):
    if isinstance(v, bytes):
        return {"hex": v.hex()}
    if isinstance(v, dict):
        return {k: jsonable(w) for k, w in v.items()}
    if isinstance(v, list):
        return [jsonable(w) for w in v]
    return v


def unjson(v):
    if isinstance(v, dict):
        if set(v) == {"hex"}:
            return bytes.fromhex(v["hex"])
        return {k: unjson(w) for k, w in v.items()}
    if isinstance(v, list):
        return [unjson(w) for w in v]
    return v


class Market:
    def __init__(self):
        self.auctions = {}   # id -> record
        self.gifts = {}      # id -> record
        self.textures = {}   # id -> {"texture": file, "thumbnail": file}
        self.next_id = 1
        self.loaded = False

    def path(self, name):
        return os.path.join(DATA_DIR, name)

    def load(self):
        if self.loaded:
            return
        self.loaded = True
        try:
            with open(self.path("auctions.json")) as f:
                data = json.load(f)
            self.auctions = {int(k): unjson(v) for k, v in data.get("auctions", {}).items()}
            self.textures = {int(k): v for k, v in data.get("textures", {}).items()}
            self.next_id = data.get("nextId", 1)
        except (OSError, ValueError):
            pass
        try:
            with open(self.path("gifts.json")) as f:
                data = json.load(f)
            self.gifts = {int(k): unjson(v) for k, v in data.get("gifts", {}).items()}
            self.next_id = max(self.next_id, data.get("nextId", 1))
        except (OSError, ValueError):
            pass

    def save(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        for name, data in (("auctions.json", {"auctions": self.auctions, "textures": self.textures,
                                              "nextId": self.next_id}),
                           ("gifts.json", {"gifts": self.gifts, "nextId": self.next_id})):
            tmp = self.path(name) + ".tmp"
            with open(tmp, "w") as f:
                json.dump(jsonable(data), f, indent=1)
            os.replace(tmp, self.path(name))

    def new_id(self):
        self.load()
        i = self.next_id
        self.next_id += 1
        return i

    def write_blob(self, name, data):
        limits.accept(len(data))
        folder = self.path("market")
        os.makedirs(folder, exist_ok=True)
        with open(os.path.join(folder, name), "wb") as f:
            f.write(data)
        return name

    def read_blob(self, name, limit=0):
        if not name:
            return b""
        try:
            with open(os.path.join(self.path("market"), name), "rb") as f:
                data = f.read()
        except OSError:
            return b""
        return data[:limit] if limit else data


MARKET = Market()


def key(xuid):
    return "%016X" % xuid


# ---- Auctions ----

def ended(a, now=None):
    return a.get("closedAt") is not None or (now or time.time()) >= a["closes"]


def close_if_due(a, now=None):
    if a.get("closedAt") is None and not a.get("cancelled") and (now or time.time()) >= a["closes"]:
        a["closedAt"] = a["closes"]
        log.info("title: auction %d ended: %s", a["id"],
                 "sold to %s for %d" % (a["topBidderTag"], a["topBid"]) if a["topBidder"]
                 else "no bids")
        notify_end(a)
        return True
    return False


def notify_end(a):
    """Message Center: SOLD to the seller and WON to the winner, or FAILED to the seller."""
    car = {"AuctionId": a["id"], "CarId": a["car"].get("carId", 0)}
    if a["topBidder"]:
        notifications.send(int(a["seller"], 16), notifications.AUCTION_SOLD,
                           dict(car, SalePrice=a["topBid"]))
        notifications.send(int(a["topBidder"], 16), notifications.AUCTION_WON, car)
    else:
        notifications.send(int(a["seller"], 16), notifications.AUCTION_FAILED, car)


def next_bid(a):
    if not a["topBidder"]:
        return a["opening"]
    step = max(100, int(a["topBid"] * 0.05) // 100 * 100)
    return a["topBid"] + step


def view(a, viewer, now=None):
    """(status, allowedActions) of an auction for a player."""
    me = key(viewer)
    over = ended(a, now) or a.get("cancelled")
    held = a["held"].get(me, 0)
    if me == a["seller"]:
        if a.get("cancelled"):
            return CANCELLED, 0 if a.get("carTaken") else RESOLVE_FAILED | RESTART
        if not over:
            return ACTIVE, 0 if a["topBidder"] else CANCEL
        if a["topBidder"]:
            return SOLD, 0 if a.get("sellerPaid") else RECEIVE_SALE
        return EXPIRED, 0 if a.get("carTaken") else RESOLVE_FAILED | RESTART
    if not over:
        actions = BID | REPORT | (BUYOUT if a["buyout"] > a["topBid"] else 0)
        if a["topBidder"] == me:
            return HIGH_BIDDER, actions
        if me in a["held"] or me in a["bidders"]:
            return OUTBID, actions | (RECEIVE_REFUND if held else 0)
        return ACTIVE, actions
    if a["topBidder"] == me and not a.get("cancelled"):
        return WON, 0 if a.get("carTaken") else RESOLVE_WON
    if me in a["bidders"]:
        return LOST, RECEIVE_REFUND if held else 0
    return EXPIRED, 0


def user(xuid_hex, tag):
    if not xuid_hex:
        return {}
    return {"qwXuid": int(xuid_hex, 16), "wzGamerTag": tag or ""}


def base_record(a, viewer):
    status, actions = view(a, viewer)
    return {"id": a["id"], "topBidAmount": a["topBid"],
            "topBidder": user(a["topBidder"], a["topBidderTag"]),
            "nextBidAmount": next_bid(a), "numBids": len(a["bids"]), "status": status,
            "reportingState": 0, "closingDate": a["closes"], "allowedActions": actions}


def extended_record(a, viewer):
    car = a["car"]
    thumb = MARKET.textures.get(a.get("texturesId", -1), {}).get("thumbnail")
    return {"base": base_record(a, viewer), "carModelId": car.get("carId", 0),
            "buyoutAmount": a["buyout"], "carClass": car.get("carClass", 0),
            "carYear": car.get("year", 0), "performanceIndex": car.get("perfIndex", 0.0),
            "isUnicorn": car.get("isUnicorn", 0),
            "LiveryThumbnail": MARKET.read_blob(thumb, 230400),
            "liveryCreator": a.get("liveryCreatorXuid", 0)}


def all_auctions():
    MARKET.load()
    now = time.time()
    changed = False
    for a in MARKET.auctions.values():
        changed |= close_if_due(a, now)
    if changed:
        MARKET.save()
    return MARKET.auctions


def get_global_config(xuid, body):
    p, _ = x.decode(A.ForzaAuctionGetGlobalConfig_Params, body)
    return x.reply(A.ForzaAuctionGetGlobalConfig_Results, {
        "feeData": [{"hours": h, "credits": c} for h, c in FEES][:p["maxFeeResults"]],
        "searchPriceThresholds": PRICE_STEPS[:p["maxSearchPriceResults"]],
        "searchYearThresholds": YEAR_STEPS[:p["maxSearchYearResults"]],
        "searchPowerThresholds": POWER_STEPS[:p["maxSearchPowerResults"]],
        "refreshParams": {"auctionsRefreshSeconds": REFRESH_SECONDS},
        "defaultTime": DEFAULT_HOURS, "serverCurrentTime": time.time()})


def create(gamertag_of):
    def handler(xuid, body):
        p, _ = x.decode(A.ForzaAuctionCreate_Params, body)
        MARKET.load()
        if sum(1 for o in MARKET.auctions.values() if o["seller"] == key(xuid)
               and not o.get("removed") and not o.get("cancelled")
               and not o.get("closedAt")) >= limits.PER_PLAYER["auctions"]:
            raise ValueError("too many open auctions")
        aid = MARKET.new_id()
        now = time.time()
        duration = max(60.0, p["durationMs"] / 1000.0)
        a = {"id": aid, "seller": key(xuid), "sellerTag": gamertag_of(xuid),
             "opening": p["openingPrice"], "buyout": p["buyoutPrice"], "fee": p["listingFeePaid"],
             "created": now, "closes": now + duration, "car": p["car"],
             "carHistory": p["carHistory"], "texturesId": p["texturesId"],
             "liveryCreatorXuid": p["liveryCreatorXuid"], "wzItemGuid": p["wzItemGuid"],
             "unencryptedSize": p["unencryptedSize"], "wasLiveryLocked": p["wasLiveryLocked"],
             "bids": [], "bidders": [], "held": {}, "topBid": 0, "topBidder": "",
             "topBidderTag": "", "closedAt": None}
        for name in BLOBS:
            a[name] = MARKET.write_blob("auction%d_%s.bin" % (aid, name), p[name])
        MARKET.auctions[aid] = a
        MARKET.save()
        log.info("title: auction %d by %016X '%s': car %d (%d) class %d PI %.0f, opening %d, buyout "
                 "%d, %.1f h, fee %d", aid, xuid, a["sellerTag"], p["car"]["carId"],
                 p["car"]["year"], p["car"]["carClass"], p["car"]["perfIndexFriendly"],
                 a["opening"], a["buyout"], duration / 3600, a["fee"])
        return x.reply(A.ForzaAuctionCreate_Results, {"auctionId": aid})
    return handler


def upload_textures(xuid, body):
    p, _ = x.decode(A.ForzaAuctionUploadAuctionTextures_Params, body)
    tid = MARKET.new_id()
    MARKET.textures[tid] = {
        "texture": MARKET.write_blob("textures%d_livery.bin" % tid, p["liveryTexture"]),
        "thumbnail": MARKET.write_blob("textures%d_thumb.bin" % tid, p["LiveryThumbnail"])}
    MARKET.save()
    log.info("title: auction textures %d from %016X (%d + %d bytes)", tid, xuid,
             len(p["liveryTexture"]), len(p["LiveryThumbnail"]))
    return x.reply(A.ForzaAuctionUploadAuctionTextures_Results, {"texturesId": tid})


def matches(a, owner, sp):
    car = a["car"]

    def given(v, none=(0, -1, 0xFFFF, 0xFF)):
        return v not in none

    if owner and a["seller"] != key(owner):
        return False
    price = max(a["opening"], a["topBid"])
    if given(sp["PriceMin"]) and price < sp["PriceMin"]:
        return False
    if given(sp["PriceMax"]) and sp["PriceMax"] > 0 and price > sp["PriceMax"]:
        return False
    if given(sp["MakeId"]) and car.get("make") != sp["MakeId"]:
        return False
    if given(sp["CarId"]) and car.get("carId") != sp["CarId"]:
        return False
    if given(sp["CountryId"]) and car.get("countryId") != sp["CountryId"]:
        return False
    if given(sp["CarClass"]) and car.get("carClass") != sp["CarClass"]:
        return False
    if given(sp["YearMin"]) and car.get("year", 0) < sp["YearMin"]:
        return False
    if given(sp["YearMax"]) and car.get("year", 0) > sp["YearMax"]:
        return False
    if sp["isUnicorn"] and not car.get("isUnicorn"):
        return False
    return True


def search(xuid, body):
    p, _ = x.decode(A.ForzaAuctionSearchForAuction_Params, body)
    sp = p["searchParams"]
    found = [a for a in all_auctions().values()
             if not ended(a) and not a.get("cancelled") and
             (p["xuidOwner"] or a["seller"] != key(xuid)) and matches(a, p["xuidOwner"], sp)]
    found.sort(key=lambda a: a["closes"])
    ids = [a["id"] for a in found][:p["maxResultIds"]]
    log.info("title: auction search by %016X (owner %016X, %s): %d", xuid, p["xuidOwner"],
             {k: v for k, v in sp.items() if v}, len(found))
    return x.reply(A.ForzaAuctionSearchForAuction_Results, {
        "auctionIds": ids, "totalAvailable": len(found),
        "auctionResults": [extended_record(a, xuid) for a in found[:p["maxResults"]]]})


def get_auctions(xuid, body):
    p, _ = x.decode(A.ForzaAuctionGetAuctions_Params, body)
    auctions = all_auctions()
    base = [base_record(auctions[i], xuid) for i in p["auctionIds"] if i in auctions]
    ext = [extended_record(auctions[i], xuid) for i in p["extendedAuctionIds"] if i in auctions]
    return x.reply(A.ForzaAuctionGetAuctions_Results, {
        "auctions": base[:p["maxAuctionResults"]],
        "extendedAuctions": ext[:p["maxExtendedAuctionResults"]]})


def get_details(xuid, body):
    p, _ = x.decode(A.ForzaAuctionGetDetails_Params, body)
    a = all_auctions().get(p["auctionId"])
    if not a:
        return x.failure(1, "no such auction")
    tex = MARKET.textures.get(a.get("texturesId", -1), {}).get("texture")
    return x.reply(A.ForzaAuctionGetDetails_Results, {"auctionDetails": {
        "seller": user(a["seller"], a["sellerTag"]), "liveryData": MARKET.read_blob(tex, 256000),
        "car": a["car"], "partsInstalled": MARKET.read_blob(a["partsInstalled"], 2048),
        "partsInTrunk": MARKET.read_blob(a["partsInTrunk"], 10240),
        "tuning": MARKET.read_blob(a["tuning"], 1024)}})


def mine(xuid, sold_by_me):
    me = key(xuid)
    out = [a for a in all_auctions().values()
           if (a["seller"] == me) == sold_by_me and (sold_by_me or me in a["bidders"])
           and not a.get("removed")]
    out.sort(key=lambda a: -a["created"])
    return out


def get_my_auctions(xuid, body):
    p, _ = x.decode(A.ForzaAuctionGetMyAuctions_Params, body)
    found = mine(xuid, True)
    log.info("title: my auctions %016X: %d", xuid, len(found))
    return x.reply(A.ForzaAuctionGetMyAuctions_Results, {
        "auctionIds": [a["id"] for a in found][:p["maxResultIds"]],
        "totalAvailable": len(found),
        "auctionResults": [extended_record(a, xuid) for a in found[:p["maxResults"]]],
        "auctionUpdateResults": [base_record(a, xuid) for a in found[:p["maxUpdateResults"]]]})


def get_my_bids(xuid, body):
    p, _ = x.decode(A.ForzaAuctionGetMyBids_Params, body)
    found = [a for a in mine(xuid, False) if not p["unicornsOnly"] or a["car"].get("isUnicorn")]
    log.info("title: my bids %016X: %d", xuid, len(found))
    return x.reply(A.ForzaAuctionGetMyBids_Results, {
        "auctionIds": [a["id"] for a in found][:p["maxResultIds"]],
        "totalAvailable": len(found),
        "auctionResults": [extended_record(a, xuid) for a in found[:p["maxResults"]]]})


def place_bid(gamertag_of):
    def handler(xuid, body):
        p, _ = x.decode(A.ForzaAuctionPlaceBid_Params, body)
        a = all_auctions().get(p["auctionId"])
        me = key(xuid)
        amount = p["amount"]
        if not a or a["seller"] == me:
            return x.failure(1, "no such auction")
        if ended(a) or a.get("cancelled"):
            log.info("title: bid %d on auction %d by %016X: closed", amount, a["id"], xuid)
            return x.failure(3, "auction closed")
        if amount < next_bid(a) and not (a["buyout"] and amount >= a["buyout"]):
            log.info("title: bid %d on auction %d by %016X: below %d", amount, a["id"], xuid,
                     next_bid(a))
            return x.failure(2, "outbid")
        # The bidder's earlier bid on this auction comes back now (the game takes the new
        # one in full); the previous top bidder's is owed until they collect it.
        refunded = a["held"].pop(me, 0)
        a["held"][me] = amount
        if a["topBidder"] and a["topBidder"] != me:
            notifications.send(int(a["topBidder"], 16), notifications.AUCTION_OUTBID,
                               {"AuctionId": a["id"], "CarId": a["car"].get("carId", 0)})
        a["bids"].append({"xuid": me, "amount": amount, "time": time.time()})
        if me not in a["bidders"]:
            a["bidders"].append(me)
        a["topBid"], a["topBidder"], a["topBidderTag"] = amount, me, gamertag_of(xuid)
        if a["buyout"] and amount >= a["buyout"]:
            a["closedAt"] = time.time()
            log.info("title: auction %d bought out by %016X for %d", a["id"], xuid, amount)
            notify_end(a)
        else:
            log.info("title: bid %d on auction %d by %016X (refunded %d)", amount, a["id"], xuid,
                     refunded)
        MARKET.save()
        return x.reply(A.ForzaAuctionPlaceBid_Results, {"amountRefunded": refunded})
    return handler


def retrieve_car(xuid, body):
    p, _ = x.decode(A.ForzaAuctionRetrieveCarForAuction_Params, body)
    a = all_auctions().get(p["auctionId"])
    if not a:
        return x.failure(1, "no such auction")
    status, actions = view(a, xuid)
    if not actions & (RESOLVE_WON | RESOLVE_FAILED):
        log.info("title: auction %d car for %016X: not theirs (status %d)", a["id"], xuid, status)
        return x.failure(1, "not yours")
    a["carTaken"] = key(xuid)
    history = dict(a["carHistory"])
    base = dict(history.get("base", {}))
    if status == WON:
        base["numOwners"] = base.get("numOwners", 0) + 1
        base["numTimesSold"] = base.get("numTimesSold", 0) + 1
        base["curOwnerNumRaces"] = base["curOwnerWinnings"] = 0
        base["curOwnerPurchaseDate"] = int(time.time())
    history["base"] = base
    car = dict(a["car"], wasOriginallyFromAuctionHouse=1) if status == WON else a["car"]
    MARKET.save()
    log.info("title: auction %d car to %016X (%s)", a["id"], xuid,
             "won" if status == WON else "returned")
    guid = b""
    try:
        guid = uuid.UUID(a.get("wzItemGuid", "")).bytes
    except ValueError:
        pass
    return x.reply(A.ForzaAuctionRetrieveCarForAuction_Results, {
        "car": car, "carHistory": history, "isOwner": status != WON,
        "partsInstalled": MARKET.read_blob(a["partsInstalled"], p["cbPartsInstalledBufferSize"]),
        "partsInTrunk": MARKET.read_blob(a["partsInTrunk"], p["cbPartsInTrunkBufferSize"]),
        "tuning": MARKET.read_blob(a["tuning"], p["cbTuningBufferSize"]),
        "liveryDetailsEncrypted": MARKET.read_blob(a["liveryDetails"],
                                                   p["cbLiveryDetailsBufferSize"]),
        "liveryGuid": guid, "unencryptedSize": a.get("unencryptedSize", 0)})


def payment_sold(xuid, body):
    p, _ = x.decode(A.ForzaAuctionRetrievePaymentForAuctionSold_Params, body)
    a = all_auctions().get(p["auctionId"])
    if not a or view(a, xuid)[1] & RECEIVE_SALE == 0:
        return x.failure(1, "nothing to collect")
    a["sellerPaid"] = True
    MARKET.save()
    log.info("title: auction %d: seller %016X collects %d", a["id"], xuid, a["topBid"])
    return x.reply(A.ForzaAuctionRetrievePaymentForAuctionSold_Results, {"bidAmount": a["topBid"]})


def payment_lost(xuid, body):
    p, _ = x.decode(A.ForzaAuctionRetrievePaymentsForAuctionLost_Params, body)
    a = all_auctions().get(p["auctionId"])
    if not a or view(a, xuid)[1] & RECEIVE_REFUND == 0:
        return x.failure(1, "nothing to collect")
    amount = a["held"].pop(key(xuid), 0)
    MARKET.save()
    log.info("title: auction %d: %016X gets %d back", a["id"], xuid, amount)
    return x.reply(A.ForzaAuctionRetrievePaymentsForAuctionLost_Results, {"bidAmount": amount})


def cancel(xuid, body):
    p, _ = x.decode(A.ForzaAuctionCancel_Params, body)
    a = all_auctions().get(p["auctionId"])
    if not a or view(a, xuid)[1] & CANCEL == 0:
        return x.failure(1, "cannot cancel")
    a["cancelled"] = True
    MARKET.save()
    log.info("title: auction %d cancelled", a["id"])
    return x.reply()


def restart(xuid, body):
    p, _ = x.decode(A.ForzaAuctionRestart_Params, body)
    a = all_auctions().get(p["auctionId"])
    if not a or view(a, xuid)[1] & RESTART == 0:
        return x.failure(1, "cannot restart")
    aid = MARKET.new_id()
    now = time.time()
    b = dict(a, id=aid, opening=p["openingPrice"], buyout=p["buyoutPrice"], fee=p["fee"],
             created=now, closes=now + max(60.0, p["durationMs"] / 1000.0), bids=[],
             bidders=[], held={}, topBid=0, topBidder="", topBidderTag="", closedAt=None,
             cancelled=False, carTaken=None, sellerPaid=False)
    a["carTaken"] = "restarted as %d" % aid
    a["removed"] = True
    MARKET.auctions[aid] = b
    MARKET.save()
    log.info("title: auction %d restarted as %d", a["id"], aid)
    return x.reply(A.ForzaAuctionRestart_Results, {"auctionIdNew": aid})


def remove(xuid, body):
    p, _ = x.decode(A.ForzaAuctionRemoveMyBannedAuction_Params, body)
    a = all_auctions().get(p["auctionId"])
    if a and a["seller"] == key(xuid):
        a["removed"] = True
        MARKET.save()
    return x.reply()


# ---- Car gifts ----

def send_car_gift(gamertag_of):
    def handler(xuid, body):
        p, _ = x.decode(A.ForzaGiftingSendCarGift_Params, body)
        MARKET.load()
        if sum(1 for o in MARKET.gifts.values() if o["sender"] == key(xuid)
               and not o.get("retrieved")) >= limits.PER_PLAYER["gifts"]:
            raise ValueError("too many unretrieved gifts")
        gid = MARKET.new_id()
        g = {"id": gid, "type": "car", "sender": key(xuid), "senderTag": gamertag_of(xuid),
             "recipient": key(p["recipientXuid"]), "sent": time.time(), "car": p["car"],
             "carHistory": p["carHistory"], "wzItemGuid": p["wzItemGuid"],
             "unencryptedSize": p["unencryptedSize"], "retrieved": False}
        for name in BLOBS:
            g[name] = MARKET.write_blob("gift%d_%s.bin" % (gid, name), p[name])
        MARKET.gifts[gid] = g
        MARKET.save()
        log.info("title: gift %d: car %d from %016X to %016X", gid, p["car"]["carId"], xuid,
                 p["recipientXuid"])
        # The recipient collects it from the Message Center (RetrieveCarGift).
        notifications.send(p["recipientXuid"], notifications.GIFT_CAR, {
            "GiftId": gid, "MakeId": p["car"]["make"], "ModelId": p["car"]["carId"],
            "SenderXuid": xuid, "SenderGamerTag": g["senderTag"], "Reason": 0})
        return x.reply(A.ForzaGiftingSendCarGift_Results, {"giftId": gid})
    return handler


def gift_listing(g):
    return {"giftId": g["id"], "senderXuid": int(g["sender"], 16), "reason": 0,
            "giftType": 0 if g["type"] == "car" else 1}


def retrieve_car_gift(xuid, body):
    p, _ = x.decode(A.ForzaGiftingRetrieveCarGift_Params, body)
    MARKET.load()
    g = MARKET.gifts.get(p["giftId"])
    if not g or g["recipient"] != key(xuid) or g["retrieved"] or g["type"] != "car":
        return x.failure(1, "no such gift")
    g["retrieved"] = True
    MARKET.save()
    log.info("title: gift %d: car to %016X", g["id"], xuid)
    return x.reply(A.ForzaGiftingRetrieveCarGift_Results, {
        "carHistory": g["carHistory"], "car": dict(g["car"], wasOriginallyGifted=1),
        "giftListing": gift_listing(g),
        "liveryDetails": MARKET.read_blob(g["liveryDetails"], p["cbLiveryDetailsBufferSize"]),
        "partsInstalled": MARKET.read_blob(g["partsInstalled"], p["cbPartsInstalled"]),
        "partsInTrunk": MARKET.read_blob(g["partsInTrunk"], p["cbPartsInTrunk"]),
        "tuning": MARKET.read_blob(g["tuning"], p["cbTuning"])})


def retrieve_credits_gift(xuid, body):
    p, _ = x.decode(A.ForzaGiftingRetrieveCreditsGift_Params, body)
    MARKET.load()
    g = MARKET.gifts.get(p["giftId"])
    if not g or g["recipient"] != key(xuid) or g["retrieved"] or g["type"] != "credits":
        return x.failure(1, "no such gift")
    g["retrieved"] = True
    MARKET.save()
    return x.reply(A.ForzaGiftingRetrieveCreditsGift_Results, {"creditAmount": g["credits"]})


# ---- Gifts from the server's operator (GET /gift on the status port, from this machine) ----

def gift_credits(recipient, amount, message="", sender=0, sender_tag="Forza Live"):
    """Credits for a player, collected from the Message Center (RetrieveCreditsGift)."""
    gid = MARKET.new_id()
    MARKET.gifts[gid] = {"id": gid, "type": "credits", "sender": key(sender),
                         "senderTag": sender_tag, "recipient": key(recipient),
                         "sent": time.time(), "credits": int(amount), "retrieved": False}
    MARKET.save()
    log.info("title: gift %d: %d credits to %016X", gid, amount, recipient)
    notifications.send(recipient, notifications.GIFT_CREDITS, {
        "GiftId": gid, "Amount": int(amount), "UserSuppliedReason": message,
        "SenderXuid": sender, "SenderGamerTag": sender_tag})
    return gid


def gift_stock_car(recipient, model, sender=0, sender_tag="Forza Live", idempotency_key=None):
    """A stock car (the game database's Data_Car id), collected from the Message Center
    (RetrieveCarGift): no parts, tuning or livery, a new car's history."""
    import forza_clubs
    MARKET.load()
    existing = next((g for g in MARKET.gifts.values()
                     if idempotency_key is not None and g.get("deliveryKey") == idempotency_key), None)
    if existing:
        if existing["recipient"] != key(recipient) or existing["car"]["carId"] != model:
            raise ValueError("delivery key reused for a different reward")
        gid = existing["id"]
        MARKET.save()  # a previous save may have failed after adding the in-memory row
        # Also repairs a crash between saving the gift and saving its notification.
        notifications.send(recipient, notifications.GIFT_CAR, {
            "GiftId": gid, "MakeId": existing["car"]["make"], "ModelId": model,
            "SenderXuid": sender, "SenderGamerTag": sender_tag, "Reason": 0},
            idempotency_key=idempotency_key)
        return gid
    info = forza_clubs.game_car(model)
    if not info:
        raise ValueError("no car %d in the game database" % model)
    year, make = info[0], info[1]
    gid = MARKET.new_id()
    MARKET.gifts[gid] = {
        "id": gid, "type": "car", "sender": key(sender), "senderTag": sender_tag,
        "recipient": key(recipient), "sent": time.time(),
        "car": {"make": make, "carId": model, "year": year},
        "carHistory": {"base": {"numOwners": 1, "originalOwner": sender_tag[:15],
                                "curOwnerPurchaseDate": int(time.time())}},
        "wzItemGuid": "", "unencryptedSize": 0, "retrieved": False,
        **{name: "" for name in BLOBS}}
    if idempotency_key is not None:
        MARKET.gifts[gid]["deliveryKey"] = idempotency_key
    MARKET.save()
    log.info("title: gift %d: car %d (make %d) to %016X", gid, model, make, recipient)
    notifications.send(recipient, notifications.GIFT_CAR, {
        "GiftId": gid, "MakeId": make, "ModelId": model, "SenderXuid": sender,
        "SenderGamerTag": sender_tag, "Reason": 0}, idempotency_key=idempotency_key)
    return gid


def pending_gifts(xuid):
    MARKET.load()
    return [g for g in MARKET.gifts.values() if g["recipient"] == key(xuid) and not g["retrieved"]]


def services(gamertag_of):
    return {
        ("Auction", "GetGlobalConfig"): get_global_config,
        ("Auction", "Create"): create(gamertag_of),
        ("Auction", "UploadAuctionTextures"): upload_textures,
        ("Auction", "SearchForAuction"): search,
        ("Auction", "GetAuctions"): get_auctions,
        ("Auction", "GetDetails"): get_details,
        ("Auction", "GetMyAuctions"): get_my_auctions,
        ("Auction", "GetMyBids"): get_my_bids,
        ("Auction", "PlaceBid"): place_bid(gamertag_of),
        ("Auction", "RetrieveCarForAuction"): retrieve_car,
        ("Auction", "RetrievePaymentForAuctionSold"): payment_sold,
        ("Auction", "RetrievePaymentsForAuctionLost"): payment_lost,
        ("Auction", "Cancel"): cancel,
        ("Auction", "Restart"): restart,
        ("Auction", "RemoveMyBannedAuction"): remove,
        ("Auction", "ReportContent"): lambda xuid, body: x.reply(),
        ("Gifting", "SendCarGift"): send_car_gift(gamertag_of),
        ("Gifting", "RetrieveCarGift"): retrieve_car_gift,
        ("Gifting", "RetrieveCreditsGift"): retrieve_credits_gift,
        ("Gifting", "MarkFileGiftRetrieved"): lambda xuid, body: x.reply(),
    }
