"""Replaying a real satellite pass through the sim's photometry.

    spec = pass_spec(tle, observer, t0, t1, step_s=0.5, model=".../starlink_visorsat.json")
    out = run_pass(spec, work_dir)          # SatModelTool --pass: the CPU mirror of the GPU evaluator

States come from SGP4 (the `sgp4` package) in TEME, which is the sim's ECI convention (a rotation of ECEF about z
by GMST: SatPhotometry.cpp earthRotationAngle). The observer is placed with WGS84, not the sim's sphere, so the
geometry is the real one; this can differ from the app's own observer by a fraction of a degree.
"""
from __future__ import annotations

import json
import math
import subprocess
from datetime import datetime, timedelta, timezone
from pathlib import Path

import numpy as np

from . import env

J2000 = datetime(2000, 1, 1, 12, 0, 0, tzinfo=timezone.utc)
OMEGA = 7.29211585531e-5
GMST_J2000 = math.radians(280.46061837)


def parse_utc(s) -> datetime:
    if isinstance(s, datetime):
        return s if s.tzinfo else s.replace(tzinfo=timezone.utc)
    import re
    s = str(s).replace("Z", "+00:00")
    # Python 3.10's fromisoformat wants 0, 3 or 6 fraction digits: pad / cut to 6.
    s = re.sub(r"\.(\d+)", lambda m: "." + (m.group(1) + "000000")[:6], s)
    d = datetime.fromisoformat(s)
    return (d if d.tzinfo else d.replace(tzinfo=timezone.utc)).astimezone(timezone.utc)


def t_j2000(dt) -> float:
    return (parse_utc(dt) - J2000).total_seconds()


def era(t: float) -> float:
    """SatPhotometry.cpp earthRotationAngle(), same split."""
    days = math.floor(t / 86400.0)
    sec = t - days * 86400.0
    turns = math.fmod(days * (0.98564736629 / 360.0), 1.0)
    return math.fmod(GMST_J2000 + turns * 2 * math.pi + OMEGA * sec, 2 * math.pi)


def geodetic_to_ecef(lat_deg, lon_deg, alt_m):
    a, f = 6378137.0, 1 / 298.257223563
    e2 = f * (2 - f)
    la, lo = math.radians(lat_deg), math.radians(lon_deg)
    n = a / math.sqrt(1 - e2 * math.sin(la) ** 2)
    return np.array([(n + alt_m) * math.cos(la) * math.cos(lo), (n + alt_m) * math.cos(la) * math.sin(lo),
                     (n * (1 - e2) + alt_m) * math.sin(la)])


def ecef_to_eci(p, t):
    th = era(t)
    c, s = math.cos(th), math.sin(th)
    return np.array([c * p[0] - s * p[1], s * p[0] + c * p[1], p[2]])


def pass_spec(tle: list[str], observer: dict, t0, t1, step_s: float = 0.5, model: str = "", budget: int = 256,
              occlusion: bool = True, extinction_k: float = 0.18, flare_tilt_deg: float = 0.0,
              tle_offset_s: float = 0.0) -> dict:
    """tle_offset_s: propagate the TLE at observation time + this (an old TLE's along-track timing error)."""
    from sgp4.api import Satrec, jday

    sat = Satrec.twoline2rv(tle[0], tle[1])
    obs_ecef = geodetic_to_ecef(observer["lat_deg"], observer["lon_deg"], observer.get("alt_m", 0.0))
    a, b = parse_utc(t0), parse_utc(t1)
    n = int((b - a).total_seconds() / step_s) + 1
    samples = []
    for i in range(n):
        dt = a + timedelta(seconds=i * step_s)
        dp = dt + timedelta(seconds=tle_offset_s)
        jd, fr = jday(dp.year, dp.month, dp.day, dp.hour, dp.minute, dp.second + dp.microsecond * 1e-6)
        err, r, v = sat.sgp4(jd, fr)
        if err:
            raise RuntimeError(f"sgp4 error {err} at {dt}")
        t = t_j2000(dt)
        samples.append({"t_j2000": t, "utc": dt.isoformat().replace("+00:00", "Z"),
                        "pos_eci_m": [x * 1e3 for x in r], "vel_eci_mps": [x * 1e3 for x in v],
                        "obs_eci_m": ecef_to_eci(obs_ecef, t).tolist()})
    return {"model": model, "budget": budget, "occlusion": occlusion, "extinction_k": extinction_k,
            "flare_tilt_deg": flare_tilt_deg, "observer": observer, "tle": tle, "tle_offset_s": tle_offset_s,
            "samples": samples}


def sat_model_tool() -> Path:
    for p in (env.REPO / "build" / "Release" / "SatModelTool.exe", env.REPO / "build" / "Release" / "SatModelTool",
              env.REPO / "build" / "SatModelTool"):
        if p.exists():
            return p
    raise SystemExit("SatModelTool not built: cmake --build build --config Release --target SatModelTool")


def run_pass(spec: dict, work: Path, name: str = "pass") -> dict:
    work = Path(work)
    work.mkdir(parents=True, exist_ok=True)
    sp, op = work / f"{name}.spec.json", work / f"{name}.out.json"
    sp.write_text(json.dumps(spec), encoding="utf-8")
    subprocess.run([str(sat_model_tool()), "--pass", str(sp), "--pass-out", str(op)], check=True)
    out = json.loads(op.read_text(encoding="utf-8"))
    for s_in, s_out in zip(spec["samples"], out["samples"]):
        s_out["utc"] = s_in["utc"]
    return out


def az_el(spec_sample: dict) -> tuple[float, float]:
    """Topocentric azimuth / elevation (deg) of the satellite for a spec sample (WGS84-free: local vertical = the
    observer's geocentric direction, good to ~0.2 deg)."""
    p = np.array(spec_sample["pos_eci_m"])
    o = np.array(spec_sample["obs_eci_m"])
    up = o / np.linalg.norm(o)
    east = np.cross([0, 0, 1], up)
    east /= np.linalg.norm(east)
    north = np.cross(up, east)
    d = p - o
    d /= np.linalg.norm(d)
    return (math.degrees(math.atan2(d @ east, d @ north)) % 360, math.degrees(math.asin(d @ up)))


# ── Putting the satellite in the app ─────────────────────────────────────────────────────────────────────────
SIM_R = 6371000.0          # SatelliteSim kEarthRadius (a sphere)
SIM_GM = 3.986004418e14


def _teme(tle, dt):
    from sgp4.api import Satrec, jday

    sat = Satrec.twoline2rv(tle[0], tle[1])
    jd, fr = jday(dt.year, dt.month, dt.day, dt.hour, dt.minute, dt.second + dt.microsecond * 1e-6)
    err, r, v = sat.sgp4(jd, fr)
    if err:
        raise RuntimeError(f"sgp4 error {err}")
    return np.array(r) * 1e3, np.array(v) * 1e3


def _arg_lat(r, v):
    h = np.cross(r, v)
    h /= np.linalg.norm(h)
    node = np.cross([0, 0, 1], h)
    node /= np.linalg.norm(node)
    rn = r / np.linalg.norm(r)
    return math.atan2(np.cross(node, rn) @ h, node @ rn), h, node


def circular_member(tle, t_center, name="replica", tle_offset_s: float = 0.0) -> dict:
    """A constellations.json List member flying the circular orbit the app can represent, fitted to the TLE at
    t_center: its plane, its argument of latitude at t_center and its angular RATE there (the radius is chosen so
    the sim's Kepler mean motion equals it: along-track timing matters more than a few km of height)."""
    t_obs = parse_utc(t_center)
    tc = t_obs + timedelta(seconds=tle_offset_s)  # where the TLE puts the satellite at t_obs
    r, v = _teme(tle, tc)
    u_c, h, node = _arg_lat(r, v)
    ua, _, _ = _arg_lat(*_teme(tle, tc - timedelta(seconds=60)))
    ub, _, _ = _arg_lat(*_teme(tle, tc + timedelta(seconds=60)))
    n_true = ((ub - ua + math.pi) % (2 * math.pi) - math.pi) / 120.0
    rad = (SIM_GM / n_true ** 2) ** (1 / 3)
    alt = float(np.float32(rad - SIM_R))
    n_sim = float(np.float32(math.sqrt(SIM_GM / (SIM_R + alt) ** 3)))  # what the app will use
    incl = math.degrees(math.acos(h[2]))
    raan = math.degrees(math.atan2(node[1], node[0])) % 360
    u0 = (u_c - n_sim * t_j2000(t_obs)) % (2 * math.pi)
    return {"name": name, "alt_km": alt / 1000.0, "incl_deg": incl, "raan_deg": raan, "u0_deg": math.degrees(u0),
            "_fit": {"t_center": tc.isoformat(), "radius_true_km": float(np.linalg.norm(r)) / 1e3,
                     "n_true": n_true, "n_sim": n_sim}}


def circular_states(member: dict, t0, t1, step_s: float):
    """The app's circular orbit for a List member (SatelliteSim buildOrbits + satEciAt), sampled."""
    a, b = parse_utc(t0), parse_utc(t1)
    rad = SIM_R + member["alt_km"] * 1000.0
    n = float(np.float32(math.sqrt(SIM_GM / rad ** 3)))
    i, O, u0 = (math.radians(member[k]) for k in ("incl_deg", "raan_deg", "u0_deg"))
    out = []
    for k in range(int((b - a).total_seconds() / step_s) + 1):
        dt = a + timedelta(seconds=k * step_s)
        u = u0 + n * t_j2000(dt)
        cu, su, ci, si, cO, sO = math.cos(u), math.sin(u), math.cos(i), math.sin(i), math.cos(O), math.sin(O)
        p = rad * np.array([cu * cO - su * ci * sO, cu * sO + su * ci * cO, su * si])
        vel = rad * n * np.array([-su * cO - cu * ci * sO, -su * sO + cu * ci * cO, cu * si])
        out.append((dt, p, vel))
    return out


def extra_roster(member: dict, type_name: str, const_name: str = "Replica pass") -> dict:
    """The file SATLIGHTSIM_EXTRA_CONSTELLATIONS points at: one List entry holding the member."""
    m = {k: v for k, v in member.items() if not k.startswith("_")}
    return {"constellations": [{"name": const_name, "distribution": "List", "type": type_name, "enabled": True,
                                "category": "Benchmarks", "members": [m]}]}
