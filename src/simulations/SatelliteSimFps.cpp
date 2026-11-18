// SatelliteSimFps.cpp — the FPS camera mode (2026-11-16; docs/subsystems/ui.md "FPS camera mode").
//
// Two mouse modes. Menu mode (the old behaviour): a free cursor, hold the right button and drag to look. FPS mode:
// the mouse looks with no button held, the OS cursor hidden and captured (GLFW raw motion when supported), a left
// click picks at the screen centre and flashes a ring there, and the HUD stays drawn but takes no pointer (App moves
// the UI's pointer off the window: uiPointerInert). Tab (KB_TOGGLE_CURSOR on the keyboard) toggles; a right press
// leaves FPS mode without starting a right-drag; a hotkey that opens a window or a text entry, and losing window
// focus, force menu mode. Nothing else changes the mode (fpsSet is the one writer).
//
// The look itself is buildUI's look block (SatelliteSimUI.cpp): the same camYaw / camPitch, FOV scaling, inversion,
// Track lock and capture-change frame skip as the right-drag, with fpsLive_ standing in for the held button.
#include "../Harness.h"
#include "../UIRenderer.h"
#include "SatelliteSim.h"
#include "UIPalette.h"
#include "clay.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace
{
const char *fpsWhyName(int w)
{
    static const char *k[] = {"tab", "right_click", "window", "focus", "harness", "settings", "intro"};
    return (w >= 0 && w < 7) ? k[w] : "?";
}
} // namespace

bool SatelliteSim::fpsLogicLive() const
{
    // (The opening is normal play: the mode is live from its first frame, the cursor captured for a player.)
    if (!fpsMode || cineActive())
        return false;
    // An HQ photo never captures the cursor: a player gets menu mode meanwhile (the harness's FPS mode is logic only,
    // so it carries on there). A player's own video recording (F6) is their session: the mode stays live.
    if (!harness::active() && photoState != 0)
        return false;
    return true;
}

bool SatelliteSim::uiPointerInert() const
{
    // A scripted pointer (`ui click` / `ui hover`) or the gamepad's virtual cursor is a pointer: the UI takes it.
    // A harness run never takes the REAL cursor (2026-11-16): the harness window sits on a monitor the developer's
    // mouse also rests on, and a hover there raised a tooltip that failed ui_lowres.py at the one window size
    // whose button lay under it. Only the scripted pointer reaches the UI in a harness run.
    return (fpsLogicLive() || harness::active()) && !harnessPtrActive_ && !vCursorActive;
}

void SatelliteSim::mouseButtonOverride(bool &lmb, bool &rmb) const
{
    if (harnessMouseBtn_ == 1)
        lmb = true;
    else if (harnessMouseBtn_ == 2)
        rmb = true;
}

uint32_t SatelliteSim::fpsWindowMask() const
{
    const bool open[] = {settingsChrome.open, viewControlsChrome.open, infoChrome.open, viewerChrome.open,
                         traceChrome.open,    msChrome.open,           skChrome.open,   wxChrome.open,
                         cineChrome.open,     bmChrome.open,           rpChrome.open,   reportBarOpen_,
                         tutActive}; // (the first-run tutorial's card has buttons: it needs the pointer like a window)
    uint32_t m = 0;
    for (size_t i = 0; i < sizeof(open) / sizeof(open[0]); ++i)
        if (open[i])
            m |= 1u << i;
    return m;
}

void SatelliteSim::fpsSet(bool on, FpsWhy why)
{
    if (on == fpsMode)
        return;
    fpsMode = on;
    fpsLastWhy_ = why;
    hintsNoteFps(on, why); // the hints' gates: left by Tab / a right click, back to FPS mode (SatelliteSimHints.cpp)
    if (on)
    {
        if (why == FpsWhy::Tab)
            fpsFacts_.enteredFromMenu = true;
        // Entered with the right button held (a right-drag going on): the FPS look takes over the capture, and the
        // button's release must not matter; a later exit while it is still held must not start a drag.
        camera.captured = false;
        camera.blockUntilRelease = true;
    }
    else
    {
        if (why == FpsWhy::Tab)
            fpsFacts_.leftByTab = true;
        if (why == FpsWhy::RightClick)
        {
            fpsFacts_.leftByRightClick = true;
            camera.blockUntilRelease = true; // the press that left must not start a right-drag look
        }
        cinematicMode = false; // (the Alt pan lives only while looking, as with a released right button)
    }
    // The OS cursor follows at once (a forced exit frees it before the window that caused it is drawn).
    fpsApplyCapture();
}

// The OS cursor: captured while the mode is live, the window has the focus and no harness runs; freed (at the window
// centre) otherwise. A right-drag in progress keeps its own capture.
void SatelliteSim::fpsApplyCapture()
{
    if (!win || harness::active())
        return;
    const bool want = fpsLogicLive() && glfwGetWindowAttrib(win, GLFW_FOCUSED) != 0;
    if (want == fpsOsCaptured_)
        return;
    if (want)
    {
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_DISABLED);
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(win, GLFW_RAW_MOUSE_MOTION, GLFW_TRUE);
    }
    else
    {
        if (glfwRawMouseMotionSupported())
            glfwSetInputMode(win, GLFW_RAW_MOUSE_MOTION, GLFW_FALSE);
        if (!camera.captured)
        {
            glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
            int ww = 0, wh = 0;
            glfwGetWindowSize(win, &ww, &wh);
            glfwSetCursorPos(win, ww * 0.5, wh * 0.5); // back at the centre, where the view was aimed
        }
    }
    fpsOsCaptured_ = want;
}

void SatelliteSim::fpsTick(const UIInput &inp, float dt)
{
    // Losing window focus ends the FPS mode (a harness window never has the focus: logic only there).
    if (win && !harness::active())
    {
        const bool focused = glfwGetWindowAttrib(win, GLFW_FOCUSED) != 0;
        if (fpsFocusedPrev_ && !focused && fpsMode)
            fpsSet(false, FpsWhy::Focus);
        fpsFocusedPrev_ = focused;
    }
    // A text field, the console or a key rebind taking the keyboard is a text entry: menu mode.
    bool kb = textEditing() || consoleOpen_;
    for (const auto &k : keybindings)
        kb = kb || k.listening || k.listeningPad;
    if (kb && !fpsKbCapturedPrev_ && fpsMode)
        fpsSet(false, FpsWhy::Window);
    fpsKbCapturedPrev_ = kb;
    // A window, the note bar or the tutorial card appearing (whoever opened it: a hotkey, a report shot, the first-run
    // tutorial) wants the pointer: menu mode. Only the appearance counts: Tab back into FPS mode with it open works.
    const uint32_t mask = fpsWindowMask();
    if (fpsMode && (mask & ~fpsMaskPrev_) != 0)
        fpsSet(false, FpsWhy::Window);
    fpsMaskPrev_ = mask;
    // A right press (a real one, or `mouse click right`) leaves the FPS mode.
    if (fpsLogicLive() && inp.rmbPressed && !vCursorActive)
        fpsSet(false, FpsWhy::RightClick);

    const bool live = fpsLogicLive();
    fpsApplyCapture();
    fpsLive_ = live;

    // The centre ring's clock (real frame time: it is feedback for the hand, not part of the scene).
    if (fpsRingT_ > 0.0f)
    {
        fpsRingT_ -= dt;
        if (fpsRingT_ <= 0.0f)
        {
            fpsRingT_ = 0.0f;
            fpsRingFade_ = kFpsRingFadeS;
        }
    }
    else if (fpsRingFade_ > 0.0f)
        fpsRingFade_ = std::max(0.0f, fpsRingFade_ - dt);
}

std::string SatelliteSim::pickAtScreenCentre()
{
    // The mouse's coordinates (window size, glfwGetWindowSize: what UIInput and the virtual cursor use), not the
    // framebuffer's: under display scaling the two differ, and the pick's pixel tolerances are in the mouse's.
    int ww = 0, wh = 0;
    if (win)
        glfwGetWindowSize(win, &ww, &wh);
    if ((ww <= 0 || wh <= 0) && ctx_)
    {
        ww = (int)ctx_->swapExtent.width;
        wh = (int)ctx_->swapExtent.height;
    }
    if (ww <= 0 || wh <= 0)
        return "";
    return pickAndSelectAt((float)ww * 0.5f, (float)wh * 0.5f, (float)ww, (float)wh);
}

void SatelliteSim::fpsCentreClick()
{
    // The pick chain at the centre (a miss selects nothing, as a click on empty sky does); the mode stays FPS.
    fpsLastPick_ = pickAtScreenCentre();
    ++fpsFacts_.clicks;
    const double t = glfwGetTime();
    if (fpsFacts_.firstClickS < 0.0)
        fpsFacts_.firstClickS = t;
    fpsFacts_.lastClickS = t;
    // The ring: a lone click shows it for kFpsRingShowS; a click while it still holds adds kFpsRingAddS to what is
    // left, kFpsRingMaxS at most; then the fade.
    fpsRingT_ = fpsRingT_ > 0.0f ? std::min(fpsRingT_ + kFpsRingAddS, kFpsRingMaxS) : kFpsRingShowS;
    fpsRingFade_ = 0.0f;
}

void SatelliteSim::buildFpsRing(UIRenderer &ui)
{
    (void)ui;
    const float a = fpsRingT_ > 0.0f ? 1.0f : (fpsRingFade_ > 0.0f ? fpsRingFade_ / kFpsRingFadeS : 0.0f);
    if (a <= 0.0f)
        return;
    const float d = (float)fs(kFpsRingDiamPx);
    const uint16_t w = (uint16_t)std::max(1L, std::lround(d * kFpsRingStroke));
    Clay_Color col = Pal::textBright;
    col.a *= a;
    CLAY(CLAY_ID("FpsRing"), {.layout = {.sizing = {CLAY_SIZING_FIXED(d), CLAY_SIZING_FIXED(d)}},
                              .cornerRadius = CLAY_CORNER_RADIUS(d * 0.5f),
                              .floating = {.zIndex = 25,
                                           .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER,
                                                            .parent = CLAY_ATTACH_POINT_CENTER_CENTER},
                                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                           .attachTo = CLAY_ATTACH_TO_ROOT},
                              .border = {.color = col, .width = CLAY_BORDER_OUTSIDE(w)}})
    {
    }
}

nlohmann::json SatelliteSim::fpsStateJson() const
{
    const FpsFacts &f = fpsFacts_;
    const float ringA = fpsRingT_ > 0.0f ? 1.0f : (fpsRingFade_ > 0.0f ? fpsRingFade_ / kFpsRingFadeS : 0.0f);
    return {{"mode", fpsMode ? "fps" : "menu"},
            {"fps_mode", fpsMode},
            {"live", fpsLogicLive()},
            {"os_captured", fpsOsCaptured_},
            {"ui_pointer_inert", uiPointerInert()},
            {"last_change", fpsWhyName((int)fpsLastWhy_)},
            {"last_pick", fpsLastPick_},
            {"selected_sat", selectedSatIndex},
            {"ring", {{"hold_s", fpsRingT_}, {"fade_s", fpsRingFade_}, {"alpha", ringA},
                      {"show_s", kFpsRingShowS}, {"add_s", kFpsRingAddS}, {"max_s", kFpsRingMaxS}, {"fade_total_s", kFpsRingFadeS}}},
            {"mouse_sensitivity", mouseSensMul},
            {"look_deg_per_px", camera.lookSens()},
            {"right_drag_held", camera.captured},
            {"camera", {{"az_deg", camera.azDeg}, {"el_deg", camera.elDeg}, {"fov_y_deg", camera.fovYDeg}}},
            {"facts", {{"left_by_tab", f.leftByTab}, {"left_by_right_click", f.leftByRightClick},
                       {"entered_from_menu", f.enteredFromMenu}, {"clicks", f.clicks},
                       {"first_click_s", f.firstClickS}, {"last_click_s", f.lastClickS},
                       {"look_deg", f.lookDeg}, {"right_drag_used", f.rightDragUsed}}}};
}

// ── Harness: fps on | off | state ─────────────────────────────────────────────────────────────────────────────
harness::Status SatelliteSim::harnessFps(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    const std::string sub = c.pos.empty() ? std::string("state") : c.pos[0];
    if (sub == "on")
        fpsSet(true, FpsWhy::Harness);
    else if (sub == "off")
        fpsSet(false, FpsWhy::Harness);
    else if (sub != "state")
    {
        a.error = "fps on | off | state";
        return harness::Status::Error;
    }
    a.result["fps"] = fpsStateJson();
    a.result["message"] = std::string("fps ") + (fpsMode ? "on" : "off") + (fpsLogicLive() ? "" : " (not live)");
    return harness::Status::Done;
}

// ── Harness: mouse move dx= dy= [frames=1] | mouse click left|right | mouse scroll dy= ──────────────────────────
// move: dx / dy pixels a frame for `frames` frames, consumed by the FPS look (menu mode: by nothing, as a free
// cursor's motion). click: the button down for one frame and up the next, through App's button state, so the
// press reaches the same code a real one does in the current mode (FPS: the centre pick / the exit; menu: the UI
// and the scene pick at the pointer).
harness::Status SatelliteSim::harnessMouse(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    nlohmann::json &r = a.result;
    const std::string sub = c.pos.empty() ? std::string() : c.pos[0];
    if (sub == "move")
    {
        if (a.frame == 0)
        {
            harnessMouseDx_ = (float)c.num("dx", 0.0);
            harnessMouseDy_ = (float)c.num("dy", 0.0);
            harnessMouseFrames_ = std::max(1, (int)c.num("frames", 1.0));
            a.scratch["az0"] = camera.azDeg;
            a.scratch["el0"] = camera.elDeg;
            a.scratch["look0"] = fpsFacts_.lookDeg;
            a.scratch["fps"] = fpsLogicLive();
            return harness::Status::Pending;
        }
        if (harnessMouseFrames_ > 0)
            return harness::Status::Pending;
        double dAz = (double)camera.azDeg - a.scratch["az0"].get<double>();
        dAz -= 360.0 * std::round(dAz / 360.0);
        r["fps_live"] = a.scratch["fps"];
        r["d_az_deg"] = dAz;
        r["d_el_deg"] = (double)camera.elDeg - a.scratch["el0"].get<double>();
        r["d_look_deg"] = fpsFacts_.lookDeg - a.scratch["look0"].get<double>();
        r["look_deg_per_px"] = camera.lookSens();
        r["camera"] = {{"az_deg", camera.azDeg}, {"el_deg", camera.elDeg}};
        r["message"] = "mouse move: d_az " + std::to_string(dAz).substr(0, 7) + " deg";
        return harness::Status::Done;
    }
    if (sub == "click")
    {
        const std::string b = c.pos.size() > 1 ? c.pos[1] : std::string("left");
        if (b != "left" && b != "right")
        {
            a.error = "mouse click left|right";
            return harness::Status::Error;
        }
        if (a.frame == 0)
        {
            a.scratch["fps_before"] = fpsMode;
            a.scratch["clicks0"] = fpsFacts_.clicks;
            harnessMouseBtn_ = b == "left" ? 1 : 2; // down on the next frame App reads the buttons
            return harness::Status::Pending;
        }
        if (a.frame == 1)
        {
            harnessMouseBtn_ = 0; // (App read it down for this frame; up on the next)
            return harness::Status::Pending;
        }
        r["button"] = b;
        r["fps_before"] = a.scratch["fps_before"];
        r["fps_after"] = fpsMode;
        r["centre_click"] = fpsFacts_.clicks != a.scratch["clicks0"].get<int>();
        r["picked"] = fpsLastPick_;
        r["selected_sat"] = selectedSatIndex;
        r["ring_hold_s"] = fpsRingT_;
        r["right_drag_held"] = camera.captured;
        r["message"] = "mouse click " + b + (fpsMode ? " (fps)" : " (menu)");
        return harness::Status::Done;
    }
    if (sub == "scroll")
    {
        // Wheel notches (+ = away from you: zoom in) for the next frame's FOV zoom, through buildUI's wheel path (the
        // pointer over the UI eats it, as a real wheel; FPS mode's pointer is never over it).
        if (a.frame == 0)
        {
            a.scratch["fov0"] = camera.fovYDeg;
            harnessScrollY_ = (float)c.num("dy", 1.0);
            return harness::Status::Pending;
        }
        r["fov_before_deg"] = a.scratch["fov0"];
        r["fov_deg"] = camera.fovYDeg;
        r["message"] = "mouse scroll: fov " + std::to_string(camera.fovYDeg).substr(0, 6) + " deg";
        return harness::Status::Done;
    }
    a.error = "mouse move dx= dy= [frames=1] | mouse click left|right | mouse scroll dy=";
    return harness::Status::Error;
}
