"""Organizer-approved community events and unicorn entitlements.

SQLite owns awards and a durable delivery outbox. Gameplay membership is evidence of
attendance, not proof of race completion. Identity is explicitly enrolled by an
organizer against the existing friends credential; it is not an arbitrary XUID header.
"""
import json
import re
import sqlite3
import struct
import time
import uuid

import forza_market as market
import forza_notifications as notifications

KIND, FEATURE = 24, 0x08
MODELS = (320, 326, 330, 1082, 1197, 1233, 1488)
NAMES = dict(zip(MODELS, ("Honda CR-X SiR", "Ferrari Dino 246 GT", "Lotus Elan Sprint",
                         "Ford Fiesta Zetec S", "Saleen S5S Raptor", "Mazda MX-5 Superlight",
                         "MINI John Cooper Works Coupe")))


def models(value):
    if not isinstance(value, list) or not value or len(value) > 7:
        raise ValueError("rewards must contain one to seven unicorn model IDs")
    if any(type(x) is not int or x not in MODELS for x in value):
        raise ValueError("unknown unicorn model")
    if len(set(value)) != len(value):
        raise ValueError("duplicate reward model")
    return value


def hx(xuid):
    if not 0 < xuid < 1 << 64:
        raise ValueError("invalid player XUID")
    return "%016X" % xuid


class Events:
    def __init__(self, server, path=None, required=False, clock=time.time):
        self.server, self.required, self.clock = server, required, clock
        self.db = sqlite3.connect(path or ":memory:")
        self.db.row_factory = sqlite3.Row
        self.db.execute("PRAGMA foreign_keys=ON")
        self.db.executescript("""
          CREATE TABLE IF NOT EXISTS meta (id INTEGER PRIMARY KEY CHECK(id=1), revision INTEGER);
          INSERT OR IGNORE INTO meta VALUES(1,1);
          CREATE TABLE IF NOT EXISTS identities (xuid TEXT PRIMARY KEY, credential TEXT NOT NULL,
            enrolled REAL NOT NULL);
          CREATE TABLE IF NOT EXISTS events (id TEXT PRIMARY KEY, name TEXT NOT NULL,
            description TEXT NOT NULL, activity TEXT NOT NULL, session TEXT NOT NULL,
            starts REAL NOT NULL, ends REAL NOT NULL, rewards TEXT NOT NULL,
            state TEXT NOT NULL, organizer TEXT NOT NULL, created REAL NOT NULL);
          CREATE TABLE IF NOT EXISTS attendance (event TEXT REFERENCES events(id), xuid TEXT,
            credential TEXT NOT NULL, first REAL NOT NULL, last REAL NOT NULL,
            seconds REAL NOT NULL DEFAULT 0, PRIMARY KEY(event,xuid));
          CREATE TABLE IF NOT EXISTS grants (event TEXT REFERENCES events(id), xuid TEXT,
            model INTEGER, credential TEXT NOT NULL, id TEXT UNIQUE NOT NULL,
            organizer TEXT NOT NULL, granted REAL NOT NULL, revoked REAL,
            PRIMARY KEY(event,xuid,model));
          CREATE TABLE IF NOT EXISTS deliveries (xuid TEXT, model INTEGER,
            state TEXT NOT NULL, delivery_key TEXT UNIQUE NOT NULL, gift INTEGER,
            error TEXT, PRIMARY KEY(xuid,model));
          CREATE TABLE IF NOT EXISTS notices (id TEXT PRIMARY KEY, xuid TEXT NOT NULL,
            message TEXT NOT NULL, sent INTEGER NOT NULL DEFAULT 0);
        """)
        self.db.commit()

    def revision(self):
        return self.db.execute("SELECT revision FROM meta").fetchone()[0]

    def bump(self):
        self.db.execute("UPDATE meta SET revision=revision+1")

    def credential(self, peer):
        row = self.server.friends.store.get(peer.xuid)
        return row.get("key", "") if row and peer.friends_ok else ""

    def authenticated(self, peer):
        row = self.db.execute("SELECT credential FROM identities WHERE xuid=?", (hx(peer.xuid),)).fetchone()
        return bool(row and peer.events_identity == (peer.xuid, row[0]) and
                    self.credential(peer) == row[0] and row[0])

    def enroll(self, peer):
        credential = self.credential(peer)
        if not credential or peer.id in self.server.restored:
            raise ValueError("player must sign in with a verified friends credential")
        xuid = hx(peer.xuid)
        if self.server.friends.store.path and not self.server.friends.store.save():
            raise OSError("could not persist the player's identity binding")
        with self.db:
            old = self.db.execute("SELECT credential FROM identities WHERE xuid=?", (xuid,)).fetchone()
            if old and old[0] != credential:
                raise ValueError("identity already enrolled; recovery requires operator investigation")
            self.db.execute("INSERT OR IGNORE INTO identities VALUES(?,?,?)", (xuid, credential, self.clock()))
            self.bump()
        return {"xuid": xuid, "enrolled": True}

    def create(self, data):
        eid = data.get("id", "")
        if not isinstance(eid, str) or not re.fullmatch(r"[a-z0-9][a-z0-9-]{0,63}", eid):
            raise ValueError("event ID: lowercase letters, numbers and hyphens, max 64")
        fields = [data.get(k, "") for k in ("name", "description", "activity", "session", "organizer")]
        if any(not isinstance(s, str) for s in fields):
            raise ValueError("event text fields must be strings")
        name, description, activity, session, organizer = fields
        if not name.strip() or len(name) > 80 or len(description) > 320 or not activity.strip() or len(activity) > 80:
            raise ValueError("name/activity required; name/activity max 80, description max 320")
        if not re.fullmatch(r"[0-9a-fA-F]{16}", session) or int(session, 16) == 0:
            raise ValueError("session must be a nonzero 16-digit session ID from server status")
        if not organizer.strip() or len(organizer) > 80:
            raise ValueError("organizer required, max 80 characters")
        start, end = data.get("starts"), data.get("ends")
        if (type(start) not in (int, float) or type(end) not in (int, float)
                or not 0 <= start < end < 1e12 or end - start > 7 * 86400):
            raise ValueError("starts/ends: Unix seconds, increasing, at most seven days apart")
        rewards = models(data.get("rewards"))
        values = (eid, name, description, activity, session.lower(), start, end,
                  json.dumps(rewards), "draft", organizer, self.clock())
        with self.db:
            if self.db.execute("SELECT 1 FROM events WHERE id=?", (eid,)).fetchone():
                raise ValueError("event ID already exists")
            self.db.execute("INSERT INTO events VALUES(?,?,?,?,?,?,?,?,?,?,?)", values)
            self.bump()
        return self.event(eid)

    def event(self, eid):
        row = self.db.execute("SELECT * FROM events WHERE id=?", (eid,)).fetchone()
        if not row:
            raise ValueError("unknown event")
        result = dict(row)
        result["rewards"] = json.loads(result["rewards"])
        return result

    def list(self, public=False):
        query = "SELECT id FROM events" + (" WHERE state='active' AND ends>?" if public else "")
        return [self.event(r[0]) for r in self.db.execute(query, (self.clock(),) if public else ())]

    def transition(self, eid, state):
        event = self.event(eid)
        if state == event["state"]:
            return event
        if state not in ("active", "closed") or event["state"] == "closed":
            raise ValueError("event is closed or transition is invalid")
        if state == "active" and self.clock() >= event["ends"]:
            raise ValueError("event has already ended")
        with self.db:
            self.db.execute("UPDATE events SET state=? WHERE id=?", (state, eid))
            self.bump()
        return self.event(eid)

    def notice(self, key, xuid, message):
        self.db.execute("INSERT OR IGNORE INTO notices(id,xuid,message) VALUES(?,?,?)",
                        (key, xuid, message[:512]))

    def observe(self, peer):
        if not self.authenticated(peer):
            return
        now, xuid, credential = self.clock(), hx(peer.xuid), self.credential(peer)
        sessions = {record[1][:8].hex() for record in (peer.match, peer.standby) if record and len(record[1]) >= 8}
        with self.db:
            for event in self.list(public=True):
                if now < event["starts"] or event["session"] not in sessions:
                    continue
                old = self.db.execute("SELECT * FROM attendance WHERE event=? AND xuid=?", (event["id"], xuid)).fetchone()
                if old and old["credential"] != credential:
                    continue
                if old:
                    # A reconnect never fills the offline gap. Polls are normally 3 s apart.
                    delta = now - old["last"]
                    self.db.execute("UPDATE attendance SET last=?, seconds=seconds+? WHERE event=? AND xuid=?",
                                    (now, delta if 0 <= delta <= 6 else 0, event["id"], xuid))
                else:
                    self.db.execute("INSERT INTO attendance(event,xuid,credential,first,last) VALUES(?,?,?,?,?)",
                                    (event["id"], xuid, credential, now, now))
                self.notice("event:" + event["id"] + ":" + xuid, xuid,
                            "Community event: " + event["name"] + ". " + event["description"] +
                            " Rewards are approved by the organizer.")

    def attendees(self, eid):
        self.event(eid)
        return [dict(r) for r in self.db.execute(
            "SELECT xuid,first,last,seconds FROM attendance WHERE event=? ORDER BY first", (eid,))]

    def award(self, eid, data):
        event = self.event(eid)
        xuid = hx(int(data.get("xuid", ""), 16))
        owned = data.get("owned_models")
        if (not isinstance(owned, list) or len(owned) > 7 or
                any(type(m) is not int or m not in event["rewards"] for m in owned) or len(set(owned)) != len(owned)):
            raise ValueError("owned_models is required: organizer-confirmed reward IDs already owned, or []")
        if event["state"] == "draft":
            raise ValueError("start the event before approving rewards")
        with self.db:
            attendee = self.db.execute("SELECT credential FROM attendance WHERE event=? AND xuid=?", (eid, xuid)).fetchone()
            identity = self.db.execute("SELECT credential FROM identities WHERE xuid=?", (xuid,)).fetchone()
            if not attendee or not identity or attendee[0] != identity[0]:
                raise ValueError("no enrolled attendance record for this player")
            for model in event["rewards"]:
                # Repeating an approval never resurrects a revoked grant.
                self.db.execute("INSERT OR IGNORE INTO grants VALUES(?,?,?,?,?,?,?,NULL)",
                                (eid, xuid, model, identity[0], str(uuid.uuid4()), event["organizer"], self.clock()))
                self.db.execute("INSERT OR IGNORE INTO deliveries VALUES(?,?,?,?,NULL,NULL)",
                                (xuid, model, "owned" if model in owned else "pending", str(uuid.uuid4())))
                if self.db.execute("SELECT 1 FROM grants WHERE xuid=? AND model=? AND revoked IS NULL",
                                   (xuid, model)).fetchone():
                    self.db.execute("UPDATE deliveries SET state=? WHERE xuid=? AND model=? AND state='cancelled'",
                                    ("owned" if model in owned else "pending", xuid, model))
            self.notice("award:" + eid + ":" + xuid, xuid,
                        "Event reward unlocked: " + event["name"] +
                        ". Multiplayer on this server: " + ", ".join(NAMES[m] for m in event["rewards"]) +
                        ". Missing cars arrive separately in Gifts & Auctions.")
            self.bump()
        return self.player(xuid)

    def revoke(self, eid, data):
        self.event(eid)
        xuid = hx(int(data.get("xuid", ""), 16))
        with self.db:
            self.db.execute("UPDATE grants SET revoked=? WHERE event=? AND xuid=? AND revoked IS NULL",
                            (self.clock(), eid, xuid))
            self.db.execute("UPDATE deliveries SET state='cancelled' WHERE xuid=? AND state='pending' AND model NOT IN "
                            "(SELECT model FROM grants WHERE xuid=? AND revoked IS NULL)", (xuid, xuid))
            self.notice("revoke:" + eid + ":" + xuid, xuid,
                        "The organizer revoked your multiplayer reward unlock for " + self.event(eid)["name"] +
                        ". Cars in your single-player garage are unchanged.")
            self.bump()
        return self.player(xuid)

    def player(self, xuid):
        return {"xuid": xuid, "grants": [dict(r) for r in self.db.execute(
            "SELECT event,model,id,organizer,granted,revoked FROM grants WHERE xuid=?", (xuid,))],
                "deliveries": [dict(r) for r in self.db.execute(
                    "SELECT model,state,gift,error FROM deliveries WHERE xuid=?", (xuid,))]}

    def mask(self, peer):
        if not self.authenticated(peer):
            return 0
        allowed = {r[0] for r in self.db.execute(
            "SELECT DISTINCT model FROM grants WHERE xuid=? AND credential=? AND revoked IS NULL",
            (hx(peer.xuid), self.credential(peer)))}
        return sum(1 << i for i, model in enumerate(MODELS) if model in allowed)

    def hello(self, peer):
        if peer.features & FEATURE:
            self.server.send(peer.addr, KIND, struct.pack(">BHB", 0, self.server.epoch, self.required))

    def on_message(self, peer, body):
        if len(body) != 17 or body[0] != 1 or not peer.features & FEATURE:
            return
        token, nonce = struct.unpack_from(">QQ", body, 1)
        if token != peer.token:
            return
        # Bound to the current registered peer, not a client-supplied XUID/header.
        authorized = self.authenticated(peer)
        if authorized:
            self.observe(peer)
        self.server.send(peer.addr, KIND, struct.pack(">BQIHIBBB", 2, nonce, self.revision(),
                         self.server.epoch, peer.id, authorized, self.required, self.mask(peer)))

    def flush(self):
        # Each side effect persists its deduplication key before this DB marks it sent.
        # A crash between those writes is safe to retry on the next server tick.
        for row in self.db.execute("SELECT * FROM deliveries WHERE state='pending' LIMIT 16").fetchall():
            try:
                gift = market.gift_stock_car(int(row["xuid"], 16), row["model"],
                                             idempotency_key="event:" + row["delivery_key"])
                with self.db:
                    self.db.execute("UPDATE deliveries SET state='sent',gift=?,error=NULL WHERE delivery_key=?",
                                    (gift, row["delivery_key"]))
            except (ValueError, OSError) as error:
                with self.db:
                    self.db.execute("UPDATE deliveries SET error=? WHERE delivery_key=?",
                                    (str(error)[:200], row["delivery_key"]))
        for row in self.db.execute("SELECT * FROM notices WHERE sent=0 LIMIT 16").fetchall():
            try:
                notifications.send(int(row["xuid"], 16), 0x100, {"Message": row["message"]},
                                   idempotency_key="event:" + row["id"])
                with self.db:
                    self.db.execute("UPDATE notices SET sent=1 WHERE id=?", (row["id"],))
            except (ValueError, OSError):
                pass

    def handle(self, method, path, data):
        parts = path.strip("/").split("/")
        if parts == ["admin", "events"]:
            if method == "GET":
                return {"events": self.list(), "revision": self.revision(), "required": self.required}
            if method == "POST":
                return self.create(data)
        if parts == ["admin", "events", "enroll"] and method == "POST":
            xuid = int(data.get("xuid", ""), 16)
            candidates = [p for p in self.server.peers.values() if p.xuid == xuid and self.credential(p)]
            if len(candidates) != 1:
                raise ValueError("expected exactly one signed-in player with this XUID")
            return self.enroll(candidates[0])
        if len(parts) == 4 and parts[:2] == ["admin", "events"]:
            eid, action = parts[2:]
            if method == "GET" and action == "attendance":
                return {"attendees": self.attendees(eid)}
            if method == "POST":
                if action in ("start", "close"):
                    return self.transition(eid, "active" if action == "start" else "closed")
                if action == "award":
                    return self.award(eid, data)
                if action == "revoke":
                    return self.revoke(eid, data)
        raise ValueError("unknown event endpoint or method")
