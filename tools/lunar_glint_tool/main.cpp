// LunarGlintTool — checks of the Moon-base glint model (src/simulations/LunarBases.cpp), no Vulkan:
//   cmake --build build --config Release --target LunarGlintTool
//   build/Release/LunarGlintTool.exe [data/lunar_bases.json] [--from 2036-11-22] [--days 730] [--limit 6] [--jitter 1]
// 1. A single flat panel aimed at an observer vs the hand formula m = -26.74 - 2.5 log10(R A cos i / (Omega_sun D^2)).
// 2. The glint windows of every base over the span: when, how long, how bright, the Moon's phase angle at the peak.
#include "LunarBases.h"
#include "SatPhotometry.h"

#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

namespace
{
constexpr double kPi = 3.14159265358979323846;
double j2000(int Y, int M, int D)
{
    const long long y = (long long)Y - (M <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(M + (M > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)D - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    return ((double)(era * 146097 + (long long)doe - 719468) - 10957.5) * 86400.0;
}
void utc(double t, char *b, size_t n)
{
    const double days = t / 86400.0 + 10957.5;
    long long z = (long long)std::floor(days) + 719468;
    const double fr = days - std::floor(days);
    const long long era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    const long long y = (long long)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    const unsigned d = doy - (153 * mp + 2) / 5 + 1;
    const unsigned m = mp < 10 ? mp + 3 : mp - 9;
    const int hh = (int)(fr * 24.0), mm = (int)((fr * 24.0 - hh) * 60.0);
    snprintf(b, n, "%04lld-%02u-%02u %02d:%02d", y + (m <= 2), m, d, hh, mm);
}
} // namespace

int main(int argc, char **argv)
{
    std::string path = "data/lunar_bases.json";
    double from = j2000(2036, 11, 22), days = 730.0, limit = 6.0, jitter = 1.0;
    for (int i = 1; i < argc; ++i)
    {
        const std::string a = argv[i];
        if (a == "--from" && i + 1 < argc)
        {
            int Y, M, D;
            if (std::sscanf(argv[++i], "%d-%d-%d", &Y, &M, &D) == 3) from = j2000(Y, M, D);
        }
        else if (a == "--days" && i + 1 < argc) days = std::atof(argv[++i]);
        else if (a == "--limit" && i + 1 < argc) limit = std::atof(argv[++i]);
        else if (a == "--jitter" && i + 1 < argc) jitter = std::atof(argv[++i]);
        else path = a;
    }
    std::vector<lunar::Base> bases;
    std::string err;
    if (!lunar::loadBases(path, bases, err))
    {
        std::printf("load failed: %s\n", err.c_str());
        return 1;
    }

    // ── 1. One flat panel vs the hand formula ────────────────────────────────────────────────────────────────────
    {
        lunar::Base b;
        b.rows = 1;
        b.areaM2 = 1000.0;
        b.rowTiltSigmaDeg = b.rowAzSigmaDeg = 0.0;
        b.panelFlatnessDeg = 1e-4;
        b.horizonDipDeg = 5.0;
        b.onlineT = -1e12;
        b.fullT = -1e11;
        int fails = 0;
        for (double eS : {-1.0, 0.0, 1.2})
            for (double lean : {0.0, 2.0, 5.0})
            {
                b.leanDeg = lean;
                lunar::Geometry g;
                g.up = glm::dvec3(0, 0, 1);
                g.baseEci = glm::dvec3(0.0);
                g.sunDistM = 1.495978707e11;
                g.sunAngR = std::asin(6.96e8 / g.sunDistM);
                g.sunDir = glm::dvec3(std::cos(eS * kPi / 180), 0.0, std::sin(eS * kPi / 180));
                g.sunVisible = 1.0;
                const glm::dvec3 rb = lunar::meanBeam(b, g);
                const double D = 3.844e8;
                const lunar::Glint gl = lunar::glintRows(b, g, 0.0, rb * D, 1.0);
                const glm::dvec3 n = glm::normalize(rb + g.sunDir);
                const double ci = glm::dot(n, g.sunDir);
                const double F = 0.04 + 0.96 * std::pow(1.0 - ci, 5.0);
                const double om = kPi * g.sunAngR * g.sunAngR;
                const double hand = -26.74 - 2.5 * std::log10(F * b.areaM2 * ci / (om * D * D));
                const double beamEl = std::asin(rb.z) * 180 / kPi;
                std::printf("panel: sun el %+.1f lean %.1f -> beam el %+.2f (expect %+.2f), model %.3f hand %.3f diff %+.4f\n",
                            eS, lean, beamEl, 2.0 * lean - eS, gl.mag, hand, gl.mag - hand);
                if (std::abs(gl.mag - hand) > 0.01 || std::abs(beamEl - (2.0 * lean - eS)) > 0.01) ++fails;
                // 0.5 deg off the mirrored Sun's centre (past its 0.27-deg radius): nothing.
                const glm::dvec3 off = glm::normalize(rb + glm::dvec3(0, 0.5 * kPi / 180, 0));
                const lunar::Glint g2 = lunar::glintRows(b, g, 0.0, off * D, 1.0);
                if (g2.mag < 30.0) ++fails;
            }
        std::printf("panel check: %s\n", fails ? "FAIL" : "ok");
    }

    // ── 2. Windows ───────────────────────────────────────────────────────────────────────────────────────────────
    std::vector<lunar::Window> w;
    lunar::findWindows(bases, from, from + days * 86400.0, limit, jitter, 0.0, nullptr, nullptr, w);
    char b0[32], b1[32];
    std::vector<int> count(bases.size(), 0);
    for (const lunar::Window &x : w)
    {
        ++count[(size_t)x.base];
        utc(x.tPeak, b0, sizeof(b0));
        utc(x.t0, b1, sizeof(b1));
        const double la = std::asin(x.site.ecef.z) * 180 / kPi, lo = std::atan2(x.site.ecef.y, x.site.ecef.x) * 180 / kPi;
        std::printf("%-8s peak %s (from %s, %.1f h) mag %.2f (site %.2f) at %.1f %.1f, Moon %.0f up, Sun %.0f; phase %.1f deg\n",
                    bases[(size_t)x.base].shortName.c_str(), b0, b1, (x.t1 - x.t0) / 3600.0, x.peakMag, x.site.mag, la, lo,
                    x.site.moonAltDeg, x.site.sunAltDeg, x.moonPhaseDeg);
    }
    for (size_t i = 0; i < bases.size(); ++i)
        std::printf("%s: %d windows brighter than mag %.1f in %.0f days\n", bases[i].name.c_str(), count[i], limit, days);
    return 0;
}
