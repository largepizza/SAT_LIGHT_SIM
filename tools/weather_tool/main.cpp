// WeatherTool: the storm catalog (src/simulations/Weather.cpp) on a synthetic Env, no Vulkan, no window.
// Prints storm counts through 2036, the latitude and local-time distributions, bin build costs and a
// determinism check. Run from the repository root (reads assets/textures/earth_water_sdf.r8: raw bytes only).
//   cmake --build build --config Release --target WeatherTool
//   build/Release/WeatherTool.exe [--threads N]
#include "StormClimatology.h"
#include "Weather.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
constexpr double kDriftEpochS = 1150891200.0; // 2036-06-21 00:00 UTC (kCloudDriftEpochS)
constexpr double kDriftRate = 6.55e-6;        // rad/s

int64_t daysFromCivil(int64_t y, unsigned m, unsigned d) {
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
double j2000(int y, int mo, int d, int h) {
    return (double)(daysFromCivil(y, (unsigned)mo, (unsigned)d) - daysFromCivil(2000, 1, 1)) * 86400.0 - 43200.0 +
           h * 3600.0;
}

struct LandMask {
    int w = 0, h = 0;
    std::vector<unsigned char> px;
    bool load(const char *path) {
        FILE *f = std::fopen(path, "rb");
        if (!f) return false;
        char magic[4];
        uint32_t hdr[3];
        bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "SLR8", 4) == 0 && std::fread(hdr, 4, 3, f) == 3 &&
                  hdr[2] == 0;
        if (ok) {
            w = (int)hdr[0];
            h = (int)hdr[1];
            px.resize((size_t)w * h);
            ok = std::fread(px.data(), 1, px.size(), f) == px.size();
        }
        std::fclose(f);
        return ok;
    }
    float at(const glm::dvec3 &e) const {
        if (px.empty()) return 0.0f;
        const double lat = std::asin(std::clamp(e.z, -1.0, 1.0)), lon = std::atan2(e.y, e.x);
        int x = (int)((lon + kPi) / (2.0 * kPi) * w), y = (int)((0.5 * kPi - lat) / kPi * h);
        x = std::clamp(x, 0, w - 1);
        y = std::clamp(y, 0, h - 1);
        return px[(size_t)y * w + x] > 128 ? 0.0f : 1.0f;
    }
};

glm::dvec3 rotZ(const glm::dvec3 &p, double a) {
    const double c = std::cos(a), s = std::sin(a);
    return {c * p.x - s * p.y, s * p.x + c * p.y, p.z};
}

float moistureField(const glm::dvec3 &p) {
    const double lat = std::asin(std::clamp(p.z, -1.0, 1.0)), lon = std::atan2(p.y, p.x);
    double m = 0.42 + 0.22 * std::sin(3.0 * lon + 1.7 * std::sin(2.0 * lat)) * std::cos(2.5 * lat) +
               0.15 * std::sin(5.0 * lon - 2.0 * lat + 0.7) * std::cos(lat) +
               0.25 * std::exp(-std::pow(lat / (14.0 * kDeg), 2.0)) -
               0.15 * std::exp(-std::pow((std::fabs(lat) - 28.0 * kDeg) / (8.0 * kDeg), 2.0));
    return (float)std::clamp(m, 0.0, 1.0);
}

glm::dvec3 sunEarthAt(double t) {
    const double n = t / 86400.0;
    const double L = (280.460 + 0.9856474 * n) * kDeg, g = (357.528 + 0.9856003 * n) * kDeg;
    const double lam = L + (1.915 * std::sin(g) + 0.020 * std::sin(2.0 * g)) * kDeg;
    const double eps = 23.439 * kDeg;
    const glm::dvec3 eci(std::cos(lam), std::cos(eps) * std::sin(lam), std::sin(eps) * std::sin(lam));
    const double gmst = std::fmod(280.46061837 + 360.98564736629 * n, 360.0) * kDeg;
    return rotZ(eci, -gmst);
}

const char *kindName(wx::StormKind k) {
    return k == wx::StormKind::Supercell ? "supercell" : k == wx::StormKind::Multicell ? "multicell" : "air-mass";
}

bool sameStorm(const wx::Storm &a, const wx::Storm &b) {
    bool ok = a.id == b.id && a.kind == b.kind && a.t0 == b.t0 && a.life == b.life && a.pos0 == b.pos0 &&
              a.vel == b.vel && a.shear == b.shear && a.potential == b.potential && a.moisture == b.moisture &&
              a.towerCount == b.towerCount;
    for (int i = 0; ok && i < a.towerCount; ++i) {
        const wx::TowerSpec &x = a.towers[i], &y = b.towers[i];
        ok = x.offE == y.offE && x.offN == y.offN && x.t0 == y.t0 && x.life == y.life && x.peak == y.peak &&
             x.growS == y.growS && x.decayS == y.decayS && x.role == y.role && x.size == y.size &&
             x.overshoot == y.overshoot && x.seed == y.seed;
    }
    return ok;
}

double ms(std::chrono::steady_clock::time_point a) {
    return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - a).count();
}

} // namespace

int main(int argc, char **argv) {
    int threads = 8;
    bool flat = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--threads") && i + 1 < argc) threads = std::max(1, std::atoi(argv[++i]));
        if (!std::strcmp(argv[i], "--flat")) flat = true; // no climatology: the catalog's own cost
    }

    LandMask mask;
    if (!mask.load("assets/textures/earth_water_sdf.r8"))
        std::printf("warning: assets/textures/earth_water_sdf.r8 not read: everything is sea\n");
    else
        std::printf("land mask %dx%d\n", mask.w, mask.h);

    wx::StormClimatology clim;
    const bool tab = clim.load("assets/textures/storm_climatology.bin");
    std::printf("climatology: %s\n", flat ? "none (--flat: uniform density)" : tab ? "baked table" : "analytic model");
    const wx::StormClimatology *climP = flat ? nullptr : &clim;

    wx::Env env;
    env.moisture = moistureField;
    env.mapToEarth = [](const glm::dvec3 &d, double t) { return rotZ(d, -kDriftRate * (t - kDriftEpochS)); };
    env.land = [&mask](const glm::dvec3 &e) { return mask.at(e); };
    env.sunEarth = sunEarthAt;
    wx::Settings set;

    auto tc = std::chrono::steady_clock::now();
    wx::Catalog cat;
    cat.configure(env, set, climP, 1);
    std::printf("configure (calibration) %.1f ms: K = %.4e, mean life air-mass %.1f min, multicell %.1f min, "
                "supercell %.1f min\n",
                ms(tc), cat.calibrationK(), cat.meanLifeS(wx::StormKind::AirMass) / 60.0,
                cat.meanLifeS(wx::StormKind::Multicell) / 60.0, cat.meanLifeS(wx::StormKind::Supercell) / 60.0);

    // --- counts through 2036 -----------------------------------------------------------------------------
    std::printf("\nStorms / towers alive on the 15th of each month, 2036 (00/06/12/18 UTC):\n");
    std::printf("  month   00h         06h         12h         18h         air-mass  multi  super\n");
    std::vector<const wx::Storm *> storms;
    std::vector<wx::Tower> towers;
    double latHist[18] = {}, kindN[3] = {}, totalStorms = 0, totalTowers = 0;
    std::vector<double> buildMs;
    for (int mo = 1; mo <= 12; ++mo) {
        std::printf("  %2d   ", mo);
        double kn[3] = {};
        for (int h = 0; h < 24; h += 6) {
            const double t = j2000(2036, mo, 15, h);
            cat.towersAt(t, towers);
            const wx::Catalog::Stats st0 = cat.stats();
            cat.stormsAt(t, storms);
            (void)st0;
            for (const wx::Storm *S : storms) {
                kn[(int)S->kind] += 1;
                kindN[(int)S->kind] += 1;
                const double lat = std::asin(S->pos0.z) / kDeg;
                latHist[std::clamp((int)((lat + 90.0) / 10.0), 0, 17)] += 1;
            }
            totalStorms += (double)storms.size();
            totalTowers += (double)towers.size();
            std::printf("%5zu/%-5zu ", storms.size(), towers.size());
        }
        const double s = kn[0] + kn[1] + kn[2];
        std::printf(" %5.1f%%  %5.1f%%  %4.1f%%\n", 100 * kn[0] / s, 100 * kn[1] / s, 100 * kn[2] / s);
    }
    const double ksum = kindN[0] + kindN[1] + kindN[2];
    std::printf("  mean alive %.0f storms, %.0f towers; kinds: air-mass %.1f%%, multicell %.1f%%, supercell %.1f%%\n",
                totalStorms / 48.0, totalTowers / 48.0, 100 * kindN[0] / ksum, 100 * kindN[1] / ksum,
                100 * kindN[2] / ksum);

    std::printf("\nLatitude histogram of storms alive (all 48 instants):\n");
    for (int i = 17; i >= 0; --i) {
        const double share = latHist[i] / std::max(1.0, ksum);
        std::printf("  %+4d..%+4d  %6.0f  %5.1f%%  %s\n", i * 10 - 90, i * 10 - 80, latHist[i], 100 * share,
                    std::string((size_t)(share * 200.0), '#').c_str());
    }

    // --- births by local solar hour, land vs sea, and bin build cost ---------------------------------------
    double landH[24] = {}, seaH[24] = {};
    {
        wx::Catalog c2;
        c2.configure(env, set, climP, 1);
        std::vector<const wx::Storm *> born;
        for (int mo = 1; mo <= 12; ++mo) {
            const int64_t b0 = (int64_t)std::floor(j2000(2036, mo, 15, 0) / wx::Catalog::kBinS);
            for (int64_t b = b0; b < b0 + 48; ++b) {
                const auto t0 = std::chrono::steady_clock::now();
                c2.stormsInBin(b, born);
                buildMs.push_back(ms(t0));
                for (const wx::Storm *S : born) {
                    const glm::dvec3 e = env.mapToEarth(S->pos0, S->t0);
                    const glm::dvec3 sun = env.sunEarth(S->t0);
                    double d = (std::atan2(e.y, e.x) - std::atan2(sun.y, sun.x)) / kDeg;
                    d = std::fmod(d + 540.0, 360.0) - 180.0;
                    const int hr = std::clamp((int)std::floor(std::fmod(12.0 + d / 15.0 + 24.0, 24.0)), 0, 23);
                    (env.land(e) > 0.5f ? landH : seaH)[hr] += 1;
                }
            }
            // Keep memory bounded: a far query evicts the month's bins.
            c2.stormsAt(j2000(2036, mo, 15, 0) + 40 * 86400.0, born);
        }
    }
    double lt = 0, stt = 0;
    for (int i = 0; i < 24; ++i) { lt += landH[i]; stt += seaH[i]; }
    std::printf("\nBirths by local solar hour (15th of each month, 2036): land %.0f, sea %.0f\n", lt, stt);
    std::printf("  hour   land%%   sea%%\n");
    for (int i = 0; i < 24; ++i)
        std::printf("  %02d   %5.1f   %5.1f   %s|%s\n", i, 100 * landH[i] / lt, 100 * seaH[i] / stt,
                    std::string((size_t)(landH[i] / lt * 300.0), 'L').c_str(),
                    std::string((size_t)(seaH[i] / stt * 300.0), 's').c_str());

    std::sort(buildMs.begin(), buildMs.end());
    double mean = 0;
    for (double v : buildMs) mean += v;
    mean /= (double)buildMs.size();
    std::printf("\nBin build (1 thread, %zu bins): mean %.2f ms, median %.2f, p95 %.2f, max %.2f\n", buildMs.size(),
                mean, buildMs[buildMs.size() / 2], buildMs[buildMs.size() * 95 / 100], buildMs.back());

    for (int th : {1, threads}) {
        wx::Catalog cj;
        cj.configure(env, set, climP, 1);
        cj.setBuildThreads(th);
        cj.stormsAt(j2000(2036, 8, 1, 12), storms);
        const wx::Catalog::Stats st = cj.stats();
        std::printf("Jump (fresh catalog, %d thread%s): %d bins in %.1f ms (%.2f ms/bin), %d storms alive\n", th,
                    th > 1 ? "s" : "", st.binsBuiltLastCall, st.buildMsLastCall,
                    st.buildMsLastCall / std::max(1, st.binsBuiltLastCall), st.stormsAlive);
        cj.towersAt(j2000(2036, 8, 1, 12) + 600.0, towers);
        const wx::Catalog::Stats s2 = cj.stats();
        std::printf("  +10 min: %d bins built, %.2f ms; towersAt %d towers\n", s2.binsBuiltLastCall,
                    s2.buildMsLastCall, s2.towersAlive);
    }

    // --- calibration check: moisture 1 everywhere --------------------------------------------------------
    {
        wx::Env e1 = env;
        e1.moisture = [](const glm::dvec3 &) { return 1.0f; };
        wx::Catalog c1;
        c1.configure(e1, set, climP, 2);
        c1.setBuildThreads(threads);
        double sum = 0;
        int n = 0;
        for (int mo = 1; mo <= 12; ++mo)
            for (int h = 0; h < 24; h += 6) {
                c1.stormsAt(j2000(2036, mo, 15, h), storms);
                sum += (double)storms.size();
                ++n;
            }
        std::printf("\nCalibration check (moisture 1 everywhere): mean alive %.0f vs target %.0f\n", sum / n,
                    (double)set.targetActive);
    }

    // --- determinism: forward vs reverse vs threaded fresh ------------------------------------------------
    {
        const int64_t b0 = (int64_t)std::floor(j2000(2036, 3, 20, 6) / wx::Catalog::kBinS);
        const int N = 16;
        wx::Catalog A, B, C, D;
        A.configure(env, set, climP, 1);
        B.configure(env, set, climP, 1);
        C.configure(env, set, climP, 1);
        C.setBuildThreads(threads);
        D.configure(env, set, climP, 1);
        D.setBuildThreads(threads);
        std::vector<std::vector<wx::Storm>> fa(N), fb(N), fc(N), fd(N);
        // D: one bin at a time, each split over threads by cell, in a shuffled order.
        for (int i = 0; i < N; ++i) {
            const int j = (i * 7 + 3) % N;
            D.stormsInBin(b0 + j, storms);
            for (auto *s : storms) fd[(size_t)j].push_back(*s);
        }
        for (int i = 0; i < N; ++i) {
            A.stormsInBin(b0 + i, storms);
            for (auto *s : storms) fa[(size_t)i].push_back(*s);
        }
        for (int i = N - 1; i >= 0; --i) {
            B.stormsInBin(b0 + i, storms);
            for (auto *s : storms) fb[(size_t)i].push_back(*s);
        }
        // C: through stormsAt (a parallel multi-bin build), then read the bins back.
        C.stormsAt((double)(b0 + N - 1) * wx::Catalog::kBinS + 1.0, storms);
        for (int i = 0; i < N; ++i) {
            C.stormsInBin(b0 + i, storms);
            for (auto *s : storms) fc[(size_t)i].push_back(*s);
        }
        size_t total = 0, bad = 0;
        for (int i = 0; i < N; ++i) {
            if (fa[(size_t)i].size() != fb[(size_t)i].size() || fa[(size_t)i].size() != fc[(size_t)i].size() ||
                fa[(size_t)i].size() != fd[(size_t)i].size()) {
                ++bad;
                continue;
            }
            for (size_t j = 0; j < fa[(size_t)i].size(); ++j) {
                ++total;
                if (!sameStorm(fa[(size_t)i][j], fb[(size_t)i][j]) || !sameStorm(fa[(size_t)i][j], fc[(size_t)i][j]) ||
                    !sameStorm(fa[(size_t)i][j], fd[(size_t)i][j]))
                    ++bad;
            }
        }
        std::printf("\nDeterminism: %d bins, %zu storms, forward vs reverse vs %d-thread jump vs per-bin cell split: %s (%zu mismatches)\n",
                    N, total, threads, bad == 0 ? "IDENTICAL" : "DIFFERENT", bad);
    }

    // --- sample storms -----------------------------------------------------------------------------------
    {
        const double t = j2000(2036, 6, 15, 21);
        cat.stormsAt(t, storms);
        std::vector<const wx::Storm *> pick;
        for (wx::StormKind k : {wx::StormKind::Supercell, wx::StormKind::Multicell, wx::StormKind::AirMass})
            for (const wx::Storm *S : storms)
                if (S->kind == k && std::count(pick.begin(), pick.end(), S) == 0) {
                    pick.push_back(S);
                    if ((k == wx::StormKind::AirMass && pick.size() >= 5) || k != wx::StormKind::AirMass) break;
                }
        std::printf("\nSample storms (alive 2036-06-15 21 UTC):\n");
        for (const wx::Storm *S : pick) {
            const glm::dvec3 e = env.mapToEarth(S->pos0, S->t0);
            std::printf("  %016llx %-9s lat %+6.1f lon %+7.1f (Earth at birth) land %.0f life %.0f min speed %.1f m/s "
                        "fd %.2f moist %.2f\n",
                        (unsigned long long)S->id, kindName(S->kind), std::asin(e.z) / kDeg,
                        std::atan2(e.y, e.x) / kDeg, (double)env.land(e), S->life / 60.0, glm::length(S->vel),
                        (double)S->potential, (double)S->moisture);
            for (int i = 0; i < S->towerCount; ++i) {
                const wx::TowerSpec &ts = S->towers[i];
                std::printf("    tower %d role %.1f off (%+.0f, %+.0f) km t0 %3.0f life %3.0f min grow %2.0f decay %2.0f "
                            "peak %.2f:",
                            i, ts.role, ts.offE / 1000.0, ts.offN / 1000.0, ts.t0 / 60.0, ts.life / 60.0,
                            ts.growS / 60.0, ts.decayS / 60.0, ts.peak);
                for (int k = 0; k <= 12; ++k)
                    std::printf(" %.2f", wx::towerStrength(ts, (double)S->life * k / 12.0));
                std::printf("\n");
            }
        }
    }
    return 0;
}
