// SatelliteSimWildfire.cpp — the wildfires drawn (2026-10-23, Wildfire.h, .plans/WEATHER_HAZARDS_PLAN.md section 1).
//
// Smoke: plume Gaussians appended to the dust list after the tornadoes, as one contiguous range whose bounds go in the
// list header's word 1 (bits 8-17 first, 18-27 count; smoke.glsl). cloud_march.comp composites them like the rocket
// plumes but SPECTRALLY (blue extinguished more than red: the Sun through smoke is red and dim) and reddens the
// sunlight inside them by their own path to the Sun; sat_sky.frag dims and reddens the Sun's light on the ground under
// them. Each fire is a column that rises from its head, bends downwind and flattens out at its injection height
// (a polyline of Gaussians, the launches' trails' scheme) plus one broad, flat PALL of the smoke it has made over the
// last days, carried downwind: many fires' palls overlap into the regional haze of a fire season.
// Fire: the front (the burnt ellipse's head and flanks) as point sprites in kFireBuf, flickering, brightest at night;
// the nearest fires' glow as engine lights (they light their own smoke, the cloud and the ground: lightning.frag
// mode 2, cloud_march.comp).
#include "SatelliteSim.h"
#include "FrameProfile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <array>
#include <cmath>

namespace {
constexpr double kReF = 6371000.0;
constexpr double kPiF = 3.14159265358979323846;
uint64_t fwMix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double fwU(uint64_t h) { return (double)(h >> 11) * (1.0 / 9007199254740992.0); }
glm::dvec3 fwEcefToEci(const glm::dvec3 &v, double ang)
{
    const double c = std::cos(ang), sn = std::sin(ang);
    return {v.x * c - v.y * sn, v.x * sn + v.y * c, v.z};
}
glm::dvec3 llDir(double latDeg, double lonDeg)
{
    const double la = latDeg * kPiF / 180.0, lo = lonDeg * kPiF / 180.0;
    return {std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)};
}
} // namespace

void SatelliteSim::weatherFillWildfires(GpuDustCloud *out, uint32_t &n)
{
    FPROF_SCOPE("weatherFillWildfires");
    smokeHdrBits = 0;
    firesNow.clear();
    if (!out || wildfireRate <= 0.0f || !biomeMap.ready()) return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const double eyeR = (double)obsEyeRadiusM();
    const glm::dvec3 eye = up * eyeR;
    const double alt = std::max(eyeR - kReF, 0.0);
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, E), glm::dot(v, N), glm::dot(v, up)); };
    const double eyeLat = glm::degrees(std::asin(std::clamp(up.z, -1.0, 1.0)));
    const double eyeLon = glm::degrees(std::atan2(up.y, up.x));

    // The fires in reach: from the ground ~600 km (the smoke on the horizon), from orbit to ~3000 km.
    const double reachKm = std::clamp(600.0 + 3.0 * alt / 1000.0, 600.0, 3000.0);
    {
        const int64_t b1 = (int64_t)std::floor(t / 86400.0), b0 = (int64_t)std::floor((t - wx::kFireMaxLifeS) / 86400.0);
        const double moved = std::acos(std::clamp(glm::dot(up, llDir(fireCacheLat, fireCacheLon)), -1.0, 1.0)) * 6371.0;
        const bool rescan = fireCacheRate != wildfireRate || moved > 0.15 * fireCacheR || reachKm > fireCacheR * 0.9 ||
                            reachKm < fireCacheR * 0.5 || b0 > fireCacheB1 || b1 < fireCacheB0;
        if (rescan)
        {
            fireCache.clear();
            fireCacheLat = eyeLat;
            fireCacheLon = eyeLon;
            fireCacheR = reachKm * 1.3;
            fireCacheRate = wildfireRate;
            wx::firesBorn(b0, b1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
            fireCacheB0 = b0;
            fireCacheB1 = b1;
        }
        else
        {
            // Time moved: add the new days' fires (forward) or the older days' (backward); the window's other end is
            // dropped by the alive test below, and the cache is bounded by a rescan when it slides past.
            if (b1 > fireCacheB1)
            {
                wx::firesBorn(fireCacheB1 + 1, b1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
                fireCacheB1 = b1;
            }
            if (b0 < fireCacheB0)
            {
                wx::firesBorn(b0, fireCacheB0 - 1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
                fireCacheB0 = b0;
            }
            if (fireCacheB1 - fireCacheB0 > 45)   // bounded: rebuild for the window
            {
                fireCache.clear();
                wx::firesBorn(b0, b1, fireCacheLat, fireCacheLon, fireCacheR, biomeMap, wildfireRate, fireCache);
                fireCacheB0 = b0;
                fireCacheB1 = b1;
            }
        }
    }
    for (const wx::Fire &f : fireCache)
    {
        if (t < f.t0 || t > f.t0 + f.life) continue;
        const glm::dvec3 g = llDir(f.latDeg, f.lonDeg);
        const double dKm = std::acos(std::clamp(glm::dot(g, up), -1.0, 1.0)) * 6371.0;
        if (dKm > reachKm) continue;
        FireNow fn;
        fn.f = f;
        fn.s = wx::fireStateAt(f, t);
        fn.rangeKm = std::sqrt(dKm * dKm + (alt / 1000.0) * (alt / 1000.0));
        firesNow.push_back(fn);
    }
    std::sort(firesNow.begin(), firesNow.end(), [](const FireNow &a, const FireNow &b) { return a.rangeKm < b.rangeKm; });

    // The Sun (Earth-fixed) for the smoke's light.
    const glm::dvec3 si = sunDirEciAt(t);
    const double gr = earthRotationAngle(t);
    const glm::dvec3 sunE = glm::normalize(glm::dvec3(si.x * std::cos(gr) + si.y * std::sin(gr),
                                                      -si.x * std::sin(gr) + si.y * std::cos(gr), si.z));
    const double kTwoPi15 = 15.749609945722419;
    const uint32_t first = n;
    const uint32_t cap = std::min<uint32_t>(kMaxDustClouds, n + 320u);
    // One Gaussian: centre C (ECEF), orthonormal axes a, b, c with sigmas, Sigma = cross-section, its light.
    auto emit = [&](const glm::dvec3 &C, const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c, double sA,
                    double sB, double sC, double Sigma, const glm::dvec3 &sunL, const glm::dvec3 &skyL, double albedo,
                    uint64_t seed) -> bool {
        if (n >= cap || Sigma < 1.0) return false;
        const glm::dvec3 rel = C - eye;
        const double range = std::max(glm::length(rel), 1.0);
        const glm::dvec3 ae = toEnu(a), be = toEnu(b), ce = toEnu(c);
        const double iA = 1.0 / (sA * sA), iB = 1.0 / (sB * sB), iC = 1.0 / (sC * sC);
        auto Mx = [&](int p, int q) { return iA * ae[p] * ae[q] + iB * be[p] * be[q] + iC * ce[p] * ce[q]; };
        GpuDustCloud &d = out[n++];
        const glm::dvec3 re = toEnu(rel);
        d.c = glm::vec4(glm::vec3(re), (float)Sigma);
        d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2), (float)(1.0 / (kTwoPi15 * sA * sB * sC)));
        const double lS = sunL.x + sunL.y + sunL.z, lI = skyL.x + skyL.y + skyL.z;
        const double share = std::min(lI / std::max(lI + lS, 1e-9), 0.999);
        const double seedK = (double)(fwMix(seed) & 255u);
        d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), (float)(4.0 * seedK + 2.0 + share));
        d.col = glm::vec4(glm::vec3(sunL + skyL), (float)(2.0 + albedo));
        const double R = 5.5 * std::max(sA, std::max(sB, sC));
        d.cone = R >= 0.95 * range ? glm::vec4(0.0f, 0.0f, 0.0f, -2.0f)
                                   : glm::vec4(glm::vec3(re / range), (float)std::sqrt(1.0 - (R / range) * (R / range)));
        return true;
    };

    const double night = 1.0 - glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0);
    GpuFireball *fo = kFireMapped ? reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16) : nullptr;
    int lightsAdded = 0;
    for (FireNow &fn : firesNow)
    {
        const wx::Fire &f = fn.f;
        const wx::FireState &s = fn.s;
        if (s.activity <= 0.0 && s.ageS <= 0.0) continue;
        const glm::dvec3 G = llDir(f.latDeg, f.lonDeg);
        const glm::dvec3 eE = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), G) + glm::dvec3(1e-12, 0, 0));
        const glm::dvec3 nE = glm::cross(G, eE);
        const double ground = std::max(0.0, (double)cpuTerrainHeightM((float)f.latDeg, (float)f.lonDeg));
        auto at = [&](double x, double y, double z) {   // metres east / north of the ignition point, up
            return glm::normalize(G + (eE * x + nE * y) / kReF) * (kReF + ground + z);
        };
        const glm::dvec2 w2(std::sin(s.windAz), std::cos(s.windAz));
        const glm::dvec3 wv = glm::normalize(eE * w2.x + nE * w2.y);
        const glm::dvec3 cv = glm::normalize(glm::cross(G, wv));   // across the wind (left of it)
        const glm::dvec2 head = wx::fireEdgeAt(s, 0.0);
        const double frontKm = 2.4 * s.halfWidthM / 1000.0;          // the active head and flanks, roughly

        // ── Smoke ──────────────────────────────────────────────────────────────────────────────────────────────
        const double sinE = glm::dot(G, sunE);
        const double dayK = glm::smoothstep(-0.06, 0.06, sinE);
        const double airm = 1.0 / (std::max(sinE, 0.0) + 0.05);
        const glm::dvec3 sunCol = glm::exp(-glm::dvec3(0.025, 0.045, 0.11) * airm) * dayK;
        const glm::dvec3 skyCol = 0.2 * glm::dvec3(0.62, 0.78, 1.0) * dayK;
        const double H = s.plumeTopM;
        const double sz = (double)f.size;
        // The source: the head front's width; its optical depth seen side-on near the source.
        const double sB0 = std::max(150.0, 0.3 * s.halfWidthM) + 300.0 * sz;
        const double tau0 = (1.5 + 3.5 * sz) * (0.25 + 0.75 * s.activity) * (0.6 + 0.4 * f.fuel) * wildfireSmoke;
        const double lam = tau0 * std::sqrt(2.0 * kPiF) * sB0;      // cross-section per metre downwind
        const double Lp = std::clamp((25e3 + 90e3 * sz) * (0.5 + s.activity), 8e3, 250e3);
        // The column: rises steeply (a big fire's updraft is stronger: steeper), then bends over and flattens at
        // the injection height within ~xr downwind.
        const double xr = 1500.0 - 600.0 * sz;
        const int K = fn.rangeKm < 150.0 ? 12 : fn.rangeKm < 500.0 ? 6 : fn.rangeKm < 1200.0 ? 3 : 1;
        // Fresh smoke near a flaming front carries black carbon: grey-brown, darker; aged smoke is paler.
        const glm::dvec3 tintC(0.72, 0.58, 0.42), tintP(0.88, 0.80, 0.68);
        const double albC = 0.42, albP = 0.7;
        auto knot = [&](double x, glm::dvec3 &P, double &sb, double &szz) {
            const double z0 = std::min(250.0 + 0.04 * H, 0.3 * H);
            const double z = z0 + (H - z0) * (1.0 - std::exp(-x / xr));
            sb = sB0 + 0.12 * x;
            szz = std::min(sb, 120.0 + 0.22 * H + 0.015 * x);   // spreads sideways, stays thin vertically: it flattens
            P = at(head.x + w2.x * (x - 300.0), head.y + w2.y * (x - 300.0), z);
        };
        int emitted = 0;
        if (tau0 > 0.02 && fn.rangeKm < 2500.0)
        {
            // Knots: the rising column finely (near fires), then a power law out to the plume's end.
            double xs[24];
            int nx = 0;
            xs[nx++] = 0.0;
            if (K >= 6)
                for (double fx : {0.3, 0.8, 1.6, 3.0})
                    xs[nx++] = fx * xr;
            {
                const double xa = xs[nx - 1];
                const int rest = std::max(1, K - (nx - 1));
                for (int j = 1; j <= rest; ++j)
                    xs[nx++] = xa + (Lp - xa) * std::pow((double)j / rest, 1.8);
            }
            glm::dvec3 P0;
            double sb0, sz0;
            knot(0.0, P0, sb0, sz0);
            for (int k = 0; k + 1 < nx; ++k)
            {
                const double x0 = xs[k], x1 = xs[k + 1];
                glm::dvec3 P1;
                double sb1, sz1;
                knot(x1, P1, sb1, sz1);
                const glm::dvec3 seg = P1 - P0;
                const double len = std::max(glm::length(seg), 1.0);
                const glm::dvec3 a = seg / len;
                glm::dvec3 b = glm::cross(a, glm::normalize(P0));
                b = glm::length(b) > 1e-6 ? glm::normalize(b) : cv;
                const glm::dvec3 c = glm::cross(a, b);
                const double sb = 0.5 * (sb0 + sb1), szv = 0.5 * (sz0 + sz1);
                // c is near vertical for a flat segment (thin), near horizontal along the wind for the rising column.
                const double cUp = std::fabs(glm::dot(c, glm::normalize(P0)));
                const double sC = glm::mix(sb, szv, cUp);
                const double Sigma = lam * std::max(x1 - x0, 0.3 * len);
                const glm::dvec3 Cc = 0.5 * (P0 + P1);
                if (emit(Cc, a, b, c, std::max((k == 0 ? 0.3 : 0.5) * len, 0.6 * sC), (k == 0 ? 0.6 : 1.0) * sb, sC, Sigma, 4.0 * kPiF * albC * tintC * sunCol,
                         4.0 * kPiF * albC * tintC * skyCol, albC, f.id ^ (uint64_t)(k + 1)))
                    ++emitted;
                P0 = P1;
                sb0 = sb1;
                sz0 = sz1;
            }
            // The pall: the smoke of the last days, carried downwind and spread wide and flat. MASS-CONSERVING: what the
            // plume carries away (lam x the wind) over the fire's life so far, less ~4 days of removal (deposition,
            // dilution below visibility), at the column's mean activity; spread over the pall's area that is a column
            // of ~0.01-0.05 from above for one big fire, so a fire season's many overlapping palls make the haze.
            {
                const double u = 5.0;
                const double age = std::min(s.ageS, 3.0 * 86400.0);
                const double sh = std::min(20e3 + 0.25 * u * age + 0.5 * Lp, 350e3);
                const double off = 0.45 * u * std::min(s.ageS, 36.0 * 3600.0) + 0.6 * Lp;
                const double zP = 0.8 * H + 200.0;
                const double kRem = 4.0 * 86400.0;
                const double lamMean = lam / std::max(0.25 + 0.75 * s.activity, 0.25) * 0.6;   // (the mean activity ~0.6)
                const double Sigma = lamMean * u * kRem * (1.0 - std::exp(-std::max(s.ageS - Lp / u, 0.0) / kRem));
                const double sAlong = 1.4 * sh;
                const double tauTop = Sigma / (2.0 * kPiF * sAlong * sh);
                const glm::dvec3 Cp = at(head.x + w2.x * off, head.y + w2.y * off, zP);
                const glm::dvec3 upP = glm::normalize(Cp);
                const glm::dvec3 ap = glm::normalize(wv - upP * glm::dot(wv, upP));
                const glm::dvec3 bp = glm::cross(upP, ap);
                if (tauTop > 0.002 && emit(Cp, ap, bp, upP, sAlong, sh, 0.35 * H + 400.0, Sigma,
                                          4.0 * kPiF * albP * tintP * sunCol, 4.0 * kPiF * albP * tintP * skyCol, albP,
                                          f.id ^ 0xFA11ull))
                    ++emitted;
            }
        }
        fn.gauss = emitted;

        // ── The front: point sprites, and the glow (engine light) of the nearest ────────────────────────────────
        const double lightK = (night + 0.06 * (1.0 - night)) * wildfireLight * s.activity;
        if (fo && lightK > 0.002 && fn.rangeKm < 400.0)
        {
            double perim = 0.0;
            glm::dvec2 prev = wx::fireEdgeAt(s, -0.62 * kPiF);
            for (int k = 1; k <= 32; ++k)
            {
                const glm::dvec2 q = wx::fireEdgeAt(s, -0.62 * kPiF + 1.24 * kPiF * k / 32.0);
                perim += glm::length(q - prev);
                prev = q;
            }
            const double spacing = std::max(120.0, fn.rangeKm * 1000.0 * 0.004);
            const int np = std::clamp((int)std::lround(perim / spacing), 1, 48);
            const double seg = perim / np;
            for (int k = 0; k < np && kFireCount < kFireballMax; ++k)
            {
                const double a = -0.62 * kPiF + 1.24 * kPiF * (k + 0.5) / np;
                const glm::dvec2 q = wx::fireEdgeAt(s, a);
                const double I = wx::fireFrontIntensity(a);
                const glm::dvec3 P = at(q.x, q.y, 15.0);
                const glm::dvec3 rel = P - eye;
                const double range = std::max(glm::length(rel), 5.0);
                {
                    const glm::dvec3 uu = rel / range;
                    const double tc = -glm::dot(eye, uu);
                    if (tc > 0.0 && tc < range && glm::length(eye + tc * uu) < kReF - 20.0) continue;   // over the horizon
                }
                const uint64_t h = fwMix(f.id ^ (uint64_t)(k * 7919 + 13));
                const double flick = 0.65 + 0.35 * std::sin(t * (6.0 + 5.0 * fwU(h)) + 6.2832 * fwU(h ^ 1)) *
                                                std::sin(t * (2.1 + 1.7 * fwU(h ^ 2)) + 1.3);
                // ~-9 at 1 km for 50 m of a full head (a flame wall's luminance x its area, on the scale the pads' and
                // the ships' lights are drawn on: ~7 magnitudes over a floodlight).
                const double m1 = -9.0 - 2.5 * std::log10(std::max(seg / 50.0 * I * lightK * flick, 1e-6));
                const double m = m1 + 5.0 * std::log10(range / 1000.0) +
                                 atmExtinctionMag(eye, rel / range, range, (double)extinctionCoeff);
                const double flare = std::min(0.008 * std::pow(10.0, 0.4 * (6.0 - m)), 14.0);
                if (flare < 2e-4) continue;
                const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.36f, 0.08f), glm::vec3(1.0f, 0.62f, 0.24f), (float)I);
                fo[kFireCount++] = {glm::vec4(glm::vec3(toEnu(rel) / range), (float)flare), glm::vec4(col, (float)range)};
                ++fn.points;
            }
            if (lightsAdded < 6 && fn.rangeKm < 200.0 && night > 0.05)
            {
                const glm::dvec3 Pl = at(head.x, head.y, 250.0);
                const double Il = std::min(40.0 * s.activity * std::max(frontKm, 0.2) * wildfireLight, 3000.0) * night;
                engineLightAdd(fwEcefToEci(Pl, gr), Il, glm::vec3(1.0f, 0.5f, 0.18f), std::max(200.0, 0.25 * frontKm * 1000.0),
                               glm::dvec3(0.0), 0.0, 0.0, false, 0.0);
                ++lightsAdded;
            }
        }
    }
    const uint32_t cnt = n - first;
    if (cnt > 0) smokeHdrBits = (first << 8u) | (cnt << 18u);
}

nlohmann::json SatelliteSim::wildfiresJson(bool world)
{
    nlohmann::json o;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    o["rate"] = wildfireRate;
    o["smoke_gain"] = wildfireSmoke;
    o["light_gain"] = wildfireLight;
    o["near_count"] = firesNow.size();
    o["smoke_range"] = {smokeHdrBits >> 8u & 1023u, smokeHdrBits >> 18u & 1023u};
    nlohmann::json a = nlohmann::json::array();
    for (size_t i = 0; i < firesNow.size() && i < 12; ++i)
    {
        const FireNow &f = firesNow[i];
        nlohmann::json r;
        r["id"] = f.f.id;
        r["lat"] = f.f.latDeg;
        r["lon"] = f.f.lonDeg;
        r["biome"] = wx::biomeName(biomeMap.at(f.f.latDeg, f.f.lonDeg));
        r["range_km"] = f.rangeKm;
        r["age_h"] = f.s.ageS / 3600.0;
        r["life_d"] = f.f.life / 86400.0;
        r["size"] = f.f.size;
        r["pyro"] = f.f.pyro > 0.5f;
        r["activity"] = f.s.activity;
        r["head_km"] = f.s.headM / 1000.0;
        r["plume_top_m"] = f.s.plumeTopM;
        r["gaussians"] = f.gauss;
        r["points"] = f.points;
        a.push_back(r);
    }
    o["near"] = a;
    if (world)
    {
        std::vector<wx::Fire> all;
        wx::firesWorld(t, biomeMap, wildfireRate, all);
        o["world_count"] = all.size();
        int big = 0;
        std::array<int, (int)wx::Biome::Count> byBiome{};
        for (const wx::Fire &f : all)
        {
            big += f.size > 0.5f;
            byBiome[(int)biomeMap.at(f.latDeg, f.lonDeg)]++;
        }
        o["world_big"] = big;
        nlohmann::json b;
        for (int k = 0; k < (int)wx::Biome::Count; ++k)
            if (byBiome[k]) b[wx::biomeName((wx::Biome)k)] = byBiome[k];
        o["world_by_biome"] = b;
    }
    return o;
}
