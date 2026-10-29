#version 450
// ── comet_splat.frag — one comet tail particle's Gaussian, added into the splat target (comet_splat.vert) ────────────
layout(location = 0) in vec2 vOff;
layout(location = 1) in vec3 vLight;
layout(location = 0) out vec4 outColor;

void main()
{
    float r2 = dot(vOff, vOff);
    if (r2 > 6.76) discard; // (2.6 sigma)
    outColor = vec4(vLight * exp(-0.5 * r2), 0.0);
}
