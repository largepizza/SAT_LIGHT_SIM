// ── star_point.frag ───────────────────────────────────────────────────────────
// Fragment shader for background stars.
//
// Minimum-size Gaussian (same technique as sat_point.frag):
//   sigmaAbsPx has a 0.7 px floor so even the faintest, smallest sprite
//   spreads across ~2-3 pixels instead of concentrating all flux in one
//   aliased pixel.  For dim stars, coreScale is small → soft dim blob.
//   For bright stars, coreScale is large → bright, still soft core.
//
// Brightness follows sqrt(intensity): correct 0→0 for invisible stars, and
// gives perceptually even compression across the Sirius-to-naked-eye range.
//
// Desaturation: faint stars mix toward white — their B-V colour is
// imperceptible at low flux, and the white tint reduces "coloured pixel" artefacts.
//
// fragTwinkle (from star_point.vert): per-star atmospheric scintillation
// modulator.  Computed in the vertex stage so it is constant across the sprite.
//
// Output: (rgb*brightness, brightness) for additive blend accumulation.
// ─────────────────────────────────────────────────────────────────────────────

#version 450

layout(location = 0) in vec3  fragColor;
layout(location = 1) in float fragIntensity;
layout(location = 2) in float fragAngSize;
layout(location = 3) in float fragTwinkle;  // scintillation modulator [0,~2]; 1 = no twinkle

layout(location = 0) out vec4 outColor;

void main() {
    vec2  c = gl_PointCoord - 0.5;
    float d = length(c);

    if (d > 0.5) discard;

    // ── Minimum-size Gaussian in absolute pixel units ──────────────────────────
    // 0.7 px sigma floor → ~1.65 px FWHM.  Even a 6 px faint-star sprite
    // spreads its flux across 2-3 pixels rather than one harsh spike.
    const float sigmaInner = 0.045;
    float sigmaAbsPx = max(sigmaInner * fragAngSize, 0.2);
    float pixD       = d * fragAngSize;
    float gaussian   = exp(-pixD * pixD / (2.0 * sigmaAbsPx * sigmaAbsPx));

    // ── Brightness: sqrt compression, correct zero for invisible stars ─────────
    // log2(2 + x) * 1.5 never reaches 0 and makes all stars equally bright;
    // sqrt(x) * 2.8 properly attenuates faint stars so they appear as dim blobs
    // rather than bright pinpoints.
    float coreScale = clamp(sqrt(fragIntensity) * 2.8, 0.0, 3.0);

    // ── Colour desaturation for dim stars ─────────────────────────────────────
    // Bright stars (Betelgeuse, Sirius) keep their B-V tint; faint stars
    // fade toward white since their colour is below perceptual threshold anyway.
    float saturation = clamp(sqrt(fragIntensity) * 1.5, 0.0, 1.0);
    vec3  starColor  = mix(vec3(1.0), fragColor, saturation);

    float brightness = gaussian * coreScale * fragTwinkle;
    outColor = vec4(starColor * brightness, brightness);
}
