#!/usr/bin/env python3
"""Contact sheets for a cinematic export (the release-cinematic audit, docs/HARNESS.md "Cinematics").

    tools/harness/.venv/Scripts/python tools/harness/cine_sheets.py <export dir> [-o <out dir>] [--notes notes.json]
        [--fps 30] [--grid]

<export dir> holds frame_NNNNN.png + cinematic.json (`cine export preview`). For every shot: its start, middle and
end frames side by side, with the shot number, name and a composition note (notes.json: {"<shot name>": "note"}),
and a rule-of-thirds grid when --grid. Plus overview.png: one row per shot. Needs Pillow.
"""
import argparse
import json
import os
import sys

from PIL import Image, ImageDraw, ImageFont


def font(size):
    for f in ("arial.ttf", "DejaVuSans.ttf", "segoeui.ttf"):
        try:
            return ImageFont.truetype(f, size)
        except OSError:
            pass
    return ImageFont.load_default()


def thumb(path, w, grid):
    im = Image.open(path).convert("RGB")
    h = round(im.height * w / im.width)
    im = im.resize((w, h), Image.LANCZOS)
    if grid:
        d = ImageDraw.Draw(im, "RGBA")
        for k in (1, 2):
            d.line([(w * k // 3, 0), (w * k // 3, h)], fill=(255, 255, 255, 70))
            d.line([(0, h * k // 3), (w, h * k // 3)], fill=(255, 255, 255, 70))
    return im


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("dir")
    ap.add_argument("-o", "--out")
    ap.add_argument("--notes")
    ap.add_argument("--fps", type=float)
    ap.add_argument("--grid", action="store_true")
    ap.add_argument("--width", type=int, default=620)
    a = ap.parse_args()
    out = a.out or os.path.join(a.dir, "sheets")
    os.makedirs(out, exist_ok=True)
    cj = json.load(open(os.path.join(a.dir, "cinematic.json"), encoding="utf-8-sig"))
    fps = a.fps or cj.get("fps", 30.0)
    notes = json.load(open(a.notes, encoding="utf-8")) if a.notes else {}
    frames = sorted(f for f in os.listdir(a.dir) if f.startswith("frame_") and f.endswith(".png"))
    if not frames:
        sys.exit("no frames")
    start = 0
    rows = []
    for i, s in enumerate(cj["shots"]):
        keys = s["keys"]
        dur = keys[-1]["t"] - keys[0]["t"] if keys else 0.0
        n = int(round(dur * fps))
        idx = [start + 2, start + n // 2, start + n - 2]
        idx = [min(max(k, 0), len(frames) - 1) for k in idx]
        rows.append((i + 1, s.get("name", ""), [os.path.join(a.dir, frames[k]) for k in idx], idx, dur))
        start += n
    print(f"{len(frames)} frames, {start} expected from the shots at {fps:g} fps")
    W = a.width
    f1, f2 = font(22), font(16)
    sheets = []
    for num, name, paths, idx, dur in rows:
        ims = [thumb(p, W, a.grid) for p in paths]
        h = ims[0].height
        note = notes.get(name, "")
        sheet = Image.new("RGB", (W * 3 + 40, h + 96), (24, 24, 26))
        d = ImageDraw.Draw(sheet)
        d.text((10, 8), f"{num}. {name}   ({dur:g} s)", font=f1, fill=(235, 235, 235))
        d.text((10, 38), note, font=f2, fill=(190, 190, 160))
        for k, im in enumerate(ims):
            x = 10 + k * (W + 10)
            sheet.paste(im, (x, 70))
            d.text((x + 4, 70 + h + 4), ("start", "middle", "end")[k] + f"  frame {idx[k]}", font=f2, fill=(160, 160, 160))
        p = os.path.join(out, f"shot_{num:02d}_{name}.png")
        sheet.save(p)
        sheets.append(p)
        print(p)
    # Overview: one row per shot, smaller.
    w2 = 320
    rowsIm = []
    for num, name, paths, idx, dur in rows:
        ims = [thumb(p, w2, a.grid) for p in paths]
        h = ims[0].height
        r = Image.new("RGB", (190 + 3 * (w2 + 6), h + 8), (24, 24, 26))
        d = ImageDraw.Draw(r)
        d.text((8, 6), f"{num}. {name}", font=f2, fill=(235, 235, 235))
        d.text((8, 28), f"{dur:g} s", font=f2, fill=(160, 160, 160))
        for k, im in enumerate(ims):
            r.paste(im, (190 + k * (w2 + 6), 4))
        rowsIm.append(r)
    ov = Image.new("RGB", (rowsIm[0].width, sum(r.height for r in rowsIm)), (24, 24, 26))
    y = 0
    for r in rowsIm:
        ov.paste(r, (0, y))
        y += r.height
    p = os.path.join(out, "overview.png")
    ov.save(p)
    print(p)


if __name__ == "__main__":
    main()
