"""Launch spacing for the harness (docs/HARNESS.md, "Launch spacing").

Relaunching the app right after it exits is the pattern the machine freezes cluster on
(docs/FREEZES.md: selftest's det_a exited and det_b, launched 2 s later, froze the machine).
Every launch waits until COOLDOWN_S have passed since the last app exit AND since the last
launch, and until fewer than MAX_APPS app processes are running; run.py and live.py record the
exit here. Standard library only.

The state is MACHINE-WIDE (not per checkout), so agents working in separate git worktrees share
one gate: %LOCALAPPDATA%/SatLightSimHarness (override with SATLIGHT_HARNESS_STATE). The running
count comes from the process list (any SAT_LIGHT_SIM* / SatLightSim* exe, the user's own
included), so a crashed run never leaves a stale slot behind. SATLIGHT_HARNESS_MAX_APPS sets the
cap (default 2).
"""
import os
import re
import subprocess
import tempfile
import time

STATE_DIR = os.environ.get("SATLIGHT_HARNESS_STATE") or os.path.join(
    os.environ.get("LOCALAPPDATA") or tempfile.gettempdir(), "SatLightSimHarness")
STAMP = os.path.join(STATE_DIR, ".last_app_exit")
LAUNCH_STAMP = os.path.join(STATE_DIR, ".last_app_launch")
LOCK = os.path.join(STATE_DIR, ".launch.lock")
COOLDOWN_S = 30.0
MAX_APPS = int(os.environ.get("SATLIGHT_HARNESS_MAX_APPS", "2"))
_APP_RE = re.compile(r"^sat_?light_?sim", re.IGNORECASE)


def running_apps():
    """Number of app processes running on this machine (any build, any checkout)."""
    try:
        if os.name == "nt":
            out = subprocess.run(["tasklist", "/FO", "CSV", "/NH"], capture_output=True, text=True,
                                 errors="replace", timeout=15).stdout
            names = [line.split('","')[0].strip('"') for line in out.splitlines() if line.strip()]
        else:
            out = subprocess.run(["ps", "-eo", "comm="], capture_output=True, text=True, timeout=15).stdout
            names = [os.path.basename(line.strip()) for line in out.splitlines()]
    except (OSError, subprocess.SubprocessError):
        return 0
    return sum(1 for n in names if _APP_RE.match(n))


def _age(path):
    try:
        return time.time() - os.path.getmtime(path)
    except OSError:
        return None


def _touch(path):
    try:
        os.makedirs(STATE_DIR, exist_ok=True)
        with open(path, "w") as f:
            f.write(str(time.time()))
    except OSError:
        pass


def _try_lock():
    os.makedirs(STATE_DIR, exist_ok=True)
    age = _age(LOCK)
    if age is not None and age > 60:      # a waiter died holding it
        try:
            os.remove(LOCK)
        except OSError:
            pass
    try:
        os.close(os.open(LOCK, os.O_CREAT | os.O_EXCL | os.O_WRONLY))
        return True
    except OSError:
        return False


def wait(cooldown=COOLDOWN_S):
    """Block until a launch is allowed, then record the launch. Call right before starting the app."""
    said = None
    while True:
        if _try_lock():
            try:
                e, l, n = _age(STAMP), _age(LAUNCH_STAMP), running_apps()
                left = max(cooldown - e if e is not None else 0.0, cooldown - l if l is not None else 0.0, 0.0)
                if left <= 0 and n < MAX_APPS:
                    _touch(LAUNCH_STAMP)
                    return
                why = (f"{n} app(s) running (max {MAX_APPS})" if n >= MAX_APPS
                       else f"spacing: {left:.0f} s since the last app launch/exit")
            finally:
                try:
                    os.remove(LOCK)
                except OSError:
                    pass
        else:
            why = "another launcher holds the gate"
        if why != said:
            print(f"launch gate: waiting ({why})", flush=True)
            said = why
        time.sleep(2.0)


def mark_exit():
    """Record that an app process has just exited."""
    _touch(STAMP)
