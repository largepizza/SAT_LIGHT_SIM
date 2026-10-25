#include "Weather.h"
#include "StormClimatology.h"
#include "../FrameProfile.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <iterator>
#include <map>
#include <thread>
#include <atomic>
#include <condition_variable>
#include <mutex>

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
    double latR;                     // asin(centre.z): a rotation about the pole keeps it
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
    std::map<int64_t, std::vector<Cyclone>> seasons;   // key basin * 100000 + year
    void buildSeason(int basin, int year, std::vector<Cyclone> &out) const;   // Weather.cpp, tropical cyclones
    const std::vector<Cyclone> &season(int basin, int year) {
        const int64_t k = (int64_t)basin * 100000 + year;
        auto it = seasons.find(k);
        if (it == seasons.end()) {
            FPROF_SCOPE("wx.buildSeason");
            std::vector<Cyclone> v;
            buildSeason(basin, year, v);
            it = seasons.emplace(k, std::move(v)).first;
        }
        return it->second;
    }
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
    std::vector<std::vector<Storm>> taskParts;   // stormsAt's per-task lists (kept for their capacity)
    // A persistent pool of threads - 1 workers (the caller is the last). A jump needs ~17 spawn bins, ~8 line bins and
    // up to 18 cyclone seasons at once; spawning threads per call cost a few hundred microseconds and kept whole bins as
    // the unit of work (17 bins on 6 threads: 18-21 ms on the frame after a jump). Every task writes its own output, and
    // the outputs are joined in task order, so the result is the same for any thread count.
    struct Pool {
        std::vector<std::thread> th;
        std::mutex m;
        std::condition_variable cvWork, cvDone;
        const std::function<void(size_t)> *job = nullptr;
        size_t nJobs = 0;
        std::atomic<size_t> next{0};
        int busy = 0;
        uint64_t gen = 0;
        bool quit = false;
        void start(int workers) {
            stop();
            quit = false;
            for (int i = 0; i < workers; ++i)
                th.emplace_back([this] {
                    uint64_t seen = 0;
                    for (;;) {
                        const std::function<void(size_t)> *f;
                        size_t n;
                        {
                            std::unique_lock<std::mutex> lk(m);
                            cvWork.wait(lk, [&] { return quit || gen != seen; });
                            if (quit) return;
                            seen = gen;
                            f = job;
                            n = nJobs;
                        }
                        for (size_t i; (i = next.fetch_add(1)) < n;) (*f)(i);
                        std::lock_guard<std::mutex> lk(m);
                        if (--busy == 0) cvDone.notify_all();
                    }
                });
        }
        void stop() {
            {
                std::lock_guard<std::mutex> lk(m);
                quit = true;
            }
            cvWork.notify_all();
            for (auto &t : th) t.join();
            th.clear();
        }
        void run(size_t n, const std::function<void(size_t)> &f) {
            if (n == 0) return;
            if (th.empty() || n == 1) {
                for (size_t i = 0; i < n; ++i) f(i);
                return;
            }
            {
                std::lock_guard<std::mutex> lk(m);
                job = &f;
                nJobs = n;
                next = 0;
                busy = (int)th.size();
                ++gen;
            }
            cvWork.notify_all();
            for (size_t i; (i = next.fetch_add(1)) < n;) f(i);
            std::unique_lock<std::mutex> lk(m);
            cvDone.wait(lk, [&] { return busy == 0; });
        }
        ~Pool() { stop(); }
    } pool;

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
                c.latR = std::asin(std::clamp(c.centre.z, -1.0, 1.0));
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
                c.latR = std::asin(std::clamp(c.centre.z, -1.0, 1.0));
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
    // rot (optional): Env::mapToEarth at tMid as a rotation about the pole, its images of x and y (rotX, rotY), when the
    // caller has checked that it IS one (mapToEarthRotZ). Applied as ex * d.x + ey * d.y + z * d.z it is the same
    // arithmetic as the sim's own Env (bit-identical) without a call and a cosine and sine per cell. With Env::landAt
    // the land share takes the cell's latitude (cached: a rotation about the pole keeps it) and the longitude computed
    // here, instead of recomputing both from e.
    void buildCellRange(int64_t bin, size_t c0, size_t c1, double tMid, double sunLonDeg, double doy,
                        std::vector<Storm> &out, const glm::dvec3 *rot = nullptr) const {
        const double binT0 = (double)bin * kBinS;
        const double freq = std::max(0.0, (double)set.frequency);
        for (size_t ci = c0; ci < c1; ++ci) {
            const Cell &c = cells[ci];
            const glm::dvec3 e = rot ? glm::dvec3(rot[0].x * c.centre.x + rot[1].x * c.centre.y,
                                                  rot[0].y * c.centre.x + rot[1].y * c.centre.y, c.centre.z)
                                     : env.mapToEarth(c.centre, tMid);
            const double latR = rot ? c.latR : std::asin(std::clamp(e.z, -1.0, 1.0));
            const double lonR = std::atan2(e.y, e.x);
            const double latE = latR / kDeg;
            const double lonE = lonR / kDeg;
            const float land = (rot && env.landAt) ? env.landAt(latR, lonR) : env.land ? env.land(e) : 0.0f;
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
                Storm &S = out.emplace_back();   // (built in place: a Storm is ~1.3 KB)
                const uint64_t id = key3((uint64_t)ci, (uint64_t)bin, (uint64_t)k, 0x570A3ull);
                makeStorm(S, id, p, tb, std::asin(std::clamp(sLat, -1.0, 1.0)) / kDeg, landP, dayOfYear(tb));
                S.potential = (float)fd;
                S.moisture = (float)m;
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
    FPROF_SCOPE("wx.configure");
    const bool sameSet = I.configured && clim == I.clim && s.frequency == I.set.frequency &&
                         s.targetActive == I.set.targetActive && s.moistureMin == I.set.moistureMin &&
                         s.moistureFull == I.set.moistureFull && s.towerRadiusM == I.set.towerRadiusM &&
                         s.lifeScale == I.set.lifeScale && s.lineFrequency == I.set.lineFrequency &&
                         s.targetLineActive == I.set.targetLineActive && s.cycloneFrequency == I.set.cycloneFrequency &&
                         s.seaFactor == I.set.seaFactor;
    const bool same = sameSet && envKey == I.envKey;
    I.env = env;
    if (same) return;
    I.set = s;
    I.clim = clim;
    I.envKey = envKey;
    I.bins.clear();
    I.lineBins.clear();
    I.binT.clear();
    I.lineBinT.clear();
    I.seasons.clear();
    if (I.cells.empty()) I.buildCells();
    if (I.lineCells.empty()) I.buildLineCells();
    // The calibration (mean lives, the spawn rate's Earth integral) is a function of the settings, the climatology and
    // the land alone (env.land is Earth-fixed; the cells are an equal-area quadrature in any frame): a new env key with the
    // same settings (the cloud map's drift or coverage: a bookmark, a Watch's jump) keeps it. Recomputing it was ~50 ms on
    // the main thread at every such jump (perf4-cpu 2026-10-24).
    if (!sameSet)
        I.calibrate();
    I.configured = true;
}

void Catalog::setBuildThreads(int n) {
    n = std::max(1, n);
    if (n == impl_->threads && (int)impl_->pool.th.size() == n - 1) return;
    impl_->threads = n;
    impl_->pool.start(n - 1);
}
void Catalog::parallelFor(size_t n, const std::function<void(size_t)> &fn) { impl_->pool.run(n, fn); }
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
    FPROF_SCOPE("wx.stormsAt");
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

    // The line systems' own bins (3 h, looking back kLineMaxLifeS).
    const int64_t l1 = (int64_t)std::floor(t / kLineBinS);
    const int64_t l0 = (int64_t)std::floor((t - kLineMaxLifeS) / kLineBinS);
    for (auto it = I.lineBins.begin(); it != I.lineBins.end();)
        if (it->first < l0 - 2 || it->first > l1 + 2) { I.lineBinT.erase(it->first); it = I.lineBins.erase(it); }
        else ++it;

    std::vector<int64_t> missing, missingL;
    for (int64_t b = b0; b <= b1; ++b)
        if (I.bins.find(b) == I.bins.end()) missing.push_back(b);
    for (int64_t b = l0; b <= l1; ++b)
        if (I.lineBins.find(b) == I.lineBins.end()) missingL.push_back(b);
    if (!missing.empty() || !missingL.empty()) {
        FPROF_SCOPE("wx.buildBins");
        const auto c0 = std::chrono::steady_clock::now();
        // Every missing spawn bin cut into ranges of cells, and every missing line bin, as tasks on the pool. Each
        // range appends its cells' storms in cell order, so a bin's ranges joined in order are the bin built whole.
        constexpr size_t kCellsPerTask = 512;
        const size_t nCh = std::max<size_t>(1, (I.cells.size() + kCellsPerTask - 1) / kCellsPerTask);
        struct BinT { double tMid, sunLon, doy; glm::dvec3 rot[2]; bool rotOk; };
        std::vector<BinT> bt(missing.size());
        for (size_t j = 0; j < missing.size(); ++j) {
            const double tMid = ((double)missing[j] + 0.5) * kBinS;
            const glm::dvec3 sun = I.env.sunEarth ? I.env.sunEarth(tMid) : glm::dvec3(1, 0, 0);
            bt[j] = {tMid, std::atan2(sun.y, sun.x) / kDeg, dayOfYear(tMid), {}, false};
            // Map -> Earth at tMid as a rotation about the pole (the sim's drift), checked bit for bit on a few cells.
            if (I.env.mapToEarth && !I.cells.empty()) {
                BinT &b = bt[j];
                b.rot[0] = I.env.mapToEarth(glm::dvec3(1, 0, 0), tMid);
                b.rot[1] = I.env.mapToEarth(glm::dvec3(0, 1, 0), tMid);
                b.rotOk = b.rot[0].z == 0.0 && b.rot[1].z == 0.0;
                for (size_t ci : {(size_t)0, I.cells.size() / 3, I.cells.size() / 2, I.cells.size() - 1}) {
                    const glm::dvec3 &d = I.cells[ci].centre;
                    const glm::dvec3 e = I.env.mapToEarth(d, tMid);
                    const glm::dvec3 r(b.rot[0].x * d.x + b.rot[1].x * d.y, b.rot[0].y * d.x + b.rot[1].y * d.y, d.z);
                    b.rotOk &= e.x == r.x && e.y == r.y && e.z == r.z;
                }
            }
        }
        // The tasks' lists are kept between calls for their capacity: growing ~400 fresh vectors of 1.3-KB storms from
        // 12 threads at once serialised on the heap (in a jump the storm records' allocation cost more than the
        // spawning).
        std::vector<std::vector<Storm>> &parts = I.taskParts;
        if (parts.size() < missing.size() * nCh) parts.resize(missing.size() * nCh);
        for (auto &pp : parts) {
            pp.clear();
            if (pp.capacity() < 32) pp.reserve(32);
        }
        std::vector<std::vector<Storm>> builtL(missingL.size());
        const size_t nSpawn = missing.size() * nCh;
        I.pool.run(nSpawn + missingL.size(), [&](size_t k) {
            if (k < nSpawn) {
                const size_t j = k / nCh, c = k % nCh;
                const size_t ca = std::min(I.cells.size(), c * kCellsPerTask), cb = std::min(I.cells.size(), ca + kCellsPerTask);
                I.buildCellRange(missing[j], ca, cb, bt[j].tMid, bt[j].sunLon, bt[j].doy, parts[k],
                                 bt[j].rotOk ? bt[j].rot : nullptr);
            } else {
                I.buildLineBin(missingL[k - nSpawn], builtL[k - nSpawn]);
            }
        });
        for (size_t j = 0; j < missing.size(); ++j) {
            std::vector<Storm> v;
            size_t n = 0;
            for (size_t c = 0; c < nCh; ++c) n += parts[j * nCh + c].size();
            v.reserve(n);
            for (size_t c = 0; c < nCh; ++c) {
                std::vector<Storm> &pp = parts[j * nCh + c];
                v.insert(v.end(), pp.begin(), pp.end());
                pp.clear();
            }
            I.bins.emplace(missing[j], std::move(v));
        }
        for (size_t j = 0; j < missingL.size(); ++j) I.lineBins.emplace(missingL[j], std::move(builtL[j]));
        I.st.binsBuiltLastCall = (int)(missing.size() + missingL.size());
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
    {
        int nLine = 0;
        for (int64_t b = l0; b <= l1; ++b) {
            auto it = I.lineBins.find(b);
            if (it == I.lineBins.end()) continue;
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
    FPROF_SCOPE("wx.towersAt");
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
    FPROF_SCOPE("wx.shieldsAt");
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
            Sh.bowM = S->bow;
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

// ── Tropical cyclones (2026-10-23) ─────────────────────────────────────────────────────────────────────────────
// Seasons per basin and year, a pure function of (basin, year, settings): genesis dates from the basin's seasonal
// curve, genesis points in its box, then each track integrated hourly in the EARTH-fixed frame (steering: westward and
// a little poleward in the trades, recurving poleward and east past a latitude of its own; intensity relaxing toward a
// potential that falls over cold water and collapses over land), then named in order of reaching tropical-storm
// strength from the basin's WMO list. Counts and seasons: climatology (~85 a year worldwide).

namespace {

struct BasinDef {
    const char *name;
    double latC, latW, lonC, lonW;  // genesis box (deg)
    double peak, sigma, count;      // the season's peak (day of year), spread (days), mean count
    double peak2, frac2;            // a second peak (the North Indian Ocean's two seasons)
    double coldLat;                 // where its potential falls away (deg poleward)
    double recurveLo, recurveHi;    // recurvature latitude range
};
const BasinDef kBasins[Catalog::kCycloneBasins] = {
    {"North Atlantic", 15.0, 7.0, -52.0, 26.0, 250.0, 33.0, 14.0, 0.0, 0.0, 30.0, 24.0, 33.0},
    {"Eastern Pacific", 13.0, 4.0, -106.0, 12.0, 222.0, 38.0, 16.0, 0.0, 0.0, 22.0, 30.0, 40.0},
    {"Western Pacific", 14.0, 6.0, 140.0, 18.0, 240.0, 55.0, 25.0, 0.0, 0.0, 30.0, 22.0, 32.0},
    {"North Indian Ocean", 13.0, 4.0, 87.0, 9.0, 140.0, 22.0, 5.0, 315.0, 0.6, 24.0, 18.0, 26.0},
    {"South-West Indian Ocean", -13.0, 5.0, 68.0, 20.0, 45.0, 42.0, 10.0, 0.0, 0.0, 28.0, 20.0, 28.0},
    {"Australian region / South Pacific", -14.0, 4.0, 152.0, 30.0, 50.0, 45.0, 11.0, 0.0, 0.0, 28.0, 20.0, 30.0},
};

// Climatological ROUTES (2026-10-23, the user: "Atlantic hurricanes move westwards and up, Pacific hurricanes track up
// through Baja California"): per basin a few polylines (lat, lon deg) after the best-track climatology's main track
// families, with a weight. A storm picks one, weighted by how near its genesis lies to it, and is steered along it by
// pure pursuit (aiming 500 km ahead on the route, keeping a share of its starting lateral offset), so storms of one
// family run roughly parallel and the families differ: Cape Verde recurvers, Caribbean straight runners, Gulf landfalls,
// East Pacific storms out to sea, up the Baja coast or recurving into Sinaloa, typhoons into the Philippines / China or
// recurving to Japan, Bay of Bengal storms north, Southern Hemisphere storms west then poleward.
struct RoutePt { double lat, lon; };
struct RouteDef { double w; int n; RoutePt p[8]; };
const RouteDef kRoutesNA[] = {
    {0.30, 8, {{12, -28}, {14, -40}, {17, -52}, {22, -60}, {28, -64}, {34, -62}, {40, -55}, {46, -42}}},   // Cape Verde, recurving
    {0.17, 7, {{13, -45}, {14.5, -58}, {15.5, -70}, {16.5, -80}, {18, -87}, {19.5, -94}, {21, -99}}},       // Caribbean -> Mexico
    {0.20, 8, {{14, -52}, {17, -64}, {20.5, -75}, {23.5, -83}, {26.5, -88}, {30, -90}, {34, -87}, {38, -82}}},  // Gulf coast
    {0.18, 8, {{16, -48}, {20, -60}, {24, -70}, {27.5, -77}, {31, -80}, {35, -77}, {39, -72}, {44, -65}}},  // Florida / East Coast
    {0.15, 6, {{15, -35}, {19, -44}, {24, -50}, {30, -52}, {36, -48}, {42, -38}}},                          // early recurve
};
const RouteDef kRoutesEP[] = {
    {0.50, 6, {{12, -97}, {14, -105}, {16, -114}, {17.5, -124}, {18.5, -134}, {19, -145}}},                 // out to sea
    {0.18, 7, {{12, -100}, {15, -105}, {18, -109}, {21, -111.5}, {24, -113}, {27, -114}, {30, -115}}},      // up the Baja coast
    {0.12, 6, {{12, -102}, {15, -106}, {18, -108}, {21, -107.5}, {23.5, -106}, {26, -104}}},                // recurving into Sinaloa
    {0.20, 6, {{12, -112}, {13.5, -125}, {15, -138}, {17, -150}, {18.5, -158}, {19.5, -168}}},              // toward Hawaii
};
const RouteDef kRoutesWP[] = {
    {0.33, 7, {{10, 155}, {12, 145}, {14, 135}, {15.5, 126}, {17, 118}, {19.5, 111}, {21.5, 106}}},         // Philippines -> S China
    {0.20, 7, {{12, 150}, {15, 140}, {19, 130}, {22.5, 123}, {26, 120}, {30, 119}, {34, 121}}},             // Taiwan -> E China
    {0.32, 8, {{11, 152}, {15, 142}, {20, 134}, {25, 129}, {30, 129.5}, {35, 134}, {40, 141}, {45, 150}}},  // recurving to Japan
    {0.15, 6, {{13, 160}, {18, 152}, {24, 147}, {30, 147}, {36, 152}, {42, 161}}},                          // early recurve
};
const RouteDef kRoutesNI[] = {
    {0.35, 5, {{10, 90}, {13, 88}, {16, 86}, {19, 85.3}, {21.5, 85}}},                                      // Odisha / Andhra
    {0.30, 5, {{11, 88}, {14, 88.5}, {18, 89.5}, {21, 90.5}, {23, 91.5}}},                                  // Bangladesh / Myanmar
    {0.35, 4, {{9, 90}, {10.5, 86}, {12, 82}, {12.5, 79}}},                                                  // Tamil Nadu
};
const RouteDef kRoutesSI[] = {
    {0.25, 7, {{-11, 72}, {-13, 62}, {-15, 54}, {-17, 49}, {-19, 44}, {-22, 40}, {-26, 38}}},                // Madagascar, Mozambique
    {0.40, 6, {{-11, 78}, {-13, 68}, {-16, 60}, {-20, 56}, {-25, 55}, {-31, 59}}},                           // Mascarenes, recurving
    {0.35, 5, {{-12, 88}, {-15, 82}, {-19, 77}, {-24, 75}, {-30, 77}}},                                      // mid-ocean
};
const RouteDef kRoutesAU[] = {
    {0.25, 7, {{-11, 128}, {-13, 122}, {-16, 117}, {-19, 114}, {-22, 113.5}, {-26, 115}, {-30, 119}}},       // Pilbara
    {0.20, 6, {{-12, 160}, {-14, 156}, {-16, 152}, {-17.5, 148}, {-19, 144}, {-21, 141}}},                   // onto Queensland
    {0.25, 6, {{-13, 155}, {-16, 156}, {-20, 157}, {-25, 159}, {-30, 162}, {-35, 166}}},                     // Coral Sea, south
    {0.30, 6, {{-12, 172}, {-15, 174}, {-18, 176}, {-22, 179}, {-26, 182}, {-31, 186}}},                     // Fiji / Tonga, SE
};
struct RouteSet { const RouteDef *r; int n; };
const RouteSet kRoutes[Catalog::kCycloneBasins] = {
    {kRoutesNA, (int)std::size(kRoutesNA)}, {kRoutesEP, (int)std::size(kRoutesEP)}, {kRoutesWP, (int)std::size(kRoutesWP)},
    {kRoutesNI, (int)std::size(kRoutesNI)}, {kRoutesSI, (int)std::size(kRoutesSI)}, {kRoutesAU, (int)std::size(kRoutesAU)},
};

// A route in the tangent plane at (lat0, lon0): km east / north (good to a few % over the routes' spans).
struct RoutePlane {
    double x[8], y[8], s[8];   // vertices and arc length (km)
    int n = 0;
};
RoutePlane routeInPlane(const RouteDef &R, double lat0, double lon0) {
    RoutePlane P;
    P.n = R.n;
    const double c = std::cos(lat0 * kDeg);
    for (int i = 0; i < R.n; ++i) {
        double dl = R.p[i].lon - lon0;
        dl -= 360.0 * std::floor((dl + 180.0) / 360.0);
        P.x[i] = dl * c * 111.2;
        P.y[i] = (R.p[i].lat - lat0) * 111.2;
        P.s[i] = i == 0 ? 0.0 : P.s[i - 1] + std::hypot(P.x[i] - P.x[i - 1], P.y[i] - P.y[i - 1]);
    }
    return P;
}
// The nearest point of the polyline to the origin: its arc length, signed distance (left of the route > 0) and tangent.
void routeNearest(const RoutePlane &P, double &sOut, double &offOut, double &tx, double &ty) {
    double best = 1e30;
    sOut = 0.0; offOut = 0.0; tx = 1.0; ty = 0.0;
    for (int i = 0; i + 1 < P.n; ++i) {
        const double ax = P.x[i], ay = P.y[i], bx = P.x[i + 1] - ax, by = P.y[i + 1] - ay;
        const double L2 = std::max(bx * bx + by * by, 1e-9);
        const double u = std::clamp(-(ax * bx + ay * by) / L2, 0.0, 1.0);
        const double px = ax + u * bx, py = ay + u * by, d2 = px * px + py * py;
        if (d2 < best) {
            best = d2;
            const double L = std::sqrt(L2);
            tx = bx / L; ty = by / L;
            sOut = P.s[i] + u * L;
            offOut = tx * (-py) - ty * (-px);   // the origin's side: tangent x (origin - p), left > 0
        }
    }
}
// The point at arc length s (extrapolated along the last segment past the end).
void routeAt(const RoutePlane &P, double s, double &x, double &y, double &tx, double &ty) {
    int i = 0;
    while (i + 2 < P.n && s > P.s[i + 1]) ++i;
    const double bx = P.x[i + 1] - P.x[i], by = P.y[i + 1] - P.y[i];
    const double L = std::max(std::hypot(bx, by), 1e-9);
    tx = bx / L; ty = by / L;
    const double u = s - P.s[i];
    x = P.x[i] + tx * u;
    y = P.y[i] + ty * u;
}

// WMO name lists (public; the 2036 Atlantic and Eastern Pacific lists are 2024's six-year rotation with the 2025
// retirements replaced; the others run on sequentially, a slice here).
const char *const kNamesNA[] = {"Alberto", "Brianna", "Chris", "Debby", "Ernesto", "Francine", "Gordon", "Holly", "Isaac",
                                "Joyce", "Kirk", "Leslie", "Miguel", "Nadine", "Oscar", "Patty", "Rafael", "Sara", "Tony",
                                "Valerie", "William"};
const char *const kNamesEP[] = {"Aletta", "Bud", "Carlotta", "Daniel", "Emilia", "Fabio", "Gilma", "Hector", "Ileana",
                                "John", "Kristy", "Lane", "Miriam", "Norman", "Olivia", "Paul", "Rosa", "Sergio", "Tara",
                                "Vicente", "Willa", "Xavier", "Yolanda", "Zeke"};
const char *const kNamesWP[] = {"Damrey", "Haikui", "Kirogi", "Yun-yeung", "Koinu", "Bolaven", "Sanba", "Jelawat",
                                "Ewiniar", "Maliksi", "Gaemi", "Prapiroon", "Maria", "Son-Tinh", "Ampil", "Wukong",
                                "Jongdari", "Shanshan", "Yagi", "Leepi", "Bebinca", "Pulasan", "Soulik", "Cimaron", "Jebi",
                                "Krathon", "Barijat", "Trami", "Kong-rey", "Yinxing", "Toraji", "Man-yi", "Usagi", "Pabuk",
                                "Wutip", "Sepat", "Mun", "Danas", "Nari", "Wipha", "Francisco", "Co-may", "Krosa", "Bailu",
                                "Podul", "Lingling", "Kajiki", "Nongfa", "Peipah", "Tapah"};
const char *const kNamesNI[] = {"Shakhti", "Montha", "Senyar", "Ditwah", "Arnab", "Murasu", "Akvan", "Kaani", "Ngamann",
                                "Sail", "Sahab", "Lulu", "Ghazeer", "Afoor", "Kyarthit", "Rohit"};
const char *const kNamesSI[] = {"Ancha", "Bheki", "Chido", "Dikeledi", "Elvis", "Faida", "Garance", "Honde", "Ivone",
                                "Jude", "Kanto", "Lira", "Maipelo", "Njazi", "Oscar", "Pamela", "Quentin", "Rajab",
                                "Savana", "Themba", "Uyapo", "Viviane", "Walter", "Xangy", "Yemurai", "Zanele"};
const char *const kNamesAU[] = {"Anika", "Billy", "Charlotte", "Dominic", "Ellie", "Freddy", "Gabrielle", "Herman",
                                "Ilsa", "Jasper", "Kirrily", "Lincoln", "Megan", "Neville", "Olga", "Paul", "Robyn",
                                "Sean", "Tasha", "Vince", "Zelia", "Alfred", "Bianca", "Courtney", "Dianne", "Errol"};

double yearStartJ2000(int year) {   // consistent with dayOfYear above (years of 365.2422 days from J2000 noon)
    return ((double)(year - 2000) * 365.2422 - 0.5) * kDay;
}

double gaussOf(Rng &r) {
    const double u1 = std::max(r.u(), 1e-12), u2 = r.u();
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}

int cycloneCategory(float v) {
    return v < 17.5f ? -1 : v < 33.0f ? 0 : v < 43.0f ? 1 : v < 50.0f ? 2 : v < 58.0f ? 3 : v < 70.0f ? 4 : 5;
}

} // namespace

const char *cycloneBasinName(int basin) {
    return (basin >= 0 && basin < Catalog::kCycloneBasins) ? kBasins[basin].name : "?";
}

const char *cycloneCategoryName(int cat, int basin) {
    if (cat < 0) return "Tropical depression";
    if (cat == 0) return basin == 3 ? "Cyclonic storm" : "Tropical storm";
    static const char *hur[] = {"", "Category 1 hurricane", "Category 2 hurricane", "Category 3 major hurricane",
                                "Category 4 major hurricane", "Category 5 major hurricane"};
    static const char *typ[] = {"", "Category 1 typhoon", "Category 2 typhoon", "Category 3 typhoon",
                                "Category 4 super typhoon", "Category 5 super typhoon"};
    static const char *cyc[] = {"", "Category 1 cyclone", "Category 2 cyclone", "Category 3 severe cyclone",
                                "Category 4 severe cyclone", "Category 5 severe cyclone"};
    cat = std::min(cat, 5);
    return (basin <= 1) ? hur[cat] : (basin == 2) ? typ[cat] : cyc[cat];
}

glm::dvec3 Cyclone::earthAt(double t) const {
    if (track.empty()) return glm::dvec3(0, 0, 1);
    const double x = std::clamp((t - t0) / kStepS, 0.0, (double)(track.size() - 1));
    const size_t i = std::min((size_t)x, track.size() - 1), j = std::min(i + 1, track.size() - 1);
    const double f = x - (double)i;
    return glm::normalize(track[i] * (1.0 - f) + track[j] * f);
}

float Cyclone::vmaxAt(double t) const {
    if (vmax.empty()) return 0.0f;
    const double x = std::clamp((t - t0) / kStepS, 0.0, (double)(vmax.size() - 1));
    const size_t i = std::min((size_t)x, vmax.size() - 1), j = std::min(i + 1, vmax.size() - 1);
    const double f = x - (double)i;
    return (float)(vmax[i] * (1.0 - f) + vmax[j] * f);
}

double Cyclone::turnAt(double t) const {
    if (turn.empty()) return 0.0;
    const double x = std::clamp((t - t0) / kStepS, 0.0, (double)(turn.size() - 1));
    const size_t i = std::min((size_t)x, turn.size() - 1), j = std::min(i + 1, turn.size() - 1);
    const double f = x - (double)i;
    return turn[i] * (1.0 - f) + turn[j] * f;
}

namespace {
// The radius of maximum wind: it contracts as the storm intensifies.
double cycloneRmw(double rmw0, double v) { return rmw0 * (1.0 - 0.5 * smooth01(30.0, 70.0, v)); }
} // namespace

void Catalog::Impl::buildSeason(int basin, int year, std::vector<Cyclone> &out) const {
    out.clear();
    const BasinDef &B = kBasins[basin];
    Rng r(key3((uint64_t)basin, (uint64_t)(year + 10000), 0xC1C10ull, 0x7C5EA5ull));
    const double lambda = B.count * std::max(0.0, (double)set.cycloneFrequency);
    const int n = poisson(r.u(), lambda);
    const double y0 = yearStartJ2000(year);
    for (int k = 0; k < n; ++k) {
        Cyclone C;
        Rng q(key3((uint64_t)basin, (uint64_t)(year + 10000), (uint64_t)k, 0xC1C1DEull));
        C.id = key3((uint64_t)basin, (uint64_t)(year + 10000), (uint64_t)k, 0x7C1Dull) | 3ull;
        C.basin = basin;
        C.year = year;
        C.seed = (uint32_t)(q.nextU64() >> 32);
        const bool second = B.frac2 > 0.0 && q.u() < B.frac2;
        double doy = (second ? B.peak2 : B.peak) + (second ? 25.0 : B.sigma) * gaussOf(q);
        doy = std::fmod(doy + 3.0 * 365.2422, 365.2422);
        C.t0 = y0 + doy * kDay;
        double lat = B.latC + B.latW * (2.0 * q.u() - 1.0);
        double lon = B.lonC + B.lonW * (2.0 * q.u() - 1.0);
        // Not over land (the boxes take in Cape York, the Top End, Madagascar's coast...): redrawn from a stream of its
        // own, so the draws below (and every other storm's) are unchanged.
        if (env.land) {
            Rng g(key3((uint64_t)basin, (uint64_t)(year + 10000), (uint64_t)k, 0x6E1A2Dull));
            for (int tries = 0; tries < 12; ++tries) {
                const glm::dvec3 pg(std::cos(lat * kDeg) * std::cos(lon * kDeg), std::cos(lat * kDeg) * std::sin(lon * kDeg),
                                    std::sin(lat * kDeg));
                if (env.land(pg) <= 0.5f) break;
                lat = B.latC + B.latW * (2.0 * g.u() - 1.0);
                lon = B.lonC + B.lonW * (2.0 * g.u() - 1.0);
            }
        }
        C.hemi = lat >= 0.0 ? 1.0f : -1.0f;
        C.rmw0 = (float)(1000.0 * q.range(35.0, 70.0));
        C.size = (float)q.u();
        // Its own potential: most storms stay weak, a few become intense (a skewed draw, 22-82 m/s: median ~35, about a
        // quarter major hurricanes, as the North Atlantic's named storms).
        const double vPot = 22.0 + 60.0 * std::pow(q.u(), 2.2);
        const double recurve = q.range(B.recurveLo, B.recurveHi);
        const double sp0 = q.range(4.5, 7.0), sp1 = q.range(8.0, 14.0);
        const double wobPh = q.range(0.0, 2.0 * kPi), wobP = q.range(1.2, 2.4) * kDay;
        // (Drawn after the draws above, so dates, genesis points and intensities are as before the routes.)
        // Its route: weighted by the family's share x how near its genesis lies to the route.
        const RouteSet &RS = kRoutes[basin];
        int route = 0;
        double off0 = 0.0;
        {
            double wSum = 0.0, wr[8] = {}, offs[8] = {};
            for (int i = 0; i < RS.n; ++i) {
                const RoutePlane P = routeInPlane(RS.r[i], lat, lon);
                double s, off, tx, ty;
                routeNearest(P, s, off, tx, ty);
                offs[i] = off;
                wr[i] = RS.r[i].w * std::exp(-(off * off) / (1200.0 * 1200.0)) + 1e-9;
                wSum += wr[i];
            }
            double u = q.u() * wSum;
            for (int i = 0; i < RS.n; ++i) {
                route = i;
                if ((u -= wr[i]) <= 0.0) break;
            }
            off0 = offs[route];
        }
        const double offKeep = q.range(0.35, 0.75);                   // the share of its lateral offset it keeps
        const double hA1 = q.range(4.0, 9.0) * kDeg, hP1 = q.range(1.5, 3.5) * kDay, hF1 = q.range(0.0, 2.0 * kPi);
        const double hA2 = q.range(3.0, 8.0) * kDeg, hP2 = q.range(3.0, 6.5) * kDay, hF2 = q.range(0.0, 2.0 * kPi);
        const bool   stall = q.u() < 0.1;                             // a stall (Harvey, Dorian): a day or two at ~1 m/s
        const double stallT = q.range(1.5, 6.0) * kDay, stallD = q.range(0.8, 2.0) * kDay;
        const double spPh = q.range(0.0, 2.0 * kPi), spP = q.range(2.0, 5.0) * kDay;
        glm::dvec3 p(std::cos(lat * kDeg) * std::cos(lon * kDeg), std::cos(lat * kDeg) * std::sin(lon * kDeg),
                     std::sin(lat * kDeg));
        double v = 12.0;
        const int maxSteps = (int)(18.0 * kDay / Cyclone::kStepS);
        bool peaked = false;
        int landH = 0;
        for (int s = 0; s < maxSteps; ++s) {
            const double phi = std::asin(std::clamp(p.z, -1.0, 1.0)) / kDeg;
            const double ap = std::fabs(phi);
            const float land = env.land ? env.land(p) : 0.0f;
            C.track.push_back(p);
            C.vmax.push_back((float)v);
            C.overLand.push_back(land > 0.5f ? 1 : 0);
            if (v > C.vPeak) { C.vPeak = (float)v; C.tPeak = C.t0 + s * Cyclone::kStepS; }
            peaked |= v > 0.9 * vPot || s > 72;
            landH = land > 0.5f ? landH + 1 : 0;
            if ((peaked && v < 11.0) || ap > 50.0 || landH > 48) break;   // (a remnant low two days inland is gone)
            // Intensity: toward the potential over warm water (slower up than down), collapsing over land.
            double sst = 1.0 - smooth01(B.coldLat - 4.0, B.coldLat + 8.0, ap);
            // The Eastern Pacific's water cools westward too (the storms that run out to sea die before Hawaii).
            if (basin == 1) sst *= 1.0 - 0.8 * smooth01(122.0, 142.0, -std::atan2(p.y, p.x) / kDeg);
            const double target = land > 0.5f ? 8.0 : vPot * sst;
            const double tau = (target > v ? 30.0 : (land > 0.5f ? 9.0 : 20.0)) * 3600.0;
            v += (target - v) * (Cyclone::kStepS / tau);
            // Steering: along its route (pure pursuit 500 km ahead, keeping offKeep of its starting lateral offset),
            // turning slowly off it and back (two slow heading waves) with a small trochoidal wobble; slow in the trades,
            // faster once recurving into the westerlies; an occasional stall.
            const double tS = s * Cyclone::kStepS;
            const double lam = std::atan2(p.y, p.x) / kDeg;
            const RoutePlane P = routeInPlane(RS.r[route], phi, lam);
            double sN, offN, tx, ty, ax, ay, atx, aty;
            routeNearest(P, sN, offN, tx, ty);
            routeAt(P, sN + 500.0, ax, ay, atx, aty);
            const double offT = off0 * offKeep;
            ax += -aty * offT;
            ay += atx * offT;
            double az = std::atan2(ay, ax);   // from local east toward local north
            az += hA1 * std::sin(2.0 * kPi * tS / hP1 + hF1) + hA2 * std::sin(2.0 * kPi * tS / hP2 + hF2);
            az += 4.0 * kDeg * std::sin(2.0 * kPi * tS / wobP + wobPh);
            glm::dvec3 e, nn;
            eastNorth(p, e, nn);
            const glm::dvec3 dir = e * std::cos(az) + nn * std::sin(az);
            const double sRec = smooth01(recurve - 6.0, recurve + 8.0, ap);
            double speed = (sp0 * (1.0 - sRec) + sp1 * sRec) * (1.0 + 0.18 * std::sin(2.0 * kPi * tS / spP + spPh));
            if (stall) speed *= 1.0 - 0.8 * smooth01(stallT, stallT + 0.3 * kDay, tS) * (1.0 - smooth01(stallT + stallD, stallT + stallD + 0.3 * kDay, tS));
            p = moveAlong(p, dir, speed * Cyclone::kStepS);
        }
        C.life = (float)((C.track.size() - 1) * Cyclone::kStepS);
        // The circulation's turn: the trapezoid integral of vmax / rmw, hour by hour (an angle that only grows, at the
        // rate the storm turns now: a rate x age jumped back and forth as the storm strengthened and weakened).
        C.turn.resize(C.vmax.size());
        for (size_t i = 0; i < C.vmax.size(); ++i) {
            const double w = (double)C.vmax[i] / cycloneRmw((double)C.rmw0, (double)C.vmax[i]);
            const double wp = i ? (double)C.vmax[i - 1] / cycloneRmw((double)C.rmw0, (double)C.vmax[i - 1]) : w;
            C.turn[i] = i ? C.turn[i - 1] + 0.5 * (w + wp) * Cyclone::kStepS : 0.0;
        }
        out.push_back(std::move(C));
    }
    std::sort(out.begin(), out.end(), [](const Cyclone &a, const Cyclone &b) { return a.t0 < b.t0; });
    // Names in order of genesis among those that reached tropical-storm strength.
    const char *const *names = nullptr;
    size_t nn = 0;
    switch (basin) {
    case 0: names = kNamesNA; nn = std::size(kNamesNA); break;
    case 1: names = kNamesEP; nn = std::size(kNamesEP); break;
    case 2: names = kNamesWP; nn = std::size(kNamesWP); break;
    case 3: names = kNamesNI; nn = std::size(kNamesNI); break;
    case 4: names = kNamesSI; nn = std::size(kNamesSI); break;
    default: names = kNamesAU; nn = std::size(kNamesAU); break;
    }
    // Sequential lists continue from year to year (an approximate offset: ~the basin's mean count a year).
    const size_t base = (basin >= 2) ? (size_t)((int64_t)(year - 2000) * (int64_t)std::lround(B.count) % (int64_t)nn) : 0;
    int named = 0;
    for (Cyclone &C : out) {
        C.seq = -1;
        if (C.vPeak < 17.5f) continue;
        C.seq = named;
        C.name = names[(base + (size_t)named) % nn];
        ++named;
    }
}

void Catalog::cycloneSeason(int basin, int year, std::vector<const Cyclone *> &out) {
    out.clear();
    if (!impl_->configured || basin < 0 || basin >= kCycloneBasins) return;
    for (const Cyclone &C : impl_->season(basin, year)) out.push_back(&C);
}

const Cyclone *Catalog::findCyclone(uint64_t id) const {
    for (const auto &kv : impl_->seasons)
        for (const Cyclone &C : kv.second)
            if (C.id == id) return &C;
    return nullptr;
}

void Catalog::cyclonesAt(double t, std::vector<CycloneState> &out) {
    FPROF_SCOPE("wx.cyclonesAt");
    out.clear();
    Impl &I = *impl_;
    if (!I.configured) return;
    // The seasons that can hold a storm alive at t: this year's and the last (genesis up to 18 days before).
    const double yf = t / (365.2422 * kDay) + 2000.0;
    const int y = (int)std::floor(yf);
    for (auto it = I.seasons.begin(); it != I.seasons.end();) {
        const int yy = (int)(it->first % 100000);
        it = (yy < y - 2 || yy > y + 2) ? I.seasons.erase(it) : std::next(it);
    }
    {   // The seasons not cached yet, built on the pool (a new year after a jump: up to 18 at once).
        std::vector<int64_t> miss;
        for (int b = 0; b < kCycloneBasins; ++b)
            for (int yy = y - 1; yy <= y + 1; ++yy) {
                const int64_t k = (int64_t)b * 100000 + yy;
                if (I.seasons.find(k) == I.seasons.end()) miss.push_back(k);
            }
        if (!miss.empty()) {
            FPROF_SCOPE("wx.buildSeasons");
            std::vector<std::vector<Cyclone>> built(miss.size());
            I.pool.run(miss.size(), [&](size_t j) {
                I.buildSeason((int)(miss[j] / 100000), (int)(miss[j] % 100000), built[j]);
            });
            for (size_t j = 0; j < miss.size(); ++j) I.seasons.emplace(miss[j], std::move(built[j]));
        }
    }
    // Map frame from Earth-fixed: the inverse of Env::mapToEarth (a rotation about the pole), from its basis images.
    const glm::dvec3 ex = I.env.mapToEarth ? I.env.mapToEarth(glm::dvec3(1, 0, 0), t) : glm::dvec3(1, 0, 0);
    const glm::dvec3 ey = I.env.mapToEarth ? I.env.mapToEarth(glm::dvec3(0, 1, 0), t) : glm::dvec3(0, 1, 0);
    for (int b = 0; b < kCycloneBasins; ++b)
        for (int yy = y - 1; yy <= y + 1; ++yy)
            for (const Cyclone &C : I.season(b, yy)) {
                if (t < C.t0 || t > C.t0 + (double)C.life) continue;
                CycloneState S;
                S.c = &C;
                S.earth = C.earthAt(t);
                S.map = glm::dvec3(glm::dot(S.earth, ex), glm::dot(S.earth, ey), S.earth.z);
                const glm::dvec3 e2 = C.earthAt(t + 1800.0);
                S.motionEarth = (e2 - S.earth) * (kREarthM / 1800.0);
                S.vmax = C.vmaxAt(t);
                const double vkt = std::max(0.0, (double)S.vmax * 1.944);
                S.pressureHpa = (float)(1010.0 - std::pow(vkt / 6.7, 1.0 / 0.644));
                S.category = cycloneCategory(S.vmax);
                S.intensity01 = (float)smooth01(17.5, 75.0, S.vmax);
                S.rmw = (float)cycloneRmw((double)C.rmw0, (double)S.vmax);
                S.turn = C.turnAt(t);
                S.arms = (float)std::clamp(2.0 + 3.0 * smooth01(28.0, 65.0, (double)S.vmax)
                                           + 0.7 * (u53(mix64((uint64_t)C.seed ^ 0xA2u)) - 0.5), 2.0, 5.4);
                S.tanPitch = (float)std::tan((30.0 - 8.0 * (double)S.intensity01) * kDeg);
                S.bandRot = std::fmod(0.04 * S.turn, 2.0 * kPi);
                S.eyeR = S.vmax > 33.0f ? 0.45f * S.rmw * (float)smooth01(33.0, 45.0, S.vmax) : 0.0f;
                S.cdoR = (float)(1000.0 * (120.0 + 180.0 * C.size) * smooth01(14.0, 40.0, S.vmax));
                S.outerR = (float)(1000.0 * (350.0 + 450.0 * C.size) * (0.4 + 0.6 * smooth01(12.0, 30.0, S.vmax)));
                const size_t i = std::min((size_t)std::max(0.0, (t - C.t0) / Cyclone::kStepS), C.overLand.size() - 1);
                S.overLand = !C.overLand.empty() && C.overLand[i] != 0;
                out.push_back(S);
            }
    std::sort(out.begin(), out.end(), [](const CycloneState &a, const CycloneState &b) { return a.vmax > b.vmax; });
}

void Catalog::cycloneTowersAt(double t, const std::vector<CycloneState> &cs, std::vector<Tower> &out) const {
    for (const CycloneState &S : cs) {
        if (S.vmax < 20.0f) continue;
        const Cyclone &C = *S.c;
        glm::dvec3 e, n;
        eastNorth(S.map, e, n);
        const double tau = t - C.t0;
        // The eyewall: a ring of regenerating towers at the radius of maximum wind, turning cyclonically.
        const int nEye = 18;   // (12 drew the eyewall's rain as a ring of separate cores on the radar)
        const double rot = (double)C.hemi * 0.25 * std::fmod(S.turn, 8.0 * 2.0 * kPi);   // (a quarter of the air's turn)
        for (int k = 0; k < nEye + 6; ++k) {
            const uint64_t h = mix64((uint64_t)C.seed * 0x9E37ull + (uint64_t)k);
            const bool wall = k < nEye;
            double ang, rad;
            if (wall) {
                ang = 2.0 * kPi * k / nEye + 0.25 * (u53(h) - 0.5) + rot;
                rad = (double)S.rmw * (0.85 + 0.3 * u53(mix64(h ^ 1)));
            } else {
                // Inner rainbands: on the crests of the bake's spiral bands (CycloneState::arms / tanPitch / bandRot),
                // trailing the rotation, 2 to 5 radii of maximum wind out, spread over the arms. (They spiralled the
                // other way, leading, until 2026-10-23.)
                const int j = k - nEye;
                const double nA = std::floor((double)S.arms + 0.15);   // the bake's count (it crossfades at .85)
                const int nArm = std::max(2, std::min(3, (int)nA));
                const double rr = (2.0 + 3.0 * (double)(j / nArm) / 2.0 + 0.6 * u53(h)) * (double)S.rmw;
                const double m = (double)(j % nArm) * nA / (double)nArm;
                ang = (double)C.hemi * (2.0 * kPi * std::round(m) / nA
                                        - std::log(rr / (double)S.rmw) / (double)S.tanPitch - S.bandRot);
                rad = rr;
            }
            const glm::dvec3 dirT = e * std::cos(ang) + n * std::sin(ang);
            Tower T;
            T.dir = moveAlong(S.map, dirT, rad);
            const double per = (40.0 + 25.0 * u53(mix64(h ^ 2))) * 60.0;
            const double cyc = 0.5 + 0.5 * std::sin(2.0 * kPi * (tau / per + u53(mix64(h ^ 3))));
            const double peak = wall ? (0.55 + 0.45 * (double)S.intensity01) : (0.35 + 0.35 * (double)S.intensity01);
            T.strength = (float)(peak * (0.7 + 0.3 * cyc) * smooth01(20.0, 30.0, S.vmax));
            T.role = wall ? 1.0f : 0.7f;
            T.size = wall ? (float)(0.15 + 0.25 * u53(mix64(h ^ 4))) : (float)(0.3 + 0.4 * u53(mix64(h ^ 4)));
            T.overshoot = (float)u53(mix64(h ^ 5));
            // The outflow aloft is outward: the heads blow away from the centre.
            glm::dvec3 sh = T.dir - S.map;
            sh -= T.dir * glm::dot(sh, T.dir);
            T.shear = glm::vec3(glm::length(sh) > 1e-12 ? glm::normalize(sh) : e);
            T.seed = (uint32_t)(mix64(h ^ 6) >> 32);
            T.stormId = C.id;
            T.towerIndex = (uint8_t)k;
            T.kind = StormKind::Tropical;
            T.age = 7200.0f;
            T.growS = 1200.0f;
            T.life = 1e6f;
            out.push_back(T);
        }
    }
}

void Catalog::cycloneShieldsAt(double t, const std::vector<CycloneState> &cs, std::vector<Shield> &out, float armCirrus) const {
    (void)t;
    for (const CycloneState &S : cs) {
        if (S.cdoR <= 0.0f) continue;
        Shield Sh;
        Sh.a = Sh.b = S.map;
        Sh.rA = Sh.rB = S.cdoR;
        Sh.strength = (float)smooth01(15.0, 35.0, S.vmax);
        Sh.thick = 1.0f;
        Sh.age01 = 0.0f;
        Sh.rain = 0.0f;
        Sh.lineK = 0.0f;
        Sh.eyeR = S.eyeR;
        Sh.seed = S.c->seed;
        Sh.stormId = S.c->id;
        Sh.kind = StormKind::Tropical;
        if (Sh.strength > 0.0f) out.push_back(Sh);
        // The OUTFLOW cirrus (2026-10-23): a thin, frayed ice sheet spreading past the CDO toward the outer bands — the
        // shield shader's aged (orphan-anvil) look: thinner, half the density, frayed at both noise scales. Weak, so the
        // arms show through it; the CDO wins where they overlap (cv2ShieldAt takes the strongest).
        Shield O = Sh;
        O.rA = O.rB = std::max(1.6f * S.cdoR, 0.6f * S.outerR);
        O.strength = 0.25f * (float)smooth01(25.0, 50.0, S.vmax);   // (0.4: grey hard-edged blotches between the arms)
        O.thick = 0.35f;
        O.age01 = 0.85f;
        O.stormId = S.c->id ^ 0x0F10ull;
        if (O.strength > 0.0f) out.push_back(O);
        // CIRRUS along the spiral arms (2026-10-23, the user: "wispier still"): each arm's crest (the bake's spiral:
        // CycloneState::arms / tanPitch / bandRot) as a chain of 4 thin capsules from inside the CDO's edge out to the
        // outer bands, ~a quarter of the spacing between arms wide (frayed out to 1.6 of it by the shader), aged and thin,
        // weaker outward: ice streaming off the bands' tops, feathering their outlines.
        const double ds = smooth01(25.0, 45.0, (double)S.vmax) * std::clamp((double)armCirrus, 0.0, 3.0);
        if (ds > 0.0 && S.cdoR > 0.0f)
        {
            glm::dvec3 e, n;
            eastNorth(S.map, e, n);
            const int nA = std::max(2, (int)std::floor((double)S.arms + 0.15));
            const double rmw = std::max((double)S.rmw, 5000.0), r0 = 0.75 * (double)S.cdoR,
                         r1 = std::max(0.95 * (double)S.outerR, 1.3 * r0);
            constexpr int kSeg = 4;
            auto armPt = [&](int m, double r) {
                const double th = (double)S.c->hemi * (2.0 * kPi * m / nA - std::log(r / rmw) / (double)S.tanPitch - S.bandRot);
                return moveAlong(S.map, e * std::cos(th) + n * std::sin(th), r);
            };
            for (int m = 0; m < nA; ++m)
                for (int k = 0; k < kSeg; ++k)
                {
                    const double fa = (double)k / kSeg, fb = (double)(k + 1) / kSeg;
                    const double ra = r0 * std::pow(r1 / r0, fa), rb = r0 * std::pow(r1 / r0, fb);
                    Shield A = Sh;
                    A.a = armPt(m, ra);
                    A.b = armPt(m, rb);
                    const double gap = 2.0 * kPi / nA * (double)S.tanPitch;   // the arms' radial spacing / r
                    A.rA = (float)(0.25 * gap * ra);
                    A.rB = (float)(0.25 * gap * rb);
                    A.strength = (float)(0.32 * (1.0 - 0.55 * fa) * ds);
                    A.thick = 0.25f;
                    A.age01 = 0.9f;
                    A.eyeR = 0.0f;
                    A.stormId = S.c->id ^ (0xA000ull + 16ull * (uint64_t)m + (uint64_t)k);
                    out.push_back(A);
                }
        }
    }
}

} // namespace wx
