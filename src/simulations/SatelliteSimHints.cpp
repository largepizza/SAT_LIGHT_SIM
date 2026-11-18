// SatelliteSimHints.cpp — the first run's hints (2026-11-17; docs/subsystems/ui.md "Hints").
//
// One small hint at a time (HintPopup: keycaps, then one line), chosen from what the player has and has not done:
//   1 look   2 move   4 go faster   3 rise / descend   5 zoom   6 the cursor (leave FPS mode)   11 right-drag to look
//   7 click a satellite   9 the time bar   10 the menu buttons   8 back to looking around (FPS mode again)
// and, outliving that first-run pass (round 2), the contextual hints, armed in every session until used (their gates
// and offer counts kept in settings): 12 roll (the horizon free), 13 Go to (a selection), 14 the coronagraph (the
// Sun stared at), 15 the star lock (past GEO), and one per silence between music tracks: 16 Sky events, 17 Missions,
// 18 Weather, 19 the picture keys.
// Gates (HintGates) are sticky facts since the pass began; hintsTick measures them, keeps the live signals (the mode,
// movement now, idle time, recent FPS clicks, the captions) and runs the scheduler: one hint on screen, faded in and
// out, at least kHintGapS between two, gone kHintLingerS after its gate is met (at once when its condition stops
// holding), a gate met before its hint showed retires it, an ignored hint rests; the order of the brief's table.
// buildHints draws it: above the opening's caption band while one shows, else bottom centre clear of the toolbars;
// 9 and 10 above the toolbar they describe, with an outline around it. Every clock is frame time (the caption
// clock's), so `--fixed-dt` runs are exact. The only teaching path (the first-run card tutorial it replaced was
// deleted 2026-11-18).
#include "../AudioSystem.h"
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
// The contextual hints 12..19, by their gate's name (settings display.hint_gates / hint_offers, harness `hints set`).
const char *const kHintCtxNames[] = {"rolled",   "gone_to",  "coronagraph", "star_lock",
                                     "sky_events", "missions", "weather",     "screenshot"};
constexpr int kHintCtxN = (int)(sizeof(kHintCtxNames) / sizeof(kHintCtxNames[0]));
} // namespace

// ─── State ────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::hintsReset(bool all)
{
    hint_ = HintRun{};
    hintsPass_ = 1;
    hintMoveIn_ = hintBoostIn_ = hintZoomIn_ = hintInputIn_ = false;
    hintRollIn_ = hintShotIn_ = false;
    hintClimbM_ = 0.0f;
    if (all) // (Replay Intro, `hints reset`: the contextual hints teach again)
    {
        hintSaved_ = HintSaved{};
        hintSaveDirty_ = true;
    }
    hintsPrime(); // (now: an edge in the same frame, a harness command after `hints on`, is not swallowed)
}

// The contextual hints' gates: facts, measured every frame whether hints show or not, and kept (display.hint_gates).
void SatelliteSim::hintsCtxGates()
{
    auto meet = [&](int n, bool v)
    {
        if (v && !hintSaved_.met[n])
        {
            hintSaved_.met[n] = true;
            hintSaveDirty_ = true;
        }
    };
    meet(12, hintRollIn_);                    // Z / C rolled the view (the horizon free)
    meet(13, followActive && !cineActive());  // a Go to (G, the selection panel's button, a Watch...)
    meet(14, inertialOcculter);               // the coronagraph on
    meet(15, inertialActive);                 // the view locked to the stars
    meet(16, skChrome.open);
    meet(17, msChrome.open);
    meet(18, wxChrome.open);
    meet(19, hintShotIn_);                    // a screenshot (either key, the button) or an HQ photo
    hintRollIn_ = hintShotIn_ = false;
}

void SatelliteSim::hintsLoadSaved(const nlohmann::json &d)
{
    static_assert(kHintFirstCtx + kHintCtxN - 1 == kHintCount, "a name per contextual hint");
    for (int i = 0; i < kHintCtxN; ++i)
    {
        const int n = kHintFirstCtx + i;
        if (d.contains("hint_gates") && d["hint_gates"].is_object())
            hintSaved_.met[n] = d["hint_gates"].value(kHintCtxNames[i], hintSaved_.met[n]);
        if (d.contains("hint_offers") && d["hint_offers"].is_object())
            hintSaved_.offers[n] = std::max(0, d["hint_offers"].value(kHintCtxNames[i], hintSaved_.offers[n]));
    }
}

nlohmann::json SatelliteSim::hintsSavedGatesJson() const
{
    nlohmann::json j = nlohmann::json::object();
    for (int i = 0; i < kHintCtxN; ++i)
        j[kHintCtxNames[i]] = hintSaved_.met[kHintFirstCtx + i];
    return j;
}

nlohmann::json SatelliteSim::hintsSavedOffersJson() const
{
    nlohmann::json j = nlohmann::json::object();
    for (int i = 0; i < kHintCtxN; ++i)
        j[kHintCtxNames[i]] = hintSaved_.offers[kHintFirstCtx + i];
    return j;
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
    // The contextual hints' keys: the pad's button when one is bound, else the keyboard's (no pad default for roll,
    // Go to or the picture keys).
    auto keyAny = [&](int kb) { key(kb, pad && kb >= 0 && kb < (int)keybindings.size() && keybindings[kb].gpButton >= 0); };
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
    case 11:
        text(tx(Txt::HintLookDrag));
        break;
    case 12:
        keyAny(KB_ROLL_LEFT);
        text("/");
        keyAny(KB_ROLL_RIGHT);
        text(tx(Txt::HintToRoll));
        break;
    case 13:
        keyAny(KB_FOLLOW_STAY);
        text(tx(Txt::HintGoTo));
        break;
    case 14:
        text(tx(Txt::HintCoronagraph));
        break;
    case 15:
        text(tx(Txt::HintStarLock));
        break;
    case 16:
        text(tx(Txt::HintSkyEvents));
        break;
    case 17:
        text(tx(Txt::HintMissions));
        break;
    case 18:
        text(tx(Txt::HintWeather));
        break;
    case 19:
        keyAny(KB_SCREENSHOT);
        text(tx(Txt::HintScreenshot));
        keyAny(KB_PHOTO);
        text(tx(Txt::HintHqPhoto));
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
    case 11: return "HintLookDrag";
    case 12: return "HintToRoll";
    case 13: return "HintGoTo";
    case 14: return "HintCoronagraph";
    case 15: return "HintStarLock";
    case 16: return "HintSkyEvents";
    case 17: return "HintMissions";
    case 18: return "HintWeather";
    case 19: return "HintScreenshot";
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
        hintsPass_ = firstRunDone ? 0 : 1; // the first frame: a genuine first run, or not
    HintRun &h = hint_;
    HintGates &g = h.g;
    hintsCtxGates();
    // Off: the setting, or a harness run without `hints on`. A finished (or never started) pass still runs the
    // contextual hints (12-19).
    if (!hintsEnabled || (harness::active() && !hintsHarnessOn_))
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
        // (a right-drag in menu mode; harness `inertial drag` holds the same button)
        if ((camera.captured || harnessDragFrames_ > 0 || harnessDragWarpPending_) && !fpsLive_)
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
        case 11: return g.dragLooked;
        default: return n >= kHintFirstCtx && n <= kHintCount && hintSaved_.met[n]; // contextual: kept across sessions
        }
    };
    for (int n = 1; n < kHintFirstCtx; ++n) // a gate met before its hint ever showed retires it (silently)
        if (h.offers[n] == 0 && h.cur != n && gate(n))
            h.retired[n] = true;

    // ── The first run is complete ──
    if (hintsPass_ == 1 && captionsEnded && g.selected && g.moved && g.timeBarShown && g.menuBarShown)
    {
        hintsPass_ = 2;
        firstRunDone = true; // display.tutorial_done: no pass on later runs
        saveSettings();
    }

    // ── Conditions ──
    const bool suppressed = cineActive() || photoState != 0 || !uiVisible;
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
        // 4 then 3 (2026-11-17 round 2): near dark ground the move reads as going nowhere until boost shows, so boost
        // comes as soon as `moved` is met (kHintHandoffS after 2 leaves), and rise / descend once boost was used or
        // its hint had its turn.
        case 3: return g.moved && (g.boosted || h.offers[4] > 0);
        case 4: return g.moved;
        case 5: return h.stillS >= kHintStillS && satRef();
        case 6: return fpsH && !h.everMenu && (h.idleS >= kHintIdleS || clicks5 >= 2 || captionsEnded);
        case 7: return menu;
        case 8: return menu && ((g.timeBarShown && g.menuBarShown) || h.menuIdleS >= kHintMenuIdleS);
        case 9: return menu && g.selected;
        case 10: return menu && g.timeBarShown;
        case 11: return menu && !lastInputWasGamepad && !g.selected; // (the pad's stick looks in either mode)
        default: return false;
        }
    };
    // Holds while it shows (else it fades at once). 5 holds on less than it needs to be offered: the still view is
    // what calls for it, not what it is about.
    auto holds = [&](int n) -> bool
    {
        switch (n)
        {
        case 3: return hintMoveIn_ || g.moved;
        case 5: return satRef();
        case 6: return fpsH && !h.everMenu;
        case 7:
        case 8:
        case 10: return menu;
        case 9: return menu && g.selected;
        case 11: return menu && !g.selected;
        default: return true;
        }
    };
    auto allowed = [&](int n) -> bool
    {
        if (gate(n))
            return false;
        if (hintsPass_ <= 0) // no pass (a returning player): the contextual hints only
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
            else if (menu && !g.selected)
            {
                if (ok(11)) // the first time in menu mode: the right-drag look, then 7
                    return 11;
                if (offerable(7, true))
                    return 7;
            }
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
        if (g.moved) // e: boost first, then rise / descend
        {
            if (ok(4))
                return 4;
            if (!g.climbed && ok(3))
                return 3;
        }
        if ((h.idleS >= kHintIdleS || clicks5 >= 2) && ok(6)) // f
            return 6;
        if (menu && !g.selected) // g
        {
            if (ok(11))
                return 11;
            if (ok(7))
                return 7;
        }
        for (int n : {2, 3, 5}) // h
            if (ok(n))
                return n;
        if (menu && h.menuIdleS >= kHintMenuIdleS && ok(8))
            return 8;
        return 0;
    };

    // ── Contextual hints (12-19): their live signals ──
    // The Sun near the view centre, up and not behind the Earth (the glare's own limb test, SatelliteSim.cpp).
    float sunDeg = 180.0f;
    {
        constexpr float kEarthR = 6'371'000.0f; // (SatelliteSim.cpp's kEarthRadius, file-local there)
        const float obsR = glm::length(obsECI);
        const float limbZ = obsR > kEarthR ? -sqrtf(std::max(0.0f, 1.0f - (kEarthR / obsR) * (kEarthR / obsR))) : 0.0f;
        const glm::vec3 s(sunDirENU);
        if (sunDirENU.w >= limbZ && glm::length(s) > 0.5f)
        {
            const float az = glm::radians(camera.azDeg), el = glm::radians(camera.elDeg);
            const glm::vec3 v(cosf(el) * sinf(az), cosf(el) * cosf(az), sinf(el)); // ENU, az from north toward east
            sunDeg = glm::degrees(acosf(std::clamp(glm::dot(v, glm::normalize(s)), -1.0f, 1.0f)));
        }
        if (sunDeg <= kHintSunDeg && !inertialOcculter)
            h.sunS += dt;
    }
    const bool aboveGeo = camEyeAltM() > kHintGeoKm * 1000.0;
    // The silence between two music tracks (or, with no music audible, one "gap" every kHintNoMusicS).
    bool audible = false, songGap = false;
    if (audio_)
    {
        audible = audio_->musicOn() && !audio_->musicPaused() &&
                  audio_->getMasterVolume() * audio_->getMusicVolume() * musicAltFade > 0.03f;
        songGap = audible && !audio_->trackPlaying() && audio_->gapRemaining() > 0.0f;
    }
    h.silentS = audible ? 0.0f : h.silentS + dt;
    if (h.silentS >= kHintNoMusicS)
    {
        h.silentS = 0.0f;
        h.fakeGapS = kHintGapWindowS;
    }
    const bool gapOpen = songGap || h.fakeGapS > 0.0f;
    h.fakeGapS = std::max(0.0f, h.fakeGapS - dt);
    if (gapOpen && !h.gapPrev)
        h.gapUsed = false; // each gap offers one hint
    h.gapPrev = gapOpen;
    h.sunDeg = sunDeg;
    h.audible = audible;
    h.songGap = songGap;
    h.gapOpen = gapOpen;
    const bool satWithMesh = selectedSatIndex >= 0 && selectedSatIndex < (int)satOrbits.size() &&
                             meshRenderer.typeMesh((int)satOrbits[selectedSatIndex].typeIdx) != nullptr;
    // Never over the captions, an open window or text entry (and only when no pass hint wants the slot: pickCtx is
    // asked after pick).
    const bool ctxFree = captionsEnded && fpsWindowMask() == 0 && !textEditing();
    auto ctxCond = [&](int n) -> bool
    {
        switch (n)
        {
        case 12: return !camLevelActive();
        case 13: return satWithMesh && !followActive && !cineActive();
        case 14: return h.sunS >= kHintSunS;
        case 15: return aboveGeo && !inertialActive;
        case 18: return !weatherUnavailable();
        default: return true; // 16, 17, 19: a gap (pickCtx)
        }
    };
    auto ctxHolds = [&](int n) -> bool
    {
        if (!ctxFree)
            return false;
        switch (n)
        {
        case 12: return !camLevelActive();
        case 13: return satWithMesh && !followActive;
        case 15: return aboveGeo;
        default: return true;
        }
    };
    auto ctxOk = [&](int n) -> bool
    {
        return !hintSaved_.met[n] && hintSaved_.offers[n] < kHintMaxOffers && h.clock >= h.restUntil[n] && ctxCond(n);
    };
    auto pickCtx = [&]() -> int
    {
        if (!ctxFree)
            return 0;
        for (int n : {12, 15, 14, 13})
            if (ctxOk(n))
                return n;
        // A gap: the first of Sky events, Missions, Weather, the picture keys not done yet, the least offered first (so
        // an ignored one waits until the others had their turn).
        int best = 0;
        if (gapOpen && !h.gapUsed)
            for (int n = 16; n <= 19; ++n)
                if (ctxOk(n) && (best == 0 || hintSaved_.offers[n] < hintSaved_.offers[best]))
                    best = n;
        return best;
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
            else if (n >= kHintFirstCtx ? !ctxHolds(n) : !holds(n))
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
                if (n == 14)
                    h.sunS = 0.0f; // (its second offer needs the Sun stared at again)
            }
            else if (newClick && n != 6 && !neverTO(n) && clicks5 >= 2 && ok(6))
            {
                // Two quick clicks in FPS mode answer themselves: hint 6 takes the slot at once (the one it replaces
                // gets its offer back).
                leave("preempted");
                h.offers[n] = std::max(0, h.offers[n] - 1);
                if (n >= kHintFirstCtx)
                {
                    hintSaved_.offers[n] = std::max(0, hintSaved_.offers[n] - 1);
                    hintSaveDirty_ = true;
                }
                h.preempt = 6;
            }
            else if (n >= kHintFirstCtx && pick() != 0)
            {
                // A pass hint wants the slot (the player Tabbed, selected...): the contextual one gives way and gets
                // its offer back.
                leave("yield");
                h.offers[n] = std::max(0, h.offers[n] - 1);
                hintSaved_.offers[n] = std::max(0, hintSaved_.offers[n] - 1);
                hintSaveDirty_ = true;
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
        const float freeS = h.clock - h.freeAt;
        if (h.preempt && ok(h.preempt))
            n = h.preempt;
        else if (freeS >= kHintHandoffS)
        {
            // The pass first; a contextual hint only when no pass hint wants the slot. Boost (4) follows sooner.
            int c = pick();
            if (c == 0)
                c = pickCtx();
            if (c && (freeS >= kHintGapS || c == 4))
                n = c;
        }
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
            if (n >= kHintFirstCtx)
            {
                ++hintSaved_.offers[n]; // (kept: a returning player is not offered it again past kHintMaxOffers)
                hintSaveDirty_ = true;
                if (n >= 16)
                    h.gapUsed = true;
            }
            if (hintsPass_ == 2)
                h.postFired[n] = true;
            if (!h.seen.empty())
                h.seen += ',';
            h.seen += std::to_string(n);
            h.log.push_back({n, h.clock, -1.0f, -1.0f, ""});
        }
    }
    if (hintSaveDirty_)
    {
        hintSaveDirty_ = false;
        saveSettings(); // (rare: a contextual gate met, an offer counted, a reset)
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

    // What it points at (an outline round it, the popup above it): 9 / 10 a whole toolbar, 14-19 a button on one.
    HBox tgt{};
    bool haveTgt = false;
    const char *tgtId = nullptr;
    switch (h.cur)
    {
    case 9:
        haveTgt = haveLeft;
        tgt = left;
        break;
    case 10:
        haveTgt = haveRight;
        tgt = right;
        break;
    case 14: tgtId = "HudViewBtn"; break;
    case 15: tgtId = "HudMoveBtn"; break;
    case 16: tgtId = "TimeSkyBtn"; break;
    case 17: tgtId = "TimeMissionsBtn"; break;
    case 18: tgtId = "TimeWeatherBtn"; break;
    case 19: // the screenshot and HQ photo buttons, side by side
        if ((haveTgt = hDrawn(CLAY_ID("TimeScreenshotBtn"), tgt)))
        {
            HBox b{};
            if (hDrawn(CLAY_ID("TimePhotoBtn"), b))
            {
                const float x1 = std::max(tgt.x + tgt.w, b.x + b.w), y1 = std::max(tgt.y + tgt.h, b.y + b.h);
                tgt.x = std::min(tgt.x, b.x);
                tgt.y = std::min(tgt.y, b.y);
                tgt.w = x1 - tgt.x;
                tgt.h = y1 - tgt.y;
            }
        }
        break;
    default: break;
    }
    if (tgtId)
        haveTgt = hDrawn(Clay_GetElementId(hintStr(tgtId)), tgt);
    const bool tgtPanel = h.cur == 9 || h.cur == 10;
    const bool tgtRight = haveTgt && tgt.x + tgt.w * 0.5f > W * 0.5f;

    float x = 0.0f, y = 0.0f;
    bool attachCap = false, placed = false;
    int place = 0;
    // Above what it points at (a toolbar: aligned to its outer edge; a button: centred on it, then clear of its
    // toolbar), unless that would cover the caption or another HUD element.
    if (haveTgt)
    {
        x = tgtPanel ? (tgtRight ? tgt.x + tgt.w - w : tgt.x) : tgt.x + tgt.w * 0.5f - w * 0.5f;
        y = tgt.y - 4.0f - gap - hh; // (clear of its outline, 4 px out)
        avoid(x, y);
        if (!hitsAny({x, y, w, hh}, true))
        {
            placed = true;
            place = tgtRight ? 3 : 2;
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

    // An outline around what it points at (also when the popup had to go elsewhere).
    if (haveTgt)
    {
        const HBox &p = tgt;
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
    j["enabled"] = hintsEnabled && (!harness::active() || hintsHarnessOn_);
    j["setting"] = hintsEnabled; // display.hints
    j["harness_on"] = hintsHarnessOn_;
    j["pass"] = hintsPass_ < 0 ? "undecided" : hintsPass_ == 0 ? "off" : hintsPass_ == 1 ? "running" : "complete";
    j["complete"] = hintsPass_ == 2;
    j["tutorial_done"] = firstRunDone;
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
                 {"suppressed", cineActive() || photoState != 0 || !uiVisible},
                 {"clock_s", h.clock},
                 {"look_deg", h.lookDeg},
                 {"move_s", h.moveS},
                 {"climb_m", h.climbM},
                 // the contextual hints' signals
                 {"level_active", camLevelActive()},
                 {"eye_alt_km", camEyeAltM() / 1000.0},
                 {"above_geo", camEyeAltM() > kHintGeoKm * 1000.0},
                 {"sun_deg", h.sunDeg},
                 {"sun_s", h.sunS},
                 {"music_audible", h.audible},
                 {"song_gap", h.songGap},
                 {"gap_open", h.gapOpen},
                 {"gap_used", h.gapUsed},
                 {"silent_s", h.silentS},
                 {"text_entry", textEditing()}};
    nlohmann::json saved = {{"gates", hintsSavedGatesJson()}, {"offers", hintsSavedOffersJson()}};
    {
        int done = 0;
        for (int n = kHintFirstCtx; n <= kHintCount; ++n)
            done += (hintSaved_.met[n] || hintSaved_.offers[n] >= kHintMaxOffers) ? 1 : 0;
        saved["all_done"] = done == kHintCtxN; // (a returning player with all done sees nothing)
    }
    j["saved"] = saved;
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
        // As a session starts: a first run's pass unless display.tutorial_done (then the contextual hints only, as a
        // returning player: their saved gates and counts are kept).
        hintsHarnessOn_ = true;
        hintsReset();
        hintsPass_ = firstRunDone ? 0 : 1;
    }
    else if (sub == "off")
    {
        hintsHarnessOn_ = false;
        hint_.cur = 0;
        hint_.alpha = 0.0f;
    }
    else if (sub == "reset")
        hintsReset(true); // a new pass and the contextual hints untaught, as a Replay Intro's first frame
    else if (sub == "gap")
    {
        // A song gap now (as the silence between two tracks: one contextual hint 16-19 may show), whatever the music.
        hint_.fakeGapS = c.num("s", (double)kHintGapWindowS);
        hint_.gapUsed = false;
    }
    else if (sub == "set")
    {
        const std::string gname = c.pos.size() > 1 ? c.pos[1] : std::string();
        const bool v = c.pos.size() > 2 ? c.pos[2] != "0" : true;
        for (int i = 0; i < kHintCtxN; ++i) // a contextual gate (kept in settings)
            if (gname == kHintCtxNames[i])
            {
                hintSaved_.met[kHintFirstCtx + i] = v;
                hintSaveDirty_ = true;
                a.result["hints"] = hintsStateJson();
                return harness::Status::Done;
            }
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
                      "drag_looked|selected|time_bar_shown|menu_bar_shown|rolled|gone_to|coronagraph|star_lock|"
                      "sky_events|missions|weather|screenshot> [0|1]";
            return harness::Status::Error;
        }
        *p = v;
    }
    else if (sub == "force")
    {
        const int n = c.pos.size() > 1 ? std::atoi(c.pos[1].c_str()) : 0;
        if (n < 0 || n > kHintCount)
        {
            a.error = "hints force <1-19> (0 = let go)";
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
        a.error = "hints on | off | reset | state | set <gate> [0|1] | force <n> | gap [s=] [pad=0|1]";
        return harness::Status::Error;
    }
    a.result["hints"] = hintsStateJson();
    char buf[160];
    snprintf(buf, sizeof(buf), "hints %s, showing %d, seen [%s]%s", hintsPass_ == 2 ? "complete" : hintsPass_ == 1 ? "running" : "off",
             hint_.cur, hint_.seen.c_str(), (harness::active() && !hintsHarnessOn_) ? " (off: `hints on`)" : "");
    a.result["message"] = std::string(buf);
    return harness::Status::Done;
}
