#pragma once
// ── Asynchronous frame readback ───────────────────────────────────────────────────────────────────
// A small ring of host-visible staging buffers the frame's own command buffer copies the finished image into
// (scaled through a blit when the output size differs from the source). A copy is read only once its frame is
// complete (VulkanContext::completedFrame(): two frames in flight), so the frame thread never waits on the GPU.
// When every slot is still busy, record() returns -1 and the caller drops (or repeats) the frame instead of
// stalling. Used by the video recorder (a consumer thread reads the slots) and the harness's settle probe.
#include <atomic>
#include <cstdint>
#include <string>
#include <vector>

#include <vulkan/vulkan.h>

struct VulkanContext;

class FrameCapture
{
public:
    enum : int { kFree = 0, kGpu = 1, kReady = 2 };
    struct Slot
    {
        VkBuffer buf = VK_NULL_HANDLE;
        VkDeviceMemory mem = VK_NULL_HANDLE;
        const uint8_t *map = nullptr;
        uint64_t frame = 0;  // VulkanContext::frameIndex of the frame that copied into it
        uint64_t seq = 0;    // order of the copies
        int tag = 0;         // the caller's (e.g. how many video frames this capture stands for)
        std::atomic<int> state{kFree};
    };

    FrameCapture() = default;
    FrameCapture(const FrameCapture &) = delete;
    FrameCapture &operator=(const FrameCapture &) = delete;
    ~FrameCapture() = default; // call destroy() with the context first

    // w, h: the output size (the copy is cropped / blitted to it). Allocates `slots` buffers of w*h*4 bytes.
    bool create(VulkanContext &ctx, uint32_t w, uint32_t h, int slots, std::string &err);
    // Frees everything. The GPU must be done with the last copy (completedFrame() >= lastRecordedFrame()).
    void destroy(VulkanContext &ctx);
    bool created() const { return !slots_.empty(); }
    uint32_t width() const { return w_; }
    uint32_t height() const { return h_; }
    bool bgra() const { return bgra_; }
    uint64_t lastRecordedFrame() const { return lastFrame_; }
    int freeSlots() const;
    int busySlots() const { return (int)slots_.size() - freeSlots(); }
    int countState(int st) const
    {
        int n = 0;
        for (const Slot &s : slots_)
            n += s.state.load(std::memory_order_acquire) == st;
        return n;
    }

    // `image` must be in TRANSFER_SRC_OPTIMAL (it stays there); `ext` / `fmt` describe it. Returns the slot, or -1
    // when none is free (nothing recorded).
    int record(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image, VkExtent2D ext, VkFormat fmt, int tag);
    // After the frame fence: every copy whose frame is complete, oldest first, moves to kReady and is passed to
    // fn(int slot, const Slot &). The consumer calls release(slot) when done with its bytes (any thread).
    template <class F> void collect(const VulkanContext &ctx, F fn) { collectUpTo(completedFrameOf(ctx), fn); }
    // Every copy recorded, whatever completedFrame() says: only after VulkanContext::drainFrames().
    template <class F> void collectAll(F fn) { collectUpTo(UINT64_MAX, fn); }
    void release(int slot) { slots_[(size_t)slot].state.store(kFree, std::memory_order_release); }
    const Slot &slot(int i) const { return slots_[(size_t)i]; }

private:
    template <class F> void collectUpTo(uint64_t done, F fn);
    uint64_t completedFrameOf(const VulkanContext &ctx) const;
    std::vector<Slot> slots_;
    uint32_t w_ = 0, h_ = 0;
    bool bgra_ = true;
    uint64_t seq_ = 0, lastFrame_ = 0;
    VkImage scaled_ = VK_NULL_HANDLE; // the blit target when the source size differs from the output
    VkDeviceMemory scaledMem_ = VK_NULL_HANDLE;
    VkFormat scaledFmt_ = VK_FORMAT_UNDEFINED;
};

template <class F> void FrameCapture::collectUpTo(uint64_t done, F fn)
{
    for (;;)
    {
        int best = -1;
        for (int i = 0; i < (int)slots_.size(); ++i)
            if (slots_[(size_t)i].state.load(std::memory_order_acquire) == kGpu && slots_[(size_t)i].frame <= done &&
                (best < 0 || slots_[(size_t)i].seq < slots_[(size_t)best].seq))
                best = i;
        if (best < 0)
            return;
        slots_[(size_t)best].state.store(kReady, std::memory_order_release);
        fn(best, slots_[(size_t)best]);
    }
}
