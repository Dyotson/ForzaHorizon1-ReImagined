"""Forza Live: scoreboards (leaderboards), Rivals and their ghosts.

The game submits a score at the end of an event (Scoreboard.SubmitScore / SubmitScoreEx:
the scoreboard id of the event, the time or score, the car, its class and performance
index, assists and tuning) and reads scoreboards for its leaderboard screens and for
Rivals (GetRows, GetRival, GetRivalAndPersonalBest, GetBestScores). The answer to a
submission asks for the ghost (uploadFiles) when the score is the player's new best; the
game then uploads it (UploadFile, an opaque blob of up to 250 KB that we keep as it is)
and another player racing that score as a rival downloads it (DownloadFile, by the row's
ghostFileId). docs/title_services.md has the details.

The 1000 Club (Title Update 4) reports each car challenge a player completes
(Challenge.ChallengeCompleted: the car and the challenge's thumbprint); its leaderboard
(GetRows type 5) ranks the players by the challenges (medals) they completed.

Everything is kept in DATA_DIR/scoreboards.json (the best score of each player on each
scoreboard, the 1000 Club medals) and DATA_DIR/ghosts/<id>.bin.
"""

import json
import logging
import os
import time
import uuid

import forza_limits as limits
import forza_xtsw as x
import forza_xtsw_all as A

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"
GHOST_MAX = 256000           # the game's download buffer (DownloadFile maxSize)
RIVAL_BOUNTY = 1000          # credits for beating a rival, shown as BOUNTY BONUS

# Score types: SubmitScore's `type`, GetRows' `type`, GetBestScores' scoreType, NotifyRival's
# `type`. A board is a (type, gameScoreboardId, isServerTimeTrial): the ids overlap, e.g. the
# I-70 speed trap is board 230 and so is the event with Id 230 (docs/title_services.md,
# "Every leaderboard").
SPEED_TRAP = 1      # speed cameras (Colorado/speed_camera.xml, boards 201-230): the speed, m/s
SPEED_ZONE = 2      # average speed zones (Colorado/average_speed.xml, boards 101-112): m/s
EVENT_TIME = 3      # events and Rivals (SubmitScoreEx): the best lap or race time, s
ONLINE_XP_TYPE = 4  # Race Central's ONLINE XP: GetRows(type 4, board 1); SubmitScore type 4
CLUB_TYPE = 5       # the 1000 Club's leaderboard: GetRows(type 5, board 1)
# Types whose scores are better when bigger (speeds, XP); times are better when smaller.
BIGGER_IS_BETTER = {SPEED_TRAP, SPEED_ZONE, ONLINE_XP_TYPE}
KEPT_TYPES = {SPEED_TRAP, SPEED_ZONE, EVENT_TIME}  # submissions kept on a board
GHOST_TYPES = {EVENT_TIME}  # a new best asks for its ghost (laps; a speed trap has none)
# GetRows view 5 ("Friends & Club"): what the speed traps and zones read at the trap.
FRIENDS_AND_CLUB = 5
# Speeds no car reaches (#349), km/h. A fast travel onto a trap recorded the speed the car had
# before the fast travel (fixed in the game by src/hooks/teleport_velocity.cpp); older games
# still send such speeds. A speed trap score above its trap's SPEED_TRAP_LIMIT_KMH is logged
# and not kept: the best known Xbox 360 result on that trap (Cyberscore's Forza Horizon speed
# trap charts, cyberscore.me.uk charts 230405-230426), with no margin. --no-trap-limits
# (ENFORCE_TRAP_LIMITS False, for tests and local servers) only logs them. Speed zones and
# boards without a limit of their own: SPEED_CEILING_KMH, always enforced.
SPEED_CEILING_KMH = 470.0   # fastest results on long straights: about 435-442 km/h
ENFORCE_TRAP_LIMITS = True  # --no-trap-limits turns it off
SPEED_TRAP_LIMIT_KMH = {
    201: 423.1,   # Gladstone
    202: 350.8,   # C-987
    203: 412.6,   # East Rt. 20
    205: 390.3,   # Carson
    206: 233.0,   # Red Rock
    209: 229.1,   # C-203
    210: 267.7,   # Sth Rt. 38
    211: 362.3,   # East Rt. 27
    212: 354.5,   # Rt. 38
    213: 411.2,   # Finley Dam
    214: 340.3,   # Rt. 25
    215: 371.5,   # Beaumont
    216: 275.3,   # West Rt. 27
    218: 436.8,   # I-70
    220: 298.8,   # C-642
    224: 436.9,   # I-76
    225: 439.6,   # Rt. 20
    226: 241.3,   # C-201
    227: 372.9,   # Rt. 27
    228: 348.9,   # Rt. 6
    229: 260.4,   # Rt. 33
    230: 348.3,   # Rt. 29
}
MAX_BOARDS = 4096   # boards on the server (the game has about 160)


def board_key(kind, sbid, stt):
    return "%d:%d:%d" % (kind, sbid, 1 if stt else 0)


class Store:
    """Best score per player per scoreboard, and the ghosts."""

    def __init__(self):
        self.boards = {}       # "type:sbid:stt" -> {xuid hex: entry}
        self.ghosts = {}       # scoreId hex -> (board key, xuid hex): waiting for UploadFile
        self.club = {}         # 1000 Club: xuid hex -> {gamertag, cars: {carId: [thumbprints]}}
        self.next_file = 1
        self.loaded = False

    def path(self):
        return os.path.join(DATA_DIR, "scoreboards.json")

    def load(self):
        if self.loaded:
            return
        self.loaded = True
        try:
            with open(self.path()) as f:
                data = json.load(f)
            self.boards = migrate(data.get("boards", {}))
            self.club = data.get("club1000", {})
            self.next_file = data.get("nextFileId", 1)
        except (OSError, ValueError):
            pass

    def save(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        tmp = self.path() + ".tmp"
        with open(tmp, "w") as f:
            json.dump({"boards": self.boards, "club1000": self.club,
                       "nextFileId": self.next_file}, f, indent=1)
        os.replace(tmp, self.path())

    def board(self, kind, sbid, stt, create=False):
        """A board's entries by player: {} for a board nobody is on, or with create the
        stored board (made when missing; None when the server has MAX_BOARDS)."""
        self.load()
        key = board_key(kind, sbid, stt)
        if create and key not in self.boards:
            if len(self.boards) >= MAX_BOARDS:
                return None
            self.boards[key] = {}
        return self.boards[key] if create else self.boards.get(key, {})

    def ranked(self, kind, sbid, stt):
        """The board's entries, best first."""
        entries = list(self.board(kind, sbid, stt).values())
        bigger = kind in BIGGER_IS_BETTER
        entries.sort(key=lambda e: (-e["score"] if bigger else e["score"], e["submitted"]))
        return entries, bigger


def migrate(boards):
    """Boards kept before the type was part of the key ("sbid:stt", 2026-10): each entry
    goes to the board of its type. Where one player had a speed and a time on the same id,
    only the later submission was kept; it stays."""
    out = {}
    for key, board in boards.items():
        parts = key.split(":")
        if len(parts) == 3:
            out.setdefault(key, {}).update(board)
            continue
        sbid, stt = int(parts[0]), int(parts[1])
        for xuid, e in board.items():
            out.setdefault(board_key(e.get("type", EVENT_TIME), sbid, stt), {})[xuid] = e
    return out


STORE = Store()


def configure(data_dir):
    global DATA_DIR
    DATA_DIR = data_dir
    STORE.__init__()


def better(new, old, bigger):
    return new > old if bigger else new < old


def implausible_speed(xuid, kind, sbid, score):
    """True when a speed on a trap or zone board is not to be kept: above SPEED_CEILING_KMH,
    or above the trap's SPEED_TRAP_LIMIT_KMH with ENFORCE_TRAP_LIMITS. Both are logged."""
    if kind not in (SPEED_TRAP, SPEED_ZONE):
        return False
    kmh = score * 3.6
    if kmh > SPEED_CEILING_KMH:
        log.warning("title: score %s: board %d type %d: %.1f km/h is more than any car "
                    "reaches (%.0f km/h); not kept", public_id(xuid), sbid, kind, kmh,
                    SPEED_CEILING_KMH)
        return True
    limit = SPEED_TRAP_LIMIT_KMH.get(sbid) if kind == SPEED_TRAP else None
    if limit is not None and round(kmh, 1) > limit:   # at the records' precision, 0.1 km/h
        log.warning("title: score %s: speed trap %d: %.1f km/h is above the trap's record "
                    "(%.1f km/h)%s", public_id(xuid), sbid, kmh, limit,
                    "; not kept" if ENFORCE_TRAP_LIMITS else "")
        return ENFORCE_TRAP_LIMITS
    return False


def public_id(xuid):
    """The player as the public leaderboards name them (forza_leaderboards.player_id)."""
    import forza_leaderboards  # it imports this module
    return forza_leaderboards.player_id("%016X" % xuid)


def submit(xuid, gamertag, kind, data, extra=None):
    """Keeps a submission if it is the player's best; returns the reply (scoreId, and
    uploadFiles when we want its ghost)."""
    sbid, stt, score = data["gameScoreboardId"], data["isServerTimeTrial"], data["score"]
    score_id = uuid.uuid4().bytes
    log.info("title: score %016X '%s': board %d%s type %d score %.3f car %d class %d PI %.3f%s",
             xuid, gamertag, sbid, " (time trial)" if stt else "", kind, score, data["carId"],
             data["carClassId"], data["carPerformanceIndex"],
             " passed %d" % extra if extra is not None else "")
    if implausible_speed(xuid, kind, sbid, score):
        return x.reply(x.ScoreboardSubmitScore_Results, {"scoreId": score_id, "uploadFiles": False})
    # Online races submit board -1 (type 4, the XP of the game): not kept.
    board = STORE.board(kind, sbid, stt, create=True) if kind in KEPT_TYPES and sbid >= 0 \
        and 0 < score < 1e9 else None
    if board is None:
        return x.reply(x.ScoreboardSubmitScore_Results, {"scoreId": score_id, "uploadFiles": False})
    key = "%016X" % xuid
    old = board.get(key)
    bigger = kind in BIGGER_IS_BETTER
    if old and not better(score, old["score"], bigger):
        return x.reply(x.ScoreboardSubmitScore_Results, {"scoreId": score_id, "uploadFiles": False})
    if not old and len(board) >= limits.MAX_PLAYERS:
        return x.reply(x.ScoreboardSubmitScore_Results, {"scoreId": score_id, "uploadFiles": False})
    entry = {"xuid": key, "gamertag": limits.clean_gamertag(gamertag, xuid), "type": kind, "score": score,
             "submitted": int(time.time()), "scoreId": score_id.hex(),
             "carId": data["carId"], "carClassId": data["carClassId"],
             "carPerformanceIndex": data["carPerformanceIndex"], "ghostFileId": -1}
    for k in ("usedSTM", "usedTCS", "usedABS", "usedAutoBrake", "usedAutoShifting", "usedClutch"):
        entry[k] = data[k]
    if old and old.get("ghostFileId", -1) >= 0:
        remove_ghost(old["ghostFileId"])
    board[key] = entry
    ghost = kind in GHOST_TYPES
    if ghost:
        STORE.ghosts[score_id.hex()] = (board_key(kind, sbid, stt), key)
    STORE.save()
    log.info("title: score %016X: new best on board %d type %d%s", xuid, sbid, kind,
             ", asking for the ghost" if ghost else "")
    return x.reply(x.ScoreboardSubmitScore_Results, {"scoreId": score_id, "uploadFiles": ghost})


def ghost_path(file_id):
    return os.path.join(DATA_DIR, "ghosts", "%d.bin" % file_id)


def remove_ghost(file_id):
    try:
        os.remove(ghost_path(file_id))
    except OSError:
        pass


def upload_file(xuid, body):
    params, _ = x.decode(x.ScoreboardUploadFile_Params, body)
    sid = params["scoreId"].hex()
    found = STORE.ghosts.pop(sid, None)
    data = params["data"]
    if not found:
        log.info("title: UploadFile %016X: unknown score %s (%d bytes)", xuid, sid, len(data))
        return x.reply()
    board_key, key = found
    limits.accept(len(data))
    entry = STORE.boards.get(board_key, {}).get(key)
    if not entry or entry["scoreId"] != sid:
        return x.reply()  # beaten again since
    file_id = STORE.next_file
    STORE.next_file += 1
    os.makedirs(os.path.dirname(ghost_path(file_id)), exist_ok=True)
    with open(ghost_path(file_id), "wb") as f:
        f.write(data)
    entry["ghostFileId"] = file_id
    STORE.save()
    log.info("title: ghost %d from %016X for board %s (%d bytes)", file_id, xuid, board_key,
             len(data))
    return x.reply()


def download_file(xuid, body):
    params, _ = x.decode(x.ScoreboardDownloadFile_Params, body)
    try:
        with open(ghost_path(params["id"]), "rb") as f:
            data = f.read()
    except OSError:
        log.info("title: DownloadFile %d for %016X: no such file", params["id"], xuid)
        return x.failure(1, "no such file")
    if params["maxSize"] > 0 and len(data) > params["maxSize"]:
        return x.failure(1, "file too big")
    log.info("title: DownloadFile %d for %016X (%d bytes)", params["id"], xuid, len(data))
    return x.reply(x.ScoreboardDownloadFile_Results, {"data": data})


def user_data(entry):
    """The row's player, with their car club when they are in one (forza_clubs)."""
    import forza_clubs
    u = {"qwXuid": int(entry["xuid"], 16), "wzGamerTag": entry["gamertag"],
         "currentCareerLevel": entry.get("level", 0)}
    club, role = forza_clubs.membership(u["qwXuid"])
    if club:
        u.update(clanId=club["id"], clanTag=club["tag"], roleInClan=role)
    return u


def row(entry, position):
    r = {k: entry[k] for k in ("score", "carId", "carClassId", "carPerformanceIndex",
                                "usedSTM", "usedTCS", "usedABS", "usedAutoBrake",
                                "usedAutoShifting", "usedClutch", "ghostFileId")}
    r.update(position=position, rank=position, user=user_data(entry),
             submittedTime=entry["submitted"],
             bounty=RIVAL_BOUNTY)
    return r


NO_ROW = {"position": 0, "rank": 0, "ghostFileId": -1}


def rows_view(entries, xuid, view, start_at, max_results):
    """(rows, the caller's row) for a view: 0 the top, 1 around the caller, 2 friends
    (everyone here), others the top."""
    key = "%016X" % xuid
    mine = next((i for i, e in enumerate(entries) if e["xuid"] == key), None)
    max_results = max(0, min(max_results, 200))
    if view == 1 and mine is not None:
        first = max(0, min(mine - max_results // 2, len(entries) - max_results))
    else:
        first = max(0, start_at - 1)
    rows = [row(e, i + 1) for i, e in enumerate(entries)][first:first + max_results]
    user_row = row(entries[mine], mine + 1) if mine is not None else NO_ROW
    return rows, user_row



# 1000 Club: 218 cars with 5 or 6 challenges each, 1145 in all (Title Update 4's
# gamedb.slt, CarChallenges). The limits keep a bad client from growing the store.
CLUB_MAX_PER_CAR = 8
CLUB_MAX_CARS = 400


def challenge_completed(gamertag_of):
    """Challenge.ChallengeCompleted(carId, challengeThumbprint): a 1000 Club challenge
    completed (a medal). Sent once per completion; a challenge done again in the same car
    sends the same thumbprint. No results."""
    def handler(xuid, body):
        p, _ = x.decode(A.ForzaChallengeChallengeCompleted_Params, body)
        car, thumb = p["carId"], p["challengeThumbprint"]
        STORE.load()
        key = "%016X" % xuid
        entry = STORE.club.setdefault(key, {"xuid": key, "cars": {}})
        entry["gamertag"] = gamertag_of(xuid)
        cars = entry["cars"]
        done = cars.get(str(car), [])
        new = thumb not in done and car != 0 and len(done) < CLUB_MAX_PER_CAR and \
            (str(car) in cars or len(cars) < CLUB_MAX_CARS)
        if new:
            cars[str(car)] = done + [thumb]
            entry.update(carId=car, submitted=int(time.time()))
            STORE.save()
        log.info("title: 1000 Club %016X '%s': challenge %08X in car %d%s, %d medals", xuid,
                 entry["gamertag"], thumb, car, "" if new else " (already counted)",
                 club_medals(entry))
        return x.reply()
    return handler


def club_medals(entry):
    return sum(len(v) for v in entry["cars"].values())


def club_entries():
    """Every player with a 1000 Club medal, most medals first (the first to get there
    first on a tie)."""
    STORE.load()
    out = []
    for e in STORE.club.values():
        medals = club_medals(e)
        if medals:
            out.append({"xuid": e["xuid"], "gamertag": e.get("gamertag", e["xuid"]),
                        "score": float(medals), "submitted": e.get("submitted", 0),
                        "carId": e.get("carId", 0), "carClassId": 0,
                        "carPerformanceIndex": 0.0, "usedSTM": 0, "usedTCS": 0, "usedABS": 0,
                        "usedAutoBrake": 0, "usedAutoShifting": 0, "usedClutch": 0,
                        "ghostFileId": -1})
    out.sort(key=lambda e: (-e["score"], e["submitted"]))
    return out


def club_only(entries, xuid):
    """A club board (isClubBoard): the rows of the caller's car club's members (none when
    the caller is in no club)."""
    import forza_clubs
    club, _ = forza_clubs.membership(xuid)
    if not club:
        return []
    return [e for e in entries
            if (forza_clubs.membership(int(e["xuid"], 16))[0] or {}).get("id") == club["id"]]


def get_rows(xuid, body):
    p, _ = x.decode(x.ScoreboardGetRows_Params, body)
    if p["type"] == ONLINE_XP_TYPE:
        entries = online_xp_entries()
    elif p["type"] == CLUB_TYPE:
        entries = club_entries()
    else:
        entries, _ = STORE.ranked(p["type"], p["gameScoreboardId"], p["isServerTimeTrial"])
    if p["isClubBoard"]:
        entries = club_only(entries, xuid)
    rows, user_row = rows_view(entries, xuid, p["view"], p["startAt"], p["maxResults"])
    # The speed traps and zones (CAmbientChallengeLeaderboard) place a new score by walking up
    # from the caller's position through the rows sent (row[position - 2]): Turn 10's Friends
    # & Club was a small group the caller was in. Here the view sends the top of everyone, so a
    # caller ranked below those rows gets the place right after them (rank keeps the real
    # one); 713 with 200 rows read past the game's array and crashed it (#288).
    if p["view"] == FRIENDS_AND_CLUB and p["type"] in (SPEED_TRAP, SPEED_ZONE) and \
            user_row["position"] > len(rows) + 1:
        user_row = dict(user_row, position=len(rows) + 1)
    log.info("title: GetRows %016X board %d%s type %d view %d%s from %d max %d: %d of %d", xuid,
             p["gameScoreboardId"], " (time trial)" if p["isServerTimeTrial"] else "",
             p["type"], p["view"], " (club)" if p["isClubBoard"] else "", p["startAt"],
             p["maxResults"], len(rows), len(entries))
    return x.reply(x.ScoreboardGetRows_Results, {"rows": rows, "userRow": user_row,
                                                 "available": len(entries),
                                                 "totalRowCount": len(entries)})


# Rollup boards (GetRollupRows: Race Central's LEADERBOARDS, "VIEW OVERALL"): eventTypeId 1
# festival events, 2 street races, 3 showcases; pivotId -1 all, else a wristband level.
# A scoreboard id is the event's Id in the game database (Events: HorizonEventID "FR61",
# Level 6 for board 199, GoPro Millennia Masters).
EVENT_TYPES = {1: "FR", 2: "ST", 3: "EX"}
EVENTS = None  # Id -> (HorizonEventID prefix, wristband level)


def events():
    global EVENTS
    if EVENTS is None:
        EVENTS = {}
        import forza_world
        if forza_world.GAME_DB and os.path.isfile(forza_world.GAME_DB):
            import sqlite3
            try:
                db = sqlite3.connect("file:%s?mode=ro" % forza_world.GAME_DB, uri=True)
                EVENTS = {i: ((h or "")[:2], lvl) for i, h, lvl in
                          db.execute("select Id, HorizonEventID, Level from Events")}
                db.close()
            except sqlite3.Error as e:
                log.warning("scoreboards: game database: %s", e)
    return EVENTS


def in_rollup(key, event_type, pivot):
    kind, sbid, stt = (int(v) for v in key.split(":"))
    if stt or kind != EVENT_TIME:
        return False
    info = events().get(sbid)
    if info is None:
        return not events()  # without the database every board counts
    prefix, level = info
    if event_type in EVENT_TYPES and prefix != EVENT_TYPES[event_type]:
        return False
    return pivot < 0 or level == pivot


def rollup_entries(event_type, pivot):
    """Every player's total over the boards of a rollup: ranked by the number of events
    they have a time on, then by the total of their best times (the score shown)."""
    STORE.load()
    players = {}
    for key, board in STORE.boards.items():
        if not in_rollup(key, event_type, pivot):
            continue
        for e in board.values():
            p = players.setdefault(e["xuid"], dict(e, score=0.0, boards=0))
            p["boards"] += 1
            p["score"] += e["score"]
            p["submitted"] = max(p["submitted"], e["submitted"])
            p["ghostFileId"] = -1
    out = sorted(players.values(), key=lambda e: (-e["boards"], e["score"]))
    return out


def online_xp_entries():
    """Every player by online XP (their profile's onlineXP, kept from UpdateAll)."""
    folder = os.path.join(DATA_DIR, "players")
    out = []
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        try:
            with open(os.path.join(folder, name)) as f:
                record = json.load(f)
        except (OSError, ValueError):
            continue
        xp = record.get("profile", {}).get("onlineXP", 0)
        if xp > 0:
            out.append({"xuid": name[:16], "gamertag": record.get("gamertag", name[:16]),
                        "score": float(xp), "submitted": record.get("profileUpdated", 0),
                        "carId": 0, "carClassId": 0, "carPerformanceIndex": 0.0,
                        "usedSTM": 0, "usedTCS": 0, "usedABS": 0, "usedAutoBrake": 0,
                        "usedAutoShifting": 0, "usedClutch": 0, "ghostFileId": -1,
                        "level": record.get("careerLevel", 0)})
    out.sort(key=lambda e: -e["score"])
    return out


def get_rollup_rows(xuid, body):
    p, _ = x.decode(x.ScoreboardGetRollupRows_Params, body)
    if p["eventTypeId"] in EVENT_TYPES and p["type"] == 0:
        entries = rollup_entries(p["eventTypeId"], p["pivotId"])
    else:
        entries = online_xp_entries()
    rows, user_row = rows_view(entries, xuid, p["view"], p["startAt"], p["maxResults"])
    log.info("title: GetRollupRows %016X type %d view %d pivot %d event type %d: %d rows", xuid,
             p["type"], p["view"], p["pivotId"], p["eventTypeId"], len(rows))
    return x.reply(x.ScoreboardGetRollupRows_Results,
                   {"rows": rows, "userRow": user_row, "available": len(entries)})


def pick_rival(entries, xuid, bigger):
    """The rival for a player: the score just better than theirs; without a score of
    their own, the slowest other player (an easy first rival)."""
    key = "%016X" % xuid
    others = [e for e in entries if e["xuid"] != key]
    if not others:
        return None
    mine = next((e for e in entries if e["xuid"] == key), None)
    if mine is None:
        return others[-1]
    ahead = [e for e in others if better(e["score"], mine["score"], bigger)]
    return ahead[-1] if ahead else others[0]


def rival_reply(xuid, body, with_best):
    p, _ = x.decode(x.ScoreboardGetRival_Params, body)
    # Rivals race events and the server's time trials: their boards are times.
    entries, bigger = STORE.ranked(EVENT_TIME, p["gameScoreboardId"], p["isServerTimeTrial"])
    if p["rivalXuid"]:
        rival = next((e for e in entries if int(e["xuid"], 16) == p["rivalXuid"]), None)
    else:
        rival = pick_rival(entries, xuid, bigger)
    mine = next((e for e in entries if e["xuid"] == "%016X" % xuid), None)
    log.info("title: rival for %016X on board %d (asked %016X): %s", xuid, p["gameScoreboardId"],
             p["rivalXuid"], "%s %.3f ghost %d" % (rival["gamertag"], rival["score"],
                                                  rival["ghostFileId"]) if rival else "none")
    if rival is None:
        result = {"rival": {"scoreData": NO_ROW, "bounty": 0}}
    else:
        result = {"rival": {"scoreData": row(rival, entries.index(rival) + 1),
                            "bounty": RIVAL_BOUNTY}}
    if with_best:
        result["personalBest"] = {"score": mine["score"] if mine else 0.0}
        return x.reply(x.ScoreboardGetRivalAndPersonalBest_Results, result)
    return x.reply(x.ScoreboardGetRival_Results, result)


def get_best_scores(xuid, body):
    p, _ = x.decode(x.ScoreboardGetBestScores_Params, body)
    entries, _ = STORE.ranked(p["scoreType"], p["gameScoreboardId"], p["isServerTimeTrial"])
    best = entries[0]["score"] if entries else 0.0
    log.info("title: GetBestScores %016X board %d type %d car %d class %d: %.3f", xuid,
             p["gameScoreboardId"], p["scoreType"], p["carId"], p["carClassId"], best)
    return x.reply(x.ScoreboardGetBestScores_Results, {"bestScore": {"score": best}})


AMBIENT_NAMES = {SPEED_TRAP: "Speed Trap %d", SPEED_ZONE: "Speed Zone %d"}


def notify_rival(xuid, body):
    """The game says it beat the rival's score: the rival gets a Message Center message
    (Rivals: "<player> beat your time")."""
    import forza_notifications as notifications
    p, _ = x.decode(x.ScoreboardNotifyRival_Params, body)
    log.info("title: %016X beat rival %016X on board %d (type %d)", xuid, p["rivalXuid"],
             p["gameScoreboardId"], p["type"])
    board = STORE.board(p["type"], p["gameScoreboardId"], p["isServerTimeTrial"])
    mine = board.get("%016X" % xuid)
    if not mine or not p["rivalXuid"] or p["rivalXuid"] == xuid:
        return x.reply()
    me = user_data(mine)
    trial = next((t for t in TIME_TRIALS if t[0] == p["gameScoreboardId"]), None) \
        if p["isServerTimeTrial"] else None
    # A speed trap or zone beaten: the game sends its board and type (1 or 2).
    name = trial[2] if trial else ""
    if not trial and p["type"] in AMBIENT_NAMES:
        name = AMBIENT_NAMES[p["type"]] % (p["gameScoreboardId"] % 100)
    notifications.send(p["rivalXuid"], notifications.RIVALS_BEATEN, {
        "senderXuid": xuid, "senderGamertag": mine["gamertag"],
        "senderClanTag": me.get("clanTag", ""), "senderClanRole": me.get("roleInClan", 0),
        "sentTime": time.time(), "timeTrialId": p["gameScoreboardId"],
        "carId": mine["carId"], "localizedTimeTrialName": name,
        "trackName": trial[5] if trial else "", "trackId": trial[4] if trial else 0,
        "score": mine["score"], "scoreType": p["type"], "category": trial[1] if trial else 0})
    return x.reply()


# Race Central's MONTHLY RIVALS: the server's time trials (GetTimeTrials). Each one is a
# scoreboard of its own (isServerTimeTrial, board = the trial id) and a round of
# TIME_TRIAL_HOPPER (Hopper.GetGameOptions(hopper, gameOptionsIndex): its track and laps).
# Categories (TimeTrial.str names FM4's: 0 time, 1 time with traffic, 2 drift, 3 Top Gear,
# 4 time spec, 5 weekly, 6 autocross): the MONTHLY RIVALS screen shows category 4 only
# (its CTimeTrialDataSource filters by the category at +0x20, which that screen sets to 4).
# With the trials in category 0 the screen had nothing to show and kept its YOUR RIVAL,
# BOUNTY and description spinners going.
TIME_TRIAL_HOPPER = 10
MONTHLY_CATEGORY = 4
TIME_TRIALS_ENABLED = False  # --time-trials
TIME_TRIALS = [
    # id, category, name, description, track id, track name, laps, car class (-1 any)
    (1001, MONTHLY_CATEGORY, "Horizon Monthly: Clear Springs", "Two laps of the Clear "
     "Springs Circuit, any car. Beat the other players' times.", 156,
     "Clear Springs Circuit", 2, -1),
    (1002, MONTHLY_CATEGORY, "Horizon Monthly: Festival Sprint", "One lap on the "
     "festival's road course in a class A car.", 322, "Festival Circuit", 1, 5),
]


def time_trial_rounds():
    """{gameOptionsIndex: (track, laps, class)} of TIME_TRIAL_HOPPER."""
    return {i: (t[4], t[6], t[7]) for i, t in enumerate(TIME_TRIALS)}


def get_time_trials(xuid, body):
    p, _ = x.decode(x.ScoreboardGetTimeTrials_Params, body)
    now = time.time()
    month = time.gmtime(now)
    start = time.mktime((month.tm_year, month.tm_mon, 1, 0, 0, 0, 0, 0, 0))
    end = start + 32 * 86400
    cats, trials = [], []
    for i, (tid, cat, name, desc, track, track_name, laps, cls) in enumerate(
            TIME_TRIALS if TIME_TRIALS_ENABLED else []):
        if not cats or cats[-1]["category"] != cat:
            cats.append({"category": cat, "startIndex": len(trials)})
        trials.append({"id": tid, "scoreType": 3, "name": name, "description": desc,
                       "trackId": track, "trackName": track_name,
                       "hopperId": TIME_TRIAL_HOPPER, "gameOptionsIndex": i,
                       "startDate": start, "endDate": end, "userRegistered": True,
                       "featured": i == 0, "active": True, "displayCarClass": cls})
    log.info("title: GetTimeTrials %016X (max %d categories, %d trials): %d", xuid,
             p["maxTimeTrialCategories"], p["maxTimeTrialResults"], len(trials))
    return x.reply(x.ScoreboardGetTimeTrials_Results, {
        "timeTrialCategories": cats[:p["maxTimeTrialCategories"]],
        "timeTrials": trials[:p["maxTimeTrialResults"]]})


def submit_score(gamertag_of, ex):
    def handler(xuid, body):
        if ex:
            p, _ = x.decode(x.ScoreboardSubmitScoreEx_Params, body)
            return submit(xuid, gamertag_of(xuid), p["type"], p["data"]["base"],
                          p["data"]["numCarsPassed"])
        p, _ = x.decode(x.ScoreboardSubmitScore_Params, body)
        return submit(xuid, gamertag_of(xuid), p["type"], p["data"])
    return handler


def services(gamertag_of):
    return {
        ("Scoreboard", "SubmitScore"): submit_score(gamertag_of, False),
        ("Scoreboard", "SubmitScoreEx"): submit_score(gamertag_of, True),
        ("Scoreboard", "UploadFile"): upload_file,
        ("Scoreboard", "DownloadFile"): download_file,
        ("Scoreboard", "GetRows"): get_rows,
        ("Scoreboard", "GetRollupRows"): get_rollup_rows,
        ("Scoreboard", "GetRival"): lambda xuid, body: rival_reply(xuid, body, False),
        ("Scoreboard", "GetRivalAndPersonalBest"): lambda xuid, body: rival_reply(xuid, body, True),
        ("Scoreboard", "GetBestScores"): get_best_scores,
        ("Scoreboard", "NotifyRival"): notify_rival,
        ("Scoreboard", "GetTimeTrials"): get_time_trials,
        ("Challenge", "ChallengeCompleted"): challenge_completed(gamertag_of),  # TU4
    }
