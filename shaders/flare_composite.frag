#version 450

// ── flare_composite.frag ───────────────────────────────────────────────────────
// Stage 3 of the flare architecture overhaul (see FlareSourcePC's comment in SatelliteSim.h).
// One additive fullscreen-triangle draw, appended at the end of recordDraw() (after satellites and
// stars, under the UI) — samples the blurred/streaked buffer flare_blur.comp produced this frame
// and adds it into the frame, replacing the deleted per-pixel flareEntries loop.

layout(location = 0) in vec2 uv;
layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 0) uniform sampler2D flareTex;

layout(push_constant) uniform PC {
    float gain; // user-tunable overall glow gain (Settings > Display, "Flare glow gain")
} pc;

void main() {
    vec3 c = texture(flareTex, uv).rgb * pc.gain;

    // Ceiling: this glow layer never adds more than ~0.8, however high "Flare glow gain"/"Flare streak"
    // go, so a satellite's own point-sprite core always reads brighter than its surrounding glow (the
    // sun and satellites both draw through here). Until 2026-09-24 it was a hard per-channel
    // min(c, 0.8): where many flares overlapped the glow went flat at the cap with a sharp edge around
    // it — the "puffy white areas with distinctive cutoffs" of a dense constellation. Now a soft knee on
    // the luminance: unchanged below kKnee, then an exponential approach to kCap with a continuous
    // slope, so overlapping glows keep their gradients and fade out smoothly.
    const float kKnee = 0.45, kCap = 0.8;
    float l = dot(c, vec3(0.2126, 0.7152, 0.0722));
    if (l > kKnee) {
        float lc = kKnee + (kCap - kKnee) * (1.0 - exp(-(l - kKnee) / (kCap - kKnee)));
        c *= lc / l;
    }
    c = min(c, vec3(1.0));

    outColor = vec4(c, 1.0);
}
