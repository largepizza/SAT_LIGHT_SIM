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
    uint  flareCount;        // lens flares: number of valid per-satellite entries
    uint  flarePad[3];
    vec4  flareEntries[8];  // lens flares: xyz=ENU dir, w=effectFlare
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
// earthElevTex:  R8_UNORM land elevation. p → p * 8848 metres. Ocean stored as 0.
// earthSpecTex:  R8_UNORM ocean mask (white=ocean, black=land). Used for wave material.
layout(set = 0, binding = 3) uniform sampler2D earthDayTex;
layout(set = 0, binding = 4) uniform sampler2D earthNightTex;
layout(set = 0, binding = 5) uniform sampler2D earthElevTex;
layout(set = 0, binding = 6) uniform sampler2D earthSpecTex;

layout(location = 0) out vec4 outColor;

const float PI = 3.14159265359;

// ── Atmosphere geometry (meters) ───────────────────────────────────────────────
const float R_EARTH = 6371000.0;
const float R_ATMOS = 6471000.0;   // 100 km above surface

// ── Rayleigh scattering (wavelength-dependent: R=650nm, G=510nm, B=440nm) ─────
const vec3  BETA_R = vec3(5.8e-6, 13.5e-6, 33.1e-6);  // 1/m, sea level
const float H_R    = 7994.0;   // Rayleigh scale height (m)

// ── Mie scattering (aerosols, wavelength-independent) ─────────────────────────
const float BETA_M = 2.1e-5;   // 1/m, sea level
const float H_M    = 1200.0;   // Mie scale height (m)
const float G_MIE  = 0.76;     // forward-scatter asymmetry (higher = sharper corona)

// ── Lighting / tone mapping ────────────────────────────────────────────────────
const float SUN_INTENSITY  = 1.0;
const float EXPOSURE_DAY   =  1.8;   // sun at zenith -- prevents white washout
const float EXPOSURE_NIGHT = 10.0;   // below horizon -- amplifies dim twilight glow

// ── Ray march quality ──────────────────────────────────────────────────────────
const int N_VIEW  = 12;   // view ray samples
const int N_LIGHT = 4;    // sun-direction samples per view sample

float phaseR(float cosA) {
    return 0.75 * (1.0 + cosA * cosA);
}
float phaseM(float cosA) {
    float g2  = G_MIE * G_MIE;
    float den = pow(max(1e-4, 1.0 + g2 - 2.0 * G_MIE * cosA), 1.5);
    return 1.5 * ((1.0 - g2) / (2.0 + g2)) * (1.0 + cosA * cosA) / den;
}
vec2 raySphere(vec3 ro, vec3 rd, float r) {
    float b  = dot(ro, rd);
    float c  = dot(ro, ro) - r * r;
    float d  = b * b - c;
    if (d < 0.0) return vec2(-1.0);
    float sq = sqrt(d);
    return vec2(-b - sq, -b + sq);
}
vec2 optDepth(vec3 p, vec3 d, float segTotal) {
    float sLen = segTotal / float(N_LIGHT);
    float odR = 0.0, odM = 0.0;
    for (int i = 0; i < N_LIGHT; ++i) {
        float h = max(0.0, length(p + d * (float(i) + 0.5) * sLen) - R_EARTH);
        odR += exp(-h / H_R);
        odM += exp(-h / H_M);
    }
    return vec2(odR, odM) * sLen;
}

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
const vec3  kSeaBase = vec3(0.0, 0.03, 0.18);   // dark, desaturated blue
const vec3  kSeaWaterColor = vec3(0.2, 0.50, 0.85) * 0.1;

// Hash without Sine (Dave Hoskins, MIT): stable for all float input magnitudes.
// The original fract(sin(dot(p, large_vec))*large_num) loses GPU sin() precision
// once the dot product exceeds ~10^4 (happens at 4th-5th octave where kOctaveM
// doubles UV scale each iteration), producing the angular banding artifact.
float seaHash(vec2 p) {
    vec3 q = fract(vec3(p.xyx) * vec3(0.1031, 0.1030, 0.0973));
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
    vec2 wv  = 1.0 - abs(sin(uv));
    vec2 swv = abs(cos(uv));
    wv = mix(wv, swv, wv);
    return pow(1.0 - pow(wv.x * wv.y, 0.65), choppy);
}

// Geometry pass (3 octaves): used in height-map trace.
float seaMap(vec2 posM, float pHeight, float seaTime) {
    float freq = kSeaFreq, amp = kSeaHeight, choppy = kSeaChoppy;
    vec2  uv   = posM; uv.x *= 0.75;
    float h    = 0.0;
    for (int i = 0; i < 3; i++) {
        float d  = seaOctave((uv + seaTime) * freq, choppy);
              d += seaOctave((uv - seaTime) * freq, choppy);
        h  += d * amp;
        uv *= kOctaveM; freq *= 1.9; amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return pHeight - h;
}

// Fragment pass (5 octaves): used for high-quality normal computation.
float seaMapDetail(vec2 posM, float pHeight, float seaTime) {
    float freq = kSeaFreq, amp = kSeaHeight, choppy = kSeaChoppy;
    vec2  uv   = posM; uv.x *= 0.75;
    float h    = 0.0;
    for (int i = 0; i < 5; i++) {
        float d  = seaOctave((uv + seaTime) * freq, choppy);
              d += seaOctave((uv - seaTime) * freq, choppy);
        h  += d * amp;
        uv *= kOctaveM; freq *= 1.9; amp *= 0.22;
        choppy = mix(choppy, 1.0, 0.2);
    }
    return pHeight - h;
}

void main() {
    vec3 dir    = normalize(enuDir);
    vec3 sunDir = normalize(sunDirENU.xyz);

    // ENU→ECEF rotation built from observer ECEF direction (needed early for terrain UV).
    vec3 enuZ = normalize(pc.obsECEFDir.xyz); // observer Up in ECEF
    vec3 enuX = normalize(cross(vec3(0.0, 0.0, 1.0), enuZ)); // East
    vec3 enuY = cross(enuZ, enuX);            // North

    // ── Elevation encoding constants ──────────────────────────────────────────────
    // Land-only normalized DEM: pixel=0 → 0 m (sea level), pixel=1 → 8848 m (Everest).
    // Ocean texels are stored as 0, but JPEG compression introduces DCT artifacts
    // (typically 1–4 out of 255 levels = 34–140 m) that cause false terrain hits.
    // All elevation reads are gated by earthSpecTex (ocean mask): if specMask > 0.5
    // the texel is ocean and terrainH is forced to 0 regardless of the elevation pixel.
    const float kElevRange  = 8848.0;
    const float kMaxTerrain = 9000.0;  // terrain shell height (m) — just above Everest

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
        obsGroundH = (obsSpec > 0.5) ? 0.0 : max(0.0, textureLod(earthElevTex, uv, 0.0).r * kElevRange);
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
        tExit = min(tExit, 250000.0);

        // Quadratic step distribution: steps grow proportionally to their index so
        // near terrain gets fine resolution (~40 m at step 0) while far terrain gets
        // coarser (~5 km at step 95). This catches low/mid-altitude ranges (Rockies,
        // Alps) that uniform spacing misses when tExit is hundreds of km.
        // Jitter is kept as a normalised fraction [0,1] so its absolute magnitude
        // scales with tExit — sample positions stay proportional to the march range
        // regardless of view direction, eliminating the per-frame drift that causes
        // flickering when panning.
        const int kN  = 96;
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
            float terrainH = (specPx > 0.5) ? 0.0 : max(0.0, textureLod(earthElevTex, uv, 0.0).r * kElevRange);

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
                    float mT    = (mSpec > 0.5) ? 0.0 : max(0.0, textureLod(earthElevTex, mUV, 0.0).r * kElevRange);
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
                    float hE2 = max(0.0, textureLod(earthElevTex, hitUV + vec2(kTexU, 0.0), 0.0).r * kElevRange);
                    float hW2 = max(0.0, textureLod(earthElevTex, hitUV - vec2(kTexU, 0.0), 0.0).r * kElevRange);
                    float hN2 = max(0.0, textureLod(earthElevTex, hitUV - vec2(0.0, kTexV), 0.0).r * kElevRange);
                    float hS2 = max(0.0, textureLod(earthElevTex, hitUV + vec2(0.0, kTexV), 0.0).r * kElevRange);
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
    float tEnd   = (tSurface > 0.0) ? min(tAtmos.y, tSurface) : tAtmos.y;

    float segLen = tEnd / float(N_VIEW);
    float cosA   = dot(dir, sunDir);
    float pR     = phaseR(cosA);
    float pM     = phaseM(cosA);

    vec3  accumR  = vec3(0.0);
    float accumM  = 0.0;
    float odR_cam = 0.0;
    float odM_cam = 0.0;

    for (int i = 0; i < N_VIEW; ++i) {
        vec3  sp  = obsPos + dir * ((float(i) + 0.5) * segLen);
        float len = length(sp);
        if (len < R_EARTH) sp *= R_EARTH / len;
        float h = max(0.0, length(sp) - R_EARTH);

        float densR = exp(-h / H_R) * segLen;
        float densM = exp(-h / H_M) * segLen;
        odR_cam += densR;
        odM_cam += densM;

        vec2 tSunEarth = raySphere(sp, sunDir, R_EARTH);
        if (tSunEarth.x > 0.0 && tSunEarth.y > 0.0) continue;

        vec2 tSun  = raySphere(sp, sunDir, R_ATMOS);
        vec2 sunOD = (tSun.y > 0.0) ? optDepth(sp, sunDir, tSun.y) : vec2(0.0);

        vec3 tau  = BETA_R       * (odR_cam + sunOD.x)
                  + BETA_M * 1.1 * (odM_cam + sunOD.y);
        vec3 attn = exp(-tau);

        accumR += attn * densR;
        accumM += dot(attn, vec3(1.0 / 3.0)) * densM;
    }

    vec3 color = SUN_INTENSITY * (pR * BETA_R * accumR + vec3(pM * BETA_M * accumM));

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
            const float kEarth = 0.018 * 0.2;

            // Build the moon's local face frame: moonZ points toward the observer
            // (tidally locked near side), moonX/moonY span the visible face plane.
            vec3 moonZ = -moonDir3;
            vec3 refUp = abs(moonZ.z) < 0.99 ? vec3(0.0, 0.0, 1.0) : vec3(1.0, 0.0, 0.0);
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

            float discFade = smoothstep(limbZ - 0.006, limbZ + 0.002, moonDirENU.z);
            vec3 moonColor = texColor * (diffuse + kEarth) * limbDark * kMoonBright;
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
            color += hClip * gElev * glow * intens * 0.06 * vec3(1.0, 0.96, 0.88) * flareAttn;
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

        vec3  shadingN = (tHit > 0.0) ? terrainNorm : normalize(hitPt);
        float sunDot   = dot(shadingN, sunDir);
        float dayFrac  = smoothstep(-0.1, 0.3, sunDot);
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
        vec3 surfColor  = mix(nightColor * 0.12,
                              dayColor * clamp(sunDot * 1.5, 0.05, 1.0),
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
                    const int N_REFL = 6;
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
            float diff    = pow(max(0.0, dot(waveN, sunDir)) * 0.4 + 0.6, 80.0) * dayFrac;
            vec3 refracted = kSeaBase * dayFrac + diff * kSeaWaterColor * 0.12;

            // Fresnel blend (distance-attenuated to prevent orbit-scale glowing ring)
            surfColor = mix(refracted, reflColor, reflStr);

            // Wave-height crest shading: raised crests catch more water-color light
            float atten = max(1.0 - dist * dist * 1e-5, 0.0);
            surfColor += kSeaWaterColor * max(pHeight - kSeaHeight, 0.0) * 0.18 * atten * dayFrac;

            // Specular: shininess narrows close-up, broadens with distance
            float specPow = clamp(600.0 / max(1.0, sqrt(dist)), 8.0, 600.0);
            float nrm     = (specPow + 8.0) / (PI * 8.0);
            surfColor    += pow(max(0.0, dot(reflect(dir, waveN), sunDir)), specPow) * nrm * dayFrac;
        }

        color += surfColor * surfAttn;
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
    color += vec3(0.0025, 0.003, 0.004) * moonIllum * moonEl * nightAmt;

    // ── Moon glow: tight corona + wide diffuse halo ───────────────────────────
    if (moonDirENU.z > limbZ - 0.05) {
        vec3  moonDir3  = normalize(moonDirENU.xyz);
        float moonAngle = acos(clamp(dot(dir, moonDir3), -1.0, 1.0));
        float moonFade  = smoothstep(limbZ - 0.006, limbZ + 0.002, moonDirENU.z);

        // Tight inner corona — peaks at the disc edge (~0.014 rad), falls to ~8% at 3× disc radius.
        // sigma = 0.012 rad ≈ 0.7°; gives a crisp bloom ring without polluting the wider sky.
        float corona = exp(-moonAngle * moonAngle / (2.0 * 0.012 * 0.012)) * nightAmt;
        color += hClip * moonFade * corona * vec3(0.92, 0.94, 1.00) * moonIllum * 0.04;

        // Wide diffuse halo — very broad Gaussian (sigma ≈ 1.8 rad) that lifts the whole
        // night sky slightly around the moon, matching the real scattered moonlight glow.
        float scale = 100.0;
        float halo  = exp(-moonAngle * moonAngle / (2.0 * 0.018 * 0.018 * scale * scale));
        color += hClip * moonFade * halo * vec3(0.88, 0.90, 1.00) * moonIllum * 0.012;
    }

    // ── Sun disc + atmospheric corona ─────────────────────────────────────────
    if (sunDirENU.w > limbZ - 0.1) {
        float angle     = acos(clamp(cosA, -1.0, 1.0));
        float disc      = 1.0 - smoothstep(0.007, 0.010, angle);
        float corona    = exp(-angle * angle / (2.0 * 0.035 * 0.035));
        float fade      = smoothstep(limbZ - 0.006, limbZ + 0.002, sunDirENU.w);
        float horizClip = hClip;
        vec3  sunCol    = vec3(1.5, 1.3, 1.0);
        color += horizClip * fade * (disc * sunCol + corona * sunCol * 0.12);
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
        // Per-satellite positions from flareEntries[] — real sky directions, not
        // bin centres. The smooth outer glow is in sat_point.frag; lensFlare()
        // here adds the spiky noise-driven corona (f0) and ghost artifacts (f1–f6).
        {
            const float kFlareThr = 1.0;
            int nFlares = int(min(glowBuf.flareCount, 8u));
            for (int gi = 0; gi < nFlares; ++gi) {
                vec4 e = glowBuf.flareEntries[gi];
                if (e.z < limbZ - 0.02) continue;
                if (e.w < kFlareThr) continue;

                vec3 satCam = mat3(pc.skyView) * normalize(e.xyz);
                if (satCam.z >= -0.01) continue;
                vec2 satUV = vec2(satCam.x, -satCam.y) / (-satCam.z * tanHF * 2.0);

                float intens     = clamp(log2(max(e.w, 1.0)) / log2(16.0), 0.0, 1.0);
                float entryScale = intens * sqrt(intens); // intens^1.5, smooth fade-in
                vec3  tint       = vec3(1.3, 1.15, 1.0);
                flareAccum += lensFlare(fragUV, satUV, intens, 0.3) * tint * entryScale * 0.25;
            }
        }

        // ── Sun lens flare ──────────────────────────────────────────────────────
        // The sun is always at intens=1.0 when above the horizon.
        // A separate horizon fade ensures the flare vanishes as the sun sets.
        // The sun disc and atmospheric corona are handled above (physics-based);
        // this adds only the camera optical artifact layer on top.
        if (pc.sunDirENU.w > -0.01) {
            float sunIntensity = 10 * clamp(pc.sunDirENU.w, 0, 1.0);
            vec3 sunCam = mat3(pc.skyView) * normalize(pc.sunDirENU.xyz);
            if (sunCam.z < -0.01) {
                vec2 sunUV    = vec2(sunCam.x, -sunCam.y) / (-sunCam.z * tanHF * 2.0);
                float sunFade = clamp(pc.sunDirENU.w * 5.0 + 0.5, 0.0, 1.0);
                vec3  sunTint = vec3(1.4, 1.2, 0.9);
                flareAccum += lensFlare(fragUV, sunUV, sunIntensity, 2.0) * sunTint * sunFade * 0.45;
            }
        }

        color += flareAccum;
    }

    outColor = vec4(color, 1.0);

    // Terrain occlusion depth for subsequent satellite/star passes.
    // Satellites and stars are drawn with gl_Position.z = 0.5 (fixed) and tested with LESS.
    // Close terrain hits write [0, 0.5) so they block those overlays; sky writes 1.0 so they pass.
    // The 150 km cap prevents space-view terrain from incorrectly culling near satellites.
    const float kOcclusionCap = 150000.0;
    gl_FragDepth = (tHit >= 0.0 && tHit < kOcclusionCap)
                   ? tHit / (kOcclusionCap * 2.0)
                   : 1.0;
}
