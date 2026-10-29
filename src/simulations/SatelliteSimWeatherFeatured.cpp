// SatelliteSimWeatherFeatured.cpp — the Weather window's FEATURED events (2026-10-23).
//
// The user (QA of the weather branch): "The weather tab is hard to follow. Anytime a new storm appears, the window's
// extents change ... Can we have reserved spots, like the twilight one in the Missions tab? We should always have a
// tornado, haboob ready to go to. ... Storms nearby is less important than cool events like tornadoes, high-category
// hurricanes." So the Storms tab opens with one card per kind of event, always in the same order and the same size:
//   Tornado      the next tornado touching down anywhere (in daylight if one comes within the search), up to 24 h ahead
//   Hurricane    the strongest tropical cyclone now, and the next one that becomes a major (Category 3+) one
//   Haboob       the next organised storm over dusty ground by day (the harness `haboob` test), up to 48 h ahead
//   Supercell    the strongest supercell now (mature and in daylight preferred)
//   Squall line  the strongest squall line / MCS now
//   Waterspout   the next waterspout at sea (daylight preferred), up to 24 h ahead
//   Wildfire     the biggest fire burning now
// Tornadoes, waterspouts and haboobs come from a FORWARD search over sim time on a second storm catalog (wxScout, the
// main catalog's configuration): every storm alive at each 30-min step. Asking the main catalog for another time evicts
// the bins the renderer needs this frame (Weather.cpp stormsAt keeps only [t - 9 h, t + 2 h]), so it is never used for
// that. The search runs on a worker thread (wxScanJob) while the window is open, and restarts when sim time goes
// back before it, moves 12 h on, or a found event is over. A storm born after step k cannot put down a tornado before
// step k's time, so the search stops once its daylit candidate touches down before the step it has reached.
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "../UIRenderer.h"
#include "clay.h"
#include "UIPalette.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>

namespace
{
constexpr double kRe = 6371000.0;
constexpr double kStepS = 1800.0;            // the search's step (= the catalog's spawn bin)
constexpr double kTornadoAheadS = 24.0 * 3600.0;
constexpr double kHaboobAheadS = 48.0 * 3600.0;

// The storm icons (tools/make_icons.py, appended after the weather icon: SatelliteSimUI.cpp loadUiIcons). +1 = the
// renderer's icon index convention.
constexpr int kIconStormTornado = 23, kIconStormHurricane = 24, kIconStormHaboob = 25, kIconStormSupercell = 26,
              kIconStormSquall = 27, kIconStormSpout = 28, kIconStormFire = 29, kIconStormCell = 30;

double wallMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Clay_String cs(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
double latOf(const glm::dvec3 &e) { return glm::degrees(std::asin(std::clamp(e.z, -1.0, 1.0))); }
double lonOf(const glm::dvec3 &e) { return glm::degrees(std::atan2(e.y, e.x)); }
int catOfV(float v) { return v < 17.5f ? -1 : v < 33.0f ? 0 : v < 43.0f ? 1 : v < 50.0f ? 2 : v < 58.0f ? 3 : v < 70.0f ? 4 : 5; }
void fmtIn(double s, char *b, size_t n)
{
    s = std::max(s, 0.0);
    const long long m = (long long)std::floor(s / 60.0);
    if (m >= 48 * 60)
        snprintf(b, n, "%lld d %lld h", m / 1440, (m % 1440) / 60);
    else if (m >= 60)
        snprintf(b, n, tx(Txt::FmtHMin), m / 60, m % 60);
    else
        snprintf(b, n, tx(Txt::FmtMin), m);
}
const char *ns(double lat) { return lat >= 0.0 ? "N" : "S"; }
const char *ew(double lon) { return lon >= 0.0 ? "E" : "W"; }
const char *stageName(int s)
{
    static const char *k[4] = {"forming", "growing", "mature", "decaying"};
    return k[std::clamp(s, 0, 3)];
}
} // namespace

double SatelliteSim::weatherSunSin(const glm::dvec3 &e, double t) const
{
    const glm::dvec3 si = sunDirEciAt(t);
    const double g = earthRotationAngle(t);
    const glm::dvec3 se(si.x * std::cos(g) + si.y * std::sin(g), -si.x * std::sin(g) + si.y * std::cos(g), si.z);
    return glm::dot(glm::normalize(se), glm::normalize(e));
}

void SatelliteSim::weatherFeaturedRestart(double now)
{
    if (wxScanJob.valid())
    {
        wxScanCancel = true;     // (the job stops at its next step; its result is dropped)
        wxScanJobStale = true;
    }
    ++wxScanRestarts;
    wxScanBase = now;
    wxScanStep = 0;
    wxScanDone = false;
    wxScanSeen.clear();
    for (int s : {(int)kFeatTornado, (int)kFeatSpout, (int)kFeatHaboob})
    {
        wxFeat[s].found = false;
        wxFeat[s].searching = true;
        wxFeat[s].hasStorm = false;
    }
}

// The harness: the forward search run to its end (waiting on the worker), and a running fire job.
void SatelliteSim::weatherFeaturedFinish(double maxS)
{
    const auto t0 = std::chrono::steady_clock::now();
    weatherFeaturedUpdate();
    while (std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(maxS))
    {
        if (wxScanJob.valid())
            wxScanJob.wait();
        weatherFeaturedUpdate();
        if (wxScanDone && !wxScanJob.valid())
            break;
        if (!wxScanJob.valid() && !wxScanDone && (!stormObjects || !stormCatalog))
            break;
    }
}

// The slots that come from the storms alive now (called by weatherRefreshRows with its rows).
void SatelliteSim::weatherFeaturedNow(const std::vector<WxStormRow> &alive)
{
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    for (int s : {(int)kFeatSupercell, (int)kFeatLine})
    {
        const WxStormRow *best = nullptr;
        float bestScore = -1.0f;
        for (const WxStormRow &r : alive)
        {
            const bool want = s == kFeatSupercell ? r.kind == (int)wx::StormKind::Supercell
                                                  : (r.kind == (int)wx::StormKind::SquallLine || r.kind == (int)wx::StormKind::Mcs);
            if (!want) continue;
            static const float kStageW[4] = {0.4f, 0.8f, 1.0f, 0.5f};
            const bool day = weatherSunSin(r.e, now) > 0.17;
            const float sc = r.score * kStageW[std::clamp(r.stage, 0, 3)] * (day ? 2.0f : 1.0f);
            if (sc > bestScore) { bestScore = sc; best = &r; }
        }
        WxFeatured &F = wxFeat[s];
        F.searching = false;
        F.found = best != nullptr;
        if (best)
        {
            F.row = *best;
            weatherStormMilestones(F.row);
            F.tEvent = now;
            F.tEnd = best->tEnd;
            F.earth = best->e;
            F.day = weatherSunSin(best->e, now) > 0.17;
        }
    }
}

// One 30-min step of the forward search on the scout catalog (tornadoes, waterspouts, haboobs), on state S. Runs on a
// worker thread (weatherFeaturedUpdate's wxScanJob): it reads the scout catalog (only this search uses it), the settings and
// the static maps, and writes S only. (On the main thread, one step was up to ~12 ms: the scout's new bins after a jump,
// the haboobs' storm parts; perf4-cpu 2026-10-24.)
void SatelliteSim::weatherScanStep(WxScanState &S) const
{
    std::vector<const wx::Storm *> v;
    const double tk = S.base + kStepS * S.step;
    {
        FPROF_SCOPE("wxf.stormsAt");
        wxScout->stormsAt(tk, v);
    }
    WxFeatured &Ft = S.ft, &Fs = S.fs, &Fh = S.fh;
    for (const wx::Storm *St : v)
    {
        // Tornadoes and waterspouts: each storm once (its tornado is fixed at its birth).
        if (tk <= S.base + kTornadoAheadS && St->kind != wx::StormKind::SquallLine && St->kind != wx::StormKind::Mcs &&
            S.seen.insert(St->id).second)
        {
            FPROF_SCOPE("wxf.tornado");
            TornadoNow T;
            if (tornadoOf(*St, tk, T) && T.t1 > S.base && T.t0 < S.base + kTornadoAheadS)
            {
                WxFeatured &F = T.spout ? Fs : Ft;
                const double tm = std::max(S.base, T.t0 + 0.35 * (T.t1 - T.t0));
                TornadoNow Tm;
                tornadoOf(*St, tm, Tm);
                const glm::dvec3 te = weatherMapToEarth(Tm.mapDir, tm);
                const float sea = oceanFraction(std::asin(std::clamp(te.z, -1.0, 1.0)), std::atan2(te.y, te.x), 3000.0, 1);
                // A waterspout needs the sea; a tornado is preferred over land (one over a lake reads as a waterspout,
                // and its rain-free base is harder to see over water).
                const bool ok = !T.spout || sea >= 0.5f;
                if (ok)
                {
                    // The rank (the first harness runs stood in a downpour over Lake Victoria, the tornado invisible): in
                    // daylight 8, over land 4 (a waterspout: always), in a drier region 2 (the map's cover over ~100 km
                    // below 0.45: a supercell in a broken field, not inside a rain shield), outside the deep tropics 1;
                    // then the earliest.
                    const bool day = weatherSunSin(te, tm) > 0.17;   // ~10 deg
                    const float moist = wxScoutEnv.moisture ? wxScoutEnv.moisture(glm::normalize(Tm.mapDir)) : 0.5f;
                    const int rank = (day ? 8 : 0) + (T.spout || sea < 0.5f ? 4 : 0) + (moist < 0.45f ? 2 : 0) +
                                     (std::abs(latOf(te)) > 20.0 ? 1 : 0);
                    const bool better = !F.found || rank > F.rank || (rank == F.rank && T.t0 < F.tEvent);
                    if (better)
                    {
                        F.found = true;
                        F.tEvent = T.t0;
                        F.tEnd = T.t1;
                        F.day = day;
                        F.earth = te;
                        F.storm = *St;
                        F.hasStorm = true;
                        F.torn = T;
                        F.spout = T.spout;
                        F.landOk = sea < 0.5f;
                        F.rank = rank;
                    }
                }
            }
        }
        // Haboobs: an organised line system at full strength (its envelope between sysOn + ramp and sysOff - ramp,
        // >= 2 h old, an hour left before 70% of its life) by day (Sun > 10 deg at its centre), whose LEADING EDGE is
        // over dust: of 9 points along the line, dG ahead of it (as haboobFrontOf: 5 + 14 km x the age ramp), at least
        // 6 on the biome map's dust >= 0.3 and their mean >= 0.4. (The centre's dust alone picked storms whose front ran
        // over farmland: no wall was drawn.) Inland (the coast's fog hid it). Ranked by its LIGHT: 2 for the Sun on the
        // side the wall advances toward, clear of the storm's anvil and 18 deg up at least (a Go to stands there, the wall
        // front-lit; against the Sun it is a dark silhouette, under a lower Sun the light crossed tens of km of the
        // storm's cloud and the wall stood in shade); 1 for a Sun 25-60 deg up (its heads cast shadows, the face lit).
        // The earliest of the best; rank 3 ends the search.
        if ((!Fh.found || Fh.rank < 3) && tk <= S.base + kHaboobAheadS && wx::isLineSystem(St->kind) && biomeMap.ready() &&
            tk - St->t0 >= 7200.0 && tk <= St->t0 + St->life * 0.7 - 3600.0)
        {
            const wx::TowerSpec &ts = St->towers[0];
            const double age = tk - St->t0;
            const bool full = age >= ts.sysOn + ts.sysRamp && age <= ts.sysOff - ts.sysRamp;
            const glm::dvec3 c = glm::normalize(St->centreAt(tk));
            const glm::dvec3 ce = weatherMapToEarth(c, tk);
            if (full && weatherSunSin(ce, tk) > 0.17)
            {
                FPROF_SCOPE("wxf.haboob");
                glm::dvec3 v = St->centreAt(tk + 60.0) - c;
                v -= c * glm::dot(v, c);
                double bowSgn = 1.0;   // (the towers' bow is toward the map-frame motion)
                glm::dvec3 mv = glm::length(v) > 1e-15 ? glm::normalize(v) : glm::normalize(glm::dvec3(St->shear));
                // (Ahead of its motion over the ground, as haboobFrontOf: the map frame drifts.)
                if (glm::dot(weatherMapToEarth(glm::normalize(St->centreAt(tk + 60.0)), tk + 60.0) -
                             weatherMapToEarth(glm::normalize(St->centreAt(tk - 60.0)), tk - 60.0),
                             weatherMapToEarth(glm::normalize(c + mv * 1e-3), tk) - ce) < 0.0)
                {
                    mv = -mv;
                    bowSgn = -1.0;
                }
                const glm::dvec3 ea = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), c) + glm::dvec3(1e-12, 0, 0));
                const glm::dvec3 no = glm::cross(c, ea);
                const glm::dvec3 L = ea * std::cos((double)St->lineAz) + no * std::sin((double)St->lineAz);
                const double dG = 8000.0 + 22000.0 * glm::smoothstep(1800.0, 3.0 * 3600.0, age);   // (haboobFrontOf's, a line's)
                int on = 0;
                double sum = 0.0;
                for (int q = -4; q <= 4; ++q)
                {
                    const double x = (double)q / 4.0;
                    const double fwd = dG + bowSgn * (double)St->bow * (1.0 - x * x);
                    const glm::dvec3 p = glm::normalize(c + (L * (x * (double)St->lineHalfM) + mv * fwd) / kRe);
                    const glm::dvec3 pe = weatherMapToEarth(p, tk);
                    const double d = biomeMap.dustAt(latOf(pe), lonOf(pe));
                    sum += d;
                    on += d >= 0.3 ? 1 : 0;
                }
                const glm::dvec3 me = weatherMapToEarth(glm::normalize(c + mv * 1e-3), tk) - ce;
                const glm::dvec3 si = sunDirEciAt(tk);
                const double gr = earthRotationAngle(tk);
                const glm::dvec3 se(si.x * std::cos(gr) + si.y * std::sin(gr), -si.x * std::sin(gr) + si.y * std::cos(gr), si.z);
                const glm::dvec3 sh = se - ce * glm::dot(se, ce);
                const double sunEl = std::asin(std::clamp(weatherSunSin(ce, tk), -1.0, 1.0)) * (180.0 / 3.14159265358979);
                // (the Sun must also reach the wall past the anvil: the middle of the leading edge)
                double lit = 0.0;
                if (glm::dot(sh, me) > 0.0)
                {
                    std::vector<wx::Tower> tw;
                    std::vector<wx::Shield> shl;
                    haboobStormParts(*wxScout, *St, tk, tw, shl);
                    const glm::dvec3 pm = weatherMapToEarth(glm::normalize(c + mv * ((dG + bowSgn * (double)St->bow) / kRe)), tk);
                    lit = haboobSunOnFace(pm, tk, tw, shl);
                }
                const int rank = (lit > 0.5 && sunEl >= 18.0 ? 2 : 0) + (sunEl >= 25.0 && sunEl <= 60.0 ? 1 : 0);
                // Inland: within ~150 km of the sea the coast's fog and haze (cv2FogDust) hid the wall from a Go to.
                const bool inland = oceanFraction(std::asin(std::clamp(ce.z, -1.0, 1.0)), std::atan2(ce.y, ce.x), 150000.0, 2) < 0.05f;
                // ... and the front the Watch will stand in front of raises a wall then (haboobStrongestPoint, the same
                // test, on the scout's towers): the leading edge's dust alone accepted lines whose front was gapped or
                // whose towers were too weak at that moment, and the Watch found nothing (2026-10-27).
                const double tEv = std::max(tk, S.now);
                HaboobPoint hq;
                double hk = 0.0;
                if (on >= 6 && sum / 9.0 >= 0.4 && inland && (!Fh.found || rank > Fh.rank) &&
                    haboobStrongestPoint(*St, tEv, hq, hk, wxScout.get()))
                {
                    Fh.found = true;
                    Fh.rank = rank;
                    Fh.searching = rank < 3;
                    Fh.tEvent = tEv;
                    Fh.tEnd = std::min(St->t0 + St->life * 0.7, St->t0 + (double)ts.sysOff - (double)ts.sysRamp);
                    Fh.day = true;
                    Fh.earth = ce;
                    Fh.storm = *St;
                    Fh.hasStorm = true;
                }
            }
        }
    }
    // Resolved: a daylit candidate touching down before this step (no storm born later can beat it), or the horizon.
    auto resolved = [&](WxFeatured &F) {
        const bool r = (F.found && F.rank >= 15 && F.tEvent <= tk) || tk >= S.base + kTornadoAheadS;
        if (r) F.searching = false;
        return r;
    };
    const bool rt = resolved(Ft), rs = resolved(Fs);
    if (tk >= S.base + kHaboobAheadS)
        Fh.searching = false;
    const bool rh = !Fh.searching;
    ++S.step;
    if (rt && rs && rh)
        S.done = true;
}

void SatelliteSim::weatherFeaturedUpdate()
{
    FPROF_SCOPE("weatherFeatured");
    if (!stormObjects || !stormCatalog || stormEnvKey == 0)
        return;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (!wxScout)
    {
        wxScout = std::make_unique<wx::Catalog>();
        wxScout->setBuildThreads(std::clamp((int)std::thread::hardware_concurrency() / 3, 1, 6));
    }
    if (wxScoutKey != stormEnvKey)
    {
        // (the scout is the running job's: reconfigure it once that is in)
        if (wxScanJob.valid())
        {
            wxScanCancel = true;
            wxScanJob.wait();
            wxScanJob.get();
            wxScanJobStale = false;
        }
        wxScout->configure(wxScoutEnv, wxScoutSettings, stormClim.get(), stormEnvKey);
        wxScoutKey = stormEnvKey;
        weatherFeaturedRestart(now);
    }
    bool restart = now < wxScanBase - 1.0 || now > wxScanBase + 12.0 * 3600.0;
    for (int s : {(int)kFeatTornado, (int)kFeatSpout, (int)kFeatHaboob})
        if (wxFeat[s].found && now > wxFeat[s].tEnd)
            restart = true;
    if (restart)
        weatherFeaturedRestart(now);

    // ── The forward search (tornadoes, waterspouts, haboobs): on a worker (wxScanJob), its results taken here ──
    if (wxScanJob.valid() && wxScanJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        WxScanState r = wxScanJob.get();
        if (!wxScanJobStale && r.base == wxScanBase)
        {
            wxScanStep = r.step;
            wxScanDone = r.done;
            wxScanSeen = std::move(r.seen);
            wxFeat[kFeatTornado] = r.ft;
            wxFeat[kFeatSpout] = r.fs;
            wxFeat[kFeatHaboob] = r.fh;
        }
        wxScanJobStale = false;
    }
    if (!wxScanJob.valid() && !wxScanDone)
    {
        WxScanState st;
        st.base = wxScanBase;
        st.now = now;
        st.step = wxScanStep;
        st.done = false;
        st.seen = wxScanSeen;
        st.ft = wxFeat[kFeatTornado];
        st.fs = wxFeat[kFeatSpout];
        st.fh = wxFeat[kFeatHaboob];
        wxScanCancel = false;
        wxScanJob = std::async(std::launch::async, [this, st = std::move(st)]() mutable {
            // Until done, cancelled (a restart), or ~0.4 s (the cards show the progress as it goes).
            const double t0 = wallMs();
            while (!st.done && !wxScanCancel.load(std::memory_order_relaxed) && wallMs() - t0 < 400.0)
                weatherScanStep(st);
            return st;
        });
    }

    // ── The hurricane slot (every 2 s of wall time, or after an hour of sim time) ──
    const double wall = wallMs() / 1000.0;
    if (wall - wxFeatNowWall > 2.0 || std::abs(now - wxFeatNowSim) > 3600.0)
    {
        FPROF_SCOPE("wxf.cyclones");
        wxFeatNowWall = wall;
        wxFeatNowSim = now;
        WxFeatured &F = wxFeat[kFeatHurricane];
        F.searching = false;
        const wx::CycloneState *best = nullptr;
        for (const wx::CycloneState &S : stormCyclonesNow)
            if (S.c && S.category >= 0 && (!best || S.vmax > best->vmax))
                best = &S;
        F.hasCyc = best != nullptr;
        if (best)
        {
            F.cyc = *best->c;
            F.earth = best->earth;
            F.tEvent = now;
            F.tEnd = best->c->t0 + best->c->life;
            F.day = weatherSunSin(best->earth, now) > 0.0;
        }
        // The next storm to become a major one (its peak still ahead, Category 3+), in any basin, this year or the next.
        if (!F.hasCycNext || F.cycNext.tPeak < now || (F.hasCyc && F.cycNext.id == F.cyc.id && F.cyc.vPeak >= 50.0f && F.cyc.tPeak < now))
        {
            F.hasCycNext = false;
            const int y = (int)std::floor(now / (365.2422 * 86400.0) + 2000.0);
            std::vector<const wx::Cyclone *> cv;
            const wx::Cyclone *nx = nullptr;
            for (int b = 0; b < wx::Catalog::kCycloneBasins; ++b)
                for (int yy = y; yy <= y + 1; ++yy)
                {
                    stormCatalog->cycloneSeason(b, yy, cv);
                    for (const wx::Cyclone *c : cv)
                        if (c->vPeak >= 50.0f && c->tPeak > now && (!F.hasCyc || c->id != F.cyc.id) && (!nx || c->tPeak < nx->tPeak))
                            nx = c;
                }
            if (nx)
            {
                F.cycNext = *nx;
                F.hasCycNext = true;
            }
        }
        F.found = F.hasCyc || F.hasCycNext;
    }

    // ── The wildfire slot (the world's fires on a worker: firesWorld scans every cell over a month of days) ──
    if (wxFireJob.valid() && wxFireJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        std::vector<wx::Fire> all = wxFireJob.get();
        WxFeatured &F = wxFeat[kFeatFire];
        F.searching = false;
        F.found = false;
        double bestS = -1.0;
        for (const wx::Fire &f : all)
        {
            const wx::FireState fst = wx::fireStateAt(f, wxFireJobT);
            if (fst.activity <= 0.05) continue;
            // (only fires whose front still burns: one run into a town or onto bare ground has no smoke or flames)
            const double share = (double)f.size * (0.3 + fst.activity) > bestS ? wx::fireBurningShare(f, fst, wxFireJobT, biomeMap) : 0.0;
            if (share < 0.4) continue;
            const double s = (double)f.size * (0.3 + fst.activity) * (1.0 + (double)f.pyro) * share;
            if (s > bestS)
            {
                bestS = s;
                F.found = true;
                F.fire = f;
                F.fireState = fst;
            }
        }
        if (F.found)
        {
            const double la = glm::radians(F.fire.latDeg), lo = glm::radians(F.fire.lonDeg);
            F.earth = glm::dvec3(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
            F.tEvent = wxFireJobT;
            F.tEnd = F.fire.t0 + F.fire.life;
            F.day = weatherSunSin(F.earth, wxFireJobT) > 0.0;
        }
    }
    if (!wxFireJob.valid() && biomeMap.ready() && wildfireRate > 0.0f &&
        (std::abs(now - wxFireSim) > 3.0 * 3600.0 || (wxFeat[kFeatFire].found && now > wxFeat[kFeatFire].tEnd)))
    {
        wxFireSim = now;
        wxFireJobT = now;
        const float rate = wildfireRate;
        const wx::BiomeMap *bio = &biomeMap;
        wxFireJob = std::async(std::launch::async, [now, rate, bio]() {
            std::vector<wx::Fire> out;
            wx::firesWorld(now, *bio, rate, out);
            return out;
        });
    }
    if (wildfireRate <= 0.0f)
    {
        wxFeat[kFeatFire].found = false;
        wxFeat[kFeatFire].searching = false;
    }
}

// ── Actions (after the layout) ───────────────────────────────────────────────────────────────────────────────────
// act: 0 / 1 the card's first / second button, 2 / 3 the second row's.
bool SatelliteSim::weatherFeaturedAct(int slot, int act, std::string &msg)
{
    FPROF_SCOPE("weatherFeaturedAct");
    if (slot < 0 || slot >= kFeatCount || !wxFeat[slot].found)
    {
        msg = tx(Txt::ValNothingThereYet);
        return false;
    }
    const WxFeatured F = wxFeat[slot];   // (a copy: the jump below refreshes the slots)
    auto setT = [&](double t) {
        const double d = std::floor(t / 86400.0);
        simDayJ2000 = (int64_t)d;
        simSecInDay = t - d * 86400.0;
        timePaused = false;
        timeScaleIdx = 0;
        timeDir = 1.0f;
    };
    auto placeLooking = [&](const glm::dvec3 &oe, double altAglM, const glm::dvec3 &target, float el) {
        if (followActive) stopFollow();
        stopTrack();
        setObserverLatLon(latOf(oe), lonOf(oe));
        obsHeightOffset = std::max(0.0f, cpuTerrainHeightM(obsLatDeg, obsLonDeg)) + (float)altAglM;
        const glm::dvec3 oEast = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), oe) + glm::dvec3(1e-12, 0, 0)), oNorth = glm::cross(oe, oEast);
        const glm::dvec3 to3 = target - oe * glm::dot(target, oe);
        camera.fovYDeg = std::max(camera.fovYDeg, 60.0f);
        aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(to3, oEast), glm::dot(to3, oNorth))), el);
        trailClearPending = true;
        skyTaaHistValid = false;
        cv2HistoryValid = false;
        for (double &bb : cv2WxBakedT) bb = -1e30;
    };
    switch (slot)
    {
    case kFeatTornado:
    case kFeatSpout:
        if (!F.hasStorm) break;
        return weatherWatchTornadoOf(F.storm, act == 1 ? 1 : 0, msg);
    case kFeatHaboob:
    {
        if (!F.hasStorm) break;
        // In front of the wall itself: the strongest stretch of the storm's gust front over dust at that moment (its towers
        // built for it, haboobStrongestPoint), standing D ahead of it and looking back at it (act 0, Watch: 6 km on the
        // ground, the wall filling the view's lower third and reaching you in ~6 min; act 1 (harness): 9 km ahead, 2 km
        // up (2.5 km until the walls were lowered to 0.5-1.8 km, 2026-10-26: that was inside the cloud above them), over its heads: from 3-4 km up the storm's own cumulus often stood in the way, from 1.2 km the regional dust haze). The first cut stood a fixed distance from the storm's
        // CENTRE, measured to an estimate of the front (bow + the outflow's lead): its real front, built from the towers
        // standing then, was 40-80 km away and a haze on the horizon, and the aloft view showed none.
        const double t = std::max(F.tEvent, (double)simDayJ2000 * 86400.0 + simSecInDay);
        setT(t);
        const wx::Storm &S = F.storm;
        const bool aloft = act == 1;
        HaboobPoint q;
        double k = 0.0;
        // (The search found a wall at tEvent; a moment later its regenerating towers may be weaker: the first moment from
        // t on, in 2-min steps up to an hour or the event's end, where the front raises one.)
        double tW = t;
        bool okW = haboobStrongestPoint(S, tW, q, k);
        for (int i = 1; !okW && i <= 30 && t + 120.0 * i <= std::max(F.tEnd, t); ++i)
        {
            tW = t + 120.0 * i;
            okW = haboobStrongestPoint(S, tW, q, k);
        }
        if (!okW)
        {
            msg = tx(Txt::ValItsGustFrontRaisesNoDust);
            return false;
        }
        if (tW != t)
            setT(tW);
        const double D = aloft ? 9000.0 : 6000.0;
        const glm::dvec3 oe = glm::normalize(q.dir + q.out * (D / kRe));
        placeLooking(oe, aloft ? 2000.0 : 2.0, q.dir, aloft ? -6.0f : 6.0f);
        camera.fovYDeg = 70.0f;
        updatePositions(tW);
        msg = std::string(tx(Txt::TxtAHaboobsDustWallOver)) + wx::biomeName(biomeMap.at(latOf(q.dir), lonOf(q.dir))) +
              (aloft ? tx(Txt::TxtKmAwaySeenFrom2) : tx(Txt::TxtKmAwayAndComingToward));
        return true;
    }
    case kFeatSupercell:
    case kFeatLine:
        if (act == 0) return weatherWatchStormNow(F.row.id, msg);
        if (act == 1) return weatherGoToStorm(F.row.id, msg);
        if (act == 2 && F.row.tTorn0 > 0.0) return weatherWatchTornado(F.row.id, msg);
        if (act == 2 && F.row.tPass > 0.0) return weatherWatchPassage(F.row.id, msg);
        break;
    case kFeatHurricane:
    {
        // act 0 (Watch) the strongest active one from orbit, the whole storm in frame; harness: 1 the eye, 2 its next
        // landfall, 3 the next major one at its peak.
        if (act <= 2 && F.hasCyc)
        {
            wxCycFeatured = {F.cyc};
            if (F.hasCycNext) wxCycFeatured.push_back(F.cycNext);
            if (act == 0)
                return weatherWatchCyclone(F.cyc.id, msg);   // (now from orbit; forward to its daylight if it is night there)
            return weatherGoToCyclone(F.cyc.id, act == 1 ? 2 : 3, msg);
        }
        if (act == 3 && F.hasCycNext)
        {
            wxCycFeatured = {F.cycNext};
            setT(F.cycNext.tPeak);
            std::string m;
            const bool ok = weatherGoToCyclone(F.cycNext.id, 1, m);
            msg = tx(Txt::ValAheadToItsPeak) + m;
            return ok;
        }
        break;
    }
    case kFeatFire:
    {
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const wx::FireState fs = wx::fireStateAt(F.fire, t);
        const double la = glm::radians(F.fire.latDeg), lo = glm::radians(F.fire.lonDeg);
        const glm::dvec3 G(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), G) + glm::dvec3(1e-12, 0, 0)), north = glm::cross(G, east);
        const glm::dvec2 head = wx::fireEdgeAt(fs, 0.0);
        const glm::dvec3 Hd = glm::normalize(G + (east * head.x + north * head.y) / kRe);
        // (the smoke's direction: the wind at ~0.85 of its top, as the harness `fires go`, so the column is seen side on)
        const glm::dvec2 wSm = wx::windAt(G, 0.85 * fs.plumeTopM, t);
        const glm::dvec3 wv = glm::length(wSm) > 0.1 ? glm::normalize(east * wSm.x + north * wSm.y)
                                                      : east * std::sin(fs.windAz) + north * std::cos(fs.windAz);
        const glm::dvec3 cv = glm::cross(G, wv);
        const bool orbit = act == 1;
        // As the harness `fires go` (side / orbit) at the time it is now: beside the column, or 300 km off it from 400 km.
        // Beside the column 10 km across the smoke's wind and 4 km upwind of the head, 300 m up (30 km off at 700 m the
        // front and column were a speck behind the haze and the fire often hid behind terrain), on the side where the Sun
        // stands behind the viewer by day (the smoke front-lit).
        double sgn = 1.0;
        {
            const glm::dvec3 si = sunDirEciAt(t);
            const double gr = earthRotationAngle(t);
            const glm::dvec3 se(si.x * std::cos(gr) + si.y * std::sin(gr), -si.x * std::sin(gr) + si.y * std::cos(gr), si.z);
            if (glm::dot(se, G) > 0.0 && glm::dot(se, cv) > 0.0)
                sgn = -1.0;   // the eye at -cv looks along +cv, toward the Sun: stand on the other side
        }
        // (Beside the COLUMN, not the head: the smoke rises from the active front's heat-weighted sections, which on a
        // big fire stand km from the head; looking at the head, the Watch framed a front with its smoke out of view,
        // 2026-10-27. The anchor is the smoke ~10 min old, where the column leans over; looked at a few degrees up.)
        const glm::dvec3 sm = wx::fireSmokeAt(F.fire, fs, t, 600.0, wx::fireInjectionHeightM(F.fire, fs, t));
        const glm::dvec3 Sd = glm::normalize(G + (east * sm.x + north * sm.y) / kRe);
        const double dSide = 5000.0;
        const glm::dvec3 oe = orbit ? glm::normalize(Hd - cv * (300000.0 / kRe))
                                    : glm::normalize(Sd - cv * (sgn * dSide / kRe) - wv * (1500.0 / kRe));
        const float elW = (float)std::clamp(0.5 * glm::degrees(std::atan2(std::max(sm.z, 300.0), dSide)), 3.0, 12.0);
        placeLooking(oe, orbit ? 400000.0 : 150.0, orbit ? Hd : Sd, orbit ? -45.0f : elW);   // (Watch: 5 km beside the column)
        fireCacheRate = -1.0f;   // rescan the fires round the new eye
        updatePositions(t);
        msg = std::string(tx(Txt::TxtAWildfireIn)) + wx::biomeName(biomeMap.at(F.fire.latDeg, F.fire.lonDeg)) +
              (orbit ? tx(Txt::TxtFromOrbit) : tx(Txt::TxtKmBesideItsSmokeColumn));
        return true;
    }
    default: break;
    }
    msg = tx(Txt::ValNotAvailable);
    return false;
}

// ── The cards ────────────────────────────────────────────────────────────────────────────────────────────────────
// Every card has the same height whatever it holds: an icon, three single lines (never wrapped, clipped at the card's
// edge) and ONE button, Watch (disabled when there is nothing to see). Nothing appears or disappears as storms come and
// go. (2026-10-24: there were up to four — Go to / Watch / Mature / Aloft / Eye / Orbit / Landfall / Next major /
// Passage — and playtesters could not tell them apart; Watch always lands on the event happening, in frame.)
void SatelliteSim::buildWeatherFeatured(const UIInput &inp, UIRenderer &ui)
{
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 oe = glm::normalize(glm::dvec3(obsDir));
    auto text = [&](const char *s, Clay_Color c, int size) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size), .wrapMode = CLAY_TEXT_WRAP_NONE}));
    };
    auto distKm = [&](const glm::dvec3 &e) { return std::acos(std::clamp(glm::dot(glm::normalize(e), oe), -1.0, 1.0)) * kRe / 1000.0; };
    auto where = [&](const glm::dvec3 &e) {
        const double la = latOf(e), lo = lonOf(e);
        return wxStr(tx(Txt::FmtKmAway2), std::abs(la), ns(la), std::abs(lo), ew(lo), distKm(e));
    };
    struct Btn { const char *tip; bool enabled; };
    auto card = [&](int slot, int icon, const char *title, Clay_Color accent, const char *status, const char *l1, const char *l2,
                    const Btn &b) {
        const float lineH = (float)fs(12) * 1.35f;
        CLAY(CLAY_IDI("WxFCard", (uint32_t)slot), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(std::max(3.0f * lineH + 18.0f, 64.0f))},
                                                              .padding = {8, 8, 6, 6},
                                                              .childGap = 8,
                                                              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                              .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                   .backgroundColor = Pal::cardBg,
                                                   .cornerRadius = CLAY_CORNER_RADIUS(4),
                                                   .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
        {
            CLAY(CLAY_IDI("WxFIconBg", (uint32_t)slot), {.layout = {.sizing = {CLAY_SIZING_FIXED(40), CLAY_SIZING_FIXED(40)},
                                                                    .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                                         .backgroundColor = accent,
                                                         .cornerRadius = CLAY_CORNER_RADIUS(6)})
            {
                CLAY(CLAY_IDI("WxFIcon", (uint32_t)slot), {.layout = {.sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(30)}},
                                                           .image = {.imageData = (void *)(intptr_t)(icon + 1)}}) {}
            }
            CLAY(CLAY_IDI("WxFText", (uint32_t)slot), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                                  .childGap = 2,
                                                                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                  .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                                       .clip = {.horizontal = true}})
            {
                CLAY(CLAY_IDI("WxFHead", (uint32_t)slot), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                      .childGap = 6,
                                                                      .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                      .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    text(title, Pal::textBright, 12);
                    text(status, Pal::textPrimary, 11);
                }
                text(l1, Pal::textDim, 11);
                text(l2, Pal::textDim, 11);
            }
            // (A fixed width: the text column is the same width on every card.)
            const float bw = (float)fs(11) * 6.0f;
            CLAY(CLAY_IDI("WxFBtns", (uint32_t)slot), {.layout = {.sizing = {CLAY_SIZING_FIXED(bw), CLAY_SIZING_FIT(0)},
                                                                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                  .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                if (uiButton(inp, ui, "WxFBtn", slot, tx(Txt::BtnWatch), b.tip, false, b.enabled, true) && b.enabled)
                {
                    wxFeatPendingSlot = slot;
                    wxFeatPendingAct = 0;
                }
            }
        }
    };
    char w[48], w2[48];
    const char *searching = tx(Txt::ValSearching);
    // ── Tornado / waterspout ──
    for (int slot : {(int)kFeatTornado, (int)kFeatSpout})
    {
        const WxFeatured &F = wxFeat[slot];
        const bool spout = slot == kFeatSpout;
        const char *title = spout ? tx(Txt::ValWaterspout) : tx(Txt::ValTornado);
        const char *st, *l1, *l2;
        if (!F.found)
        {
            st = F.searching ? searching : tx(Txt::ValNoneInTheNextDay);
            l1 = F.searching ? wxStr(tx(Txt::FmtLookingHAhead), std::max(0.0, (wxScanBase + kStepS * wxScanStep - now) / 3600.0)) : "";
            l2 = "";
        }
        else
        {
            const bool on = now >= F.tEvent && now < F.tEnd;
            fmtIn(F.tEvent - now, w, sizeof(w));
            fmtIn(F.tEnd - now, w2, sizeof(w2));
            st = on ? wxStr(tx(Txt::FmtOnTheGroundNowMore), w2) : wxStr(tx(Txt::FmtTouchesDownIn), w);
            l1 = where(F.earth);
            l2 = wxStr(tx(Txt::FmtMinMWide), F.day ? tx(Txt::TxtByDay) : tx(Txt::TxtAtNight), (F.tEnd - F.tEvent) / 60.0, 2.0 * F.torn.r0);
        }
        const Btn b = {spout ? tx(Txt::TxtTheWaterspoutOnTheWaterA) : tx(Txt::TxtTheTornadoOnTheGroundA), F.found};
        card(slot, spout ? kIconStormSpout : kIconStormTornado, title, spout ? Pal::chipIdle : Pal::btnDanger, st, l1, l2, b);
        if (!spout)
        {
            // ── Hurricane, then haboob, supercell, line (the cards' order: the coolest first) ──
            const WxFeatured &H = wxFeat[kFeatHurricane];
            const char *hs, *h1, *h2;
            if (H.hasCyc)
            {
                const wx::CycloneState *cs0 = nullptr;
                for (const wx::CycloneState &S : stormCyclonesNow)
                    if (S.c && S.c->id == H.cyc.id) cs0 = &S;
                const float vm = cs0 ? cs0->vmax : H.cyc.vmaxAt(now);
                const int cat = catOfV(vm);
                // Short: the card's lines are never wrapped, and at 1024 px the text column is ~300 px wide.
                hs = cat >= 1 ? wxStr(tx(Txt::FmtCategory), H.cyc.name.empty() ? "unnamed" : H.cyc.name.c_str(), std::min(cat, 5))
                              : wxStr("%s, %s", H.cyc.name.empty() ? "unnamed" : H.cyc.name.c_str(), cat == 0 ? tx(Txt::TxtTropicalStorm2) : "depression");
                h1 = wxStr(tx(Txt::FmtMSKt), vm, vm * 1.944f, cs0 && cs0->overLand ? tx(Txt::TxtOverLand2) : "");
                h2 = where(cs0 ? cs0->earth : H.cyc.earthAt(now));
            }
            else
            {
                hs = tx(Txt::ValNoneNow);
                h1 = h2 = "";
                if (H.hasCycNext)
                {
                    fmtIn(H.cycNext.tPeak - now, w, sizeof(w));
                    h1 = wxStr(tx(Txt::FmtNextMajorCategoryIn), H.cycNext.name.empty() ? "unnamed" : H.cycNext.name.c_str(),
                               std::min(catOfV(H.cycNext.vPeak), 5), w);
                    h2 = wx::cycloneBasinName(H.cycNext.basin);
                }
            }
            bool landfall = false;
            if (H.hasCyc)
            {
                const double k = (now - H.cyc.t0) / wx::Cyclone::kStepS;
                for (size_t i = 1; i < H.cyc.overLand.size(); ++i)
                    if (H.cyc.overLand[i] && !H.cyc.overLand[i - 1] && (double)i >= k) landfall = true;
            }
            (void)landfall;
            const Btn hb = {tx(Txt::LblTheWholeStormFromOrbitIts), H.hasCyc};
            card(kFeatHurricane, kIconStormHurricane, tx(Txt::LblHurricane), Pal::progressFill, hs, h1, h2, hb);

            const WxFeatured &B = wxFeat[kFeatHaboob];
            const char *bs, *b1, *b2;
            if (!B.found)
            {
                bs = B.searching ? searching : tx(Txt::ValNoneInTheNext2Days);
                b1 = B.searching ? wxStr(tx(Txt::FmtLookingHAhead), std::max(0.0, (wxScanBase + kStepS * wxScanStep - now) / 3600.0)) : "";
                b2 = "";
            }
            else
            {
                fmtIn(B.tEvent - now, w, sizeof(w));
                bs = B.tEvent <= now + 60.0 ? tx(Txt::ValADustWallNow) : wxStr(tx(Txt::FmtIn), w);
                b1 = where(B.earth);
                b2 = wxStr(tx(Txt::FmtGustFrontOver), B.storm.kind == wx::StormKind::Mcs ? tx(Txt::TxtStormComplex2) : tx(Txt::ValSquallLine),
                           wx::biomeName(biomeMap.at(latOf(B.earth), lonOf(B.earth))));
            }
            const Btn bb = {tx(Txt::LblTheDustWallRollingTowardYou), B.found};
            card(kFeatHaboob, kIconStormHaboob, tx(Txt::LblHaboob), Pal::tagTwilight, bs, b1, b2, bb);

            for (int s : {(int)kFeatSupercell, (int)kFeatLine})
            {
                const WxFeatured &S = wxFeat[s];
                const bool sup = s == kFeatSupercell;
                const char *ss, *s1, *s2;
                if (!S.found)
                {
                    ss = tx(Txt::ValNoneNow);
                    s1 = s2 = "";
                }
                else
                {
                    const WxStormRow &r = S.row;
                    fmtIn(r.tEnd - now, w, sizeof(w));
                    ss = wxStr("%s, %s", r.kind == (int)wx::StormKind::Mcs ? tx(Txt::TxtStormComplex) : r.kind == (int)wx::StormKind::SquallLine ? tx(Txt::TxtSquallLine)
                                                                                                                              : "supercell",
                               stageName(r.stage));
                    s1 = where(r.e);
                    if (r.tTorn0 > now)
                    {
                        fmtIn(r.tTorn0 - now, w2, sizeof(w2));
                        s2 = wxStr(tx(Txt::FmtFlashesMinTornadoIn), S.day ? tx(Txt::TxtByDay) : tx(Txt::TxtAtNight), r.flashesPerMin, w2);
                    }
                    else
                        s2 = wxStr(tx(Txt::FmtFlashesMinLeft), S.day ? tx(Txt::TxtByDay) : tx(Txt::TxtAtNight), r.flashesPerMin, w);
                }
                const Btn sb = {sup ? tx(Txt::TxtTheSupercellAtItsHeightFrom) : tx(Txt::TxtTheLineOfStormsAtIts), S.found};
                card(s, sup ? kIconStormSupercell : kIconStormSquall, sup ? tx(Txt::LblSupercell) : tx(Txt::LblSquallLine), Pal::btnAccent, ss, s1, s2, sb);
            }
        }
    }
    // ── Wildfire ──
    {
        const WxFeatured &F = wxFeat[kFeatFire];
        const char *fs0, *f1, *f2;
        if (!F.found)
        {
            fs0 = wildfireRate <= 0.0f ? tx(Txt::ValWildfiresAreOff) : F.searching || wxFireJob.valid() ? tx(Txt::ValLooking) : tx(Txt::ValNoneBurning);
            f1 = f2 = "";
        }
        else
        {
            const wx::FireState s = wx::fireStateAt(F.fire, now);
            fmtIn(F.fire.t0 + F.fire.life - now, w, sizeof(w));
            fs0 = wxStr(tx(Txt::FmtLeft), s.activity > 0.5 ? tx(Txt::TxtBurningHard) : "smouldering", F.fire.pyro > 0.5f ? tx(Txt::TxtFireStorm) : "", w);
            f1 = where(F.earth);
            f2 = wxStr(tx(Txt::FmtFrontKmSmokeToKm), wx::biomeName(biomeMap.at(F.fire.latDeg, F.fire.lonDeg)),
                       (s.headM + s.backM) / 1000.0, s.plumeTopM / 1000.0);
        }
        const Btn fb = {tx(Txt::LblTheFireAndItsSmokeFrom), F.found};
        card(kFeatFire, kIconStormFire, tx(Txt::LblWildfire), Pal::btnDanger, fs0, f1, f2, fb);
    }
}

nlohmann::json SatelliteSim::weatherFeaturedJson()
{
    static const char *kNames[kFeatCount] = {"tornado", "hurricane", "haboob", "supercell", "line", "waterspout", "wildfire"};
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    nlohmann::json j = nlohmann::json::object();
    for (int s = 0; s < kFeatCount; ++s)
    {
        const WxFeatured &F = wxFeat[s];
        nlohmann::json o = {{"found", F.found}, {"searching", F.searching}};
        if (F.found)
        {
            o["in_min"] = (F.tEvent - now) / 60.0;
            o["ends_in_min"] = (F.tEnd - now) / 60.0;
            o["day"] = F.day;
            o["lat"] = latOf(F.earth);
            o["lon"] = lonOf(F.earth);
            if (F.hasStorm) o["storm_id"] = std::to_string(F.storm.id);
            if (s == kFeatHurricane)
            {
                if (F.hasCyc) o["active"] = F.cyc.name;
                if (F.hasCycNext)
                {
                    o["next_major"] = F.cycNext.name;
                    o["next_major_peak_in_h"] = (F.cycNext.tPeak - now) / 3600.0;
                }
            }
            if (s == kFeatSupercell || s == kFeatLine) o["storm_id"] = std::to_string(F.row.id);
        }
        j[kNames[s]] = o;
    }
    j["scan_step"] = wxScanStep;
    j["scan_done"] = wxScanDone;
    j["scan_restarts"] = wxScanRestarts;
    j["scan_base_min_ago"] = (now - wxScanBase) / 60.0;
    j["scan_ahead_h"] = (wxScanBase + kStepS * wxScanStep - now) / 3600.0;
    return j;
}
