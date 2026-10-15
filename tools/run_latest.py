"""Start the existing build/Release exe (the tree the harness builds), without building anything.

    python tools/run_latest.py [--no-gate] [-- app args...]

Runs the newest SAT_LIGHT_SIM_V_* in build/Release (build/ on single-config generators); old
versioned exes linger there, hence "newest". Every dev tree shares one user data folder (user_data/,
see cmake/EnsureUserDataDir.cmake). This is a normal interactive launch: your settings, sound, no script.

It goes through the harness's launch gate (tools/harness/launchgate.py: 30 s after the last app
launch or exit, docs/FREEZES.md) and records the exit, so harness runs space themselves after it.
"""
import glob
import os
import subprocess
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(REPO, "tools", "harness"))
import launchgate  # noqa: E402

TREES = ["build/Release", "build"]


def main():
    args = sys.argv[1:]
    gate = "--no-gate" not in args
    app_args = args[args.index("--") + 1:] if "--" in args else []
    found = []
    for tree in TREES:
        found += [p for p in glob.glob(os.path.join(REPO, tree, "SAT_LIGHT_SIM_V_*"))
                  if os.path.isfile(p) and os.path.splitext(p)[1] in ("", ".exe")]
    if not found:
        sys.exit("no build found: build one first (cmake --build build --config Release)")
    exe = max(found, key=os.path.getmtime)
    print(f"running {os.path.relpath(exe, REPO)}", flush=True)
    if gate:
        launchgate.wait()
    try:
        code = subprocess.run([exe] + app_args, cwd=os.path.dirname(exe)).returncode
    finally:
        launchgate.mark_exit()
    sys.exit(code)


if __name__ == "__main__":
    main()
