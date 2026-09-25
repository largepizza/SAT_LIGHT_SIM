#version 450
// ── glare.frag — a bright satellite's glare, drawn at full resolution (see glare.vert) ────────
// The pattern is include/glare.glsl's (shared with the mesh glints' glare_mesh.frag). Occlusion is
// tested at the SOURCE's screen position, as flare_source.frag tests it: opaque cloud or terrain
// nearer than the satellite hide it, the cloud transmittance dims it (one value for the sprite).

#include "glare.glsl"

layout(location = 0) in vec3  gColor;
layout(location = 1) in float gStrength;
layout(location = 2) in float gRadiusPx;
layout(location = 4) flat in vec3 gSrc; // xy = the source's screen uv, z = its range (m)

layout(set = 0, binding = 5) uniform sampler2D cloudTargetA;  // a = tCloudOcclude (>= 0: >= 90% opaque)
layout(set = 0, binding = 6) uniform sampler2D cloudTargetB;  // rgb = cloud transmittance
layout(set = 0, binding = 7) uniform sampler2D sceneDepthTex; // terrain/ocean distance

layout(location = 0) out vec4 outColor;

void main()
{
    vec2 d = (gl_PointCoord - 0.5) * (2.0 * gRadiusPx); // pixels from the source
    if (gStrength <= 0.0 || dot(d, d) >= gRadiusPx * gRadiusPx) discard;

    vec4  cA = textureLod(cloudTargetA, gSrc.xy, 0.0);
    vec4  cB = textureLod(cloudTargetB, gSrc.xy, 0.0);
    float vis = (cA.a >= 0.0 && cA.a < gSrc.z) ? 0.0 : clamp(dot(cB.rgb, vec3(1.0 / 3.0)), 0.0, 1.0);
    if (textureLod(sceneDepthTex, gSrc.xy, 0.0).r < gSrc.z) vis = 0.0;
    if (vis <= 0.001) discard;

    vec2 g = glareProfile(d, gRadiusPx);
    // The glint's tint, whitening toward the centre as a bright source reads.
    vec3 col = mix(gColor, vec3(1.0), 0.3) * g.x + mix(gColor, vec3(1.0), 0.7) * g.y;
    outColor = vec4(col * (gStrength * vis), 1.0);
}
