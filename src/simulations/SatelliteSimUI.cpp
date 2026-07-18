// SatelliteSimUI.cpp
// All Clay UI construction for SatelliteSim: HUD panels, the tabbed settings window,
// the view-controls reference window, the intro overlay, and settings.json persistence.
// Split out of SatelliteSim.cpp (session-long UI redesign) because buildUI() alone had
// grown to ~25% of that file with zero decomposition. See CLAUDE.md for the panel/window
// inventory and the WindowChrome (drag+resize+pin) primitive this file builds on.
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../AudioSystem.h"
#include "clay.h"

#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <nlohmann/json.hpp>
#include <string>

// Icon index constants (match order passed to ui.loadIcons in buildUI's lazy-load block).
static constexpr int kIconAngleLeft = 0;  // pixel--angle-left.png  → slow down
static constexpr int kIconAngleRight = 1; // pixel--angle-right.png → speed up
// 2 = controller (unused in time controls)
static constexpr int kIconPause = 3;    // pixel--pause.png
static constexpr int kIconPlay = 4;     // pixel--play.png
static constexpr int kIconSettings = 5; // pixel--settings.png

static constexpr const char *kSettingsTabNames[11] = {
    "Constellations", "Sound", "Controls", "Camera",
    "Display", "Photometry", "Clouds", "Ocean", "Terrain", "Aurora", "Attributions"};

// Helper: short display name for a GLFW key code (used in settings window + tooltips).
static const char *keyDisplayName(int key)
{
    switch (key)
    {
    case GLFW_KEY_SPACE:
        return "Space";
    case GLFW_KEY_TAB:
        return "Tab";
    case GLFW_KEY_COMMA:
        return ",";
    case GLFW_KEY_PERIOD:
        return ".";
    case GLFW_KEY_ESCAPE:
        return "Esc";
    case GLFW_KEY_ENTER:
        return "Enter";
    case GLFW_KEY_LEFT_SHIFT:
        return "LShift";
    case GLFW_KEY_RIGHT_SHIFT:
        return "RShift";
    case GLFW_KEY_LEFT_CONTROL:
        return "LCtrl";
    case GLFW_KEY_RIGHT_CONTROL:
        return "RCtrl";
    case GLFW_KEY_LEFT_ALT:
        return "LAlt";
    case GLFW_KEY_RIGHT_ALT:
        return "RAlt";
    case GLFW_KEY_LEFT_SUPER:
        return "LSuper";
    case GLFW_KEY_RIGHT_SUPER:
        return "RSuper";
    case GLFW_KEY_F11:
        return "F11";
    case GLFW_KEY_F1:
        return "F1";
    case GLFW_KEY_F2:
        return "F2";
    case GLFW_KEY_F3:
        return "F3";
    case GLFW_KEY_F4:
        return "F4";
    case GLFW_KEY_F5:
        return "F5";
    case GLFW_KEY_F6:
        return "F6";
    case GLFW_KEY_F7:
        return "F7";
    case GLFW_KEY_F8:
        return "F8";
    case GLFW_KEY_F9:
        return "F9";
    case GLFW_KEY_F10:
        return "F10";
    case GLFW_KEY_F12:
        return "F12";
    case GLFW_KEY_UP:
        return "Up";
    case GLFW_KEY_DOWN:
        return "Down";
    case GLFW_KEY_LEFT:
        return "Left";
    case GLFW_KEY_RIGHT:
        return "Right";
    case GLFW_KEY_PAGE_UP:
        return "PgUp";
    case GLFW_KEY_PAGE_DOWN:
        return "PgDn";
    case GLFW_KEY_HOME:
        return "Home";
    case GLFW_KEY_END:
        return "End";
    case GLFW_KEY_INSERT:
        return "Ins";
    case GLFW_KEY_DELETE:
        return "Del";
    case GLFW_KEY_BACKSPACE:
        return "Bksp";
    case GLFW_KEY_SLASH:
        return "/";
    case GLFW_KEY_BACKSLASH:
        return "\\";
    case GLFW_KEY_SEMICOLON:
        return ";";
    case GLFW_KEY_APOSTROPHE:
        return "'";
    case GLFW_KEY_LEFT_BRACKET:
        return "[";
    case GLFW_KEY_RIGHT_BRACKET:
        return "]";
    case GLFW_KEY_MINUS:
        return "-";
    case GLFW_KEY_EQUAL:
        return "=";
    default:
        if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        {
            static char buf[2] = {};
            buf[0] = (char)('A' + (key - GLFW_KEY_A));
            return buf;
        }
        if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
        {
            static char buf[2] = {};
            buf[0] = (char)('0' + (key - GLFW_KEY_0));
            return buf;
        }
        return "?";
    }
}

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

// ── Small member helpers (declared in SatelliteSim.h) ─────────────────────────
void SatelliteSim::sndRollover(bool nowHov, bool prevHov) const
{
    if (audio_ && nowHov && !prevHov)
        audio_->playSfx("assets/sound/ui/buttonrollover.wav");
}
void SatelliteSim::sndClick(bool nowHov, bool lmbPressed) const
{
    if (audio_ && nowHov && lmbPressed)
        audio_->playSfx("assets/sound/ui/buttonclick.wav");
}

// setLat: moves the observer to a new latitude while preserving longitude direction
// and parallel-transporting obsFacing so it stays tangent after the position jump.
void SatelliteSim::setLat(float newLatDeg)
{
    newLatDeg = glm::clamp(newLatDeg, -90.0f, 90.0f);
    float sinL = sinf(glm::radians(newLatDeg));
    float cosL = cosf(glm::radians(newLatDeg));
    glm::vec2 xy = glm::vec2(obsDir.x, obsDir.y);
    float xyMag = glm::length(xy);
    if (xyMag > 1e-6f)
        xy /= xyMag;
    else
        xy = {1.0f, 0.0f};
    obsDir = {xy.x * cosL, xy.y * cosL, sinL};
    obsFacing = glm::normalize(obsFacing - glm::dot(obsFacing, obsDir) * obsDir);
    obsLatDeg = newLatDeg;
    obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
}

// adjustLon: rotates the observer around Earth's polar (Z) axis by deltaDeg — a
// pure longitude change. Latitude is invariant under a Z-axis rotation (obsDir.z
// untouched), and obsFacing rotates identically so it stays tangent afterward.
void SatelliteSim::adjustLon(float deltaDeg)
{
    float rad = glm::radians(deltaDeg);
    float c = cosf(rad), s = sinf(rad);
    auto rotZ = [&](const glm::vec3 &v)
    { return glm::vec3(v.x * c - v.y * s, v.x * s + v.y * c, v.z); };
    obsDir = rotZ(obsDir);
    obsFacing = rotZ(obsFacing);
    obsLatDeg = glm::degrees(asinf(glm::clamp(obsDir.z, -1.0f, 1.0f)));
    obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
}

// formatAltitude: converts a metres value to the current unit system's display string.
static void formatAltitude(char *buf, size_t bufSize, float meters, UnitSystem unit)
{
    if (unit == UnitSystem::Imperial)
        snprintf(buf, bufSize, "%.2f mi", meters / 1609.344f);
    else
        snprintf(buf, bufSize, "%.2f km", meters / 1000.0f);
}

// ─── buildUI ──────────────────────────────────────────────────────────────────
void SatelliteSim::buildUI(float dt, UIRenderer &ui)
{
    // Apply camera mouse look.
    // Yaw  (dmx): rotate obsFacing around obsDir via Rodrigues — no ENU frame, no pole issue.
    // Pitch (dmy): handled by camera.update → camera.elDeg as usual.
    //
    // Cinematic mode (RMB + ALT held): mouse input adds force to a velocity that
    // drifts and decays, so the camera coasts smoothly after the mouse stops.
    // Releasing ALT instantly zeroes the velocity and returns to direct control.
    if (win)
    {
        // Clear cinematic mode as soon as RMB is released — the toggle only lives
        // while a pan is active, so it resets automatically for the next drag.
        if (!camera.captured && cinematicMode)
        {
            cinematicMode = false;
            cinematicYawVel = 0.0f;
            cinematicPitchVel = 0.0f;
        }

        bool cinematic = camera.captured && cinematicMode;

        if (cinematic)
        {
            // Let camera.update handle RMB capture/release without applying any rotation.
            camera.update(win, 0.0f, 0.0f);

            // Mouse input adds to velocity as an impulse (kForce fraction of raw delta).
            // Velocity units: pixels-equivalent — same as dmx/dmy — so it slots straight
            // into the Rodrigues and elDeg formulas below without any unit conversion.
            const float kForce = 0.06f;
            cinematicYawVel += dmx * kForce;
            cinematicPitchVel += dmy * kForce;

            // Apply velocity this frame (identical math to the direct-control path).
            if (fabsf(cinematicYawVel) > 0.0001f)
            {
                float angle = glm::radians(-cinematicYawVel * camera.sens);
                glm::vec3 leftDir = glm::cross(obsDir, obsFacing);
                obsFacing = glm::normalize(cosf(angle) * obsFacing + sinf(angle) * leftDir);
            }
            camera.elDeg -= cinematicPitchVel * camera.sens;
            camera.elDeg = glm::clamp(camera.elDeg, -89.0f, 89.0f);

            cinematicActive = true;
        }
        else
        {
            // Normal direct control: pitch via camera.update, yaw via Rodrigues.
            camera.update(win, 0.0f, dmy);

            if (camera.captured && dmx != 0.0f)
            {
                // cross(obsDir, obsFacing) is the LEFT tangent, so negate angle for look-right.
                float angle = glm::radians(-dmx * camera.sens);
                glm::vec3 leftDir = glm::cross(obsDir, obsFacing);
                obsFacing = glm::normalize(cosf(angle) * obsFacing + sinf(angle) * leftDir);
            }

            // Kill any residual drift immediately when leaving cinematic mode.
            if (cinematicActive)
            {
                cinematicYawVel = 0.0f;
                cinematicPitchVel = 0.0f;
                cinematicActive = false;
            }
        }

        // Derive camera.azDeg from obsFacing projected into the local Earth-fixed ENU.
        // Only used for the view matrix — never fed back into movement math.
        {
            float sL = obsDir.z;
            float cLH = sqrtf(obsDir.x * obsDir.x + obsDir.y * obsDir.y);
            float inv = (cLH > 1e-7f) ? 1.0f / cLH : 0.0f;
            float cLn = obsDir.x * inv, sLn = obsDir.y * inv;
            glm::vec3 eastEF = {-sLn, cLn, 0.0f};
            glm::vec3 northEF = {-sL * cLn, -sL * sLn, cLH};
            camera.azDeg = glm::degrees(atan2f(
                glm::dot(obsFacing, eastEF),
                glm::dot(obsFacing, northEF)));
        }
    }
    dmx = dmy = 0.0f;

    const UIInput &inp = ui.input();

    // ── Lazy icon loading (first buildUI call after init) ─────────────────────
    if (!iconsLoaded && ctx_)
    {
        const char *iconPaths[] = {
            "assets/icons/ui/pixel--angle-left.png",
            "assets/icons/ui/pixel--angle-right.png",
            "assets/icons/ui/pixel--controller.png",
            "assets/icons/ui/pixel--pause.png",
            "assets/icons/ui/pixel--play.png",
            "assets/icons/ui/pixel--settings.png",
        };
        ui.loadIcons(*ctx_, iconPaths, 6);
        iconsLoaded = true;
    }

    // ── Scroll wheel → FOV zoom (when not hovering over UI panels) ───────────
    if (inp.scrollY != 0.0f && !ui.mouseOverUI())
    {
        camera.fovYDeg = glm::clamp(camera.fovYDeg - inp.scrollY * 3.0f, 10.0f, 120.0f);
    }

    // ── Tab: skip all UI when hidden ─────────────────────────────────────────
    if (!uiVisible)
        return;

    buildLeftHudPanel(inp, ui);
    buildRightHudPanel(inp, ui);
    buildSettingsWindow(inp, ui);
    buildViewControlsWindow(inp, ui);

    // ── Mouse capture rects ───────────────────────────────────────────────────
    // Left/right HUD panels are corner-anchored (CLAY_SIZING_FIT, no stored chrome) —
    // these are rough size estimates for capture purposes only, same approximation
    // the original hardcoded capture rects used.
    ui.addMouseCaptureRect(12.0f, inp.screenH - 90.0f, 320.0f, 78.0f);
    ui.addMouseCaptureRect(inp.screenW - 372.0f, inp.screenH - 50.0f, 360.0f, 38.0f);
    if (settingsChrome.open)
        ui.addMouseCaptureRect(settingsChrome.x, settingsChrome.y, settingsChrome.w, settingsChrome.h);
    if (viewControlsChrome.open)
        ui.addMouseCaptureRect(viewControlsChrome.x, viewControlsChrome.y, viewControlsChrome.w, viewControlsChrome.h);

    buildIntroOverlay(inp, ui);
}

// ─── buildLeftHudPanel ──────────────────────────────────────────────────────
// Time controls: UTC clock, speed label, slower/pause|play/faster/reverse buttons.
// Anchored to the bottom-left corner via Clay attachPoints — recomputed against
// the actual window size every frame, so it stays stuck to the corner across
// resizes instead of a persisted free position (dragging/pinning was tried and
// reverted per user feedback: a fixed-relative-to-window-edge panel is what's
// actually wanted here, not a movable one).
void SatelliteSim::buildLeftHudPanel(const UIInput &inp, UIRenderer &ui)
{
    static char timeBuf[32];
    {
        time_t unixSim = (time_t)(simDayJ2000 * 86400LL + (int64_t)simSecInDay) + 946728000;
        struct tm *utc = gmtime(&unixSim);
        if (utc)
            snprintf(timeBuf, sizeof(timeBuf), "UTC %04d-%02d-%02d %02d:%02d:%02d",
                     utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                     utc->tm_hour, utc->tm_min, utc->tm_sec);
        else
            snprintf(timeBuf, sizeof(timeBuf), "UTC --");
    }
    static char speedBuf[24];
    snprintf(speedBuf, sizeof(speedBuf), "%s%s",
             timeDir < 0.0f ? "REV " : "", kTimeLabels[timeScaleIdx]);

    const float kMargin = 12.0f;
    CLAY(CLAY_ID("LeftPanel"), {.layout = {
                                    .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                    .padding = {10, 10, 8, 8},
                                    .childGap = 6,
                                    .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                .backgroundColor = Pal::panelBg,
                                .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius),
                                .floating = {.offset = {kMargin, -kMargin}, .zIndex = 5, .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_BOTTOM}, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        // UTC time + speed indicator in one row
        CLAY(CLAY_ID("TimeHeaderRow"), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .childGap = 8,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            Clay_String timeStr{false, (int32_t)strlen(timeBuf), timeBuf};
            CLAY_TEXT(timeStr,
                      CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));

            Clay_Color speedCol = timePaused       ? Pal::speedPaused
                                  : timeDir < 0.0f ? Pal::speedRev
                                                   : Pal::speedFwd;
            Clay_String speedStr{false, (int32_t)strlen(speedBuf), speedBuf};
            // Fixed-width box sized for the worst case ("REV " + longest label, e.g.
            // "REV 1mo" = 7 chars) so toggling reverse doesn't change this row's
            // (and therefore the whole FIT-sized panel's) width — it used to grow/
            // shrink the panel every time REV turned on/off.
            CLAY(CLAY_ID("TimeSpeedBox"), {.layout = {.sizing = {CLAY_SIZING_FIXED(7.0f * fs(12) * 0.62f + 10.0f), CLAY_SIZING_FIT(0)}}})
            {
                CLAY_TEXT(speedStr, CLAY_TEXT_CONFIG({.textColor = speedCol, .fontSize = fs(12)}));
            }
        }

        // Icon button row: [◀] [⏸/▶] [▶] [R]
        const int kBtnSize = 28;
        const int kIconSize = 18;
        CLAY(CLAY_ID("TimeBtnRow"), {.layout = {
                                         .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                         .childGap = 5,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            // ── Slow down ─────────────────────────────────────────────────────
            Clay_Color slowBg = hovTimeSlower ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_ID("TimeSlowerBtn"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = slowBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovTimeSlower);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    timeScaleIdx = std::max(0, timeScaleIdx - 1);
                hovTimeSlower = n;
                static char tip[32];
                snprintf(tip, sizeof(tip), "Slow down time (%s)", keyDisplayName(keybindings[KB_SLOWER].key));
                ui.tooltip(inp, n, tip, fs(11));
                CLAY(CLAY_ID("TimeSlowerIcon"), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                                 .image = {.imageData = (void *)(intptr_t)(kIconAngleLeft + 1)}}) {}
            }

            // ── Pause / Play ──────────────────────────────────────────────────
            Clay_Color pauseBg = timePaused
                                     ? Pal::pauseActive
                                     : (hovTimePause ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_ID("TimePauseBtn"), {.layout = {
                                               .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                           .backgroundColor = pauseBg,
                                           .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovTimePause);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    timePaused = !timePaused;
                hovTimePause = n;
                static char tip[32];
                snprintf(tip, sizeof(tip), "%s (%s)", timePaused ? "Play" : "Pause", keyDisplayName(keybindings[KB_PAUSE].key));
                ui.tooltip(inp, n, tip, fs(11));
                int pauseIcon = timePaused ? kIconPlay : kIconPause;
                CLAY(CLAY_ID("TimePauseIcon"), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                                .image = {.imageData = (void *)(intptr_t)(pauseIcon + 1)}}) {}
            }

            // ── Speed up ──────────────────────────────────────────────────────
            Clay_Color fastBg = hovTimeFaster ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_ID("TimeFasterBtn"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = fastBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovTimeFaster);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    timeScaleIdx = std::min(kNumTimeScales - 1, timeScaleIdx + 1);
                hovTimeFaster = n;
                static char tip[32];
                snprintf(tip, sizeof(tip), "Speed up time (%s)", keyDisplayName(keybindings[KB_FASTER].key));
                ui.tooltip(inp, n, tip, fs(11));
                CLAY(CLAY_ID("TimeFasterIcon"), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                                 .image = {.imageData = (void *)(intptr_t)(kIconAngleRight + 1)}}) {}
            }

            // ── Reverse ───────────────────────────────────────────────────────
            // No dedicated icon asset — plain "R" text glyph, styled like the other buttons.
            Clay_Color revBg = timeDir < 0.0f
                                   ? Pal::pauseActive
                                   : (hovTimeReverse ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_ID("TimeReverseBtn"), {.layout = {
                                                 .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                             .backgroundColor = revBg,
                                             .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovTimeReverse);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    toggleTimeDirection();
                hovTimeReverse = n;
                static char tip[32];
                snprintf(tip, sizeof(tip), "Reverse time (%s)", keyDisplayName(keybindings[KB_REVERSE].key));
                ui.tooltip(inp, n, tip, fs(11));
                CLAY_TEXT(CLAY_STRING("R"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(13)}));
            }
        }
    }
}

// ─── buildRightHudPanel ─────────────────────────────────────────────────────
// Lat/lon/altitude/fps + settings gear. Anchored to the bottom-right corner,
// same fixed-to-window-edge approach as the left panel. All three geo fields
// (lat/lon/altitude) support scroll-to-adjust; holding the Move-Fast keybind
// (default LShift) multiplies the step 5x, holding Move-Fine (default LCtrl)
// divides it by 5 — reuses the same boost/fine modifiers WASD movement already
// uses, rather than inventing a separate pair of physical keys.
void SatelliteSim::buildRightHudPanel(const UIInput &inp, UIRenderer &ui)
{
    static char latBuf[20], lonBuf[20], altBuf[24], fpsBuf[24];
    {
        float absLat = fabsf(obsLatDeg);
        float absLon = fabsf(obsLonDeg);
        snprintf(latBuf, sizeof(latBuf), "%.1f\xc2\xb0 %c", absLat, obsLatDeg >= 0.0f ? 'N' : 'S');
        snprintf(lonBuf, sizeof(lonBuf), "%.1f\xc2\xb0 %c", absLon, obsLonDeg >= 0.0f ? 'E' : 'W');
        float altMeters = altModeSeaLevel ? (obsTerrainH + obsHeightOffset) : obsHeightOffset;
        formatAltitude(altBuf, sizeof(altBuf), altMeters, unitSystem);
        snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", inp.dt > 0.0f ? 1.0f / inp.dt : 0.0f);
    }
    Clay_String latStr{false, (int32_t)strlen(latBuf), latBuf};
    Clay_String lonStr{false, (int32_t)strlen(lonBuf), lonBuf};
    Clay_String altStr{false, (int32_t)strlen(altBuf), altBuf};
    Clay_String fpsStr{false, (int32_t)strlen(fpsBuf), fpsBuf};

    // Scroll step modifier — reuses the Move-Fast/Move-Fine keybindings (whatever
    // they're currently bound to) so a rebind stays reflected automatically.
    float scrollMult = 1.0f;
    if (win)
    {
        if (glfwGetKey(win, keybindings[KB_MOVE_BOOST].key) == GLFW_PRESS)
            scrollMult = 5.0f;
        else if (glfwGetKey(win, keybindings[KB_MOVE_FINE].key) == GLFW_PRESS)
            scrollMult = 0.2f;
    }
    static char geoTip[64];
    snprintf(geoTip, sizeof(geoTip), "Scroll to adjust  (%s = fast, %s = fine)",
             keyDisplayName(keybindings[KB_MOVE_BOOST].key), keyDisplayName(keybindings[KB_MOVE_FINE].key));

    const int kGearSz = 28;
    Clay_Color settingsBg = hovSettings ? Pal::btnHover : Pal::panelBgFade;

    CLAY(CLAY_ID("RightPanel"), {.layout = {
                                     .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(38)},
                                     .padding = {10, 10, 6, 6},
                                     .childGap = 7,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                 .backgroundColor = Pal::panelBg,
                                 .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius),
                                 .floating = {.offset = {-12.0f, -12.0f}, .zIndex = 5, .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_BOTTOM, .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM}, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        // ── Lat display (scroll to adjust) ────────────────────────────────
        CLAY(CLAY_ID("SBLatDisplay"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(62), CLAY_SIZING_FIT(0)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
        {
            bool n = Clay_Hovered();
            if (n && inp.scrollY != 0.0f)
                setLat(obsLatDeg + inp.scrollY * 5.0f * scrollMult);
            ui.tooltip(inp, n, geoTip, fs(11));
            CLAY_TEXT(latStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
        }

        CLAY(CLAY_ID("SBDiv2"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                 .backgroundColor = Pal::divider}) {}

        // ── Lon display (scroll to adjust) ────────────────────────────────
        CLAY(CLAY_ID("SBLonDisplay"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(62), CLAY_SIZING_FIT(0)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
        {
            bool n = Clay_Hovered();
            if (n && inp.scrollY != 0.0f)
                adjustLon(inp.scrollY * 5.0f * scrollMult);
            ui.tooltip(inp, n, geoTip, fs(11));
            CLAY_TEXT(lonStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
        }

        CLAY(CLAY_ID("SBDiv3"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                 .backgroundColor = Pal::divider}) {}

        // ── Altitude display (scroll to adjust) + MSL/AGL toggle ──────────
        CLAY(CLAY_ID("SBAltDisplay"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(78), CLAY_SIZING_FIT(0)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
        {
            bool n = Clay_Hovered();
            if (n && inp.scrollY != 0.0f)
            {
                float step = std::max(10.0f, obsHeightOffset * 0.05f) * scrollMult;
                obsHeightOffset = std::max(0.0f, obsHeightOffset + inp.scrollY * step);
            }
            ui.tooltip(inp, n, geoTip, fs(11));
            CLAY_TEXT(altStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
        }
        Clay_Color altBtnBg = hovAltModeToggle ? Pal::btnHover : Pal::btnIdle;
        CLAY(CLAY_ID("SBAltModeBtn"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(34), CLAY_SIZING_FIXED(20)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                       .backgroundColor = altBtnBg,
                                       .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovAltModeToggle);
            sndClick(n, inp.lmbPressed);
            if (n && inp.lmbPressed)
                altModeSeaLevel = !altModeSeaLevel;
            hovAltModeToggle = n;
            ui.tooltip(inp, n, "Toggle sea-level / above-terrain altitude", fs(11));
            CLAY_TEXT(altModeSeaLevel ? CLAY_STRING("MSL") : CLAY_STRING("AGL"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(10)}));
        }

        CLAY(CLAY_ID("SBDiv4"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                 .backgroundColor = Pal::divider}) {}

        // ── FPS ───────────────────────────────────────────────────────────
        CLAY(CLAY_ID("SBFps"), {.layout = {
                                    .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIT(0)},
                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
        {
            CLAY_TEXT(fpsStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));
        }

        CLAY(CLAY_ID("SBDivVersion"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                       .backgroundColor = Pal::divider}) {}

        // ── Version ───────────────────────────────────────────────────────
        CLAY(CLAY_ID("SBVersion"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}}})
        {
            CLAY_TEXT(CLAY_STRING("SAT LIGHT SIM v1.1.0"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
        }

        CLAY(CLAY_ID("SBDiv5"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                 .backgroundColor = Pal::divider}) {}

        // ── Settings gear button ──────────────────────────────────────────
        CLAY(CLAY_ID("SettingsBtn"), {.layout = {
                                          .sizing = {CLAY_SIZING_FIXED(kGearSz), CLAY_SIZING_FIXED(kGearSz)},
                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                      .backgroundColor = settingsBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovSettings);
            sndClick(n, inp.lmbPressed);
            if (n && inp.lmbPressed)
                settingsChrome.open = !settingsChrome.open;
            hovSettings = n;
            ui.tooltip(inp, n, "Open settings", fs(11));
            CLAY(CLAY_ID("SettingsIcon"), {.layout = {.sizing = {CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18)}},
                                           .image = {.imageData = (void *)(intptr_t)(kIconSettings + 1)}}) {}
        }
    }
}

// ─── buildResizableWindow ───────────────────────────────────────────────────
// Shared window frame: title bar (drag + optional close), 8-direction edge/corner
// resize (via UIRenderer::updateWindowChrome), subtle bevel border. `buildBody`
// declares the content — settings' tab strip + content, or view-controls' plain
// scroll list. One implementation instead of two near-duplicate windows.
bool SatelliteSim::buildResizableWindow(const UIInput &inp, UIRenderer &ui, WindowChrome &chrome,
                                        int winId, const char *title, bool closable, bool &hovCloseFlag,
                                        float defaultX, float defaultY,
                                        float minW, float minH, float maxW, float maxH,
                                        const std::function<void()> &buildBody)
{
    if (!chrome.open)
        return false;

    if (chrome.x < 0.0f)
    {
        chrome.x = defaultX;
        chrome.y = defaultY;
    }
    ui.updateWindowChrome(chrome, inp, minW, minH, maxW, maxH);

    bool justClosed = false;
    Clay_String titleStr{false, (int32_t)strlen(title), title};

    CLAY(CLAY_IDI("GenWin", winId), {.layout = {
                                         .sizing = {CLAY_SIZING_FIXED(chrome.w), CLAY_SIZING_FIXED(chrome.h)},
                                         .padding = {0, 0, 0, 0},
                                         .childGap = 0,
                                         .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                     .backgroundColor = Pal::panelSolid,
                                     .cornerRadius = CLAY_CORNER_RADIUS(Style::windowCornerRadius),
                                     .floating = {.offset = {chrome.x, chrome.y}, .zIndex = 10, .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_CAPTURE, .attachTo = CLAY_ATTACH_TO_ROOT},
                                     .border = {.color = Style::borderColor, .width = CLAY_BORDER_ALL(Style::borderWidthPx)}})
    {
        CLAY(CLAY_IDI("GenWinTitleBar", winId), {.layout = {
                                                     .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(36)},
                                                     .padding = {14, 14, 0, 0},
                                                     .childGap = 0,
                                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                 .backgroundColor = Pal::titleBar,
                                                 .cornerRadius = {Style::windowCornerRadius, Style::windowCornerRadius, 0, 0}})
        {
            {
                bool n = Clay_Hovered();
                if (n && inp.lmbPressed && !hovCloseFlag)
                    chrome.dragging = true;
            }

            CLAY_TEXT(titleStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(16)}));

            CLAY(CLAY_IDI("GenWinTitleSpacer", winId), {.layout = {
                                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

            if (closable)
            {
                Clay_Color closeBg = hovCloseFlag ? Pal::closeBgHov : Pal::closeBgIdle;
                CLAY(CLAY_IDI("GenWinCloseBtn", winId), {.layout = {
                                                             .sizing = {CLAY_SIZING_FIXED(24), CLAY_SIZING_FIXED(24)},
                                                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                         .backgroundColor = closeBg,
                                                         .cornerRadius = CLAY_CORNER_RADIUS(4)})
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovCloseFlag);
                    sndClick(n, inp.lmbPressed);
                    hovCloseFlag = n;
                    if (hovCloseFlag && inp.lmbPressed)
                    {
                        chrome.open = false;
                        justClosed = true;
                    }
                    CLAY_TEXT(CLAY_STRING("X"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
                }
            }
        }

        CLAY(CLAY_IDI("GenWinBody", winId), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            buildBody();
        }
    }

    return justClosed;
}

// ─── buildSettingsWindow ────────────────────────────────────────────────────
void SatelliteSim::buildSettingsWindow(const UIInput &inp, UIRenderer &ui)
{
    float defaultX = (inp.screenW - settingsChrome.w) * 0.5f;
    float defaultY = (inp.screenH - settingsChrome.h) * 0.5f;
    // minW=500 = kSliderFixedLeft+kSliderFixedRight+kSliderMinW (413+80, plus a
    // little slack) — the Photometry/Clouds sliders now shrink responsively with
    // the window (settingsSliderWidth()), so the window only needs to stay above
    // the sliders' own floor, not their old fixed 228px width.
    bool justClosed = buildResizableWindow(inp, ui, settingsChrome, 0, "Settings", true, hovSettingsClose,
                                           defaultX, defaultY, 500.0f, 420.0f, 1000.0f, 820.0f,
                                           [&]()
                                           { buildSettingsTabbedBody(inp, ui); });
    if (justClosed)
        saveSettings();
}

// ─── buildSettingsTabbedBody ────────────────────────────────────────────────
// The settings window's body: left tab strip + scrollable content for the active tab.
void SatelliteSim::buildSettingsTabbedBody(const UIInput &inp, UIRenderer &ui)
{
    CLAY(CLAY_ID("SettingsTabStrip"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_GROW(0)},
                                           .padding = {8, 6, 10, 10},
                                           .childGap = 2,
                                           .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        for (int ti = 0; ti < 11; ++ti)
        {
            bool active = settingsActiveTab == ti;
            Clay_Color tabBg = active ? Pal::btnAccent : (hovTab[ti] ? Pal::btnHover : Clay_Color{0, 0, 0, 0});
            CLAY(CLAY_IDI("SettingsTab", ti), {.layout = {
                                                   .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26)},
                                                   .padding = {8, 8, 0, 0},
                                                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                                               .backgroundColor = tabBg,
                                               .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovTab[ti]);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    settingsActiveTab = ti;
                hovTab[ti] = n;
                Clay_String tabStr{false, (int32_t)strlen(kSettingsTabNames[ti]), kSettingsTabNames[ti]};
                CLAY_TEXT(tabStr, CLAY_TEXT_CONFIG({.textColor = active ? Pal::textPrimary : Pal::volLabel, .fontSize = fs(12)}));
            }
        }
    }

    CLAY(CLAY_ID("SettingsTabDivider"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_GROW(0)}},
                                         .backgroundColor = Pal::divider}) {}

    CLAY(CLAY_ID("SettingsContent"), {.layout = {
                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                          .padding = {14, 14, 10, 10},
                                          .childGap = 4,
                                          .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                      .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        switch (settingsActiveTab)
        {
        case 0:
            buildSettingsConstellationsTab(inp, ui);
            break;
        case 1:
            buildSettingsSoundTab(inp, ui);
            break;
        case 2:
            buildSettingsControlsTab(inp, ui);
            break;
        case 3:
            buildSettingsCameraTab(inp, ui);
            break;
        case 4:
            buildSettingsDisplayTab(inp, ui);
            break;
        case 5:
            buildSettingsPhotometryTab(inp, ui);
            break;
        case 6:
            buildSettingsCloudsTab(inp, ui);
            break;
        case 7:
            buildSettingsOceanTab(inp, ui);
            break;
        case 8:
            buildSettingsTerrainTab(inp, ui);
            break;
        case 9:
            buildSettingsAuroraTab(inp, ui);
            break;
        case 10:
            buildSettingsAttributionsTab(inp, ui);
            break;
        }
    }
    ui.scrollbar(CLAY_ID("SettingsContent"));
}

// ─── buildSettingsConstellationsTab ─────────────────────────────────────────
void SatelliteSim::buildSettingsConstellationsTab(const UIInput &inp, UIRenderer &ui)
{
    static char constCntBuf[256][16]; // one slot per constellation; 256 > any realistic mod
    for (int ci = 0; ci < (int)constellations.size() && ci < 256; ++ci)
    {
        ConstellationConfig &c = constellations[ci];
        snprintf(constCntBuf[ci], sizeof(constCntBuf[ci]), "%u", c.orbitCount);

        bool hov = ci < (int)hovConst.size() && hovConst[ci];
        bool hovHlt = ci < (int)hovHighlightConst.size() && hovHighlightConst[ci];
        Clay_Color rowBg = c.highlight ? Pal::rowHighlight
                                       : (c.enabled ? Pal::rowEnabled : Pal::rowDisabled);
        CLAY(CLAY_IDI("ConstRow", ci), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                            .padding = {4, 4, 3, 3},
                                            .childGap = 6,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                        .backgroundColor = rowBg,
                                        .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            // ── ON/OFF toggle ────────────────────────────────────
            Clay_Color btnBg = c.enabled
                                   ? Pal::btnAccent
                                   : (hov ? Pal::btnAccentHv : Pal::btnIdle);
            CLAY(CLAY_IDI("ConstBtn", ci), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(18)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = btnBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hov);
                sndClick(n, inp.lmbPressed);
                if (ci < (int)hovConst.size())
                    hovConst[ci] = n;
                if (hov && inp.lmbPressed)
                    c.enabled = !c.enabled;
                CLAY_TEXT(c.enabled ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(10)}));
            }
            // ── Highlight toggle ─────────────────────────────────
            Clay_Color hltBg = c.highlight
                                   ? Pal::btnHighlight
                                   : (hovHlt ? Pal::btnHighlightHv : Pal::btnIdle);
            CLAY(CLAY_IDI("ConstHltBtn", ci), {.layout = {
                                                   .sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(18)},
                                                   .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                               .backgroundColor = hltBg,
                                               .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovHlt);
                sndClick(n, inp.lmbPressed);
                if (ci < (int)hovHighlightConst.size())
                    hovHighlightConst[ci] = n;
                if (hovHlt && inp.lmbPressed)
                    c.highlight = !c.highlight;
                CLAY_TEXT(CLAY_STRING("HLT"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(10)}));
            }
            CLAY(CLAY_IDI("ConstName", ci), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String nameStr{false, (int32_t)c.name.size(), c.name.data()};
                CLAY_TEXT(nameStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            }
            CLAY(CLAY_IDI("ConstCnt", ci), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(52), CLAY_SIZING_FIT(0)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}})
            {
                Clay_String cntStr{false, (int32_t)strlen(constCntBuf[ci]), constCntBuf[ci]};
                CLAY_TEXT(cntStr, CLAY_TEXT_CONFIG({.textColor = Pal::textCamera, .fontSize = fs(11)}));
            }
        }
    }
}

// ─── buildSettingsSoundTab ──────────────────────────────────────────────────
void SatelliteSim::buildSettingsSoundTab(const UIInput &inp, UIRenderer &ui)
{
    static char volBufs[3][8];
    struct VolRow
    {
        const char *label;
        float vol;
        bool &hMinus;
        bool &hPlus;
        int bufIdx;
    };
    VolRow volRows[] = {
        {"Master vol", audio_ ? audio_->getMasterVolume() : masterVol_, hovMasterVolMinus, hovMasterVolPlus, 0},
        {"Music vol", audio_ ? audio_->getMusicVolume() : musicVol_, hovMusicVolMinus, hovMusicVolPlus, 1},
        {"SFX vol", audio_ ? audio_->getSfxVolume() : sfxVol_, hovSfxVolMinus, hovSfxVolPlus, 2},
    };
    for (auto &vr : volRows)
    {
        snprintf(volBufs[vr.bufIdx], sizeof(volBufs[0]), "%3.0f%%", vr.vol * 100.0f);
        Clay_String volStr{false, (int32_t)strlen(volBufs[vr.bufIdx]), volBufs[vr.bufIdx]};
        CLAY(CLAY_IDI("VolRow", vr.bufIdx), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26)},
                                                 .padding = {4, 4, 2, 2},
                                                 .childGap = 6,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("VolLabel", vr.bufIdx), {.layout = {.sizing = {CLAY_SIZING_FIXED(76), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String lblStr{false, (int32_t)strlen(vr.label), vr.label};
                CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            }
            CLAY(CLAY_IDI("VolSpc", vr.bufIdx), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_Color cMinus = vr.hMinus ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_IDI("VolMinus", vr.bufIdx), {.layout = {
                                                       .sizing = {CLAY_SIZING_FIXED(20), CLAY_SIZING_FIXED(20)},
                                                       .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                   .backgroundColor = cMinus,
                                                   .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, vr.hMinus);
                sndClick(n, inp.lmbPressed);
                vr.hMinus = n;
                if (vr.hMinus && inp.lmbPressed)
                {
                    if (vr.bufIdx == 0 && audio_)
                        audio_->setMasterVolume(audio_->getMasterVolume() - 0.05f);
                    else if (vr.bufIdx == 1 && audio_)
                        audio_->setMusicVolume(audio_->getMusicVolume() - 0.05f);
                    else if (vr.bufIdx == 2 && audio_)
                        audio_->setSfxVolume(audio_->getSfxVolume() - 0.05f);
                }
                CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
            }
            CLAY(CLAY_IDI("VolVal", vr.bufIdx), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(38), CLAY_SIZING_FIT(0)},
                                                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(volStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
            }
            Clay_Color cPlus = vr.hPlus ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_IDI("VolPlus", vr.bufIdx), {.layout = {
                                                      .sizing = {CLAY_SIZING_FIXED(20), CLAY_SIZING_FIXED(20)},
                                                      .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                  .backgroundColor = cPlus,
                                                  .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, vr.hPlus);
                sndClick(n, inp.lmbPressed);
                vr.hPlus = n;
                if (vr.hPlus && inp.lmbPressed)
                {
                    if (vr.bufIdx == 0 && audio_)
                        audio_->setMasterVolume(audio_->getMasterVolume() + 0.05f);
                    else if (vr.bufIdx == 1 && audio_)
                        audio_->setMusicVolume(audio_->getMusicVolume() + 0.05f);
                    else if (vr.bufIdx == 2 && audio_)
                        audio_->setSfxVolume(audio_->getSfxVolume() + 0.05f);
                }
                CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
            }
        }
    }
}

// ─── buildSettingsControlsTab ───────────────────────────────────────────────
void SatelliteSim::buildSettingsControlsTab(const UIInput &inp, UIRenderer &ui)
{
    static char kbKeyBuf[KB_COUNT][16];
    for (int ki = 0; ki < (int)keybindings.size() && ki < KB_COUNT; ++ki)
    {
        KeyBinding &kb = keybindings[ki];
        snprintf(kbKeyBuf[ki], sizeof(kbKeyBuf[ki]), "[%s]", keyDisplayName(kb.key));

        Clay_Color rowBg = kb.listening
                               ? Pal::listenRow
                               : Clay_Color{0, 0, 0, 0};
        CLAY(CLAY_IDI("KbRow", ki), {.layout = {
                                         .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                         .padding = {4, 4, 4, 4},
                                         .childGap = 6,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                     .backgroundColor = rowBg,
                                     .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            CLAY(CLAY_IDI("KbAction", ki), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(130), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String actStr{false, (int32_t)strlen(kb.action), kb.action};
                CLAY_TEXT(actStr,
                          CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
            }

            CLAY(CLAY_IDI("KbKey", ki), {.layout = {
                                             .sizing = {CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String keyStr{false, (int32_t)strlen(kbKeyBuf[ki]), kbKeyBuf[ki]};
                Clay_Color keyCol = kb.listening
                                        ? Pal::listenKey
                                        : Pal::keyText;
                CLAY_TEXT(keyStr,
                          CLAY_TEXT_CONFIG({.textColor = keyCol, .fontSize = fs(13)}));
            }

            Clay_Color rebindBg = kb.listening
                                      ? Pal::listenBtn
                                      : (hovRebind[ki] ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_IDI("KbRebind", ki), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(20)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = rebindBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovRebind[ki]);
                sndClick(n, inp.lmbPressed);
                hovRebind[ki] = n;
                if (hovRebind[ki] && inp.lmbPressed)
                {
                    for (auto &other : keybindings)
                        other.listening = false;
                    kb.listening = true;
                }
                CLAY_TEXT(kb.listening ? CLAY_STRING("PRESS KEY") : CLAY_STRING("Rebind"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(10)}));
            }
        }
    }
}

// ─── buildSettingsCameraTab ─────────────────────────────────────────────────
void SatelliteSim::buildSettingsCameraTab(const UIInput &inp, UIRenderer &ui)
{
    CLAY_TEXT(CLAY_STRING("Right-click drag   Look around"),
              CLAY_TEXT_CONFIG({.textColor = Pal::textCamera, .fontSize = fs(12)}));
    CLAY_TEXT(CLAY_STRING("Scroll wheel        Zoom (FOV)"),
              CLAY_TEXT_CONFIG({.textColor = Pal::textCamera, .fontSize = fs(12)}));
}

// ─── buildSettingsDisplayTab ────────────────────────────────────────────────
void SatelliteSim::buildSettingsDisplayTab(const UIInput &inp, UIRenderer &ui)
{
    // ── UI Scale ──────────────────────────────────────────────────
    CLAY(CLAY_ID("UiScaleRow"), {.layout = {
                                     .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                     .padding = {4, 4, 4, 4},
                                     .childGap = 8,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Text scale"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("UiScaleSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color scaleMinusBg = hovScaleMinus ? Pal::btnHover : Pal::btnIdle;
        CLAY(CLAY_ID("UiScaleMinus"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                       .backgroundColor = scaleMinusBg,
                                       .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovScaleMinus);
            sndClick(n, inp.lmbPressed);
            hovScaleMinus = n;
            if (hovScaleMinus && inp.lmbPressed)
                uiScale = std::max(0.75f, uiScale - 0.125f);
            CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(13)}));
        }

        static char scaleBuf[8];
        snprintf(scaleBuf, sizeof(scaleBuf), "%.2fx", uiScale);
        Clay_String scaleStr{false, (int32_t)strlen(scaleBuf), scaleBuf};
        CLAY(CLAY_ID("UiScaleVal"), {.layout = {
                                         .sizing = {CLAY_SIZING_FIXED(44), CLAY_SIZING_FIT(0)},
                                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
        {
            CLAY_TEXT(scaleStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(13)}));
        }

        Clay_Color scalePlusBg = hovScalePlus ? Pal::btnHover : Pal::btnIdle;
        CLAY(CLAY_ID("UiScalePlus"), {.layout = {
                                          .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                      .backgroundColor = scalePlusBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovScalePlus);
            sndClick(n, inp.lmbPressed);
            hovScalePlus = n;
            if (hovScalePlus && inp.lmbPressed)
                uiScale = std::min(2.0f, uiScale + 0.125f);
            CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(13)}));
        }
    }

    // ── Render scale (resolution scaling, session 29) ──────────────────────
    // Below 100%, the sky/terrain/ocean background renders at reduced resolution and gets
    // upscaled — satellites/stars/UI stay at native resolution always (see SatelliteSim.h's
    // resolution-scaling member comment). A lower-end-hardware fallback, not the default.
    // Placed near the top of Display (not down by the debug/knockout tools below) since this is
    // a real user-facing performance option, not a profiling aid.
    CLAY(CLAY_ID("RenderScaleRow"), {.layout = {
                                         .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                         .padding = {4, 4, 4, 4},
                                         .childGap = 8,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Render scale"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("RenderScaleSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color rsMinusBg = hovRenderScaleMinus ? Pal::btnHover : Pal::btnIdle;
        CLAY(CLAY_ID("RenderScaleMinus"), {.layout = {
                                               .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                           .backgroundColor = rsMinusBg,
                                           .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovRenderScaleMinus);
            sndClick(n, inp.lmbPressed);
            hovRenderScaleMinus = n;
            if (hovRenderScaleMinus && inp.lmbPressed)
            {
                renderScale = std::max(0.5f, renderScale - 0.05f);
                if (ctx_)
                {
                    destroySkyLowResResources(ctx_->device);
                    createSkyLowResResources(*ctx_);
                }
            }
            CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(13)}));
        }

        static char renderScaleBuf[8];
        snprintf(renderScaleBuf, sizeof(renderScaleBuf), "%.0f%%", renderScale * 100.0f);
        Clay_String renderScaleStr{false, (int32_t)strlen(renderScaleBuf), renderScaleBuf};
        CLAY(CLAY_ID("RenderScaleVal"), {.layout = {
                                             .sizing = {CLAY_SIZING_FIXED(44), CLAY_SIZING_FIT(0)},
                                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
        {
            CLAY_TEXT(renderScaleStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(13)}));
        }

        Clay_Color rsPlusBg = hovRenderScalePlus ? Pal::btnHover : Pal::btnIdle;
        CLAY(CLAY_ID("RenderScalePlus"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = rsPlusBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovRenderScalePlus);
            sndClick(n, inp.lmbPressed);
            hovRenderScalePlus = n;
            if (hovRenderScalePlus && inp.lmbPressed)
            {
                renderScale = std::min(1.0f, renderScale + 0.05f);
                if (ctx_)
                {
                    destroySkyLowResResources(ctx_->device);
                    createSkyLowResResources(*ctx_);
                }
            }
            CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(13)}));
        }
    }

    // ── Fullscreen toggle ─────────────────────────────────────────
    bool isFs = win && glfwGetWindowMonitor(win) != nullptr;
    CLAY(CLAY_ID("WinModeRow"), {.layout = {
                                     .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                     .padding = {4, 4, 4, 4},
                                     .childGap = 8,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Window mode"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("WinModeSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color fsBg = isFs ? (hovFullscreen ? Pal::btnAccentHv : Pal::btnAccent)
                               : (hovFullscreen ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("FsToggleBtn"), {.layout = {
                                          .sizing = {CLAY_SIZING_FIXED(92), CLAY_SIZING_FIXED(22)},
                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                      .backgroundColor = fsBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovFullscreen);
            sndClick(n, inp.lmbPressed);
            hovFullscreen = n;
            if (hovFullscreen && inp.lmbPressed && win)
            {
                if (!isFs)
                {
                    glfwGetWindowPos(win, &windowedX, &windowedY);
                    glfwGetWindowSize(win, &windowedW, &windowedH);
                    GLFWmonitor *mon = glfwGetPrimaryMonitor();
                    const GLFWvidmode *mode = glfwGetVideoMode(mon);
                    glfwSetWindowMonitor(win, mon, 0, 0,
                                         mode->width, mode->height, mode->refreshRate);
                }
                else
                {
                    glfwSetWindowMonitor(win, nullptr,
                                         windowedX, windowedY, windowedW, windowedH, 0);
                }
            }
            CLAY_TEXT(isFs ? CLAY_STRING("Windowed") : CLAY_STRING("Fullscreen"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }

    // ── Unit system (metric / imperial) ───────────────────────────
    CLAY(CLAY_ID("UnitSystemRow"), {.layout = {
                                        .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                        .padding = {4, 4, 4, 4},
                                        .childGap = 8,
                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                        .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Units"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("UnitSystemSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        bool isMetric = unitSystem == UnitSystem::Metric;
        Clay_Color metricBg = isMetric ? Pal::btnAccent : (hovUnitMetric ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("UnitMetricBtn"), {.layout = {
                                            .sizing = {CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(22)},
                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                        .backgroundColor = metricBg,
                                        .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovUnitMetric);
            sndClick(n, inp.lmbPressed);
            hovUnitMetric = n;
            if (n && inp.lmbPressed)
                unitSystem = UnitSystem::Metric;
            CLAY_TEXT(CLAY_STRING("Metric"), CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
        bool isImperial = unitSystem == UnitSystem::Imperial;
        Clay_Color imperialBg = isImperial ? Pal::btnAccent : (hovUnitImperial ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("UnitImperialBtn"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(70), CLAY_SIZING_FIXED(22)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = imperialBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovUnitImperial);
            sndClick(n, inp.lmbPressed);
            hovUnitImperial = n;
            if (n && inp.lmbPressed)
                unitSystem = UnitSystem::Imperial;
            CLAY_TEXT(CLAY_STRING("Imperial"), CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }

    // ── Show controls window on startup ────────────────────────────
    CLAY(CLAY_ID("ShowControlsRow"), {.layout = {
                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                          .padding = {4, 4, 4, 4},
                                          .childGap = 8,
                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Show controls window on startup"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("ShowControlsSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color chkBg = showControlsOnStartup ? Pal::btnAccent : (hovShowControlsStartup ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("ShowControlsChk"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = chkBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovShowControlsStartup);
            sndClick(n, inp.lmbPressed);
            hovShowControlsStartup = n;
            if (n && inp.lmbPressed)
                showControlsOnStartup = !showControlsOnStartup;
            CLAY_TEXT(showControlsOnStartup ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }

    // ── GPU frame breakdown (read-only) ────────────────────────────
    // gpuMsSmoothed[]/gpuMsTotalSmoothed are EMA-smoothed GPU timestamp-query
    // results, one frame stale (see the member comments in SatelliteSim.h).
    CLAY(CLAY_ID("PerfDiv"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                         .padding = {0, 0, 6, 4}},
                              .backgroundColor = {30, 30, 32, 255}}) {}
    CLAY_TEXT(CLAY_STRING("GPU FRAME BREAKDOWN"),
              CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(11)}));

    static const char *kPerfLabels[6] = {
        "Cloud march", "Orbit compute", "Flare compute", "Sky background draw", "Satellite + star draw", "UI overlay"};
    static char perfBufs[7][20];
    for (int pi = 0; pi < 6; ++pi)
    {
        snprintf(perfBufs[pi], sizeof(perfBufs[pi]), "%.2f ms", gpuMsSmoothed[pi]);
        Clay_String labelStr{false, (int32_t)strlen(kPerfLabels[pi]), kPerfLabels[pi]};
        Clay_String valStr{false, (int32_t)strlen(perfBufs[pi]), perfBufs[pi]};
        CLAY(CLAY_IDI("PerfRow", pi), {.layout = {
                                           .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                           .padding = {4, 4, 2, 2},
                                           .childGap = 8,
                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                           .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY_TEXT(labelStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY(CLAY_IDI("PerfSpacer", pi), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            CLAY_TEXT(valStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
        }
    }
    snprintf(perfBufs[6], sizeof(perfBufs[6]), "%.2f ms", gpuMsTotalSmoothed);
    Clay_String totalStr{false, (int32_t)strlen(perfBufs[6]), perfBufs[6]};
    CLAY(CLAY_ID("PerfTotalRow"), {.layout = {
                                       .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                       .padding = {4, 4, 2, 2},
                                       .childGap = 8,
                                       .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                       .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("GPU total"), CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY(CLAY_ID("PerfTotalSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
        CLAY_TEXT(totalStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
    }

    // ── Knockout profiling toggles ──────────────────────────────────
    // Each disables one block of sat_sky.frag (see dbgSkip* in the shader). Compare
    // "Sky background draw" above with a toggle on vs. off to read off that block's
    // isolated GPU cost directly, without a GPU capture tool. Bits match SatelliteSim.h's
    // debugDisableMask comment: 1=terrain, 2=atmosphere, 4=sun optical depth, 8=ocean reflection.
    CLAY_TEXT(CLAY_STRING("KNOCKOUT PROFILING (disables rendering correctness for cost isolation)"),
              CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(11)}));
    static const char *kDebugToggleLabels[6] = {
        "Terrain march", "Atmosphere loop (N_VIEW)", "Sun optical depth (N_LIGHT)", "Ocean sky reflection",
        "Airglow red (16-step march)", "Aurora curtain march"};
    static const uint32_t kDebugToggleBits[6] = {1u, 2u, 4u, 8u, 16u, 32u};
    for (int ti = 0; ti < 6; ++ti)
    {
        bool on = (debugDisableMask & kDebugToggleBits[ti]) != 0u;
        Clay_String lblStr{false, (int32_t)strlen(kDebugToggleLabels[ti]), kDebugToggleLabels[ti]};
        CLAY(CLAY_IDI("DebugToggleRow", ti), {.layout = {
                                                  .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26)},
                                                  .padding = {4, 4, 2, 2},
                                                  .childGap = 8,
                                                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                  .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY(CLAY_IDI("DebugToggleSpacer", ti), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_Color chkBg = on ? Pal::btnAccent : (hovDebugToggle[ti] ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_IDI("DebugToggleChk", ti), {.layout = {
                                                      .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                                      .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                  .backgroundColor = chkBg,
                                                  .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovDebugToggle[ti]);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    debugDisableMask ^= kDebugToggleBits[ti];
                hovDebugToggle[ti] = n;
                CLAY_TEXT(on ? CLAY_STRING("SKIP") : CLAY_STRING("ON"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
            }
        }
    }

    // ── Save snapshot ────────────────────────────────────────────
    // Appends the current status + averaged GPU timing above to
    // perf_profiles/profile_log.jsonl next to the exe (see savePerfSnapshot).
    if (snapshotMsgTimer > 0.0f)
        snapshotMsgTimer = std::max(0.0f, snapshotMsgTimer - inp.dt);
    CLAY(CLAY_ID("SaveSnapshotRow"), {.layout = {
                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30)},
                                          .padding = {4, 4, 4, 4},
                                          .childGap = 8,
                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        Clay_Color snapBtnBg = snapshotMsgTimer > 0.0f ? Pal::btnAccent
                               : hovSaveSnapshot       ? Pal::btnHover
                                                       : Pal::btnIdle;
        CLAY(CLAY_ID("SaveSnapshotBtn"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(24)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = snapBtnBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovSaveSnapshot);
            sndClick(n, inp.lmbPressed);
            if (n && inp.lmbPressed)
                savePerfSnapshot(inp.dt);
            hovSaveSnapshot = n;
            ui.tooltip(inp, n, "Append current status + GPU timing to perf_profiles/profile_log.jsonl", fs(11));
            CLAY_TEXT(snapshotMsgTimer > 0.0f ? CLAY_STRING("Saved") : CLAY_STRING("Save Snapshot"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(11)}));
        }
    }
}

// Everything in a Photometry/Clouds slider row except the slider itself is fixed
// width — this is what's left of the window's total width once the tab strip,
// divider, paddings, label, value readout, gaps, and +/- buttons are subtracted.
// Used for both the CLAY_SIZING_FIXED() the slider renders at AND the hit-test
// math, so they can never disagree — the slider now shrinks with the window
// instead of the old fixed 228px (which could extend past a narrow window).
static constexpr float kSliderFixedLeft = 140.0f + 1.0f + 14.0f + 4.0f + 110.0f + 6.0f;               // tab strip+divider+pad+label+gap = 275
static constexpr float kSliderFixedRight = 6.0f + 58.0f + 6.0f + 22.0f + 6.0f + 22.0f + 4.0f + 14.0f; // gap+value+gap+minus+gap+plus+pad = 138
static constexpr float kSliderMinW = 80.0f;
static constexpr float kSliderMaxW = 228.0f;
static float settingsSliderWidth(float chromeW)
{
    return glm::clamp(chromeW - kSliderFixedLeft - kSliderFixedRight, kSliderMinW, kSliderMaxW);
}

// ─── buildSettingsPhotometryTab ─────────────────────────────────────────────
void SatelliteSim::buildSettingsPhotometryTab(const UIInput &inp, UIRenderer &ui)
{
    // Layout constants — must match Clay sizing declarations exactly for slider hit-test.
    // Row: [Label(110)] [Slider(responsive)] [Value(58)] [-(22)] [+(22)]  childGap=6
    const float kSliderAbsX = settingsChrome.x + kSliderFixedLeft;
    const float kSliderW = settingsSliderWidth(settingsChrome.w);

    struct PhotoParam
    {
        const char *label;
        float *val;
        float vmin, vmax, step;
        const char *fmt;
        int idx;
    };
    static char photoBufs[8][12];
    PhotoParam photoParams[] = {
        {"Brightness", &brightnessScale, 0.05f, 20.0f, 0.25f, "%.2f", 0},
        {"Day suppress", &daySuppression, 5.0f, 5000.0f, 5.0f, "%.0f", 1},
        {"Mirror boost", &mirrorBoost, 50.0f, 1000.0f, 25.0f, "%.0f", 2},
        {"Vis threshold", &visThresh, 0.0001f, 0.1f, 0.0001f, "%.3f", 3},
        {"Hlgt flare", &highlightFlare, 0.01f, 1.0f, 0.01f, "%.2f", 4},
        {"Moon suppress", &moonSuppression, 0.0f, 500.0f, 5.0f, "%.0f", 5},
        {"Pollution gain", &lightPollutionGain, 0.0f, 100.0f, 0.1f, "%.2f", 6},
        {"Extinction", &extinctionCoeff, 0.0f, 1.0f, 0.02f, "%.2f", 7},
    };
    for (auto &pp : photoParams)
    {
        int pi = pp.idx;
        snprintf(photoBufs[pi], sizeof(photoBufs[pi]), pp.fmt, *pp.val);
        Clay_String valStr{false, (int32_t)strlen(photoBufs[pi]), photoBufs[pi]};
        float t = glm::clamp((*pp.val - pp.vmin) / (pp.vmax - pp.vmin), 0.0f, 1.0f);

        CLAY(CLAY_IDI("PhotoRow", pi), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                            .padding = {4, 4, 4, 4},
                                            .childGap = 6,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("PhotoLbl", pi), {.layout = {.sizing = {CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String lblStr{false, (int32_t)strlen(pp.label), pp.label};
                CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            }

            CLAY(CLAY_IDI("PhotoSlider", pi), {.layout = {
                                                   .sizing = {CLAY_SIZING_FIXED(kSliderW), CLAY_SIZING_FIXED(16)},
                                                   .childGap = 0,
                                                   .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                               .backgroundColor = {22, 22, 24, 255},
                                               .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool hov = Clay_Hovered();
                if (hov && inp.lmbPressed)
                    draggingPhoto[pi] = true;
                if (!inp.lmbDown)
                    draggingPhoto[pi] = false;
                if (draggingPhoto[pi])
                {
                    float nt = (inp.mouseX - kSliderAbsX) / kSliderW;
                    *pp.val = glm::clamp(pp.vmin + nt * (pp.vmax - pp.vmin), pp.vmin, pp.vmax);
                }
                float fillW = t * kSliderW;
                if (fillW >= 1.0f)
                {
                    CLAY(CLAY_IDI("PhotoFill", pi), {.layout = {.sizing = {CLAY_SIZING_FIXED(fillW), CLAY_SIZING_GROW(0)}},
                                                     .backgroundColor = Pal::btnAccent,
                                                     .cornerRadius = CLAY_CORNER_RADIUS(3)}) {}
                }
            }

            CLAY(CLAY_IDI("PhotoVal", pi), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(58), CLAY_SIZING_FIT(0)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(valStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
            }

            Clay_Color cMinus = hovPhotoMinus[pi] ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_IDI("PhotoMinus", pi), {.layout = {
                                                  .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                  .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                              .backgroundColor = cMinus,
                                              .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovPhotoMinus[pi]);
                sndClick(n, inp.lmbPressed);
                hovPhotoMinus[pi] = n;
                if (hovPhotoMinus[pi] && inp.lmbPressed)
                    *pp.val = glm::clamp(*pp.val - pp.step, pp.vmin, pp.vmax);
                CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
            }

            Clay_Color cPlus = hovPhotoPlus[pi] ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_IDI("PhotoPlus", pi), {.layout = {
                                                 .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                             .backgroundColor = cPlus,
                                             .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovPhotoPlus[pi]);
                sndClick(n, inp.lmbPressed);
                hovPhotoPlus[pi] = n;
                if (hovPhotoPlus[pi] && inp.lmbPressed)
                    *pp.val = glm::clamp(*pp.val + pp.step, pp.vmin, pp.vmax);
                CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
            }
        }
    }
}

// ─── buildCloudSliderRows ────────────────────────────────────────────────────
// Shared row-renderer for the Clouds/Ocean/Terrain/Aurora tabs (split from one combined "Clouds"
// tab, session 28 follow-up #9 — 33 sliders in one list had become unmanageable). `idx` on each
// CloudSlider keeps its ORIGINAL global value (0-32) regardless of which tab it's rendered from,
// so the shared draggingCloud/hovCloudMinus/hovCloudPlus member arrays and the function-local
// static text-buffer array below don't need per-tab remapping.
void SatelliteSim::buildCloudSliderRows(const UIInput &inp, UIRenderer &ui, CloudSlider *sliders, int count)
{
    const float kSliderAbsX = settingsChrome.x + kSliderFixedLeft;
    const float kSliderW = settingsSliderWidth(settingsChrome.w);
    static char cloudBufs[33][16];

    for (int si = 0; si < count; ++si)
    {
        CloudSlider &cs = sliders[si];
        int ci = cs.idx;
        snprintf(cloudBufs[ci], sizeof(cloudBufs[ci]), cs.fmt, *cs.val);
        Clay_String valStr{false, (int32_t)strlen(cloudBufs[ci]), cloudBufs[ci]};
        float t = glm::clamp((*cs.val - cs.vmin) / (cs.vmax - cs.vmin), 0.0f, 1.0f);

        CLAY(CLAY_IDI("CloudRow", ci), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                            .padding = {4, 4, 4, 4},
                                            .childGap = 6,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("CloudLbl", ci), {.layout = {.sizing = {CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String lblStr{false, (int32_t)strlen(cs.label), cs.label};
                CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            }

            CLAY(CLAY_IDI("CloudSlider", ci), {.layout = {
                                                   .sizing = {CLAY_SIZING_FIXED(kSliderW), CLAY_SIZING_FIXED(16)},
                                                   .childGap = 0,
                                                   .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                               .backgroundColor = {22, 22, 24, 255},
                                               .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool hov = Clay_Hovered();
                if (hov && inp.lmbPressed)
                    draggingCloud[ci] = true;
                if (!inp.lmbDown)
                    draggingCloud[ci] = false;
                if (draggingCloud[ci])
                {
                    float nt = (inp.mouseX - kSliderAbsX) / kSliderW;
                    *cs.val = glm::clamp(cs.vmin + nt * (cs.vmax - cs.vmin), cs.vmin, cs.vmax);
                }
                float fillW = t * kSliderW;
                if (fillW >= 1.0f)
                {
                    CLAY(CLAY_IDI("CloudFill", ci), {.layout = {.sizing = {CLAY_SIZING_FIXED(fillW), CLAY_SIZING_GROW(0)}},
                                                     .backgroundColor = Pal::btnAccent,
                                                     .cornerRadius = CLAY_CORNER_RADIUS(3)}) {}
                }
            }

            CLAY(CLAY_IDI("CloudVal", ci), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(58), CLAY_SIZING_FIT(0)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(valStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
            }

            Clay_Color cMinus = hovCloudMinus[ci] ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_IDI("CloudMinus", ci), {.layout = {
                                                  .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                  .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                              .backgroundColor = cMinus,
                                              .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovCloudMinus[ci]);
                sndClick(n, inp.lmbPressed);
                hovCloudMinus[ci] = n;
                if (hovCloudMinus[ci] && inp.lmbPressed)
                    *cs.val = glm::clamp(*cs.val - cs.step, cs.vmin, cs.vmax);
                CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
            }

            Clay_Color cPlus = hovCloudPlus[ci] ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_IDI("CloudPlus", ci), {.layout = {
                                                 .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                             .backgroundColor = cPlus,
                                             .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovCloudPlus[ci]);
                sndClick(n, inp.lmbPressed);
                hovCloudPlus[ci] = n;
                if (hovCloudPlus[ci] && inp.lmbPressed)
                    *cs.val = glm::clamp(*cs.val + cs.step, cs.vmin, cs.vmax);
                CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
            }
        }
    }
}

// ─── buildSettingsCloudsTab ──────────────────────────────────────────────────
void SatelliteSim::buildSettingsCloudsTab(const UIInput &inp, UIRenderer &ui)
{
    CloudSlider sliders[] = {
        {"Coverage", &cloudCoverage, 0.0f, 1.0f, 0.05f, "%.2f", 0},
        {"Density", &cloudDensity, 0.1f, 10.0f, 0.1f, "%.1f", 1},
        {"L0 alt (m)", &cloudBaseAltM, 100.0f, 6000.0f, 100.0f, "%.0f", 2},
        {"L1 alt (m)", &cloudTopAltM, 4000.0f, 15000.0f, 250.0f, "%.0f", 3},
        {"Drift (1e-6)", &cloudDriftRate, 0.0f, 20e-6f, 0.5e-6f, "%.1e", 4},
        {"Sun gain", &cloudSunGain, 0.0f, 5.0f, 0.1f, "%.2f", 5},
        {"Ambient", &cloudAmbientGain, 0.0f, 20.0f, 0.05f, "%.2f", 6},
        {"Night ambient", &cloudNightAmbientGain, 0.0f, 20.0f, 0.05f, "%.2f", 33},
        {"HG g", &cloudHgG, 0.0f, 0.99f, 0.05f, "%.2f", 7},
        {"March steps", &cloudMarchSteps, 4.0f, 1024.0f, 4.0f, "%.0f", 8},
        {"Light steps", &cloudLightSteps, 1.0f, 16.0f, 1.0f, "%.0f", 9},
        {"Cirrus wind (deg)", &cloudCirrusWindDeg, 0.0f, 360.0f, 5.0f, "%.0f", 10},
        {"Cirrus stretch", &cloudCirrusStretch, 1.0f, 10.0f, 0.5f, "%.1f", 11},
        {"Shadow max dist (m)", &cloudShadowMaxDistM, 1000.0f, 60000.0f, 1000.0f, "%.0f", 16},
        {"Render dist (m)", &cloudMaxRenderDistM, 20000.0f, 800000.0f, 10000.0f, "%.0f", 17},
    };
    buildCloudSliderRows(inp, ui, sliders, (int)(sizeof(sliders) / sizeof(sliders[0])));
}

// ─── buildSettingsOceanTab ───────────────────────────────────────────────────
void SatelliteSim::buildSettingsOceanTab(const UIInput &inp, UIRenderer &ui)
{
    CloudSlider sliders[] = {
        {"Sea octaves", &oceanSeaOctaves, 1.0f, 3.0f, 1.0f, "%.0f", 21},
        {"Detail octaves", &oceanDetailOctaves, 1.0f, 5.0f, 1.0f, "%.0f", 22},
        {"Refl samples", &oceanReflSamples, 1.0f, 6.0f, 1.0f, "%.0f", 23},
    };
    buildCloudSliderRows(inp, ui, sliders, (int)(sizeof(sliders) / sizeof(sliders[0])));
}

// ─── buildSettingsTerrainTab ─────────────────────────────────────────────────
// Main atmosphere-loop quality (view/light samples) lives here rather than Clouds or Ocean —
// N_VIEW/N_LIGHT run unconditionally on every pixel (terrain, ocean, cloud, sky alike), but
// terrain/ground-level view is where this quality-vs-perf tradeoff matters most directly.
void SatelliteSim::buildSettingsTerrainTab(const UIInput &inp, UIRenderer &ui)
{
    CloudSlider sliders[] = {
        {"View samples (min)", &viewSamplesMin, 2.0f, 32.0f, 1.0f, "%.0f", 18},
        {"View samples (max)", &viewSamplesMax, 32.0f, 256.0f, 4.0f, "%.0f", 19},
        {"Light samples", &lightSamples, 2.0f, 12.0f, 1.0f, "%.0f", 20},
        {"Moon gain", &moonGain, 0.0f, 0.2f, 0.005f, "%.3f", 24},
    };
    buildCloudSliderRows(inp, ui, sliders, (int)(sizeof(sliders) / sizeof(sliders[0])));
}

// ─── buildSettingsAuroraTab ──────────────────────────────────────────────────
// Airglow + aurora share this tab — both are emissive nightglow phenomena tuned together.
void SatelliteSim::buildSettingsAuroraTab(const UIInput &inp, UIRenderer &ui)
{
    CloudSlider sliders[] = {
        {"Airglow gain", &airglowGain, 0.0f, 5.0f, 0.1f, "%.2f", 12},
        {"Airglow green", &airglowGreenGain, 0.0f, 3.0f, 0.1f, "%.2f", 13},
        {"Airglow red", &airglowRedGain, 0.0f, 3.0f, 0.1f, "%.2f", 14},
        {"Airglow sodium", &airglowSodiumGain, 0.0f, 3.0f, 0.1f, "%.2f", 15},
        {"Storm strength", &stormStrength, 0.0f, 1.0f, 0.05f, "%.2f", 25},
        {"Aurora gain", &auroraGain, 0.0f, 0.1f, 0.001f, "%.3f", 26},
        {"Aurora ground gain", &auroraGroundGain, 0.0f, 0.1f, 0.001f, "%.3f", 27},
        {"Aurora cloud gain", &auroraCloudGain, 0.0f, 0.1f, 0.001f, "%.3f", 28},
        {"Coverage freq", &auroraCoverageFreq, 0.05f, 2.0f, 0.05f, "%.2f", 29},
        {"Coverage az freq", &auroraCoverageAzFreq, 0.0f, 6.0f, 0.1f, "%.1f", 30},
        {"Coverage drift", &auroraCoverageDriftRate, 0.0f, 0.002f, 0.00002f, "%.1e", 31},
        {"Fold shimmer rate", &auroraShimmerRate, 0.0f, 0.2f, 0.002f, "%.3f", 32},
    };
    buildCloudSliderRows(inp, ui, sliders, (int)(sizeof(sliders) / sizeof(sliders[0])));
}

// ─── buildSettingsAttributionsTab ───────────────────────────────────────────
void SatelliteSim::buildSettingsAttributionsTab(const UIInput &inp, UIRenderer &ui)
{
    CLAY(CLAY_ID("Attr0"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Satellite constellation data"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("planet4589.org/space/con/conlist.html"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv0"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr1"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Lens Flare shader (modified)"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("\"Lens Flare Example\" by peterekepeter"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("shadertoy.com/view/4sX3Rs"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv1"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr2"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Icons"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("\"HackerNoon's Pixel Icon Library"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("https://github.com/hackernoon/pixel-icon-library"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
    }
}

// ─── buildViewControlsWindow ─────────────────────────────────────────────────
// Quick-reference list of simulation controls. Shown by default on first run
// (gated by showControlsOnStartup, applied once in init()); closable, but closing
// only lasts for the current run — open state itself is not persisted. Uses the
// same buildResizableWindow frame as the settings window (was a hand-rolled
// near-duplicate before; now one window implementation).
void SatelliteSim::buildViewControlsWindow(const UIInput &inp, UIRenderer &ui)
{
    buildResizableWindow(inp, ui, viewControlsChrome, 1, "Controls", true, hovViewControlsClose,
                         12.0f, 12.0f, 260.0f, 220.0f, 600.0f, 700.0f,
                         [&]()
                         { buildViewControlsBody(inp, ui); });
}

// ─── buildViewControlsBody ──────────────────────────────────────────────────
void SatelliteSim::buildViewControlsBody(const UIInput &inp, UIRenderer &ui)
{
    struct CtrlRow
    {
        const char *label;
        const char *key; // nullptr = look up dynamically from keybindings
        int kbIdx;       // valid only if key == nullptr
    };
    CtrlRow rows[] = {
        {"Move", "WASD", -1},
        {"Look around", "Right-click drag", -1},
        {"Zoom (FOV)", "Scroll wheel", -1},
        {keybindings[KB_MOVE_BOOST].action, nullptr, KB_MOVE_BOOST},
        {keybindings[KB_MOVE_FINE].action, nullptr, KB_MOVE_FINE},
        {keybindings[KB_RAISE_ELEV].action, nullptr, KB_RAISE_ELEV},
        {keybindings[KB_LOWER_ELEV].action, nullptr, KB_LOWER_ELEV},
        {keybindings[KB_RESET_ELEV].action, nullptr, KB_RESET_ELEV},
        {keybindings[KB_CINEMATIC].action, nullptr, KB_CINEMATIC},
        {keybindings[KB_PAUSE].action, nullptr, KB_PAUSE},
        {keybindings[KB_SLOWER].action, nullptr, KB_SLOWER},
        {keybindings[KB_FASTER].action, nullptr, KB_FASTER},
        {keybindings[KB_REVERSE].action, nullptr, KB_REVERSE},
        {keybindings[KB_TOGGLE_UI].action, nullptr, KB_TOGGLE_UI},
    };

    CLAY(CLAY_ID("ViewControlsScroll"), {.layout = {
                                             .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                             .padding = {12, 12, 8, 8},
                                             .childGap = 3,
                                             .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                         .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        static char keyBufs[14][16];
        int idx = 0;
        for (auto &row : rows)
        {
            const char *keyText = row.key;
            if (!keyText)
            {
                snprintf(keyBufs[idx], sizeof(keyBufs[idx]), "%s", keyDisplayName(keybindings[row.kbIdx].key));
                keyText = keyBufs[idx];
            }
            CLAY(CLAY_IDI("CtrlRow", idx), {.layout = {
                                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20)},
                                                .childGap = 6,
                                                .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                Clay_String lblStr{false, (int32_t)strlen(row.label), row.label};
                CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(12)}));
                CLAY(CLAY_IDI("CtrlSpacer", idx), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                Clay_String keyStr{false, (int32_t)strlen(keyText), keyText};
                CLAY_TEXT(keyStr, CLAY_TEXT_CONFIG({.textColor = Pal::keyText, .fontSize = fs(12)}));
            }
            ++idx;
        }
    }
    ui.scrollbar(CLAY_ID("ViewControlsScroll"));
}

// ─── buildIntroOverlay ───────────────────────────────────────────────────────
void SatelliteSim::buildIntroOverlay(const UIInput &inp, UIRenderer &ui)
{
    if (!showIntro)
        return;

    if (inp.lmbPressed || inp.rmbPressed)
        showIntro = false;
    ui.addMouseCaptureRect(0, 0, inp.screenW, inp.screenH);

    CLAY(CLAY_ID("IntroOverlay"), {.layout = {
                                       .sizing = {CLAY_SIZING_FIXED((float)inp.screenW),
                                                  CLAY_SIZING_FIXED((float)inp.screenH)},
                                       .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                          .y = CLAY_ALIGN_Y_CENTER},
                                       .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                   .backgroundColor = {0, 0, 0, 185},
                                   .floating = {.zIndex = 30, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        CLAY(CLAY_ID("IntroPanel"), {.layout = {
                                         .sizing = {CLAY_SIZING_FIXED(660),
                                                    CLAY_SIZING_FIT(0)},
                                         .childGap = 0,
                                         .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            // ── Title ─────────────────────────────────────────────────────
            CLAY_TEXT(CLAY_STRING("SAT LIGHT SIM"),
                      CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, 255},
                                        .fontSize = fs(34)}));
            CLAY_TEXT(CLAY_STRING("by papereater"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));

            CLAY(CLAY_ID("IP1"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1),
                                                        CLAY_SIZING_FIXED(22)}}}) {}

            // ── Body ──────────────────────────────────────────────────────
            CLAY(CLAY_ID("IntroBody"), {.layout = {
                                            .sizing = {CLAY_SIZING_FIXED(660), CLAY_SIZING_FIT(0)},
                                            .childGap = 14,
                                            .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {

                CLAY_TEXT(CLAY_STRING("Welcome to the near future! Every planned major space constellation has been constructed. This simulation aims to realistically model what these will look like from the ground."),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
                CLAY_TEXT(CLAY_STRING("Perpetual sunshine lies in sun synchronous orbit, following the terminator line of the Earth. This has become competitive real estate for football field-sized space datacenters and mirror reflectors"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
                CLAY_TEXT(CLAY_STRING(
                              "Whether or not they are profitable, useful, or even still functional, "
                              "they are going to be up there for a very long time."),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
                CLAY_TEXT(CLAY_STRING("We will come to miss the quiet sky."),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
            }

            CLAY(CLAY_ID("IP2"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1),
                                                        CLAY_SIZING_FIXED(48)}}}) {}

            // ── Controls ──────────────────────────────────────────────────
            CLAY(CLAY_ID("IntroControls"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(660),
                                                           CLAY_SIZING_FIT(0)},
                                                .childGap = 7,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                CLAY_TEXT(CLAY_STRING("WASD  =  Move"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                            .fontSize = fs(13)}));
                CLAY_TEXT(CLAY_STRING("Right click  =  Look"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                            .fontSize = fs(13)}));
                CLAY_TEXT(CLAY_STRING("Shift  =  Boost"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                            .fontSize = fs(13)}));
                CLAY_TEXT(CLAY_STRING("Space  =  Play / Pause"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                            .fontSize = fs(13)}));
                CLAY_TEXT(CLAY_STRING("Comma  =  Slow down time"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                            .fontSize = fs(13)}));
                CLAY_TEXT(CLAY_STRING("Period  =  Speed up time"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                            .fontSize = fs(13)}));
            }

            CLAY(CLAY_ID("IP3"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1),
                                                        CLAY_SIZING_FIXED(32)}}}) {}

            // ── Dismiss hint ──────────────────────────────────────────────
            CLAY_TEXT(CLAY_STRING("Click or press any key to continue"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textHint,
                                        .fontSize = fs(11)}));
        }
    }
}

// ─── loadSettings ─────────────────────────────────────────────────────────────
// Reads settings.json from the exe directory.  Silently skips if the file is
// missing (first run).  Logs a warning and returns on parse error.
// Must be called after initConstellation() so constellations[] is populated.
void SatelliteSim::loadSettings()
{
    auto path = (std::filesystem::path(exeDir_) / "settings.json").string();
    std::ifstream f(path);
    if (!f.is_open())
        return; // first run — silently use defaults

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception &e)
    {
        fprintf(stderr, "[SatelliteSim] Failed to parse settings.json: %s\n", e.what());
        return;
    }

    if (j.contains("photometry"))
    {
        auto &p = j["photometry"];
        brightnessScale = p.value("brightness_scale", brightnessScale);
        daySuppression = p.value("day_suppression", daySuppression);
        mirrorBoost = p.value("mirror_boost", mirrorBoost);
        visThresh = p.value("vis_thresh", visThresh);
        highlightFlare = p.value("highlight_flare", highlightFlare);
        moonSuppression = p.value("moon_suppression", moonSuppression);
        lightPollutionGain = p.value("light_pollution_gain", lightPollutionGain);
        extinctionCoeff = p.value("extinction_coeff", extinctionCoeff);
    }

    if (j.contains("display"))
    {
        auto &d = j["display"];
        uiScale = d.value("ui_scale", uiScale);
        renderScale = d.value("render_scale", renderScale);
        settingsChrome.x = d.value("win_x", settingsChrome.x);
        settingsChrome.y = d.value("win_y", settingsChrome.y);
        settingsChrome.w = d.value("win_w", settingsChrome.w);
        settingsChrome.h = d.value("win_h", settingsChrome.h);
        settingsActiveTab = std::clamp(d.value("active_tab", settingsActiveTab), 0, 10);
        int unitVal = d.value("unit_system", unitSystem == UnitSystem::Imperial ? 1 : 0);
        unitSystem = unitVal == 1 ? UnitSystem::Imperial : UnitSystem::Metric;
        showControlsOnStartup = d.value("show_controls_on_startup", showControlsOnStartup);
    }

    // Left/right HUD panels are corner-anchored, not persisted (see buildLeftHudPanel/
    // buildRightHudPanel) — only the right panel's altitude display mode survives.
    if (j.contains("hud") && j["hud"].contains("right_panel"))
        altModeSeaLevel = j["hud"]["right_panel"].value("alt_mode_sea_level", altModeSeaLevel);

    if (j.contains("audio"))
    {
        auto &a = j["audio"];
        masterVol_ = a.value("master_vol", masterVol_);
        musicVol_ = a.value("music_vol", musicVol_);
        sfxVol_ = a.value("sfx_vol", sfxVol_);
        // audio_ is null here (setAudio not called yet); volumes are applied there.
    }

    if (j.contains("camera"))
    {
        auto &c = j["camera"];
        camera.azDeg = c.value("az_deg", camera.azDeg);
        camera.elDeg = c.value("el_deg", camera.elDeg);
        camera.fovYDeg = c.value("fov_y_deg", camera.fovYDeg);
    }

    if (j.contains("observer"))
    {
        float latDeg = j["observer"].value("lat_deg", obsLatDeg);
        float lonDeg = j["observer"].value("lon_deg", obsLonDeg);
        float lat = glm::radians(latDeg);
        float lon = glm::radians(lonDeg);
        obsDir = {cosf(lat) * cosf(lon), cosf(lat) * sinf(lon), sinf(lat)};
        obsFacing = {-sinf(lat) * cosf(lon), -sinf(lat) * sinf(lon), cosf(lat)};
        obsLatDeg = latDeg;
        obsLonDeg = lonDeg;
    }

    if (j.contains("time"))
    {
        timeScaleIdx = j["time"].value("scale_idx", timeScaleIdx);
        timeScaleIdx = std::clamp(timeScaleIdx, 0, kNumTimeScales - 1);
    }

    if (j.contains("controls") && j["controls"].contains("keybindings"))
    {
        std::unordered_map<std::string, int> actionKey;
        for (const auto &kb : j["controls"]["keybindings"])
            if (kb.contains("action") && kb.contains("key"))
                actionKey[kb["action"].get<std::string>()] = kb["key"].get<int>();
        for (auto &kb : keybindings)
        {
            auto it = actionKey.find(kb.action);
            if (it != actionKey.end())
                kb.key = it->second;
        }
    }

    if (j.contains("constellations"))
    {
        std::unordered_map<std::string, const nlohmann::json *> byName;
        for (const auto &jc : j["constellations"])
            if (jc.contains("name"))
                byName[jc["name"].get<std::string>()] = &jc;
        for (auto &c : constellations)
        {
            auto it = byName.find(c.name);
            if (it != byName.end())
            {
                c.enabled = it->second->value("enabled", c.enabled);
                c.highlight = it->second->value("highlight", c.highlight);
            }
        }
    }

    if (j.contains("clouds"))
    {
        auto &c = j["clouds"];
        cloudCoverage = c.value("coverage", cloudCoverage);
        cloudDensity = c.value("density", cloudDensity);
        cloudBaseAltM = c.value("base_alt_m", cloudBaseAltM);
        cloudTopAltM = c.value("top_alt_m", cloudTopAltM);
        cloudDriftRate = c.value("drift_rate", cloudDriftRate);
        cloudSunGain = c.value("sun_gain", cloudSunGain);
        cloudAmbientGain = c.value("ambient_gain", cloudAmbientGain);
        cloudNightAmbientGain = c.value("night_ambient_gain", cloudNightAmbientGain);
        cloudHgG = c.value("hg_g", cloudHgG);
        cloudMarchSteps = c.value("march_steps", cloudMarchSteps);
        cloudLightSteps = c.value("light_steps", cloudLightSteps);
        cloudCirrusWindDeg = c.value("cirrus_wind_deg", cloudCirrusWindDeg);
        cloudCirrusStretch = c.value("cirrus_stretch", cloudCirrusStretch);
        airglowGain = c.value("airglow_gain", airglowGain);
        airglowGreenGain = c.value("airglow_green_gain", airglowGreenGain);
        airglowRedGain = c.value("airglow_red_gain", airglowRedGain);
        airglowSodiumGain = c.value("airglow_sodium_gain", airglowSodiumGain);
        cloudShadowMaxDistM = c.value("shadow_max_dist_m", cloudShadowMaxDistM);
        cloudMaxRenderDistM = c.value("max_render_dist_m", cloudMaxRenderDistM);
        viewSamplesMin = c.value("view_samples_min", viewSamplesMin);
        viewSamplesMax = c.value("view_samples_max", viewSamplesMax);
        lightSamples = c.value("light_samples", lightSamples);
        oceanSeaOctaves = c.value("ocean_sea_octaves", oceanSeaOctaves);
        oceanDetailOctaves = c.value("ocean_detail_octaves", oceanDetailOctaves);
        oceanReflSamples = c.value("ocean_refl_samples", oceanReflSamples);
        moonGain = c.value("moon_gain", moonGain);
        stormStrength = c.value("storm_strength", stormStrength);
        auroraGain = c.value("aurora_gain", auroraGain);
        auroraGroundGain = c.value("aurora_ground_gain", auroraGroundGain);
        auroraCloudGain = c.value("aurora_cloud_gain", auroraCloudGain);
        auroraCoverageFreq = c.value("aurora_coverage_freq", auroraCoverageFreq);
        auroraCoverageAzFreq = c.value("aurora_coverage_az_freq", auroraCoverageAzFreq);
        auroraCoverageDriftRate = c.value("aurora_coverage_drift_rate", auroraCoverageDriftRate);
        auroraShimmerRate = c.value("aurora_shimmer_rate", auroraShimmerRate);
    }

    fprintf(stderr, "[SatelliteSim] Loaded settings from %s\n", path.c_str());
}

// ─── saveSettings ─────────────────────────────────────────────────────────────
// Writes the current runtime state to settings.json next to the exe.
// Called on cleanup() and when the settings window is closed.
void SatelliteSim::saveSettings()
{
    if (exeDir_.empty())
        return;

    nlohmann::json j;

    j["photometry"] = {
        {"brightness_scale", brightnessScale},
        {"day_suppression", daySuppression},
        {"mirror_boost", mirrorBoost},
        {"vis_thresh", visThresh},
        {"highlight_flare", highlightFlare},
        {"moon_suppression", moonSuppression},
        {"light_pollution_gain", lightPollutionGain},
        {"extinction_coeff", extinctionCoeff}};

    j["display"] = {
        {"ui_scale", uiScale},
        {"render_scale", renderScale},
        {"active_tab", settingsActiveTab},
        {"unit_system", unitSystem == UnitSystem::Imperial ? 1 : 0},
        {"show_controls_on_startup", showControlsOnStartup}};
    if (settingsChrome.x >= 0.0f)
    {
        j["display"]["win_x"] = settingsChrome.x;
        j["display"]["win_y"] = settingsChrome.y;
        j["display"]["win_w"] = settingsChrome.w;
        j["display"]["win_h"] = settingsChrome.h;
    }

    // Left/right HUD panels are corner-anchored, not persisted — only the right
    // panel's altitude display mode survives.
    j["hud"]["right_panel"] = {{"alt_mode_sea_level", altModeSeaLevel}};

    j["audio"] = {
        {"master_vol", audio_ ? audio_->getMasterVolume() : masterVol_},
        {"music_vol", audio_ ? audio_->getMusicVolume() : musicVol_},
        {"sfx_vol", audio_ ? audio_->getSfxVolume() : sfxVol_}};

    j["camera"] = {
        {"az_deg", camera.azDeg},
        {"el_deg", camera.elDeg},
        {"fov_y_deg", camera.fovYDeg}};

    j["observer"] = {{"lat_deg", obsLatDeg}, {"lon_deg", obsLonDeg}};

    j["time"] = {{"scale_idx", timeScaleIdx}};

    j["clouds"] = {
        {"coverage", cloudCoverage},
        {"density", cloudDensity},
        {"base_alt_m", cloudBaseAltM},
        {"top_alt_m", cloudTopAltM},
        {"drift_rate", cloudDriftRate},
        {"sun_gain", cloudSunGain},
        {"ambient_gain", cloudAmbientGain},
        {"night_ambient_gain", cloudNightAmbientGain},
        {"hg_g", cloudHgG},
        {"march_steps", cloudMarchSteps},
        {"light_steps", cloudLightSteps},
        {"cirrus_wind_deg", cloudCirrusWindDeg},
        {"cirrus_stretch", cloudCirrusStretch},
        {"airglow_gain", airglowGain},
        {"airglow_green_gain", airglowGreenGain},
        {"airglow_red_gain", airglowRedGain},
        {"airglow_sodium_gain", airglowSodiumGain},
        {"shadow_max_dist_m", cloudShadowMaxDistM},
        {"max_render_dist_m", cloudMaxRenderDistM},
        {"view_samples_min", viewSamplesMin},
        {"view_samples_max", viewSamplesMax},
        {"light_samples", lightSamples},
        {"ocean_sea_octaves", oceanSeaOctaves},
        {"ocean_detail_octaves", oceanDetailOctaves},
        {"ocean_refl_samples", oceanReflSamples},
        {"moon_gain", moonGain},
        {"storm_strength", stormStrength},
        {"aurora_gain", auroraGain},
        {"aurora_ground_gain", auroraGroundGain},
        {"aurora_cloud_gain", auroraCloudGain},
        {"aurora_coverage_freq", auroraCoverageFreq},
        {"aurora_coverage_az_freq", auroraCoverageAzFreq},
        {"aurora_coverage_drift_rate", auroraCoverageDriftRate},
        {"aurora_shimmer_rate", auroraShimmerRate}};

    nlohmann::json kbArr = nlohmann::json::array();
    for (const auto &kb : keybindings)
        kbArr.push_back({{"action", kb.action}, {"key", kb.key}});
    j["controls"]["keybindings"] = kbArr;

    nlohmann::json constArr = nlohmann::json::array();
    for (const auto &c : constellations)
        constArr.push_back({{"name", c.name}, {"enabled", c.enabled}, {"highlight", c.highlight}});
    j["constellations"] = constArr;

    auto path = (std::filesystem::path(exeDir_) / "settings.json").string();
    try
    {
        std::ofstream f(path);
        f << j.dump(4) << '\n';
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "[SatelliteSim] Failed to save settings.json: %s\n", e.what());
    }
}

// ─── savePerfSnapshot ───────────────────────────────────────────────────────
// Appends one JSON record — system status + the EMA-averaged GPU pass timings
// from gpuMsSmoothed[]/gpuMsTotalSmoothed (see updateGpuTimingStats in
// SatelliteSim.cpp) — to perf_profiles/profile_log.jsonl next to the exe.
// JSON Lines (one object per line) rather than a JSON array so the log can
// grow across sessions/restarts by simple appending and be bulk-loaded later
// (e.g. pandas.read_json(path, lines=True)) without ever re-parsing the whole
// file to add an entry.
void SatelliteSim::savePerfSnapshot(float cpuDt)
{
    if (exeDir_.empty())
        return;

    nlohmann::json j;

    // Host wall-clock capture time — distinct from simulated UTC below — so
    // records can be sorted/deduped by when they were actually taken.
    {
        time_t now = time(nullptr);
        struct tm *utc = gmtime(&now);
        char buf[32];
        if (utc)
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                     utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                     utc->tm_hour, utc->tm_min, utc->tm_sec);
        else
            snprintf(buf, sizeof(buf), "unknown");
        j["captured_at_utc"] = buf;
        j["captured_at_unix"] = (int64_t)now;
    }

    // GPU device name is the main cross-hardware categorization key — without
    // it, snapshots from different users' machines can't be told apart.
    if (ctx_)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx_->physicalDevice, &props);
        j["gpu_device"] = props.deviceName;
        j["resolution"] = {{"width", ctx_->swapExtent.width}, {"height", ctx_->swapExtent.height}};
    }

    j["observer"] = {
        {"lat_deg", obsLatDeg},
        {"lon_deg", obsLonDeg},
        {"height_offset_m", obsHeightOffset},
        {"terrain_h_m", obsTerrainH},
        {"alt_mode", altModeSeaLevel ? "MSL" : "AGL"}};

    j["camera"] = {
        {"az_deg", camera.azDeg},
        {"el_deg", camera.elDeg},
        {"fov_y_deg", camera.fovYDeg}};

    // Simulated UTC — same J2000-epoch conversion as the left HUD clock (buildLeftHudPanel).
    {
        time_t unixSim = (time_t)(simDayJ2000 * 86400LL + (int64_t)simSecInDay) + 946728000;
        struct tm *utc = gmtime(&unixSim);
        char buf[32];
        if (utc)
            snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d",
                     utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                     utc->tm_hour, utc->tm_min, utc->tm_sec);
        else
            snprintf(buf, sizeof(buf), "unknown");
        j["sim_time"] = {
            {"utc", buf},
            {"day_j2000", simDayJ2000},
            {"sec_in_day", simSecInDay}};
    }

    j["time_scale"] = {
        {"idx", timeScaleIdx},
        {"label", kTimeLabels[timeScaleIdx]},
        {"multiplier", kTimeScales[timeScaleIdx]},
        {"paused", timePaused},
        {"reverse", timeDir < 0.0f}};

    j["satellites"] = {
        {"active_count", activeSatCount},
        {"visible_count", visibleCount},
        {"gpu_count", gpuSatCount},
        {"peak_magnitude", peakMagnitude}};

    nlohmann::json enabledConst = nlohmann::json::array();
    for (const auto &c : constellations)
        if (c.enabled)
            enabledConst.push_back(c.name);
    j["constellations_enabled"] = enabledConst;

    // Settings that materially affect GPU cost — needed to tell "this location is
    // slow" apart from "quality was cranked up when this was captured".
    j["quality"] = {
        {"cloud_march_steps", cloudMarchSteps},
        {"cloud_light_steps", cloudLightSteps},
        {"cloud_coverage", cloudCoverage},
        {"view_samples_min", viewSamplesMin},
        {"view_samples_max", viewSamplesMax},
        {"light_samples", lightSamples},
        {"ocean_sea_octaves", oceanSeaOctaves},
        {"ocean_detail_octaves", oceanDetailOctaves},
        {"ocean_refl_samples", oceanReflSamples}};

    // GPU pass breakdown is already EMA-smoothed over recent frames (see the
    // gpuMsSmoothed comments in SatelliteSim.h) — this is an averaged snapshot,
    // not one noisy single-frame sample. CPU frame time is the current frame's
    // raw dt (same source as the HUD fps badge), included for comparison against
    // the GPU total (a gap between them points at CPU-side or present/vsync cost).
    j["gpu_timing_ms"] = {
        {"cloud_march", gpuMsSmoothed[0]},
        {"orbit_compute", gpuMsSmoothed[1]},
        {"flare_compute", gpuMsSmoothed[2]},
        {"sky_background_draw", gpuMsSmoothed[3]},
        {"satellite_star_draw", gpuMsSmoothed[4]},
        {"ui_overlay", gpuMsSmoothed[5]},
        {"total", gpuMsTotalSmoothed}};
    j["cpu_frame"] = {
        {"dt_ms", cpuDt * 1000.0f},
        {"fps", cpuDt > 0.0f ? 1.0f / cpuDt : 0.0f}};
    // Which knockout toggles (if any) were active when this snapshot was taken — a
    // snapshot captured mid-profiling with bits set is only meaningful alongside this.
    j["debug_disable_mask"] = debugDisableMask;

    auto dir = std::filesystem::path(exeDir_) / "perf_profiles";
    auto path = dir / "profile_log.jsonl";
    try
    {
        std::filesystem::create_directories(dir);
        std::ofstream f(path, std::ios::app);
        f << j.dump() << '\n';
        snapshotMsgTimer = 1.5f;
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "[SatelliteSim] Failed to save perf snapshot: %s\n", e.what());
    }
}
