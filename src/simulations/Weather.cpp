#include "Weather.h"
#include "StormClimatology.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <map>
#include <thread>

namespace wx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kDay = 86400.0;
constexpr double kREarthM = 6371000.0;
constexpr double kREarthKm = 6371.0;
constexpr int kBandDeg = 2;
constexpr int kBands = 180 / kBandDeg;
constexpr int kMaxCandidates = 256;    // per cell and bin; lambda is ~1, ~15 over the worst hotspot
constexpr double kMin = 60.0;

uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double u53(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }

uint64_t key3(uint64_t a, uint64_t b, uint64_t c, uint64_t salt) {
    uint64_t h = mix64(a * 0x2545F4914F6CDD1Dull ^ salt);
    h = mix64(h ^ (b * 0x9E3779B97F4A7C15ull));
    return mix64(h ^ (c * 0xC2B2AE3D27D4EB4Full));
}

// A sequence of uniforms from one seed (splitmix64): each storm draws its own, in a fixed order.
struct Rng {
    uint64_t s;
    explicit Rng(uint64_t seed) : s(seed) {}
    uint64_t nextU64() {
        s += 0x9E3779B97F4A7C15ull;
        uint64_t z = s;
        z = (z ^ (z >> 30)) * 0xBF58476D1CE4E5B9ull;
        z = (z ^ (z >> 27)) * 0x94D049BB133111EBull;
        return z ^ (z >> 31);
    }
    double u() { return u53(nextU64()); }
    double range(double a, double b) { return a + (b - a) * u(); }
};

double smooth01(double e0, double e1, double x) {
    double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double dayOfYear(double t) {
    double y = (t / kDay + 0.5) / 365.2422;
    return (y - std::floor(y)) * 365.2422;
}

double wrapDeg(double a) {
    a = std::fmod(a + 180.0, 360.0);
    if (a < 0.0) a += 360.0;
    return a - 180.0;
}

void eastNorth(const glm::dvec3 &p, glm::dvec3 &e, glm::dvec3 &n) {
    e = glm::dvec3(-p.y, p.x, 0.0);
    double l = glm::length(e);
    e = (l > 1e-12) ? e / l : glm::dvec3(1.0, 0.0, 0.0);
    n = glm::cross(p, e);
}

// A small offset on the sphere from p along the tangent t (metres): exact rotation.
glm::dvec3 moveAlong(const glm::dvec3 &p, const glm::dvec3 &t, double distM) {
    if (distM <= 0.0) return p;
    const double a = distM / kREarthM;
    return glm::normalize(p * std::cos(a) + t * std::sin(a));
}

// Poisson draw by inversion of one uniform (a normal approximation past lambda 200, never reached at x1).
int poisson(double u, double lambda) {
    if (lambda > 200.0) {
        const double u2 = u53(mix64((uint64_t)(u * 9007199254740992.0) ^ 0x7E57ull));
        const double z = std::sqrt(-2.0 * std::log(std::max(u, 1e-300))) * std::cos(2.0 * kPi * u2);
        return std::clamp((int)std::lround(lambda + std::sqrt(lambda) * z), 0, kMaxCandidates);
    }
    double p = std::exp(-lambda), F = p;
    int k = 0;
    while (u > F && k < kMaxCandidates) {
        ++k;
        p *= lambda / (double)k;
        F += p;
        if (p < 1e-300) break;
    }
    return k;
}

// The kind mix at a birth: share of supercells and multicells (the rest air-mass).
void kindMix(double latDeg, double land, double doy, double &pSuper, double &pMulti) {
    const double a = std::fabs(latDeg);
    const double midLat = smooth01(20.0, 25.0, a) * (1.0 - smooth01(50.0, 56.0, a));
    // Warm season: the hemisphere's late spring / summer (the severe season peaks ~May-July in the north).
    const double peak = (latDeg >= 0.0) ? 165.0 : 165.0 + 182.6;
    const double season = 0.5 + 0.5 * std::cos(2.0 * kPi * (doy - peak) / 365.2422);
    const double warm = smooth01(0.25, 0.85, season);
    pSuper = 0.01 + 0.14 * midLat * land * warm;
    const double tropSea = (1.0 - smooth01(15.0, 25.0, a)) * (1.0 - land);
    pMulti = 0.35 + 0.05 * tropSea;
}

// x in [0, 1] with x^2 (3 - 2x) = y (the inverse of smooth01's polynomial), by bisection.
double invSmooth(double y) {
    y = std::clamp(y, 0.0, 1.0);
    double lo = 0.0, hi = 1.0;
    for (int i = 0; i < 30; ++i) {
        const double m = 0.5 * (lo + hi);
        ((m * m * (3.0 - 2.0 * m)) < y ? lo : hi) = m;
    }
    return 0.5 * (lo + hi);
}

// How favourable a place and moment is for ORGANISED convection (squall lines, MCSs) beyond what the flash
// climatology already says: warm-season continents in the mid-latitudes (the US Plains, La Plata), the Sahel and
// India's monsoon band, and the tropical oceans' squall lines. 0..1.
double lineSystemFactor(double latDeg, double land, double doy) {
    const double a = std::fabs(latDeg);
    const double peak = (latDeg >= 0.0) ? 175.0 : 175.0 + 182.6;
    const double season = 0.5 + 0.5 * std::cos(2.0 * kPi * (doy - peak) / 365.2422);
    const double midLat = smooth01(25.0, 32.0, a) * (1.0 - smooth01(48.0, 55.0, a));
    const double sub = smooth01(5.0, 10.0, a) * (1.0 - smooth01(18.0, 24.0, a));   // the Sahel / monsoon band
    const double trop = 1.0 - smooth01(12.0, 20.0, a);
    return land * (0.9 * midLat * smooth01(0.3, 0.85, season) + 0.8 * sub * smooth01(0.2, 0.7, season))
         + (1.0 - land) * (0.45 * trop + 0.15 * midLat * smooth01(0.4, 0.9, season));
}

struct Cell {
    double lat0, lat1, lon0, lon1;   // radians, map frame
    double sin0, sin1;
    glm::dvec3 centre;
    double areaKm2;
};

} // namespace

glm::dvec3 Storm::centreAt(double t) const {
    const double sp = glm::length(vel);
    if (sp < 1e-9) return pos0;
    return moveAlong(pos0, vel / sp, sp * (t - t0));
}

const char *stormKindName(StormKind k) {
    switch (k) {
    case StormKind::AirMass:    return "air-mass";
    case StormKind::Multicell:  return "multicell";
    case StormKind::Supercell:  return "supercell";
    case StormKind::SquallLine: return "squall line";
    case StormKind::Mcs:        return "MCS";
    }
    return "storm";
}

bool Catalog::towerCycleAt(const TowerSpec &ts, double tau, float &strength, float &age, float &life,
                           uint32_t &seed, float &shift) {
    strength = 0.0f; age = 0.0f; life = ts.life; seed = ts.seed; shift = 0.0f;
    if (ts.period <= 0.0f) {
        strength = towerStrength(ts, tau);
        age = (float)(tau - (double)ts.t0);
        return strength > 0.0f;
    }
    const double per = (double)ts.period, x = tau - (double)ts.t0;
    if (x < 0.0) return false;
    const int64_t c0 = (int64_t)std::floor(x / per);
    for (int64_t c = c0; c >= std::max<int64_t>(0, c0 - 1); --c) {
        const uint64_t h = mix64((uint64_t)ts.seed * 0x9E3779B97F4A7C15ull + (uint64_t)c);
        const double start = (double)ts.t0 + (double)c * per + 0.15 * per * u53(h);   // since the storm's birth
        const double lt = tau - start;
        if (lt <= 0.0 || lt >= (double)ts.life) continue;
        const double env = smooth01((double)ts.sysOn, (double)ts.sysOn + (double)ts.sysRamp, start)
                         * (1.0 - smooth01((double)ts.sysOff - (double)ts.sysRamp, (double)ts.sysOff, start));
        if (env <= 0.0) continue;
        const double pk = (double)ts.peak * env * (0.8 + 0.2 * u53(mix64(h ^ 0x51ull)));
        const double g = (ts.growS > 0.0f) ? smooth01(0.0, (double)ts.growS, lt) : 1.0;
        const double d = (ts.decayS > 0.0f) ? smooth01(0.0, (double)ts.decayS, (double)ts.life - lt) : 1.0;
        const float s = (float)(pk * g * d);
        if (s > strength) {
            strength = s;
            age = (float)lt;
            seed = (uint32_t)(mix64(h ^ 0x5EEDull) >> 32);
            shift = (float)(u53(mix64(h ^ 0x5417ull)) - 0.5);
        }
    }
    return strength > 0.0f;
}

float towerStrength(const TowerSpec &ts, double tau) {
    if (ts.period > 0.0f) {
        float s, a, l, sh;
        uint32_t sd;
        Catalog::towerCycleAt(ts, tau, s, a, l, sd, sh);
        return s;
    }
    tau -= (double)ts.t0;
    const double life = (double)ts.life;
    if (tau <= 0.0 || tau >= life || ts.peak <= 0.0f) return 0.0f;
    const double g = (ts.growS > 0.0f) ? smooth01(0.0, (double)ts.growS, tau) : 1.0;
    const double d = (ts.decayS > 0.0f) ? smooth01(0.0, (double)ts.decayS, life - tau) : 1.0;
    return (float)((double)ts.peak * g * d);
}

struct Catalog::Impl {
    Env env;
    Settings set;
    const StormClimatology *clim = nullptr;
    uint64_t envKey = 0;
    bool configured = false;
    double K = 0.0;                  // storms per (flash / km^2 / yr) per km^2 per s
    double KLine = 0.0;              // line systems per (flash density x line factor) per km^2 per s
    double meanLife[kStormKinds] = {0, 0, 0, 0, 0};  // per kind, seconds
    std::vector<Cell> cells;
    std::vector<Cell> lineCells;     // coarser (6-degree bands): line systems are rare
    std::map<int64_t, std::vector<Storm>> bins;
    std::map<int64_t, std::vector<Storm>> lineBins;
    // Each bin's storms' (birth, end) packed: stormsAt scans these, not the ~1.3 KB Storm records (a cache miss each).
    std::map<int64_t, std::vector<glm::dvec2>> binT, lineBinT;
    static const std::vector<glm::dvec2> &timesOf(std::map<int64_t, std::vector<glm::dvec2>> &tm, int64_t b,
                                                 const std::vector<Storm> &v) {
        std::vector<glm::dvec2> &T = tm[b];
        if (T.size() != v.size()) {
            T.resize(v.size());
            for (size_t i = 0; i < v.size(); ++i) T[i] = glm::dvec2(v[i].t0, v[i].t0 + (double)v[i].life);
        }
        return T;
    }
    Stats st;
    int threads = 1;

    double flash(double latDeg, double lonDeg, double doy, double hour, float land) const {
        if (!clim) return 1.0;
        return (double)clim->flashDensity(latDeg, lonDeg, doy, hour, land);
    }

    void buildCells() {
        cells.clear();
        for (int b = 0; b < kBands; ++b) {
            const double la0 = (-90.0 + b * kBandDeg) * kDeg, la1 = la0 + kBandDeg * kDeg;
            const double mid = 0.5 * (la0 + la1);
            const int nLon = std::max(1, (int)std::lround(180.0 * std::cos(mid)));
            const double dLon = 2.0 * kPi / nLon;
            for (int j = 0; j < nLon; ++j) {
                Cell c;
                c.lat0 = la0; c.lat1 = la1;
                c.lon0 = -kPi + j * dLon; c.lon1 = c.lon0 + dLon;
                c.sin0 = std::sin(la0); c.sin1 = std::sin(la1);
                const double sMid = 0.5 * (c.sin0 + c.sin1), cMid = std::sqrt(std::max(0.0, 1.0 - sMid * sMid));
                const double lonMid = 0.5 * (c.lon0 + c.lon1);
                c.centre = glm::dvec3(cMid * std::cos(lonMid), cMid * std::sin(lonMid), sMid);
                c.areaKm2 = kREarthKm * kREarthKm * dLon * (c.sin1 - c.sin0);
                cells.push_back(c);
            }
        }
    }

    void buildLineCells() {
        lineCells.clear();
        constexpr int kLB = 6;
        for (int b = 0; b < 180 / kLB; ++b) {
            const double la0 = (-90.0 + b * kLB) * kDeg, la1 = la0 + kLB * kDeg;
            const double mid = 0.5 * (la0 + la1);
            const int nLon = std::max(1, (int)std::lround(60.0 * std::cos(mid)));
            const double dLon = 2.0 * kPi / nLon;
            for (int j = 0; j < nLon; ++j) {
                Cell c;
                c.lat0 = la0; c.lat1 = la1;
                c.lon0 = -kPi + j * dLon; c.lon1 = c.lon0 + dLon;
                c.sin0 = std::sin(la0); c.sin1 = std::sin(la1);
                const double sMid = 0.5 * (c.sin0 + c.sin1), cMid = std::sqrt(std::max(0.0, 1.0 - sMid * sMid));
                const double lonMid = 0.5 * (c.lon0 + c.lon1);
                c.centre = glm::dvec3(cMid * std::cos(lonMid), cMid * std::sin(lonMid), sMid);
                c.areaKm2 = kREarthKm * kREarthKm * dLon * (c.sin1 - c.sin0);
                lineCells.push_back(c);
            }
        }
    }

    // The anvil's linger and drift, drawn AFTER everything a storm's towers took from its Rng (so adding them kept
    // every storm's towers as they were). Life then reaches to the linger's end.
    void finishAnvil(Storm &S, Rng &r) const {
        const double ls = std::max(0.05, (double)set.lifeScale);
        double towersEnd = 0.0;
        bool anvil = false;
        for (int i = 0; i < S.towerCount; ++i) {
            const TowerSpec &ts = S.towers[i];
            towersEnd = std::max(towersEnd, (double)ts.t0 + (double)ts.life);
            anvil |= ts.role >= 0.99f && ts.peak >= 0.45f;
        }
        // Each anvil-reaching tower's span above strength 0.42 (cv2ColumnSigma's "full": the head flares).
        for (int i = 0; i < S.towerCount; ++i) {
            TowerSpec &ts = S.towers[i];
            if (ts.role < 0.99f || ts.peak < 0.44f || ts.period > 0.0f) continue;
            const double q = invSmooth(0.42 / (double)ts.peak);
            ts.anvOn = (float)((double)ts.t0 + (double)ts.growS * q);
            ts.anvOff = (float)((double)ts.t0 + (double)ts.life - (double)ts.decayS * q);
        }
        const double lo = S.kind == StormKind::Supercell ? 60.0 : S.kind == StormKind::Multicell ? 45.0 : 35.0;
        const double li = r.range(lo, 2.0 * lo) * kMin * ls;
        S.upperWind = (float)r.range(6.0, 18.0);
        S.linger = anvil ? (float)std::min(li, std::max(0.0, kMaxLifeS - towersEnd)) : 0.0f;
        S.life = (float)(towersEnd + S.linger);
    }

    // A squall line or an MCS from its seed: a line (an arc for an MCS) of regenerating towers across the motion,
    // a shared shield trailing rearward over a stratiform rain region.
    void makeLineSystem(Storm &S, uint64_t id, const glm::dvec3 &pos, double t0, double latDeg, StormKind kind) const {
        Rng r(mix64(id ^ 0x11E5A57Eull));
        S = Storm{};
        S.id = id;
        S.t0 = t0;
        S.pos0 = pos;
        S.kind = kind;
        const bool mcs = kind == StormKind::Mcs;
        glm::dvec3 e, n;
        eastNorth(pos, e, n);
        // Motion: with the westerlies poleward of ~20 deg, the easterlies (and their waves) equatorward of ~12.
        const double w = smooth01(12.0, 20.0, std::fabs(latDeg));
        const double westAz = (latDeg >= 0.0) ? kPi : -kPi;
        const double motAz = westAz * (1.0 - w) + r.range(-35.0, 35.0) * kDeg;
        const double speed = mcs ? r.range(6.0, 13.0) : r.range(9.0, 17.0);
        S.vel = (e * std::cos(motAz) + n * std::sin(motAz)) * speed;
        // Its anvil blows rearward (the storm-relative flow aloft is front to rear over a trailing-stratiform line).
        const double shAz = motAz + kPi + r.range(-20.0, 20.0) * kDeg;
        S.shear = glm::vec3(e * std::cos(shAz) + n * std::sin(shAz));
        S.lineAz = (float)(motAz + 0.5 * kPi + r.range(-25.0, 25.0) * kDeg);
        S.lineHalfM = (float)(1000.0 * (mcs ? r.range(50.0, 110.0) : r.range(90.0, 260.0)));
        S.trailM = (float)(1000.0 * (mcs ? r.range(110.0, 200.0) : r.range(45.0, 100.0)));
        S.bow = (float)(S.lineHalfM * (mcs ? r.range(0.25, 0.55) : r.range(0.0, 0.35)));
        const double ls = std::max(0.05, (double)set.lifeScale);
        const double sysRamp = r.range(1.2, 2.0) * 3600.0 * ls;
        double sysLife = (mcs ? r.range(8.0, 15.0) : r.range(6.0, 11.0)) * 3600.0 * ls;
        double linger = r.range(1.5, 3.0) * 3600.0 * ls;
        sysLife = std::min(sysLife, kLineMaxLifeS - 3600.0);
        linger = std::min(linger, kLineMaxLifeS - sysLife);
        S.sysLife = (float)sysLife;
        S.linger = (float)linger;
        S.upperWind = (float)r.range(8.0, 16.0);

        const double R = (double)set.towerRadiusM;
        const double spacing = 2.0 * R * r.range(0.9, 1.25);
        const int nt = std::clamp((int)std::lround(2.0 * S.lineHalfM / spacing) + 1, 3, 16);
        const double cl = std::cos((double)S.lineAz), sl = std::sin((double)S.lineAz);
        const double cm = std::cos(motAz), sm = std::sin(motAz);
        for (int i = 0; i < nt; ++i) {
            TowerSpec &ts = S.towers[i];
            const double xn = (nt > 1) ? -1.0 + 2.0 * i / (nt - 1) : 0.0;
            const double x = xn * (double)S.lineHalfM;
            // Forward of the centre: the bow, a little ragged; an MCS's cells straggle behind its arc.
            const double fwd = (double)S.bow * (1.0 - xn * xn)
                             + (mcs ? -r.range(0.0, 1.6) * R : r.range(-0.25, 0.25) * R);
            ts.offE = (float)(x * cl + fwd * cm);
            ts.offN = (float)(x * sl + fwd * sm);
            ts.period = (float)(r.range(45.0, 75.0) * kMin * ls);
            ts.life = (float)(std::min(r.range(40.0, 65.0) * kMin * ls, 0.9 * (double)ts.period));
            ts.growS = (float)(r.range(13.0, 19.0) * kMin * ls);
            ts.decayS = (float)(r.range(0.25, 0.35) * ts.life);
            if (ts.growS + ts.decayS > ts.life) {
                const float f = ts.life / (ts.growS + ts.decayS);
                ts.growS *= f;
                ts.decayS *= f;
            }
            ts.t0 = (float)(r.u() * ts.period);
            // The line forms from its middle out and its ends die first.
            ts.sysOn = (float)(0.6 * sysRamp * std::fabs(xn));
            ts.sysOff = (float)(sysLife * (1.0 - 0.25 * std::fabs(xn) * r.u()));
            ts.sysRamp = (float)sysRamp;
            ts.peak = (float)(mcs ? r.range(0.7, 1.0) : r.range(0.75, 1.0));
            ts.role = (r.u() < 0.75) ? 1.0f : 0.7f;
            ts.size = (float)r.range(0.3, 0.9);
            ts.overshoot = (float)r.u();
            ts.seed = (uint32_t)(r.nextU64() >> 32);
        }
        S.towerCount = (uint8_t)nt;
        S.life = (float)(sysLife + linger);
    }

    // A storm's whole life from its seed. lat/land/doy: the birth's (kind mix, shear).
    void makeStorm(Storm &S, uint64_t id, const glm::dvec3 &pos, double t0, double latDeg, double land,
                   double doy) const {
        Rng r(mix64(id ^ 0x5717A11Cull));
        S = Storm{};
        S.id = id;
        S.t0 = t0;
        S.pos0 = pos;

        double pSuper, pMulti;
        kindMix(latDeg, land, doy, pSuper, pMulti);
        const double uk = r.u();
        S.kind = (uk < pSuper) ? StormKind::Supercell : (uk < pSuper + pMulti) ? StormKind::Multicell : StormKind::AirMass;

        // Shear azimuth (radians from local east toward north): westerlies poleward of 20 deg, easterlies
        // equatorward of 12 deg, turned through the poleward direction between (continuous at the equator).
        const double w = smooth01(12.0, 20.0, std::fabs(latDeg));
        const double westAz = (latDeg >= 0.0) ? kPi : -kPi;
        const double shearAz = westAz * (1.0 - w) + r.range(-40.0, 40.0) * kDeg;
        glm::dvec3 e, n;
        eastNorth(pos, e, n);
        const glm::dvec3 sh = e * std::cos(shearAz) + n * std::sin(shearAz);
        S.shear = glm::vec3(sh);

        const double speed = r.range(1.0, 6.0);
        const double motAz = shearAz + r.range(-60.0, 60.0) * kDeg;
        S.vel = (e * std::cos(motAz) + n * std::sin(motAz)) * speed;

        const double R = (double)set.towerRadiusM;
        auto offsetAt = [&](TowerSpec &ts, double baseE, double baseN, double az, double dist) {
            ts.offE = (float)(baseE + dist * std::cos(az));
            ts.offN = (float)(baseN + dist * std::sin(az));
        };
        auto common = [&](TowerSpec &ts) {
            ts.seed = (uint32_t)(r.nextU64() >> 32);
        };

        switch (S.kind) {
        case StormKind::AirMass: {
            TowerSpec &ts = S.towers[0];
            ts.life = (float)(r.range(40.0, 90.0) * kMin);
            ts.growS = (float)(r.range(15.0, 25.0) * kMin);
            ts.decayS = (float)(r.range(0.25, 0.35) * ts.life);
            // ~half never pass 0.45: a congestus that never makes an anvil.
            ts.peak = (float)(0.25 + 0.75 * std::pow(r.u(), 1.9));
            ts.role = 1.0f;
            ts.size = (float)r.u();
            ts.overshoot = (float)r.u();
            common(ts);
            S.towerCount = 1;
        } break;
        case StormKind::Multicell: {
            const int nt = 2 + std::min(2, (int)(r.u() * 3.0));
            double t = 0.0, pe = 0.0, pn = 0.0;
            for (int i = 0; i < nt; ++i) {
                TowerSpec &ts = S.towers[i];
                if (i > 0) {
                    t += r.range(12.0, 30.0) * kMin;
                    // New cells on the propagation flank: the side opposite the shear.
                    const double az = shearAz + kPi + r.range(-50.0, 50.0) * kDeg;
                    offsetAt(ts, pe, pn, az, r.range(0.8, 1.4) * R);
                    pe = ts.offE; pn = ts.offN;
                }
                ts.t0 = (float)t;
                ts.life = (float)(r.range(40.0, 70.0) * kMin);
                ts.growS = (float)(r.range(12.0, 20.0) * kMin);
                ts.decayS = (float)(r.range(0.25, 0.35) * ts.life);
                ts.peak = (float)r.range(0.5, 1.0);
                ts.role = 1.0f;
                ts.size = (float)r.u();
                ts.overshoot = (float)r.u();
                common(ts);
            }
            S.towerCount = (uint8_t)nt;
        } break;
        case StormKind::Supercell: {
            TowerSpec &d = S.towers[0];
            d.life = (float)(r.range(2.0, 4.0) * 3600.0);
            d.growS = (float)(r.range(22.0, 28.0) * kMin);
            d.decayS = (float)(r.range(0.2, 0.3) * d.life);
            d.peak = (float)r.range(0.85, 1.0);
            d.role = 1.0f;
            d.size = (float)r.range(0.5, 1.0);
            d.overshoot = (float)(0.5 + 0.5 * std::sqrt(r.u()));
            common(d);
            const int nf = std::min(2, (int)(r.u() * 3.0));
            const double side = (r.u() < 0.5) ? 1.0 : -1.0;
            for (int i = 0; i < nf; ++i) {
                TowerSpec &ts = S.towers[1 + i];
                const double az = shearAz + kPi + side * (i == 0 ? 1.0 : -1.0) * r.range(10.0, 40.0) * kDeg;
                offsetAt(ts, 0.0, 0.0, az, r.range(0.9, 1.3) * R);
                ts.t0 = (float)(15.0 * kMin + r.u() * std::max(0.0, (double)d.life - 90.0 * kMin));
                ts.life = (float)(r.range(40.0, 80.0) * kMin);
                ts.growS = (float)(r.range(12.0, 20.0) * kMin);
                ts.decayS = (float)(r.range(0.25, 0.35) * ts.life);
                ts.peak = (float)r.range(0.5, 0.8);
                ts.role = (i == 0) ? 0.7f : 0.5f;
                ts.size = (float)r.range(0.2, 0.6);
                ts.overshoot = (float)r.u();
                common(ts);
            }
            S.towerCount = (uint8_t)(1 + nf);
        } break;
        }

        const double ls = std::max(0.05, (double)set.lifeScale);
        double life = 0.0;
        for (int i = 0; i < S.towerCount; ++i) {
            TowerSpec &ts = S.towers[i];
            double t0s = ts.t0 * ls, lf = ts.life * ls, g = ts.growS * ls, dc = ts.decayS * ls;
            t0s = std::min(t0s, kMaxLifeS - 60.0);
            lf = std::min(lf, kMaxLifeS - t0s);
            if (g + dc > lf) {
                const double f = lf / (g + dc);
                g *= f; dc *= f;
            }
            ts.t0 = (float)t0s; ts.life = (float)lf; ts.growS = (float)g; ts.decayS = (float)dc;
            life = std::max(life, (double)ts.t0 + (double)ts.life);
        }
        S.life = (float)life;
        finishAnvil(S, r);
    }

    // One line-bin's squall lines and MCSs (a pure function of the bin): the flash climatology's monthly mean x the
    // organised-convection factor x an evening peak over land, on coarse cells.
    void buildLineBin(int64_t bin, std::vector<Storm> &out) const {
        out.clear();
        if (!(KLine > 0.0)) return;
        const double binT0 = (double)bin * kLineBinS;
        const double tMid = binT0 + 0.5 * kLineBinS;
        const glm::dvec3 sun = env.sunEarth ? env.sunEarth(tMid) : glm::dvec3(1, 0, 0);
        const double sunLonDeg = std::atan2(sun.y, sun.x) / kDeg;
        const double doy = dayOfYear(tMid);
        const double freq = std::max(0.0, (double)set.lineFrequency) * std::max(0.0, (double)set.frequency);
        for (size_t ci = 0; ci < lineCells.size(); ++ci) {
            const Cell &c = lineCells[ci];
            const glm::dvec3 ce = env.mapToEarth(c.centre, tMid);
            const double latE = std::asin(std::clamp(ce.z, -1.0, 1.0)) / kDeg;
            const double lonE = std::atan2(ce.y, ce.x) / kDeg;
            const float land = env.land ? env.land(ce) : 0.0f;
            // Organised systems initiate late in the afternoon and evening over land, and grow through the night.
            double hour = 12.0 + wrapDeg(lonE - sunLonDeg) / 15.0;
            hour = std::fmod(hour + 24.0, 24.0);
            const double fd = clim ? (double)clim->monthlyMean(latE, lonE, doy, land) : 1.0;
            const double diurnal = 1.0 + 0.8 * (double)land * std::cos(2.0 * kPi * (hour - 19.0) / 24.0);
            const double lambda = KLine * freq * fd * lineSystemFactor(latE, land, doy) * diurnal * c.areaKm2 * kLineBinS;
            if (!(lambda > 0.0)) continue;
            const int nc = poisson(u53(key3((uint64_t)ci, (uint64_t)bin, 0xFFFEull, 0x11E5B1Aull)), lambda);
            for (int k = 0; k < nc; ++k) {
                Rng r(key3((uint64_t)ci, (uint64_t)bin, (uint64_t)k, 0x11E5CA4Dull));
                const double sLat = c.sin0 + (c.sin1 - c.sin0) * r.u();
                const double lon = c.lon0 + (c.lon1 - c.lon0) * r.u();
                const double tb = binT0 + kLineBinS * r.u();
                const double uKeep = r.u();
                const double uKind = r.u();
                const double cl = std::sqrt(std::max(0.0, 1.0 - sLat * sLat));
                const glm::dvec3 p(cl * std::cos(lon), cl * std::sin(lon), sLat);
                const double m = env.moisture ? (double)env.moisture(p) : 1.0;
                const double f = smooth01((double)set.moistureMin + 0.1, std::max((double)set.moistureFull,
                                          (double)set.moistureMin + 0.11), m);
                if (uKeep >= f) continue;
                const glm::dvec3 pe = env.mapToEarth(p, tb);
                const double landP = env.land ? (double)env.land(pe) : 0.0;
                const double latP = std::asin(std::clamp(sLat, -1.0, 1.0)) / kDeg;
                // MCSs are continental (and the monsoon's); over the sea it is mostly squall lines.
                const StormKind kind = (uKind < 0.25 + 0.4 * landP) ? StormKind::Mcs : StormKind::SquallLine;
                Storm S;
                const uint64_t id = key3((uint64_t)ci, (uint64_t)bin, (uint64_t)k, 0x11E5570Aull) | 1ull;
                makeLineSystem(S, id, p, tb, latP, kind);
                S.potential = (float)fd;
                S.moisture = (float)m;
                out.push_back(S);
            }
        }
    }

    // One time bin's storms: a pure function of (bin, Env, Settings, K).
    void buildCellRange(int64_t bin, size_t c0, size_t c1, double tMid, double sunLonDeg, double doy,
                        std::vector<Storm> &out) const {
        const double binT0 = (double)bin * kBinS;
        const double freq = std::max(0.0, (double)set.frequency);
        for (size_t ci = c0; ci < c1; ++ci) {
            const Cell &c = cells[ci];
            const glm::dvec3 e = env.mapToEarth(c.centre, tMid);
            const double latE = std::asin(std::clamp(e.z, -1.0, 1.0)) / kDeg;
            const double lonE = std::atan2(e.y, e.x) / kDeg;
            const float land = env.land ? env.land(e) : 0.0f;
            double hour = 12.0 + wrapDeg(lonE - sunLonDeg) / 15.0;
            hour = std::fmod(hour + 24.0, 24.0);
            const double fd = flash(latE, lonE, doy, hour, land);
            const double lambda = K * freq * fd * (1.0 + ((double)set.seaFactor - 1.0) * (1.0 - (double)land)) * c.areaKm2 * kBinS;
            if (!(lambda > 0.0)) continue;
            const int nc = poisson(u53(key3((uint64_t)ci, (uint64_t)bin, 0xFFFFull, 0x9015501ull)), lambda);
            for (int k = 0; k < nc; ++k) {
                Rng r(key3((uint64_t)ci, (uint64_t)bin, (uint64_t)k, 0xCA4D1DA7Eull));
                const double sLat = c.sin0 + (c.sin1 - c.sin0) * r.u();
                const double lon = c.lon0 + (c.lon1 - c.lon0) * r.u();
                const double tb = binT0 + kBinS * r.u();
                const double uKeep = r.u();
                const double cl = std::sqrt(std::max(0.0, 1.0 - sLat * sLat));
                const glm::dvec3 p(cl * std::cos(lon), cl * std::sin(lon), sLat);
                const double m = env.moisture ? (double)env.moisture(p) : 1.0;
                const double f = smooth01((double)set.moistureMin, std::max((double)set.moistureFull,
                                          (double)set.moistureMin + 1e-6), m);
                if (uKeep >= f) continue;
                const glm::dvec3 pe = env.mapToEarth(p, tb);
                const double landP = env.land ? (double)env.land(pe) : 0.0;
                Storm S;
                const uint64_t id = key3((uint64_t)ci, (uint64_t)bin, (uint64_t)k, 0x570A3ull);
                makeStorm(S, id, p, tb, std::asin(std::clamp(sLat, -1.0, 1.0)) / kDeg, landP, dayOfYear(tb));
                S.potential = (float)fd;
                S.moisture = (float)m;
                out.push_back(S);
            }
        }
    }

    void buildBin(int64_t bin, std::vector<Storm> &out, int nThreads) const {
        out.clear();
        const double tMid = ((double)bin + 0.5) * kBinS;
        const glm::dvec3 sun = env.sunEarth ? env.sunEarth(tMid) : glm::dvec3(1, 0, 0);
        const double sunLon = std::atan2(sun.y, sun.x) / kDeg;
        const double doy = dayOfYear(tMid);
        if (nThreads <= 1) {
            buildCellRange(bin, 0, cells.size(), tMid, sunLon, doy, out);
            return;
        }
        std::vector<std::vector<Storm>> parts((size_t)nThreads);
        std::vector<std::thread> th;
        const size_t per = (cells.size() + (size_t)nThreads - 1) / (size_t)nThreads;
        for (int i = 0; i < nThreads; ++i) {
            const size_t c0 = std::min(cells.size(), (size_t)i * per), c1 = std::min(cells.size(), c0 + per);
            th.emplace_back([&, i, c0, c1] { buildCellRange(bin, c0, c1, tMid, sunLon, doy, parts[(size_t)i]); });
        }
        for (auto &t : th) t.join();
        for (auto &p : parts) out.insert(out.end(), p.begin(), p.end());
    }

    void calibrate() {
        // Mean life per kind, from the generator itself (life depends only on the kind and the settings).
        constexpr int kSamples = 4096;
        for (int k = 3; k < kStormKinds; ++k) {
            double sum = 0.0;
            for (int i = 0; i < 512; ++i) {
                Storm S;
                makeLineSystem(S, key3((uint64_t)k, (uint64_t)i, 9, 0xCA11B9ull), glm::dvec3(1.0, 0.0, 0.0), 0.0, 35.0,
                               (StormKind)k);
                sum += (double)S.life;
            }
            meanLife[k] = sum / 512.0;
        }
        for (int k = 0; k < 3; ++k) {
            double sum = 0.0;
            int cnt = 0;
            for (int i = 0; cnt < kSamples && i < kSamples * 400; ++i) {
                // Draw storms where this kind is common and keep the ones of this kind.
                Storm S;
                const double lat = (k == 2) ? 38.0 : 5.0;
                const double land = (k == 2) ? 1.0 : 0.5;
                makeStorm(S, key3((uint64_t)k, (uint64_t)i, 7, 0xCA11B8ull),
                          glm::dvec3(std::cos(lat * kDeg), 0.0, std::sin(lat * kDeg)), 0.0, lat, land, 165.0);
                if ((int)S.kind != k) continue;
                sum += (double)S.life - (double)S.linger;   // the towers' life: the spawn rate stays as it was
                ++cnt;
            }
            meanLife[k] = cnt ? sum / cnt : 3600.0;
        }
        // Earth integral of flash density x area x the local mean life, over 12 mid-month days. The cells are an
        // equal-area quadrature of the sphere in any frame; the diurnal factor's daily mean is 1, so monthlyMean.
        double acc = 0.0;
        std::vector<float> land(cells.size());
        for (size_t i = 0; i < cells.size(); ++i) land[i] = env.land ? env.land(cells[i].centre) : 0.0f;
        for (int mo = 0; mo < 12; ++mo) {
            const double doy = (mo + 0.5) * 365.2422 / 12.0;
            for (size_t i = 0; i < cells.size(); ++i) {
                const Cell &c = cells[i];
                const double latD = std::asin(c.centre.z) / kDeg, lonD = std::atan2(c.centre.y, c.centre.x) / kDeg;
                const double fd = clim ? (double)clim->monthlyMean(latD, lonD, doy, land[i]) : 1.0;
                double pS, pM;
                kindMix(latD, land[i], doy, pS, pM);
                const double ml = pS * meanLife[2] + pM * meanLife[1] + (1.0 - pS - pM) * meanLife[0];
                acc += fd * (1.0 + ((double)set.seaFactor - 1.0) * (1.0 - (double)land[i])) * c.areaKm2 * ml;
            }
        }
        acc /= 12.0;
        K = (acc > 0.0) ? (double)set.targetActive / acc : 0.0;
        // Line systems: the same integral with their own factor and mean life (the two kinds' mix by land).
        double accL = 0.0;
        for (int mo = 0; mo < 12; ++mo) {
            const double doy = (mo + 0.5) * 365.2422 / 12.0;
            for (size_t i = 0; i < cells.size(); ++i) {
                const Cell &c = cells[i];
                const double latD = std::asin(c.centre.z) / kDeg, lonD = std::atan2(c.centre.y, c.centre.x) / kDeg;
                const double fd = clim ? (double)clim->monthlyMean(latD, lonD, doy, land[i]) : 1.0;
                const double pM = 0.25 + 0.4 * land[i];
                const double ml = pM * meanLife[4] + (1.0 - pM) * meanLife[3];
                accL += fd * lineSystemFactor(latD, land[i], doy) * c.areaKm2 * ml;
            }
        }
        accL /= 12.0;
        KLine = (accL > 0.0) ? (double)set.targetLineActive / accL : 0.0;
    }
};

Catalog::Catalog() : impl_(std::make_unique<Impl>()) {}
Catalog::~Catalog() = default;

void Catalog::configure(const Env &env, const Settings &s, const StormClimatology *clim, uint64_t envKey) {
    Impl &I = *impl_;
    const bool same = I.configured && envKey == I.envKey && clim == I.clim && s.frequency == I.set.frequency &&
                      s.targetActive == I.set.targetActive && s.moistureMin == I.set.moistureMin &&
                      s.moistureFull == I.set.moistureFull && s.towerRadiusM == I.set.towerRadiusM &&
                      s.lifeScale == I.set.lifeScale && s.lineFrequency == I.set.lineFrequency &&
                      s.targetLineActive == I.set.targetLineActive;
    I.env = env;
    if (same) return;
    I.set = s;
    I.clim = clim;
    I.envKey = envKey;
    I.bins.clear();
    I.lineBins.clear();
    I.binT.clear();
    I.lineBinT.clear();
    if (I.cells.empty()) I.buildCells();
    if (I.lineCells.empty()) I.buildLineCells();
    I.calibrate();
    I.configured = true;
}

void Catalog::setBuildThreads(int n) { impl_->threads = std::max(1, n); }
double Catalog::calibrationK() const { return impl_->K; }
double Catalog::calibrationKLine() const { return impl_->KLine; }
double Catalog::meanLifeS(StormKind k) const { return impl_->meanLife[(int)k]; }

void Catalog::stormsInBin(int64_t bin, std::vector<const Storm *> &out) {
    Impl &I = *impl_;
    out.clear();
    if (!I.configured) return;
    auto it = I.bins.find(bin);
    if (it == I.bins.end()) {
        std::vector<Storm> v;
        I.buildBin(bin, v, I.threads);
        it = I.bins.emplace(bin, std::move(v)).first;
    }
    for (const Storm &S : it->second) out.push_back(&S);
}

void Catalog::stormsAt(double t, std::vector<const Storm *> &out) {
    Impl &I = *impl_;
    out.clear();
    I.st.binsBuiltLastCall = 0;
    I.st.buildMsLastCall = 0.0;
    if (!I.configured) return;
    const int64_t b1 = (int64_t)std::floor(t / kBinS);
    const int64_t b0 = (int64_t)std::floor((t - kMaxLifeS) / kBinS);
    const int64_t keepLo = (int64_t)std::floor((t - kMaxLifeS - 2.0 * kBinS) / kBinS), keepHi = b1 + 4;
    for (auto it = I.bins.begin(); it != I.bins.end();)
        if (it->first < keepLo || it->first > keepHi) { I.binT.erase(it->first); it = I.bins.erase(it); }
        else ++it;

    std::vector<int64_t> missing;
    for (int64_t b = b0; b <= b1; ++b)
        if (I.bins.find(b) == I.bins.end()) missing.push_back(b);
    if (!missing.empty()) {
        const auto c0 = std::chrono::steady_clock::now();
        std::vector<std::vector<Storm>> built(missing.size());
        if (I.threads > 1 && missing.size() > 1) {
            // A jump: one bin per thread (each a pure function of its index).
            std::vector<std::thread> th;
            const size_t nT = std::min(missing.size(), (size_t)I.threads);
            for (size_t w = 0; w < nT; ++w)
                th.emplace_back([&, w] {
                    for (size_t j = w; j < missing.size(); j += nT) I.buildBin(missing[j], built[j], 1);
                });
            for (auto &x : th) x.join();
        } else {
            for (size_t j = 0; j < missing.size(); ++j) I.buildBin(missing[j], built[j], I.threads);
        }
        for (size_t j = 0; j < missing.size(); ++j) I.bins.emplace(missing[j], std::move(built[j]));
        I.st.binsBuiltLastCall = (int)missing.size();
        I.st.buildMsLastCall =
            std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
    }
    for (int64_t b = b0; b <= b1; ++b) {
        auto it = I.bins.find(b);
        if (it == I.bins.end()) continue;
        const std::vector<glm::dvec2> &T = Impl::timesOf(I.binT, b, it->second);
        for (size_t i = 0; i < T.size(); ++i)
            if (t >= T[i].x && t < T[i].y) out.push_back(&it->second[i]);
    }
    // The line systems' own bins (3 h, looking back kLineMaxLifeS).
    {
        const int64_t l1 = (int64_t)std::floor(t / kLineBinS);
        const int64_t l0 = (int64_t)std::floor((t - kLineMaxLifeS) / kLineBinS);
        for (auto it = I.lineBins.begin(); it != I.lineBins.end();)
            if (it->first < l0 - 2 || it->first > l1 + 2) { I.lineBinT.erase(it->first); it = I.lineBins.erase(it); }
            else ++it;
        int nLine = 0;
        for (int64_t b = l0; b <= l1; ++b) {
            auto it = I.lineBins.find(b);
            if (it == I.lineBins.end()) {
                const auto c0 = std::chrono::steady_clock::now();
                std::vector<Storm> v;
                I.buildLineBin(b, v);
                it = I.lineBins.emplace(b, std::move(v)).first;
                ++I.st.binsBuiltLastCall;
                I.st.buildMsLastCall +=
                    std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
            }
            const std::vector<glm::dvec2> &T = Impl::timesOf(I.lineBinT, b, it->second);
            for (size_t i = 0; i < T.size(); ++i)
                if (t >= T[i].x && t < T[i].y) { out.push_back(&it->second[i]); ++nLine; }
        }
        I.st.lineSystemsAlive = nLine;
    }
    I.st.binsCached = (int)(I.bins.size() + I.lineBins.size());
    I.st.stormsAlive = (int)out.size();
}

void Catalog::towersAt(double t, std::vector<Tower> &out) {
    std::vector<const Storm *> storms;
    stormsAt(t, storms);
    towersAt(t, storms, out);
}

void Catalog::towersAt(double t, const std::vector<const Storm *> &storms, std::vector<Tower> &out) {
    out.clear();
    for (const Storm *S : storms) {
        const glm::dvec3 c = S->centreAt(t);
        glm::dvec3 e, n;
        eastNorth(c, e, n);
        const double tau = t - S->t0;
        const bool line = isLineSystem(S->kind);
        const double la = (double)S->lineAz;
        for (int i = 0; i < S->towerCount; ++i) {
            const TowerSpec &ts = S->towers[i];
            float s, cAge, cLife, shift;
            uint32_t cSeed;
            if (!towerCycleAt(ts, tau, s, cAge, cLife, cSeed, shift)) continue;
            Tower T;
            glm::dvec3 o = e * (double)ts.offE + n * (double)ts.offN;
            // Each new cell of a line forms a little along the line from the last (a regenerating slot moves).
            if (line) o += (e * std::cos(la) + n * std::sin(la)) * ((double)shift * 0.6 * (double)impl_->set.towerRadiusM);
            const double d = glm::length(o);
            T.dir = (d > 0.0) ? moveAlong(c, o / d, d) : c;
            glm::dvec3 sh = glm::dvec3(S->shear);
            sh -= T.dir * glm::dot(sh, T.dir);
            const double sl = glm::length(sh);
            T.shear = glm::vec3((sl > 1e-12) ? sh / sl : e);
            T.strength = s;
            T.role = ts.role;
            T.size = ts.size;
            T.overshoot = ts.overshoot;
            T.seed = cSeed;
            T.stormId = S->id;
            T.towerIndex = (uint8_t)i;
            T.kind = S->kind;
            T.age = cAge;
            T.growS = ts.growS;
            T.life = cLife;
            out.push_back(T);
        }
    }
    impl_->st.towersAlive = (int)out.size();
}

const Storm *Catalog::find(uint64_t id) const {
    for (const auto &kv : impl_->bins)
        for (const Storm &S : kv.second)
            if (S.id == id) return &S;
    for (const auto &kv : impl_->lineBins)
        for (const Storm &S : kv.second)
            if (S.id == id) return &S;
    return nullptr;
}

void Catalog::shieldsAt(double t, const ShieldParams &p, std::vector<Shield> &out) {
    std::vector<const Storm *> storms;
    stormsAt(t, storms);
    shieldsAt(t, storms, p, out);
}

void Catalog::shieldsAt(double t, const std::vector<const Storm *> &storms, const ShieldParams &p,
                        std::vector<Shield> &out) {
    out.clear();
    const double R = (double)impl_->set.towerRadiusM;
    for (const Storm *S : storms) {
        const double tau = t - S->t0;
        const glm::dvec3 c = S->centreAt(t);
        glm::dvec3 e, n;
        eastNorth(c, e, n);
        glm::dvec3 sh = glm::dvec3(S->shear);
        sh -= c * glm::dot(sh, c);
        sh = (glm::length(sh) > 1e-12) ? glm::normalize(sh) : e;
        Shield Sh;
        Sh.stormId = S->id;
        Sh.kind = S->kind;
        Sh.seed = (uint32_t)(mix64(S->id ^ 0x5141E1Dull) >> 32);
        if (isLineSystem(S->kind)) {
            const double sysLife = (double)S->sysLife, linger = std::max((double)S->linger, 1.0);
            const double ramp = (double)S->towers[0].sysRamp;
            const double act = smooth01(0.3 * ramp, 1.2 * ramp, tau) * (1.0 - smooth01(sysLife, sysLife + linger, tau));
            if (act <= 0.0) continue;
            const double age01 = smooth01(sysLife, sysLife + linger, tau);
            const double grow = smooth01(0.0, 3.0 * 3600.0, tau);
            const double half = (double)S->lineHalfM * (0.6 + 0.4 * smooth01(0.0, 1.5 * ramp, tau));
            const double trail = (double)S->trailM * (0.45 + 0.55 * grow) * (1.0 + 0.3 * age01);
            const double la = (double)S->lineAz;
            const glm::dvec3 L = e * std::cos(la) + n * std::sin(la);
            // Rearward = the shear (the anvil's downwind); the spine sits behind the line by 45% of the trail.
            const glm::dvec3 rear = sh;
            const glm::dvec3 mid = moveAlong(c, rear, 0.45 * trail);
            Sh.a = moveAlong(mid, L, half);
            Sh.b = moveAlong(mid, -L, half);
            Sh.rA = Sh.rB = (float)(0.55 * trail + R);
            Sh.strength = (float)act;
            Sh.thick = (float)(1.0 - 0.55 * age01);
            Sh.age01 = (float)age01;
            Sh.rain = (float)(act * (1.0 - 0.8 * age01));
            Sh.lineK = 1.0f;
            Sh.rear = glm::vec3(rear);
            out.push_back(Sh);
            continue;
        }
        // Single-cell kinds: fed by the anvil-reaching towers (role 1, strength over ~0.42), lingering after.
        constexpr double kOn = 0.42;   // (= finishAnvil's anvOn / anvOff threshold)
        glm::dvec3 F(0.0);
        double wSum = 0.0, act = 0.0, mature = 0.0, firstOn = 1e30, lastOff = -1e30;
        bool fed = false;
        for (int i = 0; i < S->towerCount; ++i) {
            const TowerSpec &ts = S->towers[i];
            if (ts.role < 0.99f || ts.peak < kOn + 0.02f || ts.period > 0.0f) continue;
            const double on = (double)ts.anvOn, off = (double)ts.anvOff;
            if (off <= on) continue;
            firstOn = std::min(firstOn, on);
            lastOff = std::max(lastOff, off);
            const double linger = std::max((double)S->linger, 1.0);
            const double a = smooth01(on, on + 900.0, tau) * (1.0 - smooth01(off, off + linger, tau));
            if (a <= 0.0) continue;
            fed |= tau < off;
            mature = std::max(mature, smooth01(on, on + 2700.0, tau));
            // Where its head stands now (the tower's axis, blown downwind), then the drift aloft after it died.
            const glm::dvec3 o = e * (double)ts.offE + n * (double)ts.offN;
            const double d = glm::length(o);
            const glm::dvec3 axis = (d > 0.0) ? moveAlong(c, o / d, d) : c;
            const double adv = (double)p.headDriftM * 0.6 + (double)S->upperWind * 0.8 * std::max(0.0, tau - off);
            const glm::dvec3 hp = moveAlong(axis, sh, adv);
            F += hp * a;
            wSum += a;
            act = std::max(act, a);
        }
        if (act <= 0.0 || wSum <= 0.0) continue;
        F = glm::normalize(F / wSum);
        const double kindL = S->kind == StormKind::Supercell ? 160000.0 : S->kind == StormKind::Multicell ? 90000.0 : 45000.0;
        const double L = std::min(kindL, (double)p.headDriftM + 0.6 * (double)S->upperWind * std::max(0.0, tau - firstOn));
        const double age01 = fed ? 0.0 : smooth01(lastOff, lastOff + std::max((double)S->linger, 1.0), tau);
        const double rA = (double)p.headRadiusM * (0.55 + 0.45 * mature) * (1.0 + 0.4 * age01);
        Sh.a = moveAlong(F, -sh, 0.3 * rA);
        Sh.b = moveAlong(F, sh, L * (1.0 + 0.3 * age01));
        Sh.rA = (float)rA;
        Sh.rB = (float)(1.15 * rA + 0.25 * L);
        Sh.strength = (float)act;
        Sh.thick = (float)(1.0 - 0.6 * age01);
        Sh.age01 = (float)age01;
        out.push_back(Sh);
    }
    impl_->st.shieldsAlive = (int)out.size();
}

Catalog::Stats Catalog::stats() const { return impl_->st; }

} // namespace wx
