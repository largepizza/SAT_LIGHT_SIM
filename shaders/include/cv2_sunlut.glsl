// ── Clouds v2: the Sun / sky colour table (cloud_v2_sunlut.comp bakes it, cloud_v2_march.comp reads it) ──
// A 2D array texture, kCv2SunLutW x kCv2SunLutH: layer 0 = the Sun's colour arriving at a point through
// the atmosphere (0 in the Earth's shadow), layer 1 = the zenith sky's radiance there, layers 2 / 3 = the mean
// radiance of the upper / lower hemisphere with multiple scattering (2026-10-26), layer 4 = the multiple-scattering
// function Psi in its first 32 x 32 texels (cloud_v2_sunlut.comp). All are functions of two numbers only: the
// point's height and the cosine of the Sun's zenith angle at it.
//   y: height, one row per kCv2SunLutStepM (0 .. 60 km): linear filtering between two rows is the
//      interpolation between fixed 800 m levels the march used (the sun-colour cache, pass 16).
//   x: the cosine through a cube root, so the rows are finest at the horizon, where the colour changes
//      within a fraction of a degree (0.06 deg per texel there, ~4 deg at the zenith).
#ifndef CV2_SUNLUT_GLSL
#define CV2_SUNLUT_GLSL
const float kCv2SunLutW     = 256.0;
const float kCv2SunLutH     = 76.0;
const float kCv2SunLutStepM = 800.0;
const int   kCv2SunLutLayers = 5;

float cv2SunLutMu(float x)                     // a column's cosine (x: 0..1 across the table)
{
    float s = 2.0 * x - 1.0;
    return s * s * s;
}
vec2 cv2SunLutUV(float mu, float h)
{
    return vec2(0.5 + 0.5 * sign(mu) * pow(abs(mu), 1.0 / 3.0),
                (clamp(h, 0.0, (kCv2SunLutH - 1.0) * kCv2SunLutStepM) / kCv2SunLutStepM + 0.5) / kCv2SunLutH);
}
#endif
