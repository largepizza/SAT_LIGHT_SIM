#version 450

#include "depth.glsl"

// ── SSBO: written by sat_flare.comp, read here via gl_VertexIndex ─────────────
// Since Phase 1b this is the COMPACT visible list (drawn with vkCmdDrawIndirect): gl_VertexIndex is
// a list slot, not a satellite index, and slot order changes frame to frame — never key anything
// per-satellite (hashes, twinkle seeds) off it.
struct SatVisible {
    vec3  skyDir;         // unit vector in local ENU (x=East, y=North, z=Up)
    float flareIntensity; // [0, 1+]
    uint  color;          // satellite tint, packUnorm4x8
    float angularSize;    // point sprite size (pixels)
    float rangeM;         // distance to the object, m; 0 = at infinity (stars, planets)
    float meshPx;         // pre-photometry: model mesh diameter on screen (px), 0 = none (Phase 4d)
};
layout(set = 0, binding = 1) readonly buffer SatVisibleBuf {
    SatVisible satellites[];
};

// ── Camera push constants ─────────────────────────────────────────────────────
// Declares only the fields this shader reads (a prefix of PointDrawPC, 128 bytes — see
// SatelliteSim.h). skyView/fovYRad/aspect are at the same offsets they were in the old shared
// SatDrawPC, so nothing here changes.
layout(push_constant) uniform PC {
    mat4  skyView;
    float fovYRad;
    float aspect;
    float waveTime, noTwinkle;       // (unread) offsets 72/76
    vec4  moonDirENU, obsECEFDir;    // (unread) 80, 96
    vec2  screenSizePx;              // (unread) 112
    uint  debugDisableMask;          // (unread) 120
    float manualTerrainTest;         // 124: 2 on the trail draws, 1 on the live draw at renderScale < 1
} pc;

layout(location = 0) out vec3  fragColor;
layout(location = 1) out float fragIntensity;
layout(location = 2) out float fragAngSize;  // sprite size in pixels, for abs-pixel glow
layout(location = 3) out float fragRangeM;   // for the manual scene-depth test (trails, renderScale<1)
layout(location = 4) flat out float fragManualDepth; // 1: a city light, tested in the fragment shader only

void main() {
    SatVisible sat = satellites[gl_VertexIndex];

    // ── Project ENU sky direction through the camera ──────────────────────────
    // skyView transforms a direction (w=0) from ENU to camera space.
    // In camera space: +X=right, +Y=up, -Z=forward (satellite in front → cam.z < 0).
    vec3 cam = (pc.skyView * vec4(sat.skyDir, 0.0)).xyz;

    // Invisible (below horizon / shadow / below threshold): clip before rasterization. City lights
    // (city_sprites.comp, meshPx = -1) stay out of the trails: they would streak with the camera.
    if (sat.flareIntensity <= 0.0 || (pc.manualTerrainTest > 1.5 && sat.meshPx < -0.5)) {
        gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
        gl_PointSize = 0.001;
        fragColor     = vec3(0.0);
        fragIntensity = 0.0;
        fragAngSize   = 0.001;
        fragRangeM    = 0.0;
        fragManualDepth = 0.0;
        return;
    }

    // Satellite behind camera: push outside clip volume so hardware discards it.
    if (cam.z >= -0.001) {
        gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
        gl_PointSize = 0.001;
        fragColor     = vec3(0.0);
        fragIntensity = 0.0;
        fragAngSize   = 0.001;
        fragRangeM    = 0.0;
        fragManualDepth = 0.0;
        return;
    }

    // Perspective projection.
    // tanHalfFov = tan(fovY/2). NDC x range [-1,1] corresponds to fovX,
    // NDC y range [-1,1] corresponds to fovY.
    // Vulkan Y is down, so we negate cam.y.
    float tanHalfFov = tan(pc.fovYRad * 0.5);
    float ndcX =  cam.x / (-cam.z) / (tanHalfFov * pc.aspect);
    float ndcY = -cam.y / (-cam.z) /  tanHalfFov;

    // Depth = the satellite's true range in the unified encoding (include/depth.glsl), so it is
    // hidden by any nearer surface (terrain, ocean, opaque cloud, a mesh) and by nothing farther.
    // A city light (meshPx -1) sits a few metres over the ground: its depth is taken at 0.95 of its range, as
    // the manual test below does. With the sky TAA the restored ground depth is the jittered sample's, and at
    // grazing angles a sub-pixel jitter moves it by ~0.5-2% of the distance: the lights z-fought it, blinking
    // from frame to frame (user snapshots 1-2, 2026-10-04).
    // 2026-10-06: a city light is NOT tested against the hardware depth at all. Under the sky TAA that depth is
    // restored from this frame's JITTERED sample, and at a crest seen at a low angle (a ridge in front of a far
    // city) a pixel's sample hit the ridge in some of the 16 jitter phases and the ground beyond in others: a
    // light just behind the crest was hidden or drawn whole by turns, ~240 vs ~37 levels every frame (the r35
    // Irvine horizon's worst pixels were all sprites; dilating the restored depth only moved them). It takes
    // depth 0 (always passes; points write no depth) and the fragment shader tests it against the unjittered
    // half-res scene depth instead, as the renderScale < 1 path always did.
    const bool cityLight = sat.meshPx < -0.5;
    gl_Position  = vec4(ndcX, ndcY, cityLight ? 0.0 : sceneDepthFromDistance(sat.rangeM), 1.0);
    fragManualDepth = cityLight ? 1.0 : 0.0;
    gl_PointSize = sat.angularSize;  // sized by compute shader already

    fragColor     = unpackUnorm4x8(sat.color).rgb;
    // A city light (city_sprites.comp) stands a few metres above the ground it lies on; against the
    // half-res, filtered scene depth of the manual test (renderScale < 1: the Medium preset) the ground
    // around it read nearer and hid most of them. 5% of the range: only real relief in front hides one.
    fragRangeM    = sat.meshPx < -0.5 ? 0.95 * sat.rangeM : sat.rangeM;
    fragIntensity = sat.flareIntensity;
    fragAngSize   = sat.angularSize;
}
