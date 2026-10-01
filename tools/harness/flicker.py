#!/usr/bin/env python3
"""Still-view flicker: per-pixel temporal statistics over a recorded frame sequence (review 18).

    python tools/harness/flicker.py <run>/captures/<name>_ [--skip 10] [--heat out.png] [--crop x,y,w,h]

For frames <prefix>NNNNN.png (camera still, time running), after skipping the first frames:
  std      mean over pixels of the per-pixel temporal standard deviation of luminance (0-255)
  d1       mean |frame - previous frame|
  p99      99th percentile of the per-pixel std
  >8       share of pixels whose std exceeds 8 levels (visible flicker)
Only pixels brighter than 6 on average count (space is excluded). --heat writes the std map x8.
"""
import argparse, glob, sys
import numpy as np
from PIL import Image

ap = argparse.ArgumentParser()
ap.add_argument("prefix", nargs="+")
ap.add_argument("--skip", type=int, default=10)
ap.add_argument("--heat")
ap.add_argument("--crop")
a = ap.parse_args()
for pre in a.prefix:
    fs = sorted(glob.glob(pre + "*.png"))[a.skip:]
    if len(fs) < 4:
        print(pre, "too few frames"); continue
    st = []
    for f in fs:
        im = Image.open(f).convert("RGB")
        if a.crop:
            x, y, w, h = map(int, a.crop.split(",")); im = im.crop((x, y, x + w, y + h))
        v = np.asarray(im).astype(np.float32)
        st.append(0.2126 * v[..., 0] + 0.7152 * v[..., 1] + 0.0722 * v[..., 2])
    s = np.stack(st)
    m = s.mean(0) > 6
    sd = s.std(0)
    d1 = np.abs(np.diff(s, axis=0)).mean(0)
    print(f"{pre:50s} n={len(fs)} std={sd[m].mean():.2f} d1={d1[m].mean():.2f} p99={np.percentile(sd[m],99):.1f} "
          f">8={100*(sd[m]>8).mean():.2f}%")
    if a.heat:
        Image.fromarray(np.clip(sd * 8, 0, 255).astype(np.uint8)).save(a.heat if len(a.prefix) == 1 else pre.rstrip('_/') + "_heat.png")
