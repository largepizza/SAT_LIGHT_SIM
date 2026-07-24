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
    float extinctionCoeff;  // atmospheric extinction, magnitudes per airmass (reuses the slot that
                            // was lightPollution — see SatelliteSim::updateLightPollutionDome for
                            // why that moved to the lightDomeBuf SSBO instead of push-constant space)
    float moonSuppression;  // sky background suppression ratio from moonlight (mirrors daySuppression's
                             // role, much smaller in practice — moon is ~14 magnitudes dimmer than the sun)
    float pad0;              // reserved — pads moonDirECI to 16-byte (vec3) alignment
    glm::vec3 moonDirECI;    // unit vector from Earth toward Moon in ECI
    float pad1;              // reserved — rounds struct to 128 bytes
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
    glm::mat4 skyView;    // ENU → camera space
    float fovYRad;        // vertical field of view (radians)
    float aspect;         // viewport width / height
    float gmst;           // Greenwich Mean Sidereal Time (radians)
    float waveTime;       // wall-clock seconds for wave animation (glfwGetTime)
    glm::vec4 sunDirENU;  // sun direction in ENU (xyz unit vec, w = sin(elevation))
    glm::vec4 moonDirENU; // moon direction in ENU (xyz unit vec, w = illuminated fraction)
    glm::vec4 obsECEFDir; // xyz = observer ECEF unit vector (lets sat_sky.frag convert ENU hit →
                          // ECEF → geographic lat/lon for texture UV); w = obsHeightOffset (m,
                          // user altitude offset above terrain — maxed with the GPU's own
                          // ground-height lookup as obsEffH). Despite the field's original "w
                          // unused" comment (stale — corrected here), it IS read.
    uint32_t debugDisableMask; // profiling-only knockout toggles consumed by sat_sky.frag
                               // (dbgSkipTerrain/dbgSkipAtmosphere/dbgSkipSunOD/dbgSkipOceanRefl);
                               // 0 = everything enabled (normal rendering). See Display settings tab.
    float pad0;            // explicit — GLSL push_constant layout aligns the vec2 below to 8
                            // bytes (std430 rules), same as std140/std430 buffers; C++ doesn't
                            // insert this padding automatically the way GLSL requires it, so it
                            // must be here explicitly or screenSizePx reads garbage in the shader.
    glm::vec2 screenSizePx; // CURRENT render target's pixel dimensions (session 29, resolution
                             // scaling) — skyLowResExtent when recordPrePass renders the scaled
                             // background, ctx.swapExtent everywhere else (full-res draws, and
                             // Pass 1 at renderScale==1.0). Needed because gl_FragCoord.xy is
                             // relative to whatever framebuffer THIS draw call targets, not
                             // always the full swapchain — any shader code deriving a [0,1] UV
                             // from gl_FragCoord (e.g. sampling the half-res cloud composite
                             // targets, which are ALWAYS sized off the true swap extent
                             // regardless of renderScale) must divide by this, not by an assumed
                             // full-res constant, or the result is wrong whenever the two differ.
    float skyGlareVisibility; // offset 144 — eased sun-glare gate (recordCompute(), see
                             // skyGlareEased member comment); used only by sat_sky.frag's Milky
                             // Way (stars read the CPU-side skyGlareEased directly in
                             // updateStars(), no GPU copy needed for them).
    float cloudShadowRangeM; // offset 148 — mirrors SatelliteSim::cloudShadowRangeM (C12); lets
                             // sat_sky.frag convert hitPt.xy into the cloud_shadow.comp grid's
                             // own [-range,+range] tangent-plane UV convention. Not shared via
                             // CloudParams UBO to avoid a 3-way struct-duplication growth
                             // (cloud_march.comp/sat_sky.frag/cloud_shadow.comp) for a field only
                             // sat_sky.frag needs.
    glm::vec2 cloudShadowResidualM; // offset 152 — mirrors CloudShadowPC::shadowResidualM (same
                             // frame's value); subtracted from hitPt.xy/targetENU.xy before mapping
                             // to the shadow grid's UV — see that field's comment for why.
    float beamMaxRangeM;     // offset 160 — C12 follow-up #6: settings-tunable "how far around
                             // the observer do Reflect-Orbital beams render" cutoff, mirrors
                             // CloudMarchPC's own copy (same frame's value).
    float beamSkyGlowGain;   // offset 164 — C12 follow-up #18: mirrors CloudMarchPC's own copy so
                             // the ground-spot term (this shader) and the sky glow march
                             // (cloud_march.comp) share one brightness control and read as one
                             // continuous effect instead of two independently-tuned pieces.
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
    uint32_t debugDisableMask; // perf knockout toggles — see SatDrawPC's member comment. Needed
                               // here too now that the aurora sky curtain march moved into
                               // cloud_march.comp; mirrors the same debugDisableMask value.
    float beamMaxRangeM;       // offset 132 — C12 follow-up #6: settings-tunable "how far around
                               // the observer do Reflect-Orbital beams render" cutoff, mirrors
                               // SatDrawPC's own copy (same frame's value).
    uint32_t showBeamDebugRays; // offset 136 — C12 follow-up #12: debug-only "draw each active
                               // mirror's actual current pointing direction as a long ray" toggle.
                               // Deliberately NOT part of debugDisableMask — that mask means
                               // "disable this normally-on thing" (0 = normal rendering); this is
                               // the opposite shape ("enable this normally-off extra"), so it gets
                               // its own field rather than overloading that convention.
    float beamSkyGlowGain;     // offset 140 — C12 follow-up #17: settings-tunable brightness for
                               // the simple atmospheric-scattering beam glow (dim by default,
                               // per [[feedback_shared_gain_sliders]] — its own slider, not
                               // reusing beamGain, which is the physical ground-irradiance term).
    float daySuppression;      // offset 144 — C12 follow-up #28: same daytime-suppression ratio
                               // sat_flare.comp already applies to satellites/stars
                               // (SatelliteSim::daySuppression), mirrored here so beams dim during
                               // the day too instead of rendering at full brightness regardless.
}; // total: 148 bytes
static_assert(sizeof(CloudMarchPC) == 148, "CloudMarchPC layout mismatch");

// ── Push constants for cloud_shadow.comp (shared cloud-shadow primitive, C12) ────────────────
// Fixed 128×128 dispatch, independent of screen resolution/camera — no skyView/fov/aspect needed.
struct CloudShadowPC
{
    glm::vec3 sunDirENU;  float rangeM;     // offset 0/12 — ECEF derived in-shader from obsECEFDir,
                                             // same convention as cloud_march.comp's own sunDirECEF
    glm::vec3 obsECEFDir; float waveTime;   // offset 16/28
    float cloudPhase;                       // offset 32
    float pad0;                             // offset 36 — explicit, for the vec2 below (8-byte align)
    glm::vec2 shadowResidualM;              // offset 40 — texel-snapping residual (meters), see
                                             // SatelliteSim::computeCloudShadowSnap(). Grid center
                                             // (uv=0) represents a texel-quantized world point, not
                                             // the observer's exact continuously-moving position —
                                             // this residual is the small (< 1 texel) gap between
                                             // them, applied here and subtracted by every consumer
                                             // (SatDrawPC's own copy) so a given world-space cloud
                                             // feature maps to a STABLE texel except at whole-texel
                                             // boundary crossings, instead of drifting sub-texel
                                             // every frame as the observer moves — the direct fix
                                             // for shadow "swimming"/flicker during camera motion.
}; // total: 48 bytes
static_assert(sizeof(CloudShadowPC) == 48, "CloudShadowPC layout mismatch");

// Per-frame sky glow + lens flare data, written by sat_flare.comp each frame.
//
//   bins[64]         — Spatial histogram (45°×11.25° cells, 8 az × 8 el).
//                      atomicMax(floatBitsToUint(effectFlare)) per bin.
//                      Used for the wide-Gaussian aggregate sky glow pass.
//
//   flareCount / flareEntries[32]  — Per-satellite sequential slot claim.
//                      Used by sat_sky.frag to call lensFlare() at the real
//                      satellite screen position (spiky corona + ghost artifacts).
//
// std430: bins[64]×uint(256) + flareCount+pad×uint(16) + flareEntries[8]×vec4(128)
//       + sectorBright[8]×uint(32) = 432 bytes total.
// sectorBright[8]: atomicMax per 45°-azimuth sector — stable between frames.
// flareEntries[8]: last-written direction per sector (xyz only; w unused).
static constexpr int kGlowBins = 64;
static constexpr int kMaxFlares = 8;
struct GpuGlowBuf
{
    uint32_t bins[kGlowBins];
    uint32_t flareCount; // unused; kept for layout compat
    uint32_t flarePad[3];
    glm::vec4 flareEntries[kMaxFlares]; // xyz=ENU dir per sector (last-writer)
    uint32_t sectorBright[kMaxFlares];  // floatBitsToUint(max effectFlare) per sector
};
static_assert(sizeof(GpuGlowBuf) == kGlowBins * 4 + 16 + kMaxFlares * 16 + kMaxFlares * 4, "GpuGlowBuf layout mismatch");

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
struct GpuReflectorTarget
{
    glm::vec3 posECI; // Earth-radius-scaled ECI position
    float valid;      // 1.0 = night-side (valid target), 0.0 = dayside
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
    glm::vec3 satENU;      // meters, observer-relative (East, North, Up)
    float intensity;       // groundIrradiance * beamGain — NOT the view-dependent
                           // mirrorPeak specular term; see sat_orbit.comp writer comment
    glm::vec3 targetENU;   // meters, observer-relative; exact 3D ENU projection of the
                           // chosen ground target — correctly encodes Earth curvature
    float footprintRadM;   // ground footprint radius
    glm::vec3 reflectDirENU; // unit direction, observer ENU basis — the mirror's ACTUAL current
                           // reflected-sunlight direction (reflect(-sunDirECI, surfN0)), which
                           // may differ from normalize(targetENU-satENU) while the mirror is
                           // still slewing toward bestIdx's target (MIRROR_ROT_RATE-limited).
                           // Debug-only (C12 follow-up #12): drawn as a long "pointing ray" from
                           // the satellite so a busy site's convergence — and any satellites
                           // still mid-slew and not yet converged — can be seen directly.
    float debugPad;        // Repurposed (C12 follow-up #20): carries the originating satellite's
                           // own stable dispatch index (written as float(i) in sat_orbit.comp) —
                           // used by cloud_march.comp's sky glow to downsample by a STABLE subset
                           // of satellites rather than by the atomic-append slot index (which
                           // isn't stable frame-to-frame). Name kept for minimal diff; no longer
                           // debug-only or padding.
};
static_assert(sizeof(GpuReflectBeam) == 48, "GpuReflectBeam layout mismatch");

struct GpuReflectBeams
{
    uint32_t beamCount;    // atomicAdd counter — total claims this frame, may exceed kMaxActiveBeams
    uint32_t pad0, pad1, pad2; // std430 array-of-16-byte-aligned-struct alignment padding
    GpuReflectBeam entries[kMaxActiveBeams]; // only entries[0 .. min(beamCount,kMaxActiveBeams)) are valid
};
static_assert(sizeof(GpuReflectBeams) == 16 + kMaxActiveBeams * 48, "GpuReflectBeams layout mismatch");

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
    float coverage;         // global coverage gate [0,1]
    float density;          // global density sharpness scale
    float driftRate;        // base longitude drift rate (rad/s sim-time)
    float sunGain;          // global sun brightness multiplier
    float ambientGain;      // night-side ambient (for future use in volumetrics)
    float hgG;              // Henyey-Greenstein g (C7+ volumetric march)
    float marchSteps;       // volumetric march step count (C7+)
    float lightSteps;       // volumetric light-cone step count (C7+)
    float cloudPhase;       // CPU: fmod(driftRate * simTime, 2π) — uploaded each frame
    float extinctionCoeff;  // was pad0 (freed session 23 when cloudShadowFactor was removed); now
                            // carries the same atmospheric-extinction coefficient sat_flare.comp
                            // gets via push constant, so sat_sky.frag's Milky Way term can apply
                            // identical Kasten & Young dimming without its own push-constant field
    float cirrusWindAngle;  // C13: cirrus streak wind axis, radians (was pad1)
    float cirrusStretch;    // C13: cirrus noise anisotropic elongation factor (was pad2)
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
    float stormStrength;     // [0,1] drives oval equatorward expansion, brightness, fold chaos
    float auroraGain;        // master aurora brightness multiplier (sky curtain itself)
    float auroraCloudGain;   // master gain for LOCAL aurora ambient upwelling on CLOUDS only —
                              // split from auroraGroundGain (session 28 follow-up #6) because the
                              // two formulas' magnitudes aren't comparable: clouds have no albedo
                              // term at all (roughly full reflectivity assumed) while terrain/ocean
                              // multiply by the surface's own dark albedo, so one shared slider
                              // couldn't hit "plausible" for both at once.
    float auroraGroundGain;  // master gain for the LOCAL, per-point aurora ambient/reflection
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
    float cloudNightAmbientGain; // gain on cloud_march.comp's kNightSkyAmbientColor floor term —
                                  // deliberately separate from ambientGain (which also drives city-
                                  // light upwelling) so the two can be balanced independently.
                                  // Unused in sat_sky.frag (cloud lighting is cloud_march.comp-only)
                                  // — kept for layout parity.
    float cloudBaseVariance;     // was pad4 — noise-driven cloud base height undulation (hNorm
                                  // units, 0 = old perfectly flat base). See cloudMarchCS.
    float cloudErosionEdge;      // was pad5 — cloudDensity() erosion strength at the silhouette
                                  // edge (base near 0).
    float cloudErosionCore;      // was pad6 — cloudDensity() erosion strength at the dense core
                                  // (base near 1); kept lower than cloudErosionEdge.
};
static_assert(sizeof(GpuCloudParams) == 336, "GpuCloudParams layout mismatch");

// ── Push constants for sat_orbit.comp ────────────────────────────────────────
// Offsets verified against the push_constant block in sat_orbit.comp.
// Total: 96 bytes.
struct SatOrbitPC
{
    glm::vec4 enuX;         // East  basis in ECI (w unused) — offset 0
    glm::vec4 enuY;         // North basis in ECI (w unused) — offset 16
    glm::vec4 enuZ;         // Up    basis in ECI (w unused) — offset 32
    glm::vec3 sunDirECI;    // unit vector toward sun — offset 48
    float deltaT;           // simTime - epochT0 (float precision) — offset 60
    glm::vec3 obsECI;       // observer ECI position (meters) — offset 64
    uint32_t satCount;      // total satellite count — offset 76
    uint32_t highlightMask; // bit i = constellation i in highlight mode — offset 80
    uint32_t enabledMask;   // bit i = constellation i is enabled — offset 84
    float simDt;            // simulated seconds this frame (mirror slew) — offset 88
    float elevCutoff;       // sin(Earth-limb angle) — horizon cull threshold (≤ -0.01) — offset 92
    float beamGain;         // Reflect-Orbital ground-beam intensity multiplier — offset 96
    float beamFootprintRadM;// Reflect-Orbital ground beam footprint radius (m) — offset 100
    float mirrorSlewDegPerSec; // offset 104 — C12 follow-up #20: settings-tunable mirror slew
                            // rate (was a hardcoded 1 deg/sec, MIRROR_ROT_RATE in sat_orbit.comp).
                            // At 1 deg/sec a satellite retargeting after the observer moves to a
                            // new area can take minutes to visually catch up — noticeable and
                            // frustrating while exploring interactively, even though it's a
                            // reasonable rate for passive/stationary observation. Default raised
                            // substantially (see SatelliteSim::mirrorSlewDegPerSec) and now
                            // user-tunable instead of fixed.
}; // 108 bytes
static_assert(sizeof(SatOrbitPC) == 108, "SatOrbitPC layout mismatch");

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

    // ── Orbit pipeline buffers ────────────────────────────────────────────────
    VkBuffer satOrbitBuf = VK_NULL_HANDLE; // device-local, uploaded once at init
    VkDeviceMemory satOrbitMem = VK_NULL_HANDLE;
    VkBuffer mirrorNormalsBuf = VK_NULL_HANDLE; // device-local, persistent slew state
    VkDeviceMemory mirrorNormalsMem = VK_NULL_HANDLE;
    VkBuffer reflectorTargetsBuf = VK_NULL_HANDLE; // host-visible, updated each frame
    VkDeviceMemory reflectorTargetsMem = VK_NULL_HANDLE;
    void *reflectorTargetsMapped = nullptr;
    // Reflect-Orbital ground beams (host-visible+coherent; written by sat_orbit.comp, indexed
    // by target identity + atomicMax, zeroed every frame via vkCmdFillBuffer — same pattern as
    // glowBuf, including CPU readback of the previous frame's contents for a diagnostic). Read by
    // cloud_march.comp (volumetric in-scatter) and sat_sky.frag (ground-spot direct lighting).
    // See GpuReflectBeams.
    VkBuffer reflectBeamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory reflectBeamsMem = VK_NULL_HANDLE;
    void *reflectBeamsMapped = nullptr;
    // Diagnostic readback (C12): how many of the 16 sectors currently hold an active beam, and
    // the straight-line distance (meters) from the observer to the nearest one's ground target —
    // one-frame-stale, same idiom as peakMagnitude. -1 = no active beams this/last frame.
    int lastActiveBeamCount = 0;
    float lastNearestBeamDistM = -1.0f;

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
    float renderScale = 1.0f; // [0.5, 1.0], Settings > Display "Render scale"
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
    int timeScaleIdx = 1;
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
    float gpuMsSmoothed[7] = {};     // cloud march, cloud shadow map (C12), orbit compute, flare
                                      // compute, sky background draw, satellite+star draw, UI overlay
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
    // ── Shared cloud-shadow primitive (C12) ───────────────────────────────────
    // Fixed 128×128 R16_SFLOAT grid, independent of screen resolution — NOT recreated in
    // onResize. Written by cloud_shadow.comp; sampled by sat_sky.frag (binding 18) for both
    // general cloud-shadow-on-ground and the Reflect-Orbital beam ground-spot term.
    static constexpr int kCloudShadowGridRes = 128;
    VkImage cloudShadowImg = VK_NULL_HANDLE;
    VkDeviceMemory cloudShadowMem = VK_NULL_HANDLE;
    VkImageView cloudShadowView = VK_NULL_HANDLE;
    VkSampler cloudShadowSampler = VK_NULL_HANDLE;
    VkDescriptorSetLayout cloudShadowDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool cloudShadowDescPool = VK_NULL_HANDLE;
    VkDescriptorSet cloudShadowDescSet = VK_NULL_HANDLE;
    VkPipelineLayout cloudShadowPipeLayout = VK_NULL_HANDLE;
    VkPipeline cloudShadowPipeline = VK_NULL_HANDLE;
    float cloudShadowRangeM = 80000.0f; // tangent-plane grid half-extent (settings-tunable, C12 step 6)
    // Texel-snapping residual (meters) computed once per frame in recordCompute() (alongside
    // CloudShadowPC) and reused by buildSatDrawPC() so both the grid-builder and every consumer
    // agree on the same frame's snap — see CloudShadowPC::shadowResidualM's comment for why.
    glm::vec2 cloudShadowResidualM{0.0f, 0.0f};
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
    bool showIntro = true; // cinematic intro overlay; dismissed on click or any key
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
    float moonSuppression = 6.57895f; // sky background suppression from moonlight (mirrors daySuppression,
                                   // user-tuned value — moon is ~14 magnitudes dimmer than the sun)
    float lightPollutionGain = 6.14035f; // multiplies lightDomeAz[] at the source (updateLightPollutionDome),
                                      // so satellites + stars stay coherently scaled by construction
    float extinctionCoeff = 0.39912f;   // atmospheric extinction, magnitudes per airmass (Kasten & Young
                                      // 1989); ~0.2-0.3 is typical clear-sky sea-level; shared formula
                                      // in both sat_flare.comp and updateStars() so a star and a
                                      // satellite at the same elevation dim identically
    float sunlitBgVisibility = 0.15f; // Stars/Milky Way visibility fraction in space when the sun is
                                    // off-screen but the observer is still in direct sunlight — 0 =
                                    // fully hidden (like being fully day-suppressed), 1 = as visible as
                                    // true night. Sun-on-screen always forces 0 regardless of this
                                    // slider. See recordCompute()'s sky-glare gate and updateStars().
    // ── Reflect-Orbital ground beams (C12) ────────────────────────────────────
    // groundIrradiance * beamGain is NOT the same quantity as mirrorBoost/mirrorPeak (that's the
    // view-dependent specular term the OBSERVER sees the mirror glint by; this is the physical
    // irradiance the mirror delivers to its ground target, independent of view angle — see
    // sat_orbit.comp's beam-writer comment). Uploaded via SatOrbitPC.
    float beamGain = 1.0f;
    float beamFootprintRadM = 50000.0f; // ground footprint radius; tunable constant for now
    float beamMaxRangeM = 500000.0f; // C12 follow-up #6 — render-time "is the observer close
                                      // enough to this site" cutoff (site-referenced beams have
                                      // no observer-side write gate any more, see sat_orbit.comp)
    // C12 follow-up #17: simple atmospheric-scattering beam sky glow (replaces the removed real
    // cloud-density march from follow-ups #14-#16, reverted per user request — no cloud lighting
    // yet). Own gain, separate from beamGain (that's the physical ground-irradiance term feeding
    // the ground spot; this purely scales the visual glow's brightness) — dim default, tunable.
    float beamSkyGlowGain = 0.05f;
    // C12 follow-up #20 raised this to 15 deg/sec by default (was a hardcoded 1 deg/sec constant,
    // MIRROR_ROT_RATE) to reduce a noticeable catch-up delay after the observer moves. Follow-up
    // #21: user preferred the original slew rate/behavior, so the default is back to 1 deg/sec —
    // still exposed as a tunable slider (Settings → Terrain → "Mirror slew rate (deg/s)") in case
    // it's wanted later, just no longer defaulting to the faster value.
    float mirrorSlewDegPerSec = 1.0f;
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
    float cloudCoverage = 0.61404f;
    float cloudDensity = 5.48421f;
    float cloudBaseAltM = 5585.96f; // layer 0 shell altitude (low cloud / stratus)
    float cloudTopAltM = 15000.0f; // layer 1 shell altitude (high cirrus)
    float cloudDriftRate = 1.04386e-5f;
    float cloudSunGain = 3.35526f; // near-horizon/sunset sun-gain endpoint — blended toward
                                    // cloudSunGainZenith by sun elevation (see cloud_march.comp)
    float cloudSunGainZenith = 1.0f; // sun-gain endpoint when the sun is near zenith (midday)
    float cloudAmbientGain = 1.86842f;
    float cloudNightAmbientGain = 1.0f; // decoupled night-sky floor on cloud tops (was piggybacking
                                         // on cloudAmbientGain, which also drives city-light
                                         // upwelling — see kNightSkyAmbientColor in cloud_march.comp)
    float cloudBaseVariance = 0.3f; // noise-driven cloud base height undulation, hNorm units
                                     // (0 = old perfectly flat base) — see cloudMarchCS
    float cloudErosionEdge = 0.5f;  // cloudDensity() erosion strength at the silhouette edge
    float cloudErosionCore = 0.15f; // cloudDensity() erosion strength at the dense core
    float cloudHgG = 0.27355f;
    float cloudMarchSteps = 75.57895f;
    float cloudLightSteps = 5.73684f;
    float cloudCirrusWindDeg = 40.0f; // C13: cirrus streak wind azimuth (degrees, converted to radians for the UBO)
    float cloudCirrusStretch = 4.0f;  // C13: cirrus noise anisotropic elongation factor (1 = no stretch)
    float airglowGain = 0.06579f;         // C15: master airglow brightness multiplier
    float airglowGreenGain = 0.05263f;    // C15: green (557.7nm) band gain
    float airglowRedGain = 0.01316f;      // C15: red (630.0nm) band gain — diffuse/broad, keep subtle
    float airglowSodiumGain = 0.06579f;   // C15: sodium (589.3nm) band gain — kept dim relative to green
    float cloudShadowMaxDistM = 28688.6f; // sun self-shadow cone (N_CONE) fades out beyond this distance
    float cloudMaxRenderDistM = 396315.78f; // cloudMarch tExit distance cap — raised to ~400km
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
    float viewSamplesMin = 5.68421f;
    float viewSamplesMax = 135.15790f;
    float lightSamples = 12.0f;        // N_LIGHT: optDepth sun-side sub-march count
    float oceanSeaOctaves = 3.0f;      // seaMap() octave count (height-trace geometry)
    float oceanDetailOctaves = 5.0f;   // seaMapDetail() octave count (wave normal)
    float oceanReflSamples = 6.0f;     // ocean sky-reflection loop sample count (N_REFL)
    float moonGain = 0.00526f;         // shared moonlight brightness: terrain direct term + cloud
                                        // moonContrib (default matches the prior hardcoded cloud value)
    float stormStrength = 0.32018f;    // C16: aurora oval expansion/brightness/chaos [0,1]
    float auroraGain = 0.1f;           // C16: master aurora brightness multiplier
    float auroraCloudGain = 0.00395f;  // C16: ambient aurora light on clouds only (no albedo term
                                        // in that formula, so it needs a much lower default than
                                        // terrain/ocean to land in the same plausible range)
    float auroraGroundGain = 0.00746f; // C16: ambient aurora light on terrain/ocean only
    float auroraCoverageFreq = 0.42632f;      // C16: coverage patch size (per-degree colat frequency)
    float auroraCoverageAzFreq = 4.28947f;    // C16: coverage azimuthal wobble frequency
    float auroraCoverageDriftRate = 0.0019386f; // C16: coverage evolution speed (wall-clock rad/s)
    float auroraShimmerRate = 0.00175f; // C16: curtain fold noise evolution speed (wall-clock rad/s)
    VulkanContext *ctx_ = nullptr; // set in init(), used for lazy icon loading
    AudioSystem *audio_ = nullptr; // set via setAudio(), used in buildUI()
    std::string exeDir_;           // directory containing the exe; set in init()

    // ── Key bindings (editable in the settings window) ────────────────────────
    // All interactive keys go here — both event keys (pressed once) and held keys
    // (polled each frame).  Adding a new control is one line in the keybindings
    // initializer; the settings window and rebind UI are driven entirely from this
    // vector so no other plumbing is needed.
    //
    // held=false  → dispatched in onKey() via pressed(idx)
    // held=true   → polled in recordCompute() via glfwGetKey(win, keybindings[idx].key)
    struct KeyBinding
    {
        const char *action;
        int key;
        bool held = false; // true = polled (held modifier), false = event (pressed once)
        bool listening = false;
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
        KB_RAISE_ELEV = 8,  // Q — held — raise observer above terrain
        KB_LOWER_ELEV = 9,  // E — held — lower observer toward terrain
        KB_RESET_ELEV = 10, // Z — event — snap observer back to terrain elevation
        KB_COUNT = 11,
    };

    // ── ECI → ENU rotation (updated each frame in updatePositions) ────────────
    // Encodes the surface-fixed observer's local frame in ECI coordinates.
    glm::vec4 eci2enuX{1, 0, 0, 0}; // East  basis in ECI
    glm::vec4 eci2enuY{0, 1, 0, 0}; // North basis in ECI
    glm::vec4 eci2enuZ{0, 0, 1, 0}; // Up    basis in ECI

    // ── Sun + observer state (updated each frame in updatePositions) ──────────
    glm::vec3 sunDirECI{1, 0, 0};    // unit vector from Earth toward Sun in ECI
    glm::vec4 sunDirENU{0, 1, 0, 0}; // sun direction in ENU (xyz), w = sin(elevation)
    glm::vec3 obsECI{0, 0, 6371000}; // observer ECI position (meters)

    // ── Sky-background sun-glare gate (hysteresis state, not persisted) ───────
    // Eased toward its per-frame target in recordCompute(), right after updatePositions() and
    // before updateStars(). Consumed by updateStars() (folded into nightFactorEff) and pushed to
    // sat_sky.frag via SatDrawPC for the Milky Way. See recordCompute() for the target/easing
    // logic and rationale (asymmetric fast-dim/slow-recover rates).
    float skyGlareEased = 1.0f;

    // ── TargetedReflector ground targets ──────────────────────────────────────
    // Random lat/lon points generated once at init as unit ECEF vectors.
    // Rotated to ECI each frame in updatePositions; filtered to those on the
    // night side within 30° of the terminator (usefully dark but reachable).
    static constexpr int kNumReflectorTargets = 201;
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
    std::vector<bool> hovConst;          // one entry per constellation; sized in loadDefinitions()
    std::vector<bool> hovHighlightConst; // highlight button hover state, parallel to hovConst
    bool hovTimeSlower = false;
    bool hovTimePause = false;
    bool hovTimeFaster = false;
    bool hovTimeReverse = false;
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
    bool hovRebind[KB_COUNT] = {}; // per keybinding row — sized to match keybindings vector
    bool hovFullscreen = false;
    bool hovSaveSnapshot = false;
    float snapshotMsgTimer = 0.0f; // seconds remaining to show "Saved" confirmation on the perf snapshot button
    bool hovDebugToggle[9] = {};   // one per knockout checkbox (terrain, atmosphere, sun OD, ocean refl, airglow red, aurora, cloud shadow cone, Reflect-Orbital beams, cloud shadow map)
    bool hovBeamDebugRaysToggle = false; // hover state for the "Show beam pointing rays" checkbox (C12 follow-up #12)
    bool hovPhotoMinus[9] = {};
    bool hovPhotoPlus[9] = {};
    bool draggingPhoto[9] = {};
    bool hovCloudMinus[44] = {}; // was [43] — idx 43 is C12 follow-up #20's new "Mirror slew rate" slider
    bool hovCloudPlus[44] = {};
    bool draggingCloud[44] = {}; // MUST stay sized to match hovCloudMinus/Plus — see
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
    bool altModeSeaLevel = true;                        // true = MSL (sea level), false = AGL (above terrain)
    UnitSystem unitSystem = UnitSystem::Metric;          // Display tab setting; affects altitude readout
    bool showControlsOnStartup = true;                   // Display tab setting; gates viewControlsChrome.open in init()

    // ── Private helpers ───────────────────────────────────────────────────────
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
    void createCloudShadowResources(VulkanContext &ctx);
    void createCloudShadowDescriptors(VulkanContext &ctx);
    void createCloudShadowPipeline(VulkanContext &ctx);
    void createGlowResources(VulkanContext &ctx);
    void createComputePipeline(VulkanContext &ctx);
    void createSkyBgPipeline(VulkanContext &ctx);
    void createSkyLowResResources(VulkanContext &ctx); // resolution scaling (session 29)
    void destroySkyLowResResources(VkDevice device);   // called from onResize (before recreate) and cleanup
    void createDrawPipeline(VulkanContext &ctx);
    void initStars(VulkanContext &ctx);
    void createStarPipeline(VulkanContext &ctx);
    void updateStars();
    void updateLightPollutionDome(); // called each frame before updateStars(): fills lightDomeAz[]
                                      // + uploads to lightDomeBuf for sat_flare.comp
    void updateGpuTimingStats(VulkanContext &ctx); // called at top of recordCompute(): EMA-smooths
                                                    // ctx.timestampMs into gpuMsSmoothed[]/gpuMsTotalSmoothed
    void initConstellation();                        // called once: loads definitions then builds orbits
    void loadDefinitions();                          // reads constellations.json; falls back to hardcoded defaults
    void loadHardcoded();                            // hardcoded satTypes + constellations (used as fallback)
    void buildOrbits();                              // populates satOrbits from satTypes + constellations
    void loadSettings();                             // reads settings.json; silently uses defaults if missing
    void saveSettings();                             // writes settings.json next to exe
    void savePerfSnapshot(float cpuDt);               // appends one profiling record to perf_profiles/profile_log.jsonl
    void updatePositions(double t, float dt = 0.0f); // called each frame: fills satInputData + eci2enu
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
    void setLat(float newLatDeg); // moves observer to a new latitude; used by the right panel's lat display scroll-adjust
    void adjustLon(float deltaDeg); // rotates observer around Earth's polar axis; right panel's lon display scroll-adjust
                                                     // dt = simulated seconds elapsed this frame (0 when paused);
                                                     // used for mirror slew rate so behaviour is consistent at all time scales
};

// Time scale options (simulated seconds per real second)
static constexpr float kTimeScales[] = {1.0f, 10.0f, 60.0f, 300.0f, 3600.0f,
                                        86400.0f, 86400.0f * 7.0f, 86400.0f * 30.0f, 86400.0f * 365.0f};
static constexpr const char *kTimeLabels[] = {"1x", "10x", "1m", "5m", "1h", "1d", "1w", "1mo", "1yr"};
static constexpr int kNumTimeScales = 9;
