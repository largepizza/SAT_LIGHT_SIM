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
layout(set = 0, binding = 6) uniform sampler2D splatTex;

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

void main()
{
    vec2 uv = gl_FragCoord.xy / pc.screen.xy;
    vec3 rgb = texture(splatTex, uv).rgb;
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
    // a soft shoulder (x / (1 + x)) on the luminance: the head end rolls off instead of saturating
    const float kShoulder = 0.9;
    float disp = kShoulder * x / (kShoulder + x);
    outColor = vec4((rgb / lum) * disp * cloudVis, 0.0);
}
