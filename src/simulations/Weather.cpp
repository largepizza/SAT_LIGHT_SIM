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

float towerStrength(const TowerSpec &ts, double tau) {
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
    double meanLife[3] = {0, 0, 0};  // per kind, seconds
    std::vector<Cell> cells;
    std::map<int64_t, std::vector<Storm>> bins;
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
                sum += (double)S.life;
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
    }
};

Catalog::Catalog() : impl_(std::make_unique<Impl>()) {}
Catalog::~Catalog() = default;

void Catalog::configure(const Env &env, const Settings &s, const StormClimatology *clim, uint64_t envKey) {
    Impl &I = *impl_;
    const bool same = I.configured && envKey == I.envKey && clim == I.clim && s.frequency == I.set.frequency &&
                      s.targetActive == I.set.targetActive && s.moistureMin == I.set.moistureMin &&
                      s.moistureFull == I.set.moistureFull && s.towerRadiusM == I.set.towerRadiusM &&
                      s.lifeScale == I.set.lifeScale;
    I.env = env;
    if (same) return;
    I.set = s;
    I.clim = clim;
    I.envKey = envKey;
    I.bins.clear();
    if (I.cells.empty()) I.buildCells();
    I.calibrate();
    I.configured = true;
}

void Catalog::setBuildThreads(int n) { impl_->threads = std::max(1, n); }
double Catalog::calibrationK() const { return impl_->K; }
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
        it = (it->first < keepLo || it->first > keepHi) ? I.bins.erase(it) : std::next(it);

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
        for (const Storm &S : it->second)
            if (t >= S.t0 && t < S.t0 + (double)S.life) out.push_back(&S);
    }
    I.st.binsCached = (int)I.bins.size();
    I.st.stormsAlive = (int)out.size();
}

void Catalog::towersAt(double t, std::vector<Tower> &out) {
    out.clear();
    std::vector<const Storm *> storms;
    stormsAt(t, storms);
    for (const Storm *S : storms) {
        const glm::dvec3 c = S->centreAt(t);
        glm::dvec3 e, n;
        eastNorth(c, e, n);
        const double tau = t - S->t0;
        for (int i = 0; i < S->towerCount; ++i) {
            const TowerSpec &ts = S->towers[i];
            const float s = towerStrength(ts, tau);
            if (s <= 0.0f) continue;
            Tower T;
            const glm::dvec3 o = e * (double)ts.offE + n * (double)ts.offN;
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
            T.seed = ts.seed;
            T.stormId = S->id;
            T.towerIndex = (uint8_t)i;
            T.kind = S->kind;
            T.age = (float)(tau - ts.t0);
            T.growS = ts.growS;
            T.life = ts.life;
            out.push_back(T);
        }
    }
    impl_->st.towersAlive = (int)out.size();
}

const Storm *Catalog::find(uint64_t id) const {
    for (const auto &kv : impl_->bins)
        for (const Storm &S : kv.second)
            if (S.id == id) return &S;
    return nullptr;
}

Catalog::Stats Catalog::stats() const { return impl_->st; }

} // namespace wx
