"""Turn 10's "Xls" title-service protocol (bin/xtsw): framing, codec and message types.

The game calls a service method with an HTTP/1.1 POST to the title server
(/Services.Horizon/o.xtsw, Content-Type: bin/xtsw) whose headers name the class
(X-ClassName "Forza.WebServices.Hopper, Forza.WebServices") and the method (X-Method
"GetList"); X-UserId is the caller's XUID. The body is the method's parameters, the
reply body a 12-byte header followed by the method's results:

    u32 status   0 = success; 2 = failure, the call fails with 0x80510000 | code
    u32 code
    u32 length   of an error message (UTF-8) that follows when status != 0

Parameters and results are records, described in the game by type descriptors (see
docs/title_services.md). The encoding is packed big-endian with no padding, field by
field in declaration order:

    U8, BOOL                     1 byte
    I16, U16                     2 bytes
    I32, U32, F32                4 bytes
    I64, U64                     8 bytes
    GUID                         16 raw bytes
    XNKID                        u32 length (always 8), then the 8 bytes (kinds 16 and 17,
                                 not used here, are the same with 36 and 16 bytes)
    SYSTIME                      a SYSTEMTIME as ISO 8601 text, like a WSTR:
                                 "2026-10-11T15:25:00Z" (the game parses "%4hd-%2hd-%2hd",
                                 "T", "%2hd:%2hd:%2hd", optional ".ms" and "Z"/"+hh:mm";
                                 anything else, the empty string too, fails the whole
                                 reply with 0x8050005C)
    RAW(n)                       n raw bytes (here: records of scalars kept undecoded)
    WSTR(n)                      u32 length, then that many bytes of text (n: buffer size
                                 in characters, including the terminator)
    BYTES(n)                     u32 length, then the bytes
    ARR(type, n)                 u32 count, then the elements
    S(...)                       the fields in order
"""

import struct

import forza_limits as limits


class T:
    """A field type: kind, size or bound (n), element or field list (sub)."""

    def __init__(self, kind, n=0, sub=None):
        self.kind, self.n, self.sub = kind, n, sub


_FIXED = {"U8": ">B", "BOOL": ">B", "I16": ">h", "U16": ">H", "I32": ">i", "U32": ">I",
          "F32": ">f", "I64": ">q", "U64": ">Q"}
_RAW = {"GUID": 16}
_SIZED = {"XNKID": 8}  # u32 length, then exactly that many bytes
SYSTIME_DEFAULT = "2000-01-01T00:00:00Z"


def iso_time(t=None):
    """ISO 8601 UTC text for a Unix time (now by default): the wire form of SYSTEMTIME."""
    import time as _time
    return _time.strftime("%Y-%m-%dT%H:%M:%SZ", _time.gmtime(t))

U8, BOOL, I16, U16, I32, U32, F32, I64, U64 = (T(k) for k in
                                                ("U8", "BOOL", "I16", "U16", "I32", "U32",
                                                 "F32", "I64", "U64"))
XNKID, GUID, SYSTIME = T("XNKID"), T("GUID"), T("SYSTIME")


def RAW(n):
    return T("RAW", n)


def WSTR(n=0):
    return T("WSTR", n)


def BYTES(n=0):
    return T("BYTES", n)


def ARR(elem, n=0):
    return T("ARR", n, elem)


def S(*fields):
    return T("S", 0, list(fields))


def default(t):
    if t.kind in _FIXED:
        return 0.0 if t.kind == "F32" else 0
    if t.kind in _RAW or t.kind in _SIZED or t.kind in ("BYTES", "RAW"):
        return bytes(t.n if t.kind == "RAW" else _RAW.get(t.kind, _SIZED.get(t.kind, 0)))
    if t.kind == "WSTR":
        return ""
    if t.kind == "SYSTIME":
        return SYSTIME_DEFAULT
    if t.kind == "ARR":
        return []
    return {name: default(ft) for name, ft in t.sub}


def _tag(v):
    return limits.clean_gamertag(v)


def _club_tag(v):
    return limits.clean_ascii_text(v, limits.CLUB_TAG_MAX, "CLUB")


def _club_name(v):
    return limits.clean_ascii_text(v, limits.CLUB_NAME_MAX, "Club")


def _club_desc(v):
    return limits.clean_ascii_text(v, limits.CLUB_DESC_MAX)


# Fields holding a player-chosen display string: plain ASCII on the way out (forza_limits).
_PLAYER_TEXT = {"clanTag": _club_tag, "clanName": _club_name, "clanDescription": _club_desc, "senderClanTag": _club_tag,
                "originalOwner": _tag, "creatorName": _tag,
                "SenderGamerTag": _tag, "sellerTag": _tag}
_PLAYER_TEXT.update({n: _tag for n in (
    "wzGamerTag", "senderGamertag", "requestorGamertag", "sponsorGamertag", "playerGamertag",
    "recommendingRemovalGamertag", "recommendedForRemovalGamertag", "newMemberGamertag",
    "kickedGamertag", "exMemberGamertag", "newPromotedGamertag", "gamerTag", "GamerTag",
    "gamertag")})


def encode(t, v, out=None):
    """Appends value v of type t to bytearray out (a dict for S; missing fields default)."""
    out = bytearray() if out is None else out
    k = t.kind
    if k in _FIXED:
        out += struct.pack(_FIXED[k], v)
    elif k in _RAW or k == "RAW":
        n = t.n if k == "RAW" else _RAW[k]
        out += bytes(v).ljust(n, b"\0")[:n]
    elif k in _SIZED:
        n = _SIZED[k]
        out += struct.pack(">I", n) + bytes(v).ljust(n, b"\0")[:n]
    elif k == "SYSTIME":
        b = (v if isinstance(v, str) else iso_time(v)).encode("ascii")
        out += struct.pack(">I", len(b)) + b
    elif k == "WSTR":
        b = v.encode("utf-8")
        if t.n:
            b = b[:t.n - 1]
        out += struct.pack(">I", len(b)) + b
    elif k == "BYTES":
        b = bytes(v)
        out += struct.pack(">I", len(b)) + b
    elif k == "ARR":
        items = list(v)[:t.n] if t.n else list(v)
        out += struct.pack(">I", len(items))
        for item in items:
            encode(t.sub, item, out)
    else:
        v = v or {}
        for name, ft in t.sub:
            val = v[name] if name in v else default(ft)
            if ft.kind == "WSTR" and val and name in _PLAYER_TEXT:  # stored data can't reach the game unclean
                val = _PLAYER_TEXT[name](val)
            encode(ft, val, out)
    return out


def decode(t, data, pos=0):
    """Returns (value, new position); raises ValueError on short data."""
    k = t.kind

    def take(n):
        if pos + n > len(data):
            raise ValueError("short data")
        return data[pos:pos + n]

    if k in _FIXED:
        size = struct.calcsize(_FIXED[k])
        return struct.unpack(_FIXED[k], take(size))[0], pos + size
    if k in _RAW or k == "RAW":
        n = t.n if k == "RAW" else _RAW[k]
        return take(n), pos + n
    if k in _SIZED:
        (n,) = struct.unpack(">I", take(4))
        if n != _SIZED[k]:
            raise ValueError("%s of %d bytes" % (k, n))
        pos += 4
        return take(n), pos + n
    if k in ("WSTR", "BYTES", "SYSTIME"):
        (n,) = struct.unpack(">I", take(4))
        pos += 4
        raw = take(n)
        return (raw if k == "BYTES" else raw.decode("utf-8", "replace")), pos + n
    if k == "ARR":
        (n,) = struct.unpack(">I", take(4))
        pos += 4
        if n > len(data) - pos:  # every element takes a byte: no huge loops from a length field
            raise ValueError("array of %d elements" % n)
        items = []
        for _ in range(n):
            item, pos = decode(t.sub, data, pos)
            items.append(item)
        return items, pos
    value = {}
    for name, ft in t.sub:
        value[name], pos = decode(ft, data, pos)
    return value, pos


def reply(results_type=None, results=None):
    """A success reply: the header, then the results (if the method has any)."""
    body = bytearray(struct.pack(">III", 0, 0, 0))
    if results_type is not None:
        encode(results_type, results or {}, body)
    return bytes(body)


def failure(code, message=""):
    """A failure reply: the game's call returns 0x80510000 | code."""
    msg = message.encode("utf-8")
    return struct.pack(">III", 2, code & 0xFFFF, len(msg)) + msg


# ---- Message types (field names and order from the game's type descriptors) ----

MethodPermission = S(("MethodId", I32), ("Tier", I32), ("IsEnabled", BOOL), ("Access", I32))

# Forza.WebServices.User
UserLogon2_Params = S(("loginData", S(
    ("qwXuid", U64), ("wzGamerTag", WSTR(16)), ("userTier", I32), ("language", I32),
    ("region", I32), ("locale", I32), ("LCID", I32), ("ipAddress", U32), ("machineId", U64),
    ("macAddress", BYTES(6)), ("ageGroup", U8),
    ("clientProfile", S(("ProfileId", BYTES(24)), ("TransactionCount", I32))),
    ("titleVersion", I32), ("currentCareerLevel", U8), ("currentBadgeId", I16),
    ("currentPlayerTitleId", I16), ("friendXuids", ARR(U64, 100)), ("gameCarIds", ARR(GUID, 500)))))
UserData = S(("qwXuid", U64), ("wzGamerTag", WSTR(16)), ("flags", U8), ("clanId", I32),
             ("clanTag", WSTR(5)), ("roleInClan", I32), ("currentCareerLevel", U8),
             ("currentBadgeId", I16), ("currentPlayerTitleId", I16), ("currentXPTotal", U32))
UserLogon2_Results = S(("responseData", S(
    ("isProfileValid", BOOL), ("lastOnlineProfileUpdateAllTablesTranId", I32),
    ("permissions", ARR(MethodPermission, 256)), ("featureAreasBanMask", U32),
    ("userFlags", U8), ("upsellOfferId", I64), ("upsellText", WSTR(301)),
    ("vipOfferIds", ARR(I64, 16)), ("userData", UserData), ("notificationNonce", U16),
    ("notificationPollIntervalSeconds", I32),
    ("notificationCategories", ARR(S(("type", I32), ("name", WSTR(64)),
                                     ("description", WSTR(256)),
                                     ("backgroundImageFileId", I32)), 5)),
    ("storefrontCap", I32),
    ("gameplayLogConfig", S(("Enabled", BOOL), ("FlushInterval", U32), ("Throttle", BYTES(26)),
                            ("HeartBeatInterval", U32))),
    ("carsToUnshareFromGame", ARR(GUID, 500)), ("defaultTimeoutMs", I32),
    ("defaultContentTimeoutMs", I32))))
UserGetPermissionsTable_Params = S(("maxPermissions", U32))
UserGetPermissionsTable_Results = S(("permissions", ARR(MethodPermission)),
                                    ("featureAreasBanMask", U32), ("userFlags", U8))
UserRetrieveOnlineCredits_Results = S(("credit", U32))
RaceStats = S(("finished", BOOL), ("uiNavigationArea", U8), ("userFinishPosition", U8),
              ("userScore", F32), ("winLossMargin", F32), ("biggerIsBetter", BOOL),
              ("endConditionType", U8), ("maxPlaces", U8), ("numHumans", U8), ("numAI", U8),
              ("secondsInRace", F32), ("eventId", U16), ("hopperId", U16),
              ("damageDifficultyType", U8), ("collisionMode", U8), ("hadLivery", BOOL),
              ("timeDriven", F32), ("distanceDriven", F32), ("topSpeed", F32),
              ("sessionId", XNKID), ("partySessionId", XNKID), ("userLevel", U8),
              ("userXP", U32), ("totalWinnings", U32), ("onlineWinnings", U32),
              ("currentCredits", U32))
# WriteRaceStats(Ex) parameters begin with these; scoreData (car, tuning, assists) follows.
UserWriteRaceStats_Head = S(("id", U32), ("raceStats", RaceStats))

# Forza.WebServices.Hopper
HopperGetList_Params = S(("maxCategoryResults", U32), ("maxHopperResults", U32))
HopperCategory = S(("name", WSTR(64)), ("description", WSTR(1024)), ("hopperStartIndex", U32))
HopperData = S(("id", I32), ("name", WSTR(64)), ("description", WSTR(1024)),
               ("playerCount", I32), ("allowVoteToSkip", BOOL), ("allowVoteToKick", BOOL),
               ("allowVoteToRebalance", BOOL), ("payoutModifier", F32), ("hideTrueSkill", BOOL),
               ("maxPartySize", U8), ("icon", I32), ("minAllowedRaces", I32),
               ("maxAllowedRaces", I32), ("maxAllowedWins", I32),
               ("shouldVoteCarClassBeforeLaunch", BOOL), ("randomizedOrder", BOOL),
               ("intermissionTimeMs", I32))
HopperGetList_Results = S(("hopperCategories", ARR(HopperCategory)), ("hoppers", ARR(HopperData)))
# Title Update 4 asks with GetListEx: each hopper also names the offer (DLC) it needs (0: none).
HopperDataEx = S(*HopperData.sub, ("requiredOfferId", U64))
HopperGetListEx_Results = S(("hopperCategories", ARR(HopperCategory)),
                            ("hoppers", ARR(HopperDataEx)))
SessionFilterSettings = S(
    ("numStartingMatchmakingQueryResults", U32), ("shouldQosProbe", BOOL),
    ("shouldReportQosToServer", BOOL), ("maxWaitingForQosDataSeconds", U32),
    ("maxBpsConsumedDuringQos", U32), ("maxProbesToSend", U32), ("maxMedQosRttAllowedMs", U32),
    ("maxMinQosRttAllowedMs", U32), ("minUpstreamBandwidth", U32),
    ("minDownstreamBandwidth", U32), ("scoreBonusForJoinInProgress", F32),
    ("scoreBonusForJoinInShortIntermission", F32), ("scoreBonusForEmptySlots", F32),
    ("rankFactorMinPing", F32), ("rankFactorAvgPing", F32), ("rankFactorUpstreamBandwidth", F32),
    ("rankFactorDownstreamBandwidth", F32), ("minScoreToConnect", F32))
HopperGetSearchSettings_Params = S(("hopperId", I32))
HopperGetSearchSettings_Results = S(("settings", S(
    ("sessionFilterSettings", SessionFilterSettings),
    ("maxConnectAttemptsBeforeCreating", U32), ("maxConnectSecondsBeforeCreating", U32),
    ("maxWaitingForClientsToConnectSeconds", U32), ("minPlayersToLaunch", U32),
    ("intermissionTimeMs", U32), ("shortIntermissionTimeMs", U32))))
HopperGetSessionListFilterSettings_Results = S(("settings", SessionFilterSettings))
HopperReportQosData_Params = S(("qosWireDataArray", ARR(S(
    ("sessionID", XNKID), ("bFlags", U8), ("cProbesXmit", I16), ("cProbesRecv", U8),
    ("wRttMinInMsecs", I16), ("wRttMedInMsecs", I16), ("dwUpBitsPerSec", U32),
    ("dwDnBitsPerSec", U32)))))
HopperUserRegister_Params = S(("hopperId", I32), ("sessionId", XNKID))
HopperUserRegister_Results = S(("nextRegisterSeconds", I32))
HopperGetGameOptions_Params = S(("hopperId", I32), ("roundNum", I32), ("cchOptionXmlMax", I32))
HopperGetGameOptions_Results = S(("gameOptionsXml", BYTES()))
HopperGetRaceMultipliers_Params = S(("maxSpecialMultiplierDescriptionLength", I32))
HopperGetRaceMultipliers_Results = S(("specialMultiplier", F32),
                                     ("specialMultiplierDescription", WSTR()))
HopperReportVoting_Params = S(("votingMode", I32), ("result", I32), ("trackId", I32),
                              ("hopperId", I32),
                              ("playerVotes", ARR(S(("xuid", U64), ("playerVote", I32)))))

# Forza.WebServices.OnlineProfile: the game uploads the profile after signing in and
# after every event. UpdateAll's arrays hold one record each; records of settings are
# kept as raw bytes here (their sizes on the wire).
PlayerHistory = S(
    ("isVip", BOOL), ("lastTrialImage", U32), ("lastRewardedLevel", U32), ("credits", U32),
    ("auctionsOpen", U32), ("bidsOutstanding", U32), ("numAuctionHouseTransactions", U32),
    ("currentNumberOfMessagesDownloaded", U32), ("lastMessageCenterMonth", I16),
    ("lastMessageCenterDay", I16), ("messageCenterVisits", I16), ("perfectAutocrossCount", I16),
    ("rivalsCompletionTracking", I16), ("careerCarId", U32), ("homespaceTrackId", U32),
    ("upstreamBandwidthPeak", U32), ("flags", U64), ("updateVersionOnServer", I32),
    ("playerCardBadgeId", U32), ("playerCardTitleId", U32), ("upTime", U64),
    ("totalDistanceDriven", U32), ("functionalLocationsVisited", I16),
    ("driverLevelFromProfileImport", I32))
PlayerRaceHistory = S(*[(name, U32) for name in (
    "level", "xp", "totalWinnings", "onlineWinnings", "totalRepairs", "distanceDriven",
    "timeDriven", "topSpeed", "longestJump", "numVictories", "numPodiums", "numRaces",
    "numLinearPlayRaces", "aiSkill", "numPerfectDrifts", "numPerfectDrafts", "numPerfectTurns",
    "numPerfectPasses", "numPerfectSpeeds", "numCreditsSpent", "numCreditsEarned",
    "numOnlineRaces", "numOnlineVictories", "numRaceEncountersWon", "numPerfectBurnouts",
    "numPerfectAirs", "numPerfect2Wheels", "numPerfectEDrifts", "numPerfectNearMisses",
    "numPerfectDestructions", "numPerfectTurn180s", "numPerfectJTurns",
    "numPerfectLawBreakers", "maxSkillsScore", "maxSkillsCarOrdinal", "wristbandLevel",
    "onlineXP", "onlineWristbandLevel", "onlineWristbandType", "numOnlineCarsWon",
    "numKingGames", "numCatMouseGames", "numEncountersWon")])
# (xp, wristbandLevel, onlineXP, onlineWristbandLevel, onlineWristbandType are signed and
# topSpeed, longestJump, aiSkill are floats on the game's side; all 4 bytes.)
OnlineProfileUpdateAll_Params = S(
    ("assists", ARR(RAW(26))), ("avOptions", ARR(RAW(22))), ("controllerLayout", ARR(RAW(28))),
    ("controllerAdvancedOptions", ARR(RAW(48))), ("wheelLayout", ARR(RAW(28))),
    ("wheelAdvancedOptions", ARR(RAW(48))), ("bristolLayout", ARR(RAW(28))),
    ("bristolAdvancedOptions", ARR(RAW(48))), ("hudOptions", ARR(RAW(64))),
    ("photoModeSettings", ARR(RAW(37))), ("playerHistory", ARR(PlayerHistory)),
    ("playerRaceHistory", ARR(PlayerRaceHistory)), ("uiCounters", ARR(RAW(52))),
    ("showroom", ARR(RAW(10))), ("headTrackingSettings", ARR(RAW(37))), ("raceStatsId", U32))

# Forza.WebServices.Scoreboard: leaderboards, Rivals and their ghosts.
_SuspTire = S(*[(n, F32) for n in (
    "m_Pressure", "m_Camber", "m_Toe", "m_Caster", "m_Springs", "m_Swaybar", "m_RideHeight",
    "m_DampBump", "m_DampRebound", "m_LSDTorqueAccel", "m_LSDTorqueDecel")])
CarTuning = S(("base", S(
    *[(n, F32) for n in ("m_FrontDownforce", "m_RearDownforce", "m_FinalDriveRatio",
                         "m_BrakePressure", "m_BrakeBias", "m_HandbrakePressure",
                         "m_CenterDiffTorqueSplit", "m_STMScale", "m_TCSTakeOffSlipDef",
                         "m_TCSMovingSlipDef", "m_SteerSpeed", "m_SteerAmount")],
    ("m_FrontSuspTire", _SuspTire), ("m_RearSuspTire", _SuspTire),
    *[(n, F32) for n in ("m_FuelLoad", "m_ArcadeTyreGrip", "m_ArcadeTyreGripLateralOnly",
                         "m_ArcadeTyreGripFrontLateralOnly")])),
    ("m_GearRatios", ARR(F32, 9)))
ScoreSubmission = S(
    ("gameScoreboardId", I32), ("isServerTimeTrial", BOOL), ("score", F32), ("carId", U16),
    ("garageId", I32), ("carDriveTypeId", U8), ("carPower", F32), ("carTorque", F32),
    ("carWeight", F32), ("carClassId", U8), ("carPerformanceIndex", F32),
    *[(n, BOOL) for n in ("usedSTM", "usedABS", "usedTCS", "usedAutoBrake", "usedAutoShifting",
                          "usedClutch", "usedDrafting", "usedMulligan", "usedFullSuggestedLine",
                          "usedBrakingSuggestedLine")],
    ("installedParts", S(("base", S(("m_TireBrand", I32), ("m_WheelStyle", I32))),
                         ("m_CarParts", ARR(I32, 43)))),
    ("tuning", CarTuning))
ScoreboardSubmitScore_Params = S(("type", I32), ("data", ScoreSubmission))
ScoreboardSubmitScoreEx_Params = S(("type", I32), ("data", S(("base", ScoreSubmission),
                                                              ("numCarsPassed", I32))))
ScoreboardSubmitScore_Results = S(("scoreId", GUID), ("uploadFiles", BOOL))
ScoreboardUploadFile_Params = S(("scoreId", GUID), ("data", BYTES()))
ScoreboardDownloadFile_Params = S(("id", I32), ("maxSize", I32))
ScoreboardDownloadFile_Results = S(("data", BYTES()))
ClanData = S(("clanId", I32), ("clanTag", WSTR(5)), ("clanName", WSTR(21)))
ScoreboardRow = S(
    ("position", I32), ("rank", I32), ("user", UserData), ("score", F32),
    ("submittedTime", SYSTIME), ("carId", U16), ("carClassId", U8),
    ("carPerformanceIndex", F32),
    *[(n, BOOL) for n in ("usedSTM", "usedTCS", "usedABS", "usedAutoBrake", "usedAutoShifting",
                          "usedClutch")],
    ("ghostFileId", I32), ("clanData", ClanData), ("bounty", I32))
ScoreboardGetRows_Params = S(("type", I32), ("view", I32), ("gameScoreboardId", I32),
                             ("isServerTimeTrial", BOOL), ("isClubBoard", BOOL),
                             ("startAt", I32), ("maxResults", I32))
ScoreboardGetRows_Results = S(("rows", ARR(ScoreboardRow)), ("userRow", ScoreboardRow),
                              ("available", I32), ("totalRowCount", I32))
ScoreboardGetRollupRows_Params = S(("type", I32), ("view", I32), ("pivotId", I32),
                                   ("eventTypeId", I32), ("isClubBoard", BOOL),
                                   ("startAt", I32), ("maxResults", I32))
ScoreboardGetRollupRows_Results = S(("rows", ARR(ScoreboardRow)), ("userRow", ScoreboardRow),
                                    ("available", I32))
ScoreboardGetStorefrontRows_Params = S(("type", I32), ("view", I32), ("startAt", I32),
                                       ("maxResults", I32))
ScoreboardGetStorefrontRows_Results = ScoreboardGetRollupRows_Results
ScoreboardGetRival_Params = S(("gameScoreboardId", I32), ("isServerTimeTrial", BOOL),
                              ("rivalXuid", U64))
Rival = S(("scoreData", ScoreboardRow), ("bounty", I32))
ScoreboardGetRival_Results = S(("rival", Rival))
ScoreboardGetRivalAndPersonalBest_Results = S(("rival", Rival),
                                              ("personalBest", S(("score", F32))))
ScoreboardGetBestScores_Params = S(("scoreType", I32), ("gameScoreboardId", I32),
                                   ("isServerTimeTrial", BOOL), ("carId", U16),
                                   ("carClassId", U8), ("carDriveTypeId", U8))
ScoreboardGetBestScores_Results = S(("bestScore", S(("score", F32))))
ScoreboardNotifyRival_Params = S(("gameScoreboardId", I32), ("isServerTimeTrial", BOOL),
                                 ("rivalXuid", U64), ("type", I32), ("myScoreId", GUID))
ScoreboardGetTimeTrials_Params = S(("maxTimeTrialCategories", U32), ("maxTimeTrialResults", U32))
TimeTrial = S(("id", U16), ("scoreType", I32), ("name", WSTR(129)), ("description", WSTR(513)),
              ("notRegisteredMessage", WSTR(257)), ("trackId", U16), ("trackName", WSTR(65)),
              ("hopperId", I32), ("gameOptionsIndex", I32), ("startDate", SYSTIME),
              ("endDate", SYSTIME), ("userRegistered", BOOL), ("featured", BOOL),
              ("active", BOOL), ("displayCarClass", I32))
ScoreboardGetTimeTrials_Results = S(
    ("timeTrialCategories", ARR(S(("category", I32), ("startIndex", I32)))),
    ("timeTrials", ARR(TimeTrial)))

# Xls.WebServices.Notifications: the Message Center.
NotificationsStartNewSession_Results = S(("nonce", U16), ("nextRetrieveSeconds", I32))
NotificationsRetrieveForUser_Params = S(("nonce", U16), ("maxResults", I32),
                                        ("cbParameterBlockSize", I32))
Notification = S(("id", I64), ("xuidRecipient", U64), ("notificationType", I16),
                 ("hasThumbnail", BOOL), ("isRead", BOOL), ("deleteAction", I32),
                 ("expirationDate", I64), ("cbParameters", I16))
NotificationsRetrieveForUser_Results = S(("results", ARR(Notification)),
                                         ("parameterBlock", BYTES()),
                                         ("nextRetrieveSeconds", I32))
NotificationsId_Params = S(("notificationId", I64))  # Clear, MarkRead
NotificationsGetCategories_Params = S(("maxResults", I32))
NotificationsGetCategories_Results = S(("results", ARR(S(
    ("type", I32), ("name", WSTR(64)), ("description", WSTR(256)),
    ("backgroundImageFileId", I32)))))

