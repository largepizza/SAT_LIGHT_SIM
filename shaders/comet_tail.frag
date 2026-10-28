#version 450
// ── comet_tail.frag — comet tails and comae (2026-10-27; SatelliteSimComets.cpp, CLAUDE.md "Comets") ───────────────
// The surface brightness the CPU computed per vertex (Milky Way anchor units: lum 0.05 = 21 mag/arcsec^2), shaped per
// pixel by the patch's kind — dust: striae along the emission time (McNaught's fan); ion: streamers across the ribbon;
// coma: the 1/rho profile — then, as the Milky Way: extinction, the dark-sky gate against the sky's own surface
// brightness (darksky.glsl: a faint tail fades from the outside in under twilight, the Moon or a city, and is lost in
// daylight), the global exposure, cloud occlusion (cubed like the Milky Way's, opaque cloud hides it) and a soft
// shoulder so a bright head region is never a white blob. Added post-tonemap (the main pass), at infinity.

#extension GL_EXT_control_flow_attributes : require
#include "common.glsl"
#define CLOUD_PARAMS_BINDING 4
#include "cloud_params.glsl"
layout(std430, set = 0, binding = 5) readonly buffer LightDomeBuf {
    float lightDome[16];
    float lightDomeEased[16];
};
#include "atmosphere.glsl"
#include "darksky.glsl"
#include "cloud_occlusion.glsl"

layout(set = 0, binding = 1) uniform sampler2D cloudTargetA;
layout(set = 0, binding = 2) uniform sampler2D cloudTargetB;
layout(set = 0, binding = 3) uniform sampler2D sceneDepthTex;

layout(push_constant) uniform PC {
    mat4  skyView;
    float fovYRad, aspect, screenW, screenH;
    vec4  sunDirENU;
    vec4  obs;
    vec4  misc;       // x display gain, y manual terrain test, z the Sun up, w glare gate
} pc;

layout(location = 0) in vec3  vUV;
layout(location = 1) in float vLum;
layout(location = 2) in float vSkyMag;
layout(location = 3) in float vExt;
layout(location = 4) flat in uint vKind;
layout(location = 5) flat in vec4 vCol;
layout(location = 0) out vec4 outColor;

float hash11(float x) { return fract(sin(x * 127.1 + 311.7) * 43758.5453); }
float vnoise(float x)
{
    float i = floor(x), f = fract(x);
    f = f * f * (3.0 - 2.0 * f);
    return mix(hash11(i), hash11(i + 1.0), f);
}

void main()
{
    float lum = vLum;
    vec3 col = vCol.rgb;
    if (vKind == 0u) {
        // Dust: striae — narrow bands of grains released together (synchrone-like, slightly tilted across the sizes),
        // tied to the emission time so they stay put on the tail as it evolves. Mean ~1: they redistribute the light.
        // (none in the young dust near the head: striae form as grains fragment, days after their release)
        float s = vUV.x * 1.0 + (vUV.y - 0.5) * 0.35;
        float n = 0.55 * vnoise(s * 1.9) + 0.30 * vnoise(s * 5.3 + 17.0) + 0.15 * vnoise(s * 13.1 + 41.0);
        float bands = pow(clamp(n * 1.35 - 0.15, 0.0, 1.0), 2.2) * 2.6;
        float k = clamp(vCol.w, 0.0, 1.0) * smoothstep(0.7, 3.0, vUV.z);
        lum *= mix(1.0, 0.3 + bands, k);
    } else if (vKind == 1u) {
        // Ion tail: a Gaussian across (v = +-1 is +-2.2 sigma) with a few streamers, kinked along the tail, drifting
        // with time (vCol.w).
        float v = vUV.y, s = vUV.x;
        float g = exp(-0.5 * (2.2 * v) * (2.2 * v));
        float rays = 0.0;
        for (int k = 0; k < 4; ++k) {
            float fk = float(k);
            float c = 0.55 * sin(fk * 2.1 + vCol.w * (0.7 + 0.3 * fk)) + 0.18 * sin(s * (4.0 + fk) + vCol.w * 1.3 + fk);
            float wdt = 0.06 + 0.10 * s;
            rays += exp(-((v - c) * (v - c)) / (wdt * wdt));
        }
        lum *= g * (0.55 + 0.45 * rays);
    } else {
        // Coma: 1/rho to its edge (the point carries the inside of rho0 = ~1 arcmin), greener outward (C2).
        float rho = length(vUV);
        float prof = rho < 1.0 ? (1.0 - rho * rho) * (1.0 - rho * rho) / max(rho, vCol.w) : 0.0;
        lum *= prof;
        col = mix(vec3(1.0, 0.98, 0.93), col, smoothstep(0.0, 0.35, rho));
    }
    float lumObs = lum * vExt;
    if (lumObs <= 1e-9) { outColor = vec4(0.0); return; }
    float objMag = 21.0 - 2.5 * log(lumObs / 0.05) / log(10.0);
    float vis = darkSkyVis(objMag, vSkyMag) * pc.misc.w;
    vec2 uv = gl_FragCoord.xy / vec2(pc.screenW, pc.screenH);
    float cloudVis = cloudPointVisibilityAt(cloudTargetA, cloudTargetB, uv, 1.0e12, 3.0);
    if (pc.misc.y >= 0.5 && texture(sceneDepthTex, uv).r < 5.0e29) cloudVis = 0.0;
    float x = lumObs * vis * cloud.exposureScale * pc.misc.x;
    // A soft shoulder (x / (1 + x)): the 1/rho coma and the head end of the tail roll off over a wide range instead of
    // saturating into a flat disc.
    const float kShoulder = 0.9;
    float disp = kShoulder * x / (kShoulder + x);
    outColor = vec4(col * disp * cloudVis, 0.0);
}
