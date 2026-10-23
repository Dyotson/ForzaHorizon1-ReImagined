"""The Playground Games playlist (issues #13, #14): its rounds in turn (--playground-games),
the Cat & Mouse preset a two-player lobby plays, and the profile upload after a round.

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

import argparse
import tempfile
import unittest
import xml.etree.ElementTree as ET

import forza_limits as limits
import forza_live_lsp as lsp
import forza_xtsw as x

A = 0x0009000000001241


def options(round_num, hopper=lsp.PLAYGROUND_HOPPER):
    body = bytes(x.encode(x.HopperGetGameOptions_Params,
                          {"hopperId": hopper, "roundNum": round_num, "cchOptionXmlMax": 65536}))
    reply = lsp.hopper_get_game_options(None, A, body)
    results, _ = x.decode(x.HopperGetGameOptions_Results, reply[12:])
    return ET.fromstring(results["gameOptionsXml"])


def upload(race=None, credits=None):
    params = {"playerRaceHistory": [race] if race else [],
              "playerHistory": [{"credits": credits}] if credits is not None else []}
    lsp.online_profile_update_all(A, bytes(x.encode(x.OnlineProfileUpdateAll_Params, params)))
    return lsp.load_player(A)["profile"]


class PlaygroundRoundsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = list(lsp.PLAYGROUND_ROUNDS)

    def tearDown(self):
        lsp.PLAYGROUND_ROUNDS[:] = self.saved
        self.dir.cleanup()

    def configure(self, *argv):
        parser = argparse.ArgumentParser()
        lsp.add_arguments(parser)
        lsp.configure(parser.parse_args(["--data-dir", self.dir.name] + list(argv)))

    def test_default_rotation(self):
        self.configure()
        presets = [options(12259 + n).get("GameTypePreset") for n in range(6)]
        self.assertEqual(sorted(set(presets)), ["4", "5", "6"])
        self.assertEqual(presets[:3], presets[3:])

    def test_cat_and_mouse_only(self):
        self.configure("--playground-games", "5")
        for n in range(4):
            self.assertEqual(options(12259 + n).get("GameTypePreset"), "5")

    def test_king_then_cat_and_mouse(self):
        self.configure("--playground-games", "6,5")
        presets = [options(n).get("GameTypePreset") for n in range(4)]
        self.assertEqual(presets, ["6", "5", "6", "5"])

    def test_cat_and_mouse_preset(self):
        """Clear Springs Sprint, 2 laps; two teams, each with exactly one Mouse: two players
        are two teams of one, both mice (the lobby's team/role draw, #14)."""
        self.configure("--playground-games", "5")
        root = options(0)
        self.assertEqual(root.get("TrackId"), "322")
        self.assertEqual(root.find("EndConditionOptions").get("NumLaps"), "2")
        self.assertEqual(root.find("ScoringOptions").get("NumTeams"), "2")
        mouse, cat = root.findall("Buckets/Bucket")
        self.assertEqual((mouse.get("Name"), mouse.get("MinPlayers"), mouse.get("MaxPlayers")),
                         (":Mouse", "1", "1"))
        self.assertEqual((cat.get("Name"), cat.get("MinPlayers")), (":Cat", "0"))


class ProfileUploadTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        self.saved = lsp.DATA_DIR
        lsp.DATA_DIR = self.dir.name
        limits.configure(self.dir.name)

    def tearDown(self):
        lsp.DATA_DIR = self.saved
        self.dir.cleanup()

    def test_upload_without_credits_keeps_them(self):
        """After a Cat & Mouse round the cat (no credits won) uploads its race history alone."""
        upload({"onlineXP": 660, "onlineWristbandLevel": 1}, credits=630931)
        profile = upload({"onlineXP": 1320, "onlineWristbandLevel": 2})
        self.assertEqual(profile["credits"], 630931)
        self.assertEqual(profile["onlineXP"], 1320)
        self.assertEqual(profile["onlineWristbandLevel"], 2)

    def test_online_xp_only_grows(self):
        upload({"onlineXP": 3000}, credits=1)
        self.assertEqual(upload({"onlineXP": 0})["onlineXP"], 3000)

    def test_credits_follow_the_upload(self):
        upload({"onlineXP": 10}, credits=500)
        self.assertEqual(upload(credits=650)["credits"], 650)


if __name__ == "__main__":
    unittest.main()
