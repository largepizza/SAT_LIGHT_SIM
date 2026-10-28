#pragma once
// Comets.h — comets on heliocentric conic orbits (2026-10-27; CLAUDE.md "Comets").
//
// App-independent: the orbits (a perihelion state carried by a universal-variable Kepler propagator, so parabolas,
// hyperbolas and long ellipses alike), the magnitude law, the dust phase function, the catalog (the scripted Great
// Comet of 2037 + deterministic minor comets hashed from (year, index)), and the tail geometry (dust syndyne/synchrone
// sheet, ion tail axis). Everything is a pure function of sim time t (seconds since J2000) in the J2000 mean-ecliptic
// heliocentric frame the planets use (SatelliteSim.cpp keplerEclipticPos), in AU and days.
// The renderer (SatelliteSimComets.cpp) projects what this produces for the observer.
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace comet
{
constexpr double kGaussK = 0.01720209895;   // rad/day, AU^1.5
constexpr double kMuSun = kGaussK * kGaussK; // AU^3 / day^2
constexpr double kAuKm = 1.495978707e8;
constexpr double kDayS = 86400.0;

struct Elements
{
    char name[48] = {};
    double q = 1.0;           // perihelion distance, AU
    double e = 1.0;           // eccentricity (1 = parabola)
    double iDeg = 0.0, nodeDeg = 0.0, periDeg = 0.0; // J2000 ecliptic: inclination, ascending node, argument of perihelion
    double tPeri = 0.0;       // perihelion time, s since J2000
    double H = 10.0;          // absolute total magnitude (1 AU from Sun and observer, phase 90 deg)
    double n = 4.0;           // activity index: m = H + 5 log Delta + 2.5 n log r
    double rSat = 0.04;       // AU: brightening saturates inside this (r_eff = sqrt(r^2 + rSat^2); sungrazers)
    double dust = 1.0;        // dust richness (scales the dust tail and the forward-scattering surge)
    double gas = 1.0;         // gas richness (ion tail, the coma's C2 green)
    double comaKm = 1.2e5;    // visible coma radius at 1 AU (scales as r^-0.5, capped)
    uint32_t seed = 0;
    bool great = false;       // the scripted Great Comet of 2037
    bool periodic = false;    // a Jupiter-family comet ("P/" designation)
    // derived (comet::finalize)
    glm::dvec3 pHat{1, 0, 0}, tHat{0, 1, 0}; // perihelion direction and the direction of motion there (ecliptic)
    glm::dvec3 rPeri{0.0}, vPeri{0.0};       // the perihelion state, AU and AU/day
};
void finalize(Elements &el);

struct State
{
    glm::dvec3 r{0.0}, v{0.0}; // heliocentric ecliptic, AU, AU/day
};
// Universal-variable Kepler: the state dtDays after s0 under gravitational parameter mu (AU^3/day^2). False if it
// did not converge (the input state is returned).
bool propagate(const State &s0, double dtDays, double mu, State &out);
// The comet's heliocentric state at t (s since J2000).
State stateAt(const Elements &el, double t);

// Apparent total magnitude of the coma (the head) at heliocentric r, geocentric delta (AU), phase angle (deg, Sun-comet-
// observer): H + 5 log delta + 2.5 n log r_eff, with the dust's forward-scattering surge at high phase angle (Marcus
// 2007, the compound Henyey-Greenstein "Halley-Marcus" phase function normalised at 90 deg, dust-to-gas ratio from `dust`).
double magnitude(const Elements &el, double rAU, double deltaAU, double phaseDeg);
// The dust's scattering phase function at scattering angle thetaDeg (0 = straight on, 180 = backscatter), normalised
// to 1 at 90 deg (Halley-Marcus compound Henyey-Greenstein).
double dustPhase(double scatterDeg);

// The catalog: every comet whose perihelion is within `windowYears` of t: the Great Comet + minor comets hashed per
// perihelion year (~6 a year, mostly mag 5-12 at their best). Deterministic.
void catalogNear(double t, double windowYears, std::vector<Elements> &out);
const Elements &greatComet();
// Minor comets with perihelion in calendar year `year` (deterministic: hash of (year, index)).
void minorCometsOfYear(int year, std::vector<Elements> &out);

// ── Tail geometry ────────────────────────────────────────────────────────────────────────────────────────────────
// The dust tail as a sheet over (emission age tau, ln beta): particles leave the nucleus at t - tau with its velocity
// and fly under the Sun's gravity reduced by radiation pressure, mu (1 - beta) (Finson & Probstein's syndyne /
// synchrone picture). Rows of constant tau are synchrones, columns of constant beta syndynes.
struct DustGrid
{
    int nBeta = 0, nTau = 0;
    std::vector<double> beta;        // per column
    std::vector<double> tauDays;     // per row
    std::vector<glm::dvec3> relPos;  // nTau x nBeta: particle position minus the nucleus at tBuilt, AU (ecliptic)
    std::vector<double> rSun;        // per point: the particle's distance from the Sun, AU
    std::vector<double> emitRate;    // per row: the relative dust production at emission (r(t - tau))
    double tBuilt = 0.0;             // s since J2000
    double tPeri = 0.0;              // the comet's perihelion (for striae, emission times relative to it)
};
// tauMaxDays: the oldest dust drawn; nBeta x nTau samples.
void buildDustGrid(const Elements &el, double t, int nBeta, int nTau, double tauMaxDays, DustGrid &g);
// The dust production relative to 1 AU at heliocentric distance r (AU), saturated as the magnitude law.
double dustRate(const Elements &el, double rAU);
// The ion tail's axis: along the solar wind as the comet sees it (400 km/s radial, minus the comet's velocity).
glm::dvec3 ionAxis(const State &s);
} // namespace comet
