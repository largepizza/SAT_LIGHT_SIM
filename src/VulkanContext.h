#pragma once
#define GLFW_INCLUDE_VULKAN
// GLFW_INCLUDE_VULKAN only adds <vulkan/vulkan.h> — glfw3.h still pulls in the platform's
// default OpenGL header (<GL/gl.h> on Linux, <OpenGL/gl.h> on macOS) unless told not to.
// This project is Vulkan-only and never touches OpenGL; Windows/macOS SDKs happen to ship
// that header so this was silently masked there, but it broke the Linux CI build (no
// GL/gl.h without an extra Mesa dev package) — GLFW_INCLUDE_NONE is the real fix, not
// papering over it with an apt-get install.
#define GLFW_INCLUDE_NONE
#include <GLFW/glfw3.h>
#include <optional>
#include <string>
#include <vector>

// All constants live here so simulations can reference them
constexpr uint32_t WIN_W          = 1280;
constexpr uint32_t WIN_H          = 720;
constexpr uint32_t GOL_W          = 512;
constexpr uint32_t GOL_H          = 512;
constexpr uint32_t PARTICLE_COUNT = 500'000;

// ── Internal helper types ─────────────────────────────────────────────────────
// Declared here so member function return types compile cleanly under MSVC.
// Treat these as private implementation details of VulkanContext.
struct QueueFamilyIndices {
    std::optional<uint32_t> graphics;
    std::optional<uint32_t> compute;
    bool complete() const { return graphics.has_value() && compute.has_value(); }
};
struct SwapchainDetails {
    VkSurfaceCapabilitiesKHR         caps;
    std::vector<VkSurfaceFormatKHR>  formats;
    std::vector<VkPresentModeKHR>    modes;
};

// ─────────────────────────────────────────────────────────────────────────────
struct VulkanContext {
    // ── Core objects (read by simulations) ─────────────────────────────────
    VkInstance               instance        = VK_NULL_HANDLE;
    VkDebugUtilsMessengerEXT debugMessenger  = VK_NULL_HANDLE;
    VkSurfaceKHR             surface         = VK_NULL_HANDLE;
    VkPhysicalDevice         physicalDevice  = VK_NULL_HANDLE;
    VkDevice                 device          = VK_NULL_HANDLE;
    VkQueue                  graphicsQueue   = VK_NULL_HANDLE;
    VkQueue                  computeQueue    = VK_NULL_HANDLE;
    uint32_t                 graphicsFamily  = 0;
    uint32_t                 computeFamily   = 0;

    // ── Swapchain ──────────────────────────────────────────────────────────
    VkSwapchainKHR           swapchain    = VK_NULL_HANDLE;
    std::vector<VkImage>     swapImages;
    std::vector<VkImageView> swapViews;
    VkFormat                 swapFormat   = VK_FORMAT_UNDEFINED;
    VkExtent2D               swapExtent{};
    // NEW-7 (RELEASE_v1_1_PLAN.md): Settings > Display "Frame limiter" preference, read by
    // createSwapchain() at both initial creation and every recreateSwapchain() call. Default
    // FIFO (V-Sync) — MAILBOX was the old unconditional default, which lets the GPU run flat out
    // forever on a laptop (fans/heat/battery, exactly the low-end audience this release targets).
    // createSwapchain() falls back to FIFO (spec-guaranteed present) if the requested mode isn't
    // in the surface's supported list. A simulation changes this and then reports true from
    // Simulation::consumeSwapchainRebuildRequest() to have App rebuild the swapchain with it.
    VkPresentModeKHR         presentModePreference = VK_PRESENT_MODE_FIFO_KHR;
    // The preference the current swapchain was made with (2026-10-21): a request that does not change it needs no
    // rebuild (applyFpsCapMode after loading the settings rebuilt it on every launch: ~215 ms of the first frame).
    VkPresentModeKHR         presentModeBuilt = VK_PRESENT_MODE_FIFO_KHR;
    // UC6 (RELEASE_v1_1_PLAN.md): true if the surface advertised TRANSFER_SRC support for
    // swapchain images (near-universal but not spec-guaranteed) — createSwapchain() only adds
    // VK_IMAGE_USAGE_TRANSFER_SRC_BIT to ci.imageUsage when this would be true, since requesting
    // an unsupported usage bit is invalid and would fail swapchain creation outright. Simulations
    // must gate screenshot capture on this rather than assuming the copy will always succeed.
    bool                     swapTransferDstSupported = false; // the swapchain accepts blits (background prepasses)
    bool                     screenshotSupported = false;
    // VK_KHR_pipeline_executable_properties, when the device offers it: the driver's own statistics
    // for a compiled pipeline (registers, spills, ...). Pipelines created with
    // VK_PIPELINE_CREATE_CAPTURE_STATISTICS_BIT_KHR can be queried (harness `shaders reload`).
    bool                     pipelineStatsSupported = false;
    // Set only while the harness rebuilds shaders (`shaders reload`): the statistics capture keeps a pipeline out
    // of the caches, so it is not requested at startup.
    bool                     capturePipelineStats = false;
    // The app's own pipeline cache (2026-10-02), persisted to pipeline_cache.bin next to the exe (or the user data
    // folder). Every pipeline is created through it. The driver's own Vulkan cache is capped (~512 MB on NVIDIA,
    // shared by every build): once full it stopped storing new pipelines, and every launch recompiled every shader
    // changed since — 30-40 s of startup (pipelines 18 s, the clouds' bakes 4 s, the first frames 10-15 s).
    VkPipelineCache          pipelineCache = VK_NULL_HANDLE;
    std::string              pipelineCachePath;
    void                     savePipelineCache();

    // ── Render pass & framebuffers ─────────────────────────────────────────
    VkRenderPass                renderPass = VK_NULL_HANDLE;
    // Same attachment formats/sample-counts as renderPass (so it stays compatible with the same
    // framebuffers below), but LOAD instead of CLEAR for the color attachment — for simulations
    // that pre-populate the swapchain image via a blit before the main pass begins (see
    // Simulation::recordPrePass/activeRenderPass). Depth still CLEARs normally either way.
    VkRenderPass                renderPassLoad = VK_NULL_HANDLE;
    // Third pass variant, for the boot/staging screen (BOOT_LOADER_PLAN.md §3.1): the frames App
    // presents while Simulation::init() is still running draw with this instead of renderPass.
    // Same attachment formats/sample-counts as renderPass/renderPassLoad, so it is compatible with
    // the SAME framebuffers above — no framebuffers of its own and nothing to rebuild on resize.
    // See createRenderPassBoot's definition for what actually differs.
    VkRenderPass                renderPassBoot = VK_NULL_HANDLE;
    std::vector<VkFramebuffer>  framebuffers;

    // ── Depth buffer (shared; recreated on resize) ─────────────────────────
    VkImage        depthImage  = VK_NULL_HANDLE;
    VkDeviceMemory depthMemory = VK_NULL_HANDLE;
    VkImageView    depthView   = VK_NULL_HANDLE;
    VkFormat       depthFormat = VK_FORMAT_D32_SFLOAT;

    // The photo target's own images (beginPhotoTarget).
    VkImage        photoImage = VK_NULL_HANDLE, photoDepthImage = VK_NULL_HANDLE;
    VkDeviceMemory photoMem = VK_NULL_HANDLE, photoDepthMem = VK_NULL_HANDLE;
    VkImageView    photoView = VK_NULL_HANDLE, photoDepthView = VK_NULL_HANDLE;
    VkFramebuffer  photoFramebuffer = VK_NULL_HANDLE;
    VkExtent2D     photoSavedExtent{};

    // ── Commands & sync ────────────────────────────────────────────────────
    // Frames in flight (2026-10-25, branch perf-frames). With 2, the CPU records frame f while the GPU still
    // runs frame f-1: a frame takes max(CPU, GPU) instead of CPU + GPU. The GPU itself still runs frames one
    // after the other (every frame's GPU work opens with a full barrier, beginFrameGpu / recordFrameUploads), so
    // device-local images and buffers need nothing. Only memory the CPU touches does:
    //   - CPU -> GPU: mirrorUpload() (a buffer the CPU rewrites: it writes a shadow, copied at the frame's start),
    //     uploadPatch() / uploadFill() (a few bytes into GPU memory), slotted() (staging the CPU fills and the
    //     frame's own commands copy, or a vertex / index buffer: one copy per slot);
    //   - GPU -> CPU: readbackCreate() (one host buffer per slot: the CPU reads the data of completedFrame()).
    // SATLIGHTSIM_FRAMES_IN_FLIGHT=1 restores the serial frame: one slot, every mirror written in place.
    static constexpr uint32_t kMaxFramesInFlight = 2;
    // Memory for a host copy of GPU results (slotted readbacks): cached, coherent (slotted() drops CACHED when
    // the device has no such type).
    static constexpr VkMemoryPropertyFlags kHostReadProps =
        VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT | VK_MEMORY_PROPERTY_HOST_CACHED_BIT;
    uint32_t                 framesInFlight = 2;
    uint32_t                 frameSlot = 0;   // the slot being recorded
    uint64_t                 frameIndex = 0;  // the frame being recorded (1-based; 0 before the first)
    VkCommandPool            commandPool        = VK_NULL_HANDLE;
    VkCommandBuffer          commandBuffer      = VK_NULL_HANDLE; // this slot's (beginFrameSlot)
    VkSemaphore              semImageAvailable  = VK_NULL_HANDLE; // this slot's
    std::vector<VkSemaphore> semRenderDone;   // one per swapchain image
    VkFence                  fenceFrame         = VK_NULL_HANDLE; // this slot's
    VkCommandBuffer          slotCmd[kMaxFramesInFlight] = {};
    VkCommandBuffer          slotUploadCmd[kMaxFramesInFlight] = {};
    VkSemaphore              slotAcquire[kMaxFramesInFlight] = {};
    VkFence                  slotFence[kMaxFramesInFlight] = {};
    bool                     slotSubmitted[kMaxFramesInFlight] = {};
    // Waits for this slot's previous frame, then points commandBuffer / fenceFrame / semImageAvailable, every
    // slotted buffer and every readback pointer at it. Returns true when the slot held a submitted frame (its
    // readbacks and timestamps hold data).
    bool beginFrameSlot();
    void markSlotSubmitted() { slotSubmitted[frameSlot] = true; }
    // The newest frame known complete: the readbacks hold its data (0 = none yet).
    uint64_t completedFrame() const { return frameIndex > framesInFlight ? frameIndex - framesInFlight : 0; }
    // Waits for every frame in flight (resize, a rare write into a buffer the GPU reads, a descriptor rewrite).
    void drainFrames();
    // Records this frame's uploads (mirrors, patches) into the slot's upload command buffer, opened with the full
    // barrier against the previous frame. Returns it (submit it before commandBuffer), or VK_NULL_HANDLE when
    // there is nothing to record.
    VkCommandBuffer recordFrameUploads();
    // Opens the frame's main command buffer: a full barrier against the previous frame's GPU work (only with
    // more than one frame in flight; recordFrameUploads opens with the same barrier when it records anything).
    void beginFrameGpu(VkCommandBuffer cmd);

    // CPU -> GPU, a buffer the CPU rewrites (whole, or the ranges marked with mirrorDirty). Returns the pointer to
    // write: the mapped memory itself with one frame in flight, else a shadow copied into `dst` at the start of
    // the frame's GPU work. `dst` needs VK_BUFFER_USAGE_TRANSFER_DST_BIT. The caller must not map `mem` itself.
    void *mirrorUpload(VkBuffer dst, VkDeviceMemory mem, VkDeviceSize size, bool whole, const char *name);
    void mirrorDirty(const void *base, VkDeviceSize off, VkDeviceSize len);
    void mirrorDirtyAll(const void *base);
    void mirrorForget(const void *base); // before the buffer is destroyed (unmaps / frees the shadow)
    // A few bytes into a GPU buffer, applied at the start of this frame's GPU work (in both modes).
    void uploadPatch(VkBuffer dst, VkDeviceSize off, const void *data, VkDeviceSize len);
    void uploadFill(VkBuffer dst, VkDeviceSize off, VkDeviceSize len, uint32_t value);
    // Staging for a copy recorded into THIS frame's command buffer (an image upload): host memory of this slot,
    // reused once the slot comes round again. Fill ptr, then copy from (buf, off).
    struct StagingAlloc
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceSize off = 0;
        void *ptr = nullptr;
    };
    StagingAlloc stagingAlloc(VkDeviceSize bytes);
    // GPU -> CPU. One host buffer per slot (HOST_CACHED when the device has it). Record copies into
    // readbackBuffer(id) during the frame; after beginFrameSlot, *userPtr points at the slot's buffer: the data
    // of frame completedFrame(). `userPtr` may be null (use readbackData).
    int readbackCreate(VkDeviceSize size, void **userPtr, const char *name);
    VkBuffer readbackBuffer(int id) const;
    void *readbackData(int id) const;
    void readbackDestroy(int id);
    // A buffer the CPU fills and this frame's commands read (staging copied by the frame, a vertex / index
    // buffer): one copy per slot; after beginFrameSlot *bufVar / *mapVar (and *memVar) are the slot's.
    void slotted(VkBuffer *bufVar, VkDeviceMemory *memVar, void **mapVar, VkDeviceSize size, VkBufferUsageFlags usage,
                 VkMemoryPropertyFlags props, const char *name);
    void slottedDestroy(VkBuffer *bufVar);
    // Destroys a buffer / frees memory once no frame in flight can use it.
    void deferDestroy(VkBuffer buf, VkDeviceMemory mem);
    // Per-frame counters (harness `frames`): bytes uploaded, copies, drains, one-time submits.
    struct FrameStats
    {
        uint64_t uploadBytes = 0, patchBytes = 0, copies = 0, drains = 0, oneTimeSubmits = 0, descWrites = 0;
    } frameStats, frameStatsLast;
    // Counts a descriptor write made while frames may be in flight; drains first when one could be (a set
    // bound by the previous frame may not be rewritten while it runs). Call before vkUpdateDescriptorSets
    // outside init / resize.
    void beforeDescriptorWrite();

    // ── GPU timestamp profiling ────────────────────────────────────────────
    // Single frame in flight, so it's safe to resolve the previous frame's
    // query results right after the fence wait in App::drawFrame, before the
    // command buffer is reset and re-recorded for the new frame. Slot layout
    // is a shared contract between App.cpp (writes 0, 7, 8) and SatelliteSim
    // (writes 1-5 in recordCompute; 6 in recordPrePass XOR recordDraw).
    //
    // Authoritative slot layout (keep in sync with SatelliteSim::updateGpuTimingStats,
    // kPerfLabels[] in SatelliteSimUI.cpp, and the JSON keys in savePerfSnapshot):
    //   0 frame start                 App.cpp
    //   1 scene_depth.comp done       SatelliteSim::recordCompute
    //   2 (vestigial — see below)     SatelliteSim::recordCompute
    //   3 sat_orbit.comp AND          SatelliteSim::recordCompute
    //     beam_self_march.comp done
    //   4 cloud_march.comp done       SatelliteSim::recordCompute
    //   5 sat_flare.comp done         SatelliteSim::recordCompute
    //   6 sky background draw done    SatelliteSim::recordPrePass XOR ::recordDraw
    //   7 satellite + star draw done  App.cpp
    //   8 UI overlay done             App.cpp
    //
    // History: cloud_shadow.comp (C12) held a slot here until the pipeline-unification pass
    // folded its work into cloud_march.comp; beam_cloud_block.comp was added when its cost was
    // found to be hiding inside the orbit bucket, taking slot 2 ("beam_cloud_block.comp done");
    // scene_depth.comp then took slot 1 at the head of the pass. 2026-08-09: beam_cloud_block.comp
    // itself was retired (replaced by beam_self_march.comp, a per-BEAM march dispatched AFTER
    // sat_orbit.comp instead of before it — see BEAM_CLOUD_PLAN.md) — rather than renumber every
    // downstream slot again, slot 2 was left in place as a vestigial marker (written right before
    // sat_orbit.comp now starts, so its own bucket, delta(1,2), reads ~0 every frame) and slot 3
    // was pushed out to also cover beam_self_march.comp's dispatch, so delta(2,3) — labeled
    // "orbit_compute" in kPerfLabels[]/the JSON keys — now measures both passes combined. Every
    // downstream slot NUMBER is unchanged from before this pass existed. Slot numbers are
    // documented here and nowhere else authoritative.
    static constexpr uint32_t kTimestampCount = 9;
    VkQueryPool queryPool         = VK_NULL_HANDLE;
    double      timestampPeriodNs = 0.0;   // ns/tick, from device limits; 0 = unsupported
    bool        timestampsReady   = false; // false until one full frame has been resolved
    double      timestampMs[kTimestampCount] = {}; // resolved, ms relative to slot 0

    void resetTimestamps(VkCommandBuffer cmd);
    void writeTimestamp(VkCommandBuffer cmd, VkPipelineStageFlagBits stage, uint32_t slot);
    void resolveTimestamps();

    // ── Lifecycle ──────────────────────────────────────────────────────────
    void init(GLFWwindow* window);
    void recreateSwapchain(GLFWwindow* window);

    // ── HQ photo target (review 13) ────────────────────────────────────────
    // An offscreen colour + depth image standing in for the swapchain while a simulation renders a
    // high-resolution photo: beginPhotoTarget() sets swapExtent to (w, h) (clamped to the device's
    // limits) and APPENDS the image, its view and a framebuffer to swapImages / swapViews /
    // framebuffers, so every swapImages[imgIdx] / framebuffers[imgIdx] user works unchanged with
    // imgIdx = photoIndex. App neither acquires nor presents while it is active, and defers window
    // resizes until endPhotoTarget() has removed it again (cleanupSwapchain would destroy its view).
    bool        photoActive = false;
    uint32_t    photoIndex  = 0;
    bool beginPhotoTarget(uint32_t w, uint32_t h);
    void endPhotoTarget();
    void cleanup();

    // ── Helpers exposed to simulations ─────────────────────────────────────
    VkShaderModule loadShader(const std::string& spvPath);
    uint32_t findMemoryType(uint32_t filter, VkMemoryPropertyFlags props);
    void createBuffer(VkDeviceSize size, VkBufferUsageFlags usage,
                      VkMemoryPropertyFlags props,
                      VkBuffer& buf, VkDeviceMemory& mem);
    void createImage(uint32_t w, uint32_t h, VkFormat fmt,
                     VkImageUsageFlags usage,
                     VkImage& img, VkDeviceMemory& mem,
                     uint32_t mipLevels = 1,
                     uint32_t depth = 1);
    // Generates mip levels 1..mipLevels-1 from mip 0 via linear blit.
    // Mip 0 must be in TRANSFER_DST_OPTIMAL on entry; all mips will be
    // in SHADER_READ_ONLY_OPTIMAL on return.
    void generateMipmaps(VkCommandBuffer cmd, VkImage img, VkFormat fmt,
                         uint32_t w, uint32_t h, uint32_t mipLevels);
    VkCommandBuffer beginOneTimeCommands();
    void endOneTimeCommands(VkCommandBuffer cmd);
    // Linear blit filtering is a per-format capability, not a given — see the definition.
    VkFilter bestBlitFilter(VkFormat fmt) const;

    void imageBarrier(VkCommandBuffer cmd, VkImage image,
                      VkAccessFlags srcAccess, VkAccessFlags dstAccess,
                      VkImageLayout oldLayout, VkImageLayout newLayout,
                      VkPipelineStageFlags srcStage, VkPipelineStageFlags dstStage);

private:
    void createInstance();
    void setupDebugMessenger();
    void createSurface(GLFWwindow* window);
    void pickPhysicalDevice();
    // Logs every device limit this project exceeds the Vulkan guaranteed minimum for, and
    // throws a named error if one is actually short. See the definition for why each matters.
    void logDeviceLimits(const VkPhysicalDeviceLimits &lim);
    void createDevice();
    void createSwapchain(GLFWwindow* window);
    void createRenderPass();
    void createRenderPassLoad();
    void createRenderPassBoot();
    void createDepthResources();
    void destroyDepthResources();
    void createFramebuffers();
    void createCommandPool();
    void createCommandBuffer();
    void createSyncObjects();
    void createQueryPool();
    void cleanupSwapchain();
    VkFormat findDepthFormat();

    QueueFamilyIndices findQueueFamilies(VkPhysicalDevice pd);
    SwapchainDetails   querySwapchainDetails(VkPhysicalDevice pd);

    // Frames in flight: the uploads' and readbacks' own storage.
    struct Mirror
    {
        VkBuffer dst = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        VkDeviceSize size = 0;
        uint8_t *shadow = nullptr;
        bool whole = true, mapped = false;
        std::vector<std::pair<VkDeviceSize, VkDeviceSize>> dirty; // [begin, end)
        std::string name;
    };
    std::vector<Mirror> mirrors_;
    struct Patch
    {
        VkBuffer dst;
        VkDeviceSize off, len;
        size_t data; // offset into patchData_, or SIZE_MAX for a fill
        uint32_t fill;
    };
    std::vector<Patch> patches_;
    std::vector<uint8_t> patchData_;
    struct Ring
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint8_t *map = nullptr;
        VkDeviceSize cap = 0;
    } ring_[kMaxFramesInFlight];
    struct StageChunk
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        uint8_t *map = nullptr;
        VkDeviceSize cap = 0, used = 0;
    };
    std::vector<StageChunk> stage_[kMaxFramesInFlight];
    struct Readback
    {
        VkDeviceSize size = 0;
        VkBuffer buf[kMaxFramesInFlight] = {};
        VkDeviceMemory mem[kMaxFramesInFlight] = {};
        void *map[kMaxFramesInFlight] = {};
        void **user = nullptr;
        bool live = false;
        std::string name;
    };
    std::vector<Readback> readbacks_;
    struct Slotted
    {
        VkBuffer *bufVar = nullptr;
        VkDeviceMemory *memVar = nullptr;
        void **mapVar = nullptr;
        VkBuffer buf[kMaxFramesInFlight] = {};
        VkDeviceMemory mem[kMaxFramesInFlight] = {};
        void *map[kMaxFramesInFlight] = {};
    };
    std::vector<Slotted> slotted_;
    struct Deferred
    {
        VkBuffer buf;
        VkDeviceMemory mem;
        uint64_t frame; // destroy once completedFrame() >= frame
    };
    std::vector<Deferred> deferred_;
    uint32_t hostReadMemType_ = UINT32_MAX;
    void ensureRing(uint32_t slot, VkDeviceSize bytes);
    void pointSlotted();
    void destroyFrameResources();

    static VKAPI_ATTR VkBool32 VKAPI_CALL debugCallback(
        VkDebugUtilsMessageSeverityFlagBitsEXT severity,
        VkDebugUtilsMessageTypeFlagsEXT type,
        const VkDebugUtilsMessengerCallbackDataEXT* data,
        void* userData);

public:
    // The context frameSafeUpdateDescriptorSets consults (set by init).
    static VulkanContext *sFrameCtx;
};

// vkUpdateDescriptorSets for the simulations, the UI and the mesh renderer (frames in flight, 2026-10-25): a set the
// previous frame bound may not be rewritten while that frame runs, so once frames have started it waits for the frames
// in flight first (VulkanContext::beforeDescriptorWrite; counted in frameStats.descWrites / drains). Main thread only.
void frameSafeUpdateDescriptorSets(VkDevice device, uint32_t writeCount, const VkWriteDescriptorSet *writes,
                                   uint32_t copyCount, const VkCopyDescriptorSet *copies);
