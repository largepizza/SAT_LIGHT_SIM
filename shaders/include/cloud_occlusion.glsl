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
