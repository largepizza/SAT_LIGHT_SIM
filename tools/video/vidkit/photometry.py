"""The lobe reflectance model in numpy, for animation (any Sun, any observer, per-lobe contributions).

A line-for-line port of SatModel.cpp's lobeIntensity() — the formula sat_orbit.comp evaluates on the GPU —
vectorised over observer directions. `check_against_dump()` compares it with the C++ results stored in a
SatModelTool --dump-json file; keep the two in step.

Units: radiant intensity per unit solar irradiance (m²/sr): I / r² is the satellite's flux as a fraction of
the Sun's. Magnitudes: m = -26.74 - 2.5 log10(I / r²) (the Sun's V magnitude), at r = 1000 km for m1000.
"""
from __future__ import annotations

import numpy as np

SUN_ALPHA = 0.0023  # the Sun's angular radius as a GGX alpha (SatModel.h kSunAlpha)
SUN_V = -26.74


def lobe_intensity(n, area, diff_area, albedo, f0, a2, beckmann, transmission, s, O):
    """One lobe. n, s: (3,) unit vectors; O: (..., 3) observer directions. a2 includes the source size."""
    n = np.asarray(n, float)
    s = np.asarray(s, float)
    O = np.asarray(O, float)
    ns = float(n @ s)
    no = O @ n
    out = np.zeros(no.shape)
    vis = no > 0
    if ns <= 0:
        if transmission > 0:
            out[vis] = transmission / np.pi * diff_area * (-ns) * no[vis]
        return out
    diffuse = albedo / np.pi * diff_area * ns * no
    hv = s + O
    hl = np.linalg.norm(hv, axis=-1)
    h = hv / np.maximum(hl, 1e-12)[..., None]
    nh = h @ n
    sin2 = np.clip(1 - nh * nh, 0, None)
    sh = np.clip(h @ s, 0, None)
    F = f0 + (1 - f0) * (1 - sh) ** 5
    if beckmann:
        nh_ = np.maximum(nh, 1e-9)
        nh2 = nh_ * nh_
        D = np.exp(-sin2 / (nh2 * a2)) / (np.pi * a2 * nh2 * nh2)
        D = np.where(nh > 0, D, 0.0)
        alpha = np.sqrt(a2)

        def G1(x):
            c = x / (alpha * np.sqrt(np.maximum(1e-30, 1 - x * x)))
            return np.where(c >= 1.6, 1.0, (3.535 * c + 2.181 * c * c) / (1 + 2.276 * c + 2.577 * c * c))
    else:
        den = nh * nh * a2 + sin2
        D = a2 / (np.pi * den * den)

        def G1(x):
            return 2 * x / (x + np.sqrt(a2 + (1 - a2) * x * x))
    spec = area * D * F * G1(ns) * G1(np.maximum(no, 0)) / 4
    spec = np.where(hl < 1e-12, 0.0, spec)
    out = np.where(vis, diffuse + spec, 0.0)
    return out


def lobe_terms(lobe: dict, s, O, src_a2=SUN_ALPHA ** 2):
    """(diffuse, specular) of one lobe from a dump's lobe dict, split for explaining the formula."""
    n = np.asarray(lobe["n"], float)
    a2 = lobe["alpha"] ** 2 + src_a2
    full = lobe_intensity(n, lobe["area"], lobe["diff_area"], lobe["albedo"], lobe["f0"], a2, lobe["beckmann"],
                          lobe.get("transmission", 0.0), s, O)
    diff = lobe_intensity(n, 0.0, lobe["diff_area"], lobe["albedo"], 0.0, a2, lobe["beckmann"],
                          lobe.get("transmission", 0.0), s, O)
    return diff, full - diff


def model_intensity(lobes: list[dict], s, O, src_a2=SUN_ALPHA ** 2, per_lobe=False):
    """Sum over a level's lobes (dump format). per_lobe=True also returns (L, ...) contributions."""
    O = np.asarray(O, float)
    parts = []
    for L in lobes:
        a2 = L["alpha"] ** 2 + src_a2
        parts.append(lobe_intensity(L["n"], L["area"], L["diff_area"], L["albedo"], L["f0"], a2, L["beckmann"],
                                    L.get("transmission", 0.0), s, O))
    parts = np.array(parts) if parts else np.zeros((0,) + O.shape[:-1])
    tot = parts.sum(0)
    return (tot, parts) if per_lobe else tot


def latlon_grid(n_lat=36, n_lon=72):
    """Observer directions on the dump's grid: rows lat -90..90, cols lon 0..360, cell centred."""
    lat = np.radians(-90 + (np.arange(n_lat) + 0.5) * 180 / n_lat)
    lon = np.radians((np.arange(n_lon) + 0.5) * 360 / n_lon)
    LA, LO = np.meshgrid(lat, lon, indexing="ij")
    return np.stack([np.cos(LA) * np.cos(LO), np.cos(LA) * np.sin(LO), np.sin(LA)], -1)


def mag(I, range_m=1.0e6):
    """Apparent magnitude of intensity I (m²/sr per unit irradiance) at range_m."""
    return SUN_V - 2.5 * np.log10(np.maximum(np.asarray(I, float), 1e-30) / range_m ** 2)


def check_against_dump(d) -> float:
    """Largest relative difference between this port and the C++ sphere of every level of a LobeDump."""
    O = latlon_grid(*d.sphere_brute.shape)
    worst = 0.0
    for k, lv in enumerate(d.levels):
        I = model_intensity(lv.raw, d.sphere_sun, O)
        ref = d.sphere_lobes[k]
        rel = np.abs(I - ref) / np.maximum(ref, 1e-6 * ref.max())
        worst = max(worst, float(rel.max()))
    return worst
