#version 450
// ── Rain, snow and diamond dust at the eye as PARTICLES (2026-10-04) ───────────────────────────────
// Replaces cloud_march.comp's rainDrops (a per-pixel lattice search inside the half-res cloud composite:
// a few hundred fat streaks, smeared by the sky TAA). Drawn in the main pass after the TAA, at full
// resolution, one instanced quad per drop; everything is procedural from gl_InstanceIndex (no buffers).
//
// WHERE: nested boxes about the eye, world-fixed (the rain frame cv2.rainE/N/U, the eye's position in it
// from double): level k is a box of side L = 4 m x 2^k holding N0 x 2^k drops, drawn over distances
// [L/4, L/2] (level 0 from the eye), cross-faded with the next level over [0.4, 0.5] L. Each drop wraps
// within its box as it falls, so the eye walks through the rain. The fall and the wind are integrated over sim
// time on the CPU (cv2.rainMotion / rainWind, SatelliteSimRain.cpp updateRainMotion), so both can change smoothly.
//
// WHAT: a drop's size from the AREA-weighted Marshall-Palmer distribution at the rate round the eye (0.5-6 mm;
// each particle carries a share of the rain's cross-section), its fall speed the terminal velocity of that size
// (Atlas: 2.3 m/s at 0.5 mm .. 8.8 at 4 mm); a rain particle is redrawn elsewhere every "Drop persistence" (it
// stands for many drops: kept as one body it drifted as a lump); a snowflake an aggregate of 2-8 mm falling
// ~1 m/s, fluttering, followed for good; diamond dust a tumbling ice plate, seen only by its glint. Which drops exist follows the rain
// map around the eye (cv2RainMapAt), so walking toward a shaft its drops appear in the far levels first.
//
// HOW BRIGHT: a drop is drawn as the streak it sweeps over the shutter time (pc.eyeVel.w), and its COVERAGE
// is conserved: the streak's pixels together cover the drop's cross-section, so each pixel's alpha is the
// fraction of the exposure the drop spent on it. Each particle stands for (real drops per m^3) / (particles
// per m^3) drops — the rain's real geometric cross-section per m^3 at the rate (Marshall-Palmer) — so summed
// over a ray the particles' coverage is the rain's optical depth: the particles are the near part of the rain
// volume, drawn one drop at a time. Its radiance is the march's rain sample at the eye (the lightning pass's
// cv2RainKey / cv2RainAmb) through the SAME phase (cv2_optics.glsl): drops at 42 degrees from the antisolar
// point flash the bow's colours, drops toward the Sun glow in its diffraction lobe.
#include "depth.glsl"
#define CLOUD_PARAMS_BINDING 0
#include "cloud_params.glsl"
#include "common.glsl"
#define CV2_PARAMS_BINDING 1
#define CV2_PARAMS_ONLY
#include "clouds_v2.glsl"
#include "cv2_optics.glsl"
#define CV2_FLASH_BINDING 2
#include "cloud_lightning.glsl"
#include "rain_particles.glsl"

layout(location = 0) out vec2  vLocal;   // px: x along the streak from its start, y across it
layout(location = 1) flat out vec4 vColor;   // rgb the energy (pre-tonemap, x exposure) at full coverage, a the alpha
layout(location = 2) flat out vec3 vSeg;     // x the streak's length (px), y its drawn width (px), z the range (m)

uvec3 rpPcg3d(uvec3 v)
{
    v = v * 1664525u + 1013904223u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    v ^= v >> 16u;
    v.x += v.y * v.z; v.y += v.z * v.x; v.z += v.x * v.y;
    return v;
}
vec3 rpRnd(uint j, uint k, uint salt) { return vec3(rpPcg3d(uvec3(j, k, salt)) >> 8u) * (1.0 / 16777216.0); }

void cullDrop()
{
    gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
    vLocal = vec2(0.0);
    vColor = vec4(0.0);
    vSeg   = vec3(0.0);
}

vec3 rainToEnu(vec3 r, vec3 eX, vec3 eY, vec3 eZ)
{
    vec3 e = r.x * cv2.rainE.xyz + r.y * cv2.rainN.xyz + r.z * cv2.rainU.xyz;   // ECEF
    return vec3(dot(e, eX), dot(e, eY), dot(e, eZ));
}

// A particle's phase toward the eye for light arriving along L (toward the light). kind 0 a rain drop (the march's rain
// phase: the forward diffraction spike, the broad refracted lobe and the bows), 1 a snowflake (the march's snow phase:
// broad lobes and a little of the ice optics, + its plate glint), 2 diamond dust (a tumbling plate's glint only: a lobe
// about the mirror's half vector, the normal sweeping ~3 deg over the exposure, F ~0.05: phase pi F D).
vec3 dropPhase(uint kind, vec3 vdir, vec3 L, vec3 nPl, float og)
{
    vec3  hv = normalize(L - vdir);
    float cosK = dot(vdir, L);
    if (kind == 2u)
        return vec3(exp(-(1.0 - abs(dot(nPl, hv))) / 1e-3) * (0.05 / 2e-3) * og);
    if (kind == 1u) {
        vec4 habSnow = vec4(0.2, 0.35, 0.0, 0.0);
        return vec3(0.3 * cv2HG(cosK, 0.97) + 0.55 * cv2HG(cosK, 0.35))
             + og * (0.25 * cv2IceOptics(vdir, L, habSnow) + 0.15 * cv2PillarOptics(vdir, L))
             + vec3(exp(-(1.0 - abs(dot(nPl, hv))) / 6e-4) * (0.05 / 6e-4) * 0.5 * og);
    }
    return vec3(0.3 * cv2HG(cosK, 0.97) + 0.55 * cv2HG(cosK, 0.35)) + og * 0.5 * cv2RainOptics(vdir, L);
}

void main()
{
    uint N0 = uint(pc.params.x);
    uint id = uint(gl_InstanceIndex);
    uint q  = id / N0 + 1u;
    int  k  = findMSB(q);                       // level: q in [2^k, 2^(k+1))
    if (k >= int(pc.params.y)) { cullDrop(); return; }
    uint j  = id - N0 * ((1u << uint(k)) - 1u);
    float L = kRpL0 * float(1 << k);

    uint mode  = uint(pc.obsECEFDir.w + 0.5);
    bool dust  = (mode & 1u) != 0u;
    vec3 hK    = rpRnd(j, uint(k), 0x91u);              // fixed per particle: its kind and its redraw phase
    bool flake = dust || hK.x > cv2RainKeyDir.w;    // the snow share (sleet: some of each)

    // Rain drops are REDRAWN every "Drop persistence" (cv2.snowMotion.z = sim time / it, w = on; 2026-10-23): a
    // particle stands for tens (level 0) to thousands (level 4) of real drops, and kept as one body it crossed the
    // screen as a lump at a few px a frame — the "floaty" rain. Each epoch it is a new drop at a new place (its own
    // phase, so the field never pulses), fading in and out over the epoch's ends. Flakes and diamond dust are kept
    // (a real flake is followed by the eye).
    uint  ep  = 0u;
    float wEp = 1.0;
    if (!flake && cv2.snowMotion.w > 0.5) {
        float e  = cv2.snowMotion.z + hK.y;
        ep = uint(floor(e)) + 1u;
        float ph = fract(e);
        wEp = smoothstep(0.0, 0.2, ph) * (1.0 - smoothstep(0.8, 1.0, ph)) * 1.25;   // mean 1
    }
    uint kE = uint(k) | (ep << 3u);
    vec3 h0 = rpRnd(j, kE, 0x51u);
    vec3 h1 = rpRnd(j, kE, 0xA7u);
    vec3 h2 = rpRnd(j, kE, 0x3Cu);
    float t    = cv2.precip.y;

    // ── Where it is (in the rain frame, metres from the eye) ──
    float Dmm, v;
    if (dust)       { Dmm = 0.15; v = 0.25 + 0.2 * h1.y; }
    else if (flake) { Dmm = 2.0 + 6.0 * h1.y; v = 0.8 + 0.6 * h1.z; }
    else {
        // The size a unit of the rain's CROSS-SECTION comes in (2026-10-23): every particle carries the same share of
        // the rain's geometric cross-section (Aeff below), so its size is drawn from the AREA-weighted Marshall-Palmer
        // distribution, D^2 exp(-Lambda D) = Gamma(3, Lambda), with Lambda from the rate at the eye (pc.dropLambda:
        // 2.5 / mm at 10 mm/h, 1.7 at 60). It was the NUMBER distribution above 0.5 mm at a fixed 5 mm/h (median
        // 0.74 mm, 2.9 m/s): the drawn rain fell at the speed of drizzle whatever the shower, and in a few m/s of wind
        // it went sideways ("floaty"). Now the drops seen fall ~4-7 m/s (Atlas), redrawn each epoch at the rate now.
        float lam = max(pc.dropLambda, 0.5);
        Dmm = clamp(-(log(max(1.0 - h1.y, 1e-6)) + log(max(1.0 - h1.z, 1e-6)) + log(max(1.0 - h2.x, 1e-6))) / lam, 0.5, 6.0);
        v   = max(9.65 - 10.3 * exp(-0.6 * Dmm), 1.0);
    }
    // The fall and the wind are integrated over sim time on the CPU (updateRainMotion): a drop's fall offset is its
    // terminal speed x the fall phase, quantised to 1/64 m/s so the phase can be handed over wrapped (q x (G mod L)).
    float qv   = max(floor(v * 64.0 + 0.5), 1.0);
    bool  snowW = flake && !dust;
    vec2  fallM = flake ? cv2.snowMotion.xy : cv2.rainMotion.xy;  // ice (flakes, diamond dust) has its own slower fall
    float vq   = qv / 64.0 * fallM.y;                            // its fall speed now (x "Rain fall speed")
    float fall = fract(qv * mod(fallM.x, L) / L) * L;
    vec2  drift = mod(snowW ? cv2.rainWind.zw : cv2.rainMotion.zw, L);
    vec2  windNow = cv2.rainWind.xy * (snowW ? cv2.precip.z : 1.0);
    vec2  flut = vec2(0.0), flutV = vec2(0.0);
    if (flake && !dust) {
        float o1 = 6.2831853 * floor(300.0 + 400.0 * h2.x) / 600.0;
        float o2 = 6.2831853 * floor(300.0 + 400.0 * h2.y) / 600.0;
        float a  = (0.03 + 0.06 * h2.z) * min(sqrt(max(cv2.precip.z, 1.0)), 1.5);
        flut  = a * vec2(sin(o1 * t + 6.2831853 * h0.x), cos(o2 * t + 6.2831853 * h0.y));
        flutV = a * vec2(o1 * cos(o1 * t + 6.2831853 * h0.x), -o2 * sin(o2 * t + 6.2831853 * h0.y));
    }
    vec3 eyeR = vec3(cv2.rainE.w, cv2.rainN.w, cv2.rainU.w);
    vec3 pos  = h0 * L + vec3(drift + flut, -fall);
    vec3 rel  = mod(pos - mod(eyeR, L) + 0.5 * L, L) - 0.5 * L;
    float r   = length(rel);

    // This level's share of the distance (cross-faded with the next), and none right at the eye.
    float wS = (k == 0 ? 1.0 : smoothstep(0.2 * L, 0.25 * L, r)) * (1.0 - smoothstep(0.4 * L, 0.5 * L, r))
             * smoothstep(0.08, 0.25, r);
    if (wS <= 0.0) { cullDrop(); return; }

    // ── Does it exist: the rain rate where it is ──
    float rate;
    if (dust) rate = clamp(uintBitsToFloat(cv2EyeIceS) / 5e-5, 0.0, 1.0);
    else      rate = cv2RainMapAt(rel.xy) * cv2RainBurst(t);
    float f    = clamp(sqrt(rate / 0.5), 0.0, 1.0);          // the share of particles drawn
    float pres = clamp((f - h2.z) / 0.05, 0.0, 1.0);          // fades in / out as the rate changes (no pops)
    if (pres <= 0.0) { cullDrop(); return; }

    // The real drops' geometric cross-section per m^3 (m^2 / m^3). Rain: Marshall-Palmer (N0 8000 / m^3 / mm) at
    // R = 60 rate^1.5 mm/h (a light shower 0.2 ~ 5 mm/h, a Cb core 1 ~ 60). Snow: ~4x (bigger, slower aggregates).
    // Diamond dust: half the ice fog's extinction.
    float cs;
    if (dust) cs = 0.5 * uintBitsToFloat(cv2EyeIceS);
    else {
        float R   = 60.0 * pow(max(rate, 1e-4), 1.5);
        float lam = 4.1 * pow(R, -0.21);
        cs = 8000.0 * 1.5707963 / (lam * lam * lam) * 1e-6 * (flake ? 4.0 : 1.0);
    }
    float nP   = max(f, 0.05) * float(N0 << uint(k)) / (L * L * L);   // particles per m^3 at this level
    float Aeff = cs / nP;                                             // m^2 each particle carries

    // ── On screen ──
    vec3 eZ = normalize(pc.obsECEFDir.xyz);
    vec3 eX = normalize(cross(vec3(0.0, 0.0, 1.0), eZ));
    vec3 eY = cross(eZ, eX);
    vec3 p    = rainToEnu(rel, eX, eY, eZ);
    vec3 Vw   = rainToEnu(vec3(windNow + flutV, -vq), eX, eY, eZ);
    vec3 Vrel = Vw - pc.eyeVel.xyz;
    vec3 c1 = (pc.skyView * vec4(p, 0.0)).xyz;                        // now
    // Flakes take a third of the shutter (2026-10-23): the eye follows a flake and sees a flake, not a streak; at the
    // full shutter a flake in a few m/s of wind drew as a horizontal dash, the "rain blowing sideways" look.
    float shut = pc.eyeVel.w * (snowW ? 0.35 : 1.0);
    vec3 c0 = (pc.skyView * vec4(p - Vrel * shut, 0.0)).xyz;  // the shutter's opening
    const float zN = 0.05;
    if (c0.z > -zN && c1.z > -zN) { cullDrop(); return; }
    if (c0.z > -zN) c0 = mix(c1, c0, (c1.z + zN) / (c1.z - c0.z));
    if (c1.z > -zN) c1 = mix(c0, c1, (c0.z + zN) / (c0.z - c1.z));
    float tanH = tan(pc.fovYRad * 0.5);
    vec2  scr  = vec2(pc.screenW, pc.screenH);
    float aspect = pc.screenW / pc.screenH;
    vec2  P0 = vec2(c0.x / (-c0.z) / (tanH * aspect), -c0.y / (-c0.z) / tanH) * 0.5 * scr + 0.5 * scr;
    vec2  P1 = vec2(c1.x / (-c1.z) / (tanH * aspect), -c1.y / (-c1.z) / tanH) * 0.5 * scr + 0.5 * scr;
    float pixAng = 2.0 * tanH / pc.screenH;
    float mpp  = r * pixAng;                                          // metres per pixel at the drop
    float dpx  = Dmm * 1e-3 / mpp;                                    // its real width on screen
    float Apx  = Aeff / (mpp * mpp);                                  // the coverage it carries, px^2
    float Lpx  = length(P1 - P0);
    float wD   = max(dpx, 1.0);
    float a    = Apx / (max(Lpx, wD) * wD);
    // Where one particle would stand for so many drops that it draws as a bright dot (far away, heavy rain), it
    // fades out and the rain volume carries that distance alone: real rain there is a haze, not grains.
    float grain = 1.0 - smoothstep(0.12, 0.35, a);
    if (a > 0.8) { wD *= a / 0.8; a = 0.8; }                          // more than a line can hold: wider, not opaque
    a *= wS * pres * grain * pc.params.z * wEp;
    a  = min(a, 1.0);
    if (a < 2e-4) { cullDrop(); return; }

    // ── Its light: the rain sample at the eye, through the drop's phase toward the eye ──
    vec3  vdir = normalize(p);
    float og   = cv2.rain.y;
    // A plate's normal (flakes: near level, tilted up to ~12 deg, turning as they flutter; diamond dust: every
    // orientation, tumbling) for its glint.
    vec3  nPl;
    if (dust) {
        vec3  n0 = h1 * 2.0 - 1.0;
        float tp = t * (0.5 + 1.5 * h2.x) + 6.2831853 * h2.y;
        nPl = normalize(vec3(n0.x * cos(tp) - n0.y * sin(tp), n0.x * sin(tp) + n0.y * cos(tp), n0.z) + 1e-4);
    } else {
        float tp = t * (0.7 + 1.3 * h2.x) + 6.2831853 * h2.y;
        nPl = normalize(vec3(0.21 * h1.z * vec2(cos(tp), sin(tp)), 1.0));
    }
    uint kind = dust ? 2u : (flake ? 1u : 0u);
    vec3 Lr = cv2RainKey.xyz * dropPhase(kind, vdir, cv2RainKeyDir.xyz, nPl, og) + (dust ? vec3(0.0) : cv2RainAmb.xyz);
    // The Reflect beams' light at the eye (2026-10-06; CPU, from the cloud-light list): standing in a beam, its drops
    // show the bow opposite the satellites, its flakes and ice crystals glint and halo around them.
    if (cv2.beamEye.x > 0.0)
        Lr += (cv2.beamEye.x * cv2.beamEye.y * SUN_INTENSITY) * vec3(1.0, 0.97, 0.92)
            * dropPhase(kind, vdir, cv2.beamEyeDir.xyz, nPl, og);

    // ── The quad: the streak from P0 to P1, widened by the drawn width and a pixel of antialiasing ──
    vec2  ax = (Lpx > 1e-3) ? (P1 - P0) / Lpx : vec2(1.0, 0.0);
    vec2  ay = vec2(-ax.y, ax.x);
    float hw = 0.5 * wD + 1.0;
    int   vi = gl_VertexIndex;
    float u  = (vi & 1) != 0 ? Lpx + hw : -hw;
    float w  = (vi & 2) != 0 ? hw : -hw;
    vec2  px = P0 + ax * u + ay * w;
    gl_Position = vec4(px / scr * 2.0 - 1.0, sceneDepthFromDistance(r), 1.0);
    vLocal = vec2(u, w);
    // Dust occludes nothing it is drawn for (only its glint shows).
    vColor = vec4(Lr * pc.params.w * a, dust ? 0.0 : a);
    vSeg   = vec3(Lpx, wD, r);
}
