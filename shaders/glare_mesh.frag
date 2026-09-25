#version 450
// ── glare_mesh.frag — the glare of a satellite MESH's glint (see glare_mesh.vert) ─────────────
// include/glare.glsl's pattern, as a sprite's glare (glare.frag). No occlusion test: the glint was
// found in the mesh's own rendered light (mesh_bloom.frag → glare_find.comp), which the mesh bloom
// does not test against clouds either.

#include "glare.glsl"

layout(location = 0) in vec3  gColor;
layout(location = 1) in float gStrength;
layout(location = 2) in float gRadiusPx;

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 d = (gl_PointCoord - 0.5) * (2.0 * gRadiusPx);
    if (gStrength <= 0.0 || dot(d, d) >= gRadiusPx * gRadiusPx) discard;
    vec2 g = glareProfile(d, gRadiusPx);
    vec3 col = mix(gColor, vec3(1.0), 0.3) * g.x + mix(gColor, vec3(1.0), 0.7) * g.y;
    outColor = vec4(col * gStrength, 1.0);
}
