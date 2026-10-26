"""Path-traced reference for the cloud march's MULTIPLE SCATTERING (cloud_v2_march.comp cv2DiffuseField).

Renders small cloud blobs (conservative scattering, Henyey-Greenstein g = 0.85) by backward path tracing with next-event
estimation toward the Sun, keeps the light scattered TWICE OR MORE (the single scattering is the march's own term, with its
own tuned phase), and evaluates the march's two multiple-scattering models on the same blobs with exact optical depths:

  old  the octaves (msOctaves) at the given extinction / strength ratios, + the "Silver lining" forward term
  new  the delta-Eddington diffuse field with the three finite-cloud corrections (cv2DiffuseField) + the same silver term

and prints, per blob and lighting: the correlation with the reference and the mean level against it. Units are the
march's: radiance x 4 pi / solar irradiance (phase functions with a mean of 1). numpy + scipy + Pillow only.

    python tools/cloud_ms_reference.py                 # the standard set (~6 min)
    python tools/cloud_ms_reference.py --quick         # fewer samples (~1.5 min)
    python tools/cloud_ms_reference.py --png out.png   # also a contact sheet: reference | old | new per row

Measured 2026-10-26 (--quick; the fit behind cv2DiffuseField's constants): correlation with the reference old 0.78-0.92,
new 0.89-0.96; mean level against it old 0.32-12.8x (thin cloud 4-13x too bright: no diffuse build-up; a front-lit cloud a
flat disc to its rim), new 0.41-1.54x (a thick cloud ~0.6x at gain 1: the path-traced level is ~1.6).
"""
import argparse
import numpy as np

rng = np.random.default_rng(1)
G = 0.85


def hg(cos, g):
    return (1 - g * g) / (1 + g * g - 2 * g * cos) ** 1.5      # mean 1 over the sphere


def hg_sample(d, g, n):
    u, v = rng.random(n), rng.random(n)
    s = (1 - g * g) / (1 - g + 2 * g * u)
    cost = np.clip((1 + g * g - s * s) / (2 * g), -1, 1)
    sint, phi = np.sqrt(1 - cost ** 2), 2 * np.pi * v
    a = np.where(np.abs(d[:, 2:3]) < 0.9, np.array([[0, 0, 1.]]), np.array([[1., 0, 0]]))
    t1 = np.cross(d, a); t1 /= np.linalg.norm(t1, axis=1, keepdims=True); t2 = np.cross(d, t1)
    return d * cost[:, None] + t1 * (sint * np.cos(phi))[:, None] + t2 * (sint * np.sin(phi))[:, None]


class Blob:
    """Extinction sigma0 x a profile in [0, 1] inside the unit sphere."""
    def __init__(self, sigma0, kind):
        self.s0, self.kind = sigma0, kind

    def dens(self, p):
        r = np.linalg.norm(p, axis=-1)
        if self.kind == 'hard':
            return np.where(r < 1, 1.0, 0.0)
        if self.kind == 'soft':     # density ramps over the outer half: a wispy edge
            return np.clip((1 - r) / 0.5, 0, 1) ** 1.5
        q = p * (3.0 if self.kind == 'lumpy' else 7.0)
        n = np.sin(q[..., 0] * 1.7 + 1) * np.sin(q[..., 1] * 2.1 + 2) * np.sin(q[..., 2] * 1.9 + .5)
        if self.kind == 'lumpy':
            return np.clip((0.8 - r) * 3 + 0.6 * n, 0, 1)
        n += 0.5 * np.sin(q[..., 0] * 3.3 + 2) * np.sin(q[..., 1] * 2.9 + .7) * np.sin(q[..., 2] * 3.7 + 1.3)
        return np.clip(((0.75 - r) * 6 + 0.9 * n) * 4, 0, 1)       # 'cauli': cauliflower-scale lobes


def chord_exit(p, d):
    b = np.sum(p * d, -1); c = np.sum(p * p, -1) - 1
    return -b + np.sqrt(np.maximum(b * b - c, 0))


def optdepth(blob, p, d, n=64):
    L = chord_exit(p, d); ts = (np.arange(n) + 0.5) / n
    q = p[..., None, :] + d[..., None, :] * (L[..., None, None] * ts[..., :, None])
    return blob.s0 * blob.dens(q).mean(-1) * L


def basis(view):
    a = np.array([0, 0, 1.]) if abs(view[2]) < 0.9 else np.array([1., 0, 0])
    u = np.cross(view, a); u /= np.linalg.norm(u)
    return u, np.cross(view, u)


def reference(blob, view, sun, N, spp, maxb=3000):
    """Orthographic camera; returns the multiply scattered radiance per pixel."""
    u, w = basis(view)
    xs = (np.arange(N) + 0.5) / N * 2.2 - 1.1
    X, Y = np.meshgrid(xs, xs)
    P = np.repeat(np.stack([X.ravel(), Y.ravel()], 1), spp, 0)
    o = P[:, 0:1] * u + P[:, 1:2] * w - view * 1.5
    d = np.tile(view, (len(P), 1))
    b = np.sum(o * d, 1); disc = b * b - (np.sum(o * o, 1) - 1)
    p = o + d * (-b - np.sqrt(np.maximum(disc, 0)))[:, None]
    ms = np.zeros(len(P)); nb = np.zeros(len(P), int)
    idx = np.nonzero(disc > 0)[0]
    while len(idx):
        pp, dd = p[idx], d[idx]
        done = np.zeros(len(idx), bool); coll = np.zeros(len(idx), bool)
        while not done.all():                                   # delta tracking to a real collision or the exit
            k = ~done
            step = -np.log(rng.random(k.sum())) / blob.s0
            leave = step >= chord_exit(pp[k], dd[k])
            pk = pp[k] + dd[k] * step[:, None]
            acc = rng.random(k.sum()) < blob.dens(pk)
            kk = np.nonzero(k)[0]
            pp[kk[~leave]] = pk[~leave]
            done[kk[leave]] = True
            hit = (~leave) & acc
            done[kk[hit]] = True; coll[kk[hit]] = True
        gi = idx[coll]
        if len(gi) == 0:
            break
        pc, dc = pp[coll], dd[coll]
        L = chord_exit(pc, np.tile(sun, (len(pc), 1)))
        ts = (np.arange(48) + 0.5) / 48
        q = pc[:, None, :] + sun[None, None, :] * (L[:, None, None] * ts[None, :, None])
        contrib = hg(np.sum(dc * sun, 1), G) * np.exp(-blob.s0 * blob.dens(q).mean(1) * L)
        later = nb[gi] > 0
        ms[gi[later]] += contrib[later]
        nb[gi] += 1
        p[gi] = pc; d[gi] = hg_sample(dc, G, len(gi))
        idx = gi[nb[gi] < maxb]
    return ms.reshape(-1, spp).mean(1).reshape(N, N)


def march_inputs(blob, view, sun, N, ns=300):
    u, w = basis(view)
    xs = (np.arange(N) + 0.5) / N * 2.2 - 1.1
    X, Y = np.meshgrid(xs, xs)
    o = X.ravel()[:, None] * u + Y.ravel()[:, None] * w - view * 1.5
    dt = 3.0 / ns
    P = o[:, None, :] + view * ((np.arange(ns) + 0.5) * dt)[None, :, None]
    sig = blob.s0 * blob.dens(P)
    tau = np.cumsum(sig * dt, 1) - sig * dt                      # view depth at each step's start
    m = sig > 0
    tT = np.zeros_like(sig); tB = np.zeros_like(sig)
    tT[m] = optdepth(blob, P[m], np.tile(sun, (m.sum(), 1)))
    tB[m] = optdepth(blob, P[m], np.tile(-sun, (m.sum(), 1)))
    return dict(sig=sig, T=np.exp(-tau), tau=tau, tT=tT, tB=tB, dt=dt, cosK=float(view @ sun), N=N, m=m)


def silver(d):     # the march's "Silver lining" at 0.5
    return 0.5 * (0.12 * hg(d['cosK'], 0.92) + 0.5 * hg(d['cosK'], 0.8)) * np.exp(-0.3 * d['tT'])


def model_old(d, a=0.214, b=0.557):
    pMS = 0.85 * hg(d['cosK'], 0.2) + 0.15 * hg(d['cosK'], -0.4)
    return pMS * (b * np.exp(-a * d['tT']) + b * b * np.exp(-a * a * d['tT'])) + silver(d)


def model_new(d, gain=1.0):
    """cv2DiffuseField, the shader's formula (the view leak averaged over the step as the march does)."""
    g, f = G, G * G; gp = g / (1 + g)
    t = (1 - f) * d['tT']; ts = t + (1 - f) * d['tB']; et = np.exp(-t)
    C1 = (5 - np.exp(-ts)) / ((1 - gp) * ts + 4 / 3)
    I1 = C1 - 3 * et
    I0 = 5 - (2 / 3) * C1 - (1 - gp) * C1 * t - 3 * et
    J = np.maximum(I0 + 0.3 * g * d['cosK'] * I1, 0) + (1 - f) * max(1 + 3 * gp * d['cosK'], 0) * np.maximum(et - np.exp(-d['tT']), 0)
    tS = d['sig'] * d['dt']; aV = 1 - f
    hV = 1 - np.exp(-aV * d['tau']) * (1 - np.exp(-(1 + aV) * tS)) / ((1 + aV) * np.maximum(1 - np.exp(-tS), 1e-6))
    return gain * 1.6 * J * np.exp(-0.05 * t) * hV + silver(d)


def image(d, S):
    w = d['T'] * (1 - np.exp(-d['sig'] * d['dt']))              # the march's sample weight
    return (w * np.where(d['m'], S, 0)).sum(1).reshape(d['N'], d['N'])


SUNS = {'top': [-0.2, 0, 1.], 'front': [-1., 0, 0.35], 'back': [1., 0, 0.35], 'side': [-0.4, 0.5, 0.75]}
CASES = [('soft', 2, 'top'), ('soft', 2, 'front'), ('soft', 5, 'top'), ('soft', 5, 'front'), ('soft', 5, 'back'),
         ('soft', 30, 'top'), ('soft', 30, 'front'), ('soft', 30, 'back'), ('lumpy', 60, 'top'), ('lumpy', 60, 'front'),
         ('lumpy', 60, 'back'), ('hard', 30, 'top'), ('cauli', 80, 'side'), ('cauli', 80, 'front')]


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--png')
    a = ap.parse_args()
    from scipy.ndimage import gaussian_filter
    view = np.array([1., 0, 0])
    rows = []
    print(f"{'blob':>10} {'tau_r':>5} {'light':>6} | {'ref mean':>8} | {'old r':>6} {'old/ref':>7} | {'new r':>6} {'new/ref':>7}")
    for kind, s0, light in CASES:
        N = 24 if a.quick else (48 if kind == 'cauli' else 32)
        spp = 128 if a.quick else (192 if kind == 'cauli' else 512)
        sun = np.array(SUNS[light]); sun /= np.linalg.norm(sun)
        blob = Blob(float(s0), kind)
        ref = gaussian_filter(reference(blob, view, sun, N, spp), 0.7)
        d = march_inputs(blob, view, sun, N)
        ims = [ref, image(d, model_old(d)), image(d, model_new(d))]
        m = ref > 0
        st = [(np.corrcoef(ref[m], im[m])[0, 1], im[m].mean() / ref[m].mean()) for im in ims[1:]]
        print(f"{kind:>10} {s0:>5} {light:>6} | {ref[m].mean():8.3f} | {st[0][0]:6.3f} {st[0][1]:7.2f} | {st[1][0]:6.3f} {st[1][1]:7.2f}", flush=True)
        if a.png:
            from PIL import Image
            t = []
            for im in ims:
                v = im / max(np.percentile(im[im > 0], 99.5), 1e-9) if (im > 0).any() else im
                t.append(np.array(Image.fromarray((np.clip(v, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)[::-1]).resize((96, 96))))
            rows.append(np.concatenate([np.pad(x, 2, constant_values=90) for x in t], 1))
    if a.png:
        from PIL import Image
        Image.fromarray(np.concatenate(rows, 0)).save(a.png)


if __name__ == '__main__':
    main()
