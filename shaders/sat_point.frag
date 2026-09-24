// ── sat_point.frag ──────────────────────────────────────────────────────────────
// The satellite's point of light. Its appearance comes from its apparent magnitude through the
// shared point-source model (point_style.glsl) — the same one stars and planets use, so equal
// magnitudes look equal (until 2026-09-23 satellites had their own curve: a mag-6 satellite drew
// like a mag-4.5 star). The soft glow and godray corona of bright points is a separate render-to-
// texture pipeline (flare_source.vert/.frag, composited in flare_composite.frag).
//
// Output is (rgb*brightness, brightness) for additive blend accumulation.
// ─────────────────────────────────────────────────────────────────────────────

#version 450

#define POINT_STYLE_BINDING 11 // pointStyleBuf, descSet (shared with sat_flare.comp)
#include "point_style.glsl"

layout(location = 0) in vec3  fragColor;
layout(location = 1) in float fragIntensity;
layout(location = 2) in float fragAngSize;
layout(location = 3) in float fragRangeM;

layout(location = 0) out vec4 outColor;

// ── Cloud occlusion (C12 follow-up #33) ───────────────────────────────────────
// Same half-res cloud composite targets sat_sky.frag reads (bindings 10/11 there; 5/6 here —
// this pipeline's own descriptor set, see createDescriptors()/createDrawPipeline() in
// SatelliteSim.cpp). Previously satellites (including mirror lens flares) had NO cloud
// awareness at all — this is the first time this pipeline reads cloud data.
layout(set = 0, binding = 5) uniform sampler2D cloudTargetA; // a = tCloudOcclude (>=0 => >=90% opaque)
layout(set = 0, binding = 6) uniform sampler2D cloudTargetB; // rgb = A_total (cloud transmittance)
// Shared terrain/ocean depth (flare architecture overhaul added this binding for
// flare_source.frag's own terrain test — already valid here, no descriptor changes needed).
// Long-exposure trail follow-up: this pipeline's LIVE draw gets terrain occlusion for free from
// the main render pass's hardware depth test, so this binding is normally unused here — only the
// trail draw (no depth attachment of its own) sets pc.manualTerrainTest and pays for the fetch.
layout(set = 0, binding = 7) uniform sampler2D sceneDepthTex;

// Declares a prefix of PointDrawPC (128 bytes — see SatelliteSim.h), through the last field this
// shader reads. debugDisableMask, screenSizePx and manualTerrainTest were relocated here from the
// old shared SatDrawPC tail so both point pipeline layouts fit the 128-byte maxPushConstantsSize
// floor; the sky-only scalars that used to sit between them went to the CloudParams UBO instead.
layout(push_constant) uniform PC {
    mat4  skyView;            // offset 0   — unused here, declared for layout consistency
    float fovYRad;            // offset 64  — unused here
    float aspect;             // offset 68  — unused here
    float waveTime;           // offset 72  — unused here
    float noTwinkle;          // offset 76  — unused here (star_point.vert only)
    vec4  moonDirENU;         // offset 80  — unused here (star_point.vert only)
    vec4  obsECEFDir;         // offset 96  — unused here (star_point.vert only)
    vec2  screenSizePx;       // offset 112 — full-res target size for the cloud/depth UVs below
    uint  debugDisableMask;   // offset 120 — knockout bit 4096 (satellite point cloud occlusion)
    float manualTerrainTest;  // offset 124 — 1 = do the manual sceneDepthTex hit-test below
} pc;

void main() {
    // gl_PointCoord is [0,1] across the point sprite quad; c is centred at (0,0), d is the
    // distance from centre. Done FIRST so fragments in the quad's corners (~21% of them, outside
    // the inscribed circle) exit before paying for the cloud texture fetches below.
    vec2  c = gl_PointCoord - 0.5;
    float d = length(c);
    if (d > 0.5) discard;

    // Knockout bit 4096 (profiling-only, added 2026-08-09): this pipeline had no knockout bit at
    // all before now, unlike every other perf-sensitive block in the codebase (see CLAUDE.md's
    // "GPU Performance Profiling" subsystem) — added specifically to let a per-fragment cost
    // hypothesis (2 cloud texture fetches x every covered pixel of every satellite sprite, up to
    // 120px each, potentially many simultaneously near that cap for Reflect Orbital's near-perfect
    // mirror alignment) be confirmed or ruled out with real GPU FRAME BREAKDOWN data instead of
    // guessed at — see BEAM_CLOUD_PLAN.md's session 2026-08-09 log for the reported satellite/star
    // draw cost jump this is meant to isolate.
    float cloudVis = 1.0;
    if ((pc.debugDisableMask & 4096u) == 0u) {
        // gl_FragCoord.xy must divide by THIS draw's own target size, never an assumed constant —
        // see sat_sky.frag's documented render-scale gotcha (CLAUDE.md "Subsystem: Resolution
        // Scaling"). Satellites always draw at native resolution regardless of renderScale, but the
        // cloud targets are always sized off the true swap extent, so this is the correct divisor
        // in both cases (matches sat_sky.frag's own cloudUV formula exactly).
        vec2 cloudUV = gl_FragCoord.xy / pc.screenSizePx;
        vec4 cloudA  = texture(cloudTargetA, cloudUV);
        vec4 cloudB  = texture(cloudTargetB, cloudUV);
        float tCloudOcclude = cloudA.a;
        float cloudBlock    = dot(cloudB.rgb, vec3(1.0 / 3.0));
        // Two-tier response, mirroring the two ways sat_sky.frag already treats cloud opacity:
        // a hard gate for genuinely opaque cloud (same tCloudOcclude convention that hides the moon
        // disc), and a smooth power-curve dim otherwise (same shape the Milky Way/sun disc use), so
        // satellites join the same existing visual language instead of a new one.
        float cloudHardOcclude = (tCloudOcclude >= 0.0 && tCloudOcclude < fragRangeM) ? 0.0 : 1.0;
        const float kSatCloudSuppressPower = 2.0;
        cloudVis = cloudHardOcclude * pow(clamp(cloudBlock, 0.0, 1.0), kSatCloudSuppressPower);
    }

    // ── Terrain occlusion ─────────────────────────────────────────────────────
    // Set by (a) the long-exposure trail draw, whose offscreen render pass has no depth attachment,
    // and (b) the live draw at renderScale < 1.0, where the sky pass is a low-res prepass that never
    // writes the frame's depth buffer (see buildPointDrawPC). At renderScale 1.0 the live draw
    // instead gets this for free from the hardware depth test against sat_sky.frag's gl_FragDepth.
    //
    // Same rule as the hardware test (include/depth.glsl): a surface occludes the satellite only if
    // it is nearer than the satellite's own range. (Until Phase 4 both used a 150 km cap instead,
    // so that from orbit the distant Earth did not swallow the satellites in front of it.)
    float terrainVis = 1.0;
    if (pc.manualTerrainTest >= 0.5) {
        vec2 depthUV = gl_FragCoord.xy / pc.screenSizePx;
        terrainVis = (texture(sceneDepthTex, depthUV).r < fragRangeM) ? 0.0 : 1.0;
    }

    // ── Point spread from the apparent magnitude (point_style.glsl) ──────────────
    vec2  psf  = pointPsf(satFlareToMag(fragIntensity));
    float pixD = d * fragAngSize;
    float core = psf.x * exp(-pixD * pixD / (2.0 * psf.y * psf.y));

    float brightness = core * cloudVis * terrainVis;
    outColor = vec4(fragColor * brightness, brightness);
}
