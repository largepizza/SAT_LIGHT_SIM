// ─── Event windows (2026-11-17) ──────────────────────────────────────────────────────────────────────────────────────
// The story's events as one modal window in the centre (docs/subsystems/sky-events.md "Event windows"): a banner
// (assets/events/<id>.png, 960 x 320, or a labelled placeholder), the date, a title, a short text and one button.
// A window opens when the RUNNING clock crosses an event's moment, forward or backward, at any rate (the state events:
// forward only), or when a Watch takes the player to it; a clock that is SET past an event shows nothing. As it opens
// the clock goes to 1x forward, paused while it is open; its button runs the clock on at 1x. The explode warning (the first player breakup before
// the Sun's Fury, docs/subsystems/kessler.md "The explode warning") is the same window with three buttons.
//
// The rows are kEventWinDefs below: an id (the banner's file name and the harness id), the text, the Sky events kind
// whose Watch lands on it, how it triggers and what it needs. Its moments come from eventWinMomentsOf.
#include "../FrameProfile.h"
#include "../Harness.h"
#include "../Log.h"
#include "../UIRenderer.h"
#include "../UiText.h"
#include "../VulkanContext.h"
#include "../AudioSystem.h"
#include "SatelliteSim.h"
#include "UIPalette.h"
#include "clay.h"
#include "stb_image.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>

std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp
glm::dvec3 skyEarthHelioAt(double t);     // SatelliteSim.cpp

namespace
{
Clay_String evStr(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
constexpr double kDayS = 86400.0;
constexpr double kMarsOrbitAu = 1.5237;           // "enters the inner planets": crosses Mars's orbit inbound
constexpr double kPeriSurviveS = 6.0 * 3600.0;    // "survives perihelion": 6 h after it, out of the corona
constexpr double kSolarSearchFrom = 1184068800.0; // 2037-07-10 00:00 UTC: the next solar eclipse is the 2037-07-13 total
constexpr double kSameMomentS = 1.0;              // events this close are one moment: shown one after another

enum EvRow
{
    evLunar,
    evCometIn,
    evCometPeri,
    evCometEarth,
    evCometStorm,
    evSolar,
    evFury,
    evFirstCollision,
    evTenPercent,
    evHalf,
    evNova,
    evNovaFade,
    evRowCount
};

enum EvTrig : uint8_t
{
    kTrigMoment = 0,  // the running clock crosses its moment, either way
    kTrigForward = 1, // the same, forward only (the first collision: a fact of the cascade's history)
    kTrigShare = 2,   // the share of satellites destroyed crosses `share` while the clock runs forward
};
enum EvNeeds : uint8_t
{
    kNeedNone = 0,
    kNeedFury = 1, // the Sun's Fury on (kessler.enabled)
    kNeedNova = 2, // the supernova on (stars.supernova)
};

struct EventWinDef
{
    const char *id; // assets/events/<id>.png and the harness id
    Txt title, text;
    Txt titleAlt, textAlt; // the lunar eclipse when total
    Txt btn;
    int skyKind;           // the Sky events kind whose Watch lands on it (-1 none)
    uint8_t trig, needs;
    double share;          // kTrigShare
    const Clay_Color *accent;
};

// In story order. A new event: a row here, its EvRow, its moments in eventWinMomentsOf, its text in ui_text_en.inc.
const EventWinDef kEventWinDefs[evRowCount] = {
    {"lunar_eclipse", Txt::EvTitleLunarPartial, Txt::EvTextLunarPartial, Txt::EvTitleLunarTotal, Txt::EvTextLunarTotal,
     Txt::EvBtnLunar, sky::kLunarEclipse, kTrigMoment, kNeedNone, 0.0, &Pal::textWarn},
    {"comet_inner_planets", Txt::EvTitleCometInner, Txt::EvTextCometInner, Txt::EvTitleCometInner, Txt::EvTextCometInner,
     Txt::EvBtnCometInner, sky::kComet, kTrigMoment, kNeedNone, 0.0, &Pal::spComet},
    {"comet_perihelion", Txt::EvTitleCometPeri, Txt::EvTextCometPeri, Txt::EvTitleCometPeri, Txt::EvTextCometPeri,
     Txt::EvBtnCometPeri, sky::kComet, kTrigMoment, kNeedNone, 0.0, &Pal::spComet},
    {"comet_passes_earth", Txt::EvTitleCometEarth, Txt::EvTextCometEarth, Txt::EvTitleCometEarth, Txt::EvTextCometEarth,
     Txt::EvBtnCometEarth, sky::kComet, kTrigMoment, kNeedNone, 0.0, &Pal::spComet},
    {"comet_meteor_storm", Txt::EvTitleCometStorm, Txt::EvTextCometStorm, Txt::EvTitleCometStorm, Txt::EvTextCometStorm,
     Txt::EvBtnCometStorm, sky::kCometStorm, kTrigMoment, kNeedNone, 0.0, &Pal::spStorm},
    {"australasian_eclipse", Txt::EvTitleSolar, Txt::EvTextSolar, Txt::EvTitleSolar, Txt::EvTextSolar, Txt::EvBtnSolar,
     sky::kSolarEclipse, kTrigMoment, kNeedNone, 0.0, &Pal::textWarn},
    {"suns_fury", Txt::EvTitleFury, Txt::EvTextFury, Txt::EvTitleFury, Txt::EvTextFury, Txt::EvBtnFury, -1, kTrigMoment,
     kNeedFury, 0.0, &Pal::furyBorder},
    {"first_collision", Txt::EvTitleFirstCollision, Txt::EvTextFirstCollision, Txt::EvTitleFirstCollision,
     Txt::EvTextFirstCollision, Txt::EvBtnFirstCollision, -1, kTrigForward, kNeedFury, 0.0, &Pal::furyBorder},
    {"destroyed_10", Txt::EvTitleTenPercent, Txt::EvTextTenPercent, Txt::EvTitleTenPercent, Txt::EvTextTenPercent,
     Txt::EvBtnTenPercent, -1, kTrigShare, kNeedFury, 0.10, &Pal::furyBorder},
    {"destroyed_50", Txt::EvTitleHalf, Txt::EvTextHalf, Txt::EvTitleHalf, Txt::EvTextHalf, Txt::EvBtnHalf, -1, kTrigShare,
     kNeedFury, 0.50, &Pal::furyBorder},
    {"betelgeuse_explodes", Txt::EvTitleNova, Txt::EvTextNova, Txt::EvTitleNova, Txt::EvTextNova, Txt::EvBtnNova,
     sky::kSupernova, kTrigMoment, kNeedNova, 0.0, &Pal::spNova},
    {"betelgeuse_fades", Txt::EvTitleNovaFade, Txt::EvTextNovaFade, Txt::EvTitleNovaFade, Txt::EvTextNovaFade,
     Txt::EvBtnNovaFade, sky::kSupernova, kTrigMoment, kNeedNova, 0.0, &Pal::spNova},
};
constexpr const char *kWarnId = "explode_warning";

int evRowOf(const char *id)
{
    for (int i = 0; i < evRowCount; ++i)
        if (std::strcmp(kEventWinDefs[i].id, id) == 0)
            return i;
    return -1;
}
constexpr int kWarnDef = 1000; // = SatelliteSim::kEvWarn (private there)
const char *evIdOf(int def) { return def == kWarnDef ? kWarnId : (def >= 0 && def < evRowCount ? kEventWinDefs[def].id : "?"); }
} // namespace

// ── When ───────────────────────────────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::eventWinLive() const
{
    if (!eventWinEnabled || (harness::active() && !eventWinHarnessOn_))
        return false;
    // Never over the opening's captions, a cinematic, a video export or an HQ photo.
    return !showIntro && !cineActive() && !video_.busy() && photoState == 0;
}

// The moments the worker finds (pure functions of time: the Moon and Sun of date, the comet's orbit): the lunar
// eclipses over a range round the clock (re-run when the clock nears its ends), the 2037-07-13 solar eclipse, and the
// Great Comet's crossing of Mars's orbit and its closest approach to the Earth.
void SatelliteSim::eventWinMomentsTick(double now)
{
    if (evMomentsJob_.valid() && evMomentsJob_.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        evMoments_ = evMomentsJob_.get();
        evMomentsValid_ = true;
    }
    if (evMomentsJob_.valid())
        return;
    if (evMomentsValid_ && now > evMoments_.lo + 60.0 * kDayS && now < evMoments_.hi - 60.0 * kDayS)
        return;
    const double lo = now - 730.0 * kDayS, hi = now + 1460.0 * kDayS;
    evMomentsJob_ = std::async(std::launch::async, [this, lo, hi]() {
        EvMoments m;
        m.lo = lo;
        m.hi = hi;
        for (double t = lo; t < hi;)
        {
            const double te = findEclipse(false, t);
            if (te < 0.0 || te > hi)
                break;
            // total: the Moon wholly inside the umbra at greatest eclipse (skySearchLunar's rule)
            const glm::dvec3 M = moonGeoEciM(te);
            const glm::dvec3 s = -glm::normalize(sunDirEciAt(te));
            const double sep = std::acos(std::clamp(glm::dot(glm::normalize(M), s), -1.0, 1.0));
            const double dM = glm::length(M), dS = sunDistAuAt(te) * 1.495978707e11;
            const double piM = std::asin(6378137.0 / dM), piS = 6378137.0 / dS;
            const double sS = std::asin(satphot::kSunRadiusM / dS), sM = std::asin(1737400.0 / dM);
            const double U = 1.0118 * (piM + piS - sS);
            m.lunarT.push_back(te);
            m.lunarTotal.push_back(sep < U - sM ? 1 : 0);
            t = te + 20.0 * kDayS;
        }
        m.solarT = findEclipse(true, kSolarSearchFrom);
        const comet::Elements &g = comet::greatComet();
        auto rAt = [&](double t) { return glm::length(comet::stateAt(g, t).r); };
        double a = g.tPeri - 3.0 * 365.25 * kDayS, b = g.tPeri;
        if (rAt(a) > kMarsOrbitAu)
        {
            for (int i = 0; i < 60; ++i)
            {
                const double mid = 0.5 * (a + b);
                (rAt(mid) > kMarsOrbitAu ? a : b) = mid;
            }
            m.cometInT = b;
        }
        auto dAt = [&](double t) { return glm::length(comet::stateAt(g, t).r - skyEarthHelioAt(t)); };
        double best = 1e30, bt = g.tPeri;
        for (double t = g.tPeri; t < g.tPeri + 120.0 * kDayS; t += 6.0 * 3600.0)
            if (const double d = dAt(t); d < best)
            {
                best = d;
                bt = t;
            }
        double l = bt - 6.0 * 3600.0, h = bt + 6.0 * 3600.0;
        for (int i = 0; i < 60; ++i)
        {
            const double x = l + 0.382 * (h - l), y = l + 0.618 * (h - l);
            if (dAt(x) < dAt(y))
                h = y;
            else
                l = x;
        }
        m.cometEarthT = 0.5 * (l + h);
        return m;
    });
}

// Every known moment of row `def` (none while the worker has not answered, or the row's feature is off).
void SatelliteSim::eventWinMomentsOf(int def, std::vector<EvOcc> &out) const
{
    out.clear();
    if (def < 0 || def >= evRowCount)
        return;
    const EventWinDef &d = kEventWinDefs[def];
    if ((d.needs & kNeedFury) && !kesslerEnabled)
        return;
    if ((d.needs & kNeedNova) && !supernovaEnabled)
        return;
    auto add = [&](double t, int flag = 0) {
        if (t > 0.0)
            out.push_back(EvOcc{def, t, flag});
    };
    switch (def)
    {
    case evLunar:
        if (evMomentsValid_)
            for (size_t i = 0; i < evMoments_.lunarT.size(); ++i)
                add(evMoments_.lunarT[i], evMoments_.lunarTotal[i]);
        break;
    case evCometIn: if (evMomentsValid_) add(evMoments_.cometInT); break;
    case evCometPeri: add(comet::greatComet().tPeri + kPeriSurviveS); break;
    case evCometEarth: if (evMomentsValid_) add(evMoments_.cometEarthT); break;
    case evCometStorm: add(meteors::cometStorm().tPeak); break;
    case evSolar: if (evMomentsValid_) add(evMoments_.solarT); break;
    case evFury: add(kesslerEpoch()); break;
    case evFirstCollision: if (evw_.firstColT < 1e299) add(evw_.firstColT); break;
    case evNova: add(kSupernovaT0); break;
    case evNovaFade: add(supernovaEndT()); break;
    default: break; // the shares: no moment of their own
    }
}

// ── The running clock (recordCompute, after the advance and the Kessler frontier hold) ─────────────────────────────
// tBefore = the clock before this frame's advance, tAfter = after it: what the clock RAN through this frame, the only
// interval a moment is looked for in. Anything that moved the clock between two frames (a Watch, a bookmark, a
// snapshot, a Kessler rollback, `time set`, the time field) SET it: that jump is never looked at (this frame starts
// somewhere else than the last one ended), and it also drops the share baseline and the Watch's shown event. This is the
// one place that tells the two apart.
void SatelliteSim::eventWinAdvance(double tBefore, double tAfter)
{
    FPROF_SCOPE("eventWinAdvance");
    eventWinMomentsTick(tAfter);
    const bool running = evw_.lastValid && std::abs(tBefore - evw_.lastT) < 1e-6;
    evw_.lastT = tAfter;
    evw_.lastValid = true;
    if (!running)
    {
        evw_.watched.clear();
        evw_.walkPrevValid = false;
        evw_.walkActive = false;
    }
    if (tAfter < tBefore)
        evw_.walkPrevValid = false; // (a share crossed going back is not shown; the next walk is a new baseline)
    eventWinKesslerTick(tAfter, running && tAfter >= tBefore);
    // The crossing is this frame's own advance (tBefore, tAfter]: a set happened before tBefore and is never looked at,
    // so the first frame after a `time set` still counts what the clock ran through from there.
    if (tAfter == tBefore || evw_.open || !eventWinLive())
        return;
    const bool fwd = tAfter > tBefore;
    std::vector<EvOcc> occ, hit;
    for (int d = 0; d < evRowCount; ++d)
    {
        const EventWinDef &row = kEventWinDefs[d];
        if (row.trig == kTrigShare || (row.trig == kTrigForward && !fwd))
            continue;
        eventWinMomentsOf(d, occ);
        for (const EvOcc &o : occ)
            if (fwd ? (o.t > tBefore && o.t <= tAfter) : (o.t >= tAfter && o.t < tBefore))
                hit.push_back(o);
    }
    if (hit.empty())
        return;
    // The first one met in the direction of travel, and every event at that same moment (shown one after another).
    double t1 = hit[0].t;
    for (const EvOcc &o : hit)
        t1 = fwd ? std::min(t1, o.t) : std::max(t1, o.t);
    std::vector<EvOcc> now;
    auto same = [](const EvOcc &a, const EvOcc &b) { return a.def == b.def && std::abs(a.t - b.t) < kSameMomentS; };
    for (const EvOcc &o : hit)
        if (std::abs(o.t - t1) < kSameMomentS)
        {
            // (shown already with the clock before it: a Watch's landing, a backward crossing resumed forward at 1x,
            // the second of a queued pair)
            if (std::any_of(evw_.watched.begin(), evw_.watched.end(), [&](const EvOcc &w) { return same(w, o); }))
                continue;
            now.push_back(o);
        }
    // (the ones the clock ran through are behind it now: dropped)
    evw_.watched.erase(std::remove_if(evw_.watched.begin(), evw_.watched.end(),
                                      [&](const EvOcc &w) {
                                          return std::any_of(hit.begin(), hit.end(), [&](const EvOcc &h) { return same(w, h); });
                                      }),
                       evw_.watched.end());
    if (now.empty())
        return;
    // The clock back to the moment (at 1 week/s a frame is a day: the thing would be over), then the window. A
    // millisecond past it in the direction of travel, so the next frame's interval cannot meet it again.
    const double tAt = t1 + (fwd ? 1e-3 : -1e-3);
    const double back = tAfter - tAt;
    const double days = std::floor(tAt / kDayS);
    simDayJ2000 = (int64_t)days;
    simSecInDay = tAt - days * kDayS;
    cv2SimAdvanceAcc -= back; // (the clouds' history: a step back, not a jump)
    evw_.lastT = (double)simDayJ2000 * kDayS + simSecInDay;
    for (const EvOcc &o : now)
        eventWinOpen(o, fwd ? "crossed forward" : "crossed backward");
}

// The state events. The first collision: the earliest engine collision or bump in the mirror's event log (a sliced scan
// from its start; the log is in step order, so the scan only rebuilds the times of events in the steps before the best
// so far). The shares: a sliced walk of the roster's dead seconds (200k satellites a frame), the share of the satellites
// there at the CME that are dead at the walk's second; shown when it passes 10% / 50% between two walks with the clock
// running forward all the while.
void SatelliteSim::eventWinKesslerTick(double now, bool forward)
{
    if (kesslerEnabled && kEvents.src == nullptr)
    {
        // A cascade replay under way (a step back past the mirror's kept history): no log to read yet. The share
        // baseline stays (it was dropped here, and the replay's first walks then re-crossed 10% under a 50% window).
        evw_.walkActive = false;
        return;
    }
    if (!kesslerEnabled)
    {
        evw_.firstColT = 1e300;
        evw_.colScan = 0;
        evw_.walkActive = false;
        evw_.walkPrevValid = false;
        evw_.destroyedFrac = -1.0;
        return;
    }
    FPROF_SCOPE("eventWinKesslerTick");
    if (kEvents.src != evw_.colSrc || kEventsRev != evw_.colRev || kEvents.size() < evw_.colScan)
    {
        evw_.colSrc = kEvents.src;
        evw_.colRev = kEventsRev;
        evw_.colScan = 0;
        evw_.firstColT = 1e300;
    }
    const double stepS = kessler::WorldSettings{}.stepS;
    for (const size_t end = std::min(kEvents.size(), evw_.colScan + 200000); evw_.colScan < end; ++evw_.colScan)
    {
        const kessler::EventLog::Core &ec = kEvents.core(evw_.colScan);
        const uint8_t kind = (uint8_t)(ec.kc & 7u), origin = (uint8_t)((ec.kc >> 3) & 3u);
        if ((kind != kessler::EV_COLLISION && kind != kessler::EV_BUMP) || origin == kessler::ORIGIN_USER)
            continue;
        if (kEvents.tUpper(evw_.colScan) - stepS >= evw_.firstColT)
        {
            evw_.colScan = end - 1; // (step order: everything after this is later too; new events are later still)
            continue;
        }
        evw_.firstColT = std::min(evw_.firstColT, kEvents.tOf(evw_.colScan));
    }
    const double epoch = kesslerEpoch();
    if (now <= epoch || !forward)
    {
        if (now <= epoch)
        {
            evw_.walkPrevValid = true; // before the CME nothing is destroyed: a baseline of 0
            evw_.walkPrevFrac = 0.0;
            evw_.destroyedFrac = 0.0;
        }
        return;
    }
    const int32_t sec = (int32_t)std::clamp(std::floor(now - epoch), -2147483000.0, 2147483000.0);
    if (!evw_.walkActive)
    {
        if (sec == evw_.walkSec && evw_.walkPrevValid)
            return; // (the clock has not moved a second since the last walk)
        evw_.walkActive = true;
        evw_.walkI = 0;
        evw_.walkSec = sec;
        evw_.walkN0 = evw_.walkDead = 0;
    }
    const size_t nRoster = std::min(kRosterWords.size() / 2, (size_t)activeSatCount);
    for (const size_t end = std::min(nRoster, evw_.walkI + 200000); evw_.walkI < end; ++evw_.walkI)
    {
        const size_t i = evw_.walkI;
        if (2 * i + 1 < rosterLife.size() && (0 < rosterLife[2 * i] || 0 >= rosterLife[2 * i + 1]))
            continue; // (not there at the CME: not launched yet, or retired with its group)
        ++evw_.walkN0;
        if (evw_.walkSec >= kRosterWords[2 * i])
            ++evw_.walkDead;
    }
    if (evw_.walkI < nRoster)
        return;
    evw_.walkActive = false;
    double frac = evw_.walkN0 ?(double)evw_.walkDead / (double)evw_.walkN0 : 0.0;
    evw_.destroyedFrac = frac;
    // A walk that ends under an open window (another event's, met while this walk lagged) keeps the last baseline: the
    // next walk, once the clock runs on, still sees the share passed (it was lost: the 50% window at 1 yr/s never opened).
    if (evw_.open && evw_.walkPrevValid && eventWinLive())
        return;
    if (evw_.walkPrevValid && !evw_.open && eventWinLive())
    {
        // The walk lags the clock (~7 frames: at 1 yr/s that is ~2 months), and the share was passed somewhere between
        // the last walk's second and this one's. Its own second is the k-th earliest death among the counted satellites
        // (k = ceil(share x N0)): one pass over the roster and an nth_element, once per crossing. The clock goes back to
        // it, as for a crossed moment, so the window and the 1x after it are at the event, not months past it.
        std::vector<int32_t> deaths;
        double tFirst = 1e300;
        std::vector<EvOcc> crossed;
        for (int d = 0; d < evRowCount; ++d)
        {
            const EventWinDef &row = kEventWinDefs[d];
            if (row.trig != kTrigShare || !(evw_.walkPrevFrac < row.share && frac >= row.share))
                continue;
            if (deaths.empty())
            {
                deaths.reserve(evw_.walkDead);
                for (size_t i = 0; i < nRoster; ++i)
                {
                    if (2 * i + 1 < rosterLife.size() && (0 < rosterLife[2 * i] || 0 >= rosterLife[2 * i + 1]))
                        continue;
                    if (evw_.walkSec >= kRosterWords[2 * i])
                        deaths.push_back((int32_t)kRosterWords[2 * i]);
                }
            }
            const size_t k = (size_t)std::ceil(row.share * (double)evw_.walkN0);
            double t = now;
            if (k >= 1 && k <= deaths.size())
            {
                std::nth_element(deaths.begin(), deaths.begin() + (k - 1), deaths.end());
                t = std::min(now, epoch + (double)deaths[k - 1] + 0.5); // (half a second in: the walk's floor sees it)
            }
            crossed.push_back(EvOcc{d, t, 0});
            tFirst = std::min(tFirst, t);
        }
        if (!crossed.empty())
        {
            // Only the first share passed opens (both in one walk at 1 yr/s): the clock is at ITS second, so the next
            // share is still ahead and opens when the running clock passes it (the baseline is the first share).
            const EvOcc first = *std::min_element(crossed.begin(), crossed.end(),
                                                  [](const EvOcc &a, const EvOcc &b) { return a.t < b.t; });
            crossed.assign(1, first);
            frac = kEventWinDefs[first.def].share;
            if (tFirst < now)
            {
                const double back = now - tFirst;
                const double days = std::floor(tFirst / kDayS);
                simDayJ2000 = (int64_t)days;
                simSecInDay = tFirst - days * kDayS;
                cv2SimAdvanceAcc -= back; // (the clouds' history: a step back, not a jump)
                evw_.lastT = tFirst;
                evw_.walkSec = (int32_t)std::floor(tFirst - epoch);
                // (a window shown meanwhile, now ahead of the clock again: the clock running through it is that showing)
                for (const EvOcc &r : evw_.recent)
                    if (r.t > tFirst && r.t <= now)
                        evw_.watched.push_back(r);
            }
            for (const EvOcc &o : crossed)
                eventWinOpen(o, "share crossed");
        }
    }
    // The share only grows with a clock running forward: a lower walk is the mirror not there yet (a cascade replay after
    // a step back, the window's own snap back included, republishes the dead seconds from the start), so the baseline
    // never drops while the clock runs forward (it did: 10% opened a second time ~20 s after the 50% window's replay).
    evw_.walkPrevFrac = evw_.walkPrevValid ? std::max(evw_.walkPrevFrac, frac) : frac;
    evw_.walkPrevValid = true;
}

// ── Taken to an event (a Watch) ────────────────────────────────────────────────────────────────────────────────────
// Of the row's moments, the latest one not more than 6 h after the landing (the event the player came to see, or the
// one under way), else the earliest.
void SatelliteSim::eventWinTakenTo(const char *id, double tNear)
{
    const int def = evRowOf(id);
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    evw_.lastT = now; // (the Watch's own jump is not a crossing; the clock runs on from here)
    evw_.lastValid = true;
    if (def < 0 || !eventWinLive())
        return;
    std::vector<EvOcc> occ;
    eventWinMomentsOf(def, occ);
    if (occ.empty())
        return;
    const EvOcc *pick = nullptr;
    for (const EvOcc &o : occ)
        if (o.t <= tNear + 6.0 * 3600.0 && (!pick || o.t > pick->t))
            pick = &o;
    if (!pick)
        pick = &*std::min_element(occ.begin(), occ.end(), [](const EvOcc &a, const EvOcc &b) { return a.t < b.t; });
    eventWinOpen(*pick, "watch");
}

// skyEventWatch, after a Watch landed: the rows of that kind whose moment lies in the event's window (+-2 days; the
// Great Comet's three by its label), the latest one reached by now. The Sun's Fury is missionsGoToFury's (every Fury
// Watch goes through it).
void SatelliteSim::eventWinOnSkyWatch(const sky::Event &e)
{
    if (e.kind == sky::kComet && std::strncmp(e.label, "C/2036 X1", 9) != 0)
        return;
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    std::vector<EvOcc> occ;
    const EvOcc *pick = nullptr;
    EvOcc best;
    for (int d = 0; d < evRowCount; ++d)
    {
        if (kEventWinDefs[d].skyKind != e.kind)
            continue;
        eventWinMomentsOf(d, occ);
        for (const EvOcc &o : occ)
        {
            if (o.t < e.t0 - 2.0 * kDayS || o.t > e.t1 + 2.0 * kDayS)
                continue;
            const bool reached = o.t <= now + 6.0 * 3600.0;
            const bool bestReached = pick && best.t <= now + 6.0 * 3600.0;
            if (!pick || (reached && (!bestReached || o.t > best.t)) || (!reached && !bestReached && o.t < best.t))
            {
                best = o;
                pick = &best;
            }
        }
    }
    evw_.lastT = now;
    evw_.lastValid = true;
    if (!pick || !eventWinLive())
        return;
    eventWinOpen(best, "watch");
}

// ── Open / choose ──────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::eventWinOpen(const EvOcc &o, const char *how)
{
    if (evw_.open)
    {
        evw_.queue.push_back(o);
        return;
    }
    evw_.open = true;
    evw_.cur = o;
    evw_.lastHow = how;
    evBannerFor_.clear(); // (the banner is read again at every opening: art dropped in meanwhile shows)
    evw_.prevPaused = timePaused;
    evw_.prevScale = timeScaleIdx;
    evw_.prevDir = timeDir;
    timePaused = true;
    if (o.def != kEvWarn)
    {
        // An event's card sets the clock to 1x forward as it opens (2026-11-18, the user: "so when it resumes we aren't
        // speeding away from it"): the time bar reads 1x while the card is up, and nothing can resume the old rate.
        timeScaleIdx = 0;
        timeDir = 1.0f;
    }
    if (audio_)
        audio_->playSfx("assets/sound/ui/event.wav"); // (the sfx bus: the master and effects volumes)
    if (o.def != kEvWarn)
    {
        // Shown with the clock before its moment (a Watch's landing, a backward crossing, which resumes forward, the
        // second of a queued pair): the running clock then crossing it is this same showing (eventWinAdvance).
        if (o.t > (double)simDayJ2000 * kDayS + simSecInDay)
            evw_.watched.push_back(o);
        evw_.recent.push_back(o);
        if (evw_.recent.size() > 16)
            evw_.recent.erase(evw_.recent.begin());
        ++evw_.shown;
        evw_.log.push_back(std::string(evIdOf(o.def)) + " " + how + " " + fmtUtcReadable(o.t));
        if (evw_.log.size() > 64)
            evw_.log.erase(evw_.log.begin());
    }
    Log::line(std::string("event window: ") + evIdOf(o.def) + " (" + how + ")");
}

void SatelliteSim::eventWinChoose(int choice)
{
    if (!evw_.open)
        return;
    const EvOcc o = evw_.cur;
    evw_.open = false;
    evw_.lastChoice = choice;
    if (o.def == kEvWarn)
    {
        // The warning gives the clock back as it found it.
        timePaused = evw_.prevPaused;
        timeScaleIdx = evw_.prevScale;
        timeDir = evw_.prevDir;
        const int sat = evw_.warnSat;
        evw_.warnSat = -1;
        if (choice == 1 && sat >= 0)
            infoExplodeNow(sat, evw_.warnKind);
        else if (choice == 3)
        {
            std::string msg;
            const bool ok = missionsGoToFury(1, msg); // the Sun's Fury's own Watch (and its window)
            snprintf(screenshotToastText, sizeof(screenshotToastText), "%s", msg.c_str());
            screenshotToastTimer = 4.0f;
            screenshotToastWarn = !ok;
        }
        Log::line(std::string("explode warning: ") + (choice == 1 ? "yes" : choice == 3 ? "praise the sun" : "no"));
    }
    else
    {
        // An event runs on at 1x, so it can be watched (forward, whichever way the clock was going).
        timePaused = false;
        timeScaleIdx = 0;
        timeDir = 1.0f;
    }
    if (!evw_.open && !evw_.queue.empty())
    {
        const EvOcc next = evw_.queue.front();
        evw_.queue.erase(evw_.queue.begin());
        eventWinOpen(next, "queued");
    }
}

// ── The explode warning ────────────────────────────────────────────────────────────────────────────────────────────
// The Sun's Fury ships prebaked (docs/subsystems/kessler.md "Cache and the shipped prebake"): its history is the world
// that starts at the CME. A player breakup before the CME moves the world's start to that breakup's step, so no cache
// matches and the whole cascade is computed live (kesslerRestart). So the warning shows while the clock is before the
// CME and no player breakup before it exists (one after the CME only cuts the prebake from its own step on, and Undo
// my breakups brings the prebake back, and the warning with it).
bool SatelliteSim::explodeWarnNeeded() const
{
    if (harness::active() && !eventWinHarnessOn_)
        return false;
    const double epoch = kesslerEpoch();
    if ((double)simDayJ2000 * kDayS + simSecInDay >= epoch)
        return false;
    for (const kessler::Event &e : kUserEvents)
        if (e.t < epoch)
            return false;
    return true;
}

// ── The banner ─────────────────────────────────────────────────────────────────────────────────────────────────────
bool SatelliteSim::eventWinBanner(UIRenderer &ui, const char *id)
{
    const std::string path = std::string("assets/events/") + id + ".png";
    std::error_code ec;
    if (!std::filesystem::exists(path, ec) || !ctx_ || evBannerFailed_)
        return false;
    int w = 0, h = 0, n = 0;
    stbi_uc *px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px)
    {
        Log::line("event window: could not read " + path);
        return false;
    }
    VulkanContext &ctx = *ctx_;
    if (evBannerImg_ == VK_NULL_HANDLE)
    {
        ctx.createImage(kEvBannerW, kEvBannerH, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        evBannerImg_, evBannerMem_);
        VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vi.image = evBannerImg_;
        vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vi.format = VK_FORMAT_R8G8B8A8_SRGB;
        vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx.device, &vi, nullptr, &evBannerView_);
        VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        si.magFilter = si.minFilter = VK_FILTER_LINEAR;
        si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(ctx.device, &si, nullptr, &evBannerSampler_);
        evBannerUiId_ = ui.registerImage(ctx.device, evBannerView_, evBannerSampler_);
        if (evBannerUiId_ == 0)
        {
            evBannerFailed_ = true;
            Log::line("event window: no free UI image slot for the banner; placeholders only");
            stbi_image_free(px);
            return false;
        }
    }
    // Scaled to 960 x 320 (bilinear; art made at that size is copied as it is).
    std::vector<uint8_t> img((size_t)kEvBannerW * kEvBannerH * 4);
    for (int y = 0; y < kEvBannerH; ++y)
        for (int x = 0; x < kEvBannerW; ++x)
        {
            const float fx = std::clamp(((float)x + 0.5f) * (float)w / (float)kEvBannerW - 0.5f, 0.0f, (float)(w - 1));
            const float fy = std::clamp(((float)y + 0.5f) * (float)h / (float)kEvBannerH - 0.5f, 0.0f, (float)(h - 1));
            const int x0 = (int)fx, y0 = (int)fy, x1 = std::min(x0 + 1, w - 1), y1 = std::min(y0 + 1, h - 1);
            const float ax = fx - (float)x0, ay = fy - (float)y0;
            for (int c = 0; c < 4; ++c)
            {
                const float v00 = px[((size_t)y0 * w + x0) * 4 + c], v10 = px[((size_t)y0 * w + x1) * 4 + c];
                const float v01 = px[((size_t)y1 * w + x0) * 4 + c], v11 = px[((size_t)y1 * w + x1) * 4 + c];
                const float v = (v00 * (1 - ax) + v10 * ax) * (1 - ay) + (v01 * (1 - ax) + v11 * ax) * ay;
                img[((size_t)y * kEvBannerW + x) * 4 + c] = (uint8_t)std::clamp(v + 0.5f, 0.0f, 255.0f);
            }
        }
    stbi_image_free(px);
    const VkDeviceSize bytes = (VkDeviceSize)img.size();
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stage, stageMem);
    void *mapped = nullptr;
    vkMapMemory(ctx.device, stageMem, 0, bytes, 0, &mapped);
    memcpy(mapped, img.data(), (size_t)bytes);
    vkUnmapMemory(ctx.device, stageMem);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, evBannerImg_, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)kEvBannerW, (uint32_t)kEvBannerH, 1};
    vkCmdCopyBufferToImage(cmd, stage, evBannerImg_, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ctx.imageBarrier(cmd, evBannerImg_, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, stage, nullptr);
    vkFreeMemory(ctx.device, stageMem, nullptr);
    evBannerEl_ = UIImage{};
    evBannerEl_.imageId = evBannerUiId_;
    return true;
}

void SatelliteSim::eventWinDestroy(VkDevice device)
{
    if (evMomentsJob_.valid())
        evMomentsJob_.wait();
    if (evBannerSampler_)
        vkDestroySampler(device, evBannerSampler_, nullptr);
    if (evBannerView_)
        vkDestroyImageView(device, evBannerView_, nullptr);
    if (evBannerImg_)
        vkDestroyImage(device, evBannerImg_, nullptr);
    if (evBannerMem_)
        vkFreeMemory(device, evBannerMem_, nullptr);
    evBannerSampler_ = VK_NULL_HANDLE;
    evBannerView_ = VK_NULL_HANDLE;
    evBannerImg_ = VK_NULL_HANDLE;
    evBannerMem_ = VK_NULL_HANDLE;
}

// ── The window ─────────────────────────────────────────────────────────────────────────────────────────────────────
// A dim scrim over the whole screen (it takes the pointer: nothing behind is hovered or clicked), the HUD's black panel
// in the centre with a 2-px frame in the event's colour: the banner (3:1), the date, the title, the text, the buttons.
// Built in buildUI before the hidden-HUD return, so it shows with the HUD hidden too; z 40-41 (over windows, notices,
// hints and the caption band; under tooltips).
void SatelliteSim::buildEventWindow(const UIInput &inp, UIRenderer &ui)
{
    if (!evw_.open)
        return;
    FPROF_SCOPE("buildEventWindow");
    timePaused = true; // (held while it is open, whatever a key did)
    const bool warn = evw_.cur.def == kEvWarn;
    if (!warn)
    {
        timeScaleIdx = 0; // (an event's card holds 1x forward too: a speed-up / reverse key meanwhile does not stick)
        timeDir = 1.0f;
    }
    const EventWinDef *def = warn ? nullptr : &kEventWinDefs[evw_.cur.def];
    const char *id = evIdOf(evw_.cur.def);
    if (evBannerFor_ != id)
    {
        evBannerFor_ = id;
        evBannerHas_ = eventWinBanner(ui, id);
    }
    const float W = (float)inp.screenW, H = (float)inp.screenH;
    const float pad = 14.0f;
    const float winW = std::min(W - 32.0f, std::clamp((float)fs(13) * 46.0f, 380.0f, 860.0f));
    float bw = winW - 2.0f * pad, bh = bw / 3.0f;
    if (bh > H * 0.32f)
    {
        bh = H * 0.32f;
        bw = bh * 3.0f;
    }
    const Clay_Color accent = warn ? Pal::btnDangerArmed : *def->accent;
    const bool alt = !warn && evw_.cur.flag != 0;
    const char *title = warn ? tx(Txt::EvTitleWarn) : tx(alt ? def->titleAlt : def->title);
    const char *text = warn ? tx(Txt::EvTextWarn) : tx(alt ? def->textAlt : def->text);
    ui.addMouseCaptureRect(0.0f, 0.0f, W, H);
    CLAY(CLAY_ID("EventScrim"), {.layout = {.sizing = {CLAY_SIZING_FIXED(W), CLAY_SIZING_FIXED(H)}},
                                 .backgroundColor = Clay_Color{0, 0, 0, 120},
                                 .floating = {.zIndex = 40, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
    }
    Clay_ElementDeclaration d = uiHudPanel({.zIndex = 41,
                                            .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_CENTER, .parent = CLAY_ATTACH_POINT_CENTER_CENTER},
                                            .attachTo = CLAY_ATTACH_TO_ROOT},
                                           8);
    d.layout.sizing.width = CLAY_SIZING_FIXED(winW);
    d.layout.padding = {(uint16_t)pad, (uint16_t)pad, (uint16_t)pad, (uint16_t)pad};
    d.layout.childAlignment = {.x = CLAY_ALIGN_X_CENTER};
    d.backgroundColor = Pal::panelSolid;
    d.border = {.color = accent, .width = CLAY_BORDER_OUTSIDE(2)}; // (ALL also draws between the children)
    CLAY(CLAY_ID("EventWin"), d)
    {
        CLAY(CLAY_ID("EventBanner"), {.layout = {.sizing = {CLAY_SIZING_FIXED(bw), CLAY_SIZING_FIXED(bh)},
                                                 .padding = {12, 12, 12, 12},
                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                      .backgroundColor = Pal::insetBg,
                                      .cornerRadius = CLAY_CORNER_RADIUS(3)})
        {
            if (evBannerHas_)
            {
                CLAY(CLAY_ID("EventBannerImg"), {.layout = {.sizing = {CLAY_SIZING_FIXED(bw - 24.0f), CLAY_SIZING_FIXED(bh - 24.0f)}},
                                                 .custom = {.customData = &evBannerEl_}})
                {
                }
            }
            else
            {
                CLAY_TEXT(evStr(uiKitStr(tx(Txt::FmtEvBannerPlaceholder), id, kEvBannerW, kEvBannerH)),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_WORDS,
                                            .textAlignment = CLAY_TEXT_ALIGN_CENTER}));
            }
        }
        CLAY(CLAY_ID("EventBody"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                               .padding = {4, 4, 4, 0},
                                               .childGap = 6,
                                               .layoutDirection = CLAY_TOP_TO_BOTTOM}})
        {
            if (!warn)
                CLAY_TEXT(evStr(uiKitStr("%s", fmtUtcReadable(evw_.cur.t).c_str())),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11), .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            CLAY_TEXT(evStr(title), CLAY_TEXT_CONFIG({.textColor = warn ? Pal::textWarn : accent, .fontSize = fs(20),
                                                      .wrapMode = CLAY_TEXT_WRAP_WORDS}));
            CLAY_TEXT(evStr(text), CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(13),
                                                     .wrapMode = CLAY_TEXT_WRAP_WORDS}));
        }
        int pick = 0;
        CLAY(CLAY_ID("EventBtns"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                               .padding = {4, 4, 6, 0},
                                               .childGap = 8,
                                               .childAlignment = {.x = CLAY_ALIGN_X_RIGHT},
                                               .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            if (warn)
            {
                if (uiButton(inp, ui, "EventWinBtn", 0, tx(Txt::EvBtnWarnYes), nullptr, false, true, true))
                    pick = 1;
                if (uiButton(inp, ui, "EventWinBtn", 1, tx(Txt::EvBtnWarnNo), nullptr, false, true, true))
                    pick = 2;
                if (uiDangerButton(inp, ui, "EventWinBtn", 2, tx(Txt::EvBtnWarnSun), tx(Txt::TipEvWarnSun), false, true, true))
                    pick = 3;
            }
            else if (uiButton(inp, ui, "EventWinBtn", 0, tx(def->btn), nullptr, true, true, false))
                pick = 1;
        }
        if (pick)
            evw_.lastChoice = -pick; // (applied after the layout: the CLAY block must close first)
    }
    if (evw_.lastChoice < 0)
    {
        const int c = -evw_.lastChoice;
        evw_.lastChoice = 0;
        eventWinChoose(c);
    }
}

// ── Harness ────────────────────────────────────────────────────────────────────────────────────────────────────────
nlohmann::json SatelliteSim::eventWinStateJson() const
{
    nlohmann::json j;
    j["enabled"] = eventWinEnabled;
    j["harness_on"] = eventWinHarnessOn_;
    j["live"] = eventWinLive();
    j["open"] = evw_.open;
    j["id"] = evw_.open ? evIdOf(evw_.cur.def) : "";
    j["moment_utc"] = evw_.open && evw_.cur.def != kEvWarn ? fmtUtcReadable(evw_.cur.t) : "";
    j["how"] = evw_.lastHow;
    j["queued"] = evw_.queue.size();
    j["shown"] = evw_.shown;
    j["log"] = evw_.log;
    j["last_choice"] = evw_.lastChoice;
    j["warn_needed"] = explodeWarnNeeded();
    j["warn_sat"] = evw_.warnSat;
    j["moments_ready"] = evMomentsValid_;
    j["first_collision_utc"] = evw_.firstColT < 1e299 ? fmtUtcReadable(evw_.firstColT) : "";
    j["destroyed_share"] = evw_.destroyedFrac;
    j["time_paused"] = timePaused;
    j["time_scale"] = kTimeLabels[std::clamp(timeScaleIdx, 0, kNumTimeScales - 1)];
    j["time_reverse"] = timeDir < 0.0f;
    // The clock against the moment of the event window open now, else the last one shown (the warning has none).
    const double now = (double)simDayJ2000 * kDayS + simSecInDay;
    j["now_utc"] = fmtUtcReadable(now);
    if (evw_.cur.def != kEvWarn && evw_.cur.def >= 0 && evw_.cur.t > 0.0)
        j["clock_from_moment_s"] = now - evw_.cur.t;
    return j;
}

harness::Status SatelliteSim::harnessEvents(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    const std::string sub = c.pos.empty() ? std::string("state") : c.pos[0];
    if (sub == "on")
        eventWinHarnessOn_ = true;
    else if (sub == "off")
        eventWinHarnessOn_ = false;
    else if (sub == "list")
    {
        nlohmann::json rows = nlohmann::json::array();
        std::vector<EvOcc> occ;
        for (int d = 0; d < evRowCount; ++d)
        {
            const EventWinDef &row = kEventWinDefs[d];
            eventWinMomentsOf(d, occ);
            nlohmann::json r;
            r["id"] = row.id;
            r["title"] = tx(row.title);
            r["trigger"] = row.trig == kTrigMoment ? "moment" : row.trig == kTrigForward ? "moment, forward only" : "share destroyed";
            if (row.trig == kTrigShare)
                r["share"] = row.share;
            nlohmann::json ts = nlohmann::json::array();
            for (const EvOcc &o : occ)
                ts.push_back(fmtUtcReadable(o.t) + (d == evLunar ? (o.flag ? " total" : " partial") : ""));
            r["moments_utc"] = ts;
            r["banner"] = std::string("assets/events/") + row.id + ".png";
            std::error_code ec;
            r["banner_present"] = std::filesystem::exists(std::string("assets/events/") + row.id + ".png", ec);
            rows.push_back(r);
        }
        a.result["events"] = rows;
        if (!evMomentsValid_)
            a.result["note"] = "the moments' worker has not answered yet: `wait 5` and list again";
    }
    else if (sub == "fire")
    {
        const std::string id = c.pos.size() > 1 ? c.pos[1] : std::string();
        if (id == kWarnId || id == "warn")
        {
            evw_.warnSat = selectedSatIndex;
            evw_.warnKind = kessler::EV_EXPLOSION;
            eventWinOpen(EvOcc{kEvWarn, 0.0, 0}, "harness");
        }
        else
        {
            const int d = evRowOf(id.c_str());
            if (d < 0)
            {
                std::string ids;
                for (const EventWinDef &row : kEventWinDefs)
                    ids += std::string(ids.empty() ? "" : "|") + row.id;
                a.error = "events fire <" + ids + "|explode_warning>";
                return harness::Status::Error;
            }
            // Its moment nearest now (a share: now); shown whatever the settings (a layout check).
            std::vector<EvOcc> occ;
            eventWinMomentsOf(d, occ);
            const double now = (double)simDayJ2000 * kDayS + simSecInDay;
            EvOcc o{d, now, c.has("total") ? (int)c.num("total", 1.0) : 0};
            double bestD = 1e300;
            for (const EvOcc &x : occ)
                if (std::abs(x.t - now) < bestD)
                {
                    bestD = std::abs(x.t - now);
                    o = x;
                }
            if (c.has("total"))
                o.flag = (int)c.num("total", 1.0);
            eventWinOpen(o, "harness");
        }
    }
    else if (sub == "ack")
    {
        if (!evw_.open && c.num("soft", 0.0) < 0.5)
        {
            a.error = "events ack: no event window is open (soft=1: not an error)";
            return harness::Status::Error;
        }
        eventWinChoose(std::clamp((int)c.num("choice", 1.0), 1, 3));
    }
    else if (sub != "state")
    {
        a.error = "events list | state | on | off | fire <id> [total=0|1] | ack [choice=1|2|3] [soft=1]";
        return harness::Status::Error;
    }
    a.result["events_state"] = eventWinStateJson();
    char buf[200];
    snprintf(buf, sizeof(buf), "event windows %s%s; %s%s, shown %d", eventWinLive() ? "live" : "not live",
             (harness::active() && !eventWinHarnessOn_) ? " (off: `events on`)" : "", evw_.open ? "open: " : "none open",
             evw_.open ? evIdOf(evw_.cur.def) : "", evw_.shown);
    a.result["message"] = std::string(buf);
    return harness::Status::Done;
}
