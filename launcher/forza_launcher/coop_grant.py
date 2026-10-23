"""Co-op campaign results (#309, docs/coop_campaign.md "Milestone 1 progress", phase B).

A festival event raced in a Co-op Festival world is an online race: the game writes no career
state for it. The Forza Live server keeps the result of the player who started it
(server/forza_coop_ledger.py), and after the session this applies it to the save the way the
career's own race end would have:

- `Career_RaceResults` and `Career_EventResults` at the new place, only when it is better than
  the save's (the career writer, sub_826ADDD0, keeps the best place);
- the wristband XP of the improvement: the points of the event's level at the new place less
  those of the old one (WristbandScoring, as sub_8253B230 counts what is left to earn);
- the event's prize at that place: CashPrize split by the race mode's distribution over the
  grid (the festival modes: (cars - place + 1) / cars, sub_8269BA50; the street mode: 1st all,
  2nd half, 3rd a quarter) and rounded to two significant figures (sub_826984E8);
- the event's own unlock rewards (Rewards_EventUnlock, which CTriggerEventUnlocks gives when
  that event is completed, sub_8253B988).

Left to the game: the unlocks by XP and the wristband level. CTriggerEventUnlocks runs the XP
unlocks (hubs, street races, Nemesis races, the Headline race; sub_82546080) with the save's XP
after every race, so the player's next race catches up, and the Nemesis races there have
conditions of their own (a test grant that wrote them by XP alone unlocked five the game had
not). When the XP passes the next wristband threshold the free roam says so and Race Central
awards it with its own unlocks and reward. A result that does not
improve the save changes nothing, so applying one twice is harmless. The online race already
paid its online credits and XP; the prize here is the career's, once per improvement.

The edit is the save editor's: the save lock, both files read, decoded and verified with the
player's own keys, one backup of the save ("Before co-op results"), both files checked unchanged
before a complete staged save replaces the directory through the save manager's recovery
journal. Recovery runs before another grant or game session. The server learns which results were
applied; those not applied wait there until the next session. Nothing happens offline, on the
server on this PC, or without a Forza Live identity (shader_share's rules).
"""
from contextlib import closing
from dataclasses import dataclass
import json
import math
import os
import sqlite3
import threading
import urllib.error
import urllib.request
import uuid

from . import garage, paths, save_editor, saves, shader_share

FETCH_TIMEOUT = 6
MAX_RESULTS = 50
MAX_CREDITS = save_editor.EDITABLE['Main/Credits'][2]
MAX_XP = save_editor.EDITABLE['Main/XP'][2]
STREET_STYLE = 2
_run_lock = threading.Lock()


@dataclass(frozen=True)
class Change:
    event: int
    code: str
    place: int
    old: object       # the save's place before (None: not raced)
    xp: int
    credits: int
    unlocked: tuple   # CareerEventUnlocks rows added


@dataclass(frozen=True)
class GrantResult:
    applied: tuple    # Change of each result that improved the save
    unchanged: tuple  # ids of results that did not (already as good, or unknown events)
    backup: object = None
    xp: int = 0
    credits: int = 0


# --- the career's rules -------------------------------------------------------------------------

def round_credits(value):
    """sub_826984E8: below 10 nothing, else rounded to two significant figures."""
    if value < 10:
        return 0
    step = 10 ** (math.floor(math.log10(value)) - 1)
    return int(math.floor(value / step + 0.5) * step)


def payout(cash, place, cars, style):
    """An event's prize at a place (sub_8269BA50): the street mode's TopThreeFinish, else the
    festival modes' share of the grid."""
    if cars <= 1 or not 1 <= place <= cars:
        return 0
    if style == STREET_STYLE:
        share = {1: 1.0, 2: 0.5, 3: 0.25}.get(place, 0.0)
    else:
        share = (cars - place + 1) / cars
    return round_credits(cash * share)


def points(gamedb, level, place):
    if place is None:
        return 0
    row = gamedb.execute('SELECT Points FROM WristbandScoring WHERE WristbandLevel=? AND Place=?',
                         (level, place)).fetchone()
    return int(row[0]) if row and row[0] is not None else 0


def unlock(db, event_id, added):
    if db.execute('INSERT OR IGNORE INTO CareerEventUnlocks (EventId) VALUES (?)', (event_id,)).rowcount:
        added.append(event_id)


def event_unlocks(gamedb, db, event_id):
    """The event's own unlock rewards (Rewards_EventUnlock: what completing it unlocks)."""
    added = []
    for (other,) in gamedb.execute('SELECT UnlockEventId FROM Rewards_EventUnlock WHERE EventId=?', (event_id,)):
        unlock(db, other, added)
    return tuple(added)


def career(gamedb, db, xp, credits, results):
    """Applies results ([{"id", "event", "place", "places", "code"}]) to a PlayerDatabase
    connection. Returns (changes, unchanged ids, xp, credits) after them."""
    changes, unchanged = [], []
    for result in results[:MAX_RESULTS]:
        event_id, place = result.get('event'), result.get('place')
        row = gamedb.execute('SELECT Level, CareerEventStyle, CashPrize, NumberOfDrivers, HorizonEventID '
                             'FROM Events WHERE Id=?', (event_id,)).fetchone() if type(event_id) is int else None
        cars = result.get('places') or 8
        if (row is None or type(place) is not int or type(cars) is not int or not 1 <= place <= cars <= 8
                or row[1] not in (1, STREET_STYLE)):
            unchanged.append(result.get('id'))
            continue
        level, style, cash, _, code = row
        old = db.execute('SELECT Place FROM Career_EventResults WHERE EventId=?', (event_id,)).fetchone()
        old = old[0] if old else None
        if old is not None and 1 <= old <= place:
            unchanged.append(result.get('id'))
            continue
        for (race,) in gamedb.execute('SELECT Id FROM Races WHERE EventId=? ORDER BY RaceNumber', (event_id,)):
            stored = db.execute('SELECT Place FROM Career_RaceResults WHERE RaceId=?', (race,)).fetchone()
            if stored is None:
                db.execute('INSERT INTO Career_RaceResults (RaceId, Place) VALUES (?,?)', (race, place))
            elif not 1 <= stored[0] <= place:
                db.execute('UPDATE Career_RaceResults SET Place=? WHERE RaceId=?', (place, race))
        if old is None:
            db.execute('INSERT INTO Career_EventResults (EventId, Place, RivalsBeaten) VALUES (?,?,0)', (event_id, place))
        else:
            db.execute('UPDATE Career_EventResults SET Place=? WHERE EventId=?', (place, event_id))
        gained = max(0, points(gamedb, level, place) - points(gamedb, level, old))
        prize = payout(cash or 0, place, cars, style)
        xp = min(MAX_XP, xp + gained)
        credits = min(MAX_CREDITS, credits + prize)
        added = event_unlocks(gamedb, db, event_id)
        changes.append(Change(event_id, result.get('code') or code or str(event_id), place, old, gained, prize, added))
    return changes, unchanged, xp, credits


# --- the save ------------------------------------------------------------------------------------

def _written(path, edited):
    """The edited file next to the original, flushed to disk (replaced by the caller)."""
    temporary = path.with_name('.coop-grant-'+uuid.uuid4().hex)
    with temporary.open('xb') as writer:
        writer.write(edited)
        writer.flush()
        os.fsync(writer.fileno())
    return temporary


def apply(cfg, profile, results, cancelled=lambda: False, progress=lambda *_: None):
    """Applies the results to the profile's save (closed game). GrantResult."""
    with saves.locked(profile):
        saves._recover(profile)
        saves._check(cancelled)
        profile_raw = save_editor._read(profile)
        garage_raw = garage._read(profile)
        document = save_editor.decode(profile, profile_raw, save_editor._keys(cfg))
        garage_doc = garage.decode(profile, garage_raw, garage._keys(cfg))
        values = document.values
        try:
            xp, credits = values['Main/XP'].value, values['Main/Credits'].value
        except KeyError as exc:
            raise saves.SaveError('This profile has no career values to update.') from exc
        db = garage._open(garage_doc.database)
        try:
            source = (cfg.game_files()/'media/db/gamedb.slt').resolve()
            with closing(sqlite3.connect(source.as_uri()+'?mode=ro', uri=True)) as gamedb:
                changes, unchanged, new_xp, new_credits = career(gamedb, db, xp, credits, results)
            if not changes:
                return GrantResult((), tuple(unchanged))
            db.commit()
            edited_garage = garage.encode(garage_doc, db.serialize())
        except sqlite3.Error as exc:
            raise saves.SaveError('Unsupported save or game database: '+str(exc)) from exc
        finally:
            db.close()
        edited_profile = save_editor.encode(document, {'Main/XP': new_xp, 'Main/Credits': new_credits})
        saves._check(cancelled)
        backup = saves._create(profile, 'Before co-op results', 'before-edit', cancelled, progress, 10)
        if backup is None:
            raise saves.SaveError('Could not back up the selected save.')
        profile_path = saves._inside(profile.folder/'ForzaProfile/ForzaProfile', profile.root)
        garage_path = garage._path(profile)
        # Stage the entire verified backup, then publish both edits together via
        # the save manager's recoverable directory swap. Originals stay intact
        # while either staged file is being written.
        parent = saves._inside(profile.folder.parent, profile.root)
        stage = parent / ('.save-restore-' + uuid.uuid4().hex)
        journal = parent / '.save-manager-restore.json'
        stage.mkdir()
        try:
            metadata, _ = saves._manifest(backup.path, profile)
            source = saves._inside(backup.path / 'save', profile.backups)
            copied = saves._copy(source, stage, saves._files(source, cancelled), cancelled, progress)
            if copied != metadata['files']:
                raise saves.SaveError('The co-op backup failed its integrity check.')
            for path, edited in ((garage_path, edited_garage), (profile_path, edited_profile)):
                staged_path = stage / path.relative_to(profile.folder)
                temporary = _written(staged_path, edited)
                os.replace(temporary, staged_path)
            saves._check(cancelled)
            if save_editor._read(profile) != profile_raw or garage._read(profile) != garage_raw:
                raise saves.SaveError('The save changed before the co-op results were applied.')
            saves._replace_directory(profile, stage, cancelled)
        finally:
            if not journal.exists():
                saves._remove(stage, parent)
        # what the game will read back
        save_editor.decode(profile, save_editor._read(profile), ((document.aes_key, document.mac_key),))
        garage.decode(profile, garage._read(profile), ((garage_doc.aes_key, garage_doc.mac_key),))
        return GrantResult(tuple(changes), tuple(unchanged), backup, new_xp - xp, new_credits - credits)


# --- the server ----------------------------------------------------------------------------------

def _request(base, identity, path, body=None, timeout=FETCH_TIMEOUT):
    req = urllib.request.Request(base+path, data=body, method='POST' if body is not None else 'GET', headers={
        'Content-Type': 'application/json', 'X-Xuid': identity[0], 'X-Friends-Key': identity[1]})
    with urllib.request.urlopen(req, timeout=timeout) as r:
        return json.loads(r.read(1 << 20) or b'{}')


def fetch(base, identity):
    results = _request(base, identity, '/coop/results').get('results', [])
    return [r for r in results if isinstance(r, dict)][:MAX_RESULTS] if isinstance(results, list) else []


def confirm(base, identity, ids):
    return _request(base, identity, '/coop/results/applied', json.dumps({'ids': list(ids)}).encode()).get('applied', 0)


def after_session(cfg, profile, log=print, base=None, identity=None, request=None):
    """Fetches the player's pending co-op results and applies them to the session's save.
    Returns a short result (for the log)."""
    base = shader_share.server_base(cfg) if base is None else base
    if not base:
        return 'no server'
    identity = identity or shader_share.read_identity(shader_share.identity_dirs(cfg))
    if identity is None:
        return 'no Forza Live identity yet'
    if profile is None:
        return 'no save'
    results = fetch(base, identity)
    if not results:
        return 'nothing'
    result = apply(cfg, profile, results)
    done = [r.get('id') for r in results if isinstance(r.get('id'), str)]
    confirm(base, identity, done)
    for change in result.applied:
        was = f'{change.old}' if change.old else 'not raced'
        log(f'Co-op results: {change.code} at place {change.place} (was {was}): +{change.xp} XP, '
            f'+{change.credits:,} CR' + (f', {len(change.unlocked)} event(s) unlocked' if change.unlocked else ''))
    return f'applied {len(result.applied)}, unchanged {len(result.unchanged)}'


def after_session_quietly(cfg, profile, log=print):
    if not _run_lock.acquire(blocking=False):
        return 'busy'
    try:
        return after_session(cfg, profile, log)
    except (OSError, ValueError, urllib.error.URLError, saves.SaveError) as e:
        paths.log_error('apply co-op results', e)
        return 'failed'
    finally:
        _run_lock.release()


def after_session_in_background(cfg, profile, log=print):
    """After a session: the fetch and the save edit in a daemon thread (the game is closed)."""
    if profile is None or not shader_share.server_base(cfg):
        return None
    t = threading.Thread(target=after_session_quietly, args=(cfg, profile, log), name='coop-results', daemon=True)
    t.start()
    return t
