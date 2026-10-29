#version 450
// ── comet_splat.frag — one comet tail particle's Gaussian, added into the splat atlas (comet_splat.vert) ─────────────
// Its light, within its atlas level's rectangle. (The dust's detail field: comet_field.frag, a pass of its own.)
layout(location = 0) in vec2 vOff;
layout(location = 1) in vec3 vLight;
layout(location = 2) flat in vec4 vTe;    // x te (days from now), y ln beta, w detail on (0..1)
layout(location = 3) flat in vec4 vGrad;  // xy d te / d vOff, zw d ln beta / d vOff
layout(location = 4) flat in vec4 vRect;  // the atlas level's rectangle (px)
layout(location = 0) out vec4 outColor;

void main()
{
    float r2 = dot(vOff, vOff);
    if (r2 > 6.76) discard; // (2.6 sigma)
    if (any(lessThan(gl_FragCoord.xy, vRect.xy)) || any(greaterThanEqual(gl_FragCoord.xy, vRect.zw))) discard;
    outColor = vec4(vLight * exp(-0.5 * r2), 0.0);
}
