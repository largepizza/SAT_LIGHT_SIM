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
// fpsCentreBtn_ / a popup button's slot as the harness names it.
const char *fpsPopupName(int k)
{
    static const char *n[] = {"info", "goto", "trace", "track"};
    return k >= 0 && k < 4 ? n[k] : (k == -1 ? "panel" : "");
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
                         tutActive,           // (the first-run tutorial's card has buttons: it needs the pointer like a window)
                         evw_.open};          // an event window / the explode warning (SatelliteSimEventWindows.cpp)
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

bool SatelliteSim::fpsCentrePx(float &x, float &y) const
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
        return false;
    x = (float)ww * 0.5f;
    y = (float)wh * 0.5f;
    return true;
}

std::string SatelliteSim::pickAtScreenCentre()
{
    float cx = 0.0f, cy = 0.0f;
    if (!fpsCentrePx(cx, cy))
        return "";
    return pickAndSelectAt(cx, cy, cx * 2.0f, cy * 2.0f);
}

// The selection popup (SelSatPanel, anchored to the satellite on screen) under the screen centre, in the boxes of the
// frame whose drawn bits are `drawn` (Clay keeps stale boxes: only what that frame drew counts). Only that popup:
// the toolbars, windows and every other HUD element stay inert in FPS mode.
int SatelliteSim::fpsCentreSelBtn(uint32_t drawn) const
{
    if (!fpsLogicLive() || !(drawn & kFpsSelPanelBit))
        return -2;
    float cx = 0.0f, cy = 0.0f;
    if (!fpsCentrePx(cx, cy))
        return -2;
    auto under = [&](Clay_ElementId id)
    {
        const Clay_ElementData d = Clay_GetElementData(id);
        return d.found && cx >= d.boundingBox.x && cx < d.boundingBox.x + d.boundingBox.width && cy >= d.boundingBox.y &&
               cy < d.boundingBox.y + d.boundingBox.height;
    };
    for (int k = 0; k < 4; ++k)
        if ((drawn & (1u << k)) && under(Clay_GetElementIdWithIndex(CLAY_STRING("SelActBtn"), (uint32_t)(4 + k))))
            return k;
    return under(CLAY_ID("SelSatPanel")) ? -1 : -2;
}

void SatelliteSim::fpsCentreClick()
{
    // On the selection popup: its button under the centre takes the press (buildSelActionButton), and a press between
    // its buttons does nothing, as a menu-mode click on the panel does. Elsewhere: the pick chain at the centre (a miss
    // selects nothing, as a click on empty sky does). The mode stays FPS either way (a button opening a window leaves
    // it through fpsTick's window rule).
    if (fpsCentreBtn_ != -2)
        fpsLastPopupPress_ = "panel"; // (the button's press names itself)
    else
        fpsLastPick_ = pickAtScreenCentre();
    ++fpsFacts_.clicks;
    const double t = glfwGetTime();
    if (fpsFacts_.firstClickS < 0.0)
        fpsFacts_.firstClickS = t;
    fpsFacts_.lastClickS = t;
    fpsRingFlash();
}

void SatelliteSim::fpsRingFlash()
{
    // The ring: a lone click shows it for kFpsRingShowS; a click while it still holds adds kFpsRingAddS to what is
    // left, kFpsRingMaxS at most; then the fade.
    fpsRingT_ = fpsRingT_ > 0.0f ? std::min(fpsRingT_ + kFpsRingAddS, kFpsRingMaxS) : kFpsRingShowS;
    fpsRingFade_ = 0.0f;
}

void SatelliteSim::buildFpsRing(UIRenderer &ui)
{
    (void)ui;
    // The centre dot while the selection popup is on screen in FPS mode (last frame's: the popup builds later): what
    // the centre aims at. Steady, filled, smaller than the ring, with a 1-px dark rim so it reads on a hovered (light)
    // button as on the sky; above everything, the button's tooltip included (its corner sits on the centre).
    if (fpsLive_ && (fpsSelDrawnPrev_ & kFpsSelPanelBit))
    {
        const float dd = std::max(2.0f, std::round((float)fs(kFpsRingDiamPx) * (float)kFpsDotDiamPx / (float)kFpsRingDiamPx));
        CLAY(CLAY_ID("FpsDot"), {.layout = {.sizing = {CLAY_SIZING_FIXED(dd), CLAY_SIZING_FIXED(dd)}},
                                 .backgroundColor = Pal::textBright,
                                 .cornerRadius = CLAY_CORNER_RADIUS(dd * 0.5f),
                                 .floating = {.zIndex = 101,
                                              .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER,
                                                               .parent = CLAY_ATTACH_POINT_CENTER_CENTER},
                                              .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                              .attachTo = CLAY_ATTACH_TO_ROOT},
                                 .border = {.color = Pal::panelBg, .width = CLAY_BORDER_OUTSIDE(1)}})
        {
        }
    }
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
            {"centre_popup", fpsPopupName(fpsCentreBtn_)},
            {"centre_dot", fpsLive_ && (fpsSelDrawnPrev_ & kFpsSelPanelBit) != 0},
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
        // x= y= (2026-11-18): the scripted pointer at that frame pixel (top-left origin) for the click, as a player's
        // cursor in menu mode (without it a harness run's menu-mode pointer is off the window: the click picks nothing).
        const bool at = c.has("x") || c.has("y");
        if (a.frame == 0)
        {
            a.scratch["fps_before"] = fpsMode;
            a.scratch["clicks0"] = fpsFacts_.clicks;
            a.scratch["sel0"] = selectedSatIndex;
            fpsLastPopupPress_.clear();
            if (at)
            {
                harnessPtrX_ = (float)c.num("x", 0.0);
                harnessPtrY_ = (float)c.num("y", 0.0);
                harnessPtrActive_ = true;
                harnessPtrDown_ = false;
            }
            harnessMouseBtn_ = b == "left" ? 1 : 2; // down on the next frame App reads the buttons
            return harness::Status::Pending;
        }
        if (a.frame == 1)
        {
            harnessMouseBtn_ = 0; // (App read it down for this frame; up on the next)
            return harness::Status::Pending;
        }
        if (at)
        {
            harnessPtrActive_ = false;
            r["x"] = harnessPtrX_;
            r["y"] = harnessPtrY_;
        }
        r["selected_before"] = a.scratch["sel0"];
        r["button"] = b;
        r["fps_before"] = a.scratch["fps_before"];
        r["fps_after"] = fpsMode;
        r["centre_click"] = fpsFacts_.clicks != a.scratch["clicks0"].get<int>();
        r["picked"] = fpsLastPick_;
        r["popup"] = fpsLastPopupPress_; // the selection popup took the press: its button ("goto" ...), "panel" between them
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
    if (sub == "aim")
    {
        // mouse aim <ElementId>[:index] [tol=2] [frames=120] (2026-11-17): FPS-mode mouse motion, one `mouse move`
        // frame every 3 frames (Clay's boxes are last frame's layout), until the screen centre is within tol px of the
        // element's centre: how a player turns the view onto a button of the popup anchored to the satellite.
        if (!fpsLogicLive())
        {
            a.error = "mouse aim: FPS mode is not live (fps on)";
            return harness::Status::Error;
        }
        std::string idStr = c.pos.size() > 1 ? c.pos[1] : std::string();
        int index = -1;
        const size_t colon = idStr.rfind(':');
        if (colon != std::string::npos)
        {
            index = std::atoi(idStr.c_str() + colon + 1);
            idStr = idStr.substr(0, colon);
        }
        const Clay_String cs{false, (int32_t)idStr.size(), idStr.c_str()};
        const Clay_ElementData d =
            Clay_GetElementData(index >= 0 ? Clay_GetElementIdWithIndex(cs, (uint32_t)index) : Clay_GetElementId(cs));
        float cx = 0.0f, cy = 0.0f;
        if (idStr.empty() || !d.found || !fpsCentrePx(cx, cy))
        {
            a.error = "mouse aim <ElementId>[:index]: no element '" + idStr + "' in the last frame's layout";
            return harness::Status::Error;
        }
        const float ex = d.boundingBox.x + d.boundingBox.width * 0.5f - cx;
        const float ey = d.boundingBox.y + d.boundingBox.height * 0.5f - cy;
        const double focal = (double)cy / std::tan(glm::radians((double)camera.fovYDeg) * 0.5);
        if (a.frame == 0)
        {
            a.scratch["az0"] = camera.azDeg;
            a.scratch["el0"] = camera.elDeg;
            a.scratch["ex0"] = ex;
            a.scratch["ey0"] = ey;
            a.scratch["deg0"] = glm::degrees(std::atan(std::sqrt((double)ex * ex + (double)ey * ey) / focal));
            a.scratch["px"] = 0.0;
            a.scratch["moves"] = 0;
        }
        const float tol = (float)c.num("tol", 2.0);
        const bool there = std::abs(ex) <= tol && std::abs(ey) <= tol;
        if (there || a.frame >= (int)c.num("frames", 120.0))
        {
            double dAz = (double)camera.azDeg - a.scratch["az0"].get<double>();
            dAz -= 360.0 * std::round(dAz / 360.0);
            r["element"] = c.pos[1];
            r["box"] = {d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height};
            r["start_offset_px"] = {a.scratch["ex0"], a.scratch["ey0"]};
            r["start_offset_deg"] = a.scratch["deg0"];
            r["offset_px"] = {ex, ey};
            r["moves"] = a.scratch["moves"];
            r["mouse_px"] = a.scratch["px"];
            r["d_az_deg"] = dAz;
            r["d_el_deg"] = (double)camera.elDeg - a.scratch["el0"].get<double>();
            r["centre_popup"] = fpsPopupName(fpsCentreSelBtn(fpsSelDrawn_));
            r["message"] = std::string("mouse aim ") + c.pos[1] + (there ? "" : ": did not reach it");
            if (!there)
            {
                a.error = "mouse aim: the centre is still " + std::to_string((int)ex) + ", " + std::to_string((int)ey) + " px off";
                return harness::Status::Error;
            }
            return harness::Status::Done;
        }
        if (a.frame % 3 == 0)
        {
            // Look right (+dx) moves the scene left, look down (+dy) moves it up: the offset's angle in look pixels.
            const double s = std::max(1e-6, (double)camera.lookSens());
            double dx = glm::degrees(std::atan((double)ex / focal)) / s, dy = glm::degrees(std::atan((double)ey / focal)) / s;
            if (invertMouseX)
                dx = -dx;
            if (invertMouseY)
                dy = -dy;
            harnessMouseDx_ = (float)std::clamp(dx, -500.0, 500.0);
            harnessMouseDy_ = (float)std::clamp(dy, -500.0, 500.0);
            harnessMouseFrames_ = 1;
            a.scratch["moves"] = a.scratch["moves"].get<int>() + 1;
            a.scratch["px"] = a.scratch["px"].get<double>() + std::sqrt(dx * dx + dy * dy);
        }
        return harness::Status::Pending;
    }
    a.error = "mouse move dx= dy= [frames=1] | mouse click left|right | mouse scroll dy= | mouse aim <ElementId>[:index]";
    return harness::Status::Error;
}
