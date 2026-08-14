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
// _BASE suffix: these are the physical reference coefficients. Every consumer (sat_sky.frag's
// evalCloudLayer/main, cloud_march.comp's cloudMarchCS/cirrusMarchCS/fogMarchCS) shadows these
// with a local `BETA_R`/`BETA_M` scaled by CloudParams' `atmosRayleighGain`/`atmosMieGain`
// (settings sliders "Rayleigh gain" / "Mie/haze gain", default 1.0 = reproduces these exactly) —
// see cloud_params.glsl. Kept as plain consts here rather than folded into the gain directly so
// the physical baseline stays visible/greppable on its own.
const vec3  BETA_R_BASE = vec3(5.8e-6, 13.5e-6, 33.1e-6);  // 1/m, sea level
const float H_R    = 7994.0;   // Rayleigh scale height (m)

// ── Mie scattering (aerosols, wavelength-independent) ─────────────────────────
const float BETA_M_BASE = 2.1e-5;   // 1/m, sea level
// H_M was 12.0 (meters) from commit 384df33 ("Specular reflections, moon fix, terrain fix, lots
// of fixes") until this session — 100x smaller than the ~1200m real aerosol scale height that
// pairs with BETA_M_BASE/H_R above, and with no comment explaining the drop. That commit also
// dropped G_MIE 0.76->0.26 in the same diff to fix a real, independently-documented "cone of
// light" artifact near the sun (see phaseCloud's comment below for the same failure mode) — G_MIE
// alone is a complete fix for that (it's the phase function's angular sharpness, unrelated to
// H_M's vertical density falloff), so H_M's drop looks like collateral damage from the same edit
// rather than a deliberate choice; the same diff also left a stray duplicated comment on BETA_R's
// line, consistent with a messy/exploratory pass rather than a considered redesign.
//
// Practical effect while it was 12: exp(-h/12) is ~2e-4 by 100m altitude and ~0 by 1000m, so Mie
// contributed essentially nothing to any optical-depth integral in this sim (cloud shells start
// at 2km; the atmosphere march spans 0-100km) — Mie has been silently dead system-wide, not just
// in one shader. Restored to the physically-paired reference value; cloud.atmosMieGain (settings
// "Mie/haze gain") is now a real, load-bearing control instead of scaling an already-zero term.
const float H_M    = 1200.0;   // Mie scale height (m)
const float G_MIE  = 0.26;     // forward-scatter asymmetry (higher = sharper corona)

const float SUN_INTENSITY = 1.0;

// ── Cloud noise domain frequencies ────────────────────────────────────────────
// The 192³ baked noise volume is reused at ONE fixed frequency across the ENTIRE globe.
// Frequencies are ~(old UV-space tile count)/(2*PI), since dirECEF isn't normalized to a 0-1
// globe fraction the way pUV was.
const float kCloudHorizFreq = 480.0;   // ~13 km detail features
const float kCloudColFreq   = 80.0;    // ~20 km cloud-system footprints

// ── Baked cloud noise channel layout (cloud_noise.comp) — READ BEFORE CHANGING ────────────────
// The 192³ volume packs four values per texel:
//   R = presence (Perlin-Worley) at this texel
//   G = erosion FBM — the three Worley octaves PRE-SUMMED at their fixed 0.625/0.25/0.125 weights
//   B = presence, shifted +54 texels in Z
//   A = presence, shifted +108 texels in Z
//
// The packing exists so cloudDensity() gets all three vertical presence slices from ONE fetch
// (.rba) instead of three, and the erosion octave from one more (.g) instead of a fourth read
// plus a weighted sum — T2.1 in CLOUD_PERF_PLAN.md.
//
// The B/A shifts are INTEGER TEXEL COUNTS, and that is load-bearing: it makes reading .b/.a
// bit-identical to re-reading .r at the corresponding Z offset, because a whole-texel shift
// commutes with trilinear interpolation. It holds only while (a) these constants match
// cloud_noise.comp's own kPresenceMidTexels/kPresenceTopTexels, (b) kCloudNoiseRes matches that
// file's bake resolution, and (c) the sampler stays REPEAT on W so the wrap matches the bake's
// seamless Z tiling. Change one, change all of them.
//
// The offsets used to be hand-written 0.28 / 0.56, which are 53.76 / 107.52 texels — not integer,
// so they could not be packed this way. Rounding them to 54 / 108 moved each presence slice by
// under a quarter of a texel.
const float kCloudNoiseRes     = 192.0;
const float kCloudPresenceZMid = 54.0  / kCloudNoiseRes;  // 0.28125 (was 0.28)
const float kCloudPresenceZTop = 108.0 / kCloudNoiseRes;  // 0.5625  (was 0.56)

// Z offset of the per-column shape lookup (colH). 48 texels = 0.25 exactly, unchanged from the
// hand-written value it replaces. Sampling .a at this offset yields the presence field a further
// +108 texels up (0.8125 total) — which is what the per-column BASE height noise now reads,
// letting one fetch serve both. See cloudColumnNoise() in cloud_march.comp.
const float kCloudColNoiseZ    = 48.0  / kCloudNoiseRes;  // 0.25

// ── Baked domain-warp field sampling (cloud_warp_noise.comp) — DO NOT "OPTIMIZE" THIS ─────────
// kWarpBakeRes MUST match cloud_warp_noise.comp's own bake resolution (kSz).
const int kWarpBakeRes = 192;

// Wraps a texel index into [0,N) — the same explicit pattern gradHash uses in the bake shader,
// not GLSL's `%`, whose sign behaviour for negative operands isn't safe to rely on across drivers.
int   wrapTexel(int x, int n) { int r = x % n; return r < 0 ? r + n : r; }
ivec3 wrapTexel3(ivec3 c, int n) { return ivec3(wrapTexel(c.x, n), wrapTexel(c.y, n), wrapTexel(c.z, n)); }

// Manual smoothstep-weighted trilinear blend of the baked warp texels — 8 texelFetch calls, NOT a
// plain filtered texture() read. This is deliberate and has now been established the hard way
// TWICE. Read the whole comment before changing it.
//
// Hardware trilinear interpolates LINEARLY (straight-edged) between texels; the live analytic
// Perlin this bake replaced was smoothstep-interpolated (curved) between gradient corners. At this
// bake's texel density that mismatch is geometrically legible as "tessellating triangle" faceting,
// because the value is used as a raw COORDINATE DISPLACEMENT (scaled by kWarpStrength = 32) into
// cloudNoiseTex — so the error becomes cloud *geometry*, not a small brightness shift. Raising the
// bake to 192³/2 octaves reduced but did not eliminate it; the interpolation METHOD is the cause.
//
// ── Failed attempt, 2026-08-04 (T1.2 in CLOUD_PERF_PLAN.md) ───────────────────────────────────
// It looks like this can be one filtered fetch instead of eight, by folding the smoothstep into
// the COORDINATE rather than the weight: hardware LINEAR blends with weight frac(c*R - 0.5), so
// passing c = (i0 + u + 0.5)/R should make that weight exactly `u`. That algebra is correct with
// exact arithmetic, and it was verified at the interior case and both wrap edges. It still shipped
// the faceting straight back.
//
// What it misses: **GPU texture filtering carries only ~8 bits of subtexel precision**, so the
// weight is re-quantized to 1/256 steps AFTER the smoothstep. The justification given at the time
// was that 1/256 is finer than the RGBA8 bake's own 1/255 value quantization — which is true and
// irrelevant, because the two act completely differently. The stored-value quantization is a
// static per-texel offset that interpolation then SMOOTHS into a continuous field. The weight
// quantization creates steps WITHIN each texel span — which is precisely the across-the-cell
// stair/facet structure this function exists to remove, and it is amplified 32x into a coordinate
// whose noise domain repeats every ~1 unit.
//
// If you want this cheaper, the lever is the bake (resolution/format//octaves) or removing the
// need for a coordinate warp — not the interpolation. Measure by eye, not by algebra.
vec3 cloudWarpNoiseSample(sampler3D warpTex, vec3 uvw) {
    vec3  tc = fract(uvw) * float(kWarpBakeRes) - 0.5; // texel-centered, wrapped into [0,texSize)
    ivec3 i0 = ivec3(floor(tc));
    vec3  f  = tc - vec3(i0);
    vec3  u  = f * f * (3.0 - 2.0 * f); // same smoothstep curve the analytic Perlin interpolant used

    vec3 c000 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(0,0,0), kWarpBakeRes), 0).rgb;
    vec3 c100 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(1,0,0), kWarpBakeRes), 0).rgb;
    vec3 c010 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(0,1,0), kWarpBakeRes), 0).rgb;
    vec3 c110 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(1,1,0), kWarpBakeRes), 0).rgb;
    vec3 c001 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(0,0,1), kWarpBakeRes), 0).rgb;
    vec3 c101 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(1,0,1), kWarpBakeRes), 0).rgb;
    vec3 c011 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(0,1,1), kWarpBakeRes), 0).rgb;
    vec3 c111 = texelFetch(warpTex, wrapTexel3(i0 + ivec3(1,1,1), kWarpBakeRes), 0).rgb;

    return mix(mix(mix(c000, c100, u.x), mix(c010, c110, u.x), u.y),
               mix(mix(c001, c101, u.x), mix(c011, c111, u.x), u.y), u.z);
}

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

// Altitude of this ray's closest approach to Earth's CENTER, restricted to the forward ray
// (t >= 0) — i.e. how deep this specific line of sight dips toward the ground, independent of
// the observer's own altitude. This is the quantity atmospheric-extinction gating should read,
// not observer height: an observer high in orbit still has a long real atmospheric path along any
// ray aimed near the horizon (that ray's closest approach can be right down at the surface, even
// though the observer itself is far above all air) — that's the physical reason astronaut photos
// show a reddened, extinguished band right at Earth's limb. Using observer altitude alone made
// every consumer of this formula (aurora, satellites, stars, planets, the Milky Way) go completely
// unextincted near the horizon as soon as the OBSERVER left the atmosphere, regardless of how much
// atmosphere that particular ray actually still crossed.
//
// b = dot(ro,rd) >= 0 means rd has a component pointing away from Earth, so the closest approach
// on the forward ray is the observer's own position (distance only increases from t=0 onward) —
// this reduces to the observer's own altitude, matching the old (correct-for-that-case) behaviour
// exactly. b < 0 means rd points at least partly toward Earth; the closest approach is the usual
// point-line perpendicular distance, sqrt(|ro|²-b²), factored as (roLen-b)(roLen+b) to avoid the
// same near-tangent float32 cancellation raySphere's own c term avoids (see its comment above).
float rayTangentAltM(vec3 ro, vec3 rd) {
    float b     = dot(ro, rd);
    float roLen = length(ro);
    float dMin  = (b >= 0.0) ? roLen : sqrt(max((roLen - b) * (roLen + b), 0.0));
    return dMin - R_EARTH;
}

// Bounded variant — same closest-approach math, but clamped to a finite target range (meters)
// along the ray instead of the unconstrained infinite forward ray. Needed for any target that
// actually SITS somewhere finite (a satellite, a planet) rather than being effectively background/
// infinite (stars, the Milky Way, aurora, a bare camera view ray): if the ray direction points
// roughly toward Earth — the target appears silhouetted in front of the planet from the observer's
// viewpoint — the unbounded 2-arg version's tangent point can sit far beyond where the target
// actually is, well past it, even though the real observer-to-target segment never gets anywhere
// near that low-altitude point. That shipped as a real bug: satellites appearing in front of Earth,
// as seen from a space-based observer, were getting extincted as if their sightline grazed the
// atmosphere, when the bounded segment to the satellite doesn't reach anywhere near it. Passing
// maxT clamps the closest-approach parameter to the segment [observer, target] before evaluating
// the altitude, so a target well short of the unbounded tangent point correctly reports its OWN
// altitude (itself, not some point beyond it) instead.
float rayTangentAltM(vec3 ro, vec3 rd, float maxT) {
    float t = clamp(-dot(ro, rd), 0.0, maxT);
    vec3  p = ro + rd * t;
    return length(p) - R_EARTH;
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
