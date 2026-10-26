#!/usr/bin/env python3
"""track_path.py TRACK_ID OUT [owt_path.py options]: a track's own AI route as an autodrive path.

A test tool. The game's database (`media/db/gamedb.slt`, Tracks) names each track's AI route
(`RouteId`, in the folder of its `MediaName`): `media/aiopenworld.zip`
`<medianame>/Ribbon_00/route_NNN.owt`. This extracts it (tools/unzip21.py, which needs
tools/unxmem) and converts it with tools/owt_path.py. A circuit's route is one closed lap: give
the path to tools/autodrive.py as AD_PATH_LOAD (without AD_OPEN), and it drives lap after lap.
Example: track 322, Clear Springs Sprint (the Cat & Mouse round of the Playground Games
playlist, RouteId 2): 554 points, 2228 m.

Environment: FORZA_GAME (default extracted/game) is the game's folder.
"""
import os
import sqlite3
import subprocess
import sys
import tempfile

TOOLS = os.path.dirname(os.path.abspath(__file__))
GAME = os.environ.get("FORZA_GAME", os.path.join(TOOLS, "..", "extracted", "game"))


def main():
    if len(sys.argv) < 3:
        sys.exit(__doc__)
    track, out = int(sys.argv[1]), sys.argv[2]
    db = sqlite3.connect("file:%s?mode=ro" % os.path.join(GAME, "media", "db", "gamedb.slt"), uri=True)
    row = db.execute("SELECT MediaName, RouteId, Length FROM Tracks WHERE Id = ?", (track,)).fetchone()
    if not row:
        sys.exit("no track %d in gamedb.slt" % track)
    media, route, length = row
    member = "%s/Ribbon_00/route_%03d.owt" % (media.lower(), route)
    print("track %d: %s, %d m" % (track, member, length), flush=True)
    with tempfile.TemporaryDirectory() as tmp:
        subprocess.run([sys.executable, os.path.join(TOOLS, "unzip21.py"),
                        os.path.join(GAME, "media", "aiopenworld.zip"), member, tmp],
                       check=True, stdout=subprocess.DEVNULL)
        subprocess.run([sys.executable, os.path.join(TOOLS, "owt_path.py"),
                        os.path.join(tmp, os.path.basename(member)), out] + sys.argv[3:], check=True)


if __name__ == "__main__":
    main()
