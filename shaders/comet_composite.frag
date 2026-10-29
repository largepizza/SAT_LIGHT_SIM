#version 450
// ── comet_composite.frag — comet tails + comae onto the sky (2026-10-28; SatelliteSimComets.cpp, CLAUDE.md "Comets") ──
// The tail particles' light (the half-res splat target: surface brightness in the Milky Way's anchor units, lum 0.05
// = 21 mag/arcsec^2, linear in the physical surface brightness) plus each coma (a Haser-like column, 1/rho x
// exp(-rho / rho_s), softened inside the point's ~1 arcmin, round on the sky, no edge), then as the Milky Way: the line
// of sight's extinction, the dark-sky gate against the sky's own surface brightness in this direction (darksky.glsl:
// city glow, twilight, daylight with its aureole round the Sun, moonlight), the global exposure, cloud occlusion and a
// soft shoulder on the luminance (hue kept) so a bright head region is never a white blob. Added post-tonemap.

#extension GL_EXT_control_flow_attributes : require
#include "common.glsl"
#define CLOUD_PARAMS_BINDING 4
#include "cloud_params.glsl"
layout(std430, set = 0, binding = 5) readonly buffer LightDomeBuf {
    float lightDome[16];
    float lightDomeEased[16];
    vec4  skyglowInfo;         // x = the skyglow model's zenith ratio (artificial / natural), eased x gain
};
#include "atmosphere.glsl"
#include "darksky.glsl"
#include "cloud_occlusion.glsl"
#include "comet_frame.glsl"

layout(set = 0, binding = 1) uniform sampler2D cloudTargetA;
layout(set = 0, binding = 2) uniform sampler2D cloudTargetB;
layout(set = 0, binding = 3) uniform sampler2D sceneDepthTex;
layout(set = 0, binding = 6) uniform sampler2D splatTex;   // the splat atlas: the particles' light (6 levels)
layout(set = 0, binding = 7) uniform sampler2D fieldTex;   // the dust's light-weighted (te, ln beta) field, same atlas

layout(push_constant) uniform PC {
    vec4 sunDirENU;   // xyz, w = sin(elevation)
    vec4 obs;         // x observer radius (m), y extinction k, z atmFrac, w moonBright
    vec4 misc;        // x display gain, y manual terrain test, z the Sun above the apparent horizon, w glare gate
    vec4 screen;      // xy full-res size
} pc;

layout(location = 0) in vec2 vUV;
layout(location = 0) out vec4 outColor;

// The sky's surface brightness (mag/arcsec^2) in direction d: city glow + twilight + daylight (+ its aureole round the
// Sun) + moonlight, added as light; the glare round the Sun from anywhere it is above the horizon.
float cometSkyMag(vec3 d)
{
    const float kM = 0.4 * 2.302585093;
    float L = exp(-kM * darkSkyPollutionMagPhys(d));
    float atm = pc.obs.z;
    L += atm * exp(-kM * darkSkyTwilightMag(d, pc.sunDirENU.xyz, pc.sunDirENU.w));
    float cosS = clamp(dot(d, pc.sunDirENU.xyz), -1.0, 1.0);
    float thDeg = degrees(acos(cosS));
    if (pc.sunDirENU.w > -0.02) {
        float s = smoothstep(-0.02, 0.5, pc.sunDirENU.w);
        float mDay = 11.0 - 7.0 * sqrt(s);
        float aur = 1.0 + 9.0 / max(thDeg * thDeg, 0.09);
        L += atm * exp(-kM * mDay) * aur;
    }
    if (pc.misc.z > 0.5)
        L += exp(-kM * 9.0) * 4.0 / max(thDeg * thDeg, 0.09);
    L += atm * pc.obs.w * exp(-kM * 18.0);
    return -(1.0 / kM) * log(max(L, 1e-30));
}

uint hashU(uint x)
{
    x ^= x >> 16; x *= 0x7feb352du;
    x ^= x >> 15; x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}
// A smooth value noise in [-1, 1] (hashed knots, smoothstep blend), mean 0, sharpened into bands.
float band(float x, uint seed)
{
    float fl = floor(x);
    float f = x - fl;
    uint i = uint(int(fl) + 1048576);
    float a = float(hashU(i * 0x9E3779B1u ^ seed) >> 8) * (1.0 / 16777216.0);
    float b = float(hashU((i + 1u) * 0x9E3779B1u ^ seed) >> 8) * (1.0 / 16777216.0);
    float n = mix(a, b, f * f * (3.0 - 2.0 * f)) * 2.0 - 1.0;
    return clamp(1.7 * n, -1.0, 1.0);
}
// An octave of period P (in the coordinate x) shows once its bands are a few screen pixels apart (g = |dx/dpx|).
float octave(float x, float g, float sd, float P, float c, uint seed)
{
    // ... and once the field's own spread here (sd) is well under its period
    float f = smoothstep(4.0, 10.0, P / max(g, 1e-30)) * (1.0 - smoothstep(0.12, 0.3, sd / P));
    return f > 0.0 ? 1.0 + c * f * band(x / P, seed) : 1.0;
}

// The tail's zoom detail (2026-10-28, issue 20261028_221703), a multiplier on the dust's light with mean 1: the emission
// is time-variable (jets switching with the nucleus's rotation, outbursts) — SYNCHRONIC BANDS in the emission time te;
// fragmentation events release all sizes at once from fragments that drift off — STRIAE, bands of te + 0.7 ln beta,
// oblique to the synchrones, in old dust; size sorting — FILAMENTS along the syndynes, bands in ln beta. Each octave
// fades in as its bands become a few pixels apart, so zooming in resolves finer ones.
float tailDetail(float te, float lb, float sd, vec2 gT, vec2 gB)
{
    float nT = length(gT), nB = length(gB), nS = length(gT + 0.7 * gB);
    // te is days from now (-age): striae only in dust older than a few days (fragments need time to drift off)
    float st = smoothstep(2.0, 8.0, -te);
    float m = octave(te, nT, sd, 1.7, 0.45, 0x1234u);
    m *= octave(te, nT, sd, 0.42, 0.45, 0x2345u);
    m *= octave(te, nT, sd, 0.105, 0.5, 0x3456u);
    m *= octave(te, nT, sd, 0.026, 0.55, 0x4567u);
    m *= octave(te, nT, sd, 0.0065, 0.55, 0x4568u);
    m *= octave(lb, nB, 0.0, 0.4, 0.3, 0x5678u);
    m *= octave(lb, nB, 0.0, 0.1, 0.3, 0x6789u);
    float sc = te + 0.7 * lb;
    m *= octave(sc, nS, sd, 0.9, 0.45 * st, 0x789Au);
    m *= octave(sc, nS, sd, 0.22, 0.45 * st, 0x89ABu);
    return m;
}

void main()
{
    vec2 uv = gl_FragCoord.xy / pc.screen.xy;
    // the splat atlas's levels added back (bilinear: a coarse level holds only particles wider than its pixels)
    vec3 rgb = vec3(0.0);
    vec4 fld = vec4(0.0);
    [[dont_unroll]] for (int pass = 0; pass < 2; ++pass) {
        vec4 geo = pass == 0 ? F.pad[1] : F.pad[2]; // the colour atlas, the field atlas (half its size)
        vec2 W0 = geo.xy, AT = geo.zw;
        vec2 org = vec2(0.0);
        for (int k = 0; k < 6; ++k) {
            vec2 wh = max(floor(W0 * exp2(-float(k))), vec2(1.0));
            if (k == 1) org = vec2(W0.x, 0.0);
            vec2 auv = clamp(org + uv * wh, org + 0.5, org + wh - 0.5) / AT;
            if (pass == 0) rgb += texture(splatTex, auv).rgb; else fld += texture(fieldTex, auv);
            if (k >= 1) org.y += wh.y;
        }
    }
    // the dust's zoom detail where the field holds light (its derivatives need every pixel of the quad: no branch on it)
    float wF = fld.z;
    float te = fld.x / max(wF, 1e-30), lb = fld.y / max(wF, 1e-30);
    float sd = sqrt(max(fld.w / max(wF, 1e-30) - te * te, 0.0)); // the field's spread here (days)
    vec2 gT = vec2(dFdx(te), dFdy(te)), gB = vec2(dFdx(lb), dFdy(lb)); // (in uniform control flow)
    float mDet = 1.0;
    if (wF > 1e-12) {
        mDet = tailDetail(te, lb, sd, gT, gB);
        rgb = max(rgb + (mDet - 1.0) * wF * vec3(1.05, 0.98, 0.86), vec3(0.0));
    }
    if (F.pad[0].w > 0.5) { // debug views (comets.tail_debug): 1 the emission time (fract per day), 2 the detail's share
        float lumT = max(dot(rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-30);
        outColor = F.pad[0].w < 1.5 ? vec4(fract(te), fract(te * 4.0), wF > 1e-9 ? 0.3 : 0.0, 0.0)
                                    : vec4(clamp(wF / lumT, 0.0, 1.0), clamp(sd, 0.0, 1.0), clamp(sd * 10.0, 0.0, 1.0), 0.0);
        return;
    }
    // this pixel's direction (ENU)
    vec2 ndc = uv * 2.0 - 1.0;
    float tanH = F.proj.x, aspect = F.proj.y;
    vec3 cam = normalize(vec3(ndc.x * tanH * aspect, -ndc.y * tanH, -1.0));
    vec3 d = transpose(mat3(F.skyView)) * cam;
    // the comae
    [[dont_unroll]] for (int s = 0; s < 3; ++s) {
        vec4 c1 = F.coma[3 * s + 1];
        if (c1.w < 0.5) continue;
        vec4 c0 = F.coma[3 * s];
        float rho = 2.0 * asin(clamp(0.5 * length(d - c0.xyz), 0.0, 1.0));
        float x = rho / c0.w;
        if (x > 12.0) continue;
        float L = c1.x * exp(-x) / sqrt(rho * rho + c1.y * c1.y);
        // white in the inner (dust) coma, toward C2 green outward where the gas dominates
        vec3 green = vec3(0.62, 1.12, 0.70);
        vec3 col = mix(F.coma[3 * s + 2].rgb, mix(vec3(1.0), green, 0.7 * c1.z), smoothstep(0.15, 1.5, x));
        rgb += col * L;
    }
    float lum = dot(rgb, vec3(0.2126, 0.7152, 0.0722));
    if (lum <= 1e-7) { outColor = vec4(0.0); return; }
    float ext = pow(10.0, -0.4 * atmExtinctionMag(vec3(0.0, 0.0, pc.obs.x), d, 0.0, pc.obs.y));
    float lumObs = lum * ext;
    if (lumObs <= 1e-9) { outColor = vec4(0.0); return; }
    float objMag = 21.0 - 2.5 * log(lumObs / 0.05) / log(10.0);
    float vis = darkSkyVis(objMag, cometSkyMag(d)) * pc.misc.w;
    float cloudVis = cloudPointVisibilityAt(cloudTargetA, cloudTargetB, uv, 1.0e12, 3.0);
    if (pc.misc.y >= 0.5 && texture(sceneDepthTex, uv).r < 5.0e29) cloudVis = 0.0;
    float x = lumObs * vis * cloud.exposureScale * pc.misc.x;
    // A soft shoulder (x / (1 + x)) on the BRIGHTEST channel, the hue kept: the head end rolls off instead of
    // saturating, and a bright ion tail stays its deep CO+ blue. (On the luminance, a blue whose blue channel is 2.6x
    // its luminance clipped at the display and read as a pale sky blue: issue 20261028_221813.)
    float mx = max(rgb.r, max(rgb.g, rgb.b));
    float xm = x * mx / lum;
    const float kShoulder = 0.9;
    float disp = kShoulder * xm / (kShoulder + xm);
    outColor = vec4((rgb / mx) * disp * cloudVis, 0.0);
}
