"""Forza Live: car clubs (Xls.WebServices.Clan, Forza.WebServices.CarClub) and the club
garage's shared cars (Forza.WebServices.SharedCar).

A player creates a club (CreateClan: a tag of up to 4 characters and a name) and leads it;
others find it (GetUsersToJoin lists the clubs by their leader) and ask to join
(RequestJoiningClan: a club that receives requests keeps it until a leader or co-leader
accepts it, ApproveRequestToJoin; an "invite only" club takes none) or are invited
(InviteToClan; AcceptClanInvite). Requests, invitations and the other club news reach the
players as Message Center notifications (forza_notifications.py). Roles, as the game
labels them (sub_8282A260): 1 leader, 2 co-leader (at most 3), 3 member. The player's
club is part of their UserData (clanId, clanTag, roleInClan: GetUserData, the
leaderboards' rows). Members share cars to the club garage (SharedCar.Create: the car's
data, livery and tuning, kept as sent) for the others to drive (GetClubCars,
DownloadSharedCar).

DATA_DIR/clubs.json holds the clubs, members, requests, invitations, history and shared
cars; DATA_DIR/clubs/ the shared cars' liveries, tuning and textures.
"""

import json
import logging
import os
import time

import forza_notifications as notifications
import forza_limits as limits
import forza_xtsw as x
import forza_xtsw_all as A

log = logging.getLogger("forza-live")

DATA_DIR = "forza_live_data"
LEADER, CO_LEADER, MEMBER = 1, 2, 3
MAX_CO_LEADERS = 3
# History actions (Clan.str IDS_Action_*): created, joined, changed role, shared a car,
# quit, removed.
CREATED, JOINED, CHANGED_ROLE, SHARED_CAR, QUIT, REMOVED = range(6)


class Clubs:
    def __init__(self):
        self.data = {"clubs": {}, "members": {}, "nextId": 1, "invites": [], "history": [],
                     "cars": {}}
        self.loaded = False

    def path(self):
        return os.path.join(DATA_DIR, "clubs.json")

    def load(self):
        if not self.loaded:
            self.loaded = True
            try:
                with open(self.path()) as f:
                    self.data.update(json.load(f))
            except (OSError, ValueError):
                pass
        return self.data

    def save(self):
        os.makedirs(DATA_DIR, exist_ok=True)
        tmp = self.path() + ".tmp"
        with open(tmp, "w") as f:
            json.dump(self.data, f, indent=1)
        os.replace(tmp, self.path())

    def new_id(self):
        d = self.load()
        i = d["nextId"]
        d["nextId"] += 1
        return i


CLUBS = Clubs()
PLAYER = None  # forza_live_lsp.load_player
GAME_CARS = None  # Data_Car rows by id, for the club garage's listing


def configure(data_dir, load_player):
    global DATA_DIR, PLAYER
    DATA_DIR = data_dir
    PLAYER = load_player
    CLUBS.__init__()


def key(xuid):
    return "%016X" % xuid


def membership(xuid):
    """(club, role) of a player, or (None, 0)."""
    d = CLUBS.load()
    m = d["members"].get(key(xuid))
    if not m:
        return None, 0
    club = d["clubs"].get(str(m["club"]))
    return (club, m["role"]) if club else (None, 0)


def user_data(xuid, gamertag=None):
    """The player's UserData with their club."""
    record = PLAYER(xuid) if PLAYER else {}
    club, role = membership(xuid)
    u = {"qwXuid": xuid, "wzGamerTag": gamertag or record.get("gamertag", ""),
         "currentCareerLevel": record.get("careerLevel", 0),
         "currentXPTotal": record.get("profile", {}).get("onlineXP", 0)}
    if club:
        u.update(clanId=club["id"], clanTag=club["tag"], roleInClan=role)
    return u


def members_of(club_id):
    return [(int(k, 16), m) for k, m in CLUBS.load()["members"].items() if m["club"] == club_id]


def history(xuid, club, action, role=0):
    d = CLUBS.load()
    d["history"].append({"xuid": key(xuid), "club": club["id"], "clubName": club["name"],
                         "action": action, "role": role, "time": time.time()})
    d["history"] = d["history"][-500:]


def gamertag(xuid):
    return (PLAYER(xuid) if PLAYER else {}).get("gamertag", "") or "%016X" % xuid


def names(club):
    return {"clanName": club["name"], "clanTag": club["tag"]}


def tell(club, ntype, params, roles=(LEADER, CO_LEADER, MEMBER), skip=()):
    """A Message Center notification to the club's members with these roles."""
    for m, r in members_of(club["id"]):
        if r["role"] in roles and m not in skip:
            notifications.send(m, ntype, params)


def join(xuid, club, role=MEMBER):
    CLUBS.load()["members"][key(xuid)] = {"club": club["id"], "role": role, "since": time.time()}
    history(xuid, club, JOINED, role)
    if role != LEADER:
        tell(club, notifications.CLUB_NEW_MEMBER, dict(
            names(club), newMemberXuid=xuid, newMemberGamertag=gamertag(xuid)), skip=(xuid,))


def leave(xuid, removed=False):
    d = CLUBS.load()
    club, role = membership(xuid)
    if not club:
        return
    del d["members"][key(xuid)]
    for car_id, car in list(d["cars"].items()):  # their shared cars leave with them
        if car["owner"] == key(xuid) and car["club"] == club["id"]:
            del d["cars"][car_id]
    history(xuid, club, REMOVED if removed else QUIT)
    if removed:
        notifications.send(xuid, notifications.CLUB_KICKED, dict(
            names(club), kickedXuid=xuid, kickedGamertag=gamertag(xuid)))
    else:
        tell(club, notifications.CLUB_MEMBER_QUIT, dict(
            names(club), exMemberXuid=xuid, exMemberGamertag=gamertag(xuid)),
            roles=(LEADER, CO_LEADER))
    rest = members_of(club["id"])
    if not rest:
        log.info("title: club %d '%s' closed (no members)", club["id"], club["tag"])
        del d["clubs"][str(club["id"])]
    elif role == LEADER:  # the longest-standing co-leader, else member, leads
        heir = min(rest, key=lambda r: (r[1]["role"], r[1]["since"]))
        heir[1]["role"] = LEADER


# ---- Xls.WebServices.User ----

def get_user_data(xuid, body):
    return x.reply(A.UserGetUserData_Results, {"userData": user_data(xuid)})


def accept_clan_invites(xuid, body):
    p, _ = x.decode(A.UserAcceptClanInvites_Params, body)
    CLUBS.load().setdefault("noInvites", {})[key(xuid)] = not p["acceptingClanInvites"]
    CLUBS.save()
    return x.reply()


def accepts_invites(xuid):
    return not CLUBS.load().get("noInvites", {}).get(key(xuid), False)


# ---- Xls.WebServices.Clan ----

def create_clan(xuid, body):
    p, _ = x.decode(A.ClanCreateClan_Params, body)
    d = CLUBS.load()
    tag, name = p["clanTag"].strip(), p["clanName"].strip()
    if membership(xuid)[0]:
        return x.failure(1, "already in a club")
    if not tag or any(c["tag"].lower() == tag.lower() for c in d["clubs"].values()):
        return x.failure(2, "tag taken")
    cid = CLUBS.new_id()
    club = {"id": cid, "tag": tag[:4], "name": name[:20], "description": "",
            "open": True, "created": time.time(), "requests": []}
    d["clubs"][str(cid)] = club
    join(xuid, club, LEADER)
    history(xuid, club, CREATED, LEADER)
    CLUBS.save()
    log.info("title: club %d [%s] '%s' created by %016X", cid, club["tag"], club["name"], xuid)
    return x.reply(A.ClanCreateClan_Results, {"clanId": cid})


def get_clan_status(xuid, body):
    p, _ = x.decode(A.ClanGetClanStatus_Params, body)
    who = p["xuid"] or xuid
    club, _ = membership(who)
    return x.reply(A.ClanGetClanStatus_Results, {"clanStatus": {
        "acceptsClanInvites": accepts_invites(who), "clanId": club["id"] if club else 0}})


def get_clan_members(xuid, body):
    p, _ = x.decode(A.ClanGetClanMembers_Params, body)
    rows = sorted(members_of(p["clanId"]), key=lambda r: (r[1]["role"], r[1]["since"]))
    recent = time.time() - 7 * 86400
    return x.reply(A.ClanGetClanMembers_Results, {
        "clanMembers": [{"user": user_data(m), "recentClanStatusChange": r["since"] > recent}
                        for m, r in rows[:p["maxMembers"]]],
        "totalMembers": len(rows)})


def get_users_to_join(xuid, body):
    """The clubs, each by its leader (the game shows the leader, member count, name and
    whether it is open)."""
    p, _ = x.decode(A.ClanGetUsersToJoin_Params, body)
    out = []
    for club in CLUBS.load()["clubs"].values():
        rows = members_of(club["id"])
        leader = next((m for m, r in rows if r["role"] == LEADER), rows[0][0] if rows else 0)
        out.append({"user": user_data(leader), "clanMemberCount": len(rows),
                    "clanName": club["name"], "clanIsOpenForMembership": club["open"]})
    log.info("title: clubs to join for %016X (filter %d): %d", xuid, p["userFilter"], len(out))
    return x.reply(A.ClanGetUsersToJoin_Results, {"usersInClans": out[:p["maxResults"]]})


def get_users_to_invite(xuid, body):
    """Every player the server knows who is in no club."""
    p, _ = x.decode(A.ClanGetUsersToInvite_Params, body)
    folder = os.path.join(DATA_DIR, "players")
    out = []
    try:
        names = sorted(os.listdir(folder))
    except OSError:
        names = []
    for name in names:
        if not name.endswith(".json"):
            continue
        other = int(name[:-5], 16)
        if other == xuid or membership(other)[0]:
            continue
        out.append({"user": user_data(other), "acceptsClanInvites": accepts_invites(other),
                    "hasEverLoggedIn": True, "clanName": ""})
    return x.reply(A.ClanGetUsersToInvite_Results, {"candidatesForInvite": out[:p["maxResults"]]})


def request_join(xuid, club):
    """A request to join: a club that receives requests (open) keeps it for its leaders and
    co-leaders, who get a CLUB REQUEST in the Message Center (A ACCEPT: ApproveRequestToJoin);
    an invite-only club takes none (the game then hides REQUEST TO JOIN CLUB)."""
    if membership(xuid)[0]:
        return x.failure(1, "already in a club")
    if not club["open"]:
        return x.failure(2, "invite only")
    if key(xuid) not in club["requests"]:
        club["requests"].append(key(xuid))
        log.info("title: %016X asks to join club %d [%s]", xuid, club["id"], club["tag"])
        tell(club, notifications.CLUB_REQUEST, {
            "requestorXuid": xuid, "requestorGamertag": gamertag(xuid), "clanId": club["id"]},
            roles=(LEADER, CO_LEADER))
    CLUBS.save()
    return x.reply()


def request_joining_clan(xuid, body):
    p, _ = x.decode(A.ClanRequestJoiningClan_Params, body)
    club = CLUBS.load()["clubs"].get(str(p["clanId"]))
    return request_join(xuid, club) if club else x.failure(1, "no such club")


def request_joining_clan_by_name(xuid, body):
    p, _ = x.decode(A.ClanRequestJoiningClanByName_Params, body)
    club = next((c for c in CLUBS.load()["clubs"].values()
                 if c["tag"].lower() == p["clanTag"].strip().lower()), None)
    return request_join(xuid, club) if club else x.failure(1, "no such club")


def approve_request(xuid, body):
    p, _ = x.decode(A.ClanApproveRequestToJoin_Params, body)
    club, role = membership(xuid)
    who = key(p["approvedUser"])
    if not club or role not in (LEADER, CO_LEADER) or who not in club["requests"]:
        return x.failure(1, "not allowed")
    club["requests"].remove(who)
    if not membership(p["approvedUser"])[0]:
        join(p["approvedUser"], club)
        notifications.send(p["approvedUser"], notifications.CLUB_ACCEPTED, names(club))
    CLUBS.save()
    return x.reply()


def invite(xuid, invitee):
    club, role = membership(xuid)
    if not club or role not in (LEADER, CO_LEADER):
        return x.failure(1, "not allowed")
    if membership(invitee)[0]:
        return x.failure(2, "already in a club")
    d = CLUBS.load()
    # The invitee accepts it from the Message Center (AcceptClanInvite with the club and,
    # as the invitation, the notification's id).
    nid = notifications.send(invitee, notifications.CLUB_INVITE, dict(
        names(club), sponsorXuid=xuid, sponsorGamertag=gamertag(xuid), clanId=club["id"]))
    d["invites"].append({"id": CLUBS.new_id(), "notification": nid, "club": club["id"],
                         "from": key(xuid), "to": key(invitee), "time": time.time()})
    CLUBS.save()
    log.info("title: %016X invites %016X to club %d", xuid, invitee, club["id"])
    return x.reply()


def invite_to_clan(xuid, body):
    p, _ = x.decode(A.ClanInviteToClan_Params, body)
    return invite(xuid, p["userToInvite"])


def invite_to_clan_by_name(xuid, body):
    p, _ = x.decode(A.ClanInviteToClanByName_Params, body)
    folder = os.path.join(DATA_DIR, "players")
    for name in os.listdir(folder) if os.path.isdir(folder) else []:
        other = int(name[:-5], 16) if name.endswith(".json") else 0
        if other and (PLAYER(other).get("gamertag", "").lower()
                      == p["usernameToInvite"].strip().lower()):
            return invite(xuid, other)
    return x.failure(3, "no such player")


def accept_clan_invite(xuid, body):
    p, _ = x.decode(A.ClanAcceptClanInvite_Params, body)
    d = CLUBS.load()
    inv = next((i for i in d["invites"] if i["to"] == key(xuid) and i["club"] == p["clanId"]
                and (not p["invitationId"]
                     or p["invitationId"] in (i["id"], i.get("notification")))), None)
    club = d["clubs"].get(str(p["clanId"]))
    if not inv or not club or membership(xuid)[0]:
        return x.failure(1, "no invitation")
    d["invites"] = [i for i in d["invites"] if i["to"] != key(xuid)]
    join(xuid, club)
    CLUBS.save()
    return x.reply()


def leave_clan(xuid, body):
    leave(xuid)
    CLUBS.save()
    return x.reply()


def remove_from_clan(xuid, body):
    p, _ = x.decode(A.ClanRemoveFromClan_Params, body)
    club, role = membership(xuid)
    other_club, other_role = membership(p["userToRemove"])
    if not club or other_club is not club or role != LEADER and not (
            role == CO_LEADER and other_role == MEMBER):
        return x.failure(1, "not allowed")
    leave(p["userToRemove"], removed=True)
    CLUBS.save()
    return x.reply()


def change_member_role(xuid, body):
    p, _ = x.decode(A.ClanChangeMemberRole_Params, body)
    club, role = membership(xuid)
    other_club, _ = membership(p["userToChange"])
    if not club or other_club is not club or role != LEADER or p["newRole"] not in (
            LEADER, CO_LEADER, MEMBER):
        return x.failure(1, "not allowed")
    members = CLUBS.load()["members"]
    if p["newRole"] == CO_LEADER and sum(
            1 for _, r in members_of(club["id"]) if r["role"] == CO_LEADER) >= MAX_CO_LEADERS:
        return x.failure(2, "too many co-leaders")
    if p["newRole"] == LEADER:  # make leader: the old leader steps down to co-leader
        members[key(xuid)]["role"] = CO_LEADER
    old_role = members[key(p["userToChange"])]["role"]
    members[key(p["userToChange"])]["role"] = p["newRole"]
    history(p["userToChange"], club, CHANGED_ROLE, p["newRole"])
    if p["newRole"] > old_role:  # 1 leader, 2 co-leader, 3 member: a bigger number is lower
        notifications.send(p["userToChange"], notifications.CLUB_DEMOTION, names(club))
    elif p["newRole"] < old_role:
        tell(club, notifications.CLUB_PROMOTION, dict(
            names(club), newRole=p["newRole"], newPromotedXuid=p["userToChange"],
            newPromotedGamertag=gamertag(p["userToChange"])))
    CLUBS.save()
    return x.reply()


def change_clan_details(xuid, body):
    p, _ = x.decode(A.ClanChangeClanDetails_Params, body)
    club, role = membership(xuid)
    if not club or role not in (LEADER, CO_LEADER):
        return x.failure(1, "not allowed")
    if p["newClanName"].strip():
        club["name"] = p["newClanName"].strip()[:20]
    club["description"] = p["newClanDescription"][:128]
    club["open"] = bool(p["isOpenForMembership"])
    CLUBS.save()
    return x.reply()


def get_clan_history(xuid, body):
    p, _ = x.decode(A.ClanGetClanHistory_Params, body)
    who = key(p["userXuid"] or xuid)
    rows = [h for h in CLUBS.load()["history"] if h["xuid"] == who][-max(0, p["maxEntries"]):]
    return x.reply(A.ClanGetClanHistory_Results, {"userClanHistory": [
        {"clanName": h["clubName"], "date": h["time"], "user": user_data(int(h["xuid"], 16)),
         "role": h["role"], "action": h["action"]} for h in reversed(rows)]})


def recommend_removal(xuid, body):
    p, _ = x.decode(A.ClanRecommendMemberForRemoval_Params, body)
    log.info("title: %016X recommends removing %016X from their club", xuid, p["userToRemove"])
    club, _ = membership(xuid)
    if club and membership(p["userToRemove"])[0] is club:
        tell(club, notifications.CLUB_KICK_RECOMMENDATION, {
            "recommendingRemovalXuid": xuid, "recommendingRemovalGamertag": gamertag(xuid),
            "recommendedForRemovalXuid": p["userToRemove"],
            "recommendedForRemovalGamertag": gamertag(p["userToRemove"]),
            "clanId": club["id"]}, roles=(LEADER,))
    return x.reply()


# ---- Forza.WebServices.CarClub ----

def clan_details(club):
    rows = members_of(club["id"])
    cars = sum(1 for c in CLUBS.load()["cars"].values() if c["club"] == club["id"])
    return {"clanId": club["id"], "clanTag": club["tag"], "clanName": club["name"],
            "clanDescription": club["description"], "isOpenForMembership": club["open"],
            "creationDate": club["created"], "carCount": cars, "memberCount": len(rows)}


def leaders(club, max_leaders):
    rows = sorted(members_of(club["id"]), key=lambda r: (r[1]["role"], r[1]["since"]))
    return [user_data(m) for m, r in rows if r["role"] in (LEADER, CO_LEADER)][:max_leaders]


def get_clan_details(xuid, body):
    p, _ = x.decode(A.ForzaCarClubGetClanDetails_Params, body)
    club = CLUBS.load()["clubs"].get(str(p["clanId"]))
    if not club:
        return x.failure(1, "no such club")
    return x.reply(A.ForzaCarClubGetClanDetails_Results, {"clanDetails": clan_details(club)})


def get_clan_full_info(xuid, body, by_tag=False):
    if by_tag:
        p, _ = x.decode(A.ForzaCarClubGetClanFullInfoByTag_Params, body)
        club = next((c for c in CLUBS.load()["clubs"].values()
                     if c["tag"].lower() == p["clanTag"].strip().lower()), None)
    else:
        p, _ = x.decode(A.ForzaCarClubGetClanFullInfo_Params, body)
        club = CLUBS.load()["clubs"].get(str(p["clanId"]))
    if not club:
        return x.failure(1, "no such club")
    return x.reply(A.ForzaCarClubGetClanFullInfo_Results, {
        "clanDetails": clan_details(club), "leaders": leaders(club, p["maxLeaders"])})


# ---- Forza.WebServices.SharedCar ----

def game_car(model_id):
    global GAME_CARS
    if GAME_CARS is None:
        GAME_CARS = {}
        import forza_world
        if forza_world.GAME_DB and os.path.isfile(forza_world.GAME_DB):
            import sqlite3
            try:
                db = sqlite3.connect("file:%s?mode=ro" % forza_world.GAME_DB, uri=True)
                for row in db.execute("select Id, Year, MakeID, FamilyModelID, FamilyBodyID, "
                                      "CarTypeID, EnginePlacementID, BaseCost from Data_Car"):
                    GAME_CARS[row[0]] = row[1:]
                db.close()
            except sqlite3.Error as e:
                log.warning("club garage: game database: %s", e)
    return GAME_CARS.get(model_id)


def blob_path(name):
    return os.path.join(DATA_DIR, "clubs", name)


def write_blob(name, data):
    limits.accept(len(data))
    os.makedirs(os.path.dirname(blob_path(name)), exist_ok=True)
    with open(blob_path(name), "wb") as f:
        f.write(data)
    return name


def read_blob(name, limit=0):
    try:
        with open(blob_path(name), "rb") as f:
            data = f.read()
    except (OSError, TypeError):
        return b""
    return data[:limit] if limit else data


def shared_car_create(xuid, body, ex=False):
    """SharedCar.Create, or Title Update 4's CreateEx (the car data has hasRallyUpgrades)."""
    p, _ = x.decode(A.ForzaSharedCarCreateEx_Params if ex else A.ForzaSharedCarCreate_Params, body)
    club, _ = membership(xuid)
    if not club:
        return x.failure(1, "not in a club")
    d = CLUBS.load()
    car = p["completeCarData"]
    game_id = car["gameCarId"].hex()
    existing = next((k for k, c in d["cars"].items()
                     if c["owner"] == key(xuid) and c["gameCarId"] == game_id), None)
    car_id = int(existing) if existing and p["overWriteExisting"] else CLUBS.new_id()
    old = d["cars"].get(str(car_id), {})
    if not old and sum(1 for c in d["cars"].values()
                       if c["owner"] == key(xuid)) >= limits.PER_PLAYER["shared_cars"]:
        return x.failure(1, "too many shared cars")
    d["cars"][str(car_id)] = {
        "id": car_id, "club": club["id"], "owner": key(xuid), "gameCarId": game_id,
        "car": {k: (v.hex() if isinstance(v, bytes) else v) for k, v in car.items()},
        "texturesId": p["texturesId"], "shared": old.get("shared", time.time()),
        "used": old.get("used", time.time()), "raceCount": old.get("raceCount", 0),
        "version": old.get("version", 0) + 1,
        "livery": write_blob("car%d_livery.bin" % car_id, p["liveryDetails"]),
        "tuning": write_blob("car%d_tuning.bin" % car_id, p["tuning"])}
    history(xuid, club, SHARED_CAR)
    CLUBS.save()
    log.info("title: %016X shares car %d (model %d) with club %d", xuid, car_id, car["modelId"],
             club["id"])
    return x.reply(A.ForzaSharedCarCreate_Results, {"carId": car_id})


def get_club_cars(xuid, body, ex=False):
    """SharedCar.GetClubCars, or Title Update 4's GetClubCarsEx (rows with hasRallyUpgrades,
    contentId)."""
    p, _ = x.decode(A.ForzaSharedCarGetClubCars_Params, body)
    club, _ = membership(xuid)
    out = []
    for c in CLUBS.load()["cars"].values() if club else []:
        if c["club"] != club["id"]:
            continue
        car = c["car"]
        info = game_car(car["modelId"]) or (0, 0, 0, 0, 0, 0, 0)
        row = {k: car[k] for k in ("modelId", "perfIndex", "simPeakPower", "curbWeight",
                                    "classId", "driveTypeId", "engineConfigId", "cylinderId",
                                    "aspirationTypeId", "displacementId", "isStock",
                                    "speedRating", "accelerationRating", "launchRating",
                                    "brakingRating", "handlingRating", "simPeakTorque",
                                    "weightDistribution")}
        row.update(owner=user_data(int(c["owner"], 16)), sharedDate=c["shared"],
                   lastUsedDate=c["used"], sharedCarId=c["id"], raceCount=c["raceCount"],
                   year=info[0], makeId=info[1], modelFamilyId=info[2], bodyFamilyId=info[3],
                   carTypeId=info[4], enginePlacementId=info[5], baseCost=info[6],
                   version=c["version"], hasRallyUpgrades=car.get("hasRallyUpgrades", 0))
        out.append(row)
    results = A.ForzaSharedCarGetClubCarsEx_Results if ex else A.ForzaSharedCarGetClubCars_Results
    return x.reply(results, {"carData": out[:p["maxResults"]]})


def get_car_details(xuid, body):
    p, _ = x.decode(A.ForzaSharedCarGetCarDetails_Params, body)
    cars = CLUBS.load()["cars"]
    out = []
    for car_id in p["carIds"]:
        c = cars.get(str(car_id))
        if c:
            thumb = read_blob("textures%d_thumb.bin" % c["texturesId"], 230400)
            out.append({"sharedCarId": car_id, "thumbnail": thumb,
                        "manufacturerColorIndex": c["car"]["manufacturerColorIndex"],
                        "carColorRGB": c["car"]["carColorRGB"],
                        "isMetallic": c["car"]["isMetallic"],
                        "specialColorIndex": c["car"]["specialColorIndex"]})
    return x.reply(A.ForzaSharedCarGetCarDetails_Results, {"carData": out})


def download_shared_car(xuid, body):
    p, _ = x.decode(A.ForzaSharedCarDownloadSharedCar_Params, body)
    c = CLUBS.load()["cars"].get(str(p["carId"]))
    if not c:
        return x.failure(1, "no such car")
    c["used"] = time.time()
    CLUBS.save()
    return x.reply(A.ForzaSharedCarDownloadSharedCar_Results, {
        "liveryDetails": read_blob(c["livery"], p["maxLiveryLength"]),
        "tuningDetails": read_blob(c["tuning"], p["maxTuningLength"])})


def increment_race_count(xuid, body):
    p, _ = x.decode(A.ForzaSharedCarIncrementRaceCount_Params, body)
    c = CLUBS.load()["cars"].get(str(p["carId"]))
    if c:
        c["raceCount"] += 1
        c["used"] = time.time()
        CLUBS.save()
    return x.reply()


def remove_shared_car(xuid, body):
    p, _ = x.decode(A.ForzaSharedCarRemoveSharedCar_Params, body)
    d = CLUBS.load()
    c = d["cars"].get(str(p["carId"]))
    club, role = membership(xuid)
    if c and (c["owner"] == key(xuid) or club and club["id"] == c["club"] and role == LEADER):
        del d["cars"][str(p["carId"])]
        CLUBS.save()
    return x.reply()


def upload_car_textures(xuid, body):
    p, _ = x.decode(A.ForzaSharedCarUploadCarTextures_Params, body)
    tid = CLUBS.new_id()
    write_blob("textures%d_livery.bin" % tid, p["liveryTexture"])
    write_blob("textures%d_thumb.bin" % tid, p["LiveryThumbnail"])
    CLUBS.save()
    return x.reply(A.ForzaSharedCarUploadCarTextures_Results, {"texturesId": tid})


def services():
    return {
        ("Xls.User", "GetUserData"): get_user_data,
        ("Xls.User", "AcceptClanInvites"): accept_clan_invites,
        ("Xls.Clan", "CreateClan"): create_clan,
        ("Xls.Clan", "GetClanStatus"): get_clan_status,
        ("Xls.Clan", "GetClanMembers"): get_clan_members,
        ("Xls.Clan", "GetUsersToJoin"): get_users_to_join,
        ("Xls.Clan", "GetUsersToInvite"): get_users_to_invite,
        ("Xls.Clan", "RequestJoiningClan"): request_joining_clan,
        ("Xls.Clan", "RequestJoiningClanByName"): request_joining_clan_by_name,
        ("Xls.Clan", "ApproveRequestToJoin"): approve_request,
        ("Xls.Clan", "InviteToClan"): invite_to_clan,
        ("Xls.Clan", "InviteToClanByName"): invite_to_clan_by_name,
        ("Xls.Clan", "AcceptClanInvite"): accept_clan_invite,
        ("Xls.Clan", "LeaveClan"): leave_clan,
        ("Xls.Clan", "RemoveFromClan"): remove_from_clan,
        ("Xls.Clan", "ChangeMemberRole"): change_member_role,
        ("Xls.Clan", "ChangeClanDetails"): change_clan_details,
        ("Xls.Clan", "GetClanHistory"): get_clan_history,
        ("Xls.Clan", "RecommendMemberForRemoval"): recommend_removal,
        ("CarClub", "GetClanDetails"): get_clan_details,
        ("CarClub", "GetClanFullInfo"): get_clan_full_info,
        ("CarClub", "GetClanFullInfoByTag"): lambda xuid, body: get_clan_full_info(xuid, body, True),
        ("SharedCar", "Create"): shared_car_create,
        ("SharedCar", "GetClubCars"): get_club_cars,
        ("SharedCar", "CreateEx"): lambda xuid, body: shared_car_create(xuid, body, True),
        ("SharedCar", "GetClubCarsEx"): lambda xuid, body: get_club_cars(xuid, body, True),
        ("SharedCar", "GetCarDetails"): get_car_details,
        ("SharedCar", "DownloadSharedCar"): download_shared_car,
        ("SharedCar", "IncrementRaceCount"): increment_race_count,
        ("SharedCar", "RemoveSharedCar"): remove_shared_car,
        ("SharedCar", "UploadCarTextures"): upload_car_textures,
    }
