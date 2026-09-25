#version 450
// ── glare.vert — per-satellite glare sprites (2026-09-24) ─────────────────────────────────────
// The bloom (flare_source → flare_blur → flare_composite) is a broad, quarter-resolution glow: right
// for a crowd of satellites, but a single bright glint came out as a soft, six-pointed white blob.
// This pass draws, at FULL resolution over the composite, a glare sprite for each satellite bright
// enough to bloom at all: a sharp core, many thin rays with an angular noise of their own (the
// coronal spikes of the sun's lensFlare() in sat_sky.frag), and for the brightest a thin horizontal
// anamorphic streak. Same compact visible list, projection and occlusion tests as flare_source.*;
// occlusion is tested at the SOURCE's screen position (a sprite pixel far from it says nothing about
// the source's visibility), in the fragment stage, where those textures' barriers already point.

struct SatVisible { // GpuSatVisible (SatelliteSim.h)
    vec3  skyDir;
    float flareIntensity;
    uint  color;
    float angularSize;
    float rangeM;
    float meshPx;
};
layout(set = 0, binding = 1) readonly buffer SatVisibleBuf { SatVisible satellites[]; };

layout(push_constant) uniform PC {
    mat4  skyView;
    float fovYRad;
    float aspect;
    float gain;         // glare brightness (Settings: "Glare gain") x the night eye-adaptation
    float sizePx;       // sprite radius per unit of the bloom's log response
    vec2  screenSizePx; // the main view
    float maxPointSize; // device limit
    float threshold;    // the log response a glint needs before it glares
} pc;

layout(location = 0) out vec3  gColor;
layout(location = 1) out float gStrength; // 0..1+: how far past the threshold
layout(location = 2) out float gRadiusPx;
layout(location = 3) out float gSeed;     // the ray pattern
layout(location = 4) flat out vec3 gSrc;  // xy = the source's screen uv, z = its range (m)

void cull()
{
    gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
    gl_PointSize = 1.0;
    gColor = vec3(0.0);
    gStrength = 0.0;
    gRadiusPx = 0.0;
    gSeed = 0.0;
    gSrc = vec3(0.0);
}

void main()
{
    SatVisible sat = satellites[gl_VertexIndex];
    // The bloom's own response (flare_source.frag): 0 below effectFlare 1 (~ mag 0.8), up to 4.
    float b = clamp(log2(max(sat.flareIntensity, 1.0)) * 0.5, 0.0, 4.0);
    float s = b - pc.threshold;
    vec3 cam = (pc.skyView * vec4(sat.skyDir, 0.0)).xyz;
    if (s <= 0.0 || cam.z >= -0.001 || pc.gain <= 0.0) { cull(); return; }

    float tanHalfFov = tan(pc.fovYRad * 0.5);
    vec2  ndc = vec2(cam.x / (-cam.z) / (tanHalfFov * pc.aspect), -cam.y / (-cam.z) / tanHalfFov);
    if (any(greaterThan(abs(ndc), vec2(1.3)))) { cull(); return; }

    float radius = pc.sizePx * (0.6 + s);
    gl_Position  = vec4(ndc, 0.5, 1.0);
    gl_PointSize = min(2.0 * radius, pc.maxPointSize);
    gColor    = unpackUnorm4x8(sat.color).rgb;
    gStrength = s * pc.gain;
    gRadiusPx = gl_PointSize * 0.5;
    // One ray pattern for every source, as a camera's aperture gives every point light the same
    // spikes (a per-satellite hash of its moving direction would re-roll the rays every frame).
    gSeed = 0.37;
    gSrc  = vec3(ndc * 0.5 + 0.5, sat.rangeM > 0.0 ? sat.rangeM : 5.0e29);
}
