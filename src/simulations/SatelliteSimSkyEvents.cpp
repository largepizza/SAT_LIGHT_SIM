// SatelliteSimSkyEvents.cpp — the Sky events window (2026-10-27; CLAUDE.md "Sky events window").
//
// Celestial events, built as the shell later features plug into (the Moon-base solar-array glints, the scripted
// Betelgeuse supernova of 2037-12-25, meteor showers): every kind is one row of kSkyKinds below — an id, a card title,
// an icon, a SEARCH (a pure function of a time range, run on a worker) and a WATCH (lands on the event already
// happening, at 1x, unpaused, in frame, with a toast saying what you see) and the card's two text lines. The window,
// the worker, the upcoming list and the harness `skyevents` only go through that table.
//
// Kinds so far:
//   Solar eclipse   findEclipse (SatelliteSim.cpp) from the search's start, one after another; watched from where the
//                   shadow's axis meets the Earth at that moment (the harness `eclipse solar` placement)
//   Lunar eclipse   findEclipse (umbral eclipses); watched from a dark site with the Moon ~40 deg up
//   Conjunction     two planets within 2.5 deg, or the Moon and a bright planet within 1.5 deg (geocentric, at the
//                   closest approach), both >= 12 deg from the Sun; ranked by the fainter body's light / separation;
//                   watched from a site where both are >= 10 deg up and the Sun is >= 6 deg down
//   Occultation     the Moon's disc covering Mercury .. Saturn somewhere on Earth: the true disc's geocentric
//                   separation under its horizontal parallax + radius; the site is where the line from the planet
//                   through the Moon's centre meets the Earth, kept only if at the Watch moment the place is dark
//                   (Sun <= -9 deg, -4 for Mercury / Venus) and the Moon >= 10 deg up; the contacts there are the DRAWN
//                   disc's ("Moon size (x real)"), and the Watch lands a minute before the planet reappears at the limb
//   Moon base glint a lunar base's solar array mirroring the Sun to the Earth (LunarBases.h, SatelliteSimMoonBases.cpp):
//                   lunar::findWindows; watched from the brightest clear place, the camera on the base at a 1-deg FOV
// Positions are the sim's own (the Moon of date from satphot::moonGeoEciAt, the planets J2000 as planetStates), so a
// conjunction or occultation found here is one the sim draws, even where the two frames' ~0.5 deg of precession makes
// it differ from the real sky.
#include "../FrameProfile.h"
#include "../Harness.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "../UIRenderer.h"
#include "clay.h"
#include "UIPalette.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>

std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp

namespace { struct SkyKindDef; }
// The kind table and the helpers that need SatelliteSim's private members (a friend of it).
struct SkyEventsFriend
{
    static float ocean(const SatelliteSim *S, double latRad, double lonRad) { return S->oceanFraction(latRad, lonRad, 20000.0, 1); }
    static float cloud(const SatelliteSim *S, const glm::dvec3 &ecef) { return S->cloudCoverAtEcef(ecef); }
    static void cardSolar(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2);
    static void cardLunar(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2);
    static void cardConj(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2);
    static void cardOcc(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2);
    static void cardGlint(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2);
    static void cardComet(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2);
    static const SkyKindDef kinds[sky::kKindCount];
};

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kRe = 6371000.0;
constexpr double kDay = 86400.0;
constexpr double kAheadS = 2.0 * 365.25 * kDay;   // the searches' horizon
constexpr double kBackS = 4.0 * 3600.0;           // ... and how far back (an event under way when the window opens)
constexpr double kMoonR = 1737400.0;
constexpr int kIconSkyEvents = 32;                // SatelliteSimUI.cpp loadUiIcons: 32 the window, 33-36 the kinds
constexpr int kUpcomingRows = 10;

double wallMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
Clay_String cs(const char *s) { return Clay_String{false, (int32_t)strlen(s), s}; }
double deg(double r) { return r * (180.0 / kPi); }
double rad(double d) { return d * (kPi / 180.0); }
double angDeg(const glm::dvec3 &a, const glm::dvec3 &b)
{
    return deg(std::acos(std::clamp(glm::dot(glm::normalize(a), glm::normalize(b)), -1.0, 1.0)));
}
glm::dvec3 eciToEcef(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return glm::dvec3(c * v.x + s * v.y, -s * v.x + c * v.y, v.z);
}
double latOf(const glm::dvec3 &e) { return deg(std::asin(std::clamp(e.z, -1.0, 1.0))); }
double lonOf(const glm::dvec3 &e) { return deg(std::atan2(e.y, e.x)); }
const char *bodyName(int b)
{
    if (b == sky::kBodyMoon) return "Moon";
    if (b == sky::kBodySun) return "Sun";
    if (b >= sky::kBodyComet0) return "Comet";
    if (b >= sky::kBodyMoonBase0) return "Moon base";
    return b >= 0 && b < kPlanetCount ? kPlanetNames[b] : "?";
}
// The Moon's apparent magnitude (Allen's phase law, as updatePositions' moonlight).
float moonMagAt(double t)
{
    const glm::dvec3 m = glm::normalize(moonGeoEciAt(t)), s = glm::normalize(sunDirEciAt(t));
    const double ph = deg(std::acos(std::clamp(-glm::dot(m, s), -1.0, 1.0)));
    return (float)(-12.7 + 0.026 * ph + 4.0e-9 * std::pow(ph, 4.0));
}
// A body's geocentric unit direction (ECI) and magnitude.
glm::dvec3 bodyDir(int b, double t, float *mag = nullptr)
{
    if (b == sky::kBodyMoon)
    {
        if (mag) *mag = moonMagAt(t);
        return glm::normalize(moonGeoEciAt(t));
    }
    if (b == sky::kBodySun)
    {
        if (mag) *mag = -26.7f;
        return glm::normalize(sunDirEciAt(t));
    }
    glm::dvec3 d;
    double au;
    float m;
    skyPlanetGeoAt(b, t, d, au, m);
    if (mag) *mag = m;
    return d;
}
// The eclipse geometry findEclipse tests (keep in step): the separation of the Moon from the Sun (solar) or the
// antisolar point (lunar) and the limit an eclipse needs; for a lunar one also the umbra's radius and the Moon's.
void eclipseSepLim(bool solar, double t, double &sep, double &lim, double *umbra = nullptr, double *sMoon = nullptr)
{
    const glm::dvec3 M = moonGeoEciAt(t);
    glm::dvec3 s = glm::normalize(sunDirEciAt(t));
    if (!solar) s = -s;
    sep = std::acos(std::clamp(glm::dot(glm::normalize(M), s), -1.0, 1.0));
    const double dM = glm::length(M), dS = sunDistAuAt(t) * 1.495978707e11;
    const double piM = std::asin(6378137.0 / dM), piS = 6378137.0 / dS;
    const double sS = std::asin(satphot::kSunRadiusM / dS), sM = std::asin(kMoonR / dM);
    const double U = 1.0118 * (piM + piS - sS);
    lim = solar ? piM + sS + sM - piS : U + sM;
    if (umbra) *umbra = U;
    if (sMoon) *sMoon = sM;
}
// Where the line from a source at infinity (unit `toSrc`, ECI) through the Moon's centre meets the Earth (the near
// side), as an Earth-fixed unit direction; false (and the point under the line's nearest point) when it misses.
bool axisPoint(const glm::dvec3 &toSrc, double t, glm::dvec3 &siteEcef)
{
    const glm::dvec3 M = moonGeoEciAt(t), d = -toSrc;
    const double b = glm::dot(M, d), c = glm::dot(M, M) - kRe * kRe, disc = b * b - c;
    const bool hit = disc > 0.0;
    const glm::dvec3 p = hit ? M + d * (-b - std::sqrt(disc)) : M - d * b;
    siteEcef = glm::normalize(eciToEcef(glm::normalize(p), t));
    return hit;
}
// A site where every body (ECI directions) is at least minAlt up and the Sun at most sunMax, the lowest body
// nearest prefAlt, darker better; `ref` (optional) breaks near ties toward the player's place, `landK` prefers land.
struct SiteQ
{
    double prefAlt = 35.0, minAlt = 10.0, sunMax = -6.0;
    bool sunUp = false;              // a solar event: the Sun at least minAlt up instead
    const glm::dvec3 *ref = nullptr;
    const SatelliteSim *landSim = nullptr; // oceanFraction for a land bonus (main thread only)
};
bool pickSite(double t, const glm::dvec3 *dirs, int n, const SiteQ &q, glm::dvec3 &site, double *bestScore = nullptr)
{
    glm::dvec3 de[3];
    for (int i = 0; i < n; ++i) de[i] = eciToEcef(dirs[i], t);
    const glm::dvec3 se = eciToEcef(glm::normalize(sunDirEciAt(t)), t);
    double best = -1e30;
    bool ok = false;
    for (double la = -62.0; la <= 66.0; la += 2.0)
    {
        const double cl = std::cos(rad(la)), sl = std::sin(rad(la));
        for (double lo = -180.0; lo < 180.0; lo += 4.0)
        {
            const glm::dvec3 u(cl * std::cos(rad(lo)), cl * std::sin(rad(lo)), sl);
            double alt = 90.0;
            for (int i = 0; i < n; ++i) alt = std::min(alt, deg(std::asin(std::clamp(glm::dot(de[i], u), -1.0, 1.0))));
            const double sunAlt = deg(std::asin(std::clamp(glm::dot(se, u), -1.0, 1.0)));
            if (alt < q.minAlt || (!q.sunUp && sunAlt > q.sunMax)) continue;
            double sc = -std::abs(alt - q.prefAlt) + (q.sunUp ? 0.0 : 0.5 * std::min(q.sunMax - sunAlt, 12.0));
            if (q.ref) sc -= angDeg(u, *q.ref) / 15.0;
            if (sc > best - 6.0 && q.landSim && SkyEventsFriend::ocean(q.landSim, rad(la), rad(lo)) < 0.5f) sc += 6.0;
            if (q.landSim && sc > best - 30.0) sc -= 25.0 * SkyEventsFriend::cloud(q.landSim, u); // (a clear sky first)
            if (sc > best)
            {
                best = sc;
                site = u;
                ok = true;
            }
        }
    }
    if (bestScore) *bestScore = best;
    return ok;
}
// A solar eclipse's place to watch it at t: the central line (where the shadow's axis meets the Earth) when the Sun
// stands >= 15 deg up there, else the place with the Sun >= 15 deg up that sees the most of it (the Moon's centre
// nearest the Sun's, topocentric). Returns whether the eclipse is seen there at all.
bool solarSite(double t, glm::dvec3 &site, bool *central = nullptr)
{
    const glm::dvec3 sd = glm::normalize(sunDirEciAt(t)), se = eciToEcef(sd, t), M = moonGeoEciAt(t);
    const double rS = std::asin(satphot::kSunRadiusM / (sunDistAuAt(t) * 1.495978707e11));
    glm::dvec3 u;
    const bool hit = axisPoint(sd, t, u);
    if (central) *central = hit;
    if (hit && glm::dot(se, u) > std::sin(rad(15.0)))
    {
        site = u;
        return true;
    }
    double best = 1e30;
    for (double la = -80.0; la <= 80.0; la += 2.0)
    {
        const double cl = std::cos(rad(la)), sl = std::sin(rad(la));
        for (double lo = -180.0; lo < 180.0; lo += 3.0)
        {
            const glm::dvec3 v(cl * std::cos(rad(lo)), cl * std::sin(rad(lo)), sl);
            if (glm::dot(se, v) < std::sin(rad(15.0))) continue;
            const glm::dvec3 obs = observerEciAt(v, kRe, t);
            const double sep = std::acos(std::clamp(glm::dot(glm::normalize(M - obs), sd), -1.0, 1.0));
            if (sep < best)
            {
                best = sep;
                site = v;
            }
        }
    }
    if (central) *central = false;
    return best < std::asin(kMoonR / glm::length(M)) + rS;
}

// Local minima of f over [t0, t1] sampled every `step`, each refined (golden section); cb(t, fmin) for minima < thr.
template <class F, class CB>
void scanMinima(const F &f, double t0, double t1, double step, double thr, const sky::SearchCtx &c, const CB &cb)
{
    double p2 = f(t0), p1 = f(t0 + step);
    for (double t = t0 + 2.0 * step; t <= t1; t += step)
    {
        if (c.cancelled()) return;
        const double cur = f(t);
        if (p1 <= p2 && p1 < cur && p1 < thr * 1.6)
        {
            double lo = t - 2.0 * step, hi = t;
            for (int i = 0; i < 48; ++i)
            {
                const double a = lo + 0.382 * (hi - lo), b = lo + 0.618 * (hi - lo);
                if (f(a) < f(b)) hi = b; else lo = a;
            }
            const double tm = 0.5 * (lo + hi), fm = f(tm);
            if (fm < thr) cb(tm, fm);
        }
        p2 = p1;
        p1 = cur;
    }
}
// The edge of a window where inside(t) holds, going from tIn (inside) toward dir, in steps then bisected.
template <class F>
double windowEdge(const F &inside, double tIn, double dir, double step, double maxS)
{
    double a = tIn, b = tIn;
    for (double s = step; s <= maxS; s += step)
    {
        b = tIn + dir * s;
        if (!inside(b)) break;
        a = b;
    }
    if (inside(b)) return b;
    for (int i = 0; i < 30; ++i)
    {
        const double m = 0.5 * (a + b);
        if (inside(m)) a = m; else b = m;
    }
    return a;
}
// The drawn Moon's angular radius seen from `obsEci` (as updatePositions: x "Moon size" away from the Sun).
double drawnMoonR(const glm::dvec3 &moonEci, const glm::dvec3 &obsEci, double t, float scale)
{
    const glm::dvec3 topo = moonEci - obsEci;
    const double dM = glm::length(topo);
    const double sepSun = std::acos(std::clamp(glm::dot(topo / dM, glm::normalize(sunDirEciAt(t))), -1.0, 1.0));
    const double k = glm::smoothstep(rad(1.5), rad(4.0), sepSun);
    return std::asin(std::min(kMoonR / dM, 1.0)) * (1.0 + (std::clamp((double)scale, 0.25, 6.0) - 1.0) * k);
}

// ── The kinds ──────────────────────────────────────────────────────────────────────────────────────────────────
// The card's two text lines for an event (title + when, then where / how).
using CardFn = void (*)(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2);
struct SkyKindDef
{
    const char *id;      // harness / json name
    const char *card;    // the card's title
    int icon;            // icon index (SatelliteSimUI.cpp loadUiIcons)
    const Clay_Color *accent;
    void (SatelliteSim::*search)(const sky::SearchCtx &, std::vector<sky::Event> &) const;
    bool (SatelliteSim::*watch)(const sky::Event &, std::string &);
    CardFn text;
    const char *none;    // the card's status when the search found none
    const char *tip;     // the Watch button's tooltip
};

std::string eventTitle(const sky::Event &e)
{
    char b[160];
    switch (e.kind)
    {
    case sky::kSolarEclipse:
        snprintf(b, sizeof(b), "%s solar eclipse", e.total ? "Total" : e.annular ? "Annular" : "Partial");
        break;
    case sky::kLunarEclipse:
        snprintf(b, sizeof(b), "%s lunar eclipse", e.total ? "Total" : "Partial");
        break;
    case sky::kConjunction:
        if (e.b == sky::kBodyMoon || e.a == sky::kBodyMoon)
            snprintf(b, sizeof(b), "The Moon and %s, %.1f deg apart", bodyName(e.a == sky::kBodyMoon ? e.b : e.a), e.sepDeg);
        else
            snprintf(b, sizeof(b), "%s and %s, %.1f deg apart", bodyName(e.a), bodyName(e.b), e.sepDeg);
        break;
    case sky::kOccultation:
        snprintf(b, sizeof(b), "The Moon hides %s", bodyName(e.a));
        break;
    case sky::kMoonGlint:
        snprintf(b, sizeof(b), "%s array glint, mag %.1f", e.label, e.magA);
        break;
    case sky::kComet:
        snprintf(b, sizeof(b), "%s, mag %.1f", e.label, e.magA);
        break;
    default:
        snprintf(b, sizeof(b), "Event");
    }
    return b;
}
// "2037-01-31 13:59 UTC" (fmtUtcReadable without the seconds)
std::string utcShort(double t)
{
    std::string u = fmtUtcReadable(t);
    if (u.size() > 19) u.erase(16, 3);
    return u;
}
std::string whereText(const glm::dvec3 &s)
{
    char b[64];
    const double la = latOf(s), lo = lonOf(s);
    snprintf(b, sizeof(b), "%.1f %s %.1f %s", std::abs(la), la >= 0.0 ? "N" : "S", std::abs(lo), lo >= 0.0 ? "E" : "W");
    return b;
}
} // namespace

// (Short lines: a card's text is clipped at its edge, and at 1024 x 768 a line holds ~40 characters.)
void SkyEventsFriend::cardSolar(SatelliteSim &S, const sky::Event &e, double, const char *&l1, const char *&l2)
{
    l1 = S.skStr("%s, %.1f h on Earth", eventTitle(e).c_str(), (e.t1 - e.t0) / 3600.0);
    l2 = S.skStr("%s at %s", utcShort(e.t).c_str(), whereText(e.site).c_str());
}
void SkyEventsFriend::cardLunar(SatelliteSim &S, const sky::Event &e, double, const char *&l1, const char *&l2)
{
    l1 = S.skStr("%s, %.0f min in the umbra", eventTitle(e).c_str(), (e.t1 - e.t0) / 60.0);
    l2 = S.skStr("%s at %s", utcShort(e.t).c_str(), whereText(e.site).c_str());
}
void SkyEventsFriend::cardConj(SatelliteSim &S, const sky::Event &e, double, const char *&l1, const char *&l2)
{
    l1 = S.skStr("%s", eventTitle(e).c_str());
    l2 = S.skStr("%s at %s", utcShort(e.t).c_str(), whereText(e.site).c_str());
}
void SkyEventsFriend::cardOcc(SatelliteSim &S, const sky::Event &e, double, const char *&l1, const char *&l2)
{
    l1 = S.skStr("%s, hidden %.0f min", eventTitle(e).c_str(), (e.t1 - e.t0) / 60.0);
    l2 = S.skStr("%s at %s", utcShort(e.t).c_str(), whereText(e.site).c_str());
}

void SkyEventsFriend::cardGlint(SatelliteSim &S, const sky::Event &e, double, const char *&l1, const char *&l2)
{
    l1 = S.skStr("%s, %.0f h", eventTitle(e).c_str(), (e.t1 - e.t0) / 3600.0);
    l2 = S.skStr("%s at %s", utcShort(e.t).c_str(), whereText(e.site).c_str());
}

void SkyEventsFriend::cardComet(SatelliteSim &S, const sky::Event &e, double now, const char *&l1, const char *&l2)
{
    l1 = S.skStr("%s", e.label);
    if (now >= e.t0 && now < e.t1)
        l2 = S.skStr("mag %.1f, best %s (tail %.0f deg)", e.magA, utcShort(e.t).substr(0, 10).c_str(), e.sepDeg);
    else
        l2 = S.skStr("best %s, mag %.1f, tail %.0f deg", utcShort(e.t).substr(0, 10).c_str(), e.magA, e.sepDeg);
}

const SkyKindDef SkyEventsFriend::kinds[sky::kKindCount] = {
    {"solar", "SOLAR ECLIPSE", 33, &Pal::progressFill, &SatelliteSim::skySearchSolar, &SatelliteSim::skyWatchSolar, &SkyEventsFriend::cardSolar,
     "none in the next 2 years", "The Moon covering the Sun, from where the shadow falls"},
    {"lunar", "LUNAR ECLIPSE", 34, &Pal::btnDanger, &SatelliteSim::skySearchLunar, &SatelliteSim::skyWatchLunar, &SkyEventsFriend::cardLunar,
     "none in the next 2 years", "The Moon in the Earth's shadow, from a dark site"},
    {"conjunction", "CONJUNCTION", 35, &Pal::tagTwilight, &SatelliteSim::skySearchConjunctions,
     &SatelliteSim::skyWatchConjunction, &SkyEventsFriend::cardConj, "none in the next 2 years", "Both bodies in frame, from a dark site"},
    {"occultation", "OCCULTATION", 36, &Pal::btnAccent, &SatelliteSim::skySearchOccultations,
     &SatelliteSim::skyWatchOccultation, &SkyEventsFriend::cardOcc, "none in the next 2 years", "The planet behind the Moon, a minute before it reappears"},
    {"moonglint", "MOON BASE GLINT", 37, &Pal::tagTwilight, &SatelliteSim::skySearchMoonGlints,
     &SatelliteSim::skyWatchMoonGlint, &SkyEventsFriend::cardGlint, "none in the next 2 years",
     "A lunar base's solar array flashing on the Moon, through a telescope"},
    {"comet", "COMET", 38, &Pal::progressFill, &SatelliteSim::skySearchComets, &SatelliteSim::skyWatchComet,
     &SkyEventsFriend::cardComet, "no bright comet in the next 2 years", "The comet and its tail, from a dark place"},
};
static_assert(sizeof(SkyEventsFriend::kinds) / sizeof(SkyEventsFriend::kinds[0]) == sky::kKindCount, "one row per sky::Kind");
namespace
{
const SkyKindDef *const kSkyKinds = SkyEventsFriend::kinds;

nlohmann::json eventJson(const sky::Event &e, double now)
{
    nlohmann::json o = {{"kind", kSkyKinds[e.kind].id},
                        {"title", eventTitle(e)},
                        {"utc", fmtUtcReadable(e.t)},
                        {"t_j2000", e.t},
                        {"in_h", (e.t - now) / 3600.0},
                        {"starts_in_h", (e.t0 - now) / 3600.0},
                        {"ends_in_h", (e.t1 - now) / 3600.0},
                        {"happening", now >= e.t0 && now < e.t1},
                        {"a", bodyName(e.a)},
                        {"b", bodyName(e.b)},
                        {"sep_deg", e.sepDeg},
                        {"mag_a", e.magA},
                        {"mag_b", e.magB},
                        {"score", e.score},
                        {"total", e.total},
                        {"annular", e.annular}};
    if (e.hasSite)
    {
        o["site_lat"] = latOf(e.site);
        o["site_lon"] = lonOf(e.site);
    }
    if (e.watchT != 0.0) o["watch_utc"] = fmtUtcReadable(e.watchT);
    if (e.label[0]) o["label"] = e.label;
    return o;
}
} // namespace

// ── Searches (worker thread: they read only the ephemeris functions and their context) ───────────────────────────
void SatelliteSim::skySearchSolar(const sky::SearchCtx &c, std::vector<sky::Event> &out) const
{
    for (double t = c.t0; t < c.t1 && !c.cancelled();)
    {
        const double te = findEclipse(true, t);
        if (te < 0.0 || te > c.t1) break;
        sky::Event e;
        e.kind = sky::kSolarEclipse;
        e.t = te;
        e.a = sky::kBodySun;
        e.b = sky::kBodyMoon;
        double sep, lim;
        eclipseSepLim(true, te, sep, lim);
        e.sepDeg = deg(sep);
        auto inside = [&](double tt) { double s, l; eclipseSepLim(true, tt, s, l); return s < l; };
        e.t0 = windowEdge(inside, te, -1.0, 600.0, 5.0 * 3600.0);
        e.t1 = windowEdge(inside, te, 1.0, 600.0, 5.0 * 3600.0);
        const glm::dvec3 sd = glm::normalize(sunDirEciAt(te));
        bool central = false;
        e.hasSite = solarSite(te, e.site, &central);
        (void)sd;
        if (central)
        {
            // total / annular on the central line: the Moon's disc against the Sun's, seen from there
            const glm::dvec3 obs = observerEciAt(e.site, kRe, te), M = moonGeoEciAt(te);
            const double rM = std::asin(kMoonR / glm::length(M - obs));
            const double rS = std::asin(satphot::kSunRadiusM / (sunDistAuAt(te) * 1.495978707e11));
            e.total = rM >= rS;
            e.annular = !e.total;
        }
        e.score = 1.0 - sep / lim;
        e.watchT = te;
        out.push_back(e);
        t = te + 20.0 * kDay;
    }
}

void SatelliteSim::skySearchLunar(const sky::SearchCtx &c, std::vector<sky::Event> &out) const
{
    for (double t = c.t0; t < c.t1 && !c.cancelled();)
    {
        const double te = findEclipse(false, t);
        if (te < 0.0 || te > c.t1) break;
        sky::Event e;
        e.kind = sky::kLunarEclipse;
        e.t = te;
        e.a = sky::kBodyMoon;
        e.b = sky::kBodySun;
        double sep, lim, U, sM;
        eclipseSepLim(false, te, sep, lim, &U, &sM);
        e.sepDeg = deg(sep);
        e.total = sep < U - sM;
        auto inside = [&](double tt) { double s, l; eclipseSepLim(false, tt, s, l); return s < l; };
        e.t0 = windowEdge(inside, te, -1.0, 300.0, 4.0 * 3600.0);
        e.t1 = windowEdge(inside, te, 1.0, 300.0, 4.0 * 3600.0);
        const glm::dvec3 md = glm::normalize(moonGeoEciAt(te));
        SiteQ q;
        q.prefAlt = 40.0;
        q.minAlt = 15.0;
        q.sunMax = -12.0;
        e.hasSite = pickSite(te, &md, 1, q, e.site);
        if (!e.hasSite) e.site = glm::normalize(eciToEcef(md, te)); // (under the Moon: always dark)
        e.hasSite = true;
        e.score = 1.0 - sep / lim;
        e.watchT = te;
        out.push_back(e);
        t = te + 20.0 * kDay;
    }
}

void SatelliteSim::skySearchConjunctions(const sky::SearchCtx &c, std::vector<sky::Event> &out) const
{
    // Mercury .. Saturn (Uranus at mag ~5.7 is left out) and the Moon. Planet pairs every 6 h within 2.5 deg; the Moon
    // and a planet brighter than mag 1.5 every hour within 1.5 deg, but not closer than an occultation's limit (that
    // is the Occultation kind's).
    const int kPl = 5;
    auto tryEvent = [&](int a, int b, double tm, double sepRad, double halfWin, double winStep)
    {
        sky::Event e;
        e.kind = sky::kConjunction;
        e.t = tm;
        e.a = a;
        e.b = b;
        e.sepDeg = deg(sepRad);
        const glm::dvec3 da = bodyDir(a, tm, &e.magA), db = bodyDir(b, tm, &e.magB);
        const glm::dvec3 sd = glm::normalize(sunDirEciAt(tm));
        if (angDeg(da, sd) < 12.0 || angDeg(db, sd) < 12.0) return;
        if (b == sky::kBodyMoon && e.magA > 1.5f) return;
        auto sepAt = [&](double tt) { return angDeg(bodyDir(a, tt), bodyDir(b, tt)); };
        const double lim = e.sepDeg + 0.5;
        auto inside = [&](double tt) { return sepAt(tt) < lim; };
        e.t0 = windowEdge(inside, tm, -1.0, winStep, halfWin);
        e.t1 = windowEdge(inside, tm, 1.0, winStep, halfWin);
        // A dark site, at the closest approach or as near it as the window allows.
        SiteQ q;
        q.prefAlt = 30.0;
        q.minAlt = 10.0;
        q.sunMax = -6.0;
        bool ok = false;
        for (int k = 0; k <= 8 && !ok; ++k)
        {
            const double off = (k == 0 ? 0.0 : ((k + 1) / 2) * (k % 2 ? 1.0 : -1.0)) * std::max(winStep, 3.0 * 3600.0);
            const double tt = tm + off;
            if (tt < e.t0 || tt >= e.t1) continue;
            const glm::dvec3 dd[2] = {bodyDir(a, tt), bodyDir(b, tt)};
            if (pickSite(tt, dd, 2, q, e.site))
            {
                ok = true;
                e.watchT = tt;
            }
        }
        if (!ok) return;
        e.hasSite = true;
        const float faint = std::max(e.magA, e.magB);
        e.score = std::pow(10.0, -0.4 * faint) / std::max(e.sepDeg, 0.05);
        out.push_back(e);
    };
    // planet pairs
    for (int i = 0; i < kPl && !c.cancelled(); ++i)
        for (int j = i + 1; j < kPl && !c.cancelled(); ++j)
        {
            auto f = [&](double t) { return std::acos(std::clamp(glm::dot(bodyDir(i, t), bodyDir(j, t)), -1.0, 1.0)); };
            scanMinima(f, c.t0, c.t1, 6.0 * 3600.0, rad(2.5), c,
                       [&](double tm, double s) { tryEvent(i, j, tm, s, 4.0 * kDay, 3600.0); });
        }
    // the Moon and a planet
    for (int i = 0; i < kPl && !c.cancelled(); ++i)
    {
        auto f = [&](double t) { return std::acos(std::clamp(glm::dot(bodyDir(i, t), bodyDir(sky::kBodyMoon, t)), -1.0, 1.0)); };
        scanMinima(f, c.t0, c.t1, 3600.0, rad(1.5), c,
                   [&](double tm, double s)
                   {
                       const double dM = glm::length(moonGeoEciAt(tm));
                       const double occ = std::asin(6378137.0 / dM) + std::asin(kMoonR / dM);
                       if (s >= occ) tryEvent(i, sky::kBodyMoon, tm, s, 8.0 * 3600.0, 600.0);
                   });
    }
    std::sort(out.begin(), out.end(), [](const sky::Event &x, const sky::Event &y) { return x.t < y.t; });
}

namespace
{
// An occultation's places to watch it from: where the line from planet p through the Moon's centre meets the Earth
// (the Moon's centre exactly on the planet there), every 2 min over +-4 h of the geocentric closest approach tm. The
// Watch lands a minute before the planet reappears at the DRAWN disc's limb (x "Moon size", often 2-3 h after the
// central moment at one place), so a place is kept only if at THAT moment it is dark (Sun <= -9 deg; -4 for Mercury
// and Venus) and the Moon >= 10 deg up. The best scored (higher, darker) first; up to maxN, at least 300 km apart.
struct OccSite
{
    glm::dvec3 site;
    double t0, t1, tmin, tw;
};
void occultationSites(int p, double tm, float moonScale, const sky::SearchCtx *c, size_t maxN, std::vector<OccSite> &out)
{
    struct Cand { double sc, t; glm::dvec3 u; };
    std::vector<Cand> cands;
    auto sunAltAt = [&](const glm::dvec3 &u, double t) { return deg(std::asin(std::clamp(glm::dot(eciToEcef(glm::normalize(sunDirEciAt(t)), t), u), -1.0, 1.0))); };
    for (double t = tm - 4.0 * 3600.0; t <= tm + 4.0 * 3600.0; t += 120.0)
    {
        const glm::dvec3 dp = bodyDir(p, t);
        glm::dvec3 u;
        if (!axisPoint(dp, t, u)) continue;
        const double sunAlt = sunAltAt(u, t);
        const double plAlt = deg(std::asin(std::clamp(glm::dot(eciToEcef(dp, t), u), -1.0, 1.0)));
        if (sunAlt > -4.0 || plAlt < 15.0) continue;
        cands.push_back({std::min(plAlt, 60.0) + 0.7 * std::min(-sunAlt, 18.0) + (std::abs(latOf(u)) < 66.0 ? 5.0 : 0.0), t, u});
    }
    std::sort(cands.begin(), cands.end(), [](const Cand &x, const Cand &y) { return x.sc > y.sc; });
    int tried = 0;
    for (size_t ci = 0; ci < cands.size() && tried < 24 && out.size() < maxN && !(c && c->cancelled()); ++ci)
    {
        const glm::dvec3 site = cands[ci].u;
        bool near = false;
        for (const OccSite &o : out) near |= angDeg(o.site, site) < 2.7;   // (~300 km)
        if (near) continue;
        ++tried;
        const double ts = cands[ci].t;
        // The contacts there, with the disc as drawn.
        auto topoSep = [&](double t)
        {
            const glm::dvec3 obs = observerEciAt(site, kRe + 2.0, t), M = moonGeoEciAt(t);
            return std::acos(std::clamp(glm::dot(glm::normalize(M - obs), bodyDir(p, t)), -1.0, 1.0)) - drawnMoonR(M, obs, t, moonScale);
        };
        double lo = ts - 1800.0, hi = ts + 1800.0;
        for (int i = 0; i < 40; ++i)
        {
            const double a = lo + 0.382 * (hi - lo), b = lo + 0.618 * (hi - lo);
            if (topoSep(a) < topoSep(b)) hi = b; else lo = a;
        }
        const double tmin = 0.5 * (lo + hi);
        if (topoSep(tmin) >= 0.0) continue;
        auto inside = [&](double t) { return topoSep(t) < 0.0; };
        const double t1 = windowEdge(inside, tmin, 1.0, 60.0, 4.0 * 3600.0);
        const double t0 = windowEdge(inside, tmin, -1.0, 60.0, 4.0 * 3600.0);
        const double tw = std::max(t0 + 10.0, t1 - 60.0);
        const glm::dvec3 obs = observerEciAt(site, kRe + 2.0, tw);
        const double moonAlt = deg(std::asin(std::clamp(glm::dot(glm::normalize(moonGeoEciAt(tw) - obs), glm::normalize(obs)), -1.0, 1.0)));
        if (sunAltAt(site, tw) > (p <= kVenus ? -4.0 : -9.0) || moonAlt < 10.0) continue;
        out.push_back({site, t0, t1, tmin, tw});
    }
}
} // namespace

void SatelliteSim::skySearchOccultations(const sky::SearchCtx &c, std::vector<sky::Event> &out) const
{
    for (int p = 0; p < 5 && !c.cancelled(); ++p)
    {
        auto f = [&](double t) { return std::acos(std::clamp(glm::dot(bodyDir(p, t), bodyDir(sky::kBodyMoon, t)), -1.0, 1.0)); };
        scanMinima(f, c.t0, c.t1, 3600.0, rad(1.3), c,
                   [&](double tm, double s)
                   {
                       const double dM = glm::length(moonGeoEciAt(tm));
                       if (s >= std::asin(6378137.0 / dM) + std::asin(kMoonR / dM)) return;
                       std::vector<OccSite> sites;
                       occultationSites(p, tm, c.moonSizeScale, &c, 1, sites);
                       if (sites.empty()) return;
                       const OccSite &o = sites[0];
                       sky::Event e;
                       e.kind = sky::kOccultation;
                       e.a = p;
                       e.b = sky::kBodyMoon;
                       e.t = o.tmin;
                       e.t0 = o.t0;
                       e.t1 = o.t1;
                       e.tAux = tm;
                       e.sepDeg = deg(s);
                       e.magB = moonMagAt(o.tmin);
                       bodyDir(p, o.tmin, &e.magA);
                       e.site = o.site;
                       e.hasSite = true;
                       e.watchT = o.tw;
                       e.score = std::pow(10.0, -0.4 * e.magA);
                       out.push_back(e);
                   });
    }
    std::sort(out.begin(), out.end(), [](const sky::Event &x, const sky::Event &y) { return x.t < y.t; });
}

// ── Watches (main thread) ────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::skyJumpTo(double t)
{
    if (followActive) stopFollow();
    stopTrack();
    const double d = std::floor(t / kDay);
    simDayJ2000 = (int64_t)d;
    simSecInDay = t - d * kDay;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    for (double &bb : cv2WxBakedT) bb = -1e30;
}
void SatelliteSim::skyPlaceAt(const glm::dvec3 &siteEcef, double t)
{
    setObserverLatLon(latOf(siteEcef), lonOf(siteEcef));
    obsHeightOffset = std::max(0.0f, cpuTerrainHeightM(obsLatDeg, obsLonDeg)) + 2.0f;
    updatePositions(t, 0.0f);
}
glm::vec3 SatelliteSim::skyBodyEnu(int body) const
{
    if (body == sky::kBodySun) return glm::normalize(glm::vec3(sunDirENU));
    if (body == sky::kBodyMoon) return glm::normalize(glm::vec3(moonDirENU));
    if (body >= sky::kBodyCometTail0) return cometTailEnu(body - sky::kBodyCometTail0);
    if (body >= sky::kBodyComet0) return cometHeadEnu(body - sky::kBodyComet0);
    if (body >= sky::kBodyMoonBase0)
    {
        glm::vec3 e;
        return moonBaseDrawnEnu(body - sky::kBodyMoonBase0, e) ? e : glm::normalize(glm::vec3(moonDirENU));
    }
    if (body < 0 || body >= kPlanetCount) return glm::vec3(0, 1, 0);
    const glm::vec3 e = planetStates[body].eciDir;
    return glm::normalize(glm::vec3(glm::dot(e, glm::vec3(eci2enuX)), glm::dot(e, glm::vec3(eci2enuY)), glm::dot(e, glm::vec3(eci2enuZ))));
}
void SatelliteSim::skyAimAt(int a, int b, float fovDeg, double until)
{
    glm::vec3 d = skyBodyEnu(a);
    if (b >= 0) d = glm::normalize(d + skyBodyEnu(b));
    aimCameraAzEl(glm::degrees(std::atan2(d.x, d.y)), glm::degrees(std::asin(std::clamp(d.z, -1.0f, 1.0f))));
    camera.fovYDeg = fovDeg;
    skAimA = a;
    skAimB = b;
    skAimUntil = until;
    skAimAz = camera.azDeg;
    skAimEl = camera.elDeg;
}
// Keeps a Watch's bodies in frame (the sky turns ~0.25 deg a minute) until the player looks away, moves, follows or
// tracks something, a cinematic runs, or the event is over.
void SatelliteSim::skyEventsTick()
{
    if (skAimA < 0) return;
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    auto wrap = [](float x) { return std::remainder(x, 360.0f); };
    if (now > skAimUntil || followActive || trackActive || cineActive() || std::abs(wrap(camera.azDeg - skAimAz)) > 0.05f ||
        std::abs(camera.elDeg - skAimEl) > 0.05f)
    {
        skAimA = skAimB = -1;
        return;
    }
    skyAimAt(skAimA, skAimB, camera.fovYDeg, skAimUntil);
}

bool SatelliteSim::skyWatchSolar(const sky::Event &e, std::string &msg)
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    const double tw = (now >= e.t0 && now < e.t1) ? now : e.t;
    glm::dvec3 site;
    bool central = false;
    solarSite(tw, site, &central);
    skyJumpTo(tw);
    skyPlaceAt(site, tw);
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    skyAimAt(sky::kBodySun, -1, 16.0f, e.t1);
    char b[220];
    snprintf(b, sizeof(b), "%s: the Moon %s the Sun, seen from %s (Sun %.0f deg up)", eventTitle(e).c_str(),
             central ? (e.annular ? "inside" : "covering") : "biting into", whereText(site).c_str(), glm::degrees(std::asin(std::clamp(sunDirENU.z, -1.0f, 1.0f))));
    msg = b;
    return true;
}

bool SatelliteSim::skyWatchLunar(const sky::Event &e, std::string &msg)
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    const double tw = (now >= e.t0 && now < e.t1) ? now : e.t;
    const glm::dvec3 md = glm::normalize(moonGeoEciAt(tw));
    const glm::dvec3 here = glm::normalize(glm::dvec3(obsDir));
    SiteQ q;
    q.prefAlt = 40.0;
    q.minAlt = 15.0;
    q.sunMax = -12.0;
    q.ref = &here;
    q.landSim = this;
    glm::dvec3 site;
    skyJumpTo(tw); // (first: the cloud map's drift, which the site's clear-sky score reads, is that moment's)
    if (!pickSite(tw, &md, 1, q, site)) site = glm::normalize(eciToEcef(md, tw));
    skyPlaceAt(site, tw);
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    skyAimAt(sky::kBodyMoon, -1, 12.0f, e.t1);
    char b[220];
    snprintf(b, sizeof(b), "%s: the Moon in the Earth's shadow, seen from %s (Moon %.0f deg up)", eventTitle(e).c_str(),
             whereText(site).c_str(), glm::degrees(std::asin(std::clamp(moonDirENU.z, -1.0f, 1.0f))));
    msg = b;
    return true;
}

bool SatelliteSim::skyWatchConjunction(const sky::Event &e, std::string &msg)
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    const glm::dvec3 here = glm::normalize(glm::dvec3(obsDir));
    SiteQ q;
    q.prefAlt = 30.0;
    q.minAlt = 10.0;
    q.sunMax = -6.0;
    q.ref = &here;
    q.landSim = this;
    glm::dvec3 site = e.site;
    double tw = e.watchT;
    bool placed = false;
    if (now >= e.t0 && now < e.t1)
    {
        const glm::dvec3 dd[2] = {bodyDir(e.a, now), bodyDir(e.b, now)};
        placed = pickSite(now, dd, 2, q, site);
        if (placed) tw = now;
    }
    skyJumpTo(tw); // (before the site: the cloud map's drift, which its clear-sky score reads, is that moment's)
    if (!placed)
    {
        const glm::dvec3 dd[2] = {bodyDir(e.a, tw), bodyDir(e.b, tw)};
        if (!pickSite(tw, dd, 2, q, site)) site = e.site;
    }
    skyPlaceAt(site, tw);
    const double sepNow = angDeg(glm::dvec3(skyBodyEnu(e.a)), glm::dvec3(skyBodyEnu(e.b)));
    const float fov = (float)std::clamp(sepNow * 4.0, 4.0, 40.0);
    const int planet = e.a == sky::kBodyMoon ? e.b : e.b == sky::kBodyMoon ? e.a : (e.magA <= e.magB ? e.a : e.b);
    selectedSatIndex = -1;
    selectedPlanetIndex = planet;
    formatSelectedPlanetInfo();
    skyAimAt(e.a, e.b, fov, e.t1);
    char b[240];
    const bool moonPair = e.b == sky::kBodyMoon || e.a == sky::kBodyMoon;
    snprintf(b, sizeof(b), "%s and %s %.1f deg apart, seen from %s (%.0f deg up, Sun %.0f deg down)",
             moonPair ? "The Moon" : bodyName(e.a), moonPair ? bodyName(planet) : bodyName(e.b),
             sepNow, whereText(site).c_str(), camera.elDeg,
             -glm::degrees(std::asin(std::clamp(sunDirENU.z, -1.0f, 1.0f))));
    msg = b;
    return true;
}

bool SatelliteSim::skyWatchOccultation(const sky::Event &e, std::string &msg)
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    // The place: the search's, unless it is under cloud and another place along the occultation's track is clearer
    // (the CPU cloud map, as the re-entry Watch; only while the event is still ahead or under way at the search's place).
    glm::dvec3 site = e.site;
    double t0 = e.t0, t1 = e.t1, twBase = e.watchT;
    skyJumpTo(now >= e.t0 && now < e.t1 ? std::max(now, e.watchT) : e.watchT); // (the cloud map's drift then)
    if (now < e.t0 && e.tAux != 0.0 && cloudCoverAtEcef(e.site) > 0.35f)
    {
        std::vector<OccSite> alt;
        occultationSites(e.a, e.tAux, moonSizeScale, nullptr, 6, alt);
        float best = cloudCoverAtEcef(e.site);
        for (const OccSite &o : alt)
        {
            const float cc = cloudCoverAtEcef(o.site);
            if (cc < best - 0.1f && o.t0 > now)
            {
                best = cc;
                site = o.site;
                t0 = o.t0;
                t1 = o.t1;
                twBase = o.tw;
            }
        }
    }
    const double tw = now >= t0 && now < t1 ? std::max(now, twBase) : twBase;
    skyJumpTo(tw);
    skyPlaceAt(site, tw);
    selectedSatIndex = -1;
    selectedPlanetIndex = e.a;
    formatSelectedPlanetInfo();
    skyAimAt(sky::kBodyMoon, -1, 5.0f, t1 + 600.0);
    char b[240];
    snprintf(b, sizeof(b), "%s is behind the Moon, seen from %s; it reappears at the Moon's limb in about %.0f s",
             bodyName(e.a), whereText(site).c_str(), std::max(0.0, t1 - tw));
    msg = b;
    return true;
}

// ── Moon-base glints (2026-10-27; LunarBases.h, CLAUDE.md "Moon-base glints") ──────────────────────────────────────
// The windows when a base's array mirrors the Sun to somewhere on Earth's night side brighter than the limit (with the
// Moon >= 10 deg up and the Sun <= -6 deg there): lunar::findWindows, ~3 s for 2 years on the worker. It reads
// lunarBases_, which is written once at init.
void SatelliteSim::skySearchMoonGlints(const sky::SearchCtx &c, std::vector<sky::Event> &out) const
{
    if (lunarBases_.empty()) return;
    std::vector<lunar::Window> w;
    lunar::findWindows(lunarBases_, c.t0, c.t1, c.moonBaseMagLimit, c.moonBaseJitter, 0.0,
                       [](const void *p) { return static_cast<const sky::SearchCtx *>(p)->cancelled(); }, &c, w);
    for (const lunar::Window &x : w)
    {
        sky::Event e;
        e.kind = sky::kMoonGlint;
        e.t = x.tPeak;
        e.t0 = x.t0;
        e.t1 = x.t1;
        e.a = sky::kBodyMoonBase0 + x.base;
        e.magA = (float)x.peakMag;
        e.score = -x.peakMag;
        e.hasSite = true;
        e.site = x.site.ecef;
        e.watchT = x.tPeak;
        e.tAux = x.moonPhaseDeg;
        snprintf(e.label, sizeof(e.label), "%s", lunarBases_[(size_t)x.base].shortName.c_str());
        out.push_back(e);
    }
}

bool SatelliteSim::skyWatchMoonGlint(const sky::Event &e, std::string &msg)
{
    const int bi = e.a - sky::kBodyMoonBase0;
    if (bi < 0 || bi >= (int)lunarBases_.size()) return false;
    const lunar::Base &B = lunarBases_[(size_t)bi];
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    const double tw = (now >= e.t0 && now < e.t1) ? now : e.watchT;
    skyJumpTo(tw); // (first: the cloud map's drift, which the clear-sky choice below reads, is that moment's)
    // The brightest place (the rows), then, if it is under cloud, the clearest place round it within 1 mag of it.
    lunar::Site best;
    glm::dvec3 site = e.site;
    double mag = e.magA;
    if (lunar::bestSite(B, tw, moonBaseJitter, 0.0, best, 10.0, -6.0, true))
    {
        site = best.ecef;
        mag = best.mag;
    }
    if (cloudCoverAtEcef(site) > 0.3f)
    {
        glm::dvec3 P, X, Y;
        lunar::moonBodyFrame(tw, P, X, Y);
        const glm::dvec3 M = moonGeoEciAt(tw);
        const lunar::Geometry g = lunar::geometryAt(B, tw, M, P, X, Y);
        const glm::dvec3 se = eciToEcef(glm::normalize(sunDirEciAt(tw)), tw);
        float bestCloud = cloudCoverAtEcef(site);
        const double la0 = latOf(site), lo0 = lonOf(site);
        for (double dla = -12.0; dla <= 12.0; dla += 3.0)
            for (double dlo = -15.0; dlo <= 15.0; dlo += 3.0)
            {
                const double la = std::clamp(la0 + dla, -85.0, 85.0), lo = lo0 + dlo;
                const glm::dvec3 u(std::cos(rad(la)) * std::cos(rad(lo)), std::cos(rad(la)) * std::sin(rad(lo)), std::sin(rad(la)));
                if (glm::dot(se, u) > std::sin(rad(-6.0))) continue;
                const glm::dvec3 O = observerEciAt(u, kRe, tw);
                if (glm::dot(glm::normalize(M - O), glm::normalize(O)) < std::sin(rad(10.0))) continue;
                const float cc = cloudCoverAtEcef(u);
                if (cc > bestCloud - 0.1f) continue;
                const lunar::Glint gl = lunar::glintRows(B, g, tw, O, moonBaseJitter);
                if (gl.mag > mag + 1.0) continue;
                bestCloud = cc;
                site = u;
                mag = gl.mag;
            }
    }
    skyPlaceAt(site, tw);
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    skyAimAt(e.a, -1, 1.0f, e.t1);
    char b[260];
    snprintf(b, sizeof(b), "%s: its solar array mirroring the Sun, mag %.1f, seen from %s (Moon %.0f deg up, phase angle %.1f deg)",
             B.name.c_str(), mag, whereText(site).c_str(), glm::degrees(std::asin(std::clamp(moonDirENU.z, -1.0f, 1.0f))),
             e.tAux);
    msg = b;
    return true;
}

bool SatelliteSim::skyEventWatch(const sky::Event &e, std::string &msg)
{
    FPROF_SCOPE("skyEventWatch");
    if (e.kind < 0 || e.kind >= sky::kKindCount) return false;
    const sky::Event copy = e; // (the lists are rebuilt after the jump)
    const bool ok = (this->*kSkyKinds[copy.kind].watch)(copy, msg);
    if (ok)
    {
        snprintf(screenshotToastText, sizeof(screenshotToastText), "%s", msg.c_str());
        screenshotToastTimer = 4.0f;
        screenshotToastWarn = false;
        skUpcomingWall = -1e300;
    }
    return ok;
}

// ── The search worker ────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::skyEventsUpdate()
{
    FPROF_SCOPE("skyEventsUpdate");
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    if (skScanBase < -1e299 || now < skScanBase - 3600.0 || now > skScanBase + 30.0 * kDay)
    {
        if (skScanJob.valid())
        {
            skScanCancel = true;
            skScanJobStale = true;
        }
        skScanBase = now;
        skScanKind = 0;
        for (auto &v : skFound) v.clear();
        ++skScanRestarts;
        skUpcomingWall = -1e300;
    }
    if (skScanJob.valid() && skScanJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        SkyScanResult r = skScanJob.get();
        if (!skScanJobStale && r.base == skScanBase && r.kind == skScanKind)
        {
            skFound[r.kind] = std::move(r.events);
            skScanMs[r.kind] = r.ms;
            ++skScanKind;
            skUpcomingWall = -1e300;
        }
        skScanJobStale = false;
    }
    if (!skScanJob.valid() && skScanKind < sky::kKindCount)
    {
        sky::SearchCtx ctx;
        ctx.t0 = skScanBase - kBackS;
        ctx.t1 = skScanBase + kAheadS;
        ctx.moonSizeScale = moonSizeScale;
        ctx.moonBaseJitter = moonBaseJitter;
        ctx.moonBaseMagLimit = moonBaseMagLimit;
        ctx.cometsMinor = cometsMinor;
        ctx.cancel = &skScanCancel;
        skScanCancel = false;
        const int k = skScanKind;
        const double base = skScanBase;
        skScanJob = std::async(std::launch::async, [this, ctx, k, base]() {
            SkyScanResult r;
            r.base = base;
            r.kind = k;
            const double t0 = wallMs();
            (this->*kSkyKinds[k].search)(ctx, r.events);
            r.ms = wallMs() - t0;
            return r;
        });
    }
}

void SatelliteSim::skyEventsFinish(double maxS)
{
    const auto t0 = std::chrono::steady_clock::now();
    skyEventsUpdate();
    while (skScanKind < sky::kKindCount && std::chrono::steady_clock::now() - t0 < std::chrono::duration<double>(maxS))
    {
        if (skScanJob.valid()) skScanJob.wait();
        skyEventsUpdate();
    }
    skyEventsRefreshUpcoming(true);
}

const sky::Event *SatelliteSim::skyEventNext(int kind) const
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    if (kind == sky::kComet)
    {
        // the brightest comet under way or coming within a year (a faint one sooner does not hide a great one)
        const sky::Event *best = nullptr;
        for (const sky::Event &e : skFound[kind])
            if (e.t1 > now && e.t0 < now + 365.25 * kDay && (!best || e.magA < best->magA)) best = &e;
        if (best) return best;
    }
    for (const sky::Event &e : skFound[kind])
        if (e.t1 > now) return &e;
    return nullptr;
}

void SatelliteSim::skyEventsRefreshUpcoming(bool force)
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    const double wall = wallMs() / 1000.0;
    if (!force && wall - skUpcomingWall < 1.0 && std::abs(now - skUpcomingSim) < 60.0) return;
    skUpcomingWall = wall;
    skUpcomingSim = now;
    skUpcoming.clear();
    for (int k = 0; k < sky::kKindCount; ++k)
        for (const sky::Event &e : skFound[k])
            if (e.t1 > now && e.t0 < now + kAheadS) skUpcoming.push_back(e);
    std::sort(skUpcoming.begin(), skUpcoming.end(), [](const sky::Event &a, const sky::Event &b) { return a.t < b.t; });
    if (skUpcoming.size() > (size_t)kUpcomingRows) skUpcoming.resize(kUpcomingRows);
}

const char *SatelliteSim::skStr(const char *fmt, ...)
{
    char b[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(b, sizeof(b), fmt, ap);
    va_end(ap);
    skText_.emplace_back(b);
    return skText_.back().c_str();
}

// ── The window ─────────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::buildSkyEventsWindow(const UIInput &inp, UIRenderer &ui)
{
    FPROF_SCOPE("buildSkyEventsWindow");
    if (!skChrome.open)
        return;
    skText_.clear();
    if (skChrome.w <= 0.0f)
    {
        skChrome.w = winDefault(560.0f, 0.42f, inp.screenW, 380.0f);
        skChrome.h = winDefault(std::min(780.0f, inp.screenH - 160.0f), 0.74f, inp.screenH, 360.0f);
    }
    skyEventsUpdate();
    skyEventsRefreshUpcoming(false);
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    auto text = [&](const char *s, Clay_Color c, int size) {
        CLAY_TEXT(cs(s), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs(size), .wrapMode = CLAY_TEXT_WRAP_NONE}));
    };
    auto inText = [&](double s) -> const char * {
        s = std::max(s, 0.0);
        const long long m = (long long)std::floor(s / 60.0);
        if (m >= 2 * 1440) return skStr("in %lld d %lld h", m / 1440, (m % 1440) / 60);
        if (m >= 60) return skStr("in %lld h %02lld min", m / 60, m % 60);
        return skStr("in %lld min", m);
    };
    static char titleBuf[32];
    snprintf(titleBuf, sizeof(titleBuf), "Sky events");
    buildResizableWindow(
        inp, ui, skChrome, 13, titleBuf, true, hovSkClose, inp.screenW - skChrome.w - 60.0f, 100.0f, 340.0f, 300.0f, 1400.0f,
        1400.0f,
        [&]()
        {
            CLAY(CLAY_ID("SkBody"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                .padding = {12, 12, 8, 10},
                                                .childGap = 6,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                if (skStatus != skStatusShown)
                {
                    skStatusShown = skStatus;
                    skStatusWall = glfwGetTime();
                }
                if (!skStatus.empty() && glfwGetTime() - skStatusWall > 12.0)
                    skStatus.clear();
                // (one fixed line: empty when there is no message, so nothing below moves)
                text(skStatus.empty() ? " " : skStr("%s", skStatus.c_str()), Pal::textWarn, 11);
                CLAY(CLAY_ID("SkList"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                    .padding = {0, 8, 2, 4},
                                                    .childGap = 5,
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                         .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                {
                    uiSection("SkSecNext", 0, "NEXT OF EACH KIND");
                    const float lineH = (float)fs(12) * 1.35f;
                    for (int k = 0; k < sky::kKindCount; ++k)
                    {
                        const SkyKindDef &K = kSkyKinds[k];
                        const sky::Event *e = skyEventNext(k);
                        const char *st, *l1 = "", *l2 = "";
                        if (k >= skScanKind)
                            st = "searching the next 2 years...";
                        else if (!e)
                            st = K.none;
                        else
                        {
                            st = now >= e->t0 ? skStr("HAPPENING NOW, %s left", inText(e->t1 - now) + 3)
                                              : inText(e->t0 - now);
                            K.text(*this, *e, now, l1, l2);
                        }
                        CLAY(CLAY_IDI("SkCard", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(std::max(3.0f * lineH + 18.0f, 64.0f))},
                                                                          .padding = {8, 8, 6, 6},
                                                                          .childGap = 8,
                                                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                          .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                               .backgroundColor = Pal::cardBg,
                                                               .cornerRadius = CLAY_CORNER_RADIUS(4),
                                                               .border = {.color = Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
                        {
                            CLAY(CLAY_IDI("SkIconBg", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_FIXED(40), CLAY_SIZING_FIXED(40)},
                                                                                .childAlignment = {CLAY_ALIGN_X_CENTER, CLAY_ALIGN_Y_CENTER}},
                                                                     .backgroundColor = *K.accent,
                                                                     .cornerRadius = CLAY_CORNER_RADIUS(6)})
                            {
                                CLAY(CLAY_IDI("SkIcon", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(30)}},
                                                                       .image = {.imageData = (void *)(intptr_t)(K.icon + 1)}}) {}
                            }
                            CLAY(CLAY_IDI("SkText", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                                              .childGap = 2,
                                                                              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                              .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                                                   .clip = {.horizontal = true}})
                            {
                                CLAY(CLAY_IDI("SkHead", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                                  .childGap = 6,
                                                                                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                                  .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                                {
                                    text(K.card, Pal::textBright, 12);
                                    text(st, Pal::textPrimary, 11);
                                }
                                text(l1, Pal::textDim, 11);
                                text(l2, Pal::textDim, 11);
                            }
                            const float bw = (float)fs(11) * 6.0f;
                            CLAY(CLAY_IDI("SkBtns", (uint32_t)k), {.layout = {.sizing = {CLAY_SIZING_FIXED(bw), CLAY_SIZING_FIT(0)},
                                                                              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                              .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                            {
                                if (uiButton(inp, ui, "SkCardBtn", k, "Watch", K.tip, false, e != nullptr, true) && e)
                                    skPendingKind = k;
                            }
                        }
                    }
                    uiSection("SkSecUp", 1, "UPCOMING (NEXT 2 YEARS)");
                    // Always kUpcomingRows rows of one height: a row waiting for the search, or past the list's end, is
                    // a blank line, so nothing moves as the kinds come in.
                    const float rowH = std::max((float)fs(11) * 1.35f * 2.0f + 10.0f, 34.0f);
                    for (int i = 0; i < kUpcomingRows; ++i)
                    {
                        const sky::Event *e = i < (int)skUpcoming.size() ? &skUpcoming[(size_t)i] : nullptr;
                        CLAY(CLAY_IDI("SkRow", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(rowH)},
                                                                         .padding = {6, 6, 3, 3},
                                                                         .childGap = 8,
                                                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                         .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                              .backgroundColor = e && now >= e->t0 ? Pal::rowHighlight : Pal::cardBg,
                                                              .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            CLAY(CLAY_IDI("SkRowIcon", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_FIXED(20), CLAY_SIZING_FIXED(20)}},
                                                                      .image = {.imageData = (void *)(intptr_t)((e ? kSkyKinds[e->kind].icon : kIconSkyEvents) + 1)}}) {}
                            CLAY(CLAY_IDI("SkRowText", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                                                 .childGap = 1,
                                                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                                                      .clip = {.horizontal = true}})
                            {
                                if (e)
                                {
                                    text(skStr("%s", eventTitle(*e).c_str()), Pal::textPrimary, 11);
                                    text(skStr("%s  -  %s", fmtUtcReadable(e->t).c_str(),
                                               now >= e->t0 ? "happening now" : inText(e->t0 - now)),
                                         Pal::textDim, 11);
                                }
                                else
                                    text(skScanKind < sky::kKindCount ? "searching..." : " ", Pal::textDim, 11);
                            }
                            const float bw = (float)fs(11) * 6.0f;
                            CLAY(CLAY_IDI("SkRowBtns", (uint32_t)i), {.layout = {.sizing = {CLAY_SIZING_FIXED(bw), CLAY_SIZING_FIT(0)},
                                                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}}})
                            {
                                if (uiButton(inp, ui, "SkRowBtn", i, "Watch", e ? kSkyKinds[e->kind].tip : "", false, e != nullptr, true) && e)
                                    skPendingRow = i;
                            }
                        }
                    }
                }
                ui.scrollbar(CLAY_ID("SkList"));
            }
        });
    // After the layout: a Watch (it moves time and the observer; the lists are rebuilt next frame).
    if (skPendingKind >= 0 || skPendingRow >= 0)
    {
        const sky::Event *e = nullptr;
        if (skPendingKind >= 0) e = skyEventNext(skPendingKind);
        else if (skPendingRow < (int)skUpcoming.size()) e = &skUpcoming[(size_t)skPendingRow];
        if (e)
        {
            std::string msg;
            skyEventWatch(*e, msg);
            skStatus = msg;
        }
        skPendingKind = skPendingRow = -1;
    }
}

nlohmann::json SatelliteSim::skyEventsJson()
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    nlohmann::json j = {{"open", skChrome.open}, {"search_done", skScanKind >= sky::kKindCount}, {"restarts", skScanRestarts},
                        {"base_h_ago", (now - skScanBase) / 3600.0}};
    nlohmann::json cards = nlohmann::json::object(), ms = nlohmann::json::object(), counts = nlohmann::json::object();
    for (int k = 0; k < sky::kKindCount; ++k)
    {
        const sky::Event *e = skyEventNext(k);
        cards[kSkyKinds[k].id] = e ? eventJson(*e, now) : nlohmann::json(nullptr);
        ms[kSkyKinds[k].id] = skScanMs[k];
        counts[kSkyKinds[k].id] = (int)skFound[k].size();
    }
    j["next"] = cards;
    j["search_ms"] = ms;
    j["found"] = counts;
    nlohmann::json up = nlohmann::json::array();
    for (const sky::Event &e : skUpcoming) up.push_back(eventJson(e, now));
    j["upcoming"] = up;
    j["aim_kept"] = skAimA >= 0;
    return j;
}

// ── Harness: skyevents [state] | events | watch <solar|lunar|conjunction|occultation|row> [n=<k>] | list <kind> ──
harness::Status SatelliteSim::harnessSkyEvents(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    nlohmann::json &r = a.result;
    auto pos = [&](size_t i) -> std::string { return i < c.pos.size() ? c.pos[i] : std::string(); };
    std::string sub = pos(0);
    std::transform(sub.begin(), sub.end(), sub.begin(), [](unsigned char ch) { return (char)std::tolower(ch); });
    if (sub.empty() || sub == "state")
    {
        r = skyEventsJson();
        r["message"] = std::string("sky events, search ") + (skScanKind >= sky::kKindCount ? "done" : "running");
        return harness::Status::Done;
    }
    skyEventsFinish(60.0);
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    if (sub == "events")
    {
        r = skyEventsJson();
        r["message"] = std::string("sky events, search ") + (skScanKind >= sky::kKindCount ? "done" : "not done");
        return harness::Status::Done;
    }
    auto kindOf = [&](const std::string &s) {
        for (int k = 0; k < sky::kKindCount; ++k)
            if (s == kSkyKinds[k].id) return k;
        return -1;
    };
    if (sub == "list")
    {
        const int k = kindOf(pos(1));
        if (k < 0) throw std::runtime_error("skyevents list: solar | lunar | conjunction | occultation");
        nlohmann::json arr = nlohmann::json::array();
        for (const sky::Event &e : skFound[k]) arr.push_back(eventJson(e, now));
        r["events"] = arr;
        r["message"] = std::to_string(arr.size()) + " " + kSkyKinds[k].id + " events";
        return harness::Status::Done;
    }
    if (sub == "watch")
    {
        const std::string w = pos(1);
        const sky::Event *e = nullptr;
        const int k = kindOf(w);
        if (k >= 0)
        {
            const int n = (int)c.num("n", 0.0);
            int seen = 0;
            for (const sky::Event &ev : skFound[k])
                if (ev.t1 > now && seen++ == n) { e = &ev; break; }
        }
        else if (!w.empty() && std::isdigit((unsigned char)w[0]))
        {
            const int row = std::atoi(w.c_str());
            if (row < (int)skUpcoming.size()) e = &skUpcoming[(size_t)row];
        }
        else
            throw std::runtime_error("skyevents watch: solar | lunar | conjunction | occultation [n=<k>] | <upcoming row>");
        if (!e) throw std::runtime_error("skyevents watch: nothing there");
        const nlohmann::json ev = eventJson(*e, now);
        std::string msg;
        if (!skyEventWatch(*e, msg)) throw std::runtime_error("skyevents watch: " + msg);
        skStatus = msg;
        r["event"] = ev;
        r["lat_deg"] = obsLatDeg;
        r["lon_deg"] = obsLonDeg;
        r["az_deg"] = camera.azDeg;
        r["el_deg"] = camera.elDeg;
        r["fov_deg"] = camera.fovYDeg;
        r["sun_el_deg"] = glm::degrees(std::asin(std::clamp(sunDirENU.z, -1.0f, 1.0f)));
        r["moon_el_deg"] = glm::degrees(std::asin(std::clamp(moonDirENU.z, -1.0f, 1.0f)));
        r["sun_seen_fraction"] = moonEclipseSolarObs;
        r["selected_planet"] = selectedPlanetIndex >= 0 ? kPlanetNames[selectedPlanetIndex] : "";
        r["message"] = msg;
        return harness::Status::Done;
    }
    throw std::runtime_error("skyevents: state | events | list <kind> | watch <kind> [n=] | watch <row>");
}

// ── Comets (2026-10-27; Comets.h, SatelliteSimComets.cpp, CLAUDE.md "Comets") ──────────────────────────────────────
// Every comet of the catalog over the range (comet::catalogNear: pure, so the worker builds its own) at its best:
// the day the head's light and the tail's length score highest with the comet >= 20 deg from the Sun
// (SatelliteSim::cometBestWindow), the window while it stays within ~2.5 mag of that. Kept when the best is brighter
// than mag 8 (binoculars).
void SatelliteSim::skySearchComets(const sky::SearchCtx &c, std::vector<sky::Event> &out) const
{
    std::vector<comet::Elements> cat;
    comet::catalogNear(0.5 * (c.t0 + c.t1), 0.5 * (c.t1 - c.t0) / (365.25 * kDay) + 0.6, cat);
    for (const comet::Elements &el : cat)
    {
        if (c.cancelled()) return;
        if (!el.great && !c.cometsMinor) continue;
        const double a = std::max(c.t0, el.tPeri - 200.0 * kDay), b = std::min(c.t1, el.tPeri + 200.0 * kDay);
        if (b <= a) continue;
        double tb = 0, w0 = 0, w1 = 0, mb = 99, tl = 0;
        if (!cometBestWindow(el, a, b, tb, w0, w1, mb, tl) || mb > 8.0) continue;
        sky::Event e;
        e.kind = sky::kComet;
        e.t = tb;
        e.t0 = w0;
        e.t1 = w1;
        e.magA = (float)mb;
        e.sepDeg = tl;
        e.score = -mb;
        e.watchT = tb;
        e.tAux = el.tPeri;
        snprintf(e.label, sizeof(e.label), "%s", el.name);
        out.push_back(e);
    }
    std::sort(out.begin(), out.end(), [](const sky::Event &x, const sky::Event &y) { return x.t < y.t; });
}

bool SatelliteSim::skyWatchComet(const sky::Event &e, std::string &msg)
{
    const double now = (double)simDayJ2000 * kDay + simSecInDay;
    const double tw = (now >= e.t0 && now < e.t1) ? now : e.watchT;
    cometsRefreshCatalog(tw, true);
    const int idx = cometIndexByName(e.label);
    if (idx < 0) return false;
    return cometWatchAt(idx, tw, -12.0, false, msg);
}
