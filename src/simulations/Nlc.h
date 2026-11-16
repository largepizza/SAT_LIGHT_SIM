// Mesospheric ice (noctilucent cloud) from rocket exhaust water: the physics shared by the CPU field
// (SatelliteSimNLC.cpp), the state report and tools/nlc_model.py (2026-11-16, .plans/PLUME_OVERHAUL_PLAN.md phase 3,
// issue_20261115_165612; the user's decision: physical, "the launch rate is the physics").
// docs/subsystems/launches.md "Launch-made noctilucent clouds".
//
// Ice exists at ~83 km wherever the air is colder than the FROST POINT of its water vapour. The vapour is a background
// plus every launch's exhaust water still there (deposited where the upper stage burns through 80-115 km, carried
// toward the summer pole, spread, photolysed); the temperature is a climatology by latitude and season plus the tides
// (and, in the shader, gravity waves). Nothing here is tuned to make clouds: the frequency is an output.
#pragma once

#include <algorithm>
#include <cmath>

namespace nlc
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kR = 6371000.0;
constexpr double kShellH = 83000.0;   // the layer's height (m): where polar mesospheric clouds sit (82-85 km)
constexpr double kPAir = 0.55;        // air pressure there (Pa, US standard atmosphere 1976 at 83 km)
constexpr double kRhoAir = 1.15e-5;   // air density there (kg/m^3)
// The air the exhaust water mixes into: everything above ~78 km (rho x scale height; the turbopause at ~105 km keeps it
// mixed): kg/m^2 of air per m^2 of ground.
constexpr double kAirColumn = 0.12;
constexpr double kMwRatio = 28.97 / 18.015;   // mole fraction per mass fraction of water in air
constexpr double kBgPpmv = 5.0;               // the background water vapour at 83 km (ppmv; ~2-6 observed)
constexpr double kLifeS = 3.0 * 86400.0;      // H2O's photolysis lifetime by Lyman alpha at 80-85 km (Nicolet 1981)
constexpr double kWindowS = 9.0 * 86400.0;    // deposits older than this (5% left) are dropped
// Transport of the exhaust water deposited at 100-115 km: toward the SUMMER pole at up to ~35 m/s (the shuttle's plume
// reached the Arctic from Florida in about a day, STS-107's the Antarctic in ~3: Stevens et al. 2003 GRL, 2005 JGR;
// Niciejewski et al. 2011), spread by eddy diffusion (K ~ 4e5 m^2/s: ~260 km after a day, ~450 after three).
constexpr double kMeridMps = 35.0;
constexpr double kDiffK = 4.0e5;
constexpr unsigned kMapW = 512, kMapH = 256;   // the global map (equirectangular, ~78 km a texel at the equator)
struct Job;                                    // a map being built (SatelliteSimNLC.cpp)
constexpr double kGwRmsK = 8.0;   // gravity waves' temperature rms at 83 km (K; the shader's noise, the CPU's statistics)

// Ice's saturation vapour pressure (Pa) at T (K): Murphy & Koop 2005, eq. 7.
inline double pIce(double T)
{
    return std::exp(9.550426 - 5723.265 / T + 3.53068 * std::log(T) - 0.00728332 * T);
}
// The frost point (K) of water vapour at mixing ratio ppmv at the shell's pressure.
inline double frostPointK(double ppmv)
{
    const double pw = std::max(ppmv, 1e-3) * 1e-6 * kPAir;
    double lo = 100.0, hi = 280.0;
    for (int i = 0; i < 40; ++i)
    {
        const double m = 0.5 * (lo + hi);
        (pIce(m) > pw ? hi : lo) = m;
    }
    return 0.5 * (lo + hi);
}

// Day of the year (0 = Jan 1 00:00 UTC, fractional) and the hour of the day (UTC) at t (J2000 seconds: 12:00 UTC on
// 2000-01-01).
inline double dayOfYear(double tJ2000)
{
    const double d = tJ2000 / 86400.0 + 0.5;
    return d - 365.2422 * std::floor(d / 365.2422);
}
inline double utcHours(double tJ2000)
{
    const double h = tJ2000 / 3600.0 + 12.0;
    return h - 24.0 * std::floor(h / 24.0);
}
// The season: +1 at the northern summer's height (flat from ~48 days before to after the June solstice: the polar
// summer mesopause stays cold that long, the noctilucent season), -1 at the southern's, through 0 at the equinoxes.
inline double season(double tJ2000)
{
    const double c = std::cos(2.0 * kPi * (dayOfYear(tJ2000) - 171.5) / 365.2422);
    return std::clamp(1.5 * c, -1.0, 1.0);
}

// The zonal-mean temperature at 83 km (K) at latitude lat (rad) in season s: x = sin(lat) s is +1 at the summer pole,
// -1 at the winter pole. Fitted to: the summer pole ~140 K and 69 N ~147 K in July (ALOMAR / SOFIE), 41 N ~167 K in
// summer (Fort Collins sodium lidar, Schmidt & Reising 2008: 169 here), the equator ~190 K, the winter pole ~208 K
// (low-latitude values are an ASSUMPTION, MSIS-like: 26 N 180 K summer, 198 K winter).
inline double tClimK(double lat, double s)
{
    const double x = std::sin(lat) * s;
    const double xp = std::max(x, 0.0);
    return 190.0 - 18.0 * x - 32.0 * xp * xp * xp;
}
// The tides at 83 km (K): the migrating diurnal tide, ~12 K at the equator falling off by ~35 deg, and the semidiurnal,
// ~5 K. Phases (the warmest at ~03 and 04 / 16 local solar time) are an ASSUMPTION to pin (SABER climatologies).
inline double tideK(double lat, double lon, double tJ2000)
{
    double lst = utcHours(tJ2000) + lon * (12.0 / kPi);
    const double w = 2.0 * kPi / 24.0;
    const double ld = lat / (35.0 * kPi / 180.0);
    return 12.0 * std::exp(-ld * ld) * std::cos(w * (lst - 3.0)) + 5.0 * std::cos(2.0 * w * (lst - 4.0));
}
inline double tMeanK(double lat, double lon, double tJ2000)
{
    return tClimK(lat, season(tJ2000)) + tideK(lat, lon, tJ2000);
}

// A launch's exhaust water at 80-115 km: where (lat, lon of its water-weighted centroid), when (the time the stage
// passed it), how much (kg) and how wide at first (m, half its length along the track).
struct Deposit
{
    double t = 0.0, lat = 0.0, lon = 0.0, m = 0.0, sig0 = 20000.0;
};
// Where it is at t, how wide (sigma, m) and how much water is left (kg). false: not yet deposited or gone.
// Meridional drift dlat/dt = V cos(lat) / R (closed form in the Mercator coordinate psi = atanh(sin lat): it moves
// linearly), V = kMeridMps x season toward the summer pole; no mean zonal drift (the tides' winds swing a sheet back and
// forth ~200 km twice a day and average out: left out); widened by diffusion; decayed by photolysis.
inline bool depositAt(const Deposit &d, double t, double &lat, double &lon, double &sig, double &m)
{
    const double a = t - d.t;
    if (a < 0.0 || a > kWindowS)
        return false;
    const double v = kMeridMps * season(t);
    const double s0 = std::clamp(std::sin(d.lat), -0.9962, 0.9962);   // within 85 deg of a pole
    const double psi = std::atanh(s0) + v * a / kR;
    lat = std::asin(std::clamp(std::tanh(psi), -0.9962, 0.9962));
    lon = d.lon;
    sig = std::sqrt(d.sig0 * d.sig0 + 2.0 * kDiffK * a);
    m = d.m * std::exp(-a / kLifeS);
    return true;
}
// The mixing ratio (ppmv) a column of water (kg/m^2) adds to the air above ~78 km.
inline double ppmvOfColumn(double kgM2) { return kgM2 / kAirColumn * kMwRatio * 1e6; }

// The water share of a stage's exhaust by its propellant: hydrogen-oxygen (exhaust speed >= 4.2 km/s) ~0.95, methane
// (clean, below that) ~0.44 (Raptor's O/F 3.6), kerosene and solids ~0.30.
inline double waterShare(double vex, double soot)
{
    return vex >= 4200.0 ? 0.95 : soot <= 0.1 ? 0.44 : 0.30;
}

// The chance of ice now at a point (the gravity waves' spread around the mean): P(T < frost point).
inline double iceChance(double tMean, double tFrost)
{
    return 0.5 * (1.0 + std::erf((tFrost - tMean) / (kGwRmsK * std::sqrt(2.0))));
}
} // namespace nlc
