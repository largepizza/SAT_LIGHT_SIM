"""Bake the Milky Way panorama's denoised glow (sky binding 13's alpha, sat_sky.frag "Milky Way skybox").

Source: assets/textures/8k_stars_milky_way.jpg ("The Milky Way panorama", eso0932a, ESO/S. Brunier, starless), a
4096 x 2048 8-bit sRGB JPEG in galactic longitude / latitude (u = 0.5 + l / 360, v = 0.5 + b / 180, row 0 = b -90).

Output: assets/textures/milky_way_glow.r16f — one channel, which the app puts in the alpha of an RGBA16F texture whose
RGB is the JPEG itself decoded to linear at load (what the old R8G8B8A8_SRGB sampler returned, so the default view is
unchanged; SatelliteSim::createMilkyWayBaked):
  A    a DENOISED linear luminance: a 3 x 3 median of log luminance where the source is noise-dominated, then a
       1.3-texel Gaussian whose share follows the texel's own 8-bit quantisation step. In the faint sky an sRGB step
       of 2 -> 3 is +50% of the light, and the JPEG's 8 x 8 blocks sit on top of it: magnified at a narrow field of
       view, or lifted by the space gain, that was the "grain" and blocking. The bright band keeps its dust detail.
       A bilateral filter was tried first and posterised (flat patches with hard steps). Totals kept per 64 x 64.
  The shader reads A as the smooth glow when zoomed in and derives sub-texel dust structure from it (sat_sky.frag
  mwZoomDetail); RGB stays the colour.

File: char[4] "SLMA" | uint32 width | uint32 height | uint32 mips | then the levels, each height x width float16
(little-endian), level 0 first; mips are 2 x 2 box means (the app box-filters the JPEG's linear RGB the same way).

    python tools/make_milky_way.py [--preview out.png]
"""
import argparse
import os
import struct

import numpy as np
from PIL import Image

REPO = os.path.abspath(os.path.join(os.path.dirname(__file__), ".."))
SRC = os.path.join(REPO, "assets", "textures", "8k_stars_milky_way.jpg")
OUT = os.path.join(REPO, "assets", "textures", "milky_way_glow.r16f")


def srgb_to_lin(c):
    return np.where(c <= 0.04045, c / 12.92, ((c + 0.055) / 1.055) ** 2.4)


def box_down(a):
    return 0.25 * (a[0::2, 0::2] + a[1::2, 0::2] + a[0::2, 1::2] + a[1::2, 1::2])


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--preview", default=None)
    args = ap.parse_args()
    im = Image.open(SRC).convert("RGB")
    s8 = np.asarray(im).astype(np.float32)
    H, W = s8.shape[:2]
    lin = srgb_to_lin(s8 / 255.0).astype(np.float32)
    lum = lin @ np.array([0.2126, 0.7152, 0.0722], np.float32)
    # Relative step of the source's 8-bit encoding at each texel: (lin(v + 1) - lin(v)) / lin(v), on the green
    # (luminance-dominant) value, floored at v = 1.
    g = np.maximum(s8[..., 1], 1.0)
    rel = (srgb_to_lin((g + 1.0) / 255.0) - srgb_to_lin(g / 255.0)) / np.maximum(srgb_to_lin(g / 255.0), 1e-6)
    print("source", W, "x", H, "lum mean %.5f max %.4f" % (lum.mean(), lum.max()))
    # Noise removal without the bilateral filter's posterising (tried first: flat cartoon patches with hard steps).
    # A 3 x 3 median of log luminance takes out isolated 8-bit/JPEG speckles; then a Gaussian (1.3 texels) whose
    # share follows the texel's own quantisation noise: the faint, noise-dominated sky is smoothed fully, the bright
    # band (rel step ~1-3%) keeps the median's detail. The dust lanes there are several texels wide and survive.
    from scipy.ndimage import median_filter, gaussian_filter
    Lg = np.log(lum + 1e-5)
    m = median_filter(Lg, size=3, mode="wrap")  # the wrap at the poles is a row of the same latitude: harmless
    # ...but only where the source is noisy: on the bright band (8-bit steps of 1-3%) a dark speck a texel or two
    # wide is a real dust globule, kept.
    m = Lg + (m - Lg) * np.clip((rel - 0.02) / 0.08, 0.0, 1.0)
    gsm = gaussian_filter(m, sigma=1.3, mode=["nearest", "wrap"])
    k = np.clip((rel - 0.04) / 0.25, 0.0, 1.0) ** 0.5
    k = np.maximum(k, 0.35)   # a little smoothing everywhere: the 8 x 8 JPEG blocks show on the band too
    den = np.exp(m + (gsm - m) * k) - 1e-5
    den = np.maximum(den, 0.0).astype(np.float32)
    # Conserve the light: the filter is a weighted mean of neighbours, so its total is the source's to ~1e-3; keep it
    # exact over 64 x 64 blocks anyway (the band's mean is what the default view shows at a wide field of view).
    b = 64
    sb = lum.reshape(H // b, b, W // b, b).mean(axis=(1, 3))
    db = den.reshape(H // b, b, W // b, b).mean(axis=(1, 3))
    corr = np.repeat(np.repeat(sb / np.maximum(db, 1e-7), b, 0), b, 1)
    # Smooth the block correction (bilinear-ish) so it cannot itself make 64-px steps.
    corr = np.exp(gaussian_filter(np.log(np.clip(corr, 0.5, 2.0)), sigma=24, mode=["nearest", "wrap"]))
    den = den * corr
    print("denoised: total / source %.5f" % (den.mean() / lum.mean()))
    levels = [den.astype(np.float32)]
    while min(levels[-1].shape[:2]) > 1:
        levels.append(box_down(levels[-1]))
    with open(OUT, "wb") as f:
        f.write(b"SLMA" + struct.pack("<III", W, H, len(levels)))
        for l in levels:
            f.write(l.astype("<f2").tobytes())
    print("wrote", OUT, os.path.getsize(OUT) // (1 << 20), "MB,", len(levels), "levels")
    if args.preview:
        # Side by side: the source's luminance and the denoised A, a crop on the Galactic Centre, stretched.
        y0, x0 = H // 2 - 160, W // 2 - 240
        def st(a):
            return np.clip((a / 0.08) ** (1 / 2.2) * 255, 0, 255).astype(np.uint8)
        c = np.concatenate([st(lum[y0:y0 + 320, x0:x0 + 480]), st(den[y0:y0 + 320, x0:x0 + 480])], axis=1)
        Image.fromarray(c).resize((c.shape[1] * 2, c.shape[0] * 2), Image.NEAREST).save(args.preview)


if __name__ == "__main__":
    main()
