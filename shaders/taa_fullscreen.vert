#version 450
// Fullscreen triangle with no inputs (sky TAA's depth restore, taa_depth_restore.frag).
void main() {
    vec2 uv = vec2((gl_VertexIndex << 1) & 2, gl_VertexIndex & 2);
    gl_Position = vec4(uv * 2.0 - 1.0, 0.5, 1.0);
}
