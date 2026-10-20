// SatelliteSimMissions.cpp — the Missions window (2026-10-19).
//
// The dynamic sky has outgrown the settings window: launches, crew flights, retirements and the Sun's Fury live
// here, opened from the rocket button on the right HUD panel (or M, KB_MISSIONS; harness `ui open missions`).
// Three tabs:
//   Upcoming — the next five launches (Go to: on the ground ~12 km from the pad, 45 s before lift-off at 1x; Watch:
//              the time only), then pinned whatever their order: the next twilight launch (the "jellyfish": the Sun
//              5-16 deg below the pad, the exhaust high up sunlit over a dark ground) and the next crewed launch, and
//              last, always, THE SUN'S FURY (the CME's arrival, its countdown, Go to).
//   Ongoing  — crew transfers in flight, the crewed stations (next crew flight; Select / Go to), satellite launches
//              in flight, and per program the trains raising and the retirees deorbiting.
//   Controls — the LAUNCHES and THE SUN'S FURY settings (moved here from Settings > Constellations).
//
// The schedule is a pure function of sim time (SatelliteSimLaunches.cpp); the rows are rebuilt at most twice a second
// (the pinned twilight / crewed searches every few seconds, or when sim time passes them), never per frame. Only the
// countdowns are formatted per frame. Strings handed to Clay live in msText_ (a deque: it never moves them) or in the
// cached rows, which change only before the layout.
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "clay.h"
#include "UIPalette.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <ctime>

namespace
{
constexpr double kDay = 86400.0;
constexpr double kLeadS = 45.0;         // Go to / Watch: this long before lift-off
constexpr double kFuryFlareLead = 17.5 * 3600.0; // the X-class flare: 2037-07-14 09:30 UTC, 17.5 h before the arrival
// The default bookmark "The Sun's Fury" (data/bookmarks/bookmarks.json): Monument Valley, its view and cloud drift.
constexpr double kFuryObs[3] = {-0.27445882795077464, -0.7499940459914896, 0.6018149904557067};
constexpr double kFuryBookmarkT = 1184524200.0; // 3.5 h after the arrival
constexpr double kFuryDriftPhase = 2.3823554348512275;
constexpr float kFuryDriftRate = 6.499999926745659e-06f;

double wallNow() { return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count(); }

void fmtUtcShort(double tJ2000, char *buf, size_t n)
{
    const time_t unixSim = (time_t)std::floor(tJ2000) + 946728000;
    const struct tm *u = gmtime(&unixSim);
    if (!u)
        snprintf(buf, n, "--");
    else
        snprintf(buf, n, "%04d-%02d-%02d %02d:%02d UTC", u->tm_year + 1900, u->tm_mon + 1, u->tm_mday, u->tm_hour, u->tm_min);
}

// "T- 3d 04h", "T- 4h 12m", "T- 12m 03s", "T- 42 s" (dt = target - now; past: "T+").
void fmtCountdown(double dt, char *buf, size_t n, const char *prefix = "T-")
{
    const char *pre = dt >= 0.0 ? prefix : "T+";
    double a = std::abs(dt);
    const long long s = (long long)std::floor(a);
    if (s >= 86400)
        snprintf(buf, n, "%s %lldd %02lldh", pre, s / 86400, (s % 86400) / 3600);
    else if (s >= 3600)
        snprintf(buf, n, "%s %lldh %02lldm", pre, s / 3600, (s % 3600) / 60);
    else if (s >= 60)
        snprintf(buf, n, "%s %lldm %02llds", pre, s / 60, s % 60);
    else
        snprintf(buf, n, "%s %lld s", pre, s);
}

// "in 3d 04h" style, for statuses.
void fmtIn(double dt, char *buf, size_t n)
{
    if (dt < 0.0)
    {
        snprintf(buf, n, "now");
        return;
    }
    const long long s = (long long)std::floor(dt);
    if (s >= 86400)
        snprintf(buf, n, "in %lldd %02lldh", s / 86400, (s % 86400) / 3600);
    else if (s >= 3600)
        snprintf(buf, n, "in %lldh %02lldm", s / 3600, (s % 3600) / 60);
    else if (s >= 60)
        snprintf(buf, n, "in %lldm %02llds", s / 60, s % 60);
    else
        snprintf(buf, n, "in %lld s", s);
}

Clay_String cs(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
} // namespace

const char *SatelliteSim::msStr(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    msText_.emplace_back(b);
    return msText_.back().c_str();
}

// ── The rows (cached) ───────────────────────────────────────────────────────────────────────────
void SatelliteSim::missionsRefresh(bool force)
{
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double wall = wallNow();
    // The next launches and what is flying: twice a second, or at once after a jump in sim time.
    const bool jumped = std::abs(now - msNextSim) > 6.0 * 3600.0 && std::abs(wall - msNextWall) < 0.5;
    if (force || wall - msNextWall > 0.5 || jumped || (!msNext.empty() && now > msNext.front().tL))
    {
        msNextWall = wall;
        msNextSim = now;
        launchUpcoming(now, now + 2.0 * kDay, 0, 5, msNext);
        if (msNext.size() < 5)
            launchUpcoming(now, now + 14.0 * kDay, 0, 5, msNext);
        launchOngoing(msOngoing);
    }
    // The pinned rows (wider searches): every 3 s, or when sim time passes one, or jumps.
    const bool pinJump = std::abs(now - msPinSim) > 1.0 * kDay && std::abs(wall - msPinWall) < 3.0;
    const bool passed = (msHasTwilight && now > msTwilight.tL) || (!msCrewList.empty() && now > msCrewList.front().tL);
    if (force || wall - msPinWall > 3.0 || pinJump || passed)
    {
        msPinWall = wall;
        msPinSim = now;
        msHasTwilight = false;
        std::vector<MissionLaunch> one;
        for (int d = 0; d < 7 && !msHasTwilight; ++d) // day by day: most days hold one
        {
            launchUpcoming(now + d * kDay, now + (d + 1) * kDay, 2, 1, one);
            if (!one.empty())
            {
                msTwilight = one.front();
                msHasTwilight = true;
            }
        }
        launchUpcoming(now, now + 400.0 * kDay, 1, 64, msCrewList);
        msStations = launchCrewStations();
    }
}

bool SatelliteSim::missionsGoToFury(int which, std::string &msg)
{
    if (!kesslerEnabled)
    {
        msg = "The Sun's Fury is off (Missions > Controls)";
        return false;
    }
    if (followActive)
        stopFollow();
    stopTrack();
    const double tArr = kesslerEpoch();
    const double t = which == 0 ? tArr - 120.0 : kFuryBookmarkT;
    const double days = std::floor(t / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = t - days * 86400.0;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    trailClearPending = true;
    // Monument Valley (the default bookmark's spot), on the ground, looking north at the auroral sky: at the arrival
    // it is dusk there (the Sun ~5 deg down) and the storm's sky darkens with it; the bookmark's moment is 3.5 h on.
    const glm::dvec3 d(kFuryObs[0], kFuryObs[1], kFuryObs[2]);
    setObserverLatLon(glm::degrees(std::asin(glm::clamp(d.z, -1.0, 1.0))), glm::degrees(std::atan2(d.y, d.x)));
    obsHeightOffset = 0.0f;
    camera.fovYDeg = 80.0f;
    aimCameraAzEl(0.0f, which == 0 ? 25.0f : 32.0f);
    // The bookmark's weather (the cloud map's drift is session state: without it the storm night may be overcast).
    cloudDriftRate = kFuryDriftRate;
    cloudDriftPhaseOffset = std::fmod(kFuryDriftPhase - (double)kFuryDriftRate * (kFuryBookmarkT - kCloudDriftEpochS),
                                      glm::two_pi<double>());
    updatePositions(t);
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    msg = which == 0 ? "The Sun's Fury: the CME arrives in 2 minutes (Monument Valley, dusk)"
                     : "The Sun's Fury: the storm night, 3.5 h after the arrival (Monument Valley)";
    return true;
}

bool SatelliteSim::missionsGo(const std::string &what, bool watchOnly, std::string &msg)
{
    missionsRefresh(true);
    if (what == "fury" || what == "fury_night")
        return missionsGoToFury(what == "fury" ? 0 : 1, msg);
    const MissionLaunch *m = nullptr;
    if (what == "twilight")
        m = msHasTwilight ? &msTwilight : nullptr;
    else if (what == "crew")
        m = msCrewList.empty() ? nullptr : &msCrewList.front();
    else
    {
        char *end = nullptr;
        const long i = std::strtol(what.c_str(), &end, 10);
        if (end && *end == '\0' && i >= 0 && i < (long)msNext.size())
            m = &msNext[(size_t)i];
    }
    if (!m)
    {
        msg = "no such launch (an index into the next launches, twilight, crew, fury, fury_night)";
        return false;
    }
    return launchGoToKey(m->key, !watchOnly, kLeadS, msg);
}

bool SatelliteSim::missionsOngoingAct(int row, bool go, std::string &msg)
{
    if (row < 0 || row >= (int)msOngoing.size())
    {
        msg = "no such row";
        return false;
    }
    const MissionOngoing &o = msOngoing[(size_t)row];
    bool ok = false;
    if (o.cat == 0 || o.cat == 2)
        ok = launchSelectRocketOf(o.key, msg);
    else if (o.cat == 3 || o.cat == 4)
        ok = launchSelectNearestOf(o.prog, o.cat == 3 ? 0 : 1, msg);
    else
        msg = "nothing to select in that row";
    if (!ok || !go)
        return ok;
    const std::string what = msg;
    startFollow(selectedSatIndex, true);
    if (!followActive)
    {
        msg = what + " - selected (no model to go to)";
        return false;
    }
    msg = "Flying to " + what;
    return true;
}

// An Ongoing row's launch: to its next (dir +1) / previous (-1) milestone, or milestone `index`. The batch's rocket
// timeline; when the selection or Go to rides one of its objects, that object's (so a booster stays the booster).
bool SatelliteSim::missionsMilestone(int row, int dir, int index, std::string &msg)
{
    if (row < 0 || row >= (int)msOngoing.size() || (msOngoing[(size_t)row].cat != 0 && msOngoing[(size_t)row].cat != 2))
    {
        msg = "no launch in that row";
        return false;
    }
    const uint64_t key = msOngoing[(size_t)row].key;
    MissionSubject s;
    if (!missionSubjectNow(s) || s.key != key || s.roster)
        if (!missionSubjectOfBatch(key, s, kMsPayload))
        {
            msg = "that launch has no milestones";
            return false;
        }
    return missionJump(s, dir, index, msg);
}

nlohmann::json SatelliteSim::missionsStateJson()
{
    missionsRefresh(true);
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto lj = [&](const MissionLaunch &m) {
        char u[48];
        fmtUtcShort(m.tL, u, sizeof(u));
        return nlohmann::json{{"vehicle", m.vehicle}, {"program", m.program}, {"site", m.site}, {"target", m.target},
                              {"utc", u}, {"in_hours", (m.tL - now) / 3600.0}, {"sun_elev_deg", m.sunElevDeg},
                              {"twilight", m.twilight}, {"crew", m.crew}, {"spacecraft", m.spacecraft},
                              {"people", m.people}, {"payload", m.payload}};
    };
    nlohmann::json j;
    j["open"] = msChrome.open;
    j["tab"] = msTab;
    nlohmann::json nx = nlohmann::json::array();
    for (const MissionLaunch &m : msNext)
        nx.push_back(lj(m));
    j["next"] = nx;
    j["twilight"] = msHasTwilight ? lj(msTwilight) : nlohmann::json();
    j["crew_next"] = msCrewList.empty() ? nlohmann::json() : lj(msCrewList.front());
    nlohmann::json cl = nlohmann::json::array();
    for (const MissionLaunch &m : msCrewList)
        cl.push_back(lj(m));
    j["crew_flights"] = cl;
    j["fury"] = {{"enabled", kesslerEnabled}, {"in_hours", (kesslerEpoch() - now) / 3600.0}};
    nlohmann::json og = nlohmann::json::array();
    for (const MissionOngoing &o : msOngoing)
        og.push_back({{"cat", o.cat}, {"title", o.title}, {"status", o.status}, {"count", o.count},
                      {"next_in_hours", o.tNext > 0.0 ? (o.tNext - now) / 3600.0 : 0.0}, {"progress", o.progress},
                      {"drawn", o.drawn}});
    j["ongoing"] = og;
    nlohmann::json st = nlohmann::json::array();
    for (const CrewStation &s : msStations)
    {
        double tF = 0.0;
        const int fate = s.sat >= 0 ? rosterFateAt((uint32_t)s.sat, now, &tF) : 0;
        st.push_back({{"name", constellations[s.constIdx].name}, {"sat", s.sat}, {"fate", rosterFateName(fate)},
                      {"fate_hours_ago", fate ? (now - tF) / 3600.0 : 0.0}});
    }
    j["stations"] = st;
    j["status"] = msStatus;
    return j;
}

// ── The window ──────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildMissionsWindow(const UIInput &inp, UIRenderer &ui)
{
    if (!msChrome.open)
        return;
    msText_.clear(); // last frame's strings: recorded by now
    if (msChrome.w <= 0.0f)
    {
        msChrome.w = std::min(600.0f, inp.screenW - 40.0f);
        msChrome.h = std::max(360.0f, std::min(720.0f, inp.screenH - 200.0f));
    }
    missionsRefresh(false);
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;

    auto text = [&](const char *s, Clay_Color c, int size, bool wrap = false) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size),
                                           .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    auto tag = [&](uint32_t id, const char *label, Clay_Color bg) {
        CLAY(CLAY_IDI("MsTag", id), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)}, .padding = {5, 5, 1, 1}},
                                     .backgroundColor = bg,
                                     .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            text(label, Pal::textBright, 10);
        }
    };
    auto progressBar = [&](uint32_t id, double f) {
        f = std::clamp(f, 0.0, 1.0);
        CLAY(CLAY_IDI("MsBar", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(3)}},
                                     .backgroundColor = Pal::trackBg})
        {
            CLAY(CLAY_IDI("MsBarFill", id), {.layout = {.sizing = {CLAY_SIZING_PERCENT((float)f), CLAY_SIZING_GROW(0)}},
                                             .backgroundColor = Pal::progressFill}) {}
        }
    };

    // Actions, applied after the layout (they move time and the observer; the rows are rebuilt next frame).
    int goLaunch = -1, watchLaunch = -1; // 0..4 the next launches, 100 twilight, 101 crew
    int furyGo = -1, selRocket = -1, selTrain = -1, selRetiree = -1, stationSel = -1, stationGo = -1;
    int ongoingGo = -1; // an Ongoing row's Go to (missionsOngoingAct)
    int ongoingNext = -1; // an Ongoing row's next milestone (missionsMilestone)
    bool watchReentry = false, toControls = false;

    // A launch row. id: a stable index; pin: a label shown first ("NEXT TWILIGHT").
    auto launchRow = [&](int id, const MissionLaunch &m) {
        CLAY(CLAY_IDI("MsLRow", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .padding = {8, 8, 6, 6},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                      .backgroundColor = Pal::cardBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(4),
                                      .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
        {
            CLAY(CLAY_IDI("MsLText", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                      .childGap = 3,
                                                      .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                CLAY(CLAY_IDI("MsLTitle", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                           .childGap = 6,
                                                           .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                           .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    if (m.crew)
                        tag((uint32_t)id * 4u + 0u, "CREWED", Pal::tagCrew);
                    if (m.twilight)
                        tag((uint32_t)id * 4u + 1u, "TWILIGHT", Pal::tagTwilight);
                    if (m.crew)
                        text(msStr("%s to %s (%d crew)", m.spacecraft.c_str(), m.target.c_str(), m.people), Pal::textPrimary, 12, true);
                    else
                        text(msStr("%s - %s, %d satellites", m.vehicle.c_str(), m.program.c_str(), m.payload), Pal::textPrimary, 12, true);
                }
                char u[48];
                fmtUtcShort(m.tL, u, sizeof(u));
                if (m.crew)
                    text(msStr("%s from %s, %s", m.vehicle.c_str(), m.site.c_str(), u), Pal::textDim, 11, true);
                else
                    text(msStr("%s, %s, Sun %+.0f deg at the pad", m.site.c_str(), u, m.sunElevDeg), Pal::textDim, 11, true);
            }
            CLAY(CLAY_IDI("MsLRight", id), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                       .childGap = 4,
                                                       .childAlignment = {.x = CLAY_ALIGN_X_RIGHT},
                                                       .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                char cd[48];
                fmtCountdown(m.tL - now, cd, sizeof(cd));
                text(msStr("%s", cd), Pal::volValue, 13);
                CLAY(CLAY_IDI("MsLBtns", id), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                          .childGap = 4,
                                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    if (uiButton(inp, ui, "MsGo", id, "Go to",
                                 "45 s before lift-off at 1x, on the ground beside the pad, looking at it"))
                        goLaunch = id;
                    if (uiButton(inp, ui, "MsWatch", id, "Watch", "Jump the clock to 45 s before lift-off (you stay where you are)"))
                        watchLaunch = id;
                }
            }
        }
    };

    // A generic ongoing row: title, status (+ countdown), progress, up to two buttons.
    auto ongoingRow = [&](int id, const char *title, const char *status, double tNext, double progress,
                          const std::function<void()> &buttons) {
        CLAY(CLAY_IDI("MsORow", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                 .padding = {8, 8, 5, 5},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                      .backgroundColor = Pal::cardBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(4),
                                      .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
        {
            CLAY(CLAY_IDI("MsOText", id), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                      .childGap = 3,
                                                      .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                text(title, Pal::textPrimary, 12, true); // (msStr copies: the rows may be rebuilt before this frame records)
                if (tNext > 0.0)
                {
                    char in[48];
                    fmtIn(tNext - now, in, sizeof(in));
                    text(msStr("%s %s", status, in), Pal::textDim, 11, true);
                }
                else
                    text(status, Pal::textDim, 11, true);
                if (progress >= 0.0)
                    progressBar((uint32_t)id, progress);
            }
            CLAY(CLAY_IDI("MsOBtns", id), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                      .childGap = 4,
                                                      .layoutDirection = CLAY_LEFT_TO_RIGHT}})
            {
                if (buttons)
                    buttons();
            }
        }
    };

    static char titleBuf[64];
    snprintf(titleBuf, sizeof(titleBuf), "Missions");
    buildResizableWindow(
        inp, ui, msChrome, 8, titleBuf, true, hovMsClose, inp.screenW - msChrome.w - 12.0f, 60.0f, 420.0f, 300.0f, 1600.0f,
        1400.0f,
        [&]()
        {
            CLAY(CLAY_ID("MsBody"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                .padding = {12, 12, 8, 10},
                                                .childGap = 6,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                // Tabs + the clock.
                CLAY(CLAY_ID("MsTabs"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                    .childGap = 4,
                                                    .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                    .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    static const char *kTabs[] = {"Upcoming", "Ongoing", "Controls"};
                    static const char *kTips[] = {"The next launches, the next twilight and crewed launches, the Sun's Fury",
                                                  "Crew flights, stations, rockets in flight, trains and retirements",
                                                  "Launches and the Sun's Fury: settings and live counts"};
                    for (int i = 0; i < 3; ++i)
                        if (uiButton(inp, ui, "MsTab", i, kTabs[i], kTips[i], msTab == i))
                            msTab = i;
                    CLAY(CLAY_ID("MsTabsGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}}) {}
                    char u[48];
                    fmtUtcShort(now, u, sizeof(u));
                    text(msStr("Now %s", u), Pal::textDim, 11);
                }
                if (!msStatus.empty())
                    text(msStr("%s", msStatus.c_str()), Pal::textWarn, 11, true);

                CLAY(CLAY_ID("MsList"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                    .padding = {0, 8, 2, 4},
                                                    .childGap = 5,
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                         .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                {
                    if (msTab == 0)
                    {
                        // ── Upcoming ─────────────────────────────────────────────────────────────────
                        uiSection("MsSec", 0, "NEXT LAUNCHES");
                        // Launches stand down at the storm (by default for good): an empty list before it says why.
                        const bool standDown = kesslerEnabled && launchAfterStorm == 0;
                        auto noneWhy = [&](double horizonDays, const char *plain) -> const char * {
                            if (standDown && now >= kesslerEpoch())
                                return "None: launches stood down at the Sun's Fury (Controls: After the Sun's Fury).";
                            if (standDown && kesslerEpoch() - now < horizonDays * kDay)
                                return "None before the Sun's Fury: launches stand down when it arrives.";
                            return plain;
                        };
                        if (!launchesEnabled)
                            text("Launches are off (Controls).", Pal::textDim, 11);
                        else if (msNext.empty())
                            text(noneWhy(14.0, "None in the next two weeks."), Pal::textDim, 11, true);
                        for (int i = 0; i < (int)msNext.size(); ++i)
                            launchRow(i, msNext[(size_t)i]);
                        uiSection("MsSec", 1, "NEXT TWILIGHT LAUNCH (JELLYFISH)");
                        if (msHasTwilight)
                            launchRow(100, msTwilight);
                        else if (launchesEnabled)
                            text(noneWhy(7.0, "None in the next week."), Pal::textDim, 11, true);
                        uiSection("MsSec", 2, "NEXT CREWED LAUNCH");
                        if (!msCrewList.empty())
                            launchRow(101, msCrewList.front());
                        else if (launchesEnabled)
                            text(noneWhy(400.0, "None scheduled."), Pal::textDim, 11, true);

                    }
                    else if (msTab == 1)
                    {
                        // ── Ongoing ──────────────────────────────────────────────────────────────────
                        static const char *kCats[] = {"CREW TRANSFERS", "CREWED STATIONS", "SATELLITE LAUNCHES",
                                                      "SATELLITE OPERATIONS"};
                        int rid = 0;
                        // Crew transfers.
                        uiSection("MsOSec", 0, kCats[0]);
                        int n0 = 0;
                        for (size_t i = 0; i < msOngoing.size(); ++i)
                        {
                            const MissionOngoing &o = msOngoing[i];
                            if (o.cat != 0)
                                continue;
                            ++n0;
                            const int idx = (int)i;
                            ongoingRow(rid++, msStr("%s", o.title.c_str()), msStr("%s", o.status.c_str()), o.tNext, o.progress, [&]() {
                                if (uiButton(inp, ui, "MsSelRocket", idx, "Select",
                                             o.drawn ? "Select the rocket" : "Select the rocket (not drawn from here: below the horizon or too faint)"))
                                    selRocket = idx;
                                if (uiButton(inp, ui, "MsGoRocket", idx, "Go to", "Fly to the rocket and ride along"))
                                    ongoingGo = idx;
                                if (uiButton(inp, ui, "MsNextMs", idx, "Next >",
                                             "Skip to just before this launch's next milestone, at 1x (a selected or followed "
                                             "stage stays selected / followed)"))
                                    ongoingNext = idx;
                            });
                        }
                        if (!n0)
                            text("No crew in flight.", Pal::textDim, 11);
                        // Stations.
                        uiSection("MsOSec", 1, kCats[1]);
                        if (msStations.empty())
                            text("No crewed stations in the roster.", Pal::textDim, 11);
                        for (size_t s = 0; s < msStations.size(); ++s)
                        {
                            const CrewStation &cs0 = msStations[s];
                            const MissionLaunch *nextCrew = nullptr;
                            for (const MissionLaunch &m : msCrewList)
                                if (m.target == constellations[cs0.constIdx].name)
                                {
                                    nextCrew = &m;
                                    break;
                                }
                            bool arriving = false;
                            for (const MissionOngoing &o : msOngoing)
                                if (o.cat == 0 && o.title.find(constellations[cs0.constIdx].name) != std::string::npos)
                                    arriving = true;
                            const float altKm = cs0.sat >= 0 && cs0.sat < (int)satOrbits.size() ? satOrbits[cs0.sat].altM / 1000.0f : 0.0f;
                            const char *status;
                            double tN = 0.0;
                            double tFate = 0.0;
                            const int fate = cs0.sat >= 0 ? rosterFateAt((uint32_t)cs0.sat, now, &tFate) : 0;
                            char fu[40];
                            fmtUtcShort(tFate, fu, sizeof(fu));
                            if (fate == 1)
                                status = msStr("Dead since %s, tumbling, uncrewed; no more crew flights", fu);
                            else if (fate >= 2)
                                status = msStr("%s %s; no more crew flights", fate == 2 ? "Destroyed" : "Re-entered", fu);
                            else if (!constellations[cs0.constIdx].enabled)
                                status = msStr("Hidden (Settings > Constellations); %.0f km", altKm);
                            else if (arriving)
                                status = msStr("Crewed, %.0f km; a new crew is on its way", altKm);
                            else if (nextCrew)
                            {
                                status = msStr("Crewed, %.0f km; next crew (%s)", altKm, nextCrew->spacecraft.c_str());
                                tN = nextCrew->tL;
                            }
                            else
                                status = msStr("Crewed, %.0f km", altKm);
                            const int sidx = (int)s;
                            // (a destroyed / re-entered station is not there to select or fly to; a dead one tumbles)
                            const bool en = constellations[cs0.constIdx].enabled && fate < 2;
                            ongoingRow(rid++, msStr("%s", constellations[cs0.constIdx].name.c_str()), status, tN, -1.0, [&]() {
                                if (uiButton(inp, ui, "MsStSel", sidx, "Select", "Select the station", false, en))
                                    stationSel = sidx;
                                if (uiButton(inp, ui, "MsStGo", sidx, "Go to", "Fly to the station (follow mode)", false, en))
                                    stationGo = sidx;
                            });
                        }
                        // Satellite launches in flight.
                        uiSection("MsOSec", 2, kCats[2]);
                        int n2 = 0;
                        for (size_t i = 0; i < msOngoing.size(); ++i)
                        {
                            const MissionOngoing &o = msOngoing[i];
                            if (o.cat != 2)
                                continue;
                            ++n2;
                            const int idx = (int)i;
                            ongoingRow(rid++, msStr("%s", o.title.c_str()), msStr("%s", o.status.c_str()), o.tNext, o.progress, [&]() {
                                if (uiButton(inp, ui, "MsSelRocket", idx, "Select",
                                             o.drawn ? "Select the rocket" : "Select the rocket (not drawn from here: below the horizon or too faint)"))
                                    selRocket = idx;
                                if (uiButton(inp, ui, "MsGoRocket", idx, "Go to", "Fly to the rocket and ride along"))
                                    ongoingGo = idx;
                                if (uiButton(inp, ui, "MsNextMs", idx, "Next >",
                                             "Skip to just before this launch's next milestone, at 1x (a selected or followed "
                                             "stage stays selected / followed)"))
                                    ongoingNext = idx;
                            });
                        }
                        if (!n2)
                            text("No rocket in flight.", Pal::textDim, 11);
                        // Trains raising, retirees deorbiting.
                        uiSection("MsOSec", 3, kCats[3]);
                        int n3 = 0;
                        for (size_t i = 0; i < msOngoing.size(); ++i)
                        {
                            const MissionOngoing &o = msOngoing[i];
                            if (o.cat != 3 && o.cat != 4)
                                continue;
                            ++n3;
                            const int idx = (int)i;
                            ongoingRow(rid++, msStr("%s", o.title.c_str()), msStr("%s", o.status.c_str()), o.tNext, o.progress, [&]() {
                                if (o.cat == 3)
                                {
                                    if (uiButton(inp, ui, "MsSelTrain", idx, "Select", "The nearest satellite of these trains"))
                                        selTrain = idx;
                                    if (uiButton(inp, ui, "MsGoTrain", idx, "Go to", "Fly to the nearest satellite of these trains"))
                                        ongoingGo = idx;
                                }
                                else
                                {
                                    if (uiButton(inp, ui, "MsSelRet", idx, "Select", "The nearest of these retirees"))
                                        selRetiree = idx;
                                    if (uiButton(inp, ui, "MsGoRet", idx, "Go to", "Fly to the nearest of these retirees"))
                                        ongoingGo = idx;
                                    if (uiButton(inp, ui, "MsReentry", idx, "Watch",
                                                 "The next retiree (any program) coming down over a dark sky: 25 s before it breaks up"))
                                        watchReentry = true;
                                }
                            });
                        }
                        if (!n3)
                            text(launchesEnabled ? "Nothing raising or deorbiting." : "Launches are off (Controls).", Pal::textDim, 11);
                    }
                    else
                    {
                        // ── Controls (moved from Settings > Constellations) ──────────────────────────
                        buildLaunchSettingsSection(inp, ui);
                        buildKesslerSettingsSection(inp, ui);
                    }
                }
                ui.scrollbar(CLAY_ID("MsList"));
                if (msTab == 0)
                {
                // ── The Sun's Fury: pinned under the list on Upcoming, always visible (2026-10-20: at the default
                // height it was the last card of the list and needed scrolling to find) ─────────────────────
                
                const Clay_Color furyBg = kesslerEnabled ? Pal::furyBg : Pal::cardBg;
                CLAY(CLAY_ID("MsFury"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                    .padding = {10, 10, 8, 9},
                                                    .childGap = 4,
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                         .backgroundColor = furyBg,
                                         .cornerRadius = CLAY_CORNER_RADIUS(4),
                                         .border = {.color = kesslerEnabled ? Pal::furyBorder : Pal::cardBorder,
                                                    .width = CLAY_BORDER_ALL((uint16_t)(kesslerEnabled ? 2 : 1))}})
                {
                    const double dt = kesslerEpoch() - now;
                    CLAY(CLAY_ID("MsFuryHead"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                            .childGap = 8,
                                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY(CLAY_ID("MsFuryTitle"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                 .childGap = 2,
                                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                        {
                            text("THE SUN'S FURY", kesslerEnabled ? Pal::furyText : Pal::textDim, 16);
                            text("Kessler simulation", kesslerEnabled ? Pal::furyText : Pal::textDim, 11);
                        }
                        char cd[48];
                        if (dt > 0.0)
                            fmtCountdown(dt, cd, sizeof(cd));
                        else
                            snprintf(cd, sizeof(cd), "DAY %.1f", -dt / kDay);
                        text(msStr("%s", cd), kesslerEnabled ? Pal::furyText : Pal::textDim, 16);
                    }
                    char ua[48], uf[48];
                    fmtUtcShort(kesslerEpoch(), ua, sizeof(ua));
                    fmtUtcShort(kesslerEpoch() - kFuryFlareLead, uf, sizeof(uf));
                    if (!kesslerEnabled)
                        text("The scenario is off. Turn it on in Controls.", Pal::textDim, 11, true);
                    else if (dt > 0.0)
                        text(msStr("%s: an X-class flare. %s: its coronal mass ejection arrives. Satellites die in "
                                   "the storm, the dead tumble and collide, and every collision is a cloud of "
                                   "debris that feeds the next.",
                                   uf, ua),
                             Pal::textPrimary, 11, true);
                    else
                    {
                        const nlohmann::json &j = kesslerStateCached();
                        const nlohmann::json &n = j["now"];
                        text(msStr("The cascade is running: %d dead satellites tumbling, %d destroyed or re-entered, "
                                   "%d collisions (%d catastrophic).",
                                   n.value("dead_satellites", 0), n.value("roster_gone", 0), n.value("collisions", 0),
                                   n.value("catastrophic", 0)),
                             Pal::textPrimary, 11, true);
                    }
                    CLAY(CLAY_ID("MsFuryBtns"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                            .childGap = 4,
                                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        if (kesslerEnabled)
                        {
                            if (uiButton(inp, ui, "MsFuryGo", 0, "Go to the arrival",
                                         "Two minutes before the CME arrives: dusk over Monument Valley, looking north"))
                                furyGo = 0;
                            if (uiButton(inp, ui, "MsFuryGo", 1, "The storm night",
                                         "3.5 hours in: the aurora overhead (the bookmark \"The Sun's Fury\")"))
                                furyGo = 1;
                        }
                        else if (uiButton(inp, ui, "MsFuryCtl", 0, "Controls", "The scenario's switch"))
                            toControls = true;
                    }
                }
                }
            }
        });

    // After the layout.
    std::string msg;
    auto launchOf = [&](int id) -> const MissionLaunch * {
        if (id >= 0 && id < (int)msNext.size())
            return &msNext[(size_t)id];
        if (id == 100 && msHasTwilight)
            return &msTwilight;
        if (id == 101 && !msCrewList.empty())
            return &msCrewList.front();
        return nullptr;
    };
    if (const MissionLaunch *m = launchOf(goLaunch >= 0 ? goLaunch : watchLaunch))
    {
        launchGoToKey(m->key, goLaunch >= 0, kLeadS, msg);
        msStatus = msg;
        msNextWall = msPinWall = -1e300; // rebuilt next frame (Clay still holds this frame's strings)
    }
    if (furyGo >= 0)
    {
        missionsGoToFury(furyGo, msg);
        msStatus = msg;
        msNextWall = msPinWall = -1e300; // rebuilt next frame (Clay still holds this frame's strings)
    }
    if (toControls)
        msTab = 2;
    if (selRocket >= 0 && selRocket < (int)msOngoing.size())
    {
        launchSelectRocketOf(msOngoing[(size_t)selRocket].key, msg);
        msStatus = msg;
    }
    if (ongoingNext >= 0)
    {
        missionsMilestone(ongoingNext, 1, -1, msg);
        msStatus = msg;
        msNextWall = msPinWall = -1e300; // rebuilt next frame (Clay still holds this frame's strings)
    }
    if (ongoingGo >= 0 && ongoingGo < (int)msOngoing.size())
    {
        missionsOngoingAct(ongoingGo, true, msg);
        msStatus = msg;
    }
    if (selTrain >= 0 && selTrain < (int)msOngoing.size())
    {
        launchSelectNearestOf(msOngoing[(size_t)selTrain].prog, 0, msg);
        msStatus = msg;
    }
    if (selRetiree >= 0 && selRetiree < (int)msOngoing.size())
    {
        launchSelectNearestOf(msOngoing[(size_t)selRetiree].prog, 1, msg);
        msStatus = msg;
    }
    if (watchReentry)
    {
        launchGoToNextReentry(msg);
        msStatus = msg;
        msNextWall = msPinWall = -1e300; // rebuilt next frame (Clay still holds this frame's strings)
    }
    const double tNowMs = now;
    auto stationGone = [&](int i) {
        return i >= 0 && i < (int)msStations.size() && msStations[(size_t)i].sat >= 0 &&
               rosterFateAt((uint32_t)msStations[(size_t)i].sat, tNowMs) >= 2;
    };
    if (stationGone(stationSel) || stationGone(stationGo))
    {
        msStatus = constellations[msStations[(size_t)std::max(stationSel, stationGo)].constIdx].name + " no longer exists";
        stationSel = stationGo = -1;
    }
    if (stationSel >= 0 && stationSel < (int)msStations.size())
    {
        selectSatellite(msStations[(size_t)stationSel].sat);
        msStatus = "Selected " + constellations[msStations[(size_t)stationSel].constIdx].name;
    }
    if (stationGo >= 0 && stationGo < (int)msStations.size())
    {
        const int sat = msStations[(size_t)stationGo].sat;
        selectSatellite(sat);
        startFollow(sat, true);
        msStatus = "Flying to " + constellations[msStations[(size_t)stationGo].constIdx].name;
    }
}
