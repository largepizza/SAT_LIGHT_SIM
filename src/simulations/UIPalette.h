// UIPalette.h — the HUD's colours and panel shape, shared by every SatelliteSim UI file and the boot
// screen (App.cpp). Moved out of SatelliteSimUI.cpp 2026-10-03 so the tutorial could match the rest of
// the sim instead of keeping its own palette.
//
// UI THEMES (2026-10-14): every colour is one row of UI_PALETTE below, with a column per theme —
// Classic (enum Default: near-black, red accent — the look before themes), Navy (blue-black, azure
// accent), Night (night-vision red: dim reds only, so the screen does not spoil dark adaptation) and
// Midnight (Classic's colours on darker, more opaque panels — the default theme since 2026-10-15). The Pal:: names are ordinary globals that
// applyUiTheme() rewrites, so call sites read `Pal::btnAccent` as before. Night also asks the UI
// renderer to map EVERY colour to red (UIRenderer::setColorFilter: icons, images, any literal colour a
// call site still uses), so the Night column only has to order things by brightness. Persisted as
// display.ui_theme (Display tab, "UI theme"). A new colour: add a row with all three columns, and use
// it instead of a literal Clay_Color at the call site, or it will not follow the theme.
#pragma once
#include "clay.h"
#include <cstdint>
#include <cstring>

enum class UiTheme : int
{
    Default = 0,
    Navy = 1,
    Night = 2,
    Midnight = 3, // Default's colours on darker, more opaque near-black panels — THE DEFAULT (2026-10-15)
    Count
};
inline constexpr const char *kUiThemeNames[] = {"Classic", "Navy", "Night red", "Midnight"};
inline constexpr const char *kUiThemeKeys[] = {"classic", "navy", "night", "midnight"}; // settings.json values

#define UIC(r, g, b, a) Clay_Color{r, g, b, a}
// X(name, Default, Navy, Night, Midnight) — Midnight repeats Default except for the backgrounds
#define UI_PALETTE(X)                                                                                          \
    /* Backgrounds */                                                                                          \
    X(panelBg, UIC(8, 8, 9, 210), UIC(4, 6, 14, 215), UIC(3, 0, 0, 215), UIC(0, 0, 0, 225))         /* floating panel */        \
    X(panelBgFade, UIC(8, 8, 9, 180), UIC(4, 6, 14, 185), UIC(3, 0, 0, 185), UIC(0, 0, 0, 195))     /* panel, more clear */     \
    X(panelSolid, UIC(12, 12, 13, 245), UIC(4, 6, 14, 245), UIC(4, 0, 0, 245), UIC(1, 1, 1, 250))   /* settings window */       \
    X(titleBar, UIC(18, 18, 19, 255), UIC(7, 10, 22, 255), UIC(9, 0, 0, 255), UIC(5, 5, 5, 255))    /* title / header strip */  \
    X(sectionHdr, UIC(22, 22, 23, 130), UIC(10, 15, 32, 130), UIC(12, 0, 0, 130), UIC(8, 8, 8, 130))  /* section divider strip */ \
    X(rowEnabled, UIC(45, 10, 10, 180), UIC(8, 22, 52, 180), UIC(22, 0, 0, 180), UIC(45, 10, 10, 180))  /* enabled row */           \
    X(rowDisabled, UIC(16, 16, 17, 160), UIC(5, 8, 17, 160), UIC(7, 0, 0, 160), UIC(4, 4, 4, 160)) /* disabled row */          \
    X(rowHighlight, UIC(35, 30, 8, 180), UIC(38, 32, 8, 180), UIC(48, 0, 0, 180), UIC(35, 30, 8, 180))  /* highlighted row */       \
    X(btnHighlight, UIC(160, 120, 15, 240), UIC(170, 128, 18, 240), UIC(70, 0, 0, 240), UIC(160, 120, 15, 240)) /* HLT (amber) */   \
    X(btnHighlightHv, UIC(110, 85, 10, 230), UIC(120, 90, 12, 230), UIC(50, 0, 0, 230), UIC(110, 85, 10, 230))                    \
    X(listenRow, UIC(50, 10, 10, 185), UIC(10, 24, 58, 185), UIC(30, 0, 0, 185), UIC(50, 10, 10, 185))   /* keybind capture row */   \
    /* Buttons */                                                                                              \
    X(btnIdle, UIC(30, 30, 31, 210), UIC(10, 16, 34, 210), UIC(12, 0, 0, 210), UIC(12, 12, 12, 215))                                 \
    X(btnHover, UIC(52, 52, 54, 230), UIC(20, 30, 60, 230), UIC(26, 0, 0, 230), UIC(26, 26, 27, 230))                                \
    X(btnAccent, UIC(150, 20, 20, 240), UIC(10, 36, 110, 240), UIC(40, 0, 0, 240), UIC(150, 20, 20, 240)) /* ON / active */         \
    X(btnAccentHv, UIC(100, 15, 15, 230), UIC(8, 26, 82, 230), UIC(28, 0, 0, 230), UIC(100, 15, 15, 230))                          \
    X(btnDisabled, UIC(24, 24, 25, 160), UIC(6, 9, 19, 160), UIC(7, 0, 0, 160), UIC(8, 8, 8, 160))                             \
    X(closeBgIdle, UIC(50, 16, 16, 180), UIC(30, 8, 12, 180), UIC(22, 0, 0, 180), UIC(50, 16, 16, 180)) /* [X] */                   \
    X(closeBgHov, UIC(170, 30, 30, 220), UIC(170, 34, 40, 220), UIC(160, 12, 10, 220), UIC(170, 30, 30, 220))                         \
    X(pauseActive, UIC(140, 25, 25, 230), UIC(10, 36, 110, 230), UIC(45, 0, 0, 230), UIC(140, 25, 25, 230))                         \
    X(listenBtn, UIC(120, 18, 18, 220), UIC(10, 30, 95, 220), UIC(40, 0, 0, 220), UIC(120, 18, 18, 220))                            \
    /* Chrome */                                                                                               \
    X(divider, UIC(48, 48, 50, 120), UIC(24, 34, 62, 120), UIC(26, 0, 0, 120), UIC(26, 26, 27, 120))                                 \
    X(windowBorder, UIC(255, 255, 255, 22), UIC(120, 160, 255, 22), UIC(255, 0, 0, 20), UIC(255, 255, 255, 16))                      \
    /* Text */                                                                                                 \
    X(textPrimary, UIC(205, 205, 210, 255), UIC(205, 212, 228, 255), UIC(175, 0, 0, 255), UIC(205, 205, 210, 255))                    \
    X(textDim, UIC(130, 130, 135, 200), UIC(125, 138, 165, 200), UIC(110, 0, 0, 200), UIC(130, 130, 135, 200))                         \
    X(textHint, UIC(72, 72, 76, 160), UIC(68, 80, 108, 160), UIC(66, 0, 0, 160), UIC(72, 72, 76, 160))                               \
    X(textSection, UIC(155, 155, 165, 200), UIC(150, 165, 200, 200), UIC(135, 0, 0, 200), UIC(155, 155, 165, 200))                     \
    X(textCamera, UIC(110, 110, 115, 180), UIC(105, 118, 145, 180), UIC(95, 0, 0, 180), UIC(110, 110, 115, 180))                       \
    X(textBright, UIC(255, 255, 255, 255), UIC(238, 242, 252, 255), UIC(200, 0, 0, 255), UIC(255, 255, 255, 255)) /* on colour */     \
    X(textWarn, UIC(230, 190, 120, 255), UIC(235, 195, 120, 255), UIC(215, 0, 0, 255), UIC(230, 190, 120, 255))   /* amber note */    \
    X(textOk, UIC(120, 220, 140, 255), UIC(120, 220, 160, 255), UIC(165, 0, 0, 255), UIC(120, 220, 140, 255))                         \
    X(volLabel, UIC(185, 185, 195, 220), UIC(180, 190, 212, 220), UIC(155, 0, 0, 220), UIC(185, 185, 195, 220))                       \
    X(volValue, UIC(210, 210, 215, 255), UIC(210, 216, 230, 255), UIC(180, 0, 0, 255), UIC(210, 210, 215, 255))                       \
    X(btnLabel, UIC(210, 210, 215, 255), UIC(210, 216, 230, 255), UIC(180, 0, 0, 255), UIC(210, 210, 215, 255))                       \
    X(listenKey, UIC(255, 85, 85, 255), UIC(90, 160, 255, 255), UIC(230, 0, 0, 255), UIC(255, 85, 85, 255))                        \
    X(keyText, UIC(140, 140, 145, 200), UIC(135, 148, 175, 200), UIC(120, 0, 0, 200), UIC(140, 140, 145, 200))                         \
    /* Text fields, sliders, cards, insets */                                                                  \
    X(fieldBg, UIC(20, 20, 23, 235), UIC(6, 9, 20, 235), UIC(6, 0, 0, 235), UIC(6, 6, 7, 240))                                 \
    X(fieldBgFocus, UIC(6, 6, 8, 255), UIC(2, 3, 8, 255), UIC(2, 0, 0, 255), UIC(0, 0, 0, 255))                                  \
    X(fieldBorder, UIC(48, 48, 52, 255), UIC(20, 30, 58, 255), UIC(30, 0, 0, 255), UIC(28, 28, 30, 255))                             \
    X(fieldBorderHv, UIC(90, 90, 96, 255), UIC(40, 58, 100, 255), UIC(60, 0, 0, 255), UIC(60, 60, 64, 255))                         \
    X(ghostHover, UIC(40, 40, 44, 230), UIC(14, 22, 46, 230), UIC(18, 0, 0, 230), UIC(20, 20, 22, 230)) /* frameless field hover */  \
    X(textSelBg, UIC(120, 30, 30, 255), UIC(14, 40, 100, 255), UIC(45, 0, 0, 255), UIC(120, 30, 30, 255))                            \
    X(caret, UIC(240, 240, 245, 255), UIC(220, 228, 245, 255), UIC(200, 0, 0, 255), UIC(240, 240, 245, 255))                          \
    X(rowStripe, UIC(255, 255, 255, 6), UIC(120, 160, 255, 6), UIC(255, 0, 0, 5), UIC(255, 255, 255, 4))                            \
    X(trackBg, UIC(22, 22, 24, 255), UIC(7, 11, 24, 255), UIC(9, 0, 0, 255), UIC(8, 8, 9, 255))     /* slider track */          \
    X(cardBg, UIC(14, 14, 16, 220), UIC(4, 7, 16, 220), UIC(5, 0, 0, 220), UIC(3, 3, 4, 225))                                   \
    X(cardBgSel, UIC(22, 22, 26, 230), UIC(9, 14, 30, 230), UIC(12, 0, 0, 230), UIC(10, 10, 12, 235))                               \
    X(cardBorder, UIC(40, 40, 44, 255), UIC(16, 24, 48, 255), UIC(24, 0, 0, 255), UIC(24, 24, 26, 255))                              \
    X(insetBg, UIC(4, 4, 5, 255), UIC(1, 2, 5, 255), UIC(1, 0, 0, 255), UIC(0, 0, 0, 255))            /* behind images */         \
    X(plotBg, UIC(12, 14, 18, 255), UIC(3, 5, 12, 255), UIC(3, 0, 0, 255), UIC(2, 2, 3, 255))                                    \
    X(popupBg, UIC(10, 10, 12, 230), UIC(4, 6, 14, 230), UIC(3, 0, 0, 230), UIC(1, 1, 2, 235))        /* HUD strips */            \
    X(popupBorder, UIC(60, 60, 66, 255), UIC(24, 34, 64, 255), UIC(30, 0, 0, 255), UIC(34, 34, 38, 255))                            \
    /* The tutorial's keycaps and outlines: the input a step wants pulses dim -> lit, held is solid */          X(tutHot, UIC(215, 45, 45, 255), UIC(60, 130, 235, 255), UIC(210, 0, 0, 255), UIC(215, 45, 45, 255))                                X(tutHeld, UIC(190, 32, 32, 250), UIC(20, 70, 170, 250), UIC(60, 0, 0, 250), UIC(190, 32, 32, 250))                               X(tutTargetDim, UIC(70, 22, 22, 235), UIC(8, 18, 44, 235), UIC(20, 0, 0, 235), UIC(70, 22, 22, 235))                                X(tutTargetLit, UIC(120, 22, 22, 235), UIC(16, 40, 100, 235), UIC(45, 0, 0, 235), UIC(120, 22, 22, 235))                             /* Toasts */                                                                                               \
    X(toastInfo, UIC(20, 40, 70, 235), UIC(8, 20, 52, 235), UIC(24, 0, 0, 235), UIC(20, 40, 70, 235))                               \
    X(toastOk, UIC(20, 60, 30, 235), UIC(20, 62, 40, 235), UIC(24, 0, 0, 235), UIC(20, 60, 30, 235))                                 \
    X(toastWarn, UIC(70, 45, 12, 235), UIC(72, 48, 14, 235), UIC(40, 0, 0, 235), UIC(70, 45, 12, 235))                               \
    /* Speed indicator */                                                                                      \
    X(speedFwd, UIC(200, 55, 55, 220), UIC(50, 120, 230, 220), UIC(200, 0, 0, 220), UIC(200, 55, 55, 220))                          \
    X(speedRev, UIC(155, 155, 165, 220), UIC(150, 160, 185, 220), UIC(130, 0, 0, 220), UIC(155, 155, 165, 220))                        \
    X(speedPaused, UIC(95, 95, 100, 220), UIC(90, 100, 125, 220), UIC(75, 0, 0, 220), UIC(95, 95, 100, 220))                          \
    /* Selection: the target-lock reticule (amber, distinct from the accent) */                                \
    X(reticule, UIC(255, 205, 60, 230), UIC(255, 205, 60, 230), UIC(230, 0, 0, 230), UIC(255, 205, 60, 230))                         \
    /* View-preset chips over the 3D render (buildViewChip): translucent, with a dark scrim, because a bare */ \
    /* white icon over bright clouds or the limb is invisible. */                                              \
    X(chipIdle, UIC(0, 0, 0, 84), UIC(0, 4, 12, 84), UIC(0, 0, 0, 84), UIC(0, 0, 0, 84))                                         \
    X(chipHover, UIC(0, 0, 0, 150), UIC(0, 4, 12, 150), UIC(0, 0, 0, 150), UIC(0, 0, 0, 150))                                     \
    X(chipOn, UIC(150, 20, 20, 150), UIC(10, 36, 110, 150), UIC(40, 0, 0, 150), UIC(150, 20, 20, 150))                              \
    X(chipOnHover, UIC(150, 20, 20, 205), UIC(10, 36, 110, 205), UIC(40, 0, 0, 205), UIC(150, 20, 20, 205))

namespace Pal
{
#define UI_PAL_DECL(n, d, nv, nt, mb) inline Clay_Color n = mb; // Midnight is the default theme
UI_PALETTE(UI_PAL_DECL)
#undef UI_PAL_DECL
    inline UiTheme current = UiTheme::Midnight;
}

// ── Global styling parameters ─────────────────────────────────────────────────
// Structural theming knobs shared across every window/panel — one place to tune
// the "shape" of the UI, as opposed to Pal's colors above.
namespace Style
{
    // Corner rounding — windows (Settings/Controls) vs the smaller HUD panels.
    constexpr float windowCornerRadius = 2.0f;
    constexpr float panelCornerRadius = 16.0f;

    // Border — drawn only on the real windows; the HUD panels intentionally have none (a border there
    // cut up the panels' text too much). Its colour follows the theme (Pal::windowBorder).
    constexpr uint16_t borderWidthPx = 1;
    inline const Clay_Color &borderColor = Pal::windowBorder;
}

// Rewrites every Pal:: colour for `t`. The caller also sets the UI renderer's colour filter
// (UIRenderer::setColorFilter(t == UiTheme::Night)).
inline void applyUiTheme(UiTheme t)
{
    const int i = (int)t;
#define UI_PAL_SET(n, d, nv, nt, mb) Pal::n = i == 1 ? nv : i == 2 ? nt : i == 3 ? mb : d;
    UI_PALETTE(UI_PAL_SET)
#undef UI_PAL_SET
    Pal::current = (i >= 0 && i < (int)UiTheme::Count) ? t : UiTheme::Midnight;
}

inline UiTheme uiThemeFromKey(const char *k)
{
    for (int i = 0; i < (int)UiTheme::Count; ++i)
        if (std::strcmp(k, kUiThemeKeys[i]) == 0)
            return (UiTheme)i;
    if (std::strcmp(k, "default") == 0) // its key until 2026-10-15, when Midnight became the default
        return UiTheme::Default;
    return UiTheme::Midnight;
}
