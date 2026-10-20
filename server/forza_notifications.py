"""Forza Live: Message Center notifications (Xls.WebServices.Notifications).

The game polls RetrieveForUser every nextRetrieveSeconds once its notification session has
started (StartNewSession) and shows what it gets in the Message Center. Each notification
is a record {id, xuidRecipient, notificationType, hasThumbnail, isRead, deleteAction,
expirationDate, cbParameters}; its parameters follow in the reply's parameterBlock, one
after the other, in the same xtsw encoding as service records (cbParameters bytes each).

The type picks the record its parameters decode to and the Message Center entry the game
makes of it (the dispatch in sub_82ABD248; the parameter records' member lists are filled
by static initializers, docs/title_services.md). The high byte is the category (bit 15
set for those that carry something to collect):

    0x0100 news message        0x0101 new DLC            0x0201 banned
    0x8300 auction outbid      0x8301 auction sold       0x8302 auction won
    0x8303 auction failed      0x8304 gift: credits      0x8305 gift: item
    0x8306 gift: car           0x8307 storefront out of stock
    0x8308 storefront money    0x8402 Rivals recommendation
    0x8403 Rivals: your time was beaten
    0x0500 club: request to join (to leaders)    0x0501 club: invitation
    0x0502 club: kick recommendation             0x0503 club: kicked
    0x0504 club: demotion      0x0505 club: membership accepted   0x0506 club: member quit
    0x0507 club: new member    0x0508 club: promotion             0x8501 club: shared car

DATA_DIR/notifications.json keeps them per recipient until the game clears them
(Notifications.Clear) or they expire.
"""

import json
import logging
import os
import threading
import time

import forza_xtsw as x
from forza_xtsw import BOOL, F32, I32, U8, U16, U32, U64, S, SYSTIME, WSTR

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"
POLL_SECONDS = 60       # nextRetrieveSeconds (0 made the game poll 7 times a second)
FIRST_POLL_SECONDS = 15  # Logon2's notificationPollIntervalSeconds: the first poll
KEEP_DAYS = 30          # expirationDate of a new notification
UNIX_TO_FILETIME = 116444736000000000

# The Message Center's tabs (Logon2's notificationCategories, at most 5): a notification's
# category is its type's high byte (without bit 15). The game needs a tab for the category
# of every notification it shows (without one, opening the Message Center crashed).
CATEGORIES = [
    {"type": 1, "name": "News", "description": "News from the Horizon Festival."},
    {"type": 2, "name": "Account", "description": "Your account."},
    {"type": 3, "name": "Gifts & Auctions", "description": "Gifts, auctions and storefront."},
    {"type": 4, "name": "Rivals", "description": "Your Rivals."},
    {"type": 5, "name": "Car Club", "description": "Your car club."},
]

Message = S(("Message", WSTR(513)))
ClubNames = (("clanName", WSTR(21)), ("clanTag", WSTR(5)))
AuctionCar = (("AuctionId", I32), ("CarId", I32))

# type: (name, parameter record)
TYPES = {
    0x0000: ("permissions changed", S(("Flags", U64))),
    0x8000: ("badge granted", S(("BadgeId", I32))),
    0x8001: ("set credits", S(("creditAmount", U32), ("action", I32))),
    0x0100: ("news", Message),
    0x0101: ("new DLC", S(("Message", WSTR(513)), ("OfferId", U64))),
    0x0102: ("message", Message),
    0x0200: ("message", Message),
    0x0201: ("banned", S(("BanExpireTime", SYSTIME), ("BannedFromFeatureAreas", U64))),
    0x0202: ("unbanned", S(("BannedFromFeatureAreas", U64))),
    0x0300: ("message", Message),
    0x8300: ("auction outbid", S(*AuctionCar)),
    0x8301: ("auction sold", S(*AuctionCar, ("SalePrice", U32))),
    0x8302: ("auction won", S(*AuctionCar)),
    0x8303: ("auction failed", S(*AuctionCar)),
    0x8304: ("gift: credits", S(("GiftId", I32), ("Amount", I32),
                                ("UserSuppliedReason", WSTR(513)), ("SenderXuid", U64),
                                ("SenderGamerTag", WSTR(16)))),
    0x8305: ("gift: item", S(("GiftId", I32), ("ItemId", I32), ("GiftFileType", I32),
                             ("SenderXuid", U64), ("SenderGamerTag", WSTR(16)),
                             ("Reason", I32))),
    0x8306: ("gift: car", S(("GiftId", I32), ("MakeId", I32), ("ModelId", I32),
                            ("SenderXuid", U64), ("SenderGamerTag", WSTR(16)),
                            ("Reason", I32))),
    0x8307: ("storefront: out of stock", S(("StorefrontFileId", I32),
                                           ("StorefrontFileType", I32),
                                           ("StorefrontFileTitle", WSTR(17)))),
    0x8308: ("storefront: money", S(("Amount", U32))),
    0x8309: ("profile import", S(("awardAmount", U32))),
    0x8401: ("message", Message),
    0x8402: ("Rivals recommendation", S(("TimeTrialId", I32),
                                        ("localizedTimeTrialName", WSTR(129)),
                                        ("trackName", WSTR(65)),
                                        ("description", WSTR(513)), ("category", I32))),
    0x8403: ("Rivals: time beaten", S(("senderXuid", U64), ("senderGamertag", WSTR(16)),
                                      ("senderClanTag", WSTR(5)), ("senderClanRole", I32),
                                      ("senderFlags", U8), ("sentTime", SYSTIME),
                                      ("timeTrialId", I32), ("carId", U16),
                                      ("localizedTimeTrialName", WSTR(129)),
                                      ("trackName", WSTR(65)), ("trackId", U16),
                                      ("score", F32), ("scoreType", I32), ("category", I32))),
    0x0500: ("club: join request", S(("requestorXuid", U64), ("requestorGamertag", WSTR(16)),
                                     ("clanId", I32))),
    0x0501: ("club: invitation", S(("sponsorXuid", U64), ("clanId", I32), *ClubNames,
                                   ("sponsorGamertag", WSTR(16)))),
    0x0502: ("club: kick recommendation", S(
        ("recommendingRemovalXuid", U64), ("recommendingRemovalGamertag", WSTR(16)),
        ("recommendedForRemovalXuid", U64), ("recommendedForRemovalGamertag", WSTR(16)),
        ("clanId", I32))),
    0x0503: ("club: kicked", S(("kickedXuid", U64), ("kickedGamertag", WSTR(16)), *ClubNames)),
    0x0504: ("club: demotion", S(*ClubNames)),
    0x0505: ("club: membership accepted", S(*ClubNames)),
    0x0506: ("club: member quit", S(("exMemberXuid", U64), ("exMemberGamertag", WSTR(16)),
                                    *ClubNames)),
    0x0507: ("club: new member", S(("newMemberXuid", U64), ("newMemberGamertag", WSTR(16)),
                                   *ClubNames)),
    0x0508: ("club: promotion", S(("newRole", I32), ("newPromotedXuid", U64),
                                  ("newPromotedGamertag", WSTR(16)), *ClubNames)),
    0x8501: ("club: shared car", S(("sharedCarAction", I32), ("playerXuid", U64),
                                   ("playerGamertag", WSTR(16)), ("MakeName", WSTR(31)),
                                   ("ModelName", WSTR(81)), ("perfIndex", F32),
                                   ("Year", U16))),
}
NEWS, DLC, BANNED = 0x0100, 0x0101, 0x0201
AUCTION_OUTBID, AUCTION_SOLD, AUCTION_WON, AUCTION_FAILED = 0x8300, 0x8301, 0x8302, 0x8303
GIFT_CREDITS, GIFT_ITEM, GIFT_CAR = 0x8304, 0x8305, 0x8306
RIVALS_RECOMMENDATION, RIVALS_BEATEN = 0x8402, 0x8403
CLUB_REQUEST, CLUB_INVITE, CLUB_KICK_RECOMMENDATION, CLUB_KICKED, CLUB_DEMOTION, \
    CLUB_ACCEPTED, CLUB_MEMBER_QUIT, CLUB_NEW_MEMBER, CLUB_PROMOTION = range(0x500, 0x509)
CLUB_SHARED_CAR = 0x8501


class Store:
    def __init__(self):
        self.data = {"nextId": 1, "byRecipient": {}}
        self.loaded = False
        self.lock = threading.Lock()

    def path(self):
        return os.path.join(DATA_DIR, "notifications.json")

    def load(self):
        if not self.loaded:
            self.loaded = True
            try:
                with open(self.path()) as f:
                    self.data.update(json.load(f))
            except (OSError, ValueError):
                pass
        return self.data

    def save(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        tmp = self.path() + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=1)
        os.replace(tmp, self.path())


STORE = Store()


def configure(data_dir):
    global DATA_DIR
    DATA_DIR = data_dir
    STORE.__init__()


def key(xuid):
    return "%016X" % xuid


def send(recipient, ntype, params, keep_days=KEEP_DAYS, idempotency_key=None):
    """Queues a notification of type ntype (TYPES) for recipient; returns its id."""
    if ntype not in TYPES:
        raise ValueError("unknown notification type %#x" % ntype)
    x.encode(TYPES[ntype][1], params)  # fails now rather than in a reply
    with STORE.lock:
        d = STORE.load()
        # Keep receipts even after the notification is cleared or expires.
        receipts = d.setdefault("receipts", {})
        if idempotency_key is not None and idempotency_key in receipts:
            STORE.save()  # retry a previous failed persistence before acknowledging it
            return receipts[idempotency_key]
        nid = d["nextId"]
        d["nextId"] += 1
        d["byRecipient"].setdefault(key(recipient), []).append({
            "id": nid, "type": ntype, "params": params, "sent": time.time(),
            "expires": time.time() + keep_days * 86400, "read": False})
        if idempotency_key is not None:
            receipts[idempotency_key] = nid
        STORE.save()
    log.info("title: notification %d to %016X: %s %s", nid, recipient, TYPES[ntype][0],
             {k: v for k, v in params.items() if not k.endswith("Xuid")})
    return nid


def pending(recipient):
    now = time.time()
    return [n for n in STORE.load()["byRecipient"].get(key(recipient), [])
            if n["expires"] > now]


def retrieve(xuid, body):
    p, _ = x.decode(x.NotificationsRetrieveForUser_Params, body)
    results, block = [], bytearray()
    with STORE.lock:
        for n in pending(xuid)[:p["maxResults"]]:
            params = x.encode(TYPES[n["type"]][1], n["params"])
            if len(block) + len(params) > p["cbParameterBlockSize"]:
                break
            block += params
            results.append({
                "id": n["id"], "xuidRecipient": xuid,
                "notificationType": n["type"] - 0x10000 if n["type"] & 0x8000 else n["type"],
                "isRead": n["read"],
                "expirationDate": int(n["expires"] * 10 ** 7) + UNIX_TO_FILETIME,
                "cbParameters": len(params)})
    if results:
        log.info("title: notifications for %016X: %s", xuid,
                 ", ".join("%d %#x" % (r["id"], r["notificationType"] & 0xFFFF) for r in results))
    return x.reply(x.NotificationsRetrieveForUser_Results, {
        "results": results, "parameterBlock": bytes(block), "nextRetrieveSeconds": POLL_SECONDS})


def update(xuid, body, clear):
    p, _ = x.decode(x.NotificationsId_Params, body)
    with STORE.lock:
        mine = STORE.load()["byRecipient"].get(key(xuid), [])
        for n in mine:
            if n["id"] == p["notificationId"]:
                if clear:
                    mine.remove(n)
                else:
                    n["read"] = True
                STORE.save()
                break
    log.info("title: notification %d %s by %016X", p["notificationId"],
             "cleared" if clear else "read", xuid)
    return x.reply()


def services():
    return {
        # A new session (the game starts one when a race loads) shows nothing until its
        # first poll: make that one come soon.
        ("Xls.Notifications", "StartNewSession"):
            lambda xuid, body: x.reply(x.NotificationsStartNewSession_Results,
                                       {"nonce": 1, "nextRetrieveSeconds": FIRST_POLL_SECONDS}),
        ("Xls.Notifications", "RetrieveForUser"): retrieve,
        ("Xls.Notifications", "MarkRead"): lambda xuid, body: update(xuid, body, False),
        ("Xls.Notifications", "Clear"): lambda xuid, body: update(xuid, body, True),
    }


# Samples for --test-notifications (from the sender, to the recipient). Not the gifts,
# which need a gift on the server (forza_market.gift_credits, gift_stock_car), nor the
# auction ones: the Message Center lists those without a title or text.
def samples(sender, sender_tag):
    club = {"clanName": "Horizon Kings", "clanTag": "HZN"}
    return {
        NEWS: {"Message": "Welcome back to the Horizon Festival!"},
        DLC: {"Message": "New cars are in the Autoshow.", "OfferId": 0},
        RIVALS_RECOMMENDATION: {"TimeTrialId": 1001, "localizedTimeTrialName": "Clear Springs",
                                "trackName": "Clear Springs Circuit",
                                "description": "Try this one.", "category": 0},
        RIVALS_BEATEN: {"senderXuid": sender, "senderGamertag": sender_tag,
                        "sentTime": time.time(), "timeTrialId": 199, "carId": 1398,
                        "localizedTimeTrialName": "GoPro Millennia Masters",
                        "trackName": "Millennia", "trackId": 0, "score": 93.294,
                        "scoreType": 3, "category": 0},
        CLUB_REQUEST: {"requestorXuid": sender, "requestorGamertag": sender_tag, "clanId": 1},
        CLUB_INVITE: dict(club, sponsorXuid=sender, sponsorGamertag=sender_tag, clanId=1),
        CLUB_ACCEPTED: club,
        CLUB_NEW_MEMBER: dict(club, newMemberXuid=sender, newMemberGamertag=sender_tag),
        CLUB_PROMOTION: dict(club, newRole=2, newPromotedXuid=sender,
                             newPromotedGamertag=sender_tag),
    }
