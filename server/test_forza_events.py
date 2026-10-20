"""Community events: enrollment, participation, durable awards, retries and protocol scope."""
import json
import os
import struct
import tempfile
import unittest
from unittest.mock import patch

import forza_events as ev
import forza_live_server as srv
import forza_market as market
import forza_notifications as notices


class Transport:
    def __init__(self): self.sent = []
    def sendto(self, data, addr): self.sent.append((addr, data))


class EventsTest(unittest.TestCase):
    def setUp(self):
        self.temp = tempfile.TemporaryDirectory()
        self.addCleanup(self.temp.cleanup)
        self.now = 1000.0
        self.server = srv.ForzaLive("test", 16)
        self.server.transport = Transport()
        self.server.events.db.close()
        self.path = os.path.join(self.temp.name, "events.sqlite3")
        self.server.events = ev.Events(self.server, self.path, True, lambda: self.now)
        self.addCleanup(lambda: self.server.events.db.close())
        self.old_market, self.old_notices = market.DATA_DIR, notices.DATA_DIR
        market.configure(self.temp.name); notices.configure(self.temp.name)
        self.addCleanup(lambda: market.configure(self.old_market))
        self.addCleanup(lambda: notices.configure(self.old_notices))
        self.player = self.join()
        self.server.events.enroll(self.player)
        self.sid = bytes.fromhex("0123456789abcdef")
        self.player.match = (3, self.sid + bytes(60), self.now)
        self.data = dict(id="festival-night", name="Festival Night", description="Cruise together",
                         activity="Online free roam", session=self.sid.hex(), organizer="Organizer",
                         starts=999, ends=1100, rewards=[1197])
        self.events = self.server.events
        self.events.create(self.data); self.events.transition(self.data["id"], "active")

    def join(self, key=b"A" * 16, features=15, address=20001):
        body = struct.pack(">HQQIH", 1, address, 0x9000000000001, 0, 0)
        body += b"\x04#TU4\x06Driver" + bytes([features, len(key)]) + key
        self.server.datagram_received(b"RLV1\x01" + body, ("127.0.0.1", address))
        return self.server.by_addr.get(("127.0.0.1", address))

    def poll(self, peer=None, token=None, nonce=123):
        peer = peer or self.player
        self.events.on_message(peer, struct.pack(">BQQ", 1, peer.token if token is None else token, nonce))

    def approve(self, owned=None):
        return self.events.award(self.data["id"], dict(xuid="0009000000000001", owned_models=owned or []))

    def test_attendance_requires_active_matching_session_and_enrollment(self):
        self.poll(); self.now += 3; self.poll(nonce=124)
        row = self.events.attendees(self.data["id"])[0]
        self.assertEqual(row["seconds"], 3)
        self.now += 30; self.poll(nonce=125)
        self.assertEqual(self.events.attendees(self.data["id"])[0]["seconds"], 3)
        self.player.match = None; self.now += 3; self.poll(nonce=126)
        self.assertEqual(self.events.attendees(self.data["id"])[0]["last"], 1033)

    def test_no_claim_from_arbitrary_xuid_or_wrong_credential(self):
        impostor = self.join(key=b"B" * 16, address=20002)
        self.assertFalse(impostor.friends_ok)
        self.assertFalse(self.events.authenticated(impostor))
        self.poll(impostor)
        self.assertEqual(self.events.attendees(self.data["id"]), [])
        self.poll(token=0)
        self.assertEqual(self.events.attendees(self.data["id"]), [])

    def test_restored_registration_is_not_event_proof(self):
        self.player.events_identity = None
        self.assertFalse(self.events.authenticated(self.player))
        self.poll()
        self.assertEqual(self.events.attendees(self.data["id"]), [])

    def test_old_clients_rejected_only_when_policy_required(self):
        self.assertIsNone(self.join(features=7, address=20003))
        self.events.required = False
        self.assertIsNotNone(self.join(features=7, address=20003))

    def test_welcome_already_contains_required_policy(self):
        welcome = next(d[5:] for _, d in self.server.transport.sent if d[4] == srv.WELCOME)
        motd_length = struct.unpack_from(">H", welcome, 12)[0]
        self.assertEqual(welcome[14 + motd_length + 2], 1)

    def test_award_requires_attendance_and_explicit_ownership(self):
        with self.assertRaises(ValueError): self.approve()
        self.poll()
        with self.assertRaises(ValueError):
            self.events.award(self.data["id"], {"xuid": "0009000000000001"})
        self.assertEqual(self.events.mask(self.player), 0)

    def test_already_owned_gets_unlock_without_car_duplicate(self):
        self.poll(); self.approve([1197]); self.approve([1197])
        self.events.flush()
        self.assertEqual(self.events.mask(self.player), 1 << 4)
        self.assertEqual(len(self.events.player("0009000000000001")["grants"]), 1)
        self.assertEqual(market.MARKET.gifts, {})
        self.assertEqual(len(notices.pending(self.player.xuid)), 2)  # event and award

    def test_outbox_retry_restart_and_car_notification_deduplication(self):
        self.poll(); self.approve()
        with patch("forza_clubs.game_car", return_value=(2010, 1)):
            with patch.object(notices, "send", side_effect=OSError("disk unavailable")):
                self.events.flush()
            self.assertEqual(len(market.MARKET.gifts), 1)
            # Simulate restart after gift persisted but delivery acknowledgement failed.
            market.configure(self.temp.name); notices.configure(self.temp.name)
            self.events.db.close()
            self.events = self.server.events = ev.Events(self.server, self.path, True, lambda: self.now)
            self.events.flush(); self.events.flush(); self.approve(); self.events.flush()
        self.assertEqual(len(market.MARKET.gifts), 1)
        self.assertEqual(len([n for n in notices.pending(self.player.xuid) if n["type"] == notices.GIFT_CAR]), 1)
        self.assertEqual(self.events.mask(self.player), 1 << 4)

    def test_failed_delivery_does_not_erase_grant(self):
        self.poll(); self.approve()
        with patch.object(market, "gift_stock_car", side_effect=OSError("disk full")):
            self.events.flush()
        self.assertEqual(self.events.mask(self.player), 1 << 4)
        self.assertEqual(self.events.player("0009000000000001")["deliveries"][0]["state"], "pending")

    def test_retry_after_gift_save_failure_really_persists_before_ack(self):
        self.poll(); self.approve()
        with patch("forza_clubs.game_car", return_value=(2010, 1)):
            with patch.object(market.MARKET, "save", side_effect=OSError("disk full")):
                self.events.flush()
            self.events.flush()
        market.configure(self.temp.name); market.MARKET.load()
        self.assertEqual(len(market.MARKET.gifts), 1)
        self.assertEqual(self.events.player("0009000000000001")["deliveries"][0]["state"], "sent")

    def test_notification_receipt_survives_clear_and_restart(self):
        first = notices.send(self.player.xuid, 0x100, {"Message": "Event reward"}, idempotency_key="test-receipt")
        notices.STORE.load()["byRecipient"] = {}; notices.STORE.save()
        notices.configure(self.temp.name)
        self.assertEqual(notices.send(self.player.xuid, 0x100, {"Message": "Event reward"},
                                     idempotency_key="test-receipt"), first)
        self.assertEqual(notices.pending(self.player.xuid), [])

    def test_revoke_does_not_remove_cars_or_retry_as_new_grant(self):
        self.poll(); self.approve([1197])
        self.events.revoke(self.data["id"], {"xuid": "0009000000000001"})
        self.approve([1197])
        self.assertEqual(self.events.mask(self.player), 0)
        self.assertEqual(self.events.player("0009000000000001")["deliveries"][0]["state"], "owned")

    def test_all_seven_and_separate_event_do_not_duplicate_deliveries(self):
        data = dict(self.data, id="all-seven", rewards=list(ev.MODELS))
        self.events.create(data); self.events.transition("all-seven", "active"); self.poll()
        self.events.award("all-seven", {"xuid": "0009000000000001", "owned_models": list(ev.MODELS)})
        self.approve([1197])
        self.assertEqual(self.events.mask(self.player), 127)
        self.assertEqual(len(self.events.player("0009000000000001")["deliveries"]), 7)

    def test_protocol_echoes_current_request_and_server_registration(self):
        self.poll(); body = self.server.transport.sent[-1][1][5:]
        result = struct.unpack(">BQIHIBBB", body)
        self.assertEqual(result, (2, 123, self.events.revision(), self.server.epoch, self.player.id, 1, 1, 0))
        self.assertEqual(len(body), 22)
        before = len(self.server.transport.sent)
        for bad in (b"", b"\x01", bytes(1500)):
            self.events.on_message(self.player, bad)
        self.assertEqual(len(self.server.transport.sent), before)

    def test_validation_and_closed_event_attendance(self):
        for change in ({"rewards": [309]}, {"rewards": [1197, 1197]}, {"starts": float("nan")},
                       {"session": "0" * 16}, {"organizer": ""}):
            with self.assertRaises(ValueError): self.events.create(dict(self.data, id="invalid", **change))
        self.events.transition(self.data["id"], "closed"); self.poll()
        self.assertEqual(self.events.attendees(self.data["id"]), [])
        with self.assertRaises(ValueError): self.events.transition(self.data["id"], "active")


if __name__ == "__main__": unittest.main()
