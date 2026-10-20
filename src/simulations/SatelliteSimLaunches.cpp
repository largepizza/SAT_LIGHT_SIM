// Launches, the sim side (2026-10-18, branch kessler; plan .plans/LAUNCHES_PLAN.md; data/launches.json).
//
// A roster satellite is a POSITION in its constellation; its occupant changes. Every orbital plane of a constellation
// that a program keeps flying is refreshed once per design life, in batches of a launch's payload a few days apart.
// For batch b of refresh k of plane p the arrival time is t_A = (k + phase_p) L + b gap (phase_p hashed): a rocket
// lifts off from a site the plane passes over shortly before t_A - raise (the launch window), deploys its train low,
// the train climbs to its slots and arrives at t_A; there each new satellite takes over its roster slot (which never
// changes on screen) and the satellite it replaces starts down, re-entering weeks later. So the roster's size never
// changes and launches balance re-entries one for one.
//
// Everything is a pure function of sim time: no history. Trains and retirees are drawn as satellites: Keplerian
// records in kesslerBuf's launch block (sat_orbit.comp: LAUNCH_CAP), osculating elements of their state rewritten
// as they move (a few tens of thousands at most). Rockets are point sprites in kFireBuf, their flames emissive
// Gaussians and their exhaust scattering Gaussians in the dust list (cloud_march.comp).
#include "SatelliteSim.h"
#include "Launches.h"
#include "../Log.h"
#include "../Paths.h"
#include "../VulkanContext.h"
#include "../UIRenderer.h"
#include "UIPalette.h"
#include "clay.h"

#include <nlohmann/json.hpp>

#include <cstring>
#include <filesystem>

namespace launchsim
{
using launch::Track;
constexpr uint32_t kChunk = 8;
constexpr uint32_t kLaunchCapL = 131072; // == SatelliteSim::kLaunchCap (asserted in launchInitBlock)
constexpr uint32_t kNumChunks = kLaunchCapL / kChunk;
constexpr double kDay = 86400.0;
constexpr double kYear = 365.25 * 86400.0;
constexpr double kR = satphot::kEarthRadiusM;
constexpr double kEndAltM = 200000.0;   // a retiree is lowered to here, then re-enters
constexpr double kPlumeAgeLow = 2400.0; // how long the exhaust is drawn
constexpr double kPlumeAgeHigh = 3600.0;

glm::dvec3 rotZ(const glm::dvec3 &v, double th)
{
    const double c = std::cos(th), s = std::sin(th);
    return glm::dvec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}
glm::dvec3 ecefToEci(const glm::dvec3 &v, double t) { return rotZ(v, earthRotationAngle(t)); }
glm::dvec3 eciToEcef(const glm::dvec3 &v, double t) { return rotZ(v, -earthRotationAngle(t)); }
double wrap2pi(double a)
{
    a = std::fmod(a, glm::two_pi<double>());
    return a < 0.0 ? a + glm::two_pi<double>() : a;
}
double h01(uint64_t seed, uint64_t idx, uint32_t ch) { return launch::hash01(seed, idx, ch); }
// A program's refresh period (s): the design life x the setting, or for a crew rotation its own cadence (unscaled).
double progLifeS(const launch::Program &pg, double scale)
{
    return pg.crew ? std::max(0.05, pg.lifeYears) * kYear : std::max(0.25, pg.lifeYears * scale) * kYear;
}

struct LPlane
{
    int prog = -1;
    uint32_t constIdx = 0;
    std::vector<uint32_t> sats; // by argument of latitude
    double phase = 0.0;         // [0, 1)
    uint64_t seed = 0;
    int nb = 1;                 // batches per refresh
    double gap = 2.0 * kDay;
    double inclRad = 0.0;
};

struct LMember
{
    uint32_t sat = 0;
    double rSlot = 0.0, nSlot = 0.0;
    double sRaise = 0.0;  // the climb starts (s, J2000)
    double tRamp = 0.0;   // the climb lasts
    double eps = 0.0;     // phasing residual (rad), spread over the raise
    double tEnd = 0.0;    // the retiree's re-entry
    glm::dvec3 depOff{0.0}; // at its release, the dispenser's position less the member's (radial, along, cross): eased out
    double tRel = 0.0;      // its release from the dispenser (s, J2000): the train leaves one after another
    glm::dvec3 sepDir{0.0}; // its separation drift (radial, along, cross; m/s) from the dispenser, eased out with depOff
};

struct LBatch
{
    int plane = -1, b = 0;
    int64_t k = 0;
    int prog = -1, veh = -1, site = -1;
    bool valid = false;
    std::string why;
    double tA = 0.0, tL = 0.0, tD = 0.0;
    double rPark = 0.0, nPark = 0.0, thIns = 0.0, t2 = 0.0;
    glm::dvec3 U0{1, 0, 0}, A0{0, 1, 0}, N{0, 0, 1}; // ECI at tL: the pad in the plane, prograde, normal
    glm::dvec3 G0{1, 0, 0}, Eaz{0, 1, 0};             // Earth-fixed: the pad, the ground azimuth
    double azDeg = 0.0;
    uint64_t seed = 0;
    std::vector<LMember> mem;
    std::vector<uint32_t> chunks[2]; // GPU chunks: 0 the train, 1 the retirees
    double tw[2] = {-1e300, -1e300}; // when each kind's records were last all written
    uint32_t nUp[2] = {0, 0};        // members up then
    std::vector<double> ends;        // the retirees' re-entries, sorted
    uint32_t touched = 0;
    double tDeorbBurn = 0.0, tStageRE = 0.0, tStageGone = 0.0;
    // The upper stage's deorbit (every stage burns, expendable or returning): a burn of burnDur from tDeorbBurn that
    // puts it on an ellipse with its apoapsis there (angle thApo in the plane, from U0); it reaches the entry interface
    // (100 km, angle thEI, speed vEI) at tStageGone. Before the burn it turns retrograde (tTurn on).
    double burnDur = 0.0, aE = 0.0, eE = 0.0, nE = 0.0, thApo = 0.0, thEI = 0.0, vEI = 7800.0, vrEI = 0.0;
    bool vents = false;
    bool parcelsBuilt = false;
    std::vector<launch::Parcel> parcels;
    double tVisEnd = 0.0;
    int shifts = 0, rivals = 0; // pad deconfliction: passes skipped, candidates checked
};

struct SlotInfo
{
    uint64_t owner = 0; // batch key * 2 + kind, 0 = free
    uint16_t m = 0;
    bool live = false;  // a record is up (or due)
    bool shown = false; // the GPU holds a live record
    double tw = -1e300; // when it was written (sim time)
};

uint64_t batchKey(int plane, int64_t k, int b)
{
    return ((uint64_t)(uint32_t)plane << 40) | ((uint64_t)(k + (1 << 19)) & 0xFFFFFull) << 20 | (uint64_t)(b & 0xFFFFF);
}
} // namespace launchsim
using namespace launchsim;

struct SatelliteSim::LaunchState
{
    launch::Catalog cat;
    bool tried = false, loaded = false;
    std::string err;
    bool planesBuilt = false;
    std::vector<LPlane> planes;
    std::vector<Track> booster; // per vehicle
    std::map<std::pair<int, int>, Track> upper; // (vehicle, park km)
    std::unordered_map<uint64_t, LBatch> batches;
    // A batch's launch before pad deconfliction (vehicle, site, the plane's next pass over it), cached by batch key.
    struct Cand
    {
        int veh = -1, site = -1;
        double tL = -1.0, az = 0.0;
    };
    std::unordered_map<uint64_t, Cand> cands;
    std::vector<std::vector<int>> planesBySite; // the planes that may launch from each site
    std::vector<uint32_t> freeChunks;
    std::vector<SlotInfo> slots;
    std::vector<SatOrbitElems> elems;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    void *stageMapped = nullptr;
    static constexpr uint32_t kStageRecs = 16384;
    uint32_t gpuGen = UINT32_MAX;
    bool clearGpu = true;         // zero the GPU block before anything is written (a reset)
    float lifeScale = -1.0f;      // the planes were built for this "life_scale"
    int afterStorm = -1;          // ... and this "after_storm"
    uint32_t frame = 0;
    bool active = false; // anything up on the GPU
    std::vector<uint64_t> live; // batch keys touched this frame (in a stable order)
    size_t cursor = 0;
    // The settings section's readouts (refreshed every few seconds).
    double uiListAt = -1e300, uiListSim = -1e300;
    std::string uiNext;
    int uiCount24 = 0;
    // Readouts (this frame)
    struct PlumeDbg
    {
        double rangeKm, altKm, sigKm, Sigma, tauPeak, age;
        glm::dvec3 col;
        double amb;
    };
    std::vector<PlumeDbg> plumeDbg;
    uint32_t nRockets = 0, nTrain = 0, nRetire = 0, nPlumes = 0, nReentries = 0, nWrites = 0, nBatches = 0;

    const Track &upperTrackFor(int veh, double parkKm)
    {
        const std::pair<int, int> key{veh, (int)std::lround(parkKm)};
        auto it = upper.find(key);
        if (it != upper.end())
            return it->second;
        return upper.emplace(key, launch::upperTrack(cat.vehicles[veh], parkKm * 1000.0)).first->second;
    }
};

void SatelliteSim::LaunchStateDelete::operator()(LaunchState *s) const { delete s; }

// ── Setup ───────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::launchInitBlock()
{
    static_assert(kLaunchCap == kLaunchCapL, "the launch block's size");
    // Every launch-block entry empty (no constellation: the per-constellation scans skip it).
    for (uint32_t s = 0; s < kLaunchCap && activeSatCount + s < satOrbits.size(); ++s)
    {
        SatOrbit &o = satOrbits[activeSatCount + s];
        o = SatOrbit{};
        o.constIdx = UINT32_MAX;
        o.launchSlot = (int32_t)s;
    }
    if (!launchState)
        launchState.reset(new LaunchState());
    LaunchState &L = *launchState;
    // The catalog stays; the roster may have changed (planes, batches, slots go).
    L.planesBuilt = false;
    L.planes.clear();
    L.batches.clear();
    L.cands.clear();
    L.planesBySite.clear();
    L.slots.assign(kLaunchCap, SlotInfo{});
    L.elems.assign(kLaunchCap, SatOrbitElems{});
    L.freeChunks.clear();
    for (uint32_t c = kNumChunks - 1; c-- > 0;) // the last chunk holds the transient selection's slot
        L.freeChunks.push_back(c);
    if (activeSatCount + kLaunchCap - 1 < satOrbits.size())
        satOrbits[activeSatCount + kLaunchCap - 1].typeIdx = UINT32_MAX; // no model: Track only
    if (selIsTransient())
    {
        selectedSatIndex = -1;
        selTransientKey = 0;
    }
    L.gpuGen = UINT32_MAX;
    L.clearGpu = true; // whatever the GPU block holds goes (records of the state just dropped)
    L.active = false;
    L.live.clear();
}

void SatelliteSim::launchDestroy(VkDevice device)
{
    if (!launchState)
        return;
    LaunchState &L = *launchState;
    if (L.stage)
    {
        vkDestroyBuffer(device, L.stage, nullptr);
        vkFreeMemory(device, L.stageMem, nullptr);
        L.stage = VK_NULL_HANDLE;
        L.stageMapped = nullptr;
    }
}

struct LaunchImpl
{
static void ensureCatalog(SatelliteSim::LaunchState &L)
{
    if (L.tried)
        return;
    L.tried = true;
    const std::string path = (std::filesystem::path(Paths::exeDir()) / "launches.json").string();
    L.loaded = L.cat.load(path, L.err);
    Log::line(L.loaded ? "launches: " + std::to_string(L.cat.programs.size()) + " programs, " +
                             std::to_string(L.cat.vehicles.size()) + " vehicles, " +
                             std::to_string(L.cat.sites.size()) + " sites"
                       : "launches: " + L.err);
    if (L.loaded)
        for (const launch::Vehicle &v : L.cat.vehicles)
            L.booster.push_back(launch::boosterTrack(v));
}

// ── Mass of a satellite type (for its re-entry) ─────────────────────────────────────────────────
static double launchTypeMass(const SatelliteType &ty)
{
    if (ty.massKg > 0.0f)
        return ty.massKg;
    const std::string &m = ty.modelId;
    if (m.find("spacex_ai") != std::string::npos)
        return 2500.0;
    if (m.find("starlink_v3") != std::string::npos)
        return 1900.0;
    if (m.find("starlink") != std::string::npos)
        return 800.0;
    if (m.find("oneweb") != std::string::npos)
        return 150.0;
    return 600.0;
}

// Groups each programme's constellations into planes (raan, inclination, altitude), each sorted by phase.
static void buildPlanes(SatelliteSim::LaunchState &L, const std::vector<SatOrbit> &orbits, uint32_t rosterCount,
                        const std::vector<ConstellationConfig> &consts, float lifeScale)
{
    L.planes.clear();
    for (size_t c = 0; c < consts.size(); ++c)
    {
        int prog = -1;
        for (size_t p = 0; p < L.cat.programs.size() && prog < 0; ++p)
            for (const std::string &m : L.cat.programs[p].match)
                if (consts[c].name.find(m) != std::string::npos)
                {
                    prog = (int)p;
                    break;
                }
        if (prog < 0 || consts[c].orbitCount < (L.cat.programs[prog].crew ? 1u : 2u))
            continue;
        const launch::Program &pg = L.cat.programs[prog];
        std::map<uint64_t, std::vector<uint32_t>> groups;
        const uint32_t i0 = consts[c].orbitStart, i1 = std::min(i0 + consts[c].orbitCount, rosterCount);
        for (uint32_t i = i0; i < i1; ++i)
        {
            const SatOrbit &o = orbits[i];
            const int64_t ra = (int64_t)std::lround(glm::degrees(wrap2pi((double)o.raan))) % 360;
            const int64_t inc = (int64_t)std::lround(glm::degrees((double)o.incl) * 4.0);
            const int64_t alt = (int64_t)std::lround(o.altM / 5000.0);
            const uint64_t key = ((uint64_t)ra << 40) | ((uint64_t)inc << 20) | (uint64_t)(alt & 0xFFFFF);
            groups[key].push_back(i);
        }
        const int cluster = std::max(1, consts[c].clusterSize);
        const int payload = std::max(cluster, pg.payload / cluster * cluster);
        const double life = progLifeS(pg, (double)lifeScale);
        for (auto &g : groups)
        {
            LPlane pl;
            pl.prog = prog;
            pl.constIdx = (uint32_t)c;
            pl.sats = std::move(g.second);
            std::stable_sort(pl.sats.begin(), pl.sats.end(),
                             [&](uint32_t a, uint32_t b) { return orbits[a].u0 < orbits[b].u0; });
            pl.seed = (uint64_t)c * 0x9E3779B97F4A7C15ull ^ g.first * 0xC2B2AE3D27D4EB4Full;
            pl.phase = h01(pl.seed, 1, 1);
            pl.nb = (int)((pl.sats.size() + (size_t)payload - 1) / (size_t)payload);
            pl.gap = std::min(pg.batchGapDays * kDay, life / std::max(pl.nb, 1));
            pl.inclRad = orbits[pl.sats[0]].incl;
            L.planes.push_back(std::move(pl));
        }
    }
    L.planesBySite.assign(L.cat.sites.size(), {});
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        std::vector<bool> seen(L.cat.sites.size(), false);
        for (int v : L.cat.programs[L.planes[pi].prog].vehicles)
            for (int st : L.cat.vehicles[v].sites)
                if (!seen[st])
                {
                    seen[st] = true;
                    L.planesBySite[st].push_back((int)pi);
                }
    }
    L.planesBuilt = true;
    L.lifeScale = lifeScale;
    char m[160];
    snprintf(m, sizeof(m), "launches: %zu planes in %zu programs", L.planes.size(), L.cat.programs.size());
    Log::line(m);
}

// The next time after t0 that the plane (normal N, ECI) passes over the site: both passes a day; the one whose
// prograde ground azimuth the site allows. Returns < 0 if none within two days.
static double nextCrossing(const launch::Site &site, const std::function<glm::dvec3(double)> &normalAt, double t0,
                           double &azOut)
{
    const double w = satphot::kOmegaEarth;
    for (double tb = t0; tb < t0 + 2.0 * kDay; tb += 0.5 * kDay)
    {
        glm::dvec3 N = normalAt(tb);
        // N . Rz(th) s = c (Nx sx + Ny sy) + s (Ny sx - Nx sy) + Nz sz = Rr cos(th - ph) + C
        const glm::dvec3 s = site.ecef;
        const double A = N.x * s.x + N.y * s.y, B = N.y * s.x - N.x * s.y, C = N.z * s.z;
        const double Rr = std::sqrt(A * A + B * B);
        if (Rr < 1e-9 || std::abs(C) > Rr)
            return -1.0; // the plane never passes over this latitude
        const double ph = std::atan2(B, A), dth = std::acos(std::clamp(-C / Rr, -1.0, 1.0));
        double best = -1.0;
        for (int sgn = -1; sgn <= 1; sgn += 2)
        {
            const double thS = ph + sgn * dth;
            const double th0 = earthRotationAngle(tb);
            double t = tb + wrap2pi(thS - th0) / w;
            // Refine with the normal at that time (an SSO plane turns ~1 deg a day).
            for (int it = 0; it < 2; ++it)
            {
                const glm::dvec3 Nt = normalAt(t);
                const double At = Nt.x * s.x + Nt.y * s.y, Bt = Nt.y * s.x - Nt.x * s.y, Ct = Nt.z * s.z;
                const double Rt = std::sqrt(At * At + Bt * Bt);
                if (Rt < 1e-9 || std::abs(Ct) > Rt)
                    break;
                const double ths = std::atan2(Bt, At) + sgn * std::acos(std::clamp(-Ct / Rt, -1.0, 1.0));
                double d = std::remainder(ths - earthRotationAngle(t), glm::two_pi<double>());
                t += d / w;
                N = Nt;
            }
            if (t < t0)
                continue;
            // Prograde at the pad: N x U; its azimuth on the ground.
            const glm::dvec3 U = ecefToEci(s, t);
            const glm::dvec3 Ap = glm::normalize(glm::cross(normalAt(t), U));
            const glm::dvec3 Ae = eciToEcef(Ap, t);
            const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), s));
            const glm::dvec3 north = glm::cross(s, east);
            double az = glm::degrees(std::atan2(glm::dot(Ae, east), glm::dot(Ae, north)));
            if (az < 0.0)
                az += 360.0;
            double lo = site.azMinDeg, hi = site.azMaxDeg;
            double a = az;
            if (lo < 0.0 && a > 180.0)
                a -= 360.0;
            if (a < lo || a > hi)
                continue;
            if (best < 0.0 || t < best)
            {
                best = t;
                azOut = az;
            }
        }
        if (best >= 0.0)
            return best;
    }
    return -1.0;
}

// ── Batches ─────────────────────────────────────────────────────────────────────────────────────
static double secOfKessler(const SatelliteSim &s, double t) { return std::floor(t - s.kesslerEpoch()); }

static uint64_t batchSeed(const LPlane &pl, int64_t k, int b)
{
    return pl.seed ^ ((uint64_t)(k + 77) * 0x2545F4914F6CDD1Dull) ^ ((uint64_t)b * 0x9E3779B97F4A7C15ull);
}

// The plane's normal at t (from the batch's first member's slot).
static glm::dvec3 planeNormal(const SatelliteSim &sim, const LPlane &pl, int b, int payload, double t)
{
    const size_t m0 = std::min(pl.sats.size() - 1, (size_t)b * (size_t)payload);
    const SatOrbitState st = satOrbitStateAt(sim.rosterElemsOf(sim.satOrbits[pl.sats[m0]]), t);
    return glm::normalize(glm::cross(st.posEci, st.velEci));
}

static int payloadOf(const SatelliteSim &sim, const SatelliteSim::LaunchState &L, const LPlane &pl)
{
    const launch::Program &pg = L.cat.programs[pl.prog];
    const int cluster = std::max(1, sim.constellations[pl.constIdx].clusterSize);
    return std::max(cluster, pg.payload / cluster * cluster);
}

// A batch's launch before deconfliction: a hashed vehicle and site, the first the plane passes over after the nominal
// time (arrival - raise - the flight to deployment). Cached.
static SatelliteSim::LaunchState::Cand candidate(const SatelliteSim &sim, SatelliteSim::LaunchState &L, int plane,
                                                 int64_t k, int b)
{
    const uint64_t key = batchKey(plane, k, b);
    auto it = L.cands.find(key);
    if (it != L.cands.end())
        return it->second;
    SatelliteSim::LaunchState::Cand c;
    const LPlane &pl = L.planes[plane];
    const launch::Program &pg = L.cat.programs[pl.prog];
    const double lifeS = progLifeS(pg, (double)sim.launchLifeScale);
    const uint64_t seed = batchSeed(pl, k, b);
    const double tA = (k + pl.phase) * lifeS + b * pl.gap;
    const double raise = (pg.raiseDays[0] + (pg.raiseDays[1] - pg.raiseDays[0]) * h01(seed, 2, 1)) * kDay;
    const double tNom = tA - raise;
    const int payload = payloadOf(sim, L, pl);
    auto normalAt = [&](double t) { return planeNormal(sim, pl, b, payload, t); };
    const size_t nv = pg.vehicles.size();
    for (size_t vi = 0; vi < nv && c.tL < 0.0; ++vi)
    {
        const int veh = pg.vehicles[(vi + (size_t)(h01(seed, 3, 1) * nv)) % nv];
        const launch::Vehicle &vh = L.cat.vehicles[veh];
        const double lead = vh.t2 + vh.deployAfter;
        const size_t ns = vh.sites.size();
        for (size_t si = 0; si < ns && c.tL < 0.0; ++si)
        {
            const int site = vh.sites[(si + (size_t)(h01(seed, 4, 1) * ns)) % ns];
            double a = 0.0;
            const double t = nextCrossing(L.cat.sites[site], normalAt, tNom - lead, a);
            if (t >= 0.0)
            {
                c.tL = t;
                c.az = a;
                c.veh = veh;
                c.site = site;
            }
        }
    }
    if (L.cands.size() > 200000)
        L.cands.clear();
    L.cands.emplace(key, c);
    return c;
}

// Builds a batch: where and when it launches, its trajectory frame, each member's climb and its predecessor's fall.
static void buildBatch(SatelliteSim &sim, SatelliteSim::LaunchState &L, LBatch &B, const LPlane &pl, double lifeS)
{
    const launch::Program &pg = L.cat.programs[pl.prog];
    B.prog = pl.prog;
    B.seed = batchSeed(pl, B.k, B.b);
    B.tA = (B.k + pl.phase) * lifeS + B.b * pl.gap;
    // Members: a contiguous run of the plane's slots.
    const int cluster = std::max(1, sim.constellations[pl.constIdx].clusterSize);
    const int payload = std::max(cluster, pg.payload / cluster * cluster);
    const size_t m0 = (size_t)B.b * (size_t)payload, m1 = std::min(pl.sats.size(), m0 + (size_t)payload);
    if (m0 >= m1)
    {
        B.why = "empty";
        return;
    }
    const SatOrbitElems e0 = sim.rosterElemsOf(sim.satOrbits[pl.sats[m0]]);
    auto normalAt = [&](double t) {
        const SatOrbitState st = satOrbitStateAt(e0, t);
        return glm::normalize(glm::cross(st.posEci, st.velEci));
    };
    // Vehicle, site and time: the candidate (the plane's first pass over a site its vehicle flies from), then pad
    // deconfliction: a launch whose pad another launch (of higher priority, by hash) takes within 90 min takes the
    // plane's next pass instead. The others' times are their candidates, never their deconflicted times: no recursion,
    // still a pure function of time.
    const SatelliteSim::LaunchState::Cand c0 = candidate(sim, L, B.plane, B.k, B.b);
    if (c0.tL < 0.0)
    {
        B.why = "no site reaches the plane";
        return;
    }
    B.veh = c0.veh;
    B.site = c0.site;
    double tL = c0.tL, az = c0.az;
    {
        // The launches competing for this pad around this pass (their candidates): this one waits as many passes as
        // there are rivals of higher priority (a hash), so the cluster goes out on successive passes (~a day apart).
        const uint64_t self = batchKey(B.plane, B.k, B.b);
        const double myPri = h01(self, 9, 9);
        int rank = 0;
        for (int q : L.planesBySite[B.site])
        {
            const LPlane &pq = L.planes[q];
            const launch::Program &gq = L.cat.programs[pq.prog];
            const double lifeQ = progLifeS(gq, (double)sim.launchLifeScale);
            const double lo = tL + (gq.raiseDays[0] - 1.0) * kDay, hi = tL + (gq.raiseDays[1] + 3.0) * kDay;
            const int64_t k0 = (int64_t)std::floor((lo - (pq.nb - 1) * pq.gap) / lifeQ - pq.phase);
            const int64_t k1 = (int64_t)std::floor(hi / lifeQ - pq.phase);
            for (int64_t k = k0; k <= k1; ++k)
            {
                const double base = (k + pq.phase) * lifeQ;
                const int b0 = std::max(0, (int)std::ceil((lo - base) / pq.gap));
                const int b1 = std::min(pq.nb - 1, (int)std::floor((hi - base) / pq.gap));
                for (int b = b0; b <= b1; ++b)
                {
                    const uint64_t other = batchKey(q, k, b);
                    if (other == self)
                        continue;
                    const SatelliteSim::LaunchState::Cand co = candidate(sim, L, q, k, b);
                    ++B.rivals;
                    if (co.site != B.site)
                        continue;
                    if (std::abs(co.tL - c0.tL) < 5400.0 && h01(other, 9, 9) > myPri)
                        ++rank;
                }
            }
        }
        // A rank first moves the launch to the vehicle's next pad that reaches the plane, then (past the pads) to
        // later passes.
        if (rank > 0)
        {
            const launch::Vehicle &vh0 = L.cat.vehicles[B.veh];
            const double lead = vh0.t2 + vh0.deployAfter;
            const double tNom = B.tA - (pg.raiseDays[0] + (pg.raiseDays[1] - pg.raiseDays[0]) * h01(B.seed, 2, 1)) * kDay;
            std::vector<std::pair<int, double>> pads; // (site, its first pass), the candidate's pad first
            std::vector<double> pAz;
            pads.push_back({B.site, c0.tL});
            pAz.push_back(c0.az);
            for (int st : vh0.sites)
            {
                if (st == B.site)
                    continue;
                double a2 = 0.0;
                const double t2 = nextCrossing(L.cat.sites[st], normalAt, tNom - lead, a2);
                if (t2 >= 0.0)
                {
                    pads.push_back({st, t2});
                    pAz.push_back(a2);
                }
            }
            const int np = (int)pads.size();
            B.site = pads[rank % np].first;
            tL = pads[rank % np].second;
            az = pAz[rank % np];
            for (int r = 0; r < rank / np; ++r)
            {
                double a2 = 0.0;
                const double t2 = nextCrossing(L.cat.sites[B.site], normalAt, tL + 5400.0, a2);
                if (t2 < 0.0)
                {
                    B.why = "pad taken";
                    return;
                }
                tL = t2;
                az = a2;
            }
            B.shifts = rank;
        }
    }
    // Kessler: launches stand down at the storm. By default for good: the cascade's shells are being ground into
    // debris, and no operator would send replacements into them (a slot the storm or the cascade kills is not refilled
    // either way). "After the storm: resume" brings them back three weeks on (the trains are not engine objects: they
    // do not collide, whatever the debris).
    if (sim.kesslerEnabled && tL >= sim.kesslerEpoch() &&
        (sim.launchAfterStorm == 0 || tL < sim.kesslerEpoch() + 21.0 * kDay))
    {
        B.why = sim.launchAfterStorm == 0 ? "stood down since the storm" : "stand-down after the storm";
        return;
    }
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const launch::Site &st = L.cat.sites[B.site];
    B.tL = tL;
    B.azDeg = az;
    B.t2 = std::max(vh.t2, vh.t1 + 60.0);
    B.tD = tL + B.t2 + vh.deployAfter;
    if (pg.crew)
    {
        // A crew flight: docking raise_days after lift-off (the capsule's rendezvous is not drawn).
        B.tA = std::max(tL + (pg.raiseDays[0] + (pg.raiseDays[1] - pg.raiseDays[0]) * h01(B.seed, 2, 1)) * kDay,
                        B.tD + 600.0);
    }
    else if (B.tD >= B.tA - 0.5 * kDay)
    {
        B.why = "launch window too late";
        return;
    }
    // Frames.
    B.N = normalAt(tL);
    const glm::dvec3 S = ecefToEci(st.ecef, tL);
    B.U0 = glm::normalize(S - glm::dot(S, B.N) * B.N);
    B.A0 = glm::normalize(glm::cross(B.N, B.U0));
    B.G0 = st.ecef;
    {
        const glm::dvec3 Ae = eciToEcef(B.A0, tL);
        B.Eaz = glm::normalize(Ae - glm::dot(Ae, B.G0) * B.G0);
    }
    B.rPark = kR + pg.parkKm * 1000.0;
    B.nPark = launch::meanMotion(B.rPark);
    const Track &up = L.upperTrackFor(B.veh, pg.parkKm);
    B.thIns = up.at(B.t2).x / kR;
    const double thD = B.thIns + B.nPark * vh.deployAfter;
    // Members.
    const double T = B.tA - B.tD;
    B.mem.clear();
    for (size_t m = m0; m < m1 && !pg.crew; ++m) // a crew flight carries no train
    {
        LMember mb;
        mb.sat = pl.sats[m];
        const SatOrbitElems e = sim.rosterElemsOf(sim.satOrbits[mb.sat]);
        mb.rSlot = e.rSatM;
        mb.nSlot = e.meanMot > 0.0 ? e.meanMot : launch::meanMotion(e.rSatM);
        const glm::dvec3 r = satOrbitStateAt(e, B.tD).posEci;
        const double thSlot = std::atan2(glm::dot(r, B.A0), glm::dot(r, B.U0));
        const uint64_t ms = B.seed ^ ((uint64_t)m * 0x9E3779B97F4A7C15ull);
        mb.tRamp = T * (0.55 + 0.25 * h01(ms, 5, 1));
        const double W = T - mb.tRamp;
        const double rampLag = launch::rampPhase(B.rPark, mb.rSlot, mb.tRamp, mb.tRamp) - mb.nSlot * mb.tRamp;
        const double dn = B.nPark - mb.nSlot;
        // The train leaves the stage as a tight string (~150 m apart), then each member waits its own time in the
        // parking orbit so that the climb ends exactly on its slot.
        const double lag0 = wrap2pi(thSlot - thD + 2.2e-5 * (double)(m - m0));
        const double lo = dn * 0.25 * kDay + rampLag;
        double lag = lag0;
        if (dn > 0.0)
            while (lag < lo)
                lag += glm::two_pi<double>();
        const double sNeeded = dn > 1e-12 ? (lag - rampLag) / dn : 0.0;
        if (dn > 1e-12 && sNeeded >= 0.0 && sNeeded <= W)
        {
            mb.sRaise = B.tD + sNeeded;
            mb.eps = 0.0;
        }
        else
        {
            const double s = dn > 1e-12 ? std::clamp(sNeeded, 0.0, W) : 0.0;
            mb.sRaise = B.tD + s;
            mb.eps = lag - (dn * s + rampLag);
        }
        mb.tEnd = B.tA + (pg.deorbitDays[0] + (pg.deorbitDays[1] - pg.deorbitDays[0]) * h01(ms, 6, 1)) * kDay;
        B.mem.push_back(mb);
    }
    // The upper stage after deployment: a deorbit burn 30-90 min later onto an ellipse whose perigee is in the air,
    // its apoapsis at the burn; it reaches the entry interface (100 km) about half an orbit on. Every stage does it:
    // an expendable one re-enters as a fireball, a returning one (Starship's ship) flies its entry glow. Until
    // 2026-10-19 only the expendable ones burned (40 s, while the stage sank 30 km on its circle) and Starship's ship
    // jumped from its parking orbit to 100 km at the entry interface 40 min after deploying.
    B.tDeorbBurn = B.tD + 1800.0 + 3600.0 * h01(B.seed, 7, 1);
    {
        const double ra = B.rPark, rp = kR + (vh.upperDeorbit ? 20000.0 : 45000.0); // the ship enters shallower
        B.aE = 0.5 * (ra + rp);
        B.eE = (ra - rp) / (ra + rp);
        B.nE = launch::meanMotion(B.aE);
        const double dv = std::sqrt(launch::kGM / ra) - std::sqrt(launch::kGM * (2.0 / ra - 1.0 / B.aE));
        // One engine, throttled (~12% of the stage's full thrust): a 12-60 s burn.
        const double acc = 0.12 * vh.flow2 * vh.vex2 / std::max(vh.upperMassKg, 1000.0);
        B.burnDur = std::clamp(dv / std::max(acc, 0.1), 12.0, 60.0);
        B.thApo = B.thIns + B.nPark * (B.tDeorbBurn - tL - B.t2);
        const double rEI = kR + 100000.0;
        const double cE = std::clamp((1.0 - rEI / B.aE) / std::max(B.eE, 1e-6), -1.0, 1.0);
        const double E = glm::two_pi<double>() - std::acos(cE); // after the apoapsis, falling toward the perigee
        const double M = E - B.eE * std::sin(E);
        const double nu = 2.0 * std::atan2(std::sqrt(1.0 + B.eE) * std::sin(0.5 * E), std::sqrt(1.0 - B.eE) * std::cos(0.5 * E));
        B.thEI = B.thApo + (nu < 0.0 ? nu + glm::two_pi<double>() : nu) - glm::pi<double>();
        B.vEI = std::sqrt(launch::kGM * (2.0 / rEI - 1.0 / B.aE));
        B.vrEI = std::sqrt(launch::kGM / (B.aE * (1.0 - B.eE * B.eE))) * B.eE * std::sin(nu); // < 0: falling
        B.tStageRE = B.tDeorbBurn + (M - glm::pi<double>()) / B.nE;
    }
    B.tStageGone = B.tStageRE;
    B.vents = h01(B.seed, 8, 1) < vh.ventChance;
    const double boosterEnd = tL + L.booster[B.veh].tEnd;
    B.tVisEnd = std::max({boosterEnd + kPlumeAgeLow, B.tStageGone + kPlumeAgeHigh, tL + B.t2 + kPlumeAgeHigh,
                          B.tStageGone + 480.0});
    B.ends.clear();
    for (const LMember &mb : B.mem)
        B.ends.push_back(mb.tEnd);
    std::sort(B.ends.begin(), B.ends.end());
    // Each member leaves the dispenser in turn (deployIntervalS apart): at its release it is AT the dispenser — in the
    // payload bay's door, near the nose on the zenith side (the stage flies nose prograde, belly to nadir) — and drifts
    // away from it at a fraction of a m/s, mostly upward; memberState eases that offset out over two days. (Until
    // 2026-10-19 the whole train appeared at once at deployment, a string 120 m apart behind the stage.)
    {
        const bool big = vh.upperMassKg > 50000.0; // Starship's ship: a 9-m body, the bay door high on the side
        const double noseOff = big ? 31.0 : 9.0, sideOff = big ? 4.6 : 1.6;
        for (size_t mi = 0; mi < B.mem.size(); ++mi)
        {
            LMember &mb = B.mem[mi];
            mb.tRel = B.tD + vh.deployIntervalS * (double)mi;
            glm::dvec3 Pst, Pb;
            if (!rocketPos(L, B, 1, mb.tRel - B.tL, Pst) || !rocketPos(L, B, 1, mb.tRel - B.tL - 1.0, Pb))
                continue;
            mb.depOff = glm::dvec3(0.0);
            glm::dvec3 r, v;
            if (!memberState(sim, B, mb, 0, mb.tRel, r, v))
                continue;
            const glm::dvec3 nose = glm::normalize(Pst - Pb), up = glm::normalize(Pst);
            const glm::dvec3 disp = Pst + noseOff * nose + sideOff * up;
            const glm::dvec3 U = r / glm::length(r);
            const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
            const glm::dvec3 A = glm::cross(Cn, U);
            const glm::dvec3 d = disp - r;
            mb.depOff = glm::dvec3(glm::dot(d, U), glm::dot(d, A), glm::dot(d, Cn));
            const uint64_t ms = B.seed ^ ((uint64_t)mi * 0xD1B54A32D192ED03ull);
            mb.sepDir = glm::dvec3(0.30 + 0.25 * h01(ms, 30, 1), -0.05 - 0.1 * h01(ms, 31, 1), 0.16 * (h01(ms, 32, 1) - 0.5));
        }
    }
    B.valid = true;
}

// A member's state: the train climbing (kind 0, tD <= t < tA) or the retiree falling (kind 1, tA <= t < tEnd).
static bool memberState(const SatelliteSim &sim, const LBatch &B, const LMember &mb, int kind, double t, glm::dvec3 &r,
                        glm::dvec3 &v)
{
    SatOrbitElems e = sim.rosterElemsOf(sim.satOrbits[mb.sat]);
    e.meanMot = mb.nSlot; // the slot's rate: the phase offsets below are relative to it
    double a = mb.rSlot, du = 0.0;
    if (kind == 0)
    {
        if (t < B.tD || t >= B.tA)
            return false;
        const double tA = B.tA;
        const double epsK = mb.eps * (tA - t) / std::max(tA - B.tD, 1.0);
        const double sEnd = mb.sRaise + mb.tRamp; // == tA
        double lag;
        const double full = launch::rampPhase(B.rPark, mb.rSlot, mb.tRamp, mb.tRamp);
        if (t < mb.sRaise)
        {
            a = B.rPark;
            lag = (B.nPark - mb.nSlot) * (mb.sRaise - t) + full - mb.nSlot * mb.tRamp;
        }
        else
        {
            const double tau = std::min(t - mb.sRaise, mb.tRamp);
            a = B.rPark + (mb.rSlot - B.rPark) * tau / mb.tRamp;
            lag = (full - launch::rampPhase(B.rPark, mb.rSlot, mb.tRamp, tau)) - mb.nSlot * (sEnd - mb.sRaise - tau);
        }
        du = -(lag + epsK);
    }
    else
    {
        if (t < B.tA || t >= mb.tEnd)
            return false;
        const double Td = mb.tEnd - B.tA, tau = t - B.tA;
        const double rEnd = kR + kEndAltM;
        a = mb.rSlot + (rEnd - mb.rSlot) * tau / Td;
        du = launch::rampPhase(mb.rSlot, rEnd, Td, tau) - mb.nSlot * tau;
    }
    e.u0 += du;
    e.rSatM = a;
    const SatOrbitState st = satOrbitStateAt(e, t);
    r = st.posEci;
    v = st.velocity * std::sqrt(launch::kGM / glm::length(r));
    // Leaving the stage: the member starts where the stage is and drifts into its own plane and phase over two days
    // (members of one "plane" differ by up to a degree of node, a Starmind cluster by its jitter).
    // It leaves the dispenser at tRel, drifting away at sepDir (m/s), and both ease out together.
    if (kind == 0 && glm::dot(mb.depOff, mb.depOff) > 0.0)
    {
        const double age = std::max(t - mb.tRel, 0.0);
        const double s = glm::clamp(age / (2.0 * 86400.0), 0.0, 1.0);
        const double f = 1.0 - s * s * (3.0 - 2.0 * s);
        if (f > 0.0)
        {
            const glm::dvec3 U = r / glm::length(r);
            const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
            const glm::dvec3 A = glm::cross(Cn, U);
            const glm::dvec3 o = mb.depOff + mb.sepDir * age;
            r += f * (o.x * U + o.y * A + o.z * Cn);
            if (t >= mb.tRel)
                v += f * (mb.sepDir.x * U + mb.sepDir.y * A + mb.sepDir.z * Cn);
        }
    }
    return true;
}

// Whether a member is drawn: Kessler may have killed its slot before the swap (no replacement, no retiree).
static bool memberAllowed(const SatelliteSim &sim, const LBatch &B, const LMember &mb)
{
    if (!sim.kesslerEnabled || 2ull * mb.sat >= sim.kRosterWords.size())
        return true;
    const int32_t dead = sim.kRosterWords[2ull * mb.sat];
    return dead == kessler::kNever || (double)dead > secOfKessler(sim, B.tA);
}

// ── The rocket ──────────────────────────────────────────────────────────────────────────────────
// Earth-fixed position on the ground track: x downrange along the great circle (G0, Eaz), h up.
static glm::dvec3 trackEcef(const LBatch &B, double x, double h)
{
    const double a = x / kR;
    return (kR + h) * (std::cos(a) * B.G0 + std::sin(a) * B.Eaz);
}

// The booster (stage 0) or the upper stage (1) at tau s after lift-off, ECI. False when it is not flying.
static bool rocketPos(const SatelliteSim::LaunchState &L, const LBatch &B, int stage, double tau, glm::dvec3 &P,
                      uint8_t *burn = nullptr)
{
    const double t = B.tL + tau;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    if (tau < -600.0)
        return false;
    tau = std::max(tau, 0.0);
    if (stage == 0)
    {
        const Track &tr = L.booster[B.veh];
        if (tau > tr.tEnd)
            return false;
        uint8_t b = 0;
        const glm::dvec2 p = tr.at(tau, nullptr, &b);
        if (burn)
            *burn = (tau <= 0.0) ? launch::BURN_NONE : b;
        // Before staging the two are one vehicle: the booster carries the flame.
        P = ecefToEci(trackEcef(B, p.x, p.y), t);
        return true;
    }
    if (B.tL + tau >= B.tStageGone)
        return false;
    if (burn)
        *burn = launch::BURN_NONE;
    const Track &up = const_cast<SatelliteSim::LaunchState &>(L).upperTrackFor(B.veh, L.cat.programs[B.prog].parkKm);
    if (tau <= B.t2)
    {
        const glm::dvec2 p = up.at(tau);
        const glm::dvec3 Pg = ecefToEci(trackEcef(B, p.x, p.y), t);
        const double a = p.x / kR;
        const glm::dvec3 Pi = (kR + p.y) * (std::cos(a) * B.U0 + std::sin(a) * B.A0);
        const double s = glm::clamp((tau - (vh.t1 - 60.0)) / (B.t2 - vh.t1 + 60.0), 0.0, 1.0);
        P = glm::mix(Pg, Pi, s * s * (3.0 - 2.0 * s));
        if (burn && tau > vh.t1 + 4.0)
            *burn = launch::BURN_UPPER;
        return true;
    }
    const double th = B.thIns + B.nPark * (tau - B.t2);
    P = B.rPark * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
    if (t > B.tDeorbBurn)
    {
        // The deorbit burn: from its parking circle onto the entry ellipse, blended over the burn (both start at the
        // same point at the same speed's direction, so position and velocity stay continuous).
        const double s = glm::clamp((t - B.tDeorbBurn) / std::max(B.burnDur, 1.0), 0.0, 1.0);
        P = glm::mix(P, deorbitPos(B, t), s * s * (3.0 - 2.0 * s));
    }
    if (burn && t >= B.tDeorbBurn && t < B.tDeorbBurn + B.burnDur)
        *burn = launch::BURN_UPPER;
    (void)vh;
    return true;
}

// The stage on its entry ellipse (apoapsis at the deorbit burn's start), ECI.
static glm::dvec3 deorbitPos(const LBatch &B, double t)
{
    const double M = glm::pi<double>() + B.nE * (t - B.tDeorbBurn);
    double E = M;
    for (int i = 0; i < 8; ++i)
        E -= (E - B.eE * std::sin(E) - M) / (1.0 - B.eE * std::cos(E));
    const double r = B.aE * (1.0 - B.eE * std::cos(E));
    double nu = 2.0 * std::atan2(std::sqrt(1.0 + B.eE) * std::sin(0.5 * E), std::sqrt(1.0 - B.eE) * std::cos(0.5 * E));
    // nu - pi, continuous across the apoapsis (E = pi): E's own unwrap.
    const double dnu = nu - glm::pi<double>() + glm::two_pi<double>() * std::round((E - nu) / glm::two_pi<double>());
    const double th = B.thApo + dnu;
    return r * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
}

static glm::dvec3 rocketEcef(const SatelliteSim::LaunchState &L, const LBatch &B, int stage, double tau, bool &ok)
{
    glm::dvec3 P;
    ok = rocketPos(L, B, stage, tau, P);
    return ok ? eciToEcef(P, B.tL + std::max(tau, 0.0)) : glm::dvec3(0.0);
}

// The booster's nose as a pitch angle in its trajectory's vertical plane (from downrange toward up; branch
// [-90, 270) degrees, so nothing it flies wraps). Ascent: along its velocity. After staging: engines first (along -v),
// and in its burns along its thrust (the track's acceleration less gravity): a boostback toward the pad, the landing
// burn upright. Every change between these (the flip after MECO, the flip back after an RTLS boostback) passes
// through the vertical, and averaging the ANGLE (stageNose) turns each into a slew; averaging vectors could not: the
// ones after an RTLS boostback point opposite ways, and their mean spun the booster ~340 degrees/s.
static double boosterPitchRaw(const SatelliteSim::LaunchState &L, const LBatch &B, double tau)
{
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const Track &tr = L.booster[B.veh];
    auto ang = [](glm::dvec2 d) {
        double a = std::atan2(d.y, d.x);
        if (a < -0.5 * glm::pi<double>())
            a += glm::two_pi<double>();
        return a;
    };
    tau = glm::clamp(tau, 0.0, tr.tEnd);
    glm::dvec2 v;
    uint8_t b = 0;
    tr.at(tau, &v, &b);
    if (tau <= vh.t1 + 1.0 || glm::length(v) < 1.0)
        return glm::length(v) < 1.0 ? 0.5 * glm::pi<double>() : ang(v);
    if (b == launch::BURN_BOOSTBACK || b == launch::BURN_ENTRY || b == launch::BURN_LANDING)
    {
        glm::dvec2 va, vc;
        tr.at(tau - 0.5, &va);
        tr.at(tau + 0.5, &vc);
        const glm::dvec2 acc = (vc - va) + glm::dvec2(0.0, 9.81);
        if (glm::length(acc) > 1.0)
            return ang(acc);
    }
    return ang(-v);
}

// The way the upper stage's nose points (ECI, unit), before smoothing: along its velocity to orbit and in orbit,
// then it pitches over to retrograde in the two minutes before its deorbit burn (through the vertical, never a snap)
// and holds it; a returning ship then pitches up to its entry attitude, nose 60 degrees above the velocity, belly into
// the flow (what its entry glow starts with).
static glm::dvec3 upperNoseRaw(const SatelliteSim::LaunchState &L, const LBatch &B, double tau, bool &ok)
{
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const double t = B.tL + tau;
    ok = false;
    if (tau < B.t2)
    {
        // To orbit: along the Earth-fixed velocity (in ECI the pad's rotation would tilt a rising rocket).
        bool o1 = false, o0 = false;
        const glm::dvec3 E1 = rocketEcef(L, B, 1, tau, o1);
        const glm::dvec3 E0 = rocketEcef(L, B, 1, tau - 1.0, o0);
        if (!o1)
            return glm::dvec3(0.0);
        ok = true;
        const glm::dvec3 vE = (o0 && tau > 0.5) ? E1 - E0 : glm::dvec3(0.0);
        return ecefToEci(glm::length(vE) > 0.5 ? glm::normalize(vE) : glm::normalize(E1), t);
    }
    glm::dvec3 P, P1;
    if (!rocketPos(L, B, 1, tau, P))
        return glm::dvec3(0.0);
    if (!rocketPos(L, B, 1, tau - 1.0, P1))
        P1 = P - glm::dvec3(0, 0, 1);
    ok = true;
    const glm::dvec3 vh1 = glm::normalize(P - P1), up = glm::normalize(P);
    glm::dvec3 upp = up - glm::dot(up, vh1) * vh1;
    upp = glm::length(upp) > 1e-6 ? glm::normalize(upp) : up;
    auto sm = [](double a, double b, double x) { const double s = glm::clamp((x - a) / (b - a), 0.0, 1.0); return s * s * (3.0 - 2.0 * s); };
    const double tb = B.tDeorbBurn, te = tb + B.burnDur;
    double ph = glm::pi<double>() * sm(tb - 150.0, tb - 20.0, t);
    if (!vh.upperDeorbit)
        ph -= (glm::pi<double>() - glm::radians(60.0)) * sm(te + 60.0, te + 360.0, t);
    return std::cos(ph) * vh1 + std::sin(ph) * upp;
}

// The nose (ECI, unit), smoothed (the booster over 10 s, the upper stage 6 s): no snap at staging, at a burn's start or end, or into orbit.
// Zero when the stage is not flying.
static glm::dvec3 stageNose(const SatelliteSim::LaunchState &L, const LBatch &B, int stage, double tau)
{
    if (stage == 0)
    {
        glm::dvec3 Pe;
        if (!rocketPos(L, B, 0, tau, Pe))
            return glm::dvec3(0.0);
        double sum = 0.0, wsum = 0.0;
        for (int k = 0; k <= 20; ++k)
        {
            const double w = (k == 0 || k == 20) ? 0.5 : 1.0;
            sum += w * boosterPitchRaw(L, B, tau - 0.5 * (double)k);
            wsum += w;
        }
        const double psi = sum / wsum;
        const Track &tr = L.booster[B.veh];
        const glm::dvec2 p = tr.at(glm::clamp(tau, 0.0, tr.tEnd));
        const double a = p.x / kR;
        const glm::dvec3 up = std::cos(a) * B.G0 + std::sin(a) * B.Eaz;  // Earth-fixed, at the booster
        const glm::dvec3 dr = -std::sin(a) * B.G0 + std::cos(a) * B.Eaz; // downrange there
        return ecefToEci(std::cos(psi) * dr + std::sin(psi) * up, B.tL + std::max(tau, 0.0));
    }
    glm::dvec3 sum(0.0);
    for (int k = 0; k < 7; ++k)
    {
        bool ok = false;
        const glm::dvec3 n = upperNoseRaw(L, B, tau - (double)k, ok);
        if (ok)
            sum += (k == 0 || k == 6 ? 0.5 : 1.0) * n;
        else if (k == 0)
            return glm::dvec3(0.0);
    }
    return glm::length(sum) > 1e-9 ? glm::normalize(sum) : glm::dvec3(0.0);
}

// A returning stage's entry, te s after the entry interface (B.tStageGone): ~7.4 km/s down to ~65 km, then slowing to
// ~1.2 km/s by 45 km over 8 minutes, along its orbit's plane. Position ECI; speed and altitude out.
static glm::dvec3 shipEntry(const LBatch &B, double te, double *vOut, double *hOut)
{
    te = std::clamp(te, 0.0, 480.0);
    const double v0 = B.vEI; // the entry ellipse's speed at 100 km (continuous with rocketPos)
    auto vAt = [v0](double u) { const double s = glm::clamp((u - 0.35) / 0.65, 0.0, 1.0); return v0 - (v0 - 1200.0) * s * s * (3.0 - 2.0 * s); };
    // (+ a term that starts it falling at the ellipse's own rate, vrEI, and is gone by u = 0.45)
    const double vr0 = B.vrEI;
    auto hAt = [vr0](double u) {
        const double a = glm::clamp(u / 0.45, 0.0, 1.0), b = glm::clamp((u - 0.45) / 0.55, 0.0, 1.0);
        const double w = u < 0.45 ? u * (1.0 - a) * (1.0 - a) : 0.0;
        return 100000.0 - 35000.0 * a * a * (3.0 - 2.0 * a) - 20000.0 * b * b * (3.0 - 2.0 * b) + vr0 * 480.0 * w;
    };
    // Along-track distance: the speed integrated (Simpson, 16 intervals).
    double sAlong = 0.0;
    const int N = 16;
    for (int i = 0; i <= N; ++i)
    {
        const double w = (i == 0 || i == N) ? 1.0 : (i % 2 ? 4.0 : 2.0);
        sAlong += w * vAt(te * i / N / 480.0);
    }
    sAlong *= te / (3.0 * N);
    const double u = te / 480.0;
    if (vOut)
        *vOut = vAt(u);
    if (hOut)
        *hOut = hAt(u);
    // From the entry interface on the deorbit ellipse (angle per metre at the ellipse's radius there).
    const double th = B.thEI + sAlong / (kR + 100000.0);
    return (kR + hAt(u)) * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
}

// When the upper stage vents what is left, spinning (after its deorbit burn, or 10 min after deploying for a stage
// that returns).
static double ventStart(const SatelliteSim::LaunchState &L, const LBatch &B)
{
    return L.cat.vehicles[B.veh].upperDeorbit ? B.tDeorbBurn + 50.0 : B.tD + 600.0;
}

// The exhaust's parcels, from the whole flight (built once, when the launch first matters to the view).
static void buildParcels(const SatelliteSim::LaunchState &L, LBatch &B)
{
    B.parcels.clear();
    B.parcelsBuilt = true;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const Track &bt = L.booster[B.veh];
    // The launch's wind: a hashed direction, Earth-fixed at the pad.
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), B.G0)), north = glm::cross(B.G0, east);
    const double wa = glm::two_pi<double>() * h01(B.seed, 20, 1);
    const glm::dvec3 windDir = std::cos(wa) * east + std::sin(wa) * north;
    const double windGain = 0.5 + 1.0 * h01(B.seed, 21, 1);
    // Scattering cross-section per kg of exhaust: low down the visible trail (condensing vapour and soot: an opaque
    // column ~50-100 m wide near the pad); high up the exhaust's water freezes into sub-micron ice as it expands
    // (~3000 m^2/kg of ice, ~10% of the exhaust condensing): the bright, sunlit bell seen at twilight.
    auto kappaAt = [&](double h) { return glm::mix(10.0, 1000.0, glm::smoothstep(25000.0, 90000.0, h)); };
    auto albedoAt = [&](double h) { return glm::mix(0.95 - 0.55 * vh.soot, 0.98, glm::smoothstep(15000.0, 40000.0, h)); };
    auto emit = [&](int stage, double tau, double dtE, double flow, double vex, int mode) {
        bool ok0, ok1;
        const glm::dvec3 p = rocketEcef(L, B, stage, tau, ok0);
        const glm::dvec3 pb = rocketEcef(L, B, stage, tau - 0.25, ok1);
        bool ok2;
        const glm::dvec3 pf = rocketEcef(L, B, stage, tau + 0.25, ok2);
        if (!ok0 || !ok1 || !ok2)
            return;
        const glm::dvec3 vel = (pf - pb) / 0.5;
        const double spd = glm::length(vel);
        const glm::dvec3 up = glm::normalize(p);
        glm::dvec3 ex; // the exhaust's direction
        if (mode == 0)
            ex = spd > 5.0 ? -vel / spd : -up; // thrust along the velocity
        else if (mode == 1)
        {
            // Boostback / entry / landing: the thrust from the track's acceleration less gravity.
            bool okA, okB;
            const glm::dvec3 pa = rocketEcef(L, B, stage, tau - 1.0, okA), pc = rocketEcef(L, B, stage, tau + 1.0, okB);
            const glm::dvec3 acc = (pc - 2.0 * p + pa) + 9.81 * up;
            ex = glm::length(acc) > 1.0 ? -glm::normalize(acc) : (spd > 5.0 ? vel / spd : -up);
        }
        else if (mode == 2)
            ex = spd > 5.0 ? vel / spd : up; // retrograde burn: the exhaust goes forward
        else
        {
            // Venting while spinning: the jet turns about the velocity (a spiral seen from the ground).
            const glm::dvec3 f = spd > 5.0 ? vel / spd : glm::dvec3(1, 0, 0);
            glm::dvec3 s1 = glm::cross(f, up);
            s1 = glm::length(s1) > 1e-9 ? glm::normalize(s1) : glm::dvec3(0, 1, 0);
            const glm::dvec3 s2 = glm::cross(f, s1);
            const double ang = glm::two_pi<double>() * tau / (8.0 + 10.0 * h01(B.seed, 22, 1));
            ex = std::cos(ang) * s1 + std::sin(ang) * s2;
        }
        const double h = glm::length(p) - kR;
        const double m = flow * dtE;
        const double len = std::max(spd * dtE, 10.0);
        const double maxAge = h > 40000.0 ? kPlumeAgeHigh : kPlumeAgeLow;
        // Vented propellant: droplets freezing to ice, a large cross-section, spreading slowly (the dump spirals).
        const bool vent = mode == 3;
        // (A vent's parcel is a stretch of its jet, not of the stage's path: the stage carries the jet with it.)
        B.parcels.push_back(launch::makeParcel(tau, p, vel + vex * ex, m, vent ? 2000.0 : kappaAt(h), albedoAt(h),
                                               vent ? ex : (spd > 5.0 ? vel / spd : up), vent ? vex * dtE : len,
                                               maxAge, windDir, windGain, vent ? 30.0 : 2000.0));
        B.parcels.back().thin = vent;
    };
    // The pad's ground cloud: exhaust and the deluge's steam rolling out along the ground in the first seconds.
    for (int k = 0; k < 6; ++k)
    {
        const double ang = glm::two_pi<double>() * (k + h01(B.seed, 23, k)) / 6.0;
        const glm::dvec3 dir = std::cos(ang) * east + std::sin(ang) * north;
        const glm::dvec3 p = (kR + 20.0) * B.G0;
        B.parcels.push_back(launch::makeParcel(1.0 + 0.8 * k, p, 120.0 * dir + 20.0 * B.G0, vh.flow1 * 2.0,
                                               kappaAt(0.0), 0.95, dir, 60.0, kPlumeAgeLow, windDir, windGain));
    }
    // The booster: ascent (every 1.5 s low, 2 s above ~20 km), then its burns.
    for (double tau = 1.0; tau < vh.t1; )
    {
        const double dtE = tau < 60.0 ? 1.5 : 2.0;
        emit(0, tau, dtE, vh.flow1, vh.vex1, 0);
        tau += dtE;
    }
    for (double tau = vh.t1 + 1.0; tau < bt.tEnd; tau += 2.0)
    {
        uint8_t b = 0;
        bt.at(tau, nullptr, &b);
        if (b == launch::BURN_BOOSTBACK || b == launch::BURN_ENTRY || b == launch::BURN_LANDING)
            emit(0, tau, 2.0, vh.flow1 * (b == launch::BURN_LANDING ? 0.12 : 0.35), vh.vex1, 1);
    }
    // The upper stage to orbit: every 2.5 s (at 5 s the young parcels, 10-35 km apart, drew the trail as beads).
    for (double tau = vh.t1 + 2.5; tau < B.t2; tau += 2.5)
        emit(1, tau, 2.5, vh.flow2, vh.vex2, 0);
    // Its deorbit burn, and maybe a spinning vent of what is left (the spirals seen from Norway, New Zealand...).
    {
        // (every stage burns to deorbit since 2026-10-19, for B.burnDur; part of its engines)
        const double tb = B.tDeorbBurn - B.tL;
        for (double tau = tb; tau < tb + B.burnDur; tau += 3.0)
            emit(1, tau, 3.0, vh.flow2 * 0.3, vh.vex2, 2);
    }
    if (B.vents)
    {
        const double tv = ventStart(L, B) - B.tL;
        for (double tau = tv; tau < tv + 120.0; tau += 0.5)
            emit(1, tau, 0.5, 80.0, 700.0, 3); // ~10 t of residual propellant over two minutes
    }
}

};

// ── Per frame ───────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::launchUpdate(VulkanContext &ctx, VkCommandBuffer cmd)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    L.nRockets = L.nTrain = L.nRetire = L.nWrites = L.nBatches = 0;
    ++L.frame;
    LaunchImpl::ensureCatalog(L);
    if (kesslerBuf == VK_NULL_HANDLE || activeSatCount == 0)
        return;
    if (L.stage == VK_NULL_HANDLE)
    {
        ctx.createBuffer(64ull * LaunchState::kStageRecs, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, L.stage, L.stageMem);
        vkMapMemory(ctx.device, L.stageMem, 0, VK_WHOLE_SIZE, 0, &L.stageMapped);
    }
    // kesslerBuf recreated: the GPU block is zero (all hidden); everything live must be sent again.
    if (L.gpuGen != kesslerBufGen)
    {
        L.gpuGen = kesslerBufGen;
        for (SlotInfo &s : L.slots)
        {
            s.shown = false;
            s.tw = -1e300;
        }
        for (auto &kv : L.batches)
            kv.second.tw[0] = kv.second.tw[1] = -1e300; // every batch due again
    }
    // A new design-life scale reshuffles every refresh: start over.
    if (L.planesBuilt && (L.lifeScale != launchLifeScale || (L.afterStorm >= 0 && L.afterStorm != launchAfterStorm)))
    {
        launchInitBlock();
        L.gpuGen = kesslerBufGen;
    }
    L.afterStorm = launchAfterStorm;
    if (L.clearGpu)
    {
        L.clearGpu = false;
        vkCmdFillBuffer(cmd, kesslerBuf, 4ull * kLaunchWord0, 64ull * kLaunchCap, 0u);
        VkBufferMemoryBarrier fb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        fb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        fb.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_SHADER_READ_BIT;
        fb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        fb.buffer = kesslerBuf;
        fb.offset = 4ull * kLaunchWord0;
        fb.size = 64ull * kLaunchCap;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1, &fb,
                             0, nullptr);
        for (SlotInfo &si : L.slots)
            si.shown = false;
        for (auto &kv : L.batches)
            kv.second.tw[0] = kv.second.tw[1] = -1e300;
    }
    const bool on = launchesEnabled && L.loaded;
    if (on && !L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<uint64_t> touched;
    if (on)
    {
        // Batches that matter now: from before the launch to the last retiree's re-entry.
        int builds = 0;
        for (size_t pi = 0; pi < L.planes.size(); ++pi)
        {
            const LPlane &pl = L.planes[pi];
            const launch::Program &pg = L.cat.programs[pl.prog];
            const double life = progLifeS(pg, (double)launchLifeScale);
            const double lo = t - (pg.deorbitDays[1] + 1.0) * kDay, hi = t + (pg.raiseDays[1] + 1.5) * kDay;
            const int64_t k0 = (int64_t)std::floor((lo - (pl.nb - 1) * pl.gap) / life - pl.phase);
            const int64_t k1 = (int64_t)std::floor(hi / life - pl.phase);
            for (int64_t k = k0; k <= k1; ++k)
            {
                const double base = (k + pl.phase) * life;
                const int b0 = std::max(0, (int)std::ceil((lo - base) / pl.gap));
                const int b1 = std::min(pl.nb - 1, (int)std::floor((hi - base) / pl.gap));
                for (int b = b0; b <= b1; ++b)
                {
                    const uint64_t key = batchKey((int)pi, k, b);
                    auto it = L.batches.find(key);
                    if (it == L.batches.end())
                    {
                        // At most a few hundred new batches a frame (the first frame finds every train in the sky).
                        if (++builds > 200)
                            continue;
                        LBatch nb;
                        nb.plane = (int)pi;
                        nb.k = k;
                        nb.b = b;
                        LaunchImpl::buildBatch(*this, L, nb, pl, life);
                        it = L.batches.emplace(key, std::move(nb)).first;
                    }
                    it->second.touched = L.frame;
                    if (it->second.valid)
                        touched.push_back(key);
                }
            }
        }
    }
    std::sort(touched.begin(), touched.end());
    L.live = touched;
    L.nBatches = (uint32_t)touched.size();
    // GPU chunks: a batch holds the train's while any member climbs, the retirees' while any falls.
    std::vector<VkBufferCopy> copies;
    char *stage = static_cast<char *>(L.stageMapped);
    uint32_t nStage = 0;
    auto putRecord = [&](uint32_t slot, const GpuKepOrbit &g) {
        if (nStage >= LaunchState::kStageRecs)
            return false;
        memcpy(stage + 64ull * nStage, &g, 64);
        copies.push_back({64ull * nStage, 4ull * kLaunchWord0 + 64ull * slot, 64});
        ++nStage;
        return true;
    };
    auto hideSlot = [&](uint32_t slot) {
        SlotInfo &si = L.slots[slot];
        if (si.shown)
        {
            GpuKepOrbit z{};
            if (!putRecord(slot, z))
                return false;
            si.shown = false;
        }
        si.live = false;
        si.tw = -1e300;
        if (selectedSatIndex == (int)(activeSatCount + slot))
            selectedSatIndex = -1;
        // A followed record that ends is handed over by followCheckTarget (a train member to its roster slot, a retiree
        // to its re-entry) or left in place there; it is not snapped home here.
        return true;
    };
    auto releaseChunks = [&](std::vector<uint32_t> &ch) {
        for (uint32_t c : ch)
        {
            for (uint32_t s = c * kChunk; s < (c + 1) * kChunk; ++s)
            {
                hideSlot(s);
                L.slots[s].owner = 0;
                satOrbits[activeSatCount + s].constIdx = UINT32_MAX;
            }
            L.freeChunks.push_back(c);
        }
        ch.clear();
    };
    // Free what no longer matters (a batch out of the window, retirees all down, launches off).
    for (auto it = L.batches.begin(); it != L.batches.end();)
    {
        LBatch &B = it->second;
        for (int kind = 0; kind < 2; ++kind)
        {
            if (B.chunks[kind].empty())
                continue;
            const bool need = on && B.touched == L.frame && B.valid &&
                              (kind == 0 ? (t >= B.tD && t < B.tA) : (t >= B.tA && t < B.tA + 400.0 * kDay));
            // (Only while the staging can take every hide: a freed slot whose record stayed up would be a ghost.)
            if (!need && nStage + kChunk * (uint32_t)B.chunks[kind].size() <= LaunchState::kStageRecs)
            {
                releaseChunks(B.chunks[kind]);
                B.tw[kind] = -1e300;
                B.nUp[kind] = 0;
            }
        }
        if (B.touched + 600 < L.frame && B.chunks[0].empty() && B.chunks[1].empty())
            it = L.batches.erase(it);
        else
            ++it;
    }
    // Writes: members coming up first, then the stalest. A record is rewritten every ~20 s of sim time (the GPU
    // carries it on as a Kepler orbit meanwhile: the climb's drift over 20 s is under a metre).
    const double tEpoch = (double)orbitEpochDay * 86400.0 + orbitEpochSec;
    uint32_t budget = 1200;
    auto writeMember = [&](LBatch &B, int kind, uint32_t mi, uint32_t slot) {
        const LMember &mb = B.mem[mi];
        glm::dvec3 r, v;
        if (!LaunchImpl::memberState(*this, B, mb, kind, t, r, v))
            return;
        kessler::Elems el = kessler::elemsFromState({r, v});
        if (!(el.a > kR) || el.e >= 0.9)
            return;
        const double n = launch::meanMotion(el.a);
        const SatOrbit &so = satOrbits[mb.sat];
        GpuKepOrbit g{};
        g.M = (float)wrap2pi(el.M - n * (t - tEpoch));
        g.argp = (float)wrap2pi(el.argp);
        g.raan = (float)wrap2pi(el.raan);
        g.n = (float)n;
        g.a = (float)el.a;
        g.e = (float)el.e;
        g.cosI = (float)std::cos(el.i);
        g.sinI = (float)std::sin(el.i);
        g.flux = 1.0f;
        g.bornSec = (int32_t)0x80000001;
        g.deathSec = kessler::kNever;
        // A train raising flies "open book" (bit 25: every joint at rest, arrays flat with the bus, edge into the
        // flow): the low-drag configuration that makes fresh trains bright. On station it is the roster satellite.
        g.typeConst = (uint32_t)so.typeIdx | ((so.constIdx & 0xFFu) << 16) | (kind == 0 ? (1u << 25) : 0u);
        if (!putRecord(slot, g))
            return;
        SlotInfo &si = L.slots[slot];
        si.shown = true;
        si.live = true;
        si.tw = t;
        SatOrbitElems &ce = L.elems[slot];
        ce = SatOrbitElems{};
        ce.kepler = true;
        ce.u0 = wrap2pi(el.M - n * t);
        ce.raan = el.raan;
        ce.argp = el.argp;
        ce.meanMot = n;
        ce.rSatM = el.a;
        ce.ecc = el.e;
        ce.incl = el.i;
        ce.restJoints = kind == 0;
        SatOrbit &o = satOrbits[activeSatCount + slot];
        o.typeIdx = so.typeIdx;
        o.constIdx = so.constIdx;
        o.incl = (float)el.i;
        o.altM = (float)(el.a - kR);
        o.R_sat = (float)el.a;
        o.meanMot = (float)n;
        o.launchSlot = (int32_t)slot;
        ++L.nWrites;
        if (budget > 0)
            --budget;
    };
    if (on)
    {
        // A batch's records are rewritten together, when due: never written, 20 s of sim time since, or a retiree's
        // re-entry passed. Idle batches cost nothing. The write budget runs round-robin from a moving start.
        uint32_t nT = 0, nR = 0;
        const size_t nb = touched.size();
        for (size_t q = 0; q < nb; ++q)
        {
            const size_t bi = (L.cursor + q) % nb;
            LBatch &B = L.batches[touched[bi]];
            for (int kind = 0; kind < 2; ++kind)
            {
                const bool phase = kind == 0 ? (t >= B.tD && t < B.tA) : (t >= B.tA && t < B.tA + 400.0 * kDay);
                if (!phase)
                    continue;
                std::vector<uint32_t> &ch = B.chunks[kind];
                // While the train leaves the dispenser (and its first minutes, drifting from the stage at a fraction
                // of a m/s) every 2 s, so each member appears at its release and the separation stays smooth.
                const double relEnd = B.mem.empty() ? B.tD : B.mem.back().tRel;
                const double every = (kind == 0 && t < relEnd + 600.0) ? 2.0 : 20.0;
                bool due = ch.empty() || std::abs(t - B.tw[kind]) > every;
                if (!due && kind == 1)
                {
                    const double lo = std::min(B.tw[1], t), hi = std::max(B.tw[1], t);
                    auto it = std::upper_bound(B.ends.begin(), B.ends.end(), lo);
                    due = it != B.ends.end() && *it <= hi;
                }
                if (!due || (budget == 0 && !ch.empty()))
                {
                    (kind == 0 ? nT : nR) += B.nUp[kind];
                    continue;
                }
                // Who is up now (Kessler may have taken a slot; a retiree is gone at its re-entry).
                std::vector<uint8_t> upNow(B.mem.size(), 0);
                uint32_t up = 0;
                for (size_t m = 0; m < B.mem.size(); ++m)
                {
                    const LMember &mb = B.mem[m];
                    upNow[m] = (kind == 0 ? t >= mb.tRel : t < mb.tEnd) && LaunchImpl::memberAllowed(*this, B, mb);
                    up += upNow[m];
                }
                if (up == 0 && ch.empty())
                {
                    B.nUp[kind] = 0;
                    continue;
                }
                const uint32_t need = (uint32_t)((B.mem.size() + kChunk - 1) / kChunk);
                if (ch.size() < need)
                {
                    if (L.freeChunks.size() < need - ch.size())
                        continue; // the block is full: this batch waits
                    while (ch.size() < need)
                    {
                        ch.push_back(L.freeChunks.back());
                        L.freeChunks.pop_back();
                    }
                    const uint64_t owner = touched[bi] * 2 + (uint64_t)kind;
                    for (uint32_t m = 0; m < (uint32_t)B.mem.size(); ++m)
                    {
                        const uint32_t slot = ch[m / kChunk] * kChunk + m % kChunk;
                        L.slots[slot].owner = owner;
                        L.slots[slot].m = (uint16_t)m;
                    }
                }
                bool complete = true;
                for (uint32_t m = 0; m < (uint32_t)B.mem.size(); ++m)
                {
                    const uint32_t slot = ch[m / kChunk] * kChunk + m % kChunk;
                    SlotInfo &si = L.slots[slot];
                    if (!upNow[m])
                    {
                        if (si.shown || si.live)
                            complete &= hideSlot(slot);
                        continue;
                    }
                    if (si.shown && std::abs(t - si.tw) <= every)
                        continue;
                    if (budget == 0)
                    {
                        complete = false;
                        continue;
                    }
                    writeMember(B, kind, m, slot);
                }
                B.nUp[kind] = up;
                if (complete)
                    B.tw[kind] = t;
                (kind == 0 ? nT : nR) += up;
            }
        }
        L.nTrain = nT;
        L.nRetire = nR;
        if (nb)
            L.cursor = (L.cursor + 97) % nb;
    }
    L.active = std::any_of(L.slots.begin(), L.slots.end(), [](const SlotInfo &s) { return s.shown; });
    if (copies.empty())
        return;
    vkCmdCopyBuffer(cmd, L.stage, kesslerBuf, (uint32_t)copies.size(), copies.data());
    VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
    bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    bmb.buffer = kesslerBuf;
    bmb.offset = 4ull * kLaunchWord0;
    bmb.size = 64ull * kLaunchCap;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 1,
                         &bmb, 0, nullptr);
}

uint64_t SatelliteSim::launchSlotOwner(uint32_t slot, uint32_t &m) const
{
    m = 0;
    if (!launchState || slot >= launchState->slots.size())
        return 0;
    const SlotInfo &si = launchState->slots[slot];
    m = si.m;
    return si.live ? si.owner : 0;
}

SatOrbitElems SatelliteSim::launchOrbitElems(uint32_t slot) const
{
    if (!launchState || slot >= launchState->elems.size())
        return SatOrbitElems{};
    return launchState->elems[slot];
}

std::string SatelliteSim::launchLabel(uint32_t slot) const
{
    if (slot == kLaunchCap - 1)
        return transientLabel();
    if (slot == kLaunchCap - 2)
        return followTrKey ? transientLabelOf(followTrCur) : std::string("Re-entering object");
    if (!launchState || slot >= launchState->slots.size() || launchState->slots[slot].owner == 0)
        return "?";
    const LaunchState &L = *launchState;
    const SlotInfo &si = L.slots[slot];
    auto it = L.batches.find(si.owner / 2);
    if (it == L.batches.end() || si.m >= it->second.mem.size())
        return "?";
    const LBatch &B = it->second;
    const uint32_t sat = B.mem[si.m].sat;
    const SatOrbit &o = satOrbits[sat];
    char b[200];
    const char *cn = o.constIdx < constellations.size() ? constellations[o.constIdx].name.c_str() : "Satellite";
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (si.owner % 2 == 0)
        snprintf(b, sizeof(b), "%s #%d (new, raising; %s, arrives in %.1f d)", cn,
                 (int)sat - (int)constellations[o.constIdx].orbitStart, L.cat.vehicles[B.veh].name.c_str(),
                 (B.tA - t) / kDay);
    else
        snprintf(b, sizeof(b), "%s #%d (retired, re-enters in %.1f d)", cn,
                 (int)sat - (int)constellations[o.constIdx].orbitStart, (B.mem[si.m].tEnd - t) / kDay);
    return b;
}

// ── Visuals: rockets, flames, plumes, re-entries ────────────────────────────────────────────────
void SatelliteSim::launchFillVisuals(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull)
{
    if (!launchState || !launchesEnabled || !kFireMapped)
        return;
    LaunchState &L = *launchState;
    L.nRockets = L.nPlumes = L.nReentries = 0;
    L.plumeDbg.clear();
    if (!L.loaded)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const glm::dvec3 sunD = glm::normalize(glm::dvec3(sunDirECI));
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double K = satphot::kFluxToFlare * (double)brightnessScale;
    auto earthBlocks = [&](const glm::dvec3 &P) {
        const glm::dvec3 rel = P - eye;
        const double range = glm::length(rel);
        const double b = glm::dot(eye, rel / range), c = glm::dot(eye, eye) - kR * kR;
        const double disc = b * b - c;
        return disc > 0.0 && (-b - std::sqrt(disc)) > 0.0 && (-b - std::sqrt(disc)) < range - 50.0;
    };
    auto sunlit = [&](const glm::dvec3 &P) {
        const double sp = glm::dot(P, sunD);
        return sp > 0.0 || glm::length(P - sp * sunD) > kR + 15000.0;
    };
    struct Cand
    {
        double pri;
        GpuDustCloud d;
    };
    // The view cone a Gaussian (centre relENU from the eye, extent R m) can touch: the shader skips it elsewhere.
    auto coneOf = [&](const glm::dvec3 &relEnu, double R) {
        const double d = glm::length(relEnu);
        if (R >= 0.95 * d)
            return glm::vec4(0.0f, 0.0f, 0.0f, -2.0f);
        return glm::vec4(glm::vec3(relEnu / d), (float)std::sqrt(1.0 - (R / d) * (R / d)));
    };
    // Whether the Earth hides a sphere of radius R about P entirely from the eye.
    auto earthHides = [&](const glm::dvec3 &P, double R) {
        const glm::dvec3 rel = P - eye;
        const double d = glm::length(rel);
        const glm::dvec3 u = rel / d;
        const double tc = -glm::dot(eye, u);
        if (tc <= 0.0 || tc >= d)
            return false;
        const double miss = glm::length(eye + tc * u);
        return miss < kR - R;
    };
    std::vector<Cand> plumes;
    const double nightBoost = 1.0 + 9.0 * (1.0 - dayness);
    const double pixA = ctx_ ? 2.0 * std::tan(0.5 * glm::radians((double)camera.fovYDeg)) /
                                   (double)std::max(1u, ctx_->swapExtent.height)
                             : 1e-3;
    for (uint64_t key : L.live)
    {
        auto bit = L.batches.find(key);
        if (bit == L.batches.end())
            continue;
        LBatch &B = bit->second;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        // Retirees re-entering: their fireballs.
        bool anyRe = false;
        if (t >= B.tA)
        {
            auto it = std::lower_bound(B.ends.begin(), B.ends.end(), t - 200.0);
            anyRe = it != B.ends.end() && *it <= t;
        }
        if (anyRe)
            for (size_t mi = 0; mi < B.mem.size(); ++mi)
            {
                const LMember &mb = B.mem[mi];
                const double tau = t - mb.tEnd;
                if (tau < 0.0 || tau > 200.0 || !LaunchImpl::memberAllowed(*this, B, mb))
                    continue;
                glm::dvec3 r, v;
                if (!LaunchImpl::memberState(*this, B, mb, 1, mb.tEnd - 1.0, r, v))
                    continue;
                const glm::dvec3 U0 = glm::normalize(r);
                if (glm::length(kR * U0 - eye) > 3.0e6)
                    continue;
                glm::dvec3 A = v - glm::dot(v, U0) * U0;
                A = glm::length(A) > 1.0 ? glm::normalize(A) : glm::dvec3(1, 0, 0);
                const SatOrbit &so = satOrbits[mb.sat];
                const double mass = so.typeIdx < satTypes.size() ? LaunchImpl::launchTypeMass(satTypes[so.typeIdx]) : 600.0;
                const double m0 = -4.0 - 2.5 * std::log10(std::max(mass, 1.0) / 100.0);
                const int N = (int)std::clamp(3.0 + 40.0 * std::sqrt(std::max(mass, 1.0) / 2500.0), 3.0, 80.0);
                fbDrawReentry(U0, A, m0, N, B.seed ^ ((uint64_t)mb.sat * 0x9E3779B97F4A7C15ull), tau, out, n, toSky, pixFull,
                              3, mb.sat);
                ++L.nReentries;
            }
        if (t < B.tL - 600.0 || t > B.tVisEnd)
            continue;
        const double tau = t - B.tL;
        // The upper stage's own re-entry.
        if (vh.upperDeorbit && t >= B.tStageRE && t < B.tStageRE + 200.0)
        {
            glm::dvec3 P, Pm;
            if (LaunchImpl::rocketPos(L, B, 1, B.tStageRE - B.tL - 1.0, P) &&
                LaunchImpl::rocketPos(L, B, 1, B.tStageRE - B.tL - 2.0, Pm))
            {
                const glm::dvec3 U0 = glm::normalize(P);
                if (glm::length(kR * U0 - eye) < 3.0e6)
                {
                    // Along its entry ellipse's ground track (horizontal part of its velocity).
                    glm::dvec3 A = (P - Pm) - glm::dot(P - Pm, U0) * U0;
                    A = glm::length(A) > 1.0 ? glm::normalize(A) : B.A0;
                    const double m0 = -4.0 - 2.5 * std::log10(vh.upperMassKg / 100.0);
                    fbDrawReentry(U0, A, m0, (int)std::clamp(3.0 + 40.0 * std::sqrt(vh.upperMassKg / 2500.0), 3.0, 80.0),
                                  B.seed ^ 0x51A6E5ull, t - B.tStageRE, out, n, toSky, pixFull, 4, key);
                    ++L.nReentries;
                }
            }
        }
        // A returning upper stage (Starship's ship) re-enters whole: an ~8-min plasma glow along its orbit, descending
        // from 100 to ~45 km and slowing from 7.5 to ~1.2 km/s, brightest near 70 km. (It is not followed to landing.)
        if (!vh.upperDeorbit && t >= B.tStageGone && t < B.tStageGone + 480.0)
        {
            auto shipAt = [&](double te) { return LaunchImpl::shipEntry(B, te, nullptr, nullptr); };
            const double te = t - B.tStageGone;
            double v = 0.0, h = 0.0;
            const glm::dvec3 P = LaunchImpl::shipEntry(B, te, &v, &h), Pp = shipAt(te - 6.0);
            const double I = std::min(std::pow(v / 7500.0, 3.0) * std::exp(-(h - 70000.0) / 8000.0), 3.0) *
                             glm::smoothstep(0.0, 30.0, te) * (1.0 - glm::smoothstep(420.0, 480.0, te));
            const glm::dvec3 rel = P - eye;
            const double range = std::max(glm::length(rel), 1.0);
            // Listed for the selection (and its mesh) whether glowing or not; pickable once drawn.
            size_t shipIdx = SIZE_MAX;
            if (range < 4.0e6 || ((key << 3) ^ 0x2ull) == selTransientKey ||
                ((key << 3) ^ 0x2ull) == followTrKey) // (the selected / followed one at any range)
            {
                SelTransient st;
                // The upper stage's own key: a selected or followed ship stays selected into its entry.
                st.key = (key << 3) ^ 0x2ull;
                st.stage = 1;
                st.kind = 2;
                st.a = key;
                st.tau = te;
                const glm::dvec3 P2 = shipAt(te - 12.0);
                st.pos = P;
                st.vel = (P - Pp) / 6.0;
                st.acc = (P - 2.0 * Pp + P2) / 36.0;
                // Belly first: the nose pitched ~60 degrees above the velocity, the heat shield into the flow.
                const glm::dvec3 vh1 = glm::normalize(P - Pp), up = glm::normalize(P);
                glm::dvec3 upp = up - glm::dot(up, vh1) * vh1;
                upp = glm::length(upp) > 1e-6 ? glm::normalize(upp) : up;
                st.axis = glm::normalize(0.5 * vh1 + 0.866 * upp);
                auto ti = rocketTypes.find("rocket_" + vh.id + "_upper");
                st.type = ti != rocketTypes.end() ? ti->second : -1;
                st.dirEnu = glm::vec3(toEnu(rel) / range);
                st.flare = 0.0f;
                shipIdx = selTransients.size();
                addSelTransient(st);
            }
            if (I > 1e-3 && !earthBlocks(P))
            {
                const double m = -4.0 - 2.5 * std::log10(I) + 5.0 * std::log10(range / 1.0e5) +
                                 atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
                const double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.95 * dayness);
                const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.62f, 0.38f), glm::vec3(1.0f, 0.55f, 0.75f),
                                               (float)glm::clamp(v / 7500.0, 0.0, 1.0));
                if (flare > 2e-4 && kFireCount < kFireballMax)
                    fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rel) / range), (float)flare), glm::vec4(col, (float)range)};
                if (flare > 2e-4 && shipIdx < selTransients.size())
                    selTransients[shipIdx].flare = (float)flare;
                // Its glowing wake: the last 6 s of path, an emissive Gaussian.
                if (flare > 5e-3 && n < kMaxDustClouds)
                {
                    const glm::dvec3 axis = P - Pp;
                    const double Lw = glm::length(axis);
                    const glm::dvec3 a = toEnu(axis / std::max(Lw, 1.0));
                    glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
                    b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
                    const glm::dvec3 c2 = glm::cross(a, b2);
                    const double sMin = 0.6 * range * 2.0 * pixFull;
                    const double sA = std::max(0.28 * Lw, sMin), sP = std::max(300.0, sMin);
                    const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
                    auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                    const double J = 0.7 * flare * range * range / K;
                    const glm::dvec3 ce = toEnu(P - 0.55 * axis - eye);
                    GpuDustCloud &d = out[n++];
                    d.c = glm::vec4(glm::vec3(ce), (float)(J * toSky));
                    d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                                     (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
                    d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), 0.0f);
                    d.col = glm::vec4(col, 1.0f);
                    d.cone = coneOf(ce, 5.5 * std::max(sA, sP));
                }
                ++L.nReentries;
            }
        }
        // Rockets: the flame (a point and, close up, an emissive Gaussian) or, coasting, the sunlit body.
        for (int stage = 0; stage < 2; ++stage)
        {
            glm::dvec3 P;
            uint8_t burn = 0;
            if (!LaunchImpl::rocketPos(L, B, stage, tau, P, &burn))
                continue;
            if (stage == 1 && tau < vh.t1)
                continue; // still one vehicle: drawn as the booster
            if (stage == 0 && tau > vh.t1 + 5.0 && vh.recovery == 0 && burn == launch::BURN_NONE &&
                glm::length(P) - kR < 40000.0)
                continue;
            const glm::dvec3 rel = P - eye;
            const double range = std::max(glm::length(rel), 1.0);
            // Listed for the selection whether drawn or not (a booster coasting in the dark, behind the Earth: a
            // tracked rocket stays selected); pickable only once drawn (flare > 0).
            size_t trIdx = SIZE_MAX;
            if (glm::length(P - eye) < 4.0e6 || ((key << 3) ^ (uint64_t)(stage + 1)) == selTransientKey ||
                ((key << 3) ^ (uint64_t)(stage + 1)) == followTrKey)
            {
                SelTransient st;
                st.key = (key << 3) ^ (uint64_t)(stage + 1);
                // The body axis (nose): the Earth-fixed velocity on the track (in ECI the pad's ~400 m/s of rotation
                // would tilt a rising rocket), straight up on the pad; a booster after staging and an upper stage
                // in its deorbit burn fly engines first. In orbit, the inertial velocity.
                {
                    // (stageNose: smoothed; the booster's flip, the upper stage's turn to retrograde for its deorbit
                    // burn and a ship's pitch-up to its entry attitude are slews, never snaps.)
                    glm::dvec3 ax = LaunchImpl::stageNose(L, B, stage, tau);
                    if (glm::length(ax) < 0.5)
                        ax = glm::normalize(P);
                    st.axis = ax;
                    const std::string mid = "rocket_" + vh.id + (stage == 1 ? "_upper" : tau < vh.t1 ? "_stack" : "_booster");
                    auto ti = rocketTypes.find(mid);
                    st.type = ti != rocketTypes.end() ? ti->second : -1;
                }
                st.kind = 1;
                st.stage = (uint8_t)stage;
                st.burn = burn;
                st.a = key;
                st.tau = tau;
                glm::dvec3 P1, P2;
                if (!LaunchImpl::rocketPos(L, B, stage, tau - 0.5, P1))
                    P1 = P;
                if (!LaunchImpl::rocketPos(L, B, stage, tau - 1.0, P2))
                    P2 = P1;
                st.pos = P;
                st.vel = (P - P1) / 0.5;
                st.acc = (P - 2.0 * P1 + P2) / 0.25;
                st.dirEnu = glm::vec3(toEnu(rel) / range);
                st.flare = 0.0f;
                trIdx = selTransients.size();
                addSelTransient(st);
            }
            if (earthBlocks(P))
                continue;
            ++L.nRockets;
            const glm::dvec3 dirE = toEnu(rel) / range;
            const double ext = atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
            const bool burning = burn != launch::BURN_NONE;
            double m;
            glm::vec3 col;
            if (burning)
            {
                double fm = stage == 0 ? vh.flameMag1 : vh.flameMag2;
                if (burn == launch::BURN_LANDING)
                    fm += 1.5;
                else if (burn == launch::BURN_BOOSTBACK || burn == launch::BURN_ENTRY)
                    fm += 0.7;
                m = fm + 5.0 * std::log10(range / 1.0e5) + ext - 2.5 * std::log10(std::max((double)launchRocketGain, 1e-3));
                col = glm::mix(glm::vec3(1.0f, 0.82f, 0.62f), glm::vec3(1.0f, 0.6f, 0.3f), (float)vh.soot);
            }
            else
            {
                if (!sunlit(P) || tau < 0.0)
                    continue; // on the pad, or dark: nothing to see from afar
                m = (stage == 0 ? 3.0 : 3.8) + 5.0 * std::log10(range / 1.0e6) + ext;
                col = glm::vec3(0.95f, 0.95f, 0.98f);
                // The sunlit body gives way to its mesh as that resolves (a burning stage keeps its flame's point).
                if (trIdx < selTransients.size())
                {
                    const double px = rocketMeshPx(selTransients[trIdx], pixA, eye);
                    m -= 2.5 * std::log10(std::max(1.0 - glm::smoothstep(2.0, 8.0, px), 1e-4));
                }
            }
            double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - (burning ? 0.8 : 0.97) * dayness);
            if (flare < 2e-4)
                continue;
            if (kFireCount < kFireballMax)
                fo[kFireCount++] = {glm::vec4(glm::vec3(dirE), (float)flare), glm::vec4(col, (float)range)};
            if (trIdx < selTransients.size())
                selTransients[trIdx].flare = (float)flare; // drawn: pickable
            // The flame as a shape (seen close it is long: the exhaust glows behind the engines, and high up the
            // expanding plume around it).
            if (burning && n < kMaxDustClouds && flare > 2e-3)
            {
                glm::dvec3 Pb;
                const double dts = 0.5;
                if (!LaunchImpl::rocketPos(L, B, stage, tau - dts, Pb))
                    Pb = P;
                glm::dvec3 vdir = P - Pb;
                vdir = glm::length(vdir) > 1e-3 ? glm::normalize(vdir) : glm::normalize(P);
                if (trIdx < selTransients.size() && glm::length(selTransients[trIdx].axis) > 0.5)
                    vdir = selTransients[trIdx].axis; // out of the engines: a landing burn's flame points down
                const double h = glm::length(P) - kR;
                const double hiK = glm::smoothstep(20000.0, 90000.0, h);
                const double Lf = 50.0 + 900.0 * hiK, Wf = 6.0 + 120.0 * hiK;
                const double sMin = 0.6 * range * 2.0 * pixFull;
                const double sA = std::max(0.5 * Lf, sMin), sP = std::max(Wf, sMin);
                const glm::dvec3 a = toEnu(vdir);
                glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
                b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
                const glm::dvec3 c2 = glm::cross(a, b2);
                const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
                auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                const double J = 0.6 * flare * range * range / K;
                GpuDustCloud &d = out[n++];
                d.c = glm::vec4(glm::vec3(toEnu(P - 0.5 * Lf * vdir - eye)), (float)(J * toSky));
                d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                                 (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
                d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), 0.0f);
                d.col = glm::vec4(col, 1.0f);
                d.cone = coneOf(toEnu(P - 0.5 * Lf * vdir - eye), 5.5 * std::max(sA, sP));
                // The exhaust the flame lights (at night an orange glow round the engines, longer behind them; high up
                // the expanding plume around the stage glows over kilometres).
                if (n < kMaxDustClouds)
                {
                    const double Rg = 250.0 + 2500.0 * hiK;
                    const double gA = std::max(1.6 * Rg, sMin), gP = std::max(Rg, sMin);
                    const double jA = 1.0 / (gA * gA), jP = 1.0 / (gP * gP);
                    auto Mg = [&](int p, int q) { return jA * a[p] * a[q] + jP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                    GpuDustCloud &g = out[n++];
                    // A night effect: by day the lit exhaust is lost in the sky (up close it drew a white ellipse
                    // hundreds of metres across over the rocket).
                    g.c = glm::vec4(glm::vec3(toEnu(P - 1.2 * Rg * vdir - eye)),
                                    (float)(0.25 * J * toSky * (1.0 - 0.95 * dayness)));
                    g.ia = glm::vec4((float)Mg(0, 0), (float)Mg(1, 1), (float)Mg(2, 2),
                                     (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * gA * gP * gP)));
                    g.ib = glm::vec4((float)Mg(0, 1), (float)Mg(0, 2), (float)Mg(1, 2), 0.0f);
                    g.col = glm::vec4(glm::mix(col, glm::vec3(1.0f, 0.5f, 0.2f), 0.5f), 1.0f);
                    g.cone = coneOf(toEnu(P - 1.2 * Rg * vdir - eye), 5.5 * gA);
                }
            }
        }
        // Plumes: built (once) only for launches that can be seen from here, and not past 1 h/s of warp (a plume lasts
        // about an hour: building one costs a few ms, and at a day a second a dozen launches pass each second).
        if (launchPlumeGain <= 0.0f || tau < 0.0)
            continue;
        if (!B.parcelsBuilt)
        {
            const bool fast = !timePaused && kTimeScales[timeScaleIdx] > 3600.0f;
            const glm::dvec3 padNow = ecefToEci(B.G0, t) * kR;
            const double eyeAlt = glm::length(eye) - kR;
            double dNear = glm::length(padNow - eye);
            glm::dvec3 Pst;
            if (LaunchImpl::rocketPos(L, B, 1, tau, Pst))
                dNear = std::min(dNear, glm::length(Pst - eye)); // (a stage venting half an orbit downrange)
            if (fast || dNear > 4.5e6 + 2.0 * eyeAlt)
                continue;
            LaunchImpl::buildParcels(L, B);
        }
        const double era = earthRotationAngle(t);
        for (size_t pi = 0; pi < B.parcels.size(); ++pi)
        {
            const launch::Parcel &pc = B.parcels[pi];
            const double age = tau - pc.t0;
            // Old parcels are wide and overlap their neighbours: every 2nd stands for two past 2 min, every 4th for
            // four past 10 min (the budget then reaches the whole bell, not just its young part).
            if (age < 0.0)
                continue;
            glm::dvec3 cE;
            double sig, cond, sweep = 0.0;
            if (!pc.at(age, cE, sig, cond, &sweep))
                continue;
            // ... but only once they are wide against the spacing (pc.len): thinned earlier, a trail showed gaps.
            const int dec = pc.thin ? 1 : (age > 600.0 && sig > 4.0 * pc.len) ? 4 : (age > 120.0 && sig > 2.0 * pc.len) ? 2 : 1;
            if (pi % (size_t)dec != 0)
                continue;
            const double Sigma = pc.kappa * pc.m * cond * (double)launchPlumeGain * (double)dec;
            if (Sigma < 1.0)
                continue;
            const glm::dvec3 C = rotZ(cE, era);
            const glm::dvec3 rel = C - eye;
            const double range = std::max(glm::length(rel), 1.0);
            const double ext5 = 5.5 * std::sqrt(sig * sig + 0.25 * pc.len * pc.len);
            if (range > 6.0e6 + ext5 || earthHides(C, ext5))
                continue; // over the horizon
            // Elongated along the trail while young (a parcel stands for a stretch of the path).
            const glm::dvec3 ax = rotZ(pc.axis, era);
            const double sMin = 0.6 * range * 2.0 * pixFull;
            const double sP = std::max(sig, sMin);
            const double sA = std::max(std::sqrt(sig * sig + 0.25 * pc.len * pc.len), sMin);
            const double tauPeak = Sigma / (2.0 * glm::pi<double>() * sP * sA);
            const double pri = Sigma / (range * range) * (1.0 + std::min(tauPeak, 4.0));
            Cand c;
            c.pri = pri;
            GpuDustCloud &d = c.d;
            const glm::dvec3 a = toEnu(ax);
            glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
            b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
            const glm::dvec3 c2 = glm::cross(a, b2);
            const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
            auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
            d.c = glm::vec4(glm::vec3(toEnu(rel)), (float)Sigma);
            d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                             (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
            // The Sun's colour at the parcel (its path through the air; the Earth's shadow is the shader's) x albedo.
            const double h = glm::length(C) - kR;
            glm::dvec3 tr(1.0);
            const double kc[3] = {0.11, 0.2, 0.42};
            for (int ch = 0; ch < 3; ++ch)
                tr[ch] = std::pow(10.0, -0.4 * std::min(atmExtinctionMag(C, sunD, 0.0, kc[ch]), 30.0));
            // Why twilight plumes are off-white to blue (the jellyfish, like noctilucent cloud):
            // (1) the Sun's grazing path crosses the ozone layer, whose Chappuis band takes green and orange out
            //     (cloud_v2_sunlut.comp's thin shell at 25 km: vertical optical depth 0.019 / 0.027 / 0.0016 at
            //     680 / 550 / 440 nm, x the secant where the ray crosses it, twice from above when it dips through);
            {
                const double rO = kR + 25000.0, r = glm::length(C), cz = glm::dot(C, sunD) / r;
                const double sin2 = (r / rO) * (r / rO) * (1.0 - cz * cz);
                const double sec = 1.0 / std::sqrt(std::max(1.0 - sin2, 1e-3));
                const double nX = r <= rO ? 1.0 : ((cz < 0.0 && sin2 < 1.0) ? 2.0 : 0.0);
                const double odO[3] = {0.019, 0.027, 0.0016};
                for (int ch = 0; ch < 3; ++ch)
                    tr[ch] *= std::exp(-odO[ch] * std::min(sec, 30.0) * nX);
            }
            // (2) high up the exhaust's ice is sub-micron: it scatters blue more than red (Angstrom exponent ~1.5,
            //     680 / 550 / 440 nm), normalised to keep the luminance; low down the droplets and soot are grey.
            {
                const double ice = glm::smoothstep(25000.0, 90000.0, h);
                const glm::dvec3 sp(0.727 / 0.9707, 1.0 / 0.9707, 1.398 / 0.9707);
                tr *= glm::mix(glm::dvec3(1.0), sp, ice);
            }
            const double sinE = glm::dot(glm::normalize(C), sunD);
            // The sky's light on it, relative (1 = a clear daytime sky), fading with the air above it.
            const double amb = glm::smoothstep(-0.1, 0.25, sinE) * std::exp(-std::max(h, 0.0) / 15000.0);
            d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), (float)amb);
            // At night the sky's exposure is not physical (only ~5.5x the day's: see the Kessler dust), so a sunlit
            // plume against the dark sky reads far dimmer than it is; its sunlight is raised as the eye's sky darkens.
            d.col = glm::vec4(glm::vec3(tr * pc.albedo * nightBoost), (float)(2.0 + pc.albedo));
            d.cone = coneOf(toEnu(rel), 5.5 * std::max(sA, sP));
            plumes.push_back(c);
            // High up, once the expanding exhaust has swept up about its own mass of air it piles up at its rim: a shell,
            // limb-brightened (the jellyfish's bell). Seen across a near-round parcel the rim is a ring (7 Gaussians
            // round the line of sight); across a parcel drawn out along the trail it is a tube, whose limb is two lines
            // beside the trail (2 Gaussians, as long as the parcel, so neighbours join: rings on every parcel drew the
            // young trail as a string of circles).
            const double shellK = (h > 60000.0 && sig > 2000.0) ? 0.65 * glm::smoothstep(0.3, 2.0, sweep) : 0.0;
            if (shellK > 0.02)
            {
                const double coreShare = 1.0 - shellK;
                plumes.back().d.c.w = (float)(Sigma * coreShare);
                plumes.back().pri *= coreShare;
                const glm::dvec3 vE = toEnu(rel) / range; // the line of sight
                // The trail's direction across the line of sight, and how drawn out the parcel is along it.
                glm::dvec3 aP = a - glm::dot(a, vE) * vE;
                const double aPl = glm::length(aP);
                const double elong = aPl > 1e-3 ? (sA * aPl) / sP : 1.0;
                const bool tube = elong > 1.6;
                glm::dvec3 r1, r2;
                if (tube)
                {
                    r1 = aP / aPl;
                    r2 = glm::cross(vE, r1);
                }
                else
                {
                    r1 = glm::cross(vE, glm::dvec3(0, 0, 1));
                    r1 = glm::length(r1) > 1e-6 ? glm::normalize(r1) : glm::dvec3(1, 0, 0);
                    r2 = glm::cross(vE, r1);
                }
                const int K = tube ? 2 : 7;
                const double rr = 0.9 * sig, sr = std::max(0.42 * sig, sMin);
                for (int k = 0; k < K; ++k)
                {
                    glm::dvec3 nrm, tg;
                    double sT;
                    if (tube)
                    {
                        nrm = k == 0 ? r2 : -r2;
                        tg = r1;
                        sT = std::max(std::sqrt(sig * sig + 0.25 * pc.len * pc.len * aPl * aPl), sMin);
                    }
                    else
                    {
                        const double ang = glm::two_pi<double>() * ((double)k + h01(B.seed ^ (uint64_t)(pc.t0 * 10.0), (uint64_t)k, 31)) / K;
                        nrm = std::cos(ang) * r1 + std::sin(ang) * r2;
                        tg = -std::sin(ang) * r1 + std::cos(ang) * r2;
                        sT = std::max(0.9 * sig, sMin);
                    }
                    const glm::dvec3 off = rr * nrm;
                    // Each piece is stretched along the rim (the tangent) and the line of sight.
                    const double sN = sr;
                    const double iT = 1.0 / (sT * sT), iN = 1.0 / (sN * sN), iV = 1.0 / (sig * sig);
                    auto Mr = [&](int p, int q) { return iT * tg[p] * tg[q] + iN * nrm[p] * nrm[q] + iV * vE[p] * vE[q]; };
                    Cand cr;
                    cr.pri = pri * shellK / K;
                    GpuDustCloud &dr = cr.d;
                    const glm::dvec3 ce = toEnu(rel) + off;
                    dr.c = glm::vec4(glm::vec3(ce), (float)(Sigma * shellK / K));
                    dr.ia = glm::vec4((float)Mr(0, 0), (float)Mr(1, 1), (float)Mr(2, 2),
                                      (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sT * sN * sig)));
                    dr.ib = glm::vec4((float)Mr(0, 1), (float)Mr(0, 2), (float)Mr(1, 2), (float)amb);
                    dr.col = d.col;
                    dr.cone = coneOf(ce, 5.5 * std::max(sT, sig));
                    plumes.push_back(cr);
                }
            }
            if (L.plumeDbg.size() < 4000)
                L.plumeDbg.push_back({range / 1000.0, h / 1000.0, sig / 1000.0, Sigma, tauPeak, age, tr * pc.albedo, amb});
        }
    }
    // The plumes: the brightest per pixel of sky first, as many as fit.
    if (!plumes.empty())
    {
        std::sort(plumes.begin(), plumes.end(), [](const Cand &a, const Cand &b) { return a.pri > b.pri; });
        // At most 384 plume Gaussians: a wide one covers much of the screen and costs every pixel it covers
        // (cloud_march.comp's loop), ~3 ms at 1000.
        const uint32_t nMax = std::min<uint32_t>(kMaxDustClouds, n + 384u);
        for (const Cand &c : plumes)
        {
            if (n >= nMax)
                break;
            out[n++] = c.d;
            ++L.nPlumes;
        }
    }
    uint32_t *fh = static_cast<uint32_t *>(kFireMapped);
    fh[0] = kFireCount;
}

// ── Readouts, harness, UI ───────────────────────────────────────────────────────────────────────
nlohmann::json SatelliteSim::launchStateJson() const
{
    nlohmann::json j;
    j["enabled"] = launchesEnabled;
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    j["catalog"] = L.loaded ? "ok" : L.err;
    j["planes"] = L.planes.size();
    j["batches_now"] = L.nBatches;
    j["rockets_drawn"] = L.nRockets;
    j["train_satellites"] = L.nTrain;
    j["retiring_satellites"] = L.nRetire;
    j["plume_gaussians"] = L.nPlumes;
    j["reentries_now"] = L.nReentries;
    j["records_written"] = L.nWrites;
    j["free_chunks"] = L.freeChunks.size();
    j["slots_shown"] = std::count_if(L.slots.begin(), L.slots.end(), [](const SlotInfo &x) { return x.shown; });
    j["life_scale"] = launchLifeScale;
    {
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const glm::dvec3 eyeE = glm::normalize(glm::dvec3(obsDir)) * (kR + std::max((double)obsTerrainH + 2.0, (double)obsHeightOffset));
        const std::vector<LaunchSound> ls = launchSounds(now, eyeE, glm::radians(camera.azDeg));
        nlohmann::json sa = nlohmann::json::array();
        for (const LaunchSound &x : ls)
            sa.push_back({{"level", x.level}, {"km", x.km}, {"pan", x.pan}, {"stage", x.stage}, {"burn", x.burn},
                          {"t_plus_s", x.tau}, {"alt_km", x.altKm}});
        j["sounds"] = sa;
        j["thunder_level_now"] = thunderLevelNow_;
    }
    {
        // The upper stages flying now: their timeline, in seconds from now (deployment, the turn, the deorbit burn,
        // the entry interface), and what the stage is doing.
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        nlohmann::json us = nlohmann::json::array();
        for (uint64_t key : L.live)
        {
            auto it = L.batches.find(key);
            if (it == L.batches.end() || !it->second.valid)
                continue;
            const LBatch &B = it->second;
            if (now < B.tL || now > B.tStageGone + 480.0)
                continue;
            const launch::Vehicle &vh = L.cat.vehicles[B.veh];
            glm::dvec3 P;
            uint8_t burn = 0;
            const bool fl = LaunchImpl::rocketPos(L, B, 1, now - B.tL, P, &burn);
            const glm::dvec3 nose = LaunchImpl::stageNose(L, B, 1, now - B.tL);
            glm::dvec3 P1;
            double retroDeg = -1.0;
            if (fl && LaunchImpl::rocketPos(L, B, 1, now - B.tL - 1.0, P1) && glm::length(nose) > 0.5)
                retroDeg = glm::degrees(std::acos(glm::clamp(glm::dot(nose, -glm::normalize(P - P1)), -1.0, 1.0)));
            us.push_back({{"vehicle", vh.id}, {"t_plus_s", now - B.tL}, {"deploy_in_s", B.tD - now},
                          {"release_end_in_s", (B.mem.empty() ? B.tD : B.mem.back().tRel) - now},
                          {"deorbit_burn_in_s", B.tDeorbBurn - now}, {"burn_s", B.burnDur},
                          {"entry_interface_in_s", B.tStageGone - now}, {"returns", !vh.upperDeorbit},
                          {"flying", fl}, {"burning", burn != launch::BURN_NONE},
                          {"alt_km", fl ? (glm::length(P) - kR) / 1000.0 : 0.0}, {"nose_from_retrograde_deg", retroDeg}});
        }
        j["upper_stages"] = us;
    }
    j["plume_gain"] = launchPlumeGain;
    // The plume parcels drawn: a sample (the largest optical depths first).
    std::vector<LaunchState::PlumeDbg> pd = L.plumeDbg;
    std::sort(pd.begin(), pd.end(), [](const auto &a, const auto &b) { return a.tauPeak > b.tauPeak; });
    nlohmann::json arr = nlohmann::json::array();
    for (size_t i = 0; i < pd.size() && i < 12; ++i)
        arr.push_back({{"range_km", pd[i].rangeKm}, {"alt_km", pd[i].altKm}, {"sigma_km", pd[i].sigKm},
                       {"cross_section_m2", pd[i].Sigma}, {"tau_peak", pd[i].tauPeak}, {"age_s", pd[i].age},
                       {"sun_col", {pd[i].col.x, pd[i].col.y, pd[i].col.z}}, {"sky", pd[i].amb}});
    j["plume_sample"] = arr;
    std::sort(pd.begin(), pd.end(), [](const auto &a, const auto &b) { return a.altKm > b.altKm; });
    nlohmann::json hi = nlohmann::json::array();
    for (size_t i = 0; i < pd.size() && i < 8; ++i)
        hi.push_back({{"range_km", pd[i].rangeKm}, {"alt_km", pd[i].altKm}, {"sigma_km", pd[i].sigKm},
                      {"cross_section_m2", pd[i].Sigma}, {"tau_peak", pd[i].tauPeak}, {"age_s", pd[i].age},
                      {"sun_col", {pd[i].col.x, pd[i].col.y, pd[i].col.z}}});
    j["plume_high"] = hi;
    j["plume_parcels"] = pd.size();
    return j;
}

void formatSimClock(double tJ2000, char *buf, size_t n, bool withDate); // SatelliteSim.cpp

static std::string fmtUtc(double tJ2000)
{
    char b[48];
    formatSimClock(tJ2000, b, sizeof(b), true);
    return b;
}

// Every launch with lift-off in [now, now + hours] (builds the batches it needs).
nlohmann::json SatelliteSim::launchListJson(double hours) const
{
    nlohmann::json arr = nlohmann::json::array();
    if (!launchState)
        return arr;
    LaunchState &L = const_cast<LaunchState &>(*launchState);
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return arr;
    SatelliteSim &sim = const_cast<SatelliteSim &>(*this);
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    struct Item
    {
        double tL;
        uint64_t key;
    };
    std::vector<Item> items;
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        const launch::Program &pg = L.cat.programs[pl.prog];
        const double life = progLifeS(pg, (double)launchLifeScale);
        // Lift-off precedes arrival by the raise (+ under a day for the window).
        const double lo = t + (pg.raiseDays[0] - 0.5) * kDay, hi = t + hours * 3600.0 + (pg.raiseDays[1] + 2.5) * kDay;
        const int64_t k0 = (int64_t)std::floor((lo - (pl.nb - 1) * pl.gap) / life - pl.phase);
        const int64_t k1 = (int64_t)std::floor(hi / life - pl.phase);
        for (int64_t k = k0; k <= k1; ++k)
        {
            const double base = (k + pl.phase) * life;
            const int b0 = std::max(0, (int)std::ceil((lo - base) / pl.gap));
            const int b1 = std::min(pl.nb - 1, (int)std::floor((hi - base) / pl.gap));
            for (int b = b0; b <= b1; ++b)
            {
                const uint64_t key = batchKey((int)pi, k, b);
                auto it = L.batches.find(key);
                if (it == L.batches.end())
                {
                    LBatch nb;
                    nb.plane = (int)pi;
                    nb.k = k;
                    nb.b = b;
                    LaunchImpl::buildBatch(sim, L, nb, pl, life);
                    nb.touched = L.frame;
                    it = L.batches.emplace(key, std::move(nb)).first;
                }
                const LBatch &B = it->second;
                if (B.valid && B.tL >= t && B.tL <= t + hours * 3600.0)
                    items.push_back({B.tL, key});
            }
        }
    }
    std::sort(items.begin(), items.end(), [](const Item &a, const Item &b) { return a.tL < b.tL; });
    for (const Item &it : items)
    {
        const LBatch &B = L.batches.at(it.key);
        const LPlane &pl = L.planes[B.plane];
        nlohmann::json o;
        o["utc"] = fmtUtc(B.tL);
        o["in_hours"] = (B.tL - t) / 3600.0;
        o["program"] = L.cat.programs[B.prog].name;
        o["constellation"] = constellations[pl.constIdx].name;
        o["vehicle"] = L.cat.vehicles[B.veh].name;
        o["site"] = L.cat.sites[B.site].name;
        o["azimuth_deg"] = B.azDeg;
        o["payload"] = B.mem.size();
        o["incl_deg"] = glm::degrees(pl.inclRad);
        o["arrival_days"] = (B.tA - B.tL) / kDay;
        o["plane"] = B.plane;
        o["batch"] = B.b;
        o["refresh"] = B.k;
        o["pad_shifts"] = B.shifts;
        o["pad_rivals"] = B.rivals;
        // The Sun at the pad at lift-off (twilight launches make the jellyfish).
        const glm::dvec3 sun = sunDirEciAt(B.tL);
        o["sun_elev_deg"] = glm::degrees(std::asin(glm::clamp(glm::dot(sun, ecefToEci(L.cat.sites[B.site].ecef, B.tL)), -1.0, 1.0)));
        arr.push_back(o);
    }
    return arr;
}

// Why batches did not fly: every batch the cache holds after listing `hours`, counted by program and reason.
nlohmann::json SatelliteSim::launchWhyJson(double hours) const
{
    (void)launchListJson(hours);
    nlohmann::json j = nlohmann::json::object();
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (B.prog < 0)
            continue;
        const std::string pn = L.cat.programs[B.prog].name;
        const std::string w = B.valid ? std::string("ok") : B.why;
        if (!j.contains(pn))
            j[pn] = nlohmann::json::object();
        j[pn][w] = j[pn].value(w, 0) + 1;
    }
    return j;
}

// Puts the observer near the next launch: twilight (the Sun 5-16 deg below the pad's horizon) within a week if asked,
// else the next. Time 30 s before lift-off at 1x, the camera on the climb.
bool SatelliteSim::launchGoToNext(std::string &msg, bool twilight) { return launchGoToNextWhen(msg, twilight ? 1 : 0); }

// when: 0 the next, 1 twilight (the Sun 5-16 deg down at the pad), 2 day (above 15 deg), 3 night (below -20 deg).
bool SatelliteSim::launchGoToNextWhen(std::string &msg, int when, const std::string &vehicle)
{
    const bool twilight = when != 0;
    if (launchState)
        LaunchImpl::ensureCatalog(*launchState);
    if (!launchState || !launchState->loaded || !launchesEnabled)
    {
        msg = "launches are off (or data/launches.json did not load)";
        return false;
    }
    LaunchState &L = *launchState;
    const nlohmann::json list = launchListJson(!vehicle.empty() ? 24.0 * 30.0 : twilight ? 24.0 * 7.0 : 48.0);
    const nlohmann::json *pick = nullptr;
    for (const nlohmann::json &o : list)
    {
        if (o["in_hours"].get<double>() < 0.02)
            continue;
        if (!vehicle.empty())
        {
            // A vehicle id or a part of its name, case-blind.
            std::string nm = o.value("vehicle", std::string());
            std::string q = vehicle;
            for (auto &ch : nm)
                ch = (char)std::tolower((unsigned char)ch);
            for (auto &ch : q)
                ch = (char)std::tolower((unsigned char)ch);
            std::string nmc;
            for (char ch : nm)
                if (std::isalnum((unsigned char)ch))
                    nmc += ch;
            std::string qc;
            for (char ch : q)
                if (std::isalnum((unsigned char)ch))
                    qc += ch;
            if (nmc.find(qc) == std::string::npos)
                continue;
        }
        const double se = o["sun_elev_deg"].get<double>();
        if (when == 0 || (when == 1 && se < -5.0 && se > -16.0) || (when == 2 && se > 15.0) || (when == 3 && se < -20.0))
        {
            pick = &o;
            break;
        }
    }
    if (!pick)
    {
        msg = !vehicle.empty() ? "no such launch in the next 30 days" : twilight ? "no such launch in the next week"
                                                                                : "no launch in the next 48 h";
        return false;
    }
    // Find its batch again.
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay + (*pick)["in_hours"].get<double>() * 3600.0;
    const LBatch *B = nullptr;
    for (const auto &kv : L.batches)
        if (kv.second.valid && std::abs(kv.second.tL - t0) < 1.0)
            B = &kv.second;
    if (!B)
    {
        msg = "launch not found";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tStart = B->tL - 30.0;
    const double days = std::floor(tStart / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tStart - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    // The observer: 60 km back from the pad against the ground track and 50 km to its side: the climb crosses the sky
    // ahead and the staging and the plume's spread are in view.
    const glm::dvec3 side = glm::normalize(glm::cross(B->G0, B->Eaz));
    const glm::dvec3 g = glm::normalize(B->G0 * kR - B->Eaz * 60000.0 + side * 50000.0);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = 0.0f; // on the ground (the height is above sea level, floored at the ground): it was left as it
                            // was, so a jump from orbit stayed in orbit
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    // Look at where the rocket is two minutes in.
    const Track &bt = L.booster[B->veh];
    const glm::dvec2 p = bt.at(std::min(120.0, bt.tEnd));
    const glm::dvec3 target = LaunchImpl::trackEcef(*B, p.x, p.y);
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 rel = target - up * kR;
    glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(rel);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = 70.0f;
    updatePositions(tStart);
    char b[300];
    snprintf(b, sizeof(b), "%s from %s in 30 s (%s, Sun %.1f deg)", (*pick)["vehicle"].get<std::string>().c_str(),
             (*pick)["site"].get<std::string>().c_str(), (*pick)["constellation"].get<std::string>().c_str(),
             (*pick)["sun_elev_deg"].get<double>());
    msg = b;
    return true;
}

// Aims the camera at the nearest flying rocket's stage (0 booster, 1 upper stage; the harness's `launch look`).
bool SatelliteSim::launchLook(int stage, std::string &msg)
{
    if (!launchState || !launchState->loaded)
    {
        msg = "no launch data";
        return false;
    }
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    double best = 1e30, bestTau = 0.0;
    glm::dvec3 bestP(0.0);
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (!B.valid || t < B.tL - 600.0 || t > B.tStageGone + 60.0)
            continue;
        glm::dvec3 P;
        if (!LaunchImpl::rocketPos(L, B, stage, t - B.tL, P))
            continue;
        const double d = glm::length(P - eye);
        if (d < best)
        {
            best = d;
            bestP = P;
            bestTau = t - B.tL;
        }
    }
    if (best > 1e29)
    {
        msg = "no rocket flying";
        return false;
    }
    const glm::dvec3 rel = (bestP - eye) / best;
    const glm::dvec3 e(glm::dot(rel, glm::dvec3(eci2enuX)), glm::dot(rel, glm::dvec3(eci2enuY)),
                       glm::dot(rel, glm::dvec3(eci2enuZ)));
    aimCameraAzEl((float)glm::degrees(std::atan2(e.x, e.y)), (float)glm::degrees(std::asin(glm::clamp(e.z, -1.0, 1.0))));
    char b[160];
    snprintf(b, sizeof(b), "stage %d at T%+.0f s, %.0f km, az %.1f el %.1f", stage, bestTau, best / 1000.0,
             glm::degrees(std::atan2(e.x, e.y)), glm::degrees(std::asin(glm::clamp(e.z, -1.0, 1.0))));
    msg = b;
    return true;
}

// Self-check of the phasing (harness `launch check`): for the batches built so far, how far a train member is from
// the upper stage at deployment and from its slot at arrival, and a retiree from its slot when it leaves.
nlohmann::json SatelliteSim::launchCheckJson() const
{
    nlohmann::json j;
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    double maxArr = 0.0, sumArr = 0.0, maxDep = 0.0, sumDep = 0.0, maxRet = 0.0, maxEps = 0.0;
    size_t nMem = 0, nB = 0, nResid = 0;
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (!B.valid)
            continue;
        ++nB;
        for (const LMember &mb : B.mem)
        {
            // At its release the member is at the dispenser: the stage's aft end + the bay's offset (tens of m).
            glm::dvec3 Pst;
            const bool stageOk = LaunchImpl::rocketPos(L, B, 1, mb.tRel - B.tL, Pst);
            glm::dvec3 r, v;
            const glm::dvec3 slotA = satOrbitStateAt(rosterElemsOf(satOrbits[mb.sat]), B.tA).posEci;
            if (LaunchImpl::memberState(*this, B, mb, 0, B.tA - 1e-3, r, v))
            {
                const double d = glm::length(r - slotA);
                maxArr = std::max(maxArr, d);
                sumArr += d;
            }
            if (stageOk && LaunchImpl::memberState(*this, B, mb, 0, mb.tRel, r, v))
            {
                const double d = glm::length(r - Pst);
                maxDep = std::max(maxDep, d);
                sumDep += d;
            }
            if (LaunchImpl::memberState(*this, B, mb, 1, B.tA, r, v))
                maxRet = std::max(maxRet, glm::length(r - slotA));
            maxEps = std::max(maxEps, std::abs(mb.eps));
            nResid += std::abs(mb.eps) > 1e-9;
            ++nMem;
        }
    }
    // The stages' attitude: the fastest turn of the (smoothed) nose over three flights, sampled every 0.5 s (no snaps:
    // the booster's flip, the turn to retrograde and a ship's pitch-up are slews), and the largest position step of the
    // upper stage across its deorbit burn and entry interface (continuity).
    {
        double maxRate[2] = {0.0, 0.0}, atTau[2] = {0.0, 0.0}, maxJump = 0.0;
        int nf = 0;
        for (const auto &kv : L.batches)
        {
            const LBatch &B = kv.second;
            if (!B.valid || nf >= 3)
                continue;
            ++nf;
            for (int stage = 0; stage < 2; ++stage)
            {
                const double tEnd = stage == 0 ? L.booster[B.veh].tEnd : B.tStageGone - B.tL;
                glm::dvec3 prev(0.0);
                for (double tau = stage == 0 ? 1.0 : L.cat.vehicles[B.veh].t1 + 1.0; tau < tEnd - 0.5; tau += 0.5)
                {
                    const glm::dvec3 n = LaunchImpl::stageNose(L, B, stage, tau);
                    if (glm::length(n) > 0.5 && glm::length(prev) > 0.5)
                    {
                        const double r = glm::degrees(std::acos(glm::clamp(glm::dot(n, prev), -1.0, 1.0))) / 0.5;
                        if (r > maxRate[stage])
                        {
                            maxRate[stage] = r;
                            atTau[stage] = tau;
                        }
                    }
                    prev = n;
                }
            }
            // Position continuity: 1-s steps vs the speed through the burn and to the entry interface (and the
            // returning ship's entry track just after it).
            const bool ret = !L.cat.vehicles[B.veh].upperDeorbit;
            glm::dvec3 Pp(0.0);
            bool have = false;
            for (double t = B.tDeorbBurn - 5.0; t < B.tStageGone + (ret ? 10.0 : 0.0); t += 1.0)
            {
                glm::dvec3 P;
                if (t < B.tStageGone)
                {
                    if (!LaunchImpl::rocketPos(L, B, 1, t - B.tL, P))
                        continue;
                }
                else
                    P = LaunchImpl::shipEntry(B, t - B.tStageGone, nullptr, nullptr);
                if (have)
                    maxJump = std::max(maxJump, glm::length(P - Pp));
                Pp = P;
                have = true;
                if (t > B.tDeorbBurn + 60.0 && t < B.tStageGone - 20.0)
                {
                    t = B.tStageGone - 20.0; // (the coast between: skip)
                    have = false;
                }
            }
        }
        j["booster_nose_max_deg_per_s"] = maxRate[0];
        j["booster_nose_max_at_t_plus_s"] = atTau[0];
        j["upper_nose_max_deg_per_s"] = maxRate[1];
        j["upper_nose_max_at_t_plus_s"] = atTau[1];
        j["upper_max_step_m_per_s"] = maxJump; // ~7.7-7.9 km/s: no jump
    }
    j["batches"] = nB;
    j["members"] = nMem;
    j["arrival_err_m_max"] = maxArr;
    j["arrival_err_m_mean"] = nMem ? sumArr / nMem : 0.0;
    j["deploy_err_km_max"] = maxDep / 1000.0;
    j["deploy_err_km_mean"] = nMem ? sumDep / nMem / 1000.0 : 0.0;
    j["retiree_start_err_m_max"] = maxRet;
    j["phasing_residual_rad_max"] = maxEps;
    j["members_with_residual"] = nResid;
    return j;
}

// Selects the nearest drawn train member (kind 0) or retiree (1) above the horizon (harness `launch select`).
bool SatelliteSim::launchSelect(int kind, std::string &msg)
{
    if (!launchState)
        return false;
    const LaunchState &L = *launchState;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 up = glm::normalize(eye);
    double best = 1e30;
    int bestSlot = -1;
    for (uint32_t s = 0; s < (uint32_t)L.slots.size(); ++s)
    {
        const SlotInfo &si = L.slots[s];
        if (!si.shown || (int)(si.owner % 2) != kind)
            continue;
        const glm::dvec3 P = satOrbitStateAt(L.elems[s], (double)simDayJ2000 * 86400.0 + simSecInDay).posEci;
        const glm::dvec3 rel = P - eye;
        const double d = glm::length(rel);
        if (glm::dot(rel / d, up) < 0.05 || d >= best)
            continue;
        best = d;
        bestSlot = (int)s;
    }
    if (bestSlot < 0)
    {
        msg = "none above the horizon";
        return false;
    }
    selectSatellite((int)(activeSatCount + (uint32_t)bestSlot));
    char b[240];
    snprintf(b, sizeof(b), "%s, %.0f km away, %.0f km up", launchLabel((uint32_t)bestSlot).c_str(), best / 1000.0,
             (glm::length(satOrbitStateAt(L.elems[bestSlot], (double)simDayJ2000 * 86400.0 + simSecInDay).posEci) - kR) / 1000.0);
    msg = b;
    return true;
}

// The next retiree re-entry over a dark sky (within 48 h; harness `launch reentry`): the observer beside its track,
// 25 s before its breakup, at 1x.
bool SatelliteSim::launchGoToNextReentry(std::string &msg)
{
    if (!launchState || !launchState->loaded)
    {
        msg = "no launch data";
        return false;
    }
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    double bestT = 1e300;
    glm::dvec3 bestR(0.0), bestV(0.0);
    uint32_t bestSat = 0;
    for (const auto &kv : L.batches)
    {
        const LBatch &B = kv.second;
        if (!B.valid)
            continue;
        for (const LMember &mb : B.mem)
        {
            if (mb.tEnd <= t + 60.0 || mb.tEnd > t + 48.0 * 3600.0 || mb.tEnd >= bestT)
                continue;
            glm::dvec3 r, v;
            if (!LaunchImpl::memberState(*this, B, mb, 1, mb.tEnd - 1.0, r, v))
                continue;
            // Dark there?
            const glm::dvec3 sun = sunDirEciAt(mb.tEnd);
            if (glm::dot(glm::normalize(r), sun) > -0.14)
                continue;
            bestT = mb.tEnd;
            bestR = r;
            bestV = v;
            bestSat = mb.sat;
        }
    }
    if (bestT > 1e299)
    {
        msg = "no retiree re-enters over a dark sky in the next 48 h (batches are built as time reaches them)";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tb = bestT + 81.0; // the breakup (fbBreakupT)
    const double t0 = tb - 25.0;
    const double days = std::floor(t0 / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = t0 - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    const glm::dvec3 U = glm::normalize(bestR);
    glm::dvec3 A = bestV - glm::dot(bestV, U) * U;
    A = glm::normalize(A);
    const glm::dvec3 C = glm::normalize(glm::cross(A, U));
    // The breakup is ~600 km downrange of the glow's start: watch from beside that point.
    const glm::dvec3 Bp = glm::normalize(U * kR + A * 600000.0);
    const glm::dvec3 G = glm::normalize(Bp * kR + C * 120000.0 - A * 80000.0);
    const glm::dvec3 g = eciToEcef(G, t0);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = 0.0f; // on the ground
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 rel = eciToEcef(Bp * (kR + 78000.0), tb) - up * kR;
    glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(rel);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = 80.0f;
    updatePositions(t0);
    char b[240];
    snprintf(b, sizeof(b), "%s (retired) re-enters: breakup in 25 s", satLabel((int)bestSat).c_str());
    msg = b;
    return true;
}

// ── Settings: Constellations tab, LAUNCHES ──────────────────────────────────────────────────────
void SatelliteSim::buildLaunchSettingsSection(const UIInput &inp, UIRenderer &ui)
{
    uiSection("ConstSec", 3, "LAUNCHES");
    bool on = launchesEnabled;
    if (uiToggleRow(inp, ui, "LaunchOn", 0, "Launches and retirements", on,
                    "Every constellation is kept flying: each orbital plane is refreshed once per design life by a "
                    "launch from a site under it. The train climbs to its slots, the satellites it replaces deorbit "
                    "and re-enter. Sites, rockets and programs: launches.json next to the exe."))
        launchesEnabled = on;
    if (!launchesEnabled)
        return;
    static const char *kLife[] = {"x0.5", "x1", "x2", "x4"};
    static const float kLifeV[] = {0.5f, 1.0f, 2.0f, 4.0f};
    int lSel = 1;
    for (int i = 0; i < 4; ++i)
        if (std::abs(launchLifeScale - kLifeV[i]) < 1e-3f)
            lSel = i;
    const int l = uiChoiceRow(inp, ui, "LaunchLife", 0, "Satellite design life", kLife, 4, lSel,
                              "x1 = each program's life (5-7 years). Shorter: more launches and re-entries (Starmind at "
                              "x1 is already ~13 Starship launches a day). Reshuffles the schedule.");
    if (l >= 0 && l != lSel)
        launchLifeScale = kLifeV[l];
    static const char *kPlume[] = {"Off", "x0.3", "x1", "x3"};
    static const float kPlumeV[] = {0.0f, 0.3f, 1.0f, 3.0f};
    int pSel = 2;
    for (int i = 0; i < 4; ++i)
        if (std::abs(launchPlumeGain - kPlumeV[i]) < 1e-3f)
            pSel = i;
    const int pg = uiChoiceRow(inp, ui, "LaunchPlume", 0, "Exhaust plumes", kPlume, 4, pSel,
                               "The rockets' exhaust: the trail near the ground, and at twilight the sunlit bell high up "
                               "(the 'jellyfish'), lit while the ground below is dark.");
    if (pg >= 0 && pg != pSel)
        launchPlumeGain = kPlumeV[pg];
    if (kesslerEnabled)
    {
        static const char *kAfter[] = {"Stand down", "Resume"};
        const int as = uiChoiceRow(inp, ui, "LaunchAfterStorm", 0, "After the Sun's Fury", kAfter, 2, launchAfterStorm,
                                   "Stand down: no launches from the storm on (the shells are filling with debris; no "
                                   "replacements, no retirements). Resume: launches return three weeks after it (the new "
                                   "satellites do not collide with the debris: they are not part of the cascade).");
        if (as >= 0 && as != launchAfterStorm)
            launchAfterStorm = as;
    }
    std::string why;
    if (uiButton(inp, ui, "LaunchNext", 0, "Watch the next launch", "Jumps to 30 s before the next lift-off, beside the pad."))
    {
        launchGoToNextWhen(why, 0);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (uiButton(inp, ui, "LaunchNext", 1, "Watch a twilight launch",
                 "The next lift-off within a week with the Sun 5-16 degrees below the pad's horizon: its exhaust high up "
                 "catches the sunlight."))
    {
        launchGoToNextWhen(why, 1);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (uiButton(inp, ui, "LaunchReentry", 0, "Watch a retired satellite re-enter",
                 "The next retiree coming down over a dark sky (48 h): 25 s before it breaks up."))
    {
        launchGoToNextReentry(why);
        snprintf(kUiMsg, sizeof(kUiMsg), "%s", why.c_str());
    }
    if (!launchState)
        return;
    LaunchState &L = *launchState;
    // The next launch, refreshed every few seconds (it walks the schedule).
    const double now = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
    if (now - L.uiListAt > 3.0 || std::abs(((double)simDayJ2000 * 86400.0 + simSecInDay) - L.uiListSim) > 600.0)
    {
        L.uiListAt = now;
        L.uiListSim = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const nlohmann::json list = launchListJson(24.0);
        L.uiNext.clear();
        L.uiCount24 = (int)list.size();
        if (!list.empty())
        {
            const nlohmann::json &o = list[0];
            char b[200];
            snprintf(b, sizeof(b), "%s, %s, in %.1f h", o["vehicle"].get<std::string>().c_str(),
                     o["site"].get<std::string>().c_str(), o["in_hours"].get<double>());
            L.uiNext = b;
        }
    }
    const float lw = (float)fs(170);
    uiKV("LaunchKV", 0, "Next launch", L.uiNext.empty() ? "none in 24 h" : L.uiNext.c_str(), lw);
    uiKV("LaunchKV", 1, "Launches in the next 24 h", uiKitStr("%d", L.uiCount24), lw);
    uiKV("LaunchKV", 2, "Trains climbing", uiKitStr("%u satellites", L.nTrain), lw);
    uiKV("LaunchKV", 3, "Retired, deorbiting", uiKitStr("%u satellites", L.nRetire), lw);
}

// The rockets' roar at the listener (eye: ECI-like Earth-centred position the thunder uses, Earth-fixed axes): for
// each burning rocket within 150 km, the sound arriving now left it at the retarded time tau (tau + r(tau) / c = now);
// loudness as the flow's square root over the distance, thinning with the air at the rocket. Pure function of time.
std::vector<SatelliteSim::LaunchSound> SatelliteSim::launchSounds(double now, const glm::dvec3 &eyeEcef, float camAz) const
{
    std::vector<LaunchSound> out;
    if (!launchState || !launchesEnabled)
        return out;
    const LaunchState &L = *launchState;
    if (!L.loaded)
        return out;
    constexpr double kC = 343.0;
    const glm::dvec3 upE = glm::normalize(eyeEcef);
    const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), upE));
    const glm::dvec3 nE = glm::cross(upE, eE);
    for (uint64_t key : L.live)
    {
        auto it = L.batches.find(key);
        if (it == L.batches.end())
            continue;
        const LBatch &B = it->second;
        if (now < B.tL || now > B.tL + 1200.0)
            continue;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        if (glm::length(B.G0 * kR - eyeEcef) > 2.0e6)
            continue; // nothing of it is ever within hearing
        const double T = now - B.tL;
        for (int stage = 0; stage < 2; ++stage)
        {
            // The retarded time (Earth-fixed positions: the air carries the sound): the latest emission tau whose
            // sound reaches the listener now, g(tau) = (T - tau) - |P(tau) - eye| / c = 0. g(T) < 0 (no time to travel);
            // scan back over the last 440 s (150 km of sound) for the latest sign change, then bisect. (Until 2026-10-19 a fixed-point iteration
            // that did not converge for a fast, distant stage: it alternated between the stage and the pad, and an upper
            // stage 1200 km downrange was heard burning 300 m away, at full level, minutes after it left.)
            auto posE = [&](double tau, glm::dvec3 &E, uint8_t *b) {
                glm::dvec3 Pe;
                if (!LaunchImpl::rocketPos(L, B, stage, tau, Pe, b))
                    return false;
                E = eciToEcef(Pe, B.tL + tau);
                return true;
            };
            auto g = [&](double tau, bool &ok) {
                glm::dvec3 E;
                ok = posE(tau, E, nullptr);
                return ok ? (T - tau) - glm::length(E - eyeEcef) / kC : -1e9;
            };
            double hi = T, lo = -1.0;
            bool okHi = false;
            double gHi = g(hi, okHi);
            for (double tau = T - 2.0; tau >= std::max(0.0, T - 440.0) - 1e-9; tau -= 2.0)
            {
                bool okT = false;
                const double gT = g(tau, okT);
                // A crossing either way: g falls with tau while the stage recedes or climbs slower than sound, and
                // rises while it closes faster than sound (a booster's supersonic descent to its landing pad).
                if (okT && okHi && (gT >= 0.0) != (gHi >= 0.0))
                {
                    lo = tau;
                    break;
                }
                hi = tau;
                gHi = gT;
                okHi = okT;
            }
            if (lo < 0.0)
                continue; // nothing it emitted is arriving now (not arrived yet, gone, or out of hearing)
            {
                bool okL = false;
                const bool posLo = g(lo, okL) >= 0.0;
                for (int i = 0; i < 12; ++i)
                {
                    const double mid = 0.5 * (lo + hi);
                    bool okM = false;
                    const double gM = g(mid, okM);
                    if (okM && (gM >= 0.0) == posLo)
                        lo = mid;
                    else
                        hi = mid;
                }
            }
            double tau = lo;
            glm::dvec3 P;
            uint8_t burn = 0;
            if (!posE(tau, P, &burn) || burn == launch::BURN_NONE)
                continue;
            if (stage == 1 && tau < vh.t1)
                continue; // one vehicle before staging (the booster carries the roar)
            const double r = std::max(glm::length(P - eyeEcef), 200.0);
            if (r > 150000.0)
                continue;
            const double h = glm::length(P) - kR;
            const double flow = stage == 0 ? vh.flow1 * (burn == launch::BURN_ASCENT ? 1.0 : 0.3) : vh.flow2;
            const double lvl = 9.0 * std::sqrt(flow / 2500.0) * (1000.0 / r) * std::exp(-std::max(h, 0.0) / 20000.0) *
                               (1.0 - glm::smoothstep(80000.0, 150000.0, r));
            if (lvl < 1e-3)
                continue;
            const glm::dvec3 dir = (P - eyeEcef) / r;
            const double hE = glm::dot(dir, eE), hN = glm::dot(dir, nE);
            LaunchSound s;
            s.id = 0xA0000000u | (uint32_t)((key * 2 + (uint64_t)stage) & 0x0FFFFFFFu);
            s.level = (float)std::min(lvl, 8.0);
            s.km = (float)(r / 1000.0);
            s.pan = (float)(std::sin(std::atan2(hE, hN) - (double)camAz) * std::sqrt(hE * hE + hN * hN));
            s.stage = stage;
            s.burn = burn;
            s.tau = (float)tau;
            s.altKm = (float)(h / 1000.0);
            out.push_back(s);
        }
    }
    return out;
}

// The next stage venting while sunlit over a dark ground (within 30 days; harness `launch vent`): the observer 250 km
// to the side of the stage's track, 20 s before the vent, looking at it.
bool SatelliteSim::launchGoToVent(std::string &msg) { return launchGoToStageEvent(msg, 0); }
bool SatelliteSim::launchGoToShipEntry(std::string &msg) { return launchGoToStageEvent(msg, 1); }

// ev 0: a stage venting (sunlit, over a dark ground); 1: a returning stage's re-entry glow (over a dark ground).
bool SatelliteSim::launchGoToStageEvent(std::string &msg, int ev)
{
    if (!launchState)
        return false;
    LaunchImpl::ensureCatalog(*launchState);
    LaunchState &L = *launchState;
    const nlohmann::json list = launchListJson(24.0 * 30.0);
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
    for (const nlohmann::json &o : list)
    {
        const double tl = t0 + o["in_hours"].get<double>() * 3600.0;
        const LBatch *B = nullptr;
        for (const auto &kv : L.batches)
            if (kv.second.valid && std::abs(kv.second.tL - tl) < 1.0)
                B = &kv.second;
        if (!B || (ev == 0 && !B->vents) || (ev == 1 && L.cat.vehicles[B->veh].upperDeorbit))
            continue;
        // (ev 2: 50 s before the stage starts turning for its deorbit burn; ev 3: 10 s before it deploys.)
        const double tv = ev == 0   ? LaunchImpl::ventStart(L, *B) + 30.0
                          : ev == 1 ? B->tStageGone + 230.0
                          : ev == 2 ? B->tDeorbBurn - 100.0
                                    : B->tD + 40.0;
        glm::dvec3 P;
        if (ev != 1)
        {
            if (!LaunchImpl::rocketPos(L, *B, 1, tv - B->tL, P))
                continue;
        }
        else
        {
            P = LaunchImpl::shipEntry(*B, 230.0, nullptr, nullptr); // near the glow's brightest
        }
        const glm::dvec3 sun = sunDirEciAt(tv);
        const glm::dvec3 U = glm::normalize(P);
        const double sp = glm::dot(P, sun);
        const bool lit = sp > 0.0 || glm::length(P - sp * sun) > kR + 20000.0;
        if (ev >= 2 ? !lit : ((ev == 0 && !lit) || glm::dot(U, sun) > -0.12))
            continue; // a vent must be sunlit, both over a dark ground; a deorbit or a deployment sunlit
        if (followActive)
            stopFollow();
        if (trackActive)
            stopTrack();
        const double tS = tv - 50.0;
        const double days = std::floor(tS / 86400.0);
        simDayJ2000 = (int64_t)days;
        simSecInDay = tS - days * 86400.0;
        trailClearPending = true;
        timePaused = false;
        timeScaleIdx = 0;
        timeDir = 1.0f;
        const glm::dvec3 Cn = glm::normalize(B->N);
        const glm::dvec3 G = glm::normalize(U * kR + Cn * 250000.0);
        const glm::dvec3 g = eciToEcef(G, tS);
        setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
        const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
        const glm::dvec3 rel = eciToEcef(P, tv) - up * kR;
        glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
        east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
        const glm::dvec3 north = glm::cross(up, east);
        const glm::dvec3 rn = glm::normalize(rel);
        aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                      (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
        camera.fovYDeg = 60.0f;
        updatePositions(tS);
        char b[240];
        static const char *kWhat[] = {"vents in 20 s", "re-entry glow peaks in 20 s", "turns for its deorbit burn in 0 s",
                                      "deploys in 10 s"};
        snprintf(b, sizeof(b), "%s upper stage from %s %s, %.0f km away", o["vehicle"].get<std::string>().c_str(),
                 o["site"].get<std::string>().c_str(), kWhat[std::clamp(ev, 0, 3)], glm::length(rel) / 1000.0);
        msg = b;
        return true;
    }
    msg = ev == 0   ? "no venting stage sunlit over a dark ground in the next 30 days"
          : ev == 1 ? "no returning stage re-entering over a dark ground in the next 30 days"
                    : "no sunlit upper stage at that event in the next 30 days";
    return false;
}

// ── Transient selection: rockets, the ship coming home, re-entry fireballs ───────────────────────────
// Their heads are listed each frame (addSelTransient) by the code that draws them; the selected one's state goes into
// the reserved last launch slot (transientSatIdx()) as SatOrbitElems::fixedState, so the panel, Track and the
// harness see a satellite there. Nothing else ever owns that slot (launchInitBlock keeps its chunk out of the pool).
void SatelliteSim::addSelTransient(const SelTransient &s)
{
    if (selTransients.size() < 16384)
        selTransients.push_back(s);
}

void SatelliteSim::selTransientUpdate()
{
    selTransientsT = (double)simDayJ2000 * 86400.0 + simSecInDay; // the time the list was made at
    followTransientUpdate();
    if (!selIsTransient() || !launchState)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const SelTransient *f = nullptr;
    for (const SelTransient &s : selTransients)
        if (s.key == selTransientKey)
        {
            f = &s;
            break;
        }
    // The rocket goes on as another object: a stack after staging is its upper stage (the booster falls away), an
    // upper stage coming home its ship entry, a stage that deorbits its re-entry fireball. The selection follows it.
    SelTransient nx;
    if ((!f || transientIsStack(selTransientCur) != transientIsStack(*f)) && transientSuccessor(selTransientCur, nx))
    {
        for (const SelTransient &s : selTransients)
            if (s.key == nx.key)
            {
                f = &s;
                break;
            }
    }
    if (!f)
    {
        // Out of view this frame (behind the Earth, below the brightness cut) or over: gone for good after 2 s (or a
        // few frames of a fast warp).
        if (std::abs(t - selTransientSeenT) > transientGraceS())
        {
            stopTrack();
            selectedSatIndex = -1;
            selTransientKey = 0;
            formatSelectedSatInfo();
        }
        return;
    }
    selTransientTake(*f);
    formatSelectedSatInfo(); // the label carries altitude and speed
}

// ── Following a transient (2026-10-19) ─────────────────────────────────────────────────────────────
// The followed rocket stage / ship / fireball rides the follow slot (followSlotIdx), whatever is selected. Rockets and
// the ship are evaluated from the launch model at the frame's own time (transientExactState: no stale extrapolation
// at any warp); fireballs keep the list's quadratic. At a mission's seams the follow hands over to the object that
// continues it and the camera eases across the jump; when nothing does, it ends in place (stopFollowHere).
double SatelliteSim::transientGraceS() const
{
    // 2 s of sim time, or ~3 frames of a fast warp (a frame at 1 h/s is a minute).
    return std::max(2.0, 0.1 * (double)kTimeScales[timeScaleIdx]);
}

void SatelliteSim::transientWriteSlot(uint32_t slot, const SelTransient &s, double t0)
{
    if (!launchState || slot >= launchState->elems.size())
        return;
    SatOrbitElems &e = launchState->elems[slot];
    e = SatOrbitElems{};
    e.fixedState = true;
    e.fixPos = s.pos;
    e.fixVel = s.vel;
    e.fixAcc = s.acc;
    e.fixAxis = s.axis;
    e.fixT0 = t0;
    const uint32_t idx = activeSatCount + slot;
    if (idx < satOrbits.size())
    {
        SatOrbit &o = satOrbits[idx];
        o.typeIdx = s.type >= 0 ? (uint32_t)s.type : UINT32_MAX;
        o.altM = (float)(glm::length(s.pos) - kR);
        const glm::dvec3 h = glm::cross(s.pos, s.vel);
        o.incl = glm::length(h) > 1.0 ? (float)std::acos(glm::clamp(h.z / glm::length(h), -1.0, 1.0)) : 0.0f;
        o.raan = 0.0f;
        o.meanMot = 0.0f;
    }
}

bool SatelliteSim::transientIsStack(const SelTransient &s) const
{
    if (s.kind != 1 || s.stage != 0 || !launchState)
        return false;
    auto it = launchState->batches.find(s.a);
    return it != launchState->batches.end() && it->second.veh >= 0 &&
           s.tau < launchState->cat.vehicles[it->second.veh].t1;
}

bool SatelliteSim::transientExactState(SelTransient &s, double t) const
{
    if (!launchState || (s.kind != 1 && s.kind != 2))
        return false;
    const LaunchState &L = *launchState;
    auto it = L.batches.find(s.a);
    if (it == L.batches.end() || it->second.veh < 0)
        return false;
    const LBatch &B = it->second;
    if (s.kind == 1)
    {
        const double tau = t - B.tL;
        glm::dvec3 P, P1, P2;
        uint8_t burn = 0;
        if (!LaunchImpl::rocketPos(L, B, s.stage, tau, P, &burn))
            return false;
        if (!LaunchImpl::rocketPos(L, B, s.stage, tau - 0.5, P1))
            P1 = P;
        if (!LaunchImpl::rocketPos(L, B, s.stage, tau - 1.0, P2))
            P2 = P1;
        s.pos = P;
        s.vel = (P - P1) / 0.5;
        s.acc = (P - 2.0 * P1 + P2) / 0.25;
        s.tau = tau;
        s.burn = burn;
        return true;
    }
    const double te = t - B.tStageGone;
    if (te < 0.0 || te > 480.0)
        return false;
    const glm::dvec3 P = LaunchImpl::shipEntry(B, te, nullptr, nullptr);
    const glm::dvec3 Pp = LaunchImpl::shipEntry(B, te - 6.0, nullptr, nullptr);
    const glm::dvec3 P2 = LaunchImpl::shipEntry(B, te - 12.0, nullptr, nullptr);
    s.pos = P;
    s.vel = (P - Pp) / 6.0;
    s.acc = (P - 2.0 * Pp + P2) / 36.0;
    s.tau = te;
    return true;
}

bool SatelliteSim::transientContinue(SelTransient &cur, double t, uint8_t &wantKind) const
{
    wantKind = 0;
    if (!launchState || (cur.kind != 1 && cur.kind != 2))
        return false;
    const LaunchState &L = *launchState;
    auto it = L.batches.find(cur.a);
    if (it == L.batches.end() || it->second.veh < 0)
        return false;
    const LBatch &B = it->second;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    auto typeOf = [&](const char *part) {
        auto ti = rocketTypes.find("rocket_" + vh.id + part);
        return ti != rocketTypes.end() ? ti->second : -1;
    };
    // The stack separates: its upper stage carries on (the booster is another object).
    if (cur.kind == 1 && cur.stage == 0 && cur.tau < vh.t1 && t - B.tL >= vh.t1)
    {
        cur.stage = 1;
        cur.key = (cur.a << 3) ^ 2ull;
        cur.type = typeOf("_upper");
    }
    if (cur.kind == 1 && cur.stage == 1 && t >= B.tStageGone)
    {
        if (vh.upperDeorbit)
        {
            wantKind = 4; // its re-entry fireball (from the list)
            return false;
        }
        cur.kind = 2; // the listing keeps the upper stage's key for its ship entry
        cur.type = typeOf("_upper");
    }
    return transientExactState(cur, t);
}

bool SatelliteSim::transientSuccessor(const SelTransient &cur, SelTransient &out) const
{
    const double t = selTransientsT;
    SelTransient c = cur;
    uint8_t want = 0;
    if (transientContinue(c, t, want) && (c.key != cur.key || c.kind != cur.kind))
    {
        out = c;
        return true;
    }
    if (want)
        for (const SelTransient &s : selTransients)
            if (s.kind == want && s.a == cur.a)
            {
                out = s;
                return true;
            }
    return false;
}

void SatelliteSim::followTakeTransient(const SelTransient &s, const glm::dvec3 &fromEci, double t)
{
    // The jump, Earth-fixed, eased out by updateFollow (metres at staging; tens of km where a stage's park orbit meets
    // its entry track).
    if (glm::length(fromEci) > 1.0)
        followHandD += eciToEcef(fromEci - s.pos, t);
    followTrCur = s;
    followTrKey = s.key;
    followTrSeenT = t;
    followWantKind = 0;
    transientWriteSlot(kLaunchCap - 2, s, t);
    followSatIndex = (int)followSlotIdx();
    followLaunchOwner = 0;
    snprintf(followLabel, sizeof(followLabel), "Following %s", transientLabelOf(s).c_str());
}

void SatelliteSim::followTransientUpdate()
{
    if (!followIsTransient() || !launchState)
        return;
    const double t = selTransientsT;
    if (followTrKey == 0)
    {
        // A record that ended: its fireball, once the list has it.
        for (const SelTransient &s : selTransients)
            if (followWantKind && s.kind == followWantKind && s.a == followWantA)
            {
                followTakeTransient(s, followTrCur.pos, t);
                return;
            }
        if (t > followWantUntil || t < followWantUntil - 3600.0)
            stopFollowHere("Go to ended: the object is gone. You stay here.");
        return;
    }
    // Along the mission: staging, the ship's entry (computed from the model, not looked up).
    {
        SelTransient c = followTrCur;
        uint8_t want = 0;
        if (transientContinue(c, t, want))
        {
            for (const SelTransient &s : selTransients)
                if (s.key == c.key)
                {
                    c.axis = s.axis; // the list's attitude
                    c.flare = s.flare;
                    c.dirEnu = s.dirEnu;
                    break;
                }
            if (c.key != followTrKey || c.kind != followTrCur.kind)
                followTakeTransient(c, followTrCur.pos, t);
            else
            {
                followTrCur = c;
                followTrSeenT = t;
                transientWriteSlot(kLaunchCap - 2, c, t);
                snprintf(followLabel, sizeof(followLabel), "Following %s", transientLabelOf(c).c_str());
            }
            return;
        }
        if (want)
        {
            followTrKey = 0;
            followWantKind = want;
            followWantA = followTrCur.a;
            followWantUntil = t + transientGraceS() + 5.0;
            return;
        }
    }
    // Fireballs (and a rocket the model no longer places): the list.
    for (const SelTransient &s : selTransients)
        if (s.key == followTrKey)
        {
            followTrCur = s;
            followTrSeenT = t;
            transientWriteSlot(kLaunchCap - 2, s, t);
            return;
        }
    if (std::abs(t - followTrSeenT) > transientGraceS())
        stopFollowHere("Go to ended: the object is gone. You stay here.");
}

// Each frame before updateFollow: the followed object may have ended since the last frame.
void SatelliteSim::followCheckTarget()
{
    if (!followActive || followSatIndex < 0)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const uint32_t idx = (uint32_t)followSatIndex;
    if (followIsTransient())
    {
        // Rockets at this frame's exact time (the list is a frame old: at a fast warp, many seconds).
        if (followTrKey != 0)
        {
            SelTransient c = followTrCur;
            if (transientExactState(c, t))
            {
                followTrCur = c;
                transientWriteSlot(kLaunchCap - 2, c, t);
            }
        }
        return;
    }
    if (idx >= satOrbits.size())
    {
        stopFollowHere("Go to ended: the object is gone. You stay here.");
        return;
    }
    if (rocketMeshSlotToTransient((int)idx) != (int)idx)
    {
        // An older path left the follow on a rocket's mesh slot (rebound every frame): its transient.
        if (selIsTransient())
            followTakeTransient(selTransientCur, glm::dvec3(0.0), t);
        else
            stopFollowHere("Go to ended: the object is gone. You stay here.");
        return;
    }
    // A record that ended in a fireball: ride a coasting copy of it until the list has the fireball (kind, a).
    auto awaitFireball = [&](uint8_t kind, uint64_t a) {
        const SatOrbitState st = satOrbitStateAt(orbitElemsOf(satOrbits[idx]), t);
        SelTransient g;
        g.pos = st.posEci;
        g.vel = st.velEci;
        const double r = std::max(glm::length(st.posEci), 1.0);
        g.acc = -satphot::kGM * st.posEci / (r * r * r);
        g.type = satOrbits[idx].typeIdx < satTypes.size() ? (int)satOrbits[idx].typeIdx : -1;
        followTrCur = g;
        followTrKey = 0;
        followWantKind = kind;
        followWantA = a;
        followWantUntil = t + transientGraceS() + 5.0;
        transientWriteSlot(kLaunchCap - 2, g, t);
        followSatIndex = (int)followSlotIdx();
        followLaunchOwner = 0;
    };
    const int32_t nowSec = (int32_t)std::clamp(std::floor(t - kesslerEpoch()), -2147483000.0, 2147483000.0);
    if (idx < activeSatCount)
    {
        // A roster satellite the cascade ended: destroyed (nothing left to ride) or re-entered (its fireball).
        if (kesslerEnabled && 2ull * idx + 1 < kRosterWords.size() && nowSec >= kRosterWords[2ull * idx + 1])
        {
            if (idx < kRosterDecay.size() && kRosterWords[2ull * idx + 1] == kRosterDecay[idx])
                awaitFireball(5, idx);
            else
                stopFollowHere("Go to ended: the satellite was destroyed. You stay here.");
        }
        return;
    }
    if (idx < poolIdx0())
    {
        uint32_t m = 0;
        const uint64_t owner = launchSlotOwner(idx - activeSatCount, m);
        if (followLaunchOwner != 0 && (owner != followLaunchOwner || m != followLaunchM))
        {
            const LBatch *B = nullptr;
            if (launchState)
            {
                auto it = launchState->batches.find(followLaunchOwner / 2);
                if (it != launchState->batches.end() && followLaunchM < it->second.mem.size())
                    B = &it->second;
            }
            const uint32_t sat = B ? B->mem[followLaunchM].sat : UINT32_MAX;
            if (followLaunchOwner % 2 == 0 && sat < activeSatCount && t >= B->tA - 60.0)
            {
                // A train member arrived: it IS its roster slot now (the camera eases across what is left).
                const glm::dvec3 a = satOrbitStateAt(launchOrbitElems(idx - activeSatCount), t).posEci;
                const glm::dvec3 b = satOrbitStateAt(orbitElemsOf(satOrbits[sat]), t).posEci;
                if (glm::length(a) > 1.0)
                    followHandD += eciToEcef(a - b, t);
                followSatIndex = (int)sat;
                followLaunchOwner = 0;
                snprintf(followLabel, sizeof(followLabel), "Following %s", satLabel((int)sat).c_str());
            }
            else if (followLaunchOwner % 2 == 1 && sat < activeSatCount)
                awaitFireball(3, sat);
            else
                stopFollowHere("Go to ended: the object is gone. You stay here.");
        }
        return;
    }
    const uint32_t j = idx - poolIdx0();
    if (j >= kPool.size() || j >= kPoolGen.size() || kPoolGen[j] != followPoolGen || nowSec < kPool[j].bornSec)
    {
        stopFollowHere("Go to ended: that debris piece does not exist at this time. You stay here.");
        return;
    }
    if (nowSec >= kPool[j].deathSec)
    {
        if (kPool[j].deathSec == kPool[j].decaySec)
            awaitFireball(5, idx);
        else
            stopFollowHere("Go to ended: the piece was destroyed. You stay here.");
    }
}

int SatelliteSim::rocketMeshSlotToTransient(int satIndex)
{
    if (satIndex >= 0 && (uint32_t)satIndex == followSlotIdx())
    {
        // The followed rocket's own mesh (follow's slot): as a selection, the same transient in the selection's slot.
        if (!followIsTransient() || followTrKey == 0)
            return -1;
        selTransientTake(followTrCur);
        selectedSatIndex = (int)transientSatIdx();
        selectedPlanetIndex = -1;
        return (int)transientSatIdx();
    }
    const uint32_t base = activeSatCount + kLaunchCap - 2 - kRocketMeshSlots;
    if (satIndex < (int)base || satIndex >= (int)(base + kRocketMeshSlots))
        return satIndex;
    const uint64_t key = rocketMeshKey[satIndex - (int)base];
    for (const SelTransient &s : selTransients)
        if (key != 0 && s.key == key)
        {
            selTransientTake(s);
            selectedSatIndex = (int)transientSatIdx();
            selectedPlanetIndex = -1;
            return (int)transientSatIdx();
        }
    return -1;
}

void SatelliteSim::selTransientTake(const SelTransient &s)
{
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    selTransientKey = s.key;
    selTransientCur = s;
    selTransientSeenT = t;
    if (!launchState)
        return;
    // The list's state is at the time the list was made (the visuals fill), possibly a frame ago.
    const double t0 = selTransientsT > 0.0 ? selTransientsT : t;
    SatOrbitElems &e = launchState->elems[kLaunchCap - 1];
    e = SatOrbitElems{};
    e.fixedState = true;
    e.fixPos = s.pos;
    e.fixVel = s.vel;
    e.fixAcc = s.acc;
    e.fixAxis = s.axis;
    e.fixT0 = t0;
    // A rocket has a model (Info, Go to, its mesh); a fireball none (Track only). The orbit rows read the state.
    const uint32_t idx = transientSatIdx();
    if (idx < satOrbits.size())
    {
        SatOrbit &o = satOrbits[idx];
        o.typeIdx = s.type >= 0 ? (uint32_t)s.type : UINT32_MAX;
        o.altM = (float)(glm::length(s.pos) - kR);
        const glm::dvec3 h = glm::cross(s.pos, s.vel);
        o.incl = glm::length(h) > 1.0 ? (float)std::acos(glm::clamp(h.z / glm::length(h), -1.0, 1.0)) : 0.0f;
        o.raan = 0.0f;
        o.meanMot = 0.0f;
    }
}

double SatelliteSim::rocketMeshPx(const SelTransient &s, double pixAngle, const glm::dvec3 &obsEci) const
{
    const SatMeshRenderer::TypeMesh *tm = s.type >= 0 ? meshRenderer.typeMesh(s.type) : nullptr;
    if (!tm || pixAngle <= 0.0)
        return 0.0;
    return 2.0 * std::max(0.1, (double)tm->boundsRadius) / std::max(glm::length(s.pos - obsEci), 1.0) / pixAngle;
}

// The rockets near enough to draw as meshes (recordMeshScene): the largest on screen past ~1.4 px, up to
// kRocketMeshSlots, each in a slot of the reserved chunk it keeps while it stays meshed; the selected rocket rides in
// the selection's own slot. Their elements are the frame's state (fixedState), so evalInstance poses them like any
// satellite. Uses the transients the last frame listed (the visuals fill after this pass): one frame of extrapolation.
std::vector<int> SatelliteSim::launchMeshJobs(double pixAngle, const glm::dvec3 &obsEci)
{
    std::vector<int> jobs;
    if (!launchState || !launchesEnabled)
        return jobs;
    LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    struct C
    {
        double px;
        size_t i;
    };
    std::vector<C> cs;
    for (size_t i = 0; i < selTransients.size(); ++i)
    {
        const SelTransient &s = selTransients[i];
        if (s.type < 0)
            continue;
        const double px = rocketMeshPx(s, pixAngle, obsEci);
        if (px > 1.4)
            cs.push_back({px, i});
    }
    std::sort(cs.begin(), cs.end(), [](const C &a, const C &b) { return a.px > b.px; });
    const uint32_t base = activeSatCount + kLaunchCap - 2 - kRocketMeshSlots; // slots base .. base + 5
    bool used[kRocketMeshSlots] = {};
    std::vector<size_t> want;
    for (const C &c : cs)
    {
        const SelTransient &s = selTransients[c.i];
        if (followIsTransient() && s.key == followTrKey)
            continue; // the followed one is drawn by follow mode (its own slot)
        if (selIsTransient() && s.key == selTransientKey)
        {
            jobs.push_back((int)transientSatIdx()); // the selection's slot (already written by selTransientTake)
            continue;
        }
        if (want.size() < (size_t)kRocketMeshSlots)
            want.push_back(c.i);
    }
    // Keep each rocket in the slot it had; give the rest the free ones.
    int slotOf[kRocketMeshSlots];
    for (int k = 0; k < kRocketMeshSlots; ++k)
        slotOf[k] = -1;
    for (size_t w = 0; w < want.size(); ++w)
        for (int k = 0; k < kRocketMeshSlots; ++k)
            if (!used[k] && rocketMeshKey[k] == selTransients[want[w]].key)
            {
                used[k] = true;
                slotOf[w] = k;
                break;
            }
    for (size_t w = 0; w < want.size(); ++w)
        if (slotOf[w] < 0)
            for (int k = 0; k < kRocketMeshSlots; ++k)
                if (!used[k])
                {
                    used[k] = true;
                    slotOf[w] = k;
                    break;
                }
    for (int k = 0; k < kRocketMeshSlots; ++k)
        if (!used[k])
            rocketMeshKey[k] = 0;
    for (size_t w = 0; w < want.size(); ++w)
    {
        const SelTransient &s = selTransients[want[w]];
        const int k = slotOf[w];
        rocketMeshKey[k] = s.key;
        const uint32_t slot = kLaunchCap - 2 - kRocketMeshSlots + (uint32_t)k;
        const uint32_t idx = base + (uint32_t)k;
        if (idx >= satOrbits.size() || slot >= L.elems.size())
            continue;
        SatOrbitElems &e = L.elems[slot];
        e = SatOrbitElems{};
        e.fixedState = true;
        e.fixPos = s.pos;
        e.fixVel = s.vel;
        e.fixAcc = s.acc;
        e.fixAxis = s.axis;
        // The transients are the last frame's: their time is the last fill's.
        e.fixT0 = selTransientsT;
        (void)t;
        satOrbits[idx].typeIdx = (uint32_t)s.type;
        jobs.push_back((int)idx);
    }
    return jobs;
}

std::string SatelliteSim::transientLabel() const { return transientLabelOf(selTransientCur); }

std::string SatelliteSim::transientLabelOf(const SelTransient &s) const
{
    if (!launchState)
        return "?";
    const LaunchState &L = *launchState;
    const double hKm = (glm::length(s.pos) - kR) / 1000.0;
    // Speed over the ground for a rocket near the pad reads better Earth-fixed; inertial elsewhere (orbit).
    const double vKms = glm::length(s.vel) / 1000.0;
    char b[200];
    const LBatch *B = nullptr;
    if (s.kind <= 2 || s.kind == 4)
    {
        auto it = L.batches.find(s.a);
        if (it != L.batches.end())
            B = &it->second;
    }
    const char *veh = B && B->veh >= 0 ? L.cat.vehicles[B->veh].name.c_str() : "Rocket";
    switch (s.kind)
    {
    case 1:
    {
        static const char *kBurn[] = {"coasting", "ascent burn", "boostback burn", "entry burn", "landing burn",
                                      "upper-stage burn"};
        const char *what = s.stage == 0 ? " booster" : " upper stage";
        if (B && s.stage == 0 && s.tau < L.cat.vehicles[B->veh].t1)
            what = ""; // still one vehicle
        const char *doing = kBurn[std::min<int>(s.burn, 5)];
        if (B && s.stage == 1 && s.tau > B->t2)
        {
            const double t = B->tL + s.tau;
            if (s.burn != launch::BURN_NONE)
                doing = "deorbit burn";
            else if (t < B->tD)
                doing = "coasting to deployment";
            else if (!B->mem.empty() && t < B->mem.back().tRel + 30.0)
                doing = "deploying";
            else if (t > B->tDeorbBurn - 150.0 && t < B->tDeorbBurn)
                doing = "turning for its deorbit burn";
            else if (t > B->tDeorbBurn)
                doing = "falling toward entry";
        }
        snprintf(b, sizeof(b), "%s%s (%s), T+%.0f s, %.0f km up, %.2f km/s", veh, what, doing, s.tau, hKm, vKms);
        break;
    }
    case 2:
        snprintf(b, sizeof(b), "%s ship re-entering, %.0f km up, %.2f km/s", veh, hKm, vKms);
        break;
    case 3:
        snprintf(b, sizeof(b), "Re-entry: %s (retired), %.0f km up", satLabel((int)s.a).c_str(), hKm);
        break;
    case 4:
        snprintf(b, sizeof(b), "Re-entry: %s upper stage, %.0f km up", veh, hKm);
        break;
    case 5:
        snprintf(b, sizeof(b), "Re-entry: %s, %.0f km up", satLabel((int)s.a).c_str(), hKm);
        break;
    default:
        snprintf(b, sizeof(b), "Re-entry: debris fragment, %.0f km up", hKm);
        break;
    }
    return b;
}

bool SatelliteSim::selectTransient(int kind, std::string &msg, int stage)
{
    const SelTransient *best = nullptr;
    for (const SelTransient &s : selTransients)
        if (s.flare > 0.0f && (kind == 0 || s.kind == kind) && (stage < 0 || s.stage == stage) &&
            (!best || s.flare > best->flare))
            best = &s;
    if (!best)
    {
        msg = "nothing of that kind drawn this frame";
        return false;
    }
    selTransientTake(*best);
    selectedPlanetIndex = -1;
    selectedSatIndex = (int)transientSatIdx();
    formatSelectedSatInfo();
    updateSelectedSkyDir();
    msg = transientLabel();
    return true;
}

// ── Missions window queries (2026-10-19, SatelliteSimMissions.cpp draws them) ───────────────────────────────────
namespace
{
// The inverse of batchKey.
void batchKeyDecode(uint64_t key, int &plane, int64_t &k, int &b)
{
    plane = (int)(uint32_t)(key >> 40);
    k = (int64_t)((key >> 20) & 0xFFFFFull) - (int64_t)(1 << 19);
    b = (int)(key & 0xFFFFFull);
}

double sunElevAtPad(const launch::Site &st, double t)
{
    const glm::dvec3 sun = sunDirEciAt(t);
    return glm::degrees(std::asin(glm::clamp(glm::dot(sun, ecefToEci(st.ecef, t)), -1.0, 1.0)));
}
} // namespace

void SatelliteSim::launchUpcoming(double t0, double t1, int filter, size_t maxN, std::vector<MissionLaunch> &out)
{
    out.clear();
    if (!launchState || !launchesEnabled)
        return;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale);
    struct Item
    {
        double tL;
        uint64_t key;
    };
    std::vector<Item> items;
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        const launch::Program &pg = L.cat.programs[pl.prog];
        if (filter == 1 && !pg.crew)
            continue;
        const double life = progLifeS(pg, (double)launchLifeScale);
        // As launchListJson: lift-off precedes the nominal arrival by the raise (+ the window).
        const double lo = t0 + (pg.raiseDays[0] - 0.5) * kDay, hi = t1 + (pg.raiseDays[1] + 2.5) * kDay;
        const int64_t k0 = (int64_t)std::floor((lo - (pl.nb - 1) * pl.gap) / life - pl.phase);
        const int64_t k1 = (int64_t)std::floor(hi / life - pl.phase);
        for (int64_t k = k0; k <= k1; ++k)
        {
            const double base = (k + pl.phase) * life;
            const int b0 = std::max(0, (int)std::ceil((lo - base) / pl.gap));
            const int b1 = std::min(pl.nb - 1, (int)std::floor((hi - base) / pl.gap));
            for (int b = b0; b <= b1; ++b)
            {
                const uint64_t key = batchKey((int)pi, k, b);
                auto it = L.batches.find(key);
                if (it == L.batches.end())
                {
                    LBatch nb;
                    nb.plane = (int)pi;
                    nb.k = k;
                    nb.b = b;
                    LaunchImpl::buildBatch(*this, L, nb, pl, life);
                    it = L.batches.emplace(key, std::move(nb)).first;
                }
                LBatch &B = it->second;
                B.touched = std::max(B.touched, L.frame); // kept while the window lists it
                if (!B.valid || B.tL < t0 || B.tL > t1)
                    continue;
                if (filter == 2)
                {
                    const double se = sunElevAtPad(L.cat.sites[B.site], B.tL);
                    if (!(se < -5.0 && se > -16.0))
                        continue;
                }
                items.push_back({B.tL, key});
            }
        }
    }
    std::sort(items.begin(), items.end(),
              [](const Item &a, const Item &b) { return a.tL < b.tL || (a.tL == b.tL && a.key < b.key); });
    for (size_t i = 0; i < items.size() && out.size() < maxN; ++i)
    {
        const LBatch &B = L.batches.at(items[i].key);
        const LPlane &pl = L.planes[B.plane];
        const launch::Program &pg = L.cat.programs[B.prog];
        MissionLaunch m;
        m.key = items[i].key;
        m.tL = B.tL;
        m.tA = B.tA;
        m.sunElevDeg = sunElevAtPad(L.cat.sites[B.site], B.tL);
        m.twilight = m.sunElevDeg < -5.0 && m.sunElevDeg > -16.0;
        m.crew = pg.crew;
        m.people = pg.people;
        m.payload = (int)B.mem.size();
        m.vehicle = L.cat.vehicles[B.veh].name;
        m.program = pg.name;
        m.site = L.cat.sites[B.site].name;
        m.target = constellations[pl.constIdx].name;
        m.spacecraft = pg.spacecraft;
        out.push_back(std::move(m));
    }
}

bool SatelliteSim::launchGoToKey(uint64_t key, bool moveObserver, double leadS, std::string &msg)
{
    if (!launchState || !launchesEnabled)
    {
        msg = "launches are off";
        return false;
    }
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
    {
        msg = "no launch data";
        return false;
    }
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale);
    auto it = L.batches.find(key);
    if (it == L.batches.end())
    {
        // Dropped from the cache since it was listed: build it again (a pure function of its key).
        int plane = 0, b = 0;
        int64_t k = 0;
        batchKeyDecode(key, plane, k, b);
        if (plane < 0 || plane >= (int)L.planes.size())
        {
            msg = "launch not found";
            return false;
        }
        LBatch nb;
        nb.plane = plane;
        nb.k = k;
        nb.b = b;
        const LPlane &pl = L.planes[plane];
        LaunchImpl::buildBatch(*this, L, nb, pl, progLifeS(L.cat.programs[pl.prog], (double)launchLifeScale));
        nb.touched = L.frame;
        it = L.batches.emplace(key, std::move(nb)).first;
    }
    const LBatch &B = it->second;
    if (!B.valid)
    {
        msg = "that launch does not fly (" + B.why + ")";
        return false;
    }
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tStart = B.tL - leadS;
    const double days = std::floor(tStart / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tStart - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    const launch::Site &st = L.cat.sites[B.site];
    if (moveObserver)
    {
        // On the ground ~12 km from the pad, beside the ground track and a little behind it (the climb crosses the sky
        // sideways, and the staging stays in view). The first candidate on land (the DEM reads ~0 over the sea).
        const glm::dvec3 side = glm::normalize(glm::cross(B.G0, B.Eaz));
        const glm::dvec3 cand[4] = {B.G0 * kR + side * 12000.0 - B.Eaz * 3000.0, B.G0 * kR - side * 12000.0 - B.Eaz * 3000.0,
                                    B.G0 * kR - B.Eaz * 12000.0, B.G0 * kR + side * 6000.0 - B.Eaz * 1500.0};
        glm::dvec3 g = glm::normalize(cand[0]);
        for (const glm::dvec3 &c : cand)
        {
            const glm::dvec3 u = glm::normalize(c);
            const float la = (float)glm::degrees(std::asin(glm::clamp(u.z, -1.0, 1.0)));
            const float lo = (float)glm::degrees(std::atan2(u.y, u.x));
            if (cpuTerrainHeightM(la, lo) > 10.0f)
            {
                g = u;
                break;
            }
        }
        setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
        obsHeightOffset = 0.0f; // on the ground (the height is above sea level, floored at the ground)
        // The camera on the pad, the pad low in the frame (the climb rises through the middle).
        const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
        const glm::dvec3 rel = B.G0 * kR - up * kR;
        glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
        east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
        const glm::dvec3 north = glm::cross(up, east);
        const glm::dvec3 rn = glm::normalize(rel);
        camera.fovYDeg = 60.0f;
        aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                      (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))) + 12.0f);
    }
    updatePositions(tStart);
    skyTaaHistValid = false; // a cut
    cv2HistoryValid = false;
    char b[320];
    if (L.cat.programs[B.prog].crew)
        snprintf(b, sizeof(b), "%s to %s from %s: lift-off in %.0f s", L.cat.vehicles[B.veh].name.c_str(),
                 constellations[L.planes[B.plane].constIdx].name.c_str(), st.name.c_str(), leadS);
    else
        snprintf(b, sizeof(b), "%s (%s) from %s: lift-off in %.0f s", L.cat.vehicles[B.veh].name.c_str(),
                 L.cat.programs[B.prog].name.c_str(), st.name.c_str(), leadS);
    msg = b;
    return true;
}

void SatelliteSim::launchOngoing(std::vector<MissionOngoing> &out)
{
    out.clear();
    if (!launchState || !launchesEnabled)
        return;
    const LaunchState &L = *launchState;
    if (!L.loaded)
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    struct Agg
    {
        int batches = 0, sats = 0;
        double next = 1e300, progSum = 0.0;
    };
    std::map<int, Agg> trains, retire;
    auto drawnRocket = [&](uint64_t key) {
        for (const SelTransient &s : selTransients)
            if (s.kind == 1 && s.a == key && s.flare > 0.0f)
                return true;
        return false;
    };
    auto phaseOf = [&](const LBatch &B, double tau, double &tNext) -> std::string {
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        if (tau < vh.t1)
        {
            tNext = B.tL + vh.t1;
            return "first stage burning; staging";
        }
        if (tau < B.t2)
        {
            tNext = B.tL + B.t2;
            return vh.recovery == 1   ? "upper stage burning, booster returning; orbit"
                   : vh.recovery == 2 ? "upper stage burning, booster to the droneship; orbit"
                                      : "upper stage burning, booster falling; orbit";
        }
        if (B.tL + tau < B.tD)
        {
            tNext = B.tD;
            return L.cat.programs[B.prog].crew ? "in the parking orbit; separation" : "in the parking orbit; deployment";
        }
        tNext = vh.upperDeorbit ? B.tStageRE : B.tStageGone;
        return vh.upperDeorbit ? "deployed; upper stage re-entry" : "deployed; ship coming home";
    };
    for (uint64_t key : L.live)
    {
        auto bit = L.batches.find(key);
        if (bit == L.batches.end() || !bit->second.valid)
            continue;
        const LBatch &B = bit->second;
        const launch::Program &pg = L.cat.programs[B.prog];
        const std::string &target = constellations[L.planes[B.plane].constIdx].name;
        const std::string &veh = L.cat.vehicles[B.veh].name;
        if (pg.crew)
        {
            if (t < B.tL || t >= B.tA)
                continue;
            MissionOngoing m;
            m.cat = 0;
            m.key = key;
            m.title = pg.spacecraft + " to " + target + " (" + std::to_string(pg.people) + " crew)";
            if (t < B.tL + B.t2)
                m.status = veh + ": " + phaseOf(B, t - B.tL, m.tNext);
            else
            {
                m.status = "Rendezvous with the station; docking";
                m.tNext = B.tA;
            }
            m.progress = (t - B.tL) / std::max(1.0, B.tA - B.tL);
            m.drawn = drawnRocket(key);
            out.push_back(std::move(m));
            continue;
        }
        if (t >= B.tL && t < B.tStageGone)
        {
            MissionOngoing m;
            m.cat = 2;
            m.key = key;
            m.title = veh + " - " + pg.name + " (" + std::to_string(B.mem.size()) + " satellites to " + target + ")";
            m.status = phaseOf(B, t - B.tL, m.tNext);
            m.status[0] = (char)std::toupper((unsigned char)m.status[0]);
            m.progress = (t - B.tL) / std::max(1.0, B.tStageGone - B.tL);
            m.drawn = drawnRocket(key);
            out.push_back(std::move(m));
        }
        if (t >= B.tD && t < B.tA)
        {
            Agg &a = trains[B.prog];
            ++a.batches;
            a.sats += (int)B.mem.size();
            a.next = std::min(a.next, B.tA);
            a.progSum += (t - B.tD) / std::max(1.0, B.tA - B.tD);
        }
        if (t >= B.tA && !B.ends.empty() && B.ends.back() > t)
        {
            Agg &a = retire[B.prog];
            auto it = std::upper_bound(B.ends.begin(), B.ends.end(), t);
            a.sats += (int)(B.ends.end() - it);
            a.next = std::min(a.next, *it);
            ++a.batches;
        }
    }
    std::sort(out.begin(), out.end(), [](const MissionOngoing &a, const MissionOngoing &b) {
        return a.cat < b.cat || (a.cat == b.cat && a.tNext < b.tNext);
    });
    for (const auto &kv : trains)
    {
        MissionOngoing m;
        m.cat = 3;
        m.prog = kv.first;
        m.count = kv.second.sats;
        m.title = L.cat.programs[kv.first].name + ": " + std::to_string(kv.second.batches) +
                  (kv.second.batches == 1 ? " train raising" : " trains raising");
        m.status = std::to_string(kv.second.sats) + " satellites climbing to their slots; next arrival";
        m.tNext = kv.second.next;
        m.progress = kv.second.progSum / std::max(1, kv.second.batches);
        out.push_back(std::move(m));
    }
    for (const auto &kv : retire)
    {
        MissionOngoing m;
        m.cat = 4;
        m.prog = kv.first;
        m.count = kv.second.sats;
        m.title = L.cat.programs[kv.first].name + ": " + std::to_string(kv.second.sats) + " retired, deorbiting";
        m.status = "Lowering toward re-entry; next re-entry";
        m.tNext = kv.second.next;
        out.push_back(std::move(m));
    }
}

std::vector<SatelliteSim::CrewStation> SatelliteSim::launchCrewStations()
{
    std::vector<CrewStation> out;
    if (!launchState)
        return out;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return out;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale);
    for (const LPlane &pl : L.planes)
        if (L.cat.programs[pl.prog].crew && !pl.sats.empty())
            out.push_back({(int)pl.constIdx, (int)pl.sats[0], pl.prog});
    return out;
}

bool SatelliteSim::launchSelectRocketOf(uint64_t key, std::string &msg)
{
    const SelTransient *best = nullptr;
    for (const SelTransient &s : selTransients)
        if (s.kind == 1 && s.a == key && s.flare > 0.0f && (!best || s.stage > best->stage))
            best = &s; // the upper stage (the payload's) once it flies alone
    if (!best)
    {
        msg = "not drawn from here (below the horizon or too faint)";
        return false;
    }
    selTransientTake(*best);
    selectedPlanetIndex = -1;
    selectedSatIndex = (int)transientSatIdx();
    formatSelectedSatInfo();
    updateSelectedSkyDir();
    msg = transientLabel();
    return true;
}

bool SatelliteSim::launchSelectNearestOf(int prog, int kind, std::string &msg)
{
    if (!launchState)
        return false;
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 up = glm::normalize(eye);
    double best = 1e30, bestAny = 1e30;
    int bestSlot = -1, bestSlotAny = -1;
    for (uint32_t s = 0; s < (uint32_t)L.slots.size(); ++s)
    {
        const SlotInfo &si = L.slots[s];
        if (!si.shown || (int)(si.owner % 2) != kind)
            continue;
        auto it = L.batches.find(si.owner / 2);
        if (it == L.batches.end() || it->second.prog != prog)
            continue;
        const glm::dvec3 rel = satOrbitStateAt(L.elems[s], t).posEci - eye;
        const double d = glm::length(rel);
        if (d < bestAny)
        {
            bestAny = d;
            bestSlotAny = (int)s;
        }
        if (glm::dot(rel / d, up) < 0.05 || d >= best)
            continue;
        best = d;
        bestSlot = (int)s;
    }
    const bool above = bestSlot >= 0;
    if (!above)
    {
        bestSlot = bestSlotAny;
        best = bestAny;
    }
    if (bestSlot < 0)
    {
        msg = "none up right now";
        return false;
    }
    selectSatellite((int)(activeSatCount + (uint32_t)bestSlot));
    char b[240];
    snprintf(b, sizeof(b), "%s, %.0f km away%s", launchLabel((uint32_t)bestSlot).c_str(), best / 1000.0,
             above ? "" : " (below your horizon)");
    msg = b;
    return true;
}
