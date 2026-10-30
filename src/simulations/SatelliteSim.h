#pragma once
#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE // see VulkanContext.h — must be repeated at every raw glfw3.h include site
#include "Cinematic.h"
#include <array>
#include <GLFW/glfw3.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../Simulation.h"
#include "../UIRenderer.h" // WindowChrome — used by member state below (needs complete type)
#include "../UiText.h"   // tx(Txt::...): the UI text table (src/text/ui_text_en.inc)
#include "SatModel.h"      // attitude groups, satellite models, baked lobes (lighting overhaul)
#include "SatPhotometry.h" // earthshine table axes (satTypeBuf layout)
#include "SatTrace.h"      // magnitude trace + CSV export (benchmarking M9)
#include "Refraction.h"   // refraction, mirages, turbulence (2026-10-26)
#include "SpaceWeather.h"  // aurora activity over sim time + the oval (2026-10-03)
#include "Kessler.h"       // Kessler syndrome: breakups, the debris pool, the cascade (2026-10-16)
#include "Weather.h"       // weather events: storm objects (2026-10-22, .plans/WEATHER_EVENTS_PLAN.md)
#include "Wildfire.h"   // (and Biome.h)         // the land-cover map for the weather hazards (2026-10-23)
#include "StormClimatology.h"
#include "SegVec.h"        // kEvents: a segmented vector (no reallocation hitches, 2026-10-21)
#include "SatBench.h"      // shared sample schema + bulk export (benchmarking M10)
#include "SatMeshRenderer.h"
#include "../VideoRecorder.h" // video export (SatelliteSimVideo.cpp, 2026-10-25)
#include "SatEnvProbes.h" // Phase 4: satellite meshes (model viewer; scene pass in 4c)
#include "SkyEvents.h"    // the Sky events window (2026-10-27)
#include "LunarBases.h"   // the lunar bases' solar-array glints (2026-10-27)
#include "Meteors.h"      // meteor showers and the sporadic background (2026-10-27)
#include "Comets.h"       // comets: orbits, brightness, tails (2026-10-27)
#include <filesystem>
#include <memory>

// Forward declaration only — savePerfSnapshot/buildPerfSnapshotJson are the sole users and both
// live in SatelliteSimUI.cpp, which includes the real header. Pulling all of nlohmann/json.hpp in
// here would put it in every TU that touches this simulation for two method signatures.
#include <nlohmann/json_fwd.hpp>

// Automation harness (docs/HARNESS.md). Only pointers/refs cross this header, so the full Harness.h
// (and nlohmann/json.hpp with it) stays out of every file that includes SatelliteSim.h.
class Ambience; // SatelliteSimAmbience.cpp / Ambience.h
namespace music
{
class Library; // MusicAnalysis.h: the soundtrack's key, for the ambience's tonality
}

namespace harness
{
class Runner;
struct Active;
enum class Status;
} // namespace harness

// The knockout table and settings tab names live in SatelliteSimUI.cpp; the harness reads them by
// name through these (docs/HARNESS.md: `knockout`, `ui open settings tab=...`).
int debugToggleTableSize();
bool debugToggleAt(int i, uint32_t &bit, const char *&label, const char *&jsonKey);
int settingsTabIndexByName(const std::string &name); // -1 if none (case-insensitive)
const char *settingsTabName(int i);
bool settingsTabIsAdvanced(int i);   // behind Display > "Show advanced settings"

#include <string>
#include <vector>
#include <deque>
#include <map>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <cstddef> // offsetof — GpuSatListHeader's layout asserts
#include <cmath>
#include <algorithm>
#include <functional>
#include <thread>
#include <future>
#include <atomic>
#include <mutex>
#include <chrono>

// ── Maximum satellites per frame ──────────────────────────────────────────────
static constexpr uint32_t MAX_SATELLITES = 10'000'000;

// ── Satellite attitude model ───────────────────────────────────────────────────
enum class AttitudeMode
{
    NadirPointing,     // flat face toward Earth (Starlink bus/antenna) — brief intense flares
    SunTracking,       // panel normal tracks sun for power — opposition flares
    Tumbling,          // uncontrolled random tumble — chaotic flashes (debris)
    Perpendicular,     // 90° to primary surface in the nadir plane — along orbital track
                       // (secondary only: normal = cross(surfN0, satNadir))
    AntiNadir,         // facing away from Earth center — deep-space-pointing radiator panels
                       // (secondary only: normal = -satNadir)
                       // Brightest to observers at the satellite's horizon; nearly invisible
                       // to observers directly beneath (at satellite's zenith), because the
                       // radiator face points away from them toward cold space.
    FlatMirror45,      // flat mirror oriented to reflect sunlight straight toward Earth center.
                       // Normal = normalize(sunDir + satNadir) — bisects incoming sun and
                       // outgoing nadir directions.  The reflected beam hits the ground below
                       // the satellite; angle between mirror and nadir ≈ 45° when sun is
                       // on the orbital horizon.  Models space-mirror illumination proposals
                       // (e.g. Reflect Orbital 55m mirrors).
    TargetedReflector, // as FlatMirror45 but aimed at a specific ground point on the terminator
                       // rather than straight down.  Target parametrized by SatOrbit::targetTerminatorAngle
                       // (radians along the terminator great-circle).  Each satellite's mirror
                       // normal is: normalize(sunDir + toTarget), so that reflected sunlight
                       // hits the chosen surface spot.  Multiple satellites sharing the same
                       // angle converge their beams onto one ground location — focused illumination.
    KnifeEdge,         // roll around the along-track (velocity) axis to put the sun edge-on
                       // to the flat panel, minimising its reflective cross-section.
                       // Picks whichever of the two edge-on orientations requires less roll
                       // from nadir, then clamps to ±kKnifeMaxRollDeg (solar panel gimbal limit).
                       // Models the SpaceX roll-angle adjustment adopted in 2020 (Mallama 2023):
                       // ~90% brightness reduction at standard distance when unclamped.
    SunPerp,           // normal = normalize(cross(sunDirECI, satNadir))
                       // Panel is edge-on to the sun AND edge-on to nadir; normal points
                       // perpendicular to the sun-nadir plane.  Used for thermal radiator panels
                       // on nadir-locked buses (e.g. AI1 datacenter): the bus yaws so solar
                       // panels face the sun, which constrains the hard-mounted radiators to
                       // this orientation.  irr = |dot(sun, normal)| = 0 always — the radiator
                       // intentionally never receives direct sunlight (correct thermal design).
                       // Visual contribution is through the overall diffuse scatter parameter.
    SunTrackingTilted, // flare mitigation: as SunTracking, but the sun-facing normal is pitched
                       // away from nadir toward zenith by the global flareMitigationTiltDeg
                       // setting, so the specular lobe (and the Earthshine reflect term in
                       // sat_flare.comp) point up and away from the ground instead of straight
                       // down at twilight. Pure rotation of sunDirECI within the plane it spans
                       // with zenith (-satNadir), so dot(result, sunDirECI) == cos(tiltDeg)
                       // exactly — that cosine IS the operator's real power loss vs. optimal
                       // sun-facing (Lambert's cosine law), used to compute the "power output"
                       // readout in the satellite selection panel. tiltDeg=0 is bit-identical to
                       // plain SunTracking. Used by the SpaceX AI Sats (datacenter) primary
                       // surface — see loadHardcoded()/constellations.json.
};

// ── Display unit system (right HUD panel altitude readout, settings Display tab) ──
enum class UnitSystem
{
    Metric,   // km
    Imperial, // mi
};

// NEW-7 (RELEASE_v1_1_PLAN.md) — Settings > Display "Frame limiter". Off/Cap30/Cap60/Cap120 all
// use VkPresentModeKHR MAILBOX/IMMEDIATE (uncapped submission) and rely on App::mainLoop's
// sleep-based pacing (see Simulation::targetFpsCap) for the numeric caps; VSync uses FIFO and
// needs no manual pacing at all. See VulkanContext::presentModePreference for the present-mode
// side of this.
enum class FpsCapMode
{
    Off, // uncapped, present mode IMMEDIATE (tearing allowed) — max perf regardless of comfort
    Cap30,
    Cap60,
    Cap120,
    VSync, // default — FIFO, paced by the display's own refresh rate
};

// UC1 (RELEASE_v1_1_PLAN.md) — Settings > Display graphics preset. Applying a named preset
// (anything but Custom) overwrites debugDisableMask, renderScale, and the "advanced" Clouds/
// Ocean/Terrain/Aurora sliders wholesale — see SatelliteSim::applyGraphicsPreset for the table.
// Custom is not user-selectable directly; it is set automatically the instant any of those
// advanced sliders is edited by hand (see buildCloudSliderRows), and simply means "trust
// whatever is currently loaded/set — don't overwrite it with a preset table."
enum class GraphicsPreset
{
    Planetarium, // v1.0 experience: flat textured Earth, stars, satellites, atmosphere. No clouds. The
                 // first-run seed for integrated GPUs (seedGraphicsPresetFromDevice).
    Low,         // Medium's effects (volumetric clouds, aurora, beams, fog, terrain detail) at 50% render scale
    Medium,      // mainstream discrete GPU — volumetric clouds/terrain at reduced budgets, 67% render scale
    High,        // today's tuned defaults
    Ultra,       // uncapped for showcase/screenshots
    Custom,      // user has hand-edited an advanced slider since the last named preset was applied
    Potato,      // below Planetarium — very weak GPUs / MoltenVK translation. Atmosphere scattering,
                 // the scene-depth pass, and the beam/sky-glow loops are ALL knocked out; 0.5 render
                 // scale. Appended after Custom on purpose: the persisted int index (saveSettings
                 // writes (int)graphicsPreset) of every pre-existing preset stays unchanged.
};

// ── Orbit distribution type ────────────────────────────────────────────────────
enum class OrbitDistribution
{
    Walker,      // regular Walker constellation: numPlanes planes × perPlane satellites
    RandomShell, // randomly distributed: random RAAN, random incl in [0, incl], jittered alt
    Disk,        // ring or concentric disk in a fixed orbital plane (incl + raan)
                 // Set alignTerminator=true to auto-derive the plane from sunDirECI
    Shells,      // a filed table of shell groups (ShellGroup), e.g. Starmind's FCC Table 1:
                 // each group = shells over an altitude range x planes x satellites per plane
    List,        // individual named members, each with its own elements and type (ConstMember):
                 // the real GEO belt (data/geo_satellites.json), 2026-10-20
};

// One member of a "List" constellation (2026-10-20, issue 20261020_190331 "GEO Satellites"): a real,
// named satellite with its own elements and type. A geostationary member is given by its sub-satellite
// longitude (geo): buildOrbits() puts it at the geostationary radius with a mean motion equal to the
// Earth's rotation rate and its phase solved so it sits over that longitude at every time (see there).
struct ConstMember
{
    std::string name;
    std::string operatorName; // "operator" in the file
    std::string cls;          // "class": the type key (large, medium, small, weather, spinner, ...)
    std::string purpose;      // comsat, weather, relay, navigation, ...
    uint32_t typeIdx = 0;
    bool geo = false;         // lonDeg is the sub-satellite longitude; altM / raan / u0 are derived
    float lonDeg = 0.0f;      // degrees east (geo)
    float altM = 0.0f;        // non-geo: altitude (m)
    float incl = 0.0f;        // radians
    float raan = 0.0f;        // radians (non-geo; geo: a hashed node, the inclination is tiny)
    float u0 = 0.0f;          // radians at J2000 (non-geo)
};

// One row of a "Shells" constellation: `shells` shells evenly spread over [altMin, altMax], each
// with `planes` planes of `perPlane` satellites. Non-SSO groups are Walker shells (RAAN evenly
// spread, each shell's planes staggered by 1/shells of a plane gap), the inclination running
// linearly from inclMin to inclMax across the shells. SSO groups take each shell's J2
// sun-synchronous inclination and put their planes about the dusk terminator node (LTAN 18:00):
// two planes = the 18:00 + 06:00 "X-ring".
struct ShellGroup
{
    float altMinM = 0.0f, altMaxM = 0.0f;
    float inclMin = 0.0f, inclMax = 0.0f; // radians; ignored when sso
    int shells = 1, planes = 1, perPlane = 1;
    bool sso = false;
};

// Data-driven attitude types (AttTarget, AttLaw, JointMode, AttitudeGroup, GpuAttGroup) live in
// SatModel.h — they are shared with satellite models (lighting overhaul Phase 2/3).

// ── One reflective surface of a satellite ─────────────────────────────────────
// A SatelliteType is composed of a primary surface plus an optional secondary
// surface (e.g. radiator panels perpendicular to solar panels) and an optional
// isotropic diffuse floor (structural body scatter).
// Orientation comes from EITHER the legacy `attitude` mode (group < 0) OR an explicit mount:
// `group` indexes SatelliteType::groups and `normal` is the body-frame surface normal.
// resolveAttitude() converts the legacy form into the explicit one.
struct SurfaceSpec
{
    AttitudeMode attitude; // legacy orientation mode — used only while group < 0
    float specExp;         // specular exponent (0 = Lambertian diffuse)
    float weight;          // contribution weight relative to primary (0 = disabled)
    int group = -1;        // index into SatelliteType::groups; -1 = not yet resolved (legacy)
    glm::vec3 normal{0.0f, 0.0f, 1.0f}; // body-frame normal within that group
};

// ── Per-type satellite parameters (CPU-side, drives GpuSatType) ───────────────
struct SatelliteType
{
    std::string name;
    glm::vec3 baseColor;   // visual tint
    float crossSectionM2;  // total reflective area (m²); brightness ∝ sqrt(area/10)
    SurfaceSpec primary;   // always active (solar panels, antenna face, etc.)
    SurfaceSpec secondary; // optional second surface — set weight=0 to disable
    float diffuse;         // isotropic Lambertian floor: always visible fraction [0,1]
                           // models structural body scatter; applied after litFactor
    float mirrorFrac;      // fraction of primary surface that is near-perfect mirror [0,1]
                           // adds ultra-narrow specular spike (MIRROR_BOOST×) on top of Phong lobe
                           // 0.0 = no mirror peak; 0.05 = Starlink; 0.15 = ISS solar panels
    std::vector<AttitudeGroup> groups = {}; // rigid groups; filled from JSON and/or resolveAttitude()
    // Phase 3 geometry model ("model": "<id>" in constellations.json). When set, `groups` comes
    // from the model, `lobes` holds its baked reflectance, and the legacy surface/cross-section
    // fields above are unused (baseColor stays the sprite tint).
    std::string modelId = {};
    std::vector<GpuSatLobe> lobes = {};
    int lobeBudget = 0; // the budget `lobes` was baked with (roster-size dependent; traces record it)
    // Phase 3b occlusion between parts: CPU form (the parity readout's evaluator) and GPU form
    // (packSatOcclusionGpu — `lobes` above already carry their sample ranges into it).
    SatOcclusion occlusion = {};
    GpuSatOcclusionPack occlusionGpu = {};
    // The loaded model itself (components, materials) — the mesh renderer draws it (Phase 4).
    std::shared_ptr<const SatModel> model = {};
    float meshRadiusM = 0.0f; // bounding radius of its triangles about their AABB centre (Phase 4d)
    float meshFill = 0.0f;    // mean projected area (surface / 4) over the bounding disc's (2026-10-07)
    float massKg = 0.0f;      // "mass_kg" (Kessler: collisions, drag); 0 = a default by model (kesslerTypeMass)
    // Kessler (round 2): what a breakup of this type leaves. Shares of its surface that are solar array /
    // radiator (the rest is structure), its mean projected area (surface / 4) and its bounding diameter.
    float debrisArrayShare = 0.0f, debrisRadiatorShare = 0.0f;
    float meanAreaM2 = 0.0f, sizeM = 0.0f;
    bool isModel() const { return !lobes.empty(); }
};

// ── Constellation descriptor ───────────────────────────────────────────────────
// One entry per shell. Multiple shells can share the same SatelliteType.
// orbitStart/Count are filled by initConstellation() — do not set manually.
// buildOrbits()' random numbers: the MSVC CRT rand() LCG (RAND_MAX 0x7FFF), one stream per
// constellation. Bit-identical to the rand() calls it replaced on Windows.
struct SatRng
{
    uint32_t s;
    static constexpr float kMax = 32767.0f;
    int next()
    {
        s = s * 214013u + 2531011u;
        return (int)((s >> 16) & 0x7FFFu);
    }
};

struct ConstellationConfig
{
    std::string name;
    float altM;       // orbital altitude above surface (meters)
    float incl;       // Walker: fixed inclination; RandomShell: max inclination (radians)
    int numPlanes;    // Walker: number of planes; other: total satellite count
    int perPlane;     // Walker: satellites per plane; other: ignored (use numPlanes as total)
    uint32_t typeIdx; // index into satTypes[]
    bool enabled;     // visibility toggle (hot-swappable)
    OrbitDistribution distribution = OrbitDistribution::Walker;
    float altJitterM = 0.0f;      // RandomShell: ±altitude jitter; Disk: ±per-satellite alt scatter
    float raan = 0.0f;            // Disk: orbital plane RAAN (radians); ignored if alignTerminator
    bool alignTerminator = false; // Disk: derive incl+raan from sunDirECI at init time
    int numRings = 1;             // Disk: number of concentric rings (1 = single ring)
    float ringSpacingM = 0.0f;    // Disk: altitude step between consecutive rings (meters)
    std::vector<ShellGroup> groups; // Shells: the filed groups (numPlanes = their total, perPlane = 1)
    // Formation clusters (Walker, Disk, Shells): each plane's/ring's satellites fly in clusters of
    // clusterSize, the clusters evenly spaced. 1 = no clusters. Line: clusterSpacingM apart along
    // track. Ring: a passive Clohessy-Wiltshire relative orbit about the cluster centre (see
    // SatOrbit::formP), members evenly spaced in its phase; clusterRadiusM = its along-track
    // semi-axis (rho), clusterCrossK = cross-track / radial amplitude (sqrt 3 = a circle of radius
    // rho tilted 60 deg out of the orbit plane, 2 = circular in projection on the local horizontal,
    // 0 = the in-plane 2:1 ellipse).
    int clusterSize = 1;
    float clusterSpacingM = 1000.0f;
    bool clusterRing = false;
    float clusterRadiusM = 200.0f;
    float clusterCrossK = 1.7320508f;
    // Shells only: per-cluster scatter (uniform +-, deterministic per cluster) so a managed
    // constellation does not draw perfect lattices: altitude (m), along-track phase (fraction of the
    // gap between neighbouring clusters), node and inclination (rad). 0 = none.
    float clusterAltJitterM = 0.0f;
    float clusterPhaseJitter = 0.0f;
    float clusterRaanJitterRad = 0.0f;
    float clusterInclJitterRad = 0.0f;
    // Shells, sun-synchronous groups: each plane's node is offset from its terminator node (LTAN
    // 18:00 / 06:00) by up to +-raanSpread (the filed RAAN tolerance; 15 deg = 1 h of LTAN). Even: a
    // golden-ratio sequence over the entry's SSO planes (no clumps); random: uniform. 0 = tight nodes.
    float raanSpreadRad = 0.0f;
    bool raanSpreadRandom = false;
    // List (2026-10-20): the members, one satellite each, in roster order (orbitStart + k = member k).
    std::vector<ConstMember> members;
    // Settings > Constellations groups the roster by this ("category" in constellations.json; empty =
    // inferred from the name and distribution, constCategoryOf()).
    std::string category;
    bool highlight = false;       // highlight mode: every above-horizon sat at least the census dot
    // Populated by initConstellation():
    uint32_t orbitStart = 0; // first index into satOrbits[]
    uint32_t orbitCount = 0; // number of orbits belonging to this constellation
    // Start state of this constellation's random stream in buildOrbits() (SatRng, an emulation of
    // MSVC's rand()). Recorded on the first build — the stream runs through the roster in order from
    // seed 1, exactly as the CRT rand() it replaced — so a rebuild after adding or removing another
    // constellation gives this one the same satellites. A constellation added later starts from a hash
    // of its name.
    uint32_t rngSeed = 0;
    bool rngSeeded = false;
    // Load-order index (set on the first build): the per-cluster jitter hash's key, so a rebuild after
    // removing an earlier entry leaves this one's clusters where they were.
    uint32_t defIndex = 0;
};

// ── GPU data structures ───────────────────────────────────────────────────────
// GpuSatInput (80 bytes/satellite, sat_orbit.comp → sat_flare.comp) was deleted in the lighting
// overhaul's Phase 1 (.plans/SAT_LIGHTING_PLAN.md): sat_orbit.comp now evaluates the reflectance
// model itself and writes a pre-photometry record straight into satVisibleBuf, which
// sat_flare.comp then finishes IN PLACE. That removed a 160-byte write+read round trip per
// satellite per frame, plus the buffer itself (it was allocated at MAX_SATELLITES — 800 MB).


// Per-SatelliteType reflectance + attitude parameters, indexed by GpuSatOrbit::typeIdx. These used
// to be copied into every satellite's GpuSatOrbit record even though they only vary per type — at
// millions of satellites that duplication was most of the orbit record's bandwidth. std430,
// 336 bytes. Must match SatType in sat_orbit.comp exactly. lobeCount > 0 selects the Phase 3
// geometry-model reflectance (lobes [firstLobe, firstLobe+lobeCount) of satLobeBuf); 0 keeps the
// legacy two-surface path.
struct GpuSatType
{
    float baseColorR;
    float baseColorG;
    float baseColorB;
    float crossSection; // sqrt(crossSectionM2 / 10)

    float specExp0;
    float specExp1;
    float w1;      // secondary surface weight
    float diffuse; // isotropic Lambertian floor

    float mirrorFrac;
    uint32_t groupCount; // 1..kMaxAttitudeGroups
    uint32_t firstLobe;  // model types: first entry in satLobeBuf
    uint32_t lobeCount;  // model types: lobe count; 0 = legacy two-surface model

    glm::vec3 surfNormalT0; // primary surface normal, triad coordinates of its group
    uint32_t surfGroup0;
    glm::vec3 surfNormalT1; // secondary surface normal, triad coordinates of its group
    uint32_t surfGroup1;

    GpuAttGroup groups[kMaxAttitudeGroups];

    // Phase 3b occlusion between parts (model types only; occluderCount 0 = never occluded).
    glm::vec4 originT[kMaxAttitudeGroups]; // each group's rest hinge point, root triad coords
    uint32_t firstOccluder;                // first entry in satOccluderBuf
    uint32_t occluderCount;
    float meshRadius; // Phase 4d: bounding radius of the model's mesh (m); 0 = no model
    float meshFill;   // mean projected area / bounding disc area (sat_flare.comp's resolved size; 0 = legacy)
};
static_assert(sizeof(GpuSatType) == 416, "GpuSatType layout mismatch");
static_assert(offsetof(GpuSatType, surfNormalT0) == 48, "GpuSatType std430 offset");
static_assert(offsetof(GpuSatType, groups) == 80, "GpuSatType std430 offset");
static_assert(offsetof(GpuSatType, originT) == 336, "GpuSatType std430 offset");
static_assert(offsetof(GpuSatType, firstOccluder) == 400, "GpuSatType std430 offset");

// Leading block of satTypeBuf (SatTypeBuf in sat_orbit.comp), followed by the GpuSatType array.
// Holds the two photometry sliders the reflectance model needs. Both used to be read by
// sat_flare.comp from SatFlarePC; they moved with the model, and SatOrbitPC is already at the
// 128-byte push-constant floor. Rewritten every frame in recordCompute() (host-coherent, single
// frame in flight — same pattern as lightDomeBuf).
struct GpuSatTypeHeader
{
    float brightnessScale; // global flux multiplier
    float mirrorBoost;     // mirror peak multiplier
    // Phase 3b: occlusion is skipped for a satellite whose UNOCCLUDED raw flux (effectFlare units,
    // brightnessScale applied) is below this — occlusion only dims, so it cannot become visible.
    float occlusionFluxFloor;
    uint32_t occlusionOn;  // 0 = knockout bit kDebugBitSatOcclusion set
    // Phase 4: deltaT's low half — (simTime − epochT0) − (float)(simTime − epochT0) — so
    // sat_orbit.comp's orbitPhase() gets the orbit phase to ~1 m (SatOrbitPC is full).
    float deltaTLo;
    // Phase 4d: radians per screen pixel (vertical) — sat_orbit.comp turns each model satellite's
    // bounding radius into its on-screen size (GpuSatVisible::meshPx). 0 = meshes off (knockout /
    // Potato), so no satellite becomes a mesh candidate and no sprite fades.
    float meshPixelAngle;
    float pixelAngle;     // radians per screen pixel, always (the formation merge's cluster size)
    float formationMerge; // 0..1: an unresolved formation drawn as one point (photometry.formation_merge)
    // Kessler (2026-10-16): satellites from poolBase on are the pool's Keplerian records (kesslerBuf).
    uint32_t poolBase;    // = activeSatCount
    uint32_t poolCount;   // records on the GPU
    int32_t scenNowSec;   // whole seconds since the scenario epoch (negative before it)
    uint32_t poolWord0;   // kesslerBuf word of the first record
    // Per-constellation Show / Highlight bits (2026-10-20): bit (c & 31) of word (c >> 5) for constellation c,
    // up to kMaxConstellationBits. They replaced SatOrbitPC's two 32-bit masks, which silently dropped every
    // constellation past the 32nd (its satellites read bit 0..31 of some other entry: constIdx & 31).
    uint32_t constEnabled[8];
    uint32_t constHighlight[8];
    // Stations built module by module (2026-10-21): today in days since J2000 (1e9 = every part: the deployment era
    // off). A lobe whose build-date code (GpuSatLobe::distribution bits 8-31) is later carries nothing.
    float buildDays = 1e9f;
    uint32_t padB0 = 0, padB1 = 0, padB2 = 0;
};
static_assert(sizeof(GpuSatTypeHeader) == 128, "GpuSatTypeHeader layout mismatch");
// The constellation index a satellite record carries is 8 bits wide (GpuSatOrbit::constIdx's low byte; Kessler
// pool / launch records' typeConst word), so 256 constellations is the ceiling.
static constexpr uint32_t kMaxConstellationBits = 256;

// A Kessler pool record as sat_orbit.comp reads it (16 words in kesslerBuf; see the shader's KesslerBuf).
struct GpuKepOrbit
{
    float M, argp, raan; // at the orbit epoch (sat_rebake.comp rewrites them)
    float n, a, e, cosI, sinI;
    float argpDot, raanDot, tumbleRate, tumblePhase;
    float flux;
    int32_t bornSec, deathSec;
    uint32_t typeConst; // type | constellation << 16 | flags << 24
};
static_assert(sizeof(GpuKepOrbit) == 64, "GpuKepOrbit must match sat_orbit.comp");

// The shared point-source model (shaders/include/point_style.glsl): apparent magnitude → point
// spread, for satellites, stars and planets alike. std140, rewritten every frame from the
// "Point ..." photometry sliders; bound as descSet binding 11 and starDescLayout binding 5.
struct GpuPointStyle
{
    float refMag;     // magnitude whose peak reaches 1 at the base PSF
    float gamma;      // display response: drawn flux ∝ flux^gamma
    float limitMag;   // faintest magnitude drawn
    float sigmaPx;    // base PSF sigma, pixels
    float sigmaMaxPx; // widest PSF sigma, pixels
    float satGainMag;  // satellites: exposure gain - adaptation (mag); sat_flare.comp writes DISPLAYED effectFlare
    float satPhysK;    // a model's mean pixel value pre-tonemap = satPhysK x raw flux / its area (px)
    float satResolved; // 0..1: a resolved satellite's sprite takes its model's surface brightness
};
static_assert(sizeof(GpuPointStyle) == 32, "GpuPointStyle layout mismatch");

// satTypeBuf's second block: the earthshine table (earthLut in sat_orbit.comp), written once by
// createSatBuffers() from satphot::earthshineLut(). The GpuSatType array follows it.
struct GpuEarthshineLut
{
    glm::vec2 v[satphot::kEarthLutLambda * satphot::kEarthLutCos]; // (ln irradiance, tilt rad)
    // The SH plane-irradiance fit (satphot::earthshineShLut, ratios to the irradiance): earthShA/B/C.
    glm::vec4 shA[satphot::kEarthLutLambda * satphot::kEarthLutCos]; // sh0..3
    glm::vec4 shB[satphot::kEarthLutLambda * satphot::kEarthLutCos]; // sh4..7
    glm::vec4 shC[satphot::kEarthLutLambda * satphot::kEarthLutCos]; // sh8..10
};
static constexpr size_t kSatTypeArrayOffset = sizeof(GpuSatTypeHeader) + sizeof(GpuEarthshineLut);
static_assert(kSatTypeArrayOffset % 16 == 0, "GpuSatType array must stay 16-byte aligned (std430)");

// Two-stage record in a COMPACT list (Phase 1b): sat_orbit.comp appends a PRE-photometry record
// only for satellites that survive its horizon/enable cull, sat_flare.comp finishes each one in
// place. Slot order is arbitrary (atomic append) — satVisibleIdxBuf maps slot → satellite index.
// Between the two dispatches the fields mean:
//   flareIntensity = raw flux, rangeM = range (m); angularSize -1 = highlighted constellation
//   (census mode: sat_flare.comp raises it to at least highlightFlare), else 0
// After sat_flare.comp every entry has the final meaning below (flareIntensity 0 = culled by
// visThresh; still in the list, discarded by the vertex shaders). Also the star and planet record.
// Phase 4: the tint is packed to RGBA8 (packUnorm4x8) so the record carries the range, which the
// point draws write as their depth in the unified encoding (shaders/include/depth.glsl).
struct GpuSatVisible
{
    glm::vec3 skyDir;     // unit vector in ENU (x=East, y=North, z=Up)
    float flareIntensity; // [0, 1+]
    uint32_t color;       // tint, packUnorm4x8 — packVisibleColor()
    float angularSize;    // point sprite size hint (pixels)
    float rangeM;         // distance, m; 0 = at infinity (stars, planets)
    float meshPx;         // pre-photometry only (sat_orbit → sat_flare): the model's on-screen
                          // diameter in pixels, 0 = no model (Phase 4d mesh candidates)
};
static_assert(sizeof(GpuSatVisible) == 32, "GpuSatVisible layout mismatch");

// GLSL packUnorm4x8(vec4(c, 1)): the tint as the point shaders unpack it.
inline uint32_t packVisibleColor(const glm::vec3 &c)
{
    auto q = [](float v) { return (uint32_t)std::lround(std::clamp(v, 0.0f, 1.0f) * 255.0f); };
    return q(c.r) | (q(c.g) << 8) | (q(c.b) << 16) | (255u << 24);
}

// satListBuf (Phase 1b): the compact visible list's header. Reset every frame with
// vkCmdUpdateBuffer(kSatListHeaderReset); sat_orbit.comp appends and maintains the indirect args
// via atomicMax; sat_flare.comp fills `selected` and the selected* raw fields. The whole header is
// copied back to the host every frame (pickedVisibleBuf) for selection tracking, the real visible
// count and the GPU-parity readout. Must match SatListBuf in sat_orbit.comp and sat_flare.comp.
struct GpuSatListHeader
{
    uint32_t count;                   // offset 0  — append counter == compact list length
    uint32_t dispatchX, dispatchY, dispatchZ;     // offset 4  — VkDispatchIndirectCommand (sat_flare)
    uint32_t drawVertexCount, drawInstanceCount;  // offset 16 — VkDrawIndirectCommand (point draws)
    uint32_t drawFirstVertex, drawFirstInstance;
    GpuSatVisible selected;           // offset 32 — the selected satellite's final record, or zeros
    // offset 64 — the selected satellite's PRE-photometry values as sat_orbit.comp produced them
    // (benchmarking M2, GPU parity): raw flux before sky/extinction/pollution (flare units incl.
    // brightnessScale) and range. selectedFound = 1 when the selection was
    // in this frame's compact list at all (0 = culled below the horizon / constellation disabled).
    float selectedRawFlux;
    float selectedRangeM;
    uint32_t selectedFound;
    uint32_t selectedPad;
    // offset 80 — Phase 4d: satellites big enough on screen to draw as meshes (sat_flare.comp
    // appends every satellite past the CPU's nomination size; the CPU picks the largest next frame and
    // draws them in double precision). meshCandCount is the atomic counter, so it can exceed the
    // list: the CPU then raises the nomination size until the list holds them all again.
    uint32_t meshCandCount;
    uint32_t meshCandPad[3];
    struct MeshCandidate
    {
        uint32_t sat;      // satellite index
        float meshPx;      // its mesh diameter on screen (GPU float; the CPU recomputes in double)
        float effectFlare; // its sprite's final DISPLAYED flux before the fade (bloom seed matching)
        uint32_t sizeKept; // packHalf2x16(its sprite's size (px) before the fade, log2 of the share of the unresolved
                           // point's displayed flux it kept: sat_flare.comp's resolved-object factor)
    } meshCand[1024];
};
// The GPU's candidate list, and how many of them the CPU draws as meshes. Until 2026-09-25 both were
// 64 and the list was filled in atomic-append (random) order: in the AI ring ~250 satellites are
// mesh-sized at once, so WHICH 64 got a mesh changed every frame and neighbours flickered between
// mesh and sprite. Now the list is complete (the nomination size adapts to keep it so), the CPU
// draws the largest kMaxMeshInstances, and the mesh fade-in size rises past the rest.
static constexpr int kMaxMeshCandidates = 1024;
static constexpr int kMaxMeshInstances = 256;

// Phase 4d: the satellites the CPU draws as meshes THIS frame and how much of each one's sprite
// remains (host-coherent, descSet binding 12, written in recordMeshScene before sat_flare runs), so
// the sprite fade and the mesh fade come from the same frame's geometry — no gap frame at the
// hand-off. sat_flare.comp only searches it for satellites already big enough to be meshes.
struct GpuMeshKeepList
{
    uint32_t count;
    float nominatePx; // sat_flare.comp lists satellites whose mesh spans more than this (px)
    uint32_t pad[2];
    // x = satellite, y = sprite keep, z = glare keep (float bits): the sprite's GLARE outlasts its point
    // and bloom (kGlarePointPx), so a mesh that is still point-like glares once, at its glint; w = that glint's
    // direction (review 28: octahedral, packSnorm2x16, observer ENU; 0 = the satellite's centre).
    glm::uvec4 entries[kMaxMeshInstances + 1];
};
static_assert(sizeof(GpuSatListHeader) == 96 + 16 * kMaxMeshCandidates, "GpuSatListHeader layout mismatch");
static_assert(offsetof(GpuSatListHeader, meshCandCount) == 80, "mesh candidate offset");
static_assert(offsetof(GpuSatListHeader, dispatchX) == 4, "dispatch args offset");
static_assert(offsetof(GpuSatListHeader, drawVertexCount) == 16, "draw args offset");
static_assert(offsetof(GpuSatListHeader, selected) == 32, "selected offset (std430 vec3 alignment)");
static_assert(offsetof(GpuSatListHeader, selectedRawFlux) == 64, "selected raw offset");

// Mercury..Uranus — the naked-eye-relevant classical planets plus Uranus (mag ~5.7-5.9, right at
// the edge of the star catalog's own mag-6.5 floor). Neptune excluded: never naked-eye (~mag 7.8).
enum PlanetId
{
    kMercury = 0,
    kVenus,
    kMars,
    kJupiter,
    kSaturn,
    kUranus,
    kPlanetCount
};
extern const char *const kPlanetNames[kPlanetCount];

// Per-planet ephemeris state, recomputed every frame in updatePositions() from the Keplerian
// elements in SatelliteSim.cpp (kPlanetElements/keplerEclipticPos) — see "Subsystem: Planets" in
// CLAUDE.md. Distinct from GpuSatVisible: this is the astronomy (direction/distance/phase), not
// the render-ready record (brightness/color/size), which updatePlanets() derives from it each
// frame into planetBuf.
struct PlanetState
{
    glm::vec3 eciDir{0, 1, 0};  // unit vector from Earth toward the planet, ECI
    float distanceAU = 0.0f;    // Earth-planet distance (AU)
    float sunDistAU = 0.0f;     // Sun-planet distance (AU)
    float phaseAngleDeg = 0.0f; // Sun-Planet-Earth angle (illumination phase)
    glm::dvec3 geoEci{0.0};     // Earth -> planet, AU (J2000 equatorial = the sim's ECI)
    glm::dvec3 helioEci{0.0};   // Sun -> planet, AU
};

// Jupiter's Galilean moons (2026-10-27): entries kPlanetCount.. of planetBuf (drawn as points by the star pipeline,
// selectable like a planet). Positions from Meeus ch. 44 (low accuracy), SatelliteSimPlanets.cpp.
static constexpr int kGalileanCount = 4;
static constexpr int kPlanetBodyCount = kPlanetCount + kGalileanCount;
const char *planetBodyName(int body);   // a planet's or a Galilean moon's name
glm::vec3 planetPointColor(int planet); // its true mean colour (linear, max 1): the point and the disc's mean (SatelliteSimPlanets.cpp)
float planetApparentMagnitude(PlanetId id, double rAU, double deltaAU, double alphaDeg, double sinRingB = 0.0);

// Resolved planet discs (2026-10-27): mirror of shaders/include/planet_disc.glsl (std140, vec4s only).
struct GpuPlanetDisc
{
    glm::vec4 c, bx, by, pole, x0, sun, tint, lin, pt, ring, misc, pad;
};
static_assert(sizeof(GpuPlanetDisc) == 192, "GpuPlanetDisc layout");
struct GpuPlanetDiscUBO
{
    glm::uvec4 head;
    glm::vec4 tone;
    glm::vec4 moons[4];
    GpuPlanetDisc disc[kPlanetCount];
};
static_assert(sizeof(GpuPlanetDiscUBO) == 96 + 192 * kPlanetCount, "GpuPlanetDiscUBO layout");

// The turbulence warp's UBO (2026-10-28): mirror of shaders/turb_warp.frag's TurbWarp block (std140, 16-byte members).
struct GpuTurbWarpUBO
{
    glm::vec4 camR, camU, camF, screen, params, mScale;
    glm::ivec4 anc[6];   // [phase k x 2 + level o], three boiling phases x two cell sizes
    glm::vec4 frac[6];
    glm::vec4 aniso;
    glm::vec4 tab[64];
};
static_assert(sizeof(GpuTurbWarpUBO) == 16 * (6 + 6 + 6 + 1 + 64), "GpuTurbWarpUBO layout");

// What the disc renderer and the selection panel know about each body this frame (SatelliteSimPlanets.cpp).
struct PlanetView
{
    float angRadiusArcsec = 0.0f; // equatorial angular radius
    float discPx = 0.0f;          // equatorial radius on screen, px
    float share = 0.0f;           // 0 point .. 1 resolved disc
    float ringTiltDeg = 0.0f;     // Saturn: the rings' opening toward the Earth
    float cmLonDeg = 0.0f;        // central-meridian longitude (Jupiter: System II, west; others east)
    float extinction = 1.0f;      // updatePlanets: the line of sight's transmission (V)
    float extMag = 0.0f;
    float supp = 0.0f;            // updatePlanets: drawn intensity / (raw x extinction): day, Moon, light pollution
    float sinRingB = 0.0f;        // Saturn: the rings' tilt toward the Earth (the magnitude's ring term)
    float handScale = 1.0f;       // the disc's light x this at the hand-off (1 = physical)
    bool above = false;           // updatePlanets: above the (apparent) horizon
};
struct GalileanState
{
    float x = 0.0f, y = 0.0f, z = 0.0f; // the disc's local frame (equatorial radii): x the view's right, y its up, z toward the Earth
    float xWest = 0.0f, yNorth = 0.0f;  // the same offset on the sky: celestial west / north
    float mag = 99.0f;
    bool transit = false, occulted = false, eclipsed = false, shadowOnJupiter = false;
};

// Compute push constants (must match sat_flare.comp push_constant block exactly).
// GLSL std430 layout, vec3 aligned to 16 bytes:
//   enuX/Y/Z (vec4): offsets 0,16,32
//   sunDirECI (vec3): offset 48  (48 is 16-aligned ✓)
//   satCount  (uint): offset 60
//   obsECI    (vec3): offset 64  (64 is 16-aligned ✓)
//   pad       (float): offset 76
//   total: 80 bytes
struct SatFlarePC
{
    glm::vec4 enuX;      // East  basis in ECI (w unused)
    glm::vec4 enuY;      // North basis in ECI (w unused)
    glm::vec4 enuZ;      // Up    basis in ECI (w unused)
    glm::vec3 sunDirECI; // unit vector toward sun in ECI
    uint32_t satCount;
    glm::vec3 obsECI; // observer ECI position (meters) for shadow test
    // pad3/pad4 were brightnessScale and mirrorBoost, and selectedSatIdx was elevCutoff: the
    // horizon cull and the reflectance model (their only readers) moved into sat_orbit.comp in the
    // lighting overhaul's Phase 1; brightnessScale/mirrorBoost now ride in GpuSatTypeHeader.
    uint32_t selectedSatIdx; // satellite to mirror into GpuSatListHeader::selected; UINT32_MAX = none
    float meshSatIdx;        // Phase 4c: satellite drawn as a mesh this frame (-1 = none; exact as float
                             // below 2^24) — its sprite is scaled by meshSpriteKeep (the hand-off)
    // Photometry tuning — runtime-adjustable via the settings window.
    float daySuppression;  // sky background suppression ratio (mirrors DAY_SUPPRESSION)
    float meshSpriteKeep;  // Phase 4c: 1 - mesh fade for meshSatIdx's sprite
    float visThresh;       // visibility cull threshold (mirrors VIS_THRESH)
    float highlightFlare;  // fixed flare for constellation census (mirrors HIGHLIGHT_FLARE)
    float extinctionCoeff; // atmospheric extinction, magnitudes per airmass (reuses the slot that
                           // was lightPollution — see SatelliteSim::updateLightPollutionDome for
                           // why that moved to the lightDomeBuf SSBO instead of push-constant space)
    float moonSuppression; // sky background suppression ratio from moonlight (mirrors daySuppression's
                           // role, much smaller in practice — moon is ~14 magnitudes dimmer than the sun)
    float pad0;            // reserved — pads moonDirECI to 16-byte (vec3) alignment
    glm::vec3 moonDirECI;  // unit vector from Earth toward Moon in ECI
    float sunRefIntensity; // was pad1 — S3 (RELEASE_v1_1_PLAN.md): soft ceiling reference so no
                           // satellite's effectFlare can render brighter than the sun; mirrors
                           // FlareSourcePC's own sunRefIntensity (SatelliteSim::sunFlareRefIntensity)
}; // total: 128 bytes
static_assert(sizeof(SatFlarePC) == 128, "SatFlarePC layout mismatch");

// Sky-background draw push constants (sat_sky.vert + sat_sky.frag / _lite / _minimal, via
// skyBgPipeLayout). Exactly 128 bytes — the Vulkan-guaranteed maxPushConstantsSize floor, which
// the oldest AMD integrated parts sit right at. Everything that used to trail past offset 128 here
// (debugDisableMask, screenSizePx, skyGlareVisibility, the four beam knobs, mwSuppressEased) was
// per-frame-uniform, so it moved into the CloudParams frame UBO (cloud_params.glsl / GpuCloudParams
// — sat_sky.frag already binds it) rather than a push constant. See the "Push-constant relief"
// block at the end of GpuCloudParams. The two per-draw flags that lived here (noTwinkle,
// manualTerrainTest) were only ever read by the point shaders, which now carry PointDrawPC below.
// GLSL std430 layout:
//   skyView    (mat4):  offset 0
//   fovYRad    (float): offset 64
//   aspect     (float): offset 68
//   gmst       (float): offset 72  — Greenwich Mean Sidereal Time (radians)
//   waveTime   (float): offset 76  — sim seconds; scales with time warp, pauses when paused
//   sunDirENU  (vec4):  offset 80   xyz=direction, w=sin(elevation)
//   moonDirENU (vec4):  offset 96   xyz=moon dir in ENU, w=illuminated fraction
//   obsECEFDir (vec4):  offset 112  xyz=observer ECEF unit vector, w=obsHeightOffset (m)
//   total: 128 bytes
struct SatDrawPC
{
    glm::mat4 skyView;         // ENU → camera space
    float fovYRad;             // vertical field of view (radians)
    float aspect;              // viewport width / height
    float gmst;                // Greenwich Mean Sidereal Time (radians)
    float waveTime;            // sim seconds for wave animation
    glm::vec4 sunDirENU;       // sun direction in ENU (xyz unit vec, w = sin(elevation))
    glm::vec4 moonDirENU;      // moon direction in ENU (xyz unit vec, w = illuminated fraction)
    glm::vec4 obsECEFDir;      // xyz = observer ECEF unit vector (lets sat_sky.frag convert ENU hit →
                               // ECEF → geographic lat/lon for texture UV); w = obsHeightOffset (m,
                               // user altitude offset above terrain — maxed with the GPU's own
                               // ground-height lookup as obsEffH). Despite the field's original "w
                               // unused" comment (stale — corrected here), it IS read.
}; // total: 128 bytes
static_assert(sizeof(SatDrawPC) == 128, "SatDrawPC layout mismatch");

// Point-sprite draw push constants — satellites (sat_point.vert/.frag via drawPipeLayout) AND
// stars/planets (star_point.vert/.frag via starPipeLayout). A separate, smaller struct from
// SatDrawPC so both point pipeline layouts declare a push-constant range that fits the 128-byte
// floor while still carrying the two per-draw flags (noTwinkle, manualTerrainTest) that genuinely
// cannot go in the frame UBO — they differ between the star draw and the planet draw, and between
// the live draw and the long-exposure trail draw, all within one frame. Each point shader declares
// only the prefix it reads (a push_constant block only has to be a contiguous PREFIX of the pushed
// bytes). GLSL std430 layout:
//   skyView    (mat4):  offset 0    — both verts
//   fovYRad    (float): offset 64   — both verts
//   aspect     (float): offset 68   — both verts
//   waveTime   (float): offset 72   — star_point.vert (twinkle phase)
//   noTwinkle  (float): offset 76   — star_point.vert; 1 on the planet draw only
//   moonDirENU (vec4):  offset 80   — star_point.vert (moon-disc star cull)
//   obsECEFDir (vec4):  offset 96   — star_point.vert (w = obsHeightOffset for atmFrac)
//   screenSizePx (vec2): offset 112 — sat_point.frag / star_point.frag (cloud/depth UV; always
//                                     ctx.swapExtent — point draws never render at renderScale<1)
//   debugDisableMask (uint): offset 120 — sat_point.frag (knockout bit 4096 only)
//   manualTerrainTest (float): offset 124 — both frags; 1 on the trail draw only
//   total: 128 bytes
struct PointDrawPC
{
    glm::mat4 skyView;
    float fovYRad;
    float aspect;
    float waveTime;
    float noTwinkle;
    glm::vec4 moonDirENU;
    glm::vec4 obsECEFDir;
    glm::vec2 screenSizePx;
    uint32_t debugDisableMask;
    float manualTerrainTest;
}; // total: 128 bytes
static_assert(sizeof(PointDrawPC) == 128, "PointDrawPC layout mismatch");

// ── Push constants for the rain particles (rain_particles.vert / .frag, include/rain_particles.glsl) ──
struct RainDrawPC
{
    glm::mat4 skyView;
    float fovYRad, dropLambda, screenW, screenH;   // dropLambda: the drops' Marshall-Palmer slope at the eye (1/mm)
    glm::vec4 eyeVel;      // xyz the eye's velocity (ENU, m/s), w the shutter (s)
    glm::vec4 params;      // x drops in level 0, y levels, z visibility gain, w the sky's exposure
    glm::vec4 obsECEFDir;  // xyz the observer's direction, w mode bits (1 diamond dust, 2 manual depth test)
};
static_assert(sizeof(RainDrawPC) == 128, "RainDrawPC layout mismatch");

// ── Lightning channels (SatelliteSimLightning.cpp, lightning.vert/.frag, 2026-10-05) ──
struct BoltDrawPC
{
    glm::mat4 skyView;
    float fovYRad, aspect, screenW, screenH;
    glm::vec4 params;    // x the sky's exposure, y "Lightning bolts" gain, z "Lightning glow" gain, w mode (0 channels, 1 glow)
    glm::vec4 params2;   // x highlight roll-off, yzw spare
    glm::vec4 spare;
};
static_assert(sizeof(BoltDrawPC) == 128, "BoltDrawPC layout mismatch");
struct GpuBoltSeg   // 32 B: a channel piece in its flash's local frame (t1, t2, up; metres from the origin)
{
    glm::vec3 p0;
    float w;            // brightness weight (order and position along its branch)
    glm::vec3 p1;
    uint32_t packed;    // bits 0-6 flash slot, 7-8 order, 9-20 arc / the tree's longest arc, 21-30 f along its branch
};
static_assert(sizeof(GpuBoltSeg) == 32, "GpuBoltSeg layout");
struct GpuBoltFlash   // 144 B, == BoltFlash in lightning.vert
{
    glm::vec4 o;      // xyz the origin (this frame's ENU about the eye, m), w intensity now
    glm::vec4 u;      // xyz up at the flash (ENU), w age (s)
    glm::vec4 t1;     // xyz the local frame's first tangent, w kind (0 in cloud, 1 to ground, 2 sprite, 3 spider under the base)
    glm::vec4 t2;     // xyz the second tangent, w the tree's longest arc (m)
    glm::vec4 m;      // x revealed arc (m), y the branches' share now, z 1 during the stepped leader, w spare
    glm::vec4 e[4];   // the cloud-glow emitters: xyz (ENU about the eye), w weight
};
static_assert(sizeof(GpuBoltFlash) == 144, "GpuBoltFlash layout");

// ── Push constants for cloud_march.comp (half-res cloud compute pass, C15-perf) ──────────────
// Matches the layout(push_constant) block in cloud_march.comp exactly. A separate struct from
// SatDrawPC (own pipeline layout, own push-constant range) — carries only the fields the moved
// cloudMarch/cirrusMarch bodies actually use, plus obsEffH (CPU-computed; the compute shader has
// no elevation-texture lookup of its own, see recordCompute()).
//
// Exactly 128 bytes — the 128-byte maxPushConstantsSize floor (see SatDrawPC). The five tail
// fields that used to sit past offset 128 (debugDisableMask, beamMaxRangeM, showBeamDebugRays,
// beamSkyGlowGain, cloudShadowRangeM) are all per-frame-uniform and moved into the CloudParams
// UBO this shader already binds — see the "Push-constant relief" block at the end of
// GpuCloudParams. cloud_march.comp reads them as cloud.dbgDisableMask / cloud.beamMaxRangeM /
// cloud.showBeamDebugRays / cloud.beamSkyGlowGain / cloud.cloudShadowRangeM.
struct CloudMarchPC
{
    glm::mat4 skyView;
    float fovYRad;
    float aspect;
    float waveTime;
    float obsEffH;
    glm::vec4 sunDirENU;
    glm::vec4 moonDirENU;
    glm::vec4 obsECEFDir;
}; // total: 128 bytes.
static_assert(sizeof(CloudMarchPC) == 128, "CloudMarchPC layout mismatch");

// ── Clouds v2 UBO (shaders/include/clouds_v2.glsl, CloudV2Params, std140) ─────────────────────
// .plans/CLOUDS_V2_PLAN.md. Every member is a vec4/mat4, so std140 and C++ agree by construction;
// the offsetof asserts below catch a reordering on this side. Keep the two in the same order.
struct GpuCloudV2Type
{
    glm::vec4 alt;   // x base (m), y top at full presence (m), z top fraction at zero presence, w base variation (m)
    glm::vec4 shape; // x extinction at density 1 (1/m), y erosion, z billow, w anvil
    glm::vec4 look;  // x multiple-scattering brightness, y ambient, z convective, w precipitation loading
};
static constexpr int kCloudV2Types = 5;
struct GpuCloudV2Params
{
    glm::mat4 prevSkyView;
    glm::vec4 prevObs;       // xyz previous observer ECEF unit vector, w previous tan(fovY/2)
    glm::vec4 obsDelta;      // xyz eye(now) - eye(prev), ECEF m; w previous aspect
    glm::vec4 anchorShape;   // xyz frac(anchor / period), w 1 / period
    glm::vec4 anchorDetail;
    glm::vec4 anchorCluster;
    glm::vec4 anchorCell;
    glm::vec4 frame;         // x frame index, y history valid, zw this frame's pixel in each 2x2
    glm::vec4 march;         // x base step, y growth per m, z max step, w max distance
    glm::vec4 light;         // x light length, y light steps, z MS extinction ratio, w MS contribution ratio
    glm::vec4 look;          // x coverage, y density, z detail, w ambient
    glm::vec4 look2;         // x ground bounce, y sun gain, z powder, w new-sample weight
    glm::vec4 phase;         // x/y forward g/weight, z/w backward g/weight
    glm::vec4 shell;         // x lowest base, y highest top, z debug view, w detail LOD start (m)
    glm::vec4 extra;         // x edge sharpness, y weather-lookup warp (rad)
    GpuCloudV2Type types[kCloudV2Types];
    glm::vec4 cover;         // x map value = clear, y map value = overcast, zw cos/sin of the map's drift
    glm::vec4 form;          // x wobble, y lean (m/m), z interior erosion kept, w column-edge softness
    glm::vec4 misc;          // x march budget (iterations), y moon key-light gain, z full rate (0/1), w mid-layer amount
    glm::vec4 storm;         // x storm feature scale, y storm erosion kept, z anvil amount, w deck base roughness
    glm::vec4 anchorStorm;   // the shape volume at period x storm scale (deep convection's lobes)
    glm::vec4 anchorStormDetail; // the detail volume at period x storm scale
    glm::vec4 motion;        // x the noise volumes' relative slide since last frame (m)
    glm::vec4 high;          // x amount, y 1/along-wind period, z along-wind offset (periods), w 1/across period
    glm::vec4 high2;         // x high-layer density
    glm::vec4 rain;          // x rain amount, y optics strength, z rain streaks at the eye
    glm::vec4 beam;          // x beam shaft gain (0 = the old drawn line), y beam haze, z beam light on
                             // cloud, w 1 / (1361 x beamGain): a beam's intensity -> reflecting area (m^2)
    glm::vec4 anchorMid;     // the shape volume at the mid layer's own period
    glm::vec4 atmo;          // x cloud sunlight Rayleigh gain, y twilight sky light, z mid-layer density, w beam lines
    glm::vec4 anchorFlow;    // the mesoscale volume at the flow period (the large swirls that bend the noise)
    glm::vec4 flow;          // x flow warp (m of displacement per unit of the normalised field), y layer spread
    glm::vec4 anchorCol;     // the Cb towers' lobe noise: xyz the storm shape anchor without wind, w their flow's phase
    glm::vec4 column;        // x Cb column amount, y lattice cell (m), z base radius (m), w storm cumulus top (m)
    glm::vec4 column2;       // x waist, y head flare (x base radius), z head drift (m), w lobe strength
    glm::vec4 anvil2;        // x anvil thickness (m), y anvil hang (m), z tower sparsity, w overshoot above the lid (m)
    glm::vec4 column3;       // x weather mip for the storm cumulus reach, y light LOD footprint (m), z cumulus
                             // top variation, w Cb fill
    glm::vec4 lightning;     // x flashes / min / tower, y cloud glow gain, z bolt gain, w sim time (s, wrapped)
    glm::vec4 lightVol;      // x first level baked this frame, y half-extent (m), z top (m), w godrays
    glm::vec4 misc2;         // x red-sprite chance per ground stroke, y sprite brightness, z fast motion (0/1), w adaptive parallax (px)
    glm::vec4 fog;           // x fog amount, y fog depth (m), z fog extinction (1/m), w dust amount
    glm::vec4 fog2;          // x dust scale height (m), y dust extinction (1/m), zw free
    glm::vec4 sunE;          // xyz the Sun, Earth-fixed
    // Rain / snow at the eye (review 3): a world-fixed drop lattice. rainE/N/U xyz = the axes of a local
    // frame fixed to a 0.25-degree reference point (ECEF), w = the eye's position in it (m, wrapped to
    // 1024, from double); precip x = the air temperature at the eye (deg C), y sim time mod 600 s, zw free.
    glm::vec4 rainE, rainN, rainU;
    glm::vec4 precip;
    // Review 17: the cloud MORPHOLOGY texture (tools/make_cloud_morph.py, march binding 20): anchorMorph as the
    // other anchors (xyz frac(anchor / period), w 1 / period); morph x its share of the far field (orbit), y its
    // weight in the near field's clustering.
    glm::vec4 anchorMorph;
    glm::vec4 morph;
    glm::vec4 farLight;    // review 18: the far cloud layer's key-light (x) and sky-light (y) gains
    glm::vec4 farTune;     // review 24: the far cloud layer's x slant coverage, y coverage bias, z density, w edge softness
    glm::vec4 farTune2;    // review 24: x the far layer's low-Sun light; yzw the mid layer's morphology anchor (frac(anchor x 1.7))
    // 2026-10-04: the low shape volume's rotated frame (clouds_v2.glsl: columns) and its anchors.
    glm::vec4 shapeRotA[3];
    glm::vec4 anchorShapeA, anchorStormA;
    // 2026-10-05: the rain particles' motion, integrated over sim time on the CPU (SatelliteSimRain.cpp).
    glm::vec4 rainMotion;  // x fall phase (integral of the reference fall speed / 64, m, mod 1024), y reference speed now,
                           // zw the rain's wind drift (rain frame E/N, m, mod 1024)
    glm::vec4 rainWind;    // xy the wind now (m/s, rain frame), zw the snow's wind drift (m, mod 1024)
    glm::vec4 lightning2;  // 2026-10-05: x "Storm lightning (flashes/min/cell)", yzw spare
    // 2026-10-06: the Reflect beams' light at the eye, for the rain / snow particles (rain_particles.vert; CPU, from the
    // cloud-light list in SatelliteSim.cpp). beamEye x the beams' irradiance there (Suns) x "Beam light on cloud",
    // y 1 - dayness; beamEyeDir xyz the irradiance-weighted direction to the satellites (ENU at the eye).
    glm::vec4 beamEye, beamEyeDir;
    // 2026-10-06: the snow's own fall (SatelliteSimRain.cpp): x its phase (as rainMotion.x), y its reference speed now.
    // Flakes took "Rain fall speed" (x5): at 4-7 m/s they streaked like rain. zw spare.
    glm::vec4 snowMotion;
    glm::vec4 tune, mid2, mid3, samp;   // 2026-10-11 (clouds_v2.glsl)
    glm::vec4 cu, cu2;                  // 2026-10-13: the cumulus shape (clouds_v2.glsl)
    glm::vec4 layA, layE, layN;         // the clipmap plane's anchor axes (clouds_v2.glsl)
    glm::vec4 cdcA, cdcB, cdcP0, cdcP1; // 2026-10-21: the cloud density clipmap (EXPERIMENT, clouds_v2.glsl / cv2_cdc.glsl)
    glm::vec4 mie;                      // 2026-10-22: the Mie phase table's share and row (clouds_v2.glsl)
    glm::vec4 frox;                     // 2026-10-22: the god rays' froxel volume (clouds_v2.glsl)
    glm::vec4 cdcC;                     // 2026-10-22: S2, the light march from the clipmap (clouds_v2.glsl)
    glm::vec4 grid;                     // 2026-10-22: S9, the cloud grid and full-res mode (clouds_v2.glsl)
    glm::vec4 anvil3;                   // 2026-10-22: anvil puffiness, the head top's lift (clouds_v2.glsl)
    glm::vec4 stvL0, stvL1, stvL2, stvL3, stvH, stvV;   // 2026-10-22: S3, the Sun transmittance volume (clouds_v2.glsl)
    glm::vec4 tsh0, tsh1, tsh2, tsh3;   // 2026-10-22: the terrain Sun-horizon map (clouds_v2.glsl)
    glm::vec4 perf;                     // 2026-10-24: x "Near cirrus stride" (1 = off), y its footprint (m), zw spare
    glm::vec4 skyAp;                    // 2026-10-25: the aerial-perspective volume: x ran this frame, y d0 (m), z band (sin)
    glm::vec4 ms;                       // 2026-10-26: multiple scattering: x model (0 octaves, 1 delta-Eddington), y g, z gain, w the reach away from the light (m)
    glm::vec4 amb;                      // 2026-10-26: the sky's light on the clouds: x model, y gain, z ground albedo, w the sky pass's multiple scattering
    glm::vec4 tone;                     // 2026-10-26: the sky pass's tonemap: x AgX on, y saturation, z contrast
    glm::vec4 gnd;                      // 2026-10-26: x the ground's light under cloud (the cv2Vt map) on
};
static_assert(offsetof(GpuCloudV2Params, prevObs) == 64, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, frame) == 160, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, shell) == 256, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, types) == 288, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, cover) == 288 + 48 * kCloudV2Types, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, anchorMid) == 288 + 48 * kCloudV2Types + 176, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, rainE) == 288 + 48 * kCloudV2Types + 416, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, anchorMorph) == 288 + 48 * kCloudV2Types + 480, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, farLight) == 288 + 48 * kCloudV2Types + 512, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, farTune) == 288 + 48 * kCloudV2Types + 528, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, shapeRotA) == 288 + 48 * kCloudV2Types + 560, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, rainMotion) == 288 + 48 * kCloudV2Types + 640, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, beamEye) == 288 + 48 * kCloudV2Types + 688, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, tune) == 288 + 48 * kCloudV2Types + 736, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, cu) == 288 + 48 * kCloudV2Types + 800, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, layA) == 288 + 48 * kCloudV2Types + 832, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, cdcA) == 288 + 48 * kCloudV2Types + 880, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, mie) == 288 + 48 * kCloudV2Types + 944, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, frox) == 288 + 48 * kCloudV2Types + 960, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, cdcC) == 288 + 48 * kCloudV2Types + 976, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, grid) == 288 + 48 * kCloudV2Types + 992, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, anvil3) == 288 + 48 * kCloudV2Types + 1008, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, stvL0) == 288 + 48 * kCloudV2Types + 1024, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, tsh0) == 288 + 48 * kCloudV2Types + 1120, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, perf) == 288 + 48 * kCloudV2Types + 1184, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, skyAp) == 288 + 48 * kCloudV2Types + 1200, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, ms) == 288 + 48 * kCloudV2Types + 1216, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, amb) == 288 + 48 * kCloudV2Types + 1232, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, tone) == 288 + 48 * kCloudV2Types + 1248, "GpuCloudV2Params layout");
static_assert(offsetof(GpuCloudV2Params, gnd) == 288 + 48 * kCloudV2Types + 1264, "GpuCloudV2Params layout");
static_assert(sizeof(GpuCloudV2Params) == 288 + 48 * kCloudV2Types + 1280, "GpuCloudV2Params layout");

// ── Reflect-Orbital beam->cloud light sources (host-visible) ─────────────────────────────────
// 2026-08-09, fourth design for this feature. First was a per-target CPU aggregation anchored at
// the idealized targetENU (retired same day once beam_self_march.comp made per-beam values real).
// Second was a true per-beam screen-space glow folded into cloud_march.comp's per-pixel debug-ray
// loop (distance from the CAMERA's own view ray to the beam's line) — cheap, but WRONG: that
// distance's iso-contours are not soft blobs, they're hyperbola-like curves, which read in-app as
// large white rings intersecting the beams. Third was a true per-beam list fed forward into the
// per-sample volumetric march (the correct architecture, kept since) — but at a small cap (16) the
// cloud effect was barely visible, and getting "appreciable" coverage needed raising the cap to
// 512, which tanked frame rate (the per-sample loop's cost is linear in the cap).
//
// This fourth version keeps the third's per-sample architecture and its two geometric fixes
// (`posENU` is the beam's REAL ground intersection — `satENU + reflectDirENU` traced to `R_EARTH`,
// computed on the CPU below via the same rotation-invariant local-frame `raySphere` trick the GPU
// side uses, no ECEF conversion needed; `dirToSource` is the beam's REAL direction, ground toward
// satellite), but adds clustering: a beam LOCKED onto its target (`aimErrorRad` below a threshold
// — the same convergence signal `cloud_march.comp`'s own `aimFade` already uses) is folded into a
// shared per-target cluster via intensity-weighted running average (same technique already used
// for the blockAltM/blockOpacity flicker fix); a beam still SLEWING keeps its own individual slot,
// since its geometry is genuinely unique right now. Per explicit user direction: converged beams
// at a busy site are redundant with each other and should combine; transiting beams shouldn't.
// This recovers full coverage — the effective distinct-light count is bounded near the active-
// target count (times however many DISTINCT approach directions are simultaneously converged on
// one target — see the direction-similarity gate on the cluster match, added same day: merging by
// ground position alone let satellites arriving from very different parts of the sky get blended
// into one nonsensical averaged direction) — not total active-satellite count. In-app testing
// after the clustering fix showed this cap is no longer the cost driver it was as a flat
// (non-clustered) list, so it was raised back to 512 for headroom.
//
// 2026-08-12 — this struct is UNCHANGED, but it is no longer built from scratch each frame. It is
// now the per-frame EMIT of the persistent TrackedBeamLight pools below: same clustering intent,
// same two reserved budgets, same ordering (clusters then individuals), but each entry has a stable
// cross-frame identity and its values are temporally eased rather than snapped. The parts of the
// history above that describe the intra-frame epsilon match and the running-average direction gate
// are retained as background — that mechanism is gone. See TrackedBeamLight for what replaced it
// and why.
//
// Struct must match BeamCloudLight/BeamCloudLightBuf in cloud_march.comp exactly — including
// kMaxCloudBeamLights itself, hand-duplicated there (not shared via a header); a mismatch
// silently over/under-reads the buffer with no compile or validation error.
static constexpr int kMaxCloudBeamLights = 512;
// 2026-08-09 (in-app finding: "I do not see cloud lighting on singular beams anymore" after
// clustering became stable): a converged cluster's intensity is the SUM of every beam folded into
// it, so a busy target with dozens of locked satellites is legitimately dozens of times brighter
// than any one transiting beam's own single intensity — that part is physically correct, not a
// bug. But the CPU build below used one shared top-K-by-intensity eviction pool for both
// categories: once total distinct entries (clusters + individuals) reached kMaxCloudBeamLights,
// eviction always removes the globally weakest entry, and a lone transiting beam is essentially
// always weaker than an established multi-satellite cluster — so under any capacity pressure,
// individual beams are evicted first, potentially entirely, while clusters are never at risk.
// Split the budget into two reserved, independently-evicted pools so transiting beams keep a
// guaranteed floor of visibility regardless of how many targets are simultaneously busy: clusters
// are bounded near the real target count (kNumReflectorTargets capacity is 201, so 256 leaves
// headroom), individuals get the rest.
static constexpr int kMaxClusterCloudLights = 256;
static constexpr int kMaxIndividualCloudLights = kMaxCloudBeamLights - kMaxClusterCloudLights;
struct GpuBeamCloudLight
{
    glm::vec3 posENU;      // meters, observer-relative — REAL ground intersection of this beam
    float intensity;       // groundIrradiance * beamGain for this ONE satellite
    glm::vec3 dirToSource; // unit direction from posENU toward the satellite (real beam direction)
    float footprintRadM;
    float blockAltM;    // altitude (m) where THIS beam's own path first drops below 50% transmittance
    float blockOpacity; // 0 = clear column, 1 = fully opaque
    float pad0, pad1;   // std430 array-of-vec4-pairs alignment
};
static_assert(sizeof(GpuBeamCloudLight) == 48, "GpuBeamCloudLight layout mismatch");

struct GpuBeamCloudLights
{
    uint32_t count;
    uint32_t pad0, pad1, pad2;
    GpuBeamCloudLight entries[kMaxCloudBeamLights];
};
static_assert(sizeof(GpuBeamCloudLights) == 16 + kMaxCloudBeamLights * 48, "GpuBeamCloudLights layout mismatch");

// ── TrackedBeamLight: persistent, cross-frame cloud-light identity (2026-08-12) ────────────────
// CPU-only — this is the state the GPU-facing GpuBeamCloudLights above is DERIVED from each frame,
// not a mirror of it.
//
// Why this exists. Until now the light list was rebuilt from scratch every frame, and a cluster's
// identity was EMERGENT: it was seeded by whichever beam the scan happened to reach first at a
// target, and admitted members by comparing them against a running partial average that changed as
// members were added. That partition is discontinuous in its own inputs — when the seed satellite
// drops out (a lock-window reassignment, or setting below minBeamElevSin) the survivors repartition
// from scratch, so cluster count, every direction and every summed intensity can all change in one
// frame. Since posENU/dirToSource/blockAltM/blockOpacity are all intensity-weighted means, and the
// cloud phase function is very sharply forward-peaked (hgG~0.99), that reads as lights popping in
// and out and re-aiming instantly. See BEAM_CLOUD_PLAN.md for the four in-app test rounds that
// established this, and for the 2026-08-11 attempt that tried to fix it purely by adding a fade
// on top — reverted, because easing a slot whose MEANING changes underneath it does not help, and
// recovering identity by proximity-searching a pool cost O(rawClusters x 256) per frame (20 FPS).
//
// The fix is to DECLARE identity instead of deriving it, which is possible now that sat_orbit.comp
// carries its `bestIdx` through as GpuReflectBeam::targetIdx:
//   * a CLUSTER is keyed by (targetIdx, direction bucket). The bucket is a FIXED quantization of
//     the beam's approach direction in the TARGET SITE's own local frame (reflectorSiteEnu*[]) —
//     so it depends only on that beam's own geometry, never on scan order, on a seed, or on what
//     else is in the cluster, and never on where the observer is. Nothing repartitions when a
//     member leaves.
//   * an INDIVIDUAL (transiting) beam is keyed by its originating satellite's dispatch index,
//     which sat_orbit.comp already guarantees is stable (GpuReflectBeam::debugPad).
// Both are exact integer keys, so cross-frame matching is an O(1) hash lookup rather than a
// proximity search, and a beam crossing the converged/transiting boundary simply moves between two
// eased entries — a crossfade, not a pop, with no hysteresis needed.
//
// Geometry is stored in EARTH-FIXED ECEF, in double precision, and re-projected into the current
// frame's observer ENU only at emit time. This is the single most important detail: a ground site
// is stationary in ECEF, so an entry that goes unmatched for the whole release window cannot drift,
// regardless of how far or fast the observer moves. The 2026-08-11 attempt burned three rounds
// rediscovering this — it stored observer-relative ENU and tried to correct it incrementally with
// rebase(), which is rotation-only and built for exactly ONE frame of GPU-readback staleness. Do
// not reintroduce observer-relative storage here. (doubles, not floats, because ECEF magnitudes are
// ~6.4e6 m and these values are differenced against the observer's own position at emit time.)
//
// Accepted: the eased list is history-dependent, so it is not bit-reversible under time reversal,
// unlike the orbital pipeline. Same class and precedent as skyGlareEased/mwSuppressEased — a
// visual smoothing of a derived quantity, not simulation state.
struct TrackedBeamLight
{
    // 0 = free slot. Real keys always set a high bit so that (targetIdx=0, bucket=0) and
    // (satIdx=0) can't collide with "free".
    uint32_t key = 0;
    // Eased state — what actually gets emitted.
    glm::dvec3 posECEF{0.0}; // Earth-fixed ground position of this light
    glm::dvec3 dirECEF{0.0}; // Earth-fixed unit direction, ground -> satellite
    float easedIntensity = 0.0f;
    float easedFootprintRadM = 0.0f;
    float easedBlockAltM = 0.0f;
    float easedBlockOpacity = 0.0f;
    // Per-frame accumulators (this frame's target values), zeroed at the top of every build. A slot
    // with tgtIntensity == 0 after the scan had no contributing beam this frame and is fading out
    // while HOLDING its geometry — which is exactly why the geometry has to be drift-free.
    float tgtIntensity = 0.0f;
    float tgtFootprintRadM = 0.0f;
    glm::dvec3 tgtPosSum{0.0}; // intensity-weighted
    glm::dvec3 tgtDirSum{0.0}; // intensity-weighted
    float tgtAltSum = 0.0f;
    float tgtOpacitySum = 0.0f;
};
// Power-of-two open-addressed index (key -> slot), rebuilt from the live slots at the top of every
// build. Rebuilding is O(live) <= 256 and sidesteps tombstones entirely, which is the only fiddly
// part of open addressing with deletion. Sized well above the pool so probe chains stay short.
static constexpr int kTrackedLightHashSize = 1024;
// A slot is retired once its eased intensity falls below this AND nothing is feeding it. Keeps
// fully-faded entries from occupying pool slots (and GPU light-list entries) indefinitely.
static constexpr float kTrackedLightEpsilon = 1e-4f;

// (CloudShadowPC lived here — push constants for cloud_shadow.comp's 128x128 grid, including the
//  shadowResidualM texel-snapping term that stopped shadows swimming as the observer moved. The
//  whole pass is gone; the per-pixel replacement in cloud_march.comp needs no snapping because
//  its value is a function of the world point being shaded, not of the camera's position.)

// ── Push constants for beam_self_march.comp (2026-08-09) ─────────────────────────────────────
// Replaces beam_cloud_block.comp's per-TARGET vertical march (BeamCloudBlockPC, now retired) with
// a per-BEAM slant march — needs the observer frame (obsECEFDir/obsEffH) to reconstruct each
// beam's true ECEF endpoints from its observer-relative ENU offsets, which the per-target version
// never needed (it worked in absolute ECEF target coordinates directly). See that shader's own
// header for the full design and BEAM_CLOUD_PLAN.md for the session history.
struct BeamSelfMarchPC
{
    glm::vec4 obsECEFDir; // xyz = observer ECEF unit direction (w unused)
    float obsEffH;
    float waveTime;
    float cloudPhase;
    float pad0;
}; // total: 32 bytes
static_assert(sizeof(BeamSelfMarchPC) == 32, "BeamSelfMarchPC layout mismatch");

// ── Push constants for scene_depth.comp (pipeline unification) ───────────────────────────────
// Camera-only: this pass marches terrain, so it needs the view ray and the observer, nothing
// about sun/moon/clouds. `aspect` is ALWAYS the true swapchain aspect (never a render-scaled
// one) — that is what makes the resulting depth buffer well-defined for consumers rendering at a
// different resolution than this pass.
struct SceneDepthPC
{
    glm::mat4 skyView;         // offset 0  — ENU → camera space
    float fovYRad;             // offset 64
    float aspect;              // offset 68
    uint32_t debugDisableMask; // offset 72 — bit 1 skips the terrain march; bit 1024 skips the
                               //             whole pass (fills kNoSurfaceT = nothing occludes
                               //             anything, reproducing pre-unification behaviour, so
                               //             the entire architecture A/Bs from one checkbox)
    float quarterPass;         // offset 76 — 1 = the quarter-res pre-pass (was pad0)
    glm::vec4 obsECEFDir;      // offset 80 — xyz = observer ECEF unit vector, w = height offset
    glm::vec4 sunDirENU;       // offset 96 — xyz = the Sun in ENU, w = 1: the terrain's sun shadow in .g (2026-10-25)
}; // total: 112 bytes
static_assert(sizeof(SceneDepthPC) == 112, "SceneDepthPC layout mismatch");

// Per-frame sky glow data, written by sat_flare.comp each frame.
//
//   bins[64] — Spatial histogram (45°×11.25° cells, 8 az × 8 el).
//              atomicMax(floatBitsToUint(effectFlare)) per bin.
//              Used for the wide-Gaussian aggregate sky glow pass in sat_sky.frag. Unrelated to,
//              and unaffected by, the flare/corona architecture overhaul below — a separate
//              phenomenon that happens to share this buffer for historical reasons.
//
// std430: bins[64]×uint(256).
//
// (flareCount/flareEntries[kMaxFlares] lived here — a capped atomic-append list of "bright"
// satellites that sat_sky.frag looped over PER SCREEN PIXEL to draw lens-flare coronas. Deleted in
// the flare architecture overhaul (see TERRAIN_PLAN.md): raising the cap enough to stop flicker
// with "100s" of simultaneously-bright Reflect-Orbital satellites made the per-pixel cost scale
// with it — confirmed by measurement to tank framerate to 24fps. Replaced by actually RENDERING
// every visible satellite (plus the sun) as a point into a small offscreen texture
// (flareSourceImg/flareScratchImg, see those members below), which a couple of cheap compute
// blur/streak passes turn into the corona+godray texture composited once per frame instead of once
// per pixel per bright satellite. See GpuOceanGlintBuf below for the one consumer that still needed
// a small, independent capped list of its own.)
static constexpr int kGlowBins = 64;
struct GpuGlowBuf
{
    uint32_t bins[kGlowBins]; // the SUMMED displayed effectFlare of the bin's flares past GLOW_THRESH (uint, units of 1)
    uint32_t peak;            // floatBitsToUint(the brightest effectFlare): the HUD's peak magnitude
};
static_assert(sizeof(GpuGlowBuf) == kGlowBins * 4 + 4, "GpuGlowBuf layout mismatch");

// ── Ocean satellite-glint list (flare architecture overhaul) ─────────────────────────────────
// A small, INDEPENDENT capped atomic-append list — same pattern the old flareEntries used, just
// decoupled from corona rendering and much smaller, since ocean specular glint is a minor highlight
// effect, not the primary visibility signal. Written by sat_flare.comp alongside GpuSatVisible;
// read by sat_sky.frag's ocean-glint block, which ALSO gained cloud + terrain occlusion tests at
// each entry's own screen position this round (the previous version had none at all).
static constexpr int kMaxOceanGlints = 512;
struct GpuOceanGlintBuf
{
    uint32_t count;
    uint32_t pad0, pad1, pad2;
    glm::vec4 entries[kMaxOceanGlints]; // xyz=ENU dir, w=effectFlare
};
static_assert(sizeof(GpuOceanGlintBuf) == 16 + kMaxOceanGlints * 16, "GpuOceanGlintBuf layout mismatch");

// ── Flare/corona render-to-texture pipeline (flare architecture overhaul) ────────────────────
// Replaces the deleted per-pixel flareEntries loop. Three stages, all inside recordCompute()/
// recordDraw() (no new Simulation interface hook needed):
//   1. flare_source.vert/.frag: every visible satellite (from GpuSatVisible, exactly like
//      sat_point.vert) plus one virtual point for the sun is drawn into a small offscreen target
//      (flareSourceImg), quarter the swap extent, independent of renderScale (same sizing
//      rationale as scene_depth.comp/cloud_march.comp's half-res targets — this one goes one step
//      smaller since it is deliberately going to be blurred, not sampled 1:1). Cloud + terrain
//      occlusion are tested per-point in the fragment shader (reusing sat_point.frag's existing
//      technique, plus a new terrain test this pass needs since it has no shared depth buffer of
//      its own to rely on).
//   2. flare_blur.comp: one pipeline, three dispatches, ping-ponging between flareSourceImg and
//      flareScratchImg — two separable-Gaussian passes (corona softness) then one multi-directional
//      streak pass (the godray mechanism: operates uniformly on the whole buffer, so it naturally
//      produces shafts from ANY bright, unoccluded region — sun or satellites alike — with no
//      "which light source" tracking needed).
//   3. flare_composite.vert/.frag: one additive fullscreen-triangle draw, appended at the end of
//      recordDraw(), sampling the final blurred/streaked texture into the frame.
// The sun keeps its existing disc/atmospheric-corona block and its lensFlare() ghost/streak call
// in sat_sky.frag UNCHANGED — "lens elements" (ghosts, chromatic streaks) stay sun-only, per
// explicit user decision; it ALSO becomes one bright point in this new pipeline, so it gains real
// godray shafts through cloud/terrain gaps on top of its existing hand-authored treatment.
// glare.vert/.frag push constants (drawn with flareSourcePipeLayout, whose range is FlareSourcePC's —
// so this may never grow past 112 bytes; the two fields below took over the old pad0/pad1, 2026-09-26).
struct GlarePC
{
    glm::mat4 skyView;
    float fovYRad, aspect;
    float gain;       // glareGain × the night eye-adaptation
    float sizePx;     // glareSizePx
    glm::vec2 screenSizePx;
    float maxPointSize;
    float threshold;
    float falloff; // glareFalloff
    float spikes;  // glareSpikes
    float nearGain;   // was pad0 — glareNearGain: the size multiplier a glare gets at zero range
    float nearRangeM; // was pad1 — glareNearRangeKm in metres (include/glare.glsl's glareNearScale)
    // Glare clusters (2026-10-20, include/glare.glsl): the screen grid glare_cluster.comp fills. gridW = 0 = none.
    // glare_cluster.comp reads maxPointSize as "the mesh glint list is valid this frame" (it draws nothing).
    float tilePx;
    uint32_t gridW, gridH;
    float cluster; // glareClusterMerge
};
static_assert(sizeof(GlarePC) == 128, "GlarePC layout (include/glare.glsl)");

// Mesh glints found for glare (glare_find.comp -> glare_mesh.vert; include/glint_list.glsl).
static constexpr uint32_t kMaxGlints = 64;
struct GpuGlintList
{
    uint32_t count;
    uint32_t vertexCount, instanceCount, firstVertex, firstInstance; // VkDrawIndirectCommand
    uint32_t pad[3];
    glm::vec4 pos[kMaxGlints];   // xy = screen uv, z = effectFlare
    glm::vec4 color[kMaxGlints];
};
static_assert(offsetof(GpuGlintList, vertexCount) == 4 && offsetof(GpuGlintList, pos) == 32 &&
                  sizeof(GpuGlintList) == 32 + 32 * kMaxGlints,
              "GpuGlintList layout (include/glint_list.glsl)");

struct FlareSourcePC
{
    glm::mat4 skyView;        // offset 0
    float fovYRad;            // offset 64
    float aspect;             // offset 68
    uint32_t satCeilingBits;  // offset 72 — floatBitsToUint(the satellites' bloom ceiling) (2026-10-22; was satCount,
                              //              unused since the draw became indirect)
    float sunRefIntensity;    // offset 76 — fixed reference brightness for the sun's virtual point
    glm::vec4 sunDirENU;      // offset 80 — xyz=dir, w=sin(elevation)
    glm::vec2 screenSizePx;   // offset 96 — THIS pass's own (small) target size, for the fragment
                              //             shader's occlusion UV — never assume a constant, see
                              //             the resolution-scaling gotcha this codebase already
                              //             learned once for sat_sky.frag.
    float resScale;           // offset 104 — flareExtent / ctx.swapExtent ratio; scales point size
                              //              down to this smaller target so satellites don't look
                              //              oversized once the composite upsamples back to full res
    float sunDayCompensation; // offset 108 — was pad0. The shared streak/composite gain is scaled
                              // by flareEyeAdaptGain (see recordCompute) to keep satellite glow tame
                              // in daylight — but the sun's OWN godray/corona is meant to read
                              // strongest at low sun angles/daytime, the opposite intent. This is
                              // 1/flareEyeAdaptGain, applied ONLY to the sun's own written brightness
                              // in flare_source.vert, so it nets out to full strength after the later
                              // uniform daytime suppression — satellites are unaffected.
}; // total: 112 bytes
static_assert(sizeof(FlareSourcePC) == 112, "FlareSourcePC layout mismatch");

// Push constants for flare_blur.comp — one pipeline, three dispatches per frame (see the
// architecture comment above): direction picks which image is source vs destination this
// dispatch, mode picks horizontal-gaussian / vertical-gaussian / streak.
struct FlareBlurPC
{
    uint32_t direction; // 0 = read flareSourceImg write flareScratchImg, 1 = reverse
    uint32_t mode;      // 0 = horizontal gaussian, 1 = vertical gaussian, 2 = streak
    float streakGain;   // user-tunable streak/godray strength (Settings > Display)
    float pad0;
}; // total: 16 bytes
static_assert(sizeof(FlareBlurPC) == 16, "FlareBlurPC layout mismatch");

// Push constants for flare_composite.frag — the final additive draw into the main frame.
struct FlareCompositePC
{
    float gain; // user-tunable overall glow gain (Settings > Display)
}; // total: 4 bytes
static_assert(sizeof(FlareCompositePC) == 4, "FlareCompositePC layout mismatch");

// Push constants for trail_fade.comp — see the long-exposure trail member block below.
struct TrailFadePC
{
    float decayFactor; // exp(-dt / trailDecaySeconds); dt = real wall-clock frame delta, never simDt
    float ceiling;     // per-channel soft cap on the decayed value (defense-in-depth; the primary
                       // ceiling is trail_composite.frag's own hard clamp)
    float pad0, pad1;
}; // total: 16 bytes
static_assert(sizeof(TrailFadePC) == 16, "TrailFadePC layout mismatch");

// Push constants for trail_composite.frag — the final additive draw into the main frame.
struct TrailCompositePC
{
    float gain; // user-tunable overall trail gain (Settings > Photometry, "Trail gain")
}; // total: 4 bytes
static_assert(sizeof(TrailCompositePC) == 4, "TrailCompositePC layout mismatch");

// ── GPU orbital parameters (uploaded once per buildOrbits, device-local) ─────
// 16 × 4-byte fields = 64 bytes (was 112 — the per-type reflectance fields moved to GpuSatType,
// looked up via typeIdx). All plain floats/uints — no vec3 — so C++ struct packing matches GLSL
// std430 without any alignment padding. Must match the SatOrbit struct in sat_orbit.comp exactly.
struct GpuSatOrbit
{
    float raan;    // right ascension of ascending node at epoch
    float u0;      // epoch-baked initial phase: fmod(orig_u0 + meanMot*epochT0, 2π)
    float R_sat;   // kEarthRadius + altM (meters)
    float meanMot; // sqrt(GM/R³) (rad/s)

    float cosI;    // cos(inclination)
    float sinI;    // sin(inclination)
    float cosRaan; // cos(raan); valid when !alignTerminator
    float sinRaan; // sin(raan); valid when !alignTerminator

    float tumbleRate;  // rotation rate (rad/s); 0 if not tumbling
    float tumblePhase; // epoch-baked angle: fmod(phase + rate*epochT0, 2π)
    float tumbleAxisX; // formation member (constIdx & kGpuFormationBit): formP (m)
    float tumbleAxisY; // formation member: formQ (m)

    float tumbleAxisZ;     // formation member: formK (cross-track / radial)
    float alignTerminator; // 1.0 = SSO (RAAN precesses); 0.0 = fixed RAAN
    uint32_t typeIdx;      // index into satTypeBuf's GpuSatType array
    uint32_t constIdx;     // constellation index (low 5 bits) for enabled/highlight masks;
                           // kGpuFormationBit = formation ring member (tumble* hold formP/Q/K)
    // Total: 64 bytes
};
static_assert(sizeof(GpuSatOrbit) == 64, "GpuSatOrbit layout mismatch");
static_assert(offsetof(GpuSatOrbit, tumbleAxisX) == 40 && offsetof(GpuSatOrbit, constIdx) == 60,
              "GpuSatOrbit layout mismatch (sat_orbit.comp reads the formation fields by position)");
constexpr uint32_t kGpuFormationBit = 0x80000000u; // GpuSatOrbit::constIdx flag, sat_orbit.comp mirror

// The epoch-free phases sat_rebake.comp re-bakes GpuSatOrbit's u0 / tumblePhase / raan from
// (2026-10-15, branch kessler). Written once with the full upload; 12 B per satellite.
struct GpuSatPhaseBase
{
    float u0;          // SatOrbit::u0 (argument of latitude at J2000 + its own n·t, see orbitElemsOf)
    float tumblePhase; // SatOrbit::tumblePhase (0 for a formation member)
    float raan;        // SatOrbit::raan (SSO: the value at the sim-start anchor)
};
static_assert(sizeof(GpuSatPhaseBase) == 12, "GpuSatPhaseBase must match sat_rebake.comp");

// sat_rebake.comp: the orbit epoch in whole seconds since J2000 (int64 as two words) and the SSO
// precession since the anchor at that epoch.
struct SatRebakePC
{
    uint32_t epochLo;
    int32_t epochHi;
    float raanOff;   // fmod(kSSOPrecRate·(epoch − sim start), 2π)
    uint32_t satCount;
    uint32_t poolCount; // Kessler pool records on the GPU (re-baked after the roster)
    uint32_t poolWord0;
    uint32_t pad0, pad1;
};
static_assert(sizeof(SatRebakePC) == 32, "SatRebakePC must match sat_rebake.comp");

// GpuReflectorTarget / the per-frame CPU-compacted night-side buffer it described were removed
// 2026-08-06: sat_orbit.comp now reads the static per-target ECEF buffer directly (the same one
// beam_cloud_block.comp already used) and rotates to live ECI itself via SatOrbitPC::gmstNow — see
// CLAUDE.md's TargetedReflector section for why the CPU-side compaction became dead code.

// ── Reflect-Orbital ground beams (device-local, written by sat_orbit.comp) ───
// History: originally azimuth-sector-keyed (16 sectors, GpuGlowBuf's flareEntries/sectorBright
// idiom) — let unrelated satellites aiming at different real targets collide whenever they
// shared a 22.5° bearing wedge. Re-keyed by TARGET IDENTITY (bestIdx, one slot per site) to fix
// that — but then a well-serviced site (many satellites simultaneously choosing it) could only
// ever show its single brightest satellite via atomicMax, the rest silently dropped. Tried 4
// sub-slots per site next — still arbitrary and still glitchy for a site serviced by "dozens"
// of satellites (user report, 2026-07-21): whichever 4 happened to win kept changing as
// brightness/geometry shifted, so the beam still visibly jumped between represented satellites.
//
// **Current design (2026-07-21): plain capped atomic-append, no site keying and no
// deduplication/arbitration at all.** Every satellite that reaches the TargetedReflector branch
// with a real target (bestIdx >= 0) claims its OWN slot via a single global atomicAdd counter
// and writes its own beam unconditionally — no competition, so nothing to glitch between AS LONG
// AS the cap isn't actually hit.
//
// kMaxActiveBeams (C12 follow-up #11, 2026-07-21: 256->2048): the first cap (256) was too tight
// and reintroduced arbitration by a different door — when genuinely more satellites are eligible
// than the cap, only the first N (by atomicAdd claim order, which correlates with GPU dispatch
// order and therefore with satellite ARRAY INDEX, not anything geometric) get written, and every
// later-indexed satellite is silently dropped EVERY frame. Reflect Orbital is a "Disk" (per
// CLAUDE.md — a single orbital plane, 10 concentric altitude rings, not spread across many
// planes/RAANs like a Walker constellation), so the whole constellation sweeps together along
// essentially one great circle — from a fixed observer, the VISIBLE fraction of that one ring is
// a specific, fairly large arc (not a small scattered sample of the whole sphere), and as
// low-index satellites within that eligible set changed which physical satellites they were
// (the ring sweeping across the sky over time / with observer motion), the rendered subset
// visibly "stacked" toward whichever side of the sky currently held the lower-indexed eligible
// satellites — reported as flares fading in from one side and stacking on the other, reversing
// when time direction reversed, and lagging when moving quickly into a new region. Re-estimated
// the realistic worst case at ~900 simultaneously eligible for a 5,000-satellite version of this
// constellation (a single-plane arc estimate, not the much smaller uniform-sphere estimate
// follow-up #9 used) — kMaxActiveBeams raised to 2048 for comfortable headroom over that, making
// overflow effectively never happen rather than trying to make the overflow's bias smaller.
// GpuReflectBeams is still tiny at this size (~96KB, after the debug reflectDirENU field below
// grew each entry to 48 bytes). A satellite's slot index isn't stable frame
// to frame (a race on the counter) but that no longer matters when nothing is being dropped:
// nothing else depends on WHICH index a given satellite lands in, only that everything currently
// eligible gets rendered, every frame. Zeroed every frame via vkCmdFillBuffer (resets beamCount
// to 0 too), same pattern as glowBuf.
static constexpr int kMaxActiveBeams = 2048;
struct GpuReflectBeam
{
    glm::vec3 satENU;          // meters, observer-relative (East, North, Up)
    float intensity;           // groundIrradiance * beamGain — NOT the view-dependent
                               // mirrorPeak specular term; see sat_orbit.comp writer comment
    glm::vec3 targetENU;       // meters, observer-relative; exact 3D ENU projection of the
                               // chosen ground target — correctly encodes Earth curvature
    float footprintRadM;       // ground footprint radius
    glm::vec3 reflectDirENU;   // unit direction, observer ENU basis — the mirror's ACTUAL current
                               // reflected-sunlight direction (reflect(-sunDirECI, surfN0)), which
                               // may differ from normalize(targetENU-satENU) while the mirror is
                               // still slewing toward bestIdx's target (MIRROR_ROT_RATE-limited).
                               // Debug-only (C12 follow-up #12): drawn as a long "pointing ray" from
                               // the satellite so a busy site's convergence — and any satellites
                               // still mid-slew and not yet converged — can be seen directly.
    float debugPad;            // Repurposed (C12 follow-up #20): carries the originating satellite's
                               // own stable dispatch index (written as float(i) in sat_orbit.comp) —
                               // used by cloud_march.comp's sky glow to downsample by a STABLE subset
                               // of satellites rather than by the atomic-append slot index (which
                               // isn't stable frame-to-frame). Name kept for minimal diff; no longer
                               // debug-only or padding.
    float blockAltM;           // Altitude (m above sea level) at which THIS BEAM's own real 3D path
                               // (ground intersection -> satellite) first drops below 50%
                               // transmittance. Written by beam_self_march.comp (2026-08-09), a
                               // per-beam slant march — was beam_cloud_block.comp's per-TARGET
                               // vertical-column approximation (C12 follow-up #33) before that. See
                               // beam_self_march.comp's own header for the full design and why this
                               // (not the per-target version) is what beam physics actually needs.
                               // Irrelevant when blockOpacity==0.
    float blockOpacity;        // 0 = clear path, 1 = fully opaque — see beam_self_march.comp.
                               // Consumed by cloud_march.comp (fades the volumetric glow and, as of
                               // 2026-08-09, the visible pointing ray below the cloud) and
                               // sat_sky.frag (ground-spot dimming).
    float mirrorRadiusM;       // C12 follow-up #34: repurposed from padding — equivalent-circle radius
                               // of the physical mirror (sqrt(mirrorAreaM2/PI)). Consumed by
                               // cloud_march.comp (sky tube radius) and sat_sky.frag (ground-spot core).
    float aimErrorRad;         // Repurposed from padding (2026-08-06): angle, in radians, between the
                               // mirror's current attitude and the exact ideal half-vector toward
                               // `bestIdx`'s target this frame. 0 outside a window-boundary crossfade
                               // (the mirror aims exactly at its target); non-zero only during the
                               // brief blend between two different targets at a lock-window boundary
                               // (see sat_orbit.comp's TargetedReflector block and CLAUDE.md) — lets a
                               // consumer distinguish "mid-crossfade" from "locked and settled" the
                               // same way it always could, just driven by the new stateless windowed
                               // selection instead of the old rate-limited slew.
    uint32_t targetIdx;        // 2026-08-12: sat_orbit.comp's own `bestIdx` — the resolved ground
                               // target's ORIGINAL index into reflectorTargetsECEF[]/
                               // reflectorTargetsRadiusM[]/reflectorSiteEnu*[]. Always valid
                               // ([0, reflectorTargetCount)) for any entry that exists at all, since
                               // sat_orbit.comp only writes a beam inside `if (bestIdx >= 0)`.
                               // This is a STABLE INTEGER IDENTITY, which is the entire reason it
                               // exists: the cloud-light build below keys clusters on it directly
                               // instead of epsilon-matching targetENU against whichever beam
                               // happened to be scanned first. See TrackedBeamLight's comment.
    uint32_t pad0, pad1, pad2; // MUST be declared explicitly, in both this struct and ReflectBeam
                               // in shaders/include/reflect_beam.glsl. std430 rounds the GLSL struct
                               // up to its 16-byte alignment (vec3 members) whether or not the pads
                               // are written; C++ does NOT, because glm::vec3 is 4-aligned. Before
                               // targetIdx the total happened to be exactly 64 and the two agreed by
                               // luck. Same silent-permutation hazard as GpuCloudParams — no compile
                               // error, every field past the divergence point reads its neighbour.
};
static_assert(sizeof(GpuReflectBeam) == 80, "GpuReflectBeam layout mismatch"); // 64 -> 80, 2026-08-12 (targetIdx + explicit pads)

struct GpuReflectBeams
{
    uint32_t beamCount;                      // atomicAdd counter — total claims this frame, may exceed kMaxActiveBeams
    uint32_t pad0, pad1, pad2;               // std430 array-of-16-byte-aligned-struct alignment padding
    GpuReflectBeam entries[kMaxActiveBeams]; // only entries[0 .. min(beamCount,kMaxActiveBeams)) are valid
};
static_assert(sizeof(GpuReflectBeams) == 16 + kMaxActiveBeams * 80, "GpuReflectBeams layout mismatch");

// Ground-beam compaction (perf follow-up, RELEASE_v1_1_PLAN.md): CPU-built every frame from
// ReflectBeamsBuf's readback (the same loop that already computes lastActiveBeamCount/
// beamProximityGlow), filtered to entries within beamMaxRangeM of the observer — the exact cull
// sat_sky.frag's ground-spot loop used to redo per-pixel, against the FULL raw (up to
// kMaxActiveBeams=2048) buffer, for every ground-hit pixel on screen. Consumed by sat_sky.frag
// instead of ReflectBeamsBuf directly, so that loop's trip count is bounded by how many beams are
// actually within range of the CAMERA, not by how many are active anywhere across the whole
// visible constellation (measured: disabling the "Reflect-Orbital beams" debug knockout bit nearly
// doubled frame rate — this is the dominant cost that knockout was hiding). Entries are raw,
// unaggregated ReflectBeam records (per-satellite, not summed by target) because the ground-spot
// term needs each satellite's own satENU for its elevation fade.
//
// 2026-08-10: entries are no longer raw GpuReflectBeam records — they are a PRE-SOLVED
// GpuGroundBeam, which is what makes the shader loop cheap. The Anchorage sweep measured that loop
// at 1.59 ms of sky_background_draw at Medium, and the reason was that essentially its whole body
// was view-INDEPENDENT and being recomputed for every ground-hit pixel at full resolution: a
// length(targetENU) plus a smoothstep for the range fade, an obsPos+satENU and a raySphere (two
// sqrts) for the real ray/ground intersection, the elevation fade, and the shadow attenuation. Only
// the horizontal distance from the shaded point to the landing spot, and the two Gaussians built
// from it, genuinely vary per pixel. All the rest is now folded on the CPU — once per beam, at most
// kMaxGroundBeams times per frame, in the readback loop that was already visiting these entries —
// into `weight` and a few reciprocals, so the shader's per-beam work drops to a 2D difference, a
// dot, a squared-distance reject and two exps, with the reject FIRST rather than last.
//
// Struct must match GroundBeam/GroundBeamsBuf in sat_sky.frag exactly (hand-mirrored, same
// convention and same hazard as GpuCloudParams — all plain floats so std430 needs no padding
// beyond what is written here).
static constexpr int kMaxGroundBeams = 256;
struct GpuGroundBeam
{
    float groundHitX, groundHitY; // observer-relative ENU horizontal position of the beam's REAL
                                  // ray/ground intersection (not the chosen target's site)
    float invFootprintSq;         // 1 / footprintR^2   — halo Gaussian
    float invCoreSq;              // 1 / coreR^2        — hotspot Gaussian
    float cutoffSq;               // (footprintR * 4)^2 — the shader's first and cheapest reject
    float weight;                 // intensity * rangeFade * elevFade * shadowAtten, i.e. every
                                  // view-independent multiplier the old shader loop applied
    float intensity;              // raw pre-fade intensity — CPU-side top-K ranking ONLY, never read
                                  // by the shader. Kept so eviction ranks on exactly the same
                                  // quantity it did before this rework (see the top-K comment at
                                  // the insertion site for why ranking stability matters).
    uint32_t dirOct;              // ground -> satellite unit direction (observer ENU), octahedral,
                                  // packSnorm2x16 — the solar arrays' specular lobe (was a pad)
};
static_assert(sizeof(GpuGroundBeam) == 32, "GpuGroundBeam layout mismatch");
// Solar PV parks drawn on the terrain (2026-10-02): the reflector targets of kind "solar" nearest the
// observer, appended to the ground-beam buffer (sat_sky.frag is at its sampled-image floor and carries
// many SSBOs already, so no new binding). Everything is in the MAIN observer's ENU frame, solved on the
// CPU in double: `centerQ` is the site centre relative to the observer's SEA-LEVEL point (the frame of
// sat_sky.frag's terrainQ, so the difference is small and float-exact), the axes are the site's own
// tangent axes. Hand-mirrored as SolarSite in sat_sky.frag.
static constexpr int kMaxSolarSites = 8;
struct GpuSolarSite
{
    glm::vec4 centerQ;  // xyz site centre (observer ENU, from the observer's sea-level point), w = outer radius (m)
    glm::vec4 across;   // xyz the row-spacing axis (east for trackers, north for fixed tilt), w = seed
    glm::vec4 along;    // xyz the row axis, w = row pitch (m)
    glm::vec4 normal;   // xyz panel normal (observer ENU, this frame's tracker angle), w = panel width (m)
    glm::vec4 params;   // x = block fill (area / disk), y = 1 tracker / 0 fixed, z = projected panel width (m), w = segment length (m)
};
static_assert(sizeof(GpuSolarSite) == 80, "GpuSolarSite layout mismatch");
// The refraction table (2026-10-26, SatelliteSimRefraction.cpp; Refraction.h): appended to GpuGroundBeams (no new
// sky binding: sat_sky.frag sits at its storage-buffer budget). Mirrored by RefrTable in sat_sky.frag.
struct GpuRefrEntry
{
    glm::vec4 a; // xyz = refraction per channel (rad; >= 1 = the ray ends on the surface), w = rms angle-of-arrival jitter (rad)
    glm::vec4 b; // x = Rayleigh column, y = Mie column, z = ozone (path in the layer / 20 km), w = haze column (all m)
};
struct GpuRefraction
{
    glm::vec4 map;    // e(u) = x + y sinh(z (u - w)), u = i / (N - 1): the apparent elevation of entry i
    glm::vec4 info;   // x = N (0 = no table), y = refraction above the table / Saemundsson, z = 1 refraction on, w = Sun disc gain
    glm::vec4 sun;    // x = the Sun centre's refraction (rad), y = its visible share (0..), z = shimmer gain, w = aoa there (rad)
    glm::vec4 sunCol; // the columns at the Sun's centre (as GpuRefrEntry::b)
    // Sunspot groups on the disc now (2026-10-27, SatelliteSimRefraction.cpp fillSunspots; sat_sky.frag sunspotFactor).
    glm::vec4 spotInfo;  // x = groups, y = the solar cycle's activity 0..1, z = the Moon centre's refraction (rad)
    glm::vec4 spots[2 * 16];   // group g: [2g] = (latitude, longitude from the central meridian (rad, + west), half-length (sphere radii), seed),
                               // [2g + 1] = (Joy's-law tilt (rad), envelope 0..1, complexity (delta) 0..1, size 0..1)
    GpuRefrEntry e[refr::kTableN];
    // The solar corona's structure now (2026-10-28, SatelliteSimCorona.cpp fillCorona; sat_sky.frag solarCoronaDyn): a
    // pure function of sim time from the solar cycle, the Sun's rotation and the space weather's CMEs.
    glm::vec4 corInfo;    // x = activity 0 (minimum) .. 1 (maximum), y = the fine structure's phase (fract), z = streamers,
                          // w = 1 the inertial lock's occulter (a coronagraph: drawn without an eclipse)
    glm::vec4 corInfo2;   // x = CMEs, y / z = the neutral line's apparent latitude on the east / west limb (rad),
                          // w = the coronagraph's radial display lift (exponent of r)
    glm::vec4 corStr[2 * 12];   // streamer k: [2k] = (apparent latitude of its base (rad), side +1 west / -1 east,
                                // plane-of-sky factor, amplitude), [2k + 1] = (base half-width (rad), stalk half-width
                                // (rad), cusp radius (solar radii), bend toward the neutral line 0..1)
    glm::vec4 corCme[2 * 6];    // CME c: [2c] = (axis position angle from north, west + (rad), half-width (rad), front
                                // radius on the sky (solar radii), amplitude), [2c + 1] = (1 halo, seed, core share, 0)
};
static_assert(sizeof(GpuRefraction) == 80 + 2 * 16 * 16 + refr::kTableN * 32 + 32 + 24 * 16 + 12 * 16,
              "GpuRefraction layout mismatch");
struct GpuGroundBeams
{
    uint32_t count;
    uint32_t solarCount;  // GpuSolarSite entries in `solar` (was pad0)
    uint32_t pad1, pad2;
    GpuGroundBeam entries[kMaxGroundBeams];
    GpuSolarSite solar[kMaxSolarSites];
    GpuRefraction refr;
};
static_assert(sizeof(GpuGroundBeams) == 16 + kMaxGroundBeams * 32 + kMaxSolarSites * 80 + sizeof(GpuRefraction), "GpuGroundBeams layout mismatch");
static_assert(offsetof(GpuGroundBeams, solar) == 16 + kMaxGroundBeams * 32, "GpuGroundBeams layout mismatch");

// ── Per-layer cloud shell descriptor (std140: 32 bytes, 2 × vec4) ─────────────
// Each layer is an infinitely thin sphere-shell sample of earthCloudsTex.
// Layers 0+ are evaluated in order; disabled layers (enabled=0) are skipped.
struct GpuCloudLayerParams
{
    float shellAltM;    // sphere-shell altitude above R_EARTH (m)
    float driftMult;    // cloudPhase longitude multiplier (1.0 = same speed as surface)
    float alphaMax;     // maximum opacity of this layer [0,1]
    float mipLod;       // fixed texture LOD (0=sharp, 2=soft/wispy)
    float coverageMult; // scales global coverage for this layer
    float densityMult;  // scales global density for this layer
    float enabled;      // 1.0 = active, 0.0 = skip
    float pad;
};
static_assert(sizeof(GpuCloudLayerParams) == 32, "GpuCloudLayerParams layout mismatch");

static constexpr int kNumCloudLayers = 4;

// ── Cloud parameters UBO (binding 9 in sky descriptor set) ───────────────────
// Matches the layout(binding=9) uniform CloudParams block in sat_sky.frag.
// Global tunables + per-layer descriptors.  cloudPhase is CPU-computed each frame.
// std140 layout: 96-byte global section (6×vec4) + 4 × 32-byte layer = 224 bytes.
// city_sprites.comp push constants (48 bytes).
struct CitySpritePC {
    glm::vec4 obsECEFDir;
    float simTime, gain, nightF;
    uint32_t capacity;
    float radiusM, cellM;
    int32_t halfCells;
    float innerM, weight, twinkleRate, pixAng, startFootM;
};
static_assert(sizeof(CitySpritePC) == 64, "CitySpritePC must match city_sprites.comp");

// Launch pads drawn on the ground (2026-10-19; cloud_params.glsl PAD_MAX must match).
static constexpr int kMaxPadsGpu = 12;
static constexpr int kMaxCoastPatchesGpu = 4;   // == COAST_MAX (cloud_params.glsl)
static constexpr int kMaxFiresGpu = 8;          // == FIRE_MAX (cloud_params.glsl): wildfire burn scars
static constexpr int kCoastGridN = 48;          // == COAST_GRID_N (tools/make_pad_coasts.py GRID_N)
struct GpuCloudParams
{
    // Global controls — shared across all layers
    float coverage;           // global coverage gate [0,1]
    float density;            // global density sharpness scale
    float driftRate;          // base longitude drift rate (rad/s sim-time)
    float sunGain;            // global sun brightness multiplier
    float ambientGain;        // night-side ambient (for future use in volumetrics)
    float farBlend;           // review 18: the far cloud layer's share, 0..1 (cloudFarBlend(); was v1's unread hgG)
    float oceanWaveRangeM;    // review 21: "Sea wave range (km)" x 1000 (was v1's unread marchSteps)
    float oceanWaveFootK;     // review 21: "Sea wave sharpness" (was v1's unread lightSteps)
    float cloudPhase;         // CPU: fmod(driftRate * simTime, 2π) — uploaded each frame
    float extinctionCoeff;    // was pad0 (freed session 23 when cloudShadowFactor was removed); now
                              // carries the same atmospheric-extinction coefficient sat_flare.comp
                              // gets via push constant, so sat_sky.frag's Milky Way term can apply
                              // identical Kasten & Young dimming without its own push-constant field
    float cirrusWindAngle;    // C13: cirrus streak wind axis, radians (was pad1)
    float cirrusStretch;      // C13: cirrus noise anisotropic elongation factor (was pad2)
    float airglowGain;        // C15: master airglow brightness multiplier
    float airglowGreenGain;   // C15: green (557.7nm) band gain
    float airglowRedGain;     // C15: red (630.0nm) band gain
    float airglowSodiumGain;  // C15: sodium (589.3nm) band gain — keep dim relative to green
    float groundPatternFootM; // review 8: farm/ground pattern range, a pixel footprint (m) (was v1's unread shadowMaxDistM)
    float skyLodScreenH;      // sat_sky.frag's LOD pixel rows (output height while upscaling; was v1's maxRenderDistM)
    float viewSamplesMin;     // perf (session 24 round 2): N_VIEW floor for short rays (was pad2)
    float lightSamples;       // perf (session 24): N_LIGHT optDepth sub-march count (was pad3)
    float oceanSeaOctaves;    // perf (session 24): seaMap() octave count (height-trace geometry)
    float oceanDetailOctaves; // perf (session 24): seaMapDetail() octave count (wave normal)
    float oceanReflSamples;   // perf (session 24): ocean sky-reflection loop sample count (N_REFL)
    float viewSamplesMax;     // perf (session 24 round 2): N_VIEW ceiling for long/grazing rays (was pad4)
    float sunGainZenith;      // was pad3 — sun-gain multiplier near sun zenith, blended against
                              // `sunGain` (effectively the near-horizon/sunset value) by sun
                              // elevation in both cloudMarchCS/cirrusMarchCS and evalCloudLayer.
    float moonGain;           // shared moonlight brightness master — terrain direct term AND
                              // cloud_march.comp's moonContrib (was a hardcoded 0.015 there) both
                              // read this, so one slider keeps moonlit terrain and moonlit clouds
                              // calibrated to the same brightness instead of drifting apart
    float pad1;               // ACTUALLY repurposed (see SatelliteSim.cpp) — city-detail world-fixed
                              // east offset (m); kept named pad1 since sat_sky.frag/cloud_march.comp
                              // don't need to read it (CPU computes the offset, GPU just samples the
                              // resulting texture), name is stale but layout-critical, don't rename
    float pad2;               // ACTUALLY repurposed — city-detail world-fixed north offset (m); see pad1
    // Milky Way skybox (session 27): CPU-computed ENU->galactic basis rows (fixed orientation,
    // confirmed by eye against the real star field), mirroring the eciX/Y/Z basis-vector
    // convention already used for SatOrbitPC/SatFlarePC. dirGal = dot(enuDir, mwBasisRowN.xyz)
    // for N=0,1,2. .w of row0 carries a fixed gain of 1.0 (spare otherwise).
    glm::vec4 mwBasisRow0;
    glm::vec4 mwBasisRow1;
    glm::vec4 mwBasisRow2;
    // Per-layer descriptors
    GpuCloudLayerParams layers[kNumCloudLayers];
    // Aurora (C16, TERRAIN_PLAN.md Phase E): geomagnetic curtain primitive.
    float stormStrength;    // [0,1] drives oval equatorward expansion, brightness, fold chaos
    float auroraGain;       // master aurora brightness multiplier (sky curtain itself)
    float auroraCloudGain;  // master gain for LOCAL aurora ambient upwelling on CLOUDS only —
                            // split from auroraGroundGain (session 28 follow-up #6) because the
                            // two formulas' magnitudes aren't comparable: clouds have no albedo
                            // term at all (roughly full reflectivity assumed) while terrain/ocean
                            // multiply by the surface's own dark albedo, so one shared slider
                            // couldn't hit "plausible" for both at once.
    float auroraGroundGain; // master gain for the LOCAL, per-point aurora ambient/reflection
                            // lighting on TERRAIN/OCEAN (evaluated in-shader per pixel/sample,
                            // mirroring how moonlight is local) — distinct from auroraGain above
                            // (the sky curtain's own brightness) and auroraCloudGain (clouds).
    // Aurora "erosion" coverage gate (session 28 follow-up #8) — breaks the oval into patchy arcs.
    // See auroraCoverage() in sat_sky.frag/cloud_march.comp for the full design.
    float auroraCoverageFreq;      // per-degree colatitude frequency — patch size across the band
    float auroraCoverageAzFreq;    // azimuthal wobble frequency — keeps the boundary non-circular
    float auroraCoverageDriftRate; // wall-clock rad/s evolution speed
    float auroraShimmerRate;       // curtain fold noise evolution speed (wall-clock rad/s) — was a
                                   // fixed kAuroraShimmerRate constant (session 28 follow-up #9)
    // Struct grew 320->336 here (session 30): appended rather than reusing pad1/pad2 above, which
    // turned out to already be repurposed (city-detail world offset, read by name as cloud.pad1/
    // pad2 in sat_sky.frag) despite their stale "reserved" comments — do not repurpose those.
    float cloudTwilightAmbientGain; // gain on cloud_march.comp's twilightAmbient term — sky-lit
                                    // cloud at dusk/dawn only (twilightWeight is a bell, so this
                                    // contributes nothing in daylight or full night). Same UBO slot
                                    // that used to drive a non-decaying night floor; that term was
                                    // the wrong effect and is gone. Deliberately separate from
                                    // ambientGain, which also drives city-light upwelling.
                                    // Unused in sat_sky.frag — kept for layout parity.
    float cloudBaseVariance;        // was pad4 — noise-driven cloud base height undulation (hNorm
                                    // units, 0 = old perfectly flat base). See cloudMarchCS.
    float cloudErosionEdge;         // was pad5 — cloudDensity() erosion strength at the silhouette
                                    // edge (base near 0).
    float cloudErosionCore;         // was pad6 — cloudDensity() erosion strength at the dense core
                                    // (base near 1); kept lower than cloudErosionEdge.
    // Struct grew 336->352. std140 rounds a uniform block up to a multiple of 16, so a single
    // trailing float would have left the GLSL block at 352 while this struct stayed 340 — a
    // silent mismatch. The three pads keep both at 352; take one when the next field is needed.
    float sunGainElevBand; // sin(sun elevation) at which sunGainZenith fully replaces
                           // sunGain. Was a hardcoded smoothstep(0,1,sinElev): only
                           // half-way to the zenith value at 30 degrees up, so a sunset-
                           // tuned gain stayed dominant through most of the morning.
    float twilightBandHi;  // sin(sun elev) above which twilight cloud ambient is zero.
                           // Raise to bring the term FORWARD into sunset — the original
                           // hardcoded 0.15 left a gap where direct sun had faded but the
                           // sky term had not arrived (clouds briefly went black).
    float twilightBandLo;  // sin(sun elev) below which it is zero — how far into night it
                           // carries.
    // ORDER BELOW MUST MATCH shaders/include/cloud_params.glsl EXACTLY, field for field.
    //
    // This is the one pairing the shared header cannot protect — GLSL and C++ can't share a
    // declaration, so this struct is a hand-maintained mirror. It was gotten wrong immediately
    // after that header landed: these four were appended AFTER coverageMipLod in the GLSL but
    // inserted BEFORE it here. Nothing failed to compile and the static_assert still passed,
    // because the total size was right; every field from coverageMipLod onward simply read its
    // neighbour's value. The visible result was flatSunGainScale reading pad10 (0 -> black
    // clouds) and flatCoverageScale reading 4.0 (-> coverage x4, clouds swallowing the Earth).
    //
    // A size check cannot catch a permutation. When adding a field, add it in the same position
    // in both files, and prefer appending at the end of both.
    float coverageMipLod;      // mip the volumetric march samples earthCloudsTex at. Was a
                               // hardcoded 4.0 (~78 km/texel on the 8K source): the volumetric
                               // shape could only ever follow large blobs, while the flat 2D
                               // layer sampled sharply — which is why the two never matched
                               // and the 3D->2D crossfade had to be pushed out to 800 km.
    float flatCoverageScale;   // see cloud_params.glsl — maps the shared Coverage slider onto
                               // the flat 2D layer, which needs a lower value than the
                               // volumetric for the same apparent cloud amount.
    float flatSunGainScale;    // same idea for Sun gain: the flat layer is a single multiply
                               // while the volumetric accumulates through transmittance, so
                               // the same slider lands ~4x dimmer on the flat path.
    float fogTopAltM;          // C11 (repurposed from pad10) — ground fog shell top altitude
                               // (m above sea level); see fogMarchCS in cloud_march.comp.
    float fogDensity;          // C11 (repurposed from pad11) — fog density scale.
    float cloudDistFadeStartM; // distance-based 3D->2D crossfade: fully volumetric nearer than
                               // this, fully flat-2D beyond cloudDistFadeEndM. Keyed on the
                               // per-ray distance to the cloud shell, so it actually bounds the
                               // march — maxRenderDistM caps march LENGTH from the shell entry
                               // and so does nothing from orbit, where that span is just the
                               // ~9 km shell crossing.
    float cloudDistFadeEndM;
    float fogCoverage; // C11 (repurposed from pad12) — ground fog global coverage gate.
    float fogSunGain;  // C11 (repurposed from pad13) — fog sun-lit brightness gain,
                       // separate from cloud.sunGain per [[feedback_shared_gain_sliders]].
    // Terrain march distance fade (S4, RELEASE_v1_1_PLAN.md session 31) — see cloud_params.glsl
    // for the full design rationale. Fades out sat_sky.frag's terrain-relief march step budget
    // as this ray's own march reach (tExit) grows, skipping it outright beyond End and falling
    // back to the sea-level sphere, which already exists as the "no hit" result.
    float terrainDistFadeStartM;
    float terrainDistFadeEndM;
    float cloudOpacityScale; // repurposed from pad14 — see cloud_params.glsl for the full
                             // rationale: scales the volumetric cloud march's extinction
                             // coefficient directly, since per-sample density `d` is clamped to
                             // [0,1] before extinction is derived, so the `density` slider alone
                             // has a hard ceiling on achievable opacity. Default 1.0.
    float cityLightBlurLod;  // repurposed from pad15 — see cloud_params.glsl. Mip LOD blend
                             // target for earthNightTex/cityNightDetailTex under cloud, at full
                             // local cloud opacity. Default 3.0; 0 disables the blur.
    // Atmospheric scattering strength gains — see cloud_params.glsl for the full rationale.
    // Scale BETA_R_BASE/BETA_M_BASE (common.glsl) uniformly across every shader that shadows them.
    // Default 1.0 each reproduces the original hardcoded constants exactly.
    float atmosRayleighGain;
    float atmosMieGain;
    float cloudWarpStrength; // domain-warp amplitude / spatial frequency — their PRODUCT is the
    float cloudWarpFreq;     // shear. See cloud_params.glsl for the folding threshold.
    // Erosion redesign, 416 -> 432. APPENDED rather than reusing pad1/pad2 above, which are
    // already repurposed as the city-detail world offsets despite their stale names.
    float cloudSurfaceCarve;   // 0 = heightProfile multiplies the eroded field (original),
                               // 1 = it is subtracted, displacing the top/bottom SURFACE by the
                               // erosion noise instead of fading the noise out at it
    float cloudErosionBillow;  // 0 = original inverted-Worley polarity (removes round blobs,
                               // leaves a web), 1 = flipped (removes the web, leaves lumps)
    float cloudErosionBillowH; // normalised column height over which polarity ramps wispy->billowy
    float cloudErosionFreq;    // erosion lookup coordinate scale (was a hardcoded 1.5)
    // Directional-shading contrast, 432 -> 448. See cloud_params.glsl for why all four of these
    // were flattening the only term that carries lit-side/dark-side variation.
    float cloudMultiScatter; // 0 = single scatter (hard shadows), 1 = original 3-octave sum
    float cloudShadowFloorT; // hard floor on sun transmittance (was a fixed 0.05)
    float cloudGrazeShadow;  // sunOptDepth multiplier at zero sun elevation (was a fixed 0.35)
    float cloudConeLenScale; // multiplier on the self-shadow cone's length cap
    // Shape-aware shading + flat-layer decoupling, 448 -> 464. See cloud_params.glsl.
    float cloudVertShadeGain; // strength of the height-only shading ramp ("lasagna" banding)
    float cloudDensityAO;     // occlusion driven by local density — carries real cloud shape
    float cloudAOPower;       // where 1-exp(-d*power) bites; tune against `density`
    float flatDensityScale;   // flat 2D layer opacity, decoupled from volumetric `density`
    // Flat-2D Rayleigh decoupling, 464 -> 480. See cloud_params.glsl for why the flat layer needs
    // its own Rayleigh multiplier to make the 3D->2D crossfade match.
    float flatRayleighGain;        // extra BETA_R multiplier for evalCloudLayer only; 1.0 = no-op
    float flatTwilightAmbientGain; // flat layer's share of the twilight sky ambient, stacked on
                                   // cloudTwilightAmbientGain; 1.0 = volumetric-equal strength
    // Orbital terminator gate — claimed the last two pads, so the block is still 480 but there is
    // no slack left. The NEXT field added here has to grow this struct AND cloud_params.glsl by a
    // full 16 bytes together. See cloud_params.glsl for what these do and why they are altitude-
    // gated.
    float atmosTermStrength; // 0 = previous behaviour exactly; 1 = full suppression past the cut
    float atmosTermWidth;    // rolloff half-width in sin(sun elev); smaller = harder cliff
    // Airglow coverage + polar boost, 480 -> 496. See cloud_params.glsl for the full design.
    float airglowCoverageGain; // 0 = uniform shimmer (old behaviour), 1 = full patchy coverage
    float airglowPolarGain;    // RED band only: extra boost ramping toward the geomagnetic pole
    // ── Push-constant relief (496 -> 544) ────────────────────────────────────────────────────
    // These were push-constant fields on SatDrawPC / CloudMarchPC until both structs had to fit
    // the 128-byte maxPushConstantsSize floor (oldest AMD integrated GPUs). All are per-frame-
    // uniform (one value for the whole frame, not per-draw), which is exactly what a frame UBO is
    // for. Consumers: sat_sky.frag (all of them) and cloud_march.comp (dbgDisableMask,
    // showBeamDebugRays, beamMaxRangeM, beamSkyGlowGain, cloudShadowRangeM). Filled in
    // recordCompute()'s CloudParams block; MUST match cloud_params.glsl field-for-field (run
    // tools/check_cloud_params.py). Appended, not folded into pad21/pad22 — same rule as every
    // prior growth here.
    uint32_t dbgDisableMask;    // = debugDisableMask; profiling knockout bitmask
    uint32_t showBeamDebugRays; // 0/1 — draw each mirror's live pointing ray (cloud_march.comp)
    float skyGlareVisibility;   // eased sun-glare gate for sat_sky.frag's Milky Way
    float beamMaxRangeM;        // Reflect-Orbital beam render-range cutoff (m)
    float beamSkyGlowGain;      // beam->cloud illumination brightness (shared: sky frag + march)
    float beamGlowBleedGain;    // beam-driven sky-illumination wash gain (sat_sky.frag)
    float beamProximityGlow;    // CPU [0,1] observer-near-a-beam-line wash (sat_sky.frag)
    // Sky background surface brightness (mag/arcsec^2, larger = fainter) the light-pollution dome
    // asymptotes to at its bright end — the "inner city" anchor of the dark-sky exposure gate
    // (shaders/include/darksky.glsl). Reuses the slot that carried mwSuppressEased, the single
    // non-directional Milky Way suppression scalar that gate replaced: same offset, same size, so
    // this is a rename rather than a layout change. Mirror of cloud_params.glsl.
    float darkSkyCityMag;
    float cloudShadowRangeM;    // per-pixel terrain cloud-shadow fade distance (cloud_march.comp)
    float skyScreenW;           // sat_sky.frag render-target width  (skyLowResExtent at renderScale<1,
    float skyScreenH;           // else ctx.swapExtent) — for gl_FragCoord->UV; the point shaders
                                // carry their own screenSizePx (always full-res) in PointDrawPC
    // Zodiacal light, 532 -> 560. See cloud_params.glsl for the full design.
    float zodiacalWidthDeg;     // ecliptic-latitude Gaussian sigma near the sun (degrees)
    float zodiacalOuterFadeDeg; // elongation at which the cone has fully faded (degrees)
    // LOAD-BEARING PAD, not filler. On the zodiacal-light branch these two floats sat at 488/492
    // and the vec4 landed on 496 by luck. Behind the push-constant relief block above they sit at
    // 532/536, leaving 540 — not 16-byte aligned. std140 aligns the vec4 up to 544; glm::vec4 is
    // 4-aligned by default (no GLM_FORCE_ALIGNED_GENTYPES) so C++ would pack it at 540 and every
    // subsequent field would read its neighbour's bytes. The static_assert catches the resulting
    // size change (556 vs 560), but a future permutation at equal size would not — run
    // tools/check_cloud_params.py.
    float pad21;
    glm::vec4 eclipticPoleENU;  // xyz = ENU unit vector to ecliptic north pole, w = zodiacalGain
    // ── Dark-sky twilight term (560 -> 576) ──────────────────────────────────────────────────
    // Sun's own contribution to sky background brightness, for the dark-sky exposure gate in
    // darksky.glsl. Before this, the ONLY solar gate on the Milky Way was nightFactorEffSky's
    // clamp(-sunDirENU.w * 5, 0, 1), which saturates at sin(el) = -0.2 — i.e. it stops suppressing
    // anything once the sun is a mere 11.5 deg below the horizon, less than the end of NAUTICAL
    // twilight. So the Milky Way appeared at full strength while the sky was still visibly bright.
    // It was also completely non-directional, which is the bigger error: real twilight is an arch
    // low on the sun's side of the sky while the antisolar sky is already dark, so the Milky Way
    // should emerge from the east first, not everywhere at once.
    //
    // Appended (not slotted into an existing pad) per this header's own guidance. 3 fields + pad23:
    // std140 rounds the block up to a 16-byte multiple while C++ would pack it at 572, so the pad
    // is LOAD-BEARING exactly like pad21 above. Do not reuse it.
    float darkSkyTwilightMag0;   // sky surface brightness with the sun ON the horizon (mag/arcsec^2)
    float darkSkyTwilightEndDeg; // solar depression at which the sky reaches pristine (18 = the real
                                 // end of astronomical twilight); the single best "how long after
                                 // sunset does the Milky Way come out" knob
    float darkSkyTwilightAniso;  // magnitudes the ANTISOLAR sky is darker than the solar side
    float oceanMwReflGain;       // gain on the Milky Way's reflection in the ocean surface
                                 // (sat_sky.frag). Claimed the alignment pad this block was
                                 // appended with — a real float either way, so the 16-byte
                                 // rounding it exists for is unchanged.
    // ── Environment probes (576 -> 592) — see cloud_params.glsl ─────────────────────────────
    glm::vec4 envMainObsDir; // xyz = the main observer's ECEF up (obsDir): the ENU frame the Milky
                             // Way / zodiacal bases are in, for sat_sky.frag's SKY_ENV variant
    // ── Procedural terrain detail (592 -> 656) — see cloud_params.glsl / terrain_detail.glsl ──
    glm::vec4 terrainAnchorRel;
    glm::vec4 terrainAnchorCell;
    float terrainDetailStrength;
    float terrainDetailAmpM;
    float terrainDetailGain;
    float terrainDetailErode;
    float terrainShadowStrength;
    float terrainMaterialStrength;
    float terrainDebugView;
    float terrainTextureStrength; // close-up material textures (terrain v2 P3, sat_sky.frag), 0 = off
    glm::vec4 terrainObsTexel; // xy = integer, zw = fraction of the observer's DEM texel coordinate
    glm::vec4 terrainErosion;  // x = strength, y = branching (tdErosion); z = night sky light, w = sky light (terrain v2)
    // ── Satellite ocean-glint reflection (688 -> 704) — see cloud_params.glsl ────────────────
    // The mirror-flare glints sat_flare.comp appends to OceanGlintBuf and sat_sky.frag composites
    // onto the sea surface. 2 fields + 2 pads: std140 rounds the block up to a 16-byte multiple
    // while C++ would pack it at 696, so the two fields after the pair keep it 16-byte padded. They
    // were pads (oceanGlintPad0/1) until the procedural city lights claimed them IN PLACE (2026-09-29):
    // renamed, same position, every offset unchanged — keep them exactly here.
    float oceanGlintGain;    // multiplier on the whole glint contribution (0 = no flare glints at all)
    float oceanGlintMinFlux; // minimum effectFlare (an OceanGlintBuf entry's .w) that draws at all
    float cityLightsStrength; // procedural city street lights (.plans/CITIES_PLAN.md), 0 = the old detail texture
    float cityRoadsStrength;  // the major roads (sky binding 28), x their share of the lights
    // -- Clouds v2 switch (704 -> 720), .plans/CLOUDS_V2_PLAN.md ------------------------------------
    // 1 = v2 draws the low/mid clouds: cloud_march.comp reads cloud_v2_resolve.comp's result instead
    // of running cloudMarchCS, sat_sky.frag skips flat layer 0, beam_self_march.comp and the ground
    // shadow use the v2 field. The pads are std140's rounding, load-bearing like the ones above.
    float sunCloudT;   // review 22 (was cloudsV2): the Sun disc's cloud transmittance from the eye
    float exposureScale;     // 2^(cv2ExposureEV)
    float highlightRolloff;  // cv2HighlightRolloff (sat_sky.frag's tonemap shoulder)
    float whiteBalance;      // cv2WhiteBalance (sat_sky.frag, before the tonemap)
    glm::vec4 cityParams;    // x twinkle rate, y ground glitter share under the sprites, z sprite reach (m), w sprite start (ground m per pixel)
    glm::vec4 oceanState;    // xy world offset wrapped into the wave period (m), z sea state from weather, w whitecaps (736 -> 752)
    glm::vec4 auroraSheets;  // x strength, y spacing (deg), z crisp share, w folds (752 -> 768)
    glm::vec4 taaJitter;     // xy sky TAA jitter (pixels), z orbit grade weight, w the Sun's true angular radius (rad) (768 -> 784)
    glm::vec4 moonCenter;    // xyz the Moon's centre from the Earth's, observer ENU (km); w its angular radius (rad)
    glm::vec4 moonMisc;      // x Sun fraction the observer sees past the Moon, y solar / z lunar eclipse possible, w distance (km) (784 -> 816)
    glm::vec4 cityLod;       // footprint (m/px) where x posts, y the street grid, z major roads, w the street layout end (816 -> 832)
    glm::vec4 cityLod2;      // x streets' share close up, y their share as they fade; z "Sea wave range fade" (review 28); w "Storm sea trails" (832 -> 848)
    glm::vec4 seaTune;       // review 24: x far sea ripple, y sea warp, z sea warp detail, w ripple size (px) (848 -> 864)
    // The auroral oval (864 -> 928, 2026-10-03): AuroraGpu (SpaceWeather.h), see cloud_params.glsl.
    glm::vec4 auroraMidnight; // xyz toward magnetic midnight (ECEF), w Kp
    glm::vec4 auroraOval;     // colatitudes (deg): equatorward midnight / noon, poleward midnight / noon
    glm::vec4 auroraSub;      // substorm: intensity, cos / sin MLT angle, 1 / half-width^2
    glm::vec4 auroraOval2;    // x brightness, y max lit colatitude, z ripple (deg), w substorm poleward push (deg)
    glm::vec4 auroraStorm;    // x storm features, y blue ray chance, z arc systems, w storm red glow (928 -> 944)
    glm::vec4 nightKeyENU;    // the night key light (Moon + supernova; xyz dir ENU, w its light vs the full Moon; w 0 = none:
                              // sat_sky.frag uses the Moon). Was auroraSky, unread since 2026-10-28 (944 -> 960)
    // Launch pads (960 -> 1552, 2026-10-19, SatelliteSim::launchFillPadParams): the nearest kMaxPadsGpu pads in the main
    // observer's ENU frame from its SEA-LEVEL point (terrain_detail.glsl's q). padInfo: x count, y "Launch pad graphics"
    // (0 = off), z night (1 dark .. 0 day), w how many of them (the nearest first) flatten the terrain.
    glm::vec4 padInfo;
    glm::vec4 padQ[kMaxPadsGpu];   // xyz pad centre, w the city exclusion radius (m)
    glm::vec4 padU[kMaxPadsGpu];   // xyz the layout's heading axis (horizontal at the pad), w kind (0 generic, 1 Falcon, 2 Starship)
    glm::vec4 padAux[kMaxPadsGpu]; // x plateau height (m above sea level), y seed, z flattened radius (m), w landing zone 1/0
    // Coasts around launch sites (1552 -> 11040, 2026-10-20, launchFillPadParams; data/launch_coasts.json): the nearest
    // kMaxCoastPatchesGpu patches, each a kCoastGridN^2 byte grid of the shore's signed distance, in the q frame.
    glm::vec4 coastInfo;                             // x patch count
    glm::vec4 coastC[kMaxCoastPatchesGpu];           // xyz centre, w half-width R (m)
    glm::vec4 coastE[kMaxCoastPatchesGpu];           // xyz east axis
    glm::vec4 coastN[kMaxCoastPatchesGpu];           // xyz north axis
    glm::vec4 coastCol[kMaxCoastPatchesGpu];         // rgb land colour, w land floor (m)
    glm::uvec4 coastG[kMaxCoastPatchesGpu * kCoastGridN * kCoastGridN / 16]; // the grids' bytes
    // Tropical cyclones at sea (11040 -> 11104, 2026-10-23, SatelliteSim::weatherFillSeaParams): the two nearest.
    glm::vec4 seaCycA[2];   // xyz Earth-fixed centre, w radius of maximum wind (m; 0 = none)
    glm::vec4 seaCycB[2];   // x vmax (m/s), y the wind field's reach (m)
    // Storm surge (11104 -> 11184, 2026-10-23, weatherFillSeaParams; terrain_detail.glsl tdSurgeAt), per seaCyc* entry.
    glm::vec4 surgeA[2];    // xyz the motion (Earth-fixed unit tangent), w the peak rise at the coast (m; 0 = none)
    glm::vec4 surgeB[2];    // xyz the surge side (right of the motion in the north, left in the south), w cos(reach)
    glm::vec4 surgeInfo;    // x 1 = any surge, y the largest peak (m), z the Sun's sea glitter gain
    // Wildfire burn scars (11184 -> 12224, 2026-10-24, SatelliteSimWildfire.cpp weatherFillFireParams): the nearest
    // kMaxFiresGpu fires' burnt ellipses in the q frame (cloud_params.glsl for the fields).
    glm::vec4 fireInfo;                    // x count
    glm::vec4 fireC[kMaxFiresGpu];         // xyz centre (sea-level sphere), w semi-axis along the spread (m)
    glm::vec4 fireU[kMaxFiresGpu];         // xyz spread axis, w half width (m)
    glm::vec4 fireV[kMaxFiresGpu];         // xyz across axis, w seed
    glm::vec4 fireAux[kMaxFiresGpu];       // x freshness, y 1 / edge softness (m)
    glm::uvec4 fireK[kMaxFiresGpu * 4];    // fingers' radial factor at 64 angles, 4 bytes a uint
    // The Moon's body frame (12224 -> 12256, 2026-10-27, updatePositions: the IAU rotation model, so the optical
    // libration is real), in the observer's ENU: moonPole xyz the lunar north pole, w "Moon relief" (x the LOLA
    // heights; 0 = the smooth sphere); moonPrime xyz the body x axis (0 deg longitude on the equator), w "Moon shadows".
    glm::vec4 moonPole;
    glm::vec4 moonPrime;
    // The Milky Way's look (12256 -> 12288, 2026-10-28, sat_sky.frag "Milky Way"): mwLook x the gain under the air
    // ("Milky Way gain (ground)"), y above it ("... (space)"), z the contrast above the air (a power on the band's
    // luminance about 0.01, 1 = the photograph's), w 1 = the baked RGBA16F panorama is bound (A = the denoised glow).
    // mwLook2 x "Milky Way star clouds" (the share of the light in sub-texel points when zoomed in), y "Milky Way night
    // vision" (the ground's blue-grey: rod vision), z "Milky Way zoom detail" (the dust's sub-texel structure), w an
    // input pixel's angle (rad).
    glm::vec4 mwLook;
    glm::vec4 mwLook2;
    // The zoomed star clouds' tunables (12288 -> 12336, 2026-10-28, issue 20261028_191142): mwStar0 x / y the input pixel
    // per panorama texel where the zoomed view starts / is complete, z the share of lattice cells holding a point, w a
    // point's sigma (input px); mwStar1 x / y the glow's smoothstep (the faint sky between the star clouds stays smooth),
    // z the brightness distribution's exponent a (w = (1 - a) u^-a), w the lattice cell (input px); mwStar2 x the nebula
    // exclusion (0-1), yzw unused.
    glm::vec4 mwStar0;
    glm::vec4 mwStar1;
    glm::vec4 mwStar2;
};
static_assert(sizeof(GpuCloudParams) == 12336, "GpuCloudParams layout mismatch");

// ── Push constants for sat_orbit.comp ────────────────────────────────────────
// Offsets verified against the push_constant block in sat_orbit.comp.
// Total: 96 bytes.
struct SatOrbitPC
{
    glm::vec4 enuX;               // East  basis in ECI (w unused) — offset 0
    glm::vec4 enuY;               // North basis in ECI (w unused) — offset 16
    glm::vec4 enuZ;               // Up    basis in ECI (w unused) — offset 32
    glm::vec3 sunDirECI;          // unit vector toward sun — offset 48
    float deltaT;                 // simTime - epochT0 (float precision) — offset 60
    glm::vec3 obsECI;             // observer ECI position (meters) — offset 64
    uint32_t satCount;            // total satellite count — offset 76
    uint32_t padMask0;            // offset 80 (was highlightMask: GpuSatTypeHeader::constHighlight since 2026-10-20)
    uint32_t padMask1;            // offset 84 (was enabledMask: GpuSatTypeHeader::constEnabled)
    float simDt;                  // simulated seconds this frame — offset 88
    float elevCutoff;             // sin(Earth-limb angle) — horizon cull threshold (≤ -0.01) — offset 92
    float beamGain;               // Reflect-Orbital ground-beam intensity multiplier — offset 96
    float reflectorLockWindowS;   // offset 100 — 2026-08-06 reversibility rework: fixed-width sim-time
                                  // window (seconds) a TargetedReflector satellite commits to one target
                                  // for. Target IDENTITY is chosen per-window (see sat_orbit.comp), not
                                  // by a persisted per-satellite lock — this is also the extrapolation
                                  // step the shader uses to reach a window's midpoint from `deltaT`/
                                  // `gmstNow` (see those fields below), so it doubles as "windowS".
                                  // Replaces the old rate-limited-slew design's mirrorSlewDegPerSec
                                  // (same push-constant slot, same settings-window slider).
    uint32_t targetCount;         // offset 104 — total loaded reflector targets (reflectorTargetCount).
                                  // sat_orbit.comp scans the full static ECEF set itself now (rotating
                                  // by gmstNow/derived gmstEval on the fly) instead of reading a
                                  // per-frame CPU-compacted night-side buffer — see CLAUDE.md's
                                  // TargetedReflector section. Replaces the old activeTargetCount.
    float minBeamElevSin;         // offset 108 — sin(reflectorMinElevDeg), precomputed on CPU. Candidate
                                  // targets below this local-elevation-at-target angle are rejected
                                  // outright (grazing, not just deprioritized).
    float gmstNow;                // offset 112 — current-frame GMST (rad), for rotating a target's
                                  // static ECEF entry to its LIVE ECI position (the actual aim/beam
                                  // position — see CLAUDE.md). Replaces the old mirrorSnap flag; nothing
                                  // persists across frames anymore, so there is no snap state to force.
    float windowFrac;             // offset 116 — fract(simTimeAbs / reflectorLockWindowS): this
                                  // frame's fractional position within the current lock window, in
                                  // [0,1). sat_orbit.comp uses -windowFrac*reflectorLockWindowS to
                                  // extrapolate deltaT/gmstNow back to the current window's START
                                  // instant (and one window further back for the previous window's),
                                  // exact rather than approximate for the same reason deltaT/gmstNow
                                  // themselves are (see that shader). Replaces the old
                                  // minBeamElevSinRelease — hysteresis is gone; a rate-limited ease
                                  // sized off windowFrac is what smooths a target change instead.
    float mirrorMaxRateDegPerSec; // offset 120 — real angular-rate cap for the closed-form ease
                                  // above: sat_orbit.comp derives an ease duration from the actual
                                  // angle between the previous and current window's targets (at the
                                  // current window's start instant) and this rate, then eases the
                                  // live orientation over that duration — a genuine max attitude
                                  // rate, unlike the old design's fixed-fraction-of-window crossfade
                                  // (2026-08-06 same-day follow-up: satellites were visibly snapping
                                  // to target because that crossfade only covered ONE of several
                                  // transition cases and wasn't derived from real angular distance).
    float flareMitigationTiltRad; // offset 124 — was pad2. Global flare-mitigation tilt angle
                                  // (radians) for AttitudeMode::SunTrackingTilted satellites — see
                                  // that enum value's comment and computeNormal() in sat_orbit.comp.
                                  // Deliberately global rather than per-satellite-type: it's an
                                  // operator policy knob, not orbital geometry, so it belongs beside
                                  // the other live photometry sliders (flareMitigationTiltDeg,
                                  // SatelliteSim.h) rather than growing GpuSatOrbit.
}; // 128 bytes
static_assert(sizeof(SatOrbitPC) == 128, "SatOrbitPC layout mismatch");

// ── Sky camera ────────────────────────────────────────────────────────────────
// Azimuth/elevation look direction in the local ENU frame.
// Right-click to capture mouse; WASD-style look via mouse deltas.
struct SkyCamera
{
    float azDeg = 0.0f;    // azimuth of look direction (0=North, 90=East), degrees
    float elDeg = 30.0f;   // elevation of look direction, degrees
    float fovYDeg = 70.0f; // vertical field of view, degrees
    float sens = 0.12f;    // mouse sensitivity (degrees per pixel)
    bool captured = false;

    // Phase 4: zoom reaches telescope FOVs so a resolvable satellite (the ISS is ~0.014 deg from the
    // ground) can cover enough pixels to draw as a mesh. Zoom is multiplicative so each scroll notch
    // or second of held zoom changes the FOV by the same RATIO at any magnification.
    static constexpr float kMinFovDeg = 0.5f;
    static constexpr float kMaxFovDeg = 120.0f;
    void zoomBy(float factor) { fovYDeg = glm::clamp(fovYDeg * factor, kMinFovDeg, kMaxFovDeg); }
    // Look sensitivity scaled with the FOV below 60 deg, so a pixel of mouse motion moves the view
    // by about the same number of screen pixels when zoomed in (unchanged at 60 deg and wider).
    float lookSens() const { return sens * std::min(1.0f, fovYDeg / 60.0f); }

    // Returns a view matrix that transforms ENU directions into camera space.
    // Camera convention: +X=right, +Y=up, -Z=forward (standard OpenGL).
    glm::mat4 viewMatrix() const
    {
        float az = glm::radians(azDeg);
        float el = glm::radians(elDeg);
        // Forward vector in ENU (x=East, y=North, z=Up)
        glm::vec3 fwd{sinf(az) * cosf(el), cosf(az) * cosf(el), sinf(el)};
        // World up = ENU Up. Fall back to North when near zenith/nadir.
        glm::vec3 worldUp = (fabsf(elDeg) > 88.0f)
                                ? glm::vec3{0.0f, 1.0f, 0.0f}  // North when near zenith
                                : glm::vec3{0.0f, 0.0f, 1.0f}; // ENU Up otherwise
        return glm::lookAt(glm::vec3(0.0f), fwd, worldUp);
    }

    void update(GLFWwindow *win, float dmx, float dmy)
    {
        if (!captured)
        {
            if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS)
            {
                captured = true;
                glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
            }
            return;
        }
        if (glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_RELEASE)
        {
            captured = false;
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            return;
        }
        azDeg += dmx * lookSens();
        elDeg -= dmy * lookSens(); // screen Y down → mouse up = negative dmy = increase el
        elDeg = glm::clamp(elDeg, -89.0f, 89.0f);
    }
};

// ── Fixed orbital parameters (one per satellite, computed once at init) ───────
struct SatOrbit
{
    float raan;           // right ascension of ascending node (radians)
    float incl;           // inclination (radians) — per-satellite so RandomShell can vary
    float u0;             // initial mean argument of latitude (radians)
    uint32_t typeIdx;     // index into satTypes[]
    float altM;           // orbital altitude above surface (meters) — per-satellite
    float tumbleRate;     // rotation rate (rad/s); 0 = not tumbling
    float tumblePhase;    // initial rotation angle (radians)
    glm::vec3 tumbleAxis; // fixed body tumble axis (unit vector in ECI)
    bool alignTerminator; // if true, incl/raan are recomputed from sunDirECI each frame
    float targetTerminatorAngle = 0.0f;
    // Formation ring member (cluster_shape "ring"). raan/incl/u0/altM are the CLUSTER CENTRE's
    // circular orbit; the member flies the bounded Clohessy-Wiltshire (Hill) relative orbit about it,
    // in the centre's radial / along-track / cross-track frame, with theta = u + delta:
    //   x (radial)      =     a cos(theta)        =  P cos u - Q sin u
    //   y (along-track) = -2 a sin(theta)        = -2 (P sin u + Q cos u)
    //   z (cross-track) =  k a cos(theta)        =  k x
    // P = a cos delta, Q = a sin delta. Drift-free (x'' - 2n y' - 3n^2 x = 0, y'' + 2n x' = 0,
    // z'' + n^2 z = 0): a member below the centre runs ahead, one above falls behind, and the whole
    // formation turns once per orbit. Mirrored in sat_orbit.comp and satOrbitStateAt().
    bool formation = false;
    float formP = 0.0f, formQ = 0.0f, formK = 0.0f;
    // ── Precomputed frame-invariant constants (set once in buildOrbits) ────────
    float R_sat = 0.0f;    // kEarthRadius + altM
    float meanMot = 0.0f;  // sqrt(kGM / R_sat^3)
    float cosI = 0.0f;     // cos(incl)
    float sinI = 0.0f;     // sin(incl)
    float cosRaan = 0.0f;  // cos(raan) — valid when !alignTerminator
    float sinRaan = 0.0f;  // sin(raan) — valid when !alignTerminator // TargetedReflector: angle (rad) along the terminator great-circle
    uint32_t constIdx = 0; // index into constellations[] — set by buildOrbits()
                           // that selects the ground target this mirror aims at.
                           // Terminator basis: t1=cross(sunDir,ref), t2=cross(sunDir,t1).
                           // Target = kEarthRadius × (cos(angle)×t1 + sin(angle)×t2).
    int32_t poolSlot = -1; // >= 0: a Kessler pool record (kPool[poolSlot]); its orbit lives there
    int32_t launchSlot = -1; // >= 0: a launch record (a train raising, a retiree deorbiting; SatelliteSimLaunches.cpp)
};

// (2026-10-29, workstream A) satOrbits: the roster and the launch block in a vector; the Kessler pool's entries (one per
// pool slot, after them) are MADE ON READ from the mirror's record (kPool[j], the rule kesslerSetPoolOrbit kept): a
// stored copy was ~100 B a slot, ~480 MB at the pool's 5M slots, a third copy of the pool. A pool entry read through
// operator[] lives in a small per-thread ring (the last 512 reads): read what you need from it, never keep the
// reference. Writes go to the roster / launch block only (a pool entry written is a copy, lost).
class SatOrbitTable
{
  public:
    void bindPool(const std::vector<kessler::PoolRec> *pool, size_t poolBase)
    {
        pool_ = pool;
        poolBase_ = poolBase;
    }
    size_t size() const { return (pool_ && base_.size() == poolBase_) ? poolBase_ + pool_->size() : base_.size(); }
    bool empty() const { return size() == 0; }
    const SatOrbit &operator[](size_t i) const { return i < base_.size() ? base_[i] : poolEntry(i); }
    SatOrbit &operator[](size_t i) { return i < base_.size() ? base_[i] : const_cast<SatOrbit &>(poolEntry(i)); }
    void resize(size_t n) { base_.resize(pool_ && n > poolBase_ ? poolBase_ : n); } // (the pool's part follows kPool)
    void reserve(size_t n) { base_.reserve(pool_ && n > poolBase_ ? poolBase_ : n); }
    size_t capacity() const { return base_.capacity(); }
    void clear()
    {
        base_.clear();
        pool_ = nullptr; // (a new roster: kesslerRestart binds the pool again)
    }
    void push_back(const SatOrbit &o) { base_.push_back(o); }
    SatOrbit &back() { return base_.back(); }
    const SatOrbit &back() const { return base_.back(); }
    SatOrbit *data() { return base_.data(); }
    const SatOrbit *data() const { return base_.data(); }
    std::vector<SatOrbit>::iterator begin() { return base_.begin(); }
    std::vector<SatOrbit>::iterator end() { return base_.end(); }
    std::vector<SatOrbit>::const_iterator begin() const { return base_.begin(); }
    std::vector<SatOrbit>::const_iterator end() const { return base_.end(); }
    // The stored part (the roster and the launch block): for functions that read the roster.
    operator const std::vector<SatOrbit> &() const { return base_; }
    const std::vector<SatOrbit> &stored() const { return base_; }
    static SatOrbit poolOrbit(const kessler::PoolRec &r, uint32_t j)
    {
        SatOrbit o{};
        o.typeIdx = r.type;
        o.constIdx = r.constIdx;
        o.incl = (float)r.incl;
        o.altM = (float)(r.a - satphot::kEarthRadiusM);
        o.R_sat = (float)r.a;
        o.meanMot = r.n;
        o.tumbleRate = r.tumbleRate;
        o.tumblePhase = r.tumble0;
        o.tumbleAxis = glm::vec3(kessler::poolTumbleAxis(j));
        o.poolSlot = (int32_t)j;
        return o;
    }

  private:
    std::vector<SatOrbit> base_;
    const std::vector<kessler::PoolRec> *pool_ = nullptr;
    size_t poolBase_ = 0;
    const SatOrbit &poolEntry(size_t i) const
    {
        thread_local SatOrbit ring[512];
        thread_local unsigned next = 0;
        SatOrbit &o = ring[next++ & 511u];
        const size_t j = i - poolBase_;
        o = (pool_ && i >= poolBase_ && j < pool_->size()) ? poolOrbit((*pool_)[j], (uint32_t)j) : SatOrbit{};
        return o;
    }
};

// ── SatelliteSim ──────────────────────────────────────────────────────────────
namespace launchsim
{
struct LBatch; // SatelliteSimLaunches.cpp
}

class SatelliteSim : public Simulation
{
public:
    const char *name() const override { return "SAT LIGHT SIM"; }

    void init(VulkanContext &ctx) override;
    void onResize(VulkanContext &ctx) override;
    // Startup graphics chooser (2026-10-06, SatelliteSimBootChooser.cpp; CLAUDE.md "Startup graphics
    // chooser"): App asks before init(); options 0 Full graphics, 1 Planetarium, 2 Potato.
    bool prepareBootChooser(VulkanContext &ctx, BootChooserSpec &spec) override;
    void setBootChoice(int option, bool askAgain) override;
    void recordCompute(VkCommandBuffer cmd, VulkanContext &ctx, float dt) override;
    SatDrawPC buildSkyDrawPC(VulkanContext &ctx);     // sky background (recordPrePass + recordDraw Pass 1)
    PointDrawPC buildPointDrawPC(VulkanContext &ctx); // satellite / star / planet / trail point draws
    void recordPrePass(VkCommandBuffer cmd, VulkanContext &ctx, float dt, uint32_t imgIdx) override;
    // renderPassLoad when the background was drawn before the pass (renderScale < 1, or the sky TAA path).
    VkRenderPass activeRenderPass(VulkanContext &ctx) override
    {
        return (renderScale < 0.999f || skyTaaUsedThisFrame) ? ctx.renderPassLoad : ctx.renderPass;
    }
    void recordDraw(VkCommandBuffer cmd, VulkanContext &ctx, float dt) override;
    void buildUI(float dt, UIRenderer &ui) override;
    void setAudio(AudioSystem *audio) override;
    void setWindow(GLFWwindow *w) override { win = w; }
    VkClearValue clearColor() const override { return {{{0.0f, 0.0f, 0.015f, 1.0f}}}; }
    // NEW-7: numeric caps (Off/Cap30/Cap60/Cap120 all run MAILBOX/IMMEDIATE present, uncapped
    // submission) need App::mainLoop to pace them manually; VSync (FIFO) paces itself.
    float targetFpsCap() const override
    {
        switch (fpsCapMode)
        {
        case FpsCapMode::Cap30:
            return 30.0f;
        case FpsCapMode::Cap60:
            return 60.0f;
        case FpsCapMode::Cap120:
            return 120.0f;
        default:
            return 0.0f; // Off, VSync
        }
    }
    bool consumeSwapchainRebuildRequest() override
    {
        if (!fpsCapSwapchainRebuildPending)
            return false;
        fpsCapSwapchainRebuildPending = false;
        return true;
    }
    // UC6: see Simulation.h for the calling convention (peek before ui.record(), record the copy
    // after the render pass ends, finalize at the top of the next frame).
    bool wantsCleanScreenshot() const override { return screenshotRequested && !screenshotIncludeUI; }
    void recordScreenshotCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image) override;
    void finalizeScreenshot() override;
    // Video export (SatelliteSimVideo.cpp): the clean frame is copied before the UI while recording without it.
    bool wantsPreUiCopy() const override;
    void recordPreUiCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image) override;
    // UC6: shared by KB_SCREENSHOT (dispatchKeyAction) and the left HUD panel's camera button —
    // builds screenshotPath and sets screenshotRequested, or no-ops if a capture is already in
    // flight (copy pending or still encoding). withUi: the shot keeps the interface (the screenshot's own
    // toast, the issue-report note bar and window are left out: shotHidesOverlays).
    void requestScreenshot(bool withUi = false);
    // A player's screenshot or issue-report shot WITH the UI is being taken this frame: buildUI leaves out the
    // things that only exist because of the shot (its toast, the report note bar, the Issue reports window), so
    // the image shows what the player was looking at. Harness captures (`capture ui=1`) never set it.
    bool shotHidesOverlays() const { return screenshotRequested && screenshotIncludeUI && screenshotHideOverlays_; }
    // HQ photo (review 13): render photoScaleSetting x the window's resolution offscreen (App switches to
    // VulkanContext's photo target while photoScaleRequest() > 0), the clouds at full rate and time
    // paused, let photoSettleFrames frames accumulate (cloud history, sky TAA), then save that frame
    // as screenshots/satlight_hq_<time>.png and restore everything. Point and glare sizes are scaled by
    // the factor so stars and satellites keep their on-screen look; the bloom's radius is not.
    void requestPhoto();
    void updatePhoto(VulkanContext &ctx);
    void endPhoto();
    uint32_t photoScaleRequest() const override { return photoScaleActive; }
    float photoScaleSetting = 1.0f;   // display.photo_scale: the photo's resolution in window sizes (1-4)
    float photoSettleFrames = 48.0f;  // display.photo_frames: frames accumulated before the capture
    uint32_t photoScaleActive = 0;
    int   photoState = 0;             // 0 idle, 1 settling, 2 captured (restoring once read back)
    int   photoFrame = 0;
    std::string photoPathOverride;    // harness `photo`: the capture path instead of screenshots/
    bool  photoSavedPaused = false;
    float photoSavedFullRateKm = 0.0f, photoSavedSparse = 0.0f;
    float photoSavedPointSigma = 0.0f, photoSavedPointMax = 0.0f, photoSavedGlareSize = 0.0f;
    float photoSavedGodrayFroxels = 1.0f;   // the photo traces the god rays per pixel (the froxels' direct path)
    void cleanup(VkDevice device) override;
    void onKey(GLFWwindow *w, int key, int action) override;
    void onCursorPos(GLFWwindow *w, double x, double y) override;
    // Automation harness (docs/HARNESS.md, SatelliteSimHarness.cpp).
    float frameDt(float realDt) override;
    bool wantsQuit() const override { return harnessQuit_; }
    void onChar(GLFWwindow *w, unsigned int codepoint) override;
    // The console, a focused text field (Esc cancels the edit) and a playing / exporting cinematic (Esc stops it)
    // keep Esc from quitting the app.
    // (Esc closes the report bar; since 2026-10-21 it also cancels a key rebind that is listening instead of quitting)
    bool capturesKeyboard() const override
    {
        if (consoleOpen_ || textEdit_.id != 0 || cineActive() || reportBarOpen_)
            return true;
        for (const auto &kb : keybindings)
            if (kb.listening || kb.listeningPad)
                return true;
        return false;
    }

private:
    // ── SSBOs ─────────────────────────────────────────────────────────────────
    // satVisibleBuf, satOrbitBuf and satTypeBuf are sized to the loaded satellite/type counts, not
    // to MAX_SATELLITES — see createSatBuffers(), which runs once initConstellation() knows them.
    VkBuffer satVisibleBuf = VK_NULL_HANDLE; // device-local; sat_orbit.comp → sat_flare.comp (in place) → vertex
    VkDeviceMemory satVisibleMem = VK_NULL_HANDLE;
    VkBuffer satTypeBuf = VK_NULL_HANDLE; // host-visible/coherent: GpuSatTypeHeader + GpuSatType[]
    VkDeviceMemory satTypeMem = VK_NULL_HANDLE;
    void *satTypeMapped = nullptr;
    // Phase 1b compact visible list: slot → satellite index, and the list header / indirect args.
    VkBuffer satVisibleIdxBuf = VK_NULL_HANDLE; // device-local, uint per compact slot
    VkDeviceMemory satVisibleIdxMem = VK_NULL_HANDLE;
    VkBuffer satListBuf = VK_NULL_HANDLE; // device-local GpuSatListHeader (also INDIRECT_BUFFER)
    VkDeviceMemory satListMem = VK_NULL_HANDLE;
    VkBuffer pointStyleBuf = VK_NULL_HANDLE; // host-coherent UBO: GpuPointStyle, rewritten every frame
    VkDeviceMemory pointStyleMem = VK_NULL_HANDLE;
    void *pointStyleMapped = nullptr;
    VkBuffer satLobeBuf = VK_NULL_HANDLE; // host-visible/coherent: every model type's GpuSatLobe[] (Phase 3)
    VkDeviceMemory satLobeMem = VK_NULL_HANDLE;
    void *satLobeMapped = nullptr;
    // Phase 3b: every model type's occluders and lobe sample points, packed like satLobeBuf.
    VkBuffer satOccluderBuf = VK_NULL_HANDLE;
    VkDeviceMemory satOccluderMem = VK_NULL_HANDLE;
    void *satOccluderMapped = nullptr;
    VkBuffer satLobeSampleBuf = VK_NULL_HANDLE;
    VkDeviceMemory satLobeSampleMem = VK_NULL_HANDLE;
    void *satLobeSampleMapped = nullptr;

    // ── Satellite picking / selection tracking ────────────────────────────────
    // pickedVisibleBuf mirrors the 80-byte GpuSatListHeader every frame (host-visible, mapped
    // once like glowBuf): its `selected` record lets buildUI reproject the selected satellite
    // without reading back the full (device-local) satVisibleBuf, and its `count` is the real
    // visible-satellite count. Only a click (pickSatelliteAt) reads the list itself.
    // See pickSatelliteAt/projectSkyDirToScreen.
    VkBuffer pickedVisibleBuf = VK_NULL_HANDLE;
    VkDeviceMemory pickedVisibleMem = VK_NULL_HANDLE;
    void *pickedVisibleMapped = nullptr;
    int selectedSatIndex = -1;        // index into satOrbits[]/satVisibleBuf; -1 = no selection
    glm::vec3 lastPickedSkyDir{0.0f}; // previous frame's ENU sky direction for the selection
    float lastPickedFlare = 0.0f;     // previous frame's flareIntensity for the selection (>0 = on screen)
    // Cached info text, reformatted only when selectedSatIndex changes (see formatSelectedSatInfo).
    // Separate per-line buffers, not one multi-line string — Clay/UIRenderer text draws a single
    // line per CLAY_TEXT call with no embedded-newline support.
    // buildSelectedSatPanel renders only [0] (name) and [1] (type) for a satellite — its orbit rows
    // [2..6] are shown by the satellite window instead, as the ORBIT section's `viewerOrbitValue`
    // (see updateViewerObserverInfo).
    // A PLANET selection still renders the whole array: it has no model window to move into.
    // 7th slot (session follow-up): optional "Power output" line, filled only for satellites whose
    // primary surface uses AttitudeMode::SunTrackingTilted (see formatSelectedSatInfo); left empty
    // for everything else, including planets — formatSelectedPlanetInfo never touches
    // planetInfoLine[6].
    static constexpr int kSelInfoLines = 7;
    // 160 wide: a rocket's or a train member's label runs past 40 characters (it was cut mid-word).
    static constexpr int kSelInfoLineLen = 160;
    char selInfoLine[kSelInfoLines][kSelInfoLineLen] = {};
    char planetInfoLine[kSelInfoLines][kSelInfoLineLen] = {}; // same shape, filled by formatSelectedPlanetInfo

    // ── Photometry readout + GPU parity (benchmarking M2) ─────────────────────
    // The exact inputs the last sat_orbit.comp dispatch was given (captured where SatOrbitPC is
    // built). Next frame, updateSelectedPhotometry() re-evaluates the selected satellite with the
    // CPU evaluator (SatPhotometry) at THOSE inputs and compares its raw flux with the value
    // sat_flare.comp mirrored into GpuSatListHeader::selectedRawFlux — so the gap measures only
    // GPU-vs-CPU arithmetic (float orbit baking, float math), not a frame of motion.
    struct ParityInputs
    {
        bool valid = false;
        int satIdx = -1;
        double tAbs = 0.0; // sim time of that dispatch, seconds since J2000
        glm::vec3 sunDirECI{0.0f};
        glm::vec3 obsECI{0.0f};
        float flareTiltRad = 0.0f;
        float brightnessScale = 1.0f;
        float mirrorBoost = 1000.0f;
        bool occlusionOn = true;
    };
    ParityInputs parityPending;
    // The magnitude readout the selection panel shows ("Mag -1.23  (1000 km -1.10)"). It used to be
    // three lines (magnitude / range+phase / the GPU-vs-CPU parity text) — the range and phase moved
    // to the info window's OBSERVER box, and the parity LINE was dropped from the UI (2026-09-24) with
    // the whole selector-window readout; the parity CHECK and its mismatch log below stay.
    static constexpr int kSelPhotLines = 2; // magnitude; its formation's combined magnitude
    char selPhotLine[kSelPhotLines][48] = {}; // rebuilt every frame
    bool selParityWarn = false;               // parity gap above kParityWarnMag (drives the log's severity)
    bool selParityValid = false;              // the last check's numbers (harness `state` selection.parity)
    double selParityDmag = 0.0, selParityRangeGpuM = 0.0, selParityRangeCpuM = 0.0;
    static constexpr double kParityWarnMag = 0.02;
    int parityLogCooldown = 0;                // frames until the next mismatch may be logged
    void updateSelectedPhotometry();
    // The selected satellite's orbit exactly as uploadSatOrbits() bakes it (floats promoted, SSO
    // RAAN anchored at sim start) — what the parity readout and the trace evaluate.
    SatOrbitElems orbitElemsOf(const SatOrbit &orb) const;
    SatOrbitElems rosterElemsOf(const SatOrbit &orb) const; // the plain roster orbit (no Kessler state)
    // The ground-site mirror aim as sat_orbit.comp sees it (the loaded reflector targets + the lock
    // window / slew / elevation settings), for the CPU evaluator: satGroundSiteIdeal().
    SatGroundSiteAim groundSiteAim() const;

    // ── Magnitude trace (benchmarking M9) ─────────────────────────────────────
    // "Trace pass" in the selected-satellite panel: the CPU evaluator over the selection's current
    // (or next) pass, plotted in its own window and exportable as a self-describing CSV that
    // SatModelTool --replay-trace re-runs row for row (SatTrace.h). Computed once per click, not per
    // frame; only the "now" marker moves.
    static constexpr int kTraceSamples = 400;
    WindowChrome traceChrome;
    bool hovTraceClose = false, hovTraceExport = false, hovTraceRetrace = false, hovSelTraceBtn = false;
    SatTraceSetup traceSetup;
    std::vector<SatTraceRow> traceRows;
    bool traceValid = false;
    double traceT0 = 0.0, traceT1 = 0.0;
    float traceMagBright = 0.0f, traceMagFaint = 0.0f; // plot y range (top / bottom), magnitudes
    std::vector<float> tracePlotX, tracePlotMag, tracePlotMagAbove, tracePlotPhase;
    float traceNowX[2] = {}, traceNowY[2] = {0.0f, 1.0f};
    static constexpr int kTraceGridLines = 12; // whole-magnitude grid lines
    float traceGridX[2] = {0.0f, 1.0f};
    float traceGridY[kTraceGridLines][2] = {};
    float traceNowTickX[2] = {}, traceNowTickY[2] = {}; // short tick at the current magnitude
    UIPlotSeries traceSeries[5 + kTraceGridLines];
    UIPlot tracePlot;
    bool tracePassFound = false;
    char traceTitle[96] = {};
    // Axis tick labels, positioned over the plot each frame from its last laid-out size.
    static constexpr int kTraceMagTicks = kTraceGridLines + 2, kTracePhaseTicks = 5, kTraceTimeTicks = 5;
    int traceMagTickCount = 0;
    float traceMagTickFrac[kTraceMagTicks] = {};
    char traceMagTickBuf[kTraceMagTicks][8] = {};
    char traceTimeTickBuf[kTraceTimeTicks][12] = {};
    char traceStatus[200] = {};     // why there is no trace, or that the window is not a pass
    // The pass's numbers (the window's PASS tiles): peak apparent magnitude (NAN = dark the whole pass), when
    // and at what phase, the pass's length and highest elevation.
    double tracePeakMag = NAN, tracePeakT = 0.0, tracePeakPhase = 0.0, traceMaxEl = 0.0;
    int tracePassS = 0;
    char traceTileBuf[8][32] = {};  // the tiles' values, formatted every frame the window is open
    char traceExportStatus[160] = {}; // last export result (kept across live retraces)
    // Live mode: retrace at up to kTraceLiveHz while the window is open, whenever the result would
    // differ — the observer moved, the selection changed, the pass ended, or a photometry input
    // (extinction, flare tilt, occlusion) changed. traceRetraceMs shows what one retrace costs.
    static constexpr double kTraceLiveHz = 10.0;
    bool traceLive = true;
    bool hovTraceLive = false;
    std::chrono::steady_clock::time_point traceLastRetrace{};
    double traceRetraceMs = 0.0;
    bool traceStale() const; // would a retrace now give a different trace?
    void computeSelectedTrace();
    void exportTrace();
    // Why a type's magnitude can't be measured by the CPU evaluator (legacy type, ground-site aim),
    // or nullptr when it can. Shared by the trace and the bulk export.
    const char *photometryUnsupportedReason(const SatelliteType &type) const;

    // ── Bulk export (benchmarking M10) ────────────────────────────────────────
    // Settings → Photometry: every instant, at a fixed cadence, when a satellite of the chosen
    // source satisfies SatBench's sampling constraints for the current observer, written in SatBench's
    // own samples CSV schema (runBulkExport / writeBenchSamplesCsv, SatBench.h) on a worker thread
    // over copies of everything it reads. `SatModelTool --summarize-samples` reads the result.
    int bulkSource = -1;     // -1 = the selected satellite, else a constellation index
    int bulkWindowIdx = 0;   // kBulkWindowDays[]
    int bulkCadenceIdx = 1;  // kBulkCadenceS[]
    static constexpr double kBulkWindowDays[2] = {1.0, 7.0};
    static constexpr double kBulkCadenceS[3] = {10.0, 30.0, 60.0};
    std::thread bulkThread;
    std::atomic<float> bulkProgress{0.0f};
    std::atomic<bool> bulkCancel{false};
    std::atomic<bool> bulkRunning{false};
    std::mutex bulkMutex;
    std::string bulkResult;  // the worker's final message (guarded by bulkMutex)
    char bulkStatus[320] = {};
    char bulkSourceBuf[96] = {}, bulkWindowBuf[32] = {}, bulkCadenceBuf[32] = {}, bulkProgressBuf[64] = {};
    bool hovBulkSource = false, hovBulkWindow = false, hovBulkCadence = false, hovBulkGo = false;
    void startBulkExport();
    void buildBulkExportRows(const UIInput &inp, UIRenderer &ui);
    void buildTraceWindow(const UIInput &inp, UIRenderer &ui);
    // "Trace pass" in the selection UI (icon + label).
    void buildTraceButton(const UIInput &inp, UIRenderer &ui, int idx);
    // The one place the selection panel's and the out-of-view chip's action buttons are drawn: the icon
    // alone, tooltip = the button's name. `id` must be unique among the buttons drawn this frame (the
    // panel and the chip each pass their own); `name` must outlive the frame (Clay stores the pointer
    // for the tooltip) — all callers pass a literal.
    bool buildSelActionButton(const UIInput &inp, UIRenderer &ui, int id, const char *name, int iconIdx,
                              bool on, bool &hov);

    // ── Phase 4: satellite mesh renderer + model viewer (.plans/SAT_RENDERER_PHASE4.md) ──────────
    // "VIEW" on a constellation row (or "Info" on a selected satellite — it was "View model" until the
    // window grew an orbit/brightness readout) opens a window showing that
    // type's geometry model, rendered by SatMeshRenderer into an offscreen image the UI draws
    // (UIImage). The model sits at its constellation's altitude above the observer's ground point,
    // posed by its attitude law, lit by the sun (Studio: fixed 35 deg over its horizon; Live: the sim's
    // real sun, including Earth's shadow), earthshine and moonlight, with the Earth below reflected in
    // its surfaces. Drag to orbit, scroll to zoom.
    SatMeshRenderer meshRenderer;
    bool meshRendererInit = false;
    // TWO windows, one subject (2026-09-24): `infoChrome` is the satellite window — the small 4:3
    // render plus the collapsible sections — and `viewerChrome` is the 3D view POPPED OUT of it by the
    // Maximize chip. Both share every member below and the one offscreen target, sized to whichever
    // view is larger (updateViewerView), so the smaller one shows a crop instead of stretching.
    WindowChrome infoChrome;
    WindowChrome viewerChrome;
    int viewerType = -1;          // satTypes index shown
    int viewerSatIndex = -1;      // >= 0: track this satellite (its real position/attitude), else placed
                                  // at viewerAltM above the observer
    float viewerAltM = 550000.0f; // altitude it is placed at
    char viewerTitle[96] = {};
    // SATELLITE section: type, model id, size across, mesh (triangles, parts).
    static constexpr int kViewerSatRows = 4;
    char viewerSatValue[kViewerSatRows][64] = {};
    float viewerYawDeg = 35.0f, viewerPitchDeg = 18.0f;
    float viewerDist = 0.0f; // m from the model's centre; 0 = frame it on the next render
    // The render target's width / height: the projection's aspect, and what the two views crop.
    float viewerAspect = 1.2f;
    // The info window's render band height (fixed 4:3): set with the crops, used by the builder as its
    // FIXED height so the band and the target can never disagree.
    float viewerBandH = 0.0f;
    // Each view's sub-rect of the target: the popped-out view samples all of it, the small 4:3 band the
    // centred crop (see UIImage's u0..v1).
    float viewerUvMini[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    float viewerUvPop[4] = {0.0f, 0.0f, 1.0f, 1.0f};
    bool viewerSpin = true, viewerStudioLight = false, viewerSunlitPose = true, viewerDetail = true;
    bool viewerShadows = true, viewerReflections = true;
    // Glare on the viewer's glints (SatMeshRenderer::recordViewerGlare, 2026-09-26), and the ground
    // observer's effectFlare per unit of intensity per unit irradiance that scales it (0 = none), set by
    // updateViewerObserverInfo at 10 Hz.
    bool viewerGlare = true;
    double viewerFlarePerI = 0.0;
    bool viewerDragging = false;
    uint32_t viewerImageId = 0;
    UIImage viewerImageMini, viewerImagePop;
    bool hovViewerClose = false, hovInfoClose = false, hovViewerBtn[12] = {}, hovSelInfoBtn = false;
    // Title-bar Select / Go to icons: one hover pair per window, so the two title bars don't fight
    // over it (buildViewTitleIcons picks by `popout`).
    bool hovViewerTitleBtn[2] = {}, hovInfoTitleBtn[2] = {};
    // The view-preset chips over each render: [row][chip], row 0 = the info window's small view.
    static constexpr int kViewChipRows = 2;
    bool hovViewChip[kViewChipRows][8] = {};
    bool viewerMarkers = true; // the observer / ground-site markers
    // The info window's collapsible sections (buildInfoWindow). Satellite / Orbit / Photometry start
    // OPEN — those are what the window is for; the rest start collapsed. Open state is session-only,
    // like the Clouds tab's section state (buildCloudSliderSections), not a preference.
    // 2026-10-19 (the user: "the satellite tab is listed first and is the least relevant"): STATUS, BRIGHTNESS,
    // SKY POSITION, ORBIT open; the technical MODEL (type, model id, size, triangles), VIEW (camera + render) and
    // CHECK collapsed at the bottom. The stat tiles and the ACTIONS (Trace / Go to / Track / Explode) sit
    // above the list and never scroll away.
    // 2026-10-20: MISSION (kInfoMission, drawn second, after STATUS; only for an object with a launch timeline:
    // missionSubjectOfIndex) — the milestones with T+ / UTC, the current one highlighted, per-row skips, Prev / Next.
    enum InfoSect { kInfoStatus = 0, kInfoBright, kInfoSky, kInfoOrbit, kInfoModel, kInfoView, kInfoCheck, kInfoMission };
    static constexpr int kInfoSectionCount = 8;
    bool infoSectionOpen[kInfoSectionCount] = {true, true, true, true, false, false, false, true};
    // STATUS rows, formatted each frame (a transient's label is longer than the kit's 96-char ring)
    static constexpr int kInfoStatusRows = 8; // 8 since 2026-10-20: a GEO member adds operator, mission and slot
    char infoStatusValue[kInfoStatusRows][160] = {};
    const char *infoStatusLabel[kInfoStatusRows] = {};
    int infoStatusCount = 0;
    // THE call site of a player breakup from the info window (the viewed satellite): today the Kessler
    // scenario's kesslerBreakupFromUi; its result goes to the toast. kind = kessler::EV_EXPLOSION (the harness can still pass EV_ASAT).
    void infoExplodeSelected(uint8_t kind);
    bool hovInfoSection[kInfoSectionCount] = {};
    // ORBIT section rows: altitude, inclination, RAAN, period, then the flare-mitigation power readout
    // (only for types whose primary surface uses the tilt, so the count is usually 4).
    static constexpr int kViewerOrbitRows = 5;
    char viewerOrbitValue[kViewerOrbitRows][40] = {};
    int viewerOrbitCount = 0;
    // SKY POSITION section: where the viewed satellite is in the parked ground observer's sky — visibility
    // ("Above horizon" / "Below horizon"), elevation, azimuth, range. Empty = no satellite.
    static constexpr int kViewerObsRows = 4;
    char viewerObsValue[kViewerObsRows][40] = {};
    // BRIGHTNESS section: the CPU evaluator on the viewed satellite — magnitude as seen (after extinction), above
    // the air, the extinction, the 1000 km magnitude, the phase angle. "-" where it does not apply.
    static constexpr int kViewerPhotRows = 6;
    char viewerPhotValue[kViewerPhotRows][40] = {};
    static const char *const kViewerPhotLabels[kViewerPhotRows];
    double viewerObsNextWall = 0.0;
    void updateViewerObserverInfo();
    // Select a satellite as a click on it does (clears a planet selection, refreshes its info).
    void selectSatellite(int idx);
    // Photometric check: sun-only render integrated on the CPU vs evalSatLobesPosed() (see
    // recordModelViewer). Requested by the button, recorded in recordCompute, read the next buildUI.
    bool viewerCheckRequested = false, viewerCheckAwaiting = false;
    uint64_t viewerCheckFrame = 0;
    double viewerCheckModelI = 0.0, viewerCheckTanHalf = 0.0, viewerCheckPhaseDeg = 0.0;
    // CHECK section: the render and the model at 1000 km (sun only) and their difference; a note when the
    // satellite is dark from that side.
    char viewerCheckValue[3][40] = {};
    char viewerCheckNote[96] = {};
    std::vector<bool> hovViewConst;
    // Viewer camera presets: 0 free orbit; 1 "from you" — on the line from the satellite toward the
    // observer, so the view is the satellite as you see it; 2 "toward you" — behind the satellite,
    // looking past it at your marker on the Earth.
    int viewerAim = 0;
    int viewerLastSelected = -1; // the selection when the viewer last took it (it follows CHANGES only)
    uint32_t viewerBgW = 0, viewerBgH = 0; // the HDR background's size (SatEnvProbes::ensureViewerBg)
    uint64_t viewerProbeFrame = 0;
    // A real satellite of constellation `constIdx` to show: the one highest in the observer's sky
    // (or, if none is up, the nearest) — so the constellation row's VIEW shows a satellite where it is.
    int pickViewerSatellite(int constIdx) const;
    // A roster satellite's own name, or nullptr (2026-10-20): a List member's name (the GEO belt), or the
    // constellation's name when it has exactly one satellite (a station). satLabel() uses it.
    const char *rosterMemberName(uint32_t idx) const;
    const ConstMember *rosterMember(uint32_t idx) const; // a List member, or nullptr
    // Settings > Constellations group of an entry: its "category", else inferred from its name / distribution.
    std::string constCategoryOf(const ConstellationConfig &c) const;
    // Open the satellite window (and, with popOut, the 3D view as well — what the constellation row's
    // VIEW does, since that caller asked for the view rather than the readout).
    void openModelViewer(int typeIdx, const char *label, float altM, int satIndex = -1, bool popOut = false);
    // The viewed object's model can change under the same index (2026-10-19): a rocket stages (stack -> upper stage), a
    // ship starts its entry, a transient becomes a fireball. Called by updateViewerView AND recordModelViewer (the
    // selection's slot is rewritten between them), so the switch lands in the frame it happens: same camera direction,
    // distance scaled by the models' sizes, the env probe kept (it belongs to the index). An object with no model (a
    // re-entry fireball) keeps the last model, posed at its position: viewerKeepModel.
    void viewerSyncModel();
    void viewerFillModelRows(int typeIdx);
    bool viewerKeepModel = false;
    // The selection's handovers that are not transient-to-transient (SatelliteSimLaunches.cpp): a train member's record
    // that ends hands the selection (and the 3D view) to its roster slot, a retiree's to its re-entry fireball once the
    // list has it (selWant*), and while Go to rides a transient the selected transient mirrors it (selFollowKeyPrev).
    void launchSlotEndedSelection(uint32_t slot);
    uint8_t selWantKind = 0;
    uint64_t selWantA = 0;
    double selWantUntil = 0.0;
    bool selWantViewer = false;
    uint64_t selFollowKeyPrev = 0;
    // The selection panel's / out-of-view chip's three actions. "Info" opens the satellite window
    // (it was "View model": the window leads with the orbit, brightness and observer readouts, so it
    // reads as an info pane).
    void buildInfoButton(const UIInput &inp, UIRenderer &ui, int idx);
    // The shared per-frame prologue for both windows: sizes the one target, registers the image,
    // handles drag/zoom, reads the photometric check back, refreshes the readouts.
    void updateViewerView(const UIInput &inp, UIRenderer &ui);
    void buildInfoWindow(const UIInput &inp, UIRenderer &ui);       // render band + sections
    void buildViewPopoutWindow(const UIInput &inp, UIRenderer &ui); // the maximized render
    bool buildViewChip(const UIInput &inp, UIRenderer &ui, int row, int k, const char *name, int iconIdx, bool on);
    void buildViewChips(const UIInput &inp, UIRenderer &ui, int row);
    // The viewer's marker dots in its target's pixels (recordModelViewer, from the frame's projection),
    // for their "You" / "Target" labels (buildViewerMarkerLabels, floating over the image: view 0 the
    // info window's band, 1 the pop-out). One frame behind the render, like every viewer readout.
    struct ViewerMarkerLabel
    {
        bool on = false;
        float x = 0.0f, y = 0.0f;
    };
    ViewerMarkerLabel viewerMarkerLabels[2];
    void buildViewerMarkerLabels(int view);
    int viewerGoneFate(double *whenT = nullptr) const; // the viewed roster object destroyed (2) / re-entered (3), else 0
    glm::dvec3 rocketEngineGeom(int typeIdx) const; // (exit plane z, exit radius, length) of a rocket model
    void buildViewerGoneLabel(int view);
    char viewerGoneBuf_[2][64] = {};
    // Select / Go to in a satellite window's title bar. `popout` = the popped-out 3D view (its own
    // hover pair), false = the info window.
    void buildViewTitleIcons(const UIInput &inp, UIRenderer &ui, bool popout);
    // Which window's title icons were hovered last frame — buildResizableWindow skips starting a
    // title-bar drag when the click is on one of them (winId 3 = pop-out, 4 = info window).
    bool viewTitleIconsHovered(int winId) const;
    void recordModelViewer(VkCommandBuffer cmd, float dt);
    // The env renderer's star field (sat_sky.frag SKY_ENV, skyDescSet binding 24): createStarEnvBuffer.
    static constexpr int kStarEnvCount = 8404;     // = the catalogue = sat_sky.frag's STAR_ENV_COUNT
    static constexpr uint32_t kStarEnvGrid = 32;   // cells per cube-face edge (~2.8 deg)
    static constexpr float kStarEnvMaxSigmaPx = 2.0f; // bright stars widen at most this far there
    VkBuffer starEnvBuf = VK_NULL_HANDLE;
    VkDeviceMemory starEnvMem = VK_NULL_HANDLE;
    void *starEnvMapped = nullptr;
    void createStarEnvBuffer(VulkanContext &ctx);
    void uploadStarEnvHeader();

    // ── Phase 4c: satellite meshes in the main view ──────────────────────────────────────────────
    // The followed satellite (else the selected one) is drawn as a mesh by SatMeshRenderer's scene
    // pass once it is big enough on screen: it fades in from kMeshFadeInPx to kMeshFullPx across its
    // bounding diameter while its sprite fades out (SatFlarePC::meshSatIdx/meshSpriteKeep), and its
    // over-white glints seed the bloom (recordBloom). Positions are CPU double: the satellite from
    // satOrbitStateAt, the observer from obsDir/radius (or followObsEcef), camera-relative floats.
    // (4d generalises this from one satellite to every satellite big enough on screen.)
    static constexpr float kMeshFadeInPx = 1.5f, kMeshFullPx = 3.0f;
    // The fade-in size actually used: kMeshFadeInPx, raised while more than kMaxMeshInstances satellites
    // are that big (the AI ring close up), so the ones past the cap stay sprites rather than popping
    // between the two (recordMeshScene). The mesh then spans meshFadePx..2x it.
    float meshFadePx = kMeshFadeInPx;
    // The sprite (the apparent-magnitude flare: point, glare and bloom) fades out over a WIDER range
    // than the mesh fades in, so the magnitude-based flare stays with the satellite while its model
    // resolves; the bloom is split between the two so its total stays the sprite's.
    static constexpr float kSpriteGoneFullPx = 10.0f;
    // Mesh glare (mesh_bloom.frag): below this on-screen size a mesh is still a point and all its light
    // may glare; by 3x it, only sun-like reflections do.
    static constexpr float kGlarePointPx = 12.0f;
    VkBuffer meshKeepBuf = VK_NULL_HANDLE;
    VkDeviceMemory meshKeepMem = VK_NULL_HANDLE;
    void *meshKeepMapped = nullptr;
    // Drawn this frame, for picking (a meshed satellite has no sprite to click).
    struct MeshDrawn
    {
        int sat;
        glm::vec3 enuDir;
        float radiusPx;
    };
    std::vector<MeshDrawn> meshDrawn;
    int meshSceneSatIdx = -1;     // the FOLLOWED satellite drawn this frame (-1 none) — its sprite fade
                                  // goes through SatFlarePC; GPU candidates fade their own
    std::vector<GpuSatListHeader::MeshCandidate> meshCandidates; // last frame's (read with the header)
    // (2026-10-26) the launch-record slots recordMeshScene drew as meshes last frame: launchUpdate rewrites their GPU
    // records every frame, so each one's point (bloom, glare, its fading sprite) stays on its mesh at any warp.
    std::vector<uint32_t> meshLaunchSlots;
    bool meshesDrawnThisFrame = false;
    // The selected satellite's direction from the CPU orbit (double), refreshed after
    // updatePositions(): the selection panel places itself with it, so a satellite stays "in view"
    // in Earth's shadow and while its sprite has handed over to a mesh (both have no sprite).
    glm::vec3 selSkyDirCpu{0.0f, 0.0f, 1.0f};
    bool selAboveEarth = false; // not hidden behind the Earth
    void updateSelectedSkyDir();
    float meshSceneFade = 0.0f;   // its fade
    void recordMeshScene(VkCommandBuffer cmd, VulkanContext &ctx, float dt);
    uint32_t meshCandTotal = 0; // last frame's meshCandCount: past kMaxMeshCandidates = the list overflowed

    // ── Environment probes: full-renderer reflections + ambient on meshes (2026-09-24) ───────────
    // SatEnvProbes renders sat_sky.frag (-DSKY_ENV) as six cube faces around a satellite: the mesh
    // shader reflects that and takes its SH irradiance as diffuse light, so a mirror shows the real
    // Earth, clouds, aurora and Milky Way, and an edge-on face is lit by the Earth it sees. Slot 0 is
    // the model viewer's (a face per frame while it is open, Live light); scene instances share the
    // rest. A probe belongs to the satellite it was made for and FOLLOWS it (2026-09-25): its owner
    // keeps it however far it drifts between refreshes; another instance shares the nearest probe within
    // max(20 km, 2% of its altitude), else takes the least recently used slot. kEnvRendersPerFrame
    // probes are (re)rendered per frame, new ones first, then the one that has drifted furthest. Until
    // its own probe exists an instance borrows the nearest rendered one; with none it falls back to
    // earth_env.glsl + the photometric earthshine. Probes used to be tied to positions: at 5 min/s a
    // satellite outran the 20 km reuse radius every frame, took a fresh slot, and every instance but
    // the one rendered that frame dropped to the (differently lit) fallback — the ambient light of
    // everything flickered.
    SatEnvProbes envProbes;
    bool envReflections = true; // Settings → Display "Full-renderer reflections" (off = the analytic fallback)
    // Settings → Photometry "Sharp mirror reflections" (2026-09-25): the mirror-smooth pixels of up to
    // kMaxSharpReflInstances of the largest instances with such a material reflect the env renderer
    // per pixel (sat_sky.frag SKY_REFL) instead of a probe texel; needs envReflections.
    bool sharpReflections = true;
    static constexpr int kMaxSharpReflInstances = 4;
    static constexpr int kEnvRendersPerFrame = 1;
    struct EnvProbeSlot
    {
        bool rendered = false;
        int owner = -1;           // the satellite it follows
        glm::dvec3 posEcef{0.0};  // where it was rendered
        glm::dvec3 wantEcef{0.0}; // where its instances are now
        uint64_t lastUsed = 0;    // envFrame it was last assigned
        bool wanted = false;      // assigned this frame
        double renderedWall = 0.0; // glfwGetTime() of the last render
        double renderedSimT = 0.0; // ... and its sim time (J2000 s): a jump re-renders it whole
        uint32_t faceCursor = 0;   // refresh: the next pair of faces to re-render
    };
    EnvProbeSlot envSlots[SatEnvProbes::kProbes];
    uint64_t envFrame = 0;
    bool envActive() const;
    // The sky's push constants for the env renderer at posEcef, camera rotation camToEcef (sky camera
    // space → ECEF axes).
    // viewExposure / glareVis: the exposure and sun-glare gate of the view that will SHOW the result (the
    // main view for scene probes, the model viewer for its own) — the env renderer's post-tonemap terms
    // (Milky Way, zodiacal light, stars) follow that view's rules, not the probe position's.
    SatDrawPC envSkyPC(const glm::dvec3 &posEcef, const glm::dmat3 &camToEcef, float fovYRad, float aspect,
                       float viewExposure, float glareVis) const;
    float viewerExposureNow = 1.8f; // the model viewer's exposure this frame (recordModelViewer)
    float viewerGlareEased = 1.0f;  // its sun-glare gate for stars / Milky Way (skyGlareEased's rule)
    void renderEnvProbe(VkCommandBuffer cmd, int slot, const glm::dvec3 &posEcef, uint32_t faceMask = 0x3Fu);
    // Picks (and schedules) a scene probe for an instance at posEcef; kNoProbe until rendered.
    uint32_t assignEnvProbe(const glm::dvec3 &posEcef, int sat);
    uint32_t nearestRenderedProbe(const glm::dvec3 &posEcef) const; // kNoProbe if none within 1000 km
    int viewerProbeSat = -1; // the satellite slot 0 was last rendered for (a new one renders all faces)
    void renderScheduledEnvProbes(VkCommandBuffer cmd);
    void writeMeshSceneDescriptors(VulkanContext &ctx); // sky 22/23, scene_depth 3 (init + resize)
    float skyExposure() const; // sat_sky.frag's exposure for this frame (bloom threshold)

    // ── Phase 4e: follow mode ─────────────────────────────────────────────────────────────────────
    // The observer is MOVED to the satellite: it rides at `followOffset` in the satellite's local
    // orbital frame (x along-track, y cross-track, z radial), so the satellite stays put while the
    // Earth and sky turn beneath — and the mesh is drawn because it is close (recordMeshScene). WASD /
    // Q-E move the offset (speed scales with the distance to it), RMB looks around (and turns the aim
    // lock off). Position is double end to end: followObsEcef is authoritative; obsDir / obsHeightOffset
    // are derived from it (sky shaders), and updatePositions() takes followRadiusM as the radius.
    bool followActive = false;
    int followSatIndex = -1;
    bool followAimLock = true;
    glm::dvec3 followOffset{0.0};
    glm::dvec3 followObsEcef{0.0};
    double followRadiusM = 0.0;
    glm::vec3 followSavedObsDir{0.0f}, followSavedFacing{0.0f};
    float followSavedHeight = 0.0f, followSavedEl = 0.0f, followSavedFov = 70.0f;
    bool hovSelFollowBtn = false, hovFollowAim = false, hovFollowExit = false, hovFollowStay = false;
    bool hovFollowReturn = false, hovFollowAgain = false;
    char followLabel[128] = {};
    // ── Go to: Return / follow again (2026-10-20, issue 20261019_223437) ──────────────────────────
    // KB_FOLLOW_RETURN, the follow HUD's "Return", harness `follow return`: while following, fly the camera back to
    // the default offset (behind and above, ~4 model radii) in the object's own frame; after leaving Go to ("Stay
    // here", Exit, an object that ended), follow the last object again (followLast*: what was followed, kept for
    // the session). A transient (a rocket, the ship) is continued along its mission to now first.
    int        followLastIdx = -1;        // satellite index (a transient: -1, see followLastTr)
    bool       followLastIsTr = false;    // the transient itself: followLastTr (declared with SelTransient)
    uint64_t   followLastOwner = 0;       // a launch record: its owner then (a slot changes hands)
    uint16_t   followLastM = 0;
    uint32_t   followLastPoolGen = 0;
    char       followLastLabel[128] = {};
    double     followAgainSimT = 0.0;     // the sim time it was left (a jump of more than 10 min drops the chip: its label is stale)
    double     followAgainUntil = 0.0;    // the "Go to <last> again" chip shows until this glfwGetTime() after leaving
    void followRemember();                // store the current follow target as the last one
    bool followReturn(std::string *msg = nullptr);
    // Go to -> Go to and Return (2026-10-20, issue 20261019_223726): a flight between two places NEAR the target,
    // flown in the target's own along/cross/radial frame from the camera's offset A to B: the direction turns, the
    // distance changes geometrically. A switch from one followed object to another starts from where the camera is
    // (the old object's frame at this frame's time), not from the parked ground observer; it was flown along the
    // observer's line of sight, whose start distance clamped at 1 m: the camera passed through the new satellite.
    bool       followFlightRel = false;
    // (2026-10-21) The default offset (-4r, 0, 1.5r) was just set (startFollow: followOffset; followReturn:
    // followFlightRelB): the next updateFollow puts it on the side BEHIND the object's motion over the ground (a
    // rocket slower than 30 m/s: behind its engine end). A transient's carried frame heading can point backward (a
    // booster's boostback keeps the side), where "-4r along" was in front.
    bool       followDefaultSideOffset = false, followDefaultSideRelB = false;
    double     followDefaultSideDot = 0.0;
    glm::dvec3 followFwdRefEcef{0.0}; // (harness) the motion heading behind which the default offset sits (unit, ECEF) // (harness: the frame's along axis . the motion's heading when last applied)
    bool       followFlightRelKeepDir = false; // B = A's direction at the follow distance (a switch), else B is set
    glm::dvec3 followFlightRelA{0.0}, followFlightRelB{0.0};
    // The camera's floor (issue 20261019_223321): never below the ground (the GPU's detailed ground under the eye,
    // eased, and the CPU DEM at the camera) or the sea. followGpuFloorM is the eased GPU reading.
    double     followGpuFloorM = -1e30;
    double     followFloorM(const glm::dvec3 &eyeEcef, float dt);
    glm::dvec3 followTargetEcef(int idx, double t) const; // a followed object's position (ECEF), with followHandD
    // Review 22: "Go to" and "Exit" FLY (fly = true: the UI's buttons): from the eye to the follow point (re-evaluated
    // every frame as the satellite moves) and back to the saved ground spot, the arc slerped and the altitude
    // interpolated in log space with a rise for long arcs (slow near the ground, fast high up), 2-8 s by the log of
    // the distance. Any move key ends it at its destination. The harness, cinematics and the settings save keep the
    // instant jump (fly = false). followFlight: 0 none, 1 to the satellite, 2 home.
    int        followFlight = 0;
    double     followFlightT = 0.0, followFlightDur = 0.0;
    glm::dvec3 followFlightFrom{0.0};
    float      followSavedGround = 0.0f, followFlightEl0 = 0.0f, followFlightFov0 = 50.0f;
    glm::vec3  followFlightFacing0{0.0f};
    glm::vec3  followFlightView0{0.0f};     // review 22b: the view direction (ECEF) a flight to a satellite starts from
    bool       followFlightAlign = false;   // put the arrival on the approach line (next updateFollow)
    double     followDefaultDist = 0.0;
    // Review 23: a flight with a clear line of sight flies straight along it (followFlightLine) toward the arrival point
    // as it was at the start (followFlightTarget0); the free aim keeps its view fixed in the satellite's frame, which is
    // last frame's (followBasis*).
    bool       followFlightLine = false;
    glm::dvec3 followFlightTarget0{0.0};
    // Review 24: the line is the OBSERVER'S line of sight to the satellite (the parked ground observer to the satellite,
    // re-aimed every frame), so a flare the observer sees stays in view all the way in and out. Set on a flight's
    // first update (followFlightInit): the starting distance along it and the start's offset from it (faded out).
    bool       followFlightInit = false;
    double     followFlightS0 = 0.0;
    glm::dvec3 followFlightDelta0{0.0};
    // The followed satellite's position at the last updateFollow (ECEF). A flight home starts from the camera's place
    // RELATIVE to it (followFlightFromRel: followFlightFrom holds that offset until the flight's first update adds this
    // frame's satellite): taken absolute, the start lagged the satellite by a frame of orbital motion (~130 m at 1x)
    // and the view swung ~37 deg round a satellite 166 m away on the first frame home (2026-10-04, Reflect Orbital #997).
    glm::dvec3 followLastP{0.0};
    bool       followFlightFromRel = false;
    glm::dvec3 followHomeEcef() const;
    glm::dvec3 followBasisT{0.0}, followBasisN{0.0}, followBasisR{0.0};
    bool       followBasisValid = false;
    double followFlightDuration(double distM) const;
    void startFollow(int satIndex, bool fly = false);
    void stopFollow(bool fly = false);
    // Leave Go to WHERE THE CAMERA IS (2026-10-19, "Stay here": the follow HUD's button, KB_FOLLOW_STAY, harness
    // `follow stay`): the observer becomes a free observer at the camera's place (direction, altitude above sea level,
    // facing, elevation and FOV kept), Earth-fixed from then on. The parked ground observer is dropped: the settings
    // save the new free observer like any other. `why` (optional) is shown as a toast (a followed object that ended).
    void stopFollowHere(const char *why = nullptr);
    // A handover between objects (a rocket's stack to its upper stage, a stage to its re-entry, a raising train
    // member to its roster slot): the jump between them (ECEF), eased out over ~2 s so the camera glides.
    glm::dvec3 followHandD{0.0};
    glm::dvec3 followHandViewA{0.0}, followHandViewB{0.0}; // the free view's turn with the eased handover (updateFollow)
    double followBasisSimT = 0.0; // the sim time followBasis* was made at (updateFollow: the frame's heading turn rate)
    // Harness `follow log on|off`: one row per updateFollow (t, the object's kind/key, offset, the camera and its view
    // in the object's frame, the frame's axes) to find discontinuities through a mission's handovers.
    struct FollowLogRow
    {
        double t, dt;
        int sat, kind, stage;
        uint64_t key;
        glm::dvec3 off, camRel, view, T, Traw, R, P;
        double viewObjDeg, dist, handM;
        int flight;
    };
    bool followLogOn_ = false;
    std::vector<FollowLogRow> followLog_;
    // Following a launch record (a train member raising, a retiree deorbiting): its slot's owner and member when
    // follow began (a slot changes hands when its batch ends).
    uint64_t followLaunchOwner = 0;
    uint16_t followLaunchM = 0;
    // Per frame, before updateFollow: hand over to the object that continues the followed one, or end in place.
    void followCheckTarget();
    bool isFollowing(int satIndex) const; // follow rides this satellite (a selected transient: the same object)
    void updateFollow(float dt);
    glm::dvec3 followFlightPos(const glm::dvec3 &from, const glm::dvec3 &to, double u) const;
    void buildFollowButton(const UIInput &inp, UIRenderer &ui, int idx);
    void buildFollowHud(const UIInput &inp, UIRenderer &ui);

    // ── Selection "Track" (the Track button in the selection panel / chip) ────────────────────────
    // A CAMERA lock on the SELECTED satellite: updateTrack() re-aims from its live ENU direction every
    // frame, so it stays centred as it crosses the sky, while the observer stays where it is (WASD
    // still walks) and the wheel's FOV zoom still works — follow mode is the opposite bargain, it
    // moves the observer to the satellite. buildUI swallows look input while the lock is on, since
    // nothing may fight the aim; the `camera` command, a deselect, a planet selection, follow mode and
    // the info window's Go to all release it. Tracking a satellite that has gone below the horizon
    // keeps working on purpose (the aim just goes behind the Earth) instead of silently dropping.
    bool trackActive = false;
    bool hovSelTrackBtn = false;
    void startTrack();
    void stopTrack();
    void updateTrack();
    // Point the camera at an az/el in the observer's local ENU. obsFacing is the authority (buildUI
    // derives camera.azDeg from it every frame), so this sets obsFacing and camera.azDeg/elDeg
    // together; the harness's `camera az=`/`look`/`track` commands go through it as well.
    void aimCameraAzEl(float azDeg, float elDeg);
    void buildTrackButton(const UIInput &inp, UIRenderer &ui, int idx);

    // ── Orbit pipeline buffers ────────────────────────────────────────────────
    VkBuffer satOrbitBuf = VK_NULL_HANDLE; // device-local, uploaded once at init
    VkDeviceMemory satOrbitMem = VK_NULL_HANDLE;
    // The 7-sim-day rebake runs on the GPU (sat_rebake.comp, 2026-10-15): it rewrites satOrbitBuf's
    // u0 / tumblePhase / raan from these per-satellite bases in exact fixed-point turns. The CPU bake +
    // full upload it replaced cost ~27 ms per rebake (13.7 ms a frame at 1 yr/s, 1.38M satellites).
    VkBuffer satPhaseBaseBuf = VK_NULL_HANDLE;
    VkDeviceMemory satPhaseBaseMem = VK_NULL_HANDLE;

    // ── Kessler syndrome: "The Sun's Fury" (SatelliteSimKessler.cpp, 2026-10-16) ─────────────────
    // A scripted Carrington-class CME arrives after the 2037-07-13 eclipse; it kills a share of the
    // satellites (they tumble), dead formation members bump their neighbours, and a statistical
    // collision engine (cube method, kessler::World on a worker thread) carries the cascade forward:
    // every breakup a NASA-SBM debris cloud of Keplerian pool records. Everything is a pure function of
    // sim time (an event log; born / end seconds), so playback in either direction is exact. Before the
    // CME nothing differs from the plain sky. Plan and design notes: .plans/KESSLER_PLAN.md.
    bool kesslerEnabled = true;       // settings kessler.enabled (applied at launch)
    float kesslerRateMult = 1.0f;     // kessler.rate_multiplier: collision probability x (1 = physical)
    float kesslerAvoidance = 0.9f;    // kessler.avoidance: share of active-vs-debris encounters dodged
    float kesslerHorizonYears = 20.0f; // kessler.horizon_years: how far the cascade is simulated
    float kesslerDustGain = 100.0f;    // kessler.dust_gain: the drawn dust clouds' brightness (x the physical estimate)
    float kesslerFireColor = 0.8f;     // kessler.fireball_colors: fragments tinted by material (0 = temperature only)
    float kesslerSmallReGain = 30.0f;  // kessler.small_reentry_gain: small fragments' fireballs (x physical: meteor-faint)
    struct KesslerWorker;             // the World + its thread (SatelliteSimKessler.cpp)
    struct KesslerWorkerDelete        // (an incomplete type here: deleted where it is defined)
    {
        void operator()(KesslerWorker *w) const;
    };
    std::unique_ptr<KesslerWorker, KesslerWorkerDelete> kWorker;
    // (2026-10-21) A restart's world built on a thread of its own (a player breakup with no rollback point, e.g.
    // before the CME: ~220 ms that froze the click frame). Sim time holds (kWorkerDone false) until it is installed.
    struct RestartOut
    {
        std::unique_ptr<KesslerWorker, KesslerWorkerDelete> wk;
        uint64_t hash = 0;
        double cacheFrontier = 0.0;
        // (2026-10-29) Restored from a world snapshot (SatelliteSimKessler.cpp, "Snapshots"): the mirror is rebuilt from
        // the world at install, with these streams (made before the snapshot, windows still to come).
        bool fromSnap = false;
        double snapLoadMs = 0.0;
        // (made on the restart's thread, swapped in at install: ~2.5 s on the main thread otherwise)
        std::vector<kessler::PoolRec> snapPool;
        std::vector<uint32_t> snapGen;
        std::unordered_map<int32_t, std::vector<std::pair<uint32_t, int32_t>>> snapReentries;
        std::map<int32_t, std::vector<kessler::SmallStream>> snapStreams;
        size_t snapStreamCount = 0;
    };
    std::filesystem::path kSnapRestore; // the snapshot the next kesslerRestart starts from (consumed by it)
    bool kesslerSnapshots = true;       // settings: kessler.snapshots (no UI)
    int kSnapRestores = 0;              // (harness `kessler mem`)
    double kLastSnapLoadMs = 0.0, kLastSnapInstallMs = 0.0;
    double kSnapLastMarkT = 0.0;
    std::filesystem::path kesslerSnapDir() const;
    bool kesslerSnapEligible() const;
    std::filesystem::path kesslerBestSnapshot(double lo, double hi) const; // the latest snapshot with lo < F <= hi
    std::thread kRestartThread;
    std::thread kDisposeThread; // the replaced world freed off the main thread
    std::atomic<bool> kRestartReady{false};
    bool kRestartBusy = false;
    std::vector<kessler::Event> kRestartQueue; // (2026-10-21c) breakups clicked while a rebuild runs: applied when it is in
    RestartOut kRestartOut;
    std::chrono::steady_clock::time_point kRestartT0;
    void kesslerRestartInstall(RestartOut &out);
    bool kesslerRestartPoll(bool wait); // true while one is still building (wait: join it now)
    std::vector<kessler::PoolRec> kPool;  // the main thread's mirror of the world's pool
    std::vector<int32_t> kRosterWords;    // ... of each roster satellite's dead and end seconds (2 per satellite)
    std::vector<int32_t> kRosterDecay;    // ... and its drag re-entry second (an end AT it is a re-entry)
    // Re-entries by the hour of their second (since the epoch): satellite indices (roster, or activeSatCount + record).
    // Entries are checked against the current death when used (a later collision may have ended the object first).
    std::unordered_map<int32_t, std::vector<std::pair<uint32_t, int32_t>>> kReentries; // (satellite, its re-entry second)
    // Garbage collection (round 3): pool slots are reused once their record is gone (kessler::World::addRecord), so a
    // slot holds several records over time (generations). kPool[j] is the generation SHOWN at the current time;
    // kPoolHist keeps every generation of a reused slot from the last kHistKeepS of sim time, oldest first, so playing
    // time backwards over a reuse shows the earlier record again (kGenBounds: each later generation's birth second ->
    // slot, the moments a slot changes hands). Going back past what was pruned replays the world (kReplayPending).
    std::vector<uint32_t> kPoolGen;
    struct KGen
    {
        uint32_t gen;
        kessler::PoolRec rec;
    };
    std::unordered_map<uint32_t, std::vector<KGen>> kPoolHist;
    std::multimap<int32_t, uint32_t> kGenBounds;
    static constexpr int32_t kHistKeepS = 7 * 86400;
    int32_t kShownSec = INT32_MIN, kHistPrunedUntil = INT32_MIN, kHistPrunedAt = INT32_MIN;
    bool kReplayPending = false;
    void kesslerSetPoolOrbit(uint32_t j);
    void kesslerShowGen(uint32_t j, int32_t sec);
    const kessler::PoolRec *kesslerPoolGenWithDecay(uint32_t j, int32_t decaySec) const;
    void kesslerIndexReentry(uint32_t sat, int32_t sec);
    void kesslerPruneOldIndexes(); // (2026-10-28) streams / re-entry index older than 30 days before now
    // The fireballs (round 3): heads as point sprites appended to the visible list by kessler_fireballs.comp
    // (CPU-built, host-visible kFireBuf), trails as emissive Gaussians in the dust buffer.
    static constexpr uint32_t kFireballMax = 8192;
    struct GpuFireball
    {
        glm::vec4 dirFlare; // ENU direction from the eye; w = effectFlare
        glm::vec4 colRange; // rgb; w = range (m)
    };
    VkBuffer kFireBuf = VK_NULL_HANDLE;
    VkDeviceMemory kFireMem = VK_NULL_HANDLE;
    void *kFireMapped = nullptr;
    uint32_t kFireCount = 0, kFireTrails = 0, kReentriesNow = 0; // this frame (harness state)
    // Small fragments coming down (round 4): the worker's streams, by the day their window opens, and the individual
    // re-entries they make, expanded per hour (a pure function of the hour) for the hours a fireball can be drawn in.
    struct KSmallRe
    {
        int32_t sec;
        float incl, raan, u, massKg;
        uint32_t seed;
    };
    std::map<int32_t, std::vector<kessler::SmallStream>> kSmallStreams;
    size_t kSmallStreamCount = 0;
    std::map<int32_t, std::vector<KSmallRe>> kSmallReHours;
    uint32_t kSmallReNow = 0, kSmallReHourCount = 0;
    void kesslerAddStreams(const kessler::SmallStream *v, size_t n);
    const std::vector<KSmallRe> &kesslerSmallReHour(int32_t hr);
    VkDescriptorSetLayout kFireDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool kFireDescPool = VK_NULL_HANDLE;
    VkDescriptorSet kFireDescSet = VK_NULL_HANDLE;
    VkPipelineLayout kFirePipeLayout = VK_NULL_HANDLE;
    VkPipeline kFirePipeline = VK_NULL_HANDLE;
    void kesslerCreateFireballPass(VulkanContext &ctx);
    void kesslerRecordFireballs(VkCommandBuffer cmd, VulkanContext &ctx);
    void kesslerDestroyFireballPass(VkDevice device);
    // The next re-entry after the current time whose breakup is in darkness (sun below -8 deg there) and, preferring
    // dead satellites, the heaviest in the next 6 h: returns its satellite index, second and breakup point (ECI m).
    bool kesslerNextReentry(uint32_t &sat, double &tJ2000, glm::dvec3 &breakupEci, glm::dvec3 &velEci) const;
    // Sets the clock 25 s before that re-entry's breakup (1x, running) and the observer ~120 km to the side of its
    // track, looking at the breakup point. False (and why in msg) when none is due in the next 6 h.
    bool kesslerGoToNextReentry(std::string &msg, bool watch = false); // watch: mid-glow, the fireball tracked
    bool selWantTrack = false;        // the selWant* fireball's camera lock (a Watch)
    // ... of the applied events: since 2026-10-28 a VIEW of the world's own EventLog (no copy), up to the count the
    // worker published and the mirror has taken in (kesslerSyncProcess).
    struct KEventView
    {
        const kessler::EventLog *src = nullptr;
        size_t n = 0;
        size_t size() const { return n; }
        bool empty() const { return n == 0; }
        kessler::Event operator[](size_t i) const { return (*src)[i]; } // (rebuilt: kessler::EventLog keeps cores)
        const kessler::EventLog::Core &core(size_t i) const { return src->core(i); }
        double tOf(size_t i) const { return src->tOf(i); }
        double tUpper(size_t i) const { return src->tUpper(i); }
        void clear() { n = 0; }
        template <class K, class Less>
        size_t lowerBound(const K &key, Less less) const
        {
            size_t lo = 0, hi = n;
            while (lo < hi)
            {
                const size_t mid = lo + (hi - lo) / 2;
                if (less((*src)[mid], key))
                    lo = mid + 1;
                else
                    hi = mid;
            }
            return lo;
        }
    };
    KEventView kEvents;
    double kFrontier = 0.0;               // simulated up to here (J2000 s)
    double kCacheFrontier = 0.0;          // the loaded cache's frontier: a cache is only ever rewritten longer
    bool kWorkerDone = false;
    uint64_t kHash = 0;                   // the world's inputs (cache key)
    int kDebrisConst = -1;                // the "Collision debris" pseudo-constellation (fragments)
    uint32_t kDebrisType = 0;
    int kDebrisKit[3][3] = {{-1, -1, -1}, {-1, -1, -1}, {-1, -1, -1}}; // [kind: array, radiator, structure][S, M, L]
    std::map<std::string, int> rocketTypes; // "rocket_<vehicle>_<stack|booster|upper>" -> its type index
    // GPU: kesslerBuf = roster dead / end seconds + pool records (sat_orbit.comp binding 11, rebake 2),
    // kPoolBaseBuf = the records' J2000 phases (rebake 3). Sized for kPoolCap records; grown on demand.
    VkBuffer kesslerBuf = VK_NULL_HANDLE;
    VkDeviceMemory kesslerMem = VK_NULL_HANDLE;
    VkBuffer kPoolBaseBuf = VK_NULL_HANDLE;
    VkDeviceMemory kPoolBaseMem = VK_NULL_HANDLE;
    VkBuffer kStageBuf = VK_NULL_HANDLE; // persistent upload staging (per-frame copies)
    VkDeviceMemory kStageMem = VK_NULL_HANDLE;
    void *kStageMapped = nullptr;
    // The dust clouds (round 2): up to kMaxDustClouds Gaussian ellipsoids a frame, built on the CPU from the recent
    // breakups (kesslerFillDust) and integrated in closed form by cloud_march.comp (binding 23).
    // 1024 since the launches (2026-10-18): the Kessler dust takes at most kDustMax, the fireball trails run up to
    // kFireTrailMax, the rocket plumes and flames take the rest (SatelliteSimLaunches.cpp).
    static constexpr uint32_t kMaxDustClouds = 1024;
    static constexpr uint32_t kDustMax = 384;
    static constexpr uint32_t kFireTrailMax = 704;
    struct GpuDustCloud
    {
        glm::vec4 c;   // centre relative to the eye (m, ENU axes); w = cross-section x albedo x gain (m^2)
        glm::vec4 ia;  // inverse covariance xx, yy, zz; w = 1 / ((2 pi)^1.5 sqrt(det))
        glm::vec4 ib;  // inverse covariance xy, xz, yz; w unused
        glm::vec4 col; // tint; w: 0 scattering dust, 1 emissive, 2 a rocket plume
        glm::vec4 cone; // the directions it can touch: xyz unit (ENU), w = cos of the half-angle; 0 = everywhere
    };
    static_assert(sizeof(GpuDustCloud) == 80); // cloud_march.comp DustCloud
    VkBuffer kDustBuf = VK_NULL_HANDLE;
    VkDeviceMemory kDustMem = VK_NULL_HANDLE;
    void *kDustMapped = nullptr;
    uint32_t kDustCount = 0; // drawn this frame (harness state)
    // The clouds drawn this frame (harness `kessler state` -> dust.clouds): event index, kind, dust kg, small
    // fragments, age (s), the rank's effective size sqrt(amp / priority) (km), its light (amp, before the sky units), cut fade.
    std::vector<std::array<double, 8>> kDustDbg;
    void kesslerFillDust(float pixelAngle);
    // Fills the fireballs for the current time into kFireBuf and appends their trails to the dust list (out, n).
    void kesslerFillFireballs(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull);
    // tagKind / tagA: what the fireball is, for picking (SelTransient: 3 a launch retiree, 4 an upper stage, 5 a Kessler
    // re-entry, 6 a small fragment).
    void fbDrawReentry(const glm::dvec3 &U0, const glm::dvec3 &A, double m0, int N, uint64_t seed, double tau,
                       GpuDustCloud *out, uint32_t &n, double toSky, double pixFull, uint8_t tagKind = 0,
                       uint64_t tagA = 0);

    // ── Transient selection (2026-10-19): rocket stages, Starship's ship coming home and re-entry fireballs are not
    // records; each frame the code that draws their heads lists them here, and one reserved launch slot (the last,
    // transientSatIdx()) carries the selected one's live position (SatOrbitElems::fixedState), so the selection
    // panel, Track and the harness treat it as a satellite. Gone for 2 s of sim time: deselected.
    struct SelTransient
    {
        uint64_t key = 0;   // stable over the object's life
        uint8_t kind = 0;   // 1 rocket stage, 2 ship entry, 3 retiree re-entry, 4 upper-stage re-entry, 5 Kessler
                            // re-entry, 6 small fragment, 7 a payload fairing half (stage = the half, 0 / 1), 8 a
                            // visiting spacecraft (crew / cargo), 9 a cargo ship's destructive re-entry (2026-10-21)
        uint8_t stage = 0;  // rockets: 0 booster, 1 upper; burn in burn
        uint8_t burn = 0;
        uint64_t a = 0;     // the batch key (1, 2, 4), the satellite (3, 5)
        double tau = 0.0;   // seconds since lift-off (1) / entry (2) / the glow's start (3-6)
        glm::dvec3 pos{0.0}, vel{0.0}, acc{0.0}; // ECI, at the frame's time
        glm::dvec3 axis{0.0};                    // rockets: the body axis toward the nose (ECI); 0 = none
        int type = -1;                           // rockets: its model type (rocketTypes), -1 = none
        glm::vec3 dirEnu{0.0f, 0.0f, 1.0f};
        float flare = 0.0f;
        // A full attitude (2026-10-20, a tumbling fairing half): body -> ECI at attT0, turning at attOmega (ECI).
        bool hasAtt = false;
        glm::dquat attQ{1.0, 0.0, 0.0, 0.0};
        glm::dvec3 attOmega{0.0};
        double attT0 = 0.0;
    };
    std::vector<SelTransient> selTransients; // this frame's
    SelTransient selTransientCur;            // the selected one, as last seen
    uint64_t selTransientKey = 0;
    uint64_t launchWantSelKey = 0; // a harness jump's stage, selected once the list has it (selTransientUpdate)
    bool launchWantTrack = false;  // ... and the camera locked on it (a Watch, 2026-10-24)
    double launchWantSelUntil = 0.0; // (a Watch's want lapses past this sim time)
    uint64_t launchFocusKey = 0;   // the launch a jump last framed (launch next / deploy / missions go): `launch
                                   // select kind=rocket` prefers its stages (2026-10-20)
    double selTransientSeenT = 0.0;
    double selTransientsT = 0.0;             // the sim time selTransients was made at
    uint32_t transientSatIdx() const { return activeSatCount + kLaunchCap - 1; }
    // The FOLLOWED transient has a slot of its own (2026-10-19): follow keeps riding it whatever is selected, and it is
    // re-evaluated at the frame's exact time (transientExactState) rather than extrapolated from the last list.
    uint32_t followSlotIdx() const { return activeSatCount + kLaunchCap - 2; }
    bool followIsTransient() const { return followActive && followSatIndex >= 0 && (uint32_t)followSatIndex == followSlotIdx(); }
    SelTransient followTrCur;
    SelTransient followLastTr;   // Go to's last object when it was a transient (followRemember)
    uint64_t followTrKey = 0;    // 0 with followWantKind: a record that ended, waiting for its fireball in the list
    double followTrSeenT = 0.0;
    uint8_t followWantKind = 0;  // the successor awaited (a SelTransient kind with a == followWantA), until followWantUntil
    uint64_t followWantA = 0;
    double followWantUntil = 0.0;
    uint32_t followPoolGen = 0;  // following a Kessler pool piece: its slot's generation (a reused slot is another object)
    // Follow moves to `s` (followed in the follow slot); the camera eases across the jump from `fromEci`.
    void followTakeTransient(const SelTransient &s, const glm::dvec3 &fromEci, double t);
    // Advances `cur` along its rocket's mission (stack -> upper stage at staging; a returning upper stage -> its ship
    // entry) and sets its exact state at t. False when it no longer exists as a rocket (wantKind: what continues it).
    bool transientContinue(SelTransient &cur, double t, uint8_t &wantKind) const;
    void transientWriteSlot(uint32_t slot, const SelTransient &s, double t0); // fixedState elements + the slot's type
    // The object that continues `cur` in this frame's list: a stack's upper stage after staging, an upper stage's
    // ship entry or re-entry fireball. False: none (yet).
    bool transientSuccessor(const SelTransient &cur, SelTransient &out) const;
    bool transientIsStack(const SelTransient &s) const; // a rocket's stage 0 before staging (one vehicle)
    // A rocket stage / ship entry's state at time t from the launch model itself (position, velocity, acceleration;
    // the axis is kept). False for the fireballs, which keep their extrapolation.
    bool transientExactState(SelTransient &s, double t) const;
    glm::dvec3 transientPosAt(const SelTransient &s, double t) const; // the model's position at t, else s.pos
    void followTransientUpdate(); // after the frame's list: track, hand over, or end in place
    // A rocket drawn as a mesh is picked (and followed) through its mesh slot: the transient it holds, taken as the
    // selection. Returns transientSatIdx(), or the index unchanged when it is not a rocket mesh slot.
    int rocketMeshSlotToTransient(int satIndex);
    bool selIsTransient() const { return selectedSatIndex >= 0 && (uint32_t)selectedSatIndex == transientSatIdx(); }
    void addSelTransient(const SelTransient &s);
    bool transientWanted(uint8_t kind, uint64_t a) const; // a re-entry the selection / Go to holds or waits for
    void selTransientUpdate();                // after the frame's visuals: the reserved slot's state, the label
    void selTransientTake(const SelTransient &s); // make it the selection's object (the caller selects the index)
    std::string transientLabel() const;
    std::string transientLabelOf(const SelTransient &s) const;
    // The Info window's time row for a transient (2026-10-21): its label and value, long durations as d/h/m, and a
    // visiting spacecraft's by phase ("Docked since <date> (n days)", "Since undocking", ...).
    void transientTimeRow(const SelTransient &s, std::string &label, std::string &value) const;
    double transientGraceS() const; // how long a listed transient may be missing before it counts as gone
    bool launchFocusListed() const; // the framed launch (launchFocusKey) has a stage in this frame's list
    bool selectTransient(int kind, std::string &msg, int stage = -1, bool nearest = false); // harness: the brightest (or
                                                                                            // nearest) of a kind (0 = any)
    // Rockets near enough to draw as meshes: up to kRocketMeshSlots of the reserved chunk's slots, bound to the
    // drawn rockets' keys (stable while they stay meshed). Returns their satellite indices (recordMeshScene).
    static constexpr int kRocketMeshSlots = 6; // the reserved chunk: 6 rocket meshes, the followed transient, the selection
    uint64_t rocketMeshKey[kRocketMeshSlots] = {};
    std::vector<int> launchMeshJobs(double pixAngle, const glm::dvec3 &obsEci);
    void launchStackJobs(double pixAngle, const glm::dvec3 &obsEci, std::vector<int> &jobs); // payload stacks
    double rocketMeshPx(const SelTransient &s, double pixAngle, const glm::dvec3 &obsEci) const;

    // ── Launches (SatelliteSimLaunches.cpp, 2026-10-18; .plans/LAUNCHES_PLAN.md) ─────────────────────
    // Every plane of a constellation a program in data/launches.json keeps flying is refreshed once per design life:
    // a rocket from a site under the plane deploys a train low, the train climbs to its slots, and the satellites it
    // replaces deorbit and re-enter. All a pure function of sim time. Trains and retirees are Keplerian records in the
    // launch block of kesslerBuf (satellites [activeSatCount, poolIdx0())); rockets are point sprites (kFireBuf),
    // their flames emissive and their plumes scattering Gaussians in the dust list.
    struct LaunchState;
    friend struct LaunchImpl; // SatelliteSimLaunches.cpp's helpers
    struct LaunchStateDelete
    {
        void operator()(LaunchState *s) const;
    };
    std::unique_ptr<LaunchState, LaunchStateDelete> launchState;
    bool launchesEnabled = true;     // launches.enabled
    float launchLifeScale = 1.0f;    // launches.life_scale: x every program's design life (fewer launches above 1)
    float launchPlumeGain = 1.0f;    // launches.plume_gain: the plumes' optical depth x
    float launchRocketGain = 1.0f;   // launches.rocket_gain: the flames' brightness x
    // Exhaust tuning (2026-10-21, issue 20261021_175638: "sliders for exhaust plumes, I wanna test ranges"; Settings >
    // Launches). 1 = the look before them, exactly. Drawn per frame (launchFillVisuals' addPlume, cloud_march
    // dustPad2) except the growth, which the parcels are built with (a change rebuilds every plume: B.plumeGrowJob).
    float launchPlumeBright = 1.0f;  // launches.plume_brightness: the light the exhaust scatters x
    float launchPlumeWidth = 1.0f;   // launches.plume_width: every plume Gaussian's radius x (same mass: thinner wide)
    float launchPlumeBillows = 1.0f; // launches.plume_billows: the billows' contrast x (cloud_march.comp)
    float launchPlumeGrowth = 1.0f;  // launches.plume_growth: the low trail's / ground clouds' turbulent growth x
    float launchPlumeDispersal = 1.0f; // launches.plume_dispersal (2026-10-26): old exhaust's widening with age x (no UI)
    float launchGroundCloudGain = 1.0f; // launches.ground_cloud_gain: lift-off steam / landing dust & spray density x
    // Rocket light on clouds, haze, plumes and the ground (2026-10-20, launches.engine_light_gain): the burning
    // engines (ascent, boostback, entry and landing burns), a ship's entry glow and the pads' floodlights as point
    // lights. Physical intensity from each flame's magnitude, lifted at night by the moonlight's own lift (moonGain /
    // the full Moon's physical irradiance), x this gain. Listed by launchFillVisuals / launchFillPadLights into
    // engineLights_, written after the dust list's 1024 entries (kDustBuf tail, header word 1) and drawn by the
    // lightning pass's mode 2 (lightning.frag: clouds and fog, the haze along the ray, the ground) and by
    // cloud_march.comp's plumes (dustCloudsCS).
    float engineLightGain = 1.0f;
    static constexpr uint32_t kMaxEngineLights = 32;
    // (2026-10-29) After the engine lights: per smoke Gaussian of the dust list a vec4 (its marginal vertical sigma, m, 0 =
    // the flat closed form is right for it; its tilt across its footprint, 3 sigma_h / R; the weight of the curved form for
    // the eye's place; 0), five to a GpuDustCloud slot. Computed on the CPU in double (view-independent); cloud_march.comp's
    // smokeCurvedInt reads it (dust[kMaxDustClouds + kMaxEngineLights + j / 5]).
    static constexpr uint32_t kDustSideSlots = (kMaxDustClouds + 4) / 5;
    std::vector<GpuDustCloud> engineLights_;  // this frame's (kMaxEngineLights at most, brightest kept)
    float engineAdaptEased = 1.0f;            // the eye's adaptation to the lights at night (engineLightsFlush)
    glm::dvec3 engineAdaptPrevEye{0.0};       // last frame's eye (ECI) and sim time: a cut snaps the adaptation
    double engineAdaptPrevT = -1e30;
    uint32_t engineLightCount = 0;
    void engineLightAdd(const glm::dvec3 &posEci, double intensitySunsM2, glm::vec3 col, double r0,
                        const glm::dvec3 &noseEci, double hullLen, double hullR, bool noGround,
                        double meshK = 1.0, const glm::dvec3 *meshPosEci = nullptr);
    void engineLightsFlush();
    // (2026-10-21) Drops the plume Gaussians that cannot show (SatelliteSimLaunches.cpp): their summed worst case, as
    // if all of them overlapped one pixel, stays under 2e-4 of white on screen and 1% of dimming.
    void dustCullInsignificant(const GpuDustCloud *list, uint32_t n, std::vector<uint8_t> &drop);
    // Knockout bit 33554432 = off (the A/B).
    uint32_t plumesCulled = 0;       // this frame's (launch state: dust_list.culled)
    glm::vec4 plumeCullStop{0.0f};   // the summed bounds where it stopped, and the next one's (light, dimming)
    // The same lights on the MESHES (2026-10-21, sat_mesh.frag: a landing booster, a pad's tower, a droneship's deck
    // lit by the floods and the burn): Earth-fixed positions, intensity (suns x m^2, lifted at night like the engine lights,
    // before the eye's adaptation), colour and the source's soft radius. engineLightAdd adds a light here too
    // (meshK x its intensity, at meshPosEci if given: a flame's light from just behind its nozzles); a pad's or a
    // ship's individual floods are mesh-only (meshLightAdd: their single cloud light stands at the pad's centre).
    // recordMeshScene takes the kMeshLocalLights most important to what the camera sees (last frame's list).
    struct MeshLocalLight
    {
        glm::dvec3 ecef{0.0}; // Earth-fixed (taken at its frame's time: a pad's or a ship's floods stay put)
        double I = 0.0;
        glm::vec3 col{1.0f};
        float rs = 1.0f;
        glm::dvec3 aimEcef{0.0}; // a floodlight's aim (unit, Earth-fixed; 0 = all round)
        float cosCut = -2.0f;     // ... and its beam: full inside acos(cosCut) - 10 deg, none past it
        bool physical = false;    // (2026-10-21) an entry plasma's light on its own hull: neither lifted at night nor adapted
    };
    std::vector<MeshLocalLight> meshLights_, meshLightsPrev_;
    float meshLightAdapt_ = 1.0f; // engineAdaptEased at the flush that made meshLightsPrev_
    int meshLightsScene_ = 0, meshLightsViewer_ = 0; // lights in the last scene / info-window view frame (harness)
    glm::vec4 viewerLightDbg_[8]{};                   // the info window's lights (xyz rel. to the subject, w I)
    uint32_t viewerLightMaskDbg_ = 0;
    int viewerArmPartsDrawn_ = 0; // a station's arm parts in the info window's 3D view (harness)
    void meshLightAdd(const glm::dvec3 &posEci, double intensitySunsM2, glm::vec3 col, double rs,
                      const glm::dvec3 *aimEci = nullptr, double beamHalfDeg = 180.0, bool physical = false);
    static double flameIntensitySunsM2(double mag100km);
    // (2026-10-21, cost) Fills frame.lightInfo / lightPos / lightCol / lightDir with last frame's mesh lights, the
    // positions relative to `originEcef` (the frame's world origin, ECEF axes): the scene pass (the camera) and the
    // info window's 3D view (the viewed object). meshLightMask: which of those lights can reach a bounding sphere
    // (centre in the same frame) with more than a hair of displayed light at `exposure` — GpuMeshInstance::trans[1].w
    // (bits), so sat_mesh.frag loops only over lights that matter to that instance.
    void fillMeshLocalLights(struct GpuMeshFrame &frame, const glm::dvec3 &originEcef) const;
    static uint32_t meshLightMask(const struct GpuMeshFrame &frame, const glm::dvec3 &centre, double radius, double exposure);
    int launchAfterStorm = 0;       // launches.after_storm: 0 stand down for good at the CME, 1 resume after 3 weeks
    // The deployment era (2026-10-20, launches.deployment_era; launches.json "deployment" / "roster_history" / site and
    // vehicle "online" dates): the constellations are BUILT UP by launches from the late 2010s to 2036 instead of
    // standing complete at any date. Each roster satellite's born / gone second (rosterLife, 2 per satellite, seconds
    // since kesslerEpoch(); INT32_MIN+1 / kNever = always) is uploaded after the Kessler roster words (sat_orbit.comp)
    // and read by the Kessler world (no collisions before a satellite exists). Off: everything always there (as before).
    bool launchDeployEra = true;
    // launches.era_benchmark_shells (2026-10-21): the 2020 V1.0 / VisorSat benchmark shells in the era's sky (off: they
    // are photometric references standing for Gen1 satellites the first fills already fly; Constellations > Benchmark).
    bool launchEraBenchmarkShells = false;
    // The share of roster satellites [i0, i1) there at t (the deployment era; 1 with it off): sorted born / gone seconds
    // per range, cached until rosterLife changes. The ambience's closed-form shell counts scale by it.
    double rosterRangePresentFrac(uint32_t i0, uint32_t i1, double t) const;
    // A mesh instance's date (GpuMeshInstance::trans[0].w, instCompAbsent): today's build-date code with the deployment
    // era on, 0 = every part (stations built module by module, 2026-10-21).
    float meshBuildCode(double tJ2000) const { return launchDeployEra ? (float)(tJ2000 / 86400.0 + kSatSinceBias) : 0.0f; }
    mutable std::unordered_map<uint64_t, std::pair<std::vector<int32_t>, std::vector<int32_t>>> rangeLifeCache_;
    mutable uint64_t rangeLifeKey_ = 0;
    std::vector<int32_t> rosterLife;
    uint64_t rosterLifeKey = 0;      // what rosterLife was computed for (0 = not yet)
    uint64_t kLifeHashKey = 0;       // ... and what the running Kessler world was built with
    bool rosterLifeUploaded = false; // the GPU holds the current rosterLife
    void launchPrewarm();            // (2026-10-21) at load: the first walk's batches, built in parallel
    void launchJoinBuildJob();       // (2026-10-21b) waits for the background batch build (before the roster changes)
    void rosterLifeEnsure();         // (re)computes rosterLife when stale (builds the launch planes if needed)
    void rosterLifeUpload(VulkanContext &ctx);
    nlohmann::json deployStatsJson(double fromT, double toT, double stepDays) const; // harness `deploy stats`
    nlohmann::json deployStateJson() const;                                        // harness `deploy state`
    struct DeployRow
    {
        std::string name;      // the cohort (program / constellation)
        double deployed = 0.0, target = 0.0;
        double perDay = 0.0;   // launches a day now (its first fills + refreshes, by arrival)
        double startT = 0.0, completeT = 0.0;
    };
    std::vector<DeployRow> deployRows(double t) const; // the Missions window's BUILD-OUT section
    // Launch-site graphics (2026-10-19, launches.pad_graphics): the pads on the ground (sat_sky.frag padGround, the
    // terrain graded flat under them, no city lights within ~3 km), their towers / mounts / tanks as meshes and their
    // lights at night. Independent of launchesEnabled (the pads are there whether or not anything flies).
    bool launchPadGraphics = true;
    static constexpr int kPadMeshSlots = 16; // two reserved chunks below the rockets' (launchInitBlock)
    // Payload stacks on their upper stage (2026-10-20): satellites not yet released and the dispenser, drawn as meshes
    // in six reserved chunks below the pads' (launchMeshJobs). Not selectable.
    static constexpr int kStackMeshSlots = 48;
    // Booster landing ships (2026-10-20, droneships): one reserved chunk below the stacks' (launchMeshJobs). Scenery,
    // not selectable (the booster on the deck is).
    static constexpr int kShipMeshSlots = 8;
    // A station arm (2026-10-21, Canadarm2: two booms + the wrist): one reserved chunk below the ships'. Not selectable.
    static constexpr int kArmMeshSlots = 8;
    nlohmann::json launchShipsJson() const; // harness `launch ships`: the ships on station / heading home
    void launchShipJobs(double pixAngle, const glm::dvec3 &obsEci, std::vector<int> &jobs); // their meshes
    void launchArmJobs(double pixAngle, const glm::dvec3 &obsEci, std::vector<int> &jobs);
    // (2026-10-21) A station's arm in the info window's 3D view: its parts' types and poses (body -> the view's
    // frame, origin) from the station root group's pose there. Returns how many (<= 3).
    int viewerArmParts(int sat, double t, const glm::dmat3 &rootR, const glm::dvec3 &rootT, int *types,
                       glm::dmat3 *Rb, glm::dvec3 *org); // the station arms (Canadarm2)
    double shipSeaState(const glm::dvec3 &ecefUnit) const; // the sea state at a ship (its swell; 1 = default)
    void launchFillShipLights();            // their lights at night (kFireBuf)
    void launchFillShipGlow(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull); // their lit decks
    // The next booster landing on a ship (harness `launch droneship`): the clock leadS before touchdown at 1x, the
    // observer distM from the ship (az clockwise from its bow) heightM above the sea, looking at the deck; wantDay /
    // wantNight: the Sun above 10 / below -8 degrees there; vehicle: an id filter.
    bool launchGoToShipLanding(double leadS, double distM, double azDeg, double heightM, int dayNight,
                               const std::string &vehicle, std::string &msg);
    void launchFillPadParams(GpuCloudParams &cp); // the UBO's pad block (nearest kMaxPadsGpu)
    void weatherFillSeaParams(GpuCloudParams &cp); // the UBO's cyclones-at-sea block (SatelliteSimWeather.cpp)
    void launchFillPadLights(); // their floodlights / beacons as point sprites (kFireBuf)
    nlohmann::json launchPadsJson() const;
    bool launchGoToPad(const std::string &site, double distM, double azDeg, double heightM, std::string &msg);
    // The next tower catch (0 a ship, 1 a booster; wantDay: the Sun above 10 deg at the pad): the clock leadS before it.
    bool launchGoToLanding(int which, double leadS, double distM, std::string &msg, bool wantDay = false);
    void launchInitBlock(bool keepPlanes = false); // keepPlanes: the roster is unchanged (a Kessler restart)          // the launch block's satOrbits entries (empty) and the launch state (reset)
    void launchUpdate(VulkanContext &ctx, VkCommandBuffer cmd); // per frame: schedule, records, uploads
    void launchFillVisuals(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull); // rockets, plumes, re-entries
    SatOrbitElems launchOrbitElems(uint32_t slot) const;
    uint64_t launchSlotOwner(uint32_t slot, uint32_t &m) const; // a launch slot's owner (batch key * 2 + kind; 0 = free)
    std::string launchLabel(uint32_t slot) const;
    std::string launchStatus(uint32_t slot) const; // "<type> - raising to slot, h/target km" | "- deorbiting" | ""
    nlohmann::json launchStateJson() const;
    nlohmann::json launchListJson(double hours) const;
    nlohmann::json launchWhyJson(double hours) const;
    nlohmann::json launchPadStatsJson(double hours) const; // harness `launch padstats`: same-pad pressure (deployment era)
    bool launchGoToNext(std::string &msg, bool twilight);
    // padDistM > 0 (2026-10-20): the observer on the ground that far from the pad, beside the track (side -1 / +1),
    // looking at the pad; lead = seconds before lift-off.
    bool launchGoToNextWhen(std::string &msg, int when, const std::string &vehicle = std::string(),
                            double padDistM = 0.0, double lead = 30.0, double side = 1.0);
    bool launchLook(int stage, std::string &msg);
    struct LaunchSound
    {
        uint32_t id;
        float level, km, pan; // the thunder synth's event (SatelliteSimAmbience.cpp thunderStep)
        int stage = 0, burn = 0;  // readouts (harness `launch state`): the stage, its burn and T+ as heard now
        float tau = 0.0f, altKm = 0.0f;
    };
    std::vector<LaunchSound> launchSounds(double now, const glm::dvec3 &eye, float camAz) const;
    nlohmann::json launchCheckJson() const;
    nlohmann::json launchJitterJson(int maxBatches); // harness `launch jitter`
    // Mission-operations suite (2026-10-20, harness `launch trace` / `launch formation`; tools/harness/mission_ops.py):
    // the next launch of each (vehicle, program) pair after fromT and its phases measured from the launch model.
    nlohmann::json launchTraceJson(double fromT, double days, const std::string &vehFilter, const std::string &progFilter,
                                   bool trace);
    nlohmann::json launchTraceBatchJson(uint64_t key);
    const void *launchEnsureBatch(uint64_t key); // (an LBatch*: SatelliteSimLaunches.cpp)
    nlohmann::json launchFormationJson(uint64_t key, int samples, bool before = false);
    void launchExactElems(bool on);                 // harness `launch exact on|off`
    void launchLegacyRecords(bool on);              // harness `launch legacy on|off`
    bool launchSelect(int kind, std::string &msg, int member = -1); // member >= 0: only that member of a batch
    bool launchGoToNextReentry(std::string &msg, bool watch = false); // watch: mid-glow, the fireball tracked
    bool launchGoToVent(std::string &msg);
    bool launchGoToShipEntry(std::string &msg);
    bool launchShipDay_ = false; // (harness `launch ship day=1`) the next ship entry over a SUNLIT ground
    bool launchGoToStageEvent(std::string &msg, int ev, const std::string &filter = "", int minMembers = 0);
    // (2026-10-20) The next crew flight event (launch, sep, phasing, arrive, axis, hold400, hold200, hold20, dock,
    // undock, depart, deorbit, entry, chutes, splash) at a station whose name contains `station` (any if empty): the
    // clock leadS before it at 1x, its spacecraft (its rocket before the separation) selected and, follow, followed.
    bool launchGoToCrew(const std::string &station, const std::string &event, double leadS, bool follow, std::string &msg,
                        const std::string &craft = std::string());
    void buildLaunchSettingsSection(const UIInput &inp, UIRenderer &ui);
    void buildLaunchGraphicsSection(const UIInput &inp, UIRenderer &ui); // pads, exhaust sliders, flames, engine light
    void launchDestroy(VkDevice device);

    // ── Missions window (2026-10-19, SatelliteSimMissions.cpp; queries in SatelliteSimLaunches.cpp) ───────────
    // The schedule and what is flying now, outside the settings window: the next launches (Go to / Watch), the next
    // twilight ("jellyfish") and crewed launches pinned, the Sun's Fury last; ongoing crew transfers, crewed stations,
    // rockets in flight and the constellations' trains / retirees; and the LAUNCHES / THE SUN'S FURY controls.
    struct MissionLaunch
    {
        uint64_t key = 0;        // the batch (launchGoToKey)
        double tL = 0.0;         // lift-off, J2000 s
        double tA = 0.0;         // arrival (trains) / docking (crew)
        double sunElevDeg = 0.0; // the Sun at the pad at lift-off
        bool crew = false, twilight = false;
        bool cargo = false;      // a cargo ship to a station (2026-10-21)
        bool firstFill = false;  // the deployment era: a plane's first fill (its slots were empty)
        int people = 0, payload = 0;
        std::string vehicle, program, site, target, spacecraft; // target: the constellation or the station
    };
    // Lift-offs in [t0, t1] (J2000 s), sorted, at most maxN. filter: 0 all, 1 crewed, 2 twilight (the Sun 5-16 deg
    // below the pad's horizon). Builds (and keeps) the batches it needs.
    void launchUpcoming(double t0, double t1, int filter, size_t maxN, std::vector<MissionLaunch> &out);
    double launchUpcomingBudgetMs = 0.0;   // (2026-10-21) > 0: build at most ~this long, then report incomplete
    bool launchUpcomingIncomplete = false; // the last launchUpcoming ran out of its budget (out left unchanged)
    bool msNextPending = false, msPinPending = false; // the Missions rows' refresh to finish next frame
    // Jumps to leadS before the batch's lift-off at 1x. moveObserver: on the ground ~12 km beside the pad (on land
    // where one side is sea), the camera on the pad; else the time only.
    bool launchGoToKey(uint64_t key, bool moveObserver, double leadS, std::string &msg, double distM = 12000.0);
    // WATCH (2026-10-24, the user: "one button that takes them to a rocket launching"): the launch already under way —
    // afterS after lift-off at 1x, on the ground beside the pad, the rocket selected and tracked once listed.
    bool launchWatchKey(uint64_t key, double afterS, std::string &msg);
    double launchWatchLitT = -1.0; int launchWatchScanN = 0; // (harness: a twilight Watch's sunlit search)
    static constexpr double kWatchLiftoffS = 8.0;    // a launch's Watch: lift-off + this (clear of the tower)
    static constexpr double kWatchTwilightS = 300.0; // marks a twilight launch's Watch (from further back)
    static constexpr double kWatchLeadS = 5.0;       // every launch Watch lands this long before lift-off (T-5 s)
    bool launchWatchCatch(std::string &msg); // the next Starship catch, in its landing burn, the ship tracked
    float cloudCoverAtEcef(const glm::dvec3 &dirEcef) const; // the cloud map's cover there (0..1, CPU copy; a Watch's sky)
    struct MissionOngoing
    {
        int cat = 0;            // 0 crew transfer, 1 crewed station, 2 satellite launch in flight, 3 trains raising, 4 retiring
        std::string title, status;
        uint64_t key = 0;       // the batch (0, 2)
        int prog = -1;          // the program (3, 4)
        int sat = -1;           // the station's satellite (1)
        int count = 0;          // satellites (3, 4)
        double tNext = 0.0;     // what the status counts down to (J2000 s; 0 = none)
        double progress = -1.0; // 0..1 (-1 = none)
        bool drawn = false;     // the rocket is drawn this frame (selectable)
    };
    void launchOngoing(std::vector<MissionOngoing> &out); // crew transfers, rockets, trains and retirees (not stations)
    struct CrewStation
    {
        int constIdx = -1, sat = -1, prog = -1;
    };
    std::vector<CrewStation> launchCrewStations();         // the stations a crew program serves
    bool launchSelectRocketOf(uint64_t key, std::string &msg); // the batch's rocket stage (drawn or not, 2026-10-20)
    // The batch's rocket as a transient at time t from the launch model alone (the upper stage once staged, the ship on
    // its entry), whether the list has it or not. False when it no longer exists as a rocket.
    bool launchRocketTransient(uint64_t key, double t, SelTransient &out) const;
    bool launchSelectNearestOf(int prog, int kind, std::string &msg); // the nearest train member (0) / retiree (1)

    // ── Mission milestones (2026-10-20, SatelliteSimLaunches.cpp): a launch's timeline from its batch's phase times
    // (lift-off, max-Q, staging, fairing, SECO, deployment, deorbit, entry, the ship's descent and catch; the booster's
    // recovery burns read off its track), trains (release, raising, arrival), retirees (deorbit, re-entry), and for a
    // roster satellite the batch that put it there and the one that replaces it. The Info window's MISSION section,
    // the follow HUD's Prev / Next, KB_MILESTONE_NEXT / _PREV, Missions' Ongoing rows, harness `missions milestone`.
    struct MissionMilestone
    {
        double t = 0.0;     // J2000 s
        std::string label;
        uint8_t who = 0;    // kMs* bits: whose milestone
        double lead = 10.0; // a skip lands this long before it (less where milestones crowd)
    };
    // kMsStage (2026-10-20): a crewed upper stage after its spacecraft has left (the payload role is the spacecraft then)
    static constexpr uint8_t kMsPayload = 1, kMsBooster = 2, kMsTrain = 4, kMsRetiree = 8, kMsStage = 16;
    struct MissionSubject
    {
        bool ok = false;
        uint64_t key = 0;     // the batch (a roster satellite: the one that will replace it)
        uint64_t keyPrev = 0; // a roster satellite: the batch that put it there
        uint8_t role = 0;     // kMsPayload (the rocket / upper stage / ship), kMsBooster, kMsTrain, kMsRetiree
        int member = -1;      // a train member / retiree: its index in the batch
        int sat = -1;         // its roster slot
        bool roster = false;  // a roster satellite in operations
        bool followed = false;
        std::string title, what;
        std::vector<MissionMilestone> ms; // the subject's, sorted (skips walk these)
        int cur = -1;                     // the last one passed
        double t0 = 0.0, t1 = 0.0;
        std::vector<std::pair<std::string, std::string>> facts;
    };
    launchsim::LBatch *missionBatch(uint64_t key); // built if not cached (a pure function of its key)
    void missionFinish(MissionSubject &s, double now);
    void missionBatchMilestones(const launchsim::LBatch &B, std::vector<MissionMilestone> &ms) const; // every object's
    bool missionSubjectOfBatch(uint64_t key, MissionSubject &out, uint8_t role = kMsPayload);
    bool missionRosterBatches(int sat, double t, uint64_t &keyPrev, uint64_t &keyNext, int &member);
    bool missionSubjectOfIndex(int satIndex, MissionSubject &out); // a transient slot, a launch record, a roster satellite
    bool missionSubjectOfIndexRaw(int satIndex, MissionSubject &out); // ... without the facts rows
    void missionFacts(MissionSubject &s, int satIndex, double t);
    bool missionSubjectNow(MissionSubject &out);                    // Go to's object, else the selection's
    // To just before milestone `index` (>= 0), else the next (dir +1) / previous (-1) one, at 1x; the selection and Go
    // to stay on the subject's object at the new time (a stage, its booster, a train member's record, its roster slot).
    bool missionJump(const MissionSubject &s, int dir, int index, std::string &msg);
    bool missionJumpNow(int dir, std::string &msg);
    void missionAttachAt(const MissionSubject &s, double t, bool sel, bool fol);
    void missionAttachAfterJump(); // after launchUpdate: a launch record's new slot
    nlohmann::json missionSubjectJson(const MissionSubject &s) const;
    uint64_t msAttachOwner = 0;
    uint16_t msAttachM = 0;
    bool msAttachSel = false, msAttachFollow = false;
    int msAttachTries = 0, msAttachFallback = -1;
    MissionSubject infoMission_, followMission_; // the Info window's MISSION section, the follow HUD (refreshed 4x a s)
    double infoMissionWall_ = -1e300, followMissionWall_ = -1e300;
    int infoMissionIdx_ = -2;
    bool hovFollowMsPrev = false, hovFollowMsNext = false;
    void buildMissionTimeline(const UIInput &inp, UIRenderer &ui, const MissionSubject &s, float lw, int &jumpIdx,
                              int &jumpDir); // the Info window's MISSION section body (SatelliteSimUI.cpp)
    std::deque<std::string> msTimelineText_;  // its strings this frame (a deque never moves them)

    WindowChrome msChrome;
    bool hovMsClose = false, hovTimeMissions = false;
    int msTab = 0;                       // 0 upcoming, 1 ongoing, 2 the Kessler tracker (was Controls until 2026-10-21)
    int msUpcomingActs_ = 0, msOngoingActs_ = 0; // Go to / Watch / Fury on Upcoming, any button on Ongoing (the tutorial)
    int msBuildOutOpen = -1;             // Ongoing's BUILD-OUT list: -1 = auto (collapsed once every cohort is complete)
    double msNextWall = -1e300, msNextSim = -1e300; // when the next-launches rows were made (wall clock, sim time)
    double msPinWall = -1e300, msPinSim = -1e300;   // ... the pinned twilight / crewed rows
    std::vector<MissionLaunch> msNext;   // the next launches
    std::vector<MissionLaunch> msCrewList; // crewed flights in the next ~400 days (the stations' next flights too)
    MissionLaunch msTwilight;
    bool msHasTwilight = false;
    std::vector<MissionOngoing> msOngoing;
    std::vector<CrewStation> msStations;
    std::string msStatus;
    std::string msStatusShown;           // msStatus as first drawn, and when (wall clock): it fades after 12 s
    double msStatusWall = 0.0;
    std::deque<std::string> msText_;     // this frame's formatted strings (Clay keeps the pointers; a deque never moves them)
    const char *msStr(const char *fmt, ...);
    void missionsRefresh(bool force);
    bool missionsGoToFury(int which, std::string &msg); // 0 the CME's arrival (dusk over Monument Valley), 1 the storm night
    bool missionsGo(const std::string &what, bool watchOnly, std::string &msg); // harness: an index, twilight, crew, fury, fury_night
    bool missionsWatch(const std::string &what, std::string &msg); // the window's Watch: the launch under way, tracked
    // An Ongoing row's object (msOngoing[row]): the rocket of a crew transfer / satellite launch, the nearest raising
    // train member, the nearest retiree. Selected by key / index whether drawn or not; go = also Go to it (2026-10-20).
    bool missionsOngoingAct(int row, bool go, std::string &msg);
    bool missionsMilestone(int row, int dir, int index, std::string &msg); // an Ongoing row's next / previous milestone
    nlohmann::json missionsStateJson();
    void buildMissionsWindow(const UIInput &inp, UIRenderer &ui);

    // ── The Sky events window (2026-10-27, SatelliteSimSkyEvents.cpp; CLAUDE.md "Sky events window") ─────────
    // Celestial events: solar and lunar eclipses, conjunctions (two planets, or a planet and the Moon), lunar
    // occultations of the bright planets. One fixed-height card per kind (the next one) and the next ~10 events of
    // every kind within two years, each with ONE Watch (the event already happening, in frame, 1x). The kinds are a
    // table (kSkyKinds): a new kind is a search + a Watch + a card text. The searches run on a worker (skScanJob),
    // one kind after another, over [base - 4 h, base + 2 y], restarted when sim time leaves [base - 1 h, base + 30 d].
    // The sky-events button on the right HUD panel, key K (KB_SKY_EVENTS), harness `ui open skyevents` / `skyevents`.
    friend struct SkyEventsFriend;           // SatelliteSimSkyEvents.cpp: the kind table and its helpers
    WindowChrome skChrome;
    bool hovSkClose = false, hovTimeSky = false;
    struct SkyScanResult
    {
        double base = 0.0;
        int kind = 0;
        std::vector<sky::Event> events;
        double ms = 0.0;
    };
    std::future<SkyScanResult> skScanJob;
    std::atomic<bool> skScanCancel{false};
    bool skScanJobStale = false;
    double skScanBase = -1e300;              // the search's start; < -1e299 = never searched
    int skScanKind = 0;                      // the next kind to search (kKindCount = done)
    double skScanMs[sky::kKindCount] = {};   // each kind's search time on the worker (state)
    int skScanRestarts = 0;
    std::vector<sky::Event> skFound[sky::kKindCount]; // every event of each kind in the range, by time
    std::vector<sky::Event> skUpcoming;      // the next events of every kind (rebuilt when a kind comes in, or 2 s)
    double skUpcomingWall = -1e300, skUpcomingSim = -1e300;
    int skPendingKind = -1, skPendingRow = -1; // a Watch pressed this frame (acted on after the layout)
    std::string skStatus, skStatusShown;
    double skStatusWall = 0.0;
    std::deque<std::string> skText_;
    const char *skStr(const char *fmt, ...);
    // The Watch's aim kept on the bodies while the player does not move the camera (the sky turns ~0.25 deg a minute):
    int skAimA = -1, skAimB = -1;           // body ids (sky::kBodyMoon / kBodySun / a planet), -1 = none
    double skAimUntil = 0.0;                // sim time it stops
    float skAimAz = 0.0f, skAimEl = 0.0f;   // what it set last frame (a difference = the player looked away)
    void skyEventsUpdate();                  // the worker: take a finished kind, start the next; restart on a jump
    void skyEventsFinish(double maxS);       // the harness: the searches run to their end
    void skyEventsRefreshUpcoming(bool force);
    void skyEventsTick();                    // buildUI, after updateTrack: the kept aim
    bool skyEventWatch(const sky::Event &e, std::string &msg); // dispatches to the kind's Watch
    const sky::Event *skyEventNext(int kind) const; // the kind's card: the next one not over (nullptr = none)
    void buildSkyEventsWindow(const UIInput &inp, UIRenderer &ui);
    nlohmann::json skyEventsJson();
    harness::Status harnessSkyEvents(harness::Active &a);
    // The kinds (one search + one Watch + the card's lines each; the table in SatelliteSimSkyEvents.cpp)
    void skySearchSolar(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    void skySearchLunar(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    void skySearchConjunctions(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    void skySearchOccultations(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    bool skyWatchSolar(const sky::Event &e, std::string &msg);
    bool skyWatchLunar(const sky::Event &e, std::string &msg);
    bool skyWatchConjunction(const sky::Event &e, std::string &msg);
    bool skyWatchOccultation(const sky::Event &e, std::string &msg);
    void skySearchMoonGlints(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    bool skyWatchMoonGlint(const sky::Event &e, std::string &msg);
    void skySearchSupernova(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    bool skyWatchSupernova(const sky::Event &e, std::string &msg);

    // ── Moon-base glints (2026-10-27, SatelliteSimMoonBases.cpp; CLAUDE.md "Moon-base glints") ──────────────────
    // The lunar south-pole bases' sun-tracking solar arrays (data/lunar_bases.json, lunar::Base) mirror the Sun to
    // the Earth near full Moon: a point source on the drawn Moon at the base's place, appended to kFireBuf (point,
    // bloom, glare). The bases are read once at init and never written after (the sky-event search reads them on a
    // worker).
    std::vector<lunar::Base> lunarBases_;
    bool moonBaseGlints = true;           // moon_bases.enabled
    float moonBaseJitter = 1.0f;          // moon_bases.tilt_jitter_scale: x the rows' tilt / azimuth errors
    float moonBaseMagLimit = 3.0f;        // moon_bases.event_mag_limit: a sky event when brighter than this
    struct MoonBaseNow                    // this frame, per base (harness `moonbase`)
    {
        bool built = false, sunUp = false, drawn = false;
        double magTop = 99.0, magSeen = 99.0, extinction = 0.0, flare = 0.0, rowsLit = 0.0;
        double obsElevDeg = 0.0, phaseDeg = 0.0, beamOffDeg = 0.0, sunElevDeg = 0.0, sunVisible = 0.0;
        glm::vec3 enu{0.0f, 0.0f, 1.0f};   // drawn, refracted (ENU)
    };
    std::vector<MoonBaseNow> moonBaseNow_;
    double moonBaseCpuMs = 0.0;            // the fill's time a frame (eased; harness moonbase -> cpu_ms)
    void moonBasesLoad();
    void moonBasesFill();                  // kesslerFillDust's finish: the glints into kFireBuf
    // The base's place on the DRAWN Moon (x "Moon size"), refracted, from the observer (ENU); false if not loaded.
    bool moonBaseDrawnEnu(int i, glm::vec3 &enu) const;
    harness::Status harnessMoonBase(harness::Active &a);
    // A selected base's glint (2026-10-28, issue 20261028_192740; SatelliteSimMoonBases.cpp): picked by a click on its
    // drawn glint, a readout panel like a star's with Track. Gives way to any other selection (buildSelectedSatPanel).
    int selectedMoonBase = -1;
    char moonBaseInfoLine[kSelInfoLines][kSelInfoLineLen] = {};
    std::vector<lunar::Window> moonBaseWin_;      // the selected base's glint windows round now (cached)
    int moonBaseWinFor = -1;
    double moonBaseWinT0 = 0.0, moonBaseWinT1 = 0.0;
    int pickMoonBaseAt(float clickX, float clickY, float screenW, float screenH) const;
    void formatSelectedMoonBaseInfo();
    // The whole click / centre-select chain at a screen point (comet, Moon-base glint, planet, star, satellite), the one
    // used by the mouse, the select key and harness `select pick`. Returns what it selected ("" = nothing new).
    std::string pickAndSelectAt(float x, float y, float screenW, float screenH);

    // ── Meteors (2026-10-27, Meteors.h, SatelliteSimMeteors.cpp; CLAUDE.md "Meteors") ──────────────────────────────
    // The major annual showers + the sporadic background, generated as a pure function of sim time near the observer
    // (meteors::generate), drawn each frame in kesslerFillDust's finish: heads as points in kFireBuf (bloom, glare),
    // their streaks over the eye's persistence as emissive Gaussians in the dust list, fragments, persistent trains.
    bool meteorsEnabled = true;           // meteors.enabled
    float meteorRateGain = 1.0f;          // meteors.rate (x the showers' flux)
    float meteorFireballGain = 1.0f;      // meteors.fireball_rate (x the rate of fireballs / bolides, M <= -4)
    float meteorTrainDuration = 0.3f;     // meteors.train_duration (x the persistent trains' decay time; 1 = the old)
    // Time-lapse draw past 2x (2026-10-28, issue 20261028_220640): every meteor that burned during the frame's sim-time
    // interval is drawn as its streak over that interval, its light integrated and divided by the exposure
    // min(interval, timelapse_exposure_s) (>= the eye's persistence). meteors.timelapse false = the old cut-off.
    bool meteorTimelapse = true;          // meteors.timelapse
    float meteorTimelapseExposureS = 0.1f; // meteors.timelapse_exposure_s
    double meteorPrevT_ = 0.0;            // last frame's sim time (the interval)
    bool meteorLapse_ = false;            // this frame: time-lapse
    double meteorLapseDt_ = 0.0, meteorLapseE_ = 0.0, meteorDrawsEst_ = 0.0, meteorGenMs_ = 0.0, meteorRankMs_ = 0.0;
    std::vector<meteors::Meteor> meteorForced_; // harness `meteors force`
    // The generated meteors per (shower, band, time bin), kept while the eye stays within 30 km of where they were
    // generated: each frame generates only the bins that have just begun (a bin is 2 s for the faint bands).
    std::unordered_map<uint64_t, std::vector<meteors::Meteor>> meteorBinCache_;
    glm::dvec3 meteorCacheEye_{0.0};
    double meteorCacheGain_[2] = {-1.0, -1.0};
    uint64_t meteorForcedCount_ = 0;
    struct MeteorNow                      // drawn this frame (harness `meteors`)
    {
        int shower = 0;
        float M = 0.0f, mag = 99.0f, flare = 0.0f, azDeg = 0.0f, elDeg = 0.0f, age = 0.0f, dur = 0.0f, vKms = 0.0f;
        float rangeKm = 0.0f, hKm = 0.0f;
        int fragments = 0, trainSegs = 0, gauss = 0;
        bool forced = false;
    };
    std::vector<MeteorNow> meteorNow_;
    double meteorCpuMs = 0.0;             // the fill's time a frame (eased)
    int meteorDraws_ = 0, meteorGenerated_ = 0, meteorHeads_ = 0, meteorGauss_ = 0, meteorDustAt_ = 0;
    bool meteorWarpOff_ = false;          // this frame: not drawn (time warp)
    int meteorBandMax_ = 5;               // this frame: the faintest band drawn (time-lapse: as the draw budget allows)
    static constexpr double kMeteorCacheRadiusM = 30000.0; // the bin cache serves every eye within this of its eye
    void meteorsFill(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull); // kesslerFillDust's finish
    float meteorSkySuppress(const glm::vec3 &enu) const; // the stars' sky dimming (night, dome, Moon) in that direction
    harness::Status harnessMeteors(harness::Active &a);
    // The Watch: a dark, clear site with the radiant well up, near the shower's peak (within [t0, t1]), at 1x.
    bool meteorWatchShower(int shower, double t0, double t1, double tPeak, std::string &msg);
    void skySearchMeteorShowers(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    bool skyWatchMeteorShower(const sky::Event &e, std::string &msg);
    // ── Comets (2026-10-27, SatelliteSimComets.cpp + Comets.h; CLAUDE.md "Comets") ──────────────────────────────
    // The Great Comet of 2037 (a scripted sungrazer passing 560,000 km from the Earth) and minor comets hashed per year,
    // on heliocentric conics (pure functions of sim time). The head is a point in kFireBuf (point, bloom, glare); the
    // tails are 3D particle clouds (comet::buildParticles: dust grains under mu (1 - beta), ion parcels in the solar
    // wind) splatted into a half-res target (comet_splat.vert/.frag, cometsRecordSplat) and composited with the coma
    // (comet_composite.frag) after the sky, at infinity, gated against the sky's surface brightness (darksky.glsl).
    struct CometGeom
    {
        comet::State s;
        double r = 1.0, delta = 1.0, phaseDeg = 90.0, elongDeg = 90.0, mag = 99.0;
        double comaRad = 0.0, ionLenAU = 0.0, tailDeg = 0.0;
        glm::dvec3 toComet{0.0, 0.0, 1.0}; // ecliptic unit vector from the observer
    };
    struct CometNow
    {
        int idx = -1;                       // into comets_
        CometGeom g;
        double pointShare = 1.0, magPoint = 99.0, magSeen = 99.0, extMag = 0.0, flare = 0.0;
        glm::vec3 headEnu{0, 0, 1}, headEnuDrawn{0, 0, 1};
        bool below = false, drawnHead = false;
        int tailSlot = -1;
        float peakLum = 0.0f;
        std::shared_ptr<const comet::ParticleBuild> build; // this frame's particles (tailSlot >= 0)
        glm::vec3 tailAimEnu{0, 0, 1};   // the visible tail's far end (lum > ~23.7 mag/arcsec^2), for the Watch's framing
        float tailSpanDeg = 0.0f;        // ... and its angle from the head
    };
    struct CometTailCache
    {
        std::string name;
        std::shared_ptr<const comet::ParticleBuild> cur;          // the build drawn
        std::future<std::shared_ptr<const comet::ParticleBuild>> job;
        double jobT = 0.0;
        int uploadedSlot = -1;                                    // the GPU region holding cur (-1 none)
        double uploadedT = -1e300;
        bool used = false;
    };
    bool cometsEnabled = true;               // comets.enabled
    bool cometsMinor = true;                 // comets.minor: the hashed minor comets as well as the Great Comet
    bool cometTailsEnabled = true;           // comets.tails
    float cometTailGain = 1.0f;              // comets.tail_gain: x the tails' physical brightness
    float cometTailGainDisplay = 1.0f;       // comets.display_gain: x what the tail pass shows (exposure-like)
    int cometTailDebug = 0;                  // comets.tail_debug: 1 the detail field's emission time, 2 its weight (no UI)
    float cometTailDetail = 1.0f;            // comets.tail_detail: the dust tail's zoom detail (bands, striae; 0 = off)
    std::vector<comet::Elements> comets_;    // the catalog near now
    double cometsCatalogT = -1e300;
    std::vector<CometNow> cometNow_;         // this frame
    std::vector<CometTailCache> cometTails_;
    float cometMoonBright = 0.0f, cometAtmFrac = 1.0f;
    double cometCpuMs = 0.0;
    uint64_t cometTailBuilds = 0;
    uint32_t cometDrawsLast = 0;
    uint32_t cometGrainN[3] = {0, 0, 0};     // grains in each GPU region (comet tail slot)
    uint64_t cometUploadBytes = 0;
    double cometLastBuildMs = 0.0;
    uint64_t cometSyncBuilds = 0;
    double cometTailPrevT = -1e300, cometTailPrevDt = 0.0; // the last frame's sim time and step (a jump vs warp)
    bool cometParticlesSync = false;         // comets.particles_sync: build on the frame (harness videos: exact)
    float cometSplatScale = 0.5f;            // comets.splat_scale: the splat target's size x the swapchain's
    // the splat pass: a half-res RGBA16F target the particles are added into, composited by comet_composite.frag
    VkImage cometSplatImg = VK_NULL_HANDLE;
    VkDeviceMemory cometSplatMem = VK_NULL_HANDLE;
    VkImageView cometSplatView = VK_NULL_HANDLE;
    VkSampler cometSplatSampler = VK_NULL_HANDLE;
    VkRenderPass cometSplatRenderPass = VK_NULL_HANDLE;
    VkFramebuffer cometSplatFramebuffer = VK_NULL_HANDLE;
    VkExtent2D cometSplatExtent{0, 0};       // level 0 of the splat atlas (the half-res screen)
    VkExtent2D cometSplatAtlas{0, 0};        // the atlas: level 0 + levels 1-5 (each half the last) to its right
    VkImage cometFieldImg = VK_NULL_HANDLE;  // the dust's (emission time, ln beta) field, light-weighted (RGBA32F)
    VkDeviceMemory cometFieldMem = VK_NULL_HANDLE;
    VkImageView cometFieldView = VK_NULL_HANDLE;
    VkExtent2D cometFieldExtent{0, 0}, cometFieldAtlas{0, 0}; // the field atlas: half the splat atlas
    VkRenderPass cometFieldRenderPass = VK_NULL_HANDLE;
    VkFramebuffer cometFieldFramebuffer = VK_NULL_HANDLE;
    VkPipeline cometFieldPipeline = VK_NULL_HANDLE;
    VkPipelineLayout cometSplatPipeLayout = VK_NULL_HANDLE;
    VkPipeline cometSplatPipeline = VK_NULL_HANDLE;
    bool cometSplatDrawn = false;            // this frame's splat pass ran (the composite reads it)
    void cometsCreateSplatTarget(VulkanContext &ctx);
    void cometsDestroySplatTarget(VkDevice device);
    void cometsRecordSplat(VkCommandBuffer cmd, VulkanContext &ctx); // recordCompute's end, outside any pass
    VkBuffer cometBuf = VK_NULL_HANDLE;
    VkDeviceMemory cometMem = VK_NULL_HANDLE;
    void *cometMapped = nullptr;
    VkDescriptorSetLayout cometDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool cometDescPool = VK_NULL_HANDLE;
    VkDescriptorSet cometDescSet = VK_NULL_HANDLE;
    VkPipelineLayout cometPipeLayout = VK_NULL_HANDLE;
    VkPipeline cometPipeline = VK_NULL_HANDLE;
    int selectedCometIndex = -1;
    char cometInfoLine[kSelInfoLines][kSelInfoLineLen] = {};
    void cometGeometry(const comet::Elements &el, double t, const glm::dvec3 &obsHelio, CometGeom &g) const;
    glm::dvec3 cometObserverHelio(double t) const;
    glm::vec3 cometEnu(const glm::dvec3 &dirEcl, double t) const;
    glm::vec3 cometHeadEnu(int i) const;
    glm::vec3 cometTailEnu(int i) const;
    int cometIndexByName(const char *name) const;
    void cometsRefreshCatalog(double t, bool force);
    void cometsUpdate();                     // recordCompute, after updatePlanets
    void cometsBuildTails(double t, const glm::dvec3 &obsHelio);
    void cometsFillHeads();                  // kesslerFillDust's finish: the heads into kFireBuf
    void cometsInit(VulkanContext &ctx);
    void cometsWriteDescriptors(VulkanContext &ctx); // also after the cloud / depth targets are recreated
    void cometsRecordDraw(VkCommandBuffer cmd, VulkanContext &ctx, const PointDrawPC &pc);
    void cometsDestroy(VkDevice device);
    int pickCometAt(float x, float y, float w, float h);
    void formatSelectedCometInfo();
    bool cometWatchAt(int idx, double t, double sunMaxDeg, bool fromOrbit, std::string &msg);
    bool cometBestWindow(const comet::Elements &el, double t0, double t1, double &tBest, double &w0, double &w1,
                         double &magBest, double &tailBest) const;
    harness::Status harnessComets(harness::Active &a);
    // ── The inertial (sidereal) camera lock (2026-10-28, SatelliteSimInertial.cpp; CLAUDE.md "Inertial camera lock") ──
    bool inertialActive = false;
    bool inertialHold = false;               // the eye held fixed in ECI too (from orbit the Earth turns beneath)
    bool inertialTickApplyNow_ = false;
    bool inertialOcculter = false;           // a coronagraph: the Sun's disc, glare and bloom hidden while locked
    glm::dvec3 inertialEyeEci{0.0};          // m, geocentric ECI
    glm::dvec3 inertialViewEci{0.0, 0.0, 1.0};
    glm::vec3 inertialLastObsDir{0.0f};
    float inertialLastAz = 0.0f, inertialLastEl = 0.0f;
    double inertialLastTheta = 0.0;
    glm::dvec3 inertialCamDirEcef() const;
    void inertialStart(bool hold);
    void inertialStop();
    void inertialTick();                     // recordCompute, after the time advance (and updateFollow)
    harness::Status harnessInertial(harness::Active &a);
    void skySearchComets(const sky::SearchCtx &c, std::vector<sky::Event> &out) const;
    bool skyWatchComet(const sky::Event &e, std::string &msg);

    // shared by the Watches: the moment (1x, unpaused), the place, the aim and the selection
    void skyJumpTo(double t);
    void skyPlaceAt(const glm::dvec3 &siteEcef, double t);
    glm::vec3 skyBodyEnu(int body) const;    // after updatePositions: the body's direction from the observer (ENU)
    void skyAimAt(int a, int b, float fovDeg, double until);

    // ── The Weather window (2026-10-23, SatelliteSimWeatherWindow.cpp) ──────────────────────────────────────
    // Like Missions, for the storm objects (Weather.h): Radar (a local precipitation radar centred on the observer,
    // computed on the GPU from the same field the cloud march draws: shaders/weather_radar.comp), Storms (the storms
    // near the observer and the strongest worldwide: kind, stage, age, time left, distance, lightning; Go to /
    // Watch) and Controls (the storm-object settings). The weather-cloud button on the right HUD panel, key N
    // (KB_WEATHER), harness `ui open weather` / `weather ...`. Nothing runs while it is closed.
    WindowChrome wxChrome;
    bool hovWxClose = false, hovTimeWeather = false;
    int wxTab = 1;                       // 0 radar, 1 storms (drawn first, the default), 2 controls, 3 cyclones
    int wxRangeIdx = 1;                  // the radar's radius: kWxRangesKm[idx]
    static constexpr float kWxRangesKm[3] = {50.0f, 150.0f, 400.0f};
    bool wxShowClouds = true, wxShowLabels = true, wxLoop = false;
    std::string wxStatus, wxStatusShown;
    double wxStatusWall = 0.0;
    std::deque<std::string> wxText_;     // this frame's formatted strings (Clay keeps the pointers)
    const char *wxStr(const char *fmt, ...);
    void buildWeatherWindow(const UIInput &inp, UIRenderer &ui);
    void buildWeatherRadarTab(const UIInput &inp, UIRenderer &ui, float bodyW, float bodyH);
    void buildWeatherStormsTab(const UIInput &inp, UIRenderer &ui);
    void buildWeatherControlsTab(const UIInput &inp, UIRenderer &ui);
    // The storms (rows rebuilt at most twice a second, or after a jump in sim time).
    struct WxStormRow
    {
        uint64_t id = 0;
        int kind = 0;                    // wx::StormKind (an unknown value is named generically)
        double t0 = 0.0, tEnd = 0.0;     // birth, end (J2000 s)
        double tMature = 0.0;            // the moment worth watching: the dominant tower grown, its anvil spread
        glm::dvec3 e{0.0, 0.0, 1.0};     // Earth-fixed centre now
        double distKm = 0.0, bearingDeg = 0.0;
        float strength = 0.0f;           // the strongest tower now
        int towersAlive = 0, towerCount = 0;
        int stage = 0;                   // 0 forming, 1 growing, 2 mature, 3 decaying
        float flashesPerMin = 0.0f;      // estimate: lightning rate x the dominant towers' activity (cloud_v2_lightning.comp)
        float score = 0.0f;
        // Milestones (2026-10-23): its tornado's touchdown / lift (0 none: SatelliteSim::tornadoOf), and when a line
        // system's convective line passes over the observer where they stand now (0 none within its life).
        double tTorn0 = 0.0, tTorn1 = 0.0;
        double tPass = 0.0;
    };
    std::vector<WxStormRow> wxNear, wxWorld;  // within kWxNearKm, nearest first; the strongest worldwide
    int wxStormsAlive = 0;
    double wxRowsWall = -1e300, wxRowsSim = -1e300;
    void weatherRefreshRows(bool force);
    static void weatherStormState(const wx::Storm &st, double t, float lightningRate, WxStormRow &r);
    bool weatherStormRowCached(uint64_t id, WxStormRow &out);           // this refresh's row, else computed now
    bool wxWorldAllKinds = false;        // the worldwide list: organised storms only (off) or every cell
    uint64_t wxSelStorm = 0;             // the storm picked on the radar / in the list (highlighted)
    int wxPendingAct = 0;                // 1 Go to, 2 Watch: applied after the layout
    uint64_t wxPendingId = 0;
    bool wxRadarRanCapable = false;      // the clouds' pass ran (the radar can be dispatched)
    int wxLoopShownPos = -1;
    bool weatherStormNow(uint64_t id, double t, WxStormRow &out);       // a storm's row at time t (false: not alive)
    bool weatherGoToStorm(uint64_t id, std::string &msg, double distM = 50000.0, float aglM = -1.0f, float elDeg = 8.0f); // stand upwind of its strongest tower
    bool weatherWatchStormNow(uint64_t id, std::string &msg);          // the window's one button: the storm happening, in frame
    bool weatherWatchStorm(uint64_t id, std::string &msg);              // its mature stage at 1x, then Go to
    bool weatherWatchTornado(uint64_t id, std::string &msg);            // a minute before its touchdown, 6 km on its inflow side
    bool weatherWatchPassage(uint64_t id, std::string &msg);            // 20 min before its line reaches you, looking at it
    void weatherStormMilestones(WxStormRow &r);                         // tTorn0/1, tPass for a row
    double weatherLinePassage(const wx::Storm &st, double from) const;  // when its line passes over the observer (0 none)
    glm::dvec3 weatherMapToEarth(const glm::dvec3 &mapDir, double t) const;
    nlohmann::json weatherWindowJson();                                 // harness `weather state`
    // The radar: GPU pass (set 0 = the cloud march's, set 1 = the output), read back the next frame.
    static constexpr int kWxN = 256;     // cells a side
    VkPipeline wxRadarPipe = VK_NULL_HANDLE;
    VkPipelineLayout wxRadarPipeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout wxRadarSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool wxRadarPool = VK_NULL_HANDLE;
    VkDescriptorSet wxRadarSet = VK_NULL_HANDLE;
    VkBuffer wxRadarBuf = VK_NULL_HANDLE;
    VkDeviceMemory wxRadarMem = VK_NULL_HANDLE;
    void *wxRadarMapped = nullptr;
    VkQueryPool wxRadarQuery = VK_NULL_HANDLE;
    bool wxRadarFailed = false;
    bool wxRadarWanted = false;          // buildUI asks, recordCompute dispatches
    bool wxRadarInFlight = false;
    uint64_t wxRadarFrame = 0;           // the frame index that dispatched it        // dispatched last frame: read back now
    double wxRadarDispWall = -1e300;     // when the last one was dispatched (wall)
    double wxRadarDispSim = -1e300;      // ... at this sim time
    glm::dvec3 wxRadarDispObs{0.0};      // ... centred here
    float wxRadarDispCellM = 0.0f;
    float wxRadarGpuMs = 0.0f, wxRadarCpuMs = 0.0f;
    int wxRadarDispatches = 0;
    void weatherRadarEnsure(VulkanContext &ctx);
    void weatherRadarRecord(VkCommandBuffer cmd);                       // recordCompute, after the clouds' pass
    void weatherRadarDestroy(VkDevice device);
    // The last frame read back (what is shown), and the loop's history (rain only).
    struct WxRadarFrame
    {
        std::vector<float> rain, cover, ground, core;
        double simT = 0.0;
        glm::dvec3 obs{0.0, 0.0, 1.0};
        float cellM = 0.0f;
        bool valid = false;
    };
    WxRadarFrame wxShown;
    std::deque<WxRadarFrame> wxLoopFrames;
    int wxLoopPos = -1;                  // the loop frame shown (-1 = the latest)
    double wxLoopWall = 0.0;
    void weatherRadarReadback();
    void weatherRadarRaster(const WxRadarFrame &f);
    // A radar dispatch for the program itself (2026-10-24): 50-km radius, read back into wxProbeFrame (not shown). The
    // tornado views use it to stand where the renderer's own rain is not (a CPU estimate from towers and shields stood
    // in full rain: the map's rain and the envelope's decks are only on the GPU).
    bool wxRadarProbe = false, wxRadarDispProbe = false;
    int wxRadarProbeRange = 0;
    WxRadarFrame wxProbeFrame;
    struct WxTornFix
    {
        int stage = 0;          // 0 idle, 1 waiting for the weather cube, 2 probe asked, 3 probe dispatched
        int frames = 0;
        glm::dvec3 te{0, 0, 1}, up{0, 0, 1};
        bool close = false;
        float aglM = 60.0f, elDeg = 10.0f;
        double d0 = 4500.0, dStep = 2000.0;   // the candidate rings' first distance and spacing (m)
        double losMax = 0.8;                  // the line of sight's share scored for rain (the subject's own core left out)
        int rangeIdx = 0;                     // the probe's radius: kWxRangesKm[rangeIdx]
        const char *what = "tornado";
        double before = -1.0, after = -1.0, distKm = 0.0;   // (harness: rain at the first and the chosen spot)
    } wxTornFix;
    glm::dvec3 wxStormLastTE{0, 0, 1}, wxStormLastUp{0, 0, 1};   // weatherGoToStorm's target and the side it stood on
    void weatherTornadoFixTick();
    float weatherProbeRainAt(const glm::dvec3 &e) const;
    // The image (a CPU raster uploaded in recordCompute, as the Kessler tracker's globe).
    VkImage wxImg = VK_NULL_HANDLE;
    VkDeviceMemory wxImgMem = VK_NULL_HANDLE;
    VkImageView wxImgView = VK_NULL_HANDLE;
    VkSampler wxImgSampler = VK_NULL_HANDLE;
    VkBuffer wxStage = VK_NULL_HANDLE;
    VkDeviceMemory wxStageMem = VK_NULL_HANDLE;
    void *wxStageMapped = nullptr;
    uint32_t wxUiId = 0;
    bool wxUploadPending = false, wxImageFailed = false;
    bool weatherRadarEnsureImage(UIRenderer &ui);
    void weatherRadarRecordUpload(VkCommandBuffer cmd);
    UIImage wxImgEl;
    std::vector<UITextQuad> wxQuads;
    std::vector<UITextRun> wxRuns;
    std::deque<std::string> wxRunText;
    UITextBlock wxMarks;
    struct WxRadarStats
    {
        float maxRain = 0.0f, maxDbz = 0.0f, rainShare = 0.0f, coreShare = 0.0f;
        int raining = 0, snowing = 0, stormsOnMap = 0;
        float centreRain = 0.0f, eyeTempC = 0.0f;
    } wxStats;
    int wxHoverStorm = -1;               // the storm marker under the pointer (index into wxMapStorms)
    struct WxMapStorm { float x, y; uint64_t id; int kind; int rank; float strength; };
    std::vector<WxMapStorm> wxMapStorms;
    // ── Named storms (2026-10-23, SatelliteSimWeatherCyclones.cpp): the tropical cyclones (wx::Cyclone) ────────────
    // Copies (the catalog's seasons can be rebuilt under a pointer): the active ones, the selected basin's season (this
    // year and the next, within ~200 days of now), the next genesis in every basin. Rebuilt every 2 s or after a jump.
    std::vector<wx::Cyclone> wxCycActive, wxCycSeason, wxCycNext;
    std::vector<wx::Cyclone> wxCycFeatured;   // the Storms tab's featured hurricane slot's copies (its Go to)
    int wxCycBasin = -1;                 // the season list's basin (-1: the strongest active one's, else North Atlantic)
    uint64_t wxSelCyclone = 0;           // the cyclone shown with its milestones (and drawn highlighted)
    bool wxCycZoom = true;               // the map: the selected cyclone's region (true) or the tropics
    double wxCycWall = -1e300, wxCycSim = -1e300;
    uint64_t wxCycEnvKey = 0;
    void weatherRefreshCyclones(bool force);
    const wx::Cyclone *weatherCycloneById(uint64_t id) const;           // from the copies above
    struct WxMilestone { double t; std::string label; int kind; };       // kind: 0 genesis .. 7 dissipation, 6 landfall
    void weatherCycloneMilestones(const wx::Cyclone &c, std::vector<WxMilestone> &out) const;
    // Go to a cyclone now: 0 near (60 km behind its motion, 1.5 km up, as `storms go cyclone=`), 1 from orbit over the
    // eye, 2 in the eye, 3 its (next, else first) landfall point; Watch: its peak (genesis while upcoming) from orbit;
    // a milestone: the clock there at 1x, the camera on the storm (a landfall from the coast, 2 h before).
    bool weatherGoToCyclone(uint64_t id, int mode, std::string &msg);
    bool weatherWatchCyclone(uint64_t id, std::string &msg);
    bool weatherCycloneMilestone(uint64_t id, int index, std::string &msg);
    void buildWeatherCyclonesTab(const UIInput &inp, UIRenderer &ui, float bodyW);
    nlohmann::json weatherCyclonesJson();
    int wxCycPendingAct = 0, wxCycPendingArg = 0;   // 1 go (arg = mode), 2 watch, 3 milestone (arg = index)
    uint64_t wxCycPendingId = 0;
    // The track map: an equirect land / sea raster (re-made only when its box changes), the tracks as overlay quads.
    static constexpr int kWxMapW = 512, kWxMapH = 256;
    double wxMapBox[4] = {-180.0, -60.0, 180.0, 60.0};   // lon0, lat0, lon1, lat1 shown
    double wxMapRastered[4] = {0, 0, 0, 0};
    VkImage wxMapImg = VK_NULL_HANDLE;
    VkDeviceMemory wxMapImgMem = VK_NULL_HANDLE;
    VkImageView wxMapView = VK_NULL_HANDLE;
    VkSampler wxMapSampler = VK_NULL_HANDLE;
    VkBuffer wxMapStage = VK_NULL_HANDLE;
    VkDeviceMemory wxMapStageMem = VK_NULL_HANDLE;
    void *wxMapStageMapped = nullptr;
    uint32_t wxMapUiId = 0;
    bool wxMapUploadPending = false, wxMapFailed = false;
    float wxMapCpuMs = 0.0f;
    bool weatherMapEnsureImage(UIRenderer &ui);
    void weatherMapRaster();
    UIImage wxMapEl;
    std::vector<UITextQuad> wxMapQuads;
    std::vector<UITextRun> wxMapRuns;
    std::deque<std::string> wxMapRunText;
    UITextBlock wxMapMarks;
    struct WxMapCyc { float x, y; uint64_t id; };
    std::vector<WxMapCyc> wxMapCycs;
    int wxMapHover = -1;
    static constexpr VkDeviceSize kStageBytes = 16ull << 20;
    uint32_t kPoolCap = 16384;            // records the GPU buffers hold
    // Satellite indices: [0, activeSatCount) the roster, then kLaunchCap launch records (trains raising, retirees
    // deorbiting: SatelliteSimLaunches.cpp), then the Kessler pool from poolIdx0(). kesslerBuf's words: the roster's
    // dead / end seconds, the launch records from kLaunchWord0, the pool from kPoolWord0 (sat_orbit.comp LAUNCH_CAP).
    static constexpr uint32_t kLaunchCap = 131072;
    uint32_t poolIdx0() const { return activeSatCount + kLaunchCap; }
    uint32_t visibleCapacity() const { return std::max<uint32_t>(activeSatCount, 1u) + kLaunchCap + kPoolCap + kCitySpriteMax; }
    uint32_t kLaunchWord0 = 0;            // first launch-record word
    uint32_t kesslerBufGen = 0;           // bumped whenever kesslerBuf is (re)created (the launch records are re-sent)
    uint32_t kPoolWord0 = 0;              // first pool word (after the roster's dead / end seconds)
    uint32_t kPoolGpu = 0;                // records uploaded (drawn)
    std::vector<uint32_t> kDirtyRoster;   // roster satellites whose two words to upload
    std::vector<uint32_t> kDirtyPool;     // uploaded records whose death changed
    size_t kDirtyPoolHead = 0;            // (2026-10-21c) the next of kDirtyPool to upload (arrival order)
    size_t kDirtyRosterSorted = 0;        // the sorted prefix of kDirtyRoster
    bool kRosterFullUpload = false;
    bool kRestartPending = false;
    bool kInitDone = false;               // kesslerInit has run (settings loaded before it apply directly)         // a kessler.* setting changed after launch: re-simulate next frame
    double kesslerEpoch() const;          // the scenario epoch (the CME's arrival), J2000 s
    void kesslerInit(VulkanContext &ctx); // after the roster's buffers exist: storm, world, cache, worker
    void kesslerShutdown();               // stop the worker, write the cache
    double kesslerSaveCacheNow(const std::string &copyTo, std::string &where); // harness `kessler savecache`
    nlohmann::json kesslerMemJson(); // harness `kessler mem` (2026-10-28): bytes by structure + the process's figures
    void launchMemReport(std::vector<std::pair<std::string, double>> &out) const; // its part for the launches
    // syncUntil > 0: when the world starts within two steps of it (a breakup before the CME starts the world at its own
    // step), that step is computed before this returns (its debris is up the same frame).
    void kesslerRestart(VulkanContext &ctx, const std::vector<kessler::Event> &userEvents, double keepEngineBefore,
                        double syncUntil = -1.0, bool async = false);
    double kWorldStart = 0.0; // where the running world starts (the CME, or a step before it: a pre-CME breakup)
    // A player breakup without replaying the history (2026-10-19): the world rewinds to its last checkpoint at or
    // before the breakup (World::rollback), re-runs the breakup's step here, and the mirror is rewound to match.
    bool kesslerBreakupInPlace(const kessler::Event &ev);
    void kesslerMirrorRewind(double tCheckpoint);
    double kLastBreakupMs = 0.0; // how long the last player breakup took on the main thread (harness state)
    bool kLastBreakupFast = false;
    bool kForceRebuildOnce = false; // harness `kessler breakup ... rebuild=1`: the old full replay (for comparison)
    void kesslerSyncProcess(int32_t nowSec, double budgetMs); // works through the queue of the worker's output
    std::chrono::steady_clock::time_point kHistPruneWall{}; // the last history prune (wall time; kesslerSync)
    double kHistPruneFrontier = 0.0; // the mirrored frontier when the last prune pass started (2026-10-28)
    size_t kPruneBucket = SIZE_MAX; // the prune pass's next kPoolHist bucket (SIZE_MAX: none running; 2026-10-21)
    double kSyncBudgetMs = 3.0; // its wall-time budget a frame (2026-10-20, the hitches at a fast warp)
    double kSyncBudgetHeldMs = 12.0; // ... while time is held at the frontier (the player is waiting; 2026-10-28)
    void kesslerSync(VulkanContext &ctx);  // per frame: take the worker's new events / records / deaths
    void kesslerWant(double tJ2000);       // ask the (lazy) worker to simulate at least this far
    double kWantFloor = 0.0;               // the furthest explicit kesslerWant (harness waits); the warp's target may drop
    void kesslerRecordUploads(VkCommandBuffer cmd); // per frame: copy them to the GPU
    void kesslerEnsureCapacity(VulkanContext &ctx, size_t records, bool inFrame = false);
    // (2026-10-21) A pool grown during a frame: the old buffers to copy from (kesslerRecordUploads) and, with the visible
    // list's, to destroy once that frame has finished (kesslerRetireTick, called after the next fence wait).
    struct KGrowCopy
    {
        VkBuffer oldK = VK_NULL_HANDLE, oldB = VK_NULL_HANDLE;
        VkDeviceSize bytesK = 0, bytesB = 0;
    } kGrowCopy;
    struct KRetired
    {
        VkBuffer buf;
        VkDeviceMemory mem;
        uint64_t frame;
    };
    std::vector<KRetired> kRetired;
    std::thread kRetireThread; // (2026-10-21c) frees the retired buffers off the main thread
    uint64_t kRetireFrame = 0;
    void kesslerRetireTick(VkDevice device, bool all = false);
    // (2026-10-21) The next growth's buffers, allocated on a worker thread once the pool passes 40% of its capacity:
    // allocating ~0.5 GB of device memory in the frame took ~10 ms.
    struct KPrealloc
    {
        uint32_t cap = 0, poolWord0 = 0;
        double ms = 0.0; // how long the allocation took (logged)
        VkBuffer vis = VK_NULL_HANDLE, idx = VK_NULL_HANDLE, k = VK_NULL_HANDLE, b = VK_NULL_HANDLE;
        VkDeviceMemory visM = VK_NULL_HANDLE, idxM = VK_NULL_HANDLE, kM = VK_NULL_HANDLE, bM = VK_NULL_HANDLE;
    };
    std::future<KPrealloc> kPreFut;
    uint32_t kPreCap = 0; // the capacity the allocation in flight is for
    uint64_t kGrowHolds = 0; // frames whose sync stopped at the GPU pool's capacity waiting for the allocation
    void kesslerPreallocTick(VulkanContext &ctx, bool force = false);
    void kesslerGrowDescriptors(VulkanContext &ctx);
    bool kesslerGrowReady(uint32_t slot);
    void kesslerPreallocDrop(VkDevice device, bool retire);
    void kesslerUploadAllSync(VulkanContext &ctx); // after (re)allocating: roster ends now, records progressively
    bool kesslerUserEvent(uint8_t kind, uint32_t satA, uint32_t satB, std::string *why);
    std::vector<kessler::Event> kUserEvents; // breakups the player (or a script) caused
    uint32_t kesslerRefOfSat(uint32_t sat) const; // satellite index -> world reference (or kNoRef)
    nlohmann::json kesslerStateJson() const; // exact, now (walks everything: the harness's state)
    nlohmann::json kesslerStateJsonImpl(bool full) const;
    // Live counts at one second: dead / gone satellites, live pieces, events by kind.
    struct KCounts
    {
        size_t dead = 0, frag = 0, fragReal = 0, rosterGone = 0, kills = 0, bumps = 0, col = 0, cat = 0, user = 0;
        size_t byClass[3] = {0, 0, 0};
        double smallMade = 0.0;
        // The Kessler tracker's (2026-10-21): re-entered roster satellites, explosions / ASAT strikes, working
        // satellites, the objects by altitude x inclination (50-km shells from 150 km, 5-deg bands folded to
        // 0-90), the collisions per day since the CME and the latest significant breakups (event indices).
        static constexpr int kAlt = 40, kInc = 18;
        size_t reentered = 0, explosions = 0, working = 0;
        float deb[kAlt * kInc] = {};
        float work[kAlt * kInc] = {};
        std::vector<uint32_t> colDay, catDay;
        uint32_t recent[8] = {};
        double recentT[8] = {};
        int nRecent = 0;
    };
    struct KCountsWalk
    {
        KCounts acc;
        int32_t sec = 0;
        double t = 0.0;
        size_t pool = 0, roster = 0, ev = 0;
        bool active = false;
    };
    // Walks up to `budget` pool records, roster entries and events (x4) from the cursors; true when all are done.
    bool kesslerCountsWalk(KCountsWalk &w, size_t budget) const;
    nlohmann::json kesslerCountsJson(const KCounts &c) const;
    // For the settings tab, which reads it every frame: the cheap fields at most 4 times a second, the counts from a
    // walk spread over frames (kesslerCountsTick, ~150k records a frame while the tab reads it). A full walk of 5M
    // pieces, the roster and ~10M events took 50-180 ms on the main thread, four times a second: the late-cascade
    // stutter (issue 20261016_155342).
    const nlohmann::json &kesslerStateCached() const;
    void kesslerCountsTick();
    mutable nlohmann::json kStateCache;
    mutable double kStateCacheWall = -1.0;
    mutable double kCountsWantedWall = -1.0;
    KCountsWalk kCountsWalkState;
    KCounts kCountsLive;
    bool kCountsLiveValid = false;
    uint64_t kCountsLiveGen = 0; // bumped at every published walk (the tracker's tables follow it)
    mutable std::vector<int8_t> kTypeClass; // satellite type -> debris kit class (0 S, 1 M, 2 L) or -1
    std::string kesslerStatusLine() const; // the HUD line while the frontier holds time back
    // A satellite's display name: "<constellation> #<n>"; a dead one "... #<n> (dead)" (its roster number),
    // a fragment "Debris <event>.<k>".
    std::string satLabel(int idx) const;
    // A roster satellite's fate at time t (Kessler's event log, as mirrored): 0 working (or no scenario), 1 dead
    // (tumbling), 2 destroyed (broken up), 3 re-entered; *whenT = the J2000 second it happened (2026-10-20).
    int rosterFateAt(uint32_t idx, double t, double *whenT = nullptr) const;
    static const char *rosterFateName(int fate); // "working" / "dead" / "destroyed" / "re-entered"
    void kesslerDropGoneSelection(); // a selected roster satellite that is gone now (destroyed, re-entered) is dropped
    void buildKesslerBanner(UIRenderer &ui);
    void buildKesslerSettingsSection(const UIInput &inp, UIRenderer &ui);
    void buildKesslerGraphicsSection(const UIInput &inp, UIRenderer &ui); // dust, fireball colours, small re-entries
    void kesslerBreakupFromUi(int sat, uint8_t kind);

    // ── Kessler tracker (2026-10-21, issue 20261021_175638; SatelliteSimKesslerTracker.cpp) ───────────────────────
    // The Missions window's "Kessler" tab (it replaced "Controls", whose settings moved to Settings > Launches &
    // debris): big counts, a small globe seen from the equator with the debris shells drawn round it as a polar plot
    // (radius = altitude, angle = latitude: the limb of an equatorial view IS a meridian), coloured by the chance a
    // year that a 10 m^2 satellite there is hit, markers for the latest breakups (click a row to watch one), and the
    // collisions per day. Everything comes from the counts walk (kesslerCountsWalk, spread over frames while the tab
    // reads kesslerStateCached): nothing walks the event log or the pool per frame. The globe is a CPU raster (256^2,
    // only when something changed: the counts, a drag, the Sun moving ~1 deg) uploaded in recordCompute.
    static constexpr int kTrN = 256;      // the globe image (texels a side)
    static constexpr int kTrRecent = 8;   // breakups listed / marked
    void buildKesslerTrackerTab(const UIInput &inp, UIRenderer &ui, float bodyW);
    bool kesslerTrackerEnsureImage(UIRenderer &ui);
    void kesslerTrackerRenderGlobe();
    void kesslerTrackerRecordUpload(VkCommandBuffer cmd);
    void kesslerTrackerDestroy(VkDevice device);
    void kesslerTrackerTables(); // the density tables from the last complete counts walk (kCountsLive)
    bool kesslerWatchEvent(uint32_t ev, std::string &msg); // just after it (1.5 s), 80 km beside it, a piece tracked
    int64_t kWatchPieceEv = -1;       // a Watch: select + track a record of this breakup once mirrored
    double kWatchPieceUntil = 0.0;
    uint32_t kWatchPieceScan = 0;     // (the pool scanned in pieces, a frame each)
    double kWatchPieceOffKm = -1.0;   // (harness: the piece's distance from the breakup's recorded place)
    void kesslerWatchPieceTick();
    std::string kesslerEventTitle(const kessler::Event &e) const;
    nlohmann::json kesslerTrackerJson(); // harness `missions state` -> tracker
    VkImage trImg = VK_NULL_HANDLE;
    VkDeviceMemory trMem = VK_NULL_HANDLE;
    VkImageView trView = VK_NULL_HANDLE;
    VkSampler trSampler = VK_NULL_HANDLE;
    VkBuffer trStage = VK_NULL_HANDLE;
    VkDeviceMemory trStageMem = VK_NULL_HANDLE;
    void *trStageMapped = nullptr;
    uint32_t trUiId = 0;
    bool trUploadPending = false;
    bool trImageFailed = false;
    UIImage trImgEl;
    std::vector<UITextQuad> trQuads;
    std::vector<UITextRun> trRuns;
    std::deque<std::string> trRunText;
    UITextBlock trMarks;
    double trViewLonDeg = 1e9;       // the globe's central meridian (1e9: the observer's on first show)
    double trRenderedLon = 1e9, trRenderedSunLon = 1e9, trRenderWall = -1.0;
    uint64_t trRenderedGen = ~0ull, trTablesGen = ~0ull;
    bool trDrag = false;
    float trDragX = 0.0f;
    float trGlobePx = 0.0f;          // the globe element's size on screen (last frame)
    int trHoverMarker = -1, trSelEvent = -1;
    // Density tables (alt x lat band): objects per km^3, debris (pieces + dead) and working satellites.
    static constexpr int kTrLat = 36;
    std::vector<float> trDebDen, trWorkDen;
    float trWorstChance = 0.0f, trWorstAltKm = 0.0f, trWorstLatDeg = 0.0f;
    struct TrMarker
    {
        float x = 0.0f, y = 0.0f; // in the globe's image (0..1)
        bool front = true;
        uint32_t ev = 0;
        int rank = 0; // 1 = the latest
    };
    std::vector<TrMarker> trMarkers;
    std::vector<float> trPlotX, trPlotY, trPlotY2;
    UIPlotSeries trSeries[2];
    UIPlot trPlot;
    uint32_t trPlotPeak = 0;
    int trPlotPeakDay = 0, trPlotBin = 1;

public:
    // ── Explode (2026-10-19): the one call the UI makes to break a satellite up ─────────────────────────
    // Any roster satellite or debris piece, at ANY sim time: before the CME too (the Kessler scenario then starts at that
    // moment, and is switched on if it was off). Before the CME working satellites dodge everything, so only what the
    // player broke (and its dead neighbours) collides; the configured avoidance applies from the CME on. It is an event
    // in the log at the current sim time: playing back before it undoes it. Computed in place (the debris is up the
    // same frame) and time keeps running. Not for rockets, raising trains or retiring satellites (launch objects).
    // `reason` says why not, or what happened ("Explosion: <name>").
    enum class BreakupKind
    {
        Explosion,
        Asat
    };
    struct ExplodeResult
    {
        bool ok = false;
        std::string reason;
    };
    ExplodeResult explodeSatellite(int satIndex, BreakupKind kind);
    // The deployment era's roster table (born / gone seconds since the Kessler epoch, 2 per satellite; may be empty).
    const std::vector<int32_t> &rosterLifeTable() const { return rosterLife; }

private:
    char kBannerBuf[160] = {};
    char kUiMsg[200] = {};
    VkDescriptorSetLayout satRebakeDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool satRebakeDescPool = VK_NULL_HANDLE;
    VkDescriptorSet satRebakeDescSet = VK_NULL_HANDLE;
    VkPipelineLayout satRebakePipeLayout = VK_NULL_HANDLE;
    VkPipeline satRebakePipeline = VK_NULL_HANDLE;
    // mirrorNormalsBuf (persistent mirror lock/slew state) and reflectorTargetsBuf (the per-frame
    // CPU-compacted night-side ECI buffer) were removed 2026-08-06 — see CLAUDE.md's
    // TargetedReflector section. sat_orbit.comp now derives everything it needs (live AND
    // window-eval target positions) directly from reflectorTargetsECEFBuf below plus
    // SatOrbitPC::gmstNow, with no per-frame CPU rotation/compaction step and no persisted GPU
    // state — target selection and mirror orientation are both pure functions of sim time.
    //
    // C12 follow-up #33: reflectorTargetsECEF[]/reflectorTargetsRadiusM[] never change after
    // target generation, so this is uploaded ONCE (right after initConstellation() in init(), see
    // that call site) rather than refreshed per frame. xyz = unit ECEF direction, w = ground
    // radius incl. terrain elevation. Read by sat_orbit.comp (TargetedReflector target search).
    // beam_cloud_block.comp, the buffer's other former reader, was retired 2026-08-09 —
    // beam_self_march.comp doesn't need it (reconstructs ECEF from ReflectBeamsBuf's own ENU
    // offsets instead — see that shader's header).
    VkBuffer reflectorTargetsECEFBuf = VK_NULL_HANDLE; // host-visible+coherent, written once
    VkDeviceMemory reflectorTargetsECEFMem = VK_NULL_HANDLE;
    void *reflectorTargetsECEFMapped = nullptr;
    // beamCloudBlockBuf (per-TARGET cloud occlusion result, beam_cloud_block.comp's own output)
    // retired 2026-08-09 — beam_self_march.comp now writes blockAltM/blockOpacity directly into
    // each beam's own ReflectBeamsBuf entry (per BEAM, not per target), no intermediate buffer.
    // Reflect-Orbital ground beams (host-visible+coherent; written by sat_orbit.comp, indexed
    // by target identity + atomicMax, zeroed every frame via vkCmdFillBuffer — same pattern as
    // glowBuf, including CPU readback of the previous frame's contents for a diagnostic). Read by
    // cloud_march.comp (debug pointing rays only, as of C12 follow-up #44) and sat_sky.frag
    // (ground-spot direct lighting). See GpuReflectBeams.
    VkBuffer reflectBeamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory reflectBeamsMem = VK_NULL_HANDLE;
    void *reflectBeamsMapped = nullptr;
    // beamCloudLightBuf — host-visible+coherent, written by the CPU each frame in recordCompute()
    // (a small capped list of individual real-beam light sources, no per-target aggregation — see
    // GpuBeamCloudLights' own comment for the 2026-08-09 design history), read by
    // cloud_march.comp's cloudMarchCS as a real per-sample directional light source.
    VkBuffer beamCloudLightBuf = VK_NULL_HANDLE;
    VkDeviceMemory beamCloudLightMem = VK_NULL_HANDLE;
    void *beamCloudLightMapped = nullptr;
    // Ground-beam compaction (perf follow-up) — host-visible+coherent, written by the CPU each
    // frame from reflectBeamsBuf's readback (same loop lastActiveBeamCount/beamProximityGlow use).
    VkBuffer groundBeamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory groundBeamsMem = VK_NULL_HANDLE;
    void *groundBeamsMapped = nullptr;
    // Diagnostic readback (C12): how many of the 16 sectors currently hold an active beam, and
    // the straight-line distance (meters) from the observer to the nearest one's ground target —
    // one-frame-stale, same idiom as peakMagnitude. -1 = no active beams this/last frame.
    int lastActiveBeamCount = 0;
    // How many of those survived the observer-range cull into the compacted ground-spot list
    // (kMaxGroundBeams cap). Logged in perf snapshots alongside lastActiveBeamCount because the two
    // drive different shaders: lastActiveBeamCount bounds cloud_march.comp's per-pixel pointing-ray
    // loop, this one bounds sat_sky.frag's per-ground-pixel spot loop, and at a site like Anchorage
    // (itself a reflector target, few neighbouring targets) they diverge sharply.
    int lastGroundBeamCount = 0;
    float lastNearestBeamDistM = -1.0f;
    // 2026-08-09 debug instrumentation (BEAM_CLOUD_PLAN.md): raw min/max/avg blockOpacity across
    // every active beam this frame, computed straight from reflectBeamsBuf's readback with no
    // per-target aggregation/argmax in the way — see the Beams settings tab. If Max stays ~0.00
    // regardless of visible cloud cover, beam_self_march.comp's own march isn't finding cloud
    // (a shell/geometry/binding bug upstream of every consumer). If Max is meaningfully >0 but
    // rendering still looks unaffected, the bug is downstream in a consumer instead.
    float dbgBeamOpacityMin = 0.0f;
    float dbgBeamOpacityMax = 0.0f;
    float dbgBeamOpacityAvg = 0.0f;
    int dbgBeamOccludedCount = 0; // count of beams this frame with blockOpacity > 0.1
    int dbgBeamSampleCount = 0;   // same as lastActiveBeamCount, kept alongside for clarity
    // Beam-driven sky-glow "pollution dome" (C12 follow-up #31) — parallel to lightDomeBuf's
    // 16-sector scheme, but populated by ACTIVE Reflect-Orbital beams instead of a static
    // night-lights texture, so satellites/stars/Milky Way dim near a bright beam the same way
    // they already dim near real light pollution. Written by sat_orbit.comp (atomicMax per
    // sector, host-visible+coherent so the CPU can also read back the previous frame's contents —
    // same one-frame-stale idiom as reflectBeamsBuf/glowBuf). Read directly on the GPU by
    // sat_flare.comp and sat_sky.frag's Milky Way section; updateStars() reads the CPU-side
    // beamGlowDomeAz[] copy below. Deliberately a SEPARATE buffer from lightDomeBuf, not merged —
    // two independent phenomena that happen to share a consumption pattern.
    static constexpr int kNumBeamGlowSectors = 16;
    VkBuffer beamGlowDomeBuf = VK_NULL_HANDLE;
    VkDeviceMemory beamGlowDomeMem = VK_NULL_HANDLE;
    void *beamGlowDomeMapped = nullptr;
    float beamGlowDomeAz[kNumBeamGlowSectors]{}; // CPU-side copy of the previous frame's contents

    // ── sat_flare.comp descriptors / pipeline ─────────────────────────────────
    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;

    // ── sat_orbit.comp descriptors / pipeline ─────────────────────────────────
    VkDescriptorSetLayout orbitDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool orbitDescPool = VK_NULL_HANDLE;
    VkDescriptorSet orbitDescSet = VK_NULL_HANDLE;
    VkPipelineLayout orbitPipeLayout = VK_NULL_HANDLE;
    VkPipeline orbitPipeline = VK_NULL_HANDLE;

    // ── Pipelines ─────────────────────────────────────────────────────────────
    VkPipelineLayout compPipeLayout = VK_NULL_HANDLE;
    VkPipeline compPipeline = VK_NULL_HANDLE;
    VkPipelineLayout skyBgPipeLayout = VK_NULL_HANDLE; // sky/ground background
    VkPipeline skyBgPipeline = VK_NULL_HANDLE;
    // Minimal stand-in used when debugDisableMask bit 262144 is set (Potato preset): sat_sky.frag
    // is ~490 ms/frame on a 2015 AMD GPU via MoltenVK and no quality knob touches that. Same
    // pipeline layout / descriptor set / render pass as skyBgPipeline — only the fragment module
    // differs (shaders/sat_sky_minimal.frag). Created and resized alongside skyBgPipeline.
    VkPipeline skyBgMinimalPipeline = VK_NULL_HANDLE;
    // Planetarium-tier stand-in used when debugDisableMask bit 524288 is set: sat_sky.frag compiled
    // a second time with -DSKY_LITE (shaders/sat_sky_lite.frag.spv) — heaviest subsystems #ifdef'd
    // out for GPUs where the full shader collapses occupancy but the minimal shader is too austere.
    // Same layout / descriptor set / render pass; created and resized alongside skyBgPipeline.
    VkPipeline skyBgLitePipeline = VK_NULL_HANDLE;
    VkPipelineLayout drawPipeLayout = VK_NULL_HANDLE;
    VkPipeline drawPipeline = VK_NULL_HANDLE;

    // ── Resolution scaling (session 29) ─────────────────────────────────────────
    // Below 100%, sky_bg renders to a low-res offscreen target then gets blitted (linear-
    // filtered upscale) into the swapchain image before the main render pass opens — see
    // recordPrePass/activeRenderPass. At the default 1.0 this whole path is skipped and behavior
    // is byte-identical to before this feature existed. Reuses skyBgPipeLayout (same push
    // constants/descriptor set) — only the pipeline's viewport/render-pass/depth-state differ.
    // Depth is deliberately NOT blitted (depth-format blit support isn't spec-guaranteed, a real
    // portability concern specifically on the lower-end hardware this feature targets) — a known,
    // accepted tradeoff: satellites/stars are not occluded by terrain while scaled below 100%.
    float renderScale = 1.0f;                          // [0.5, 1.0], Settings > Display "Render scale"
    // The half-res compute passes (scene depth + its quarter-res seed, the cloud march, resolve and
    // composite) follow the render scale too (2026-10-04): at 50% they march a quarter of the pixels.
    // Off = the old behaviour (always half the swap extent). Settings > Display, key
    // display.clouds_follow_render_scale. computeHalfExtent() is the ONE sizing rule for all of them.
    bool computeFollowsRenderScale = false;
    VkExtent2D computeHalfExtent(const VulkanContext &ctx) const;
    VkExtent2D computeHalfExtentBuilt{0, 0}; // what the targets were last created at
    // S9 (2026-10-22, "Full-resolution clouds", clouds_v2.full_res): the CLOUD grid — the v2 march / resolve images
    // and the final cloud targets A/B the sky pass reads. Half the render extent (= computeHalfExtent), or all of it
    // with full-res clouds on: then cloud_march.comp still composites at half res into cmHalf* and
    // cloud_v2_combine.comp writes the full-res targets (SatelliteSimCloudsV2.cpp, "Full-resolution combine").
    bool cv2FullResActive() const { return cv2FullRes > 0.5f && (debugDisableMask & 32768u) == 0u; }
    VkExtent2D computeCloudExtent(const VulkanContext &ctx) const;
    VkExtent2D computeCloudExtentBuilt{0, 0};
    VkImage cmHalfImg[4] = {};                 // cloud_march.comp's half-res outputs in full-res mode: A, B, dB, dA
    VkDeviceMemory cmHalfMem[4] = {};
    VkImageView cmHalfView[4] = {};
    VkDescriptorSetLayout cv2CombineDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool cv2CombineDescPool = VK_NULL_HANDLE;
    VkDescriptorSet cv2CombineDescSet = VK_NULL_HANDLE;
    VkPipelineLayout cv2CombinePipeLayout = VK_NULL_HANDLE;
    VkPipeline cv2CombinePipeline = VK_NULL_HANDLE;
    void createCloudCombine(VulkanContext &ctx);              // pipeline + set (once)
    void createCloudCombineTargets(VulkanContext &ctx);       // the half images (1x1 while off) + every set that points at them
    void destroyCloudCombineTargets(VkDevice device);
    void destroyCloudCombine(VkDevice device);
    void recordCloudCombine(VkCommandBuffer cmd, VulkanContext &ctx, const CloudMarchPC &cpc);
    void recreateComputeScaledTargets(VulkanContext &ctx);
    // Automatic render scale (2026-10-04, Settings > Display "Automatic render scale"): moves renderScale
    // in 5% steps between dynResMinScale and 1 to keep the GPU frame under the target frame rate's budget
    // (x 0.9). Keys display.dynamic_resolution / dynamic_target_fps / dynamic_min_scale.
    bool dynResEnabled = false;
    float dynResTargetFps = 60.0f;
    float dynResMinScale = 0.5f;
    float dynResGpuMs = 0.0f;       // eased GPU frame (ms)
    double dynResNextChangeS = 0.0; // steady-clock seconds: no change before this
    void updateDynamicResolution();
    VkRenderPass skyLowResRenderPass = VK_NULL_HANDLE; // color-only, CLEAR, finalLayout=TRANSFER_SRC_OPTIMAL
    VkImage skyLowResColorImg = VK_NULL_HANDLE;
    VkDeviceMemory skyLowResColorMem = VK_NULL_HANDLE;
    VkImageView skyLowResColorView = VK_NULL_HANDLE;
    VkFramebuffer skyLowResFramebuffer = VK_NULL_HANDLE;
    VkPipeline skyLowResPipeline = VK_NULL_HANDLE; // low-res viewport variant of skyBgPipeline
    VkExtent2D skyLowResExtent{};

    // Moon state (updated each frame in updatePositions)
    glm::vec3 moonDirECI{1, 0, 0};    // unit vector toward moon in ECI (equatorial orbit)
    glm::vec4 moonDirENU{0, 1, 0, 0}; // xyz = moon dir in ENU, w = illuminated fraction

    // ── Stars ─────────────────────────────────────────────────────────────────
    struct StarRecord
    {
        glm::vec3 eciDir;   // unit vector toward star in ECI (J2000)
        float rawIntensity; // magnitude-derived brightness (no night factor)
        glm::vec3 color;    // spectral color from B-V index
        float angSize;      // point sprite size in pixels
    };
    std::vector<StarRecord> starRecords;
    std::vector<uint32_t> starPackedColor_;   // packVisibleColor(starRecords[i].color), packed once (updateStars)
    VkBuffer starBuf = VK_NULL_HANDLE;
    VkDeviceMemory starMem = VK_NULL_HANDLE;
    void *starMapped = nullptr;
    VkDescriptorSetLayout starDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool starDescPool = VK_NULL_HANDLE;
    VkDescriptorSet starDescSet = VK_NULL_HANDLE;
    VkPipelineLayout starPipeLayout = VK_NULL_HANDLE;
    VkPipeline starPipeline = VK_NULL_HANDLE;
    uint32_t starCount = 0;
    // ── The star catalogue's metadata, the faint stars, the selectable stars and the 2037 supernova (2026-10-27,
    // SatelliteSimStars.cpp; CLAUDE.md "Stars"). starRecords / starInfo_ are one list sorted by magnitude (brightest
    // first), from assets/stars/star_catalog.bin (tools/make_star_catalog.py: HYG v4.1 to mag 8.5), else the
    // compiled-in BSC5 set (star_catalog.h, no names). A selected star is selectedStarIndex (its index; the other selections
    // win: buildSelectedSatPanel drops it while a satellite, planet or comet is selected).
    struct StarInfo
    {
        float vmag = 0.0f, bv = 0.0f, distPc = 0.0f; // catalogue V, B-V, distance (0 = unknown)
        uint32_t hip = 0, hr = 0;
        uint32_t name = 0, desig = 0, spect = 0, conName = 0; // offsets into starStrings_ (0 = "")
        char con[4] = {};                                  // IAU abbreviation
        bool variable = false;
    };
    std::vector<StarInfo> starInfo_;
    std::string starStrings_;
    bool starCatalogFromFile = false;
    const char *starStr(uint32_t off) const { return off < starStrings_.size() ? starStrings_.c_str() + off : ""; }
    bool loadStarCatalog();                           // fills starInfo_ / starStrings_ and the RA/Dec it returns via
    std::vector<glm::vec2> starRaDec_;                // ... this (degrees, J2000)
    int starIndexByName(const std::string &name) const; // proper name, designation, "HIP n" / "HR n"; -1 if none
    std::string starDisplayName(int i) const;
    int pickStarAt(float clickX, float clickY, float screenW, float screenH, float *distPx = nullptr) const;
    void formatSelectedStarInfo(int i);
    bool starEnuNow(int i, glm::vec3 &enu, float *flare = nullptr) const; // star i's direction now (ENU) and drawn intensity
    int selectedStarIndex = -1;
    char starInfoLine[kSelInfoLines][kSelInfoLineLen] = {};
    uint32_t starDrawCount = 0;                       // the prefix of starRecords written this frame (sorted by mag)
    uint32_t starVisibleCount = 0;                    // ... of which drawn (intensity > 0), harness `stars`
    float starLimitMagFrame = 0.0f;                   // the faintest catalogue magnitude that could show this frame
    float starNelmZenith = 0.0f;                      // naked-eye limit at the zenith (harness `stars`)
    double starUpdateMs = 0.0;                        // updateStars' wall time (eased)
    double starPrepMs = 0.0;                          // ... of which before the per-star loop (tables, grid)
    uint64_t starElTabBuilds = 0;                     // per-elevation table rebuilds (harness `stars`)
    int starBetelgeuse = -1;                          // its index (HIP 27989), -1 without it
    // The per-elevation table updateStars reads: x the line of sight's extinction (mag), y the refraction (rad,
    // apparent - true; < -1 = below the apparent horizon), z the angle-of-arrival jitter (rad), w the scintillation
    // (-1 = no table), over sin(true elevation) from the limb up, quadratic toward the limb.
    std::unique_ptr<glm::vec4[]> starElTab_;
    double starElTabR_ = -1.0;
    float starElTabK_ = -1.0f;
    uint64_t starElTabRefr_ = ~0ull;
    bool starElTabRefrOn_ = false;
    uint64_t refrSerial_ = 0;                         // bumped when the refraction table is re-derived
    // Faint stars only under dark skies (the naked-eye limit from the sky's brightness in each direction).
    bool starSkyLimit = true;                         // stars.sky_limit
    float starLimitBoost = 1.2f;                      // stars.limit_boost: mag beyond the formula's dark-sky NELM
    float starDarkExtraMag = 1.0f;                    // stars.dark_sky_extra: the point limit this much fainter for stars under a dark sky
    // The scripted Betelgeuse supernova (Type II-P), 2037-12-25. A pure function of sim time.
    bool supernovaEnabled = true;                     // stars.supernova
    bool betelgeuseVariable = true;                   // stars.betelgeuse_variable
    static constexpr double kSupernovaT0 = 1198594800.0; // 2037-12-25 03:00:00 UTC (s since J2000): shock breakout
    static double supernovaMagAt(double t, float *bv = nullptr); // apparent V (extinction by the ISM in it), 99 before
    static double supernovaEndT();                    // when it fades below the star it was (+0.5)
    float betelgeuseMagAt(double t, float *bv) const;   // the star's V and B-V now (the supernova from kSupernovaT0)
    float snMagSeen = 99.0f;                          // this frame: + the air's extinction (99 = not a supernova now)
    glm::vec3 snEnu{0.0f, 0.0f, 1.0f};
    glm::vec3 snColor{1.0f};
    bool snAbove = false;                             // above the apparent horizon
    float snLightK = 0.0f;                            // its light vs the full Moon's (above the air; 0 = not a supernova / set)
    // The NIGHT KEY LIGHT (2026-10-28, issue 20261028_222111): the Moon and the supernova as ONE directional source for
    // everything the Moon lights (terrain, sea glint and foam, clouds' key light, the moonlit sky): direction = their
    // light-weighted mean (exact when one dominates), w = their summed light vs the full Moon. moonDirENU itself when no
    // supernova is up. The cloud passes take it as their push constants' moonDirENU; sat_sky.frag reads it from
    // CloudParams.nightKeyENU (w 0 = use the Moon). SatelliteSimStars.cpp.
    glm::vec4 nightKeyLightENU() const;
    double snFlare = 0.0;
    void supernovaFill();                             // kesslerFillDust's finish: the supernova into kFireBuf
    harness::Status harnessStars(harness::Active &a);

    // ── Planets ──────────────────────────────────────────────────────────────
    // Reuses starPipeline/starPipeLayout/starDescLayout unchanged (same GpuSatVisible-shaped
    // point-sprite pipeline) — only a second small host-mapped buffer + descriptor set, drawn with
    // a second vkCmdDraw call. See "Subsystem: Planets" in CLAUDE.md.
    VkBuffer planetBuf = VK_NULL_HANDLE;
    VkDeviceMemory planetMem = VK_NULL_HANDLE;
    void *planetMapped = nullptr;
    VkDescriptorPool planetDescPool = VK_NULL_HANDLE; // starDescPool is sized maxSets=1, so this
                                                      // gets its own tiny pool for one more set
    VkDescriptorSet planetDescSet = VK_NULL_HANDLE;   // allocated with starDescLayout (same shape)
    // Resolved planet discs (2026-10-27, planet_disc.vert/.frag, SatelliteSimPlanets.cpp): their own small pipeline and
    // set (sat_sky.frag is at the 16 sampled-image floor): 0 the disc UBO, 1-2 the cloud composite, 3 the scene depth,
    // 4 the planet maps (a 2D array: Mars, Jupiter, Saturn, Saturn's ring profile).
    VkBuffer planetDiscBuf = VK_NULL_HANDLE;
    VkDeviceMemory planetDiscMem = VK_NULL_HANDLE;
    void *planetDiscMapped = nullptr;
    VkDescriptorSetLayout planetDiscLayout = VK_NULL_HANDLE;
    VkDescriptorPool planetDiscPool = VK_NULL_HANDLE;
    VkDescriptorSet planetDiscSet = VK_NULL_HANDLE;
    VkPipelineLayout planetDiscPipeLayout = VK_NULL_HANDLE;
    VkPipeline planetDiscPipeline = VK_NULL_HANDLE;
    VkImage planetMapImg = VK_NULL_HANDLE;
    VkDeviceMemory planetMapMem = VK_NULL_HANDLE;
    VkImageView planetMapView = VK_NULL_HANDLE;
    VkSampler planetMapSampler = VK_NULL_HANDLE;
    uint32_t planetMapLayers = 0;
    uint32_t planetDiscCount = 0;   // this frame's resolved discs (0 = the pass draws nothing)

    // The turbulence warp (2026-10-28, SatelliteSimTurbulence.cpp): a copy of the composed frame (sampled), set 0 = the
    // UBO, the copy, the cloud composite A / B, the scene depth.
    VkImage turbFrameImg = VK_NULL_HANDLE;
    VkDeviceMemory turbFrameMem = VK_NULL_HANDLE;
    VkImageView turbFrameView = VK_NULL_HANDLE;
    VkSampler turbFrameSampler = VK_NULL_HANDLE;
    VkExtent2D turbFrameExtent{0, 0};
    VkBuffer turbBuf = VK_NULL_HANDLE;
    VkDeviceMemory turbMem = VK_NULL_HANDLE;
    void *turbMapped = nullptr;
    VkDescriptorSetLayout turbLayout = VK_NULL_HANDLE;
    VkDescriptorPool turbPool = VK_NULL_HANDLE;
    VkDescriptorSet turbSet = VK_NULL_HANDLE;
    VkPipelineLayout turbPipeLayout = VK_NULL_HANDLE;
    VkPipeline turbPipeline = VK_NULL_HANDLE;
    bool turbWarpActive_ = false;   // this frame runs the pass (set in recordDraw, read by wantsPreUiCopy)
    float turbMaxPx_ = 0.0f;        // the largest rms displacement in view this frame (px; harness state)
    float turbAltFade_ = 1.0f;      // 1 under the air, 0 from orbit
    double turbClock_ = 0.0;        // real (frame) seconds the field evolves on
    double twinkleClock_ = 0.0;     // real seconds x starTwinkleRate, mod 1000 (PointDrawPC::waveTime)
    uint64_t turbFrames_ = 0;       // frames the pass ran (harness state)

    // ── Simulation state ──────────────────────────────────────────────────────
    SkyCamera camera;
    // simTime is split into an integer day counter and a double-precision
    // seconds-within-day value.  This avoids accumulated float precision loss
    // when a large J2000 epoch base is added to a small per-frame delta.
    // simSecInDay is re-based to [0, 86400) each frame so it stays small.
    // Use simTimeDouble() wherever a full-precision double is needed.
    int64_t simDayJ2000 = 0;     // integer days since J2000 (2000-01-01 12:00 TT)
    double simSecInDay = 0.0;    // seconds within current day [0, 86400)
    int64_t simInitDayJ2000 = 0; // values at construction — used for display
    double simInitSecInDay = 0.0;
    // Bug fix: this was 1 ("10x"), so a launch that never touched settings.json's "time" key
    // (or replayed the intro, which doesn't re-run loadSettings) came up at 10x instead of the
    // intended 1x default — the inconsistency the user reported ("time base doesn't seem to be
    // consistently set on bootup"). loadSettings() still overrides this with whatever was
    // persisted; this is only the compiled-in fallback.
    int timeScaleIdx = 0;
    bool timePaused = false;
    float timeDir = 1.0f; // +1 = forward, -1 = reverse
    // Observer position/facing in Earth-fixed ECEF — canonical movement state.
    // obsLatDeg / obsLonDeg are display caches derived each frame; camera.azDeg is also derived.
    // Initial: lat=67°S lon=67°W, facing north.
    //   obsDir    = (cos(-67°)cos(-67°), cos(-67°)sin(-67°), sin(-67°))
    //             ≈ (0.1527, -0.3596, -0.9205)
    //   obsFacing = (-sin(-67°)cos(-67°), -sin(-67°)sin(-67°), cos(-67°))
    //             ≈ (0.3596, -0.8473, 0.3907)
    glm::vec3 obsDir = {0.1527f, -0.3596f, -0.9205f}; // unit position vector
    glm::vec3 obsFacing = {1, 0, 0};                  // unit tangent (forward direction, north)
    float obsLatDeg = -67.0f;                         // display cache — derived from obsDir
    float obsLonDeg = -67.0f;                         // display cache — derived from obsDir
    bool cloudMarchTargetsClear = false;              // cloudMarchTargetA/B hold "no cloud" (Potato skips cloud_march.comp)
    bool satFlareIdleCleared = false;                 // glowBuf/oceanGlintBuf emptied while no satellite is active
    bool sceneDepthHoldsNoSurface = true;             // sceneDepthImg/QImg hold kNoSurfaceT (created so; knockout 1024)
    float obsTerrainH = 0.0f;                         // terrain elevation at observer lat/lon (m)
    float obsHeightOffset = 0.0f;                     // user-controlled height above terrain (m, Q/E/Z)
    // The eye's distance from the Earth's centre, as the shaders place it: max(ground, height offset)
    // + 2 m (observerEffHeight). The satellites' ENU frame (obsECI) and every CPU evaluator use this;
    // until review 7 they used R + ground + offset, a different eye by the ground height, so over
    // mountains the beams and satellites sat ~km off the drawn scene and jumped as the ground changed.
    // The ground is the GPU's detailed ground under the eye (terrainFrameBuf .y, last frame's) when there is one, the 18-km
    // CPU DEM (obsTerrainH) only before the first readback: 2026-10-22 (issue 20261022_174155), at 62.5 N 139.9 E the DEM
    // read 2324 m over real ground at 1872 m, so a climb from 1872 to 2250 m left this eye pinned at 2324 m (the sky TAA saw
    // no motion and ghosted every edge; satellites, beams and lights sat 450 m off the drawn scene).
    float obsGroundM() const
    {
        // (Following: the GPU's ground is under the camera, not the parked observer this may be asked about.)
        return (!followActive && terrainFrameMapped && std::isfinite(terrainFrameMapped[1]) && terrainFrameMapped[1] > -500.0f)
                   ? terrainFrameMapped[1] : obsTerrainH;
    }
    float obsEyeRadiusM(float heightOffset) const { return (float)satphot::kEarthRadiusM + std::max(obsGroundM(), heightOffset) + 2.0f; }
    float obsEyeRadiusM() const { return obsEyeRadiusM(obsHeightOffset); }
    // The observer every readout describes (magnitude, elevation, extinction, the Info window, the 3D view's "you"
    // marker, picks "high in your sky"): Go to MOVES the observer, so while following it is the camera
    // (followObsEcef); only the last ground position is remembered (followSavedObsDir / Height: Exit / Return home,
    // settings.json). 2026-10-28, issue 20261028_223234: the readouts used that parked ground position (a "ghost"
    // ground observer), so a satellite beside the camera read "below your horizon".
    glm::dvec3 readoutObsEcef() const;
    glm::dvec3 readoutObsEci(double tJ2000) const;
    // 2026-08-09 (in-app finding: beam/cloud-glow ground spots visibly "drag" behind the observer
    // while moving): satENU/reflectDirENU/targetENU read back from reflectBeamsBuf each frame are
    // expressed in the East/North/Up basis at whatever obsDir PRODUCED them (beam_self_march.comp's
    // own push-constant fill, one frame ago) — that basis ROTATES as obsLatDeg/obsLonDeg change via
    // WASD movement, so reinterpreting last frame's ENU numbers against this frame's obsPos without
    // re-projecting them is a real rotation error, not just ordinary 1-frame staleness. These cache
    // exactly the obsDir/obsEffH that produced the data currently sitting in reflectBeamsMapped, so
    // recordCompute's readback loop can un-rotate it into the current frame's basis before use.
    // Defaulted to match obsDir/0 so the very first frame's correction is a no-op.
    // Frames in flight (2026-10-25): one per frame slot, the values of the frame whose beams the slot's
    // readback holds (written when a frame is recorded, read at the top of the slot's next frame).
    glm::vec3 lastBeamObsDirS[VulkanContext::kMaxFramesInFlight] = {{0.1527f, -0.3596f, -0.9205f},
                                                                   {0.1527f, -0.3596f, -0.9205f}};
    float lastBeamObsEffHS[VulkanContext::kMaxFramesInFlight] = {};
    double lastBeamObsRadiusS[VulkanContext::kMaxFramesInFlight] = {}; // the obsECI radius (0 = none yet)
    // Persistent cloud-light pools (2026-08-12) — see TrackedBeamLight's own comment for the whole
    // design. Two pools with independently reserved eviction budgets, exactly as the previous
    // per-frame build had, so a busy target's summed intensity can never starve lone transiting
    // beams out of the list. NOTE these are the one piece of cross-frame state in this subsystem;
    // everything else in the readback loop is rebuilt from scratch each frame. rebase() must NOT be
    // applied to them — they are stored in Earth-fixed ECEF precisely so that they need no such
    // correction (the 2026-08-11 revert is the cautionary tale).
    TrackedBeamLight trackedClusters[kMaxClusterCloudLights]{};
    TrackedBeamLight trackedIndividuals[kMaxIndividualCloudLights]{};
    // key -> slot+1 (0 = empty), rebuilt from live slots at the top of every build.
    uint32_t trackedClusterHash[kTrackedLightHashSize]{};
    uint32_t trackedIndividualHash[kTrackedLightHashSize]{};
    // Live-slot counts, for the Beams settings tab's diagnostics readout.
    int lastClusterLightCount = 0;
    int lastIndividualLightCount = 0;
    uint32_t activeSatCount = 0;
    uint32_t visibleCount = 0;   // compact-list length (above-horizon, enabled), one frame stale
    uint32_t gpuSatCount = 0;    // in-frustum sats written to GPU buffer
    float loopMs = 0.0f;         // satellite loop time last frame (milliseconds)
    float peakMagnitude = 99.0f; // brightest steady-state sat magnitude this frame

    // ── GPU frame timing (perf HUD, Display settings tab) ──────────────────────
    // EMA-smoothed breakdown of ctx.timestampMs, updated once per frame in
    // updateGpuTimingStats(). One-frame-stale by construction (same pattern as
    // peakMagnitude above): App resolves the query pool right after the fence wait,
    // before buildUI/recordCompute run, so these hold the previous completed frame's
    // GPU time when buildUI reads them, and get refreshed for the following frame's
    // display at the top of recordCompute().
    float gpuMsSmoothed[8] = {};     // scene depth, beam cloud block, orbit compute, cloud march,
                                     // flare compute, sky background draw, satellite+star draw,
                                     // UI overlay — order fixed by VulkanContext's slot table;
                                     // kPerfLabels[] and savePerfSnapshot() mirror it
    float gpuMsTotalSmoothed = 0.0f; // whole-frame GPU time
    // UNSMOOTHED copies of the same eight deltas, written by the same updateGpuTimingStats() call.
    // The EMA above exists so the HUD numbers don't flicker; the automated knockout sweep wants
    // the opposite (a clean per-frame sample it can average over its OWN fixed window), and reading
    // an EMA there would have meant either waiting out its ~40-frame settle at every one of the
    // sweep's kDebugToggleCount+1 steps or silently letting the previous step bleed into the next.
    float gpuMsRaw[8] = {};
    float gpuMsRawTotal = 0.0f;

    // ── CPU frame timing (2026-08-10) ─────────────────────────────────────────
    // The GPU side of the Anchorage worst-case work brought Medium to 15.5 ms GPU, at which point
    // the Release wall-clock frame was 18.3 ms — a ~2.8 ms non-GPU remainder that was NEARLY
    // IDENTICAL at Planetarium (2.73 ms against a 6.0 ms GPU frame, i.e. 31% of that frame). Fixed
    // per-frame cost that doesn't scale with rendering load is exactly the shape the GPU timestamp
    // buckets were built to expose, and nothing equivalent existed on the CPU side — so the
    // remainder was a single opaque number and any attempt to shrink it would have been guesswork.
    // This is the same instrument, same conventions: raw per-frame values, an EMA for the HUD, and
    // the sweep/snapshot logging the raw ones.
    //
    // ONE-FRAME-STALE, and deliberately so: timers accumulate into cpuAccumMs[] as the frame runs,
    // and beginCpuFrameTiming() (first thing in buildUI, the first sim call of a frame) publishes
    // the completed previous frame into cpuMsRaw[]/cpuMsSmoothed[] and clears the accumulator. That
    // matches gpuMsRaw[]'s own staleness, so a sweep step samples a CPU frame and a GPU frame from
    // the same moment rather than one lagging the other.
    enum CpuBucket
    {
        CPU_BUILD_UI = 0,     // Clay layout for the whole HUD/settings window
        CPU_UPDATE_POSITIONS, // sun/moon/planets/observer basis (O(1), not per-satellite)
        CPU_BEAM_READBACK,    // reflectBeamsBuf readback + sort + clustering + ground-beam top-K
        CPU_UPDATE_STARS,     // per-star ENU + suppression chain over the catalogue
        CPU_LIGHT_DOME,       // updateLightPollutionDome's 16 sectors x 4 radii
        CPU_UPDATE_PLANETS,   // 6 planets, render-ready entries
        CPU_ORBIT_REBAKE,     // uploadSatOrbits(): the 7-sim-day orbit rebake (every ~2 frames at 1 yr/s)
        CPU_KESSLER,          // the cascade's main-thread side: taking the worker's batches, the uploads
        CPU_LAUNCHES,         // launches: the schedule, the trains' and retirees' records, rockets and plumes
        CPU_COUNT,
    };
    float cpuAccumMs[CPU_COUNT] = {};    // accumulating, current (incomplete) frame
    float cpuMsRaw[CPU_COUNT] = {};      // last COMPLETE frame, unsmoothed — sweep reads this
    float cpuMsSmoothed[CPU_COUNT] = {}; // EMA, for the HUD
    void beginCpuFrameTiming();

    // Scoped accumulator. Deliberately adds rather than assigns, so a bucket whose work happens in
    // several places (or inside a loop) can be timed with several instances in the same frame.
    struct CpuTimer
    {
        float *acc;
        std::chrono::steady_clock::time_point t0;
        explicit CpuTimer(float &a) : acc(&a), t0(std::chrono::steady_clock::now()) {}
        ~CpuTimer()
        {
            *acc += std::chrono::duration<float, std::milli>(
                        std::chrono::steady_clock::now() - t0)
                        .count();
        }
        CpuTimer(const CpuTimer &) = delete;
        CpuTimer &operator=(const CpuTimer &) = delete;
    };

    // ── Perf knockout toggles (profiling-only; not persisted) ──────────────────
    // Bitmask sent to sat_sky.frag as SatDrawPC::debugDisableMask, so the individual cost of
    // the terrain march / atmosphere loop / sun optical-depth sub-march / ocean sky-reflection
    // loop / airglow-red supplemental march / aurora curtain march / cloud self-shadow light cone
    // / Reflect-Orbital beams / cloud shadow map can be measured in isolation via gpuMsSmoothed
    // deltas, without needing a GPU capture tool. See the dbgSkip* helpers near the top of
    // sat_sky.frag for the bit assignments (1,2,4,8,16,32); bit 64 (cloud self-shadow cone) and
    // bit 128 (Reflect-Orbital beam volumetric term) are checked directly in cloud_march.comp;
    // bit 128 (beam ground-spot term) and bit 256 (cloud shadow map) are also checked in
    // sat_sky.frag (128 gates both consumers of the same feature, checked in both shaders).
    // Bit 512 gates the beam_self_march.comp DISPATCH itself (2026-08-09 — repurposed from
    // beam_cloud_block.comp's own identical producer-side skip bit, now retired along with that
    // pass), in recordCompute() — no shader reads it. Bits 256 and 512 are the two producer-side
    // knockouts; every other bit disables a consumer block inside a shader.
    //
    // 2026-08-10 (Anchorage worst-case profiling session) added four bits for blocks that had no
    // knockout at all and were therefore permanently invisible inside a lumped timestamp bucket:
    // 8192 = the per-pixel Reflect-Orbital beam POINTING-RAY loop in cloud_march.comp (up to 2048
    // iterations per half-res pixel; bit 128 never reached it — CLAUDE.md flagged that gap and this
    // closes it), 16384 = cirrusMarchCS, 32768 = cloudMarchCS (the volumetric low/mid march itself),
    // 65536 = sat_sky.frag's unconditional 64-bin satellite sky-glow loop. 8192 and 65536 are also
    // the two blocks no graphics preset could reach, so they were paid in full even at Planetarium.
    //
    // Bit 1048576 (2026-09-23) is the Phase 3b occlusion between satellite parts in sat_orbit.comp;
    // it reaches the shader through GpuSatTypeHeader::occlusionOn, not through any UBO.
    //
    // The authoritative bit/label/json-key table is kDebugToggles at the top of SatelliteSimUI.cpp.
    uint32_t debugDisableMask = 0;
    static constexpr uint32_t kDebugBitSatOcclusion = 1048576u;
    static constexpr uint32_t kDebugBitMeshes = 2097152u; // Phase 4c: no satellite meshes in the scene
    // No meshes, env probes or sharp reflections: the knockout, Potato (262144) and Planetarium / SKY_LITE (524288,
    // whose sky shader has the mesh composite compiled out).
    static constexpr uint32_t kNoMeshBits = kDebugBitMeshes | 262144u | 524288u;
    // Occlusion between satellite parts is OFF by default (Photometry tab "Satellite part
    // occlusion", persisted as photometry.sat_part_occlusion): it costs ~10x the orbit dispatch at
    // 10M satellites for a subtle effect, so no preset or first run turns it on. The knockout bit
    // above can still force it off while this is on (profiling A/B).
    bool satPartOcclusion = false;
    float orbitDeltaTLo = 0.0f; // low half of this frame's orbit deltaT (GpuSatTypeHeader::deltaTLo)
    bool satOcclusionActive() const
    {
        return satPartOcclusion && (debugDisableMask & kDebugBitSatOcclusion) == 0;
    }
    // Occlusion is skipped for satellites fainter than this, unoccluded (see occlusionFluxFloor).
    static constexpr double kOcclusionMagFloor = 10.0;
    // That magnitude in raw effectFlare units (0.008 = mag 6, brightnessScale applied).
    static double occlusionFluxFloor(float brightnessScale)
    {
        return 0.008 * std::pow(10.0, -0.4 * (kOcclusionMagFloor - 6.0)) * brightnessScale;
    }

    // ── Automated knockout sweep (profiling-only) ──────────────────────────────
    // "Run knockout sweep" (Display tab) walks the kDebugToggles table on its own — baseline first,
    // then one step per bit — holding each mask for a fixed frame window and averaging gpuMsRaw[]
    // over it, then appends ONE profile_log.jsonl record carrying every step's bucket breakdown
    // plus its delta against the baseline.
    //
    // Why this exists: doing it by hand is ~18 checkbox toggles x 8 numbers read off a HUD, which is
    // both slow and unreliable — the scene has to hold still for the whole session or the steps
    // aren't comparable, and the HUD values are EMA-smoothed, so a hand capture taken too soon
    // after a toggle silently reads a blend of two configurations. The sweep pauses sim time for
    // its duration (restored afterwards) so every step measures the same frame.
    //
    // THE BASELINE IS THE CURRENT MASK, NOT ZERO — this was a real defect in the first version
    // (2026-08-10) and is the single thing most likely to be "fixed" back into a bug. Zeroing the
    // mask for the baseline means that on any preset which knocks effects out (Planetarium disables
    // eight of them; Low three), the sweep silently RE-ENABLES all of them and then measures the
    // cost of removing each one again. That is a valid cost table for "this preset's slider values
    // with every effect on", but it is NOT a profile of the preset as shipped, and it inflates the
    // baseline — the Planetarium sweep read 18.73 ms against a real 10.4 ms. Anchoring to the live
    // mask instead makes every step answer the question actually being asked: "of what this preset
    // still renders, what does each piece cost me?" Bits already set in the baseline have nothing
    // left to remove, so they are skipped outright and reported as already_disabled rather than
    // logged as a meaningless ~0 ms row.
    //
    // kDebugToggleSlots sizes hovDebugToggle[] and the accumulators below; the static_assert in
    // startKnockoutSweep() keeps it honest.
    static constexpr int kDebugToggleSlots = 24;
    static constexpr int kSweepSettleFrames = 6;  // discard after a mask change — covers the
                                                  // one-frame-stale timestamp readback plus a
                                                  // little driver/clock hysteresis
    static constexpr int kSweepSampleFrames = 24; // then average this many consecutive frames
    // A sweep now measures the baseline TWICE — step 0 at the start and one extra step at the end,
    // both with the baseline mask — and logs the pair plus their drift. Two reasons, both seen in
    // real captures: (1) the 2026-08-10 Release Medium sweep showed a systematic ~+0.32 ms
    // orbit_compute delta on ten unrelated knockout rows, i.e. the FIRST window alone was elevated,
    // which silently inflates every cost_ms by the same amount; (2) "the scene drifted between
    // captures" has been the standing caveat on every cross-sweep comparison this session, and a
    // start/end pair measures that drift instead of leaving it to be argued about. Large drift
    // means the whole sweep is suspect and should be retaken.
    bool sweepActive = false;
    int sweepStep = 0;           // 0 = baseline, 1..sweepBitCount = sweepBits[step-1],
                                 // sweepBitCount+1 = baseline re-measure
    int sweepFrame = 0;          // frames elapsed within the current step
    uint32_t sweepSavedMask = 0; // the baseline mask — restored when the sweep finishes
    // Indices into kDebugToggles for the rows this sweep will actually measure: everything NOT
    // already disabled by sweepSavedMask. Sized for the whole table (the mask==0 case).
    int sweepBits[kDebugToggleSlots] = {};
    int sweepBitCount = 0;
    bool sweepSavedPaused = false;
    // Sized +2, not +1: baseline, every measured bit, and the trailing baseline re-measure.
    float sweepAccum[kDebugToggleSlots + 2][8] = {}; // [step][bucket] running sum, ms
    float sweepAccumTotal[kDebugToggleSlots + 2] = {};
    // CPU wall-clock frame time, averaged over the SAME windows as the GPU buckets. Originally the
    // record carried only `cpuDt` — a single instantaneous frame sampled on whichever frame the
    // sweep happened to finish, right after a mask change, while every GPU number beside it was a
    // 24-frame average. Comparing the two produced a GPU-vs-CPU gap that jumped around by several
    // ms between otherwise-identical captures. Now that the GPU side of the Anchorage work is done
    // and CPU frame time is the binding constraint, that scalar has to be measured as carefully as
    // the GPU ones are.
    float sweepAccumCpu[kDebugToggleSlots + 2] = {};
    // Per-bucket CPU breakdown, same windows again. A knockout bit should barely move these (they
    // are almost all GPU-independent work), so a row that DOES move one is telling you the bit has
    // a CPU-side consumer you didn't know about — which is the other half of why this is logged.
    float sweepAccumCpuBucket[kDebugToggleSlots + 2][CPU_COUNT] = {};
    bool hovRunSweep = false;
    float sweepDoneMsgTimer = 0.0f; // seconds remaining to show the "Sweep saved" confirmation
    // NAME IS STALE, BEHAVIOR IS NOT A BUG — read this before "fixing" the default again. This
    // started as a literal debug-only visualization (a green line) back when the volumetric tube
    // it now replaces was live. When that tube was thrown out for graphics/perf reasons, this ray
    // was reworked in a later session (realistic color, altitude attenuation) into THE actual beam
    // visual — it is the genuine, production Reflect-Orbital beam rendering now, not a diagnostic
    // overlay, and correctly defaults to true. (A same-session pass mistakenly "fixed" this to
    // false, reasoning from the still-stale shader-side header comment in cloud_march.comp that
    // calls it "Opt-in and off by default" — reverted; see BEAM_CLOUD_PLAN.md 2026-08-09.) It
    // draws each active beam's ACTUAL current mirror-pointing direction as a long ray, occluded by
    // terrain ONLY — genuinely no cloud awareness at all, which is the real, still-open bug (this
    // ray is what a player actually sees, so its lack of cloud occlusion is why beams read as
    // passing straight through cloud regardless of the ground-spot/glow fixes elsewhere). Being
    // replaced by the per-beam march project — see BEAM_CLOUD_PLAN.md. Not persisted to
    // settings.json (unlike debugDisableMask, which is).
    // See GpuReflectBeam::reflectDirENU and cloud_march.comp (C12 follow-up #12).
    bool showBeamDebugRays = true;

    // ── Sky glow SSBO ─────────────────────────────────────────────────────────
    // Written by sat_flare.comp each frame via binned atomicMax; read by sat_sky.frag.
    VkBuffer glowBuf = VK_NULL_HANDLE;
    VkDeviceMemory glowMem = VK_NULL_HANDLE;
    void *glowMapped = nullptr;
    // Frames in flight: the readback ids (VulkanContext::readbackCreate) behind glowMapped / reflectBeamsMapped /
    // beamGlowDomeMapped; recordFrameReadbacks records their copies.
    int rbGlow = -1, rbReflectBeams = -1, rbBeamGlowDome = -1;

    // ── Light pollution dome SSBO ─────────────────────────────────────────────
    // Host-visible, updated each frame by updateLightPollutionDome() (CPU); read by
    // sat_flare.comp (satellites) and directly by updateStars() (stars, no upload needed —
    // same array, CPU already has it). 16 azimuth sectors (bumped from 8 — session 26 follow-up:
    // 8 hard-edged 45° wedges showed visible blocky transitions over wide, fairly uniform bright
    // regions like Europe); both consumers additionally interpolate between sector centers rather
    // than hard-binning, so this no longer needs to match GlowBuf's own (unrelated) 8-sector
    // azBin scheme — decoupled on purpose.
    static constexpr int kNumLightSectors = 16;
    VkBuffer lightDomeBuf = VK_NULL_HANDLE;
    VkDeviceMemory lightDomeMem = VK_NULL_HANDLE;
    void *lightDomeMapped = nullptr;
    float lightDomeAz[kNumLightSectors]{}; // CPU-side copy, shared with updateStars()
    // Second half of lightDomeBuf (which is therefore 2*kNumLightSectors floats, not
    // kNumLightSectors): the DARK-SKY dome. Same 16 sectors, but each entry is a normalized [0,1]
    // "how far from pristine toward inner-city" figure — the RAW, pre-lightPollutionGain city
    // brightness log-mapped against mwPollutionThresholdLo/Hi and then temporally eased with
    // mwFadeInTimeS/mwFadeOutTimeS. Consumed by sat_sky.frag (Milky Way, zodiacal) and
    // cloud_march.comp (aurora) through darkSkySkyMag() in shaders/include/darksky.glsl.
    //
    // A separate half rather than a second buffer: it is the same conceptual dome sampled by the
    // same sector geometry, every consumer that wants one already binds this buffer, and
    // sat_flare.comp needs no change at all because it declares only the first 16 floats (trailing
    // storage-buffer data is simply not read). Grown to 32 rather than reusing lightDomeAz because
    // the two halves must NOT be the same numbers: satellites/stars want the gain-scaled linear
    // value, dark-sky features want the ungained log-mapped one — that decoupling is deliberate
    // and predates this (see mwPollutionThresholdLo/Hi).
    //
    // Replaces mwPollutionRaw + mwSuppressEased, which together were a MAX over all 16 sectors
    // eased into one non-directional scalar — i.e. a city on the north horizon suppressed the dark
    // southern zenith identically. Easing moved here, onto the dome ITSELF (the input), so the
    // hysteresis still does its job without flattening the direction information out of it.
    float lightDomeEasedAz[kNumLightSectors]{};
    // The skyglow model (2026-10-28, CLAUDE.md "Light Pollution Dome"): every lit texel of the night map within 300 km
    // (assets/textures/skyglow_sources.bin, tools/make_skyglow_sources.py) summed with a Walker's-law kernel into the
    // ARTIFICIAL / natural sky brightness ratio: skyglowZ at the zenith, skyglowD[s] each sector's directional excess
    // (shaders/include/darksky.glsl darkSkyArtRatio: ratio = Z iso(el) + D(az) dome(el)). *Eased are the eased values
    // (mwFadeIn/OutTimeS); the GPU gets them x skyglowGain (lightDomeEasedAz[] = D, buffer float 32 = Z).
    struct SkyglowSrc { uint16_t loc; uint16_t groundM; float flux; };
    std::vector<SkyglowSrc> skyglowSrc;
    std::vector<uint32_t> skyglowTileOff;
    int skyglowW = 0, skyglowH = 0, skyglowTile = 64;
    float skyglowZ = 0.0f, skyglowD[kNumLightSectors]{};          // this place's target
    float skyglowZEased = 0.0f, skyglowDEased[kNumLightSectors]{};
    double skyglowLastLat = 1e9, skyglowLastLon = 1e9;
    float skyglowLastAlt = -1e9f;
    float skyglowGain = 1.0f;          // "Skyglow (x calibrated)", photometry.skyglow_gain
    double skyglowMs = 0.0;            // the last recompute's cost
    int skyglowSourcesUsed = 0;
    void loadSkyglowSources();
    void computeSkyglow(double latDeg, double lonDeg, float altM, float &Z, float *D, int *nUsed) const;
    // The sky in a direction (ENU), ratio artificial / natural, from the eased values x gain (the CPU mirror of
    // darkSkyArtRatio).
    float skyglowRatioAt(float bearingRad, float z) const;
    VkDescriptorSetLayout skyDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool skyDescPool = VK_NULL_HANDLE;
    VkDescriptorSet skyDescSet = VK_NULL_HANDLE;
    // Noise texture (binding 1): RGBA PNG tiled for lens-flare angular corona variation.
    VkImage noiseTex = VK_NULL_HANDLE;
    VkDeviceMemory noiseTexMem = VK_NULL_HANDLE;
    VkImageView noiseTexView = VK_NULL_HANDLE;
    VkSampler noiseSampler = VK_NULL_HANDLE;
    // Moon texture (binding 2): near-side face disc image for surface detail.
    VkImage moonTex = VK_NULL_HANDLE;
    VkDeviceMemory moonTexMem = VK_NULL_HANDLE;
    VkImageView moonTexView = VK_NULL_HANDLE;
    // The Moon's albedo + LOLA elevation map (binding 2 since 2026-10-27; tools/make_moon_maps.py). moonTex above is the
    // old face-on photo, kept for Potato's sat_sky_minimal.frag (updateMoonDescriptor swaps binding 2) and as the map's
    // fallback.
    VkImage moonMap = VK_NULL_HANDLE;
    VkDeviceMemory moonMapMem = VK_NULL_HANDLE;
    VkImageView moonMapView = VK_NULL_HANDLE;
    VkSampler moonMapSampler = VK_NULL_HANDLE;
    uint32_t moonMapW = 0, moonMapH = 0;
    bool moonDescLegacy = false;
    void createMoonMap(VulkanContext &ctx, const std::vector<unsigned char> &legacy, int lw, int lh);
    void updateMoonDescriptor(VulkanContext &ctx, bool legacy);
    VkSampler moonSampler = VK_NULL_HANDLE;
    // Earth day texture (binding 3): 8K equirectangular colour map.
    VkImage earthDayImg = VK_NULL_HANDLE;
    VkDeviceMemory earthDayMem = VK_NULL_HANDLE;
    VkImageView earthDayView = VK_NULL_HANDLE;
    VkSampler earthDaySampler = VK_NULL_HANDLE;
    uint32_t earthDayMips = 1;
    // Earth night texture (binding 4): 8K equirectangular night-lights map.
    VkImage earthNightImg = VK_NULL_HANDLE;
    VkDeviceMemory earthNightMem = VK_NULL_HANDLE;
    VkImageView earthNightView = VK_NULL_HANDLE;
    VkSampler earthNightSampler = VK_NULL_HANDLE;
    uint32_t earthNightMips = 1;
    // City-detail world-fixed offset (metres): the observer's own cumulative north/east
    // displacement, accumulated every frame from consecutive obsLatDeg/obsLonDeg deltas. Added
    // to hitPt.xy in sat_sky.frag's "City detail texture blend" to cancel that coordinate's
    // observer-relative drift with a plain translation — see the comment there for why a
    // translation is sufficient (no basis/anchor-snap machinery needed). Packed into CloudParams
    // pad1/pad2. Double precision on CPU is cheap insurance against long play sessions; only
    // cast to float when uploading.
    double cityOffsetEastM = 0.0;
    double cityOffsetNorthM = 0.0;
    bool cityOffsetInit = false;
    glm::dvec3 cityPrevObsDir = glm::dvec3(0.0, 0.0, 1.0);
    // ── Sky TAA (2026-09-30): the background's temporal anti-aliasing (sky_taa.comp; SatelliteSim.cpp
    // "Sky TAA"). Display tab "Temporal AA (background)", display.sky_taa.
    struct SkyTaaPC
    {
        glm::vec4 c2e[3];   // camera -> ENU rows (this frame)
        glm::vec4 e2p[3];   // this frame's ENU -> last frame's camera, rows
        glm::vec4 eyeDelta; // xyz this eye - last eye (this ENU, m), w history valid
        glm::vec4 cam;      // tan(fovY/2), aspect, new-sample weight still / moving
    };
    static_assert(sizeof(SkyTaaPC) == 128, "SkyTaaPC must be exactly 128 bytes");
    bool skyTaaEnabled = true;
    float skyTaaWeightStill = 0.1f;
    float skyTaaWeightMoving = 0.35f;
    // Issue 20261022_174155 (2026-10-22): sky-depth pixels (translucent cloud, haze) and far surfaces take the eye's
    // parallax at this stand-in distance for their history weight / clip while the EYE moves (sky_taa.comp, e2p[1].w).
    // 0 = off (the old behaviour). display.sky_taa_parallax_floor_m.
    float skyTaaParallaxFloorM = 0.0f;   // 0 = off (2026-10-22 run B: the 1500-m floor doubled the climb error vs TAA-off; the eye-height fix is what fixed 174155)
    // Diagnostic (sky_taa.comp's debug modes 1-4; 0 = off). display.sky_taa_debug. Not for users.
    int skyTaaDebug = 0;
    // The sky TAA's eye height from the GPU's detailed ground (issue 20261022_174155; false = the CPU DEM's, the old
    // behaviour, for A/B). display.sky_taa_gpu_eye.
    bool skyTaaEyeFromGpuGround = true;
    bool skyTaaUsedThisFrame = false;
    bool skyTaaHistValid = false;
    int skyTaaHist = 0;
    uint32_t skyTaaFrame = 0;
    glm::mat4 skyTaaPrevView{1.0f};
    glm::dvec3 skyTaaPrevObsDir{0.0, 0.0, 1.0};
    double skyTaaPrevEyeR = 0.0;
    // The sim time the last resolve ran at, and this frame's step (recordCompute): the still flags follow what sim
    // time DID, not the pause flag (a cinematic or a harness path moves it while paused).
    double skyTaaPrevSimT = -1.0;
    float skyTaaFrameDt = 0.0f;
    float skyTaaPrevTanHF = 0.0f, skyTaaPrevAspect = 0.0f;
    VkImage skyTaaColorImg = VK_NULL_HANDLE, skyTaaDepthImg = VK_NULL_HANDLE;
    VkDeviceMemory skyTaaColorMem = VK_NULL_HANDLE, skyTaaDepthMem = VK_NULL_HANDLE;
    VkImageView skyTaaColorView = VK_NULL_HANDLE, skyTaaDepthView = VK_NULL_HANDLE;
    VkImage skyTaaHistColorImg[2] = {}, skyTaaHistDepthImg[2] = {};
    VkDeviceMemory skyTaaHistColorMem[2] = {}, skyTaaHistDepthMem[2] = {};
    VkImageView skyTaaHistColorView[2] = {}, skyTaaHistDepthView[2] = {};
    VkRenderPass skyTaaRenderPass = VK_NULL_HANDLE;
    VkFramebuffer skyTaaFramebuffer = VK_NULL_HANDLE;
    VkPipeline skyTaaPipeline = VK_NULL_HANDLE;
    VkSampler skyTaaLinearSampler = VK_NULL_HANDLE, skyTaaNearestSampler = VK_NULL_HANDLE;
    VkDescriptorPool skyTaaPool = VK_NULL_HANDLE;
    VkDescriptorSetLayout skyTaaResolveLayout = VK_NULL_HANDLE, skyTaaRestoreLayout = VK_NULL_HANDLE;
    VkPipelineLayout skyTaaResolvePipeLayout = VK_NULL_HANDLE, skyTaaRestorePipeLayout = VK_NULL_HANDLE;
    VkPipeline skyTaaResolvePipeline = VK_NULL_HANDLE, skyTaaRestorePipeline = VK_NULL_HANDLE;
    VkDescriptorSet skyTaaResolveSet[2] = {}, skyTaaRestoreSet[2] = {};
    // Temporal UPSCALING (2026-10-04): below render scale 100% the sky pass renders into the top-left
    // skyTaaInExtent() of its (full-size) input images with a jitter in INPUT pixels, and the resolve
    // reconstructs the full-resolution frame from it and the history. Nothing is recreated on a scale
    // change (viewport/scissor are dynamic). skyTaaJitterNow = this frame's jitter (input px).
    VkExtent2D skyTaaInExtent(const VulkanContext &ctx) const;
    glm::vec2 skyTaaJitterNow{0.0f};
    bool skyTaaWanted() const;
    void createSkyTaaResources(VulkanContext &ctx);
    void destroySkyTaaResources(VkDevice device);
    bool recordSkyTaa(VkCommandBuffer cmd, VulkanContext &ctx, uint32_t imgIdx);
    bool hovSkyTaa = false;
    double cityPrevObsLatRad = 0.0;
    double cityPrevObsLonRad = 0.0;
    // City day/night detail textures (bindings 14/15): small tileable high-frequency maps,
    // blended onto dayColor/nightColor near cities (see terrain block in sat_sky.frag). Hardcoded
    // tiling scale + distance fade, no CloudParams UBO fields.
    VkImage cityDayDetailImg = VK_NULL_HANDLE;
    VkDeviceMemory cityDayDetailMem = VK_NULL_HANDLE;
    VkImageView cityDayDetailView = VK_NULL_HANDLE;
    VkSampler cityDayDetailSampler = VK_NULL_HANDLE;
    uint32_t cityDayDetailMips = 1;
    VkImage cityNightDetailImg = VK_NULL_HANDLE;
    VkDeviceMemory cityNightDetailMem = VK_NULL_HANDLE;
    VkImageView cityNightDetailView = VK_NULL_HANDLE;
    VkSampler cityNightDetailSampler = VK_NULL_HANDLE;
    uint32_t cityNightDetailMips = 1;
    // Earth specular texture (binding 6): 8K R8_UNORM ocean mask (white=ocean, black=land).
    // Used to gate the wave normal + specular glint material on sea-level sphere hits.
    VkImage earthSpecImg = VK_NULL_HANDLE;
    VkDeviceMemory earthSpecMem = VK_NULL_HANDLE;
    VkImageView earthSpecView = VK_NULL_HANDLE;
    VkSampler earthSpecSampler = VK_NULL_HANDLE;
    uint32_t earthSpecMips = 1;
    // Close-up terrain materials (terrain v2 P3, sky binding 27): RGBA8 2D array, two layers per
    // material (albedo ratio + height; normal XY + roughness + AO), baked by
    // tools/make_terrain_materials.py into assets/textures/terrain_materials.rgba8.
    VkImage terrainMatImg = VK_NULL_HANDLE;
    VkDeviceMemory terrainMatMem = VK_NULL_HANDLE;
    VkImageView terrainMatView = VK_NULL_HANDLE;
    VkSampler terrainMatSampler = VK_NULL_HANDLE;
    void createTerrainMaterials(VulkanContext &ctx);
    // Major roads for the procedural city lights (.plans/CITIES_PLAN.md, sky binding 28): one uint
    // storage buffer baked by tools/make_city_roads.py (header, lat/lon cell lists, float ECEF segments).
    VkBuffer cityRoadsBuf = VK_NULL_HANDLE;
    VkDeviceMemory cityRoadsMem = VK_NULL_HANDLE;
    void createCityRoads(VulkanContext &ctx);
    // City lights as satellite point sprites (city_sprites.comp, .plans/CITIES_PLAN.md): appended to
    // the compact visible list after sat_flare.comp; kCitySpriteMax extra slots are reserved there.
    static constexpr uint32_t kCitySpriteMax = 131072;
    static constexpr int kCitySpriteLevels = 7;          // 64 m .. 4096 m cells, reach 8 .. 1024 km (the last
                                                         // level is twice as wide: 4096 m is the anchor)
    VkDescriptorSetLayout citySpriteDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool citySpriteDescPool = VK_NULL_HANDLE;
    VkDescriptorSet citySpriteDescSet = VK_NULL_HANDLE;
    VkPipelineLayout citySpritePipeLayout = VK_NULL_HANDLE;
    VkPipeline citySpritePipeline = VK_NULL_HANDLE;
    void createCitySprites(VulkanContext &ctx);
    // Earth cloud map (binding 7): 8K R8_UNORM grayscale cloud coverage map.
    VkImage earthCloudsImg = VK_NULL_HANDLE;
    VkDeviceMemory earthCloudsMem = VK_NULL_HANDLE;
    VkImageView earthCloudsView = VK_NULL_HANDLE;
    VkSampler earthCloudsSampler = VK_NULL_HANDLE;
    uint32_t earthCloudsMips = 1;
    // Cloud 3D noise volume (binding 8): 128³ RGBA Perlin-Worley/Worley, baked once at init.
    VkImage cloudNoiseImg = VK_NULL_HANDLE;
    VkDeviceMemory cloudNoiseMem = VK_NULL_HANDLE;
    VkImageView cloudNoiseView = VK_NULL_HANDLE;
    VkSampler cloudNoiseSampler = VK_NULL_HANDLE;
    // Cloud/cirrus domain-warp 3D noise volume (cloud_march.comp binding 9): 128³ RGB, tiling
    // period 16 cells, baked once at init. Replaces cloudWarpOffset's old 3 live warpPerlin3
    // calls with a single texture read — see cloud_warp_noise.comp for the tiling/repetition
    // trade-off this bake deliberately accepted.
    VkImage cloudWarpNoiseImg = VK_NULL_HANDLE;
    VkDeviceMemory cloudWarpNoiseMem = VK_NULL_HANDLE;
    VkImageView cloudWarpNoiseView = VK_NULL_HANDLE;
    VkSampler cloudWarpNoiseSampler = VK_NULL_HANDLE;
    // Aurora 3D noise volume (binding 16): 1024x16x256 RGBA8 — R=curtain fold base,
    // G/B=column-window colA/colB, baked once at init. See aurora_noise.comp.
    VkImage auroraNoiseImg = VK_NULL_HANDLE;
    VkDeviceMemory auroraNoiseMem = VK_NULL_HANDLE;
    VkImageView auroraNoiseView = VK_NULL_HANDLE;
    VkSampler auroraNoiseSampler = VK_NULL_HANDLE;
    // Milky Way skybox texture (binding 13): 8K equirectangular galactic panorama.
    VkImage milkyWayImg = VK_NULL_HANDLE;
    VkDeviceMemory milkyWayMem = VK_NULL_HANDLE;
    VkImageView milkyWayView = VK_NULL_HANDLE;
    VkSampler milkyWaySampler = VK_NULL_HANDLE;
    uint32_t milkyWayMips = 1;
    bool milkyWayBaked = false;   // the RGBA16F panorama with milky_way_glow.r16f in A (sat_sky.frag zoom path)
    bool createMilkyWayBaked(VulkanContext &ctx);
    // Cloud params UBO (binding 9): host-visible, persistently mapped, updated each frame.
    VkBuffer cloudParamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory cloudParamsMem = VK_NULL_HANDLE;
    void *cloudParamsMapped = nullptr;
    // ── Half-resolution cloud march output (C15-perf) ─────────────────────────
    // Written by cloud_march.comp each frame at half ctx.swapExtent; sampled by sat_sky.frag as
    // bindings 10/11 (skyDescSet). Recreated in onResize (swapchain-size-dependent, unlike every
    // other image in this class). Target A: rgb=B_total additive radiance, a=tCloudOcclude.
    // Target B: rgb=A_total multiplicative attenuation, a=cloudBlock sun-dimming scalar.
    VkImage cloudMarchTargetAImg = VK_NULL_HANDLE;
    VkDeviceMemory cloudMarchTargetAMem = VK_NULL_HANDLE;
    VkImageView cloudMarchTargetAView = VK_NULL_HANDLE;
    VkImage cloudMarchTargetBImg = VK_NULL_HANDLE;
    VkDeviceMemory cloudMarchTargetBMem = VK_NULL_HANDLE;
    VkImageView cloudMarchTargetBView = VK_NULL_HANDLE;
    VkSampler cloudMarchSampler = VK_NULL_HANDLE; // shared by both targets; resolution-independent

    // ── Clouds v2 (SatelliteSimCloudsV2.cpp, .plans/CLOUDS_V2_PLAN.md) ──────────────────────────
    // Settings (persisted under "clouds_v2"; harness: set clouds_v2.<key> <value>; Clouds tab).
    // The only volumetric cloud renderer since 2026-09-27 (v1's cloudMarchCS was deleted).
    float cv2Coverage = 0.96f;          // x the weather map's brightness before the remap below
    float cv2CoverClear = 0.0026f;       // map brightness that is clear sky ...
    float cv2CoverFull = 1.0f;         // ... and that is overcast (thin cloud is grey in the imagery)
    float cv2Density = 1.3f;           // x every type's extinction
    float cv2Detail = 0.77f;            // erosion detail strength
    float cv2Wobble = 0.21f;           // 3D noise on a column's edge (fraction of the span): bulges, turrets
    float cv2Lean = 0.15f;             // horizontal shift of a tower per metre of height
    float cv2InteriorErosion = 0.74f;  // share of the erosion that applies deep inside a cloud
    float cv2ColumnEdge = 1.7f;        // 1 / the column edge's softness (fraction of the span)
    float cv2AmbientGain = 2.0f;       // sky light (zenith radiance x this)
    float cv2SunGain = 1.0f;
    float cv2MoonGain = 1.0f;          // x the terrain's moonlight (cloud.moonGain) as the night key light
    float cv2BounceGain = 0.95f;        // light reflected up from the ground
    float cv2Powder = 0.21f;
    float cv2MsExtinction = 0.21f;     // multiple scattering: extinction ratio per octave ...
    float cv2MsStrength = 0.56f;        // ... and contribution ratio per octave
    float cv2PhaseG = 0.57f;            // forward-scattering lobe (silver lining)
    float cv2HistoryWeightMoving = 0.05f; // the same weight once the view moves (parallax past ~2 half-res px)
    float cv2HistoryWeight = 0.046f;     // weight of a new sample over its reprojected history (still view;
                                       // the resolve raises it toward 0.35 with motion)
    float cv2HqHistWeight = 0.0f;       // HQ cinematic export only (cineTick): replaces the still weight per settle frame
                                       // with a running mean's 1/n (and lifts the accumulation cap); 0 = off
    float cv2BeamEyeIrr = 0.0f;           // the Reflect beams' irradiance at the eye (Suns), per frame (rain particles)
    glm::vec3 cv2BeamEyeDir{0.0f, 0.0f, 1.0f};   // its weighted direction to the satellites (ENU)
    float cv2LightLenM = 2500.0f;      // light-march length
    float cv2LightSteps = 7.0f;
    float cv2LightLodFootprintM = 0.0f;   // past this pixel footprint (m) a 2-step light march; 0 = off (the default since 2026-10-11:
                                          // at a long light march its second step is one sample for kilometres - the main source of grain)
    float cv2ShaftTempC = 15.0f;          // the air 500 m over the ground under the eye (deg C): the shafts' rain / snow
    float cv2EyeTempC = 15.0f;            // the air temperature at the eye (deg C; rain / sleet / snow), per frame
    float cv2LightningRate = 1.1f;       // flashes per minute of a full-strength (anvil-reaching) tower; 0 = off
    float cv2LightningGlow = 0.088f;      // the flash's light in the cloud
    float cv2LightningBolt = 1.25f;        // the cloud-to-ground channels
    float cv2LightningSprites = 0.013f;    // chance a ground stroke sets off a red sprite 50-90 km up (0 = never)
    float cv2EvoWindMps = 8.0f;           // the weather map's evolution: its advecting wind (m/s); 0 = static
    float cv2EvoGrowth = 0.12f;           // ... and how much its coverage grows and decays (0 = none)
    float cv2EvoWindowH = 0.01f;            // ... each advected copy's window (h): the displacement's bound
    float cv2EvoDiurnal = 0.6f;           // afternoon convection over land (0 = none)
    float cv2FogAmount = 0.26f;           // fog (cv2FogDust): how readily it forms (0 = none)
    float cv2FogDepthM = 200.0f;          // ... its depth over the smoothed ground (valleys fill deeper)
    float cv2FogDensity = 0.0013f;        // ... its extinction (1/m; ~2.3 km visibility)
    float cv2DustAmount = 0.5f;           // dust over dry land (0 = none)
    float cv2DustHeightM = 1500.0f;       // ... its scale height (m)
    float cv2IceFogAmount = 0.42f;         // ice fog / diamond dust over the ice sheets (0 = none), with ice optics
    float cv2IceFogDensity = 0.000079f;     // ... its extinction at the ground (1/m; ~20 km visibility)
    float cv2DustDensity = 0.00025f;      // ... its extinction at the ground in a full plume (1/m; ~15 km visibility)
    float cv2AdaptiveRate = 0.0f;         // 1 = while moving, full rate only in tiles whose clouds show parallax (dynamic sampling)
    float cv2AdaptiveParallaxPx = 1.6f;   // ... past this many half-res pixels a frame (or history from off screen)
    float cv2HalfRateMoving = 0.0f;       // 1 = where the march would run at full rate, half (a checkerboard)
    // EXPERIMENT (2026-10-21, .plans/CLOUD_REBUILD_PLAN.md S1): the cloud density clipmap. Coarse march steps skip the field
    // in known-empty cells. 0 off, 1 = 9-point bake, 2 = 36-point bake (the measured choice: -4..-9 ms below orbit at full
    // rate, harness_runs/s0). Off until judged in motion.
    float cv2CdcSkip = 2.0f;   // the occupancy clipmap, on by default since 2026-10-22 (36-point bake)
    float cv2CdcBudget = 65536.0f;        // ... cells baked a frame at most
    float cv2CdcFp = -1.0f;               // ... the bake's read footprint: > 0 x the cell, < 0 x the march's own footprint there
    float cv2CdcMaxLevel = 6.0f;          // ... levels the march may use (0-8)
    float cv2CdcFpUsed = -1.0f, cv2CdcMaxLevelUsed = -1.0f, cv2CdcSkipUsed = -1.0f;
    float cv2CdcDustKeyUsed = -1.0f;   // the haboob field the clipmap's cells were baked with (dust on, gain, cloud form)
    float cv2CdcMaxRate = 4.0f;           // ... used only up to this time rate (x real time)
    float cv2CdcLight = 1.0f;             // S2 (2026-10-22): the light march reads the clipmap's mean beyond its first 2 steps
    float cv2CdcLightUsed = -1.0f;
    float cv2CdcLightLevels = 3.0f, cv2CdcLightLevelsUsed = -1.0f;   // ... on levels below this (cells to 400 m)
    float cv2CdcNearM = 0.0f;             // coarse steps skip only beyond this distance (m)
    float cv2FullRes = 1.0f;              // S9: full-resolution clouds (computeCloudExtent; on for High / Ultra, the compiled defaults)
    float cv2FullResMovingMode = 0.0f;    // full-res clouds while the eye moves from altitude (issues 20261022_210358 / _220214): 0 sparse, rotating (default); 1 sparse, held pixel; 2 full rate (every pixel, +1-6 ms; an A/B since the march's depth fix)
    bool  cv2CdcActive = false;           // this frame (fillCloudsV2Params)
    // 2026-10-22 (r1022 orbit, the matrix's "clouds thin out climbing from orbit"): the clipmap's bake reads the field at the
    // march's footprint FROM THE EYE AT BAKE TIME, and its cells never expire with time paused: climbing, cells baked lower
    // (finer footprint: gaps between resolved puffs) read "empty" where the coarser footprint now has cloud, and the march
    // skipped whole cloud masses. The guard drops the clipmap while the eye moves fast for its height and starts a new epoch
    // when it comes back or after the eye has moved a quarter of its distance scale. 0 = the old behaviour.
    float cv2CdcMotionGuard = 1.0f;
    float cv2SunVolumeOn = 1.0f;           // diagnostic (r1022): 0 = no Sun transmittance volume (clouds lit by the light march alone)
    float cv2EyeHeightNow = 1.0f;          // the v2 eye from this frame's height offset, not last frame's GPU readback (0 = old)
    glm::dvec3 cv2CdcRefEye{0.0};          // the eye when the clipmap's current epoch began
    double cv2CdcRefS = 0.0;               // ... and its distance scale then
    bool  cv2CdcWasActive = false;
    int   cv2CdcSlowFrames = 0;
    float cv2MiePhase = 0.11f;             // "Mie phase" (2026-10-22): the droplets' Mie phase's share of single scattering
    float cv2MieOptics = 0.33f;            // "Mie optics": the tuned phase x the Mie table's structure (corona, cloudbow, glory)
    float cv2DropletReffUm = 9.7f;       // ... their effective radius (um): 4 continental .. 15+ maritime / rain-forming
    // 2026-10-11 (clouds_v2.glsl tune / mid2 / mid3 / samp; slots 270-282).
    // 2026-10-13 (issue 20261012_235712, slots 289-295, UBO cu / cu2): the cumulus shape.
    float cv2CuHeightVar = 0.6f;          // "Cumulus height variation": each cloud's top varies by this (x) across a field
    float cv2CuBreakup = 0.6f;            // "Cumulus break-up": the lobes' strength at the base (shallow sheets break into puffs)
    float cv2CuHeadroom = 0.5f;           // "Cumulus top headroom": how far the tops stay under the type's ceiling (0 = cut flat at it)
    float cv2CuFineLobes = 0.5f;          // "Storm cumulus fine lobes": the cumulus-scale lobes kept on deep convection's storm-scale ones
    float cv2CbFineLobes = 0.5f;          // "Cb tower fine lobes": the same on the towers
    float cv2CuWisps = 0.5f;              // "Cumulus wisps": erosion concentrated at the cloud's edge (frayed edges, solid cores)
    float cv2SilverLining = 0.5f;         // "Silver lining": forward-scattered sunlight leaking through thin cloud toward the Sun
    float cv2SmallCumulus = 0.06f;        // the near field makes low puffs this far under the coverage threshold (field units)
    float cv2ErosionVar = 0.6f;           // erosion strength and grain vary by region (0 = uniform)
    float cv2LobeVar = 0.5f;              // the lobes' strength varies by region (0 = uniform)
    float cv2CirrusDirWarp = 0.5f;        // the cirrus streaks' heading swings (their slope; 0 = along the flowed parallels)
    float cv2AsDensity = 2.5f;            // x the altostratus sheets' extinction (the altocumulus keeps "Mid layer density")
    float cv2MidThick = 1.0f;             // x the mid layer's thickness (450-1200 m)
    float cv2MidShadow = 0.3f;            // the share of the mid layer's extinction in the SHADOW it casts (1 = a hard-edged shadow)
    float cv2MidFieldKm = 0.0f;           // the mid layer's field size (km; 0 = the cluster field's patches)
    float cv2MidCover = 0.0f;             // mid layer cover bias (sd of its regime noise; + = more sheet)
    float cv2SampAccum = 39.0f;           // the resolve's per-pixel accumulation: the sample count it caps at (0 = off)
    float cv2SampNewSmooth = 0.42f;        // how much a young pixel leans on its neighbourhood's estimate
    float cv2MsOctaves = 2.0f;            // multiple-scattering octaves (2-8): strength^k x exp(-od x extinction^k)
    // Multiple scattering as a delta-Eddington diffuse field (2026-10-26, cloud_v2_march.comp cv2DiffuseField): dark
    // wisps and edges, bright cores. 0 = the octaves above (with "Powder", which only that model uses).
    float cv2MsModel = 1.0f;              // 0 octaves, 1 delta-Eddington
    float cv2MsDiffG = 0.85f;             // the droplets' asymmetry g (delta-scaled: f = g^2)
    float cv2MsDiffGain = 1.0f;           // x the diffuse field (1 ~ the octaves' energy in a thick cloud; ~1.6 = path traced)
    float cv2MsBackM = 1200.0f;           // how far the march away from the light reaches (m)
    // The sky's light on the clouds (2026-10-26): the hemispheres' mean radiance with multiple scattering
    // (cloud_v2_sunlut.comp layers 2-3); 0 = the zenith x "Sky ambient" with the twilight terms.
    float cv2SkyLightModel = 1.0f;        // 0 zenith, 1 hemispheres + multiple scattering
    float cv2SkyLightGain = 1.0f;         // x the hemispheres' light (1 = physical)
    float cv2SkyGroundAlbedo = 0.25f;     // the ground's albedo in the sky's multiple scattering and under the clouds
    float cv2SkyMs = 1.0f;                // the SKY PASS's multiple scattering (sat_sky.frag, sky_ap.comp; 0 = single + the shadowed-air term)
    float cv2GroundOvercast = 1.0f;       // the ground's light under cloud (cloud_v2_lightning.comp's cv2Vt map; needs cv2SkyMs)
    // The tonemap (2026-10-26, sat_sky.frag): AgX (Sobotka's; Wrensch's fit) in place of 1 - e^-x per channel.
    float cv2Tonemap = 1.0f;              // 0 = 1 - e^-x (with "Highlight roll-off"), 1 = AgX
    float cv2ToneSat = 1.15f;             // AgX look: saturation about the luminance
    float cv2ToneContrast = 1.15f;        // AgX look: power on the curve's output (contrast about mid grey)
    float cv2LobeOcclusion = 0.0f;        // sky light darker in the creases between lobes, brighter on their tips
    float cv2LightFootprint = 0.0f;       // the light march's samples read the field at this x its length (0 = point samples). 0.08 hid grain but a cumulus lost its self-shadow: flat, glowing (issue 20261013_181252)
    float cv2SampClampSd = 1.4f;         // the history clamp in motion (sd of the new samples around)
    // "Sun shadow cell (m)": the Sun transmittance volume's finest level's cell (levels 1-3: x4 / x16 / x64). Key
    // `shadow_ring1_cell_m`, kept from the shaft lattice's ring 1, which it replaced.
    float cv2ShadowRing1CellM = 250.0f;
    float cv2Godrays = 1.0f;              // "God rays": how much of the clouds' shadow the AIR takes (the sky pass reads the STV), 0..1
    float cv2HazeAloft = 1.9f;            // "Haze near clouds": extra aerosol in the sky pass where the weather is (x sea-level Mie)
    float cv2GodrayRangeKm = 400.0f;      // the light volume's half-extent about the eye (km)
    int   cv2ShapeFrame = 0;   // the low shape volume's frame this frame (harness state)
    float cv2StepBaseM = 60.0f;        // step at the eye ...
    float cv2StepGrowth = 0.014f;       // ... growing by this per metre of distance ...
    float cv2StepMaxM = 1700.0f;       // ... capped here (and at 1.2% of the distance beyond)
    float cv2MaxIters = 430.0f;        // march budget per ray (what is left is filled, not dropped)
    float cv2MaxDistKm = 600.0f;
    float cv2FullRateAboveKm = 30.0f;  // above this eye altitude every pixel is marched every frame
    float cv2SparseWhenStill = 0.0f;   // > 0.5: while the view is still, march 1 pixel in 4 (history fills in)
    int   cv2StillFrames = 0;          // consecutive frames with no camera / eye / time-warp motion
    int   cv2EyeStillFrames = 0;       // consecutive frames whose EYE motion showed under 1/4 half-res px of parallax (no time warp, history kept)
    float cv2MidAmount = 0.75f;         // the mid-level layer (altocumulus / altostratus), 0 = off
    float cv2StormScale = 12.0f;        // deep convection's lobes and erosion are this much larger
    float cv2StormDetail = 0.24f;       // share of the erosion kept on deep convection
    float cv2Anvil = 8.0f;             // the anvil shield's density ("Anvils"), 0 = off. With storm objects each storm's
                                       // shield (cv2ShieldAt, 2026-10-23): a puffy layer round the towers' flared heads
                                       // that outlives them. 8 with the user's anvil tune (thickness 3.7 km, hang,
                                       // puffiness, lift, lobes, waist, overshoot, storm erosion), 2026-10-23
    float cv2BaseRoughness = 0.38f;     // bumps on cloud bases (decks most), x the default amplitude
    float cv2HighAmount = 1.3f;        // the high layer (cirrus / cirrostratus / cirrocumulus), 0 = off
    float cv2HighDensity = 0.32f;       // x its extinction
    float cv2CirrusStretch = 11.0f;     // cirrus fibres are this much longer along the wind than across
    float cv2CirrusFieldKm = 3700.0f;  // cirrus regions: the size of the areas the high layer covers (was the
                                       // ~cluster scale: splotches)
    float cv2CirrusFlow = 3.3f;        // the flow warp at cirrus level, x the low cloud's (the jet bends more)
    float cv2CirrusWindMps = 30.0f;    // the jet: cirrus moves this fast eastward over the map
    float cv2RainAmount = 3.0f;         // rain shafts under precipitating cloud, 0 = none
    float cv2OpticsGain = 1.0f;        // halos, sundogs, circumzenithal arc, rainbows
    float cv2RainStreaks = 3.1f;       // the rain particles' visibility (x their real coverage; "Drops at the eye")
    float moveSpeedPerHeight = 1.0f;   // WASD: this many times the height above the ground per second (capped
                                       // at the old fixed 0.08 rad/s, ~510 km/s): slow near the surface
    float cv2DropDistM = 33.0f;        // the rain particles' reach (m; each doubling is one more level, at most 64)
    float cv2RainParticlesK = 16.0f;   // drops in the nearest box, thousands (each farther level holds twice as many)
    float cv2RainShutterMs = 33.0f;    // a drop's streak: how far it falls in this time (ms)
    float cv2LightningStormRate = 0.53f;   // flashes / min per 20 x 20 km of storm at full strength (whole storms flash)
    float cv2LightningTendrils = 1.23f;    // the channels' branching (x the branch and tendril counts)
    // The rain / snow particles' motion (2026-10-23 rework, SatelliteSimRain.cpp): new keys, so a settings file tuned
    // against the old model (fall x10 to hide the lumped far drops' slow drift) does not carry over.
    float cv2RainFallSpeed = 1.0f;      // x the drops' terminal velocities (1 = physical; heavier rain falls faster too)
    float cv2RainWindGain = 1.0f;       // x the surface wind at the eye (a few m/s, from place and time: surfaceWindAt)
    float cv2RainStormWind = 8.0f;      // a storm tower's outflow (m/s at full strength, a few km out), away from it
    float cv2RainGusts = 1.0f;          // the wind's turbulence (x; small continuous swings)
    float cv2GustFronts = 1.0f;         // gust fronts: sudden gusts (1-2 s onset, 5-20 s decay), more near storms
    float cv2DropPersistMs = 60.0f;     // how long one rain particle is drawn before it is redrawn elsewhere (0 = never)
    float cv2SnowWind = 0.7f;          // the flakes' drift x the surface wind ("snow_drift", 2026-10-23; was "snow_wind")
    float cv2ExposureEV = 0.0f;        // exposure compensation (stops) on the sky's auto exposure
    float cv2HighlightRolloff = 1.0f;  // 0 = the old tonemap; 1 = a long highlight shoulder
    float cv2WhiteBalance = 0.6f;      // 0..1: adapt to the sunlight's colour at the observer
    float cv2BeamShafts = 1.0f;        // beams as light in the volumetric march (0 = the old drawn line)
    float cv2AutoExposure = 1.0f;      // 0..1: how far the metered auto exposure may darken a bright day
    // ── Global exposure (2026-09-28) ──
    // The scene is an HDR composite: the sky is tonemapped from radiance, and the post-tonemap terms
    // (satellites, stars, planets, the Milky Way, zodiacal light) carry their own photometry. ONE
    // exposure offset in stops now applies to all of them: the user's Exposure (EV) plus the metered
    // auto exposure. The sky multiplies its radiance by 2^EV; the point sources shift the point style's
    // reference and limit magnitudes by 2.5 log10(2^EV) (brighter exposure = fainter stars show); the
    // Milky Way / zodiacal light scale with it. Auto exposure only DARKENS, and only by day: the night
    // exposures were tuned by hand and stay put.
    float autoExposureEV = 0.0f;       // runtime: the metered offset (<= 0), eased
    float globalExposureEV() const { return cv2ExposureEV + autoExposureEV; }
    float exposureGainMag() const { return 2.5f * 0.30103f * globalExposureEV(); }
    // The meter: the frame's central 60%, blitted to 64x36 (linear RGBA8), copied to the host and read
    // after the next fence (finalizeScreenshot).
    static constexpr uint32_t kMeterW = 64, kMeterH = 36;
    VkImage meterImg = VK_NULL_HANDLE;
    VkDeviceMemory meterImgMem = VK_NULL_HANDLE;
    VkBuffer meterBuf = VK_NULL_HANDLE;   // slotted (one per frame slot): the slot's copy is read after its fence
    VkDeviceMemory meterBufMem = VK_NULL_HANDLE;
    void *meterMapped = nullptr;
    bool meterPendingS[VulkanContext::kMaxFramesInFlight] = {};
    double meterLastT = 0.0;
    float meterMeanLum = 0.0f, meterClipFrac = 0.0f;   // last reading (harness `state`)
    void readExposureMeter();
    float cv2BeamHaze = 1.6f;          // aerosol (haze, dust) in the beams' air scattering, x
    float cv2BeamLight = 32.0f;        // beam light on cloud, x the physical irradiance (the night view's
                                       // moon is ~2400x physical; 1 = beams dim beside it)
    // Per-layer scales, so tuning the cumulus does not move the cirrus or the altocumulus (all three
    // used to read the shape volume at cv2ShapePeriodM).
    float cv2CirrusPeriodM = 11000.0f;  // cirrus fibre spacing across the wind (along it: x stretch)
    float cv2MidPeriodM = 5300.0f;     // altocumulus cloudlets
    float cv2MidDensity = 0.47f;        // x the mid layer's extinction (the low system keeps "Density")
    float cv2CloudSunRayleigh = 0.86f;  // Rayleigh on the sunlight reaching cloud, x physical (the sky's own
                                       // gain no longer reddens every low-sun cloud)
    float cv2TwilightSky = 0.0f;       // extra sky light on cloud while the Sun is low or just set at it
    float cv2BeamLines = 0.25f;        // each beam's own faint streak beside the shafts (the shafts cluster converged beams)
    float cv2FlowWarp = 0.018f;        // the flow field's displacement, as a fraction of its period (curved systems)
    float cv2FlowPeriodKm = 6000.0f;   // the flow field's scale (swirls of about half this)
    float cv2LayerSpread = 0.57f;       // 0: mid/high cloud only over the low cloud; 1: spread around whole systems
    // Convective shape (pass 12): each cell is one cloud with a vertical profile — a narrower base,
    // widest high up, a domed top under the cell's own height — instead of a cone per column.
    float cv2TopHeavy = 1.0f;          // how much narrower the base is than the body (0 = straight sides)
    float cv2TowerTop = 0.85f;         // the tallest cell's top, as a fraction of its type's span (headroom)
    float cv2BaseFlatness = 0.91f;      // cumulus bases: how flat (the 3D lobes damped near the base)
    // Review 22: convective lobes taper up the cloud ("Lobes (3D)" units; replace it on cumulus types).
    float cv2CuLobesBottom = 0.044f, cv2CuLobesTop = 0.23f;   // slots 244 / 245, UBO farLight.zw
    float sunCloudTEased = 1.0f;       // review 22: the Sun disc's cloud transmittance (CloudParams::sunCloudT)
    // Review 22: "Fast-flight cloud LOD" (slot 246, clouds_v2.fast_flight_lod): while boost is held and the eye
    // moves, or the eye moves > 150 m a frame, the march takes <= 2 light steps, 60% of its iteration budget and
    // a 1.5x base step (boosting up through cloud is the most expensive frame there is, and the history is useless).
    float cv2FastFlightLod = 0.0f;
    // Review 22 (experiment): "Foveated full rate (radius)" (slot 247, clouds_v2.fovea_radius, 0 = off): while the
    // adaptive rate runs (the view moves), tiles within this radius of the centre (x the half height) march at
    // full rate whatever their parallax.
    float cv2FoveaRadius = 0.0f;
    bool  boostHeldNow = false, cv2FastLodNow = false;
    // Cumulonimbus as three layers (pass 15): the cumulus field capped at cv2CbCumulusTopKm, discrete
    // concave towers on a lattice (cv2ColumnSigma), and the anvil hanging lower around their heads.
    float cv2CbColumns = 2.0f;         // Cb column density (0 = no towers: a storm is tall congestus; the pass-14 deep cores are gone)
    float cv2CbSpacingKm = 30.0f;      // lattice cell: about one tower per cell where storms are strong
    float cv2CbRadiusKm = 15.0f;        // a tower's base radius (waist ~0.7x, head ~1.6x)
    float cv2CbCumulusTopKm = 12.0f;    // the low (cumulus/congestus) field's top in storm regions
    float cv2CbWaist = 0.89f;           // a tower's waist, x its base radius (1 = none: <1 read as smokestacks)
    float cv2CbFlare = 1.4f;           // its head under the anvil, x its base radius
    float cv2CbHeadDriftKm = 15.0f;     // how far downwind the head is blown
    float cv2CbLobes = 0.37f;          // cauliflower lobes on the tower (fraction of its radius)
    float cv2CbHeadLobes = 0.12f;      // ... and on its flared head (the anvil): smoother reads as the anvil
    float cv2AnvilPuff = 0.45f;          // "Anvil puffiness" (2026-10-22): relief kept on the heads, x(1 + 1.5 it) head lobes, softer edge; 0 = before
    // S3 (2026-10-22): the Sun transmittance volume (cloud_lightning.glsl cv2Stv), the clouds' one Sun shadow. It replaced the
    // fine grid, the shaft rings, the cascade and the layer map (deleted 2026-10-22).
    double cv2StvLatA[4] = {}, cv2StvLatB[4] = {};   // the eye on each level's lattice (cells)
    int    cv2StvFrames[4] = {};
    float  cv2StvCellUsed[4] = {};
    double cv2StvHUsed = 0.0;
    bool   cv2StvOnNow = false;
    glm::dvec3 cv2CdcAnchor{0.0, 0.0, 1.0};        // the clipmap plane's anchor (a unit vector in the DRIFTED frame)
    bool   cv2CdcAnchorSet = false;
    int    cv2StvBoost = 0;                        // frames left of levels 0-1's second bake (catching up)
    float cv2AnvilLiftM = 230.0f;       // "Anvil top lift (m)": how far the head lobes raise / lower its top
    float cv2CbSparsity = 0.33f;       // 0: a tower in every lattice cell of a storm; 1: only the strongest
    float cv2CbFill = 0.48f;            // share of 3x3 lattice blocks holding a storm cell (a dominant tower + a flanking line)
    float cv2CbCumulusVar = 0.8f;      // the storm cumulus top's regional variation (+- fraction of it)
    float cv2CbCumulusReachKm = 31.0f;  // how far beyond a tower's edge the low cloud takes the storm cumulus top
    float cv2CbOvershootKm = 0.14f;     // a tower's overshooting dome above the anvil lid (every tower meets it)
    float cv2AnvilThickKm = 3.7f;      // the anvil shield's thickness over the storm's core
    float cv2AnvilHangKm = 0.87f;       // how much lower it hangs around a tower's head (the mushroom)
    float cv2DetailLodStartM = 8900.0f; // detail erosion fades from here to 4x
    float cv2ShapePeriodM = 1900.0f;   // tiling periods of the noise volumes
    float cv2DetailPeriodM = 1800.0f;
    float cv2CellPeriodM = 32000.0f;
    float cv2ClusterPeriodM = 256000.0f;
    float cv2WindMps = 8.0f;           // noise advection (the weather map itself is static until phase 5)
    float cv2EdgeSharpness = 1.5f;     // density gain after erosion (clamped at 1): harder surfaces
    float cv2WeatherWarpKm = 7.0f;     // mesoscale warp of the weather lookup (hides its 5 km texels)
    int   cv2DebugView = 0;            // 1 iterations, 2 opacity, 3 depth zebra, 4 weather at shell entry
    GpuCloudV2Type cv2Types[kCloudV2Types] = {
        // alt: base, top, flatness (0 dome, 1 deck), base var | shape: ext, erosion, billow, anvil | look: ms, amb, conv, precip
        {{400.0f, 1300.0f, 1.0f, 150.0f}, {0.030f, 0.35f, 0.3f, 0.0f}, {1.0f, 1.0f, 0.0f, 0.5f}},  // stratus
        {{900.0f, 2200.0f, 0.7f, 250.0f}, {0.040f, 0.55f, 0.6f, 0.0f}, {1.0f, 1.0f, 0.5f, 0.5f}},  // stratocumulus
        {{1000.0f, 3200.0f, 0.0f, 300.0f}, {0.050f, 0.75f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 0.5f}},  // cumulus
        {{1000.0f, 7000.0f, 0.0f, 300.0f}, {0.060f, 0.8f, 1.0f, 0.0f}, {1.0f, 1.0f, 1.0f, 1.0f}},   // congestus
        {{900.0f, 11500.0f, 0.15f, 300.0f}, {0.080f, 0.7f, 0.9f, 1.0f}, {1.0f, 0.9f, 0.9f, 2.0f}}};  // cumulonimbus

    // Frame-to-frame state for the temporal resolve.
    uint32_t cv2Frame = 0;
    bool cv2FullRateNow = false;       // this frame's march covers every half-res pixel
    bool cv2FullResHeldNow = false;    // this frame's full-res sparse sample is held at one pixel of each block (cv2FullResMovingMode 1)
    bool cv2HalfRateNow = false;       // ... or, "Half rate while moving", half of them (a checkerboard)
    bool cv2AdaptiveNow = false;       // ... or the adaptive rate: sparse, with full-rate tiles where the parallax is large
    bool cv2SmoothNow = false;         // ... or the full-res grid's smooth tiles at a quarter of its density (pass F)
    bool cv2HistoryValid = false;
    glm::mat4 cv2PrevSkyView{1.0f};
    glm::vec3 cv2PrevObsDir{0.0f, 0.0f, 1.0f};
    glm::dvec3 cv2PrevEye{0.0};
    float cv2PrevTanHalf = 1.0f, cv2PrevAspect = 1.0f;
    double cv2PrevSimT = 0.0;
    double cv2PrevDrift = 0.0;          // the map drift angle last frame (rad)
    uint64_t cv2SettingsHash = 0;      // a change drops the history
    // The Sun shadow grids live in the clouds' space, not on screen: a resize of the screen images (the HQ photo's
    // target, a render-scale step) or a change of the march RATE drops the screen history but not them.
    uint64_t cv2FieldHash = 0;         // cv2SettingsHash without the march rate
    bool cv2ScreenResetOnly = false;   // the history was dropped by recreating the screen images alone

    // Resources. Static: the bakes, the UBO, samplers. Swapchain-sized: the quarter-grid samples
    // and the half-grid resolved/history images.
    VkImage cv2WeatherImg = VK_NULL_HANDLE;
    // Weather EVOLUTION (cloud_v2_weather.comp, decision A): the bake's pipeline and per-mip sets stay
    // alive, and recordWeatherEvolution re-bakes one face per frame (all mips) as sim time moves.
    struct GpuWeatherPC
    {
        int32_t faceSize;
        float srcLod;
        int32_t face;          // -1 = all six
        float pad;             // the convective-variance ring's scale in texels (2 at the 2048 level, else 1)
        glm::vec4 evo0;        // the two advected copies' ages (s) and weights
        glm::vec4 evo1;        // x wind (m/s), y growth, z sim time (s, wrapped), w the faces to bake (bits; 0 = all six)
        glm::vec4 evo2;        // xyz the Sun three hours ago (map frame), w afternoon land convection
        glm::vec4 remap;       // x Coverage, y clear, z full (coarse levels store the true mean coverage), w mip
        glm::vec4 wx;          // xy cos / sin of the map's drift (cv2Drift), z the first row to bake (a slice), w unused
        glm::vec4 storm;       // the storms' own cloud (stormStamp): x tower radius (m), y storm cumulus reach (m),
                               // z head drift (m), w Cb flare
        glm::vec4 storm2;      // x "Storm cloud field" (0 = off), y its clear moat's strength
    };
    static_assert(sizeof(GpuWeatherPC) == 128, "GpuWeatherPC == cloud_v2_weather.comp's PC");
    GpuWeatherPC weatherEvoPC(int face, uint32_t mip) const;
    uint64_t weatherBakeHash() const;  // the settings the weather cube was baked with (a change re-bakes every face)
    void recordWeatherEvolution(VkCommandBuffer cmd);
    void recordWeatherBakeDispatches(VkCommandBuffer cmd, int face, uint32_t faceMask, int slice = -1, uint32_t rectMask = 0u);
    // (2026-10-27, weather perf pass 2) In time warp a face in view bakes its two finest levels only over the rectangle
    // the eye can see (its horizon + the cloud tops beyond it + the flow's reach: weatherViewRect); the rest of the face
    // is due at the out-of-view rate. cv2WxRectT / cv2WxRect: the last such bake (level-0 texels [x0, y0, x1, y1)).
    bool weatherViewRect(int face, const glm::dvec3 &mapEye, double capRad, glm::ivec4 &rect) const;
    bool cv2WxRectBake = true;         // clouds_v2.weather_rect_bake (no UI): the A/B
    float cv2WxRectMaxShare = 0.5f;    // clouds_v2.weather_rect_max_share: a rectangle over this share of a face bakes whole
    float cv2WxRectMarginDeg = 12.0f;  // clouds_v2.weather_rect_margin_deg: the cap past the eye's horizon (deg)
    int cv2WxRectLevels = 5;           // clouds_v2.weather_rect_levels: the levels baked as the rectangle (from 0; >= 128 a side)
    double cv2WxRectT[6] = {-1e30, -1e30, -1e30, -1e30, -1e30, -1e30};
    glm::ivec4 cv2WxRect[6]{};
    uint32_t cv2WxRectBakes = 0;       // faces baked as a rectangle so far (harness state)
    // One face's re-bake outside time warp is spread over kCv2WxSlices frames of rows (a 2048 face is ~1.2 ms).
    static constexpr int kCv2WxSlices = 4;
    int cv2WxSliceFace = -1, cv2WxSliceNext = 0;
    uint64_t cv2WxStormSig = 0;   // the storms uploaded (counts), last frame
    VkImageView cv2WxL1View = VK_NULL_HANDLE;   // level 1 of the cube, read by the bake of level 0
    VkDescriptorSetLayout cv2WxSetLayout = VK_NULL_HANDLE;
    VkPipelineLayout cv2WxPipeLayout = VK_NULL_HANDLE;
    VkPipeline cv2WxPipeline = VK_NULL_HANDLE;
    VkDescriptorPool cv2WxPool = VK_NULL_HANDLE;
    std::vector<VkDescriptorSet> cv2WxSets;
    std::vector<VkImageView> cv2WxMipViews;
    double cv2WxBakedT[6] = {};        // sim time each face was last baked at
    double cv2WxPrevT = -1e300;        // sim time of the last frame's recordWeatherEvolution (its step: time warp)
    bool cv2WxOneFace = false;         // A/B (clouds_v2.weather_one_face): never bake all six faces together
    // "Weather cube tick (s)" (clouds_v2.weather_tick_s, 2026-10-25): with storm objects stamped, a face is re-baked once
    // it is this much sim time behind. It was 45 s: a hurricane moves ~45 m/s through the map's frame (the drift plus its
    // track), so every re-bake stepped its arms ~2 km and its eyewall round, which read from orbit as cells jumping.
    float cv2WxTickS = 10.0f;
    VkQueryPool cv2WxQuery = VK_NULL_HANDLE;   // two timestamps round a bake (harness state: weather.cube.bake_ms)
    // Two stamps per frame slot (frames in flight): a slot's are read when it comes round again, after its fence.
    uint32_t cv2WxSlot = 0;
    bool cv2WxStamped[VulkanContext::kMaxFramesInFlight] = {};
    float cv2WxStampedFaces[VulkanContext::kMaxFramesInFlight] = {}, cv2WxLastBakeFaces = 0.0f;   // faces in the stamped bake (a slice: a quarter)
    float cv2WxBakeMs = 0.0f;                  // the last bake's GPU time
    float cv2WxBakeMsPerFace = 0.0f;           // eased
    uint32_t cv2WxAllSixBakes = 0;     // frames that baked all six faces at once (harness state)
    uint64_t cv2WxHash = 0;            // the evolution settings the faces were baked with
    // ── Weather events: storm objects (SatelliteSimWeather.cpp, shaders/include/cv2_storms.glsl) ──
    // The towers alive this frame, uploaded with a spatial index; the cloud shaders draw towers only from it.
    struct GpuStormTower { glm::vec4 a, b, c; };   // = cv2_storms.glsl CV2Tower
    static_assert(sizeof(GpuStormTower) == 48, "GpuStormTower == cv2_storms.glsl's CV2Tower");
    static constexpr uint32_t kStormMaxTowers = 8192, kStormTableSize = 65536, kStormListSize = 196608,
                              kStormBinsPerFace = 256, kStormMaxPerBin = 12;   // = cv2_storms.glsl's constants
    static constexpr VkDeviceSize kStormTowersOffset = 16;
    static constexpr VkDeviceSize kStormTableOffset = kStormTowersOffset + 48ull * kStormMaxTowers;
    static constexpr VkDeviceSize kStormListOffset = kStormTableOffset + 8ull * kStormTableSize;
    // The anvil SHIELDS (2026-10-23, = cv2_storms.glsl CV2Shield / kCv2Shield*): one per storm, in coarser bins.
    struct GpuStormShield { glm::vec4 a, b, c, d; };
    static_assert(sizeof(GpuStormShield) == 64, "GpuStormShield == cv2_storms.glsl's CV2Shield");
    static constexpr uint32_t kShieldMax = 4096, kShieldTableSize = 32768, kShieldListSize = 131072,
                              kShieldBinsPerFace = 128, kShieldMaxPerBin = 31;   // (31: a cyclone's arm cirrus)
    static constexpr uint32_t kShieldEnvBinsPerFace = 64;   // the bake's envelope index (= kCv2ShieldEnvBinsPerFace)
    static constexpr VkDeviceSize kShieldHeadOffset = kStormListOffset + 4ull * kStormListSize;
    static constexpr VkDeviceSize kShieldsOffset = kShieldHeadOffset + 16;
    static constexpr VkDeviceSize kShieldTableOffset = kShieldsOffset + 64ull * kShieldMax;
    static constexpr VkDeviceSize kShieldListOffset = kShieldTableOffset + 8ull * kShieldTableSize;
    // Tropical cyclones (2026-10-23, = cv2_storms.glsl CV2Cyclone): the bake stamps their bands, eyewall and eye.
    struct GpuStormCyclone { glm::vec4 a, b, c, d; };
    static_assert(sizeof(GpuStormCyclone) == 64, "GpuStormCyclone == cv2_storms.glsl's CV2Cyclone");
    static constexpr uint32_t kCycloneMax = 8;
    static constexpr VkDeviceSize kCycHeadOffset = kShieldListOffset + 4ull * kShieldListSize;
    static constexpr VkDeviceSize kCyclonesOffset = kCycHeadOffset + 16;
    // The ENVELOPE index (2026-10-22): the same towers, each listed out to its envelope (cv2_storms.glsl
    // cv2StormEnvRadius: ~1.8x its reach), read only by the weather bake (its storm cloud field, stormStamp).
    static constexpr uint32_t kStormEnvTableSize = 131072, kStormEnvListSize = 393216;
    // The haboobs' DUST WALL grid (2026-10-24, = cv2_storms.glsl cv2DustHead / cv2DustFrame / cv2DustGrid): weatherFillDustGrid.
    static constexpr uint32_t kDustGridN = 512;   // = kCv2DustGridN
    static constexpr VkDeviceSize kDustHeadOffset = kCyclonesOffset + 64ull * kCycloneMax;
    static constexpr VkDeviceSize kDustFrameOffset = kDustHeadOffset + 16;
    static constexpr VkDeviceSize kDustGridOffset = kDustFrameOffset + 64;
    static constexpr VkDeviceSize kStormEnvTableOffset = kDustGridOffset + 4ull * kDustGridN * kDustGridN;
    static constexpr VkDeviceSize kStormEnvListOffset = kStormEnvTableOffset + 8ull * kStormEnvTableSize;
    // The envelope index's own copy of the towers, a.xyz where the low cloud's weather lookup sees each (the flowed map,
    // cv2FlowDisp): the bake stamps there (buildStormEnvIndex).
    static constexpr VkDeviceSize kStormEnvTowersOffset = kStormEnvListOffset + 4ull * kStormEnvListSize;
    // The shields' copy for the bake (their spines at the flowed positions) and its index.
    static constexpr VkDeviceSize kShieldEnvTableOffset = kStormEnvTowersOffset + 48ull * kStormMaxTowers;
    static constexpr VkDeviceSize kShieldEnvListOffset = kShieldEnvTableOffset + 8ull * kShieldTableSize;
    static constexpr VkDeviceSize kShieldEnvOffset = kShieldEnvListOffset + 4ull * kShieldListSize;
    static constexpr VkDeviceSize kStormBufBytes = kShieldEnvOffset + 64ull * kShieldMax;
    static_assert(kShieldHeadOffset % 16 == 0 && kStormEnvTableOffset % 16 == 0 && kShieldEnvOffset % 16 == 0,
                  "std430: the storm buffer's vec4 arrays start on 16 bytes");
    VkBuffer stormBuf = VK_NULL_HANDLE;
    VkDeviceMemory stormMem = VK_NULL_HANDLE;
    void *stormMapped = nullptr;
    std::unique_ptr<wx::Catalog> stormCatalog;
    std::unique_ptr<wx::StormClimatology> stormClim;
    std::vector<wx::Tower> stormTowers;          // this frame's, strongest first
    struct StormEntry { uint32_t key, tower; };
    std::vector<StormEntry> stormEntries;
    std::vector<glm::uvec2> stormTable;
    std::vector<StormEntry> stormEnvEntries;     // the envelope index's (key, tower) pairs
    std::vector<GpuStormTower> stormGpuTowers;   // this frame's towers as uploaded (the envelope index copies them)
    std::vector<glm::uvec2> stormEnvTable;
    std::vector<uint32_t> stormCounts;
    // The slots each table filled last (fillStormIndex clears and re-uploads only those).
    std::vector<uint32_t> stormTableUsed, stormEnvTableUsed, shieldTableUsed, shieldEnvTableUsed;
    std::vector<wx::Shield> stormShields;        // this frame's anvil shields
    // (2026-10-26) the organised storms alive this frame (id, centre in the map frame): the haboobs' candidates, so a
    // storm whose towers have just died keeps its outflow's dust (haboobFrontOf's lagged towers)
    std::vector<std::pair<uint64_t, glm::dvec3>> stormOrganisedNow;
    std::vector<GpuStormShield> stormGpuShields; // as uploaded (the bake's flowed copy is made from these)
    std::vector<StormEntry> shieldEntries, shieldEnvEntries;
    std::vector<glm::uvec2> shieldTable, shieldEnvTable;
    float stormComplexes = 1.0f;                 // clouds_v2.storm_complexes: x the squall lines' / MCSs' spawn rate
    float stormCyclones = 1.0f;                  // clouds_v2.storm_cyclones: x each basin's tropical cyclones a season
    std::vector<wx::CycloneState> stormCyclonesNow;   // this frame's, strongest first
    float stormSurge = 1.0f;                     // clouds_v2.storm_surge: x the cyclones' storm surge (0 = none, 0..2)
    double surgeAtDir(const glm::dvec3 &earthDir, double *peakOut = nullptr) const;   // the rise at the coast (m), CPU
    glm::dvec3 stormFlowedDir(const glm::dvec3 &wd, double t) const;   // where the low cloud reads the cube for wd
    // Tornadoes and waterspouts (2026-10-23, SatelliteSimWeather.cpp): fixed by their storm's id; drawn as plume
    // Gaussians (the dust list after the launches').
    struct TornadoNow
    {
        bool active = false, spout = false, sea = false;
        double t0 = 0.0, t1 = 0.0;          // J2000 s
        uint32_t seed = 0;
        uint64_t stormId = 0;
        float r0 = 100.0f;                  // its width at the ground (m, radius)
        glm::dvec3 mapDir{0, 0, 1}, earthDir{0, 0, 1};
        glm::dvec3 upwindMap{1, 0, 0};      // map-frame tangent away from the forward flank (the inflow side)
        // Drawn (weatherFillTornadoes, 2026-10-23): the DRAWN cloud's base over it (the GPU probe, eased; else the Cb
        // type's base), the optical depth of the cloud above, and the light on the funnel relative to clear sky.
        float baseAglM = 0.0f, tauAbove = 0.0f, lightK = 1.0f;
        bool  baseProbed = false;
        // (2026-10-27, issue 20261027_212628) 0..1: whether a real storm base stands over it (the probe found cloud within
        // kTornadoMaxBaseAglM of the ground with at least kTornadoMinTauAbove above it), eased. The funnel, wall cloud,
        // debris and roar are x this: no cloud over the column, no tornado.
        float present = 0.0f;
    };
    std::vector<TornadoNow> tornadoesNow;   // drawn this frame
    // The tornado probes (cloud_v2_lightning.comp threads 2-5): the columns asked for this frame (storm ids, 0 = none) and
    // what came back, per storm, eased (base altitude m ASL, optical depth above).
    // Per frame slot (frames in flight): the columns a frame asked for and whether its lightning pass answered
    // them; the slot's copy of the flash buffer's head holds that frame's answers.
    uint64_t tornadoProbeIdsS[VulkanContext::kMaxFramesInFlight][4] = {};
    bool tornadoProbeRanS[VulkanContext::kMaxFramesInFlight] = {};
    bool harnessTornadoHidden_ = false;   // harness `storms tornado draw=0`: the funnels are listed but not drawn (A/B)
    // found: the LAST answer held a base (0xFFFF = no cloud in the column); present: the eased presence (see TornadoNow).
    struct TornadoProbe { float baseAsl = 0.0f, tau = 0.0f, present = 0.0f; bool valid = false, found = false;
                          double wall = 0.0, presWall = 0.0; };
    std::unordered_map<uint64_t, TornadoProbe> tornadoProbes;
    // Each funnel's ground (2026-10-25): the full DEM under it (launchsim::demElevCpu), xyz = the Earth-fixed direction it
    // was read at, w = metres above sea level; re-read once the funnel has moved ~500 m. (weatherFillTornadoes)
    std::unordered_map<uint64_t, glm::dvec4> tornadoGround;
    bool tornadoOf(const wx::Storm &S, double t, TornadoNow &out) const;   // its tornado (if any) and where at t

    // ── The Weather window's FEATURED events (2026-10-23, SatelliteSimWeatherFeatured.cpp) ─────────────────────
    // Reserved slots, always in the same order and the same size (the user: "anytime a new storm appears the window's
    // extents change"; "we should always have a tornado, haboob ready to go to"; cool events over storms near you): the
    // next tornado, the strongest hurricane / typhoon, the next haboob, the strongest supercell, the strongest squall
    // line / MCS, the next waterspout, the biggest wildfire. Tornadoes, waterspouts and haboobs are searched FORWARD in
    // sim time (up to 24 / 24 / 48 h) on a second storm catalog (`wxScout`, same configuration: asking the main one for
    // another time evicts the bins the renderer needs), a few steps a frame while the window is open; the rest come from
    // the storms / cyclones alive now and the fires burning now (worker thread: firesWorld scans the globe).
    enum WxFeatKind { kFeatTornado = 0, kFeatHurricane, kFeatHaboob, kFeatSupercell, kFeatLine, kFeatSpout, kFeatFire, kFeatCount };
    struct WxFeatured
    {
        bool found = false;           // something to show
        bool searching = true;        // the forward search has not reached it yet
        double tEvent = 0.0, tEnd = 0.0;   // J2000 s: the moment to watch, and when it is over
        bool day = true;              // the Sun up there at tEvent
        glm::dvec3 earth{0.0, 0.0, 1.0};   // Earth-fixed, at tEvent
        bool hasStorm = false;
        wx::Storm storm;              // a copy (the catalogs evict)
        TornadoNow torn;              // tornado / waterspout slots
        bool spout = false, landOk = false;   // (its rank: a tornado over land beats one over water)
        int rank = -1;                // tornado / waterspout: day 8 + land 4 + drier region 2 + outside the tropics 1
        WxStormRow row;               // supercell / line slots (alive now)
        wx::Cyclone cyc, cycNext;     // hurricane slot: the strongest active one, and the next major one
        bool hasCyc = false, hasCycNext = false;
        wx::Fire fire;
        wx::FireState fireState;
    };
    WxFeatured wxFeat[kFeatCount];
    std::unique_ptr<wx::Catalog> wxScout;
    wx::Env wxScoutEnv;               // the main catalog's configuration (SatelliteSimWeather.cpp keeps a copy)
    wx::Settings wxScoutSettings;
    uint64_t wxScoutKey = 0;
    double wxScanBase = -1e300;       // the forward search's start (sim s)
    int wxScanStep = 0;               // the next 30-min step
    bool wxScanDone = true;
    int wxScanRestarts = 0;
    std::unordered_set<uint64_t> wxScanSeen;   // tornado / waterspout candidates already looked at
    // The search's state on its worker (perf4-cpu 2026-10-24: the steps ran on the main thread, up to ~12 ms each).
    struct WxScanState
    {
        double base = 0.0, now = 0.0;
        int step = 0;
        bool done = false;
        std::unordered_set<uint64_t> seen;
        WxFeatured ft, fs, fh;   // the tornado, waterspout and haboob slots as the search left them
    };
    std::future<WxScanState> wxScanJob;
    std::atomic<bool> wxScanCancel{false};
    bool wxScanJobStale = false;   // a restart while it ran: its result is dropped
    void weatherScanStep(WxScanState &S) const;
    void weatherFeaturedFinish(double maxS);   // harness: the search to its end
    double wxFeatNowWall = -1e300, wxFeatNowSim = -1e300;
    double wxFireSim = -1e300;
    std::future<std::vector<wx::Fire>> wxFireJob;
    double wxFireJobT = 0.0;
    int wxFeatPendingSlot = -1, wxFeatPendingAct = 0;   // applied after the layout
    std::vector<uint64_t> wxNearSlotIds;                // the Storms tab's sticky "near you" slots
    std::vector<WxStormRow> wxNearSlots;
    void weatherFeaturedUpdate();                       // per frame while the window is open (bounded)
    void weatherFeaturedNow(const std::vector<WxStormRow> &alive);   // the supercell / line slots (rows refresh)
    void weatherFeaturedRestart(double now);
    bool weatherFeaturedAct(int slot, int act, std::string &msg);
    bool weatherWatchTornadoOf(const wx::Storm &st, int mode, std::string &msg);   // 0 a minute before, 1 close, mid-way
    double weatherSunSin(const glm::dvec3 &earth, double t) const;
    void buildWeatherFeatured(const UIInput &inp, UIRenderer &ui);
    nlohmann::json weatherFeaturedJson();
    void weatherFillTornadoes(GpuDustCloud *out, uint32_t &n);
    double stormUploadedT = -1e300;
    uint64_t stormEnvKey = 0;
    bool  stormObjects = true;                   // weather.storm_objects (off = no towers at all)
    float stormFrequency = 1.0f;                 // weather.storm_frequency: x the calibrated spawn rate
    float stormLifeScale = 1.0f;                 // weather.storm_life: x every storm's lifetime
    float stormCloudField = 1.0f;                // clouds_v2.storm_field: each storm's own cloud in the weather cube
                                                 // (cloud_v2_weather.comp stormStamp; 0 = the map alone)
    float stormMoat = 1.0f;                      // clouds_v2.storm_moat: the clear moat round a storm (x the field)
    bool  stormIndexTight = true;                // clouds_v2.storm_index_tight: list a bin only where a tower / shield can reach it
                                                 // (2026-10-27; false = every bin of the bounding rectangle, the A/B)
    int   stormShieldBinsAB = 0;                 // clouds_v2.storm_shield_bins_ab (no UI): 64 = list shields in the old
                                                 // 156-km bins (an A/B); 0 = kShieldBinsPerFace (128)
    float stormArmCirrus = 1.0f;                 // clouds_v2.storm_arm_cirrus: the cirrus along a cyclone's arms (x, 0 = none)
    // Wildfires (2026-10-23, Wildfire.h, SatelliteSimWildfire.cpp): hashed on the biome map's fuel x the season's
    // dryness; smoke as plume Gaussians (a contiguous range of the dust list, spectral: smoke.glsl), the front as point
    // sprites (kFireBuf) and its light as engine lights.
    float wildfireRate = 1.0f;     // clouds_v2.wildfires: x the calibrated fire rate (0 = none)
    float wildfireSmoke = 1.0f;    // clouds_v2.wildfire_smoke: x the smoke's optical depth
    float wildfireLight = 1.0f;    // clouds_v2.wildfire_light: x the fire front's light (points and the glow)
    // The smoke's look (2026-10-26, issues 20261026_204045 / _204239: "too beige", "do we have any controls for smoke?"):
    float wildfireSmokeBright = 1.0f;   // clouds_v2.wildfire_smoke_brightness: x the light the smoke scatters
    float wildfireSmokeTint = 0.0f;     // clouds_v2.wildfire_smoke_tint: -1 blue-grey .. 0 neutral grey .. +1 brown
    int wildfireFlameBand = 1;           // clouds_v2.wildfire_flame_band (no UI): 1 = the close-up flames as a band of tongues on screen, 0 = the old sparse tongues (A/B)
    float flameLodScale = 1.0f;          // x the close-up flames' spacing on screen: widened when they overflow their budget
    float flameNearScale = 1.0f;         // x the close-up flames' spacing floor near the eye (1.2 x the flame height): likewise
    bool flameOverflow = false, flameTight = false;   // this frame: the tongues did not fit / fitted only with fewer flames each
    int flameMaxSubLast = 4;             // this frame: the most flames a resolved tongue was given (harness `fires`)
    uint32_t flamesWantedLast = 0;
    int wildfireDebug = 0;               // clouds_v2.wildfire_debug (no UI, harness): bits 1 no flames, 2 no points, 4 no smoke, 8 no smoke at the flames, 16 no embers
    int wildfireSmokeCurtain = 1;        // clouds_v2.wildfire_smoke_curtain (no UI): 1 = smoke Gaussians from the smoke's own spread
                                         // (curtains along the front, nested nodes along a segment, 2026-10-28), 0 = the old pieces (A/B)
    bool wildfireScars = true;           // clouds_v2.wildfire_scars (no UI): the burn scars on the ground (off: isolate the smoke)
    int wildfireSmokeAlloc = 1;          // clouds_v2.wildfire_smoke_alloc (no UI): 1 = trail pieces shared by what is on screen, 0 = the old fair share (A/B)
    float wildfireSmokeSpread = 1.0f;   // clouds_v2.wildfire_smoke_spread: x the plumes' width (same mass: thinner)
    float wildfireSmokeSpectral = 0.6f; // clouds_v2.wildfire_smoke_spectral: x the Angstrom exponent (1 = ~1.6: the
                                        // Sun / sky through smoke reddened and its thin parts blue; 0 = grey)
    // Smoke trajectories (2026-10-26, SatelliteSimWildfire.cpp): each parcel advected from its emission through the wind
    // at its own place and time, cached and only extended (a pure function of its emission time).
    struct SmokeEmit
    {
        int nS = 0, mainSrc = 0;
        glm::dvec2 c[5]{};
        double w[5]{}, ext[5]{};
        glm::dvec2 ax[5]{};                    // each section's principal axis along the front (unit, xy)
        double extL[5]{}, extW[5]{};           // its rms spread along / across that axis (m)
        // Each section as 4 SUB-PIECES of equal heat along the front (2026-10-29): offset of the sub-piece's centre from
        // the section's centre (m), its share of the section's heat, and its 2x2 covariance (xx, xy, yy, m^2) about its
        // own centre. The curtain is drawn as a chain of these (merged 4 -> 2 -> 1 as it spreads), never as one straight
        // Gaussian across a curved front.
        glm::dvec2 subOff[5][4]{};
        double subW[5][4]{};
        glm::dvec3 subC[5][4]{};
        double H = 0.0, act = 0.0, Q = 0.0;
        glm::dvec2 wDir{0.0, 1.0};
        bool ready = false;
        uint32_t frame = 0;
    };
    struct SmokeTraj
    {
        const SmokeEmit *em = nullptr;
        const wx::Fire *f = nullptr;   // (valid while being extended this frame only)
        int si = 0;                    // the source; 5 = the low layer (main source), 6 = the pall's parcels
        double tE = 0.0, Hb = 0.0, w0 = 0.0;
        std::vector<glm::dvec2> p;     // positions at the schedule's ages a (fireSmokeStepS)
        std::vector<double> a;
        double need = 0.0;             // the age it must reach this frame
        uint32_t frame = 0;
    };
    std::unordered_map<uint64_t, SmokeEmit> smokeEmits;
    std::unordered_map<uint64_t, SmokeTraj> smokeTrajs;
    uint32_t smokeFrame = 0;
    uint32_t smokeTrajSteps = 0;       // steps integrated this frame (harness `fires`)
    std::vector<std::array<float, 9>> smokeSegDbg;   // the nearest fire's segments (harness `fires`)
    std::vector<wx::Fire> fireCache;           // born in [fireCacheB0, fireCacheB1] round (fireCacheLat, fireCacheLon)
    double fireCacheLat = 1e9, fireCacheLon = 1e9, fireCacheR = 0.0;
    int64_t fireCacheB0 = 0, fireCacheB1 = -1;
    float fireCacheRate = -1.0f;
    struct FireNow
    {
        wx::Fire f;
        wx::FireState s;
        double rangeKm = 0.0;
        double reachK = 1.0;                   // faded out over the last 20% of the reach (no pop)
        float groundM = 0.0f;                  // the ground under its ignition point (the full DEM, cached per fire)
        int gauss = 0, points = 0;
        std::vector<glm::vec4> knots;          // the upper smoke layer's knots (x, y m east / north of ignition, z, age s)
    };
    std::vector<FireNow> firesNow;             // alive and in reach this frame, nearest first
    std::unordered_map<uint64_t, float> fireGroundCache;   // fire id -> the full DEM's height there (a file read each)
    float fireLodScale = 1.0f;                 // x the front points' spacing: widened when a crowd of fires overflows
                                               // the frame's point budget, relaxed back slowly (the levels cross-fade)
    uint32_t firePointsLast = 0, fireFlamesLast = 0, fireLightsLast = 0, fireEmbersLast = 0, nearSmokeLast = 0;   // this frame (harness `fires`)
    std::vector<glm::vec4> fireGlowDbg;   // this frame's glow lights: ENU from the eye (m) xyz, w = r0 (harness `fires`)
    double firePeakFlare = 0.0, fireFlareSum = 0.0, fireGlowSum = 0.0;     // this frame's points / glow (harness `fires`)
    uint32_t smokeSegsLast = 0, smokePiecesLast = 0, smokeSinglesLast = 0, smokeOffViewLast = 0, smokeLostLast = 0;   // (harness `fires`)
    uint32_t fireSmokeSources = 0;                                          // this frame's smoke streaks (harness `fires`)
    // The night map's luminance (earthNightCpu: ~18 km, 0..255 sRGB; the land's blue base ~14): the built-up mask.
    float nightLumCpu(float latDeg, float lonDeg) const
    {
        if (earthNightCpu.empty()) return 0.0f;
        const int x = std::clamp((int)((lonDeg + 180.0f) / 360.0f * earthNightCpuW), 0, earthNightCpuW - 1);
        const int y = std::clamp((int)((90.0f - latDeg) / 180.0f * earthNightCpuH), 0, earthNightCpuH - 1);
        return (float)earthNightCpu[(size_t)y * earthNightCpuW + x];
    }
    // The fire's points and close-up flames stand on the DRAWN ground (2026-10-24): wildfire_ground.comp moves each one
    // to the detailed terrain (DEM + the procedural detail, terrain_detail.glsl) under it before they are drawn. The
    // CPU only knows the DEM (and the 18-km copy between): close up most points sat under the drawn surface and the
    // depth test hid them ("they disappear when you are close").
    uint32_t firePtFirst = 0, firePtCount = 0;     // kFireBuf's wildfire points this frame
    uint32_t flameFirst = 0, flameCount = 0;       // the dust list's flame Gaussians this frame
    VkDescriptorSetLayout fireGroundDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool fireGroundDescPool = VK_NULL_HANDLE;
    VkDescriptorSet fireGroundDescSet = VK_NULL_HANDLE;
    std::array<uint64_t, 6> fireGroundBound_{}; // the handles its set was last written with
    VkPipelineLayout fireGroundPipeLayout = VK_NULL_HANDLE;
    VkPipeline fireGroundPipe = VK_NULL_HANDLE;
    bool fireGroundFailed = false;
    void createWildfireGround(VulkanContext &ctx);
    void recordWildfireGround(VkCommandBuffer cmd, VulkanContext &ctx);
    void weatherFillFireParams(GpuCloudParams &cp);   // the burn scars (sat_sky.frag fireScarAt): last frame's firesNow
    void destroyWildfireGround(VkDevice device);
    uint32_t smokeHdrBits = 0;
    uint32_t aerosolFirst = 0xFFFFFFFFu;       // this frame's first haboob Gaussian (weatherFillHaboobs; one range with the smoke)                 // the dust list header word 1's smoke range (smoke.glsl), this frame's
    struct FirePtSeg   // a segment's points (weatherFillWildfires builds the segments in parallel)
    {
        std::vector<GpuFireball> pts;
        std::vector<double> fl;
    };
    std::vector<FirePtSeg> firePtSeg;
    void weatherFillWildfires(GpuDustCloud *out, uint32_t &n, double pixFull);
    // Haboobs (2026-10-23, SatelliteSimHaboob.cpp): the dust walls on storms' gust fronts over the biome map's dust.
    struct HaboobPoint
    {
        glm::dvec3 dir{0, 0, 1}, out{1, 0, 0}, tan{0, 1, 0};   // Earth-fixed: position, outward (the front's motion), along
        double strength = 0.0;
    };
    float haboobGain = 1.0f;          // clouds_v2.haboobs: x the dust walls (0 = none)
    int haboobsNow = 0;               // storms drawing one this frame
    double haboobNearestKm = -1.0;
    // (towers: the towers to build it from; nullptr = this frame's, stormTowers. lagCat: the catalog the lingering
    // outflow's past towers come from; nullptr = the main catalog when towers is nullptr, else none)
    void haboobFrontOf(const wx::Storm &S, double t, std::vector<HaboobPoint> &pts,
                       const std::vector<wx::Tower> *towers = nullptr, wx::Catalog *lagCat = nullptr) const;
    double haboobPointK(const HaboobPoint &q) const;   // the dust raised there: the biome map's dust x the front's strength x gain
    // The haboob of storm S at t: the front point with the most dust (false: none drawn). Builds the storm's towers
    // itself, from cat (nullptr = the main catalog; the Weather window's forward search passes its scout).
    bool haboobStrongestPoint(const wx::Storm &S, double t, HaboobPoint &best, double &bestK, wx::Catalog *cat = nullptr) const;
    // The Sun on a haboob's face at an Earth-fixed point, clear of its storm's anvil (0..1), and the storm's parts for it.
    double haboobSunOnFace(const glm::dvec3 &pE, double t, const std::vector<wx::Tower> &tw,
                           const std::vector<wx::Shield> &sh) const;
    void haboobStormParts(wx::Catalog &cat, const wx::Storm &S, double t, std::vector<wx::Tower> &tw,
                          std::vector<wx::Shield> &sh) const;
    void weatherFillHaboobs(GpuDustCloud *out, uint32_t &n);
    // The haboobs as CLOUD (2026-10-24): their dust walls rasterised into the storm buffer's dust grid round the observer
    // (cv2_storms.glsl cv2DustWallAt), drawn by the clouds v2 march (clouds_v2.glsl cv2DustWallSigma). Within the grid the
    // plume Gaussians give way to it.
    bool haboobField = true;          // clouds_v2.haboob_field (0 = the plume Gaussians everywhere, the old look)
    void weatherFillDustGrid();
    std::vector<uint32_t> dustGridCpu;           // the grid as written (kDustGridN^2)
    glm::ivec4 dustBox{0, 0, 0, 0};              // the cells written last time (lo x, lo y, hi x, hi y; hi exclusive)
    glm::dvec3 dustGridE{1, 0, 0}, dustGridN{0, 1, 0}, dustGridUp{0, 0, 1};
    double dustGridCellM = 0.0;                  // 0 = no grid this frame
    double dustGridT = -1e300;                   // the sim time the grid was built for
    glm::dvec3 dustGridEye{0, 0, 0};
    glm::dvec3 dustNoiseOff{0, 0, 0};            // the lobe noise's displacement (m, ECEF): the wall's motion + the rise
    glm::dvec3 dustNoiseVel{0, 0, 0};            // the nearest wall's motion over the ground (m/s, ECEF)
    static constexpr double kDustLobePeriodM = 4000.0;   // the walls' lobe noise period: lobes of ~1 km, 500 m, 250 m
    double dustNoiseT = -1e300;
    int dustWallsNow = 0, dustCellsNow = 0;      // storms in the grid, cells with dust (harness `fires`)
    bool dustGpuOn = false;                      // the grid's head says a wall: the march with the haboob code (perf pass 4)
    double dustGridMs = 0.0;
    nlohmann::json wildfiresJson(bool world);
    double stormPrevUploadT = -1e300;            // the previous upload's sim time (a jump re-bakes the weather cube)
    double stormLastFrameT = -1e300;             // last frame's sim time, and how many frames in a row moved > 2 h:
    int    stormFastFrames = 0;                  // past one, the storms are hidden (time warp of ~1 week/s and up)
    struct StormStats { int towers = 0, bins = 0, entries = 0, truncated = 0, lost = 0, dropped = 0; double cpuMs = 0;
                        int envBins = 0, envEntries = 0, envTruncated = 0, envLost = 0;
                        int shields = 0, shieldBins = 0, shieldEntries = 0, shieldTruncated = 0, lineSystems = 0;
                        double msCatalog = 0, msTowerIndex = 0, msShields = 0; int cyclones = 0; } stormStats;
    void createStormBuffer(VulkanContext &ctx);
    void destroyStormBuffer(VkDevice device);
    void updateStorms();                         // per frame, before the clouds are recorded
    void buildStormEnvIndex();                   // on a frame the weather bake runs (its storm cloud field)
    // (2026-10-27, weather perf pass 2) The envelope index is built on a worker (stormEnvJob), its own threads, not the
    // storm pool (the main thread uses that later in the frame), and joined before the frame's uploads
    // (finishStormEnvIndex: recordScreenshotCopy; also at the top of updateStorms and before a new job). It reads only
    // what updateStorms left this frame. clouds_v2.storm_env_async (no UI): false = built on the frame, the A/B.
    struct StormEnvDirty { uint32_t n = 0, listUsed = 0, nS = 0, shieldListUsed = 0; };
    StormEnvDirty buildStormEnvIndexWork(bool ownThreads);
    void finishStormEnvIndex();
    std::future<StormEnvDirty> stormEnvJob;
    // (2026-10-27) A job still running kStormEnvJoinWaitMs into the frame's end is not waited for (it was 11.7 ms in one
    // frame at 1 mo/s): the
    // frame's bake reads the previous index (the GPU copy: the job's ranges were not marked dirty), the job is joined at
    // the next updateStorms and uploaded that frame, and that frame starts no new job (stormEnvHoldFrame: the shadow
    // ranges it uploads must not be written meanwhile).
    bool stormEnvLate = false, stormEnvHoldFrame = false;
    static constexpr float kStormEnvJoinWaitMs = 1.0f;   // the most the frame waits for the job at its end
    uint64_t stormEnvLateCount = 0;   // (harness state weather.cube.env_late)
    double stormEnvJobT = -1e300;
    float stormEnvJobMs = 0.0f;     // the last envelope index's own time (harness state weather.cube.env_job_ms)   // the sim time the job in flight was started for
    bool cv2WxLastAllSix = false;
    bool stormsUpdatedEarly = false;   // updateStorms ran at the top of this frame's recordCompute (time warp)
    bool stormsMainPathLast = false;   // last frame went through recordCloudsV2 (not Potato's flat path)   // last frame's bake was a time-warp bake (the index's job starts early)
    void startStormEnvJob();
    bool stormEnvAsync = true;
    uint64_t stormEnvHash() const;
    // The towers nearest the observer (Earth-fixed lat / lon, distance, bearing, strength ...); dominantOnly: role 1.
    nlohmann::json stormNearestJson(int n, float minStrength = 0.0f, float rangeKm = 1e9f, bool dominantOnly = false) const;
    uint32_t cv2WxRebakes = 0;         // faces re-baked so far (harness state)
    VkDeviceMemory cv2WeatherMem = VK_NULL_HANDLE;
    VkImageView cv2WeatherView = VK_NULL_HANDLE;
    VkImage cv2ShapeImg = VK_NULL_HANDLE, cv2DetailImg = VK_NULL_HANDLE, cv2MesoImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2ShapeMem = VK_NULL_HANDLE, cv2DetailMem = VK_NULL_HANDLE, cv2MesoMem = VK_NULL_HANDLE;
    VkImageView cv2ShapeView = VK_NULL_HANDLE, cv2DetailView = VK_NULL_HANDLE, cv2MesoView = VK_NULL_HANDLE;
    // Review 17: cloud morphology (closed / open cells, streets, cumulus fields), assets/textures/cloud_morph.rgba8.
    // The cloud droplets' Mie phase (2026-10-22, tools/make_mie_lut.py -> assets/textures/cloud_mie.rgba16f): NA angles x
    // one row per effective radius, march binding 24. cv2MieReff: the rows' radii (um).
    // The shape volume with cap-profile lobes (2026-10-22, noise mode 3, "shape_caps"): baked beside cv2ShapeImg with
    // the same seed; cv2SyncShapeVolume (top of recordCompute, before anything binds the sets) swaps the descriptors.
    VkImage cv2ShapeCapsImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2ShapeCapsMem = VK_NULL_HANDLE;
    VkImageView cv2ShapeCapsView = VK_NULL_HANDLE;
    float cv2ShapeCaps = 1.0f, cv2ShapeCapsUsed = 0.0f;
    VkImageView cv2ShapeActiveView() const { return (cv2ShapeCaps > 0.5f && cv2ShapeCapsView) ? cv2ShapeCapsView : cv2ShapeView; }
    void cv2SyncShapeVolume(VulkanContext &ctx);
    VkImage cv2MieImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2MieMem = VK_NULL_HANDLE;
    VkImageView cv2MieView = VK_NULL_HANDLE;
    std::vector<float> cv2MieReff;
    void createCloudMie(VulkanContext &ctx);
    VkImage cv2MorphImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2MorphMem = VK_NULL_HANDLE;
    VkImageView cv2MorphView = VK_NULL_HANDLE;
    void createCloudMorph(VulkanContext &ctx);
    float cv2MorphOrbit = 0.8f;        // "Morphology (orbit)": its share of the far field's presence
    // "Imagery share" (review 19): the morphology's fixed weight in the low cloud's placement, added to the
    // cells thresholded by the map (0.3 x it in field units). The same at every distance, so the clouds stay
    // put with altitude; higher, the imagery's 5-50 km structure shows more up close and from orbit alike.
    float cv2ImageryShare = 0.5f;
    float cv2MorphPeriodKm = 320.0f;   // "Morphology period (km)": one tile of the texture
    // "Far-field sharpness" (review 18, octaves): the far field's 2D reads are filtered to the footprint x
    // 2^(1 - this). 0 = twice the footprint (each visit's ray then reads about the same value: no flicker);
    // higher = sharper, the history's sub-texel jitter supersampling the finer structure, at some grain.
    float cv2MorphBreakup = 0.0f;
    // The FAR CLOUD LAYER (review 18, cloud_v2_far.comp): from afar the low clouds are a full-resolution 2D
    // layer evaluated from the same field as the march's far field (the map's fraction + the imagery
    // morphology), shaded as a slab, instead of the half-res volumetric march — the march cannot resolve
    // the 1-5 km structure from orbit, and costs more. Fades in from "Far cloud layer from (km)" of eye
    // altitude, full (and the march skipped) at "... full at (km)".
    float cv2FarLayerFromKm = 600.0f;
    // Review 24 (slots 251-254): the far layer's apparent coverage at a slant (the cells' sides fill the gaps), a bias
    // on its coverage (field units), its optical depth and the softness of its sub-pixel edges.
    // The bias is 0 since issue 20261021_173412: cv2FarColumn finds the march's own (eroded) cloud edge, and the old
    // +0.12 doubled the far layer's cloud against the march's.
    float cv2FarSlant = 0.91f, cv2FarCoverBias = -0.1f, cv2FarDensity = 1.2f, cv2FarSoftness = 1.0f;   // review 24b: the user's tuning
    float cv2FarLowSun = 0.38f;   // slot 255: the far layer's key light goes as mu0^(1 / (1 + this)) (0 = a flat slab)
    float cv2FarKeyGain = 4.1f, cv2FarSkyGain = 0.6f;   // review 24b: the user's tuning (was 10 / 0.3)   // "Far cloud layer sunlight / sky light" (review 21: matched to the march's cloud radiance at 1460 km and its image at 3000-8000 km)
    float cv2FarLayerFullKm = 8900.0f;   // review 24b: the user's tuning (was 1500)
    // The volumetric clouds are off (knockout 32768: Planetarium, Potato, or the Performance tab). Then nothing of
    // clouds v2 runs on the GPU — march, resolve, far layer, lightning, rain map — and every host consumer of
    // their outputs (lightning bolts, rain particles, thunder, the rain ambience, the Sun's cloud transmittance)
    // treats them as absent rather than reading the last frame they were written.
    bool cloudsV2Off() const { return (debugDisableMask & 32768u) != 0u; }
    // Tier gating (2026-10-29): the Weather window and its Settings tab are greyed out without the volumetric
    // clouds (Planetarium, Potato): storms, rain, fog and the radar are all drawn by them.
    bool weatherUnavailable() const { return cloudsV2Off(); }
    // The Potato sky (knockout 262144): no meshes, no cloud composite, no dust / plume list.
    bool potatoSkyOn() const { return (debugDisableMask & 262144u) != 0u; }
    // Planetarium's SKY_LITE or Potato: no meshes (kNoMeshBits), no procedural ground.
    bool lightSkyOn() const { return (debugDisableMask & (262144u | 524288u)) != 0u; }
    // What a Watch / Go needs to land on something visible. tierNeedReason = nullptr when this graphics mode draws
    // it, else a short line for the disabled button's tooltip (SatelliteSimBookmarks.cpp, SatelliteSimSkyEvents.cpp,
    // SatelliteSimMissions.cpp).
    enum TierNeed : uint32_t
    {
        kNeedClouds = 1u,  // the volumetric clouds (storms, rain, snow, rainbows): off on Planetarium and Potato
        kNeedAurora = 2u,  // the aurora (knockout 32): off on Planetarium and Potato
        kNeedEclipse = 4u, // the Moon over the Sun / in the Earth's shadow: sat_sky_minimal.frag draws neither
    };
    static_assert(kNeedEclipse == 4u, "SatelliteSimSkyEvents.cpp skyKindNeeds() writes it as 4u");
    const char *tierNeedReason(uint32_t needs) const
    {
        if ((needs & kNeedClouds) && cloudsV2Off())
            return tx(Txt::TipTierNeedClouds);
        if ((needs & kNeedAurora) && (debugDisableMask & 32u) != 0u)
            return tx(Txt::TipTierNeedAurora);
        if ((needs & kNeedEclipse) && potatoSkyOn())
            return tx(Txt::TipTierNeedEclipse);
        return nullptr;
    }
    static uint32_t defaultBookmarkNeeds(const std::string &id);
    // The lightning pass (and so the rain map, the flash list and the Sun profile in cv2FlashBuf) runs this frame.
    bool cloudsV2LightningActive() const { return !cloudsV2Off() && cloudFarBlend() < 0.999f; }
    float cloudFarBlend() const
    {
        if (cloudsV2Off())
            return 0.0f;   // the far layer is clouds too: off with them
        const double h = (double)obsEyeRadiusM() - satphot::kEarthRadiusM;
        const double a = (double)cv2FarLayerFromKm * 1000.0, b = std::max((double)cv2FarLayerFullKm * 1000.0, a + 1000.0);
        const double x = std::clamp((h - a) / (b - a), 0.0, 1.0);
        return (float)(x * x * (3.0 - 2.0 * x));
    }
    VkSampler cv2RepeatSampler = VK_NULL_HANDLE; // trilinear REPEAT (noise) — also the weather cube
    VkSampler cv2ClampSampler = VK_NULL_HANDLE;  // bilinear CLAMP (screen targets)
    VkBuffer cv2ParamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory cv2ParamsMem = VK_NULL_HANDLE;
    void *cv2ParamsMapped = nullptr;
    VkImage cv2NewImg = VK_NULL_HANDLE, cv2NewDepthImg = VK_NULL_HANDLE;
    VkImage cv2AgeImg = VK_NULL_HANDLE, cv2AgeHistImg = VK_NULL_HANDLE;   // the resolve's sample ages (R32F)
    VkDeviceMemory cv2AgeMem = VK_NULL_HANDLE, cv2AgeHistMem = VK_NULL_HANDLE;
    VkImageView cv2AgeView = VK_NULL_HANDLE, cv2AgeHistView = VK_NULL_HANDLE;
    VkImage cv2ResolvedImg = VK_NULL_HANDLE, cv2ResolvedDepthImg = VK_NULL_HANDLE, cv2HistoryImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2NewMem = VK_NULL_HANDLE, cv2NewDepthMem = VK_NULL_HANDLE;
    VkDeviceMemory cv2ResolvedMem = VK_NULL_HANDLE, cv2ResolvedDepthMem = VK_NULL_HANDLE, cv2HistoryMem = VK_NULL_HANDLE;
    VkImageView cv2NewView = VK_NULL_HANDLE, cv2NewDepthView = VK_NULL_HANDLE;
    VkImageView cv2ResolvedView = VK_NULL_HANDLE, cv2ResolvedDepthView = VK_NULL_HANDLE, cv2HistoryView = VK_NULL_HANDLE;
    uint32_t cv2HalfW = 0, cv2HalfH = 0, cv2QuarterW = 0, cv2QuarterH = 0;
    VkDescriptorSetLayout cv2MarchDescLayout = VK_NULL_HANDLE, cv2ResolveDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool cv2DescPool = VK_NULL_HANDLE;
    VkDescriptorSet cv2MarchDescSet = VK_NULL_HANDLE, cv2ResolveDescSet = VK_NULL_HANDLE;
    VkPipelineLayout cv2MarchPipeLayout = VK_NULL_HANDLE, cv2ResolvePipeLayout = VK_NULL_HANDLE;
    VkPipeline cv2MarchPipeline = VK_NULL_HANDLE, cv2ResolvePipeline = VK_NULL_HANDLE;
    VkPipeline cv2MarchPassBPipeline = VK_NULL_HANDLE;   // the adaptive rate's full-rate tiles (spec constant 2)
    // The same two without the haboob code (spec constant 3 = 0, perf pass 4): bound while no dust wall is in the grid.
    VkPipeline cv2MarchNoDustPipeline = VK_NULL_HANDLE, cv2MarchPassBNoDustPipeline = VK_NULL_HANDLE;
    VkPipeline cv2TilesPipeline = VK_NULL_HANDLE;        // ... and its tile classification (cloud_v2_tiles.comp)
    // Smooth tiles (2026-10-25, the full-res grid): pass F (spec constant 2 = 2, with and without the haboob code), the
    // tiles' classification (cloud_v2_tiles.comp kMode 1) and the sparse grid's fill (kMode 2).
    VkPipeline cv2MarchPassFPipeline = VK_NULL_HANDLE, cv2MarchPassFNoDustPipeline = VK_NULL_HANDLE;
    VkPipeline cv2TilesSmoothPipeline = VK_NULL_HANDLE, cv2TilesFillPipeline = VK_NULL_HANDLE;
    // Adaptive rate: the tiles pass A listed (header {pad, dispatch x, y, z}, 4096 tile ids, 4096 flags),
    // and pass B's full-rate targets.
    VkBuffer cv2TileBuf = VK_NULL_HANDLE;
    VkDeviceMemory cv2TileMem = VK_NULL_HANDLE;
    static constexpr uint32_t kCv2MaxTiles = 4096;
    VkImage cv2FullImg = VK_NULL_HANDLE, cv2FullDepthImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2FullMem = VK_NULL_HANDLE, cv2FullDepthMem = VK_NULL_HANDLE;
    VkImageView cv2FullView = VK_NULL_HANDLE, cv2FullDepthView = VK_NULL_HANDLE;
    // The far cloud layer (review 18, cloud_v2_far.comp): full swap resolution, rgb radiance + a transmittance,
    // march set binding 21, sky set binding 29 (imageLoad), GENERAL layout.
    VkImage cv2FarImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2FarMem = VK_NULL_HANDLE;
    VkImageView cv2FarView = VK_NULL_HANDLE;
    VkPipeline cv2FarPipeline = VK_NULL_HANDLE;
    // Lightning (include/cloud_lightning.glsl): cloud_v2_lightning.comp writes this frame's flashes
    // into cv2FlashBuf (host-visible: the host reads them back next frame for thunder), bound as
    // binding 12 of the march set and 21 of cloud_march's.
    VkPipeline cv2LightningPipeline = VK_NULL_HANDLE;
    VkPipeline cv2FroxPipeline = VK_NULL_HANDLE;   // the god rays' froxel volume (2026-10-22, cloud_v2_froxel.comp)
    // The aerial-perspective volume (2026-10-25, sky_ap.comp, include/sky_ap.glsl): the sky pass's atmosphere loop once per
    // froxel tile, its sums at kSkyApZ normalised depths; sat_sky.frag reads them (sky set binding 33) outside a band round
    // the horizon / the limb. The buffer (~66 MB) is made the first frame the volume runs (ensureSkyAp): until then binding
    // 33 holds the flash buffer as a placeholder (never read: cv2.skyAp.x is 0). Set 1 of its pipeline = the buffer.
    static constexpr uint32_t kSkyApX = 320, kSkyApY = 180, kSkyApZ = 48;   // == include/sky_ap.glsl
    static constexpr VkDeviceSize kSkyApBytes = (VkDeviceSize)kSkyApX * kSkyApY * kSkyApZ * 24;
    VkPipeline            skyApPipeline   = VK_NULL_HANDLE;
    VkPipelineLayout      skyApPipeLayout = VK_NULL_HANDLE;
    VkDescriptorSetLayout skyApSetLayout  = VK_NULL_HANDLE;
    VkDescriptorPool      skyApPool       = VK_NULL_HANDLE;
    VkDescriptorSet       skyApSet        = VK_NULL_HANDLE;
    VkBuffer              skyApBuf        = VK_NULL_HANDLE;
    VkDeviceMemory        skyApMem        = VK_NULL_HANDLE;
    // "Aerial perspective volume" (clouds_v2.sky_ap, slot 337): on by default; 0 = the per-pixel loop everywhere (the A/B).
    float cv2SkyAp = 1.0f;
    // "... horizon band (deg)" (clouds_v2.sky_ap_band_deg, slot 338): the band either side of the horizon / limb (and the
    // atmosphere's top seen from above it) where the sky pass keeps its own loop; at least 2.5 tiles.
    float cv2SkyApBandDeg = 2.0f;
    // The loop's step factor inside the band (clouds_v2.sky_ap_band_steps, no UI): it matches the volume's accuracy there.
    float cv2SkyApBandSteps = 1.0f;
    float photoSavedSkyAp = 1.0f;                  // the HQ photo takes the exact loop everywhere
    bool  skyApRanThisFrame = false;
    bool  ensureSkyAp(VulkanContext &ctx);         // makes the buffer + set the first time; false = not available
    VkPipeline cv2TshPipeline = VK_NULL_HANDLE;    // the terrain Sun-horizon map (2026-10-22, cloud_v2_terrsun.comp)
    float cv2TerrainGodrays = 1.0f;                // "Terrain god rays" (clouds_v2.terrain_godrays): the terrain shadows the air,
                                                   // the fog / dust through the Sun-horizon map (not the ground: it has its own)
    bool  cv2TshOnNow = false;                     // the map is baked this frame (fillCloudsV2Params)
    uint32_t cv2FroxFrame = 0;                     // the froxels' sample sequence index (cv2FroxJitter, cloud_lightning.glsl)
    // "Near cirrus stride" (2026-10-24, the perf pass; key near_cirrus_stride, slot 335): a pure thin-ice sample within a
    // 12-m footprint (the eye inside the high layer, where it is a soft haze) stands for this many view steps. 1 = off (the
    // look). From 12.4 km under an anvil (issue 20261023_230322): cloud march 34.6 -> 27.8 ms at 2, 25.0 at 4.
    float cv2NearCirrusStride = 4.0f;
    // "Smooth tiles at half density" (2026-10-25, perf-storm; keys smooth_tiles / smooth_tile_threshold, slots 339-340):
    // with full-resolution clouds, the 32x32 tiles whose last resolved frame varies by less than the threshold (display
    // levels / 255 over any 4x4 window; transmittance twice that) are marched at the half-res sparse grid's density and
    // filled (cloud_v2_tiles.comp). Full res cost 4-7 ms over half res in the user's storm views (1920x1009).
    float cv2SmoothTiles = 1.0f;
    // "Terrain shadows at half resolution" (2026-10-25, key clouds.terrain_shadow_half_res, slot 341): the terrain's soft sun
    // shadow marched once per half-res texel by scene_depth.comp (.g of the depth) and read by sat_sky.frag at the texels
    // matching its pixel's surface; 0 = marched per full-res pixel (1.7-2.5 ms of the sky pass on sunlit ground).
    float terrainShadowHalfRes = 1.0f;
    float cv2SmoothTileThresh = 30.0f;   // settled captures at the noise floor up to 40; motion +0.05 levels (tstab, 20-40)
    // "God ray reads (near)" (2026-10-24, perf pass 3; key godray_reads, slot 336): the sky pass's reads of the Sun shadow
    // (froxels or volume) per atmosphere step within 30 km, half that beyond. 8 = the look as built; 4 is ~0.4-1 ms of the
    // sky pass cheaper (1920x1009 High) with settled captures within noise: fewer reads per frame, so the sky TAA has more to
    // average in motion (the user's to judge; opt-in).
    float cv2GodrayReads = 8.0f;
    float cv2GodrayFroxels = 1.0f;                 // on by default since 2026-10-22: "godray_froxels": the sky pass reads the volume
    // Per-pass GPU time inside the cloud bucket (2026-10-21, .plans/CLOUD_REBUILD_PLAN.md S0): kCv2PassStamps timestamps
    // at fixed points of recordCloudsV2 + one after cloud_march.comp; a pass skipped this frame is a zero interval.
    // Harness `state` -> clouds_v2.pass_ms (eased over ~30 frames) / pass_ms_raw.
    static constexpr uint32_t kCv2PassStamps = 12;
    VkQueryPool cv2PassQuery = VK_NULL_HANDLE;
    uint32_t cv2PassStampNext = kCv2PassStamps;   // the next stamp this frame writes (cv2PassStampTo)
    bool  cv2PassStampsWrittenS[VulkanContext::kMaxFramesInFlight] = {}; // per slot: that frame wrote all of them
    float cv2PassMsRaw[kCv2PassStamps - 1] = {}, cv2PassMsEased[kCv2PassStamps - 1] = {};
    void cv2PassStampTo(VkCommandBuffer cmd, uint32_t id);   // writes stamps cv2PassStampNext .. id
    // The Sun / sky colour table (cloud_v2_sunlut.comp, include/cv2_sunlut.glsl): the Sun's colour and the zenith
    // sky's radiance by height and Sun cosine, a 2-layer array baked when the scattering gains change
    // (cv2SunLutKey); march set bindings 13 (storage, the bake) and 14 (sampled, the march). It took the
    // bindings of the light volume (cloud_v2_lightvol.comp, 2026-09-29: unbaked since the god rays moved to
    // the Sun shadow cascade, deleted 2026-10-13).
    VkPipeline cv2SunLutPipeline = VK_NULL_HANDLE;
    VkImage cv2SunLutImg = VK_NULL_HANDLE;
    VkDeviceMemory cv2SunLutMem = VK_NULL_HANDLE;
    VkImageView cv2SunLutView = VK_NULL_HANDLE;
    static constexpr uint32_t kCv2SunLutW = 256, kCv2SunLutH = 76;   // == cv2_sunlut.glsl
    float cv2SunLutKey[4] = {-1.0f, -1.0f, -1.0f, -1.0f};                    // the gains it was baked with
    VkBuffer cv2FlashBuf = VK_NULL_HANDLE;
    // The cloud density clipmap (2026-10-21, EXPERIMENT, include/cv2_cdc.glsl): device-local, march set binding 23,
    // baked by cloud_v2_cdc.comp. 8 levels x 128 x 128 x 64 uints.
    VkBuffer cv2CdcBuf = VK_NULL_HANDLE;
    VkDeviceMemory cv2CdcMem = VK_NULL_HANDLE;
    VkPipeline cv2CdcPipeline = VK_NULL_HANDLE;
    static constexpr uint32_t kCv2CdcCells = 8u * 128u * 128u * 64u;
    static constexpr uint32_t kCv2CdcListCap = 65536u;   // == kCv2CdcListCap (cv2_cdc.glsl)
    // The list header + list, then cells, S2's sums and sums of squares (cv2_cdc.glsl's cv2CdcData). Only the head is
    // allocated until the clipmap is first switched on (cv2EnsureCdcBuffer): ~100 MB of VRAM for an experiment that is off.
    static constexpr VkDeviceSize kCv2CdcHeadBytes = 16 + (VkDeviceSize)kCv2CdcListCap * 4;
    static constexpr VkDeviceSize kCv2CdcBufBytes = kCv2CdcHeadBytes + (VkDeviceSize)kCv2CdcCells * 12;
    bool cv2CdcBufFull = false;
    void cv2EnsureCdcBuffer(VulkanContext &ctx);
    VkPipeline cv2CdcBakePipeline = VK_NULL_HANDLE;   // pass 1 (cv2CdcPipeline is pass 0, the check)
    uint32_t cv2CdcEpoch = 0;
    VkDeviceMemory cv2FlashMem = VK_NULL_HANDLE;
    // The host's view of the flash buffer: a per-slot copy of its first kCv2FlashHeadBytes (the flash list, rain map
    // and light, tornado columns, eval counters), the values of ctx.completedFrame(); host writes go through
    // ctx.uploadPatch / uploadFill. cv2FlashRealMapped maps the GPU's buffer itself (init, and the harness after
    // draining the frames in flight).
    void *cv2FlashMapped = nullptr;
    void *cv2FlashRealMapped = nullptr;
    VkBuffer cv2FlashHeadBuf = VK_NULL_HANDLE;
    VkDeviceMemory cv2FlashHeadMem = VK_NULL_HANDLE;
    VkBuffer cv2BlueNoiseBuf = VK_NULL_HANDLE;   // 64x64 blue-noise tile (march binding 15)
    // The march's per-ray cloud opacity profile (2026-10-26): one uvec2 per 2x2 block of the cloud grid, march binding 22,
    // cloud_march.comp binding 26 (the unified volumetric composite). Sized with the cloud targets.
    VkBuffer       cv2ProfileBuf = VK_NULL_HANDLE;
    VkDeviceMemory cv2ProfileMem = VK_NULL_HANDLE;
    VkDeviceMemory cv2BlueNoiseMem = VK_NULL_HANDLE;
    static constexpr uint32_t kCv2FlashMax = 128;             // == kCv2FlashMax in cloud_lightning.glsl
    // + review 22's rain map (cloud_lightning.glsl): 4 workgroup maxima and 32 x 32 rain rates after the flashes.
    static constexpr VkDeviceSize kCv2RainMapOffset = 16 + kCv2FlashMax * 64 + 16 + 80;   // + review 22b's Sun profile
    static constexpr VkDeviceSize kCv2RainLightOffset = kCv2RainMapOffset + 1024 * 4;   // cv2RainKey .. cv2RainMisc
    static constexpr VkDeviceSize kCv2EvalCountOffset = kCv2RainLightOffset + 4 * 16;                 // cv2EvalCount
    static constexpr VkDeviceSize kCv2FlashHeadBytes = kCv2EvalCountOffset + 32 * 4;                  // what the host reads
    static constexpr uint32_t kCv2FroxX = 320, kCv2FroxY = 180, kCv2FroxZ = 64;   // == cloud_lightning.glsl (the god rays' froxels)
    static constexpr VkDeviceSize kCv2FroxBytes = (VkDeviceSize)kCv2FroxX * kCv2FroxY * kCv2FroxZ * 4;   // cv2GodFrox
    static constexpr VkDeviceSize kCv2StvOffset = kCv2EvalCountOffset + 32 * 4 + kCv2FroxBytes;      // cv2Stv (S3)
    static constexpr VkDeviceSize kCv2StvLevelBytes = 128ull * 128 * 48 * 4;                          // one level: 128^2 rays x 48 knots
    static constexpr VkDeviceSize kCv2TshOffset = kCv2StvOffset + 4 * kCv2StvLevelBytes;               // cv2TerrSun (terrain Sun horizon)
    static constexpr VkDeviceSize kCv2TshBytes = 4ull * 128 * 128 * 4;                                 // 4 levels of 128^2 rays, one float
    static constexpr VkDeviceSize kCv2SkyPsiOffset = kCv2TshOffset + kCv2TshBytes;                     // cv2SkyPsi (the sky's multiple scattering)
    static constexpr VkDeviceSize kCv2GroundSkyOffset = kCv2SkyPsiOffset + 32ull * 32 * 16;             // cv2GroundSky (the ground's sky light)
    static constexpr VkDeviceSize kCv2VtOffset = kCv2GroundSkyOffset + 64ull * 16 * 16;                // cv2VtHdr + cv2Vt (the cloud over the ground)
    static constexpr VkDeviceSize kCv2FlashBufBytes = kCv2VtOffset + 5ull * 4 * 16 + 2ull * 5 * 32 * 32 * 16 + 5ull * 32 * 32 * 4; // + ... cv2GroundSky, cv2VtHdr, cv2Vt, cv2VtUp, cv2VtStamp
    VkDescriptorSetLayout cloudMarchDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool cloudMarchDescPool = VK_NULL_HANDLE;
    VkDescriptorSet cloudMarchDescSet = VK_NULL_HANDLE;
    VkPipelineLayout cloudMarchPipeLayout = VK_NULL_HANDLE;
    VkPipeline cloudMarchPipeline = VK_NULL_HANDLE;
    // ── Shared scene depth (pipeline unification) ─────────────────────────────
    // Half ctx.swapExtent, R32_SFLOAT, written by scene_depth.comp at the very top of
    // recordCompute. Holds the LINEAR distance in metres along each view ray to the first
    // terrain/ocean surface, or kNoSurfaceT (1e30) for rays that reach space.
    //
    // Same sizing rule as cloudMarchTargetA/B — half of the SWAP extent, deliberately
    // independent of renderScale — so cloud_march.comp reads it 1:1 with texelFetch on its own
    // dispatch grid, while fragment consumers use gl_FragCoord.xy / pc.screenSizePx.
    //
    // R32, not R16: distances reach 3.6e6 m from LEO and half-float saturates at 65504. That
    // overflow is exactly the bug this buffer retires (see tEnterCombined) — do not shrink it.
    // Recreated in onResize alongside the cloud targets; see there for the descriptor patches.
    // 16 B, device-local: x = the observer's eye ground height with terrain detail (obsEffH),
    // computed once per frame by scene_depth.comp and read by sat_sky.frag (binding 26) instead of
    // evaluating the eight detail octaves again in every pixel (measured 1.5 ms at 1600x900).
    // y = the detailed ground alone (no height offset). Copied every frame into the host-mapped
    // terrainFrameRead*, which the harness reads (exact `observer agl=`, `state`'s ground).
    VkBuffer terrainFrameBuf = VK_NULL_HANDLE;
    VkDeviceMemory terrainFrameMem = VK_NULL_HANDLE;
    VkBuffer terrainFrameReadBuf = VK_NULL_HANDLE;
    VkDeviceMemory terrainFrameReadMem = VK_NULL_HANDLE;
    const float *terrainFrameMapped = nullptr; // last completed frame's terrainFrameBuf
    bool harnessGpuGroundValid() const { return terrainFrameMapped && (debugDisableMask & 1024u) == 0; }
    VkImage sceneDepthImg = VK_NULL_HANDLE;
    VkDeviceMemory sceneDepthMem = VK_NULL_HANDLE;
    VkImageView sceneDepthView = VK_NULL_HANDLE;
    // Quarter-resolution pre-pass of scene_depth.comp (2026-09-25): the same march at 1/16 of the
    // pixels, whose result seeds the half-resolution pass (as the half-res one seeds sat_sky.frag).
    // Two sets: sceneDepthQDescSet writes this image and samples the half-res one at binding 7 (not
    // read in quarter mode, but it must be a valid view in the layout it is in at that moment);
    // sceneDepthDescSet writes the half-res image and samples this one at binding 7.
    VkImage sceneDepthQImg = VK_NULL_HANDLE;
    VkDeviceMemory sceneDepthQMem = VK_NULL_HANDLE;
    VkImageView sceneDepthQView = VK_NULL_HANDLE;
    VkDescriptorSet sceneDepthQDescSet = VK_NULL_HANDLE;
    VkSampler sceneDepthSampler = VK_NULL_HANDLE; // resolution-independent; created once
    VkDescriptorSetLayout sceneDepthDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool sceneDepthDescPool = VK_NULL_HANDLE;
    VkDescriptorSet sceneDepthDescSet = VK_NULL_HANDLE;
    VkPipelineLayout sceneDepthPipeLayout = VK_NULL_HANDLE;
    VkPipeline sceneDepthPipeline = VK_NULL_HANDLE;

    // ── Flare/corona render-to-texture pipeline (flare architecture overhaul) ─────────────────
    // See FlareSourcePC's comment above for the three-stage design. flareExtent is a QUARTER of
    // ctx.swapExtent (one step smaller than scene_depth/cloud_march's half-res convention — this
    // buffer is deliberately going to be blurred, not sampled 1:1), independent of renderScale,
    // recreated in onResize alongside those.
    VkExtent2D flareExtent{};
    VkImage flareSourceImg = VK_NULL_HANDLE; // RGBA16F, COLOR_ATTACHMENT|STORAGE|SAMPLED
    VkDeviceMemory flareSourceMem = VK_NULL_HANDLE;
    VkImageView flareSourceView = VK_NULL_HANDLE;
    VkImage flareScratchImg = VK_NULL_HANDLE; // RGBA16F, STORAGE|SAMPLED — compute ping-pong + final composite source
    VkDeviceMemory flareScratchMem = VK_NULL_HANDLE;
    VkImageView flareScratchView = VK_NULL_HANDLE;
    VkSampler flareSampler = VK_NULL_HANDLE;             // shared by both images; resolution-independent, created once
    VkRenderPass flareSourceRenderPass = VK_NULL_HANDLE; // single color attachment, CLEAR, finalLayout=COLOR_ATTACHMENT_OPTIMAL
    VkFramebuffer flareSourceFramebuffer = VK_NULL_HANDLE;
    // Stage 1 (render): reuses the EXISTING descLayout/descSet (satVisibleBuf binding 1,
    // cloudTargetA/B bindings 5/6 are already there for sat_point.frag; binding 7 (sceneDepthTex)
    // is new — see createDescriptors()) — own pipeline layout only because the push constant type
    // (FlareSourcePC) differs from drawPipeLayout's SatDrawPC.
    VkPipelineLayout flareSourcePipeLayout = VK_NULL_HANDLE;
    VkPipeline flareSourcePipeline = VK_NULL_HANDLE;
    // Stage 2 (blur/streak): own tiny descriptor set — two STORAGE_IMAGE bindings, nothing this
    // codebase's existing sets already provide.
    VkDescriptorSetLayout flareBlurDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool flareBlurDescPool = VK_NULL_HANDLE;
    VkDescriptorSet flareBlurDescSet = VK_NULL_HANDLE;
    VkPipelineLayout flareBlurPipeLayout = VK_NULL_HANDLE;
    VkPipeline flareBlurPipeline = VK_NULL_HANDLE;
    // Stage 3 (composite): own tiny descriptor set — one COMBINED_IMAGE_SAMPLER binding
    // (flareScratchImg, sampled directly in VK_IMAGE_LAYOUT_GENERAL — legal, and this image is
    // small enough that skipping a layout-transition barrier costs nothing measurable). Targets
    // the MAIN render pass (ctx.renderPass), same render-pass-compatibility trick drawPipeline
    // already relies on to also work under ctx.renderPassLoad at renderScale<1.
    VkDescriptorSetLayout flareCompositeDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool flareCompositeDescPool = VK_NULL_HANDLE;
    VkDescriptorSet flareCompositeDescSet = VK_NULL_HANDLE;
    VkPipelineLayout flareCompositePipeLayout = VK_NULL_HANDLE;
    VkPipeline flareCompositePipeline = VK_NULL_HANDLE;
    VkPipeline glarePipeline = VK_NULL_HANDLE; // per-satellite glare sprites (main pass)
    // Mesh-glint glare (2026-09-24): glare_find.comp lists the concentrated glints mesh_bloom.frag put
    // in the flare source's alpha; glare_mesh.vert/.frag draws them like glare.vert's sprites.
    VkBuffer glintBuf = VK_NULL_HANDLE;
    VkDeviceMemory glintMem = VK_NULL_HANDLE;
    // Its host copy (2026-10-06): the harness reports the main view's glints (`state` -> glints), so a
    // glare's position can be checked against where the Sun's image is (the mirror shot of the release
    // cinematic). Copied at the end of recordCompute when glare_find ran; glintReadValid says whether the
    // copy is this frame's.
    VkBuffer glintReadBuf = VK_NULL_HANDLE;
    VkDeviceMemory glintReadMem = VK_NULL_HANDLE;
    void *glintReadMapped = nullptr;
    bool glintReadValid = false;     // the frame being recorded copies it
    bool glintReadValidSlot[VulkanContext::kMaxFramesInFlight] = {}; // per slot: glintReadMapped holds a list
    VkDescriptorSetLayout glareFindDescLayout = VK_NULL_HANDLE, glareMeshDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool glintDescPool = VK_NULL_HANDLE;
    VkDescriptorSet glareFindDescSet = VK_NULL_HANDLE, glareMeshDescSet = VK_NULL_HANDLE;
    VkPipelineLayout glareFindPipeLayout = VK_NULL_HANDLE, glareMeshPipeLayout = VK_NULL_HANDLE;
    VkPipeline glareFindPipeline = VK_NULL_HANDLE; // compute: not recreated on resize
    VkPipeline glareMeshPipeline = VK_NULL_HANDLE; // graphics: recreated with glarePipeline
    // Glare clusters (2026-10-20, include/glare.glsl, glare_cluster.comp): the screen grid every glaring
    // source is summed into (3 uints a tile, kGlareGridMaxTiles at most; binding 1 of glareMeshDescLayout),
    // the compute that fills it and the glare sprites' own layout (descLayout + the grid's set).
    static constexpr uint32_t kGlareGridMaxTiles = 1u << 18;
    VkBuffer glareGridBuf = VK_NULL_HANDLE;
    VkDeviceMemory glareGridMem = VK_NULL_HANDLE;
    VkPipelineLayout glareClusterPipeLayout = VK_NULL_HANDLE, glarePipeLayout = VK_NULL_HANDLE;
    VkPipeline glareClusterPipeline = VK_NULL_HANDLE; // compute: not recreated on resize
    GlarePC buildGlarePC(const VulkanContext &ctx, float eyeAdapt) const;
    float glareGridTilePx = 0.0f;                     // this frame's grid (0 = not filled: no clusters)
    uint32_t glareGridW = 0, glareGridH = 0;
    bool meshGlareListed = false;                  // glare_find.comp ran this frame (recordCompute)
    float meshGlareRangeM = 0.0f;                  // the nearest mesh drawn this frame (0 = none): its
                                                   // glint list's records carry it so a mesh's glare gets
                                                   // the proximity size a sprite's rangeM gives its own
                                                   // (updateMeshes -> recordCompute -> glare_find.comp)
    // Harness: the SELECTED satellite's mesh hand-off this frame (state's selection.mesh): its fade, the
    // sprite's keep, glare keep, on-screen size, and the ENU directions of its centre and its sprite.
    struct SelMeshDbg
    {
        bool  drawn = false;
        float fade = 0.0f, keep = 1.0f, glareKeep = 1.0f, px = 0.0f, glintW = 0.0f, glintShare = 0.0f;
        glm::vec3 centreEnu{0.0f}, spriteEnu{0.0f};
    } selMeshDbg;
    // Ocean-glint list (see GpuOceanGlintBuf) — device-local, zeroed every frame like glowBuf.
    VkBuffer oceanGlintBuf = VK_NULL_HANDLE;
    VkDeviceMemory oceanGlintMem = VK_NULL_HANDLE;
    // ... and its host-visible copy (8 KB, copied at the end of every recordCompute that ran
    // sat_flare.comp): every satellite at effectFlare >= 1 with its sky direction — a superset of the
    // ones that glare. The ambience's glare pad counts the ones glaring ON SCREEN from it (one frame
    // stale, like pickedVisibleBuf). computeAmbienceContext zeroes the count after reading, so a frame
    // whose flare pass was skipped reads an empty list rather than a stale one.
    VkBuffer glareReadBuf = VK_NULL_HANDLE;
    VkDeviceMemory glareReadMem = VK_NULL_HANDLE;
    void *glareReadMapped = nullptr;
    // User tunables (Settings > Display), persisted in settings.json. First-pass defaults —
    // expected to need retuning once seen in-app, same as every other constant this session.
    float flareGlowGain = 0.005f;       // post-composite overall multiplier — was 1.0f, 100x the
                                        // UI slider's actual [0, 0.01] range (SatelliteSimUI.cpp),
                                        // so a fresh settings.json (or any older save predating
                                        // this key) booted the flare glow fully maxed out
    // Glare sprites (glare.vert/.frag, glare_mesh.*, include/glare.glsl, 2026-09-24): a sharp core, a
    // tight halo and thin spikes, at full resolution over the broad bloom, for bright satellites and
    // bright mesh glints alike. Threshold is in the bloom's log response (0 at effectFlare 1 ~ mag 0.8,
    // 4 at its cap). Falloff: spike brightness along its length, (1 - r/length)^falloff - higher keeps
    // a crowd of glares from summing into a white patch the size of the sprite.
    // Defaults are the values tuned on screen for satellites seen from the ground at a distance (the
    // user's own settings.json, 2026-09-26: gain 0.684, size 29.79, threshold 1.765, falloff 4.678,
    // near gain 8.0, near range 21.86 km).
    float glareGain = 0.684f;
    float glareSizePx = 29.79f; // sprite radius per unit of response past the threshold
    float glareThreshold = 1.765f;
    float glareFalloff = 4.678f;
    float glareSpikes = 10.0f;
    // Glare clusters (2026-10-20, issue 20261019_222938): how strongly a glare in a crowd gives up its spikes to
    // the brighter / summed light around it (include/glare.glsl). 0 = every source draws its own star (as before).
    float glareClusterMerge = 1.0f;
    // The Milky Way's look (2026-10-28, GpuCloudParams::mwLook; Photometry tab, photometry.milky_way_*).
    float mwGainGround = 1.0f;
    float mwGainSpace = 2.5f;
    float mwContrastSpace = 1.25f;
    float mwNightVision = 0.5f;
    float mwStarClouds = 0.7f;
    float mwZoomDetail = 0.6f;
    bool mwZoomOn = true;   // photometry.milky_way_zoom (no UI): false = the old single fetch at every field of view (A/B)
    // The zoomed star clouds' tunables (2026-10-28, issue 20261028_191142; GpuCloudParams::mwStar0-2, Photometry tab,
    // photometry.milky_way_star_*). Defaults = the look they replaced (constants in sat_sky.frag's mwZoomDetail).
    float mwZoomFrom = 0.6f;        // the zoomed view starts at this input pixel / panorama texel ...
    float mwZoomFull = 0.3f;        // ... and is complete at this one
    float mwStarFill = 0.9f;        // share of lattice cells holding a point
    float mwStarSize = 0.9f;        // a point's sigma (input px)
    float mwStarCellPx = 4.0f;      // lattice cell (input px): smaller = denser, fainter points
    float mwStarLumExp = 0.65f;     // the points' brightness distribution: w = (1 - a) u^-a (0 = all equal)
    float mwStarFaintLo = 0.003f;   // the band's glow below which the sky stays smooth (no points) ...
    float mwStarFaintHi = 0.02f;    // ... and above which it has its full share of points
    float mwStarNebula = 1.0f;      // how fully the magenta emission nebulae stay a smooth glow (0 = points there too)
    // Seeing (2026-10-28, Photometry tab): "Star twinkle rate" x the twinkle's old frequencies, on REAL time
    // (photometry.star_twinkle_rate, issue 20261028_191408); "Turbulence warp" x the angle-of-arrival rms the whole picture
    // is displaced by (photometry.turbulence_warp, issue 20261028_193115; 0 = no image motion at all: sat_sky.frag's own
    // Sun / Moon disc jitter stays off, refrSun.z = 0).
    float starTwinkleRate = 0.3f;
    float turbWarpGain = 1.0f;
    // ...and they stay exactly that for a source at `glareNearRangeKm` or beyond: proximity scales the
    // sprite's SIZE alone, so a satellite up in the sky is untouched while one resolved metres away (the
    // 3D viewer) or closing in on a zoomed 3D mesh spreads a much wider glare than a distant point
    // (glare.glsl's glareNearScale: 1 → glareNearGain over 0..nearRangeM, smoothstepped).
    float glareNearGain = 8.0f;      // the size multiplier at zero range (1 = off)
    float glareNearRangeKm = 21.86f; // where the multiplier is 1 again: a satellite seen from the ground
                                     // is never this close, so the tuned look above is intact
    float flareStreakGain = 0.35f;      // per-tap streak/godray strength (flare_blur.comp mode=2)
    float sunFlareRefIntensity = 40.0f; // fixed reference brightness for the sun's virtual point
                                        // in the flare-source buffer — NOT a slider (kept small in
                                        // scope this round); tune in code if the sun's godray/glow
                                        // contribution looks under- or over-powered relative to
                                        // satellites once seen in-app.

    // ── Long-exposure trail pipeline (fun side feature) ──────────────────────────────────────
    // Persistent, real-time-decayed accumulator for satellite/star/planet point splats — leaves
    // fading trails behind them, most dramatic at high timeScaleIdx. Same three-stage shape as the
    // flare pipeline above (fade/splat entirely inside recordCompute(), composite draw appended in
    // recordDraw()), reusing sat_point.vert/frag and star_point.vert/frag UNCHANGED against a new
    // render pass — no new pipeline LAYOUTS needed for the splat stage, only new VkPipeline objects
    // targeting trailAccumRenderPass instead of ctx.renderPass.
    //
    // Decay is REAL-TIME (wall-clock), not sim-time: a fixed real-world persistence window, so
    // cranking timeScaleIdx up automatically makes trails longer/showier (more simulated motion
    // fits inside the same real-world window) with no retuning. Runs every frame trails are
    // enabled, even while timePaused — matches a real camera shutter aging even when nothing new
    // is landing on it.
    //
    // Splats are drawn from the LIVE satVisibleBuf/starBuf/planetBuf this same frame's sat_flare.comp/
    // updateStars()/updatePlanets() already computed — one sample per real display frame. At very
    // high timeScaleIdx (where a satellite/star can move a large angle between two real frames)
    // this reads as short dashes rather than one continuous streak rather than smoothly interpolated
    // motion — accepted for this pass (temporal supersampling to resample positions several times
    // per real frame, reusing the fact that this sim's orbital/rotational math is a pure function of
    // absolute time, is a natural follow-up but not implemented here).
    VkExtent2D trailAccumExtent{};
    VkImage trailAccumImg = VK_NULL_HANDLE; // RGBA16F, COLOR_ATTACHMENT|STORAGE|SAMPLED, full ctx.swapExtent
                                            // (NOT quarter-res like flareExtent — downscaling would
                                            // blur thin satellite/star streaks; matches the
                                            // "satellites/stars/UI always render at native
                                            // resolution" Resolution Scaling design goal)
    VkDeviceMemory trailAccumMem = VK_NULL_HANDLE;
    VkImageView trailAccumView = VK_NULL_HANDLE;
    // trailSampler intentionally absent — reuses flareSampler (LINEAR/CLAMP_TO_EDGE, resolution-
    // independent) for the composite's sampled read.
    VkRenderPass trailAccumRenderPass = VK_NULL_HANDLE; // 1 color attachment, LOAD_OP_LOAD (never
                                                        // cleared by the render pass itself —
                                                        // persistence is the whole point),
                                                        // VK_IMAGE_LAYOUT_GENERAL throughout (same
                                                        // "sample directly in GENERAL" trick
                                                        // flareScratchImg already uses)
    VkFramebuffer trailAccumFramebuffer = VK_NULL_HANDLE;
    // Stage A (fade, compute): multiplies trailAccumImg in place by a real-time exponential decay.
    VkDescriptorSetLayout trailFadeDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool trailFadeDescPool = VK_NULL_HANDLE;
    VkDescriptorSet trailFadeDescSet = VK_NULL_HANDLE;
    VkPipelineLayout trailFadePipeLayout = VK_NULL_HANDLE;
    VkPipeline trailFadePipeline = VK_NULL_HANDLE;
    // Stage B (splat, graphics): reuses drawPipeLayout/descSet and starPipeLayout/starDescSet/
    // planetDescSet completely unchanged (same SatDrawPC push-constant range, same bound buffers) —
    // only new VkPipeline objects, targeting trailAccumRenderPass with depthTestEnable=false (this
    // offscreen target has no depth attachment) instead of ctx.renderPass.
    VkPipeline trailSatPipeline = VK_NULL_HANDLE;
    VkPipeline trailStarPipeline = VK_NULL_HANDLE; // also used for the planet trail draw, exactly
                                                   // like the live starPipeline is
    // Stage C (composite, graphics): additive fullscreen-triangle draw into ctx.renderPass, appended
    // after the flare composite draw in recordDraw() (order between the two doesn't matter — both
    // are ONE/ONE additive blending, which is commutative).
    VkDescriptorSetLayout trailCompositeDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool trailCompositeDescPool = VK_NULL_HANDLE;
    VkDescriptorSet trailCompositeDescSet = VK_NULL_HANDLE;
    VkPipelineLayout trailCompositePipeLayout = VK_NULL_HANDLE;
    VkPipeline trailCompositePipeline = VK_NULL_HANDLE;
    // User tunables (Settings > Display / Photometry), persisted in settings.json.
    bool trailEnabled = false;
    float trailDecaySeconds = 4.0f; // real-world exponential decay time constant
    float trailCompositeGain = 1.0f;
    // One-shot flag consumed at the top of the trail block in recordCompute(): set on toggle-on,
    // resize, and the manual "Clear Trail" button. NOT set on timescale/observer/pause changes —
    // trails are meant to persist through those.
    bool trailClearPending = true;

    // ── Per-beam cloud occlusion march (2026-08-09) ────────────────────────────
    // Replaces the per-target beam_cloud_block.comp pass (retired) — own small descriptor set/
    // pipeline, same shape (SSBO + 3 cloud textures + CloudParams UBO), but binding 0 is
    // reflectBeamsBuf itself (read+write) instead of a separate target buffer + output buffer, and
    // it's dispatched over BEAM_MAX_ACTIVE (2048) threads instead of 201 targets. See
    // beam_self_march.comp's header for the full design.
    VkDescriptorSetLayout beamSelfMarchDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool beamSelfMarchDescPool = VK_NULL_HANDLE;
    VkDescriptorSet beamSelfMarchDescSet = VK_NULL_HANDLE;
    VkPipelineLayout beamSelfMarchPipeLayout = VK_NULL_HANDLE;
    VkPipeline beamSelfMarchPipeline = VK_NULL_HANDLE;
    // Earth elevation texture (binding 5): 21600×10800 R8_UNORM land-elevation DEM.
    // Pixel p → elevation_m = p * 8848; ocean stored as 0. Terrain shell = R_EARTH + 9000 m.
    VkImage earthElevImg = VK_NULL_HANDLE;
    VkDeviceMemory earthElevMem = VK_NULL_HANDLE;
    VkImageView earthElevView = VK_NULL_HANDLE;
    VkSampler earthElevSampler = VK_NULL_HANDLE;
    uint32_t earthElevMips = 1;
    int earthElevW = 0, earthElevH = 0; // the DEM's size (terrainObsTexel)
    // CPU-side downsampled elevation for observer height lookup (2160×1080, ~18km/px)
    std::vector<uint8_t> earthElevCpu;
    int earthElevCpuW = 0, earthElevCpuH = 0;
    // CPU-side downsampled night-lights luminance for observer light-pollution lookup
    // (2160×1080, ~18km/px) — single byte per texel, precomputed Rec.709 luminance. Box-filtered
    // (not nearest-neighbor) so it doesn't itself alias before anything samples it.
    std::vector<uint8_t> earthNightCpu;
    int earthNightCpuW = 0, earthNightCpuH = 0;
    // Full-resolution (14999x7500, ~2.7 km/px) ocean mask from the DEM, one bit per texel (DEM
    // pixel <= 15/255, the ocean baseline — lakes are land here), ~14 MB. The ambience's
    // ocean/beach drivers; the 18 km/px earthElevCpu turns every coastal town into half ocean.
    std::vector<uint64_t> oceanMaskCpu;
    int oceanMaskW = 0, oceanMaskH = 0;
    // Box-filtered 2048x1024 RGB copy of the day map: the ambience's land cover (veg / forest /
    // desert / ice drivers), classified from the same colours the ground is drawn with.
    std::vector<uint8_t> earthDayCpu;
    int earthDayCpuW = 0, earthDayCpuH = 0;
    // The land-cover map for the weather hazards (Biome.h: wildfires' fuel, haboobs' dust), built at load from the
    // day map, the elevation and the ocean mask.
    wx::BiomeMap biomeMap;
    // Box-filtered 1024x512 byte copy of the CLOUD MAP (~39 km/px), the 2D coverage the clouds are
    // drawn from: the ambience's `cloud` driver, i.e. how cloudy the sky is over the listener. The
    // ground wind bed follows it, so it is windy in some places and a light breeze in others — the
    // same weather the eye sees (the driver samples it with the same `cloudPhase` drift the surface
    // overlay and the volumetric march use).
    std::vector<uint8_t> earthCloudsCpu;
    int earthCloudsCpuW = 0, earthCloudsCpuH = 0;

    // ── UI visibility & settings ──────────────────────────────────────────────
    // UC3: persisted (see loadSettings/saveSettings) so this only auto-plays on first run —
    // defaults true (compiled-in default), and the "no settings.json at all" branch of
    // loadSettings() never touches it, so a genuine first run always shows it; any existing
    // settings.json (even pre-UC3) loads a persisted value that defaults to false so upgrading
    // users don't suddenly get a cinematic that didn't exist before. Replayable via the Display
    // tab's "Replay Intro" button, which just sets this back to true.
    bool showIntro = true;
    bool uiVisible = true;
    bool iconsLoaded = false;
    // UI scale (2026-10-21, low resolutions): `uiScaleUser` is the player's setting (display.ui_scale, "Text scale",
    // 0.75-2.0); `uiScale` is what every builder reads = uiScaleUser x uiAutoScaleFactor(window height) when
    // `uiScaleAuto` (display.ui_auto_scale, default on): 1 at 1000 px and taller (a maximized window on a 1080p desktop is ~1009), down to 0.7 at 700 px.
    // Set once per frame at the top of buildUI (updateUiScale). fs() never lets the auto factor take text below kAutoFontFloorPx (12 px; 16 before the SDF font).
    float uiScale = 1.5f;     // effective (per frame)
    float uiScaleUser = 1.5f; // the setting
    bool uiScaleAuto = true;
    static float uiAutoScaleFactor(float screenH) { return screenH > 0.0f ? std::clamp(screenH / 1000.0f, 0.7f, 1.0f) : 1.0f; }
    void updateUiScale(float screenH) { uiScale = uiScaleUser * (uiScaleAuto ? uiAutoScaleFactor(screenH) : 1.0f); }
    // UI colour theme (UIPalette.h: 0 Default, 1 Navy, 2 Night red, 3 Midnight = the default), display.ui_theme. buildUI applies it
    // (applyUiTheme + the renderer's night filter) whenever it differs from what is applied.
    int uiTheme = 3; // Midnight (the default since 2026-10-15)
    // Window opacity (2026-10-20, display.window_opacity, Display tab): the windows' panel alpha. 1 = opaque: at the
    // themes' 245/255 the text of a window under another showed through it (bright text at ~4%).
    float windowOpacity = 1.0f;
    void syncUiTheme();
    float masterVol_ = 0.4f; // mirrors AudioSystem default (display fallback)
    float musicVol_ = 0.7f;
    float sfxVol_ = 1.0f;
    float ambienceVol_ = 0.7f;
    // Sound tab (advanced) — persisted under "audio". Defaults are the user's own settings.json
    // (2026-09-26), rounded to 2 s.f. per the cloud/photometry rule: fades 0.27/0.32, root 41 Hz,
    // group gains (Ambience::groupNames() order) 0.93 / 1.0 / 1.0 / 1.6 / 1.2 / 2.0.
    float musicGapS = 30.0f;          // silence between tracks, for the ambience
    float ambFadeInScale = 0.27f;     // multiplies every layer's fade_in_s
    float ambFadeOutScale = 0.32f;    // ... and fade_out_s
    float musicAltFade = 1.0f;        // music bus multiplier from altitude (updateAmbience)
    float ambRootHz = 41.0f;          // the tonal root every pitched ambience voice is a multiple of
    float ambGroupGain[6] = {0.93f, 1.0f, 1.0f, 1.6f, 1.2f, 2.0f}; // Ambience::groupNames() order
    // ── Ambient sound (SatelliteSimAmbience.cpp, the layer table in Ambience.h) ─────────────────
    // The sim's side: the context drivers, computed each frame from the CAMERA (in follow mode the
    // camera, not the parked telescope — ambience is what the listener hears).
    Ambience *ambience_ = nullptr;
    // Thunder (thunderStep, 2026-10-10): every flash's channel is a SOUND SOURCE whose shell expands at 343 m/s
    // in sim time; each frame the pieces of channel the shell carries across the listener — where the listener
    // is NOW — are what is heard, streamed to the "thunder" layer's synth. So the roll follows the listener's
    // motion (flying toward a storm it comes sooner and compressed; outrunning the sound, it never comes) and
    // the time scale; paused, nothing arrives.
    struct ThunderSource
    {
        uint32_t id = 0;
        double t0 = 0.0;                 // sim time the return stroke sounded
        glm::dvec3 origin{0.0};          // ECEF, m
        float extentM = 0.0f, energy = 1.0f;
        std::vector<glm::vec4> pts;      // xyz a channel piece's midpoint from the origin (ECEF axes, m), w its power weight
        std::vector<glm::vec4> dirh;     // xyz its direction, w its half-length (m)
        bool heard = false;
    };
    static constexpr int kThunderVoices = 8;      // == ThunderSynth::kVoices
    std::vector<ThunderSource> thunderSources_;
    struct ThunderVoice { uint32_t id = 0; bool used = false; float level = 0.0f; double lastSoundT = 0.0; } thunderVoice_[kThunderVoices];
    double thunderPrevS_ = 0.0, thunderRealT_ = 0.0;
    glm::dvec3 thunderPrevEye_{0.0};
    bool thunderPrevValid_ = false;
    float thunderHoldS_ = 0.0f;                   // the `thunder` driver: arriving or sounding within the last seconds
    float thunderLevelNow_ = 0.0f;                // the loudest voice's level this frame (harness `lightning`)
    uint32_t thunderRolls_ = 0;                   // flashes heard so far (harness `lightning`)
    void updateThunder(float dt);
    void thunderStep(double nowS, const glm::dvec3 &eye, float realDt);
    void thunderRecordTick(float dt);             // harness `audio record`: the render's own clock carries the sound
    double thunderRecordS_ = 0.0;
    bool thunderRecording_ = false;
    struct AmbienceDrivers
    {
        int altM = -1, aglM = -1, groundM = -1, latDeg = -1, lonDeg = -1, sunElDeg = -1, oceanNear = -1, oceanWide = -1,
            urban = -1, beam = -1, aurora = -1, wind = -1, cloud = -1, timeScale = -1, following = -1, intro = -1, veg = -1,
            forest = -1, desert = -1, ice = -1, speed = -1, eas = -1, glareN = -1, glareSum = -1, beamSite = -1, musicGap = -1, rain = -1,
            thunder = -1, rocketMag = -1, stageMag = -1, fireballMag = -1, dustMag = -1, gust = -1, surfaceWind = -1,
            rainDriven = -1, tornado = -1;
    } ambD_;
    // The upwell's other sources (2026-10-21, issue 20261021_002542 "Upwells": the stem should rise during launches,
    // near firing engines, by collision dust and re-entries). The code that DRAWS each source lists it here every frame
    // (ambUpwellSource: its direction from the eye in ENU, range, and apparent magnitude as seen from the camera, with
    // no daylight suppression); computeAmbienceContext weights each by being on screen (the glare's soft frame edge)
    // or near (a per-kind range), sums the fluxes per kind and publishes them as equivalent magnitudes:
    //   rocket_mag    burning engines (ascent, boostback, entry and landing burns; the flame's magnitude at 100 km)
    //   stage_mag     sunlit rocket stages and fairing halves coasting (the bodies' magnitudes, before the mesh fade)
    //   fireball_mag  re-entry fireballs (every glowing fragment) and the ship / capsule entry glows
    //   dust_mag      Kessler collision dust clouds (each tube piece's light, Earth-shadowed at its centre)
    // 30 = nothing. The table's "music_upwell" maps each to a gain. Consumed (cleared) by the ambience each frame.
    struct AmbUpSrc
    {
        glm::vec3 dir; // ENU unit vector from the eye
        float rangeM;  // to the source (dust: to the piece's near edge)
        float mag;     // apparent magnitude at the camera
        uint8_t kind;  // 0 rocket, 1 stage, 2 fireball, 3 dust
    };
    std::vector<AmbUpSrc> ambUpSrc_;
    void ambUpwellSource(uint8_t kind, const glm::dvec3 &relEnu, double rangeM, double mag)
    {
        if (ambUpSrc_.size() < 32768 && rangeM > 0.0 && mag < 25.0)
            ambUpSrc_.push_back({glm::vec3(relEnu / std::max(glm::length(relEnu), 1e-9)), (float)rangeM, (float)mag, kind});
    }
    float ambUpFlux_[4] = {};    // eased weighted flux per kind (10^(-0.4 m) units)
    float ambUpFluxRaw_[4] = {}; // this frame's
    int ambUpCount_[4] = {};     // sources weighed in this frame
    glm::dvec3 ambPrevCamEcef{0.0}; // wind rush: the camera's last position (ECEF, m)
    bool ambPrevValid = false;
    float ambSpeedEased = 0.0f;     // m/s, eased over ~0.3 s
    float ambSpeedRaw[3] = {};      // the last three raw speeds (median-of-3: one-frame spikes out)
    float ambCloudEased = -1.0f;    // sky coverage over the listener, eased over ~1.5 s (-1 = unset)
    float ambRainEased = 0.0f;      // the rain rate at the eye (cloud_v2_march.comp -> terrainFrame.w), eased ~2 s
    // The glare pad (layer beam_glare, synth "pad"): the flares GLARING ON SCREEN, from the host copy
    // of the bright-flare list (glareReadBuf) — `glare_n` is how many (each counts in full once it is
    // 0.4 past the glare threshold, partially below, so the count never steps), `glare_sum` their
    // combined strength past the threshold (the glare sprite's own `s`). Eased: up in ~0.1 s (a flare
    // is fleeting), down in ~0.6 s. Raw values and the count of glaring sources are for the Beams
    // tab's readout.
    float ambGlareN = 0.0f, ambGlareSum = 0.0f;
    float ambGlareNRaw = 0.0f, ambGlareSumRaw = 0.0f;
    int ambGlareCount = 0;
    // The beam-site hum (layer beam_site_hum, synth "bass"): converged beams on sites near the
    // listener, accumulated in the beam readback loop (SatelliteSim.cpp), eased ~1.5 s here.
    float ambBeamSiteRaw = 0.0f;
    float ambBeamSite = -1.0f; // -1 = unset
    // Music vs the beam sound (2026-09-27, the user's rule): while a track plays, glare raises the
    // track's UPWELL stem (ambUpwellGain -> AudioSystem::setUpwellGain, mapped by ambience.json's
    // "music_upwell"); the beam pad and site hum speak only between tracks, under `music_gap` — a
    // triangle over the silent gap (0 at its start, 1 halfway, 0 at its end), 1 when no music is
    // audible (off, paused, muted, faded out in high orbit), slewed so a pause never steps it.
    float ambMusicGap = -1.0f;  // -1 = unset
    float ambIntroFade = -1.0f; // ambience bus multiplier: 0.5 during the intro (Gravity Wave leads); -1 = unset
    float ambUpwellGain = 0.0f;

    // ── Tonality: the ambience's key follows the soundtrack (MusicAnalysis.h) ─────────────────────
    // Every {"root": k} voice and the pad's masks come from here. With ambRootFollowMusic on, the root
    // is the playing track's tonic (at its live tuning-curve pitch), and it glides to the next track's
    // tonic through the silent gap between them; off (or before the analysis is ready) it is the
    // Sound tab's ambRootHz with the table's fallback scale.
    music::Library *musicLib_ = nullptr;
    std::vector<std::string> musicTracks_; // the playlist, as handed to AudioSystem (and the library)
    bool ambRootFollowMusic = true;        // Sound tab (advanced), persisted
    struct TonalState
    {
        double rootLog2 = -1.0;  // log2(Hz) as played; < 0 = unset
        double targetLog2 = 0.0; // where it is gliding to
        int octave = 0;          // octave shift applied to the current source (chosen when it changes)
        std::string sourceKey;   // what the target is derived from ("t:1", "g:1>2", "manual")
        uint16_t scale = 0x5AD, chord = 0x81; // relative to the root
        std::string source, key, chordName, scaleText;
        float tuningCents = 0.0f;
    } tonal_;
    void startMusicAnalysis();
    void updateTonality(float dt);
    nlohmann::json tonalityJson() const;
    std::string tonalityLine() const; // Sound tab readout
    std::vector<std::vector<int>> ambGroupConsts_; // per shell group: constellation indices it matches
    void initAmbience();
    void prepareUI(UIRenderer &ui) override; // (2026-10-21) the icon atlas, at load
    void loadUiIcons(UIRenderer &ui);
    void updateAmbience(float dt);
    void computeAmbienceContext(float dt);
    bool oceanAt(double latRad, double lonRad) const;
    // Whether the DEM has any land (oceanMaskCpu: a texel above the sea baseline) within radiusM of a point (2026-10-23:
    // the terrain Sun-horizon map is not baked over open ocean). A box in lat / lon around the circle, word-wise.
    bool landWithin(double latRad, double lonRad, double radiusM) const;
    // Whether earthElevCpu (18 km/px, nearest-sampled: a peak narrower than a texel can be missed) has terrain of minM
    // or more in the terrain Sun-horizon map's reach: +-acrossM across the Sun (axis ga), from backM away from the Sun
    // to aheadM toward it (axis s), about the eye's ground point up (all ECEF unit vectors).
    bool terrainInSunBox(const glm::dvec3 &up, const glm::dvec3 &s, const glm::dvec3 &ga, double acrossM, double backM,
                         double aheadM, float minM) const;
    glm::dvec3 tshLandCheckSun_{0.0};
    glm::dvec3 tshPrevUp_{0.0};
    bool tshLandNear_ = true;                 // landWithin around the eye, re-checked past ~25 km of motion
    glm::dvec3 tshLandCheckUp_{0.0};
    float oceanFraction(double latRad, double lonRad, double radiusM, int rings) const;
    std::string ambienceNowPlaying() const; // Sound tab readout
    // ── Photometry tuning (synced to SatFlarePC each frame) ───────────────────
    // Defaults below are the user-tuned values baked in from settings.json rather than placeholder
    // guesses — re-synced 2026-08-10 (extinctionCoeff, lightPollutionGain, and the Milky Way
    // pollution-response block moved noticeably; the rest were already current), then rounded to
    // 2 significant figures 2026-08-15 (see the cloud block's note below — same pass, same rule).
    float brightnessScale = 1.01f;
    float daySuppression = 570.0f;
    float mirrorBoost = 1000.0f;
    float visThresh = 0.0001f;
    float highlightFlare = 0.01f;
    float moonSuppression = 6.6f;    // sky background suppression from moonlight (mirrors daySuppression,
                                     // user-tuned value — moon is ~14 magnitudes dimmer than the sun)
    float extinctionCoeff = 0.18f;  // sea-level ZENITH extinction, magnitudes (atmosphere.glsl);
                                     // ~0.2-0.3 is typical clear-sky V band. One line-of-sight
                                     // formula for satellites, stars, planets and the Milky Way.
    // Shared point-source model (point_style.glsl, GpuPointStyle): one magnitude → appearance
    // mapping for satellites, stars and planets, so equal magnitudes look equal. Defaults
    // approximate the stars' previous look at magnitudes 0 and 6.
    float pointRefMag = 1.5f;        // "Point peak mag": magnitude whose core peaks at 1
    float pointGamma = 0.8f;         // "Point contrast": drawn flux ∝ flux^gamma (1 = linear)
    float pointLimitMag = 8.0f;      // "Point limit mag": faintest drawn
    float pointSigmaPx = 0.45f;      // "Point size (px)": base PSF sigma
    float pointSigmaMaxPx = 6.0f;    // "Point max size (px)": PSF growth cap
    // Zoom optics (Phase 4): zooming is magnification M = tan(35°)/tan(fov/2) (1 at the default 70°
    // FOV and wider), and the view is modelled as an instrument whose aperture grows with it at the
    // eye's 7 mm exit pupil — D = min(7 mm · M, zoomApertureMaxMm) — like binoculars/telescopes.
    // A point source collects D² more light, so it draws 5·log10(D / 7 mm) magnitudes brighter; the
    // sky background's surface brightness does not change (fixed exit pupil), so zoomed-in points
    // stand out against a daytime or light-polluted sky as they do through a real telescope. Applied
    // by shifting the point model's reference and limit magnitudes (pointEff*), so every consumer —
    // satellites, stars, planets, GPU and CPU — gets it with no shader change. Magnitude READOUTS
    // stay physical (naked-eye). 7 mm = off. Resolved meshes (Phase 4c) must use the same D: their
    // per-pixel brightness stays constant while D grows with M and dims once D is capped.
    float zoomApertureMaxMm = 200.0f; // "Zoom aperture max (mm)" — 7 = zoom does not gather light
    static constexpr float kEyePupilMm = 7.0f;
    float zoomMagnification() const
    {
        return std::max(1.0f, tanf(glm::radians(35.0f)) / tanf(glm::radians(camera.fovYDeg) * 0.5f));
    }
    float opticsGainMag() const
    {
        float d = std::min(kEyePupilMm * zoomMagnification(), std::max(zoomApertureMaxMm, kEyePupilMm));
        return 5.0f * log10f(d / kEyePupilMm);
    }
    float pointEffRefMag() const { return pointRefMag + opticsGainMag() + exposureGainMag() - satAdaptMag; }
    float pointEffLimitMag() const { return pointLimitMag + opticsGainMag() + exposureGainMag() - satAdaptMag; }
    // ── Satellite display (2026-10-07) ──────────────────────────────────────────────────────────
    // sat_flare.comp writes a DISPLAYED effectFlare (point_style.glsl): x 10^(0.4 satDisplayGainMag()) — the
    // exposure and the adaptation below, which the points' reference magnitude also carries — and, for a satellite
    // whose model resolves, faded into the model's own surface brightness (satResolvedStrength).
    float satResolvedStrength = 1.0f; // "Resolved satellites look like their model" (photometry.sat_resolved)
    // Adaptation: bright RESOLVED satellites in view (the meshes' light in the sky exposure's units, as a share of
    // the frame) raise the points' limiting magnitude, as an eye or camera adapts to a sunlit object: a StarMind
    // cluster 500 m away hides the mag-5 satellites behind it. "Satellite adaptation" (photometry.sat_adaptation).
    float satAdaptationStrength = 1.0f;
    float satAdaptMag = 0.0f;        // eased (magnitudes the points lose)
    float satAdaptLum = 0.0f;        // last frame's resolved-satellite light / frame (harness)
    float satAdaptFrameMag = 0.0f;   // above the air: the frame's own displayed luminance's share (harness)
    float formationMergeStrength = 1.0f; // "Merge unresolved formations" (photometry.formation_merge)
    float satDisplayGainMag() const { return exposureGainMag() - satAdaptMag; }
    // sat_orbit.comp's formation merge for one member (the GPU-parity check): flux factor.
    double formationMergeFactor(const SatOrbit &o, int satIdx, double rangeM) const;
    // A formation member's whole cluster, above the air (CPU photometry of every member, summed): +inf when dark
    // or not in a formation; count = its members.
    double formationMagnitude(int satIdx, const SatPhotInputs &in, double tJ2000, int &count) const;
    // CPU mirror of point_style.glsl's pointPsf(): x = peak, y = sigma (px).
    glm::vec2 pointPsf(float mag) const
    {
        const float k = 1.3287712f; // 0.4 * log2(10)
        const float refMag = pointEffRefMag(), limitMag = pointEffLimitMag();
        float d = exp2f(-k * pointGamma * (mag - refMag));
        float dl = exp2f(-k * pointGamma * (limitMag - refMag));
        d = std::max(d - dl, 0.0f);
        if (d <= 1.0f)
            return {d, pointSigmaPx};
        float s = std::min(pointSigmaPx * sqrtf(d), pointSigmaMaxPx);
        return {std::min(d * pointSigmaPx * pointSigmaPx / (s * s), 1.0f), s}; // point_style.glsl: stops at white
    }
    static float pointSpriteSizePx(float sigma) { return 2.0f * (3.0f * sigma + 1.0f); }
    void uploadPointStyle(); // → pointStyleBuf (every frame, and once at creation)
    // Ground-directed flare mitigation attitude control for space datacenters (AttitudeMode::
    // SunTrackingTilted). A single global operator-policy knob rather than a per-satellite-type
    // JSON constant — real operators would tune one mitigation posture across a fleet, and it
    // needs to be live-tunable to compare against the unmitigated (0°) baseline. Pitches the
    // sun-tracking normal away from nadir toward zenith by this many degrees; power output drops
    // by cos(tiltDeg) (Lambert's cosine law — see the enum comment and sat_orbit.comp's
    // computeNormal()), read back for the selection-panel "Power output" line in
    // formatSelectedSatInfo(). 0° = no mitigation, bit-identical to plain SunTracking.
    float flareMitigationTiltDeg = 0.0f;
    // ── Dark-sky exposure gate (Milky Way / zodiacal / aurora) ────────────────
    // The city half of the sky background is the skyglow model since 2026-10-28 (skyglowGain, updateLightPollutionDome,
    // shaders/include/darksky.glsl): mwPollutionThresholdLo/Hi and darkSkyCityMag (a log ramp of the night map from
    // 1e-6 toward an artistic 13 mag/arcsec^2) are gone, with their settings keys.
    // Twilight half of the same gate — the sun's own contribution to sky brightness. See
    // darkSkyTwilightMag() in shaders/include/darksky.glsl for the model and why the old
    // nightFactorEff ramp alone was not enough (it stops suppressing anything with the sun only
    // 11.5 deg down, and is the same in every direction).
    float darkSkyTwilightMag0 = 12.0f;   // sky brightness with the sun ON the horizon (mag/arcsec^2)
    float darkSkyTwilightEndDeg = 18.0f; // solar depression at which the sky reaches pristine — the
                                         // real end of astronomical twilight, and the main knob for
                                         // "how long after sunset does the Milky Way appear"
    float darkSkyTwilightAniso = 3.0f;   // magnitudes the antisolar sky is darker than the solar side
    // Temporal easing, now applied to the DOME (the gate's input) rather than to a final
    // visibility scalar — so the hysteresis still smooths a flight over a city boundary without
    // flattening the per-direction/per-sample structure the gate exists to produce.
    float mwFadeInTimeS = 2.1f;             // seconds to fade back IN once local pollution drops (bright
                                            // area -> dark, or ascending into space)
    float mwFadeOutTimeS = 0.26f;            // seconds to fade back OUT once local pollution rises
    float sunlitBgVisibility = 0.15f;       // Stars/Milky Way visibility fraction in space when the sun is
                                            // off-screen but the observer is still in direct sunlight — 0 =
                                            // fully hidden (like being fully day-suppressed), 1 = as visible as
                                            // true night. Sun-on-screen always forces 0 regardless of this
                                            // slider. See recordCompute()'s sky-glare gate and updateStars().
    // ── Reflect-Orbital ground beams (C12) ────────────────────────────────────
    // groundIrradiance * beamGain is NOT the same quantity as mirrorBoost/mirrorPeak (that's the
    // view-dependent specular term the OBSERVER sees the mirror glint by; this is the physical
    // irradiance the mirror delivers to its ground target, independent of view angle — see
    // sat_orbit.comp's beam-writer comment). Uploaded via SatOrbitPC.
    float beamGain = 0.0088f;
    // C12 follow-up #34: beamFootprintRadM (a flat, tunable constant) removed — the ground
    // footprint is now physically derived in sat_orbit.comp from mirror area + range to target.
    float beamMaxRangeM = 1100000.0f; // C12 follow-up #6 — render-time "is the observer close
                                        // enough to this site" cutoff (site-referenced beams have
                                        // no observer-side write gate any more, see sat_orbit.comp)
    // C12 follow-up #17: simple atmospheric-scattering beam sky glow (replaces the removed real
    // cloud-density march from follow-ups #14-#16, reverted per user request — no cloud lighting
    // yet). Own gain, separate from beamGain (that's the physical ground-irradiance term feeding
    // the ground spot; this purely scales the visual glow's brightness) — dim default, tunable.
    float beamSkyGlowGain = 0.0055f;
    // 2026-08-06 reversibility rework: replaced the old rate-limited mirror slew (deg/sec,
    // integrated frame-by-frame — inherently history-dependent, so it could not be made
    // reversible) with a fixed-width sim-time window a satellite commits to one target for. See
    // sat_orbit.comp's TargetedReflector block and CLAUDE.md for the full design. Same settings
    // slider slot (Settings → Beams), retitled "Target lock window (s)".
    float reflectorLockWindowS = 90.0f;
    // 2026-08-06 same-day follow-up: real angular-rate cap for the rate-limited ease that smooths
    // a TargetedReflector mirror's orientation across a target change — see sat_orbit.comp's
    // TargetedReflector block (nearFallbackIdeal/startAim/destAtStart) for how the ease duration
    // is derived from this and the actual angle to cover. The window-crossfade-only version
    // shipped earlier the same day only covered one of several transition cases and wasn't tied to
    // real angular distance, which read as satellites snapping to target.
    float mirrorMaxRateDegPerSec = 0.001f;
    // S1 follow-up (RELEASE_v1_1_PLAN.md): minimum acceptable local elevation angle of the
    // satellite as seen FROM a candidate ground target, in degrees. Below this, a target is
    // rejected outright by sat_orbit.comp's TargetedReflector selection (grazing beams suffer
    // heavy atmospheric extinction and aren't worth taking even as a last resort) rather than
    // being deprioritized — see that shader's own comment for the full rationale, including why
    // this replaced a pure "nearest target" rule. Sent as sin(radians(this)) via
    // SatOrbitPC::minBeamElevSin (recordCompute fills it; the sin conversion happens there, not
    // per-candidate in the shader). This is now the ONLY floor — the 2026-08-06 reversibility
    // rework removed the separate release floor/hysteresis margin along with the persisted lock it
    // existed to protect (temporal stability now comes from reflectorLockWindowS instead).
    float reflectorMinElevDeg = 10.0f;
    // (beamExtinctionMult lived here — user-tunable extra extinction for the deleted analytic
    // beam sky tube. Removed in C12 follow-up #44 along with the tube itself: the replacement
    // per-sample beam-cloud term is a real volumetric contribution composited through the cloud
    // march's own transmittance, with no separate closed-form extinction exponent to tune.)
    // C12 follow-up #30: gain for the near-field directional sky-glow bleed — the replacement for
    // the tube glow's near-field behavior (which has structural artifacts up close: "cut in half,"
    // a "hard shell," darkening in the middle — a single-point analytic approximation was never
    // designed for a camera near/inside the volume). The tube fades out approaching a beam
    // (crossfade in the shader) while this purely angular (no segment geometry) glow term fades
    // in — own gain per [[feedback_shared_gain_sliders]], not a reuse of beamSkyGlowGain.
    float beamGlowBleedGain = 0.00048f;
    // C12 follow-up #40: radius (meters) of the crossfade blend zone around a beam's own 3D line —
    // was a hardcoded kNearFieldCrossoverM constant in cloud_march.comp, now user-tunable.
    // Per-pixel cloud shadow fade distance. Was 80 km (matching the deleted 128x128 grid's
    // half-extent, so anything the grid shadowed still was). Measured 2026-08-06: pushing this to
    // the slider ceiling costs nothing, because the shadow march is per-terrain-pixel and gated on
    // `tScene < kNoSurfaceT` — raising the range does not add samples, it only stops the fade from
    // killing shadows the march already computed. So it now defaults maxed, and it is deliberately
    // NOT in applyGraphicsPreset's table; Planetarium still removes the whole pass via bit 256.
    float cloudShadowRangeM = 300000.0f;
    float beamNearFieldFadeM = 1000.0f;
    // 2026-08-09: exposed per explicit user request ("How can we control the thresholding on what
    // gets considered a group?"). Beams converged on the same real target must not all blend into
    // one cloud light when they arrive from very different parts of the sky — satellites on
    // different orbital passes genuinely illuminate that cloud from different directions, and with
    // hgG~0.99 averaging them together doesn't read as "slightly off", it reads as light arriving
    // from the wrong place entirely (in-app test #8, BEAM_CLOUD_PLAN.md).
    //
    // 2026-08-12 — SEMANTICS CHANGED, same slider, same settings key, same direction of effect.
    // Was: the maximum angle between a candidate beam and a cluster's RUNNING-AVERAGE direction for
    // the two to merge. That test depended on which beams had already joined, which is precisely
    // what made the partition order-dependent and discontinuous. Now: the angular SIZE of a fixed
    // direction bucket in the target site's own local frame. A beam's bucket is a pure function of
    // its own direction, so a cluster can no longer repartition when a member leaves. Lower =
    // finer buckets (more, more coherent clusters); higher = coarser (fewer, cheaper, more
    // blending). Stored in degrees for the UI; converted to bucket counts once per frame.
    float beamClusterDirThresholdDeg = 15.0f;
    // 2026-08-12: cross-frame fade for the cloud-light list. Now that every light has a stable
    // integer identity (see TrackedBeamLight), a light that appears, disappears or changes strength
    // can be eased instead of snapping. Deliberately asymmetric fast-in/slow-out, the same
    // convention mwFadeInTimeS/mwFadeOutTimeS and skyGlareEased already use here: a beam becoming
    // relevant should register promptly, but one dropping out should linger rather than blink off.
    // A pure appearance-smoothing knob — it does not change which lights exist, only how quickly
    // their contribution ramps.
    float beamClusterFadeInS = 0.71f;
    float beamClusterFadeOutS = 1.4f;
    // Geometry (position/direction/block altitude/opacity) eases on its own, much shorter constant,
    // deliberately NOT user-exposed. It exists to absorb the small residual steps that survive
    // stable keying — a beam entering or leaving a bucket shifts that bucket's intensity-weighted
    // mean geometry slightly — without letting a light visibly lag genuinely fast beam motion. If
    // this ever needs tuning it should become a slider rather than being folded into the fade
    // times above, which answer a different question.
    static constexpr float kTrackedLightGeomEaseS = 0.12f;
    // C12 follow-up #41: 0-1, how close the observer is to ANY active beam's actual 3D line —
    // smoothstepped from lastNearestBeamDistM/beamNearFieldFadeM each frame in recordCompute().
    // Drives the non-directional sky-glow wash in sat_sky.frag, replacing #39/#40's directional
    // dome-based approach (which read as a narrow pillar and faded incorrectly with altitude).
    float beamProximityGlow = 0.0f;
    // ── Milky Way skybox basis (session 27) ────────────────────────────────────
    // ENU->galactic rotation, recomputed each frame in updatePositions() and uploaded to
    // CloudParams. Orientation confirmed by eye against the real star field — no runtime
    // tuning knobs needed (see updatePositions() for the fixed longitude-mirror correction).
    glm::vec3 mwRow0{1.0f, 0.0f, 0.0f};
    glm::vec3 mwRow1{0.0f, 1.0f, 0.0f};
    glm::vec3 mwRow2{0.0f, 0.0f, 1.0f};
    // ── Zodiacal light ecliptic pole (session follow-up) ────────────────────────
    // ENU-frame unit vector to the ecliptic north pole, recomputed each frame in
    // updatePositions() alongside the Milky Way basis above. Unlike mwRow0-2 this is not a full
    // lon/lat basis — zodiacal light is a pure analytic falloff (elongation from the sun +
    // ecliptic latitude asin(dot(dir, eclipticPoleENU))), not texture-sampled, so only the pole
    // direction is needed.
    glm::vec3 eclipticPoleENU{0.0f, 0.0f, 1.0f};
    // Cloud tunables (CPU-side; uploaded to cloudParamsBuf each frame)
    // Defaults below are the user-tuned values baked in from settings.json (most recently
    // 2026-08-10 — cloudDensity, cloudAmbientGain, airglowCoverageGain/PolarGain, and most of the
    // Reflect-Orbital beam gains/ranges moved; the rest of the block was already current) rather
    // than placeholder guesses. They are
    // a measured, self-consistent SET — the flat-2D scales calibrate against the volumetric path,
    // and the lighting gains against each other. Do not re-derive any one of them in isolation.
    //
    // 2026-08-15: every default here (and in the photometry/beam blocks above) is rounded to
    // 2 SIGNIFICANT FIGURES, deliberately. Baking a slider position straight out of settings.json
    // produces values like 0.838158 / 157.302979 / 151902.171875 whose extra digits are the
    // slider's pixel quantization, not tuning intent, and which make the block unreadable. Keep new
    // defaults to 2 s.f. as well; if a value genuinely needs more precision than that to behave
    // correctly, say so in a comment on that line rather than silently reintroducing noise digits.
    // Anything here that GraphicsPreset::High also lists must change in both places at once:
    // High's table row is documented as "the compiled-in class member defaults, verbatim."
    float cloudCoverage = 1.0f;
    float cloudDensity = 1.3f;
    float cloudBaseAltM = 6000.0f; // layer 0 shell altitude (low cloud / stratus)
    float cloudTopAltM = 15000.0f; // layer 1 shell altitude (high cirrus)
    float cloudDriftRate = 6.5e-06f;
    // The map's drift phase = rate x (t - kCloudDriftEpochS) + this offset (session state, never
    // saved). The default puts the default rate's map exactly where rate x (t - J2000 - kGmstShiftS) did,
    // i.e. where it was before the Earth rotation gained its GMST term (an old time + kGmstShiftS shows the
    // same ground, Sun and clouds it used to).
    static constexpr double kCloudDriftEpochS = 1150891200.0;   // 2036-06-21 00:00 UTC, the start epoch
    static constexpr double kCloudDriftPhaseDefault = 3.6793436499; // fmod(float(6.5e-6) x (1150891200 - kGmstShiftS), 2pi)
    // 2026-10-03: the Earth rotation angle gained GMST at J2000 (280.46 deg). This shift, added to a time,
    // turns the Earth to where the old angle put it at that time; the start time moved by it.
    static constexpr double kGmstShiftS = 18901.0;
    double cloudDriftPhaseOffset = kCloudDriftPhaseDefault;
    double cloudDriftPhase() const;
    float cloudSunGain = 1.1f;       // near-horizon/sunset sun-gain endpoint — blended toward
                                     // cloudSunGainZenith by sun elevation (see cloud_march.comp)
    float cloudSunGainZenith = 1.0f; // sun-gain endpoint when the sun is near zenith (midday)
    float cloudAmbientGain = 1.0f;
    float cloudTwilightAmbientGain = 0.40f; // manual gain on sky-lit cloud during twilight (was piggybacking
                                            // on cloudAmbientGain, which also drives city-light
                                            // upwelling — see kNightSkyAmbientColor in cloud_march.comp)
    float cloudBaseVariance = 0.27f;        // noise-driven cloud base height undulation, hNorm units
                                            // (0 = old perfectly flat base) — see cloudMarchCS
    float cloudErosionEdge = 0.89f;         // cloudDensity() erosion strength at the silhouette edge
    float sunGainElevBand = 0.02f;          // ~1.1 deg elevation band — user-tuned 2026-08-04
    // Brought forward from the original hardcoded 0.15 so the sky term overlaps the tail of
    // direct sunlight instead of starting after it; 0.35 is ~20 deg of sun elevation.
    float twilightBandHi = 0.14f;
    float twilightBandLo = -0.25f; // user-tuned 2026-08-04
    // 1.0 rather than 0.0: a compromise starting point. Lower = more small-scale structure and
    // a closer match to the flat layer, at the cost of worse texture-cache behaviour (mip 0 of
    // the 8K map is ~33 MB and is sampled once per in-cloud march step).
    float coverageMipLod = 0.0f;
    // Measured against the volumetric at MIP 0: volumetric (coverage 1.00, sun gain 0.46)
    // matched flat (coverage 0.69, sun gain 1.84). Defaults encode those ratios so the shared
    // sliders now move both paths together instead of only ever suiting one of them.
    float flatCoverageScale = 0.18f;
    float flatSunGainScale = 1.3f;
    // Clouds at 11 km have a ground-level horizon of ~374 km, so this band puts the transition
    // near the horizon when standing on the surface, and makes everything 2D from orbit.
    float cloudDistFadeStartM = 150000.0f;
    float cloudDistFadeEndM = 400000.0f;
    // S4 (RELEASE_v1_1_PLAN.md, session 31): terrain-relief march distance fade. Below the start
    // distance the march gets its full step budget; between start and end the budget fades out;
    // past the end it is skipped entirely and the ray falls back to the sea-level sphere (exactly
    // what the "terrain off" knockout produces). So a LOWER start is CHEAPER.
    // The start was 300000 (chosen so ground views, whose grazing rays cap at ~250 km reach, were
    // untouched) until the 2026-08-06 pass took it to 50000 for a long, gradual roll-off instead of
    // a late abrupt one — distant relief now dissolves into the sphere over 50-900 km rather than
    // holding full detail to 300 km and then dropping. NOTE this is now lower than Medium's and
    // Low's start distances in applyGraphicsPreset; see the comment on that table.
    float terrainDistFadeStartM = 50000.0f;
    float terrainDistFadeEndM = 900000.0f;
    // Procedural terrain detail (terrain_detail.glsl; Terrain tab; clouds.terrain_detail_* keys).
    float terrainDetailStrength = 1.0f;
    float terrainDetailAmpM = 160.0f;
    float terrainDetailGain = 0.5f;
    float terrainDetailErode = 0.7f;
    float terrainShadowStrength = 1.0f;
    float terrainMaterialStrength = 1.0f;
    float terrainErosionStrength = 0.6f; // erosion octaves (tdErosion): fraction of the detail amplitude
    float groundPatternRangeM = 800.0f;  // review 8: farm fields stop past this pixel footprint (m); was a fixed 400
    float terrainErosionSize = 1.0f;     // ... x their cell size (512/256 m); powers of two: 0.5, 1, 2, 4
    float terrainErosionBranch = 1.0f;   // how much each erosion octave follows the gullies before it
    float terrainSkyLight = 8.0f;        // x the terrain's sky ambient (zenith integral x 0.4) — terrain v2 P1
    float terrainTextureStrength = 1.0f; // close-up material textures (terrain v2 P3)
    float cityLightsStrength = 1.0f;     // procedural city street lights (.plans/CITIES_PLAN.md)
    float cityRoadsStrength = 2.0f;      // the major roads' share of them
    float citySpriteGain = 4.0f;         // city lights as satellite point sprites (0 = off)
    float citySpriteGround = 1.0f;      // the ground glitter's share kept where the sprites carry the light
    float cityTwinkleRate = 4.0f;        // glitter + sprite scintillation rate (1 = 0.5-1.2 rad/s)
    float citySpriteStartFootM = 18.0f;  // sprites begin where a pixel spans this much ground (m): far only
    // City light LOD (review 13, Night lights tab): the ground footprint (m per pixel) where each layer ends.
    float cityPostsFootM = 27.0f;        // lamp posts and their pools
    float cityGridFootM = 380.0f;        // the street grid (past it: the glitter alone)
    float cityRoadsFootM = 1300.0f;       // the real major roads
    float cityLayoutFootM = 2000.0f;      // the street layout (past it: the far glitter only)
    float cityStreetShareNear = 0.0f;    // the streets' share of the light close up
    float cityStreetShareFar = 0.13f;    // ... and as they start to fade out
    // The city light sprites take over where a pixel spans citySpriteStartFootM of ground (the ground's
    // own footprint, stretched at grazing angles — where its glitter points smear into blobs); nearer
    // lights are the ground's glitter alone: up close a point sprite read as a floating lantern (user
    // review 2). The view's radians per pixel, and the nearest range that can qualify (the grazing
    // stretch is capped at 5x, as the ground's footprint).
    float cityPixAng(const VulkanContext &c) const
    {
        return glm::radians(camera.fovYDeg) / std::max(1.0f, (float)c.swapExtent.height);
    }
    float citySpriteMinRangeM(const VulkanContext &c) const
    {
        return glm::clamp(citySpriteStartFootM / std::max(5.0f * cityPixAng(c), 1e-6f), 500.0f, 400000.0f);
    }
    // The sprites' gain after the eye-altitude fade (off by 150 km: from orbit the ground's glitter).
    float citySpriteGainAt() const
    {
        return citySpriteGain * cityLightsStrength
             * (1.0f - glm::smoothstep(100000.0f, 150000.0f, (float)obsHeightOffset));
    }
    float terrainNightSkyLight = 0.2f;   // moonless night sky on the day albedo, fraction of the full Moon overhead
    int terrainDebugView = 0; // harness `debugview` only; not persisted
    // Cloud opacity scale (see GpuCloudParams::cloudOpacityScale) — multiplies the volumetric
    // cloud march's extinction-per-metre constant directly (and, since this same value also
    // scales layer 0's flat-2D-crossfade alphaMax ceiling in recordCompute(), the flat layer used
    // at higher observer altitude too), since raising `cloudDensity` alone cannot push a
    // saturated column past full opacity. 1.0 reproduces the original hardcoded extinction/alphaMax
    // exactly. This briefly defaulted to 7.0 (2026-08-02) to force opacity through moderately dense
    // cloud; the 2026-08-06 tuning pass took it back to ~1.0 and got the opacity from `density`/
    // erosion instead, which does not harden thin/wispy edges the way a raw extinction multiplier
    // does. The 2026-08-15 2-s.f. rounding pass took the residual 1.014 to exactly neutral — the
    // knob is still live if it's ever needed.
    float cloudOpacityScale = 0.46f;
    // (cityLightBlurLod, the city lights' blur under cloud, was removed 2026-10-06: from orbit it drew every
    // city under thin cloud as a ~1000-km grey blob. GpuCloudParams::cityLightBlurLod is unread, written 0.)
    // Atmospheric scattering strength gains (see GpuCloudParams::atmosRayleighGain/atmosMieGain
    // and common.glsl's BETA_R_BASE/BETA_M_BASE) — 1.0 reproduces the original hardcoded physical
    // constants exactly. Rayleigh gain controls how much red/orange the sky and horizon clouds
    // pick up at grazing angles/low sun elevation (preserves the R:G:B ratio, just deepens or
    // shallows the effect); Mie gain controls how much wavelength-neutral haze dilutes that color
    // back toward white/grey.
    // Domain-warp shear controls (see GpuCloudParams::cloudWarpStrength and cloud_params.glsl's
    // folding-threshold note). Were hardcoded kWarpStrength/kWarpFreq in cloud_march.comp.
    // Frequency defaulted to 6.0 until the warp was measured against the noise domains it feeds:
    // that put the detail lookup at a shear ratio of 0.8 and the per-column lookup at 4.8, i.e.
    // badly folded — the reported pinching/banding/"wavy chips". 3.0 halves the shear while
    // leaving displacement magnitude (the large-scale structure movement) untouched.
    float cloudWarpStrength = 32.0f;
    float cloudWarpFreq = 3.7f;
    // Erosion redesign (see GpuCloudParams and cloud_params.glsl). cloudSurfaceCarve = 0 and
    // cloudErosionBillow = 0 together reproduce the previous erosion exactly, so the two of them
    // bisect this change; cloudErosionFreq = 1.5 was the old hardcoded coordinate scale (the
    // 2026-08-06 pass took it to 0.5, i.e. coarser erosion lumps than the original constant).
    // 1.0 (fully subtractive) per in-app review — it also happened to mask the density-clamp
    // plateau documented in cloudDensity(), which is now fixed at its source.
    float cloudSurfaceCarve = 1.0f;
    float cloudErosionBillow = 1.0f;
    float cloudErosionBillowH = 0.45f;
    float cloudErosionFreq = 0.5f;
    // Directional-shading contrast. Previous hardcoded equivalents were 1.0 / 0.05 / 0.35 / 1.0.
    // These were first pushed hard toward contrast (clouds were shading near-uniform at sunset),
    // then pulled back by the 2026-08-06 pass once the erosion redesign and the terminator gate
    // were supplying that contrast structurally: shadowFloorT/grazeShadow/coneLenScale now sit
    // near or below the original constants and multiScatter roughly halved. See cloud_params.glsl.
    float cloudMultiScatter = 0.55f;
    float cloudShadowFloorT = 0.11f;
    float cloudGrazeShadow = 0.33f;
    float cloudConeLenScale = 0.79f;
    // Height-only shading was the "lasagna" cause; halved and now sun-elevation weighted, with a
    // density-driven occlusion term supplying the horizontal variation it never had.
    float cloudVertShadeGain = 0.53f;
    float cloudDensityAO = 0.50f;
    float cloudAOPower = 0.05f;
    // 1.0 = previous coupled behaviour. Raise after lowering `density` for the volumetric path.
    float flatDensityScale = 2.0f;
    // Flat 2D layer's own Rayleigh multiplier, stacked on atmosRayleighGain (see
    // GpuCloudParams::flatRayleighGain). 1.0 = the previous fully-coupled behaviour; raise or
    // lower it to close the hue/depth step across the 3D->2D crossfade.
    float flatRayleighGain = 0.18f;
    // Flat 2D layer's twilight sky ambient, stacked on cloudTwilightAmbientGain (see
    // GpuCloudParams::flatTwilightAmbientGain). The flat path had no ambient term at all before
    // this, so there is no prior behaviour to preserve — 1.0 starts it at the same strength the
    // volumetric shell gets, which is the matched-crossfade starting point; 0 disables it.
    float flatTwilightAmbientGain = 2.1f;
    // Orbital terminator gate (see GpuCloudParams::atmosTermStrength). Defaulted ON at the "mid"
    // setting measured during design — SZA 92 about 23x down, day side untouched — because the
    // whole point is to look at it. Drag strength to 0 for an exact A/B against the old look;
    // nothing else in the frame changes when you do.
    float atmosTermStrength = 1.0f;
    float atmosTermWidth = 0.071f;
    float atmosRayleighGain = 0.98f;
    float atmosMieGain = 1.0f;
    // C11 ground fog layer — real per-sample volumetric march in cloud_march.comp's fogMarchCS.
    // (Originally also reused beamCloudLighting() for beam godrays through fog; that reuse was
    // removed with the function itself 2026-08-09 — fog no longer carries a beam term.) Retuned
    // in-app 2026-08-06: a much taller (1.4 km) but far thinner (density ~0.07)
    // layer than the first-pass 300 m / 1.0 guess — the thin tall version reads as real haze the
    // camera can fly up through, where the thick shallow one read as a hard ground-hugging slab.
    // (v1's ground fog shell — fogTopAltM/Density/Coverage/SunGain — was retired 2026-09-29 for the v2
    // field's fog + dust, cv2Fog*/cv2Dust*; its GpuCloudParams fields are unread.)
    float cloudErosionCore = 1.0f;    // cloudDensity() erosion strength at the dense core
    float cloudHgG = 0.99f;
    float cloudCirrusWindDeg = 40.0f;  // C13: cirrus streak wind azimuth (degrees, converted to radians for the UBO)
    float cloudCirrusStretch = 2.4f;   // C13: cirrus noise anisotropic elongation factor (1 = no stretch)
    float airglowGain = 0.066f;        // C15: master airglow brightness multiplier
    float airglowGreenGain = 0.053f;   // C15: green (557.7nm) band gain
    float airglowRedGain = 0.013f;     // C15: red (630.0nm) band gain — diffuse/broad, keep subtle
    float airglowSodiumGain = 0.08f;   // C15: sodium (589.3nm) band gain — kept dim relative to green
    float airglowCoverageGain = 0.32f; // patchy-coverage strength for all 3 airglow bands, [0,1]
    float airglowPolarGain = 2.4f;     // red band only: extra boost toward the geomagnetic pole
    float zodiacalGain = 0.01f;        // master brightness multiplier for the zodiacal light cone
    // Milky Way reflected in the ocean surface (sat_sky.frag's sky-reflection block). Small by
    // nature: it is multiplied by reflStr (Fresnel, capped at 0.5, times exp(-dist/40000)) AND by
    // featureReflFade (altitude + distance to the ocean point) on the way into surfColor, and the
    // panorama's own linear luminance peaks around 0.05 — so this exists to make a faint band
    // possible on calm near water rather than to be left at 1.0. 0 disables the reflection
    // entirely. Was 1.6 through 2026-09-07 — dropped after the reflected band read far too bright
    // against the real sky it mirrors.
    float oceanMwReflGain = 0.4f;
    // Satellite mirror-flare glints on the water (sat_sky.frag's OceanGlintBuf loop — the entries
    // sat_flare.comp appends, composited onto sea-level hits only). Added 2026-09-26: the ocean end
    // of a flare was the one part of the photometry with no control at all, so a mild satellite's
    // glint could only be turned down by dimming the satellite itself (point sprite, bloom, glare
    // and the reflection share brightnessScale/flareGlowGain). See the Ocean tab's "Ocean flare refl"
    // / "Flare refl floor", and tools/settings_defaults_diff.py to compare any settings.json against
    // the defaults below.
    //
    // THEY SHIPPED AT THE VALUES THEY REPLACED — 1.0 × the whole contribution and a 2.0 effectFlare
    // floor, the two constants hardcoded at the use site — so that the new sliders could not change
    // anybody's look on their own. Later the same day they were re-defaulted to the tuned pair out
    // of build-win-release/Release/settings.json, verbatim and float32-exact:
    // 0.02017543837428093 and 37.894737243652344 (= 0.020175438f / 37.894737f below — the literals
    // are written to the digits that round to those same floats). A slider drag produced the stored
    // digits, so the tail is pixel noise rather than intent: read them as "the glint term at ~2 % of
    // what it was" and "only entries at ~60 % of fIntens' range or brighter draw at all". The tuned
    // file carries both keys, so this changes nothing for it; a settings.json written by an older
    // build (neither key present) now gets this look instead of the old hardcoded one.
    float oceanGlintGain = 0.020175438f;  // multiplier on the whole glint term (0 = no flare glints on water)
    // "Sea glitter (Sun)" (2026-10-27, issue 20261027_001448 shot 1): x the Sun's glint on the waves (the smooth path and
    // its sparkles alike), clouds.ocean_sun_glitter, UBO surgeInfo.z. 1 = the physical microfacet lobe.
    float oceanSunGlitter = 3.0f;
    float oceanGlintMinFlux = 37.894737f; // min effectFlare of an entry that draws at all (the writer's
                                          // own floor, OCEAN_GLINT_THRESH, is 1.0)
    float zodiacalWidthDeg = 10.0f;    // ecliptic-latitude Gaussian sigma near the sun (degrees)
    float zodiacalOuterFadeDeg = 80.0f; // elongation at which the cone has fully faded (degrees)
    // Sun self-shadow cone (N_CONE) fades out beyond this distance. Was 22 km, when the cone
    // marched a fixed stride and distance directly bought samples. The cone now absorbs distance
    // into its stride (see cloud_march.comp's shadowFade comment), so this became a reach knob
    // rather than a cost knob and the 2026-08-06 pass effectively unbounded it — self-shadowing
    // now survives all the way out to the cloud shell's own horizon instead of dropping off a
    // cliff at 22 km and leaving distant cloud flat-lit.
    float cloudShadowMaxDistM = 6000000.0f;
    float cloudMaxRenderDistM = 800000.0f; // cloudMarch tExit distance cap — raised to ~400km
                                           // (session 28 follow-up #10): the low-cloud shell's own
                                           // geometric horizon distance at 11km altitude is
                                           // ~sqrt(2*R_EARTH*11000)≈374km; the prior 165km default
                                           // cut the march off well short of that, letting
                                           // aurora/Milky Way/stars show straight through clouds
                                           // near the horizon instead of them thinning out naturally
    // Perf follow-up (session 24): main atmosphere loop + ocean wave quality, all previously
    // hardcoded compile-time constants.
    // N_VIEW is now adaptive per-ray (round 2): a fixed sample count badly serves a loop whose
    // path length (tEnd) varies from ~100km (straight up) to 2000+km (grazing/horizon/orbit) —
    // see the adaptive-N_VIEW comment in sat_sky.frag for the full reasoning. viewSamplesMin is
    // the user-validated "looks convincing" floor for short ground-level rays (4 showed visible
    // artifacts in testing; 6 was clean — round 3); viewSamplesMax is the ceiling for long/grazing
    // rays. That ceiling was the prior universal fixed value (124) until the 2026-08-06 tuning
    // pass raised it to ~157 (rounded to 160 by the 2026-08-15 2-s.f. pass): the orbital
    // terminator gate (atmosTermStrength) puts real structure in the deep-twilight tail of the
    // integral, where 124 samples banded on long limb rays.
    float viewSamplesMin = 6.5f;
    float viewSamplesMax = 160.0f;
    float lightSamples = 2.4f;               // N_LIGHT: optDepth sun-side sub-march count
    float oceanSeaOctaves = 3.0f;            // seaMap() octave count (height-trace geometry)
    float oceanDetailOctaves = 5.0f;         // seaMapDetail() octave count (wave normal)
    float orbitGrade = 1.0f;
    // The Moon as a body (updatePositions): true topocentric position and size, eclipses.
    float moonSizeScale = 1.0f;             // "Moon size (x real)" (slot 220; the old disc was 3x). 1 = true size by default since 2026-10-27 (conjunctions, occultations, eclipses as they are); review 16 had 2.5
    // The farthest the player's eye may climb above sea level (review 16: Q/E boost reached the Moon). Past GEO
    // and the full-disc views the high-orbit ambience is built for, a quarter of the way to the Moon.
    static constexpr float kMaxObsHeightM = 1.0e8f;
    double moonDistM = 3.844e8;
    float moonIllumFrac = 0.5f;             // the Moon's lit fraction (moonDirENU.w is its light vs full, review 16)
    float moonAngR = 0.004578f;
    float sunAngRTrue = 0.0046542f; // the Sun's true angular radius (rad) this frame, from its distance
    glm::vec3 moonCenterENUkm{0.0f};
    glm::vec3 moonPoleENU{0.0f, 0.0f, 1.0f};   // the Moon's body frame in the observer's ENU (IAU rotation model)
    glm::vec3 moonPrimeENU{1.0f, 0.0f, 0.0f};
    double moonLibLonDeg = 0.0, moonLibLatDeg = 0.0, moonPolePADeg = 0.0;   // the sub-Earth point (geocentric optical libration)
    float moonRelief = 1.0f;                // "Moon relief" (x the LOLA heights on the disc; slot 355)
    float moonShadows = 1.0f;               // "Moon shadows" (0/1: the heightfield's cast shadows; slot 356)
    float moonEclipseSolarObs = 1.0f;
    float moonEclipseSkyObs = 1.0f;   // the sky light's share at the observer in a solar eclipse (eclipse.glsl's blurred visibility)
    bool moonEclipseSolarPossible = false, moonEclipseLunarPossible = false;
    glm::dvec3 moonGeoEciM(double t) const;
    double findEclipse(bool solar, double t0) const;                // orbit colour grade (Atmosphere tab, slot 219; the Artemis II photos)
    float auroraSheetGain = 1.7f;           // aurora sheets (Atmosphere tab, slots 215-218): strength, 0 = off
    float auroraSheetSpacingDeg = 0.35f;     // colatitude between sheets
    float auroraSheetCrisp = 0.61f;          // share of crisp (thin) sheets
    float auroraSheetFold = 1.2f;           // curls / spirals
    // Aurora storms (Atmosphere tab, slots 298-301, 2026-10-14; cloud_params.glsl auroraStorm).
    float auroraStormFeatures = 1.2f;       // tall red-topped curtains + magenta borders above Kp ~5
    float auroraBlueRayChance = 0.004f;     // rare giant rayed bands with sunlit blue tops (per ~220-km sheet stretch)
    float auroraArcSystems = 1.0f;          // sheets grouped into a few bright arc systems (0 = all alike; 0..1, was
                                            // 2 outside its 0-1 slider until 2026-10-21: the shader maps 1 to that look)
    float auroraStormRedGain = 0.96f;        // storm-time 630-nm red equatorward of the oval
    float oceanSeaStateGain = 1.0f;          // waves follow the weather (Ocean tab, slot 213; 0 = fixed sea)
    float oceanWhitecapGain = 1.0f;          // whitecap foam gain (Ocean tab, slot 214)
    float oceanWaveRangeKm = 100.0f;         // review 21: waves fade to flat from 0.6x this (was a fixed 3-8 km; slot 237)
    // Review 24 (Ocean tab, slots 248-250): the resolvable ripple where the octaves are filtered away (0 = the mirror),
    // and the wave field's 2D warp at 24 / 8 cells (octave-0 cells, ~18 m).
    float oceanFarRipple = 1.7f, oceanWarp = 3.5f, oceanWarpDetail = 1.2f;
    float oceanFarRippleSize = 1.5f;   // review 27 (slot 256): the far ripple's cell, in along-view pixel footprints
    float oceanStormTrails = 1.0f;     // 2026-10-04 (slot 260): "Storm sea trails", the sky TAA keeps a stormy sea's history
                                       // unclipped (crests smear like spray and chop); 0 = off, x the sea state's storm share
    float oceanWaveRangeFade = 0.4f;   // review 28 (slot 257): the share of the wave range the waves fade over (0.4 = from 0.6x)
    float oceanMotionFilter = 1.0f;          // 2026-10-22: the wave octaves at the pixel footprint while the eye moves (cp.oceanWaveFootK)
    float seaMotionK = 0.0f;                 // its eased share (SatelliteSim.cpp, the CloudParams fill)
    glm::dvec3 seaMotionPrevEye{0.0};
    bool seaMotionPrevValid = false;
    float oceanWaveSharpness = 0.1f;         // review 21: x the footprint the wave octaves are filtered at (1 = review 14; slot 243)
    float oceanReflSamples = 6.0f;           // ocean sky-reflection loop sample count (N_REFL)
    float moonGain = 0.0053f;                // shared moonlight brightness: terrain direct term + cloud
                                             // moonContrib (default matches the prior hardcoded cloud value)
    float stormStrength = 0.29f;             // C16: aurora fold chaos / coverage fill [0,1]; DERIVED each frame
                                             // from the activity index (Kp - 1) / 9 (max 0.85) since 2026-10-03
    // Aurora activity (2026-10-03, SpaceWeather.h): "Space weather (0 manual / 1 auto)" picks between the
    // deterministic space-weather history over sim time and a fixed "Aurora Kp (manual)"; "Storm frequency"
    // scales the CME and coronal-hole odds. spaceWeather_ / auroraGpu_ are this frame's.
    float auroraActivityAuto = 1.0f;         // slot 258
    float auroraKpManual = 3.0f;             // slot 25 (was "Storm strength")
    float auroraStormRate = 1.0f;            // slot 259
    SpaceWeather spaceWeather_{};
    AuroraGpu auroraGpu_{};
    float auroraGain = 0.1f;                 // C16: master aurora brightness multiplier
    float auroraCloudGain = 0.0018f;         // C16: ambient aurora light on clouds only (no albedo term
                                             // in that formula, so it needs a much lower default than
                                             // terrain/ocean to land in the same plausible range)
    float auroraGroundGain = 0.0075f;        // C16: ambient aurora light on terrain/ocean only
    float auroraCoverageFreq = 0.42f;        // C16: coverage patch size (per-degree colat frequency)
    float auroraCoverageAzFreq = 3.1f;       // C16: coverage azimuthal wobble frequency
    float auroraCoverageDriftRate = 0.0012f; // C16: coverage evolution speed (wall-clock rad/s)
    float auroraShimmerRate = 0.041f;       // C16: curtain fold noise evolution speed (wall-clock rad/s)
    VulkanContext *ctx_ = nullptr;           // set in init(), used for lazy icon loading
    AudioSystem *audio_ = nullptr;           // set via setAudio(), used in buildUI()
    std::string exeDir_;                     // directory containing the exe (read-only game data); set in init()
    std::string userDataDir_;                // per-user writable dir for settings/perf (see Paths.h); set in init()

    // ── NEW-3: crash-safe mode ──────────────────────────────────────────────
    // A sentinel file is created at the top of init() and deleted at the bottom of cleanup()
    // (the clean-exit path). If it's already present at the NEXT launch, the previous run never
    // reached cleanup() — crash, hang + force-kill, power loss — so this run steps the preset down one
    // tier (crashRecoveryPreset: full -> Planetarium -> Potato) and shows a one-line notice, converting "launch -> crash -> uninstall"
    // into a recoverable outcome. See applySettings-adjacent logic in init()/cleanup().
    char crashNoticeBuf[192] = {};          // its text (Clay keeps the pointer until record)
    float crashRecoveryNoticeTimer = 0.0f; // seconds remaining to show the notice banner; see buildCrashRecoveryNotice
    bool crashRecoveryMode = false;        // mirrors the crashDetected local in init(); read by finishIntro()
                                           // so a crash-recovery launch never runs the UC1 benchmark promote/
                                           // demote (that launch already forced Planetarium for a different reason)

    // ── UC3: cinematic intro camera path (folds in UC1 mechanism 2, the first-run benchmark) ──
    // Does not move obsDir/lat-lon DURING playback — only obsHeightOffset (altitude, literally
    // what Q/E controls), camera.elDeg/fovYDeg, and a facing-azimuth rotation change across the
    // beat sheet (see kIntroKeyframes in SatelliteSim.cpp). It DOES force obsDir/lat-lon once, at
    // the very start of playback (see updateIntroCinematic's one-time init block) — to the fixed
    // kIntroObserverLatDeg/LonDeg vantage point, not whatever the player's last position happened
    // to be — so the intro (including a Display-tab replay) is reproducible regardless of where
    // the player has since wandered off to. Since obsDir is fixed for the rest of playback after
    // that, the East/North tangent basis below is computed once and stays valid the whole time —
    // no great-circle interpolation needed.
    bool introBasisValid = false;
    glm::vec3 introEastEF{1, 0, 0}, introNorthEF{0, 1, 0};
    float introElapsed = 0.0f;    // seconds since the intro cinematic started
    int introCaptionIndex = 0;    // index into kIntroKeyframes of the most recently reached caption
    bool introSkipped = false;    // true if dismissed early (Space/gamepad Start) — gates the benchmark below
    bool introIsReplay = false;   // set by the Display tab's "Replay Intro" button; suppresses the
                                  // benchmark regardless of how the replay ends (see finishIntro) —
                                  // it's a one-shot first-run decision, not something a replay should redo
    float introBenchMsSum = 0.0f; // accumulates gpuMsTotalSmoothed across the camera-motion beats (see updateIntroCinematic)
    int introBenchFrames = 0;

    // Post-intro "graphics set to X" notice (UC1 mechanism 3: always tell the user, never
    // silently re-decide) — same dismissible-banner pattern as buildCrashRecoveryNotice, separate
    // timer/text since the two can in principle be showing different things.
    float graphicsAutoNoticeTimer = 0.0f;
    char graphicsAutoNoticeText[192] = {};

    // ── UC6: screenshots ────────────────────────────────────────────────────────
    // See Simulation.h's wantsCleanScreenshot/recordScreenshotCopy/finalizeScreenshot doc
    // comments for the three-phase (request -> copy -> readback) protocol this drives.
    bool screenshotRequested = false;                     // set by dispatchKeyAction(KB_SCREENSHOT); consumed by
                                                          // recordScreenshotCopy (also gates wantsCleanScreenshot)
    bool screenshotCopyPending = false;
    uint64_t screenshotCopyFrame = 0;   // the frame index whose command buffer holds the copy (frames in flight)
    // The copy's readback (the part of finalizeScreenshot after the exposure meter). force: wait for the frames in
    // flight first, so a recorded path or an export moves on in the same frame (one rendered frame per recorded frame,
    // as with a single frame in flight).
    void finalizeScreenshotCopy(bool force = false);                   // true between "copy recorded" and "readback finalized"
    VkBuffer screenshotStagingBuf = VK_NULL_HANDLE;       // host-visible; freed/recreated per capture —
    VkDeviceMemory screenshotStagingMem = VK_NULL_HANDLE; // screenshots are rare, not a hot path
    uint32_t screenshotW = 0, screenshotH = 0;
    VkFormat screenshotFormat = VK_FORMAT_UNDEFINED;
    std::string screenshotPath; // full output path, built at request time
    float screenshotToastTimer = 0.0f;
    char screenshotToastText[160] = {};
    bool screenshotToastWarn = false; // amber instead of green: a failure or input that was not understood
    bool snapshotKeyPending = false;   // KB_SAVE_SNAPSHOT: saved in buildUI, which has the frame's dt
    // PNG encoding (stbi_write_png) is genuinely slow in an unoptimized Debug build — easily
    // tens of seconds at 1080p+, which reads as "the game froze" since finalizeScreenshot() used
    // to run it synchronously on the main thread. Moved to a detached background thread: the main
    // thread only maps the GPU buffer, swizzles into a plain std::vector it hands off by move, and
    // returns immediately. screenshotEncoding guards against starting a second capture while one
    // is still encoding (the vector handoff means the GPU staging buffer itself is free again
    // immediately, but re-entrant encodes would still race on the toast result below).
    // screenshotResultReady/screenshotResultMutex/screenshotResultText are the thread's one-shot
    // handoff back to the main thread (checked once per frame in buildUI) — the atomic gates
    // whether it's worth taking the mutex at all, avoiding any per-frame lock when idle.
    std::atomic<bool> screenshotEncoding{false};
    std::atomic<bool> screenshotResultReady{false};
    std::mutex screenshotResultMutex;
    std::string screenshotResultText;
    // Kept joinable (never .detach()ed) so cleanup() can join it before the object it captures
    // (`this`, for the mutex/atomics/string above) is destroyed — a detached thread still running
    // past that point would be a use-after-free. screenshotEncoding already prevents two threads
    // existing at once, so join() here is always fast (the previous one has either already
    // finished or is about to).
    std::thread screenshotThread;
    // Harness captures (docs/HARNESS.md `capture`): keep the UI in the shot, crop and rescale before
    // the encode. Reset after every request.
    bool screenshotIncludeUI = false;
    bool screenshotHideOverlays_ = false; // set with screenshotIncludeUI by a player's shot (shotHidesOverlays)
    // Display tab "SCREENSHOTS": keep the UI in F12 screenshots / in issue-report shots (F9 / F7). The
    // "Screenshot with UI" key (KB_SCREENSHOT_UI) always keeps it. display.screenshot_ui / report_shot_ui.
    bool screenshotUiSetting = false;
    bool reportShotUiSetting = false;
    int screenshotCrop[4] = {0, 0, 0, 0}; // x, y, w, h in frame pixels; w == 0 = the whole frame
    float screenshotScale = 1.0f;          // < 1 box-filtered down, > 1 nearest-neighbour up (pixel peeping)
    uint32_t screenshotLastW_ = 0, screenshotLastH_ = 0; // the last copy's encoded size (finalizeScreenshotCopy)
    uint64_t screenshotSerial_ = 0;                      // +1 per copy handed to the encoder

    // ── Video export (SatelliteSimVideo.cpp, 2026-10-25) ────────────────────────────────────────────
    // An .mp4 of the frames as they are shown: VideoRecorder copies a frame into a ring of host buffers (FrameCapture,
    // read back two frames later) and encodes on a worker thread (Media Foundation's H.264 on Windows, else an
    // ffmpeg on PATH). Nothing on the frame thread waits: a frame with no free buffer is dropped and the next one
    // repeated for it. KB_VIDEO / the HUD's video button record to <user data>/videos/; the harness's `video` and
    // `path play ... video=` to <run>/videos/.
    VideoRecorder video_;
    bool videoUiSetting = false;     // display.video_ui: the UI in the video (else the frame before the UI)
    float videoFpsSetting = 60.0f;   // display.video_fps
    float videoScaleSetting = 1.0f;  // display.video_scale (of the window)
    bool videoByHarness_ = false;    // started by the harness: no REC badge, no toast
    bool hovVideoBtn = false;
    std::string videoLastFile_;
    void videoToggle();                                    // KB_VIDEO / the HUD button
    bool videoStart(const VideoRecordOptions &o, bool byHarness, std::string &err);
    void videoStop();
    void videoFrameTick(float dt);                         // recordCompute: the recorder's clock
    void videoPostUiCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image); // recordScreenshotCopy (ui=on)
    void videoAfterFence();                                // finalizeScreenshot: readbacks, a finished file
    void buildVideoHud(const UIInput &inp, UIRenderer &ui); // the REC badge
    // The harness's settle probe (`wait settle auto`): a small copy of every settle frame before the UI, compared with
    // the frames 8 and 32 before (mean |luminance difference|, 0-255) to stop once the image has stopped changing.
    FrameCapture settleProbe_;
    bool settleProbeWanted_ = false;
    std::deque<std::vector<float>> settleLum_;
    std::vector<float> settleTrace_;                       // per probed frame: the 8-frame difference (-1 = no history)
    std::vector<float> settleTrace32_;                     // ... and the 32-frame one (drift vs noise)
    void settleProbeReset();
    void settleProbeCollect();

    // ── Automation harness (docs/HARNESS.md; all code in SatelliteSimHarness.cpp) ──────────────
    // Null outside a harness run: every hook below is then a no-op.
    harness::Runner *harnessRunner_ = nullptr;
    bool harnessQuit_ = false;
    bool harnessStateStv_ = false; // `state sun_volume=1`: harnessStateJson reads the Sun volume's levels back (slow)
    // `wait settle`: frames left. While > 0 the frame time is kHarnessSettleDt and sim time is held,
    // so every eased quantity (sky glare, dark-sky dome, mesh fades, beam-light fades, env probes)
    // converges without the scene moving.
    int harnessSettleFrames_ = 0;
    bool harnessSettleSavedPaused_ = false;
    static constexpr float kHarnessSettleDt = 0.5f;
    // `camera track`: re-aim every frame. 0 off, 1 selection, 2 sun, 3 moon, 4 planet.
    int harnessTrack_ = 0;
    int harnessTrackPlanet_ = -1;
    std::string lastSweepRecordJson; // the knockout sweep's last record (for `sweep`)
    // `path key` / `path play` (docs/HARNESS.md "Camera paths"): keyframes in path seconds, cubic
    // Hermite (Catmull-Rom tangents) per channel; angles unwrapped against the previous key, altitude
    // interpolated in log space (2 m to LEO is five orders of magnitude).
    // The harness's camera path IS the cinematic's current shot (review 17, Cinematic.h): `path key` edits
    // the keys the Cinematics window shows, and either can play or export them.
    using HarnessCamKey = CineKey;
    float harnessFixedDtOverride_ = 0.0f; // > 0 while a path plays: 1 / fps

    // ── Cinematics (review 17, SatelliteSimCinematic.cpp) ──────────────────────────────────────────────
    Cinematic cine_;
    int cineShot_ = 0;                    // the shot being edited
    std::vector<CineKey> &cineKeys();     // its keys (a shot is created if there is none)
    CineKey cineCurrentPose() const;      // the current observer, camera and sim time as a key
    enum class CineRun { Idle, Play, ExportPreview, ExportHQ };
    CineRun cineRun_ = CineRun::Idle;
    bool   cineLoop_ = false;
    bool   cinePlayOneShot_ = false;      // play / export only the current shot
    double cineT_ = 0.0;                  // global time into the cinematic (or the shot)
    int    cineFrame_ = 0, cineFrames_ = 0, cineSettle_ = 0, cineLastShot_ = -1, cinePrerollShot_ = -1;
    int    cineHqShot_ = -2, cineHqPrior_ = 0;   // the HQ export's running-mean prior (cineTick)
    double cineShotSim_ = 0.0;            // the shot's sim time at its start (when no key sets it)
    float  cineFixedDt_ = 0.0f;           // > 0 while exporting: 1 / fps (frameDt)
    bool   cineSavedPaused_ = false;
    bool   cineVideoExport_ = false; // the preview export writes one .mp4 (video_) instead of PNG frames (2026-10-25)
    float  cineSavedFullRateKm_ = 0.0f, cineSavedSparse_ = 0.0f;
    float  cineSavedPointSigma_ = 0.0f, cineSavedPointMax_ = 0.0f, cineSavedGlare_ = 0.0f;
    std::string cineOutDir_, cineStatus_;
    float  cineScrub_ = 0.0f, cineExportFps_ = 30.0f, cineSimRateUi_ = 1.0f, cineEaseUi_ = 0.5f;
    // Motion blur (review 17): each exported frame is the mean of cineBlurSubs_ subframes spread over a 180-degree
    // shutter (half a frame), accumulated in LINEAR light in finalizeScreenshot (cineAccum_) before the encode.
    float  cineBlurSubs_ = 1.0f;
    float  cineVideoScale_ = 1.0f;   // the video export's output scale (harness `cine preview scale=`; 1 = the window)
    std::string cineVideoTag_;       // appended to the video export's file name (harness `cine preview shot=N` -> _shotN)
    int    cineSub_ = 0, cineAccumSubs_ = 0, cineAccumCount_ = 0;
    std::vector<float> cineAccum_;
    bool   cineHqSettingsSaved_ = false;
    WindowChrome cineChrome;
    bool   hovCineClose = false, hovCineBtn[40] = {}, hovTimeCine = false, hovCineKey[24][5] = {}, hovCineFile[16][2] = {};
    bool   hovCineShotTab[16] = {}, hovCineHudStop = false;
    float  cineKeyGap_ = 2.0f;           // "+ Key at view": seconds after the last key
    int    cineDragSlider_ = -1;
    std::vector<std::string> cineFiles_;  // <user data>/cinematics/*.json, refreshed when the window opens
    char   cineInfoBuf_[256] = {}, cineKeyBuf_[24][128] = {};
    // 2026-10-03: the file the cinematic was loaded from / saved to (empty = never saved), an export's wall-clock
    // start (progress, ETA), and the timeline's drag state.
    std::string cineFile_;
    double cineExportT0_ = 0.0;
    int    cineTimelineDrag_ = -1;            // -2 = the playhead, >= 0 = a key being dragged
    bool   hovCineTimeline_ = false;
    std::string cineDeleteArmed_;             // a file whose Delete was clicked once (a second click deletes)
    double cineEstimateHqS() const;           // the HQ export's expected wall time at the last measured frame cost
    bool   cineKeyFollowMismatch(std::string &why) const;   // a follow shot's key can only be taken while following
    bool cineActive() const { return cineRun_ != CineRun::Idle; }
    void cineStart(CineRun mode, bool oneShot);
    void cineStop(const char *why);
    void cineTick(float dt);              // buildUI, right after harnessTick: plays / exports
    void cineApplyAt(double t, bool force);
    bool cineSave(const std::string &file, std::string &err);
    bool cineLoad(const std::string &file, std::string &err);
    std::string cineDir() const;
    void cineRefreshFiles();
    void buildCinematicWindow(const UIInput &inp, UIRenderer &ui);
    void buildCineHud(const UIInput &inp, UIRenderer &ui);   // playback / export progress, over everything
    static std::string cineFileStem(const std::string &name);   // a cinematic's name as a file name
    nlohmann::json cineLookSettings();    // the settings a shot stores: clouds, photometry, rosters, render
    HarnessCamKey harnessEvalPath(double t) const;
    void harnessApplyCam(const HarnessCamKey &k);
    // `snapshot` / `report load`: put the sim in a perf snapshot record's view (settings unless told not to,
    // sim time paused, observer, camera, cloud drift phase). raw = settings as saved, past their range guards.
    // Returns where the drift phase came from.
    std::string harnessApplyViewRecord(const nlohmann::json &rec, bool applySettings, bool raw, bool driftDefault);
    // `overlay`: screen text and labels that follow a sky target, drawn even with the HUD hidden (so
    // `ui hide` + `capture ui=on` gives a clean titled frame for videos).
    struct HarnessOverlay
    {
        std::string id, text;
        float x = 0.5f, y = 0.1f; // screen fractions (text), or the offset in px from the target (label)
        float size = 28.0f;
        int target = -1;           // -1 = screen text; else the camera-track code (1 sel, 2 sun, 3 moon, 4 planet)
        int planet = -1;
        bool center = true;
        glm::vec4 color{255.0f, 255.0f, 255.0f, 255.0f}; // 0-255 RGBA (Clay_Color at draw time)
    };
    std::vector<HarnessOverlay> harnessOverlays_;
    void buildHarnessOverlays(const UIInput &inp, UIRenderer &ui);
    // `probe x y`: terrain_probe.comp re-runs the terrain march for one pixel (one thread, only when
    // requested, right after scene_depth.comp) into a host-visible buffer read back next frame.
    VkDescriptorSetLayout probeDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool probeDescPool = VK_NULL_HANDLE;
    VkDescriptorSet probeDescSet = VK_NULL_HANDLE;
    VkPipelineLayout probePipeLayout = VK_NULL_HANDLE;
    VkPipeline probePipeline = VK_NULL_HANDLE;
    VkBuffer probeBuf = VK_NULL_HANDLE;
    VkDeviceMemory probeMem = VK_NULL_HANDLE;
    void *probeMapped = nullptr;
    bool probeRequested = false;
    glm::vec2 probePx{0.0f};
    void createTerrainProbe(VulkanContext &ctx);
    void recordTerrainProbe(VkCommandBuffer cmd, VulkanContext &ctx);
    void destroyTerrainProbe(VkDevice device);
    int sweepsCompleted = 0;
    // The ~ console: the same command language typed by a person (docs/HARNESS.md). Outside a
    // harness run its first command lazily creates a runner writing to harness_runs/console_<time>
    // next to the exe, in real time (no fixed frame step).
    UIRenderer *harnessUi_ = nullptr; // this frame's renderer (buildUI's argument), for `ui dump`
    bool consoleOpen_ = false;
    std::string consoleInput_;
    std::vector<std::string> consoleHistory_;
    int consoleHistIdx_ = -1;
    std::vector<std::string> consoleView_; // this frame's lines; Clay keeps raw pointers into them
    bool consoleKey(int key, int action); // true = consumed
    void ensureConsoleRunner();
    void buildHarnessConsole(const UIInput &inp, UIRenderer &ui);
    // (see harnessOverlays_)
    void harnessInit();
    void harnessTick();
    harness::Status harnessExec(harness::Active &a);
    bool harnessLookDir(int track, int planet, glm::vec3 &enuDir); // false if unavailable
    nlohmann::json harnessStateJson();
    nlohmann::json auroraStateJson() const; // SatelliteSimHarness.cpp: the aurora activity (SpaceWeather.h)
    float cpuTerrainHeightM(float latDeg, float lonDeg) const;
    nlohmann::json buildSettingsJson();
    void applySettingsJson(const nlohmann::json &j, bool isPatch);

    // ── Key bindings (editable in the settings window) ────────────────────────
    // All interactive keys go here — both event keys (pressed once) and held keys
    // (polled each frame).  Adding a new control is one line in the keybindings
    // initializer; the settings window and rebind UI are driven entirely from this
    // vector so no other plumbing is needed.
    //
    // held=false  → dispatched in onKey() via pressed(idx)
    // held=true   → polled in recordCompute() via glfwGetKey(win, keybindings[idx].key)
    //
    // gpButton mirrors key but for an Xbox-style gamepad (GLFW_GAMEPAD_BUTTON_*, -1 =
    // unbound) — either input fires the same action, so rebinding one never disturbs the
    // other. listening/listeningPad are mutually exclusive across the whole vector (the UI
    // clears every other flag before setting one), each capturing the next keyboard key or
    // gamepad button respectively — see onKey() and pollGamepad().
    struct KeyBinding
    {
        const char *action;
        int key;
        int gpButton = -1;
        bool held = false; // true = polled (held modifier), false = event (pressed once)
        bool listening = false;
        bool listeningPad = false;
    };
    std::vector<KeyBinding> keybindings;

    // Canonical index constants — keeps onKey / recordCompute in sync with the
    // keybindings array without magic numbers.
    enum KB
    {
        KB_TOGGLE_UI = 0,
        KB_PAUSE = 1,
        KB_SLOWER = 2,
        KB_FASTER = 3,
        KB_REVERSE = 4,
        KB_MOVE_BOOST = 5,     // held
        KB_MOVE_FINE = 6,      // event — toggles fine (slow) movement mode; see fineMoveToggled
        KB_CINEMATIC = 7,      // event — toggles camera drift mode while panning
        KB_RAISE_ELEV = 8,     // Q — held — raise observer above terrain (gamepad: analog right trigger, see gpElevRaise, not this binding's gpButton)
        KB_LOWER_ELEV = 9,     // E — held — lower observer toward terrain (gamepad: analog left trigger, see gpElevLower)
        KB_RESET_ELEV = 10,    // Z — event — snap observer back to terrain elevation
        KB_ZOOM_IN = 11,       // held — narrows FOV (zoom in)
        KB_ZOOM_OUT = 12,      // held — widens FOV (zoom out)
        KB_ZOOM_RESET = 13,    // event — snap FOV back to default
        KB_SELECT_SAT = 14,    // event — select the satellite nearest the center of the screen
        KB_SCREENSHOT = 15,    // event — UC6: capture screenshots/satlight_<timestamp>.png (clean, no UI)
        KB_TOGGLE_CURSOR = 16, // event — UC5: gamepad virtual-cursor mode toggle (default: Menu/Start)
        KB_TOGGLE_TRAILS = 17, // event — long-exposure trail on/off (default: F; Select Satellite
                               // moved off F to T to free this up — see keybindings init)
        KB_SAVE_SNAPSHOT = 18, // event — a shot into the active issue report (+ a line in perf_profiles/profile_log.jsonl)
        KB_PHOTO = 19,         // event — HQ photo: a supersampled, settled screenshot (requestPhoto)
        KB_REPORT_NEW = 20,    // event — a snapshot that starts a NEW issue report (KB_SAVE_SNAPSHOT adds to the active one)
        KB_SCREENSHOT_UI = 21, // event — a screenshot that keeps the UI (default F10)
        KB_MISSIONS = 22,      // event — the Missions window (launches, crew flights, the Sun's Fury) open / closed
        KB_FOLLOW_STAY = 23,   // event — G: with a satellite selected, Go to it; while following, leave Go to where the
                               // camera is (stopFollowHere). One key in and out (2026-10-28, issue 20261028_223530)
        KB_FOLLOW_RETURN = 24, // event — Go to: back to the followed object, or follow the last one again; H
        KB_MILESTONE_NEXT = 25, // event — the selected / followed launch object's next mission milestone; ]
        KB_MILESTONE_PREV = 26, // event — ... its previous one; [
        KB_WEATHER = 27,       // event — the Weather window (radar, storms) open / closed; N
        KB_VIDEO = 28,         // event — start / stop a video recording (videoToggle); F6
        KB_SKY_EVENTS = 29,    // event — the Sky events window (eclipses, conjunctions, occultations) open / closed; K
        KB_INFO = 30,          // event — the selected satellite's Info window open / closed; I
        KB_TRACK = 31,         // event — Track (lock the camera on the selection) on / off; T
        KB_COUNT = 32,
    };

    // Dispatches the event-style action for keybindings[bindIdx] — shared by onKey()
    // (keyboard) and pollGamepad() (gamepad edge-detect) so the two input paths can never
    // drift apart. No-op for held bindings (MOVE_BOOST, RAISE/LOWER_ELEV, ZOOM_IN/OUT):
    // those are polled directly, not dispatched.
    void dispatchKeyAction(int bindIdx);

    // Polled once per frame from recordCompute(). Scans for a connected gamepad, edge-detects
    // event-style button presses (dispatchKeyAction) and rebind capture (listeningPad), and
    // fills gpMoveFwd/gpMoveRight/gpLookYawDeg/gpLookPitchDeg from the sticks for the
    // movement/look code in recordCompute()/buildUI() to consume.
    void pollGamepad(float dt);
    // True if the gamepad button bound to keybindings[bindIdx] is currently held down.
    bool gpHeld(int bindIdx) const;
    // UC4: reports the virtual cursor's screen position/click state (updated in pollGamepad);
    // see Simulation.h for the calling convention.
    bool virtualCursor(float &x, float &y, bool &lmb) const override;
    // Harness `ui click` (docs/HARNESS.md): a scripted pointer that overrides the mouse for a few frames.
    bool  harnessPtrActive_ = false, harnessPtrDown_ = false;
    float harnessPtrX_ = 0.0f, harnessPtrY_ = 0.0f;

    // ── Text entry (2026-10-03, SatelliteSimUI.cpp "Text fields") ───────────────────────────────────────────
    // One field at a time has the keyboard. Enter / Tab / a click elsewhere commits, Esc cancels. A field learns
    // that its edit ended when it is next drawn (doneId), so the commit needs no callback.
    struct TextEdit
    {
        uint32_t id = 0;              // Clay id of the focused field (0 = none)
        std::string buf;
        int  caret = 0;
        bool selAll = false;          // the whole text is selected: the first key replaces it
        int  anchor = -1;             // the other end of a selection (Shift+move, a mouse drag); -1 = none
        bool enterCommits = false;    // multiline: Enter commits and Shift+Enter is the new line (the note bar)
        bool ensureCaret = false;     // a key moved the caret: its text box scrolls it into view when next drawn
        bool drawn = false, hovered = false;   // the focused field this frame
        uint32_t doneId = 0;          // a field whose edit was committed: it applies doneBuf when next drawn
        std::string doneBuf;
        int  doneAge = 0;
        double t0 = 0.0;              // caret blink phase
        bool multiline = false;       // a textArea: Enter is a new line (Ctrl+Enter / Tab / a click elsewhere commit)
        size_t maxLen = 120;
    } textEdit_;
    bool textEditing() const { return textEdit_.id != 0; }
    void textEditFocus(uint32_t id, const std::string &text);
    void textEditCommit();
    bool textEditKey(int key, int action);  // onKey: true = consumed
    bool textEditSel(int &lo, int &hi) const; // the selected range [lo, hi), false = none
    bool textEditDropSel();                 // erase it (the caret lands at its start): true = there was one
    void textEditEndFrame(const UIInput &inp);
    // A one-line text box: true (and value set) on the frame an edit is committed.
    bool textField(const UIInput &inp, UIRenderer &ui, Clay_ElementId id, std::string &value, float width, float fontPx,
                   const char *tip = nullptr, size_t maxLen = 64);
    // A number shown as text that turns into a text box on click (sliders' value labels): true on commit, v set
    // (clamped to [vmin, vmax]). `devUnbound`: a tuning slider's value, which dev mode (devUnboundedValues) lets
    // a typed number leave the range of.
    bool numberField(const UIInput &inp, UIRenderer &ui, Clay_ElementId id, float &v, float vmin, float vmax,
                     const char *fmt, float width, float fontPx, Clay_Color color, bool devUnbound = false);
    bool numberFieldD(const UIInput &inp, UIRenderer &ui, Clay_ElementId id, double &v, double vmin, double vmax,
                      const char *fmt, float width, float fontPx, Clay_Color color, bool devUnbound = false);
    // A slider row's -/+ step: clamped to the range, except in dev mode from a value already outside it (so a
    // typed out-of-range value can be nudged without snapping back).
    float sliderStep(float v, float delta, float vmin, float vmax) const
    {
        if (devUnboundedValues && (v < vmin || v > vmax))
            return v + delta;
        return glm::clamp(v + delta, vmin, vmax);
    }
    // Text shown as-is (a readout) that turns into a text box on click: true on commit, `out` = the typed text.
    // `editText` is what the box starts with (the readout may carry units or a hemisphere letter).
    bool inlineTextField(const UIInput &inp, UIRenderer &ui, Clay_ElementId id, const char *shown, const std::string &editText,
                         std::string &out, float width, float fontPx, Clay_Color color, const char *tip);
    char textFieldBufs_[256][128] = {};    // per-frame display strings (Clay keeps raw pointers until record)
    int  textFieldBufN_ = 0;
    const char *textFieldStr(const std::string &s);
    // A multi-line text box (2026-10-12; rebuilt 2026-10-13): the text wraps to `width`, the box grows from
    // minLines to maxLines and then scrolls (wheel, a thumb on its right edge, and it follows the caret). A click
    // puts the caret there, a drag or Shift+arrows select, a double click takes a word; Left / Right / Up / Down
    // (Ctrl: by word), Home / End, Ctrl+A / C / X / V. Enter adds a line and Ctrl+Enter / Tab / a click elsewhere
    // commit (`enterCommits`: Enter commits, Shift+Enter adds the line); Esc cancels. `readOnlyColor`: no editing,
    // no frame — a long text shown in that colour.
    // It wraps and draws its own lines (UITextBlock) and only the visible ones: nothing here goes through Clay's
    // text measurement, whose cache a long text that changes with every key press filled within a minute
    // ("Clay ran out of capacity while attempting to measure text elements").
    bool textArea(const UIInput &inp, UIRenderer &ui, Clay_ElementId id, std::string &value, float width, int minLines,
                  float fontPx, const char *placeholder, size_t maxLen = 4000, int maxLines = 10, bool enterCommits = false,
                  const Clay_Color *readOnlyColor = nullptr, int16_t layerZ = 10); // layerZ: its root's z (scroll thumb)
    struct TextAreaDraw
    {
        UITextBlock block;
        std::vector<UITextRun> runs;
        std::vector<UITextQuad> quads;
        std::string text;                  // what the runs point into (Clay keeps the block until record)
    };
    std::vector<std::unique_ptr<TextAreaDraw>> textAreaDraws_; // one per text box drawn this frame (addresses stable)
    int  textAreaDrawN_ = 0;
    std::vector<std::pair<int, int>> taLines_;                 // scratch: wrapped lines [first, last) of a text
    static void textWrapLines(const UIRenderer &ui, const std::string &s, float fontPx, float maxW,
                              std::vector<std::pair<int, int>> &lines);
    const UIRenderer *taUi_ = nullptr;     // the focused box's wrap, for Up / Down (set when it is drawn)
    float taWrapW_ = 0.0f, taFontPx_ = 12.0f;
    bool  taDragging_ = false;             // a mouse selection in the focused box
    double taClickT_ = 0.0;
    int   taClickIdx_ = -1;

    // ── UI kit (2026-10-03, SatelliteSimUI.cpp "UI kit") ───────────────────────────────────────────────────
    // The blocks every window and settings tab is built from, in the Cinematics window's style: a section is a
    // small-caps title over a 1-px rule; a readout is a label column and a value column; a stat tile is a label
    // over a large value in a dark inset; toggles, choices and buttons are 20-px pills. Ids are hashed from
    // (`key`, index), so a helper needs no hover member of its own: the previous frame's hover (for the rollover
    // sound) lives in uiHov_.
    void uiSection(const char *key, int idx, const char *title);
    void uiKV(const char *key, int idx, const char *label, const char *value, float labelW = 0.0f,
              const Clay_Color *valueColor = nullptr); // nullptr = Pal::volValue
    void uiStatTile(const char *key, int idx, const char *label, const char *value, Clay_Color valueColor);
    bool uiButton(const UIInput &inp, UIRenderer &ui, const char *key, int idx, const char *label, const char *tip,
                  bool on = false, bool enabled = true, bool grow = false);
    // A destructive action's pill (Pal::btnDanger*): `armed` = the first click of two has been made.
    bool uiDangerButton(const UIInput &inp, UIRenderer &ui, const char *key, int idx, const char *label, const char *tip,
                        bool armed, bool enabled = true, bool grow = false);
    bool uiToggleRow(const UIInput &inp, UIRenderer &ui, const char *key, int idx, const char *label, bool &v,
                     const char *tip = nullptr, const char *onText = "On", const char *offText = "Off");
    int  uiChoiceRow(const UIInput &inp, UIRenderer &ui, const char *key, int idx, const char *label,
                     const char *const *options, int count, int current, const char *tip = nullptr);
    bool uiHovRoll(uint32_t id, bool nowHov);   // rollover sound + the hover memory; returns nowHov
    std::unordered_map<uint32_t, bool> uiHov_;

    // ── Settings search (2026-10-28, issue 20261028_222345; SatelliteSimUI.cpp "Settings search") ─────────────
    // The search box over the Settings window's tab strip: with a query, the body builds every tab in turn and
    // each ROW asks searchRow() whether to draw itself. The kit rows (uiToggleRow, uiChoiceRow, uiKV, uiButton at
    // the top level), buildCloudSliderRows / Sections and the Photometry rows do it themselves, so a row added with
    // them is searchable with no extra code; a hand-built row calls searchRow(its id, its label) and skips itself,
    // and hand-built text that is not a row skips itself while searchActive(). Headings (the tab, a uiSection, a
    // slider section) are deferred and drawn above their first matching row. Outside the Settings body (other
    // windows use the same kit) nothing is filtered: `on` is set only while the body is built.
    struct SettingsSearch
    {
        std::string query;                     // as committed (the box's live text while it is focused)
        std::vector<std::string> words;        // lowercased; a row matches when every word is in its text
        bool on = false;                       // building the Settings body in search mode
        bool countOnly = false;                // an advanced tab while they are hidden: count matches, draw nothing
        int  rowDepth = 0;                     // > 0 inside a row: nested kit calls (a choice's pills) never filter
        int  tab = -1;                         // the tab being built
        bool tabShown = false;
        std::string sec;                       // the pending section heading
        bool secShown = false, secMatch = false;
        int  matches = 0, hiddenMatches = 0, hdrN = 0;
        std::unordered_set<uint32_t> seen;     // rows drawn this frame (a slot shown in two tabs is listed once)
        std::deque<std::string> text;          // this frame's heading strings (Clay keeps the pointers until record)
    } search_;
    bool searchActive() const { return search_.on; }
    int  searchClickPending_ = -1;   // a tab link clicked in the results: open it (and clear the query) after the layout
    bool uiInputLmbPressed_ = false;
    bool searchRow(uint32_t id, const char *label, const char *tip = nullptr); // true = draw this row
    void searchSection(const char *title);                                       // a heading, deferred
    bool searchMatch(const char *text) const;
    struct SearchRowScope // nested kit calls inside a row draw unconditionally
    {
        SatelliteSim &s;
        explicit SearchRowScope(SatelliteSim &ss) : s(ss) { ++s.search_.rowDepth; }
        ~SearchRowScope() { --s.search_.rowDepth; }
    };
    void buildSettingsSearchBody(const UIInput &inp, UIRenderer &ui);
    void buildSettingsTabBody(const UIInput &inp, UIRenderer &ui, int tab);
    char uiKitBufs_[160][96] = {};              // per-frame strings the kit formats (labels with suffixes)
    int  uiKitBufN_ = 0;
    const char *uiKitStr(const char *fmt, ...);

    // ── Bookmarks (2026-10-03, SatelliteSimBookmarks.cpp) ──────────────────────────────────────────────────
    // A place at a moment: the observer (ECEF direction + height), the camera, the sim time and the cloud map's
    // drift (session state, like a snapshot's view: without it a storm bookmark frames clear sky), with a
    // thumbnail of the clean frame. Stored in <user data>/bookmarks/bookmarks.json + <id>.png. Thumbnails live in
    // one 2048x2048 SRGB atlas (kBmThumbW x kBmThumbH cells), drawn through a single UIImage id with sub-rects.
    static constexpr int kBmThumbW = 256, kBmThumbH = 144, kBmAtlasW = 2048, kBmAtlasH = 2048;
    static constexpr int kBmMax = (kBmAtlasW / kBmThumbW) * (kBmAtlasH / kBmThumbH); // 8 x 14 = 112
    struct Bookmark
    {
        std::string id, name;
        double simT = 0.0;
        glm::dvec3 obsDir{0.0, 0.0, 1.0};
        float heightM = 0.0f, az = 0.0f, el = 0.0f, fov = 60.0f;
        double driftPhase = 0.0;
        float driftRate = 0.0f;
        bool hasDrift = false;
        int slot = -1;           // atlas cell, -1 = no thumbnail yet
        bool thumbTried = false; // its PNG was looked for (loaded lazily, a few per frame)
        UIImage img;             // the thumbnail's sub-rect (Clay keeps a pointer: lives in the vector)
        char meta[2][64] = {};   // "2036-11-22 02:06 UTC", "34.05 N 118.24 W  1.2 km"
        // Optional "look" (the shipped defaults, 2026-10-06): the clock a Go sets, so an event bookmark (an eclipse,
        // a flash of lightning) is not flown past at a fast time scale. -1 = leave the clock alone (user bookmarks).
        int lookTimeScaleIdx = -1;
        int lookPaused = -1;
    };
    std::vector<Bookmark> bookmarks_;
    bool bookmarksLoaded_ = false;
    WindowChrome bmChrome;
    bool hovBmClose = false, hovTimeBookmark = false;
    std::string bmNewName_;               // the name field beside "Add" (empty = "Bookmark N")
    std::string bmCaptureFor_;            // the bookmark the screenshot copy in flight is for (finalizeScreenshot)
    std::string bmPendingThumb_;          // a bookmark whose thumbnail the next clean frame provides
    int bmPendingDelay_ = 0;              // frames to wait before capturing it (a Go settles first)
    std::vector<uint8_t> bmThumbPixels_;  // the captured thumbnail (RGBA, kBmThumbW x kBmThumbH), uploaded in buildUI
    std::string bmThumbPixelsFor_;
    std::string bmDeleteArmed_, bmStatus_;
    int bmDeletePending_ = -1;            // the window's Delete / Add, applied by the next bookmarkTick (before any
    bool bmAddPending_ = false;           // layout: Clay keeps pointers into bookmarks_ until the frame is recorded)
    bool bmRestorePending_ = false;       // "Restore default bookmarks", applied the same way (it grows the vector)
    // The atlas: one image, registered once with the UI.
    VkImage bmAtlasImg = VK_NULL_HANDLE;
    VkDeviceMemory bmAtlasMem = VK_NULL_HANDLE;
    VkImageView bmAtlasView = VK_NULL_HANDLE;
    VkSampler bmAtlasSampler = VK_NULL_HANDLE;
    uint32_t bmAtlasUiId = 0;
    std::string bookmarkDir() const;
    void bookmarksLoad();
    void bookmarksSave();
    // The shipped defaults (data/bookmarks -> <exe dir>/default_bookmarks: bookmarks.json + <id>.png). They seed the
    // list when the user has no bookmarks.json yet; bookmarkRestoreDefaults re-adds any default whose id is missing
    // (copying its thumbnail) and returns how many it added, leaving the user's own bookmarks alone.
    static constexpr const char *kBmDefaultsDir = "default_bookmarks";
    int  bookmarkRestoreDefaults();
    int  bookmarkAdd(const std::string &name);           // the current view; returns its index
    void bookmarkUpdate(int i);                          // replace a bookmark's view (and thumbnail) with the current one
    void bookmarkGo(int i);
    void bookmarkDelete(int i);
    void bookmarkFormatMeta(Bookmark &b) const;
    void bookmarkRequestThumb(const std::string &id, int delayFrames = 0);
    void bookmarkCaptureThumb(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h); // finalizeScreenshot
    void bookmarkTick(UIRenderer &ui);                   // buildUI: thumbnail requests and uploads
    bool bookmarkEnsureAtlas(UIRenderer &ui);
    void bookmarkUploadThumb(int slot, const uint8_t *rgba);
    void bookmarkLoadThumb(Bookmark &b);
    int  bookmarkFreeSlot() const;
    void bookmarksDestroy(VkDevice device);
    void buildBookmarksWindow(const UIInput &inp, UIRenderer &ui);

    // ── Issue reports (2026-10-12, SatelliteSimReports.cpp) ────────────────────────────────────────────────
    // An ISSUE is a folder <user data>/reports/<id>/ holding report.json (title, description, status, shots)
    // and one full-resolution PNG per SHOT. A shot is the perf snapshot record + the harness state + the
    // camera's last seconds (the trail) + a one-line note, and its image is the frame presented right after
    // the key press: nothing is paused, settled or reset for it. Save Snapshot (F9) adds a shot to the active
    // issue, New Issue Snapshot (F7) starts another; harness `report` replays one.
    struct ReportPose
    {
        double t, lat, lon, alt, az, el, fov, simT; // t: seconds on the report clock (frame dt summed)
    };
    static constexpr double kReportTrailSeconds = 12.0;
    static constexpr double kReportTrailStepS = 1.0 / 30.0;
    std::vector<ReportPose> reportTrail_;
    double reportClock_ = 0.0;
    float  reportFrameMs_[240] = {};      // the last frames' wall dt (a ring)
    int    reportFrameN_ = 0;
    std::string reportActiveId_;          // the issue new shots go to ("" = the next shot starts one)
    int    reportShotPending_ = 0;        // 1 = a shot into the active issue, 2 = into a new issue
    bool   reportNewKeyPending = false;   // KB_REPORT_NEW
    bool   reportPendingBar_ = true;      // open the note bar for the pending shot (the harness may not)
    std::string reportPendingNote_, reportPendingTitle_;
    // The note bar shown after a shot (title + this shot's note; Enter saves, Esc closes).
    bool   reportBarOpen_ = false, reportBarFocusPending_ = false;
    int    reportBarShot_ = 0;            // 1-based shot the note field edits
    std::string reportBarTitle_, reportBarNote_;
    std::string reportDir() const;
    // What a report's texts may hold (typed, pasted or read from a file: reportSetField and reportsLoad cut to it).
    static constexpr size_t kReportTitleMax = 120, kReportNoteMax = 4000, kReportDescMax = 20000;
    ReportPose reportPoseNow() const;
    void reportTick(float dt);                            // buildUI: the trail, the keys, a pending shot
    bool reportTakeShot(bool newIssue, bool openBar);     // false = the screenshot path is busy (try next frame)
    void reportSetField(const std::string &id, int shot, const char *key, const std::string &value); // shot 0 = the issue
    void buildReportBar(const UIInput &inp, UIRenderer &ui);
    // The bar must never trap the player: it opens unfocused when a movement key or the look button is held,
    // gives the keyboard back on a right-click (look) as on a click elsewhere, and stays up unfocused for
    // reportBarTimer_ seconds (Enter or a click focuses the note). onKey asks reportBarKey first.
    float  reportBarTimer_ = 0.0f;
    bool   reportBarCloseOnCommit_ = false;   // Enter / Esc in a field: close the bar, not just the edit
    bool   reportBarKey(int key, int action);  // true = consumed

    // ── The outbox (step 2): the Issue reports window ──────────────────────────────────────────────────────
    // The list is a light copy of every report.json under reportDir() (texts, shot notes, pins: never the
    // records). It is reloaded in reportTick, BEFORE any layout, whenever reportsDirty_ is set: Clay keeps
    // pointers into it (thumbnail UIImages, strings) until the frame is recorded, so nothing may grow or shrink
    // it later in the frame — the window's New / Delete are queued in reportOp_ for the next tick.
    struct ReportShotInfo
    {
        int n = 0;
        std::string note, image;
        char meta[2][64] = {};         // "2036-11-22 02:06 UTC", "34.05 N 118.24 W  1.2 km"
        bool hasPin = false;
        float pinX = 0.0f, pinY = 0.0f; // 0..1 across the image, from its top left
        bool hasRect = false;          // an area of the frame (drag on the image): shot_NN_crop.png holds it
        float rectX = 0.0f, rectY = 0.0f, rectW = 0.0f, rectH = 0.0f;
        int imgW = 0, imgH = 0;
        int cell = -1;                 // thumbnail cell in the atlas, -1 = none yet
        bool thumbTried = false;       // its image file was read (and failed, if cell is still -1)
        bool onScreen = true;          // its thumbnail box was inside the scroll view last frame (only those get cells)
        UIImage img;
    };
    struct ReportIssueInfo
    {
        std::string id, title, description, category, status, resolution;
        char resLine[160] = {};        // "Resolved in 1a2b3c4 by <who>, 2026-10-13 04:00" (tools/harness/reports.py resolve)
        std::vector<ReportShotInfo> shots;
        char line[96] = {};            // "3 shots   clouds   open"
    };
    std::vector<ReportIssueInfo> reportIssues_;
    bool reportsLoaded_ = false, reportsDirty_ = false;
    // The report text field being edited (title, description or a shot's note), so its text is saved even when
    // the field is not drawn again to take its commit (Back, another issue, Export, the window closed):
    // reportFlushEdit writes the live (or committed, not yet applied) text into report.json.
    struct ReportEditTarget
    {
        uint32_t id = 0;               // the field's Clay id
        std::string issue, field;      // "title" / "description" / "note"
        int shot = 0;                  // n, for a note
        size_t maxLen = 0;
        bool trim = false;             // a textArea: trailing newlines / spaces dropped as it does
    } reportEditTarget_;
    void reportNoteEditTarget(uint32_t id, const std::string &issue, int shot, const char *field, size_t maxLen, bool trim);
    void reportFlushEdit();
    WindowChrome rpChrome;
    bool hovRpClose = false, hovTimeReport = false;
    std::string reportViewId_;          // the issue the window shows ("" = the list)
    int  reportViewShot_ = 0;           // its shot in the large preview (n)
    std::string reportPreviewKey_;      // "<id>/<n>" the atlas's preview region holds
    bool reportPreviewOk_ = false;
    UIImage reportPreviewImg_;
    std::string reportDeleteArmed_, reportStatus_;
    struct ReportOp
    {
        int kind = 0;                   // 1 delete issue, 2 delete shot, 3 new issue from this view
        std::string id;
        int shot = 0;
    } reportOp_;
    std::string reportThumbPath_;       // finalizeScreenshot: also write this frame's thumbnail here
    // One atlas, registered once with the UI: a 1024 x 576 preview at its top left, thumbnail cells in the rest.
    static constexpr int kRpAtlas = 2048, kRpPrevW = 1024, kRpPrevH = 576, kRpThumbW = 256, kRpThumbH = 144;
    static constexpr int kRpCells = 16 + 80; // 4 x 4 right of the preview, 8 x 10 below it
    struct ReportCell
    {
        std::string key;                // "<id>/<n>"
        uint64_t used = 0;              // reportCellClock_ when last drawn
    } reportCells_[kRpCells];
    uint64_t reportCellClock_ = 0;
    VkImage rpAtlasImg = VK_NULL_HANDLE;
    VkDeviceMemory rpAtlasMem = VK_NULL_HANDLE;
    VkImageView rpAtlasView = VK_NULL_HANDLE;
    VkSampler rpAtlasSampler = VK_NULL_HANDLE;
    uint32_t rpAtlasUiId = 0;
    void reportsLoad();
    ReportIssueInfo *reportFind(const std::string &id);
    void reportWriteThumb(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h); // finalizeScreenshot
    bool reportEnsureAtlas(UIRenderer &ui);
    void reportUpload(int x, int y, int w, int h, const uint8_t *rgba);
    void reportThumbTick(UIRenderer &ui);                 // lazy thumbnails + the preview, a file per frame
    void reportGo(const std::string &id, int shot, bool withSettings);
    void reportSetPin(const std::string &id, int shot, bool set, float x, float y);
    // The area of a shot's frame the problem is in (0..1 from the image's top left), and its full-resolution
    // crop beside the shot's PNG (shot_NN_crop.png: what a reader of the report looks at first).
    void reportSetRect(const std::string &id, int shot, bool set, float x, float y, float w, float h);
    bool  rpMarkDrag_ = false;          // a press on the shot's frame: a click = the pin, a drag = the area
    float rpMarkX0_ = 0.0f, rpMarkY0_ = 0.0f;
    TextAreaDraw rpMarks_;              // the pin, the area and the rubber band, drawn over the frame
    void reportDeleteIssue(const std::string &id);
    void reportDeleteShot(const std::string &id, int shot);
    void reportsDestroy(VkDevice device);
    // Step 3: one file to send. <user data>/report_exports/<id>.zip (stored, not compressed: the PNGs already
    // are) = the issue's folder + log_tail.txt (the session log's end, the exe / user folders redacted).
    // Returns the zip's path, "" on failure (reportStatus_ says why). An open issue becomes "sent".
    std::string reportExportZip(const std::string &id);
    void buildReportsWindow(const UIInput &inp, UIRenderer &ui);
    // applySettingsJson's range guards. A report replay sets settingsRawImport_, so a value typed past its
    // range in dev mode (devUnboundedValues) comes back as it was saved.
    bool settingsRawImport_ = false;
    template <class T>
    T settingClamp(T v, T lo, T hi) const { return settingsRawImport_ ? v : std::clamp(v, lo, hi); }

    // ── HUD text entry (2026-10-03): the time bar's UTC clock, latitude, longitude and altitude ────────────
    bool setSimTimeFromText(const std::string &text);    // "2036-11-22 02:06", "02:06:30" (today), ISO; false = unparsed
    bool setLatLonFromText(const std::string &text, bool isLat); // "34.05", "34.05 S", "34.05, -118.24"
    bool setAltitudeFromText(const std::string &text);   // in the HUD's unit (km / mi), or with m/km/ft/mi
    void setObserverLatLon(double latDeg, double lonDeg);

    // ── ECI → ENU rotation (updated each frame in updatePositions) ────────────
    // Encodes the surface-fixed observer's local frame in ECI coordinates.
    glm::vec4 eci2enuX{1, 0, 0, 0}; // East  basis in ECI
    glm::vec4 eci2enuY{0, 1, 0, 0}; // North basis in ECI
    glm::vec4 eci2enuZ{0, 0, 1, 0}; // Up    basis in ECI

    // ── Sun + observer state (updated each frame in updatePositions) ──────────
    glm::vec3 sunDirECI{1, 0, 0};    // unit vector from Earth toward Sun in ECI
    glm::vec4 sunDirENU{0, 1, 0, 0}; // sun direction in ENU (xyz), w = sin(elevation)

    // ── Refraction, mirages and optical turbulence (2026-10-26, SatelliteSimRefraction.cpp, Refraction.h) ──
    bool  refrEnabled = true;       // refraction.enabled: the Sun's disc and the stars bent by the air's profile
    float refrMirage = 1.0f;        // refraction.mirage: x the surface layer's and the inversion's temperature steps
    float refrTurbulence = 1.0f;    // refraction.turbulence: x Cn^2 (shimmer, the stars' twinkle and jitter)
    float refrDispersion = 1.0f;    // refraction.dispersion: x the physical dispersion (the green rim)
    float sunDiscGain = 1.0f;       // refraction.sun_disc_gain: x the Sun disc's physical radiance
    float refrForceDt = -100.0f;    // refraction.force_sea_air_dt: < -50 = from place and time (K)
    float refrForceInvZ = -1.0f;    // refraction.force_inversion_m: < 0 from place and time, 0 none, > 0 its height (m)
    float refrForceInvDT = 2.0f;    // refraction.force_inversion_dt (K, with force_inversion_m > 0)
    float refrForceInvDz = 15.0f;   // refraction.force_inversion_dz (m)
    refr::Table refrTable;
    refr::Profile refrProfileUsed;
    std::future<refr::Table> refrJob;
    refr::Profile refrJobProfile;
    std::chrono::steady_clock::time_point refrLastStart{};
    std::vector<glm::dvec2> refrInv;     // (true, apparent) elevation of the primary image, ascending
    GpuRefraction refrGpu{};
    double refrSunR = 0.0, refrSunVis = 1.0;
    int sunspotGroups = 0;   // fillSunspots: the groups sent this frame
    std::atomic<float> refrBuildMs{0.0f};
    std::atomic<int> refrBuilds{0};
    refr::Profile refrProfileAt(double simT) const;
    void updateRefraction();
    void fillSunspots(double simT);
    void fillCorona(double simT);          // SatelliteSimCorona.cpp: refrGpu.cor* (the corona's structure now)
    nlohmann::json coronaJson() const;     // harness `corona`
    int coronaStreamers = 0, coronaCmes = 0;
    // The coronagraph's radial display lift (the inertial lock's occulter): x r^this, as coronagraph images are
    // displayed (the K corona falls ~1000x from 2 to 9 radii). settings photometry.corona_display_lift (no UI).
    float coronaDisplayLift = 1.2f;
    void refrDerive();                          // the inverse map and the Sun's quantities, from refrTable
    bool refractEnu(glm::vec3 &enu, float &aoa, float &scint) const;   // a source at infinity: false = below the apparent horizon
    nlohmann::json refractionJson() const;
    glm::vec3 obsECI{0, 0, 6371000}; // observer ECI position (meters)

    // ── Planets (updated each frame in updatePositions/updatePlanets) ─────────
    PlanetState planetStates[kPlanetCount]{};                                // ephemeris; direction/distance/phase
    bool planetEnabled[kPlanetCount] = {true, true, true, true, true, true}; // per-planet toggle
    bool showPlanets = true;                                                 // global toggle, settings-persisted
    bool planetDiscs = true;          // resolved planet discs (settings planets.resolved_discs)
    float planetDiscGain = 1.5f;      // x their physical surface brightness (planets.disc_gain)
    PlanetView planetView[kPlanetCount]{};
    GalileanState galilean[kGalileanCount]{};
    int selectedPlanetIndex = -1;                                            // index into planetStates[]/kPlanetNames[], -1 = none selected;
                                                                             // mutually exclusive with selectedSatIndex (see its declaration)

    // ── Sky-background sun-glare gate (hysteresis state, not persisted) ───────
    // Eased toward its per-frame target in recordCompute(), right after updatePositions() and
    // before updateStars(). Consumed by updateStars() (folded into nightFactorEff) and pushed to
    // sat_sky.frag via SatDrawPC for the Milky Way. See recordCompute() for the target/easing
    // logic and rationale (asymmetric fast-dim/slow-recover rates).
    float skyGlareEased = 1.0f;

    // ── TargetedReflector ground targets ──────────────────────────────────────
    // S1 (RELEASE_v1_1_PLAN.md): real solar-farm sites loaded from reflector_targets.json (falls
    // back to random points — see loadReflectorTargets()), stored as unit ECEF vectors. Rotated to
    // ECI each frame in updatePositions; filtered to those on the night side.
    // kNumReflectorTargets is a CAPACITY (buffer sizing), not the real count — see
    // reflectorTargetCount below. Modders can supply anywhere up to this many sites.
    static constexpr int kNumReflectorTargets = 201;
    // Real number of loaded targets this run (<= kNumReflectorTargets); set by loadReflectorTargets()
    // /generateReflectorTargetsRandomFallback(). Only [0, reflectorTargetCount) of the arrays below
    // are meaningful — entries beyond it are zero-initialized and never read.
    int reflectorTargetCount = 0;
    // Index into the loaded array that's the observer-spawn pin (reflector_targets.json's
    // "observer_spawn": true entry, or index 0 in the random fallback) — -1 if somehow neither
    // path set one. Purely informational/logging today; the pin works simply by being a real,
    // correctly-populated entry like any other (see loadReflectorTargets()'s doc comment for the
    // bug this replaced: index 0 used to be silently left as a degenerate zero-vector).
    int reflectorObserverSpawnIdx = -1;
    // reflectorActiveCount (S1's per-frame night-side compaction count) was removed 2026-08-06 —
    // sat_orbit.comp now scans all reflectorTargetCount targets itself each frame, gating night-
    // side per-candidate from the live/eval-time sun direction instead of relying on a CPU-
    // precompacted subset. See sat_orbit.comp's TargetedReflector block and CLAUDE.md.
    glm::vec3 reflectorTargetsECEF[kNumReflectorTargets]{}; // unit ECEF, set by initConstellation
    // Real ground radius per target (C12 follow-up #18) — kEarthRadius + actual terrain elevation
    // at that target's lat/lon, looked up once via earthElevCpu when targets are generated
    // (buildOrbits() runs after createGlowResources() has loaded earthElevCpu — see init()'s call
    // order). Fixes targets on any elevated terrain (mountains, plateaus) being placed at the
    // sea-level sphere, which put the "ground" endpoint of every beam-related ray for that target
    // underground. Defaults to kEarthRadius (sea level) if earthElevCpu isn't available for
    // whatever reason. Consumed by updatePositions() in place of the bare kEarthRadius constant
    // when converting reflectorTargetsECEF to a real ECI position.
    float reflectorTargetsRadiusM[kNumReflectorTargets]{};
    int beamSiteConverged[kNumReflectorTargets]{}; // converged beams per site, last beam readback (harness `beams`)
    glm::dvec3 beamSiteDirEcef[kNumReflectorTargets]{}; // their summed ground->satellite directions (ECEF)
    // Per-site local ENU frame (2026-08-12), computed once alongside the radius above and never
    // changed after — a target site is fixed in ECEF, so its own local East/North/Up frame is too.
    // Used ONLY by the cloud-light build's direction bucketing: quantizing a beam's approach
    // direction in the SITE's frame (rather than the observer's) makes a beam's bucket a pure
    // function of its own geometry — observer-independent, so panning/flying the camera cannot
    // repartition a cluster. Purely an internal quantization frame, so it deliberately does NOT
    // have to match terrain.glsl's enuBasis() convention; unlike that one it guards the polar
    // degeneracy, since a NaN here would silently poison a bucket index.
    glm::vec3 reflectorSiteEnuX[kNumReflectorTargets]{};
    glm::vec3 reflectorSiteEnuY[kNumReflectorTargets]{};
    glm::vec3 reflectorSiteEnuZ[kNumReflectorTargets]{};
    // What each target is FOR (2026-10-02, reflector_targets.json "kind"): only Solar is drawn on the
    // terrain (a PV array, see GpuSolarSite); Agriculture and Daylight (polar towns) are plain ground.
    enum class ReflectorKind : uint8_t { None, Solar, Agriculture, Daylight };
    ReflectorKind reflectorKind[kNumReflectorTargets]{};
    double reflectorLatDeg[kNumReflectorTargets]{}, reflectorLonDeg[kNumReflectorTargets]{};
    float  reflectorSolarAreaM2[kNumReflectorTargets]{};   // panel-park area (capacity x 2.2 ha/MW, or area_km2)
    bool   reflectorSolarTracker[kNumReflectorTargets]{};  // single-axis N-S trackers, else fixed tilt
    float  reflectorTrackerAngle[kNumReflectorTargets]{};  // eased tracker angle (rad, + = toward east)
    bool   solarArraysEnabled = true;                      // settings: terrain.solar_arrays

    // Mirror slew rate for TargetedReflector: maximum degrees the mirror normal
    // may rotate per real second.  Prevents instant snapping when the nearest
    // valid target changes (e.g. a target crosses into daylight and a different
    // one takes over).  The mirror physically slews toward the goal direction.
    static constexpr float kMirrorRotRateDegPerSec = 1.0f;

    // Maximum body roll for KnifeEdge attitude (degrees from nadir-pointing).
    // Real Starlink solar panels counter-rotate around the along-track axis to
    // compensate body roll.  At 80° the panels still receive ~17% of peak
    // irradiance; beyond this the gimbal runs out of range and power drops
    // sharply.  Limits knife-edge effectiveness when the geometry demands >80°.
    static constexpr float kKnifeMaxRollDeg = 80.0f;

    // Per-satellite current mirror normal in ECI (TargetedReflector only).
    // satMirrorNormals (CPU-era placeholder for GPU mirror slew state) and mirrorSnapFrames/
    // requestMirrorSnap() (forced every TargetedReflector mirror to its ideal attitude for a few
    // frames after an event that invalidated persisted GPU lock/slew state) were removed
    // 2026-08-06 — the reversibility rework deleted the persisted state itself, so there is
    // nothing left to snap out of. See sat_orbit.comp's TargetedReflector block.

    // ── Satellite type catalogue (defined once in initConstellation) ──────────
    std::vector<SatelliteType> satTypes;

    // ── Orbital parameters (fixed at init, positions computed by GPU) ─────────
    std::vector<ConstellationConfig> constellations;
    SatOrbitTable satOrbits; // (the pool's entries made on read from kPool: SatOrbitTable)

    // Re-bake satOrbitBuf when the sim has advanced more than this many days from
    // the baked epoch, keeping float deltaT < 7×86400 = 604800 s (float ULP ≈ 0.07 s).
    static constexpr int64_t kOrbitRebakeDays = 7;
    // Epoch at which satOrbitBuf was last baked (two-part, matches simTime representation).
    // uploadSatOrbits() re-bakes if |simDayJ2000 - orbitEpochDay| > kOrbitRebakeDays.
    int64_t orbitEpochDay = 0;
    double orbitEpochSec = 0.0;

    // ── Gamepad state (Xbox controller support, works the same over Bluetooth or USB —
    //    Windows exposes both as an XInput device, which GLFW 3.4's joystick backend already
    //    talks to) ────────────────────────────────────────────────────────────────
    // GLFW joystick id of the active gamepad; -1 = none connected. Re-scanned in pollGamepad()
    // whenever it goes stale (disconnect), so plug-in/plug-out works without a restart.
    int gamepadId = -1;
    GLFWgamepadstate gpState{};                                     // last frame's full state (for held-button checks in recordCompute)
    unsigned char prevGpButtons[GLFW_GAMEPAD_BUTTON_LAST + 1] = {}; // previous frame's buttons, for edge detection

    // Gamepad rebind-capture anti-self-satisfy guard (see pollGamepad's rebind-capture block).
    // The "Bind Pad" click that sets some binding's listeningPad=true happens via the SAME A
    // press pollGamepad's own edge-detect loop would otherwise see as "a fresh button press" one
    // function call later in the same frame — without this, clicking "Bind Pad" with A instantly
    // self-captured A as the new binding, before the player ever got a chance to press anything
    // else. gpRebindListenIdx tracks which keybinding index the snapshot below belongs to (-1 =
    // none); gpRebindHeldAtStart snapshots which buttons were already down the moment that listen
    // session began, and each stays ineligible for capture until seen released at least once.
    int gpRebindListenIdx = -1;
    bool gpRebindHeldAtStart[GLFW_GAMEPAD_BUTTON_LAST + 1] = {};
    float gpMoveFwd = 0.0f, gpMoveRight = 0.0f;       // left stick, deadzoned, [-1,1] — combines additively with WASD
    float gpLookYawDeg = 0.0f, gpLookPitchDeg = 0.0f; // right stick, this-frame look delta in degrees (already dt-scaled)
    // Analog triggers for elevation — deliberately NOT part of the keybindings/gpButton
    // rebind system (triggers are axes, not digital buttons; same reasoning as WASD/sticks
    // not being rebindable). [0,1] pressure, combined via max() with the (still rebindable)
    // digital KB_RAISE_ELEV/KB_LOWER_ELEV state in recordCompute's elevation block, so
    // "pressure corresponds to vertical speed."
    float gpElevRaise = 0.0f, gpElevLower = 0.0f;

    // UC4: which input device produced the most recent activity — set true by pollGamepad() on
    // any stick deflection/trigger pressure/button press, set false by onKey() (any keypress) and
    // buildUI() (mouse click/drag/scroll). Not persisted (a per-session UI nicety, not a
    // preference). Read by buildViewControlsBody() to lead with whichever device the player is
    // actually holding, instead of always listing keyboard first — see RELEASE_v1_1_PLAN.md UC4.
    bool lastInputWasGamepad = false;

    // ── UC4/UC5: gamepad virtual cursor ("cheap 90%" UI navigation) ────────────
    // Explicitly toggled by KB_TOGGLE_CURSOR (default: Menu/Start), not merely "a UI window is
    // open" — the earlier automatic version hijacked the right stick the instant Settings/Controls
    // was open, which fought free-look and dropped new gamepad players straight into cursor mode
    // on first launch (see RELEASE_v1_1_PLAN.md UC5). While the toggle is off, the right stick
    // always drives camera look, UI visible or not. vCursorX/Y start at -1 as a "not yet
    // positioned" sentinel; first activation centers on screen.
    float vCursorX = -1.0f, vCursorY = -1.0f;
    bool vCursorToggled = false; // player-facing on/off state, flipped by KB_TOGGLE_CURSOR
    bool vCursorActive = false;  // this-frame effective state = vCursorToggled && uiVisible && !showIntro
    bool vCursorClick = false;   // A button currently held (level state, like the real lmb App.cpp
                                 // reads from GLFW) — UIRenderer::beginFrame() does its own frame-to-
                                 // frame edge detection into UIInput::lmbPressed, same as the mouse path

    // ── Mouse state / window handle ───────────────────────────────────────────
    GLFWwindow *win = nullptr;
    int windowedX = 100, windowedY = 100;  // saved windowed position (for restore)
    int windowedW = 1280, windowedH = 720; // saved windowed size (for restore)
    bool firstMouse = true;
    double prevX = 0, prevY = 0;
    float dmx = 0, dmy = 0; // accumulated delta for this frame
    // Cinematic drift mode (toggled by KB_CINEMATIC while RMB is held).
    // Mouse adds force to velocity instead of directly rotating; velocity coasts and decays.
    // Mode is cleared automatically when RMB is released.
    bool cinematicMode = false;     // toggle state
    float cinematicYawVel = 0.0f;   // pixels-equivalent/s driving Rodrigues yaw
    float cinematicPitchVel = 0.0f; // pixels-equivalent/s driving elDeg pitch
    bool cinematicActive = false;   // true last frame — used to detect transition-out

    // Fine (slow) movement mode — flipped by KB_MOVE_FINE (dispatchKeyAction). A toggle rather
    // than a held modifier (unlike KB_MOVE_BOOST): holding a stick-click down while also using
    // that same stick to move is uncomfortable, so a single press latches slow movement on/off.
    // Cleared whenever KB_MOVE_BOOST (sprint) is active — the two are contradictory intents.
    bool fineMoveToggled = false;

    // Look-axis inversion (persisted, Settings > Controls). Applied where the frame's mouse
    // dmx/dmy and the gamepad right-stick look deltas are CONSUMED, not at the input callbacks —
    // so the raw accumulators and the gamepad virtual-cursor path (which reuses the right stick)
    // stay untouched.
    bool invertMouseX = false;
    bool invertMouseY = false;
    bool invertPadX = false;
    bool invertPadY = false;

    // ── UI hover state (one-frame lag) ────────────────────────────────────────
    std::vector<bool> hovConst;           // one entry per constellation; sized in loadDefinitions()
    std::vector<bool> hovHighlightConst;  // highlight button hover state, parallel to hovConst
    // Settings > Constellations (2026-10-20): session state of the grouped list.
    std::unordered_map<std::string, bool> constGroupOpen_; // by category; absent = folded
    int constListOpen_ = -1;                                // the List entry whose members are shown
    int constListPage_ = 0;
    std::string constListFilter_;
    std::deque<std::string> constUiText_;                   // this frame's formatted strings (Clay keeps pointers)
    bool hovShowPlanets = false;          // "Show planets" global toggle hover state
    bool hovPlanetBtn[kPlanetCount] = {}; // per-planet ON/OFF toggle hover state
    bool hovTimeSlower = false;
    bool hovTimePause = false;
    bool hovTimeFaster = false;
    bool hovTimeReverse = false;
    bool hovScreenshot = false; // UC6: left HUD panel camera button
    bool hovPhoto = false;      // HQ photo button beside it
    bool hovSettings = false;
    bool hovSettingsClose = false;
    bool hovAltModeToggle = false;
    bool hovViewControlsClose = false;
    bool hovUnitMetric = false;
    bool hovUnitImperial = false;
    bool hovOpenControlsWindow = false; // Controls tab's "Open Controls Reference" button
    bool hovInvertMouseX = false, hovInvertMouseY = false; // Controls tab look-invert toggles
    bool hovInvertPadX = false, hovInvertPadY = false;
    bool hovTab[17] = {}; // one per settings-window tab (kSettingsTabNames: kSettingsTabCount)
    bool hovScaleMinus = false;
    bool hovScalePlus = false;
    bool hovRenderScaleMinus = false;
    bool hovRenderScalePlus = false;
    bool hovTrailsBtn = false; // "Star Trails" HUD icon-button hover state (buildLeftHudPanel)
    bool hovMasterVolMinus = false;
    bool hovMasterVolPlus = false;
    bool hovMusicVolMinus = false;
    bool hovMusicVolPlus = false;
    bool hovSfxVolMinus = false;
    bool hovSfxVolPlus = false;
    bool hovAmbVolMinus = false;
    bool hovAmbVolPlus = false;
    bool hovMusicBtn[3] = {}; // Sound tab player: prev / pause-play / next
    bool hovRebind[KB_COUNT] = {};    // per keybinding row — sized to match keybindings vector
    bool hovRebindPad[KB_COUNT] = {}; // per keybinding row, gamepad-button rebind button
    bool hovFullscreen = false;
    bool hovSaveSnapshot = false;
    float snapshotMsgTimer = 0.0f; // seconds remaining to show "Saved" confirmation on the perf snapshot button
    bool hovResetDefaults = false;
    float resetDefaultsMsgTimer = 0.0f;          // seconds remaining to show the "Restart to apply" confirmation (NEW-5)
    bool hovDebugToggle[kDebugToggleSlots] = {}; // one per knockout checkbox — see kDebugToggles
                                                 // (top of SatelliteSimUI.cpp) for the row list
    bool hovBeamDebugRaysToggle = false;         // hover state for the "Show beam pointing rays" checkbox (C12 follow-up #12)
    bool hovBeamDebugRaysToggleQuick = false;    // hover state for the Display-tab quick-access copy
                                                 // of the same checkbox, next to Graphics preset
                                                 // (2026-08-06) — separate bool because it's a second,
                                                 // simultaneously-visible Clay element bound to the
                                                 // same showBeamDebugRays bool; sharing one hover bool
                                                 // between two elements would fight over it.
    // Sized 35, not 33 — the highest Photometry idx in use is 34 ("Glare near range (km)";
    // "Glare near gain" is 33, added after the [33] resize and never followed up). While it was
    // [33] an idx-33/34 write landed in the NEXT array's [0] (hovPhotoMinus[33] -> hovPhotoPlus[0],
    // hovPhotoPlus[33] -> draggingPhoto[0], draggingPhoto[33] -> hovCloudMinus[0]), i.e. the
    // near-glare rows' hover flags fought the first rows' and their drag lock could grab the wrong
    // slider — same class of silent OOB as [[feedback_cloud_slider_arrays]], found and corrected
    // 2026-09-26. Per that memory, all three arrays grow together with any new slider id.
    // idx map: 0-10 the original photometry params + Trail decay/gain, 11-18 dark-sky mags + MW
    // fades, 19/20 trail sliders, 21 flare-mitigation tilt, 22-26 the point model, 27 zoom
    // aperture, 28-32 glare, 33/34 glare near.
    // [38] since 2026-10-07 (35-37: resolved satellites, satellite adaptation, formation merge; photoBufs too).
    // [39] since 2026-10-20 (38: glare cluster merge).
    // [45] since 2026-10-28 (39-44: the Milky Way's look; photoBufs too).
    // [56] since 2026-10-28 (45-53: the Milky Way's zoomed star clouds; photoBufs too).
    bool hovPhotoMinus[56] = {};
    bool hovPhotoPlus[56] = {};
    bool draggingPhoto[56] = {};
    // One slot count for all four per-slider arrays (and cloudBufs in buildCloudSliderRows), so they
    // cannot drift apart again. 112-151: the clouds v2 sliders (2026-09-27).
    static constexpr int kCloudSliderSlots = 428;   // 400-427 (2026-10-28): the settings round-up (Sky tab, Weather fires / haboobs, ocean motion filter, plume dispersal, TAA weights); was 357;   // 355-356 (2026-10-27): Moon relief / shadows; 354 (2026-10-27): the Sun's sea glitter; 353 (2026-10-26): the ground's light under cloud; 352 (2026-10-26): the sky's multiple scattering; 349-351 (2026-10-26): the tonemap; 346-348 (2026-10-26): the sky's light on the clouds; 342-345 (2026-10-26): delta-Eddington multiple scattering; 341: terrain shadows at half res; 339-340 (2026-10-25): smooth tiles; 330-334 (2026-10-23): rain / snow motion (fall, surface wind, storm outflow,
    // gust fronts, drop persistence); 326: "Tropical cyclones (x)"; 325: "Storm complexes"; 321-324 (2026-10-23): storm
    // objects (frequency, life, cloud field, moat);
    // was 321: 313-320 (2026-10-21, merged 10-22): the exhaust sliders (Launches);
    // was 313: 312 (2026-10-22): Sun shadow volume (S3); 310-311: anvil puffiness, anvil top lift;
    // was 310: 302-309 (2026-10-22): the cloud rebuild's experiments (Weather tab)
    // was 302: 296-297 (2026-10-14): layer shadows, cirrus self-shadow; 298-301 aurora storms
    // was 296:   // 289-295 (2026-10-13): the cumulus shape
    // was 289:   // 286-288 (2026-10-12): cloud shadow rings; 270-282 (2026-10-11): small cumulus, erosion / lobe variation, mid layer x5, cirrus warp, sampler x3, far shadow range
    // (was 270:)   // 261-266 rain particles (count, shutter, fall speed, wind, storm wind, gusts)
    bool hovCloudMinus[kCloudSliderSlots] = {}; // was [88] — idx 88/89 are the zodiacal light gain/width sliders,
                                 // idx 90 the ocean Milky Way reflection gain (2026-09-08),
                                 // idx 91-96 the terrain detail sliders, 97/98 terrain erosion (2026-09-25),
                                 // idx 110/111 the satellite ocean-glint gain/floor (2026-09-26, a
                                 // reuse of the last free slots — the Sound tab's are 99-108)
    bool hovCloudPlus[kCloudSliderSlots] = {};
    bool draggingCloud[kCloudSliderSlots] = {}; // MUST stay sized to match hovCloudMinus/Plus — see
                                 // feedback_cloud_slider_arrays memory: this one was missed once
                                 // already and the out-of-bounds write corrupted the window-chrome
                                 // state declared right below, breaking the settings window.
    // Collapsible section state for the Clouds tab (see buildCloudSliderSections). Indexed by a
    // section's position in its tab's own section array, NOT by slider idx — sections are a pure
    // presentation grouping and own no slider state. Sized 24 (capacity) so adding a category
    // needs no array edit; kCloudSectionSlots is asserted against in buildCloudSliderSections.
    static constexpr int kCloudSectionSlots = 48;   // Clouds 0-11, Weather 12-23, Atmosphere 24-29, Terrain 30-35
    bool cloudSectionOpen[kCloudSectionSlots] = {}; // all collapsed on open — the point of the grouping
    bool hovCloudSection[kCloudSectionSlots] = {};

    // ── Window chrome (drag+resize; see UIRenderer::WindowChrome) ──────────────
    // x/y default to -1 (uninitialized, centers/places on first open); w/h are set
    // once by the owning builder before the first updateWindowChrome() call.
    // Only the two real windows (Settings, View Controls) have chrome — the left/right
    // HUD panels are fixed to their screen corner (see buildLeftHudPanel/buildRightHudPanel),
    // recomputed from screenW/H every frame so they track window resizes automatically.
    WindowChrome settingsChrome;
    WindowChrome viewControlsChrome;
    int settingsActiveTab = 0; // index into kSettingsTabNames (persisted; clamped on load)

    // ── Right HUD panel: altitude display mode + unit system ──────────────────
    bool altModeSeaLevel = true;                // true = MSL (sea level), false = AGL (above terrain)
    UnitSystem unitSystem = UnitSystem::Metric; // Display tab setting; affects altitude readout

    // ── NEW-7: frame limiter ────────────────────────────────────────────────
    FpsCapMode fpsCapMode = FpsCapMode::VSync;  // Display tab setting; see FpsCapMode comment
    bool fpsCapSwapchainRebuildPending = false; // set true when fpsCapMode changes; see
                                                // consumeSwapchainRebuildRequest() override
    bool hovFpsCap[5] = {};                     // one per FpsCapMode button, same order as the enum

    // ── UC1: graphics preset ─────────────────────────────────────────────────
    // Default High until init() either loads a persisted value or (first run only) seeds one from
    // VkPhysicalDeviceProperties::deviceType — see seedGraphicsPresetFromDevice() in init().
    GraphicsPreset graphicsPreset = GraphicsPreset::High;
    // Display tab "Dev mode: unbounded values" (display.dev_unbounded_values): a number typed into a settings
    // slider's value box (the Clouds-style rows and the Photometry rows) is taken as typed, outside the slider's
    // range. Dragging still stays in range. Nothing downstream is guaranteed to cope with such a value.
    bool devUnboundedValues = false;
    bool showAdvancedSettings = false; // Display tab "Show advanced settings" — reveals the
                                       // Clouds/Ocean/Terrain/Aurora tabs; off by default so a
                                       // new user's front door is Preset + the handful of
                                       // top-level controls, not 46 developer sliders.
    bool hovPreset[6] = {};            // one per clickable preset button (Custom has no button —
                                       // it's a read-only status, not a click target; Potato does)
    float fpsBadgeEma = 0.0f;          // EMA-smoothed 1/dt for the HUD fps badge, so a true low
                                       // frame rate reads as a steady number instead of flickering
                                       // between adjacent integers frame to frame
    bool hovAdvancedToggle = false;
    bool hovReplayIntro = false; // UC3 "Replay Intro" button, Display tab
    // UC3 follow-up: "Play intro on startup" checkbox, Display tab. Persisted (loadSettings/
    // saveSettings) and applied to showIntro at load time. Default true so a genuine first run
    // (no settings.json at all — loadSettings() returns before reaching this key) still opens with
    // the cinematic; an existing settings.json missing this key (i.e. upgrading from a build that
    // predates it) defaults to false at load time instead, so upgrading players don't suddenly get
    // a cinematic that didn't exist in their version — see loadSettings().
    bool playIntroOnStartup = true;
    bool hovPlayIntroStartup = false;

    // ── Startup graphics chooser (2026-10-06, SatelliteSimBootChooser.cpp) ──────────────────────
    // display.ask_graphics_mode: show the chooser on the loading screen before init() (absent = true).
    bool askGraphicsModeOnStartup = true;
    int bootChoiceMode = -1;      // the option picked this launch: 0 Full, 1 Planetarium, 2 Potato; -1 none shown
    bool bootChoiceAsk = true;    // the chooser's "ask on every startup" box (applied after loadSettings)
    // A cheap look at settings.json + the crash sentinel BEFORE init (peekBootSettings): what the chooser
    // pre-selects, and which tier init() should build pipelines for.
    bool bootPeekDone = false, bootPeekHasFile = false, bootPeekCrash = false, bootPeekAsk = true;
    int bootPeekPreset = -1;      // display.graphics_preset as saved (-1 = none)
    uint32_t bootPeekMask = 0;    // display.debug_disable_mask as saved
    // Tier pipeline deferral: on a light tier (Planetarium / Potato) the three FULL sat_sky.frag
    // pipelines (the inline sky, the low-res prepass, the SKY_TAA pass) are not created at init or on a
    // resize; ensureFullSkyPipelines() creates them the first frame anything needs them.
    bool fullSkyDeferred = false;
    // The safety net: the first seconds of frames after loading, wall clock (updateBootSafetyNet).
    bool bootSafetyDone = false;
    double bootSafetyT0 = 0.0, bootSafetyPrevT = 0.0;
    int bootSafetyWarm = 0;
    std::vector<float> bootSafetyMs;
    void peekBootSettings();
    int deviceRecommendedBootMode(VulkanContext &ctx) const; // 0 Full / 1 Planetarium / 2 Potato — see the .cpp
    GraphicsPreset recommendedFullPreset(VulkanContext &ctx) const;
    void applyBootChoice(VulkanContext &ctx, bool crashDetected);
    bool fullSkyNeeded() const;
    void ensureFullSkyPipelines(VulkanContext &ctx);
    void updateBootSafetyNet();
    bool hovRootFollowMusic = false;
    bool hovSatOcclusionChk = false, hovEnvReflChk = false, hovSharpReflChk = false;

    // ── Private helpers ───────────────────────────────────────────────────────
    // NEW-7: pushes fpsCapMode's present-mode requirement into VulkanContext and flags App to
    // rebuild the swapchain with it (see consumeSwapchainRebuildRequest() above). Called both from
    // the Settings > Display button row and once after loadSettings() if the persisted mode isn't
    // the VulkanContext default (VSync/FIFO).
    void applyFpsCapMode()
    {
        if (!ctx_)
            return;
        switch (fpsCapMode)
        {
        case FpsCapMode::VSync:
            ctx_->presentModePreference = VK_PRESENT_MODE_FIFO_KHR;
            break;
        case FpsCapMode::Off:
            ctx_->presentModePreference = VK_PRESENT_MODE_IMMEDIATE_KHR;
            break;
        default:
            ctx_->presentModePreference = VK_PRESENT_MODE_MAILBOX_KHR;
            break; // Cap30/60/120
        }
        if (ctx_->presentModePreference != ctx_->presentModeBuilt)
            fpsCapSwapchainRebuildPending = true; // (only a change: 2026-10-21)
    }
    void createBuffers(VulkanContext &ctx);
    // Frames in flight (VulkanContext "Commands & sync"): a buffer the CPU writes and the GPU reads. Returns the
    // pointer to write (the mapped memory with one frame in flight; with two, a shadow copied into the buffer —
    // then device-local — at the start of each frame's GPU work). `whole`: copied whole each frame, else only
    // what ctx.mirrorDirty marks. Destroy with destroyMirrored.
    void *createMirrored(VulkanContext &ctx, VkDeviceSize size, VkBufferUsageFlags usage, VkBuffer &buf,
                         VkDeviceMemory &mem, bool whole, const char *name);
    void destroyMirrored(VulkanContext &ctx, VkBuffer &buf, VkDeviceMemory &mem, void *&mapped);
    // The GPU -> CPU copies of the frame (readbacks, VulkanContext::readbackCreate), recorded at the end of
    // recordCompute: the buffers the GPU wrote this frame into this slot's host copies.
    void recordFrameReadbacks(VkCommandBuffer cmd, VulkanContext &ctx);
    void createSatBuffers(VulkanContext &ctx); // per-satellite/per-type buffers; after initConstellation()
    void createDescriptors(VulkanContext &ctx);
    void createOrbitDescriptors(VulkanContext &ctx);
    void createOrbitPipeline(VulkanContext &ctx);
    void uploadSatOrbits(VulkanContext &ctx);
    bool orbitsBuiltOnce = false; // buildOrbits() has run: later constellations seed from their name
    glm::vec3 orbitAnchorSunDirECI{1.0f, 0.0f, 0.0f}; // sunDirECI at the first build (terminator planes)
    // Runtime roster changes (2026-10-15): every constellation loaded at startup, in load order, with its
    // random stream recorded — what rosterAdd() can put back.
    std::vector<ConstellationConfig> constellationDefs;
    void createSatRosterBuffers(VulkanContext &ctx);   // the per-satellite GPU buffers (roster + Kessler pool)
    void destroySatRosterBuffers(VkDevice device);
    void writeSatRosterDescriptors(VulkanContext &ctx, bool noDrain = false); // every set binding one of them
    // (2026-10-27) A pool growth during a frame (kesslerEnsureCapacity) re-points the sets above at the new buffers
    // WITHOUT draining the frames in flight: the sets are copied into fresh ones (a pool of their own), the new buffers
    // written there, and the handles swapped; the old sets stay valid for the frames still using them, and their pool
    // is destroyed a few frames later. Updating the sets in place drained both frames: 24-62 ms at 1 d/s after the CME.
    bool swapSatRosterDescriptors(VulkanContext &ctx);
    VkDescriptorPool rosterSetPool = VK_NULL_HANDLE;   // the pool the current swapped sets came from
    std::vector<std::pair<VkDescriptorPool, uint64_t>> rosterSetPoolsOld;   // (pool, retire frame)
    void rebuildRoster(VulkanContext &ctx);             // after `constellations` changed (drops index-keyed state)
    bool rosterRemove(VulkanContext &ctx, const std::string &name);
    bool rosterAdd(VulkanContext &ctx, const std::string &name, std::string *why = nullptr);
    void createSatRebake(VulkanContext &ctx);                 // sat_rebake.comp pipeline + set (after createSatBuffers)
    void recordSatRebake(VkCommandBuffer cmd);                // moves the orbit epoch to now on the GPU
    void createCloudNoisePipeline(VulkanContext &ctx);
    void createCloudWarpNoisePipeline(VulkanContext &ctx);
    void createAuroraNoisePipeline(VulkanContext &ctx);
    void createCloudMarchResources(VulkanContext &ctx);
    void createCloudMarchDescriptors(VulkanContext &ctx);
    void createCloudMarchPipeline(VulkanContext &ctx);
    void createSceneDepthResources(VulkanContext &ctx);
    // ── Clouds v2 (SatelliteSimCloudsV2.cpp, .plans/CLOUDS_V2_PLAN.md) ──
    void createCloudsV2(VulkanContext &ctx);        // bakes + UBO + pipelines + targets (after createGlowResources)
    void createCloudsV2Targets(VulkanContext &ctx); // swapchain-sized images + their descriptor writes
    void destroyCloudsV2Targets(VkDevice device);
    void destroyCloudsV2(VkDevice device);
    void makeSmoothTilePipelines(VulkanContext &ctx, const char *marchSpv);
    // harness `shaders reload [march=<spv>]`; "" = ok, stats = the driver's statistics for the march
    // Harness `shaders reload weather=<spv>` (2026-10-25): the weather cube's bake from another SPV (an empty path = the
    // shipped one); every face is re-baked. Returns an error or "".
    std::string reloadWeatherBake(VulkanContext &ctx, const std::string &spv);
    std::string reloadCloudsV2Shaders(VulkanContext &ctx, const std::string &marchSpv, std::string &stats,
                                      uint32_t wgX = 0, uint32_t wgY = 0, const std::string &resolveSpv = {},
                                      const std::string &lightningSpv = {}, const std::string &compositeSpv = {});
    // harness `shaders reload sky=<spv> sky_taa=<spv>`: the full sky pass's fragment SPVs (the plain one, used at 100% with
    // the sky TAA off and by the low-res prepass, and the SKY_TAA one); "" = the shipped files. Rebuilds the full sky
    // pipelines from them (the same destroy + create as ensureFullSkyPipelines); stats = the TAA pipeline's (else the plain
    // one's) driver statistics.
    std::string skyFragSpvOverride, skyTaaFragSpvOverride;
    std::string reloadSkyShaders(VulkanContext &ctx, const std::string &skySpv, const std::string &skyTaaSpv, std::string &stats);
    // The march's workgroup size (specialization constants): it bounds the driver's registers per
    // thread (65536 / the group's threads), i.e. the march's occupancy. Harness `shaders reload wg=`.
    uint32_t cv2MarchWg[2] = {16, 16};
    void writeCloudsV2ConsumerDescriptors(VulkanContext &ctx); // cloud_march / beam_self_march bindings
    void recordCloudsV2(VkCommandBuffer cmd, VulkanContext &ctx, const CloudMarchPC &cpc);
    void fillCloudsV2Params(VulkanContext &ctx, const CloudMarchPC &cpc);
    void createSceneDepthDescriptors(VulkanContext &ctx);
    void createSceneDepthPipeline(VulkanContext &ctx);
    void writeSceneDepthSeedDescriptors(VulkanContext &ctx);
    void createBeamSelfMarchDescriptors(VulkanContext &ctx);
    void createBeamSelfMarchPipeline(VulkanContext &ctx);
    void createGlowResources(VulkanContext &ctx);
    void createComputePipeline(VulkanContext &ctx);
    void createSkyBgPipeline(VulkanContext &ctx);
    void createSkyLowResResources(VulkanContext &ctx); // resolution scaling (session 29)
    void destroySkyLowResResources(VkDevice device);   // called from onResize (before recreate) and cleanup
    void createDrawPipeline(VulkanContext &ctx);
    // ── Flare/corona render-to-texture pipeline (flare architecture overhaul) ─────────────────
    void createFlareResources(VulkanContext &ctx);   // images, samplers, render pass, framebuffer
    void destroyFlareResources(VkDevice device);     // called from onResize (before recreate) and cleanup
    void createFlareDescriptors(VulkanContext &ctx); // new flareBlur (2 storage images) and
                                                     // flareComposite (1 sampler) descriptor sets.
                                                     // descLayout/descSet bindings 7/8 and
                                                     // skyDescSet binding 20 are added directly in
                                                     // createDescriptors()/createGlowResources()
                                                     // themselves (descriptor set layouts are
                                                     // immutable once created, so those two can't
                                                     // be "extended" afterward from here).
    void createFlarePipelines(VulkanContext &ctx);   // flareSource (graphics), flareBlur (compute),
                                                     // flareComposite (graphics) pipelines
    // ── Long-exposure trail pipeline ───────────────────────────────────────────────────────────
    // Must run after createFlarePipelines (needs drawPipeLayout/descSet) AND after initPlanets
    // (needs starPipeLayout/starDescSet/planetDescSet) — see init()'s call order.
    void createTrailResources(VulkanContext &ctx);   // image/view/mem, render pass, framebuffer
    void destroyTrailResources(VkDevice device);     // called from onResize (before recreate) and cleanup
    void createTrailDescriptors(VulkanContext &ctx); // trailFade (1 storage image) and
                                                     // trailComposite (1 sampler) descriptor sets
    void createTrailPipelines(VulkanContext &ctx);   // trailFade (compute), trailSat/trailStar
                                                     // (graphics, reuse existing pipeline layouts),
                                                     // trailComposite (graphics) pipelines
    void initStars(VulkanContext &ctx);
    void createStarPipeline(VulkanContext &ctx);
    // Lightning channels, glow and thunder (SatelliteSimLightning.cpp, 2026-10-05).
    struct BoltSegC : GpuBoltSeg { float arc = 0.0f, f = 0.0f; };
    struct BoltTree
    {
        uint32_t id = 0;
        int kind = 0;
        glm::dvec3 originEcef{0.0}, up{0.0}, t1{0.0}, t2{0.0};
        glm::vec3 groundLocal{0.0f}, exitLocal{0.0f};
        float baseLocalZ = -1000.0f, strength = 0.5f, maxArc = 1.0f, mainArc = 1.0f;
        std::vector<BoltSegC> segs;
        std::vector<std::vector<glm::vec4>> arms;   // a spider's arms: xyz local, w arc
        uint64_t lastFrame = 0;
    };
    static constexpr uint32_t kBoltMaxFlashes = 128;
    static constexpr uint32_t kBoltMaxSegs = 1u << 17;
    void createLightningResources(VulkanContext &ctx);
    void createLightningPipeline(VulkanContext &ctx);
    void writeLightningDescriptors(VulkanContext &ctx);
    void writeLightningMeshDescriptor(VulkanContext &ctx);
    void destroyLightningResources(VkDevice device);
    void updateLightningBolts();
    void recordLightning(VkCommandBuffer cmd, VulkanContext &ctx);
    void buildBoltTree(BoltTree &t, uint32_t seed, float distM);
    void addThunderSource(const BoltTree &t, double flashStartS);
    std::unordered_map<uint32_t, BoltTree> boltTrees_;
    VkBuffer boltFlashBuf = VK_NULL_HANDLE, boltSegBuf = VK_NULL_HANDLE;
    VkDeviceMemory boltFlashMem = VK_NULL_HANDLE, boltSegMem = VK_NULL_HANDLE;
    void *boltFlashMapped = nullptr, *boltSegMapped = nullptr;
    VkDescriptorSetLayout boltDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool boltDescPool = VK_NULL_HANDLE;
    VkDescriptorSet boltDescSet = VK_NULL_HANDLE;
    VkPipelineLayout boltPipeLayout = VK_NULL_HANDLE;
    VkPipeline boltPipeline = VK_NULL_HANDLE;
    uint32_t boltFlashCount = 0, boltSegCount = 0;
    uint64_t boltFrame = 0;
    // Per frame slot: the observer direction the slot's frame ran the lightning pass with, and whether it ran.
    glm::dvec3 cv2LightningObsDirS[VulkanContext::kMaxFramesInFlight] = {{0.0, 0.0, 1.0}, {0.0, 0.0, 1.0}};
    bool cv2LightningObsDirValidS[VulkanContext::kMaxFramesInFlight] = {};
    // Harness `lightning spawn`: flashes placed by hand, run through the same channels, glow and thunder.
    struct InjectedFlash { uint32_t id, seed; int kind; glm::dvec3 origin, ground; float base, top, strength, dur; double startS; };
    std::vector<InjectedFlash> boltInjected_;
    uint32_t boltInjectSerial_ = 0;
    // Rain, snow and diamond dust at the eye as particles (SatelliteSimRain.cpp, 2026-10-04).
    void createRainParticles(VulkanContext &ctx);
    void createRainPipeline(VulkanContext &ctx);
    void writeRainDescriptors(VulkanContext &ctx);
    void destroyRainParticles(VkDevice device);
    void recordRainParticles(VkCommandBuffer cmd, VulkanContext &ctx, float dt);
    VkDescriptorSetLayout rainDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool rainDescPool = VK_NULL_HANDLE;
    VkDescriptorSet rainDescSet = VK_NULL_HANDLE;
    VkPipelineLayout rainPipeLayout = VK_NULL_HANDLE;
    VkPipeline rainPipeline = VK_NULL_HANDLE;
    bool cv2LightningRanThisFrame = false;   // the rain light at the eye is this frame's (recordCloudsV2)
    glm::dvec3 rainPrevEye{0.0};
    void updateRainMotion(GpuCloudV2Params &p);   // fillCloudsV2Params: integrates the fall and the wind over sim time
    double rainPrevSimT = 0.0;
    bool rainPrevSimTValid = false;
    double rainFallPhase = 0.0;                  // integral of the reference fall speed (m)
    glm::dvec2 rainDrift{0.0}, snowDrift{0.0};   // integral of the wind (m, rain frame E/N)
    double snowFallPhase = 0.0;                 // the snow's fall phase (integral of its reference speed, m)
    glm::vec2 rainWindNow{0.0f};                 // eased wind (m/s)
    // The surface wind at the eye (2026-10-23, SatelliteSimRain.cpp surfaceWindAt): a pure function of place and sim
    // time (+ the storm towers and the rain map's last frame), shared by the rain / snow particles and the ambience.
    struct SurfaceWind
    {
        glm::vec2 v{0.0f};        // the wind now (rain frame E/N, m/s), gusts and outflow included, before easing
        glm::vec2 base{0.0f};     // the mean surface wind (m/s)
        glm::vec2 outflow{0.0f};  // the storms' and the shower's outflow (m/s)
        glm::vec2 cyclone{0.0f};  // a tropical cyclone's wind field (m/s; Weather.h CycloneState)
        float cycloneVmax = 0.0f; // that cyclone's own maximum wind (0 none within its reach)
        float gust = 0.0f;        // the gust fronts' envelope now (0 calm .. ~1.5 a strong front), for the ambience
        float storm = 0.0f;       // storm proximity (0 .. 1.5): towers within a few tens of km
        int gustsActive = 0;      // gust fronts contributing now
    };
    SurfaceWind surfaceWindAt(double simT, float rainRateN);
    SurfaceWind surfaceWindForAmbience();
    SurfaceWind surfaceWindNow_;
    double surfaceWindSimT_ = -1e300;
    float rainRateEased = 0.0f;
    float rainFallRefNow = 1.0f;
    bool rainPrevEyeValid = false;
    int rainModeLastFrame = 0;               // 0 none, 1 rain / snow, 2 diamond dust (harness state)
    uint32_t rainInstancesLastFrame = 0;
    int rainLevelsLastFrame = 0;
    void updateStars();
    // (its per-elevation table: starElTab_, with the catalogue's members above)
    void initPlanets(VulkanContext &ctx);                                       // planetBuf + planetDescSet, reusing starDescLayout
    void updatePlanets();                                                       // mirrors updateStars(); called right after it
    int pickPlanetAt(float clickX, float clickY, float screenW, float screenH); // mirrors
                                                                                // pickSatelliteAt but reads the already host-mapped
                                                                                // planetBuf directly — no device->host staging copy
    void formatSelectedPlanetInfo();                                            // mirrors formatSelectedSatInfo, fills planetInfoLine[]
    void createPlanetDiscs(VulkanContext &ctx);         // maps, UBO, set, pipeline (after initPlanets)
    void createPlanetDiscPipeline(VulkanContext &ctx);  // (viewport baked in: rebuilt on resize)
    void writePlanetDiscTargets(VulkanContext &ctx);    // the cloud composite / scene depth after they are recreated
    void destroyPlanetDiscs(VkDevice device);
    // The turbulence warp (2026-10-28, SatelliteSimTurbulence.cpp, shaders/turb_warp.frag): the composed frame
    // resampled through a seeing field before the UI.
    void createTurbWarp(VulkanContext &ctx);           // UBO, set, pipeline (init)
    void createTurbWarpFrame(VulkanContext &ctx);      // the frame copy at ctx.swapExtent (init, onResize)
    void writeTurbWarpTargets(VulkanContext &ctx);     // the cloud composite / scene depth after they are recreated
    void destroyTurbWarp(VkDevice device);
    void updateTurbWarp(VulkanContext &ctx);           // recordDraw: the UBO + whether the pass runs this frame
    void recordTurbWarp(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image); // recordPreUiCopy
    nlohmann::json turbWarpJson() const;               // harness state
    void updatePlanetDiscs(GpuSatVisible *dst);         // from updatePlanets: the discs, the moons, the hand-off
    void recordPlanetDiscs(VkCommandBuffer cmd, const PointDrawPC &pc);
    double planetSinRingB(int planet) const;             // Saturn's rings' tilt toward the Earth (sin B); 0 for the rest
    nlohmann::json planetsJson() const;                  // harness state: the discs and the Galilean moons
    void updateLightPollutionDome(float dt);                                    // called each frame before updateStars(): fills lightDomeAz[]
    void uploadLightDome();                                                     // memcpys both dome halves into lightDomeBuf
                                                                                // + uploads to lightDomeBuf for sat_flare.comp
    void updateGpuTimingStats(VulkanContext &ctx);                              // called at top of recordCompute(): EMA-smooths
                                                                                // ctx.timestampMs into gpuMsSmoothed[]/gpuMsTotalSmoothed
    void initConstellation();                                                   // called once: loads definitions then builds orbits
    void writeResolvedSatTypes() const; // dumps resolved types (new attitude format) to the user data dir
    bool loadModelType(SatelliteType &t, SatModel &model); // Phase 3: read a geometry model file
    void bakeModelType(SatelliteType &t, const SatModel &model, int budget, size_t rosterCount,
                       std::vector<std::string> *logOut = nullptr); // bake + validate + OBJ (logOut: from a worker)
    // Max baked facet lobes per model type. GPU cost is (visible satellites of the type) × lobes,
    // so the budget depends on how many satellites fly the type: mass constellations keep it
    // tight, small rosters (stations, telescopes, depots) get enough to stay exact.
    static constexpr int kSatLobeBudget = 48;
    static constexpr int kSatLobeBudgetSmall = 256;
    static constexpr size_t kSatLobeSmallRoster = 10000;
    void loadDefinitions();                                                     // reads constellations.json; falls back to hardcoded defaults
    void loadHardcoded();                                                       // hardcoded satTypes + constellations (used as fallback)
    void buildOrbits();                                                         // populates satOrbits from satTypes + constellations
    // S1 (RELEASE_v1_1_PLAN.md): reads reflector_targets.json (real solar-farm sites, moddable
    // like constellations.json); falls back to generateReflectorTargetsRandomFallback() if
    // missing/malformed/empty. Called from buildOrbits(), same call-order constraint as the old
    // inline code it replaced (needs earthElevCpu, populated by createGlowResources() earlier in
    // init()). Sets reflectorTargetCount and reflectorObserverSpawnIdx.
    void loadReflectorTargets();
    // Fills groundBeams.solar from the solar targets nearest the observer and eases each one's tracker
    // angle toward the Sun, or at night the strongest beam landing on it (dirs: ground -> satellite,
    // observer ENU; hits: landing points relative to the observer's sea-level point; w: weights).
    struct SolarBeamHit { glm::vec3 hitQ, dirToSat; float weight; };
    void fillSolarSites(GpuGroundBeams &gb, const std::vector<SolarBeamHit> &hits, float dt);
    // Fallback used only when reflector_targets.json is absent/unusable: kNumReflectorTargets-1
    // uniformly-random lat/lon points, plus a REAL fixed entry at index 0 for the observer spawn
    // point (67S 67W) — fixes the same "index 0 left as a degenerate zero-vector" bug the JSON
    // path fixes, so both paths guarantee a real reachable target near spawn on first launch.
    void generateReflectorTargetsRandomFallback();
    // Shared by both paths above: looks up real terrain elevation at reflectorTargetsECEF[ti]
    // (3x3-max texel neighborhood, C12 follow-up #23) and writes reflectorTargetsRadiusM[ti].
    // Defaults to sea level if earthElevCpu isn't populated for whatever reason.
    void computeReflectorTargetElevationRadius(int ti);
    void loadSettings(); // reads settings.json; silently uses defaults if missing
    void saveSettings(); // writes settings.json next to exe
    // UC1: overwrites debugDisableMask/renderScale/advanced sliders per the named preset's table
    // (no-op data-wise for Custom — see GraphicsPreset comment). Recreates the render-scale
    // offscreen target since presets can change renderScale.
    void applyGraphicsPreset(GraphicsPreset p);
    // The tier a device should start on: Potato for MoltenVK on non-Apple GPUs (the 2015 MacBook Pro's GCN 1.0),
    // Planetarium for integrated / CPU / virtual GPUs and discrete ones with <= 2 GB of VRAM, else Medium.
    // Static and needs no SatelliteSim, so the startup graphics chooser can pre-select it before init; `why`
    // gets a one-line reason with the device's name, type and memory.
    static GraphicsPreset recommendedPresetForDevice(VkPhysicalDevice pd, std::string *why = nullptr);
    // UC1 first-run seed: recommendedPresetForDevice, logged. Only called when no persisted preset exists.
    GraphicsPreset seedGraphicsPresetFromDevice(VulkanContext &ctx) const;
    // Crash recovery steps the tier DOWN one: a full tier (Low .. Ultra, Custom) -> Planetarium -> Potato.
    static GraphicsPreset crashRecoveryPreset(GraphicsPreset current);
    void savePerfSnapshot(float cpuDt);                // appends one profiling record to perf_profiles/profile_log.jsonl
    nlohmann::json buildPerfSnapshotJson(float cpuDt); // the shared body of the above and of the sweep record
    void appendPerfRecord(const nlohmann::json &j);    // one JSONL line into perf_profiles/profile_log.jsonl
    void startKnockoutSweep();                         // Display tab "Run knockout sweep" button
    void updateKnockoutSweep(float cpuDt);             // one step of the sweep state machine; called
                                                       // once per frame from recordCompute()
    void updatePositions(double t, float dt = 0.0f);   // called each frame: fills satInputData + eci2enu
    void toggleTimeDirection() { timeDir = -timeDir; } // shared by KB_REVERSE and the left-panel Reverse button

    // ── Satellite picking (see "Satellite picking / selection tracking" members above) ────
    // Pure camera geometry mirror of sat_point.vert's projection (shaders/sat_point.vert:34,47,60-62);
    // shared by the one-shot full-buffer scan below and the per-frame tracked-selection reprojection.
    bool projectSkyDirToScreen(const glm::vec3 &skyDir, float screenW, float screenH,
                               float &outX, float &outY) const;
    // One-shot: copies satVisibleBuf back to a transient host-visible staging buffer and scans it
    // for the nearest on-screen visible satellite to (clickX, clickY). Returns -1 if none hit.
    int pickSatelliteAt(float clickX, float clickY, float screenW, float screenH);
    void formatSelectedSatInfo(); // fills selectedSatInfoBuf from satOrbits[selectedSatIndex]; call when selection changes

    // ── Small UI helpers shared across every UI builder method ────────────────
    // (formerly local lambdas inside the single monolithic buildUI(); now member
    // functions since buildUI is split across buildLeftHudPanel/buildSettingsWindow/etc.)
    // The automatic (window-height) factor may shrink text down to kAutoFontFloorPx, or to the player's own size where
    // that is already smaller. The floor was 16 px while the font was a baked bitmap (it broke up below that); the SDF
    // atlas (2026-10-21, UIRenderer::loadFont) stays clean at 9-12 px, so short windows now really get smaller text.
    static constexpr int kAutoFontFloorPx = 12;
    uint16_t fs(int base) const
    {
        const int eff = (int)(base * uiScale + 0.5f), user = (int)(base * uiScaleUser + 0.5f);
        return (uint16_t)std::max(8, std::max(eff, std::min(user, kAutoFontFloorPx)));
    }
    // Defined in SatelliteSimUI.cpp — need AudioSystem's complete type (only forward-declared here).
    void sndRollover(bool nowHov, bool prevHov) const;
    void sndClick(bool nowHov, bool lmbPressed) const;

    // ── UI builders (defined in SatelliteSimUI.cpp) ────────────────────────────
    void buildLeftHudPanel(const UIInput &inp, UIRenderer &ui);
    void buildRightHudPanel(const UIInput &inp, UIRenderer &ui);
    void buildSelectedSatPanel(const UIInput &inp, UIRenderer &ui); // floating panel that tracks selectedSatIndex

    // Shared resizable+draggable+(optionally) closable window frame — title bar,
    // 8-direction edge/corner resize, bevel border — used by both the settings
    // window and the view-controls window so there is one window implementation,
    // not two. `buildBody` declares whatever content goes inside (tabs+content for
    // settings, a plain scroll list for view-controls). Returns true the frame the
    // close button was clicked (closable windows only), so callers can react (e.g.
    // save settings).
    // titleExtras (optional) adds elements to the title bar just left of the close button;
    // titleHighlight draws the title in the selection colour.
    bool buildResizableWindow(const UIInput &inp, UIRenderer &ui, WindowChrome &chrome,
                              int winId, const char *title, bool closable, bool &hovCloseFlag,
                              float defaultX, float defaultY,
                              float minW, float minH, float maxW, float maxH,
                              const std::function<void()> &buildBody,
                              const std::function<void()> &titleExtras = nullptr,
                              bool titleHighlight = false);
    std::string winTitleBuf_[32]; // buildResizableWindow's ellipsized titles, per winId (Clay keeps the pointers)
    static constexpr float kWinScreenMargin = 8.0f; // a window keeps this far inside the screen (size caps)
    // The right HUD panel's compact form (no FPS / version) when it would reach the left panel (buildRightHudPanel).
    bool hudCompact_ = false;
    float hudCompactKey_ = -1.0f; // screen size + UI scale it was decided for
    int hudCompactSettle_ = 0;
    // A window's DEFAULT extent (2026-10-21, low resolutions): `px` (its 1080p size, which it keeps on any screen
    // that is big enough) but at most `frac` of the screen's extent `scr`, and never below `minPx` (unless the
    // screen itself is smaller; buildResizableWindow caps every size at the screen).
    static float winDefault(float px, float frac, float scr, float minPx = 0.0f)
    {
        return std::max(std::min(px, scr * frac), std::min(minPx, scr - 2.0f * kWinScreenMargin));
    }

    void buildSettingsWindow(const UIInput &inp, UIRenderer &ui);
    void buildSettingsTabbedBody(const UIInput &inp, UIRenderer &ui);
    void buildSettingsConstellationsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsLaunchesTab(const UIInput &inp, UIRenderer &ui); // "Launches" (2026-10-21)
    void buildSettingsSkyTab(const UIInput &inp, UIRenderer &ui);      // "Sky" (2026-10-28): stars, planets, the Moon, comets, meteors, refraction
    void buildSettingsSoundTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsControlsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsDisplayTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsPerformanceTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsPhotometryTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsCloudsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsOceanTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsTerrainTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsAuroraTab(const UIInput &inp, UIRenderer &ui);   // the "Atmosphere" tab
    void buildSettingsWeatherTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsNightLightsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsBeamsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsAttributionsTab(const UIInput &inp, UIRenderer &ui);
    // Shared slider-row struct/renderer for the Clouds/Ocean/Terrain/Aurora tabs (split from one
    // combined "Clouds" tab, session 28 follow-up #9) — `idx` indexes the shared draggingCloud/
    // hovCloudMinus/hovCloudPlus member arrays and a function-local static text-buffer array, so
    // each tab's slider subset keeps its ORIGINAL global index (no renumbering needed) even though
    // only a slice of the full 0-32 range is passed to any one call.
    struct CloudSlider
    {
        const char *label;
        float *val;
        float vmin, vmax, step;
        const char *fmt;
        int idx;
    };
    // marksPreset: an edit sets the graphics preset to Custom (every tab but Sound). Slot idx 99-108
    // are the Sound tab's (ambience); 110/111 are the satellite ocean-glint pair added 2026-09-26 and
    // rendered from the Ocean tab.
    void buildCloudSliderRows(const UIInput &inp, UIRenderer &ui, CloudSlider *sliders, int count,
                              bool marksPreset = true);
    // Collapsible category grouping on top of buildCloudSliderRows. A section owns no slider
    // state — it only decides whether its slice of the array is rendered this frame — so a
    // slider's global `idx` (and therefore its hover/drag/text-buffer slots) is untouched by
    // which section it lands in, and sliders can be regrouped freely without renumbering.
    struct CloudSliderSection
    {
        const char *title;
        CloudSlider *sliders;
        int count;
    };
    // base: this tab's first section slot (each tab owns its own range of open/hover states).
    void buildCloudSliderSections(const UIInput &inp, UIRenderer &ui, CloudSliderSection *sections, int count,
                                  int base = 0);
    void buildViewControlsWindow(const UIInput &inp, UIRenderer &ui);
    void buildViewControlsBody(const UIInput &inp, UIRenderer &ui);
    void buildIntroOverlay(const UIInput &inp, UIRenderer &ui);
    void buildCrashRecoveryNotice(float dt, const UIInput &inp, UIRenderer &ui); // NEW-3
    void buildGraphicsAutoNotice(float dt, const UIInput &inp, UIRenderer &ui);  // UC1 mechanism 3
    // ── First-run tutorial (2026-10-03, SatelliteSimTutorial.cpp) ────────────────────────────────
    // Replaced the intro's "WASD to move / Q-E / click a satellite" beat (shown while clicking was still
    // disabled) and the post-intro "Click to select any satellite" hint (review 17). One card at a time,
    // each step finished by DOING it (the keys / buttons light on the graphic as they are pressed), then
    // the HUD's own controls pointed at with outlines. Runs after the intro, once (display.tutorial_done).
    enum TutStep
    {
        TUT_LOOK = 0,
        TUT_MOVE,
        TUT_ALTITUDE,
        TUT_BOOST,
        TUT_SELECT,
        TUT_INFO,       // the selection's Info (2026-10-21: the four buttons had one step, any of them finished it)
        TUT_GOTO,       // Go to, then leave it again (Exit flies home, Stay here stays)
        TUT_TRACE,      // Trace pass or Track
        TUT_TIME,
        TUT_CAPTURE,
        TUT_MISSIONS,   // open the Missions window; Upcoming: Go to / Watch a launch (or the Sun's Fury)
        TUT_ONGOING,    // the Ongoing tab (a few seconds on it, or any of its buttons); "Back to <time>" offered
        TUT_SETTINGS,
        TUT_GRAPHICS,   // only when the tutorial starts on a light preset (tutGraphicsCard): no clouds there
        TUT_COUNT,
    };
    // The light presets (Planetarium, Potato) draw no clouds; a first run on an integrated GPU starts on
    // Planetarium, and the tutorial ends with a card that says so and where to change it (2026-10-04).
    bool tutGraphicsCard = false;
    int tutBasePreset = 0;
    int tutStepCount() const { return tutGraphicsCard ? (int)TUT_COUNT : (int)TUT_GRAPHICS; }
    bool tutActive = false;
    bool tutorialDone = false;      // persisted (display.tutorial_done); Skip or Finish sets it
    bool tutAutoChecked = false;    // the once-per-session auto start has been considered
    int tutStep = 0;
    float tutClock = 0.0f;          // drives the highlight pulse
    float tutProgress = 0.0f;       // 0..1 toward finishing the current step's action
    float tutDoneT = -1.0f;         // >= 0: the step is done, its "Done" shows this long before the next
    float tutLastAz = 0.0f, tutLastEl = 0.0f;
    // The state a step started from (Time / Capture / Settings / Actions finish on a CHANGE of it).
    int tutBaseTimeIdx = 0;
    bool tutBasePaused = false, tutBaseReverse = false, tutBaseTrails = false, tutBaseCine = false, tutBaseBookmarks = false;
    bool tutBaseSettings = false, tutBaseReports = false;
    bool tutBaseInfo = false, tutBaseTrace = false, tutBaseTrack = false, tutGoSeen = false;
    int tutBaseMsUpcoming = 0, tutBaseMsOngoing = 0;
    float tutOngoingS = 0.0f;       // seconds on the Missions window's Ongoing tab (TUT_ONGOING)
    // Where the player was before the Missions step's Go to / Watch moved the clock and the place (2026-10-21):
    // every later card offers "Back to <time>" until it is used (tutReturnNow), so nobody is left days ahead.
    bool tutRetSaved = false, tutRetValid = false;
    double tutRetT = 0.0, tutRetDriftPhaseOff = 0.0;
    glm::vec3 tutRetObsDir{0.0f, 0.0f, 1.0f};
    float tutRetHeight = 0.0f, tutRetAz = 0.0f, tutRetEl = 0.0f, tutRetFov = 70.0f, tutRetDir = 1.0f, tutRetDriftRate = 0.0f;
    int tutRetTimeIdx = 0;
    bool tutRetPaused = false;
    char tutRetLabel[64] = {};
    char tutPickMsg[96] = {};       // "Pick one for me"'s result when it found nothing
    char tutKeyLabel[3][16] = {};   // rebindable keycap labels (keyDisplayName's letters rotate through a pool)
    bool hovTutNext = false, hovTutBack = false, hovTutSkip = false, hovReplayTutorial = false, hovTutPick = false,
         hovTutReturn = false;
    // ── Gamepad navigation of the selection (2026-10-03, pollGamepad) ──
    // With a satellite selected and no virtual cursor: D-pad left/right move a focus ring across its
    // buttons (Info, Go to, Trace pass, Track = SelActBtn id % 4), A presses the focused one (the
    // button's own click path: buildSelActionButton), B goes back one layer (padBack). The default pad
    // bindings moved to make room (time on D-pad up/down, pause X, reverse Y): kPadLayoutVersion.
    static constexpr int kPadLayoutVersion = 2;
    // Keyboard layout of the saved bindings (controls.key_layout). 2 (2026-10-28, issue 20261028_223530): T is Track,
    // I Info, G Go to / leave; "Select Satellite" lost its T (a saved T is dropped once), and "Leave Go To Here" was
    // renamed "Go To / Leave Here" (a saved key under the old name carries over).
    static constexpr int kKeyLayoutVersion = 2;
    void trackToggle();          // Track (key T, the panel's button); during Go to it is the aim lock
    bool trackShownOn() const;   // the Track button lit
    bool selectionKeyAction(int bindIdx); // KB_INFO / KB_TRACK / KB_FOLLOW_STAY on the selection; false = nothing to do
    int padFocus = 0;               // 0 Info, 1 Go to, 2 Trace pass, 3 Track
    int padFocusSat = -1;           // the selection padFocus was chosen for (a new one resets it)
    bool padActivatePending = false; // A pressed: the focused button clicks in this frame's buildUI
    bool padActivateNow = false;
    char padHintBuf[64] = {};
    uint32_t selActionAvailMask() const; // which of the four buttons the selection has (bit = id % 4)
    void padFocusStep(int dir);
    bool padBack();                 // true if B did something (closed / left / released / deselected)
    bool padContextButton(int glfwPadButton, bool cursorActive); // true = used (the binding does not fire)
    bool padFocusVisible() const { return lastInputWasGamepad && !vCursorActive; }
    uint32_t selBtnDrawnMask = 0;   // SelActBtn ids drawn this frame (buildSelActionButton; reset by buildSelectedSatPanel)
    char tutBodyBuf[400] = {};
    char tutHeadBuf[48] = {};
    void startTutorial(int step = 0);
    void endTutorial(bool finished);
    void setTutorialStep(int step);
    void updateTutorial(float dt); // completion detection, every frame after the look block
    bool tutInfoOn() const;        // the info window open on the selection
    bool tutNeedsPick(int step) const; // Select / Info / Go to / Trace: no (suitable) selection yet
    bool tutPickSatellite();       // "Pick one for me": a sunlit satellite with a model, high in the sky; aims at it
    void tutSaveReturn();
    void tutReturnNow();
    void buildTutorial(const UIInput &inp, UIRenderer &ui);
    void buildTutKeyboard(int step);  // the key cluster graphic (Q W E / A S D / Shift)
    void buildTutMouse(int step, const UIInput &inp);
    void buildTutGamepad(int step);
    bool tutGamepadView() const { return lastInputWasGamepad; }
    void buildScreenshotToast(float dt, const UIInput &inp, UIRenderer &ui);     // UC6 confirmation toast
    // UC3: advances introElapsed and drives obsHeightOffset/camera.elDeg/fovYDeg/obsFacing from
    // kIntroKeyframes; called from recordCompute() in place of the normal WASD/zoom block while
    // showIntro is true. Also accumulates the UC1 benchmark and auto-ends the intro at the last
    // keyframe (calling finishIntro(false)).
    void updateIntroCinematic(float dt);
    // Ends the intro (showIntro=false). wasSkipped=true means the user dismissed early (click/key/
    // pad) — no representative frame-time average was collected, so the benchmark promote/demote
    // is skipped entirely and whatever preset was already active (the device-type seed, or a prior
    // session's saved preset) stands, per RELEASE_v1_1_PLAN.md UC3: "do not run the benchmark
    // during the skip path."
    void finishIntro(bool wasSkipped);
    void setLat(float newLatDeg);   // moves observer to a new latitude; used by the right panel's lat display scroll-adjust
    void adjustLon(float deltaDeg); // rotates observer around Earth's polar axis; right panel's lon display scroll-adjust
                                    // dt = simulated seconds elapsed this frame (0 when paused);
                                    // used for mirror slew rate so behaviour is consistent at all time scales
};

// Time scale options (simulated seconds per real second)
static constexpr float kTimeScales[] = {1.0f, 10.0f, 60.0f, 300.0f, 3600.0f,
                                        86400.0f, 86400.0f * 7.0f, 86400.0f * 30.0f, 86400.0f * 365.0f};
static constexpr const char *kTimeLabels[] = {"1x", "10x", "1m", "5m", "1h", "1d", "1w", "1mo", "1yr"};
static constexpr int kNumTimeScales = 9;

// ── UC3 intro cinematic fixed vantage (session follow-up) ─────────────────────
// The intro always opens from this exact real-world spot — the California coast at twilight,
// facing out toward the SpaceX AI-datacenter satellites and the Reflect Orbital mirrors aimed at
// the nearby Topaz solar farm — rather than wherever the player's last saved/current observer
// position happens to be. Values are the live camera/observer state the vantage was designed
// against (settings.json: observer.lat_deg/lon_deg, camera.az_deg/el_deg/fov_y_deg). Forced onto
// obsDir/obsLatDeg/obsLonDeg/camera.azDeg/elDeg/fovYDeg once, at the start of every intro playback
// (see updateIntroCinematic's one-time init block) — including a Display-tab replay — so the
// cinematic is reproducible no matter where the player has since wandered off to.
static constexpr float kIntroObserverLatDeg = 35.869096f; // 300 m SSW of the original (review 12: the eroded ridge filled the right of frame)
static constexpr float kIntroObserverLonDeg = -121.401905f;
static constexpr float kIntroStartAzDeg = -61.32f;
static constexpr float kIntroStartElDeg = 20.8f;
static constexpr float kIntroStartFovDeg = 70.0f;
// The intro's cloud layout, as a rate: the map sat at phase kIntroCloudDriftRate x (time since
// J2000) when the intro was tuned (a clear patch over the vantage). updateIntroCinematic's one-time
// init turns it into this session's cloudDriftPhaseOffset. It used to FORCE cloudDriftRate to this
// value, which settings.json then saved: every intro silently changed the player's drift rate.
static constexpr float kIntroCloudDriftRate = 6.55e-6f;

// ── UC3 intro cinematic beat sheet (RELEASE_v1_1_PLAN.md) ─────────────────────
// Shared between SatelliteSim.cpp (updateIntroCinematic/finishIntro, the playback) and
// SatelliteSimUI.cpp (buildIntroOverlay, the caption text) — header-scope so both translation
// units see the identical table without a getter. `text == nullptr` means "no new caption at
// this keyframe" (introCaptionIndex just holds whatever the last non-null one was).
struct IntroKeyframe
{
    float t;    // seconds from intro start
    float altM; // obsHeightOffset target
    float azDeg, elDeg, fovDeg;
    const char *text;
};
static constexpr float songbeat = 7.61f; // beat timings below are synced to this (music tempo)
static constexpr IntroKeyframe kIntroKeyframes[] = {
    // Beat 0 — "2036" title/date card. Ground, twilight, facing the fixed vantage above.
    {0.0f, 0.0f, kIntroStartAzDeg, kIntroStartElDeg, kIntroStartFovDeg, "2036"},
    // Beat 1 — first narrative line; holds the same framing. The skip hint (buildIntroOverlay)
    // only appears once this beat is reached — see kIntroHintRevealIndex.
    {songbeat * 1.0, 0.0f, kIntroStartAzDeg, kIntroStartElDeg, kIntroStartFovDeg,
     "Satellite megaconstellations dominate the night sky"},
    // Beat 3 — level out in preparation for launch. Still ground level.
    {songbeat * 2.0, 00000.0f, kIntroStartAzDeg - 5, 25.0f, 64.0f, "From the ground, we watch sunlight glint off their solar arrays"},
    // Beat 4 — the pull to LEO begins (ascent happens across THIS beat's transition). Still
    // facing horizontally west, per the storyboard, even as altitude climbs.
    {songbeat * 3.0, 60000.0f, kIntroStartAzDeg - 25, 35.0f, 62.0f, "They power an orbital network of communications and AI compute."},
    // Beat 5 — pull continues; camera starts rotating away from due-west as we climb high enough
    // to reveal the Earth's curve.
    {songbeat * 4.0, 140000.0f, kIntroStartAzDeg - 35, 20.0f, 62.0f, "A promise to the markets above all else"},
    {songbeat * 5.0, 250000.0f, kIntroStartAzDeg - 45, 10.0, 70.0f, "We will come to miss the quiet sky"},
    // Beat 6 — arrival in LEO: pulled back and up enough to see the backlit AI sats against a
    // rising sun. Camera motion stops here; beats 7-8 hold this exact framing. Title reveal.
    // Azimuth nudged +5deg right (vs. the beat 5 pan target) to catch the sunset before FOV expands.
    {songbeat * 6.0, 300000.0f, kIntroStartAzDeg - 45, 0.0, 80.0f, "SAT LIGHT SIM"},
    // Beat 7 — arrival: the camera settles on the final framing and the title fades over the last
    // second (buildIntroOverlay's alphaOut), then finishIntro(false) hands over to the tutorial
    // (SatelliteSimTutorial.cpp). Until 2026-10-03 this beat was a "WASD to move / Q-E / click a
    // satellite" caption that unlocked movement but not clicking, and a ninth keyframe held it for a
    // further beat; the controls are the tutorial's job now.
    {songbeat * 7.0, 300000.0f, kIntroStartAzDeg - 45, 20.0, 120.0f, nullptr},
};
static constexpr int kIntroKeyframeCount = sizeof(kIntroKeyframes) / sizeof(kIntroKeyframes[0]);
static constexpr int kIntroYearIndex = 0;       // "2036" title/date card
static constexpr int kIntroHintRevealIndex = 1; // skip hint doesn't show before this beat is reached
static constexpr int kIntroTitleIndex = 6;      // "SAT LIGHT SIM" reveal, arrival in LEO
// The camera moves until the last keyframe, so the whole intro feeds the UC1 benchmark accumulator.
static constexpr float kIntroBenchEndT = songbeat * 7.0f;
static constexpr const char *kGraphicsPresetNames[] = {"Planetarium", "Low", "Medium", "High", "Ultra", "Custom", "Potato"};
