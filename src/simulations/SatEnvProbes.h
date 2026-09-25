#pragma once
// Environment probes for satellite meshes (2026-09-24): the FULL sky renderer seen from a satellite.
//
// sat_sky.frag built with -DSKY_ENV (sat_sky_env.frag.spv) draws the atmosphere, the textured Earth
// and ocean, the flat cloud decks, city lights, airglow, the aurora, the Moon and the Milky Way from
// any position, in pre-exposure HDR. A probe is that renderer's six cube faces around one position:
// its mips are the reflection of a surface of any roughness, and its order-2 SH projection
// (env_probe_sh.comp) is the diffuse light a face receives — the real Earth below, with its clouds,
// oceans and terminator, not the one-direction earthshine the photometry uses. The same pipeline
// draws the model viewer's background at the viewer's own camera.
//
// Each probe is its own cube image with its own descriptor set (sat_mesh.frag set 1), bound per
// draw — the scene pass already issues one draw per instance — so no cube-array feature is needed.
// Slot 0 belongs to the model viewer; SatelliteSim assigns the others to scene instances.
#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>

#include <cstdint>

struct VulkanContext;

class SatEnvProbes
{
public:
    static constexpr int kProbes = 8; // slot 0: the model viewer
    // Texels per face edge. The viewer's probe is what a mirror filling the window reflects, so it is
    // finer (0.18 deg per texel, finer than the 8K Earth texture seen from orbit); it is re-rendered a
    // face per frame. 128 (0.7 deg) read as pixelated in mirrors.
    static constexpr uint32_t kViewerFaceSize = 512;
    static constexpr uint32_t kSceneFaceSize = 256; // 0.35 deg
    static uint32_t faceSize(int slot) { return slot == 0 ? kViewerFaceSize : kSceneFaceSize; }
    static uint32_t mipCount(int slot); // full chain: log2(faceSize) + 1
    static constexpr VkFormat kFormat = VK_FORMAT_R16G16B16A16_SFLOAT;

    // skyLayout: SatelliteSim's skyBgPipeLayout (the env pipeline binds the sky's own set 0).
    void init(VulkanContext &ctx, VkPipelineLayout skyLayout);
    void cleanup(VkDevice device);
    bool ready() const { return envPipe != VK_NULL_HANDLE; }

    // sat_mesh.frag set 1: binding 0 = this probe's cube (samplerCube).
    VkDescriptorSetLayout probeSetLayout() const { return meshSetLayout; }
    VkDescriptorSet probeSet(int slot) const { return meshSets[(slot >= 0 && slot < kProbes) ? slot : 0]; }
    // vec4 sh[kProbes * 9]: E(n)/π = Σ sh[slot*9 + k].rgb · P_k(n) (env_probe_sh.comp).
    VkBuffer shBuffer() const { return shBuf; }
    VkDeviceSize shBufferSize() const { return sizeof(glm::vec4) * 9 * kProbes; }

    // Records the faces of probe `slot` in faceMask (bit f = face f; push constants pcs[f], pcSize bytes
    // each, SatDrawPC — faceCamToWorld() gives each face's rotation), then its whole mip chain and its
    // SH projection. Faces left out keep their last render. Outside any render pass. The probe is
    // SHADER_READ_ONLY afterwards (fragment + compute).
    void recordProbe(VkCommandBuffer cmd, int slot, VkDescriptorSet skyDescSet, const void *pcs, uint32_t pcSize,
                     uint32_t faceMask = 0x3Fu);

    // Cube face f (Vulkan order +X −X +Y −Y +Z −Z): the rotation from the sky shader's camera space
    // (x right, y up, looking down −z) to the probe's world axes (ECEF). faceSelfCheck() verifies the
    // table against Vulkan's cube-face selection rule (logged at init).
    static glm::dmat3 faceCamToWorld(int face);
    static bool faceSelfCheck(double &maxErr);

    // ── Model viewer background ──────────────────────────────────────────────────────────────────
    // An HDR target at w×h drawn with the same env pipeline at the viewer's camera; sat_mesh_bg.frag
    // samples it, tonemaps it and puts the observer / target markers on it.
    bool ensureViewerBg(VulkanContext &ctx, uint32_t w, uint32_t h); // true if (re)created
    void recordViewerBg(VkCommandBuffer cmd, VkDescriptorSet skyDescSet, const void *pc, uint32_t pcSize);
    VkImageView viewerBgView() const { return bgView; }
    VkSampler sampler() const { return linearSampler; }

private:
    VkDevice device_ = VK_NULL_HANDLE;
    VkPipelineLayout skyLayout_ = VK_NULL_HANDLE; // SatelliteSim's skyBgPipelineLayout (not owned)
    VkFilter blitFilter = VK_FILTER_LINEAR;

    struct Probe
    {
        uint32_t size = 0, mips = 0;
        VkImage image = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkImageView cubeView = VK_NULL_HANDLE;
        VkImageView faceView[6] = {};
        VkFramebuffer fb[6] = {};
    };
    Probe probes[kProbes];

    VkRenderPass probePass = VK_NULL_HANDLE; // RGBA16F, final TRANSFER_SRC (mip generation)
    VkRenderPass bgPass = VK_NULL_HANDLE;    // RGBA16F, final SHADER_READ_ONLY (compatible)
    VkPipeline envPipe = VK_NULL_HANDLE;
    VkSampler linearSampler = VK_NULL_HANDLE;

    // sat_mesh.frag set 1, one per probe.
    VkDescriptorSetLayout meshSetLayout = VK_NULL_HANDLE;
    VkDescriptorPool pool = VK_NULL_HANDLE;
    VkDescriptorSet meshSets[kProbes] = {};

    // SH projection.
    VkBuffer shBuf = VK_NULL_HANDLE;
    VkDeviceMemory shMem = VK_NULL_HANDLE;
    VkDescriptorSetLayout shSetLayout = VK_NULL_HANDLE;
    VkDescriptorSet shSets[kProbes] = {};
    VkPipelineLayout shLayout = VK_NULL_HANDLE;
    VkPipeline shPipe = VK_NULL_HANDLE;

    // Viewer background.
    uint32_t bgW = 0, bgH = 0;
    VkImage bgImage = VK_NULL_HANDLE;
    VkDeviceMemory bgMem = VK_NULL_HANDLE;
    VkImageView bgView = VK_NULL_HANDLE;
    VkFramebuffer bgFb = VK_NULL_HANDLE;
    void destroyViewerBg();
};
