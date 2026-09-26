"""Launch spacing for the harness (docs/HARNESS.md, "Launch spacing").

Relaunching the app right after it exits is the pattern the machine freezes cluster on
(docs/FREEZES.md: selftest's det_a exited and det_b, launched 2 s later, froze the machine).
Every launch waits until COOLDOWN_S have passed since the last app exit; run.py and live.py
record the exit here. Standard library only.
"""
import os
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
STAMP = os.path.join(REPO, "harness_runs", ".last_app_exit")
COOLDOWN_S = 30.0


def wait(cooldown=COOLDOWN_S):
    """Sleep until `cooldown` seconds have passed since the last recorded app exit."""
    try:
        last = os.path.getmtime(STAMP)
    except OSError:
        return
    left = cooldown - (time.time() - last)
    if left > 0:
        print(f"launch spacing: waiting {left:.0f} s since the last app exit", flush=True)
        time.sleep(left)


def mark_exit():
    """Record that an app process has just exited."""
    try:
        os.makedirs(os.path.dirname(STAMP), exist_ok=True)
        with open(STAMP, "w") as f:
            f.write(str(time.time()))
    except OSError:
        pass
