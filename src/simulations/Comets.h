#pragma once
// Comets.h — comets on heliocentric conic orbits (2026-10-27; CLAUDE.md "Comets").
//
// App-independent: the orbits (a perihelion state carried by a universal-variable Kepler propagator, so parabolas,
// hyperbolas and long ellipses alike), the magnitude law, the dust phase function, the catalog (the scripted Great
// Comet of 2037 + deterministic minor comets hashed from (year, index)), and the tails as 3D particles (dust grains,
// ion parcels: buildParticles; the ion tail axis). Everything is a pure function of sim time t (seconds since J2000) in the J2000 mean-ecliptic
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

// ── Tail particles (2026-10-28) ─────────────────────────────────────────────────────────────────────────────────
// The tails are clouds of SAMPLE particles in 3D heliocentric space. A dust grain leaves the nucleus at its emission
// time te (a fixed absolute grid: slots uniform in asinh((te - tPeri) / tau0), fine round perihelion, coarse far from
// it, stable as time runs), with a size (radiation-pressure ratio beta, stratified in ln beta) and an ejection velocity
// (sunward-biased, v0 sqrt(beta) r^-0.5), and flies under the Sun's gravity reduced by radiation pressure, mu (1 - beta),
// carried by the universal-variable Kepler propagator. Syndynes, synchrones, the perihelion burst's synchrone and an
// anti-tail (when the Earth crosses the orbital plane) emerge from it. The ion tail is parcels of plasma picked up at
// the nucleus and carried radially by the solar wind (accelerating to ~250 km/s) in a few streamers whose kinks are
// frozen into the plasma. Everything is a pure function of (tBuild, the particle's indices).
// A build stores each particle's state at tBuild relative to an anchor (the nucleus then); the GPU carries it to the
// frame's time by a second-order Taylor step (the acceleration from the particle's own heliocentric position).
struct GpuGrain
{
    glm::vec4 p; // xyz: position at tBuild minus the anchor (AU, ecliptic); w: flux weight (mag-0 flux at 1 AU from the
                 //      Sun and the observer; dust: at phase-function 1, i.e. 90 deg scattering)
    glm::vec4 v; // xyz: heliocentric velocity at tBuild (AU/day); w: spread (AU) at tBuild (the Gaussian it stands for)
    glm::vec4 q; // x: 1 - beta (dust; ions 0: no gravity), y: spread growth (AU/day), z: age at tBuild (days),
                 // w: kind (0 dust, 1 ion) + 4 x the comet's slot
    // The local (emission time, size) field round a dust grain (2026-10-28, the tail's zoom detail): the splat maps each
    // pixel of the grain's Gaussian to the emission time and ln beta of the dust there (a 2x2 screen Jacobian), so
    // time-variable emission (synchronic bands), fragmentation striae and size filaments resolve as the view zooms in.
    // d: x, y, z = half floats (dP/dte at fixed beta: AU per day of emission time; dP/d ln beta at fixed te: AU) packed
    //    as (Gx, Gy), (Gz, Bx), (By, Bz); w = the float bits of the field's emission time at the grain's position (days
    //    from perihelion, its own ejection offset taken out). Its ln beta is the grain's own (q.x). Ions: 0.
    glm::uvec4 d;
};
static_assert(sizeof(GpuGrain) == 64, "GpuGrain is mirrored in comet_frame.glsl");
struct ParticleOpts
{
    double tauMaxDays = 45.0;   // the oldest dust
    double tau0Days = 0.05;     // the slot grid's asinh scale (a sungrazer: hours round perihelion)
    double du = 0.01;           // slot width in asinh units
    int grainsMin = 8, grainsMax = 96; // grains per slot: grainsMin x cosh(u)^0.33 within these (a wide slot gets more)
    double v0KmS = 0.5;         // ejection speed at beta 1, 1 AU
    double ionDtDays = 0.01;    // ion parcel grid: a pick-up at every whole w = t / ionDtDays + nu / ionDnuRad (nu the
    double ionDnuRad = 0.02;    // true anomaly, unwrapped): finer where the radial direction swings (perihelion)
    double ionMaxAgeDays = 2.5;
    int ionPerSlot = 12;
    double dustFluxK = 0.025;   // the dust's flux per (dust rate x day of emission), x Fhead0 x dust
    double ionFluxK = 0.3;      // the ion tail's total flux, x Fhead0 x gas x r_eff^-n
    int maxGrains = 120000;
    bool detail = true;         // the dust's (emission time, size) field for the splat's zoom detail (GpuGrain d / e)
    int threads = 4;
};
struct ParticleBuild
{
    double tBuild = -1e300;     // s since J2000
    glm::dvec3 anchor{0.0};     // the nucleus at tBuild (heliocentric ecliptic AU)
    std::vector<GpuGrain> grains;
    uint32_t nDust = 0, nIon = 0;
    // For framing, from obsHelio (the observer at tBuild): the visible tail's span from the head (rad) and a direction
    // (ecliptic unit vector from the observer) toward its far part.
    double spanRad = 0.0;
    glm::dvec3 aimEcl{0.0, 0.0, 1.0};
    double buildMs = 0.0;
};
// slot: the comet's GPU slot (0..3), written into q.w.
void buildParticles(const Elements &el, double tBuild, const glm::dvec3 &obsHelio, int slot, const ParticleOpts &o,
                    ParticleBuild &out);
// The particle build grid's step at heliocentric distance r (AU): a power of two in seconds, ~0.003 of the local
// dynamical time sqrt(r^3 / mu) (the GPU's Taylor step then errs by ~1e-8 r), 4 s .. 4 h.
double particleBuildStepS(double rAU);
// The dust production relative to 1 AU at heliocentric distance r (AU), saturated as the magnitude law.
double dustRate(const Elements &el, double rAU);
// The ion tail's axis: along the solar wind as the comet sees it (400 km/s radial, minus the comet's velocity).
glm::dvec3 ionAxis(const State &s);
} // namespace comet
