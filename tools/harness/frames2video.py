#!/usr/bin/env python3
"""Assemble a recorded `path play record=<name>` into a video with ffmpeg (docs/HARNESS.md).

    python tools/harness/frames2video.py harness_runs/<run>/captures/<name> -o out.mp4 [--fps 30]
    python tools/harness/frames2video.py screenshots/cinematics/<export folder> [-o out.mp4]

<name> is the prefix before _00000.png. A cinematic export FOLDER (frame_00000.png ...) takes its fps from
the folder's cinematic.json and writes <folder>.mp4 beside it unless -o / --fps say otherwise.
Needs ffmpeg on PATH (or --ffmpeg). H.264, yuv420p, CRF 18. cine2mp4.bat at the repo root wraps this.
"""
import argparse
import glob
import json
import os
import shutil
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix", help="path to the frames without _00000.png, or a cinematic export folder")
    ap.add_argument("-o", "--out")
    ap.add_argument("--fps", type=float)
    ap.add_argument("--crf", type=int, default=18)
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    a = ap.parse_args()
    if os.path.isdir(a.prefix):
        folder = os.path.normpath(a.prefix)
        if a.fps is None:
            try:
                with open(os.path.join(folder, "cinematic.json"), encoding="utf-8") as f:
                    a.fps = float(json.load(f).get("fps", 30.0))
            except (OSError, ValueError):
                pass
        if a.out is None:
            a.out = folder + ".mp4"
        a.prefix = os.path.join(folder, "frame")
    if a.out is None:
        sys.exit("-o is required unless the input is an export folder")
    if a.fps is None:
        a.fps = 30.0
    frames = sorted(glob.glob(a.prefix + "_[0-9][0-9][0-9][0-9][0-9].png"))
    if not frames:
        sys.exit(f"no frames matching {a.prefix}_#####.png")
    cmd = [a.ffmpeg, "-y", "-loglevel", "error", "-framerate", str(a.fps), "-i", a.prefix + "_%05d.png",
           "-c:v", "libx264", "-pix_fmt", "yuv420p", "-crf", str(a.crf),
           "-vf", "pad=ceil(iw/2)*2:ceil(ih/2)*2", a.out]
    r = subprocess.run(cmd)
    if r.returncode != 0:
        sys.exit(r.returncode)
    print(f"{len(frames)} frames -> {os.path.abspath(a.out)} ({len(frames) / a.fps:.1f} s at {a.fps:g} fps)")


if __name__ == "__main__":
    main()
