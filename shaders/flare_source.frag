#version 450

// ── flare_source.frag ──────────────────────────────────────────────────────────
// Soft circular falloff per point — the blur/streak compute passes (flare_blur.comp) add the rest
// of the corona's softness and the godray shafts, so this only needs a single Gaussian, not
// sat_point.frag's inner/outer dual-term split. Cloud + terrain occlusion are tested here, once
// per source, reusing the exact techniques already established elsewhere in this codebase:
//   - cloud: same hard tCloudOcclude gate + soft A_total dim sat_point.frag already uses.
//   - terrain: a plain sceneDepthTex hit-test, same as the (now-deleted) corona loop and the sun
//     disc/corona block used — this pass has no shared hardware depth buffer of its own to rely
//     on (unlike sat_point.frag, which piggybacks on the main render pass's depth attachment), so
//     it has to test explicitly.

layout(location = 0) in vec3  fragColor;
layout(location = 1) in float fragIntensity;

layout(location = 0) out vec4 outColor;

layout(set = 0, binding = 5) uniform sampler2D cloudTargetA;  // a = tCloudOcclude (>=0 => >=90% opaque)
layout(set = 0, binding = 6) uniform sampler2D cloudTargetB;  // rgb = A_total (cloud transmittance)
layout(set = 0, binding = 7) uniform sampler2D sceneDepthTex; // shared terrain/ocean depth

layout(push_constant) uniform PC {
    mat4  skyView;          // unused here
    float fovYRad;          // unused here
    float aspect;           // unused here
    uint  satCount;         // unused here
    float sunRefIntensity;  // unused here
    vec4  sunDirENU;        // unused here
    vec2  screenSizePx;     // this pass's own (small) target size
    float resScale;         // unused here
    float pad0;
} pc;

const float kNoSurfaceT = 1e30; // mirrors common.glsl's constant — this tiny shader skips the
                                 // #include machinery for a single value

void main() {
    vec2  c = gl_PointCoord - 0.5;
    float d = length(c);
    if (d > 0.5) discard;

    vec2 uv = gl_FragCoord.xy / pc.screenSizePx;
    vec4 cloudA = texture(cloudTargetA, uv);
    vec4 cloudB = texture(cloudTargetB, uv);
    float tCloudOcclude = cloudA.a;
    float cloudBlockV   = dot(cloudB.rgb, vec3(1.0 / 3.0));
    float cloudHardOcclude = (tCloudOcclude >= 0.0) ? 0.0 : 1.0;
    float cloudVis = cloudHardOcclude * clamp(cloudBlockV, 0.0, 1.0);

    float terrainVis = (texture(sceneDepthTex, uv).r >= kNoSurfaceT * 0.5) ? 1.0 : 0.0;

    // Single soft Gaussian — deliberately wider than sat_point.frag's inner core (this IS the
    // corona seed the blur passes spread further, not the crisp point itself).
    const float sigma = 0.28;
    float g = exp(-(d * d) / (2.0 * sigma * sigma));
    float brightness = g * clamp(log2(max(fragIntensity, 1.0)) * 0.5, 0.0, 4.0) * cloudVis * terrainVis;

    outColor = vec4(fragColor * brightness, brightness);
}
