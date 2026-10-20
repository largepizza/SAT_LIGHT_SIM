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
    glm::dvec3 depVel{0.0}; // at its release, the dispenser's velocity less the member's (same axes, rotating frame)
    // Its place in the payload stack (2026-10-20, launches.json "stack"; stackSlot): in the upper stage's body frame,
    // the satellite's origin (slotO), its body axes (slotR: columns = its X, Y, Z in the stage's), the direction it
    // leaves in (exitDir). stacked: the program has a stack — the satellite is drawn on the stage until its release
    // (a fairing vehicle, once the fairing is gone), leaves it in the stack's attitude (q0, body -> ECI at tRel) with
    // a little tip-off rate (omega, ECI), and slews to its attitude law from slewT0 over slewDur.
    bool stacked = false;
    glm::dvec3 slotO{0.0}, exitDir{0.0, 0.0, 1.0};
    glm::dmat3 slotR{1.0};
    glm::dquat q0{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 omega{0.0};
    double slewT0 = 0.0, slewDur = 0.0;
};

// A fairing half falling away (2026-10-20): its centre of mass sampled every kFairDt s from the separation (ECI,
// position and velocity), its attitude at the separation and its tumble rate. The model's origin is the stage's
// (the half was modelled in place); cb is its centre in that frame.
constexpr double kFairDt = 2.0;
struct LFairing
{
    std::vector<glm::dvec3> c, cv;
    glm::dquat q0{1.0, 0.0, 0.0, 0.0};
    glm::dvec3 w{0.0};
    double tEnd = 0.0; // gone (below ~60 km, or the table's end)
};

struct LBatch
{
    bool resid = false; // a member has a phasing residual (rewritten every 2 s while raising)
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
    // The pad's height (2026-10-19): its plateau (the DEM there, the terrain flattened to it) + the vehicle's mount.
    // Added to the track near the pad (trackEcef; gone by ~40 km downrange).
    double padBase = 0.0;
    // A returning ship's landing (2026-10-19): after its entry (B.tStageGone .. + kShipEntryS) it descends belly
    // first to ~1.2 km over a Starship pad (tShipDesc .. tShipFlip), flips, burns and is caught by the tower
    // (tShipCatch), held there until tShipEnd. landPad < 0: no pad in reach, a controlled splashdown at the same
    // point of its descent (the sea under it: catchH 0). Descent in the landing frame (the pad's or the splash
    // point's gnomonic plane: x east, y north, metres; h above sea level): from (dX0, dH0) with velocity (dV0, dVz0)
    // to F = (dXF, dHF) with (dVF, dVzF) over T1 (cubic Hermite per axis), then to the catch point (0, catchH) at rest
    // over T2.
    int landPad = -1;
    bool shipLands = false;
    double tShipDesc = 0.0, tShipFlip = 0.0, tShipCatch = 0.0, tShipEnd = 0.0;
    glm::dvec3 LG{1, 0, 0}, LE{0, 1, 0}, LN{0, 0, 1}; // landing frame (Earth-fixed): up, east, north at the target
    double catchH = 0.0;
    glm::dvec2 dX0{0.0}, dV0{0.0}, dXF{0.0}, dVF{0.0};
    double dH0 = 0.0, dVz0 = 0.0, dHF = 0.0, dVzF = 0.0, T1 = 1.0, T2 = 1.0;
    // The entry model's start vs the deorbit ellipse at the entry interface (ECI, m): faded out over the entry's
    // first 60 s (shipEntry), so the ship's track is continuous there (it jumped ~0.5 km).
    glm::dvec3 entryOff{0.0};
    // The payload fairing (2026-10-20): its halves separate at tFair (-1: no fairing), then fall (built lazily by
    // ensureFairing; fairBuilt). deployInterval: the members' spacing in time (the stack's, else the vehicle's).
    double tFair = -1.0;
    bool fairBuilt = false;
    LFairing fair[2];
    double deployInterval = 2.0;
};

// A launch pad on the ground (2026-10-19): one per distinct place among the catalog's sites (the Jiuquan crewed
// and uncrewed entries share a pad). Drawn by sat_sky.frag (the flat parts: launchFillPadParams), as a static mesh
// (the tower, the mount, the tanks: launchMeshJobs) and as lights at night (launchFillPadLights).
struct LPad
{
    int site = -1;                 // the first catalog site here
    int kind = 0;                  // 0 generic, 1 Falcon (fixed service structure, a landing zone), 2 Starship (catch tower)
    glm::dvec3 g{1, 0, 0};         // unit, Earth-fixed
    glm::dvec3 u{0, 1, 0}, v{0, 0, 1}; // Earth-fixed horizontal axes: u the layout's heading, v = up x u (to its left)
    double elevM = 0.0;            // plateau height (m above sea level)
    uint32_t seed = 0;
    double exclR = 3000.0;         // no city grid, glitter or sprites inside this (m)
    double flatR = 400.0;          // graded flat to elevM inside this (blending out to twice it)
    bool lz = false;               // a landing zone (Falcon pads) ...
    double lzAng = 0.0;            // ... kLzDistM out at this angle from u (toward v), on land
    std::string name;
};
// Where things stand on a pad, in its (u, v) frame (metres): shared by the ground pattern (sat_sky.frag padGround,
// which must agree), the meshes and the lights.
constexpr double kPadTowerV = -32.0;   // the tower's centre (u = 0); its arms reach +v over the mount at the centre
constexpr double kShipCatchAboveGround = 55.0; // a caught ship's aft end (the chopsticks hold it under its flaps)
constexpr double kShipEntryS = 480.0;  // the entry glow (shipEntry) lasts this long
constexpr double kCaughtHoldS = 1800.0;
constexpr double kLzDistM = 2300.0;    // a landing zone's distance from its pad (sat_sky.frag must agree) // a caught booster or ship is drawn in the tower this long

// A pre-decoded R8 map's value at a place (0..255, bilinear on texel centres as the GPU filters it): the halves
// (tools/make_raw_textures.py: "SLR8", width, total height, first row) are read four texels at a time.
double rawR8At(const char *const *paths, int nParts, double latDeg, double lonDeg)
{
    struct Part
    {
        FILE *f = nullptr;
        uint32_t w = 0, h = 0, first = 0;
    };
    Part parts[2];
    nParts = std::clamp(nParts, 1, 2);
    bool ok = true;
    for (int i = 0; i < nParts && ok; ++i)
    {
        parts[i].f = std::fopen(paths[i], "rb");
        char magic[4];
        uint32_t hdr[3];
        ok = parts[i].f && std::fread(magic, 1, 4, parts[i].f) == 4 && std::memcmp(magic, "SLR8", 4) == 0 &&
             std::fread(hdr, 4, 3, parts[i].f) == 3;
        if (ok)
        {
            parts[i].w = hdr[0];
            parts[i].h = hdr[1];
            parts[i].first = hdr[2];
        }
    }
    double val = -1.0;
    if (ok)
    {
        const uint32_t W = parts[0].w, H = parts[0].h;
        auto texel = [&](int x, int y) -> double {
            x = ((x % (int)W) + (int)W) % (int)W;
            y = std::clamp(y, 0, (int)H - 1);
            const Part &p = (nParts > 1 && (uint32_t)y >= parts[1].first) ? parts[1] : parts[0];
            std::fseek(p.f, (long)(16 + ((size_t)(y - (int)p.first) * W + (size_t)x)), SEEK_SET);
            unsigned char b = 0;
            return std::fread(&b, 1, 1, p.f) == 1 ? (double)b : 0.0;
        };
        const double u = (glm::radians(lonDeg) + glm::pi<double>()) / glm::two_pi<double>();
        const double vv = (0.5 * glm::pi<double>() - glm::radians(latDeg)) / glm::pi<double>();
        const double tx = u * W - 0.5, ty = vv * H - 0.5;
        const int x0 = (int)std::floor(tx), y0 = (int)std::floor(ty);
        const double fx = tx - x0, fy = ty - y0;
        val = (texel(x0, y0) * (1 - fx) + texel(x0 + 1, y0) * fx) * (1 - fy) +
              (texel(x0, y0 + 1) * (1 - fx) + texel(x0 + 1, y0 + 1) * fx) * fy;
    }
    for (Part &p : parts)
        if (p.f)
            std::fclose(p.f);
    return val;
}
// The DEM's height at a place (m above sea level), decoded as the GPU does (terrain.glsl: v / 255 x 9000 - 15 / 255
// x 9000, floored at 0; bilinear of the decoded texels ~ of the raw ones above the baseline).
double demElevCpu(double latDeg, double lonDeg)
{
    static const char *kParts[2] = {"assets/textures/earth_elevation_0.r8", "assets/textures/earth_elevation_1.r8"};
    const double v = rawR8At(kParts, 2, latDeg, lonDeg);
    return v < 0.0 ? 0.0 : std::max(0.0, v / 255.0 * 9000.0 - 15.0 / 255.0 * 9000.0);
}
// The water map's signed distance to the shore there (m, > 0 in water; terrain.glsl kShoreSdfMaxM), or 0 unknown.
double shoreDistCpu(double latDeg, double lonDeg)
{
    static const char *kSdf[1] = {"assets/textures/earth_water_sdf.r8"};
    const double v = rawR8At(kSdf, 1, latDeg, lonDeg);
    const double kShoreSdfMaxM = 4.0 * glm::pi<double>() * 6371000.0 / 4096.0;
    return v < 0.0 ? 0.0 : (v / 255.0 - 0.5) * 2.0 * kShoreSdfMaxM;
}

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
    std::vector<LBatch *> livePtr; // ... and the batches (valid until the next launchUpdate: only it erases batches)
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
    std::unordered_map<int, glm::dvec3> engineGeom; // per rocket type: (nozzle exit z, exit radius, length) in metres
    // Per plane: the crewed station's dead second (Kessler's roster word) the plane's batches were built against; a
    // change (a breakup, the cascade reaching it, a rollback) rebuilds them (crewSync).
    std::vector<int32_t> crewDead;
    std::vector<LPad> pads;          // the launch pads (ensureCatalog)
    // Roster satellite -> its plane and place in it (missionRosterBatches; rebuilt with the planes).
    std::vector<int32_t> satPlane;
    std::vector<uint32_t> satPos;
    std::vector<int> sitePad;        // per catalog site: its pad
    uint64_t padMeshKey[16] = {};    // the pads drawn as meshes, per reserved slot (pad index + 1; 0 = free)
    uint32_t nRockets = 0, nTrain = 0, nRetire = 0, nPlumes = 0, nReentries = 0, nWrites = 0, nBatches = 0;
    // Upper stages carrying a visible payload stack this frame (launchFillVisuals -> launchMeshJobs): the stage's
    // state at the list's time and its body frame.
    struct StackDraw
    {
        uint64_t key = 0;
        double t = 0.0;
        glm::dvec3 P{0.0}, V{0.0}, A{0.0};
        glm::dmat3 S{1.0};
        int type = -1; // the stage's model (its mesh size: which stack is nearest)
    };
    std::vector<StackDraw> stackDraws;
    uint64_t stackMeshKey[SatelliteSim::kStackMeshSlots] = {}; // per reserved slot: batch key ^ (member + 1) << 48
    uint32_t nStacked = 0, nFairings = 0;
    double lastFullFreeT = -1e300; // the last full pass over the batches (launchUpdate's freeing)

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
    // The last chunk holds the rocket meshes and the transient selection's slots, the two before it the pads' meshes,
    // the six before those the payload stacks' (2026-10-20).
    for (uint32_t c = kNumChunks - 3 - kStackMeshSlots / kChunk; c-- > 0;)
        L.freeChunks.push_back(c);
    static_assert(kPadMeshSlots == 2 * kChunk, "the pads' mesh slots: two chunks");
    static_assert(kStackMeshSlots % kChunk == 0, "the stacks' mesh slots: whole chunks");
    std::fill(std::begin(L.stackMeshKey), std::end(L.stackMeshKey), 0ull);
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
    L.livePtr.clear();
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
    buildPads(L);
}

// The pads: one per distinct place (sites within 150 m share one), its kind from the vehicles that fly from it (a
// tower-caught vehicle with a mount: a Starship pad; Falcon: a fixed service structure and a landing zone), its
// layout turned to the middle of its launch azimuths, its ground height from the DEM.
static void buildPads(SatelliteSim::LaunchState &L)
{
    L.pads.clear();
    L.sitePad.assign(L.cat.sites.size(), -1);
    for (size_t i = 0; i < L.cat.sites.size(); ++i)
    {
        launch::Site &st = L.cat.sites[i];
        int at = -1;
        for (size_t p = 0; p < L.pads.size(); ++p)
            if (glm::dot(L.pads[p].g, st.ecef) > std::cos(150.0 / kR))
                at = (int)p;
        if (at < 0)
        {
            LPad pd;
            pd.site = (int)i;
            pd.name = st.name;
            pd.g = st.ecef;
            int kind = 0;
            for (const launch::Vehicle &vh : L.cat.vehicles)
                if (std::find(vh.sites.begin(), vh.sites.end(), (int)i) != vh.sites.end())
                {
                    if (vh.recovery == 1 && vh.mountM > 0.0)
                        kind = 2;
                    else if (vh.id.rfind("falcon9", 0) == 0)
                        kind = std::max(kind, 1);
                }
            pd.kind = st.padKind >= 0 ? std::min(st.padKind, 2) : kind;
            double head = st.padHeadingDeg;
            if (std::isnan(head))
            {
                double a0 = st.azMinDeg, a1 = st.azMaxDeg;
                if (a1 < a0)
                    a1 += 360.0;
                head = 0.5 * (a0 + a1);
            }
            const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), pd.g));
            const glm::dvec3 north = glm::cross(pd.g, east);
            // The water map around it (most pads are on a coast): the layout's back (-u: the access road, the
            // trench, the tanks) turns to the most land 1.5-3 km out when the heading's own back is water there.
            auto shoreAt = [&](double az, double dist) {
                const glm::dvec3 dir = std::cos(az) * north + std::sin(az) * east;
                const glm::dvec3 q = glm::normalize(pd.g + dir * (dist / kR));
                return shoreDistCpu(glm::degrees(std::asin(glm::clamp(q.z, -1.0, 1.0))), glm::degrees(std::atan2(q.y, q.x)));
            };
            double hr = glm::radians(head);
            if (std::isnan(st.padHeadingDeg) && shoreAt(hr + glm::pi<double>(), 1500.0) > -200.0)
            {
                double best = 1e30;
                for (int k = 0; k < 24; ++k)
                {
                    const double az = glm::two_pi<double>() * k / 24.0;
                    const double d = shoreAt(az, 1500.0) + shoreAt(az, 3000.0);
                    if (d < best)
                    {
                        best = d;
                        hr = az + glm::pi<double>();
                    }
                }
                head = glm::degrees(hr);
            }
            pd.u = std::cos(hr) * north + std::sin(hr) * east;
            pd.v = glm::cross(pd.g, pd.u);
            // The ground: never below 3 m (a pad at the sea's level would be drawn as the sea).
            pd.elevM = std::max(demElevCpu(st.latDeg, st.lonDeg), 3.0);
            uint64_t h = 1469598103934665603ull;
            for (char c : st.id)
                h = (h ^ (uint64_t)(unsigned char)c) * 1099511628211ull;
            pd.seed = (uint32_t)(h & 0xFFFFu);
            pd.exclR = pd.kind == 1 ? 3200.0 : 3000.0;
            pd.flatR = pd.kind == 2 ? 450.0 : 350.0;
            // A landing zone (Falcon pads): on the most land kLzDistM out, if any (none in the water).
            pd.lz = false;
            if (pd.kind == 1)
            {
                double best = -300.0;
                for (int k = 0; k < 24; ++k)
                {
                    const double a = glm::two_pi<double>() * (k + 0.5) / 24.0;
                    const glm::dvec3 dir = std::cos(a) * pd.u + std::sin(a) * pd.v;
                    const glm::dvec3 q = glm::normalize(pd.g + dir * (kLzDistM / kR));
                    const double la = glm::degrees(std::asin(glm::clamp(q.z, -1.0, 1.0))), lo = glm::degrees(std::atan2(q.y, q.x));
                    const double d = shoreDistCpu(la, lo);
                    // Dry land: not water, and above the sea's level (land at 0 m is drawn as water).
                    if (d < best && demElevCpu(la, lo) > 1.0)
                    {
                        best = d;
                        pd.lz = true;
                        pd.lzAng = a;
                    }
                }
            }
            at = (int)L.pads.size();
            L.pads.push_back(pd);
            Log::line("launches: pad " + st.id + " kind " + std::to_string(pd.kind) + ", heading " +
                      std::to_string((int)std::lround(head)) + " deg, ground " + std::to_string((int)std::lround(pd.elevM)) + " m");
        }
        L.sitePad[i] = at;
        st.elevM = L.pads[at].elevM;
    }
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
    L.satPlane.clear();
    L.satPos.clear();
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

// A returning ship's landing (2026-10-19; the user: SpaceX's 2036 plan is full reuse, the ship caught by the tower).
// Its deorbit burn is timed so that its entry ends 120-450 km short of a Starship pad along its ground track (and
// within 200 km of it across): the first such moment in the 72 h after deployment (30-s steps over the burn time,
// then 2-s steps around the best), a pure function of the batch. The entry itself is the same closed form as before;
// the descent (shipDescentEcef) then bends from wherever it ends onto the pad. With no pad in reach in 72 h, the old
// burn time and a touchdown on the sea 250 km past the entry's end.
template <class F> static void planShipLanding(SatelliteSim &sim, SatelliteSim::LaunchState &L, LBatch &B, F &&setDeorbit)
{
    (void)sim;
    const double tD0 = B.tDeorbBurn;
    const glm::dvec3 omega(0.0, 0.0, 7.2921159e-5);
    // The entry's end (te = kShipEntryS) at the current burn: its angle in the plane past the entry interface.
    const glm::dvec3 P480 = shipEntry(B, kShipEntryS, nullptr, nullptr);
    double dTh = std::atan2(glm::dot(P480, B.A0), glm::dot(P480, B.U0)) - B.thEI;
    dTh -= glm::two_pi<double>() * std::floor(dTh / glm::two_pi<double>() + 0.5);
    const double hEnd = glm::length(P480) - kR;
    double vEnd = 0.0;
    shipEntry(B, kShipEntryS, &vEnd, nullptr);
    // The end (Earth-fixed unit position, direction over the ground) for a burn at Td: only the phase moves.
    auto endAt = [&](double Td, glm::dvec3 &e, glm::dvec3 &dirT) {
        const double th = B.thEI + B.nPark * (Td - tD0) + dTh;
        const double tE = B.tStageGone + (Td - tD0) + kShipEntryS;
        const glm::dvec3 r = (kR + hEnd) * (std::cos(th) * B.U0 + std::sin(th) * B.A0);
        const glm::dvec3 vI = vEnd * (-std::sin(th) * B.U0 + std::cos(th) * B.A0);
        const glm::dvec3 rE = eciToEcef(r, tE), vE = eciToEcef(vI, tE) - glm::cross(omega, rE);
        e = glm::normalize(rE);
        dirT = vE - glm::dot(vE, e) * e;
        dirT = glm::length(dirT) > 1e-6 ? glm::normalize(dirT) : glm::dvec3(1, 0, 0);
    };
    std::vector<int> pads;
    for (size_t p = 0; p < L.pads.size(); ++p)
        if (L.pads[p].kind == 2)
            pads.push_back((int)p);
    auto score = [&](double Td, int &best) {
        glm::dvec3 e, dT;
        endAt(Td, e, dT);
        const glm::dvec3 c = glm::cross(e, dT);
        double sBest = 1e30;
        best = -1;
        for (int p : pads)
        {
            const glm::dvec3 g = L.pads[p].g;
            if (glm::dot(g, e) < 0.98)
                continue;
            const double a = kR * glm::dot(g - e, dT), x = kR * glm::dot(g - e, c);
            if (a < 120e3 || a > 450e3 || std::abs(x) > 200e3)
                continue;
            const double sc = std::abs(x) + 0.3 * std::abs(a - 260e3);
            if (sc < sBest)
            {
                sBest = sc;
                best = p;
            }
        }
        return sBest;
    };
    double TdBest = -1.0;
    int padBest = -1;
    for (double Td = tD0; Td < tD0 + 72.0 * 3600.0 && TdBest < 0.0; Td += 30.0)
    {
        int p;
        if (score(Td, p) < 1e29)
        {
            double sMin = 1e30;
            for (double T2 = Td; T2 < Td + 600.0; T2 += 2.0)
            {
                int q;
                const double sc = score(T2, q);
                if (sc < sMin)
                {
                    sMin = sc;
                    TdBest = T2;
                    padBest = q;
                }
            }
        }
    }
    glm::dvec3 tgt;
    if (TdBest >= 0.0)
    {
        B.tDeorbBurn = TdBest;
        setDeorbit();
        B.landPad = padBest;
        tgt = L.pads[padBest].g;
        B.catchH = L.pads[padBest].elevM + kShipCatchAboveGround;
    }
    else
    {
        glm::dvec3 e, dT;
        endAt(tD0, e, dT);
        const double a = 250e3 / kR;
        tgt = std::cos(a) * e + std::sin(a) * dT;
        B.landPad = -1;
        B.catchH = 0.0;
    }
    B.LG = glm::normalize(tgt);
    B.LE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), B.LG));
    B.LN = glm::cross(B.LG, B.LE);
    // Where the entry hands over: its state at te = kShipEntryS (Earth-fixed, from its last second).
    B.tShipDesc = B.tStageGone + kShipEntryS;
    const glm::dvec3 Q1 = eciToEcef(shipEntry(B, kShipEntryS, nullptr, nullptr), B.tShipDesc);
    const glm::dvec3 Q0 = eciToEcef(shipEntry(B, kShipEntryS - 1.0, nullptr, nullptr), B.tShipDesc - 1.0);
    glm::dvec2 x1, x0;
    double h1, h0;
    landCoords(B, Q1, x1, h1);
    landCoords(B, Q0, x0, h0);
    B.dX0 = x1;
    B.dH0 = h1;
    B.dV0 = x1 - x0;
    B.dVz0 = h1 - h0;
    // F: 1.1 km over the catch point and 400 m back along the approach, falling belly first at ~75 m/s.
    const glm::dvec2 appr = glm::length(x1) > 1.0 ? -glm::normalize(x1) : glm::dvec2(1.0, 0.0);
    B.dXF = -400.0 * appr;
    B.dHF = B.catchH + 1100.0;
    B.dVF = 20.0 * appr;
    B.dVzF = -75.0;
    // Durations: the horizontal Hermite's start tangent stays under 3x the chord (no loop back) at 2x; the landing
    // burn's likewise.
    B.T1 = glm::clamp(2.0 * glm::length(B.dXF - B.dX0) / std::max(glm::length(B.dV0), 200.0), 150.0, 1000.0);
    const double LF = std::sqrt(glm::dot(B.dXF, B.dXF) + (B.dHF - B.catchH) * (B.dHF - B.catchH));
    B.T2 = glm::clamp(2.0 * LF / std::sqrt(glm::dot(B.dVF, B.dVF) + B.dVzF * B.dVzF), 15.0, 60.0);
    B.tShipFlip = B.tShipDesc + B.T1;
    B.tShipCatch = B.tShipFlip + B.T2;
    B.tShipEnd = B.tShipCatch + (B.landPad >= 0 ? kCaughtHoldS : 30.0);
    B.shipLands = true;
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
    // A crewed station the cascade or the player killed (dead, destroyed or re-entered) before lift-off gets no more
    // crews (issue 20261019_231126). A flight already up keeps flying (launchOngoing reports the station lost). Read
    // from the roster word mirrored from the event log, so a pure function of time and the log; crewSync rebuilds the
    // plane's batches when the word changes.
    if (pg.crew && !pl.sats.empty() && pl.sats[0] < sim.activeSatCount && 2ull * pl.sats[0] + 1 < sim.kRosterWords.size())
    {
        // (lost = dead or ended, whichever first: a breakup ends a satellite without a dead second)
        const int32_t dead = std::min(sim.kRosterWords[2ull * pl.sats[0]], sim.kRosterWords[2ull * pl.sats[0] + 1]);
        if (sim.kesslerEnabled && dead != kessler::kNever && (double)dead <= secOfKessler(sim, tL))
        {
            B.why = "station lost";
            return;
        }
    }
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const launch::Site &st = L.cat.sites[B.site];
    B.tL = tL;
    B.azDeg = az;
    B.padBase = (B.site >= 0 && B.site < (int)L.sitePad.size() && L.sitePad[B.site] >= 0
                     ? L.pads[L.sitePad[B.site]].elevM
                     : 0.0) +
                vh.mountM;
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
    // (not before the last member has left the dispenser and drifted clear: a Starship train takes ~20-30 min)
    const launch::Stack &stk = pg.stack;
    B.deployInterval = stk.intervalS >= 0.0 ? stk.intervalS : vh.deployIntervalS;
    B.tDeorbBurn = std::max(B.tD + 1800.0 + 3600.0 * h01(B.seed, 7, 1),
                            B.tD + B.deployInterval * (double)B.mem.size() + 900.0);
    // The payload fairing (2026-10-20): its halves go once the upper stage passes vh.fairingSepKm (~110 km: the
    // heating has fallen off), found on the upper stage's track and refined to 0.1 s.
    B.tFair = -1.0;
    B.fairBuilt = false;
    if (vh.fairingSepKm > 0.0)
    {
        const double hSep = vh.fairingSepKm * 1000.0;
        auto alt = [&](double tau) {
            glm::dvec3 P;
            return rocketPos(L, B, 1, tau, P) ? glm::length(P) - kR : -1.0;
        };
        double lo = vh.t1 + 5.0, hi = -1.0;
        if (alt(lo) >= hSep)
            hi = lo;
        else
            for (double tau = lo; tau <= vh.t2; tau += 2.0)
                if (alt(tau) >= hSep)
                {
                    hi = tau;
                    lo = tau - 2.0;
                    break;
                }
        if (hi < 0.0)
            hi = lo = std::min(vh.t1 + 30.0, vh.t2);
        while (hi - lo > 0.1)
        {
            const double m = 0.5 * (lo + hi);
            (alt(m) >= hSep ? hi : lo) = m;
        }
        B.tFair = B.tL + hi;
    }
    auto setDeorbit = [&]()
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
        B.tStageGone = B.tStageRE;
        B.entryOff = glm::dvec3(0.0);
        if (!vh.upperDeorbit)
            B.entryOff = deorbitPos(B, B.tStageGone) - shipEntry(B, 0.0, nullptr, nullptr);
    };
    setDeorbit();
    if (!vh.upperDeorbit)
        planShipLanding(sim, L, B, setDeorbit);
    B.vents = h01(B.seed, 8, 1) < vh.ventChance;
    const double boosterEnd = tL + L.booster[B.veh].tEnd + (vh.recovery == 1 ? kCaughtHoldS : 0.0);
    B.tVisEnd = std::max({boosterEnd + kPlumeAgeLow, B.tStageGone + kPlumeAgeHigh, tL + B.t2 + kPlumeAgeHigh,
                          B.tStageGone + kShipEntryS, B.shipLands ? B.tShipEnd : 0.0});
    B.ends.clear();
    B.resid = false;
    for (const LMember &mb : B.mem)
    {
        B.ends.push_back(mb.tEnd);
        B.resid = B.resid || std::abs(mb.eps) > 1e-9;
    }
    std::sort(B.ends.begin(), B.ends.end());
    // Each member leaves the dispenser in turn (deployIntervalS apart): at its release it is AT the dispenser — in the
    // payload bay's door, near the nose on the zenith side (the stage flies nose prograde, belly to nadir) — and drifts
    // away from it at a fraction of a m/s, mostly upward; memberState eases that offset out over two days. (Until
    // 2026-10-19 the whole train appeared at once at deployment, a string 120 m apart behind the stage.)
    {
        const bool big = vh.upperMassKg > 50000.0; // Starship's ship: a 9-m body, the bay door high on the side
        // (5.3 m up: a stowed satellite's underside clears the 4.5-m hull)
        const double noseOff = big ? 31.0 : 9.0, sideOff = big ? 5.3 : 1.6;
        for (size_t mi = 0; mi < B.mem.size(); ++mi)
        {
            LMember &mb = B.mem[mi];
            mb.tRel = B.tD + B.deployInterval * (double)mi;
            // Its slot in the stack (2026-10-20): it leaves from there, in the stack's attitude.
            mb.stacked = stackSlot(vh, stk, mi, B.mem.size(), mb.slotO, mb.slotR, mb.exitDir);
            glm::dvec3 Pst, Pb;
            if (!rocketPos(L, B, 1, mb.tRel - B.tL, Pst) || !rocketPos(L, B, 1, mb.tRel - B.tL - 1.0, Pb))
                continue;
            mb.depOff = glm::dvec3(0.0);
            mb.depVel = glm::dvec3(0.0);
            // The dispenser less the member's own path, in the member's (radial, along, cross) axes, at tt.
            auto offsetAt = [&](double tt, glm::dvec3 &o) {
                glm::dvec3 P1, P0, r, v;
                if (!rocketPos(L, B, 1, tt - B.tL, P1) || !rocketPos(L, B, 1, tt - B.tL - 1.0, P0) ||
                    !memberState(sim, B, mb, 0, tt, r, v))
                    return false;
                const glm::dvec3 nose = glm::normalize(P1 - P0), up = glm::normalize(P1);
                const glm::dvec3 disp = mb.stacked ? P1 + upperFrame(L, B, tt - B.tL) * mb.slotO
                                                   : P1 + noseOff * nose + sideOff * up;
                const glm::dvec3 U = r / glm::length(r);
                const glm::dvec3 Cn = glm::normalize(glm::cross(r, v));
                const glm::dvec3 A = glm::cross(Cn, U);
                const glm::dvec3 d = disp - r;
                o = glm::dvec3(glm::dot(d, U), glm::dot(d, A), glm::dot(d, Cn));
                return true;
            };
            glm::dvec3 o0, oA, oB;
            // (one-sided, second order: the first member's own path starts at its release)
            if (!offsetAt(mb.tRel, o0) || !offsetAt(mb.tRel + 0.5, oA) || !offsetAt(mb.tRel + 1.0, oB))
                continue;
            mb.depOff = o0;
            // The member leaves with the dispenser's velocity (+ its push): relative to its own plane and phase that is
            // tens to a hundred m/s across (the stage's plane differs from the slot's by up to a degree of node).
            mb.depVel = -3.0 * o0 + 4.0 * oA - oB; // (/ 2h, h = 0.5 s)
            const uint64_t ms = B.seed ^ ((uint64_t)mi * 0xD1B54A32D192ED03ull);
            // Mostly up, a little back, + the dispenser's spread (Starship's: a few cm/s, so the members leave as
            // one string along the same free-flight arc, sepSpeed x interval apart; a stack's: ~0.1 m/s).
            const double sp = stk.sepMps >= 0.0 ? stk.sepMps : vh.sepSpeedMps;
            const double sd = stk.spreadMps >= 0.0 ? stk.spreadMps : vh.sepSpreadMps;
            mb.sepDir = sp * glm::dvec3(0.97, -0.22, 0.0) +
                        sd * glm::dvec3(2.0 * h01(ms, 30, 1) - 1.0, 2.0 * h01(ms, 31, 1) - 1.0, 2.0 * h01(ms, 32, 1) - 1.0);
            if (mb.stacked)
            {
                // Out of its slot (2026-10-20): along the stack's exit direction (Starship: out of the door, up; a
                // flat stack: forward along the stage; a column: radially out), in the member's own axes at tRel.
                glm::dvec3 r, v;
                const glm::dmat3 S = upperFrame(L, B, mb.tRel - B.tL);
                if (memberState(sim, B, mb, 0, mb.tRel, r, v))
                {
                    const glm::dvec3 U = glm::normalize(r), Cn = glm::normalize(glm::cross(r, v)), A = glm::cross(Cn, U);
                    const glm::dvec3 e = S * mb.exitDir;
                    mb.sepDir = sp * glm::dvec3(glm::dot(e, U), glm::dot(e, A), glm::dot(e, Cn)) +
                                sd * glm::dvec3(2.0 * h01(ms, 30, 1) - 1.0, 2.0 * h01(ms, 31, 1) - 1.0,
                                                2.0 * h01(ms, 32, 1) - 1.0);
                }
                // It leaves in the stack's attitude with a little tip-off rate (0.2-0.8 deg/s about a hashed axis),
                // and its own control turns it to its attitude law 1-2 min later (over 3 min), before it unfolds.
                mb.q0 = glm::normalize(glm::quat_cast(S * mb.slotR));
                const double a1 = glm::two_pi<double>() * h01(ms, 33, 1), c1 = 2.0 * h01(ms, 34, 1) - 1.0;
                const glm::dvec3 ax(std::sqrt(1.0 - c1 * c1) * std::cos(a1), std::sqrt(1.0 - c1 * c1) * std::sin(a1), c1);
                mb.omega = ax * glm::radians(0.2 + 0.6 * h01(ms, 35, 1)) * (stk.layout == launch::Stack::Pez ? 0.5 : 1.0);
                mb.slewT0 = mb.tRel + 60.0 + 60.0 * h01(ms, 36, 1);
                mb.slewDur = 180.0;
            }
        }
    }
    B.valid = true;
}

// Diagnostics only (harness `launch jitter`): build records and drift the old way, for a before / after comparison.
static inline bool gLegacyRecords = false;

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
    // It leaves the dispenser at tRel with the dispenser's velocity + its push sepDir (m/s) and moves away as a FREE
    // body does (Clohessy-Wiltshire about the parking circle: a radial push makes it rise and fall back behind, an
    // along-track one drift), and the offset eases out over two days (the satellite's own thrusters). (Until 2026-10-19 the drift was a straight line in the
    // rotating frame, which no orbit follows: the GPU's Kepler record of it was off by metres after 2 s and by up
    // to hundreds of metres after the 20-s rewrites — the jitter at 10x.)
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
            // The whole offset from its own path flies free from the release: the dispenser's position and velocity
            // relative to that path (+ its push), as Clohessy-Wiltshire motion about the parking circle. (Holding the
            // dispenser's offset fixed in the rotating frame — up to ~100 km across, the planes' difference — took a
            // constant push no orbit has: 0.07 m/s^2 across, 14 m off a Kepler record after 20 s.)
            glm::dvec3 o, cwV;
            launch::cwState(mb.depOff, mb.depVel + mb.sepDir, B.nPark, age, o, cwV);
            if (gLegacyRecords) // (harness `launch jitter`: the pre-2026-10-19 straight drift, for comparison)
            {
                o = mb.depOff + mb.sepDir * age;
                cwV = mb.sepDir;
            }
            r += f * (o.x * U + o.y * A + o.z * Cn);
            if (t >= mb.tRel)
                v += f * (cwV.x * U + cwV.y * A + cwV.z * Cn);
        }
    }
    return true;
}

// A member's unfolding (2026-10-19): a train member leaves its dispenser stowed and its model's stowable parts
// (arrays, radiators: SatComponent::stow) unfold over SatModel::unfoldDurS, starting unfoldDelayS (+-15% per member)
// after its release; a retiree is deployed. t0 / dur as SatOrbitElems::unfoldT0 / unfoldDur (dur 0 = deployed).
static void memberUnfold(const SatelliteSim &sim, const LMember &mb, int kind, double &t0, double &dur)
{
    t0 = 0.0;
    dur = 0.0;
    if (kind != 0)
        return;
    const SatOrbit &so = sim.satOrbits[mb.sat];
    if (so.typeIdx >= sim.satTypes.size() || !sim.satTypes[so.typeIdx].model)
        return;
    const SatModel &m = *sim.satTypes[so.typeIdx].model;
    if (m.unfoldDurS <= 0.0)
        return;
    const double j = h01((uint64_t)mb.sat * 0x9E3779B97F4A7C15ull, 41, 1);
    t0 = mb.tRel + m.unfoldDelayS * (0.85 + 0.3 * j);
    dur = m.unfoldDurS;
}

// The Kepler record a member's GPU slot carries from t (and the CPU's copy of it, ce): its OSCULATING orbit at t.
// The GPU propagates it as a Kepler orbit until the next rewrite, so it must be the orbit the member is really on
// (2026-10-19, the 10x jitter): the velocity is memberState's own derivative (its drift and every eased offset
// included; the analytic one was the bare circle's), an SSO plane's precession is the record's node rate (removed
// from the velocity first, so it is not counted twice), and the epoch phase uses the mean motion as the GPU's float
// holds it (with the double one the float's rounding, x up to 7 days, put a satellite up to ~140 m off, a different
// amount at every rewrite).
static bool memberRecord(const SatelliteSim &sim, const LBatch &B, const LMember &mb, int kind, double t,
                         double tEpoch, GpuKepOrbit &g, SatOrbitElems &ce)
{
    glm::dvec3 r, v, rA, rB, vT;
    if (!memberState(sim, B, mb, kind, t, r, v))
        return false;
    if (gLegacyRecords)
    {
        // The pre-2026-10-19 record (harness `launch jitter` only): memberState's analytic velocity, no node rate,
        // the double mean motion.
        const kessler::Elems el = kessler::elemsFromState({r, v});
        if (!(el.a > kR) || el.e >= 0.9)
            return false;
        const double n = launch::meanMotion(el.a);
        g = GpuKepOrbit{};
        g.M = (float)wrap2pi(el.M - n * (t - tEpoch));
        g.argp = (float)wrap2pi(el.argp);
        g.raan = (float)wrap2pi(el.raan);
        g.n = (float)n;
        g.a = (float)el.a;
        g.e = (float)el.e;
        g.cosI = (float)std::cos(el.i);
        g.sinI = (float)std::sin(el.i);
        ce = SatOrbitElems{};
        ce.kepler = true;
        ce.u0 = wrap2pi(el.M - n * t);
        ce.raan = el.raan;
        ce.argp = el.argp;
        ce.meanMot = n;
        ce.rSatM = el.a;
        ce.ecc = el.e;
        ce.incl = el.i;
        return true;
    }
    const double h = 0.25;
    const bool okA = memberState(sim, B, mb, kind, t + h, rA, vT), okB = memberState(sim, B, mb, kind, t - h, rB, vT);
    if (okA && okB)
        v = (rA - rB) / (2.0 * h);
    else if (okA)
        v = (rA - r) / h;
    else if (okB)
        v = (r - rB) / h;
    const SatOrbit &so = sim.satOrbits[mb.sat];
    const SatOrbitElems re = sim.rosterElemsOf(so);
    const double raanDot = re.sso ? satphot::kSSOPrecRate : 0.0;
    const glm::dvec3 vRel = v - glm::cross(glm::dvec3(0.0, 0.0, raanDot), r);
    const kessler::Elems el = kessler::elemsFromState({r, vRel});
    if (!(el.a > kR) || el.e >= 0.9)
        return false;
    const float nf = (float)launch::meanMotion(el.a);
    const double n = (double)nf;
    g = GpuKepOrbit{};
    g.M = (float)wrap2pi(el.M - n * (t - tEpoch));
    g.argp = (float)wrap2pi(el.argp);
    g.raan = (float)wrap2pi(el.raan - raanDot * (t - tEpoch));
    g.n = nf;
    g.a = (float)el.a;
    g.e = (float)el.e;
    g.cosI = (float)std::cos(el.i);
    g.sinI = (float)std::sin(el.i);
    g.raanDot = (float)raanDot;
    g.flux = 1.0f;
    g.bornSec = (int32_t)0x80000001;
    g.deathSec = kessler::kNever;
    // A train raising flies "open book" (bit 25: every joint at rest, arrays flat with the bus, edge into the
    // flow): the low-drag configuration that makes fresh trains bright. On station it is the roster satellite.
    // Bit 26: still unfolding — the tumble words hold the progress (rate, value at the orbit epoch).
    double uT0 = 0.0, uDur = 0.0;
    memberUnfold(sim, mb, kind, uT0, uDur);
    const bool unfolding = uDur > 0.0 && t < uT0 + uDur;
    g.typeConst = (uint32_t)so.typeIdx | ((so.constIdx & 0xFFu) << 16) | (kind == 0 ? (1u << 25) : 0u) |
                  (unfolding ? (1u << 26) : 0u);
    if (unfolding)
    {
        g.tumbleRate = (float)(1.0 / uDur);
        g.tumblePhase = (float)((tEpoch - uT0) / uDur);
    }
    ce = SatOrbitElems{};
    ce.kepler = true;
    ce.u0 = el.M - n * t;
    ce.raan = el.raan - raanDot * t;
    ce.raanDot = raanDot;
    ce.argp = el.argp;
    ce.meanMot = n;
    ce.rSatM = el.a;
    ce.ecc = el.e;
    ce.incl = el.i;
    ce.restJoints = kind == 0;
    if (unfolding)
    {
        ce.unfoldT0 = uT0;
        ce.unfoldDur = uDur;
    }
    if (kind == 0 && mb.stacked && t < mb.slewT0 + mb.slewDur)
    {
        // Just released from its stack (2026-10-20): the stack's attitude, tumbling slowly, until its control acquires
        // its law (CPU only: the GPU's point keeps the law).
        ce.attFix = true;
        ce.attQ = mb.q0;
        ce.attOmega = mb.omega;
        ce.attT0 = mb.tRel;
        ce.attSlewT0 = mb.slewT0;
        ce.attSlewDur = mb.slewDur;
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
    // Near the pad the track stands on its plateau and mount (a booster coming home lands on them); downrange the
    // offset is gone (a droneship's deck is at the sea's level).
    const double base = B.padBase * (1.0 - glm::smoothstep(5000.0, 40000.0, std::abs(x)));
    return (kR + base + h) * (std::cos(a) * B.G0 + std::sin(a) * B.Eaz);
}

// The landing frame's gnomonic plane (x east, y north at the target, metres) and back.
static glm::dvec3 landPoint(const LBatch &B, const glm::dvec2 &x, double h)
{
    return (kR + h) * glm::normalize(B.LG + (x.x / kR) * B.LE + (x.y / kR) * B.LN);
}
static void landCoords(const LBatch &B, const glm::dvec3 &P, glm::dvec2 &x, double &h)
{
    const double r = glm::length(P);
    const glm::dvec3 d = P / r;
    const double c = std::max(glm::dot(d, B.LG), 1e-3);
    x = kR * glm::dvec2(glm::dot(d, B.LE), glm::dot(d, B.LN)) / c;
    h = r - kR;
}
static double herm(double s, double p0, double m0, double p1, double m1)
{
    const double s2 = s * s, s3 = s2 * s;
    return (2 * s3 - 3 * s2 + 1) * p0 + (s3 - 2 * s2 + s) * m0 + (-2 * s3 + 3 * s2) * p1 + (s3 - s2) * m1;
}
// A returning ship after its entry (Earth-fixed): the belly-flop descent to F, the flip and landing burn to the catch
// point, then held in the tower (or, with no pad in reach, at rest on the sea).
static glm::dvec3 shipDescentEcef(const LBatch &B, double t, uint8_t *burn)
{
    glm::dvec2 x;
    double h;
    uint8_t b = launch::BURN_NONE;
    if (t < B.tShipFlip)
    {
        const double s = glm::clamp((t - B.tShipDesc) / B.T1, 0.0, 1.0);
        x = glm::dvec2(herm(s, B.dX0.x, B.dV0.x * B.T1, B.dXF.x, B.dVF.x * B.T1),
                       herm(s, B.dX0.y, B.dV0.y * B.T1, B.dXF.y, B.dVF.y * B.T1));
        h = herm(s, B.dH0, B.dVz0 * B.T1, B.dHF, B.dVzF * B.T1);
    }
    else if (t < B.tShipCatch)
    {
        const double s = glm::clamp((t - B.tShipFlip) / B.T2, 0.0, 1.0);
        x = glm::dvec2(herm(s, B.dXF.x, B.dVF.x * B.T2, 0.0, 0.0), herm(s, B.dXF.y, B.dVF.y * B.T2, 0.0, 0.0));
        h = herm(s, B.dHF, B.dVzF * B.T2, B.catchH, 0.0);
        b = launch::BURN_LANDING;
    }
    else
    {
        x = glm::dvec2(0.0);
        h = B.catchH;
    }
    if (burn)
        *burn = b;
    return landPoint(B, x, h);
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
        // A tower-caught booster stays in the chopsticks (half an hour) after its landing burn.
        if (tau > tr.tEnd + (vh.recovery == 1 && tr.landed ? kCaughtHoldS : 0.0))
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
    {
        // A returning ship: drawn by its entry glow (shipEntry) until tShipDesc, then here to the catch.
        if (!B.shipLands || t < B.tShipDesc || t >= B.tShipEnd)
            return false;
        P = ecefToEci(shipDescentEcef(B, t, burn), t);
        return true;
    }
    if (burn)
        *burn = launch::BURN_NONE;
    const Track &up = const_cast<SatelliteSim::LaunchState &>(L).upperTrackFor(B.veh, L.cat.programs[B.prog].parkKm);
    if (tau <= B.t2)
    {
        const glm::dvec2 p = up.at(tau);
        glm::dvec3 Pg = ecefToEci(trackEcef(B, p.x, p.y), t);
        const double a = p.x / kR;
        const glm::dvec3 Pi = (kR + p.y) * (std::cos(a) * B.U0 + std::sin(a) * B.A0);
        // The blend into the inertial plane starts AT staging (2026-10-19): it started 60 s before, so at separation
        // the upper stage was already ~4 km from the stack it came out of, a jump Go to and the selection carried.
        const double s = glm::clamp((tau - vh.t1) / std::max(B.t2 - vh.t1, 1.0), 0.0, 1.0);
        const double sm = s * s * (3.0 - 2.0 * s);
        // The upper track is integrated on its own and stands ~1 km from the booster's at separation: it starts from the
        // stack's point (the booster track's at t1, Earth-fixed) and the difference fades out with the blend.
        const Track &bt = L.booster[B.veh];
        if (vh.t1 <= bt.tEnd)
        {
            const glm::dvec2 pb = bt.at(vh.t1), pu = up.at(vh.t1);
            const glm::dvec3 d = trackEcef(B, pb.x, pb.y) - trackEcef(B, pu.x, pu.y);
            Pg += ecefToEci(d, t) * (1.0 - sm);
        }
        P = glm::mix(Pg, Pi, sm);
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
    if (B.shipLands && t >= B.tShipDesc)
    {
        // The ship coming home: from its entry attitude (nose 60 degrees above the inertial velocity, as the entry
        // glow's listing draws it) to the belly flop (level, nose along its path over the ground) over 90 s; the
        // flip to upright (2 s before the landing burn to 5 s into it); upright in the burn and in the tower.
        auto sm = [](double a, double b, double x) { const double s = glm::clamp((x - a) / (b - a), 0.0, 1.0); return s * s * (3.0 - 2.0 * s); };
        const glm::dvec3 up = glm::normalize(P);
        if (t >= B.tShipFlip + 5.0)
            return up;
        const glm::dvec3 vI = glm::normalize(P - P1);
        glm::dvec3 upp = up - glm::dot(up, vI) * vI;
        upp = glm::length(upp) > 1e-6 ? glm::normalize(upp) : up;
        const glm::dvec3 entryAx = glm::normalize(0.5 * vI + 0.866 * upp);
        // Over the ground (Earth-fixed velocity), level.
        const glm::dvec3 E1 = shipDescentEcef(B, t, nullptr), E0 = shipDescentEcef(B, t - 1.0, nullptr);
        glm::dvec3 hz = ecefToEci(E1 - E0, t);
        hz -= glm::dot(hz, up) * up;
        if (glm::length(hz) < 1e-3)
            hz = ecefToEci(-(B.dXF.x * B.LE + B.dXF.y * B.LN), t); // (F lies back along the approach)
        hz = glm::normalize(hz - glm::dot(hz, up) * up);
        glm::dvec3 ax = glm::normalize(glm::mix(entryAx, hz, sm(B.tShipDesc, B.tShipDesc + 90.0, t)));
        ax = glm::normalize(glm::mix(ax, up, sm(B.tShipFlip - 2.0, B.tShipFlip + 5.0, t)));
        return ax;
    }
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
// The upper stage's body frame at tau (body -> ECI, columns = its X, Y, Z): +Z the nose (stageNose), -Y as near the
// nadir as it can be — the rocket models' attitude law (primary +Z on the "velocity" the transient hands in,
// secondary -Y on the nadir), so a stack posed in it sits where the stage's mesh is drawn. (2026-10-20)
static glm::dmat3 upperFrame(const SatelliteSim::LaunchState &L, const LBatch &B, double tau)
{
    glm::dvec3 P;
    if (!rocketPos(L, B, 1, tau, P))
        return glm::dmat3(1.0);
    const glm::dvec3 up = glm::normalize(P);
    glm::dvec3 Z = stageNose(L, B, 1, tau);
    if (glm::length(Z) < 0.5)
        Z = up;
    Z = glm::normalize(Z);
    glm::dvec3 Y = up - glm::dot(up, Z) * Z;
    if (glm::length(Y) < 1e-6)
        Y = std::abs(Z.z) < 0.9 ? glm::dvec3(0, 0, 1) - Z.z * Z : glm::dvec3(1, 0, 0) - Z.x * Z;
    Y = glm::normalize(Y);
    return glm::dmat3(glm::cross(Y, Z), Y, Z);
}

// Member i of n's place in its program's stack (2026-10-20; tools/make_rocket_models.py lays the stacks out and checks
// them against every fairing): in the upper stage's body frame (+Z the nose, the payload adapter's top at
// vh.payloadBaseM), the satellite's origin o, its body axes R (columns: its X, Y, Z in the stage's frame) and the
// direction it leaves in. Released top tier first. Pez (Starship): every satellite from the door's slot, flat across
// the ship (its nadir face aft, its X out of the door), leaving through the door (+Y, the leeward side: zenith while
// the ship flies belly down). Flat: nadir face down, one or two a tier, leaving forward along the stage. Column:
// around the dispenser column, nadir face out, the long axis (X) along the stage, leaving radially.
static bool stackSlot(const launch::Vehicle &vh, const launch::Stack &st, size_t i, size_t n, glm::dvec3 &o,
                      glm::dmat3 &R, glm::dvec3 &exitDir)
{
    if (st.layout == launch::Stack::None)
        return false;
    glm::dvec3 slot;
    if (st.layout == launch::Stack::Column)
    {
        const size_t per = (size_t)std::max(st.perTier, 1);
        const size_t tiers = (n + per - 1) / per;
        const size_t k = tiers - 1 - i / per, j = i % per;
        const double phi = glm::two_pi<double>() * (double)j / (double)per +
                           ((k % 2) ? glm::pi<double>() / (double)per : 0.0);
        const glm::dvec3 er(std::cos(phi), std::sin(phi), 0.0), zs(0.0, 0.0, 1.0);
        R = glm::dmat3(zs, glm::cross(er, zs), er);
        slot = er * st.offsetM + zs * (vh.payloadBaseM + st.baseM + ((double)k + 0.5) * st.pitchM);
        exitDir = er;
    }
    else
    {
        R = glm::dmat3(glm::dvec3(0, 1, 0), glm::dvec3(1, 0, 0), glm::dvec3(0, 0, -1));
        if (st.layout == launch::Stack::Pez)
        {
            slot = glm::dvec3(0.0, 0.0, vh.doorM);
            exitDir = glm::dvec3(0.0, 1.0, 0.0);
        }
        else
        {
            const size_t per = (size_t)std::clamp(st.perTier, 1, 2);
            const size_t tiers = (n + per - 1) / per;
            const size_t k = tiers - 1 - i / per, j = i % per;
            const double s = per == 2 ? (j == 0 ? -1.0 : 1.0) : 0.0;
            // The satellite's X lies along the stage's Y, its Y along the stage's X.
            slot = (st.sideX ? glm::dvec3(0.0, s * st.offsetM, 0.0) : glm::dvec3(s * st.offsetM, 0.0, 0.0)) +
                   glm::dvec3(0.0, 0.0, vh.payloadBaseM + st.baseM + ((double)k + 0.5) * st.pitchM);
            exitDir = glm::dvec3(0.0, 0.0, 1.0);
        }
    }
    o = slot - R * st.center;
    return true;
}

// The upper stage's model at t (2026-10-20): with its fairing until the halves separate, bare after (the payload on its
// adapter), and a crewed stage empty once its spacecraft has left. Falls back to "_upper" when a variant is missing.
static int upperStageType(const SatelliteSim &sim, const SatelliteSim::LaunchState &L, const LBatch &B, double t)
{
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    auto find = [&](const char *part) {
        auto it = sim.rocketTypes.find("rocket_" + vh.id + part);
        return it != sim.rocketTypes.end() ? it->second : -1;
    };
    if (B.tFair > 0.0 && t >= B.tFair)
    {
        if (L.cat.programs[B.prog].crew && t >= B.tD)
        {
            const int e = find("_upper_empty");
            if (e >= 0)
                return e;
        }
        const int b = find("_upper_bare");
        if (b >= 0)
            return b;
    }
    return find("_upper");
}

// The fairing halves' fall (2026-10-20), built the first time they are drawn: each half leaves the stage at tFair with
// the stage's velocity + 1.8 m/s outward (the separation springs), its top tipping outward at ~11 deg/s, and falls
// under gravity and drag (ballistic coefficient 60 kg/m^2, air ~1.2 exp(-h / 8 km) moving with the Earth); its centre
// is sampled every kFairDt s until it is below 60 km on the way down (it would burn or parachute). cb = the +X half's
// centre on the stage (stage frame: launches.json fairing_center_m, the model's origin).
static void ensureFairing(const SatelliteSim::LaunchState &L, LBatch &B, const glm::dvec3 &cb)
{
    if (B.fairBuilt || B.tFair < 0.0)
        return;
    B.fairBuilt = true;
    const double tau = B.tFair - B.tL;
    glm::dvec3 P, Pa, Pb;
    if (!rocketPos(L, B, 1, tau, P) || !rocketPos(L, B, 1, tau + 0.5, Pa) || !rocketPos(L, B, 1, tau - 0.5, Pb))
        return;
    const glm::dvec3 V = Pa - Pb;
    const glm::dmat3 S0 = upperFrame(L, B, tau);
    const glm::dvec3 wE(0.0, 0.0, 7.2921159e-5);
    for (int h = 0; h < 2; ++h)
    {
        LFairing &F = B.fair[h];
        F.c.clear();
        F.cv.clear();
        const double ca = h ? -1.0 : 1.0; // the second half: the first turned 180 deg about the stage's axis
        const glm::dmat3 Rh = S0 * glm::dmat3(glm::dvec3(ca, 0, 0), glm::dvec3(0, ca, 0), glm::dvec3(0, 0, 1));
        F.q0 = glm::normalize(glm::quat_cast(Rh));
        const uint64_t hs = B.seed ^ (0xFA1Bull + (uint64_t)h * 0x9E3779B97F4A7C15ull);
        F.w = Rh * glm::dvec3(0.02 * (h01(hs, 1, 1) - 0.5), glm::radians(9.0 + 4.0 * h01(hs, 2, 1)),
                              0.05 * (h01(hs, 3, 1) - 0.5));
        glm::dvec3 r = P + Rh * cb;
        glm::dvec3 v = V + Rh * glm::dvec3(1.8, 0.0, 0.0);
        auto acc = [&](const glm::dvec3 &x, const glm::dvec3 &u) {
            const double rr = glm::length(x);
            const glm::dvec3 g = -launch::kGM * x / (rr * rr * rr);
            const double rho = 1.225 * std::exp(-(rr - kR) / 8000.0);
            const glm::dvec3 vr = u - glm::cross(wE, x);
            return g - (0.5 * rho * glm::length(vr) / 60.0) * vr;
        };
        bool rising = true;
        for (int k = 0; k <= 1200; ++k)
        {
            if (k % (int)kFairDt == 0)
            {
                F.c.push_back(r);
                F.cv.push_back(v);
            }
            const double hgt = glm::length(r) - kR;
            if (glm::dot(r, v) < 0.0)
                rising = false;
            if (!rising && hgt < 60000.0)
                break;
            // RK4, 1 s
            const glm::dvec3 k1v = acc(r, v), k1r = v;
            const glm::dvec3 k2v = acc(r + 0.5 * k1r, v + 0.5 * k1v), k2r = v + 0.5 * k1v;
            const glm::dvec3 k3v = acc(r + 0.5 * k2r, v + 0.5 * k2v), k3r = v + 0.5 * k2v;
            const glm::dvec3 k4v = acc(r + k3r, v + k3v), k4r = v + k3v;
            r += (k1r + 2.0 * k2r + 2.0 * k3r + k4r) / 6.0;
            v += (k1v + 2.0 * k2v + 2.0 * k3v + k4v) / 6.0;
        }
        F.tEnd = B.tFair + kFairDt * (double)(F.c.size() - 1);
    }
}

// A fairing half at t: its centre = its model's origin (ECI), its velocity, attitude (body -> ECI). False outside
// [tFair, tEnd].
static bool fairingAt(const LBatch &B, int h, double t, glm::dvec3 &pos, glm::dvec3 &vel, glm::dmat3 &R)
{
    const LFairing &F = B.fair[h];
    if (F.c.size() < 2 || t < B.tFair || t > F.tEnd)
        return false;
    const double x = (t - B.tFair) / kFairDt;
    const size_t i = std::min((size_t)x, F.c.size() - 2);
    const double s = x - (double)i, d = kFairDt;
    // Cubic Hermite on the samples and their velocities.
    const double s2 = s * s, s3 = s2 * s;
    const glm::dvec3 c = (2 * s3 - 3 * s2 + 1) * F.c[i] + (s3 - 2 * s2 + s) * d * F.cv[i] + (-2 * s3 + 3 * s2) * F.c[i + 1] +
                         (s3 - s2) * d * F.cv[i + 1];
    vel = glm::mix(F.cv[i], F.cv[i + 1], s);
    const double wl = glm::length(F.w);
    glm::dquat q = F.q0;
    if (wl > 1e-12)
        q = glm::angleAxis(wl * (t - B.tFair), F.w / wl) * q;
    R = glm::mat3_cast(glm::normalize(q));
    pos = c;
    return true;
}

// A transient's full attitude (a fairing half, 2026-10-20) into the elements its mesh slot is posed from.
static void transientAtt(SatOrbitElems &e, const SatelliteSim::SelTransient &s)
{
    if (!s.hasAtt)
        return;
    e.attFix = true;
    e.attQ = s.attQ;
    e.attOmega = s.attOmega;
    e.attT0 = s.attT0;
    e.attSlewDur = 0.0;
}

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
    // From the entry interface on the deorbit ellipse (angle per metre at the ellipse's radius there), plus the
    // ellipse's own offset there, eased out over the first minute (continuity at the hand-over).
    const double th = B.thEI + sAlong / (kR + 100000.0);
    const double fo = 1.0 - glm::smoothstep(0.0, 60.0, te);
    return (kR + hAt(u)) * (std::cos(th) * B.U0 + std::sin(th) * B.A0) + fo * B.entryOff;
}

// When the upper stage vents what is left, spinning (after its deorbit burn, or 10 min after deploying for a stage
// that returns).
static double ventStart(const SatelliteSim::LaunchState &L, const LBatch &B)
{
    return L.cat.vehicles[B.veh].upperDeorbit ? B.tDeorbBurn + 50.0 : B.tD + 600.0;
}

// A rocket model's engines (tools/make_rocket_models.py): the exit plane's height on the body axis (the origin is the
// aft end), the radius the engines' exits span, and the vehicle's length. From the components named engines /
// upper_nozzle / side_nozzle (cones, the exit at their bottom); a model without them: the aft end, 0.7 x the body.
static glm::dvec3 engineGeomOf(const SatelliteType &ty)
{
    double zExit = 1e30, rExit = 0.0, zMin = 1e30, zMax = -1e30, rBody = 0.0;
    if (ty.model)
        for (const SatComponent &c : ty.model->components)
        {
            const bool revolved = c.prim == PrimitiveKind::Cone || c.prim == PrimitiveKind::Cylinder ||
                                  c.prim == PrimitiveKind::Profile;
            const double hz = revolved ? 0.5 * c.height : 0.0;
            zMin = std::min(zMin, (double)c.position.z - hz);
            zMax = std::max(zMax, (double)c.position.z + hz);
            const double rxy = std::sqrt((double)c.position.x * c.position.x + (double)c.position.y * c.position.y);
            if (c.prim == PrimitiveKind::Cylinder)
                rBody = std::max(rBody, rxy + c.radius);
            if (c.prim == PrimitiveKind::Profile)
                for (const glm::vec2 &q : c.profile)
                    rBody = std::max(rBody, rxy + (double)q.y);
            const bool eng = c.name.rfind("engines", 0) == 0 || c.name.rfind("upper_nozzle", 0) == 0 ||
                             c.name.rfind("side_nozzle", 0) == 0;
            if (eng && (c.prim == PrimitiveKind::Cone || c.prim == PrimitiveKind::Profile)) // the exit at the bottom
            {
                zExit = std::min(zExit, (double)c.position.z - hz);
                rExit = std::max(rExit, rxy + c.radius);
            }
        }
    if (zMin > zMax)
        return glm::dvec3(0.0, 2.0, 40.0);
    if (rExit <= 0.0)
    {
        zExit = 0.0;
        rExit = std::max(0.7 * rBody, 0.5);
    }
    return glm::dvec3(zExit, rExit, zMax - zMin);
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
    const double windPhase = glm::two_pi<double>() * h01(B.seed, 24, 1);
    int stream = -1;           // a burn: its parcels are drawn as one polyline (consecutive in B.parcels)
    double lastEmit = -1e30;   // a gap in the emission starts a new stream
    int lastStage = -1, lastMode = -1;
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
                                               maxAge, windDir, windGain, vent ? 30.0 : 2000.0, windPhase));
        B.parcels.back().thin = vent;
        if (!vent)
        {
            if (stage != lastStage || mode != lastMode || tau - lastEmit > 1.6 * dtE)
                ++stream;
            B.parcels.back().stream = stream;
            lastEmit = tau;
            lastStage = stage;
            lastMode = mode;
        }
        B.parcels.back().stage = stage;
        B.parcels.back().dt = dtE;
    };
    // The pad's ground cloud: exhaust and the deluge's steam rolling out along the ground in the first seconds.
    for (int k = 0; k < 6; ++k)
    {
        const double ang = glm::two_pi<double>() * (k + h01(B.seed, 23, k)) / 6.0;
        const glm::dvec3 dir = std::cos(ang) * east + std::sin(ang) * north;
        const glm::dvec3 p = (kR + B.padBase + 20.0) * B.G0;
        B.parcels.push_back(launch::makeParcel(1.0 + 0.8 * k, p, 120.0 * dir + 20.0 * B.G0, vh.flow1 * 2.0,
                                               kappaAt(0.0), 0.95, dir, 60.0, kPlumeAgeLow, windDir, windGain, 2000.0,
                                               windPhase));
    }
    // The booster: ascent (every 1 s low, 1.5 s above ~20 km), then its burns. The parcels are the polyline's knots:
    // the trail between them is drawn as segments (launchFillVisuals), so the spacing sets how finely it bends.
    for (double tau = 1.0; tau < vh.t1; )
    {
        const double dtE = tau < 60.0 ? 1.0 : 1.5;
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
    // The upper stage to orbit: every 1 s. In near vacuum consecutive parcels separate at the exhaust speed (~3.4 km
    // a second of burn) and each spreads at ~2 km/s: at 2.5 s the youngest stood 8.5 km apart and swelled into a
    // string of separate puffs before they merged (the user's "poofs").
    for (double tau = vh.t1 + 1.0; tau < B.t2; tau += 1.0)
        emit(1, tau, 1.0, vh.flow2, vh.vex2, 0);
    // Its deorbit burn, and maybe a spinning vent of what is left (the spirals seen from Norway, New Zealand...).
    {
        // (every stage burns to deorbit since 2026-10-19, for B.burnDur; part of its engines)
        const double tb = B.tDeorbBurn - B.tL;
        for (double tau = tb; tau < tb + B.burnDur; tau += 1.0)
            emit(1, tau, 1.0, vh.flow2 * 0.3, vh.vex2, 2); // knots every second, like the ascent's
    }
    // A returning ship's landing burn (2026-10-19): its exhaust over the pad.
    if (B.shipLands)
        for (double tau = B.tShipFlip - B.tL + 0.5; tau < B.tShipCatch - B.tL; tau += 2.0)
            emit(1, tau, 2.0, vh.flow2 * 0.5, vh.vex2, 1);
    if (B.vents)
    {
        const double tv = ventStart(L, B) - B.tL;
        for (double tau = tv; tau < tv + 120.0; tau += 0.5)
            emit(1, tau, 0.5, 80.0, 700.0, 3); // ~10 t of residual propellant over two minutes
    }
}

// Crewed stations: rebuild a plane's cached batches when its station's dead second changes (a breakup, a rollback,
// the worker reaching it), so the schedule follows the event log (buildBatch's "station lost").
static void crewSync(const SatelliteSim &sim, SatelliteSim::LaunchState &L)
{
    if (!L.planesBuilt)
        return;
    if (L.crewDead.size() != L.planes.size())
        L.crewDead.assign(L.planes.size(), kessler::kNever);
    std::vector<int> changed;
    for (size_t pi = 0; pi < L.planes.size(); ++pi)
    {
        const LPlane &pl = L.planes[pi];
        if (!L.cat.programs[pl.prog].crew || pl.sats.empty())
            continue;
        int32_t d = kessler::kNever;
        if (sim.kesslerEnabled && 2ull * pl.sats[0] + 1 < sim.kRosterWords.size())
            d = std::min(sim.kRosterWords[2ull * pl.sats[0]], sim.kRosterWords[2ull * pl.sats[0] + 1]);
        if (d != L.crewDead[pi])
        {
            L.crewDead[pi] = d;
            changed.push_back((int)pi);
        }
    }
    if (changed.empty())
        return;
    for (auto it = L.batches.begin(); it != L.batches.end();)
    {
        const LBatch &B = it->second;
        // (a crew batch carries no train and no retirees: no GPU chunks to release)
        if (B.chunks[0].empty() && B.chunks[1].empty() &&
            std::find(changed.begin(), changed.end(), B.plane) != changed.end())
            it = L.batches.erase(it);
        else
            ++it;
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
    LaunchImpl::crewSync(*this, L);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<std::pair<uint64_t, LBatch *>> touchedP;
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
                        touchedP.push_back({key, &it->second});
                }
            }
        }
    }
    // (2026-10-20: the batches by pointer: Starmind's eight-a-launch trains made ~5000 live batches, and the hash
    // lookups over them each frame cost ~0.5 ms.)
    std::sort(touchedP.begin(), touchedP.end(), [](const auto &a, const auto &b) { return a.first < b.first; });
    std::vector<uint64_t> touched(touchedP.size());
    L.livePtr.resize(touchedP.size());
    for (size_t i = 0; i < touchedP.size(); ++i)
    {
        touched[i] = touchedP[i].first;
        L.livePtr[i] = touchedP[i].second;
    }
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
        if (selectedSatIndex == (int)(activeSatCount + slot) || viewerSatIndex == (int)(activeSatCount + slot))
            launchSlotEndedSelection(slot); // to its roster slot, or (a retiree) to its re-entry once listed
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
    // Free what no longer matters (a batch out of the window, retirees all down, launches off). The whole map every 8th
    // frame (and after a jump), else only the live batches (2026-10-20: ~6000 batches a frame cost ~0.3 ms).
    auto freeBatch = [&](LBatch &B) {
        for (int kind = 0; kind < 2; ++kind)
        {
            if (B.chunks[kind].empty())
                continue;
            const bool need = on && B.touched == L.frame && B.valid &&
                              (kind == 0 ? (t >= B.tD && t < B.tA) : (t >= B.tA && t < B.tA + 400.0 * kDay));
            if (!need && nStage + kChunk * (uint32_t)B.chunks[kind].size() <= LaunchState::kStageRecs)
            {
                releaseChunks(B.chunks[kind]);
                B.tw[kind] = -1e300;
                B.nUp[kind] = 0;
            }
        }
    };
    const bool fullFree = (L.frame % 8u) == 0u || !on || std::abs(t - L.lastFullFreeT) > 600.0;
    if (fullFree)
        L.lastFullFreeT = t;
    else
        for (LBatch *pb : L.livePtr)
            freeBatch(*pb);
    for (auto it = fullFree ? L.batches.begin() : L.batches.end(); it != L.batches.end();)
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
        GpuKepOrbit g{};
        SatOrbitElems ce{};
        if (!LaunchImpl::memberRecord(*this, B, mb, kind, t, tEpoch, g, ce))
            return;
        if (!putRecord(slot, g))
            return;
        const SatOrbit &so = satOrbits[mb.sat];
        SlotInfo &si = L.slots[slot];
        si.shown = true;
        si.live = true;
        si.tw = t;
        L.elems[slot] = ce;
        const double elA = ce.rSatM, elI = ce.incl, n = ce.meanMot;
        SatOrbit &o = satOrbits[activeSatCount + slot];
        o.typeIdx = so.typeIdx;
        o.constIdx = so.constIdx;
        o.incl = (float)elI;
        o.altM = (float)(elA - kR);
        o.R_sat = (float)elA;
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
            LBatch &B = *L.livePtr[bi];
            for (int kind = 0; kind < 2; ++kind)
            {
                const bool phase = kind == 0 ? (t >= B.tD && t < B.tA) : (t >= B.tA && t < B.tA + 400.0 * kDay);
                if (!phase)
                    continue;
                std::vector<uint32_t> &ch = B.chunks[kind];
                // While the train leaves the dispenser (and its first minutes, drifting from the stage at a fraction
                // of a m/s) every 2 s, so each member appears at its release and the separation stays smooth.
                // (2026-10-19) Also every 2 s for a train with a phasing residual (LMember::eps): its slow drift is no
                // free orbit (a circle flown faster than circular), and a Kepler record of it was off by 5-15 m after 20 s.
                const double relEnd = B.mem.empty() ? B.tD : B.mem.back().tRel;
                const double every = (kind == 0 && (t < relEnd + 600.0 || B.resid)) ? 2.0 : 20.0;
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

// The selection panel's second line for a launch record (formatSelectedSatInfo, refreshed every frame while one is
// selected): the satellite's type and what it is doing now, e.g. "Starlink V2 Mini - raising to slot, 412/565 km".
// Empty when the slot is not a train member or a retiree (the panel then shows the type alone).
std::string SatelliteSim::launchStatus(uint32_t slot) const
{
    if (!launchState || slot >= kLaunchCap - 2 || slot >= launchState->slots.size() || launchState->slots[slot].owner == 0)
        return {};
    const LaunchState &L = *launchState;
    const SlotInfo &si = L.slots[slot];
    auto it = L.batches.find(si.owner / 2);
    if (it == L.batches.end() || si.m >= it->second.mem.size())
        return {};
    const LBatch &B = it->second;
    const LMember &m = B.mem[si.m];
    const SatOrbit &o = satOrbits[m.sat];
    const char *tn = o.typeIdx < satTypes.size() ? satTypes[o.typeIdx].name.c_str() : "Satellite";
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double hKm = (glm::length(satOrbitStateAt(launchOrbitElems(slot), t).posEci) - kR) / 1000.0;
    char b[200];
    if (si.owner % 2 == 1)
        snprintf(b, sizeof(b), "%s - deorbiting, %.0f km", tn, hKm);
    else if (t < m.tRel)
        snprintf(b, sizeof(b), "%s - in the dispenser, %.0f km", tn, hKm);
    else if (t < m.sRaise)
        snprintf(b, sizeof(b), "%s - parking orbit, %.0f km", tn, hKm);
    else
        snprintf(b, sizeof(b), "%s - raising to slot, %.0f/%.0f km", tn, hKm, (m.rSlot - kR) / 1000.0);
    return b;
}

// A launch record that ends (hideSlot) while it is the selection or in the 3D view: a train member that arrived IS its
// roster slot now (same place, same model), so both move there; a retiree's selection waits for its re-entry fireball
// (selWant*, picked up in selTransientUpdate). Anything else ends the selection, as before.
void SatelliteSim::launchSlotEndedSelection(uint32_t slot)
{
    const int idx = (int)(activeSatCount + slot);
    const bool sel = selectedSatIndex == idx, view = viewerSatIndex == idx;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // The owner as the slot holds it (hideSlot has just cleared `live`, so launchSlotOwner would say "free").
    const uint64_t owner = launchState && slot < launchState->slots.size() ? launchState->slots[slot].owner : 0;
    const uint32_t m = owner ? launchState->slots[slot].m : 0;
    uint32_t sat = UINT32_MAX;
    const LBatch *B = nullptr;
    if (owner != 0 && launchState)
    {
        auto it = launchState->batches.find(owner / 2);
        if (it != launchState->batches.end() && m < it->second.mem.size())
        {
            B = &it->second;
            sat = B->mem[m].sat;
        }
    }
    char lb[200];
    snprintf(lb, sizeof(lb), "launch slot %u ended (selected %d, viewed %d): owner kind %d, %.0f s from arrival", slot,
             (int)sel, (int)view, owner ? (int)(owner % 2) : -1, B ? t - B->tA : 0.0);
    Log::line(lb);
    int to = -1;
    if (B && owner % 2 == 0 && sat < activeSatCount && t >= B->tA - 60.0)
        to = (int)sat;
    else if (B && owner % 2 == 1 && sat < activeSatCount && sel)
    {
        selWantKind = 3;
        selWantA = sat;
        selWantUntil = t + transientGraceS() + 5.0;
        selWantViewer = view;
    }
    if (sel)
    {
        selectedSatIndex = to;
        formatSelectedSatInfo();
    }
    if (view && to >= 0)
    {
        viewerSatIndex = to;
        viewerLastSelected = selectedSatIndex;
        viewerSyncModel();
        snprintf(viewerTitle, sizeof(viewerTitle), "%s", satLabel(to).c_str());
        viewerObsNextWall = 0.0;
    }
}

// ── Visuals: rockets, flames, plumes, re-entries ────────────────────────────────────────────────
void SatelliteSim::launchFillVisuals(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull)
{
    launchFillPadLights(); // the pads' lights at night, whether or not anything flies (2026-10-19)
    if (!launchState || !launchesEnabled || !kFireMapped)
        return;
    LaunchState &L = *launchState;
    L.nRockets = L.nPlumes = L.nReentries = 0;
    L.nFairings = 0;
    L.plumeDbg.clear();
    L.stackDraws.clear();
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
    for (size_t li = 0; li < L.live.size() && li < L.livePtr.size(); ++li)
    {
        const uint64_t key = L.live[li];
        LBatch &B = *L.livePtr[li];
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
                if (glm::length(kR * U0 - eye) > 3.0e6 && !transientWanted(3, mb.sat))
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
                if (glm::length(kR * U0 - eye) < 3.0e6 || transientWanted(4, key))
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
        // The payload fairing's halves (2026-10-20): separated at ~110 km, they fall away tumbling. Listed (selectable,
        // drawn as meshes up close) and, sunlit, a faint point each.
        if (B.tFair > 0.0 && t >= B.tFair && t < B.tFair + 1300.0)
        {
            auto fi = rocketTypes.find("rocket_" + vh.id + "_fairing");
            if (fi != rocketTypes.end())
            {
                LaunchImpl::ensureFairing(L, B, vh.fairingCenter);
                for (int h = 0; h < 2; ++h)
                {
                    glm::dvec3 P, V;
                    glm::dmat3 R;
                    if (!LaunchImpl::fairingAt(B, h, t, P, V, R))
                        continue;
                    const glm::dvec3 rel = P - eye;
                    const double range = std::max(glm::length(rel), 1.0);
                    const uint64_t fkey = (key << 3) ^ (uint64_t)(5 + h);
                    if (range > 4.0e6 && fkey != selTransientKey && fkey != followTrKey)
                        continue;
                    ++L.nFairings;
                    SelTransient st;
                    st.key = fkey;
                    st.kind = 7;
                    st.stage = (uint8_t)h;
                    st.a = key;
                    st.tau = t - B.tFair;
                    glm::dvec3 P1, V1;
                    glm::dmat3 R1;
                    st.pos = P;
                    st.vel = V;
                    st.acc = LaunchImpl::fairingAt(B, h, t - 1.0, P1, V1, R1) ? V - V1 : glm::dvec3(0.0);
                    st.axis = R[2];
                    st.type = fi->second;
                    st.dirEnu = glm::vec3(toEnu(rel) / range);
                    st.flare = 0.0f;
                    st.hasAtt = true;
                    st.attQ = glm::normalize(glm::quat_cast(R));
                    st.attOmega = B.fair[h].w;
                    st.attT0 = t;
                    const size_t idx = selTransients.size();
                    addSelTransient(st);
                    if (earthBlocks(P) || !sunlit(P))
                        continue;
                    // Sunlit: a white half-shell of some 60 m^2, about as bright as a coasting stage; it gives way to
                    // its mesh as that resolves.
                    double m = 4.3 + 5.0 * std::log10(range / 1.0e6) +
                               atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
                    const double px = rocketMeshPx(selTransients[idx], pixA, eye);
                    const double keep = 1.0 - glm::smoothstep(2.0, 8.0, px);
                    if (keep <= 0.0)
                        continue;
                    m -= 2.5 * std::log10(keep);
                    const double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - 0.97 * dayness);
                    if (flare < 2e-4)
                        continue;
                    selTransients[idx].flare = (float)flare;
                    if (kFireCount < kFireballMax)
                        fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rel) / range), (float)flare),
                                            glm::vec4(0.95f, 0.95f, 0.98f, (float)range)};
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
                    st.type = stage == 1 ? LaunchImpl::upperStageType(*this, L, B, t)
                                         : [&] {
                                               auto ti = rocketTypes.find("rocket_" + vh.id + (tau < vh.t1 ? "_stack" : "_booster"));
                                               return ti != rocketTypes.end() ? ti->second : -1;
                                           }();
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
                // Its payload stack, visible once the fairing is gone (2026-10-20): drawn by launchMeshJobs.
                const launch::Stack &stk = L.cat.programs[B.prog].stack;
                if (stage == 1 && B.tFair > 0.0 && t >= B.tFair && t < B.tStageGone &&
                    (stk.layout == launch::Stack::Flat || stk.layout == launch::Stack::Column))
                {
                    LaunchState::StackDraw sd;
                    sd.key = key;
                    sd.t = t;
                    sd.P = st.pos;
                    sd.V = st.vel;
                    sd.A = st.acc;
                    sd.S = LaunchImpl::upperFrame(L, B, tau);
                    sd.type = st.type;
                    L.stackDraws.push_back(sd);
                }
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
                    // Gone once the mesh is up: floored at 1e-4 (+10 mag) a point of magnitude 3 at 1000 km seen
                    // from 100 m (-17) still drew a blinding white disc on the hull (the user's QA shot 6).
                    const double px = rocketMeshPx(selTransients[trIdx], pixA, eye);
                    const double keep = 1.0 - glm::smoothstep(2.0, 8.0, px);
                    if (keep <= 0.0)
                        continue;
                    m -= 2.5 * std::log10(keep);
                }
            }
            double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * (1.0 - (burning ? 0.8 : 0.97) * dayness);
            if (flare < 2e-4)
                continue;
            if (trIdx < selTransients.size())
                selTransients[trIdx].flare = (float)flare; // drawn: pickable
            if (!burning)
            {
                if (kFireCount < kFireballMax)
                    fo[kFireCount++] = {glm::vec4(glm::vec3(dirE), (float)flare), glm::vec4(col, (float)range)};
                continue;
            }
            // The flame, from the engines out (2026-10-19, the user's QA: "a plume emerging from the engine", not a
            // white cloud round the vessel). Its geometry is the model's: the nozzle exit plane and the radius the
            // exits span (engineGeomOf), the nose axis (out of the engines: a landing burn's flame points down).
            glm::dvec3 vdir;
            {
                glm::dvec3 Pb;
                if (!LaunchImpl::rocketPos(L, B, stage, tau - 0.5, Pb))
                    Pb = P;
                vdir = P - Pb;
                vdir = glm::length(vdir) > 1e-3 ? glm::normalize(vdir) : glm::normalize(P);
                if (trIdx < selTransients.size() && glm::length(selTransients[trIdx].axis) > 0.5)
                    vdir = selTransients[trIdx].axis;
            }
            glm::dvec3 eg(0.0, 2.0, 40.0);
            {
                const int ty = trIdx < selTransients.size() ? selTransients[trIdx].type : -1;
                if (ty >= 0 && ty < (int)satTypes.size())
                {
                    auto it = L.engineGeom.find(ty);
                    if (it == L.engineGeom.end())
                        it = L.engineGeom.emplace(ty, LaunchImpl::engineGeomOf(satTypes[(size_t)ty])).first;
                    eg = it->second;
                }
            }
            const glm::dvec3 Pexit = P + eg.x * vdir;
            const double D = 2.0 * eg.y;
            const double hAlt = glm::length(P) - kR;
            // The flame lengthens and widens as the air thins (the exhaust under-expanded at altitude).
            const double hiK = glm::smoothstep(20000.0, 90000.0, hAlt);
            // A chain of three Gaussians: the bright core at the nozzle, the flame, its fading tail. Each starts far
            // enough behind the exit that its forward tail stays inside the bell (the old flame was one Gaussian
            // 950 m long centred half its length behind the stage: it reached past the nose).
            const double lenK[3] = {1.2, 3.5, 9.0}, widK[3] = {0.75, 1.1, 1.8}, shareK[3] = {0.5, 0.32, 0.18};
            double offK[3], sAk[3], sPk[3];
            for (int k = 0; k < 3; ++k)
            {
                const double len = D * lenK[k] * (1.0 + 2.5 * hiK);
                sAk[k] = 0.42 * len;
                sPk[k] = eg.y * widK[k] * (1.0 + 2.0 * hiK);
                offK[k] = k == 0 ? 2.2 * sAk[0] : offK[k - 1] + 1.2 * (sAk[k - 1] + sAk[k]);
            }
            const glm::dvec3 Pcore = Pexit - offK[0] * vdir;
            const double Lcore = offK[1] + sAk[1];
            const double Lpx = Lcore / std::max(range * pixA, 1e-9);
            // The point (bloom, glare) sits on the core, not on the hull (it drew a white disc on the aft end). Once
            // the flame resolves, a point's 1/r^2 no longer holds: its brightness stops growing where the flame is
            // ~4 px long, and most of the light goes to the flame's Gaussians (a glare remains at every scale).
            {
                const glm::dvec3 relC = Pcore - eye;
                const double rC = std::max(glm::length(relC), 1.0);
                const double rEff = std::max(rC, Lcore / (4.0 * pixA));
                const double pointK = glm::mix(1.0, 0.3, glm::smoothstep(4.0, 30.0, Lpx));
                double fP = flare * (range / rEff) * (range / rEff) * pointK;
                // ...and its glare stays modest once the flame is resolved (2026-10-20, issue 20261019_222938 shot 2:
                // from 200 m under a Super Heavy the point was ~2e4, a glare and bloom over the whole screen; "a
                // smaller glare with all 33 engines lit up is the ideal look"). Geometric blend toward a flare of 14
                // (just past the glare threshold) as the flame grows from 4 to 30 px; the lit nozzles (the mesh,
                // sat_mesh.frag) and the flame's Gaussians carry the light.
                {
                    const double capK = glm::smoothstep(4.0, 30.0, Lpx);
                    if (fP > 14.0 && capK > 0.0)
                        fP = std::exp(glm::mix(std::log(fP), std::log(14.0), capK));
                }
                if (fP > 2e-4 && kFireCount < kFireballMax)
                    fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(relC) / rC), (float)fP), glm::vec4(col, (float)rC)};
            }
            if (n < kMaxDustClouds && flare > 2e-3)
            {
                const double sMin = 0.6 * range * 2.0 * pixFull;
                const glm::dvec3 a = toEnu(vdir);
                glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
                b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
                const glm::dvec3 c2 = glm::cross(a, b2);
                // Its light: unresolved, the point's on-screen energy (toSky: a Gaussian of magnitude m carries what a
                // star of magnitude m shows, ~10 mag over a surface's physical brightness); resolved, its surface
                // brightness in the sky's own units (pi x radiance per unit solar irradiance: a sunlit white face is 1)
                // x kFlameCore, the core's excess over the point magnitudes' average (tuned for points). Without the
                // hand-over a flame a few hundred metres long seen from a kilometre was a blown-out white sheet over
                // the vessel even by day (the user's QA shots 5-6).
                // ...x 0.18 at night (2026-10-20): the night exposure is ~5.5x the day's, and a resolved flame's
                // Gaussians, hundreds of times past white, saturated out to 3-4 sigma: a round white blob several
                // times the engine cluster's size (issue 20261019_222938 shot 2). As if the eye adapted to the flame.
                const double kFlameCore = 30.0 * (1.0 - 0.82 * (1.0 - dayness));
                const double dayK = 1.0 - 0.8 * dayness;
                const double J = 0.6 * flare / std::max(dayK, 1e-3) * range * range / K;
                const double corePx = 2.0 * sPk[0] / std::max(range * pixA, 1e-9);
                const double resK = glm::smoothstep(1.5, 8.0, corePx);
                const double unitK = toSky * dayK * std::pow(glm::pi<double>() * kFlameCore / std::max(toSky * dayK, 1e-30), resK);
                for (int k = 0; k < 3 && n < kMaxDustClouds; ++k)
                {
                    const double sA = std::max(sAk[k], sMin), sP = std::max(sPk[k], sMin);
                    const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
                    auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                    const glm::dvec3 ce = toEnu(Pexit - offK[k] * vdir - eye);
                    // Faded out as the eye comes inside it (its Mahalanobis radius from 3 to 1.5 sigma): from under a
                    // rising stage, in its exhaust, the tail's Gaussian lit every direction at a third of its core's
                    // brightness - a white screen (issue 20261019_222938 shot 2).
                    const double mA = glm::dot(ce, a) / sA, mP2 = (glm::dot(ce, ce) - glm::dot(ce, a) * glm::dot(ce, a)) / (sP * sP);
                    const double inK = glm::smoothstep(1.5, 3.0, std::sqrt(mA * mA + std::max(mP2, 0.0)));
                    if (inK <= 0.0)
                        continue;
                    // Resolved and seen along its axis (from under a rising stage), a Gaussian's whole length adds up in
                    // each pixel: the flame was a white disc over the engines. Its surface brightness is held at its
                    // side-on value (x sP sqrt(q), q the inverse variance along the line of sight), and from the
                    // exhaust side, within ~40 deg of the axis, it thins further, so the lit nozzles show through
                    // (sat_mesh.frag; "a smaller glare with all 33 engines lit up").
                    double viewK = 1.0;
                    {
                        const double rc = std::max(glm::length(ce), 1e-3);
                        const double cz = glm::dot(-ce / rc, a); // + = the eye on the nose side
                        const double qv = cz * cz / (sA * sA) + (1.0 - cz * cz) / (sP * sP);
                        const double endK = glm::smoothstep(0.75, 0.97, -cz);
                        viewK = glm::mix(1.0, std::min(1.0, sP * std::sqrt(qv)) * glm::mix(1.0, 0.15, endK), resK);
                    }
                    GpuDustCloud &d = out[n++];
                    d.c = glm::vec4(glm::vec3(ce), (float)(J * shareK[k] * unitK * inK * viewK));
                    d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                                     (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
                    d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), 0.0f);
                    // The core white-hot, the tail the exhaust's colour.
                    d.col = glm::vec4(glm::mix(glm::vec3(1.0f, 0.95f, 0.88f), col, 0.5f * (float)k), 1.0f);
                    d.cone = coneOf(ce, 5.5 * std::max(sA, sP));
                }
                // The exhaust the flame lights: a night effect (by day the lit exhaust is lost in the sky; up close it
                // drew a white ellipse hundreds of metres across over the rocket), scaled with the engines, behind them.
                if (n < kMaxDustClouds)
                {
                    // 12 exit diameters low down (30 drew a 270-m ellipse round a Super Heavy whose lit column filled
                    // the screen from a few km), growing with the expanding exhaust aloft.
                    const double Rg = D * (12.0 + 318.0 * hiK);
                    const double gA = std::max(1.6 * Rg, sMin), gP = std::max(Rg, sMin);
                    const double jA = 1.0 / (gA * gA), jP = 1.0 / (gP * gP);
                    auto Mg = [&](int p, int q) { return jA * a[p] * a[q] + jP * (b2[p] * b2[q] + c2[p] * c2[q]); };
                    const glm::dvec3 ce = toEnu(Pexit - 2.0 * gA * vdir - eye);
                    // Gone as the eye comes within a few of its lengths: from inside (under a rising stage, in its
                    // exhaust) it was a white haze over the whole screen (issue 20261019_222938 shot 2).
                    // (0.12 of the flame, was 0.25: from a few km at night it saturated into a column taller than the rocket.)
                    const double nearK = glm::smoothstep(2.0 * gA, 6.0 * gA, glm::length(ce));
                    GpuDustCloud &g = out[n++];
                    g.c = glm::vec4(glm::vec3(ce), (float)(0.12 * J * unitK * (1.0 - 0.95 * dayness) * nearK));
                    g.ia = glm::vec4((float)Mg(0, 0), (float)Mg(1, 1), (float)Mg(2, 2),
                                     (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * gA * gP * gP)));
                    g.ib = glm::vec4((float)Mg(0, 1), (float)Mg(0, 2), (float)Mg(1, 2), 0.0f);
                    g.col = glm::vec4(glm::mix(col, glm::vec3(1.0f, 0.5f, 0.2f), 0.5f), 1.0f);
                    g.cone = coneOf(ce, 5.5 * gA);
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
        // One Gaussian of exhaust: centre C and trail direction axE (Earth-fixed), the half-length it stands for along
        // the trail (hl: its 1-sigma extent beyond its own radius), radius sig, cross-section Sigma (m^2).
        auto addPlume = [&](const glm::dvec3 &CE, const glm::dvec3 &axE, double hl, double sig, double Sigma,
                            double sweep, double albedo, double age, uint64_t rimSeed, bool trail) {
            if (Sigma < 1.0)
                return;
            const glm::dvec3 C = rotZ(CE, era);
            const glm::dvec3 rel = C - eye;
            const double range = std::max(glm::length(rel), 1.0);
            const double ext5 = 5.5 * std::sqrt(sig * sig + hl * hl);
            if (range > 6.0e6 + ext5 || earthHides(C, ext5))
                return; // over the horizon
            // Elongated along the trail (a Gaussian stands for a stretch of the path).
            const glm::dvec3 ax = rotZ(axE, era);
            const double sMin = 0.6 * range * 2.0 * pixFull;
            const double sP = std::max(sig, sMin);
            const double sA = std::max(std::sqrt(sig * sig + hl * hl), sMin);
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
            //     680 / 550 / 440 nm, x the secant where the ray crosses it). Counted ONCE, the secant capped at 12:
            //     the 15-35 km layer is thick, so a grazing ray's path through it is ~10-20 vertical columns, not 30
            //     twice; with both, the reddened low sunlight lost its green under the ice's blue and the plumes
            //     between the orange trail and the white bell came out magenta (the user's "odd purple trail").
            {
                const double rO = kR + 25000.0, r = glm::length(C), cz = glm::dot(C, sunD) / r;
                const double sin2 = (r / rO) * (r / rO) * (1.0 - cz * cz);
                const double sec = 1.0 / std::sqrt(std::max(1.0 - sin2, 1e-3));
                const double nX = r <= rO ? 1.0 : ((cz < 0.0 && sin2 < 1.0) ? 1.0 : 0.0);
                const double odO[3] = {0.019, 0.027, 0.0016};
                for (int ch = 0; ch < 3; ++ch)
                    tr[ch] *= std::exp(-odO[ch] * std::min(sec, 12.0) * nX);
            }
            // (2) high up the exhaust's ice is sub-micron: it scatters blue more than red (Angstrom exponent ~1.5,
            //     680 / 550 / 440 nm), normalised to keep the luminance; low down the droplets and soot are grey.
            //     The blue only where the sunlight still has its blue: ice lit by the reddened Sun is lit red.
            {
                const double ice = glm::smoothstep(30000.0, 100000.0, h) * glm::clamp(tr[2] / std::max(tr[0], 1e-6), 0.0, 1.0);
                const glm::dvec3 sp(0.727 / 0.9707, 1.0 / 0.9707, 1.398 / 0.9707);
                tr *= glm::mix(glm::dvec3(1.0), sp, ice);
            }
            const double sinE = glm::dot(glm::normalize(C), sunD);
            // The sky's light on it, relative (1 = a clear daytime sky), fading with the air above it.
            const double amb = glm::smoothstep(-0.1, 0.25, sinE) * std::exp(-std::max(h, 0.0) / 15000.0);
            // w = 4 x a per-parcel seed (cloud_march.comp's billows) + the sky light on it (unread there).
            const double seedK = (double)(launch::hash01(rimSeed, 7, 3) * 255.0);
            d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), (float)(4.0 * std::floor(seedK) + amb));
            // At night the sky's exposure is not physical (only ~5.5x the day's: see the Kessler dust), so a sunlit
            // plume against the dark sky reads far dimmer than it is; its sunlight is raised as the eye's sky darkens.
            d.col = glm::vec4(glm::vec3(tr * albedo * nightBoost), (float)(2.0 + albedo));
            d.cone = coneOf(toEnu(rel), 5.5 * std::max(sA, sP));
            plumes.push_back(c);
            // High up, once the expanding exhaust has swept up about its own mass of air it piles up at its rim: a
            // shell, limb-brightened (the jellyfish's bell). Along a trail the rim is a tube, whose limb is two lines
            // beside the trail (2 Gaussians as long as the segment, so neighbours join); only a lone parcel, or a
            // trail seen end-on, shows a ring (7 Gaussians round the line of sight). Rings on every parcel drew the
            // trail as a string of circles, and the young vacuum trail as puffs.
            const double shellK = (h > 60000.0 && sig > 2000.0) ? 0.45 * glm::smoothstep(0.3, 2.0, sweep) : 0.0;
            if (shellK > 0.02)
            {
                const double coreShare = 1.0 - shellK;
                plumes.back().d.c.w = (float)(Sigma * coreShare);
                plumes.back().pri *= coreShare;
                const glm::dvec3 vE = toEnu(rel) / range; // the line of sight
                glm::dvec3 aP = a - glm::dot(a, vE) * vE;
                const double aPl = glm::length(aP);
                const bool tube = trail ? aPl > 0.3 : (aPl > 1e-3 && (sA * aPl) / sP > 1.6);
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
                        sT = std::max(std::sqrt(sig * sig + hl * hl * aPl * aPl), sMin);
                    }
                    else
                    {
                        const double ang = glm::two_pi<double>() * ((double)k + h01(rimSeed, (uint64_t)k, 31)) / K;
                        nrm = std::cos(ang) * r1 + std::sin(ang) * r2;
                        tg = -std::sin(ang) * r1 + std::cos(ang) * r2;
                        sT = std::max(0.9 * sig, sMin);
                    }
                    const glm::dvec3 off = rr * nrm;
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
                    dr.ib = glm::vec4((float)Mr(0, 1), (float)Mr(0, 2), (float)Mr(1, 2), d.ib.w + (float)(4 * (k + 1)));
                    dr.col = d.col;
                    dr.cone = coneOf(ce, 5.5 * std::max(sT, sig));
                    plumes.push_back(cr);
                }
            }
            if (L.plumeDbg.size() < 4000)
                L.plumeDbg.push_back({range / 1000.0, h / 1000.0, sig / 1000.0, Sigma, tauPeak, age, tr * albedo, amb});
        };
        // Each parcel's state now (age >= 0: emitted; ok: still drawn).
        struct PState
        {
            glm::dvec3 cE;
            double sig = 0.0, cond = 0.0, sweep = 0.0;
            bool ok = false;
        };
        std::vector<PState> ps(B.parcels.size());
        for (size_t pi = 0; pi < B.parcels.size(); ++pi)
        {
            const double age = tau - B.parcels[pi].t0;
            ps[pi].ok = age >= 0.0 && B.parcels[pi].at(age, ps[pi].cE, ps[pi].sig, ps[pi].cond, &ps[pi].sweep);
        }
        const double gain = (double)launchPlumeGain;
        // A burn's parcels are the knots of a polyline: each draws the stretch of trail since the knot before it
        // (centre the midpoint, axis and length the chord between the two as they are NOW, so the trail bends and
        // shears with the winds that carry its parcels), and the youngest knot connects to the burning stage's nozzle
        // (the head grows with the burn instead of a parcel appearing every interval: the trail used to arrive in
        // jumps, each new parcel a needle reaching ahead of the rocket - the user's "intermittent, squished" trail).
        int prev = -1;
        for (size_t pi = 0; pi < B.parcels.size(); ++pi)
        {
            const launch::Parcel &pc = B.parcels[pi];
            const PState &s = ps[pi];
            if (!s.ok)
                continue;
            const double age = tau - pc.t0;
            if (prev >= 0 && B.parcels[(size_t)prev].stream != pc.stream)
                prev = -1;
            // Old parcels are wide and overlap their neighbours: every 2nd stands for two past 2 min, every 4th for
            // four past 10 min, every 8th past 30 min (the budget then reaches the whole bell, not just its young
            // part) - but only once they are wide against the spacing (pc.len): thinned earlier, a trail showed gaps.
            const int dec = pc.thin ? 1
                            : (age > 1800.0 && s.sig > 8.0 * pc.len) ? 8
                            : (age > 600.0 && s.sig > 4.0 * pc.len)  ? 4
                            : (age > 120.0 && s.sig > 2.0 * pc.len)  ? 2 : 1;
            const bool last = pi + 1 >= B.parcels.size() || B.parcels[pi + 1].stream != pc.stream || pc.stream < 0 ||
                              !ps[pi + 1].ok;
            if (pi % (size_t)dec != 0 && !last)
                continue;
            const uint64_t rimSeed = B.seed ^ (uint64_t)(pc.t0 * 10.0);
            if (pc.stream < 0 || prev < 0)
            {
                // A vent's jet stretch, the pad's ground cloud, or the first knot of a burn: as emitted.
                const double Sigma = pc.kappa * pc.m * s.cond * gain * (double)dec;
                addPlume(s.cE, pc.axis, 0.5 * pc.len, s.sig, Sigma, s.sweep, pc.albedo, age, rimSeed, pc.stream >= 0);
            }
            else
            {
                const PState &q = ps[(size_t)prev];
                glm::dvec3 seg = s.cE - q.cE;
                const double sl = glm::length(seg);
                const glm::dvec3 axE = sl > 1e-3 ? seg / sl : pc.axis;
                double Sigma = 0.0;
                for (int k = prev + 1; k <= (int)pi; ++k)
                    Sigma += B.parcels[(size_t)k].kappa * B.parcels[(size_t)k].m;
                // The chord's condensed share: the mean of its two ends (the younger end alone made a new segment,
                // born with nothing condensed, dimmer than the head it replaced: the trail pulsed once a knot).
                Sigma *= 0.5 * (s.cond + q.cond) * gain;
                // Half the chord as the along-trail sigma joins neighbours smoothly; a young segment is tighter, so
                // the head does not reach ahead of the stage.
                const double cA = glm::mix(0.35, 0.5, glm::smoothstep(0.0, 3.0 * std::max(pc.dt, 0.5), age));
                addPlume(0.5 * (s.cE + q.cE), axE, cA * sl, 0.5 * (s.sig + q.sig), Sigma, 0.5 * (s.sweep + q.sweep),
                         pc.albedo, age, rimSeed, true);
            }
            prev = (int)pi;
            // The head: from the youngest knot to the nozzle, while the burn goes on.
            if (pc.stream >= 0 && pi + 1 < B.parcels.size() && B.parcels[pi + 1].stream == pc.stream &&
                !ps[pi + 1].ok)
            {
                const launch::Parcel &nx = B.parcels[pi + 1];
                const double f = (tau - pc.t0) / std::max(nx.t0 - pc.t0, 1e-3);
                bool okN = false;
                const glm::dvec3 nz = LaunchImpl::rocketEcef(L, B, nx.stage, tau, okN);
                if (okN && f > 0.0 && f < 1.0 + 1e-6)
                {
                    glm::dvec3 seg = nz - s.cE;
                    const double sl = glm::length(seg);
                    const glm::dvec3 axE = sl > 1e-3 ? seg / sl : nx.axis;
                    // The exhaust's own spread over the head's age (its knot's radius, shrinking to the nozzle).
                    // Like a segment's: the mean of its ends' condensed shares (the nozzle's is 0) and radii (8 m), so
                    // when the next knot is born the segment that replaces the head is the same Gaussian.
                    const double condH = 0.5 * s.cond;
                    addPlume(0.5 * (nz + s.cE), axE, 0.35 * sl, 0.5 * (s.sig + 8.0),
                             nx.kappa * nx.m * f * condH * gain, 0.0, nx.albedo, 0.0, rimSeed ^ 0x5Aull, true);
                }
            }
        }
    }
    // The plumes: the brightest per pixel of sky first, as many as fit.
    if (!plumes.empty())
    {
        std::sort(plumes.begin(), plumes.end(), [](const Cand &a, const Cand &b) { return a.pri > b.pri; });
        // At most 384 plume Gaussians: a wide one covers much of the screen and costs every pixel it covers
        // (cloud_march.comp's loop), ~3 ms at 1000.
        const uint32_t nMax = std::min<uint32_t>(kMaxDustClouds, n + 384u);
        // Near the cut a Gaussian fades by its priority (to 0 at the cut, full at 3x it): as plumes age and the view
        // moves their ranks shift, and a hard cut switched pieces of a trail on and off (blocky in time warp).
        const size_t room = nMax > n ? (size_t)(nMax - n) : 0;
        const double cut = plumes.size() > room && room > 0 ? plumes[room].pri : 0.0;
        for (const Cand &c : plumes)
        {
            if (n >= nMax)
                break;
            out[n] = c.d;
            if (cut > 0.0)
                out[n].c.w *= (float)glm::clamp((c.pri - cut) / (2.0 * cut), 0.0, 1.0);
            ++n;
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
    j["fairing_halves_drawn"] = L.nFairings; // (2026-10-20)
    j["stack_meshes"] = L.nStacked;          // satellites drawn on their upper stage (payload stacks)
    j["stack_stages"] = L.stackDraws.size();
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
                          {"alt_km", fl ? (glm::length(P) - kR) / 1000.0 : 0.0}, {"nose_from_retrograde_deg", retroDeg},
                          {"fairing_sep_in_s", B.tFair > 0.0 ? B.tFair - now : 0.0},
                          {"model", [&] {
                               const int ty = LaunchImpl::upperStageType(*this, L, B, now);
                               return ty >= 0 && ty < (int)satTypes.size() ? satTypes[ty].modelId : std::string();
                           }()},
                          {"payload_layout", L.cat.programs[B.prog].stack.layout},
                          {"deploy_interval_s", B.deployInterval}});
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
    glm::dvec3 depOffMax(0.0), depVelMax(0.0);
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
            for (int a = 0; a < 3; ++a)
            {
                depOffMax[a] = std::max(depOffMax[a], std::abs(mb.depOff[a]));
                depVelMax[a] = std::max(depVelMax[a], std::abs(mb.depVel[a]));
            }
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
    // The ship's landing (2026-10-19): the hand-over from the entry glow to the descent (the position and velocity
    // jump there, Earth-fixed), the descent's fastest 1-s step (no jumps on the way down), the catch (its distance
    // from the pad's centre, its height above the pad's ground and its speed), and the boosters' catches likewise.
    {
        double maxEntryP = 0.0, maxEntryV = 0.0, maxHandP = 0.0, maxHandV = 0.0, maxDescStep = 0.0, maxCatchOff = 0.0, maxCatchV = 0.0;
        double catchAbove[2] = {1e9, -1e9}, boostOff = 0.0, boostAbove[2] = {1e9, -1e9};
        int nShip = 0, nCaught = 0, nSplash = 0, nBoost = 0;
        nlohmann::json ex = nlohmann::json::array();
        for (const auto &kv : L.batches)
        {
            const LBatch &B = kv.second;
            if (!B.valid)
                continue;
            const launch::Vehicle &vh = L.cat.vehicles[B.veh];
            if (vh.recovery == 1 && L.booster[B.veh].landed)
            {
                glm::dvec3 P;
                const double tc = L.booster[B.veh].tEnd;
                if (LaunchImpl::rocketPos(L, B, 0, tc + 5.0, P))
                {
                    const glm::dvec3 E = eciToEcef(P, B.tL + tc + 5.0);
                    const LPad &pd = L.pads[L.sitePad[B.site]];
                    const glm::dvec3 d = E - (kR + pd.elevM) * pd.g;
                    const double above = glm::dot(d, pd.g);
                    boostOff = std::max(boostOff, glm::length(d - above * pd.g));
                    boostAbove[0] = std::min(boostAbove[0], above);
                    boostAbove[1] = std::max(boostAbove[1], above);
                    ++nBoost;
                }
            }
            if (!B.shipLands)
                continue;
            ++nShip;
            (B.landPad >= 0 ? nCaught : nSplash)++;
            {
                // The entry's start vs the stage on its deorbit ellipse a moment before (and its velocity).
                glm::dvec3 Pa, Pb;
                if (LaunchImpl::rocketPos(L, B, 1, B.tStageGone - 1e-3 - B.tL, Pa) &&
                    LaunchImpl::rocketPos(L, B, 1, B.tStageGone - 1.0 - B.tL, Pb))
                {
                    const glm::dvec3 E0 = LaunchImpl::shipEntry(B, 0.0, nullptr, nullptr);
                    const glm::dvec3 E1 = LaunchImpl::shipEntry(B, 1.0, nullptr, nullptr);
                    maxEntryP = std::max(maxEntryP, glm::length(E0 - Pa));
                    maxEntryV = std::max(maxEntryV, glm::length((E1 - E0) - (Pa - Pb)));
                }
            }
            const double tH = B.tShipDesc;
            const glm::dvec3 a1 = eciToEcef(LaunchImpl::shipEntry(B, kShipEntryS, nullptr, nullptr), tH);
            const glm::dvec3 a0 = eciToEcef(LaunchImpl::shipEntry(B, kShipEntryS - 1.0, nullptr, nullptr), tH - 1.0);
            glm::dvec3 b0, b1;
            LaunchImpl::rocketPos(L, B, 1, tH - B.tL, b0);
            LaunchImpl::rocketPos(L, B, 1, tH + 1.0 - B.tL, b1);
            b0 = eciToEcef(b0, tH);
            b1 = eciToEcef(b1, tH + 1.0);
            maxHandP = std::max(maxHandP, glm::length(b0 - a1));
            maxHandV = std::max(maxHandV, glm::length((b1 - b0) - (a1 - a0)));
            glm::dvec3 prevV(0.0), prev(0.0);
            bool have = false;
            for (double t = tH; t < B.tShipCatch + 2.0; t += 1.0)
            {
                glm::dvec3 P;
                if (!LaunchImpl::rocketPos(L, B, 1, t - B.tL, P))
                    continue;
                const glm::dvec3 E = eciToEcef(P, t);
                if (have)
                {
                    const glm::dvec3 v = E - prev;
                    if (glm::length(prevV) > 0.0)
                        maxDescStep = std::max(maxDescStep, glm::length(v - prevV)); // m/s per s: the acceleration
                    prevV = v;
                }
                prev = E;
                have = true;
            }
            if (B.landPad >= 0)
            {
                const LPad &pd = L.pads[B.landPad];
                glm::dvec3 P, P1;
                LaunchImpl::rocketPos(L, B, 1, B.tShipCatch - B.tL, P);
                LaunchImpl::rocketPos(L, B, 1, B.tShipCatch - 1.0 - B.tL, P1);
                const glm::dvec3 E = eciToEcef(P, B.tShipCatch), E1 = eciToEcef(P1, B.tShipCatch - 1.0);
                const glm::dvec3 d = E - (kR + pd.elevM) * pd.g;
                const double above = glm::dot(d, pd.g);
                maxCatchOff = std::max(maxCatchOff, glm::length(d - above * pd.g));
                catchAbove[0] = std::min(catchAbove[0], above);
                catchAbove[1] = std::max(catchAbove[1], above);
                maxCatchV = std::max(maxCatchV, glm::length(E - E1));
            }
            if (ex.size() < 4)
            {
                nlohmann::json o;
                o["pad"] = B.landPad >= 0 ? L.cat.sites[L.pads[B.landPad].site].id : std::string("splashdown");
                o["deorbit_after_deploy_h"] = (B.tDeorbBurn - B.tD) / 3600.0;
                o["descent_from_km"] = glm::length(B.dX0) / 1000.0;
                o["descent_s"] = B.T1;
                o["landing_burn_s"] = B.T2;
                o["start_alt_km"] = B.dH0 / 1000.0;
                o["start_speed_m_s"] = glm::length(B.dV0);
                ex.push_back(o);
            }
        }
        j["ship_landings"] = nShip;
        j["ship_caught"] = nCaught;
        j["ship_splashdowns"] = nSplash;
        j["ship_entry_start_pos_m_max"] = maxEntryP;
        j["ship_entry_start_vel_m_s_max"] = maxEntryV;
        j["ship_handover_pos_m_max"] = maxHandP;
        j["ship_handover_vel_m_s_max"] = maxHandV;
        j["ship_descent_max_accel_m_s2"] = maxDescStep;
        j["ship_catch_off_centre_m_max"] = maxCatchOff;
        j["ship_catch_above_ground_m"] = {catchAbove[0], catchAbove[1]};
        j["ship_catch_speed_m_s_max"] = maxCatchV;
        j["ship_examples"] = ex;
        j["booster_catches"] = nBoost;
        j["booster_catch_off_centre_m_max"] = boostOff;
        j["booster_catch_above_ground_m"] = {boostAbove[0], boostAbove[1]};
    }
    j["batches"] = nB;
    j["members"] = nMem;
    j["arrival_err_m_max"] = maxArr;
    j["arrival_err_m_mean"] = nMem ? sumArr / nMem : 0.0;
    j["deploy_err_km_max"] = maxDep / 1000.0;
    j["deploy_err_km_mean"] = nMem ? sumDep / nMem / 1000.0 : 0.0;
    j["retiree_start_err_m_max"] = maxRet;
    j["phasing_residual_rad_max"] = maxEps;
    // the dispenser less a member's own path at its release (radial, along, cross): m and m/s
    j["release_offset_m_max"] = {depOffMax.x, depOffMax.y, depOffMax.z};
    j["release_rel_vel_mps_max"] = {depVelMax.x, depVelMax.y, depVelMax.z};
    j["members_with_residual"] = nResid;
    return j;
}

// The GPU's view of a launch record (sat_orbit.comp's Kepler block, in float as it runs there; orbitPhase's product
// is exact, so only the final angle is rounded).
static glm::dvec3 gpuKeplerEmu(const GpuKepOrbit &g, double deltaT)
{
    const float M = (float)launchsim::wrap2pi((double)g.M + (double)g.n * deltaT);
    const float dtf = (float)deltaT;
    const float argp = g.argp + g.argpDot * dtf, raan = g.raan + g.raanDot * dtf;
    const float e = g.e, a = g.a, ci = g.cosI, si = g.sinI;
    float E = M + e * std::sin(M);
    for (int k = 0; k < 5; ++k)
        E -= (E - e * std::sin(E) - M) / (1.0f - e * std::cos(E));
    const float cE = std::cos(E), sE = std::sin(E), sq = std::sqrt(std::max(0.0f, 1.0f - e * e));
    const float cO = std::cos(raan), sO = std::sin(raan), cw = std::cos(argp), sw = std::sin(argp);
    const glm::vec3 P(cw * cO - sw * sO * ci, cw * sO + sw * cO * ci, sw * si);
    const glm::vec3 Q(-sw * cO - cw * sO * ci, -sw * sO + cw * cO * ci, cw * si);
    return glm::dvec3((a * (cE - e)) * P + (a * sq * sE) * Q);
}

void SatelliteSim::launchLegacyRecords(bool on)
{
    LaunchImpl::gLegacyRecords = on;
    if (launchState)
        for (auto &kv : launchState->batches)
            kv.second.tw[0] = kv.second.tw[1] = -1e300; // rewrite everything now
    if (launchState)
        for (SlotInfo &si : launchState->slots)
            si.tw = -1e300;
}

// Harness `launch jitter` (2026-10-19): how far a train member's drawn position jumps when its record is rewritten
// (the CPU copy, which places the mesh, and the GPU's, which places the point), over the release, the first minutes
// (rewritten every 2 s) and later (every 20 s); the new records against the pre-2026-10-19 ones on the same batches.
nlohmann::json SatelliteSim::launchJitterJson(int maxBatches)
{
    nlohmann::json j;
    if (!launchState)
        return j;
    const LaunchState &L = *launchState;
    struct Phase
    {
        const char *name;
        double from, to, every;
    };
    static const Phase kPh[] = {{"release_0_60s", 1.0, 60.0, 2.0},
                                {"early_1_10min", 60.0, 600.0, 2.0},
                                {"later_10min_6h", 600.0, 21600.0, 20.0},
                                {"day_1_2", 86400.0, 172800.0, 20.0}};
    const bool keepLegacy = LaunchImpl::gLegacyRecords;
    for (int legacy = 1; legacy >= 0; --legacy)
    {
        LaunchImpl::gLegacyRecords = legacy != 0;
        nlohmann::json out;
        for (const Phase &ph : kPh)
        {
            double maxC = 0.0, sumC = 0.0, maxG = 0.0, sumG = 0.0, maxQ = 0.0;
            nlohmann::json worst;
            size_t n = 0, nb = 0;
            for (const auto &kv : L.batches)
            {
                const LBatch &B = kv.second;
                if (!B.valid || B.mem.empty() || (int)nb >= maxBatches)
                    continue;
                ++nb;
                for (size_t mi = 0; mi < B.mem.size(); mi += std::max<size_t>(1, B.mem.size() / 8))
                {
                    const LMember &mb = B.mem[mi];
                    if (std::abs(mb.eps) > 1e-9)
                        continue; // (a phasing residual: rewritten every 2 s, see LaunchState::update)
                    for (int k = 0; k < 6; ++k)
                    {
                        const double tw = mb.tRel + ph.from + (ph.to - ph.from) * (k + 0.37) / 6.0;
                        const double t1 = tw + ph.every;
                        const double tEpoch = std::floor((tw - 3.5 * 86400.0) / 86400.0) * 86400.0; // mid-rebake
                        GpuKepOrbit g0{}, g1{};
                        SatOrbitElems c0{}, c1{};
                        if (!LaunchImpl::memberRecord(*this, B, mb, 0, tw, tEpoch, g0, c0) ||
                            !LaunchImpl::memberRecord(*this, B, mb, 0, t1, tEpoch, g1, c1))
                            continue;
                        const double dC = glm::length(satOrbitStateAt(c0, t1).posEci - satOrbitStateAt(c1, t1).posEci);
                        const double dG = glm::length(gpuKeplerEmu(g0, t1 - tEpoch) - gpuKeplerEmu(g1, t1 - tEpoch));
                        // (the GPU's float floor: the same record a frame later, against its double position)
                        const double q = glm::length(gpuKeplerEmu(g1, t1 + 0.1 - tEpoch) -
                                                     satOrbitStateAt(c1, t1 + 0.1).posEci);
                        if (dC > maxC)
                            worst = {{"age_s", tw - mb.tRel}, {"raise_in_s", mb.sRaise - tw}, {"arrive_in_s", B.tA - tw},
                                     {"sep", {mb.sepDir.x, mb.sepDir.y, mb.sepDir.z}}, {"ecc", c0.ecc},
                                     {"alt_km", (c0.rSatM - kR) / 1000.0}, {"vehicle", L.cat.vehicles[B.veh].id}};
                        if (dC >= maxC)
                        {
                            // the path's acceleration less gravity (radial, along, cross): what no Kepler record follows
                            glm::dvec3 ra, rb, rc, vv;
                            const double hh = 2.0;
                            if (LaunchImpl::memberState(*this, B, mb, 0, tw - hh, ra, vv) &&
                                LaunchImpl::memberState(*this, B, mb, 0, tw, rb, vv) &&
                                LaunchImpl::memberState(*this, B, mb, 0, tw + hh, rc, vv))
                            {
                                const glm::dvec3 acc = (ra - 2.0 * rb + rc) / (hh * hh) +
                                                       launch::kGM * rb / std::pow(glm::length(rb), 3.0);
                                const glm::dvec3 U = glm::normalize(rb), Cn = glm::normalize(glm::cross(rb, rc - ra)),
                                                 A = glm::cross(Cn, U);
                                worst["accel_resid_rac"] = {glm::dot(acc, U), glm::dot(acc, A), glm::dot(acc, Cn)};
                            }
                        }
                        maxC = std::max(maxC, dC);
                        sumC += dC;
                        maxG = std::max(maxG, dG);
                        sumG += dG;
                        maxQ = std::max(maxQ, q);
                        ++n;
                    }
                }
            }
            out[ph.name] = {{"rewrite_s", ph.every},
                            {"samples", n},
                            {"cpu_jump_m_max", maxC},
                            {"cpu_jump_m_mean", n ? sumC / n : 0.0},
                            {"gpu_jump_m_max", maxG},
                            {"gpu_jump_m_mean", n ? sumG / n : 0.0},
                            {"gpu_vs_cpu_m_max", maxQ},
                            {"worst_cpu", worst}};
        }
        j[legacy ? "before" : "after"] = out;
    }
    LaunchImpl::gLegacyRecords = keepLegacy;
    return j;
}

// Selects the nearest drawn train member (kind 0) or retiree (1) above the horizon (harness `launch select`).
bool SatelliteSim::launchSelect(int kind, std::string &msg, int member)
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
        if (!si.shown || (int)(si.owner % 2) != kind || (member >= 0 && (int)si.m != member))
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
    bool padOn = launchPadGraphics;
    if (uiToggleRow(inp, ui, "LaunchPads", 0, "Launch site graphics", padOn,
                    "The pads on the ground: graded flat, concrete, flame trenches, tank farms, the towers (Starship's "
                    "catch tower), landing zones, their floodlights and beacons at night; no city lights within ~3 km."))
        launchPadGraphics = padOn;
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
    if (uiButton(inp, ui, "LaunchCatch", 0, "Watch a Starship catch",
                 "The next ship coming home to a tower: 90 s before the catch, 2.5 km from the pad (its descent, the "
                 "flip and the landing burn)."))
    {
        launchGoToLanding(0, 90.0, 2500.0, why);
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
bool SatelliteSim::launchGoToStageEvent(std::string &msg, int ev, const std::string &filter, int minMembers)
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
        uint64_t bkey = 0;
        for (const auto &kv : L.batches)
            if (kv.second.valid && std::abs(kv.second.tL - tl) < 1.0)
            {
                B = &kv.second;
                bkey = kv.first;
            }
        if (!B || (ev == 0 && !B->vents) || (ev == 1 && L.cat.vehicles[B->veh].upperDeorbit) || (ev == 4 && B->tFair < 0.0) ||
            (int)B->mem.size() < minMembers)
            continue;
        if (!filter.empty())
        {
            // (harness `launch deploy filter=Starmind`: a part of the constellation, program or vehicle name)
            auto low = [](std::string x) {
                for (char &ch : x)
                    ch = (char)std::tolower((unsigned char)ch);
                return x;
            };
            const std::string f = low(filter);
            const std::string hay = low(o.value("constellation", std::string()) + "|" + o.value("program", std::string()) +
                                        "|" + o.value("vehicle", std::string()));
            if (hay.find(f) == std::string::npos)
                continue;
        }
        // (ev 2: 50 s before the stage starts turning for its deorbit burn; ev 3: 10 s before it deploys; ev 4: 10 s
        // before its fairing separates.)
        const double tv = ev == 0   ? LaunchImpl::ventStart(L, *B) + 30.0
                          : ev == 1 ? B->tStageGone + 230.0
                          : ev == 2 ? B->tDeorbBurn - 100.0
                          : ev == 3 ? B->tD + 40.0
                                    : B->tFair + 40.0;
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
        if (ev >= 2)
            launchWantSelKey = (bkey << 3) ^ 2ull; // its upper stage, selected once listed (selTransientUpdate)
        char b[240];
        static const char *kWhat[] = {"vents in 20 s", "re-entry glow peaks in 20 s", "turns for its deorbit burn in 0 s",
                                      "deploys in 10 s", "drops its fairing in 10 s"};
        snprintf(b, sizeof(b), "%s upper stage from %s %s, %.0f km away", o["vehicle"].get<std::string>().c_str(),
                 o["site"].get<std::string>().c_str(), kWhat[std::clamp(ev, 0, 4)], glm::length(rel) / 1000.0);
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
// A re-entry the selection or Go to holds or waits for (kind 3 a retiree: a = its satellite; 4 an upper stage: a = the
// batch): listed even where it is not drawn (behind the Earth, out of range, too faint), so the selection carries on.
bool SatelliteSim::transientWanted(uint8_t kind, uint64_t a) const
{
    auto rides = [&](const SelTransient &c) {
        return c.a == a && (c.kind == kind || (kind == 4 && c.kind == 1 && c.stage == 1));
    };
    if (selIsTransient() && rides(selTransientCur))
        return true;
    if (selWantKind == kind && selWantA == a)
        return true;
    if (followIsTransient() && (followTrKey ? rides(followTrCur) : (followWantKind == kind && followWantA == a)))
        return true;
    // A retiree's own record selected or followed (its fireball comes next).
    if (kind == 3 && a < activeSatCount && launchState)
        for (int idx : {selectedSatIndex, followActive ? followSatIndex : -1})
            if (idx >= (int)activeSatCount && (uint32_t)idx < poolIdx0() && (uint32_t)idx < transientSatIdx() - 1)
            {
                uint32_t m = 0;
                const uint64_t owner = launchSlotOwner((uint32_t)idx - activeSatCount, m);
                if (owner % 2 == 1)
                {
                    auto it = launchState->batches.find(owner / 2);
                    if (it != launchState->batches.end() && m < it->second.mem.size() && it->second.mem[m].sat == a)
                        return true;
                }
            }
    return false;
}

void SatelliteSim::addSelTransient(const SelTransient &s)
{
    if (selTransients.size() < 16384)
        selTransients.push_back(s);
}

void SatelliteSim::selTransientUpdate()
{
    selTransientsT = (double)simDayJ2000 * 86400.0 + simSecInDay; // the time the list was made at
    if (launchWantSelKey)
    {
        // (2026-10-20) the stage a `launch deploy` / `fairing` / `deorbit` jump framed: selected once it is listed.
        for (const SelTransient &s : selTransients)
            if (s.key == launchWantSelKey)
            {
                selTransientTake(s);
                selectedSatIndex = (int)transientSatIdx();
                selectedPlanetIndex = -1;
                launchWantSelKey = 0;
                break;
            }
    }
    followTransientUpdate();
    const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // A retiree's record ended while selected: its re-entry fireball, once the list has it (launchSlotEndedSelection).
    if (selWantKind)
    {
        if (selectedSatIndex >= 0 || tNow > selWantUntil || tNow < selWantUntil - 3600.0)
            selWantKind = 0;
        else
            for (const SelTransient &s : selTransients)
                if (s.kind == selWantKind && s.a == selWantA)
                {
                    selTransientTake(s);
                    selectedSatIndex = (int)transientSatIdx();
                    selectedPlanetIndex = -1;
                    if (selWantViewer && viewerSatIndex >= (int)activeSatCount && viewerSatIndex < (int)poolIdx0())
                    {
                        viewerSatIndex = selectedSatIndex;
                        viewerLastSelected = selectedSatIndex;
                        viewerSyncModel();
                    }
                    selWantKind = 0;
                    formatSelectedSatInfo();
                    break;
                }
    }
    // Go to rides a transient that is also the selection: the selection IS the followed object, through every
    // handover follow makes (staging, the ship's entry, the fireball), even while the list does not show it.
    if (followIsTransient() && followTrKey != 0 && selIsTransient() &&
        (selTransientKey == followTrKey || (selFollowKeyPrev != 0 && selTransientKey == selFollowKeyPrev)))
    {
        SelTransient c = followTrCur;
        for (const SelTransient &s : selTransients)
            if (s.key == c.key)
            {
                c.axis = s.axis;
                c.flare = s.flare;
                c.dirEnu = s.dirEnu;
                break;
            }
        selTransientTake(c);
        selFollowKeyPrev = followTrKey;
        formatSelectedSatInfo();
        return;
    }
    if (!followIsTransient())
        selFollowKeyPrev = 0;
    else if (followTrKey != 0)
        selFollowKeyPrev = followTrKey;
    // A selected train member or retiree: its panel line carries its height (launchStatus), refreshed every frame.
    if (selectedSatIndex >= (int)activeSatCount && selectedSatIndex < (int)poolIdx0() && !selIsTransient())
        formatSelectedSatInfo();
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
    // An expendable upper stage's listing gives way to its re-entry fireball (kind 4, the same batch) as it starts to
    // glow, before the model's tStageGone says "fireball" (transientContinue): take it as soon as it is listed.
    if (!f && selTransientCur.kind == 1 && selTransientCur.stage == 1)
        for (const SelTransient &s : selTransients)
            if (s.kind == 4 && s.a == selTransientCur.a)
            {
                f = &s;
                break;
            }
    if (!f)
    {
        // Out of view this frame (behind the Earth, below the brightness cut) or over: gone for good after 2 s (or a
        // few frames of a fast warp). A stage that has just ended in its re-entry fireball waits longer for the list to
        // show it (as Go to does: followWantUntil), or the selection and the Info window were dropped at the handover.
        SelTransient cw = selTransientCur;
        uint8_t wantFb = 0;
        if (transientContinue(cw, t, wantFb))
        {
            // (2026-10-20) A rocket or the ship the list does not hold this frame (not drawn, out of the list's reach,
            // its visuals skipped): the launch model places it, so the selection stays, by its batch key.
            selTransientTake(cw);
            formatSelectedSatInfo();
            return;
        }
        if (std::abs(t - selTransientSeenT) > transientGraceS() + (wantFb ? 5.0 : 0.0))
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
    LaunchImpl::transientAtt(e, s);
    e.engineGlow = (s.kind == 1 && s.burn != 0) ? 1.0f : 0.0f; // its nozzles lit (sat_mesh.frag)
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
        if (s.stage == 1)
            s.type = LaunchImpl::upperStageType(*this, L, B, t); // its fairing goes (2026-10-20)
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

// Where an object (a stage, the ship) is at time t by the launch model: the handover's "from" must be the predecessor
// at the SAME time as its successor. followTrCur alternates between the list's time and the frame's (a frame apart: at
// 10x, ~0.17 s, ~300 m at staging), and the jump eased out by updateFollow carried that motion as a 1.3-km offset of
// the camera from the rocket it follows (2026-10-19: the rocket left the frame at staging).
glm::dvec3 SatelliteSim::transientPosAt(const SelTransient &s, double t) const
{
    SelTransient p = s;
    return transientExactState(p, t) ? p.pos : s.pos;
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
    // Time running backwards (or a jump back, 2026-10-20): the ship's entry before it began is its upper stage, an
    // upper stage before staging is the stack again (a selection taken from a list made at a later time came out as
    // "upper stage" at T+74 s, placed on the stack's track).
    if (cur.kind == 2 && t < B.tStageGone)
        cur.kind = 1;
    if (cur.kind == 1 && cur.stage == 1 && t - B.tL < vh.t1)
    {
        cur.stage = 0;
        cur.key = (cur.a << 3) ^ 1ull;
        cur.type = typeOf("_stack");
        cur.tau = t - B.tL;
        return transientExactState(cur, t);
    }
    // The stack separates: its upper stage carries on (the booster is another object).
    if (cur.kind == 1 && cur.stage == 0 && cur.tau < vh.t1 && t - B.tL >= vh.t1)
    {
        cur.stage = 1;
        cur.key = (cur.a << 3) ^ 2ull;
        cur.type = typeOf("_upper");
    }
    // The ship's entry hands back to the rocket path for its descent, landing burn and catch (2026-10-19).
    if (cur.kind == 2 && B.shipLands && t >= B.tShipDesc)
    {
        cur.kind = 1;
        cur.stage = 1;
        cur.type = typeOf("_upper");
    }
    if (cur.kind == 1 && cur.stage == 1 && t >= B.tStageGone && (!B.shipLands || t < B.tShipDesc))
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
                followTakeTransient(c, transientPosAt(followTrCur, t), t);
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
            snprintf(followLabel, sizeof(followLabel), "Following %s", transientLabelOf(s).c_str()); // its height moves
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
        // Rockets at this frame's exact time (the list is a frame old: at a fast warp, many seconds), along the
        // mission: a stack that staged since the last frame is its upper stage now.
        if (followTrKey != 0)
        {
            SelTransient c = followTrCur;
            uint8_t want = 0;
            if (transientContinue(c, t, want))
            {
                if (c.key != followTrKey || c.kind != followTrCur.kind)
                    followTakeTransient(c, transientPosAt(followTrCur, t), t);
                else
                {
                    followTrCur = c;
                    transientWriteSlot(kLaunchCap - 2, c, t);
                }
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
    // A pad's mesh (its tower, mount, tanks) is scenery, a payload stack's (2026-10-20) part of its stage: not
    // selectable.
    if (satIndex >= (int)(base - kPadMeshSlots - kStackMeshSlots) && satIndex < (int)base)
        return -1;
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
    LaunchImpl::transientAtt(e, s);
    e.engineGlow = (s.kind == 1 && s.burn != 0) ? 1.0f : 0.0f; // its nozzles lit (sat_mesh.frag)
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
    if (!launchState)
        return jobs;
    LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // The pads' static meshes (2026-10-19): those over ~1.4 px, nearest first, each kept in its reserved slot while
    // it stays. Ground-fixed: the state is the pad's at t with the Earth's rotation, the "velocity" axis its heading
    // (the model's +X), so the tower stands where the ground pattern draws its base.
    if (launchPadGraphics)
    {
        LaunchImpl::ensureCatalog(L);
        const glm::dvec3 omega(0.0, 0.0, 7.2921159e-5);
        struct PC
        {
            double px;
            int p;
            int type;
        };
        std::vector<PC> pcs;
        for (size_t pi = 0; pi < L.pads.size(); ++pi)
        {
            const LPad &pd = L.pads[pi];
            auto ti = rocketTypes.find(pd.kind == 2 ? "pad_starship" : pd.kind == 1 ? "pad_falcon" : "pad_generic");
            if (ti == rocketTypes.end())
                continue;
            const SatMeshRenderer::TypeMesh *tm = meshRenderer.typeMesh(ti->second);
            if (!tm)
                continue;
            const glm::dvec3 P = ecefToEci((kR + pd.elevM) * pd.g, t);
            const double px = 2.0 * std::max(1.0, (double)tm->boundsRadius) / std::max(glm::length(P - obsEci), 1.0) /
                              std::max(pixAngle, 1e-9);
            if (px > 1.4)
                pcs.push_back({px, (int)pi, ti->second});
        }
        std::sort(pcs.begin(), pcs.end(), [](const PC &a, const PC &b) { return a.px > b.px || (a.px == b.px && a.p < b.p); });
        if (pcs.size() > (size_t)kPadMeshSlots)
            pcs.resize(kPadMeshSlots);
        bool usedP[kPadMeshSlots] = {};
        int slotP[kPadMeshSlots];
        for (size_t w = 0; w < pcs.size(); ++w)
        {
            slotP[w] = -1;
            for (int k = 0; k < kPadMeshSlots; ++k)
                if (!usedP[k] && L.padMeshKey[k] == (uint64_t)pcs[w].p + 1)
                {
                    usedP[k] = true;
                    slotP[w] = k;
                    break;
                }
        }
        for (size_t w = 0; w < pcs.size(); ++w)
            if (slotP[w] < 0)
                for (int k = 0; k < kPadMeshSlots; ++k)
                    if (!usedP[k])
                    {
                        usedP[k] = true;
                        slotP[w] = k;
                        break;
                    }
        for (int k = 0; k < kPadMeshSlots; ++k)
            if (!usedP[k])
                L.padMeshKey[k] = 0;
        const uint32_t pbase = kLaunchCap - 2 - kRocketMeshSlots - kPadMeshSlots;
        for (size_t w = 0; w < pcs.size(); ++w)
        {
            const LPad &pd = L.pads[pcs[w].p];
            const int k = slotP[w];
            L.padMeshKey[k] = (uint64_t)pcs[w].p + 1;
            const uint32_t slot = pbase + (uint32_t)k, idx = activeSatCount + slot;
            if (idx >= satOrbits.size() || slot >= L.elems.size())
                continue;
            SatOrbitElems &e = L.elems[slot];
            e = SatOrbitElems{};
            e.fixedState = true;
            e.fixPos = ecefToEci((kR + pd.elevM) * pd.g, t);
            e.fixVel = glm::cross(omega, e.fixPos);
            e.fixAcc = glm::cross(omega, e.fixVel);
            e.fixAxis = ecefToEci(pd.u, t);
            e.fixT0 = t;
            satOrbits[idx].typeIdx = (uint32_t)pcs[w].type;
            jobs.push_back((int)idx);
        }
    }
    if (!launchesEnabled)
        return jobs;
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
        LaunchImpl::transientAtt(e, s);
        e.engineGlow = (s.kind == 1 && s.burn != 0) ? 1.0f : 0.0f; // its nozzles lit (sat_mesh.frag)
        // The transients are the last frame's: their time is the last fill's.
        e.fixT0 = selTransientsT;
        (void)t;
        satOrbits[idx].typeIdx = (uint32_t)s.type;
        jobs.push_back((int)idx);
    }
    launchStackJobs(pixAngle, obsEci, jobs);
    return jobs;
}

// The payload stacks (2026-10-20, issue 20261019_225243): the satellites still on an upper stage whose fairing has gone
// and its dispenser, as meshes in the reserved stack slots — the nearest stacks first, each satellite stowed (folded,
// its arrays packed) in its slot's attitude, posed rigidly with the stage (fixedState from the stage's listed state).
// A satellite leaves the stack at its release and is from then on its launch record (memberState: from the same
// point, in the same attitude). Not selectable.
void SatelliteSim::launchStackJobs(double pixAngle, const glm::dvec3 &obsEci, std::vector<int> &jobs)
{
    LaunchState &L = *launchState;
    struct Want
    {
        uint64_t key; // batch key ^ (what + 1) << 48: what = member index, or 0xFFFF the dispenser
        int type;
        glm::dvec3 P, V, A;
        glm::dmat3 R;
        double t;
        bool stowed;
    };
    std::vector<Want> want;
    std::vector<std::pair<double, size_t>> order;
    for (size_t i = 0; i < L.stackDraws.size(); ++i)
    {
        const LaunchState::StackDraw &sd = L.stackDraws[i];
        const SatMeshRenderer::TypeMesh *tm = sd.type >= 0 ? meshRenderer.typeMesh(sd.type) : nullptr;
        const double r = tm ? std::max(1.0, (double)tm->boundsRadius) : 10.0;
        const double px = 2.0 * r / std::max(glm::length(sd.P - obsEci), 1.0) / std::max(pixAngle, 1e-9);
        if (px > 1.4)
            order.push_back({px, i});
    }
    std::sort(order.begin(), order.end(), [](const auto &a, const auto &b) { return a.first > b.first; });
    for (const auto &o : order)
    {
        const LaunchState::StackDraw &sd = L.stackDraws[o.second];
        auto bit = L.batches.find(sd.key);
        if (bit == L.batches.end() || !bit->second.valid)
            continue;
        const LBatch &B = bit->second;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        const launch::Stack &stk = L.cat.programs[B.prog].stack;
        if (!stk.dispenser.empty())
        {
            auto di = rocketTypes.find("rocket_dispenser_" + stk.dispenser);
            if (di != rocketTypes.end())
                want.push_back({sd.key ^ (0xFFFFull << 48), di->second, sd.P + sd.S * glm::dvec3(0.0, 0.0, vh.payloadBaseM),
                                sd.V, sd.A, sd.S, sd.t, false});
        }
        for (size_t mi = 0; mi < B.mem.size() && want.size() < (size_t)kStackMeshSlots; ++mi)
        {
            const LMember &mb = B.mem[mi];
            if (!mb.stacked || sd.t >= mb.tRel || !LaunchImpl::memberAllowed(*this, B, mb))
                continue;
            const uint32_t ty = satOrbits[mb.sat].typeIdx;
            if (ty >= satTypes.size())
                continue;
            want.push_back({sd.key ^ ((uint64_t)(mi + 1) << 48), (int)ty, sd.P + sd.S * mb.slotO, sd.V, sd.A,
                            sd.S * mb.slotR, sd.t, true});
        }
        if (want.size() >= (size_t)kStackMeshSlots)
            break;
    }
    if (want.size() > (size_t)kStackMeshSlots)
        want.resize(kStackMeshSlots);
    // Keep each object in the slot it had (its mesh fade), give the rest the free ones.
    bool used[kStackMeshSlots] = {};
    std::vector<int> slotOf(want.size(), -1);
    for (size_t w = 0; w < want.size(); ++w)
        for (int k = 0; k < kStackMeshSlots; ++k)
            if (!used[k] && L.stackMeshKey[k] == want[w].key)
            {
                used[k] = true;
                slotOf[w] = k;
                break;
            }
    for (size_t w = 0; w < want.size(); ++w)
        if (slotOf[w] < 0)
            for (int k = 0; k < kStackMeshSlots; ++k)
                if (!used[k])
                {
                    used[k] = true;
                    slotOf[w] = k;
                    break;
                }
    for (int k = 0; k < kStackMeshSlots; ++k)
        if (!used[k])
            L.stackMeshKey[k] = 0;
    const uint32_t sbase = kLaunchCap - 2 - kRocketMeshSlots - kPadMeshSlots - kStackMeshSlots;
    L.nStacked = 0;
    for (size_t w = 0; w < want.size(); ++w)
    {
        const Want &W = want[w];
        const int k = slotOf[w];
        L.stackMeshKey[k] = W.key;
        const uint32_t slot = sbase + (uint32_t)k, idx = activeSatCount + slot;
        if (idx >= satOrbits.size() || slot >= L.elems.size())
            continue;
        SatOrbitElems &e = L.elems[slot];
        e = SatOrbitElems{};
        e.fixedState = true;
        e.fixPos = W.P;
        e.fixVel = W.V;
        e.fixAcc = W.A;
        e.fixAxis = W.R[2];
        e.fixT0 = W.t;
        e.attFix = true;
        e.attQ = glm::normalize(glm::quat_cast(W.R));
        e.attT0 = W.t;
        if (W.stowed)
        {
            e.restJoints = true;
            e.unfoldT0 = W.t + 1.0e9; // stowed: progress 0
            e.unfoldDur = 1.0;
        }
        satOrbits[idx].typeIdx = (uint32_t)W.type;
        jobs.push_back((int)idx);
        L.nStacked += W.stowed ? 1u : 0u;
    }
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
    if (s.kind <= 2 || s.kind == 4 || s.kind == 7)
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
    case 7:
        snprintf(b, sizeof(b), "%s payload fairing half, %.0f s after separation, %.0f km up, %.2f km/s", veh, s.tau, hKm,
                 vKms);
        break;
    default:
        snprintf(b, sizeof(b), "Re-entry: debris fragment, %.0f km up", hKm);
        break;
    }
    return b;
}

bool SatelliteSim::selectTransient(int kind, std::string &msg, int stage, bool nearest)
{
    const SelTransient *best = nullptr;
    const glm::dvec3 eye(obsECI);
    for (const SelTransient &s : selTransients)
    {
        // nearest (2026-10-20, harness `launch select ... nearest=1`): any listed one with a model, drawn or not.
        const bool ok = nearest ? (s.type >= 0 || s.flare > 0.0f) : s.flare > 0.0f;
        if (!ok || !(kind == 0 || s.kind == kind) || !(stage < 0 || s.stage == stage))
            continue;
        if (!best || (nearest ? glm::length(s.pos - eye) < glm::length(best->pos - eye) : s.flare > best->flare))
            best = &s;
    }
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
    LaunchImpl::crewSync(*this, L);
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
            double tLost = 0.0;
            const int fate = rosterFateAt(L.planes[B.plane].sats[0], t, &tLost);
            if (fate != 0)
            {
                // Launched before its station was lost (issue 20261019_231126): nothing to dock with.
                m.status = std::string("The station is ") + rosterFateName(fate) + " (" + fmtUtc(tLost) +
                           "); rendezvous called off";
                m.tNext = 0.0;
            }
            else if (t < B.tL + B.t2)
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

bool SatelliteSim::launchRocketTransient(uint64_t key, double t, SelTransient &out) const
{
    if (!launchState)
        return false;
    const LaunchState &L = *launchState;
    auto it = L.batches.find(key);
    if (it == L.batches.end() || it->second.veh < 0)
        return false;
    const LBatch &B = it->second;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const double tau = t - B.tL;
    // The payload's vehicle: the stack before staging, the upper stage after (its ship entry, its landing).
    SelTransient c;
    c.kind = 1;
    c.a = key;
    c.stage = tau < vh.t1 ? 0 : 1;
    c.tau = std::min(tau, vh.t1 - 1e-3); // transientContinue moves a stack past staging itself
    c.key = (key << 3) ^ (uint64_t)(c.stage + 1);
    auto ti = rocketTypes.find("rocket_" + vh.id + (c.stage == 1 ? "_upper" : "_stack"));
    c.type = ti != rocketTypes.end() ? ti->second : -1;
    uint8_t want = 0;
    if (!transientContinue(c, t, want))
        return false;
    if (c.kind == 1)
    {
        glm::dvec3 ax = LaunchImpl::stageNose(L, B, c.stage, c.tau);
        c.axis = glm::length(ax) > 0.5 ? ax : glm::normalize(c.pos);
    }
    const glm::dvec3 rel = c.pos - glm::dvec3(obsECI);
    const double d = glm::length(rel);
    if (d > 1.0)
        c.dirEnu = glm::vec3(glm::dot(rel, glm::dvec3(eci2enuX)) / d, glm::dot(rel, glm::dvec3(eci2enuY)) / d,
                             glm::dot(rel, glm::dvec3(eci2enuZ)) / d);
    c.flare = 0.0f;
    out = c;
    return true;
}

bool SatelliteSim::launchSelectRocketOf(uint64_t key, std::string &msg)
{
    const SelTransient *best = nullptr;
    for (const SelTransient &s : selTransients)
        if ((s.kind == 1 || s.kind == 2) && s.a == key && (!best || s.stage > best->stage))
            best = &s; // the upper stage (the payload's) once it flies alone
    // (2026-10-20, issue 20261019_224758) Drawn or not: the list holds rockets within 4000 km only (and the selected one
    // at any range); beyond, the launch model places it. It was "not drawn from here" whenever the head was not drawn.
    SelTransient made;
    if (!best)
    {
        const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
        if (!launchRocketTransient(key, selTransientsT > 0.0 ? selTransientsT : tNow, made))
        {
            msg = "that rocket is not flying now";
            return false;
        }
        best = &made;
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

// ── Launch pads on the ground (2026-10-19) ──────────────────────────────────────────────────────────────────────
// The user: "Launchpads and landing pads should probably have specialized graphics / play well with the city light
// graphics; we may need to establish exclusion zones near launch sites for rendering city grids." Three parts:
//  - the flat parts are a ground pattern in sat_sky.frag (padGround: graded ground, concrete apron, flame trench /
//    deflector, tank farms, roads, a landing zone; exactly box-filtered, so they average out with the footprint and
//    a pad from orbit is a pale speck, never a blob), with the terrain graded flat under them (terrain_detail.glsl
//    tdPadFlat) and no city grid, glitter, farms or city sprites within exclR (sat_sky.frag, city_sprites.comp);
//  - the tower (Starship: the catch tower with its chopsticks over the mount), the mount, masts and tanks as a static
//    mesh (data/satellite_models/pad_*.json, tools/make_pad_models.py), in reserved launch slots (launchMeshJobs);
//  - the floodlights, the beacons and the landing zone's lights at night as point sprites (launchFillPadLights).
// Everything in the pad's (u, v) frame: u its heading (the middle of its launch azimuths), v = up x u.

static glm::dvec3 padPointEcef(const LPad &p, double u, double v, double h)
{
    return (kR + p.elevM + h) * p.g + u * p.u + v * p.v;
}

void SatelliteSim::launchFillPadParams(GpuCloudParams &cp)
{
    cp.padInfo = glm::vec4(0.0f);
    if (!launchPadGraphics || !launchState)
        return;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (L.pads.empty())
        return;
    // The observer's ENU basis exactly as the shaders build it, from its SEA-LEVEL point (as fillSolarSites).
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const glm::dvec3 ex = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
    const glm::dvec3 ey = glm::cross(up, ex);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, up)); };
    const double R = kR;
    struct C
    {
        int p;
        double d2;
    };
    std::vector<C> cs;
    for (size_t p = 0; p < L.pads.size(); ++p)
    {
        const double d2 = glm::dot(L.pads[p].g - up, L.pads[p].g - up) * R * R;
        if (d2 < 2500e3 * 2500e3)
            cs.push_back({(int)p, d2});
    }
    std::sort(cs.begin(), cs.end(), [](const C &a, const C &b) { return a.d2 < b.d2 || (a.d2 == b.d2 && a.p < b.p); });
    const int n = std::min((int)cs.size(), kMaxPadsGpu);
    int nFlat = 0;
    for (int k = 0; k < n; ++k)
    {
        const LPad &pd = L.pads[cs[k].p];
        const glm::dvec3 c = toEnu(pd.g) * R - glm::dvec3(0.0, 0.0, R);
        cp.padQ[k] = glm::vec4(glm::vec3(c), (float)pd.exclR);
        cp.padU[k] = glm::vec4(glm::vec3(toEnu(pd.u)), (float)pd.kind);
        cp.padAux[k] = glm::vec4((float)pd.elevM, (float)pd.seed, (float)pd.flatR,
                                 pd.lz ? (float)(1.0 + pd.lzAng / glm::two_pi<double>()) : 0.0f);
        if (cs[k].d2 < 300e3 * 300e3)
            nFlat = k + 1;
    }
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    cp.padInfo = glm::vec4((float)n, 1.0f, (float)(1.0 - dayness), (float)nFlat);
}

// Each pad's lights (u, v, height m, kind: 0 flood, 1 red beacon (blinking), 2 landing-zone flood).
struct PadLight
{
    float u, v, h;
    int kind;
};
static void padLightsOf(const LPad &p, std::vector<PadLight> &out)
{
    out.clear();
    if (p.kind == 2)
    {
        for (int i = 0; i < 4; ++i)
            out.push_back({(i & 1) ? 105.0f : -105.0f, (i & 2) ? 105.0f : -105.0f, 45.0f, 0});
        for (int lv = 0; lv < 3; ++lv)
            for (int sd = 0; sd < 2; ++sd)
                out.push_back({sd ? 7.0f : -7.0f, (float)kPadTowerV + 6.5f, 40.0f + 40.0f * lv, 0});
        out.push_back({0.0f, (float)kPadTowerV, 153.0f, 1});
        out.push_back({0.0f, (float)kPadTowerV, 75.0f, 1});
        out.push_back({-230.0f, 120.0f, 28.0f, 0});
        out.push_back({-200.0f, 175.0f, 28.0f, 0});
    }
    else
    {
        const float tv = p.kind == 1 ? -25.0f : -22.0f, th = p.kind == 1 ? 80.0f : 70.0f;
        const float mast[3][2] = {{60.0f, 60.0f}, {60.0f, -60.0f}, {-60.0f, 55.0f}};
        for (const auto &m : mast)
        {
            out.push_back({m[0], m[1], 110.0f, 1});
            out.push_back({m[0], m[1], 35.0f, 0});
        }
        out.push_back({5.0f, tv + 6.0f, 0.4f * th, 0});
        out.push_back({-5.0f, tv + 6.0f, 0.75f * th, 0});
        out.push_back({0.0f, tv, th + 2.0f, 1});
        if (p.lz)
            for (int i = 0; i < 4; ++i)
            {
                const float a = 0.785398f + 1.570796f * i;
                const float cu = (float)(kLzDistM * std::cos(p.lzAng)), cv = (float)(kLzDistM * std::sin(p.lzAng));
                out.push_back({cu + 55.0f * std::cos(a), cv + 55.0f * std::sin(a), 15.0f, 2});
            }
    }
}

void SatelliteSim::launchFillPadLights()
{
    if (!launchPadGraphics || !launchState || !kFireMapped)
        return;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    const double night = 1.0 - dayness;
    if (night < 0.02 || L.pads.empty())
        return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const glm::dvec3 eyeE = eciToEcef(eye, t);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    std::vector<PadLight> lights;
    for (size_t pi = 0; pi < L.pads.size(); ++pi)
    {
        const LPad &pd = L.pads[pi];
        if (glm::length(pd.g * kR - eyeE) > 160e3)
            continue;
        padLightsOf(pd, lights);
        for (size_t li = 0; li < lights.size(); ++li)
        {
            const PadLight &pl = lights[li];
            const glm::dvec3 P = ecefToEci(padPointEcef(pd, pl.u, pl.v, pl.h), t);
            const glm::dvec3 rel = P - eye;
            const double range = std::max(glm::length(rel), 5.0);
            if (range > 150e3)
                continue;
            // Behind the Earth's curve (a pad below the horizon from the ground): none.
            {
                const glm::dvec3 u = rel / range;
                const double tc = -glm::dot(eye, u);
                if (tc > 0.0 && tc < range && glm::length(eye + tc * u) < kR + pd.elevM - 50.0)
                    continue;
            }
            // Magnitude at 1 km: a mast's flood cluster seen from the side ~-4, a medium-intensity red aviation
            // beacon ~-2 (flashing ~40 a minute), a landing zone's flood ~-3. (Physical floods are brighter still; the
            // flare is capped below the glare's huge spikes.)
            // Floods -3 (was -4: from a pad's edge they were a field of large stars, issue 20261019_222938 shot 1).
            double m1 = pl.kind == 0 ? -3.0 : pl.kind == 1 ? -2.0 : -3.0;
            double blink = 1.0;
            if (pl.kind == 1)
            {
                const double ph = std::fmod(t / 1.5 + h01(pd.seed, li, 3), 1.0);
                blink = ph < 0.3 ? 1.0 : 0.0;
                if (blink <= 0.0)
                    continue;
            }
            const double m = m1 + 5.0 * std::log10(range / 1000.0) +
                             atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
            // Capped at 14 (was 25): just past the glare threshold (effectFlare ~11.5), so a nearby flood glares faintly
            // and a mast's cluster reads as one soft glow (glare clusters, include/glare.glsl), not a sea urchin.
            const double flare = std::min(0.008 * std::pow(10.0, 0.4 * (6.0 - m)) * night * blink, 14.0);
            if (flare < 2e-4 || kFireCount >= kFireballMax)
                continue;
            const glm::vec3 col = pl.kind == 1 ? glm::vec3(1.0f, 0.12f, 0.06f)
                                : pl.kind == 2 ? glm::vec3(1.0f, 0.93f, 0.8f)
                                               : glm::vec3(0.96f, 0.97f, 1.0f);
            fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rel) / range), (float)flare), glm::vec4(col, (float)range)};
        }
    }
    static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;
}

nlohmann::json SatelliteSim::launchPadsJson() const
{
    nlohmann::json a = nlohmann::json::array();
    if (!launchState)
        return a;
    const LaunchState &L = *launchState;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 eyeE = eciToEcef(glm::dvec3(obsECI), t);
    for (const LPad &p : L.pads)
    {
        nlohmann::json o;
        o["site"] = L.cat.sites[p.site].id;
        o["name"] = p.name;
        o["kind"] = p.kind == 2 ? "starship" : p.kind == 1 ? "falcon" : "generic";
        o["ground_m"] = p.elevM;
        o["lat"] = glm::degrees(std::asin(glm::clamp(p.g.z, -1.0, 1.0)));
        o["lon"] = glm::degrees(std::atan2(p.g.y, p.g.x));
        o["dist_km"] = glm::length(p.g * kR - eyeE) / 1000.0;
        o["landing_zone"] = p.lz;
        a.push_back(o);
    }
    return a;
}

// The observer at a pad (harness `launch pad`): distM from its centre toward azDeg (clockwise from the pad's heading
// u), heightM above its ground, looking at the tower's middle.
bool SatelliteSim::launchGoToPad(const std::string &site, double distM, double azDeg, double heightM, std::string &msg)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    int pi = -1;
    for (size_t s = 0; s < L.cat.sites.size(); ++s)
        if (L.cat.sites[s].id == site && s < L.sitePad.size())
            pi = L.sitePad[s];
    if (pi < 0)
    {
        msg = "no site '" + site + "' (see `launch pads`)";
        return false;
    }
    const LPad &pd = L.pads[pi];
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double a = glm::radians(azDeg);
    const glm::dvec3 off = distM * (std::cos(a) * pd.u - std::sin(a) * pd.v);
    const glm::dvec3 g = glm::normalize(pd.g * kR + off);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = (float)(pd.elevM + heightM);
    const glm::dvec3 tgt = padPointEcef(pd, 0.0, pd.kind == 2 ? kPadTowerV * 0.5 : -10.0, pd.kind == 2 ? 60.0 : 35.0);
    const glm::dvec3 up = g;
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rel = tgt - (kR + pd.elevM + heightM) * g;
    const glm::dvec3 rn = glm::normalize(rel);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay);
    char b[200];
    snprintf(b, sizeof(b), "at %s (%s pad, ground %.0f m), %.0f m out", pd.name.c_str(),
             pd.kind == 2 ? "Starship" : pd.kind == 1 ? "Falcon" : "launch", pd.elevM, distM);
    msg = b;
    return true;
}

// The next catch (harness `launch landing`): which 0 a ship, 1 a booster. The clock leadS before the catch, 1x, the
// observer distM from the pad (to the side of the approach), 30 m up, looking at the catch point.
bool SatelliteSim::launchGoToLanding(int which, double leadS, double distM, std::string &msg, bool wantDay)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<MissionLaunch> ml;
    launchUpcoming(t0 - 4.0 * kDay, t0 + 30.0 * kDay, 0, 4000, ml);
    const LBatch *best = nullptr;
    double bestT = 1e300;
    for (const MissionLaunch &m : ml)
    {
        auto it = L.batches.find(m.key);
        if (it == L.batches.end() || !it->second.valid)
            continue;
        const LBatch &B = it->second;
        const launch::Vehicle &vh = L.cat.vehicles[B.veh];
        double tc = -1.0;
        if (which == 0 && B.shipLands && B.landPad >= 0)
            tc = B.tShipCatch;
        else if (which == 1 && vh.recovery == 1 && L.booster[B.veh].landed)
            tc = B.tL + L.booster[B.veh].tEnd;
        if (wantDay && tc > 0.0)
        {
            const int pi = which == 0 ? B.landPad : L.sitePad[B.site];
            if (pi < 0 || sunElevAtPad(L.cat.sites[L.pads[pi].site], tc) < 10.0)
                continue;
        }
        if (tc > t0 + leadS + 1.0 && tc < bestT)
        {
            bestT = tc;
            best = &B;
        }
    }
    if (!best)
    {
        msg = which == 0 ? "no ship catch in the next 30 days" : "no booster catch in the next 30 days";
        return false;
    }
    const LBatch &B = *best;
    const int pi = which == 0 ? B.landPad : L.sitePad[B.site];
    const LPad &pd = L.pads[pi];
    if (followActive)
        stopFollow();
    if (trackActive)
        stopTrack();
    const double tS = bestT - leadS;
    const double days = std::floor(tS / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tS - days * 86400.0;
    trailClearPending = true;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    // To the side of the approach (the ship's descent comes in along the landing frame's -dXF), else of the pad's v.
    glm::dvec3 side = pd.v;
    if (which == 0)
    {
        const glm::dvec3 ap = -(B.dXF.x * B.LE + B.dXF.y * B.LN);
        if (glm::length(ap) > 1.0)
            side = glm::normalize(glm::cross(pd.g, glm::normalize(ap)));
    }
    const glm::dvec3 g = glm::normalize(pd.g * kR + distM * side);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(g.z, -1.0, 1.0))), glm::degrees(std::atan2(g.y, g.x)));
    obsHeightOffset = (float)(pd.elevM + 30.0);
    const double catchUp = which == 0 ? kShipCatchAboveGround + 40.0 : L.cat.vehicles[B.veh].mountM + 60.0;
    const glm::dvec3 tgt = padPointEcef(pd, 0.0, 0.0, catchUp + (which == 0 ? 250.0 : 0.0));
    const glm::dvec3 up = g;
    const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
    const glm::dvec3 north = glm::cross(up, east);
    const glm::dvec3 rn = glm::normalize(tgt - (kR + pd.elevM + 30.0) * g);
    aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(rn, east), glm::dot(rn, north))),
                  (float)glm::degrees(std::asin(glm::clamp(glm::dot(rn, up), -1.0, 1.0))));
    camera.fovYDeg = 40.0f;
    updatePositions(tS);
    char b[300];
    if (which == 0)
        snprintf(b, sizeof(b),
                 "%s ship to the tower at %s: catch in %.0f s (descent from %.0f km, %.0f s; flip and landing burn %.0f s)",
                 L.cat.vehicles[B.veh].name.c_str(), pd.name.c_str(), leadS, B.dH0 / 1000.0, B.T1, B.T2);
    else
        snprintf(b, sizeof(b), "%s booster to the tower at %s: catch in %.0f s", L.cat.vehicles[B.veh].name.c_str(),
                 pd.name.c_str(), leadS);
    msg = b;
    return true;
}

// ── Mission milestones (2026-10-20, the user: "skip to the next mission milestone", "mission activity in the info
// screen") ─────────────────────────────────────────────────────────────────────────────────────────────────────────
// A launch's timeline from its batch's phase times: lift-off, max-Q and MECO / staging from the booster's track, the
// fairing, SECO, the deployment, the upper stage's deorbit burn and entry, the ship's descent and catch; the booster's
// own recovery burns are READ OFF its track (every change of its burn kind after staging, then its landing), so a new
// recovery kind (a droneship) shows up without code here. Trains: release, raising, arrival; retirees: deorbit,
// re-entry; a roster satellite in operations: the batch that put it there, and the one that will replace it.
// A subject's skips land `lead` before each milestone (10 s, less where milestones crowd) at 1x, and keep the
// selection and Go to on the subject's object at the new time (missionJump / missionAttachAfterJump).
namespace
{
std::string msFmtRel(double dt)
{
    char b[48];
    const char *pre = dt < 0.0 ? "T-" : "T+";
    const long long s = (long long)std::floor(std::abs(dt));
    if (s >= 86400)
        snprintf(b, sizeof(b), "%s%lldd %02lldh", pre, s / 86400, (s % 86400) / 3600);
    else if (s >= 3600)
        snprintf(b, sizeof(b), "%s%lldh %02lldm", pre, s / 3600, (s % 3600) / 60);
    else if (s >= 60)
        snprintf(b, sizeof(b), "%s%lldm %02llds", pre, s / 60, s % 60);
    else
        snprintf(b, sizeof(b), "%s%llds", pre, s);
    return b;
}
const char *msBurnName(uint8_t burn)
{
    switch (burn)
    {
    case launch::BURN_BOOSTBACK:
        return "Booster boostback burn";
    case launch::BURN_ENTRY:
        return "Booster entry burn";
    case launch::BURN_LANDING:
        return "Booster landing burn";
    case launch::BURN_ASCENT:
        return "Booster burn";
    case launch::BURN_UPPER:
        return "Booster burn";
    default:
        return "Booster burn"; // a kind added later (a droneship's): still a milestone
    }
}
} // namespace

LBatch *SatelliteSim::missionBatch(uint64_t key)
{
    if (!launchState)
        launchInitBlock();
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return nullptr;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale);
    auto it = L.batches.find(key);
    if (it == L.batches.end())
    {
        int plane = 0, b = 0;
        int64_t k = 0;
        batchKeyDecode(key, plane, k, b);
        if (plane < 0 || plane >= (int)L.planes.size())
            return nullptr;
        LBatch nb;
        nb.plane = plane;
        nb.k = k;
        nb.b = b;
        const LPlane &pl = L.planes[plane];
        LaunchImpl::buildBatch(*this, L, nb, pl, progLifeS(L.cat.programs[pl.prog], (double)launchLifeScale));
        it = L.batches.emplace(key, std::move(nb)).first;
    }
    it->second.touched = std::max(it->second.touched, L.frame);
    return &it->second;
}

// The batch's whole timeline (every object's milestones, `who` says whose), sorted.
void SatelliteSim::missionBatchMilestones(const LBatch &B, std::vector<MissionMilestone> &ms) const
{
    const SatelliteSim &sim = *this;
    const LaunchState &L = *launchState;
    using MM = SatelliteSim::MissionMilestone;
    const uint8_t P = SatelliteSim::kMsPayload, Bo = SatelliteSim::kMsBooster, Tr = SatelliteSim::kMsTrain,
                  Re = SatelliteSim::kMsRetiree;
    ms.clear();
    if (!B.valid || B.veh < 0)
        return;
    const launch::Vehicle &vh = L.cat.vehicles[B.veh];
    const launch::Program &pg = L.cat.programs[B.prog];
    auto add = [&](double t, uint8_t who, const std::string &label) { ms.push_back(MM{t, label, who, 10.0}); };
    add(B.tL, P | Bo | Tr, "Lift-off from " + L.cat.sites[B.site].name);
    // Max-Q: the largest dynamic pressure on the climb (the booster's track, ground-relative speed).
    const Track &tr = L.booster[B.veh];
    {
        double qMax = 0.0, tq = -1.0;
        for (size_t i = 0; i < tr.p.size() && (double)i * tr.dt < vh.t1; ++i)
        {
            const double q = 0.5 * launch::airDensity(tr.p[i].y) * glm::dot(tr.v[i], tr.v[i]);
            if (q > qMax)
            {
                qMax = q;
                tq = (double)i * tr.dt;
            }
        }
        if (tq > 5.0)
        {
            char b[64];
            snprintf(b, sizeof(b), "Max-Q (%.0f kPa)", qMax / 1000.0);
            add(B.tL + tq, P | Bo | Tr, b);
        }
    }
    add(B.tL + vh.t1, P | Bo | Tr, "MECO and stage separation");
    // The booster after staging: each change of its burn kind, then its landing (or its end).
    {
        uint8_t prev = launch::BURN_NONE;
        for (size_t i = 0; i < tr.burn.size(); ++i)
        {
            const double tau = (double)i * tr.dt;
            const uint8_t b = tr.burn[i];
            if (tau > vh.t1 + 0.5 && b != prev && b != launch::BURN_NONE)
                add(B.tL + tau, Bo, msBurnName(b));
            prev = b;
        }
        if (tr.tEnd > vh.t1 + 5.0)
        {
            const char *end = !tr.landed               ? (vh.recovery == 0 ? "Booster falls into the sea" : "Booster's track ends")
                              : vh.recovery == 1        ? (vh.catchM > 0.0 ? "Booster caught by the tower" : "Booster lands at the pad")
                              : vh.recovery == 2        ? "Booster lands on the droneship"
                                                        : "Booster touches down";
            add(B.tL + tr.tEnd, Bo, end);
        }
    }
    if (B.tFair >= 0.0)
        add(B.tFair, P | Tr, "Fairing separation");
    add(B.tL + B.t2, P | Tr, "SECO: in the parking orbit");
    const size_t n = B.mem.size();
    if (pg.crew)
        add(B.tD, P, pg.spacecraft.empty() ? "Spacecraft separation" : pg.spacecraft + " separates");
    else if (n > 0)
    {
        add(B.tD, P | Tr, "Deployment begins (" + std::to_string(n) + " satellites)");
        if (n > 1)
            add(B.tD + B.deployInterval * (double)(n - 1), P | Tr, "Last satellite released");
    }
    if (B.tDeorbBurn > B.tD)
        add(B.tDeorbBurn, P, "Upper stage deorbit burn");
    add(B.tStageGone, P, vh.upperDeorbit ? "Upper stage re-enters (burns up)" : "Ship entry interface (100 km)");
    if (!vh.upperDeorbit && B.shipLands)
    {
        add(B.tShipDesc, P, "Entry ends: belly-flop descent");
        add(B.tShipFlip, P, "Flip and landing burn");
        add(B.tShipCatch, P, B.landPad >= 0 ? "Ship caught by the tower" : "Ship splashes down");
    }
    if (pg.crew)
        add(B.tA, P, "Docking with " + sim.constellations[L.planes[B.plane].constIdx].name);
    else if (n > 0)
    {
        double r0 = 1e300;
        for (const LMember &m : B.mem)
            r0 = std::min(r0, m.sRaise);
        if (r0 < B.tA)
            add(r0, Tr, "Train starts raising");
        add(B.tA, Tr | Re, "Arrival in the slots; the old satellites retire");
        if (!B.ends.empty())
        {
            add(B.ends.front(), Re, "First retiree re-enters");
            if (B.ends.size() > 1)
                add(B.ends.back(), Re, "Last retiree re-enters");
        }
    }
    std::stable_sort(ms.begin(), ms.end(), [](const MM &a, const MM &b) { return a.t < b.t; });
}

// Fills s.ms from the batch for its role (and member), the current milestone and the leads.
void SatelliteSim::missionFinish(MissionSubject &s, double now)
{
    LaunchState &L = *launchState;
    LBatch *B = missionBatch(s.key);
    s.ms.clear();
    if (!B || !B->valid)
    {
        s.ok = false;
        return;
    }
    std::vector<MissionMilestone> all;
    missionBatchMilestones(*B, all);
    const bool member = s.member >= 0 && (size_t)s.member < B->mem.size();
    for (const MissionMilestone &m : all)
    {
        if (!(m.who & s.role))
            continue;
        if (member && s.role == kMsTrain && (m.label == "Train starts raising" || m.label == "Last satellite released"))
            continue; // the member's own below
        if (member && s.role == kMsRetiree && m.label.find("retiree re-enters") != std::string::npos)
            continue;
        s.ms.push_back(m);
    }
    if (member)
    {
        const LMember &mb = B->mem[(size_t)s.member];
        if (s.role == kMsTrain)
        {
            if (mb.tRel > B->tD + 0.5)
                s.ms.push_back({mb.tRel, "Released from the dispenser", kMsTrain, 10.0});
            if (mb.sRaise < B->tA)
                s.ms.push_back({mb.sRaise, "Starts raising to its slot", kMsTrain, 10.0});
        }
        else if (s.role == kMsRetiree)
        {
            for (MissionMilestone &m : s.ms)
                if (m.t == B->tA)
                    m.label = "Replacement arrives: retires, starts deorbiting";
            s.ms.push_back({mb.tEnd, "Re-enters", kMsRetiree, 10.0});
        }
    }
    // A roster satellite: how it got there (the batch before).
    if (s.roster && s.keyPrev)
        if (LBatch *Bp = missionBatch(s.keyPrev); Bp && Bp->valid)
        {
            s.ms.push_back({Bp->tL, "Launched (" + L.cat.vehicles[Bp->veh].name + ")", kMsTrain, 10.0});
            if (s.member >= 0 && (size_t)s.member < Bp->mem.size())
                s.ms.push_back({Bp->mem[(size_t)s.member].tRel, "Released from the dispenser", kMsTrain, 10.0});
            s.ms.push_back({Bp->tA, "Arrived in its slot", kMsTrain, 10.0});
        }
    std::stable_sort(s.ms.begin(), s.ms.end(), [](const MissionMilestone &a, const MissionMilestone &b) { return a.t < b.t; });
    // Leads: 10 s, less where two crowd (never past the previous one).
    for (size_t i = 0; i < s.ms.size(); ++i)
    {
        const double gap = i ? s.ms[i].t - s.ms[i - 1].t : 1e9;
        s.ms[i].lead = std::clamp(0.45 * gap, 0.5, 10.0);
    }
    s.cur = -1;
    for (size_t i = 0; i < s.ms.size(); ++i)
        if (s.ms[i].t <= now)
            s.cur = (int)i;
    s.t0 = s.ms.empty() ? 0.0 : s.ms.front().t;
    s.t1 = s.ms.empty() ? 0.0 : s.ms.back().t;
    s.ok = !s.ms.empty();
}

bool SatelliteSim::missionSubjectOfBatch(uint64_t key, MissionSubject &out, uint8_t role)
{
    out = MissionSubject{};
    if (!launchState)
        launchInitBlock();
    LBatch *B = missionBatch(key);
    if (!B || !B->valid)
        return false;
    LaunchState &L = *launchState;
    const launch::Program &pg = L.cat.programs[B->prog];
    const std::string &target = constellations[L.planes[B->plane].constIdx].name;
    out.key = key;
    out.role = role;
    out.title = pg.crew ? pg.spacecraft + " to " + target + " on " + L.cat.vehicles[B->veh].name
                        : L.cat.vehicles[B->veh].name + " - " + pg.name + ", " + std::to_string(B->mem.size()) +
                              " satellites to " + target;
    out.what = role == kMsBooster ? "the booster" : role == kMsTrain ? "the train" : role == kMsRetiree ? "the retirees"
                                                                                                       : "the rocket";
    missionFinish(out, (double)simDayJ2000 * 86400.0 + simSecInDay);
    return out.ok;
}

// The roster satellite at index `sat` (its plane's batches: the one that put it there, the one that replaces it).
bool SatelliteSim::missionRosterBatches(int sat, double t, uint64_t &keyPrev, uint64_t &keyNext, int &member)
{
    if (!launchState)
        return false;
    LaunchState &L = *launchState;
    LaunchImpl::ensureCatalog(L);
    if (!L.loaded)
        return false;
    if (!L.planesBuilt)
        LaunchImpl::buildPlanes(L, satOrbits, activeSatCount, constellations, launchLifeScale);
    if (L.satPlane.size() != activeSatCount)
    {
        L.satPlane.assign(activeSatCount, -1);
        L.satPos.assign(activeSatCount, 0);
        for (size_t pi = 0; pi < L.planes.size(); ++pi)
            for (size_t j = 0; j < L.planes[pi].sats.size(); ++j)
                if (L.planes[pi].sats[j] < activeSatCount)
                {
                    L.satPlane[L.planes[pi].sats[j]] = (int32_t)pi;
                    L.satPos[L.planes[pi].sats[j]] = (uint32_t)j;
                }
    }
    if (sat < 0 || (uint32_t)sat >= activeSatCount || L.satPlane[(size_t)sat] < 0)
        return false;
    const int pi = L.satPlane[(size_t)sat];
    const LPlane &pl = L.planes[(size_t)pi];
    const launch::Program &pg = L.cat.programs[pl.prog];
    if (pg.crew)
        return false;
    const int payload = LaunchImpl::payloadOf(*this, L, pl);
    const int pos = (int)L.satPos[(size_t)sat];
    const int b = pos / payload;
    member = pos % payload;
    const double life = progLifeS(pg, (double)launchLifeScale);
    const int64_t k = (int64_t)std::floor((t - b * pl.gap) / life - pl.phase);
    keyPrev = batchKey(pi, k, b);
    keyNext = batchKey(pi, k + 1, b);
    return true;
}

// Extra rows for the MISSION section: what comes next, a train member's climb, a satellite's design life.
void SatelliteSim::missionFacts(MissionSubject &s, int idx, double t)
{
    auto inStr = [](double dt) {
        std::string r = msFmtRel(dt).substr(2);
        return dt >= 0.0 ? "in " + r : r + " ago";
    };
    for (const MissionMilestone &m : s.ms)
        if (m.t > t)
        {
            s.facts.push_back({"Next", m.label + ", " + inStr(m.t - t)});
            break;
        }
    if (!launchState)
        return;
    const LaunchState &L = *launchState;
    LBatch *B = missionBatch(s.key);
    if ((uint32_t)idx >= activeSatCount && (uint32_t)idx < poolIdx0() && (uint32_t)idx != transientSatIdx() &&
        (uint32_t)idx != followSlotIdx())
    {
        const std::string st = launchStatus((uint32_t)idx - activeSatCount);
        if (!st.empty())
            s.facts.push_back({"Now", st.substr(st.find(" - ") == std::string::npos ? 0 : st.find(" - ") + 3)});
    }
    if (B && B->valid && s.member >= 0 && (size_t)s.member < B->mem.size() && !s.roster)
    {
        const LMember &mb = B->mem[(size_t)s.member];
        char b[120];
        if (s.role == kMsTrain && t >= mb.sRaise && t < B->tA)
        {
            snprintf(b, sizeof(b), "%.0f%% of the climb to %.0f km", 100.0 * (t - mb.sRaise) / std::max(1.0, B->tA - mb.sRaise),
                     (mb.rSlot - kR) / 1000.0);
            s.facts.push_back({"Raising", b});
        }
        if (s.role == kMsTrain && t < B->tA)
            s.facts.push_back({"Arrives", inStr(B->tA - t) + " (" + fmtUtc(B->tA) + ")"});
        if (s.role == kMsRetiree && t < mb.tEnd)
            s.facts.push_back({"Re-enters", inStr(mb.tEnd - t) + " (" + fmtUtc(mb.tEnd) + ")"});
    }
    if (s.roster)
    {
        if (LBatch *Bp = missionBatch(s.keyPrev); Bp && Bp->valid)
        {
            s.facts.push_back({"In its slot", "since " + fmtUtc(Bp->tA) + " (" + inStr(Bp->tA - t) + ")"});
            const launch::Program &pg = L.cat.programs[Bp->prog];
            const double life = progLifeS(pg, (double)launchLifeScale);
            char b[96];
            snprintf(b, sizeof(b), "%.1f years, %.0f%% used", life / kYear, 100.0 * std::clamp((t - Bp->tA) / life, 0.0, 9.99));
            s.facts.push_back({"Design life", b});
        }
        if (B && B->valid)
            s.facts.push_back({"Replaced", inStr(B->tA - t) + " (" + fmtUtc(B->tA) + ")"});
    }
}

bool SatelliteSim::missionSubjectOfIndex(int idx, MissionSubject &out)
{
    const bool ok = missionSubjectOfIndexRaw(idx, out);
    if (ok)
        missionFacts(out, idx, (double)simDayJ2000 * 86400.0 + simSecInDay);
    return ok;
}

bool SatelliteSim::missionSubjectOfIndexRaw(int idx, MissionSubject &out)
{
    out = MissionSubject{};
    if (!launchState || !launchesEnabled || idx < 0 || idx >= (int)satOrbits.size())
        return false;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const SelTransient *tr = nullptr;
    if ((uint32_t)idx == transientSatIdx() && selIsTransient())
        tr = &selTransientCur;
    else if ((uint32_t)idx == followSlotIdx() && followIsTransient() && followTrKey)
        tr = &followTrCur;
    if (tr)
    {
        if (tr->kind == 1 || tr->kind == 2 || tr->kind == 4)
        {
            LBatch *B = missionBatch(tr->a);
            if (!B || !B->valid)
                return false;
            const double t1 = launchState->cat.vehicles[B->veh].t1;
            const bool booster = tr->kind == 1 && tr->stage == 0 && t - B->tL >= t1;
            if (!missionSubjectOfBatch(tr->a, out, booster ? kMsBooster : kMsPayload))
                return false;
            out.what = booster ? "the booster" : tr->kind == 2 ? "the ship" : tr->kind == 4 ? "the upper stage's re-entry"
                                                : (tr->stage == 0 ? "the rocket" : "the upper stage");
            return true;
        }
        if (tr->kind == 3 && tr->a < activeSatCount)
        {
            // A retiree's re-entry: the batch that retired it (its replacement's, arrived before now).
            uint64_t kp = 0, kn = 0;
            int m = -1;
            if (!missionRosterBatches((int)tr->a, t, kp, kn, m) || !missionSubjectOfBatch(kp, out, kMsRetiree))
                return false;
            out.member = m;
            out.sat = (int)tr->a;
            out.what = "a retired satellite re-entering";
            missionFinish(out, t);
            return out.ok;
        }
        return false;
    }
    if ((uint32_t)idx >= activeSatCount && (uint32_t)idx < poolIdx0())
    {
        uint32_t m = 0;
        const uint64_t owner = launchSlotOwner((uint32_t)idx - activeSatCount, m);
        if (!owner)
            return false;
        const uint8_t role = owner % 2 == 0 ? kMsTrain : kMsRetiree;
        if (!missionSubjectOfBatch(owner / 2, out, role))
            return false;
        out.member = (int)m;
        if (LBatch *B = missionBatch(owner / 2); B && m < B->mem.size())
            out.sat = (int)B->mem[m].sat;
        out.what = role == kMsTrain ? "a satellite in the train" : "a retired satellite";
        missionFinish(out, t);
        return out.ok;
    }
    if ((uint32_t)idx < activeSatCount)
    {
        if (rosterFateAt((uint32_t)idx, t) != 0)
            return false; // dead or gone: the Kessler cascade owns it now
        uint64_t kp = 0, kn = 0;
        int m = -1;
        if (!missionRosterBatches(idx, t, kp, kn, m))
            return false;
        if (!missionSubjectOfBatch(kn, out, kMsRetiree))
        {
            // No replacement flies (launches stood down, ...): the past only.
            out = MissionSubject{};
            out.key = kn;
            out.role = kMsRetiree;
        }
        out.roster = true;
        out.keyPrev = kp;
        out.member = m;
        out.sat = idx;
        out.what = "a satellite in operations";
        const LaunchState &L = *launchState;
        if (LBatch *Bp = missionBatch(kp); Bp && Bp->valid)
        {
            const launch::Program &pg = L.cat.programs[Bp->prog];
            out.title = pg.name + ": launched on " + L.cat.vehicles[Bp->veh].name + " from " + L.cat.sites[Bp->site].name;
        }
        else
            out.title = "No launch on record for this slot";
        missionFinish(out, t);
        if (LBatch *Bn = missionBatch(kn); !Bn || !Bn->valid)
            out.facts.push_back({"Replacement", Bn ? "none scheduled (" + Bn->why + ")" : "none scheduled"});
        out.ok = !out.ms.empty();
        return out.ok;
    }
    return false;
}

bool SatelliteSim::missionSubjectNow(MissionSubject &out)
{
    if (followActive && followSatIndex >= 0 && missionSubjectOfIndex(followSatIndex, out))
    {
        out.followed = true;
        return true;
    }
    return selectedSatIndex >= 0 && missionSubjectOfIndex(selectedSatIndex, out);
}

// Puts the selection / Go to (whichever was on the subject) on the subject's object at time t.
void SatelliteSim::missionAttachAt(const MissionSubject &s, double t, bool sel, bool fol)
{
    msAttachOwner = 0;
    if (!sel && !fol)
        return;
    LBatch *B = missionBatch(s.key);
    if (!B || !B->valid)
        return;
    auto takeTransient = [&](const SelTransient &c) {
        if (fol && followActive)
            followTakeTransient(c, glm::dvec3(0.0), t); // the camera keeps its offset in the object's frame
        if (sel)
        {
            selTransientTake(c);
            selectedPlanetIndex = -1;
            selectedSatIndex = (int)transientSatIdx();
            formatSelectedSatInfo();
        }
    };
    auto takeIndex = [&](int idx) {
        if (fol && followActive)
        {
            followSatIndex = idx;
            followLaunchOwner = 0;
            followTrKey = 0;
            followWantKind = 0;
            snprintf(followLabel, sizeof(followLabel), "Following %s", satLabel(idx).c_str());
        }
        if (sel)
            selectSatellite(idx);
    };
    auto rocket = [&]() {
        SelTransient c;
        if (launchRocketTransient(s.key, t, c))
            takeTransient(c);
    };
    if (s.role == kMsPayload)
    {
        rocket();
        return;
    }
    if (s.role == kMsBooster)
    {
        const launch::Vehicle &vh = launchState->cat.vehicles[B->veh];
        SelTransient c;
        c.kind = 1;
        c.a = s.key;
        c.stage = 0;
        c.key = (s.key << 3) ^ 1ull;
        auto ti = rocketTypes.find("rocket_" + vh.id + (t - B->tL < vh.t1 ? "_stack" : "_booster"));
        c.type = ti != rocketTypes.end() ? ti->second : -1;
        if (transientExactState(c, t))
        {
            glm::dvec3 ax = LaunchImpl::stageNose(*launchState, *B, 0, c.tau);
            c.axis = glm::length(ax) > 0.5 ? ax : glm::normalize(c.pos);
            takeTransient(c);
        }
        return;
    }
    // A member (or a roster satellite: the retiree of s.key, the train member of s.keyPrev).
    if (s.member < 0)
        return;
    const LBatch *Bt = B; // the batch whose train it is at time t
    uint64_t trainKey = s.key;
    if (s.role == kMsRetiree)
    {
        if (t >= B->tA)
        {
            if ((size_t)s.member < B->mem.size() && t < B->mem[(size_t)s.member].tEnd)
            {
                msAttachOwner = s.key * 2 + 1;
                msAttachM = (uint16_t)s.member;
            }
            else
                return; // its re-entry: the fireball, from the list
        }
        else if (s.roster && s.keyPrev)
        {
            LBatch *Bp = missionBatch(s.keyPrev);
            if (Bp && Bp->valid && t < Bp->tA)
            {
                Bt = Bp;
                trainKey = s.keyPrev;
            }
            else
            {
                takeIndex(s.sat); // in operations
                return;
            }
        }
        else
        {
            takeIndex(s.sat);
            return;
        }
    }
    if (!msAttachOwner)
    {
        if (t >= Bt->tA)
        {
            takeIndex(s.sat >= 0 ? s.sat : (int)Bt->mem[(size_t)std::min<int>(s.member, (int)Bt->mem.size() - 1)].sat);
            return;
        }
        if (t < Bt->tD)
        {
            SelTransient c;
            if (launchRocketTransient(trainKey, t, c))
                takeTransient(c); // still on its rocket
            return;
        }
        msAttachOwner = trainKey * 2;
        msAttachM = (uint16_t)s.member;
    }
    // The record's slot is allocated by launchUpdate at the new time: attached right after it (this frame).
    msAttachSel = sel;
    msAttachFollow = fol && followActive;
    msAttachTries = 0;
    msAttachFallback = s.sat;
    if (msAttachFollow)
        followLaunchOwner = 0; // the old slot changes hands: not "the object is gone"
}

void SatelliteSim::missionAttachAfterJump()
{
    if (!msAttachOwner || !launchState)
        return;
    const LaunchState &L = *launchState;
    for (uint32_t s = 0; s < (uint32_t)L.slots.size(); ++s)
        if (L.slots[s].live && L.slots[s].owner == msAttachOwner && L.slots[s].m == msAttachM)
        {
            const int idx = (int)(activeSatCount + s);
            if (msAttachFollow && followActive)
            {
                followSatIndex = idx;
                followLaunchOwner = msAttachOwner;
                followLaunchM = msAttachM;
                followTrKey = 0;
                followWantKind = 0;
                snprintf(followLabel, sizeof(followLabel), "Following %s", launchLabel(s).c_str());
            }
            if (msAttachSel)
            {
                selectedSatIndex = idx;
                selectedPlanetIndex = -1;
                formatSelectedSatInfo();
            }
            msAttachOwner = 0;
            return;
        }
    // Not up yet (the staging is shared): a few frames, then the satellite's roster slot.
    if (++msAttachTries > 30)
    {
        msAttachOwner = 0;
        if (msAttachFallback >= 0)
        {
            if (msAttachFollow && followActive)
            {
                followSatIndex = msAttachFallback;
                followLaunchOwner = 0;
            }
            if (msAttachSel)
                selectSatellite(msAttachFallback);
        }
    }
}

bool SatelliteSim::missionJump(const MissionSubject &s, int dir, int index, std::string &msg)
{
    if (!s.ok || s.ms.empty())
    {
        msg = "No mission milestones here";
        return false;
    }
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    int pick = -1;
    if (index >= 0)
        pick = index < (int)s.ms.size() ? index : -1;
    else if (dir > 0)
    {
        for (size_t i = 0; i < s.ms.size() && pick < 0; ++i)
            if (s.ms[i].t - s.ms[i].lead > now + 0.5)
                pick = (int)i;
    }
    else
    {
        for (size_t i = s.ms.size(); i-- > 0 && pick < 0;)
            if (s.ms[i].t - s.ms[i].lead < now - 1.5)
                pick = (int)i;
    }
    if (pick < 0)
    {
        msg = dir > 0 ? "No later milestone" : "No earlier milestone";
        return false;
    }
    const MissionMilestone &m = s.ms[(size_t)pick];
    const double tNew = m.t - m.lead;
    // Who rides the subject: the selection (it or its slot) and Go to.
    const bool sel = selectedSatIndex >= 0 && [&]() {
        MissionSubject q;
        return missionSubjectOfIndex(selectedSatIndex, q) && q.key == s.key && q.role == s.role && q.member == s.member;
    }();
    const bool fol = followActive && followSatIndex >= 0 && [&]() {
        MissionSubject q;
        return missionSubjectOfIndex(followSatIndex, q) && q.key == s.key && q.role == s.role && q.member == s.member;
    }();
    const double days = std::floor(tNew / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tNew - days * 86400.0;
    timeScaleIdx = 0; // at 1x: the milestone happens in front of you
    timeDir = 1.0f;
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    missionAttachAt(s, tNew, sel, fol);
    updatePositions(tNew);
    char b[200];
    snprintf(b, sizeof(b), "%s in %.0f s (%s)", m.label.c_str(), m.lead, msFmtRel(m.t - s.ms.front().t).c_str());
    msg = b;
    infoMissionWall_ = followMissionWall_ = -1e300; // the readouts refresh now
    return true;
}

bool SatelliteSim::missionJumpNow(int dir, std::string &msg)
{
    MissionSubject s;
    if (!missionSubjectNow(s))
    {
        msg = followActive || selectedSatIndex >= 0 ? "This object has no mission timeline"
                                                    : "Select a rocket, a train or a satellite first";
        return false;
    }
    return missionJump(s, dir, -1, msg);
}

nlohmann::json SatelliteSim::missionSubjectJson(const MissionSubject &s) const
{
    nlohmann::json j;
    j["ok"] = s.ok;
    if (!s.ok && s.ms.empty())
        return j;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    j["title"] = s.title;
    j["what"] = s.what;
    j["role"] = s.role;
    j["member"] = s.member;
    j["sat"] = s.sat;
    j["roster"] = s.roster;
    j["followed"] = s.followed;
    j["current"] = s.cur;
    nlohmann::json ms = nlohmann::json::array();
    for (size_t i = 0; i < s.ms.size(); ++i)
        ms.push_back({{"i", i}, {"label", s.ms[i].label}, {"utc", fmtUtc(s.ms[i].t)}, {"in_s", s.ms[i].t - now},
                      {"lead_s", s.ms[i].lead}, {"who", s.ms[i].who}});
    j["milestones"] = ms;
    nlohmann::json f = nlohmann::json::array();
    for (const auto &kv : s.facts)
        f.push_back({kv.first, kv.second});
    j["facts"] = f;
    return j;
}
