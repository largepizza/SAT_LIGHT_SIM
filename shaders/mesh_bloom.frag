#version 450
// Satellite mesh glints → the flare/bloom source (lighting overhaul Phase 4c). Drawn as a fullscreen
// triangle inside flareSourceRenderPass (1/4 resolution, additive). Each texel takes the brightest
// scene-mesh pixel of the block it covers and seeds the same log-compressed glow flare_source.frag
// gives a satellite sprite — so a glint keeps its bloom/streaks once the sprite has handed over to
// the mesh. Only the part of the radiance that the sky's exposure would push past white glows.

layout(set = 0, binding = 0, rgba32f) uniform readonly image2D meshColorImg;
layout(push_constant) uniform PC {
    float exposure; // the sky's exposure this frame (sat_sky.frag's day/night mix)
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
    float best = 0.0;
    vec3  tint = vec3(1.0);
    for (int y = 0; y < s; ++y)
        for (int x = 0; x < s; ++x) {
            ivec2 p = min(base + ivec2(x, y), size - 1);
            vec3  L = imageLoad(meshColorImg, p).rgb;
            float l = dot(L, vec3(0.2126, 0.7152, 0.0722)) * pc.exposure;
            if (l > best) { best = l; tint = L / max(dot(L, vec3(0.2126, 0.7152, 0.0722)), 1e-6); }
        }
    // Same response flare_source.frag applies to a sprite's effectFlare: nothing below white.
    float b = clamp(log2(max(best, 1.0)) * 0.5, 0.0, 4.0) * pc.gain;
    outColor = vec4(tint * b, b);
}
