#!/usr/bin/env python3
"""Image helpers for harness captures (docs/HARNESS.md). Needs Pillow + numpy:

    py -3 -m venv tools/harness/.venv
    tools/harness/.venv/Scripts/python -m pip install -r tools/harness/requirements.txt

Subcommands (all print JSON to stdout, so an agent can read numbers before looking at pictures):

  sheet <png|dir>... -o sheet.png [--cols 3] [--width 480]
        Labelled contact sheet: many variants in one image (cheaper to look at than N images).
  diff a.png b.png [-o heat.png] [--gain 8] [--threshold 8]
        Per-pixel difference: mean/max abs, RMS, PSNR, fraction of pixels past the threshold,
        the bounding box of the changed region, and an amplified heatmap image.
  stats a.png [--grid 4]
        Luminance mean/percentiles, fraction clipped black/white, and a coarse grid of mean
        luminance (to spot a black or blown-out region without looking).
  crop a.png x y w h [--scale 4] -o out.png
        Cut a region out and enlarge it with nearest neighbour (pixel-exact).
  seams a.png [--axis both] [--top 10]
        Rows/columns whose mean luminance jumps against their neighbours: tile seams, bands,
        the horizontal lines of a bad terrain march.
  audio a.wav [b.wav ...] [-o spec.png]
        For `audio record` WAVs: RMS / peak / ungated loudness (LUFS, BS.1770 K-weighting), energy
        per band, stereo correlation, transients per second, the loudest spectral peaks, and a
        log-frequency spectrogram (+ RMS strip) per file — several files stack into one image.
        Chirps, beeps and clicks are visible as shapes; a hum as horizontal lines; wind as a band.
"""
import argparse
import glob
import json
import math
import os
import sys
import wave

try:
    import numpy as np
    from PIL import Image, ImageDraw, ImageFont
except ImportError:
    sys.exit("imgtools needs Pillow + numpy: tools/harness/.venv/Scripts/python -m pip install -r tools/harness/requirements.txt")


def load(p):
    return np.asarray(Image.open(p).convert("RGB")).astype(np.float32)


def lum(a):
    return 0.2126 * a[..., 0] + 0.7152 * a[..., 1] + 0.0722 * a[..., 2]


def font(size):
    for name in ("arial.ttf", "DejaVuSans.ttf", "Helvetica.ttc"):
        try:
            return ImageFont.truetype(name, size)
        except OSError:
            pass
    return ImageFont.load_default()


def expand(paths):
    out = []
    for p in paths:
        if os.path.isdir(p):
            cap = os.path.join(p, "captures")
            out += sorted(glob.glob(os.path.join(cap if os.path.isdir(cap) else p, "*.png")))
        else:
            out += sorted(glob.glob(p)) or [p]
    return out


def cmd_sheet(a):
    paths = expand(a.inputs)
    if not paths:
        sys.exit("no images")
    ims = [Image.open(p).convert("RGB") for p in paths]
    w = a.width
    thumbs = [im.resize((w, max(1, round(im.height * w / im.width))), Image.LANCZOS) for im in ims]
    cols = min(a.cols, len(thumbs))
    rows = math.ceil(len(thumbs) / cols)
    label_h = 22
    cell_h = max(t.height for t in thumbs) + label_h
    sheet = Image.new("RGB", (cols * w + (cols + 1) * 4, rows * cell_h + (rows + 1) * 4), (24, 24, 24))
    d = ImageDraw.Draw(sheet)
    f = font(14)
    for i, (t, p) in enumerate(zip(thumbs, paths)):
        x = 4 + (i % cols) * (w + 4)
        y = 4 + (i // cols) * (cell_h + 4)
        sheet.paste(t, (x, y + label_h))
        d.text((x + 3, y + 3), os.path.splitext(os.path.basename(p))[0], fill=(235, 235, 235), font=f)
    sheet.save(a.out)
    print(json.dumps({"sheet": os.path.abspath(a.out), "images": len(paths), "size": sheet.size}))


def cmd_diff(a):
    x, y = load(a.a), load(a.b)
    if x.shape != y.shape:
        sys.exit(f"size mismatch {x.shape} vs {y.shape}")
    d = np.abs(x - y)
    dl = d.max(axis=2)
    mse = float((d ** 2).mean())
    changed = dl > a.threshold
    res = {"mean_abs": float(d.mean()), "max_abs": float(d.max()), "rms": math.sqrt(mse),
           "psnr_db": float("inf") if mse == 0 else 10 * math.log10(255 ** 2 / mse),
           "changed_frac": float(changed.mean()), "threshold": a.threshold}
    if changed.any():
        ys, xs = np.nonzero(changed)
        res["changed_bbox"] = [int(xs.min()), int(ys.min()), int(xs.max() - xs.min() + 1), int(ys.max() - ys.min() + 1)]
    if a.out:
        heat = np.clip(dl * a.gain, 0, 255).astype(np.int32)
        # dark-red-yellow-white ramp
        rgb = np.stack([np.clip(heat * 3, 0, 255), np.clip(heat * 3 - 255, 0, 255), np.clip(heat * 3 - 510, 0, 255)], -1)
        Image.fromarray(rgb.astype(np.uint8)).save(a.out)
        res["heatmap"] = os.path.abspath(a.out)
    print(json.dumps(res, indent=1))


def cmd_stats(a):
    x = load(a.a)
    L = lum(x)
    g = a.grid
    h, w = L.shape
    grid = [[round(float(L[r * h // g:(r + 1) * h // g, c * w // g:(c + 1) * w // g].mean()), 1) for c in range(g)] for r in range(g)]
    res = {"size": [w, h], "lum_mean": float(L.mean()),
           "lum_p": {str(p): float(np.percentile(L, p)) for p in (1, 5, 50, 95, 99)},
           "black_frac": float((x.max(axis=2) <= 2).mean()), "white_frac": float((x.min(axis=2) >= 253).mean()),
           "rgb_mean": [float(v) for v in x.reshape(-1, 3).mean(axis=0)], "lum_grid": grid}
    print(json.dumps(res, indent=1))


def cmd_crop(a):
    im = Image.open(a.a).convert("RGB").crop((a.x, a.y, a.x + a.w, a.y + a.h))
    if a.scale != 1:
        im = im.resize((a.w * a.scale, a.h * a.scale), Image.NEAREST)
    im.save(a.out)
    print(json.dumps({"crop": os.path.abspath(a.out), "size": im.size}))


def cmd_seams(a):
    L = lum(load(a.a))
    out = {}
    for axis, name in ((1, "rows"), (0, "cols")):
        if a.axis not in ("both", name):
            continue
        prof = L.mean(axis=axis)  # mean per row (axis=1) or column (axis=0)
        # second difference: a one-line step or spike stands out against smooth gradients
        d2 = np.abs(prof[1:-1] - 0.5 * (prof[:-2] + prof[2:]))
        idx = np.argsort(d2)[::-1][:a.top]
        med = float(np.median(d2)) + 1e-6
        out[name] = [{"index": int(i + 1), "jump": round(float(d2[i]), 3), "x_median": round(float(d2[i] / med), 1)} for i in idx]
    print(json.dumps(out, indent=1))


# ── audio ────────────────────────────────────────────────────────────────────────────────────────
BANDS = [("sub", 20, 60), ("low", 60, 250), ("lowmid", 250, 1000), ("mid", 1000, 4000),
         ("high", 4000, 12000), ("air", 12000, 20000)]


def read_wav(path):
    with wave.open(path, "rb") as w:
        if w.getsampwidth() != 2:
            sys.exit(f"{path}: only 16-bit PCM WAV is supported")
        sr, ch, n = w.getframerate(), w.getnchannels(), w.getnframes()
        x = np.frombuffer(w.readframes(n), dtype="<i2").astype(np.float64) / 32768.0
    return sr, x.reshape(-1, ch)


def biquad(x, b, a):
    # Direct form I, one channel. A plain loop: fast enough for seconds of audio, no scipy needed.
    y = np.empty_like(x)
    x1 = x2 = y1 = y2 = 0.0
    b0, b1, b2 = b
    _, a1, a2 = a
    for i, v in enumerate(x.tolist()):
        o = b0 * v + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
        x2, x1, y2, y1 = x1, v, y1, o
        y[i] = o
    return y


def lufs(x, sr):
    if sr != 48000:
        return None  # the BS.1770 coefficients below are for 48 kHz
    b1, a1 = (1.53512485958697, -2.69169618940638, 1.19839281085285), (1.0, -1.69065929318241, 0.73248077421585)
    b2, a2 = (1.0, -2.0, 1.0), (1.0, -1.99004745483398, 0.99007225036621)
    ms = sum(float(np.mean(biquad(biquad(x[:, c], b1, a1), b2, a2) ** 2)) for c in range(x.shape[1]))
    return -0.691 + 10.0 * math.log10(ms) if ms > 0 else -120.0


def db(v):
    return 20.0 * math.log10(v) if v > 1e-12 else -120.0


def colormap(t):
    # black -> violet -> orange -> pale yellow, t in [0, 1]
    stops = np.array([[0, 0, 0], [40, 10, 90], [150, 30, 110], [240, 110, 40], [255, 240, 170]], np.float64)
    t = np.clip(t, 0, 1) * (len(stops) - 1)
    i = np.minimum(t.astype(int), len(stops) - 2)
    f = (t - i)[..., None]
    return (stops[i] * (1 - f) + stops[i + 1] * f).astype(np.uint8)


def spectrogram_image(mono, sr, width=1000, height=300, range_db=70.0):
    nfft, hop = 4096, 256
    if len(mono) < nfft:
        mono = np.pad(mono, (0, nfft - len(mono)))
    win = np.hanning(nfft)
    frames = 1 + (len(mono) - nfft) // hop
    idx = np.arange(nfft)[None, :] + hop * np.arange(frames)[:, None]
    spec = np.abs(np.fft.rfft(mono[idx] * win, axis=1)) * (2.0 / win.sum())
    freqs = np.fft.rfftfreq(nfft, 1.0 / sr)
    rows = 20.0 * (1000.0 ** (np.arange(height)[::-1] / (height - 1)))  # 20 Hz .. 20 kHz, log
    mag = np.stack([np.interp(rows, freqs, spec[f]) for f in range(frames)], axis=1)
    cols = np.linspace(0, frames - 1, width).astype(int)
    d = 20.0 * np.log10(np.maximum(mag[:, cols], 1e-12))
    hi_db = float(np.percentile(d, 99.5))  # auto range: the loudest cells are the top of the scale
    return colormap((d - (hi_db - range_db)) / range_db), rows


def cmd_audio(a):
    panels, reports = [], []
    for path in a.inputs:
        sr, x = read_wav(path)
        mono = x.mean(axis=1)
        n = len(mono)
        rms = math.sqrt(float(np.mean(x ** 2)))
        peak = float(np.max(np.abs(x))) if n else 0.0
        spec = np.abs(np.fft.rfft(mono * np.hanning(n))) ** 2
        freqs = np.fft.rfftfreq(n, 1.0 / sr)
        total = float(spec[(freqs >= 20) & (freqs <= 20000)].sum()) or 1e-30
        bands = {name: round(10 * math.log10(max(float(spec[(freqs >= lo) & (freqs < hi)].sum()) / total, 1e-12)), 1)
                 for name, lo, hi in BANDS}
        centroid = float((spec * freqs).sum() / max(spec.sum(), 1e-30))
        # The strongest narrow peaks (tones / resonances), 1/6-octave apart at least.
        sm = np.convolve(spec, np.ones(9) / 9, mode="same")
        cand = np.argsort(spec)[::-1]
        peaks = []
        for k in cand[:4000]:
            f = freqs[k]
            if f < 30 or f > 16000 or spec[k] < 4 * sm[k]:
                continue
            if all(abs(math.log2(f / p)) > 1 / 6 for p in peaks):
                peaks.append(float(f))
            if len(peaks) == 5:
                break
        corr = float(np.corrcoef(x[:, 0], x[:, 1])[0, 1]) if x.shape[1] == 2 and n > 1 else 1.0
        # Transients: 2 ms windows of the first difference far above the median.
        dif = np.abs(np.diff(mono))
        w = max(1, int(0.002 * sr))
        e = dif[: len(dif) // w * w].reshape(-1, w).mean(axis=1)
        trans = int(np.sum((e > 8 * np.median(e)) & (np.r_[0, np.diff((e > 8 * np.median(e)).astype(int))] == 1)))
        seg = max(1, int(0.05 * sr))
        strip = [db(math.sqrt(float(np.mean(mono[i:i + seg] ** 2)))) for i in range(0, n - seg + 1, seg)]
        rep = {"file": path, "seconds": round(n / sr, 2), "rms_dbfs": round(db(rms), 1), "peak_dbfs": round(db(peak), 1),
               "lufs_ungated": None, "bands_db_rel": bands, "centroid_hz": round(centroid), "tonal_peaks_hz": [round(p) for p in peaks],
               "stereo_corr": round(corr, 2), "transients_per_s": round(trans / max(n / sr, 1e-9), 2),
               "rms_dbfs_min_max_50ms": [round(min(strip), 1), round(max(strip), 1)] if strip else None}
        L = lufs(x, sr)
        rep["lufs_ungated"] = round(L, 1) if L is not None else None
        reports.append(rep)

        img, rows = spectrogram_image(mono, sr, width=a.width)
        H, W = img.shape[0], img.shape[1]
        stripH, pad = 50, 40
        canvas = Image.new("RGB", (W + pad, H + stripH + 36), (18, 18, 22))
        canvas.paste(Image.fromarray(img), (pad, 22))
        dr = ImageDraw.Draw(canvas)
        f = font(12)
        dr.text((pad, 3), f"{os.path.basename(path)}   RMS {rep['rms_dbfs']} dBFS   LUFS {rep['lufs_ungated']}   "
                          f"peak {rep['peak_dbfs']}   corr {rep['stereo_corr']}", fill=(230, 230, 230), font=f)
        for fr in (50, 100, 200, 500, 1000, 2000, 5000, 10000):
            y = 22 + int((H - 1) * (1 - math.log(fr / 20.0) / math.log(1000.0)))
            dr.line([(pad - 4, y), (pad, y)], fill=(200, 200, 200))
            dr.text((2, y - 7), f"{fr // 1000}k" if fr >= 1000 else str(fr), fill=(200, 200, 200), font=f)
        secs = n / sr
        for t in range(int(secs) + 1):
            x0 = pad + int(t / max(secs, 1e-9) * (W - 1))
            dr.line([(x0, 22 + H), (x0, 26 + H)], fill=(200, 200, 200))
        if strip:
            # Scaled to its own range (at least 12 dB), labelled, so a 6 dB gust is visible.
            y0 = 30 + H
            top = max(strip)
            bot = min(min(strip), top - 12.0)
            pts = [(pad + int(i / max(len(strip) - 1, 1) * (W - 1)),
                    y0 + stripH - 4 - int(np.clip((v - bot) / (top - bot), 0, 1) * (stripH - 8))) for i, v in enumerate(strip)]
            dr.line(pts, fill=(120, 200, 255), width=1)
            dr.text((2, y0), f"{top:.0f}", fill=(120, 200, 255), font=f)
            dr.text((2, y0 + stripH - 14), f"{bot:.0f}", fill=(120, 200, 255), font=f)
        panels.append(canvas)

    if a.out and panels:
        W = max(p.width for p in panels)
        out = Image.new("RGB", (W, sum(p.height for p in panels)), (18, 18, 22))
        y = 0
        for p in panels:
            out.paste(p, (0, y))
            y += p.height
        out.save(a.out)
    print(json.dumps({"files": reports, "image": a.out}, indent=1))


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    sp = ap.add_subparsers(dest="cmd", required=True)
    s = sp.add_parser("sheet"); s.add_argument("inputs", nargs="+"); s.add_argument("-o", "--out", required=True)
    s.add_argument("--cols", type=int, default=3); s.add_argument("--width", type=int, default=480)
    s = sp.add_parser("diff"); s.add_argument("a"); s.add_argument("b"); s.add_argument("-o", "--out")
    s.add_argument("--gain", type=float, default=8.0); s.add_argument("--threshold", type=float, default=8.0)
    s = sp.add_parser("stats"); s.add_argument("a"); s.add_argument("--grid", type=int, default=4)
    s = sp.add_parser("crop"); s.add_argument("a")
    for k in ("x", "y", "w", "h"):
        s.add_argument(k, type=int)
    s.add_argument("--scale", type=int, default=4); s.add_argument("-o", "--out", required=True)
    s = sp.add_parser("seams"); s.add_argument("a"); s.add_argument("--axis", default="both", choices=["both", "rows", "cols"])
    s.add_argument("--top", type=int, default=10)
    s = sp.add_parser("audio"); s.add_argument("inputs", nargs="+"); s.add_argument("-o", "--out")
    s.add_argument("--width", type=int, default=1000)
    a = ap.parse_args()
    {"sheet": cmd_sheet, "diff": cmd_diff, "stats": cmd_stats, "crop": cmd_crop, "seams": cmd_seams,
     "audio": cmd_audio}[a.cmd](a)


if __name__ == "__main__":
    main()
