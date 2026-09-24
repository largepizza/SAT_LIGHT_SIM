#version 450
// Satellite mesh glints → the flare/bloom source (lighting overhaul Phase 4c/4d). A fullscreen
// triangle inside flareSourceRenderPass (1/4 resolution, additive). Each texel sums the scene-mesh
// pixels it covers, each weighted by its instance's bloomScale — the bloom seed that satellite's
// sprite would have put down (flare_source: b(effectFlare) over its point disc), per unit of the
// mesh's rendered flux. So a mesh seeds exactly its sprite's glow, placed wherever its light actually
// is: across the hand-off a flare keeps its punch, and on a large model the glow sits on the glint.

layout(set = 0, binding = 0, rgba32f) uniform readonly image2D meshColorImg; // rgb radiance, a = slot + 1
struct MeshInstanceBloom { vec4 pad[20]; uint firstMaterial, firstOccluder, occluderCount; float bloomScale; uvec4 tail; };
layout(set = 0, binding = 1, std430) readonly buffer MeshInstances { MeshInstanceBloom instances[]; };
layout(push_constant) uniform PC {
    float exposure; // unused (kept for the layout)
    float gain;
    float scale;    // scene pixels per target pixel
    float pad;
} pc;
layout(location = 0) out vec4 outColor;

void main()
{
    ivec2 size = imageSize(meshColorImg);
    int   s    = max(1, int(pc.scale + 0.5));
    ivec2 base = ivec2(gl_FragCoord.xy) * s;
    float seed = 0.0;
    vec3  col  = vec3(0.0);
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            vec4 m = imageLoad(meshColorImg, min(base + ivec2(x, y), size - 1));
            if (m.a < 0.5) continue;
            // a = instance slot + 1 + the photometric share of the pixel's light (sat_mesh.frag):
            // only sunlight and earthshine seed the bloom, not reflections of the Earth or moonlight,
            // matching the model intensity bloomScale is normalised by.
            float l = dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) * fract(m.a);
            float k = l * instances[int(floor(m.a)) - 1].bloomScale;
            seed += k;
            col  += m.rgb * (k / max(dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-6));
        }
    if (seed <= 0.0) discard;
    outColor = vec4(col * pc.gain, seed * pc.gain); // rgb carries the tint, like fragColor × brightness
}
