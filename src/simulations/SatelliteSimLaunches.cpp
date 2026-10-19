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
    glm::dvec3 depOff{0.0}; // at deployment, the stage's position less the member's (radial, along, cross): eased out
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
    bool vents = false;
    bool parcelsBuilt = false;
    std::vector<launch::Parcel> parcels;
    double tVisEnd = 0.0;
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
    static constexpr uint32_t kStageRecs = 8192;
    uint32_t gpuGen = UINT32_MAX;
    bool clearGpu = true;         // zero the GPU block before anything is written (a reset)
    float lifeScale = -1.0f;      // the planes were built for this "life_scale"
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
    for (uint32_t c = kNumChunks; c-- > 0;)
        L.freeChunks.push_back(c);
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
        if (prog < 0 || consts[c].orbitCount < 2)
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
        const double life = std::max(0.25, pg.lifeYears * (double)lifeScale) * kYear;
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
    const double lifeS = std::max(0.25, pg.lifeYears * (double)sim.launchLifeScale) * kYear;
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
    const double raise = (pg.raiseDays[0] + (pg.raiseDays[1] - pg.raiseDays[0]) * h01(B.seed, 2, 1)) * kDay;
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
        const uint64_t self = batchKey(B.plane, B.k, B.b);
        const double myPri = h01(self, 9, 9);
        for (int iter = 0; iter < 4; ++iter)
        {
            bool conflict = false;
            for (int q : L.planesBySite[B.site])
            {
                const LPlane &pq = L.planes[q];
                const launch::Program &gq = L.cat.programs[pq.prog];
                const double lifeQ = std::max(0.25, gq.lifeYears * (double)sim.launchLifeScale) * kYear;
                const double lo = tL + (gq.raiseDays[0] - 1.0) * kDay, hi = tL + (gq.raiseDays[1] + 3.0) * kDay;
                const int64_t k0 = (int64_t)std::floor((lo - (pq.nb - 1) * pq.gap) / lifeQ - pq.phase);
                const int64_t k1 = (int64_t)std::floor(hi / lifeQ - pq.phase);
                for (int64_t k = k0; k <= k1 && !conflict; ++k)
                {
                    const double base = (k + pq.phase) * lifeQ;
                    const int b0 = std::max(0, (int)std::ceil((lo - base) / pq.gap));
                    const int b1 = std::min(pq.nb - 1, (int)std::floor((hi - base) / pq.gap));
                    for (int b = b0; b <= b1 && !conflict; ++b)
                    {
                        const uint64_t other = batchKey(q, k, b);
                        if (other == self || h01(other, 9, 9) < myPri)
                            continue;
                        const SatelliteSim::LaunchState::Cand co = candidate(sim, L, q, k, b);
                        conflict = co.site == B.site && std::abs(co.tL - tL) < 5400.0;
                    }
                }
                if (conflict)
                    break;
            }
            if (!conflict)
                break;
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
    }
    // Kessler: no launches for three weeks after the storm (the infrastructure, the debris).
    if (sim.kesslerEnabled && tL >= sim.kesslerEpoch() && tL < sim.kesslerEpoch() + 21.0 * kDay)
    {
        B.why = "stand-down after the storm";
        return;
    }
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const launch::Site &st = L.cat.sites[B.site];
    B.tL = tL;
    B.azDeg = az;
    B.t2 = std::max(vh.t2, vh.t1 + 60.0);
    B.tD = tL + B.t2 + vh.deployAfter;
    if (B.tD >= B.tA - 0.5 * kDay)
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
    for (size_t m = m0; m < m1; ++m)
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
    // The upper stage after deployment: a deorbit burn half a day later at most, the re-entry 25 min after it.
    B.tDeorbBurn = B.tD + 1800.0 + 3600.0 * h01(B.seed, 7, 1);
    B.tStageRE = B.tDeorbBurn + 1500.0;
    B.tStageGone = vh.upperDeorbit ? B.tStageRE : B.tD + 2400.0;
    B.vents = h01(B.seed, 8, 1) < vh.ventChance;
    const double boosterEnd = tL + L.booster[B.veh].tEnd;
    B.tVisEnd = std::max({boosterEnd + kPlumeAgeLow, B.tStageGone + kPlumeAgeHigh, tL + B.t2 + kPlumeAgeHigh});
    B.ends.clear();
    for (const LMember &mb : B.mem)
        B.ends.push_back(mb.tEnd);
    std::sort(B.ends.begin(), B.ends.end());
    // Each member's offset from the stage at deployment (memberState eases it out).
    {
        glm::dvec3 Pst;
        if (rocketPos(L, B, 1, B.tD - B.tL, Pst))
            for (size_t mi = 0; mi < B.mem.size(); ++mi)
            {
                LMember &mb = B.mem[mi];
                glm::dvec3 r, v;
                if (!memberState(sim, B, mb, 0, B.tD, r, v))
                    continue;
                const glm::dvec3 U = r / glm::length(r);
                const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
                const glm::dvec3 A = glm::cross(Cn, U);
                const glm::dvec3 d = Pst - r;
                // Released one after another: a string 120 m apart behind the stage.
                mb.depOff = glm::dvec3(glm::dot(d, U), glm::dot(d, A) - 120.0 * (double)mi, glm::dot(d, Cn));
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
    if (kind == 0 && glm::dot(mb.depOff, mb.depOff) > 0.0)
    {
        const double s = glm::clamp((t - B.tD) / (2.0 * 86400.0), 0.0, 1.0);
        const double f = 1.0 - s * s * (3.0 - 2.0 * s);
        if (f > 0.0)
        {
            const glm::dvec3 U = r / glm::length(r);
            const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
            const glm::dvec3 A = glm::cross(Cn, U);
            r += f * (mb.depOff.x * U + mb.depOff.y * A + mb.depOff.z * Cn);
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
    double r = B.rPark;
    if (vh.upperDeorbit && t > B.tDeorbBurn)
        r -= 30000.0 * std::min((t - B.tDeorbBurn) / 1500.0, 1.0); // falling toward its re-entry
    P = r * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
    if (burn && vh.upperDeorbit && t >= B.tDeorbBurn && t < B.tDeorbBurn + 40.0)
        *burn = launch::BURN_UPPER;
    return true;
}

static glm::dvec3 rocketEcef(const SatelliteSim::LaunchState &L, const LBatch &B, int stage, double tau, bool &ok)
{
    glm::dvec3 P;
    ok = rocketPos(L, B, stage, tau, P);
    return ok ? eciToEcef(P, B.tL + std::max(tau, 0.0)) : glm::dvec3(0.0);
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
    auto kappaAt = [&](double h) { return glm::mix(10.0, 300.0, glm::smoothstep(25000.0, 90000.0, h)); };
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
        B.parcels.push_back(launch::makeParcel(tau, p, vel + vex * ex, m, kappaAt(h), albedoAt(h),
                                               spd > 5.0 ? vel / spd : up, len, maxAge, windDir, windGain));
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
    // The booster: ascent (every 1.5 s low, 3 s above 20 km), then its burns.
    for (double tau = 1.0; tau < vh.t1; )
    {
        const double dtE = tau < 60.0 ? 1.5 : 3.0;
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
    // The upper stage to orbit.
    for (double tau = vh.t1 + 5.0; tau < B.t2; tau += 5.0)
        emit(1, tau, 5.0, vh.flow2, vh.vex2, 0);
    // Its deorbit burn, and maybe a spinning vent of what is left (the spirals seen from Norway, New Zealand...).
    if (vh.upperDeorbit)
    {
        const double tb = B.tDeorbBurn - B.tL;
        for (double tau = tb; tau < tb + 40.0; tau += 5.0)
            emit(1, tau, 5.0, vh.flow2 * 0.6, vh.vex2, 2);
        if (B.vents)
            for (double tau = tb + 50.0; tau < tb + 170.0; tau += 1.5)
                emit(1, tau, 1.5, 6.0, 300.0, 3);
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
    }
    // A new design-life scale reshuffles every refresh: start over.
    if (L.planesBuilt && L.lifeScale != launchLifeScale)
    {
        launchInitBlock();
        L.gpuGen = kesslerBufGen;
    }
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
            const double life = std::max(0.25, pg.lifeYears * (double)launchLifeScale) * kYear;
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
                        if (++builds > 400)
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
        if (followActive && followSatIndex == (int)(activeSatCount + slot))
            stopFollow();
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
            if (!need)
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
        g.typeConst = (uint32_t)so.typeIdx | ((so.constIdx & 0xFFu) << 16);
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
                bool due = ch.empty() || std::abs(t - B.tw[kind]) > 20.0;
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
                    upNow[m] = (kind == 0 || t < mb.tEnd) && LaunchImpl::memberAllowed(*this, B, mb);
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
                    if (si.shown && std::abs(t - si.tw) <= 20.0)
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

SatOrbitElems SatelliteSim::launchOrbitElems(uint32_t slot) const
{
    if (!launchState || slot >= launchState->elems.size())
        return SatOrbitElems{};
    return launchState->elems[slot];
}

std::string SatelliteSim::launchLabel(uint32_t slot) const
{
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
    if (si.owner % 2 == 0)
        snprintf(b, sizeof(b), "%s #%d (raising, %s)", cn, (int)sat - (int)constellations[o.constIdx].orbitStart,
                 L.cat.vehicles[B.veh].name.c_str());
    else
        snprintf(b, sizeof(b), "%s #%d (retired, deorbiting)", cn, (int)sat - (int)constellations[o.constIdx].orbitStart);
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
                fbDrawReentry(U0, A, m0, N, B.seed ^ ((uint64_t)mb.sat * 0x9E3779B97F4A7C15ull), tau, out, n, toSky, pixFull);
                ++L.nReentries;
            }
        if (t < B.tL - 600.0 || t > B.tVisEnd)
            continue;
        const double tau = t - B.tL;
        // The upper stage's own re-entry.
        if (vh.upperDeorbit && t >= B.tStageRE && t < B.tStageRE + 200.0)
        {
            glm::dvec3 P;
            if (LaunchImpl::rocketPos(L, B, 1, B.tStageRE - B.tL - 1.0, P))
            {
                const glm::dvec3 U0 = glm::normalize(P);
                if (glm::length(kR * U0 - eye) < 3.0e6)
                {
                    const double th = B.thIns + B.nPark * (B.tStageRE - B.tL - B.t2);
                    glm::dvec3 A = -std::sin(th) * B.U0 + std::cos(th) * B.A0;
                    const double m0 = -4.0 - 2.5 * std::log10(vh.upperMassKg / 100.0);
                    fbDrawReentry(U0, A, m0, (int)std::clamp(3.0 + 40.0 * std::sqrt(vh.upperMassKg / 2500.0), 3.0, 80.0),
                                  B.seed ^ 0x51A6E5ull, t - B.tStageRE, out, n, toSky, pixFull);
                    ++L.nReentries;
                }
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
            }
            double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - (burning ? 0.8 : 0.97) * dayness);
            if (flare < 2e-4)
                continue;
            if (kFireCount < kFireballMax)
                fo[kFireCount++] = {glm::vec4(glm::vec3(dirE), (float)flare), glm::vec4(col, (float)range)};
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
                    g.c = glm::vec4(glm::vec3(toEnu(P - 1.2 * Rg * vdir - eye)), (float)(0.25 * J * toSky));
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
            if (fast || glm::length(padNow - eye) > 4.5e6 + 2.0 * eyeAlt)
                continue;
            LaunchImpl::buildParcels(L, B);
        }
        const double era = earthRotationAngle(t);
        for (const launch::Parcel &pc : B.parcels)
        {
            const double age = tau - pc.t0;
            glm::dvec3 cE;
            double sig, cond, sweep = 0.0;
            if (age < 0.0 || !pc.at(age, cE, sig, cond, &sweep))
                continue;
            const double Sigma = pc.kappa * pc.m * cond * (double)launchPlumeGain;
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
            const double sinE = glm::dot(glm::normalize(C), sunD);
            // The sky's light on it, relative (1 = a clear daytime sky), fading with the air above it.
            const double amb = glm::smoothstep(-0.1, 0.25, sinE) * std::exp(-std::max(h, 0.0) / 15000.0);
            d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), (float)amb);
            // At night the sky's exposure is not physical (only ~5.5x the day's: see the Kessler dust), so a sunlit
            // plume against the dark sky reads far dimmer than it is; its sunlight is raised as the eye's sky darkens.
            d.col = glm::vec4(glm::vec3(tr * pc.albedo * nightBoost), 2.0f);
            d.cone = coneOf(toEnu(rel), 5.5 * std::max(sA, sP));
            plumes.push_back(c);
            // High up, once the expanding exhaust has swept up about its own mass of air it piles up at its rim: a shell,
            // limb-brightened (the jellyfish's bell). Drawn as the core plus a ring of Gaussians across the line of sight.
            const double shellK = (h > 60000.0 && sig > 2000.0) ? 0.65 * glm::smoothstep(0.3, 2.0, sweep) : 0.0;
            if (shellK > 0.02)
            {
                const double coreShare = 1.0 - shellK;
                plumes.back().d.c.w = (float)(Sigma * coreShare);
                plumes.back().pri *= coreShare;
                const glm::dvec3 vE = toEnu(rel) / range; // the line of sight
                glm::dvec3 r1 = glm::cross(vE, glm::dvec3(0, 0, 1));
                r1 = glm::length(r1) > 1e-6 ? glm::normalize(r1) : glm::dvec3(1, 0, 0);
                const glm::dvec3 r2 = glm::cross(vE, r1);
                const int K = 7;
                const double rr = 0.9 * sig, sr = std::max(0.42 * sig, sMin);
                for (int k = 0; k < K; ++k)
                {
                    const double ang = glm::two_pi<double>() * ((double)k + h01(B.seed ^ (uint64_t)(pc.t0 * 10.0), (uint64_t)k, 31)) / K;
                    const glm::dvec3 off = rr * (std::cos(ang) * r1 + std::sin(ang) * r2);
                    // Each piece is stretched along the rim (the tangent) and the line of sight.
                    const glm::dvec3 tg = -std::sin(ang) * r1 + std::cos(ang) * r2;
                    const double sT = std::max(0.9 * sig, sMin), sN = sr;
                    const glm::dvec3 nrm = glm::normalize(off);
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
        for (const Cand &c : plumes)
        {
            if (n >= kMaxDustClouds)
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
    j["life_scale"] = launchLifeScale;
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
        const double life = std::max(0.25, pg.lifeYears * (double)launchLifeScale) * kYear;
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
        // The Sun at the pad at lift-off (twilight launches make the jellyfish).
        const glm::dvec3 sun = sunDirEciAt(B.tL);
        o["sun_elev_deg"] = glm::degrees(std::asin(glm::clamp(glm::dot(sun, ecefToEci(L.cat.sites[B.site].ecef, B.tL)), -1.0, 1.0)));
        arr.push_back(o);
    }
    return arr;
}

// Puts the observer near the next launch: twilight (the Sun 5-16 deg below the pad's horizon) within a week if asked,
// else the next. Time 30 s before lift-off at 1x, the camera on the climb.
bool SatelliteSim::launchGoToNext(std::string &msg, bool twilight) { return launchGoToNextWhen(msg, twilight ? 1 : 0); }

// when: 0 the next, 1 twilight (the Sun 5-16 deg down at the pad), 2 day (above 15 deg), 3 night (below -20 deg).
bool SatelliteSim::launchGoToNextWhen(std::string &msg, int when)
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
    const nlohmann::json list = launchListJson(twilight ? 24.0 * 7.0 : 48.0);
    const nlohmann::json *pick = nullptr;
    for (const nlohmann::json &o : list)
    {
        if (o["in_hours"].get<double>() < 0.02)
            continue;
        const double se = o["sun_elev_deg"].get<double>();
        if (when == 0 || (when == 1 && se < -5.0 && se > -16.0) || (when == 2 && se > 15.0) || (when == 3 && se < -20.0))
        {
            pick = &o;
            break;
        }
    }
    if (!pick)
    {
        msg = twilight ? "no such launch in the next week" : "no launch in the next 48 h";
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
        glm::dvec3 Pst;
        const bool stageOk = LaunchImpl::rocketPos(L, B, 1, B.tD - B.tL, Pst);
        for (const LMember &mb : B.mem)
        {
            glm::dvec3 r, v;
            const glm::dvec3 slotA = satOrbitStateAt(rosterElemsOf(satOrbits[mb.sat]), B.tA).posEci;
            if (LaunchImpl::memberState(*this, B, mb, 0, B.tA - 1e-3, r, v))
            {
                const double d = glm::length(r - slotA);
                maxArr = std::max(maxArr, d);
                sumArr += d;
            }
            if (stageOk && LaunchImpl::memberState(*this, B, mb, 0, B.tD, r, v))
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
