#version 450
// Model viewer background: the full sky renderer seen from the viewer's camera (SatEnvProbes'
// SKY_ENV target, HDR) — or, before that exists, the camera's ray into earth_env.glsl — plus the
// Sun's disc, through the same exposure as the mesh. The observer / ground-site markers are drawn
// over the mesh by sat_mesh_marker.frag (2026-09-25; they were drawn here, under it).

#include "common.glsl"
#include "terrain.glsl"
#include "sat_mesh_common.glsl"
#include "earth_env.glsl"

layout(set = 0, binding = 9) uniform sampler2D viewerBgTex; // SatEnvProbes::viewerBgView (HDR)

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 far = frame.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - frame.camPos.xyz);
    vec3 ro  = frame.camPos.xyz - frame.earthCenter.xyz;
    vec3 L;
    if (frame.bgParams.x > 0.5)
        L = texture(viewerBgTex, vNdc * 0.5 + 0.5).rgb; // the full renderer, from this camera
    else
        L = earthEnv(ro, dir, frame.sunDir.xyz, 0.0, frame.earthCenter.w);

    // Sun disc (0.27° radius, softened), only where the Earth does not hide it (the env renderer
    // leaves it out: on a mesh the GGX sun lobe is the glint).
    if (frame.sunDir.w > 0.5 && raySphere(ro, dir, R_EARTH).x < 0.0) {
        float c = dot(dir, frame.sunDir.xyz);
        L += vec3(1.0, 0.96, 0.88) * 40.0 * smoothstep(0.99998, 0.999992, c);
        L += vec3(1.0, 0.94, 0.82) * 0.08 * pow(max(c, 0.0), 800.0);
    }
    vec3 col = vec3(1.0) - exp(-frame.camPos.w * L);
    outColor = vec4(col, 1.0);
}
