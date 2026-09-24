#pragma once
// Render meshes for geometry models (lighting overhaul Phase 4, .plans/SAT_RENDERER_PHASE4.md).
//
// A separate tessellation from tessellateSatModel(), which feeds the photometric lobe bake: this
// one is for drawing — finer on curved primitives, SMOOTH normals on cylinders/cones/spheres (flat
// on planes/boxes), and UVs in metres along the surface so procedural patterns (solar-cell grid,
// panel seams) keep their physical size on any component. Same REST pose and body frame as
// tessellateSatModel(), so the poses from evalGroupPoses() place it (posed = R·rest + t) and the
// occluders from buildSatOcclusion() line up with it.
#include "SatModel.h"

#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

struct SatMeshVertex
{
    glm::vec3 pos;    // rest pose, body frame, metres
    glm::vec3 normal; // unit, outward
    glm::vec2 uv;     // metres along the surface
    uint32_t packed;  // group (bits 0-7) | material (8-19, model-local) | component (20-31)
};
static_assert(sizeof(SatMeshVertex) == 36, "SatMeshVertex layout (sat_mesh.vert inputs)");

inline uint32_t packSatMeshVertexIds(int group, int material, int component)
{
    return (uint32_t)(group & 0xFF) | ((uint32_t)(material & 0xFFF) << 8) | ((uint32_t)(component & 0xFFF) << 20);
}

struct SatRenderMesh
{
    std::vector<SatMeshVertex> vertices;
    std::vector<uint32_t> indices; // triangle list, counter-clockwise seen from outside
    glm::vec3 boundsCenter{0.0f};  // rest pose bounding sphere
    float boundsRadius = 0.0f;
};

// `segments` = facets around a revolved primitive (and sphere longitude); latitude uses half.
SatRenderMesh buildSatRenderMesh(const SatModel &m, int segments = 48);

// Material as the mesh shader reads it (std430, 48 bytes; sat_mesh.frag `MeshMaterial`).
struct GpuSatMeshMaterial
{
    glm::vec3 color;      // diffuse tint (and specular tint for metals: F0 >= 0.5)
    float albedo;         // Lambertian ρd
    float f0;             // Schlick F0
    float roughness;      // microfacet α
    uint32_t beckmann;    // 1 = Beckmann distribution, else GGX (as the photometry)
    uint32_t pattern;     // procedural surface pattern (0 = none) — 4b procedural detail
    glm::vec4 extra;      // rgb = transmission tint (luminance 1), a = transmission (Phase 4f)
};
static_assert(sizeof(GpuSatMeshMaterial) == 48, "GpuSatMeshMaterial layout");
GpuSatMeshMaterial packSatMeshMaterial(const SatMaterial &m);

// Occluder as the mesh shader reads it (std430, 80 bytes; sat_mesh.frag `MeshOccluder`): the same
// primitive as SatOccluder in the REST body frame (not the root-triad frame of GpuSatOccluder), so
// the shader poses it with the instance's group transform. Occluder i is component i.
struct GpuSatMeshOccluder
{
    glm::vec3 center;
    uint32_t kind;  // PrimitiveKind
    glm::vec3 half; // SatOccluder::half
    uint32_t group;
    glm::vec4 axisX; // the component's local axes in the rest frame (xyz)
    glm::vec4 axisY;
    glm::vec4 axisZ;
};
static_assert(sizeof(GpuSatMeshOccluder) == 80, "GpuSatMeshOccluder layout");
std::vector<GpuSatMeshOccluder> packSatMeshOccluders(const SatOcclusion &occ);
