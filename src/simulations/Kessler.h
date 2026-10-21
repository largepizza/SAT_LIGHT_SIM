#pragma once
// Kessler syndrome (branch kessler, 2026-10-16; plan: .plans/KESSLER_PLAN.md). The app-independent
// half: orbit elements from state vectors and back, J2 secular rates, the NASA Standard Breakup
// Model, a drag lifetime estimate and the statistical collision engine (cube method). No Vulkan, no
// SatelliteSim — SatelliteSimKessler.cpp turns its results into satellites.
//
// Everything is deterministic: random numbers come from hashes of (seed, index), never from a
// shared stream, so results do not depend on thread count or evaluation order.

#include <cmath>
#include <cstdint>
#include <deque>
#include <functional>
#include <map>
#include <memory>
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
    int kind = -1;          // a structural piece's kind (0 array, 1 radiator, 2 structure); -1 = by the parent's surface
};

struct BreakupResult
{
    bool catastrophic = false;
    double nReal = 0.0;     // fragments >= LcMin the model predicts
    double nLarge = 0.0;    // of them >= LcRecord (the visible pieces)
    double massKg = 0.0;    // the mass the model breaks up (catastrophic: both objects; else the ejecta)
    std::vector<Fragment> fragments; // the LARGE pieces (Lc >= LcRecord): weight 1 unless capped
    std::vector<Fragment> small;     // statistical samples of the small population (their weights sum to it)
};

// Where a breakup's fragments are cut (round 2: only what can be seen becomes a record).
struct BreakupSizes
{
    double LcMin = 0.1;     // smallest fragment counted (m)
    double LcRecord = 1.0;  // from here up a fragment is a record of its own
    double LcMax = 4.0;     // the largest piece (a fraction of the parent's size: its arrays, radiators, bus)
    int maxLarge = 96;      // records per breakup (more: each stands for several)
    double maxLargeMass = 1e30; // the large pieces' total mass never exceeds this (part of the parent's)
    int nSmallSamples = 16; // statistical orbits for the small population
};

// A collision between a target of mass mT and a projectile of mass mP at relative speed vRel (m/s):
// catastrophic above 40 J/g of specific energy.
BreakupResult breakupCollision(double mT, double mP, double vRel, const BreakupSizes &sz, uint64_t seed);
// An explosion (scale factor S = 1) of an object of mass m.
BreakupResult breakupExplosion(double m, const BreakupSizes &sz, uint64_t seed);

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
constexpr uint32_t kSmallRef = 0xFFFFFFFEu; // the projectile was one of the small fragments (the density grid)
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
    double sizeM = 3.0;     // bounding diameter (the largest piece a breakup leaves scales with it)
    double arrayShare = 0.0, radiatorShare = 0.0; // of its surface: which kinds of large pieces it leaves
    int kitKind = -1;       // a debris kit type: the kind it is (0 array, 1 radiator, 2 structure), else -1
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
    int32_t decaySec = kNever;              // when drag brings it down: a death AT this second is a re-entry
    uint16_t type = 0;
    uint8_t constIdx = 0;
    uint8_t flags = 0;      // 1 = forced tumble (a dead satellite), 2 = fragment
    uint32_t event = 0;     // the event that made it
    uint32_t parent = kNoRef;
};
constexpr uint8_t kRecDead = 1, kRecFragment = 2, kRecFree = 4; // 4: its slot has been released

// A dead roster satellite's tumble: an integer period (s) and a phase from its index, so the GPU can take
// the angle mod one period exactly at any date (sat_orbit.comp deadTumble).
void deadTumble(uint32_t sat, uint32_t &periodS, double &phase0);
// A dead formation member's along-track drift (no station keeping, differential drag): turns of argument of
// latitude after tauSec whole seconds (+ frac) dead. 0.5 k tau^2 with k = +-j x 2^-47 turns/s^2 (j 1..3 from the
// index: ~1-4 km after a day, ~100-400 km after ten), taken mod one turn in exact integer arithmetic so the GPU
// (sat_orbit.comp formationDrift) matches at any age.
double formationDriftTurns(uint32_t sat, uint32_t tauSec, double frac);

enum EventKind : uint8_t
{
    EV_KILL = 0,      // a satellite dies (the storm, damage): it tumbles on its orbit until drag brings it down
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
    uint32_t firstRec = 0, nRec = 0; // nRec: pool records it made (filled by apply); firstRec unused (slots recycle)
    float mProj = 0.0f;              // b == kSmallRef: the small fragment's mass (kg; set by the engine)
    float dustKg = 0.0f;             // fine dust it made: the drawn dust cloud (filled by apply)
    float nSmall = 0.0f;             // small fragments (LcMin .. LcRecord) it added to the density grid
    float dvSmall = 0.0f;            // their rms spread (m/s): the dust cloud's expansion
    float pos[3] = {0, 0, 0};        // where (ECI, m) and how fast (m/s) the breakup happened (filled by apply)
    float vel[3] = {0, 0, 0};
};

// The small fragments as a density field: S statistical orbits per breakup, each spread over its whole orbit
// (orbit-averaged; ECI longitude averaged) into altitude x latitude x heading cells. Counts are fixed point, so
// adding and removing in any order is exact: the engine's draws, and so the history, never depend on order.
struct DebrisGrid
{
    static constexpr int NR = 110, NL = 90, NH = 8, NS = 32; // 20 km x equal-area latitude bands x 45 deg; 32 samples
    static constexpr double kRMin = kRMean + 150000.0, kDR = 20000.0;
    static constexpr double kFix = 4096.0;   // count units per fragment
    std::vector<int64_t> cnt;                // [r][lat][heading]
    std::vector<int64_t> massG;              // [r][lat]: count-weighted mass (grams x kFix)
    void clear();
    void deposit(double a, double e, double incl, double argp, double weight, double massKg, uint64_t seed, int sign);
    // The same deposit into other arrays (a death bucket): adding a bucket to / subtracting it from the grid is exact.
    void depositInto(std::vector<int64_t> &c, std::vector<int64_t> &m, double a, double e, double incl, double argp,
                     double weight, double massKg, uint64_t seed, int sign = 1) const;
    // Collision rate per m^2 of cross-section (1/s per m^2) for an object at r moving at v. `pick` (0..1) chooses
    // the heading bin a hit came from (by its share of the rate); vRelOut / massOut are its speed and the cell's
    // mean fragment mass.
    double rate(const glm::dvec3 &r, const glm::dvec3 &v, double pick, double &vRelOut, double &massOut) const;
    double total() const; // fragments in the grid
};

// The small fragments that drag brings down in one window of time (30 days): their deposits summed, subtracted from
// the grid when the window ends (deaths are thus up to 30 days late). Bounded memory however many fragments there are:
// at most one ~0.7-MB pair of arrays per window ahead (a per-fragment record reached 66M records and ~5 GB by day 150).
struct DeathBucket
{
    std::vector<int64_t> cnt, massG;
};

// Small fragments coming down (round 4): one stream per statistical orbit of the density grid that re-enters inside
// the horizon. Its fragments' re-entries are spread over a window about its decay second (a population of one size in
// one orbit still comes down over days: the atmosphere varies, the orbits spread); the main thread expands a stream
// into individual fireballs, by the hour, as a pure function of time.
// Packed to 24 bytes (tens of millions of them late in a cascade); a breakup's statistical orbits are merged into at
// most kMaxStreamsPerBreakup streams by decay time.
struct SmallStream
{
    int32_t deathSec = 0;   // its decay second (the window's centre), from the epoch
    int32_t bornSec = 0;    // the breakup
    uint16_t halfMin = 60;  // the window's half width (min): 15% of its lifetime (+ the merged spread), 1 h .. 5 days
    uint16_t inclQ = 0, raanQ = 0; // its plane at the breakup (x 2 pi / 65536)
    uint16_t massQ = 0;     // each fragment's mass: 2^(massQ / 2048 - 16) kg
    float count = 0.0f;     // fragments
    float raanDot = 0.0f;   // rad/s
    static constexpr int kMaxStreamsPerBreakup = 4;
    int32_t halfS() const { return (int32_t)halfMin * 60; }
    double incl() const { return inclQ * (6.283185307179586 / 65536.0); }
    double raan() const { return raanQ * (6.283185307179586 / 65536.0); }
    double massKg() const { return std::exp2(massQ / 2048.0 - 16.0); }
    uint32_t seed() const
    {
        uint64_t h = (uint64_t)(uint32_t)deathSec * 0x9E3779B97F4A7C15ull ^ (uint64_t)(uint32_t)bornSec * 0xC2B2AE3D27D4EB4Full ^
                     ((uint64_t)raanQ << 16 | massQ) * 0x165667B19E3779F9ull;
        h ^= h >> 29;
        return (uint32_t)(h ^ (h >> 32));
    }
};
static_assert(sizeof(SmallStream) == 24, "SmallStream packing");

struct WorldSettings
{
    double epoch = 0.0;          // the scenario epoch (J2000 s): born / death seconds count from here
    // Where the world starts (J2000 s, on the step grid from the epoch): the epoch, or earlier when the player broke
    // something up before the CME (2026-10-19). Before the epoch working satellites dodge everything (avoidance 1):
    // only the dead and the debris collide.
    double startT = 0.0;
    double raanAnchorT = 0.0;    // the sim's SSO RAAN anchor (orbitElemsOf)
    double horizonS = 50.0 * 365.25 * 86400.0;
    double stepS = 6.0 * 3600.0;
    double LcMin = 0.1;
    int maxRecordsPerEvent = 96;  // a breakup's large-piece records at an empty pool, tapering as it fills
    double LcRecord = 1.0;        // fragments from here up are records (visible); smaller ones are the grid
    double dustShare = 0.05;      // of the broken-up mass, fine dust (the drawn cloud)
    double clusterHitP = 0.6;     // a catastrophic breakup of a formation member damages each neighbour with this
    int kitType[3][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}; // debris kit types [kind][S, M, L]
    double kitArea[3][3] = {};    // their mean areas (a large piece's area snaps to its class: flux 1)
    uint32_t poolCapacity = 5000000; // LIVE records (slots recycle; GPU ~116 B each with the visible list: ~580 MB full)
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
    std::vector<int32_t> rosterDead;  // second each roster satellite died (kNever): from then on it tumbles,
                                      // never manoeuvres, and drag lowers nothing but its end second
    std::vector<int32_t> rosterEnd;   // end second: destroyed or re-entered (kNever)
    std::vector<int32_t> rosterDecay; // its drag re-entry second once dead (kNever): an end AT it is a re-entry
    // The deployment era (2026-10-20): each roster satellite is there from its born second to its gone second (its
    // first launch; a retired benchmark shell). Not there, it is in no collision and the storm cannot kill it.
    // Empty: always there. Set by the sim (rosterLife) before reset().
    std::vector<int32_t> rosterBorn, rosterGone;
    bool rosterPresent(uint32_t i, int32_t sec) const
    {
        return i >= rosterBorn.size() || (sec >= rosterBorn[i] && sec < rosterGone[i]);
    }
    std::vector<PoolRec> pool;
    std::vector<Event> events;        // applied, in time order
    DebrisGrid grid;                  // the small fragments
    std::map<int32_t, DeathBucket> deathBuckets; // by window index (30 days from the epoch): leave the grid at its end
    static constexpr double kDeathBucketS = 30.0 * 86400.0;
    uint64_t smallAlive = 0;          // statistical orbits in the grid (the benchmark's count)
    std::vector<SmallStream> smallStreams; // new since the last take (the worker publishes and clears them)
    uint64_t smallStreamsMade = 0;
    double frontier = 0.0;            // simulated up to here (J2000 s)
    uint64_t sampleCount = 0;
    double profState = 0.0, profPairs = 0.0, profApply = 0.0, profRefs = 0.0; // seconds spent (the worker logs them)

    // Garbage collection (round 3): a slot is freed kReleaseDelayS after its record's death (re-entry or destruction;
    // the delay lets a fireball finish) and reused by the next record born after that, so the capacity counts LIVE
    // pieces. Each reuse is a new generation of the slot. Deterministic (a LIFO free list fed in time order), so a
    // replay reassigns every slot identically and the event log's references stay valid.
    static constexpr int32_t kReleaseDelayS = 300;
    std::vector<uint32_t> poolGen;   // generation of each slot's current record
    std::vector<uint32_t> poolFree;  // slots whose record is gone, reusable
    struct Release
    {
        int32_t sec;
        uint32_t slot, gen;
        bool operator>(const Release &o) const { return sec != o.sec ? sec > o.sec : slot > o.slot; }
    };
    std::vector<Release> releaseHeap; // a min-heap of pending releases (stale ones are skipped)
    uint64_t recAdded = 0;            // records ever added
    // Change notifications since the last take, in order (the main thread mirrors the pool): a record placed in a
    // slot (its generation) or a record's death changed.
    struct PoolChange
    {
        uint32_t slot, gen;
        int32_t deathSec;
        uint8_t isRecord;
        PoolRec rec; // isRecord: the record as placed (a later change to it is its own entry)
    };
    std::vector<PoolChange> poolChanges;
    std::vector<uint32_t> rosterChanged;                    // satellites whose dead / end second changed

    void reset();
    // The engine's step grid: steps of stepS from the epoch (before it too). Every step starts on it, whatever the
    // worker's chunks: the history does not depend on when the player's time made it pause.
    double stepStart(double t) const { return s.epoch + std::floor((t - s.epoch) / s.stepS) * s.stepS; }
    int64_t stepIndex(double t0) const { return (int64_t)std::llround((t0 - s.epoch) / s.stepS); }

    // ── Rollback (2026-10-19) ───────────────────────────────────────────────────────────────────────
    // A player breakup at time t must not replay the whole history: the world keeps a journal of every change since
    // its oldest kept checkpoint (one per step start, from shortly before the player's time), so it can be rewound
    // to the last step start at or before t exactly (undoing the journal in reverse) and only the step containing t
    // re-run. Bounded: journaling starts at checkpoints the caller asks for, the caller drops old ones, and past
    // journalCap entries the journal is dropped (a breakup then falls back to a full replay).
    struct Checkpoint
    {
        double t = 0.0;
        size_t journal = 0, events = 0, pool = 0; // journal: an absolute index (journalOff + position)
        uint64_t recAdded = 0, smallAlive = 0, smallStreamsMade = 0, sampleCount = 0;
        size_t pendingNext = 0;
    };
    std::vector<Checkpoint> checkpoints;
    size_t journalCap = 1000000; // ~200 MB of entries
    bool wantCheckpoints = false;   // the stepping loops checkpoint each step start from checkpointFrom on
    double checkpointFrom = 1e300;
    void checkpoint(double t, size_t pendingNext);      // at a step start (simulate): journal from here on
    void dropCheckpointsBefore(double t);               // keep the latest at or before t, and the later ones
    void dropJournal();                                 // no rollback any more (until the next checkpoint)
    int checkpointAtOrBefore(double t) const;           // the latest checkpoint at or before t, or -1
    size_t journalSize() const { return journal.size(); }
    // Rewinds to checkpoint k (which is dropped with every later one); returns its pendingNext. The outbox
    // (poolChanges, rosterChanged, smallStreams) is cleared: the caller rebuilds its mirror from the world.
    size_t rollback(int k);
    State stateOf(uint32_t ref, double t) const;
    bool alive(uint32_t ref, double t) const; // exists (a dead satellite still does: it can be hit)
    bool dead(uint32_t ref, double t) const;  // a roster satellite past its death (or any pool record)
    int32_t secOf(double t) const;
    // Applies one event (fills its record range and pushes it to `events`). Returns false (and does
    // nothing) when an object it names is no longer alive at its time.
    bool apply(Event ev);
    // Runs from the frontier to tEnd: due seed / user events (in `pending`, sorted by time) and engine
    // samples. Stops early when *stop turns true.
    void simulate(double tEnd, std::vector<Event> &pending, size_t &pendingNext, const bool *stop);

private:
    uint32_t addRecord(const PoolRec &r);
    void killRoster(uint32_t ref, double t, uint32_t eventIdx);
    void spawnFragments(const BreakupResult &br, uint32_t refA, uint32_t refB, const State &sa, const State &sb,
                        double t, uint32_t eventIdx, uint32_t seed);
    void spawnSmall(const BreakupResult &br, const State &st, double t, uint32_t seed, Event &ev);
    void retireParticles(double t);
    BreakupSizes sizesFor(uint32_t refA, uint32_t refB, int maxLarge) const;
    int kindOf(uint32_t ref, double h) const;
    void addStructuralPieces(BreakupResult &br, uint32_t ref, int side, uint32_t seed) const;
    void damageCluster(uint32_t ref, double t, uint32_t eventIdx, uint32_t seed);
    PoolRec recordFromState(const State &st, double t, uint32_t parentRef, const glm::dvec3 &dv) const;
    double deathTime(const PoolRec &r, double t) const;
    // Seconds until (a, e) with ballistic coefficient cdAm decays, from time t. Once the scripted storm is over
    // (its density factor is exactly 0 from 12 days on) it depends on the orbit and cdAm alone: a lazily filled
    // table (perigee-weighted height x log cdAm, log-interpolated), since a breakup makes up to ~110 of these.
    double lifetime(double t, double a, double e, double cdAm) const;
    mutable std::vector<float> lifeTab; // log(seconds); NaN = not yet computed

    // The journal (rollback): what each change overwrote.
    enum JOp : uint8_t
    {
        J_POOL,      // pool[a] was rec
        J_GEN,       // poolGen[a] was b
        J_RDEAD,     // rosterDead[a] was b
        J_REND,      // rosterEnd[a] was b
        J_RDECAY,    // rosterDecay[a] was b
        J_FREE_PUSH, // a pushed on poolFree
        J_FREE_POP,  // a popped from poolFree
        J_HEAP_PUSH, // (b = sec, a = slot, c = gen) pushed on the release heap
        J_HEAP_POP,  // ... popped from it
        J_GRID,      // a deposit into the grid (d[], seed b): undone by its negative
        J_BUCKET,    // a deposit into death bucket a
        J_RETIRE,    // death bucket a left the grid (bucket)
    };
    struct JEntry
    {
        JOp op = J_POOL;
        uint32_t a = 0, c = 0;
        int64_t b = 0;
        PoolRec rec;
        double d[6] = {0, 0, 0, 0, 0, 0};
        std::shared_ptr<DeathBucket> bucket;
    };
    std::deque<JEntry> journal;
    size_t journalOff = 0;   // entries dropped from the front
    bool journalOn = false;
    void jPool(uint32_t slot)
    {
        if (journalOn)
        {
            JEntry e;
            e.op = J_POOL;
            e.a = slot;
            e.rec = pool[slot];
            journal.push_back(std::move(e));
        }
    }
    void jSimple(JOp op, uint32_t a, int64_t b = 0, uint32_t c = 0)
    {
        if (journalOn)
        {
            JEntry e;
            e.op = op;
            e.a = a;
            e.b = b;
            e.c = c;
            journal.push_back(std::move(e));
        }
    }
    void jDeposit(JOp op, uint32_t key, double a, double ecc, double incl, double argp, double weight, double massKg,
                  uint64_t seed)
    {
        if (journalOn)
        {
            JEntry e;
            e.op = op;
            e.a = key;
            e.b = (int64_t)seed;
            e.d[0] = a, e.d[1] = ecc, e.d[2] = incl, e.d[3] = argp, e.d[4] = weight, e.d[5] = massKg;
            journal.push_back(std::move(e));
        }
    }
    friend void endObject(World &w, uint32_t ref, double t, bool keepWeighted);
};
void endObject(World &w, uint32_t ref, double t, bool keepWeighted);

// The orbit elements the CPU mirror (satOrbitStateAt) takes for a roster object / a pool record.
SatOrbitElems rosterOrbitElems(const RosterObj &o, double raanAnchorT);
SatOrbitElems poolOrbitElems(const PoolRec &r, uint32_t poolIndex);
// A pool record's tumble axis (sat_orbit.comp poolTumbleAxis: a uniform direction from its index).
glm::dvec3 poolTumbleAxis(uint32_t poolIndex);

// Lowers the calling thread's priority (the engine runs beside the frame loop). No-op off Windows.
void lowerThreadPriority();

// The storm's scripted history: which satellites the Carrington-class storm kills (by altitude and
// inclination), when, and the dead formation members bumping their neighbours. Deterministic.
std::vector<Event> stormSeedEvents(const World &w, double arriveT, uint64_t seed);
// The order a world applies its pending events in (from index `from` on): by engine step, then the scripted events,
// the engine's, the player's, then by time. simulate() applies a step's events in that order.
void sortPending(std::vector<Event> &pending, size_t from, const World &w);
} // namespace kessler
