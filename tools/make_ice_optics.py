#!/usr/bin/env python3
"""Ice-crystal halo tables (2026-11-10, issue_20261110_180734 "Optics physics").

Writes assets/textures/ice_optics.rgba8: the single-scattering phase function of sunlight through hexagonal ice
crystals, from Monte-Carlo geometric optics (refraction, Fresnel reflection, total internal reflection through
the eight faces of a hexagonal prism, per colour channel at the ice indices R 650 / G 550 / B 450 nm), for the
crystal POPULATIONS whose orientation decides which halos exist:

  random  A  columns  c/2a = 2.5, every orientation      22 deg halo, a faint 46 deg halo (the circular halos)
  random  B  compact  c/2a = 1.0, every orientation      22 + 46 deg halos (the 90 deg prism needs a basal face)
  plates     c/2a = 0.25, c-axis vertical, tilt 1 deg     sundogs, parhelic circle, 120 deg parhelia, circumzenithal
                                                           (Sun < 32.2 deg) and circumhorizon (Sun > 57.8 deg) arcs,
                                                           subsun, subparhelia, subparhelic circle
  columns    c/2a = 2.5, c-axis horizontal, random spin   upper / lower tangent arcs -> the circumscribed halo above
             tilt 1.5 deg                                  ~29 deg, supralateral + infralateral arcs
  parry      c/2a = 2.5, c-axis horizontal, one prism      Parry arcs (suncave / sunvex), Tape arcs, the Parry
             face up (spin 30 +- 1 deg)                     supralateral arcs

Nothing is placed by hand: every arc is where the crystals send the rays. The orientation and shape ARE the
material dependence of halos (a 22 deg halo needs 60 deg prism faces in any orientation, a sundog needs plates
falling flat, a Parry arc needs columns held with a face level); iridescence and coronae are diffraction by
small droplets / young ice and are not here (the Mie table, tools/make_mie_lut.py, covers the droplet side).

Layout (raw, little-endian), a 3D RGBA8 texture of NT x H x NE texels:
    header  'SLIO', uint32 NT, uint32 H, uint32 NE, uint32 NP, float32 PMAX, uint32 NRAND, uint32 NPOP
    data    NE layers (Sun elevation) x H rows x NT texels of RGBA8, row-major, layer-major.
    x: the scattering angle theta from the light, 0..64 deg at 0.5 deg (128 texels), then 64..180 at 1 deg (116).
    y: rows 0-1 random A, rows 2-3 random B (phi-independent: the two rows are identical, sample the row seam);
       then NP = 91 rows per oriented population (plates, columns, parry): phi = the azimuth about the light's
       direction from its zenith side, 0..180 deg at 2 deg (folded: the pattern is mirror-symmetric about the
       Sun's vertical plane for every population).
    z: the light's elevation above the crystals' local horizontal: 0..20 at 1 deg (21 layers), 22.5..60 at 2.5 (16),
       65..90 at 5 (6).
       A light BELOW the local horizontal is the mirror image (elevation -e, phi -> 180 - phi): the shader does that.
    texel = (P / PMAX)^(1/3) in 8 bits, P = 4 pi dN / (N dOmega): the phase function in this codebase's convention
    (isotropic = 1, cv2HG), WITHOUT the undeviated rays (theta < 0.3 deg: two parallel faces; the march carries
    the forward peak as delta-scaled extinction). A = 255 (unused).

    python tools/make_ice_optics.py                # writes the table (~2-3 min) and tools/out/ice_optics_preview.png
    python tools/make_ice_optics.py --check        # the geometry audit only: predicted vs traced feature positions
    python tools/make_ice_optics.py --rays 4000000 # more rays per (population, elevation, channel)
Standard library + numpy + scipy (+ Pillow for the preview).
"""
import argparse, math, os, struct, sys, time
import numpy as np

# ── Table layout (mirrored by shaders/include/cv2_optics.glsl: change both) ──────────────────────────────────
NT_FINE, NT_COARSE = 128, 116
NT = NT_FINE + NT_COARSE            # 244 theta texels
NP = 91                             # phi rows per oriented population (0..180 deg at 2 deg)
NRAND = 2                           # random populations, 2 rows each
ROW0 = 2 * NRAND                    # first oriented row (4)
POPS = ["plates", "columns", "parry"]
NPOP = len(POPS)
H = ROW0 + NP * NPOP                # 277 rows
# 0..20 at 1 deg: below ~20 deg the plate features that ride with the Sun (the subsun at 2e below it, the subparhelia,
# the sundogs' tails) move 2 deg of sky per degree of elevation, and a trilinear blend of two layers 2.5 deg apart drew
# them as two lobes 5 deg apart, a streak ~40% longer than either (issue_20261111_164656 "Weird optic"). 1 deg: 2 apart.
ELEVS = [1.0 * i for i in range(21)] + [20.0 + 2.5 * i for i in range(1, 17)] + [65.0, 70.0, 75.0, 80.0, 85.0, 90.0]   # 43
NE = len(ELEVS)
assert NE == 43
PMAX = 512.0
N_ICE = np.array([1.3069, 1.3108, 1.3165])        # R, G, B (CV2_N_ICE)
DELTA_DEG = 0.3                                   # rays this close to undeviated are the forward peak, not the table

THETA_EDGES = np.concatenate([np.arange(0.0, 64.0, 0.5), np.arange(64.0, 180.0001, 1.0)])   # NT + 1 edges
assert len(THETA_EDGES) == NT + 1
THETA_MID = 0.5 * (THETA_EDGES[:-1] + THETA_EDGES[1:])
PHI_MID = 2.0 * np.arange(NP)

# ── Crystal: a hexagonal prism, circumradius 1, length c; faces as (outward normal, distance) ───────────────
def crystal(aspect):
    c = 2.0 * aspect
    ang = np.radians(30.0 + 60.0 * np.arange(6))
    normals = np.zeros((8, 3))
    normals[:6, 0] = np.cos(ang)
    normals[:6, 1] = np.sin(ang)
    normals[6] = (0, 0, 1)
    normals[7] = (0, 0, -1)
    dist = np.full(8, math.cos(math.radians(30.0)))
    dist[6:] = 0.5 * c
    area = np.full(8, c)                         # prism faces: edge 1 x length c
    area[6:] = 1.5 * math.sqrt(3.0)              # hexagon of circumradius 1
    va = np.radians(60.0 * np.arange(7))
    verts = np.stack([np.cos(va), np.sin(va)], axis=1)   # 7: closes the ring
    return normals, dist, area, verts, c

def rot_z(a):
    c, s = np.cos(a), np.sin(a)
    z, o = np.zeros_like(a), np.ones_like(a)
    return np.stack([np.stack([c, -s, z], -1), np.stack([s, c, z], -1), np.stack([z, z, o], -1)], -2)

def rot_y(a):
    c, s = np.cos(a), np.sin(a)
    z, o = np.zeros_like(a), np.ones_like(a)
    return np.stack([np.stack([c, z, s], -1), np.stack([z, o, z], -1), np.stack([-s, z, c], -1)], -2)

def rot_axis(axis, a):
    """Rodrigues: rotate by a about unit axis (N,3)."""
    x, y, z = axis[:, 0], axis[:, 1], axis[:, 2]
    c, s = np.cos(a), np.sin(a)
    C = 1.0 - c
    return np.stack([np.stack([c + x * x * C, x * y * C - z * s, x * z * C + y * s], -1),
                     np.stack([y * x * C + z * s, c + y * y * C, y * z * C - x * s], -1),
                     np.stack([z * x * C - y * s, z * y * C + x * s, c + z * z * C], -1)], -2)

def random_rotations(rng, n):
    q = rng.standard_normal((n, 4))
    q /= np.linalg.norm(q, axis=1, keepdims=True)
    w, x, y, z = q[:, 0], q[:, 1], q[:, 2], q[:, 3]
    return np.stack([np.stack([1 - 2 * (y * y + z * z), 2 * (x * y - z * w), 2 * (x * z + y * w)], -1),
                     np.stack([2 * (x * y + z * w), 1 - 2 * (x * x + z * z), 2 * (y * z - x * w)], -1),
                     np.stack([2 * (x * z - y * w), 2 * (y * z + x * w), 1 - 2 * (x * x + y * y)], -1)], -2)

def orientations(pop, rng, n):
    """World = R . crystal. The crystal's c-axis is its z."""
    if pop.startswith("random"):
        return random_rotations(rng, n)
    if pop == "plates":
        sigma = math.radians(1.0)
        tx, ty = rng.standard_normal(n) * sigma, rng.standard_normal(n) * sigma
        tau = np.sqrt(tx * tx + ty * ty)
        alpha = np.arctan2(ty, tx)
        axis = np.stack([-np.sin(alpha), np.cos(alpha), np.zeros(n)], -1)
        return rot_axis(axis, tau) @ rot_z(rng.uniform(0, 2 * np.pi, n))
    # c-axis horizontal: Rz(azimuth) Ry(tilt) Ry(90) Rz(spin)
    if pop == "columns":
        spin = rng.uniform(0, 2 * np.pi, n)
        tilt = rng.standard_normal(n) * math.radians(1.5)
    else:   # parry: one prism face level (spin 30 deg puts a prism-face normal on the vertical)
        spin = np.radians(30.0) + rng.standard_normal(n) * math.radians(1.0)
        tilt = rng.standard_normal(n) * math.radians(1.0)
    az = rng.uniform(0, 2 * np.pi, n)
    return rot_z(az) @ rot_y(tilt) @ rot_y(np.full(n, 0.5 * np.pi)) @ rot_z(spin)

def sample_entry(rng, din, normals, dist, area, verts, c):
    """Uniform points on the crystal's projection along din (N,3, into the crystal): face index + point."""
    n = din.shape[0]
    proj = area[None, :] * np.maximum(0.0, -(din @ normals.T))        # (N,8)
    cum = np.cumsum(proj, axis=1)
    r = rng.uniform(0, 1, n) * cum[:, -1]
    face = (cum < r[:, None]).sum(axis=1)
    face = np.minimum(face, 7)
    p = np.zeros((n, 3))
    u1, u2 = rng.uniform(0, 1, n), rng.uniform(0, 1, n)
    prism = face < 6
    k = np.where(prism, face, 0)
    v0, v1 = verts[k], verts[k + 1]
    pp = v0 + u1[:, None] * (v1 - v0)
    p[prism, 0] = pp[prism, 0]
    p[prism, 1] = pp[prism, 1]
    p[prism, 2] = (u2[prism] - 0.5) * c
    basal = ~prism
    nb = int(basal.sum())
    if nb:
        t = rng.integers(0, 6, nb)
        a, b = rng.uniform(0, 1, nb), rng.uniform(0, 1, nb)
        sa = np.sqrt(a)
        w1, w2 = sa * (1 - b), sa * b           # uniform in the triangle (0, v_t, v_t+1)
        q = w1[:, None] * verts[t] + w2[:, None] * verts[t + 1]
        p[basal, 0] = q[:, 0]
        p[basal, 1] = q[:, 1]
        p[basal, 2] = np.where(face[basal] == 6, 0.5 * c, -0.5 * c)
    return face, p

def fresnel(cos_i, cos_t, n1, n2):
    rs = (n1 * cos_i - n2 * cos_t) / (n1 * cos_i + n2 * cos_t)
    rp = (n1 * cos_t - n2 * cos_i) / (n1 * cos_t + n2 * cos_i)
    return 0.5 * (rs * rs + rp * rp)

def trace(din, face, p, u_entry, u_bounce, normals, dist, n_ice, max_bounce):
    """Geometric optics through one crystal for N rays (crystal frame). Returns the outgoing directions (N,3)
    and a mask of rays that left (the rest were still inside after max_bounce interactions)."""
    N = din.shape[0]
    out = np.zeros((N, 3))
    left = np.zeros(N, dtype=bool)
    nf = normals[face]                                   # (N,3) outward normal of the entry face
    cos_i = -np.einsum("ij,ij->i", din, nf)
    sin_t2 = (1.0 - cos_i * cos_i) / (n_ice * n_ice)
    cos_t = np.sqrt(np.maximum(1.0 - sin_t2, 0.0))
    R = fresnel(cos_i, cos_t, 1.0, n_ice)
    refl = u_entry < R
    out[refl] = din[refl] + 2.0 * cos_i[refl, None] * nf[refl]       # external reflection: done
    left[refl] = True
    idx = np.nonzero(~refl)[0]
    d = (din[idx] + cos_i[idx, None] * nf[idx]) / n_ice - cos_t[idx, None] * nf[idx]
    d /= np.linalg.norm(d, axis=1, keepdims=True)
    pos = p[idx]
    last = face[idx]
    for b in range(max_bounce):
        if idx.size == 0:
            break
        denom = d @ normals.T                             # (M,8)
        tt = (dist[None, :] - pos @ normals.T) / np.where(denom > 1e-12, denom, 1e-12)
        tt[denom <= 1e-12] = np.inf
        tt[np.arange(idx.size), last] = np.inf
        f = np.argmin(tt, axis=1)
        t = tt[np.arange(idx.size), f]
        pos = pos + t[:, None] * d
        nf = normals[f]
        ci = np.einsum("ij,ij->i", d, nf)
        st2 = n_ice * n_ice * (1.0 - ci * ci)
        tir = st2 >= 1.0
        ct = np.sqrt(np.maximum(1.0 - st2, 0.0))
        Rf = np.where(tir, 1.0, fresnel(ci, ct, n_ice, 1.0))
        reflect = u_bounce[b][idx] < Rf
        ex = ~reflect
        if ex.any():
            dout = n_ice * (d[ex] - ci[ex, None] * nf[ex]) + ct[ex, None] * nf[ex]
            dout /= np.linalg.norm(dout, axis=1, keepdims=True)
            out[idx[ex]] = dout
            left[idx[ex]] = True
        d = d - 2.0 * ci[:, None] * nf
        keep = reflect
        idx, d, pos, last = idx[keep], d[keep], pos[keep], f[keep]
    return out, left

def bins_for(o, s, up):
    """theta bin, phi bin, delta mask for outgoing world directions o (N,3) about the light direction s. The eye sees
    a ray travelling along o as coming FROM -o on the sky: theta is that sky point's distance from the light."""
    o = -o
    cos_th = np.clip(o @ s, -1.0, 1.0)
    th = np.degrees(np.arccos(cos_th))
    u = up - s * (s @ up)
    nu = np.linalg.norm(u)
    u = u / nu if nu > 1e-6 else np.array([-1.0, 0.0, 0.0])
    r = np.cross(s, u)
    phi = np.degrees(np.arctan2(np.abs(o @ r), o @ u))
    it = np.clip(np.searchsorted(THETA_EDGES, th, side="right") - 1, 0, NT - 1)
    ip = np.clip(np.rint(phi / 2.0).astype(int), 0, NP - 1)
    return it, ip, th < DELTA_DEG

def solid_angle_2d():
    """Steradians per folded (theta, phi) bin: (cos t0 - cos t1) x the phi width (2 deg at the ends, 4 inside)."""
    e = np.radians(THETA_EDGES)
    dth = np.cos(e[:-1]) - np.cos(e[1:])
    w = np.full(NP, np.radians(4.0))
    w[0] = w[-1] = np.radians(2.0)
    return dth[:, None] * w[None, :]

def solid_angle_1d():
    e = np.radians(THETA_EDGES)
    return 2.0 * np.pi * (np.cos(e[:-1]) - np.cos(e[1:]))

def smooth(P, sigma_tex, sigma_row):
    from scipy.ndimage import gaussian_filter1d
    Q = gaussian_filter1d(P, sigma_tex, axis=0, mode="nearest")
    if P.ndim == 2 and sigma_row > 0:
        Q = gaussian_filter1d(Q, sigma_row, axis=1, mode="nearest")
    return Q

def denoise(P):
    """Keep bright features sharp (the Sun's 0.53 deg disk is the only blur), smooth the dim background more."""
    narrow = smooth(P, 0.4, 0.3)
    if P.ndim == 2:
        wide = smooth(P, 1.5, 1.2)
    else:
        wide = smooth(P, 1.5, 0.0)
    w = np.clip((wide - 0.5) / 4.0, 0.0, 1.0)          # above ~4.5 x isotropic: a feature, keep narrow
    return narrow * w + wide * (1.0 - w)

def run_population(pop, elev_deg, nrays, rng, max_bounce=12):
    """P[ch] (NT,NP) for an oriented population, (NT,) for a random one, plus the undeviated share per channel."""
    aspect = {"randomA": 2.5, "randomB": 1.0, "plates": 0.25, "columns": 2.5, "parry": 2.5}[pop]
    normals, dist, area, verts, c = crystal(aspect)
    e = math.radians(elev_deg)
    s = np.array([math.cos(e), 0.0, math.sin(e)])
    up = np.array([0.0, 0.0, 1.0])
    Rm = orientations(pop, rng, nrays)
    D = -s
    din = np.einsum("nji,j->ni", Rm, D)                 # R^T D
    face, p = sample_entry(rng, din, normals, dist, area, verts, c)
    u_entry = rng.uniform(0, 1, nrays)
    u_bounce = rng.uniform(0, 1, (max_bounce, nrays))
    oriented = not pop.startswith("random")
    P = np.zeros((3, NT, NP)) if oriented else np.zeros((3, NT))
    delta = np.zeros(3)
    for ch in range(3):
        out, left = trace(din, face, p, u_entry, u_bounce, normals, dist, N_ICE[ch], max_bounce)
        o = np.einsum("nij,nj->ni", Rm[left], out[left])
        it, ip, isdelta = bins_for(o, s, up)
        delta[ch] = isdelta.sum() / nrays
        keep = ~isdelta
        if oriented:
            flat = np.bincount(it[keep] * NP + ip[keep], minlength=NT * NP).reshape(NT, NP)
            P[ch] = 4.0 * np.pi * flat / nrays / solid_angle_2d()
        else:
            flat = np.bincount(it[keep], minlength=NT)
            P[ch] = 4.0 * np.pi * flat / nrays / solid_angle_1d()
    return P, delta

def encode(P):
    return np.clip(np.cbrt(np.clip(P, 0.0, None) / PMAX), 0.0, 1.0)

# ── The geometry audit: closed-form positions the trace must reproduce ───────────────────────────────────────
def min_dev(n, A_deg):
    return math.degrees(2.0 * math.asin(n * math.sin(math.radians(A_deg) / 2.0)) - math.radians(A_deg))

def sundog_offset(n, e_deg):
    """Horizontal plates, light at elevation e: the 60 deg prism at the effective index n' = sqrt(n^2 - sin^2 e)/cos e,
    the sundog's azimuth from the Sun is D(n') (the angle in the horizontal plane; the sky angle is acos(cos e cos D)...).
    Returns the great-circle distance from the Sun (what theta measures)."""
    e = math.radians(e_deg)
    npr = math.sqrt(n * n - math.sin(e) ** 2) / math.cos(e)
    if npr * 0.5 >= 1.0:
        return None
    Daz = 2.0 * math.asin(npr * 0.5) - math.radians(60.0)
    # the sundog sits at the Sun's elevation, Daz away in azimuth: cos theta = sin^2 e + cos^2 e cos Daz
    return math.degrees(math.acos(math.sin(e) ** 2 + math.cos(e) ** 2 * math.cos(Daz)))

def cza_theta(n, e_deg):
    """Circumzenithal arc: in at the top basal face (incidence 90 - e: sin r = cos e / n), out through a vertical
    prism face (sin o = n cos r = sqrt(n^2 - cos^2 e)); total internal reflection once n^2 - cos^2 e >= 1, i.e. the Sun
    above acos(sqrt(n^2 - 1)) = 32.2 deg. The arc's elevation is o, so it sits theta = o - e above the Sun."""
    e = math.radians(e_deg)
    v = n * n - math.cos(e) ** 2
    if v >= 1.0:
        return None
    return math.degrees(math.asin(math.sqrt(v))) - e_deg

def cha_theta(n, e_deg):
    """Circumhorizon arc: the same 90 deg prism the other way round: in through a vertical prism face (sin r = sin e / n),
    out through the bottom basal face (sin o = n cos r = sqrt(n^2 - sin^2 e) from the vertical); exists only when
    sin^2 e > n^2 - 1, the Sun above 57.8 deg. Its elevation is 90 - o: theta = e - (90 - o) below the Sun."""
    e = math.radians(e_deg)
    v = n * n - math.sin(e) ** 2
    if v >= 1.0:
        return None
    return e_deg - (90.0 - math.degrees(math.asin(math.sqrt(v))))

def peak_theta(P, rows, lo, hi):
    """theta of the maximum of P over rows (slice) between lo..hi deg."""
    m = (THETA_MID >= lo) & (THETA_MID <= hi)
    prof = P[:, rows].mean(axis=1) if P.ndim == 2 else P
    prof = np.where(m, prof, -1.0)
    i = int(np.argmax(prof))
    return THETA_MID[i], prof[i]

def inner_edge_theta(P, rows, lo, hi):
    """theta where P first exceeds half its peak (the halo's sharp inner edge), between lo..hi."""
    m = (THETA_MID >= lo) & (THETA_MID <= hi)
    prof = P[:, rows].mean(axis=1) if P.ndim == 2 else P
    pk = np.max(np.where(m, prof, -1.0))
    for i in np.nonzero(m)[0]:
        if prof[i] >= 0.5 * pk:
            return THETA_MID[i], pk
    return None, pk

def audit(nrays, seed):
    rng = np.random.default_rng(seed)
    ok = True
    for pop in ("randomA", "randomB"):
        print(f"== {pop} ({'columns c/2a 2.5' if pop == 'randomA' else 'compact c/2a 1.0'}): circular halos, per channel ==")
        P, delta = run_population(pop, 20.0, nrays, rng)
        P = np.stack([denoise(P[ch]) for ch in range(3)])
        for ch, nm in enumerate("RGB"):
            for A, lo, hi in ((60.0, 18.0, 30.0), (90.0, 40.0, 56.0)):
                pred = min_dev(N_ICE[ch], A)
                edge, pk = inner_edge_theta(P[ch], None, lo, hi)
                pkt, pkv = peak_theta(P[ch], None, lo, hi)
                d = (edge if edge is not None else 999) - pred
                flag = "ok" if abs(d) <= 0.6 else ("faint" if pkv < 3.0 else "MISS")
                ok &= flag != "MISS"
                print(f"  {nm} {int(A)}-deg prism: predicted minimum deviation {pred:6.2f}  traced inner edge {edge}  peak {pkt:5.1f} x{pkv:6.1f}  {flag}")
        print(f"  undeviated share {delta.mean():.3f}; table integral {float((P[1] * solid_angle_1d()).sum() / (4 * np.pi)):.3f}")
    print("== plates: sundog distance vs Sun elevation (theta at phi = 90 +- 4 deg) ==")
    for e in (0.0, 10.0, 20.0, 30.0, 40.0, 50.0, 60.0):
        P, _ = run_population("plates", e, nrays, rng)
        Pg = denoise(P[1])
        pred = sundog_offset(N_ICE[1], e)
        rows = slice(43, 48)
        if pred is not None:   # the sundog sits at the Sun's elevation: in Sun-centred coordinates its phi is below 90 at height
            ee = math.radians(e)
            npr = math.sqrt(N_ICE[1] ** 2 - math.sin(ee) ** 2) / math.cos(ee)
            Daz = 2.0 * math.asin(npr * 0.5) - math.radians(60.0)
            phi = math.degrees(math.atan2(math.cos(ee) * math.sin(Daz), math.sin(ee) * math.cos(ee) * (1.0 - math.cos(Daz))))
            j = int(round(phi / 2.0))
            rows = slice(max(j - 2, 0), min(j + 3, NP))
        edge, pk = inner_edge_theta(Pg, rows, 18.0, 50.0)
        czp = cza_theta(N_ICE[1], e)
        cz_edge, czpk = inner_edge_theta(Pg, slice(0, 3), 30.0, 70.0) if (czp is not None and e > 0.0) else (None, 0.0)
        if e == 0.0:
            czp = None   # the top face's projected area is 0 with the Sun on the horizon: no light enters it
        pc = Pg[:, :].max()
        sub = None
        if e > 0:   # subsun: theta = 2e straight below (phi = 180)
            sub_t, sub_v = peak_theta(Pg, slice(NP - 2, NP), max(2 * e - 3, 0.5), 2 * e + 3)
            sub = f"subsun theta {sub_t:5.1f} (pred {2 * e:5.1f}) x{sub_v:.0f}"
        line = f"  e={e:4.1f}: sundog pred {pred if pred is None else round(pred, 2)} traced edge {edge} (x{pk:.0f});"
        if czp is not None:
            d = (cz_edge if cz_edge is not None else 999) - czp
            flag = 'ok' if abs(d) <= 1.0 else ('near cutoff' if czp > 48.0 else 'MISS')
            ok &= flag != 'MISS'
            line += f" CZA pred theta {czp:5.2f} traced {cz_edge} (x{czpk:.0f}) {flag};"
        else:
            line += " CZA: none predicted (e > 32.2);"
        if sub:
            line += " " + sub
        if pred is not None and edge is not None and pk >= 10.0:   # (x < 10: the sundog is dying out near 61 deg)
            ok &= abs(edge - pred) <= 0.8
        print(line)
    for e in (50.0, 60.0, 70.0, 80.0):   # the circumhorizon arc: below the Sun (phi = 180), e > 57.8 only
        P, _ = run_population("plates", e, nrays, rng)
        Pg = denoise(P[1])
        pred = cha_theta(N_ICE[1], e)
        t, val = peak_theta(Pg, slice(NP - 3, NP), 40.0, 62.0)
        if pred is None:
            flag = "ok" if val < 3.0 else "MISS (an arc where none should be)"
            print(f"  e={e:4.1f}: circumhorizon arc not expected (e < 57.8): strongest 40-62 deg below the Sun x{val:.1f} {flag}")
        else:
            edge, pk = inner_edge_theta(Pg, slice(NP - 3, NP), 40.0, 62.0)
            flag = "ok" if edge is not None and abs(edge - pred) <= 1.0 else ("near cutoff" if e < 62.0 else "MISS")
            ok &= flag != "MISS"
            print(f"  e={e:4.1f}: circumhorizon arc predicted theta {pred:5.2f} below the Sun, traced edge {edge} (x{pk:.0f}) {flag}")
    print("== columns (singly oriented): tangent arcs ==")
    for e in (0.0, 15.0, 30.0, 45.0):
        P, _ = run_population("columns", e, nrays, rng)
        Pg = denoise(P[1])
        up_t, up_v = inner_edge_theta(Pg, slice(0, 2), 18.0, 30.0)
        lo_t, lo_v = inner_edge_theta(Pg, slice(NP - 2, NP), 18.0, 30.0)
        side_t, side_v = peak_theta(Pg, slice(44, 47), 18.0, 40.0)
        sup_t, sup_v = peak_theta(Pg, slice(0, 2), 40.0, 60.0)
        print(f"  e={e:4.1f}: upper tangent inner edge {up_t} (x{up_v:.0f}), lower {lo_t} (x{lo_v:.0f}), side (phi 90) peak {side_t} x{side_v:.1f}, "
              f"above-Sun 40-60 peak {sup_t} x{sup_v:.1f} (supralateral ~46)")
    print("== parry ==")
    for e in (0.0, 15.0, 30.0):
        P, _ = run_population("parry", e, nrays, rng)
        Pg = denoise(P[1])
        t, v = peak_theta(Pg, slice(0, 2), 18.0, 40.0)
        print(f"  e={e:4.1f}: strongest above-Sun feature theta {t:5.1f} x{v:.0f} (the suncave Parry arc sits above the tangent arc)")
    print("AUDIT", "OK" if ok else "FAILED")
    return ok

def preview(table, path):
    try:
        from PIL import Image
    except ImportError:
        return
    layers = [8, 12, 16]   # e = 20, 30, 40
    tiles = []
    for L in layers:
        img = table[L, :, :, :3]
        tiles.append(np.repeat(img, 2, axis=0))
    sheet = np.concatenate(tiles, axis=1)
    Image.fromarray(sheet.astype(np.uint8)).save(path)

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--rays", type=int, default=2000000)
    ap.add_argument("--seed", type=int, default=7)
    ap.add_argument("--check", action="store_true")
    ap.add_argument("--out", default=os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))),
                                                  "assets", "textures", "ice_optics.rgba8"))
    a = ap.parse_args()
    if a.check:
        sys.exit(0 if audit(a.rays, a.seed) else 1)
    rng = np.random.default_rng(a.seed)
    t0 = time.time()
    table = np.full((NE, H, NT, 4), 255, dtype=np.uint8)
    # random populations: elevation-independent, 4x the rays (they are tiny)
    for k, pop in enumerate(("randomA", "randomB")):
        P, delta = run_population(pop, 20.0, 4 * a.rays, rng)
        for ch in range(3):
            v = encode(denoise(P[ch]))
            table[:, 2 * k, :, ch] = np.rint(v * 255.0)
            table[:, 2 * k + 1, :, ch] = table[:, 2 * k, :, ch]
        print(f"{pop}: undeviated {delta.mean():.3f}, peak {P.max():.1f}, {time.time() - t0:.0f} s")
    for pi, pop in enumerate(POPS):
        for li, e in enumerate(ELEVS):
            P, delta = run_population(pop, e, a.rays, rng)
            for ch in range(3):
                v = encode(denoise(P[ch]))
                table[li, ROW0 + pi * NP:ROW0 + (pi + 1) * NP, :, ch] = np.rint(v * 255.0).T
        print(f"{pop}: done, last undeviated {delta.mean():.3f}, peak {P.max():.1f}, {time.time() - t0:.0f} s")
    os.makedirs(os.path.dirname(a.out), exist_ok=True)
    with open(a.out, "wb") as f:
        f.write(b"SLIO")
        f.write(struct.pack("<IIIIfII", NT, H, NE, NP, PMAX, NRAND, NPOP))
        f.write(table.tobytes())
    print(f"wrote {a.out}: {NT} x {H} x {NE} RGBA8 = {table.nbytes / 1e6:.1f} MB in {time.time() - t0:.0f} s")
    out_dir = os.path.join(os.path.dirname(os.path.abspath(__file__)), "out")
    os.makedirs(out_dir, exist_ok=True)
    preview(table, os.path.join(out_dir, "ice_optics_preview.png"))

if __name__ == "__main__":
    main()
