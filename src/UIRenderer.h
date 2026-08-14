#pragma once
#define GLFW_INCLUDE_VULKAN
#include <GLFW/glfw3.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>

struct VulkanContext;   // forward declare
struct Clay_ElementId;  // forward declare (full definition in clay.h, included by UIRenderer.cpp
                        // and by any .cpp that calls scrollbar())

// Single vertex format for all UI geometry (rectangles, text glyphs, icons).
// The fragment shader switches rendering mode based on `mode`.
// localPos/halfSize/cornerRadius drive a rounded-rect SDF in the fragment shader
// (mode=0 only) — Clay reports a per-element cornerRadius but the renderer never
// actually read it before; every "rounded" panel/button/window rendered as a
// plain sharp-cornered rect regardless of the CLAY_CORNER_RADIUS() value.
struct UIVertex {
    glm::vec2 pos;          // screen-space pixels, origin top-left
    glm::vec2 uv;           // atlas UV (font atlas for mode=1, icon atlas for mode=2)
    glm::vec4 color;        // RGBA [0,1]
    float     mode;         // 0.0 = solid rectangle, 1.0 = text glyph, 2.0 = icon sprite
    glm::vec2 localPos;     // position relative to the rect's center, in pixels
    glm::vec2 halfSize;     // rect half-width/half-height, in pixels
    glm::vec4 cornerRadius; // topLeft, topRight, bottomLeft, bottomRight (pixels) — matches Clay_CornerRadius field order
};

struct UIPushConstants {
    glm::vec2 screenSize; // viewport size in pixels, used by vertex shader for NDC conversion
};

// Per-frame mouse, button, and scroll state — read by simulations in buildUI().
struct UIInput {
    float screenW  = 0, screenH  = 0; // window dimensions in pixels
    float mouseX   = 0, mouseY   = 0; // current cursor position
    float dMouseX  = 0, dMouseY  = 0; // cursor delta since last frame
    bool  lmbDown     = false; // left mouse button held
    bool  lmbPressed  = false; // went down this frame
    bool  lmbReleased = false; // went up this frame
    bool  rmbDown     = false; // right mouse button held
    bool  rmbPressed  = false;
    bool  rmbReleased = false;
    float scrollY  = 0.0f;    // vertical scroll delta this frame (positive = scroll up)
    float dt = 0;
};

// Bitmask of which edge(s) of a window are being hovered/dragged for resize.
// A single bit = a straight edge (EW/NS cursor); two adjacent bits = a corner
// (NWSE/NESW cursor). See UIRenderer::updateWindowChrome.
enum WindowResizeEdge : uint8_t {
    kResizeNone   = 0,
    kResizeLeft   = 1 << 0,
    kResizeRight  = 1 << 1,
    kResizeTop    = 1 << 2,
    kResizeBottom = 1 << 3,
};

// Shared drag/resize state for a movable, resizable Clay window or panel.
// -1 x/y is the "not yet placed" sentinel (existing convention) — the owning
// sim centers/places the window on first open. w/h have no such sentinel; the
// owner must set them once (e.g. a default size) before the first
// updateWindowChrome() call.
struct WindowChrome {
    bool    open       = false;
    float   x = -1.0f, y = -1.0f;
    float   w = 0.0f, h = 0.0f;
    bool    dragging   = false;
    uint8_t resizeEdge = kResizeNone;

    // Internal to UIRenderer::updateWindowChrome — anchor snapshot (mouse position +
    // window rect at the moment a drag/resize begins) so tracking stays absolute
    // rather than accumulating per-frame deltas. Without this, once a min/max size
    // or screen-edge clamp caps the window for a moment, the cursor and the window
    // edge drift apart permanently (the edge keeps matching the clamped rate of
    // change instead of re-snapping to the cursor once back in range).
    bool  dragAnchored_   = false;
    float dragStartMouseX_ = 0, dragStartMouseY_ = 0, dragStartX_ = 0, dragStartY_ = 0;
    bool  resizeAnchored_  = false;
    float resizeStartMouseX_ = 0, resizeStartMouseY_ = 0;
    float resizeStartX_ = 0, resizeStartY_ = 0, resizeStartW_ = 0, resizeStartH_ = 0;
};

class UIRenderer {
public:
    // Call after VulkanContext is initialized. `window` is used only to set OS resize
    // cursors while hovering/dragging a window edge (see updateWindowChrome).
    void init(VulkanContext& ctx, GLFWwindow* window);

    // Call when swapchain is recreated (window resize).
    void onResize(VulkanContext& ctx);

    // Release all Vulkan resources.
    void cleanup(VkDevice device);

    // Call once at the start of each frame, before the simulation's buildUI().
    void beginFrame(float width, float height,
                    float mouseX, float mouseY, bool lmbDown, bool rmbDown,
                    float scrollDeltaX, float scrollDeltaY,
                    float dt);

    // Call inside the render pass, after sim->recordDraw() and before vkCmdEndRenderPass.
    void record(VkCommandBuffer cmd, VulkanContext& ctx);

    // Per-frame input state — read by simulations in buildUI().
    const UIInput& input() const { return frameInput; }

    // Register a screen rect that should absorb mouse events (toolbar, open windows, etc.).
    void addMouseCaptureRect(float x, float y, float w, float h);

    // True if mouse is currently over any registered capture rect (one-frame lag).
    bool mouseOverUI() const { return prevMouseOverUI; }

    // Font IDs for CLAY_TEXT_CONFIG.
    uint16_t defaultFontId() const { return 0; }

    // ── Icon atlas ────────────────────────────────────────────────────────────
    // Load PNG icons from disk and pack into a horizontal GPU atlas.
    // Call from buildUI() on first frame (lazy init); safe to call once.
    // Returns the number of icons successfully loaded.
    int loadIcons(VulkanContext& ctx, const char* const* paths, int count);

    // Number of icons currently loaded.
    int iconCount() const { return (int)iconEntries.size(); }

    // ── Window chrome (drag + resize) ────────────────────────────────────────────
    // Applies pending drag/resize deltas to `c` and clamps position/size — call once
    // per window, BEFORE building its CLAY() tree, mirroring the existing
    // apply-delta-before-layout idiom. Does NOT arm dragging (that requires
    // Clay_Hovered() on the title bar, which only the caller's CLAY tree can do);
    // callers set c.dragging = true themselves when their title bar is pressed.
    // Resize IS armed here — a plain mouse-rect hit test (no CLAY element needed)
    // against all four edges/corners of [x,y,w,h], each edge anchored so the
    // opposite edge stays fixed (dragging the left edge doesn't move the right one).
    // Also requests an OS resize cursor (EW/NS/NWSE/NESW) via glfwSetCursor while
    // hovering or dragging an edge — applied once at the end of the frame in record().
    // Returns true while the window is being dragged or resized this frame, so
    // callers can skip other click handling.
    bool updateWindowChrome(WindowChrome& c, const UIInput& inp,
                             float minW, float minH, float maxW, float maxH);

    // ── Tooltips ──────────────────────────────────────────────────────────────
    // Declares a small floating text box near the mouse cursor if `show` is true.
    // Flips to the left/above the cursor when it would otherwise clip off the
    // right/bottom screen edge (uses inp.screenW/H — no post-layout size query
    // needed since the flip only needs to know which side has room, not the
    // tooltip's exact rendered width).
    // Call right after computing an element's current-frame hover bool, e.g.:
    //   bool n = Clay_Hovered(); ... ui.tooltip(inp, n, "Speed up time (.)");
    void tooltip(const UIInput& inp, bool show, const char* text, uint16_t fontSize = 12);

    // ── Scroll indicator ─────────────────────────────────────────────────────
    // Draws a thin vertical scrollbar thumb along the right edge of the scroll
    // container identified by `containerId` (the same CLAY_ID(...) passed to that
    // container's own CLAY() call) — a no-op if the container isn't found or its
    // content isn't taller than the visible area. Call anywhere in the same frame
    // after that container has been declared (floating elements aren't scoped to
    // their lexical position). Makes scrollable panels visibly scrollable instead
    // of relying on the user discovering they can scroll by trial and error.
    void scrollbar(Clay_ElementId containerId);

private:
    // ── Clay state ────────────────────────────────────────────────────────
    void*    clayMemory     = nullptr;
    uint32_t clayMemorySize = 0;

    // ── Font (stb_truetype) ───────────────────────────────────────────────
    struct FontAtlas {
        VkImage        image   = VK_NULL_HANDLE;
        VkDeviceMemory memory  = VK_NULL_HANDLE;
        VkImageView    view    = VK_NULL_HANDLE;
        VkSampler      sampler = VK_NULL_HANDLE;
        // Glyphs are baked once at a fixed pixel height (stbtt_BakeFontBitmap — a plain bitmap
        // atlas, not an SDF), then every requested Clay fontSize just scales that one baked
        // bitmap by a ratio (see pushText's renderScale) — there's no re-rasterization per size.
        // bakedSize was 32px, fine for normal UI text (11-19px * up to 2.0x uiScale tops out
        // around 32-38, a mild upscale) but visibly blocky on the intro's large title captions
        // (fs(48)/fs(34), which can request 68-96px — a 2-3x upscale of a 32px source). Bumped to
        // 48 with the atlas area scaled by the same (48/32)^2 factor to preserve the same packing
        // headroom stbtt_BakeFontBitmap's simple shelf-packer had at the old size (it silently
        // drops/omits glyphs that don't fit, so this ratio isn't optional). Still a raster
        // upscale, not resolution-independent — true crispness at arbitrary uiScale/title sizes
        // would need an SDF font atlas instead, a larger separate change.
        int            atlasW  = 768;
        int            atlasH  = 768;
        float          bakedSize = 48.0f;
        std::vector<uint8_t> charData;
        std::vector<uint8_t> fileData;
        void*                fontInfo = nullptr;
    } font;

    // ── Icon atlas (stb_image, RGBA8) ─────────────────────────────────────
    struct IconEntry {
        float u0, v0, u1, v1; // UV rectangle in the packed atlas
    };
    VkImage        iconImage   = VK_NULL_HANDLE;
    VkDeviceMemory iconMemory  = VK_NULL_HANDLE;
    VkImageView    iconView    = VK_NULL_HANDLE;
    VkSampler      iconSampler = VK_NULL_HANDLE;
    std::vector<IconEntry> iconEntries;

    void createIconPlaceholder(VulkanContext& ctx);
    void uploadIconAtlas(VulkanContext& ctx, const std::vector<uint8_t>& rgba, int w, int h);
    void rebindIconDescriptor(VkDevice device);

    // ── Window-edge resize cursors ──────────────────────────────────────────
    GLFWwindow* window_     = nullptr;
    GLFWcursor* cursorEW    = nullptr; // GLFW_RESIZE_EW_CURSOR
    GLFWcursor* cursorNS    = nullptr; // GLFW_RESIZE_NS_CURSOR
    GLFWcursor* cursorNWSE  = nullptr; // GLFW_RESIZE_NWSE_CURSOR
    GLFWcursor* cursorNESW  = nullptr; // GLFW_RESIZE_NESW_CURSOR
    int         pendingCursorShape = -1; // GLFW_RESIZE_*_CURSOR requested this frame, or -1 = default
    void requestCursor(int glfwResizeCursorShape) { pendingCursorShape = glfwResizeCursorShape; }
    void applyCursor(); // called once at the end of record()

    // ── GPU geometry buffers (persistently mapped host-visible) ───────────
    static constexpr uint32_t MAX_VERTS   = 65536;
    static constexpr uint32_t MAX_INDICES = MAX_VERTS * 3;

    VkBuffer       vertBuf = VK_NULL_HANDLE;
    VkDeviceMemory vertMem = VK_NULL_HANDLE;
    void*          vertMapped = nullptr;

    VkBuffer       idxBuf  = VK_NULL_HANDLE;
    VkDeviceMemory idxMem  = VK_NULL_HANDLE;
    void*          idxMapped = nullptr;

    // ── Descriptors ───────────────────────────────────────────────────────
    VkDescriptorSetLayout descLayout = VK_NULL_HANDLE;
    VkDescriptorPool      descPool   = VK_NULL_HANDLE;
    VkDescriptorSet       descSet    = VK_NULL_HANDLE;

    // ── Pipeline ──────────────────────────────────────────────────────────
    VkPipelineLayout pipeLayout = VK_NULL_HANDLE;
    VkPipeline       pipeline   = VK_NULL_HANDLE;

    // ── Per-frame CPU-side geometry ───────────────────────────────────────
    std::vector<UIVertex>  vertices;
    std::vector<uint32_t>  indices;
    uint32_t               batchVertOffset = 0; // running write offset in vertBuf (in #vertices)
    uint32_t               batchIdxOffset  = 0; // running write offset in idxBuf  (in #indices)
    float                  frameW = 800.0f, frameH = 600.0f;

    // ── Input tracking ────────────────────────────────────────────────────
    UIInput frameInput;
    bool    prevLmb = false, prevRmb = false;
    float   prevMx  = 0,     prevMy  = 0;
    bool    mouseIsOverUI = false;
    bool    prevMouseOverUI = false;
    int     tooltipSeq = 0; // unique CLAY id suffix per tooltip() call this frame; reset in beginFrame

    // ── Private helpers ───────────────────────────────────────────────────
    void loadFont(VulkanContext& ctx);
    void createPipeline(VulkanContext& ctx);
    void destroyPipeline(VkDevice device);
    void flushBatch(VkCommandBuffer cmd);

    // cornerRadius defaults to 0 (sharp corners) — safe for every existing caller
    // (text glyphs, icons, border strips) that doesn't care about rounding; only
    // the RECTANGLE render-command handler in record() passes a real value.
    void pushQuad(float x, float y, float w, float h,
                  float u0, float v0, float u1, float v1,
                  glm::vec4 color, float mode,
                  glm::vec4 cornerRadius = glm::vec4(0.0f));
    void pushText(float x, float y, const char* text, int len,
                  float fontSize, glm::vec4 color);
};
