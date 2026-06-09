// ── star_point.frag ───────────────────────────────────────────────────────────
// Fragment shader for background stars.
//
// Stars are genuine point sources: no outer glow, no satellite-style bloom.
// A tight Gaussian core with log2-compressed brightness renders them as sharp
// pinpoints across the full dynamic range from naked-eye limit (~mag 6) down
// to bright stars like Sirius (-1.46) without any visible sprite artefacts.
//
// Atmospheric scintillation (twinkling) can be layered on top in a future pass
// once a per-frame time value is exposed in the push constants.
//
// Output: (rgb*brightness, brightness) for additive blend accumulation.
// ─────────────────────────────────────────────────────────────────────────────

#version 450

layout(location = 0) in vec3  fragColor;
layout(location = 1) in float fragIntensity;
layout(location = 2) in float fragAngSize; // from sat_point.vert — unused for stars

layout(location = 0) out vec4 outColor;

void main() {
    vec2  c = gl_PointCoord - 0.5;
    float d = length(c);

    if (d > 0.5) discard;

    // Tight Gaussian core — naturally reaches ~0 at sprite edge, no cutoff.
    // sigmaInner = 0.045 (relative): inner radius is 4.5% of sprite half-width.
    // exp(-(0.5)²/(2×0.045²)) ≈ 0 so no boundary is visible.
    const float sigmaInner = 0.045;
    // Same scale-from-zero formula as sat_point.frag — dim stars fade in
    // smoothly rather than snapping from a bright dot to invisible.
    float coreScale = min(log2(2.0 + fragIntensity) * 1.5, 3.0);
    float brightness = exp(-d * d / (2.0 * sigmaInner * sigmaInner)) * coreScale;

    outColor = vec4(fragColor * brightness, brightness);
}
