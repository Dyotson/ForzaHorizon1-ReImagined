"""Player-chosen display strings are plain ASCII (forza_limits.clean_gamertag and friends):
Zalgo, CJK, emoji, bidi overrides, zero-width and control characters never reach the game."""

import struct
import tempfile
import unittest

import forza_chat as ch
import forza_clubs as clubs
import forza_limits as limits
import forza_xtsw as x
import forza_xtsw_all as A

XUID = 0xE000123456789ABC


def tag(s):
    return limits.clean_gamertag(s, XUID)


class GamertagTests(unittest.TestCase):
    def test_plain_names_pass(self):
        self.assertEqual(tag("Dyotson"), "Dyotson")
        self.assertEqual(tag("  Two   Words 42 "), "Two Words 42")

    def test_accents_are_transliterated(self):
        self.assertEqual(tag("José Müller"), "Jose Muller")

    def test_zalgo_loses_its_marks(self):
        self.assertEqual(tag("Zalgo" + "̀́̂̃" * 15), "Zalgo")

    def test_only_foreign_glyphs_gives_the_fallback(self):
        for s in ("中文名前", "\U0001F600\U0001F680", "", "   ", "̀" * 15):
            self.assertEqual(tag(s), "Driver9ABC", s)
        self.assertEqual(limits.clean_gamertag("中"), limits.clean_gamertag("中"))

    def test_invisible_and_control_characters(self):
        self.assertEqual(tag("a‮b​c‍d\x00e\x07f\n"), "abcd e f")
        self.assertEqual(tag("‮evil"), "evil")

    def test_length_cap(self):
        self.assertEqual(tag("A" * 40), "A" * 15)
        self.assertEqual(tag("Abcdefghijklmn Pqrs"), "Abcdefghijklmn")  # no trailing space

    def test_bytes_never_raise(self):
        self.assertEqual(tag(b"\xff\xfe bad \xc3"), "bad")
        self.assertTrue(tag(None).startswith("Driver"))

    def test_result_is_always_ascii_alnum_or_space(self):
        for s in ("<b>x</b>", "a\tb", "x_y-z", "\ud83d"):
            out = tag(s)
            self.assertTrue(out.isascii() and all(c.isalnum() or c == " " for c in out), out)


class TextTests(unittest.TestCase):
    def test_ascii_text(self):
        self.assertEqual(limits.clean_ascii_text("Café 中文 Club", 20), "Cafe Club")
        self.assertEqual(limits.clean_ascii_text("x" * 50, 20), "x" * 20)
        self.assertEqual(limits.clean_ascii_text("中文", 20, "Club 7"), "Club 7")
        self.assertEqual(limits.clean_ascii_text("a\nb\tc", 20), "a b c")


class OutputTests(unittest.TestCase):
    def test_encoder_cleans_stored_names(self):
        raw = x.encode(A.UserGetUserData_Results, {"userData": {
            "qwXuid": XUID, "wzGamerTag": "Zed" + "́" * 40, "clanTag": "中文",
            "clanId": 1}})
        self.assertNotIn(b"\xe4\xb8", raw)
        self.assertNotIn(b"\xcc\x81", raw)
        self.assertIn(b"Zed", raw)

    def test_club_with_cjk_name(self):
        with tempfile.TemporaryDirectory() as d:
            clubs.configure(d, lambda xuid: {"gamertag": "中文"})
            raw = ("Z\u4e2d\u6587z", "\u4e2d\u6587\u6e38")  # built by hand: encode() cleans
            body = b"".join(struct.pack(">I", len(b.encode())) + b.encode() for b in raw)
            reply = clubs.create_clan(XUID, body)
            club = next(c for c in clubs.CLUBS.load()["clubs"].values() if c["tag"] == "Zz")
            self.assertEqual(club["name"], "Club %d" % club["id"])
            self.assertEqual(club["tag"], "Zz")


class ChatTests(unittest.TestCase):
    def test_chat_strips_marks_and_controls(self):
        self.assertEqual(ch.clean("hi" + "́" * 30 + "‮ there\x00"), "hi there")
        self.assertEqual(len(ch.clean("x" * 1000)), ch.MAX_CHARS)


if __name__ == "__main__":
    unittest.main()
