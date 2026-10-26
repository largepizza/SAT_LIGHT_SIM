// Asynchronous frame readback (see FrameCapture.h).
#include "FrameCapture.h"

#include "VulkanContext.h"

#include <stdexcept>

uint64_t FrameCapture::completedFrameOf(const VulkanContext &ctx) const { return ctx.completedFrame(); }

bool FrameCapture::create(VulkanContext &ctx, uint32_t w, uint32_t h, int slots, std::string &err)
{
    destroy(ctx);
    if (w == 0 || h == 0 || slots < 1)
    {
        err = "FrameCapture: bad size";
        return false;
    }
    w_ = w;
    h_ = h;
    slots_ = std::vector<Slot>((size_t)slots);
    const VkDeviceSize size = (VkDeviceSize)w * h * 4;
    try
    {
        for (Slot &s : slots_)
        {
            // Cached host memory: the consumer reads every byte (uncached reads are one bus transaction each).
            try
            {
                ctx.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT, VulkanContext::kHostReadProps, s.buf, s.mem);
            }
            catch (const std::exception &)
            {
                if (s.buf)
                    vkDestroyBuffer(ctx.device, s.buf, nullptr);
                s.buf = VK_NULL_HANDLE;
                ctx.createBuffer(size, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                                 VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, s.buf, s.mem);
            }
            void *p = nullptr;
            if (vkMapMemory(ctx.device, s.mem, 0, VK_WHOLE_SIZE, 0, &p) != VK_SUCCESS)
                throw std::runtime_error("vkMapMemory failed");
            s.map = (const uint8_t *)p;
        }
    }
    catch (const std::exception &e)
    {
        err = std::string("FrameCapture: ") + e.what();
        destroy(ctx);
        return false;
    }
    return true;
}

void FrameCapture::destroy(VulkanContext &ctx)
{
    for (Slot &s : slots_)
    {
        if (s.mem && s.map)
            vkUnmapMemory(ctx.device, s.mem);
        if (s.buf)
            vkDestroyBuffer(ctx.device, s.buf, nullptr);
        if (s.mem)
            vkFreeMemory(ctx.device, s.mem, nullptr);
    }
    slots_.clear();
    if (scaled_)
        vkDestroyImage(ctx.device, scaled_, nullptr);
    if (scaledMem_)
        vkFreeMemory(ctx.device, scaledMem_, nullptr);
    scaled_ = VK_NULL_HANDLE;
    scaledMem_ = VK_NULL_HANDLE;
    scaledFmt_ = VK_FORMAT_UNDEFINED;
    seq_ = lastFrame_ = 0;
}

int FrameCapture::freeSlots() const
{
    int n = 0;
    for (const Slot &s : slots_)
        n += s.state.load(std::memory_order_acquire) == kFree;
    return n;
}

int FrameCapture::record(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image, VkExtent2D ext, VkFormat fmt, int tag)
{
    int si = -1;
    for (int i = 0; i < (int)slots_.size(); ++i)
        if (slots_[(size_t)i].state.load(std::memory_order_acquire) == kFree)
        {
            si = i;
            break;
        }
    if (si < 0)
        return -1;
    Slot &s = slots_[(size_t)si];
    bgra_ = (fmt == VK_FORMAT_B8G8R8A8_SRGB || fmt == VK_FORMAT_B8G8R8A8_UNORM);

    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {w_, h_, 1};
    if (ext.width >= w_ && ext.height >= h_ && ext.width - w_ <= 1 && ext.height - h_ <= 1)
    {
        // Same size (an odd last row / column cropped: H.264 wants even sizes): a straight copy.
        vkCmdCopyImageToBuffer(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf, 1, &region);
    }
    else
    {
        // Scaled: blit into an image of the output size in the SAME format (an sRGB format decodes, filters in linear
        // light and re-encodes, so the bytes stay display-encoded), then copy that.
        if (!scaled_ || scaledFmt_ != fmt)
        {
            if (scaled_)
            {
                // Only the GPU of an older frame could still read it: those are complete before anything recorded now.
                vkDestroyImage(ctx.device, scaled_, nullptr);
                vkFreeMemory(ctx.device, scaledMem_, nullptr);
            }
            ctx.createImage(w_, h_, fmt, VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT, scaled_,
                            scaledMem_);
            scaledFmt_ = fmt;
        }
        ctx.imageBarrier(cmd, scaled_, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkImageBlit blit{};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.srcOffsets[1] = {(int32_t)ext.width, (int32_t)ext.height, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        blit.dstOffsets[1] = {(int32_t)w_, (int32_t)h_, 1};
        vkCmdBlitImage(cmd, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, scaled_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1,
                       &blit, ctx.bestBlitFilter(fmt));
        ctx.imageBarrier(cmd, scaled_, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        vkCmdCopyImageToBuffer(cmd, scaled_, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, s.buf, 1, &region);
    }
    // Make the copy visible to the host once the frame's fence signals.
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0, nullptr);
    s.frame = ctx.frameIndex;
    s.seq = ++seq_;
    s.tag = tag;
    lastFrame_ = ctx.frameIndex;
    s.state.store(kGpu, std::memory_order_release);
    return si;
}
