#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include "VulkanContext.h"

class UIRenderer;   // forward declare — simulations include UIRenderer.h in their .cpp
class AudioSystem;  // forward declare — simulations include AudioSystem.h in their .cpp

// Abstract base class for all simulations.
// To add a new simulation:
//   1. Create MySimulation.h/.cpp in src/simulations/
//   2. Inherit from Simulation and implement all pure virtual methods
//   3. In main.cpp, construct App with std::make_unique<MySimulation>()
class Simulation {
public:
    virtual ~Simulation() = default;

    // Called once after Vulkan context is ready.
    // Create all GPU resources (images, buffers, pipelines, descriptors) here.
    virtual void init(VulkanContext& ctx) = 0;

    // Called when the swapchain is recreated (e.g. window resize).
    // Recreate any pipeline that bakes in the viewport size.
    // Compute pipelines are viewport-independent and do NOT need recreation.
    virtual void onResize(VulkanContext& ctx) = 0;

    // Record compute work into an already-begun command buffer, before the render pass.
    // Default implementation does nothing (simulations with no compute override this).
    // dt = seconds since last frame.
    virtual void recordCompute(VkCommandBuffer cmd, VulkanContext& ctx, float dt) {}

    // Record work AFTER recordCompute but BEFORE the main render pass begins — e.g. an offscreen
    // low-resolution pass whose result gets blitted into the swapchain image ahead of time, for
    // simulations that pre-populate the framebuffer instead of letting the main render pass clear
    // it (see activeRenderPass). imgIdx identifies which swapchain image this frame targets (the
    // render pass itself only needs a framebuffer, but a pre-pass blit operates on the raw
    // VkImage directly, outside the render pass abstraction). Default implementation does nothing.
    virtual void recordPrePass(VkCommandBuffer cmd, VulkanContext& ctx, float dt, uint32_t imgIdx) {}

    // Which render pass App should begin for this frame's main draw pass. Default: ctx.renderPass
    // (the normal CLEAR-based pass). A simulation that used recordPrePass to pre-fill the
    // framebuffer via a blit should return a LOAD-based render pass instead (same attachment
    // formats, so it stays compatible with the existing ctx.framebuffers — render pass
    // compatibility only requires matching format/sample-count, not matching load/store ops) so
    // the pre-pass's contents survive into the main pass instead of being cleared away.
    virtual VkRenderPass activeRenderPass(VulkanContext& ctx) { return ctx.renderPass; }

    // Record draw calls into an already-begun command buffer inside an open render pass.
    // The render pass is begun and ended by App; do NOT call vkCmdBeginRenderPass here.
    // dt = seconds since last frame.
    virtual void recordDraw(VkCommandBuffer cmd, VulkanContext& ctx, float dt) = 0;

    // Declare Clay UI layout elements for this frame.
    // Called between UIRenderer::beginFrame() and UIRenderer::record().
    // ui provides per-frame input state (mouse, buttons) and the mouse-capture API.
    // Use CLAY() and CLAY_TEXT() macros to emit layout elements.
    // Default implementation declares no UI.
    virtual void buildUI(float dt, UIRenderer& ui) {}

    // Clear color used when App begins the render pass.
    virtual VkClearValue clearColor() const { return {{{0.0f, 0.0f, 0.0f, 1.0f}}}; }

    // Called once before the device is destroyed. Release all Vulkan resources.
    virtual void cleanup(VkDevice device) = 0;

    // Optional input handlers — default implementations do nothing.
    virtual void onKey(GLFWwindow* window, int key, int action) {}
    virtual void onCursorPos(GLFWwindow* window, double x, double y) {}

    // Called by App after both the simulation and AudioSystem are initialised.
    // Override to configure the playlist and store the pointer for use in buildUI.
    // Default: no-op (simulations without audio ignore this).
    virtual void setAudio(AudioSystem* /*audio*/) {}

    // Called by App immediately after init(). Gives the simulation access to the
    // native window handle for operations like fullscreen toggling.
    // Default: no-op.
    virtual void setWindow(GLFWwindow* /*window*/) {}

    // Window title shown while this simulation runs
    virtual const char* name() const { return "SAT LIGHT SIM"; }

    // NEW-7 (RELEASE_v1_1_PLAN.md): target frame rate for App-side pacing, in Hz. Vulkan present
    // modes have no native "cap to N fps" concept — FIFO paces to the display's own refresh rate,
    // and MAILBOX/IMMEDIATE just run flat out — so a fixed numeric cap (e.g. "60" on a 144 Hz
    // panel) needs App::mainLoop to sleep out the difference itself. Return 0 (default) for "no
    // manual pacing needed" — either genuinely uncapped, or already paced by FIFO/V-Sync.
    virtual float targetFpsCap() const { return 0.0f; }

    // NEW-7: one-shot flag. Return true exactly once when this simulation has changed
    // VulkanContext::presentModePreference (e.g. the Settings > Display frame-limiter changed)
    // and needs App to rebuild the swapchain with it — mirrors the same recreateSwapchain +
    // onResize sequence App already runs after a window resize. Must clear its own pending state
    // before returning true, since App calls this once per frame and won't call it again.
    virtual bool consumeSwapchainRebuildRequest() { return false; }

    // UC4 (RELEASE_v1_1_PLAN.md): "cheap 90%" gamepad UI navigation — a virtual cursor driven by
    // the right stick instead of full focus-based traversal. Called by App right before
    // ui.beginFrame() each frame; when this returns true, App overrides that frame's real mouse
    // position and left-click state with (x, y, lmb) before building the UI, so every existing
    // Clay_Hovered()/click handler works unmodified. Default: no override (mouse/touch-only
    // simulations ignore this).
    virtual bool virtualCursor(float& x, float& y, bool& lmb) const { return false; }

    // ── UC6: screenshots (RELEASE_v1_1_PLAN.md) ────────────────────────────────
    // True for exactly one frame — the frame a screenshot was requested and should be captured
    // "clean" (no UI overlay). Checked by App before ui.record() to decide whether to skip it;
    // read-only (does not consume/clear anything) so it can be checked without side effects.
    virtual bool wantsCleanScreenshot() const { return false; }

    // Called once per frame, right after the render pass ends (image is in PRESENT_SRC_KHR
    // layout, holding the final composited frame — including UI unless wantsCleanScreenshot()
    // suppressed it) and before the command buffer is ended. A no-op unless a screenshot is
    // actually pending. Implementations that need one should record a layout transition + copy of
    // `image` into their own staging buffer here; the buffer isn't safe to read until the fence
    // for THIS submission is waited on, which happens at the top of the NEXT drawFrame — see
    // finalizeScreenshot(). Default: no-op.
    virtual void recordScreenshotCopy(VkCommandBuffer /*cmd*/, VulkanContext& /*ctx*/, VkImage /*image*/) {}

    // Called once per frame, right after App waits on the frame fence (same point
    // ctx.resolveTimestamps() already reads back the previous frame's GPU timestamps — the
    // established "read back what last frame's GPU work produced" point in this codebase) —
    // finishes any screenshot whose copy was recorded last frame: maps the staging buffer,
    // encodes, writes the file, clears pending state. Default: no-op.
    virtual void finalizeScreenshot() {}
};
