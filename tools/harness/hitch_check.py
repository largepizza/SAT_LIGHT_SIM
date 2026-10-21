#!/usr/bin/env python3
"""Frame-hitch regression check over the time-scale ladder (CLAUDE.md "Frame profile and hitches").

    python tools/harness/hitch_check.py                       # scenarios A, C, D (launches the app 3 times)
    python tools/harness/hitch_check.py --scenarios A          # right after load only (~2 min)
    python tools/harness/hitch_check.py --scenarios B --settings path/to/user_settings.json
    python tools/harness/hitch_check.py --run harness_runs/x   # check a finished run folder (no launch)

Scenarios (each is one harness run, every rate from 1x to 1 yr/s for 180 frames, then `hitches`):
    A  right after load (tools/harness/scripts/hitch_ladder.satcmd)
    B  the same with --settings (a user's settings.json; required for B)
    C  in 2031, before the deployment-era fill is done (hitch_ladder_2031.satcmd)
    D  after the Sun's Fury: day 32 of the cascade (hitch_ladder_cme.satcmd)

Every rate's wall-time p99 and max are checked against tools/harness/hitch_thresholds.json ("default", then the
scenario's and the rate's overrides), plus the first frame after load (the max of A's / B's 1x step) and the time
from launch to the first frame (the log's `boot: Starting`). Exits 1 if any check fails, so a change that brings a
hitch back fails here. Thresholds are for an RTX 3070 Ti class machine at the harness's 1600x900: `--scale 1.5`
loosens all of them on a slower machine. Other agents' GPU work on the same machine adds noise: a failure that does
not repeat on a second run is noise; `--retries 1` reruns a failing scenario once and keeps the better result.
"""
import argparse
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
SCRIPTS = {"A": "hitch_ladder.satcmd", "B": "hitch_ladder.satcmd", "C": "hitch_ladder_2031.satcmd",
           "D": "hitch_ladder_cme.satcmd"}
RATES = ["1x", "10x", "1m", "5m", "1h", "1d", "1w", "1mo", "1yr"]


def load_thresholds(path):
    with open(path) as f:
        return json.load(f)


def limits_for(th, scen, rate):
    lim = dict(th.get("default", {}))
    lim.update(th.get("scenarios", {}).get(scen, {}).get("all", {}))
    lim.update(th.get("rates", {}).get(rate, {}))
    lim.update(th.get("scenarios", {}).get(scen, {}).get(rate, {}))
    return lim


def parse_run(run):
    """[(rate, wall_ms dict, hitch lines)] in script order, and the boot time (ms) from the log."""
    steps = []
    rate = None
    with open(os.path.join(run, "results.jsonl")) as f:
        for line in f:
            j = json.loads(line)
            cmd = j.get("cmd", "")
            m = re.match(r"\s*time\s+scale\s+(\S+)", cmd)
            if m:
                rate = m.group(1)
            r = j.get("result") or {}
            if cmd.startswith("hitches") and "wall_ms" in r and rate is not None:
                steps.append((rate, r["wall_ms"], r.get("hitches", [])))
    boot = None
    log = os.path.join(run, "satlight_log.txt")
    if os.path.exists(log):
        with open(log, errors="replace") as f:
            for line in f:
                m = re.search(r"boot: Starting \((\d+) ms\)", line)
                if m:
                    boot = int(m.group(1))
    return steps, boot


def check_run(run, scen, th, scale):
    steps, boot = parse_run(run)
    fails = []
    rows = []
    seen = set()
    for rate, w, hitches in steps:
        key = rate
        first = scen in ("A", "B") and rate == "1x" and "1x" not in seen  # the step that holds the first frame
        seen.add(rate)
        lim = limits_for(th, scen, rate)
        p99_lim = lim.get("p99_ms", 1e9) * scale
        max_lim = (th.get("first_frame_max_ms", 1e9) if first else lim.get("max_ms", 1e9)) * scale
        ok = w.get("p99", 0) <= p99_lim and w.get("max", 0) <= max_lim
        rows.append((scen, key + (" (load)" if first else ""), w, p99_lim, max_lim, ok))
        if not ok:
            fails.append("%s %s: p99 %.1f (limit %.0f), max %.1f (limit %.0f)%s" % (
                scen, key, w.get("p99", 0), p99_lim, w.get("max", 0), max_lim,
                (" - worst: " + hitches[0][:240]) if hitches else ""))
    if not steps:
        fails.append("%s: no `hitches` results in %s" % (scen, run))
    if boot is not None and "boot_max_ms" in th and boot > th["boot_max_ms"] * scale:
        fails.append("%s: launch to first frame %d ms (limit %.0f)" % (scen, boot, th["boot_max_ms"] * scale))
    return rows, fails, boot


def run_scenario(scen, args):
    out = os.path.join(args.out, "hitch_%s" % scen)
    cmd = [sys.executable, os.path.join(HERE, "run.py"), os.path.join(HERE, "scripts", SCRIPTS[scen]),
           "--out", out, "--timeout", "1800", "--quiet"]
    if args.exe:
        cmd += ["--exe", args.exe]
    if scen == "B":
        cmd += ["--settings", args.settings]
    print("running scenario %s ..." % scen, flush=True)
    subprocess.run(cmd, cwd=REPO)
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scenarios", default="A,C,D")
    ap.add_argument("--run", nargs="*", help="check these finished run folders instead (scenario from --scenarios, in order)")
    ap.add_argument("--settings", help="settings.json for scenario B")
    ap.add_argument("--exe")
    ap.add_argument("--out", default=os.path.join(REPO, "harness_runs", "hitch_check"))
    ap.add_argument("--thresholds", default=os.path.join(HERE, "hitch_thresholds.json"))
    ap.add_argument("--scale", type=float, default=1.0, help="multiply every limit (a slower machine)")
    ap.add_argument("--retries", type=int, default=0, help="rerun a failing scenario this many times")
    args = ap.parse_args()
    th = load_thresholds(args.thresholds)
    scens = [s.strip().upper() for s in args.scenarios.split(",") if s.strip()]
    if "B" in scens and not args.run and not args.settings:
        sys.exit("scenario B needs --settings <settings.json>")
    all_rows, all_fails = [], []
    for i, scen in enumerate(scens):
        if args.run:
            if i >= len(args.run):
                break
            rows, fails, boot = check_run(args.run[i], scen, th, args.scale)
        else:
            tries = 0
            while True:
                rows, fails, boot = check_run(run_scenario(scen, args), scen, th, args.scale)
                if not fails or tries >= args.retries:
                    break
                tries += 1
                print("scenario %s failed; retrying (%d/%d)" % (scen, tries, args.retries), flush=True)
        if boot is not None:
            print("%s: launch to first frame %.1f s" % (scen, boot / 1000.0))
        all_rows += rows
        all_fails += fails
    print("%-4s %-10s %7s %7s %7s %8s %8s  %s" % ("scen", "rate", "p50", "p99", "max", "p99 lim", "max lim", ""))
    for scen, rate, w, p99l, maxl, ok in all_rows:
        print("%-4s %-10s %7.1f %7.1f %7.1f %8.0f %8.0f  %s" % (scen, rate, w.get("p50", 0), w.get("p99", 0),
                                                              w.get("max", 0), p99l, maxl, "ok" if ok else "FAIL"))
    if all_fails:
        print("\nFAILED:")
        for f in all_fails:
            print("  " + f)
        sys.exit(1)
    print("\nall within limits")


if __name__ == "__main__":
    main()
