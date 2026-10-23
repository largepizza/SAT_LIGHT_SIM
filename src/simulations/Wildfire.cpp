// Wildfire.cpp — see Wildfire.h.
#include "Wildfire.h"
#include <algorithm>
#include <cmath>
#include <limits>

namespace wx {

namespace {
constexpr double kPi = 3.14159265358979323846;
uint64_t mix64(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double u01(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
double sstep(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
// The calibrated birth rate per 1-degree cell per day at fuel 1, dryness 1, at the equator's cell area: ~50 fires born
// a day worldwide at the dry season (mean life ~6 days: ~300 burning at once, the order of the large fires MODIS sees on a given day).
constexpr double kBirthRate = 0.045;

// The fire hashed in cell (ilat, ilon) for day bin db, if any.
bool fireInCell(int ilat, int ilon, int64_t db, const BiomeMap &bio, float rateScale, Fire &f)
{
    const uint64_t key = (uint64_t)((ilat + 90) * 360 + (ilon + 180));
    const uint64_t h0 = mix64(key * 0x100000001B3ull ^ mix64((uint64_t)db + 0x5EED0F1Eull));
    const double lat = ilat + u01(mix64(h0 ^ 1)), lon = ilon + u01(mix64(h0 ^ 2));
    const float fuel = bio.fuelAt(lat, lon);
    if (fuel < 0.1f) return false;
    const double doy = std::fmod((double)db + 1.0, 365.2422);
    const double dry = BiomeMap::seasonalDryness(lat, doy < 0 ? doy + 365.2422 : doy);
    const double lam = kBirthRate * rateScale * fuel * dry * dry * std::cos(lat * kPi / 180.0);
    if (u01(mix64(h0 ^ 3)) >= lam) return false;
    f.id = h0;
    f.seed = (uint32_t)(h0 >> 32);
    f.latDeg = lat;
    f.lonDeg = lon;
    f.fuel = fuel;
    f.size = (float)std::pow(u01(mix64(h0 ^ 4)), 2.0);   // most fires small
    // Ignited mostly in the local afternoon (12-19 h local solar time). Bin db starts at J2000 + db days = 12:00 UTC.
    const double localH = 12.0 + 7.0 * u01(mix64(h0 ^ 5));
    const double utcAfterBin = std::fmod(localH - lon / 15.0 - 12.0 + 48.0, 24.0);
    f.t0 = (double)db * 86400.0 + utcAfterBin * 3600.0;
    // Life: log-normal, median ~2.5 days, longer for big fires; 0.4..30 days.
    const double z = std::sqrt(-2.0 * std::log(std::max(u01(mix64(h0 ^ 6)), 1e-12))) *
                     std::cos(2.0 * kPi * u01(mix64(h0 ^ 7)));
    f.life = 86400.0 * std::clamp(2.5 * std::exp(0.8 * z) * (0.4 + 1.6 * f.size), 0.4, 30.0);
    f.spreadMps = (0.02 + 0.12 * std::pow((double)f.size, 1.5)) * (0.6 + 0.8 * u01(mix64(h0 ^ 8)));
    // The wind it spreads with: the trades (toward the west) in the tropics, the westerlies (toward the east) beyond.
    const double base = std::fabs(lat) < 30.0 ? -0.5 * kPi : 0.5 * kPi;
    f.windAz = base + 1.3 * (u01(mix64(h0 ^ 9)) - 0.5) * 2.0;
    const Biome b = bio.at(lat, lon);
    f.pyro = (f.size > 0.7f && (b == Biome::Boreal || b == Biome::TemperateForest || b == Biome::Shrubland) &&
              u01(mix64(h0 ^ 10)) < 0.35) ? 1.0f : 0.0f;
    return true;
}

// Every fire born in day bins [b0, b1] in the cells round (latC, lonC); alive at t only when t is finite.
void scanCells(double t, int64_t b0, int64_t b1, int lat0, int lat1, double latC, double lonC, double radiusKm, bool all,
               const BiomeMap &bio, float rateScale, std::vector<Fire> &out)
{
    for (int il = lat0; il <= lat1; ++il)
    {
        if (il < -90 || il > 89) continue;
        const double cl = std::cos((il + 0.5) * kPi / 180.0);
        int lo0 = -180, lo1 = 179;
        if (!all)
        {
            const double dLon = cl > 0.02 ? radiusKm / (111.2 * cl) + 1.0 : 360.0;
            if (dLon < 180.0)
            {
                lo0 = (int)std::floor(lonC - dLon);
                lo1 = (int)std::floor(lonC + dLon);
            }
        }
        for (int ilw = lo0; ilw <= lo1; ++ilw)
        {
            const int ilon = ((ilw + 180) % 360 + 360) % 360 - 180;
            for (int64_t db = b0; db <= b1; ++db)
            {
                Fire f;
                if (!fireInCell(il, ilon, db, bio, rateScale, f)) continue;
                if (std::isfinite(t) && (t < f.t0 || t > f.t0 + f.life)) continue;
                if (!all)
                {
                    const double p1 = latC * kPi / 180.0, p2 = f.latDeg * kPi / 180.0;
                    const double c = std::sin(p1) * std::sin(p2) +
                                     std::cos(p1) * std::cos(p2) * std::cos((f.lonDeg - lonC) * kPi / 180.0);
                    if (std::acos(std::clamp(c, -1.0, 1.0)) * 6371.0 > radiusKm) continue;
                }
                out.push_back(f);
            }
        }
    }
}
} // namespace

FireState fireStateAt(const Fire &f, double t)
{
    FireState s;
    s.ageS = t - f.t0;
    if (s.ageS < 0.0 || s.ageS > f.life) return s;
    const double grow = sstep(0.0, 4.0 * 3600.0, s.ageS);
    const double decay = 1.0 - sstep(0.7, 1.0, s.ageS / f.life);
    const double localH = std::fmod(t / 3600.0 + 12.0 + f.lonDeg / 15.0 + 48.0, 24.0);
    double dh = std::fabs(localH - 15.5);
    dh = std::min(dh, 24.0 - dh);
    const double diur = 0.3 + 0.7 * std::exp(-(dh / 4.5) * (dh / 4.5));
    s.activity = grow * decay * diur * (0.6 + 0.4 * f.fuel);
    s.headM = 40.0 + f.spreadMps * s.ageS * 0.7;
    s.backM = 0.12 * s.headM;
    s.halfWidthM = 0.28 * s.headM + 30.0;
    s.windAz = f.windAz + 0.35 * std::sin(2.0 * kPi * s.ageS / (1.7 * 86400.0) + (f.seed & 1023) * 0.006);
    s.plumeTopM = std::min((800.0 + 2200.0 * f.size) * (0.4 + 0.6 * s.activity) * (f.pyro > 0.5f ? 2.6 : 1.0), 8000.0);
    return s;
}

glm::dvec2 fireEdgeAt(const FireState &s, double a)
{
    const glm::dvec2 w(std::sin(s.windAz), std::cos(s.windAz)), c(std::cos(s.windAz), -std::sin(s.windAz));
    const double semi = 0.5 * (s.headM + s.backM), off = 0.5 * (s.headM - s.backM);
    return w * (off + semi * std::cos(a)) + c * (s.halfWidthM * std::sin(a));
}

double fireFrontIntensity(double a)
{
    const double h = 0.5 * (1.0 + std::cos(a));
    return 0.1 + 0.9 * std::pow(h, 2.5);
}

void firesNear(double t, double latDeg, double lonDeg, double radiusKm, const BiomeMap &bio, float rateScale,
               std::vector<Fire> &out)
{
    out.clear();
    if (!bio.ready() || rateScale <= 0.0f) return;
    const double dLat = radiusKm / 111.2 + 1.0;
    scanCells(t, (int64_t)std::floor((t - kFireMaxLifeS) / 86400.0), (int64_t)std::floor(t / 86400.0),
              (int)std::floor(latDeg - dLat), (int)std::floor(latDeg + dLat), latDeg, lonDeg, radiusKm, false, bio,
              rateScale, out);
}

void firesBorn(int64_t binFrom, int64_t binTo, double latDeg, double lonDeg, double radiusKm, const BiomeMap &bio,
               float rateScale, std::vector<Fire> &out)
{
    if (!bio.ready() || rateScale <= 0.0f || binTo < binFrom) return;
    const double dLat = radiusKm / 111.2 + 1.0;
    scanCells(std::numeric_limits<double>::quiet_NaN(), binFrom, binTo, (int)std::floor(latDeg - dLat),
              (int)std::floor(latDeg + dLat), latDeg, lonDeg, radiusKm, false, bio, rateScale, out);
}

void firesWorld(double t, const BiomeMap &bio, float rateScale, std::vector<Fire> &out)
{
    out.clear();
    if (!bio.ready() || rateScale <= 0.0f) return;
    scanCells(t, (int64_t)std::floor((t - kFireMaxLifeS) / 86400.0), (int64_t)std::floor(t / 86400.0), -90, 89, 0.0,
              0.0, 0.0, true, bio, rateScale, out);
}

} // namespace wx
