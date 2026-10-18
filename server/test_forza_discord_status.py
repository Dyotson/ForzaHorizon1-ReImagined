import io
import json
import os
import tempfile
import unittest
import urllib.error
from unittest import mock

import forza_discord_status as ds


class DiscordStatusTest(unittest.TestCase):
    def setUp(self):
        ds._state.update(key=None, sent=0.0)
        self.dir = tempfile.mkdtemp()

    def test_summary_skips_probe(self):
        world = {"playlist": 3, "session": "aa", "players": 2, "slots": 8}
        status = {"rooms": {"": [{}, {}], "#TU4": [{"match": world}],
                            "probe": [{"match": dict(world, session="bb")}]}}
        self.assertEqual(ds.summary(status), (3, 1, {"Horizon World": [(2, 8)]}))

    def test_one_lobby_named_by_its_host_when_members_searched_elsewhere(self):
        # live 2026-10-18: a custom Street Race lobby turned Circuit Race; members kept the
        # playlist they had searched (4, and 3 for two who came from Find Game)
        sess = lambda playlist, gt, n=1: {"playlist": playlist, "session": "88", "players": n,
                                          "slots": 8, "game_type": gt}
        peers = [{"id": 1, "match": sess(2, 2, 5)}] + \
            [{"id": i, "standby": sess(4, 4)} for i in (2, 3, 4)] + \
            [{"id": i, "standby": sess(3, None)} for i in (5, 6)]
        self.assertEqual(ds.summary({"rooms": {"#TU4": peers}}),
                         (6, 0, {"Custom Circuit Race": [(6, 8)]}))

    def test_lobbies_by_playlist_and_custom_game_type(self):
        def m(playlist, session, players=1, slots=8, game_type=None):
            return {"playlist": playlist, "session": session, "players": players,
                    "slots": slots, "game_type": game_type}
        status = {"rooms": {"#TU4": [
            {"id": 1, "match": m(4, "aa", 5, game_type=4)},           # custom street race host
            {"id": 2, "standby": m(4, "aa", game_type=4)},            # and 5 members
            {"id": 3, "standby": m(4, "aa", game_type=4)},
            {"id": 4, "standby": m(4, "aa", game_type=4)},
            {"id": 5, "standby": m(4, "aa", game_type=4)},
            {"id": 6, "standby": m(4, "aa", game_type=4)},
            {"id": 7, "match": m(3, "bb", 2)},                        # Horizon World
            {"id": 8, "standby": m(3, "cc"), "match": None},          # world whose host left
            {"id": 9, "standby": m(3, "cc")},
            {"id": 10, "match": m(3, "dd", 1, game_type=3)},          # custom point-to-point
            {"id": 11, "match": m(ds.PRIVATE, "ee", 2)},              # private custom game
            {"id": 12, "match": None, "standby": None}],              # single player
            "probe": [{"id": 13, "match": m(3, "ff")}]}}
        found = ds.lobbies(status)
        self.assertEqual(found, {"Custom Street Race": [(6, 8)],
                                 "Horizon World": [(2, 8), (2, 8)],
                                 "Custom Point-to-Point": [(1, 8)],
                                 "Private custom games": [(2, 8)]})
        text = ds.render(14, 2, now=100, found=found)
        self.assertIn("• **Custom Street Race**: 6 players — 6/8", text)
        self.assertIn("• **Horizon World**: 4 players — 2/8 · 2/8", text)
        self.assertIn("• Private custom games: 2 players", text)
        self.assertIn("• Single player or menus: 1", text)
        self.assertLess(text.index("Street Race"), text.index("**Horizon World**"))
        self.assertLess(text.index("Point-to-Point"), text.index("Private"))

    def test_edit_when_lobbies_change(self):
        status = {"rooms": {"#TU4": [{"id": 1, "match": {"playlist": 1, "session": "aa",
                                                          "players": 1, "slots": 8}}]}}
        with mock.patch.object(ds, "WEBHOOK", "https://discord.com/api/webhooks/1/x"), \
                mock.patch.object(ds, "post", return_value=True) as post:
            ds.update(status, self.dir, now=1000)
            ds.update(status, self.dir, now=1030)
            status["rooms"]["#TU4"][0]["match"]["players"] = 2
            ds.update(status, self.dir, now=1060)
        self.assertEqual(post.call_count, 2)
        self.assertIn("**Races**: 2 players — 2/8", post.call_args[0][1])

    def test_render(self):
        self.assertIn("**1 player online** · 1 Horizon World open", ds.render(1, 1, now=100))
        self.assertIn("**4 players online** · 0 Horizon Worlds open", ds.render(4, 0, now=100))
        self.assertIn("offline", ds.render(0, 0, online=False, now=100))
        self.assertIn("<t:100:R>", ds.render(0, 0, now=100))

    def test_due_on_change_and_heartbeat(self):
        ds._state.update(key=(2, 1, ()), sent=1000.0)
        self.assertFalse(ds.due((2, 1, ()), now=1100))
        self.assertTrue(ds.due((3, 1, ()), now=1100))
        self.assertTrue(ds.due((2, 1, ()), now=1000 + ds.HEARTBEAT))

    def test_post_creates_then_edits_and_recreates_after_delete(self):
        calls = []

        def fake(method, url, content):
            calls.append((method, url))
            if method == "PATCH" and len(calls) == 2:
                raise urllib.error.HTTPError(url, 404, "gone", {}, io.BytesIO(b""))
            return {"id": str(len(calls))}

        with mock.patch.object(ds, "WEBHOOK", "https://discord.com/api/webhooks/1/x"), \
                mock.patch.object(ds, "_request", side_effect=fake):
            self.assertTrue(ds.post(self.dir, "a"))           # POST, keeps id "1"
            self.assertTrue(ds.post(self.dir, "b"))           # PATCH 404 -> POST, id "3"
            self.assertTrue(ds.post(self.dir, "c"))           # PATCH id 3
        self.assertEqual([c[0] for c in calls], ["POST", "PATCH", "POST", "PATCH"])
        self.assertTrue(calls[-1][1].endswith("/messages/3"))
        with open(os.path.join(self.dir, "discord_status.json")) as f:
            self.assertEqual(json.load(f)["id"], "3")

    def test_post_never_raises(self):
        with mock.patch.object(ds, "WEBHOOK", "https://discord.com/api/webhooks/1/x"), \
                mock.patch.object(ds, "_request", side_effect=OSError("down")):
            self.assertFalse(ds.post(self.dir, "a"))

    def test_disabled_without_webhook(self):
        with mock.patch.object(ds, "WEBHOOK", ""), mock.patch.object(ds, "post") as post:
            ds.update({"rooms": {"": [{}]}, "worlds": {}}, self.dir)
            post.assert_not_called()


if __name__ == "__main__":
    unittest.main()
