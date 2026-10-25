#!/usr/bin/env python3
"""Frame-time benchmark: wall clock, GPU and CPU, still and moving, in the matrix suite's scenes.

The matrix suite judges images and GPU cost; this one judges FRAME TIME, which is what a CPU/GPU overlap (frames in
flight), a pass split or a cheaper CPU path changes. Uncapped present (`display.fps_cap_mode 0`, IMMEDIATE), so a
frame is as long as the slower of the CPU and the GPU allow, not the display's refresh.

    python tools/harness/frame_bench.py gen harness_runs/fb/fb.satcmd [--only alt0m,storm_gnd] [--reps 1]
    python tools/harness/run.py harness_runs/fb/fb.satcmd --window 1920x1009 --out harness_runs/fb/<name>
    python tools/harness/frame_bench.py report <run dir> [<run dir> ...]
    python tools/harness/frame_bench.py compare <base run dir>[,<base2>...] <new run dir>[,<new2>...]

Per scene (matrix_suite.py's cells: the same poses, times and player-speed paths):
  still     `perf frames=180`: GPU total, wall frame, the CPU buckets (time paused)
  live      the same with time running at 1x, 1 min/s and 1 h/s (the CPU's moving parts: launches, Kessler,
            storms, the weather bake)
  motions   each matrix path (walk / fast / look / rise ...) played at the player's speed, unrecorded (full
            speed), between `hitches clear` and `hitches` (wall p50 / p90 / p99 / max over its frames), with the
            path's own GPU mean
`compare` takes several runs per side (the noise between runs of one build is 3-10%): it prints the per-row
median of each side and the change, and flags a row whose change is outside the spread of either side.
"""
import argparse
import json
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import matrix_suite as mx  # noqa: E402

DEFAULT_CELLS = ["alt0m", "alt1km", "alt3km", "alt15km", "alt500km", "alt5000km", "storm_gnd", "storm_15km",
                 "sea_20m", "night_3km", "beams_site", "aur_gnd", "launch", "iss", "starmind", "eclipse"]
LIVE_CELLS = {"alt0m", "alt500km", "storm_15km", "launch"}
LIVE_RATES = ["1x", "1m", "1h"]
STILL_FRAMES = 180
SETS = mx.SETS + [("display.fps_cap_mode", "0")]


def gen_cell(c, reps):
    p = f"fb_{c.name}"
    L = mx.setup(c) + mx.settle(c)
    for r in range(reps):
        L += [f"perf frames={STILL_FRAMES} name={p}_still_r{r}"]
    if c.seq:  # follow-mode cells: time running is the only way to see them move
        for tag, rate, _n in c.seq:
            L += ["time play", f"time scale {rate}", "wait 30"]
            for r in range(reps):
                L += [f"perf frames={STILL_FRAMES} name={p}_seq{tag}_r{r}"]
            L += ["time pause", "time scale 1x"]
    if c.name in LIVE_CELLS:
        for rate in LIVE_RATES:
            L += ["time play", f"time scale {rate}", "wait 60"]
            for r in range(reps):
                L += [f"perf frames={STILL_FRAMES} name={p}_live{rate}_r{r}"]
            L += ["time pause", "time scale 1x"] + mx.setup(c) + mx.settle(c, 60)
    for m in c.motions:
        L += [f"# --- {c.name} {m}"] + mx.setup(c) + ["path clear"]
        L += [mx.fmt_key(t, k) for t, k in mx.path_keys(c, m)]
        for r in range(reps):
            L += ["path goto 0", f"wait settle {mx.SETTLE_PRE}", "hitches threshold 100000", "hitches clear",
                  f"path play fps={mx.FPS}", ("hitches top=0 list=0", f"{p}_{m}_r{r}")]
    L += list(c.teardown)
    return L


def cmd_gen(a):
    want = a.only.split(",") if a.only else DEFAULT_CELLS
    cells = [c for c in mx.build_cells() if c.name in want]
    body = [f"set {k} {v}" for k, v in SETS] + ["hitches threshold 100000"]
    for c in cells:
        body += gen_cell(c, a.reps)
    body += mx.restore_lines(None) + ["set display.fps_cap_mode 4"]
    head = [f"# frame_bench.py gen {' '.join(sys.argv[2:])}",
            f"# {len(cells)} cells",
            f"# python tools/harness/run.py {a.out} --window 1920x1009 --out <run dir>"]
    # A motion's `hitches` line is tagged through a sidecar (line number -> row): the harness strips comments.
    lines, tags = [], {}
    for x in head + body + ["quit"]:
        if isinstance(x, tuple):
            tags[len(lines) + 1] = x[1]
            x = x[0]
        lines.append(x)
    os.makedirs(os.path.dirname(os.path.abspath(a.out)) or ".", exist_ok=True)
    open(a.out, "w", encoding="utf-8").write("\n".join(lines) + "\n")
    json.dump(tags, open(a.out + ".tags.json", "w"), indent=0)
    print(json.dumps({"script": a.out, "cells": [c.name for c in cells], "lines": len(lines)}, indent=1))


def load_tags(d):
    """The motion tags (line -> row) of the script a run was made from: a copy beside it, else run.json's script."""
    for cand in [os.path.join(d, f) for f in os.listdir(d) if f.endswith(".tags.json")]:
        if os.path.exists(cand):
            return {int(k): v for k, v in json.load(open(cand)).items()}
    for line in open(os.path.join(d, "results.jsonl"), encoding="utf-8"):
        src = json.loads(line).get("src", "")
        scr = src.rsplit(":", 1)[0]
        if scr and os.path.exists(scr + ".tags.json"):
            return {int(k): v for k, v in json.load(open(scr + ".tags.json")).items()}
        break
    return {}


def load_run(d):
    """{row: [samples]} from one run's results.jsonl."""
    rows = {}
    tags = load_tags(d)
    last_path_gpu = None
    for line in open(os.path.join(d, "results.jsonl"), encoding="utf-8"):
        try:
            j = json.loads(line)
        except ValueError:
            continue
        cmd = j.get("cmd", "")
        r = j.get("result") or {}
        if cmd.startswith("perf ") and "name=fb_" in cmd:
            nm = cmd.split("name=")[1].split()[0]
            base, _, rep = nm.rpartition("_r")
            w = r.get("wall_frame_ms") or {}
            g = r.get("gpu_total_ms") or {}
            cpu = r.get("cpu_ms") or {}
            rows.setdefault(base, []).append({"wall": w.get("mean"), "wall_p90": w.get("p90"), "wall_max": w.get("max"),
                                              "gpu": g.get("mean"), "cpu": sum(v for v in cpu.values() if v),
                                              "kind": "still"})
        elif cmd.startswith("path play"):
            gm = r.get("gpu_ms_mean") or {}
            last_path_gpu = gm.get("total")
        elif cmd.startswith("hitches top=0") and int(j.get("src", "x:0").rsplit(":", 1)[1] or 0) in tags:
            nm = tags[int(j["src"].rsplit(":", 1)[1])]
            base, _, rep = nm.rpartition("_r")
            w = r.get("wall_ms") or {}
            rows.setdefault(base, []).append({"wall": w.get("mean"), "wall_p90": w.get("p90"), "wall_max": w.get("max"),
                                              "wall_p99": w.get("p99"), "gpu": last_path_gpu, "cpu": None,
                                              "kind": "motion"})
    return rows


def agg(runs):
    """Merge several runs: per row, the list of every sample's wall / gpu / p90."""
    out = {}
    for d in runs:
        for k, lst in load_run(d).items():
            o = out.setdefault(k, {"wall": [], "gpu": [], "p90": [], "max": [], "cpu": []})
            for s in lst:
                if s["wall"] is not None:
                    o["wall"].append(s["wall"])
                if s["gpu"] is not None:
                    o["gpu"].append(s["gpu"])
                if s["wall_p90"] is not None:
                    o["p90"].append(s["wall_p90"])
                if s["wall_max"] is not None:
                    o["max"].append(s["wall_max"])
                if s["cpu"] is not None:
                    o["cpu"].append(s["cpu"])
    return out


def med(v):
    return statistics.median(v) if v else float("nan")


def cmd_report(a):
    rows = agg(a.runs)
    print(f"{'row':34s} {'wall':>7s} {'p90':>7s} {'max':>7s} {'gpu':>7s} {'cpu':>6s} {'w-g':>6s}")
    for k in sorted(rows):
        o = rows[k]
        w, g = med(o["wall"]), med(o["gpu"])
        print(f"{k:34s} {w:7.2f} {med(o['p90']):7.2f} {med(o['max']):7.2f} {g:7.2f} {med(o['cpu']):6.2f} {w - g:6.2f}")


def cmd_compare(a):
    A = agg(a.base.split(","))
    B = agg(a.new.split(","))
    print(f"{'row':34s} {'wall A':>7s} {'wall B':>7s} {'dWall':>7s} {'%':>6s} {'p90 A':>7s} {'p90 B':>7s} "
          f"{'gpu A':>7s} {'gpu B':>7s} {'flag':>5s}")
    tot = {"still": [0.0, 0.0], "motion": [0.0, 0.0]}
    for k in sorted(set(A) & set(B)):
        a_, b_ = A[k], B[k]
        wa, wb = med(a_["wall"]), med(b_["wall"])
        spread = max((max(a_["wall"]) - min(a_["wall"])) if len(a_["wall"]) > 1 else 0.0,
                     (max(b_["wall"]) - min(b_["wall"])) if len(b_["wall"]) > 1 else 0.0)
        d = wb - wa
        flag = "WORSE" if d > max(spread, 0.05 * wa) else ("" if d > -max(spread, 0.05 * wa) else "better")
        kind = "motion" if any(m in k for m in ("_walk", "_fast", "_look", "_rise", "_descend", "_strafe")) else "still"
        tot[kind][0] += wa
        tot[kind][1] += wb
        print(f"{k:34s} {wa:7.2f} {wb:7.2f} {d:+7.2f} {100 * d / wa:+6.1f} {med(a_['p90']):7.2f} {med(b_['p90']):7.2f} "
              f"{med(a_['gpu']):7.2f} {med(b_['gpu']):7.2f} {flag:>5s}")
    for kind, (sa, sb) in tot.items():
        if sa:
            print(f"SUM {kind:8s} wall {sa:8.1f} -> {sb:8.1f} ms ({100 * (sb - sa) / sa:+.1f}%)")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gen")
    g.add_argument("out")
    g.add_argument("--only", help="comma-separated matrix cell names")
    g.add_argument("--reps", type=int, default=1, help="samples of each row within the run")
    g.set_defaults(fn=cmd_gen)
    r = sp.add_parser("report")
    r.add_argument("runs", nargs="+")
    r.set_defaults(fn=cmd_report)
    c = sp.add_parser("compare")
    c.add_argument("base")
    c.add_argument("new")
    c.set_defaults(fn=cmd_compare)
    a = ap.parse_args()
    a.fn(a)


if __name__ == "__main__":
    main()
