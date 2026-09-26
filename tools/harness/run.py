#!/usr/bin/env python3
"""Run a harness script in the app and report what happened (docs/HARNESS.md).

    python tools/harness/run.py tools/harness/scripts/smoke.satcmd
    python tools/harness/run.py my.satcmd --config Debug --timeout 300 --out harness_runs/foo
    python tools/harness/run.py -c "time set 2036-06-21T04:00:00Z; wait settle; capture sky"

Standard library only. Prints the run folder, each command's result (errors highlighted), the
captures written, and exits non-zero when the app crashed, timed out, or any command failed.
The run folder holds results.jsonl, summary.json, captures/*.png + *.json sidecars, the app's
log (satlight_log.txt), and the settings.json the run ended with.
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import tempfile
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))


def find_exe(config):
    pats = [os.path.join(REPO, "build", config, "SAT_LIGHT_SIM_V_*.exe"),
            os.path.join(REPO, "build", "SAT_LIGHT_SIM_V_*"),
            os.path.join(REPO, "build", config, "SAT_LIGHT_SIM_V_*")]
    found = []
    for p in pats:
        found += [f for f in glob.glob(p) if os.path.isfile(f)]
    if not found:
        sys.exit(f"no built app under build/{config} - build it first "
                 f"(cmake --build build --config {config} --target SatLightSim)")
    return max(found, key=os.path.getmtime)  # the freshest build (old versioned exes linger)


def _crashes():
    """crashes.py lives next to this file. Forensics are best-effort: if it cannot read the event
    log (not Windows, no powershell), a run must still work, so every call here is guarded."""
    sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
    import crashes
    return crashes


def crash_preflight():
    """Does the machine hard-reset since the last run? (See docs/HARNESS.md, "Machine-level
    resets".) Returns a one-line warning or None; never raises."""
    try:
        return _crashes().preflight_message()
    except Exception:
        return None


def crash_witness(out):
    """Called when a run died without writing summary.json - which is exactly what a firmware
    reset does. Writes crash_witness.txt next to what survived, so the evidence outlives the
    next reboot. Returns the run-relative path or None; never raises."""
    try:
        return _crashes().write_witness(out)
    except Exception:
        return None


def blackbox_start(out):
    """GPU/CPU telemetry into <run>/blackbox.csv, fsynced per sample, from ~1 s before the launch
    (so the idle -> load step is recorded) to the exit. A machine reset leaves no dump; this file
    then ends at the last sample before the power went (docs/HARNESS.md, Machine-level resets).
    Best-effort: returns None rather than fail a run."""
    try:
        sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
        import blackbox
        os.makedirs(out, exist_ok=True)
        bb = blackbox.BlackBox(os.path.join(out, "blackbox.csv")).start()
        time.sleep(1.0)
        bb.mark("launch")
        return bb
    except Exception:
        return None


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("script", nargs="?", help=".satcmd file")
    ap.add_argument("-c", "--commands", help="inline commands instead of a file ('; ' separated)")
    ap.add_argument("--config", default="Release", help="build config to run (default Release)")
    ap.add_argument("--exe", help="explicit exe path")
    ap.add_argument("--out", help="run folder (default harness_runs/<stamp>_<script>)")
    ap.add_argument("--window", default="1600x900")
    ap.add_argument("--settings", help="start from this settings.json (default: built-in defaults)")
    ap.add_argument("--fixed-dt", help="seconds per frame fed to the sim (default 1/60; 0 = real time)")
    ap.add_argument("--timeout", type=float, default=600, help="seconds before the run is killed")
    ap.add_argument("--quiet", action="store_true", help="only errors and the summary")
    ap.add_argument("--no-blackbox", action="store_true", help="don't record GPU/CPU telemetry (blackbox.csv)")
    a = ap.parse_args()

    if not a.script and not a.commands:
        ap.error("give a script file or -c commands")
    script = a.script
    if a.commands:
        fd, script = tempfile.mkstemp(suffix=".satcmd", prefix="inline_")
        with os.fdopen(fd, "w") as f:
            f.write(a.commands.replace("; ", "\n") + "\n")
    script = os.path.abspath(script)
    exe = os.path.abspath(a.exe) if a.exe else find_exe(a.config)
    stem = "inline" if a.commands else os.path.splitext(os.path.basename(script))[0]
    out = os.path.abspath(a.out) if a.out else os.path.join(
        REPO, "harness_runs", time.strftime("%Y%m%d_%H%M%S") + "_" + stem)

    cmd = [exe, "--script", script, "--out", out, "--window", a.window, "--timeout", str(a.timeout + 5)]
    if a.settings:
        cmd += ["--settings", os.path.abspath(a.settings)]
    if a.fixed_dt is not None:
        cmd += ["--fixed-dt", a.fixed_dt]
    warn = crash_preflight()
    if warn:
        print(warn, flush=True)
    print(f"exe: {exe}\nrun: {out}", flush=True)
    bb = None if a.no_blackbox else blackbox_start(out)
    t0 = time.time()
    try:
        proc = subprocess.run(cmd, cwd=REPO, timeout=a.timeout + 30,
                              stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        code, output = proc.returncode, proc.stdout
    except subprocess.TimeoutExpired as e:
        code, output = "killed (timeout)", (e.stdout or b"").decode(errors="replace") if isinstance(e.stdout, bytes) else (e.stdout or "")
    wall = time.time() - t0
    if bb:
        bb.mark("exit %s" % code)
        bb.stop()
    with open(os.path.join(out, "app_stdout.txt"), "w", encoding="utf-8", errors="replace") if os.path.isdir(out) else open(os.devnull, "w") as f:
        f.write(output or "")

    results = []
    rpath = os.path.join(out, "results.jsonl")
    if os.path.exists(rpath):
        with open(rpath, encoding="utf-8") as f:
            results = [json.loads(l) for l in f if l.strip()]
    summary = {}
    spath = os.path.join(out, "summary.json")
    if os.path.exists(spath):
        with open(spath, encoding="utf-8") as f:
            summary = json.load(f)

    for r in results:
        if r.get("ok"):
            if not a.quiet:
                msg = (r.get("result") or {}).get("message", "ok")
                print(f"  ok   {r['cmd']:<60.60} {msg}")
        else:
            print(f"  ERR  {r['src']}: {r['cmd']}\n       -> {r.get('error')}")
    caps = sorted(glob.glob(os.path.join(out, "captures", "*.png")))
    if caps and not a.quiet:
        print("captures:")
        for c in caps:
            print("  " + os.path.relpath(c, REPO))
    status = summary.get("status", "no summary (crashed?)")
    print(f"status: {status}  exit={code}  commands={len(results)}  errors={summary.get('errors', '?')}  wall={wall:.1f}s")
    if not summary or code != 0:
        log = os.path.join(out, "satlight_log.txt")
        if os.path.exists(log):
            with open(log, encoding="utf-8", errors="replace") as f:
                tail = f.read().splitlines()[-25:]
            print("log tail:\n  " + "\n  ".join(tail))
        if output:
            print("stdout/stderr tail:\n  " + "\n  ".join(output.splitlines()[-25:]))
    ok = code == 0 and status == "ok"
    if not summary and os.path.isdir(out):
        wit = crash_witness(out)
        if wit:
            print("crash witness: " + os.path.relpath(wit, REPO)
                  + "  (why: no summary.json - see docs/HARNESS.md, Machine-level resets)")
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
