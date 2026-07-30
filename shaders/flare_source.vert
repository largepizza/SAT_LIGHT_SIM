#version 450

// ── flare_source.vert ──────────────────────────────────────────────────────────
// Stage 1 of the flare architecture overhaul (see FlareSourcePC's comment in SatelliteSim.h).
// Renders every visible satellite (from satVisibleBuf, exactly like sat_point.vert — same
// SatVisible layout, same projection math) plus one extra virtual point representing the sun
// (gl_VertexIndex == pc.satCount) into a small offscreen target that a couple of cheap compute
// blur/streak passes then turn into a soft corona+godray texture, replacing the old per-pixel
// flareEntries loop in sat_sky.frag. This is a SECOND, parallel draw of the same satellite data
// into a different, smaller, blurred target — not a replacement for the crisp dot-sprite pass.

struct SatVisible {
    vec3  skyDir;
    float flareIntensity;
    vec3  baseColor;
    float angularSize;
};
layout(set = 0, binding = 1) readonly buffer SatVisibleBuf {
    SatVisible satellites[];
};

layout(push_constant) uniform PC {
    mat4  skyView;
    float fovYRad;
    float aspect;
    uint  satCount;
    float sunRefIntensity;
    vec4  sunDirENU;     // xyz = dir, w = sin(elevation)
    vec2  screenSizePx;  // this pass's own target size — unused here, read by the fragment stage
    float resScale;      // flareExtent / ctx.swapExtent ratio
    float pad0;
} pc;

layout(location = 0) out vec3  fragColor;
layout(location = 1) out float fragIntensity;

void main() {
    vec3  skyDir;
    float intensity;
    vec3  baseColor;
    float angSize;

    if (gl_VertexIndex >= int(pc.satCount)) {
        // Virtual "sun" point — not read from satVisibleBuf at all.
        skyDir    = normalize(pc.sunDirENU.xyz);
        intensity = (pc.sunDirENU.w < -0.05) ? 0.0 : pc.sunRefIntensity;
        baseColor = vec3(1.0, 0.95, 0.85);
        angSize   = 64.0; // fixed base size — blur/streak passes do the rest of the spreading
    } else {
        SatVisible sat = satellites[gl_VertexIndex];
        skyDir    = sat.skyDir;
        intensity = sat.flareIntensity;
        baseColor = sat.baseColor;
        angSize   = sat.angularSize;
    }

    vec3 cam = (pc.skyView * vec4(skyDir, 0.0)).xyz;

    if (intensity <= 0.0 || cam.z >= -0.001) {
        gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
        gl_PointSize = 0.001;
        fragColor     = vec3(0.0);
        fragIntensity = 0.0;
        return;
    }

    float tanHalfFov = tan(pc.fovYRad * 0.5);
    float ndcX =  cam.x / (-cam.z) / (tanHalfFov * pc.aspect);
    float ndcY = -cam.y / (-cam.z) /  tanHalfFov;

    gl_Position  = vec4(ndcX, ndcY, 0.5, 1.0);
    // Scale down to this pass's own (smaller) target resolution — the blur/streak passes add the
    // rest of the softness; without this scale, points would look proportionally oversized once
    // the composite draw upsamples the result back to full resolution.
    gl_PointSize = max(angSize * pc.resScale, 1.0);

    fragColor     = baseColor;
    fragIntensity = intensity;
}
