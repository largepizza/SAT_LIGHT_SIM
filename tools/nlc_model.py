#!/usr/bin/env python3
"""Launch-made mesospheric ice over a launch site: how often physics allows it (2026-11-15, .plans/PLUME_OVERHAUL_PLAN.md
phase 3, issue_20261115_165612). The engine's layer uses the same equations (src/simulations/Nlc.h is the
authority: its climatology, 26 N: 180 K summer, 198 K winter, are this script's defaults); this prints the frequency the
model predicts so it is an OUTPUT, not a tuned constant (the user's decision: physical; "the launch rate is the physics").

Ice exists where the air's temperature T is below the FROST POINT of its water vapour, which rises with the vapour:
    p_ice(T) = exp(9.550426 - 5723.265 / T + 3.53068 ln T - 0.00728332 T) Pa      (Murphy & Koop 2005, ice)
    frost point: p_ice(T_f) = x_H2O * p_air
Water vapour at ~83 km = background (~5 ppmv) + every launch's exhaust water still there:
  - deposited by the upper stage between ~80 and ~115 km (the shuttle put ~half its exhaust water at 103-114 km:
    Stevens et al. 2003, GRL; it reached the Arctic in ~1 day and made polar summer clouds);
  - destroyed by Lyman-alpha photolysis, lifetime ~3 days at 80-85 km (Nicolet 1981: J ~3.5e-6 /s);
  - carried off by the mesospheric wind (~20-40 m/s) and spread (a regional patch's residence time ~R / U).
Temperature at 83 km: a seasonal climatology for the site's latitude + the diurnal tide + gravity waves:
  - mid-latitude summer mesopause ~167 K at 84 km (Fort Collins 41 N sodium lidar, Schmidt & Reising 2008);
    the low-latitude values below are ASSUMPTIONS to pin (marked);
  - the diurnal tide reaches ~25 K at low latitudes (monthly zonal means; review in the atomic oxygen SABER paper);
  - gravity waves: a few K to ~10 K rms.
Fresh exhaust is a separate, transient path: nearly pure vapour raises the local frost point to ~200 K (the 2018
"Super Soaker" release at ~85 km made ice within 18 s in 197-232 K air: Collins et al. 2021, JGR Space Physics),
lasting minutes until it dilutes.

    python tools/nlc_model.py                       # the defaults: 30 Starships a day at Starbase (26 N)
    python tools/nlc_model.py --launches 60 --water-t 500 --lat 26
Standard library only.
"""
import argparse, math

R_AIR = 287.05

def p_ice(T):
    return math.exp(9.550426 - 5723.265 / T + 3.53068 * math.log(T) - 0.00728332 * T)

def frost_point(x, p_air):
    pw = x * p_air
    lo, hi = 100.0, 260.0
    for _ in range(60):
        m = 0.5 * (lo + hi)
        if p_ice(m) > pw: hi = m
        else: lo = m
    return 0.5 * (lo + hi)

def phi(z):
    return 0.5 * (1.0 + math.erf(z / math.sqrt(2.0)))

def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--launches', type=float, default=30.0, help='launches a day from the site')
    ap.add_argument('--water-t', type=float, default=500.0, help='exhaust water per launch into 80-115 km (t)')
    ap.add_argument('--lat', type=float, default=26.0)
    ap.add_argument('--h-km', type=float, default=83.0)
    ap.add_argument('--p-pa', type=float, default=0.55, help='air pressure at that height (US std 83 km ~0.55 Pa)')
    ap.add_argument('--rho', type=float, default=1.1e-5, help='air density at that height (kg/m^3)')
    ap.add_argument('--bg-ppmv', type=float, default=5.0)
    ap.add_argument('--life-d', type=float, default=3.0, help='H2O photolysis lifetime (days)')
    ap.add_argument('--wind', type=float, default=25.0, help='mean wind carrying the patch off (m/s)')
    ap.add_argument('--region-km', type=float, default=1000.0, help="the site's region (km across)")
    ap.add_argument('--layer-km', type=float, default=10.0, help="the vapour layer's thickness (km)")
    ap.add_argument('--band-deg', type=float, default=30.0, help='the latitude band the water spreads round the globe in')
    ap.add_argument('--t-summer', type=float, default=180.0, help='83 km mean, local summer (K) [Nlc.h tClimK at 26 N]')
    ap.add_argument('--t-winter', type=float, default=198.0, help='83 km mean, local winter (K) [Nlc.h tClimK at 26 N]')
    ap.add_argument('--tide', type=float, default=15.0, help='diurnal tide amplitude (K)')
    ap.add_argument('--gw', type=float, default=8.0, help='gravity waves, rms (K)')
    a = ap.parse_args()

    mw_per_day = a.launches * a.water_t * 1e3                    # kg of water a day
    # (1) the regional patch downwind of the site: resident ~ the time the wind takes to cross the region
    t_res = min(a.region_km * 1e3 / a.wind, a.life_d * 86400.0)
    air_region = (a.region_km * 1e3) ** 2 * a.layer_km * 1e3 * a.rho
    x_region = mw_per_day / 86400.0 * t_res / air_region * (28.97 / 18.015)
    # (2) the band round the globe it ends in (until photolysis): resident = rate x lifetime
    R = 6.371e6
    lat0 = math.radians(max(a.lat - a.band_deg / 2, -90)); lat1 = math.radians(min(a.lat + a.band_deg / 2, 90))
    area_band = 2 * math.pi * R * R * (math.sin(lat1) - math.sin(lat0))
    x_band = mw_per_day * a.life_d / (area_band * a.layer_km * 1e3 * a.rho) * (28.97 / 18.015)
    x_bg = a.bg_ppmv * 1e-6
    print(f"water: {a.launches:.0f} launches/day x {a.water_t:.0f} t = {mw_per_day/1e6:.1f} kt/day into ~80-115 km")
    print(f"  background {a.bg_ppmv:.1f} ppmv; the launch band ({a.band_deg:.0f} deg round the globe) +{x_band*1e6:.1f} ppmv;"
          f" the patch over the site (+{t_res/3600:.1f} h of exhaust in {a.region_km:.0f} km) +{x_region*1e6:.0f} ppmv")
    for name, x in (("background", x_bg), ("band", x_bg + x_band), ("over the site", x_bg + x_band + x_region)):
        print(f"  frost point, {name:>14}: {x*1e6:7.1f} ppmv -> {frost_point(x, a.p_pa):5.1f} K")
    Tf = frost_point(x_bg + x_band + x_region, a.p_pa)
    Tf_band = frost_point(x_bg + x_band, a.p_pa)
    print(f"temperature at {a.h_km:.0f} km, {a.lat:.0f} deg: summer {a.t_summer:.0f} K, winter {a.t_winter:.0f} K "
          f"(annual cosine), tide +-{a.tide:.0f} K, gravity waves {a.gw:.0f} K rms")
    print("month  T mean  P(T < frost) over the site   P(T < frost) in the band   (time fraction; ice needs it held ~hours)")
    sig = math.sqrt(a.tide * a.tide / 2.0 + a.gw * a.gw)
    for mo in range(1, 13):
        # northern summer peaks ~July 1 (day 182): cosine
        ph = math.cos(2 * math.pi * ((mo - 0.5) * 30.4 - 182.0) / 365.25)
        if a.lat < 0: ph = -ph
        Tm = 0.5 * (a.t_summer + a.t_winter) - 0.5 * (a.t_winter - a.t_summer) * ph
        print(f"  {mo:2d}   {Tm:5.1f}       {phi((Tf - Tm) / sig) * 100:5.1f}%                     {phi((Tf_band - Tm) / sig) * 100:5.1f}%")
    print(f"(fresh exhaust, minutes: frost point ~200 K in nearly pure vapour, Super Soaker 2018: transient ice at any season)")

if __name__ == '__main__':
    main()
