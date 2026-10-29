#version 450
// ── comet_composite.vert — a full-screen triangle at infinity (kDepthFar: every surface hides it) ──────────────────
#include "depth.glsl"
layout(location = 0) out vec2 vUV;
void main()
{
    vec2 p = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    vUV = p;
    gl_Position = vec4(p * 2.0 - 1.0, kDepthFar, 1.0);
}
