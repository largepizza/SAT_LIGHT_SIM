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
    float visPad;
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
} pc;

layout(location = 0) out vec3  fragColor;
layout(location = 1) out float fragIntensity;
layout(location = 2) out float fragAngSize;  // sprite size in pixels, for abs-pixel glow
layout(location = 3) out float fragRangeM;   // for the manual scene-depth test (trails, renderScale<1)

void main() {
    SatVisible sat = satellites[gl_VertexIndex];

    // ── Project ENU sky direction through the camera ──────────────────────────
    // skyView transforms a direction (w=0) from ENU to camera space.
    // In camera space: +X=right, +Y=up, -Z=forward (satellite in front → cam.z < 0).
    vec3 cam = (pc.skyView * vec4(sat.skyDir, 0.0)).xyz;

    // Invisible (below horizon / shadow / below threshold): clip before rasterization.
    if (sat.flareIntensity <= 0.0) {
        gl_Position  = vec4(0.0, 0.0, 2.0, 1.0);
        gl_PointSize = 0.001;
        fragColor     = vec3(0.0);
        fragIntensity = 0.0;
        fragAngSize   = 0.001;
        fragRangeM    = 0.0;
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
    gl_Position  = vec4(ndcX, ndcY, sceneDepthFromDistance(sat.rangeM), 1.0);
    gl_PointSize = sat.angularSize;  // sized by compute shader already

    fragColor     = unpackUnorm4x8(sat.color).rgb;
    fragRangeM    = sat.rangeM;
    fragIntensity = sat.flareIntensity;
    fragAngSize   = sat.angularSize;
}
