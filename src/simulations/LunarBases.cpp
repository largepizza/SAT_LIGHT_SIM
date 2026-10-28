// LunarBases.cpp — the lunar south-pole bases' solar-array glints (2026-10-27). See LunarBases.h.
#include "LunarBases.h"
#include "SatPhotometry.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>

namespace lunar
{
namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kD2R = kPi / 180.0;

// "YYYY-MM-DD[THH:MM[:SS]]" UTC -> s since J2000 (2000-01-01 12:00 UTC).
bool parseUtc(const std::string &s, double &t)
{
    int Y = 0, M = 0, D = 0, h = 0, mi = 0, se = 0;
    const int n = std::sscanf(s.c_str(), "%d-%d-%d%*[T ]%d:%d:%d", &Y, &M, &D, &h, &mi, &se);
    if (n < 3 || M < 1 || M > 12 || D < 1 || D > 31) return false;
    // days from civil (Howard Hinnant)
    const long long y = (long long)Y - (M <= 2);
    const long long era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153u * (unsigned)(M + (M > 2 ? -3 : 9)) + 2u) / 5u + (unsigned)D - 1u;
    const unsigned doe = yoe * 365u + yoe / 4u - yoe / 100u + doy;
    const long long days = era * 146097 + (long long)doe - 719468; // days since 1970-01-01
    t = ((double)days - 10957.5) * 86400.0 + h * 3600.0 + mi * 60.0 + se;
    return true;
}
uint32_t hashU(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;
    return x;
}
double hash01(uint32_t a, uint32_t b, uint32_t c)
{
    return ((double)hashU(a * 0x9E3779B1u ^ hashU(b * 0x85EBCA77u ^ hashU(c + 0x27d4eb2fu))) + 0.5) / 4294967296.0;
}
// A standard normal from three hashed words (Box-Muller).
double gauss(uint32_t seed, uint32_t row, uint32_t which)
{
    const double u1 = hash01(seed, row, 2 * which), u2 = hash01(seed, row, 2 * which + 1);
    return std::sqrt(-2.0 * std::log(u1)) * std::cos(2.0 * kPi * u2);
}
double angle(const glm::dvec3 &a, const glm::dvec3 &b)
{
    // (atan2 of the cross and dot: exact for the small angles that matter here)
    return std::atan2(glm::length(glm::cross(a, b)), glm::dot(a, b));
}
// The share of a disc of radius r1 covered by one of radius r2 whose centre is d away (flat geometry, small angles).
double discOverlap(double r1, double r2, double d)
{
    if (d >= r1 + r2) return 0.0;
    if (d <= std::abs(r2 - r1)) return r2 >= r1 ? 1.0 : (r2 * r2) / (r1 * r1);
    const double a1 = std::acos(std::clamp((d * d + r1 * r1 - r2 * r2) / (2.0 * d * r1), -1.0, 1.0));
    const double a2 = std::acos(std::clamp((d * d + r2 * r2 - r1 * r1) / (2.0 * d * r2), -1.0, 1.0));
    const double lens = r1 * r1 * (a1 - std::sin(2.0 * a1) * 0.5) + r2 * r2 * (a2 - std::sin(2.0 * a2) * 0.5);
    return std::clamp(lens / (kPi * r1 * r1), 0.0, 1.0);
}
} // namespace

void moonBodyFrame(double t, glm::dvec3 &P, glm::dvec3 &X, glm::dvec3 &Y)
{
    // The IAU WGCCRE 2009 rotation model (Archinal et al. 2011, Table 2), ICRF taken as the sim's ECI: the same as
    // SatelliteSim::updatePositions (which calls this).
    const double d = t / 86400.0, T = d / 36525.0, r = kD2R;
    const double E1 = (125.045 - 0.0529921 * d) * r, E2 = (250.089 - 0.1059842 * d) * r,
                 E3 = (260.008 + 13.0120009 * d) * r, E4 = (176.625 + 13.3407154 * d) * r,
                 E5 = (357.529 + 0.9856003 * d) * r, E6 = (311.589 + 26.4057084 * d) * r,
                 E7 = (134.963 + 13.0649930 * d) * r, E8 = (276.617 + 0.3287146 * d) * r,
                 E9 = (34.226 + 1.7484877 * d) * r, E10 = (15.134 - 0.1589763 * d) * r,
                 E11 = (119.743 + 0.0036096 * d) * r, E12 = (239.961 + 0.1643573 * d) * r,
                 E13 = (25.053 + 12.9590088 * d) * r;
    const double a0 = (269.9949 + 0.0031 * T - 3.8787 * std::sin(E1) - 0.1204 * std::sin(E2) + 0.0700 * std::sin(E3) -
                       0.0172 * std::sin(E4) + 0.0072 * std::sin(E6) - 0.0052 * std::sin(E10) + 0.0043 * std::sin(E13)) * r;
    const double d0 = (66.5392 + 0.0130 * T + 1.5419 * std::cos(E1) + 0.0239 * std::cos(E2) - 0.0278 * std::cos(E3) +
                       0.0068 * std::cos(E4) - 0.0029 * std::cos(E6) + 0.0009 * std::cos(E7) + 0.0008 * std::cos(E10) -
                       0.0009 * std::cos(E13)) * r;
    const double W = std::fmod(38.3213 + 13.17635815 * d - 1.4e-12 * d * d + 3.5610 * std::sin(E1) + 0.1208 * std::sin(E2) -
                               0.0642 * std::sin(E3) + 0.0158 * std::sin(E4) + 0.0252 * std::sin(E5) - 0.0066 * std::sin(E6) -
                               0.0047 * std::sin(E7) - 0.0046 * std::sin(E8) + 0.0028 * std::sin(E9) + 0.0052 * std::sin(E10) +
                               0.0040 * std::sin(E11) + 0.0019 * std::sin(E12) - 0.0044 * std::sin(E13), 360.0) * r;
    P = glm::dvec3(std::cos(d0) * std::cos(a0), std::cos(d0) * std::sin(a0), std::sin(d0));
    const glm::dvec3 N(-std::sin(a0), std::cos(a0), 0.0);
    const glm::dvec3 M = glm::cross(P, N);
    X = std::cos(W) * N + std::sin(W) * M;
    Y = glm::cross(P, X);
}

glm::dvec3 bodyDir(const Base &b, const glm::dvec3 &P, const glm::dvec3 &X, const glm::dvec3 &Y)
{
    const double la = b.latDeg * kD2R, lo = b.lonDeg * kD2R;
    return std::cos(la) * std::cos(lo) * X + std::cos(la) * std::sin(lo) * Y + std::sin(la) * P;
}

bool loadBases(const std::string &path, std::vector<Base> &out, std::string &err)
{
    out.clear();
    std::ifstream f(path);
    if (!f)
    {
        err = "cannot open " + path;
        return false;
    }
    try
    {
        nlohmann::json j = nlohmann::json::parse(f, nullptr, true, true);
        uint32_t k = 0;
        for (const auto &e : j.at("bases"))
        {
            Base b;
            b.name = e.value("name", std::string("Moon base"));
            b.shortName = e.value("short", b.name);
            b.latDeg = e.at("lat_deg").get<double>();
            b.lonDeg = e.at("lon_deg").get<double>();
            b.elevM = e.value("elev_m", b.elevM);
            b.horizonDipDeg = e.value("horizon_dip_deg", b.horizonDipDeg);
            const auto &a = e.at("array");
            b.areaM2 = a.value("area_km2", 0.1) * 1e6;
            b.rows = std::max(1, a.value("rows", b.rows));
            b.rowTiltSigmaDeg = a.value("row_tilt_sigma_deg", b.rowTiltSigmaDeg);
            b.rowAzSigmaDeg = a.value("row_azimuth_sigma_deg", b.rowAzSigmaDeg);
            b.panelFlatnessDeg = a.value("panel_flatness_deg", b.panelFlatnessDeg);
            b.leanDeg = a.value("lean_deg", b.leanDeg);
            b.f0 = a.value("glass_f0", b.f0);
            if (!parseUtc(a.at("online").get<std::string>(), b.onlineT) || !parseUtc(a.at("complete").get<std::string>(), b.fullT))
            {
                err = "bad date in " + b.name;
                return false;
            }
            b.fullT = std::max(b.fullT, b.onlineT + 1.0);
            b.seed = hashU(0x51ED27u + ++k * 7919u);
            if (e.contains("tint") && e["tint"].is_array() && e["tint"].size() == 3)
                b.tint = glm::vec3(e["tint"][0].get<float>(), e["tint"][1].get<float>(), e["tint"][2].get<float>());
            out.push_back(b);
        }
    }
    catch (const std::exception &ex)
    {
        err = ex.what();
        out.clear();
        return false;
    }
    return true;
}

double builtShare(const Base &b, double t)
{
    if (t < b.onlineT) return 0.0;
    return std::clamp((t - b.onlineT) / (b.fullT - b.onlineT), 0.0, 1.0);
}

Geometry geometryAt(const Base &b, double t, const glm::dvec3 &moonGeoEci, const glm::dvec3 &P, const glm::dvec3 &X,
                    const glm::dvec3 &Y)
{
    Geometry g;
    g.up = bodyDir(b, P, X, Y);
    g.baseEci = moonGeoEci + (kMoonRadiusM + b.elevM) * g.up;
    const double sd = sunDistAuAt(t) * 1.495978707e11;
    const glm::dvec3 S = glm::normalize(sunDirEciAt(t)) * sd - g.baseEci;
    g.sunDistM = glm::length(S);
    g.sunDir = S / g.sunDistM;
    g.sunAngR = std::asin(satphot::kSunRadiusM / g.sunDistM);
    const double e = std::asin(std::clamp(glm::dot(g.sunDir, g.up), -1.0, 1.0));
    g.sunElevDeg = e / kD2R;
    // The disc's share over the local horizon (-dip): the area of a circle's cap, by the centre's height in radii.
    const double x = std::clamp((e + b.horizonDipDeg * kD2R) / g.sunAngR, -1.0, 1.0);
    g.sunVisible = (std::acos(-x) + x * std::sqrt(1.0 - x * x)) / kPi;
    // The Earth in front of the Sun (a lunar eclipse seen from the base: the glints come near FULL Moon, i.e. with
    // the Earth near the Sun in the base's sky). Its disc + ~1.5% of its radius of opaque low atmosphere.
    const double dE = glm::length(g.baseEci);
    const double aE = std::asin(std::min(6378137.0 * 1.015 / dE, 1.0));
    const double sep = angle(g.sunDir, -g.baseEci / dE);
    g.sunVisible *= 1.0 - discOverlap(g.sunAngR, aE, sep);
    return g;
}

namespace
{
// A row's normal: facing the Sun's azimuth (+ its tracking error), leaning back by lean (+ its tilt error).
glm::dvec3 rowNormal(const Geometry &g, double azErr, double el)
{
    glm::dvec3 sh = g.sunDir - g.up * glm::dot(g.sunDir, g.up);
    const double l = glm::length(sh);
    sh = l > 1e-12 ? sh / l : glm::dvec3(1.0, 0.0, 0.0);
    const glm::dvec3 side = glm::cross(g.up, sh);
    const glm::dvec3 h = sh * std::cos(azErr) + side * std::sin(azErr);
    return h * std::cos(el) + g.up * std::sin(el);
}
double schlick(double f0, double c) { return f0 + (1.0 - f0) * std::pow(1.0 - std::clamp(c, 0.0, 1.0), 5.0); }
void obsGeom(const Base &b, const Geometry &g, const glm::dvec3 &obsEci, glm::dvec3 &o, double &D, Glint &out)
{
    const glm::dvec3 v = obsEci - g.baseEci;
    D = glm::length(v);
    o = v / D;
    out.obsElevDeg = std::asin(std::clamp(glm::dot(o, g.up), -1.0, 1.0)) / kD2R;
    out.phaseDeg = angle(g.sunDir, o) / kD2R;
    out.visible = g.sunVisible > 0.0 && out.obsElevDeg > -b.horizonDipDeg;
}
} // namespace

glm::dvec3 meanBeam(const Base &b, const Geometry &g, double leanAddDeg)
{
    const glm::dvec3 n = rowNormal(g, 0.0, (b.leanDeg + leanAddDeg) * kD2R);
    return 2.0 * glm::dot(n, g.sunDir) * n - g.sunDir;
}

Glint glintRows(const Base &b, const Geometry &g, double t, const glm::dvec3 &obsEci, double jitterScale, double leanAddDeg)
{
    Glint out;
    glm::dvec3 o;
    double D;
    obsGeom(b, g, obsEci, o, D, out);
    out.beamOffDeg = angle(meanBeam(b, g, leanAddDeg), o) / kD2R;
    const double share = builtShare(b, t);
    if (!out.visible || share <= 0.0) return out;
    const int n = std::max(1, (int)std::lround(b.rows * share));
    const double aRow = b.areaM2 / (double)b.rows;
    const double rs = g.sunAngR, sig = std::max(2.0 * b.panelFlatnessDeg * kD2R, 1e-6);
    const double norm = 1.0 / (kPi * (rs * rs + sig * sig));
    const double lean = (b.leanDeg + leanAddDeg) * kD2R;
    double sum = 0.0;
    for (int k = 0; k < n; ++k)
    {
        const double dt = gauss(b.seed, (uint32_t)k, 0) * b.rowTiltSigmaDeg * jitterScale * kD2R;
        const double da = gauss(b.seed, (uint32_t)k, 1) * b.rowAzSigmaDeg * jitterScale * kD2R;
        const glm::dvec3 nr = rowNormal(g, da, lean + dt);
        const double ci = glm::dot(nr, g.sunDir);
        double v = 0.0;
        if (ci > 0.0 && glm::dot(nr, o) > 0.0)
        {
            const glm::dvec3 r = 2.0 * ci * nr - g.sunDir;
            const double dl = angle(r, o);
            const double p = 0.5 * std::erfc((dl - rs) / (1.41421356237 * sig)) * norm;
            v = schlick(b.f0, ci) * aRow * ci * p;
            if (p > 0.01 * norm) out.rowsLit += 1.0; // (the row's mirrored Sun covers the observer)
        }
        sum += v;
    }
    out.ratio = sum * g.sunVisible / (D * D);
    out.mag = magFromRatio(out.ratio, g.sunDistM);
    return out;
}

Glint glintAggregate(const Base &b, const Geometry &g, double t, const glm::dvec3 &obsEci, double jitterScale, double leanAddDeg)
{
    Glint out;
    glm::dvec3 o;
    double D;
    obsGeom(b, g, obsEci, o, D, out);
    const glm::dvec3 rb = meanBeam(b, g, leanAddDeg);
    out.beamOffDeg = angle(rb, o) / kD2R;
    const double share = builtShare(b, t);
    if (!out.visible || share <= 0.0) return out;
    // The beam's spread per axis: the disc + the flatness, and twice each row error (a mirror doubles a tilt).
    const double rs = g.sunAngR, sig = 2.0 * b.panelFlatnessDeg * kD2R;
    const double d2 = 0.25 * rs * rs + sig * sig;
    const double st = 2.0 * b.rowTiltSigmaDeg * jitterScale * kD2R, sa = 2.0 * b.rowAzSigmaDeg * jitterScale * kD2R;
    const double sEl = std::sqrt(d2 + st * st), sAz = std::sqrt(d2 + sa * sa);
    // Offsets in the base's local frame: elevation, and azimuth x cos(elevation).
    const double eB = std::asin(std::clamp(glm::dot(rb, g.up), -1.0, 1.0)), eO = std::asin(std::clamp(glm::dot(o, g.up), -1.0, 1.0));
    glm::dvec3 hb = rb - g.up * glm::dot(rb, g.up), ho = o - g.up * glm::dot(o, g.up);
    const double azD = angle(hb, ho) * std::cos(0.5 * (eB + eO));
    const double dEl = eO - eB;
    const double p = std::exp(-0.5 * (dEl * dEl / (sEl * sEl) + azD * azD / (sAz * sAz))) / (2.0 * kPi * sEl * sAz);
    const glm::dvec3 nm = glm::normalize(rb + g.sunDir);
    const double ci = std::max(glm::dot(nm, g.sunDir), 0.0);
    out.ratio = schlick(b.f0, ci) * b.areaM2 * share * ci * p * g.sunVisible / (D * D);
    out.mag = magFromRatio(out.ratio, g.sunDistM);
    return out;
}

namespace
{
glm::dvec3 eciToEcefL(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return glm::dvec3(c * v.x + s * v.y, -s * v.x + c * v.y, v.z);
}
constexpr double kRe = 6371000.0;
} // namespace

bool bestSite(const Base &b, double t, double jitterScale, double leanAddDeg, Site &out, double minMoonAltDeg,
              double maxSunAltDeg, bool useRows)
{
    glm::dvec3 P, X, Y;
    moonBodyFrame(t, P, X, Y);
    const glm::dvec3 M = moonGeoEciAt(t);
    const Geometry g = geometryAt(b, t, M, P, X, Y);
    if (g.sunVisible <= 0.0 || builtShare(b, t) <= 0.0) return false;
    const glm::dvec3 se = eciToEcefL(glm::normalize(sunDirEciAt(t)), t);
    const double sMoon = std::sin(minMoonAltDeg * kD2R), sSun = std::sin(maxSunAltDeg * kD2R);
    bool ok = false;
    out.mag = 99.0;
    // Coarse grid (3 deg), then refined round the best (0.5 deg).
    bool rowsNow = false;
    auto tryAt = [&](double la, double lo) {
        const double cl = std::cos(la * kD2R), sl = std::sin(la * kD2R);
        const glm::dvec3 u(cl * std::cos(lo * kD2R), cl * std::sin(lo * kD2R), sl);
        const double sunS = glm::dot(se, u);
        if (sunS > sSun) return;
        const glm::dvec3 O = observerEciAt(u, kRe, t);
        const glm::dvec3 md = glm::normalize(M - O);
        const double moonS = glm::dot(md, glm::normalize(O));
        if (moonS < sMoon) return;
        const Glint gl = rowsNow ? glintRows(b, g, t, O, jitterScale, leanAddDeg) : glintAggregate(b, g, t, O, jitterScale, leanAddDeg);
        if (gl.mag < out.mag)
        {
            out.ecef = u;
            out.mag = gl.mag;
            out.moonAltDeg = std::asin(std::clamp(moonS, -1.0, 1.0)) / kD2R;
            out.sunAltDeg = std::asin(std::clamp(sunS, -1.0, 1.0)) / kD2R;
            ok = true;
        }
    };
    // 5-deg grid, then 1 deg round the best, then 0.25 deg (with the rows when asked).
    for (double la = -80.0; la <= 80.0; la += 5.0)
        for (double lo = -180.0; lo < 180.0; lo += 5.0) tryAt(la, lo);
    if (!ok) return false;
    auto refine = [&](double span, double step) {
        const double la0 = std::asin(std::clamp(out.ecef.z, -1.0, 1.0)) / kD2R, lo0 = std::atan2(out.ecef.y, out.ecef.x) / kD2R;
        for (double dla = -span; dla <= span + 1e-9; dla += step)
            for (double dlo = -span; dlo <= span + 1e-9; dlo += step) tryAt(std::clamp(la0 + dla, -89.0, 89.0), lo0 + dlo);
    };
    refine(5.0, 1.0);
    if (useRows)
    {
        rowsNow = true;
        out.mag = 99.0;
        refine(2.0, 0.5);
        refine(0.5, 0.25);
    }
    else
        refine(1.0, 0.25);
    return out.mag < 98.0;
}

void findWindows(const std::vector<Base> &bases, double t0, double t1, double magLimit, double jitterScale,
                 double leanAddDeg, bool (*cancelled)(const void *), const void *cancelCtx, std::vector<Window> &out)
{
    constexpr double kCoarse = 1800.0, kFine = 300.0;
    auto stop = [&]() { return cancelled && cancelled(cancelCtx); };
    for (size_t bi = 0; bi < bases.size(); ++bi)
    {
        const Base &b = bases[bi];
        // The beam reaches the Earth only if its mean axis passes within the Earth's radius + ~4 sigma of the spread.
        const double spreadDeg = 2.0 * std::max(b.rowTiltSigmaDeg, b.rowAzSigmaDeg) * jitterScale + 0.3 + 2.0 * b.panelFlatnessDeg;
        Window cur;
        bool in = false;
        auto close = [&]() {
            if (!in) return;
            in = false;
            // The peak re-measured with the rows (their hashed scatter: the aggregate's far tail may hold no row);
            // a window the rows do not make brighter than the limit is dropped.
            Site sr;
            if (!bestSite(b, cur.tPeak, jitterScale, leanAddDeg, sr, 10.0, -6.0, true) || sr.mag >= magLimit) return;
            cur.site = sr;
            cur.peakMag = sr.mag;
            const glm::dvec3 M = moonGeoEciAt(cur.tPeak);
            const glm::dvec3 m = glm::normalize(M), s = glm::normalize(sunDirEciAt(cur.tPeak));
            cur.moonPhaseDeg = std::acos(std::clamp(-glm::dot(m, s), -1.0, 1.0)) / kD2R;
            out.push_back(cur);
        };
        for (double tc = std::max(t0, b.onlineT); tc < t1; tc += kCoarse)
        {
            if (stop()) return;
            glm::dvec3 P, X, Y;
            moonBodyFrame(tc, P, X, Y);
            const glm::dvec3 M = moonGeoEciAt(tc);
            const Geometry g = geometryAt(b, tc, M, P, X, Y);
            const glm::dvec3 toE = glm::normalize(-g.baseEci), mb = meanBeam(b, g, leanAddDeg);
            const double off = std::atan2(glm::length(glm::cross(mb, toE)), glm::dot(mb, toE)) / kD2R;
            // (the Moon's phase moves ~0.25 deg in 30 min: a margin for the step)
            if (off > 1.0 + 4.0 * spreadDeg + 0.6)
            {
                close();
                continue;
            }
            for (double tf = tc; tf < std::min(tc + kCoarse, t1); tf += kFine)
            {
                Site s;
                if (bestSite(b, tf, jitterScale, leanAddDeg, s) && s.mag < magLimit)
                {
                    if (!in)
                    {
                        in = true;
                        cur = Window();
                        cur.base = (int)bi;
                        cur.t0 = tf;
                        cur.site = s;
                        cur.tPeak = tf;
                    }
                    cur.t1 = tf + kFine;
                    if (s.mag < cur.site.mag)
                    {
                        cur.site = s;
                        cur.tPeak = tf;
                    }
                }
                else
                    close();
            }
        }
        close();
    }
    std::sort(out.begin(), out.end(), [](const Window &a, const Window &c) { return a.tPeak < c.tPeak; });
}
} // namespace lunar
