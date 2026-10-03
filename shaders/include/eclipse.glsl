// eclipse.glsl — the Moon's shadow in a solar eclipse, per point (2026-10-03).
// Needs cloud_params.glsl first (cloud.moonCenter: the Moon's centre, Earth-centred in the observer's ENU, km;
// cloud.moonMisc.y: 1 = an eclipse is possible this frame; cloud.taaJitter.w: the Sun's true angular radius).
//
// Every sunlit term gates its call on cloud.moonMisc.y > 0.5 (uniform), so outside an eclipse this costs a branch.
// Shared by sat_sky.frag (ground, air, sky light), cloud_v2_march.comp and cloud_v2_far.comp (clouds), so the shadow
// falls on everything from the same geometry.

// The share of the Sun's disc seen from pKm past the Moon. pKm: Earth-centred, the observer's ENU axes, km; sunD:
// unit, same axes. blurKm > 0 averages it over roughly that radius around the point — the SKY light there, which
// comes from the sunlit air within reach; 0 = the direct sunlight at the point itself.
// The covered share is the equal-disc lens area at the centre distance's place between the umbra's edge (|am - as|)
// and first contact (am + as): exact when the discs are equal and within ~2% of the Sun for the real radii.
float eclipseSunVis(vec3 pKm, vec3 sunD, float blurKm)
{
    vec3  v  = cloud.moonCenter.xyz - pKm;
    float dv = length(v);
    if (dot(v, sunD) <= 0.0) return 1.0;
    float d  = length(cross(v, sunD)) / dv;         // the Moon-Sun separation seen from the point (rad)
    float am = 1737.4 / dv, as = cloud.taaJitter.w, b = blurKm / dv;
    float lo = abs(am - as) - b, hi = am + as + b;
    float u  = clamp((d - lo) / (hi - lo), 0.0, 1.0);
    float covered = (2.0 / 3.14159265) * (acos(u) - u * sqrt(max(1.0 - u * u, 0.0)));
    float vMin = as > am ? 1.0 - (am * am) / (as * as) : 0.0;   // an annular eclipse's ring
    return 1.0 - (1.0 - vMin) * covered;
}

// Sky light radius: the sunlit air that lights a point's sky (cloud and ground ambient, the air's second scattering).
const float kEclipseSkyBlurKm = 180.0;
// How far the light that reaches the shadow travels sideways through the air from the sunlit air around it.
const float kEclipseSideKm = 80.0;

// The SKY light at a point at altitude hM (m) in the Moon's shadow: the blurred visibility above, coloured by the
// sideways path its light took through the air at that height (needs common.glsl). Low in the shadow it is deep
// orange (the 360-degree sunset of totality), high up pale; outside the shadow white (no change). Ambient terms
// (cloud, ground, fog) multiply their sky light by it; the sky's own second scattering uses it too.
vec3 eclipseSkyLight(vec3 pKm, vec3 sunD, float hM)
{
    float s = eclipseSunVis(pKm, sunD, kEclipseSkyBlurKm);
    float h = max(hM, 0.0);
    // + the ozone shell crossed at a grazing angle (cloud_v2_march.comp's sunTransmit: vertical optical depth
    // (0.019, 0.027, 0.0016), secant ~20): the Chappuis band takes the green and yellow out, so the shadow's sky is
    // deep blue overhead and orange-red at the horizon (without it, teal over yellow-green).
    vec3  tint = exp(-(BETA_R_BASE * exp(-h / H_R) + vec3(1.1 * BETA_M_BASE * exp(-h / H_M))) * (kEclipseSideKm * 1000.0)
                     - vec3(0.019, 0.027, 0.0016) * 20.0);
    return s * mix(tint, vec3(1.0), s);
}
