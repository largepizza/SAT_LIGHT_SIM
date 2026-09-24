#version 450
// Satellite mesh (lighting overhaul Phase 4). One instance per gl_InstanceIndex (the draw's
// firstInstance selects it); each vertex is posed by its attitude group: world = origin + R·rest + t.

#include "sat_mesh_common.glsl"

layout(location = 0) in vec3 inPos;     // rest pose, body frame (m)
layout(location = 1) in vec3 inNormal;
layout(location = 2) in vec2 inUv;      // metres along the surface
layout(location = 3) in uint inPacked;  // group | material << 8 | component << 20

layout(location = 0) out vec3 vWorld;
layout(location = 1) out vec3 vNormal;
layout(location = 2) out vec2 vUv;
layout(location = 3) flat out uint vMaterial;  // global material index
layout(location = 4) flat out uint vComponent;
layout(location = 5) flat out uint vInstance;
layout(location = 6) out vec3 vRest;         // rest-pose body position (procedural patterns)
layout(location = 7) flat out uint vGroup;

void main()
{
    MeshInstance inst = instances[gl_InstanceIndex];
    uint g = inPacked & 0xFFu;
    mat3 R = instGroupRot(inst, g);
    uint comp = (inPacked >> 20) & 0xFFFu;
    vWorld  = inst.origin.xyz + R * inPos + inst.trans[g].xyz + instPivotOffset(inst, g, comp);
    vNormal = R * inNormal;
    vUv     = inUv;
    vMaterial  = inst.firstMaterial + ((inPacked >> 8) & 0xFFFu);
    vComponent = (inPacked >> 20) & 0xFFFu;
    vInstance  = uint(gl_InstanceIndex);
    vRest      = inPos;
    vGroup     = g;
    gl_Position = frame.viewProj * vec4(vWorld, 1.0);
}
