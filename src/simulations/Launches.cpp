// Launches, the app-independent half (see Launches.h; plan .plans/LAUNCHES_PLAN.md).
#include "Launches.h"

#include <algorithm>
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

// ── Catalog ──────────────────────────────────────────────────────────────────────────────────────
bool Catalog::load(const std::string &path, std::string &err)
{
    sites.clear();
    vehicles.clear();
    programs.clear();
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
            programs.push_back(pg);
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
Track boosterTrack(const Vehicle &vh)
{
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
    // After MECO: integrated. The boostback's target speed is found by a few shots (the landing x -> 0).
    auto fly = [&](double vBack, Track *out) {
        glm::dvec2 p = p1, v = v1;
        bool boost = vh.recovery == 1, entryDone = false, landing = false;
        double entryT = -1.0;
        const double dt = 0.05;
        double t = t1;
        double rec = t1;
        const double g = 9.81, k = 9.81 / (280.0 * 280.0);
        for (int it = 0; it < 200000; ++it)
        {
            uint8_t b = BURN_NONE;
            glm::dvec2 a(0.0, -g);
            const double spd = glm::length(v);
            a -= k * (airDensity(p.y) / 1.225) * spd * v; // drag
            if (vh.recovery == 0)
            {
                // Expendable: falls away; drawn until it breaks up low in the atmosphere.
                if (p.y < 30000.0 && v.y < 0.0)
                    break;
            }
            else
            {
                if (boost && t > t1 + 12.0)
                {
                    // Boostback: horizontal thrust toward the target speed.
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
                    if (t - entryT < 22.0)
                    {
                        a -= 28.0 * v / std::max(spd, 1.0);
                        b = BURN_ENTRY;
                    }
                    else
                        entryDone = true;
                }
                // Landing burn: a suicide-burn profile (the vertical speed that stops at the ground at aL), thrust up only.
                const double aL = 30.0;
                if (!landing && v.y < 0.0 && p.y < spd * spd / (2.0 * aL) + 300.0 && p.y < 15000.0)
                    landing = true;
                if (landing)
                {
                    const double vzD = -std::min(0.85 * std::sqrt(2.0 * aL * std::max(p.y, 0.0)) + 1.5, 400.0);
                    const double thrust = std::clamp(g + (vzD - v.y) * 1.5, 0.0, aL + g);
                    a = glm::dvec2(std::clamp(-v.x * 0.6, -aL, aL), thrust - g);
                    b = BURN_LANDING;
                    if (p.y <= 0.5)
                    {
                        if (out)
                        {
                            out->landed = true;
                            out->p.push_back({p.x, 0.0});
                            out->v.push_back({0.0, 0.0});
                            out->burn.push_back(BURN_NONE);
                        }
                        return p.x;
                    }
                }
            }
            v += a * dt;
            p += v * dt;
            t += dt;
            if (p.y <= 0.0)
                break;
            if (out && t >= rec + out->dt - 1e-9)
            {
                rec += out->dt;
                out->p.push_back(p);
                out->v.push_back(v);
                out->burn.push_back(b);
            }
        }
        return p.x;
    };
    double vBack = 0.0;
    if (vh.recovery == 1)
    {
        // Secant on the boostback speed for a landing at x = 0 (the pad / the tower).
        double va = -0.25 * v1.x, vb = -0.6 * v1.x;
        double fa = fly(va, nullptr), fb = fly(vb, nullptr);
        for (int i = 0; i < 12 && std::abs(fb) > 50.0 && std::abs(fb - fa) > 1e-6; ++i)
        {
            const double vc = vb - fb * (vb - va) / (fb - fa);
            va = vb;
            fa = fb;
            vb = std::clamp(vc, -3.0 * v1.x, v1.x);
            fb = fly(vb, nullptr);
        }
        vBack = vb;
    }
    else if (vh.recovery == 2)
        vBack = 0.55 * v1.x; // a short boostback: lands ~downrange on the droneship
    const size_t n0 = tr.p.size();
    fly(vBack, &tr);
    // The return lands exactly on the pad: the residual is taken out smoothly after the boostback.
    if (vh.recovery == 1 && tr.landed && tr.p.size() > n0 + 2)
    {
        const double resid = tr.p.back().x;
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
        const double vx = vx1 + (vOrb - vx1) * e;
        if (!tr.p.empty())
            x += 0.5 * (vx + tr.v.back().x) * tr.dt;
        tr.p.push_back({x, h});
        tr.v.push_back({vx, dh});
        tr.burn.push_back(BURN_UPPER);
    }
    tr.tEnd = (double)(tr.p.size() - 1) * tr.dt;
    return tr;
}

double rampPhase(double a0, double a1, double T, double tau)
{
    if (T <= 0.0 || std::abs(a1 - a0) < 1e-6)
        return meanMotion(a0) * tau;
    const double k = (a1 - a0) / T;
    const double a = a0 + k * tau;
    return 2.0 * std::sqrt(kGM) / k * (1.0 / std::sqrt(a0) - 1.0 / std::sqrt(a));
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
                  double vExp, double windPhase)
{
    Parcel pc;
    pc.t0 = t0;
    pc.m = m;
    pc.kappa = kappa;
    pc.albedo = albedo;
    pc.axis = axis;
    pc.len = len;
    const glm::dvec3 omega(0.0, 0.0, 7.2921159e-5);
    glm::dvec3 p = p0, v = v0;
    double sig = 8.0;
    const double h0 = glm::length(p0) - kR;
    double Me = airDensity(h0) * 4.18879 * sig * sig * sig;
    // Evaporation: a low trail dissipates in minutes, a stratospheric one lingers, the high exhaust's ice longest.
    const double tauEvap = h0 < 3000.0 ? 420.0 : h0 < 18000.0 ? 1500.0 : 3600.0;
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
        const double sigDot = vExp * (m / M) + Kd / sig;
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
        const double dM = dMe * dt;
        v = (M * (v + acc * dt) + dM * w) / (M + dM);
        Me += dM;
        sig += sigDot * dt;
        p += v * dt;
        // The ground: the exhaust spreads along it (the pad's ground cloud).
        const double hn = glm::length(p) - kR;
        if (hn < 3.0)
        {
            const glm::dvec3 un = glm::normalize(p);
            p = un * (kR + 3.0);
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
