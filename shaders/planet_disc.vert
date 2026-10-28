#version 450
// Resolved planet discs: one quad per resolved planet (include/planet_disc.glsl), projected exactly as star_point.vert
// projects a star (the camera has no roll), at kDepthFar. The quad is a few arcminutes at most, so the local coordinates
// interpolate affinely across it to float precision.
#include "depth.glsl"
#include "planet_disc.glsl"

layout(push_constant) uniform PC {   // prefix of PointDrawPC
    mat4  skyView;
    float fovYRad;
    float aspect;
    float waveTime;
    float noTwinkle;
} pc;

layout(location = 0) out vec2 vQ;            // local x, y (equatorial radii)
layout(location = 1) flat out int vDisc;

void main() {
    const vec2 k[6] = vec2[6](vec2(-1, -1), vec2(1, -1), vec2(1, 1), vec2(-1, -1), vec2(1, 1), vec2(-1, 1));
    int d = gl_InstanceIndex;
    PlanetDisc D = pd.disc[d];
    vec2 q = k[gl_VertexIndex] * D.bx.w;
    vec2 qj = q + D.misc.yz;                      // the turbulence moves the whole image
    vec3 dir = D.c.xyz + D.c.w * (qj.x * D.bx.xyz + qj.y * D.by.xyz);
    vec3 cam = (pc.skyView * vec4(dir, 0.0)).xyz;
    float th = tan(pc.fovYRad * 0.5);
    if (cam.z >= -1e-4) {
        gl_Position = vec4(0.0, 0.0, 2.0, 1.0);
    } else {
        gl_Position = vec4(cam.x / (-cam.z) / (th * pc.aspect), -cam.y / (-cam.z) / th, kDepthFar, 1.0);
    }
    vQ = q;
    vDisc = d;
}
