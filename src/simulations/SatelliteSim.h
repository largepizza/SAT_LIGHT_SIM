#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../Simulation.h"
#include "../UIRenderer.h" // WindowChrome — used by member state below (needs complete type)

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>
#include <algorithm>
#include <functional>
#include <thread>
#include <atomic>
#include <mutex>

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
    Planetarium, // v1.0 experience: flat textured Earth, stars, satellites, atmosphere. No clouds.
    Low,         // integrated graphics / old laptops — flat 2D cloud paste, tight terrain reach
    Medium,      // mainstream discrete GPU — volumetric clouds/terrain at reduced budgets
    High,        // today's tuned defaults
    Ultra,       // uncapped for showcase/screenshots
    Custom,      // user has hand-edited an advanced slider since the last named preset was applied
};

// ── Orbit distribution type ────────────────────────────────────────────────────
enum class OrbitDistribution
{
    Walker,      // regular Walker constellation: numPlanes planes × perPlane satellites
    RandomShell, // randomly distributed: random RAAN, random incl in [0, incl], jittered alt
    Disk,        // ring or concentric disk in a fixed orbital plane (incl + raan)
                 // Set alignTerminator=true to auto-derive the plane from sunDirECI
};

// ── One reflective surface of a satellite ─────────────────────────────────────
// A SatelliteType is composed of a primary surface plus an optional secondary
// surface (e.g. radiator panels perpendicular to solar panels) and an optional
// isotropic diffuse floor (structural body scatter).
struct SurfaceSpec
{
    AttitudeMode attitude; // how the surface normal is oriented each frame
    float specExp;         // specular exponent (0 = Lambertian diffuse)
    float weight;          // contribution weight relative to primary (0 = disabled)
};

// ── Per-type satellite parameters (CPU-side, drives GpuSatInput fields) ───────
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
};

// ── Constellation descriptor ───────────────────────────────────────────────────
// One entry per shell. Multiple shells can share the same SatelliteType.
// orbitStart/Count are filled by initConstellation() — do not set manually.
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
    bool highlight = false;       // highlight mode: show all sats at fixed brightness, ignoring lighting
    // Populated by initConstellation():
    uint32_t orbitStart = 0; // first index into satOrbits[]
    uint32_t orbitCount = 0; // number of orbits belonging to this constellation
};

// ── GPU data structures ───────────────────────────────────────────────────────
// std430 packing: vec3 alignment=16 size=12, so vec3+float fills one 16-byte block.
// Five vec3+float blocks (80 bytes) + one float4 tail = 80 bytes total.
//
// Byte map:
//   [  0] eciRelPos (vec3) + range (float)         — position data
//   [ 16] surfN0    (vec3) + elevation (float)      — primary surface normal
//   [ 32] surfN1    (vec3) + specExp0 (float)       — secondary surface normal
//   [ 48] baseColor (vec3) + specExp1 (float)       — colour + secondary specular
//   [ 64] crossSection + w1 + diffuse + _pad (float4) — photometric scalars
//   Total: 80 bytes

struct GpuSatInput
{
    glm::vec3 eciRelPos; // observer-relative ECI position (meters)
    float range;         // distance (meters)
    glm::vec3 surfN0;    // primary surface normal in ECI (attitude-dependent unit vector)
    float elevation;     // elevation above local horizon (radians), pre-computed on CPU
    glm::vec3 surfN1;    // secondary surface normal in ECI (radiators, body, etc.)
    float specExp0;      // primary surface specular exponent (0 = Lambertian)
    glm::vec3 baseColor; // satellite tint from SatelliteType
    float specExp1;      // secondary surface specular exponent (0 = Lambertian)
    float crossSection;  // sqrt(crossSectionM2 / 10.0): area brightness scale (~1 = 10 m²)
    float w1;            // secondary surface weight relative to primary (0 = disabled)
    float diffuse;       // isotropic Lambertian floor — structural body scatter [0,1]
    float mirrorFrac;    // fraction of primary surface that is near-perfect mirror [0,1]
};
static_assert(sizeof(GpuSatInput) == 80, "GpuSatInput layout mismatch");

struct GpuSatVisible
{
    glm::vec3 skyDir;     // unit vector in ENU (x=East, y=North, z=Up)
    float flareIntensity; // [0, 1+]
    glm::vec3 baseColor;  // satellite tint
    float angularSize;    // point sprite size hint (pixels)
};
static_assert(sizeof(GpuSatVisible) == 32, "GpuSatVisible layout mismatch");

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
    float elevCutoff; // sin(Earth-limb angle) — horizon cull threshold (≤ -0.01)
    // Photometry tuning — runtime-adjustable via the settings window.
    float brightnessScale; // global flux multiplier (mirrors BRIGHTNESS_SCALE in shader)
    float daySuppression;  // sky background suppression ratio (mirrors DAY_SUPPRESSION)
    float mirrorBoost;     // mirror peak multiplier (mirrors MIRROR_BOOST)
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

// Draw push constants (passed to sat_point.vert and both sky shaders).
// GLSL std430 layout:
//   skyView    (mat4):  offset 0
//   fovYRad    (float): offset 64
//   aspect     (float): offset 68
//   gmst       (float): offset 72  — Greenwich Mean Sidereal Time (radians)
//   waveTime   (float): offset 76  — wall-clock seconds (glfwGetTime); constant speed regardless of time warp
//   sunDirENU  (vec4):  offset 80   xyz=direction, w=sin(elevation)
//   moonDirENU (vec4):  offset 96   xyz=moon dir in ENU, w=illuminated fraction
//   obsECEFDir (vec4):  offset 112  xyz=observer ECEF unit vector (for ENU→ECEF→lat/lon), w=unused
//   total: 128 bytes
struct SatDrawPC
{
    glm::mat4 skyView;         // ENU → camera space
    float fovYRad;             // vertical field of view (radians)
    float aspect;              // viewport width / height
    float gmst;                // Greenwich Mean Sidereal Time (radians)
    float waveTime;            // wall-clock seconds for wave animation (glfwGetTime)
    glm::vec4 sunDirENU;       // sun direction in ENU (xyz unit vec, w = sin(elevation))
    glm::vec4 moonDirENU;      // moon direction in ENU (xyz unit vec, w = illuminated fraction)
    glm::vec4 obsECEFDir;      // xyz = observer ECEF unit vector (lets sat_sky.frag convert ENU hit →
                               // ECEF → geographic lat/lon for texture UV); w = obsHeightOffset (m,
                               // user altitude offset above terrain — maxed with the GPU's own
                               // ground-height lookup as obsEffH). Despite the field's original "w
                               // unused" comment (stale — corrected here), it IS read.
    uint32_t debugDisableMask; // profiling-only knockout toggles consumed by sat_sky.frag
                               // (dbgSkipTerrain/dbgSkipAtmosphere/dbgSkipSunOD/dbgSkipOceanRefl);
                               // 0 = everything enabled (normal rendering). See Display settings tab.
    float pad0;                // explicit — GLSL push_constant layout aligns the vec2 below to 8
                               // bytes (std430 rules), same as std140/std430 buffers; C++ doesn't
                               // insert this padding automatically the way GLSL requires it, so it
                               // must be here explicitly or screenSizePx reads garbage in the shader.
    glm::vec2 screenSizePx;    // CURRENT render target's pixel dimensions (session 29, resolution
                               // scaling) — skyLowResExtent when recordPrePass renders the scaled
                               // background, ctx.swapExtent everywhere else (full-res draws, and
                               // Pass 1 at renderScale==1.0). Needed because gl_FragCoord.xy is
                               // relative to whatever framebuffer THIS draw call targets, not
                               // always the full swapchain — any shader code deriving a [0,1] UV
                               // from gl_FragCoord (e.g. sampling the half-res cloud composite
                               // targets, which are ALWAYS sized off the true swap extent
                               // regardless of renderScale) must divide by this, not by an assumed
                               // full-res constant, or the result is wrong whenever the two differ.
    float skyGlareVisibility;  // offset 144 — eased sun-glare gate (recordCompute(), see
                               // skyGlareEased member comment); used only by sat_sky.frag's Milky
                               // Way (stars read the CPU-side skyGlareEased directly in
                               // updateStars(), no GPU copy needed for them).
    // (cloudShadowRangeM at 148 and cloudShadowResidualM at 152 lived here — both existed only to
    //  map hitPt.xy into cloud_shadow.comp's observer-centred grid, and to undo that grid's
    //  texel-snapping. The grid is gone; the shadow now arrives in cloudTargetB.a, already
    //  evaluated at this pixel's own terrain hit point. Every field below shifted down 12 bytes.)
    float beamMaxRangeM;     // offset 148 — C12 follow-up #6: settings-tunable "how far around
                             // the observer do Reflect-Orbital beams render" cutoff, mirrors
                             // CloudMarchPC's own copy (same frame's value).
    float beamSkyGlowGain;   // offset 152 — C12 follow-up #18: mirrors CloudMarchPC's own copy so
                             // the ground-spot term (this shader) and the sky glow march
                             // (cloud_march.comp) share one brightness control and read as one
                             // continuous effect instead of two independently-tuned pieces.
    float beamGlowBleedGain; // offset 156 — C12 follow-up #39: moved here from CloudMarchPC — the
                             // near-field volumetric march/bleed in cloud_march.comp was removed
                             // entirely (didn't read as intended even after tuning, added real
                             // cost for no visible benefit); this same slider now drives the
                             // beam-driven sky-illumination wash added to this shader instead (see
                             // the beamGlowDome consumption near the city-glow composite).
    float beamProximityGlow; // offset 160 — C12 follow-up #41: CPU-computed [0,1] "how close is
                             // the observer to any active beam's actual line" value (see
                             // SatelliteSim::beamProximityGlow). Replaces the directional
                             // azimuth-sector dome lookup the wash used in #39/#40 — applied
                             // uniformly regardless of view direction.
    float noTwinkle;         // offset 164 — S3/planets follow-up (RELEASE_v1_1_PLAN.md, session 30):
                             // 0 (default) = normal star_point.vert twinkle/scintillation; 1 = gate
                             // it off. Set only on the planet draw call's own copy of this PC —
                             // real planets are small resolved discs and don't atmospheric-
                             // scintillate the way point-source stars do. Only star_point.vert
                             // reads this; every other consumer of SatDrawPC can ignore it (a GLSL
                             // push_constant declaration only needs to be a PREFIX of the pushed
                             // bytes, so shaders that don't declare it are unaffected).
}; // total: 168 bytes
static_assert(sizeof(SatDrawPC) == 168, "SatDrawPC layout mismatch");

// ── Push constants for cloud_march.comp (half-res cloud compute pass, C15-perf) ──────────────
// Matches the layout(push_constant) block in cloud_march.comp exactly. A separate struct from
// SatDrawPC (own pipeline layout, own push-constant range) — carries only the fields the moved
// cloudMarch/cirrusMarch bodies actually use, plus obsEffH (CPU-computed; the compute shader has
// no elevation-texture lookup of its own, see recordCompute()).
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
    uint32_t debugDisableMask;  // perf knockout toggles — see SatDrawPC's member comment. Needed
                                // here too now that the aurora sky curtain march moved into
                                // cloud_march.comp; mirrors the same debugDisableMask value.
    float beamMaxRangeM;        // offset 132 — C12 follow-up #6: settings-tunable "how far around
                                // the observer do Reflect-Orbital beams render" cutoff, mirrors
                                // SatDrawPC's own copy (same frame's value).
    uint32_t showBeamDebugRays; // offset 136 — C12 follow-up #12: debug-only "draw each active
                                // mirror's actual current pointing direction as a long ray" toggle.
                                // Deliberately NOT part of debugDisableMask — that mask means
                                // "disable this normally-on thing" (0 = normal rendering); this is
                                // the opposite shape ("enable this normally-off extra"), so it gets
                                // its own field rather than overloading that convention.
    float beamSkyGlowGain;      // offset 140 — C12 follow-up #17: settings-tunable brightness for
                                // the beam->cloud illumination term (C12 follow-up #44 — this used
                                // to gain the old analytic sky tube; it's that term's direct
                                // successor, not a new concept, so it keeps the same slider, per
                                // [[feedback_shared_gain_sliders]]).
    float cloudShadowRangeM;    // offset 144 — distance (m) beyond which the per-pixel terrain
                                // cloud shadow fades out and stops being marched. Restored (with a
                                // new meaning) after the 128x128 grid was deleted: that grid had a
                                // hard extent which accidentally kept its cost near zero from
                                // altitude, and dropping it made the per-pixel replacement run over
                                // the whole screen from orbit. Now a smooth fade rather than a hard
                                // edge — see the call site in cloud_march.comp's main().
    // (daySuppression/beamExtinctionMult/beamNearFieldFadeM lived here, at offsets 148/152/156 —
    //  all three existed only for the analytic beam sky-glow block deleted in C12 follow-up #44.
    //  The new per-sample beam-cloud term gates day/night with the march's own per-sample
    //  sampleDayness instead (no CPU-fed ratio needed), and has no separate extinction/near-
    //  field-fade concept — it's a real volumetric term composited through the march's own
    //  transmittance, not a closed-form stand-in that needed its own fade heuristics. The CPU
    //  members themselves (daySuppression, beamNearFieldFadeM) are NOT removed — still read
    //  elsewhere (satellite/star day suppression, SatDrawPC's ground-spot-adjacent crossfade
    //  radius) — only their now-unused mirror copies in this one struct are gone.
    //  beamGlowBleedGain, before that, had already moved to SatDrawPC in follow-up #39.)
}; // total: 148 bytes.
static_assert(sizeof(CloudMarchPC) == 148, "CloudMarchPC layout mismatch");

// ── Per-target beam->cloud light sources (C12 follow-up #44, host-visible) ───────────────────
// Built each frame on the CPU in recordCompute() by aggregating ReflectBeamsBuf's per-satellite
// entries by target position (the same readback loop that already computes lastActiveBeamCount/
// beamProximityGlow — see that comment for why grouping by targetENU needs no target-ID
// plumbing). Consumed by cloud_march.comp's cloudMarchCS as a real per-sample volumetric light
// source, replacing the deleted analytic sky tube. Bounded at kMaxCloudBeamLights regardless of
// how many satellites are servicing any given target — the structural fix for the per-satellite
// cost/flicker problems the original C12 follow-up #14/#16 real-march attempt hit.
// Struct must match BeamCloudLight/BeamCloudLightBuf in cloud_march.comp exactly.
static constexpr int kMaxCloudBeamLights = 16;
struct GpuBeamCloudLight
{
    glm::vec3 targetENU; // meters, observer-relative
    float aggIntensity;  // sum of groundIrradiance*beamGain across every satellite servicing this target
    float footprintRadM; // largest ground footprint radius among contributing satellites
    // blockAltM/blockOpacity (C12 follow-up #46): repurposed from pad0/pad1, zero size change.
    // Copied straight from a contributing GpuReflectBeam entry — beam_cloud_block.comp already
    // computes this ONCE PER TARGET PER FRAME (a 12-step vertical march, not per screen sample),
    // and every satellite servicing the same target reads the identical beamCloudBlockBuf[bestIdx]
    // value, so no re-aggregation logic is needed beyond copying whichever contributing entry's
    // value (see SatelliteSim.cpp's beamCloudLightBuf aggregation). Lets cloud_march.comp derive a
    // height-based directional cutoff (bright above this target's own cloud-opacity altitude, dark
    // below) as a cheap per-light lookup instead of a second live self-shadow march — see that
    // follow-up's log entry for why the live march (#45) was too expensive and got replaced.
    float blockAltM;    // altitude (m) where this target's column first drops below 50% transmittance
    float blockOpacity; // 0 = clear column, 1 = fully opaque
    float pad2;         // std430 array-of-vec4-pairs alignment
};
static_assert(sizeof(GpuBeamCloudLight) == 32, "GpuBeamCloudLight layout mismatch");

struct GpuBeamCloudLights
{
    uint32_t count;
    uint32_t pad0, pad1, pad2;
    GpuBeamCloudLight entries[kMaxCloudBeamLights];
};
static_assert(sizeof(GpuBeamCloudLights) == 16 + kMaxCloudBeamLights * 32, "GpuBeamCloudLights layout mismatch");

// (CloudShadowPC lived here — push constants for cloud_shadow.comp's 128x128 grid, including the
//  shadowResidualM texel-snapping term that stopped shadows swimming as the observer moved. The
//  whole pass is gone; the per-pixel replacement in cloud_march.comp needs no snapping because
//  its value is a function of the world point being shaded, not of the camera's position.)

// ── Push constants for beam_cloud_block.comp (C12 follow-up #33) ─────────────────────────────
// No sun/observer fields needed — this pass evaluates a fixed set of ground targets in true
// ECEF, independent of view direction or observer position.
struct BeamCloudBlockPC
{
    float waveTime;
    float cloudPhase;
}; // total: 8 bytes
static_assert(sizeof(BeamCloudBlockPC) == 8, "BeamCloudBlockPC layout mismatch");

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
    float pad0;                // offset 76 — explicit, aligns the vec4 below to 16
    glm::vec4 obsECEFDir;      // offset 80 — xyz = observer ECEF unit vector, w = height offset
}; // total: 96 bytes
static_assert(sizeof(SceneDepthPC) == 96, "SceneDepthPC layout mismatch");

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
    uint32_t bins[kGlowBins];
};
static_assert(sizeof(GpuGlowBuf) == kGlowBins * 4, "GpuGlowBuf layout mismatch");

// ── Ocean satellite-glint list (flare architecture overhaul) ─────────────────────────────────
// A small, INDEPENDENT capped atomic-append list — same pattern the old flareEntries used, just
// decoupled from corona rendering and much smaller, since ocean specular glint is a minor highlight
// effect, not the primary visibility signal. Written by sat_flare.comp alongside GpuSatVisible;
// read by sat_sky.frag's ocean-glint block, which ALSO gained cloud + terrain occlusion tests at
// each entry's own screen position this round (the previous version had none at all).
static constexpr int kMaxOceanGlints = 32;
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
struct FlareSourcePC
{
    glm::mat4 skyView;      // offset 0
    float fovYRad;          // offset 64
    float aspect;           // offset 68
    uint32_t satCount;      // offset 72 — gl_VertexIndex >= satCount means "this is the sun"
    float sunRefIntensity;  // offset 76 — fixed reference brightness for the sun's virtual point
    glm::vec4 sunDirENU;    // offset 80 — xyz=dir, w=sin(elevation)
    glm::vec2 screenSizePx; // offset 96 — THIS pass's own (small) target size, for the fragment
                            //             shader's occlusion UV — never assume a constant, see
                            //             the resolution-scaling gotcha this codebase already
                            //             learned once for sat_sky.frag.
    float resScale;         // offset 104 — flareExtent / ctx.swapExtent ratio; scales point size
                            //              down to this smaller target so satellites don't look
                            //              oversized once the composite upsamples back to full res
    float pad0;             // offset 108
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

// ── GPU orbital parameters (uploaded once per buildOrbits, device-local) ─────
// 28 × 4-byte fields = 112 bytes.  All plain floats/uints — no vec3 — so
// C++ struct packing matches GLSL std430 without any alignment padding.
// Must match the SatOrbit struct in sat_orbit.comp exactly.
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

    float tumbleRate;      // rotation rate (rad/s); 0 if not tumbling
    float tumblePhase;     // epoch-baked angle: fmod(phase + rate*epochT0, 2π)
    float alignTerminator; // 1.0 = SSO (RAAN precesses); 0.0 = fixed RAAN
    float tumbleAxisX;

    float tumbleAxisY;
    float tumbleAxisZ;
    uint32_t primaryAttitude; // AttitudeMode cast to uint
    uint32_t secondaryAttitude;

    float baseColorR;
    float baseColorG;
    float baseColorB;
    float crossSection; // sqrt(crossSectionM2 / 10)

    float specExp0;
    float specExp1;
    float w1; // secondary surface weight
    float diffuse;

    float mirrorFrac;
    uint32_t constIdx; // constellation index for enabled/highlight masks
    uint32_t pad0;
    uint32_t pad1;
    // Total: 112 bytes
};
static_assert(sizeof(GpuSatOrbit) == 112, "GpuSatOrbit layout mismatch");

// ── Per-frame reflector target upload (host-visible, 201 × 16 bytes) ─────────
// Written by CPU in updatePositions(); read by sat_orbit.comp each frame.
//
// S1 compaction (RELEASE_v1_1_PLAN.md): CPU now packs only the night-side-VALID targets into
// [0, reflectorActiveCount) instead of writing all reflectorTargetCount entries with a 0/1 valid
// flag — `origIdx` (renamed from `valid`) holds the target's ORIGINAL index (reflectorTargetsECEF/
// RadiusM's index, and BeamCloudBlockBuf's index) instead, so sat_orbit.comp's scan never needs to
// skip day-side entries and its winning bestIdx still resolves correctly against
// BeamCloudBlockBuf, which stays indexed by original target index. Entries at or beyond
// reflectorActiveCount are stale/unused — sat_orbit.comp's loop bound is
// SatOrbitPC::activeTargetCount, not kNumReflectorTargets.
struct GpuReflectorTarget
{
    glm::vec3 posECI; // Earth-radius-scaled ECI position
    float origIdx;    // original target index (float; sat_orbit.comp casts back to int) — see above
};
static_assert(sizeof(GpuReflectorTarget) == 16, "GpuReflectorTarget layout mismatch");

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
    glm::vec3 satENU;        // meters, observer-relative (East, North, Up)
    float intensity;         // groundIrradiance * beamGain — NOT the view-dependent
                             // mirrorPeak specular term; see sat_orbit.comp writer comment
    glm::vec3 targetENU;     // meters, observer-relative; exact 3D ENU projection of the
                             // chosen ground target — correctly encodes Earth curvature
    float footprintRadM;     // ground footprint radius
    glm::vec3 reflectDirENU; // unit direction, observer ENU basis — the mirror's ACTUAL current
                             // reflected-sunlight direction (reflect(-sunDirECI, surfN0)), which
                             // may differ from normalize(targetENU-satENU) while the mirror is
                             // still slewing toward bestIdx's target (MIRROR_ROT_RATE-limited).
                             // Debug-only (C12 follow-up #12): drawn as a long "pointing ray" from
                             // the satellite so a busy site's convergence — and any satellites
                             // still mid-slew and not yet converged — can be seen directly.
    float debugPad;          // Repurposed (C12 follow-up #20): carries the originating satellite's
                             // own stable dispatch index (written as float(i) in sat_orbit.comp) —
                             // used by cloud_march.comp's sky glow to downsample by a STABLE subset
                             // of satellites rather than by the atomic-append slot index (which
                             // isn't stable frame-to-frame). Name kept for minimal diff; no longer
                             // debug-only or padding.
    float blockAltM;         // C12 follow-up #33: altitude (m above sea level) at which this
                             // beam's own ground target's vertical cloud column first drops below
                             // 50% transmittance — copied from beamCloudBlockBuf[bestIdx] at write
                             // time (sat_orbit.comp). Irrelevant when blockOpacity==0.
    float blockOpacity;      // 0 = clear column, 1 = fully opaque — see beam_cloud_block.comp.
                             // Consumed by cloud_march.comp (fades the tube/bleed out below the
                             // cloud) and sat_sky.frag (replaces the ground-spot's old, range-
                             // limited cloudShadowTex lookup).
    float mirrorRadiusM;     // C12 follow-up #34: repurposed from padding — equivalent-circle radius
                             // of the physical mirror (sqrt(mirrorAreaM2/PI)). Consumed by
                             // cloud_march.comp (sky tube radius) and sat_sky.frag (ground-spot core).
    float pad1;              // std430 16-byte alignment
};
static_assert(sizeof(GpuReflectBeam) == 64, "GpuReflectBeam layout mismatch");

struct GpuReflectBeams
{
    uint32_t beamCount;                      // atomicAdd counter — total claims this frame, may exceed kMaxActiveBeams
    uint32_t pad0, pad1, pad2;               // std430 array-of-16-byte-aligned-struct alignment padding
    GpuReflectBeam entries[kMaxActiveBeams]; // only entries[0 .. min(beamCount,kMaxActiveBeams)) are valid
};
static_assert(sizeof(GpuReflectBeams) == 16 + kMaxActiveBeams * 64, "GpuReflectBeams layout mismatch");

// Ground-beam compaction (perf follow-up, RELEASE_v1_1_PLAN.md): CPU-built every frame from
// ReflectBeamsBuf's readback (the same loop that already computes lastActiveBeamCount/
// beamProximityGlow/GpuBeamCloudLights below), filtered to entries within beamMaxRangeM of the
// observer — the exact cull sat_sky.frag's ground-spot loop used to redo per-pixel, against the
// FULL raw (up to kMaxActiveBeams=2048) buffer, for every ground-hit pixel on screen. Consumed by
// sat_sky.frag instead of ReflectBeamsBuf directly, so that loop's trip count is bounded by how
// many beams are actually within range of the CAMERA, not by how many are active anywhere across
// the whole visible constellation (measured: disabling the "Reflect-Orbital beams" debug knockout
// bit nearly doubled frame rate — this is the dominant cost that knockout was hiding). Entries are
// raw, unaggregated ReflectBeam records (unlike GpuBeamCloudLights, which sums by target) because
// the ground-spot term needs each satellite's own satENU for its elevation fade.
// Struct must match GroundBeamsBuf in sat_sky.frag exactly.
static constexpr int kMaxGroundBeams = 256;
struct GpuGroundBeams
{
    uint32_t count;
    uint32_t pad0, pad1, pad2;
    GpuReflectBeam entries[kMaxGroundBeams];
};
static_assert(sizeof(GpuGroundBeams) == 16 + kMaxGroundBeams * 64, "GpuGroundBeams layout mismatch");

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
struct GpuCloudParams
{
    // Global controls — shared across all layers
    float coverage;           // global coverage gate [0,1]
    float density;            // global density sharpness scale
    float driftRate;          // base longitude drift rate (rad/s sim-time)
    float sunGain;            // global sun brightness multiplier
    float ambientGain;        // night-side ambient (for future use in volumetrics)
    float hgG;                // Henyey-Greenstein g (C7+ volumetric march)
    float marchSteps;         // volumetric march step count (C7+)
    float lightSteps;         // volumetric light-cone step count (C7+)
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
    float shadowMaxDistM;     // cloudMarch's sun self-shadow cone fades out beyond this distance (m)
    float maxRenderDistM;     // cloudMarch's tExit distance cap (was a hardcoded 80km)
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
    float pad14; // reserved
    float pad15; // reserved
};
static_assert(sizeof(GpuCloudParams) == 400, "GpuCloudParams layout mismatch");

// ── Push constants for sat_orbit.comp ────────────────────────────────────────
// Offsets verified against the push_constant block in sat_orbit.comp.
// Total: 96 bytes.
struct SatOrbitPC
{
    glm::vec4 enuX;             // East  basis in ECI (w unused) — offset 0
    glm::vec4 enuY;             // North basis in ECI (w unused) — offset 16
    glm::vec4 enuZ;             // Up    basis in ECI (w unused) — offset 32
    glm::vec3 sunDirECI;        // unit vector toward sun — offset 48
    float deltaT;               // simTime - epochT0 (float precision) — offset 60
    glm::vec3 obsECI;           // observer ECI position (meters) — offset 64
    uint32_t satCount;          // total satellite count — offset 76
    uint32_t highlightMask;     // bit i = constellation i in highlight mode — offset 80
    uint32_t enabledMask;       // bit i = constellation i is enabled — offset 84
    float simDt;                // simulated seconds this frame (mirror slew) — offset 88
    float elevCutoff;           // sin(Earth-limb angle) — horizon cull threshold (≤ -0.01) — offset 92
    float beamGain;             // Reflect-Orbital ground-beam intensity multiplier — offset 96
    float mirrorSlewDegPerSec;  // offset 100 — C12 follow-up #20: settings-tunable mirror slew
                                // rate (was a hardcoded 1 deg/sec, MIRROR_ROT_RATE in sat_orbit.comp).
                                // At 1 deg/sec a satellite retargeting after the observer moves to a
                                // new area can take minutes to visually catch up — noticeable and
                                // frustrating while exploring interactively, even though it's a
                                // reasonable rate for passive/stationary observation. Default raised
                                // substantially (see SatelliteSim::mirrorSlewDegPerSec) and now
                                // user-tunable instead of fixed. C12 follow-up #34: was offset 104 —
                                // beamFootprintRadM removed entirely (footprint is now physically
                                // derived in sat_orbit.comp from mirror area + range).
    uint32_t activeTargetCount; // offset 104 — S1 (RELEASE_v1_1_PLAN.md) target compaction: number
                                // of VALID entries at the front of ReflectorTargetBuf this frame (CPU
                                // packs only night-side-valid targets there — see updatePositions());
                                // replaces the old fixed NUM_TARGETS=201 scan bound in sat_orbit.comp.
    float minBeamElevSin;       // offset 108 — S1 follow-up: sin(reflectorMinElevDeg), precomputed on
                                // CPU. Candidate targets below this local-elevation-at-target angle are
                                // rejected outright (grazing, not just deprioritized) — see
                                // sat_orbit.comp's TargetedReflector block for the full rationale.
}; // 112 bytes
static_assert(sizeof(SatOrbitPC) == 112, "SatOrbitPC layout mismatch");

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
        azDeg += dmx * sens;
        elDeg -= dmy * sens; // screen Y down → mouse up = negative dmy = increase el
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
};

// ── SatelliteSim ──────────────────────────────────────────────────────────────
class SatelliteSim : public Simulation
{
public:
    const char *name() const override { return "SAT LIGHT SIM"; }

    void init(VulkanContext &ctx) override;
    void onResize(VulkanContext &ctx) override;
    void recordCompute(VkCommandBuffer cmd, VulkanContext &ctx, float dt) override;
    SatDrawPC buildSatDrawPC(VulkanContext &ctx, VkExtent2D targetExtent); // shared by recordPrePass and recordDraw
    void recordPrePass(VkCommandBuffer cmd, VulkanContext &ctx, float dt, uint32_t imgIdx) override;
    VkRenderPass activeRenderPass(VulkanContext &ctx) override { return renderScale < 0.999f ? ctx.renderPassLoad : ctx.renderPass; }
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
    bool wantsCleanScreenshot() const override { return screenshotRequested; }
    void recordScreenshotCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image) override;
    void finalizeScreenshot() override;
    // UC6: shared by KB_SCREENSHOT (dispatchKeyAction) and the left HUD panel's camera button —
    // builds screenshotPath and sets screenshotRequested, or no-ops if a capture is already in
    // flight (copy pending or still encoding).
    void requestScreenshot();
    void cleanup(VkDevice device) override;
    void onKey(GLFWwindow *w, int key, int action) override;
    void onCursorPos(GLFWwindow *w, double x, double y) override;

private:
    // ── SSBOs ─────────────────────────────────────────────────────────────────
    VkBuffer satInputBuf = VK_NULL_HANDLE; // device-local; sat_orbit.comp writes, sat_flare.comp reads
    VkDeviceMemory satInputMem = VK_NULL_HANDLE;
    VkBuffer satVisibleBuf = VK_NULL_HANDLE; // device-local, sat_flare.comp→vertex
    VkDeviceMemory satVisibleMem = VK_NULL_HANDLE;

    // ── Satellite picking / selection tracking ────────────────────────────────
    // pickedVisibleBuf mirrors just the selected satellite's 32-byte GpuSatVisible entry
    // each frame (host-visible, mapped once like glowBuf) so buildUI can reproject its
    // screen position without ever reading back the full (device-local) satVisibleBuf
    // except at the moment of an initial click. See pickSatelliteAt/projectSkyDirToScreen.
    VkBuffer pickedVisibleBuf = VK_NULL_HANDLE;
    VkDeviceMemory pickedVisibleMem = VK_NULL_HANDLE;
    void *pickedVisibleMapped = nullptr;
    int selectedSatIndex = -1;        // index into satOrbits[]/satVisibleBuf; -1 = no selection
    glm::vec3 lastPickedSkyDir{0.0f}; // previous frame's ENU sky direction for the selection
    float lastPickedFlare = 0.0f;     // previous frame's flareIntensity for the selection (>0 = on screen)
    // Cached info text, reformatted only when selectedSatIndex changes (see formatSelectedSatInfo).
    // Separate per-line buffers, not one multi-line string — Clay/UIRenderer text draws a single
    // line per CLAY_TEXT call with no embedded-newline support.
    static constexpr int kSelInfoLines = 6;
    char selInfoLine[kSelInfoLines][40] = {};
    char planetInfoLine[kSelInfoLines][40] = {}; // same shape, filled by formatSelectedPlanetInfo

    // ── Orbit pipeline buffers ────────────────────────────────────────────────
    VkBuffer satOrbitBuf = VK_NULL_HANDLE; // device-local, uploaded once at init
    VkDeviceMemory satOrbitMem = VK_NULL_HANDLE;
    VkBuffer mirrorNormalsBuf = VK_NULL_HANDLE; // device-local, persistent slew state
    VkDeviceMemory mirrorNormalsMem = VK_NULL_HANDLE;
    VkBuffer reflectorTargetsBuf = VK_NULL_HANDLE; // host-visible, updated each frame
    VkDeviceMemory reflectorTargetsMem = VK_NULL_HANDLE;
    void *reflectorTargetsMapped = nullptr;
    // C12 follow-up #33: static ECEF companion to reflectorTargetsBuf (which is ECI, rotates with
    // GMST every frame) — reflectorTargetsECEF[]/reflectorTargetsRadiusM[] never change after
    // target generation, so this is uploaded ONCE (right after initConstellation() in init(), see
    // that call site) rather than refreshed per frame. xyz = unit ECEF direction, w = ground
    // radius incl. terrain elevation. Read by beam_cloud_block.comp, which needs true ECEF (cloud
    // lon/lat sampling) and would otherwise need to redo the GMST rotation reflectorTargetsBuf
    // already has baked in.
    VkBuffer reflectorTargetsECEFBuf = VK_NULL_HANDLE; // host-visible+coherent, written once
    VkDeviceMemory reflectorTargetsECEFMem = VK_NULL_HANDLE;
    void *reflectorTargetsECEFMapped = nullptr;
    // C12 follow-up #33: per-target cloud occlusion result — x=blockAltM, y=blockOpacity, one
    // entry per reflector target (kNumReflectorTargets). Device-local: written every frame by
    // beam_cloud_block.comp (each of the 201 threads owns exactly one index, no atomics, full
    // overwrite every dispatch), read the same frame by sat_orbit.comp — no CPU involvement at
    // all, so no host-visible mapping and no per-frame zero-fill needed (nothing to go stale).
    VkBuffer beamCloudBlockBuf = VK_NULL_HANDLE;
    VkDeviceMemory beamCloudBlockMem = VK_NULL_HANDLE;
    // Reflect-Orbital ground beams (host-visible+coherent; written by sat_orbit.comp, indexed
    // by target identity + atomicMax, zeroed every frame via vkCmdFillBuffer — same pattern as
    // glowBuf, including CPU readback of the previous frame's contents for a diagnostic). Read by
    // cloud_march.comp (debug pointing rays only, as of C12 follow-up #44) and sat_sky.frag
    // (ground-spot direct lighting). See GpuReflectBeams.
    VkBuffer reflectBeamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory reflectBeamsMem = VK_NULL_HANDLE;
    void *reflectBeamsMapped = nullptr;
    // Per-target beam->cloud light sources (C12 follow-up #44) — host-visible+coherent, written
    // by the CPU each frame in recordCompute() (aggregated from reflectBeamsBuf's readback, same
    // place lastActiveBeamCount/beamProximityGlow are computed), read by cloud_march.comp's real
    // per-sample beam illumination term. See GpuBeamCloudLights.
    VkBuffer beamCloudLightBuf = VK_NULL_HANDLE;
    VkDeviceMemory beamCloudLightMem = VK_NULL_HANDLE;
    void *beamCloudLightMapped = nullptr;
    // Ground-beam compaction (perf follow-up) — host-visible+coherent, written by the CPU each
    // frame alongside beamCloudLightBuf (same readback loop, same reasoning). See GpuGroundBeams.
    VkBuffer groundBeamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory groundBeamsMem = VK_NULL_HANDLE;
    void *groundBeamsMapped = nullptr;
    // Diagnostic readback (C12): how many of the 16 sectors currently hold an active beam, and
    // the straight-line distance (meters) from the observer to the nearest one's ground target —
    // one-frame-stale, same idiom as peakMagnitude. -1 = no active beams this/last frame.
    int lastActiveBeamCount = 0;
    float lastNearestBeamDistM = -1.0f;
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
    VkBuffer starBuf = VK_NULL_HANDLE;
    VkDeviceMemory starMem = VK_NULL_HANDLE;
    void *starMapped = nullptr;
    VkDescriptorSetLayout starDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool starDescPool = VK_NULL_HANDLE;
    VkDescriptorSet starDescSet = VK_NULL_HANDLE;
    VkPipelineLayout starPipeLayout = VK_NULL_HANDLE;
    VkPipeline starPipeline = VK_NULL_HANDLE;
    uint32_t starCount = 0;

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
    float obsTerrainH = 0.0f;                         // terrain elevation at observer lat/lon (m)
    float obsHeightOffset = 0.0f;                     // user-controlled height above terrain (m, Q/E/Z)
    uint32_t activeSatCount = 0;
    uint32_t visibleCount = 0;   // above-horizon sats this frame (UI display)
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
    // Bit 512 gates the beam_cloud_block.comp DISPATCH itself, in recordCompute() — no shader
    // reads it. Bits 256 and 512 are the two producer-side knockouts; every other bit disables a
    // consumer block inside a shader.
    uint32_t debugDisableMask = 0;
    // Debug-only, not persisted (same convention as debugDisableMask) — draws each active
    // Reflect-Orbital beam's ACTUAL current mirror-pointing direction as a long ray, so
    // convergence at a busy site (and any satellite still mid-slew, not yet aimed at its target)
    // can be seen directly. See GpuReflectBeam::reflectDirENU and cloud_march.comp (C12 follow-up #12).
    bool showBeamDebugRays = false;

    // ── Sky glow SSBO ─────────────────────────────────────────────────────────
    // Written by sat_flare.comp each frame via binned atomicMax; read by sat_sky.frag.
    VkBuffer glowBuf = VK_NULL_HANDLE;
    VkDeviceMemory glowMem = VK_NULL_HANDLE;
    void *glowMapped = nullptr;

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
    VkImage sceneDepthImg = VK_NULL_HANDLE;
    VkDeviceMemory sceneDepthMem = VK_NULL_HANDLE;
    VkImageView sceneDepthView = VK_NULL_HANDLE;
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
    // Ocean-glint list (see GpuOceanGlintBuf) — device-local, zeroed every frame like glowBuf.
    VkBuffer oceanGlintBuf = VK_NULL_HANDLE;
    VkDeviceMemory oceanGlintMem = VK_NULL_HANDLE;
    // User tunables (Settings > Display), persisted in settings.json. First-pass defaults —
    // expected to need retuning once seen in-app, same as every other constant this session.
    float flareGlowGain = 1.0f;         // post-composite overall multiplier
    float flareStreakGain = 0.35f;      // per-tap streak/godray strength (flare_blur.comp mode=2)
    float sunFlareRefIntensity = 40.0f; // fixed reference brightness for the sun's virtual point
                                        // in the flare-source buffer — NOT a slider (kept small in
                                        // scope this round); tune in code if the sun's godray/glow
                                        // contribution looks under- or over-powered relative to
                                        // satellites once seen in-app.

    // ── Per-target beam cloud block (C12 follow-up #33) ───────────────────────
    // Own small descriptor set/pipeline, modeled on the cloud-shadow one just above but
    // deliberately NOT sharing its observer-centered grid — see beam_cloud_block.comp's header
    // for why. Dispatched with kNumReflectorTargets (201) threads, 64 per workgroup.
    VkDescriptorSetLayout beamCloudBlockDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool beamCloudBlockDescPool = VK_NULL_HANDLE;
    VkDescriptorSet beamCloudBlockDescSet = VK_NULL_HANDLE;
    VkPipelineLayout beamCloudBlockPipeLayout = VK_NULL_HANDLE;
    VkPipeline beamCloudBlockPipeline = VK_NULL_HANDLE;
    // Earth elevation texture (binding 5): 21600×10800 R8_UNORM land-elevation DEM.
    // Pixel p → elevation_m = p * 8848; ocean stored as 0. Terrain shell = R_EARTH + 9000 m.
    VkImage earthElevImg = VK_NULL_HANDLE;
    VkDeviceMemory earthElevMem = VK_NULL_HANDLE;
    VkImageView earthElevView = VK_NULL_HANDLE;
    VkSampler earthElevSampler = VK_NULL_HANDLE;
    uint32_t earthElevMips = 1;
    // CPU-side downsampled elevation for observer height lookup (2160×1080, ~18km/px)
    std::vector<uint8_t> earthElevCpu;
    int earthElevCpuW = 0, earthElevCpuH = 0;
    // CPU-side downsampled night-lights luminance for observer light-pollution lookup
    // (2160×1080, ~18km/px) — single byte per texel, precomputed Rec.709 luminance. Box-filtered
    // (not nearest-neighbor) so it doesn't itself alias before anything samples it.
    std::vector<uint8_t> earthNightCpu;
    int earthNightCpuW = 0, earthNightCpuH = 0;
    // Half-resolution box-blur of earthNightCpu (~37km/px) — updateLightPollutionDome() samples
    // this bilinearly instead of earthNightCpu directly, the CPU-array equivalent of picking a
    // coarser mip level, to smooth the dome's blocky per-sector transitions.
    std::vector<uint8_t> earthNightCpuBlur;
    int earthNightCpuBlurW = 0, earthNightCpuBlurH = 0;

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
    float uiScale = 1.5f;    // text/UI size multiplier (0.75 – 2.0)
    float masterVol_ = 0.8f; // mirrors AudioSystem default (display fallback)
    float musicVol_ = 0.6f;
    float sfxVol_ = 1.0f;
    // ── Photometry tuning (synced to SatFlarePC each frame) ───────────────────
    // Defaults below are the user-tuned values as of the C16 (aurora) session-28 follow-up #21/#22
    // (2026-07-16) commit — baked in from settings.json rather than the original placeholder guesses.
    float brightnessScale = 1.0125f;
    float daySuppression = 574.605f;
    float mirrorBoost = 429.17f;
    float visThresh = 0.0001f;
    float highlightFlare = 0.17066f;
    float moonSuppression = 6.57895f;    // sky background suppression from moonlight (mirrors daySuppression,
                                         // user-tuned value — moon is ~14 magnitudes dimmer than the sun)
    float lightPollutionGain = 6.14035f; // multiplies lightDomeAz[] at the source (updateLightPollutionDome),
                                         // so satellites + stars stay coherently scaled by construction
    float extinctionCoeff = 0.39912f;    // atmospheric extinction, magnitudes per airmass (Kasten & Young
                                         // 1989); ~0.2-0.3 is typical clear-sky sea-level; shared formula
                                         // in both sat_flare.comp and updateStars() so a star and a
                                         // satellite at the same elevation dim identically
    float sunlitBgVisibility = 0.15f;    // Stars/Milky Way visibility fraction in space when the sun is
                                         // off-screen but the observer is still in direct sunlight — 0 =
                                         // fully hidden (like being fully day-suppressed), 1 = as visible as
                                         // true night. Sun-on-screen always forces 0 regardless of this
                                         // slider. See recordCompute()'s sky-glare gate and updateStars().
    // ── Reflect-Orbital ground beams (C12) ────────────────────────────────────
    // groundIrradiance * beamGain is NOT the same quantity as mirrorBoost/mirrorPeak (that's the
    // view-dependent specular term the OBSERVER sees the mirror glint by; this is the physical
    // irradiance the mirror delivers to its ground target, independent of view angle — see
    // sat_orbit.comp's beam-writer comment). Uploaded via SatOrbitPC.
    float beamGain = 0.004023f;
    // C12 follow-up #34: beamFootprintRadM (a flat, tunable constant) removed — the ground
    // footprint is now physically derived in sat_orbit.comp from mirror area + range to target.
    float beamMaxRangeM = 1305172.5f; // C12 follow-up #6 — render-time "is the observer close
                                      // enough to this site" cutoff (site-referenced beams have
                                      // no observer-side write gate any more, see sat_orbit.comp)
    // C12 follow-up #17: simple atmospheric-scattering beam sky glow (replaces the removed real
    // cloud-density march from follow-ups #14-#16, reverted per user request — no cloud lighting
    // yet). Own gain, separate from beamGain (that's the physical ground-irradiance term feeding
    // the ground spot; this purely scales the visual glow's brightness) — dim default, tunable.
    float beamSkyGlowGain = 0.022989f;
    // C12 follow-up #20 raised this to 15 deg/sec by default (was a hardcoded 1 deg/sec constant,
    // MIRROR_ROT_RATE) to reduce a noticeable catch-up delay after the observer moves. Follow-up
    // #21: user preferred the original slew rate/behavior, so the default is back to 1 deg/sec —
    // still exposed as a tunable slider (Settings → Terrain → "Mirror slew rate (deg/s)") in case
    // it's wanted later, just no longer defaulting to the faster value.
    float mirrorSlewDegPerSec = 5.068965f;
    // S1 follow-up (RELEASE_v1_1_PLAN.md): minimum acceptable local elevation angle of the
    // satellite as seen FROM a candidate ground target, in degrees. Below this, a target is
    // rejected outright by sat_orbit.comp's TargetedReflector selection (grazing beams suffer
    // heavy atmospheric extinction and aren't worth taking even as a last resort) rather than
    // being deprioritized — see that shader's own comment for the full rationale, including why
    // this replaced a pure "nearest target" rule. Sent as sin(radians(this)) via
    // SatOrbitPC::minBeamElevSin (recordCompute fills it; the sin conversion happens there, not
    // per-candidate in the shader).
    float reflectorMinElevDeg = 20.0f;
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
    float beamGlowBleedGain = 0.001379f;
    // C12 follow-up #40: radius (meters) of the crossfade blend zone around a beam's own 3D line —
    // was a hardcoded kNearFieldCrossoverM constant in cloud_march.comp, now user-tunable.
    // Per-pixel cloud shadow fade distance. 80 km matches the deleted grid's half-extent, so
    // anything previously shadowed still is; beyond it the shadow was sub-pixel anyway.
    float cloudShadowRangeM = 80000.0f;
    float beamNearFieldFadeM = 71510.867188f;
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
    // Cloud tunables (CPU-side; uploaded to cloudParamsBuf each frame)
    // Defaults below are the user-tuned values as of the C16 (aurora) session-28 follow-up #21/#22
    // (2026-07-16) commit — baked in from settings.json rather than the original placeholder guesses.
    float cloudCoverage = 1.0f;
    float cloudDensity = 8.179311f;
    float cloudBaseAltM = 5585.964844f; // layer 0 shell altitude (low cloud / stratus)
    float cloudTopAltM = 15000.0f;      // layer 1 shell altitude (high cirrus)
    float cloudDriftRate = 1.0438595e-05f;
    float cloudSunGain = 1.471264f;      // near-horizon/sunset sun-gain endpoint — blended toward
                                         // cloudSunGainZenith by sun elevation (see cloud_march.comp)
    float cloudSunGainZenith = 0.45977f; // sun-gain endpoint when the sun is near zenith (midday)
    float cloudAmbientGain = 4.827586f;
    float cloudTwilightAmbientGain = 10.574713f; // manual gain on sky-lit cloud during twilight (was piggybacking
                                                 // on cloudAmbientGain, which also drives city-light
                                                 // upwelling — see kNightSkyAmbientColor in cloud_march.comp)
    float cloudBaseVariance = 0.367816f;         // noise-driven cloud base height undulation, hNorm units
                                                 // (0 = old perfectly flat base) — see cloudMarchCS
    float cloudErosionEdge = 0.965517f;          // cloudDensity() erosion strength at the silhouette edge
    float sunGainElevBand = 0.121379f;           // ~14.5 deg elevation; was effectively 1.0 (half at 30 deg)
    // Brought forward from the original hardcoded 0.15 so the sky term overlaps the tail of
    // direct sunlight instead of starting after it; 0.35 is ~20 deg of sun elevation.
    float twilightBandHi = 0.013793f;
    float twilightBandLo = -0.382759f; // unchanged from the original hardcoded value
    // 1.0 rather than 0.0: a compromise starting point. Lower = more small-scale structure and
    // a closer match to the flat layer, at the cost of worse texture-cache behaviour (mip 0 of
    // the 8K map is ~33 MB and is sampled once per in-cloud march step).
    float coverageMipLod = 0.0f;
    // Measured against the volumetric at MIP 0: volumetric (coverage 1.00, sun gain 0.46)
    // matched flat (coverage 0.69, sun gain 1.84). Defaults encode those ratios so the shared
    // sliders now move both paths together instead of only ever suiting one of them.
    float flatCoverageScale = 0.602299f;
    float flatSunGainScale = 3.968965f;
    // Clouds at 11 km have a ground-level horizon of ~374 km, so this band puts the transition
    // near the horizon when standing on the surface, and makes everything 2D from orbit.
    float cloudDistFadeStartM = 151902.171875f;
    float cloudDistFadeEndM = 399347.8125f;
    // S4 (RELEASE_v1_1_PLAN.md, session 31): terrain-relief march distance fade. Ground-level
    // grazing rays cap at 250 km reach (tCap at obsEffH=0), so 300000 leaves ground view fully
    // unaffected; from LEO (tCap up to 3600 km) most of the screen's grazing/horizon rays fall
    // beyond 900000 and skip the march outright, falling back to the sea-level sphere.
    float terrainDistFadeStartM = 300000.0f;
    float terrainDistFadeEndM = 900000.0f;
    // C11 ground fog layer — real per-sample volumetric march in cloud_march.comp's fogMarchCS,
    // reusing beamCloudLightBuf for beam godrays and a fixed small self-shadow march for sun
    // godrays. First-pass defaults, expect retuning once seen in-app.
    float fogTopAltM = 300.0f;         // shell top altitude (m above sea level); sea level is the base
    float fogDensity = 1.0f;           // density scale, analogous to cloud.density
    float fogCoverage = 0.6f;          // global coverage gate for the patchiness noise, [0,1]
    float fogSunGain = 1.0f;           // sun-lit fog brightness gain, own slider (not cloud.sunGain)
    float cloudErosionCore = 0.37931f; // cloudDensity() erosion strength at the dense core
    float cloudHgG = 0.356053f;
    float cloudMarchSteps = 215.034485f;
    float cloudLightSteps = 12.896552f;
    float cloudCirrusWindDeg = 40.0f;          // C13: cirrus streak wind azimuth (degrees, converted to radians for the UBO)
    float cloudCirrusStretch = 2.184211f;      // C13: cirrus noise anisotropic elongation factor (1 = no stretch)
    float airglowGain = 0.065789f;             // C15: master airglow brightness multiplier
    float airglowGreenGain = 0.052632f;        // C15: green (557.7nm) band gain
    float airglowRedGain = 0.013158f;          // C15: red (630.0nm) band gain — diffuse/broad, keep subtle
    float airglowSodiumGain = 0.065789f;       // C15: sodium (589.3nm) band gain — kept dim relative to green
    float cloudShadowMaxDistM = 22022.988281f; // sun self-shadow cone (N_CONE) fades out beyond this distance
    float cloudMaxRenderDistM = 800000.0f;     // cloudMarch tExit distance cap — raised to ~400km
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
    // artifacts in testing; 6 was clean — round 3); viewSamplesMax is the prior universal fixed
    // value (124), kept as the ceiling for long/grazing rays since that was already proven
    // correct at all altitudes before this change.
    float viewSamplesMin = 6.482759f;
    float viewSamplesMax = 124.689659f;
    float lightSamples = 2.0f;                 // N_LIGHT: optDepth sun-side sub-march count
    float oceanSeaOctaves = 3.0f;              // seaMap() octave count (height-trace geometry)
    float oceanDetailOctaves = 5.0f;           // seaMapDetail() octave count (wave normal)
    float oceanReflSamples = 6.0f;             // ocean sky-reflection loop sample count (N_REFL)
    float moonGain = 0.005263f;                // shared moonlight brightness: terrain direct term + cloud
                                               // moonContrib (default matches the prior hardcoded cloud value)
    float stormStrength = 0.333333f;           // C16: aurora oval expansion/brightness/chaos [0,1]
    float auroraGain = 0.1f;                   // C16: master aurora brightness multiplier
    float auroraCloudGain = 0.001754f;         // C16: ambient aurora light on clouds only (no albedo term
                                               // in that formula, so it needs a much lower default than
                                               // terrain/ocean to land in the same plausible range)
    float auroraGroundGain = 0.007456f;        // C16: ambient aurora light on terrain/ocean only
    float auroraCoverageFreq = 0.426316f;      // C16: coverage patch size (per-degree colat frequency)
    float auroraCoverageAzFreq = 4.289474f;    // C16: coverage azimuthal wobble frequency
    float auroraCoverageDriftRate = 0.001193f; // C16: coverage evolution speed (wall-clock rad/s)
    float auroraShimmerRate = 0.001754f;       // C16: curtain fold noise evolution speed (wall-clock rad/s)
    VulkanContext *ctx_ = nullptr;             // set in init(), used for lazy icon loading
    AudioSystem *audio_ = nullptr;             // set via setAudio(), used in buildUI()
    std::string exeDir_;                       // directory containing the exe (read-only game data); set in init()
    std::string userDataDir_;                  // per-user writable dir for settings/perf (see Paths.h); set in init()

    // ── NEW-3: crash-safe mode ──────────────────────────────────────────────
    // A sentinel file is created at the top of init() and deleted at the bottom of cleanup()
    // (the clean-exit path). If it's already present at the NEXT launch, the previous run never
    // reached cleanup() — crash, hang + force-kill, power loss — so this run forces the
    // Planetarium preset and shows a one-line notice, converting "launch -> crash -> uninstall"
    // into a recoverable outcome. See applySettings-adjacent logic in init()/cleanup().
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
    char introControlsTextBuf[96] = {}; // member buffer for the final WASD/Q-E controls caption (built
                                        // from live keybindings — Clay stores raw string pointers read
                                        // after buildUI returns, so this can't be a stack local; see
                                        // CLAUDE.md's Clay runtime-string rule)

    // Post-intro "graphics set to X" notice (UC1 mechanism 3: always tell the user, never
    // silently re-decide) — same dismissible-banner pattern as buildCrashRecoveryNotice, separate
    // timer/text since the two can in principle be showing different things.
    float graphicsAutoNoticeTimer = 0.0f;
    char graphicsAutoNoticeText[128] = {};

    // ── UC6: screenshots ────────────────────────────────────────────────────────
    // See Simulation.h's wantsCleanScreenshot/recordScreenshotCopy/finalizeScreenshot doc
    // comments for the three-phase (request -> copy -> readback) protocol this drives.
    bool screenshotRequested = false;                     // set by dispatchKeyAction(KB_SCREENSHOT); consumed by
                                                          // recordScreenshotCopy (also gates wantsCleanScreenshot)
    bool screenshotCopyPending = false;                   // true between "copy recorded" and "readback finalized"
    VkBuffer screenshotStagingBuf = VK_NULL_HANDLE;       // host-visible; freed/recreated per capture —
    VkDeviceMemory screenshotStagingMem = VK_NULL_HANDLE; // screenshots are rare, not a hot path
    uint32_t screenshotW = 0, screenshotH = 0;
    VkFormat screenshotFormat = VK_FORMAT_UNDEFINED;
    std::string screenshotPath; // full output path, built at request time
    float screenshotToastTimer = 0.0f;
    char screenshotToastText[160] = {};
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
        KB_MOVE_BOOST = 5,  // held
        KB_MOVE_FINE = 6,   // held
        KB_CINEMATIC = 7,   // event — toggles camera drift mode while panning
        KB_RAISE_ELEV = 8,  // Q — held — raise observer above terrain (gamepad: analog right trigger, see gpElevRaise, not this binding's gpButton)
        KB_LOWER_ELEV = 9,  // E — held — lower observer toward terrain (gamepad: analog left trigger, see gpElevLower)
        KB_RESET_ELEV = 10, // Z — event — snap observer back to terrain elevation
        KB_ZOOM_IN = 11,    // held — narrows FOV (zoom in)
        KB_ZOOM_OUT = 12,   // held — widens FOV (zoom out)
        KB_ZOOM_RESET = 13, // event — snap FOV back to default
        KB_SELECT_SAT = 14, // event — select the satellite nearest the center of the screen
        KB_SCREENSHOT = 15, // event — UC6: capture screenshots/satlight_<timestamp>.png (clean, no UI)
        KB_COUNT = 16,
    };

    // Dispatches the event-style action for keybindings[bindIdx] — shared by onKey()
    // (keyboard) and pollGamepad() (gamepad edge-detect) so the two input paths can never
    // drift apart. No-op for held bindings (MOVE_BOOST/FINE, RAISE/LOWER_ELEV, ZOOM_IN/OUT):
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

    // ── ECI → ENU rotation (updated each frame in updatePositions) ────────────
    // Encodes the surface-fixed observer's local frame in ECI coordinates.
    glm::vec4 eci2enuX{1, 0, 0, 0}; // East  basis in ECI
    glm::vec4 eci2enuY{0, 1, 0, 0}; // North basis in ECI
    glm::vec4 eci2enuZ{0, 0, 1, 0}; // Up    basis in ECI

    // ── Sun + observer state (updated each frame in updatePositions) ──────────
    glm::vec3 sunDirECI{1, 0, 0};    // unit vector from Earth toward Sun in ECI
    glm::vec4 sunDirENU{0, 1, 0, 0}; // sun direction in ENU (xyz), w = sin(elevation)
    glm::vec3 obsECI{0, 0, 6371000}; // observer ECI position (meters)

    // ── Planets (updated each frame in updatePositions/updatePlanets) ─────────
    PlanetState planetStates[kPlanetCount]{};                                // ephemeris; direction/distance/phase
    bool planetEnabled[kPlanetCount] = {true, true, true, true, true, true}; // per-planet toggle
    bool showPlanets = true;                                                 // global toggle, settings-persisted
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
    // S1 compaction: how many of reflectorTargetCount are night-side-valid THIS FRAME — set by
    // updatePositions(), which packs exactly this many valid entries at the front of
    // reflectorTargetsMapped (GpuReflectorTarget[]) each frame instead of uploading all
    // reflectorTargetCount with a per-entry valid flag. Sent to sat_orbit.comp as
    // SatOrbitPC::activeTargetCount so its per-satellite nearest-target scan only walks the real
    // night-side subset (typically well under reflectorTargetCount) instead of skipping day-side
    // entries one by one. See sat_orbit.comp's ReflectorTargetBuf doc comment for the packed
    // layout (w holds the ORIGINAL index, not a 0/1 valid flag, once compacted).
    int reflectorActiveCount = 0;
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
    // Persistent between frames; slews toward the ideal target normal at
    // kMirrorRotRateDegPerSec.  Zero-vector = uninitialized (snaps on first frame).
    std::vector<glm::vec3> satMirrorNormals;

    // ── Satellite type catalogue (defined once in initConstellation) ──────────
    std::vector<SatelliteType> satTypes;

    // ── Orbital parameters (fixed at init, positions computed by GPU) ─────────
    std::vector<ConstellationConfig> constellations;
    std::vector<SatOrbit> satOrbits;

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
    float gpMoveFwd = 0.0f, gpMoveRight = 0.0f;                     // left stick, deadzoned, [-1,1] — combines additively with WASD
    float gpLookYawDeg = 0.0f, gpLookPitchDeg = 0.0f;               // right stick, this-frame look delta in degrees (already dt-scaled)
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

    // ── UC4: gamepad virtual cursor ("cheap 90%" UI navigation) ────────────────
    // Active only while a UI window (Settings/Controls) is open — outside that, the right
    // stick drives camera look as normal (see pollGamepad). vCursorX/Y start at -1 as a
    // "not yet positioned" sentinel; first activation centers on screen.
    float vCursorX = -1.0f, vCursorY = -1.0f;
    bool vCursorActive = false;
    bool vCursorClick = false; // A button currently held (level state, like the real lmb App.cpp
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

    // ── UI hover state (one-frame lag) ────────────────────────────────────────
    std::vector<bool> hovConst;           // one entry per constellation; sized in loadDefinitions()
    std::vector<bool> hovHighlightConst;  // highlight button hover state, parallel to hovConst
    bool hovShowPlanets = false;          // "Show planets" global toggle hover state
    bool hovPlanetBtn[kPlanetCount] = {}; // per-planet ON/OFF toggle hover state
    bool hovTimeSlower = false;
    bool hovTimePause = false;
    bool hovTimeFaster = false;
    bool hovTimeReverse = false;
    bool hovScreenshot = false; // UC6: left HUD panel camera button
    bool hovSettings = false;
    bool hovSettingsClose = false;
    bool hovAltModeToggle = false;
    bool hovViewControlsClose = false;
    bool hovUnitMetric = false;
    bool hovUnitImperial = false;
    bool hovShowControlsStartup = false;
    bool hovTab[11] = {}; // one per settings-window tab
    bool hovScaleMinus = false;
    bool hovScalePlus = false;
    bool hovRenderScaleMinus = false;
    bool hovRenderScalePlus = false;
    bool hovMasterVolMinus = false;
    bool hovMasterVolPlus = false;
    bool hovMusicVolMinus = false;
    bool hovMusicVolPlus = false;
    bool hovSfxVolMinus = false;
    bool hovSfxVolPlus = false;
    bool hovRebind[KB_COUNT] = {};    // per keybinding row — sized to match keybindings vector
    bool hovRebindPad[KB_COUNT] = {}; // per keybinding row, gamepad-button rebind button
    bool hovFullscreen = false;
    bool hovSaveSnapshot = false;
    float snapshotMsgTimer = 0.0f; // seconds remaining to show "Saved" confirmation on the perf snapshot button
    bool hovResetDefaults = false;
    float resetDefaultsMsgTimer = 0.0f;  // seconds remaining to show the "Restart to apply" confirmation (NEW-5)
    bool hovDebugToggle[12] = {};        // one per knockout checkbox (terrain, atmosphere, sun OD, ocean refl, airglow red, aurora, cloud shadow cone, Reflect-Orbital beams, cloud shadow map, beam cloud block dispatch, scene depth pass, fog layer)
    bool hovBeamDebugRaysToggle = false; // hover state for the "Show beam pointing rays" checkbox (C12 follow-up #12)
    // Sized 11, not 9 — flare_glow_gain/flare_streak_gain (flare architecture overhaul) added two
    // more PhotoParam rows; per [[feedback_cloud_slider_arrays]], all three hover/dragging arrays
    // must grow together with any new slider id.
    bool hovPhotoMinus[11] = {};
    bool hovPhotoPlus[11] = {};
    bool draggingPhoto[11] = {};
    bool hovCloudMinus[61] = {}; // was [59] — idx 59-60 are the S4 terrain fade sliders
    bool hovCloudPlus[61] = {};
    bool draggingCloud[61] = {}; // MUST stay sized to match hovCloudMinus/Plus — see
                                 // feedback_cloud_slider_arrays memory: this one was missed once
                                 // already and the out-of-bounds write corrupted the window-chrome
                                 // state declared right below, breaking the settings window.

    // ── Window chrome (drag+resize; see UIRenderer::WindowChrome) ──────────────
    // x/y default to -1 (uninitialized, centers/places on first open); w/h are set
    // once by the owning builder before the first updateWindowChrome() call.
    // Only the two real windows (Settings, View Controls) have chrome — the left/right
    // HUD panels are fixed to their screen corner (see buildLeftHudPanel/buildRightHudPanel),
    // recomputed from screenW/H every frame so they track window resizes automatically.
    WindowChrome settingsChrome;
    WindowChrome viewControlsChrome;
    int settingsActiveTab = 0; // index into the 8 settings tabs (persisted)

    // ── Right HUD panel: altitude display mode + unit system ──────────────────
    bool altModeSeaLevel = true;                // true = MSL (sea level), false = AGL (above terrain)
    UnitSystem unitSystem = UnitSystem::Metric; // Display tab setting; affects altitude readout
    bool showControlsOnStartup = true;          // Display tab setting; gates viewControlsChrome.open in init()

    // ── NEW-7: frame limiter ────────────────────────────────────────────────
    FpsCapMode fpsCapMode = FpsCapMode::VSync;  // Display tab setting; see FpsCapMode comment
    bool fpsCapSwapchainRebuildPending = false; // set true when fpsCapMode changes; see
                                                // consumeSwapchainRebuildRequest() override
    bool hovFpsCap[5] = {};                     // one per FpsCapMode button, same order as the enum

    // ── UC1: graphics preset ─────────────────────────────────────────────────
    // Default High until init() either loads a persisted value or (first run only) seeds one from
    // VkPhysicalDeviceProperties::deviceType — see seedGraphicsPresetFromDevice() in init().
    GraphicsPreset graphicsPreset = GraphicsPreset::High;
    bool showAdvancedSettings = false; // Display tab "Show advanced settings" — reveals the
                                       // Clouds/Ocean/Terrain/Aurora tabs; off by default so a
                                       // new user's front door is Preset + the handful of
                                       // top-level controls, not 46 developer sliders.
    bool hovPreset[5] = {};            // one per named preset button (Custom has no button —
                                       // it's a read-only status, not a click target)
    bool hovAdvancedToggle = false;
    bool hovReplayIntro = false; // UC3 "Replay Intro" button, Display tab

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
        fpsCapSwapchainRebuildPending = true;
    }
    void createBuffers(VulkanContext &ctx);
    void createDescriptors(VulkanContext &ctx);
    void createOrbitDescriptors(VulkanContext &ctx);
    void createOrbitPipeline(VulkanContext &ctx);
    void uploadSatOrbits(VulkanContext &ctx);
    void createCloudNoisePipeline(VulkanContext &ctx);
    void createCloudWarpNoisePipeline(VulkanContext &ctx);
    void createAuroraNoisePipeline(VulkanContext &ctx);
    void createCloudMarchResources(VulkanContext &ctx);
    void createCloudMarchDescriptors(VulkanContext &ctx);
    void createCloudMarchPipeline(VulkanContext &ctx);
    void createSceneDepthResources(VulkanContext &ctx);
    void createSceneDepthDescriptors(VulkanContext &ctx);
    void createSceneDepthPipeline(VulkanContext &ctx);
    void createBeamCloudBlockDescriptors(VulkanContext &ctx);
    void createBeamCloudBlockPipeline(VulkanContext &ctx);
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
    void initStars(VulkanContext &ctx);
    void createStarPipeline(VulkanContext &ctx);
    void updateStars();
    void initPlanets(VulkanContext &ctx);                                       // planetBuf + planetDescSet, reusing starDescLayout
    void updatePlanets();                                                       // mirrors updateStars(); called right after it
    int pickPlanetAt(float clickX, float clickY, float screenW, float screenH); // mirrors
                                                                                // pickSatelliteAt but reads the already host-mapped
                                                                                // planetBuf directly — no device->host staging copy
    void formatSelectedPlanetInfo();                                            // mirrors formatSelectedSatInfo, fills planetInfoLine[]
    void updateLightPollutionDome();                                            // called each frame before updateStars(): fills lightDomeAz[]
                                                                                // + uploads to lightDomeBuf for sat_flare.comp
    void updateGpuTimingStats(VulkanContext &ctx);                              // called at top of recordCompute(): EMA-smooths
                                                                                // ctx.timestampMs into gpuMsSmoothed[]/gpuMsTotalSmoothed
    void initConstellation();                                                   // called once: loads definitions then builds orbits
    void loadDefinitions();                                                     // reads constellations.json; falls back to hardcoded defaults
    void loadHardcoded();                                                       // hardcoded satTypes + constellations (used as fallback)
    void buildOrbits();                                                         // populates satOrbits from satTypes + constellations
    // S1 (RELEASE_v1_1_PLAN.md): reads reflector_targets.json (real solar-farm sites, moddable
    // like constellations.json); falls back to generateReflectorTargetsRandomFallback() if
    // missing/malformed/empty. Called from buildOrbits(), same call-order constraint as the old
    // inline code it replaced (needs earthElevCpu, populated by createGlowResources() earlier in
    // init()). Sets reflectorTargetCount and reflectorObserverSpawnIdx.
    void loadReflectorTargets();
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
    // UC1 first-run seed: VkPhysicalDeviceProperties::deviceType -> Low (integrated/CPU/virtual)
    // or Medium (discrete). Coarse on purpose — see RELEASE_v1_1_PLAN.md UC1, "do not build a
    // GPU-name lookup table." Only called once, from init(), when no persisted preset exists.
    GraphicsPreset seedGraphicsPresetFromDevice(VulkanContext &ctx) const;
    void savePerfSnapshot(float cpuDt);                // appends one profiling record to perf_profiles/profile_log.jsonl
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
    uint16_t fs(int base) const { return (uint16_t)std::max(8, (int)(base * uiScale + 0.5f)); }
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
    bool buildResizableWindow(const UIInput &inp, UIRenderer &ui, WindowChrome &chrome,
                              int winId, const char *title, bool closable, bool &hovCloseFlag,
                              float defaultX, float defaultY,
                              float minW, float minH, float maxW, float maxH,
                              const std::function<void()> &buildBody);

    void buildSettingsWindow(const UIInput &inp, UIRenderer &ui);
    void buildSettingsTabbedBody(const UIInput &inp, UIRenderer &ui);
    void buildSettingsConstellationsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsSoundTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsControlsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsCameraTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsDisplayTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsPhotometryTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsCloudsTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsOceanTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsTerrainTab(const UIInput &inp, UIRenderer &ui);
    void buildSettingsAuroraTab(const UIInput &inp, UIRenderer &ui);
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
    void buildCloudSliderRows(const UIInput &inp, UIRenderer &ui, CloudSlider *sliders, int count);
    void buildViewControlsWindow(const UIInput &inp, UIRenderer &ui);
    void buildViewControlsBody(const UIInput &inp, UIRenderer &ui);
    void buildIntroOverlay(const UIInput &inp, UIRenderer &ui);
    void buildCrashRecoveryNotice(float dt, const UIInput &inp, UIRenderer &ui); // NEW-3
    void buildGraphicsAutoNotice(float dt, const UIInput &inp, UIRenderer &ui);  // UC1 mechanism 3
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
static constexpr float kIntroObserverLatDeg = 35.871456f;
static constexpr float kIntroObserverLonDeg = -121.400291f;
static constexpr float kIntroStartAzDeg = -61.32f;
static constexpr float kIntroStartElDeg = 20.8f;
static constexpr float kIntroStartFovDeg = 70.0f;

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
const float songbeat = 7.61;
static constexpr IntroKeyframe kIntroKeyframes[] = {
    // Beat 0 — "2036" title/date card. Ground, twilight, facing the fixed vantage above.
    {0.0f, 0.0f, kIntroStartAzDeg, kIntroStartElDeg, kIntroStartFovDeg, "2036"},
    // Beat 1 — first narrative line; holds the same framing. The skip hint (buildIntroOverlay)
    // only appears once this beat is reached — see kIntroHintRevealIndex.
    {songbeat * 1.0, 0.0f, kIntroStartAzDeg, kIntroStartElDeg, kIntroStartFovDeg,
     "Satellite megaconstellations dominate the night sky"},
    // Beat 3 — level out in preparation for launch. Still ground level.
    {songbeat * 2.0, 10000.0f, kIntroStartAzDeg - 5, 25.0f, 64.0f, "From the ground, we see the glare of sunlight reflect down from them"},
    // Beat 4 — the pull to LEO begins (ascent happens across THIS beat's transition). Still
    // facing horizontally west, per the storyboard, even as altitude climbs.
    {songbeat * 3.0, 60000.0f, kIntroStartAzDeg - 25, 35.0f, 62.0f, "They power a global network of communication and AI compute."},
    // Beat 5 — pull continues; camera starts rotating away from due-west as we climb high enough
    // to reveal the Earth's curve.
    {songbeat * 4.0, 140000.0f, kIntroStartAzDeg - 35, 20.0f, 62.0f, "A promise to the markets above all else"},
    {songbeat * 5.0, 250000.0f, kIntroStartAzDeg - 45, 10.0, 70.0f, "We will come to miss the quiet sky"},
    // Beat 6 — arrival in LEO: pulled back and up enough to see the backlit AI sats against a
    // rising sun. Camera motion stops here; beats 7-8 hold this exact framing. Title reveal.
    {songbeat * 6.0, 300000.0f, kIntroStartAzDeg - 50, 0.0, 80.0f, "SAT LIGHT SIM"},
    // Beat 7 — controls hint. "WASD to move" is a fixed line (buildIntroOverlay); the Q/E line is
    // generated at render time from live keybindings, not this literal (kIntroControlsIndex marks
    // which entry to override).
    {songbeat * 7.0, 300000.0f, kIntroStartAzDeg - 50, 20.0, 120.0f, "Q / E to raise/lower height"},
    {songbeat * 7.5, 0000.0f, kIntroStartAzDeg - 50, 20.0, 80.0f, nullptr}, // hold, then auto-handoff (finishIntro(false))
};
static constexpr int kIntroKeyframeCount = sizeof(kIntroKeyframes) / sizeof(kIntroKeyframes[0]);
static constexpr int kIntroYearIndex = 0;       // "2036" title/date card
static constexpr int kIntroHintRevealIndex = 1; // skip hint doesn't show before this beat is reached
static constexpr int kIntroTitleIndex = 6;      // "SAT LIGHT SIM" reveal, arrival in LEO
static constexpr int kIntroControlsIndex = 7;   // WASD / Q-E controls hint
// Beats 0-6 (through arrival in LEO, where camera motion stops) feed the UC1 benchmark
// accumulator; the static hold over beats 7-8 isn't representative load, so it's excluded.
static constexpr float kIntroBenchEndT = 39.0f;
static constexpr const char *kGraphicsPresetNames[] = {"Planetarium", "Low", "Medium", "High", "Ultra", "Custom"};
