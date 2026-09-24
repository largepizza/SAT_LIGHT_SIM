#ifndef SATLIGHTSIM_EARTH_ENV_GLSL
#define SATLIGHTSIM_EARTH_ENV_GLSL
// Earth + atmosphere radiance along an ARBITRARY ray, in Earth-centred ECEF coordinates (lighting
// overhaul Phase 4, .plans/SAT_RENDERER_PHASE4.md — "reflections: per-pixel ray"). What a mirror,
// solar array or stainless hull on a satellite reflects: the analytic atmosphere, the textured
// ground (day side, city lights on the night side), and a flat cloud deck. Grown out of the Potato
// sky (sat_sky_minimal.frag: analyticSky / flatClouds), but written in ECEF from any origin instead
// of the observer's ENU frame. Deliberately NOT the full sky: no volumetric clouds (cloud_march is
// screen-space), no aurora/airglow yet, no sun disc — the sun's reflection is the mesh shader's own
// GGX sun lobe (the disc folded into α, as in the photometry), so including it here would double it.
//
// The includer declares, before #include:  sampler2D earthDayTex, earthNightTex, earthCloudsTex.
// Needs common.glsl (raySphere, R_EARTH, R_ATMOS, BETA_*_BASE, H_R, H_M, phaseR/M, SUN_INTENSITY)
// and terrain.glsl (posToUV).
//
// Output units: sat_sky.frag's PRE-exposure radiance (the mesh adds it to its lit colour, and one
// exposure/tonemap is applied to the sum). The Potato constants are display-referred, so the result
// is scaled by kEnvToScene ≈ 1 / EXPOSURE_DAY: a reflection of the sunlit Earth then reads, after
// the day exposure, as bright as the Potato sky draws it.

const float kEnvToScene = 0.55;

const float kEnvRayleighGain = 2.0;
const float kEnvMieGain      = 0.2;
const float kEnvCloudAltM    = 3000.0;
const float kEnvCloudCover   = 0.55;
const float kEnvCloudDensity = 3.0;
const float kEnvCloudAlpha   = 0.95;
const float kEnvCloudDrift   = 0.03;
const vec3  kEnvCloudDay     = vec3(1.00, 0.98, 0.95);
const vec3  kEnvCloudDusk    = vec3(0.78, 0.36, 0.15);
const vec3  kEnvCloudNight   = vec3(0.008, 0.011, 0.020);

float envSmootherstep(float e0, float e1, float x)
{
    float t = clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * t * (t * (t * 6.0 - 15.0) + 10.0);
}

float envAirmass(float sinEl)
{
    if (sinEl <= 0.0) return 40.0 - sinEl * 260.0;
    float elDeg = degrees(asin(min(sinEl, 1.0)));
    return 1.0 / (sinEl + 0.50572 * pow(elDeg + 6.07995, -1.6364));
}

// sat_sky_minimal.frag's analyticSky(), in ECEF: `ro` is the ray origin relative to Earth's centre.
vec3 envAtmosphere(vec3 ro, vec3 rd, float tGround, vec3 sun, out float viewTrans)
{
    float b     = dot(ro, rd);
    float tLow  = clamp(-b, 0.0, (tGround > 0.0) ? tGround : 1.0e12);
    vec3  pLow  = ro + rd * tLow;
    float hLow  = clamp(length(pLow) - R_EARTH, 0.0, 1.0e6);
    float rayEl = abs(dot(rd, normalize(pLow)));

    const int kSunTaps = 16;
    vec2  tShell  = raySphere(ro, rd, R_ATMOS);
    float hitsAir = step(0.0, tShell.y);
    float segA = max(tShell.x, 0.0);
    float segB = (tShell.y > 0.0) ? tShell.y : 0.0;
    if (tGround > 0.0) segB = min(segB, tGround);

    float sunVis = 0.0, sunSinEl = 0.0;
    for (int i = 0; i < kSunTaps; ++i) {
        float t  = mix(segA, segB, (float(i) + 0.5) / float(kSunTaps));
        vec3  sp = ro + rd * t;
        float bs = dot(sp, sun);
        float sv = 1.0;
        if (bs < 0.0) {
            float mr = length(sp + sun * (-bs));
            sv = envSmootherstep(R_EARTH - 60000.0, R_EARTH + 160000.0, mr);
        }
        sunVis   += sv;
        sunSinEl += dot(normalize(sp), sun);
    }
    sunVis   = envSmootherstep(0.0, 1.0, sunVis / float(kSunTaps));
    sunSinEl /= float(kSunTaps);

    float densR = exp(-hLow / H_R);
    float densM = exp(-hLow / H_M);
    float amV   = envAirmass(max(rayEl, 0.02));
    float amS   = envAirmass(sunSinEl);
    vec3  odR    = BETA_R_BASE       * (H_R * densR * amV) * hitsAir;
    float odM    = BETA_M_BASE * 1.1 * (H_M * densM * amV) * hitsAir;
    vec3  odRsun = BETA_R_BASE       * (H_R * densR * amS) * hitsAir;
    float odMsun = BETA_M_BASE * 1.1 * (H_M * densM * amS) * hitsAir;
    viewTrans = exp(-dot(odR + vec3(odM), vec3(1.0 / 3.0)));

    float mu = dot(rd, sun);
    vec3 inR = phaseR(mu) * (vec3(1.0) - exp(-odR)) * exp(-odRsun);
    vec3 inM = phaseM(mu) * (1.0 - exp(-odM)) * exp(-vec3(odMsun));
    float lowSun = 1.0 - smoothstep(0.03, 0.38, sunSinEl);
    float azBias = mix(1.0, mix(0.42, 1.18, smoothstep(-0.55, 0.55, mu)), lowSun);
    return max(SUN_INTENSITY * sunVis * azBias * (inR * kEnvRayleighGain + inM * kEnvMieGain), vec3(0.0));
}

// Radiance along ro + t·rd (ECEF, ro relative to Earth's centre). `lod` blurs the ground and cloud
// textures for rough reflectors; `cloudPhase` is the Earth rotation angle (drifts the deck like the
// Potato sky). Stars and the Moon are not included.
vec3 earthEnv(vec3 ro, vec3 rd, vec3 sun, float lod, float cloudPhase)
{
    vec2  tb = raySphere(ro, rd, R_EARTH);
    float tGround = (tb.x > 0.0) ? tb.x : -1.0;
    float viewTrans;
    vec3  sky = envAtmosphere(ro, rd, tGround, sun, viewTrans);
    vec3  color = sky;
    if (tGround > 0.0) {
        vec3  p      = ro + rd * tGround;
        vec2  uv     = posToUV(p);
        vec3  dayC   = textureLod(earthDayTex, uv, lod).rgb;
        vec3  nightC = textureLod(earthNightTex, uv, lod).rgb;
        float lit    = smoothstep(-0.10, 0.18, dot(normalize(p), sun));
        vec3  ground = mix(nightC * 1.3, dayC * (0.05 + 0.95 * lit), lit);
        color = ground * viewTrans + sky * (1.0 - viewTrans);
    }
    // Flat cloud deck (the Potato sky's flatClouds, in ECEF).
    vec2  tc = raySphere(ro, rd, R_EARTH + kEnvCloudAltM);
    float t  = (tc.x > 1.0) ? tc.x : tc.y;
    if (t > 1.0 && (tGround < 0.0 || t < tGround)) {
        vec3  c  = ro + rd * t;
        vec2  uv = posToUV(c);
        uv.x = fract(uv.x + cloudPhase * kEnvCloudDrift);
        float raw   = textureLod(earthCloudsTex, uv, lod).r;
        float alpha = clamp((raw - (1.0 - kEnvCloudCover)) * kEnvCloudDensity, 0.0, kEnvCloudAlpha);
        if (alpha > 0.0) {
            float sunDot = dot(normalize(c), sun);
            float night  = smoothstep(0.12, 0.0, sunDot);
            float dusk   = exp(-pow((sunDot - 0.06) / 0.075, 2.0)) * (1.0 - night);
            vec3  lit    = mix(mix(kEnvCloudDay, kEnvCloudNight, night), kEnvCloudDusk, dusk);
            color = mix(color, lit, alpha);
        }
    }
    return color * kEnvToScene;
}

#endif // SATLIGHTSIM_EARTH_ENV_GLSL
