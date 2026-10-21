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
    float glarePoint; // 1 = point-like on screen (all its light may glare), 0 = resolved (mesh_bloom.frag)
    vec4 earthX;   // earthshine SH frame: xyz = the Sun's side perpendicular to nadir, w = sh0
    vec4 earthZ;   // xyz = nadir, w = sh1
    vec4 earthShA; // sh2..sh5 (SatEarthLight — diffuse = max(SH(n), vector irradiance))
    vec4 earthShB; // sh6..sh9
    vec4 earthShC; // x = sh10, y = sharp reflections, z = 1 - unfolding, w = a burning stage's engine glow
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

// Per component, FIVE vec4s (2026-10-20; firstComponent counts vec4s): [5c] xyz = joint pivot (rest frame, relative
// to its group's hinge; 0 = the hinge), w = its group's parent (−1: root — never pivoted). SatComponent::pivot,
// satPivotOffset(). [5c + 1] xyz = the stow axis x (1 - stowScale) (0 = never stowed), w = the stow root;
// [5c + 2] xy = the stow window, zw = the fold window (SatComponent::stow* / fold*; the instance's earthShC.z =
// 1 - unfolding progress); [5c + 3] xyz = the fold hinge point, w = the fold angle (radians, 0 = never folded);
// [5c + 4] xyz = the fold axis, w = the part's build-date code (stations module by module, 2026-10-21: satSinceCode,
// SatModel.h; 0 = always). The instance's trans[0].w = today's code (0 = every part: no date / the era off).
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
// components[].w = the parent group (-1 = root), + 100 for a RENDER-ONLY part (SatMeshRenderer::setTypeModels).
vec3 instPivotOffset(MeshInstance inst, uint g, uint comp)
{
    vec4 c = components[inst.firstComponent + 5u * comp];
    float parent = c.w >= 50.0 ? c.w - 100.0 : c.w;
    if (parent < 0.0 || c.xyz == vec3(0.0)) return vec3(0.0);
    return (instGroupRot(inst, uint(parent)) - instGroupRot(inst, g)) * c.xyz;
}

// A render-only part (greebles: trusses, gimbals, fittings, terminals) is drawn but absent from the
// photometric model, so none of its light may seed the bloom or the glare, which are normalised by that
// model's intensity (2026-10-06: a sunlit aluminium fitting on a Starmind satellite seeded bloom discs
// hundreds of pixels wide — its light had no share of the model's flux to be scaled against).
bool instCompRenderOnly(MeshInstance inst, uint comp)
{
    return components[inst.firstComponent + 5u * comp].w >= 50.0;
}

// A stowed part (launches, 2026-10-19: an accordion-folded array or radiator while the satellite unfolds): its scale
// along the stow axis at the instance's unfolding progress (1 = deployed; satComponentStowScale, SatModel.cpp), and
// that axis. Returns 1 for a part that is never stowed or a deployed satellite.
float instStowScale(MeshInstance inst, uint comp, out vec3 axis, out float root)
{
    axis = vec3(0.0);
    root = 0.0;
    if (inst.earthShC.z <= 0.0) return 1.0;
    vec4 st = components[inst.firstComponent + 5u * comp + 1u];
    float ls = length(st.xyz);
    if (ls <= 0.0) return 1.0;
    vec4 ph = components[inst.firstComponent + 5u * comp + 2u];
    axis = st.xyz / ls;
    root = st.w;
    return mix(1.0 - ls, 1.0, smoothstep(ph.x, ph.y, 1.0 - inst.earthShC.z));
}

// A folded part (launches, 2026-10-20: an array folded flat onto the bus, half a bus folded over the other): its fold
// angle at the instance's unfolding progress (0 = deployed or never folded; satComponentFoldAngle, SatModel.cpp), the
// hinge point and axis (rest body frame).
float instFoldAngle(MeshInstance inst, uint comp, out vec3 point, out vec3 axis)
{
    point = vec3(0.0);
    axis = vec3(0.0, 0.0, 1.0);
    if (inst.earthShC.z <= 0.0) return 0.0;
    vec4 fp = components[inst.firstComponent + 5u * comp + 3u];
    if (fp.w == 0.0) return 0.0;
    vec4 ph = components[inst.firstComponent + 5u * comp + 2u];
    point = fp.xyz;
    axis = components[inst.firstComponent + 5u * comp + 4u].xyz;
    return fp.w * (1.0 - smoothstep(ph.z, ph.w, 1.0 - inst.earthShC.z));
}

// A part not built yet on the instance's date (a station built module by module, 2026-10-21): not drawn (sat_mesh.vert
// collapses it) and casts no shadow.
bool instCompAbsent(MeshInstance inst, uint comp)
{
    if (inst.trans[0].w <= 0.0) return false;
    float code = components[inst.firstComponent + 5u * comp + 4u].w;
    return code > 0.0 && code > inst.trans[0].w;
}

// Rodrigues: v turned by angle a about the unit axis k.
vec3 rotateAbout(vec3 v, vec3 k, float a)
{
    float c = cos(a), s = sin(a);
    return v * c + cross(k, v) * s + k * (dot(k, v) * (1.0 - c));
}

#endif // SATLIGHTSIM_SAT_MESH_COMMON_GLSL

// The scene mesh target's alpha = instance slot + 1 + a packed fraction (2026-10-19): the photometric share of the
// pixel's light (what seeds the bloom, 64 levels) and whether its reflection may glare (meshGlareRoughK, 8 levels).
// Readers: mesh_bloom.frag, mesh_refl_add.comp (keep their copies of the unpack in step).
float meshGlareRoughK(float rough) { return 1.0 - smoothstep(0.04, 0.10, rough); }
float meshSharePack(float photFrac, float glareK)
{
    return (floor(clamp(photFrac, 0.0, 1.0) * 63.99) * 8.0 + floor(clamp(glareK, 0.0, 1.0) * 7.99)) / 512.0;
}
