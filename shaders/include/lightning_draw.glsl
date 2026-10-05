#ifndef SATLIGHTSIM_LIGHTNING_DRAW_GLSL
#define SATLIGHTSIM_LIGHTNING_DRAW_GLSL
// Lightning channels and glow (lightning.vert / .frag, SatelliteSimLightning.cpp, 2026-10-05).
layout(push_constant) uniform BoltPC {
    mat4  skyView;
    float fovYRad, aspect, screenW, screenH;
    vec4  params;    // x the sky's exposure, y "Lightning bolts", z "Lightning glow", w mode (0 channels, 1 glow)
    vec4  params2;   // x highlight roll-off
    vec4  spare;     // x the eye's altitude (m)
} pc;

struct BoltFlash {     // == GpuBoltFlash (SatelliteSim.h)
    vec4 o;            // xyz origin (ENU about the eye, m), w intensity now
    vec4 u;            // xyz up, w age (s)
    vec4 t1;           // xyz, w kind: 0 in cloud, 1 to ground, 2 red sprite, 3 spider under the base
    vec4 t2;           // xyz, w the tree's longest arc (m)
    vec4 m;            // x revealed arc (m), y the branches' share, z 1 during the stepped leader
    vec4 e[4];         // glow emitters: xyz (ENU about the eye), w weight
};
struct BoltSeg { vec3 p0; float w; vec3 p1; uint packed; };
layout(std430, set = 0, binding = 0) readonly buffer BoltFlashBuf { BoltFlash bFlash[]; };
layout(std430, set = 0, binding = 1) readonly buffer BoltSegBuf   { BoltSeg   bSeg[]; };
layout(set = 0, binding = 2) uniform sampler2D cloudTargetA;   // a: signed cloud distance (km, cloud_occlusion.glsl)
layout(set = 0, binding = 3) uniform sampler2D cloudTargetB;   // rgb: cloud transmittance

// The air between the eye and a point at `range` (ENU about the eye): the transmittance of its column of air
// (8 km scale height, ~0.15 optical depth per scale height in the visible), so a flash seen from orbit crosses only
// the air below it. A flat 90 km haze length dimmed everything seen from 350 km to ~5%.
float boltAir(vec3 p, float range)
{
    const float H = 8000.0;
    float hE = max(pc.spare.x, 0.0);
    float hP = max(length(p + vec3(0.0, 0.0, 6371000.0 + hE)) - 6371000.0, 0.0);
    float c  = abs(hP - hE) / max(range, 1.0);
    float col = (c > 0.02) ? H * abs(exp(-hE / H) - exp(-hP / H)) / c
                           : range * exp(-min(hE, hP) / H);
    return exp(-0.15 * min(col, range * exp(-min(hE, hP) / H)) / H);
}

// The light of a flash leaving the cloud around it, per unit intensity, at distance r: diffusion through the cloud
// (a broad core ~4 km: from orbit a flash lights the tops over 10-20 km, as in the ISS photographs) and a faint
// long tail (the light reaching the cloud and rain around it).
// r is floored at ~3.5 km (the light has diffused through at least that much cloud wherever it leaves it: without
// it a cloud surface right at the flash drew a hard point, a lamp rather than lightning), and the tail is windowed to
// 0 by kBoltGlowR, inside the quad lightning.vert draws (its edges showed as squares from orbit).
const float kBoltGlowR = 45000.0;
float boltGlow(float r)
{
    float q2 = (r * r + 1.2e7) / 1.6e7;
    return (pow(1.0 + q2, -1.5) + 0.04 / (1.0 + r * r / 2.25e8)) * (1.0 - smoothstep(0.45 * kBoltGlowR, 0.8 * kBoltGlowR, r));
}

// The sky's tonemap (sat_sky.frag): the channels are added after it, screen-blended (lightning pipeline).
vec3 boltTonemap(vec3 x)
{
    return mix(vec3(1.0) - exp(-x), vec3(1.0) - 1.0 / (vec3(1.0) + x + 0.5 * x * x), pc.params2.x);
}
#endif
