#version 450
// Rain particles: one streak (rain_particles.vert). Coverage of a capsule of the drawn width, antialiased over a
// pixel; the drop's light through the sky's tonemap (it is drawn after it), blended over the frame by its alpha:
// premultiplied, ONE / ONE_MINUS_SRC_ALPHA — a drop dimmer than the sky behind it darkens it, as a real one does.
#define CLOUD_PARAMS_BINDING 0
#include "cloud_params.glsl"
#include "rain_particles.glsl"

layout(set = 0, binding = 3) uniform sampler2D sceneDepthTex;   // half-res, metres (the manual test)

layout(location = 0) in vec2  vLocal;
layout(location = 1) flat in vec4 vColor;
layout(location = 2) flat in vec3 vSeg;
layout(location = 0) out vec4 outColor;

void main()
{
    float s    = clamp(vLocal.x, 0.0, vSeg.x);
    float dist = length(vec2(vLocal.x - s, vLocal.y));
    float cov  = clamp(0.5 * vSeg.y + 0.5 - dist, 0.0, 1.0);
    if (cov <= 0.0) discard;
    // Without the hardware depth (render scale < 1 with the sky TAA off) test the half-res scene depth.
    if ((uint(pc.obsECEFDir.w + 0.5) & 2u) != 0u
        && texture(sceneDepthTex, gl_FragCoord.xy / vec2(pc.screenW, pc.screenH)).r < vSeg.z) discard;
    vec3 x = vColor.rgb * cov;
    vec3 y = mix(vec3(1.0) - exp(-x), vec3(1.0) - 1.0 / (vec3(1.0) + x + 0.5 * x * x), cloud.highlightRolloff);
    outColor = vec4(y, vColor.a * cov);
}
