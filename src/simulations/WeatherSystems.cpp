// Weather v2 systems (see WeatherSystems.h).
#include "WeatherSystems.h"
#include <algorithm>
#include <cmath>

namespace wx {

namespace {
constexpr double kPi = 3.14159265358979323846;
constexpr double kR = 6371000.0;
constexpr double kBinS = 43200.0;          // spawn bins: 12 h
constexpr double kCellDeg = 20.0;          // spawn cells: 20 deg of longitude
constexpr int    kCells = 18;
constexpr double kTrackStepS = 1800.0;     // the track's resolution
constexpr double kMaxLifeDays = 5.5;
constexpr double kSteerH = 4000.0;         // the steering level (m)

uint64_t mix64(uint64_t x)
{
    x ^= x >> 30; x *= 0xBF58476D1CE4E5B9ull;
    x ^= x >> 27; x *= 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double u01(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
double gauss(double x, double c, double s) { const double u = (x - c) / s; return std::exp(-u * u); }
double wrapDeg(double a) { a = std::fmod(a + 180.0, 360.0); if (a < 0.0) a += 360.0; return a - 180.0; }
glm::dvec3 dirOf(double latDeg, double lonDeg)
{
    const double la = latDeg * kPi / 180.0, lo = lonDeg * kPi / 180.0;
    return {std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)};
}
// The Sun's declination from the time alone (rad): the tracks must not depend on when they were first asked for.
double declOf(double t)
{
    const double days = t / 86400.0;   // since J2000 (1 January, 12:00)
    return 23.44 * kPi / 180.0 * std::sin(2.0 * kPi * (days - 79.0) / 365.2422);
}
// Genesis preference by longitude (mean ~1): off the east coasts of the continents (the Gulf Stream, the Kuroshio)
// in the north; nearly zonal in the south.
double lonWeight(double hemi, double lonDeg)
{
    if (hemi > 0.0)
        return 0.75 + 1.1 * gauss(wrapDeg(lonDeg + 65.0), 0.0, 28.0) + 1.1 * gauss(wrapDeg(lonDeg - 145.0), 0.0, 28.0)
               + 0.35 * gauss(wrapDeg(lonDeg - 15.0), 0.0, 20.0) - 0.45 * gauss(wrapDeg(lonDeg - 85.0), 0.0, 35.0);
    return 0.85 + 0.3 * gauss(wrapDeg(lonDeg + 50.0), 0.0, 30.0) + 0.3 * gauss(wrapDeg(lonDeg - 35.0), 0.0, 30.0)
           + 0.3 * gauss(wrapDeg(lonDeg - 155.0), 0.0, 30.0);
}
}

void SystemCatalog::systemsAt(double t, double /*decl*/, const SystemSettings &s, std::vector<SystemState> &out)
{
    out.clear();
    const double lifeMax = kMaxLifeDays * 86400.0 * std::max(s.lifeScale, 0.1);
    const int64_t b0 = (int64_t)std::floor((t - lifeMax) / kBinS), b1 = (int64_t)std::floor(t / kBinS);
    const uint64_t skey = mix64((uint64_t)(s.waves * 1000.0) ^ ((uint64_t)(s.eddies * 1000.0) << 20)
                                ^ ((uint64_t)(s.lifeScale * 1000.0) << 40));
    const double perCellBin = std::max(s.etcPerDay, 0.0) * (kBinS / 86400.0) / (double)kCells;
    for (int h = 0; h < 2; ++h)
    {
        const double hemi = h == 0 ? 1.0 : -1.0;
        for (int64_t b = b0; b <= b1; ++b)
        {
            // Winter storm tracks are busier (the declination opposite to the hemisphere).
            const double bt = (double)b * kBinS;
            const double winter = std::clamp(-hemi * declOf(bt) / 0.41, -1.0, 1.0);
            for (int c = 0; c < kCells; ++c)
            {
                const uint64_t id = mix64(0x5157E35ull ^ ((uint64_t)h << 62) ^ ((uint64_t)(b & 0xFFFFFFFFFFull) << 8) ^ (uint64_t)c);
                const double lon = -180.0 + kCellDeg * ((double)c + u01(mix64(id ^ 1)));
                const double p = perCellBin * lonWeight(hemi, lon) * (1.0 + 0.3 * winter);
                if (u01(id) >= p) continue;
                const double birth = bt + kBinS * u01(mix64(id ^ 2));
                const double life = (3.0 + 2.5 * u01(mix64(id ^ 3))) * 86400.0 * std::max(s.lifeScale, 0.1);
                if (birth > t || birth + life < t) continue;
                Track &tr = tracks_[id];
                if (tr.pos.empty() || tr.settingsKey != skey)
                {
                    tr.birthT = birth; tr.lifeS = life; tr.hemi = hemi;
                    tr.seed = u01(mix64(id ^ 4));
                    tr.radiusM = (8.0e5 + 4.0e5 * u01(mix64(id ^ 5)));
                    tr.settingsKey = skey;
                    const double lat = hemi * (34.0 + 12.0 * u01(mix64(id ^ 6)) - 3.0 * winter);
                    glm::dvec3 d = dirOf(lat, lon);
                    const int n = (int)std::ceil(life / kTrackStepS) + 2;
                    tr.pos.assign(1, d);
                    tr.pos.reserve(n);
                    for (int k = 1; k < n; ++k)
                    {
                        // The steering wind (x 0.8: a low moves slower than the flow aloft) + a poleward drift that
                        // fades as it occludes (lows deepen poleward of the jet, then stall).
                        auto vel = [&](const glm::dvec3 &p, double tt) {
                            const WindState ws = windState(tt, declOf(tt), s.waves, s.eddies);
                            const glm::dvec3 east = glm::normalize(glm::dvec3(-p.y, p.x, 0.0) + glm::dvec3(1e-9, 0, 0));
                            const glm::dvec3 north = glm::cross(p, east);
                            const double age = (tt - birth) / life;
                            return windAt(p, kSteerH, ws) * 0.8 + north * (hemi * 3.0 * (1.0 - age));
                        };
                        const double tt = birth + (k - 1) * kTrackStepS;
                        const glm::dvec3 v0 = vel(d, tt);
                        const glm::dvec3 dm = glm::normalize(d + v0 * (0.5 * kTrackStepS / kR));
                        const glm::dvec3 v1 = vel(dm, tt + 0.5 * kTrackStepS);
                        d = glm::normalize(d + v1 * (kTrackStepS / kR));
                        tr.pos.push_back(d);
                    }
                }
                tr.lastUsedT = t;
                const double f = (t - tr.birthT) / kTrackStepS;
                const int k = std::clamp((int)std::floor(f), 0, (int)tr.pos.size() - 2);
                const double w = std::clamp(f - k, 0.0, 1.0);
                SystemState st;
                st.id = id;
                st.kind = SystemKind::Extratropical;
                st.centre = glm::normalize(tr.pos[k] * (1.0 - w) + tr.pos[k + 1] * w);
                glm::dvec3 mv = tr.pos[k + 1] - tr.pos[k];
                mv -= st.centre * glm::dot(mv, st.centre);
                const double ml = glm::length(mv);
                st.heading = ml > 1e-12 ? mv / ml : glm::normalize(glm::dvec3(-st.centre.y, st.centre.x, 0.0) + glm::dvec3(1e-9, 0, 0));
                st.speedMps = ml * kR / kTrackStepS;
                st.hemi = tr.hemi;
                st.radiusM = tr.radiusM * std::max(s.sizeScale, 0.1);
                st.age01 = std::clamp((t - tr.birthT) / tr.lifeS, 0.0, 1.0);
                const double a = st.age01;
                const double rise = std::clamp(a / 0.25, 0.0, 1.0), fall = std::clamp((1.0 - a) / 0.3, 0.0, 1.0);
                st.intensity = (0.75 + 0.25 * tr.seed) * rise * rise * (3.0 - 2.0 * rise) * fall * fall * (3.0 - 2.0 * fall);
                st.seed = tr.seed;
                out.push_back(st);
            }
        }
    }
    // Forget the tracks not asked for this time once they are out of every window it could ask about.
    for (auto it = tracks_.begin(); it != tracks_.end();)
        if (it->second.lastUsedT != t && (it->second.birthT + it->second.lifeS < t - 86400.0 || it->second.birthT > t + 86400.0))
            it = tracks_.erase(it);
        else
            ++it;
}

} // namespace wx
