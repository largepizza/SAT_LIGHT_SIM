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
// Periodic value noise on the circle (N cells round, smooth), drawn anew every T seconds and cross-faded between
// draws: 0..1.
double circNoise(double a, int N, uint64_t seed, double t, double T)
{
    const double x = a / (2.0 * kPi) * N;
    const double fx = std::floor(x), u = x - fx;
    const double su = u * u * (3.0 - 2.0 * u);
    const int i0 = (int)((((int64_t)fx % N) + N) % N), i1 = (i0 + 1) % N;
    const double e = std::floor(t / T), w = t / T - e;
    const double sw = w * w * (3.0 - 2.0 * w);
    auto v = [&](int i, double ep) {
        return u01(mix64(seed ^ ((uint64_t)i * 0x9E3779B97F4A7C15ull) ^ mix64((uint64_t)(int64_t)ep + 77)));
    };
    const double a0 = v(i0, e) + (v(i1, e) - v(i0, e)) * su;
    const double a1 = v(i0, e + 1) + (v(i1, e + 1) - v(i0, e + 1)) * su;
    return a0 + (a1 - a0) * sw;
}

// The wind's travelling waves: k (rad/m), the streamfunction amplitude (m^2/s), angular frequency, phase, and the phase
// per metre of height (the field changes with height: a plume's layers drift apart).
struct WindWave
{
    glm::dvec3 k;
    double amp, omega, phase, hPhase;
};
constexpr int kWavesSyn = 6, kWavesMeso = 6, kWavesSmall = 6;
struct WindWaves
{
    WindWave w[kWavesSyn + kWavesMeso + kWavesSmall + 1];
    WindWaves()
    {
        uint64_t h = 0x57A11D5EEDull;
        auto next = [&]() {
            h = mix64(h + 0x632BE59BD9B4E019ull);
            return u01(h);
        };
        auto make = [&](WindWave &o, double lamKm0, double lamKm1, double speed, double per0, double per1, double hLam0,
                        double hLam1) {
            const double z = 2.0 * next() - 1.0, ph = 2.0 * kPi * next();
            const double r = std::sqrt(std::max(1.0 - z * z, 0.0));
            const double lam = (lamKm0 + (lamKm1 - lamKm0) * next()) * 1000.0;
            const double kk = 2.0 * kPi / lam;
            o.k = glm::dvec3(r * std::cos(ph), r * std::sin(ph), z) * kk;
            o.amp = speed / kk;
            const double per = per0 + (per1 - per0) * next();
            o.omega = (next() < 0.5 ? -1.0 : 1.0) * 2.0 * kPi / per;
            o.phase = 2.0 * kPi * next();
            o.hPhase = hLam1 > 0.0 ? (next() < 0.5 ? -1.0 : 1.0) * 2.0 * kPi / (hLam0 + (hLam1 - hLam0) * next()) : 0.0;
        };
        int j = 0;
        for (int i = 0; i < kWavesSyn; ++i)
            make(w[j++], 2500.0, 4500.0, 3.6, 3.0 * 86400.0, 7.0 * 86400.0, 0.0, 0.0);
        for (int i = 0; i < kWavesMeso; ++i)
            make(w[j++], 250.0, 600.0, 1.3, 6.0 * 3600.0, 18.0 * 3600.0, 3000.0, 6000.0);
        for (int i = 0; i < kWavesSmall; ++i)
            make(w[j++], 15.0, 45.0, 0.75, 1500.0, 4200.0, 800.0, 2000.0);
        make(w[j++], 3000.0, 4000.0, 1.0, 4.0 * 86400.0, 6.0 * 86400.0, 0.0, 0.0); // the thermal-wind turning angle
    }
};
const WindWaves &windWaves()
{
    static const WindWaves ww;
    return ww;
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
    // (the fuel less the built-up share: no fire starts in a town or city, 2026-10-24)
    const float fuel = bio.burnableAt(lat, lon);
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
    // The wind it spreads with: the wind field near the ground a third of the way through its life (windAt: the same
    // field its smoke follows, so neighbouring fires agree).
    {
        const double la = lat * kPi / 180.0, lo = lon * kPi / 180.0;
        const glm::dvec2 w = windAt(glm::dvec3(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)),
                                    10.0, f.t0 + 0.3 * f.life);
        f.windAz = std::atan2(w.x, w.y);
    }
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
    s.windAz = f.windAz;
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

double fireFrontRadiusK(const Fire &f, const FireState &s, double a)
{
    // Fingers: two scales of a radial perturbation, stronger toward the head (where the front runs); they change as the
    // fire grows.
    const double head = 0.5 * (1.0 + std::cos(a));
    const double n1 = circNoise(a, 9, f.id ^ 0xF1A9ull, s.ageS, 20.0 * 3600.0) - 0.5;
    const double n2 = circNoise(a, 31, f.id ^ 0xF2A9ull, s.ageS, 7.0 * 3600.0) - 0.5;
    return 1.0 + (0.10 + 0.14 * head) * n1 + 0.06 * n2;
}

glm::dvec2 fireFrontAt(const Fire &f, const FireState &s, double a, double *dsda)
{
    const glm::dvec2 w(std::sin(s.windAz), std::cos(s.windAz)), c(std::cos(s.windAz), -std::sin(s.windAz));
    const double semi = 0.5 * (s.headM + s.backM), off = 0.5 * (s.headM - s.backM);
    const double k = fireFrontRadiusK(f, s, a);
    if (dsda)
        *dsda = k * std::sqrt(semi * semi * std::sin(a) * std::sin(a) +
                              s.halfWidthM * s.halfWidthM * std::cos(a) * std::cos(a));
    return w * (off + k * semi * std::cos(a)) + c * (k * s.halfWidthM * std::sin(a));
}

void fireOffsetLatLon(const Fire &f, const glm::dvec2 &q, double &latDeg, double &lonDeg)
{
    latDeg = f.latDeg + q.y / 111195.0;
    lonDeg = f.lonDeg + q.x / (111195.0 * std::max(std::cos(f.latDeg * kPi / 180.0), 0.05));
}

double fireFrontHeat(const Fire &f, const FireState &s, double a, double t, const BiomeMap *bio)
{
    // The ACTIVE ARC (2026-10-24): a new fire burns all round; within ~half a day only the head and the upper flanks are
    // still going (the back and the lower flanks have burnt out or crept to a stop against what has burnt). Its half
    // angle A (of the ellipse's parameter) narrows from ~165 deg to 50-80 deg by the fire: the head and the upper flanks
    // of an ellipse ~3x as long as it is wide. The arc is turned off the axis by a slowly changing angle (one flank
    // usually runs harder). It replaced a ring at 0.1 (the back) .. 1 (the head) with 12% floors in every gap: summed
    // along the whole perimeter, the points drew a closed bright ring.
    const double hd = 0.5 * (1.0 + std::cos(a));
    const double mature = sstep(1.0 * 3600.0, 14.0 * 3600.0, s.ageS);
    const double A = 2.9 + (0.85 + 0.55 * u01(mix64(f.id ^ 0xA1C0ull)) - 2.9) * mature;
    const double skew = (circNoise(0.0, 4, f.id ^ 0xA1C1ull, t, 9.0 * 3600.0) - 0.5) * 1.1 * mature;
    const double cA = std::cos(a - skew) - std::cos(A);
    const double env = sstep(-0.1, 0.22, cA);
    const double shape = 0.25 + 0.75 * std::pow(hd, 1.5);
    // Runs of flame and gaps along it: a coarse pattern (~18-32 runs round by the fire's size, re-drawn over ~1.5 h)
    // thresholded so ~40% of the head and more of the flanks are out, nothing burning in a gap; a finer variation in
    // each run.
    const int N1 = 18 + (int)(14.0 * f.size);
    const double n1 = circNoise(a, N1, f.id ^ 0xB0A1ull, t, 1.5 * 3600.0);
    const double thr = 0.40 + 0.16 * (1.0 - hd);
    const double run = sstep(thr, thr + 0.12, n1);
    const double n2 = circNoise(a, 120, f.id ^ 0xB0A2ull, t, 0.6 * 3600.0);
    const double fine = 0.45 + 1.1 * n2;
    // Isolated flare-ups off the arc (a stump, a pocket of fuel), fewer as the fire ages.
    const double n3 = circNoise(a, 44, f.id ^ 0xB0A3ull, t, 2.5 * 3600.0);
    const double iso = (1.0 - env) * 0.3 * sstep(0.84, 0.92, n3) * (1.0 - 0.6 * sstep(12.0 * 3600.0, 48.0 * 3600.0, s.ageS));
    double h = (env * shape * run + iso) * fine;
    if (bio && h > 0.0)
    {
        // The front stops where nothing burns (a lake, a town, bare ground): the biome map at the front's point.
        double la, lo;
        fireOffsetLatLon(f, fireFrontAt(f, s, a), la, lo);
        h *= sstep(0.08, 0.3, (double)bio->burnableAt(la, lo));
    }
    return h;
}

double fireFlameHeightM(const Fire &f, const FireState &s)
{
    // A grass fire's flames ~1.5 m, a shrub or surface forest fire's 3-8 m, a crown fire's 20-40 m: by the fire's size
    // and its activity now (lower at night).
    return (1.5 + 26.0 * std::pow((double)f.size, 1.5) * (0.4 + 0.6 * (double)f.fuel)) * (0.35 + 0.65 * s.activity);
}

double fireFlameDepthM(const Fire &f) { return 6.0 + 34.0 * (double)f.size; }

glm::dvec2 windAt(const glm::dvec3 &dirEcef, double hM, double t)
{
    const WindWaves &ww = windWaves();
    const glm::dvec3 up = glm::normalize(dirEcef);
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    const double lat = std::asin(std::clamp(up.z, -1.0, 1.0)) * 180.0 / kPi, alat = std::fabs(lat);
    const double sgn = lat >= 0.0 ? 1.0 : -1.0;
    const double h = std::max(hM, 0.0);
    const glm::dvec3 X = up * 6371000.0;
    // The zonal climatology (m/s east, north): trades toward the west and the equator, westerlies, polar easterlies.
    auto g = [](double x, double m, double sd) { return std::exp(-((x - m) / sd) * ((x - m) / sd)); };
    const double trade = g(alat, 13.0, 10.0), west = g(alat, 46.0, 13.0), polar = g(alat, 75.0, 9.0);
    glm::dvec2 base(-6.5 * trade * std::max(1.0 - h / 7000.0, 0.2) + 8.5 * west * (1.0 + h / 4000.0) - 3.0 * polar,
                    -2.5 * sgn * trade);
    // The eddies: the curl of the streamfunction (up x grad psi).
    auto curl = [&](int i0, int i1) {
        glm::dvec3 gr(0.0);
        for (int i = i0; i < i1; ++i)
        {
            const WindWave &w = ww.w[i];
            gr += w.k * (w.amp * std::cos(glm::dot(w.k, X) + w.omega * t + w.phase + w.hPhase * h));
        }
        const glm::dvec3 v = glm::cross(up, gr);
        return glm::dvec2(glm::dot(v, E), glm::dot(v, N));
    };
    glm::dvec2 large = base + curl(0, kWavesSyn) + curl(kWavesSyn, kWavesSyn + kWavesMeso);
    // Near the ground slower and turned toward low pressure (backed: counterclockwise in the north); above, the
    // thermal-wind turning grows with height.
    const double fr = sstep(0.0, 1000.0, h);
    const double speedK = 0.55 + 0.45 * fr + 0.1 * h / 1000.0;
    const WindWave &tw = ww.w[kWavesSyn + kWavesMeso + kWavesSmall];
    const double gam = 0.6 * std::sin(glm::dot(tw.k, X) + tw.omega * t + tw.phase);
    const double beta = sgn * 0.42 * (1.0 - fr) + gam * std::min(h, 6000.0) / 3000.0;
    const double cb = std::cos(beta), sb = std::sin(beta);
    large = glm::dvec2(cb * large.x - sb * large.y, sb * large.x + cb * large.y) * speedK;
    return large + curl(kWavesSyn + kWavesMeso, kWavesSyn + kWavesMeso + kWavesSmall);
}

double fireInjectionHeightM(const Fire &f, const FireState &s, double t)
{
    const double la = f.latDeg * kPi / 180.0, lo = f.lonDeg * kPi / 180.0;
    const glm::dvec3 G(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
    const glm::dvec2 u = windAt(G, 0.5 * s.plumeTopM, t);
    return s.plumeTopM * std::clamp(std::cbrt(6.0 / (glm::length(u) + 2.0)), 0.65, 1.25);
}

double fireUpdraftMps(const Fire &f) { return 5.0 + 9.0 * (double)f.size + 5.0 * (double)f.pyro; }

double fireSmokeRiseM(double ageS, double Hb, double w0)
{
    const double e = std::exp(-ageS * w0 / std::max(Hb, 1.0));
    return Hb * (1.0 - e) + (30.0 + 0.08 * Hb) * e;
}

glm::dvec2 fireSmokeSource(const Fire &f, const FireState &s, double t)
{
    const double la = f.latDeg * kPi / 180.0, lo = f.lonDeg * kPi / 180.0;
    const glm::dvec3 G(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
    const glm::dvec2 u = windAt(G, 10.0, t);
    const glm::dvec2 w = glm::length(u) > 0.05 ? u / glm::length(u) : glm::dvec2(std::sin(s.windAz), std::cos(s.windAz));
    return fireFrontAt(f, s, 0.0) + w * (250.0 + 0.1 * s.halfWidthM);
}

glm::dvec3 fireSmokeAt(const Fire &f, const FireState &s, double t, double ageS, double Hb, int steps)
{
    const double la = f.latDeg * kPi / 180.0, lo = f.lonDeg * kPi / 180.0;
    const glm::dvec3 G(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
    const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), G) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 nE = glm::cross(G, eE);
    const double w0 = fireUpdraftMps(f);
    glm::dvec2 xy = fireSmokeSource(f, s, t);
    double z0 = fireSmokeRiseM(0.0, Hb, w0), ta = 0.0;
    const double t1 = std::max(ageS, 1.0), tA = std::min(60.0, t1);
    for (int j = 0; j <= steps; ++j)
    {
        const double tb = j == 0 ? tA : tA * std::pow(t1 / tA, (double)j / steps);
        const double zb = fireSmokeRiseM(tb, Hb, w0);
        const glm::dvec3 d = glm::normalize(G + (eE * xy.x + nE * xy.y) / 6371000.0);
        xy += windAt(d, 0.5 * (z0 + zb), t - 0.5 * (ta + tb)) * (tb - ta);
        z0 = zb;
        ta = tb;
    }
    return glm::dvec3(xy, z0);
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

void firesBornRows(int64_t binFrom, int64_t binTo, int rowFrom, int rowTo, double latDeg, double lonDeg, double radiusKm,
                   const BiomeMap &bio, float rateScale, std::vector<Fire> &out)
{
    if (!bio.ready() || rateScale <= 0.0f || binTo < binFrom || rowTo < rowFrom) return;
    scanCells(std::numeric_limits<double>::quiet_NaN(), binFrom, binTo, rowFrom, rowTo, latDeg, lonDeg, radiusKm, false, bio,
              rateScale, out);
}

void firesWorld(double t, const BiomeMap &bio, float rateScale, std::vector<Fire> &out)
{
    out.clear();
    if (!bio.ready() || rateScale <= 0.0f) return;
    scanCells(t, (int64_t)std::floor((t - kFireMaxLifeS) / 86400.0), (int64_t)std::floor(t / 86400.0), -90, 89, 0.0,
              0.0, 0.0, true, bio, rateScale, out);
}

} // namespace wx
