#pragma once
// Kessler syndrome (branch kessler, 2026-10-16; plan: .plans/KESSLER_PLAN.md). The app-independent
// half: orbit elements from state vectors and back, J2 secular rates, the NASA Standard Breakup
// Model, a drag lifetime estimate and the statistical collision engine (cube method). No Vulkan, no
// SatelliteSim — SatelliteSimKessler.cpp turns its results into satellites.
//
// Everything is deterministic: random numbers come from hashes of (seed, index), never from a
// shared stream, so results do not depend on thread count or evaluation order.

#include <cstdint>
#include <functional>
#include <vector>

#include <glm/glm.hpp>

#include "SatPhotometry.h"

namespace kessler
{
constexpr double kGM = 3.986004418e14;  // m^3/s^2 (satphot::kGM)
constexpr double kReq = 6378137.0;      // equatorial radius for J2 (m)
constexpr double kRMean = 6371000.0;    // the sim's Earth radius (altitudes)
constexpr double kJ2 = 1.08262668e-3;
constexpr double kPi = 3.14159265358979323846;
constexpr double kTwoPi = 2.0 * kPi;

struct State
{
    glm::dvec3 r{0.0}; // m, ECI
    glm::dvec3 v{0.0}; // m/s
};

// Classical elements; angles in radians. M = mean anomaly.
struct Elems
{
    double a = 0.0, e = 0.0, i = 0.0, raan = 0.0, argp = 0.0, M = 0.0;
};

Elems elemsFromState(const State &s);
State stateFromElems(const Elems &el);

// Secular J2 rates of a Keplerian orbit: dM/dt (including the two-body mean motion), dω/dt, dΩ/dt.
struct Secular
{
    double Mdot = 0.0, argpDot = 0.0, raanDot = 0.0;
};
Secular secularJ2(double a, double e, double incl);

// Deterministic uniforms / normals from (seed, index, channel).
double hash01(uint64_t seed, uint64_t index, uint32_t channel);
double hashNormal(uint64_t seed, uint64_t index, uint32_t channel);

// ── NASA Standard Breakup Model (Johnson et al. 2001, Krisko 2011) ───────────────────────────────
struct Fragment
{
    double Lc = 0.1;        // characteristic length (m)
    double areaM2 = 0.01;   // mean cross-section
    double massKg = 0.1;
    double weight = 1.0;    // real fragments this record stands for (representative particles)
    glm::dvec3 dv{0.0};     // ejection velocity relative to its parent (m/s)
    int side = 0;           // 0 = from the target / exploding object, 1 = from the projectile
};

struct BreakupResult
{
    bool catastrophic = false;
    double nReal = 0.0;     // fragments >= LcMin the model predicts
    std::vector<Fragment> fragments;
};

// A collision between a target of mass mT and a projectile of mass mP at relative speed vRel (m/s):
// catastrophic above 40 J/g of specific energy. At most maxRecords records are made; each stands for
// nReal / records fragments (weight).
BreakupResult breakupCollision(double mT, double mP, double vRel, double LcMin, int maxRecords, uint64_t seed);
// An explosion (scale factor S = 1) of an object of mass m.
BreakupResult breakupExplosion(double m, double LcMin, int maxRecords, uint64_t seed);

// ── Drag ──────────────────────────────────────────────────────────────────────────────────────────
// Atmospheric density (kg/m^3) at altitude h (m) for moderate solar activity (piecewise exponential).
double atmosDensity(double hM);
// Seconds from tStart until the orbit (a, e) decays to ~120 km, for a ballistic coefficient
// cdAm = Cd A / m (m^2/kg). densityFactor(t) multiplies the atmosphere (a geomagnetic storm).
// Returns > maxS when the object outlives maxS.
double decayTime(double tStart, double a, double e, double cdAm, double maxS,
                 const std::function<double(double)> &densityFactor);

// ── Collision engine (cube method, Liou 2006) ─────────────────────────────────────────────────────
// One object as the engine sees it at a sample instant.
struct EngineObject
{
    uint32_t id = 0;        // the caller's index (roster satellite or pool record)
    glm::dvec3 r{0.0}, v{0.0};
    float radiusM = 1.0f;   // collision radius
    float weight = 1.0f;    // representative count
    bool active = false;    // manoeuvring (dodges tracked objects)
};

struct EngineCollision
{
    uint32_t a = 0, b = 0;  // ids
    double vRel = 0.0;
};

struct EngineParams
{
    double cubeM = 20000.0;     // cube size
    double avoidance = 0.9;     // share of active-vs-tracked encounters avoided
    double rateMultiplier = 1.0;
};

// Collisions in one sample of length dt (s): every pair sharing a cube where at least one is passive
// collides with probability (vRel sigma dt / V) x weights x (1 - avoidance if one is active). Pairs of
// active objects are skipped (coordinated constellations). `objs` holds every live object at the sample
// instant. Deterministic in (seed, ids).
std::vector<EngineCollision> engineSample(const std::vector<EngineObject> &objs, double dt, const EngineParams &p,
                                          uint64_t seed);

// ── The world: a deterministic history of breakups ────────────────────────────────────────────────
// Object references: a roster satellite's index, or kPoolBit | a pool record's index.
constexpr uint32_t kPoolBit = 0x80000000u;
constexpr uint32_t kNoRef = 0xFFFFFFFFu;
constexpr int32_t kNever = 0x7FFFFFFF;

// A roster satellite as the world needs it (a compact copy of SatOrbit: the world runs on a worker).
struct RosterObj
{
    float raan = 0.0f, incl = 0.0f, u0 = 0.0f, R = 0.0f, n = 0.0f;
    float formP = 0.0f, formQ = 0.0f, formK = 0.0f;
    uint16_t type = 0;
    uint8_t constIdx = 0;
    bool sso = false, formation = false;
    uint8_t clusterSize = 1, clusterSlot = 0;
};

struct TypeInfo
{
    double massKg = 260.0;
    double areaM2 = 10.0;   // mean projected area (collision cross-section, drag)
    double radiusM = 1.8;   // sqrt(area / pi)
    bool passive = false;   // never manoeuvres (debris)
};

// A pool record: a dead satellite (forced tumble) or a fragment. Angles at t = 0 (J2000), rates per
// second, all as the GPU's floats; a, e, incl in double for the CPU.
struct PoolRec
{
    float M0 = 0.0f, argp0 = 0.0f, raan0 = 0.0f, tumble0 = 0.0f;
    float n = 0.0f, argpDot = 0.0f, raanDot = 0.0f, tumbleRate = 0.0f;
    double a = 0.0, e = 0.0, incl = 0.0;
    float flux = 1.0f;      // flux scale against its type's model
    float massKg = 1.0f, radiusM = 0.1f, cdAm = 0.01f, weight = 1.0f;
    int32_t bornSec = 0, deathSec = kNever; // seconds since the scenario epoch
    uint16_t type = 0;
    uint8_t constIdx = 0;
    uint8_t flags = 0;      // 1 = forced tumble (a dead satellite), 2 = fragment
    uint32_t event = 0;     // the event that made it
    uint32_t parent = kNoRef;
};
constexpr uint8_t kRecDead = 1, kRecFragment = 2;

enum EventKind : uint8_t
{
    EV_KILL = 0,      // a satellite dies (the storm, a bump, damage): becomes a tumbling pool record
    EV_BUMP = 1,      // a dead formation member drifts into its neighbour (gentle: may kill it)
    EV_COLLISION = 2, // hypervelocity collision (NASA SBM)
    EV_EXPLOSION = 3,
    EV_ASAT = 4,      // a kinetic interceptor (a 20-kg projectile at ~10 km/s)
};
enum EventOrigin : uint8_t
{
    ORIGIN_ENGINE = 0,
    ORIGIN_SEED = 1, // the scenario's scripted history (the storm)
    ORIGIN_USER = 2,
};

struct Event
{
    double t = 0.0;
    uint32_t a = kNoRef, b = kNoRef;
    uint8_t kind = EV_KILL, origin = ORIGIN_ENGINE, catastrophic = 0, pad = 0;
    uint32_t seed = 0;
    float vRel = 0.0f, nReal = 0.0f;
    uint32_t firstRec = 0, nRec = 0; // pool records it made (filled by apply)
};

struct WorldSettings
{
    double epoch = 0.0;          // the scenario epoch (J2000 s): born / death seconds count from here
    double raanAnchorT = 0.0;    // the sim's SSO RAAN anchor (orbitElemsOf)
    double horizonS = 50.0 * 365.25 * 86400.0;
    double stepS = 6.0 * 3600.0;
    double LcMin = 0.1;
    int maxRecordsPerEvent = 64;  // a breakup's records at an empty pool, tapering as it fills
    uint32_t poolCapacity = 1000000;
    double activeSample = 0.25;  // share of active satellites in each engine sample (weight 1 / it)
    double stormDensityGain = 7.0; // thermosphere density x (1 + gain x scriptedStormShape)
    uint32_t debrisType = 0;
    uint8_t debrisConst = 0;
    double debrisModelArea = 0.03; // the debris type's mean area (fragment flux scale)
    EngineParams engine;
};

class World
{
public:
    WorldSettings s;
    std::vector<RosterObj> roster;
    std::vector<TypeInfo> types;
    std::vector<int32_t> rosterEnd;   // end second per roster satellite (kNever)
    std::vector<uint32_t> rosterCorpse; // its pool record once dead (kNoRef)
    std::vector<PoolRec> pool;
    std::vector<Event> events;        // applied, in time order
    double frontier = 0.0;            // simulated up to here (J2000 s)
    uint64_t sampleCount = 0;

    // Change notifications since the last take (the main thread mirrors pool and rosterEnd).
    std::vector<std::pair<uint32_t, int32_t>> poolDeaths;   // (record, death second)
    std::vector<std::pair<uint32_t, int32_t>> rosterDeaths; // (satellite, end second)

    void reset();
    State stateOf(uint32_t ref, double t) const;
    bool alive(uint32_t ref, double t) const;
    int32_t secOf(double t) const;
    // Applies one event (fills its record range and pushes it to `events`). Returns false (and does
    // nothing) when an object it names is no longer alive at its time.
    bool apply(Event ev);
    // Runs from the frontier to tEnd: due seed / user events (in `pending`, sorted by time) and engine
    // samples. Stops early when *stop turns true.
    void simulate(double tEnd, std::vector<Event> &pending, size_t &pendingNext, const bool *stop);

private:
    uint32_t addRecord(const PoolRec &r);
    void killAsCorpse(uint32_t ref, double t, uint32_t eventIdx);
    void spawnFragments(const BreakupResult &br, uint32_t refA, uint32_t refB, const State &sa, const State &sb,
                        double t, uint32_t eventIdx, uint32_t seed);
    PoolRec recordFromState(const State &st, double t, uint32_t parentRef, const glm::dvec3 &dv) const;
    double deathTime(const PoolRec &r, double t) const;
};

// The orbit elements the CPU mirror (satOrbitStateAt) takes for a roster object / a pool record.
SatOrbitElems rosterOrbitElems(const RosterObj &o, double raanAnchorT);
SatOrbitElems poolOrbitElems(const PoolRec &r, uint32_t poolIndex);
// A pool record's tumble axis (sat_orbit.comp poolTumbleAxis: a uniform direction from its index).
glm::dvec3 poolTumbleAxis(uint32_t poolIndex);

// The storm's scripted history: which satellites the Carrington-class storm kills (by altitude and
// inclination), when, and the dead formation members bumping their neighbours. Deterministic.
std::vector<Event> stormSeedEvents(const World &w, double arriveT, uint64_t seed);
} // namespace kessler
