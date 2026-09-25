// ── glint_list.glsl — mesh glints found for glare (GpuGlintList, SatelliteSim.h) ───────────────
// glare_find.comp appends; glare_mesh.vert draws one glare sprite per entry, vkCmdDrawIndirect'd on
// the header's own VkDrawIndirectCommand (vertexCount = entries written).
// GLINT_LIST_ACCESS: `readonly` in the vertex stage (writable storage there needs
// vertexPipelineStoresAndAtomics, which this app does not enable).
#ifndef GLINT_LIST_ACCESS
#define GLINT_LIST_ACCESS
#endif
const uint kMaxGlints = 64u;
layout(set = 0, binding = GLINT_LIST_BINDING, std430) GLINT_LIST_ACCESS buffer GlintList {
    uint count;                                              // append counter (may pass kMaxGlints)
    uint vertexCount, instanceCount, firstVertex, firstInstance; // VkDrawIndirectCommand
    uint pad0, pad1, pad2;
    vec4 glintPos[kMaxGlints];   // xy = screen uv, z = the glint's light in effectFlare units
    vec4 glintColor[kMaxGlints]; // rgb = tint (max channel 1)
} glints;
