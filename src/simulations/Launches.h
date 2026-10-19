#pragma once
// Launches (2026-10-18, branch kessler; plan .plans/LAUNCHES_PLAN.md). The app-independent half: the catalog
// (data/launches.json: sites, vehicles, the programs that keep each constellation flying), the rocket's ascent and
// the booster's flight as 2D tracks (downrange, altitude), the orbit-raising and deorbit phase laws, and the
// exhaust parcels the plumes are drawn from. No Vulkan, no SatelliteSim — SatelliteSimLaunches.cpp schedules
// launches from the roster and draws them.
//
// Everything is a pure function of sim time: a launch is found from its plane, refresh and batch numbers through
// hashes and closed forms, never from a history, so time warp, reverse playback and bookmarks see the same sky.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <glm/glm.hpp>

namespace launch
{
constexpr double kGM = 3.986004418e14;
constexpr double kR = 6371000.0; // the sim's Earth radius

struct Site
{
    std::string id, name;
    double latDeg = 0.0, lonDeg = 0.0;
    double azMinDeg = 0.0, azMaxDeg = 360.0; // allowed launch azimuths (clockwise from north; wraps)
    glm::dvec3 ecef{1.0, 0.0, 0.0};          // unit, Earth-fixed
};

struct Vehicle
{
    std::string id, name;
    std::vector<int> sites;          // indices into Catalog::sites
    double t1 = 162.0;               // MECO / booster separation (s)
    double h1 = 68000.0, x1 = 80000.0; // altitude and downrange there (m)
    double t2 = 520.0;               // SECO: insertion into the parking orbit (s)
    double deployAfter = 900.0;      // payload deployment, s after SECO
    double flow1 = 2500.0, flow2 = 300.0; // propellant flow (kg/s): booster, upper stage
    double vex1 = 2900.0, vex2 = 3400.0;  // exhaust speed (m/s)
    int recovery = 2;                // booster: 0 expendable, 1 return to the pad (RTLS / tower catch), 2 droneship
    double flameMag1 = -4.0, flameMag2 = -2.0; // the flame's magnitude seen from 100 km
    bool upperDeorbit = true;        // the upper stage burns to re-enter after deployment (else it returns: no fireball)
    double upperMassKg = 4000.0;
    double soot = 0.5;               // 0 clean (methane, hydrogen) .. 1 sooty (kerosene, solids): the low trail's albedo
    double ventChance = 0.35;        // the upper stage vents spinning after its deorbit burn (a spiral)
};

struct Program
{
    std::string name;
    std::vector<std::string> match;  // constellation name substrings
    std::vector<int> vehicles;       // indices into Catalog::vehicles
    double lifeYears = 5.0;          // design life: each plane is refreshed once per life
    int payload = 40;                // satellites per launch (a multiple of the formation's cluster size)
    double parkKm = 300.0;           // deployment altitude
    double raiseDays[2] = {20.0, 35.0};   // deployment to arrival in the slot
    double deorbitDays[2] = {20.0, 60.0}; // the retiree: arrival of its replacement to re-entry
    double batchGapDays = 2.0;       // launches into one plane, apart
};

struct Catalog
{
    std::vector<Site> sites;
    std::vector<Vehicle> vehicles;
    std::vector<Program> programs;
    bool load(const std::string &path, std::string &err);
};

// ── Tracks ───────────────────────────────────────────────────────────────────────────────────────
// (x, h) = downrange along the ground track (m) and altitude (m), sampled every dt seconds from lift-off.
enum Burn : uint8_t
{
    BURN_NONE = 0,
    BURN_ASCENT = 1,
    BURN_BOOSTBACK = 2,
    BURN_ENTRY = 3,
    BURN_LANDING = 4,
    BURN_UPPER = 5,
};

struct Track
{
    double dt = 0.5;
    std::vector<glm::dvec2> p, v; // (x, h), (vx, vz)
    std::vector<uint8_t> burn;
    double tEnd = 0.0;            // the last sample (landing, or the end of drawing)
    bool landed = false;
    glm::dvec2 at(double t, glm::dvec2 *vel = nullptr, uint8_t *b = nullptr) const;
};

// The booster's whole flight (ascent, boostback, entry and landing burns: ground-relative) and the upper stage's
// ascent to its parking orbit at hPark (to t2).
Track boosterTrack(const Vehicle &v);
Track upperTrack(const Vehicle &v, double hPark);

// ── Orbit raising / lowering ─────────────────────────────────────────────────────────────────────
// A circular orbit whose radius goes linearly from a0 to a1 over T seconds: the integral of its mean motion
// over the first tau seconds (rad).
double rampPhase(double a0, double a1, double T, double tau);
inline double meanMotion(double a) { return std::sqrt(kGM / (a * a * a)); }

// ── Exhaust parcels ──────────────────────────────────────────────────────────────────────────────
// A parcel of exhaust (mass m, emitted at p0 with velocity v0 relative to the air, Earth-fixed axes about the
// Earth's centre): it expands into the air and entrains it — near the ground a narrow trail that the wind carries
// and diffusion widens, high up a bell expanding at km/s that falls back into the thicker air (the "jellyfish").
// Sampled at log-spaced ages; at(age) interpolates the centre and the radius.
struct Parcel
{
    double t0 = 0.0;             // emission (s after lift-off)
    bool thin = false;           // a vent's jet (never decimated: its arms are thin)
    double m = 0.0;              // kg
    double kappa = 1.0;          // scattering cross-section per kg (m^2/kg)
    double albedo = 0.9;
    glm::dvec3 axis{0.0};        // the trail's direction at emission (unit, Earth-fixed): the parcel is a segment
    double len = 0.0;            // its length (m)
    std::vector<float> age;      // samples
    std::vector<glm::dvec3> c;   // centre (Earth-fixed, m from the Earth's centre)
    std::vector<float> sig;      // radius (m, 1 sigma)
    std::vector<float> cond;     // condensed share (0..1): vapour turns to ice as it expands and cools
    std::vector<float> sweep;    // air taken in / exhaust mass: past ~1 the gas piles up at its rim (a shell)
    bool at(double a, glm::dvec3 &cOut, double &sOut, double &condOut, double *sweepOut = nullptr) const;
    float maxAge() const { return age.empty() ? 0.0f : age.back(); }
};

double airDensity(double hM); // kg/m^3
// Integrates a parcel to maxAge (s). wind(h) = the wind at altitude h (m/s, Earth-fixed axes) for the launch.
// vExp: how fast the parcel spreads while it outweighs the air it has taken in (exhaust gas ~2 km/s into vacuum;
// vented propellant, droplets freezing to ice, ~100 m/s).
Parcel makeParcel(double t0, const glm::dvec3 &p0, const glm::dvec3 &v0, double m, double kappa, double albedo,
                  const glm::dvec3 &axis, double len, double maxAge, const glm::dvec3 &windDir, double windGain,
                  double vExp = 2000.0);

double hash01(uint64_t seed, uint64_t index, uint32_t channel);
} // namespace launch
