#ifndef SATLIGHTSIM_COMMON_GLSL
#define SATLIGHTSIM_COMMON_GLSL

// Shared scalar constants and small pure helpers.
//
// Every definition here was previously hand-duplicated across 2-4 shaders. That convention was
// deliberate (there was no include mechanism until the pipeline-unification pass) and it worked
// for a long time, but it did eventually produce real silent drift — see the notes on optDepth
// in atmosphere-side code and the two dead-but-drifted copies removed from sat_sky.frag.
//
// Rule for adding to this file: only put something here if every caller genuinely wants the SAME
// value or behaviour. A quantity that is deliberately tuned per-consumer (the geographic
// day/night curves, for instance, which differ on purpose between clouds, terrain, airglow and
// aurora) does NOT belong here — sharing those would silently unify a look decision.

const float PI = 3.14159265359;

// ── Earth / atmosphere geometry (meters) ──────────────────────────────────────
const float R_EARTH = 6371000.0;
const float R_ATMOS = 6471000.0;   // 100 km above surface

// ── Rayleigh scattering (wavelength-dependent: R=650nm, G=510nm, B=440nm) ─────
const vec3  BETA_R = vec3(5.8e-6, 13.5e-6, 33.1e-6);  // 1/m, sea level
const float H_R    = 7994.0;   // Rayleigh scale height (m)

// ── Mie scattering (aerosols, wavelength-independent) ─────────────────────────
const float BETA_M = 2.1e-5;   // 1/m, sea level
const float H_M    = 12.0;     // Mie scale height (m)
const float G_MIE  = 0.26;     // forward-scatter asymmetry (higher = sharper corona)

const float SUN_INTENSITY = 1.0;

// ── Cloud noise domain frequencies ────────────────────────────────────────────
// The 192³ baked noise volume is reused at ONE fixed frequency across the ENTIRE globe.
// Frequencies are ~(old UV-space tile count)/(2*PI), since dirECEF isn't normalized to a 0-1
// globe fraction the way pUV was.
const float kCloudHorizFreq = 480.0;   // ~13 km detail features
const float kCloudColFreq   = 80.0;    // ~20 km cloud-system footprints

// "No surface along this ray" sentinel for the shared scene-depth buffer.
//
// Deliberately a large finite value rather than -1.0: every consumer's operation is either
// `tExit = min(tExit, tScene)` or `if (tEnter >= tScene) return;`, both of which are branch-free
// and correct with a large sentinel. With -1.0 each of the ~7 consumption sites would need its
// own "did we actually hit anything" guard, and a single missed guard is a silent
// everything-is-occluded bug. Consumers that genuinely need "is there a surface at all" test
// `tScene < kSceneDepthValid`.
const float kNoSurfaceT      = 1e30;
const float kSceneDepthValid = 1e29;

// ── Ray-sphere intersection ───────────────────────────────────────────────────
// Returns (tNear, tFar) along the ray. Solves |ro + t*rd|² = r² → t² + 2bt + c = 0
// where b = dot(ro,rd), c = |ro|²-r². When ro is inside the sphere, tNear < 0 and tFar > 0.
// Returns vec2(-1.0) when the ray misses entirely.
//
// PRECISION — do not "simplify" c back to dot(ro,ro) - r*r. At Earth scale both terms are
// ~4e13, so their difference catastrophically cancels in float32 exactly at grazing/near-tangent
// rays, i.e. every horizon, across all ~30 call sites in this project. The factored
// (|ro|-r)*(|ro|+r) form keeps the small difference in the first factor where float32 can still
// represent it.
vec2 raySphere(vec3 ro, vec3 rd, float r) {
    float b     = dot(ro, rd);                // half the t^1 coefficient of the quadratic
    float roLen = length(ro);
    float c     = (roLen - r) * (roLen + r);  // |ro|²-r², cancellation-safe form
    float d     = b * b - c;                  // discriminant; negative = ray misses entirely
    if (d < 0.0) return vec2(-1.0);
    float sq = sqrt(d);
    return vec2(-b - sq, -b + sq);            // tNear = entry distance, tFar = exit distance
}

// Rotates a direction vector around the Z (polar) axis by angle theta — used to advect the 3D
// cloud noise's sampling position in lockstep with the 2D coverage map's own longitude drift
// (cloudPhase * driftMult). Without this the coverage silhouette slides while the 3D structure
// underneath stays fixed: a static blob's leading edge gets progressively uncovered (reads as
// "growing") and its trailing edge covered back up (reads as "shrinking"), instead of the whole
// cloud genuinely translating.
vec3 rotateZ(vec3 v, float theta) {
    float c = cos(theta), s = sin(theta);
    return vec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z);
}

// Clamp-and-scale: maps v from [lo,hi] → [newLo,newHi], clamped to the output range.
// Used throughout cloud density to shift where the noise "zero floor" lands — raising lo raises
// the threshold at which noise starts contributing density.
float remap(float v, float lo, float hi, float newLo, float newHi) {
    return newLo + clamp((v - lo) / (hi - lo), 0.0, 1.0) * (newHi - newLo);
}

// ── Scattering phase functions ────────────────────────────────────────────────
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
// The old gF=0.8 (division blows up near cosA=1) caused the "cone of light" artifact.
float phaseCloud(float cosA) {
    const float gF = 0.3, gB = -0.1;
    float g2f = gF * gF, g2b = gB * gB;
    float fwd = 1.5 * ((1.0 - g2f) / (2.0 + g2f)) * (1.0 + cosA * cosA)
                / pow(max(1e-4, 1.0 + g2f - 2.0 * gF * cosA), 1.5);
    float bwd = 1.5 * ((1.0 - g2b) / (2.0 + g2b)) * (1.0 + cosA * cosA)
                / pow(max(1e-4, 1.0 + g2b - 2.0 * gB * cosA), 1.5);
    return mix(fwd, bwd, 0.3);
}

#endif // SATLIGHTSIM_COMMON_GLSL
