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
    outColor = vec4(c, 1.0);
}
