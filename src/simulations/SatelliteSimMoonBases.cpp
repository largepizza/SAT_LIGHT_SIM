// SatelliteSimMoonBases.cpp — the lunar south-pole bases' solar-array glints in the sky (2026-10-27; CLAUDE.md
// "Moon-base glints"; the model in LunarBases.h/.cpp, the bases in data/lunar_bases.json).
//
// Each frame (kesslerFillDust's finish, after the other kFireBuf users) every built base whose Sun is up is evaluated
// from the observer: a cheap aggregate first, the rows only where it can be seen at all (aggregate brighter than
// mag 14). A visible glint is a point source at the base's place on the DRAWN Moon (x "Moon size", the disc's own
// sphere: moonDir + moonAngR x the base's body vector), refracted like the stars, dimmed by the air along the line of
// sight, appended to kFireBuf as a city light (meshPx -1: no trail, the manual terrain test) so it gets the point,
// bloom and glare. Nothing per pixel; with the Moon below the horizon, or no glint, it costs a few geometry calls.
#include "../FrameProfile.h"
#include "../Harness.h"
#include "../Log.h"
#include "../Paths.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>

std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp

namespace
{
constexpr double kPi = 3.14159265358979323846;
double rad(double d) { return d * (kPi / 180.0); }
double deg(double r) { return r * (180.0 / kPi); }
glm::dvec3 eciToEcefMb(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return glm::dvec3(c * v.x + s * v.y, -s * v.x + c * v.y, v.z);
}
} // namespace

void SatelliteSim::moonBasesLoad()
{
    const std::string path = (std::filesystem::path(Paths::exeDir()) / "lunar_bases.json").string();
    std::string err;
    if (!lunar::loadBases(path, lunarBases_, err))
    {
        Log::line("moon bases: none (" + err + ")");
        lunarBases_.clear();
    }
    else
        Log::line("moon bases: " + std::to_string(lunarBases_.size()) + " loaded from " + path);
    moonBaseNow_.assign(lunarBases_.size(), MoonBaseNow());
}

bool SatelliteSim::moonBaseDrawnEnu(int i, glm::vec3 &enu) const
{
    if (i < 0 || i >= (int)lunarBases_.size()) return false;
    const lunar::Base &B = lunarBases_[(size_t)i];
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    glm::dvec3 P, X, Y;
    lunar::moonBodyFrame(t, P, X, Y);
    const glm::dvec3 n = lunar::bodyDir(B, P, X, Y) * (1.0 + B.elevM / lunar::kMoonRadiusM);
    const glm::dvec3 M = moonGeoEciAt(t);
    const glm::dvec3 tDir = glm::normalize(M - glm::dvec3(obsECI));
    // The drawn disc is a sphere of angular radius moonAngR about the Moon's direction (sat_sky.frag): the base's
    // place on it is moonDir + moonAngR x its body vector.
    const glm::dvec3 d = glm::normalize(tDir + (double)moonAngR * n);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    enu = glm::normalize(glm::vec3((float)glm::dot(d, ex), (float)glm::dot(d, ey), (float)glm::dot(d, ez)));
    if (refrEnabled && refrTable.valid)
    {
        glm::vec3 a = enu;
        float aoa, sc;
        if (refractEnu(a, aoa, sc)) enu = a;
    }
    return true;
}

void SatelliteSim::moonBasesFill()
{
    FPROF_SCOPE("moonBasesFill");
    if (moonBaseNow_.size() != lunarBases_.size()) moonBaseNow_.assign(lunarBases_.size(), MoonBaseNow());
    for (MoonBaseNow &m : moonBaseNow_) m = MoonBaseNow();
    if (!moonBaseGlints || lunarBases_.empty() || !kFireMapped) return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    glm::dvec3 P, X, Y;
    lunar::moonBodyFrame(t, P, X, Y);
    const glm::dvec3 M = moonGeoEciAt(t);
    const glm::dvec3 eye(obsECI);
    const double dayness = glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    for (size_t i = 0; i < lunarBases_.size(); ++i)
    {
        const lunar::Base &B = lunarBases_[i];
        MoonBaseNow &now = moonBaseNow_[i];
        now.built = lunar::builtShare(B, t) > 0.0;
        if (!now.built) continue;
        const lunar::Geometry g = lunar::geometryAt(B, t, M, P, X, Y);
        now.sunElevDeg = g.sunElevDeg;
        now.sunVisible = g.sunVisible;
        now.sunUp = g.sunVisible > 0.0;
        if (!now.sunUp) continue;
        // The aggregate first: the rows only when the glint can be seen at all.
        const lunar::Glint ga = lunar::glintAggregate(B, g, t, eye, moonBaseJitter);
        now.obsElevDeg = ga.obsElevDeg;
        now.phaseDeg = ga.phaseDeg;
        now.beamOffDeg = ga.beamOffDeg;
        if (ga.mag > 14.0) continue;
        const lunar::Glint gr = lunar::glintRows(B, g, t, eye, moonBaseJitter);
        now.magTop = gr.mag;
        now.rowsLit = gr.rowsLit;
        if (gr.mag > 20.0) continue;
        glm::vec3 enu;
        if (!moonBaseDrawnEnu((int)i, enu)) continue;
        now.enu = enu;
        const glm::dvec3 de = glm::normalize(M - eye); // (the line of sight through the air)
        now.extinction = atmExtinctionMag(eye, de, 0.0, (double)extinctionCoeff);
        now.magSeen = gr.mag + now.extinction;
        // effectFlare (the 0.008 <-> mag 6 anchor), dimmed by day like the pads' lights; below the apparent horizon
        // the manual terrain test hides it.
        now.flare = std::min(0.008 * std::pow(10.0, 0.4 * (6.0 - now.magSeen)) * (1.0 - 0.9 * dayness), 60.0);
        if (now.flare < 2e-4 || kFireCount >= kFireballMax) continue;
        fo[kFireCount++] = {glm::vec4(enu, (float)now.flare), glm::vec4(B.tint, (float)(0.97 * moonDistM))};
        now.drawn = true;
    }
    static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;
}

// ── Harness: moonbase [state] | windows [days=] [limit=] [jitter=] | check ─────────────────────────────────────────
harness::Status SatelliteSim::harnessMoonBase(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    nlohmann::json &r = a.result;
    const std::string sub = c.pos.empty() ? std::string("state") : c.pos[0];
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    auto siteJson = [](const glm::dvec3 &e) {
        return nlohmann::json{{"lat", deg(std::asin(std::clamp(e.z, -1.0, 1.0)))}, {"lon", deg(std::atan2(e.y, e.x))}};
    };
    auto windowJson = [&](const lunar::Window &w) {
        nlohmann::json o = {{"base", lunarBases_[(size_t)w.base].shortName},
                            {"peak_utc", fmtUtcReadable(w.tPeak)},
                            {"start_utc", fmtUtcReadable(w.t0)},
                            {"end_utc", fmtUtcReadable(w.t1)},
                            {"hours", (w.t1 - w.t0) / 3600.0},
                            {"in_days", (w.tPeak - t) / 86400.0},
                            {"peak_mag", w.peakMag},
                            {"moon_phase_angle_deg", w.moonPhaseDeg},
                            {"moon_alt_deg", w.site.moonAltDeg},
                            {"sun_alt_deg", w.site.sunAltDeg}};
        o["site"] = siteJson(w.site.ecef);
        return o;
    };
    if (sub == "state")
    {
        // (the values of the last frame drawn: `wait 2` after a jump)
        nlohmann::json arr = nlohmann::json::array();
        for (size_t i = 0; i < lunarBases_.size(); ++i)
        {
            const lunar::Base &B = lunarBases_[i];
            const MoonBaseNow &m = moonBaseNow_[i];
            nlohmann::json o = {{"name", B.name},
                                {"lat_deg", B.latDeg},
                                {"lon_deg", B.lonDeg},
                                {"built_share", lunar::builtShare(B, t)},
                                {"area_km2", B.areaM2 * 1e-6},
                                {"rows", B.rows},
                                {"sun_elev_deg", m.sunElevDeg},
                                {"sun_visible", m.sunVisible},
                                {"observer_elev_deg_at_base", m.obsElevDeg},
                                {"phase_angle_deg", m.phaseDeg},
                                {"beam_off_deg", m.beamOffDeg},
                                {"mag_above_air", m.magTop},
                                {"mag_seen", m.magSeen},
                                {"extinction_mag", m.extinction},
                                {"rows_lit", m.rowsLit},
                                {"flare", m.flare},
                                {"drawn", m.drawn}};
            glm::vec3 enu;
            if (moonBaseDrawnEnu((int)i, enu))
            {
                o["az_deg"] = deg(std::atan2((double)enu.x, (double)enu.y));
                o["el_deg"] = deg(std::asin(std::clamp((double)enu.z, -1.0, 1.0)));
                const glm::vec3 md = glm::normalize(glm::vec3(moonDirENU));
                o["from_moon_centre_deg"] = deg(std::acos(std::clamp((double)glm::dot(md, enu), -1.0, 1.0)));
            }
            lunar::Site s;
            if (lunar::bestSite(B, t, moonBaseJitter, 0.0, s, 10.0, -6.0, true))
            {
                o["best_site_now"] = siteJson(s.ecef);
                o["best_site_now"]["mag"] = s.mag;
                o["best_site_now"]["moon_alt_deg"] = s.moonAltDeg;
                o["best_site_now"]["sun_alt_deg"] = s.sunAltDeg;
            }
            else
                o["best_site_now"] = nullptr;
            std::vector<lunar::Window> w;
            const std::vector<lunar::Base> one(1, B);
            lunar::findWindows(one, t - 6.0 * 3600.0, t + 90.0 * 86400.0, moonBaseMagLimit, moonBaseJitter, 0.0, nullptr,
                               nullptr, w);
            o["next_window"] = nullptr;
            for (lunar::Window &x : w)
                if (x.t1 > t)
                {
                    x.base = (int)i;
                    o["next_window"] = windowJson(x);
                    break;
                }
            arr.push_back(o);
        }
        r["bases"] = arr;
        r["enabled"] = moonBaseGlints;
        r["jitter_scale"] = moonBaseJitter;
        r["event_mag_limit"] = moonBaseMagLimit;
        r["moon_drawn_radius_deg"] = deg((double)moonAngR);
        r["message"] = std::to_string(lunarBases_.size()) + " moon bases";
        return harness::Status::Done;
    }
    if (sub == "windows")
    {
        const double days = c.num("days", 730.0), lim = c.num("limit", (double)moonBaseMagLimit),
                     jit = c.num("jitter", (double)moonBaseJitter);
        std::vector<lunar::Window> w;
        lunar::findWindows(lunarBases_, t, t + days * 86400.0, lim, jit, 0.0, nullptr, nullptr, w);
        nlohmann::json arr = nlohmann::json::array();
        for (const lunar::Window &x : w) arr.push_back(windowJson(x));
        r["windows"] = arr;
        r["message"] = std::to_string(w.size()) + " windows in " + std::to_string((int)days) + " days";
        return harness::Status::Done;
    }
    if (sub == "check")
    {
        // One flat panel aimed at a point 384,400 km away vs the hand formula (as tools/lunar_glint_tool).
        lunar::Base b;
        b.rows = 1;
        b.areaM2 = 1000.0;
        b.rowTiltSigmaDeg = b.rowAzSigmaDeg = 0.0;
        b.panelFlatnessDeg = 1e-4;
        b.horizonDipDeg = 5.0;
        b.onlineT = -1e12;
        b.fullT = -1e11;
        lunar::Geometry g;
        g.sunDistM = 1.495978707e11;
        g.sunAngR = std::asin(6.96e8 / g.sunDistM);
        g.sunDir = glm::dvec3(std::cos(rad(0.8)), 0.0, std::sin(rad(0.8)));
        const glm::dvec3 rb = lunar::meanBeam(b, g);
        const double D = 3.844e8;
        const lunar::Glint gl = lunar::glintRows(b, g, 0.0, rb * D);
        const double ci = glm::dot(glm::normalize(rb + g.sunDir), g.sunDir);
        const double F = 0.04 + 0.96 * std::pow(1.0 - ci, 5.0);
        const double hand = -26.74 - 2.5 * std::log10(F * b.areaM2 * ci / (kPi * g.sunAngR * g.sunAngR * D * D));
        r["model_mag"] = gl.mag;
        r["hand_mag"] = hand;
        r["diff"] = gl.mag - hand;
        r["message"] = std::string("panel check ") + (std::abs(gl.mag - hand) < 0.01 ? "ok" : "FAIL");
        return harness::Status::Done;
    }
    throw std::runtime_error("moonbase: state | windows [days=] [limit=] [jitter=] | check");
}
