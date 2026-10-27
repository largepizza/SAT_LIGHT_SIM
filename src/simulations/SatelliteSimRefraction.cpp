// Refraction, mirages and optical turbulence at the eye (2026-10-26, issue 20261026_203414 "Sun refraction mk 2").
//
// Each frame the sim knows the air's profile at the observer (refrProfileAt: a pure function of place and sim time —
// the sea's or ground's temperature step under the air, an elevated inversion some days, the turbulence they drive)
// and a table built from it by refr::build on a worker (Refraction.h): for 512 apparent elevations around the
// horizon, the true direction per colour channel, the air columns along the BENT path, the angle-of-arrival jitter
// and the scintillation. sat_sky.frag draws the Sun's disc through it (the cut-off "flat bottom" of the old
// Saemundsson / tangent-height pair is gone, the inferior mirage's Etruscan-vase and Omega shapes, the mock mirage's
// slices and the green rim / flash come out of the same tracer), updateStars() places and twinkles the stars with it.
#include "SatelliteSim.h"
#include "../FrameProfile.h"
#include "Log.h"
#include <cstdio>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <nlohmann/json.hpp>

namespace
{
double rHash(int64_t a, int64_t b, int64_t c, uint32_t seed)
{
    uint64_t h = (uint64_t)a * 0x9E3779B97F4A7C15ull ^ (uint64_t)b * 0xC2B2AE3D27D4EB4Full ^ (uint64_t)c * 0x165667B19E3779F9ull ^ seed;
    h ^= h >> 33; h *= 0xFF51AFD7ED558CCDull; h ^= h >> 33; h *= 0xC4CEB9FE1A85EC53ull; h ^= h >> 33;
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}
// Smooth value noise in [0, 1] over (x, y, z) cells: no pops at cell or day boundaries.
double rNoise(double x, double y, double z, uint32_t seed)
{
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    double ux = x - fx, uy = y - fy, uz = z - fz;
    ux = ux * ux * (3 - 2 * ux); uy = uy * uy * (3 - 2 * uy); uz = uz * uz * (3 - 2 * uz);
    const int64_t ix = (int64_t)fx, iy = (int64_t)fy, iz = (int64_t)fz;
    double v = 0.0;
    for (int k = 0; k < 8; ++k)
    {
        const int dx = k & 1, dy = (k >> 1) & 1, dz = (k >> 2) & 1;
        const double w = (dx ? ux : 1 - ux) * (dy ? uy : 1 - uy) * (dz ? uz : 1 - uz);
        v += w * rHash(ix + dx, iy + dy, iz + dz, seed);
    }
    return v;
}
double smooth01(double a, double b, double x)
{
    const double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3 - 2 * t);
}
constexpr double kTwoPi = 6.283185307179586;
constexpr double kSunDrawnR = 0.007;   // the Sun as drawn (sat_sky.frag r0, 1.5x the true radius)
} // namespace

refr::Profile SatelliteSim::refrProfileAt(double simT) const
{
    refr::Profile p;
    const double lat = obsLatDeg, lon = obsLonDeg;
    const bool sea = oceanAt(glm::radians(lat), glm::radians(lon));
    p.surfaceZ = sea ? 0.0 : std::max((double)obsGroundM(), 0.0);
    // T0 is the air zRef above the SURFACE: the eye temperature's sea-level climate (SatelliteSimCloudsV2.cpp, the
    // rain block's tC: keep in step) less 6.5 C/km to the surface. It was 273.15 + cv2EyeTempC, the air at the EYE:
    // from ~33 km up T0 fell below ~70 K, the profile's standard atmosphere passed 0 K above it, every table entry
    // was NaN and refr::lookup indexed the table with (int)NaN (the 2026-10-26 crashes climbing to orbit).
    const double latR = glm::radians(lat);
    const double sunDec = std::asin(std::clamp((double)glm::normalize(sunDirECI).z, -1.0, 1.0));
    const double season = (sunDec / glm::radians(23.44)) * (lat >= 0.0 ? 1.0 : -1.0);
    const double tSurfC = -18.0 + 45.0 * std::pow(std::cos(latR), 1.5) + std::min(0.25 * std::abs(lat), 15.0) * season
                        - 0.0065 * (p.surfaceZ + p.zRef);
    p.T0 = std::clamp(273.15 + tSurfC, 200.0, 330.0);
    // The surface layer. Over the sea the water is usually warmer than the air above it at dusk (0.4-2 K in the
    // subtropics: the Omega / Etruscan-vase sunsets), now and then colder (warm air advected over a cold current:
    // a looming, superior-mirage sea). Over land the ground heats by day and cools after sunset.
    const double day = simT / 43200.0;
    const double n1 = rNoise(lat / 3.0, lon / 3.0, day, 11u), n2 = rNoise(lat / 3.0, lon / 3.0, day, 12u);
    double dTs;
    if (sea)
    {
        dTs = 0.2 + 1.4 * n1;
        if (n2 < 0.2) dTs = glm::mix(dTs, -(0.3 + 1.5 * n1), (float)smooth01(0.2, 0.1, n2));
    }
    else
    {
        const double sEl = sunDirENU.w;
        dTs = 6.0 * std::sqrt(std::clamp(sEl, 0.0, 1.0)) - 2.0 * (1.0 - smooth01(-0.1, 0.15, sEl));
    }
    // It breathes: the convective cells over the water come and go over tens of seconds.
    const double ph = kTwoPi * rHash((int64_t)std::floor(lat), (int64_t)std::floor(lon), 0, 13u);
    dTs *= 1.0 + 0.25 * std::sin(kTwoPi * simT / 83.0 + ph) + 0.12 * std::sin(kTwoPi * simT / 29.0 + 2.3 * ph);
    // An elevated inversion (the marine layer's lid, a warm front aloft) on some days: ducting, mock-mirage slices,
    // the green flash it magnifies. Its height rides gravity waves of a few minutes.
    const double n3 = rNoise(lat / 5.0, lon / 5.0, simT / 21600.0, 21u);
    const double n4 = rNoise(lat / 5.0, lon / 5.0, simT / 21600.0, 22u), n5 = rNoise(lat / 5.0, lon / 5.0, simT / 21600.0, 23u);
    const double pInv = sea ? 0.45 : 0.3;
    const double invK = smooth01(pInv, pInv - 0.1, n3);
    p.invZ = 3.0 * std::pow(130.0, n4);   // 3-400 m, median ~35 m: below a raised eye too (the mock mirage's slices)
    p.invDz = 5.0 + 30.0 * n5;
    p.invDT = invK * (0.5 + 3.5 * n5 * n4);
    p.invZ *= 1.0 + 0.06 * std::sin(kTwoPi * simT / 170.0 + 1.7 * ph) + 0.03 * std::sin(kTwoPi * simT / 53.0 + 0.6 * ph);
    if (refrForceDt > -50.0f) dTs = refrForceDt;
    if (refrForceInvZ >= 0.0f)
    {
        p.invZ = refrForceInvZ;
        p.invDT = refrForceInvZ > 0.0f ? refrForceInvDT : 0.0;
        p.invDz = refrForceInvDz;
    }
    p.dTs = dTs * refrMirage;
    p.invDT *= refrMirage;
    // Turbulence: the surface layer's Cn^2 grows with the temperature step (convection); the free troposphere is
    // Hufnagel-Valley's.
    p.cnSurf = (2e-15 + 6e-15 * p.dTs * p.dTs) * refrTurbulence;
    p.cnFree = refrTurbulence;
    p.dispScale = refrDispersion;
    return p;
}

void SatelliteSim::updateRefraction()
{
    FPROF_SCOPE("refraction");
    const double simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double h0 = (double)obsEyeRadiusM(obsHeightOffset) - (double)satphot::kEarthRadiusM;
    bool took = false;
    if (refrJob.valid() && refrJob.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
    {
        refrTable = refrJob.get();
        refrProfileUsed = refrJobProfile;
        took = true;
    }
    const auto now = std::chrono::steady_clock::now();
    const double since = std::chrono::duration<double>(now - refrLastStart).count();
    const double r0 = satphot::kEarthRadiusM + std::max(h0, 0.0);
    const double eLimb = -std::acos(std::min(satphot::kEarthRadiusM / r0, 1.0));
    const double eSun = std::asin(std::clamp((double)sunDirENU.w, -1.0, 1.0));
    const bool sunLow = std::abs(eSun - eLimb) < 0.08;
    const double dh = std::abs(h0 - refrTable.h0);
    const bool moved = dh > 0.02 * std::max(h0, 0.0) + 0.3;
    const double interval = sunLow ? 0.1 : 1.0;
    const int threads = std::max(1, (int)std::thread::hardware_concurrency() / 2);
    if (!refrTable.valid)
    {
        // The first table on this frame (a start, a load): no frame without one.
        const auto t0 = std::chrono::steady_clock::now();
        refrJobProfile = refrProfileAt(simT);
        refr::build(refrJobProfile, h0, refrTable, threads);
        refrProfileUsed = refrJobProfile;
        refrBuildMs = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
        ++refrBuilds;
        refrLastStart = now;
        took = true;
    }
    else if (!refrJob.valid() && (since > interval || moved))
    {
        refrJobProfile = refrProfileAt(simT);
        refrLastStart = now;
        const refr::Profile pr = refrJobProfile;
        refrJob = std::async(std::launch::async, [pr, h0, threads, this] {
            const auto t0 = std::chrono::steady_clock::now();
            refr::Table t;
            refr::build(pr, h0, t, threads);
            refrBuildMs = (float)std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
            ++refrBuilds;
            return t;
        });
    }
    (void)took;
    refrDerive();
    fillSunspots(simT);
}

// Sunspots (2026-10-27, issue 20261027_000948 "Sunspot look": "only circular ... we need pinprick dotted dark spots,
// umbras and penumbras only on large spots or clusters"). Active regions as BIPOLAR GROUPS on the solar sphere, a
// pure function of sim time (time warp, reversal and bookmarks see the same Sun):
//   - kSlots slots, each a sequence of groups: slot k's cycle C_k (24-64 days), the group of cycle c exists with a
//     chance following the 11-year cycle's activity (spaceWeather_.cycle), lives 4-60 days (bigger = longer),
//     grows over its first ~12% and decays over its second half;
//   - its size is heavy-tailed (most groups are a few pores, a few are large, complex regions), its latitude 6-32
//     degrees in either hemisphere, its tilt Joy's law (leader nearer the equator, ~half the latitude), and it rides
//     the Sun's differential rotation (synodic rate 14.71 - 2.40 sin^2 - 1.79 sin^4 deg/day less the Earth's 0.986);
//   - the Sun's Fury's flare region (2037-07-14 09:30 UTC): a large delta group crossing the central meridian at
//     the flare, on the disc for the two weeks round it (the user: "we'd expect a large cluster in the centre").
// The shader (sat_sky.frag sunspotFactor) draws each as a leader, followers and pores; the 16 largest on the visible
// side are sent.
void SatelliteSim::fillSunspots(double simT)
{
    GpuRefraction &g = refrGpu;
    constexpr double kDay = 86400.0;
    constexpr int kSlots = 48, kMax = 16;
    const double act = std::clamp(spaceWeather_.cycle, 0.0, 1.0);
    auto omegaSyn = [](double latR) {
        const double s2 = std::sin(latR) * std::sin(latR);
        return glm::radians(14.713 - 2.396 * s2 - 1.787 * s2 * s2 - 0.9856) / kDay;
    };
    auto wrapPi = [](double a) { return a - kTwoPi * std::floor((a + 0.5 * kTwoPi) / kTwoPi); };
    struct G { glm::vec4 a, b; double rank; };
    G gs[kSlots + 1];
    int n = 0;
    for (int k = 0; k < kSlots; ++k)
    {
        const double C = (24.0 + 40.0 * rHash(k, 0, 0, 301u)) * kDay;
        const double x = simT / C + rHash(k, 1, 0, 301u);
        const int64_t c = (int64_t)std::floor(x);
        const double tAge = (x - (double)c) * C;
        if (rHash(k, c, 2, 301u) > 0.2 + 0.8 * act) continue;
        const double sz = std::pow(rHash(k, c, 3, 301u), 2.4);
        const double life = std::min((4.0 + 56.0 * sz) * kDay, 0.9 * C);
        if (tAge > life) continue;
        const double a01 = tAge / life;
        const double env = smooth01(0.0, 0.12, a01) * (1.0 - smooth01(0.5, 1.0, a01));
        if (env < 0.03) continue;
        const double latD = 6.0 + 26.0 * std::pow(rHash(k, c, 5, 301u), 1.3);
        const double lat = glm::radians(latD) * (rHash(k, c, 4, 301u) < 0.5 ? -1.0 : 1.0);
        const double lonCM = wrapPi(kTwoPi * rHash(k, c, 6, 301u) + omegaSyn(lat) * tAge);
        if (std::abs(lonCM) > glm::radians(100.0)) continue;
        const double E = 0.02 + 0.13 * sz;
        const double tilt = glm::radians(0.5 * latD + 14.0 * (rHash(k, c, 7, 301u) - 0.5));
        const double cx = sz > 0.45 ? rHash(k, c, 8, 301u) : 0.0;
        const uint32_t seed = (uint32_t)(k * 4099 + (int)(((c % 4093) + 4093) % 4093) + 1);
        gs[n++] = {glm::vec4((float)lat, (float)lonCM, (float)E, (float)seed),
                   glm::vec4((float)tilt, (float)env, (float)cx, (float)sz), sz * env};
    }
    {
        const double dt = simT - (kesslerEpoch() - 17.5 * 3600.0);   // the Sun's Fury's X-class flare
        if (dt > -16.0 * kDay && dt < 20.0 * kDay)
        {
            const double lat = glm::radians(14.0);
            const double lonCM = omegaSyn(lat) * dt;
            const double env = smooth01(-16.0 * kDay, -9.0 * kDay, dt) * (1.0 - smooth01(9.0 * kDay, 20.0 * kDay, dt));
            if (std::abs(lonCM) < glm::radians(100.0) && env > 0.03)
                gs[n++] = {glm::vec4((float)lat, (float)lonCM, 0.24f, 7777.0f), glm::vec4(glm::radians(9.0f), (float)env, 1.0f, 1.0f), 10.0};
        }
    }
    std::sort(gs, gs + n, [](const G &p, const G &q) { return p.rank > q.rank; });
    n = std::min(n, kMax);
    g.spotInfo = glm::vec4((float)n, (float)act, 0.0f, 0.0f);
    for (int i = 0; i < n; ++i)
    {
        g.spots[2 * i] = gs[i].a;
        g.spots[2 * i + 1] = gs[i].b;
    }
    sunspotGroups = n;
}

void SatelliteSim::refrDerive()
{
    const refr::Table &t = refrTable;
    GpuRefraction &g = refrGpu;
    if (!t.valid)
    {
        g.info = glm::vec4(0.0f);
        return;
    }
    {
        int bad = 0, firstBad = -1;
        for (int i = 0; i < refr::kTableN; ++i)
        {
            const refr::Entry &e = t.e[i];
            if (!std::isfinite(e.dR[0]) || !std::isfinite(e.dR[1]) || !std::isfinite(e.dR[2]) || !std::isfinite(e.colR))
            {
                if (firstBad < 0) firstBad = i;
                ++bad;
            }
        }
        static int logged = 0;
        if ((bad || !std::isfinite(t.c) || !std::isfinite(t.alpha) || !std::isfinite(t.eC)) && logged < 3)
        {
            ++logged;
            const refr::Profile &p = refrProfileUsed;
            char buf[512];
            std::snprintf(buf, sizeof buf,
                          "refraction: table has %d non-finite entries (first %d), map eC %g c %g alpha %g u0 %g, h0 %g; profile "
                          "surfaceZ %g T0 %g P0 %g dTs %g invZ %g invDz %g invDT %g cnSurf %g",
                          bad, firstBad, t.eC, t.c, t.alpha, t.u0, t.h0, p.surfaceZ, p.T0, p.P0, p.dTs, p.invZ, p.invDz,
                          p.invDT, p.cnSurf);
            Log::line(buf);
        }
    }
    // The primary image: from the top down while the true elevation keeps falling.
    refrInv.clear();
    double minT = 1e9;
    for (int i = refr::kTableN - 1; i >= 0; --i)
    {
        const refr::Entry &e = t.e[i];
        if (e.dR[1] >= refr::kHit) break;
        const double eA = t.eAt(i), eT = eA - e.dR[1];
        if (eT >= minT) break;
        minT = eT;
        refrInv.push_back(glm::dvec2(eT, eA));
    }
    std::reverse(refrInv.begin(), refrInv.end());
    // The Sun: its centre's refraction (the primary image) and the share of its disc the eye sees through every
    // image (an inferior mirage adds the inverted one; ducting removes stripes).
    const double eS = std::asin(std::clamp((double)sunDirENU.w, -1.0, 1.0));
    const double r = kSunDrawnR;
    double area = 0.0;
    const double eTop = t.eAt(refr::kTableN - 1);
    // The air columns the Sun's VISIBLE disc is seen through (2026-10-27, issue 20261027_001448 shots 2-3: "at a certain
    // stage in the sunset the Sun suddenly gets brighter"). The glare and corona follow the light that arrives, and took
    // the columns of the ray through the disc's CENTRE: while that ray grazed the sea it crossed hundreds of km of air
    // (no glare at all over a bright half-set disc), and the frame its apparent centre went below the horizon the
    // lookup gave the short column of a ray ending on the sea, and the whole veil came on at once (the frame's mean
    // 0.23 -> 0.73 between two 4-s steps). Now: the mean over the visible rows, weighted by chord x transmittance, then
    // scaled so its transmittance (green, base extinction) is the rows' mean transmittance. Continuous as rows set.
    double wSum = 0.0, tSum = 0.0, cAcc[4] = {0.0, 0.0, 0.0, 0.0};
    auto tauRef = [](double cR, double cM, double cO, double cH) {   // green, common.glsl's base extinction
        return 13.5e-6 * cR + 2.1e-5 * 1.1 * cM + 0.027 * cO + 0.0 * cH;
    };
    for (int i = 0; i < refr::kTableN; ++i)
    {
        const refr::Entry &e = t.e[i];
        if (e.dR[1] >= refr::kHit) continue;
        const double eA = t.eAt(i), x = eA - e.dR[1] - eS;
        if (std::abs(x) >= r) continue;
        const double de = 0.5 * (t.eAt(std::min(i + 1, refr::kTableN - 1)) - t.eAt(std::max(i - 1, 0)));
        const double ch = 2.0 * std::sqrt(r * r - x * x) * de;
        area += ch;
        const double tr = std::exp(-tauRef(e.colR, e.colM, e.colO3, e.colH));
        wSum += ch;
        tSum += ch * tr;
        cAcc[0] += ch * tr * e.colR; cAcc[1] += ch * tr * e.colM; cAcc[2] += ch * tr * e.colO3; cAcc[3] += ch * tr * e.colH;
    }
    // the part of the disc above the table (no mirages there)
    if (eS + r > eTop)
    {
        const double lo = std::max(eS - r, eTop) - eS;
        // integral of 2 sqrt(r^2 - x^2) from lo to r
        auto F = [&](double x) { return x * std::sqrt(std::max(r * r - x * x, 0.0)) + r * r * std::asin(std::clamp(x / r, -1.0, 1.0)); };
        area += F(r) - F(lo);
    }
    refrSunVis = area / (glm::pi<double>() * r * r);
    double eApp = eS;
    if (!refrInv.empty() && eS > refrInv.back().x)
        eApp = eS + t.topScale * refr::saemundsson(eS);
    else if (!refrInv.empty() && eS >= refrInv.front().x)
    {
        auto it = std::lower_bound(refrInv.begin(), refrInv.end(), eS, [](const glm::dvec2 &a, double v) { return a.x < v; });
        const glm::dvec2 b = *it, a = (it == refrInv.begin()) ? b : *(it - 1);
        const double w = (b.x > a.x) ? (eS - a.x) / (b.x - a.x) : 0.0;
        eApp = a.y + (b.y - a.y) * w;
    }
    else if (!refrInv.empty())
        eApp = eS + (refrInv.front().y - refrInv.front().x);
    if (!std::isfinite(eApp) || !std::isfinite(refrSunVis))
    {
        static int loggedSun = 0;
        if (loggedSun++ < 3)
        {
            char buf[384];
            std::snprintf(buf, sizeof buf, "refraction: non-finite Sun (eS %g eApp %g vis %g sunDirENU %g %g %g %g, inv %zu, obsDir %g %g %g, h %g)",
                          eS, eApp, refrSunVis, sunDirENU.x, sunDirENU.y, sunDirENU.z, sunDirENU.w, refrInv.size(),
                          obsDir.x, obsDir.y, obsDir.z, obsHeightOffset);
            Log::line(buf);
        }
        eApp = std::isfinite(eS) ? eS : 0.0;
        if (!std::isfinite(refrSunVis)) refrSunVis = 0.0;
    }
    refrSunR = eApp - eS;
    refr::Entry ce;
    double dRg;
    refr::lookup(t, eApp, dRg, &ce);

    g.map = glm::vec4((float)t.eC, (float)t.c, (float)t.alpha, (float)t.u0);
    g.info = glm::vec4((float)refr::kTableN, (float)t.topScale, refrEnabled ? 1.0f : 0.0f, sunDiscGain);
    g.sun = glm::vec4(refrEnabled ? (float)refrSunR : 0.0f, (float)refrSunVis, refrTurbulence, ce.aoa);
    if (eS + r > eTop)
        g.sunCol = glm::vec4(-1.0f);   // (part of) the disc above the table: the shader's exact Sun path (the top entry's
                                       // columns, ~4 deg up, used to tint and dim the glare of every higher Sun)
    else if (tSum > 1e-30 && wSum > 0.0)
    {
        const double m[4] = {cAcc[0] / tSum, cAcc[1] / tSum, cAcc[2] / tSum, cAcc[3] / tSum};
        const double tm = tauRef(m[0], m[1], m[2], m[3]);
        const double want = -std::log(std::max(tSum / wSum, 1e-300));
        const double s = tm > 1e-12 ? want / tm : 1.0;
        g.sunCol = glm::vec4((float)(s * m[0]), (float)(s * m[1]), (float)(s * m[2]), (float)(s * m[3]));
    }
    else
        g.sunCol = glm::vec4(ce.colR, ce.colM, ce.colO3, ce.colH);
    for (int i = 0; i < refr::kTableN; ++i)
    {
        const refr::Entry &e = t.e[i];
        g.e[i].a = glm::vec4(e.dR[0], e.dR[1], e.dR[2], e.aoa);
        g.e[i].b = glm::vec4(e.colR, e.colM, e.colO3, e.colH);
    }
}

bool SatelliteSim::refractEnu(glm::vec3 &enu, float &aoa, float &scint) const
{
    aoa = 0.0f;
    scint = 0.0f;
    const refr::Table &t = refrTable;
    if (!t.valid || refrInv.empty())
        return true;
    const double eT = std::asin(std::clamp((double)enu.z, -1.0, 1.0));
    double eA = eT;
    if (refrEnabled)
    {
        if (eT > refrInv.back().x)
            eA = eT + t.topScale * refr::saemundsson(eT);
        else if (eT >= refrInv.front().x)
        {
            auto it = std::lower_bound(refrInv.begin(), refrInv.end(), eT, [](const glm::dvec2 &a, double v) { return a.x < v; });
            const glm::dvec2 b = *it, a = (it == refrInv.begin()) ? b : *(it - 1);
            const double w = (b.x > a.x) ? (eT - a.x) / (b.x - a.x) : 0.0;
            eA = a.y + (b.y - a.y) * w;
        }
        else
            return false;   // below the apparent horizon
    }
    refr::Entry e;
    double dRg;
    if (refr::lookup(t, eA, dRg, &e) && !refrEnabled)
        return true;
    aoa = e.aoa;
    scint = e.scint;
    if (refrEnabled && eA != eT)
    {
        glm::vec2 h(enu.x, enu.y);
        const float hl = glm::length(h);
        if (hl > 1e-9f)
            h /= hl;
        enu = glm::vec3(h * (float)std::cos(eA), (float)std::sin(eA));
    }
    return true;
}

nlohmann::json SatelliteSim::refractionJson() const
{
    nlohmann::json j;
    const refr::Profile &p = refrProfileUsed;
    j["enabled"] = refrEnabled;
    j["profile"] = {{"surface_z_m", p.surfaceZ}, {"T0_K", p.T0}, {"sea_air_dT_K", p.dTs}, {"inversion_m", p.invZ},
                    {"inversion_dz_m", p.invDz}, {"inversion_dT_K", p.invDT}, {"cn2_surface_1m", p.cnSurf},
                    {"cn_free_scale", p.cnFree}, {"dispersion", p.dispScale}};
    const refr::Table &t = refrTable;
    j["valid"] = t.valid;
    j["h0_m"] = t.h0;
    j["builds"] = refrBuilds.load();
    j["build_ms"] = refrBuildMs.load();
    if (!t.valid)
        return j;
    // the apparent horizon: the lowest apparent elevation whose ray escapes, scanning up
    double eHor = 0.0;
    int nHit = 0;
    for (int i = 0; i < refr::kTableN; ++i)
        if (t.e[i].dR[1] >= refr::kHit) ++nHit;
    for (int i = 0; i < refr::kTableN; ++i)
        if (t.e[i].dR[1] < refr::kHit) { eHor = t.eAt(i); break; }
    j["apparent_horizon_deg"] = glm::degrees(eHor);
    j["geometric_dip_deg"] = glm::degrees(-std::acos(std::min(satphot::kEarthRadiusM / (satphot::kEarthRadiusM + std::max(t.h0, 0.0)), 1.0)));
    j["hit_entries"] = nHit;
    j["primary_true_lo_deg"] = refrInv.empty() ? 0.0 : glm::degrees(refrInv.front().x);
    j["sun_true_el_deg"] = glm::degrees(std::asin(std::clamp((double)sunDirENU.w, -1.0, 1.0)));
    j["sun_refraction_arcmin"] = refrSunR * 3437.75;
    j["sun_visible_share"] = refrSunVis;
    {
        nlohmann::json sg = nlohmann::json::array();
        for (int i = 0; i < sunspotGroups; ++i)
        {
            const glm::vec4 a = refrGpu.spots[2 * i], b = refrGpu.spots[2 * i + 1];
            sg.push_back({{"lat_deg", glm::degrees(a.x)}, {"lon_cm_deg", glm::degrees(a.y)}, {"half_len", a.z},
                          {"env", b.y}, {"complex", b.z}, {"size", b.w}});
        }
        j["sunspot_groups"] = sg;
        j["solar_cycle"] = refrGpu.spotInfo.y;
    }
    auto at = [&](double eDeg) {
        refr::Entry e;
        double dRg;
        const bool hit = refr::lookup(t, glm::radians(eDeg), dRg, &e);
        return nlohmann::json{{"e_app_deg", eDeg}, {"hit", hit}, {"R_arcmin", {e.dR[0] * 3437.75, e.dR[1] * 3437.75, e.dR[2] * 3437.75}},
                              {"aoa_arcsec", e.aoa * 206265.0}, {"scint", e.scint}, {"colR_km", e.colR * 1e-3}, {"colM_km", e.colM * 1e-3}};
    };
    j["samples"] = {at(-0.2), at(-0.1), at(-0.05), at(0.0), at(0.05), at(0.1), at(0.2), at(0.5), at(1.0), at(3.0), at(10.0), at(45.0)};
    // the mapping app -> true near the horizon, every 8th entry from the bottom (for plots)
    nlohmann::json m = nlohmann::json::array();
    for (int i = 0; i < refr::kTableN; i += 4)
    {
        const refr::Entry &e = t.e[i];
        const double eA = t.eAt(i);
        if (eA > glm::radians(1.5)) break;
        m.push_back({glm::degrees(eA), e.dR[1] >= refr::kHit ? nullptr : nlohmann::json(glm::degrees(eA - e.dR[1])),
                     e.dR[0] >= refr::kHit ? nullptr : nlohmann::json((e.dR[0] - e.dR[2]) * 206265.0)});
    }
    j["map_app_true_disp_arcsec"] = m;
    return j;
}
