"""Path-traced reference for the cloud march's DROPLET PHASE model (cloud_v2_march.comp, "Droplet phase", cv2.micro).

tools/cloud_ms_reference.py fitted the diffuse field (cv2DiffuseField) against blobs of Henyey-Greenstein g = 0.85 cloud.
Real water cloud is not HG: about half its scattering is a diffraction peak a few degrees wide, the rest a broad lobe with a
cloudbow and a glory. This renders the same blobs with that phase (Jendersie & d'Eon 2023, "An Approximate Mie Scattering
Function for Fog and Cloud Rendering": HG + Draine fitted to Lorenz-Mie for droplet diameters 5-50 um), keeps the light
scattered TWICE OR MORE (the march's single scattering is the Mie table, exact), and evaluates the march's multiple
scattering on the same blobs with exact optical depths:

  hg     the diffuse field as shipped (g 0.85, the forward beam's extra light at the Eddington phase), gain 1
  micro  the droplet model: the diffuse field at the droplets' own mean cosine, its gain, and the forward ORDERS (light
         scattered n >= 1 times into the diffraction peak, then once toward the eye) at the droplet phase with its peak
         convolved once more (HG composition: HG(g) * HG(g) = HG(g^2))

and prints, per blob and lighting: the correlation with the reference and the mean level against it, for the gains given.

    python tools/cloud_micro_reference.py --quick [--d 20] [--gain 1.8] [--png out.png]

Measured 2026-11-04 (256 spp, peak capped at g 0.97, d = 20 um): mean level against the reference, shipped HG model 0.69
(backlit 0.38-0.42), droplet model at gain 1.8 with the T / (T + 2) side leak 1.01 (thick 0.99-1.08, backlit 0.68-0.78,
thin soft blobs 0.9-2.4); d = 10 um 1.04. Default --variant B is the shader's model; A is the first cut (one forward order).
"""
import argparse
import numpy as np
import cloud_ms_reference as R


def jde(d):
    """Jendersie & d'Eon 2023: (w_D, g_HG, g_D, alpha) for droplet diameter d (um, 5-50)."""
    gHG = np.exp(-0.0990567 / (d - 1.67154))
    gD = np.exp(-2.20679 / (d + 3.91029) - 0.428934)
    a = np.exp(3.62489 - 8.29288 / (d + 5.52825))
    wD = np.exp(-0.599085 / (d - 0.641583) - 0.665888)
    return wD, min(gHG, PEAK_CAP), gD, a


# The diffraction peak (g ~0.99, ~1 deg) makes next-event estimation spiky: a path that happens to point within a degree of
# the Sun scores ~5000x. The test views are >= 20 deg from the Sun, where a peak of g 0.97 (~5 deg) scatters the same light,
# so the reference and the model both cap it there (--peak 1 for the true peak).
PEAK_CAP = 0.97


def draine(c, g, a):    # mean 1 over the sphere
    return R.hg(c, g) * (1 + a * c * c) / (1 + a * (1 + 2 * g * g) / 3)


def phase(c, d):
    wD, gHG, gD, a = jde(d)
    return (1 - wD) * R.hg(c, gHG) + wD * draine(c, gD, a)


def phase_fwd(c, d):
    """The forward orders' phase: the peak convolved once more (HG(g^2)), the broad part unchanged."""
    wD, gHG, gD, a = jde(d)
    return (1 - wD) * R.hg(c, gHG * gHG) + wD * draine(c, gD, a)


class Sampler:
    def __init__(self, d):
        th = np.concatenate([[0.0], np.geomspace(1e-5, np.pi, 20000)])
        c = np.cos(th)
        p = phase(c, d) * np.sin(th)
        cdf = np.concatenate([[0.0], np.cumsum(0.5 * (p[1:] + p[:-1]) * np.diff(th))])
        self.cdf = cdf / cdf[-1]; self.c = c
        self.g = float(np.sum(0.5 * ((p * c)[1:] + (p * c)[:-1]) * np.diff(th)) / (cdf[-1]))

    def sample(self, dvec, n):
        cost = np.interp(R.rng.random(n), self.cdf, self.c)
        sint, phi = np.sqrt(np.maximum(1 - cost ** 2, 0)), 2 * np.pi * R.rng.random(n)
        a = np.where(np.abs(dvec[:, 2:3]) < 0.9, np.array([[0, 0, 1.]]), np.array([[1., 0, 0]]))
        t1 = np.cross(dvec, a); t1 /= np.linalg.norm(t1, axis=1, keepdims=True); t2 = np.cross(dvec, t1)
        return dvec * cost[:, None] + t1 * (sint * np.cos(phi))[:, None] + t2 * (sint * np.sin(phi))[:, None]


def reference(blob, view, sun, N, spp, d, S, maxb=3000):
    u, w = R.basis(view)
    xs = (np.arange(N) + 0.5) / N * 2.2 - 1.1
    X, Y = np.meshgrid(xs, xs)
    P = np.repeat(np.stack([X.ravel(), Y.ravel()], 1), spp, 0)
    o = P[:, 0:1] * u + P[:, 1:2] * w - view * 1.5
    dd0 = np.tile(view, (len(P), 1))
    b = np.sum(o * dd0, 1); disc = b * b - (np.sum(o * o, 1) - 1)
    p = o + dd0 * (-b - np.sqrt(np.maximum(disc, 0)))[:, None]
    dvec = dd0.copy()
    ms = np.zeros(len(P)); nb = np.zeros(len(P), int)
    idx = np.nonzero(disc > 0)[0]
    while len(idx):
        pp, dd = p[idx], dvec[idx]
        done = np.zeros(len(idx), bool); coll = np.zeros(len(idx), bool)
        while not done.all():
            k = ~done
            step = -np.log(R.rng.random(k.sum())) / blob.s0
            leave = step >= R.chord_exit(pp[k], dd[k])
            pk = pp[k] + dd[k] * step[:, None]
            acc = R.rng.random(k.sum()) < blob.dens(pk)
            kk = np.nonzero(k)[0]
            pp[kk[~leave]] = pk[~leave]
            done[kk[leave]] = True
            hit = (~leave) & acc
            done[kk[hit]] = True; coll[kk[hit]] = True
        gi = idx[coll]
        if len(gi) == 0:
            break
        pc, dc = pp[coll], dd[coll]
        L = R.chord_exit(pc, np.tile(sun, (len(pc), 1)))
        ts = (np.arange(48) + 0.5) / 48
        q = pc[:, None, :] + sun[None, None, :] * (L[:, None, None] * ts[None, :, None])
        contrib = phase(np.sum(dc * sun, 1), d) * np.exp(-blob.s0 * blob.dens(q).mean(1) * L)
        later = nb[gi] > 0
        ms[gi[later]] += contrib[later]
        nb[gi] += 1
        p[gi] = pc; dvec[gi] = S.sample(dc, len(gi))
        idx = gi[nb[gi] < maxb]
    return ms.reshape(-1, spp).mean(1).reshape(N, N)


def hv_leak(dat, f):
    tS = dat['sig'] * dat['dt']; aV = 1 - f
    return 1 - np.exp(-aV * dat['tau']) * (1 - np.exp(-(1 + aV) * tS)) / ((1 + aV) * np.maximum(1 - np.exp(-tS), 1e-6))


def phase_order(c, d, n, gtot):
    """The phase of n scatterings composed (Legendre moments ^ n): the peak exactly (HG(g^n)), the rest an HG keeping the
    first moment (gtot^n)."""
    wD, gHG, gD, a = jde(d)
    wp = (1 - wD) ** n
    gb = (gtot ** n - wp * gHG ** n) / max(1 - wp, 1e-6)
    return wp * R.hg(c, gHG ** n) + (1 - wp) * R.hg(c, np.clip(gb, -0.9, 0.99))


VARIANT = 'B'
KMAX = 3


def model_micro(dat, d, g, gain, fwd_gain=1.0):
    if VARIANT == 'B':
        return model_B(dat, d, g, gain)
    return model_A(dat, d, g, gain, fwd_gain)


def model_B(dat, d, g, gain):
    f = g * g; gp = g / (1 + g); mu = dat['cosK']
    t = (1 - f) * dat['tT']; ts = t + (1 - f) * dat['tB']; et = np.exp(-t)
    C1 = (5 - np.exp(-ts)) / ((1 - gp) * ts + 4 / 3)
    I1 = C1 - 3 * et
    I0 = 5 - (2 / 3) * C1 - (1 - gp) * C1 * t - 3 * et
    J = np.maximum(I0 + 0.3 * g * mu * I1, 0)
    tau = dat['tT']; F = 0; pk = np.exp(-tau)
    for k in range(1, KMAX + 1):
        pk = pk * tau / k
        F = F + pk * phase_order(mu, d, k + 1, g)
    T = dat['tT'] + dat['tB']
    return gain * T / (T + 2.0) * 1.6 * J * np.exp(-0.05 * t) * hv_leak(dat, f) + F


def model_A(dat, d, g, gain, fwd_gain=1.0):
    """The shader's droplet model (cv2DiffuseField with cv2.micro.x on), the multiple scattering only."""
    f = g * g; gp = g / (1 + g); mu = dat['cosK']
    t = (1 - f) * dat['tT']; ts = t + (1 - f) * dat['tB']; et = np.exp(-t)
    C1 = (5 - np.exp(-ts)) / ((1 - gp) * ts + 4 / 3)
    I1 = C1 - 3 * et
    I0 = 5 - (2 / 3) * C1 - (1 - gp) * C1 * t - 3 * et
    J = np.maximum(I0 + 0.3 * g * mu * I1, 0)
    wD = jde(d)[0]; fpk = 1 - wD
    F = phase_fwd(mu, d) * np.maximum(np.exp(-(1 - fpk) * dat['tT']) - np.exp(-dat['tT']), 0)
    return gain * 1.6 * J * np.exp(-0.05 * t) * hv_leak(dat, f) + fwd_gain * F


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument('--quick', action='store_true')
    ap.add_argument('--d', type=float, default=20.0, help='droplet diameter (um)')
    ap.add_argument('--gain', type=float, nargs='+', default=[1.8])
    ap.add_argument('--png')
    ap.add_argument('--cases', default='')
    ap.add_argument('--variant', default='B')
    ap.add_argument('--peak', type=float, default=0.97)
    ap.add_argument('--kmax', type=int, default=3)
    a = ap.parse_args()
    global VARIANT, KMAX, PEAK_CAP
    VARIANT, KMAX, PEAK_CAP = a.variant, a.kmax, a.peak
    from scipy.ndimage import gaussian_filter
    S = Sampler(a.d)
    print(f"d = {a.d} um: phase mean cosine {S.g:.3f}; peak share {1 - jde(a.d)[0]:.3f}, g_peak {jde(a.d)[1]:.4f}")
    view = np.array([1., 0, 0])
    cases = R.CASES if not a.cases else [c for c in R.CASES if f"{c[0]}{c[1]}{c[2]}" in a.cases.split(',')]
    hdr = ' | '.join(f"g{x:<4} r  lvl" for x in a.gain)
    print(f"{'blob':>6} {'tau':>4} {'light':>6} | {'ref':>6} | {'hg r':>5} {'lvl':>5} | {hdr}")
    rows = []
    for kind, s0, light in cases:
        N = 24 if a.quick else 32
        spp = 128 if a.quick else 384
        sun = np.array(R.SUNS[light]); sun /= np.linalg.norm(sun)
        blob = R.Blob(float(s0), kind)
        ref = gaussian_filter(reference(blob, view, sun, N, spp, a.d, S), 0.7)
        dat = R.march_inputs(blob, view, sun, N)
        hgm = R.image(dat, R.model_new(dat))
        ims = [R.image(dat, model_micro(dat, a.d, S.g, gn)) for gn in a.gain]
        m = ref > 0
        def st(im): return np.corrcoef(ref[m], im[m])[0, 1], im[m].mean() / ref[m].mean()
        s = ' | '.join(f"{st(im)[0]:6.3f} {st(im)[1]:5.2f}" for im in ims)
        print(f"{kind:>6} {s0:>4} {light:>6} | {ref[m].mean():6.3f} | {st(hgm)[0]:5.3f} {st(hgm)[1]:5.2f} | {s}", flush=True)
        if a.png:
            from PIL import Image
            t = []
            for im in [ref, hgm] + ims:
                v = im / max(np.percentile(ref[ref > 0], 99.5), 1e-9) if (ref > 0).any() else im
                t.append(np.array(Image.fromarray((np.clip(v, 0, 1) ** (1 / 2.2) * 255).astype(np.uint8)[::-1]).resize((96, 96))))
            rows.append(np.concatenate([np.pad(x, 2, constant_values=90) for x in t], 1))
    if a.png:
        from PIL import Image
        Image.fromarray(np.concatenate(rows, 0)).save(a.png)


if __name__ == '__main__':
    main()
