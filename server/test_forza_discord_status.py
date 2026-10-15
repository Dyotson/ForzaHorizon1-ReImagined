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

    def test_summary_skips_probe_and_empty_worlds(self):
        status = {"rooms": {"": [{}, {}], "#TU4": [{}], "probe": [{}]},
                  "worlds": {"a": {"humans": 2}, "b": {"humans": 0}}}
        self.assertEqual(ds.summary(status), (3, 1))

    def test_render(self):
        self.assertIn("**1 player online** · 1 Horizon World open", ds.render(1, 1, now=100))
        self.assertIn("**4 players online** · 0 Horizon Worlds open", ds.render(4, 0, now=100))
        self.assertIn("offline", ds.render(0, 0, online=False, now=100))
        self.assertIn("<t:100:R>", ds.render(0, 0, now=100))

    def test_due_on_change_and_heartbeat(self):
        ds._state.update(key=(2, 1), sent=1000.0)
        self.assertFalse(ds.due(2, 1, now=1100))
        self.assertTrue(ds.due(3, 1, now=1100))
        self.assertTrue(ds.due(2, 1, now=1000 + ds.HEARTBEAT))

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
