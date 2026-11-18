// SatelliteSimHints.cpp — the first run's hints (2026-11-17; docs/subsystems/ui.md "Hints").
//
// One small hint at a time (HintPopup: keycaps, then one line), chosen from what the player has and has not done:
//   1 look   2 move   3 rise / descend   4 go faster   5 zoom   6 the cursor (leave FPS mode)   7 click a satellite
//   9 the time bar   10 the menu buttons   8 back to looking around (FPS mode again)
// Gates (HintGates) are sticky facts since the pass began; hintsTick measures them, keeps the live signals (the mode,
// movement now, idle time, recent FPS clicks, the captions) and runs the scheduler: one hint on screen, faded in and
// out, at least kHintGapS between two, gone kHintLingerS after its gate is met (at once when its condition stops
// holding), a gate met before its hint showed retires it, an ignored hint rests; the order of the brief's table.
// buildHints draws it: above the opening's caption band while one shows, else bottom centre clear of the toolbars;
// 9 and 10 above the toolbar they describe, with an outline around it. Every clock is frame time (the caption
// clock's), so `--fixed-dt` runs are exact. Replaces the first-run card tutorial's teaching (that card no longer
// starts on its own; SatelliteSimTutorial.cpp).
#include "../Harness.h"
#include "../UIRenderer.h"
#include "../UiText.h"
#include "SatelliteSim.h"
#include "UIPalette.h"
#include "clay.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
Clay_String hintStr(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }

struct HBox
{
    float x, y, w, h;
};
bool hOverlap(const HBox &a, const HBox &b)
{
    const float ox = std::min(a.x + a.w, b.x + b.w) - std::max(a.x, b.x);
    const float oy = std::min(a.y + a.h, b.y + b.h) - std::max(a.y, b.y);
    return ox > 1.0f && oy > 1.0f;
}
bool hDrawn(Clay_ElementId id, HBox &b)
{
    Clay_BoundingBox bb{};
    if (!UIRenderer::declaredThisFrame(id, &bb) || bb.width <= 0.0f || bb.height <= 0.0f)
        return false;
    b = {bb.x, bb.y, bb.width, bb.height};
    return true;
}
const char *const kPlaceNames[] = {"bottom", "caption", "left_panel", "right_panel"};
} // namespace

// ─── State ────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::hintsReset()
{
    hint_ = HintRun{};
    hintsPass_ = 1;
    hintMoveIn_ = hintBoostIn_ = hintZoomIn_ = hintInputIn_ = false;
    hintClimbM_ = 0.0f;
    hintsPrime(); // (now: an edge in the same frame, a harness command after `hints on`, is not swallowed)
}

// Last frame's values for the edges (the selection, the clock's controls, the windows, the pad's mode, the view).
void SatelliteSim::hintsPrime()
{
    HintRun &h = hint_;
    h.primed = true;
    h.fpsClicksPrev = fpsFacts_.clicks;
    h.lookFactsPrev = fpsFacts_.lookDeg;
    h.selPrev = selectedSatIndex;
    h.timeIdxPrev = timeScaleIdx;
    h.pausedPrev = timePaused;
    h.reversePrev = timeDir < 0.0f;
    h.padMenuPrev = vCursorToggled && uiVisible;
    h.winPrev[0] = msChrome.open;
    h.winPrev[1] = wxChrome.open;
    h.winPrev[2] = skChrome.open;
    h.winPrev[3] = settingsChrome.open;
    h.azPrev = camera.azDeg;
    h.elPrev = camera.elDeg;
}

void SatelliteSim::hintsNoteFps(bool on, FpsWhy why)
{
    HintGates &g = hint_.g;
    if (why == FpsWhy::Tab || why == FpsWhy::RightClick)
        hintInputIn_ = true;
    if (!on)
    {
        if (why == FpsWhy::Tab)
            g.leftByTab = true;
        else if (why == FpsWhy::RightClick)
            g.leftByRightClick = true;
    }
    else if (why != FpsWhy::Intro && why != FpsWhy::Harness)
        g.returned = true; // (Tab, or the Settings toggle: the player chose to look around again)
}

bool SatelliteSim::hintMenuMode() const
{
    // A pad player's menu mode is the virtual cursor (Start); the stick looks in either keyboard mode.
    return lastInputWasGamepad ? (vCursorToggled && uiVisible) : !fpsMode;
}

bool SatelliteSim::hintSatAtCentre() const
{
    // The drawn mesh satellites (a short list, the pick's own first test): one within its size or kHintCentrePx.
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
    for (const MeshDrawn &m : meshDrawn)
    {
        float sx = 0.0f, sy = 0.0f;
        if (!projectSkyDirToScreen(m.enuDir, (float)ww, (float)wh, sx, sy))
            continue;
        if (std::hypot(sx - ww * 0.5f, sy - wh * 0.5f) <= std::max(m.radiusPx, kHintCentrePx))
            return true;
    }
    return false;
}

// ─── Content ──────────────────────────────────────────────────────────────────────────────────────
// Keycaps (labels from the live bindings; W A S D are literal, as in the movement code) and text pieces.
int SatelliteSim::hintParts(int n, bool pad, bool menu, HintPart *o, char (*keys)[24]) const
{
    int k = 0, nk = 0;
    auto text = [&](const char *s) { o[k++] = {s, false}; };
    auto lit = [&](const char *s) { o[k++] = {s, true}; };
    auto key = [&](int kb, bool gp)
    {
        snprintf(keys[nk], 24, "%s", actionLabel(kb, gp).s);
        o[k++] = {keys[nk], true};
        nk = std::min(nk + 1, 3);
    };
    switch (n)
    {
    case 1:
        text(pad ? tx(Txt::HintLookPad) : menu ? tx(Txt::HintLookDrag) : tx(Txt::HintLookMouse));
        break;
    case 2:
        if (pad)
            text(tx(Txt::HintMovePad));
        else
        {
            lit("W");
            lit("A");
            lit("S");
            lit("D");
            text(tx(Txt::HintToMove));
        }
        break;
    case 3:
        if (pad) // (the triggers: analog, no binding of their own; gpElevRaise / gpElevLower)
        {
            lit("RT");
            text("/");
            lit("LT");
        }
        else
        {
            key(KB_RAISE_ELEV, false);
            text("/");
            key(KB_LOWER_ELEV, false);
        }
        text(tx(Txt::HintRiseDescend));
        break;
    case 4:
        text(tx(Txt::HintHold));
        key(KB_MOVE_BOOST, pad);
        text(tx(Txt::HintGoFaster));
        break;
    case 5:
        if (pad)
        {
            key(KB_ZOOM_OUT, true);
            text("/");
            key(KB_ZOOM_IN, true);
            text(tx(Txt::HintToZoom));
        }
        else
            text(tx(Txt::HintScrollZoom));
        break;
    case 6:
        key(KB_TOGGLE_CURSOR, pad);
        text(pad ? tx(Txt::HintCursorPad) : tx(Txt::HintCursorKeys));
        break;
    case 7:
        text(tx(Txt::HintClickSat));
        break;
    case 8:
        key(KB_TOGGLE_CURSOR, pad);
        text(tx(Txt::HintLookAgain));
        break;
    case 9:
        text(tx(Txt::HintTimeBar));
        break;
    case 10:
        text(tx(Txt::HintMenuBar));
        break;
    default:
        break;
    }
    return k;
}

std::string SatelliteSim::hintText(int n, bool pad, bool menu) const
{
    HintPart p[8];
    char keys[4][24];
    const int np = hintParts(n, pad, menu, p, keys);
    std::string s;
    for (int i = 0; i < np; ++i)
    {
        if (!s.empty())
            s += ' ';
        s += p[i].key ? "[" + std::string(p[i].s) + "]" : std::string(p[i].s);
    }
    return s;
}

namespace
{
// The text id of a hint's main line (harness `hints state`).
const char *hintTextId(int n, bool pad, bool menu)
{
    switch (n)
    {
    case 1: return pad ? "HintLookPad" : menu ? "HintLookDrag" : "HintLookMouse";
    case 2: return pad ? "HintMovePad" : "HintToMove";
    case 3: return "HintRiseDescend";
    case 4: return "HintGoFaster";
    case 5: return pad ? "HintToZoom" : "HintScrollZoom";
    case 6: return pad ? "HintCursorPad" : "HintCursorKeys";
    case 7: return "HintClickSat";
    case 8: return "HintLookAgain";
    case 9: return "HintTimeBar";
    case 10: return "HintMenuBar";
    default: return "";
    }
}
} // namespace

// ─── The scheduler ────────────────────────────────────────────────────────────────────────────────
// buildUI, every frame after the look block (this frame's turn is in fpsFacts_.lookDeg) and before the click handling;
// the movement signals are the last recordCompute's.
void SatelliteSim::hintsTick(const UIInput &inp, float dt)
{
    if (hintsPass_ < 0)
        hintsPass_ = tutorialDone ? 0 : 1; // the first frame: a genuine first run, or not
    HintRun &h = hint_;
    HintGates &g = h.g;
    if (hintsPass_ <= 0 || (harness::active() && !hintsHarnessOn_))
    {
        h.cur = 0;
        h.alpha = 0.0f;
        h.forced = false;
        hintInputIn_ = hintZoomIn_ = false;
        hintClimbM_ = 0.0f;
        return;
    }
    dt = std::clamp(dt, 0.0f, 0.25f);
    h.clock += dt;
    const bool padMenu = vCursorToggled && uiVisible;
    const bool menu = hintMenuMode();
    const bool captionsEnded = !showIntro; // (no opening at all counts as ended)
    if (!h.primed)
        hintsPrime();

    // ── Gates and live signals ──
    bool input = hintInputIn_ || inp.lmbPressed || inp.rmbPressed || inp.scrollY != 0.0f;
    hintInputIn_ = false;
    // The view turned: the mouse in either mode (buildUI's look block counts it) and the pad's right stick.
    double dLook = std::max(0.0, fpsFacts_.lookDeg - h.lookFactsPrev);
    h.lookFactsPrev = fpsFacts_.lookDeg;
    dLook += ((double)std::fabs(gpLookYawDeg) + (double)std::fabs(gpLookPitchDeg)) * (double)camera.fovLookScale();
    if (dLook > 1e-3)
    {
        input = true;
        h.lookDeg += dLook;
        if (camera.captured && !fpsLive_)
            g.dragLooked = true;
    }
    if (h.lookDeg >= kHintLookDeg)
        g.looked = true;
    // Movement: the merged input of the movement block (W A S D, the left stick, the harness's `inertial move`).
    if (hintMoveIn_)
    {
        h.moveS += dt;
        h.movingS += dt;
        h.sinceMoveS = 0.0f;
        input = true;
    }
    else
    {
        h.movingS = 0.0f;
        h.sinceMoveS += dt;
    }
    if (h.moveS >= kHintMoveS)
        g.moved = true;
    if (hintClimbM_ > 0.0f)
    {
        h.climbM += hintClimbM_;
        input = true;
    }
    hintClimbM_ = 0.0f;
    if (h.climbM >= kHintClimbM)
        g.climbed = true;
    if (hintBoostIn_)
        g.boosted = true;
    if (hintZoomIn_)
    {
        g.zoomed = true;
        input = true;
    }
    hintZoomIn_ = false;
    // FPS-mode clicks (fpsCentreClick counts them)
    if (fpsFacts_.clicks < h.fpsClicksPrev)
        h.fpsClicksPrev = fpsFacts_.clicks;
    const bool newClick = fpsFacts_.clicks > h.fpsClicksPrev;
    for (int c = h.fpsClicksPrev; c < fpsFacts_.clicks; ++c)
    {
        h.clickAt[h.clickN++ % 8] = h.clock;
        g.clicked = true;
        input = true;
    }
    h.fpsClicksPrev = fpsFacts_.clicks;
    int clicks5 = 0;
    for (int i = 0; i < std::min(h.clickN, 8); ++i)
        if (h.clock - h.clickAt[i] <= kHintClickWinS)
            ++clicks5;
    // The pad's mode: Start toggles the virtual cursor (the keyboard's edges come from fpsSet: hintsNoteFps).
    if (padMenu != h.padMenuPrev)
    {
        input = true;
        if (padMenu)
            g.leftByTab = true;
        else
            g.returned = true;
        h.padMenuPrev = padMenu;
    }
    if (menu)
        h.everMenu = true;
    // A satellite selected (a change to one: a selection standing when the pass began does not count).
    if (selectedSatIndex >= 0 && selectedSatIndex != h.selPrev)
        g.selected = true;
    h.selPrev = selectedSatIndex;
    // The time bar used (9) and the menu windows opened (10), by their buttons or their keys.
    const bool timeUsed = timeScaleIdx != h.timeIdxPrev || timePaused != h.pausedPrev || (timeDir < 0.0f) != h.reversePrev;
    h.timeIdxPrev = timeScaleIdx;
    h.pausedPrev = timePaused;
    h.reversePrev = timeDir < 0.0f;
    const bool winNow[4] = {msChrome.open, wxChrome.open, skChrome.open, settingsChrome.open};
    bool winUsed = false;
    for (int i = 0; i < 4; ++i)
    {
        winUsed = winUsed || (winNow[i] && !h.winPrev[i]);
        h.winPrev[i] = winNow[i];
    }
    if (timeUsed)
        g.timeBarShown = true;
    if (winUsed)
        g.menuBarShown = true;
    input = input || timeUsed || winUsed;
    h.idleS = input ? 0.0f : h.idleS + dt;
    h.menuIdleS = (menu && !input) ? h.menuIdleS + dt : 0.0f;
    {
        float dAz = camera.azDeg - h.azPrev;
        dAz -= 360.0f * std::round(dAz / 360.0f);
        const float dEl = camera.elDeg - h.elPrev;
        h.azPrev = camera.azDeg;
        h.elPrev = camera.elDeg;
        if (std::fabs(dAz) + std::fabs(dEl) <= kHintStillDegS * std::max(dt, 1e-3f))
            h.stillS += dt;
        else
            h.stillS = 0.0f;
    }

    auto gate = [&](int n) -> bool
    {
        switch (n)
        {
        case 1: return g.looked;
        case 2: return g.moved;
        case 3: return g.climbed;
        case 4: return g.boosted;
        case 5: return g.zoomed;
        case 6: return g.leftByTab || g.leftByRightClick;
        case 7: return g.selected;
        case 8: return g.returned;
        case 9: return g.timeBarShown;
        case 10: return g.menuBarShown;
        default: return false;
        }
    };
    for (int n = 1; n <= kHintCount; ++n) // a gate met before its hint ever showed retires it (silently)
        if (h.offers[n] == 0 && h.cur != n && gate(n))
            h.retired[n] = true;

    // ── The first run is complete ──
    if (hintsPass_ == 1 && captionsEnded && g.selected && g.moved && g.timeBarShown && g.menuBarShown)
    {
        hintsPass_ = 2;
        tutorialDone = true; // display.tutorial_done: no hints (and no card) on later runs
        tutAutoChecked = true;
        saveSettings();
    }

    // ── Conditions ──
    const bool suppressed = cineActive() || photoState != 0 || tutActive || !uiVisible;
    const bool beforeBeat = showIntro && introElapsed < kIntroKeyframes[kHintBeat].t;
    const bool fpsH = !menu;
    int satRefMemo = -1; // a satellite to zoom on: one selected, or a drawn one at the centre (asked only when needed)
    auto satRef = [&]() -> bool
    {
        if (satRefMemo < 0)
            satRefMemo = (selectedSatIndex >= 0 || hintSatAtCentre()) ? 1 : 0;
        return satRefMemo == 1;
    };
    // The never-time-out cases (5a and 5c): they ignore the 20-s timeout, the rest and the offer cap.
    auto neverTO = [&](int n) -> bool
    {
        if (n == 6)
            return captionsEnded && !h.everMenu;
        if (n == 7)
            return captionsEnded && menu && !g.selected;
        if (n == 2)
            return g.selected && g.timeBarShown && g.menuBarShown && !g.moved;
        return false;
    };
    // Offered when (besides its gate unmet).
    auto offerCond = [&](int n) -> bool
    {
        switch (n)
        {
        case 1: return true;
        case 2: return g.looked || neverTO(2);
        case 3: return hintMoveIn_ || g.moved;
        case 4: return h.movingS >= kHintStreakS;
        case 5: return h.stillS >= kHintStillS && satRef();
        case 6: return fpsH && !h.everMenu && (h.idleS >= kHintIdleS || clicks5 >= 2 || captionsEnded);
        case 7: return menu;
        case 8: return menu && ((g.timeBarShown && g.menuBarShown) || h.menuIdleS >= kHintMenuIdleS);
        case 9: return menu && g.selected;
        case 10: return menu && g.timeBarShown;
        default: return false;
        }
    };
    // Holds while it shows (else it fades at once). 4 and 5 hold on less than they need to be offered: the streak
    // and the still view are what call for them, not what they are about.
    auto holds = [&](int n) -> bool
    {
        switch (n)
        {
        case 3: return hintMoveIn_ || g.moved;
        case 4: return h.sinceMoveS < kHintMoveGraceS;
        case 5: return satRef();
        case 6: return fpsH && !h.everMenu;
        case 7:
        case 8:
        case 10: return menu;
        case 9: return menu && g.selected;
        default: return true;
        }
    };
    auto allowed = [&](int n) -> bool
    {
        if (gate(n))
            return false;
        if (hintsPass_ == 2) // complete: 3, 4, 5 and 8 once more each this session
            return (n == 3 || n == 4 || n == 5 || n == 8) && !h.postFired[n];
        return true;
    };
    auto offerable = [&](int n, bool nto) -> bool
    {
        if (!allowed(n))
            return false;
        if (nto || n == 9 || n == 10)
            return true;
        return h.offers[n] < kHintMaxOffers && h.clock >= h.restUntil[n];
    };
    auto ok = [&](int n) { return offerable(n, false) && offerCond(n); };
    // The order (the brief's 5a-h): the first match shows.
    auto pick = [&]() -> int
    {
        if (captionsEnded) // a
        {
            if (!h.everMenu)
            {
                if (offerable(6, true) && offerCond(6))
                    return 6;
            }
            else if (menu && !g.selected && offerable(7, true))
                return 7;
        }
        if (menu && g.selected) // b
        {
            if (ok(9))
                return 9;
            if (ok(10))
                return 10;
        }
        if (g.selected && g.timeBarShown && g.menuBarShown) // c
        {
            if (menu && ok(8))
                return 8;
            if (!g.moved && offerable(2, true))
                return 2;
        }
        if (!g.looked && ok(1)) // d
            return 1;
        if (hintMoveIn_) // e
        {
            if (!g.climbed && ok(3))
                return 3;
            if (ok(4))
                return 4;
        }
        if ((h.idleS >= kHintIdleS || clicks5 >= 2) && ok(6)) // f
            return 6;
        if (menu && !g.selected && ok(7)) // g
            return 7;
        for (int n : {2, 3, 5}) // h
            if (ok(n))
                return n;
        if (menu && h.menuIdleS >= kHintMenuIdleS && ok(8))
            return 8;
        return 0;
    };

    // ── The slot ──
    auto leave = [&](const char *how)
    {
        if (!h.leaving)
        {
            h.leaving = true;
            h.leaveHow = how;
        }
    };
    if (h.cur && h.forced)
    {
        h.curS += dt;
        h.alpha = 1.0f;
    }
    else if (h.cur)
    {
        const int n = h.cur;
        h.curS += dt;
        h.upS[n] += dt;
        if (!h.leaving)
        {
            if (suppressed)
                leave("suppressed");
            else if (gate(n))
            {
                if (h.gateS < 0.0f && !h.log.empty() && h.log.back().n == n)
                    h.log.back().gateAtS = h.clock; // (the log: when it was met)
                h.gateS = h.gateS < 0.0f ? 0.0f : h.gateS + dt;
                if (h.gateS >= kHintLingerS)
                    leave("gate");
            }
            else if (!holds(n))
                leave("condition");
            else if ((n == 9 || n == 10) && h.upS[n] >= kHintMenuShowS)
            {
                (n == 9 ? g.timeBarShown : g.menuBarShown) = true;
                leave("shown");
            }
            else if (n != 9 && n != 10 && !neverTO(n) && h.curS >= kHintIgnoredS)
            {
                leave("timeout");
                h.restUntil[n] = h.clock + kHintFadeS + kHintRestS;
            }
            else if (newClick && n != 6 && !neverTO(n) && clicks5 >= 2 && ok(6))
            {
                // Two quick clicks in FPS mode answer themselves: hint 6 takes the slot at once (the one it replaces
                // gets its offer back).
                leave("preempted");
                h.offers[n] = std::max(0, h.offers[n] - 1);
                h.preempt = 6;
            }
        }
        h.alpha = h.leaving ? std::max(0.0f, h.alpha - dt / kHintFadeS) : std::min(1.0f, h.alpha + dt / kHintFadeS);
        if (h.leaving && h.alpha <= 0.0f)
        {
            for (auto it = h.log.rbegin(); it != h.log.rend(); ++it)
                if (it->n == n && it->goneS < 0.0f)
                {
                    it->goneS = h.clock;
                    it->how = h.leaveHow;
                    break;
                }
            h.cur = 0;
            h.leaving = false;
            h.freeAt = h.clock;
        }
    }
    if (!h.cur && !suppressed && !beforeBeat)
    {
        int n = 0;
        if (h.preempt && ok(h.preempt))
            n = h.preempt;
        else if (h.clock - h.freeAt >= kHintGapS)
            n = pick();
        h.preempt = 0;
        if (n)
        {
            h.cur = n;
            h.curS = 0.0f;
            h.alpha = 0.0f;
            h.leaving = false;
            h.gateS = -1.0f;
            h.forced = false;
            ++h.offers[n];
            if (hintsPass_ == 2)
                h.postFired[n] = true;
            if (!h.seen.empty())
                h.seen += ',';
            h.seen += std::to_string(n);
            h.log.push_back({n, h.clock, -1.0f, -1.0f, ""});
        }
    }
}

// ─── The popup ────────────────────────────────────────────────────────────────────────────────────
// After every HUD element it keeps clear of (their boxes are the last layout's; only those declared this frame count).
void SatelliteSim::buildHints(const UIInput &inp, UIRenderer &ui)
{
    (void)ui;
    const bool drawnPrev = hintDrawnPrev_;
    const int prevN = hintDrawnPrevN_;
    hintDrawnPrev_ = false;
    HintRun &h = hint_;
    if (h.cur == 0)
        return;
    const float W = (float)inp.screenW, H = (float)inp.screenH;
    const bool pad = lastInputWasGamepad, menu = hintMenuMode();
    HintPart parts[8];
    const int np = hintParts(h.cur, pad, menu, parts, hintKeyBuf_);
    const float a = std::clamp(h.alpha, 0.0f, 1.0f);
    const uint16_t fsz = fs(13);
    const float kh = (float)fsz * 1.6f; // a keycap's height
    const float maxW = std::max(60.0f, W - 24.0f);
    const float gap = 8.0f, margin = 12.0f;

    // Its size: last frame's box when it showed this hint, else an estimate (it fades in from 0 then).
    float w = std::min(maxW, (float)fsz * 22.0f), hh = kh + 16.0f;
    HBox me{};
    if (drawnPrev && prevN == h.cur)
    {
        const Clay_ElementData d = Clay_GetElementData(CLAY_ID("HintPopup"));
        if (d.found && d.boundingBox.width > 0.0f)
        {
            me = {d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height};
            w = me.w;
            hh = me.h;
        }
    }

    // What it keeps clear of.
    HBox obst[12];
    int no = 0;
    HBox left{}, right{}, cap{};
    const bool haveLeft = hDrawn(CLAY_ID("LeftPanel"), left), haveRight = hDrawn(CLAY_ID("RightPanel"), right);
    if (haveLeft)
        obst[no++] = left;
    if (haveRight)
        obst[no++] = right;
    for (const Clay_ElementId id : {CLAY_IDI("HudPopupPanel", 0), CLAY_IDI("HudPopupPanel", 1), CLAY_ID("SelSatChip"),
                                    CLAY_ID("FollowHud"), CLAY_ID("FollowAgainBtn"), CLAY_ID("CineHud"), CLAY_ID("FpsRing")})
        if (no < 12 && hDrawn(id, obst[no]))
            ++no;
    const bool capNow = introCaptionBuilt_ && hDrawn(CLAY_ID("CineCaption"), cap);
    auto hitsAny = [&](const HBox &r, bool withCap)
    {
        for (int i = 0; i < no; ++i)
            if (hOverlap(r, obst[i]))
                return true;
        return withCap && capNow && hOverlap(r, cap);
    };
    auto clampXY = [&](float &x, float &y)
    {
        x = std::clamp(x, margin, std::max(margin, W - margin - w));
        y = std::clamp(y, margin, std::max(margin, H - margin - hh));
    };
    // Up past whatever it would cover, then on screen.
    auto avoid = [&](float &x, float &y)
    {
        for (int it = 0; it < 8; ++it)
        {
            bool moved = false;
            const HBox r{x, y, w, hh};
            for (int i = 0; i < no && !moved; ++i)
                if (hOverlap(r, obst[i]))
                {
                    y = obst[i].y - gap - hh;
                    moved = true;
                }
            if (!moved && capNow && hOverlap(r, cap))
            {
                y = cap.y - gap - hh;
                moved = true;
            }
            if (!moved)
                break;
        }
        clampXY(x, y);
    };

    float x = 0.0f, y = 0.0f;
    bool attachCap = false, placed = false;
    int place = 0;
    // 9 and 10: above the toolbar they describe, unless that would cover the caption or another HUD element.
    if ((h.cur == 9 && haveLeft) || (h.cur == 10 && haveRight))
    {
        const HBox &p = h.cur == 9 ? left : right;
        x = h.cur == 9 ? p.x : p.x + p.w - w;
        y = p.y - 4.0f - gap - hh; // (clear of its outline, 4 px out)
        avoid(x, y);
        if (!hitsAny({x, y, w, hh}, true))
        {
            placed = true;
            place = h.cur == 9 ? 2 : 3;
        }
    }
    if (!placed && capNow)
    {
        // On the caption band's top edge (Clay places it there this frame, so it rises when the caption wraps).
        x = cap.x + cap.w * 0.5f - w * 0.5f;
        y = cap.y - gap - hh;
        place = 1;
        if (!hitsAny({x, y, w, hh}, false))
            attachCap = true;
        else
            avoid(x, y);
        placed = true;
    }
    if (!placed && showIntro && drawnPrev && prevN == h.cur && h.place == 1)
    {
        x = me.x; // between two captions (the next one's first frame draws nothing): stay put
        y = me.y;
        place = 1;
        placed = true;
    }
    if (!placed)
    {
        float clear = 24.0f; // bottom centre, clear of both corner toolbars when they draw
        if (haveLeft)
            clear = std::max(clear, H - left.y + 12.0f);
        if (haveRight)
            clear = std::max(clear, H - right.y + 12.0f);
        x = (W - w) * 0.5f;
        y = H - clear - hh;
        avoid(x, y);
        place = 0;
    }
    h.place = place;

    Clay_FloatingElementConfig fl{};
    fl.zIndex = kHintZ;
    fl.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    if (attachCap)
    {
        fl.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
        fl.parentId = CLAY_ID("CineCaption").id;
        fl.attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM, .parent = CLAY_ATTACH_POINT_CENTER_TOP};
        fl.offset = {0.0f, -gap};
    }
    else
    {
        fl.attachTo = CLAY_ATTACH_TO_ROOT;
        fl.offset = {x, y};
    }
    Clay_ElementDeclaration d = uiHudPanel(fl, 6);
    d.layout.layoutDirection = CLAY_LEFT_TO_RIGHT;
    d.layout.sizing = {CLAY_SIZING_FIT(0, maxW), CLAY_SIZING_FIT(0)};
    d.layout.childAlignment = {.y = CLAY_ALIGN_Y_CENTER};
    d.backgroundColor.a *= a;
    Clay_Color tc = Pal::textPrimary;
    tc.a *= a;
    CLAY(CLAY_ID("HintPopup"), d)
    {
        for (int i = 0; i < np; ++i)
        {
            if (parts[i].key)
            {
                const float kw = std::max(kh, (float)fsz * 0.62f * (float)strlen(parts[i].s) + 14.0f);
                uiKeycap(CLAY_IDI("HintKey", i), kw, kh, CLAY_CORNER_RADIUS(4), parts[i].s, fsz, false, false, 0.0f,
                         nullptr, a);
            }
            else
                CLAY_TEXT(hintStr(parts[i].s), CLAY_TEXT_CONFIG({.textColor = tc, .fontSize = fsz}));
        }
    }
    hintDrawnPrev_ = true;
    hintDrawnPrevN_ = h.cur;

    // 9 and 10: an outline around the toolbar (also when the popup had to go elsewhere).
    if ((h.cur == 9 && haveLeft) || (h.cur == 10 && haveRight))
    {
        const HBox &p = h.cur == 9 ? left : right;
        const float m = 4.0f, pulse = 0.5f + 0.5f * sinf(h.clock * 4.5f);
        CLAY(CLAY_ID("HintOutline"),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(p.w + 2.0f * m), CLAY_SIZING_FIXED(p.h + 2.0f * m)}},
              .backgroundColor = {Pal::tutHot.r, Pal::tutHot.g, Pal::tutHot.b, (12.0f + 18.0f * pulse) * a},
              .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius + m),
              .floating = {.offset = {p.x - m, p.y - m},
                           .zIndex = kHintZ,
                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                           .attachTo = CLAY_ATTACH_TO_ROOT},
              .border = {.color = {Pal::tutHot.r, Pal::tutHot.g, Pal::tutHot.b, (140.0f + 115.0f * pulse) * a},
                         .width = {2, 2, 2, 2, 0}}})
        {
        }
    }
}

// ─── Harness ──────────────────────────────────────────────────────────────────────────────────────
nlohmann::json SatelliteSim::hintsStateJson() const
{
    const HintRun &h = hint_;
    const HintGates &g = h.g;
    const bool pad = lastInputWasGamepad, menu = hintMenuMode();
    nlohmann::json j;
    j["enabled"] = hintsPass_ > 0 && (!harness::active() || hintsHarnessOn_);
    j["harness_on"] = hintsHarnessOn_;
    j["pass"] = hintsPass_ < 0 ? "undecided" : hintsPass_ == 0 ? "off" : hintsPass_ == 1 ? "running" : "complete";
    j["complete"] = hintsPass_ == 2;
    j["tutorial_done"] = tutorialDone;
    j["gates"] = {{"looked", g.looked}, {"moved", g.moved}, {"climbed", g.climbed}, {"boosted", g.boosted},
                  {"zoomed", g.zoomed}, {"clicked", g.clicked}, {"left_by_tab", g.leftByTab},
                  {"left_by_right_click", g.leftByRightClick}, {"returned", g.returned}, {"drag_looked", g.dragLooked},
                  {"selected", g.selected}, {"time_bar_shown", g.timeBarShown}, {"menu_bar_shown", g.menuBarShown}};
    int clicks5 = 0;
    for (int i = 0; i < std::min(h.clickN, 8); ++i)
        if (h.clock - h.clickAt[i] <= kHintClickWinS)
            ++clicks5;
    j["live"] = {{"mode", menu ? "menu" : "fps"},
                 {"pad", pad},
                 {"moving", hintMoveIn_},
                 {"moving_s", h.movingS},
                 {"idle_s", h.idleS},
                 {"menu_idle_s", h.menuIdleS},
                 {"still_s", h.stillS},
                 {"fps_clicks_5s", clicks5},
                 {"caption_index", introCaptionIndex},
                 {"captions_ended", !showIntro},
                 {"before_beat", showIntro && introElapsed < kIntroKeyframes[kHintBeat].t},
                 {"window_open", fpsWindowMask() != 0},
                 {"ever_menu", h.everMenu},
                 {"suppressed", cineActive() || photoState != 0 || tutActive || !uiVisible},
                 {"clock_s", h.clock},
                 {"look_deg", h.lookDeg},
                 {"move_s", h.moveS},
                 {"climb_m", h.climbM}};
    if (h.cur)
    {
        nlohmann::json s = {{"n", h.cur},
                            {"text_id", hintTextId(h.cur, pad, menu)},
                            {"text", hintText(h.cur, pad, menu)},
                            {"alpha", h.alpha},
                            {"shown_s", h.curS},
                            {"leaving", h.leaving},
                            {"forced", h.forced},
                            {"gate_met_s", h.gateS},
                            {"place", kPlaceNames[std::clamp(h.place, 0, 3)]}};
        const Clay_ElementData d = Clay_GetElementData(CLAY_ID("HintPopup"));
        if (hintDrawnPrev_ && d.found)
            s["box"] = {d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height};
        j["showing"] = s;
        j["showing_n"] = h.cur;
    }
    else
    {
        j["showing"] = nullptr;
        j["showing_n"] = 0;
    }
    nlohmann::json offers = nlohmann::json::object(), texts = nlohmann::json::object(), retired = nlohmann::json::array();
    for (int n = 1; n <= kHintCount; ++n)
    {
        offers[std::to_string(n)] = h.offers[n];
        texts[std::to_string(n)] = hintText(n, pad, menu);
        if (h.retired[n])
            retired.push_back(n);
    }
    j["offers"] = offers;
    j["retired"] = retired;
    j["texts"] = texts;
    j["seen"] = h.seen;
    nlohmann::json log = nlohmann::json::array();
    for (const HintLogEntry &e : h.log)
        log.push_back({{"n", e.n}, {"shown_s", e.shownS}, {"gone_s", e.goneS}, {"gate_met_s", e.gateAtS}, {"how", e.how}});
    j["log"] = log;
    return j;
}

// hints on | off | reset | state | set <gate> [0|1] | force <n> [pad=0|1]
harness::Status SatelliteSim::harnessHints(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    const std::string sub = c.pos.empty() ? std::string("state") : c.pos[0];
    if (c.has("pad"))
        lastInputWasGamepad = c.num("pad", 0.0) > 0.5; // the pad's labels and mode (the harness has no controller)
    if (sub == "on")
    {
        // As a session starts: a first run's pass unless display.tutorial_done (then nothing, as a returning player).
        hintsHarnessOn_ = true;
        hintsReset();
        hintsPass_ = tutorialDone ? 0 : 1;
    }
    else if (sub == "off")
    {
        hintsHarnessOn_ = false;
        hint_.cur = 0;
        hint_.alpha = 0.0f;
    }
    else if (sub == "reset")
        hintsReset(); // a new pass, as an opening's first frame starts one
    else if (sub == "set")
    {
        const std::string gname = c.pos.size() > 1 ? c.pos[1] : std::string();
        const bool v = c.pos.size() > 2 ? c.pos[2] != "0" : true;
        HintGates &g = hint_.g;
        bool *p = gname == "looked"                ? &g.looked
                  : gname == "moved"               ? &g.moved
                  : gname == "climbed"             ? &g.climbed
                  : gname == "boosted"             ? &g.boosted
                  : gname == "zoomed"              ? &g.zoomed
                  : gname == "clicked"             ? &g.clicked
                  : gname == "left_by_tab"         ? &g.leftByTab
                  : gname == "left_by_right_click" ? &g.leftByRightClick
                  : gname == "returned"            ? &g.returned
                  : gname == "drag_looked"         ? &g.dragLooked
                  : gname == "selected"            ? &g.selected
                  : gname == "time_bar_shown"      ? &g.timeBarShown
                  : gname == "menu_bar_shown"      ? &g.menuBarShown
                                                   : nullptr;
        if (!p)
        {
            a.error = "hints set <looked|moved|climbed|boosted|zoomed|clicked|left_by_tab|left_by_right_click|returned|"
                      "drag_looked|selected|time_bar_shown|menu_bar_shown> [0|1]";
            return harness::Status::Error;
        }
        *p = v;
    }
    else if (sub == "force")
    {
        const int n = c.pos.size() > 1 ? std::atoi(c.pos[1].c_str()) : 0;
        if (n < 0 || n > kHintCount)
        {
            a.error = "hints force <1-10> (0 = let go)";
            return harness::Status::Error;
        }
        // Pinned at full alpha, no rules (layout checks); 0 lets the scheduler have the slot back.
        hint_.cur = n;
        hint_.forced = n > 0;
        hint_.alpha = n > 0 ? 1.0f : 0.0f;
        hint_.leaving = false;
        hint_.curS = 0.0f;
        if (n == 0)
            hint_.freeAt = hint_.clock;
    }
    else if (sub != "state")
    {
        a.error = "hints on | off | reset | state | set <gate> [0|1] | force <n> [pad=0|1]";
        return harness::Status::Error;
    }
    a.result["hints"] = hintsStateJson();
    char buf[160];
    snprintf(buf, sizeof(buf), "hints %s, showing %d, seen [%s]%s", hintsPass_ == 2 ? "complete" : hintsPass_ == 1 ? "running" : "off",
             hint_.cur, hint_.seen.c_str(), (harness::active() && !hintsHarnessOn_) ? " (off: `hints on`)" : "");
    a.result["message"] = std::string(buf);
    return harness::Status::Done;
}
