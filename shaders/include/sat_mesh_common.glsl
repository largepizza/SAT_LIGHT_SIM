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
    vec4 params;      // x = self-shadows on, y = env reflections on, z = procedural detail on,
                      // w = 1: photometric-check output (sun only, scalar, L·d² — see sat_mesh.frag)
    vec4 marker0;     // model viewer background: the observer's world position, w = 1 to draw
    vec4 marker1;     // a ground-site mirror's current target, w = 1 to draw
    vec4 bgParams;    // x = 1: the HDR background (SatEnvProbes) is valid, y = marker radius px, z = viewport px
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
    float bloomScale;   // scene: bloom seed per unit of rendered luminance (mesh_bloom.frag)
    uint firstComponent; // into components[] (per-component joint pivots)
    uint probeSlot;      // environment probe lighting it (set 1 is that probe), 0xFFFFFFFF = none
    float glareNorm; // sprite effectFlare per unit of bloom seed (mesh_bloom.frag -> glare_find.comp)
    uint cpad2;
    vec4 earthX;   // earthshine SH frame: xyz = the Sun's side perpendicular to nadir, w = sh0
    vec4 earthZ;   // xyz = nadir, w = sh1
    vec4 earthShA; // sh2..sh5 (SatEarthLight — diffuse = max(SH(n), vector irradiance))
    vec4 earthShB; // sh6..sh9
    vec4 earthShC; // x = sh10
};

// Irradiance of the lit Earth on a plane with unit normal n (fraction of sunlight): the SH fit of
// the whole cap, floored at the exact vector value — SatEarthLight::diffuse() (SatModel.h).
float instEarthDiffuse(MeshInstance inst, vec3 n)
{
    vec3  ex = inst.earthX.xyz, ez = inst.earthZ.xyz, ey = cross(ez, ex);
    float x = dot(n, ex), y = dot(n, ey), z = dot(n, ez);
    float x2 = x * x, y2 = y * y, z2 = z * z;
    float s = inst.earthX.w + inst.earthZ.w * z + inst.earthShA.x * x + inst.earthShA.y * (3.0 * z2 - 1.0)
            + inst.earthShA.z * x * z + inst.earthShA.w * (x2 - y2)
            + inst.earthShB.x * (35.0 * z2 * z2 - 30.0 * z2 + 3.0) + inst.earthShB.y * x * z * (7.0 * z2 - 3.0)
            + inst.earthShB.z * (x2 - y2) * (7.0 * z2 - 1.0) + inst.earthShB.w * x * z * (x2 - 3.0 * y2)
            + inst.earthShC.x * (x2 * x2 - 6.0 * x2 * y2 + y2 * y2);
    return max(max(s, 0.0), inst.earthshine.w * max(dot(n, inst.earthshine.xyz), 0.0));
}
layout(set = 0, binding = 1, std430) readonly buffer MeshInstances { MeshInstance instances[]; };

struct MeshMaterial {
    vec3  color;
    float albedo;
    float f0;
    float roughness;
    uint  beckmann;
    uint  pattern;
    vec4  extra;
    vec4  lattice; // open lattice (truss): x = coverage (1 = solid), y = bay pitch (m), z = member width (bays)
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

// Per component: xyz = joint pivot (rest frame, relative to its group's hinge; 0 = the hinge),
// w = its group's parent (−1: root — never pivoted). SatComponent::pivot, satPivotOffset().
layout(set = 0, binding = 7, std430) readonly buffer MeshComponents { vec4 components[]; };

layout(set = 0, binding = 4) uniform sampler2D earthDayTex;
layout(set = 0, binding = 5) uniform sampler2D earthNightTex;
layout(set = 0, binding = 6) uniform sampler2D earthCloudsTex;

mat3 instGroupRot(MeshInstance inst, uint g)
{
    return mat3(inst.rot[g * 3u].xyz, inst.rot[g * 3u + 1u].xyz, inst.rot[g * 3u + 2u].xyz);
}

// A component turned about its own pivot by its group's joint: its posed position gains
// (R_parent − R_group)·pivot (satPivotOffset, SatModel.cpp).
vec3 instPivotOffset(MeshInstance inst, uint g, uint comp)
{
    vec4 c = components[inst.firstComponent + comp];
    if (c.w < 0.0 || c.xyz == vec3(0.0)) return vec3(0.0);
    return (instGroupRot(inst, uint(c.w)) - instGroupRot(inst, g)) * c.xyz;
}

#endif // SATLIGHTSIM_SAT_MESH_COMMON_GLSL
