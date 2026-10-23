// SatelliteSimHaboob.cpp — haboobs (2026-10-23, .plans/WEATHER_HAZARDS_PLAN.md section 2).
//
// A haboob is a storm's gust front lifting dust: the cold outflow spreads ahead of a squall line / MCS (and of strong
// multicells and supercells) and where it crosses bare, dry ground (the biome map's DUST field: the Sahara, the Sahel,
// Arabia, the Thar, the Gobi, the SW US, inland Australia) it raises a wall of dust 1-2.5 km tall, dense inside
// (visibility of tens of metres), billowing on its face, with a thinning pall behind it.
// Drawn from this frame's storm towers (map frame, wx::Catalog), as plume Gaussians in the dust list just before the
// wildfire smoke, in the same "aerosol" range (smoke.glsl: no sky light given back, extinction spectral: the Sun through a
// haboob goes orange, the ground under it is shadowed): along the front, one wall Gaussian per arc spacing ahead of each
// dominant / line tower (pushed further out as the storm ages), and a lower, wider pall behind every few. Below ~3 km
// cloud_march.comp gives them the shared Earth-fixed billow field (the cauliflower face of the wall).
#include "SatelliteSim.h"
#include "FrameProfile.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>

namespace {
constexpr double kReH = 6371000.0;
constexpr double kPiH = 3.14159265358979323846;
uint64_t hbMix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
} // namespace

// The gust front points of one storm now (Earth-fixed unit directions, the front's local tangent and outward
// direction, and the local strength 0..1). Empty when the storm has no mature tower.
void SatelliteSim::haboobFrontOf(const wx::Storm &S, double t, std::vector<HaboobPoint> &pts) const
{
    pts.clear();
    const double ph = cloudDriftPhase(), cph = std::cos(ph), sph = std::sin(ph);
    auto toE = [&](const glm::dvec3 &m) { return glm::dvec3(m.x * cph + m.y * sph, -m.x * sph + m.y * cph, m.z); };
    const double sp = glm::length(S.vel);
    const glm::dvec3 nAx = sp > 1e-9 ? glm::normalize(glm::cross(S.pos0, S.vel)) : glm::dvec3(0, 0, 1);
    const double age = t - S.t0;
    const bool line = wx::isLineSystem(S.kind);
    // The outflow leads the towers by a few km once the storm has rained a while, more for the organised systems.
    const double grow = glm::smoothstep(1800.0, 3.0 * 3600.0, age);
    const double dG = line ? 5000.0 + 14000.0 * grow : 3000.0 + 7000.0 * grow;
    struct TP { glm::dvec3 d; double s; double along; };
    std::vector<TP> tw;
    for (const wx::Tower &T : stormTowers)
        if (T.stormId == S.id && (line || T.role >= 0.99f) && T.strength > 0.25f)
            tw.push_back({T.dir, (double)T.strength, 0.0});
    if (tw.empty()) return;
    const glm::dvec3 c = glm::normalize(S.centreAt(t));
    glm::dvec3 mv = glm::normalize(glm::cross(nAx, c));            // the motion at the centre (map frame)
    if (sp < 0.5) mv = glm::normalize(glm::dvec3(S.shear));
    const glm::dvec3 acr = glm::normalize(glm::cross(c, mv));      // across the motion
    for (TP &p : tw) p.along = glm::dot(p.d - c, acr) * kReH;
    std::sort(tw.begin(), tw.end(), [](const TP &a, const TP &b) { return a.along < b.along; });
    auto push = [&](const glm::dvec3 &mapDir, const glm::dvec3 &outMap, double s) {
        HaboobPoint q;
        const glm::dvec3 e = glm::normalize(toE(mapDir));
        glm::dvec3 o = toE(outMap);
        o = glm::normalize(o - e * glm::dot(o, e));
        q.dir = e;
        q.out = o;
        q.tan = glm::cross(e, o);
        q.strength = s;
        pts.push_back(q);
    };
    auto ahead = [&](const glm::dvec3 &d, const glm::dvec3 &dirMap, double dist) {
        return glm::normalize(d + dirMap * (dist / kReH));
    };
    if (line || tw.size() >= 2)
    {
        // Along the line of towers, dG ahead; extended a little past each end, curving back (the front's flanks).
        const double stepM = 4000.0;
        auto at = [&](size_t i) { return tw[i].d; };
        for (size_t i = 0; i + 1 < tw.size(); ++i)
        {
            const double L = std::max(glm::length(at(i + 1) - at(i)) * kReH, 1.0);
            const int nS = std::max(1, (int)(L / stepM));
            for (int k = 0; k < nS; ++k)
            {
                const double f = (double)k / nS;
                const glm::dvec3 d = glm::normalize(glm::mix(at(i), at(i + 1), f));
                push(ahead(d, mv, dG), mv, glm::mix(tw[i].s, tw[i + 1].s, f));
            }
        }
        for (int end = 0; end < 2; ++end)
        {
            const TP &e = end == 0 ? tw.front() : tw.back();
            const double sgn = end == 0 ? -1.0 : 1.0;
            for (int k = 0; k <= 3; ++k)
            {
                const double a = (0.25 + 0.2 * k) * 0.5 * kPiH;    // swinging back round the end
                const glm::dvec3 o = glm::normalize(mv * std::cos(a) + acr * (sgn * std::sin(a)));
                push(ahead(e.d, o, dG), o, e.s * (1.0 - 0.2 * k));
            }
        }
    }
    else
    {
        // A single cell: a semicircle of outflow round the strongest tower, widest ahead of its motion.
        const TP &e = tw.front();
        for (int k = -5; k <= 5; ++k)
        {
            const double a = k * (0.8 * kPiH / 10.0);
            const glm::dvec3 o = glm::normalize(mv * std::cos(a) + acr * std::sin(a));
            push(ahead(e.d, o, dG * (0.7 + 0.3 * std::cos(a))), o, e.s * (0.6 + 0.4 * std::cos(a)));
        }
    }
}

void SatelliteSim::weatherFillHaboobs(GpuDustCloud *out, uint32_t &n)
{
    FPROF_SCOPE("weatherFillHaboobs");
    haboobsNow = 0;
    haboobNearestKm = -1.0;
    aerosolFirst = n;   // the haboobs and the wildfire smoke are one range (smoke.glsl): their own light, spectral
    if (!out || haboobGain <= 0.0f || !stormObjects || !stormCatalog || !biomeMap.ready()) return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const double eyeR = (double)obsEyeRadiusM();
    const glm::dvec3 eye = up * eyeR;
    const double alt = std::max(eyeR - kReH, 0.0);
    const double reachKm = std::clamp(350.0 + 2.0 * alt / 1000.0, 350.0, 2000.0);
    const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up) + glm::dvec3(1e-12, 0, 0));
    const glm::dvec3 N = glm::cross(up, E);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, E), glm::dot(v, N), glm::dot(v, up)); };
    const double ph = cloudDriftPhase(), cph = std::cos(ph), sph = std::sin(ph);
    const glm::dvec3 si = sunDirEciAt(t);
    const double gr = earthRotationAngle(t);
    const glm::dvec3 sunE = glm::normalize(glm::dvec3(si.x * std::cos(gr) + si.y * std::sin(gr),
                                                      -si.x * std::sin(gr) + si.y * std::cos(gr), si.z));
    const double kTwoPi15 = 15.749609945722419;
    const uint32_t cap = std::min<uint32_t>(kMaxDustClouds, n + 200u);
    // The storms with a dominant tower in reach, nearest first.
    std::vector<std::pair<double, uint64_t>> ids;
    for (const wx::Tower &tw : stormTowers)
    {
        if ((tw.role < 0.99f && !wx::isLineSystem(tw.kind)) || tw.kind == wx::StormKind::Tropical ||
            tw.kind == wx::StormKind::AirMass) continue;
        const glm::dvec3 d = glm::normalize(tw.dir);
        const glm::dvec3 ed(d.x * cph + d.y * sph, -d.x * sph + d.y * cph, d.z);
        const double km = std::acos(std::clamp(glm::dot(ed, up), -1.0, 1.0)) * 6371.0;
        if (km > reachKm) continue;
        bool have = false;
        for (auto &p : ids)
            if (p.second == tw.stormId) { p.first = std::min(p.first, km); have = true; }
        if (!have) ids.push_back({km, tw.stormId});
    }
    std::sort(ids.begin(), ids.end());
    std::vector<HaboobPoint> pts;
    for (const auto &sid : ids)
    {
        const wx::Storm *S = stormCatalog->find(sid.second);
        if (!S) continue;
        haboobFrontOf(*S, t, pts);
        if (pts.empty()) continue;
        // Spacing on screen: the wall pieces ~3-4 km apart near, coarser far.
        const double sp = std::clamp(sid.first * 1000.0 * 0.015, 3500.0, 20000.0);
        double acc = 1e30;
        glm::dvec3 prev = pts.front().dir;
        int made = 0;
        for (size_t i = 0; i < pts.size() && n < cap; ++i)
        {
            acc += glm::length(pts[i].dir - prev) * kReH;
            prev = pts[i].dir;
            if (acc < sp) continue;
            acc = 0.0;
            const HaboobPoint &q = pts[i];
            const double lat = glm::degrees(std::asin(std::clamp(q.dir.z, -1.0, 1.0)));
            const double lon = glm::degrees(std::atan2(q.dir.y, q.dir.x));
            const double dust = biomeMap.dustAt(lat, lon);
            const double k = dust * q.strength * haboobGain;
            if (k < 0.08) continue;
            const double ground = std::max(0.0, (double)cpuTerrainHeightM((float)lat, (float)lon));
            const uint64_t h = hbMix(S->id ^ (uint64_t)(i * 2654435761u));
            const double H = (1500.0 + 1500.0 * std::min(1.0, k)) * (0.85 + 0.3 * ((h & 1023) / 1023.0));
            const double sinE = glm::dot(q.dir, sunE);
            const double dayK = glm::smoothstep(-0.06, 0.06, sinE);
            const double airm = 1.0 / (std::max(sinE, 0.0) + 0.05);
            const glm::dvec3 sunCol = glm::exp(-glm::dvec3(0.03, 0.05, 0.12) * airm) * dayK;
            const glm::dvec3 skyCol = 0.2 * glm::dvec3(0.62, 0.78, 1.0) * dayK;
            // Ochre, and dimmer than cloud: a thick wall of mineral dust (absorbing in the blue) reads tan-brown, never the
            // white of a cumulus (its in-scatter at 0.55 of the plume scale; in the aerosol range it gives back no sky light either).
            const glm::dvec3 tint = glm::dvec3(0.78, 0.55, 0.34) * 0.55;
            const double alb = 0.62;
            auto emit = [&](const glm::dvec3 &C, const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c, double sA,
                            double sB, double sC, double Sigma, uint64_t seed) {
                if (n >= cap || Sigma < 1.0) return;
                const glm::dvec3 rel = C - eye;
                const double range = std::max(glm::length(rel), 1.0);
                const glm::dvec3 ae = toEnu(a), be = toEnu(b), ce = toEnu(c);
                const double iA = 1.0 / (sA * sA), iB = 1.0 / (sB * sB), iC = 1.0 / (sC * sC);
                auto Mx = [&](int p, int r) { return iA * ae[p] * ae[r] + iB * be[p] * be[r] + iC * ce[p] * ce[r]; };
                GpuDustCloud &d = out[n++];
                const glm::dvec3 re = toEnu(rel);
                d.c = glm::vec4(glm::vec3(re), (float)Sigma);
                d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2), (float)(1.0 / (kTwoPi15 * sA * sB * sC)));
                const glm::dvec3 sunL = 4.0 * kPiH * alb * tint * sunCol, skyL = 4.0 * kPiH * alb * tint * skyCol;
                const double lS = sunL.x + sunL.y + sunL.z, lI = skyL.x + skyL.y + skyL.z;
                const double share = std::min(lI / std::max(lI + lS, 1e-9), 0.999);
                d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2),
                                 (float)(4.0 * (double)(hbMix(seed) & 127u) + 2.0 + share));   // seeds < 128: a wall (cloud_march.comp)
                d.col = glm::vec4(glm::vec3(sunL + skyL), (float)(2.0 + alb));
                const double R = 5.5 * std::max(sA, std::max(sB, sC));
                d.cone = R >= 0.95 * range ? glm::vec4(0.0f, 0.0f, 0.0f, -2.0f)
                                           : glm::vec4(glm::vec3(re / range), (float)std::sqrt(1.0 - (R / range) * (R / range)));
            };
            // The wall: a tall, dense slab along the front (its outer face ~1.5 km deep), resting on the ground.
            const double sAl = 0.7 * sp, sOut = 1500.0, sZ = 0.42 * H;
            const glm::dvec3 Cw = q.dir * (kReH + ground + 0.45 * H);
            const double tauOut = 25.0 * std::min(1.0, k);   // through the wall, outward (visibility tens of m inside)
            emit(Cw, q.tan, q.out, q.dir, sAl, sOut, sZ, tauOut * 2.0 * kPiH * sAl * sZ, h);
            // The head: the gust front's rolled nose, bulging forward high up (the wall leans out over its foot).
            {
                const glm::dvec3 Ch = glm::normalize(q.dir + q.out * (900.0 / kReH)) * (kReH + ground + 0.72 * H);
                const double sZh = 0.22 * H;
                emit(Ch, q.tan, q.out, q.dir, 0.8 * sAl, 1100.0, sZh, 0.8 * tauOut * 2.0 * kPiH * 0.8 * sAl * sZh, h ^ 0x4EADull);
            }
            ++made;
            // The pall behind every other piece: lower, wider, thinner (the dust settling behind the front).
            if ((made & 1) == 0)
            {
                const glm::dvec3 Cp = glm::normalize(q.dir - q.out * (9000.0 / kReH)) * (kReH + ground + 0.3 * H);
                const double sPal = 1.4 * sp, sBack = 7000.0, sZp = 0.35 * H;
                emit(Cp, q.tan, q.out, glm::normalize(Cp), sPal, sBack, sZp, 1.5 * std::min(1.0, k) * 2.0 * kPiH * sPal * sZp,
                     h ^ 0x9A11ull);
            }
        }
        if (made > 0)
        {
            ++haboobsNow;
            if (haboobNearestKm < 0.0) haboobNearestKm = sid.first;
        }
    }
}
