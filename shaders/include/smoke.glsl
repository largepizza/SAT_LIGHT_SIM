// Wildfire smoke (2026-10-23, SatelliteSim::weatherFillWildfires): Gaussians in the dust list, a contiguous range given
// by the list header's word 1 (bits 8-17 the first, 18-27 the count; bits 0-7 are the engine lights' count).
// Smoke is spectral: fine combustion aerosol extinguishes blue more than red (Angstrom exponent ~1.6, 650 / 550 /
// 450 nm), so the Sun seen through it, and the light that reaches the ground under it, turn orange and red.
const vec3 kSmokeSpec = vec3(0.765, 1.0, 1.378);
// x the Angstrom exponent ("Smoke spectral", CloudV2Params gnd.y; 1 = the physical ~1.6, 0 = grey).
vec3 smokeSpecK(float s) { return pow(kSmokeSpec, vec3(max(s, 0.0))); }
// The colour of the light a smoke Gaussian of optical depth tau (at 550 nm) scatters, luminance 1 (2026-10-26, "it appears
// so beige"): its scattering is as spectral as its extinction (sp = smokeSpecK), so a thin veil scatters blue light
// (aged smoke from the side is blue-grey) and a thick one saturates to grey: (1 - e^(-tau sp)) / (1 - e^(-tau)).
vec3 smokeScatterColor(float tau, vec3 sp)
{
    vec3 F = (vec3(1.0) - exp(-tau * sp)) / max(1.0 - exp(-tau), 1e-5);
    return F / max(dot(F, vec3(0.2126, 0.7152, 0.0722)), 1e-4);
}
uint smokeFirst(uint hdrWord) { return (hdrWord >> 8u) & 1023u; }
uint smokeCount(uint hdrWord) { return (hdrWord >> 18u) & 1023u; }

// The optical depth of one Gaussian (inverse covariance ia.xyz / ib.xyz, peak normaliser x cross-section = k) along the
// half-infinite ray x + s u, s >= 0. x is relative to the Gaussian's centre.
float smokeColumn(vec3 x, vec3 u, vec4 ia, vec4 ib, float k)
{
    vec3 Mu = vec3(ia.x * u.x + ib.x * u.y + ib.y * u.z, ib.x * u.x + ia.y * u.y + ib.z * u.z,
                   ib.y * u.x + ib.z * u.y + ia.z * u.z);
    float q = dot(u, Mu);
    if (q <= 0.0) return 0.0;
    vec3 Mx = vec3(ia.x * x.x + ib.x * x.y + ib.y * x.z, ib.x * x.x + ia.y * x.y + ib.z * x.z,
                   ib.y * x.x + ib.z * x.y + ia.z * x.z);
    float b = dot(x, Mu) / q;   // the closest approach is at s = -b
    float e = dot(x, Mx) - b * b * q;
    if (e > 40.0) return 0.0;
    float sq = sqrt(0.5 * q);
    // erf(sq (inf + b)) - erf(sq b) = 1 - erf(sq b), by the same approximation as the plumes' (dustErf)
    float z = sq * b, z2 = z * z;
    float t = 1.0 - exp(-z2 * (1.2732395 + 0.147 * z2) / (1.0 + 0.147 * z2));
    float er = sign(z) * sqrt(max(t, 0.0));
    return k * exp(-0.5 * e) * sqrt(3.14159265 / (2.0 * q)) * (1.0 - er);
}
