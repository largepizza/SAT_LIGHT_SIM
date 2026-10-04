// UIPalette.h — the HUD's colours and panel shape, shared by every SatelliteSim UI file
// (SatelliteSimUI.cpp, SatelliteSimTutorial.cpp). Moved out of SatelliteSimUI.cpp 2026-10-03 so the
// tutorial could match the rest of the sim instead of keeping its own palette.
#pragma once
#include "clay.h"
#include <cstdint>

// ── UI color palette ──────────────────────────────────────────────────────────
// Edit here to restyle the entire UI. All buildUI colors reference these names.
namespace Pal
{
    // Backgrounds
    constexpr Clay_Color panelBg = {8, 8, 9, 210};            // floating panel
    constexpr Clay_Color panelBgFade = {8, 8, 9, 180};        // panel, slightly transparent
    constexpr Clay_Color panelSolid = {12, 12, 13, 245};      // settings window
    constexpr Clay_Color titleBar = {18, 18, 19, 255};        // title / header strip
    constexpr Clay_Color sectionHdr = {22, 22, 23, 130};      // section divider strip
    constexpr Clay_Color rowEnabled = {45, 10, 10, 180};      // enabled constellation row
    constexpr Clay_Color rowDisabled = {16, 16, 17, 160};     // disabled constellation row
    constexpr Clay_Color rowHighlight = {35, 30, 8, 180};     // highlighted constellation row
    constexpr Clay_Color btnHighlight = {160, 120, 15, 240};  // HLT active (amber)
    constexpr Clay_Color btnHighlightHv = {110, 85, 10, 230}; // HLT hovered
    constexpr Clay_Color listenRow = {50, 10, 10, 185};       // keybind capture row
    // Buttons
    constexpr Clay_Color btnIdle = {30, 30, 31, 210};      // default button
    constexpr Clay_Color btnHover = {52, 52, 54, 230};     // hovered button
    constexpr Clay_Color btnAccent = {150, 20, 20, 240};   // ON / active (red)
    constexpr Clay_Color btnAccentHv = {100, 15, 15, 230}; // accent hovered
    constexpr Clay_Color closeBgIdle = {50, 16, 16, 180};  // [X] idle
    constexpr Clay_Color closeBgHov = {170, 30, 30, 220};  // [X] hovered
    constexpr Clay_Color pauseActive = {140, 25, 25, 230}; // pause btn when paused
    constexpr Clay_Color listenBtn = {120, 18, 18, 220};   // rebind btn while listening
    // Chrome
    constexpr Clay_Color divider = {48, 48, 50, 120}; // separator line
    // Text
    constexpr Clay_Color textPrimary = {205, 205, 210, 255}; // main readable text
    constexpr Clay_Color textDim = {130, 130, 135, 200};     // secondary / dim
    constexpr Clay_Color textHint = {72, 72, 76, 160};       // hint / footer
    constexpr Clay_Color textSection = {155, 155, 165, 200}; // section header labels
    constexpr Clay_Color textCamera = {110, 110, 115, 180};  // dim descriptive text
    constexpr Clay_Color volLabel = {185, 185, 195, 220};    // vol/scale label
    constexpr Clay_Color volValue = {210, 210, 215, 255};    // vol/scale value readout
    constexpr Clay_Color btnLabel = {210, 210, 215, 255};    // text inside +/- buttons
    constexpr Clay_Color listenKey = {255, 85, 85, 255};     // key label while listening
    constexpr Clay_Color keyText = {140, 140, 145, 200};     // normal key label
    // Speed indicator
    constexpr Clay_Color speedFwd = {200, 55, 55, 220};    // forward (red)
    constexpr Clay_Color speedRev = {155, 155, 165, 220};  // reverse (grey)
    constexpr Clay_Color speedPaused = {95, 95, 100, 220}; // paused (dark grey)
    // Selection
    constexpr Clay_Color reticule = {255, 205, 60, 230}; // target-lock reticule (amber — distinct
                                                         // from the red accent used everywhere else)
    // View-preset chips over the 3D render (buildViewChip): translucent, so the image reads through
    // them, with a dark scrim under the idle/hover states because a bare white icon over bright clouds
    // or the Earth's limb is invisible.
    constexpr Clay_Color chipIdle = {0, 0, 0, 84};
    constexpr Clay_Color chipHover = {0, 0, 0, 150};
    constexpr Clay_Color chipOn = {150, 20, 20, 150}; // the accent, translucent
    constexpr Clay_Color chipOnHover = {150, 20, 20, 205};
}

// ── Global styling parameters ─────────────────────────────────────────────────
// Structural theming knobs shared across every window/panel — one place to tune
// the "shape" of the UI, as opposed to Pal's colors above. Added because the
// bevel border used to be redeclared per-element (each with its own hardcoded
// 1px width), so testing a different width meant editing three separate spots
// that could silently drift out of sync.
namespace Style
{
    // Corner rounding ("bevel", in the sense the user actually meant — not a
    // border) — windows (Settings/Controls) are slightly more rounded than the
    // smaller HUD panels. This is the knob to retune for a "more/less rounded"
    // look.
    constexpr float windowCornerRadius = 2.0f;
    constexpr float panelCornerRadius = 16.0f;

    // Border — a separate, purely optional decoration, currently drawn only on
    // the two real windows (Settings, Controls); the HUD panels intentionally
    // have none (a border there cut up the panels' text too much).
    constexpr uint16_t borderWidthPx = 1;
    constexpr Clay_Color borderColor = {255, 255, 255, 22};
}
