#!/usr/bin/env python3
"""Cloud consistency across altitude (review 19; docs/HARNESS.md, "Climb consistency").

The question it answers: as the eye climbs straight up over one place, are the clouds the SAME clouds, only
seen with bigger pixels? Every level of detail (cell field, far field, far cloud layer) must be a filtered
version of the one below it, never a different placement. One launch climbs from a user's snapshot looking
straight down; `score` reprojects each frame onto the next altitude's frame (exact nadir geometry on a sphere
at the clouds' height), filters it to that frame's footprint, and compares.

    python tools/harness/climb.py gen <profile_log.jsonl> [--index -1] [--fov 40] -o harness_runs/climb/c.satcmd
        [--variant "tag:key value;key value"]...
    python tools/harness/run.py harness_runs/climb/c.satcmd --window 1600x900 --out harness_runs/climb/runX
    python tools/harness/climb.py score harness_runs/climb/runX [--sheet]
    python tools/harness/climb.py compare runA runB

Scores per altitude pair (lo -> hi): r = correlation of the cloud signal (the red channel: cloud is white, the
sea blue) over the frame area both see; frac_lo / frac_hi = the cloud fraction (signal past a threshold set from
the pair); iou = the cloud masks' intersection over union. A consistent renderer keeps r and iou high and the
fractions equal at every step; a substitution between levels shows as a dip at the altitude where it happens.
"""
import argparse
import glob
import json
import math
import os
import sys

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageFilter
    from scipy.ndimage import map_coordinates, gaussian_filter
except ImportError:
    sys.exit("climb needs Pillow + numpy + scipy")

R_EARTH = 6371000.0
H_REF = 1500.0            # the sphere the frames are matched on (the low clouds' middle)
EL = -89.9                # the harness camera's el (-90 is the pole of the look angles)
ALTS = [8000, 12000, 18000, 27000, 40000, 60000, 90000, 135000, 200000, 300000, 450000, 680000,
        1000000, 1500000, 2500000]


def gen(a):
    out = []
    variants = a.variant or ["base:"]
    for v in variants:
        tag, _, sets = v.partition(":")
        out.append(f"# ===== variant {tag}")
        out.append(f"snapshot {os.path.abspath(a.log).replace(os.sep, '/')} index={a.index}")
        out.append("set clouds_v2.auto_exposure 0")
        out.append("time pause")
        for kv in [s.strip() for s in sets.split(";") if s.strip()]:
            out.append(f"set {kv}")
        for h in ALTS:
            out.append(f"observer alt={h}")
            out.append(f"camera el={EL} fov={a.fov}")
            out.append("wait settle 40")
            out.append("wait 90")
            out.append(f"capture {tag}_climb_{h:08d}")
    os.makedirs(os.path.dirname(os.path.abspath(a.o)), exist_ok=True)
    with open(a.o, "w") as f:
        f.write("\n".join(out) + "\n")
    print(f"wrote {a.o}: {len(variants)} variant(s) x {len(ALTS)} altitudes")


def cam(h, fovDeg, W, H):
    """Eye, forward/right/up and tan(half fov) of the harness nadir camera, in a frame with z = up at the eye."""
    e = math.radians(EL)
    f = np.array([0.0, math.cos(e), math.sin(e)])
    r = np.array([1.0, 0.0, 0.0])
    u = np.cross(r, f)
    return np.array([0.0, 0.0, R_EARTH + h]), f, r, u, math.tan(math.radians(fovDeg) * 0.5), W / H


def ground_points(h, fov, W, H, xs, ys):
    eye, f, r, u, t, asp = cam(h, fov, W, H)
    nx = (xs + 0.5) / W * 2 - 1
    ny = 1 - (ys + 0.5) / H * 2
    d = f[None, :] + (nx * t * asp)[:, None] * r[None, :] + (ny * t)[:, None] * u[None, :]
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    rr = R_EARTH + H_REF
    b = d @ eye
    c = eye @ eye - rr * rr
    disc = b * b - c
    tt = np.where(disc > 0, -b - np.sqrt(np.maximum(disc, 0)), np.nan)
    return eye[None, :] + tt[:, None] * d


def project(h, fov, W, H, P):
    eye, f, r, u, t, asp = cam(h, fov, W, H)
    v = P - eye[None, :]
    zc = v @ f
    xc = v @ r
    yc = v @ u
    nx = xc / zc / (t * asp)
    ny = yc / zc / t
    return (nx + 1) * 0.5 * W - 0.5, (1 - ny) * 0.5 * H - 0.5


def signal(path):
    im = np.asarray(Image.open(path).convert("RGB")).astype(np.float32) / 255.0
    return im[..., 0]


def frames(run):
    caps = sorted(glob.glob(os.path.join(run, "captures", "*climb_*.png")))
    by = {}
    for c in caps:
        b = os.path.basename(c)[:-4]
        tag, _, h = b.rpartition("climb_")
        tag = tag.rstrip("_") or "run"
        side = c[:-4] + ".json"
        fov = 40.0
        if os.path.exists(side):
            try:
                s = json.load(open(side))
                fov = float(s.get("state", {}).get("camera", {}).get("fov_y_deg", fov))
            except Exception:
                pass
        by.setdefault(tag, []).append((int(h), c, fov))
    for t in by:
        by[t].sort()
    return by


def _place(v, ok, Hh, Wh):
    g = np.zeros((Hh, Wh), np.float32)
    g.ravel()[ok] = v
    return g


def score_pair(lo, hi):
    (hl, pl, fl), (hh, ph, fh) = lo, hi
    sl, sh = signal(pl), signal(ph)
    Hh, Wh = sh.shape
    Hl, Wl = sl.shape
    # Every hi pixel's point on the cloud sphere, then where the lo frame saw it.
    ys, xs = np.mgrid[0:Hh, 0:Wh]
    xs, ys = xs.ravel().astype(np.float64), ys.ravel().astype(np.float64)
    P = ground_points(hh, fh, Wh, Hh, xs, ys)
    lx, ly = project(hl, fl, Wl, Hl, P)
    ok = np.isfinite(lx) & (lx >= 1) & (lx < Wl - 2) & (ly >= 1) & (ly < Hl - 2)
    if ok.sum() < 500:
        return None
    # The lo frame filtered to the hi frame's footprint (nadir footprint ratio on the cloud sphere).
    k = max((hh - H_REF) / (hl - H_REF) * (fh / fl), 1.0)
    # Large ratios: box-reduce first (a 300x Gaussian on the full frame is slow), then the remainder.
    q = max(int(k // 2), 1)
    if q > 1:
        Hq, Wq = Hl // q, Wl // q
        slq = sl[:Hq * q, :Wq * q].reshape(Hq, q, Wq, q).mean(axis=(1, 3))
        slf = gaussian_filter(slq, sigma=0.5 * k / q)
        a = map_coordinates(slf, [(ly[ok] + 0.5) / q - 0.5, (lx[ok] + 0.5) / q - 0.5], order=1, mode="nearest")
    else:
        slf = gaussian_filter(sl, sigma=0.5 * k)
        a = map_coordinates(slf, [ly[ok], lx[ok]], order=1)
    b = sh.ravel()[ok]
    # Cloud DETAIL only: each minus its own background (a normalised Gaussian over the shared area, ~1/20 of
    # the frame), so the sun glint, the haze and the limb do not count as agreement.
    okm = ok.reshape(Hh, Wh).astype(np.float32)
    sg = Wh / 20.0
    den = gaussian_filter(okm, sg) + 1e-6
    def hp(v):
        g = np.zeros((Hh, Wh), np.float32)
        g.ravel()[ok] = v
        return v - (gaussian_filter(g, sg) / den).ravel()[ok]
    a, b = hp(a), hp(b)
    r = float(np.corrcoef(a, b)[0, 1]) if a.std() > 1e-6 and b.std() > 1e-6 else float("nan")
    thr = 0.06
    ma, mb = a > thr, b > thr
    inter, uni = float((ma & mb).sum()), float((ma | mb).sum())
    img = np.full((Hh, Wh), np.nan, np.float32)
    img.ravel()[ok] = a + 0.5
    return dict(lo_km=hl / 1000, hi_km=hh / 1000, r=r, frac_lo=float(ma.mean()), frac_hi=float(mb.mean()),
                iou=inter / uni if uni > 0 else 1.0, mean_abs=float(np.abs(a - b).mean() * 255),
                _img=img, _hi=np.where(ok.reshape(Hh, Wh), 0, 0).astype(np.float32) + _place(b, ok, Hh, Wh) + 0.5, _ok=ok.reshape(Hh, Wh))


def score(a):
    res = {}
    for tag, fs in frames(a.run).items():
        rows, sheet = [], []
        for lo, hi in zip(fs[:-1], fs[1:]):
            s = score_pair(lo, hi)
            if s is None:
                continue
            sheet.append(s)
            rows.append({k: v for k, v in s.items() if not k.startswith("_")})
        res[tag] = rows
        # Against FIXED references: drift that each step hides (a little per step) adds up here.
        refs = {}
        for rk in (a.ref or [8, 40, 200]):
            lo = min(fs, key=lambda x: abs(x[0] / 1000 - rk))
            row = []
            for hi in fs:
                if hi[0] <= lo[0]:
                    continue
                sc = score_pair(lo, hi)
                if sc is not None:
                    row.append((hi[0] / 1000, sc["r"], sc["frac_lo"], sc["frac_hi"]))
            refs[lo[0] / 1000] = row
            print(f"   vs {lo[0]/1000:.0f} km: " + "  ".join(f"{h:.0f}:{r:.2f}" for h, r, _, _ in row))
        res[tag + "#refs"] = {str(k): v for k, v in refs.items()}
        print(f"== {tag}")
        print(f"{'lo km':>8} {'hi km':>8} {'r':>6} {'iou':>6} {'frac lo':>8} {'frac hi':>8} {'|d|':>6}")
        for r in rows:
            print(f"{r['lo_km']:8.0f} {r['hi_km']:8.0f} {r['r']:6.3f} {r['iou']:6.3f} {r['frac_lo']:8.3f} {r['frac_hi']:8.3f} {r['mean_abs']:6.1f}")
        if rows:
            print(f"   mean r {np.mean([r['r'] for r in rows]):.3f}  min r {min(r['r'] for r in rows):.3f}  "
                  f"mean iou {np.mean([r['iou'] for r in rows]):.3f}  min iou {min(r['iou'] for r in rows):.3f}")
        if a.sheet and sheet:
            tw, th = 400, 225
            out = Image.new("RGB", (tw * 3, th * len(sheet)))
            for i, s in enumerate(sheet):
                lo = np.nan_to_num(s["_img"], nan=0.0)
                hi = s["_hi"] * s["_ok"]
                d = np.clip(np.abs(lo - hi) * 4, 0, 1) * s["_ok"]
                for j, m in enumerate([lo, hi, d]):
                    im = Image.fromarray((np.clip(m, 0, 1) * 255).astype(np.uint8)).resize((tw, th))
                    ImageDraw.Draw(im).text((4, 4), f"{s['lo_km']:.0f}->{s['hi_km']:.0f} km r={s['r']:.2f}"
                                            if j == 0 else ["", "hi", "|diff| x4"][j], fill=255)
                    out.paste(im.convert("RGB"), (j * tw, i * th))
            p = os.path.join(a.run, f"climb_{tag}.png")
            out.save(p)
            print(f"   sheet: {p}")
    with open(os.path.join(a.run, "climb_score.json"), "w") as f:
        json.dump(res, f, indent=1)


def compare(a):
    A = json.load(open(os.path.join(a.runA, "climb_score.json")))
    B = json.load(open(os.path.join(a.runB, "climb_score.json")))
    for tag in A:
        if tag not in B or tag.endswith("#refs"):
            continue
        print(f"== {tag}: r / iou   A -> B")
        for ra, rb in zip(A[tag], B[tag]):
            print(f"{ra['lo_km']:8.0f} -> {ra['hi_km']:<8.0f} r {ra['r']:.3f} -> {rb['r']:.3f}   iou {ra['iou']:.3f} -> {rb['iou']:.3f}"
                  f"   frac {ra['frac_lo']:.2f}/{ra['frac_hi']:.2f} -> {rb['frac_lo']:.2f}/{rb['frac_hi']:.2f}")


def main():
    p = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = p.add_subparsers(dest="cmd", required=True)
    g = sp.add_parser("gen")
    g.add_argument("log")
    g.add_argument("--index", type=int, default=-1)
    g.add_argument("--fov", type=float, default=40.0)
    g.add_argument("--variant", action="append")
    g.add_argument("-o", required=True)
    s = sp.add_parser("score")
    s.add_argument("run")
    s.add_argument("--sheet", action="store_true")
    s.add_argument("--ref", type=float, action="append", help="reference altitudes (km) for the fixed comparisons")
    c = sp.add_parser("compare")
    c.add_argument("runA")
    c.add_argument("runB")
    a = p.parse_args()
    {"gen": gen, "score": score, "compare": compare}[a.cmd](a)


if __name__ == "__main__":
    main()
