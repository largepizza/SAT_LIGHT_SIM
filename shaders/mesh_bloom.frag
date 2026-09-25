#version 450
// Satellite mesh glints → the flare/bloom source (lighting overhaul Phase 4c/4d). A fullscreen
// triangle inside flareSourceRenderPass (1/4 resolution, additive). Each texel sums the scene-mesh
// pixels it covers, each weighted by its instance's bloomScale — the bloom seed that satellite's
// sprite would have put down (flare_source: b(effectFlare) over its point disc), per unit of the
// mesh's rendered flux. So a mesh seeds exactly its sprite's glow, placed wherever its light actually
// is: across the hand-off a flare keeps its punch, and on a large model the glow sits on the glint.
// Alpha carries the same light as effectFlare units (seed x the instance's glareNorm = its sprite's
// effectFlare per unit of seed): glare_find.comp finds concentrated glints in it and gives them the
// sharp glare a sprite of that brightness gets (2026-09-24). Only while the mesh is still point-like
// (tail.w) does all its light count; resolved, only SUN-LIKE surface brightness does — the Sun seen in
// a mirror or a quartz radiator, thousands of times a white panel's radiance. The first cut counted
// all of it, so every edge and vertex of a close-up satellite, each a sliver of a very bright
// satellite's flux, flared, while the one thing that should — the Sun in a mirror — could not: its
// effectFlare passed the RGBA16F target's 65504 and the glint's position came out NaN.

layout(set = 0, binding = 0, rgba32f) uniform readonly image2D meshColorImg; // rgb radiance, a = slot + 1
// Stride must equal GpuMeshInstance (432 B, SatMeshRenderer.h): the earthshine SH block was appended
// 2026-09-24 — this struct must grow with it or every instance past the first reads the wrong one.
// tail = firstComponent, probeSlot, glareNorm, glarePoint (float bits).
struct MeshInstanceBloom { vec4 pad[20]; uint firstMaterial, firstOccluder, occluderCount; float bloomScale; uvec4 tail; vec4 earthSh[5]; };
layout(set = 0, binding = 1, std430) readonly buffer MeshInstances { MeshInstanceBloom instances[]; };
layout(push_constant) uniform PC {
    float exposure; // unused (kept for the layout)
    float gain;
    float scale;    // scene pixels per target pixel
    float pad;
} pc;
layout(location = 0) out vec4 outColor;

// Sun-like surface brightness, in the mesh target's units (π × radiance per unit solar irradiance: a
// sunlit white face ~1, the Sun's disc π/Ω_sun ≈ 4.6e4). The Sun in the mirror preset peaks ~4e4, in an
// OSR radiator ~2e3, in Starlink's dielectric film ~2e2; a rough-metal edge glint, even at grazing
// incidence, stays below ~50.
const float kGlareRadiance0 = 150.0, kGlareRadiance1 = 1500.0;
// Glare saturates at effectFlare 256 (glare.glsl's log response); the cap keeps the half-float target
// (and glare_find.comp's sums) finite.
const float kMaxTexelFlare = 1.0e4;

void main()
{
    ivec2 size = imageSize(meshColorImg);
    int   s    = max(1, int(pc.scale + 0.5));
    ivec2 base = ivec2(gl_FragCoord.xy) * s;
    float seed = 0.0, flare = 0.0;
    vec3  col  = vec3(0.0);
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            vec4 m = imageLoad(meshColorImg, min(base + ivec2(x, y), size - 1));
            if (m.a < 0.5) continue;
            // a = instance slot + 1 + the photometric share of the pixel's light (sat_mesh.frag):
            // only sunlight and earthshine seed the bloom, not reflections of the Earth or moonlight,
            // matching the model intensity bloomScale is normalised by.
            float l = dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)) * fract(m.a);
            MeshInstanceBloom inst = instances[int(floor(m.a)) - 1];
            float k = l * inst.bloomScale;
            seed += k;
            float glareW = max(uintBitsToFloat(inst.tail.w), smoothstep(kGlareRadiance0, kGlareRadiance1, l));
            flare += k * uintBitsToFloat(inst.tail.z) * glareW;
            col  += m.rgb * (k / max(dot(m.rgb, vec3(0.2126, 0.7152, 0.0722)), 1e-6));
        }
    if (seed <= 0.0) discard;
    outColor = vec4(col * pc.gain, min(flare, kMaxTexelFlare)); // rgb carries the tint, like fragColor × brightness
}
