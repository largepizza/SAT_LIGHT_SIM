#!/usr/bin/env python3
"""How Reflect Orbital's quad sail shapes the reflected beam (2026-11-04).

Compares a flat single 18 m square mirror, the four coplanar scalloped quadrants (the model,
tools/make_reflector_model.py), the quadrants canted toward the centre (a shallow dish) by a few tenths of a degree to
a degree, and quadrants billowed (each a shallow concave paraboloid). Physical optics at the scale that matters:

  * Each film patch reflects the Sun's limb-darkened disc (angular radius 4.65 mrad, u = 0.6), blurred by the film's
    microfacet roughness (GGX alpha 0.0005, the model's 'mirror' preset; the reflected ray turns by 2x the normal).
  * A patch whose normal is tilted by t moves its image by 2t (small angles), so on the ground at range R the patch's
    spot is displaced by 2tR. The ground irradiance is the sum of the patches' displaced Sun images, each
    rho * A_patch * cos(i) * L_sun / R^2 (normal to the beam).
  * Diffraction: lambda / D = 550 nm / 18 m = 3e-8 rad (1.5 cm at 500 km) against the Sun's 9.3 mrad: the scalloped
    edges change the AREA, never the spot shape. The outline itself smears the spot by its own size (18 m in ~4.6 km).

    python tools/reflector_quad_analysis.py [--range-km 500] [--plot out.png]
Prints the tables quoted in docs/subsystems/satellites.md ("Reflect Orbital quad sail").
"""
import argparse
import math
import os
import sys

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import make_reflector_model as mrm  # noqa: E402

SUN_R = 4.65e-3          # rad, the Sun's angular radius
LIMB_U = 0.6             # linear limb darkening
E_SUN = 1361.0           # W/m2 above the atmosphere
LUX_SUN = 1.27e5         # lux above the atmosphere (photopic)
M_SUN = -26.74
RHO = 0.95               # the mirror preset's F0
ALPHA = 0.0005           # GGX alpha of the film
FULL_MOON_LUX = 0.25

STEP = 0.05e-3           # rad per grid cell
HALF = 42e-3             # grid half-width, rad


def grid():
    n = int(round(2 * HALF / STEP))
    ax = (np.arange(n) - n // 2) * STEP
    return ax, np.meshgrid(ax, ax, indexing="xy")


def sun_kernel(X, Y):
    """Sun radiance pattern in reflected-angle space, normalised to integrate (sum x STEP^2) to 1 (i.e. / E_sun)."""
    r = np.hypot(X, Y) / SUN_R
    mu = np.sqrt(np.clip(1.0 - r * r, 0.0, 1.0))
    L = np.where(r < 1.0, 1.0 - LIMB_U * (1.0 - mu), 0.0)
    # Film roughness: GGX normal distribution, reflected angle = 2 x normal angle.
    t = np.hypot(X, Y) / 2.0
    a2 = ALPHA * ALPHA
    D = a2 / (math.pi * (a2 + t * t) ** 2)  # GGX, small angles (cos ~ 1, tan ~ angle)
    D = D / D.sum()
    L = np.real(np.fft.ifft2(np.fft.fft2(np.fft.ifftshift(L)) * np.fft.fft2(np.fft.ifftshift(D))))
    L = np.fft.fftshift(L)
    return L / (L.sum() * STEP * STEP)


def point_in_poly(x, y, pts):
    inside = False
    j = len(pts) - 1
    for i in range(len(pts)):
        xi, yi = pts[i]
        xj, yj = pts[j]
        if (yi > y) != (yj > y) and x < (xj - xi) * (y - yi) / (yj - yi) + xi:
            inside = not inside
        j = i
    return inside


def facet_patches(kind="q", cant_deg=0.0, sag_m=0.0, flat_square=False, n=24):
    """(area_m2, tilt_x, tilt_y) of the film patches. tilt = the normal's lean (rad) in body X / Y."""
    out = []
    if flat_square:
        out.append((18.0 * 18.0, 0.0, 0.0))
        return out
    base = mrm.quadrant_outline()
    area_q = mrm.polygon_area(base)
    th = math.radians(cant_deg)
    for k in range(4):
        u = mrm.rot2((math.sqrt(0.5), math.sqrt(0.5)), k)
        if sag_m <= 0.0:
            out.append((area_q, -u[0] * th, -u[1] * th))
            continue
        # A billowed quadrant: a concave paraboloid over the quadrant's inscribed width w, depth sag_m at the middle.
        # Sample its slope over the quadrant's area (points inside the outline).
        pts = np.array([mrm.rot2(p, k) for p in base])
        lo, hi = pts.min(0), pts.max(0)
        xs = np.linspace(lo[0], hi[0], n)
        ys = np.linspace(lo[1], hi[1], n)
        cx, cy = pts.mean(0)
        w = 0.5 * (hi[0] - lo[0] + hi[1] - lo[1]) * 0.5  # half-width scale
        inside = [(x, y) for x in xs for y in ys if point_in_poly(x, y, pts)]
        a = area_q / len(inside)
        for x, y in inside:
            # z = sag * (r/w)^2 (concave toward +Z): slope = 2 sag r / w^2; the normal leans toward the middle.
            sx, sy = 2.0 * sag_m * (x - cx) / (w * w), 2.0 * sag_m * (y - cy) / (w * w)
            out.append((a, -sx - u[0] * th, -sy - u[1] * th))
    return out


def irradiance_map(patches, K):
    """Ground irradiance pattern in angle space (W/m2 at R = 1 m: divide by R^2), sun image per patch."""
    n = K.shape[0]
    H = np.zeros_like(K)
    for a, tx, ty in patches:
        # The reflected beam turns by 2 x the tilt; bilinear splat.
        fx, fy = 2.0 * tx / STEP + n // 2, 2.0 * ty / STEP + n // 2
        ix, iy = int(math.floor(fx)), int(math.floor(fy))
        wx, wy = fx - ix, fy - iy
        for dx, dy, w in ((0, 0, (1 - wx) * (1 - wy)), (1, 0, wx * (1 - wy)), (0, 1, (1 - wx) * wy), (1, 1, wx * wy)):
            H[iy + dy, ix + dx] += a * w
    E = np.real(np.fft.ifft2(np.fft.fft2(np.fft.ifftshift(H)) * np.fft.fft2(np.fft.ifftshift(K))))
    return np.fft.fftshift(E) * RHO * E_SUN  # x 1/R^2 (cos i = 1: normal incidence on the mirror)


def mag_of(E):
    return M_SUN - 2.5 * math.log10(max(E, 1e-30) / E_SUN)


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("--range-km", type=float, default=500.0)
    ap.add_argument("--plot", default="")
    a = ap.parse_args()
    R = a.range_km * 1e3
    ax, (X, Y) = grid()
    K = sun_kernel(X, Y)
    n = K.shape[0]
    c = n // 2

    cases = [
        ("flat 18 m square (324 m2)", dict(flat_square=True)),
        ("4 coplanar quadrants", dict(kind="q")),
        ("cant 0.10 deg", dict(kind="q", cant_deg=0.10)),
        ("cant 0.25 deg", dict(kind="q", cant_deg=0.25)),
        ("cant 0.50 deg", dict(kind="q", cant_deg=0.50)),
        ("cant 1.00 deg", dict(kind="q", cant_deg=1.00)),
        ("billow 2 cm", dict(kind="q", sag_m=0.02)),
        ("billow 5 cm", dict(kind="q", sag_m=0.05)),
    ]
    print("range %.0f km, normal incidence; the Sun's image alone is %.2f km across (2 x 4.65 mrad x R)"
          % (a.range_km, 2 * SUN_R * R / 1e3))
    print("%-28s %7s %10s %8s %8s %9s %9s %8s %10s" % (
        "case", "film m2", "peak mW/m2", "peak lx", "x moon", "mag@peak", "mag@aim", "FWHM km", "sub-spot km"))
    profiles = {}
    for name, kw in cases:
        P = facet_patches(**kw)
        A = sum(p[0] for p in P)
        E = irradiance_map(P, K) / (R * R)
        peak = E.max()
        aim = E[c, c]
        half = (E >= 0.5 * peak).sum() * (STEP * R) ** 2
        fwhm = 2.0 * math.sqrt(half / math.pi) / 1e3  # equivalent-circle diameter of the half-peak area
        th = math.radians(kw.get("cant_deg", 0.0))
        sub = 2.0 * th * R / 1e3
        lux = peak / E_SUN * LUX_SUN
        print("%-28s %7.1f %10.2f %8.2f %8.1f %9.2f %9.2f %8.2f %10.2f" % (
            name, A, peak * 1e3, lux, lux / FULL_MOON_LUX, mag_of(peak), mag_of(aim), fwhm, sub))
        profiles[name] = (E[c, :].copy(), E[np.arange(n), np.arange(n)].copy())

    # A sweep across the spot (the light curve as the spot crosses an observer): magnitude vs ground offset, along a
    # boom (body X) and along a quadrant bisector (the diagonal, where canted sub-spots sit).
    offs_km = [0, 1, 2, 2.5, 3, 4, 5, 6, 8, 10]
    print("\nmagnitude vs ground offset from the aim point (km): along a boom | along the diagonal")
    print("%-28s " % "case" + " ".join("%6.1f" % o for o in offs_km))
    for name, (row, diag) in profiles.items():
        def at(arr, km, diagonal):
            step_km = STEP * R / 1e3 * (math.sqrt(2.0) if diagonal else 1.0)
            i = c + int(round(km / step_km))
            return mag_of(arr[i]) if 0 <= i < len(arr) and arr[i] > 1e-12 else float("nan")
        print("%-28s " % name + " ".join("%6.2f" % at(row, o, False) for o in offs_km) + "  |  " +
              " ".join("%6.2f" % at(diag, o, True) for o in offs_km))

    # A pass seen from the aim point (the mirror holds its spot there): 500 km circular orbit overhead, the Sun 15 deg
    # below the observer's horizon in the orbit plane, behind the observer as the satellite rises.
    print("\npass over the aim point (h %.0f km, Sun 15 deg below the horizon, opposite the rising satellite):"
          % a.range_km)
    print("%6s %8s %7s %8s %8s %8s %8s" % ("elev", "range km", "inc deg", "coplanar", "flat18", "cant.25", "cant.5"))
    h = a.range_km * 1e3
    Re = 6371e3
    sun = np.array([math.cos(math.radians(-15.0)) * -1.0, math.sin(math.radians(-15.0))])  # toward the Sun (x, up)
    fr = {}
    for name, kw in cases:
        P = facet_patches(**kw)
        E = irradiance_map(P, K)
        fr[name] = E[c, c]  # x 1/R^2 at normal incidence
    for el in (90, 70, 50, 40, 30, 20, 15):
        e = math.radians(el)
        rng = -Re * math.sin(e) + math.sqrt((Re * math.sin(e)) ** 2 + h * h + 2 * Re * h)
        to_obs = -np.array([math.cos(e), math.sin(e)])  # satellite -> observer (2D, x toward the satellite)
        cos2i = float(np.dot(to_obs, sun))
        inc = 0.5 * math.degrees(math.acos(max(-1.0, min(1.0, cos2i))))
        ci = math.cos(math.radians(inc))
        row = []
        for name in ("4 coplanar quadrants", "flat 18 m square (324 m2)", "cant 0.25 deg", "cant 0.50 deg"):
            row.append(mag_of(fr[name] * ci / (rng * rng)))
        print("%6.0f %8.0f %7.1f %8.2f %8.2f %8.2f %8.2f" % (el, rng / 1e3, inc, *row))

    if a.plot:
        import matplotlib
        matplotlib.use("Agg")
        import matplotlib.pyplot as plt
        sel = ["4 coplanar quadrants", "cant 0.10 deg", "cant 0.25 deg", "cant 0.50 deg", "billow 5 cm"]
        fig, axs = plt.subplots(1, len(sel), figsize=(4 * len(sel), 4))
        ext = [ax[0] * R / 1e3, ax[-1] * R / 1e3] * 2
        for axp, name in zip(axs, sel):
            kw = dict(cases)[name]
            E = irradiance_map(facet_patches(**kw), K) / (R * R)
            axp.imshow(E * 1e3, extent=ext, origin="lower", cmap="inferno")
            axp.set_xlim(-12, 12)
            axp.set_ylim(-12, 12)
            axp.set_title("%s\npeak %.1f mW/m2" % (name, E.max() * 1e3))
            axp.set_xlabel("km (boom X)")
        fig.tight_layout()
        fig.savefig(a.plot, dpi=90)
        print("plot:", a.plot)


if __name__ == "__main__":
    main()
