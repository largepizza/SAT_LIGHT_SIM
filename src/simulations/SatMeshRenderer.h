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

#include <algorithm>
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
                           // w = output mode: 0 viewer (tonemapped), 1 photometric check, 2 scene (HDR + distance),
                           // 3 viewer glare source (check shading; L·d² and L)
    // Model viewer background only (sat_mesh_bg.frag): markers on the Earth, world positions
    // relative to the frame origin; w = 1 to draw. 0 = the observer, 1 = a mirror's ground site.
    glm::vec4 marker0;
    glm::vec4 marker1;
    glm::vec4 bgParams; // x = 1: the HDR background (SatEnvProbes) is valid, y = marker radius (px),
                        // z = viewport height (px)
};
static_assert(sizeof(GpuMeshFrame) == 256, "GpuMeshFrame layout (sat_mesh_common.glsl)");

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
    float bloomScale; // scene: bloom seed per unit of rendered luminance (energy-matched to the sprite)
    uint32_t firstComponent; // Phase 4f: into the per-component pivot buffer (binding 7)
    uint32_t probeSlot;      // environment probe (SatEnvProbes) lighting it, kNoProbe = none (the
                             // analytic earth_env.glsl reflection and the photometric earthshine)
    float glareNorm;         // the sprite's effectFlare per unit of bloom seed: mesh_bloom.frag writes a
                             // glint's light in effectFlare units for glare_find.comp (0 = no glare)
    float glarePoint;        // 1 while the mesh is small enough on screen to be a point (all its light
                             // may glare), 0 once resolved (only sun-like reflections glare)
    // Earthshine as a broad source (SatEarthLight, 2026-09-24): the SH plane-irradiance fit and its
    // frame, world axes. Diffuse light = max(SH(n), earthshine.w·(n·earthshine.xyz)₊).
    glm::vec4 earthX;   // xyz = the Sun's side ⟂ nadir, w = sh0
    glm::vec4 earthZ;   // xyz = nadir, w = sh1
    glm::vec4 earthShA; // sh2..sh5
    glm::vec4 earthShB; // sh6..sh9
    glm::vec4 earthShC; // x = sh10
};
static_assert(sizeof(GpuMeshInstance) == 432, "GpuMeshInstance layout (sat_mesh_common.glsl; mesh_bloom.frag mirrors the stride)");
struct SatEarthLight;
// Fills the instance's earthshine fields (earthshine, earthX/Z/Sh) from `L`, turning its world
// vectors with `rot` (e.g. ECI → the renderer's ECEF axes).
void setMeshInstanceEarth(GpuMeshInstance &inst, const SatEarthLight &L, const glm::dmat3 &rot);

static constexpr uint32_t kNoProbe = 0xFFFFFFFFu;

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
        uint32_t firstMaterial = 0, firstOccluder = 0, occluderCount = 0, firstComponent = 0;
        glm::vec3 boundsCenter{0.0f}; // rest pose
        float boundsRadius = 0.0f;
        int triangles = 0;
        bool hasSharp = false; // a material smooth enough for the sharp (per-pixel) reflection pass
    };

    // Environment probes (SatEnvProbes): the layout of pipeline set 1 (one probe's cube, bound per
    // draw) and the buffer of every probe's SH irradiance (set 0, binding 8).
    struct ProbeBindings
    {
        VkDescriptorSetLayout setLayout = VK_NULL_HANDLE;
        VkBuffer shBuffer = VK_NULL_HANDLE;
        VkDeviceSize shBytes = 0;
    };
    void init(VulkanContext &ctx, const EarthTextures &earth, const ProbeBindings &probes);
    // The model viewer's HDR background (SatEnvProbes::viewerBgView), sampled by sat_mesh_bg.frag.
    void setViewerBackground(VkImageView view, VkSampler sampler);
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
    void recordViewer(VkCommandBuffer cmd, const GpuMeshFrame &frame, const GpuMeshInstance &inst, int typeIdx,
                      VkDescriptorSet probeSet);

    // ── Viewer glare (2026-09-26) ───────────────────────────────────────────────────
    // The glints that make the flare the ground observer sees, drawn as the main view's glare. A sun-only
    // render from the viewer camera at half its resolution (sat_mesh.frag mode 3: L·d² and L, RGBA32F),
    // viewer_glare_find.comp listing its specular 5×5 maxima as effectFlare (the texel's intensity ×
    // flarePerI), and glare_mesh.vert/.frag drawing them additively onto the resolved viewer image.
    // Record right after recordViewer() in the same frame: it draws the instance recordViewer() wrote
    // (slot 0) with its own frame UBO. glarePc is SatelliteSim's GlarePC (include/glare.glsl).
    struct ViewerGlare
    {
        float flarePerI = 0.0f; // effectFlare per unit of intensity per unit irradiance (0 = no glare)
        float minFlare = 1.0f;  // exp2(2 × glare threshold), as glare_find.comp
        float tanHalfX = 0.0f, tanHalfY = 0.0f;
        glm::vec3 tint{1.0f};   // the sunlight's colour, max channel 1
    };
    void recordViewerGlare(VkCommandBuffer cmd, const GpuMeshFrame &frame, int typeIdx, VkDescriptorSet probeSet,
                           const ViewerGlare &g, const void *glarePc, uint32_t glarePcSize);
    // Glints the last completed recordViewerGlare() listed (the list's append count, capped at 64).
    uint32_t viewerGlintCount() const
    {
        return glintMapped ? std::min(*static_cast<const uint32_t *>(glintMapped), 64u) : 0u;
    }

    // ── Photometric check ─────────────────────────────────────────────────────────
    // Renders `inst` (frame.params.w = 1: sun only, scalar, L·d² per pixel) into a kCheckSize² R32F
    // target and copies it to host memory. After the frame that recorded it has completed (the next
    // buildUI), checkPixels() holds the image, row-major, top row first.
    static constexpr uint32_t kCheckSize = 512;
    void recordCheck(VkCommandBuffer cmd, const GpuMeshFrame &frame, const GpuMeshInstance &inst, int typeIdx,
                     VkDescriptorSet probeSet);
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
    // `insts[i]` is drawn with the mesh of `types[i]` and probe set `probeSets[i]` (at most
    // kMaxInstances - 2 of them).
    void recordScene(VkCommandBuffer cmd, const GpuMeshFrame &frame, const std::vector<GpuMeshInstance> &insts,
                     const std::vector<int> &types, const std::vector<VkDescriptorSet> &probeSets);
    // Mesh glints into the flare/bloom source (4c): a fullscreen additive draw, recorded INSIDE the
    // caller's flare-source render pass, turning the over-white part of the scene radiance (times
    // `exposure`, the sky's) into the same log-compressed glow the satellite sprites seed there.
    void createBloomPipeline(VulkanContext &ctx, VkRenderPass flareSourcePass);
    void recordBloom(VkCommandBuffer cmd, uint32_t targetW, uint32_t targetH, float exposure, float gain);

    // ── Sharp reflections (2026-09-25) ──────────────────────────────────────────────────────────
    // The scene pass also writes a reflection G-buffer (RGBA32UI, sceneReflGView): for each mirror-smooth
    // pixel of an instance flagged for it (GpuMeshInstance::earthShC.y), the reflected direction, its
    // weight and slot + 1. recordReflections() then draws sat_sky.frag's SKY_REFL variant once per such
    // instance (its own SatDrawPC: the instance's position as the observer, slot + 1 in `aspect`), inside
    // its screen rectangle, into an RGBA16F target, and mesh_refl_add.comp adds that into the mesh
    // radiance. skyLayout: SatelliteSim's skyBgPipeLayout (the pass binds the sky's own set).
    void createReflPipeline(VulkanContext &ctx, VkPipelineLayout skyLayout);
    VkImageView sceneReflGView() const { return sceneReflGViewH; }
    struct ReflDraw
    {
        VkRect2D rect;       // its pixels on screen (scissor)
        const void *pc;      // SatDrawPC, pcSize bytes
    };
    void recordReflections(VkCommandBuffer cmd, VkDescriptorSet skyDescSet, const std::vector<ReflDraw> &draws,
                           uint32_t pcSize);

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkFormat colorFormat = VK_FORMAT_B8G8R8A8_SRGB;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
    EarthTextures earth_;
    ProbeBindings probes_;

    // Shared geometry + per-type data (host-visible; small).
    VkBuffer vertexBuf = VK_NULL_HANDLE, indexBuf = VK_NULL_HANDLE;
    VkDeviceMemory vertexMem = VK_NULL_HANDLE, indexMem = VK_NULL_HANDLE;
    VkBuffer materialBuf = VK_NULL_HANDLE, occluderBuf = VK_NULL_HANDLE;
    VkDeviceMemory materialMem = VK_NULL_HANDLE, occluderMem = VK_NULL_HANDLE;
    // Per component: xyz = joint pivot (rest frame, from its group hinge), w = parent group (−1 none).
    VkBuffer componentBuf = VK_NULL_HANDLE;
    VkDeviceMemory componentMem = VK_NULL_HANDLE;
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
    VkPipeline viewerMeshPipe = VK_NULL_HANDLE, viewerBgPipe = VK_NULL_HANDLE, viewerMarkerPipe = VK_NULL_HANDLE;
    uint32_t viewerW = 0, viewerH = 0;
    VkImage viewerColorMs = VK_NULL_HANDLE, viewerDepthMs = VK_NULL_HANDLE, viewerResolve = VK_NULL_HANDLE;
    VkDeviceMemory viewerColorMsMem = VK_NULL_HANDLE, viewerDepthMsMem = VK_NULL_HANDLE,
                   viewerResolveMem = VK_NULL_HANDLE;
    VkImageView viewerColorMsView = VK_NULL_HANDLE, viewerDepthMsView = VK_NULL_HANDLE,
                viewerResolveView = VK_NULL_HANDLE;
    VkFramebuffer viewerFb = VK_NULL_HANDLE;
    bool viewerHasContent = false;

    // Viewer glare: the source render (half res), the glint list, the find and draw pipelines, and a
    // pass that loads the resolved viewer image and adds the glare sprites onto it.
    VkRenderPass glareSrcPass = VK_NULL_HANDLE, glareOverPass = VK_NULL_HANDLE;
    VkPipeline glareSrcMeshPipe = VK_NULL_HANDLE;
    uint32_t glareSrcW = 0, glareSrcH = 0;
    VkImage glareSrc = VK_NULL_HANDLE, glareSrcDepth = VK_NULL_HANDLE;
    VkDeviceMemory glareSrcMem = VK_NULL_HANDLE, glareSrcDepthMem = VK_NULL_HANDLE;
    VkImageView glareSrcView = VK_NULL_HANDLE, glareSrcDepthView = VK_NULL_HANDLE;
    VkFramebuffer glareSrcFb = VK_NULL_HANDLE, glareOverFb = VK_NULL_HANDLE;
    VkBuffer glareFrameBuf = VK_NULL_HANDLE, glintBuf = VK_NULL_HANDLE;
    VkDeviceMemory glareFrameMem = VK_NULL_HANDLE, glintMem = VK_NULL_HANDLE;
    void *glareFrameMapped = nullptr, *glintMapped = nullptr;
    VkDescriptorSet descSetGlare = VK_NULL_HANDLE; // descLayout, its own frame UBO (glareFrameBuf)
    VkDescriptorSetLayout glareFindLayout = VK_NULL_HANDLE, glareDrawLayout = VK_NULL_HANDLE;
    VkDescriptorPool glareDescPool = VK_NULL_HANDLE;
    VkDescriptorSet glareFindSet = VK_NULL_HANDLE, glareDrawSet = VK_NULL_HANDLE;
    VkPipelineLayout glareFindPipeLayout = VK_NULL_HANDLE, glareDrawPipeLayout = VK_NULL_HANDLE;
    VkPipeline glareFindPipe = VK_NULL_HANDLE, glareDrawPipe = VK_NULL_HANDLE;
    void createViewerGlare(VulkanContext &ctx);  // passes, buffers, layouts, find + draw pipelines
    void createViewerGlareTargets(VulkanContext &ctx);
    void destroyViewerGlareTargets();

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
    VkPipeline sceneMeshPipe = VK_NULL_HANDLE;  // shading, depth EQUAL after the pre-pass
    VkPipeline sceneDepthPipe = VK_NULL_HANDLE; // depth pre-pass (lattice cut-outs only)
    uint32_t sceneW = 0, sceneH = 0;
    VkImage sceneColor = VK_NULL_HANDLE, sceneDist = VK_NULL_HANDLE, sceneDepth = VK_NULL_HANDLE;
    VkDeviceMemory sceneColorMem = VK_NULL_HANDLE, sceneDistMem = VK_NULL_HANDLE, sceneDepthMem = VK_NULL_HANDLE;
    VkImageView sceneColorViewH = VK_NULL_HANDLE, sceneDistViewH = VK_NULL_HANDLE, sceneDepthView = VK_NULL_HANDLE;
    VkFramebuffer sceneFb = VK_NULL_HANDLE;
    VkImage sceneReflG = VK_NULL_HANDLE;              // reflection G-buffer (RGBA32UI)
    VkDeviceMemory sceneReflGMem = VK_NULL_HANDLE;
    VkImageView sceneReflGViewH = VK_NULL_HANDLE;
    VkImage reflRad = VK_NULL_HANDLE;                 // the SKY_REFL output (RGBA16F)
    VkDeviceMemory reflRadMem = VK_NULL_HANDLE;
    VkImageView reflRadView = VK_NULL_HANDLE;
    VkFramebuffer reflFb = VK_NULL_HANDLE;
    VkRenderPass reflPass = VK_NULL_HANDLE;
    VkPipelineLayout skyLayout_ = VK_NULL_HANDLE;     // not owned
    VkPipeline reflPipe = VK_NULL_HANDLE;
    VkDescriptorSetLayout reflAddLayout = VK_NULL_HANDLE;
    VkDescriptorPool reflAddPool = VK_NULL_HANDLE;
    VkDescriptorSet reflAddSet = VK_NULL_HANDLE;
    VkPipelineLayout reflAddPipeLayout = VK_NULL_HANDLE;
    VkPipeline reflAddPipe = VK_NULL_HANDLE;
    void writeReflAddDescriptors();
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
