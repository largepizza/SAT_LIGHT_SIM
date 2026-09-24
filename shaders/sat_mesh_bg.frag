#version 450
// Model viewer background: the camera's own ray into earth_env.glsl (the Earth below, atmosphere,
// clouds) plus the sun's disc, through the same exposure as the mesh.

#include "common.glsl"
#include "terrain.glsl"
#include "sat_mesh_common.glsl"
#include "earth_env.glsl"

layout(location = 0) in vec2 vNdc;
layout(location = 0) out vec4 outColor;

void main()
{
    vec4 far = frame.invViewProj * vec4(vNdc, 1.0, 1.0);
    vec3 dir = normalize(far.xyz / far.w - frame.camPos.xyz);
    vec3 ro  = frame.camPos.xyz - frame.earthCenter.xyz;
    vec3 L   = earthEnv(ro, dir, frame.sunDir.xyz, 0.0, frame.earthCenter.w);

    // Sun disc (0.27° radius, softened), only where the Earth does not hide it.
    if (frame.sunDir.w > 0.5 && raySphere(ro, dir, R_EARTH).x < 0.0) {
        float c = dot(dir, frame.sunDir.xyz);
        L += vec3(1.0, 0.96, 0.88) * 40.0 * smoothstep(0.99998, 0.999992, c);
        L += vec3(1.0, 0.94, 0.82) * 0.08 * pow(max(c, 0.0), 800.0);
    }
    outColor = vec4(vec3(1.0) - exp(-frame.camPos.w * L), 1.0);
}
