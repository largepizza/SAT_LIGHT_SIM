#!/usr/bin/env python3
"""Solve the elements of the scripted Great Comet of 2037 against the sim's own ephemeris.

The sim's Earth is the JPL/Standish EM-barycentre row of kEarthElements (SatelliteSim.cpp), evaluated at
T = (seconds since J2000 / 86400) / 36525 — this script mirrors keplerEclipticPos exactly. The comet is a
parabola (Kreutz-like sungrazer: q ~ 0.006 AU, retrograde, i = 142 deg) built to pass a chosen point P beside
the Earth at a chosen moment on its OUTBOUND leg:

  P = E(t_enc) + d * n,   n perpendicular to the comet-Earth relative velocity, as close to ecliptic north as that
                          allows (so the comet passes over the northern hemisphere, circumpolar there)

Its plane holds the Sun and P, tilted to i; the true anomaly at P follows from r_P and q; Barker's equation gives
the perihelion time. The script prints the elements (paste into src/simulations/Comets.cpp, kGreatComet) and checks
the closest approach by brute force in the same frame.

  python tools/comet_design.py
"""
import math
from datetime import datetime, timezone

K_GAUSS = 0.01720209895          # rad/day, AU^1.5
MU = K_GAUSS ** 2                # AU^3/day^2
AU_KM = 1.495978707e8
J2000 = datetime(2000, 1, 1, 12, 0, 0, tzinfo=timezone.utc)


def tsec(s):
    return (datetime.fromisoformat(s).replace(tzinfo=timezone.utc) - J2000).total_seconds()


def earth_helio(t):
    """keplerEclipticPos(kEarthElements, T) — the sim's Earth (EM barycentre), J2000 ecliptic, AU."""
    T = t / 86400.0 / 36525.0
    a = 1.00000261 + 0.00000562 * T
    e = 0.01671123 - 0.00004392 * T
    i = math.radians(-0.00001531 - 0.01294668 * T)
    L = 100.46457166 + 35999.37244981 * T
    peri = 102.93768193 + 0.32327364 * T
    node = 0.0
    M = math.fmod(L - peri, 360.0)
    if M > 180: M -= 360
    if M < -180: M += 360
    M = math.radians(M)
    E = M
    for _ in range(8):
        E -= (E - e * math.sin(E) - M) / (1 - e * math.cos(E))
    xo, yo = a * (math.cos(E) - e), a * math.sqrt(1 - e * e) * math.sin(E)
    w, n = math.radians(peri - node), math.radians(node)
    cw, sw, cn, sn, ci, si = math.cos(w), math.sin(w), math.cos(n), math.sin(n), math.cos(i), math.sin(i)
    return ((cw * cn - sw * sn * ci) * xo + (-sw * cn - cw * sn * ci) * yo,
            (cw * sn + sw * cn * ci) * xo + (-sw * sn + cw * cn * ci) * yo,
            (sw * si) * xo + (cw * si) * yo)


def add(a, b): return tuple(x + y for x, y in zip(a, b))
def sub(a, b): return tuple(x - y for x, y in zip(a, b))
def mul(a, s): return tuple(x * s for x in a)
def dot(a, b): return sum(x * y for x, y in zip(a, b))
def cross(a, b): return (a[1] * b[2] - a[2] * b[1], a[2] * b[0] - a[0] * b[2], a[0] * b[1] - a[1] * b[0])
def norm(a): return math.sqrt(dot(a, a))
def unit(a): return mul(a, 1.0 / norm(a))


def rot(v, k, ang):
    """Rodrigues: v rotated by ang about unit k."""
    c, s = math.cos(ang), math.sin(ang)
    return add(add(mul(v, c), mul(cross(k, v), s)), mul(k, dot(k, v) * (1 - c)))


def parabola_state(q, phat, that, dt_days):
    """Position/velocity dt days after perihelion on a parabola (perihelion direction phat, motion that)."""
    W = 3.0 * math.sqrt(MU / (2 * q ** 3)) * dt_days
    # Barker: D^3 + 3D = W  (D = tan(nu/2))
    y = math.cbrt(W / 2 + math.sqrt(W * W / 4 + 1))
    D = y - 1 / y
    nu = 2 * math.atan(D)
    r = q * (1 + D * D)
    pos = add(mul(phat, r * math.cos(nu)), mul(that, r * math.sin(nu)))
    p = 2 * q
    vr = math.sqrt(MU / p) * math.sin(nu)
    vt = math.sqrt(MU / p) * (1 + math.cos(nu))
    vel = add(mul(phat, vr * math.cos(nu) - vt * math.sin(nu)), mul(that, vr * math.sin(nu) + vt * math.cos(nu)))
    return pos, vel, nu, r


def solve(t_enc_iso, q, inc_deg, d_km):
    t_enc = tsec(t_enc_iso)
    E = earth_helio(t_enc)
    dt = 60.0
    VE = mul(sub(earth_helio(t_enc + dt), earth_helio(t_enc - dt)), 86400.0 / (2 * dt))  # AU/day
    zhat = (0.0, 0.0, 1.0)
    n = zhat
    P = E
    for _ in range(6):
        P = add(E, mul(n, d_km / AU_KM))
        Phat = unit(P)
        zperp = unit(sub(zhat, mul(Phat, dot(zhat, Phat))))
        inc = math.radians(inc_deg)
        h = unit(add(mul(zperp, math.cos(inc)), mul(cross(Phat, zperp), math.sin(inc))))
        that_P = cross(h, Phat)
        rP = norm(P)
        nuP = math.acos(2 * q / rP - 1)
        phat = rot(Phat, h, -nuP)
        that = cross(h, phat)
        D = math.tan(nuP / 2)
        dt_days = math.sqrt(2 * q ** 3 / MU) * (D + D ** 3 / 3)
        _, vP, _, _ = parabola_state(q, phat, that, dt_days)
        vrel = unit(sub(vP, VE))
        n = unit(sub(zhat, mul(vrel, dot(zhat, vrel))))   # the offset across the relative motion, northward
    Tp = t_enc - dt_days * 86400.0
    # Elements in the J2000 ecliptic: node line = zhat x h; omega from the node to perihelion.
    node_vec = cross(zhat, h)
    if norm(node_vec) < 1e-12:
        node_vec = (1.0, 0.0, 0.0)
    node_vec = unit(node_vec)
    Om = math.atan2(node_vec[1], node_vec[0])
    om = math.atan2(dot(cross(node_vec, phat), h), dot(node_vec, phat))
    i = math.acos(max(-1.0, min(1.0, h[2])))
    return dict(Tp=Tp, q=q, i=math.degrees(i), Om=math.degrees(Om) % 360, om=math.degrees(om) % 360,
                phat=phat, that=that, h=h, t_enc=t_enc, dt_days=dt_days)


def elements_frame(q, i_deg, Om_deg, om_deg):
    i, Om, om = map(math.radians, (i_deg, Om_deg, om_deg))
    cO, sO, ci, si, cw, sw = math.cos(Om), math.sin(Om), math.cos(i), math.sin(i), math.cos(om), math.sin(om)
    phat = (cO * cw - sO * sw * ci, sO * cw + cO * sw * ci, sw * si)
    that = (-cO * sw - sO * cw * ci, -sO * sw + cO * cw * ci, cw * si)
    return phat, that


def check(el, label):
    phat, that = elements_frame(el['q'], el['i'], el['Om'], el['om'])
    best = (1e9, 0)
    t0 = el['t_enc'] - 3 * 86400
    for k in range(0, 6 * 86400, 60):
        t = t0 + k
        pos, _, _, _ = parabola_state(el['q'], phat, that, (t - el['Tp']) / 86400.0)
        d = norm(sub(pos, earth_helio(t))) * AU_KM
        if d < best[0]:
            best = (d, t)
    dmin, tmin = best
    print(f"{label}: closest approach {dmin:,.0f} km at {datetime.fromtimestamp(tmin + J2000.timestamp(), timezone.utc)}")
    return dmin, tmin


if __name__ == '__main__':
    el = solve('2037-03-17T03:30:00', q=0.0062, inc_deg=142.0, d_km=560000.0)
    Tp_iso = datetime.fromtimestamp(el['Tp'] + J2000.timestamp(), timezone.utc)
    print(f"perihelion {Tp_iso}  (t = {el['Tp']:.1f} s since J2000), {el['dt_days']:.3f} d before the encounter")
    print(f"q {el['q']} AU  i {el['i']:.4f}  Omega {el['Om']:.4f}  omega {el['om']:.4f}")
    check(el, 'great comet')
    # Elongation / distance timeline (geocentric, ecliptic), daily
    phat, that = elements_frame(el['q'], el['i'], el['Om'], el['om'])
    for dd in [-30, -10, -3, -1, -0.25, 0, 0.25, 1, 2, 3, 5, 10, 15, 20, 24, 26, 27, 27.3, 27.6, 28, 29, 31, 35, 45, 60]:
        t = el['Tp'] + dd * 86400
        pos, _, _, r = parabola_state(el['q'], phat, that, dd)
        E = earth_helio(t)
        g = sub(pos, E)
        delta = norm(g)
        elong = math.degrees(math.acos(dot(unit(g), unit(mul(E, -1)))))
        phase = math.degrees(math.acos(dot(unit(mul(pos, -1)), unit(mul(g, -1)))))
        beta = math.degrees(math.asin(unit(g)[2]))
        print(f"  T{dd:+7.2f} d  r {r:7.4f} AU  Delta {delta:7.4f} AU ({delta * AU_KM:12,.0f} km)  elong {elong:6.1f}"
              f"  phase {phase:6.1f}  ecl.lat {beta:+6.1f}")
