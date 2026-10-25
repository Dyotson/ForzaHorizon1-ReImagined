"""Co-op campaign (#309, docs/coop_campaign.md): the festival's events as rounds of a
Co-op Festival world.

The Co-op Festival playlist (forza_live_lsp.COOP_HOPPER) is a Horizon World whose
placement only finds worlds led by a friend (forza_live_server.on_match). Its host types
`/event <id or code>` (Events.Id or Events.HorizonEventID, e.g. FR05) in WORLD chat;
forza_chat.Chat hands that to command() and the world's next round becomes the event,
through the world event path (forza_world.Worlds, the client's world_events.cpp).

A campaign round carries the single-player event's settings (campaign_xml):
- the track, laps and circuit / point to point (`Races`), the class (`TargetClass`);
- the car restrictions: `EventRestrictions.Value` decoded the way the game decodes it
  (decode_restrictions), into the bucket's `CarRestrictions`, which the lobby's car check
  and the online AI fill's car pick both use;
- the field: `NumberOfDrivers` CPU drivers (MaxAIPlayers), the grid of
  `CareerRaceModes.xml`'s festival modes (GridOrderingType PIWithPlayerLast);
- the time of day: `TimeOfDayStart` (seconds after midnight) as the nearest of the four
  times the online options can name (HorizonTimeOfDay);
- the crowd (`CrowdSize`, `CrowdExcitement`);
- the cars: ForceOnlyRentalCars 0 (the presets' Overrides force it to 1). With 1, a player's
  car comes from the rental garage (Data_Car.IsRentable: one stock car per class), so an event
  that car doesn't meet ("S class, Audi": the S rental is a Lamborghini) had no car for anyone
  ("NO CARS AVAILABLE") and ended at once. With 0, the game picks a loaner from every installed
  car that meets the restrictions (SELECT Id FROM Restrictions_Data_Car AS Garage WHERE
  Garage.ClassId=6 AND Garage.MakeId=3 ... ORDER BY PerformanceIndex DESC): a player who owns
  none races an Audi R8 GT, which stays out of its garage;
- the CPU drivers' skill: AIDifficulty Medium, and the event's own `AISkillMed` /
  `AITemperamentMed`, which the server sends to every console of the world
  (WORLD_EVENT op 2) and the client's hook writes into each CPU driver (the online
  AI's Easy is stronger than the career's Pro).

EventRestrictions.Value (TU4 sub_826A33B8, base sub_8260A448's decoder; both fill the
76-byte restriction the bucket XML's CarRestrictions fills, TU4 sub_826A8E88): hex, a
40-bit field mask in 5 bytes (byte k holds bits 8k..8k+7), then one big-endian value per
set bit in bit order: u8 ids and comparators (0 Equals, 1 DoesNotEqual), u16 make / model
family / model ids and PI, u32 years and base costs, floats for power and weight. Bits 34
(AllowUpgrades) and 39 (RallyOnly) are flags without a value. A mask of 0 leaves the
defaults (no restriction, upgrades allowed). The game's decoder stores bit 4 (PowerMin) in
PowerMax's place (both write +12); no event uses either.
"""

import logging
import os
import sqlite3
import struct

log = logging.getLogger("forza-live")

# (mask bit, CarRestrictions attribute, value bytes, kind): the fields of TU4's
# sub_826A33B8 in the order it reads them. Bit 1 is unused.
RESTRICTION_FIELDS = [
    (0, "CarClassId", 1, "u"),
    (2, "PIMin", 2, "u"), (3, "PIMax", 2, "u"),
    (4, "PowerMax", 4, "f"),  # PowerMin's bit: the game stores it in PowerMax (see above)
    (5, "PowerMax", 4, "f"),
    (6, "WeightMin", 4, "f"), (7, "WeightMax", 4, "f"),
    (8, "YearMin", 4, "i"), (9, "YearMax", 4, "i"),
    (10, "BodyFamilyComparator", 1, "c"), (11, "BodyFamilyId", 1, "u"),
    (12, "CarTypeComparator", 1, "c"), (13, "CarTypeId", 1, "u"),
    (14, "RegionComparator", 1, "c"), (15, "RegionId", 1, "u"),
    (16, "CountryComparator", 1, "c"), (17, "CountryId", 1, "u"),
    (18, "MakeComparator", 1, "c"), (19, "MakeId", 2, "u"),
    (20, "ModelFamilyComparator", 1, "c"), (21, "ModelFamilyId", 2, "u"),
    (22, "ModelComparator", 1, "c"), (23, "ModelId", 2, "u"),
    (24, "DriveTypeComparator", 1, "c"), (25, "DriveTypeId", 1, "u"),
    (26, "EngineConfigComparator", 1, "c"), (27, "EngineConfigId", 1, "u"),
    (28, "EnginePlacementComparator", 1, "c"), (29, "EnginePlacementId", 1, "u"),
    (30, "CylinderComparator", 1, "c"), (31, "CylinderId", 1, "u"),
    (32, "AspirationComparator", 1, "c"), (33, "AspirationId", 1, "u"),
    (34, "AllowUpgrades", 0, "flag"),
    (35, "FamilySpecialComparator", 1, "c"), (36, "FamilySpecialId", 1, "u"),
    (37, "BaseCostMin", 4, "i"), (38, "BaseCostMax", 4, "i"),
    (39, "RallyOnly", 0, "flag"),
]
COMPARATORS = {0: "Equals", 1: "DoesNotEqual"}

# HorizonTimeOfDay: the online options' four times (TU4 sub_82696DB8: seconds after midnight)
TIMES_OF_DAY = [("Dawn", 28020), ("Midday", 43200), ("Sunset", 63000), ("Midnight", 0)]

GAME_DB = None
# Street races (CareerTypeId 4) are off by default: an online round runs one with its
# checkpoints, but no traffic was seen in it (docs/coop_campaign.md, question 4).
STREET = False
AI_FIELD = False    # test knob (--coop-ai-field): AIFieldId = the event id (docs: question 3)
_events = None
RACE_MULTIPLIERS = [1.0]  # test knob: GetRaceMultipliers for campaign rounds, one per round in turn
_multipliers = {}        # round id -> its multiplier


def decode_restrictions(value):
    """EventRestrictions.Value -> {CarRestrictions attribute: value} for the bits set
    (strings for comparators, "true"/"false" for flags). {} for no restriction."""
    raw = bytes.fromhex(value.strip())
    if len(raw) < 5:
        raise ValueError("restriction %r: no mask" % value)
    mask = sum(raw[k] << (8 * k) for k in range(5))
    if not mask:
        return {}
    out = {"AllowUpgrades": "true" if mask >> 34 & 1 else "false"}
    if mask >> 39 & 1:
        out["RallyOnly"] = "true"
    pos = 5
    for bit, name, size, kind in RESTRICTION_FIELDS:
        if not mask >> bit & 1 or kind == "flag":
            continue
        if pos + size > len(raw):
            raise ValueError("restriction %r: too short for bit %d" % (value, bit))
        chunk = raw[pos:pos + size]
        pos += size
        if kind == "f":
            out[name] = "%f" % struct.unpack(">f", chunk)[0]
        elif kind == "i":
            out[name] = str(struct.unpack(">i", chunk)[0])
        elif kind == "c":
            out[name] = COMPARATORS.get(chunk[0], "Equals")
        else:
            out[name] = str(int.from_bytes(chunk, "big"))
    return out


# A CarRestrictions attribute's Data_Car column (the lobby's loaner pick names them alike);
# the ranges as (column, scale). PowerMin/Max and WeightMin/Max: no event sets them.
CAR_IDS = {"CarClassId": "ClassID", "BodyFamilyId": "FamilyBodyID", "CarTypeId": "CarTypeID",
           "RegionId": "RegionID", "CountryId": "CountryID", "MakeId": "MakeID",
           "ModelFamilyId": "FamilyModelID", "ModelId": "ModelId", "DriveTypeId": "DriveTypeID",
           "EngineConfigId": "EngineConfigID", "EnginePlacementId": "EnginePlacementID",
           "CylinderId": "CylinderID", "AspirationId": "AspirationTypeId",
           "FamilySpecialId": "FamilySpecialID"}
CAR_RANGES = {"Year": ("Year", 1), "BaseCost": ("BaseCost", 1), "PI": ("PerformanceIndex", 1000)}


def car_fits(car, restrictions):
    """A stock Data_Car row (dict) meets a decoded restriction: the ids with their comparators,
    and the year, base cost and PI ranges (0: open)."""
    for attr, column in CAR_IDS.items():
        if attr in restrictions and car.get(column) is not None:
            equal = int(car[column]) == int(restrictions[attr])
            if equal == (restrictions.get(attr.replace("Id", "Comparator")) == "DoesNotEqual"):
                return False
    for attr, (column, scale) in CAR_RANGES.items():
        if car.get(column) is not None:
            value = car[column] * scale
            low, high = float(restrictions.get(attr + "Min", 0)), float(restrictions.get(attr + "Max", 0))
            if (low and value < low) or (high and value > high):
                return False
    return True


def stock_cars(db):
    """Data_Car's cars the lobby may lend (not the unicorns), as dicts; None without the table."""
    columns = sorted(set(CAR_IDS.values()) | {c for c, _ in CAR_RANGES.values()})
    try:
        cursor = db.execute("SELECT %s FROM Data_Car WHERE IsUnicorn = 0" % ", ".join(
            '"%s"' % c for c in columns))
        return [dict(zip(columns, row)) for row in cursor]
    except sqlite3.Error:
        return None


def time_of_day(seconds):
    """The HorizonTimeOfDay name nearest to a time (seconds after midnight)."""
    day = 86400

    def gap(t):
        d = abs((seconds - t) % day)
        return min(d, day - d)
    return min(TIMES_OF_DAY, key=lambda nt: gap(nt[1]))[0]


class Event:
    """A campaign event as a round: the columns of Events, Races and EventRestrictions the
    online options can carry."""

    def __init__(self, row, restrictions, names):
        (self.id, self.code, self.style, self.level, self.career_type, self.track, self.laps,
         self.ribbon, car_class, self.drivers, self.ai_skill, self.ai_temperament,
         self.time_of_day, crowd_size, crowd_excitement, self.cash) = row
        self.car_class = car_class if car_class is not None and 0 <= car_class < 255 else 255
        self.street = self.career_type == 4
        self.preset = 198 if self.street else (1 if self.ribbon == 0 else 2)
        self.laps = max(1, self.laps or 1)
        self.drivers = max(0, min(7, self.drivers or 0))
        self.crowd = (crowd_size if crowd_size is not None else 1.0,
                      crowd_excitement if crowd_excitement is not None else 1.0)
        self.restrictions = restrictions
        self.names = names
        self.loaners = None  # stock cars that meet the event (None: not known)

    def describe(self):
        """A short text for chat: FR05: B class, 1960-1969, 7 CPU drivers, 1 lap, Sunset."""
        return "%s: %s" % (self.code, ", ".join(self.describe_parts()))

    def restriction_text(self):
        """The class and restrictions alone: S class, Audi."""
        return ", ".join(self.describe_parts()[:-3])

    def describe_parts(self):
        parts = [("%s class" % self.names.get(("class", self.car_class)))
                 if ("class", self.car_class) in self.names else "any class"]
        r = self.restrictions
        if "YearMin" in r or "YearMax" in r:
            parts.append("%s-%s" % (r.get("YearMin", "") if r.get("YearMin", "0") != "0" else "",
                                    r.get("YearMax", "") if r.get("YearMax", "0") != "0" else ""))
        for key, kind in (("MakeId", "make"), ("DriveTypeId", "drive"), ("CountryId", "country"),
                          ("RegionId", "region"), ("BodyFamilyId", "body"),
                          ("ModelFamilyId", "model family"), ("ModelId", "model")):
            if key in r:
                neg = r.get(key.replace("Id", "Comparator")) == "DoesNotEqual"
                label = self.names.get((kind, int(r[key])), "%s %s" % (kind, r[key]))
                parts.append(("not " if neg else "") + label)
        if "BaseCostMax" in r and r["BaseCostMax"] != "0":
            parts.append("up to %s CR" % r["BaseCostMax"])
        if "BaseCostMin" in r and r["BaseCostMin"] != "0":
            parts.append("from %s CR" % r["BaseCostMin"])
        if r.get("AllowUpgrades") == "false":
            parts.append("stock")
        parts.append("%d CPU driver%s" % (self.drivers, "" if self.drivers == 1 else "s"))
        if self.street:
            parts.append("street race (no traffic online, so no career result)")
        else:
            parts.append("%d lap%s" % (self.laps, "" if self.laps == 1 else "s"))
        parts.append(time_of_day(self.time_of_day))
        return parts


def load_events(path=None):
    """{Events.Id: Event}: the festival road races (CareerTypeId 1 and 2) and the street races
    (4; offered with STREET), on tracks the game offers online (VisibleInMP), from the game's
    database. Showcases, Nemesis races and the Headline race have flows of their own."""
    global _events
    if _events is not None and path is None:
        return _events
    path = path or GAME_DB
    events = {}
    if path and os.path.isfile(path):
        try:
            db = sqlite3.connect("file:%s?mode=ro" % path, uri=True)
            names = {("class", i): p.replace("CLASS_", "")
                     for i, p in db.execute("SELECT Id, BadgeTexturePathPrefix FROM CarClasses")}
            names.update({("make", i): n for i, n in
                          db.execute("SELECT ID, IconPathBase FROM List_CarMake")})
            names.update({("drive", i): n for i, n in
                          db.execute("SELECT ID, DriveType FROM List_DriveType")})
            restr = {e: v for e, v in db.execute(
                "SELECT EventID, Value FROM EventRestrictions WHERE BucketId = 0")}
            cars = stock_cars(db)
            rows = db.execute(
                "SELECT e.Id, e.HorizonEventID, e.CareerEventStyle, e.Level, e.CareerTypeId, "
                "r.TrackId, r.NumLaps, t.RibbonConfiguration, e.TargetClass, e.NumberOfDrivers, "
                "e.AISkillMed, e.AITemperamentMed, e.TimeOfDayStart, e.CrowdSize, "
                "e.CrowdExcitement, e.CashPrize "
                "FROM Events e JOIN Races r ON r.EventId = e.Id AND r.RaceNumber = 1 "
                "JOIN Tracks t ON t.id = r.TrackId "
                "WHERE e.IsDLC = 0 AND t.VisibleInMP = 1 AND ("
                "(e.CareerTypeId IN (1, 2) AND t.RibbonConfiguration IN (0, 2)) OR "
                "(e.CareerTypeId = 4 AND t.RibbonConfiguration = 6)) ORDER BY e.Id").fetchall()
            db.close()
            for row in rows:
                try:
                    r = decode_restrictions(restr[row[0]]) if row[0] in restr else {}
                except ValueError as e:
                    log.warning("campaign: event %d: %s", row[0], e)
                    continue
                event = events[row[0]] = Event(row, r, names)
                if cars is not None:
                    fit = dict(r, CarClassId=str(event.car_class)) if event.car_class != 255 else r
                    event.loaners = sum(1 for c in cars if car_fits(c, fit))
        except sqlite3.Error as e:
            log.warning("campaign: cannot read %s: %s", path, e)
    _events = events
    return events


def available():
    """The events /event can start, in Events.Id order."""
    return [e for e in load_events().values() if STREET or not e.street]


def find(query):
    """An event by Events.Id or HorizonEventID (any case), or None."""
    q = query.strip()
    events = available()
    if q.isdigit():
        return next((e for e in events if e.id == int(q)), None)
    return next((e for e in events if (e.code or "").lower() == q.lower()), None)


CAMPAIGN_XML = """<GameOptions GameTypePreset="{preset}" GameTypeFilterType="{filter}" TrackId="{track}"
    UseTraffic="{traffic}" CollisionMode="Default" MaxPlayers="{players}" MaxAIPlayers="{ai}"
    AIDifficulty="Medium"{ai_field} NumPlayerBuckets="1" NumTeams="0"
    HorizonTimeOfDayIsRandom="false" HorizonTimeOfDay="{tod}" PauseTimeOfDay="false"
    CrowdSize="{crowd_size:.3f}" CrowdExcitement="{crowd_excitement:.3f}"
    TimePlayerAFKUntilKick="-1.000" MinRecommendedCarClass="255" MaxRecommendedCarClass="255">
  <Buckets>
    <Bucket Name="GROUP 1" CanStartEndOfRaceCountdown="1" MinPlayers="1" MaxPlayers="{players}">
      <CarRestrictions {restrictions}/>
    </Bucket>
  </Buckets>
  <StartingGridOptions StartType="OpenWorld" GridOrderingType="PIWithPlayerLast"/>
  <EndConditionOptions Type="{end}" NumLaps="{laps}" EndRaceCounter="30.0"/>
  <ScoringOptions Type="TimeBased" BiggerIsBetter="0"/>
  <Overrides ForceTeamColors="0" StockUpgradesOrTuning="AllowUpgradesAndTuning" ForceOnlyRentalCars="0" ForceOffSuggestedLine="0" ForceOffSTM="0" ForceOffTCS="0" EasiestBrakingAllowed="Assisted" EasiestSteeringAllowed="Assisted" EasiestShiftingAllowed="Automatic" ForcedCamera="None"/>
</GameOptions>
"""


def campaign_xml(event, laps=0):
    """The round's GameOptions XML (bytes). laps overrides the event's (tests)."""
    attrs = {"CarClassId": str(event.car_class)}
    attrs.update((k, v) for k, v in event.restrictions.items() if k != "CarClassId")
    players = event.drivers + 1
    return CAMPAIGN_XML.format(
        preset=event.preset,
        filter="StreetRace" if event.street else ("Circuit" if event.preset == 1 else "PointToPoint"),
        track=event.track, traffic="true" if event.street else "false", players=players,
        ai=event.drivers, ai_field=' AIFieldId="%d"' % event.id if AI_FIELD else "",
        tod=time_of_day(event.time_of_day), crowd_size=event.crowd[0],
        crowd_excitement=event.crowd[1],
        restrictions=" ".join('%s="%s"' % kv for kv in attrs.items()),
        end="Checkpoint" if event.street else "NumLaps", laps=laps or event.laps).encode()


def ai_message(event, sid):
    """WORLD_EVENT op 2 to every console of the world (world_events.cpp): event id u32,
    world session id (8 bytes), track u32, AI skill u32 and AI temperament u32 (AISkills /
    AITemperaments row ids of the event's Medium columns)."""
    return struct.pack(">BI", 2, event.id) + sid + struct.pack(
        ">III", event.track, event.ai_skill or 0, event.ai_temperament or 0)


def add_arguments(parser):
    parser.add_argument("--coop-street", action="store_true",
                        help="offer the street races in the Co-op Festival's /event list too "
                             "(they run online with their checkpoints, without traffic, so "
                             "their results do not go to the careers)")
    parser.add_argument("--coop-ai-field", action="store_true",
                        help="test knob: campaign rounds name the event as their AIFieldId")
    parser.add_argument("--coop-race-multiplier", default="1",
                        help="test knob: Hopper.GetRaceMultipliers' specialMultiplier for the "
                             "players of campaign rounds, comma separated: one per round in turn")


def configure(args, game_db):
    global GAME_DB, STREET, AI_FIELD, RACE_MULTIPLIERS, _events
    GAME_DB = game_db
    STREET = args.coop_street
    AI_FIELD = args.coop_ai_field
    RACE_MULTIPLIERS = [float(v) for v in str(getattr(args, "coop_race_multiplier", "1")).split(",")
                        if v.strip()] or [1.0]
    _multipliers.clear()
    _events = None
    log.info("co-op campaign: %d events for /event (%s)", len(available()),
             "festival and street races" if STREET else "festival races")


def race_multiplier(round_id):
    """The specialMultiplier of a campaign round's races (RACE_MULTIPLIERS in turn)."""
    if round_id not in _multipliers:
        if len(_multipliers) > 1000:
            _multipliers.clear()
        _multipliers[round_id] = RACE_MULTIPLIERS[len(_multipliers) % len(RACE_MULTIPLIERS)]
    return _multipliers[round_id]


USAGE = ("Co-op Festival: /event <code or id> starts a festival event for this world, "
         "e.g. /event FR05; /events [level] lists them")
# Where /event works, in the game's own menu names (#395: a Custom Free Roam's host was only
# told "You are not in a Horizon World"). One line each: the Chat tab shows one notice.
WHERE = "Multiplayer > Find Game > Co-op Festival"
NOT_IN_WORLD = "/event needs a Co-op Festival world (custom games have none): " + WHERE
IN_HORIZON_WORLD = "/event needs a Co-op Festival world, not the Horizon World: " + WHERE
NOT_WORLD_CHAT = "Type /event in World chat (To: Horizon World), in a Co-op Festival world"


def listing(level=None, width=180):
    """Notices for /events: the festival races' codes per wristband level (0-6), then the
    street races (/events street); each line at most `width` characters."""
    groups = {}
    for e in available():
        key = "Street" if e.street else "Level %d" % e.level if e.level >= 0 else "Intro"
        groups.setdefault(key, []).append(e.code)
    wanted = [k for k in sorted(groups, key=lambda k: (k == "Street", k))
              if level is None or k == ("Street" if level == "street" else "Level %s" % level)]
    lines = []
    for key in wanted:
        line = key + ":"
        for code in sorted(groups[key], key=lambda c: (len(c), c)):
            if len(line) + 1 + len(code) > width and line != key + ":":
                lines.append(line)
                line = key + ":"
            line += " " + code
        lines.append(line)
    return lines or ["No events for that level"]
