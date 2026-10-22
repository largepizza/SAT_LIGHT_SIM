#!/usr/bin/env python3
"""Cloud droplet Mie phase table (2026-10-22, .plans/CLOUD_REBUILD_PLAN.md S5).

Writes assets/textures/cloud_mie.rgba16f: the single-scattering phase function of liquid cloud droplets, from the
Lorenz-Mie series (Bohren & Huffman's BHMIE), for a gamma size distribution (Hansen 1971: effective radius r_eff,
effective variance 0.1) at each of NR effective radii, integrated over the visible spectrum (400-700 nm) against
sunlight (a 5778 K blackbody) and the CIE 1931 colour matching functions, in linear sRGB. Everything a cloud's
single scattering shows comes out of it: the forward diffraction peak (the corona round the Sun / Moon through
thin cloud, and the bright silver lining), the cloudbow ~138 deg from the light (~40 deg from the antisolar point,
broad and nearly white), the supernumeraries inside it, and the glory round the antisolar point.

Layout (raw, little-endian):
    header  4 bytes 'SLMI', uint32 NA (angles), uint32 NR (radii), float32 r_eff[NR] (um), float32 g[NR][3]
    data    2 NR rows x NA texels of RGBA float16. Rows 0..NR-1: rgb = 4 pi p(theta) / 8 (mean 1/8 over the sphere: cv2HG's
            convention, scaled under half-float max), a = the scattering efficiency's spectral mean Qsca (~2)
            Rows NR..2NR-1: the STRUCTURE, rgb = p / exp(log p smoothed over a Gaussian of SMOOTH_DEG), clamped to
            [0.25, 8]: ~1 away from the optics, the corona's rings, the cloudbow and the glory where they are. The
            march multiplies its own tuned phase by it ("Mie optics"): the phenomena without the physical phase's
            ~20x weaker side scattering, which the multiple-scattering term was not tuned for.
    texel i is at theta = 180 deg x (i + 0.5) / NA.

    python tools/make_mie_lut.py            # writes the table and tools/out/mie_preview.png
    python tools/make_mie_lut.py --check    # prints g, bow and glory positions, no file
Standard library + numpy (+ Pillow for the preview).
"""
import argparse, math, os, struct, sys
from concurrent.futures import ProcessPoolExecutor
import numpy as np

NA = 2048                     # angles over 0..180 deg (0.088 deg: the corona's first ring at 20 um is ~1.6 deg)
REFF = np.array([4.0, 5.0, 6.0, 7.0, 8.0, 10.0, 12.0, 14.0, 17.0, 20.0, 25.0, 30.0])   # um
NR = len(REFF)
VEFF = 0.1
SMOOTH_DEG = 8.0
LAMBDA = np.arange(400.0, 701.0, 10.0)   # nm
RADII = np.exp(np.linspace(np.log(0.6), np.log(80.0), 160))   # um: the distributions' common radius grid

def water_index(lam_nm):
    # Cauchy fit through n(400) = 1.3390, n(700) = 1.3310 (1.3334 at 550 nm); absorption is negligible here.
    return 1.3271 + 1900.0 / (lam_nm * lam_nm)

def bhmie(x, m, mu):
    """Bohren & Huffman: S1, S2 at cos(theta) = mu, and Qsca, for size parameter x and relative index m."""
    nstop = int(x + 4.05 * x ** (1.0 / 3.0) + 2.0)
    y = m * x
    nmx = int(max(nstop, abs(y)) + 15)
    D = np.zeros(nmx + 1, dtype=complex)
    for n in range(nmx, 0, -1):                       # logarithmic derivative, downward
        D[n - 1] = n / y - 1.0 / (D[n] + n / y)
    psi0, psi1 = math.cos(x), math.sin(x)
    chi0, chi1 = -math.sin(x), math.cos(x)
    xi1 = complex(psi1, -chi1)
    pi0 = np.zeros_like(mu); pi1 = np.ones_like(mu)
    S1 = np.zeros_like(mu, dtype=complex); S2 = np.zeros_like(mu, dtype=complex)
    qsca = 0.0
    for n in range(1, nstop + 1):
        fn = (2.0 * n + 1.0) / (n * (n + 1.0))
        psi = (2.0 * n - 1.0) * psi1 / x - psi0
        chi = (2.0 * n - 1.0) * chi1 / x - chi0
        xi = complex(psi, -chi)
        an = ((D[n] / m + n / x) * psi - psi1) / ((D[n] / m + n / x) * xi - xi1)
        bn = ((D[n] * m + n / x) * psi - psi1) / ((D[n] * m + n / x) * xi - xi1)
        qsca += (2.0 * n + 1.0) * (abs(an) ** 2 + abs(bn) ** 2)
        tau = n * mu * pi1 - (n + 1.0) * pi0
        S1 += fn * (an * pi1 + bn * tau)
        S2 += fn * (an * tau + bn * pi1)
        psi0, psi1 = psi1, psi
        chi0, chi1 = chi1, chi
        xi1 = complex(psi1, -chi1)
        pi_n = ((2.0 * n + 1.0) * mu * pi1 - (n + 1.0) * pi0) / n
        pi0, pi1 = pi1, pi_n
    qsca *= 2.0 / (x * x)
    return S1, S2, qsca

def one(args):
    """For one wavelength: per radius, sigma_sca(r) x the (unnormalised) phase, and sigma_sca(r)."""
    lam, mu = args
    m = water_index(lam)
    k = 2.0 * math.pi / (lam * 1e-3)                  # 1/um
    rows = np.zeros((len(RADII), len(mu)))
    csca = np.zeros(len(RADII))
    for j, r in enumerate(RADII):
        S1, S2, q = bhmie(k * r, m, mu)
        i11 = 0.5 * (np.abs(S1) ** 2 + np.abs(S2) ** 2)          # unpolarised
        csca[j] = q * math.pi * r * r
        rows[j] = i11 / (k * k)                                  # = dC_sca / dOmega (um^2 / sr)
    return lam, rows, csca

def gamma_weights(reff):
    a, b = reff, VEFF
    w = RADII ** ((1.0 - 3.0 * b) / b) * np.exp(-RADII / (a * b))
    w *= np.gradient(RADII)                                       # dr on the log grid
    return w / w.sum()

def cmf(lam):
    # CIE 1931 2-degree, the multi-lobe Gaussian fit of Wyman, Sloan & Shirley (JCGT 2013).
    def g(x, mu, s1, s2):
        s = np.where(x < mu, s1, s2)
        return np.exp(-0.5 * ((x - mu) / s) ** 2)
    X = 1.056 * g(lam, 599.8, 37.9, 31.0) + 0.362 * g(lam, 442.0, 16.0, 26.7) - 0.065 * g(lam, 501.1, 20.4, 26.2)
    Y = 0.821 * g(lam, 568.8, 46.9, 40.5) + 0.286 * g(lam, 530.9, 16.3, 31.1)
    Z = 1.217 * g(lam, 437.0, 11.8, 36.0) + 0.681 * g(lam, 459.0, 26.0, 13.8)
    return np.stack([X, Y, Z], 1)

def sun(lam):
    l = lam * 1e-9
    return 1.0 / (l ** 5 * (np.exp(6.62607e-34 * 2.99792e8 / (l * 1.380649e-23 * 5778.0)) - 1.0))

def build():
    theta = (np.arange(NA) + 0.5) / NA * math.pi
    mu = np.cos(theta)
    with ProcessPoolExecutor(max_workers=max(1, (os.cpu_count() or 4) - 2)) as ex:
        res = sorted(ex.map(one, [(l, mu) for l in LAMBDA]), key=lambda t: t[0])
    dcs = np.stack([r[1] for r in res])          # [L, R, A]
    cs = np.stack([r[2] for r in res])           # [L, R]
    xyz = cmf(LAMBDA) * sun(LAMBDA)[:, None]     # [L, 3]
    M = np.array([[3.2406, -1.5372, -0.4986], [-0.9689, 1.8758, 0.0415], [0.0557, -0.2040, 1.0570]])
    w_rgb = xyz @ M.T                            # [L, 3]: each wavelength's weight in linear sRGB
    w_rgb /= w_rgb.sum(0, keepdims=True)         # a flat spectrum -> (1, 1, 1)
    out = np.zeros((NR, NA, 4), dtype=np.float32)
    gs = np.zeros((NR, 3))
    dOm = 2.0 * math.pi * np.sin(theta) * (math.pi / NA)
    for i, reff in enumerate(REFF):
        w = gamma_weights(reff)
        num = np.einsum('lra,r->la', dcs, w)          # [L, A]  (number-weighted dC/dOmega)
        den = cs @ w                                  # [L]     (number-weighted C_sca)
        p = 4.0 * math.pi * num / den[:, None]        # 4 pi p, per wavelength (mean 1 over the sphere)
        rgb = np.einsum('la,lc->ac', p, w_rgb)
        out[i, :, :3] = np.maximum(rgb, 0.0)
        # Qsca, spectrally averaged (the area-weighted efficiency: ~2 for cloud droplets)
        area = (math.pi * RADII ** 2) @ w
        out[i, :, 3] = float(np.mean(den / area))
        for c in range(3):
            pc = out[i, :, c]
            gs[i, c] = np.sum(pc * mu * dOm) / np.sum(pc * dOm)
    return theta, out, gs

def report(theta, out, gs):
    deg = np.degrees(theta)
    for i, reff in enumerate(REFF):
        p = out[i, :, 1]
        sel = (deg > 125) & (deg < 150)
        bow = deg[sel][np.argmax(p[sel])]
        print(f"r_eff {reff:5.1f} um  g(rgb) {gs[i,0]:.3f} {gs[i,1]:.3f} {gs[i,2]:.3f}  Qsca {out[i,0,3]:.3f}  "
              f"4pi p(0) {p[0]:9.1f}  p(90) {p[NA//2]:.4f}  cloudbow at {bow:.1f} deg ({180-bow:.1f} from antisolar)  "
              f"glory 4pi p(180) {p[-1]:.3f}")

def preview(theta, out, path):
    try:
        from PIL import Image, ImageDraw
    except ImportError:
        return
    W, H = 1440, 900
    im = Image.new('RGB', (W, H), (16, 16, 20))
    d = ImageDraw.Draw(im)
    deg = np.degrees(theta)
    def y(v):   # log scale 1e-2 .. 1e5
        return H - 20 - (math.log10(max(v, 1e-2)) + 2.0) / 7.0 * (H - 40)
    for dec in range(-2, 6):
        d.line([(0, y(10.0 ** dec)), (W, y(10.0 ** dec))], fill=(40, 40, 48))
    cols = [(255, 120, 120), (120, 255, 120), (120, 160, 255)]
    for i in (0, 5, 9):   # 4, 10, 20 um
        for c in range(3):
            pts = [(deg[k] / 180.0 * W, y(out[i, k, c])) for k in range(0, NA, 2)]
            d.line(pts, fill=tuple(int(v * (0.4 + 0.3 * (i / 9))) for v in cols[c]), width=1)
    im.save(path)
    print("preview", path)

def structure(out):
    sig = SMOOTH_DEG / 180.0 * NA
    k = np.arange(-int(4 * sig), int(4 * sig) + 1)
    w = np.exp(-0.5 * (k / sig) ** 2); w /= w.sum()
    st = np.zeros_like(out)
    for i in range(NR):
        for c in range(3):
            lp = np.log(np.maximum(out[i, :, c], 1e-6))
            pad = np.concatenate([lp[len(k) // 2:0:-1], lp, lp[-2:-len(k) // 2 - 2:-1]])   # reflect at 0 and 180
            sm = np.convolve(pad, w, mode='valid')
            st[i, :, c] = np.clip(np.exp(lp - sm[:NA]), 0.25, 8.0)
        st[i, :, 3] = 1.0
    return st

def write(path, theta, out, gs):
    with open(path, 'wb') as f:
        f.write(b'SLMI')
        f.write(struct.pack('<II', NA, NR))
        f.write(struct.pack(f'<{NR}f', *REFF))
        f.write(struct.pack(f'<{NR * 3}f', *gs.flatten()))
        o = out.copy()
        o[..., :3] *= 0.125   # the 30-um forward peak passes half-float max
        f.write(o.astype(np.float16).tobytes())
        f.write(structure(out).astype(np.float16).tobytes())
    print("wrote", path, os.path.getsize(path), "bytes")

if __name__ == '__main__':
    ap = argparse.ArgumentParser()
    ap.add_argument('--check', action='store_true')
    ap.add_argument('--out', default='assets/textures/cloud_mie.rgba16f')
    a = ap.parse_args()
    theta, out, gs = build()
    report(theta, out, gs)
    st = structure(out)
    deg = np.degrees(theta)
    i10 = list(REFF).index(10.0)
    for a0 in (1.0, 3.0, 20.0, 90.0, 138.0, 142.0, 170.0, 178.0, 179.9):
        j = int(a0 / 180.0 * NA)
        print(f"  structure at {a0:6.1f} deg (10 um, rgb): {st[i10, j, 0]:.2f} {st[i10, j, 1]:.2f} {st[i10, j, 2]:.2f}")
    if not a.check:
        write(a.out, theta, out, gs)
        os.makedirs('tools/out', exist_ok=True)
        preview(theta, out, 'tools/out/mie_preview.png')
