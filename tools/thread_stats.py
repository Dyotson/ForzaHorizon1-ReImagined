#!/usr/bin/env python3
"""Per-thread CPU time, run-queue wait and wakeups of a running game, from /proc (docs/perf.md,
"Low-end CPUs").

  thread_stats.py sample PID OUT    every second until PID exits, one JSON line per sample:
                                    per thread (name, CPU ticks, schedstat run and run-queue
                                    wait ns, timeslices, voluntary and involuntary switches),
                                    and the GPU time of the process's DRM clients (amdgpu and
                                    others that report drm-engine-* in fdinfo)
  thread_stats.py report DIR [--top N] [--cpus 0-3]
                                    the samples in DIR/threads.jsonl between the times in
                                    DIR/marks.txt ("<epoch> start", "<epoch> end"; written by
                                    tools/perf_scenario.sh with PERF_THREADS=1): % of a core
                                    each thread ran and waited for a CPU, its voluntary
                                    switches a second (mostly wakeups), the GPU ms per second;
                                    with --cpus, how busy those CPUs were besides the game
                                    (other load spoiling a PERF_CPUSET run)

Thread names are the runtime's (XThreadNNNN for guest threads; eu-stack -p PID shows their
guest entry functions).
"""
import json
import os
import sys
import time


def sample(pid, out):
    with open(out, "w") as f:
        while os.path.exists(f"/proc/{pid}"):
            now = time.time()
            rows = {}
            try:
                tids = os.listdir(f"/proc/{pid}/task")
            except OSError:
                break
            for tid in tids:
                base = f"/proc/{pid}/task/{tid}"
                try:
                    stat = open(base + "/stat").read()
                    name = stat[stat.index("(") + 1:stat.rindex(")")]
                    fields = stat[stat.rindex(")") + 2:].split()
                    run, wait, slices = (int(x) for x in open(base + "/schedstat").read().split())
                    vol = invol = 0
                    for line in open(base + "/status"):
                        if line.startswith("voluntary_ctxt"):
                            vol = int(line.split()[1])
                        elif line.startswith("nonvoluntary_ctxt"):
                            invol = int(line.split()[1])
                    rows[tid] = [name, int(fields[11]) + int(fields[12]), run, wait, slices, vol, invol]
                except (OSError, ValueError):
                    pass
            gpu = {}
            try:
                for fd in os.listdir(f"/proc/{pid}/fdinfo"):
                    try:
                        text = open(f"/proc/{pid}/fdinfo/{fd}").read()
                    except OSError:
                        continue
                    if "drm-engine-" not in text:
                        continue
                    client, engines = None, {}
                    for line in text.splitlines():
                        if line.startswith("drm-client-id:"):
                            client = line.split()[1]
                        elif line.startswith("drm-engine-"):
                            key, value = line.split(":", 1)
                            engines[key[11:]] = int(value.split()[0])
                    if client:
                        gpu[client] = engines
            except OSError:
                pass
            cpus = {}
            try:
                for line in open("/proc/stat"):
                    if line.startswith("cpu") and line[3].isdigit():
                        v = [int(x) for x in line.split()[1:]]
                        cpus[line.split()[0][3:]] = sum(v) - v[3] - v[4]  # busy ticks (not idle, iowait)
            except OSError:
                pass
            f.write(json.dumps({"t": now, "th": rows, "gpu": gpu, "cpus": cpus}) + "\n")
            f.flush()
            time.sleep(1.0)


def cpu_list(spec):
    out = []
    for part in spec.split(","):
        lo, _, hi = part.partition("-")
        out += [str(c) for c in range(int(lo), int(hi or lo) + 1)]
    return out


def report(d, top, cpus=None):
    marks = {}
    for line in open(os.path.join(d, "marks.txt")):
        t, name = line.split()
        marks[name] = float(t)
    snaps = [json.loads(line) for line in open(os.path.join(d, "threads.jsonl"))]
    a = min(snaps, key=lambda s: abs(s["t"] - marks["start"]))
    b = min(snaps, key=lambda s: abs(s["t"] - marks["end"]))
    dt = b["t"] - a["t"]
    rows = []
    for tid, rb in b["th"].items():
        ra = a["th"].get(tid) or [rb[0], 0, 0, 0, 0, 0, 0]
        rows.append(((rb[2] - ra[2]) / 1e7 / dt, (rb[3] - ra[3]) / 1e7 / dt,
                     (rb[5] - ra[5]) / dt, (rb[6] - ra[6]) / dt, rb[0], tid))
    rows.sort(reverse=True)

    def gpu_ns(s):
        return sum(sum(e.values()) for e in s.get("gpu", {}).values())

    game = sum(r[0] for r in rows)
    line = (f"{dt:.0f} s: CPU {game:.0f}% of a core, run-queue wait "
            f"{sum(r[1] for r in rows):.0f}%, {sum(r[2] for r in rows) / 1000:.1f}k voluntary "
            f"switches/s, GPU {(gpu_ns(b) - gpu_ns(a)) / 1e6 / dt:.0f} ms/s, {len(rows)} threads")
    if cpus and "cpus" in a and "cpus" in b:
        ticks = os.sysconf("SC_CLK_TCK")
        busy = sum(b["cpus"].get(c, 0) - a["cpus"].get(c, 0) for c in cpus) * 100 / ticks / dt
        line += f"; CPUs {','.join(cpus)} busy {busy - game:.0f}% of a core besides the game"
    print(line)
    for run, wait, vol, invol, name, tid in rows[:top]:
        print(f"  {run:5.1f}% run {wait:5.1f}% wait {vol:7.0f} vol/s {invol:5.0f} invol/s  {name} [{tid}]")


def main():
    if len(sys.argv) >= 4 and sys.argv[1] == "sample":
        sample(sys.argv[2], sys.argv[3])
    elif len(sys.argv) >= 3 and sys.argv[1] == "report":
        opts = dict(zip(sys.argv[3::2], sys.argv[4::2]))
        report(sys.argv[2], int(opts.get("--top", 20)),
               cpu_list(opts["--cpus"]) if "--cpus" in opts else None)
    else:
        sys.exit(__doc__)


if __name__ == "__main__":
    main()
