// Video export (2026-10-25): the player's recording (KB_VIDEO, the HUD's video button), the harness's `video` /
// `path play ... video=` (SatelliteSimHarness.cpp), and the harness's settle probe (`wait settle auto`).
// The frame-side pieces: VideoRecorder.h / FrameCapture.h (src/). See CLAUDE.md "Video export".
#include "../FrameProfile.h"
#include "../Harness.h"
#include "SatelliteSim.h"
#include "../Log.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "clay.h"
#include "UIPalette.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>

namespace fs = std::filesystem;

const char *keyDisplayName(int key); // SatelliteSimUI.cpp

// ─── Start / stop ─────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::videoStart(const VideoRecordOptions &o, bool byHarness, std::string &err)
{
    if (!ctx_)
    {
        err = "no Vulkan context";
        return false;
    }
    if (video_.busy())
    {
        err = video_.recording() ? "already recording" : "the previous video is still being written";
        return false;
    }
    // The swapchain's size (the photo target is never recorded: App skips the copy while it is up).
    const uint32_t w = ctx_->photoActive ? 0 : ctx_->swapExtent.width, h = ctx_->photoActive ? 0 : ctx_->swapExtent.height;
    if (w < 16 || h < 16)
    {
        err = "no window to record (minimised, or an HQ photo is being taken)";
        return false;
    }
    const double t0 = harness::nowS();
    if (!video_.start(*ctx_, w, h, o, err))
    {
        Log::line("video: start failed: " + err);
        return false;
    }
    videoByHarness_ = byHarness;
    Log::line("video: recording " + o.path + " (" + video_.stats().value("encoder", std::string()) + ", " +
              std::to_string(video_.stats().value("width", 0)) + "x" + std::to_string(video_.stats().value("height", 0)) +
              " @ " + std::to_string((int)o.fps) + " fps, opened in " + std::to_string((int)((harness::nowS() - t0) * 1000.0)) +
              " ms)");
    return true;
}

void SatelliteSim::videoStop()
{
    if (video_.recording())
        video_.stop();
}

void SatelliteSim::videoToggle()
{
    if (video_.recording())
    {
        video_.stop();
        if (!videoByHarness_)
        {
            snprintf(screenshotToastText, sizeof(screenshotToastText), "Finishing the video...");
            screenshotToastTimer = 2.0f;
            screenshotToastWarn = false;
        }
        return;
    }
    if (video_.busy())
        return; // still writing the last one
    std::error_code ec;
    fs::path dir = fs::path(userDataDir_.empty() ? exeDir_ : userDataDir_) / "videos";
    fs::create_directories(dir, ec);
    std::time_t now = std::time(nullptr);
    struct tm *lt = localtime(&now);
    char nameBuf[64];
    if (lt)
        strftime(nameBuf, sizeof(nameBuf), "satlight_%Y-%m-%d_%H-%M-%S", lt);
    else
        snprintf(nameBuf, sizeof(nameBuf), "satlight_video");
    fs::path p = dir / (std::string(nameBuf) + ".mp4");
    for (int k = 2; k < 100 && fs::exists(p, ec); ++k)
        p = dir / (std::string(nameBuf) + "_" + std::to_string(k) + ".mp4");
    VideoRecordOptions o;
    o.path = p.string();
    o.fps = videoFpsSetting;
    o.scale = videoScaleSetting;
    o.ui = videoUiSetting;
    std::string err;
    if (!videoStart(o, false, err))
    {
        snprintf(screenshotToastText, sizeof(screenshotToastText), "Video failed: %.120s", err.c_str());
        screenshotToastTimer = 5.0f;
        screenshotToastWarn = true;
        return;
    }
    snprintf(screenshotToastText, sizeof(screenshotToastText), "Recording video (%s to stop)",
             keyDisplayName(keybindings[KB_VIDEO].key));
    screenshotToastTimer = 2.5f;
    screenshotToastWarn = false;
}

// ─── Per frame ────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::videoFrameTick(float dt)
{
    if (!video_.recording())
        return;
    // A harness settle frame is 0.5 s of frame time with the sim held: one video frame each (the convergence plays
    // back fast instead of as 30 copies of every settle frame).
    // (The frame a settle ends in still has its 0.5-s step: a command after the settle runs in that frame's buildUI.)
    const bool settleFrame = harnessSettleFrames_ > 0 || (harnessRunner_ && dt >= 0.99f * kHarnessSettleDt);
    const double d = settleFrame ? 1.0 / video_.options().fps : (double)dt;
    video_.tick(d);
}

bool SatelliteSim::wantsPreUiCopy() const
{
    return (video_.wantsCapture() && !video_.options().ui) || settleProbeWanted_;
}

void SatelliteSim::recordPreUiCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image)
{
    FPROF_SCOPE("video.preUiCopy");
    // The main pass ended in PRESENT_SRC_KHR; its colour writes must be visible to the transfer.
    ctx.imageBarrier(cmd, image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    if (video_.wantsCapture() && !video_.options().ui)
        video_.recordCopy(cmd, ctx, image, ctx.swapExtent, ctx.swapFormat);
    if (settleProbeWanted_ && settleProbe_.created())
        settleProbe_.record(cmd, ctx, image, ctx.swapExtent, ctx.swapFormat, 0);
    settleProbeWanted_ = false;
    // The UI is drawn next in the load variant of the main pass, whose initial layout is TRANSFER_DST_OPTIMAL.
    ctx.imageBarrier(cmd, image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
}

void SatelliteSim::videoPostUiCopy(VkCommandBuffer cmd, VulkanContext &ctx, VkImage image)
{
    if (ctx.photoActive || !video_.wantsCapture() || !video_.options().ui)
        return;
    FPROF_SCOPE("video.copy");
    ctx.imageBarrier(cmd, image, VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                     VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                     VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    video_.recordCopy(cmd, ctx, image, ctx.swapExtent, ctx.swapFormat);
    ctx.imageBarrier(cmd, image, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                     VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                     VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
}

void SatelliteSim::videoAfterFence()
{
    if (!ctx_)
        return;
    FPROF_SCOPE("video.afterFence");
    if (video_.poll(*ctx_))
    {
        const nlohmann::json &r = video_.lastResult();
        videoLastFile_ = r.value("file", std::string());
        Log::line("video: done " + r.dump());
        if (!videoByHarness_)
        {
            if (r.contains("error"))
                snprintf(screenshotToastText, sizeof(screenshotToastText), "Video failed: %.120s",
                         r["error"].get<std::string>().c_str());
            else
                snprintf(screenshotToastText, sizeof(screenshotToastText), "Saved %s (%.0f s)",
                         fs::path(videoLastFile_).filename().string().c_str(), r.value("duration_s", 0.0));
            screenshotToastTimer = 4.0f;
            screenshotToastWarn = r.contains("error");
        }
    }
    settleProbeCollect();
}

// ─── The REC badge ────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildVideoHud(const UIInput &inp, UIRenderer &ui)
{
    (void)inp;
    (void)ui;
    if (!video_.busy() || videoByHarness_)
        return;
    static char buf[96];
    const double s = video_.secondsRecorded();
    if (video_.recording())
        snprintf(buf, sizeof(buf), "REC  %d:%02d   %s to stop", (int)s / 60, (int)s % 60,
                 keyDisplayName(keybindings[KB_VIDEO].key));
    else
        snprintf(buf, sizeof(buf), "Writing the video...");
    Clay_String str{false, (int32_t)strlen(buf), buf};
    CLAY(CLAY_ID("VideoRecBadge"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {10, 10, 5, 5}},
                                    .backgroundColor = Pal::recActive,
                                    .cornerRadius = CLAY_CORNER_RADIUS(4),
                                    .floating = {.offset = {12, 12}, .zIndex = 30, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        CLAY_TEXT(str, CLAY_TEXT_CONFIG({.textColor = Pal::textBright, .fontSize = fs(13)}));
    }
}

// ─── The harness's settle probe ───────────────────────────────────────────────────────────────────
void SatelliteSim::settleProbeReset()
{
    settleLum_.clear();
    settleTrace_.clear();
    settleTrace32_.clear();
    if (!ctx_)
        return;
    // ~1/4 of the window: fine enough to see grain settle, cheap to compare (~120 kB a copy).
    const uint32_t w = std::max(16u, ctx_->swapExtent.width / 4) & ~1u, h = std::max(16u, ctx_->swapExtent.height / 4) & ~1u;
    if (!settleProbe_.created() || settleProbe_.width() != w || settleProbe_.height() != h)
    {
        if (settleProbe_.created())
            ctx_->drainFrames(); // (only on a size change)
        std::string err;
        if (!settleProbe_.create(*ctx_, w, h, 4, err))
            Log::line("harness: settle probe: " + err);
    }
}

void SatelliteSim::settleProbeCollect()
{
    if (!ctx_ || !settleProbe_.created())
        return;
    settleProbe_.collect(*ctx_, [&](int slot, const FrameCapture::Slot &s)
    {
        const uint32_t w = settleProbe_.width(), h = settleProbe_.height();
        const bool bgra = settleProbe_.bgra();
        std::vector<float> L((size_t)w * h);
        const uint8_t *p = s.map;
        for (size_t i = 0; i < L.size(); ++i, p += 4)
        {
            const float r = p[bgra ? 2 : 0], g = p[1], b = p[bgra ? 0 : 2];
            L[i] = 0.2126f * r + 0.7152f * g + 0.0722f * b;
        }
        settleProbe_.release(slot);
        auto diff = [&](size_t lag)
        {
            if (settleLum_.size() < lag)
                return -1.0f;
            const std::vector<float> &o = settleLum_[settleLum_.size() - lag];
            double sum = 0.0;
            for (size_t i = 0; i < L.size(); ++i)
                sum += std::fabs(L[i] - o[i]);
            return (float)(sum / (double)L.size());
        };
        settleTrace_.push_back(diff(8));
        settleTrace32_.push_back(diff(32));
        settleLum_.push_back(std::move(L));
        while (settleLum_.size() > 32)
            settleLum_.pop_front();
    });
}
