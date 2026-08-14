#version 450

// ── trail_composite.frag ───────────────────────────────────────────────────────
// Stage C of the long-exposure trail pipeline (see the trail block comment in SatelliteSim.h). One
// additive fullscreen-triangle draw, appended at the end of recordDraw() (after the flare composite
// draw — order between the two doesn't matter, both are ONE/ONE additive blending, which is
// commutative) — samples the persistent trailAccumImg (decayed + splatted this frame in
// recordCompute()) and adds it into the frame.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D trailTex;

layout(push_constant) uniform PC {
    float gain; // user-tunable overall trail gain (Settings > Photometry, "Trail gain")
} pc;

void main() {
    vec3 c = texture(trailTex, uv).rgb * pc.gain;

    // Reinhard tonemap (c / (1+c)) instead of a hard per-channel clamp. The hard clamp
    // (min(c, vec3(0.8)), matching flare_composite.frag) was tried first, but a hard clamp maps
    // every value above its ceiling to the SAME output — and steady-state accumulation (a
    // near-stationary point re-splatted every frame converges to roughly
    // 1/(1-decayFactor) x one frame's contribution, easily 100-250x at the default decay rate)
    // pushes most visible stars/satellites well past any reasonable fixed ceiling within a few
    // seconds. That flattened exactly the kind of subtle relative brightness difference real
    // effects (atmospheric extinction dimming near the horizon, light-pollution dimming, cloud
    // occlusion) are supposed to produce — the trail looked uniformly bright end to end regardless
    // of how dim any individual sample actually was. Reinhard stays near-linear for small/moderate
    // values (where those effects live) and only compresses — never flattens to a constant — once
    // truly saturated, so relative ordering (and therefore visible dimming) survives at any
    // brightness. Asymptotes to 1.0 per channel, so it still can't blow out the frame.
    c = c / (1.0 + c);

    outColor = vec4(c, 1.0);
}
