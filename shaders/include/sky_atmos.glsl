// sky_atmos.glsl — the sky's single-scattering atmosphere helpers, shared by sat_sky.frag (the per-pixel loop) and
// sky_ap.comp (the aerial-perspective volume: the same loop once per screen tile, 2026-10-25). Moved here unchanged
// from sat_sky.frag so the two cannot drift.
// Needs, before it: common.glsl (R_EARTH, R_ATMOS, H_R, H_M, SUN_INTENSITY, raySphere, phaseR/phaseM),
// cloud_params.glsl (cloud.lightSamples) and a bool dbgSkipSunOD().

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
// kAirglowRedPeakM/HalfWidthM moved to cloud_march.comp with the red band's march itself —
// kAirglowRedColor stays here too, still used by auroraSampleAt's color blend below.
const vec3  kAirglowRedColor      = vec3(1.0, 0.12, 0.05);
// Horizontal patchiness so the bands don't read as a perfectly flat, featureless ring
// around the sky (a pure function of altitude alone has zero horizontal variation).
// Reuses the analytic warpPerlin3 noise already used for cloud domain warp — no new
// texture/binding, matches the "reuse existing noise infra" C15 design directive
// (which pre-dates the cloud warp's migration from a noiseTex lookup to this analytic
// evaluator — see cloudWarpOffset's comment; follow the current code, not the stale plan).
const float kAirglowNoiseFreq = 4.0;
// (kAirglowDriftRate, a straight drift of the noise at 0.015 units/s of pc.waveTime, is gone since 2026-11-12: the
// pattern swept past at ~24 km/s and jumped at 00:00 UTC, where pc.waveTime = sim seconds in the day wraps.
// airglowTimeOfs / airglowWindDir below.)
// First-pass brightness scale, same convention as kNightGlowScale/kCityUpwellStrength
// above: raw accumulation (density × segLen, summed over qualifying march samples) is
// a large unnormalized number, this brings it into visible range. Deliberately
// conservative — real airglow is famously faint. Tune via cloud.airglowGain (settings
// slider) rather than editing this constant.
const float kAirglowScale = 0.0000005;

// The mean of exp(-h / H) over a stretch of an atmosphere step whose height runs linearly from a to b (2026-10-23): the
// N_VIEW loop's densities, integrated over each step. A midpoint sample of a 1-3 km scale height over a ~15-km step was
// off by tens of times and changed with the step count (rings round the zenith). Three scale heights at once (Rayleigh,
// Mie, the haze aloft) from the exponentials already taken (ea, eb: the loop carries each step's end to the next step's
// start), dh = b - a: (ea - eb) H / dh, one reciprocal for the three (2026-10-24, perf: three divisions a step before).
// Below 8 m of height change the trapezoid, within 4e-6 of the exact mean for every H here.
const vec3 kAtmH = vec3(H_R, H_M, 2500.0);
vec3 atmHalfAvg3(vec3 ea, vec3 eb, float dh)
{
    return abs(dh) > 8.0 ? (ea - eb) * (kAtmH / dh) : 0.5 * (ea + eb);
}
const vec3 kAtmInvH = vec3(1.0 / H_R, 1.0 / H_M, 1.0 / 2500.0);   // Rayleigh, Mie, the haze aloft

// Marches N_LIGHT steps from point p toward direction d over distance segTotal
// and returns (Rayleigh optical depth, Mie optical depth) — i.e. ∫ρ(h) ds for each species.
// Multiply by BETA_R / BETA_M in the caller to convert to actual extinction coefficients.
// Called once per view sample to accumulate the sun-side transmittance at that altitude.
// Kept local, NOT shared with cloud_march.comp's copy of the same integral. See that file's
// optDepth comment for why: sharing forces a runtime trip count, and cloud_march's is a
// compile-time constant it needs to keep. This copy's count is settings-tunable, so it has a
// runtime bound either way and loses nothing by staying here.
vec2 optDepth(vec3 p, vec3 d, float segTotal) {
    // Perf knockout: zero optical depth = "sun ray unattenuated", the same fallback the
    // callers already use when tSun.y <= 0 (no atmosphere intersection) — a safe, already-
    // exercised code path, not a new one. Isolates every optDepth() call site (main N_VIEW
    // loop, ocean sky-reflection loop, and both fixed-count supplemental marches) at once.
    if (dbgSkipSunOD())
        return vec2(0.0);
    int   N_LIGHT = int(max(2.0, cloud.lightSamples));
    float sLen = segTotal / float(N_LIGHT);  // length of each sun-ray sub-step
    float odR = 0.0, odM = 0.0;
    for (int i = 0; i < N_LIGHT; ++i) {
        float h = max(0.0, length(p + d * (float(i) + 0.5) * sLen) - R_EARTH);  // altitude at sub-step midpoint
        odR += exp(-h / H_R);  // Rayleigh density (exponential profile, scale height H_R)
        odM += exp(-h / H_M);  // Mie density (exponential profile, scale height H_M)
    }
    return vec2(odR, odM) * sLen;  // summed densities x step length -> optical depth units
}

// The Sun's path from p along d as an optical depth, EXACTLY (2026-10-26, with the sky's multiple scattering): the
// Rayleigh and Mie columns by the Chapman function (atmosphere.glsl atmColumnInf: the tangent point of a grazing path
// counted, which optDepth's few midpoints step over) plus ozone (atmosphere.glsl atmOzoneTau: the Chappuis band, a
// layer from 15 to 35 km). Ozone takes the orange and
// yellow out of the long twilight paths: it is why the twilight zenith is deep blue (without it, olive or grey). The
// same model as the clouds' Sun (cloud_v2_sunlut.comp sunTransmit). The caller tests the Earth's shadow.
#ifdef SKY_LITE
vec3 sunPathTauExact(vec3 p, vec3 d, vec3 BR, float BM) { return vec3(0.0); }   // (never taken there)
#else
vec3 sunPathTauExact(vec3 p, vec3 d, vec3 BR, float BM)
{
    float r = length(p), cz = dot(p, d) / r;
    float odR = atmColumnInf(r, cz, H_R) * H_R, odM = atmColumnInf(r, cz, H_M) * H_M;
    return BR * odR + BM * 1.1 * odM + atmOzoneTau(p, d);
}
// Its Rayleigh and Mie columns alone (atmStepAttn carries them along a step by height).
vec2 sunPathCols(vec3 p, vec3 d)
{
    float r = length(p), cz = dot(p, d) / r;
    return vec2(atmColumnInf(r, cz, H_R) * H_R, atmColumnInf(r, cz, H_M) * H_M);
}
#endif

// ── A step's sunlight to the eye, integrated over the step (2026-11-04) ──────────────────────────────────────────────
// The single-scattering loops (sat_sky.frag's N_VIEW loop, sky_ap.comp) took the Sun's path and the eye's column at ONE
// point of a step (the middle of its lit part) and applied that transmittance to the whole step's air. With the default
// ~15-km steps that is the coarse part of the integral: the air is concentrated at the step's low end (Mie 1.2-km scale
// height), and toward a low Sun the low end is also the most extinguished, so the middle's transmittance over-lit the
// step, most for the Mie and the blue. Under a sunset storm the air in front of the clouds (a piece of the first step,
// which took the WHOLE step's middle) read ~2x a 512-step reference: the blue-grey veil of issue_20261104_020521.
// Here the transmittance is averaged over the stretch [t0, t1] of a step, weighted by each species' own density:
//  - the Sun's columns are taken once (cS at height hRef: the call the loop already made) and carried to any point of
//    the step by height, cS x exp(-(h - hRef) / H) (the Chapman column of a fixed Sun direction scales so; ozone as is);
//  - the eye's columns to a point are the step start's plus the exact mean of exp(-h / H) for a height linear from hS;
//  - Rayleigh: two Gauss-Legendre nodes, density-weighted; Mie: the median of its own column along the stretch
//    (where its mass is: a step climbing 5 km holds most of its Mie in its first fifth).
// Against a fine reference (a Python model of the loop, low Sun, 4-15 km stretches climbing 0-13 km): Rayleigh within
// ~1%, Mie within ~4%, where the one-point form was +10..+87% for a piece and +14% for a whole 15-km step. In the app,
// the report's storm with the loop everywhere: cloud mean RGB 154,100,84 -> 146,80,59 (512-step loop 146,80,57).
// xyz: the Rayleigh-weighted transmittance (Sun -> air -> eye) over [t0, t1]; w: the Mie-weighted mean of its channels.
// oo / b2: the ray's closed form r^2 = oo + t (b2 + t); tS / hS / eS (exp(-hS / H_R, H_M)) / odS: the step's start and
// the eye's columns there; odMid: the eye's columns to the step's middle; mK: the step's Mie column over the pure Mie
// one (the haze near clouds rides it). One loop over the nodes, not unrolled (the sky pass's speed follows its code
// size). Not in SKY_LITE (its SPIR-V budget): it keeps the one-point form.
// Whole steps: 1 = 3 nodes where the Sun's transmittance changes over them, 0 = the middle alone. The callers use 1 only
// with the eye inside the atmosphere: from orbit the one-point loop is already within ~0.1 level of a 512-step one, and
// the band there (the whole limb ring) paid +0.3-0.5 ms for the nodes. The piece in front of the clouds always takes them.
const int kStepNodes = 1;
vec4 atmStepAttn(float oo, float b2, float tS, float hS, vec2 eS, vec2 odS, vec2 odMid, float mK, float t0, float t1,
                 float hRef, vec2 cS, vec3 oz, vec3 BR, float BM, int nodes)
{
    // One node, the loop's form before 2026-11-04 (the eye's columns to the step's middle, odMid, and the Sun's at hRef),
    // where the Sun's transmittance hardly changes over the stretch: the spread of its exponent over the stretch's climb
    // (the Sun's columns carried by height) under 0.5 (day, high air, the volume's short near slices); or where the eye
    // sees nothing of it (red transmittance from the eye under e^-8: the far side of the limb); or when not asked (nodes
    // 0). The eye's own column over a step changes near-linearly and the middle gets it right to ~1%. Toward a low Sun
    // near the ground or along the limb: 3 nodes; always for nodes 2 (a stretch hRef is not the middle of).
    float r0 = 0.0, r1 = 0.0, spread = 0.0;
    if (nodes == 1) {
        r0 = sqrt(max(oo + t0 * (b2 + t0), 0.0));
        r1 = sqrt(max(oo + t1 * (b2 + t1), 0.0));
        vec2 gH = exp(min(abs(r1 - r0) * kAtmInvH.xy, vec2(30.0))) - 1.0;
        spread = BR.z * cS.x * gH.x + BM * 1.1 * cS.y * gH.y;
    }
    if (nodes == 0 || (nodes == 1 && (spread < 0.5 || BR.x * odS.x + BM * 1.1 * odS.y > 8.0))) {
        vec3 a = exp(-(BR * (odMid.x + cS.x) + BM * 1.1 * (odMid.y + cS.y) + oz));
        return vec4(a, dot(a, vec3(1.0 / 3.0)));
    }
    float L  = t1 - t0;
    if (nodes == 2) {
        r0 = sqrt(max(oo + t0 * (b2 + t0), 0.0));
        r1 = sqrt(max(oo + t1 * (b2 + t1), 0.0));
    }
    // the Mie column's median along the stretch: x = -ln(1 - (1 - e^-k) / 2) / k, k = its climb in Mie scale heights
    float k  = clamp((r1 - r0) * kAtmInvH.y, -30.0, 30.0);
    float xM = abs(k) < 1e-3 ? 0.5 : -log(1.0 - 0.5 * (1.0 - exp(-k))) / k;
    vec3  xs = vec3(0.2113249, 0.7886751, xM);
    vec2  eRefInv = exp(min(hRef * kAtmInvH.xy, vec2(80.0)));   // (where 3 nodes run the air is low: no overflow)
    vec3  sR = vec3(0.0), aM = vec3(0.0);
    float wR = 0.0;
    [[dont_unroll]] for (int j = 0; j < 3; ++j) {
        float t   = t0 + xs[j] * L;
        float h   = max(0.0, sqrt(max(oo + t * (b2 + t), 0.0)) - R_EARTH);
        vec2  e   = exp(-h * kAtmInvH.xy);
        float dh  = h - hS;
        vec2  cam = odS + (abs(dh) > 8.0 ? (eS - e) * (kAtmH.xy / dh) : 0.5 * (eS + e)) * ((t - tS) * vec2(1.0, mK));
        vec2  sun = cS * (e * eRefInv);   // (exp(-(h - hRef) / H), from the node's own exponentials)
        vec3  a   = exp(-(BR * (cam.x + sun.x) + BM * 1.1 * (cam.y + sun.y) + oz));
        if (j < 2) { sR += e.x * a; wR += e.x; }
        if (j == 2) aM = a;
    }
    return vec4(sR / max(wR, 1e-30), dot(aM, vec3(1.0 / 3.0)));
}

// The Earth's shadow along a ray, EXACTLY (2026-10-13): the shadow of a sphere lit from infinity is the cylinder
// |p - (p.s) s| < R_EARTH on the anti-Sun side (p.s < 0), so the ray ro + t rd is in it over one interval
// [x, y] (x > y: never). The same test as raySphere(p, s, R_EARTH) having two positive roots, for points above the
// ground. The main atmosphere loop has its own inline copy (tShA / tShB in main()).
vec2 earthShadowSpan(vec3 ro, vec3 rd, vec3 s) {
    vec3  op = ro - dot(ro, s) * s;
    vec3  dp = rd - dot(rd, s) * s;
    float qa = dot(dp, dp);
    float qb = dot(op, dp);
    float lo = length(op);
    float qd = qb * qb - qa * (lo - R_EARTH) * (lo + R_EARTH);   // raySphere's cancellation-free c
    if (qa <= 1e-12 || qd <= 0.0) return vec2(1e30, -1e30);
    float q  = sqrt(qd);
    vec2  t  = vec2(-qb - q, -qb + q) / qa;
    float os = dot(ro, s), ds = dot(rd, s);                       // clip to the anti-Sun half-space
    if (abs(ds) > 1e-9) {
        float tc = -os / ds;
        if (ds > 0.0) t.y = min(t.y, tc); else t.x = max(t.x, tc);
    } else if (os >= 0.0) t = vec2(1e30, -1e30);
    return t;
}
// The step [t0, t1] against that span: x = its sunlit share, y = the middle of its lit part (the step's middle when
// it is all lit). The sky-light integrals below tested only each step's MIDPOINT: with this pass's 2-sample optDepth
// (light_samples) a grazing sun path just outside the shadow is far from black, so a whole step's light switched
// on or off at the Sun elevation where its midpoint crossed the shadow — a contour line across the terrain's sky
// light, the shadowed air's sky light and the sea's reflected sky (zenmodel.py: up to 50% in 0.01 deg).
vec2 earthShadowLit(vec2 sh, float t0, float t1) {
    float ov = max(0.0, min(t1, sh.y) - max(t0, sh.x));
    float tL = 0.5 * (t0 + t1);
    if (ov > 0.0) tL = (sh.x <= t0) ? 0.5 * (sh.y + t1) : 0.5 * (t0 + sh.x);
    return vec2(clamp(1.0 - ov / max(t1 - t0, 1e-3), 0.0, 1.0), tL);
}

// Zenith sky radiance at a point (review 7): the light that reaches air in the Earth's shadow — the
// march's skyZenithAt, with this pass's optDepth for the sun path. Four steps: it lights the shadowed
// stretch of a ray, whose total is small.
vec3 skyZenithSky(vec3 p, vec3 sunDir, vec3 BETA_R, float BETA_M) {
    vec3 zen = normalize(p);
    vec2 tSA = raySphere(p, zen, R_ATMOS);
    if (tSA.y <= 0.0) return vec3(0.0);
    float seg = tSA.y * 0.25;
    float odR = 0.0, odM = 0.0, accM = 0.0;
    vec3  accR = vec3(0.0);
    vec2  shS = earthShadowSpan(p, zen, sunDir);
    for (int i = 0; i < 4; ++i) {
        vec3  sp = p + zen * ((float(i) + 0.5) * seg);
        float h  = max(0.0, length(sp) - R_EARTH);
        float dR = exp(-h / H_R) * seg, dM = exp(-h / H_M) * seg;
        odR += dR; odM += dM;
        vec2 lit = earthShadowLit(shS, float(i) * seg, float(i + 1) * seg);
        if (lit.x <= 0.0) continue;
        if (lit.x < 1.0) {   // the lit part: its own middle's air and sun path (the midpoint may be in the shadow)
            sp = p + zen * lit.y;
            float hl = max(0.0, length(sp) - R_EARTH);
            dR = exp(-hl / H_R) * seg * lit.x; dM = exp(-hl / H_M) * seg * lit.x;
        }
        vec2 tS  = raySphere(sp, sunDir, R_ATMOS);
        vec2 so  = (tS.y > 0.0) ? optDepth(sp, sunDir, tS.y) : vec2(0.0);
        vec3 att = exp(-(BETA_R * (odR + so.x) + BETA_M * 1.1 * (odM + so.y)));
        accR += att * dR;
        accM += dot(att, vec3(1.0 / 3.0)) * dM;
    }
    float cu = dot(zen, sunDir);
    return SUN_INTENSITY * (phaseR(cu) * BETA_R * accR + vec3(phaseM(cu) * BETA_M * accM));
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

// Airglow coverage patchiness (cloud.airglowCoverageGain) for the green/sodium bands — a
// lower-frequency companion to kAirglowNoiseFreq's mild +-40% shimmer above, remapped through a
// threshold (same idiom as cloud_march.comp's own copy / auroraCoverage) so there are real dim
// gaps between brighter patches instead of a gentle wobble. gain=0 reproduces the old uniform-
// minus-shimmer look exactly; gain=1 is full patchiness. Duplicated in cloud_march.comp (which
// needs its own copy for the red band's supplemental march) rather than shared — matches this
// file's standing convention for small per-shader noise helpers, see the aurora functions above.
// ── Airglow over time (2026-11-12) ───────────────────────────────────────────
// The noise's time offset: a closed loop through noise space once a UTC day (t = pc.waveTime, sim seconds in the
// day, which wraps 86400 -> 0 with no jump here). Moving the sphere through the 3D noise MORPHS its patches rather
// than sliding them: at radius 0.6 the 1600-km features (kAirglowNoiseFreq 4) change at ~70 m/s, the speed real
// nightglow structure moves at (tens of m/s; the old drift was ~24 km/s).
// (Not in SKY_LITE, whose SPIR-V has a build-enforced budget: it has no green / sodium march.)
#ifndef SKY_LITE
const float kAirglowEvolveR = 0.6;
vec3 airglowTimeOfs(float t) {
    float th = t * (6.28318531 / 86400.0);
    return kAirglowEvolveR * vec3(cos(th), sin(th), 0.5 * sin(2.0 * th));
}
// ... and the mesopause's zonal wind carries the patterns round the pole: the sample's ECEF direction turned by
// 2 x the clouds' drift phase (cloud.cloudPhase, continuous sim time, ~47 m/s at the equator; a whole multiple of
// the phase, so its 2 pi wrap is seamless). With the daily loop the pattern does not repeat from night to night.
vec3 airglowWindDir(vec3 d) {
    float a = 2.0 * cloud.cloudPhase;
    float c = cos(a), s = sin(a);
    return vec3(c * d.x - s * d.y, s * d.x + c * d.y, d.z);
}
// The bands' brightness through the night and the year, per sample (x green 557.7, y sodium 589.3, z red 630.0; mean
// ~1 over a night, so the gains keep their meaning). r = the sample's direction, s = the Sun's, z = the Earth's axis,
// all in one frame. Hours since the sample's own sunset from the Sun's local hour angle and the sunset hour angle
// (latitude and season in it). What each band does (nightglow climatology, mid-latitudes):
//  * green: brightest after dusk, falling into the night, a weaker post-midnight rise (~2 h);
//  * sodium: little nightly change; a strong annual cycle, winter maximum ~2-3x summer (the hemisphere whose
//    pole points away from the Sun);
//  * red (F-region recombination): ~3x in the first hours after dusk, decaying through the evening as the
//    ionosphere does (x the equatorial anomaly in cloud_march.comp's own copy).
vec3 airglowNightK(vec3 r, vec3 s, vec3 z) {
    float rz = dot(r, z), sz = dot(s, z);
    float H  = atan(dot(cross(s, r), z), dot(s, r) - sz * rz);        // 0 = local noon, > 0 = afternoon / evening
    float cH0 = -rz * sz / max(sqrt(max((1.0 - rz * rz) * (1.0 - sz * sz), 0.0)), 1e-4);
    float H0 = acos(clamp(cH0, -1.0, 1.0));                             // sunset hour angle
    float tSS = mod(H - H0, 6.28318531) * (12.0 / 3.14159265);          // hours since sunset
    float x  = (H - sign(H) * 3.14159265) * (12.0 / 3.14159265);        // hours from local midnight
    float ev = exp(-0.4 * tSS);
    float green = 0.85 + 0.4 * ev + 0.15 * exp(-0.44 * (x - 2.0) * (x - 2.0));
    float na = (0.9 + 0.2 * ev) * (1.0 - 0.45 * clamp(rz * sz * 3.6, -1.0, 1.0));
    float red = 0.66 + 2.06 * exp(-0.5 * tSS);
    return vec3(green, na, red);
}
float airglowCoverageMask(vec3 dirECEF, float t, vec3 seedOffset) {
    float n = warpPerlin3(dirECEF * (kAirglowNoiseFreq * 0.5)
                          + 0.6 * airglowTimeOfs(t) + seedOffset);
    return smoothstep(-0.2, 0.5, n);
}
#endif
