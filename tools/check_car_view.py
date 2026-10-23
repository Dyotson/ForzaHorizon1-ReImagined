#!/usr/bin/env python3
"""Check that the car view the game is given (src/hooks/database_query.cpp, #167) answers
exactly like Title Update 4's own Restrictions_Data_Car, on your game files.

Usage: tools/check_car_view.py [--update DIR] [--dlc DIR]
  --update  the Title Update 4 folder (default.xex, media.zip; default extracted/tu4)
  --dlc     a folder with DLC packages (searched for Media/DLCZips/*.zip); optional

The game's CREATE VIEW statement is read from the update's default.xex, the rewrite from the
hook's source. Both views are made on TU4's gamedb.slt (from media.zip), first as it is, then
with the database patches (*_merge.slt) of the update and the DLC merged in numeric order as
the game does (INSERT OR REPLACE of every table both have), then again with edge cases (a car
without level 4 springs or level 5 tyres, a second level 4 spring, a drivetrain without a level
4 transmission, a drivetrain with two, a second drivetrain per car with and without one, a car
without a drivetrain, cars not drivable). For each: the view's columns (names and declared
types) and the rows of a few queries, in order, must be the same. One PASS/FAIL line each;
exit status = failures. SQLite here is newer than the game's 3.6.19; the statements use only
what 3.6.19 has. Standard library only (LZX through tools/lzx_delta.py, slow: about a minute).
"""
import argparse
import glob
import os
import re
import sqlite3
import sys
import tempfile
import zipfile

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(HERE)
sys.path.insert(0, HERE)
import extract_map_points  # noqa: E402  (read_member: zip members, method 21 too)
import xex_image  # noqa: E402

HOOK = os.path.join(ROOT, "forza-recomp", "src", "hooks", "database_query.cpp")
QUERIES = [
    "SELECT * FROM Restrictions_Data_Car WHERE IsCarInstalled(ModelId) ORDER BY ModelId",
    "SELECT * FROM Restrictions_Data_Car",
    "SELECT Id, IsRally, CouldRally, CarValue FROM Restrictions_Data_Car WHERE CouldRally=1",
    "SELECT COUNT(1) FROM Restrictions_Data_Car AS Garage WHERE IsRally=1",
]


def game_view(update):
    load, image = xex_image.load_image(os.path.join(update, "default.xex"))
    i = image.find(b"CREATE VIEW Restrictions_Data_Car AS ")
    if i < 0:
        raise SystemExit("no Restrictions_Data_Car view in this default.xex (not Title Update 4?)")
    return image[i:image.index(b"\0", i)].decode()


def hook_view():
    src = open(HOOK).read()
    body = src[src.index("kIndexedView[] ="):]
    body = body[:body.index(";\n")]
    return "".join(re.findall(r'"((?:[^"\\]|\\.)*)"', body))


def merges(update, dlc):
    found = {}
    zips = [os.path.join(update, "media.zip")]
    if dlc:
        zips += glob.glob(os.path.join(dlc, "**", "Media", "DLCZips", "*.zip"), recursive=True)
    for z in zips:
        try:
            names = [i.filename for i in zipfile.ZipFile(z).infolist()]
        except (OSError, zipfile.BadZipFile):
            continue
        for n in names:
            m = re.search(r"(\d+)_merge\.slt$", n, re.I)
            if m and m.group(1) not in found:
                found[m.group(1)] = (z, n)
    return [found[k] for k in sorted(found, key=int)]


def database(gamedb, patches, tmp):
    path = os.path.join(tmp, "merged.slt")
    with open(path, "wb") as f:
        f.write(gamedb)
    c = sqlite3.connect(path)
    for k, (z, n) in enumerate(patches):
        p = os.path.join(tmp, "patch%d.slt" % k)
        with open(p, "wb") as f:
            f.write(extract_map_points.read_member(z, n))
        c.execute("ATTACH ? AS filedb", (p,))
        main = {r[0] for r in c.execute("SELECT tbl_name FROM sqlite_master WHERE type='table'")}
        for (t,) in c.execute("SELECT tbl_name FROM filedb.sqlite_master WHERE type='table'").fetchall():
            if t in main:
                c.execute("INSERT OR REPLACE INTO [%s] SELECT * FROM filedb.[%s]" % (t, t))
        c.commit()
        c.execute("DETACH DATABASE filedb")
    c.close()
    return path


def edge_cases(c):
    ids = [r[0] for r in c.execute("SELECT ModelId FROM Data_Car ORDER BY Id")]
    dt = lambda o: c.execute("SELECT DrivetrainID FROM List_UpgradeDrivetrain WHERE Ordinal=?", (o,)).fetchone()[0]
    c.execute("DELETE FROM List_UpgradeSpringDamper WHERE Ordinal=? AND Level=4", (ids[0],))
    c.execute("DELETE FROM List_UpgradeTireCompound WHERE Ordinal=? AND Level=5", (ids[1],))
    c.execute("INSERT INTO List_UpgradeSpringDamper (Ordinal, Level, IsStock) VALUES (?, 4, 0)", (ids[2],))
    c.execute("DELETE FROM List_UpgradeDrivetrainTransmission WHERE DrivetrainID=? AND Level=4", (dt(ids[3]),))
    c.execute("INSERT INTO List_UpgradeDrivetrainTransmission (DrivetrainID, Level, IsStock) VALUES (?, 4, 0)", (dt(ids[4]),))
    c.execute("INSERT INTO List_UpgradeDrivetrain (Ordinal, DrivetrainID, Level, IsStock) VALUES (?, 999001, 2, 0)", (ids[5],))
    c.execute("INSERT INTO List_UpgradeDrivetrainTransmission (DrivetrainID, Level, IsStock) VALUES (999001, 4, 1)")
    c.execute("INSERT INTO List_UpgradeDrivetrain (Ordinal, DrivetrainID, Level, IsStock) VALUES (?, 999002, 2, 0)", (ids[6],))
    c.execute("INSERT INTO List_UpgradeDrivetrainTransmission (DrivetrainID, Level, IsStock) VALUES (999002, 3, 1)")
    c.execute("DELETE FROM List_UpgradeDrivetrain WHERE Ordinal=?", (ids[7],))
    c.execute("UPDATE Data_Car SET IsDrivable=0 WHERE ModelId IN (?, ?)", (ids[8], ids[9]))


def open_db(path, view, edges):
    c = sqlite3.connect(":memory:")
    src = sqlite3.connect(path)
    src.backup(c)
    src.close()
    c.execute("PRAGMA automatic_index=OFF")  # as 3.6.19, which has none
    c.create_function("IsCarInstalled", 1, lambda x: x % 7 != 3)
    c.create_function("IsDrivableCar", 1, lambda x: x % 5 == 1)
    c.executescript(view)
    if edges:
        edge_cases(c)
    return c


def compare(label, path, game, hook, edges):
    a, b = open_db(path, game, edges), open_db(path, hook, edges)
    problems = []
    cols_a = [r[1:3] for r in a.execute("PRAGMA table_info(Restrictions_Data_Car)")]
    cols_b = [r[1:3] for r in b.execute("PRAGMA table_info(Restrictions_Data_Car)")]
    if cols_a != cols_b:
        problems.append("columns differ: %s" % [(x, y) for x, y in zip(cols_a, cols_b) if x != y][:3])
    rows = 0
    for q in QUERIES:
        ca, cb = a.execute(q), b.execute(q)
        ra, rb = ca.fetchall(), cb.fetchall()
        rows += len(ra)
        if [d[0] for d in ca.description] != [d[0] for d in cb.description] or ra != rb:
            problems.append("%s: %d vs %d rows, not the same" % (q[:60], len(ra), len(rb)))
    cars = a.execute("SELECT COUNT(*) FROM Data_Car").fetchone()[0]
    print("%s %s: %d cars, %d columns, %d rows compared%s" % (
        "FAIL" if problems else "PASS", label, cars, len(cols_a), rows,
        "; " + "; ".join(problems) if problems else ""))
    return 1 if problems else 0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--update", default=os.path.join(ROOT, "extracted", "tu4"))
    ap.add_argument("--dlc")
    args = ap.parse_args()
    game, hook = game_view(args.update), hook_view()
    gamedb = extract_map_points.read_member(os.path.join(args.update, "media.zip"), "db/gamedb.slt")
    if gamedb is None:
        raise SystemExit("no db/gamedb.slt in %s/media.zip" % args.update)
    patches = merges(args.update, args.dlc)
    fails = 0
    with tempfile.TemporaryDirectory() as tmp:
        os.makedirs(os.path.join(tmp, "plain"))
        os.makedirs(os.path.join(tmp, "merged"))
        plain = database(gamedb, [], os.path.join(tmp, "plain"))
        fails += compare("TU4 gamedb", plain, game, hook, False)
        merged = database(gamedb, patches, os.path.join(tmp, "merged"))
        fails += compare("with %d database patches" % len(patches), merged, game, hook, False)
        fails += compare("with the patches and edge cases", merged, game, hook, True)
    return fails


if __name__ == "__main__":
    sys.exit(main())
