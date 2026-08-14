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

    // Hard ceiling, per-channel: this glow layer can never add more than 0.5 in any channel, no
    // matter how high "Flare glow gain"/"Flare streak" are pushed. Below a satellite's own point-
    // sprite core (sat_point.frag can reach ~3x brightness at its inner core, i.e. full white on
    // its own) the core always reads brighter than its surrounding flare, so the flare no longer
    // washes the point itself out to white — this is what satellites AND the sun both draw
    // through (flare_source.vert adds one virtual point for the sun, gl_VertexIndex==satCount).
    c = min(c, vec3(0.8));

    outColor = vec4(c, 1.0);
}
