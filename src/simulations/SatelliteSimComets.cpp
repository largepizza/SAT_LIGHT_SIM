// SatelliteSimComets.cpp — comets in the sky (2026-10-27; CLAUDE.md "Comets"; Comets.h for the orbits).
//
// Per frame (cometsUpdate, recordCompute after updatePlanets): the catalog near now (the Great Comet of 2037 + the
// hashed minor comets), each one's geometry from the observer (heliocentric ecliptic, the planets' Earth + the
// observer's offset: a comet 560,000 km away shows a degree of parallax), its magnitude, and for the brightest few
// the tail and coma as triangle grids of sky directions, each vertex carrying its surface brightness in the Milky
// Way's anchor units (lum 0.05 = 21 mag/arcsec^2, linear in physical surface brightness). The head is a point
// source: cometsFillHeads appends it to kFireBuf (kesslerFillDust's finish), so the point, bloom and glare draw it.
// The tail pass (comet_tail.vert/.frag) draws the grids after the sky background, at infinity behind every surface,
// hidden by cloud, gated against the sky's own surface brightness per vertex (darksky.glsl: light pollution, twilight,
// daylight with the aureole round the Sun, moonlight) and dimmed by the line of sight's extinction.
#include "../FrameProfile.h"
#include "../Harness.h"
#include "Comets.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"

#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cctype>
#include <cstring>

glm::dvec3 skyEarthHelioAt(double t);     // SatelliteSim.cpp
std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kAuM = 1.495978707e11;
constexpr double kDay = 86400.0;
constexpr double kLumPerFluxSr = 2.952e-4;     // lum (0.05 = 21 mag/arcsec^2) per (mag-0 flux per steradian)
constexpr double kPsfRad = 2.9e-4;             // ~1 arcmin: the inner coma the eye sees as the point
constexpr int kMaxTails = 3;                   // comets with a drawn tail / coma
constexpr uint32_t kMaxPatches = 16;
constexpr uint32_t kMaxVerts = 12288;

double deg(double r) { return r * (180.0 / kPi); }
double rad(double d) { return d * (kPi / 180.0); }
double obliquity(double t) { return (23.439 - 0.0000004 * (t / kDay)) * (kPi / 180.0); }
glm::dvec3 eclToEci(const glm::dvec3 &v, double eps)
{
    return glm::dvec3(v.x, v.y * std::cos(eps) - v.z * std::sin(eps), v.y * std::sin(eps) + v.z * std::cos(eps));
}
glm::dvec3 eciToEcl(const glm::dvec3 &v, double eps)
{
    return glm::dvec3(v.x, v.y * std::cos(eps) + v.z * std::sin(eps), -v.y * std::sin(eps) + v.z * std::cos(eps));
}
glm::vec3 lumNormColor(glm::vec3 c) { return c / std::max(0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b, 1e-3f); }

struct GpuCometPatch
{
    uint32_t off, nu, nv, kind; // kind 0 dust, 1 ion, 2 coma
    glm::vec4 col;              // rgb (luminance 1); w = kind parameter (dust striae strength; ion streamer phase;
                                // coma: the point's share radius rho0)
};
struct GpuCometVert
{
    glm::vec4 dirLum; // ENU direction (unnormalised on the coma's tangent plane); w = lum
    glm::vec4 uv;     // the fragment's coordinates (dust: emission day rel. perihelion, ln-beta share; ion: s, v; coma: x, y)
};
constexpr size_t kHeaderBytes = sizeof(GpuCometPatch) * kMaxPatches;
} // namespace

// The comet's geometry from the observer at t (sim's frame): heliocentric state, geocentric vector (ecliptic, AU).
void SatelliteSim::cometGeometry(const comet::Elements &el, double t, const glm::dvec3 &obsHelio, CometGeom &g) const
{
    const comet::State s = comet::stateAt(el, t);
    g.s = s;
    g.r = glm::length(s.r);
    const glm::dvec3 d = s.r - obsHelio;
    g.delta = glm::length(d);
    g.toComet = d / g.delta;
    g.phaseDeg = deg(std::acos(std::clamp(glm::dot(-s.r / g.r, -g.toComet), -1.0, 1.0)));
    g.elongDeg = deg(std::acos(std::clamp(glm::dot(g.toComet, -glm::normalize(obsHelio)), -1.0, 1.0)));
    g.mag = comet::magnitude(el, g.r, g.delta, g.phaseDeg);
    // The visible coma: grows as the comet nears the Sun (r^-0.5, within limits), seen at the comet's distance.
    const double rk = el.comaKm * std::clamp(1.0 / std::sqrt(std::max(g.r, 0.05)), 0.5, 3.0);
    g.comaRad = std::min(std::atan(rk / (g.delta * comet::kAuKm)), rad(35.0));
    // The ion tail: ~0.1 AU for a bright comet, longer near the Sun and with more gas.
    g.ionLenAU = 0.06 * std::sqrt(el.gas) * std::clamp(1.0 / std::sqrt(std::max(g.r, 0.1)), 0.7, 2.5) *
                 std::clamp(std::pow(10.0, -0.1 * (el.H - 6.0)), 0.3, 1.5);
    const glm::dvec3 tip = s.r + comet::ionAxis(s) * g.ionLenAU - obsHelio;
    g.tailDeg = deg(std::acos(std::clamp(glm::dot(glm::normalize(tip), g.toComet), -1.0, 1.0)));
}

glm::dvec3 SatelliteSim::cometObserverHelio(double t) const
{
    return skyEarthHelioAt(t) + eciToEcl(glm::dvec3(obsECI), obliquity(t)) / kAuM;
}

// An ecliptic direction (from the observer) into ENU.
glm::vec3 SatelliteSim::cometEnu(const glm::dvec3 &dirEcl, double t) const
{
    const glm::dvec3 e = eclToEci(dirEcl, obliquity(t));
    return glm::normalize(glm::vec3((float)glm::dot(e, glm::dvec3(eci2enuX)), (float)glm::dot(e, glm::dvec3(eci2enuY)),
                                    (float)glm::dot(e, glm::dvec3(eci2enuZ))));
}

glm::vec3 SatelliteSim::cometHeadEnu(int i) const
{
    if (i < 0 || i >= (int)comets_.size()) return glm::vec3(0, 1, 0);
    const double t = (double)simDayJ2000 * kDay + simSecInDay;
    CometGeom g;
    cometGeometry(comets_[i], t, cometObserverHelio(t), g);
    return cometEnu(g.toComet, t);
}
// A point 40% down the ion tail (the Watch frames the head and this).
glm::vec3 SatelliteSim::cometTailEnu(int i) const
{
    if (i < 0 || i >= (int)comets_.size()) return glm::vec3(0, 1, 0);
    // the drawn dust tail's far end, 60% of the way out (this frame's, when it has one)
    for (const CometNow &cn : cometNow_)
        if (cn.idx == i && cn.tailSpanDeg > 1.0f)
            return glm::normalize(glm::mix(cn.headEnuDrawn, cn.tailAimEnu, 0.6f));
    const double t = (double)simDayJ2000 * kDay + simSecInDay;
    const glm::dvec3 oh = cometObserverHelio(t);
    CometGeom g;
    cometGeometry(comets_[i], t, oh, g);
    const glm::dvec3 p = g.s.r + comet::ionAxis(g.s) * (0.4 * g.ionLenAU) - oh;
    return cometEnu(glm::normalize(p), t);
}
int SatelliteSim::cometIndexByName(const char *name) const
{
    for (size_t i = 0; i < comets_.size(); ++i)
        if (std::strcmp(comets_[i].name, name) == 0) return (int)i;
    return -1;
}

void SatelliteSim::cometsRefreshCatalog(double t, bool force)
{
    if (!force && std::abs(t - cometsCatalogT) < 20.0 * kDay) return;
    cometsCatalogT = t;
    std::vector<comet::Elements> all;
    comet::catalogNear(t, 1.2, all);
    const std::string selName = selectedCometIndex >= 0 && selectedCometIndex < (int)comets_.size() ? comets_[selectedCometIndex].name : "";
    comets_.clear();
    for (const comet::Elements &e : all)
        if (e.great || cometsMinor) comets_.push_back(e);
    selectedCometIndex = selName.empty() ? -1 : cometIndexByName(selName.c_str());
}

// ── Per frame ───────────────────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::cometsUpdate()
{
    FPROF_SCOPE("cometsUpdate");
    const auto c0 = std::chrono::steady_clock::now();
    cometNow_.clear();
    cometPatchCount = 0;
    cometVertCount = 0;
    if (selectedSatIndex >= 0 || selectedPlanetIndex >= 0) selectedCometIndex = -1; // (another selection took over)
    if (!cometsEnabled)
    {
        comets_.clear();
        cometsCatalogT = -1e300;
        selectedCometIndex = -1;
        return;
    }
    const double t = (double)simDayJ2000 * kDay + simSecInDay;
    cometsRefreshCatalog(t, false);
    const glm::dvec3 oh = cometObserverHelio(t);
    const double eps = obliquity(t);

    // The sky's suppression terms for the head (as sat_flare.comp does for satellites: a bright head shows by day).
    const glm::vec3 obsN = glm::normalize(obsECI);
    const float sunElev = glm::dot(sunDirECI, obsN);
    const float tD = glm::clamp((sunElev + 0.05f) / 0.39f, 0.0f, 1.0f);
    const float dayBright = tD * tD;
    const float obsH = std::max(0.0f, glm::length(obsECI) - (float)satphot::kEarthRadiusM);
    const float atmFrac = 1.0f - glm::clamp((obsH - 40000.0f) / 60000.0f, 0.0f, 1.0f);
    const float moonElev = glm::dot(moonDirECI, obsN);
    const float tm = glm::clamp(moonElev / 0.5f, 0.0f, 1.0f);
    const float moonBright = tm * tm * moonDirENU.w;
    cometMoonBright = moonBright;
    cometAtmFrac = atmFrac;

    for (int i = 0; i < (int)comets_.size(); ++i)
    {
        const comet::Elements &el = comets_[i];
        CometNow cn;
        cn.idx = i;
        cometGeometry(el, t, oh, cn.g);
        if (cn.g.mag > 14.0) continue;
        cn.headEnu = cometEnu(cn.g.toComet, t);
        // The point: the inner coma within ~1 arcmin of a 1/rho coma (all of it while the coma is smaller).
        cn.pointShare = std::clamp(kPsfRad / std::max(cn.g.comaRad, 1e-9), 0.003, 1.0);
        cn.magPoint = cn.g.mag - 2.5 * std::log10(cn.pointShare);
        // Refraction, as the planets (a source at infinity): placed where the air bends it.
        glm::vec3 enuR = cn.headEnu;
        float aoa = 0.0f, scint = -1.0f;
        bool below = enuR.z < -0.3f;
        if (refrTable.valid && enuR.z > -0.3f) below = !refractEnu(enuR, aoa, scint);
        cn.headEnuDrawn = enuR;
        cn.below = below;
        const double ext = atmExtinctionMag(glm::dvec3(obsECI), eclToEci(cn.g.toComet, eps), cn.g.delta * kAuM,
                                            (double)extinctionCoeff);
        cn.extMag = ext;
        cn.magSeen = cn.magPoint + ext;
        // Light pollution dims the head as it dims a planet (the dome, updateLightPollutionDome).
        float bearing = std::atan2(enuR.x, enuR.y);
        if (bearing < 0.0f) bearing += 6.2831853f;
        const float secF = bearing * (float(kNumLightSectors) / 6.2831853f) - 0.5f;
        const int sec0 = (int)std::floor(secF);
        const float fr = secF - (float)sec0;
        const int s0 = ((sec0 % kNumLightSectors) + kNumLightSectors) % kNumLightSectors, s1 = (s0 + 1) % kNumLightSectors;
        const float domeAz = glm::mix(lightDomeAz[s0], lightDomeAz[s1], fr);
        const float elevFalloff = 0.35f / (std::max(enuR.z, 0.0f) + 0.35f);
        const float domeVal = glm::clamp(domeAz * (0.4f + 0.6f * elevFalloff), 0.0f, 1.0f);
        double flare = 0.008 * std::pow(10.0, 0.4 * (6.0 - cn.magSeen));
        flare /= 1.0 + (dayBright * daySuppression + moonBright * moonSuppression) * atmFrac;
        flare *= 1.0 - domeVal * 0.85f;
        cn.flare = below ? 0.0 : std::min(flare, 2.0e4);
        cometNow_.push_back(cn);
    }
    // The tails: the brightest few (by the head's magnitude) bright enough to show.
    std::vector<int> ord(cometNow_.size());
    for (size_t k = 0; k < ord.size(); ++k) ord[k] = (int)k;
    std::sort(ord.begin(), ord.end(), [&](int a, int b) { return cometNow_[a].g.mag < cometNow_[b].g.mag; });
    int nT = 0;
    for (int k : ord)
    {
        if (nT >= kMaxTails) break;
        CometNow &cn = cometNow_[k];
        if (cn.g.mag > 9.0 || !cometTailsEnabled) continue;
        // By day on the ground (the Sun > ~6 deg up) the sky is ~4-6 mag/arcsec^2: no tail survives the gate, so
        // its geometry is not built.
        if (sunDirENU.w > 0.1f && atmFrac > 0.9f) continue;
        cn.tailSlot = nT++;
    }
    cometsBuildTails(t, oh);
    cometCpuMs = 0.9 * cometCpuMs + 0.1 * std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
}

// The dust grids: (re)built off the frame thread when the tail's shape has moved on; the first one on the frame.
void SatelliteSim::cometsBuildTails(double t, const glm::dvec3 &oh)
{
    // Slots follow the comets by name.
    for (CometTailCache &c : cometTails_) c.used = false;
    GpuCometPatch *patches = cometMapped ? static_cast<GpuCometPatch *>(cometMapped) : nullptr;
    GpuCometVert *verts = cometMapped ? reinterpret_cast<GpuCometVert *>(static_cast<char *>(cometMapped) + kHeaderBytes) : nullptr;
    const double eps = obliquity(t);
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    auto enuOf = [&](const glm::dvec3 &eclDir) {
        const glm::dvec3 e = eclToEci(eclDir, eps);
        return glm::dvec3(glm::dot(e, ex), glm::dot(e, ey), glm::dot(e, ez));
    };
    auto addPatch = [&](uint32_t nu, uint32_t nv, uint32_t kind, glm::vec3 col, float param) -> GpuCometVert * {
        if (!patches || cometPatchCount >= kMaxPatches || cometVertCount + nu * nv > kMaxVerts) return nullptr;
        patches[cometPatchCount] = GpuCometPatch{cometVertCount, nu, nv, kind, glm::vec4(lumNormColor(col), param)};
        cometPatchCount++;
        GpuCometVert *v = verts + cometVertCount;
        cometVertCount += nu * nv;
        return v;
    };
    // The Jacobian of a grid of directions: the solid angle per unit (u, v) at each vertex, central differences,
    // with a floor for the sheet's thickness seen edge-on (thickRad(j, i): its angular thickness there).
    auto solidAngle = [&](const std::vector<glm::dvec3> &d, int nu, int nv, const std::vector<double> &uu,
                          const std::vector<double> &vv, const std::vector<double> &thickRad, std::vector<double> &out) {
        out.assign((size_t)nu * nv, 1e-12);
        for (int j = 0; j < nv; ++j)
            for (int i = 0; i < nu; ++i)
            {
                const int i0 = std::max(i - 1, 0), i1 = std::min(i + 1, nu - 1), j0 = std::max(j - 1, 0), j1 = std::min(j + 1, nv - 1);
                const glm::dvec3 du = (d[(size_t)j * nu + i1] - d[(size_t)j * nu + i0]) / std::max(uu[i1] - uu[i0], 1e-12);
                const glm::dvec3 dv = (d[(size_t)j1 * nu + i] - d[(size_t)j0 * nu + i]) / std::max(vv[j1] - vv[j0], 1e-12);
                const double cr = glm::length(glm::cross(du, dv));
                const double su = std::max(uu[i1] - uu[i0], 1e-12) / std::max(i1 - i0, 1);
                const double sv = std::max(vv[j1] - vv[j0], 1e-12) / std::max(j1 - j0, 1);
                const double fl = thickRad[(size_t)j * nu + i] * std::max(glm::length(du) / sv, glm::length(dv) / su);
                out[(size_t)j * nu + i] = std::sqrt(cr * cr + fl * fl);
            }
    };

    for (CometNow &cn : cometNow_)
    {
        if (cn.tailSlot < 0) continue;
        const comet::Elements &el = comets_[cn.idx];
        // the cache slot for this comet
        CometTailCache *slot = nullptr;
        for (CometTailCache &c : cometTails_)
            if (c.name == el.name) slot = &c;
        if (!slot)
        {
            for (CometTailCache &c : cometTails_)
                if (!c.used && !c.job.valid()) { slot = &c; break; }
            if (!slot) { cometTails_.emplace_back(); slot = &cometTails_.back(); }
            slot->name = el.name;
            slot->grid = comet::DustGrid();
        }
        slot->used = true;
        const int nB = el.great ? 22 : 12, nTau = el.great ? 96 : 40;
        const double tauMax = el.great ? 45.0 : 25.0;
        // Rebuild cadence: faster near the Sun, where the shape changes within hours.
        const double rebuildS = cn.g.r < 0.15 ? 120.0 : cn.g.r < 0.5 ? 600.0 : 1800.0;
        if (slot->job.valid() && slot->job.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
        {
            slot->grid = slot->job.get();
            cometTailBuilds++;
        }
        const bool stale = slot->grid.nTau == 0 || std::abs(t - slot->grid.tBuilt) > rebuildS;
        const bool far = slot->grid.nTau == 0 || std::abs(t - slot->grid.tBuilt) > 2.0 * kDay;
        if (stale && !slot->job.valid())
        {
            if (far)
            {
                comet::buildDustGrid(el, t, nB, nTau, tauMax, slot->grid); // a jump: on the frame (a few ms)
                cometTailBuilds++;
            }
            else
            {
                const comet::Elements elc = el;
                slot->job = std::async(std::launch::async, [elc, t, nB, nTau, tauMax]() {
                    comet::DustGrid g;
                    comet::buildDustGrid(elc, t, nB, nTau, tauMax, g);
                    return g;
                });
            }
        }
        const comet::DustGrid &G = slot->grid;
        if (G.nTau == 0) continue;
        const glm::dvec3 nuc = cn.g.s.r; // the nucleus now (the grid rides with it between rebuilds)
        const double Fhead0 = std::pow(10.0, -0.4 * el.H);
        float peak = 0.0f;

        // ── Dust: the syndyne / synchrone sheet ──────────────────────────────────────────────────────────────
        {
            const int nu = G.nBeta, nv = G.nTau;
            std::vector<glm::dvec3> d((size_t)nu * nv);
            std::vector<double> uu(nu), vv(nv), thick((size_t)nu * nv), flux((size_t)nu * nv);
            const double lb0 = std::log(G.beta.front()), lb1 = std::log(G.beta.back());
            for (int i = 0; i < nu; ++i) uu[i] = std::log(G.beta[i]);
            for (int j = 0; j < nv; ++j) vv[j] = G.tauDays[j];
            // Size distribution per ln(beta): cross-section ~ beta^0.5 (dN/da ~ a^-3.5), the sub-micron end (beta
            // past ~0.5) scattering less; normalised over the grid.
            std::vector<double> wB(nu);
            double wSum = 0.0;
            for (int i = 0; i < nu; ++i)
            {
                const double b = G.beta[i];
                wB[i] = std::sqrt(b) * std::exp(-(b / 0.55) * (b / 0.55));
                wSum += wB[i] * (i > 0 && i < nu - 1 ? 0.5 * (uu[i + 1] - uu[i - 1]) : 0.5 * (uu[1] - uu[0]));
            }
            const double Kd = 0.25 * el.dust * Fhead0 * (double)cometTailGain / std::max(wSum, 1e-9);
            for (int j = 0; j < nv; ++j)
            {
                const double tau = G.tauDays[j];
                const double fadeT = 1.0 - glm::smoothstep(0.55 * G.tauDays.back(), G.tauDays.back(), tau);
                for (int i = 0; i < nu; ++i)
                {
                    const size_t q = (size_t)j * nu + i;
                    const glm::dvec3 P = nuc + G.relPos[q];
                    const glm::dvec3 D = P - oh;
                    const double dp = glm::length(D);
                    d[q] = enuOf(D / dp);
                    const double rp = std::max(glm::length(P), 1e-4);
                    const double scat = deg(std::acos(std::clamp(glm::dot(P / rp, -D / dp), -1.0, 1.0)));
                    const double fadeB = glm::smoothstep(0.0, 0.25, (uu[i] - lb0) / (lb1 - lb0)) *
                                         (1.0 - glm::smoothstep(0.5, 1.0, (uu[i] - lb0) / (lb1 - lb0)));
                    flux[q] = Kd * G.emitRate[j] * wB[i] * comet::dustPhase(scat) / (rp * rp * dp * dp) * fadeT * fadeB;
                    // the sheet's thickness: the grains' ejection speeds (~0.5 km/s x sqrt(beta)) x their age
                    const double th = 2e-5 + 2.9e-4 * std::sqrt(G.beta[i]) * tau;
                    thick[q] = th / dp;
                }
            }
            std::vector<double> om;
            solidAngle(d, nu, nv, uu, vv, thick, om);
            const glm::vec3 hd = cn.headEnuDrawn;
            if (GpuCometVert *v = addPatch((uint32_t)nu, (uint32_t)nv, 0, glm::vec3(1.0f, 0.93f, 0.80f), el.great ? 1.0f : 0.35f))
                for (int j = 0; j < nv; ++j)
                    for (int i = 0; i < nu; ++i)
                    {
                        const size_t q = (size_t)j * nu + i;
                        const float lum = (float)std::min(kLumPerFluxSr * flux[q] / om[q], 400.0);
                        peak = std::max(peak, lum);
                        if (lum > 0.004f)
                        {
                            const float a = glm::degrees(std::acos(std::clamp(glm::dot(hd, glm::vec3(d[q])), -1.0f, 1.0f)));
                            if (a > cn.tailSpanDeg)
                            {
                                cn.tailSpanDeg = a;
                                cn.tailAimEnu = glm::vec3(d[q]);
                            }
                        }
                        const double tEmitRel = (t - G.tauDays[j] * kDay - G.tPeri) / kDay;
                        v[q] = GpuCometVert{glm::vec4(glm::vec3(d[q]), lum),
                                            glm::vec4((float)tEmitRel, (float)((uu[i] - lb0) / (lb1 - lb0)), (float)G.tauDays[j], 0.0f)};
                    }
        }

        // ── Ion tail: a straight ribbon along the solar wind as the comet sees it ────────────────────────────
        {
            const int nu = 5, nv = 36;
            const glm::dvec3 ax = comet::ionAxis(cn.g.s);
            const double L = cn.g.ionLenAU;
            std::vector<glm::dvec3> d((size_t)nu * nv);
            std::vector<double> uu(nu), vv(nv), thick((size_t)nu * nv), flux((size_t)nu * nv);
            const double vAcross = 2.2; // v = +-1 is +-2.2 sigma of the ribbon's Gaussian
            for (int i = 0; i < nu; ++i) uu[i] = -1.0 + 2.0 * i / (nu - 1);
            for (int j = 0; j < nv; ++j)
            {
                const double s = std::pow((double)j / (nv - 1), 1.6);
                vv[j] = s;
            }
            // Gas tail flux ~ 0.12 x the coma's (no forward surge: ions emit), along exp(-s / 0.35).
            const double Fion = 0.12 * el.gas * Fhead0 * std::pow(std::sqrt(cn.g.r * cn.g.r + el.rSat * el.rSat), -el.n) *
                                (double)cometTailGain;
            const double lamN = 0.35 * (1.0 - std::exp(-1.0 / 0.35));
            for (int j = 0; j < nv; ++j)
            {
                const double s = vv[j];
                const glm::dvec3 Q = nuc + ax * (s * L);
                glm::dvec3 c = glm::cross(ax, Q - oh);
                if (glm::length(c) < 1e-12) c = glm::cross(ax, glm::dvec3(0, 0, 1));
                c = glm::normalize(c);
                const double w = 0.0015 + 0.06 * s * L; // sigma, AU
                const double lam = std::exp(-s / 0.35) / lamN * (1.0 - glm::smoothstep(0.75, 1.0, s));
                for (int i = 0; i < nu; ++i)
                {
                    const size_t q = (size_t)j * nu + i;
                    const glm::dvec3 P = Q + c * (uu[i] * vAcross * w);
                    const glm::dvec3 D = P - oh;
                    const double dp = glm::length(D);
                    d[q] = enuOf(D / dp);
                    flux[q] = Fion * lam * (vAcross / std::sqrt(2.0 * kPi)) / (dp * dp);
                    thick[q] = w / dp;
                }
            }
            std::vector<double> om;
            solidAngle(d, nu, nv, uu, vv, thick, om);
            // the streamers' phase drifts with sim time (hours)
            const float phase = (float)std::fmod(t / 5400.0, 1000.0);
            if (GpuCometVert *v = addPatch((uint32_t)nu, (uint32_t)nv, 1, glm::vec3(0.45f, 0.66f, 1.0f), phase))
                for (int j = 0; j < nv; ++j)
                    for (int i = 0; i < nu; ++i)
                    {
                        const size_t q = (size_t)j * nu + i;
                        const float lum = (float)std::min(kLumPerFluxSr * flux[q] / om[q], 400.0);
                        peak = std::max(peak, lum);
                        v[q] = GpuCometVert{glm::vec4(glm::vec3(d[q]), lum), glm::vec4((float)vv[j], (float)uu[i], 0.0f, 0.0f)};
                    }
        }

        // ── Coma: a 1/rho glow on the tangent plane round the head ───────────────────────────────────────────
        if (cn.g.comaRad > 1.5 * kPsfRad)
        {
            const int n = 13;
            const glm::dvec3 h = glm::dvec3(cn.headEnuDrawn);
            glm::dvec3 e1 = glm::cross(h, glm::dvec3(0, 0, 1));
            if (glm::length(e1) < 1e-6) e1 = glm::cross(h, glm::dvec3(0, 1, 0));
            e1 = glm::normalize(e1);
            const glm::dvec3 e2 = glm::cross(e1, h);
            const double tc = std::tan(cn.g.comaRad);
            const double Fcoma = std::pow(10.0, -0.4 * cn.g.mag) * (1.0 - cn.pointShare);
            const double L0 = kLumPerFluxSr * Fcoma / (3.351 * cn.g.comaRad * cn.g.comaRad);
            const float rho0 = (float)std::max(kPsfRad / cn.g.comaRad, 0.003);
            // C2 green for a gas-rich coma, whiter with dust
            const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.95f, 0.85f), glm::vec3(0.55f, 1.0f, 0.62f),
                                           (float)std::clamp(el.gas / (el.gas + 0.6 * el.dust), 0.0, 1.0));
            if (GpuCometVert *v = addPatch(n, n, 2, col, rho0))
                for (int j = 0; j < n; ++j)
                    for (int i = 0; i < n; ++i)
                    {
                        const double x = -1.2 + 2.4 * i / (n - 1), y = -1.2 + 2.4 * j / (n - 1);
                        const glm::dvec3 P = h + (e1 * x + e2 * y) * tc;
                        v[(size_t)j * n + i] = GpuCometVert{glm::vec4(glm::vec3(P), (float)std::min(L0, 1e6)),
                                                            glm::vec4((float)x, (float)y, 0.0f, 0.0f)};
                    }
            peak = std::max(peak, (float)(L0 / rho0));
        }
        cn.peakLum = peak;
    }
    // drop the slots no comet uses (after their jobs end)
    cometTails_.erase(std::remove_if(cometTails_.begin(), cometTails_.end(),
                                     [](CometTailCache &c) {
                                         if (c.used) return false;
                                         if (c.job.valid()) c.job.wait();
                                         return true;
                                     }),
                      cometTails_.end());
    if (cometMapped && cometPatchCount > 0 && ctx_)
        ctx_->mirrorDirty(cometMapped, 0, kHeaderBytes + sizeof(GpuCometVert) * cometVertCount);
}

// kesslerFillDust's finish: the heads into kFireBuf (point, bloom, glare).
void SatelliteSim::cometsFillHeads()
{
    if (!kFireMapped) return;
    GpuFireball *fo = reinterpret_cast<GpuFireball *>(static_cast<char *>(kFireMapped) + 16);
    for (CometNow &cn : cometNow_)
    {
        cn.drawnHead = false;
        if (cn.flare < 2e-4 || kFireCount >= kFireballMax) continue;
        const comet::Elements &el = comets_[cn.idx];
        const glm::vec3 col = glm::mix(glm::vec3(1.0f, 0.97f, 0.9f), glm::vec3(0.8f, 1.0f, 0.82f),
                                       (float)std::clamp(0.35 * el.gas / (el.gas + el.dust), 0.0, 0.5));
        // range past 1e9 m: drawn at infinity (behind every surface, every cloud in front of it)
        fo[kFireCount++] = {glm::vec4(cn.headEnuDrawn, (float)cn.flare), glm::vec4(col, 2.0e9f)};
        cn.drawnHead = true;
    }
    static_cast<uint32_t *>(kFireMapped)[0] = kFireCount;
}

// ── GPU: the tail pass ──────────────────────────────────────────────────────────────────────────────────────────
struct CometPC
{
    glm::mat4 skyView;
    float fovYRad, aspect, screenW, screenH;
    glm::vec4 sunDirENU;
    glm::vec4 obs;  // x observer radius (m), y extinction k, z atmFrac, w moonBright
    glm::vec4 misc; // x exposure x gain, y manual terrain test, z the Sun above the horizon (0/1), w glare gate
};
static_assert(sizeof(CometPC) == 128, "CometPC must be 128 bytes");

void SatelliteSim::cometsInit(VulkanContext &ctx)
{
    const VkDeviceSize sz = kHeaderBytes + sizeof(GpuCometVert) * kMaxVerts;
    cometMapped = createMirrored(ctx, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cometBuf, cometMem, false, "comets");
    std::memset(cometMapped, 0, (size_t)sz);
    VkDescriptorSetLayoutBinding b[6] = {};
    const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, nullptr};
    b[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &cometDescLayout);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &cometDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cometDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &cometDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &cometDescSet);
    cometsWriteDescriptors(ctx);

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CometPC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &cometDescLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &pl, nullptr, &cometPipeLayout);

    VkShaderModule vert = ctx.loadShader("shaders/comet_tail.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/comet_tail.frag.spv");
    VkPipelineShaderStageCreateInfo st[2] = {};
    st[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    st[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};
    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.scissorCount = 1;
    VkDynamicState dyn[2] = {VK_DYNAMIC_STATE_VIEWPORT, VK_DYNAMIC_STATE_SCISSOR};
    VkPipelineDynamicStateCreateInfo dsi{VK_STRUCTURE_TYPE_PIPELINE_DYNAMIC_STATE_CREATE_INFO};
    dsi.dynamicStateCount = 2;
    dsi.pDynamicStates = dyn;
    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;
    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE; // at infinity (kDepthFar): every surface hides it, as the stars
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;
    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = st;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rast;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.pDynamicState = &dsi;
    ci.layout = cometPipeLayout;
    ci.renderPass = ctx.renderPass;
    ci.subpass = 0;
    if (vkCreateGraphicsPipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &cometPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create the comet tail pipeline");
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

void SatelliteSim::cometsWriteDescriptors(VulkanContext &ctx)
{
    if (!cometDescSet) return;
    VkDescriptorBufferInfo vb{cometBuf, 0, VK_WHOLE_SIZE}, ub{cloudParamsBuf, 0, VK_WHOLE_SIZE}, db{lightDomeBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo ca{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cbi{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sd{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[6] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &vb, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ca, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cbi, nullptr, nullptr};
    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sd, nullptr, nullptr};
    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ub, nullptr};
    w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &db, nullptr};
    frameSafeUpdateDescriptorSets(ctx.device, 6, w, 0, nullptr);
}

void SatelliteSim::cometsRecordDraw(VkCommandBuffer cmd, VulkanContext &ctx, const PointDrawPC &ppc)
{
    if (!cometPipeline || cometPatchCount == 0) return;
    // Skip the draw when nothing could show: every comet's brightest vertex far under the sky behind it (a coarse
    // CPU test against the day sky; the shader's gate does the rest).
    float peak = 0.0f;
    for (const CometNow &cn : cometNow_)
        if (cn.tailSlot >= 0) peak = std::max(peak, cn.peakLum);
    const float dayK = glm::clamp((sunDirENU.w + 0.02f) / 0.3f, 0.0f, 1.0f) * cometAtmFrac;
    if (peak * (1.0f - dayK) + peak * dayK * 1e-4f < 1e-5f) return;
    CometPC pc{};
    pc.skyView = ppc.skyView;
    pc.fovYRad = ppc.fovYRad;
    pc.aspect = ppc.aspect;
    pc.screenW = ppc.screenSizePx.x;
    pc.screenH = ppc.screenSizePx.y;
    pc.sunDirENU = sunDirENU;
    pc.obs = glm::vec4(glm::length(obsECI), extinctionCoeff, cometAtmFrac, cometMoonBright);
    // the Sun above the apparent horizon (its aureole and glare), from anywhere
    const float obsR = glm::length(obsECI);
    const float limbZ = obsR > (float)satphot::kEarthRadiusM + 1.0f
                            ? -std::sqrt(std::max(0.0f, 1.0f - ((float)satphot::kEarthRadiusM / obsR) * ((float)satphot::kEarthRadiusM / obsR)))
                            : 0.0f;
    pc.misc = glm::vec4(cometTailGainDisplay, ppc.manualTerrainTest, sunDirENU.w > limbZ ? 1.0f : 0.0f,
                        glm::mix(skyGlareEased, 1.0f, cometAtmFrac));
    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometPipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometPipeLayout, 0, 1, &cometDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, cometPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    const GpuCometPatch *patches = static_cast<const GpuCometPatch *>(cometMapped);
    for (uint32_t p = 0; p < cometPatchCount; ++p)
        vkCmdDraw(cmd, (patches[p].nu - 1) * (patches[p].nv - 1) * 6, 1, 0, p);
    cometDrawsLast = cometPatchCount;
}

void SatelliteSim::cometsDestroy(VkDevice device)
{
    for (CometTailCache &c : cometTails_)
        if (c.job.valid()) c.job.wait();
    cometTails_.clear();
    if (cometPipeline) vkDestroyPipeline(device, cometPipeline, nullptr);
    if (cometPipeLayout) vkDestroyPipelineLayout(device, cometPipeLayout, nullptr);
    if (cometDescPool) vkDestroyDescriptorPool(device, cometDescPool, nullptr);
    if (cometDescLayout) vkDestroyDescriptorSetLayout(device, cometDescLayout, nullptr);
    cometPipeline = VK_NULL_HANDLE;
    cometPipeLayout = VK_NULL_HANDLE;
    cometDescPool = VK_NULL_HANDLE;
    cometDescLayout = VK_NULL_HANDLE;
    cometDescSet = VK_NULL_HANDLE;
    if (cometBuf && ctx_) destroyMirrored(*ctx_, cometBuf, cometMem, cometMapped);
}

// ── Selection ───────────────────────────────────────────────────────────────────────────────────────────────────
int SatelliteSim::pickCometAt(float x, float y, float w, float h)
{
    int best = -1;
    float bestD = 14.0f;
    for (const CometNow &cn : cometNow_)
    {
        if (!cn.drawnHead && cn.tailSlot < 0) continue;
        float sx, sy;
        if (!projectSkyDirToScreen(cn.headEnuDrawn, w, h, sx, sy)) continue;
        const float d = std::hypot(sx - x, sy - y);
        if (d < bestD) { bestD = d; best = cn.idx; }
    }
    return best;
}
void SatelliteSim::formatSelectedCometInfo()
{
    for (auto &l : cometInfoLine) l[0] = '\0';
    if (selectedCometIndex < 0 || selectedCometIndex >= (int)comets_.size()) return;
    const comet::Elements &el = comets_[selectedCometIndex];
    const CometNow *cn = nullptr;
    for (const CometNow &c : cometNow_)
        if (c.idx == selectedCometIndex) cn = &c;
    std::snprintf(cometInfoLine[0], kSelInfoLineLen, "%s", el.name);
    if (!cn) return;
    std::snprintf(cometInfoLine[1], kSelInfoLineLen, "Comet, magnitude %.1f (head %.1f as seen)", cn->g.mag, cn->magSeen);
    std::snprintf(cometInfoLine[2], kSelInfoLineLen, "From Earth %.4f AU (%.0f km)", cn->g.delta, cn->g.delta * comet::kAuKm);
    std::snprintf(cometInfoLine[3], kSelInfoLineLen, "From the Sun %.3f AU, %.1f deg from it in the sky", cn->g.r, cn->g.elongDeg);
    std::snprintf(cometInfoLine[4], kSelInfoLineLen, "Tail %.1f deg, coma %.2f deg", cn->g.tailDeg, deg(cn->g.comaRad));
    const double t = (double)simDayJ2000 * kDay + simSecInDay;
    std::snprintf(cometInfoLine[5], kSelInfoLineLen, "Perihelion %.4f AU, %s (%+.1f d)", el.q, fmtUtcReadable(el.tPeri).substr(0, 10).c_str(),
                  (t - el.tPeri) / kDay);
}

// ── Watch: a dark place where the head and the tail are well up, the camera framing both ─────────────────────────
bool SatelliteSim::cometWatchAt(int idx, double t, double sunMaxDeg, bool fromOrbit, std::string &msg)
{
    if (idx < 0 || idx >= (int)comets_.size()) return false;
    const comet::Elements el = comets_[idx];
    if (followActive) stopFollow();
    stopTrack();
    const double d = std::floor(t / kDay);
    simDayJ2000 = (int64_t)d;
    simSecInDay = t - d * kDay;
    timePaused = false;
    timeScaleIdx = 0;
    timeDir = 1.0f;
    trailClearPending = true;
    skyTaaHistValid = false;
    cv2HistoryValid = false;
    for (double &bb : cv2WxBakedT) bb = -1e30;
    // Geocentric directions (ECI) of the head and the tail's 40% point, and the Sun.
    const glm::dvec3 eh = skyEarthHelioAt(t);
    CometGeom g;
    cometGeometry(el, t, eh, g);
    const double eps = obliquity(t);
    const glm::dvec3 hE = eclToEci(g.toComet, eps);
    const glm::dvec3 tE = eclToEci(glm::normalize(g.s.r + comet::ionAxis(g.s) * (0.4 * g.ionLenAU) - eh), eps);
    const glm::dvec3 sE = glm::normalize(sunDirEciAt(t));
    const double th = earthRotationAngle(t), cth = std::cos(th), sth = std::sin(th);
    auto toEcef = [&](const glm::dvec3 &v) { return glm::dvec3(cth * v.x + sth * v.y, -sth * v.x + cth * v.y, v.z); };
    const glm::dvec3 hF = toEcef(hE), tF = toEcef(tE), sF = toEcef(sE);
    const glm::dvec3 here = glm::normalize(glm::dvec3(obsDir));
    double best = -1e30;
    glm::dvec3 site = here;
    for (double la = -60.0; la <= 66.0; la += 2.0)
        for (double lo = -180.0; lo < 180.0; lo += 3.0)
        {
            const glm::dvec3 u(std::cos(rad(la)) * std::cos(rad(lo)), std::cos(rad(la)) * std::sin(rad(lo)), std::sin(rad(la)));
            const double aH = deg(std::asin(std::clamp(glm::dot(hF, u), -1.0, 1.0)));
            const double aT = deg(std::asin(std::clamp(glm::dot(tF, u), -1.0, 1.0)));
            const double aS = deg(std::asin(std::clamp(glm::dot(sF, u), -1.0, 1.0)));
            // (from orbit the horizon dips ~20 deg: the head may stand below the ground's horizon)
            if (aH < (fromOrbit ? -12.0 : std::clamp(g.elongDeg - 6.0, 2.0, 8.0)) || aS > sunMaxDeg) continue;
            double sc = -std::abs(aH - 30.0) * 0.5 + std::min(aT, 40.0) * 0.6 + 0.4 * std::min(sunMaxDeg - aS, 10.0);
            sc -= deg(std::acos(std::clamp(glm::dot(u, here), -1.0, 1.0))) / 30.0;
            if (sc > best - 20.0) sc -= 20.0 * cloudCoverAtEcef(u);
            if (sc > best) { best = sc; site = u; }
        }
    const bool found = best > -1e29;
    setObserverLatLon(deg(std::asin(std::clamp(site.z, -1.0, 1.0))), deg(std::atan2(site.y, site.x)));
    obsHeightOffset = fromOrbit ? 420000.0f : std::max(0.0f, cpuTerrainHeightM(obsLatDeg, obsLonDeg)) + 2.0f;
    updatePositions(t, 0.0f);
    cometsRefreshCatalog(t, true);
    cometsUpdate();
    idx = cometIndexByName(el.name);
    if (idx < 0) return false;
    selectedSatIndex = -1;
    selectedPlanetIndex = -1;
    selectedCometIndex = idx;
    formatSelectedCometInfo();
    // Frame the head and the tail: aim between them, the field a little wider than the tail.
    const glm::vec3 h = cometHeadEnu(idx), tt = cometTailEnu(idx);
    const glm::vec3 mid = glm::normalize(h + tt);
    float sepDeg = (float)deg(std::acos(std::clamp((double)glm::dot(h, tt), -1.0, 1.0)));
    for (const CometNow &cn : cometNow_)
        if (cn.idx == idx && cn.tailSpanDeg > 1.0f) sepDeg = 0.6f * cn.tailSpanDeg;
    aimCameraAzEl(glm::degrees(std::atan2(mid.x, mid.y)), glm::degrees(std::asin(std::clamp(mid.z, -1.0f, 1.0f))));
    camera.fovYDeg = std::clamp(sepDeg * 1.9f + 6.0f, 10.0f, 110.0f);
    skAimA = sky::kBodyComet0 + idx;
    skAimB = sky::kBodyCometTail0 + idx;
    skAimUntil = t + 6.0 * 3600.0;
    skAimAz = camera.azDeg;
    skAimEl = camera.elDeg;
    char b[260];
    CometGeom gs;
    cometGeometry(el, t, cometObserverHelio(t), gs);
    std::snprintf(b, sizeof(b), "%s, magnitude %.1f, %.0f deg from the Sun, from %.1f%c %.1f%c%s (Sun %.0f deg)%s", el.name, gs.mag,
                  gs.elongDeg, std::abs(obsLatDeg), obsLatDeg >= 0 ? 'N' : 'S', std::abs(obsLonDeg), obsLonDeg >= 0 ? 'E' : 'W',
                  fromOrbit ? " in orbit" : "", glm::degrees(std::asin(std::clamp(sunDirENU.w, -1.0f, 1.0f))),
                  found ? "" : " (no dark place: the comet is near the Sun)");
    msg = b;
    return true;
}

// The comet's display moments over [t0, t1] (daily): the brightest-looking day with the comet >= 20 deg from the
// Sun (score: the head's light and the tail's length), and the stretch around it while it stays bright.
bool SatelliteSim::cometBestWindow(const comet::Elements &el, double t0, double t1, double &tBest, double &w0, double &w1,
                                   double &magBest, double &tailBest) const
{
    double best = -1e30;
    std::vector<double> ts, mags;
    for (double t = t0; t <= t1; t += 0.25 * kDay)
    {
        const glm::dvec3 eh = skyEarthHelioAt(t);
        CometGeom g;
        cometGeometry(el, t, eh, g);
        ts.push_back(t);
        mags.push_back(g.mag);
        if (g.elongDeg < 20.0 || g.mag > 9.0) continue;
        const double sc = -g.mag + 2.5 * std::log10(1.0 + g.tailDeg / 5.0);
        if (sc > best) { best = sc; tBest = t; magBest = g.mag; tailBest = g.tailDeg; }
    }
    if (best < -1e29) return false;
    // refine the closest approach / peak within +-6 h (hourly)
    for (double t = tBest - 6.0 * 3600.0; t <= tBest + 6.0 * 3600.0; t += 1800.0)
    {
        CometGeom g;
        cometGeometry(el, t, skyEarthHelioAt(t), g);
        if (g.elongDeg < 20.0) continue;
        const double sc = -g.mag + 2.5 * std::log10(1.0 + g.tailDeg / 5.0);
        if (sc > best) { best = sc; tBest = t; magBest = g.mag; tailBest = g.tailDeg; }
    }
    const double thr = std::max(magBest + 2.5, std::min(6.5, magBest + 4.0));
    size_t k = 0;
    while (k + 1 < ts.size() && ts[k + 1] <= tBest) ++k;
    size_t a = k, b = k;
    while (a > 0 && mags[a - 1] < thr) --a;
    while (b + 1 < ts.size() && mags[b + 1] < thr) ++b;
    w0 = ts[a];
    w1 = std::max(ts[b] + 0.25 * kDay, tBest + 3600.0);
    return true;
}

// ── Harness: comets [state] | watch [k|name] [days=] [sun_max=] [orbit=1] | list [days=] ─────────────────────
harness::Status SatelliteSim::harnessComets(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    nlohmann::json &r = a.result;
    const std::string sub = c.pos.empty() ? std::string("state") : c.pos[0];
    const double t = (double)simDayJ2000 * kDay + simSecInDay;
    if (sub == "state")
    {
        nlohmann::json arr = nlohmann::json::array();
        for (const CometNow &cn : cometNow_)
        {
            const comet::Elements &el = comets_[cn.idx];
            nlohmann::json o = {{"name", el.name},
                                {"index", cn.idx},
                                {"great", el.great},
                                {"mag", cn.g.mag},
                                {"mag_point", cn.magPoint},
                                {"mag_seen", cn.magSeen},
                                {"extinction_mag", cn.extMag},
                                {"flare", cn.flare},
                                {"head_drawn", cn.drawnHead},
                                {"below_horizon", cn.below},
                                {"r_au", cn.g.r},
                                {"delta_au", cn.g.delta},
                                {"delta_km", cn.g.delta * comet::kAuKm},
                                {"phase_deg", cn.g.phaseDeg},
                                {"elong_deg", cn.g.elongDeg},
                                {"coma_deg", deg(cn.g.comaRad)},
                                {"tail_deg", cn.g.tailDeg},
                                {"ion_len_au", cn.g.ionLenAU},
                                {"tail_slot", cn.tailSlot},
                                {"peak_lum", cn.peakLum},
                                {"days_from_perihelion", (t - el.tPeri) / kDay},
                                {"q_au", el.q},
                                {"e", el.e},
                                {"az_deg", deg(std::atan2((double)cn.headEnuDrawn.x, (double)cn.headEnuDrawn.y))},
                                {"el_deg", deg(std::asin(std::clamp((double)cn.headEnuDrawn.z, -1.0, 1.0)))}};
            arr.push_back(o);
        }
        r["comets"] = arr;
        // the first dust patch's vertex surface brightness across a few rows (lum, tau days) — the shape check
        if (cometMapped && cometPatchCount > 0)
        {
            const GpuCometPatch *pp = static_cast<const GpuCometPatch *>(cometMapped);
            const GpuCometVert *vv = reinterpret_cast<const GpuCometVert *>(static_cast<const char *>(cometMapped) + kHeaderBytes);
            for (uint32_t p = 0; p < cometPatchCount; ++p)
                if (pp[p].kind == 0)
                {
                    nlohmann::json rows = nlohmann::json::array();
                    for (uint32_t j : {pp[p].nv / 8, pp[p].nv / 3, pp[p].nv / 2, (pp[p].nv * 3) / 4})
                    {
                        nlohmann::json row = {{"tau_days", vv[pp[p].off + j * pp[p].nu].uv.z}};
                        nlohmann::json l = nlohmann::json::array();
                        for (uint32_t i = 0; i < pp[p].nu; ++i) l.push_back(vv[pp[p].off + j * pp[p].nu + i].dirLum.w);
                        row["lum"] = l;
                        rows.push_back(row);
                    }
                    r["dust_rows"] = rows;
                    break;
                }
        }
        r["catalog_size"] = comets_.size();
        r["patches"] = cometPatchCount;
        r["verts"] = cometVertCount;
        r["draws_last"] = cometDrawsLast;
        r["tail_builds"] = cometTailBuilds;
        r["cpu_ms"] = cometCpuMs;
        r["enabled"] = cometsEnabled;
        r["tail_gain"] = cometTailGain;
        r["display_gain"] = cometTailGainDisplay;
        r["moon_bright"] = cometMoonBright;
        r["atm_frac"] = cometAtmFrac;
        if (selectedCometIndex >= 0) r["selected"] = comets_[selectedCometIndex].name;
        return harness::Status::Done;
    }
    if (sub == "list")
    {
        // every comet of the catalog near now, with its best display (days= the search's reach, default 400)
        const double days = c.num("days", 400.0);
        if (comets_.empty()) cometsRefreshCatalog(t, true);
        nlohmann::json arr = nlohmann::json::array();
        for (const comet::Elements &el : comets_)
        {
            double tb = 0, w0 = 0, w1 = 0, mb = 99, tl = 0;
            nlohmann::json o = {{"name", el.name}, {"perihelion_utc", fmtUtcReadable(el.tPeri)}, {"q_au", el.q}, {"H", el.H}};
            {
                double mm = 99.0, tm = 0.0;
                for (double ts = el.tPeri - 200.0 * kDay; ts <= el.tPeri + 200.0 * kDay; ts += kDay)
                {
                    CometGeom g;
                    cometGeometry(el, ts, skyEarthHelioAt(ts), g);
                    if (g.mag < mm) { mm = g.mag; tm = ts; }
                }
                o["min_mag"] = mm;
                o["min_mag_utc"] = fmtUtcReadable(tm);
            }
            if (cometBestWindow(el, t - 30.0 * kDay, t + days * kDay, tb, w0, w1, mb, tl))
            {
                o["best_utc"] = fmtUtcReadable(tb);
                o["best_mag"] = mb;
                o["best_tail_deg"] = tl;
                o["window_utc"] = {fmtUtcReadable(w0), fmtUtcReadable(w1)};
            }
            arr.push_back(o);
        }
        r["comets"] = arr;
        return harness::Status::Done;
    }
    if (sub == "watch")
    {
        // comets watch [k|great] [days=<rel. perihelion>] [utc=] [sun_max=-12] [orbit=1]: k a catalog index or "great"
        int idx = -1;
        if (comets_.empty()) cometsRefreshCatalog(t, true); // (just switched on: the frame has not run yet)
        if (c.pos.size() > 1)
        {
            const std::string k = c.pos[1];
            if (k == "great")
            {
                for (size_t i = 0; i < comets_.size(); ++i)
                    if (comets_[i].great) idx = (int)i;
            }
            else if (!k.empty() && std::isdigit((unsigned char)k[0]))
                idx = std::atoi(k.c_str());
            else
                for (size_t i = 0; i < comets_.size(); ++i)
                    if (std::strstr(comets_[i].name, k.c_str())) idx = (int)i; // a part of its name
        }
        else
        {
            double bm = 99.0;
            for (const CometNow &cn : cometNow_)
                if (cn.g.mag < bm) { bm = cn.g.mag; idx = cn.idx; }
        }
        if (idx < 0 || idx >= (int)comets_.size())
        {
            a.error = "no such comet near now (comets list)";
            return harness::Status::Error;
        }
        double tw = t;
        if (c.has("days")) tw = comets_[idx].tPeri + c.num("days", 0.0) * kDay;
        std::string msg;
        cometWatchAt(idx, tw, c.num("sun_max", -12.0), c.num("orbit", 0.0) > 0.5, msg);
        r["message"] = msg;
        r["utc"] = fmtUtcReadable(tw);
        return harness::Status::Done;
    }
    a.error = "comets [state] | list [days=] | watch [k|great] [days=] [sun_max=] [orbit=1]";
    return harness::Status::Error;
}
