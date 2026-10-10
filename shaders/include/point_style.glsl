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
    // Satellites only (sat_flare.comp / sat_point.frag, 2026-10-07): sat_flare.comp writes a DISPLAYED
    // effectFlare — physical x 10^(0.4 satGainMag) x the resolved-object factor — so the bloom, glare, sky glow
    // and ocean glints follow the exposure, the adaptation and an object's size. refMag/limitMag above already
    // carry the same shift for stars and planets, so a satellite's point is drawn at pointPsf(mag + satGainMag).
    float satGainMag;  // exposure gain - satellite adaptation, magnitudes (SatelliteSim::satDisplayGainMag)
    float satPhysK;    // a model's mean pixel value before the tonemap = satPhysK x raw flux / its area in pixels
    float satResolved; // 0..1: how far a resolved satellite's sprite takes its model's surface brightness
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

// The resolved-object factor (sat_flare.comp): the displayed effectFlare whose point, spread over a Gaussian of
// sigma^2 = sigmaPx^2 + nPx / 2pi, has the peak a model of nPx pixels shows: 1 - exp(-satPhysK x rawFlux / nPx),
// its mean pixel through the sky exposure and tonemap. In the frame of pointPsf with the shifted reference
// magnitude undone (it is a displayed value). Returns x = that effectFlare, y = sigma^2.
vec2 resolvedSatFlare(float rawFlux, float nPx)
{
    const float k = 1.3287712; // 0.4 · log2(10)
    float s0 = pointStyle.sigmaPx;
    float sig2 = s0 * s0 + nPx / 6.2831853;
    float peak = 1.0 - exp(-pointStyle.satPhysK * rawFlux / max(nPx, 1e-3));
    float d  = peak * sig2 / (s0 * s0);
    float dl = exp2(-k * pointStyle.gamma * (pointStyle.limitMag - pointStyle.refMag));
    float refU = pointStyle.refMag - pointStyle.satGainMag;
    float m = refU - log2(max(d + dl, 1e-30)) / (k * pointStyle.gamma);
    return vec2(0.008 * exp2(k * (6.0 - m)), sig2);
}

// Point-sprite edge length that holds the PSF out to 3 sigma, plus a pixel of margin each side.
float pointSpriteSizePx(float sigma) { return 2.0 * (3.0 * sigma + 1.0); }
float pointSpriteSigmaPx(float sizePx) { return (0.5 * sizePx - 1.0) / 3.0; } // its inverse

#endif
