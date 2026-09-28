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

#endif
