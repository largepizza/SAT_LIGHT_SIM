// SatelliteSimHudPopups.cpp - the right HUD panel's Movement and View popups (2026-10-29, issue 20261029_180413;
// CLAUDE.md "HUD popups").
//
// Two small rounded panels in the style of the selected-satellite panel, opened from the buttons where the MSL / AGL
// toggle used to be (SatelliteSimUI.cpp buildRightHudPanel). UNPINNED a popup sits over its button and closes on any
// click in the sim (hudPopupsTick); PINNED (the pin button, top right) it stays, and its title bar drags it. Every
// row is a UI-kit row, so it matches the settings tabs; every control is also a Settings row (the rule: a control
// that changes the look has one) - Movement: reference frame (Earth / Stars = the inertial lock), cinematic mode
// (Controls tab), altitude gauge (Display tab); View: exposure (Photometry), coronagraph (Photometry),
// precipitation (Weather).
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "clay.h"
#include "UIPalette.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr int kIconPinH = 43; // SatelliteSimUI.cpp loadUiIcons: 41 movement, 42 view, 43 pin
constexpr float kDefaultExposureEV = 0.0f;
} // namespace

// Called from buildUI after the scene pick: an unpinned popup closes when the player clicks in the sim (a click on
// any UI element, the popup itself included, is not "in the sim": mouseOverUI is last frame's capture result).
void SatelliteSim::hudPopupsTick(const UIInput &inp, UIRenderer &ui)
{
    if (!(inp.lmbPressed || inp.rmbPressed) || ui.mouseOverUI())
        return;
    if (!hudMove.pinned)
        hudMove.open = false;
    if (!hudView.pinned)
        hudView.open = false;
}

void SatelliteSim::buildHudPopup(const UIInput &inp, UIRenderer &ui, int which)
{
    HudPopup &hp = which ? hudView : hudMove;
    if (!hp.open)
    {
        hp.dragging = false;
        return;
    }
    const Clay_ElementId btnId = which ? CLAY_ID("HudViewBtn") : CLAY_ID("HudMoveBtn");
    const Clay_ElementId panelId = CLAY_IDI("HudPopupPanel", which);
    const Clay_ElementId titleId = CLAY_IDI("HudPopupTitle", which);
    const Clay_ElementData panelD = Clay_GetElementData(panelId);
    const Clay_ElementData btnD = Clay_GetElementData(btnId);
    const float panelW = panelD.found ? panelD.boundingBox.width : (float)fs(260);
    const float panelH = panelD.found ? panelD.boundingBox.height : (float)fs(140);

    // ── Where it is: pinned = its own spot (dragged by the title), else over its button ──
    if (hp.pinned && hp.x < 0.0f)
    {
        hp.x = panelD.found ? panelD.boundingBox.x : std::max(8.0f, inp.screenW - panelW - 12.0f);
        hp.y = panelD.found ? panelD.boundingBox.y : std::max(8.0f, inp.screenH - panelH - 70.0f);
    }
    if (hp.pinned)
    {
        const Clay_ElementData td = Clay_GetElementData(titleId);
        if (td.found && inp.lmbPressed && inp.mouseX >= td.boundingBox.x && inp.mouseX <= td.boundingBox.x + td.boundingBox.width &&
            inp.mouseY >= td.boundingBox.y && inp.mouseY <= td.boundingBox.y + td.boundingBox.height && !hovHudPin[which])
        {
            hp.dragging = true;
            hp.grabX = inp.mouseX - hp.x;
            hp.grabY = inp.mouseY - hp.y;
        }
        if (hp.dragging && inp.lmbDown)
        {
            hp.x = inp.mouseX - hp.grabX;
            hp.y = inp.mouseY - hp.grabY;
        }
        if (!inp.lmbDown)
            hp.dragging = false;
        hp.x = std::clamp(hp.x, 4.0f, std::max(4.0f, inp.screenW - panelW - 4.0f)); // a resize can never lose it
        hp.y = std::clamp(hp.y, 4.0f, std::max(4.0f, inp.screenH - panelH - 4.0f));
    }
    else
        hp.dragging = false;
    float px = hp.x, py = hp.y;
    if (!hp.pinned)
    {
        const float bx = btnD.found ? btnD.boundingBox.x + btnD.boundingBox.width : inp.screenW - 40.0f;
        const float by = btnD.found ? btnD.boundingBox.y : inp.screenH - 50.0f;
        px = std::clamp(bx - panelW, 8.0f, std::max(8.0f, inp.screenW - panelW - 8.0f));
        py = std::max(8.0f, by - panelH - 6.0f);
    }

    auto kitStr = [](const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; };
    Clay_FloatingElementConfig fl = {};
    fl.offset = {px, py};
    fl.zIndex = 6;
    fl.attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP};
    fl.attachTo = CLAY_ATTACH_TO_ROOT;

    CLAY(panelId, {.layout = {.sizing = {CLAY_SIZING_FIXED((float)fs(270)), CLAY_SIZING_FIT(0)},
                              .padding = {10, 10, 8, 8},
                              .childGap = 3,
                              .layoutDirection = CLAY_TOP_TO_BOTTOM},
                   .backgroundColor = Pal::panelBg,
                   .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius),
                   .floating = fl})
    {
        // ── Title row: the name (the drag handle once pinned) and the pin, a small rounded button top right ──
        CLAY(titleId, {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                  .padding = {0, 0, 0, 2},
                                  .childGap = 6,
                                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                  .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("HudPopupName", which), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
            {
                CLAY_TEXT(kitStr(which ? tx(Txt::TtlView) : tx(Txt::TtlMovement)),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(13), .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            const float pinSz = (float)std::max(20, (int)fs(18));
            CLAY(CLAY_IDI("HudPopupPin", which), {.layout = {.sizing = {CLAY_SIZING_FIXED(pinSz), CLAY_SIZING_FIXED(pinSz)},
                                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                   .backgroundColor = hp.pinned ? Pal::btnAccent : (hovHudPin[which] ? Pal::btnHover : Pal::btnIdle),
                                                   .cornerRadius = CLAY_CORNER_RADIUS(pinSz * 0.5f)})
            {
                const bool n = Clay_Hovered();
                sndRollover(n, hovHudPin[which]);
                sndClick(n, inp.lmbPressed);
                hovHudPin[which] = n;
                ui.tooltip(inp, n, hp.pinned ? tx(Txt::TipUnpinPanel) : tx(Txt::TipPinPanel), fs(11));
                if (n && inp.lmbPressed)
                {
                    hp.pinned = !hp.pinned;
                    if (hp.pinned) // pin it where it is now
                    {
                        hp.x = px;
                        hp.y = py;
                    }
                }
                CLAY(CLAY_IDI("HudPopupPinIcon", which), {.layout = {.sizing = {CLAY_SIZING_FIXED(12), CLAY_SIZING_FIXED(12)}},
                                                          .image = {.imageData = (void *)(intptr_t)(kIconPinH + 1)}}) {}
            }
        }

        if (which == 0)
        {
            // Reference frame: Earth (the view turns with the ground) / Stars (the inertial lock). "Sun" was considered
            // (CLAUDE.md "HUD popups"): a Sun-fixed frame is the Earth-fixed one turned once a year, and a view locked on
            // the Sun is what Track / Go to / the sky events' Watches already give, so it is not offered.
            static const char *kFrames[2] = {tx(Txt::OptFrameEarth), tx(Txt::OptFrameStars)};
            if (int c = uiChoiceRow(inp, ui, "HudFrame", 0, tx(Txt::LblReferenceFrame), kFrames, 2, inertialActive ? 1 : 0,
                                    tx(Txt::TipFrameMovement));
                c >= 0 && (c == 1) != inertialActive)
            {
                if (c == 1)
                    inertialStart(obsHeightOffset > 100000.0f);
                else
                    inertialStop();
            }
            uiToggleRow(inp, ui, "HudCinematic", 0, tx(Txt::LblCinematicMode), cinematicLatched, tx(Txt::TipCinematicMovement));
            static const char *kGauge[2] = {tx(Txt::NoteMsl), tx(Txt::NoteAgl)};
            if (int c = uiChoiceRow(inp, ui, "HudGauge", 0, tx(Txt::LblAltitudeGauge), kGauge, 2, altModeSeaLevel ? 0 : 1); c >= 0)
                altModeSeaLevel = c == 0;
        }
        else
        {
            // Exposure: a fixed offset in stops on top of the metered one (clouds_v2.exposure_ev), a reset beside it.
            {
                const Clay_ElementId tid = CLAY_ID("HudExposureTrack");
                const float vmin = -3.0f, vmax = 3.0f;
                const Clay_ElementData td = Clay_GetElementData(tid);
                if (td.found)
                {
                    const Clay_BoundingBox bb = td.boundingBox;
                    const bool over = inp.mouseX >= bb.x - 4 && inp.mouseX <= bb.x + bb.width + 4 && inp.mouseY >= bb.y - 5 &&
                                      inp.mouseY <= bb.y + bb.height + 5;
                    if (inp.lmbPressed && over)
                        hudSliderDrag_ = true;
                    if (hudSliderDrag_ && inp.lmbDown && bb.width > 1.0f)
                    {
                        float nv = vmin + std::clamp((inp.mouseX - bb.x) / bb.width, 0.0f, 1.0f) * (vmax - vmin);
                        nv = std::round(nv / 0.05f) * 0.05f;
                        if (std::abs(nv) < 0.025f)
                            nv = 0.0f; // a detent at the metered exposure
                        cv2ExposureEV = std::clamp(nv, vmin, vmax);
                    }
                }
                if (!inp.lmbDown)
                    hudSliderDrag_ = false;
                static char evBuf[16];
                snprintf(evBuf, sizeof(evBuf), "%+.2f", cv2ExposureEV);
                CLAY(CLAY_ID("HudExposureRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT((float)fs(20) + 6.0f)},
                                                            .padding = {2, 2, 2, 2},
                                                            .childGap = 6,
                                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    CLAY_TEXT(kitStr(tx(Txt::LblExposure)), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    CLAY(tid, {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)fs(10))}},
                               .backgroundColor = Pal::trackBg,
                               .cornerRadius = CLAY_CORNER_RADIUS(3)})
                    {
                        // The fill grows from the middle (0 EV) toward the value.
                        const float f0 = (0.0f - vmin) / (vmax - vmin), f1 = std::clamp((cv2ExposureEV - vmin) / (vmax - vmin), 0.0f, 1.0f);
                        const float lo = std::min(f0, f1), hi = std::max(f0, f1);
                        CLAY(CLAY_ID("HudExposureGapL"), {.layout = {.sizing = {CLAY_SIZING_PERCENT(lo), CLAY_SIZING_GROW(0)}}}) {}
                        CLAY(CLAY_ID("HudExposureFill"), {.layout = {.sizing = {CLAY_SIZING_PERCENT(std::max(hi - lo, 0.012f)), CLAY_SIZING_GROW(0)}},
                                                          .backgroundColor = Pal::btnAccent,
                                                          .cornerRadius = CLAY_CORNER_RADIUS(3)}) {}
                    }
                    CLAY(CLAY_ID("HudExposureVal"), {.layout = {.sizing = {CLAY_SIZING_FIXED((float)fs(40)), CLAY_SIZING_FIT(0)},
                                                                .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}})
                    {
                        CLAY_TEXT(kitStr(evBuf), CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                    }
                    const bool dirty = std::abs(cv2ExposureEV - kDefaultExposureEV) > 1e-4f;
                    if (uiButton(inp, ui, "HudExposureReset", 0, tx(Txt::BtnResetShort), tx(Txt::TipResetExposure), false, dirty))
                        cv2ExposureEV = kDefaultExposureEV;
                }
            }
            uiToggleRow(inp, ui, "HudCoronagraph", 0, tx(Txt::LblCoronagraph), inertialOcculter, tx(Txt::TipCoronagraph));
            uiToggleRow(inp, ui, "HudPrecip", 0, tx(Txt::LblPrecipitation), precipParticles, tx(Txt::TipPrecipitationParticles));
        }
    }
    const Clay_ElementData d = Clay_GetElementData(panelId);
    if (d.found)
        ui.addMouseCaptureRect(d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height);
    else
        ui.addMouseCaptureRect(px, py, panelW, panelH);
}
