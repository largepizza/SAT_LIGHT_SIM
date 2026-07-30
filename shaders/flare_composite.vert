#version 450

// ── flare_composite.vert ───────────────────────────────────────────────────────
// Plain fullscreen triangle — stage 3 of the flare architecture overhaul (see FlareSourcePC's
// comment in SatelliteSim.h). No camera/scene data needed; the fragment shader just samples the
// blurred/streaked buffer flare_blur.comp produced this frame.

layout(location = 0) out vec2 uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.5, 1.0);
}
