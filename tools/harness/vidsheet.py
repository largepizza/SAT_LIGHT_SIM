#!/usr/bin/env python3
"""Look at a harness video without watching it (2026-11-06, the storm look work: "use videos for validation").

    tools/harness/.venv/Scripts/python tools/harness/vidsheet.py <video.mp4> [-o sheet.png] [--n 16] [--cols 4]
                                                                  [--sim-s-per-frame 5] [--crop x,y,w,h]

Writes a contact sheet of n frames evenly spaced over the clip (labelled with the frame and, given the sim step, the sim
time), and under it the frame-to-frame change over EVERY frame (mean |luminance difference| of a 1/8-size copy, each
frame normalised to its own mean and spread so a sunset or the exposure is not a change), with the median, the spikes
(> 3x the median) and their frames printed as JSON. The encoder's keyframes (every --gop frames) are
replaced by their neighbours' mean: they re-code the whole picture. A smooth evolution is a flat-ish curve; a pop, a jump or a
regeneration that replaces a cloud is a spike. Needs ffmpeg on PATH and the harness venv (Pillow + numpy).
"""
import argparse
import json
import os
import subprocess
import sys

import numpy as np
from PIL import Image, ImageDraw

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from imgtools import font  # noqa: E402


def probe(path):
    out = subprocess.run(["ffprobe", "-v", "error", "-select_streams", "v:0", "-count_packets", "-show_entries",
                          "stream=width,height,nb_read_packets", "-of", "json", path], capture_output=True, text=True)
    s = json.loads(out.stdout)["streams"][0]
    return int(s["width"]), int(s["height"]), int(s["nb_read_packets"])


def frames_gray(path, w, h, crop):
    vf = (f"crop={crop[2]}:{crop[3]}:{crop[0]}:{crop[1]}," if crop else "") + f"scale={w}:{h},format=gray"
    p = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vf", vf, "-f", "rawvideo", "-"], capture_output=True)
    a = np.frombuffer(p.stdout, dtype=np.uint8)
    return a.reshape(-1, h, w).astype(np.float32)


def frame_rgb(path, i, crop):
    vf = f"select=eq(n\\,{i})" + (f",crop={crop[2]}:{crop[3]}:{crop[0]}:{crop[1]}" if crop else "")
    p = subprocess.run(["ffmpeg", "-v", "error", "-i", path, "-vf", vf, "-vsync", "0", "-frames:v", "1", "-f", "image2pipe",
                        "-vcodec", "png", "-"], capture_output=True)
    from io import BytesIO
    return Image.open(BytesIO(p.stdout)).convert("RGB")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("video")
    ap.add_argument("-o", "--out")
    ap.add_argument("--n", type=int, default=16)
    ap.add_argument("--cols", type=int, default=4)
    ap.add_argument("--width", type=int, default=480)
    ap.add_argument("--sim-s-per-frame", type=float, default=0.0)
    ap.add_argument("--crop", help="x,y,w,h in the video's pixels")
    ap.add_argument("--gop", type=int, default=120, help="the encoder's keyframe interval: those frames' change is the codec "
                    "re-coding the whole picture, not the sim (the harness's mf-hw H.264: 120); 0 = keep")
    a = ap.parse_args()
    crop = [int(v) for v in a.crop.split(",")] if a.crop else None
    W, H, N = probe(a.video)
    cw, ch = (crop[2], crop[3]) if crop else (W, H)
    g = frames_gray(a.video, max(cw // 8, 16) // 2 * 2, max(ch // 8, 16) // 2 * 2, crop)
    z = (g - g.mean(axis=(1, 2), keepdims=True)) / np.maximum(g.std(axis=(1, 2), keepdims=True), 1.0) * 40.0
    d = np.abs(np.diff(z, axis=0)).mean(axis=(1, 2))
    if a.gop > 0:   # (a keyframe's change: the neighbours' mean)
        for k in range(a.gop - 1, len(d), a.gop):
            nb = [d[j] for j in (k - 1, k + 1) if 0 <= j < len(d)]
            d[k] = float(np.mean(nb)) if nb else d[k]
    med = float(np.median(d)) if len(d) else 0.0
    spikes = [(int(i) + 1, round(float(v) / max(med, 1e-6), 1)) for i, v in enumerate(d) if v > 3.0 * med]

    fnt = font(15)
    idx = [int(round(k * (len(g) - 1) / max(a.n - 1, 1))) for k in range(a.n)]
    tiles = []
    for i in idx:
        im = frame_rgb(a.video, i, crop)
        im = im.resize((a.width, int(im.height * a.width / im.width)), Image.BOX)
        lab = f"f{i}" + (f"  t+{i * a.sim_s_per_frame / 60.0:.0f} min" if a.sim_s_per_frame else "")
        tiles.append((lab, im))
    th = tiles[0][1].height + 20
    rows = (len(tiles) + a.cols - 1) // a.cols
    plotH = 140
    sheet = Image.new("RGB", (a.cols * (a.width + 4), rows * th + plotH + 10), (24, 24, 24))
    dr = ImageDraw.Draw(sheet)
    for k, (lab, im) in enumerate(tiles):
        x, y = (k % a.cols) * (a.width + 4), (k // a.cols) * th
        sheet.paste(im, (x, y + 20))
        dr.text((x + 4, y + 2), lab, fill=(230, 230, 230), font=fnt)
    # The change curve.
    y0 = rows * th + 5
    PW = sheet.width - 10
    top = max(float(d.max()) if len(d) else 1.0, 3.0 * med, 1e-6)
    pts = [(5 + PW * i / max(len(d) - 1, 1), y0 + plotH - plotH * float(v) / top) for i, v in enumerate(d)]
    dr.line([(5, y0 + plotH - plotH * 3.0 * med / top), (5 + PW, y0 + plotH - plotH * 3.0 * med / top)], fill=(120, 60, 60))
    if len(pts) > 1:
        dr.line(pts, fill=(120, 200, 255))
    dr.text((8, y0), f"frame change (median {med:.2f}; red = 3x)", fill=(200, 200, 200), font=fnt)
    out = a.out or os.path.splitext(a.video)[0] + "_sheet.png"
    sheet.save(out)
    print(json.dumps({"sheet": out, "frames": int(len(g)), "median": round(med, 3), "max": round(float(d.max()), 3) if len(d) else 0,
                      "spikes": spikes[:40], "n_spikes": len(spikes)}, indent=1))


if __name__ == "__main__":
    main()
