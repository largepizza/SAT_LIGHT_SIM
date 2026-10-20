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
    // The pad on the ground (2026-10-19, launch-site graphics): its kind (-1 = from the vehicles that fly from it:
    // 2 a Starship pad with its catch tower, 1 a Falcon pad (fixed service structure, a landing zone), 0 another
    // pad), the heading of its layout (deg clockwise from north; NaN = the middle of its azimuth range), and its
    // ground height (m above sea level, read from the DEM when the catalog is loaded; the terrain is flattened to it).
    int padKind = -1;
    double padHeadingDeg = std::nan("");
    double elevM = 0.0;
};

struct Vehicle
{
    std::string id, name;
    std::vector<int> sites;          // indices into Catalog::sites
    double t1 = 162.0;               // MECO / booster separation (s)
    double h1 = 68000.0, x1 = 80000.0; // altitude and downrange there (m)
    double t2 = 520.0;               // SECO: insertion into the parking orbit (s)
    double deployAfter = 900.0;      // payload deployment, s after SECO
    double deployIntervalS = 2.0;    // the satellites leave the dispenser one after another, this far apart
    double sepSpeedMps = 0.45;       // their separation speed from the dispenser (mostly up, a little back)
    double sepSpreadMps = 0.12;      // +- per member and axis: a stack's spread (Starship's PEZ dispenser: a few cm/s,
                                     // so the train leaves as one clean string, sep x interval apart)
    double flow1 = 2500.0, flow2 = 300.0; // propellant flow (kg/s): booster, upper stage
    double vex1 = 2900.0, vex2 = 3400.0;  // exhaust speed (m/s)
    int recovery = 2;                // booster: 0 expendable, 1 return to the pad (RTLS / tower catch), 2 droneship
    double flameMag1 = -4.0, flameMag2 = -2.0; // the flame's magnitude seen from 100 km
    bool upperDeorbit = true;        // the upper stage burns to re-enter after deployment (else it returns: no fireball)
    double upperMassKg = 4000.0;
    double soot = 0.5;               // 0 clean (methane, hydrogen) .. 1 sooty (kerosene, solids): the low trail's albedo
    double ventChance = 0.35;        // the upper stage vents spinning after its deorbit burn (a spiral)
    double mountM = 0.0;             // the launch mount's height: the vehicle stands on it (Starship: ~20 m)
    double catchM = 0.0;             // a tower-caught booster stops this far above the mount (its aft end)
    // Payload fairing (2026-10-20, issue 20261019_225243): the halves separate once the upper stage passes this
    // altitude (0 = no fairing: Starship, Dragon); the payload adapter's top on the upper stage (its model's +Z, m:
    // tools/make_rocket_models.py prints it); Starship's payload door ("PEZ" slot) height above its aft end.
    double fairingSepKm = 0.0;
    double payloadBaseM = 0.0;
    glm::dvec3 fairingCenter{0.0}; // the +X fairing half's centre on the stage (its model's origin), stage frame
    double doorM = 31.1;
};

// How a program's satellites ride to orbit (2026-10-20, launches.json "stack"; tools/make_rocket_models.py derives
// and checks the numbers): drawn on the upper stage once its fairing is gone, released one after another.
struct Stack
{
    enum Layout : int { None = 0, Pez = 1, Flat = 2, Column = 3 };
    int layout = None;
    int perTier = 1;            // flat: 1 or 2 side by side; column: around the column
    bool sideX = false;         // flat, 2 a tier: side by side along the satellite's X (else its Y)
    double pitchM = 1.0;        // tier to tier along the stage
    double offsetM = 0.0;       // flat: each centre's offset from the axis; column: the centres' radius
    double baseM = 0.3;         // the lowest tier's bottom above the payload adapter
    double columnRadiusM = 0.0; // column: the dispenser's
    glm::dvec3 center{0.0};     // the stowed satellite's centre (body frame): placed at the slot
    std::string dispenser;      // its model: rocket_dispenser_<this>.json ("" = none)
    double intervalS = -1.0, sepMps = -1.0, spreadMps = -1.0; // < 0: the vehicle's
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
    // Crew rotation (2026-10-19, the Missions window): a crewed program flies people to a station (a constellation of
    // one) every life_years (NOT scaled by the design-life setting). No train, no retiree: raise_days is the time from
    // lift-off to docking, and only the rocket and its upper stage are drawn (the capsule's rendezvous is not).
    bool crew = false;
    std::string spacecraft;          // "Crew Dragon", "Shenzhou", ...
    int people = 0;
    Stack stack;
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
// A free body's motion relative to a circular orbit of mean motion n (Clohessy-Wiltshire), starting at the reference
// point with velocity v0 = (radial, along-track, cross-track), after t seconds: position p and velocity v, same axes.
void cwFree(const glm::dvec3 &v0, double n, double t, glm::dvec3 &p, glm::dvec3 &v);
// The same from position p0 and velocity v0 (the general Clohessy-Wiltshire solution).
void cwState(const glm::dvec3 &p0, const glm::dvec3 &v0, double n, double t, glm::dvec3 &p, glm::dvec3 &v);

// ── Exhaust parcels ──────────────────────────────────────────────────────────────────────────────
// A parcel of exhaust (mass m, emitted at p0 with velocity v0 relative to the air, Earth-fixed axes about the
// Earth's centre): it expands into the air and entrains it — near the ground a narrow trail that the wind carries
// and diffusion widens, high up a bell expanding at km/s that falls back into the thicker air (the "jellyfish").
// Sampled at log-spaced ages; at(age) interpolates the centre and the radius.
struct Parcel
{
    double t0 = 0.0;             // emission (s after lift-off)
    bool thin = false;           // a vent's jet (never decimated: its arms are thin)
    int stream = -1;             // the burn it belongs to (>= 0: drawn as a polyline through its stream's parcels)
    int stage = 0;               // which stage emitted it (the head segment runs to that stage's nozzle)
    double dt = 0.0;             // the emission interval it stands for (s)
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
                  double vExp = 2000.0, double windPhase = 0.0);

double hash01(uint64_t seed, uint64_t index, uint32_t channel);
} // namespace launch
