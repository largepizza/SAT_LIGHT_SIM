#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>

#define GLM_FORCE_DEPTH_ZERO_TO_ONE
#include <glm/glm.hpp>
#include <glm/gtc/constants.hpp>
#include <glm/gtc/matrix_transform.hpp>

#include "../Simulation.h"

#include <string>
#include <vector>
#include <cstdint>
#include <cmath>

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
    float lightPollution;  // 0=none..1=full; dims (not culls) satellite brightness near
                            // brightly-lit cities at low altitude — see SatelliteSim::updateStars
}; // total: 104 bytes
static_assert(sizeof(SatFlarePC) == 104, "SatFlarePC layout mismatch");

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
    glm::vec4 obsECEFDir; // observer ECEF unit vector (xyz); w unused.
                          // Lets sat_sky.frag convert ENU hit → ECEF → geographic lat/lon for texture UV.
}; // total: 128 bytes
static_assert(sizeof(SatDrawPC) == 128, "SatDrawPC layout mismatch");

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
// std140 layout: 64-byte global section (4×vec4) + 4 × 32-byte layer = 192 bytes.
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
    float shadowSteps;      // terrain/ocean cloudShadowFactor march step count (was pad0)
    float cirrusWindAngle;  // C13: cirrus streak wind axis, radians (was pad1)
    float cirrusStretch;    // C13: cirrus noise anisotropic elongation factor (was pad2)
    float airglowGain;        // C15: master airglow brightness multiplier
    float airglowGreenGain;   // C15: green (557.7nm) band gain
    float airglowRedGain;     // C15: red (630.0nm) band gain
    float airglowSodiumGain;  // C15: sodium (589.3nm) band gain — keep dim relative to green
    // Per-layer descriptors
    GpuCloudLayerParams layers[kNumCloudLayers];
};
static_assert(sizeof(GpuCloudParams) == 192, "GpuCloudParams layout mismatch");

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
}; // 96 bytes
static_assert(sizeof(SatOrbitPC) == 96, "SatOrbitPC layout mismatch");

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

    // ── Orbit pipeline buffers ────────────────────────────────────────────────
    VkBuffer satOrbitBuf = VK_NULL_HANDLE; // device-local, uploaded once at init
    VkDeviceMemory satOrbitMem = VK_NULL_HANDLE;
    VkBuffer mirrorNormalsBuf = VK_NULL_HANDLE; // device-local, persistent slew state
    VkDeviceMemory mirrorNormalsMem = VK_NULL_HANDLE;
    VkBuffer reflectorTargetsBuf = VK_NULL_HANDLE; // host-visible, updated each frame
    VkDeviceMemory reflectorTargetsMem = VK_NULL_HANDLE;
    void *reflectorTargetsMapped = nullptr;

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

    // ── Sky glow SSBO ─────────────────────────────────────────────────────────
    // Written by sat_flare.comp each frame via binned atomicMax; read by sat_sky.frag.
    VkBuffer glowBuf = VK_NULL_HANDLE;
    VkDeviceMemory glowMem = VK_NULL_HANDLE;
    void *glowMapped = nullptr;
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
    // Cloud params UBO (binding 9): host-visible, persistently mapped, updated each frame.
    VkBuffer cloudParamsBuf = VK_NULL_HANDLE;
    VkDeviceMemory cloudParamsMem = VK_NULL_HANDLE;
    void *cloudParamsMapped = nullptr;
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
    // (2160×1080, ~18km/px) — single byte per texel, precomputed Rec.709 luminance.
    std::vector<uint8_t> earthNightCpu;
    int earthNightCpuW = 0, earthNightCpuH = 0;
    float lightPollution = 0.0f; // 0=none .. 1=full; computed each frame in recordCompute()

    // ── UI visibility & settings ──────────────────────────────────────────────
    bool showIntro = true; // cinematic intro overlay; dismissed on click or any key
    bool uiVisible = true;
    bool settingsOpen = false;
    bool iconsLoaded = false;
    float uiScale = 1.5f;    // text/UI size multiplier (0.75 – 2.0)
    float masterVol_ = 0.8f; // mirrors AudioSystem default (display fallback)
    float musicVol_ = 0.6f;
    float sfxVol_ = 1.0f;
    // ── Photometry tuning (synced to SatFlarePC each frame) ───────────────────
    // Defaults below are the user-tuned values as of the C15 (airglow) commit — baked in from
    // settings.json rather than the original placeholder guesses.
    float brightnessScale = 1.275f;
    float daySuppression = 1516.64f;
    float mirrorBoost = 429.17f;
    float visThresh = 0.0001f;
    float highlightFlare = 0.17066f;
    // Cloud tunables (CPU-side; uploaded to cloudParamsBuf each frame)
    // Defaults below are the user-tuned values as of the C15 (airglow) commit — baked in from
    // settings.json rather than the original placeholder guesses.
    float cloudCoverage = 0.87281f;
    float cloudDensity = 3.26974f;
    float cloudBaseAltM = 6000.0f; // layer 0 shell altitude (low cloud / stratus)
    float cloudTopAltM = 15000.0f; // layer 1 shell altitude (high cirrus)
    float cloudDriftRate = 1.04386e-5f;
    float cloudSunGain = 1.14035f;
    float cloudAmbientGain = 2.0f;
    float cloudHgG = 0.15632f;
    float cloudMarchSteps = 138.21053f;
    float cloudLightSteps = 4.02632f;
    float cloudShadowSteps = 8.21053f; // terrain/ocean cloud-shadow march step count (separate scale from lightSteps: this covers up to ~60 km, lightSteps covers one shell thickness)
    float cloudCirrusWindDeg = 40.0f; // C13: cirrus streak wind azimuth (degrees, converted to radians for the UBO)
    float cloudCirrusStretch = 4.0f;  // C13: cirrus noise anisotropic elongation factor (1 = no stretch)
    float airglowGain = 0.06579f;         // C15: master airglow brightness multiplier
    float airglowGreenGain = 0.05263f;    // C15: green (557.7nm) band gain
    float airglowRedGain = 0.01316f;      // C15: red (630.0nm) band gain — diffuse/broad, keep subtle
    float airglowSodiumGain = 0.06579f;   // C15: sodium (589.3nm) band gain — kept dim relative to green
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

    // ── TargetedReflector ground targets ──────────────────────────────────────
    // Random lat/lon points generated once at init as unit ECEF vectors.
    // Rotated to ECI each frame in updatePositions; filtered to those on the
    // night side within 30° of the terminator (usefully dark but reachable).
    static constexpr int kNumReflectorTargets = 201;
    glm::vec3 reflectorTargetsECEF[kNumReflectorTargets]{}; // unit ECEF, set by initConstellation

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
    bool hovLatSouth = false;
    bool hovLatNorth = false;
    bool hovSettings = false;
    bool hovSettingsClose = false;
    bool hovScaleMinus = false;
    bool hovScalePlus = false;
    bool hovMasterVolMinus = false;
    bool hovMasterVolPlus = false;
    bool hovMusicVolMinus = false;
    bool hovMusicVolPlus = false;
    bool hovSfxVolMinus = false;
    bool hovSfxVolPlus = false;
    bool hovRebind[KB_COUNT] = {}; // per keybinding row — sized to match keybindings vector
    bool hovFullscreen = false;
    bool hovPhotoMinus[5] = {};
    bool hovPhotoPlus[5] = {};
    bool draggingPhoto[5] = {};
    bool hovCloudMinus[17] = {};
    bool hovCloudPlus[17] = {};
    bool draggingCloud[17] = {};
    // ── Settings window position (persisted; -1 = uninitialized, centers on first open) ─
    float settingsWinX = -1.0f;
    float settingsWinY = -1.0f;
    bool settingsDragging = false;

    // ── Private helpers ───────────────────────────────────────────────────────
    void createBuffers(VulkanContext &ctx);
    void createDescriptors(VulkanContext &ctx);
    void createOrbitDescriptors(VulkanContext &ctx);
    void createOrbitPipeline(VulkanContext &ctx);
    void uploadSatOrbits(VulkanContext &ctx);
    void createCloudNoisePipeline(VulkanContext &ctx);
    void createGlowResources(VulkanContext &ctx);
    void createComputePipeline(VulkanContext &ctx);
    void createSkyBgPipeline(VulkanContext &ctx);
    void createDrawPipeline(VulkanContext &ctx);
    void initStars(VulkanContext &ctx);
    void createStarPipeline(VulkanContext &ctx);
    void updateStars();
    void initConstellation();                        // called once: loads definitions then builds orbits
    void loadDefinitions();                          // reads constellations.json; falls back to hardcoded defaults
    void loadHardcoded();                            // hardcoded satTypes + constellations (used as fallback)
    void buildOrbits();                              // populates satOrbits from satTypes + constellations
    void loadSettings();                             // reads settings.json; silently uses defaults if missing
    void saveSettings();                             // writes settings.json next to exe
    void updatePositions(double t, float dt = 0.0f); // called each frame: fills satInputData + eci2enu
                                                     // dt = simulated seconds elapsed this frame (0 when paused);
                                                     // used for mirror slew rate so behaviour is consistent at all time scales
};

// Time scale options (simulated seconds per real second)
static constexpr float kTimeScales[] = {1.0f, 10.0f, 60.0f, 300.0f, 3600.0f,
                                        86400.0f, 86400.0f * 7.0f, 86400.0f * 30.0f, 86400.0f * 365.0f};
static constexpr const char *kTimeLabels[] = {"1x", "10x", "1m", "5m", "1h", "1d", "1w", "1mo", "1yr"};
static constexpr int kNumTimeScales = 9;
