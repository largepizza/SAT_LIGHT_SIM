// SatelliteSimMeteors.cpp — meteor showers in the sky (2026-10-27; Meteors.h, CLAUDE.md "Meteors").
//
// Each frame (kesslerFillDust's finish, after the Kessler fireballs and before the launches) the meteors alive now
// near the observer are generated (meteors::generate: a pure function of sim time) and drawn with the re-entry
// fireballs' machinery:
//   head      a point in kFireBuf (kessler_fireballs.comp -> the point, bloom and glare draws), its brightness from
//             the absolute magnitude, the light curve, the distance, the air along the line of sight and the sky's
//             dimming in that direction (night, light-pollution dome, the Moon: the stars' chain), so a city or a
//             bright Moon takes the faint ones away through the point model's own limit
//   streak    the path the head swept over the eye's persistence (0.1 s, or the frame if longer): three emissive
//             Gaussians in the dust list, the front brightest — the motion drawn within one frame, not a stepping point
//   fragments a fireball's (M <= -4) pieces after its first flare: heads + short streaks
//   train     a bolide's persistent train: Gaussians along the 78-108 km part of its path, green then orange, fading
//             over seconds to a minute and drifting with a hashed upper-atmosphere wind that turns with height
// Not drawn in time warp (above 2x: at 10x a 0.5 s meteor would flash for 3 frames, ten times as often).
// The dust list's share: at most 96 Gaussians, and never past kMaxDustClouds - 200 (the launches, tornadoes,
// haboobs and wildfire smoke come after); the brightest meteors first.
#include "../FrameProfile.h"
#include "../Harness.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kRe = 6371000.0;
constexpr double kPersistS = 0.10;   // the eye's persistence: the streak's length in time
constexpr uint32_t kMeteorGaussMax = 96;
constexpr int kMeteorHeadMax = 600;
double rad(double d) { return d * (kPi / 180.0); }
double deg(double r) { return r * (180.0 / kPi); }
glm::dvec3 ecefToEci(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return {c * v.x - s * v.y, s * v.x + c * v.y, v.z};
}
glm::dvec3 eciToEcef(const glm::dvec3 &v, double t)
{
    const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
    return {c * v.x + s * v.y, -s * v.x + c * v.y, v.z};
}
float timeScaleNow(bool paused, int idx)
{
    static constexpr float kScales[] = {1.0f, 10.0f, 60.0f, 300.0f, 3600.0f, 86400.0f, 86400.0f * 7.0f, 86400.0f * 30.0f,
                                        86400.0f * 365.0f};
    return paused ? 0.0f : kScales[std::clamp(idx, 0, 8)];
}
} // namespace

// The stars' sky dimming (updateStars) in direction enu: the night gate (with a daylight floor of 9 mag, so a bright
// bolide still shows by day), the light-pollution dome and the Moon. Hand-kept copy of updateStars' chain.
float SatelliteSim::meteorSkySuppress(const glm::vec3 &enu) const
{
    const float nightFactor = glm::clamp(-sunDirENU.w * 5.0f, 0.0f, 1.0f);
    const float obsR = glm::length(obsECI), obsH = obsR - 6371000.0f;
    const float atmFrac = 1.0f - glm::clamp((obsH - 40000.0f) / 60000.0f, 0.0f, 1.0f);
    const float nightEff = glm::mix(skyGlareEased, nightFactor, atmFrac);
    const float night = 2.5e-4f + (1.0f - 2.5e-4f) * nightEff; // 10^(-0.4 x 9)
    float bearing = atan2f(enu.x, enu.y);
    if (bearing < 0.0f) bearing += 2.0f * glm::pi<float>();
    const float secF = bearing * (float(kNumLightSectors) / (2.0f * glm::pi<float>())) - 0.5f;
    const int sec0 = (int)floorf(secF);
    const float fr = secF - float(sec0);
    const int s0 = ((sec0 % kNumLightSectors) + kNumLightSectors) % kNumLightSectors, s1 = (s0 + 1) % kNumLightSectors;
    const float domeAz = glm::mix(lightDomeAz[s0], lightDomeAz[s1], fr);
    const float elevFalloff = 0.35f / (std::max(enu.z, 0.0f) + 0.35f);
    const float domeVal = glm::clamp(domeAz * (0.4f + 0.6f * elevFalloff), 0.0f, 1.0f);
    const float moonElev = glm::dot(moonDirECI, glm::normalize(obsECI));
    const float tm = glm::clamp(moonElev / 0.5f, 0.0f, 1.0f);
    const float moonBright = tm * tm * moonDirENU.w;
    return night * (1.0f - domeVal * 0.99f) * (1.0f - moonBright * 0.9f);
}

void SatelliteSim::meteorsFill(GpuDustCloud *out, uint32_t &n, double toSky, double pixFull)
{
    FPROF_SCOPE("meteorsFill");
    const auto c0 = std::chrono::steady_clock::now();
    struct Clock
    {
        std::chrono::steady_clock::time_point c0;
        double &o;
        ~Clock() { o = 0.9 * o + 0.1 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count(); }
    } clock{c0, meteorCpuMs};
    meteorNow_.clear();
    meteorDraws_ = meteorGenerated_ = meteorHeads_ = meteorGauss_ = 0;
    meteorDustAt_ = (int)n;
    meteorWarpOff_ = false;
    if (!meteorsEnabled || !kFireMapped) return;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // The persistent trains' decay time x "train_duration" (meteors.train_duration, 0.3 by default since 2026-10-28:
    // a -10 bolide's train was drawn for ~200 s at full scale) and their cut: kTrainCut decay times after passing.
    const double tlK = (double)std::max(meteorTrainDuration, 0.0f);
    constexpr double kTrainCut = 4.0;
    // Drop forced meteors that are over.
    meteorForced_.erase(std::remove_if(meteorForced_.begin(), meteorForced_.end(),
                                       [&](const meteors::Meteor &m) {
                                           return t - m.tStart > m.dur + 6.0 * m.trainLife * tlK + 2.0 || t < m.tStart - 600.0;
                                       }),
                        meteorForced_.end());
    // Time warp: past 2x the faint bands are not drawn (at 10x a 0.5-s meteor flashes for 3 frames and ten times as
    // often), but fireballs and bolides (bands 0-1, M <= -4: a few an hour) are, up to 1 min/s, so a bolide and its
    // train show whatever speed the sky is played at (2026-10-28, issue 20261028_191529: replayed at 10x it never
    // appeared). Which meteors exist never depends on the warp.
    const float warpNow = timeScaleNow(timePaused, timeScaleIdx);
    if (warpNow > 60.0f)
    {
        meteorWarpOff_ = true;
        return;
    }
    meteorBandMax_ = warpNow > 2.0f ? 1 : meteors::kBands - 1;
    const glm::dvec3 eye(obsECI);
    const glm::dvec3 eyeEcef = eciToEcef(eye, t);
    std::vector<meteors::Meteor> ms;
    // The bins alive now, each generated once (meteorBinCache_) while the eye stays within 30 km and the gains hold.
    if (glm::length(eyeEcef - meteorCacheEye_) > kMeteorCacheRadiusM || meteorCacheGain_[0] != meteorRateGain || meteorCacheGain_[1] != meteorFireballGain)
    {
        meteorBinCache_.clear();
        meteorCacheEye_ = eyeEcef;
        meteorCacheGain_[0] = meteorRateGain;
        meteorCacheGain_[1] = meteorFireballGain;
    }
    for (auto it = meteorBinCache_.begin(); it != meteorBinCache_.end();)
    {
        const int b = (int)((it->first >> 48) & 0xFF);
        const int64_t k = (int64_t)(it->first & 0xFFFFFFFFFFFFull) - (int64_t)0x800000000000ll;
        const double T = meteors::kBandBinS[b];
        if ((double)(k + 1) * T < t - meteors::kBandMaxAgeS[b] - T || (double)k * T > t + T)
            it = meteorBinCache_.erase(it);
        else
            ++it;
    }
    for (int sIdx = 0; sIdx < meteors::showerCount(); ++sIdx)
    {
        if (sIdx != meteors::kSporadic && meteors::zhrAt(sIdx, meteors::solarLongitudeDeg(sunDirEciAt(t))) <= 0.0) continue;
        for (int b = 0; b <= meteorBandMax_; ++b)
        {
            const double T = meteors::kBandBinS[b];
            const int64_t k0 = (int64_t)std::floor((t - meteors::kBandMaxAgeS[b]) / T), k1 = (int64_t)std::floor(t / T);
            for (int64_t k = k0; k <= k1; ++k)
            {
                const uint64_t key = ((uint64_t)sIdx << 56) | ((uint64_t)b << 48) | (uint64_t)((k + (int64_t)0x800000000000ll) & 0xFFFFFFFFFFFFll);
                auto it = meteorBinCache_.find(key);
                if (it == meteorBinCache_.end())
                {
                    std::vector<meteors::Meteor> v;
                    meteors::GenParams gp;
                    gp.tA = (double)k * T;
                    gp.tB = (double)(k + 1) * T;
                    gp.eyeEcef = meteorCacheEye_;
                    gp.eyeMarginM = kMeteorCacheRadiusM; // valid for every eye the cache serves
                    gp.cullMag = 10.0; // (fixed: the cache must not depend on the exposure; pointEffLimitMag ~8-9)
                    gp.rateGain = meteorRateGain;
                    gp.fireballGain = meteorFireballGain;
                    gp.onlyShower = sIdx;
                    gp.onlyBand = b;
                    meteorDraws_ += meteors::generate(gp, [&](const meteors::Meteor &m) { v.push_back(m); });
                    it = meteorBinCache_.emplace(key, std::move(v)).first;
                }
                for (const meteors::Meteor &m : it->second)
                {
                    const double age = t - m.tStart;
                    if (age >= 0.0 && age <= m.dur + (m.trainLife > 0.0f ? kTrainCut * m.trainLife * tlK : 0.0)) ms.push_back(m);
                }
            }
        }
    }
    meteorGenerated_ = (int)ms.size();
    for (const meteors::Meteor &m : meteorForced_)
        if (t >= m.tStart) ms.push_back(m);
    if (ms.empty()) return;

    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto toEnu = [&](const glm::dvec3 &v) { return glm::dvec3(glm::dot(v, ex), glm::dot(v, ey), glm::dot(v, ez)); };
    const double K = satphot::kFluxToFlare * (double)brightnessScale;
    const double R = satphot::kEarthRadiusM;
    auto earthHides = [&](const glm::dvec3 &rel, double range) {
        const double b = glm::dot(eye, rel / range), c = glm::dot(eye, eye) - R * R, disc = b * b - c;
        if (disc <= 0.0) return false;
        const double tn = -b - std::sqrt(disc);
        return tn > 0.0 && tn < range;
    };
    // The apparent magnitude of absolute magnitude M x intensity I seen at P (ECI), and its effectFlare after the
    // sky's dimming. False when hidden by the Earth.
    struct Seen
    {
        glm::dvec3 rel, dirEnu;
        double range = 0.0, mag = 99.0, flare = 0.0;
    };
    auto see = [&](const glm::dvec3 &Peci, double Mabs, Seen &s) {
        s.rel = Peci - eye;
        s.range = glm::length(s.rel);
        if (s.range < 1.0 || earthHides(s.rel, s.range)) return false;
        s.dirEnu = toEnu(s.rel / s.range);
        s.mag = Mabs + 5.0 * std::log10(s.range / 1.0e5) + atmExtinctionMag(eye, s.rel / s.range, s.range, (double)extinctionCoeff);
        s.flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - s.mag)) * (double)meteorSkySuppress(glm::vec3(s.dirEnu));
        return true;
    };
    const double sMinK = 0.6 * 2.0 * pixFull; // a Gaussian at least ~ a half-res pixel wide
    // Below the point model's limit nothing shows: no Gaussians for it (a city's sky hides most of them).
    const double flareVisible = 0.5 * 0.008 * std::pow(10.0, 0.4 * (6.0 - (double)pointEffLimitMag()));
    // An emissive Gaussian segment from A to Bp (ECI) carrying flareEq of on-screen energy.
    auto addSeg = [&](const glm::dvec3 &A, const glm::dvec3 &Bp, double width, double flareEq, double range, const glm::vec3 &col) {
        if (n >= kMaxDustClouds - 200u || (uint32_t)meteorGauss_ >= kMeteorGaussMax || !(flareEq > flareVisible)) return false;
        const glm::dvec3 ax = Bp - A;
        const double L = glm::length(ax);
        const glm::dvec3 a = L > 1e-3 ? toEnu(ax / L) : glm::dvec3(0, 0, 1);
        glm::dvec3 b2 = glm::cross(a, glm::dvec3(0, 0, 1));
        b2 = glm::length(b2) > 1e-6 ? glm::normalize(b2) : glm::dvec3(1, 0, 0);
        const glm::dvec3 c2 = glm::cross(a, b2);
        const double sMin = sMinK * range;
        const double sA = std::max(0.29 * L, sMin), sP = std::max(width, sMin);
        const double iA = 1.0 / (sA * sA), iP = 1.0 / (sP * sP);
        auto Mx = [&](int p, int q) { return iA * a[p] * a[q] + iP * (b2[p] * b2[q] + c2[p] * c2[q]); };
        const double J = std::min(flareEq, 3000.0) * range * range / K;
        const glm::dvec3 ce = toEnu(0.5 * (A + Bp) - eye);
        GpuDustCloud &d = out[n++];
        const double ext = 5.0 * std::max(sA, sP), dd = glm::length(ce);
        d.cone = ext >= 0.95 * dd ? glm::vec4(0.0f, 0.0f, 0.0f, -2.0f)
                                  : glm::vec4(glm::vec3(ce / dd), (float)std::sqrt(1.0 - (ext / dd) * (ext / dd)));
        d.c = glm::vec4(glm::vec3(ce), (float)(J * toSky));
        d.ia = glm::vec4((float)Mx(0, 0), (float)Mx(1, 1), (float)Mx(2, 2),
                         (float)(1.0 / (std::pow(2.0 * glm::pi<double>(), 1.5) * sA * sP * sP)));
        d.ib = glm::vec4((float)Mx(0, 1), (float)Mx(0, 2), (float)Mx(1, 2), 0.0f);
        d.col = glm::vec4(col, 1.0f);
        ++meteorGauss_;
        return true;
    };
    auto addHead = [&](const Seen &s, double flare, const glm::vec3 &col) {
        if (flare < 2e-4 || kFireCount >= kFireballMax || meteorHeads_ >= kMeteorHeadMax) return;
        fo[kFireCount++] = {glm::vec4(glm::vec3(s.dirEnu), (float)flare), glm::vec4(col, (float)s.range)};
        ++meteorHeads_;
    };

    // The heads first, ranked: the streaks and trains go to the brightest.
    struct Cand
    {
        size_t i;
        double flare;
    };
    std::vector<Cand> order;
    order.reserve(ms.size());
    for (size_t i = 0; i < ms.size(); ++i)
    {
        const meteors::Meteor &m = ms[i];
        const double age = t - m.tStart;
        const double x = std::min(age / m.dur, 1.0);
        const double I = age <= m.dur ? meteors::lightCurve(m, x) : 0.0;
        Seen s;
        double fl = 0.0;
        if (see(ecefToEci(meteors::pathAt(m, x), t), (double)m.M - 2.5 * std::log10(std::max(I, 1e-6)), s)) fl = I > 0.0 ? s.flare : 0.0;
        if (m.trainLife > 0.0f && tlK > 0.0) fl += 1e-3 + 0.01 * std::pow(10.0, -0.4 * (double)m.M); // a train ranks by its meteor
        order.push_back({i, fl});
    }
    std::sort(order.begin(), order.end(), [](const Cand &a, const Cand &b) { return a.flare > b.flare; });
    const double wPersist = std::max(kPersistS, 1.0 / 30.0);
    for (const Cand &cd : order)
    {
        const meteors::Meteor &m = ms[cd.i];
        const double age = t - m.tStart;
        MeteorNow rec;
        rec.shower = m.shower;
        rec.M = m.M;
        rec.age = (float)age;
        rec.dur = (float)m.dur;
        rec.vKms = (float)(m.vMs / 1000.0);
        rec.forced = m.forced != 0;
        const glm::dvec3 dirE = glm::normalize(m.E - m.B);
        if (age <= m.dur)
        {
            const double x = age / m.dur;
            const double I = meteors::lightCurve(m, x);
            const glm::dvec3 Pe = meteors::pathAt(m, x);
            Seen s;
            if (I > 1e-5 && see(ecefToEci(Pe, t), (double)m.M - 2.5 * std::log10(I), s))
            {
                const glm::vec3 col = meteors::color(m, I);
                rec.mag = (float)s.mag;
                rec.flare = (float)s.flare;
                rec.azDeg = (float)deg(std::atan2(s.dirEnu.x, s.dirEnu.y));
                rec.elDeg = (float)deg(std::asin(std::clamp(s.dirEnu.z, -1.0, 1.0)));
                rec.rangeKm = (float)(s.range / 1000.0);
                rec.hKm = (float)((glm::length(Pe) - kRe) / 1000.0);
                // The streak: the path over the persistence window (or what there is of it), on screen.
                const double back = std::min(age, wPersist) * m.vMs;
                const double pxLen = back / s.range / std::max(2.0 * pixFull, 1e-9);
                double headK = 1.0;
                if (pxLen > 1.5)
                {
                    headK = 0.4 + 0.6 * glm::smoothstep(-2.0, -6.0, s.mag); // a fireball's head stays a blinding point
                    // three pieces, front brightest (the persistence fades): weights 0.5 / 0.3 / 0.2 of the rest
                    static constexpr double kW[3] = {0.5, 0.3, 0.2};
                    for (int q = 0; q < 3; ++q)
                    {
                        const glm::dvec3 A = ecefToEci(Pe - dirE * (back * q / 3.0), t), Bq = ecefToEci(Pe - dirE * (back * (q + 1) / 3.0), t);
                        if (addSeg(A, Bq, 6.0, 0.6 * kW[q] * s.flare, s.range, col)) ++rec.gauss;
                    }
                }
                addHead(s, headK * s.flare, col);
                // A fireball lights the clouds, the haze and the ground round it (the launches' engine lights).
                const double Mnow = (double)m.M - 2.5 * std::log10(I);
                if (Mnow <= -4.0)
                    engineLightAdd(ecefToEci(Pe, t), flameIntensitySunsM2(Mnow) * (1.0 - glm::clamp(((double)sunDirENU.w + 0.1) / 0.3, 0.0, 1.0)),
                                   col, 800.0, ecefToEci(dirE, t), 0.0, 0.0, false, 0.0);
            }
            // Fragments of a fireball.
            const int nf = meteors::fragmentCount(m);
            for (int k = 0; k < nf; ++k)
            {
                glm::dvec3 off;
                double rel;
                if (!meteors::fragmentAt(m, k, age, off, rel)) continue;
                Seen sf;
                const glm::dvec3 Pf = Pe + off;
                if (!see(ecefToEci(Pf, t), (double)m.M - 2.5 * std::log10(rel), sf)) continue;
                const glm::vec3 col = meteors::color(m, rel);
                addHead(sf, 0.5 * sf.flare, col);
                const double back = std::min(age, wPersist) * m.vMs * 0.8;
                if (addSeg(ecefToEci(Pf, t), ecefToEci(Pf - dirE * back, t), 8.0, 0.5 * sf.flare, sf.range, col)) ++rec.gauss;
                ++rec.fragments;
            }
        }
        // The persistent train: what the meteor passed between 78 and 108 km, glowing and drifting with the wind.
        if (m.trainLife > 0.0f && tlK > 0.0)
        {
            constexpr int kSeg = 10;
            const double hLo = 78000.0, hHi = 108000.0;
            // the path's fraction where its height crosses hHi .. hLo (heights fall along the path)
            auto hAt = [&](double x) { return glm::length(meteors::pathAt(m, x)) - kRe; };
            auto xAtH = [&](double h) {
                double lo = 0.0, hi = 1.0;
                if (hAt(0.0) <= h) return 0.0;
                if (hAt(1.0) >= h) return 1.0;
                for (int it = 0; it < 30; ++it)
                {
                    const double mid = 0.5 * (lo + hi);
                    (hAt(mid) > h ? lo : hi) = mid;
                }
                return 0.5 * (lo + hi);
            };
            const double xa = xAtH(hHi), xb = xAtH(hLo), xNow = std::min(age / m.dur, 1.0);
            const double tl = (double)m.trainLife * tlK;
            for (int q = 0; q < kSeg && xb > xa + 1e-4; ++q)
            {
                const double x0 = xa + (xb - xa) * q / kSeg, x1 = xa + (xb - xa) * (q + 1) / kSeg;
                if (x1 > xNow) break;
                const double xm = 0.5 * (x0 + x1);
                const double ag = age - xm * m.dur;
                if (ag > kTrainCut * tl) continue;
                auto drift = [&](double x) {
                    const glm::dvec3 P = meteors::pathAt(m, x);
                    const glm::dvec3 up = glm::normalize(P);
                    glm::dvec3 east = glm::cross(glm::dvec3(0, 0, 1), up);
                    east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1, 0, 0);
                    const glm::dvec3 north = glm::cross(up, east);
                    const glm::dvec2 w = meteors::trainWind(m, glm::length(P) - kRe);
                    const double a = std::max(0.0, age - x * m.dur);
                    return P + (east * w.x + north * w.y) * a;
                };
                const glm::dvec3 A = drift(x0), Bq = drift(x1);
                // Its light: ~3.5 mag under the meteor's peak in all, a wake +2.5 mag over the first ~1.5 s, fading.
                const double Itr = std::exp(-ag / tl) * (1.0 + 9.0 * std::exp(-ag / 1.5)) / kSeg;
                Seen st;
                if (!see(ecefToEci(0.5 * (A + Bq), t), (double)m.M + 3.5 - 2.5 * std::log10(std::max(Itr, 1e-9)), st)) continue;
                const double g = std::exp(-ag / 2.0);
                const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.6f, 0.35f), glm::vec3(0.45f, 1.0f, 0.5f), (float)g);
                if (addSeg(ecefToEci(A, t), ecefToEci(Bq, t), 25.0 + 5.0 * ag, st.flare, st.range, col)) ++rec.trainSegs;
            }
        }
        if (rec.flare > 0.0f || rec.trainSegs > 0 || rec.fragments > 0) meteorNow_.push_back(rec);
    }
    static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;
}

// ── The Watch: a dark site with the radiant well up, near the peak, at 1x, looking 45 deg from the radiant ────────
bool SatelliteSim::meteorWatchShower(int s, double t0, double t1, double tPeak, std::string &msg)
{
    FPROF_SCOPE("meteorWatchShower");
    const meteors::Shower &S = meteors::shower(s);
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    std::vector<double> cand;
    if (now >= t0 && now < t1)
        for (int h = 0; h <= 24; ++h) cand.push_back(now + h * 3600.0);
    else
        for (int h = -14; h <= 14; ++h) cand.push_back(tPeak + h * 3600.0);
    struct Pick
    {
        double score = -1e30, t = 0.0;
        glm::dvec3 site{0.0, 0.0, 1.0};
    };
    Pick best;
    std::vector<Pick> top; // the best sites at the chosen time (cloud re-ranking)
    for (int pass = 0; pass < 2; ++pass)
    {
        for (double tc : pass == 0 ? cand : std::vector<double>{best.t})
        {
            if (tc < t0 || tc >= t1) continue;
            const glm::dvec3 sunE = sunDirEciAt(tc);
            const double lam = meteors::solarLongitudeDeg(sunE);
            const double prof = meteors::zhrAt(s, lam) / std::max(S.zhr + S.zhr2, 1e-6);
            const glm::dvec3 se = eciToEcef(glm::normalize(sunE), tc), re = eciToEcef(meteors::radiantEci(s, lam), tc);
            const glm::dvec3 md = glm::normalize(moonGeoEciAt(tc));
            const glm::dvec3 me = eciToEcef(md, tc);
            const double ph = std::acos(std::clamp(-glm::dot(md, glm::normalize(sunE)), -1.0, 1.0));
            const double lit = 0.5 * (1.0 + std::cos(ph));
            for (double la = -58.0; la <= 68.0; la += 3.0)
            {
                const double cl = std::cos(rad(la)), sl = std::sin(rad(la));
                for (double lo = -180.0; lo < 180.0; lo += 4.0)
                {
                    const glm::dvec3 u(cl * std::cos(rad(lo)), cl * std::sin(rad(lo)), sl);
                    const double sunAlt = deg(std::asin(std::clamp(glm::dot(se, u), -1.0, 1.0)));
                    const double sh = glm::dot(re, u);
                    if (sunAlt > -15.0 || sh < std::sin(rad(25.0))) continue;
                    const double moonUp = glm::smoothstep(-0.05, 0.3, glm::dot(me, u));
                    const float city = nightLumCpu((float)la, (float)lo);
                    double sc = sh * prof * (1.0 - 0.75 * moonUp * lit) - 0.03 * std::max(0.0f, city - 16.0f) +
                                0.004 * std::min(-sunAlt - 15.0, 6.0);
                    if (now >= t0 && now < t1) sc -= 0.01 * (tc - now) / 3600.0;
                    if (pass == 1)
                    {
                        top.push_back({sc, tc, u});
                        continue;
                    }
                    if (sc > best.score) best = {sc, tc, u};
                }
            }
        }
        if (best.score < -1e29) break;
    }
    if (best.score < -1e29)
    {
        msg = std::string(S.name) + ": no dark site with the radiant up near the peak";
        return false;
    }
    skyJumpTo(best.t); // (first: the cloud map's drift is that moment's)
    std::sort(top.begin(), top.end(), [](const Pick &a, const Pick &b) { return a.score > b.score; });
    if (top.size() > 60) top.resize(60);
    glm::dvec3 site = best.site;
    double bestSc = -1e30;
    for (const Pick &p : top)
    {
        const double la = deg(std::asin(std::clamp(p.site.z, -1.0, 1.0))), lo = deg(std::atan2(p.site.y, p.site.x));
        double sc = p.score - 0.6 * cloudCoverAtEcef(p.site);
        if (oceanFraction(rad(la), rad(lo), 20000.0, 1) < 0.5f) sc += 0.08; // ground under the observer
        if (sc > bestSc)
        {
            bestSc = sc;
            site = p.site;
        }
    }
    skyPlaceAt(site, best.t);
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    skAimA = skAimB = -1;
    // The radiant in the local sky, and a direction ~45 deg from it, as high as is comfortable (~50 deg).
    const double lam = meteors::solarLongitudeDeg(sunDirEciAt(best.t));
    const glm::dvec3 rEci = meteors::radiantEci(s, lam);
    const glm::vec3 rEnu = glm::normalize(glm::vec3((float)glm::dot(rEci, glm::dvec3(eci2enuX)), (float)glm::dot(rEci, glm::dvec3(eci2enuY)),
                                                    (float)glm::dot(rEci, glm::dvec3(eci2enuZ))));
    float bAz = 0.0f, bEl = 50.0f, bSc = -1e30f;
    for (float az = 0.0f; az < 360.0f; az += 5.0f)
        for (float el = 20.0f; el <= 80.0f; el += 2.0f)
        {
            const glm::vec3 d(std::cos(glm::radians(el)) * std::sin(glm::radians(az)), std::cos(glm::radians(el)) * std::cos(glm::radians(az)),
                              std::sin(glm::radians(el)));
            const float a = glm::degrees(std::acos(std::clamp(glm::dot(d, rEnu), -1.0f, 1.0f)));
            const float sc = -std::abs(a - 45.0f) * 3.0f - std::abs(el - 50.0f);
            if (sc > bSc)
            {
                bSc = sc;
                bAz = az;
                bEl = el;
            }
        }
    aimCameraAzEl(bAz, bEl);
    camera.fovYDeg = 75.0f;
    const double zhrNow = meteors::zhrAt(s, lam);
    char b[300];
    snprintf(b, sizeof(b), "%s (ZHR ~%.0f now) from %.1f %s %.1f %s: radiant %.0f deg up, Sun %.0f deg down, Moon %s; looking 45 deg from the radiant",
             S.name, zhrNow, std::abs(obsLatDeg), obsLatDeg >= 0 ? "N" : "S", std::abs(obsLonDeg), obsLonDeg >= 0 ? "E" : "W",
             deg(std::asin(std::clamp((double)rEnu.z, -1.0, 1.0))), -deg(std::asin(std::clamp((double)sunDirENU.z, -1.0, 1.0))),
             moonDirENU.z > 0.0f ? (moonDirENU.w > 0.5f ? "up and bright" : "up") : "down");
    msg = b;
    return true;
}

// ── Harness: meteors [state] | count [minutes=60] [lm=6.5] | force [kind=meteor|fireball|bolide] [shower=] [mag=]
//    [az=] [el=] [v=] | go [shower=] ─────────────────────────────────────────────────────────────────────────────
harness::Status SatelliteSim::harnessMeteors(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    nlohmann::json &r = a.result;
    const std::string sub = c.pos.empty() ? std::string("state") : c.pos[0];
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double lam = meteors::solarLongitudeDeg(sunDirEciAt(t));
    auto showersNow = [&]() {
        nlohmann::json arr = nlohmann::json::array();
        for (int s = 0; s < meteors::showerCount(); ++s)
        {
            const double z = meteors::zhrAt(s, lam);
            if (z <= 0.0) continue;
            const meteors::Shower &S = meteors::shower(s);
            nlohmann::json o = {{"id", S.id}, {"name", S.name}, {"zhr", z}, {"v_kms", S.vKms}, {"r", S.r}};
            if (s != meteors::kSporadic)
            {
                const glm::dvec3 re = meteors::radiantEci(s, lam);
                const glm::dvec3 e(glm::dot(re, glm::dvec3(eci2enuX)), glm::dot(re, glm::dvec3(eci2enuY)), glm::dot(re, glm::dvec3(eci2enuZ)));
                o["radiant_az_deg"] = deg(std::atan2(e.x, e.y));
                o["radiant_el_deg"] = deg(std::asin(std::clamp(e.z, -1.0, 1.0)));
                o["hourly_rate_est"] = z * std::max(e.z, 0.0);
            }
            arr.push_back(o);
        }
        return arr;
    };
    if (sub == "state")
    {
        r["enabled"] = meteorsEnabled;
        r["rate_gain"] = meteorRateGain;
        r["fireball_gain"] = meteorFireballGain;
        r["solar_longitude_deg"] = lam;
        r["showers_active"] = showersNow();
        r["sky_suppress_zenith_mag"] = -2.5 * std::log10(std::max(meteorSkySuppress(glm::vec3(0, 0, 1)), 1e-12f));
        r["point_limit_mag"] = pointEffLimitMag();
        r["warp_off"] = meteorWarpOff_;
        r["band_max"] = meteorBandMax_;
        r["train_duration"] = meteorTrainDuration;
        r["poisson_draws"] = meteorDraws_;
        r["generated_alive"] = meteorGenerated_;
        r["heads"] = meteorHeads_;
        r["gaussians"] = meteorGauss_;
        r["dust_list_at"] = meteorDustAt_;
        r["dust_list_total"] = kDustCount;
        r["forced"] = meteorForced_.size();
        r["cpu_ms"] = meteorCpuMs;
        nlohmann::json arr = nlohmann::json::array();
        for (const MeteorNow &m : meteorNow_)
            arr.push_back({{"shower", meteors::shower(m.shower).id}, {"abs_mag", m.M}, {"mag", m.mag}, {"flare", m.flare},
                           {"az_deg", m.azDeg}, {"el_deg", m.elDeg}, {"age_s", m.age}, {"dur_s", m.dur}, {"v_kms", m.vKms},
                           {"range_km", m.rangeKm}, {"h_km", m.hKm}, {"fragments", m.fragments}, {"train_segs", m.trainSegs},
                           {"gaussians", m.gauss}, {"forced", m.forced}});
        r["meteors"] = arr;
        r["message"] = std::to_string(meteorNow_.size()) + " meteors drawn, " + std::to_string(meteorHeads_) + " heads, " +
                       std::to_string(meteorGauss_) + " Gaussians";
        return harness::Status::Done;
    }
    if (sub == "count")
    {
        // Every meteor that started in the last `minutes` near the observer, at its peak: how bright it was from here
        // (+ the sky's dimming in its direction now). Counts by the limiting magnitude (lm, 6.5 = a dark-sky eye's)
        // and by brightness, all sky and in the camera's view.
        const double minutes = c.num("minutes", 60.0), lm = c.num("lm", 6.5);
        meteors::GenParams gp;
        gp.tA = t - minutes * 60.0;
        gp.tB = t;
        const glm::dvec3 eye(obsECI);
        gp.eyeEcef = eciToEcef(eye, t);
        gp.cullMag = lm;
        gp.rateGain = meteorRateGain;
        gp.fireballGain = meteorFireballGain;
        if (c.has("shower")) gp.onlyShower = meteors::showerByName(c.str("shower").c_str());
        const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
        const glm::vec3 camF(std::cos(glm::radians(camera.elDeg)) * std::sin(glm::radians(camera.azDeg)),
                             std::cos(glm::radians(camera.elDeg)) * std::cos(glm::radians(camera.azDeg)), std::sin(glm::radians(camera.elDeg)));
        const float halfFov = glm::radians(camera.fovYDeg) * 0.5f;
        int all = 0, seen = 0, inView = 0, le0 = 0, le4 = 0, le8 = 0, gen = 0;
        std::vector<int> byShower(meteors::showerCount(), 0);
        const int draws = meteors::generate(gp, [&](const meteors::Meteor &m) {
            ++gen;
            const glm::dvec3 rel = ecefToEci(meteors::pathAt(m, 0.667), t) - eye; // (Earth-fixed: where it is from here)
            const double range = glm::length(rel);
            const double b = glm::dot(eye, rel / range), cc = glm::dot(eye, eye) - kRe * kRe, disc = b * b - cc;
            if (disc > 0.0 && -b - std::sqrt(disc) > 0.0 && -b - std::sqrt(disc) < range) return;
            const glm::dvec3 d = rel / range;
            const glm::vec3 enu((float)glm::dot(d, ex), (float)glm::dot(d, ey), (float)glm::dot(d, ez));
            const double mag = (double)m.M + 5.0 * std::log10(range / 1e5) + atmExtinctionMag(eye, d, range, (double)extinctionCoeff) -
                               2.5 * std::log10(std::max(meteorSkySuppress(enu), 1e-12f));
            ++all;
            if (mag > lm) return;
            ++seen;
            ++byShower[m.shower];
            if (mag <= 0.0) ++le0;
            if (mag <= -4.0) ++le4;
            if (mag <= -8.0) ++le8;
            if (std::acos(std::clamp(glm::dot(enu, camF), -1.0f, 1.0f)) < halfFov) ++inView;
        });
        r["minutes"] = minutes;
        r["lm"] = lm;
        r["generated"] = gen;
        r["poisson_draws"] = draws;
        r["above_horizon"] = all;
        r["seen"] = seen;
        r["per_hour"] = seen * 60.0 / minutes;
        r["in_view_per_hour"] = inView * 60.0 / minutes;
        r["brighter_0"] = le0;
        r["brighter_m4"] = le4;
        r["brighter_m8"] = le8;
        nlohmann::json bs = nlohmann::json::object();
        for (int s = 0; s < meteors::showerCount(); ++s)
            if (byShower[s]) bs[meteors::shower(s).id] = byShower[s];
        r["by_shower"] = bs;
        r["sky_suppress_zenith_mag"] = -2.5 * std::log10(std::max(meteorSkySuppress(glm::vec3(0, 0, 1)), 1e-12f));
        r["showers_active"] = showersNow();
        char buf[200];
        snprintf(buf, sizeof(buf), "%.1f meteors/h brighter than %.1f all sky (%.1f/h in view), %d <= 0, %d <= -4, %d <= -8 in %.0f min",
                 seen * 60.0 / minutes, lm, inView * 60.0 / minutes, le0, le4, le8, minutes);
        r["message"] = buf;
        return harness::Status::Done;
    }
    if (sub == "force")
    {
        const std::string kind = c.str("kind", "fireball");
        int s = c.has("shower") ? meteors::showerByName(c.str("shower").c_str()) : -1;
        if (s < 0)
        {
            double bz = 0.0;
            for (int q = 0; q < meteors::kSporadic; ++q)
                if (meteors::zhrAt(q, lam) > bz)
                {
                    bz = meteors::zhrAt(q, lam);
                    s = q;
                }
            if (s < 0) s = meteors::kSporadic;
        }
        const double M = c.num("mag", kind == "bolide" ? -10.0 : kind == "meteor" ? 2.0 : -5.0);
        const double v = c.num("v", s == meteors::kSporadic ? 40.0 : meteors::shower(s).vKms) * 1000.0;
        const double az = c.num("az", camera.azDeg), el = c.num("el", std::max(camera.elDeg, 15.0f));
        // The end point: along the view (az, el) at the height the meteor would end at.
        meteors::Meteor m;
        const double vk = v / 1000.0;
        const double hE = std::clamp(65.0 + 0.35 * vk + 2.5 * M, 20.0, 80.0 + 0.55 * vk - 8.0) * 1000.0;
        const glm::dvec3 dEnu(std::cos(rad(el)) * std::sin(rad(az)), std::cos(rad(el)) * std::cos(rad(az)), std::sin(rad(el)));
        const glm::dvec3 dEci = glm::dvec3(eci2enuX) * dEnu.x + glm::dvec3(eci2enuY) * dEnu.y + glm::dvec3(eci2enuZ) * dEnu.z;
        const glm::dvec3 eye(obsECI);
        const double b = glm::dot(eye, dEci), cc = glm::dot(eye, eye) - (kRe + hE) * (kRe + hE), disc = b * b - cc;
        if (disc < 0.0) throw std::runtime_error("meteors force: that direction never reaches the meteor layer");
        const double L = -b + std::sqrt(disc);
        const glm::dvec3 endEcef = glm::normalize(eciToEcef(eye + dEci * L, t));
        glm::dvec3 rdE = s == meteors::kSporadic ? glm::dvec3(0.0) : eciToEcef(meteors::radiantEci(s, lam), t);
        if (glm::dot(rdE, endEcef) < 0.2 || c.has("radiant_el"))
        {
            // a radiant behind the camera, raised `radiant_el` (35 deg): the meteor crosses the view away from it
            const double rel = c.num("radiant_el", 35.0), raz = az + 180.0 + c.num("radiant_daz", 30.0);
            const glm::dvec3 rEnu(std::cos(rad(rel)) * std::sin(rad(raz)), std::cos(rad(rel)) * std::cos(rad(raz)), std::sin(rad(rel)));
            rdE = eciToEcef(glm::dvec3(eci2enuX) * rEnu.x + glm::dvec3(eci2enuY) * rEnu.y + glm::dvec3(eci2enuZ) * rEnu.z, t);
        }
        if (!meteors::buildPath(m, endEcef, rdE, v, M)) throw std::runtime_error("meteors force: the radiant is below that horizon");
        m.shower = s;
        m.seed = 0xF0CEDull + 0x9E3779B97F4A7C15ull * (++meteorForcedCount_);
        m.forced = 1;
        m.band = M <= -8.0 ? 0 : M <= -4.0 ? 1 : 2;
        m.tStart = t + c.num("delay", 0.2);
        meteorForced_.push_back(m);
        r["duration_s"] = m.dur;
        r["begin_h_km"] = m.hB / 1000.0;
        r["end_h_km"] = m.hE / 1000.0;
        r["path_km"] = glm::length(m.B - m.E) / 1000.0;
        r["train_life_s"] = m.trainLife;
        r["shower"] = meteors::shower(s).id;
        char buf[200];
        snprintf(buf, sizeof(buf), "forced %s (abs mag %.1f, %.0f km/s, %.2f s, train %.1f s) toward az %.0f el %.0f", kind.c_str(), M, vk,
                 m.dur, m.trainLife, az, el);
        r["message"] = buf;
        return harness::Status::Done;
    }
    if (sub == "go")
    {
        int s = c.has("shower") ? meteors::showerByName(c.str("shower").c_str()) : -1;
        double bestT = 1e300;
        int bestS = -1;
        for (int q = 0; q < meteors::kSporadic; ++q)
        {
            if (s >= 0 && q != s) continue;
            double t0, t1;
            double tp = meteors::peakAfter(q, t - 2.0 * 86400.0, &t0, &t1);
            if (t1 < t) tp = meteors::peakAfter(q, tp + 300.0 * 86400.0, &t0, &t1); // this year's is over: next year's
            if (s < 0 && meteors::shower(q).zhr < 15.0) continue;
            if (tp < bestT)
            {
                bestT = tp;
                bestS = q;
            }
        }
        if (bestS < 0) throw std::runtime_error("meteors go: no shower");
        double t0, t1;
        double tp = meteors::peakAfter(bestS, t - 2.0 * 86400.0, &t0, &t1);
        if (t1 < t) tp = meteors::peakAfter(bestS, tp + 300.0 * 86400.0, &t0, &t1);
        std::string msg;
        if (!meteorWatchShower(bestS, t0, t1, tp, msg)) throw std::runtime_error("meteors go: " + msg);
        r["shower"] = meteors::shower(bestS).id;
        r["peak_utc"] = fmtUtcReadable(tp);
        r["lat_deg"] = obsLatDeg;
        r["lon_deg"] = obsLonDeg;
        r["az_deg"] = camera.azDeg;
        r["el_deg"] = camera.elDeg;
        r["message"] = msg;
        return harness::Status::Done;
    }
    throw std::runtime_error("meteors: state | count [minutes=] [lm=] [shower=] | force [kind=meteor|fireball|bolide] [shower=] [mag=] [az=] "
                             "[el=] [v=] [delay=] [radiant_el=] | go [shower=]");
}
