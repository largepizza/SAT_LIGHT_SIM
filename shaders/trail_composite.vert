#version 450

// ── trail_composite.vert ───────────────────────────────────────────────────────
// Plain fullscreen triangle — stage C of the long-exposure trail pipeline (see the trail block
// comment in SatelliteSim.h). Hand-duplicated from flare_composite.vert rather than reusing its
// .spv: this codebase's convention is that small generic shader bodies get duplicated per feature
// rather than letting one feature's pipeline silently depend on another feature's shader file (see
// CLAUDE.md's "Still hand-duplicated, by choice" note on optDepth/aurora).

layout(location = 0) out vec2 uv;

void main() {
    vec2 pos = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    uv = pos;
    gl_Position = vec4(pos * 2.0 - 1.0, 0.5, 1.0);
}
