"""Xbox LIVE stats in-process (forza_stats.py): XSessionWriteStats of the TrueSkill view,
the rating of a game, XUserReadStats rows.

Run: cd server && python3 -m unittest discover -p 'test_*.py'
"""

import struct
import tempfile
import unittest

import forza_limits as limits
import forza_stats as stats

A, B, C = 0x0009000000000A01, 0x0009000000000B02, 0x0009000000000C03
TAGS = {A: "DriverA", B: "DriverB", C: "DriverC"}
SKILL_VIEW = 0xFFFF0000
SESSION = 0x80AB0000000000CD


def write_body(xuid, score, team=-1, view=SKILL_VIEW, session=SESSION):
    """A write as live_xmsg.cpp sends it (after the op byte): the game's two INT32
    properties, value in the first 4 of 8 bytes."""
    props = [(stats.RELATIVE_SCORE, struct.pack(">i", score) + bytes(4)),
             (stats.SESSION_TEAM, struct.pack(">i", team) + bytes(4))]
    body = struct.pack(">QQB", session, xuid, 1) + struct.pack(">IB", view, len(props))
    for pid, value in props:
        body += struct.pack(">IB", pid, stats.TYPE_INT32) + value
    return body


def read_body(xuids, specs, request=7):
    body = struct.pack(">IB", request, len(xuids)) + b"".join(struct.pack(">Q", x) for x in xuids)
    body += bytes([len(specs)])
    for view, cols in specs:
        body += struct.pack(">IB", view, len(cols)) + b"".join(struct.pack(">H", c) for c in cols)
    return body


def parse_rows(reply):
    """{view: (total, [(xuid, rank, rating, tag, {col: value})])} of a STATS_ROWS answer."""
    request, nviews = struct.unpack_from(">IB", reply, 0)
    pos, out = 5, {}
    for _ in range(nviews):
        view, total, nrows = struct.unpack_from(">IIB", reply, pos)
        pos += 9
        rows = []
        for _ in range(nrows):
            xuid, rank, rating, n = struct.unpack_from(">QIqB", reply, pos)
            pos += 21
            tag = reply[pos:pos + n].decode()
            ncols = reply[pos + n]
            pos += n + 1
            cols = {}
            for _ in range(ncols):
                cid, kind = struct.unpack_from(">HB", reply, pos)
                raw = reply[pos + 3:pos + 11]
                cols[cid] = (struct.unpack(">q", raw)[0] if kind == stats.TYPE_INT64 else
                             struct.unpack(">d", raw)[0] if kind == stats.TYPE_DOUBLE else
                             None if kind == stats.TYPE_NULL else raw)
                pos += 11
            rows.append((xuid, rank, rating, tag, cols))
        out[view] = (total, rows)
    return out


SKILL_COLS = [stats.COL_SKILL, stats.COL_GAMES, stats.COL_MU, stats.COL_SIGMA]


class StatsTest(unittest.TestCase):
    def setUp(self):
        self.dir = tempfile.TemporaryDirectory()
        stats.configure(self.dir.name)
        self.room = {A, B, C}

    def tearDown(self):
        self.dir.cleanup()

    def write(self, writer, xuid, score, team=-1, now=0.0, **kw):
        stats.write(writer, TAGS.get, write_body(xuid, score, team, **kw), allowed=self.room,
                    now=now)

    def read(self, xuids, specs=((SKILL_VIEW, SKILL_COLS),)):
        return parse_rows(stats.read(A, read_body(xuids, specs), TAGS.get))

    def test_new_players_get_defaults(self):
        total, rows = self.read([A, B])[SKILL_VIEW]
        self.assertEqual(total, 0)
        self.assertEqual([(r[0], r[1], r[3]) for r in rows], [(A, 0, "DriverA"), (B, 0, "DriverB")])
        self.assertEqual(rows[0][4], {61: 1, 62: 0, 63: 3.0, 64: 1.0})

    def test_a_game_is_rated(self):
        # The host writes every player's result; the game is rated once the writes settle.
        self.write(A, A, 1000)
        self.write(A, B, 500)
        self.write(A, C, 10)
        stats.settle(now=1.0)
        self.assertEqual(stats.STORE.players, {})   # not yet
        stats.settle(now=stats.SETTLE + 1)
        total, rows = self.read([C, B, A])[SKILL_VIEW]
        self.assertEqual(total, 3)
        by = {r[0]: r for r in rows}
        mu = {x: by[x][4][63] for x in by}
        self.assertTrue(mu[A] > mu[B] > mu[C], mu)
        self.assertTrue(all(by[x][4][64] < 1.0 for x in by))    # less uncertain
        self.assertEqual({x: by[x][4][62] for x in by}, {A: 1, B: 1, C: 1})
        self.assertEqual([by[x][1] for x in (A, B, C)], [1, 2, 3])  # rank
        self.assertEqual(by[A][2], by[A][4][61])                    # rating = skill
        self.assertEqual(by[A][3], "DriverA")

    def test_every_console_writes_the_same_game(self):
        for writer in (A, B):
            self.write(writer, A, 2)
            self.write(writer, B, 1)
        stats.settle(now=100)
        rows = self.read([A, B])[SKILL_VIEW][1]
        self.assertEqual([r[4][62] for r in rows], [1, 1])

    def test_teams_and_draws(self):
        self.write(A, A, 5, team=0)
        self.write(A, B, 3, team=0)
        self.write(A, C, 4, team=1)
        stats.settle(now=100)
        mu = {r[0]: r[4][63] for r in self.read([A, B, C])[SKILL_VIEW][1]}
        self.assertAlmostEqual(mu[A], mu[B])   # one team, one result
        self.assertTrue(mu[A] > 3.0 > mu[C])
        stats.configure(self.dir.name)
        self.write(A, A, 7, session=1)
        self.write(A, B, 7, session=1)
        stats.settle(now=100)
        mu2 = {r[0]: r[4][63] for r in self.read([A, B])[SKILL_VIEW][1]}
        self.assertAlmostEqual(mu2[A] - mu[A], -(mu2[B] - mu[B]), places=6)  # a draw

    def test_written_ranked_read_standard(self):
        # The game writes 0xFFFF0000 and reads 0xFFFE0000 for its party.
        self.write(A, A, 5)
        self.write(A, B, 1)
        stats.settle(now=100)
        total, rows = self.read([A], [(0xFFFE0000, SKILL_COLS)])[0xFFFE0000]
        self.assertEqual((total, rows[0][4][62]), (2, 1))

    def test_one_player_is_not_rated(self):
        self.write(A, A, 5)
        stats.settle(now=100)
        self.assertEqual(self.read([A])[SKILL_VIEW][1][0][4][62], 0)

    def test_writes_for_players_outside_the_room_ignored(self):
        self.room = {A, B}
        self.write(A, A, 5)
        self.write(A, C, 9)
        stats.settle(now=100)
        self.assertEqual(stats.STORE.players, {})   # only A was left: one player

    def test_kept_across_restarts(self):
        self.write(A, A, 5)
        self.write(A, B, 1)
        stats.settle(now=100)
        before = self.read([A, B])
        stats.configure(self.dir.name)
        self.assertEqual(self.read([A, B]), before)

    def test_other_views_keep_their_columns(self):
        body = struct.pack(">QQB", 0, B, 1) + struct.pack(">IB", 5, 1)
        body += struct.pack(">IB", 0x20000007, stats.TYPE_INT64) + struct.pack(">q", 123456)
        stats.write(A, TAGS.get, body, allowed=self.room)
        total, rows = self.read([A, B], [(5, [7, 8])])[5]
        self.assertEqual(total, 1)
        self.assertEqual(rows[1][4], {7: 123456, 8: None})
        self.assertEqual(rows[0][4], {7: None, 8: None})

    def test_limits(self):
        old = limits.MAX_PLAYERS
        limits.MAX_PLAYERS = 1
        try:
            self.write(A, A, 5)
            self.write(A, B, 1)
            stats.settle(now=100)
            self.assertEqual(len(stats.STORE.players), 1)
        finally:
            limits.MAX_PLAYERS = old
        # an answer fits one datagram however many players are asked for
        many = [0x0009000000001000 + i for i in range(100)]
        reply = stats.read(A, read_body(many, [(SKILL_VIEW, SKILL_COLS)]), TAGS.get)
        self.assertLessEqual(len(reply), stats.MAX_ROWS_BYTES)
        self.assertLess(len(parse_rows(reply)[SKILL_VIEW][1]), 100)

    def test_bad_input(self):
        with self.assertRaises(struct.error):
            stats.write(A, TAGS.get, b"\x00\x01", allowed=self.room)
        with self.assertRaises(ValueError):
            stats.write(A, TAGS.get, write_body(A, 1)[:-3], allowed=self.room)


if __name__ == "__main__":
    unittest.main()
