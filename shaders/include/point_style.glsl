// ── Shared point-source appearance: apparent magnitude → point spread (2026-09-23) ─────────────
// One mapping for every point of light — satellites (sat_point.frag, sized by sat_flare.comp), stars
// and planets (star_point.frag, sized by updateStars()/updatePlanets()) — so equal magnitudes look
// equal. They used to have independently tuned curves: a mag-6 satellite drew like a mag-4.5 star,
// and Jupiter at -2 like a mag-3 satellite. CPU mirror: pointPsf() in SatelliteSim.cpp.
//
// Model: displayed flux D = 10^(-0.4 gamma (m - refMag)) minus its value at limitMag (so a point
// fades to exactly nothing there), drawn as a Gaussian PSF of sigma sigmaPx and peak D while D <= 1.
// Brighter points saturate: the peak stays at 1 and the PSF widens (sigma = sigmaPx sqrt(D)) so the
// drawn flux keeps growing as D, up to sigmaMaxPx, past which the core brightens instead.
//
// #define POINT_STYLE_BINDING before including (it is a uniform block in the includer's set 0).
#ifndef POINT_STYLE_GLSL
#define POINT_STYLE_GLSL

layout(set = 0, binding = POINT_STYLE_BINDING) uniform PointStyleUBO {
    float refMag;     // magnitude whose peak reaches 1 at the base PSF
    float gamma;      // display response: drawn flux ∝ flux^gamma (1 = linear)
    float limitMag;   // faintest magnitude drawn
    float sigmaPx;    // base PSF sigma, pixels
    float sigmaMaxPx; // widest PSF sigma, pixels
    float psPad0, psPad1, psPad2;
} pointStyle;

// Satellites carry effectFlare (0.008 = mag 6, see sat_flare.comp); stars/planets 10^(-0.4 m).
float satFlareToMag(float effectFlare) { return 6.0 - 2.5 * log2(max(effectFlare, 1e-30) / 0.008) * 0.30103; }
float relFluxToMag(float relFlux)      { return -2.5 * log2(max(relFlux, 1e-30)) * 0.30103; }

// x = peak (the Gaussian's centre value), y = sigma (pixels).
vec2 pointPsf(float mag)
{
    const float k = 1.3287712; // 0.4 · log2(10)
    float d  = exp2(-k * pointStyle.gamma * (mag - pointStyle.refMag));
    float dl = exp2(-k * pointStyle.gamma * (pointStyle.limitMag - pointStyle.refMag));
    d = max(d - dl, 0.0);
    float s0 = pointStyle.sigmaPx;
    if (d <= 1.0)
        return vec2(d, s0);
    float s = min(s0 * sqrt(d), pointStyle.sigmaMaxPx);
    return vec2(d * s0 * s0 / (s * s), s);
}

// Point-sprite edge length that holds the PSF out to 3 sigma, plus a pixel of margin each side.
float pointSpriteSizePx(float sigma) { return 2.0 * (3.0 * sigma + 1.0); }

#endif
