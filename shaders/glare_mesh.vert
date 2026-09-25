#version 450
// ── glare_mesh.vert — glare sprites on satellite MESH glints (2026-09-24) ─────────────────────
// One point per glint glare_find.comp listed (drawn indirect on the list's own count), sized and
// weighted exactly as glare.vert does a sprite of the same effectFlare, so a satellite keeps its glare
// across the sprite → mesh hand-off, and on a large model the glare sits on the glint that makes it.

#include "glare.glsl"
#define GLINT_LIST_BINDING 0
#define GLINT_LIST_ACCESS readonly
#include "glint_list.glsl"

layout(location = 0) out vec3  gColor;
layout(location = 1) out float gStrength;
layout(location = 2) out float gRadiusPx;

void main()
{
    vec4 p = glints.glintPos[gl_VertexIndex];
    float b = clamp(log2(max(p.z, 1.0)) * 0.5, 0.0, 4.0);
    float s = b - gpc.threshold;
    if (s <= 0.0 || gpc.gain <= 0.0) {
        gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
        gl_PointSize = 1.0;
        gColor = vec3(0.0);
        gStrength = 0.0;
        gRadiusPx = 0.0;
        return;
    }
    float radius = gpc.sizePx * (0.6 + s);
    gl_Position  = vec4(p.xy * 2.0 - 1.0, 0.5, 1.0);
    gl_PointSize = min(2.0 * radius, gpc.maxPointSize);
    gColor    = glints.glintColor[gl_VertexIndex].rgb;
    gStrength = s * gpc.gain;
    gRadiusPx = gl_PointSize * 0.5;
}
