#!/usr/bin/env python3
"""Drive a running app through its live inbox (docs/HARNESS.md). Standard library only.

    python tools/harness/live.py start                     # launch the app with --live harness_live/
    python tools/harness/live.py send "observer lat=46.5 lon=8; wait settle; capture a"
    python tools/harness/live.py send -f some.satcmd
    python tools/harness/live.py status
    python tools/harness/live.py stop                      # sends `quit`

Live mode keeps one app process (and its loaded textures and roster) across many experiments, so
each batch costs only its own frames instead of a ~10 s startup. Every batch is one inbox file; the
answer is outbox/<same name>.json with one result per command. Captures land in the live run
folder that `status` prints.
"""
import argparse
import glob
import json
import os
import subprocess
import sys
import time

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
DEFAULT_DIR = os.path.join(REPO, "harness_live")


def read_json(p):
    try:
        with open(p, encoding="utf-8") as f:
            return json.load(f)
    except (OSError, json.JSONDecodeError):
        return None


def alive(d, max_age=5.0):
    st = read_json(os.path.join(d, "status.json"))
    return st is not None and time.time() - os.path.getmtime(os.path.join(d, "status.json")) < max_age, st


def cmd_start(a):
    ok, _ = alive(a.dir)
    if ok:
        print("already running")
        return
    sys.path.insert(0, os.path.dirname(__file__))
    from run import find_exe
    exe = os.path.abspath(a.exe) if a.exe else find_exe(a.config)
    os.makedirs(a.dir, exist_ok=True)
    for f in glob.glob(os.path.join(a.dir, "inbox", "*")):
        os.remove(f)
    out = os.path.join(a.dir, "run_" + time.strftime("%Y%m%d_%H%M%S"))
    args = [exe, "--live", a.dir, "--out", out, "--window", a.window]
    if a.settings:
        args += ["--settings", os.path.abspath(a.settings)]
    if a.fixed_dt is not None:
        args += ["--fixed-dt", a.fixed_dt]
    import launchgate
    launchgate.wait()  # spacing after the last app exit (docs/HARNESS.md, "Launch spacing")
    flags = 0x00000008 if os.name == "nt" else 0  # DETACHED_PROCESS
    app = subprocess.Popen(args, cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
    # --forensics: the flight recorder (blackbox.py, docs/FREEZES.md), detached like the app, records
    # GPU/CPU telemetry into the run folder until the app's process exits.
    if a.forensics:
        try:
            os.makedirs(out, exist_ok=True)
            subprocess.Popen([sys.executable, os.path.join(os.path.dirname(os.path.abspath(__file__)), "blackbox.py"),
                              "--out", os.path.join(out, "blackbox.csv"), "--until-pid", str(app.pid)],
                             cwd=REPO, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, creationflags=flags)
        except OSError:
            pass
    t0 = time.time()
    while time.time() - t0 < a.timeout:
        ok, st = alive(a.dir)
        if ok:
            print(f"running: {st['run_dir']}")
            return
        time.sleep(0.5)
    sys.exit("app did not come up (see the run folder's satlight_log.txt)")


def cmd_send(a):
    ok, _ = alive(a.dir)
    if not ok:
        sys.exit("no live app (python tools/harness/live.py start)")
    text = open(a.file, encoding="utf-8").read() if a.file else a.commands.replace("; ", "\n")
    name = f"{int(time.time() * 1000)}_{os.getpid()}"
    inbox = os.path.join(a.dir, "inbox")
    tmp = os.path.join(inbox, name + ".tmp")
    with open(tmp, "w", encoding="utf-8") as f:
        f.write(text + "\n")
    os.replace(tmp, os.path.join(inbox, name + ".satcmd"))  # atomic: the app never reads half a file
    out = os.path.join(a.dir, "outbox", name + ".json")
    t0 = time.time()
    while time.time() - t0 < a.timeout:
        res = read_json(out)
        if res is not None:
            bad = 0
            for r in res["results"]:
                if r.get("ok"):
                    print(f"  ok   {r.get('cmd', ''):<56.56} {(r.get('result') or {}).get('message', 'ok')}")
                else:
                    bad += 1
                    print(f"  ERR  {r.get('cmd', '')}\n       -> {r.get('error')}")
            if a.json:
                print(json.dumps(res, indent=1))
            sys.exit(1 if bad else 0)
        time.sleep(0.1)
    sys.exit(f"no answer within {a.timeout}s (is a long command still running? see status)")


def cmd_status(a):
    ok, st = alive(a.dir)
    print(json.dumps({"alive": ok, **(st or {})}, indent=1))


def cmd_stop(a):
    a.commands, a.file, a.json = "quit", None, False
    try:
        cmd_send(a)
    except SystemExit:
        pass
    sys.path.insert(0, os.path.dirname(__file__))
    import launchgate
    launchgate.mark_exit()


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--dir", default=DEFAULT_DIR)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("start")
    s.add_argument("--config", default="Release"); s.add_argument("--exe"); s.add_argument("--window", default="1600x900")
    s.add_argument("--settings"); s.add_argument("--fixed-dt"); s.add_argument("--timeout", type=float, default=90)
    s.add_argument("--forensics", action="store_true", help="record GPU/CPU telemetry to blackbox.csv (docs/FREEZES.md)")
    s.add_argument("--no-blackbox", action="store_true", help=argparse.SUPPRESS)
    s = sp.add_parser("send")
    s.add_argument("commands", nargs="?", default=""); s.add_argument("-f", "--file")
    s.add_argument("--timeout", type=float, default=300); s.add_argument("--json", action="store_true")
    sp.add_parser("status")
    s = sp.add_parser("stop"); s.add_argument("--timeout", type=float, default=30)
    a = ap.parse_args()
    {"start": cmd_start, "send": cmd_send, "status": cmd_status, "stop": cmd_stop}[a.cmd](a)


if __name__ == "__main__":
    main()
