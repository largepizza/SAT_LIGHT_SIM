// SatelliteSimComets.cpp — comets in the sky (2026-10-27, particles 2026-10-28; CLAUDE.md "Comets"; Comets.h).
//
// Per frame (cometsUpdate, recordCompute after updatePlanets): the catalog near now (the Great Comet of 2037 + the
// hashed minor comets), each one's geometry from the observer (heliocentric ecliptic, the planets' Earth + the
// observer's offset: a comet 560,000 km away shows a degree of parallax), its magnitude, and for the brightest few
// the tails as 3D PARTICLES (comet::buildParticles: dust grains released over the last weeks and flown under the
// Sun's gravity reduced by radiation pressure, ion parcels carried by the solar wind). A build is a pure function of
// its time on a fixed grid (comet::particleBuildStepS: ~0.003 of the nucleus's dynamical time), made on a worker
// (or on the frame after a jump); the GPU carries each particle from the build's time to the frame's by a Taylor step.
// The head is a point source: cometsFillHeads appends it to kFireBuf (kesslerFillDust's finish), so the point, bloom
// and glare draw it. cometsRecordSplat (recordCompute's end) adds every particle as a Gaussian of its own spread into a
// half-res RGBA16F target (comet_splat.vert/.frag: per particle its distance, the dust's forward-scattering phase at
// its own scattering angle, surface brightness in the Milky Way's anchor units, lum 0.05 = 21 mag/arcsec^2);
// cometsRecordDraw composites it with the analytic coma (comet_composite.frag) after the sky, at infinity behind every
// surface, hidden by cloud, gated against the sky's own surface brightness per pixel (darksky.glsl: light pollution,
// twilight, daylight with the aureole round the Sun, moonlight) and dimmed by the line of sight's extinction.
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
#include <thread>

glm::dvec3 skyEarthHelioAt(double t);     // SatelliteSim.cpp
std::string fmtUtcReadable(double tJ2000); // SatelliteSimLaunches.cpp

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kAuM = 1.495978707e11;
constexpr double kDay = 86400.0;
constexpr double kLumPerFluxSr = 2.952e-4;     // lum (0.05 = 21 mag/arcsec^2) per (mag-0 flux per steradian)
constexpr double kPsfRad = 2.9e-4;             // ~1 arcmin: the inner coma the eye sees as the point
constexpr int kMaxTails = 3;                   // comets with drawn tails
// GPU regions of the particle buffer, one per tail slot: the brightest comet's is the large one.
constexpr uint32_t kRegionCap[kMaxTails] = {132000u, 24000u, 24000u};
constexpr uint32_t kRegionOff[kMaxTails] = {0u, 132000u, 156000u};
constexpr uint32_t kGrainCap = 180000u;
constexpr VkFormat kFieldFormat = VK_FORMAT_R32G32B32A32_SFLOAT; // the detail field (comet_field.frag)
constexpr float kSplatCapPx = 22.0f;        // a particle's quad radius (splat px) past which it draws into a coarser level

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

// The frame's data at the head of the comet buffer (mirrored in comet_splat.vert / comet_composite.frag), 512 bytes.
struct GpuCometFrame
{
    glm::mat4 skyView;  // ENU -> camera (a rotation)
    glm::vec4 proj;     // x tan(fovY / 2), y aspect, z splat target width, w height
    glm::vec4 eclX, eclY, eclZ; // rows of ecliptic -> ENU
    glm::vec4 misc;     // x lum cull, y largest splat radius (px), z the splat pixel's angle (rad), w coma count
    glm::vec4 comet[kMaxTails][2]; // [0] xyz anchor - observer (AU), w the frame's time - the build's (days); [1] xyz anchor
    glm::vec4 coma[kMaxTails][3];  // [0] head ENU (as seen), w the coma's scale length (rad); [1] x kLum x flux / (2 pi rho_s),
                                   // y rho0 (rad), z the gas share; [2] rgb dust colour, w the comet's days from perihelion
    glm::vec4 pad[8];
};
static_assert(sizeof(GpuCometFrame) == 512, "GpuCometFrame is 512 bytes");
constexpr size_t kHeaderBytes = sizeof(GpuCometFrame);
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

// The particle builds: one per tail on the build grid (comet::particleBuildStepS), made on a worker while the frame
// draws the last one (the GPU's Taylor step carries it), on the frame after a jump (or always with
// comets.particles_sync, for exact harness videos).
void SatelliteSim::cometsBuildTails(double t, const glm::dvec3 &oh)
{
    for (CometTailCache &c : cometTails_) c.used = false;
    const int hw = std::max(2, (int)std::thread::hardware_concurrency());
    // A JUMP (sim time moved > 4x the last frame's step, or the first frame) builds on the frame; steady time warp keeps
    // the worker's build however many grid steps behind (the GPU's Taylor step carries it): at 1 d/s through
    // perihelion the grid step is 8-500 s and a build on every frame cost ~20 ms each (hitches, 2026-10-28).
    const double frameDt = std::abs(t - cometTailPrevT);
    const bool jumped = !(frameDt <= 4.0 * std::max(cometTailPrevDt, 1.0));
    cometTailPrevDt = frameDt;
    cometTailPrevT = t;
    for (CometNow &cn : cometNow_)
    {
        if (cn.tailSlot < 0) continue;
        const comet::Elements &el = comets_[cn.idx];
        CometTailCache *slot = nullptr;
        for (CometTailCache &c : cometTails_)
            if (c.name == el.name) slot = &c;
        if (!slot)
        {
            for (CometTailCache &c : cometTails_)
                if (!c.used && !c.job.valid()) { slot = &c; break; }
            if (!slot) { cometTails_.emplace_back(); slot = &cometTails_.back(); }
            slot->name = el.name;
            slot->cur.reset();
            slot->uploadedSlot = -1;
        }
        slot->used = true;
        comet::ParticleOpts o;
        if (el.great)
        {
            o.tauMaxDays = 45.0;
            o.tau0Days = 0.05;
            o.du = 0.01;
            o.grainsMin = 12;
            o.grainsMax = 128;
            o.maxGrains = (int)kRegionCap[cn.tailSlot] - 12000; // (the ion parcels: up to ~10k through perihelion)
        }
        else
        {
            o.tauMaxDays = 25.0;
            o.tau0Days = 2.0;
            o.du = 0.01;
            o.grainsMin = 6;
            o.grainsMax = 40;
            o.v0KmS = 0.3;
            o.ionPerSlot = 6;
            o.ionDtDays = 0.02;
            o.maxGrains = (int)kRegionCap[cn.tailSlot] - 3000;
        }
        o.dustFluxK *= (double)cometTailGain;
        o.ionFluxK *= (double)cometTailGain;
        o.detail = cometTailDetail > 0.0f;
        const double step = comet::particleBuildStepS(cn.g.r);
        const double tg = std::floor(t / step + 0.5) * step;
        auto take = [&](std::shared_ptr<const comet::ParticleBuild> b) {
            if (b && (!slot->cur || b->tBuild != slot->cur->tBuild)) { slot->cur = std::move(b); cometTailBuilds++; }
        };
        if (slot->job.valid() && slot->job.wait_for(std::chrono::seconds(0)) == std::future_status::ready) take(slot->job.get());
        const bool need = !slot->cur || slot->cur->tBuild != tg;
        if (need)
        {
            const double lag = slot->cur ? std::abs(t - slot->cur->tBuild) : 1e300;
            const bool far = !slot->cur || cometParticlesSync ||
                             (lag > 3.0 * step && (jumped || lag > 8.0 * std::max(frameDt, 1.0)));
            if (far)
            {
                if (slot->job.valid())
                {
                    const double jt = slot->jobT;
                    auto b = slot->job.get(); // (a worker is mid-build: let it finish)
                    if (jt == tg) take(std::move(b));
                }
                if (!slot->cur || slot->cur->tBuild != tg)
                {
                    FPROF_SCOPE("comet.syncBuild");
                    o.threads = std::max(2, hw - 2);
                    auto b = std::make_shared<comet::ParticleBuild>();
                    comet::buildParticles(el, tg, oh, cn.tailSlot, o, *b);
                    take(std::move(b));
                    cometSyncBuilds++;
                }
            }
            else if (!slot->job.valid())
            {
                o.threads = std::max(2, hw / 2 - 1);
                const comet::Elements elc = el;
                const int ts = cn.tailSlot;
                slot->jobT = tg;
                slot->job = std::async(std::launch::async, [elc, tg, oh, ts, o]() {
                    auto b = std::make_shared<comet::ParticleBuild>();
                    comet::buildParticles(elc, tg, oh, ts, o, *b);
                    return std::shared_ptr<const comet::ParticleBuild>(std::move(b));
                });
            }
        }
        cn.build = slot->cur;
        if (!cn.build) continue;
        cometLastBuildMs = cn.build->buildMs;
        // upload into the slot's region when it changed (the build's grains carry their slot in q.w)
        if (cometMapped && (slot->uploadedSlot != cn.tailSlot || slot->uploadedT != cn.build->tBuild))
        {
            const uint32_t n = std::min<uint32_t>((uint32_t)cn.build->grains.size(), kRegionCap[cn.tailSlot]);
            comet::GpuGrain *dst = reinterpret_cast<comet::GpuGrain *>(static_cast<char *>(cometMapped) + kHeaderBytes) +
                                   kRegionOff[cn.tailSlot];
            std::memcpy(dst, cn.build->grains.data(), sizeof(comet::GpuGrain) * n);
            for (uint32_t k = 0; k < n; ++k) // (a build made while the comet held another slot: its tags)
                dst[k].q.w = (float)(4 * cn.tailSlot) + std::fmod(dst[k].q.w, 4.0f);
            if (ctx_)
                ctx_->mirrorDirty(cometMapped, kHeaderBytes + sizeof(comet::GpuGrain) * kRegionOff[cn.tailSlot],
                                  sizeof(comet::GpuGrain) * n);
            cometUploadBytes += sizeof(comet::GpuGrain) * n;
            cometGrainN[cn.tailSlot] = n;
            slot->uploadedSlot = cn.tailSlot;
            slot->uploadedT = cn.build->tBuild;
        }
        // the framing (the Watch, cometTailEnu): the visible tail's span and its far part
        cn.tailSpanDeg = (float)deg(cn.build->spanRad);
        cn.tailAimEnu = cometEnu(cn.build->aimEcl, t);
    }
    // regions no comet uses draw nothing
    for (int s = 0; s < kMaxTails; ++s)
    {
        bool used = false;
        for (const CometNow &cn : cometNow_)
            if (cn.tailSlot == s && cn.build) used = true;
        if (!used) cometGrainN[s] = 0;
    }
    cometTails_.erase(std::remove_if(cometTails_.begin(), cometTails_.end(),
                                     [](CometTailCache &c) {
                                         if (c.used) return false;
                                         if (c.job.valid()) c.job.wait();
                                         return true;
                                     }),
                      cometTails_.end());
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

// ── GPU: the splat pass and the composite ───────────────────────────────────────────────────────────────────────
struct CometPC
{
    glm::vec4 sunDirENU;
    glm::vec4 obs;    // x observer radius (m), y extinction k, z atmFrac, w moonBright
    glm::vec4 misc;   // x exposure x gain, y manual terrain test, z the Sun above the horizon (0/1), w glare gate
    glm::vec4 screen; // xy full-res size, z the splat target holds this frame's particles (0/1), w unused
};
static_assert(sizeof(CometPC) <= 128, "CometPC must fit 128 bytes");

namespace
{
VkPipeline makeCometPipeline(VulkanContext &ctx, VkPipelineLayout layout, VkRenderPass rp, const char *vs, const char *fs,
                             bool depthTest, bool sumAlpha = false)
{
    VkShaderModule vert = ctx.loadShader(vs);
    VkShaderModule frag = ctx.loadShader(fs);
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
    ds.depthTestEnable = depthTest ? VK_TRUE : VK_FALSE; // the composite at infinity (kDepthFar): every surface hides it
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
    if (sumAlpha) cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE; // (the field target sums all four channels)
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
    ci.layout = layout;
    ci.renderPass = rp;
    ci.subpass = 0;
    VkPipeline p = VK_NULL_HANDLE;
    if (vkCreateGraphicsPipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &p) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create a comet pipeline");
    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
    return p;
}
} // namespace

void SatelliteSim::cometsCreateSplatTarget(VulkanContext &ctx)
{
    const float sc = std::clamp(cometSplatScale, 0.25f, 1.0f);
    cometSplatExtent.width = std::max(2u, (uint32_t)std::lround(ctx.swapExtent.width * sc));
    cometSplatExtent.height = std::max(2u, (uint32_t)std::lround(ctx.swapExtent.height * sc));
    cometSplatAtlas.width = cometSplatExtent.width + (cometSplatExtent.width + 1) / 2;
    cometSplatAtlas.height = cometSplatExtent.height;
    ctx.createImage(cometSplatAtlas.width, cometSplatAtlas.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, cometSplatImg, cometSplatMem);
    cometFieldExtent.width = std::max(2u, cometSplatExtent.width / 4);
    cometFieldExtent.height = std::max(2u, cometSplatExtent.height / 4);
    cometFieldAtlas.width = cometFieldExtent.width + (cometFieldExtent.width + 1) / 2;
    cometFieldAtlas.height = cometFieldExtent.height;
    ctx.createImage(cometFieldAtlas.width, cometFieldAtlas.height, kFieldFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, cometFieldImg, cometFieldMem);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vci.image = cometSplatImg;
    vkCreateImageView(ctx.device, &vci, nullptr, &cometSplatView);
    vci.format = kFieldFormat;
    vci.image = cometFieldImg;
    vkCreateImageView(ctx.device, &vci, nullptr, &cometFieldView);
    // its first use is a render pass from UNDEFINED; the composite samples it only after a pass ran — but the
    // descriptor needs a valid layout: transition once to SHADER_READ_ONLY
    {
        auto cmd = ctx.beginOneTimeCommands();
        for (VkImage im : {cometSplatImg, cometFieldImg})
            ctx.imageBarrier(cmd, im, 0, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                             VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                             VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        ctx.endOneTimeCommands(cmd);
    }
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = cometSplatRenderPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &cometSplatView;
    fci.width = cometSplatAtlas.width;
    fci.height = cometSplatAtlas.height;
    fci.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &cometSplatFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create the comet splat framebuffer");
    fci.renderPass = cometFieldRenderPass;
    fci.pAttachments = &cometFieldView;
    fci.width = cometFieldAtlas.width;
    fci.height = cometFieldAtlas.height;
    if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &cometFieldFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create the comet field framebuffer");
}

void SatelliteSim::cometsDestroySplatTarget(VkDevice device)
{
    if (cometSplatFramebuffer) vkDestroyFramebuffer(device, cometSplatFramebuffer, nullptr);
    if (cometSplatView) vkDestroyImageView(device, cometSplatView, nullptr);
    if (cometSplatImg) vkDestroyImage(device, cometSplatImg, nullptr);
    if (cometSplatMem) vkFreeMemory(device, cometSplatMem, nullptr);
    if (cometFieldFramebuffer) vkDestroyFramebuffer(device, cometFieldFramebuffer, nullptr);
    cometFieldFramebuffer = VK_NULL_HANDLE;
    if (cometFieldView) vkDestroyImageView(device, cometFieldView, nullptr);
    if (cometFieldImg) vkDestroyImage(device, cometFieldImg, nullptr);
    if (cometFieldMem) vkFreeMemory(device, cometFieldMem, nullptr);
    cometFieldView = VK_NULL_HANDLE;
    cometFieldImg = VK_NULL_HANDLE;
    cometFieldMem = VK_NULL_HANDLE;
    cometSplatFramebuffer = VK_NULL_HANDLE;
    cometSplatView = VK_NULL_HANDLE;
    cometSplatImg = VK_NULL_HANDLE;
    cometSplatMem = VK_NULL_HANDLE;
}

void SatelliteSim::cometsInit(VulkanContext &ctx)
{
    const VkDeviceSize sz = kHeaderBytes + sizeof(comet::GpuGrain) * kGrainCap;
    cometMapped = createMirrored(ctx, sz, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT, cometBuf, cometMem, false, "comets");
    std::memset(cometMapped, 0, (size_t)sz);
    VkDescriptorSetLayoutBinding b[8] = {};
    const VkShaderStageFlags vf = VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT;
    b[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, vf, nullptr};
    b[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, vf, nullptr};
    b[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    b[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 8;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &cometDescLayout);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}, {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 5},
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

    // the splat render pass: one RGBA16F attachment, cleared, left for the composite's sampling
    {
        VkAttachmentDescription color{};
        color.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        color.samples = VK_SAMPLE_COUNT_1_BIT;
        color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
        color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
        color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
        color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
        color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        color.finalLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        const VkAttachmentReference ref{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
        VkSubpassDescription sub{};
        sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
        sub.colorAttachmentCount = 1;
        sub.pColorAttachments = &ref;
        VkSubpassDependency dep[2] = {};
        dep[0].srcSubpass = VK_SUBPASS_EXTERNAL;
        dep[0].dstSubpass = 0;
        dep[0].srcStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
        dep[0].dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[0].dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].srcSubpass = 0;
        dep[1].dstSubpass = VK_SUBPASS_EXTERNAL;
        dep[1].srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
        dep[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
        dep[1].dstStageMask = VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT;
        dep[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
        rpci.attachmentCount = 1;
        rpci.pAttachments = &color;
        rpci.subpassCount = 1;
        rpci.pSubpasses = &sub;
        rpci.dependencyCount = 2;
        rpci.pDependencies = dep;
        if (vkCreateRenderPass(ctx.device, &rpci, nullptr, &cometSplatRenderPass) != VK_SUCCESS)
            throw std::runtime_error("SatelliteSim: failed to create the comet splat render pass");
        VkAttachmentDescription field = color; // the detail field (comet_field.frag): its own pass at half the size
        field.format = kFieldFormat;
        rpci.pAttachments = &field;
        if (vkCreateRenderPass(ctx.device, &rpci, nullptr, &cometFieldRenderPass) != VK_SUCCESS)
            throw std::runtime_error("SatelliteSim: failed to create the comet field render pass");
    }
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = sci.addressModeV = sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(ctx.device, &sci, nullptr, &cometSplatSampler);
    }
    cometsCreateSplatTarget(ctx);
    cometsWriteDescriptors(ctx);

    VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(CometPC)};
    VkPipelineLayoutCreateInfo pl{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pl.setLayoutCount = 1;
    pl.pSetLayouts = &cometDescLayout;
    pl.pushConstantRangeCount = 1;
    pl.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &pl, nullptr, &cometPipeLayout);
    cometSplatPipeLayout = cometPipeLayout;
    cometSplatPipeline = makeCometPipeline(ctx, cometPipeLayout, cometSplatRenderPass, "shaders/comet_splat.vert.spv",
                                           "shaders/comet_splat.frag.spv", false);
    cometFieldPipeline = makeCometPipeline(ctx, cometPipeLayout, cometFieldRenderPass, "shaders/comet_splat.vert.spv",
                                           "shaders/comet_field.frag.spv", false, true);
    cometPipeline = makeCometPipeline(ctx, cometPipeLayout, ctx.renderPass, "shaders/comet_composite.vert.spv",
                                      "shaders/comet_composite.frag.spv", true);
}

void SatelliteSim::cometsWriteDescriptors(VulkanContext &ctx)
{
    if (!cometDescSet || !cometSplatView) return;
    VkDescriptorBufferInfo vb{cometBuf, 0, VK_WHOLE_SIZE}, ub{cloudParamsBuf, 0, VK_WHOLE_SIZE}, db{lightDomeBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo ca{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cbi{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sd{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sp{cometSplatSampler, cometSplatView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo fv{cometSplatSampler, cometFieldView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet w[8] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &vb, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &ca, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cbi, nullptr, nullptr};
    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sd, nullptr, nullptr};
    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ub, nullptr};
    w[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 5, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &db, nullptr};
    w[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 6, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sp, nullptr, nullptr};
    w[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cometDescSet, 7, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &fv, nullptr, nullptr};
    frameSafeUpdateDescriptorSets(ctx.device, 8, w, 0, nullptr);
}

// The frame's header and the splat pass (recordCompute's end, outside any render pass).
void SatelliteSim::cometsRecordSplat(VkCommandBuffer cmd, VulkanContext &ctx)
{
    FPROF_SCOPE("cometsRecordSplat");
    cometSplatDrawn = false;
    if (!cometSplatPipeline || !cometMapped) return;
    uint32_t total = 0;
    for (int s = 0; s < kMaxTails; ++s) total += cometGrainN[s];
    int comae = 0;
    for (const CometNow &cn : cometNow_)
        if (cn.tailSlot >= 0) comae++;
    if (total == 0 && comae == 0) return;
    const double t = (double)simDayJ2000 * kDay + simSecInDay;
    const double eps = obliquity(t);
    const glm::dvec3 oh = cometObserverHelio(t);
    GpuCometFrame &F = *static_cast<GpuCometFrame *>(cometMapped);
    F = GpuCometFrame{};
    F.skyView = camera.viewMatrix();
    const float tanH = std::tan(glm::radians(camera.fovYDeg) * 0.5f);
    F.proj = glm::vec4(tanH, (float)ctx.swapExtent.width / (float)ctx.swapExtent.height, (float)cometSplatExtent.width,
                       (float)cometSplatExtent.height);
    {
        const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
        glm::dvec3 col[3];
        for (int j = 0; j < 3; ++j)
        {
            glm::dvec3 e(0.0);
            e[j] = 1.0;
            const glm::dvec3 q = eclToEci(e, eps);
            col[j] = glm::dvec3(glm::dot(q, ex), glm::dot(q, ey), glm::dot(q, ez));
        }
        F.eclX = glm::vec4((float)col[0].x, (float)col[1].x, (float)col[2].x, 0.0f);
        F.eclY = glm::vec4((float)col[0].y, (float)col[1].y, (float)col[2].y, 0.0f);
        F.eclZ = glm::vec4((float)col[0].z, (float)col[1].z, (float)col[2].z, 0.0f);
    }
    const float pixAng = 2.0f * tanH / (float)cometSplatExtent.height;
    F.misc = glm::vec4(1.0e-6f, kSplatCapPx, pixAng, (float)kMaxTails);
    // The splat atlas (comet_frame.glsl F.pad[1]): level 0 the half-res screen at (0, 0), level k >= 1 W >> k by H >> k
    // at (W, sum of the levels above it). A particle wider than the quad cap draws into the coarsest level holding it
    // within the cap (its fill bounded), the composite adds the levels back, bilinear: zoomed in, every particle of the
    // tail in view still draws (until 2026-10-28 those past 4x the cap were culled: the tail vanished at a 20 deg field).
    F.pad[0] = glm::vec4(std::clamp(cometTailDetail, 0.0f, 4.0f), 0.0f, 0.0f, (float)cometTailDebug);
    F.pad[1] = glm::vec4((float)cometSplatExtent.width, (float)cometSplatExtent.height, (float)cometSplatAtlas.width,
                         (float)cometSplatAtlas.height);
    F.pad[2] = glm::vec4((float)cometFieldExtent.width, (float)cometFieldExtent.height, (float)cometFieldAtlas.width,
                         (float)cometFieldAtlas.height);
    for (const CometNow &cn : cometNow_)
    {
        if (cn.tailSlot < 0) continue;
        const int s = cn.tailSlot;
        const comet::Elements &el = comets_[cn.idx];
        if (cn.build)
        {
            F.comet[s][0] = glm::vec4(glm::vec3(cn.build->anchor - oh), (float)((t - cn.build->tBuild) / kDay));
            F.comet[s][1] = glm::vec4(glm::vec3(cn.build->anchor), (float)(el.rSat * el.rSat));
        }
        // the coma: a Haser-like column (1/rho x exp(-rho / rho_s)), its total the head's light less the point's share
        if (cn.g.comaRad > 1.5 * kPsfRad && !cn.below)
        {
            const double rhoS = cn.g.comaRad / 2.5;
            const double Fc = std::pow(10.0, -0.4 * cn.g.mag) * (1.0 - cn.pointShare);
            const double Lc = kLumPerFluxSr * Fc / (2.0 * kPi * rhoS);
            const float gasShare = (float)std::clamp(el.gas / (el.gas + 0.6 * el.dust), 0.0, 1.0);
            F.coma[s][0] = glm::vec4(cn.headEnuDrawn, (float)rhoS);
            F.coma[s][1] = glm::vec4((float)std::min(Lc, 1e9), (float)kPsfRad, gasShare, 1.0f);
            F.coma[s][2] = glm::vec4(lumNormColor(glm::vec3(1.0f, 0.95f, 0.86f)), 0.0f);
        }
        // w: the comet's days from perihelion now (the tail detail's emission times are taken from it)
        F.coma[s][2].w = (float)((t - el.tPeri) / kDay);
    }
    ctx.mirrorDirty(cometMapped, 0, kHeaderBytes);

    VkClearValue clr[1] = {};
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = cometSplatRenderPass;
    rbi.framebuffer = cometSplatFramebuffer;
    rbi.renderArea = {{0, 0}, cometSplatAtlas};
    rbi.clearValueCount = 1;
    rbi.pClearValues = clr;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (total > 0)
    {
        VkViewport vp{0, 0, (float)cometSplatAtlas.width, (float)cometSplatAtlas.height, 0, 1};
        VkRect2D sc{{0, 0}, cometSplatAtlas};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometSplatPipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometPipeLayout, 0, 1, &cometDescSet, 0, nullptr);
        CometPC pc{};
        vkCmdPushConstants(cmd, cometPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        for (int s = 0; s < kMaxTails; ++s)
            if (cometGrainN[s] > 0) vkCmdDraw(cmd, 6, cometGrainN[s], 0, kRegionOff[s]);
    }
    vkCmdEndRenderPass(cmd);
    // the dust's detail field (cleared even with the detail off: the composite reads it)
    rbi.renderPass = cometFieldRenderPass;
    rbi.framebuffer = cometFieldFramebuffer;
    rbi.renderArea = {{0, 0}, cometFieldAtlas};
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    if (total > 0 && cometTailDetail > 0.0f)
    {
        VkViewport vp{0, 0, (float)cometFieldAtlas.width, (float)cometFieldAtlas.height, 0, 1};
        VkRect2D sc{{0, 0}, cometFieldAtlas};
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometFieldPipeline);
        vkCmdSetViewport(cmd, 0, 1, &vp);
        vkCmdSetScissor(cmd, 0, 1, &sc);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometPipeLayout, 0, 1, &cometDescSet, 0, nullptr);
        CometPC pc{};
        pc.screen.w = 1.0f;
        vkCmdPushConstants(cmd, cometPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
        for (int s = 0; s < kMaxTails; ++s)
            if (cometGrainN[s] > 0) vkCmdDraw(cmd, 6, cometGrainN[s], 0, kRegionOff[s]);
    }
    vkCmdEndRenderPass(cmd);
    cometSplatDrawn = true;
}

void SatelliteSim::cometsRecordDraw(VkCommandBuffer cmd, VulkanContext &ctx, const PointDrawPC &ppc)
{
    cometDrawsLast = 0;
    if (!cometPipeline || !cometSplatDrawn) return;
    CometPC pc{};
    pc.sunDirENU = sunDirENU;
    pc.obs = glm::vec4(glm::length(obsECI), extinctionCoeff, cometAtmFrac, cometMoonBright);
    // the Sun above the apparent horizon (its aureole and glare), from anywhere
    const float obsR = glm::length(obsECI);
    const float limbZ = obsR > (float)satphot::kEarthRadiusM + 1.0f
                            ? -std::sqrt(std::max(0.0f, 1.0f - ((float)satphot::kEarthRadiusM / obsR) * ((float)satphot::kEarthRadiusM / obsR)))
                            : 0.0f;
    const bool occ = inertialActive && inertialOcculter; // (the inertial lock's occulter: no Sun, no glare)
    pc.misc = glm::vec4(cometTailGainDisplay, ppc.manualTerrainTest, (sunDirENU.w > limbZ && !occ) ? 1.0f : 0.0f,
                        occ ? 1.0f : glm::mix(skyGlareEased, 1.0f, cometAtmFrac));
    pc.screen = glm::vec4((float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 1.0f, 0.0f);
    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometPipeline);
    vkCmdSetViewport(cmd, 0, 1, &vp);
    vkCmdSetScissor(cmd, 0, 1, &sc);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, cometPipeLayout, 0, 1, &cometDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, cometPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    cometDrawsLast = 1;
}

void SatelliteSim::cometsDestroy(VkDevice device)
{
    for (CometTailCache &c : cometTails_)
        if (c.job.valid()) c.job.wait();
    cometTails_.clear();
    if (cometPipeline) vkDestroyPipeline(device, cometPipeline, nullptr);
    if (cometSplatPipeline) vkDestroyPipeline(device, cometSplatPipeline, nullptr);
    if (cometFieldPipeline) vkDestroyPipeline(device, cometFieldPipeline, nullptr);
    cometFieldPipeline = VK_NULL_HANDLE;
    if (cometPipeLayout) vkDestroyPipelineLayout(device, cometPipeLayout, nullptr);
    if (cometDescPool) vkDestroyDescriptorPool(device, cometDescPool, nullptr);
    if (cometDescLayout) vkDestroyDescriptorSetLayout(device, cometDescLayout, nullptr);
    cometsDestroySplatTarget(device);
    if (cometSplatRenderPass) vkDestroyRenderPass(device, cometSplatRenderPass, nullptr);
    if (cometFieldRenderPass) vkDestroyRenderPass(device, cometFieldRenderPass, nullptr);
    cometFieldRenderPass = VK_NULL_HANDLE;
    if (cometSplatSampler) vkDestroySampler(device, cometSplatSampler, nullptr);
    cometSplatRenderPass = VK_NULL_HANDLE;
    cometSplatSampler = VK_NULL_HANDLE;
    cometPipeline = VK_NULL_HANDLE;
    cometSplatPipeline = VK_NULL_HANDLE;
    cometPipeLayout = VK_NULL_HANDLE;
    cometSplatPipeLayout = VK_NULL_HANDLE;
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
    inertialStop();
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
                                {"tail_span_deg", cn.tailSpanDeg},
                                {"build_t_offset_s", cn.build ? t - cn.build->tBuild : 0.0},
                                {"build_dust", cn.build ? cn.build->nDust : 0u},
                                {"build_ion", cn.build ? cn.build->nIon : 0u},
                                {"build_step_s", comet::particleBuildStepS(cn.g.r)},
                                {"days_from_perihelion", (t - el.tPeri) / kDay},
                                {"q_au", el.q},
                                {"e", el.e},
                                {"az_deg", deg(std::atan2((double)cn.headEnuDrawn.x, (double)cn.headEnuDrawn.y))},
                                {"el_deg", deg(std::asin(std::clamp((double)cn.headEnuDrawn.z, -1.0, 1.0)))}};
            arr.push_back(o);
        }
        r["comets"] = arr;
        r["catalog_size"] = comets_.size();
        r["grains"] = {cometGrainN[0], cometGrainN[1], cometGrainN[2]};
        r["upload_mb_total"] = (double)cometUploadBytes / 1048576.0;
        r["sync_builds"] = cometSyncBuilds;
        r["last_build_ms"] = cometLastBuildMs;
        r["particles_sync"] = cometParticlesSync;
        r["splat_extent"] = {cometSplatExtent.width, cometSplatExtent.height};
        r["splat_atlas"] = {cometSplatAtlas.width, cometSplatAtlas.height};
        r["draws_last"] = cometDrawsLast;
        r["tail_builds"] = cometTailBuilds;
        r["cpu_ms"] = cometCpuMs;
        r["enabled"] = cometsEnabled;
        r["tail_gain"] = cometTailGain;
        r["tail_detail"] = cometTailDetail;
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
