#include "Cinematic.h"

#include <algorithm>
#include <cmath>

double Cinematic::duration() const
{
    double d = 0.0;
    for (const auto &s : shots)
        d += s.duration();
    return d;
}

int Cinematic::shotAt(double t, double &local) const
{
    local = 0.0;
    if (shots.empty())
        return -1;
    double acc = 0.0;
    for (int i = 0; i < (int)shots.size(); ++i)
    {
        const double d = shots[i].duration();
        if (t < acc + d || i + 1 == (int)shots.size())
        {
            local = std::clamp(t - acc, 0.0, d) + (shots[i].keys.empty() ? 0.0 : shots[i].keys.front().t);
            return i;
        }
        acc += d;
    }
    return -1;
}

CineKey cineEval(const std::vector<CineKey> &Kin, double t)
{
    // Azimuth unwrapped against the previous key (2026-10-03: the comment always said so, the code did not — a key
    // at az 240 followed by one taken at -120, the same direction, swung the camera a full turn between them).
    std::vector<CineKey> K = Kin;
    for (size_t j = 1; j < K.size(); ++j)
    {
        while (K[j].az - K[j - 1].az > 180.0)
            K[j].az -= 360.0;
        while (K[j].az - K[j - 1].az < -180.0)
            K[j].az += 360.0;
    }
    if (K.empty())
        return CineKey{};
    if (t <= K.front().t)
        return K.front();
    if (t >= K.back().t)
        return K.back();
    size_t i = 0;
    while (i + 1 < K.size() && K[i + 1].t < t)
        ++i;
    const CineKey &a = K[i], &b = K[i + 1];
    const double h = b.t - a.t, s = (t - a.t) / h;
    const double h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s;
    const double h01 = -2 * s * s * s + 3 * s * s, h11 = s * s * s - s * s;
    // Catmull-Rom tangent of channel f at key j (one-sided at the ends), per path second.
    auto tangent = [&](size_t j, double (*f)(const CineKey &)) -> double
    {
        const size_t j0 = j > 0 ? j - 1 : j, j1 = j + 1 < K.size() ? j + 1 : j;
        const double dt = K[j1].t - K[j0].t;
        return dt > 0.0 ? (f(K[j1]) - f(K[j0])) / dt : 0.0;
    };
    auto herm = [&](double (*f)(const CineKey &))
    { return h00 * f(a) + h10 * h * tangent(i, f) + h01 * f(b) + h11 * h * tangent(i + 1, f); };
    CineKey r;
    r.t = t;
    r.lat = herm([](const CineKey &k) { return k.lat; });
    r.lon = herm([](const CineKey &k) { return k.lon; });
    r.alt = std::exp(herm([](const CineKey &k) { return std::log(std::max(k.alt, 0.0) + 10.0); })) - 10.0;
    r.az = herm([](const CineKey &k) { return k.az; });
    r.el = herm([](const CineKey &k) { return k.el; });
    r.fov = std::exp(herm([](const CineKey &k) { return std::log(k.fov); }));
    r.ox = herm([](const CineKey &k) { return k.ox; });
    r.oy = herm([](const CineKey &k) { return k.oy; });
    r.oz = herm([](const CineKey &k) { return k.oz; });
    r.hasOffset = a.hasOffset && b.hasOffset;
    // The view: its components interpolated, then normalised (a key without one is aimed at the satellite,
    // i.e. along -offset, so a shot can mix both).
    if (a.hasView || b.hasView)
    {
        auto vOf = [](const CineKey &k, int c) -> double
        {
            if (k.hasView)
                return c == 0 ? k.vx : c == 1 ? k.vy : k.vz;
            const double l = std::sqrt(k.ox * k.ox + k.oy * k.oy + k.oz * k.oz);
            if (l <= 1e-9)
                return 0.0;
            return -(c == 0 ? k.ox : c == 1 ? k.oy : k.oz) / l;
        };
        const double x = (1.0 - (3 * s * s - 2 * s * s * s)) * vOf(a, 0) + (3 * s * s - 2 * s * s * s) * vOf(b, 0);
        const double y = (1.0 - (3 * s * s - 2 * s * s * s)) * vOf(a, 1) + (3 * s * s - 2 * s * s * s) * vOf(b, 1);
        const double z = (1.0 - (3 * s * s - 2 * s * s * s)) * vOf(a, 2) + (3 * s * s - 2 * s * s * s) * vOf(b, 2);
        const double l = std::sqrt(x * x + y * y + z * z);
        if (l > 1e-6)
        {
            r.hasView = true;
            r.vx = x / l;
            r.vy = y / l;
            r.vz = z / l;
        }
    }
    // Sim time: linear between the keys that set it (hold outside them).
    const CineKey *sa = nullptr, *sb = nullptr;
    for (const auto &k : K)
    {
        if (!k.hasSim)
            continue;
        if (k.t <= t)
            sa = &k;
        if (k.t >= t && !sb)
            sb = &k;
    }
    if (sa || sb)
    {
        r.hasSim = true;
        if (sa && sb && sb->t > sa->t)
            r.simT = sa->simT + (sb->simT - sa->simT) * (t - sa->t) / (sb->t - sa->t);
        else
            r.simT = sa ? sa->simT : sb->simT;
    }
    return r;
}

nlohmann::json cineToJson(const Cinematic &c)
{
    nlohmann::json j;
    j["format"] = "sat-light-sim-cinematic/1";
    j["name"] = c.name;
    j["fps"] = c.fps;
    j["shots"] = nlohmann::json::array();
    for (const auto &s : c.shots)
    {
        nlohmann::json js;
        js["name"] = s.name;
        js["sim_rate"] = s.simRate;
        js["ease"] = s.ease;
        if (s.simStartValid)
            js["sim_start_j2000_s"] = s.simStart;
        if (!s.settings.is_null())
            js["settings"] = s.settings;
        if (s.followSat >= 0)
            js["follow_sat"] = s.followSat;
        if (s.hasDrift)
            js["cloud_drift"] = {{"phase_offset", s.driftOffset}, {"rate", s.driftRate}};
        js["keys"] = nlohmann::json::array();
        for (const auto &k : s.keys)
        {
            nlohmann::json jk = {{"t", k.t}, {"lat", k.lat}, {"lon", k.lon}, {"alt", k.alt},
                                 {"az", k.az}, {"el", k.el}, {"fov", k.fov}};
            if (k.hasSim)
                jk["sim_j2000_s"] = k.simT;
            if (s.followSat >= 0)
            {
                jk["offset_m"] = {k.ox, k.oy, k.oz};
                if (k.hasView)
                    jk["view_sat"] = {k.vx, k.vy, k.vz};
            }
            js["keys"].push_back(jk);
        }
        j["shots"].push_back(js);
    }
    return j;
}

bool cineFromJson(const nlohmann::json &j, Cinematic &c, std::string &err)
{
    try
    {
        Cinematic out;
        out.name = j.value("name", std::string("untitled"));
        out.fps = std::clamp(j.value("fps", 30.0), 1.0, 240.0);
        for (const auto &js : j.at("shots"))
        {
            CineShot s;
            s.name = js.value("name", std::string());
            s.simRate = js.value("sim_rate", 1.0);
            s.ease = std::clamp(js.value("ease", 0.0), 0.0, 1.0);
            if (js.contains("sim_start_j2000_s"))
            {
                s.simStartValid = true;
                s.simStart = js["sim_start_j2000_s"].get<double>();
            }
            if (js.contains("settings"))
                s.settings = js["settings"];
            s.followSat = js.value("follow_sat", -1);
            if (js.contains("cloud_drift"))
            {
                s.hasDrift = true;
                s.driftOffset = js["cloud_drift"].value("phase_offset", 0.0);
                s.driftRate = js["cloud_drift"].value("rate", 0.0);
            }
            for (const auto &jk : js.at("keys"))
            {
                CineKey k;
                k.t = jk.value("t", 0.0);
                k.lat = jk.value("lat", 0.0);
                k.lon = jk.value("lon", 0.0);
                k.alt = jk.value("alt", 0.0);
                k.az = jk.value("az", 0.0);
                k.el = jk.value("el", 0.0);
                k.fov = jk.value("fov", 60.0);
                if (jk.contains("sim_j2000_s"))
                {
                    k.hasSim = true;
                    k.simT = jk["sim_j2000_s"].get<double>();
                }
                if (jk.contains("offset_m") && jk["offset_m"].size() == 3)
                {
                    k.ox = jk["offset_m"][0].get<double>();
                    k.oy = jk["offset_m"][1].get<double>();
                    k.oz = jk["offset_m"][2].get<double>();
                    k.hasOffset = true;
                }
                if (jk.contains("view_sat") && jk["view_sat"].size() == 3)
                {
                    k.hasView = true;
                    k.vx = jk["view_sat"][0].get<double>();
                    k.vy = jk["view_sat"][1].get<double>();
                    k.vz = jk["view_sat"][2].get<double>();
                }
                s.keys.push_back(k);
            }
            std::sort(s.keys.begin(), s.keys.end(), [](const CineKey &a, const CineKey &b) { return a.t < b.t; });
            out.shots.push_back(std::move(s));
        }
        c = std::move(out);
        return true;
    }
    catch (const std::exception &e)
    {
        err = e.what();
        return false;
    }
}
