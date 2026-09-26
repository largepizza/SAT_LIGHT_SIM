#include "App.h"
#include "Harness.h"
#include "Log.h"
#include "version.h"
#include "clay.h" // the boot screen's Clay tree (buildBootUI) — CLAY_IMPLEMENTATION lives in UIRenderer.cpp
#include <stdexcept>
#include <algorithm>
#include <cctype>
#include <cmath>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <string>
#include <thread>
#include <vector>
#include "stb_image_write.h" // boot-frame captures; the implementation lives in UIRenderer.cpp

// SATLIGHTSIM_FRAME_TRACE=1 → print a per-phase CPU wall-clock breakdown of drawFrame() every
// 60 frames. Pins down whether the frame is lost in vkWaitForFences (GPU busy), vkQueueSubmit
// (MoltenVK synchronous encode+submit), vkQueuePresentKHR (compositor/vsync stall), or the
// command recording itself.
namespace {
bool frameTraceEnabled() {
    static int v = -1;
    if (v < 0) { const char *e = std::getenv("SATLIGHTSIM_FRAME_TRACE"); v = (e && e[0] == '1') ? 1 : 0; }
    return v == 1;
}

// The loading screen is on by default, harness runs included: its frames all come before init()
// returns, i.e. before a script's first command, so a script's timing does not change and the
// harness sees what a player sees. SATLIGHTSIM_BOOT_SCREEN=0/off turns it off (run.py
// --boot-screen off): the old white-window launch, for comparison.
bool bootScreenEnabled() {
    static int v = -1;
    if (v < 0) {
        v = 1;
        if (const char *e = std::getenv("SATLIGHTSIM_BOOT_SCREEN")) {
            std::string s(e);
            for (char &c : s) c = (char)std::tolower((unsigned char)c);
            if (s == "0" || s == "off" || s == "false" || s == "no") v = 0;
        }
    }
    return v == 1;
}
}

App::App(std::unique_ptr<Simulation> s) : sim(std::move(s)) {}

void App::run() {
    initWindow();
    ctx.init(window);
    bootT0 = glfwGetTime();

    // The loading screen (BOOT_LOADER_PLAN.md). Nothing used to be presented until sim->init()
    // returned: ~3.7 s of white window. Now the UI is created FIRST (it is the only thing that can
    // draw while init runs) with its pipeline built against ctx.renderPassBoot; init() reports each
    // step through Simulation::bootStatus and every report presents a frame. Afterwards
    // rebuildPipeline() re-points the UI at ctx.renderPass for the real frames. With the screen off
    // the old ordering is unchanged.
    const bool bootScreen = bootScreenEnabled();
    if (bootScreen) {
        if (const char *e = std::getenv("SATLIGHTSIM_BOOT_CAPTURE"))
            bootCapture = harness::active() && e[0] == '1' && ctx.screenshotSupported;
        ui.init(ctx, window, ctx.renderPassBoot);
        bootFrame(); // title + version, before any loading starts
        sim->setBootStatus([this](const char *line) { bootStatus(line); });
    }

    sim->init(ctx);
    sim->setBootStatus(nullptr);
    simInited = true;
    if (bootResizeDuringInit) // the window changed size mid-init: rebuild what init sized
        sim->onResize(ctx);
    sim->setWindow(window);  // give sim access to window handle (e.g. fullscreen toggle)
    // A muted harness run gets an engine with no device: silent, and its mix is still there for
    // `audio record` to render (docs/HARNESS.md).
    audio.init(harness::active() && harness::options().mute);
    sim->setAudio(&audio);  // let the simulation configure its playlist

    if (bootScreen) {
        // What init() does not cover is the first drawFrame's own work (the lazy icon atlas, the
        // first-frame pipeline compiles); this last line holds the screen until then, and its log
        // line times the whole init.
        bootStatus("Starting");
        // The frame just submitted still uses the UI pipeline rebuildPipeline() is about to destroy
        // (and maybe the capture buffer): destroying a pipeline a pending command buffer uses is
        // undefined, and here it lost the device on the first real frame's vkQueueSubmit.
        vkDeviceWaitIdle(ctx.device);
        if (bootCapBuf != VK_NULL_HANDLE) {
            vkDestroyBuffer(ctx.device, bootCapBuf, nullptr);
            vkFreeMemory(ctx.device, bootCapMem, nullptr);
            bootCapBuf = VK_NULL_HANDLE;
            bootCapMem = VK_NULL_HANDLE;
        }
        ui.rebuildPipeline(ctx, ctx.renderPass);
    } else {
        ui.init(ctx, window);
    }
    mainLoop();
    // Wait for GPU idle before tearing down
    vkDeviceWaitIdle(ctx.device);
    audio.cleanup();
    ui.cleanup(ctx.device);
    sim->cleanup(ctx.device);
    ctx.cleanup();
    glfwDestroyWindow(window);
    glfwTerminate();
}

void App::initWindow() {
    glfwInit();
    glfwWindowHint(GLFW_CLIENT_API, GLFW_NO_API);
    // Boot maximized (windowed, not exclusive fullscreen) rather than at the fixed WIN_W x WIN_H
    // default — a small window the player has to manually enlarge undercuts the intro cinematic's
    // impact and invites fiddling with the window instead of watching it. WIN_W/WIN_H are still
    // passed as the restore size for whenever the player un-maximizes later.
    // Harness runs (docs/HARNESS.md) use a fixed, non-resizable window instead, so every capture
    // of a script has the same pixel size, and it doesn't steal focus from whatever the user is doing.
    const bool fixedWindow = harness::active() && harness::options().winW > 0;
    glfwWindowHint(GLFW_MAXIMIZED, fixedWindow ? GLFW_FALSE : GLFW_TRUE);
    if (fixedWindow)
    {
        glfwWindowHint(GLFW_RESIZABLE, GLFW_FALSE);
        glfwWindowHint(GLFW_FOCUS_ON_SHOW, GLFW_FALSE);
    }
    // macOS: render at logical (point) resolution, not the 2x Retina backing size. On the
    // integrated/older discrete GPUs these machines have, a maximized Retina framebuffer is
    // ~4x the pixels and the volumetric passes can't keep up. Also makes the fixed-bitmap
    // font atlas pixel-exact instead of upscaled. No-op on non-Apple platforms.
    glfwWindowHint(GLFW_COCOA_RETINA_FRAMEBUFFER, GLFW_FALSE);
    window = glfwCreateWindow(fixedWindow ? harness::options().winW : WIN_W,
                              fixedWindow ? harness::options().winH : WIN_H, sim->name(), nullptr, nullptr);
    glfwSetWindowUserPointer(window, this);
    glfwSetFramebufferSizeCallback(window, cbResize);
    glfwSetKeyCallback(window, cbKey);
    glfwSetCursorPosCallback(window, cbCursorPos);
    glfwSetScrollCallback(window, cbScroll);
    glfwSetCharCallback(window, cbChar);
}

void App::mainLoop() {
    lastTime = glfwGetTime();
    while (!glfwWindowShouldClose(window) && !sim->wantsQuit()) {
        double frameStart = glfwGetTime();
        glfwPollEvents();
        drawFrame();

        // NEW-7: manual pacing for numeric FPS caps (see Simulation::targetFpsCap comment —
        // FIFO/V-Sync already paces itself, this only fires for the 30/60/120 caps).
        float capHz = sim->targetFpsCap();
        if (capHz > 0.0f) {
            double budget = 1.0 / (double)capHz;
            double remaining = budget - (glfwGetTime() - frameStart);
            if (remaining > 0.0)
                std::this_thread::sleep_for(std::chrono::duration<double>(remaining));
        }
    }
}

// ─── Loading screen (BOOT_LOADER_PLAN.md) ─────────────────────────────────────
// While Simulation::init() runs, each step it reports (Simulation::bootStatus) is appended here and
// one frame is presented, so the screen always names the step in progress. A boot frame has no
// simulation in it: the same command buffer, fence and semaphores as drawFrame() (no new
// synchronization), ctx.renderPassBoot instead of the sim's pass, and nothing that touches the GPU
// timestamp slots — resolving a half-written boot frame at the next fence wait would put a bogus
// number in the first real --frame-trace output, which is also why submittedOnce is left alone.
void App::bootStatus(const char* line) {
    bootLines.emplace_back(line ? line : "");
    // One fsynced line per step, with the time since launch: the per-step cost of the launch.
    char msg[224];
    std::snprintf(msg, sizeof(msg), "boot: %s (%.0f ms)", bootLines.back().c_str(),
                  (glfwGetTime() - bootT0) * 1000.0);
    Log::line(msg);
    bootFrame();
}

void App::bootFrame() {
    // The window is up: keep the message pump fed (Esc/close work; the sim's input handlers are
    // held back until init() returns — see the callbacks at the bottom of this file).
    glfwPollEvents();

    vkWaitForFences(ctx.device, 1, &ctx.fenceFrame, VK_TRUE, UINT64_MAX);

    uint32_t imgIdx;
    VkResult res = vkAcquireNextImageKHR(ctx.device, ctx.swapchain, UINT64_MAX,
                                          ctx.semImageAvailable, VK_NULL_HANDLE, &imgIdx);
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        // The window is resizable while it boots. Same recovery as drawFrame(), except that
        // sim->onResize() waits until init() has created what it rebuilds (bootResizeDuringInit).
        ctx.recreateSwapchain(window);
        if (simInited) sim->onResize(ctx); else bootResizeDuringInit = true;
        ui.onResize(ctx);
        return;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("vkAcquireNextImageKHR failed.");

    int ww = 0, wh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    // No sim is running: no cursor, no clicks, dt 0 (nothing on this screen is time-driven).
    ui.beginFrame((float)ww, (float)wh, -1.0f, -1.0f, false, false, 0.0f, 0.0f, 0.0f);
    scrollX = scrollY = 0.0f;
    buildBootUI();

    vkResetFences(ctx.device, 1, &ctx.fenceFrame);
    vkResetCommandBuffer(ctx.commandBuffer, 0);
    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(ctx.commandBuffer, &bi);

    VkClearValue clearValues[2];
    clearValues[0].color = {{0.0f, 0.0f, 0.0f, 1.0f}}; // black, like buildBootUI's root
    clearValues[1].depthStencil = {1.0f, 0};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass      = ctx.renderPassBoot;
    rbi.framebuffer     = ctx.framebuffers[imgIdx]; // renderPassBoot is compatible with these
    rbi.renderArea      = {{0, 0}, ctx.swapExtent};
    rbi.clearValueCount = 2;
    rbi.pClearValues    = clearValues;
    vkCmdBeginRenderPass(ctx.commandBuffer, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    ui.record(ctx.commandBuffer, ctx);
    vkCmdEndRenderPass(ctx.commandBuffer);

    // Harness capture of this frame: the same PRESENT_SRC -> TRANSFER_SRC -> copy -> back sequence
    // SatelliteSim::recordScreenshotCopy uses, into a host buffer read once the fence says so.
    const uint32_t capW = ctx.swapExtent.width, capH = ctx.swapExtent.height;
    if (bootCapture) {
        const VkDeviceSize need = (VkDeviceSize)capW * capH * 4;
        if (bootCapSize != need) {
            if (bootCapBuf != VK_NULL_HANDLE) {
                vkDestroyBuffer(ctx.device, bootCapBuf, nullptr);
                vkFreeMemory(ctx.device, bootCapMem, nullptr);
            }
            ctx.createBuffer(need, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             bootCapBuf, bootCapMem);
            bootCapSize = need;
        }
        VkImage img = ctx.swapImages[imgIdx];
        ctx.imageBarrier(ctx.commandBuffer, img, VK_ACCESS_MEMORY_READ_BIT, VK_ACCESS_TRANSFER_READ_BIT,
                         VK_IMAGE_LAYOUT_PRESENT_SRC_KHR, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                         VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
        VkBufferImageCopy region{};
        region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
        region.imageExtent = {capW, capH, 1};
        vkCmdCopyImageToBuffer(ctx.commandBuffer, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, bootCapBuf, 1, &region);
        ctx.imageBarrier(ctx.commandBuffer, img, VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT,
                         VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_PRESENT_SRC_KHR,
                         VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT);
    }
    vkEndCommandBuffer(ctx.commandBuffer);

    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &ctx.semImageAvailable;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &ctx.commandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &ctx.semRenderDone[imgIdx];
    if (vkQueueSubmit(ctx.graphicsQueue, 1, &si, ctx.fenceFrame) != VK_SUCCESS)
        throw std::runtime_error("vkQueueSubmit failed.");

    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &ctx.semRenderDone[imgIdx];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &ctx.swapchain;
    pi.pImageIndices      = &imgIdx;
    res = vkQueuePresentKHR(ctx.graphicsQueue, &pi);

    if (bootCapture) {
        vkWaitForFences(ctx.device, 1, &ctx.fenceFrame, VK_TRUE, UINT64_MAX);
        bootCaptureWrite(capW, capH);
    }

    if (res == VK_ERROR_OUT_OF_DATE_KHR || resized) {
        resized = false;
        ctx.recreateSwapchain(window);
        if (simInited) sim->onResize(ctx); else bootResizeDuringInit = true;
        ui.onResize(ctx);
    }
}

void App::bootCaptureWrite(uint32_t w, uint32_t h) {
    void *mapped = nullptr;
    if (vkMapMemory(ctx.device, bootCapMem, 0, VK_WHOLE_SIZE, 0, &mapped) != VK_SUCCESS || !mapped)
        return;
    const size_t count = (size_t)w * h;
    std::vector<uint8_t> px(count * 4);
    std::memcpy(px.data(), mapped, count * 4);
    vkUnmapMemory(ctx.device, bootCapMem);
    const bool bgra = ctx.swapFormat == VK_FORMAT_B8G8R8A8_SRGB || ctx.swapFormat == VK_FORMAT_B8G8R8A8_UNORM;
    for (size_t i = 0; i < count; ++i) {
        if (bgra) std::swap(px[i * 4 + 0], px[i * 4 + 2]);
        px[i * 4 + 3] = 255;
    }
    const std::filesystem::path dir = std::filesystem::path(harness::options().outDir) / "captures";
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    char name[32];
    std::snprintf(name, sizeof(name), "boot_%02d.png", bootFrameIndex++);
    stbi_write_png_compression_level = 4;
    stbi_write_png((dir / name).string().c_str(), (int)w, (int)h, 4, px.data(), (int)w * 4);
}

// The look: black, the title and version centred, and the loading steps under them — the newest
// at the top in full white, each older one pushed down a row and fainter until it is gone. The
// intro's typography (white, the UI font, the title at 34 px), so the handover into the intro
// reads as one piece. Sizes follow the window height rather than uiScale: that setting is loaded
// inside init(), after the first loading frame is already on screen.
void App::buildBootUI() {
    int ww = 0, wh = 0;
    glfwGetWindowSize(window, &ww, &wh);
    // Never below 1: the font is one baked bitmap, and scaled much under ~16 px its thin strokes
    // break up (a faded "Milky Way" lost its "l" at 13.5 px).
    const float s = std::clamp((float)wh / 900.0f, 1.0f, 1.6f);

    // Clay keeps raw pointers until ui.record(), which runs in the same bootFrame() — so static
    // storage (and bootLines' own strings, untouched until the next frame) is enough.
    static char version[32];
    std::snprintf(version, sizeof(version), "v%s", APP_VERSION);
    Clay_String titleStr  { false, (int32_t)std::strlen(sim->name()), sim->name() };
    Clay_String versionStr{ false, (int32_t)std::strlen(version), version };

    // The fade is set in PERCEIVED brightness (1 -> 0.06 over six rows) and raised to 2.2: the UI
    // blends linearly into an sRGB swapchain, so a linear alpha step reads far brighter than it
    // says (alpha 22 of 255 still showed at ~40 % grey).
    constexpr int kShown = 6;
    static const float kPerceived[kShown] = { 1.0f, 0.60f, 0.38f, 0.23f, 0.13f, 0.06f };

    CLAY(CLAY_ID("BootRoot"), {
        .layout = {
            .sizing = { CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0) },
            .padding = { 0, 0, (uint16_t)(wh * 0.34f), 0 },
            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_TOP },
            .layoutDirection = CLAY_TOP_TO_BOTTOM },
        .backgroundColor = { 0, 0, 0, 255 } })
    {
        CLAY_TEXT(titleStr, CLAY_TEXT_CONFIG({ .textColor = { 255, 255, 255, 255 },
                                               .fontSize = (uint16_t)(34 * s) }));
        CLAY(CLAY_ID("BootGapVersion"), { .layout = { .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(8 * s) } } }) {}
        CLAY_TEXT(versionStr, CLAY_TEXT_CONFIG({ .textColor = { 120, 126, 138, 255 },
                                                 .fontSize = (uint16_t)(14 * s) }));
        CLAY(CLAY_ID("BootGapList"), { .layout = { .sizing = { CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(36 * s) } } }) {}
        const int n = (int)bootLines.size();
        for (int k = 0; k < kShown && k < n; ++k) {
            const std::string &l = bootLines[(size_t)(n - 1 - k)];
            Clay_String str{ false, (int32_t)l.size(), l.c_str() };
            CLAY(CLAY_IDI("BootLine", k), {
                .layout = { .sizing = { CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(24 * s) },
                            .childAlignment = { .x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER } } })
            {
                CLAY_TEXT(str, CLAY_TEXT_CONFIG({ .textColor = { 225, 229, 238, 255.0f * std::pow(kPerceived[k], 2.2f) },
                                                  .fontSize = (uint16_t)(16 * s) }));
            }
        }
    }
}

void App::drawFrame() {
    const bool ft = frameTraceEnabled();
    static uint64_t ftFrame = 0;
    const bool ftPrint = ft && (ftFrame % 60 == 0);
    double ftT0 = ft ? glfwGetTime() : 0.0, ftPrev = ftT0;
    auto ftMark = [&](const char *name) {
        if (!ftPrint) return;
        double now = glfwGetTime();
        std::printf("  %-14s %7.1f ms\n", name, (now - ftPrev) * 1000.0);
        ftPrev = now;
    };
    int ftRecreated = 0;

    vkWaitForFences(ctx.device, 1, &ctx.fenceFrame, VK_TRUE, UINT64_MAX);
    ftMark("waitFences");

    // Resolve last frame's GPU timestamp queries now that the fence proves the GPU
    // is done with them. Skipped on the very first call: the fence starts pre-signaled
    // so nothing has actually been submitted yet, and there'd be no query data to read.
    if (submittedOnce) {
        ctx.resolveTimestamps();
        // UC6: same reasoning — a screenshot copy recorded last frame is only safe to read back
        // once this fence wait proves the GPU has finished writing it.
        sim->finalizeScreenshot();
    }

    uint32_t imgIdx;
    VkResult res = vkAcquireNextImageKHR(ctx.device, ctx.swapchain, UINT64_MAX,
                                          ctx.semImageAvailable, VK_NULL_HANDLE, &imgIdx);
    ftMark("acquire");
    if (res == VK_ERROR_OUT_OF_DATE_KHR) {
        ctx.recreateSwapchain(window);
        sim->onResize(ctx);
        ui.onResize(ctx);
        if (ft) { ++ftFrame; std::printf("[frame-trace] acquire OUT_OF_DATE -> full swapchain recreate\n"); }
        return;
    }
    if (res != VK_SUCCESS && res != VK_SUBOPTIMAL_KHR)
        throw std::runtime_error("vkAcquireNextImageKHR failed.");

    // Compute dt. Clamped only against genuine multi-second hitches (first-frame shader
    // compile, window resize, breakpoint) — NOT against a merely slow GPU. The old 0.05s
    // (20 fps) ceiling silently pinned the HUD fps badge and every perf snapshot at 20 on
    // hardware actually running slower, and ran the sim in slow-motion to match. 1.0s (1 fps)
    // still bounds a pathological stall while letting true frame times through even on very
    // slow setups (MoltenVK translation on old GPUs can genuinely land here).
    double now = glfwGetTime();
    float  dt  = std::min((float)(now - lastTime), 1.0f);
    lastTime   = now;
    dt = sim->frameDt(dt); // harness: fixed step (docs/HARNESS.md)

    // Get current mouse state
    double mx, my;
    glfwGetCursorPos(window, &mx, &my);
    bool lmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_LEFT)  == GLFW_PRESS;
    bool rmb = glfwGetMouseButton(window, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
    int  ww, wh;
    glfwGetWindowSize(window, &ww, &wh);

    // While the camera has the cursor captured (GLFW_CURSOR_DISABLED, used for
    // RMB mouse-look), GLFW reports an unbounded "virtual" position that free-drifts
    // far outside the window as the user pans. Feeding that raw value to the UI made
    // Clay's hit-testing register phantom hovers/clicks (rollover blips, accidental
    // keybind rebinds) on whatever always-visible panel the drifted coordinate landed
    // on. Freeze the UI-facing cursor at its last known on-screen position instead.
    if (glfwGetInputMode(window, GLFW_CURSOR) == GLFW_CURSOR_DISABLED) {
        mx = uiMouseX;
        my = uiMouseY;
    } else {
        uiMouseX = mx;
        uiMouseY = my;
    }

    // UC4: gamepad virtual cursor — when active, overrides the real mouse position/click state
    // for this frame's UI pass so every existing Clay_Hovered()/click handler works unmodified.
    float vx, vy;
    bool  vClick;
    if (sim->virtualCursor(vx, vy, vClick)) {
        mx  = vx;
        my  = vy;
        lmb = vClick;
    }

    // Prepare Clay layout for this frame — simulation may call CLAY() in buildUI()
    ui.beginFrame((float)ww, (float)wh,
                  (float)mx, (float)my, lmb, rmb,
                  scrollX, scrollY, dt);
    scrollX = scrollY = 0.0f; // consumed

    // Advance music playlist (detects track end, starts next track).
    audio.update(dt);

    // Let the simulation declare its UI elements and read input state via ui.
    sim->buildUI(dt, ui);
    ftMark("buildUI");

    // Record GPU commands
    vkResetFences(ctx.device, 1, &ctx.fenceFrame);
    vkResetCommandBuffer(ctx.commandBuffer, 0);

    VkCommandBufferBeginInfo bi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    vkBeginCommandBuffer(ctx.commandBuffer, &bi);

    // GPU timestamp profiling: slot 0 marks frame start. Slots 1-5 are written inside
    // sim->recordCompute() (compute-pass breakdown: scene depth, beam cloud block, orbit compute,
    // cloud march, flare compute); slot 6 is written inside sim->recordPrePass() or
    // sim->recordDraw() (end of the sky background pass, whichever path rendered it).
    // Slots 7-8 mark the end of the satellite+star draw and the UI overlay respectively.
    // See VulkanContext::kTimestampCount for the authoritative slot table.
    ctx.resetTimestamps(ctx.commandBuffer);
    ctx.writeTimestamp(ctx.commandBuffer, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, 0);

    // 1. Simulation compute work (before render pass)
    sim->recordCompute(ctx.commandBuffer, ctx, dt);
    ftMark("recordCompute");

    // 1b. Optional offscreen pre-pass (e.g. a low-res background blitted into the swapchain
    // image ahead of time) — must run before the main render pass begins. Default: no-op.
    sim->recordPrePass(ctx.commandBuffer, ctx, dt, imgIdx);

    // 2. Begin render pass — App now owns this
    VkClearValue clearValues[2];
    clearValues[0] = sim->clearColor();
    clearValues[1].depthStencil = {1.0f, 0};   // far depth = 1.0
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.sType           = VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO;
    rbi.renderPass      = sim->activeRenderPass(ctx);
    rbi.framebuffer     = ctx.framebuffers[imgIdx];
    rbi.renderArea      = {{0, 0}, ctx.swapExtent};
    rbi.clearValueCount = 2;
    rbi.pClearValues    = clearValues;
    vkCmdBeginRenderPass(ctx.commandBuffer, &rbi, VK_SUBPASS_CONTENTS_INLINE);

    // 3. Simulation draw calls (render pass is already open)
    sim->recordDraw(ctx.commandBuffer, ctx, dt);
    ctx.writeTimestamp(ctx.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 7);

    // 4. UI draws on top of the simulation — UC6 clean-shot mode skips this for a captured frame
    // (nobody shares a screenshot with a settings panel in it).
    if (!sim->wantsCleanScreenshot())
        ui.record(ctx.commandBuffer, ctx);
    ctx.writeTimestamp(ctx.commandBuffer, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 8);

    vkCmdEndRenderPass(ctx.commandBuffer);

    // UC6: record the screenshot copy (a no-op unless one is actually pending) now that the
    // swapchain image holds the fully composited frame — must happen after the render pass ends
    // (the image is back in a layout a transfer op can act on) and before the command buffer is
    // ended, since this project has one command buffer / one frame in flight.
    sim->recordScreenshotCopy(ctx.commandBuffer, ctx, ctx.swapImages[imgIdx]);

    vkEndCommandBuffer(ctx.commandBuffer);
    ftMark("record rest");

    // Submit
    // Includes TRANSFER now (not just COLOR_ATTACHMENT_OUTPUT): a simulation's recordPrePass may
    // blit directly into the swapchain image before the main render pass opens (resolution
    // scaling) — that write must also wait for the presentation engine to be done with this
    // image, the same guarantee the render pass itself already got from this semaphore.
    VkPipelineStageFlags waitStage = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT;
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.waitSemaphoreCount   = 1;
    si.pWaitSemaphores      = &ctx.semImageAvailable;
    si.pWaitDstStageMask    = &waitStage;
    si.commandBufferCount   = 1;
    si.pCommandBuffers      = &ctx.commandBuffer;
    si.signalSemaphoreCount = 1;
    si.pSignalSemaphores    = &ctx.semRenderDone[imgIdx];
    if (vkQueueSubmit(ctx.graphicsQueue, 1, &si, ctx.fenceFrame) != VK_SUCCESS)
        throw std::runtime_error("vkQueueSubmit failed.");
    submittedOnce = true;
    ftMark("queueSubmit");

    // Present
    VkPresentInfoKHR pi{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
    pi.waitSemaphoreCount = 1;
    pi.pWaitSemaphores    = &ctx.semRenderDone[imgIdx];
    pi.swapchainCount     = 1;
    pi.pSwapchains        = &ctx.swapchain;
    pi.pImageIndices      = &imgIdx;
    res = vkQueuePresentKHR(ctx.graphicsQueue, &pi);
    ftMark("queuePresent");
    // NOTE: VK_SUBOPTIMAL_KHR is deliberately NOT a trigger here. MoltenVK returns it on almost
    // every present on Retina/scaled displays, and recreateSwapchain() rebuilds every graphics
    // pipeline (viewport is baked in) — on MoltenVK that re-converts SPIR-V→MSL and recompiles
    // sat_sky.frag et al. into fresh MTLRenderPipelineStates, ~hundreds of ms. Doing that every
    // frame pinned the whole app at a few fps. SUBOPTIMAL still presents correctly; only a real
    // size change (resized flag) or OUT_OF_DATE actually needs a rebuild. The acquire path above
    // already ignores SUBOPTIMAL for the same reason.
    if (res == VK_ERROR_OUT_OF_DATE_KHR || resized || sim->consumeSwapchainRebuildRequest()) {
        resized = false;
        ctx.recreateSwapchain(window);
        sim->onResize(ctx);
        ui.onResize(ctx);
        ftRecreated = 1;
    }
    ftMark("post-present");

    if (ftPrint) {
        std::printf("[frame-trace] frame %llu  present_res=%d  recreated=%d  TOTAL %.1f ms\n\n",
                    (unsigned long long)ftFrame, (int)res, ftRecreated,
                    (glfwGetTime() - ftT0) * 1000.0);
        std::fflush(stdout);
    }
    if (ft) ++ftFrame;
}

void App::cbResize(GLFWwindow* w, int, int) {
    reinterpret_cast<App*>(glfwGetWindowUserPointer(w))->resized = true;
}

void App::cbKey(GLFWwindow* w, int key, int, int action, int) {
    auto* app = reinterpret_cast<App*>(glfwGetWindowUserPointer(w));
    // The loading screen pumps events while init() is still running; the sim's handlers wait for
    // it to finish (Esc still closes the window, and mainLoop() then exits straight away).
    if (!app->simInited) {
        if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS) glfwSetWindowShouldClose(w, GLFW_TRUE);
        return;
    }
    // Esc quits — unless a text field (the harness console) has the keyboard; it closes that instead.
    if (key == GLFW_KEY_ESCAPE && action == GLFW_PRESS && !app->sim->capturesKeyboard())
        glfwSetWindowShouldClose(w, GLFW_TRUE);
    app->sim->onKey(w, key, action);
}

void App::cbChar(GLFWwindow* w, unsigned int codepoint) {
    auto* app = reinterpret_cast<App*>(glfwGetWindowUserPointer(w));
    if (app->simInited) app->sim->onChar(w, codepoint);
}

void App::cbCursorPos(GLFWwindow* w, double x, double y) {
    auto* app = reinterpret_cast<App*>(glfwGetWindowUserPointer(w));
    if (app->simInited) app->sim->onCursorPos(w, x, y);
}

void App::cbScroll(GLFWwindow* w, double dx, double dy) {
    auto* app = reinterpret_cast<App*>(glfwGetWindowUserPointer(w));
    app->scrollX += (float)dx;
    app->scrollY += (float)dy;
}
