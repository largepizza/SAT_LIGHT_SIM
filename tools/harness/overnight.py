#!/usr/bin/env python3
"""Run the harness in a loop until the machine freezes, and leave a trail to read in the morning
(docs/HARNESS.md, "The flight recorder").

    python tools/harness/overnight.py --iterations 2            # prove the loop works
    python tools/harness/overnight.py --hours 10 --fix-power    # the unattended night

One launch is one chance in the dark. The freezes land 0.5-3 s into a launch, leave no dump and no
bugcheck, and answer no keyboard interrupt: the only evidence that exists is what was already on disk
before the power went. This loop buys a few hundred launches while nobody is awake, and each one leaves
a run folder whose satlight_log.txt names the last step it reached (2026-09-26's entry 6: `init:
texture: assets/textures/earth_elevation.png`, the launch's slowest step).

harness_runs/overnight/ is the trail:

  overnight.log     every event, one line, fsynced - the loop's own flight recorder. Its tail is the
                    first thing to read in the morning: the last iteration, what it ran, and the last
                    step the app logged.
  last_launch.json  written *before* every launch: which run folder is live right now. Nothing after a
                    freeze runs, so this pointer has to exist already - it is how the morning finds the
                    run that died without guessing from timestamps.
  loop.csv          marks in the recording format (epoch,utc,mark,text): iteration starts and results.
                    Read it back with `blackbox.py --summary harness_runs/overnight/loop.csv`.
  runs/NNN_.../     the run folders themselves: results.jsonl, captures, the app's log, and the run's
                    own blackbox.csv (100 ms, bracketed by `mark launch` / `mark exit`).
  iter_NNN_run.txt  run.py's output for that iteration: status line, log tail, crash witness.
  overnight.pid     the loop's lock, so two loops cannot fight over the machine.

The machine-wide recorder is checked every pass and restarted if it has died: under pythonw a dead
recorder is invisible, and the whole point of the night is that it is there when the freeze happens. It
is also restarted if it is *alive at the wrong rate*: the rate is persisted nowhere - the lock names the
pid and nothing else, and the day file's own `blackbox start` mark is the only place it was ever written
down - so this loop reads that mark back instead of assuming one. (Half the resolution of the one
recording that matters is not something to discover in the morning.) The recorder is **opt-in** on a
machine that is used for other work, so this loop starts (and at the end stops) its own - it never
installs a logon launcher and never touches a recorder that was already running.
--fix-power sets the idle timeouts that would change the machine's power state mid-night to 'never'
- here: hard disk after 20 min, display after 15 min (AC) / 10 min (DC), both Windows defaults that
outlive the High performance plan's standby and hibernate, which are already 'never' - and reports
what it changed. A display-off or spindown in the middle of a soak is at best noise and at worst the
GPU/display power transition being chased.

Stopping: Ctrl-C, --hours, or --iterations. A freeze takes the loop with it - that is the signal. Nothing
comes back by itself after the reboot (the recorder is opt-in and this loop is on no logon hook), so the
machine is yours again and nothing overwrites the evidence: read overnight.log's tail, then relaunch.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.abspath(os.path.join(HERE, "..", ".."))
sys.path.insert(0, HERE)
import blackbox  # noqa: E402  (same folder; stdlib only, no package)

OUT = os.path.join(REPO, "harness_runs", "overnight")
RUNS = os.path.join(OUT, "runs")
LOG = os.path.join(OUT, "overnight.log")
MARKS = os.path.join(OUT, "loop.csv")
POINTER = os.path.join(OUT, "last_launch.json")
LOCK = os.path.join(OUT, "overnight.pid")
PYW = os.path.join(os.path.dirname(sys.executable), "pythonw.exe")

# powercfg: (SUB group, setting) to read + the name `powercfg /change` takes. 0 seconds means never.
POWER = {"standby": ("SUB_SLEEP", "STANDBYIDLE", "standby-timeout"),
         "hibernate": ("SUB_SLEEP", "HIBERNATEIDLE", "hibernate-timeout"),
         "disk": ("SUB_DISK", "DISKIDLE", "disk-timeout"),
         "display": ("SUB_VIDEO", "VIDEOIDLE", "monitor-timeout")}

# A recorder that has just been (re)started keeps the GPU awake for ~45 s: its streaming nvidia-smi is an
# NVML client, and the GPU stays at P0 while one is attached (measured 2026-09-26: restart at 08:15:28,
# back at P8 by 08:16:16). The first launch waits that out, or it would start warm and never cross the
# idle -> load step - the transition the resets sit on - which is the one thing this loop is here for.
RECORDER_SETTLE = 50.0

# The recorders this loop started itself (see recorder()). The machine-wide recorder is opt-in: nothing
# starts one at logon, so a soak starts its own - and stops it again on the way out, rather than leaving a
# 100 ms sampler behind on a machine that is used for other work.
OURS = []


def log(msg):
    """One fsynced line. This file is what survives if the machine does not - and it is the only output
    there is under pythonw, where sys.stdout is None."""
    line = "%s  %s\n" % (blackbox._utc(time.time()), msg)
    with open(LOG, "a", encoding="utf-8") as f:
        f.write(line)
        f.flush()
        try:
            os.fsync(f.fileno())
        except OSError:
            pass
    if sys.stdout is not None:
        try:
            sys.stdout.write(line)
            sys.stdout.flush()
        except Exception:
            pass


def mark(text):
    """A row in loop.csv, in the recording format, so `blackbox.py --summary harness_runs/overnight/
    loop.csv` reads the loop's timeline back next to the telemetry."""
    blackbox.write_row(MARKS, "mark", [text])


def pointer(fields):
    """last_launch.json, written and fsynced *before* the app starts. After a freeze nothing else runs,
    so the run folder that was live has to be written down in advance."""
    tmp = POINTER + ".tmp"
    with open(tmp, "w", encoding="utf-8") as f:
        json.dump(fields, f, indent=2, sort_keys=True)
        f.write("\n")
        f.flush()
        os.fsync(f.fileno())
    os.replace(tmp, POINTER)


def find_exe():
    """The newest app build, whichever tree it is in. run.py looks only under build/, which is how a
    reset got spent testing a stale exe (docs/HARNESS.md, Gotchas), so the loop picks here and passes
    --exe: the log then names exactly what ran."""
    found = []
    for tree in ("build", "build-win-release"):
        pat = os.path.join(REPO, tree, "**", "SAT_LIGHT_SIM_V_*.exe")
        found += [f for f in glob.glob(pat, recursive=True)
                  if os.sep + "_deps" + os.sep not in f and os.sep + "CMakeFiles" + os.sep not in f]
    return max(found, key=os.path.getmtime) if found else None


def build_stamp(exe):
    """(commit, date) the exe was configured with, from the build tree's generated/version.h - the same
    pair a run log's first line carries. It is stamped when CMake runs, not when the exe was linked."""
    d = os.path.dirname(os.path.abspath(exe))
    for _ in range(3):
        h = os.path.join(d, "generated", "version.h")
        if os.path.exists(h):
            commit = date = "?"
            with open(h, encoding="utf-8", errors="replace") as f:
                for line in f:
                    if "APP_GIT_COMMIT" in line and '"' in line:
                        commit = line.split('"')[1]
                    elif "APP_BUILD_DATE" in line and '"' in line:
                        date = line.split('"')[1]
            return commit, date
        d = os.path.dirname(d)
    return "?", "?"


def recorder_rate():
    """(pid, gpu_ms) of the recorder that wrote the newest `blackbox start` mark in the day files, or
    None. That mark is the only place the sampling rate is written down (the lock names just the pid) and
    nothing persists it, so a recorder that is not this loop's own - started by hand, or by the logon
    launcher of an older setup - can be at any rate at all: the rate is read back from the file, never
    assumed."""
    found = []
    for off in (0, -1):  # today, then yesterday, for a night that straddles UTC midnight
        day = time.strftime("%Y%m%d", time.gmtime(time.time() + off * 86400))
        for t, src, fields in blackbox.read(os.path.join(blackbox.DAEMON_DIR, day + ".csv")):
            tok = fields[0].split() if src == "mark" and fields else []
            if tok[:2] == ["blackbox", "start"] and "pid" in tok and "gpu_ms" in tok:
                found.append((t, int(tok[tok.index("pid") + 1]), int(tok[tok.index("gpu_ms") + 1])))
    return max(found)[1:] if found else None


def stop_recorder(pid):
    """Stop a recorder by pid. Killing it is the only way: the daemon re-reads nothing (it sleeps an hour
    between prunes), so deleting its lock would leave it writing rows into a file a second recorder owns."""
    try:
        subprocess.run(["taskkill", "/pid", str(pid), "/f"], stdout=subprocess.DEVNULL,
                       stderr=subprocess.STDOUT)
    except OSError as e:
        log("recorder: could not stop pid %d: %r" % (pid, e))
        return False
    for _ in range(20):
        if not blackbox.DaemonLock().holder():
            return True
        time.sleep(0.5)
    return False


def recorder(gpu_ms):
    """The pid of the live recorder, at the sampling rate this loop asked for.

    The recorder is opt-in on a machine that is used for other work (nothing starts one at logon), so this
    loop is what starts one for a soak - and main()'s finally stops the one it started, because a 100 ms
    sampler is this loop's business and not the machine's. It is also the only thing that can record a
    freeze with no run live, and pythonw hides its death, so it is checked every pass. A recorder that is
    alive at the *wrong* rate is the other trap: the rate is not persisted, the whole point of --gpu-ms is
    that a freeze can end mid-sample, so a mismatched holder is restarted once here - and then the first
    launch waits out the wake-up (RECORDER_SETTLE)."""
    holder = blackbox.DaemonLock().holder()
    if holder:
        rate = recorder_rate()
        if not rate or rate[0] != holder or rate[1] == gpu_ms:
            log("recorder: pid %d is recording%s"
                % (holder, "" if not rate else " (gpu_ms %d)" % rate[1]))
            return holder
        log("recorder: pid %d is recording at gpu_ms %d, this loop wants %d - restarting it"
            % (holder, rate[1], gpu_ms))
        if not stop_recorder(holder):
            log("recorder: pid %d would not stop - leaving it (the night records at gpu_ms %d)"
                % (holder, rate[1]))
            return holder
    else:
        log("recorder: no live holder in %s - starting one (gpu_ms %d)"
            % (os.path.relpath(blackbox.DAEMON_LOCK, REPO), gpu_ms))
    py = PYW if os.path.exists(PYW) else sys.executable
    flags = getattr(subprocess, "CREATE_NO_WINDOW", 0) | getattr(subprocess, "DETACHED_PROCESS", 0)
    try:
        subprocess.Popen([py, os.path.join(HERE, "blackbox.py"), "--daemon", "--gpu-ms", str(gpu_ms)],
                         cwd=REPO, stdin=subprocess.DEVNULL, stdout=subprocess.DEVNULL,
                         stderr=subprocess.DEVNULL, creationflags=flags, close_fds=True)
    except OSError as e:
        log("recorder: could not start it: %r" % (e,))
        return 0
    for _ in range(24):  # a few seconds, or it writes why not to harness_runs/blackbox/daemon.log
        time.sleep(0.5)
        holder = blackbox.DaemonLock().holder()
        if holder:
            break
    if holder:
        log("recorder: pid %d is recording (gpu_ms %d)" % (holder, gpu_ms))
        log("recorder: waiting %.0fs for the GPU to fall back to idle before the first launch - a "
            "recorder that just started holds it at P0 (measured ~45 s)" % RECORDER_SETTLE)
        time.sleep(RECORDER_SETTLE)
        OURS.append(holder)  # main()'s finally stops it: see "the recorder is opt-in" in the docstring
    else:
        log("recorder: STILL not running - see harness_runs/blackbox/daemon.log")
    return holder


def power_state():
    """{name: {'ac': seconds, 'dc': seconds}} - 0 is never. powercfg /q prints the indexes in hex."""
    state = {}
    for name, (sub, setting, _) in POWER.items():
        p = subprocess.run(["powercfg", "/q", "SCHEME_CURRENT", sub, setting],
                           stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, errors="replace")
        vals = {}
        for line in (p.stdout or "").splitlines():
            s = line.strip()
            for scope in ("AC", "DC"):
                if s.startswith("Current %s Power Setting Index:" % scope):
                    try:
                        vals[scope.lower()] = int(s.split(":", 1)[1].strip(), 16)
                    except (IndexError, ValueError):
                        pass
        if vals:
            state[name] = vals
    return state


def fix_power():
    """Idle timeouts to never, where they are not, then read back. powercfg /q can still answer from the
    power service's cache for a moment after /change, so the read back is what says whether it took."""
    state = power_state()
    changed = []
    for name, (_, _, change) in POWER.items():
        for scope in ("ac", "dc"):
            old = state.get(name, {}).get(scope, 0)
            if old:
                subprocess.run(["powercfg", "/change", "%s-timeout-%s" % (change, scope), "0"],
                               stdout=subprocess.DEVNULL, stderr=subprocess.STDOUT)
                changed.append("%s %s: %ds -> never" % (name, scope, old))
    if not changed:
        log("power: nothing to change - no idle timeout would change the machine's state mid-night")
        return changed
    for line in changed:
        log("power: " + line)
    time.sleep(2.0)
    log("power: after the change, still set: " + risky_power())
    return changed


def risky_power():
    """One line naming every idle timeout that is still set, for the header of the night's log."""
    state = power_state()
    risk = ["%s/%s=%ds" % (name, scope, v) for name, d in sorted(state.items())
            for scope, v in sorted(d.items()) if v]
    return ", ".join(risk) if risk else "none (standby/hibernate/disk/display: never)"


def read_run(run_dir):
    """(status, errors, last log line, last `init:` breadcrumb, last sample epoch) of one run folder -
    which, after a freeze, is the folder the machine died in, so any of these may be missing."""
    status, errors = "no summary (crashed?)", "?"
    sp = os.path.join(run_dir, "summary.json")
    if os.path.exists(sp):
        try:
            with open(sp, encoding="utf-8") as f:
                d = json.load(f)
            status, errors = d.get("status", status), d.get("errors", errors)
        except (OSError, ValueError):
            pass
    last = init = ""
    lp = os.path.join(run_dir, "satlight_log.txt")
    if os.path.exists(lp):
        with open(lp, encoding="utf-8", errors="replace") as f:
            for line in f:
                line = line.rstrip()
                if line.startswith("["):
                    last = line
                    if "init:" in line:
                        init = line
    end = None
    bb = os.path.join(run_dir, "blackbox.csv")
    if os.path.exists(bb):
        rows = blackbox.read(bb)
        end = rows[-1][0] if rows else None
    return status, errors, last, init, end


def one_iteration(n, script, exe, timeout, gap, pid):
    """One launch: from the pointer that says it is live, to the line that says how it went."""
    stem = os.path.splitext(os.path.basename(script))[0]
    run_dir = os.path.join(RUNS, "%03d_%s_%s" % (n, time.strftime("%Y%m%d_%H%M%S"), stem))
    day = os.path.join(blackbox.DAEMON_DIR, time.strftime("%Y%m%d", time.gmtime()) + ".csv")

    def rel(p):
        return os.path.relpath(p, REPO)

    pointer({"iteration": n, "script": rel(script), "exe": rel(exe), "run_dir": rel(run_dir),
             "started_utc": blackbox._utc(time.time()), "recorder_pid": pid,
             "app_log": rel(os.path.join(run_dir, "satlight_log.txt")),
             "run_blackbox": rel(os.path.join(run_dir, "blackbox.csv")),
             "recorder_day_file": rel(day)})
    mark("iter %d start %s" % (n, stem))
    t0 = time.time()
    cap = timeout + 180  # run.py kills a hung app itself; this only catches run.py itself wedging
    out_path = os.path.join(OUT, "iter_%03d_run.txt" % n)
    rc = 0
    try:
        # The child's stdout is a file, not a pipe: under pythonw there is no console, and an invalid
        # standard handle is what makes pythonw hand a process sys.stdout = None, where a print() raises.
        # A real file handle keeps run.py's output (and its own pipes to the app) working.
        with open(out_path, "w", encoding="utf-8", errors="replace") as fo:
            p = subprocess.run([sys.executable, os.path.join(HERE, "run.py"), script, "--forensics", "--exe", exe,
                                "--out", run_dir, "--timeout", str(timeout)],
                               cwd=REPO, stdout=fo, stderr=subprocess.STDOUT, text=True, timeout=cap)
            rc = p.returncode
    except subprocess.TimeoutExpired:
        rc = "loop-killed after %gs" % cap
    wall = time.time() - t0
    status, errors, last, init, end = read_run(run_dir)
    log("iter %d %s: rc=%s status=%s errors=%s wall=%.1fs  %s"
        % (n, stem, rc, status, errors, wall, rel(run_dir)))
    if last:
        log("iter %d   last log line: %s" % (n, last))
    if init and init != last:
        log("iter %d   last breadcrumb: %s" % (n, init))
    if end:
        log("iter %d   run blackbox ends %s" % (n, blackbox._utc(end)))
    mark("iter %d done rc=%s status=%s wall=%.1fs" % (n, rc, status, wall))
    if status != "ok" or rc != 0:
        # The interesting case: a crash, a kill, or a reset the app survived. Keep the app's log where a
        # relaunch cannot rename it, and put its tail in this log - the one file the morning is certain
        # to read.
        src = os.path.join(run_dir, "satlight_log.txt")
        if os.path.exists(src):
            dst = os.path.join(OUT, "iter_%03d_app_log.txt" % n)
            with open(src, encoding="utf-8", errors="replace") as fi, \
                    open(dst, "w", encoding="utf-8", errors="replace") as fo:
                fo.write(fi.read())
            with open(dst, encoding="utf-8", errors="replace") as f:
                lines = f.read().splitlines()
            log("iter %d   kept the app log: %s  tail: %s"
                % (n, rel(dst), " | ".join(lines[-3:]) if lines else "(empty)"))
    time.sleep(gap)
    return rc, status


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--scripts", nargs="+", default=["smoke.satcmd", "viewer_glare.satcmd"],
                    help="scripts to rotate through, by name under tools/harness/scripts/ or by path "
                         "(default: smoke.satcmd viewer_glare.satcmd)")
    ap.add_argument("--exe", help="app exe (default: the newest SAT_LIGHT_SIM_V_*.exe under build*/)")
    ap.add_argument("--timeout", type=float, default=240, help="seconds before a run is killed (default 240)")
    ap.add_argument("--gap", type=float, default=15,
                    help="idle seconds between runs: every launch then re-crosses idle -> load, and the "
                         "PCIe link is back at gen1 before it retrains (the transition the resets sit "
                         "on). Measured 2026-09-26: that idle state returns ~4 s after the app exits, so "
                         "15 s is ample and a shorter gap starts warm (default 15)")
    ap.add_argument("--hours", type=float, default=0, help="stop after this many hours (default: never)")
    ap.add_argument("--iterations", type=int, default=0, help="stop after this many runs (default: never)")
    ap.add_argument("--gpu-ms", type=int, default=100,
                    help="sampling period to record at (default 100). A recorder that is already live at "
                         "a different rate is restarted to match - the rate is not persisted anywhere, so "
                         "a reset quietly brings the launcher's 250 ms default back")
    ap.add_argument("--fix-power", action="store_true",
                    help="set the idle timeouts that would sleep the machine mid-night to 'never'")
    a = ap.parse_args()

    os.makedirs(RUNS, exist_ok=True)
    lock = blackbox.DaemonLock(LOCK)
    other = lock.acquire()
    if other:
        log("overnight: a loop is already running (pid %d, %s) - not starting a second one" % (other, LOCK))
        return 1
    scripts = [s if os.path.isabs(s) else os.path.join(HERE, "scripts", s) for s in a.scripts]
    missing = [s for s in scripts if not os.path.exists(s)]
    if missing:
        log("overnight: no such script: %s" % ", ".join(missing))
        lock.release()
        return 2
    exe = os.path.abspath(a.exe) if a.exe else find_exe()
    if not exe or not os.path.exists(exe):
        log("overnight: no app exe - build it first "
            "(cmake --build build --config Release --target SatLightSim)")
        lock.release()
        return 2

    commit, date = build_stamp(exe)
    try:
        free = shutil.disk_usage(REPO).free / 1e9
    except OSError:
        free = 0.0
    if a.fix_power:
        fix_power()
    if a.hours and a.iterations:
        until = "for %g h or %d runs, whichever comes first" % (a.hours, a.iterations)
    elif a.hours:
        until = "for %g h" % a.hours
    elif a.iterations:
        until = "for %d runs" % a.iterations
    else:
        until = "until stopped"
    log("overnight: start, pid %d - %s" % (os.getpid(), time.strftime("%Y-%m-%d %H:%M:%S")))
    log("overnight: exe %s  %.2f MB, built %s, stamp %s/%s"
        % (os.path.relpath(exe, REPO), os.path.getsize(exe) / 1e6,
           time.strftime("%Y-%m-%d %H:%M:%S", time.localtime(os.path.getmtime(exe))), commit, date))
    log("overnight: scripts %s; timeout %gs; gap %gs; %s"
        % (", ".join(os.path.basename(s) for s in scripts), a.timeout, a.gap, until))
    log("overnight: %.1f GB free; power idle timeouts still set: %s" % (free, risky_power()))
    log("overnight: morning: tail this file, then last_launch.json -> that run's satlight_log.txt (last "
        "breadcrumb) and blackbox.csv; the recorder's day file ends at the freeze")
    log("overnight: stop: kill pid %d, or delete %s" % (os.getpid(), os.path.relpath(LOCK, REPO)))
    # "for N hours" is measured from *here*, not from after the recorder's 50 s settle: --hours 0.0005 has
    # to launch nothing at all, and a night budgeted for 6 h should get 6 h of launches. Measuring it after
    # the settle is how a "1.8 s" test run still launched the app once (2026-09-26, entry 8's reset).
    t0 = time.time()
    pid = recorder(a.gpu_ms)
    mark("overnight start pid %d exe %s" % (os.getpid(), os.path.basename(exe)))

    stop_at = t0 + a.hours * 3600.0 if a.hours else 0.0
    n = 0
    try:
        while not (a.iterations and n >= a.iterations) and not (stop_at and time.time() >= stop_at):
            n += 1
            recorder(a.gpu_ms)  # a dead recorder is invisible under pythonw; the night needs it alive
            one_iteration(n, scripts[(n - 1) % len(scripts)], exe, a.timeout, a.gap, pid)
    except KeyboardInterrupt:
        log("overnight: stopped by hand after %d runs" % n)
    finally:
        mark("overnight stop after %d runs" % n)
        log("overnight: stopped after %d runs - if the last run has no summary.json, this log's tail and "
            "last_launch.json are the record" % n)
        # The recorder is opt-in here: stop the one this loop started (never one that was already running).
        for pid in OURS:
            if blackbox.DaemonLock().holder() == pid:
                stopped = stop_recorder(pid)
                log("recorder: %s pid %d - this loop started it, and a standing 100 ms sampler is not "
                    "something to leave behind" % ("stopped" if stopped else "could not stop", pid))
        lock.release()
    return 0


if __name__ == "__main__":
    sys.exit(main())
