#version 450
// ── comet_field.frag — a dust grain's local (emission time, ln beta) field, into the field atlas (comet_splat.vert's
// field pass, half the splat atlas's resolution) ──────────────────────────────────────────────────────────────────────
// Weighted by the light that carries it: (L te, L ln beta, L, L te^2), te in days from now. The composite divides it
// back and draws the tail's zoom detail from it per screen pixel (comet_composite.frag): the field is linear inside
// each Gaussian, so the light-weighted mean of overlapping particles is a smooth field even where they are wide, and
// its spread (from the last channel: sheets of the tail overlapping in projection, particles that disagree) says which
// bands it can carry. RGBA32F: in half floats te^2 (~900 for old dust) loses the spread entirely.
layout(location = 0) in vec2 vOff;
layout(location = 1) in vec3 vLight;
layout(location = 2) flat in vec4 vTe;    // x te (days from now), y ln beta, z per sigma -> per full-res pixel, w detail on (0..1)
layout(location = 3) flat in vec4 vGrad;  // xy d te / d vOff, zw d ln beta / d vOff
layout(location = 4) flat in vec4 vRect;  // the atlas level's rectangle (px)
layout(location = 0) out vec4 outField;
layout(location = 1) out vec4 outGrad;    // the field atlas's second target: its screen gradients (below)

void main()
{
    float r2 = dot(vOff, vOff);
    if (r2 > 6.76) discard; // (2.6 sigma)
    if (any(lessThan(gl_FragCoord.xy, vRect.xy)) || any(greaterThanEqual(gl_FragCoord.xy, vRect.zw))) discard;
    float W = vTe.w * dot(vLight, vec3(0.2126, 0.7152, 0.0722)) * exp(-0.5 * r2);
    // the light-weighted magnitudes of the field's gradients per full-res pixel (te, ln beta, the striae's te + 0.7
    // ln beta): the composite's LOD, smooth across texels (derivatives of the bilinear field are not: issue
    // 20261030_140112's blocky "pixel noise")
    // (half floats: each clamped where no octave shows anyway, 10 days / e-folds per pixel; the weight in alpha)
    vec3 g = min(vTe.z * vec3(length(vGrad.xy), length(vGrad.zw), length(vGrad.xy + 0.7 * vGrad.zw)), vec3(10.0));
    outGrad = W * vec4(g, 1.0);
    float te = vTe.x + dot(vGrad.xy, vOff);
    float lb = vTe.y + dot(vGrad.zw, vOff);
    outField = vec4(W * te, W * lb, W, W * te * te);
}
