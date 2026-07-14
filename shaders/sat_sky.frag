#version 450

// ── Camera + sun push constants (same layout as C++ SatDrawPC, 128 bytes) ─────
// The pipeline layout declares VK_SHADER_STAGE_VERTEX_BIT|FRAGMENT_BIT so both
// stages share one push constant range.  The fragment uses skyView/fovYRad to
// project glowBuf ENU directions into screen UV for the lens flare pass.
layout(push_constant) uniform PC {
    mat4  skyView;     // ENU -> camera space (rotation, no translation)
    float fovYRad;     // vertical field of view in radians
    float aspect;      // viewport width / height
    float gmst;        // Greenwich Mean Sidereal Time (radians)
    float waveTime;    // wall-clock seconds for wave animation
    vec4  sunDirENU;   // xyz = sun dir in ENU, w = sin(sun elevation)
    vec4  moonDirENU;  // xyz = moon dir in ENU, w = illuminated fraction
    vec4  obsECEFDir;  // xyz = observer ECEF unit vector; w unused
} pc;

layout(location = 0) in  vec3 enuDir;           // interpolated ENU view ray (not normalised)
layout(location = 1) in flat vec4 sunDirENU;    // passed through from vertex (same as pc.sunDirENU)
layout(location = 2) in flat vec4 moonDirENU;   // moon dir + phase pass-through

// Sky glow + lens flare data, written by sat_flare.comp each frame.
// Must match GpuGlowBuf (SatelliteSim.h) exactly.
layout(std430, set = 0, binding = 0) readonly buffer GlowBuf {
    uint  bins[64];          // sky glow: floatBitsToUint(max effectFlare) per bin
    uint  flareCount;        // unused; kept for layout compat
    uint  flarePad[3];
    vec4  flareEntries[8];   // xyz=ENU dir per sector (last-writer within 45°-az sector)
    uint  sectorBright[8];   // floatBitsToUint(max effectFlare) per sector — stable
} glowBuf;

// RGBA noise texture (binding 1): tiled REPEAT sampler, used for angular corona
// variation in lensFlare().  Replaces the original ShaderToy's iChannel0 lookup.
layout(set = 0, binding = 1) uniform sampler2D noiseTex;

// Moon surface texture (binding 2): near-side face disc image.
// Sampled with an orthographic projection of the surface normal onto the moon's
// local face frame — maps the near hemisphere to the full [0,1] UV range.
layout(set = 0, binding = 2) uniform sampler2D moonTex;

// Earth textures (bindings 3-6): 8K equirectangular maps.
// UV derived from ENU hit point → ECEF → geographic lat/lon.
// earthDayTex:   SRGB colour map (auto-linearised on read).
// earthNightTex: SRGB city-light map.
// earthElevTex:  R8_UNORM land elevation. Ocean baseline = 15/255; land = (p - 15/255) * 8848 m.
// earthSpecTex:  R8_UNORM ocean mask (white=ocean, black=land). Used for wave material.
layout(set = 0, binding = 3) uniform sampler2D earthDayTex;
layout(set = 0, binding = 4) uniform sampler2D earthNightTex;
layout(set = 0, binding = 5) uniform sampler2D earthElevTex;
layout(set = 0, binding = 6) uniform sampler2D earthSpecTex;
layout(set = 0, binding = 7) uniform sampler2D earthCloudsTex;

// Cloud 3D noise volume (binding 8): 128³ RGBA, baked by cloud_noise.comp at init.
// R = Perlin-Worley FBM (base shape), G/B/A = inverted Worley erosion octaves.
layout(set = 0, binding = 8) uniform sampler3D cloudNoiseTex;

// Cloud / volumetrics tunables (binding 9).
// cloudPhase is CPU-computed fmod(driftRate * simTime, 2π) and uploaded each frame.
// std140: 96-byte global section (6×vec4) + 4×32-byte CloudLayer = 224 bytes total.
struct CloudLayer {
    float shellAltM;    // sphere-shell altitude above R_EARTH (m)
    float driftMult;    // cloudPhase longitude multiplier
    float alphaMax;     // maximum opacity [0,1]
    float mipLod;       // fixed texture LOD
    float coverageMult; // per-layer coverage scale
    float densityMult;  // per-layer density scale
    float enabled;      // 1.0 = active
    float pad;
};
layout(set = 0, binding = 9) uniform CloudParams {
    float coverage;
    float density;
    float driftRate;
    float sunGain;
    float ambientGain;
    float hgG;
    float marchSteps;
    float lightSteps;
    float cloudPhase;
    float pad0;             // reserved — was shadowSteps (cloudShadowFactor's step count); that
                            // function was removed session 23 (dominant surface cloud cost,
                            // cloud shadowing on terrain/ocean unused), freeing this slot again
    float cirrusWindAngle;  // C13: cirrus streak wind axis, radians (was pad1)
    float cirrusStretch;    // C13: cirrus noise anisotropic elongation factor (was pad2)
    float airglowGain;        // C15: master airglow brightness multiplier
    float airglowGreenGain;   // C15: green (557.7nm) band gain
    float airglowRedGain;     // C15: red (630.0nm) band gain
    float airglowSodiumGain;  // C15: sodium (589.3nm) band gain — keep dim relative to green
    float shadowMaxDistM;     // cloudMarch's sun self-shadow cone fades out beyond this distance (m)
    float maxRenderDistM;     // cloudMarch's tExit distance cap (was a hardcoded 80km)
    float viewSamples;        // perf (session 24): N_VIEW atmosphere-loop sample count (was pad2)
    float lightSamples;       // perf (session 24): N_LIGHT optDepth sub-march count (was pad3)
    float oceanSeaOctaves;    // perf (session 24): seaMap() octave count (height-trace geometry)
    float oceanDetailOctaves; // perf (session 24): seaMapDetail() octave count (wave normal)
    float oceanReflSamples;   // perf (session 24): ocean sky-reflection loop sample count (N_REFL)
    float pad4;               // reserved
    CloudLayer layers[4];
} cloud;

// Half-resolution cloud march output (written by cloud_march.comp, see the "velvet-rolling-
// squirrel" plan / TERRAIN_PLAN.md session 23 log). Replaces the old inline cirrusMarch()/
// cloudMarch() calls in main() below — those functions moved to that compute shader.
// Target A: rgb = combined additive radiance (B_total), a = tCloudOcclude (m, -1 = none).
// Target B: rgb = combined multiplicative attenuation (A_total), a = cloudBlock (sun-dim scalar).
layout(set = 0, binding = 10) uniform sampler2D cloudTargetA;
layout(set = 0, binding = 11) uniform sampler2D cloudTargetB;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// ── Atmosphere geometry (meters) ───────────────────────────────────────────────
const float R_EARTH = 6371000.0;
const float R_ATMOS = 6471000.0;   // 100 km above surface

// ── Cloud noise domain frequencies ─────────────────────────────────────────────
// Cloud procedural noise (cloudNoiseTex) is sampled by TRUE 3D unit-sphere position
// (dirECEF = normalize(pECEF)), not by lat/lon UV. A sphere embedded in R^3 carries its
// natural induced metric, so this has no pole singularity and no latitude-dependent scale
// distortion — unlike an equirectangular UV, whose atan2/asin derivatives blow up at the
// poles (causing both the visible polar noise compression and a real perf hit, since the
// raymarch's empty-air skip gets defeated by aliased density near the poles).
// dirECEF is also altitude-invariant by construction (same value straight up/down at a
// given lat/lon), so cloud "presence" shape naturally has no unwanted Z-sweep with no hack
// needed. Frequencies are ~(old UV-space tile count)/(2*PI), since dirECEF isn't normalized
// to a 0-1 globe fraction the way pUV was — retune visually, these are starting points.
const float kCloudHorizFreq = 480.0;   // ~13 km detail features (was kHorizTiles=3000 in pUV space)
const float kCloudColFreq   = 80.0;    // ~20 km cloud-system footprints (was kColTiles=500)

// ── Domain warp: breaks single-frequency tiling + gives weather-system curviness ──────────
// The 192³ baked noise volume is reused at ONE fixed frequency (kCloudHorizFreq/kCloudColFreq)
// across the ENTIRE globe. From ground level you never see enough footprint at once to notice
// the repeat, but from LEO the same tile period becomes an obvious repeating pattern. Standard
// fix (Inigo Quilez's domain-warping technique): offset the sampling coordinate by a second,
// much LOWER-frequency noise field before the main frequency multiply. This also naturally
// gives cloud systems a swept/curved silhouette instead of the raw round Worley-cell blob
// shape — closer to how real weather systems look, without needing true curl noise.
// The warp field's own sample coordinate is advected by cloudPhase, so it slowly drifts over
// time — this is what gives the 3D noise itself genuine flow, not just the existing 2D
// coverage-map UV slide (cloudPhase*driftMult elsewhere), which only reveals/hides a STATIC 3D
// volume and never displaces the volume's own internal structure.
const float kWarpFreq      = 6;    // low relative to kCloudColFreq=80 — large-scale sweep only
// kWarpStrength UNITS FIXED: this used to be a dirECEF-space offset applied BEFORE the
// kCloudHorizFreq(480)/kCloudColFreq(80) multiply, which silently amplified it by whichever
// frequency it fed into — at the old semantics, strength=0.3 shifted the fine-detail sample by
// up to 0.3*480=144 texels, 75% of the entire 192-texel tile, tearing across the texture's own
// REPEAT period (the reported seams/grid artifacts, worse at higher strength because the
// amplification is linear in it). Now cloudWarpOffset() returns a UVW-TEXEL-space offset
// applied AFTER the frequency multiply instead, so this value means "texels of displacement,"
// identical at every target frequency, with no hidden amplification. Old dirECEF-space tuning
// does not carry over 1:1 — retune from scratch; ~8-16 is a reasonable starting range.
const float kWarpStrength  = 32.0;
const float kWarpDriftRate = 0.08;   // independent multiplier on cloudPhase for the warp field's
                                      // own drift speed, separate from the coverage map's rate
const float kWarpEvolveRate = 0.00002; // pc.waveTime (wall-clock sec) multiplier — the actual
                                      // visible boiling rate; see windOfs comment in
                                      // cloudWarpOffset for why cloudPhase alone was too slow

// City upwelling baseline scale — see comment at its use site (cloudMarch, cityUp) for why the
// old code (cloud.ambientGain alone, default 0.02) was effectively invisible. This constant
// gives it a real baseline; cloud.ambientGain still scales it further from the settings UI.
// NOTE: 50.0 blew out to solid white — this contribution accumulates across every march sample
// with hNorm<0.45 along a column (potentially dozens for a thick cloud), not just once, so the
// per-sample value compounds a lot more than a single-sample estimate suggests. Cut by ~8x;
// nudge from here rather than jumping back toward the old value.
const float kCityUpwellStrength = 0.004;

// City-brightness response curve, shared by the cloud upwelling (below) and the atmospheric
// city-glow term (see kNightGlowScale) so both read the same "how bright is this city" signal
// consistently. earthNightTex luminance varies enormously between a small town and a major
// metro core — a LINEAR response (the old cityMask = max(0,cityLum-kNightFloor)) means bright
// cities dominate completely while small towns barely clear the floor and contribute nothing.
// Reinhard-style compression (raw/(raw+k)) has a steep slope near 0 (small towns get a real,
// visible response) and naturally saturates toward 1.0 for large raw values (major metros can't
// run away and blow out) — compresses the huge input dynamic range into a much narrower, more
// even output range. Smaller k = more aggressive compression (steeper low-end boost, earlier
// high-end saturation).
const float kNightFloor    = 0.002;
const float kCityCompressK = 0.08;
float cityBrightness(float lum) {
    float raw = max(0.0, lum - kNightFloor);
    return raw / (raw + kCityCompressK);
}
// Atmospheric city-glow strength (Step 7 / C10 in TERRAIN_PLAN.md — previously deferred,
// implemented here alongside the cloud upwelling fix so both read the same brightness curve
// and sell as one consistent light source instead of bright clouds over a flat-black sky).
// First-pass value, deliberately conservative — kCityUpwellStrength's first guess (50) blew
// out badly, so start low here and raise if the glow reads as too subtle.
const float kNightGlowScale = 0.0000002;

// ── Airglow (C15, TERRAIN_PLAN.md Phase E) ──────────────────────────────────────
// Three altitude-banded emissive nightglow layers, riding the N_VIEW atmosphere loop
// where their peak altitude falls inside it (green/sodium), or a small supplemental
// march where it doesn't (red — peaks at 275km, well past N_VIEW's ~100km ceiling;
// see the airglowRed march after the N_VIEW loop in main()). Density per layer is a
// Gaussian in altitude: exp(-((h-peakAltM)/halfWidthM)^2). Real airglow altitudes are
// near-constant physical constants (not scene-dependent), so they're hardcoded here
// rather than exposed as CloudParams sliders — only per-band brightness (which is a
// legitimate first-pass visual guess, unlike the altitudes) is user-tunable.
const float kAirglowGreenPeakM      = 96000.0;   // O I 557.7nm — dominant visible band
const float kAirglowGreenHalfWidthM = 9000.0;
const vec3  kAirglowGreenColor      = vec3(0.35, 1.0, 0.25);
const float kAirglowSodiumPeakM      = 90000.0;  // Na D 589.3nm — sharp/thin
const float kAirglowSodiumHalfWidthM = 6500.0;
const vec3  kAirglowSodiumColor      = vec3(1.0, 0.65, 0.15);
const float kAirglowRedPeakM      = 275000.0;    // O I 630.0nm — diffuse/broad halo
const float kAirglowRedHalfWidthM = 75000.0;
const vec3  kAirglowRedColor      = vec3(1.0, 0.12, 0.05);
// Horizontal patchiness so the bands don't read as a perfectly flat, featureless ring
// around the sky (a pure function of altitude alone has zero horizontal variation).
// Reuses the analytic warpPerlin3 noise already used for cloud domain warp — no new
// texture/binding, matches the "reuse existing noise infra" C15 design directive
// (which pre-dates the cloud warp's migration from a noiseTex lookup to this analytic
// evaluator — see cloudWarpOffset's comment; follow the current code, not the stale plan).
const float kAirglowNoiseFreq = 4.0;
const float kAirglowDriftRate = 0.015; // wall-clock rad/s (pc.waveTime), slow independent drift
// First-pass brightness scale, same convention as kNightGlowScale/kCityUpwellStrength
// above: raw accumulation (density × segLen, summed over qualifying march samples) is
// a large unnormalized number, this brings it into visible range. Deliberately
// conservative — real airglow is famously faint. Tune via cloud.airglowGain (settings
// slider) rather than editing this constant.
const float kAirglowScale = 0.0000005;

// ── 3D volumetric ↔ flat 2D crossfade band ─────────────────────────────────────
// cloudMarch (expensive per-sample 3D shell march) and evalCloudLayer (cheap flat-texture
// paste at layers[0]/[1]'s shellAltM, i.e. the same physical cloud base/top) render the SAME
// shell at two different fidelities. Below kCloud3DFadeStart: pure 3D. Above kCloud3DFadeEnd:
// pure flat 2D (cheap enough for orbit). Both sides read this same pair of constants so the
// crossfade is symmetric — previously the flat paste used a hard `obsEffH < 8000` boolean while
// the volumetric fade didn't reach zero until 180 km, so 8-180 km altitude showed both the flat
// shell AND the still-near-full-strength 3D volume composited at once (visible as the flat
// texture "shell" intersecting the volumetric clouds).
const float kCloud3DFadeStart = 800000.0;
const float kCloud3DFadeEnd   = 3000000.0;

// ── View-march step count vs. altitude ──────────────────────────────────────────
// Step count needed for a glitchless march scales with the shell's ANGULAR size on screen,
// which shrinks with observer altitude — LEO (400-600 km) can look correct with far fewer
// steps than ground level needs. This band is intentionally separate from kCloud3DFadeStart
// (800 km): that constant now sits above typical LEO, so opacity fading alone doesn't reduce
// cost anywhere satellites actually orbit — the reported "LEO cloud perf is awful" case sits
// entirely inside the always-full-3D zone below kCloud3DFadeStart. This band supplies the
// actual LEO-perf lever: steps ramp from full ground quality down to a floor well before 800 km.
const float kMarchStepsAltStart = 2000.0;    // below this: full cloud.marchSteps (ground/aircraft)
const float kMarchStepsAltEnd   = 200000.0;   // at/above this: kMarchStepsFloor (LEO and up)
const float kMarchStepsFloor    = 12.0;       // minimum steps once the shell is angularly tiny

// ── Rayleigh scattering (wavelength-dependent: R=650nm, G=510nm, B=440nm) ─────
const vec3  BETA_R = vec3(5.8e-6, 13.5e-6, 33.1e-6);  // 1/m, sea level //vec3(5.8e-6, 13.5e-6, 33.1e-6);  // 1/m, sea level
const float H_R    = 7994.0;   // Rayleigh scale height (m)

// ── Mie scattering (aerosols, wavelength-independent) ─────────────────────────
const float BETA_M = 2.1e-5;   // 1/m, sea level
const float H_M    = 12.0;   // Mie scale height (m)
const float G_MIE  = 0.26;     // forward-scatter asymmetry (higher = sharper corona)

// ── Lighting / tone mapping ────────────────────────────────────────────────────
const float SUN_INTENSITY  = 1.0;
const float EXPOSURE_DAY   =  1.8;   // sun at zenith -- prevents white washout
const float EXPOSURE_NIGHT = 10.0;   // below horizon -- amplifies dim twilight glow

// ── Ray march quality ──────────────────────────────────────────────────────────
// Was fixed const (124/12) — perf follow-up (session 24): the main atmosphere loop runs
// unconditionally on every pixel (terrain, ocean, cloud, satellite, or empty space) before any
// surface-specific work, so this is the single most-paid-for cost in the whole shader. Now
// UBO-tunable ("View samples"/"Light samples" sliders) so the user can empirically test how much
// of the ground-level frame budget this actually costs before investing in a transmittance LUT.
// Defaults (124/12) preserve prior behavior exactly.

float phaseR(float cosA) {
    return 0.75 * (1.0 + cosA * cosA);
}
float phaseM(float cosA) {
    float g2  = G_MIE * G_MIE;
    float den = pow(max(1e-4, 1.0 + g2 - 2.0 * G_MIE * cosA), 1.5);
    return 1.5 * ((1.0 - g2) / (2.0 + g2)) * (1.0 + cosA * cosA) / den;
}
// Cloud dual-lobe HG: mild forward scatter (g=0.3) + very weak backscatter (g=-0.1).
// gF=0.3 gives ~5x forward vs perpendicular — enough for a silver lining without a spotlight.
// The old gF=0.8 (div near cosA=1) caused the "cone of light" artifact.
float phaseCloud(float cosA) {
    const float gF = 0.3, gB = -0.1;
    float g2f = gF * gF, g2b = gB * gB;
    float fwd = 1.5 * ((1.0 - g2f) / (2.0 + g2f)) * (1.0 + cosA * cosA)
                / pow(max(1e-4, 1.0 + g2f - 2.0 * gF * cosA), 1.5);
    float bwd = 1.5 * ((1.0 - g2b) / (2.0 + g2b)) * (1.0 + cosA * cosA)
                / pow(max(1e-4, 1.0 + g2b - 2.0 * gB * cosA), 1.5);
    return mix(fwd, bwd, 0.3);
}
// Analytic ray-sphere intersection. Returns (tNear, tFar) along the ray.
// Solves |ro + t*rd|² = r²  →  t² + 2bt + c = 0  where b=dot(ro,rd), c=|ro|²-r².
// When ro is inside the sphere, tNear < 0 and tFar > 0 (one root behind, one ahead).
// Both components are negative (vec2(-1)) on a miss (discriminant < 0).
//
// c is deliberately computed as (|ro|-r)*(|ro|+r), NOT dot(ro,ro)-r*r. At this project's scale
// |ro| and r are both ~R_EARTH (~6.37e6 m), so dot(ro,ro) and r*r are both ~1e13-magnitude
// float32 numbers — subtracting two nearly-equal huge numbers to get a value that should be much
// smaller (near the horizon, the true discriminant is small) destroys precision catastrophically
// right where every shell march (clouds, cirrus, airglow, atmosphere) needs it most: grazing/
// near-tangent rays, i.e. the horizon. (|ro|-r) is instead a direct small-number subtraction —
// |ro| and r are each independently accurate to ~1 part in 2^24, so their difference retains
// that same relative precision instead of inheriting the ~1e6-magnitude absolute error that
// squaring-then-subtracting would produce. This is the standard fix for planetary-scale
// ray-sphere tests in single precision; it does not fully eliminate float32's inherent precision
// floor exactly AT true tangency (b*b itself is still a large number there), but it removes the
// much larger, unconditional cancellation error that was present at every distance, not just
// exact tangency.
vec2 raySphere(vec3 ro, vec3 rd, float r) {
    float b     = dot(ro, rd);           // half the t^1 coefficient of the quadratic
    float roLen = length(ro);
    float c     = (roLen - r) * (roLen + r);  // |ro|²-r², cancellation-safe form
    float d     = b * b - c;             // discriminant; negative = ray misses sphere entirely
    if (d < 0.0) return vec2(-1.0);
    float sq = sqrt(d);
    return vec2(-b - sq, -b + sq);    // tNear = entry distance, tFar = exit distance
}
// Marches N_LIGHT steps from point p toward direction d over distance segTotal
// and returns (Rayleigh optical depth, Mie optical depth) — i.e. ∫ρ(h) ds for each species.
// Multiply by BETA_R / BETA_M in the caller to convert to actual extinction coefficients.
// Called once per view sample to accumulate the sun-side transmittance at that altitude.
vec2 optDepth(vec3 p, vec3 d, float segTotal) {
    int   N_LIGHT = int(max(2.0, cloud.lightSamples));
    float sLen = segTotal / float(N_LIGHT);  // length of each sun-ray sub-step
    float odR = 0.0, odM = 0.0;
    for (int i = 0; i < N_LIGHT; ++i) {
        float h = max(0.0, length(p + d * (float(i) + 0.5) * sLen) - R_EARTH);  // altitude at sub-step midpoint
        odR += exp(-h / H_R);  // Rayleigh density (exponential profile, scale height H_R)
        odM += exp(-h / H_M);  // Mie density (exponential profile, scale height H_M)
    }
    return vec2(odR, odM) * sLen;  // multiply summed densities by step length → optical depth units
}

// Rotates a direction vector around the Z (polar) axis by angle theta — used to advect the 3D
// cloud noise's sampling position in lockstep with the 2D coverage map's own longitude drift
// (cloud.cloudPhase * driftMult). Without this, the coverage silhouette slides while the 3D
// structure underneath stays fixed in place — a static blob's leading edge gets progressively
// uncovered (reads as "growing") and its trailing edge gets covered back up (reads as
// "shrinking"), instead of the whole cloud genuinely translating. Rotating dirECEF by the same
// angle the 2D UV shifts by keeps the two locked together.
vec3 rotateZ(vec3 v, float theta) {
    float c = cos(theta), s = sin(theta);
    return vec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}

// Clamp-and-scale: maps v from [lo,hi] → [newLo,newHi], clamped to the output range.
// Used throughout cloud density to shift where the noise "zero floor" lands —
// changing lo raises or lowers the threshold at which noise starts contributing density.
float remap(float v, float lo, float hi, float newLo, float newHi) {
    return newLo + clamp((v - lo) / (hi - lo), 0.0, 1.0) * (newHi - newLo);
}

// ── Analytic 3D gradient noise for the cloud domain warp ───────────────────────
// The warp used to read cloudNoiseTex (a 192³ DISCRETELY STORED texture) at kWarpFreq=0.1,
// which spans only ~38 texels across the whole visible range. Trilinear filtering between
// stored texel values is piecewise-multilinear, not truly smooth — each grid cell interpolates
// as a flat-ish shard, not a curved surface. That's invisible at the texture's intended dense
// sampling rate (kCloudHorizFreq=480+), but reading it this sparsely exposed the underlying
// voxel grid directly as faceted, straight-edged geometry — the reported "tessellating"
// artifacts, baked into the cloud edge wherever the warp perturbed the presence threshold.
// Fix: evaluate gradient noise ANALYTICALLY at the exact continuous query point instead of
// interpolating a coarse discrete grid — same hash/gradient technique cloud_noise.comp uses to
// bake the volume, just run live here instead of pre-baked to a fixed low resolution. No
// texture, no discretization, no grid to facet against, and (bonus) no REPEAT-wrap seam class
// of bug possible at all, since there's no stored tile to wrap.
uvec3 warpHashU(uvec3 v) {
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 warpGradHash(ivec3 c) {
    return normalize(-1.0 + 2.0 * (vec3(warpHashU(uvec3(c))) * (1.0 / 4294967296.0)));
}
float warpPerlin3(vec3 p) {
    ivec3 i = ivec3(floor(p));
    vec3  f = fract(p);
    vec3  u = f * f * (3.0 - 2.0 * f);   // smoothstep — gives C1-continuous interpolation
    float v000 = dot(warpGradHash(i),                   f              );
    float v100 = dot(warpGradHash(i + ivec3(1,0,0)), f - vec3(1,0,0));
    float v010 = dot(warpGradHash(i + ivec3(0,1,0)), f - vec3(0,1,0));
    float v110 = dot(warpGradHash(i + ivec3(1,1,0)), f - vec3(1,1,0));
    float v001 = dot(warpGradHash(i + ivec3(0,0,1)), f - vec3(0,0,1));
    float v101 = dot(warpGradHash(i + ivec3(1,0,1)), f - vec3(1,0,1));
    float v011 = dot(warpGradHash(i + ivec3(0,1,1)), f - vec3(0,1,1));
    float v111 = dot(warpGradHash(i + ivec3(1,1,1)), f - vec3(1,1,1));
    return mix(mix(mix(v000, v100, u.x), mix(v010, v110, u.x), u.y),
               mix(mix(v001, v101, u.x), mix(v011, v111, u.x), u.y), u.z);
}

// Low-frequency domain-warp offset for cloud noise sampling — see kWarpFreq/Strength/DriftRate
// comment above. Returns a UVW-TEXEL-space offset (see kWarpStrength units note) — callers add
// it AFTER multiplying their dirECEF by kCloudHorizFreq/kCloudColFreq, not before, so the same
// absolute texel displacement applies at every target frequency with no hidden amplification.
// Three warpPerlin3 evaluations (offset to decorrelate) build a pseudo-random vector field —
// pure ALU cost, no texture bandwidth, and no grid to facet against at any zoom level.
//
// windOfs evolution: driven by pc.waveTime (wall-clock seconds, constant rate regardless of
// sim time-warp — same push constant already used for ocean wave animation), not cloud.cloudPhase.
// cloudPhase advances at cloud.driftRate*simTime (~3e-6 rad/sim-second, calibrated for a
// realistic "~1 deg/day" weather drift) — a full cycle takes ~24 days of SIMULATED time, so at
// normal time scales the warp was effectively frozen over any real observation window. A small
// residual cloudPhase term is kept so the warp's long-run drift direction still tracks the
// coherent weather-system motion; pc.waveTime supplies the actual visible boiling motion.
vec3 cloudWarpOffset(vec3 dirECEF) {
    vec3 windOfs = vec3(cloud.cloudPhase * kWarpDriftRate + pc.waveTime * kWarpEvolveRate,
                         pc.waveTime * kWarpEvolveRate * 0.6,
                         cloud.cloudPhase * kWarpDriftRate * 0.7 + pc.waveTime * kWarpEvolveRate * 0.8);
    vec3 p       = dirECEF * kWarpFreq + windOfs;
    float wx = warpPerlin3(p);
    float wy = warpPerlin3(p + vec3(11.3, 47.7,  5.9));
    float wz = warpPerlin3(p + vec3(71.9,  3.1, 29.4));
    return vec3(wx, wy, wz) * kWarpStrength;
}

// cirrusWindAngleAt/cirrusDomainWarp moved to shaders/cloud_march.comp (C15-perf, half-res cloud
// pass) — they were exclusive to cirrusMarch, which moved there too.

// ── Lens flare (adapted from "Lens Flare Example" by peterekepeter, public domain)
// ─────────────────────────────────────────────────────────────────────────────
// Produces the visible corona/bloom around the source AND the reflected ghost
// artifacts that appear along the flare axis (source -> screen centre -> beyond).
// Diffraction spikes are intentionally omitted; instead the irregular corona
// shape (human-eye / dirty-lens airy-disk pattern) is produced entirely by the
// noise texture lookup on f0.
//
// Coordinate space: ShaderToy-style UV.
//   x in [-0.5*aspect, +0.5*aspect],  y in [-0.5, +0.5].
//
// Parameters:
//   uv     -- current fragment position in flare UV space
//   pos    -- source (satellite / sun) position in flare UV space
//   intens -- normalised brightness [0,1]; controls f0 scale and ghost strength
//
// Returns an HDR additive RGB contribution.
// The call site multiplies by a tint and an overall scale factor.
// ─────────────────────────────────────────────────────────────────────────────
// bokehMult: independent brightness scalar for the ghost/bokeh elements (f2–f6).
// Use a small value (e.g. 0.3) for satellites, larger (e.g. 2.0) for the sun.
// Separates corona brightness (intens) from artifact brightness (bokehMult).
vec3 lensFlare(vec2 uv, vec2 pos, float intens, float bokehMult) {

    // uvd: radially distorted UV -- uv * |uv|.
    // Near screen centre uvd ~= 0; toward edges it bends outward.
    // Ghost artifacts use uvd so their positions follow the curved optical path
    // of real multi-element lens reflections.
    vec2 uvd = uv * length(uv);

    // d: displacement from current fragment to source.
    vec2 d = uv - pos;

    // dist: radius^0.1 -- nearly 1.0 everywhere, dips to 0 right at the source.
    // Used as a small radial term in f0's shimmer modulation.
    float dist = pow(length(d), 0.1);

    // ang: polar angle [-pi, +pi] around the source.
    // Used to sample the noise texture angularly so the corona has irregular lobes.
    float ang = atan(d.y, d.x);

    // ── Angular corona noise via texture lookup ────────────────────────────────
    // Replicates the original ShaderToy formula:
    //   noise(sin(ang*4 + pos.x)*4 - cos(ang*3 + pos.y))
    //
    // The argument is a smoothly-varying scalar that changes both with the angle
    // around the source (ang) and with the source's screen position (pos.x, pos.y).
    // This means each satellite at a different screen position has a unique corona
    // shape -- the lobes don't align between adjacent satellites.
    //
    // Mapping the scalar to a UV coordinate for noiseTex:
    //   We use a 1D slice along the texture's x-axis (v = 0.5, middle row).
    //   The u coordinate wraps via the REPEAT sampler so any float value is valid.
    //   The noise value (red channel) is then passed into sin(...*16)*0.1 which
    //   creates fine angular variation (+/-10%) around the corona rim.
    // noiseSeed is a smoothly-varying float that changes with angle and source position.
    // We map it into [0,1] UV space by dividing by the expected range (~8) and adding
    // 0.5 to centre it, then rely on REPEAT wrapping for values outside [0,1].
    // Using fract() explicitly makes the wrapping behaviour unambiguous.
    // The v coordinate is fixed at 0.25 (upper quarter of texture, away from the
    // edge to avoid any border artifacts on some hardware).
    float noiseSeed = sin(ang * 4.0 + pos.x) * 4.0 - cos(ang * 3.0 + pos.y);
    float noiseU    = fract(noiseSeed * 0.125 + 0.5); // map [-8,+8] -> [0,1], wrapping
    float angNoise  = texture(noiseTex, vec2(noiseU, 0.25)).r;

    // ── Source glow: Lorentzian corona centered on the source ─────────────────
    // The Lorentzian  1/(r * scale + 1)  is wider and softer than a Gaussian,
    // matching real lens-coating scatter on a bright point source.
    //
    float scale = 1200.0; // corona radius: higher = tighter. 60 = wide (visible at 200px), 1200 = tight (visible at ~15px)
    //   r = 0.005 (~5px at 1080p):  f0 = 1/(0.005*60+1) = 0.77
    //   r = 0.02  (~22px):          f0 = 1/(0.02 *60+1) = 0.45
    //   r = 0.05  (~54px):          f0 = 1/(0.05 *60+1) = 0.25
    //   r = 0.10  (~108px):         f0 = 1/(0.10 *60+1) = 0.14
    //   r = 0.20  (~216px):         f0 = 1/(0.20 *60+1) = 0.077
    // This gives a wide, visible corona that extends well past the satellite dot
    // and fades naturally without a hard edge.  The old scale of 200 fell to
    // <0.05 at only 50px, making the corona invisible at our additive blend scale.
    //
    // The modulation line applies the noise-driven angular shimmer:
    //   sin(angNoise * 16) * 0.1  -- fine ripple from texture (+/- 10% per lobe)
    //   dist * 0.1                -- barely-there radial taper (~constant ~1)
    //   + 0.8                     -- base boost so the corona is always bright
    // sin(noise*16) oscillates rapidly around the corona, creating 8-16 irregular
    // bright lobes -- the airy-disk / human-eye diffraction pattern.
    float f0 = 1.0 / (length(d) * scale + 1.0);
    f0 = f0 + f0 * (sin(angNoise * 16.0) * 20.8 + dist);
    // Scale by intensity so dimmer satellites have a proportionally smaller corona.
    f0 *= 0.1;// + intens * 0.5);

    // ── Large near-source bloom: soft blob mirrored through screen centre ──────
    // Placed at -1.2*pos (reflected slightly beyond centre).
    // Represents light that bounced backward through the lens and re-emerged near
    // the entrance pupil.  Multiplier 4.0 (reduced from original 7.0) and
    // contribution capped below to prevent peripheral over-saturation.
    float f1 = max(0.01 - pow(length(uv + 1.2 * pos), 1.9), 0.0) * 4.0;
    f1 *= 0.6;

    // ── Ghost artifacts: fade when source is near screen centre ───────────────
    // When pos ~= (0,0) (looking directly at the source), uvd + k*pos ~= uvd,
    // which is nearly zero everywhere near centre.  The Lorentzian denominator
    // (1 + 32*r^2) then approaches 1 everywhere, lighting up the entire screen.
    //
    // ghostFade = smoothstep(0.03, 0.12, |pos|):
    //   source within ~3% screen height of centre: ghosts = 0
    //   source more than 12% screen height off-centre: ghosts full
    // This also makes physical sense: looking directly at the source means ghost
    // reflection paths don't form visible off-axis elements.
    float ghostFade = smoothstep(0.03, 0.12, length(pos));

    // ── Bokeh halos: large circular rings reflected through screen centre ──────
    // Classic rainbow-ringed bokeh circles opposite the source.
    // Lorentzian  1/(1 + 32*r^2)  matches wide, soft real ghost disc profiles.
    // Three slightly offset RGB positions produce chromatic aberration fringing.

    float f2  = max(1.0/(1.0 + 32.0*pow(length(uvd + 0.80*pos), 2.0)), 0.0) * 0.25 * bokehMult;
    float f22 = max(1.0/(1.0 + 32.0*pow(length(uvd + 0.85*pos), 2.0)), 0.0) * 0.23 * bokehMult;
    float f23 = max(1.0/(1.0 + 32.0*pow(length(uvd + 0.90*pos), 2.0)), 0.0) * 0.21 * bokehMult;

    // ── Star-shaped secondary bokeh (between source and centre) ───────────────
    // uvx = 1.5*uv - 0.5*uvd.  The 2.4 exponent gives a slightly star-shaped
    // profile (intermediate between circle and square).
    // RGB variants at 0.40/0.45/0.50*pos create a second tier of chromatic split.
    vec2 uvx = mix(uv, uvd, -0.5);
    float f4  = max(0.01 - pow(length(uvx + 0.40*pos), 2.4), 0.0) * 6.0;
    float f42 = max(0.01 - pow(length(uvx + 0.45*pos), 2.4), 0.0) * 5.0;
    float f43 = max(0.01 - pow(length(uvx + 0.50*pos), 2.4), 0.0) * 3.0;

    // ── Compact sparkle dots along the flare axis ─────────────────────────────
    // High exponent (5.5) = sharp dropoff = tight bright pinpoints at 0.2/0.4/0.6*pos.
    uvx = mix(uv, uvd, -0.4);
    float f5  = max(0.01 - pow(length(uvx + 0.20*pos), 5.5), 0.0) * 2.0;
    float f52 = max(0.01 - pow(length(uvx + 0.40*pos), 5.5), 0.0) * 2.0;
    float f53 = max(0.01 - pow(length(uvx + 0.60*pos), 5.5), 0.0) * 2.0;

    // ── Broad streaks on the camera-side of centre ────────────────────────────
    // Negative multiplier places these between centre and the source.
    // Low exponent (1.6) = broad, diffuse -- reads as a smear on the front element.
    uvx = mix(uv, uvd, -0.5);
    float f6  = max(0.01 - pow(length(uvx - 0.300*pos), 1.6), 0.0) * 6.0;
    float f62 = max(0.01 - pow(length(uvx - 0.325*pos), 1.6), 0.0) * 3.0;
    float f63 = max(0.01 - pow(length(uvx - 0.350*pos), 1.6), 0.0) * 5.0;

    // ── Assemble ──────────────────────────────────────────────────────────────
    vec3 c = vec3(0.0);

    // Source corona -- achromatic (warm white set by call-site tint).
    c += vec3(f0);
    c += vec3(f1 * 0.5);  // bloom at -1.2*pos

    // Ghost terms: chromatic, gated by ghostFade to prevent centre blowout.
    // bokehMult independently scales all ghost/reflection artifacts from the corona (f0).
    c.r += (f2  + f4  + f5  + f6)  * 0.4 * ghostFade * bokehMult;
    c.g += (f22 + f42 + f52 + f62) * 0.4 * ghostFade * bokehMult;
    c.b += (f23 + f43 + f53 + f63) * 0.4 * ghostFade * bokehMult;

    // Slight vignette: outer screen positions have more lens distortion.
    c = c * 1.3 - vec3(length(uvd) * 0.05);

    return max(c, vec3(0.0));
}

// ── Ocean wave functions (adapted from "Seascape" by Alexander Alekseev aka TDM, 2014)
// License: CC-BY-NC-SA 3.0 — tdmaav@gmail.com
// posM = ENU East/North metres + geographic phase offset (observer-relative, ~Earth-fixed);
// pHeight = metres above R_EARTH; seaTime = 1.0 + pc.waveTime * kSeaSpeed.

const mat2  kOctaveM       = mat2(1.6, 1.2, -1.2, 1.6);
const float kSeaFreq       = 0.056;
const float kSeaHeight     = 2;
const float kSeaChoppy     = 3.0;   // 4.0 → 2.0: rounder crests, less plateau cliffs
const float kSeaSpeed      = 1.5;
const vec3  kSeaBase = vec3(0.01, 0.04, 0.08);   // dark, desaturated blue
const vec3  kSeaWaterColor = vec3(0.2, 0.50, 0.85) * 0.1;

// Hash without Sine (Dave Hoskins, MIT): stable for all float input magnitudes.
// The original fract(sin(dot(p, large_vec))*large_num) loses GPU sin() precision
// once the dot product exceeds ~10^4 (happens at 4th-5th octave where kOctaveM
// doubles UV scale each iteration), producing the angular banding artifact.
float seaHash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973) * 0.1); //vec3(0.1031, 0.1030, 0.0973)
    q += dot(q, q.yzx + 33.33);
    return fract((q.x + q.y) * q.z);
}
float seaNoise(vec2 p) {
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    return -1.0 + 2.0 * mix(
        mix(seaHash(i + vec2(0.0, 0.0)), seaHash(i + vec2(1.0, 0.0)), u.x),
        mix(seaHash(i + vec2(0.0, 1.0)), seaHash(i + vec2(1.0, 1.0)), u.x),
        u.y);
}

float seaOctave(vec2 uv, float choppy) {
    uv += seaNoise(uv);
    vec2 wv  = 1.0 - abs(sin(mod(uv, vec2(2.0 * PI, 2.0 * PI))));
    vec2 swv = abs(cos(mod(uv, vec2(2.0 * PI, 2.0 * PI))));
    wv = mix(wv, swv, wv);
    return pow(1.0 - pow(wv.x * wv.y, 0.65), choppy);
}

// Geometry pass (3 octaves default): used in height-map trace. Octave count is UBO-tunable
// (cloud.oceanSeaOctaves, perf session 24) — this is called up to 10x per ocean pixel by
// heightMapTracing's secant refinement, so it's a direct multiplicative cost lever.
float seaMap(vec2 posM, float pHeight, float seaTime) {
    float freq = kSeaFreq, amp = kSeaHeight, choppy = kSeaChoppy;
    vec2  uv   = posM; uv.x *= 0.75;
    float h    = 0.0;
    int   nOct = int(max(1.0, cloud.oceanSeaOctaves));
    for (int i = 0; i < nOct; i++) {
        float d  = seaOctave((uv + seaTime) * freq, choppy);
              d += seaOctave((uv - seaTime) * freq, choppy);
        h  += d * amp;
        uv *= kOctaveM; freq *= 1.9; amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return pHeight - h;
}

// Fragment pass (5 octaves default): used for high-quality normal computation. Octave count is
// UBO-tunable (cloud.oceanDetailOctaves, perf session 24).
float seaMapDetail(vec2 posM, float pHeight, float seaTime) {
    float freq = kSeaFreq, amp = kSeaHeight, choppy = kSeaChoppy;
    vec2  uv   = posM; uv.x *= 0.75;
    float h    = 0.0;
    int   nOct = int(max(1.0, cloud.oceanDetailOctaves));
    for (int i = 0; i < nOct; i++) {
        float d  = seaOctave((uv + seaTime) * freq, choppy);
              d += seaOctave((uv - seaTime) * freq, choppy);
        h  += d * amp;
        uv *= kOctaveM; freq *= 1.9; amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return pHeight - h;
}

// ── Thin-shell cloud layer evaluator ─────────────────────────────────────────
// Intersects a sphere shell at R_EARTH + shellAltM, samples earthCloudsTex at the
// hit point's geographic lat/lon (Earth-fixed UV + per-layer longitude drift), and
// blends the result into `color`.
//
// Lighting uses dot(normalize(cloudPointECEF), sunDirECEF) — the sun angle at the
// cloud's own geographic location, NOT the observer's sun elevation.  This ensures
// clouds on the dark side of Earth are dark regardless of where the observer is.
void evalCloudLayer(
    vec3  obsPos,  vec3 dir,  float tSurface,
    vec3  enuX,    vec3 enuY, vec3  enuZ,
    vec3  sunDirECEF,
    float odRcam,  float odMcam,
    float coverage, float density, float sunGain,
    float shellAltM, float driftMult, float alphaMax, float mipLod,
    float cloudPhase,
    inout vec3 color)
{
    vec2  tc = raySphere(obsPos, dir, R_EARTH + shellAltM);
    float t  = (tc.x > 0.001) ? tc.x : tc.y;
    if (t <= 0.001) return;
    if (tSurface > 0.0 && t >= tSurface) return;

    // Hit point in ENU → convert to ECEF for geographic UV and sun-dot
    vec3  hitENU = obsPos + t * dir;
    vec3  cECEF  = hitENU.x * enuX + hitENU.y * enuY + hitENU.z * enuZ;
    float cL     = length(cECEF);
    float cLon   = atan(cECEF.y, cECEF.x);
    float cLat   = asin(clamp(cECEF.z / cL, -1.0, 1.0));

    // Earth-fixed UV with per-layer longitude drift
    vec2  uv    = vec2(fract((cLon + PI) / (2.0*PI) + cloudPhase * driftMult / (2.0*PI)),
                       (0.5*PI - cLat) / PI);
    float raw   = textureLod(earthCloudsTex, uv, mipLod).r;
    float alpha = clamp((raw - (1.0 - coverage)) * density, 0.0, alphaMax);
    if (alpha <= 0.0) return;

    // Sun angle at the cloud's geographic position — independent of observer location
    float cloudSunDot  = dot(normalize(cECEF), sunDirECEF);
    float cloudDayFrac = smoothstep(-0.1, 0.15, cloudSunDot);
    vec3  cloudColor   = vec3(max(0.0, cloudSunDot + 0.1) * sunGain) * cloudDayFrac;

    vec3 attn = exp(-(BETA_R * odRcam + BETA_M * 1.1 * odMcam));
    color = mix(color, cloudColor * attn, alpha);
}

// ── Volumetric cloud density (Nubis remap + erosion) ─────────────────────────
// Returns a density value [0,1] at the given 3D noise coordinate.
//   uvwXY:         dirECEF * kCloudHorizFreq, UNFRACTED — horizontal noise position; two fixed
//                  Z-offsets are added internally to sample a "base" and "top" archetype slice.
//   uvwDetail:     full 3D coordinate (Z=altitude) for the Worley erosion channels
//   hNorm:         normalized height within the cloud shell [0,1] — blends base→top presence
//   coverage:      per-sample 2D coverage map value × global coverage slider — controls threshold
//   density:       global linear scale from CloudParams UBO (user-tunable at runtime)
//   heightProfile: caller-supplied soft fade [0,1] — 0 at shell base/top, 1 in mid-layer
float cloudDensity(vec3 uvwXY, vec3 uvwDetail, float hNorm, float coverage, float density, float heightProfile) {
    // ── Stage 1: Base shape — blend two fixed-Z archetype slices by hNorm ─────
    // Sampling two DIFFERENT fixed Z-slices of the Perlin-Worley R channel gives the cloud
    // footprint genuine width variation with height instead of the "perfect cylinder" a single
    // constant-Z slice produced. Threshold EACH slice independently, then blend the resulting
    // (post-threshold) alpha values — not the raw noise values. Blending raw values first would
    // regress the mid-height sample toward the mean of two decorrelated fields, which can only
    // ever shrink coverage monotonically (a smooth cone) — it can never produce a region present
    // in one slice but absent in the other, which is exactly what a real overhang/concavity is.
    // Post-threshold blending preserves each slice's independent shape so those regions can
    // appear/disappear across height non-monotonically. mix() is still smooth in hNorm, so this
    // remains immune to the raymarch-step banding a raw continuous Z-sweep caused (see
    // project_cloud_march_steps memory) — there's no threshold crossing along hNorm to alias.
    const float kPresenceZBase = 0.0;
    const float kPresenceZTop  = 0.5;
    // No manual fract() here: cloudNoiseSampler is already REPEAT-addressed, which wraps the
    // raw coordinate correctly. Pre-wrapping with fract() creates an artificial sawtooth
    // discontinuity in the coordinate as seen by the GPU's automatic screen-space derivative
    // computation (used for LOD/filtering), which manual wrapping does not need to introduce.
    float nsBaseR = texture(cloudNoiseTex, uvwXY + vec3(0.0, 0.0, kPresenceZBase)).r;
    float nsTopR  = texture(cloudNoiseTex, uvwXY + vec3(0.0, 0.0, kPresenceZTop)).r;
    float baseA   = remap(nsBaseR, 1.0 - coverage, 1.0, 0.0, 1.0);
    float topA    = remap(nsTopR,  1.0 - coverage, 1.0, 0.0, 1.0);
    float base    = mix(baseA, topA, clamp(hNorm, 0.0, 1.0));
    if (base <= 0.0) return 0.0;  // clear sky at this point — skip the more expensive erosion fetch

    // ── Stage 2: High-frequency erosion detail (G/B/A channels at full 3D Z) ──
    // Uses uvwDetail (altitude in Z) so the Worley erosion varies with height,
    // giving each cloud tower 3D interior structure (bumpy surface, wispy edges).
    // Worley cells are spherical, not gradient-plane-aligned, so this does NOT
    // re-introduce the horizontal slab problem that afflicted the Perlin R channel.
    // G/B/A = inverted Worley cells at three successive octave scales:
    //   G (weight 0.625): coarse Worley — chews large bites from cloud edges (cumulus cauliflower)
    //   B (weight 0.25):  medium Worley — adds mid-scale texture to edge fraying
    //   A (weight 0.125): fine Worley   — adds wispy tendrils at the very edge
    vec4  nsF     = texture(cloudNoiseTex, uvwDetail * 1.5 + vec3(0.37, 0.53, 0.71));
    float erosion = nsF.g * 0.625 + nsF.b * 0.25 + nsF.a * 0.125;

    // ── Stage 3: Apply erosion by raising the base shape's lower floor ────────
    // remap(base, 0.2*erosion, 1.0, 0.0, 1.0):
    //   New lower cutoff = 0.2 * erosion (ranges 0.0 to ~0.2 depending on Worley cells).
    //   Cloud edges (small base values near the threshold) get their floor pushed up →
    //   they drop below the new floor and disappear = the edge is eroded away.
    //   Cloud interiors (base near 1.0) are far above the floor → unaffected → stay dense.
    // Net visual effect: large rounded blobs develop wispy edges and cauliflower surfaces.
    float eroded  = remap(base, 0.2 * erosion, 1.0, 0.0, 1.0);  // erosion floor fixed; density is a linear scale

    // ── Stage 4: Modulate by height profile and global density ────────────────
    // heightProfile fades density to zero at the base and top of the cloud shell,
    // preventing hard horizontal planes from being visible when looking through a layer.
    // density is a linear user-tunable scale applied last so it doesn't shift the erosion logic.
    return clamp(eroded * heightProfile * density, 0.0, 1.0);
}

// ── Cloud raymarch diagnostics ────────────────────────────────────────────────
// Set CLOUD_DEBUG to 1-5 to replace cloud output with a diagnostic overlay.
// Set to 0 for normal rendering.
//
//  1 = 2D coverage at column entry — white=overcast, black=clear.
//      Question: Is the coverage map causing a solid overcast everywhere?
//
//  2 = hNorm of FIRST cloud hit — red=hit near base, green=hit near top, dark-blue=no hit.
//      Question: Are all clouds at the same altitude (should show varying colour if 3D)?
//
//  3 = Fraction of march steps where d>0 — white=solid cloud in this column, black=clear.
//      Question: Is the march mostly in cloud (overcast) or mostly clear (scattered)?
//
//  4 = noiseUVW at march midpoint — R=X, G=Y, B=Z noise coords.
//      Question: Is the noise actually varying in 3D, or is one axis stuck constant?
//
//  5 = posZ value at first cloud hit — greyscale [0,1].
//      Question: Is posZ (the Z anti-banding offset) actually varying across the image?
//      UNIFORM GREY = posZ is constant for all visible pixels = geographic UV barely changes
//      within the visible cloud footprint from ground level. This confirms the Z-layer
//      banding is caused by posZ not spanning enough geographic range from the surface.
#define CLOUD_DEBUG 0

// cloudShadowFactor() removed (C15-perf follow-up, session 23) — it was the dominant
// remaining surface-level cloud cost (full-res, ~every terrain/ocean pixel, up to 64
// steps each) and cloud shadowing on terrain/ocean isn't currently used. directSun below
// no longer multiplies by it. Its CloudParams UBO slot reverted to `pad0`, and the
// CLOUD_ISOLATE_COLH/SHADOW debug switches that existed only to isolate this function's
// seam bugs went with it.

// cloudMarch()/cirrusMarch() moved to shaders/cloud_march.comp (half-res compute pass —
// C15-perf). main() below now samples cloudTargetA/cloudTargetB (bindings 10/11) instead
// of calling these directly. See TERRAIN_PLAN.md session 23 log for the design.

void main() {
    vec3 dir    = normalize(enuDir);
    vec3 sunDir = normalize(sunDirENU.xyz);

    // ENU→ECEF rotation built from observer ECEF direction (needed early for terrain UV).
    vec3 enuZ = normalize(pc.obsECEFDir.xyz); // observer Up in ECEF
    vec3 enuX = normalize(cross(vec3(0.0, 0.0, 1.0), enuZ)); // East
    vec3 enuY = cross(enuZ, enuX);            // North

#if CLOUD_DEBUG == 6
    // Minimal, direct test: sample cloudNoiseTex straight from the view direction, bypassing
    // the entire raymarch/threshold/lighting pipeline entirely. If the seam still appears here,
    // it's unambiguously baked into the volume itself (or in this one-line coordinate build);
    // if it's clean, something between here and the raymarch is still the culprit.
    {
        vec3 dirECEFView = normalize(dir.x * enuX + dir.y * enuY + dir.z * enuZ);
        outColor = vec4(texture(cloudNoiseTex, dirECEFView * kCloudHorizFreq).rgb, 1.0);
        return;
    }
#endif

    // Sun direction in ECEF — used by evalCloudLayer for per-cloud-point illumination.
    // Transforms ENU sunDir into ECEF so cloud day/night is geographically correct,
    // not relative to the observer's view of the sun.
    vec3 sunDirECEF = sunDir.x * enuX + sunDir.y * enuY + sunDir.z * enuZ;

    // ── Elevation encoding constants ──────────────────────────────────────────────
    // Land-only normalized DEM: pixel=0 → 0 m (sea level), pixel=1 → 8848 m (Everest).
    // Ocean texels are stored as 0, but JPEG compression introduces DCT artifacts
    // (typically 1–4 out of 255 levels = 34–140 m) that cause false terrain hits.
    // All elevation reads are gated by earthSpecTex (ocean mask): if specMask > 0.5
    // the texel is ocean and terrainH is forced to 0 regardless of the elevation pixel.
    const float kElevRange  = 9000.0;
    const float kMaxTerrain = 9000.0;  // terrain shell height (m) — just above Everest
    const float kElevOffset = 15.0 / 255.0 * kElevRange;  // DEM ocean baseline (~529 m)

    // GPU-side observer ground height: single texture fetch at the observer's lat/lon.
    // This matches the terrain march formula exactly, so the observer never sinks into
    // terrain regardless of what the CPU computed.  pc.obsECEFDir.w is the CPU's total
    // height above sea level (obsTerrainH + obsHeightOffset); we take the max of the
    // GPU ground height and the CPU value so user-controlled altitude offsets still work.
    float obsGroundH;
    {
        vec3  od  = normalize(pc.obsECEFDir.xyz);
        float lat = asin(clamp(od.z, -1.0, 1.0));
        float lon = atan(od.y, od.x);
        vec2  uv  = vec2((lon + PI) / (2.0*PI), (0.5*PI - lat) / PI);
        float obsSpec = textureLod(earthSpecTex, uv, 0.0).r;
        obsGroundH = (obsSpec > 0.5) ? 0.0 : max(0.0, textureLod(earthElevTex, uv, 0.0).r * kElevRange - kElevOffset);
    }
    float obsEffH = max(obsGroundH, max(0.0, pc.obsECEFDir.w));

    // Observer position: +2 m eye height above ground.
    vec3 obsPos = vec3(0.0, 0.0, R_EARTH + obsEffH + 2.0);

    // For elevated observers the visible region extends below the geometric horizon.
    // limbZ = sin(Earth-limb depression angle) — negative, approaches 0 at sea level.
    float obsR  = length(obsPos);
    float limbZ = (obsR > R_EARTH) ? -sqrt(max(0.0, 1.0 - (R_EARTH / obsR) * (R_EARTH / obsR))) : 0.0;
    float hClip = smoothstep(limbZ - 0.02, limbZ + 0.03, dir.z);

    // ── Phase 1: terrain march (runs before atmosphere so we can truncate tEnd) ─

    vec2 tBase  = raySphere(obsPos, dir, R_EARTH);
    vec2 tShell = raySphere(obsPos, dir, R_EARTH + kMaxTerrain);

    // March for rays that could plausibly intersect terrain (up to ~44° above horizon).
    // Beyond that angle no terrain on Earth is geometrically reachable from any altitude.
    float tHit      = -1.0;
    float tSeaLvl   = (tBase.x > 0.0) ? tBase.x : -1.0;
    vec2  hitUV     = vec2(0.0);
    vec3  terrainNorm = vec3(0.0, 0.0, 1.0); // overwritten on terrain hit

    if (dir.z < 0.7 && tShell.y > 0.0) {
        float tExit = (tBase.x > 0.0) ? tBase.x
                    : (tShell.y > 0.0  ? tShell.y : 0.0);
        // Cap scales with observer altitude so terrain is visible from LEO.
        // At ground the old 250 km limit is preserved (horizon is close anyway).
        // At LEO (400 km) it extends to 900 km, covering ~65° off-nadir views.
        // Limb-grazing rays that would otherwise generate very long tShell paths
        // are still clamped so the march stays bounded.
        float tCap = mix(250000.0, 3600000.0, clamp(obsEffH / 400000.0, 0.0, 1.0));
        tExit = min(tExit, tCap);

        // Quadratic step distribution: steps grow proportionally to their index so
        // near terrain gets fine resolution while far terrain gets coarser steps.
        // Step count scales with altitude: 196 at ground (terrain is close),
        // up to 320 at LEO so the last steps near Earth are ~2.8 km — fine enough
        // to detect Himalayan-scale terrain from 400 km nadir.
        // Was mix(320.0, 320.0, ...) — a literal no-op that always paid the LEO-tuned 320-step
        // budget even at ground level, directly contradicting the "196 at ground" comment above.
        // Restored to match documented intent (session 24 perf follow-up).
        int kN = int(mix(196.0, 320.0, clamp(obsEffH / 800000.0, 0.0, 1.0)));
        float jitter  = textureLod(noiseTex, gl_FragCoord.xy * (1.0/128.0), 0.0).r;
        float tPrev   = 2.0;

        for (int i = 0; i < kN; ++i) {
            if (tHit >= 0.0) break;
            float frac = (float(i) + jitter) / float(kN);
            float t    = 2.0 + (tExit - 2.0) * frac * frac;
            if (t > tExit) break;
            vec3  p = obsPos + t * dir;
            float rayH = length(p) - R_EARTH;
            if (rayH <= 0.0) break;

            vec3  pE  = p.x * enuX + p.y * enuY + p.z * enuZ;
            float pL  = length(pE);
            float lat = asin(clamp(pE.z / pL, -1.0, 1.0));
            float lon = atan(pE.y, pE.x);
            vec2  uv  = vec2((lon + PI) / (2.0 * PI), (0.5 * PI - lat) / PI);
            float specPx   = textureLod(earthSpecTex, uv, 0.0).r;
            float terrainH = (specPx > 0.5) ? 0.0 : max(0.0, textureLod(earthElevTex, uv, 0.0).r * kElevRange - kElevOffset);

            if (rayH < terrainH) {
                float tLo = tPrev, tHi = t;
                for (int j = 0; j < 12; ++j) {
                    float tM  = (tLo + tHi) * 0.5;
                    vec3  pm  = obsPos + tM * dir;
                    float mH  = length(pm) - R_EARTH;
                    vec3  pmE = pm.x * enuX + pm.y * enuY + pm.z * enuZ;
                    float mL  = length(pmE);
                    float mLat = asin(clamp(pmE.z / mL, -1.0, 1.0));
                    float mLon = atan(pmE.y, pmE.x);
                    vec2  mUV  = vec2((mLon + PI) / (2.0*PI), (0.5*PI - mLat) / PI);
                    float mSpec = textureLod(earthSpecTex, mUV, 0.0).r;
                    float mT    = (mSpec > 0.5) ? 0.0 : max(0.0, textureLod(earthElevTex, mUV, 0.0).r * kElevRange - kElevOffset);
                    if (mH < mT) tHi = tM; else tLo = tM;
                }
                tHit = (tLo + tHi) * 0.5;
                vec3  ph  = obsPos + tHit * dir;
                vec3  phE = ph.x * enuX + ph.y * enuY + ph.z * enuZ;
                float phL = length(phE);
                hitUV = vec2((atan(phE.y, phE.x) + PI) / (2.0*PI),
                             (0.5*PI - asin(clamp(phE.z / phL, -1.0, 1.0))) / PI);

                // Terrain normal from elevation gradient (central differences).
                // Builds hit-point local East/North/Up in ECEF, then maps to observer ENU.
                {
                    const float kTexU = 1.0 / 21600.0;
                    const float kTexV = 1.0 / 10800.0;
                    float hE2 = max(0.0, textureLod(earthElevTex, hitUV + vec2(kTexU, 0.0), 0.0).r * kElevRange - kElevOffset);
                    float hW2 = max(0.0, textureLod(earthElevTex, hitUV - vec2(kTexU, 0.0), 0.0).r * kElevRange - kElevOffset);
                    float hN2 = max(0.0, textureLod(earthElevTex, hitUV - vec2(0.0, kTexV), 0.0).r * kElevRange - kElevOffset);
                    float hS2 = max(0.0, textureLod(earthElevTex, hitUV + vec2(0.0, kTexV), 0.0).r * kElevRange - kElevOffset);
                    float hitLat2 = PI * 0.5 - hitUV.y * PI;
                    float texLon  = max(100.0, 2.0 * PI * R_EARTH * abs(cos(hitLat2)) / 21600.0);
                    float texLat  = PI * R_EARTH / 10800.0; // ~1853 m/texel
                    float dE2     = (hE2 - hW2) / (2.0 * texLon);
                    float dN2     = (hN2 - hS2) / (2.0 * texLat);
                    vec3 hUpE     = phE / phL;
                    vec3 hEsE     = normalize(vec3(-hUpE.y, hUpE.x, 0.0)); // East in ECEF
                    vec3 hNrE     = cross(hUpE, hEsE);                      // North in ECEF
                    vec3 nECEF    = normalize(-dE2 * hEsE + -dN2 * hNrE + hUpE);
                    terrainNorm   = normalize(vec3(dot(nECEF, enuX), dot(nECEF, enuY), dot(nECEF, enuZ)));
                }
            }
            tPrev = t;
        }
    }

    // Effective surface distance: terrain if found, else sea level
    float tSurface = (tHit > 0.0) ? tHit : tSeaLvl;

    // ── Phase 2: atmosphere integration, truncated at the surface ─────────────
    vec2  tAtmos = raySphere(obsPos, dir, R_ATMOS);
    // Clamped to 0: when the observer is above R_ATMOS (reachable via the uncapped "Raise
    // Elevation" control) and looking outward/away from Earth, raySphere's forward root
    // (tAtmos.y) goes negative — the 100km shell is now entirely behind the camera. Without
    // this clamp, segLen would go negative and the whole loop below would march backward from
    // the observer instead of contributing nothing, corrupting the sky colour along that ray.
    float tEnd   = max(0.0, (tSurface > 0.0) ? min(tAtmos.y, tSurface) : tAtmos.y);

    int   N_VIEW = int(max(8.0, cloud.viewSamples));
    float segLen = tEnd / float(N_VIEW);
    float cosA   = dot(dir, sunDir);
    float pR     = phaseR(cosA);
    float pM     = phaseM(cosA);

    vec3  accumR  = vec3(0.0);
    float accumM  = 0.0;
    float accumCity = 0.0;
    vec3  accumAirglow = vec3(0.0); // green + sodium bands (C15) — ride these same samples
    float odR_cam = 0.0;
    float odM_cam = 0.0;

    // ── Single-scattering atmosphere integration (Rayleigh + Mie) ────────────
    // N_VIEW uniform steps from the observer toward the atmosphere exit (or truncated at
    // the surface).  At each step the scattered sunlight is accumulated using:
    //   - Two running totals (odR_cam, odM_cam): optical depth from the CAMERA to this step.
    //     These are also read back after the loop to attenuate the surface/moon/cloud colours.
    //   - Per-step sun optical depth (sunOD): optical depth from THIS STEP to the SUN,
    //     computed by calling optDepth along the sun direction.
    //   - Phase functions pR/pM: angular weighting of how much scatter points toward the camera.
    for (int i = 0; i < N_VIEW; ++i) {
        vec3  sp  = obsPos + dir * ((float(i) + 0.5) * segLen);  // midpoint of this atmosphere step
        float len = length(sp);
        if (len < R_EARTH) sp *= R_EARTH / len;  // clamp underground samples to Earth surface
        float h = max(0.0, length(sp) - R_EARTH);  // altitude above sea level (metres)

        // Running camera-side optical depth: accumulated from step 0 to this step.
        // densR/densM = density × step length = optical depth contribution of this step alone.
        float densR = exp(-h / H_R) * segLen;   // Rayleigh: peaks at sea level, scale height H_R
        float densM = exp(-h / H_M) * segLen;   // Mie: concentrated near surface, scale height H_M
        odR_cam += densR;
        odM_cam += densM;

        // City light-pollution upwelling (Step 7 / C10, TERRAIN_PLAN.md). An INDEPENDENT light
        // source, not derived from sunlight, so it's computed here — BEFORE the sun-shadow test
        // below, which specifically triggers when this sample is in Earth's shadow (i.e. at
        // night, exactly when city glow matters). Uses camera-side attenuation only (no
        // sun-side optical depth term — irrelevant for a non-solar light source). densR weights
        // near-surface atmosphere heavily, so an observer directly over a city gets strong
        // zenith glow while a distant observer only picks up dim glow from low horizon samples
        // — both fall out naturally from the same accumulation used for Rayleigh/Mie above.
        {
            vec3  spECEF    = sp.x * enuX + sp.y * enuY + sp.z * enuZ;
            float spLen     = length(spECEF);
            vec3  spDirECEF = spECEF / spLen;
            float spLat     = asin(clamp(spDirECEF.z, -1.0, 1.0));
            float spLon     = atan(spDirECEF.y, spDirECEF.x);
            vec2  spUV      = vec2((spLon + PI) / (2.0*PI), (0.5*PI - spLat) / PI);
            float spLum     = dot(textureLod(earthNightTex, spUV, 4.0).rgb, vec3(0.2126, 0.7152, 0.0722));
            vec3  attnCam   = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
            accumCity += cityBrightness(spLum) * densR * dot(attnCam, vec3(1.0 / 3.0));

            // Airglow (C15): green (96km) + sodium (90km) bands both fall inside this loop's
            // own altitude range (h spans 0..~100km along an open-sky ray), so they ride these
            // existing samples for free — no dedicated march needed (unlike red, see below the
            // loop). Gated by the SAMPLE's own geographic day/night (not the observer's), same
            // dot-product test cloud lighting uses (evalCloudLayer/cloudMarch sampleDayness) —
            // physically correct since the glow originates at that geographic point, not at the
            // observer. Horizontal patchiness from a slow analytic domain warp avoids a flat,
            // featureless ring (a pure function of altitude alone has none).
            float airDayness = clamp((dot(spDirECEF, sunDirECEF) + 0.15) / 0.3, 0.0, 1.0);
            float airNight   = 1.0 - airDayness;
            if (airNight > 0.001) {
                float airPatch = 0.6 + 0.4 * warpPerlin3(spDirECEF * kAirglowNoiseFreq
                                    + vec3(pc.waveTime * kAirglowDriftRate, 17.0, -5.0));
                float dzG = (h - kAirglowGreenPeakM) / kAirglowGreenHalfWidthM;
                float dzS = (h - kAirglowSodiumPeakM) / kAirglowSodiumHalfWidthM;
                float densAirG = exp(-dzG * dzG) * segLen;
                float densAirS = exp(-dzS * dzS) * segLen;
                accumAirglow += (kAirglowGreenColor  * cloud.airglowGreenGain  * densAirG
                                + kAirglowSodiumColor * cloud.airglowSodiumGain * densAirS)
                                * airNight * airPatch;
            }
        }

        // Shadow test: skip samples in Earth's shadow.
        // If the sun-ray from this point has TWO positive intersections with R_EARTH, the sun
        // is behind Earth from here → no direct sunlight → no in-scatter contribution.
        vec2 tSunEarth = raySphere(sp, sunDir, R_EARTH);
        if (tSunEarth.x > 0.0 && tSunEarth.y > 0.0) continue;

        // Compute sun-side optical depth from this sample to the atmosphere boundary.
        vec2 tSun  = raySphere(sp, sunDir, R_ATMOS);
        vec2 sunOD = (tSun.y > 0.0) ? optDepth(sp, sunDir, tSun.y) : vec2(0.0);

        // Combined transmittance τ = BETA × (cam_depth + sun_depth):
        //   cam_depth: how much atmosphere light must traverse from here to the camera.
        //   sun_depth: how much atmosphere sunlight must traverse from the sun to here.
        // Mie multiplied by 1.1 to account for aerosol absorption (σ_ext > σ_scat).
        vec3 tau  = BETA_R       * (odR_cam + sunOD.x)
                  + BETA_M * 1.1 * (odM_cam + sunOD.y);
        vec3 attn = exp(-tau);  // total transmittance: sun → this sample → camera

        // Accumulate in-scattered radiance for each particle type.
        // Multiplying by density (densR/densM) weights by how many particles are at this altitude.
        accumR += attn * densR;                              // Rayleigh: wavelength-dependent (blue sky)
        accumM += dot(attn, vec3(1.0 / 3.0)) * densM;       // Mie: wavelength-neutral (white haze/corona)
    }

    vec3 color = SUN_INTENSITY * (pR * BETA_R * accumR + vec3(pM * BETA_M * accumM));

    // City light-pollution glow dome, composited once here (see accumCity comment in the loop
    // above). nightFactor fades it out through the day — cheap local gate rather than reusing
    // any later-computed day/night variable, since none exists yet at this point in main().
    float nightFactor = 1.0 - smoothstep(-0.05, 0.1, sunDirENU.w);
    color += accumCity * vec3(1.0, 0.72, 0.42) * nightFactor * kNightGlowScale;

    // ── Airglow (C15) ─────────────────────────────────────────────────────────
    // Green + sodium bands (accumAirglow) rode the N_VIEW loop above for free.
    color += accumAirglow * kAirglowScale * cloud.airglowGain;

    // Red band (630nm) supplemental march: peaks at 275km, well past N_VIEW's ~100km
    // ceiling (R_ATMOS), so it can't ride those samples the way green/sodium do — a
    // dedicated small march covers the [100km, 500km] altitude band instead.
    //
    // Entry/exit uses the same below/inside/above shell-relative classification cloudMarch and
    // cirrusMarch already use, keyed on obsEffH (the observer's actual altitude) rather than
    // blindly assuming the observer is always below the band. The uncapped "Raise Elevation"
    // control (climb rate scales with current height, no ceiling) makes it easy to fly up INTO
    // this 100-500km band — and once obsEffH exceeds ~100km and the view ray points outward/away
    // from Earth, raySphere's forward root on the inner (100km) sphere goes negative (that sphere
    // is now behind the camera). The original version of this march always started at tAtmos.y
    // regardless, so in that configuration it began marching from a negative t — behind the
    // camera — sweeping back through the observer's own position. That was the "red glow above
    // the observer" bug: looking toward zenith while elevated into the band is exactly the
    // outward-ray case that triggers it.
    {
        const float kAirglowInnerM = 100000.0; // = R_ATMOS - R_EARTH, shared with the N_VIEW loop
        const float kAirglowFarM   = 500000.0;
        vec2 tAirFar = raySphere(obsPos, dir, R_EARTH + kAirglowFarM);

        float rtEnter, rtExit;
        if (obsEffH < kAirglowInnerM) {
            // Typical ground-based case: enter where the primary N_VIEW loop already stopped.
            rtEnter = tAtmos.y;
            rtExit  = tAirFar.y;
        } else if (obsEffH <= kAirglowFarM) {
            // Observer is inside the band itself — start marching immediately instead of at
            // tAtmos.y, which can be behind the camera here.
            rtEnter = 0.0;
            rtExit  = tAirFar.y;
            if (tAtmos.x > 0.0 && tAtmos.x < rtExit) rtExit = tAtmos.x; // or down through the inner boundary
        } else {
            // Above the band entirely: enter through the outer sphere, exit at the inner one.
            rtEnter = tAirFar.x;
            rtExit  = (tAtmos.x > 0.0) ? tAtmos.x : tAirFar.y;
        }
        if (tSurface > 0.0) rtExit = min(rtExit, tSurface);

        if (rtEnter < rtExit && rtExit > 0.0) {
            const int N_AIRGLOW_RED = 16;
            float rSegLen = (rtExit - rtEnter) / float(N_AIRGLOW_RED);
            vec3  accumAirglowRed = vec3(0.0);
            for (int i = 0; i < N_AIRGLOW_RED; ++i) {
                vec3  rp        = obsPos + dir * (rtEnter + (float(i) + 0.5) * rSegLen);
                float rh        = length(rp) - R_EARTH;
                vec3  rDirECEF  = normalize(rp.x * enuX + rp.y * enuY + rp.z * enuZ);
                float rDayness  = clamp((dot(rDirECEF, sunDirECEF) + 0.15) / 0.3, 0.0, 1.0);
                float rNight    = 1.0 - rDayness;
                if (rNight > 0.001) {
                    float rPatch = 0.6 + 0.4 * warpPerlin3(rDirECEF * kAirglowNoiseFreq
                                        + vec3(pc.waveTime * kAirglowDriftRate, 17.0, -5.0));
                    float dzR    = (rh - kAirglowRedPeakM) / kAirglowRedHalfWidthM;
                    float densAirR = exp(-dzR * dzR) * rSegLen;
                    accumAirglowRed += kAirglowRedColor * cloud.airglowRedGain * densAirR * rNight * rPatch;
                }
            }
            color += accumAirglowRed * kAirglowScale * cloud.airglowGain;
        }
    }

    // ── Moon disc ─────────────────────────────────────────────────────────────
    // kMoonTexRotDeg: rotates the texture CW in the UV plane to align the image's
    // north pole with the physical lunar north pole as seen from the observer.
    // Tune this until the terminator's shadow boundary matches the image poles.
    const float kMoonTexRotDeg = 180.0;
    const float kMoonAngR      = 0.004578 * 3.0;
    const float kMoonBright    = 0.54;
    if (moonDirENU.z > limbZ - kMoonAngR * 2.0) {
        vec3  moonDir3 = normalize(moonDirENU.xyz);

        // ── Atmospheric refraction squish ─────────────────────────────────────
        // Near the horizon, differential refraction lifts the bottom limb more
        // than the top, compressing the apparent disc height.  The Bennett formula
        // gives refraction R(el) in arcminutes; the squish fraction is the
        // difference in R across the disc diameter, divided by the disc diameter.
        float squish = 0.0;
        float elDeg  = degrees(asin(clamp(moonDirENU.z, -1.0, 1.0)));
        if (elDeg < 15.0) {
            float r   = degrees(kMoonAngR);             // disc angular radius, degrees
            float elo = max(elDeg - r, 0.2);            // lower limb elevation (clamped off ground)
            float ehi = elDeg + r;                      // upper limb elevation
            float Rlo = 1.02 / tan(radians(elo + 10.3 / (elo + 5.11))); // arcmin
            float Rhi = 1.02 / tan(radians(ehi + 10.3 / (ehi + 5.11)));
            squish = 0; //clamp((Rlo - Rhi) / (2.0 * r * 60.0), 0.0, 0.5);
        }
        // Stretching dir.z before intersection maps screen pixels into a
        // vertically compressed disc-space — the silhouette becomes a physical
        // ellipse (shorter in elevation) matching the naked-eye refraction effect.
        vec3  dirR  = normalize(vec3(dir.xy, dir.z * (1.0 + squish)));

        vec3  oc    = -moonDir3;
        float bm    = dot(oc, dirR);
        float cm    = 1.0 - kMoonAngR * kMoonAngR;
        float discm = bm * bm - cm;
        float tm    = -bm - sqrt(max(discm, 0.0));
        if (discm >= 0.0 && tm > 0.0) {
            vec3  hp = tm * dirR;
            vec3  n  = normalize(hp - moonDir3);
            float diffuse  = max(0.0, dot(n, sunDir)) * moonDirENU.w;
            float mu       = max(0.0, dot(n, -moonDir3));
            float limbDark = 0.35 + 0.65 * sqrt(mu);
            // Earthshine inversely follows moon phase: new moon (full Earth) = maximum.
            float earthshine = 0.0008 * mu * (1.0 - moonDirENU.w);

            // Build the moon's local face frame: moonZ points toward the observer
            // (tidally locked near side), moonX/moonY span the visible face plane.
            // refUp = celestial north pole in ENU: converts ECEF (0,0,1) to observer ENU
            // by dotting with the ENU basis vectors (enuX/Y/Z are in ECEF-space).
            // This correctly rotates the texture with parallactic angle as the observer
            // moves across Earth, instead of always aligning north with local zenith.
            vec3 moonZ = -moonDir3;
            vec3 northCelENU = vec3(enuX.z, enuY.z, enuZ.z);
            vec3 refUp = (abs(dot(northCelENU, moonZ)) < 0.99) ? northCelENU : vec3(1.0, 0.0, 0.0);
            vec3 moonX = normalize(cross(refUp, moonZ));
            vec3 moonY = cross(moonZ, moonX);

            // Orthographic projection of the surface normal onto the face plane.
            // At the disc centre n == moonZ → UV (0.5, 0.5); at the limb UV spans [0,1].
            vec2 moonUV = vec2(dot(n, moonX), dot(n, moonY)) * 0.5 + 0.5;

            // Rotate UV around disc centre by kMoonTexRotDeg to align image north pole
            // with the physical lunar north pole. Positive = CCW rotation of the texture.
            float rotRad = radians(kMoonTexRotDeg);
            float cosR = cos(rotRad), sinR = sin(rotRad);
            vec2  uvc  = moonUV - 0.5;
            moonUV = vec2(cosR * uvc.x - sinR * uvc.y,
                          sinR * uvc.x + cosR * uvc.y) + 0.5;

            vec3 texColor = texture(moonTex, moonUV).rgb;

            float discFade = (tSurface > 0.0) ? 0.0 : 1.0;
            vec3 moonColor = texColor * (diffuse + earthshine) * limbDark * kMoonBright;
            vec3 moonAttn  = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
            color += discFade * moonColor * moonAttn;
        }
    }

    // ── Satellite constellation sky glow (pre-tonemap) ────────────────────────
    // Wide Gaussian (kSig = 0.90 rad ~= 51 deg) over 64 sky bins.
    // Each occupied bin represents the brightest satellite in that 45°×11.25° cell.
    // Runs pre-tonemap so the exposure system scales it: invisible at noon,
    // visible at dusk, prominent at night.
    {
        const float TWO_PI = 6.28318530718;
        vec3  flareAttn = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));
        const float kSig = 0.90;
        for (int gi = 0; gi < 64; ++gi) {
            uint fluxBits = glowBuf.bins[gi];
            if (fluxBits == 0u) continue;
            float flux   = uintBitsToFloat(fluxBits);
            // Derive bin-centre ENU direction from bin index.
            // azBin=0 is North, increasing toward East (matches atan(x,y) convention).
            float az     = (float(gi / 8) + 0.5) * (TWO_PI / 8.0);
            float elSin  = (float(gi % 8) + 0.5) / 8.0; // z = sin(elevation)
            float elCos  = sqrt(max(0.0, 1.0 - elSin * elSin));
            vec3  fd     = vec3(sin(az) * elCos, cos(az) * elCos, elSin);
            if (fd.z < limbZ - 0.05) continue;
            float angle  = acos(clamp(dot(dir, fd), -1.0, 1.0));
            float glow   = exp(-angle * angle / (2.0 * kSig * kSig)) * 0.01;
            float gElev  = smoothstep(-0.08, 0.02, fd.z);
            float intens = clamp(log2(max(flux, 1.0)) / 4.0, 0.0, 1.5);
            float atmosW = 1.0 - exp(-odR_cam / 5000.0);
            color += hClip * gElev * glow * intens * 0.06 * vec3(1.0, 0.96, 0.88) * flareAttn * atmosW;
        }
    }

    // ── Phase 3: ground / terrain composite ──────────────────────────────────
    // The atmosphere was truncated at tSurface, so odR_cam/odM_cam represent
    // optical depth from the observer to the surface. Transmittance = e^(-tau).
    // We ADD attenuated surface colour to the atmosphere scatter already in `color`.
    if (tSurface > 0.0) {
        vec3 surfAttn = exp(-(BETA_R * odR_cam + BETA_M * 1.1 * odM_cam));

        vec2 uvSurf;
        vec3 hitPt;
        if (tHit > 0.0) {
            uvSurf = hitUV;
            hitPt  = obsPos + tHit * dir;
        } else {
            hitPt        = obsPos + tSeaLvl * dir;
            vec3  hE     = hitPt.x * enuX + hitPt.y * enuY + hitPt.z * enuZ;
            float geoLat = asin(clamp(hE.z / R_EARTH, -1.0, 1.0));
            float geoLon = atan(hE.y, hE.x);
            uvSurf = vec2((geoLon + PI) / (2.0*PI), (0.5*PI - geoLat) / PI);
        }

        vec3  shadingN   = (tHit > 0.0) ? terrainNorm : normalize(hitPt);
        float sunDot     = dot(shadingN, sunDir);
        // Geographic horizon gate: dot(normalize(hitPt), sunDir) is the sun's elevation above
        // the local horizon at the TERRAIN POINT's geographic location.  The slope normal
        // (shadingN) cannot be used for this — a steep slope can face toward the sun even
        // when the terrain point is on the night side of Earth.  Gate dayFrac by the radial
        // direction so no illumination leaks past the terminator regardless of slope angle.
        // Margin [-0.03, 0.02]: thin alpenglow zone for mountain peaks that see the sun just
        // past the flat horizon; anything below -1.7° geographic is forced to zero.
        float geoSunDot   = dot(normalize(hitPt), sunDir);
        float horizonGate = smoothstep(-0.03, 0.02, geoSunDot);
        float dayFrac     = smoothstep(-0.1, 0.3, sunDot) * horizonGate;
        // directSun combines day/night blend for all sun-driven contributions. Used to also
        // multiply in cloud-shadow transmittance (cloudShadowFactor(), removed session 23 —
        // cloud shadowing on terrain/ocean isn't currently used, and it was the dominant
        // remaining full-res surface-level cloud cost).
        float directSun   = dayFrac;
        // Antimeridian seam fix: longitude wraps at ±PI so dFdx(uvSurf.x) jumps by ~1.0
        // across that boundary. The GPU would pick the highest mip level, blurring a
        // vertical strip. Clamp the derivative to the small expected value instead.
        vec2 uvd_dx = dFdx(uvSurf);
        vec2 uvd_dy = dFdy(uvSurf);
        if (uvd_dx.x >  0.5) uvd_dx.x -= 1.0;
        if (uvd_dx.x < -0.5) uvd_dx.x += 1.0;
        if (uvd_dy.x >  0.5) uvd_dy.x -= 1.0;
        if (uvd_dy.x < -0.5) uvd_dy.x += 1.0;
        vec3 dayColor   = textureGrad(earthDayTex,   uvSurf, uvd_dx, uvd_dy).rgb;
        vec3 nightColor = textureGrad(earthNightTex, uvSurf, uvd_dx, uvd_dy).rgb;

        // ── Spectral sun color at terrain hit ─────────────────────────────────
        // Sun light arriving at the terrain is orange at low angles (long atmospheric path).
        // Normalized so the brightest channel = 1.0 (preserves hue; noon ≈ warm white).
        vec3 sunSpecTint = vec3(1.0);
        {
            vec2 tSET = raySphere(hitPt, sunDir, R_EARTH);
            if (!(tSET.x > 0.0 && tSET.y > 0.0)) {  // terrain not in Earth's shadow
                vec2 tSAT = raySphere(hitPt, sunDir, R_ATMOS);
                if (tSAT.y > 0.0) {
                    vec2 sODT    = optDepth(hitPt, sunDir, tSAT.y);
                    vec3 attnT   = exp(-(BETA_R * sODT.x + BETA_M * 1.1 * sODT.y));
                    float maxAttn = max(max(attnT.r, attnT.g), max(attnT.b, 0.001));
                    sunSpecTint  = attnT / maxAttn;  // hue-normalized: max channel → 1.0
                }
            }
        }

        // ── Sky ambient at terrain hit ─────────────────────────────────────────
        // 4-step zenith integration gives the scattered sky color illuminating terrain
        // faces: blue during day, warm-orange during twilight. Mirrors the cloud skyAmbientBase.
        vec3 skyAmbientTerrain = vec3(0.0);
        {
            vec3  zenT = normalize(hitPt);  // terrain zenith (radially outward)
            vec2  tSAT = raySphere(hitPt, zenT, R_ATMOS);
            if (tSAT.y > 0.0) {
                const int N_ZT = 4;
                float zSegT    = tSAT.y / float(N_ZT);
                float cosAT    = dot(zenT, sunDir);
                float pR_upT   = 0.75 * (1.0 + cosAT * cosAT);
                float pM_upT   = phaseM(cosAT);
                float odR_zt = 0.0, odM_zt = 0.0;
                float skyAmbTM = 0.0;
                for (int zi = 0; zi < N_ZT; ++zi) {
                    vec3  sp = hitPt + zenT * ((float(zi) + 0.5) * zSegT);
                    float h  = max(0.0, length(sp) - R_EARTH);
                    float dR = exp(-h / H_R) * zSegT;
                    float dM = exp(-h / H_M) * zSegT;
                    odR_zt += dR;  odM_zt += dM;
                    vec2 tSE  = raySphere(sp, sunDir, R_EARTH);
                    if (tSE.x > 0.0 && tSE.y > 0.0) continue;
                    vec2 tSun = raySphere(sp, sunDir, R_ATMOS);
                    vec2 sunOD = (tSun.y > 0.0) ? optDepth(sp, sunDir, tSun.y) : vec2(0.0);
                    vec3 tau      = BETA_R * (odR_zt + sunOD.x) + BETA_M * 1.1 * (odM_zt + sunOD.y);
                    vec3 attnStep = exp(-tau);
                    skyAmbientTerrain += attnStep * dR;
                    skyAmbTM         += dot(attnStep, vec3(1.0 / 3.0)) * dM;
                }
                skyAmbientTerrain = SUN_INTENSITY * (pR_upT * BETA_R * skyAmbientTerrain
                                                   + vec3(pM_upT * BETA_M * skyAmbTM));
            }
        }

        vec3 surfColor  = mix(nightColor * 0.12,
                              dayColor * sunSpecTint * clamp(sunDot * 1.5, 0.05, 1.0)
                            + dayColor * skyAmbientTerrain * 0.4,  // sky ambient fill (blue day, orange dusk)
                              dayFrac);

        // ── Ocean wave material (sea-level hits only, not terrain) ─────────────
        // ShaderToy "Seascape" by TDM adapted to Earth ENU/ECEF space.
        // heightMapTracing: 8-step secant refinement around the sea-sphere hit.
        // getNormal: central differences on seaMapDetail (5 octaves).
        // getSeaColor: kSeaBase refraction + atmosphere reflection + specular.
        float oceanMask = textureGrad(earthSpecTex, uvSurf, uvd_dx, uvd_dy).r;
        if (oceanMask > 0.5 && tHit < 0.0) {
            vec3  surfUp  = normalize(hitPt);
            float dist    = tSeaLvl;
            float seaTime = 1.0 + pc.waveTime * kSeaSpeed;

            

            // Altitude fade: full 3D waves at low altitude, smooth specular from orbit.
            float altFade = 1.0 - smoothstep(3000.0, 8000.0, obsEffH);

            // Wave UV strategy:
            //   posM = hitPt.xy (ENU East/North metres from observer nadir) — always small,
            //   so the 0.5 m normal epsilon is hundreds of float steps above ULP.
            //   An observer-geographic phase offset (modulo first-octave wave period ≈ 39.3 m)
            //   is added so the pattern is approximately Earth-fixed without accumulating
            //   large absolute coordinates. Derived entirely from enuZ (observer ECEF unit vec).
            vec2 obsPhase = vec2(0.0);
            if (altFade > 0.01) {
                const float wvScale = 2.0 * PI / kSeaFreq;
                float oLat  = asin(clamp(enuZ.z, -1.0, 1.0));
                float oLon  = atan(enuZ.y, enuZ.x);
                obsPhase.x  = fract(oLon * R_EARTH * cos(oLat) / wvScale) * wvScale;
                obsPhase.y  = fract(oLat * R_EARTH           / wvScale) * wvScale;
            }
            vec2 posM = hitPt.xy + obsPhase;

            // ── heightMapTracing (low altitude only) ──────────────────────────
            // Bracket: ±2.5 m vertical around the sea-sphere intersection.
            // hm > 0 at the near end (above waves), hx < 0 at the far end (inside).
            if (altFade > 0.01 && dist < 5000.0) {
                float cosEl  = max(0.05, abs(dot(dir, surfUp)));
                float traceR = min(60.0, 2.5 / cosEl);
                float tm     = tSeaLvl - traceR;
                float tx     = tSeaLvl + traceR;

                // Height above sea level computed as obsEffH + 2 + t*dir.z — avoids
                // catastrophic cancellation in length(p)-R_EARTH at sea level (float
                // ULP at 6.37 M m is 0.76 m, which quantises 1.5 m waves into ~2 steps).
                vec3  plo = obsPos + tm * dir;
                float hm  = seaMap(plo.xy + obsPhase, obsEffH + 2.0 + tm * dir.z, seaTime);
                vec3  phi = obsPos + tx * dir;
                float hx  = seaMap(phi.xy + obsPhase, obsEffH + 2.0 + tx * dir.z, seaTime);

                if (hx < 0.0) {
                    for (int i = 0; i < 8; i++) {
                        float tmid = mix(tm, tx, hm / (hm - hx));
                        vec3  pm   = obsPos + tmid * dir;
                        float hmid = seaMap(pm.xy + obsPhase, obsEffH + 2.0 + tmid * dir.z, seaTime);
                        if (hmid < 0.0) { tx = tmid; hx = hmid; }
                        else             { tm = tmid; hm = hmid; }
                        if (abs(hmid) < 0.001) break;
                    }
                    float tWave = mix(tm, tx, hm / (hm - hx));
                    hitPt  = obsPos + tWave * dir;
                    surfUp = normalize(hitPt);
                    posM   = hitPt.xy + obsPhase;
                    dist   = tWave;
                }
            }

            // Same precision fix: obsEffH + 2 + dist*dir.z instead of length(hitPt)-R_EARTH.
            float pHeight = obsEffH + 2.0 + dist * dir.z;
            vec3  viewDir = normalize(-dir);

            // ── getNormal (central differences on seaMapDetail) ───────────────
            // posM is in ENU East/North metres, so +eps in x = East, +eps in y = North.
            // Normal in ENU = normalize(East_slope, North_slope, Up_component).
            vec3 waveN = surfUp;
            if (altFade > 0.01) {
                float eps = max(0.5, dist * 0.0008);
                float n0  = seaMapDetail(posM,                    pHeight, seaTime);
                float nX  = seaMapDetail(posM + vec2(eps, 0.0),  pHeight, seaTime) - n0;
                float nY  = seaMapDetail(posM + vec2(0.0,  eps), pHeight, seaTime) - n0;
                waveN = normalize(vec3(nX, nY, 0.0) + eps * surfUp);
                float distFade = smoothstep(3000.0, 8000.0, dist);
                waveN = normalize(mix(waveN, surfUp, max(distFade, 1.0 - altFade)));
            }

            // ── getSeaColor ────────────────────────────────────────────────────
            // Fresnel: cubic ramp, capped at 0.5 (ShaderToy formula)
            float fresnel = min(pow(clamp(1.0 - dot(waveN, viewDir), 0.0, 1.0), 3.0), 0.5);

            // Sky reflection — 6-sample atmosphere, distance-gated
            vec3 reflDir   = reflect(dir, waveN);
            vec3 reflColor = vec3(0.12, 0.28, 0.50) * dayFrac;
            float reflStr  = fresnel * exp(-dist / 40000.0);
            if (dot(reflDir, surfUp) > 0.0 && reflStr > 0.005) {
                vec2 tAR = raySphere(hitPt, reflDir, R_ATMOS);
                if (tAR.y > 0.0) {
                    int   N_REFL = int(max(1.0, cloud.oceanReflSamples)); // perf session 24, was const 6
                    float rStart = max(0.0, tAR.x);
                    float rSeg   = (tAR.y - rStart) / float(N_REFL);
                    float rcosA  = dot(reflDir, sunDir);
                    float rpR    = phaseR(rcosA);
                    float rpM    = phaseM(rcosA);
                    vec3  rAccR  = vec3(0.0);
                    float rAccM  = 0.0;
                    float rodR   = 0.0, rodM = 0.0;
                    for (int ri = 0; ri < N_REFL; ++ri) {
                        vec3  rp   = hitPt + reflDir * (rStart + (float(ri) + 0.5) * rSeg);
                        float rh   = max(0.0, length(rp) - R_EARTH);
                        float rdR  = exp(-rh / H_R) * rSeg;
                        float rdM  = exp(-rh / H_M) * rSeg;
                        rodR += rdR; rodM += rdM;
                        vec2 tSE  = raySphere(rp, sunDir, R_EARTH);
                        if (tSE.x > 0.0 && tSE.y > 0.0) continue;
                        vec2 tSun = raySphere(rp, sunDir, R_ATMOS);
                        vec2 sOD  = (tSun.y > 0.0) ? optDepth(rp, sunDir, tSun.y) : vec2(0.0);
                        vec3 rtau  = BETA_R * (rodR + sOD.x) + BETA_M * 1.1 * (rodM + sOD.y);
                        vec3 rattn = exp(-rtau);
                        rAccR += rattn * rdR;
                        rAccM += dot(rattn, vec3(1.0 / 3.0)) * rdM;
                    }
                    reflColor = SUN_INTENSITY * (rpR * BETA_R * rAccR + vec3(rpM * BETA_M * rAccM));
                }
            }

            // Refracted subsurface color (SEA_BASE + diffuse * SEA_WATER_COLOR)
            // directSun replaces dayFrac for all sun-driven contributions so clouds shadow the ocean.
            float diff    = pow(max(0.0, dot(waveN, sunDir)) * 0.4 + 0.6, 80.0) * directSun;
            vec3 refracted = kSeaBase * directSun + diff * kSeaWaterColor * 0.12;

            // Fresnel blend (distance-attenuated to prevent orbit-scale glowing ring)
            surfColor = mix(refracted, reflColor, reflStr);

            // Wave-height crest shading: raised crests catch more water-color light
            float atten = max(1.0 - dist * dist * 1e-5, 0.0);
            surfColor += kSeaWaterColor * max(pHeight - kSeaHeight, 0.0) * 0.18 * atten * directSun;

            // Specular: shininess narrows close-up, broadens with distance
            float specPow = clamp(600.0 / max(1.0, sqrt(dist)), 8.0, 600.0);
            float nrm     = (specPow + 8.0) / (PI * 8.0);
            surfColor    += pow(max(0.0, dot(reflect(dir, waveN), sunDir)), specPow) * nrm * directSun;

            // Moon glint on ocean — nighttime only, dims with phase (new moon = brightest Earth).
            if (moonDirENU.z > limbZ && moonDirENU.w > 0.01) {
                vec3  moonDir3o = normalize(moonDirENU.xyz);
                float mSpecPow  = 120.0;
                float mNrm      = (mSpecPow + 8.0) / (PI * 8.0);
                surfColor += pow(max(0.0, dot(reflect(dir, waveN), moonDir3o)), mSpecPow)
                           * mNrm * moonDirENU.w * clamp(moonDirENU.z, 0.0, 1.0)
                           * 0.006 * (1.0 - dayFrac);
            }
            // Mirror satellite flare glints — sector-stable selection via sectorBright.
            {
                for (int fi = 0; fi < 8; ++fi) {
                    if (glowBuf.sectorBright[fi] == 0u) continue;
                    float flux = uintBitsToFloat(glowBuf.sectorBright[fi]);
                    if (flux < 2.0) continue;
                    vec3 fe = normalize(glowBuf.flareEntries[fi].xyz);
                    if (fe.z < limbZ - 0.02) continue;
                    float fSpecPow = 80.0;
                    float fNrm     = (fSpecPow + 8.0) / (PI * 8.0);
                    float fIntens  = clamp(log2(max(flux, 1.0)) / 10.0, 0.0, 1.0);
                    surfColor += pow(max(0.0, dot(reflect(dir, waveN), fe)), fSpecPow)
                               * fNrm * fIntens * 0.008 * vec3(1.2, 1.1, 1.0) * (1.0 - dayFrac) * altFade;
                }
            }
        }

        color += surfColor * surfAttn;
    }

    // ── Cloud layers (C3/C4 unified: thin-shell 2D overlays) ─────────────────
    // Layers 0/1 double as the volumetric shell's base/top (same shellAltM values cloudMarch
    // reads below), so their flat paste here must crossfade against cloudMarch's own fade using
    // the SAME kCloud3DFadeStart/End band — not an independent threshold — or the two renders
    // overlap. Layers 2/3 (e.g. a standalone high cirrus deck) are always flat, at full weight.
    //
    // Iterate HIGH INDEX -> LOW INDEX: layers are conventionally ordered by increasing altitude
    // (layer0 = low/near, layer1 = cirrus/far, and any future layer2/3 should follow the same
    // convention). evalCloudLayer composites each call ON TOP of whatever `color` already holds,
    // so the farthest-from-a-ground-observer shell must be drawn FIRST (as background) and the
    // nearest drawn LAST (on top) for correct back-to-front compositing — ascending-index order
    // had this backwards (cirrus drew over the low deck regardless of which was actually nearer).
    for (int li = 3; li >= 0; --li) {
        if (cloud.layers[li].enabled < 0.5) continue;
        float fadeWeight = 1.0;
        if (li < 2) {
            fadeWeight = smoothstep(kCloud3DFadeStart, kCloud3DFadeEnd, obsEffH);
            if (fadeWeight < 0.001) continue;
        }
        evalCloudLayer(
            obsPos, dir, tSurface, enuX, enuY, enuZ, sunDirECEF,
            odR_cam, odM_cam,
            cloud.coverage * cloud.layers[li].coverageMult,
            cloud.density  * cloud.layers[li].densityMult,
            cloud.sunGain,
            cloud.layers[li].shellAltM,
            cloud.layers[li].driftMult,
            cloud.layers[li].alphaMax * fadeWeight,
            cloud.layers[li].mipLod,
            cloud.cloudPhase,
            color);
    }

    // ── Half-resolution cloud composite (C15-perf) ───────────────────────────────
    // cirrusMarch/cloudMarch ran in shaders/cloud_march.comp at half resolution; sample the
    // precomputed result here instead of marching per full-res pixel. Target A: rgb=B_total
    // (combined additive radiance), a=tCloudOcclude (m, -1=none, only set when the cloud is
    // ≥90% opaque — used below for satellite/star depth occlusion, NOT for terrain suppression).
    // Target B: rgb=A_total (combined multiplicative attenuation), a=tEnterCombined (m, -1=none;
    // ALWAYS valid whenever either layer rendered anything, regardless of opacity — this is what
    // terrain suppression must use; tCloudOcclude's 90%-opacity gate meant most non-solid cloud
    // never got suppressed at all, a real bug, not just the documented mid-shell approximation).
    // textureSize gives the half-res target's own dimensions; ×2 approximates the full-res frame
    // size (exact when swapExtent is even, off by at most one texel otherwise — invisible for
    // this soft, bilinearly-sampled data).
    vec2  cloudHalfRes = vec2(textureSize(cloudTargetA, 0));
    vec2  cloudUV      = gl_FragCoord.xy / (cloudHalfRes * 2.0);
    vec4  cloudA       = texture(cloudTargetA, cloudUV);
    vec4  cloudB       = texture(cloudTargetB, cloudUV);
    // cloudBlock (post-tonemap sun-disc dimming, used below) derived from A_total's luminance
    // rather than a separate stored scalar — A_total already tracks combined opacity closely
    // (→0 when opaque, →1 when clear), and this frees Target B's alpha for tEnterCombined instead.
    float cloudBlock    = dot(cloudB.rgb, vec3(1.0/3.0));
    float tCloudOcclude = cloudA.a;
    float tEnterCombined = cloudB.a;
    // Terrain-occlusion correction: the compute pass has no terrain data, so it always marches
    // the shell's full potential extent. If terrain (this pixel's own accurate tSurface, computed
    // above) sits closer than either layer's entry point, suppress the whole combined
    // contribution — exact for "terrain fully blocks the cloud," not for a mid-shell partial
    // truncation (a ridge poking partway into the shell) or for terrain sitting geometrically
    // between the cirrus and low-cloud layers specifically (both merged into one target, so they
    // can't be independently suppressed) — accepted approximations, see the half-res-cloud-pass
    // plan's "Known limitation" note.
    if (!(tSurface > 0.0 && tEnterCombined >= 0.0 && tSurface < tEnterCombined)) {
        color = color * cloudB.rgb + cloudA.rgb;
    }

    // ── Auto-exposure tone mapping ─────────────────────────────────────────────
    float dayness  = clamp((sunDirENU.w + 0.2) / 1.2, 0.0, 1.0);
    float exposure = mix(EXPOSURE_NIGHT, EXPOSURE_DAY, pow(dayness, 0.4));
    color = vec3(1.0) - exp(-exposure * color);

    // ── Night ambient floor ────────────────────────────────────────────────────
    float nightAmt = 1.0 - clamp(dayness * 5.0, 0.0, 1.0);
    color += vec3(0.0008, 0.001, 0.002) * nightAmt;

    // ── Moonlight ambient ──────────────────────────────────────────────────────
    float moonEl    = clamp(moonDirENU.z, 0.0, 1.0);
    float moonIllum = moonDirENU.w;
    // Atmosphere weight: glow and ambient fade to zero above the atmosphere.
    float atmosWeight = 1.0 - exp(-odR_cam / 5000.0);
    color += vec3(0.0025, 0.003, 0.004) * moonIllum * moonEl * nightAmt * atmosWeight;

    // ── Moon glow: tight corona + wide diffuse halo (atmosphere-only) ─────────
    if (moonDirENU.z > limbZ - 0.05) {
        vec3  moonDir3  = normalize(moonDirENU.xyz);
        float moonAngle = acos(clamp(dot(dir, moonDir3), -1.0, 1.0));
        float moonFade  = smoothstep(limbZ - 0.006, limbZ + 0.002, moonDirENU.z);

        // Tight inner corona — peaks at disc edge, falls off quickly.
        float corona = exp(-moonAngle * moonAngle / (2.0 * 0.012 * 0.012)) * nightAmt;
        color += hClip * moonFade * corona * vec3(0.92, 0.94, 1.00) * moonIllum * 0.04 * atmosWeight;

        // Wide diffuse halo — scattered moonlight glow, atmosphere-only.
        float scale = 100.0;
        float halo  = exp(-moonAngle * moonAngle / (2.0 * 0.018 * 0.018 * scale * scale));
        color += hClip * moonFade * halo * vec3(0.88, 0.90, 1.00) * moonIllum * 0.012 * atmosWeight;
    }

    // ── Sun disc + atmospheric corona ─────────────────────────────────────────
    if (sunDirENU.w > limbZ - 0.1) {
        float angle      = acos(clamp(cosA, -1.0, 1.0));
        const float kSunAngR = 0.00466; // solar angular radius (~0.267°)
        // Geometric fade: smooth transition as sun centre crosses the geometric limb.
        float geomFade   = smoothstep(limbZ - kSunAngR, limbZ + kSunAngR, sunDirENU.w);
        // Disc pixel: hard-clipped by terrain/ocean hit for this fragment direction.
        float discVis    = (1.0 - smoothstep(0.007, 0.010, angle))
                         * (tSurface > 0.0 ? 0.0 : 1.0);
        // Sunset shift: redden and widen corona as sun approaches the limb.
        float sunsetT    = clamp(1.0 - (sunDirENU.w - limbZ) / 0.15, 0.0, 1.0);
        vec3  sunCol     = mix(vec3(1.5, 1.3, 1.0), vec3(1.8, 0.7, 0.2), sunsetT * 0.7);
        float coronaSig  = mix(0.035, 0.08, sunsetT * sunsetT);
        float corona     = exp(-angle * angle / (2.0 * coronaSig * coronaSig));
        // Attenuate sun disc and corona by cloud transmittance on this view ray (cloudBlock
        // sampled from cloudTargetB.a above, replacing the old dot(cloudTFinal, vec3(1/3))).
        color += (discVis * geomFade * sunCol + corona * geomFade * sunCol * 0.12) * cloudBlock;
    }

    // ── Camera lens flares (post-tonemap) ─────────────────────────────────────
    // Applied after all physics-based rendering so they read as pure camera
    // optical artifacts on top of the scene.
    //
    // UV space: x in [-0.5*aspect, +0.5*aspect], y in [-0.5, +0.5].
    //
    // Fragment projection:
    //   fragCamDir = mat3(skyView) * enuDir  (camera-space ray, z ~= -1)
    //   fragUV = vec2(camDir.x, -camDir.y) * invTanHF2
    //   No perspective divide since z ~= -1 throughout the fullscreen tri.
    //
    // Source projection (satellite or sun):
    //   satCam = mat3(skyView) * normalize(enu)
    //   satUV  = vec2(satCam.x, -satCam.y) / (-satCam.z * tanHF * 2)
    //   Perspective divide by -satCam.z is required here.
    {
        float tanHF     = tan(pc.fovYRad * 0.5);
        float invTanHF2 = 1.0 / (tanHF * 2.0);

        vec3 fragCamDir = mat3(pc.skyView) * enuDir;
        vec2 fragUV     = vec2(fragCamDir.x, -fragCamDir.y) * invTanHF2;

        vec3 flareAccum = vec3(0.0);

        // ── Satellite lens flares ───────────────────────────────────────────────
        // One entry per 45°-az sector; sectorBright holds the stable atomicMax
        // brightness so the flare intensity doesn't flicker even when many
        // bright satellites compete within the same sector.
        {
            const float kFlareThr = 1.0;
            for (int gi = 0; gi < 8; ++gi) {
                if (glowBuf.sectorBright[gi] == 0u) continue;
                float bright = uintBitsToFloat(glowBuf.sectorBright[gi]);
                if (bright < kFlareThr) continue;
                vec3 satDir = normalize(glowBuf.flareEntries[gi].xyz);
                if (satDir.z < limbZ - 0.02) continue;

                vec3 satCam = mat3(pc.skyView) * satDir;
                if (satCam.z >= -0.01) continue;
                vec2 satUV = vec2(satCam.x, -satCam.y) / (-satCam.z * tanHF * 2.0);

                float intens     = clamp(log2(max(bright, 1.0)) / log2(16.0), 0.0, 1.0);
                float entryScale = intens * sqrt(intens);
                vec3  tint       = vec3(1.3, 1.15, 1.0);
                flareAccum += lensFlare(fragUV, satUV, intens, 0.3) * tint * entryScale * 0.25;
            }
        }

        // ── Sun lens flare ──────────────────────────────────────────────────────
        // Gate on limbZ (sin of geometric limb depression, already accounts for observer
        // altitude) so the flare correctly persists when the sun is visible past the
        // curved Earth from orbit — not just when sunDirENU.w > 0.
        if (pc.sunDirENU.w > limbZ - 0.05) {
            float above        = pc.sunDirENU.w - limbZ;
            float sunIntensity = 10.0 * clamp(above / 0.5, 0.0, 1.0);
            vec3 sunCam = mat3(pc.skyView) * normalize(pc.sunDirENU.xyz);
            if (sunCam.z < -0.01) {
                vec2 sunUV    = vec2(sunCam.x, -sunCam.y) / (-sunCam.z * tanHF * 2.0);
                float sunFade = clamp(above * 8.0, 0.0, 1.0);
                vec3  sunTint = vec3(1.4, 1.2, 0.9);
                flareAccum += lensFlare(fragUV, sunUV, sunIntensity, 2.0) * sunTint * sunFade * 0.45;
            }
        }

        // Lens flares are a screen-space camera-optics artifact, not light literally travelling
        // to each pixel — source visibility is already handled per-source above (sun: limbZ
        // gate at its `if`; satellites: satDir.z horizon cull + camera-facing check). Do NOT
        // gate flareAccum by this fragment's OWN terrain hit (tHit): that tests whether THIS
        // pixel's unrelated view ray hit land, not whether the source is occluded. The old
        // `tHit > 0.0 ? 0.0 : 1.0` mask zeroed the flare's additive glow on every terrain pixel
        // anywhere on screen — invisible at ground level (terrain only fills the lower frame),
        // but at LEO twilight, where terrain fills most of the screen under a large sun flare,
        // it hard-clipped the raymarched terrain silhouette out of the middle of the glow.
        color += flareAccum;
    }

    outColor = vec4(color, 1.0);

    // Terrain/ocean occlusion depth for subsequent satellite/star passes.
    // Satellites and stars are drawn with gl_Position.z = 0.5 (fixed) and tested with LESS.
    // Close surface hits write [0, 0.5) so they block those overlays; sky writes 1.0 so they pass.
    // The 150 km cap prevents space-view terrain from incorrectly culling near satellites.
    // tSeaLvl covers ocean pixels that have no terrain hit but still block satellites.
    const float kOcclusionCap = 150000.0;
    float tOcclude = (tHit >= 0.0) ? tHit : tSeaLvl;
    // Opaque cloud also occludes satellites/stars behind it (cloud is above terrain so
    // tCloudOcclude is only used when no terrain/ocean is closer).
    if (tOcclude < 0.0 && tCloudOcclude >= 0.0) tOcclude = tCloudOcclude;
    gl_FragDepth = (tOcclude >= 0.0 && tOcclude < kOcclusionCap)
                   ? tOcclude / (kOcclusionCap * 2.0)
                   : 1.0;
}
