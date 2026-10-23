"""Forza Live title servers ("LSP"): Turn 10's title services for Forza Horizon.

forza_live_server.py calls handle() for every HTTP request on --http-port that isn't the
status page. The game calls Turn 10's services with POST /Services.Horizon/o.xtsw
(X-ClassName, X-Method; forza_xtsw.py has the format, docs/title_services.md the
services). This answers the ones online play needs:

    Forza.WebServices.User     Logon2 (the "Horizon server" sign-in, with a permission
                               table that allows every feature), GetPermissionsTable,
                               RetrieveOnlineCredits, WriteRaceStats(Ex) (kept per player)
    Forza.WebServices.OnlineProfile  UpdateAll (online XP and credits, kept per player:
                               the game takes its online XP from Logon2), UpdateAllTables
    Forza.WebServices.Hopper   GetList (the Find Game playlists; GetListEx on Title
                               Update 4), GetSearchSettings,
                               GetGameOptions (each round's track and game type),
                               GetSessionListFilterSettings, UserRegister, ReportQosData,
                               ReportVoting, GetRaceMultipliers
    Forza.WebServices.Scoreboard  scores, leaderboards, Rivals and ghosts (forza_scores.py)
    Forza.WebServices.Auction, Gifting  the Auction House and car gifts (forza_market.py)
    Xls.WebServices.Clan, User; Forza.WebServices.CarClub, SharedCar  car clubs and the
                               club garage (forza_clubs.py)
    Forza.WebServices.LargeFileUpload, StorefrontPhoto.Create, Storefront (photos); Xls.WebServices.Content
                               photo uploads and their downloads (forza_photos.py)
    Xls.WebServices.Notifications  the Message Center (forza_notifications.py): Rivals,
                               gifts, auctions and car club messages

Other methods get an empty answer: success and zero bytes, which read as zero, false,
empty strings and empty lists whatever the method's results are (the game ignores bytes
after the results). Every
request is logged (with --dump-requests saved under DATA_DIR/requests); player records go
to DATA_DIR/players/<XUID>.json. Files an operator puts under DATA_DIR/storage/<path> are
served as they are for GET /storage/<path>.
"""

import hmac
import json
import logging
import os
import time
import urllib.parse

import forza_clubs as clubs
import forza_limits as limits
import forza_leaderboards as leaderboards
import forza_market as market
import forza_notifications as notifications
import forza_photos as photos
import forza_scores as scores
import forza_shader_lists as shader_lists
import forza_stats as stats
import forza_world as world
import forza_xtsw as x
import forza_xtsw_all as A

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"
DUMP = False
# The world's rounds are endless: the client (world_drop_in, on by default) runs them as
# an online free roam that later players JUMP IN to, and a free roam has no timed end.
# Clients with world_drop_in off run them as Find Game's race-like free roam, whose lobby
# does not let a player into a game in progress ("WAITING FOR PLAYERS TO FINISH GAME");
# for them a round length (minutes) ends each round on a timer, and the players waiting in
# the lobby load in with everyone at the next round (docs/online_vision.md).
WORLD_ROUND_MINUTES = 0.0

# The playlists "Find Game" offers. Hopper ids are Turn 10's: the game keeps per-hopper
# options in its database (HopperRaces) and media/gametunablesettings
# (Default{Races,Playground}HopperOptions.xml).
# Find Game lists the first category's hoppers; "Horizon World" is its first row.
WORLD_HOPPER = 3
PLAYGROUND_HOPPER_ID = 2
# Find Game lists the first category's hoppers only, so all three are in it.
HOPPER_CATEGORIES = [
    {"name": "Online", "description": "The shared world, races and playground games.",
     "hopperStartIndex": 0},
]
HOPPERS = [
    # The shared world: online free roam (the game's own Quick Match playlist,
    # DefaultPlaygroundHopperOptions.xml, is free roam through matchmaking too). Everyone
    # who picks it is placed into the fullest world session with room (the Forza Live
    # server answers the session search), or hosts a new one.
    {"id": WORLD_HOPPER, "name": "Horizon World",
     "description": "Free roam with everyone online. JUMP IN to a world with room, "
                    "or open a new one and drive at once.",
     "playerCount": 8, "allowVoteToSkip": False, "allowVoteToKick": False,
     "shouldVoteCarClassBeforeLaunch": False, "intermissionTimeMs": 5000},
    {"id": 1, "name": "Races", "description": "We pick the routes, you pick the game type, "
                                              "and everyone votes for the Car class.",
     "playerCount": 8},
    {"id": PLAYGROUND_HOPPER_ID, "name": "Playground Games",
     "description": "Infected, Cat & Mouse and King, in turn.", "playerCount": 8},
]
HOPPER_DEFAULTS = {"allowVoteToSkip": True, "allowVoteToKick": True,
                   "allowVoteToRebalance": False, "payoutModifier": 1.0, "hideTrueSkill": True,
                   # -1: no limit (beginner hoppers set them: a player may join while
                   # minAllowedRaces <= races < maxAllowedRaces and wins < maxAllowedWins)
                   "maxPartySize": 8, "icon": 0, "minAllowedRaces": -1, "maxAllowedRaces": -1,
                   "maxAllowedWins": -1, "shouldVoteCarClassBeforeLaunch": True,
                   "randomizedOrder": False, "intermissionTimeMs": 30000}
SESSION_FILTER = {
    "numStartingMatchmakingQueryResults": 25, "shouldQosProbe": False,
    "shouldReportQosToServer": False, "maxWaitingForQosDataSeconds": 5,
    "maxBpsConsumedDuringQos": 64000, "maxProbesToSend": 4, "maxMedQosRttAllowedMs": 500,
    "maxMinQosRttAllowedMs": 500, "minUpstreamBandwidth": 0, "minDownstreamBandwidth": 0,
    "scoreBonusForJoinInProgress": 0.0, "scoreBonusForJoinInShortIntermission": 0.0,
    "scoreBonusForEmptySlots": 0.0, "rankFactorMinPing": 0.0, "rankFactorAvgPing": 0.0,
    "rankFactorUpstreamBandwidth": 0.0, "rankFactorDownstreamBandwidth": 0.0,
    "minScoreToConnect": -1000000.0}
SEARCH_SETTINGS = {
    "sessionFilterSettings": SESSION_FILTER, "maxConnectAttemptsBeforeCreating": 3,
    "maxConnectSecondsBeforeCreating": 20, "maxWaitingForClientsToConnectSeconds": 20,
    "minPlayersToLaunch": 2, "intermissionTimeMs": 30000, "shortIntermissionTimeMs": 10000}
# The world starts with its first player: nobody waits in a lobby for a second one (this
# and the client's world_drop_in, which runs the launch check a host alone never ran).
WORLD_SEARCH_SETTINGS = dict(SEARCH_SETTINGS, minPlayersToLaunch=1, intermissionTimeMs=5000,
                             shortIntermissionTimeMs=5000, maxConnectAttemptsBeforeCreating=2,
                             maxConnectSecondsBeforeCreating=10)


def hopper_get_search_settings(xuid, body):
    params, _ = x.decode(x.HopperGetSearchSettings_Params, body)
    settings = WORLD_SEARCH_SETTINGS if params["hopperId"] == WORLD_HOPPER else SEARCH_SETTINGS
    return x.reply(x.HopperGetSearchSettings_Results, {"settings": settings})


def add_arguments(parser):
    parser.add_argument("--data-dir", default=os.environ.get("FORZA_LIVE_DATA", "forza_live_data"),
                        help="player records and operator-provided title storage files "
                             "(default ./forza_live_data)")
    parser.add_argument("--dump-requests", action="store_true",
                        help="save every title server request to DATA_DIR/requests")
    parser.add_argument("--playground-games", default="",
                        help="the Playground Games playlist's rounds in turn, as game type "
                             "presets: 4 Infected, 5 Cat & Mouse, 6 King (default: all three, "
                             "4,5,6)")
    parser.add_argument("--test-notifications", action="store_true",
                        help="queue a sample Message Center notification of each kind, a "
                             "credits gift and a car gift for every player who signs in (once "
                             "per server run)")
    parser.add_argument("--time-trials", action="store_true",
                        help="offer the server's Monthly Rivals time trials "
                             "(forza_scores.TIME_TRIALS; a trial needs a score on its board "
                             "to have a rival)")
    parser.add_argument("--enforce-trap-limits", action="store_true",
                        help="drop speed trap scores above the trap's estimated limit "
                             "(forza_scores.SPEED_TRAP_LIMIT_KMH); without it they are only "
                             "logged. 470 km/h on any trap or zone is always enforced")
    parser.add_argument("--world-round-minutes", type=float, default=WORLD_ROUND_MINUTES,
                        help="length of a Horizon World round in minutes (default "
                             "%(default)s: endless). Only for clients with world_drop_in off, "
                             "whose players wait in the lobby until the round ends; with "
                             "drop-in the world is a free roam without a timed end")


def configure(args):
    global DATA_DIR, DUMP, WORLD_ROUND_MINUTES
    DATA_DIR = args.data_dir
    DUMP = args.dump_requests
    WORLD_ROUND_MINUTES = max(0.0, args.world_round_minutes)
    scores.TIME_TRIALS_ENABLED = args.time_trials
    scores.ENFORCE_TRAP_LIMITS = args.enforce_trap_limits
    TEST_NOTIFICATIONS["on"] = args.test_notifications
    if args.playground_games:
        wanted = [p.strip() for p in args.playground_games.split(",") if p.strip()]
        by_preset = {xml.split(b'GameTypePreset="')[1].split(b'"')[0].decode(): xml
                     for xml in load_playground_rounds()}
        PLAYGROUND_ROUNDS[:] = [by_preset[p] for p in wanted if p in by_preset]
    os.makedirs(DATA_DIR, exist_ok=True)
    limits.configure(DATA_DIR)
    scores.configure(DATA_DIR)
    stats.configure(DATA_DIR)
    market.configure(DATA_DIR)
    clubs.configure(DATA_DIR, load_player)
    notifications.configure(DATA_DIR)
    photos.configure(DATA_DIR)
    shader_lists.configure(DATA_DIR)
    leaderboards.configure(DATA_DIR)


def dump(method, path, headers, body):
    folder = os.path.join(DATA_DIR, "requests")
    os.makedirs(folder, exist_ok=True)
    name = "%d-%s-%s" % (int(time.time() * 1000), method,
                         urllib.parse.quote(headers.get("x-method", path), safe="")[:100])
    with open(os.path.join(folder, name), "wb") as f:
        f.write(("\r\n".join(f"{k}: {v}" for k, v in headers.items()) + "\r\n\r\n").encode())
        f.write(body)


def stored_file(path):
    rel = os.path.normpath(path.lstrip("/"))
    if rel.startswith(".."):
        return None
    full = os.path.join(DATA_DIR, rel)
    if os.path.isfile(full):
        with open(full, "rb") as f:
            return f.read()
    return None


# ---- Player records ----

def player_path(xuid):
    return os.path.join(DATA_DIR, "players", "%016X.json" % xuid)


def load_player(xuid):
    try:
        with open(player_path(xuid)) as f:
            return json.load(f)
    except (OSError, ValueError):
        return {}


def save_player(xuid, record):
    path = player_path(xuid)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    if not os.path.exists(path) and len(os.listdir(os.path.dirname(path))) >= limits.MAX_PLAYERS:
        log.warning("player limit reached: %016X not saved", xuid)
        return
    tmp = path + ".tmp"
    with open(tmp, "w") as f:
        json.dump(record, f, indent=1, sort_keys=True)
    os.replace(tmp, path)


# ---- Services ----

# --test-notifications: samples of every kind for each player at their first sign-in.
TEST_NOTIFICATIONS = {"on": False, "sent": set()}
TEST_SENDER = 0x0009000000C0FFEE
TEST_GIFT_CAR = 1103  # Data_Car: 2010 Nissan 370Z

# The game asks the permission table before opening an online feature (Rivals is feature
# 61, others 20, 21, 22, 28, 45, 66, 84, 123...: sub_8264D570 -> sub_82CE97B8 looks the
# feature up by MethodId in the table Logon2 / GetPermissionsTable gave and returns its
# Access as the error code). A feature missing from the table is error 4 and the game says
# "The Forza Horizon server is not available at this time". Everything is allowed here.
PERMISSIONS = [{"MethodId": i, "Tier": 0, "IsEnabled": True, "Access": 0} for i in range(256)]


def user_logon2(xuid, body):
    params, _ = x.decode(x.UserLogon2_Params, body)
    login = params["loginData"]
    xuid = login["qwXuid"] or xuid
    login["wzGamerTag"] = limits.clean_gamertag(login["wzGamerTag"], xuid)
    record = load_player(xuid)
    record.update(gamertag=login["wzGamerTag"], lastLogon=int(time.time()),
                  careerLevel=login["currentCareerLevel"], logons=record.get("logons", 0) + 1)
    save_player(xuid, record)
    log.info("title: Logon2 %016X '%s' level %d", xuid, login["wzGamerTag"],
             login["currentCareerLevel"])
    if TEST_NOTIFICATIONS["on"] and xuid not in TEST_NOTIFICATIONS["sent"]:
        TEST_NOTIFICATIONS["sent"].add(xuid)
        for ntype, params in notifications.samples(TEST_SENDER, "Turn 10").items():
            notifications.send(xuid, ntype, params)
        market.gift_credits(xuid, 10000, "Have fun!")
        market.gift_stock_car(xuid, TEST_GIFT_CAR)
    return x.reply(x.UserLogon2_Results, {"responseData": {
        "isProfileValid": True, "permissions": PERMISSIONS,
        "userData": {"qwXuid": xuid, "wzGamerTag": login["wzGamerTag"],
                     "currentCareerLevel": login["currentCareerLevel"],
                     "currentBadgeId": login["currentBadgeId"],
                     "currentPlayerTitleId": login["currentPlayerTitleId"],
                     "currentXPTotal": record.get("profile", {}).get("onlineXP", 0)},
        # The Message Center's first poll (RetrieveForUser) comes this long after the
        # sign-in; later ones after each answer's nextRetrieveSeconds.
        "notificationNonce": 1,
        "notificationPollIntervalSeconds": notifications.FIRST_POLL_SECONDS,
        "notificationCategories": notifications.CATEGORIES,
        "gameplayLogConfig": {"Enabled": False, "Throttle": bytes(26)},
        "defaultTimeoutMs": 30000, "defaultContentTimeoutMs": 60000}})


def user_write_race_stats(xuid, body):
    head, _ = x.decode(x.UserWriteRaceStats_Head, body)
    stats = head["raceStats"]
    record = load_player(xuid)
    record.update(userLevel=stats["userLevel"], userXP=stats["userXP"],
                  currentCredits=stats["currentCredits"], totalWinnings=stats["totalWinnings"],
                  onlineWinnings=stats["onlineWinnings"],
                  races=record.get("races", 0) + 1, lastRace={
                      k: (v.hex() if isinstance(v, bytes) else v) for k, v in stats.items()})
    save_player(xuid, record)
    log.info("title: race stats %016X: event %d hopper %d place %d/%d finished %d, level %d "
             "XP %d credits %d", xuid, stats["eventId"], stats["hopperId"],
             stats["userFinishPosition"], stats["maxPlaces"], stats["finished"],
             stats["userLevel"], stats["userXP"], stats["currentCredits"])
    return x.reply()


PROFILE_FIELDS = ("level", "xp", "onlineXP", "onlineWristbandLevel", "numOnlineRaces",
                  "numOnlineVictories", "numRaces", "numVictories", "numCreditsEarned",
                  "onlineWinnings", "totalWinnings")


def online_profile_update_all(xuid, body):
    """The profile upload: keeps the player's levels, XP (career and online) and credits."""
    params, _ = x.decode(x.OnlineProfileUpdateAll_Params, body)
    record = load_player(xuid)
    profile = {}
    if params["playerRaceHistory"]:
        race = params["playerRaceHistory"][0]
        profile = {k: race[k] - (1 << 32) if race[k] >= 1 << 31 else race[k]
                   for k in PROFILE_FIELDS}
    if params["playerHistory"]:
        profile["credits"] = params["playerHistory"][0]["credits"]
    # Online XP lives on the server: the game takes it from Logon2 (userData.currentXPTotal)
    # and reports it back here, so the upload right after signing in carries what we gave.
    # It only grows; keep the largest.
    stored = record.get("profile", {})
    for key in ("onlineXP", "onlineWristbandLevel"):
        if key in profile:
            profile[key] = max(profile[key], stored.get(key, 0))
    # An upload carries only the tables that changed: after a Playground Games round without
    # credits won there is no playerHistory, and the stored credits read 0 (#13).
    if profile:
        profile = dict(stored, **profile)
        if profile != record.get("profile"):
            log.info("title: profile %016X: level %d, online XP %d (wristband %d), %d online "
                     "races, %d credits", xuid, profile.get("level", 0),
                     profile.get("onlineXP", 0), profile.get("onlineWristbandLevel", 0),
                     profile.get("numOnlineRaces", 0), profile.get("credits", 0))
        record.update(profile=profile, profileUpdated=int(time.time()))
        save_player(xuid, record)
    return x.reply()


def hopper_get_list(xuid, body):
    params, _ = x.decode(x.HopperGetList_Params, body)
    hoppers = [dict(HOPPER_DEFAULTS, **h) for h in HOPPERS]
    return x.reply(x.HopperGetList_Results, {
        "hopperCategories": HOPPER_CATEGORIES[:params["maxCategoryResults"]],
        "hoppers": hoppers[:params["maxHopperResults"]]})


def hopper_get_list_ex(xuid, body):
    """Title Update 4's GetList: the same playlists, none of them needing a DLC offer."""
    params, _ = x.decode(x.HopperGetList_Params, body)
    hoppers = [dict(HOPPER_DEFAULTS, **h) for h in HOPPERS]
    return x.reply(x.HopperGetListEx_Results, {
        "hopperCategories": HOPPER_CATEGORIES[:params["maxCategoryResults"]],
        "hoppers": hoppers[:params["maxHopperResults"]]})


def hopper_user_register(xuid, body):
    params, _ = x.decode(x.HopperUserRegister_Params, body)
    log.info("title: hopper %d register %016X session %s", params["hopperId"], xuid,
             params["sessionId"].hex())
    return x.reply(x.HopperUserRegister_Results, {"nextRegisterSeconds": 60})


EMPTY_ANSWER = 4096  # zero bytes after the header for methods without a handler

# Each hopper's rounds: (GameTypePreset, TrackId, laps). The game's database lists the
# races of its own default playlist in HopperRaces (1 = circuit, 2 = point to point);
# 7 is the playground game of DefaultPlaygroundHopperOptions.xml.
HOPPER_ROUNDS = {
    1: [(1, 322, 3), (2, 155, 1), (1, 107, 3), (2, 145, 1)],
    2: [(7, 317, 0)],  # unused: PLAYGROUND_ROUNDS
    # Free roam (GameOptionPresets.xml's IDS_PresetFreeRoam) from the festival (track 1101,
    # the custom game's default starting location), no end condition.
    WORLD_HOPPER: [(7, 1101, 0)],
}

# The car class of a round without one of its own: the Horizon World takes any class (-1,
# as the game's IDS_PresetFreeRoam and DefaultPlaygroundHopperOptions.xml do; 255 is not a
# class id and made the lobby accept only S-class cars, #6), the Races playlist keeps 255
# for the lobby's class vote.
ANY_CAR_CLASS = -1
RACES_CAR_CLASS = 255


GAME_OPTIONS_XML = """<GameOptions GameTypePreset="{preset}"{filter} TrackId="{track}" UseTraffic="false"
    CollisionMode="Default" MaxPlayers="{players}" MaxAIPlayers="0" NumPlayerBuckets="1"
    NumTeams="0" HorizonTimeOfDayIsRandom="true" HorizonTimeOfDay="Invalid"
    PauseTimeOfDay="false" MinRecommendedCarClass="{car_class}" MaxRecommendedCarClass="{car_class}">
  <Buckets>
    <Bucket Name="GROUP 1" CanStartEndOfRaceCountdown="1" MinPlayers="1" MaxPlayers="{players}">
      <CarRestrictions CarClassId="{car_class}"/>
    </Bucket>
  </Buckets>
  <StartingGridOptions StartType="OpenWorld" GridOrderingType="Random"/>
  <EndConditionOptions Type="{end}" NumLaps="{laps}"{time} EndRaceCounter="30.0"/>
  <ScoringOptions Type="TimeBased" BiggerIsBetter="0"/>
</GameOptions>
"""


# The Playground Games playlist (hopper 2): the game's own Infected, Cat & Mouse and King
# presets (server/playground_rounds.xml, from GameOptionPresets.xml), one per round.
PLAYGROUND_HOPPER = 2


def load_playground_rounds():
    import xml.etree.ElementTree as ET
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), "playground_rounds.xml")
    try:
        root = ET.parse(path).getroot()
    except (OSError, ET.ParseError) as e:
        log.warning("playground rounds: %s", e)
        return []
    return [ET.tostring(g, encoding="unicode").strip().encode() for g in root]


PLAYGROUND_ROUNDS = load_playground_rounds()


# A Monthly Rivals time trial's options (Scoreboard.GetTimeTrials gives the hopper and
# index): a circuit (preset 1) alone.
TIME_TRIAL_XML = """<GameOptions GameTypePreset="1" GameTypeFilterType="Circuit" TrackId="{track}"
    UseTraffic="false" CollisionMode="Default" MaxPlayers="1" MaxAIPlayers="0"
    NumPlayerBuckets="1" NumTeams="0" HorizonTimeOfDayIsRandom="false" HorizonTimeOfDay="Noon"
    PauseTimeOfDay="true" MinRecommendedCarClass="255" MaxRecommendedCarClass="255">
  <Buckets>
    <Bucket Name="GROUP 1" CanStartEndOfRaceCountdown="1" MinPlayers="1" MaxPlayers="1">
      <CarRestrictions CarClassId="{cls}"/>
    </Bucket>
  </Buckets>
  <StartingGridOptions StartType="Standing" GridOrderingType="Random"/>
  <EndConditionOptions Type="NumLaps" NumLaps="{laps}" EndRaceCounter="30.0"/>
  <ScoringOptions Type="TimeBased" BiggerIsBetter="0"/>
</GameOptions>
"""


def default_round_xml(hopper, round_num):
    """The built-in round (HOPPER_ROUNDS) of a hopper as GameOptions XML bytes."""
    rounds = HOPPER_ROUNDS.get(hopper, HOPPER_ROUNDS[1])
    preset, track, laps = rounds[round_num % len(rounds)]
    players = next((h["playerCount"] for h in HOPPERS if h["id"] == hopper), 8)
    # A free roam (laps 0) has no end, or a timed one (Type "Time", seconds) in the world.
    end, time_attr = ("NumLaps", "") if laps else ("None", "")
    if hopper == WORLD_HOPPER and WORLD_ROUND_MINUTES > 0:
        end, time_attr = "Time", ' Time="%d"' % round(WORLD_ROUND_MINUTES * 60)
    return GAME_OPTIONS_XML.format(
        preset=preset, track=track, laps=max(laps, 1), players=players,
        car_class=ANY_CAR_CLASS if hopper == WORLD_HOPPER else RACES_CAR_CLASS,
        # the custom game's Free Roam preset sets it (GameOptionPresets.xml)
        filter=' GameTypeFilterType="FreeRoam"' if preset == 7 else "",
        end=end, time=time_attr).encode()


def hopper_get_game_options(server, xuid, body):
    """A round's options as GameOptions XML (the format of the game's
    media/gametunablesettings Default*HopperOptions.xml). DATA_DIR/hoppers/<hopperId>.xml
    overrides the built-in rotation (HOPPER_ROUNDS, picked by roundNum). In the Horizon
    World the session host asks for the next round when a round starts, and again when the
    server asked it to run an event (forza_world.py)."""
    params, _ = x.decode(x.HopperGetGameOptions_Params, body)
    hopper, round_num = params["hopperId"], params["roundNum"]
    path = os.path.join(DATA_DIR, "hoppers", "%d.xml" % hopper)
    found = server.world_of(xuid) if hopper == WORLD_HOPPER and server else None
    event_xml = world.WORLDS.next_round(*found) if found else None
    if event_xml:
        xml = event_xml
        log.info("title: hopper %d round %d for %016X: world %s event", hopper, round_num, xuid,
                 found[0].hex())
    elif os.path.isfile(path):
        with open(path, "rb") as f:
            xml = f.read()
    elif hopper == scores.TIME_TRIAL_HOPPER:
        track, laps, cls = scores.time_trial_rounds().get(round_num, scores.time_trial_rounds()[0])
        xml = TIME_TRIAL_XML.format(track=track, laps=laps, cls=cls).encode()
        log.info("title: time trial %d options for %016X: track %d laps %d class %d", round_num,
                 xuid, track, laps, cls)
    elif hopper == PLAYGROUND_HOPPER and PLAYGROUND_ROUNDS:
        # roundNum counts up by one per round from a large start (12259, 12260, ...)
        xml = PLAYGROUND_ROUNDS[round_num % len(PLAYGROUND_ROUNDS)]
        log.info("title: hopper %d round %d for %016X: playground preset %s", hopper, round_num,
                 xuid, xml.split(b'GameTypePreset="')[1].split(b'"')[0].decode())
    else:
        xml = default_round_xml(hopper, round_num)
        log.info("title: hopper %d round %d for %016X: built-in round", hopper, round_num, xuid)
    return x.reply(x.HopperGetGameOptions_Results, {"gameOptionsXml": xml})


def dlc_string_table_info(xuid, body):
    """No DLC string table (#79). The game saves the table this names to its "marketplace"
    save (marketplace:\\<Path>) and downloads it with Content.DownloadContent(Id); with the
    empty answer (Id 0, no Path) it saved a file without a name, that failed, the marketplace
    manager (0x833F4CEC) went to state 3, and its check on the way into Horizon Rally
    (sub_82753DB0) said "MARKETPLACE ERROR: There was not enough space for the marketplace
    save file". Id -1 is the game's own "none": it skips the download and the save."""
    return x.reply(A.ForzaDLCMetadataGetStringTableInfo_Results,
                   {"stringTableInfo": {"base": {"Id": -1}, "DigestId": -1}})


def gamertag_of(xuid):
    return limits.clean_gamertag(load_player(xuid).get("gamertag"), xuid)


SERVICES = {
    ("User", "Logon2"): user_logon2,
    ("User", "GetPermissionsTable"):
        lambda xuid, body: x.reply(x.UserGetPermissionsTable_Results, {"permissions": PERMISSIONS[
            :x.decode(x.UserGetPermissionsTable_Params, body)[0]["maxPermissions"]]}),
    ("User", "RetrieveOnlineCredits"):
        lambda xuid, body: x.reply(x.UserRetrieveOnlineCredits_Results, {"credit": 0}),
    ("User", "WriteRaceStats"): user_write_race_stats,
    ("User", "WriteRaceStatsEx"): user_write_race_stats,
    ("OnlineProfile", "UpdateAll"): online_profile_update_all,
    ("OnlineProfile", "UpdateAllTables"): lambda xuid, body: x.reply(),
    ("Hopper", "GetList"): hopper_get_list,
    ("Hopper", "GetListEx"): hopper_get_list_ex,  # Title Update 4
    ("Hopper", "GetSearchSettings"): hopper_get_search_settings,
    ("Hopper", "GetSessionListFilterSettings"):
        lambda xuid, body: x.reply(x.HopperGetSessionListFilterSettings_Results,
                                   {"settings": SESSION_FILTER}),
    ("Hopper", "UserRegister"): hopper_user_register,
    ("Hopper", "ReportQosData"): lambda xuid, body: x.reply(),
    ("Hopper", "ReportVoting"): lambda xuid, body: x.reply(),
    ("DLCMetadata", "GetStringTableInfo"): dlc_string_table_info,
    ("Hopper", "GetRaceMultipliers"):
        lambda xuid, body: x.reply(x.HopperGetRaceMultipliers_Results,
                                   {"specialMultiplier": 1.0}),
    **scores.services(gamertag_of),
    **market.services(gamertag_of),
    **clubs.services(),
    **notifications.services(),
    **photos.services(),
}


def service_call(headers, body, server=None):
    """bin/xtsw call -> reply body."""
    cls = headers.get("x-classname", "").split(",")[0]          # Forza.WebServices.Hopper
    short = cls.rsplit(".", 1)[-1]
    method = headers.get("x-method", "")
    try:
        xuid = int(headers.get("x-userid", "0"), 16)
    except ValueError:
        xuid = 0
    if cls.startswith("Xls.WebServices."):
        short = "Xls." + short
    elif not cls.startswith("Forza.WebServices."):
        short = None
    fn = SERVICES.get((short, method))
    if (short, method) == ("Hopper", "GetGameOptions"):  # needs the server's world sessions
        fn = lambda xuid, body: hopper_get_game_options(server, xuid, body)  # noqa: E731
    if fn is None:
        log.info("title: %s.%s from %016X (%d bytes): empty answer", cls, method, xuid,
                 len(body))
        return x.reply() + bytes(EMPTY_ANSWER)
    try:
        return fn(xuid, body)
    except ValueError as e:  # parameters that don't parse
        log.warning("title: %s.%s: bad parameters (%s): %s", cls, method, e, body.hex())
        return x.failure(2, "bad parameters")


ADMIN_TOKEN = os.environ.get("FORZA_ADMIN_TOKEN", "")


def is_admin(peer, headers, query):
    """Operator endpoints (/gift, /notify, /world/event): requests from this machine, or
    carrying the FORZA_ADMIN_TOKEN (header X-Admin-Token or ?token=). A request that came
    through a reverse proxy (X-Forwarded-For / X-Real-IP) counts as remote."""
    if ADMIN_TOKEN:
        given = headers.get("x-admin-token") or query.get("token", [""])[0]
        if hmac.compare_digest(given.encode(), ADMIN_TOKEN.encode()):
            return True
    proxied = "x-forwarded-for" in headers or "x-real-ip" in headers or "forwarded" in headers
    # Behind a tunnel agent on this machine every internet request comes from loopback:
    # a trusted proxy's address is not "this machine", only the token admits then.
    return (bool(peer) and peer[0] in ("127.0.0.1", "::1") and not proxied
            and not limits.is_trusted_proxy(peer[0]))


def find_player(who):
    """A player's XUID from a gamertag or a hex XUID (players who signed in here)."""
    folder = os.path.join(DATA_DIR, "players")
    for name in sorted(os.listdir(folder)) if os.path.isdir(folder) else []:
        if name.endswith(".json"):
            xuid = int(name[:-5], 16)
            if who.lower() in (name[:-5].lower(), load_player(xuid).get("gamertag", "").lower()):
                return xuid
    return None


def notify_request(query):
    """GET /notify?to=<gamertag|XUID>&type=<n>[&field=value...] (from this machine only):
    queue a Message Center notification, its parameters from the query (missing ones
    zero); type "sample" queues one of each kind."""
    arg = {k: v[0] for k, v in query.items()}
    to = find_player(arg.pop("to", ""))
    if to is None:
        return 404, "text/plain", b"no such player"
    kind = arg.pop("type", "sample")
    if kind == "sample":
        ids = [notifications.send(to, t, p)
               for t, p in notifications.samples(TEST_SENDER, "Turn 10").items()]
    else:
        ntype = int(kind, 0)
        if ntype not in notifications.TYPES:
            return 400, "text/plain", b"unknown type"
        fields = dict(notifications.TYPES[ntype][1].sub)
        params = {}
        for name, value in arg.items():
            if name in fields:
                kind = fields[name].kind
                params[name] = (float(value) if kind == "F32" else value if kind in (
                    "WSTR", "SYSTIME") else int(value, 0))
        ids = [notifications.send(to, ntype, params)]
    return 200, "application/json", json.dumps({"to": "%016X" % to, "ids": ids}).encode()


def gift_request(query):
    """GET /gift?to=<gamertag|XUID>&credits=<n>[&message=...] or &car=<Data_Car id> (from
    this machine only): a gift the player collects from the Message Center."""
    arg = {k: v[0] for k, v in query.items()}
    to = find_player(arg.get("to", ""))
    if to is None:
        return 404, "text/plain", b"no such player"
    try:
        if "credits" in arg:
            gid = market.gift_credits(to, int(arg["credits"]), arg.get("message", ""))
        elif "car" in arg:
            gid = market.gift_stock_car(to, int(arg["car"]))
        else:
            return 400, "text/plain", b"credits=<n> or car=<id>"
    except ValueError as e:
        return 400, "text/plain", str(e).encode()
    return 200, "application/json", json.dumps({"to": "%016X" % to, "giftId": gid}).encode()


def handle(server, method, path, headers, body, peer):
    """Returns (HTTP status, content type, body bytes)."""
    if DUMP:
        dump(method, path, headers, body)
    url = urllib.parse.urlsplit(path)
    if method == "POST" and url.path.endswith("/o.xtsw"):
        log.info("LSP %s.%s (%d bytes)", headers.get("x-classname", "?").split(",")[0],
                 headers.get("x-method", "?"), len(body))
        return 200, "bin/xtsw", service_call(headers, body, server)
    log.info("LSP request %s %s (%d bytes)", method, path, len(body))
    if url.path.startswith("/shader-lists"):  # players' shader lists (forza_shader_lists.py)
        answer = shader_lists.handle(server, method, path, headers, body)
        if answer is not None:
            return answer
    if url.path.startswith("/leaderboards"):  # public leaderboards as JSON (forza_leaderboards.py)
        answer = leaderboards.handle(method, path)
        if answer is not None:
            return answer
    if url.path in ("/notify", "/gift"):
        query = urllib.parse.parse_qs(url.query)
        if not is_admin(peer, headers, query):
            return 403, "text/plain", b"forbidden"
        return (notify_request if url.path == "/notify" else gift_request)(query)
    if url.path.startswith("/storage/") and method == "GET":
        data = stored_file(url.path)
        if data is not None:
            return 200, "application/octet-stream", data
    return 404, "text/plain", b"not found"
