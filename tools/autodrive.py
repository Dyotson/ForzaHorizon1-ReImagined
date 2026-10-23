#!/usr/bin/env python3
"""autodrive.py X SECONDS [LOG]: drive console X's car in a race by following the CPU drivers.

A test tool (no human at the wheel): X is the instance name of a test session whose game
runs with XDG_DATA_HOME=$FORZA_OUT/xdg_X and FORZA_AUTOPRESS_FIFO=$FORZA_OUT/X/in.fifo
(FORZA_OUT defaults to forza-recomp/out/svc). AD_LOOK, AD_SPEED, AD_GAIN tune the
lookahead (m), the share of the CPU driver's speed, and the steering gain. It finished
the 4 laps of GoPro Millennia Masters (Clear Springs Circuit) with the Aventador, 8th,
best lap 1:55.13.

Reads every car from guest memory (list at *(*(*(ROOT)+4)+4) +0x80/+0x84, ROOT 0x832DF024,
TU4 0x833CAAE4; car =
vehicle+0x2D90; kind at vehicle+0x2DD0: 0 ours, 1 a CPU driver; position car-0xA20,
forward car-0xA40), records the leading CPU driver's trail as the path (the racing line,
with its speeds), and steers toward a point ahead on it with short stick pulses through
the console's autopress FIFO; throttle/brake keep a bit under the CPU driver's speed there.
"""
import math, os, struct, sys, time

X = sys.argv[1]
SECS = float(sys.argv[2])
LOG = open(sys.argv[3], "a") if len(sys.argv) > 3 else sys.stdout
O = os.environ.get("FORZA_OUT", os.path.normpath(os.path.join(os.path.dirname(os.path.abspath(__file__)), "..",
                                             "forza-recomp", "out", "svc")))
# AD_XDG / AD_FIFO: the game's XDG_DATA_HOME and input FIFO when they are not FORZA_OUT's.
WANT = ("XDG_DATA_HOME=%s" % os.environ.get("AD_XDG", "%s/xdg_%s" % (O, X))).encode()
HZ = 15.0
LOOKAHEAD = float(os.environ.get("AD_LOOK", "18"))
SPEED_FACTOR = float(os.environ.get("AD_SPEED", "0.8"))
GAIN = float(os.environ.get("AD_GAIN", "0.25"))  # seconds of full lock per radian per tick
LEAD_KINDS = {int(k) for k in os.environ.get("AD_KINDS", "1,3").split(",") if k}
PATH_SAVE = os.environ.get("AD_PATH_SAVE")  # write the lap (x z speed per line) when closed
PATH_LOAD = os.environ.get("AD_PATH_LOAD")  # start with a saved lap (races without CPU cars)
# (a circuit's own AI route makes one: tools/track_path.py; Cat & Mouse driven that way, #13)
# AD_OPEN=1: the loaded path is point to point (a rally stage, tools/owt_path.py), not a lap;
# exit once the car, after it got going, has not moved for 8 s although it was told to back
# out (the game holds it after the finish; routes can run on past the finish line).
OPEN = os.environ.get("AD_OPEN") == "1"


def find_pid():
    for p in os.listdir("/proc"):
        if not p.isdigit():
            continue
        try:
            if open("/proc/%s/comm" % p).read().strip() != "forza":
                continue
            if WANT in open("/proc/%s/environ" % p, "rb").read().split(b"\0"):
                return p
        except OSError:
            pass
    return None


pid = find_pid()
if not pid:
    print("no game", file=LOG)
    sys.exit(1)
mem = open("/proc/%s/mem" % pid, "rb")
# The car list's root: 0x832DF024 in the base executable, 0x833CAAE4 in Title Update 4
# (a TU4 game runs with --update_data_root).
CAR_ROOT = 0x833CAAE4 if b"--update_data_root" in open("/proc/%s/cmdline" % pid, "rb").read() else 0x832DF024


def rd(a, n):
    mem.seek(0x100000000 + a)
    return mem.read(n)


def u32(a):
    return struct.unpack(">I", rd(a, 4))[0]


def ok(a):
    return 0x00100000 <= a < 0xF0000000


def cars():
    """[(kind, (x, y, z), (fx, fz))]"""
    out = []
    root = u32(CAR_ROOT)
    if not ok(root):
        return out
    holder = u32(root + 4)
    if not ok(holder):
        return out
    lst = u32(holder + 4)
    if not ok(lst):
        return out
    first, end = u32(lst + 0x80), u32(lst + 0x84)
    if not ok(first) or end < first or end - first > 256:
        return out
    for slot in range(first, end, 4):
        v = u32(slot)
        if not ok(v):
            continue
        kind = u32(v + 0x2DD0)
        car = u32(v + 0x2D90)
        if not ok(car):
            continue
        p = struct.unpack(">3f", rd(car - 0xA20, 12))
        f = struct.unpack(">3f", rd(car - 0xA40, 12))
        if not all(math.isfinite(c) and abs(c) < 1e5 for c in p):
            continue
        fl = math.hypot(f[0], f[2])
        fw = (f[0] / fl, f[2] / fl) if 0.2 < fl < 2 else (0.0, 0.0)
        out.append((kind, p, fw, slot))
    return out


fifo = open(os.environ.get("AD_FIFO", "%s/%s/in.fifo" % (O, X)), "w", buffering=1)


def send(*lines):
    for line in lines:
        fifo.write(line + "\n")


path = []          # [(x, z, speed)] of the leader
closed = False     # the leader came back to its start: path is one lap, used in a loop
if PATH_LOAD:
    path = [tuple(float(v) for v in line.split()) for line in open(PATH_LOAD) if line.strip()]
    closed = not OPEN
leader = None      # its slot
last = {}          # slot -> (t, x, z)
own_last = None
stuck_since = None
stopped_since = None
moved = False
reverse_until = 0
t_end = time.time() + SECS
tick = 1.0 / HZ
n = 0
idx = 0            # our position on the path
while time.time() < t_end:
    t0 = time.time()
    cs = cars()
    own = next((c for c in cs if c[0] == 0), None)
    cpu = [c for c in cs if c[0] in LEAD_KINDS]
    now = time.time()
    # speeds
    speeds = {}
    for kind, p, fw, slot in cs:
        if slot in last:
            lt, lx, lz = last[slot]
            dt = now - lt
            if dt > 0:
                speeds[slot] = math.hypot(p[0] - lx, p[2] - lz) / dt
        last[slot] = (now, p[0], p[2])
    if own is None:
        time.sleep(0.2)
        continue
    ox, oz = own[1][0], own[1][2]
    fx, fz = own[2]
    v = speeds.get(own[3], 0.0)
    # the leader: the CPU car furthest along our heading at first, then keep it
    if leader is None and cpu:
        best = max(cpu, key=lambda c: (c[1][0] - ox) * fx + (c[1][2] - oz) * fz)
        leader = best[3]
    lead = next((c for c in cpu if c[3] == leader), None)
    if lead is not None and not closed and not OPEN:
        lx, lz = lead[1][0], lead[1][2]
        if not path or math.hypot(lx - path[-1][0], lz - path[-1][1]) > 2.0:
            path.append((lx, lz, speeds.get(leader, 0.0)))
        if len(path) > 300 and math.hypot(lx - path[0][0], lz - path[0][1]) < 12:
            closed = True
            print(time.strftime("%T"), "lap closed: %d points" % len(path), file=LOG, flush=True)
            if PATH_SAVE:
                with open(PATH_SAVE, "w") as f:
                    f.writelines("%.2f %.2f %.2f\n" % p for p in path)
    if len(path) < 5:
        send("RT:%.3f" % (tick * 1.2))
        time.sleep(max(0, tick - (time.time() - t0)))
        continue
    # our nearest path point, searched forward from the last one (laps: wrap)
    N = len(path)
    best_i, best_d = idx, 1e9
    for k0 in range(idx, idx + 200):
        if not closed and k0 >= N:
            break
        k = k0 % N
        d = math.hypot(path[k][0] - ox, path[k][1] - oz)
        if d < best_d:
            best_i, best_d = k, d
    if best_d > 60:  # lost: search everything
        for k in range(len(path)):
            d = math.hypot(path[k][0] - ox, path[k][1] - oz)
            if d < best_d:
                best_i, best_d = k, d
    idx = best_i
    # target: LOOKAHEAD (+ speed) meters along the path
    look = LOOKAHEAD + 0.6 * v
    j, acc = idx, 0.0
    while acc < look and (closed or j + 1 < N):
        a, b = path[j % N], path[(j + 1) % N]
        acc += math.hypot(b[0] - a[0], b[1] - a[1])
        j += 1
    j %= N
    tx, tz = path[j][0] - ox, path[j][1] - oz
    tl = math.hypot(tx, tz) or 1.0
    cross = fx * tz / tl - fz * tx / tl
    dot = fx * tx / tl + fz * tz / tl
    err = math.atan2(cross, dot)  # > 0: target to the left
    # speed target: the leader's speed a bit further on
    span = (j - idx) % N + 10
    vs = [path[(idx + k) % N][2] for k in range(span) if path[(idx + k) % N][2] > 1]
    target_v = SPEED_FACTOR * (min(vs) if vs else 15.0)
    target_v = max(8.0, min(target_v, 60.0))
    if abs(err) > 0.6:
        target_v = min(target_v, 10.0)
    cmds = []
    if now < reverse_until:
        cmds.append("LT:%.3f" % (tick * 1.2))
        cmds.append(("LSRIGHT" if err > 0 else "LSLEFT") + ":%.3f" % (tick * 1.1))
    else:
        if v < target_v:
            cmds.append("RT:%.3f" % (tick * 1.2))
        elif v > target_v + 4:
            cmds.append("LT:%.3f" % (tick * 0.8))
        hold = min(tick * 1.1, GAIN * abs(err))
        if hold > 0.01:
            cmds.append(("LSLEFT" if err > 0 else "LSRIGHT") + ":%.3f" % hold)
        # stuck: throttling but not moving
        if v < 1.0 and len(path) > 20:
            stuck_since = stuck_since or now
            if now - stuck_since > 2.5:
                reverse_until = now + 1.6
                stuck_since = None
        else:
            stuck_since = None
        moved = moved or v > 5
        if OPEN and moved and v < 0.5:
            stopped_since = stopped_since or now
            if now - stopped_since > 8:
                print(time.strftime("%T"), "stopped at the end of the path (idx %d of %d)" % (idx, N), file=LOG, flush=True)
                break
        else:
            stopped_since = None
    send(*cmds)
    n += 1
    if n % int(HZ * 3) == 0:
        print("%s v %.1f/%.1f err %+.2f path %d idx %d dist %.1f cpu %d" % (
            time.strftime("%T"), v, target_v, err, len(path), idx, best_d, len(cpu)),
            file=LOG, flush=True)
    time.sleep(max(0, tick - (time.time() - t0)))
