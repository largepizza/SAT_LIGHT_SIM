#version 450
// ── glare.vert — per-satellite glare sprites (2026-09-24) ─────────────────────────────────────
// The bloom (flare_source → flare_blur → flare_composite) is a broad, quarter-resolution glow: right
// for a crowd of satellites, but a single bright glint came out as a soft, six-pointed white blob.
// This pass draws, at FULL resolution over the composite, a glare sprite for each satellite bright
// enough to bloom at all: a sharp core, a tight halo and thin spikes (include/glare.glsl). Same
// compact visible list, projection and occlusion tests as flare_source.*;
// occlusion is tested at the SOURCE's screen position (a sprite pixel far from it says nothing about
// the source's visibility), in the fragment stage, where those textures' barriers already point.

struct SatVisible { // GpuSatVisible (SatelliteSim.h)
    vec3  skyDir;
    float flareIntensity;
    uint  color;
    float angularSize;
    float rangeM;
    float glareFlare; // sat_flare.comp: a point-like mesh's glare (its sprite's effectFlare x glare keep)
};
layout(set = 0, binding = 1) readonly buffer SatVisibleBuf { SatVisible satellites[]; };

#include "glare.glsl"

layout(location = 0) out vec3  gColor;
layout(location = 1) out float gStrength; // 0..1+: how far past the threshold
layout(location = 2) out float gRadiusPx;
layout(location = 4) flat out vec3 gSrc;  // xy = the source's screen uv, z = its range (m)

void cull()
{
    gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
    gl_PointSize = 1.0;
    gColor = vec3(0.0);
    gStrength = 0.0;
    gRadiusPx = 0.0;
    gSrc = vec3(0.0);
}

void main()
{
    SatVisible sat = satellites[gl_VertexIndex];
    // The bloom's own response (flare_source.frag): 0 below effectFlare 1 (~ mag 0.8), up to 4.
    // A satellite handing over to its mesh keeps its glare here, at its centre, while the mesh is
    // still point-like (the record's last field), after its point and bloom have gone.
    float b = clamp(log2(max(max(sat.flareIntensity, sat.glareFlare), 1.0)) * 0.5, 0.0, 4.0);
    float s = b - gpc.threshold;
    vec3 cam = (gpc.skyView * vec4(sat.skyDir, 0.0)).xyz;
    if (s <= 0.0 || cam.z >= -0.001 || gpc.gain <= 0.0) { cull(); return; }

    float tanHalfFov = tan(gpc.fovYRad * 0.5);
    vec2  ndc = vec2(cam.x / (-cam.z) / (tanHalfFov * gpc.aspect), -cam.y / (-cam.z) / tanHalfFov);
    if (any(greaterThan(abs(ndc), vec2(1.3)))) { cull(); return; }

    float radius = gpc.sizePx * (0.6 + s) * glareNearScale(sat.rangeM); // a near source glares wider
    gl_Position  = vec4(ndc, 0.5, 1.0);
    gl_PointSize = min(2.0 * radius, gpc.maxPointSize);
    gColor    = unpackUnorm4x8(sat.color).rgb;
    gStrength = s * gpc.gain;
    gRadiusPx = gl_PointSize * 0.5;
    gSrc  = vec3(ndc * 0.5 + 0.5, sat.rangeM > 0.0 ? sat.rangeM : 5.0e29);
}
