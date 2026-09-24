#pragma once
// Satellite mesh renderer (lighting overhaul Phase 4, .plans/SAT_RENDERER_PHASE4.md).
//
// Owns every geometry model's render mesh (SatMesh.h) in one vertex/index buffer, their materials
// and occluders, and the pipelines that draw them. 4b: the model viewer — an offscreen target the
// UI shows through UIImage. 4c adds the scene pass (HDR radiance + distance into the shared depth).
//
// Frame of reference: Earth-fixed ECEF AXES, origin chosen by the caller (the viewer uses the
// satellite's body origin), so every position the GPU sees is small; see sat_mesh_common.glsl.
#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include "SatMesh.h"

#include <vector>

struct VulkanContext;

// std140 mirror of sat_mesh_common.glsl's MeshFrame.
struct GpuMeshFrame
{
    glm::mat4 viewProj;
    glm::mat4 invViewProj;
    glm::vec4 camPos;      // xyz, w = exposure
    glm::vec4 sunDir;      // xyz, w = draw the sun disc (background)
    glm::vec4 moonDir;     // xyz, w = moonlight irradiance (fraction of sunlight)
    glm::vec4 earthCenter; // xyz, w = Earth rotation angle (cloud drift)
    glm::vec4 params;      // x = self-shadows, y = reflections, z = procedural detail,
                           // w = output mode: 0 viewer (tonemapped), 1 photometric check, 2 scene (HDR + distance)
};
static_assert(sizeof(GpuMeshFrame) == 208, "GpuMeshFrame layout (sat_mesh_common.glsl)");

// std430 mirror of sat_mesh_common.glsl's MeshInstance.
struct GpuMeshInstance
{
    glm::vec4 origin;     // xyz body origin, w = fade
    glm::vec4 rot[12];    // group g column c = rot[g*3+c]
    glm::vec4 trans[4];
    glm::vec4 sun;        // xyz, w = litFactor
    glm::vec4 sunColor;   // rgb, w = Earth's α² (earthshine lobe width)
    glm::vec4 earthshine; // xyz, w = irradiance (fraction of sunlight)
    uint32_t firstMaterial;
    uint32_t firstOccluder;
    uint32_t occluderCount;
    uint32_t pad;
};
static_assert(sizeof(GpuMeshInstance) == 336, "GpuMeshInstance layout (sat_mesh_common.glsl)");

class SatMeshRenderer
{
public:
    struct EarthTextures
    {
        VkImageView day = VK_NULL_HANDLE, night = VK_NULL_HANDLE, clouds = VK_NULL_HANDLE;
        VkSampler daySampler = VK_NULL_HANDLE, nightSampler = VK_NULL_HANDLE, cloudsSampler = VK_NULL_HANDLE;
    };
    // One entry per satellite type; `model` null = no geometry (legacy type).
    struct TypeModel
    {
        const SatModel *model = nullptr;
        const SatOcclusion *occlusion = nullptr;
    };
    struct TypeMesh
    {
        bool valid = false;
        uint32_t firstIndex = 0, indexCount = 0;
        int32_t vertexOffset = 0;
        uint32_t firstMaterial = 0, firstOccluder = 0, occluderCount = 0;
        glm::vec3 boundsCenter{0.0f}; // rest pose
        float boundsRadius = 0.0f;
        int triangles = 0;
    };

    void init(VulkanContext &ctx, const EarthTextures &earth);
    void cleanup(VkDevice device);
    // Builds every type's render mesh and uploads the shared buffers. Call once the roster is loaded.
    void setTypeModels(VulkanContext &ctx, const std::vector<TypeModel> &types);
    const TypeMesh *typeMesh(int typeIdx) const
    {
        return (typeIdx >= 0 && typeIdx < (int)typeMeshes.size() && typeMeshes[typeIdx].valid)
                   ? &typeMeshes[typeIdx] : nullptr;
    }

    // ── Model viewer target ─────────────────────────────────────────────────────
    // (Re)creates the offscreen target at w×h if the size changed; returns true if it did (the
    // caller then re-points its UIImage). The image is SHADER_READ_ONLY after recordViewer().
    bool ensureViewerTarget(VulkanContext &ctx, uint32_t w, uint32_t h);
    VkImageView viewerView() const { return viewerResolveView; }
    VkSampler viewerSampler() const { return sampler; }
    bool viewerRendered() const { return viewerHasContent; }
    // Records the viewer pass (outside any render pass): background, then `inst` of type `typeIdx`.
    void recordViewer(VkCommandBuffer cmd, const GpuMeshFrame &frame, const GpuMeshInstance &inst, int typeIdx);

    // ── Photometric check ─────────────────────────────────────────────────────────
    // Renders `inst` (frame.params.w = 1: sun only, scalar, L·d² per pixel) into a kCheckSize² R32F
    // target and copies it to host memory. After the frame that recorded it has completed (the next
    // buildUI), checkPixels() holds the image, row-major, top row first.
    static constexpr uint32_t kCheckSize = 512;
    void recordCheck(VkCommandBuffer cmd, const GpuMeshFrame &frame, const GpuMeshInstance &inst, int typeIdx);
    const float *checkPixels() const { return static_cast<const float *>(checkMapped); }

    // ── Scene pass (4c) ─────────────────────────────────────────────────────────────
    // Satellite meshes in the main view, rendered BEFORE scene_depth.comp into two full-swap-extent
    // storage-capable targets that the rest of the frame reads with imageLoad: RGBA32F pre-exposure
    // radiance (sat_sky.frag composites it as a surface, with the atmosphere in front of it) and R32F
    // TRUE distance from the camera (0 = no mesh; scene_depth.comp mins it into the shared depth).
    // Always recorded — with no instances it only clears, so every consumer can read it blind.
    bool ensureSceneTarget(VulkanContext &ctx, uint32_t w, uint32_t h); // true if (re)created
    VkImageView sceneColorView() const { return sceneColorViewH; }
    VkImageView sceneDistView() const { return sceneDistViewH; }
    // `insts[i]` is drawn with the mesh of `types[i]` (at most kMaxInstances - 2 of them).
    void recordScene(VkCommandBuffer cmd, const GpuMeshFrame &frame, const std::vector<GpuMeshInstance> &insts,
                     const std::vector<int> &types);
    // Mesh glints into the flare/bloom source (4c): a fullscreen additive draw, recorded INSIDE the
    // caller's flare-source render pass, turning the over-white part of the scene radiance (times
    // `exposure`, the sky's) into the same log-compressed glow the satellite sprites seed there.
    void createBloomPipeline(VulkanContext &ctx, VkRenderPass flareSourcePass);
    void recordBloom(VkCommandBuffer cmd, uint32_t targetW, uint32_t targetH, float exposure, float gain);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    EarthTextures earth_;

    // Shared geometry + per-type data (host-visible; small).
    VkBuffer vertexBuf = VK_NULL_HANDLE, indexBuf = VK_NULL_HANDLE;
    VkDeviceMemory vertexMem = VK_NULL_HANDLE, indexMem = VK_NULL_HANDLE;
    VkBuffer materialBuf = VK_NULL_HANDLE, occluderBuf = VK_NULL_HANDLE;
    VkDeviceMemory materialMem = VK_NULL_HANDLE, occluderMem = VK_NULL_HANDLE;
    std::vector<TypeMesh> typeMeshes;

    // Per-frame data (host-coherent, mapped).
    static constexpr uint32_t kMaxInstances = 4096;
    VkBuffer frameBuf = VK_NULL_HANDLE, instanceBuf = VK_NULL_HANDLE;
    VkDeviceMemory frameMem = VK_NULL_HANDLE, instanceMem = VK_NULL_HANDLE;
    void *frameMapped = nullptr, *instanceMapped = nullptr;
    VkBuffer checkFrameBuf = VK_NULL_HANDLE;
    VkDeviceMemory checkFrameMem = VK_NULL_HANDLE;
    void *checkFrameMapped = nullptr;

    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkDescriptorPool descPool = VK_NULL_HANDLE;
    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkDescriptorSet descSetCheck = VK_NULL_HANDLE; // same layout, its own frame UBO (checkFrameBuf)
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkSampler sampler = VK_NULL_HANDLE;

    // Viewer pass.
    VkRenderPass viewerPass = VK_NULL_HANDLE;
    VkPipeline viewerMeshPipe = VK_NULL_HANDLE, viewerBgPipe = VK_NULL_HANDLE;
    uint32_t viewerW = 0, viewerH = 0;
    VkImage viewerColorMs = VK_NULL_HANDLE, viewerDepthMs = VK_NULL_HANDLE, viewerResolve = VK_NULL_HANDLE;
    VkDeviceMemory viewerColorMsMem = VK_NULL_HANDLE, viewerDepthMsMem = VK_NULL_HANDLE,
                   viewerResolveMem = VK_NULL_HANDLE;
    VkImageView viewerColorMsView = VK_NULL_HANDLE, viewerDepthMsView = VK_NULL_HANDLE,
                viewerResolveView = VK_NULL_HANDLE;
    VkFramebuffer viewerFb = VK_NULL_HANDLE;
    bool viewerHasContent = false;

    // Photometric check pass (single-sampled, R32F → host).
    VkRenderPass checkPass = VK_NULL_HANDLE;
    VkPipeline checkMeshPipe = VK_NULL_HANDLE;
    VkImage checkColor = VK_NULL_HANDLE, checkDepth = VK_NULL_HANDLE;
    VkDeviceMemory checkColorMem = VK_NULL_HANDLE, checkDepthMem = VK_NULL_HANDLE;
    VkImageView checkColorView = VK_NULL_HANDLE, checkDepthView = VK_NULL_HANDLE;
    VkFramebuffer checkFb = VK_NULL_HANDLE;
    VkBuffer checkReadBuf = VK_NULL_HANDLE;
    VkDeviceMemory checkReadMem = VK_NULL_HANDLE;
    void *checkMapped = nullptr;
    void createCheckPass(VulkanContext &ctx);

    // Scene pass.
    VkRenderPass scenePass = VK_NULL_HANDLE;
    VkPipeline sceneMeshPipe = VK_NULL_HANDLE;
    uint32_t sceneW = 0, sceneH = 0;
    VkImage sceneColor = VK_NULL_HANDLE, sceneDist = VK_NULL_HANDLE, sceneDepth = VK_NULL_HANDLE;
    VkDeviceMemory sceneColorMem = VK_NULL_HANDLE, sceneDistMem = VK_NULL_HANDLE, sceneDepthMem = VK_NULL_HANDLE;
    VkImageView sceneColorViewH = VK_NULL_HANDLE, sceneDistViewH = VK_NULL_HANDLE, sceneDepthView = VK_NULL_HANDLE;
    VkFramebuffer sceneFb = VK_NULL_HANDLE;
    VkBuffer sceneFrameBuf = VK_NULL_HANDLE;
    VkDeviceMemory sceneFrameMem = VK_NULL_HANDLE;
    void *sceneFrameMapped = nullptr;
    VkDescriptorSet descSetScene = VK_NULL_HANDLE;
    void createScenePass(VulkanContext &ctx);
    void destroySceneTarget();

    // Bloom source.
    VkDescriptorSetLayout bloomDescLayout = VK_NULL_HANDLE;
    VkDescriptorPool bloomDescPool = VK_NULL_HANDLE;
    VkDescriptorSet bloomDescSet = VK_NULL_HANDLE;
    VkPipelineLayout bloomPipeLayout = VK_NULL_HANDLE;
    VkPipeline bloomPipe = VK_NULL_HANDLE;

    void createDescriptors(VulkanContext &ctx);
    void writeGeometryDescriptors();
    void createViewerPass(VulkanContext &ctx);
    void createPipelines(VulkanContext &ctx);
    void destroyViewerTarget();
    void destroyGeometry();
};
