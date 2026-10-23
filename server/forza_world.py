"""Events in the Horizon World (docs/online_vision.md, phase 2).

The Horizon World is Find Game's playlist 3: one endless online free roam per session of
up to 8 players (forza_live_lsp.py serves its rounds, forza_live_server.py places players).
This module turns a world session's next round into one of the game's festival events:

    world (free roam) --event requested--> the host ends the free roam (client hook)
    --> lobby, 30 s wait (client hook) --> the event: the humans with their own cars, the
    free grid slots filled with CPU drivers --> lobby --> world again

How a round's options reach the game (traced in docs/online_vision.md): the session host
asks Hopper.GetGameOptions for the NEXT round when a round starts, so the lobby after a
round already shows the next one. For an event the host asks again: the server sends
WORLD_EVENT to the world's host (forza_live_server.py); its client (world_events.cpp)
re-requests the next round, which we answer with the event, then ends the free roam. The
event round's own prefetch gets the world back.

Events come from the game's database (media/db/gamedb.slt, read-only): festival road races
(Events with CareerTypeId 1 or 2, their first race's track and laps), on tracks the game
offers online. The bucket's car restriction is the event's TargetClass (a CarClasses id);
the detailed EventRestrictions (makes, years, ...) are not decoded yet. Without the database
the four races of the game's own default playlist (HopperRaces) are used, unrestricted.

AI: the game fills an online lobby with CPU drivers itself: the host keeps
min(MaxPlayers - humans, MaxAIPlayers) of them (sub_8287BBE0), so MaxAIPlayers = 7 gives
an 8-car grid at any human count. AIDifficulty follows the human share (ai_difficulty);
the client sets it again from the final count at launch.
"""

import logging
import os
import random
import sqlite3
import time

import forza_campaign

log = logging.getLogger("forza-live")

MAX_PLAYERS = 8
# How long a requested event waits for the world's host to take it (it re-requests the
# next round, which is the event, then ends the free roam).
PENDING_TIMEOUT = 120.0

GAME_DB = None          # path of gamedb.slt, or None
EVENT_MINUTES = 0.0     # schedule: an event every N minutes of a world (0: off)
EVENT_LAPS = 0          # override the events' lap count (tests), 0: the event's own
EVENT_IDS = []          # restrict the rotation to these event ids (empty: all)
EVENT_AI = MAX_PLAYERS - 1  # MaxAIPlayers of an event
EVENT_PLAYERS = MAX_PLAYERS  # an event's grid (MaxPlayers)

# The game's own default playlist (HopperRaces): (event id, preset, track, laps, class)
FALLBACK_EVENTS = [(0, 1, 322, 3, 255), (0, 2, 155, 1, 255), (0, 1, 107, 3, 255),
                   (0, 2, 145, 1, 255)]

_events = None
_rotation = 0


def default_game_db():
    here = os.path.dirname(os.path.abspath(__file__))
    path = os.path.join(here, "..", "extracted", "game", "media", "db", "gamedb.slt")
    return os.path.normpath(path) if os.path.isfile(path) else None


def add_arguments(parser):
    parser.add_argument("--game-db", default=os.environ.get("FORZA_GAME_DB") or default_game_db(),
                        help="the game's media/db/gamedb.slt, for the Horizon World's events "
                             "(default: extracted/game/media/db/gamedb.slt if present)")
    parser.add_argument("--world-event-minutes", type=float, default=EVENT_MINUTES,
                        help="start an event in every Horizon World after N minutes of free "
                             "roam (default %(default)s: only on request, GET /world/event)")
    parser.add_argument("--world-event-laps", type=int, default=0,
                        help="lap count for every event (default: each event's own)")
    parser.add_argument("--world-event-ai", type=int, default=EVENT_AI,
                        help="MaxAIPlayers of an event (default %(default)s: CPU drivers fill "
                             "the grid of 8 at any human count; 0: no CPU drivers)")
    parser.add_argument("--world-event-players", type=int, default=EVENT_PLAYERS,
                        help="an event's grid, humans and CPU drivers (default %(default)s); "
                             "a grid full of humans starts without the wait")
    parser.add_argument("--world-events", default="",
                        help="comma-separated event ids the worlds pick from (default: all "
                             "festival road races)")


def configure(args):
    global GAME_DB, EVENT_MINUTES, EVENT_LAPS, EVENT_IDS, EVENT_AI, EVENT_PLAYERS
    GAME_DB = args.game_db
    EVENT_PLAYERS = max(1, min(MAX_PLAYERS, args.world_event_players))
    EVENT_AI = max(0, min(EVENT_PLAYERS - 1, args.world_event_ai))
    EVENT_MINUTES = max(0.0, args.world_event_minutes)
    EVENT_LAPS = max(0, args.world_event_laps)
    EVENT_IDS = [int(x) for x in args.world_events.split(",") if x.strip()]
    events = load_events()
    log.info("world events: %d from %s, schedule %s", len(events),
             GAME_DB or "the default playlist",
             "every %g min" % EVENT_MINUTES if EVENT_MINUTES else "on request")


def load_events():
    """[(event id, preset, track, laps, car class)] of the festival road races."""
    global _events
    if _events is not None:
        return _events
    events = []
    if GAME_DB and os.path.isfile(GAME_DB):
        try:
            db = sqlite3.connect("file:%s?mode=ro" % GAME_DB, uri=True)
            rows = db.execute(
                "SELECT e.Id, t.RibbonConfiguration, r.TrackId, r.NumLaps, e.TargetClass "
                "FROM Events e JOIN Races r ON r.EventId = e.Id AND r.RaceNumber = 1 "
                "JOIN Tracks t ON t.id = r.TrackId "
                "WHERE e.CareerTypeId IN (1, 2) AND e.IsDLC = 0 AND t.VisibleInMP = 1 "
                "AND t.RibbonConfiguration IN (0, 2) ORDER BY e.Id").fetchall()
            db.close()
            # RibbonConfiguration 0: a circuit (GameTypePreset 1), 2: point to point (2)
            events = [(eid, 1 if ribbon == 0 else 2, track, max(1, laps),
                       cls if cls is not None and 0 <= cls < 255 else 255)
                      for eid, ribbon, track, laps, cls in rows]
        except sqlite3.Error as e:
            log.warning("world events: cannot read %s: %s", GAME_DB, e)
    if EVENT_IDS:
        events = [e for e in events if e[0] in EVENT_IDS]
    _events = events or list(FALLBACK_EVENTS)
    return _events


def pick_event(event_id=None):
    global _rotation
    events = load_events()
    if event_id:
        for e in events:
            if e[0] == event_id:
                return e
    _rotation += 1
    return random.choice(events) if not EVENT_IDS else events[_rotation % len(events)]


def ai_difficulty(humans, grid=MAX_PLAYERS):
    """The CPU drivers' level from the human share of the grid: at most 1/3 humans Easy,
    2/3 or more Hard, in between Medium (8 cars: 1-2 easy, 3-5 medium, 6-8 hard). The
    names are the game's AIDifficulty values (MPAIDifficultyMap rows 0-2). The client's
    world_events.cpp applies the same rule at launch."""
    if 3 * humans <= grid:
        return "Easy"
    if 3 * humans >= 2 * grid:
        return "Hard"
    return "Medium"


EVENT_XML = """<GameOptions GameTypePreset="{preset}" GameTypeFilterType="{filter}" TrackId="{track}"
    UseTraffic="false" CollisionMode="Default" MaxPlayers="{players}" MaxAIPlayers="{ai}"
    AIDifficulty="{difficulty}" NumPlayerBuckets="1" NumTeams="0" HorizonTimeOfDayIsRandom="true"
    HorizonTimeOfDay="Invalid" PauseTimeOfDay="false" TimePlayerAFKUntilKick="-1.000"
    MinRecommendedCarClass="255" MaxRecommendedCarClass="255">
  <Buckets>
    <Bucket Name="GROUP 1" CanStartEndOfRaceCountdown="1" MinPlayers="1" MaxPlayers="{players}">
      <CarRestrictions CarClassId="{car_class}"/>
    </Bucket>
  </Buckets>
  <StartingGridOptions StartType="OpenWorld" GridOrderingType="Random"/>
  <EndConditionOptions Type="NumLaps" NumLaps="{laps}" EndRaceCounter="30.0"/>
  <ScoringOptions Type="TimeBased" BiggerIsBetter="0"/>
</GameOptions>
"""


def event_xml(event, humans):
    eid, preset, track, laps, car_class = event
    return EVENT_XML.format(
        preset=preset, filter="Circuit" if preset == 1 else "PointToPoint", track=track,
        players=EVENT_PLAYERS, ai=EVENT_AI, difficulty=ai_difficulty(humans, EVENT_PLAYERS),
        car_class=car_class, laps=EVENT_LAPS or laps).encode()


class Worlds:
    """Event state per world session (by session id): None, or
    {"event": ..., "state": "pending" (the host is told to take it) | "next" (served as
    the next round), "since": time, "campaign": a forza_campaign.Event or None}. A campaign
    round (the Co-op Festival's /event) is kept in `rounds` for RESULT_WINDOW after it
    started, with the world's members, so their race results can be named (result())."""

    RESULT_WINDOW = 20 * 60.0

    def __init__(self):
        self.state = {}       # session id (bytes) -> event state
        self.started = {}     # session id -> when its current free roam started
        self.members = {}     # session id -> {xuid: gamertag} (forza_live_server.world_tick)
        self.rounds = {}      # session id -> (campaign event, when it started, members)

    def request(self, sid, event_id=None, campaign=None, by=""):
        if campaign is not None:
            event = (campaign.id, campaign.preset, campaign.track, campaign.laps,
                     campaign.car_class)
        else:
            event = pick_event(event_id)
        self.state[sid] = {"event": event, "state": "pending", "since": time.monotonic(),
                           "campaign": campaign, "by": by}
        log.info("world %s: %s %d requested%s (preset %d, track %d, %d laps, class %d)",
                 sid.hex(), "campaign event" if campaign else "event", event[0],
                 " by %s (%s)" % (by, campaign.code) if campaign else "", event[1], event[2],
                 event[3], event[4])
        return event

    def next_round(self, sid, humans):
        """GetGameOptions from the host of world sid: the event's XML, or None (world)."""
        st = self.state.get(sid)
        if st and st["state"] == "pending":
            st["state"] = "next"
            campaign = st.get("campaign")
            if campaign is not None:
                log.info("world %s: next round is campaign event %d (%s), %d human(s), AI "
                         "Medium (skill %s, temperament %s)", sid.hex(), campaign.id,
                         campaign.code, humans, campaign.ai_skill, campaign.ai_temperament)
                return forza_campaign.campaign_xml(campaign, EVENT_LAPS)
            log.info("world %s: next round is event %d, %d human(s), AI %s", sid.hex(),
                     st["event"][0], humans, ai_difficulty(humans))
            return event_xml(st["event"], humans)
        if st and st["state"] == "next":
            # the event round started (its prefetch): the world comes back after it
            log.info("world %s: event %d started; the world is next", sid.hex(),
                     st["event"][0])
            if st.get("campaign") is not None:
                self.rounds[sid] = (st["campaign"], time.monotonic(),
                                    dict(self.members.get(sid, {})), st.get("by", ""))
            del self.state[sid]
        self.started[sid] = time.monotonic()
        return None

    def pending(self, sid):
        st = self.state.get(sid)
        return st["event"] if st and st["state"] == "pending" else None

    def campaigns(self):
        """[(sid, campaign event)] of the campaign rounds asked for and not started yet
        (their CPU drivers' skill goes to every console of the world)."""
        return [(sid, st["campaign"]) for sid, st in self.state.items()
                if st.get("campaign") is not None]

    def result(self, xuid, place, places, finished):
        """A player's race result (User.WriteRaceStatsEx): logged with the campaign round
        of the world it was in, if one started there recently. Returns (sid, event) or
        None."""
        now = time.monotonic()
        for sid, (campaign, since, members, by) in list(self.rounds.items()):
            if now - since > self.RESULT_WINDOW:
                del self.rounds[sid]
                continue
            if xuid in members:
                log.info("co-op: world %s campaign event %d (%s, asked by %s): %s place %d/%d "
                         "finished %d", sid.hex(), campaign.id, campaign.code, by or "?",
                         members[xuid] or "%016X" % xuid, place, places, finished)
                return sid, campaign
        return None

    def tick(self, sids, scheduled=None):
        """Every few seconds with the world sessions that exist: expire stale requests,
        schedule events (EVENT_MINUTES) in the worlds of `scheduled` (default: all; the
        Co-op Festival's worlds only get the events their host asks for). Returns
        [(sid, event)] still waiting for their host."""
        now = time.monotonic()
        for sid in list(self.state):
            st = self.state[sid]
            if sid not in sids or (st["state"] == "pending" and now - st["since"] > PENDING_TIMEOUT):
                if st["state"] == "pending":
                    log.info("world %s: event %d not taken, dropped", sid.hex(), st["event"][0])
                del self.state[sid]
        for sid in list(self.started):
            if sid not in sids:
                del self.started[sid]
        if EVENT_MINUTES:
            for sid in sids if scheduled is None else [s for s in sids if s in scheduled]:
                started = self.started.setdefault(sid, now)
                if sid not in self.state and now - started >= EVENT_MINUTES * 60:
                    self.request(sid)
                    self.started[sid] = now
        return [(sid, st["event"]) for sid, st in self.state.items() if st["state"] == "pending"]


WORLDS = Worlds()
