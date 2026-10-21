// Launches, the app-independent half (see Launches.h; plan .plans/LAUNCHES_PLAN.md).
#include "Launches.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

#include <nlohmann/json.hpp>

namespace launch
{
double hash01(uint64_t seed, uint64_t index, uint32_t channel)
{
    uint64_t h = seed * 0x9E3779B97F4A7C15ull ^ (index + 0x632BE59BD9B4E019ull) * 0xC2B2AE3D27D4EB4Full ^
                 ((uint64_t)channel + 1ull) * 0x165667B19E3779F9ull;
    h ^= h >> 31;
    h *= 0xBF58476D1CE4E5B9ull;
    h ^= h >> 27;
    h *= 0x94D049BB133111EBull;
    h ^= h >> 33;
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}

// ── Dates and the deployment curve ──────────────────────────────────────────────────────────────
double dateToJ2000(const std::string &s)
{
    int y = 0, mo = 0, d = 0, hh = 0, mm = 0, ss = 0;
    const int n = std::sscanf(s.c_str(), "%d-%d-%d%*[T ]%d:%d:%d", &y, &mo, &d, &hh, &mm, &ss);
    if (n < 3 || mo < 1 || mo > 12 || d < 1 || d > 31)
        return std::nan("");
    // Days from civil (Howard Hinnant): days since 1970-01-01.
    const int yy = y - (mo <= 2 ? 1 : 0);
    const int era = (yy >= 0 ? yy : yy - 399) / 400;
    const unsigned yoe = (unsigned)(yy - era * 400);
    const unsigned doy = (153u * (unsigned)(mo + (mo > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)d - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long long days1970 = (long long)era * 146097 + (long long)doe - 719468;
    const long long days2000 = days1970 - 10957; // 2000-01-01
    // J2000 = 2000-01-01 12:00 (the sim's UTC time base).
    return (double)days2000 * 86400.0 - 43200.0 + hh * 3600.0 + mm * 60.0 + ss;
}

static double sigm(double x) { return 1.0 / (1.0 + std::exp(-x)); }

void SCurve::fit(double start, double complete, double steep, double tRef, double fRef)
{
    S = start;
    C = std::max(complete, start + 86400.0);
    k = std::max(steep, 0.5);
    m = 0.5;
    const double x = (tRef - S) / (C - S);
    if (x <= 0.0 || x >= 1.0 || !(fRef > 0.0) || fRef >= 1.0)
        return;
    const double f = std::clamp(fRef, 1e-4, 1.0 - 1e-4);
    // f(x; m) falls as m rises: bisect m over a wide range.
    double lo = -1.5, hi = 2.5;
    for (int i = 0; i < 80; ++i)
    {
        m = 0.5 * (lo + hi);
        (frac(tRef) > f ? lo : hi) = m;
    }
    m = 0.5 * (lo + hi);
}

double SCurve::frac(double t) const
{
    const double x = (t - S) / (C - S);
    if (x <= 0.0)
        return 0.0;
    if (x >= 1.0)
        return 1.0;
    const double a = sigm(-k * m), b = sigm(k * (1.0 - m));
    return (sigm(k * (x - m)) - a) / std::max(b - a, 1e-12);
}

double SCurve::inv(double f) const
{
    f = std::clamp(f, 0.0, 1.0);
    const double a = sigm(-k * m), b = sigm(k * (1.0 - m));
    const double s = std::clamp(a + f * (b - a), 1e-15, 1.0 - 1e-15);
    const double x = m + std::log(s / (1.0 - s)) / k;
    return S + std::clamp(x, 0.0, 1.0) * (C - S);
}

// ── Catalog ──────────────────────────────────────────────────────────────────────────────────────
bool Catalog::load(const std::string &path, std::string &err)
{
    sites.clear();
    vehicles.clear();
    programs.clear();
    history.clear();
    std::ifstream f(path);
    if (!f)
    {
        err = "cannot open " + path;
        return false;
    }
    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const std::exception &e)
    {
        err = std::string("parse error: ") + e.what();
        return false;
    }
    try
    {
        for (const auto &s : j.at("sites"))
        {
            Site st;
            st.id = s.at("id").get<std::string>();
            st.name = s.value("name", st.id);
            st.latDeg = s.at("lat").get<double>();
            st.lonDeg = s.at("lon").get<double>();
            if (s.contains("azimuth"))
            {
                st.azMinDeg = s["azimuth"][0].get<double>();
                st.azMaxDeg = s["azimuth"][1].get<double>();
            }
            st.padKind = s.value("pad_kind", -1);
            if (s.contains("online"))
            {
                st.onlineT = dateToJ2000(s["online"].get<std::string>());
                if (std::isnan(st.onlineT))
                {
                    err = "site " + st.id + ": bad online date";
                    return false;
                }
            }
            st.fictional = s.value("fictional", false);
            if (s.contains("online_for"))
                for (auto it = s["online_for"].begin(); it != s["online_for"].end(); ++it)
                {
                    const double tv = dateToJ2000(it.value().get<std::string>());
                    if (std::isnan(tv))
                    {
                        err = "site " + st.id + ": bad online_for date";
                        return false;
                    }
                    st.onlineFor.push_back({it.key(), tv});
                }
            if (s.contains("pad_history"))
            {
                for (const auto &e : s["pad_history"])
                {
                    const double tv = dateToJ2000(e.at(0).get<std::string>());
                    if (std::isnan(tv))
                    {
                        err = "site " + st.id + ": bad pad_history date";
                        return false;
                    }
                    st.padHistory.push_back({tv, std::clamp(e.at(1).get<int>(), -1, 2)});
                }
                std::sort(st.padHistory.begin(), st.padHistory.end());
            }
            if (s.contains("pad_heading_deg"))
                st.padHeadingDeg = s["pad_heading_deg"].get<double>();
            const double la = glm::radians(st.latDeg), lo = glm::radians(st.lonDeg);
            st.ecef = glm::dvec3(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
            sites.push_back(st);
        }
        auto siteIdx = [&](const std::string &id) {
            for (size_t i = 0; i < sites.size(); ++i)
                if (sites[i].id == id)
                    return (int)i;
            return -1;
        };
        for (const auto &v : j.at("vehicles"))
        {
            Vehicle vh;
            vh.id = v.at("id").get<std::string>();
            vh.name = v.value("name", vh.id);
            for (const auto &s : v.at("sites"))
            {
                const int k = siteIdx(s.get<std::string>());
                if (k < 0)
                {
                    err = "vehicle " + vh.id + ": unknown site " + s.get<std::string>();
                    return false;
                }
                vh.sites.push_back(k);
            }
            vh.t1 = v.value("meco_s", vh.t1);
            vh.h1 = v.value("meco_alt_km", vh.h1 / 1000.0) * 1000.0;
            vh.x1 = v.value("meco_downrange_km", vh.x1 / 1000.0) * 1000.0;
            vh.t2 = v.value("seco_s", vh.t2);
            vh.deployAfter = v.value("deploy_after_seco_s", vh.deployAfter);
            vh.deployIntervalS = std::max(v.value("deploy_interval_s", vh.deployIntervalS), 0.0);
            vh.sepSpeedMps = std::max(v.value("sep_speed_mps", vh.sepSpeedMps), 0.0);
            vh.sepSpreadMps = std::max(v.value("sep_spread_mps", vh.sepSpreadMps), 0.0);
            vh.flow1 = v.value("flow1_kg_s", vh.flow1);
            vh.flow2 = v.value("flow2_kg_s", vh.flow2);
            vh.vex1 = v.value("exhaust1_m_s", vh.vex1);
            vh.vex2 = v.value("exhaust2_m_s", vh.vex2);
            const std::string rec = v.value("booster", std::string("droneship"));
            vh.recovery = rec == "expendable" ? 0 : (rec == "return" || rec == "tower") ? 1 : 2;
            vh.flameMag1 = v.value("flame1_mag_100km", vh.flameMag1);
            vh.flameMag2 = v.value("flame2_mag_100km", vh.flameMag2);
            vh.upperDeorbit = v.value("upper_stage_deorbit", vh.upperDeorbit);
            vh.upperMassKg = v.value("upper_stage_mass_kg", vh.upperMassKg);
            vh.soot = v.value("soot", vh.soot);
            vh.ventChance = v.value("vent_chance", vh.ventChance);
            vh.mountM = std::max(v.value("mount_m", vh.mountM), 0.0);
            vh.catchM = std::max(v.value("catch_m", vh.catchM), 0.0);
            vh.fairingSepKm = std::max(v.value("fairing_sep_km", vh.fairingSepKm), 0.0);
            vh.payloadBaseM = v.value("payload_base_m", vh.payloadBaseM);
            if (v.contains("fairing_center_m"))
                vh.fairingCenter = glm::dvec3(v["fairing_center_m"][0].get<double>(), v["fairing_center_m"][1].get<double>(),
                                              v["fairing_center_m"][2].get<double>());
            vh.doorM = v.value("door_m", vh.doorM);
            vh.spacecraftDockM = v.value("spacecraft_dock_m", vh.spacecraftDockM);
            vh.droneDistKm = std::max(v.value("droneship_km", vh.droneDistKm), 0.0);
            vh.rtlsMaxKg = std::max(v.value("rtls_max_kg", vh.rtlsMaxKg), 0.0);
            if (v.contains("landing_ships"))
                for (const auto &ls : v["landing_ships"])
                {
                    Vehicle::LandingShip sh;
                    sh.name = ls.value("name", std::string("droneship"));
                    sh.model = ls.value("model", std::string("ship_droneship"));
                    sh.deckM = ls.value("deck_m", sh.deckM);
                    if (ls.contains("sites"))
                        for (const auto &s : ls["sites"])
                        {
                            const int k = siteIdx(s.get<std::string>());
                            if (k < 0)
                            {
                                err = "vehicle " + vh.id + ": landing ship " + sh.name + ": unknown site " + s.get<std::string>();
                                return false;
                            }
                            sh.sites.push_back(k);
                        }
                    vh.ships.push_back(sh);
                }
            if (v.contains("online"))
            {
                vh.onlineT = dateToJ2000(v["online"].get<std::string>());
                if (std::isnan(vh.onlineT))
                {
                    err = "vehicle " + vh.id + ": bad online date";
                    return false;
                }
            }
            vehicles.push_back(vh);
        }
        auto vehIdx = [&](const std::string &id) {
            for (size_t i = 0; i < vehicles.size(); ++i)
                if (vehicles[i].id == id)
                    return (int)i;
            return -1;
        };
        for (const auto &p : j.at("programs"))
        {
            Program pg;
            pg.name = p.at("name").get<std::string>();
            for (const auto &m : p.at("match"))
                pg.match.push_back(m.get<std::string>());
            for (const auto &v : p.at("vehicles"))
            {
                const int k = vehIdx(v.get<std::string>());
                if (k < 0)
                {
                    err = "program " + pg.name + ": unknown vehicle " + v.get<std::string>();
                    return false;
                }
                pg.vehicles.push_back(k);
            }
            pg.lifeYears = p.value("life_years", pg.lifeYears);
            pg.payload = std::clamp(p.value("payload", pg.payload), 1, 64);
            pg.parkKm = p.value("park_km", pg.parkKm);
            if (p.contains("raise_days"))
            {
                pg.raiseDays[0] = p["raise_days"][0].get<double>();
                pg.raiseDays[1] = p["raise_days"][1].get<double>();
            }
            if (p.contains("deorbit_days"))
            {
                pg.deorbitDays[0] = p["deorbit_days"][0].get<double>();
                pg.deorbitDays[1] = p["deorbit_days"][1].get<double>();
            }
            pg.batchGapDays = p.value("batch_gap_days", pg.batchGapDays);
            if (p.contains("payload_mass_kg"))
                pg.payloadMassKg = p["payload_mass_kg"].get<double>();
            else if (p.contains("sat_mass_kg"))
                pg.payloadMassKg = p["sat_mass_kg"].get<double>() * (double)pg.payload;
            if (p.contains("crew"))
            {
                const auto &cr = p["crew"];
                pg.crew = true;
                pg.spacecraft = cr.value("spacecraft", std::string("crew capsule"));
                pg.people = cr.value("people", 3);
                pg.crewModel = cr.value("model", std::string());
                pg.crewLands = cr.value("lands", false);
            }
            if (p.contains("stack"))
            {
                const auto &js = p["stack"];
                Stack &st = pg.stack;
                const std::string lay = js.value("layout", std::string("flat"));
                st.layout = lay == "pez" ? Stack::Pez : lay == "column" ? Stack::Column : Stack::Flat;
                st.perTier = std::clamp(js.value("per_tier", 1), 1, 12);
                st.sideX = js.value("side_axis", std::string("y")) == "x";
                st.pitchM = std::max(js.value("pitch_m", st.pitchM), 0.05);
                st.offsetM = js.value("offset_m", st.offsetM);
                st.baseM = js.value("base_m", st.baseM);
                st.columnRadiusM = js.value("column_radius_m", st.columnRadiusM);
                if (js.contains("member_center"))
                    st.center = glm::dvec3(js["member_center"][0].get<double>(), js["member_center"][1].get<double>(),
                                           js["member_center"][2].get<double>());
                st.dispenser = js.value("dispenser", std::string());
                st.intervalS = js.value("deploy_interval_s", st.intervalS);
                st.sepMps = js.value("sep_speed_mps", st.sepMps);
                st.spreadMps = js.value("sep_spread_mps", st.spreadMps);
            }
            if (p.contains("deployment"))
            {
                const auto &jd = p["deployment"];
                std::vector<nlohmann::json> blocks;
                if (jd.is_array())
                    for (const auto &b : jd)
                        blocks.push_back(b);
                else
                    blocks.push_back(jd);
                for (const auto &b : blocks)
                {
                    Deployment dp;
                    if (b.contains("match"))
                        for (const auto &m : b["match"])
                            dp.match.push_back(m.get<std::string>());
                    dp.startT = dateToJ2000(b.at("start").get<std::string>());
                    dp.completeT = dateToJ2000(b.at("complete").get<std::string>());
                    if (std::isnan(dp.startT) || std::isnan(dp.completeT) || dp.completeT <= dp.startT)
                    {
                        err = "program " + pg.name + ": deployment needs start < complete (YYYY-MM-DD)";
                        return false;
                    }
                    dp.present = std::max(b.value("present_2026", 0.0), 0.0);
                    dp.steep = b.value("steepness", dp.steep);
                    dp.stagger = std::max(b.value("shell_stagger", dp.stagger), 0.0);
                    if (b.contains("early_vehicles"))
                    {
                        for (const auto &v : b["early_vehicles"])
                        {
                            const int k = vehIdx(v.get<std::string>());
                            if (k < 0)
                            {
                                err = "program " + pg.name + ": unknown early vehicle " + v.get<std::string>();
                                return false;
                            }
                            dp.earlyVehicles.push_back(k);
                        }
                        dp.earlyUntilT = dateToJ2000(b.value("early_until", std::string("2000-01-01")));
                    }
                    pg.deploy.push_back(dp);
                }
            }
            programs.push_back(pg);
        }
        referenceT = dateToJ2000(j.value("deployment_reference", std::string("2026-10-20")));
        minFirstLifeFrac = std::clamp(j.value("min_first_life", minFirstLifeFrac), 0.0, 2.0);
        if (j.contains("roster_history"))
            for (const auto &h : j["roster_history"])
            {
                RosterHistory rh;
                for (const auto &m : h.at("match"))
                    rh.match.push_back(m.get<std::string>());
                rh.fraction = h.value("fraction", false);
                rh.benchmark = h.value("benchmark", false);
                if (h.contains("born"))
                {
                    // A single object (a station): there from this date.
                    const double tb = dateToJ2000(h["born"].get<std::string>());
                    if (std::isnan(tb))
                    {
                        err = "roster_history: bad born date";
                        return false;
                    }
                    rh.count = {{tb - 1.0, 0.0}, {tb, 1e9}};
                    if (h.contains("gone"))
                    {
                        const double tg = dateToJ2000(h["gone"].get<std::string>());
                        rh.count.push_back({tg, 1e9});
                        rh.count.push_back({tg + 1.0, 0.0});
                    }
                }
                else
                    for (const auto &c : h.at("count"))
                    {
                        const double tc = dateToJ2000(c.at(0).get<std::string>());
                        if (std::isnan(tc))
                        {
                            err = "roster_history: bad date";
                            return false;
                        }
                        rh.count.push_back({tc, c.at(1).get<double>()});
                    }
                std::sort(rh.count.begin(), rh.count.end());
                history.push_back(rh);
            }
    }
    catch (const std::exception &e)
    {
        err = std::string("schema: ") + e.what();
        return false;
    }
    return true;
}

// ── Tracks ───────────────────────────────────────────────────────────────────────────────────────
glm::dvec2 Track::at(double t, glm::dvec2 *vel, uint8_t *b) const
{
    if (p.empty())
        return glm::dvec2(0.0);
    if (p.size() == 1)
    {
        if (vel)
            *vel = v[0];
        if (b)
            *b = burn[0];
        return p[0];
    }
    const double f = std::clamp(t / dt, 0.0, (double)(p.size() - 1));
    const size_t i = std::min((size_t)f, p.size() - 2);
    const double a = f - (double)i;
    if (vel)
        *vel = glm::mix(v[i], v[i + 1], a);
    if (b)
        *b = burn[a < 0.5 ? i : i + 1];
    return glm::mix(p[i], p[i + 1], a);
}

double airDensity(double hM)
{
    // Piecewise exponential (US Standard Atmosphere-like), smooth enough for drag and entrainment.
    const double h = std::max(hM, 0.0) / 1000.0;
    if (h < 25.0)
        return 1.225 * std::exp(-h / 7.6);
    if (h < 75.0)
        return 1.225 * std::exp(-25.0 / 7.6) * std::exp(-(h - 25.0) / 6.4);
    if (h < 150.0)
        return 1.225 * std::exp(-25.0 / 7.6) * std::exp(-50.0 / 6.4) * std::exp(-(h - 75.0) / 7.4);
    return 1.225 * std::exp(-25.0 / 7.6) * std::exp(-50.0 / 6.4) * std::exp(-75.0 / 7.4) * std::exp(-(h - 150.0) / 25.0);
}

// The booster's flight: the ascent's closed form to MECO, then integrated (gravity, drag, the burns).
// recovery: 0 expendable, 1 back to the pad (x = 0; a tower catch stops catch_m above the mount), 2 downrange: a short
// retrograde trim burn (when the ballistic arc overshoots) puts it down droneDistKm out, on a deck hLand high.
Track boosterTrack(const Vehicle &vh, int recovery, double hLand)
{
    const int rec = recovery < 0 ? vh.recovery : recovery;
    if (hLand < 0.0)
        hLand = rec == 2 ? (vh.ships.empty() ? 3.0 : vh.ships[0].deckM) : rec == 1 ? vh.catchM : 0.0;
    Track tr;
    tr.dt = 0.5;
    const double t1 = vh.t1, h1 = vh.h1, x1 = vh.x1;
    // Ascent: h = h1 s^2, x = x1 s^3 (vertical off the pad, pitching over).
    for (double t = 0.0; t < t1; t += tr.dt)
    {
        const double s = t / t1;
        tr.p.push_back({x1 * s * s * s, h1 * s * s});
        tr.v.push_back({3.0 * x1 * s * s / t1, 2.0 * h1 * s / t1});
        tr.burn.push_back(BURN_ASCENT);
    }
    const glm::dvec2 p1(x1, h1), v1(3.0 * x1 / t1, 2.0 * h1 / t1);
    // After MECO: integrated. The boostback's target speed is found by a few shots (the landing x -> the target).
    auto fly = [&](double vBack, bool doBoost, Track *out, double glideTarget) {
        glm::dvec2 p = p1, v = v1;
        bool boost = doBoost, entryDone = false, landing = false;
        double entryT = -1.0;
        double lt0 = 0.0, lT = 1.0;  // the landing burn: its start and length
        glm::dvec2 lp0(0.0), lv0(0.0);
        const double dt = 0.05;
        double t = t1;
        double rec2 = t1;
        // Drag (2026-10-20, issue 20261020_213739 "Rockets land tilted"): a returning booster falls engines first with
        // a ballistic coefficient m / (Cd A) ~2200 kg/m^2 (a ~30-t Falcon 9 core, 3.7 m across: terminal ~190 m/s at sea
        // level). It was 280 m/s terminal (~half the real drag) and with no lift the booster reached 8 km still crossing
        // the ground at 650-1000 m/s, so its landing burn leaned 35-50 degrees.
        const double g = 9.81, k = 1.225 / (2.0 * 2200.0);
        // The aerodynamic descent after the entry burn: the grid fins and the body at angle of attack give lift up to
        // kGlideLD x the drag, steered (time constant kGlideTau) to the horizontal speed that reaches the landing point
        // in the time left (the target; none: to zero). A real booster arrives over its target nearly vertical: a few
        // degrees of lean in the last kilometre, then the landing burn's own small divert.
        const double kGlideLD = 0.6, kGlideTau = 3.0, kGlideLead = 8.0;
        const bool glideToX = rec == 1 || (rec == 2 && glideTarget >= 0.0);
        const double glideX = rec == 1 ? 0.0 : glideTarget;
        for (int it = 0; it < 200000; ++it)
        {
            uint8_t b = BURN_NONE;
            glm::dvec2 a(0.0, -g);
            const double spd = glm::length(v);
            a -= k * (airDensity(p.y) / 1.225) * spd * v; // drag
            if (rec == 0)
            {
                // Expendable: falls away; drawn until it breaks up low in the atmosphere.
                if (p.y < 30000.0 && v.y < 0.0)
                    break;
            }
            else
            {
                if (boost && t > t1 + 12.0)
                {
                    // Boostback (RTLS) or a droneship mission's trim: horizontal thrust toward the target speed.
                    const double dv = vBack - v.x;
                    if (std::abs(dv) < 1.0)
                        boost = false;
                    else
                    {
                        a.x += glm::sign(dv) * 30.0;
                        b = BURN_BOOSTBACK;
                    }
                }
                if (!entryDone && v.y < 0.0 && p.y < 60000.0)
                {
                    if (entryT < 0.0)
                        entryT = t;
                    // (22 s, longer while still fast: up to 40 s above 900 m/s, a droneship booster arriving from far downrange)
                    if (t - entryT < 22.0 || (spd > 900.0 && t - entryT < 40.0))
                    {
                        a -= 28.0 * v / std::max(spd, 1.0);
                        b = BURN_ENTRY;
                    }
                    else
                        entryDone = true;
                }
                if (entryDone && !landing && v.y < 0.0)
                {
                    const double hgo = std::max(p.y - hLand, 0.0);
                    const double tgo = std::max(hgo / std::max(-v.y, 50.0), 1.0);
                    const double vDes = glideToX ? (glideX - p.x) / (tgo + kGlideLead) : 0.0;
                    const double lim = kGlideLD * k * (airDensity(p.y) / 1.225) * spd * spd;
                    a.x += std::clamp((vDes - v.x) / kGlideTau, -lim, lim);
                }
                // Landing burn (2026-10-20): a constant deceleration that brings BOTH components of the velocity to
                // zero together at hC (a tower catch stops hC above the mount, a droneship on its deck), started when
                // that deceleration reaches aL (from 8 km down: above it the air has yet to slow the booster; a
                // faster arrival takes a harder burn, ~20 s from 8 km). The first cut flew a vertical suicide-burn profile with a 300-m margin
                // and an independent horizontal damping: a booster arriving at ~900 m/s down and ~1300 m/s across (a
                // droneship's: the entry burn removes ~600 m/s) touched down at ~40 m/s down and ~360 m/s across.
                const double aL = 25.0;
                const double kTouchV = 1.5; // touchdown speed (m/s): a real landing burn ends at a metre or two a second
                const double hC = hLand;
                if (!landing && entryDone && v.y < 0.0 && p.y > hC && p.y - hC < 8000.0)
                {
                    const double h = p.y - hC;
                    if (std::max(v.y * v.y, std::abs(v.x * v.y)) >= 2.0 * aL * h)
                    {
                        landing = true;
                        lt0 = t;
                        lp0 = p;
                        lv0 = v;
                        lT = 2.0 * h / std::max(-v.y + kTouchV, 1.0);
                    }
                }
                if (landing)
                {
                    // Analytic: p = p0 + v0 s + a s^2 / 2 with a = (vE - v0) / T, vE = (0, -kTouchV): it touches down on
                    // hC at kTouchV straight down (p0.y + (v0.y + vE.y) T / 2 = hC), the horizontal speed nulled.
                    const glm::dvec2 vE(0.0, -kTouchV);
                    const glm::dvec2 aB = (vE - lv0) / lT;
                    const double sN = std::min(t + dt - lt0, lT);
                    p = lp0 + lv0 * sN + 0.5 * aB * (sN * sN);
                    v = lv0 + aB * sN;
                    t += dt;
                    if (sN >= lT)
                    {
                        if (out)
                        {
                            out->landed = true;
                            out->p.push_back({p.x, hC});
                            out->v.push_back({0.0, 0.0});
                            out->burn.push_back(BURN_NONE);
                        }
                        return p.x;
                    }
                    if (out && t >= rec2 + out->dt - 1e-9)
                    {
                        rec2 += out->dt;
                        out->p.push_back(p);
                        out->v.push_back(v);
                        out->burn.push_back(BURN_LANDING);
                    }
                    continue;
                }
            }
            v += a * dt;
            p += v * dt;
            t += dt;
            if (p.y <= 0.0)
                break;
            if (out && t >= rec2 + out->dt - 1e-9)
            {
                rec2 += out->dt;
                out->p.push_back(p);
                out->v.push_back(v);
                out->burn.push_back(b);
            }
        }
        return p.x;
    };
    double vBack = 0.0, target = 0.0;
    bool doBoost = false;
    if (rec == 1)
    {
        // Secant on the boostback speed for a landing at x = 0 (the pad / the tower / its landing zone).
        doBoost = true;
        double va = -0.25 * v1.x, vb = -0.6 * v1.x;
        double fa = fly(va, true, nullptr, 0.0), fb = fly(vb, true, nullptr, 0.0);
        for (int i = 0; i < 12 && std::abs(fb) > 50.0 && std::abs(fb - fa) > 1e-6; ++i)
        {
            const double vc = vb - fb * (vb - va) / (fb - fa);
            va = vb;
            fa = fb;
            vb = std::clamp(vc, -3.0 * v1.x, v1.x);
            fb = fly(vb, true, nullptr, 0.0);
        }
        vBack = vb;
    }
    else if (rec == 2)
    {
        // No boostback when the arc falls short of the ship's station (the ship waits where the booster comes down);
        // else a short retrograde trim, secant-solved on the speed it leaves the booster with (2026-10-20: it aimed
        // a boostback at 0.55 x the MECO speed that was never burned).
        const double xBal = fly(v1.x, false, nullptr, -1.0);
        target = vh.droneDistKm * 1000.0;
        if (target > 0.0 && xBal > target + 200.0)
        {
            doBoost = true;
            double va = v1.x, fa = xBal - target;
            double vb = v1.x * target / xBal, fb = fly(vb, true, nullptr, target) - target;
            for (int i = 0; i < 14 && std::abs(fb) > 20.0 && std::abs(fb - fa) > 1e-6; ++i)
            {
                const double vc = vb - fb * (vb - va) / (fb - fa);
                va = vb;
                fa = fb;
                vb = std::clamp(vc, 0.0, v1.x);
                fb = fly(vb, true, nullptr, target) - target;
            }
            vBack = vb;
        }
        else
            target = -1.0; // where it comes down
    }
    // The MECO sample itself (2026-10-20, mission-ops suite): fly() records from t1 + dt on, so without it every
    // post-MECO sample sat one index early: the whole flight after staging ran 0.5 s ahead, and the stack jumped
    // ~800 m (twice its speed for one sample) at separation.
    tr.p.push_back(p1);
    tr.v.push_back(v1);
    tr.burn.push_back(BURN_ASCENT);
    const size_t n0 = tr.p.size();
    fly(vBack, doBoost, &tr, rec == 2 ? target : 0.0);
    // It lands exactly on its target: the residual is taken out smoothly after the boostback.
    if ((rec == 1 || (rec == 2 && target > 0.0)) && tr.landed && tr.p.size() > n0 + 2)
    {
        const double resid = tr.p.back().x - (rec == 1 ? 0.0 : target);
        for (size_t i = n0; i < tr.p.size(); ++i)
        {
            const double s = (double)(i - n0) / (double)(tr.p.size() - 1 - n0);
            tr.p[i].x -= resid * s * s * (3.0 - 2.0 * s);
        }
    }
    tr.tEnd = (double)(tr.p.size() - 1) * tr.dt;
    return tr;
}

Track upperTrack(const Vehicle &vh, double hPark)
{
    Track tr;
    tr.dt = 0.5;
    const double t1 = vh.t1, t2 = std::max(vh.t2, vh.t1 + 60.0), h1 = vh.h1, x1 = vh.x1;
    const double vz1 = 2.0 * h1 / t1, vx1 = 3.0 * x1 / t1;
    const double vOrb = std::sqrt(kGM / (kR + hPark));
    const double T = t2 - t1;
    double x = 0.0;
    for (double t = 0.0; t <= t2 + 1e-9; t += tr.dt)
    {
        if (t < t1)
        {
            const double s = t / t1;
            x = x1 * s * s * s;
            tr.p.push_back({x, h1 * s * s});
            tr.v.push_back({3.0 * x1 * s * s / t1, 2.0 * h1 * s / t1});
            tr.burn.push_back(BURN_ASCENT);
            continue;
        }
        // Upper stage: altitude by a cubic Hermite (h1, vz1) -> (hPark, 0); horizontal speed eased to orbital.
        const double s = (t - t1) / T;
        const double h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s, h01 = -2 * s * s * s + 3 * s * s;
        const double h = h00 * h1 + h10 * vz1 * T + h01 * hPark;
        const double dh = ((6 * s * s - 6 * s) * h1 + (3 * s * s - 4 * s + 1) * vz1 * T + (-6 * s * s + 6 * s) * hPark) / T;
        const double e = s * s * (3.0 - 2.0 * s);
        // x is the arc ALONG THE GROUND (the angle x kR: rocketPos turns it into the inertial plane at x / kR), so its
        // rate is the speed x kR / (kR + h). (Until 2026-10-20 the speed itself: at SECO the stage ran 5-13% faster than
        // its parking orbit and lost 250-950 m/s in one step; mission-ops suite, insertion.dv_seco_m_s.)
        const double vx = vx1 + (vOrb * kR / (kR + h) - vx1) * e; // (vx1: the ascent's, already a ground rate)
        if (!tr.p.empty())
            x += 0.5 * (vx + tr.v.back().x) * tr.dt;
        tr.p.push_back({x, h});
        tr.v.push_back({vx, dh});
        tr.burn.push_back(BURN_UPPER);
    }
    tr.tEnd = (double)(tr.p.size() - 1) * tr.dt;
    return tr;
}

void cwFree(const glm::dvec3 &v0, double n, double t, glm::dvec3 &p, glm::dvec3 &v)
{
    const double c = std::cos(n * t), s = std::sin(n * t);
    const double vr = v0.x, va = v0.y, vc = v0.z;
    p = glm::dvec3(vr / n * s + 2.0 * va / n * (1.0 - c), 2.0 * vr / n * (c - 1.0) + va * (4.0 * s / n - 3.0 * t),
                   vc / n * s);
    v = glm::dvec3(vr * c + 2.0 * va * s, -2.0 * vr * s + va * (4.0 * c - 3.0), vc * c);
}

void cwState(const glm::dvec3 &p0, const glm::dvec3 &v0, double n, double t, glm::dvec3 &p, glm::dvec3 &v)
{
    const double c = std::cos(n * t), s = std::sin(n * t);
    const double x0 = p0.x, y0 = p0.y, z0 = p0.z, xd = v0.x, yd = v0.y, zd = v0.z;
    p = glm::dvec3((4.0 - 3.0 * c) * x0 + s / n * xd + 2.0 / n * (1.0 - c) * yd,
                   6.0 * (s - n * t) * x0 + y0 - 2.0 / n * (1.0 - c) * xd + (4.0 * s - 3.0 * n * t) / n * yd,
                   c * z0 + s / n * zd);
    v = glm::dvec3(3.0 * n * s * x0 + c * xd + 2.0 * s * yd, 6.0 * n * (c - 1.0) * x0 - 2.0 * s * xd + (4.0 * c - 3.0) * yd,
                   -n * s * z0 + c * zd);
}

double rampPhase(double a0, double a1, double T, double tau, double f0, double f1)
{
    if (T <= 0.0)
        return meanMotion(a0) * f0 * tau;
    if (std::abs(a1 - a0) < 1e-6)
        return meanMotion(a0) * (f0 * tau + 0.5 * (f1 - f0) * tau * tau / T);
    const double k = (a1 - a0) / T;
    const double a = a0 + k * tau;
    // The integral of sqrt(GM) a^-3/2 (A + B a) over a from a0 (the factor f = A + B a is linear in tau, so in a).
    const double Bf = (f1 - f0) / (k * T), Af = f0 - Bf * a0;
    return std::sqrt(kGM) / k *
           (2.0 * Af * (1.0 / std::sqrt(a0) - 1.0 / std::sqrt(a)) + 2.0 * Bf * (std::sqrt(a) - std::sqrt(a0)));
}

// ── Parcels ──────────────────────────────────────────────────────────────────────────────────────
bool Parcel::at(double a, glm::dvec3 &cOut, double &sOut, double &condOut, double *sweepOut) const
{
    if (age.empty() || a < 0.0 || a > (double)age.back())
        return false;
    size_t i = (size_t)(std::upper_bound(age.begin(), age.end(), (float)a) - age.begin());
    if (i == 0)
        i = 1;
    if (i >= age.size())
        i = age.size() - 1;
    const double a0 = age[i - 1], a1 = age[i];
    const double f = a1 > a0 ? std::clamp((a - a0) / (a1 - a0), 0.0, 1.0) : 1.0;
    cOut = glm::mix(c[i - 1], c[i], f);
    sOut = glm::mix((double)sig[i - 1], (double)sig[i], f);
    condOut = glm::mix((double)cond[i - 1], (double)cond[i], f);
    if (sweepOut)
        *sweepOut = glm::mix((double)sweep[i - 1], (double)sweep[i], f);
    return true;
}

Parcel makeParcel(double t0, const glm::dvec3 &p0, const glm::dvec3 &v0, double m, double kappa, double albedo,
                  const glm::dvec3 &axis, double len, double maxAge, const glm::dvec3 &windDir, double windGain,
                  double vExp, double windPhase, const ParcelOpts *opts)
{
    const ParcelOpts O = opts ? *opts : ParcelOpts{};
    Parcel pc;
    pc.t0 = t0;
    pc.m = m;
    pc.kappa = kappa;
    pc.albedo = albedo;
    pc.axis = axis;
    pc.len = len;
    const glm::dvec3 omega(0.0, 0.0, 7.2921159e-5);
    glm::dvec3 p = p0, v = v0;
    double sig = O.sig0;
    const double h0 = glm::length(p0) - kR;
    double Me = airDensity(h0) * 4.18879 * sig * sig * sig;
    // Evaporation: a low trail dissipates in minutes, a stratospheric one lingers, the high exhaust's ice longest.
    const double tauEvap = O.evapS > 0.0 ? O.evapS : h0 < 3000.0 ? 420.0 : h0 < 18000.0 ? 1500.0 : 3600.0;
    // Condensation: the trail is not visible at the nozzle - the gas has to cool as it expands (soot and droplets low
    // down within a fraction of a second; the ice of the high exhaust over a couple of seconds). Without it the
    // newest exhaust, still metres wide, was an opaque white needle at the stage.
    const double tauCond = glm::mix(0.25, 1.5, glm::smoothstep(20000.0, 90000.0, h0));
    double age = 0.0, nextRec = 0.0;
    auto record = [&]() {
        pc.age.push_back((float)age);
        pc.c.push_back(p);
        pc.sig.push_back((float)sig);
        pc.cond.push_back((float)(std::exp(-age / tauEvap) * (1.0 - std::exp(-age / tauCond))));
        pc.sweep.push_back((float)(Me / m));
    };
    record();
    nextRec = 0.25;
    while (age < maxAge)
    {
        const double r = glm::length(p);
        const double h = r - kR;
        const glm::dvec3 up = p / r;
        const double rho = airDensity(h);
        const double M = m + Me;
        // Wind: the launch's wind direction (Earth-fixed, horizontal at the pad), its speed by altitude: a few m/s
        // at the ground, the jet stream near 12 km, the mesosphere's strong winds near 90 km.
        const double hk = h / 1000.0;
        // Layered: the direction turns with height (tides and gravity waves, vertical wavelengths ~10-25 km, strongest
        // above ~50 km), and the speed varies from layer to layer, so a trail shears into kinks and corkscrews as it
        // ages (the twisted noctilucent trails of twilight launches). windPhase decorrelates launches.
        const double gw = glm::smoothstep(35.0, 90.0, hk);
        const double wspd = windGain * (6.0 + 28.0 * std::exp(-std::pow((hk - 12.0) / 6.0, 2.0)) +
                                        45.0 * std::exp(-std::pow((hk - 95.0) / 15.0, 2.0)) +
                                        15.0 * gw * (1.0 + std::sin(6.2831853 * hk / 13.0 + 3.0 * windPhase))) +
                            2.0 * std::sin(6.2831853 * hk / 3.1 + 5.0 * windPhase); // fine shear in the lower layers
        const double twist = (0.12 + 0.55 * gw) * (std::sin(6.2831853 * hk / 9.0 + windPhase) +
                                                   0.6 * std::sin(6.2831853 * hk / 23.0 + 2.1 * windPhase));
        glm::dvec3 w = windDir - glm::dot(windDir, up) * up;
        if (glm::length(w) > 1e-9)
        {
            w = glm::normalize(w);
            const glm::dvec3 side = glm::cross(up, w);
            w = (std::cos(twist) * w + std::sin(twist) * side) * wspd;
        }
        else
            w = glm::dvec3(0.0);
        const glm::dvec3 vr = v - w;
        // Expansion: the gas's own (km/s into vacuum) while it outweighs the air it has taken in, then diffusion.
        const double Kd = hk < 15.0 ? 25.0 : hk < 60.0 ? 8.0 : 400.0;
        // (turbulent growth at the parcel's speed through the air, capped at 60 m/s: a young exhaust parcel still moves
        // at km/s, and uncapped it swelled to hundreds of metres in a second)
        const double sigDot = vExp * (m / M) + Kd / sig + O.growK * std::min(glm::length(vr), 60.0);
        // Entrainment: the air swept by the growing surface and the parcel's motion through the air. High up the
        // air is so thin (mean free path ~1 km at 200 km) that the exhaust largely passes through it: the coupling
        // weakens, and the bell grows to hundreds of km as seen in twilight launches.
        const double couple = glm::mix(1.0, 0.03, glm::smoothstep(80000.0, 160000.0, h));
        const double dMe = couple * rho * 12.566 * sig * sig * (sigDot + 0.25 * glm::length(vr));
        // Step: a bounded change of radius and of mass, growing with age.
        double dt = std::max(1e-3, 0.03 * age);
        dt = std::min(dt, 0.15 * sig / std::max(sigDot, 1e-6));
        dt = std::min(dt, 5.0);
        dt = std::min(dt, maxAge - age + 1e-6);
        // Momentum: entrained air mixes in at the wind's velocity (exact for any step: no stiffness); gravity acts on
        // the exhaust's share (the air it carries is neutral); Coriolis and centrifugal in the Earth-fixed frame.
        const double g = kGM / (r * r);
        glm::dvec3 acc = -g * up * (m / M) - 2.0 * glm::cross(omega, v) - glm::cross(omega, glm::cross(omega, p)) * (m / M);
        if (O.buoy > 0.0)
            acc += up * (O.buoy * std::exp(-age / O.buoyS));
        const double dM = dMe * dt;
        v = (M * (v + acc * dt) + dM * w) / (M + dM);
        Me += dM;
        sig += sigDot * dt;
        p += v * dt;
        // The ground: the exhaust spreads along it (the pad's ground cloud).
        const double hn = glm::length(p) - kR;
        if (hn < O.groundM)
        {
            const glm::dvec3 un = glm::normalize(p);
            p = un * (kR + O.groundM);
            const double vrad = glm::dot(v, un);
            if (vrad < 0.0)
                v -= 1.2 * vrad * un; // a little rebound: it billows up
            v *= 0.9;
        }
        age += dt;
        if (age >= nextRec || age >= maxAge)
        {
            record();
            nextRec = std::max(nextRec * 1.3, nextRec + 0.25);
        }
    }
    return pc;
}
} // namespace launch
