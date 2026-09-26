#pragma once
#define GLFW_INCLUDE_VULKAN
#define GLFW_INCLUDE_NONE // see VulkanContext.h — must be repeated at every raw glfw3.h include site
#include <GLFW/glfw3.h>
#include <memory>
#include <string>
#include <vector>
#include "VulkanContext.h"
#include "Simulation.h"
#include "UIRenderer.h"
#include "AudioSystem.h"

class App {
public:
    // Pass ownership of the simulation to run.
    explicit App(std::unique_ptr<Simulation> sim);
    void run();

private:
    GLFWwindow*              window   = nullptr;
    bool                     resized  = false;
    double                   lastTime = 0.0;
    bool                     submittedOnce = false; // guards the first-frame timestamp resolve (fence starts pre-signaled)
    // BOOT_LOADER_PLAN.md Phase A: true once sim->init(ctx) has returned. The boot frames present
    // before it, and the sim-facing halves of their resize/rebuild handling must stay off until it
    // set — SatelliteSim::onResize() destroys and recreates the sim's own resources, so calling it
    // before init() made them is a use of uninitialized handles.
    bool                     simInited = false;
    double                   bootT0    = 0.0; // glfwGetTime() at the top of run(); boot-screen clock
    // Loading screen state. bootLines holds every status the sim reported, oldest first; the screen
    // shows the newest few, fading with age. bootResizeDuringInit: the swapchain was recreated while
    // init() was still running, so the sim's size-dependent resources are stale and need one
    // onResize() once init returns (it cannot be called mid-init).
    std::vector<std::string> bootLines;
    bool                     bootResizeDuringInit = false;
    // SATLIGHTSIM_BOOT_CAPTURE=1 in a harness run: every loading-screen frame is also written to
    // <run>/captures/boot_NN.png, so the screen can be checked without a person watching it.
    bool                     bootCapture = false;
    int                      bootFrameIndex = 0;
    VkBuffer                 bootCapBuf = VK_NULL_HANDLE;
    VkDeviceMemory           bootCapMem = VK_NULL_HANDLE;
    VkDeviceSize             bootCapSize = 0;
    VulkanContext            ctx;
    std::unique_ptr<Simulation> sim;
    UIRenderer               ui;
    AudioSystem              audio;
    float                    scrollX  = 0.0f; // accumulated scroll for Clay
    float                    scrollY  = 0.0f;
    double                   uiMouseX = 0.0; // last cursor pos while GLFW_CURSOR_NORMAL
    double                   uiMouseY = 0.0; // (frozen during camera-look capture; see drawFrame)

    void initWindow();
    void mainLoop();
    void drawFrame();
    void bootStatus(const char* line);   // append a loading-screen line, log it, present a frame
    void bootFrame();                    // one frame of the loading screen (BOOT_LOADER_PLAN.md)
    void buildBootUI();                  // its Clay tree — called between beginFrame and record
    void bootCaptureWrite(uint32_t w, uint32_t h); // PNG of the frame just copied (bootCapture)

    static void cbResize(GLFWwindow* w, int, int);
    static void cbKey(GLFWwindow* w, int key, int scancode, int action, int mods);
    static void cbCursorPos(GLFWwindow* w, double x, double y);
    static void cbChar(GLFWwindow* w, unsigned int codepoint);
    static void cbScroll(GLFWwindow* w, double dx, double dy);
};
