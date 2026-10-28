#ifndef SATLIGHTSIM_CLOUD_OCCLUSION_GLSL
#define SATLIGHTSIM_CLOUD_OCCLUSION_GLSL

// How a point source (satellite, flare source, glare sprite) is hidden by the cloud composite.
// cloudTargetA's alpha (written by cloud_march.comp) is a signed distance in KILOMETRES:
//   >= 0   opaque cloud (the ray's transmittance fell below 0.5 there and ended below 0.1)
//   <  0   -(the transmittance-weighted mean distance of whatever cloud the ray crossed); with no
//          cloud the transmittance is 1 and the distance does not matter (-60000)
// Kilometres because the target is RGBA16F: in metres anything past 65.5 km overflowed to +inf,
// so from orbit the range test never fired and every cloud dimmed the satellites in front of it
// (the night side's clouds swallowed the AI disk seen from 2000 km).
// cloudT = cloudTargetB's transmittance (mean of rgb); rangeM = the source's distance.
// The emissive flag (2026-10-20): cloud_march.comp marks a texel whose distance is the nearest emissive / plume
// light's (no cloud there) by the half float's lowest mantissa bit, and clears it on every real cloud distance.
// The target is RGBA16F and gathered texels are exact, so the bit survives; filtered reads are not flags.
float cloudAlphaFlag(float aKm, bool emit) {
    uint h = packHalf2x16(vec2(aKm, 0.0)) & 0xFFFFu;
    h = emit ? (h | 1u) : (h & ~1u);
    return unpackHalf2x16(h).x;
}
bool cloudAlphaIsEmit(float aKm) {
    return abs(aKm) < 50000.0 && (packHalf2x16(vec2(aKm, 0.0)) & 1u) != 0u;
}

float cloudPointVisibility(float aKm, float cloudT, float rangeM, float power)
{
    float tC = abs(aKm) * 1000.0;
    if (aKm >= 0.0 && rangeM > tC) return 0.0;
    float behind = smoothstep(tC * 0.99 - 300.0, tC * 1.01 + 300.0, rangeM);
    return mix(1.0, pow(clamp(cloudT, 0.0, 1.0), power), behind);
}

// Review 12: per TEXEL, then blended. The filtered alpha at a cloud edge blends a real distance with the
// no-cloud -60000 km into something near 0 km, which reads as an opaque cloud in front of everything: from
// 8500 km the AI ring's satellites (in front of the clouds) went dark along every cloud outline, and the
// outline flickered with the cloud march's sampling. Each of the four texels decides with its own distance
// and transmittance (green), and the four visibilities take the bilinear weights.
float cloudPointVisibilityAt(sampler2D texA, sampler2D texB, vec2 uv, float rangeM, float power)
{
    vec4  ga = textureGather(texA, uv, 3);
    vec4  gt = textureGather(texB, uv, 1);
    vec2  f  = fract(uv * vec2(textureSize(texA, 0)) - 0.5);
    vec4  w  = vec4((1.0 - f.x) * f.y, f.x * f.y, f.x * (1.0 - f.y), (1.0 - f.x) * (1.0 - f.y));
    float v  = 0.0;
    for (int k = 0; k < 4; ++k) v += w[k] * cloudPointVisibility(ga[k], gt[k], rangeM, power);
    return v;
}

#endif

// The air split of a cloud texel seen through wildfire smoke (2026-10-26, issue 20261026_204045: far clouds "poke through"
// the smoke as light-blue shapes): sat_sky.frag adds the air from the eye to this distance in front of the cloud,
// undimmed. Physically it is the air to the smoke (tS) + the smoke's transmittance x the air from there to the cloud (tC);
// the split is solved so the air to it carries that much light, in AIR-BRIGHTNESS space: the horizon's air saturates
// within ~50 km, and a split by distance (tS + T (tC - tS)) put almost the whole bright horizon haze in front of every far
// cloud. kS = the share of the texel's opacity that is the cloud seen through the smoke.
// The brightness is 1 - e^(-0.2 c(t)), c = the molecular column along THIS ray from the eye (atmosphere.glsl, sea-level
// vertical columns; 0.2 c = t / 40 km for a level ray at sea level, the form it replaced, 2026-10-27). It was
// 1 - e^(-t / 40 km) whatever the ray: from orbit the air in front of a cloud is all in its last ~100 km, hundreds of km
// out, where that form had saturated (both ends at b ~ 1, the split clamped at 552 km): every cloud past 552 km seen
// through even a trace of smoke lost the air in front of it, the limb's blue haze, and read orange-yellow in a hard-edged
// patch wherever the smoke's optical depth passed 1e-3 (issue 20261027_182936, Queensland from 156 / 1834 km).
#ifdef ATMOSPHERE_GLSL
float smokeAirSplitM(vec3 eye, vec3 dir, float tS, float tC, float kS)
{
    tS = clamp(tS, 0.0, tC);
    float cS = atmColumn(eye, dir, max(tS, 1.0), ATM_H_RAY), cC = atmColumn(eye, dir, max(tC, 1.0), ATM_H_RAY);
    float bS = 1.0 - exp(-0.2 * cS), bC = 1.0 - exp(-0.2 * cC);
    float b  = bS + clamp(kS, 0.0, 1.0) * max(bC - bS, 0.0);
    // the column the split must reach, then its distance: c(t) grows with t, bisected over [tS, tC]
    float cT = min(-5.0 * log(max(1.0 - b, 1e-7)), cC);
    if (cT <= cS + 1e-6) return tS;
    float lo = tS, hi = tC;
    [[dont_unroll]] for (int k = 0; k < 14; ++k) {
        float m = 0.5 * (lo + hi);
        if (atmColumn(eye, dir, max(m, 1.0), ATM_H_RAY) < cT) lo = m; else hi = m;
    }
    return 0.5 * (lo + hi);
}
#endif
