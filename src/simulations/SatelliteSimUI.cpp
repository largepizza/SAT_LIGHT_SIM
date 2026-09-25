// SatelliteSimUI.cpp
// All Clay UI construction for SatelliteSim: HUD panels, the tabbed settings window,
// the view-controls reference window, the intro overlay, and settings.json persistence.
// Split out of SatelliteSim.cpp (session-long UI redesign) because buildUI() alone had
// grown to ~25% of that file with zero decomposition. See CLAUDE.md for the panel/window
// inventory and the WindowChrome (drag+resize+pin) primitive this file builds on.
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../AudioSystem.h"
#include "../Log.h"
#include "../Harness.h"
#include "version.h"
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
static constexpr int kIconPause = 3;      // pixel--pause.png
static constexpr int kIconPlay = 4;       // pixel--play.png
static constexpr int kIconSettings = 5;   // pixel--settings.png
static constexpr int kIconCamera = 6;     // camera-solid.png — UC6 screenshot button
static constexpr int kIconStarTrails = 7; // pixel--star-trails.png — long-exposure trail toggle
static constexpr int kIconEye = 8;        // pixel--eye.png — follow mode (fly to the satellite)
static constexpr int kIconSelect = 9;     // pixel--crosshair.png — view/select the satellite's model
static constexpr int kIconTrace = 10;     // pixel--trace.png — "Trace pass" (magnitude over the pass)
static constexpr int kIconInfo = 11;      // pixel--info.png — "Info": the satellite's info window
static constexpr int kIconSpin = 12;      // pixel--spin.png — the free camera's idle rotation
static constexpr int kIconObserver = 13;  // pixel--observer.png — the view from the ground observer
static constexpr int kIconStudio = 14;    // pixel--studio.png — studio lighting (vs the live sky)
static constexpr int kIconMaximize = 15;  // pixel--maximize.png — pop the 3D view out / restore

// The satellite action buttons (the selection panel's, the out-of-view chip's and the info window's)
// are ICON-ONLY: the button's name is the tooltip, never a sentence. `kSelIconBtnMin` matches the view
// window's title-bar icon targets.
static constexpr float kSelIconBtnMin = 24.0f;

// The view-preset chips drawn over a render (buildViewChips): Spin, Observer, Studio, Maximize /
// Restore — the 3D view's own controls. Select and Go to are title-bar icons now (they act on the
// subject, not on the view). The rows live in SatelliteSim::kViewChipRows.
static constexpr int kViewChipCount = 4;

// Settings schema version (NEW-5). Bump this whenever a settings.json change would make an
// old file's graphics-affecting values (photometry/clouds/render_scale) meaningless against
// new code — e.g. re-tuned defaults, or (once UC1 lands) a preset system replacing raw
// sliders. On mismatch, loadSettings() keeps camera/audio/keybindings/observer/constellations
// (those don't go stale the same way) but leaves photometry/clouds/render_scale at their
// compiled-in defaults rather than loading possibly-nonsensical old values.
static constexpr int kSettingsSchemaVersion = 1;

// ── Knockout profiling table ────────────────────────────────────────────────────────────────────
// The single source of truth for the Display tab's knockout checkboxes AND for the automated
// knockout sweep (updateKnockoutSweep / startKnockoutSweep below). Bit values are documented in
// SatelliteSim.h's debugDisableMask comment and CLAUDE.md's "GPU Performance Profiling" subsystem;
// every one has a mathematically-safe zero/no-op fallback, which is what makes them safe both as a
// profiling tool and as the backing store for the shipped graphics presets (applyGraphicsPreset).
//
// json_key is the sweep log's per-step identifier: a stable, greppable name that does NOT change
// when a display label is reworded, so sweeps captured across sessions stay comparable.
//
// The last four (8192-65536) were added 2026-08-10 for the Anchorage worst-case profiling session.
// Each covers a block that had real, suspected-large cost but NO knockout at all, so it was
// permanently invisible inside a lumped timestamp bucket: the beam pointing-ray loop and the
// satellite sky-glow bin loop additionally had no quality slider and no preset reach either, so
// they were paid in full at every tier including Planetarium.
struct DebugToggleEntry
{
    uint32_t bit;
    const char *label;   // Display tab checkbox text
    const char *jsonKey; // stable key in the sweep log
};
static constexpr DebugToggleEntry kDebugToggles[] = {
    {1u, "Terrain march", "terrain_march"},
    {2u, "Atmosphere loop (N_VIEW)", "atmosphere_loop"},
    {4u, "Sun optical depth (N_LIGHT)", "sun_optical_depth"},
    {8u, "Ocean sky reflection", "ocean_sky_reflection"},
    {16u, "Airglow red (16-step march)", "airglow_red"},
    {32u, "Aurora curtain march", "aurora_curtain"},
    {64u, "Cloud self-shadow cone", "cloud_self_shadow_cone"},
    {128u, "Reflect-Orbital beams", "reflect_beams_glow_and_spot"},
    {256u, "Cloud shadow (per-pixel)", "cloud_shadow_per_pixel"},
    {512u, "Beam self-march dispatch", "beam_self_march_dispatch"},
    {1024u, "Scene depth pass", "scene_depth_pass"},
    {2048u, "Fog layer (C11)", "fog_layer"},
    {4096u, "Satellite point cloud occlusion", "sat_point_cloud_occlusion"},
    {8192u, "Beam pointing rays (per-pixel)", "beam_pointing_rays"},
    {16384u, "Cirrus march", "cirrus_march"},
    {32768u, "Volumetric cloud march", "volumetric_cloud_march"},
    {65536u, "Satellite sky-glow bins (64)", "sat_sky_glow_bins"},
    // Not a feature knockout — an OPTIMIZATION knockout. Setting it forces cloud_march.comp's beam
    // pointing-ray loop back to the pre-2026-08-10 full-buffer scan instead of the per-tile culled
    // list. The image must be pixel-identical either way (the fallback recomputes exactly what the
    // cull supplies), so this is both the correctness A/B for the cull and the way to measure what
    // it actually bought: sweep cost_ms here is the SAVING, reported with the opposite sign to
    // every other row.
    {131072u, "Beam tile cull OFF (A/B)", "beam_tile_cull_disabled"},
    // Phase 3b occlusion between satellite parts (sat_orbit.comp, geometry-model types only).
    {1048576u, "Satellite part occlusion", "sat_part_occlusion"},
    // Phase 4c satellite meshes in the main view (SatMeshRenderer scene pass + composite).
    {2097152u, "Satellite meshes", "sat_meshes"},
};
static constexpr int kDebugToggleCount = (int)(sizeof(kDebugToggles) / sizeof(kDebugToggles[0]));
int debugToggleTableSize() { return kDebugToggleCount; }
bool debugToggleAt(int i, uint32_t &bit, const char *&label, const char *&jsonKey)
{
    if (i < 0 || i >= kDebugToggleCount)
        return false;
    bit = kDebugToggles[i].bit;
    label = kDebugToggles[i].label;
    jsonKey = kDebugToggles[i].jsonKey;
    return true;
}
// The matching static_assert against SatelliteSim::kDebugToggleSlots lives inside
// startKnockoutSweep() — that constant is a private member, so only a member function can see it.

// 2026-08-06: "Beams" inserted at 10, shifting Attributions 10 -> 11. Everything that indexes tabs
// by number (hovTab[], the advanced-tab predicate and its bounce, the switch in
// buildSettingsTabbedBody, loadSettings' active_tab clamp) moved with it. The only user-visible
// consequence is that a settings.json saved with active_tab == 10 reopens on Beams rather than
// Attributions once — appending at 11 instead would have avoided that but put the tab button
// underneath Attributions in the strip, which reads as an afterthought.
static constexpr const char *kSettingsTabNames[12] = {
    "Constellations", "Sound", "Controls", "Camera",
    "Display", "Photometry", "Clouds", "Ocean", "Terrain", "Aurora", "Beams", "Attributions"};
int settingsTabIndexByName(const std::string &name)
{
    for (int i = 0; i < 12; ++i)
    {
        const char *t = kSettingsTabNames[i];
        size_t n = strlen(t);
        if (n != name.size())
            continue;
        bool eq = true;
        for (size_t k = 0; k < n && eq; ++k)
            eq = tolower((unsigned char)t[k]) == tolower((unsigned char)name[k]);
        if (eq)
            return i;
    }
    return -1;
}
const char *settingsTabName(int i) { return (i >= 0 && i < 12) ? kSettingsTabNames[i] : ""; }

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
        // Rotating pool, not a single shared buffer: callers that pass two keyDisplayName()
        // results to the same snprintf/format call (e.g. the intro's "Q / E" controls hint)
        // evaluate both arguments before either is read, so a single static buffer let the
        // second call's letter silently clobber the first (shipped as "Q / Q" in the intro).
        if ((key >= GLFW_KEY_A && key <= GLFW_KEY_Z) || (key >= GLFW_KEY_0 && key <= GLFW_KEY_9))
        {
            static char bufs[4][2] = {};
            static int slot = 0;
            char *buf = bufs[slot];
            slot = (slot + 1) % 4;
            buf[0] = (key <= GLFW_KEY_Z) ? (char)('A' + (key - GLFW_KEY_A)) : (char)('0' + (key - GLFW_KEY_0));
            return buf;
        }
        return "?";
    }
}

// Helper: short display name for a GLFW gamepad button code (used in settings window).
// -1 (unbound) is handled by the caller, not here.
static const char *gamepadButtonDisplayName(int button)
{
    switch (button)
    {
    case GLFW_GAMEPAD_BUTTON_A:
        return "A";
    case GLFW_GAMEPAD_BUTTON_B:
        return "B";
    case GLFW_GAMEPAD_BUTTON_X:
        return "X";
    case GLFW_GAMEPAD_BUTTON_Y:
        return "Y";
    case GLFW_GAMEPAD_BUTTON_LEFT_BUMPER:
        return "LB";
    case GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER:
        return "RB";
    case GLFW_GAMEPAD_BUTTON_BACK:
        return "Back";
    case GLFW_GAMEPAD_BUTTON_START:
        return "Start";
    case GLFW_GAMEPAD_BUTTON_GUIDE:
        return "Guide";
    case GLFW_GAMEPAD_BUTTON_LEFT_THUMB:
        return "LS";
    case GLFW_GAMEPAD_BUTTON_RIGHT_THUMB:
        return "RS";
    case GLFW_GAMEPAD_BUTTON_DPAD_UP:
        return "D-Up";
    case GLFW_GAMEPAD_BUTTON_DPAD_RIGHT:
        return "D-Right";
    case GLFW_GAMEPAD_BUTTON_DPAD_DOWN:
        return "D-Down";
    case GLFW_GAMEPAD_BUTTON_DPAD_LEFT:
        return "D-Left";
    default:
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
    // Selection
    constexpr Clay_Color reticule = {255, 205, 60, 230}; // target-lock reticule (amber — distinct
                                                         // from the red accent used everywhere else)
    // View-preset chips over the 3D render (buildViewChip): translucent, so the image reads through
    // them, with a dark scrim under the idle/hover states because a bare white icon over bright clouds
    // or the Earth's limb is invisible.
    constexpr Clay_Color chipIdle = {0, 0, 0, 84};
    constexpr Clay_Color chipHover = {0, 0, 0, 150};
    constexpr Clay_Color chipOn = {150, 20, 20, 150};      // the accent, translucent
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
    // First sim entry point of the frame — publish the previous (complete) frame's CPU bucket
    // timings and clear the accumulator before anything starts filling it again. See
    // beginCpuFrameTiming()'s own comment for why the resulting one-frame staleness is deliberate
    // and matches gpuMsRaw[]'s. Must stay the FIRST statement here: the CpuTimer immediately below
    // writes into the accumulator this call resets.
    beginCpuFrameTiming();
    CpuTimer _tUI(cpuAccumMs[CPU_BUILD_UI]);
    // Automation harness (docs/HARNESS.md): run this frame's commands before anything reads the
    // camera/observer/settings they change. A no-op outside a harness run.
    harnessUi_ = &ui;
    harnessTick();

    // Apply camera mouse look.
    // Yaw  (dmx): rotate obsFacing around obsDir via Rodrigues — no ENU frame, no pole issue.
    // Pitch (dmy): handled by camera.update → camera.elDeg as usual.
    //
    // Cinematic mode (RMB + ALT held): mouse input adds force to a velocity that
    // drifts and decays, so the camera coasts smoothly after the mouse stops.
    // Releasing ALT instantly zeroes the velocity and returns to direct control.
    // UC3: skip during the intro cinematic — updateIntroCinematic() (recordCompute) drives
    // camera.elDeg/obsFacing directly, and letting RMB-look run concurrently would fight it (and
    // would leave the cursor captured/hidden partway through a cutscene the user hasn't consented
    // to control yet). Same early-unlock exception as the WASD/Q-E movement block in
    // recordCompute: once the controls-hint beat is showing, updateIntroCinematic has already
    // stopped forcing the camera (see its controlsLive check), so mouse-look — and this block's
    // own camera.azDeg-from-obsFacing derivation just below, which movement now depends on too —
    // can safely run alongside the (by then static) cinematic hold.
    if (win && (!showIntro || introCaptionIndex >= kIntroControlsIndex))
    {
        // Optional per-axis look inversion (Settings > Controls). Mutate the frame's accumulated
        // deltas once, up front, so every consumer below (cinematic drift + direct control) sees
        // the inverted value; dmx/dmy are zeroed at the end of this block regardless.
        if (invertMouseX)
            dmx = -dmx;
        if (invertMouseY)
            dmy = -dmy;

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
                float angle = glm::radians(-cinematicYawVel * camera.lookSens());
                glm::vec3 leftDir = glm::cross(obsDir, obsFacing);
                obsFacing = glm::normalize(cosf(angle) * obsFacing + sinf(angle) * leftDir);
            }
            camera.elDeg -= cinematicPitchVel * camera.lookSens();
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
                float angle = glm::radians(-dmx * camera.lookSens());
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

        // Gamepad look (right stick): applied unconditionally, independent of the mouse/RMB
        // capture logic above — a controller has no cursor to capture, so it always drives
        // the camera when deflected. Filled by pollGamepad() in the previous recordCompute.
        if (gpLookYawDeg != 0.0f || gpLookPitchDeg != 0.0f)
        {
            const float gpScale = camera.lookSens() / camera.sens; // FOV scaling, as for the mouse
            float angle = glm::radians(-gpLookYawDeg * gpScale);
            glm::vec3 leftDir = glm::cross(obsDir, obsFacing);
            obsFacing = glm::normalize(cosf(angle) * obsFacing + sinf(angle) * leftDir);
            camera.elDeg = glm::clamp(camera.elDeg - gpLookPitchDeg * gpScale, -89.0f, 89.0f);
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

    // UC4/UC5: mouse click/drag/scroll means the player is at the mouse, not the pad. lmbPressed
    // specifically needs the !vCursorActive guard — App.cpp overrides inp.lmbDown/lmbPressed with
    // the gamepad cursor's own A-button click whenever the virtual cursor is active (see
    // virtualCursor()'s contract in Simulation.h), so an unqualified lmbPressed check here treated
    // every gamepad-cursor click as "player switched to mouse," immediately fighting pollGamepad's
    // own (correct) lastInputWasGamepad=true for that same press. That fight was visible as a
    // one-two-frame flicker in the Controls window's key-label order (e.g. "WASD / L stick" vs
    // "L stick / WASD") every time A was pressed with the cursor up. vCursorActive here is exactly
    // the same value App.cpp consulted to decide whether to override lmb this frame, so it
    // correctly distinguishes a real mouse click from a relayed gamepad one.
    if ((inp.lmbPressed && !vCursorActive) || inp.rmbPressed || inp.scrollY != 0.0f || camera.captured)
        lastInputWasGamepad = false;

    // UC3: the cinematic intro hides the entire normal HUD — no left/right panels, no
    // settings/view-controls windows, no satellite/planet picking, no scroll-to-zoom — nothing
    // but buildIntroOverlay's own caption + skip-hint. Checked ahead of uiVisible (and ahead of
    // icon loading/scroll/click handling below) so none of that runs while the intro owns the
    // camera, independent of whether the player has the rest of the UI toggled on or off.
    if (showIntro)
    {
        buildIntroOverlay(inp, ui);
        return;
    }

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
            "assets/icons/ui/camera-solid.png",
            "assets/icons/ui/pixel--star-trails.png",
            "assets/icons/ui/pixel--eye.png",
            "assets/icons/ui/pixel--crosshair.png",
            "assets/icons/ui/pixel--trace.png",
            "assets/icons/ui/pixel--info.png",
            "assets/icons/ui/pixel--spin.png",
            "assets/icons/ui/pixel--observer.png",
            "assets/icons/ui/pixel--studio.png",
            "assets/icons/ui/pixel--maximize.png",
        };
        // The count comes from the list itself — the hand-maintained "bump this when you add an icon"
        // number used to be a silent way to drop the last icon of the array.
        const int wanted = (int)(sizeof(iconPaths) / sizeof(iconPaths[0]));
        const int got = ui.loadIcons(*ctx_, iconPaths, wanted);
        // One line into the startup log (Log::path()): which directory the icons were resolved from,
        // and how many of them loaded. A stale or missing asset otherwise shows up only as a wrong or
        // magenta glyph, with nothing recorded anywhere about where it came from — the exact confusion
        // the CMake runtime-file sync (`sat_sync_runtime_sources`) exists to prevent.
        std::error_code cwdEc;
        const std::string cwd = std::filesystem::current_path(cwdEc).string();
        char iconLog[512];
        snprintf(iconLog, sizeof(iconLog), "UI: %d/%d icons loaded ('%s', cwd '%s')", got, wanted, iconPaths[0],
                 cwd.c_str());
        Log::line(iconLog);
        if (got < wanted)
            fprintf(stderr, "[icons] only %d of %d loaded — see %s\n", got, wanted, Log::path().c_str());
        iconsLoaded = true;
    }

    // ── Scroll wheel → FOV zoom (when not hovering over UI panels) ───────────
    if (inp.scrollY != 0.0f && !ui.mouseOverUI())
    {
        camera.zoomBy(powf(0.93f, inp.scrollY)); // ~5 deg per notch at 70 deg, ~0.04 deg at 0.5 deg
    }

    // ── Left-click → satellite/planet pick/select ─────────────────────────────
    // Scene interaction like camera look/pan, so gated only on UI hover (not on uiVisible) —
    // left mouse button is otherwise unused for scene interaction (RMB drives camera look).
    // Planets get priority on an exact overlap (pickPlanetAt tried first, only 6 candidates) —
    // they're rare and the more interesting selection; mutually exclusive with satellite selection.
    if (inp.lmbPressed && !ui.mouseOverUI())
    {
        int planetHit = pickPlanetAt(inp.mouseX, inp.mouseY, inp.screenW, inp.screenH);
        if (planetHit >= 0)
        {
            if (planetHit != selectedPlanetIndex || selectedSatIndex >= 0)
            {
                selectedPlanetIndex = planetHit;
                selectedSatIndex = -1;
                formatSelectedPlanetInfo();
                if (audio_)
                    audio_->playSfx("assets/sound/ui/buttonclick.wav");
            }
        }
        else
        {
            int hit = pickSatelliteAt(inp.mouseX, inp.mouseY, inp.screenW, inp.screenH);
            if (hit != selectedSatIndex || selectedPlanetIndex >= 0)
            {
                selectedSatIndex = hit;
                selectedPlanetIndex = -1;
                formatSelectedSatInfo();
                if (hit >= 0 && audio_)
                    audio_->playSfx("assets/sound/ui/buttonclick.wav"); // confirmation chirp — only on an actual (re)selection, not on deselect
            }
        }
    }

    buildHarnessConsole(inp, ui); // the ~ console draws even with the HUD hidden

    // ── Tab: skip all UI when hidden ─────────────────────────────────────────
    if (!uiVisible)
        return;

    buildLeftHudPanel(inp, ui);
    buildRightHudPanel(inp, ui);
    buildSettingsWindow(inp, ui);
    buildViewControlsWindow(inp, ui);
    buildTraceWindow(inp, ui);
    updateViewerView(inp, ui); // the satellite windows' shared prologue (target, drag, readouts)
    buildInfoWindow(inp, ui);
    buildViewPopoutWindow(inp, ui);
    buildFollowHud(inp, ui);
    buildSelectedSatPanel(inp, ui);

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
    if (traceChrome.open)
        ui.addMouseCaptureRect(traceChrome.x, traceChrome.y, traceChrome.w, traceChrome.h);
    // The satellite windows: without these a drag on a render also clicked the sky behind the window
    // and re-selected (or deselected) whatever satellite was under it.
    if (infoChrome.open && meshRendererInit)
        ui.addMouseCaptureRect(infoChrome.x, infoChrome.y, infoChrome.w, infoChrome.h);
    if (viewerChrome.open && meshRendererInit)
        ui.addMouseCaptureRect(viewerChrome.x, viewerChrome.y, viewerChrome.w, viewerChrome.h);

    buildCrashRecoveryNotice(dt, inp, ui);
    buildGraphicsAutoNotice(dt, inp, ui);
    buildScreenshotToast(dt, inp, ui);

    // UC4: draw the virtual cursor itself — inp.mouseX/Y IS vCursorX/Y here (App overrode the
    // real mouse position before this frame's ui.beginFrame()), so no separate position plumbing
    // is needed. Drawn last so it's above every other panel.
    //
    // pointerCaptureMode = PASSTHROUGH is load-bearing, not decorative: a floating element with
    // no explicit pointerCaptureMode defaults to CLAY_POINTER_CAPTURE_MODE_CAPTURE, which stops
    // Clay_SetPointerState's root DFS dead as soon as it finds the pointer inside THAT element —
    // and the pointer is *always* inside this 10x10 dot, by construction, since it's drawn
    // exactly at the pointer's own position. Every root below it (every panel, every button)
    // never even gets hit-tested. This is why real hover/click looked totally dead while
    // stationary (the dot's own stale hitbox from last frame permanently overlapped the current
    // test point) but worked in brief "blips" while moving (the one-frame-stale dot briefly
    // lagged behind the live cursor, leaving a gap for the real element underneath to get
    // tested) — not a coordinate or deadzone bug at all. PASSTHROUGH makes the dot purely visual.
    if (vCursorActive)
    {
        Clay_Color dotCol = vCursorClick ? Pal::btnAccent : Clay_Color{255, 255, 255, 230};
        CLAY(CLAY_ID("VirtualCursor"), {.layout = {.sizing = {CLAY_SIZING_FIXED(10), CLAY_SIZING_FIXED(10)}},
                                        .backgroundColor = dotCol,
                                        .cornerRadius = CLAY_CORNER_RADIUS(5),
                                        .floating = {.offset = {inp.mouseX - 5.0f, inp.mouseY - 5.0f},
                                                     .zIndex = 40,
                                                     .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                                     .attachTo = CLAY_ATTACH_TO_ROOT}}) {}
    }
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

            // ── Screenshot (UC6) ─────────────────────────────────────────────────
            bool shotBusy = screenshotEncoding.load() || screenshotCopyPending || screenshotRequested;
            Clay_Color shotBg = shotBusy ? Pal::pauseActive : (hovScreenshot ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_ID("TimeScreenshotBtn"), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                .backgroundColor = shotBg,
                                                .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovScreenshot);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    requestScreenshot();
                hovScreenshot = n;
                static char tip[32];
                snprintf(tip, sizeof(tip), "Screenshot (%s)", keyDisplayName(keybindings[KB_SCREENSHOT].key));
                ui.tooltip(inp, n, tip, fs(11));
                CLAY(CLAY_ID("TimeScreenshotIcon"), {.layout = {
                                                         .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                                     .image = {.imageData = (void *)(intptr_t)(kIconCamera + 1)}}) {}
            }

            // ── Star Trails ───────────────────────────────────────────────────────
            // Single consolidated control: OFF hides the trail immediately (recordDraw()'s
            // composite draw is itself gated on trailEnabled) and ON always starts from a blank
            // buffer (trailClearPending) — there is no separate "clear" affordance anywhere else.
            Clay_Color trailsBg = trailEnabled ? Pal::pauseActive : (hovTrailsBtn ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_ID("TrailsBtn"), {.layout = {
                                            .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                        .backgroundColor = trailsBg,
                                        .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovTrailsBtn);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                {
                    trailEnabled = !trailEnabled;
                    if (trailEnabled)
                        trailClearPending = true; // always start a fresh exposure on enable
                }
                hovTrailsBtn = n;
                static char tip[40];
                snprintf(tip, sizeof(tip), "Star trails (%s)", keyDisplayName(keybindings[KB_TOGGLE_TRAILS].key));
                ui.tooltip(inp, n, tip, fs(11));
                CLAY(CLAY_ID("TrailsIcon"), {.layout = {
                                                 .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                             .image = {.imageData = (void *)(intptr_t)(kIconStarTrails + 1)}}) {}
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
        // inp.dt is the real frame delta (App clamps it only against multi-second hitches), so
        // 1/dt is the true frame rate. EMA-smooth it so a genuinely low rate shows a steady number.
        float instFps = inp.dt > 0.0f ? 1.0f / inp.dt : 0.0f;
        fpsBadgeEma = fpsBadgeEma > 0.0f ? fpsBadgeEma + 0.1f * (instFps - fpsBadgeEma) : instFps;
        snprintf(fpsBuf, sizeof(fpsBuf), "%.0f fps", fpsBadgeEma);
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
        // Built from APP_VERSION (CMake → version.h) so it can't drift from the VERSION file.
        static char versionBuf[48] = {};
        if (!versionBuf[0])
            snprintf(versionBuf, sizeof(versionBuf), "SAT LIGHT SIM v%s", APP_VERSION);
        Clay_String versionStr{false, (int32_t)strlen(versionBuf), versionBuf};
        CLAY(CLAY_ID("SBVersion"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}}})
        {
            CLAY_TEXT(versionStr,
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

// ─── buildSelectedSatPanel ──────────────────────────────────────────────────
// Floating info panel for the satellite OR planet currently selected via left-click (see the
// click handling in buildUI above, and SatelliteSim::pickSatelliteAt/pickPlanetAt +
// formatSelectedSatInfo/formatSelectedPlanetInfo — mutually exclusive, selecting one clears the
// other). Tracks the selection's live screen position every frame by reprojecting its ENU
// direction: for a satellite that's lastPickedSkyDir, the GPU-computed direction mirrored back
// each frame via the tiny copy in recordCompute (one-frame-stale by design, same idiom as
// peakMagnitude, settles within ~2 frames of a fresh click); for a planet it's read straight out
// of planetBuf's own host-mapped memory (no GPU round-trip needed — see pickPlanetAt's comment),
// so it's never stale. When the selection is currently off-screen or below the horizon, the
// floating panel is skipped in favor of a small fixed corner chip, so it isn't silently lost.
//
// TREATMENT (2026-09-24): the panel carries name + type + the magnitude readout and one row of
// icon buttons. The orbital elements it used to list (altitude, inclination, RAAN, period, the
// flare-mitigation power) and the range/phase line live in the 3D view window now — ten text rows
// under the satellite covered the sky it is trying to show. The last line, the GPU-vs-CPU parity
// Δmag, stays: it compares this dispatch's own numbers against the CPU evaluator, which is the one
// thing the view window (a CPU-only evaluator) has no counterpart for.
// Registers an element's mouse-capture rect from its bounds in the last layout, falling back to
// the given estimate on its first frame. Estimates alone let clicks on a panel's lower rows (the
// Trace button) fall through to satellite picking, which deselected before the button could run.
static void captureLaidOut(UIRenderer &ui, Clay_ElementId id, float x, float y, float w, float h)
{
    const Clay_ElementData d = Clay_GetElementData(id);
    if (d.found)
        ui.addMouseCaptureRect(d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height);
    else
        ui.addMouseCaptureRect(x, y, w, h);
}

void SatelliteSim::buildSelectedSatPanel(const UIInput &inp, UIRenderer &ui)
{
    if (selectedSatIndex < 0 && selectedPlanetIndex < 0)
        return;

    bool isPlanet = selectedPlanetIndex >= 0;
    glm::vec3 pickSkyDir;
    float pickFlare;
    const char (*infoLines)[40];
    if (isPlanet)
    {
        const GpuSatVisible *entries = static_cast<const GpuSatVisible *>(planetMapped);
        pickSkyDir = entries[selectedPlanetIndex].skyDir;
        pickFlare = entries[selectedPlanetIndex].flareIntensity;
        infoLines = planetInfoLine;
    }
    else
    {
        // Phase 4c: from the CPU orbit (updateSelectedSkyDir), not the GPU sprite record, which is
        // empty whenever the satellite draws no sprite — in Earth's shadow, or handed over to its mesh.
        pickSkyDir = selSkyDirCpu;
        pickFlare = selAboveEarth ? 1.0f : 0.0f;
        infoLines = selInfoLine;
    }

    float sx = 0.0f, sy = 0.0f;
    bool onScreen = pickFlare > 0.0f &&
                    projectSkyDirToScreen(pickSkyDir, inp.screenW, inp.screenH, sx, sy);

    const float kMargin = 12.0f;
    if (!onScreen)
    {
        static char chipBuf[64];
        snprintf(chipBuf, sizeof(chipBuf), "Selected: %s (out of view)", infoLines[0]);
        Clay_String chipStr{false, (int32_t)strlen(chipBuf), chipBuf};
        CLAY(CLAY_ID("SelSatChip"), {.layout = {
                                         .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                         .padding = {10, 10, 6, 6},
                                         .childGap = 10,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                     .backgroundColor = Pal::panelBgFade,
                                     .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius),
                                     .floating = {.offset = {kMargin, kMargin}, .zIndex = 6, .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP}, .attachTo = CLAY_ATTACH_TO_ROOT}})
        {
            CLAY_TEXT(chipStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));
            if (!isPlanet)
            {
                buildInfoButton(inp, ui, 0);
                buildFollowButton(inp, ui, 0);
                buildTraceButton(inp, ui, 0);
            }
        }
        captureLaidOut(ui, CLAY_ID("SelSatChip"), kMargin, kMargin, 360.0f, 34.0f);
        return;
    }

    // ── Target reticule: 4 fixed-size corner brackets around the satellite ────────────────
    // Purely decorative (no capture rect) — Clay's default per-floating-element pointer capture
    // doesn't feed our own manual mouseOverUI() hit-testing (see CLAUDE.md's Manual Hit-Testing
    // note), so this can't accidentally swallow clicks meant for scene interaction.
    const float kBoxSize = 6.0f;
    const float kBorderW = 2.0f;
    const float kRadius = 10.0f;
    struct ReticuleCorner
    {
        float ox, oy; // top-left offset of this corner's box, relative to (sx, sy)
        Clay_BorderWidth border;
    };
    const ReticuleCorner corners[4] = {
        {-kRadius, -kRadius, {(uint16_t)kBorderW, 0, (uint16_t)kBorderW, 0, 0}},                     // TL
        {kRadius - kBoxSize, -kRadius, {0, (uint16_t)kBorderW, (uint16_t)kBorderW, 0, 0}},           // TR
        {-kRadius, kRadius - kBoxSize, {(uint16_t)kBorderW, 0, 0, (uint16_t)kBorderW, 0}},           // BL
        {kRadius - kBoxSize, kRadius - kBoxSize, {0, (uint16_t)kBorderW, 0, (uint16_t)kBorderW, 0}}, // BR
    };
    for (int i = 0; i < 4; ++i)
    {
        const ReticuleCorner &c = corners[i];
        CLAY(CLAY_IDI("SelReticuleCorner", i), {.layout = {.sizing = {CLAY_SIZING_FIXED(kBoxSize), CLAY_SIZING_FIXED(kBoxSize)}},
                                                .floating = {.offset = {sx + c.ox, sy + c.oy}, .zIndex = 4, .attachTo = CLAY_ATTACH_TO_ROOT},
                                                .border = {.color = Pal::reticule, .width = c.border}}) {}
    }

    const float kOffsetX = 18.0f, kOffsetY = -8.0f; // nudge off the point itself
    CLAY(CLAY_ID("SelSatPanel"), {.layout = {
                                      .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                      .padding = {10, 10, 8, 8},
                                      .childGap = 3,
                                      .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                  .backgroundColor = Pal::panelBg,
                                  .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius),
                                  .floating = {.offset = {sx + kOffsetX, sy + kOffsetY}, .zIndex = 6, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        Clay_String headStr{false, (int32_t)strlen(infoLines[0]), infoLines[0]};
        CLAY_TEXT(headStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(13)}));
        if (isPlanet)
        {
            // A planet has no 3D model window to move the rest of its readout into, so it keeps the
            // whole block (type / magnitude / distance / phase / sun distance).
            for (int i = 1; i < kSelInfoLines; ++i)
            {
                if (infoLines[i][0] == '\0')
                    continue;
                Clay_String lineStr{false, (int32_t)strlen(infoLines[i]), infoLines[i]};
                CLAY_TEXT(lineStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));
            }
        }
        else
        {
            // A satellite shows its type here and moves the orbit lines (altitude / inclination /
            // RAAN / period / power) to the info window, which describes whatever it is showing.
            if (infoLines[1][0])
            {
                Clay_String lineStr{false, (int32_t)strlen(infoLines[1]), infoLines[1]};
                CLAY_TEXT(lineStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));
            }
            // Magnitude only. The range/phase line moved to the info window's OBSERVER box, and the
            // GPU-parity Δmag readout was dropped entirely (2026-09-24) — it was a development
            // instrument in a panel the player reads, and the check still logs a mismatch.
            if (selPhotLine[0][0])
            {
                Clay_String lineStr{false, (int32_t)strlen(selPhotLine[0]), selPhotLine[0]};
                CLAY_TEXT(lineStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12),
                                                      .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }
            CLAY(CLAY_ID("SelSatButtons"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                       .childGap = 6,
                                                       .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                       .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                buildInfoButton(inp, ui, 1);
                buildFollowButton(inp, ui, 1);
                buildTraceButton(inp, ui, 1);
            }
        }
    }
    // Panel size isn't known until Clay lays it out this frame — this is a rough estimate for
    // capture purposes only, same approximation the corner HUD panels' capture rects already use.
    captureLaidOut(ui, CLAY_ID("SelSatPanel"), sx + kOffsetX, sy + kOffsetY, 240.0f, 110.0f);
}

// ─── buildTraceWindow (benchmarking M9) ──────────────────────────────────────
// The selected satellite's magnitude over its pass (computeSelectedTrace): apparent magnitude after
// extinction (solid), above the atmosphere (faint), phase angle on the second axis (orange), and a
// marker at the current sim time. Retrace re-runs it for the current selection and time; Export
// writes the CSV that SatModelTool --replay-trace checks.
//
// PLACEMENT (2026-09-24): default 520x380, pinned to the bottom-left just above the time controls
// panel, so it coexists with the 3D view window in the top-right corner instead of covering the sky
// between them (it used to open centered, 680x440, over the middle of the screen). Its labels are
// sized for that narrower width — the legend and the "Now …" readout wrap in two otherwise.
void SatelliteSim::buildTraceWindow(const UIInput &inp, UIRenderer &ui)
{
    if (!traceChrome.open)
        return;
    if (traceChrome.w <= 0.0f)
    {
        // 520x380, shrunk on a small window (never wider than ~34% of it, never taller than ~50%) so
        // that the default leaves the middle of the sky alone and stops short of the view window. The
        // height is what the plot gets the leftovers of: the summary, the two readout lines, the
        // legend, the axis titles, the clock gutter and the button row come to ~240 px with the title
        // bar, so 380 leaves a ~140 px plot.
        traceChrome.w = std::min(520.0f, std::max(420.0f, inp.screenW * 0.34f));
        traceChrome.h = std::min(380.0f, std::max(300.0f, inp.screenH * 0.5f));
    }
    // Live mode: retrace at up to kTraceLiveHz, only when the result would change.
    if (traceLive && std::chrono::steady_clock::now() - traceLastRetrace >=
                         std::chrono::duration<double>(1.0 / kTraceLiveHz) &&
        traceStale())
        computeSelectedTrace();
    // ── Per frame: the "now" marker, and the traced satellite's magnitude at the current time,
    // evaluated with the trace's own inputs so it sits exactly on the curve.
    const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const bool nowIn = traceValid && tNow >= traceT0 && tNow <= traceT1 && traceT1 > traceT0;
    const float nowX = nowIn ? (float)((tNow - traceT0) / (traceT1 - traceT0)) : 0.0f;
    traceNowX[0] = traceNowX[1] = nowX;
    traceNowY[0] = nowIn ? 0.0f : NAN;
    traceNowY[1] = nowIn ? 1.0f : NAN;
    traceNowTickY[0] = traceNowTickY[1] = NAN;
    bool observerMoved = false;
    if (traceValid && traceSetup.satelliteIndex >= 0 && traceSetup.satelliteIndex < (int)satOrbits.size())
    {
        // Both readout lines are rebuilt together, so one can never show last pass's numbers.
        traceNowLine[0] = traceNowDetail[0] = '\0';
        const uint32_t ti = satOrbits[traceSetup.satelliteIndex].typeIdx;
        const SatelliteType &type = satTypes[ti];
        const SatTraceRow r = evalSatTraceRow(traceSetup, type.groups, type.lobes, &type.occlusion, tNow);
        char clock[16];
        time_t unixSim = (time_t)std::floor(tNow) + 946728000;
        struct tm *utc = gmtime(&unixSim);
        snprintf(clock, sizeof(clock), "%02d:%02d:%02d", utc ? utc->tm_hour : 0, utc ? utc->tm_min : 0,
                 utc ? utc->tm_sec : 0);
        // Two short lines rather than one 72-character one: the window is 520 px wide by default, and
        // the single line wrapped to two messily (a mid-word break in the middle of the numbers).
        snprintf(traceNowDetail, sizeof(traceNowDetail), "el %.0f deg  phase %.0f deg", r.elevationDeg, r.phaseDeg);
        if (r.elevationDeg <= 0.0)
            snprintf(traceNowLine, sizeof(traceNowLine), "Now %s: below the horizon", clock);
        else if (!std::isfinite(r.magApparent))
            snprintf(traceNowLine, sizeof(traceNowLine), "Now %s: in Earth's shadow", clock);
        else
        {
            snprintf(traceNowLine, sizeof(traceNowLine), "Now %s: mag %.2f  (%.2f above the air)", clock,
                     r.magApparent, r.mag);
            if (nowIn)
            {
                const float y = (traceMagFaint - (float)r.magApparent) / (traceMagFaint - traceMagBright);
                traceNowTickX[0] = nowX - 0.012f;
                traceNowTickX[1] = nowX + 0.012f;
                traceNowTickY[0] = traceNowTickY[1] = y;
            }
        }
        const float obsRadius = (float)satphot::kEarthRadiusM + obsTerrainH + obsHeightOffset;
        observerMoved = glm::length(glm::dvec3(obsDir) - traceSetup.obsDirEcef) > 1e-7 ||
                        std::abs((double)obsRadius - traceSetup.obsRadiusM) > 1.0;
    }

    static char titleBuf[128];
    snprintf(titleBuf, sizeof(titleBuf), "Magnitude trace: %s", traceTitle);
    auto text = [&](const char *s, Clay_Color c, float size) {
        Clay_String str{false, (int32_t)strlen(s), s};
        CLAY_TEXT(str, CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size)}));
    };
    auto button = [&](int id, const char *label, bool &hov, const char *tip) {
        bool clicked = false;
        CLAY(CLAY_IDI("TraceBtn", id), {.layout = {
                                            .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(24)},
                                            .padding = {10, 10, 0, 0},
                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                        .backgroundColor = hov ? Pal::btnHover : Pal::btnIdle,
                                        .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hov);
            sndClick(n, inp.lmbPressed);
            hov = n;
            clicked = n && inp.lmbPressed;
            ui.tooltip(inp, n, tip, fs(11));
            text(label, Pal::btnLabel, 11);
        }
        return clicked;
    };
    auto legend = [&](int id, Clay_Color c, const char *label) {
        CLAY(CLAY_IDI("TraceLegendSwatch", id), {.layout = {.sizing = {CLAY_SIZING_FIXED(14), CLAY_SIZING_FIXED(3)}},
                                                 .backgroundColor = c}) {}
        text(label, Pal::textDim, 11);
    };
    // Tick labels float over the plot's edges, placed from its size in the last layout (one frame
    // behind while the window is resized, which is invisible).
    const Clay_ElementId plotId = CLAY_ID("TracePlot");
    const Clay_ElementData plotData = Clay_GetElementData(plotId);
    const float plotW = plotData.found ? plotData.boundingBox.width : 0.0f;
    const float plotH = plotData.found ? plotData.boundingBox.height : 0.0f;
    const Clay_Color kPhaseCol = {255, 173, 77, 220};
    auto tick = [&](Clay_String prefix, int i, const char *label, Clay_Color c, Clay_FloatingAttachPointType elemPt,
                    Clay_FloatingAttachPointType parentPt, float ox, float oy) {
        Clay_String str{false, (int32_t)strlen(label), label};
        CLAY(CLAY_SIDI(prefix, (uint32_t)i),
             {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}},
              .floating = {.offset = {ox, oy},
                           .parentId = plotId.id,
                           .zIndex = 11,
                           .attachPoints = {.element = elemPt, .parent = parentPt},
                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                           .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID}})
        {
            CLAY_TEXT(str, CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(11)}));
        }
    };
    static const char *kPhaseTicks[kTracePhaseTicks] = {"180", "135", "90", "45", "0"};

    buildResizableWindow(
        inp, ui, traceChrome, 2, titleBuf, true, hovTraceClose, 12.0f,
        inp.screenH - traceChrome.h - 88.0f, 420.0f, 300.0f, 1600.0f, 1000.0f,
        [&]()
        {
            CLAY(CLAY_ID("TraceBody"), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                            .padding = {12, 12, 8, 10},
                                            .childGap = 6,
                                            .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                if (traceSummary[0])
                    text(traceSummary, Pal::textPrimary, 12);
                if (traceValid && traceNowLine[0])
                    text(traceNowLine, {255, 255, 255, 230}, 12);
                if (traceValid && traceNowDetail[0])
                    text(traceNowDetail, Pal::textDim, 11);
                if (traceValid)
                {
                    CLAY(CLAY_ID("TraceLegend"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                             .childGap = 6,
                                                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                             .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        // Short labels: the full ones ("apparent (after extinction)", "above atmosphere")
                        // total ~527 px and no longer fit the narrower window's 496 px content width.
                        legend(0, {140, 204, 255, 255}, "apparent");
                        legend(1, {140, 204, 255, 90}, "above air");
                        legend(2, {255, 173, 77, 190}, "phase");
                        legend(3, {255, 255, 255, 128}, "now");
                    }
                    // Axis titles over the two label gutters.
                    CLAY(CLAY_ID("TraceAxisTitles"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        text("mag", Pal::textDim, 11);
                        CLAY(CLAY_ID("TraceAxisTitleGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                        text("phase (deg)", kPhaseCol, 11);
                    }
                    // Plot between two label gutters; the labels themselves float (tick() below).
                    CLAY(CLAY_ID("TracePlotRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                              .childGap = 0,
                                                              .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY(CLAY_ID("TraceGutterL"), {.layout = {.sizing = {CLAY_SIZING_FIXED(34), CLAY_SIZING_GROW(0)}}}) {}
                        CLAY(CLAY_ID("TracePlotFrame"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                                    .padding = {1, 1, 1, 1}},
                                                         .backgroundColor = {12, 14, 18, 255},
                                                         .border = {.color = Style::borderColor, .width = CLAY_BORDER_ALL(1)}})
                        {
                            CLAY(plotId, {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                          .custom = {.customData = &tracePlot}}) {}
                        }
                        CLAY(CLAY_ID("TraceGutterR"), {.layout = {.sizing = {CLAY_SIZING_FIXED(34), CLAY_SIZING_GROW(0)}}}) {}
                    }
                    CLAY(CLAY_ID("TraceGutterB"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(16)}}}) {}
                    if (plotData.found)
                    {
                        // Label only as many ticks as fit: at least ~1.4 text heights apart
                        // vertically, and a clock label's width plus a gap horizontally. Grid
                        // lines stay at every whole magnitude.
                        const float minGapY = fs(11) * 1.4f, clockW = fs(11) * 5.5f;
                        const float magPitch = traceMagTickCount > 1
                                                   ? plotH * (traceMagTickFrac[0] - traceMagTickFrac[1])
                                                   : plotH;
                        const int magEvery = std::max(1, (int)std::ceil(minGapY / std::max(magPitch, 1.0f)));
                        const int phaseEvery = plotH / (kTracePhaseTicks - 1) >= minGapY ? 1 : 2;
                        const int timeEvery = plotW / (kTraceTimeTicks - 1) >= clockW * 1.3f
                                                  ? 1
                                                  : (plotW / 2.0f >= clockW * 1.3f ? 2 : kTraceTimeTicks - 1);
                        for (int i = 0; i < traceMagTickCount; ++i)
                            if (i % magEvery == 0)
                                tick(CLAY_STRING("TraceMagTick"), i, traceMagTickBuf[i], Pal::textDim, CLAY_ATTACH_POINT_RIGHT_CENTER,
                                 CLAY_ATTACH_POINT_LEFT_TOP, -6.0f, (1.0f - traceMagTickFrac[i]) * plotH);
                        for (int i = 0; i < kTracePhaseTicks; ++i)
                            if (i % phaseEvery == 0)
                                tick(CLAY_STRING("TracePhaseTick"), i, kPhaseTicks[i], kPhaseCol, CLAY_ATTACH_POINT_LEFT_CENTER,
                                 CLAY_ATTACH_POINT_RIGHT_TOP, 6.0f, plotH * i / (kTracePhaseTicks - 1));
                        for (int i = 0; i < kTraceTimeTicks; ++i)
                            if (i % timeEvery == 0)
                                tick(CLAY_STRING("TraceTimeTick"), i, traceTimeTickBuf[i], Pal::textDim,
                                 i == 0 ? CLAY_ATTACH_POINT_LEFT_TOP
                                        : (i == kTraceTimeTicks - 1 ? CLAY_ATTACH_POINT_RIGHT_TOP : CLAY_ATTACH_POINT_CENTER_TOP),
                                 CLAY_ATTACH_POINT_LEFT_BOTTOM, plotW * i / (kTraceTimeTicks - 1), 4.0f);
                    }
                }
                if (traceStatus[0])
                    text(traceStatus, traceValid ? Pal::textDim : Pal::listenKey, 11);
                if (traceExportStatus[0])
                    text(traceExportStatus, Pal::textDim, 11);
                if (observerMoved && !traceLive)
                    text("You have moved since this trace was taken; Retrace to trace from here.", Pal::textHint, 11);
                CLAY(CLAY_ID("TraceButtons"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                          .childGap = 8,
                                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    if (button(0, "Retrace", hovTraceRetrace, "Trace the current selection's pass from the current time"))
                        computeSelectedTrace();
                    if (button(2, traceLive ? "Live: ON" : "Live: OFF", hovTraceLive,
                               "Retrace up to 10 times a second when you move, the selection changes, the pass ends "
                               "or a photometry setting changes"))
                        traceLive = !traceLive;
                    if (traceValid &&
                        button(1, "Export CSV", hovTraceExport, "Write this trace as CSV (replay: SatModelTool --replay-trace)"))
                        exportTrace();
                    static char costBuf[48];
                    snprintf(costBuf, sizeof(costBuf), "retrace %.1f ms", traceRetraceMs);
                    text(costBuf, Pal::textHint, 11);
                }
            }
        });
}

// ─── Info window + the popped-out 3D view (Phase 4b; split 2026-09-24) ───────
// ONE satellite window by default. The info window leads with a compact 4:3 render of the subject —
// the view-preset chips sit over it — and scrolls through collapsible sections below: Satellite,
// Orbit and Photometry open, Observer / Camera / Render / Check collapsed (the Clouds tab's form).
// "Maximize" over the render pops the 3D view out into its own larger, resizable window; the small
// render stays live underneath it (one texture drawn twice, the small one showing a centred 4:3 crop
// of the larger target — see UIImage's u0..v1).
//
// One window with a pop-out rather than two permanent windows: both describe the same subject, so a
// second always-open window would double the chrome the player has to place and close. The pop-out is
// on demand, which is also what makes a small render worth having — the preset chips are one click at
// any size.
void SatelliteSim::updateViewerView(const UIInput &inp, UIRenderer &ui)
{
    if (!meshRendererInit || (!infoChrome.open && !viewerChrome.open))
        return; // nothing visible: no target to size, no drag to read, no readouts to refresh
    // A viewer tracking a satellite follows the selection (as the magnitude trace does), when the
    // newly selected satellite has a model.
    if (selectedSatIndex >= 0 && selectedSatIndex != viewerLastSelected && selectedSatIndex != viewerSatIndex &&
        selectedSatIndex < (int)satOrbits.size() && meshRenderer.typeMesh((int)satOrbits[selectedSatIndex].typeIdx))
    {
        const SatOrbit &orb = satOrbits[selectedSatIndex];
        openModelViewer((int)orb.typeIdx, satTypes[orb.typeIdx].name.c_str(), orb.altM, selectedSatIndex);
    }
    if (infoChrome.w <= 0.0f)
    {
        // Right edge: the trace window lives above the time controls in the bottom-left corner, so the
        // satellite windows anchor right. Narrow, because its body is a scrolling list of sections.
        infoChrome.w = std::min(470.0f, std::max(380.0f, inp.screenW * 0.34f));
        infoChrome.h = std::min(680.0f, std::max(470.0f, inp.screenH - 24.0f)); // 470 = its min height
    }
    if (viewerChrome.w <= 0.0f)
    {
        // The pop-out: as large as the desktop allows, placed beside (left of) the info window so
        // opening it does not cover the window it came out of.
        viewerChrome.w = std::min(900.0f, std::max(480.0f, inp.screenW * 0.48f));
        viewerChrome.h = std::min(640.0f, std::max(360.0f, inp.screenH * 0.8f));
    }

    // ── The two view boxes, the ONE target, and each view's crop ──────────────────────────
    // Sizes are derived from the chrome rects rather than from last frame's layout, so the target and
    // the crops can never disagree with what gets drawn (a resize costs one frame of mismatch, the
    // same idiom the image element's own box already used). `kViewPad` is the padding the two builders
    // give their image frame — keep them in step with this.
    const float kViewPad = 10.0f;
    const float bandW = std::max(32.0f, infoChrome.w - 2.0f * kViewPad - 2.0f);
    const float bandH = bandW * 0.75f; // the mini view is 4:3 whatever the window is
    viewerBandH = bandH;
    const float popW = std::max(32.0f, viewerChrome.w - 2.0f * kViewPad - 2.0f);
    const float popH = std::max(32.0f, viewerChrome.h - std::max(36.0f, (float)fs(16) + 16.0f) - 2.0f * kViewPad - 2.0f);
    const bool usePop = viewerChrome.open && popW * popH >= bandW * bandH; // the larger view drives it
    const uint32_t tw = (uint32_t)std::max(16.0f, usePop ? popW : bandW);
    const uint32_t th = (uint32_t)std::max(16.0f, usePop ? popH : bandH);
    const bool recreated = meshRenderer.ensureViewerTarget(*ctx_, tw, th);
    if (envProbes.ready())
    {
        if (envProbes.ensureViewerBg(*ctx_, tw, th))
            meshRenderer.setViewerBackground(envProbes.viewerBgView(), envProbes.sampler());
        viewerBgW = tw;
        viewerBgH = th;
    }
    if (viewerImageId == 0)
        viewerImageId = ui.registerImage(ctx_->device, meshRenderer.viewerView(), meshRenderer.viewerSampler());
    else if (recreated)
        ui.updateImage(ctx_->device, viewerImageId, meshRenderer.viewerView(), meshRenderer.viewerSampler());
    viewerAspect = (float)tw / (float)th;
    // Each view samples a centred sub-rect with its OWN aspect, so one render fills both shapes (4:3
    // and whatever the pop-out is) with no stretch. The view that drives the target samples all of it.
    {
        const float srcAspect = viewerAspect;
        float mU0 = 0.0f, mV0 = 0.0f, mU1 = 1.0f, mV1 = 1.0f;
        const float bandAspect = bandW / bandH; // 4:3
        if (srcAspect > bandAspect)
        {
            const float s = bandAspect / srcAspect;
            mU0 = (1.0f - s) * 0.5f;
            mU1 = 1.0f - mU0;
        }
        else if (srcAspect < bandAspect)
        {
            const float s = srcAspect / bandAspect;
            mV0 = (1.0f - s) * 0.5f;
            mV1 = 1.0f - mV0;
        }
        viewerUvMini[0] = mU0;
        viewerUvMini[1] = mV0;
        viewerUvMini[2] = mU1;
        viewerUvMini[3] = mV1;
        const float pU0 = 0.0f, pV0 = 0.0f, pU1 = 1.0f, pV1 = 1.0f;
        const float popAspect = popW / popH;
        if (!usePop && viewerChrome.open && popAspect > 0.0f)
        {
            // The mini view drove the target (a tiny pop-out): crop for it instead. Won't happen with
            // the default sizes, but the window is resizable all the way down.
            if (srcAspect > popAspect)
            {
                const float s = popAspect / srcAspect;
                viewerUvPop[0] = (1.0f - s) * 0.5f;
                viewerUvPop[2] = 1.0f - viewerUvPop[0];
            }
            else if (srcAspect < popAspect)
            {
                const float s = srcAspect / popAspect;
                viewerUvPop[1] = (1.0f - s) * 0.5f;
                viewerUvPop[3] = 1.0f - viewerUvPop[1];
            }
        }
        else
        {
            viewerUvPop[0] = pU0;
            viewerUvPop[1] = pV0;
            viewerUvPop[2] = pU1;
            viewerUvPop[3] = pV1;
        }
        viewerImageMini.imageId = viewerImageId;
        viewerImageMini.u0 = viewerUvMini[0];
        viewerImageMini.v0 = viewerUvMini[1];
        viewerImageMini.u1 = viewerUvMini[2];
        viewerImageMini.v1 = viewerUvMini[3];
        viewerImagePop.imageId = viewerImageId;
        viewerImagePop.u0 = viewerUvPop[0];
        viewerImagePop.v0 = viewerUvPop[1];
        viewerImagePop.u1 = viewerUvPop[2];
        viewerImagePop.v1 = viewerUvPop[3];
    }

    // ── Drag to orbit / scroll to zoom, on either view ────────────────────────────────────
    // Hit-tested against the two image elements' laid-out boxes (the PREVIOUS frame's, the established
    // idiom) and gated on the preset chips' hover flags: without that gate a click on a chip would also
    // grab the camera and orbit the model under the pointer.
    auto overBox = [&](const Clay_ElementData &d) {
        return d.found && inp.mouseX >= d.boundingBox.x && inp.mouseX < d.boundingBox.x + d.boundingBox.width &&
               inp.mouseY >= d.boundingBox.y && inp.mouseY < d.boundingBox.y + d.boundingBox.height;
    };
    const bool overImg = (infoChrome.open && overBox(Clay_GetElementData(CLAY_ID("ViewerImageMini")))) ||
                         (viewerChrome.open && overBox(Clay_GetElementData(CLAY_ID("ViewerImagePop"))));
    bool chipHover = false;
    for (int row = 0; row < kViewChipRows; ++row)
        for (int k = 0; k < kViewChipCount; ++k)
            chipHover = chipHover || hovViewChip[row][k];
    if (overImg && !chipHover && inp.lmbPressed && !infoChrome.dragging && !viewerChrome.dragging &&
        infoChrome.resizeEdge == kResizeNone && viewerChrome.resizeEdge == kResizeNone)
    {
        viewerDragging = true;
        viewerAim = 0; // taking the camera ends a preset (it continues from where the preset left it)
    }
    if (!inp.lmbDown)
        viewerDragging = false;
    if (viewerDragging)
    {
        viewerYawDeg -= inp.dMouseX * 0.4f;
        viewerPitchDeg = glm::clamp(viewerPitchDeg + inp.dMouseY * 0.3f, -85.0f, 85.0f);
    }
    else if (viewerSpin && viewerAim == 0)
        viewerYawDeg += 8.0f * inp.dt;
    if (overImg && inp.scrollY != 0.0f && viewerDist > 0.0f)
        viewerDist *= powf(0.88f, inp.scrollY);

    // Photometric check result (recorded last frame, which has completed by now): integrate the
    // render's L·d² over each pixel's solid angle → radiant intensity toward the camera, per unit solar
    // irradiance, and compare with the lobe model's (both reduced to 1000 km).
    if (viewerCheckAwaiting && meshRenderer.checkPixels())
    {
        viewerCheckAwaiting = false;
        const int n = (int)SatMeshRenderer::kCheckSize;
        const double t = viewerCheckTanHalf, pix = 2.0 * t / n;
        const float *px = meshRenderer.checkPixels();
        double sum = 0.0;
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x)
            {
                const float v = px[(size_t)y * n + x];
                if (v <= 0.0f)
                    continue;
                const double sx = ((x + 0.5) / n * 2.0 - 1.0) * t, sy = ((y + 0.5) / n * 2.0 - 1.0) * t;
                sum += v * pix * pix / std::pow(1.0 + sx * sx + sy * sy, 1.5);
            }
        const double iRender = sum / glm::pi<double>();
        const double mR = satMagnitudeFromIntensity(iRender, 1.0e6), mM = satMagnitudeFromIntensity(viewerCheckModelI, 1.0e6);
        if (std::isfinite(mR) && std::isfinite(mM))
            snprintf(viewerCheckLine, sizeof(viewerCheckLine),
                     "Sun only, phase %.0f deg%s:\nrender %.2f, model %.2f\n(1000 km; difference %+.3f)",
                     viewerCheckPhaseDeg, viewerShadows ? ", shadows" : "", mR, mM, mR - mM);
        else
            snprintf(viewerCheckLine, sizeof(viewerCheckLine), "Phase %.0f deg: not sunlit from\nthis side (render %s, model %s)",
                     viewerCheckPhaseDeg, std::isfinite(mR) ? "lit" : "dark", std::isfinite(mM) ? "lit" : "dark");
    }

    // Observer box: re-evaluated at up to 10 Hz.
    if (glfwGetTime() >= viewerObsNextWall)
    {
        viewerObsNextWall = glfwGetTime() + 0.1;
        updateViewerObserverInfo();
    }
}

// ─── buildViewChip ───────────────────────────────────────────────────────────
// One view-preset chip over a render: the icon alone, tooltip = its name (the same rule as every other
// satellite action button), on a translucent scrim so it stays readable over bright clouds or the
// Earth's limb — a bare white icon on those is invisible. `on` is the accent fill: the "obvious tell"
// that this preset is the active one.
bool SatelliteSim::buildViewChip(const UIInput &inp, UIRenderer &ui, int row, int k, const char *name, int iconIdx,
                                 bool on)
{
    const float sz = std::max(26.0f, (float)fs(12) + 14.0f);
    const float isz = sz - 10.0f;
    bool &hov = hovViewChip[row][k];
    bool clicked = false;
    Clay_Color bg = on ? (hov ? Pal::chipOnHover : Pal::chipOn) : (hov ? Pal::chipHover : Pal::chipIdle);
    CLAY(CLAY_SIDI(CLAY_STRING("ViewChip"), row * 8 + k),
         {.layout = {.sizing = {CLAY_SIZING_FIXED(sz), CLAY_SIZING_FIXED(sz)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = bg,
          .cornerRadius = CLAY_CORNER_RADIUS(5)})
    {
        bool n = Clay_Hovered();
        sndRollover(n, hov);
        sndClick(n, inp.lmbPressed);
        hov = n;
        clicked = n && inp.lmbPressed;
        ui.tooltip(inp, n, name, fs(11));
        CLAY(CLAY_SIDI(CLAY_STRING("ViewChipIcon"), row * 8 + k),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(isz), CLAY_SIZING_FIXED(isz)}},
              .image = {.imageData = (void *)(intptr_t)(iconIdx + 1)}}) {}
    }
    return clicked;
}

// ─── buildViewChips ──────────────────────────────────────────────────────────
// The chip row over a render: the 3D VIEW's own controls, nothing else. Select and Go to used to
// live here too and moved back to the title bar — they act on the SUBJECT (they are the same two
// actions the selection panel carries), while these four change what the render shows.
//   Spin      — the free camera's idle rotation. A plain toggle: clicking it while lit stops it, and
//               "nothing lit" is simply the free camera (there is no Free chip to light up).
//   Observer  — the view from the ground observer's direction, which is what makes a ground flare's
//               specular show on the model. Exclusive with Spin by construction: taking a preset
//               clears the free camera, so the Spin chip reads off while it is active.
//   Studio    — LIGHTING (live sky vs the fixed studio sun). Deliberately its own group: spin + live
//               light is a legitimate combination, so it is not part of the view radio set.
//   Maximize  — pops the render out into its own window ("Restore" from inside it).
// ─── buildViewerMarkerLabels ─────────────────────────────────────────────────
// "You" / "Target" beside the viewer's marker dots: floating text anchored to the image element, placed
// from the dot's position in the render target (viewerMarkerLabels) through the view's crop of it
// (viewerUvMini / viewerUvPop) onto the element's last laid-out box. Pointer-transparent, so a drag on
// the image under a label still orbits the camera.
void SatelliteSim::buildViewerMarkerLabels(int view)
{
    if (!meshRenderer.viewerRendered() || viewerBgW == 0 || viewerBgH == 0)
        return;
    const Clay_ElementId imgId = view == 0 ? CLAY_ID("ViewerImageMini") : CLAY_ID("ViewerImagePop");
    const Clay_ElementData d = Clay_GetElementData(imgId);
    if (!d.found || d.boundingBox.width < 1.0f || d.boundingBox.height < 1.0f)
        return;
    const float *uv = view == 0 ? viewerUvMini : viewerUvPop;
    static const Clay_Color kLabelColor[2] = {{90, 242, 255, 255}, {255, 158, 46, 255}};
    const float r = std::max(4.0f, 5.0f * uiScale); // the dot's radius (recordModelViewer)
    for (int m = 0; m < 2; ++m)
    {
        const ViewerMarkerLabel &L = viewerMarkerLabels[m];
        if (!L.on)
            continue;
        const float u = L.x / (float)viewerBgW, v = L.y / (float)viewerBgH;
        const float ex = (u - uv[0]) / std::max(uv[2] - uv[0], 1e-6f) * d.boundingBox.width;
        const float ey = (v - uv[1]) / std::max(uv[3] - uv[1], 1e-6f) * d.boundingBox.height;
        if (ex < 0.0f || ey < 0.0f || ex > d.boundingBox.width || ey > d.boundingBox.height)
            continue;
        CLAY(CLAY_IDI("ViewerMarkerLabel", view * 2 + m),
             {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {4, 4, 1, 1}},
              .backgroundColor = {0, 0, 0, 150},
              .cornerRadius = CLAY_CORNER_RADIUS(3),
              .floating = {.offset = {ex + r + 5.0f, ey - 0.5f * (float)fs(11) - 2.0f},
                           .parentId = imgId.id,
                           .zIndex = 11,
                           .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP, .parent = CLAY_ATTACH_POINT_LEFT_TOP},
                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                           .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID}})
        {
            if (m == 0)
                CLAY_TEXT(CLAY_STRING("You"), CLAY_TEXT_CONFIG({.textColor = kLabelColor[0], .fontSize = fs(11)}));
            else
                CLAY_TEXT(CLAY_STRING("Target"), CLAY_TEXT_CONFIG({.textColor = kLabelColor[1], .fontSize = fs(11)}));
        }
    }
}

void SatelliteSim::buildViewChips(const UIInput &inp, UIRenderer &ui, int row)
{
    const bool popped = viewerChrome.open;
    CLAY(CLAY_SIDI(CLAY_STRING("ViewChips"), row),
         {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                     .childGap = 4,
                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        const bool spinning = viewerSpin && viewerAim == 0;
        if (buildViewChip(inp, ui, row, 0, "Spin", kIconSpin, spinning))
        {
            if (spinning)
                viewerSpin = false;
            else
            {
                viewerAim = 0; // spin belongs to the free camera, so asking for it takes it back
                viewerSpin = true;
            }
            viewerDragging = false;
        }
        if (buildViewChip(inp, ui, row, 1, "Observer", kIconObserver, viewerAim == 1))
            viewerAim = viewerAim == 1 ? 0 : 1;
        if (buildViewChip(inp, ui, row, 2, "Studio", kIconStudio, viewerStudioLight))
            viewerStudioLight = !viewerStudioLight;
        if (buildViewChip(inp, ui, row, 3, popped ? "Restore" : "Maximize", kIconMaximize, popped))
            viewerChrome.open = !viewerChrome.open;
    }
}

// ─── buildViewTitleIcons ─────────────────────────────────────────────────────
// Select and Go to, in the title bar of both satellite windows (left of the close button) — the two
// SUBJECT actions, as distinct from the view chips over the render. Icon-only with name tooltips, as
// every satellite action button is. `popout` picks which window's hover state to use, so the two
// title bars do not fight over it.
void SatelliteSim::buildViewTitleIcons(const UIInput &inp, UIRenderer &ui, bool popout)
{
    const bool tracked = viewerSatIndex >= 0 && viewerSatIndex < (int)satOrbits.size();
    if (!tracked)
        return;
    const bool isSelected = selectedSatIndex == viewerSatIndex;
    const bool isFollowed = followActive && followSatIndex == viewerSatIndex;
    bool *hov = popout ? hovViewerTitleBtn : hovInfoTitleBtn;
    const int idBase = popout ? 0 : 2; // unique Clay ids across the two windows
    auto icon = [&](int k, int iconIdx, bool on, const char *name) {
        bool clicked = false;
        const float sz = std::max(24.0f, (float)fs(12) + 12.0f);
        CLAY(CLAY_SIDI(CLAY_STRING("ViewTitleBtn"), idBase + k),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(sz), CLAY_SIZING_FIXED(sz)},
                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
              .backgroundColor = hov[k] ? (on ? Pal::btnAccentHv : Pal::btnHover)
                                        : (on ? Pal::btnAccent : Pal::btnIdle),
              .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hov[k]);
            sndClick(n, inp.lmbPressed);
            hov[k] = n;
            clicked = n && inp.lmbPressed;
            ui.tooltip(inp, n, name, fs(11));
            const float isz = sz - 8.0f;
            CLAY(CLAY_SIDI(CLAY_STRING("ViewTitleIcon"), idBase + k),
                 {.layout = {.sizing = {CLAY_SIZING_FIXED(isz), CLAY_SIZING_FIXED(isz)}},
                  .image = {.imageData = (void *)(intptr_t)(iconIdx + 1)}}) {}
        }
        CLAY(CLAY_SIDI(CLAY_STRING("ViewTitleGap"), idBase + k),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(6), CLAY_SIZING_FIXED(1)}}}) {}
        return clicked;
    };
    if (icon(0, kIconSelect, isSelected, "Select"))
        selectSatellite(viewerSatIndex);
    if (icon(1, kIconEye, isFollowed, "Go to"))
    {
        if (isFollowed)
            stopFollow();
        else
        {
            selectSatellite(viewerSatIndex);
            startFollow(viewerSatIndex);
        }
    }
}

// ─── buildInfoWindow (the satellite window; split + sections 2026-09-24) ─────
// Title: the subject's name, amber + "(selected)" while it is the selection. Body: the fixed 4:3 render
// at the top with the chip row over it (it does not scroll away — only the sections below do), then the
// collapsible sections: Satellite / Orbit / Photometry open, Observer / Camera / Render / Check
// collapsed. The values come from updateViewerObserverInfo (10 Hz, the CPU evaluator on the VIEWED
// satellite — the viewer can be tracking one that is not the selection).
void SatelliteSim::buildInfoWindow(const UIInput &inp, UIRenderer &ui)
{
    if (!infoChrome.open || !meshRendererInit)
        return;
    const bool tracked = viewerSatIndex >= 0 && viewerSatIndex < (int)satOrbits.size();
    const bool isSelected = tracked && selectedSatIndex == viewerSatIndex;

    static const char *kInfoSectionNames[kInfoSectionCount] = {
        "SATELLITE", "ORBIT", "PHOTOMETRY", "OBSERVER", "CAMERA", "RENDER", "CHECK"};
    static const char *kOrbitLabels[kViewerOrbitRows] = {"Altitude", "Inclination", "RAAN", "Period", "Power"};

    auto text = [&](const char *s, Clay_Color c, float size) {
        Clay_String str{false, (int32_t)strlen(s), s};
        CLAY_TEXT(str, CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size)}));
    };
    // A full-width button of a section; `on` shows an active state.
    auto button = [&](int id, const char *label, const char *tip, bool on = false) {
        bool clicked = false;
        bool &hov = hovViewerBtn[id];
        CLAY(CLAY_IDI("ViewerBtn", id), {.layout = {
                                             .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED((float)fs(11) + 12.0f)},
                                             .padding = {8, 8, 0, 0},
                                             .childAlignment = {.x = CLAY_ALIGN_X_LEFT, .y = CLAY_ALIGN_Y_CENTER}},
                                         .backgroundColor = hov ? (on ? Pal::btnAccentHv : Pal::btnHover)
                                                                : (on ? Pal::btnAccent : Pal::btnIdle),
                                         .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hov);
            sndClick(n, inp.lmbPressed);
            hov = n;
            clicked = n && inp.lmbPressed;
            ui.tooltip(inp, n, tip, fs(11));
            Clay_String ls{false, (int32_t)strlen(label), label};
            CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(11),
                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        return clicked;
    };
    // A collapsible section: header + body, the Clouds tab's form (buildCloudSliderSections). "+"/"-"
    // rather than a chevron glyph — the font atlas bakes ASCII 32-126 only — and the open state is
    // session-only, not a preference.
    auto infoSection = [&](int si, const std::function<void()> &body) {
        bool open = infoSectionOpen[si];
        CLAY(CLAY_IDI("InfoSectHdr", si), {.layout = {
                                               .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                               .padding = {8, 10, 0, 0},
                                               .childGap = 8,
                                               .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                               .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                           .backgroundColor = hovInfoSection[si] ? Pal::btnHover : Pal::sectionHdr,
                                           .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovInfoSection[si]);
            sndClick(n, inp.lmbPressed);
            hovInfoSection[si] = n;
            if (n && inp.lmbPressed)
                infoSectionOpen[si] = !open;
            CLAY(CLAY_IDI("InfoSectMark", si), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIXED(10), CLAY_SIZING_FIT(0)},
                                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(open ? CLAY_STRING("-") : CLAY_STRING("+"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(12)}));
            }
            CLAY(CLAY_IDI("InfoSectTitle", si), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String ts{false, (int32_t)strlen(kInfoSectionNames[si]), kInfoSectionNames[si]};
                CLAY_TEXT(ts, CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(12)}));
            }
        }
        if (open)
            body();
    };

    // Title: the satellite's name, highlighted while it is the selection.
    static char titleBuf[140];
    snprintf(titleBuf, sizeof(titleBuf), "%s%s", viewerTitle, isSelected ? "  (selected)" : "");

    // minH 470: the render band is 4:3 (268 px at the 380 px minimum width) and the sections need a
    // usable slice under it — below that the window would be all render and no list.
    buildResizableWindow(
        inp, ui, infoChrome, 4, titleBuf, true, hovInfoClose, inp.screenW - infoChrome.w - 12.0f, 12.0f,
        380.0f, 470.0f, 1400.0f, 1600.0f,
        [&]()
        {
            CLAY(CLAY_ID("InfoBody"), {.layout = {
                                           .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                           .padding = {10, 10, 8, 8},
                                           .childGap = 8,
                                           .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                // ── The render: fixed 4:3 with the chips over it, never scrolls away ──────────────
                // The black backing is on the PARENT: Clay emits an element's CUSTOM command before the
                // RECTANGLE for its own backgroundColor, which would cover the image (see UIImage).
                CLAY(CLAY_ID("ViewerImageFrameMini"), {.layout = {
                                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(viewerBandH)}},
                                                      .backgroundColor = {0, 0, 0, 255}})
                {
                    CLAY(CLAY_ID("ViewerImageMini"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                      .custom = {.customData = meshRenderer.viewerRendered()
                                                                                   ? &viewerImageMini
                                                                                   : nullptr}}) {}
                    // Floating, anchored to the image element (not a child of it: an element's own
                    // children are laid out by it). Without the anchor a window resize slides the chips
                    // off the image.
                    CLAY(CLAY_ID("ViewChipsMini"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}},
                                                    .floating = {.offset = {8, 8},
                                                                 .parentId = CLAY_ID("ViewerImageMini").id,
                                                                 .zIndex = 12,
                                                                 .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                                                                  .parent = CLAY_ATTACH_POINT_LEFT_TOP},
                                                                 .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID}})
                    {
                        buildViewChips(inp, ui, 0);
                    }
                    buildViewerMarkerLabels(0);
                }
                text("Drag to orbit, scroll to zoom. Cyan: you; orange: site", Pal::textHint, 11);
                // ── The sections: only these scroll ──────────────────────────────────────────────
                CLAY(CLAY_ID("InfoSections"), {.layout = {
                                                   .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                   .childGap = 4,
                                                   .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                               .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                {
                    // ── SATELLITE: what model it is ──────────────────────────────────────────────
                    infoSection(0, [&]()
                                {
                                    text(viewerInfo, Pal::textDim, 11);
                                    if (!tracked)
                                        text("Not tracking a satellite: pick one from the sky.", Pal::textHint, 11);
                                    else if (isSelected)
                                        text("Selected in the sky", Pal::textDim, 11);
                                    if (tracked && followActive && followSatIndex == viewerSatIndex)
                                        text("Following it (Go to)", Pal::textDim, 11);
                                });
                    // ── ORBIT: the rows the selection panel used to carry ───────────────────────
                    infoSection(1, [&]()
                                {
                                    for (int i = 0; i < viewerOrbitCount && i < kViewerOrbitRows; ++i)
                                    {
                                        CLAY(CLAY_IDI("InfoOrbitRow", i), {.layout = {
                                                                               .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                               .childGap = 8,
                                                                               .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                               .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                                        {
                                            CLAY(CLAY_IDI("InfoOrbitLabel", i), {.layout = {
                                                                                     .sizing = {CLAY_SIZING_FIXED((float)fs(11) * 8.0f),
                                                                                                CLAY_SIZING_FIT(0)}}})
                                            {
                                                Clay_String ls{false, (int32_t)strlen(kOrbitLabels[i]), kOrbitLabels[i]};
                                                CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = Pal::textDim,
                                                                                .fontSize = fs(11),
                                                                                .wrapMode = CLAY_TEXT_WRAP_NONE}));
                                            }
                                            Clay_String vs{false, (int32_t)strlen(viewerOrbitValue[i]), viewerOrbitValue[i]};
                                            CLAY_TEXT(vs, CLAY_TEXT_CONFIG({.textColor = Pal::volValue,
                                                                            .fontSize = fs(11),
                                                                            .wrapMode = CLAY_TEXT_WRAP_NONE}));
                                        }
                                    }
                                });
                    // ── PHOTOMETRY: the CPU evaluator on the viewed satellite + the pass trace ──
                    infoSection(2, [&]()
                                {
                                    for (const auto &line : viewerPhotLine)
                                        if (line[0])
                                            text(line, Pal::textDim, 11);
                                    if (tracked && button(10, "Trace pass", "Trace pass"))
                                    {
                                        selectSatellite(viewerSatIndex);
                                        computeSelectedTrace();
                                    }
                                });
                    // ── OBSERVER: where it is in the parked observer's sky ─────────────────────
                    infoSection(3, [&]()
                                {
                                    for (const auto &line : viewerObsLine)
                                        if (line[0])
                                            text(line, Pal::textDim, 11);
                                    if (button(9, viewerMarkers ? "Markers: on" : "Markers: off", "Markers",
                                               viewerMarkers))
                                        viewerMarkers = !viewerMarkers;
                                });
                    // ── CAMERA: the presets' long tail (the chips carry the quick ones) ────────
                    infoSection(4, [&]()
                                {
                                    static const char *kAimLabels[3] = {"Camera: Free", "Camera: From you",
                                                                        "Camera: Toward you"};
                                    if (button(8, kAimLabels[viewerAim], "Camera aim", viewerAim != 0))
                                        viewerAim = (viewerAim + 1) % 3;
                                    if (button(0, viewerSpin ? "Spin: on" : "Spin: off", "Spin", viewerSpin))
                                        viewerSpin = !viewerSpin;
                                    if (button(5, "Reset view", "Reset view"))
                                    {
                                        viewerDist = 0.0f;
                                        viewerYawDeg = 35.0f;
                                        viewerPitchDeg = 18.0f;
                                        viewerAim = 0;
                                    }
                                });

                    // ── RENDER ─────────────────────────────────────────────────────────────────
                    infoSection(5, [&]()
                                {
                                    if (button(1, viewerStudioLight ? "Light: Studio" : "Light: Live", "Light",
                                               viewerStudioLight))
                                        viewerStudioLight = !viewerStudioLight;
                                    if (button(2, viewerSunlitPose ? "Pose: Sunlit" : "Pose: Rest", "Pose",
                                               viewerSunlitPose))
                                        viewerSunlitPose = !viewerSunlitPose;
                                    if (button(3, viewerShadows ? "Shadows: on" : "Shadows: off", "Shadows",
                                               viewerShadows))
                                        viewerShadows = !viewerShadows;
                                    if (button(4, viewerReflections ? "Reflections: on" : "Reflections: off",
                                               "Reflections", viewerReflections))
                                        viewerReflections = !viewerReflections;
                                    if (button(7, viewerDetail ? "Detail: on" : "Detail: off", "Detail",
                                               viewerDetail))
                                        viewerDetail = !viewerDetail;
                                });

                    // ── CHECK: the render-vs-lobes cross-check ─────────────────────────────────
                    infoSection(6, [&]()
                                {
                                    if (button(6, "Photometric check", "Photometric check"))
                                        viewerCheckRequested = true;
                                    if (viewerCheckLine[0])
                                        text(viewerCheckLine, Pal::textDim, 11);
                                });
                }
                // A visible thumb on the section list: without it nothing says the list continues
                // below the window's edge (the tab bodies are long and only the summary is in view).
                ui.scrollbar(CLAY_ID("InfoSections"));
            }
        },
        [&]() { buildViewTitleIcons(inp, ui, false); }, isSelected);
}

// ─── buildViewPopoutWindow (the maximized 3D view) ───────────────────────────
// The render on its own, resizable, with the same chip row. Opened by "Maximize" over the info
// window's small view; the small view keeps running underneath (one texture, its own crop), so
// "Restore" is just closing this one.
void SatelliteSim::buildViewPopoutWindow(const UIInput &inp, UIRenderer &ui)
{
    if (!viewerChrome.open || !meshRendererInit)
        return;
    const bool tracked = viewerSatIndex >= 0 && viewerSatIndex < (int)satOrbits.size();
    const bool isSelected = tracked && selectedSatIndex == viewerSatIndex;

    static char titleBuf[140];
    snprintf(titleBuf, sizeof(titleBuf), "%s%s", viewerTitle, isSelected ? "  (selected)" : "");

    // The pop-out opens beside (left of) the info window — or at the right edge if the info window has
    // never been placed. Only used the first time it is placed (x < 0).
    const float popDefaultX = infoChrome.x >= 0.0f ? std::max(12.0f, infoChrome.x - viewerChrome.w - 12.0f)
                                                   : inp.screenW - viewerChrome.w - 12.0f;
    buildResizableWindow(
        inp, ui, viewerChrome, 3, titleBuf, true, hovViewerClose, popDefaultX, 12.0f, 400.0f, 300.0f, 2000.0f,
        1400.0f,
        [&]()
        {
            // The black backing is on the PARENT — see the info window's copy of this note. The padding
            // is kViewPad in updateViewerView, which sizes the target from it.
            CLAY(CLAY_ID("ViewerImageFramePop"), {.layout = {
                                                      .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                      .padding = {10, 10, 10, 10}},
                                                  .backgroundColor = {0, 0, 0, 255}})
            {
                CLAY(CLAY_ID("ViewerImagePop"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                 .custom = {.customData = meshRenderer.viewerRendered()
                                                                              ? &viewerImagePop
                                                                              : nullptr}}) {}
                CLAY(CLAY_ID("ViewChipsPop"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}},
                                               .floating = {.offset = {10, 10},
                                                            .parentId = CLAY_ID("ViewerImagePop").id,
                                                            .zIndex = 12,
                                                            .attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_TOP,
                                                                             .parent = CLAY_ATTACH_POINT_LEFT_TOP},
                                                            .attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID}})
                {
                    buildViewChips(inp, ui, 1);
                }
                buildViewerMarkerLabels(1);
            }
        },
        [&]() { buildViewTitleIcons(inp, ui, true); }, isSelected);
}

// ─── viewTitleIconsHovered ───────────────────────────────────────────────────
// Reads the title-icon hover state for a window id: buildResizableWindow's title bar must NOT start a
// drag when the click landed on one of those icons (they are buttons drawn inside the title bar).
bool SatelliteSim::viewTitleIconsHovered(int winId) const
{
    switch (winId)
    {
    case 3: return hovViewerTitleBtn[0] || hovViewerTitleBtn[1];
    case 4: return hovInfoTitleBtn[0] || hovInfoTitleBtn[1];
    default: return false;
    }
}

// ─── buildSelActionButton ───────────────────────────────────────────────────
// One action button for the selection panel / out-of-view chip: the icon on its own, with the button's
// NAME as the tooltip ("Info" / "Go to" / "Trace pass"). No label in the row and no paragraph of
// explanation — that is the treatment for every satellite action button now, the info window's own
// icon buttons included; a button that needs a sentence to explain itself is mislabeled.
// Returns true when clicked this frame.
bool SatelliteSim::buildSelActionButton(const UIInput &inp, UIRenderer &ui, int id, const char *name, int iconIdx,
                                        bool on, bool &hov)
{
    bool clicked = false;
    const float sz = std::max(kSelIconBtnMin, (float)fs(12) + 12.0f);
    const float isz = sz - 8.0f;
    CLAY(CLAY_SIDI(CLAY_STRING("SelActBtn"), id),
         {.layout = {.sizing = {CLAY_SIZING_FIXED(sz), CLAY_SIZING_FIXED(sz)},
                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
          .backgroundColor = hov ? (on ? Pal::btnAccentHv : Pal::btnHover) : (on ? Pal::btnAccent : Pal::btnIdle),
          .cornerRadius = CLAY_CORNER_RADIUS(4)})
    {
        bool n = Clay_Hovered();
        sndRollover(n, hov);
        sndClick(n, inp.lmbPressed);
        hov = n;
        clicked = n && inp.lmbPressed;
        ui.tooltip(inp, n, name, fs(11));
        CLAY(CLAY_SIDI(CLAY_STRING("SelActIcon"), id),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(isz), CLAY_SIZING_FIXED(isz)}},
              .image = {.imageData = (void *)(intptr_t)(iconIdx + 1)}}) {}
    }
    return clicked;
}

// ─── buildInfoButton ─────────────────────────────────────────────────────────
// The "Info" button (a serif "i" icon, tooltip "Info") for a selected satellite whose type has a
// geometry model: opens the info window (the model viewer, which now leads with the satellite's orbit
// and brightness and keeps the 3D view in its left half). The button is lit while that window is
// showing this satellite.
void SatelliteSim::buildInfoButton(const UIInput &inp, UIRenderer &ui, int idx)
{
    if (selectedSatIndex < 0 || selectedSatIndex >= (int)satOrbits.size())
        return;
    const SatOrbit &orb = satOrbits[selectedSatIndex];
    if (!meshRenderer.typeMesh((int)orb.typeIdx))
        return;
    const bool on = infoChrome.open && meshRendererInit && viewerSatIndex == selectedSatIndex;
    if (buildSelActionButton(inp, ui, idx * 4 + 0, "Info", kIconInfo, on, hovSelInfoBtn))
        openModelViewer((int)orb.typeIdx, satTypes[orb.typeIdx].name.c_str(), orb.altM, selectedSatIndex);
}

// ─── Follow mode UI (Phase 4e) ───────────────────────────────────────────────
// The "Go to" button (an eye icon, tooltip "Go to") next to "Info" / "Trace pass" for a selected
// satellite with a geometry model: it starts follow mode — fly to the satellite and ride along. The
// name is "Go to" because that is what the player is asking for; the mechanism is still follow mode
// everywhere else.
void SatelliteSim::buildFollowButton(const UIInput &inp, UIRenderer &ui, int idx)
{
    if (selectedSatIndex < 0 || selectedSatIndex >= (int)satOrbits.size() ||
        !meshRenderer.typeMesh((int)satOrbits[selectedSatIndex].typeIdx))
        return;
    const bool on = followActive && followSatIndex == selectedSatIndex;
    if (buildSelActionButton(inp, ui, idx * 4 + 1, "Go to", kIconEye, on, hovSelFollowBtn))
    {
        if (on)
            stopFollow();
        else
            startFollow(selectedSatIndex);
    }
}

// Top-centre chip while following: what, how far, the aim lock and the way back.
void SatelliteSim::buildFollowHud(const UIInput &inp, UIRenderer &ui)
{
    if (!followActive)
        return;
    static char distBuf[64];
    const double d = glm::length(followOffset);
    if (d < 1000.0)
        snprintf(distBuf, sizeof(distBuf), "%.1f m away", d);
    else
        snprintf(distBuf, sizeof(distBuf), "%.2f km away", d / 1000.0);
    auto chipButton = [&](const char *id, const char *label, bool &hov, const char *tip) {
        bool clicked = false;
        Clay_String ids{false, (int32_t)strlen(id), id};
        CLAY(CLAY_SID(ids), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED((float)fs(11) + 11.0f)},
                                        .padding = {8, 8, 0, 0},
                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                             .backgroundColor = hov ? Pal::btnHover : Pal::btnIdle,
                             .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hov);
            sndClick(n, inp.lmbPressed);
            hov = n;
            clicked = n && inp.lmbPressed;
            ui.tooltip(inp, n, tip, fs(11));
            Clay_String ls{false, (int32_t)strlen(label), label};
            CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        return clicked;
    };
    CLAY(CLAY_ID("FollowHud"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                           .padding = {12, 12, 6, 6},
                                           .childGap = 10,
                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                           .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                .backgroundColor = Pal::panelBgFade,
                                .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius),
                                .floating = {.offset = {0.0f, 12.0f}, .zIndex = 6,
                                             .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP, .parent = CLAY_ATTACH_POINT_CENTER_TOP},
                                             .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        Clay_String ls{false, (int32_t)strlen(followLabel), followLabel};
        CLAY_TEXT(ls, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12), .wrapMode = CLAY_TEXT_WRAP_NONE}));
        Clay_String ds{false, (int32_t)strlen(distBuf), distBuf};
        CLAY_TEXT(ds, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
        if (chipButton("FollowAimBtn", followAimLock ? "Aim: locked" : "Aim: free", hovFollowAim,
                       "Locked: the camera keeps the satellite centred while you move. Free: look around with RMB "
                       "(looking unlocks it)"))
            followAimLock = !followAimLock;
        if (chipButton("FollowExitBtn", "Exit", hovFollowExit, "Return to the ground where you were"))
            stopFollow();
    }
    captureLaidOut(ui, CLAY_ID("FollowHud"), inp.screenW * 0.5f - 200.0f, 12.0f, 400.0f, 34.0f);
}

// ─── buildTraceButton ────────────────────────────────────────────────────────
// "Trace pass" for the selected satellite — an icon (pixel--trace.png) + label in its floating panel,
// and in the corner chip when it is out of view or too faint to draw (the trace then finds its pass or
// its next one). Shown for every satellite: when one can't be traced (legacy type, ground-site aim) the
// window says why.
void SatelliteSim::buildTraceButton(const UIInput &inp, UIRenderer &ui, int idx)
{
    if (selectedSatIndex < 0 || selectedSatIndex >= (int)satOrbits.size())
        return;
    if (buildSelActionButton(inp, ui, idx * 4 + 2, "Trace pass", kIconTrace, false, hovSelTraceBtn))
        computeSelectedTrace();
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
                                        const std::function<void()> &buildBody,
                                        const std::function<void()> &titleExtras, bool titleHighlight)
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
        // Title bar height follows the UI scale; the title never wraps (a wrapped title spilled below
        // the bar into the body) and is clipped short of the close button instead.
        const float titleBarH = std::max(36.0f, (float)fs(16) + 16.0f);
        CLAY(CLAY_IDI("GenWinTitleBar", winId), {.layout = {
                                                     .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(titleBarH)},
                                                     .padding = {14, 14, 0, 0},
                                                     .childGap = 0,
                                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                 .backgroundColor = Pal::titleBar,
                                                 .cornerRadius = {Style::windowCornerRadius, Style::windowCornerRadius, 0, 0}})
        {
            {
                bool n = Clay_Hovered();
                // Title-bar icons (Select / Go to on the satellite windows) are buttons drawn INSIDE
                // the bar: a click on one of them must not also start a window drag.
                const bool overExtra = viewTitleIconsHovered(winId);
                if (n && inp.lmbPressed && !hovCloseFlag && !overExtra)
                    chrome.dragging = true;
            }

            CLAY(CLAY_IDI("GenWinTitleText", winId), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}},
                                                      .clip = {.horizontal = true}})
            {
                CLAY_TEXT(titleStr, CLAY_TEXT_CONFIG({.textColor = titleHighlight ? Pal::reticule : Pal::textPrimary,
                                                      .fontSize = fs(16), .wrapMode = CLAY_TEXT_WRAP_NONE}));
            }

            if (titleExtras)
                titleExtras();

            if (closable)
            {
                Clay_Color closeBg = hovCloseFlag ? Pal::closeBgHov : Pal::closeBgIdle;
                const float closeSz = std::max(24.0f, (float)fs(12) + 12.0f);
                CLAY(CLAY_IDI("GenWinCloseBtn", winId), {.layout = {
                                                             .sizing = {CLAY_SIZING_FIXED(closeSz), CLAY_SIZING_FIXED(closeSz)},
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
    // minW=680: the Photometry/Clouds sliders shrink responsively with the window
    // (settingsSliderWidth()) and only need ~500 (kSliderFixedLeft+kSliderFixedRight+
    // kSliderMinW), but the Controls tab's keybinding rows do NOT — each row is five
    // CLAY_SIZING_FIXED children (action label 130 + key readout 60 + rebind btn 120 +
    // gamepad readout 70 + gamepad rebind btn 90 = 470, +4 gaps*6 +8 padding = 502) that
    // Clay does not reflow/shrink, plus the 140px tab strip + 1px divider + 28px content
    // padding = ~671 total. 500 let a user resize below that and run those rows off the
    // right edge with no clip to catch it (SettingsContent's clip is vertical-only).
    // 680 covers Controls with a little slack; also clears the Display tab's preset
    // button row (kept single-line-safe by putting its own label on its own row above
    // the buttons — see buildSettingsDisplayTab).
    static char settingsTitleBuf[64];
    if (!settingsTitleBuf[0])
        snprintf(settingsTitleBuf, sizeof(settingsTitleBuf), "Settings — v%s (%s)", APP_VERSION, APP_GIT_COMMIT);
    bool justClosed = buildResizableWindow(inp, ui, settingsChrome, 0, settingsTitleBuf, true, hovSettingsClose,
                                           defaultX, defaultY, 680.0f, 420.0f, 1000.0f, 820.0f,
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
        // UC1 settings restructure: the Clouds/Ocean/Terrain/Aurora/Beams tabs are ~46 developer
        // sliders — hidden behind Display > "Show advanced settings" so a new user's front door
        // is the preset selector, not a wall of tuning knobs. hovTab[]/settingsActiveTab still
        // index by the tab's real (unchanged) id even while its button is skipped here, so
        // nothing about the other tabs' state needs remapping.
        for (int ti = 0; ti < 12; ++ti)
        {
            bool isAdvancedTab = (ti >= 6 && ti <= 10);
            if (isAdvancedTab && !showAdvancedSettings)
                continue;
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
            buildSettingsBeamsTab(inp, ui);
            break;
        case 11:
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
                {
                    c.enabled = !c.enabled;
                    // TargetedReflector mirrors carry no persisted state to invalidate any more
                    // (2026-08-06 reversibility rework) — re-enabling a constellation just resumes
                    // normal per-frame selection, no snap needed.
                }
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
            // ── View the type's 3D model (Phase 4 model viewer) ──
            {
                if (hovViewConst.size() != constellations.size())
                    hovViewConst.assign(constellations.size(), false);
                const bool hasMesh = meshRenderer.typeMesh((int)c.typeIdx) != nullptr;
                const bool hovView = hovViewConst[ci];
                CLAY(CLAY_IDI("ConstViewBtn", ci), {.layout = {
                                                        .sizing = {CLAY_SIZING_FIXED(38), CLAY_SIZING_FIXED(18)},
                                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                    .backgroundColor = hasMesh ? (hovView ? Pal::btnHover : Pal::btnIdle)
                                                                               : Pal::rowDisabled,
                                                    .cornerRadius = CLAY_CORNER_RADIUS(3)})
                {
                    bool n = Clay_Hovered();
                    if (hasMesh)
                    {
                        sndRollover(n, hovView);
                        sndClick(n, inp.lmbPressed);
                        if (n && inp.lmbPressed) // a real satellite of it, where it is now — and selected,
                        {                        // so the viewer always refers to the selection
                            const int pick = pickViewerSatellite((int)ci);
                            selectSatellite(pick);
                            // popOut: this caller asked for the VIEW, not the readout, so the 3D view
                            // opens as well as the satellite window.
                            openModelViewer((int)c.typeIdx, c.name.c_str(), c.altM, pick, true);
                        }
                    }
                    hovViewConst[ci] = n;
                    ui.tooltip(inp, n, hasMesh ? "Select one of this constellation's satellites (the one highest in your sky) "
                                                 "and view it in 3D where it is now - a quick way to find a station"
                                               : "No 3D model: this type uses the legacy two-surface model", fs(11));
                    CLAY_TEXT(CLAY_STRING("VIEW"), CLAY_TEXT_CONFIG({.textColor = hasMesh ? Pal::textPrimary : Pal::textHint,
                                                                   .fontSize = fs(10)}));
                }
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

    // ── Planets ────────────────────────────────────────────────────────────
    // Same ON/OFF toggle-row pattern as the constellation list above — no new UI pattern
    // invented (RELEASE_v1_1_PLAN.md follow-up, session 30). A global "Show planets" toggle
    // sits above the per-planet rows; per-planet rows still work when the global is off (so
    // preferences survive), they're just moot until it's back on.
    CLAY(CLAY_ID("PlanetsSectionGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(10)}}}) {}
    CLAY(CLAY_ID("PlanetsSectionLabel"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
    {
        CLAY_TEXT(CLAY_STRING("PLANETS"), CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
    }
    {
        Clay_Color showBg = showPlanets ? Pal::btnAccent : (hovShowPlanets ? Pal::btnAccentHv : Pal::btnIdle);
        CLAY(CLAY_ID("ShowPlanetsRow"), {.layout = {
                                             .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                             .padding = {4, 4, 3, 3},
                                             .childGap = 6,
                                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                             .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_ID("ShowPlanetsBtn"), {.layout = {
                                                 .sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(18)},
                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                             .backgroundColor = showBg,
                                             .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovShowPlanets);
                sndClick(n, inp.lmbPressed);
                hovShowPlanets = n;
                if (n && inp.lmbPressed)
                    showPlanets = !showPlanets;
                CLAY_TEXT(showPlanets ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(10)}));
            }
            CLAY(CLAY_ID("ShowPlanetsName"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
            {
                CLAY_TEXT(CLAY_STRING("Show planets"), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            }
        }
    }
    for (int pi = 0; pi < kPlanetCount; ++pi)
    {
        bool hov = hovPlanetBtn[pi];
        Clay_Color rowBg = planetEnabled[pi] ? Pal::rowEnabled : Pal::rowDisabled;
        CLAY(CLAY_IDI("PlanetRow", pi), {.layout = {
                                             .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                             .padding = {4, 4, 3, 3},
                                             .childGap = 6,
                                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                             .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                         .backgroundColor = rowBg,
                                         .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            Clay_Color btnBg = planetEnabled[pi] ? Pal::btnAccent : (hov ? Pal::btnAccentHv : Pal::btnIdle);
            CLAY(CLAY_IDI("PlanetBtn", pi), {.layout = {
                                                 .sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(18)},
                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                             .backgroundColor = btnBg,
                                             .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hov);
                sndClick(n, inp.lmbPressed);
                hovPlanetBtn[pi] = n;
                if (hov && inp.lmbPressed)
                    planetEnabled[pi] = !planetEnabled[pi];
                CLAY_TEXT(planetEnabled[pi] ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(10)}));
            }
            CLAY(CLAY_IDI("PlanetName", pi), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String nameStr{false, (int32_t)strlen(kPlanetNames[pi]), kPlanetNames[pi]};
                CLAY_TEXT(nameStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
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
    // The floating quick-reference window (buildViewControlsWindow) used to auto-open on
    // startup; user testing found almost nobody closed it, so demo footage was consistently
    // full of it sitting in the top-left. It no longer opens itself — this button is the only
    // way to bring it up now, and it lives here rather than on the HUD since a player reaching
    // for a control reminder is already in Settings looking at the live rebind list below.
    CLAY(CLAY_ID("OpenControlsWindowRow"), {.layout = {
                                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(32)},
                                                .padding = {4, 4, 4, 4},
                                                .childGap = 8,
                                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Quick-reference overlay"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("OpenControlsWindowSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color btnBg = viewControlsChrome.open ? Pal::btnAccent : (hovOpenControlsWindow ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("OpenControlsWindowBtn"), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(24)},
                                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                .backgroundColor = btnBg,
                                                .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovOpenControlsWindow);
            sndClick(n, inp.lmbPressed);
            hovOpenControlsWindow = n;
            if (n && inp.lmbPressed)
                viewControlsChrome.open = !viewControlsChrome.open;
            CLAY_TEXT(viewControlsChrome.open ? CLAY_STRING("Showing") : CLAY_STRING("Show window"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }
    CLAY(CLAY_ID("ControlsTabDiv"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                .padding = {0, 0, 4, 8}},
                                     .backgroundColor = {40, 40, 44, 255}}) {}

    static char kbKeyBuf[KB_COUNT][16];
    static char kbPadBuf[KB_COUNT][16];
    for (int ki = 0; ki < (int)keybindings.size() && ki < KB_COUNT; ++ki)
    {
        KeyBinding &kb = keybindings[ki];
        snprintf(kbKeyBuf[ki], sizeof(kbKeyBuf[ki]), "[%s]", keyDisplayName(kb.key));
        snprintf(kbPadBuf[ki], sizeof(kbPadBuf[ki]), "[%s]", kb.gpButton >= 0 ? gamepadButtonDisplayName(kb.gpButton) : "-");

        Clay_Color rowBg = (kb.listening || kb.listeningPad)
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
                    {
                        other.listening = false;
                        other.listeningPad = false;
                    }
                    kb.listening = true;
                }
                CLAY_TEXT(kb.listening ? CLAY_STRING("PRESS KEY") : CLAY_STRING("Rebind"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(10)}));
            }

            CLAY(CLAY_IDI("KbPad", ki), {.layout = {
                                             .sizing = {CLAY_SIZING_FIXED(70), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String padStr{false, (int32_t)strlen(kbPadBuf[ki]), kbPadBuf[ki]};
                Clay_Color padCol = kb.listeningPad
                                        ? Pal::listenKey
                                        : Pal::keyText;
                CLAY_TEXT(padStr,
                          CLAY_TEXT_CONFIG({.textColor = padCol, .fontSize = fs(13)}));
            }

            Clay_Color rebindPadBg = kb.listeningPad
                                         ? Pal::listenBtn
                                         : (hovRebindPad[ki] ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_IDI("KbRebindPad", ki), {.layout = {
                                                   .sizing = {CLAY_SIZING_FIXED(90), CLAY_SIZING_FIXED(20)},
                                                   .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                               .backgroundColor = rebindPadBg,
                                               .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovRebindPad[ki]);
                sndClick(n, inp.lmbPressed);
                hovRebindPad[ki] = n;
                if (hovRebindPad[ki] && inp.lmbPressed)
                {
                    for (auto &other : keybindings)
                    {
                        other.listening = false;
                        other.listeningPad = false;
                    }
                    kb.listeningPad = true;
                }
                CLAY_TEXT(kb.listeningPad ? CLAY_STRING("PRESS PAD") : CLAY_STRING("Bind Pad"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(10)}));
            }
        }
    }

    // ── Look-axis inversion ───────────────────────────────────────────────────
    // Applied where the mouse dmx/dmy and gamepad right-stick look deltas are consumed
    // (SatelliteSim.cpp / SatelliteSimUI.cpp look block). Persisted in settings.json's "controls".
    CLAY(CLAY_ID("InvertDiv"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                           .padding = {0, 0, 8, 6}},
                                .backgroundColor = {40, 40, 44, 255}}) {}
    CLAY_TEXT(CLAY_STRING("INVERT LOOK AXES"),
              CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(10)}));

    struct InvertRow
    {
        const char *label;
        bool *value;
        bool *hov;
    };
    InvertRow invertRows[] = {
        {"Mouse X (horizontal)", &invertMouseX, &hovInvertMouseX},
        {"Mouse Y (vertical)", &invertMouseY, &hovInvertMouseY},
        {"Controller X (horizontal)", &invertPadX, &hovInvertPadX},
        {"Controller Y (vertical)", &invertPadY, &hovInvertPadY},
    };
    int invIdx = 0;
    for (auto &row : invertRows)
    {
        CLAY(CLAY_IDI("InvertRow", invIdx), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                                 .padding = {4, 4, 2, 2},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            Clay_String lbl{false, (int32_t)strlen(row.label), row.label};
            CLAY_TEXT(lbl, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY(CLAY_IDI("InvertSpacer", invIdx), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_Color chkBg = *row.value ? Pal::btnAccent : (*row.hov ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_IDI("InvertChk", invIdx), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                 .backgroundColor = chkBg,
                                                 .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, *row.hov);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    *row.value = !*row.value;
                *row.hov = n;
                CLAY_TEXT(*row.value ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
            }
        }
        ++invIdx;
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
    // ── Graphics preset (UC1, RELEASE_v1_1_PLAN.md) ────────────────────────
    // The front door: a new user should see this before any of the ~46 developer sliders behind
    // "Show advanced settings" below. Custom has no button of its own — it's a status readout for
    // "you edited an advanced slider by hand", not something you click into. Label and buttons are
    // on separate rows (not one wide LEFT_TO_RIGHT row) deliberately: 5 buttons at 82px + gaps
    // already need ~450px, and cramming a scaled (uiScale up to 2x) text label into whatever's
    // left of a single row is exactly the kind of fixed-width overflow the settings window had —
    // stacking removes the collision instead of trying to out-guess the label's rendered width.
    CLAY(CLAY_ID("PresetSection"), {.layout = {
                                        .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                        .padding = {4, 4, 4, 4},
                                        .childGap = 4,
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        // static, not automatic: Clay stores this pointer and dereferences it in ui.record(),
        // after this frame's buildUI has returned. A stack buffer survives that only by luck
        // (Debug keeps it; Release reuses the frame and the label renders as garbage).
        static char presetLabelBuf[40];
        snprintf(presetLabelBuf, sizeof(presetLabelBuf), "Graphics preset%s",
                 graphicsPreset == GraphicsPreset::Custom ? " — Custom" : "");
        Clay_String presetLabelStr2{false, (int32_t)strlen(presetLabelBuf), presetLabelBuf};
        CLAY_TEXT(presetLabelStr2, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));

        CLAY(CLAY_ID("PresetButtonRow"), {.layout = {
                                              .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                              .childGap = 6,
                                              .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            // Display order is cheapest → most expensive; it is independent of the enum's
            // numeric order (Potato is enum value 6, appended after Custom, but shown first).
            static const char *kPresetLabels[6] = {"Potato", "Planetarium", "Low", "Medium", "High", "Ultra"};
            static const GraphicsPreset kPresetValues[6] = {
                GraphicsPreset::Potato, GraphicsPreset::Planetarium, GraphicsPreset::Low,
                GraphicsPreset::Medium, GraphicsPreset::High, GraphicsPreset::Ultra};
            for (int i = 0; i < 6; ++i)
            {
                bool isActive = graphicsPreset == kPresetValues[i];
                Clay_Color btnBg = isActive ? Pal::btnAccent : (hovPreset[i] ? Pal::btnHover : Pal::btnIdle);
                CLAY(CLAY_IDI("PresetBtn", i), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIXED(82), CLAY_SIZING_FIXED(22)},
                                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                .backgroundColor = btnBg,
                                                .cornerRadius = CLAY_CORNER_RADIUS(3)})
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovPreset[i]);
                    sndClick(n, inp.lmbPressed);
                    hovPreset[i] = n;
                    if (n && inp.lmbPressed && graphicsPreset != kPresetValues[i])
                        applyGraphicsPreset(kPresetValues[i]);
                    Clay_String presetLabelStr{false, (int32_t)strlen(kPresetLabels[i]), kPresetLabels[i]};
                    CLAY_TEXT(presetLabelStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
                }
            }
        }

        // Quick-access copy of the "Show beam pointing rays" checkbox (buildSettingsBeamsTab owns
        // the canonical one, next to the beam diagnostics). Placed here too (2026-08-06 user
        // request) so a low-end preset + beams-on "planetarium demo" combo — the reason this
        // debug view is graduating toward a real display mode — is a single-tab operation instead
        // of a tab switch. Both checkboxes drive the same showBeamDebugRays bool; only the hover
        // state is duplicated (hovBeamDebugRaysToggleQuick), since Clay hover is per-element.
        CLAY(CLAY_ID("BeamDebugRayRowQuick"), {.layout = {
                                                   .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                                   .padding = {0, 0, 2, 2},
                                                   .childGap = 8,
                                                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                   .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY_TEXT(CLAY_STRING("Show beam pointing rays"), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY(CLAY_ID("BeamDebugRaySpacerQuick"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_Color rayChkBgQuick = showBeamDebugRays ? Pal::btnAccent : (hovBeamDebugRaysToggleQuick ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_ID("BeamDebugRayChkQuick"), {.layout = {
                                                       .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                                       .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                   .backgroundColor = rayChkBgQuick,
                                                   .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovBeamDebugRaysToggleQuick);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed)
                    showBeamDebugRays = !showBeamDebugRays;
                hovBeamDebugRaysToggleQuick = n;
                CLAY_TEXT(showBeamDebugRays ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
            }
        }
    }

    // ── Replay Intro (UC3) ───────────────────────────────────────────────────
    CLAY(CLAY_ID("ReplayIntroRow"), {.layout = {
                                         .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                         .padding = {4, 4, 4, 4},
                                         .childGap = 8,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        Clay_Color replayBtnBg = hovReplayIntro ? Pal::btnHover : Pal::btnIdle;
        CLAY(CLAY_ID("ReplayIntroBtn"), {.layout = {
                                             .sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(22)},
                                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                         .backgroundColor = replayBtnBg,
                                         .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovReplayIntro);
            sndClick(n, inp.lmbPressed);
            hovReplayIntro = n;
            if (n && inp.lmbPressed && !showIntro)
            {
                // Rewind the playhead; introIsReplay suppresses the UC1 benchmark regardless of
                // how this playthrough ends (see finishIntro) — it's a one-shot first-run
                // decision, not something a replay should redo.
                showIntro = true;
                introElapsed = 0.0f;
                introCaptionIndex = 0;
                introBasisValid = false;
                introBenchMsSum = 0.0f;
                introBenchFrames = 0;
                introIsReplay = true;
            }
            ui.tooltip(inp, n, "Replay the cinematic intro", fs(11));
            CLAY_TEXT(CLAY_STRING("Replay Intro"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(11)}));
        }
    }

    // ── Play intro on startup (UC3 follow-up) ─────────────────────────────────
    // Same labeled-row + toggle pattern as "Show controls window on startup" below. Disabling
    // this does NOT touch the observer/camera position — that's already restored from
    // settings.json's own "observer"/"camera" blocks regardless, so turning the intro off simply
    // resumes at whatever location was last saved.
    CLAY(CLAY_ID("PlayIntroRow"), {.layout = {
                                       .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                       .padding = {4, 4, 4, 4},
                                       .childGap = 8,
                                       .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                       .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Play intro on startup"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("PlayIntroSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color chkBg = playIntroOnStartup ? Pal::btnAccent : (hovPlayIntroStartup ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("PlayIntroChk"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                       .backgroundColor = chkBg,
                                       .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovPlayIntroStartup);
            sndClick(n, inp.lmbPressed);
            hovPlayIntroStartup = n;
            if (n && inp.lmbPressed)
                playIntroOnStartup = !playIntroOnStartup;
            CLAY_TEXT(playIntroOnStartup ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }

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

    // Long-exposure trails: consolidated to a single ON/OFF control (turning OFF immediately hides
    // the trail — recordDraw()'s composite draw is itself gated on trailEnabled — and turning ON
    // always starts from a blank buffer via trailClearPending, so there is nothing separate to
    // "clear"). Lives as an icon button next to the Screenshot button (buildLeftHudPanel,
    // "TrailsBtn") and on the Star Trails hotkey (default F, KB_TOGGLE_TRAILS), not here — no
    // settings-window control for it. "Trail decay (s)"/"Trail gain" sliders (tuning, not the
    // on/off control itself) still live in the Photometry tab alongside flareGlowGain/flareStreakGain.

    // ── Frame limiter (NEW-7, RELEASE_v1_1_PLAN.md) ────────────────────────
    // MAILBOX was the old unconditional default, which runs the GPU flat out forever on a
    // laptop — fans, heat, and battery drain, a real comfort issue for exactly the low-end
    // audience this release targets. Defaults to V-Sync (FIFO).
    CLAY(CLAY_ID("FpsCapRow"), {.layout = {
                                    .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                    .padding = {4, 4, 4, 4},
                                    .childGap = 6,
                                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                    .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Frame limiter"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("FpsCapSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        static const char *kFpsCapLabels[5] = {"Off", "30", "60", "120", "V-Sync"};
        static const FpsCapMode kFpsCapValues[5] = {
            FpsCapMode::Off, FpsCapMode::Cap30, FpsCapMode::Cap60, FpsCapMode::Cap120, FpsCapMode::VSync};
        for (int i = 0; i < 5; ++i)
        {
            bool isActive = fpsCapMode == kFpsCapValues[i];
            Clay_Color btnBg = isActive ? Pal::btnAccent : (hovFpsCap[i] ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_IDI("FpsCapBtn", i), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(44), CLAY_SIZING_FIXED(22)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = btnBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovFpsCap[i]);
                sndClick(n, inp.lmbPressed);
                hovFpsCap[i] = n;
                if (n && inp.lmbPressed && fpsCapMode != kFpsCapValues[i])
                {
                    fpsCapMode = kFpsCapValues[i];
                    applyFpsCapMode();
                }
                Clay_String fpsCapLabelStr{false, (int32_t)strlen(kFpsCapLabels[i]), kFpsCapLabels[i]};
                CLAY_TEXT(fpsCapLabelStr,
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
            }
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

    // ── Show advanced settings (UC1) ────────────────────────────────
    // Reveals the Clouds/Ocean/Terrain/Aurora/Beams tabs (hidden from the tab bar above otherwise).
    CLAY(CLAY_ID("AdvancedToggleRow"), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                            .padding = {4, 4, 4, 4},
                                            .childGap = 8,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Show advanced settings"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
        CLAY(CLAY_ID("AdvancedToggleSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color advBg = showAdvancedSettings ? Pal::btnAccent : (hovAdvancedToggle ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("AdvancedToggleChk"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = advBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovAdvancedToggle);
            sndClick(n, inp.lmbPressed);
            hovAdvancedToggle = n;
            if (n && inp.lmbPressed)
            {
                showAdvancedSettings = !showAdvancedSettings;
                // A hidden tab can't be clicked back to, so bounce off it now rather than leave
                // its stale content showing behind a tab bar that no longer has a button for it.
                if (!showAdvancedSettings && settingsActiveTab >= 6 && settingsActiveTab <= 10)
                    settingsActiveTab = 4; // Display
            }
            CLAY_TEXT(showAdvancedSettings ? CLAY_STRING("On") : CLAY_STRING("Off"),
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

    // Order must match gpuMsSmoothed[]'s slot semantics — see VulkanContext::kTimestampCount
    // for the authoritative table, and savePerfSnapshot() below for the matching JSON keys.
    // "Beam cloud block (retired)" always reads ~0.00ms as of 2026-08-09 — that dispatch was
    // replaced by beam_self_march.comp, whose real cost now folds into "Orbit compute" instead
    // (see savePerfSnapshot()'s own comment on gpu_timing_ms for why the slot wasn't rewired).
    static const char *kPerfLabels[8] = {
        "Scene depth", "Beam cloud block (retired)", "Orbit compute", "Cloud march", "Flare compute", "Sky background draw", "Satellite + star draw", "UI overlay"};
    static char perfBufs[8][20];
    for (int pi = 0; pi < 8; ++pi)
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

    // ── CPU frame breakdown (2026-08-10) ────────────────────────────
    // Same instrument as the GPU rows above, for the other side of the frame. "Other" is the
    // wall-clock frame time minus everything measured here — present/vsync wait, driver submit,
    // App-side work, and any CPU block that doesn't have a bucket yet. A large "Other" means the
    // cost is somewhere this table doesn't look, which is itself the finding.
    CLAY(CLAY_ID("CpuPerfDiv"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                            .padding = {0, 0, 6, 4}},
                                 .backgroundColor = {30, 30, 32, 255}}) {}
    CLAY_TEXT(CLAY_STRING("CPU FRAME BREAKDOWN"),
              CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(11)}));
    {
        static const char *kCpuLabels[CPU_COUNT] = {
            "Build UI (Clay)", "Update positions", "Beam readback + cluster",
            "Update stars", "Light pollution dome", "Update planets"};
        static char cpuBufs[CPU_COUNT + 2][20];
        float measured = 0.0f;
        for (int ci = 0; ci < CPU_COUNT; ++ci)
            measured += cpuMsSmoothed[ci];
        for (int ci = 0; ci < CPU_COUNT; ++ci)
        {
            snprintf(cpuBufs[ci], sizeof(cpuBufs[ci]), "%.2f ms", cpuMsSmoothed[ci]);
            Clay_String lbl{false, (int32_t)strlen(kCpuLabels[ci]), kCpuLabels[ci]};
            Clay_String val{false, (int32_t)strlen(cpuBufs[ci]), cpuBufs[ci]};
            CLAY(CLAY_IDI("CpuPerfRow", ci), {.layout = {
                                                  .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                                  .padding = {4, 4, 2, 2},
                                                  .childGap = 8,
                                                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                  .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                CLAY_TEXT(lbl, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
                CLAY(CLAY_IDI("CpuPerfSpacer", ci), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                CLAY_TEXT(val, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
            }
        }
        // inp.dt is this frame's real wall clock; the buckets are last frame's. At steady state
        // that's the same number, and a one-frame skew is not worth a second smoothing chain.
        float other = std::max(0.0f, inp.dt * 1000.0f - gpuMsTotalSmoothed - measured);
        snprintf(cpuBufs[CPU_COUNT], sizeof(cpuBufs[CPU_COUNT]), "%.2f ms", other);
        Clay_String otherStr{false, (int32_t)strlen(cpuBufs[CPU_COUNT]), cpuBufs[CPU_COUNT]};
        CLAY(CLAY_ID("CpuPerfOtherRow"), {.layout = {
                                              .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                              .padding = {4, 4, 2, 2},
                                              .childGap = 8,
                                              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                              .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY_TEXT(CLAY_STRING("Other (present/submit/App)"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
            CLAY(CLAY_ID("CpuPerfOtherSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            CLAY_TEXT(otherStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        }
    }

    // ── Knockout profiling toggles ──────────────────────────────────
    // Each disables one shader block or one whole dispatch. Compare the bucket rows above with a
    // toggle on vs. off to read that block's isolated GPU cost directly, without a GPU capture
    // tool — or press "Run knockout sweep" below to have the app do the whole table automatically.
    // The bit/label/json-key table is kDebugToggles at the top of this file; the bit semantics are
    // documented in SatelliteSim.h's debugDisableMask comment.
    //
    // 512 and 1024 are the PRODUCER-side knockouts (they skip a whole dispatch in recordCompute);
    // every other bit disables a block inside a shader. 256 used to be producer-side too, back
    // when the shadow was its own 128x128 dispatch; it now gates the per-pixel shadow march
    // inside cloud_march.comp, so its cost shows up in that bucket.
    //
    // 1024 is the big one: the scene depth pass fills kNoSurfaceT when skipped, so nothing
    // occludes anything and the renderer reverts to its pre-unification occlusion behaviour.
    // That makes the entire shared-depth architecture a single A/B checkbox.
    CLAY_TEXT(CLAY_STRING("KNOCKOUT PROFILING (disables rendering correctness for cost isolation)"),
              CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(11)}));
    // kDebugToggles also drives the automated knockout sweep below — adding a row there adds a
    // checkbox here and a sweep step, both for free.
    for (int ti = 0; ti < kDebugToggleCount; ++ti)
    {
        bool on = (debugDisableMask & kDebugToggles[ti].bit) != 0u;
        Clay_String lblStr{false, (int32_t)strlen(kDebugToggles[ti].label), kDebugToggles[ti].label};
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
                if (n && inp.lmbPressed && !sweepActive) // a sweep owns the mask while it runs
                    debugDisableMask ^= kDebugToggles[ti].bit;
                hovDebugToggle[ti] = n;
                CLAY_TEXT(on ? CLAY_STRING("SKIP") : CLAY_STRING("ON"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
            }
        }
    }

    // (The Reflect-Orbital beam diagnostic readout and the "Show beam pointing rays" toggle used to
    // sit here, between the knockout toggles and Save Snapshot. Moved to the Beams tab 2026-08-06 —
    // they belong with the beam sliders they are used to interpret, not next to the GPU profiling
    // controls they only ever shared a tab with by accident of when they were written.)

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

    // ── Automated knockout sweep ─────────────────────────────────
    // One press measures every knockout bit in turn and writes a single record carrying the whole
    // per-effect cost table (see updateKnockoutSweep). The label doubles as the progress readout —
    // the sweep runs over several seconds and pauses sim time, so it needs to be obvious that it's
    // running and roughly how far along it is.
    {
        static char sweepBtnBuf[32];
        if (sweepActive)
            // +2 total steps: the baseline, every measured bit, and the trailing baseline re-measure.
            snprintf(sweepBtnBuf, sizeof(sweepBtnBuf), "Sweeping %d/%d...", sweepStep + 1, sweepBitCount + 2);
        else if (sweepDoneMsgTimer > 0.0f)
            snprintf(sweepBtnBuf, sizeof(sweepBtnBuf), "Sweep saved");
        else
            snprintf(sweepBtnBuf, sizeof(sweepBtnBuf), "Run knockout sweep");
        Clay_String sweepStr{false, (int32_t)strlen(sweepBtnBuf), sweepBtnBuf};
        CLAY(CLAY_ID("RunSweepRow"), {.layout = {
                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30)},
                                          .padding = {4, 4, 4, 4},
                                          .childGap = 8,
                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            Clay_Color sweepBg = (sweepActive || sweepDoneMsgTimer > 0.0f) ? Pal::btnAccent
                                 : hovRunSweep                             ? Pal::btnHover
                                                                           : Pal::btnIdle;
            CLAY(CLAY_ID("RunSweepBtn"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(180), CLAY_SIZING_FIXED(24)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = sweepBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hovRunSweep);
                sndClick(n, inp.lmbPressed);
                if (n && inp.lmbPressed && !sweepActive)
                    startKnockoutSweep();
                hovRunSweep = n;
                ui.tooltip(inp, n,
                           "Measures every knockout bit automatically (~15s, pauses time). "
                           "Hold the camera still. Writes one record with the full cost table.",
                           fs(11));
                CLAY_TEXT(sweepStr, CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(11)}));
            }
        }
    }

    // ── Reset to defaults (NEW-5) ───────────────────────────────────
    // Deletes settings.json from the user data directory and asks for a restart, rather than
    // resetting live members in place — simplest safe option given how many scattered fields
    // loadSettings/saveSettings enumerate (a live in-place reset would need a 4th hand-maintained
    // copy of that same field list, which is exactly the kind of permutation risk CLAUDE.md
    // warns about for CloudParams). Once UC1's preset system lands, "reset" becomes "apply the
    // auto-detected preset" instead and can act immediately without a restart.
    if (resetDefaultsMsgTimer > 0.0f)
        resetDefaultsMsgTimer = std::max(0.0f, resetDefaultsMsgTimer - inp.dt);
    CLAY(CLAY_ID("ResetDefaultsRow"), {.layout = {
                                           .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(30)},
                                           .padding = {4, 4, 4, 4},
                                           .childGap = 8,
                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                           .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        Clay_Color resetBtnBg = resetDefaultsMsgTimer > 0.0f ? Pal::btnAccent
                                : hovResetDefaults           ? Pal::btnHover
                                                             : Pal::btnIdle;
        CLAY(CLAY_ID("ResetDefaultsBtn"), {.layout = {
                                               .sizing = {CLAY_SIZING_FIXED(140), CLAY_SIZING_FIXED(24)},
                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                           .backgroundColor = resetBtnBg,
                                           .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovResetDefaults);
            sndClick(n, inp.lmbPressed);
            if (n && inp.lmbPressed)
            {
                std::error_code ec;
                std::filesystem::remove((std::filesystem::path(userDataDir_) / "settings.json"), ec);
                resetDefaultsMsgTimer = 3.0f;
            }
            hovResetDefaults = n;
            ui.tooltip(inp, n, "Delete saved settings and restore defaults on next launch", fs(11));
            CLAY_TEXT(resetDefaultsMsgTimer > 0.0f ? CLAY_STRING("Restart to apply") : CLAY_STRING("Reset to Defaults"),
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
// Label column width. Was 110, which the longest labels in use ("Mirror slew rate (deg/s)",
// "Beam near-field fade (m)" — 24 chars) overflowed at fs(12) even at uiScale 1.0, and which
// most of the two-word labels overflowed once uiScale went up (fs() scales the FONT; none of
// these layout constants scale with it). 150 fits every current label on one line at uiScale 1.0.
// Raising this costs slider width 1:1 via kSliderFixedLeft, but the slider is clamped to
// kSliderMaxW long before that matters: at the window's 680px minimum the slider still gets
// 680-315-138 = 227 of its 228 max.
static constexpr float kSliderLabelW = 150.0f;
// Minimum slider-row height. The row is FIT, not FIXED, at this value — a label that still wraps
// (long label + high uiScale) grows its row instead of spilling over the next one. It was FIXED,
// which is what made wrapped label text overlap the row below it and run past the bottom of the
// scroll content.
static constexpr float kSliderRowMinH = 28.0f;
static constexpr float kSliderFixedLeft = 140.0f + 1.0f + 14.0f + 4.0f + kSliderLabelW + 6.0f;        // tab strip+divider+pad+label+gap = 315
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
    static char photoBufs[33][12];
    PhotoParam photoParams[] = {
        {"Brightness", &brightnessScale, 0.05f, 20.0f, 0.25f, "%.2f", 0},
        {"Day suppress", &daySuppression, 5.0f, 5000.0f, 5.0f, "%.0f", 1},
        {"Mirror boost", &mirrorBoost, 50.0f, 1000.0f, 25.0f, "%.0f", 2},
        {"Vis threshold", &visThresh, 0.0001f, 0.1f, 0.0001f, "%.3f", 3},
        {"Hlgt flare", &highlightFlare, 0.01f, 1.0f, 0.01f, "%.2f", 4},
        {"Moon suppress", &moonSuppression, 0.0f, 500.0f, 5.0f, "%.0f", 5},
        {"Pollution gain", &lightPollutionGain, 0.0f, 100.0f, 0.1f, "%.2f", 6},
        {"Extinction", &extinctionCoeff, 0.0f, 1.0f, 0.02f, "%.2f", 7},
        {"Sunlit sky vis", &sunlitBgVisibility, 0.0f, 1.0f, 0.01f, "%.2f", 8},
        // Flare architecture overhaul: replaces the deleted per-pixel flareEntries loop with a
        // render-to-texture + blur/streak pipeline (see FlareSourcePC's comment in SatelliteSim.h).
        {"Flare glow gain", &flareGlowGain, 0.0f, 0.01f, 0.0005f, "%.2f", 9},
        {"Flare streak", &flareStreakGain, 0.0f, 1.0f, 0.02f, "%.2f", 10},
        // Dark-sky exposure gate (Milky Way / zodiacal / aurora) — deliberately separate from
        // "Pollution gain"/"Extinction" above so tuning those for star/satellite realism never
        // forces a dark-sky retune. Lo/Hi are the two ends of the sky-brightness ramp and
        // "City sky mag" is the value it ramps TO; the fade times ease the dome feeding it. See
        // the mwPollutionThresholdLo/darkSkyCityMag member comments (SatelliteSim.h) and
        // shaders/include/darksky.glsl.
        {"MW pollut. lo", &mwPollutionThresholdLo, 0.0f, 0.5f, 0.005f, "%.3f", 11},
        {"MW pollut. hi", &mwPollutionThresholdHi, 0.001f, 0.5f, 0.005f, "%.3f", 12},
        // mag/arcsec^2, so LOWER = brighter city sky = harsher suppression. 22 is pristine (the gate
        // does nothing); ~16 is already brighter than any real site. The floor goes to 1.0 anyway —
        // below the real-world range on purpose — as an artistic knob for fully washing dark-sky
        // features out under city glow. Default (SatelliteSim.h) is 13.0.
        {"City sky mag", &darkSkyCityMag, 1.0f, 22.0f, 0.1f, "%.1f", 13},
        // Twilight half of the same gate. "Twilight end" is the one to reach for first: it is the
        // solar depression at which the sky is considered fully dark, so it directly sets how long
        // after sunset dark-sky features come out (18 deg = real astronomical twilight).
        {"Twilight sky mag", &darkSkyTwilightMag0, 8.0f, 20.0f, 0.25f, "%.1f", 14},
        {"Twilight end (deg)", &darkSkyTwilightEndDeg, 0.0f, 30.0f, 0.5f, "%.1f", 15},
        {"Twilight aniso", &darkSkyTwilightAniso, 0.0f, 8.0f, 0.25f, "%.2f", 16},
        {"MW fade in (s)", &mwFadeInTimeS, 0.0f, 120.0f, 1.0f, "%.1f", 17},
        {"MW fade out (s)", &mwFadeOutTimeS, 0.0f, 60.0f, 0.5f, "%.1f", 18},
        // Long-exposure trail pipeline — "Long exposure trails" ON/OFF + "Clear Trail" live in the
        // Display tab (near "Render scale"); these two gains are siblings of flareGlowGain/
        // flareStreakGain just above, so they live in this same tab.
        {"Trail decay (s)", &trailDecaySeconds, 0.2f, 30.0f, 0.2f, "%.1f", 19},
        {"Trail gain", &trailCompositeGain, 0.0f, 5.0f, 0.05f, "%.2f", 20},
        // Datacenter flare mitigation tilt — see AttitudeMode::SunTrackingTilted (SatelliteSim.h)
        // and formatSelectedSatInfo's "Power output" readout. 0-45 deg: past ~45 deg the specular
        // lobe is pitched further from nadir than from zenith, so mitigation gains diminish while
        // the cos(tilt) power cost keeps climbing — not a hard physical limit, just past the
        // useful range for a gimbal-limited real panel.
        {"Flare mitigate tilt (deg)", &flareMitigationTiltDeg, 0.0f, 45.0f, 1.0f, "%.0f", 21},
        // Shared point-source model (point_style.glsl): satellites, stars and planets are all drawn
        // from their apparent magnitude through these, so equal magnitudes look equal.
        {"Point peak mag", &pointRefMag, -4.0f, 6.0f, 0.1f, "%.1f", 22},
        {"Point contrast", &pointGamma, 0.2f, 1.5f, 0.05f, "%.2f", 23},
        {"Point limit mag", &pointLimitMag, 3.0f, 12.0f, 0.1f, "%.1f", 24},
        {"Point size (px)", &pointSigmaPx, 0.2f, 2.0f, 0.05f, "%.2f", 25},
        {"Point max size (px)", &pointSigmaMaxPx, 1.0f, 20.0f, 0.5f, "%.1f", 26},
        // Zoom optics (SatelliteSim.h, opticsGainMag): zooming in gathers light like a telescope
        // whose aperture grows with magnification up to this; 7 mm = naked eye (no gain).
        {"Zoom aperture max (mm)", &zoomApertureMaxMm, 7.0f, 1000.0f, 1.0f, "%.0f", 27},
        // Per-satellite glare sprites (glare.vert/.frag): sharp rays + an anamorphic streak on the
        // brightest glints, over the broad "Flare" bloom above.
        {"Glare gain", &glareGain, 0.0f, 4.0f, 0.05f, "%.2f", 28},
        {"Glare size (px)", &glareSizePx, 4.0f, 200.0f, 2.0f, "%.0f", 29},
        {"Glare threshold", &glareThreshold, 0.0f, 3.5f, 0.05f, "%.2f", 30},
        {"Glare falloff", &glareFalloff, 0.5f, 8.0f, 0.1f, "%.1f", 31},
        {"Glare spikes", &glareSpikes, 3.0f, 32.0f, 1.0f, "%.0f", 32},
    };
    for (auto &pp : photoParams)
    {
        int pi = pp.idx;
        snprintf(photoBufs[pi], sizeof(photoBufs[pi]), pp.fmt, *pp.val);
        Clay_String valStr{false, (int32_t)strlen(photoBufs[pi]), photoBufs[pi]};
        float t = glm::clamp((*pp.val - pp.vmin) / (pp.vmax - pp.vmin), 0.0f, 1.0f);

        CLAY(CLAY_IDI("PhotoRow", pi), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(kSliderRowMinH)},
                                            .padding = {4, 4, 4, 4},
                                            .childGap = 6,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("PhotoLbl", pi), {.layout = {.sizing = {CLAY_SIZING_FIXED(kSliderLabelW), CLAY_SIZING_FIT(0)}}})
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

    // Occlusion between satellite parts (geometry-model types) — off by default, see
    // satPartOcclusion (SatelliteSim.h). Same labeled-row + toggle pattern as "Play intro on startup".
    CLAY(CLAY_ID("SatOcclusionRow"), {.layout = {
                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                          .padding = {4, 4, 4, 4},
                                          .childGap = 8,
                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Satellite part occlusion"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
        CLAY(CLAY_ID("SatOcclusionSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

        Clay_Color chkBg = satPartOcclusion ? Pal::btnAccent : (hovSatOcclusionChk ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("SatOcclusionChk"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = chkBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovSatOcclusionChk);
            sndClick(n, inp.lmbPressed);
            hovSatOcclusionChk = n;
            if (n && inp.lmbPressed)
                satPartOcclusion = !satPartOcclusion;
            ui.tooltip(inp, n, "Parts of a modelled satellite shadow and hide each other (costly at millions of satellites)", fs(11));
            CLAY_TEXT(satPartOcclusion ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }
    // Environment probes for satellite meshes (SatEnvProbes): the full sky renderer around each
    // satellite drawn as a model, for its reflections and the Earth light on its sides.
    CLAY(CLAY_ID("EnvReflRow"), {.layout = {
                                     .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                     .padding = {4, 4, 4, 4},
                                     .childGap = 8,
                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                     .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Full-renderer reflections"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
        CLAY(CLAY_ID("EnvReflSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
        Clay_Color chkBg = envReflections ? Pal::btnAccent : (hovEnvReflChk ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("EnvReflChk"), {.layout = {
                                         .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                     .backgroundColor = chkBg,
                                     .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovEnvReflChk);
            sndClick(n, inp.lmbPressed);
            hovEnvReflChk = n;
            if (n && inp.lmbPressed)
                envReflections = !envReflections;
            ui.tooltip(inp, n,
                       "Satellite models reflect, and are lit by, what the full sky renderer draws around them - "
                       "the Earth with its clouds, oceans and city lights, the aurora, the Moon and the Milky Way. "
                       "Off: a simpler analytic Earth (cheaper). The model viewer uses it for its background too",
                       fs(11));
            CLAY_TEXT(envReflections ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }
    // Sharp mirror reflections (SKY_REFL): the full renderer per pixel in mirror-smooth surfaces.
    CLAY(CLAY_ID("SharpReflRow"), {.layout = {
                                       .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                       .padding = {4, 4, 4, 4},
                                       .childGap = 8,
                                       .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                       .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Sharp mirror reflections"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
        CLAY(CLAY_ID("SharpReflSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
        Clay_Color chkBg = sharpReflections ? Pal::btnAccent : (hovSharpReflChk ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("SharpReflChk"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                       .backgroundColor = chkBg,
                                       .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovSharpReflChk);
            sndClick(n, inp.lmbPressed);
            hovSharpReflChk = n;
            if (n && inp.lmbPressed)
                sharpReflections = !sharpReflections;
            ui.tooltip(inp, n,
                       "Mirrors (a Reflect Orbital membrane, quartz radiators) reflect the full sky renderer pixel by "
                       "pixel, sharp at any distance, instead of a reflection map. Costs a sky render of the mirror's "
                       "area on screen, for the four largest such satellites in view. Needs full-renderer reflections",
                       fs(11));
            CLAY_TEXT(sharpReflections ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }
    buildBulkExportRows(inp, ui);
}

// ─── buildBulkExportRows (benchmarking M10) ──────────────────────────────────
// Photometry tab: source (the selected satellite or a constellation), window, cadence, and an
// Export / Cancel button with progress. The constraints are SatBench's defaults, stated in the UI.
void SatelliteSim::buildBulkExportRows(const UIInput &inp, UIRenderer &ui)
{
    // Pick up a finished worker's result (and join it) on the UI thread.
    if (!bulkRunning.load() && bulkThread.joinable())
    {
        bulkThread.join();
        std::lock_guard<std::mutex> lk(bulkMutex);
        snprintf(bulkStatus, sizeof(bulkStatus), "%s", bulkResult.c_str());
    }
    if (bulkSource >= (int)constellations.size())
        bulkSource = -1;
    if (bulkSource < 0)
        snprintf(bulkSourceBuf, sizeof(bulkSourceBuf), "Selected satellite");
    else
        snprintf(bulkSourceBuf, sizeof(bulkSourceBuf), "%s", constellations[bulkSource].name.c_str());
    snprintf(bulkWindowBuf, sizeof(bulkWindowBuf), bulkWindowIdx == 0 ? "Next 24 h" : "Next 7 days");
    snprintf(bulkCadenceBuf, sizeof(bulkCadenceBuf), "Every %.0f s", kBulkCadenceS[bulkCadenceIdx]);
    const bool running = bulkRunning.load();
    if (running)
        snprintf(bulkProgressBuf, sizeof(bulkProgressBuf), "Cancel (%.0f%%)", 100.0f * bulkProgress.load());

    auto label = [&](const char *s, Clay_Color c, float size) {
        Clay_String str{false, (int32_t)strlen(s), s};
        CLAY_TEXT(str, CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size)}));
    };
    // One labelled row with a button; returns true when the button was clicked.
    auto row = [&](int id, const char *name, const char *value, bool &hov, const char *tip) {
        bool clicked = false;
        CLAY(CLAY_IDI("BulkRow", id), {.layout = {
                                           .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                           .padding = {4, 4, 4, 4},
                                           .childGap = 8,
                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                           .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("BulkRowLbl", id), {.layout = {.sizing = {CLAY_SIZING_FIXED(kSliderLabelW), CLAY_SIZING_FIT(0)}}})
            {
                label(name, Pal::volLabel, 12);
            }
            CLAY(CLAY_IDI("BulkRowBtn", id), {.layout = {
                                                  .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(22)},
                                                  .padding = {10, 10, 0, 0},
                                                  .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                              .backgroundColor = hov ? Pal::btnHover : Pal::btnIdle,
                                              .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                bool n = Clay_Hovered();
                sndRollover(n, hov);
                sndClick(n, inp.lmbPressed);
                hov = n;
                clicked = n && inp.lmbPressed;
                ui.tooltip(inp, n, tip, fs(11));
                label(value, Pal::btnLabel, 11);
            }
        }
        return clicked;
    };

    CLAY(CLAY_ID("BulkHeader"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}, .padding = {4, 4, 12, 2}}})
    {
        label("BULK EXPORT  (samples CSV, SatBench schema)", Pal::textDim, 11);
    }
    if (row(0, "Source", bulkSourceBuf, hovBulkSource, "Click to cycle: the selected satellite, or a whole constellation") &&
        !running)
        bulkSource = bulkSource + 1 >= (int)constellations.size() ? -1 : bulkSource + 1;
    if (row(1, "Window", bulkWindowBuf, hovBulkWindow, "From the current sim time") && !running)
        bulkWindowIdx = (bulkWindowIdx + 1) % 2;
    if (row(2, "Cadence", bulkCadenceBuf, hovBulkCadence, "Time between samples within a pass") && !running)
        bulkCadenceIdx = (bulkCadenceIdx + 1) % 3;
    if (row(3, "Export", running ? bulkProgressBuf : "Export CSV", hovBulkGo,
            running ? "Stop the export" : "Sample every visible pass for the current observer"))
        startBulkExport();
    CLAY(CLAY_ID("BulkNotes"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                           .padding = {4, 4, 2, 2},
                                           .childGap = 2,
                                           .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        label("Constraints (SatBench defaults): Sun -18..-6 deg, elevation >= 20 deg, fully sunlit.", Pal::textHint, 11);
        if (bulkStatus[0])
            label(bulkStatus, Pal::textDim, 11);
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
    // Was [46] — undersized again relative to the live idx range (idx 61/62, "Opacity scale" and
    // "City light blur LOD"), same class of bug as the [33]->[46] fix documented in
    // feedback_cloud_slider_arrays memory: an OOB static-array write that doesn't crash, it just
    // silently corrupts a neighboring slider's display text — reported as "Opacity scale has a
    // bugged display, can't see what value is selected." Must stay >= (highest idx in use) + 1,
    // same as hovCloudMinus/hovCloudPlus/draggingCloud above.
    static char cloudBufs[91][16];

    for (int si = 0; si < count; ++si)
    {
        CloudSlider &cs = sliders[si];
        int ci = cs.idx;
        snprintf(cloudBufs[ci], sizeof(cloudBufs[ci]), cs.fmt, *cs.val);
        Clay_String valStr{false, (int32_t)strlen(cloudBufs[ci]), cloudBufs[ci]};
        float t = glm::clamp((*cs.val - cs.vmin) / (cs.vmax - cs.vmin), 0.0f, 1.0f);

        CLAY(CLAY_IDI("CloudRow", ci), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(kSliderRowMinH)},
                                            .padding = {4, 4, 4, 4},
                                            .childGap = 6,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_IDI("CloudLbl", ci), {.layout = {.sizing = {CLAY_SIZING_FIXED(kSliderLabelW), CLAY_SIZING_FIT(0)}}})
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
                    graphicsPreset = GraphicsPreset::Custom; // UC1: any advanced-tab edit leaves a preset
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
                {
                    *cs.val = glm::clamp(*cs.val - cs.step, cs.vmin, cs.vmax);
                    graphicsPreset = GraphicsPreset::Custom;
                }
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
                {
                    *cs.val = glm::clamp(*cs.val + cs.step, cs.vmin, cs.vmax);
                    graphicsPreset = GraphicsPreset::Custom;
                }
                CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
            }
        }
    }
}

// ─── buildCloudSliderSections ────────────────────────────────────────────────
// Collapsible category headers wrapping buildCloudSliderRows. The Clouds tab had grown to ~47
// sliders in one flat list — long past the point where scrolling to a known knob was faster than
// reading every label on the way there. Sections are pure presentation: each one just gates
// whether its slice of the array gets rendered this frame, so every slider keeps its original
// global `idx` (and therefore its draggingCloud/hovCloudMinus/hovCloudPlus/cloudBufs slots)
// regardless of which category it is filed under. Regrouping is free; renumbering is never needed.
//
// Collapse state is deliberately NOT persisted to settings.json — it is transient view state, not
// a preference, and every section starts collapsed so the tab opens as a short list of categories.
void SatelliteSim::buildCloudSliderSections(const UIInput &inp, UIRenderer &ui,
                                            CloudSliderSection *sections, int count)
{
    static char sectCountBufs[kCloudSectionSlots][8];
    for (int si = 0; si < count && si < kCloudSectionSlots; ++si)
    {
        CloudSliderSection &sec = sections[si];
        bool open = cloudSectionOpen[si];
        snprintf(sectCountBufs[si], sizeof(sectCountBufs[si]), "%d", sec.count);
        Clay_String cntStr{false, (int32_t)strlen(sectCountBufs[si]), sectCountBufs[si]};

        CLAY(CLAY_IDI("CloudSectHdr", si), {.layout = {
                                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                                .padding = {8, 10, 0, 0},
                                                .childGap = 8,
                                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                            .backgroundColor = hovCloudSection[si] ? Pal::btnHover : Pal::sectionHdr,
                                            .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovCloudSection[si]);
            sndClick(n, inp.lmbPressed);
            hovCloudSection[si] = n;
            if (n && inp.lmbPressed)
                cloudSectionOpen[si] = !open;

            // "-"/"+" rather than a chevron glyph: the font atlas bakes ASCII 32-126 only
            // (see CLAUDE.md, "Font atlas is a fixed-size bitmap") — a Unicode triangle would
            // render as a missing-glyph box.
            CLAY(CLAY_IDI("CloudSectMark", si), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(10), CLAY_SIZING_FIT(0)},
                                                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(open ? CLAY_STRING("-") : CLAY_STRING("+"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(12)}));
            }
            CLAY(CLAY_IDI("CloudSectTitle", si), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
            {
                Clay_String tStr{false, (int32_t)strlen(sec.title), sec.title};
                CLAY_TEXT(tStr, CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(12)}));
            }
            CLAY_TEXT(cntStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(11)}));
        }

        if (cloudSectionOpen[si])
            buildCloudSliderRows(inp, ui, sec.sliders, sec.count);
    }
}

// ─── buildSettingsCloudsTab ──────────────────────────────────────────────────
// Sliders are grouped into collapsible categories (buildCloudSliderSections). Grouping is by what
// a knob *does* to the render, not by which shader it lands in — a user hunting for "why are my
// clouds too dark at sunset" should find every relevant knob in one place.
void SatelliteSim::buildSettingsCloudsTab(const UIInput &inp, UIRenderer &ui)
{
    // Bulk / layer geometry — how much cloud there is and where the deck sits.
    CloudSlider secCoverage[] = {
        {"Coverage", &cloudCoverage, 0.0f, 1.0f, 0.05f, "%.2f", 0},
        {"Density", &cloudDensity, 0.1f, 10.0f, 0.1f, "%.1f", 1},
        {"L0 alt (m)", &cloudBaseAltM, 100.0f, 6000.0f, 100.0f, "%.0f", 2},
        {"L1 alt (m)", &cloudTopAltM, 4000.0f, 15000.0f, 250.0f, "%.0f", 3},
        {"Base variance", &cloudBaseVariance, 0.0f, 1.0f, 0.05f, "%.2f", 34},
        {"Drift (1e-6)", &cloudDriftRate, 0.0f, 20e-6f, 0.5e-6f, "%.1e", 4},
        // `density` alone can't push a saturated cloud column past full opacity (per-sample
        // density is clamped to [0,1] before extinction is derived from it). This boosts
        // extinction only once a ray has already accumulated real depth (see cloudMarchCS's
        // coreBoost comment) — thin edges/wisps are unaffected, so pushing this high solidifies
        // genuinely thick decks (city lights at night, the sun disc, should fully disappear
        // under one) without flattening cloud silhouettes into hard-edged blobs. Wider range than
        // a flat multiplier would tolerate, precisely because edges no longer pay for it.
        {"Opacity scale", &cloudOpacityScale, 0.2f, 15.0f, 0.2f, "%.1f", 61},
    };

    // Silhouette / noise detail — what an individual cloud looks like up close.
    CloudSlider secShape[] = {
        {"Erosion (edge)", &cloudErosionEdge, 0.0f, 1.0f, 0.05f, "%.2f", 35},
        {"Erosion (core)", &cloudErosionCore, 0.0f, 1.0f, 0.05f, "%.2f", 36},
        {"Erosion billow", &cloudErosionBillow, 0.0f, 1.0f, 0.05f, "%.2f", 68},
        {"Billow height", &cloudErosionBillowH, 0.0f, 1.0f, 0.05f, "%.2f", 69},
        {"Erosion freq", &cloudErosionFreq, 0.5f, 6.0f, 0.1f, "%.2f", 70},
        {"Surface carve", &cloudSurfaceCarve, 0.0f, 1.0f, 0.05f, "%.2f", 67},
        // Domain-warp shear. The noise field folds (pinched/banded/"wavy chip" clouds) once
        // strength * 2 * freq / 480 exceeds ~1 — see cloud_params.glsl. Strength is how far
        // cloud structure is displaced; frequency is how fast that displacement varies. Want
        // big movement without pinching? Raise strength AND lower frequency.
        {"Warp strength", &cloudWarpStrength, 0.0f, 64.0f, 1.0f, "%.0f", 65},
        {"Warp frequency", &cloudWarpFreq, 0.5f, 12.0f, 0.25f, "%.2f", 66},
    };

    // Direct sun + ambient response, including the twilight-band falloff that governs how clouds
    // colour through sunset. Shadowing/self-occlusion is its own section below.
    CloudSlider secLighting[] = {
        {"Sun gain (horizon)", &cloudSunGain, 0.0f, 8.0f, 0.1f, "%.2f", 5},
        {"Sun gain (zenith)", &cloudSunGainZenith, 0.0f, 8.0f, 0.1f, "%.2f", 37},
        {"Sun gain elev band", &sunGainElevBand, 0.02f, 1.0f, 0.01f, "%.2f", 47},
        {"Ambient", &cloudAmbientGain, 0.0f, 20.0f, 0.05f, "%.2f", 6},
        {"Twilight ambient", &cloudTwilightAmbientGain, 0.0f, 20.0f, 0.05f, "%.2f", 33},
        {"Twilight band hi", &twilightBandHi, -0.1f, 0.8f, 0.01f, "%.2f", 48},
        {"Twilight band lo", &twilightBandLo, -0.9f, 0.0f, 0.01f, "%.2f", 49},
        {"HG g", &cloudHgG, 0.0f, 0.99f, 0.05f, "%.2f", 7},
        {"Multi-scatter", &cloudMultiScatter, 0.0f, 1.0f, 0.05f, "%.2f", 71},
    };

    // Self-shadowing and the shadow clouds cast down onto terrain/ocean/city lights.
    CloudSlider secShadow[] = {
        {"Shadow max dist (m)", &cloudShadowMaxDistM, 1000.0f, 6000000.0f, 1000.0f, "%.0f", 16},
        {"Shadow cone len", &cloudConeLenScale, 0.25f, 4.0f, 0.25f, "%.2f", 74},
        {"Shadow floor", &cloudShadowFloorT, 0.0f, 0.3f, 0.01f, "%.3f", 72},
        {"Sunset shadow", &cloudGrazeShadow, 0.0f, 1.0f, 0.05f, "%.2f", 73},
        {"Vert shade gain", &cloudVertShadeGain, 0.0f, 1.0f, 0.05f, "%.2f", 75},
        {"Density AO", &cloudDensityAO, 0.0f, 1.0f, 0.05f, "%.2f", 76},
        {"Density AO power", &cloudAOPower, 0.05f, 4.0f, 0.05f, "%.2f", 77},
        // A correct opacity value still looks wrong if what leaks through a hazy/thin cloud is a
        // pixel-sharp copy of the raw city-lights texture — real light diffuses through cloud
        // droplets. Blends earthNightTex/cityNightDetailTex toward this mip LOD as local cloud
        // opacity rises (0 = no blur, higher = softer glow). See sat_sky.frag's terrain branch.
        {"City light blur LOD", &cityLightBlurLod, 0.0f, 20.0f, 0.5f, "%.1f", 62},
    };

    // The flat 2D cloud paste that the volumetric march crossfades into with distance, plus the
    // crossfade range itself — these only matter together, so they share a section.
    CloudSlider secDistance[] = {
        {"Render dist (m)", &cloudMaxRenderDistM, 20000.0f, 800000.0f, 10000.0f, "%.0f", 17},
        {"Cloud 3D fade start (m)", &cloudDistFadeStartM, 5000.0f, 800000.0f, 5000.0f, "%.0f", 53},
        {"Cloud 3D fade end (m)", &cloudDistFadeEndM, 10000.0f, 2000000.0f, 10000.0f, "%.0f", 54},
        {"Coverage mip", &coverageMipLod, 0.0f, 6.0f, 0.25f, "%.2f", 50},
        {"Flat coverage scale", &flatCoverageScale, 0.1f, 2.0f, 0.01f, "%.2f", 51},
        {"Flat sun gain scale", &flatSunGainScale, 0.1f, 10.0f, 0.05f, "%.2f", 52},
        {"2D density scale", &flatDensityScale, 0.1f, 8.0f, 0.1f, "%.2f", 78},
        // Flat-layer-only Rayleigh multiplier, stacked on the global "Rayleigh gain" in the
        // Atmospheric scattering section. The flat paste and the volumetric march respond to
        // Rayleigh completely differently (closed-form double multiply vs. per-step transmittance
        // accumulation), so this is what closes the hue/depth step across the crossfade above.
        {"2D Rayleigh gain", &flatRayleighGain, 0.0f, 4.0f, 0.05f, "%.2f", 79},
        // The flat layer's share of the twilight sky ambient, on top of the shared "Twilight
        // ambient" slider in the Lighting section — so that one still moves both paths together
        // and this only sets the ratio between them. 0 = flat clouds go dark through twilight the
        // way they did before this term existed.
        {"2D twilight ambient", &flatTwilightAmbientGain, 0.0f, 4.0f, 0.05f, "%.2f", 80},
    };

    CloudSlider secCirrus[] = {
        {"Cirrus wind (deg)", &cloudCirrusWindDeg, 0.0f, 360.0f, 5.0f, "%.0f", 10},
        {"Cirrus stretch", &cloudCirrusStretch, 1.0f, 10.0f, 0.5f, "%.1f", 11},
    };

    // C11 ground fog layer (fogMarchCS, cloud_march.comp) — real volumetric mist shell with
    // sun/beam godrays. First-pass defaults, expect retuning once seen in-app.
    CloudSlider secFog[] = {
        {"Fog top altitude (m)", &fogTopAltM, 50.0f, 200000.0f, 50.0f, "%.0f", 55},
        {"Fog density", &fogDensity, 0.0f, 10.0f, 0.1f, "%.1f", 56},
        {"Fog coverage", &fogCoverage, 0.0f, 1.0f, 0.05f, "%.2f", 57},
        {"Fog sun gain", &fogSunGain, 0.0f, 8.0f, 0.1f, "%.2f", 58},
    };

    // Atmospheric scattering strength — scales the physical Rayleigh/Mie coefficients shared
    // by the sky atmosphere, clouds, cirrus, fog, terrain ambient, ocean reflection, and moon/
    // sun attenuation (see common.glsl's BETA_R_BASE/BETA_M_BASE and cloud_params.glsl's
    // atmosRayleighGain/atmosMieGain). 1.0 = original hardcoded behavior. Rayleigh gain
    // controls how much red/orange the horizon and sunsets pick up; Mie gain controls how
    // much wavelength-neutral haze dilutes that color back toward white/grey. Not cloud-specific,
    // but it lives here because it is tuned against the cloud/sunset look more than anything else.
    CloudSlider secAtmos[] = {
        {"Rayleigh gain", &atmosRayleighGain, 0.0f, 3.0f, 0.05f, "%.2f", 63},
        {"Mie/haze gain", &atmosMieGain, 0.0f, 3.0f, 0.05f, "%.2f", 64},
        // Orbital terminator gate — artistic suppression of scattered sunlight past the
        // terminator, inert below 40 km observer altitude. Strength 0 is an exact A/B against the
        // previous look. Width is the rolloff half-width in sin(sun elevation): 0.08 puts solar
        // zenith 92 about 23x down, 0.035 is effectively a hard cliff. See cloud_params.glsl.
        {"Terminator cut", &atmosTermStrength, 0.0f, 1.0f, 0.05f, "%.2f", 81},
        {"Terminator width", &atmosTermWidth, 0.01f, 0.40f, 0.005f, "%.3f", 82},
    };

    // Sample budgets — the only two knobs here that trade image quality directly against GPU cost.
    CloudSlider secQuality[] = {
        {"March steps", &cloudMarchSteps, 4.0f, 1024.0f, 4.0f, "%.0f", 8},
        {"Light steps", &cloudLightSteps, 1.0f, 16.0f, 1.0f, "%.0f", 9},
    };

#define CLOUD_SEC(title, arr) {title, arr, (int)(sizeof(arr) / sizeof((arr)[0]))}
    CloudSliderSection sections[] = {
        CLOUD_SEC("Coverage & layers", secCoverage),
        CLOUD_SEC("Shape & noise", secShape),
        CLOUD_SEC("Lighting", secLighting),
        CLOUD_SEC("Shadowing & AO", secShadow),
        CLOUD_SEC("Distance & 2D falloff", secDistance),
        CLOUD_SEC("Cirrus", secCirrus),
        CLOUD_SEC("Ground fog", secFog),
        CLOUD_SEC("Atmospheric scattering", secAtmos),
        CLOUD_SEC("Quality / performance", secQuality),
    };
#undef CLOUD_SEC
    buildCloudSliderSections(inp, ui, sections, (int)(sizeof(sections) / sizeof(sections[0])));
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
        {"Cloud shadow range (m)", &cloudShadowRangeM, 5000.0f, 300000.0f, 5000.0f, "%.0f", 38},
        // S4 (RELEASE_v1_1_PLAN.md): terrain-relief march distance fade — see cloud_params.glsl.
        {"Terrain fade start (m)", &terrainDistFadeStartM, 50000.0f, 1000000.0f, 10000.0f, "%.0f", 59},
        {"Terrain fade end (m)", &terrainDistFadeEndM, 100000.0f, 4000000.0f, 25000.0f, "%.0f", 60},
    };
    buildCloudSliderRows(inp, ui, sliders, (int)(sizeof(sliders) / sizeof(sliders[0])));
}

// ─── buildSettingsBeamsTab ───────────────────────────────────────────────────
// Reflect-Orbital mirror beams (C12), split out of Terrain 2026-08-06. They had accumulated there
// for no reason beyond the ground spot landing on terrain — the sliders tune a satellite
// subsystem, not the ground, and by the time there were seven of them they were the majority of a
// tab whose name gave no hint they existed. The live beam-count readout and the pointing-ray debug
// toggle came over from Display at the same time so everything beam-related is in one place.
//
// Slider `idx` values are unchanged by the move (39/41-46) — they are global ids indexing
// hovCloudMinus/hovCloudPlus/draggingCloud/cloudBufs, not per-tab positions. See
// [[feedback_cloud_slider_arrays]]: those four arrays are sized against the id space, so moving a
// row between tabs is free but adding one is not.
void SatelliteSim::buildSettingsBeamsTab(const UIInput &inp, UIRenderer &ui)
{
    CloudSlider sliders[] = {
        {"Beam gain", &beamGain, 0.0f, 1.0f, 0.0001f, "%.3f", 39},
        // 2026-08-06 same-day follow-up: reuses slot 40, freed by C12 follow-up #34's removed
        // "Beam footprint (m)" slider (see [[feedback_cloud_slider_arrays]] — no array resize
        // needed, this is filling an already-accounted-for index). Real angular-rate cap for the
        // TargetedReflector orientation ease — see sat_orbit.comp's TargetedReflector block.
        {"Mirror max slew rate (deg/s)", &mirrorMaxRateDegPerSec, 0.001f, 1.0f, 0.001f, "%.3f", 40},
        {"Beam max range (m)", &beamMaxRangeM, 50000.0f, 2000000.0f, 50000.0f, "%.0f", 41},
        {"Beam sky glow gain", &beamSkyGlowGain, 0.0f, 0.05f, 0.001f, "%.3f", 42},
        // 2026-08-06 reversibility rework: replaces the old rate-limited "Mirror slew rate
        // (deg/s)" slider — target lock is now a fixed sim-time window instead of a persisted
        // per-satellite lock, so the tunable is a duration, not a rate. Same slot (43).
        {"Target lock window (s)", &reflectorLockWindowS, 10.0f, 300.0f, 5.0f, "%.0f", 43},
        // S1 follow-up (RELEASE_v1_1_PLAN.md): reuses slot 44, freed by C12 follow-up #44's
        // removed "Beam extinction" slider — see that removal's comment (still accurate re: why
        // the slot was empty; this is the first thing to reuse it).
        {"Min beam elevation (deg)", &reflectorMinElevDeg, 0.0f, 60.0f, 1.0f, "%.0f", 44},
        {"Beam glow bleed gain", &beamGlowBleedGain, 0.0f, 0.01f, 0.0001f, "%.2f", 45},
        {"Beam near-field fade (m)", &beamNearFieldFadeM, 1000.0f, 500000.0f, 1000.0f, "%.0f", 46},
        // 2026-08-09: new slot (83) — see [[feedback_cloud_slider_arrays]], hovCloudMinus/Plus/
        // draggingCloud/cloudBufs all resized to 84 for this one. 2026-08-12: the member's meaning
        // changed (it is now the angular SIZE of a fixed direction bucket rather than a
        // running-average merge tolerance) but the label, slot, range and direction of effect are
        // all unchanged, so nothing here needed touching — see the member's own comment.
        {"Beam cluster direction threshold (deg)", &beamClusterDirThresholdDeg, 1.0f, 90.0f, 1.0f, "%.0f", 83},
        // 2026-08-12: new slots (86/87) — hovCloudMinus/Plus/draggingCloud/cloudBufs all resized
        // 86 -> 88 for these two, per [[feedback_cloud_slider_arrays]]. Cross-frame fade for the
        // cloud-light list, which only became possible once lights got stable identities; see
        // TrackedBeamLight in SatelliteSim.h.
        {"Beam light fade in (s)", &beamClusterFadeInS, 0.0f, 3.0f, 0.05f, "%.2f", 86},
        {"Beam light fade out (s)", &beamClusterFadeOutS, 0.0f, 6.0f, 0.05f, "%.2f", 87},
    };
    buildCloudSliderRows(inp, ui, sliders, (int)(sizeof(sliders) / sizeof(sliders[0])));

    // ── Reflect-Orbital beam diagnostic (C12) ────────────────────────
    // lastActiveBeamCount/lastNearestBeamDistM are read back from reflectBeamsBuf each frame
    // (one-frame-stale, same idiom as peakMagnitude) — a quick way to tell "is anything being
    // written at all" and "how far is the nearest one" apart from the render itself, since a
    // beam's target being merely tens of km away can still be too far to notice visually.
    {
        static char beamCountBuf[24];
        static char beamDistBuf[24];
        snprintf(beamCountBuf, sizeof(beamCountBuf), "%d", lastActiveBeamCount);
        if (lastNearestBeamDistM >= 0.0f)
            snprintf(beamDistBuf, sizeof(beamDistBuf), "%.1f km", lastNearestBeamDistM / 1000.0f);
        else
            snprintf(beamDistBuf, sizeof(beamDistBuf), "none");
        CLAY(CLAY_ID("BeamDiagRow"), {.layout = {
                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                          .padding = {4, 4, 2, 2},
                                          .childGap = 8,
                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY_TEXT(CLAY_STRING("Active beams / nearest"), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY(CLAY_ID("BeamDiagSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_String countStr{false, (int32_t)strlen(beamCountBuf), beamCountBuf};
            Clay_String distStr{false, (int32_t)strlen(beamDistBuf), beamDistBuf};
            CLAY_TEXT(countStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
            CLAY_TEXT(CLAY_STRING(" / "), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY_TEXT(distStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        }
    }

    // ── Cloud-light pool occupancy (2026-08-12) ────────────────────────
    // Live tracked lights in each reserved pool, against their caps. Directly answers the two
    // questions the 2026-08-11 revert left open: is the cluster count sitting near the real active
    // TARGET count (healthy — identities are being recognized frame to frame) or pinned at the cap
    // (churn: entries respawning instead of matching, which is what made that attempt expensive),
    // and are transiting beams actually getting slots. A count that climbs well past the number of
    // sites in view and stays there means fade-out is holding entries longer than it should — turn
    // "Beam light fade out (s)" down.
    {
        static char lightPoolBuf[48];
        snprintf(lightPoolBuf, sizeof(lightPoolBuf), "%d/%d clusters, %d/%d individual",
                 lastClusterLightCount, kMaxClusterCloudLights,
                 lastIndividualLightCount, kMaxIndividualCloudLights);
        CLAY(CLAY_ID("BeamLightPoolRow"), {.layout = {
                                               .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                               .padding = {4, 4, 2, 2},
                                               .childGap = 8,
                                               .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                               .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY_TEXT(CLAY_STRING("Cloud lights"), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY(CLAY_ID("BeamLightPoolSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_String poolStr{false, (int32_t)strlen(lightPoolBuf), lightPoolBuf};
            CLAY_TEXT(poolStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        }
    }

    // ── beam_self_march.comp occlusion debug (2026-08-09, BEAM_CLOUD_PLAN.md) ────────
    // Raw min/max/avg blockOpacity across every active beam this frame, and how many currently
    // read as occluded (>0.1) — no per-target aggregation in the way, unlike what the ray/ground
    // spot/cloud-lighting consumers each apply on top. Max stuck at 0.00 with visible cloud cover
    // on screen means the MARCH itself isn't finding cloud; Max >0 but no visible change means the
    // bug is in a consumer instead. See that shader's header and the memory note this was added
    // to debug.
    {
        static char opBuf[64];
        snprintf(opBuf, sizeof(opBuf), "min %.2f / max %.2f / avg %.2f / occluded %d/%d",
                 dbgBeamOpacityMin, dbgBeamOpacityMax, dbgBeamOpacityAvg,
                 dbgBeamOccludedCount, dbgBeamSampleCount);
        CLAY(CLAY_ID("BeamOpacityDiagRow"), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                                 .padding = {4, 4, 2, 2},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY_TEXT(CLAY_STRING("Beam blockOpacity"), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
            CLAY(CLAY_ID("BeamOpacityDiagSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_String opStr{false, (int32_t)strlen(opBuf), opBuf};
            CLAY_TEXT(opStr, CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        }
    }

    // ── Debug pointing-ray visualization (C12 follow-up #12) ─────────
    // Draws each active beam's mirror's ACTUAL current reflected-sunlight direction as a thin
    // off-white ray from the satellite — not a knockout toggle (doesn't disable anything normal),
    // so it gets its own checkbox rather than living in debugDisableMask.
    CLAY(CLAY_ID("BeamDebugRayRow"), {.layout = {
                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26)},
                                          .padding = {4, 4, 2, 2},
                                          .childGap = 8,
                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
    {
        CLAY_TEXT(CLAY_STRING("Show beam pointing rays"), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
        CLAY(CLAY_ID("BeamDebugRaySpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
        Clay_Color rayChkBg = showBeamDebugRays ? Pal::btnAccent : (hovBeamDebugRaysToggle ? Pal::btnHover : Pal::btnIdle);
        CLAY(CLAY_ID("BeamDebugRayChk"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIXED(22)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = rayChkBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            bool n = Clay_Hovered();
            sndRollover(n, hovBeamDebugRaysToggle);
            sndClick(n, inp.lmbPressed);
            if (n && inp.lmbPressed)
                showBeamDebugRays = !showBeamDebugRays;
            hovBeamDebugRaysToggle = n;
            CLAY_TEXT(showBeamDebugRays ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
        }
    }
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
        {"Airglow coverage", &airglowCoverageGain, 0.0f, 1.0f, 0.05f, "%.2f", 84},
        {"Airglow polar boost (red)", &airglowPolarGain, 0.0f, 6.0f, 0.1f, "%.2f", 85},
        {"Zodiacal gain", &zodiacalGain, 0.0f, 0.1f, 0.001f, "%.3f", 88},
        {"Zodiacal width (deg)", &zodiacalWidthDeg, 5.0f, 60.0f, 1.0f, "%.0f", 89},
        // Ocean Milky Way reflection (2026-09-08). Appended at the next free index rather than
        // slotted next to the other sky-feature gains: renumbering this table means renumbering
        // hovCloudMinus/hovCloudPlus/draggingCloud/cloudBufs in lockstep, and those four have
        // drifted apart before.
        {"Ocean MW refl", &oceanMwReflGain, 0.0f, 8.0f, 0.1f, "%.2f", 90},
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
    CLAY(CLAY_ID("AttrAbout"), {.layout = {
                                    .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                    .padding = {6, 6, 5, 5},
                                    .childGap = 4,
                                    .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Sat Light Sim"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        static char aboutBuf[64];
        if (!aboutBuf[0])
            snprintf(aboutBuf, sizeof(aboutBuf), "v%s (%s), built %s", APP_VERSION, APP_GIT_COMMIT, APP_BUILD_DATE);
        Clay_String aboutStr{false, (int32_t)strlen(aboutBuf), aboutBuf};
        CLAY_TEXT(aboutStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("Constellation parameters are drawn from public filings and are "
                              "approximations for illustrative purposes."),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("This simulator uses AI generated code. There's an irony there."),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDivAbout"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                              .padding = {0, 0, 2, 2}},
                                   .backgroundColor = {30, 30, 32, 255}}) {}

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
        CLAY_TEXT(CLAY_STRING("Earth day/night/cloud/specular/normal/elevation maps"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("Solar System Scope — solarsystemscope.com/textures (CC BY 4.0)"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv2"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("AttrMilkyWay"), {.layout = {
                                       .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                       .padding = {6, 6, 5, 5},
                                       .childGap = 4,
                                       .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Milky Way skybox"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("\"The Milky Way panorama\" (eso0932a) — ESO/S. Brunier"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("European Southern Observatory — eso.org (CC BY 4.0)"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDivMilkyWay"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                 .padding = {0, 0, 2, 2}},
                                      .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr3"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Full moon texture"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("papereater"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv3"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr4"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("City lights detail textures"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("From KSP mod RSSVE, NASA Visible Earth imagery, edited by Theysen"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("CC BY-NC-SA 4.0 — Sat Light Sim is distributed as free software"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv4"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr5"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Music"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("papereater"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv5"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr6"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Noise texture"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("Default RGBA noise texture, shadertoy.com"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv6"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr7"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Ocean shader (heavy inspiration, modified)"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("\"Seascape\" by Alexander Alekseev aka TDM — 2014"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("shadertoy.com/view/Ms2SD1 (CC BY-NC-SA 3.0)"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv7"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr8"), {.layout = {
                                .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                .padding = {6, 6, 5, 5},
                                .childGap = 4,
                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Star catalogue"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("Yale Bright Star Catalogue (Hoffleit & Warren 1991, CDS V/50)"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
    }

    CLAY(CLAY_ID("AttrDiv8"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr9"), {.layout = {
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

    CLAY(CLAY_ID("AttrDiv9"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                          .padding = {0, 0, 2, 2}},
                               .backgroundColor = {30, 30, 32, 255}}) {}

    CLAY(CLAY_ID("Attr10"), {.layout = {
                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                 .padding = {6, 6, 5, 5},
                                 .childGap = 4,
                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY_TEXT(CLAY_STRING("Software libraries"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
        CLAY_TEXT(CLAY_STRING("GLFW (zlib), glm (MIT), Clay (zlib), stb (MIT), miniaudio (MIT),"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("nlohmann/json (MIT) — full license texts in THIRD_PARTY_NOTICES.txt"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
        CLAY_TEXT(CLAY_STRING("next to the executable"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
    }
}

// ─── buildViewControlsWindow ─────────────────────────────────────────────────
// Quick-reference list of simulation controls. No longer shown automatically on startup — it used
// to (gated by a since-removed showControlsOnStartup setting), but user testing found almost nobody
// closed it, so demo recordings consistently had it sitting in the top-left. Now closed by default
// every launch and only opened via the "Show window" button in Settings > Controls
// (buildSettingsControlsTab); closing it (or leaving it open) only lasts for the current run either
// way — open state itself is not persisted. Uses the same buildResizableWindow frame as the
// settings window (was a hand-rolled near-duplicate before; now one window implementation).
void SatelliteSim::buildViewControlsWindow(const UIInput &inp, UIRenderer &ui)
{
    buildResizableWindow(inp, ui, viewControlsChrome, 1, "Controls", true, hovViewControlsClose,
                         12.0f, 12.0f, 260.0f, 220.0f, 600.0f, 700.0f,
                         [&]()
                         { buildViewControlsBody(inp, ui); });
}

// ─── buildViewControlsBody ──────────────────────────────────────────────────
// UC4: two clearly-separated groups. "Digital" rows are all real keybindings entries — looked up
// live, so a rebind (or a controller with a custom gpButton) is reflected immediately, including
// KB_RAISE_ELEV/KB_LOWER_ELEV (Q/E), which used to be a hardcoded, non-rebind-aware string here.
// "Analog" rows are the axes (WASD+stick move, mouse-drag+stick look, scroll zoom, trigger
// elevation) that have no keybindings entry at all — deliberately not rebindable (see
// gpElevRaise/gpElevLower's comment in SatelliteSim.h) — labeled as such instead of being mixed
// in with the rebindable list. Within each row, lastInputWasGamepad puts whichever device the
// player is actually holding first (RELEASE_v1_1_PLAN.md UC4: "show the active input device's
// column first").
void SatelliteSim::buildViewControlsBody(const UIInput &inp, UIRenderer &ui)
{
    struct DigitalRow
    {
        const char *label;
        int kbIdx;
    };
    DigitalRow digitalRows[] = {
        {keybindings[KB_ZOOM_IN].action, KB_ZOOM_IN},
        {keybindings[KB_ZOOM_OUT].action, KB_ZOOM_OUT},
        {keybindings[KB_ZOOM_RESET].action, KB_ZOOM_RESET},
        {keybindings[KB_MOVE_BOOST].action, KB_MOVE_BOOST},
        {keybindings[KB_MOVE_FINE].action, KB_MOVE_FINE},
        {keybindings[KB_RAISE_ELEV].action, KB_RAISE_ELEV},
        {keybindings[KB_LOWER_ELEV].action, KB_LOWER_ELEV},
        {keybindings[KB_RESET_ELEV].action, KB_RESET_ELEV},
        {keybindings[KB_SELECT_SAT].action, KB_SELECT_SAT},
        {keybindings[KB_CINEMATIC].action, KB_CINEMATIC},
        {keybindings[KB_PAUSE].action, KB_PAUSE},
        {keybindings[KB_SLOWER].action, KB_SLOWER},
        {keybindings[KB_FASTER].action, KB_FASTER},
        {keybindings[KB_REVERSE].action, KB_REVERSE},
        {keybindings[KB_TOGGLE_UI].action, KB_TOGGLE_UI},
        {keybindings[KB_SCREENSHOT].action, KB_SCREENSHOT},
        {keybindings[KB_TOGGLE_CURSOR].action, KB_TOGGLE_CURSOR},
    };
    struct AnalogRow
    {
        const char *label;
        const char *kbText;  // nullptr = no keyboard/mouse equivalent
        const char *padText; // nullptr = no gamepad equivalent
    };
    AnalogRow analogRows[] = {
        {"Move", "WASD", "L stick"},
        {"Look around", "Right-click drag", "R stick"},
        {"Zoom (FOV)", "Scroll wheel", nullptr},
        {"Raise/Lower elevation", nullptr, "RT / LT trigger"},
    };

    auto ctrlRow = [&](int idx, const char *label, const char *keyText)
    {
        CLAY(CLAY_IDI("CtrlRow", idx), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(20)},
                                            .childGap = 6,
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            Clay_String lblStr{false, (int32_t)strlen(label), label};
            CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(12)}));
            CLAY(CLAY_IDI("CtrlSpacer", idx), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            Clay_String keyStr{false, (int32_t)strlen(keyText), keyText};
            CLAY_TEXT(keyStr, CLAY_TEXT_CONFIG({.textColor = Pal::keyText, .fontSize = fs(12)}));
        }
    };

    CLAY(CLAY_ID("ViewControlsScroll"), {.layout = {
                                             .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                             .padding = {12, 12, 8, 8},
                                             .childGap = 3,
                                             .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                         .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
    {
        static char keyBufs[KB_COUNT + 4][40];
        int idx = 0;
        for (auto &row : digitalRows)
        {
            const KeyBinding &kb = keybindings[row.kbIdx];
            const char *kbStr = keyDisplayName(kb.key);
            if (kb.gpButton >= 0)
            {
                const char *padStr = gamepadButtonDisplayName(kb.gpButton);
                if (lastInputWasGamepad)
                    snprintf(keyBufs[idx], sizeof(keyBufs[idx]), "%s / %s", padStr, kbStr);
                else
                    snprintf(keyBufs[idx], sizeof(keyBufs[idx]), "%s / %s", kbStr, padStr);
            }
            else
                snprintf(keyBufs[idx], sizeof(keyBufs[idx]), "%s", kbStr);
            ctrlRow(idx, row.label, keyBufs[idx]);
            ++idx;
        }

        CLAY(CLAY_ID("AnalogDiv"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                               .padding = {0, 0, 4, 4}},
                                    .backgroundColor = {40, 40, 44, 255}}) {}
        CLAY_TEXT(CLAY_STRING("ANALOG (not rebindable)"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(10)}));
        for (auto &row : analogRows)
        {
            if (row.kbText && row.padText)
            {
                if (lastInputWasGamepad)
                    snprintf(keyBufs[idx], sizeof(keyBufs[idx]), "%s / %s", row.padText, row.kbText);
                else
                    snprintf(keyBufs[idx], sizeof(keyBufs[idx]), "%s / %s", row.kbText, row.padText);
            }
            else
                snprintf(keyBufs[idx], sizeof(keyBufs[idx]), "%s", row.kbText ? row.kbText : row.padText);
            ctrlRow(idx, row.label, keyBufs[idx]);
            ++idx;
        }
    }
    ui.scrollbar(CLAY_ID("ViewControlsScroll"));
}

// ─── buildCrashRecoveryNotice ────────────────────────────────────────────────
// NEW-3: dismissible-by-timeout banner shown for a few seconds when init() detected the previous
// session's sentinel still present (see the "session.lock" comment in SatelliteSim::init/cleanup).
void SatelliteSim::buildCrashRecoveryNotice(float dt, const UIInput &inp, UIRenderer &ui)
{
    (void)inp;
    if (crashRecoveryNoticeTimer <= 0.0f)
        return;
    crashRecoveryNoticeTimer -= dt;

    CLAY(CLAY_ID("CrashNotice"), {.layout = {
                                      .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                      .padding = {16, 16, 10, 10},
                                      .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
                                  .backgroundColor = {70, 46, 12, 235},
                                  .cornerRadius = CLAY_CORNER_RADIUS(6),
                                  .floating = {.offset = {0, 16}, .zIndex = 25, .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP, .parent = CLAY_ATTACH_POINT_CENTER_TOP}, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        CLAY_TEXT(CLAY_STRING("Recovered from an unexpected exit last time — graphics reset to Planetarium. "
                              "Change it in Settings > Display."),
                  CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, 255}, .fontSize = fs(13)}));
    }
}

// ─── buildIntroOverlay ───────────────────────────────────────────────────────
// UC3: the cinematic camera path (updateIntroCinematic, recordCompute) IS the intro now — this
// just overlays the current beat's caption, translucent so the flythrough stays visible under it.
// Dismissal is intentionally narrow: only the literal Space bar (onKey) or gamepad Start
// (pollGamepad) end it early. It used to be dismissible by any click or any keypress, which meant
// an incidental click into the window (or just touching the keyboard) skipped it before most
// players ever saw it play — the mouse capture rect below still exists to swallow clicks so they
// don't leak through to satellite picking or camera look, but a click no longer skips anything.
void SatelliteSim::buildIntroOverlay(const UIInput &inp, UIRenderer &ui)
{
    if (!showIntro)
        return;

    ui.addMouseCaptureRect(0, 0, inp.screenW, inp.screenH);

    // Fade the current caption in over its first 0.8s; fade everything out over the last 1.0s of
    // the whole sequence so the final line doesn't just vanish at the auto-handoff.
    float capStartT = kIntroKeyframes[introCaptionIndex].t;
    float alphaIn = glm::clamp((introElapsed - capStartT) / 0.8f, 0.0f, 1.0f);
    float tEnd = kIntroKeyframes[kIntroKeyframeCount - 1].t;
    float alphaOut = glm::clamp((tEnd - introElapsed) / 1.0f, 0.0f, 1.0f);
    uint8_t textA = (uint8_t)(255.0f * std::min(alphaIn, alphaOut));

    bool isYearBeat = (introCaptionIndex == kIntroYearIndex);
    bool isTitleBeat = (introCaptionIndex == kIntroTitleIndex);
    bool isControlsBeat = (introCaptionIndex == kIntroControlsIndex);
    const char *text = kIntroKeyframes[introCaptionIndex].text;
    // The controls beat reads live keybindings instead of a compile-time literal, so a rebind or
    // a controller-only player still sees the right prompt (UC4: don't hardcode control text).
    if (isControlsBeat)
    {
        snprintf(introControlsTextBuf, sizeof(introControlsTextBuf),
                 "%s / %s to raise/lower height",
                 keyDisplayName(keybindings[KB_RAISE_ELEV].key), keyDisplayName(keybindings[KB_LOWER_ELEV].key));
        text = introControlsTextBuf;
    }

    if (text && textA > 0)
    {
        CLAY(CLAY_ID("IntroCaptionOuter"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED((float)inp.screenW),
                                                           CLAY_SIZING_FIXED((float)inp.screenH)},
                                                .padding = {40, 40, 0, 90},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                                   .y = CLAY_ALIGN_Y_BOTTOM},
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                            .floating = {.zIndex = 30, .attachTo = CLAY_ATTACH_TO_ROOT}})
        {
            // No background panel here (deliberately — a translucent rounded rectangle used to sit
            // behind this text; user testing found it read as UI clutter, especially since players
            // rarely dismissed the intro quickly, so demo footage was full of it). A dedicated
            // serif face was tried in its place (2026-09-08) but reverted the same day: the stbtt
            // baked-bitmap atlas this renderer uses has no per-size re-rasterization (see
            // UIRenderer::FontAtlas's bakedSize comment), and the serif face's hinting produced
            // visibly misaligned glyphs at these large caption sizes (most obviously "2036"'s 3/6)
            // once scaled up from the bake. Fixing that for real needs an SDF font atlas, not
            // attempted here — regular UI font stays the only font for now.
            CLAY(CLAY_ID("IntroCaptionPanel"), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                    .padding = {24, 24, 16, 16},
                                                    .childGap = 4,
                                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER},
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                if (isYearBeat)
                {
                    CLAY_TEXT(CLAY_STRING("2036"),
                              CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, (float)textA}, .fontSize = fs(48)}));
                }
                else if (isTitleBeat)
                {
                    CLAY_TEXT(CLAY_STRING("SAT LIGHT SIM"),
                              CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, (float)textA}, .fontSize = fs(34)}));
                }
                else if (isControlsBeat)
                {
                    CLAY_TEXT(CLAY_STRING("WASD to move"),
                              CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, (float)textA}, .fontSize = fs(19)}));
                    Clay_String txtStr{false, (int32_t)strlen(text), text};
                    CLAY_TEXT(txtStr, CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, (float)textA}, .fontSize = fs(19)}));
                }
                else
                {
                    Clay_String txtStr{false, (int32_t)strlen(text), text};
                    CLAY_TEXT(txtStr, CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, (float)textA}, .fontSize = fs(19)}));
                }
            }
        }
    }

    // Skip hint, bottom-right corner, not faded with the caption above (needs to stay legible/
    // discoverable regardless of which beat is showing). Only appears once the first real
    // narrative line ("Satellite megaconstellations...") has been reached — not from frame 1 —
    // so the opening "2036" title card reads as a clean establishing beat, and the hint (the only
    // thing that reveals Space can skip at all) doesn't compete with it for attention immediately.
    if (introCaptionIndex >= kIntroHintRevealIndex)
    {
        CLAY(CLAY_ID("IntroSkipHint"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                   .padding = {8, 8, 4, 4}},
                                        .backgroundColor = {0, 0, 0, 130},
                                        .cornerRadius = CLAY_CORNER_RADIUS(4),
                                        .floating = {.offset = {-16, -16},
                                                     .zIndex = 30,
                                                     .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_BOTTOM,
                                                                      .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM},
                                                     .attachTo = CLAY_ATTACH_TO_ROOT}})
        {
            CLAY_TEXT(CLAY_STRING("Press SPACE to skip"),
                      CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(12)}));
        }
    }
}

// ─── buildGraphicsAutoNotice ─────────────────────────────────────────────────
// UC1 mechanism 3: dismissible-by-timeout banner shown once, right after the intro cinematic
// finishes, telling the user what preset the UC1 benchmark chose. Same pattern as
// buildCrashRecoveryNotice, kept as its own timer/text pair since the two notices are triggered
// by unrelated events and can in principle both be live (though crashRecoveryMode suppresses the
// benchmark that feeds this one — see finishIntro()).
void SatelliteSim::buildGraphicsAutoNotice(float dt, const UIInput &inp, UIRenderer &ui)
{
    (void)inp;
    if (graphicsAutoNoticeTimer <= 0.0f)
        return;
    graphicsAutoNoticeTimer -= dt;

    Clay_String msgStr{false, (int32_t)strlen(graphicsAutoNoticeText), graphicsAutoNoticeText};
    CLAY(CLAY_ID("GraphicsAutoNotice"), {.layout = {
                                             .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                             .padding = {16, 16, 10, 10},
                                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
                                         .backgroundColor = {20, 40, 70, 235},
                                         .cornerRadius = CLAY_CORNER_RADIUS(6),
                                         .floating = {.offset = {0, 16}, .zIndex = 25, .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP, .parent = CLAY_ATTACH_POINT_CENTER_TOP}, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        CLAY_TEXT(msgStr, CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, 255}, .fontSize = fs(13)}));
    }
}

// ─── buildScreenshotToast ────────────────────────────────────────────────────
// UC6: bottom-center confirmation toast ("Saved satlight_....png") after F12. Same dismissible-
// timer-banner pattern as buildCrashRecoveryNotice/buildGraphicsAutoNotice, anchored to the
// bottom instead of the top so it doesn't collide with either of those (both top-anchored).
void SatelliteSim::buildScreenshotToast(float dt, const UIInput &inp, UIRenderer &ui)
{
    (void)inp;
    if (screenshotToastTimer <= 0.0f)
        return;
    screenshotToastTimer -= dt;

    Clay_String msgStr{false, (int32_t)strlen(screenshotToastText), screenshotToastText};
    CLAY(CLAY_ID("ScreenshotToast"), {.layout = {
                                          .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                          .padding = {14, 14, 8, 8},
                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER}},
                                      .backgroundColor = {20, 60, 30, 235},
                                      .cornerRadius = CLAY_CORNER_RADIUS(6),
                                      .floating = {.offset = {0, -16}, .zIndex = 25, .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM, .parent = CLAY_ATTACH_POINT_CENTER_BOTTOM}, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        CLAY_TEXT(msgStr, CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, 255}, .fontSize = fs(13)}));
    }
}

// ─── Harness console (docs/HARNESS.md) ───────────────────────────────────────
// A translucent strip across the top: the recent command/result lines and the input line. Key and
// text handling live in SatelliteSimHarness.cpp (consoleKey / onChar).
void SatelliteSim::buildHarnessConsole(const UIInput &inp, UIRenderer &ui)
{
    if (!consoleOpen_)
        return;
    consoleView_.clear();
    const int maxLines = std::max(4, (int)(inp.screenH * 0.38f / (fs(12) + 4)) - 2);
    if (harnessRunner_)
    {
        const auto &lines = harnessRunner_->consoleLines();
        const int first = std::max(0, (int)lines.size() - maxLines);
        for (int i = first; i < (int)lines.size(); ++i)
            consoleView_.push_back(lines[i]);
    }
    else
        consoleView_.push_back("Harness console - `help` lists commands; Enter runs, Up/Down history, Esc or ~ closes.");
    consoleView_.push_back("> " + consoleInput_ + "_");
    const float h = inp.screenH * 0.40f;
    ui.addMouseCaptureRect(0.0f, 0.0f, inp.screenW, h);
    CLAY(CLAY_ID("HarnessConsole"), {.layout = {.sizing = {CLAY_SIZING_FIXED(inp.screenW), CLAY_SIZING_FIXED(h)},
                                               .padding = {12, 12, 8, 8},
                                               .childGap = 4,
                                               .childAlignment = {.y = CLAY_ALIGN_Y_BOTTOM},
                                               .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                    .backgroundColor = {6, 8, 10, 225},
                                    .floating = {.offset = {0, 0}, .zIndex = 60, .attachTo = CLAY_ATTACH_TO_ROOT},
                                    .clip = {.vertical = true}})
    {
        for (size_t i = 0; i < consoleView_.size(); ++i)
        {
            const std::string &l = consoleView_[i];
            const bool input = i + 1 == consoleView_.size();
            const bool err = l.rfind("  ERROR", 0) == 0;
            const bool echo = l.rfind("> ", 0) == 0;
            Clay_Color col = input ? Clay_Color{255, 255, 255, 255}
                             : err ? Clay_Color{255, 110, 100, 255}
                             : echo ? Clay_Color{150, 200, 255, 255}
                                    : Clay_Color{200, 200, 200, 255};
            Clay_String s{false, (int32_t)l.size(), l.c_str()};
            CLAY_TEXT(s, CLAY_TEXT_CONFIG({.textColor = col, .fontSize = fs(12), .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
    }
}

// ─── seedGraphicsPresetFromDevice ────────────────────────────────────────────
// UC1 mechanism 1 of 3 (RELEASE_v1_1_PLAN.md): coarse device-type seed. Deliberately NOT a
// GPU-name lookup table — deviceType is the only signal guaranteed never catastrophically wrong.
// Mechanism 2 (an in-app benchmark during the UC3 intro cinematic) is a later phase; mechanism 3
// (tell the user, never silently re-decide) is the fprintf + one-shot-ness in loadSettings below.
GraphicsPreset SatelliteSim::seedGraphicsPresetFromDevice(VulkanContext &ctx) const
{
    VkPhysicalDeviceProperties props{};
    vkGetPhysicalDeviceProperties(ctx.physicalDevice, &props);
    switch (props.deviceType)
    {
    case VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU:
        return GraphicsPreset::Medium;
    case VK_PHYSICAL_DEVICE_TYPE_INTEGRATED_GPU:
    case VK_PHYSICAL_DEVICE_TYPE_CPU:
    case VK_PHYSICAL_DEVICE_TYPE_VIRTUAL_GPU:
    default:
        return GraphicsPreset::Low;
    }
}

// ─── applyGraphicsPreset ─────────────────────────────────────────────────────
// UC1: the preset table. Planetarium/Low/Medium/High/Ultra each overwrite debugDisableMask,
// renderScale, and the "advanced" Clouds/Ocean/Terrain/Aurora sliders wholesale; Custom is a
// no-op (see GraphicsPreset comment in SatelliteSim.h). debugDisableMask bit values are the
// documented knockout bits from CLAUDE.md's "GPU Performance Profiling" subsystem — every one of
// them already has "a mathematically-safe zero/no-op fallback" per that doc, which is what makes
// promoting them from a profiling tool to a shipped preset system safe to do without touching
// shader code. High's numbers are the compiled-in class member defaults verbatim (today's tuned
// values); Ultra pushes each slider toward its UI-exposed ceiling; Low/Medium/Planetarium pull
// them down, roughly halving each tier's cost band per RELEASE_v1_1_PLAN.md's preset table.
//
// TIER ORDERING IS A REAL INVARIANT, and one field can silently break it: terrainFadeStartM is the
// distance at which the terrain march's step budget BEGINS fading out, so a LOWER value is CHEAPER,
// the opposite direction from every other number in this table. When the 2026-08-06 tuning pass
// took the compiled-in default from 300000 down to 50000 (for a long gradual relief roll-off rather
// than a late abrupt one), High became cheaper on that axis than Medium and Low, which still had
// 200000/100000. Fixed by pinning Planetarium/Low/Medium/High all to the same 50000 start and
// letting terrainFadeEndM alone carry the tier scaling (equal start + lower end = strictly fewer
// samples at every distance, so the ordering holds by construction and can't invert again).
// Ultra deliberately keeps a much higher start (900000) — that is the "no fade until far" end.
void SatelliteSim::applyGraphicsPreset(GraphicsPreset p)
{
    if (p == GraphicsPreset::Custom)
    {
        graphicsPreset = p;
        return; // nothing to apply — trust whatever is currently loaded/set
    }

    static constexpr uint32_t kBitTerrain = 1u, kBitOceanRefl = 8u, kBitAirglowRed = 16u,
                              kBitAurora = 32u, kBitBeams = 128u, kBitCloudShadow = 256u,
                              kBitBeamBlock = 512u, kBitFog = 2048u;
    // Potato-only knockout bits (see CLAUDE.md "GPU Performance Profiling" bit table). Each has a
    // documented math-safe fallback, same as the ones above.
    static constexpr uint32_t kBitSceneDepth = 1024u, kBitBeamRayLoop = 8192u,
                              kBitCirrusMarch = 16384u, kBitCloudMarch = 32768u,
                              kBitSkyGlowLoop = 65536u, kBitMinimalSky = 262144u,
                              kBitLiteSky = 524288u; // sat_sky.frag -DSKY_LITE variant (Planetarium)

    struct PresetValues
    {
        uint32_t mask;
        float renderScale;
        float cloudCoverage, cloudMarchSteps, cloudLightSteps;
        float viewSamplesMin, viewSamplesMax, lightSamples;
        float oceanSeaOctaves, oceanDetailOctaves, oceanReflSamples;
        float terrainFadeStartM, terrainFadeEndM;
        float cloudFadeStartM, cloudFadeEndM;
    } v{};

    switch (p)
    {
    case GraphicsPreset::Potato:
        // Below Planetarium, for machines that can't sustain the fullscreen raymarch at all
        // (2015-era / integrated GPUs, MoltenVK translation). Everything Planetarium turns off,
        // PLUS: the half-res scene-depth pass (bit 1024 — Earth is flat here anyway, nothing
        // consumes real terrain depth), the per-pixel beam pointing-ray loop (8192), and the two
        // volumetric-march compute kernels + the 64-bin sky-glow loop (16384/32768/65536 — all
        // no-ops at cloudCoverage 0). Atmosphere scattering (bit 2) is deliberately LEFT ON —
        // measured on a MoltenVK/GCN1 target it cost only a few ms while its absence removed the
        // entire sky gradient. viewSamples at the floor so the atmosphere it keeps is cheap.
        //
        // renderScale 1.0, NOT 0.5: on MoltenVK the < 1.0 path adds a whole extra offscreen render
        // pass + a vkCmdBlitImage into the swapchain every frame, and each is another command-
        // encoder boundary — on this driver that costs more than the pixels it saves (which the
        // knockout sweep already proved don't matter here). Matches CLAUDE.md's "prefer 100%".
        v = {kBitTerrain | kBitOceanRefl | kBitAirglowRed | kBitAurora | kBitBeams | kBitCloudShadow |
                 kBitBeamBlock | kBitFog | kBitSceneDepth | kBitBeamRayLoop |
                 kBitCirrusMarch | kBitCloudMarch | kBitSkyGlowLoop | kBitMinimalSky,
             1.0f, 0.0f, 64.0f, 2.0f, 6.0f, 20.0f, 2.0f, 3.0f, 5.0f, 3.0f, 50000.0f, 100000.0f, 5000.0f, 10000.0f};
        break;
    case GraphicsPreset::Planetarium:
        // v1.0 experience: flat textured Earth (cloudCoverage 0 — no cloud layer at all), terrain
        // relief and ocean reflection off (the sea-level sphere / flat ocean fallbacks already
        // exist and are exactly what "terrain off" looks like), every nightglow phenomenon off.
        // Ocean sea/detail octaves still run full quality (3/5) even though reflection itself is
        // knocked out — those two octave counts drive the height-trace/wave-normal geometry the
        // flat-ocean fallback still uses, and are cheap enough to never scale down (user directive
        // 2026-08-04: "never compromise on ocean quality"); reflSamples is the one ocean slider
        // still turned down here since kBitOceanRefl makes it a true no-op at this tier.
        // kBitLiteSky (SKY_OPTIMIZATION_PLAN.md Phase 1): sat_sky.frag compiled with -DSKY_LITE —
        // Milky Way / 64-bin sky-glow loop / cirrus+high cloud layers / the 3x3 cloud rgb blur /
        // aurora surface glow / per-atmosphere-step city-upwelling + airglow #ifdef'd out, for weak
        // GPUs (2015 AMD via MoltenVK) where the full shader collapses fragment occupancy (measured
        // 2 -> 22 FPS on the 2015 target). The full shader stays the default for Medium+ and any GPU
        // that can run it. Phase 2: atmosphere march viewSamplesMin/Max 6/48 -> 4/10, and the two
        // volumetric-cloud compute kernels knocked out (kBitCloudMarch/kBitCirrusMarch — coverage
        // is 0 at this tier so they march nothing but still cost a dispatch + the sample below).
        v = {kBitTerrain | kBitOceanRefl | kBitAirglowRed | kBitAurora | kBitBeams | kBitCloudShadow |
                 kBitBeamBlock | kBitFog | kBitLiteSky | kBitCloudMarch | kBitCirrusMarch,
             1.0f, 0.0f, 64.0f, 2.0f, 4.0f, 10.0f, 2.0f, 3.0f, 5.0f, 3.0f, 50000.0f, 100000.0f, 5000.0f, 10000.0f};
        break;
    case GraphicsPreset::Low:
        // Terrain stays on (a bare sea-level sphere with no relief reads as more "wrong" than
        // "fast" to a player who came for the planet) but with a tight fade budget; clouds forced
        // fully flat-2D (cloudFadeStartM at its UI-slider floor — "2D cloud paste only" — but
        // cloudFadeEndM raised to 50000 so a sea-level player looking straight up still catches the
        // volumetric layer at the zenith instead of transitioning to flat before it's ever visible;
        // this was the main source of Low looking broken rather than just cheap). Ocean sea/detail/
        // reflection octaves matched to Medium/High/Ultra (3/5/6) per the same "never compromise on
        // ocean quality" directive — those sliders are effectively free relative to everything else
        // this tier turns down.
        v = {kBitAurora | kBitBeams | kBitFog,
             0.7f, 1.0f, 64.0f, 4.0f, 6.0f, 64.0f, 2.0f, 3.0f, 5.0f, 6.0f, 50000.0f, 300000.0f, 5000.0f, 50000.0f};
        break;
    case GraphicsPreset::Medium:
        // Nothing disabled outright — volumetric clouds and aurora both run, at reduced step
        // budgets and a tighter terrain/cloud reach than High. Ocean sea/detail octaves and
        // reflection samples are maxed out here (matching High/Ultra) rather than scaled down
        // with everything else — measured cost of those three sliders is negligible, so there is
        // no real budget to save by tightening them at this tier.
        v = {0u,
             0.85f, 1.0f, 128.0f, 10.0f, 6.0f, 96.0f, 2.0f, 3.0f, 5.0f, 6.0f, 50000.0f, 600000.0f, 80000.0f, 200000.0f};
        break;
    case GraphicsPreset::High:
        // The compiled-in class member defaults, verbatim — "today's tuned values." Re-synced
        // 2026-08-15 with the 2-significant-figure default cleanup (every tuned default in
        // SatelliteSim.h was rounded to 2 s.f. for legibility; the four sample counts and two
        // cloud-fade distances below moved with them). If you change a default in
        // SatelliteSim.h that appears in PresetValues, change it here in the same edit.
        v = {0u,
             1.0f, 1.0f, 220.0f, 13.0f, 6.5f, 160.0f, 2.4f, 3.0f, 5.0f, 6.0f,
             50000.0f, 900000.0f, 150000.0f, 400000.0f};
        break;
    case GraphicsPreset::Ultra:
        // Uncapped for showcase/screenshots — pushed to each slider's UI-exposed ceiling.
        v = {0u,
             1.0f, 1.0f, 512.0f, 16.0f, 16.0f, 256.0f, 4.0f, 3.0f, 5.0f, 6.0f,
             900000.0f, 3600000.0f, 400000.0f, 900000.0f};
        break;
    default:
        return;
    }

    debugDisableMask = v.mask;
    renderScale = v.renderScale;
    cloudCoverage = v.cloudCoverage;
    cloudMarchSteps = v.cloudMarchSteps;
    cloudLightSteps = v.cloudLightSteps;
    viewSamplesMin = v.viewSamplesMin;
    viewSamplesMax = v.viewSamplesMax;
    lightSamples = v.lightSamples;
    oceanSeaOctaves = v.oceanSeaOctaves;
    oceanDetailOctaves = v.oceanDetailOctaves;
    oceanReflSamples = v.oceanReflSamples;
    terrainDistFadeStartM = v.terrainFadeStartM;
    terrainDistFadeEndM = v.terrainFadeEndM;
    cloudDistFadeStartM = v.cloudFadeStartM;
    cloudDistFadeEndM = v.cloudFadeEndM;
    graphicsPreset = p;

    if (std::getenv("SATLIGHTSIM_FRAME_TRACE"))
        fprintf(stderr, "[sky] applyGraphicsPreset(%s): mask=%u renderScale=%.3f ctx_=%p\n",
                kGraphicsPresetNames[(int)p], debugDisableMask, renderScale, (void *)ctx_);

    if (ctx_)
    {
        destroySkyLowResResources(ctx_->device);
        createSkyLowResResources(*ctx_);
    }
}

// ─── loadSettings ─────────────────────────────────────────────────────────────
// Reads settings.json from the exe directory.  Silently skips if the file is
// missing (first run).  Logs a warning and returns on parse error.
// Must be called after initConstellation() so constellations[] is populated.
void SatelliteSim::loadSettings()
{
#ifdef SAT_FRESH_SETTINGS
    // Fresh-settings build (SatLightSimFresh / -DSAT_FRESH_SETTINGS): never read a persisted
    // settings.json, so every launch is a genuine out-of-the-box first run. Still seed the
    // graphics preset from the device the same way a real first run does (below), so what's
    // being tested is the true default experience, not an unseeded High.
    fprintf(stderr, "[SatelliteSim] FRESH build: ignoring any settings.json; using defaults.\n");
    applyGraphicsPreset(seedGraphicsPresetFromDevice(*ctx_));
    return;
#endif
    auto path = (std::filesystem::path(userDataDir_) / "settings.json").string();
    std::ifstream f(path);
    if (!f.is_open())
    {
        // UC1 first-run graphics selection: no persisted preference at all, so seed a preset
        // from the device's VkPhysicalDeviceProperties::deviceType rather than leaving whatever
        // the compiled-in defaults (High) happen to be — those are the author's own tuned values
        // on the author's own GPU, not a safe default for unknown hardware. Everything else
        // (camera, audio, keybindings, ...) has no persisted value either, so this is genuinely
        // first-run only, never a silent re-decision on a later launch (see RELEASE_v1_1_PLAN.md
        // UC1, "always tell the user, never silently re-decide" — the visible notice is UC3's
        // job; this is the mechanism it reads from).
        applyGraphicsPreset(seedGraphicsPresetFromDevice(*ctx_));
        fprintf(stderr, "[SatelliteSim] First run: seeded graphics preset %d from device type.\n",
                (int)graphicsPreset);
        return;
    }

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

    applySettingsJson(j, false);

    fprintf(stderr, "[SatelliteSim] Loaded settings from %s\n", path.c_str());
}

// ─── applySettingsJson ────────────────────────────────────────────────────────
// The body of loadSettings, shared with the harness's `set` command (docs/HARNESS.md): every
// field reads `value(key, current)`, so a partial object (one key) changes exactly that key.
// isPatch: the object is a harness patch rather than a settings.json (see its three uses below).
void SatelliteSim::applySettingsJson(const nlohmann::json &j, bool isPatch)
{
    // Schema versioning (NEW-5): an old/missing schema_version means the graphics-affecting
    // sections below (photometry/clouds/render_scale) may hold values that no longer make sense
    // against current code (re-tuned defaults, or — once UC1 lands — a preset system). Camera,
    // audio, keybindings, observer position, time scale, and constellation toggles are not
    // "graphics" and stay loaded regardless; only photometry/clouds/render_scale are gated.
    int loadedSchemaVersion = j.value("schema_version", 0);
    // A harness patch (`set section.key value`) carries no schema_version: it is written against
    // the running build by definition.
    bool schemaMatches = isPatch || loadedSchemaVersion == kSettingsSchemaVersion;
    if (!schemaMatches)
        fprintf(stderr, "[SatelliteSim] settings.json schema %d != current %d — resetting "
                        "photometry/clouds/render_scale to defaults, keeping the rest.\n",
                loadedSchemaVersion, kSettingsSchemaVersion);

    if (schemaMatches && j.contains("photometry"))
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
        pointRefMag = p.value("point_ref_mag", pointRefMag);
        pointGamma = p.value("point_gamma", pointGamma);
        pointLimitMag = p.value("point_limit_mag", pointLimitMag);
        zoomApertureMaxMm = p.value("zoom_aperture_max_mm", zoomApertureMaxMm);
        pointSigmaPx = p.value("point_sigma_px", pointSigmaPx);
        pointSigmaMaxPx = p.value("point_sigma_max_px", pointSigmaMaxPx);
        sunlitBgVisibility = p.value("sunlit_bg_visibility", sunlitBgVisibility);
        flareGlowGain = p.value("flare_glow_gain", flareGlowGain);
        flareStreakGain = p.value("flare_streak_gain", flareStreakGain);
        glareGain = p.value("glare_gain", glareGain);
        glareSizePx = p.value("glare_size_px", glareSizePx);
        glareThreshold = p.value("glare_threshold", glareThreshold);
        glareFalloff = p.value("glare_falloff", glareFalloff);
        glareSpikes = p.value("glare_spikes", glareSpikes);
        mwPollutionThresholdLo = p.value("mw_pollution_threshold_lo", mwPollutionThresholdLo);
        mwPollutionThresholdHi = p.value("mw_pollution_threshold_hi", mwPollutionThresholdHi);
        darkSkyCityMag = p.value("dark_sky_city_mag", darkSkyCityMag);
        darkSkyTwilightMag0 = p.value("dark_sky_twilight_mag0", darkSkyTwilightMag0);
        darkSkyTwilightEndDeg = p.value("dark_sky_twilight_end_deg", darkSkyTwilightEndDeg);
        darkSkyTwilightAniso = p.value("dark_sky_twilight_aniso", darkSkyTwilightAniso);
        mwFadeInTimeS = p.value("mw_fade_in_time_s", mwFadeInTimeS);
        mwFadeOutTimeS = p.value("mw_fade_out_time_s", mwFadeOutTimeS);
        trailDecaySeconds = p.value("trail_decay_seconds", trailDecaySeconds);
        trailCompositeGain = p.value("trail_composite_gain", trailCompositeGain);
        flareMitigationTiltDeg = p.value("flare_mitigation_tilt_deg", flareMitigationTiltDeg);
        satPartOcclusion = p.value("sat_part_occlusion", satPartOcclusion);
        envReflections = p.value("full_renderer_reflections", envReflections);
        sharpReflections = p.value("sharp_mirror_reflections", sharpReflections);
    }

    if (j.contains("display"))
    {
        auto &d = j["display"];
        if (schemaMatches)
        {
            uiScale = d.value("ui_scale", uiScale);
            renderScale = d.value("render_scale", renderScale);
            int fpsCapVal = d.value("fps_cap_mode", (int)fpsCapMode);
            fpsCapMode = (fpsCapVal >= 0 && fpsCapVal <= 4) ? (FpsCapMode)fpsCapVal : FpsCapMode::VSync;
            // UC1: default to Custom (NOT a device-seeded preset) when the key is simply absent —
            // upgrading from a pre-UC1 settings.json must never silently overwrite hand-tuned
            // values. True first-run device seeding only happens in the "no file at all" branch
            // above; this is a different case (file exists, schema matches, preset just never
            // existed as a concept yet).
            int presetVal = d.value("graphics_preset", (int)GraphicsPreset::Custom);
            graphicsPreset = (presetVal >= 0 && presetVal <= (int)GraphicsPreset::Potato)
                                 ? (GraphicsPreset)presetVal
                                 : GraphicsPreset::Custom;
            showAdvancedSettings = d.value("show_advanced_settings", showAdvancedSettings);
            debugDisableMask = (uint32_t)d.value("debug_disable_mask", (int64_t)debugDisableMask);
        }
        settingsChrome.x = d.value("win_x", settingsChrome.x);
        settingsChrome.y = d.value("win_y", settingsChrome.y);
        settingsChrome.w = d.value("win_w", settingsChrome.w);
        settingsChrome.h = d.value("win_h", settingsChrome.h);
        settingsActiveTab = std::clamp(d.value("active_tab", settingsActiveTab), 0, 11);
        int unitVal = d.value("unit_system", unitSystem == UnitSystem::Imperial ? 1 : 0);
        unitSystem = unitVal == 1 ? UnitSystem::Imperial : UnitSystem::Metric;
        // Feature preference, not a graphics-tuning value — unconditional, not gated behind
        // schemaMatches. trailClearPending stays true regardless (its own compiled-in default),
        // so a trail-enabled load always starts from a blank buffer.
        trailEnabled = d.value("trail_enabled", trailEnabled);
        // UC3 follow-up: back to real persisted behavior now that the cinematic itself is settled
        // (the always-on-every-launch testing override is gone). "play_intro_on_startup" absent
        // (upgrading from a build that predates this key) defaults to false, not the compiled-in
        // true, so upgrading players don't suddenly get a cinematic that didn't exist in their
        // version — a true first run never reaches this block at all (see the "no file" early
        // return above), so it still keeps the compiled-in true default there.
        if (!isPatch || d.contains("play_intro_on_startup"))
        {
            playIntroOnStartup = d.value("play_intro_on_startup", false);
            showIntro = playIntroOnStartup;
        }
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

    if (j.contains("controls"))
    {
        auto &ctl = j["controls"];
        invertMouseX = ctl.value("invert_mouse_x", invertMouseX);
        invertMouseY = ctl.value("invert_mouse_y", invertMouseY);
        invertPadX = ctl.value("invert_pad_x", invertPadX);
        invertPadY = ctl.value("invert_pad_y", invertPadY);
    }

    if (j.contains("controls") && j["controls"].contains("keybindings"))
    {
        // hasGp distinguishes "no gp_button key in this settings.json" (older file, predating
        // gamepad support — keep the compiled-in default gpButton) from an explicit rebind.
        struct LoadedBinding
        {
            int key;
            int gpButton;
            bool hasGp;
        };
        std::unordered_map<std::string, LoadedBinding> actionKey;
        for (const auto &kb : j["controls"]["keybindings"])
            if (kb.contains("action") && kb.contains("key"))
                actionKey[kb["action"].get<std::string>()] = {
                    kb["key"].get<int>(), kb.value("gp_button", -1), kb.contains("gp_button")};
        for (auto &kb : keybindings)
        {
            auto it = actionKey.find(kb.action);
            if (it != actionKey.end())
            {
                kb.key = it->second.key;
                if (it->second.hasGp)
                    kb.gpButton = it->second.gpButton;
            }
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

    if (j.contains("planets"))
    {
        auto &pj = j["planets"];
        showPlanets = pj.value("show_planets", showPlanets);
        if (pj.contains("list"))
        {
            std::unordered_map<std::string, const nlohmann::json *> byName;
            for (const auto &jp : pj["list"])
                if (jp.contains("name"))
                    byName[jp["name"].get<std::string>()] = &jp;
            for (int pi = 0; pi < kPlanetCount; ++pi)
            {
                auto it = byName.find(kPlanetNames[pi]);
                if (it != byName.end())
                    planetEnabled[pi] = it->second->value("enabled", planetEnabled[pi]);
            }
        }
    }

    if (schemaMatches && j.contains("clouds"))
    {
        auto &c = j["clouds"];
        cloudCoverage = c.value("coverage", cloudCoverage);
        cloudDensity = c.value("density", cloudDensity);
        cloudBaseAltM = c.value("base_alt_m", cloudBaseAltM);
        cloudTopAltM = c.value("top_alt_m", cloudTopAltM);
        cloudDriftRate = c.value("drift_rate", cloudDriftRate);
        cloudSunGain = c.value("sun_gain", cloudSunGain);
        cloudSunGainZenith = c.value("sun_gain_zenith", cloudSunGainZenith);
        cloudAmbientGain = c.value("ambient_gain", cloudAmbientGain);
        // Key renamed with the term's meaning; an old night_ambient_gain value is not
        // meaningful for the twilight bell, so it is deliberately not migrated.
        cloudTwilightAmbientGain = c.value("twilight_ambient_gain", cloudTwilightAmbientGain);
        sunGainElevBand = c.value("sun_gain_elev_band", sunGainElevBand);
        twilightBandHi = c.value("twilight_band_hi", twilightBandHi);
        twilightBandLo = c.value("twilight_band_lo", twilightBandLo);
        coverageMipLod = c.value("coverage_mip_lod", coverageMipLod);
        flatCoverageScale = c.value("flat_coverage_scale", flatCoverageScale);
        flatSunGainScale = c.value("flat_sun_gain_scale", flatSunGainScale);
        cloudDistFadeStartM = c.value("cloud_dist_fade_start_m", cloudDistFadeStartM);
        cloudDistFadeEndM = c.value("cloud_dist_fade_end_m", cloudDistFadeEndM);
        terrainDistFadeStartM = c.value("terrain_dist_fade_start_m", terrainDistFadeStartM);
        terrainDistFadeEndM = c.value("terrain_dist_fade_end_m", terrainDistFadeEndM);
        cloudBaseVariance = c.value("cloud_base_variance", cloudBaseVariance);
        cloudErosionEdge = c.value("cloud_erosion_edge", cloudErosionEdge);
        cloudErosionCore = c.value("cloud_erosion_core", cloudErosionCore);
        cloudHgG = c.value("hg_g", cloudHgG);
        cloudMarchSteps = c.value("march_steps", cloudMarchSteps);
        cloudLightSteps = c.value("light_steps", cloudLightSteps);
        cloudCirrusWindDeg = c.value("cirrus_wind_deg", cloudCirrusWindDeg);
        cloudCirrusStretch = c.value("cirrus_stretch", cloudCirrusStretch);
        airglowGain = c.value("airglow_gain", airglowGain);
        airglowGreenGain = c.value("airglow_green_gain", airglowGreenGain);
        airglowRedGain = c.value("airglow_red_gain", airglowRedGain);
        airglowSodiumGain = c.value("airglow_sodium_gain", airglowSodiumGain);
        airglowCoverageGain = c.value("airglow_coverage_gain", airglowCoverageGain);
        airglowPolarGain = c.value("airglow_polar_gain", airglowPolarGain);
        zodiacalGain = c.value("zodiacal_gain", zodiacalGain);
        oceanMwReflGain = c.value("ocean_mw_refl_gain", oceanMwReflGain);
        zodiacalWidthDeg = c.value("zodiacal_width_deg", zodiacalWidthDeg);
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
        beamGain = c.value("beam_gain", beamGain);
        beamMaxRangeM = c.value("beam_max_range_m", beamMaxRangeM);
        beamSkyGlowGain = c.value("beam_sky_glow_gain", beamSkyGlowGain);
        // mirror_slew_deg_per_sec: pre-2026-08-06 key, superseded by reflector_lock_window_s
        // below (rate-limited slew replaced by a fixed sim-time lock window) — absent-key default
        // pattern means an old settings.json simply falls back to the compiled-in default here.
        reflectorLockWindowS = c.value("reflector_lock_window_s", reflectorLockWindowS);
        mirrorMaxRateDegPerSec = c.value("mirror_max_rate_deg_per_sec", mirrorMaxRateDegPerSec);
        reflectorMinElevDeg = c.value("reflector_min_elev_deg", reflectorMinElevDeg);
        // beam_extinction_mult: C12 follow-up #44 — key deliberately no longer read; a stale
        // value in an old settings.json is simply ignored (no member left to load it into).
        beamGlowBleedGain = c.value("beam_glow_bleed_gain", beamGlowBleedGain);
        cloudShadowRangeM = c.value("cloud_shadow_range_m", cloudShadowRangeM);
        beamNearFieldFadeM = c.value("beam_near_field_fade_m", beamNearFieldFadeM);
        beamClusterDirThresholdDeg = c.value("beam_cluster_dir_threshold_deg", beamClusterDirThresholdDeg);
        beamClusterFadeInS = c.value("beam_cluster_fade_in_s", beamClusterFadeInS);
        beamClusterFadeOutS = c.value("beam_cluster_fade_out_s", beamClusterFadeOutS);
        fogTopAltM = c.value("fog_top_alt_m", fogTopAltM);
        fogDensity = c.value("fog_density", fogDensity);
        fogCoverage = c.value("fog_coverage", fogCoverage);
        fogSunGain = c.value("fog_sun_gain", fogSunGain);
        cloudOpacityScale = c.value("cloud_opacity_scale", cloudOpacityScale);
        cityLightBlurLod = c.value("city_light_blur_lod", cityLightBlurLod);
        cloudWarpStrength = c.value("cloud_warp_strength", cloudWarpStrength);
        cloudWarpFreq = c.value("cloud_warp_freq", cloudWarpFreq);
        cloudSurfaceCarve = c.value("cloud_surface_carve", cloudSurfaceCarve);
        cloudErosionBillow = c.value("cloud_erosion_billow", cloudErosionBillow);
        cloudErosionBillowH = c.value("cloud_erosion_billow_h", cloudErosionBillowH);
        cloudErosionFreq = c.value("cloud_erosion_freq", cloudErosionFreq);
        cloudMultiScatter = c.value("cloud_multi_scatter", cloudMultiScatter);
        cloudShadowFloorT = c.value("cloud_shadow_floor_t", cloudShadowFloorT);
        cloudGrazeShadow = c.value("cloud_graze_shadow", cloudGrazeShadow);
        cloudConeLenScale = c.value("cloud_cone_len_scale", cloudConeLenScale);
        cloudVertShadeGain = c.value("cloud_vert_shade_gain", cloudVertShadeGain);
        cloudDensityAO = c.value("cloud_density_ao", cloudDensityAO);
        cloudAOPower = c.value("cloud_ao_power", cloudAOPower);
        flatDensityScale = c.value("cloud_flat_density_scale", flatDensityScale);
        flatRayleighGain = c.value("cloud_flat_rayleigh_gain", flatRayleighGain);
        flatTwilightAmbientGain =
            c.value("cloud_flat_twilight_ambient_gain", flatTwilightAmbientGain);
        atmosRayleighGain = c.value("atmos_rayleigh_gain", atmosRayleighGain);
        atmosMieGain = c.value("atmos_mie_gain", atmosMieGain);
        atmosTermStrength = c.value("atmos_term_strength", atmosTermStrength);
        atmosTermWidth = c.value("atmos_term_width", atmosTermWidth);
    }

    // UC1: a named preset (anything but Custom) is the authority on debugDisableMask/renderScale/
    // the advanced sliders — re-derive them from the table now. This makes preset-table retuning
    // in a later build reach existing installs automatically, and means whatever was loaded above
    // (which predates presets on an older file, or could otherwise disagree) never wins over the
    // preset's own name. Custom is intentionally skipped — it means "trust what was just loaded."
    // A patch re-derives only when it names the preset itself: `set clouds.coverage 0.2` must not be
    // overwritten by the preset table it is deliberately departing from.
    const bool patchNamesPreset = isPatch && j.contains("display") && j["display"].contains("graphics_preset");
    if (graphicsPreset != GraphicsPreset::Custom && (!isPatch || patchNamesPreset))
        applyGraphicsPreset(graphicsPreset);
}

// ─── saveSettings ─────────────────────────────────────────────────────────────
// Writes the current runtime state to settings.json in the per-user data directory (NEW-4).
// Called on cleanup() and when the settings window is closed.
void SatelliteSim::saveSettings()
{
#ifdef SAT_FRESH_SETTINGS
    // Fresh-settings build: never persist, so the next launch stays pristine. See loadSettings().
    return;
#endif
    if (userDataDir_.empty())
        return;

    nlohmann::json j = buildSettingsJson();
    auto path = (std::filesystem::path(userDataDir_) / "settings.json").string();
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

// Everything saveSettings persists, as one object. Also the harness's `get` (docs/HARNESS.md).
nlohmann::json SatelliteSim::buildSettingsJson()
{
    nlohmann::json j;

    j["schema_version"] = kSettingsSchemaVersion;
    j["app_version"] = APP_VERSION;
    j["git_commit"] = APP_GIT_COMMIT;

    j["photometry"] = {
        {"brightness_scale", brightnessScale},
        {"day_suppression", daySuppression},
        {"mirror_boost", mirrorBoost},
        {"vis_thresh", visThresh},
        {"highlight_flare", highlightFlare},
        {"moon_suppression", moonSuppression},
        {"light_pollution_gain", lightPollutionGain},
        {"extinction_coeff", extinctionCoeff},
        {"point_ref_mag", pointRefMag},
        {"point_gamma", pointGamma},
        {"point_limit_mag", pointLimitMag},
        {"zoom_aperture_max_mm", zoomApertureMaxMm},
        {"point_sigma_px", pointSigmaPx},
        {"point_sigma_max_px", pointSigmaMaxPx},
        {"sunlit_bg_visibility", sunlitBgVisibility},
        {"flare_glow_gain", flareGlowGain},
        {"flare_streak_gain", flareStreakGain},
        {"glare_gain", glareGain},
        {"glare_size_px", glareSizePx},
        {"glare_threshold", glareThreshold},
        {"glare_falloff", glareFalloff},
        {"glare_spikes", glareSpikes},
        {"mw_pollution_threshold_lo", mwPollutionThresholdLo},
        {"mw_pollution_threshold_hi", mwPollutionThresholdHi},
        {"dark_sky_city_mag", darkSkyCityMag},
        {"dark_sky_twilight_mag0", darkSkyTwilightMag0},
        {"dark_sky_twilight_end_deg", darkSkyTwilightEndDeg},
        {"dark_sky_twilight_aniso", darkSkyTwilightAniso},
        {"mw_fade_in_time_s", mwFadeInTimeS},
        {"mw_fade_out_time_s", mwFadeOutTimeS},
        {"trail_decay_seconds", trailDecaySeconds},
        {"trail_composite_gain", trailCompositeGain},
        {"flare_mitigation_tilt_deg", flareMitigationTiltDeg},
        {"sat_part_occlusion", satPartOcclusion},
        {"full_renderer_reflections", envReflections},
        {"sharp_mirror_reflections", sharpReflections}};

    j["display"] = {
        {"ui_scale", uiScale},
        {"render_scale", renderScale},
        {"fps_cap_mode", (int)fpsCapMode},
        {"graphics_preset", (int)graphicsPreset},
        {"show_advanced_settings", showAdvancedSettings},
        {"debug_disable_mask", debugDisableMask},
        {"active_tab", settingsActiveTab},
        {"unit_system", unitSystem == UnitSystem::Imperial ? 1 : 0},
        {"play_intro_on_startup", playIntroOnStartup},
        {"trail_enabled", trailEnabled}};
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

    // Follow mode (Phase 4e) persists the ground observer it will return to, not the orbit.
    const glm::vec3 saveObsDir = followActive ? followSavedObsDir : obsDir;
    j["camera"] = {
        {"az_deg", camera.azDeg},
        {"el_deg", followActive ? followSavedEl : camera.elDeg},
        {"fov_y_deg", followActive ? followSavedFov : camera.fovYDeg}};

    j["observer"] = {{"lat_deg", glm::degrees(asinf(glm::clamp(saveObsDir.z, -1.0f, 1.0f)))},
                     {"lon_deg", glm::degrees(atan2f(saveObsDir.y, saveObsDir.x))}};

    j["time"] = {{"scale_idx", timeScaleIdx}};

    j["clouds"] = {
        {"coverage", cloudCoverage},
        {"density", cloudDensity},
        {"base_alt_m", cloudBaseAltM},
        {"top_alt_m", cloudTopAltM},
        {"drift_rate", cloudDriftRate},
        {"sun_gain", cloudSunGain},
        {"sun_gain_zenith", cloudSunGainZenith},
        {"ambient_gain", cloudAmbientGain},
        {"twilight_ambient_gain", cloudTwilightAmbientGain},
        {"sun_gain_elev_band", sunGainElevBand},
        {"twilight_band_hi", twilightBandHi},
        {"twilight_band_lo", twilightBandLo},
        {"coverage_mip_lod", coverageMipLod},
        {"flat_coverage_scale", flatCoverageScale},
        {"flat_sun_gain_scale", flatSunGainScale},
        {"cloud_dist_fade_start_m", cloudDistFadeStartM},
        {"cloud_dist_fade_end_m", cloudDistFadeEndM},
        {"terrain_dist_fade_start_m", terrainDistFadeStartM},
        {"terrain_dist_fade_end_m", terrainDistFadeEndM},
        {"cloud_base_variance", cloudBaseVariance},
        {"cloud_erosion_edge", cloudErosionEdge},
        {"cloud_erosion_core", cloudErosionCore},
        {"hg_g", cloudHgG},
        {"march_steps", cloudMarchSteps},
        {"light_steps", cloudLightSteps},
        {"cirrus_wind_deg", cloudCirrusWindDeg},
        {"cirrus_stretch", cloudCirrusStretch},
        {"airglow_gain", airglowGain},
        {"airglow_green_gain", airglowGreenGain},
        {"airglow_red_gain", airglowRedGain},
        {"airglow_sodium_gain", airglowSodiumGain},
        {"airglow_coverage_gain", airglowCoverageGain},
        {"airglow_polar_gain", airglowPolarGain},
        {"zodiacal_gain", zodiacalGain},
        {"ocean_mw_refl_gain", oceanMwReflGain},
        {"zodiacal_width_deg", zodiacalWidthDeg},
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
        {"aurora_shimmer_rate", auroraShimmerRate},
        {"beam_gain", beamGain},
        {"beam_max_range_m", beamMaxRangeM},
        {"beam_sky_glow_gain", beamSkyGlowGain},
        {"reflector_lock_window_s", reflectorLockWindowS},
        {"mirror_max_rate_deg_per_sec", mirrorMaxRateDegPerSec},
        {"reflector_min_elev_deg", reflectorMinElevDeg},
        {"beam_glow_bleed_gain", beamGlowBleedGain},
        {"cloud_shadow_range_m", cloudShadowRangeM},
        {"beam_near_field_fade_m", beamNearFieldFadeM},
        {"beam_cluster_dir_threshold_deg", beamClusterDirThresholdDeg},
        {"beam_cluster_fade_in_s", beamClusterFadeInS},
        {"beam_cluster_fade_out_s", beamClusterFadeOutS},
        {"fog_top_alt_m", fogTopAltM},
        {"fog_density", fogDensity},
        {"fog_coverage", fogCoverage},
        {"fog_sun_gain", fogSunGain},
        {"cloud_opacity_scale", cloudOpacityScale},
        {"city_light_blur_lod", cityLightBlurLod},
        {"cloud_warp_strength", cloudWarpStrength},
        {"cloud_warp_freq", cloudWarpFreq},
        {"cloud_surface_carve", cloudSurfaceCarve},
        {"cloud_erosion_billow", cloudErosionBillow},
        {"cloud_erosion_billow_h", cloudErosionBillowH},
        {"cloud_erosion_freq", cloudErosionFreq},
        {"cloud_multi_scatter", cloudMultiScatter},
        {"cloud_shadow_floor_t", cloudShadowFloorT},
        {"cloud_graze_shadow", cloudGrazeShadow},
        {"cloud_cone_len_scale", cloudConeLenScale},
        {"cloud_vert_shade_gain", cloudVertShadeGain},
        {"cloud_density_ao", cloudDensityAO},
        {"cloud_ao_power", cloudAOPower},
        {"cloud_flat_density_scale", flatDensityScale},
        {"cloud_flat_rayleigh_gain", flatRayleighGain},
        {"cloud_flat_twilight_ambient_gain", flatTwilightAmbientGain},
        {"atmos_rayleigh_gain", atmosRayleighGain},
        {"atmos_mie_gain", atmosMieGain},
        {"atmos_term_strength", atmosTermStrength},
        {"atmos_term_width", atmosTermWidth}};

    nlohmann::json kbArr = nlohmann::json::array();
    for (const auto &kb : keybindings)
        kbArr.push_back({{"action", kb.action}, {"key", kb.key}, {"gp_button", kb.gpButton}});
    j["controls"]["keybindings"] = kbArr;
    j["controls"]["invert_mouse_x"] = invertMouseX;
    j["controls"]["invert_mouse_y"] = invertMouseY;
    j["controls"]["invert_pad_x"] = invertPadX;
    j["controls"]["invert_pad_y"] = invertPadY;

    nlohmann::json constArr = nlohmann::json::array();
    for (const auto &c : constellations)
        constArr.push_back({{"name", c.name}, {"enabled", c.enabled}, {"highlight", c.highlight}});
    j["constellations"] = constArr;

    // Planets (RELEASE_v1_1_PLAN.md follow-up, session 30) — same shape as constellations above,
    // ungated by schema_version like constellations_enabled/constellations: this isn't a
    // graphics-tuning value that a version mismatch could make dangerous to silently reapply.
    nlohmann::json planetArr = nlohmann::json::array();
    for (int pi = 0; pi < kPlanetCount; ++pi)
        planetArr.push_back({{"name", kPlanetNames[pi]}, {"enabled", planetEnabled[pi]}});
    j["planets"] = {{"show_planets", showPlanets}, {"list", planetArr}};
    return j;
}

// ─── savePerfSnapshot ───────────────────────────────────────────────────────
// Appends one JSON record — system status + the EMA-averaged GPU pass timings
// from gpuMsSmoothed[]/gpuMsTotalSmoothed (see updateGpuTimingStats in
// SatelliteSim.cpp) — to perf_profiles/profile_log.jsonl in the per-user data
// directory (NEW-4). JSON Lines (one object per line) rather than a JSON array
// so the log can grow across sessions/restarts by simple appending and be
// bulk-loaded later (e.g. pandas.read_json(path, lines=True)) without ever
// re-parsing the whole file to add an entry.
// Split out of savePerfSnapshot() so the automated knockout sweep can reuse every context field
// (build identity, GPU, resolution, observer, camera, sim time, quality settings, ...) verbatim
// and simply attach its own "knockout_sweep" object — rather than growing a second, inevitably
// drifting copy of the same 100 lines. savePerfSnapshot() is now just this plus a write.
nlohmann::json SatelliteSim::buildPerfSnapshotJson(float cpuDt)
{
    nlohmann::json j;

    // Build identity (NEW-1) — without this, snapshots from different builds/commits can't be
    // told apart once re-tuned defaults or quality-slider ranges change.
    j["app_version"] = APP_VERSION;
    j["git_commit"] = APP_GIT_COMMIT;

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
        // render_scale matters more than any other quality field for interpreting these numbers:
        // the sky/terrain/cloud background renders at this fraction of the swapchain extent while
        // scene_depth and the cloud targets are ALWAYS half the full extent. So the relative cost
        // of those fixed-size passes rises sharply as this drops, and two snapshots at different
        // render scales are not comparable even at identical resolution/viewpoint.
        {"render_scale", renderScale},
        {"cloud_march_steps", cloudMarchSteps},
        {"cloud_light_steps", cloudLightSteps},
        {"cloud_coverage", cloudCoverage},
        // The three that actually bound cloud_march's cost, added while tuning the sun
        // self-shadow cone (CLOUD_PERF_PLAN.md Tier 3): without shadow_max_dist_m in particular,
        // two captures at the same viewpoint and the same light-step count were indistinguishable
        // in the log even though the shadow range between them had been changed deliberately —
        // which is the whole reason the capture was taken.
        {"cloud_shadow_max_dist_m", cloudShadowMaxDistM},
        {"cloud_max_render_dist_m", cloudMaxRenderDistM},
        {"cloud_dist_fade_end_m", cloudDistFadeEndM},
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
    // Index order matches gpuMsSmoothed[]'s slot semantics (C12 follow-up #22: orbit now runs
    // before cloud march — see updateGpuTimingStats()'s comment) — key NAMES, not indices, are
    // what matter for reading old snapshots; a re-ordering like this changes which index a given
    // name reads from going forward, so don't compare index positions across the reorder.
    // beam_cloud_block is new as of the pipeline-unification pass; snapshots taken before it
    // exists have no such key, and their orbit_compute value silently INCLUDES this cost.
    // cloud_shadow_map disappeared when that dispatch was folded into cloud_march.comp - its
    // cost now lives inside the cloud_march bucket. Snapshots that still carry the old key
    // predate that change.
    // 2026-08-09: beam_cloud_block.comp itself was retired (replaced by beam_self_march.comp,
    // dispatched in a different pipeline position — see SatelliteSim.cpp's recordCompute) but its
    // timestamp slot (gpuMsSmoothed[1]) was deliberately left in place rather than rewired, so this
    // key now reads ~0 always. beam_self_march.comp's real cost instead folds into orbit_compute
    // (gpuMsSmoothed[2]) — it's dispatched between sat_orbit.comp's own barriers and that bucket's
    // timestamp write, the same reasoning bit-1024/scene-depth's "reads ~0 when skipped" convention
    // already establishes for a bucket with nothing left to measure.
    j["gpu_timing_ms"] = {
        {"scene_depth", gpuMsSmoothed[0]},
        {"beam_cloud_block", gpuMsSmoothed[1]},
        {"orbit_compute", gpuMsSmoothed[2]},
        {"cloud_march", gpuMsSmoothed[3]},
        {"flare_compute", gpuMsSmoothed[4]},
        {"sky_background_draw", gpuMsSmoothed[5]},
        {"satellite_star_draw", gpuMsSmoothed[6]},
        {"ui_overlay", gpuMsSmoothed[7]},
        {"total", gpuMsTotalSmoothed}};
    j["cpu_frame"] = {
        {"dt_ms", cpuDt * 1000.0f},
        {"fps", cpuDt > 0.0f ? 1.0f / cpuDt : 0.0f}};
    // EMA-smoothed CPU bucket breakdown (2026-08-10) — the counterpart to gpu_timing_ms. On a
    // knockout_sweep record this is superseded by knockout_sweep.baseline_cpu, which is averaged
    // over the sweep's own window rather than EMA-smoothed; both are kept so a plain snapshot
    // still carries the breakdown.
    {
        static const char *kKeys[CPU_COUNT] = {
            "build_ui", "update_positions", "beam_readback", "update_stars",
            "light_pollution_dome", "update_planets"};
        nlohmann::json c;
        float measured = 0.0f;
        for (int i = 0; i < CPU_COUNT; ++i)
        {
            c[kKeys[i]] = cpuMsSmoothed[i];
            measured += cpuMsSmoothed[i];
        }
        c["measured_total"] = measured;
        c["other"] = cpuDt * 1000.0f - gpuMsTotalSmoothed - measured;
        j["cpu_timing_ms"] = c;
    }
    // Which knockout toggles (if any) were active when this snapshot was taken — a
    // snapshot captured mid-profiling with bits set is only meaningful alongside this.
    j["debug_disable_mask"] = debugDisableMask;

    // Reflect-Orbital beam load (2026-08-10). Added because the Anchorage worst-case captures were
    // uninterpretable without it: the per-pixel pointing-ray loop in cloud_march.comp iterates
    // min(beamCount, 2048) times for EVERY half-res pixel, so beam count is a first-order driver of
    // that bucket's cost — yet nothing in the log recorded it, and it varies enormously by observer
    // location (a site that is itself a reflector target with few neighbours concentrates far more
    // beams than a typical viewpoint). show_beam_rays matters for the same reason: it is the only
    // thing that gated that loop before bit 8192 existed, so two otherwise-identical snapshots can
    // differ by several ms on this field alone.
    j["beams"] = {
        {"active_count", lastActiveBeamCount},
        {"ground_spot_count", lastGroundBeamCount},
        {"max_active_capacity", kMaxActiveBeams},
        {"ground_spot_capacity", kMaxGroundBeams},
        {"show_beam_rays", showBeamDebugRays},
        {"max_range_m", beamMaxRangeM}};

    j["graphics_preset"] = kGraphicsPresetNames[(int)graphicsPreset];

    // Frame limiter (2026-08-10). Without it a CPU-vs-GPU gap is ambiguous: under VSync/FIFO the
    // wall-clock frame time is quantised to the display's refresh interval, so a frame that
    // "costs" 16.7 ms may really be 6 ms of work waiting on present — which is exactly what
    // Planetarium looked like once its GPU total fell to ~6 ms. Under a numeric cap or Off there
    // is no such quantisation and the gap is real work. Also records the build config, since these
    // numbers were being read across Debug and Release captures and only the CPU side differs.
    {
        static const char *kCapNames[] = {"Off", "Cap30", "Cap60", "Cap120", "VSync"};
        int capIdx = (int)fpsCapMode;
        j["frame_limiter"] = (capIdx >= 0 && capIdx < 5) ? kCapNames[capIdx] : "unknown";
    }
#ifdef NDEBUG
    j["build_config"] = "Release";
#else
    j["build_config"] = "Debug";
#endif

    return j;
}

// One JSONL line appended to perf_profiles/profile_log.jsonl in the per-user data directory.
void SatelliteSim::appendPerfRecord(const nlohmann::json &j)
{
    if (userDataDir_.empty())
        return;
    auto dir = std::filesystem::path(userDataDir_) / "perf_profiles";
    auto path = dir / "profile_log.jsonl";
    try
    {
        std::filesystem::create_directories(dir);
        std::ofstream f(path, std::ios::app);
        f << j.dump() << '\n';
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "[SatelliteSim] Failed to append perf record: %s\n", e.what());
    }
}

void SatelliteSim::savePerfSnapshot(float cpuDt)
{
    if (userDataDir_.empty())
        return;
    nlohmann::json j = buildPerfSnapshotJson(cpuDt);
    j["record_kind"] = "snapshot";
    appendPerfRecord(j);
    snapshotMsgTimer = 1.5f;
}

// ─── Automated knockout sweep ────────────────────────────────────────────────
// Walks kDebugToggles on its own: step 0 measures the baseline (mask 0), steps 1..N each measure
// exactly one bit set. Every step holds its mask for kSweepSettleFrames discarded frames and then
// accumulates gpuMsRaw[] over kSweepSampleFrames, so the result is a real average over a fixed
// window rather than a single noisy sample or a half-settled EMA read.
//
// Two things are forced for the sweep's duration and restored afterwards:
//   - the user's own debugDisableMask (the sweep needs a clean baseline to subtract, and a
//     user-set bit would make every "delta" a difference against the wrong reference)
//   - sim time is PAUSED, so all N+1 steps measure the same frame. Without this a sweep of even a
//     few seconds at 30fps sees satellites move, beams re-target and clouds drift, and the
//     resulting per-bit deltas mix real cost against scene change — the single most likely way to
//     read a spurious result out of this tool.
// Camera/observer are already static unless the user moves them, which is what the on-screen
// progress readout is for.
void SatelliteSim::startKnockoutSweep()
{
    static_assert(kDebugToggleCount == kDebugToggleSlots,
                  "hovDebugToggle[]/sweepAccum[] in SatelliteSim.h are sized by kDebugToggleSlots — "
                  "bump it in the same edit that adds a kDebugToggles row");
    if (sweepActive || userDataDir_.empty())
        return;
    // Every number the sweep produces comes from the timestamp query pool. Without it the record
    // would be a full page of zeros that looks like a measurement — refuse rather than mislead.
    if (!ctx_ || ctx_->timestampPeriodNs <= 0.0)
    {
        fprintf(stderr, "[SatelliteSim] Knockout sweep unavailable: GPU timestamp queries not supported.\n");
        return;
    }
    sweepActive = true;
    sweepStep = 0;
    sweepFrame = 0;
    sweepSavedMask = debugDisableMask; // the BASELINE — see the header comment on why this is not 0
    sweepSavedPaused = timePaused;
    timePaused = true;
    // Measure only what this configuration still renders. A bit the preset already disables has
    // nothing left to knock out, so setting it again would just re-measure the baseline.
    sweepBitCount = 0;
    for (int i = 0; i < kDebugToggleCount; ++i)
        if ((sweepSavedMask & kDebugToggles[i].bit) == 0u)
            sweepBits[sweepBitCount++] = i;
    for (int s = 0; s <= kDebugToggleSlots + 1; ++s)
    {
        sweepAccumTotal[s] = 0.0f;
        sweepAccumCpu[s] = 0.0f;
        for (int b = 0; b < 8; ++b)
            sweepAccum[s][b] = 0.0f;
        for (int c = 0; c < CPU_COUNT; ++c)
            sweepAccumCpuBucket[s][c] = 0.0f;
    }
}

void SatelliteSim::updateKnockoutSweep(float cpuDt)
{
    if (sweepDoneMsgTimer > 0.0f)
        sweepDoneMsgTimer = std::max(0.0f, sweepDoneMsgTimer - cpuDt);
    if (!sweepActive)
        return;

    ++sweepFrame;
    if (sweepFrame > kSweepSettleFrames)
    {
        for (int b = 0; b < 8; ++b)
            sweepAccum[sweepStep][b] += gpuMsRaw[b];
        sweepAccumTotal[sweepStep] += gpuMsRawTotal;
        sweepAccumCpu[sweepStep] += cpuDt * 1000.0f;
        for (int c = 0; c < CPU_COUNT; ++c)
            sweepAccumCpuBucket[sweepStep][c] += cpuMsRaw[c];
    }
    if (sweepFrame < kSweepSettleFrames + kSweepSampleFrames)
        return;

    // Step complete — advance, or finish and write the record.
    ++sweepStep;
    sweepFrame = 0;
    if (sweepStep <= sweepBitCount)
    {
        // Baseline mask PLUS this row's bit — an incremental knockout on top of whatever the
        // preset already disables, not a from-scratch mask.
        debugDisableMask = sweepSavedMask | kDebugToggles[sweepBits[sweepStep - 1]].bit;
        return;
    }
    if (sweepStep == sweepBitCount + 1)
    {
        // Trailing baseline re-measure — see the sweepStep comment in SatelliteSim.h for why.
        debugDisableMask = sweepSavedMask;
        return;
    }

    const float inv = 1.0f / (float)kSweepSampleFrames;
    // Bucket key order matches gpuMsSmoothed[]'s slot semantics — same names savePerfSnapshot's
    // gpu_timing_ms uses, so a sweep step and a plain snapshot are directly comparable.
    static const char *kBucketKeys[8] = {
        "scene_depth", "beam_cloud_block", "orbit_compute", "cloud_march",
        "flare_compute", "sky_background_draw", "satellite_star_draw", "ui_overlay"};
    // Order must match the CpuBucket enum in SatelliteSim.h.
    static const char *kCpuBucketKeys[CPU_COUNT] = {
        "build_ui", "update_positions", "beam_readback", "update_stars",
        "light_pollution_dome", "update_planets"};
    auto bucketsOf = [&](int step)
    {
        nlohmann::json o;
        for (int b = 0; b < 8; ++b)
            o[kBucketKeys[b]] = sweepAccum[step][b] * inv;
        o["total"] = sweepAccumTotal[step] * inv;
        // Wall-clock frame time over the same window. `cpu_frame_ms` MINUS `total` is the
        // non-GPU-shader part of the frame — CPU work, present/vsync pacing, driver overhead — and
        // once the GPU total drops below the frame-cap interval that difference is what actually
        // bounds frame rate, so it belongs in the same record as the GPU buckets rather than being
        // reconstructed from a separate one-frame sample.
        o["cpu_frame_ms"] = sweepAccumCpu[step] * inv;
        return o;
    };
    // CPU bucket breakdown, same windows. "other" is what the wall-clock frame cost beyond the GPU
    // total and every measured bucket — present/vsync wait, driver submit, App-side work, and any
    // CPU block that doesn't have a bucket yet. Reported rather than hidden precisely because a
    // large "other" is the signal that the cost is somewhere this table doesn't look.
    auto cpuBucketsOf = [&](int step)
    {
        nlohmann::json o;
        float measured = 0.0f;
        for (int c = 0; c < CPU_COUNT; ++c)
        {
            float v = sweepAccumCpuBucket[step][c] * inv;
            o[kCpuBucketKeys[c]] = v;
            measured += v;
        }
        float wall = sweepAccumCpu[step] * inv;
        o["measured_total"] = measured;
        o["other"] = wall - sweepAccumTotal[step] * inv - measured;
        return o;
    };

    // Restore the user's state BEFORE building the record, so the record's own
    // debug_disable_mask/time_scale fields describe the session rather than the sweep's last step.
    debugDisableMask = sweepSavedMask;
    timePaused = sweepSavedPaused;
    sweepActive = false;

    nlohmann::json j = buildPerfSnapshotJson(cpuDt);
    j["record_kind"] = "knockout_sweep";
    // Overwrite the EMA-derived gpu_timing_ms buildPerfSnapshotJson just filled in. At this instant
    // gpuMsSmoothed[] is still decaying out of the sweep's LAST knockout step, so it describes a
    // deliberately-broken renderer, not this viewpoint. The sweep's own baseline window is the
    // honest answer to "what does this scene cost", and putting it under the standard key keeps
    // sweep records directly comparable to plain snapshots in the same log.
    j["gpu_timing_ms"] = bucketsOf(0);

    nlohmann::json steps = nlohmann::json::array();
    const float baseTotal = sweepAccumTotal[0] * inv;
    // Rows the baseline mask already disabled were never measured — list them by key so a reader
    // can tell "this preset doesn't render that" apart from "that turned out to be free."
    nlohmann::json alreadyOff = nlohmann::json::array();
    for (int i = 0; i < kDebugToggleCount; ++i)
        if ((sweepSavedMask & kDebugToggles[i].bit) != 0u)
            alreadyOff.push_back(kDebugToggles[i].jsonKey);
    for (int si = 0; si < sweepBitCount; ++si)
    {
        const int i = sweepBits[si];
        nlohmann::json s;
        s["key"] = kDebugToggles[i].jsonKey;
        s["label"] = kDebugToggles[i].label;
        s["bit"] = kDebugToggles[i].bit;
        s["buckets"] = bucketsOf(si + 1);
        // Positive = this block costs that many ms. A knockout can legitimately come out slightly
        // NEGATIVE (measurement noise, or a skip that makes a later pass do more work because
        // nothing occludes it any more — bit 1024/scene depth is the standing example), so this is
        // deliberately not clamped: a negative number is information, not an error to hide.
        // si + 1, NOT i + 1. `i` is the row's index in kDebugToggles; `si` is its slot in the
        // accumulators, and the two only coincide when the baseline mask is 0 (which is why this
        // read correctly on Medium/Ultra and produced garbage on Planetarium, whose mask skips
        // eight rows: it indexed accumulator slots that were never written, so cost_ms came back as
        // the full baseline). The stored buckets were always right — only this scalar was wrong.
        s["cost_ms"] = baseTotal - sweepAccumTotal[si + 1] * inv;
        steps.push_back(s);
    }
    j["knockout_sweep"] = {
        {"settle_frames", kSweepSettleFrames},
        {"sample_frames", kSweepSampleFrames},
        {"baseline_mask", sweepSavedMask},
        {"baseline", bucketsOf(0)},
        {"baseline_cpu", cpuBucketsOf(0)},
        // Same mask, measured again after every knockout step. If baseline_end differs materially
        // from baseline, the scene moved during the sweep and every cost_ms carries that drift —
        // retake rather than reason around it.
        {"baseline_end", bucketsOf(sweepBitCount + 1)},
        {"baseline_drift_ms", sweepAccumTotal[sweepBitCount + 1] * inv - baseTotal},
        {"already_disabled", alreadyOff},
        {"steps", steps}};

    appendPerfRecord(j);
    lastSweepRecordJson = j.dump();
    ++sweepsCompleted;
    sweepDoneMsgTimer = 3.0f;
    printf("[SatelliteSim] Knockout sweep complete (%d steps measured, %d already disabled by the "
           "baseline mask 0x%X, baseline %.2f ms) -> profile_log.jsonl\n",
           sweepBitCount, kDebugToggleCount - sweepBitCount, sweepSavedMask, baseTotal);
}
