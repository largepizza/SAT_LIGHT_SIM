#ifndef SATLIGHTSIM_SAT_MESH_COMMON_GLSL
#define SATLIGHTSIM_SAT_MESH_COMMON_GLSL
// Descriptor set of the satellite mesh pipelines (sat_mesh.vert/.frag, sat_mesh_bg.frag) — Phase 4.
// Mirrors SatMeshRenderer.h (GpuMeshFrame, GpuMeshInstance) and SatMesh.h (GpuSatMeshMaterial,
// GpuSatMeshOccluder). Frame of reference ("world"): Earth-fixed ECEF AXES with its origin wherever
// the CPU put it (the viewer: the satellite's body origin; the scene: the camera) — every position
// here is small, so float32 is exact enough; Earth's centre is `frame.earthCenter`.

layout(set = 0, binding = 0, std140) uniform MeshFrame {
    mat4 viewProj;
    mat4 invViewProj;
    vec4 camPos;      // xyz world, w = exposure (viewer: output = 1 − exp(−exposure·L))
    vec4 sunDir;      // xyz unit (ECEF axes), w = 1 if the sun disc is drawn in the background
    vec4 moonDir;     // xyz unit, w = moonlight irradiance (fraction of sunlight)
    vec4 earthCenter; // xyz world position of Earth's centre, w = Earth rotation angle (cloud drift)
    vec4 params;      // x = self-shadows on, y = env reflections on, z = env lod bias,
                      // w = 1: photometric-check output (sun only, scalar, L·d² — see sat_mesh.frag)
} frame;

struct MeshInstance {
    vec4 origin;      // xyz world position of the body origin, w = fade (1 = opaque)
    vec4 rot[12];     // group g's rotation, column c = rot[g * 3 + c].xyz (posed = R·rest + t)
    vec4 trans[4];    // group g's translation (xyz)
    vec4 sun;         // xyz unit sun direction, w = litFactor (Earth-shadow, 0..1)
    vec4 sunColor;    // rgb transmitted sunlight tint, w = Earth's angular α² for the earthshine lobe
    vec4 earthshine;  // xyz unit direction of the effective earthshine source, w = irradiance (× sun)
    uint firstMaterial;
    uint firstOccluder;
    uint occluderCount;
    uint instPad;
};
layout(set = 0, binding = 1, std430) readonly buffer MeshInstances { MeshInstance instances[]; };

struct MeshMaterial {
    vec3  color;
    float albedo;
    float f0;
    float roughness;
    uint  beckmann;
    uint  pattern;
    vec4  extra;
};
layout(set = 0, binding = 2, std430) readonly buffer MeshMaterials { MeshMaterial materials[]; };

struct MeshOccluder {
    vec3 center;
    uint kind;
    vec3 halfExt;
    uint group;
    vec4 axisX;
    vec4 axisY;
    vec4 axisZ;
};
layout(set = 0, binding = 3, std430) readonly buffer MeshOccluders { MeshOccluder occluders[]; };

layout(set = 0, binding = 4) uniform sampler2D earthDayTex;
layout(set = 0, binding = 5) uniform sampler2D earthNightTex;
layout(set = 0, binding = 6) uniform sampler2D earthCloudsTex;

mat3 instGroupRot(MeshInstance inst, uint g)
{
    return mat3(inst.rot[g * 3u].xyz, inst.rot[g * 3u + 1u].xyz, inst.rot[g * 3u + 2u].xyz);
}

#endif // SATLIGHTSIM_SAT_MESH_COMMON_GLSL
