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
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../AudioSystem.h"
#include "clay.h"
#include "../UiTextClay.h"
#include "UIPalette.h"
#include "SatPhotometry.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>

// SatelliteSimUI.cpp: the names the settings window shows for a key / a gamepad button.
const char *keyDisplayName(int key);
const char *gamepadButtonDisplayName(int button);

namespace
{
// The HUD's palette (UIPalette.h): grey panels, the red accent for "this one" and "held".
// Its colours follow the UI theme: Pal::tutHot / tutHeld / tutTargetDim..Lit, the graphics' body Pal::fieldBg.
Clay_Color mixColor(Clay_Color a, Clay_Color b, float t)
{
    return {a.r + (b.r - a.r) * t, a.g + (b.g - a.g) * t, a.b + (b.b - a.b) * t, a.a + (b.a - a.a) * t};
}

constexpr int16_t kZCard = 40;      // above the notices (25) and the selection panel (6)
constexpr float kDoneHoldS = 1.1f;  // a finished step's "Done" before the next card
constexpr float kLastHoldS = 3.0f;  // the last card's closing line
constexpr float kLookDeg = 70.0f;   // degrees of look (yaw + pitch) that finish the look step
constexpr float kMoveS = 1.5f;      // seconds of moving / climbing / boosting that finish those steps
constexpr float kBoostS = 1.0f;
constexpr float kOngoingS = 5.0f;   // seconds on the Missions window's Ongoing tab that finish that step

const char *const kTitles[] = {
    tx(Txt::LblLookAround), tx(Txt::OptMoveOverTheEarth), tx(Txt::OptClimbAndDescend), tx(Txt::OptGoFaster), tx(Txt::OptPickASatellite),
    tx(Txt::OptSatelliteInfo), tx(Txt::OptGoToRideAlong), tx(Txt::OptTraceAndTrack), tx(Txt::OptTime), tx(Txt::OptPictures),
    tx(Txt::OptMissionsWhatsNext), tx(Txt::OptMissionsUnderWay), tx(Txt::OptMenus), tx(Txt::OptGraphics)};

Clay_String clayStr(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }

// One keycap or pad button: `target` = this step wants it (amber, pulsing), `down` = held now (green).
// `fl` places it as a floating child of the element being built (the gamepad's sticks and buttons).
void tutCap(Clay_ElementId id, float w, float h, Clay_CornerRadius rad, const char *label, uint16_t fsz,
            bool target, bool down, float pulse, const Clay_FloatingElementConfig *fl = nullptr)
{
    const Clay_Color bg = down     ? Pal::tutHeld
                          : target ? mixColor(Pal::tutTargetDim, Pal::tutTargetLit, pulse)
                                   : Pal::btnIdle;
    const Clay_Color bc = (target || down) ? Clay_Color{Pal::tutHot.r, Pal::tutHot.g, Pal::tutHot.b, 140.0f + 115.0f * pulse}
                                           : Clay_Color{Pal::textPrimary.r, Pal::textPrimary.g, Pal::textPrimary.b, 40};
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
    tutRetSaved = tutRetValid = false;
    tutGraphicsCard = graphicsPreset == GraphicsPreset::Planetarium || graphicsPreset == GraphicsPreset::Potato ||
                      step >= TUT_GRAPHICS;
    setTutorialStep(std::clamp(step, 0, tutStepCount() - 1));
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
    if (step >= tutStepCount())
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
    tutBaseReports = rpChrome.open;
    tutBaseInfo = tutInfoOn();
    tutBaseTrace = traceChrome.open;
    tutBaseTrack = trackActive;
    tutGoSeen = false;
    tutBaseMsUpcoming = msUpcomingActs_;
    tutBaseMsOngoing = msOngoingActs_;
    tutOngoingS = 0.0f;
    tutPickMsg[0] = '\0';
    tutBasePreset = (int)graphicsPreset;
    // The Missions step's Watch moves the clock and the place: remember where the player was, unless
    // a jump already happened (Back to this step must not make the launch's moment the place to come back to).
    if (tutStep == TUT_MISSIONS && !tutRetValid)
        tutSaveReturn();
}

bool SatelliteSim::tutInfoOn() const
{
    return selectedSatIndex >= 0 && infoChrome.open && viewerSatIndex == selectedSatIndex;
}

// Select / Info / Go to / Trace with nothing (suitable) selected: the card asks for a selection and offers
// "Pick one for me". Info and Go to need a geometry model (selActionAvailMask bits 0 / 1), Trace or Track bits 2 / 3.
bool SatelliteSim::tutNeedsPick(int step) const
{
    switch (step)
    {
    case TUT_SELECT:
        return selectedSatIndex < 0;
    case TUT_INFO:
        return (selActionAvailMask() & 0x1u) == 0u;
    case TUT_GOTO:
        return !followActive && (selActionAvailMask() & 0x2u) == 0u;
    case TUT_TRACE:
        return (selActionAvailMask() & 0xCu) == 0u;
    default:
        return false;
    }
}

// "Pick one for me": a SUNLIT satellite with a geometry model, high in the observer's sky — a crewed station first if
// one is up, else the highest Starlink. Then the camera is aimed at it (not while riding along with Go to).
bool SatelliteSim::tutPickSatellite()
{
    tutPickMsg[0] = '\0';
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 obs = followActive ? readoutObsEci(t)
                                        : observerEciAt(glm::dvec3(obsDir),
                                                        satphot::kEarthRadiusM + (double)std::max(obsTerrainH, obsHeightOffset), t);
    const glm::dvec3 up = glm::normalize(obs);
    const glm::dvec3 sun = glm::normalize(glm::dvec3(sunDirECI));
    auto lit = [&](const glm::dvec3 &p) {
        const double a = glm::dot(p, sun);
        return a > 0.0 || glm::length(p - a * sun) > satphot::kEarthRadiusM;
    };
    int best = -1;
    for (int pass = 0; pass < 2 && best < 0; ++pass) // pass 0: sunlit and > 12 deg up; pass 1: anything > 5 deg up
    {
        double bestScore = -1e9;
        const double minSin = std::sin(glm::radians(pass == 0 ? 12.0 : 5.0));
        for (int ci = 0; ci < (int)constellations.size(); ++ci)
        {
            const ConstellationConfig &c = constellations[ci];
            const bool station = c.name.find("Space Station") != std::string::npos || c.name.find("Tiangong") != std::string::npos;
            const bool starlink = c.name.rfind("Starlink", 0) == 0;
            if (!c.enabled || (!station && !starlink) || c.orbitCount == 0)
                continue;
            const uint32_t i0 = c.orbitStart;
            if (i0 >= satOrbits.size() || !meshRenderer.typeMesh((int)satOrbits[i0].typeIdx))
                continue;
            const uint32_t stride = std::max<uint32_t>(1u, c.orbitCount / 20000u);
            for (uint32_t k = 0; k < c.orbitCount; k += stride)
            {
                const uint32_t i = i0 + k;
                if (i >= activeSatCount || rosterFateAt(i, t) != 0)
                    continue;
                const glm::dvec3 p = satOrbitStateAt(orbitElemsOf(satOrbits[i]), t).posEci;
                const double el = glm::dot(glm::normalize(p - obs), up);
                if (el < minSin || (pass == 0 && !lit(p)))
                    continue;
                const double score = el + (station ? 2.0 : 0.0);
                if (score > bestScore)
                {
                    bestScore = score;
                    best = (int)i;
                }
            }
        }
    }
    if (best >= 0)
        selectSatellite(best);
    if (best < 0 || selectedSatIndex != best)
    {
        snprintf(tutPickMsg, sizeof(tutPickMsg), tx(Txt::FmtNoneIsUpRightNowSpeed));
        return false;
    }
    if (!followActive)
    {
        stopTrack();
        updateSelectedSkyDir();
        const glm::vec3 d = selSkyDirCpu;
        if (glm::dot(d, d) > 1e-12f)
            aimCameraAzEl(glm::degrees(atan2f(d.x, d.y)), glm::degrees(asinf(glm::clamp(d.z, -1.0f, 1.0f))));
    }
    return true;
}

void SatelliteSim::tutSaveReturn()
{
    tutRetSaved = true;
    tutRetT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    tutRetObsDir = followActive ? followSavedObsDir : obsDir;
    tutRetHeight = followActive ? followSavedHeight : obsHeightOffset;
    tutRetAz = camera.azDeg;
    tutRetEl = camera.elDeg;
    tutRetFov = camera.fovYDeg;
    tutRetTimeIdx = timeScaleIdx;
    tutRetPaused = timePaused;
    tutRetDir = timeDir;
    tutRetDriftRate = cloudDriftRate;
    tutRetDriftPhaseOff = cloudDriftPhaseOffset;
    const time_t unixSim = (time_t)std::floor(tutRetT) + 946728000;
    const struct tm *u = gmtime(&unixSim);
    if (u)
        snprintf(tutRetLabel, sizeof(tutRetLabel), tx(Txt::FmtBackTo), u->tm_year + 1900, u->tm_mon + 1,
                 u->tm_mday, u->tm_hour, u->tm_min);
    else
        snprintf(tutRetLabel, sizeof(tutRetLabel), tx(Txt::FmtBackToWhereYouWere));
}

// Back to the time and place before the Missions step's jump (bookmarkGo's cut).
void SatelliteSim::tutReturnNow()
{
    if (!tutRetValid)
        return;
    tutRetValid = false;
    if (followActive)
        stopFollow();
    stopTrack();
    const double days = std::floor(tutRetT / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tutRetT - days * 86400.0;
    obsDir = glm::normalize(tutRetObsDir);
    obsLatDeg = glm::degrees(asinf(glm::clamp(obsDir.z, -1.0f, 1.0f)));
    obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
    obsHeightOffset = tutRetHeight;
    camera.fovYDeg = tutRetFov;
    aimCameraAzEl(tutRetAz, tutRetEl);
    cloudDriftRate = tutRetDriftRate;
    cloudDriftPhaseOffset = tutRetDriftPhaseOff;
    timeScaleIdx = tutRetTimeIdx;
    timePaused = tutRetPaused;
    timeDir = tutRetDir;
    obsTerrainH = cpuTerrainHeightM(obsLatDeg, obsLonDeg);
    updatePositions(tutRetT, 0.0f);
    trailClearPending = true;
    skyTaaHistValid = false; // a cut
    cv2HistoryValid = false;
    msNextWall = msPinWall = -1e300; // the Missions rows are rebuilt for the new time
}

// ─── Completion ───────────────────────────────────────────────────────────────────────────────────
// Every frame, after buildUI's look block (this frame's camera turn is in camera.azDeg/elDeg) and
// before the left-click pick. Reads the keys itself rather than hooking the movement code: the steps
// only need to know that the player pressed them while the camera could move.
void SatelliteSim::updateTutorial(float dt)
{
    FPROF_SCOPE("updateTutorial");
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
        if (tutDoneT > (tutStep + 1 >= tutStepCount() ? kLastHoldS : kDoneHoldS))
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
    case TUT_INFO:
        // Already open when the card came up (a replay): closing and opening it again counts, as does Next.
        if (!tutInfoOn())
            tutBaseInfo = false;
        else if (!tutBaseInfo)
            tutProgress = 1.0f;
        break;
    case TUT_GOTO:
        // Go to, then leave it: Exit (flying home: followFlight 2) or Stay here (follow ends where the camera is).
        if (followActive)
            tutGoSeen = true;
        if (tutGoSeen && (!followActive || followFlight == 2))
            tutProgress = 1.0f;
        break;
    case TUT_TRACE:
        if (!traceChrome.open)
            tutBaseTrace = false;
        if (!trackActive)
            tutBaseTrack = false;
        if ((traceChrome.open && !tutBaseTrace) || (trackActive && !tutBaseTrack))
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
    case TUT_MISSIONS:
        // Watch on an Upcoming launch, or the Sun's Fury's: the clock and place moved.
        if (msUpcomingActs_ != tutBaseMsUpcoming)
        {
            tutRetValid = tutRetSaved;
            tutProgress = 1.0f;
        }
        break;
    case TUT_ONGOING:
        // A few seconds on the Ongoing tab (time to read it), or any of its buttons.
        if (msChrome.open && msTab == 1)
            tutOngoingS += dt;
        tutProgress = std::max(tutOngoingS / kOngoingS, msOngoingActs_ != tutBaseMsOngoing ? 1.0f : 0.0f);
        break;
    case TUT_SETTINGS:
        if ((settingsChrome.open && !tutBaseSettings) || cineChrome.open != tutBaseCine || bmChrome.open != tutBaseBookmarks ||
            rpChrome.open != tutBaseReports)
            tutProgress = 1.0f;
        break;
    case TUT_GRAPHICS:
        if ((settingsChrome.open && !tutBaseSettings) || (int)graphicsPreset != tutBasePreset)
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
                                     .backgroundColor = Pal::fieldBg,
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
    const bool last = step + 1 >= tutStepCount();
    static_assert(sizeof(kTitles) / sizeof(kTitles[0]) == TUT_COUNT, "a title per step");

    // ── Outline what the card talks about (the HUD's own controls; boxes from the last layout) ──
    // Up to three boxes, each the union of its elements (the Missions step outlines a launch's buttons AND the Sun's
    // Fury card). Only elements drawn this frame: Clay keeps stale boxes for the rest.
    const bool needPick = tutNeedsPick(step);
    constexpr int kBoxes = 3;
    bool haveBox[kBoxes] = {};
    Clay_BoundingBox box[kBoxes]{};
    auto addBox = [&](int g, Clay_ElementId id)
    {
        const Clay_ElementData d = Clay_GetElementData(id);
        if (!d.found || d.boundingBox.width <= 0.0f)
            return;
        Clay_BoundingBox &b = box[g];
        if (!haveBox[g])
            b = d.boundingBox;
        else
        {
            const float x1 = std::max(b.x + b.width, d.boundingBox.x + d.boundingBox.width);
            const float y1 = std::max(b.y + b.height, d.boundingBox.y + d.boundingBox.height);
            b.x = std::min(b.x, d.boundingBox.x);
            b.y = std::min(b.y, d.boundingBox.y);
            b.width = x1 - b.x;
            b.height = y1 - b.y;
        }
        haveBox[g] = true;
    };
    // The selection's buttons (SelActBtn id % 4: 0 Info, 1 Go to, 2 Trace pass, 3 Track) drawn this frame.
    auto selBtns = [&](uint32_t which)
    {
        for (int id = 0; id < 8; ++id)
            if ((selBtnDrawnMask & (1u << id)) && (which & (1u << (id % 4))))
                addBox(0, CLAY_SIDI(CLAY_STRING("SelActBtn"), id));
    };
    switch (step)
    {
    case TUT_INFO:
        if (!needPick)
            selBtns(0x1u);
        break;
    case TUT_GOTO:
        if (followActive)
        {
            addBox(0, CLAY_ID("FollowStayBtn"));
            addBox(1, CLAY_ID("FollowExitBtn"));
        }
        else if (!needPick)
            selBtns(0x2u);
        break;
    case TUT_TRACE:
        if (!needPick)
            selBtns(0xCu);
        break;
    case TUT_TIME:
        addBox(0, CLAY_ID("TimeSlowerBtn"));
        addBox(0, CLAY_ID("TimePauseBtn"));
        addBox(0, CLAY_ID("TimeFasterBtn"));
        addBox(0, CLAY_ID("TimeReverseBtn"));
        break;
    case TUT_CAPTURE:
        addBox(0, CLAY_ID("TimeScreenshotBtn"));
        addBox(0, CLAY_ID("TimePhotoBtn"));
        addBox(0, CLAY_ID("TrailsBtn"));
        break;
    case TUT_MISSIONS:
        if (!msChrome.open)
            addBox(0, CLAY_ID("TimeMissionsBtn"));
        else if (msTab != 0)
            addBox(0, CLAY_SIDI(CLAY_STRING("MsTab"), 0));
        else
        {
            // The first launch row's Watch (the next launch, else the pinned twilight / crewed one), and the Sun's
            // Fury card's.
            const int row = !msNext.empty() ? 0 : msHasTwilight ? 100 : !msCrewList.empty() ? 101 : -1;
            if (row >= 0)
                addBox(0, CLAY_SIDI(CLAY_STRING("MsLBtns"), row));
            addBox(1, CLAY_ID("MsFuryBtns"));
        }
        break;
    case TUT_ONGOING:
        if (!msChrome.open)
            addBox(0, CLAY_ID("TimeMissionsBtn"));
        else if (msTab != 1)
            addBox(0, CLAY_SIDI(CLAY_STRING("MsTab"), 1));
        break;
    case TUT_SETTINGS: // the right panel's menu buttons (Missions had its own steps)
        addBox(0, CLAY_ID("TimeBookmarkBtn"));
        addBox(0, CLAY_ID("TimeCineBtn"));
        addBox(0, CLAY_ID("TimeReportBtn"));
        addBox(0, CLAY_ID("SettingsBtn"));
        break;
    case TUT_GRAPHICS:
        addBox(0, CLAY_ID("SettingsBtn"));
        break;
    default:
        break;
    }
    for (int g = 0; g < kBoxes; ++g)
    {
        if (!haveBox[g] || done)
            continue;
        const float m = 4.0f;
        const Clay_BoundingBox &b = box[g];
        // PASSTHROUGH: it sits over the buttons it points at (CLAUDE.md, the pointer-capture rule).
        CLAY(CLAY_SIDI(CLAY_STRING("TutOutline"), g),
             {.layout = {.sizing = {CLAY_SIZING_FIXED(b.width + 2.0f * m), CLAY_SIZING_FIXED(b.height + 2.0f * m)}},
              .backgroundColor = {Pal::tutHot.r, Pal::tutHot.g, Pal::tutHot.b, 20.0f + 25.0f * pulse},
              .cornerRadius = CLAY_CORNER_RADIUS(6),
              .floating = {.offset = {b.x - m, b.y - m},
                           .zIndex = kZCard + 2,
                           .pointerCaptureMode = CLAY_POINTER_CAPTURE_MODE_PASSTHROUGH,
                           .attachTo = CLAY_ATTACH_TO_ROOT},
              .border = {.color = {Pal::tutHot.r, Pal::tutHot.g, Pal::tutHot.b, 140.0f + 115.0f * pulse}, .width = {2, 2, 2, 2, 0}}}) {}
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
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), pad ? tx(Txt::FmtPushTheRightStickToLook)
                                                      : tx(Txt::FmtHoldTheRightMouseButtonAnd),
                 P(KB_ZOOM_OUT), P(KB_ZOOM_IN));
        break;
    case TUT_MOVE:
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), "%s", pad ? tx(Txt::TxtTheLeftStickMovesYouOver)
                                                           : tx(Txt::TxtWASDMoveYou));
        break;
    case TUT_ALTITUDE:
        snprintf(k1, sizeof(k1), "%s", K(KB_RAISE_ELEV));
        snprintf(k2, sizeof(k2), "%s", K(KB_LOWER_ELEV));
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtRtClimbsAndLtDescendsFrom));
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtClimbsAndDescendsFromTheGround), k1, k2);
        break;
    case TUT_BOOST:
        snprintf(k1, sizeof(k1), "%s", K(KB_MOVE_BOOST));
        snprintf(k2, sizeof(k2), "%s", K(KB_MOVE_FINE));
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtClickAndHoldTheLeftStick),
                     P(KB_MOVE_BOOST));
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtHoldWhileMovingOrClimbingTo), k1, k2);
        break;
    case TUT_SELECT:
        snprintf(k1, sizeof(k1), "%s", K(KB_SELECT_SAT));
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtPutASatelliteNearTheMiddle), P(KB_SELECT_SAT));
        else if (keybindings[KB_SELECT_SAT].key >= 0)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtLeftClickASatelliteAnyMoving2), k1);
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtLeftClickASatelliteAnyMoving));
        break;
    case TUT_INFO:
    case TUT_GOTO:
    case TUT_TRACE:
        if (needPick)
        {
            if (selectedSatIndex >= 0 && step != TUT_TRACE)
                snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtThisOneHasNo3dModel));
            else
                snprintf(tutBodyBuf, sizeof(tutBodyBuf), "%s", pad ? tx(Txt::TxtSelectASatelliteFirstPutOne)
                                                               : tx(Txt::TxtSelectASatelliteFirstLeftClick));
            if (tutPickMsg[0])
                snprintf(tutBodyBuf + strlen(tutBodyBuf), sizeof(tutBodyBuf) - strlen(tutBodyBuf), " %s", tutPickMsg);
        }
        else if (step == TUT_INFO)
        {
            snprintf(k1, sizeof(k1), "%s", K(KB_INFO));
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtInfoTheIOpensItsWindow),
                     pad ? "" : tx(Txt::TxtOr), pad ? "" : k1, pad ? tx(Txt::TxtDPadLeftRightPicksA) : "");
        }
        else if (step == TUT_GOTO && !followActive)
        {
            snprintf(k1, sizeof(k1), "%s", K(KB_FOLLOW_STAY));
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtGoToTheEyeFliesYou),
                     pad ? "" : tx(Txt::TxtOr), pad ? "" : k1, pad ? tx(Txt::TxtDPadToTheEyeThen) : "");
        }
        else if (step == TUT_GOTO)
        {
            snprintf(k1, sizeof(k1), "%s", K(KB_FOLLOW_STAY));
            if (pad)
                snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtYouAreRidingAlongMoveAround));
            else
                snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtYouAreRidingAlongWasdAnd),
                         k1);
        }
        else
        {
            snprintf(k1, sizeof(k1), "%s", K(KB_TRACK));
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtTracePassPlotsItsBrightnessOver),
                     pad ? "" : " (", pad ? "" : k1, pad ? "" : ")");
        }
        break;
    case TUT_TIME:
        if (pad)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtSpeedTimeUpOrSlowIt),
                     P(KB_FASTER), P(KB_SLOWER), P(KB_PAUSE), P(KB_REVERSE));
        else
        {
            snprintf(k1, sizeof(k1), "%s", K(KB_SLOWER));
            snprintf(k2, sizeof(k2), "%s", K(KB_PAUSE));
            snprintf(k3, sizeof(k3), "%s", K(KB_FASTER));
            snprintf(k4, sizeof(k4), "%s", K(KB_REVERSE));
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtSlowDownPauseSpeedUpOr),
                     k1, k2, k3, k4);
        }
        break;
    case TUT_CAPTURE:
        snprintf(k1, sizeof(k1), "%s", K(KB_SCREENSHOT));
        snprintf(k2, sizeof(k2), "%s", K(KB_PHOTO));
        snprintf(k3, sizeof(k3), "%s", K(KB_TOGGLE_TRAILS));
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtScreenshotHqPhotoForASharper),
                 k1, k2, k3, pad ? tx(Txt::TxtOnAControllerAfterTheTutorial) : "");
        break;
    case TUT_MISSIONS:
        snprintf(k1, sizeof(k1), "%s", K(KB_MISSIONS));
        if (!msChrome.open)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtMissionsTheRocketButtonOrLists),
                     k1);
        else if (msTab != 0)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtOpenTheUpcomingTab));
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtUpcomingTheNextLaunchesWatchTakes));
        break;
    case TUT_ONGOING:
        if (!msChrome.open)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtOpenMissionsAgainTheRocketButton), K(KB_MISSIONS));
        else if (msTab != 1)
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtNowOpenTheOngoingTab));
        else
            snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtOngoingRocketsInFlightCrewsOn));
        if (tutRetValid)
            snprintf(tutBodyBuf + strlen(tutBodyBuf), sizeof(tutBodyBuf) - strlen(tutBodyBuf), tx(Txt::FmtTheButtonBelowTakesYouBack));
        break;
    case TUT_SETTINGS:
        snprintf(k1, sizeof(k1), "%s", K(KB_TOGGLE_UI));
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtBookmarksSaveAPlaceAndTime),
                 pad ? P(KB_TOGGLE_UI) : k1, pad ? tx(Txt::TxtAfterTheTutorialStartGivesA) : "");
        break;
    case TUT_GRAPHICS:
        snprintf(tutBodyBuf, sizeof(tutBodyBuf), tx(Txt::FmtGraphicsAreOnTheFastestPreset),
                 kGraphicsPresetNames[(int)graphicsPreset]);
        break;
    default:
        tutBodyBuf[0] = '\0';
        break;
    }
    snprintf(tutHeadBuf, sizeof(tutHeadBuf), "%d / %d", step + 1, tutStepCount());

    // ── Where: beside the panel it points at, else low in the middle of the view ──
    Clay_FloatingElementConfig fl{};
    fl.zIndex = kZCard;
    // The Missions window docks top right: its two cards sit above the left panel, out of its way.
    if (step == TUT_TIME || step == TUT_CAPTURE || ((step == TUT_MISSIONS || step == TUT_ONGOING) && msChrome.open))
    {
        fl.offset = {0.0f, -10.0f};
        fl.parentId = CLAY_ID("LeftPanel").id;
        fl.attachPoints = {.element = CLAY_ATTACH_POINT_LEFT_BOTTOM, .parent = CLAY_ATTACH_POINT_LEFT_TOP};
        fl.attachTo = CLAY_ATTACH_TO_ELEMENT_WITH_ID;
    }
    else if (step == TUT_SETTINGS || step == TUT_GRAPHICS || ((step == TUT_MISSIONS || step == TUT_ONGOING) && !msChrome.open))
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
    const bool bar = step <= TUT_BOOST || step == TUT_ONGOING;
    bool clickNext = false, clickBack = false, clickSkip = false, clickPick = false, clickReturn = false;
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
                    ui.tooltip(inp, n, tx(Txt::TipReplayItFromSettingsDisplay), fs(11));
                    CLAY_TEXT(txc(Txt::NoteSkipTutorial), CLAY_TEXT_CONFIG({.textColor = n ? Pal::btnLabel : Pal::textDim, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                }
            }

            CLAY_TEXT(clayStr(tutBodyBuf), CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));

            if (done)
            {
                CLAY_TEXT(last ? txc(Txt::NoteAllSetReplayThisAnyTime) : txc(Txt::NoteDone),
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
                    clickBack = button(CLAY_ID("TutBack"), tx(Txt::BtnBack), false, hovTutBack);
                // A controller moves through the card with Start / View (pollGamepad), no cursor needed.
                if (pad)
                    CLAY_TEXT(last ? txc(Txt::NoteStartFinishViewSkip) : txc(Txt::NoteStartNextViewSkip),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_NONE}));
                CLAY(CLAY_ID("TutFootGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                if (needPick && !done && !pad)
                    clickPick = button(CLAY_ID("TutPick"), tx(Txt::BtnPickOneForMe), false, hovTutPick);
                // After the Missions step's jump: back to the time and place before it (tutReturnNow).
                if (tutRetValid && step > TUT_MISSIONS)
                    clickReturn = button(CLAY_ID("TutReturn"), tutRetLabel, false, hovTutReturn);
                clickNext = button(CLAY_ID("TutNext"), last ? tx(Txt::BtnFinish) : tx(Txt::BtnNext2), true, hovTutNext);
            }
        }
    }
    if (const Clay_ElementData d = Clay_GetElementData(CLAY_ID("TutCard")); d.found)
        ui.addMouseCaptureRect(d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height);

    // Acted on after the card is closed: never leave a CLAY block early (CLAUDE.md).
    if (clickPick)
        tutPickSatellite();
    if (clickReturn)
        tutReturnNow();
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
