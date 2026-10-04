// SatelliteSimTutorial.cpp — the first-run tutorial (2026-10-03).
//
// Playtesters did not find the controls: the intro's last beat printed "WASD to move / Q-E / click a
// satellite" while clicking was still disabled, and a separate "Click to select any satellite" hint
// repeated it after the intro. This replaces both. One card at a time, near the bottom of the view or
// beside the HUD control it describes:
//   look, move, climb, boost, select    — finished by DOING it; a keyboard / mouse / gamepad graphic
//                                         lights the input to use (amber) and what is held (green)
//   the satellite's buttons, time,       — outlines on the HUD's own controls; finished by using one
//   pictures, menus                        of them, or Next
// It follows the intro (finishIntro), or the first frame when the intro is off, once: Skip or Finish
// sets display.tutorial_done. Settings > Display > Replay Tutorial runs it again. A harness run never
// starts it on its own (`tutorial start [step]` does).
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../AudioSystem.h"
#include "clay.h"
#include "UIPalette.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>

// SatelliteSimUI.cpp: the names the settings window shows for a key / a gamepad button.
const char *keyDisplayName(int key);
const char *gamepadButtonDisplayName(int button);

namespace
{
// The HUD's palette (UIPalette.h): grey panels, the red accent for "this one" and "held".
constexpr Clay_Color kKeyTargetBorder = {215, 45, 45, 255}; // scaled by the pulse below
constexpr Clay_Color kKeyHeld = {190, 32, 32, 250};
constexpr Clay_Color kGraphicBody = {20, 20, 22, 235};

constexpr int16_t kZCard = 40;      // above the notices (25) and the selection panel (6)
constexpr float kDoneHoldS = 1.1f;  // a finished step's "Done" before the next card
constexpr float kLastHoldS = 3.0f;  // the last card's closing line
constexpr float kLookDeg = 70.0f;   // degrees of look (yaw + pitch) that finish the look step
constexpr float kMoveS = 1.5f;      // seconds of moving / climbing / boosting that finish those steps
constexpr float kBoostS = 1.0f;

const char *const kTitles[] = {
    "Look around", "Move over the Earth", "Climb and descend", "Go faster", "Pick a satellite",
    "Your satellite", "Time", "Pictures", "Menus"};

Clay_String clayStr(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }

// One keycap or pad button: `target` = this step wants it (amber, pulsing), `down` = held now (green).
// `fl` places it as a floating child of the element being built (the gamepad's sticks and buttons).
void tutCap(Clay_ElementId id, float w, float h, Clay_CornerRadius rad, const char *label, uint16_t fsz,
            bool target, bool down, float pulse, const Clay_FloatingElementConfig *fl = nullptr)
{
    const Clay_Color bg = down     ? kKeyHeld
                          : target ? Clay_Color{70.0f + 50.0f * pulse, 22, 22, 235}
                                   : Pal::btnIdle;
    const Clay_Color bc = (target || down) ? Clay_Color{kKeyTargetBorder.r, kKeyTargetBorder.g, kKeyTargetBorder.b, 140.0f + 115.0f * pulse}
                                           : Clay_Color{255, 255, 255, 40};
    const uint16_t bw = target ? 2 : 1;
    Clay_ElementDeclaration d{};
    d.layout.sizing = {CLAY_SIZING_FIXED(w), CLAY_SIZING_FIXED(h)};
    d.layout.childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER};
    d.backgroundColor = bg;
    d.cornerRadius = rad;
    if (fl)
        d.floating = *fl;
    d.border = {.color = bc, .width = {bw, bw, bw, bw, 0}};
    CLAY(id, d)
    {
        if (label && label[0])
            CLAY_TEXT(clayStr(label), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fsz, .wrapMode = CLAY_TEXT_WRAP_NONE}));
    }
}

Clay_FloatingElementConfig childAt(float x, float y)
{
    Clay_FloatingElementConfig f{};
    f.offset = {x, y};
    f.zIndex = kZCard + 1;
    f.pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH;
    f.attachTo = CLAY_ATTACH_TO_PARENT;
    return f;
}
} // namespace

// ─── State ────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::startTutorial(int step)
{
    tutActive = true;
    tutAutoChecked = true;
    setTutorialStep(std::clamp(step, 0, TUT_COUNT - 1));
}

void SatelliteSim::endTutorial(bool finished)
{
    (void)finished; // Skip and Finish both mean "don't show it again"; Replay Tutorial brings it back
    tutActive = false;
    tutorialDone = true;
    tutDoneT = -1.0f;
}

void SatelliteSim::setTutorialStep(int step)
{
    if (step >= TUT_COUNT)
    {
        endTutorial(true);
        return;
    }
    tutStep = std::max(0, step);
    tutProgress = 0.0f;
    tutDoneT = -1.0f;
    tutLastAz = camera.azDeg;
    tutLastEl = camera.elDeg;
    tutBaseTimeIdx = timeScaleIdx;
    tutBasePaused = timePaused;
    tutBaseReverse = timeDir < 0.0f;
    tutBaseTrails = trailEnabled;
    tutBaseCine = cineChrome.open;
    tutBaseBookmarks = bmChrome.open;
    tutBaseSettings = settingsChrome.open;
    tutBaseAction = tutActionOn();
}

// Any of the selection's actions in use: its info window, Go to, Trace pass, Track.
bool SatelliteSim::tutActionOn() const
{
    if (selectedSatIndex < 0)
        return false;
    return (infoChrome.open && viewerSatIndex == selectedSatIndex) || followActive || traceChrome.open || trackActive;
}

// ─── Completion ───────────────────────────────────────────────────────────────────────────────────
// Every frame, after buildUI's look block (this frame's camera turn is in camera.azDeg/elDeg) and
// before the left-click pick. Reads the keys itself rather than hooking the movement code: the steps
// only need to know that the player pressed them while the camera could move.
void SatelliteSim::updateTutorial(float dt)
{
    if (!tutAutoChecked)
    {
        // The intro was off (finishIntro starts it otherwise): the first frame in the scene.
        tutAutoChecked = true;
        if (!tutorialDone && !tutActive && !harnessRunner_)
            startTutorial();
    }
    if (!tutActive)
        return;
    tutClock += dt;

    float dAz = camera.azDeg - tutLastAz;
    dAz -= 360.0f * std::round(dAz / 360.0f);
    const float dEl = camera.elDeg - tutLastEl;
    tutLastAz = camera.azDeg;
    tutLastEl = camera.elDeg;

    if (tutDoneT >= 0.0f)
    {
        tutDoneT += dt;
        if (tutDoneT > (tutStep + 1 >= TUT_COUNT ? kLastHoldS : kDoneHoldS))
            setTutorialStep(tutStep + 1);
        return;
    }

    const bool live = win && !consoleOpen_ && !textEditing() && !cineActive();
    auto key = [&](int k) { return live && k >= 0 && glfwGetKey(win, k) == GLFW_PRESS; };
    const bool moving = key(GLFW_KEY_W) || key(GLFW_KEY_A) || key(GLFW_KEY_S) || key(GLFW_KEY_D) ||
                        (live && (gpMoveFwd != 0.0f || gpMoveRight != 0.0f));
    const bool climbing = key(keybindings[KB_RAISE_ELEV].key) || key(keybindings[KB_LOWER_ELEV].key) ||
                          (live && (gpElevRaise > 0.0f || gpElevLower > 0.0f));
    const bool boost = key(keybindings[KB_MOVE_BOOST].key) || (live && gpHeld(KB_MOVE_BOOST));
    const bool looking = live && (camera.captured || gpLookYawDeg != 0.0f || gpLookPitchDeg != 0.0f);

    switch (tutStep)
    {
    case TUT_LOOK:
        if (looking)
            tutProgress += (std::fabs(dAz) + std::fabs(dEl)) / kLookDeg;
        break;
    case TUT_MOVE:
        if (moving)
            tutProgress += dt / kMoveS;
        break;
    case TUT_ALTITUDE:
        if (climbing)
            tutProgress += dt / kMoveS;
        break;
    case TUT_BOOST:
        if (boost && (moving || climbing))
            tutProgress += dt / kBoostS;
        break;
    case TUT_SELECT:
        tutProgress = selectedSatIndex >= 0 ? 1.0f : 0.0f;
        break;
    case TUT_ACTIONS:
        // Already in use when the card came up (a replay with the info window open): Next moves on.
        if (!tutBaseAction && tutActionOn())
            tutProgress = 1.0f;
        break;
    case TUT_TIME:
        if (timeScaleIdx != tutBaseTimeIdx || timePaused != tutBasePaused || (timeDir < 0.0f) != tutBaseReverse)
            tutProgress = 1.0f;
        break;
    case TUT_CAPTURE:
        if (trailEnabled != tutBaseTrails || photoState != 0 || screenshotRequested || screenshotCopyPending || screenshotEncoding.load())
            tutProgress = 1.0f;
        break;
    case TUT_SETTINGS:
        if ((settingsChrome.open && !tutBaseSettings) || cineChrome.open != tutBaseCine || bmChrome.open != tutBaseBookmarks)
            tutProgress = 1.0f;
        break;
    default:
        break;
    }
    if (tutProgress >= 1.0f)
    {
        tutProgress = 1.0f;
        tutDoneT = 0.0f;
        if (audio_)
            audio_->playSfx("assets/sound/ui/buttonclick.wav");
    }
}

// ─── Graphics ─────────────────────────────────────────────────────────────────────────────────────
// Q W E / A S D / Shift, labelled from the live bindings (W A S D are fixed in the movement code).
void SatelliteSim::buildTutKeyboard(int step)
{
    const float pulse = 0.5f + 0.5f * sinf(tutClock * 4.5f);
    const float k = fs(13) * 1.85f;
    const float gap = std::max(3.0f, k * 0.1f);
    const uint16_t fsz = fs(13);
    const Clay_CornerRadius r = CLAY_CORNER_RADIUS(4);
    const bool live = win && !consoleOpen_ && !textEditing();
    auto down = [&](int key) { return live && key >= 0 && glfwGetKey(win, key) == GLFW_PRESS; };
    // keyDisplayName's letters come from a 4-slot rotating pool, and Clay reads the label pointers after
    // buildUI returns: copy them.
    snprintf(tutKeyLabel[0], sizeof(tutKeyLabel[0]), "%s", keyDisplayName(keybindings[KB_RAISE_ELEV].key));
    snprintf(tutKeyLabel[1], sizeof(tutKeyLabel[1]), "%s", keyDisplayName(keybindings[KB_LOWER_ELEV].key));
    snprintf(tutKeyLabel[2], sizeof(tutKeyLabel[2]), "%s", keyDisplayName(keybindings[KB_MOVE_BOOST].key));
    const bool tMove = step == TUT_MOVE, tAlt = step == TUT_ALTITUDE, tBoost = step == TUT_BOOST;

    CLAY(CLAY_ID("TutKbd"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                        .childGap = (uint16_t)gap,
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY(CLAY_ID("TutKbdRow0"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .childGap = (uint16_t)gap}})
        {
            tutCap(CLAY_ID("TutKeyQ"), k, k, r, tutKeyLabel[0], fsz, tAlt, down(keybindings[KB_RAISE_ELEV].key), pulse);
            tutCap(CLAY_ID("TutKeyW"), k, k, r, "W", fsz, tMove, down(GLFW_KEY_W), pulse);
            tutCap(CLAY_ID("TutKeyE"), k, k, r, tutKeyLabel[1], fsz, tAlt, down(keybindings[KB_LOWER_ELEV].key), pulse);
        }
        CLAY(CLAY_ID("TutKbdRow1"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                .padding = {(uint16_t)(k * 0.35f), 0, 0, 0},
                                                .childGap = (uint16_t)gap}})
        {
            tutCap(CLAY_ID("TutKeyA"), k, k, r, "A", fsz, tMove, down(GLFW_KEY_A), pulse);
            tutCap(CLAY_ID("TutKeyS"), k, k, r, "S", fsz, tMove, down(GLFW_KEY_S), pulse);
            tutCap(CLAY_ID("TutKeyD"), k, k, r, "D", fsz, tMove, down(GLFW_KEY_D), pulse);
        }
        CLAY(CLAY_ID("TutKbdRow2"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}}})
        {
            tutCap(CLAY_ID("TutKeyShift"), k * 2.4f, k, r, tutKeyLabel[2], fsz, tBoost, down(keybindings[KB_MOVE_BOOST].key), pulse);
        }
    }
}

// A mouse seen from above: the button the step wants in amber, held buttons green, the wheel.
void SatelliteSim::buildTutMouse(int step, const UIInput &inp)
{
    const float pulse = 0.5f + 0.5f * sinf(tutClock * 4.5f);
    const float w = fs(13) * 3.4f;
    const float hb = w * 0.62f, hBody = w * 0.85f, rr = w * 0.45f;
    const bool tL = step == TUT_SELECT, tR = step == TUT_LOOK;
    CLAY(CLAY_ID("TutMouse"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                          .childGap = 2,
                                          .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY(CLAY_ID("TutMouseTop"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .childGap = 2}})
        {
            tutCap(CLAY_ID("TutMouseL"), w * 0.5f - 1.0f, hb, {rr, 2, 2, 2}, "", fs(11), tL, inp.lmbDown, pulse);
            tutCap(CLAY_ID("TutMouseR"), w * 0.5f - 1.0f, hb, {2, rr, 2, 2}, "", fs(11), tR, inp.rmbDown || camera.captured, pulse);
            Clay_FloatingElementConfig wheel = childAt(0.0f, 0.0f);
            wheel.attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER};
            tutCap(CLAY_ID("TutMouseWheel"), w * 0.13f, hb * 0.42f, CLAY_CORNER_RADIUS(w * 0.06f), "", fs(11), false,
                   inp.scrollY != 0.0f, pulse, &wheel);
        }
        tutCap(CLAY_ID("TutMouseBody"), w, hBody, {2, 2, rr, rr}, "", fs(11), false, false, pulse);
    }
}

// A twin-stick pad: shoulders above, sticks, D-pad and face buttons on the body.
void SatelliteSim::buildTutGamepad(int step)
{
    const float pulse = 0.5f + 0.5f * sinf(tutClock * 4.5f);
    const float u = fs(13) * 0.72f;
    const float W = u * 16.0f, H = u * 8.0f;
    const uint16_t fsz = fs(11);
    const bool pad = gamepadId >= 0;
    auto btn = [&](int b) { return pad && gpState.buttons[b] == GLFW_PRESS; };
    const bool dLS = gpMoveFwd != 0.0f || gpMoveRight != 0.0f || btn(GLFW_GAMEPAD_BUTTON_LEFT_THUMB);
    const bool dRS = gpLookYawDeg != 0.0f || gpLookPitchDeg != 0.0f || btn(GLFW_GAMEPAD_BUTTON_RIGHT_THUMB);
    const int selBtn = keybindings[KB_SELECT_SAT].gpButton;
    auto face = [&](int b) { return step == TUT_SELECT && b == selBtn; };

    CLAY(CLAY_ID("TutPad"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                        .childGap = 3,
                                        .layoutDirection = CLAY_TOP_TO_BOTTOM}})
    {
        CLAY(CLAY_ID("TutPadShoulders"), {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIT(0)},
                                                     .padding = {(uint16_t)(u * 1.2f), (uint16_t)(u * 1.2f), 0, 0},
                                                     .childGap = 3}})
        {
            const Clay_CornerRadius top = {u * 0.7f, u * 0.7f, 2, 2};
            tutCap(CLAY_ID("TutPadLT"), u * 2.8f, u * 1.5f, top, "LT", fsz, step == TUT_ALTITUDE, gpElevLower > 0.0f, pulse);
            tutCap(CLAY_ID("TutPadLB"), u * 2.8f, u * 1.5f, top, "LB", fsz, false, btn(GLFW_GAMEPAD_BUTTON_LEFT_BUMPER), pulse);
            CLAY(CLAY_ID("TutPadShoulderGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
            tutCap(CLAY_ID("TutPadRB"), u * 2.8f, u * 1.5f, top, "RB", fsz, false, btn(GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER), pulse);
            tutCap(CLAY_ID("TutPadRT"), u * 2.8f, u * 1.5f, top, "RT", fsz, step == TUT_ALTITUDE, gpElevRaise > 0.0f, pulse);
        }
        CLAY(CLAY_ID("TutPadBody"), {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}},
                                     .backgroundColor = kGraphicBody,
                                     .cornerRadius = CLAY_CORNER_RADIUS(u * 3.0f),
                                     .border = {.color = Style::borderColor, .width = {1, 1, 1, 1, 0}}})
        {
            const float ds = u * 3.0f; // stick
            const Clay_CornerRadius round = CLAY_CORNER_RADIUS(ds * 0.5f);
            Clay_FloatingElementConfig f = childAt(u * 1.8f, u * 0.9f);
            tutCap(CLAY_ID("TutPadLS"), ds, ds, round, "L", fsz, step == TUT_MOVE || step == TUT_BOOST, dLS, pulse, &f);
            f = childAt(W * 0.56f, H - ds - u * 0.6f);
            tutCap(CLAY_ID("TutPadRS"), ds, ds, round, "R", fsz, step == TUT_LOOK, dRS, pulse, &f);
            f = childAt(W * 0.27f, H - u * 3.2f);
            tutCap(CLAY_ID("TutPadDpad"), u * 2.4f, u * 2.4f, CLAY_CORNER_RADIUS(3), "+", fs(13), false,
                   btn(GLFW_GAMEPAD_BUTTON_DPAD_UP) || btn(GLFW_GAMEPAD_BUTTON_DPAD_DOWN) ||
                       btn(GLFW_GAMEPAD_BUTTON_DPAD_LEFT) || btn(GLFW_GAMEPAD_BUTTON_DPAD_RIGHT),
                   pulse, &f);
            const float db = u * 1.7f, cx = W * 0.80f, cy = H * 0.36f;
            const Clay_CornerRadius br = CLAY_CORNER_RADIUS(db * 0.5f);
            struct FaceBtn
            {
                int b;
                float dx, dy;
            };
            const FaceBtn faces[4] = {{GLFW_GAMEPAD_BUTTON_Y, 0.0f, -1.0f},
                                      {GLFW_GAMEPAD_BUTTON_X, -1.0f, 0.0f},
                                      {GLFW_GAMEPAD_BUTTON_B, 1.0f, 0.0f},
                                      {GLFW_GAMEPAD_BUTTON_A, 0.0f, 1.0f}};
            for (int i = 0; i < 4; ++i)
            {
                f = childAt(cx + faces[i].dx * db - db * 0.5f, cy + faces[i].dy * db - db * 0.5f);
                tutCap(CLAY_SIDI(CLAY_STRING("TutPadFace"), i), db, db, br, gamepadButtonDisplayName(faces[i].b), fsz,
                       face(faces[i].b), btn(faces[i].b), pulse, &f);
            }
        }
    }
}

// ─── The card ─────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildTutorial(const UIInput &inp, UIRenderer &ui)
{
    if (!tutActive)
        return;
    const bool pad = tutGamepadView();
    const float pulse = 0.5f + 0.5f * sinf(tutClock * 4.5f);
    const bool done = tutDoneT >= 0.0f;
    const int step = tutStep;
    const bool last = step + 1 >= TUT_COUNT;
    static_assert(sizeof(kTitles) / sizeof(kTitles[0]) == TUT_COUNT, "a title per step");

    // ── Outline what the card talks about (the HUD's own controls; boxes from the last layout) ──
    bool haveBox = false;
    Clay_BoundingBox box{};
    auto addBox = [&](Clay_ElementId id)
    {
        const Clay_ElementData d = Clay_GetElementData(id);
        if (!d.found || d.boundingBox.width <= 0.0f)
            return;
        if (!haveBox)
            box = d.boundingBox;
        else
        {
            const float x1 = std::max(box.x + box.width, d.boundingBox.x + d.boundingBox.width);
            const float y1 = std::max(box.y + box.height, d.boundingBox.y + d.boundingBox.height);
            box.x = std::min(box.x, d.boundingBox.x);
            box.y = std::min(box.y, d.boundingBox.y);
            box.width = x1 - box.x;
            box.height = y1 - box.y;
        }
        haveBox = true;
    };
    switch (step)
    {
    case TUT_ACTIONS:
        for (int id = 0; id < 8; ++id)
            if (selBtnDrawnMask & (1u << id))
                addBox(CLAY_SIDI(CLAY_STRING("SelActBtn"), id));
        break;
    case TUT_TIME:
        addBox(CLAY_ID("TimeSlowerBtn"));
        addBox(CLAY_ID("TimePauseBtn"));
        addBox(CLAY_ID("TimeFasterBtn"));
        addBox(CLAY_ID("TimeReverseBtn"));
        break;
    case TUT_CAPTURE:
        addBox(CLAY_ID("TimeScreenshotBtn"));
        addBox(CLAY_ID("TimePhotoBtn"));
        addBox(CLAY_ID("TrailsBtn"));
        break;
    case TUT_SETTINGS: // the right panel's menu buttons
        addBox(CLAY_ID("TimeBookmarkBtn"));
        addBox(CLAY_ID("TimeCineBtn"));
        addBox(CLAY_ID("SettingsBtn"));
        break;
    default:
        break;
    }
    if (haveBox && !done)
    {
        const float m = 4.0f;
        // PASSTHROUGH: it sits over the buttons it points at (CLAUDE.md, the pointer-capture rule).
        CLAY(CLAY_ID("TutOutline"), {.layout = {.sizing = {CLAY_SIZING_FIXED(box.width + 2.0f * m), CLAY_SIZING_FIXED(box.height + 2.0f * m)}},
                                     .backgroundColor = {200, 30, 30, 20.0f + 25.0f * pulse},
                                     .cornerRadius = CLAY_CORNER_RADIUS(6),
                                     .floating = {.offset = {box.x - m, box.y - m},
                                                  .zIndex = kZCard + 2,
                                                  .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                                                  .attachTo = CLAY_ATTACH_TO_ROOT},
                                     .border = {.color = {kKeyTargetBorder.r, kKeyTargetBorder.g, kKeyTargetBorder.b, 140.0f + 115.0f * pulse}, .width = {2, 2, 2, 2, 0}}}) {}
    }

    // ── The text, from the live bindings ──
    auto K = [&](int kb) { return keyDisplayName(keybindings[kb].key); };
    auto P = [&](int kb)
    {
        const int b = keybindings[kb].gpButton;
        return b < 0 ? "-" : gamepadButtonDisplayName(b);
    };
    char k1[16], k2[16], k3[16], k4[16];
    switch (step)
    {
    case TUT_LOOK:
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), pad ? "Push the right stick to look around. %s and %s zoom; click the right stick to reset it."
                                                      : "Hold the right mouse button and drag to look around. The mouse wheel zooms.",
                 P(KB_ZOOM_OUT), P(KB_ZOOM_IN));
        break;
    case TUT_MOVE:
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), "%s", pad ? "The left stick moves you over the ground, the way you are facing."
                                                           : "W A S D move you over the ground, the way you are facing.");
        break;
    case TUT_ALTITUDE:
        snprintf(k1, sizeof(k1), "%s", K(KB_RAISE_ELEV));
        snprintf(k2, sizeof(k2), "%s", K(KB_LOWER_ELEV));
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "RT climbs and LT descends: from the ground up to orbit and beyond.");
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "%s climbs and %s descends: from the ground up to orbit and beyond.", k1, k2);
        break;
    case TUT_BOOST:
        snprintf(k1, sizeof(k1), "%s", K(KB_MOVE_BOOST));
        snprintf(k2, sizeof(k2), "%s", K(KB_MOVE_FINE));
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Click and hold the left stick (%s) while moving to go much faster. Push the sticks gently to move slowly.",
                     P(KB_MOVE_BOOST));
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Hold %s while moving or climbing to go much faster. %s toggles slow, fine movement.", k1, k2);
        break;
    case TUT_SELECT:
        snprintf(k1, sizeof(k1), "%s", K(KB_SELECT_SAT));
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Put a satellite near the middle of the view and press %s to select it.", P(KB_SELECT_SAT));
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Left-click a satellite, any moving point of light, to select it. %s picks the one nearest the middle of the view.", k1);
        break;
    case TUT_ACTIONS:
        if (selectedSatIndex < 0)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Select a satellite first: its buttons appear beside it.");
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Info opens its 3D model, orbit and brightness. Go to flies you out to ride beside it. "
                                                     "Trace plots its brightness over the pass. Track keeps the camera on it.%s",
                     pad ? " D-pad left / right choose one, A presses it, B goes back." : " Try one.");
        break;
    case TUT_TIME:
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Speed time up (%s) or slow it down (%s), pause (%s) or reverse it (%s). The satellites, Sun and stars follow the clock.",
                     P(KB_FASTER), P(KB_SLOWER), P(KB_PAUSE), P(KB_REVERSE));
        else
        {
            snprintf(k1, sizeof(k1), "%s", K(KB_SLOWER));
            snprintf(k2, sizeof(k2), "%s", K(KB_PAUSE));
            snprintf(k3, sizeof(k3), "%s", K(KB_FASTER));
            snprintf(k4, sizeof(k4), "%s", K(KB_REVERSE));
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Slow down (%s), pause (%s), speed up (%s) or reverse (%s) time. The satellites, Sun and stars follow the clock.",
                     k1, k2, k3, k4);
        }
        break;
    case TUT_CAPTURE:
        snprintf(k1, sizeof(k1), "%s", K(KB_SCREENSHOT));
        snprintf(k2, sizeof(k2), "%s", K(KB_PHOTO));
        snprintf(k3, sizeof(k3), "%s", K(KB_TOGGLE_TRAILS));
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Screenshot (%s), HQ photo (%s) for a sharper image, and Star trails (%s) for a long exposure.%s",
                 k1, k2, k3, pad ? " On a controller, after the tutorial, Start gives a cursor for these buttons." : "");
        break;
    case TUT_SETTINGS:
        snprintf(k1, sizeof(k1), "%s", K(KB_TOGGLE_UI));
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), "Bookmarks save a place and time, Cinematics record camera paths, and Settings hold graphics, "
                                                 "constellations, sound and key bindings. %s hides the interface for a clean view.%s",
                 pad ? P(KB_TOGGLE_UI) : k1, pad ? " After the tutorial, Start gives a cursor for these buttons." : "");
        break;
    default:
        tutBodyBuf[0] = '\0';
        break;
    }
    snprintf(tutHeadBuf, sizeof(tutHeadBuf), "%d / %d", step + 1, (int)TUT_COUNT);

    // ── Where: beside the panel it points at, else low in the middle of the view ──
    Clay_FloatingElementConfig fl{};
    fl.zIndex = kZCard;
    if (step == TUT_TIME || step == TUT_CAPTURE)
    {
        fl.offset = {0.0f, -10.0f};
        fl.parentId = CLAY_ID("LeftPanel").id;
        fl.attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_TOP};
        fl.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
    }
    else if (step == TUT_SETTINGS)
    {
        fl.offset = {0.0f, -10.0f};
        fl.parentId = CLAY_ID("RightPanel").id;
        fl.attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_BOTTOM, .parent = CLAY_ATTACH_POINT_RIGHT_TOP};
        fl.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
    }
    else
    {
        // Above both corner panels (last frame's boxes): at 1600 px the card's width reached the right one.
        float clear = 24.0f;
        for (const Clay_ElementId id : {CLAY_ID("LeftPanel"), CLAY_ID("RightPanel")})
            if (const Clay_ElementData d = Clay_GetElementData(id); d.found && d.boundingBox.height > 0.0f)
                clear = std::max(clear, inp.screenH - d.boundingBox.y + 12.0f);
        fl.offset = {0.0f, -clear};
        fl.attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_BOTTOM, .parent = CLAY_ATTACH_POINT_CENTER_BOTTOM};
        fl.attachTo = CLAY_ATTACH_TO_ROOT;
    }

    // Wide and short: the graphic beside the text, so the card keeps to the bottom of the view.
    const float cardW = std::min(std::max(420.0f, fs(13) * 38.0f), inp.screenW - 24.0f);
    const bool graphic = step <= TUT_SELECT;
    const bool bar = step <= TUT_BOOST;
    bool clickNext = false, clickBack = false, clickSkip = false;
    auto button = [&](Clay_ElementId id, const char *label, bool accent, bool &hov) -> bool
    {
        bool clicked = false;
        CLAY(id, {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED((float)fs(12) + 10.0f)},
                             .padding = {12, 12, 0, 0},
                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                  .backgroundColor = accent ? (hov ? Pal::btnAccentHv : Pal::btnAccent) : (hov ? Pal::btnHover : Pal::btnIdle),
                  .cornerRadius = CLAY_CORNER_RADIUS(4)})
        {
            const bool n = Clay_Hovered();
            sndRollover(n, hov);
            sndClick(n, inp.lmbPressed);
            hov = n;
            clicked = n && inp.lmbPressed;
            CLAY_TEXT(clayStr(label), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12), .wrapMode = CLAY_TEXT_WRAP_NONE}));
        }
        return clicked;
    };

    CLAY(CLAY_ID("TutCard"), {.layout = {.sizing = {CLAY_SIZING_FIXED(cardW), CLAY_SIZING_FIT(0)},
                                         .padding = {14, 14, 10, 10},
                                         .childGap = 14,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT},
                              .backgroundColor = Pal::panelBg,
                              .cornerRadius = CLAY_CORNER_RADIUS(Style::panelCornerRadius),
                              .floating = fl,
                              .border = {.color = Style::borderColor, .width = {Style::borderWidthPx, Style::borderWidthPx, Style::borderWidthPx, Style::borderWidthPx, 0}}})
    {
        if (graphic)
        {
            CLAY(CLAY_ID("TutGraphic"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}}})
            {
                if (pad)
                    buildTutGamepad(step);
                else if (step == TUT_LOOK || step == TUT_SELECT)
                    buildTutMouse(step, inp);
                else
                    buildTutKeyboard(step);
            }
        }

        CLAY(CLAY_ID("TutText"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                             .childGap = 5,
                                             .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            CLAY(CLAY_ID("TutHead"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .childGap = 10,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
            {
                CLAY_TEXT(clayStr(kTitles[step]), CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(15), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY_TEXT(clayStr(tutHeadBuf), CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY(CLAY_ID("TutHeadGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                CLAY(CLAY_ID("TutSkip"), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {6, 6, 2, 2}},
                                          .backgroundColor = hovTutSkip ? Pal::btnHover : Clay_Color{0, 0, 0, 0},
                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
                {
                    const bool n = Clay_Hovered();
                    sndRollover(n, hovTutSkip);
                    sndClick(n, inp.lmbPressed);
                    hovTutSkip = n;
                    clickSkip = n && inp.lmbPressed;
                    ui.tooltip(inp, n, "Replay it from Settings > Display", fs(11));
                    CLAY_TEXT(CLAY_STRING("Skip tutorial"), CLAY_TEXT_CONFIG({.textColor = n ? Pal::btnLabel : Pal::textDim, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }

            CLAY_TEXT(clayStr(tutBodyBuf), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));

            if (done)
            {
                CLAY_TEXT(last ? CLAY_STRING("All set. Replay this any time from Settings > Display.") : CLAY_STRING("Done!"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(13)}));
            }
            else if (bar)
            {
                CLAY(CLAY_ID("TutBar"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(4)}},
                                         .backgroundColor = Pal::btnIdle,
                                         .cornerRadius = CLAY_CORNER_RADIUS(2)})
                {
                    CLAY(CLAY_ID("TutBarFill"), {.layout = {.sizing = {CLAY_SIZING_PERCENT(std::clamp(tutProgress, 0.0f, 1.0f)), CLAY_SIZING_GROW(0)}},
                                                 .backgroundColor = Pal::btnAccent,
                                                 .cornerRadius = CLAY_CORNER_RADIUS(2)}) {}
                }
            }

            CLAY(CLAY_ID("TutFoot"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
            {
                if (step > 0)
                    clickBack = button(CLAY_ID("TutBack"), "Back", false, hovTutBack);
                // A controller moves through the card with Start / View (pollGamepad), no cursor needed.
                if (pad)
                    CLAY_TEXT(last ? CLAY_STRING("Start: finish   View: skip") : CLAY_STRING("Start: next   View: skip"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY(CLAY_ID("TutFootGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                clickNext = button(CLAY_ID("TutNext"), last ? "Finish" : "Next", true, hovTutNext);
            }
        }
    }
    if (const Clay_ElementData d = Clay_GetElementData(CLAY_ID("TutCard")); d.found)
        ui.addMouseCaptureRect(d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height);

    // Acted on after the card is closed: never leave a CLAY block early (CLAUDE.md).
    if (clickSkip)
        endTutorial(false);
    else if (clickBack)
        setTutorialStep(step - 1);
    else if (clickNext)
    {
        if (last)
            endTutorial(true);
        else
            setTutorialStep(step + 1);
    }
}
