#!/usr/bin/env python3
"""Garbage-collect harness run folders (harness_runs/, harness_live/run_*). Standard library only.

    python tools/harness/gc.py                    # dry run: what would go, and how much space
    python tools/harness/gc.py --apply            # delete it
    python tools/harness/gc.py --keep-days 3 --apply
    python tools/harness/gc.py --slim --apply     # also drop frame-sequence recordings from kept runs
    python tools/harness/gc.py --keep <run>       # pin a run (writes <run>/KEEP)

A run is KEPT when any of these holds:
  - it is younger than --keep-days (default 7), by its newest file
  - it has a KEEP file (pin it with --keep, or `touch <run>/KEEP`)
  - its folder name is referenced from CLAUDE.md, docs/, .plans/ or the agent memory folder
    (a design note citing `harness_runs/cloud_v2_p10b` keeps that run, so the evidence stays)
  - it is the live app's current folder (harness_live/session.lock names it) or the newest live run
Everything else is deleted with --apply. --slim additionally deletes `path play record=` frame sequences
(captures/<name>_00000.png ...) from kept runs older than --slim-days (1), keeping every 30th frame, which is
where most of the space goes. Nothing outside harness_runs/ and harness_live/ is touched.
"""
import argparse
import os
import re
import shutil
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
RUNS = os.path.join(REPO, "harness_runs")
LIVE = os.path.join(REPO, "harness_live")
MEMORY = os.path.join(os.path.expanduser("~"), ".claude", "projects", "c--Code-Shader-Fun", "memory")
FRAME_RE = re.compile(r"^(.*)_(\d{5})\.png$")


def dir_stats(path):
    size, newest = 0, 0.0
    for root, _, files in os.walk(path):
        for f in files:
            try:
                st = os.stat(os.path.join(root, f))
            except OSError:
                continue
            size += st.st_size
            newest = max(newest, st.st_mtime)
    return size, newest or os.path.getmtime(path)


def referenced_names():
    """Every token in the notes that could be a run folder name."""
    text = []
    for base in (os.path.join(REPO, "CLAUDE.md"), os.path.join(REPO, "docs"), os.path.join(REPO, ".plans"), MEMORY):
        if os.path.isfile(base):
            paths = [base]
        elif os.path.isdir(base):
            paths = [os.path.join(r, f) for r, _, fs in os.walk(base) for f in fs if f.endswith((".md", ".txt", ".satcmd"))]
        else:
            paths = []
        for p in paths:
            try:
                with open(p, encoding="utf-8", errors="ignore") as fh:
                    text.append(fh.read())
            except OSError:
                pass
    blob = "\n".join(text)
    return set(re.findall(r"harness_(?:runs|live)[/\\]([A-Za-z0-9_.\-]+)", blob))


def live_current():
    lock = os.path.join(LIVE, "session.lock")
    keep = set()
    if os.path.isfile(lock):
        try:
            with open(lock, encoding="utf-8") as fh:
                s = fh.read()
            for m in re.findall(r"run_\d{8}_\d{6}", s):
                keep.add(m)
        except OSError:
            pass
    runs = sorted(d for d in os.listdir(LIVE) if d.startswith("run_")) if os.path.isdir(LIVE) else []
    if runs:
        keep.add(runs[-1])
    return keep


def human(n):
    for u in ("B", "KB", "MB", "GB"):
        if n < 1024 or u == "GB":
            return f"{n:.1f} {u}" if u != "B" else f"{n} B"
        n /= 1024.0


def slim_run(path, apply):
    """Frame sequences (any captures/ folder under the run, nested runs included): keep every 30th frame.
    Returns bytes freed."""
    freed = 0
    for root, _, files in os.walk(path):
        if os.path.basename(root) != "captures":
            continue
        for f in files:
            m = FRAME_RE.match(f)
            if m and int(m.group(2)) % 30 != 0:
                p = os.path.join(root, f)
                freed += os.path.getsize(p)
                if apply:
                    os.remove(p)
    return freed


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--apply", action="store_true", help="delete (default: dry run)")
    ap.add_argument("--keep-days", type=float, default=7.0)
    ap.add_argument("--slim", action="store_true", help="drop frame-sequence recordings from kept runs")
    ap.add_argument("--slim-days", type=float, default=1.0, help="slim kept runs at least this old (default 1)")
    ap.add_argument("--keep", metavar="RUN", help="pin a run folder (writes KEEP) and exit")
    ap.add_argument("-v", "--verbose", action="store_true")
    a = ap.parse_args()

    if a.keep:
        p = a.keep if os.path.isdir(a.keep) else os.path.join(RUNS, a.keep)
        if not os.path.isdir(p):
            sys.exit(f"no such run: {a.keep}")
        open(os.path.join(p, "KEEP"), "w").close()
        print(f"pinned {p}")
        return

    refs = referenced_names()
    live_keep = live_current()
    now = time.time()
    cands = []
    for base, pred in ((RUNS, lambda d: True), (LIVE, lambda d: d.startswith("run_"))):
        if not os.path.isdir(base):
            continue
        for d in sorted(os.listdir(base)):
            p = os.path.join(base, d)
            if os.path.isdir(p) and pred(d):
                cands.append((base, d, p))

    del_bytes = keep_bytes = slim_bytes = 0
    n_del = n_keep = 0
    for base, d, p in cands:
        size, newest = dir_stats(p)
        age_d = (now - newest) / 86400.0
        why = None
        if os.path.exists(os.path.join(p, "KEEP")):
            why = "pinned"
        elif d in refs:
            why = "referenced"
        elif base == LIVE and d in live_keep:
            why = "live"
        elif age_d < a.keep_days:
            why = f"{age_d:.1f} d old"
        if why:
            n_keep += 1
            keep_bytes += size
            if a.slim and age_d >= a.slim_days:
                slim_bytes += slim_run(p, a.apply)
            if a.verbose:
                print(f"  keep  {human(size):>9}  {os.path.relpath(p, REPO)}  ({why})")
        else:
            n_del += 1
            del_bytes += size
            if a.verbose:
                print(f"  del   {human(size):>9}  {os.path.relpath(p, REPO)}  ({age_d:.1f} d old)")
            if a.apply:
                shutil.rmtree(p, ignore_errors=True)

    verb = "deleted" if a.apply else "would delete"
    print(f"{verb} {n_del} runs ({human(del_bytes)}); kept {n_keep} ({human(keep_bytes)})"
          + (f"; slim {'freed' if a.apply else 'would free'} {human(slim_bytes)}" if a.slim else "")
          + ("" if a.apply else "   [dry run: --apply to delete]"))


if __name__ == "__main__":
    main()
