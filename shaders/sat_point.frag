// ── sat_point.frag ────────────────────────────────────────────────────────────
// Point-sprite fragment shader for satellite rendering.
//
// Two layers per sprite, both Gaussians that naturally reach ~0 before the edge:
//
//   inner — tight relative-sigma Gaussian (4.5% of sprite radius).
//           Stays tiny regardless of sprite size — looks like a true point source.
//           Log2-compressed brightness so ISS-class sats don't blow out.
//
//   glow  — outer soft Gaussian in absolute pixel units (fragAngSize converts).
//           Sigma grows with log2(intensity); sprite is sized to 6σ in
//           sat_flare.comp so the Gaussian is ~1% at the sprite boundary —
//           no smoothstep needed, no hard circular edge visible.
//           Zero for intensity ≤ 1; fades in log-scale above that.
//
// Lens flare corona + ghost artifacts are rendered by sat_sky.frag via lensFlare()
// using per-satellite positions from glowBuf.flareEntries.
//
// Output is (rgb*brightness, brightness) for additive blend accumulation.
// ─────────────────────────────────────────────────────────────────────────────

#version 450

layout(location = 0) in vec3  fragColor;
layout(location = 1) in float fragIntensity;
layout(location = 2) in float fragAngSize;

layout(location = 0) out vec4 outColor;

void main() {
    // gl_PointCoord is [0,1] across the point sprite quad.
    // c is centered at (0,0); d is distance from centre.
    vec2  c = gl_PointCoord - 0.5;
    float d = length(c);

    if (d > 0.5) discard;

    // ── Inner core: tight pinpoint, log-compressed brightness ─────────────────
    // sigmaInner = 0.045 (relative): 4.5% of sprite half-width.
    // exp(-(0.5)²/(2×0.045²)) ≈ 0 — naturally reaches zero at sprite edge, no cutoff.
    // Stays visually small regardless of sprite size; lensFlare() adds the corona.
    const float sigmaInner = 0.045;
    // Sigmoid ramp: x²/(1+x²) where x = intensity × scale.
    // Near-zero at visThresh → smooth fade in/out.
    // Half-max at intensity ≈ 1/scale = 0.05 → ramps quickly above that.
    // Full brightness (3.0) by intensity ≈ 0.2; capped there for bright sats.
    //   intensity=0.008 (visThresh) → coreScale ≈ 0.008  (nearly invisible)
    //   intensity=0.05              → coreScale ≈ 1.75   (comfortably bright)
    //   intensity=0.15              → coreScale ≈ 2.93   (near-full)
    //   intensity=0.5+              → coreScale = 3.0    (capped)
    // Tune scale: higher = ramp starts earlier (brighter at threshold).
    float x = fragIntensity * 30.0;
    float coreScale = min((x * x / (1.0 + x * x)) * 3.5, 3.0);
    float inner = exp(-d * d / (2.0 * sigmaInner * sigmaInner)) * coreScale;

    // ── Outer glow: absolute-pixel Gaussian, independent of sprite size ────────
    // pixelDist converts gl_PointCoord distance to real pixels via the sprite size.
    // glowSigmaPx MUST match sat_flare.comp (log2(intensity)*7) so the sprite is
    // sized to exactly 6σ — the Gaussian hits ~1% at the boundary, no edge visible.
    // glowBrightness = 0 for intensity ≤ 1, so dim sats have no soft halo.
    // glowSigmaPx MUST match sat_flare.comp (log2(effectFlare)*10) so the sprite
    // is correctly sized relative to the glow.
    // Lorentzian 1/(1+r²) has a heavier tail than Gaussian — glow spreads more
    // gradually with no abrupt dropoff that could read as a hard circular edge.
    // Full-sprite smoothstep drives glow to exactly 0.0 at the sprite boundary,
    // eliminating any residual circle artifact at any intensity.
    // kGlowMax caps peak brightness so the halo is always dimmer than the inner
    // core — the bright point stays distinguishable inside the diffuse glow.
    // Keep low: many overlapping sprites stack additively; aggregate cluster glow
    // comes from the sky glow bins pass, not from individual sprites.
    // Tune: 0.05 = very subtle, 0.08 = moderate, 0.15 = dramatic single-sat.
    const float kGlowMax = 0.08;
    float pixelDist      = d * fragAngSize;
    float glowSigmaPx    = max(0.5, log2(max(fragIntensity, 1e-4)) * 10.0);
    float glowBrightness = clamp(log2(max(fragIntensity, 1.0)) * 0.5, 0.0, kGlowMax);
    float r              = pixelDist / glowSigmaPx;
    float glow           = (1.0 / (1.0 + r * r)) * glowBrightness;
    glow                *= 1.0 - smoothstep(0.0, 0.5, d);

    float brightness = inner + glow;
    outColor = vec4(fragColor * brightness, brightness);
}
