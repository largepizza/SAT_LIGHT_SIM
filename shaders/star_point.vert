#version 450

// ── SSBO: star records written by CPU (updateStars) each frame ────────────────
struct SatVisible {
    vec3  skyDir;         // ENU unit vector
    float flareIntensity; // raw magnitude-based intensity × night factor
    vec3  baseColor;      // B-V derived RGB
    float angularSize;    // point sprite size (pixels)
};
layout(set = 0, binding = 1) readonly buffer SatVisibleBuf {
    SatVisible satellites[];
};

// ── Push constants (full SatDrawPC, 128 bytes) ────────────────────────────────
layout(push_constant) uniform PC {
    mat4  skyView;
    float fovYRad;
    float aspect;
    float gmst;        // offset 72
    float waveTime;    // offset 76 — simSecInDay, used for twinkling
    vec4  sunDirENU;   // offset 80 — unused
    vec4  moonDirENU;  // offset 96 — unused
    vec4  obsECEFDir;  // offset 112: xyz=obs ECEF, w=obsHeightOffset (m)
} pc;

layout(location = 0) out vec3  fragColor;
layout(location = 1) out float fragIntensity;
layout(location = 2) out float fragAngSize;
layout(location = 3) out float fragTwinkle;  // scintillation modulator [0,~2]; 1 = no twinkle

void main() {
    SatVisible sat = satellites[gl_VertexIndex];

    vec3 cam = (pc.skyView * vec4(sat.skyDir, 0.0)).xyz;

    if (sat.flareIntensity <= 0.0 || cam.z >= -0.001) {
        gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
        gl_PointSize = 0.001;
        fragColor     = vec3(0.0);
        fragIntensity = 0.0;
        fragAngSize   = 0.001;
        fragTwinkle   = 1.0;
        return;
    }

    float tanHalfFov = tan(pc.fovYRad * 0.5);
    gl_Position  = vec4( cam.x / (-cam.z) / (tanHalfFov * pc.aspect),
                        -cam.y / (-cam.z) /  tanHalfFov,
                         0.5, 1.0);
    gl_PointSize = sat.angularSize;

    fragColor     = sat.baseColor;
    fragIntensity = sat.flareIntensity;
    fragAngSize   = sat.angularSize;

    // ── Atmospheric scintillation ─────────────────────────────────────────────
    // Physics: Kolmogorov turbulence → amplitude scales with air mass (1/sin(el)).
    // Space: atmFrac → 0 so twinkling vanishes above the atmosphere.
    //
    // Precision note: waveTime = simSecInDay (0–86400).  Calling sin() directly
    // with a large argument (43200 × freq × 2π ≈ 1e6) loses all precision in
    // float32 GPU trig.  Fix: reduce to fractional cycle [0,1] with fract()
    // before multiplying by 2π, so the sin argument is always in [0, 2π].

    // Per-star unique phase seeds (hash of ECI direction).
    float h1 = fract(sin(dot(sat.skyDir, vec3(127.1, 311.7,  74.7))) * 43758.5453);
    float h2 = fract(sin(dot(sat.skyDir, vec3(269.5, 183.3, 246.1))) * 65734.7831);

    float t = pc.waveTime;
    const float PI2 = 6.28318530718;

    // Fractional-cycle phase: fract(t*freq) in [0,1], then add per-star hash offset.
    float p1 = fract(fract(t *  3.7) + h1);
    float p2 = fract(fract(t *  7.1) + h2);
    float p3 = fract(fract(t * 11.3) + fract(h1 * 0.3 + h2 * 0.7));

    float tw = sin(p1 * PI2)
             + sin(p2 * PI2) * 0.60
             + sin(p3 * PI2) * 0.30;
    tw /= 1.90;  // normalise to [-1, 1]

    // Atmospheric fraction: 1 at sea level, decays above ~80 km.
    // obsECEFDir.w = obsHeightOffset (user altitude above terrain, metres).
    float obsHeight = max(0.0, pc.obsECEFDir.w);
    float atmFrac   = clamp(exp(-obsHeight / 80000.0), 0.0, 1.0);

    // Air mass proxy: 1/sin(elevation), clamped 1–12.
    // sin(elevation) = sat.skyDir.z (ENU z = Up).
    float sinEl   = max(sat.skyDir.z, 0.05);  // floor at sin(~3°)
    float airMass = clamp(1.0 / sinEl, 1.0, 12.0);

    // Amplitude: 25 % base at zenith, up to 90 % near the horizon, scaled by atmFrac.
    float twAmp = (0.25 + 0.75 * ((airMass - 1.0) / 11.0)) * atmFrac;
    twAmp = min(twAmp, 0.90);

    fragTwinkle = max(0.0, 1.0 + tw * twAmp);
}
