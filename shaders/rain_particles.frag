#version 450
// Rain particles: one streak (rain_particles.vert). Coverage of a capsule of the drawn width, antialiased over a
// pixel; the drop's light through the sky's tonemap (it is drawn after it), blended over the frame by its alpha:
// premultiplied, ONE / ONE_MINUS_SRC_ALPHA.
//
// THE LIGHT OF THE SCENE IT IS IN FRONT OF (2026-10-31, issue_20261030_204744 "Snow in shaded conditions is
// black"): a particle's light is ONE analytic sample for the whole field (cv2RainKey / cv2RainAmb, the march's
// rain sample at the eye), so it does not know the radiance of the scene behind it. Under a thick deck the
// ambient collapses (the clear sky's zenith x the column's diffuse transmission: 0.05 measured at the report's
// view) while the cloud base the same pass draws is ~0.7, and the over-blend then SUBTRACTS: every flake was
// darker than everything behind it, near-black against sunlit cloud. A flake is translucent and nearly white,
// so it scatters the light of the scene it sits in and can never be much darker than that scene: the
// particle's contribution is floored at kCv2PrecipFloor x its alpha share of the resolved background
// (sky_taa.comp's frame, one fetch per precipitation pixel), which makes the composite
// bg x (1 + (kCv2PrecipFloor - 1) * alpha): never darker than the background, and brighter than it by a
// fraction that follows the background's own radiance and hue. Where the particle's own light is already the
// brighter one (rain against dark cloud or sea) nothing changes.
#define CLOUD_PARAMS_BINDING 0
#include "cloud_params.glsl"
#include "rain_particles.glsl"

layout(set = 0, binding = 3) uniform sampler2D sceneDepthTex;   // half-res, metres (the manual test)
// The frame the particles are drawn over, 1:1 with gl_FragCoord: skyTaaHistColorView[0] and [1] (the resolve's
// ping-pong; the vert's mode bit 3 says which one holds this frame). HDR display units, as the tonemap writes.
layout(set = 0, binding = 4) uniform sampler2D sceneBg0;
layout(set = 0, binding = 5) uniform sampler2D sceneBg1;

// The floor: the particle's contribution is at least this x the background's radiance x its alpha, i.e. the
// composite is at least bg x (1 + 0.3 * alpha) wherever the particle used to subtract light. 1.3 = a 30%
// brightening at full coverage; measured on the report's view, its sky view and its dark-terrain view
// (qa/waves/20261030b/evidence/c11/): all three keep the flakes visible and none of them clips.
const float kCv2PrecipFloor = 1.3;

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
    uint bits = uint(pc.obsECEFDir.w + 0.5);
    if ((bits & 2u) != 0u
        && texture(sceneDepthTex, gl_FragCoord.xy / vec2(pc.screenW, pc.screenH)).r < vSeg.z) discard;
    vec3 x = vColor.rgb * cov;
    vec3 y = mix(vec3(1.0) - exp(-x), vec3(1.0) - 1.0 / (vec3(1.0) + x + 0.5 * x * x), cloud.highlightRolloff);
    float a = vColor.a * cov;
    // A flake cannot be darker than the scene it is in front of (see the header): floor its contribution at the
    // background's own radiance x alpha. Mode bit 2 = the resolved background is there (the sky TAA ran this
    // frame), bit 3 = which of the two history images holds it. Diamond dust has alpha 0 (it only glints).
    if ((bits & 4u) != 0u && a > 1e-4)
    {
        ivec2 ip = ivec2(gl_FragCoord.xy);
        vec3  bg = (bits & 8u) != 0u ? texelFetch(sceneBg1, ip, 0).rgb : texelFetch(sceneBg0, ip, 0).rgb;
        // The floor, held under the white ceiling: the composite is y + bg x (1 - a), so lift y only as far as
        // white leaves room for. A flake in front of a near-white cloud is as bright as it can be there (a
        // translucent white thing against white is invisible, as it is in life) - it never clips the frame.
        vec3 lift = a * (kCv2PrecipFloor * bg);
        vec3 room = max(vec3(1.0) - bg * (1.0 - a), vec3(0.0));
        y = max(y, min(lift, room));
    }
    outColor = vec4(y, a);
}
