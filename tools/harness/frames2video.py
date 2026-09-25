#!/usr/bin/env python3
"""Assemble a recorded `path play record=<name>` into a video with ffmpeg (docs/HARNESS.md).

    python tools/harness/frames2video.py harness_runs/<run>/captures/<name> -o out.mp4 [--fps 30]

<name> is the prefix before _00000.png. Needs ffmpeg on PATH (or --ffmpeg). H.264, yuv420p, CRF 18.
"""
import argparse
import glob
import os
import shutil
import subprocess
import sys


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("prefix", help="path to the frames without _00000.png")
    ap.add_argument("-o", "--out", required=True)
    ap.add_argument("--fps", type=float, default=30.0)
    ap.add_argument("--crf", type=int, default=18)
    ap.add_argument("--ffmpeg", default=shutil.which("ffmpeg") or "ffmpeg")
    a = ap.parse_args()
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
