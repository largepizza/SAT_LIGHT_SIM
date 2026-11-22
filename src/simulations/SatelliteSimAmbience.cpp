// SatelliteSimAmbience.cpp — the sim's side of the ambient sound: the CONTEXT the layer table
// (Ambience.h, assets/sound/ambience/ambience.json) is evaluated against.
//
// Every driver is a function of what the sim already knows — no new data except the ocean mask:
//   alt_m / agl_m / ground_m  the eye: max(ground, obsHeightOffset); ground = the GPU's (DEM + detail)
//                        when the depth pass runs, else the CPU's DEM copy (like harness `state`)
//   lat_deg / lon_deg    geocentric, from obsDir (so follow mode reports the camera)
//   sun_el_deg           the Sun's elevation at the camera (at altitude: at the sub-camera point)
//   ocean_near/_wide     ocean fraction within ~3 km / ~25 km (oceanMaskCpu, 2.7 km/px)
//   veg / forest /       land cover under the camera, 0..1, classified from the day map's colour
//   desert / ice         (the ground as drawn): vegetated (green), dark green canopy, bright tan,
//                        white. Thresholds from sampled biomes (Sahara, Amazon, taiga, Iowa, UK,
//                        Greenland, ...). The map is a dry-season mosaic: the Great Plains read tan,
//                        so "not desert and not ice" is the better test for open grassland.
//   urban                city brightness under the camera (night-lights map, the light-pollution
//                        dome's response curve), 0..1
//   beam                 Reflect Orbital light on the ground at the camera: the same Gaussian
//                        spots sat_sky.frag draws, summed at the origin of groundBeams
//   glare_n / glare_sum  the flares GLARING ON SCREEN (the glare sprites' own test: past the glare
//                        threshold, in the view): how many (each in full once 0.4 past the threshold)
//                        and their combined strength past it. From the host copy of sat_flare.comp's
//                        bright-flare list; eased up in 0.1 s, down in 0.6 s. The glare pad's input.
//                        Range: 0 to ~470 — facing the Reflect ring from a lit site, hundreds of
//                        mirrors glare at once (0.06 facing away from it, 0 from 120 km off).
//   beam_site            Reflect beams converging on ground sites near the listener: each converged
//                        beam counts 1 within 15 km of the camera, 1/(1 + (excess/25 km)^2) beyond
//                        (~180 on the Topaz site, ~10 from 120 km away). Eased ~1.5 s. The site hum's
//                        input.
//   rocket_mag / stage_mag / fireball_mag / dust_mag   the upwell's other sources (2026-10-21): burning
//                        engines, sunlit coasting stages, re-entry fireballs, collision dust — the summed apparent
//                        flux of each kind, each source counted when on screen (soft frame edge) or near (rocket
//                        8 km, stage 1.5 km, fireball 15 km, dust 20 km; nothing past 6x that), as an equivalent
//                        magnitude (30 = none). Eased up 0.15 s, down 0.8 s. Mapped by "music_upwell".
//   music_gap            where the beam sounds may speak: 0 while a track plays (the track's upwell
//                        stem answers the glare instead), a triangle over the silent gap between
//                        tracks (0 -> 1 halfway -> 0), 1 when no music is audible. Slewed 1/4 per s.
//   aurora               the auroral oval under the camera (band only, no curtain noise), 0..1
//   wind                 a smooth pseudo-random wind strength over position and sim time, 0..1
//   time_scale           sim seconds per wall second (time-of-day layers fade out under time warp)
//   following / intro    0/1
//   speed_mps / eas      the camera's speed through the air (ECEF, so time warp alone moves nothing)
//                        and its EQUIVALENT AIRSPEED, speed x sqrt(rho / rho0) with an 8.5 km scale
//                        height: what a wind rush scales with — loud low down, nothing in orbit
//                        at 7.6 km/s. A jump of more than 20 km in one frame (observer, Go to, a
//                        harness teleport) is not motion: the speed is held at 0 for that frame.
//   <group>_count        satellites of a shell group within its hearing range (closed form below)
//   <group>_near_m       expected distance to the nearest one
#include "../FrameProfile.h"
#include "SatelliteSim.h"

#include "Ambience.h"
#include "AmbientSynth.h"
#include "Harness.h"
#include "AudioSystem.h"
#include "Log.h"
#include "MusicAnalysis.h"
#include "Paths.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cstring>
#include <cctype>
#include <cmath>
#include <filesystem>
#include <fstream>

namespace
{
constexpr double kReM = 6371000.0;
constexpr float kTimeScalesAmb[] = {1.0f, 10.0f, 60.0f, 300.0f, 3600.0f, 86400.0f, 604800.0f, 2592000.0f, 31536000.0f};
constexpr const char *kAmbiencePath = "assets/sound/ambience/ambience.json";

bool globMatch(const std::string &pat, const std::string &s)
{
    // '*' only, case-insensitive.
    size_t p = 0, i = 0, star = std::string::npos, mark = 0;
    auto eq = [](char a, char b) { return std::tolower((unsigned char)a) == std::tolower((unsigned char)b); };
    while (i < s.size())
    {
        if (p < pat.size() && pat[p] == '*')
        {
            star = p++;
            mark = i;
        }
        else if (p < pat.size() && eq(pat[p], s[i]))
        {
            ++p;
            ++i;
        }
        else if (star != std::string::npos)
        {
            p = star + 1;
            i = ++mark;
        }
        else
            return false;
    }
    while (p < pat.size() && pat[p] == '*')
        ++p;
    return p == pat.size();
}

float hash3(int x, int y, int z)
{
    uint32_t h = (uint32_t)x * 374761393u + (uint32_t)y * 668265263u + (uint32_t)z * 2246822519u;
    h = (h ^ (h >> 13)) * 1274126177u;
    h ^= h >> 16;
    return (float)(h & 0xFFFFFF) / 16777215.0f;
}

float valueNoise(double x, double y, double z)
{
    const double fx = std::floor(x), fy = std::floor(y), fz = std::floor(z);
    const int ix = (int)fx, iy = (int)fy, iz = (int)fz;
    auto s = [](double t) { return (float)(t * t * (3.0 - 2.0 * t)); };
    const float tx = s(x - fx), ty = s(y - fy), tz = s(z - fz);
    auto lerp = [](float a, float b, float t) { return a + (b - a) * t; };
    const float c00 = lerp(hash3(ix, iy, iz), hash3(ix + 1, iy, iz), tx);
    const float c10 = lerp(hash3(ix, iy + 1, iz), hash3(ix + 1, iy + 1, iz), tx);
    const float c01 = lerp(hash3(ix, iy, iz + 1), hash3(ix + 1, iy, iz + 1), tx);
    const float c11 = lerp(hash3(ix, iy + 1, iz + 1), hash3(ix + 1, iy + 1, iz + 1), tx);
    return lerp(lerp(c00, c10, ty), lerp(c01, c11, ty), tz);
}

float smoothstepf(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

void SatelliteSim::initAmbience()
{
    ambience_ = new Ambience();
    Ambience &a = *ambience_;
    ambD_.altM = a.registerDriver("alt_m");
    ambD_.aglM = a.registerDriver("agl_m");
    ambD_.groundM = a.registerDriver("ground_m");
    ambD_.latDeg = a.registerDriver("lat_deg");
    ambD_.lonDeg = a.registerDriver("lon_deg");
    ambD_.sunElDeg = a.registerDriver("sun_el_deg");
    ambD_.oceanNear = a.registerDriver("ocean_near");
    ambD_.oceanWide = a.registerDriver("ocean_wide");
    ambD_.urban = a.registerDriver("urban");
    ambD_.beam = a.registerDriver("beam");
    ambD_.aurora = a.registerDriver("aurora");
    ambD_.wind = a.registerDriver("wind");
    ambD_.cloud = a.registerDriver("cloud");
    ambD_.timeScale = a.registerDriver("time_scale");
    ambD_.following = a.registerDriver("following");
    ambD_.intro = a.registerDriver("intro");
    ambD_.veg = a.registerDriver("veg");
    ambD_.forest = a.registerDriver("forest");
    ambD_.desert = a.registerDriver("desert");
    ambD_.ice = a.registerDriver("ice");
    ambD_.speed = a.registerDriver("speed_mps");
    ambD_.eas = a.registerDriver("eas");
    ambD_.glareN = a.registerDriver("glare_n");
    ambD_.glareSum = a.registerDriver("glare_sum");
    ambD_.beamSite = a.registerDriver("beam_site");
    ambD_.musicGap = a.registerDriver("music_gap");
    ambD_.rain = a.registerDriver("rain");
    ambD_.thunder = a.registerDriver("thunder");
    ambD_.rocketMag = a.registerDriver("rocket_mag");
    ambD_.stageMag = a.registerDriver("stage_mag");
    ambD_.fireballMag = a.registerDriver("fireball_mag");
    ambD_.dustMag = a.registerDriver("dust_mag");
    ambD_.gust = a.registerDriver("gust");
    ambD_.surfaceWind = a.registerDriver("surface_wind");
    ambD_.rainDriven = a.registerDriver("rain_driven");
    ambD_.tornado = a.registerDriver("tornado");
    ambD_.impact = a.registerDriver("impact");
    ambD_.rocket = a.registerDriver("rocket");

    std::string err;
    if (!a.load(kAmbiencePath, err))
    {
        Log::line("ambience: " + err);
        fprintf(stderr, "[ambience] %s\n", err.c_str());
        return;
    }
    // Which constellations each shell group covers: a pattern matches the type's model id or name.
    ambGroupConsts_.assign(a.shellGroups().size(), {});
    std::string summary;
    for (size_t g = 0; g < a.shellGroups().size(); ++g)
    {
        const auto &grp = a.shellGroups()[g];
        for (size_t c = 0; c < constellations.size(); ++c)
        {
            const SatelliteType &t = satTypes[constellations[c].typeIdx];
            for (const std::string &p : grp.patterns)
                if (globMatch(p, t.modelId) || globMatch(p, t.name))
                {
                    ambGroupConsts_[g].push_back((int)c);
                    break;
                }
        }
        summary += " " + grp.name + "=" + std::to_string(ambGroupConsts_[g].size());
    }
    Log::line("ambience: " + std::to_string(a.layerCount()) + " layers; shell groups (constellations):" + summary);
}

bool SatelliteSim::oceanAt(double latRad, double lonRad) const
{
    if (oceanMaskCpu.empty())
        return false;
    const double u = (lonRad + glm::pi<double>()) / (2.0 * glm::pi<double>());
    const double v = (0.5 * glm::pi<double>() - latRad) / glm::pi<double>();
    int x = (int)std::floor(u * oceanMaskW) % oceanMaskW;
    if (x < 0)
        x += oceanMaskW;
    const int y = std::clamp((int)(v * oceanMaskH), 0, oceanMaskH - 1);
    const size_t i = (size_t)y * oceanMaskW + x;
    return (oceanMaskCpu[i >> 6] >> (i & 63)) & 1ull;
}

// Centre + `rings` rings of 8 samples out to radiusM (tangent-plane offsets; fine at these radii).
float SatelliteSim::oceanFraction(double latRad, double lonRad, double radiusM, int rings) const
{
    // The ring directions' cosines and sines, computed once by the same expressions (bit-identical; the storm
    // catalog calls this for every spawn cell of every time bin, and these 16 calls were most of its cost).
    struct RingDirs
    {
        double c[2][8], s[2][8];
        RingDirs()
        {
            for (int odd = 0; odd < 2; ++odd)
                for (int k = 0; k < 8; ++k)
                {
                    const double b = (k + 0.5 * odd) * (glm::pi<double>() / 4.0);
                    c[odd][k] = std::cos(b);
                    s[odd][k] = std::sin(b);
                }
        }
    };
    static const RingDirs kRing;
    int hits = oceanAt(latRad, lonRad) ? 1 : 0, n = 1;
    const double cosLat = std::max(0.05, std::cos(latRad));
    for (int r = 1; r <= rings; ++r)
    {
        const double d = radiusM * r / rings / kReM;
        for (int k = 0; k < 8; ++k)
        {
            hits += oceanAt(latRad + d * kRing.c[r & 1][k], lonRad + d * kRing.s[r & 1][k] / cosLat) ? 1 : 0;
            ++n;
        }
    }
    return (float)hits / (float)n;
}

void SatelliteSim::computeAmbienceContext(float dt)
{
    FPROF_SCOPE("computeAmbienceContext");
    Ambience &a = *ambience_;
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    const double lat = std::asin(std::clamp(up.z, -1.0, 1.0));
    const double lon = std::atan2(up.y, up.x);
    const bool gpuGround = terrainFrameMapped && (debugDisableMask & 1024u) == 0;
    const float ground = gpuGround ? terrainFrameMapped[1] : obsTerrainH;
    const float eyeAsl = std::max(ground, obsHeightOffset);
    a.set(ambD_.altM, eyeAsl);
    a.set(ambD_.aglM, eyeAsl - ground);
    a.set(ambD_.groundM, ground);
    a.set(ambD_.latDeg, (float)glm::degrees(lat));
    a.set(ambD_.lonDeg, (float)glm::degrees(lon));
    a.set(ambD_.sunElDeg, glm::degrees(std::asin(std::clamp(sunDirENU.w, -1.0f, 1.0f))));
    a.set(ambD_.oceanNear, oceanFraction(lat, lon, 3000.0, 1));
    a.set(ambD_.oceanWide, oceanFraction(lat, lon, 25000.0, 2));
    a.set(ambD_.timeScale, kTimeScalesAmb[std::clamp(timeScaleIdx, 0, 8)]);
    a.set(ambD_.following, followActive ? 1.0f : 0.0f);
    a.set(ambD_.intro, showIntro ? 1.0f : 0.0f);

    // Camera speed and equivalent airspeed (wind rush).
    {
        const glm::dvec3 cam = up * (kReM + (double)eyeAsl);
        float raw = 0.0f;
        if (ambPrevValid && dt > 1e-4f)
        {
            const double d = glm::length(cam - ambPrevCamEcef);
            raw = d > 20000.0 ? 0.0f : (float)(d / dt);
        }
        ambPrevCamEcef = cam;
        ambPrevValid = true;
        // Median of the last three: a ONE-frame jump of the eye is not flight. The ground under the
        // camera switches from the CPU's coarse DEM to the GPU's detailed one on the first readback
        // (and on every `observer agl=`), moving the eye hundreds of metres in a frame — it read as
        // 20+ km/s and blew a wind rush at the spawn point.
        ambSpeedRaw[0] = ambSpeedRaw[1];
        ambSpeedRaw[1] = ambSpeedRaw[2];
        ambSpeedRaw[2] = raw;
        const float lo = std::min(ambSpeedRaw[0], ambSpeedRaw[1]), hi = std::max(ambSpeedRaw[0], ambSpeedRaw[1]);
        raw = std::clamp(ambSpeedRaw[2], lo, hi);
        ambSpeedEased += (raw - ambSpeedEased) * (1.0f - expf(-std::max(dt, 0.0f) / 0.3f));
        a.set(ambD_.speed, ambSpeedEased);
        a.set(ambD_.eas, ambSpeedEased * sqrtf(expf(-std::max(eyeAsl, 0.0f) / 8500.0f)));
    }

    // City brightness under the camera: bilinear night-lights luminance, the dome's response curve.
    float urban = 0.0f;
    if (!earthNightCpu.empty())
    {
        const double u = (lon + glm::pi<double>()) / (2.0 * glm::pi<double>());
        const double v = (0.5 * glm::pi<double>() - lat) / glm::pi<double>();
        const double fx = u * earthNightCpuW - 0.5, fy = v * earthNightCpuH - 0.5;
        const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
        const float tx = (float)(fx - x0), ty = (float)(fy - y0);
        auto px = [&](int x, int y)
        {
            x = ((x % earthNightCpuW) + earthNightCpuW) % earthNightCpuW;
            y = std::clamp(y, 0, earthNightCpuH - 1);
            return earthNightCpu[(size_t)y * earthNightCpuW + x] / 255.0f;
        };
        const float lum = glm::mix(glm::mix(px(x0, y0), px(x0 + 1, y0), tx), glm::mix(px(x0, y0 + 1), px(x0 + 1, y0 + 1), tx), ty);
        const float raw = std::max(0.0f, lum - 0.06f);
        urban = raw / (raw + 0.08f);
    }
    a.set(ambD_.urban, urban);

    // Land cover from the day map, a 3x3 texel average (~60 km) under the camera.
    {
        float veg = 0.0f, forest = 0.0f, desert = 0.0f, ice = 0.0f;
        if (!earthDayCpu.empty())
        {
            const int cx = (int)std::floor((lon + glm::pi<double>()) / (2.0 * glm::pi<double>()) * earthDayCpuW);
            const int cy = std::clamp((int)((0.5 * glm::pi<double>() - lat) / glm::pi<double>() * earthDayCpuH), 0,
                                      earthDayCpuH - 1);
            float rgb[3] = {0, 0, 0};
            for (int dy = -1; dy <= 1; ++dy)
                for (int dx = -1; dx <= 1; ++dx)
                {
                    const int x = ((cx + dx) % earthDayCpuW + earthDayCpuW) % earthDayCpuW;
                    const int y = std::clamp(cy + dy, 0, earthDayCpuH - 1);
                    for (int k = 0; k < 3; ++k)
                        rgb[k] += earthDayCpu[((size_t)y * earthDayCpuW + x) * 3 + k] / 9.0f;
                }
            const float R = rgb[0], G = rgb[1], B = rgb[2], L = (R + G + B) / 3.0f;
            ice = smoothstepf(170.0f, 210.0f, L) * smoothstepf(15.0f, -5.0f, R - B);
            desert = smoothstepf(95.0f, 150.0f, L) * smoothstepf(10.0f, 25.0f, R - B) * smoothstepf(5.0f, -15.0f, G - R);
            forest = smoothstepf(100.0f, 55.0f, L) * smoothstepf(0.0f, 10.0f, G - R) * smoothstepf(10.0f, 22.0f, G - B);
            veg = smoothstepf(-12.0f, 5.0f, G - R) * smoothstepf(8.0f, 25.0f, G - B) * (1.0f - ice);
        }
        a.set(ambD_.veg, veg);
        a.set(ambD_.forest, forest);
        a.set(ambD_.desert, desert);
        a.set(ambD_.ice, ice);
    }

    // Reflect Orbital light at the camera's ground point: the ground spots are observer-relative,
    // so the camera sits at their origin. Same Gaussians as sat_sky.frag, without its display gain.
    float beam = 0.0f;
    if (groundBeamsMapped)
    {
        const auto *gb = static_cast<const GpuGroundBeams *>(groundBeamsMapped);
        const uint32_t n = std::min<uint32_t>(gb->count, kMaxGroundBeams);
        for (uint32_t i = 0; i < n; ++i)
        {
            const GpuGroundBeam &b = gb->entries[i];
            const float d2 = b.groundHitX * b.groundHitX + b.groundHitY * b.groundHitY;
            if (d2 > b.cutoffSq || b.weight <= 0.0f)
                continue;
            beam += b.weight * (std::exp(-0.5f * d2 * b.invFootprintSq) + 2.0f * std::exp(-0.5f * d2 * b.invCoreSq));
        }
    }
    a.set(ambD_.beam, beam);

    // Glare pad (layer beam_glare, synth "pad"): the flares glaring ON SCREEN. The same test the
    // glare sprites make (glare.vert): the bloom's log response b = log2(effectFlare) / 2, glaring
    // when it passes glareThreshold, and inside the view — with a soft frame edge (full at 95% of the
    // half-extent, none past 115%) so a flare sliding out of frame fades rather than cutting off. It
    // is what you SEE: the first cut drove the beam sound from beams near the camera's line of sight,
    // and flying through the atmosphere put you next to beams you were not looking at, everywhere.
    // Not tested: occlusion by terrain or cloud (the sprite tests it per pixel on the GPU).
    {
        float n = 0.0f, sum = 0.0f;
        int cnt = 0;
        if (glareReadMapped && ctx_)
        {
            auto *g = static_cast<GpuOceanGlintBuf *>(glareReadMapped);
            const uint32_t count = std::min<uint32_t>(g->count, (uint32_t)kMaxOceanGlints);
            const glm::mat3 view(camera.viewMatrix());
            const float tanHalf = tanf(glm::radians(camera.fovYDeg) * 0.5f);
            const float aspect = (float)ctx_->swapExtent.width / (float)std::max(1u, ctx_->swapExtent.height);
            for (uint32_t i = 0; i < count; ++i)
            {
                const glm::vec4 e = g->entries[i];
                const float s = std::clamp(log2f(std::max(e.w, 1.0f)) * 0.5f, 0.0f, 4.0f) - glareThreshold;
                if (s <= 0.0f)
                    continue;
                const glm::vec3 c = view * glm::vec3(e);
                if (c.z >= -1e-3f)
                    continue;
                const float nx = c.x / (-c.z) / (tanHalf * aspect), ny = c.y / (-c.z) / tanHalf;
                const float edge = smoothstepf(1.15f, 0.95f, std::max(std::fabs(nx), std::fabs(ny)));
                if (edge <= 0.0f)
                    continue;
                n += std::min(1.0f, s / 0.4f) * edge;
                sum += s * edge;
                ++cnt;
            }
            g->count = 0; // consumed: a frame whose flare pass did not run reads an empty list
        }
        ambGlareNRaw = n;
        ambGlareSumRaw = sum;
        ambGlareCount = cnt;
        auto ease = [dt](float cur, float raw)
        { return cur + (raw - cur) * (1.0f - expf(-std::max(dt, 0.0f) / (raw > cur ? 0.1f : 0.6f))); };
        ambGlareN = ease(ambGlareN, n);
        ambGlareSum = ease(ambGlareSum, sum);
        a.set(ambD_.glareN, ambGlareN);
        a.set(ambD_.glareSum, ambGlareSum);
    }

    // The upwell's other sources (2026-10-21, issue 20261021_002542): burning rockets, sunlit stages, re-entry
    // fireballs, collision dust — listed by the code that draws them (ambUpwellSource, last frame's). Each counts its
    // apparent flux 10^(-0.4 m) (m at the camera, no daylight suppression: an engine is as present by day) x
    // max(on screen with the glare's soft frame edge, near: full within the kind's near range, none past 6x it), so a
    // launch watched from the pad or followed rises fully whichever way the camera looks, and a faint dot does
    // nothing. Summed per kind, eased (up 0.15 s, down 0.8 s) and published as equivalent magnitudes (30 = none).
    {
        static const float kNearM[4] = {8000.0f, 1500.0f, 15000.0f, 20000.0f};
        float flux[4] = {};
        int cnt[4] = {};
        if (ctx_)
        {
            const glm::mat3 view(camera.viewMatrix());
            const float tanHalf = tanf(glm::radians(camera.fovYDeg) * 0.5f);
            const float aspect = (float)ctx_->swapExtent.width / (float)std::max(1u, ctx_->swapExtent.height);
            for (const AmbUpSrc &u : ambUpSrc_)
            {
                if (u.kind > 3)
                    continue;
                float w = 0.0f;
                const glm::vec3 c = view * u.dir;
                if (c.z < -1e-3f)
                {
                    const float nx = c.x / (-c.z) / (tanHalf * aspect), ny = c.y / (-c.z) / tanHalf;
                    w = smoothstepf(1.15f, 0.95f, std::max(std::fabs(nx), std::fabs(ny)));
                }
                w = std::max(w, smoothstepf(6.0f * kNearM[u.kind], kNearM[u.kind], u.rangeM));
                if (w <= 0.0f)
                    continue;
                flux[u.kind] += w * std::pow(10.0f, -0.4f * std::max(u.mag, -40.0f));
                ++cnt[u.kind];
            }
        }
        ambUpSrc_.clear(); // consumed: a frame whose visuals did not run reads nothing
        const int drv[4] = {ambD_.rocketMag, ambD_.stageMag, ambD_.fireballMag, ambD_.dustMag};
        for (int k = 0; k < 4; ++k)
        {
            ambUpFluxRaw_[k] = flux[k];
            ambUpCount_[k] = cnt[k];
            // Eased in log space (the flux spans tens of magnitudes): up in ~0.15 s, down in ~0.8 s.
            const float cur = ambUpFlux_[k], raw = flux[k];
            const float lc = log10f(std::max(cur, 1e-12f)), lr = log10f(std::max(raw, 1e-12f));
            const float l = lc + (lr - lc) * (1.0f - expf(-std::max(dt, 0.0f) / (lr > lc ? 0.15f : 0.8f)));
            ambUpFlux_[k] = dt > 0.0f ? (l <= -11.9f ? 0.0f : powf(10.0f, l)) : raw;
            a.set(drv[k], ambUpFlux_[k] > 1e-12f ? std::min(-2.5f * log10f(ambUpFlux_[k]), 30.0f) : 30.0f);
        }
    }

    // Beam-site hum (layer beam_site_hum, synth "bass"): converged beams on sites near the listener,
    // summed in the beam readback loop (SatelliteSim.cpp). Eased ~1.5 s: a Go to moves it in a frame.
    ambBeamSite = (ambBeamSite < 0.0f || dt <= 0.0f)
                      ? ambBeamSiteRaw
                      : ambBeamSite + (ambBeamSiteRaw - ambBeamSite) * (1.0f - expf(-dt / 1.5f));
    a.set(ambD_.beamSite, ambBeamSite);

    // Music gap (see ambMusicGap in SatelliteSim.h). "Audible" is the player running AND the bus
    // actually carrying it: music volume 0, or the altitude fade in high orbit, count as no music.
    {
        float target = 1.0f;
        if (audio_ && audio_->musicOn() && !audio_->musicPaused() && audio_->getMusicVolume() * musicAltFade > 0.03f)
        {
            if (audio_->trackPlaying())
                target = 0.0f;
            else if (audio_->gapRemaining() > 0.0f)
            {
                const float ph = std::clamp(1.0f - audio_->gapRemaining() / std::max(audio_->musicGap(), 1e-3f), 0.0f, 1.0f);
                target = 1.0f - std::fabs(2.0f * ph - 1.0f);
            }
            else
                target = 0.0f; // between a gap's end and the next track's first frame
        }
        const float step = 0.25f * std::max(dt, 0.0f);
        ambMusicGap = (ambMusicGap < 0.0f) ? target : std::clamp(target, ambMusicGap - step, ambMusicGap + step);
        a.set(ambD_.musicGap, ambMusicGap);
    }

    // Auroral oval under the camera: the shaders' oval (aurora_oval.glsl, its CPU mirror) without the
    // curtain/coverage noise — a hum wants a smooth region, not patches. Clamped to 1 (a storm's band is
    // brighter than that).
    {
        const float band = std::min(auroraOvalWeightCpu(auroraGpu_, up), 1.0f);
        a.set(ambD_.aurora, auroraGain > 0.0f ? band : 0.0f);
    }

    // Wind: two octaves of value noise over the ECEF direction (~500 km cells, no dateline seam)
    // and sim time (a few hours). Deterministic for a given place and time.
    {
        const double tH = ((double)simDayJ2000 * 86400.0 + simSecInDay) / 3600.0;
        const glm::dvec3 p = up * 12.0;
        const float w = 0.65f * valueNoise(p.x, p.y, p.z + tH / 5.0) +
                        0.35f * valueNoise(p.x * 2.7 + 17.0, p.y * 2.7, p.z * 2.7 + tH / 1.7);
        a.set(ambD_.wind, std::clamp((w - 0.2f) / 0.6f, 0.0f, 1.0f));
    }

    // Cloudiness over the listener, straight from the CLOUD MAP — the 2D coverage the clouds are
    // drawn from (earthCloudsCpu), sampled with the same cloudPhase longitude drift the surface
    // overlay and the volumetric march use, so the weather the ear gets moves with the weather the
    // eye sees. The ground wind bed follows this (driver `cloud`): a full bed under cloud, a light
    // breeze in clear air — which is what makes the wind vary from place to place. It is the map's
    // OWN coverage, not what the `coverage` slider draws of it, and if the map failed to load the
    // driver is 1.0 (full wind) rather than a silently calmed bed.
    // Eased over ~1.5 s: the map is coarse (39 km/px), so a Go to — or the first frame after the map
    // loads — would otherwise step the bed's level by several dB in a single frame.
    {
        float raw = 1.0f;
        if (!earthCloudsCpu.empty())
        {
            const double twoPi = 2.0 * glm::pi<double>();
            double u = (lon + glm::pi<double>()) / twoPi +
                       cloudDriftPhase() / twoPi;
            u -= std::floor(u);
            const double v = (0.5 * glm::pi<double>() - lat) / glm::pi<double>();
            const double fx = u * earthCloudsCpuW - 0.5, fy = v * earthCloudsCpuH - 0.5;
            const int x0 = (int)std::floor(fx), y0 = (int)std::floor(fy);
            const float tx = (float)(fx - x0), ty = (float)(fy - y0);
            auto px = [&](int x, int y)
            {
                x = ((x % earthCloudsCpuW) + earthCloudsCpuW) % earthCloudsCpuW;
                y = std::clamp(y, 0, earthCloudsCpuH - 1);
                return earthCloudsCpu[(size_t)y * earthCloudsCpuW + x] / 255.0f;
            };
            raw = glm::mix(glm::mix(px(x0, y0), px(x0 + 1, y0), tx),
                           glm::mix(px(x0, y0 + 1), px(x0 + 1, y0 + 1), tx), ty);
        }
        ambCloudEased = (ambCloudEased < 0.0f || dt <= 0.0f)
                            ? raw
                            : ambCloudEased + (raw - ambCloudEased) * (1.0f - expf(-dt / 1.5f));
        a.set(ambD_.cloud, std::clamp(ambCloudEased, 0.0f, 1.0f));
    }

    // ── Rain: the cloud field's rain rate around the eye (0 drizzle-free .. 1 a Cb core): the mean of the rain map's
    // central 6x6 cells (+-120 m; cloud_v2_lightning.comp, the flash buffer, read back a frame later) — the map the
    // drops are drawn from, so what you hear is what falls on screen, and a shaft is heard as it comes near. ──
    {
        // Snow falls silently: the rain sound only for the liquid share (as the streaks' temperature).
        const float liquid = glm::smoothstep(-1.0f, 3.0f, cv2EyeTempC);
        float raw = 0.0f;
        // (Only below 6 km: the map is refreshed by the lightning pass, which is not dispatched once the far
        // cloud layer takes over from orbit — a jump there would otherwise keep the last rain audible.)
        if (cv2FlashMapped && cloudsV2LightningActive() && (double)obsEyeRadiusM() - satphot::kEarthRadiusM < 6000.0)
        {
            const float *map = (const float *)((const char *)cv2FlashMapped + kCv2RainMapOffset);
            float sum = 0.0f;
            for (int j = 13; j < 19; ++j)
                for (int i = 13; i < 19; ++i)
                    sum += map[j * 32 + i];
            raw = std::isfinite(sum) ? std::clamp(sum / 36.0f, 0.0f, 1.0f) * liquid : 0.0f;
        }
        ambRainEased += (raw - ambRainEased) * (dt > 0.0f ? 1.0f - expf(-dt / 2.0f) : 1.0f);
        a.set(ambD_.rain, ambRainEased);

        // The surface wind at the eye (2026-10-23, SatelliteSimRain.cpp surfaceWindAt) — the wind the drops and flakes
        // are drawn in: `gust` the gust fronts' envelope (0 calm .. ~1.5; a 1-2 s onset, so the wind_gust layer
        // swells with the gust the rain is seen to lean in), `surface_wind` its speed / 15 m/s, `rain_driven` the rain
        // rate pushed up to +60% by a gust (wind-driven rain lands harder: the rain layer's intensity).
        const SurfaceWind sw = surfaceWindForAmbience();
        const float gust = std::isfinite(sw.gust) ? std::clamp(sw.gust, 0.0f, 2.0f) : 0.0f;
        const float spd = glm::length(sw.v);
        a.set(ambD_.gust, gust);
        a.set(ambD_.surfaceWind, std::isfinite(spd) ? std::clamp(spd / 15.0f, 0.0f, 2.0f) : 0.0f);
        a.set(ambD_.rainDriven, std::min(ambRainEased * (1.0f + 0.6f * std::min(gust, 1.0f)), 1.2f));
    }

    // A tornado's roar (2026-10-23, issue 20261023_223907: "there isn't a train-like roar when tornados are in proximity"):
    // driver `tornado` = the summed loudness of the funnels on the ground near the listener, 1 / (1 + (d / 2 km)^2) each
    // (d to the nearest point of the funnel, from the ground to its cloud base), x sqrt(its width / 150 m) (a waterspout
    // half), x its lifecycle (from touchdown, gone as it ropes out and dissipates). Layer tornado_roar.
    {
        const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
        float loud = 0.0f;
        for (const TornadoNow &T : tornadoesNow)
        {
            const double life = std::max(T.t1 - T.t0, 1.0), tau = tNow - T.t0;
            if (tau < 0.0 || tau > life) continue;
            const double lower = glm::smoothstep(0.0, std::min(90.0, 0.2 * life), tau);
            const double fade = 1.0 - glm::smoothstep(0.85, 1.0, tau / life);
            const double dh = std::acos(std::clamp(glm::dot(up, T.earthDir), -1.0, 1.0)) * kReM;
            const double dz = std::max(0.0, (double)(eyeAsl - ground) - (double)std::max(T.baseAglM, 300.0f));
            const double d = std::sqrt(dh * dh + dz * dz);
            const double size = std::sqrt(std::max((double)T.r0, 10.0) / 150.0) * (T.spout ? 0.5 : 1.0);
            loud += (float)(size * lower * lower * fade * (double)T.present / (1.0 + (d / 2000.0) * (d / 2000.0)));
        }
        a.set(ambD_.tornado, std::min(loud, 2.0f));
    }

    // ── Satellite shells ─────────────────────────────────────────────────────────────────────
    // Closed form, not a search over the roster. For a Walker shell of N satellites at radius R and
    // inclination i, the surface density at latitude phi is N / (2 pi^2 R^2 sqrt(sin^2 i - sin^2 phi))
    // (satellites linger near +-i); a RandomShell spreads N over the |phi| < i band. The count within
    // hearing range H of a camera at radial distance dr from the shell is density x pi (H^2 - dr^2),
    // and the nearest is ~ sqrt(dr^2 + (0.5 / sqrt(density))^2). A Disk (one orbital plane, rings)
    // uses its plane: z = out-of-plane distance, the in-plane annulus density N / (pi (R1^2 - R0^2)).
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const glm::dvec3 cam(obsECI);
    const double camR = glm::length(cam);
    const double camLat = std::asin(std::clamp(cam.z / std::max(camR, 1.0), -1.0, 1.0));
    for (size_t g = 0; g < a.shellGroups().size() && g < ambGroupConsts_.size(); ++g)
    {
        const auto &grp = a.shellGroups()[g];
        const double H = grp.hearingM;
        double count = 0.0, nearest = 1e9;
        bool followedInGroup = false;
        // N satellites spread over the annulus [R0, R1] of the plane with normal nrm.
        auto planeBand = [&](const glm::dvec3 &nrm, double R0, double R1, double N)
        {
            const double z = glm::dot(cam, nrm);
            const double rho = glm::length(cam - nrm * z);
            const double sigma = N / (glm::pi<double>() * std::max(R1 * R1 - R0 * R0, 1.0));
            const double outside = std::max({0.0, R0 - rho, rho - R1});
            const double a2 = H * H - z * z;
            if (a2 > 0.0)
            {
                const double ar = std::sqrt(a2);
                const double frac = std::clamp((std::min(rho + ar, R1) - std::max(rho - ar, R0)) / (2.0 * ar), 0.0, 1.0);
                count += sigma * glm::pi<double>() * a2 * frac;
            }
            const double spacing = 0.5 / std::sqrt(sigma);
            nearest = std::min(nearest, std::sqrt(z * z + (outside + spacing) * (outside + spacing)));
        };
        // N satellites on a shell of radius R (+- halfM) at inclination incl; Walker or random.
        auto inclinedShell = [&](double R, double halfM, double incl, double N, bool random)
        {
            double iEff = incl;
            if (iEff > glm::half_pi<double>())
                iEff = glm::pi<double>() - iEff;
            const double absLat = std::fabs(camLat);
            const double reach = std::clamp((iEff + 0.05 - absLat) / 0.1, 0.0, 1.0);
            double sigma = 0.0;
            if (reach > 0.0)
            {
                if (random)
                    sigma = N / (4.0 * glm::pi<double>() * R * R * std::max(std::sin(iEff), 0.05));
                else
                {
                    const double si = std::sin(iEff), sp = std::sin(absLat);
                    const double eps = std::max(2.0 * si * std::cos(iEff) * 0.035, 0.004);
                    sigma = N / (2.0 * glm::pi<double>() * glm::pi<double>() * R * R * std::sqrt(std::max(si * si - sp * sp, eps)));
                }
                sigma *= reach * reach * (3.0 - 2.0 * reach);
            }
            const double dr = std::max(0.0, std::fabs(camR - R) - halfM);
            if (sigma > 0.0)
            {
                const double a2 = H * H - dr * dr;
                if (a2 > 0.0)
                    count += sigma * glm::pi<double>() * a2;
                const double spacing = 0.5 / std::sqrt(sigma);
                nearest = std::min(nearest, std::sqrt(dr * dr + spacing * spacing));
            }
        };
        for (int ci : ambGroupConsts_[g])
        {
            const ConstellationConfig &c = constellations[ci];
            if (!c.enabled || c.orbitCount == 0)
                continue;
            if (followActive && followSatIndex >= (int)c.orbitStart && followSatIndex < (int)(c.orbitStart + c.orbitCount))
                followedInGroup = true;
            // The deployment era (2026-10-21): only the satellites there now count (rosterLife; 1 with the era off).
            const double N = (double)c.orbitCount * rosterRangePresentFrac(c.orbitStart, c.orbitStart + c.orbitCount, t);
            if (N <= 0.0)
                continue;
            if (c.distribution == OrbitDistribution::Disk)
            {
                // The rings are NOT one plane: an SSO ring's inclination follows its own altitude,
                // so across a 1400 km-deep disk the outer rings are tilted several degrees from the
                // inner ones (hundreds of km at that radius). Use the plane of the ring nearest the
                // camera's radius — its first satellite, in initConstellation()'s ring order — and
                // that ring's share of the satellites as a band ringSpacing wide.
                const int nr = std::max(1, c.numRings);
                const double spacingM = std::max((double)c.ringSpacingM, 1.0);
                const double Rc = kReM + c.altM;
                const int k = std::clamp((int)std::lround((camR - Rc) / spacingM + 0.5 * (nr - 1)), 0, nr - 1);
                const uint32_t perRing = (c.orbitCount + (uint32_t)nr - 1) / (uint32_t)nr;
                const uint32_t first = c.orbitStart + std::min((uint32_t)k * perRing, c.orbitCount - 1);
                const SatOrbitState st = satOrbitStateAt(orbitElemsOf(satOrbits[first]), t);
                const glm::dvec3 nrm = glm::normalize(glm::cross(st.posEci, st.velocity));
                const double half = 0.5 * (nr - 1) * spacingM + c.altJitterM;
                planeBand(nrm, Rc - half - 0.5 * spacingM, Rc + half + 0.5 * spacingM, N);
            }
            else if (c.distribution == OrbitDistribution::Shells)
            {
                // Per filed group, in initConstellation()'s order (shell, plane, satellite). An SSO
                // group's planes are bands of their own (each node of the X-ring), oriented by the
                // shell nearest the camera's radius; a Walker group is one inclined shell.
                uint32_t gStart = c.orbitStart;
                for (const ShellGroup &sg : c.groups)
                {
                    const uint32_t perShell = (uint32_t)(sg.planes * sg.perPlane);
                    const uint32_t gCount = (uint32_t)sg.shells * perShell;
                    if (gCount == 0 || gStart + gCount > c.orbitStart + c.orbitCount)
                        break;
                    const double R0 = kReM + sg.altMinM, R1 = kReM + sg.altMaxM;
                    const double gap = sg.shells > 1 ? (R1 - R0) / (sg.shells - 1) : 1000.0;
                    // The group's own deployed share (Starmind fills its shells one after another).
                    const double gFrac = rosterRangePresentFrac(gStart, gStart + gCount, t);
                    if (gFrac <= 0.0)
                    {
                        gStart += gCount;
                        continue;
                    }
                    if (sg.sso)
                    {
                        const int k = std::clamp((int)std::lround((camR - R0) / std::max(gap, 1.0)), 0, sg.shells - 1);
                        for (int p = 0; p < sg.planes; ++p)
                        {
                            const uint32_t first = gStart + (uint32_t)k * perShell + (uint32_t)p * (uint32_t)sg.perPlane;
                            const SatOrbitState st = satOrbitStateAt(orbitElemsOf(satOrbits[first]), t);
                            const glm::dvec3 nrm = glm::normalize(glm::cross(st.posEci, st.velocity));
                            planeBand(nrm, R0 - 0.5 * gap, R1 + 0.5 * gap, (double)sg.shells * sg.perPlane * gFrac);
                        }
                    }
                    else
                        inclinedShell(0.5 * (R0 + R1), 0.5 * (R1 - R0), 0.5 * ((double)sg.inclMin + sg.inclMax), (double)gCount * gFrac, false);
                    gStart += gCount;
                }
            }
            else
            {
                inclinedShell(kReM + c.altM, (double)c.altJitterM, c.incl, N,
                              c.distribution == OrbitDistribution::RandomShell);
            }
        }
        if (followedInGroup)
            nearest = std::min(nearest, glm::length(followOffset));
        a.set(grp.countDriver, (float)count);
        a.set(grp.nearDriver, (float)nearest);
    }
}

void SatelliteSim::updateAmbience(float dt)
{
    FPROF_SCOPE("updateAmbience");
    if (!ambience_)
    {
        FPROF_SCOPE("ambience.init");
        initAmbience();
    }
    if (!ambience_->loaded())
        return;
    computeAmbienceContext(dt);
    updateTonality(dt);
    ambience_->setFadeScales(ambFadeInScale, ambFadeOutScale);
    ambience_->setRootHz((float)std::exp2(tonal_.rootLog2));
    ambience_->setTonal(tonal_.scale, tonal_.chord, ambience_->tonality().tension);
    for (size_t g = 0; g < Ambience::groupNames().size() && g < 6; ++g)
        ambience_->setGroupGain(Ambience::groupNames()[g], ambGroupGain[g]);
    {
        FPROF_SCOPE("ambience.update");
        ambience_->update(dt, audio_);
    }
    updateThunder(dt);

    // The playing track's upwell stem: its gain is the table's "music_upwell" map of the glare on
    // screen, burning rockets, fireballs and collision dust (the loudest source), slewed linearly (up over fade_in_s, down over fade_out_s, full scale). Moving between
    // tracks it keeps its value, so a glare that outlasts a track carries into the next one's stem.
    if (audio_)
    {
        float target = 0.0f;
        const bool haveUpwell = ambience_->upwellTarget(target);
        if (showIntro)
            target = 0.0f; // never during the intro: the cinematic is cut to Gravity Wave as written
        if (haveUpwell)
        {
            const float up = std::max(dt, 0.0f) / ambience_->upwellFadeInS();
            // A replayed intro takes the stem away within a second, not over the usual fade.
            const float down = std::max(dt, 0.0f) / (showIntro ? 1.0f : ambience_->upwellFadeOutS());
            ambUpwellGain = std::clamp(target, ambUpwellGain - down, ambUpwellGain + up);
        }
        else
            ambUpwellGain = 0.0f;
        audio_->setUpwellGain(ambUpwellGain);

        // The intro is Gravity Wave's: the whole ambience bus sits at half while it plays, and comes
        // back over ~2 s when it ends (skipped or finished).
        const float introTarget = showIntro ? 0.5f : 1.0f;
        const float introStep = 0.25f * std::max(dt, 0.0f);
        ambIntroFade = (ambIntroFade < 0.0f) ? introTarget
                                             : std::clamp(introTarget, ambIntroFade - introStep, ambIntroFade + introStep);
        audio_->setAmbienceFade(ambIntroFade);
    }

    // Music makes room for the high-orbit ambience: full through LEO, half by 5000 km (MEO), silent
    // from 35786 km (GEO, the start of HEO) up — linear in log altitude, so it is a slow, even fade
    // across MEO rather than a drop. Slewed at 1/4 per second (a Go to jump fades over ~4 s).
    if (audio_)
    {
        const float alt = ambience_->get(ambD_.altM);
        const float kFull = 1.5e6f, kHalf = 5.0e6f, kSilent = 3.5786e7f;
        float target = 1.0f;
        if (alt >= kSilent)
            target = 0.0f;
        else if (alt > kHalf)
            target = 0.5f * (1.0f - log10f(alt / kHalf) / log10f(kSilent / kHalf));
        else if (alt > kFull)
            target = 1.0f - 0.5f * log10f(alt / kFull) / log10f(kHalf / kFull);
        const float step = 0.25f * std::max(dt, 0.0f);
        musicAltFade = dt <= 0.0f ? target
                                  : std::clamp(target, musicAltFade - step, musicAltFade + step);
        audio_->setMusicFade(musicAltFade);
    }
}

// ── Thunder ────────────────────────────────────────────────────────────────────────────────────────
// Each flash's channel is a sound source (SatelliteSimLightning.cpp addThunderSource) whose front left it at t0
// and travels at 343 m/s in SIM time. A piece of channel at distance d from the listener is due when the front has
// gone that far; the roll is played by a PLAYHEAD per source (2026-11-21, thunderStep): the path difference behind
// the channel's nearest piece heard so far, which starts when the front reaches the listener, follows it, never
// runs back and never goes faster than 3x sound.
//   * standing still, the roll is the channel's geometry (near pieces first: a clap for a channel seen
//     broadside, a long rumble for one running away);
//   * flying toward the storm the sound comes sooner, compressed at most 3x; outrunning it (the player moves at
//     many times the speed of sound) it waits, and plays when the front catches up; a roll is heard once;
//   * paused nothing arrives and the roll goes on at release; time warp plays it faster; a jump (a teleport, > 20 s
//     of sim time in a frame) arrives silently;
//   * the delay is capped at 3 s unless "Distant sounds arrive within 3 s" (Sound tab) is off.
// (Until 2026-11-21 a roll was the pieces whose g = d - 343 (t - t0) changed sign between two frames: a listener
// faster than sound got a blip or nothing, and in a busy storm flashes were dropped: docs/subsystems/sound.md.)
// A piece's power is its length x weight / d^2, more across the line of sight (a tortuous channel radiates
// mostly sideways); the level sent to the synth is sqrt(power per second, eased), so it needs no knowledge of the
// roll's peak. A synth voice per sounding flash, shared when there are more flashes than voices (AmbientSynth
// thunder, pushEvent).
void SatelliteSim::updateThunder(float dt)
{
    if (!ambience_)
        return;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // The eye as the lightning's (double: the float radius steps by 0.5 m, a third of a frame of sound).
    const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
    const glm::dvec3 eye = glm::normalize(glm::dvec3(obsDir)) * (6371000.0 + std::max((double)ground + 2.0, (double)obsHeightOffset));
    thunderRecording_ = false;
    thunderStep(now, eye, dt);
}

void SatelliteSim::thunderRecordTick(float dt)
{
    if (!thunderPrevValid_)
        return;
    if (!thunderRecording_)
    {
        thunderRecording_ = true;
        thunderRecordS_ = thunderPrevS_;
    }
    thunderRecordS_ += (double)dt;
    thunderStep(thunderRecordS_, thunderPrevEye_, dt);
}

void SatelliteSim::thunderStep(double now, const glm::dvec3 &eye, float realDt)
{
    FPROF_SCOPE("thunderStep");
    constexpr double kC = 343.0;
    constexpr float kGain = 270.0f;   // sqrt(m of channel / m^2 / s) -> the synth's level (~4 for a strike 1 km off)
    // The delay: distance / 343 m/s, and with "Distant sounds arrive within 3 s" (the default) never more than that:
    // the front counts as arrived once it has gone min(distance, 3 s of sound).
    const double capM = ambSoundQuick ? kSoundQuickS * kC : 1e18;
    thunderRealT_ += std::max((double)realDt, 0.0);
    const bool resync = !thunderPrevValid_ || std::abs(now - thunderPrevS_) > 20.0 ||
                        glm::length(eye - thunderPrevEye_) > 5000.0;
    const double nowP = resync ? now : thunderPrevS_;
    thunderPrevS_ = now;
    thunderPrevEye_ = eye;
    thunderPrevValid_ = true;
    const double dS = now - nowP;
    const glm::dvec3 camR = camRightEcef(); // left / right = the screen's right (rolls with the camera; 2026-11-14)
    if (!thunderSimulating_)
    {
        // Satellite collisions and the launches' thumps (their own layer: heard in orbit too), then the rockets.
        impactStep(nowP, now, realDt, eye, camR, resync);
        rocketStep(now, eye, camR, realDt);
    }

    // A source is done once its shell is ~50 km out (thunder is not heard that far), or time went back past it.
    for (size_t i = 0; i < thunderSources_.size();)
    {
        const double age = now - thunderSources_[i].t0;
        if (age < -1.0 || age > 150.0)
        {
            if (thunderLogOn_)
                if (ThunderStat *st = thunderStat(thunderSources_[i].id))
                    st->end = 1;
            thunderSources_.erase(thunderSources_.begin() + (long)i);
        }
        else
            ++i;
    }

    const int li = ambience_ ? ambience_->layerIndex("thunder") : -1;
    const int vi = (li >= 0) ? ambience_->layerVoice(li) : -1;
    AmbientSynth *syn = thunderSink_ ? thunderSink_ : (audio_ && vi >= 0) ? audio_->voiceSynth(vi) : nullptr;
    const float layerGain = thunderSimulating_ ? thunderSimGain_ : (li >= 0 ? ambience_->layerCurrent(li) : 0.0f);

    bool sounded[kThunderVoices] = {};
    bool near = false;
    thunderLevelNow_ = 0.0f;

    // Thunder. Each source's roll is played by a PLAYHEAD (2026-11-21): the path difference, in metres behind the
    // channel's nearest piece, that has been heard so far. It follows the front (w = 343 (t - t0) - the nearest
    // piece's distance from the listener NOW), never runs back, and never faster than 3x sound, so every roll whose
    // front reaches the listener is heard once and whole, whatever the frame time and however the listener moved.
    struct Sounding
    {
        ThunderSource *s;
        float level, dKm, pan;
        int voice;
    };
    static thread_local std::vector<Sounding> snd;
    static thread_local std::vector<float> dist;
    snd.clear();
    const float kAtt = 1.0f - std::exp(-std::max(realDt, 0.0f) / 0.015f), kRel = 1.0f - std::exp(-std::max(realDt, 0.0f) / 0.08f);
    for (ThunderSource &s : thunderSources_)
    {
        if (s.done)
            continue;
        const glm::dvec3 rel = s.origin - eye;
        const double dC = glm::length(rel), ext = (double)s.extentM;
        const double R = kC * (now - s.t0);
        s.lastDM = (float)dC;
        if (s.play < 0.0)
        {
            // Not arriving within ~2 s, or out of hearing: the centre's bound is enough. `before`: the front has
            // certainly not reached the listener yet (the roll starts only when it then does: a genuine arrival).
            if (R < std::min(dC - ext, capM) - 700.0)
            {
                s.before = s.seen = true;
                continue;
            }
            if (dC - ext > 45000.0)
            {
                s.before = R < std::min(dC - ext, capM);
                s.seen = true;
                continue;
            }
        }
        // The pieces' distances from the listener now: the nearest is where the roll starts.
        const glm::vec3 r(rel);
        const size_t n = s.pts.size();
        dist.resize(n);
        float dMinF = 1e30f, dMaxF = 0.0f;
        size_t iMinPiece = 0;
        for (size_t i = 0; i < n; ++i)
        {
            const float d = glm::length(r + glm::vec3(s.pts[i]));
            dist[i] = d;
            if (d < dMinF)
            {
                dMinF = d;
                iMinPiece = i;
            }
            dMaxF = std::max(dMaxF, d);
        }
        const double dMin = dMinF, span = (double)dMaxF - dMin;
        const size_t iMin = iMinPiece;
        const double w = R - std::min(dMin, capM); // how far the front is past the nearest piece (or past the cap)
        if (s.play < 0.0)
        {
            if (w < 0.0)
            {
                near = near || (w > -700.0 && dMin < 45000.0); // the layer is up before the first clap
                s.before = s.seen = true;
                s.passed = false;
                continue;
            }
            // The front is past the nearest piece and nothing of it was heard. It starts if it arrived just now (it
            // was still ahead on the last step, or the source is new and the strike that close). Otherwise a jump (a
            // teleport, a time jump) or a flight in from out of hearing put the listener behind it: it passed
            // silently, unless the listener gets ahead of it again.
            const bool arrived = s.before || (!s.seen && w <= span + 60.0);
            s.seen = true;
            if (resync || !arrived)
            {
                s.before = false;
                s.passed = true;
                continue;
            }
            if (dS <= 0.0)
            {
                near = s.before = true; // time held with the front at the listener: it starts when time runs
                continue;
            }
            s.before = s.passed = false;
            s.play = 0.0;
        }
        // The strike (2026-11-21): as the front of a channel within 8 km arrives (the roll starts), its snap, crackle
        // and boom, once (the synth shapes them by the distance; farther off the roll alone is the thunder).
        if (!s.struck && s.play == 0.0)
        {
            s.struck = true;
            if (dMin < 8000.0 && syn)
            {
                const glm::dvec3 toMin = glm::dvec3(r + glm::vec3(s.pts[iMin])) / std::max(dMin, 1.0);
                const float ev[5] = {0.0f, s.energy, (float)(dMin / 1000.0), (float)glm::dot(toMin, camR), 2.0f};
                syn->pushEvent(ev, 5);
                ++thunderStrikes_;
            }
        }
        if (resync)
        {
            s.play = std::max(s.play, w); // what the jump skipped is gone
            s.pw = 0.0f;
        }
        double P = 0.0, sumD = 0.0;
        glm::dvec3 sumDir(0.0);
        const double step = dS > 0.0 ? std::min(w - s.play, 3.0 * kC * dS) : 0.0;
        if (step > 0.0)
        {
            const float u0 = (float)s.play, u1 = (float)(s.play + step);
            for (size_t i = 0; i < n; ++i)
            {
                const float u = dist[i] - dMinF, hl = s.dirh[i].w;
                if (u + hl + 3.0f < u0 || u - hl - 3.0f > u1)
                    continue;
                const float rr = std::max(dist[i], 30.0f);
                const glm::vec3 wN = r + glm::vec3(s.pts[i]);
                const float c = glm::dot(glm::vec3(s.dirh[i]), wN) / rr;
                // The piece spans +-h in arrival distance (its length along the line to the listener).
                const float h = std::max(hl * std::abs(c), 3.0f);
                const float ov = std::min(u1, u + h) - std::max(u0, u - h);
                if (ov <= 0.0f)
                    continue;
                const float a2 = s.pts[i].w * (0.25f + 0.75f * (1.0f - std::min(c * c, 1.0f))) / (rr * rr) * (ov / (2.0f * h));
                P += (double)a2;
                sumD += (double)(a2 * rr);
                sumDir += glm::dvec3(wN / rr) * (double)a2;
            }
            s.play += step;
        }
        if (dS > 0.0)
        {
            // Power per second of sim time (a front caught up at 3x is that much denser, as a listener closing on it
            // hears), eased: 15 ms up, 80 ms down, so the steps of the frame rate and the gaps between pieces are not
            // steps in the sound.
            const float rate = (float)(P / std::max(dS, 1.0 / 240.0));
            s.pw += (rate - s.pw) * (rate > s.pw ? kAtt : kRel);
        }
        if (P > 0.0)
        {
            s.lastKm = (float)(sumD / P) / 1000.0f;
            const glm::dvec3 dir = sumDir / std::max(glm::length(sumDir), 1e-12);
            // Left/right: what is arriving against the screen's right (a level camera: the bearing against the
            // heading, times the horizontal share; overhead = centre).
            s.lastPan = (float)glm::dot(dir, camR);
        }
        else if (s.lastKm <= 0.0f)
            s.lastKm = (float)dMin / 1000.0f;
        if (s.play >= span + 60.0 && s.pw < 1e-9f)
            s.done = true;
        const float level = dS > 0.0 ? kGain * std::sqrt(std::max(s.pw, 0.0f)) * s.energy *
                                           (1.0f - glm::smoothstep(20.0f, 40.0f, s.lastKm))
                                     : 0.0f;
        if (!s.done)
            near = true; // a roll in progress keeps the layer up (through a hold too)
        if (level <= 1e-4f)
            continue;
        snd.push_back(Sounding{&s, level, s.lastKm, s.lastPan, -1});
    }

    // Voices: a flash keeps the voice it has; a new one takes a free voice; with none free it joins the flash's
    // voice nearest in distance (powers add), so nothing that arrives is dropped and no voice changes hands mid-roll.
    struct Mix
    {
        double pw = 0.0, km = 0.0, pan = 0.0;
    } mix[kThunderVoices];
    for (Sounding &q : snd)
        for (int k = 0; k < kThunderVoices; ++k)
            if (thunderVoice_[k].used && thunderVoice_[k].id == q.s->id)
                q.voice = k;
    std::stable_sort(snd.begin(), snd.end(), [](const Sounding &a, const Sounding &b) { return (a.voice >= 0) != (b.voice >= 0) ? a.voice >= 0 : a.level > b.level; });
    for (Sounding &q : snd)
    {
        bool merged = false;
        if (q.voice < 0)
        {
            for (int k = 0; k < kThunderVoices && q.voice < 0; ++k)
                if (!thunderVoice_[k].used)
                    q.voice = k;
            if (q.voice >= 0)
            {
                thunderVoice_[q.voice] = ThunderVoice{};
                thunderVoice_[q.voice].id = q.s->id;
                thunderVoice_[q.voice].used = true;
                thunderVoice_[q.voice].km = q.dKm;
            }
            else
            {
                float best = 1e30f;
                for (int k = 0; k < kThunderVoices; ++k)
                {
                    const float e = std::abs(std::log(std::max(thunderVoice_[k].km, 0.05f) / std::max(q.dKm, 0.05f)));
                    if (e < best)
                    {
                        best = e;
                        q.voice = k;
                    }
                }
                merged = q.voice >= 0;
            }
        }
        ThunderStat *stt = thunderLogOn_ ? thunderStat(q.s->id) : nullptr;
        if (stt)
        {
            if (stt->firstS < 0.0)
            {
                stt->firstS = now;
                stt->dFirstKm = q.dKm;
            }
            stt->lastS = now;
            ++stt->fArrive;
            stt->eArrive += (double)q.level * q.level * std::max((double)realDt, 0.0);
            stt->peak = std::max(stt->peak, q.level);
            if (merged)
                ++stt->fMerged;
            if (q.voice < 0)
                ++stt->fNoVoice;
        }
        if (!q.s->heard)
        {
            q.s->heard = true;
            ++thunderRolls_;
        }
        if (q.voice < 0)
            continue;
        Mix &m = mix[q.voice];
        const double p2 = (double)q.level * q.level;
        m.pw += p2;
        m.km += p2 * q.dKm;
        m.pan += p2 * q.pan;
    }
    bool sent[kThunderVoices] = {};
    for (int k = 0; k < kThunderVoices; ++k)
    {
        if (mix[k].pw <= 0.0)
            continue;
        ThunderVoice &tv = thunderVoice_[k];
        const float level = (float)std::sqrt(mix[k].pw);
        tv.km = (float)(mix[k].km / mix[k].pw);
        sounded[k] = true;
        tv.level = level;
        tv.lastSoundT = thunderRealT_;
        thunderLevelNow_ = std::max(thunderLevelNow_, level);
        if (syn)
        {
            const float ev[4] = {(float)k, level, tv.km, (float)(mix[k].pan / mix[k].pw)};
            sent[k] = syn->pushEvent(ev, 4);
        }
    }
    if (thunderLogOn_)
        for (const Sounding &q : snd)
            if (ThunderStat *stt = thunderStat(q.s->id))
            {
                if (q.voice < 0)
                    continue;
                // (A muted harness run has no device pulling the synth: its queue fills, which is not a loss in play.)
                const bool offlineFull = !sent[q.voice] && syn && !thunderSink_ && audio_ && audio_->isOffline();
                if (!syn)
                    ++stt->fNoSynth;
                else if (!sent[q.voice] && !offlineFull)
                    ++stt->fQueueFull;
                else if (layerGain <= 1e-4f)
                    ++stt->fMuted;
                else
                {
                    ++stt->fPushed;
                    stt->ePushed += (double)q.level * q.level * std::max((double)realDt, 0.0) * (double)layerGain * layerGain;
                }
            }

    // A voice with nothing arriving this step is told so; it is free again after 0.6 s of that (the synth's own
    // release is under 0.4 s).
    for (int k = 0; k < kThunderVoices; ++k)
    {
        ThunderVoice &tv = thunderVoice_[k];
        if (!tv.used || sounded[k])
            continue;
        if (tv.level > 0.0f && syn)
        {
            const float ev[4] = {(float)k, 0.0f, std::max(tv.km, 0.05f), 0.0f};
            syn->pushEvent(ev, 4);
        }
        tv.level = 0.0f;
        if (thunderRealT_ - tv.lastSoundT > 0.6)
            tv.used = false;
    }
    // The layer's driver: up while thunder is arriving, about to, or rolled within the last 4 s.
    bool any = near;
    for (const ThunderVoice &tv : thunderVoice_)
        any = any || (tv.used && tv.level > 0.0f);
    thunderHoldS_ = any ? 4.0f : std::max(thunderHoldS_ - std::max(realDt, 0.0f), 0.0f);
    if (ambience_ && !thunderSimulating_)
        ambience_->set(ambD_.thunder, thunderHoldS_ > 0.0f ? 1.0f : 0.0f);
}

SatelliteSim::ThunderStat *SatelliteSim::thunderStat(uint32_t id)
{
    for (size_t i = thunderStats_.size(); i-- > 0;)
        if (thunderStats_[i].id == id)
            return &thunderStats_[i];
    return nullptr;
}

// A new source takes its place in the list, and with the log on its row and a kept copy. At the cap the one that
// matters least goes: a roll already heard or passed, else the source farthest from the listener (until 2026-11-21
// the OLDEST went, at 48: in a wide storm the flashes on the horizon pushed out the near ones before their sound
// had arrived).
void SatelliteSim::thunderInsertSource(ThunderSource &&s, int kind)
{
    s.kind = kind;
    if (thunderLogOn_)
    {
        const double now = thunderSimulating_ ? thunderPrevS_ : (double)simDayJ2000 * 86400.0 + simSecInDay;
        const glm::dvec3 eye = thunderPrevValid_ ? thunderPrevEye_ : thunderLogEye_;
        if (!thunderSimulating_ && thunderRec_.size() < 4000)
        {
            thunderRec_.push_back(s);
            thunderRecAddS_.push_back(now);
            thunderRecKind_.push_back(kind);
        }
        if (thunderStats_.size() < 8000)
        {
            ThunderStat st;
            st.id = s.id;
            st.kind = kind;
            st.t0 = s.t0;
            st.addS = now;
            st.d0Km = (float)(glm::length(s.origin - eye) / 1000.0);
            st.extentKm = s.extentM / 1000.0f;
            st.pieces = (uint32_t)s.pts.size();
            thunderStats_.push_back(st);
        }
    }
    if (thunderPrevValid_)
        s.lastDM = (float)glm::length(s.origin - thunderPrevEye_);
    if (thunderSources_.size() >= kThunderMaxSources)
    {
        size_t worst = 0;
        float worstKey = -1.0f;
        for (size_t i = 0; i < thunderSources_.size(); ++i)
        {
            const ThunderSource &o = thunderSources_[i];
            const float key = (o.done || o.passed) ? 1e12f : o.play >= 0.0 ? -1.0f : o.lastDM - o.extentM;
            if (key > worstKey)
            {
                worstKey = key;
                worst = i;
            }
        }
        if (worstKey < s.lastDM - s.extentM)
        {
            // The new one is the farthest of all, and every other is still to be heard: it is the one left out.
            if (thunderLogOn_)
                if (ThunderStat *st = thunderStat(s.id))
                    st->end = 2;
            return;
        }
        if (thunderLogOn_)
            if (ThunderStat *st = thunderStat(thunderSources_[worst].id))
                st->end = thunderSources_[worst].heard ? 3 : 2;
        thunderSources_.erase(thunderSources_.begin() + (long)worst);
    }
    thunderSources_.push_back(std::move(s));
}

// ── Harness: lightning thunder ... (2026-11-21, issue_20261121_163317) ──────────────────────────────────────
// What became of each flash's sound: `log on|off|clear` keeps every source made and a row per source (what arrived at
// the listener against what was sent to the synth); `state [name]` reports them (rows in captures/<name>.thunder.json);
// `save <name>` / `load <file>` write / read the kept sources (captures/<name>.thundersrc), so two builds are compared
// on the same flashes; `sim [name] [dt_ms=30] [seconds=60] [speed=0] [hold_at=] [hold_s=] [tail=6]` runs the kept
// sources again through thunderStep on a virtual clock: steps of dt_ms, the listener moving east at `speed` m/s, sim
// time held for hold_s real seconds at hold_at, the layer's gate and fades emulated, the events sent to a private
// thunder synth rendered into captures/<name>.wav. Each row also carries when the first wavefront should arrive
// (closed form for the listener's path) and the nearest piece's distance then: the reference a miss is counted against.
namespace
{
bool writeWavStereo16(const std::string &path, const std::vector<float> &lr, uint32_t sr)
{
    std::ofstream f(path, std::ios::binary);
    if (!f)
        return false;
    const uint32_t dataBytes = (uint32_t)(lr.size() * 2);
    auto u32 = [&](uint32_t v) { f.write((const char *)&v, 4); };
    auto u16 = [&](uint16_t v) { f.write((const char *)&v, 2); };
    f.write("RIFF", 4);
    u32(36 + dataBytes);
    f.write("WAVEfmt ", 8);
    u32(16);
    u16(1);
    u16(2);
    u32(sr);
    u32(sr * 4);
    u16(4);
    u16(16);
    f.write("data", 4);
    u32(dataBytes);
    for (float s : lr)
    {
        const int16_t q = (int16_t)std::lround(std::clamp(s, -1.0f, 1.0f) * 32767.0f);
        f.write((const char *)&q, 2);
    }
    return (bool)f;
}
} // namespace

nlohmann::json SatelliteSim::thunderStatsJson() const
{
    nlohmann::json rows = nlohmann::json::array();
    for (const ThunderStat &s : thunderStats_)
        rows.push_back({{"id", s.id}, {"kind", s.kind}, {"t0_s", s.t0 - thunderLogStartS_}, {"add_s", s.addS - thunderLogStartS_},
                        {"d0_km", s.d0Km}, {"extent_km", s.extentKm}, {"pieces", s.pieces},
                        {"first_s", s.firstS < 0.0 ? -1.0 : s.firstS - thunderLogStartS_},
                        {"last_s", s.lastS < 0.0 ? -1.0 : s.lastS - thunderLogStartS_}, {"d_first_km", s.dFirstKm},
                        {"f_arrive", s.fArrive}, {"f_pushed", s.fPushed}, {"f_merged", s.fMerged}, {"f_no_voice", s.fNoVoice}, {"f_no_synth", s.fNoSynth},
                        {"f_queue_full", s.fQueueFull}, {"f_muted", s.fMuted}, {"e_arrive", s.eArrive}, {"e_pushed", s.ePushed},
                        {"peak", s.peak}, {"end", s.end}});
    return rows;
}

harness::Status SatelliteSim::harnessThunder(harness::Active &a)
{
    using nlohmann::json;
    const harness::Command &c = a.cmd;
    json &r = a.result;
    const std::string sub = c.pos.size() > 1 ? c.pos[1] : std::string("state");
    const std::string arg = c.pos.size() > 2 ? c.pos[2] : std::string();
    constexpr double kC = 343.0;
    auto nowS = [&] { return (double)simDayJ2000 * 86400.0 + simSecInDay; };
    auto eyeNow = [&] {
        const float ground = (terrainFrameMapped && (debugDisableMask & 1024u) == 0) ? terrainFrameMapped[1] : obsTerrainH;
        return glm::normalize(glm::dvec3(obsDir)) * (6371000.0 + std::max((double)ground + 2.0, (double)obsHeightOffset));
    };
    // When the first wavefront of a kept source reaches a listener at eye0 + vel (t - start), and the nearest piece's
    // distance then (the listener slower than sound: one crossing, by bisection).
    auto arrival = [&](const ThunderSource &s, const glm::dvec3 &eye0, const glm::dvec3 &vel, double start, double &dMinM) {
        auto f = [&](double t, double &dm) {
            const glm::vec3 rel(s.origin - (eye0 + vel * (t - start)));
            float best = 1e30f;
            for (const glm::vec4 &p : s.pts)
            {
                const glm::vec3 w = rel + glm::vec3(p);
                best = std::min(best, glm::dot(w, w));
            }
            dm = std::sqrt((double)best);
            return std::min(dm, ambSoundQuick ? kSoundQuickS * kC : 1e18) - kC * (t - s.t0);
        };
        double lo = s.t0, hi = s.t0 + 200.0, dm = 0.0;
        if (f(lo, dm) <= 0.0)
        {
            dMinM = dm;
            return lo;
        }
        if (f(hi, dm) > 0.0)
        {
            dMinM = dm;
            return -1.0;
        }
        for (int i = 0; i < 40; ++i)
        {
            const double mid = 0.5 * (lo + hi);
            if (f(mid, dm) > 0.0)
                lo = mid;
            else
                hi = mid;
        }
        f(hi, dMinM);
        return hi;
    };
    auto truthRows = [&](json rows, const glm::dvec3 &eye0, const glm::dvec3 &vel, double start) {
        for (json &row : rows)
        {
            const uint32_t id = row["id"].get<uint32_t>();
            for (const ThunderSource &s : thunderRec_)
                if (s.id == id)
                {
                    double dm = 0.0;
                    const double ta = arrival(s, eye0, vel, start, dm);
                    row["arrive_s"] = ta < 0.0 ? -1.0 : ta - start;
                    row["dmin_km"] = dm / 1000.0;
                    break;
                }
        }
        return rows;
    };
    auto summarise = [&](const json &rows, double seconds, json &out) {
        int should = 0, heard = 0, missed = 0, noVoice = 0, noSynth = 0, muted = 0, queue = 0, evicted = 0, never = 0;
        for (const json &row : rows)
        {
            const double ta = row.value("arrive_s", -1.0);
            if (ta < 0.0 || ta > seconds - 0.5 || row.value("dmin_km", 1e9) > 30.0)
                continue;
            ++should;
            if (row["f_pushed"].get<int>() > 0)
            {
                ++heard;
                continue;
            }
            ++missed;
            if (row["end"].get<int>() == 2)
                ++evicted;
            else if (row["f_arrive"].get<int>() == 0)
                ++never;
            else if (row["f_no_voice"].get<int>() > 0)
                ++noVoice;
            else if (row["f_no_synth"].get<int>() > 0)
                ++noSynth;
            else if (row["f_queue_full"].get<int>() > 0)
                ++queue;
            else if (row["f_muted"].get<int>() > 0)
                ++muted;
        }
        out["sources"] = rows.size();
        out["should_hear"] = should;   // first wavefront arrives in the window, nearest piece within 30 km
        out["heard"] = heard;
        out["missed"] = missed;
        out["missed_by"] = {{"evicted_by_cap", evicted}, {"nothing_arrived", never}, {"no_voice", noVoice}, {"no_synth", noSynth},
                            {"queue_full", queue}, {"layer_muted", muted}};
        out["voice_steals"] = thunderSteals_;
    };

    if (sub == "log")
    {
        if (arg == "on" || arg == "clear")
        {
            thunderStats_.clear();
            thunderRec_.clear();
            thunderRecAddS_.clear();
            thunderRecKind_.clear();
            thunderSteals_ = 0;
            thunderLogStartS_ = nowS();
            thunderLogEye_ = eyeNow();
            if (arg == "on")
                thunderLogOn_ = true;
        }
        else if (arg == "off")
            thunderLogOn_ = false;
        else
        {
            a.error = "lightning thunder log on|off|clear";
            return harness::Status::Error;
        }
        r["message"] = std::string("thunder log ") + (thunderLogOn_ ? "on" : "off");
        return harness::Status::Done;
    }
    if (sub == "state")
    {
        const double seconds = nowS() - thunderLogStartS_;
        const json rows = truthRows(thunderStatsJson(), thunderLogEye_, glm::dvec3(0.0), thunderLogStartS_);
        summarise(rows, seconds, r);
        r["log"] = thunderLogOn_;
        r["seconds"] = seconds;
        r["kept_sources"] = thunderRec_.size();
        if (!arg.empty())
        {
            const std::string path = harnessRunner_->capturePath(arg, ".thunder.json");
            std::ofstream(path) << json{{"summary", r}, {"rows", rows}}.dump(1) << '\n';
            r["file"] = path;
        }
        return harness::Status::Done;
    }
    if (sub == "save" || sub == "load")
    {
        const std::string path = sub == "save" ? harnessRunner_->capturePath(arg.empty() ? "thunder" : arg, ".thundersrc") : arg;
        if (sub == "save")
        {
            std::ofstream f(path, std::ios::binary);
            const uint32_t magic = 0x52444854u, n = (uint32_t)thunderRec_.size(); // 'THDR'
            f.write((const char *)&magic, 4);
            f.write((const char *)&n, 4);
            f.write((const char *)&thunderLogStartS_, 8);
            f.write((const char *)&thunderLogEye_, 24);
            for (size_t i = 0; i < thunderRec_.size(); ++i)
            {
                const ThunderSource &s = thunderRec_[i];
                const uint32_t np = (uint32_t)s.pts.size();
                const int32_t kind = thunderRecKind_[i];
                f.write((const char *)&s.id, 4);
                f.write((const char *)&kind, 4);
                f.write((const char *)&s.t0, 8);
                f.write((const char *)&thunderRecAddS_[i], 8);
                f.write((const char *)&s.origin, 24);
                f.write((const char *)&s.extentM, 4);
                f.write((const char *)&s.energy, 4);
                f.write((const char *)&np, 4);
                f.write((const char *)s.pts.data(), (std::streamsize)np * 16);
                f.write((const char *)s.dirh.data(), (std::streamsize)np * 16);
            }
            if (!f)
            {
                a.error = "could not write " + path;
                return harness::Status::Error;
            }
        }
        else
        {
            std::ifstream f(path, std::ios::binary);
            uint32_t magic = 0, n = 0;
            f.read((char *)&magic, 4);
            f.read((char *)&n, 4);
            if (!f || magic != 0x52444854u || n > 4000)
            {
                a.error = "not a thunder source file: " + path;
                return harness::Status::Error;
            }
            thunderRec_.clear();
            thunderRecAddS_.clear();
            thunderRecKind_.clear();
            f.read((char *)&thunderLogStartS_, 8);
            f.read((char *)&thunderLogEye_, 24);
            for (uint32_t i = 0; i < n && f; ++i)
            {
                ThunderSource s;
                uint32_t np = 0;
                int32_t kind = 0;
                double addS = 0.0;
                f.read((char *)&s.id, 4);
                f.read((char *)&kind, 4);
                f.read((char *)&s.t0, 8);
                f.read((char *)&addS, 8);
                f.read((char *)&s.origin, 24);
                f.read((char *)&s.extentM, 4);
                f.read((char *)&s.energy, 4);
                f.read((char *)&np, 4);
                if (!f || np > 2000000)
                    break;
                s.pts.resize(np);
                s.dirh.resize(np);
                f.read((char *)s.pts.data(), (std::streamsize)np * 16);
                f.read((char *)s.dirh.data(), (std::streamsize)np * 16);
                thunderRec_.push_back(std::move(s));
                thunderRecAddS_.push_back(addS);
                thunderRecKind_.push_back(kind);
            }
        }
        r["file"] = path;
        r["kept_sources"] = thunderRec_.size();
        r["message"] = sub + " " + std::to_string(thunderRec_.size()) + " thunder sources";
        return harness::Status::Done;
    }
    if (sub == "sim")
    {
        if (thunderRec_.empty())
        {
            a.error = "no kept thunder sources: `lightning thunder log on`, let flashes happen, or `lightning thunder load <file>`";
            return harness::Status::Error;
        }
        const std::string name = arg.empty() ? std::string("thundersim") : arg;
        const double dt = std::clamp(c.num("dt_ms", 30.0), 1.0, 500.0) / 1000.0;
        const double seconds = std::clamp(c.num("seconds", 60.0), 1.0, 600.0), tail = std::clamp(c.num("tail", 6.0), 0.0, 60.0);
        const double speed = c.num("speed", 0.0), holdAt = c.num("hold_at", -1.0), holdS = c.num("hold_s", 0.0);
        const double jitter = std::clamp(c.num("jitter", 0.0), 0.0, 0.9); // +- this share of dt, a frame at a time
        const uint32_t sr = 48000;
        std::unique_ptr<AmbientSynth> syn = AmbientSynth::create("thunder", sr, 12345u);
        const int li = ambience_ ? ambience_->layerIndex("thunder") : -1;
        const float g0 = li >= 0 ? ambience_->layerGain(li) : 0.9f;
        const float fadeIn = li >= 0 ? ambience_->layerFadeInS(li) : 0.5f, fadeOut = li >= 0 ? ambience_->layerFadeOutS(li) : 3.0f;

        // The live state steps aside.
        std::vector<ThunderSource> keepSrc = std::move(thunderSources_);
        std::vector<ThunderStat> keepStats = std::move(thunderStats_);
        ThunderVoice keepVoice[kThunderVoices];
        std::copy(std::begin(thunderVoice_), std::end(thunderVoice_), keepVoice);
        const double keepPrevS = thunderPrevS_, keepRealT = thunderRealT_;
        const glm::dvec3 keepPrevEye = thunderPrevEye_;
        const bool keepValid = thunderPrevValid_, keepLog = thunderLogOn_;
        const float keepHold = thunderHoldS_, keepLevel = thunderLevelNow_;
        const uint32_t keepRolls = thunderRolls_, keepSteals = thunderSteals_;
        thunderSources_.clear();
        thunderStats_.clear();
        for (ThunderVoice &v : thunderVoice_)
            v = ThunderVoice{};
        thunderPrevValid_ = false;
        thunderHoldS_ = 0.0f;
        thunderSteals_ = 0;
        thunderLogOn_ = true;
        thunderSimulating_ = true;
        thunderSink_ = syn.get();

        const glm::dvec3 eye0 = thunderLogEye_, up = glm::normalize(eye0);
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up));
        const glm::dvec3 vel = east * speed;
        const double start = thunderLogStartS_;
        double simT = start, realT = 0.0, sampleAcc = 0.0;
        float gain = 0.0f;
        bool driver = false;
        size_t next = 0;
        std::vector<float> lr, buf;
        lr.reserve((size_t)((seconds + tail) * sr) * 2);
        json trace = json::array();
        uint32_t rng = 2463534242u;
        int steps = 0, mutedSteps = 0;
        while (realT < seconds + tail + holdS)
        {
            rng ^= rng << 13;
            rng ^= rng >> 17;
            rng ^= rng << 5;
            const double d = dt * (1.0 + jitter * ((double)(rng >> 8) / 8388608.0 - 1.0));
            realT += d;
            const bool held = holdS > 0.0 && realT >= holdAt && realT < holdAt + holdS;
            if (!held)
                simT += d;
            // The layer, as Ambience::update (which runs before the thunder step: on the last step's driver).
            const float prevGain = gain;
            const float target = driver ? g0 : 0.0f, stepG = (float)d * g0;
            gain = gain < target ? std::min(target, gain + stepG / std::max(fadeIn, 1e-3f)) : std::max(target, gain - stepG / std::max(fadeOut, 1e-3f));
            thunderSimGain_ = gain;
            while (next < thunderRec_.size() && thunderRecAddS_[next] <= simT && simT <= start + seconds)
            {
                ThunderSource s = thunderRec_[next];
                s.heard = s.done = s.passed = s.seen = s.before = false;
                s.play = -1.0;
                s.pw = 0.0f;
                s.lastKm = 0.0f;
                thunderInsertSource(std::move(s), thunderRecKind_[next]);
                ++next;
            }
            const glm::dvec3 eye = eye0 + vel * (simT - start);
            thunderStep(simT, eye, (float)d);
            driver = thunderHoldS_ > 0.0f;
            // What the device would play of it: the voice runs only while its gain is up (a stopped voice is frozen).
            sampleAcc += d * sr;
            const uint32_t n = (uint32_t)sampleAcc;
            sampleAcc -= n;
            const size_t at = lr.size();
            lr.resize(at + (size_t)n * 2, 0.0f);
            if (gain > 1e-4f || prevGain > 1e-4f)
            {
                syn->render(lr.data() + at, n);
                for (uint32_t i = 0; i < n; ++i)
                {
                    const float g = prevGain + (gain - prevGain) * ((float)(i + 1) / (float)n);
                    lr[at + 2 * i] *= g;
                    lr[at + 2 * i + 1] *= g;
                }
            }
            else
                ++mutedSteps;
            json lv = json::array();
            for (const ThunderVoice &v : thunderVoice_)
                lv.push_back(v.used ? std::round(v.level * 1000.0f) / 1000.0f : -1.0f);
            trace.push_back({std::round(realT * 1e4) / 1e4, std::round((simT - start) * 1e4) / 1e4, std::round(gain * 1000.0f) / 1000.0f, lv});
            ++steps;
        }
        const json rows = truthRows(thunderStatsJson(), eye0, vel, start);
        summarise(rows, seconds, r);

        thunderSink_ = nullptr;
        thunderSimulating_ = false;
        thunderSimGain_ = 1.0f;
        thunderSources_ = std::move(keepSrc);
        thunderStats_ = std::move(keepStats);
        std::copy(std::begin(keepVoice), std::end(keepVoice), thunderVoice_);
        thunderPrevS_ = keepPrevS;
        thunderRealT_ = keepRealT;
        thunderPrevEye_ = keepPrevEye;
        thunderPrevValid_ = keepValid;
        thunderLogOn_ = keepLog;
        thunderHoldS_ = keepHold;
        thunderLevelNow_ = keepLevel;
        thunderRolls_ = keepRolls;
        thunderSteals_ = keepSteals;

        const std::string wav = harnessRunner_->capturePath(name, ".wav");
        if (!writeWavStereo16(wav, lr, sr))
        {
            a.error = "could not write " + wav;
            return harness::Status::Error;
        }
        r["dt_ms"] = dt * 1000.0;
        r["seconds"] = seconds;
        r["speed_mps"] = speed;
        r["steps"] = steps;
        r["muted_steps"] = mutedSteps;
        r["fade_in_s"] = fadeIn;
        r["fade_out_s"] = fadeOut;
        r["kept_sources"] = thunderRec_.size();
        r["wav"] = wav;
        const std::string path = harnessRunner_->capturePath(name, ".thunder.json");
        std::ofstream(path) << json{{"summary", r}, {"rows", rows}, {"trace_columns", "real_s, sim_s, layer_gain, [voice levels, -1 = free]"}, {"trace", trace}}.dump() << '\n';
        r["file"] = path;
        return harness::Status::Done;
    }
    a.error = "lightning thunder log on|off|clear | state [name] | save <name> | load <file> | sim [name] [dt_ms=30] [seconds=60] "
              "[speed=0] [hold_at=] [hold_s=] [jitter=0] [tail=6]";
    return harness::Status::Error;
}

// ── Collision sounds (2026-11-03, issue_20261030_180204) ───────────────────────────────────────────────
// Space carries no sound; the hit is the film's convention, timed with the flash (the "impact frame": the step whose
// sim interval holds the impact, at any time scale; no hit across a jump, see thunderStep's resync) and shaped by what
// hit what and how far off:
//   energy E = the pair's kinetic energy in its centre-of-mass frame (0.5 mT mP / (mT + mP) vrel^2: what the breakup
//   takes; a Starmind pair head-on at 15 km/s ~1.4e11 J, a 1-kg piece at 10 km/s 5e7 J), e01 = (log10 E - 6) / 6;
//   closeness c = (1 + d / 2 km)^-0.75 in orbit (1 km 0.74, 10 km 0.26, 250 km 0.027); from the ground (the eye under
//   100 km) the hit is a distant, low-passed boom instead: c = (1 + d / 300 km)^-0.75, cutoff ~220 Hz, a slow
//   attack, a long dark tail.
//   gain = (0.35 + e01) x c; bass = 0.6 e01 + 0.4 x mass term (log10 of the pair's mass, 10 kg .. 30 t);
//   in orbit: cutoff 1.5 .. 9 kHz with c, attack 2 ms close .. 30 ms far, a crack up close, reverb send 0.6 c (+ e01).
bool SatelliteSim::impactParams(const KCollVis &c, const glm::dvec3 &eye, ImpactSound &s) const
{
    s = ImpactSound{};
    s.ev = c.ev;
    s.te = c.te;
    const double d = std::max(glm::length(c.pos - eye), 1.0);
    s.distKm = d / 1000.0;
    const double mu = c.mT * c.mP / std::max(c.mT + c.mP, 1e-6);
    s.energyJ = 0.5 * mu * c.vrel * c.vrel;
    s.massKg = c.mT + c.mP;
    s.ground = glm::length(eye) - 6371000.0 < 100000.0;
    const double e01 = std::clamp((std::log10(std::max(s.energyJ, 1.0)) - 6.0) / 6.0, 0.0, 1.0);
    const double m01 = std::clamp((std::log10(std::max(s.massKg, 1.0)) - 1.0) / 3.5, 0.0, 1.0);
    impactShape(e01, std::clamp(0.6 * e01 + 0.4 * m01, 0.0, 1.0), d, s.ground, s);
    // Left / right: the impact's direction against the screen's right (as thunder's), the right taken from the view
    // (ENU) onto this frame's ENU axes in ECI.
    const glm::dvec3 ex(eci2enuX), ey(eci2enuY), ez(eci2enuZ);
    const glm::dvec3 rel = c.pos - eye;
    const glm::mat3 &V = camera.viewEnu; // rows: right, up, back (ENU components)
    const glm::dvec3 rightEci = (double)V[0][0] * ex + (double)V[1][0] * ey + (double)V[2][0] * ez;
    s.pan = (float)(glm::dot(rel, rightEci) / std::max(glm::length(rel), 1e-9));
    return s.gain > 0.004f;
}

// (2026-11-21) The shape, shared with the launches' distant booms (launchThumps): gain, bass, cutoff, attack, decay,
// reverb and crack from the hit's size e01, its bass, the distance and where the listener is.
void SatelliteSim::impactShape(double e01, double bass, double d, bool ground, ImpactSound &s)
{
    const double close = std::pow(1.0 + d / (ground ? 300000.0 : 2000.0), -0.75);
    s.ground = ground;
    s.gain = (float)((0.35 + 1.0 * e01) * close);
    s.bass = (float)bass;
    s.thumpHz = (float)(80.0 - 50.0 * s.bass);
    if (ground)
    {
        s.cutoffHz = (float)(180.0 + 80.0 * e01);
        s.attackS = 0.08f;
        s.decayS = (float)(1.0 + 1.0 * s.bass);
        s.reverb = 0.45f;
        s.crack = 0.0f;
    }
    else
    {
        s.cutoffHz = (float)(1500.0 + 7500.0 * close);
        s.attackS = (float)(0.002 + 0.028 * (1.0 - close));
        s.decayS = (float)(0.2 + 0.6 * s.bass);
        s.reverb = (float)std::clamp(0.6 * close * (0.6 + 0.4 * e01), 0.0, 1.0);
        s.crack = (float)std::clamp(close * (0.4 + 0.6 * e01), 0.0, 1.0);
    }
}

void SatelliteSim::impactStep(double nowP, double now, float realDt, const glm::dvec3 &eyeEcef, const glm::dvec3 &camR, bool resync)
{
    FPROF_SCOPE("impactStep");
    if (!ambience_)
        return;
    const glm::dvec3 eye = glm::dvec3(obsECI);
    const int li = ambience_->layerIndex("impact");
    const int vi = (li >= 0) ? ambience_->layerVoice(li) : -1;
    AmbientSynth *syn = (audio_ && vi >= 0) ? audio_->voiceSynth(vi) : nullptr;
    bool near = false;
    for (const KCollVis &c : kCollVis)
    {
        ImpactSound s;
        if (!impactParams(c, eye, s))
            continue;
        // The layer is up through the approach (the voice is made the first time its layer is audible) and the tail.
        if (now > c.te - kCollApproachS && now < c.te + 6.0)
            near = true;
        // Once per crossing: time run back over it and forward again plays it again, as the flash does.
        if (!(c.te > nowP && c.te <= now))
            continue;
        if (syn)
        {
            const float ev[9] = {s.gain, s.bass, s.cutoffHz, s.reverb, s.attackS, s.decayS, s.pan, s.thumpHz, s.crack};
            s.sent = syn->pushEvent(ev, 9);
        }
        impactSounds_.push_back(s);
        if (impactSounds_.size() > 8)
            impactSounds_.erase(impactSounds_.begin());
    }
    // The launches' thumps (2026-11-21, issue_20261121_164722): ignition, staging and the booster's relights, each one
    // hit on this synth: the bang through the air where the air carries it, and the collisions' own distant boom
    // from far off or from above the air.
    bool soon = false;
    for (LaunchThump &t : launchThumps(nowP, now, eyeEcef, camR, resync, soon))
    {
        if (syn)
        {
            const float ev[9] = {t.gain, t.bass, t.cutoffHz, t.reverb, t.attackS, t.decayS, t.pan, t.thumpHz, t.crack};
            t.sent = syn->pushEvent(ev, 9);
        }
        near = true;
        launchThumpLog_.push_back(t);
        if (launchThumpLog_.size() > 16)
            launchThumpLog_.erase(launchThumpLog_.begin());
    }
    launchThumpSoon_ = soon;
    near = near || soon;
    impactHoldS_ = near ? 6.0f : std::max(impactHoldS_ - std::max(realDt, 0.0f), 0.0f);
    ambience_->set(ambD_.impact, impactHoldS_ > 0.0f ? 1.0f : 0.0f);
}

// ── Rockets (2026-11-21, issue_20261121_164722) ─────────────────────────────────────────────────────────
// The rockets' own layer (`rocket`: a thunder synth whose voices are all rockets; driver `rocket`). Each burning
// stage is one voice carrying two things (launchSounds): the ROAR as it arrives through the air (the retarded time,
// 1 / distance, thinning with the air at the rocket and at the listener), and the FAR RUMBLE of the stage as it burns
// now (the film's convention, the collisions' distance law: heard hundreds of km off and from above the air). The
// layer is up from 3 s before an ignition's sound arrives to 6 s after the last sound. Until this date the rockets
// shared the thunder layer and its 8 voices: gated off above 25 km with the thunder, silent beyond ~100 km.
void SatelliteSim::rocketStep(double now, const glm::dvec3 &eye, const glm::dvec3 &camR, float realDt)
{
    FPROF_SCOPE("rocketStep");
    if (!ambience_)
        return;
    const int li = ambience_->layerIndex("rocket");
    const int vi = (li >= 0) ? ambience_->layerVoice(li) : -1;
    AmbientSynth *syn = (audio_ && vi >= 0) ? audio_->voiceSynth(vi) : nullptr;
    bool sounded[kThunderVoices] = {};
    rocketLevelNow_ = 0.0f;
    std::vector<LaunchSound> ls = launchSounds(now, eye, camR);
    // (more stages than voices: the loudest are served first, and take the quietest other voice)
    std::stable_sort(ls.begin(), ls.end(), [](const LaunchSound &a, const LaunchSound &b) { return a.level + a.rumble > b.level + b.rumble; });
    for (const LaunchSound &s : ls)
    {
        int v = -1;
        for (int k = 0; k < kThunderVoices; ++k)
            if (rocketVoice_[k].used && rocketVoice_[k].id == s.id)
                v = k;
        if (v < 0)
        {
            for (int k = 0; k < kThunderVoices; ++k)
            {
                if (sounded[k])
                    continue;
                if (!rocketVoice_[k].used)
                {
                    v = k;
                    break;
                }
                if (v < 0 || rocketVoice_[k].level < rocketVoice_[v].level)
                    v = k;
            }
            if (v < 0)
                continue;
            rocketVoice_[v] = ThunderVoice{};
            rocketVoice_[v].id = s.id;
            rocketVoice_[v].used = true;
        }
        sounded[v] = true;
        rocketVoice_[v].level = s.level + s.rumble;
        rocketVoice_[v].lastSoundT = thunderRealT_;
        rocketLevelNow_ = std::max(rocketLevelNow_, s.level + s.rumble);
        if (syn)
        {
            const float ev[6] = {(float)v, s.level, s.km, s.pan, 1.0f, s.rumble}; // 1: a rocket
            syn->pushEvent(ev, 6);
        }
    }
    bool any = launchThumpSoon_;
    for (int k = 0; k < kThunderVoices; ++k)
    {
        ThunderVoice &tv = rocketVoice_[k];
        if (!tv.used)
            continue;
        if (sounded[k])
        {
            any = true;
            continue;
        }
        if (tv.level > 0.0f && syn)
        {
            const float ev[6] = {(float)k, 0.0f, 5.0f, 0.0f, 1.0f, 0.0f};
            syn->pushEvent(ev, 6);
        }
        tv.level = 0.0f;
        if (thunderRealT_ - tv.lastSoundT > 2.0)
            tv.used = false;
    }
    rocketHoldS_ = any ? 6.0f : std::max(rocketHoldS_ - std::max(realDt, 0.0f), 0.0f);
    ambience_->set(ambD_.rocket, rocketHoldS_ > 0.0f ? 1.0f : 0.0f);
}

// ── Tonality ──────────────────────────────────────────────────────────────────────────────────────
// The soundtrack analysis (MusicAnalysis.h): every playlist track, on a worker thread (harness runs:
// synchronously, so a scripted run hears the same key every time). The cache lives in the user data
// folder; the exe's own folder is a read-only fallback, which is what a harness run (its user data is
// its run folder) finds warm when the app has been run normally from the same build.
void SatelliteSim::startMusicAnalysis()
{
    if (musicLib_ || musicTracks_.empty())
        return;
    musicLib_ = new music::Library();
    std::vector<std::string> dirs = {(std::filesystem::path(userDataDir_.empty() ? Paths::userDataDir() : userDataDir_) /
                                      "music_analysis")
                                         .string()};
    const std::string exeCache = (std::filesystem::path(Paths::exeDir()) / "music_analysis").string();
    if (exeCache != dirs[0])
        dirs.push_back(exeCache);
    musicLib_->start(musicTracks_, dirs, harnessRunner_ != nullptr);
}

namespace
{
std::string chordLabel(const music::Analysis &a, uint16_t absMask)
{
    for (const music::Chord &c : a.chords)
        if (c.mask == absMask && c.rootPc >= 0)
            return music::chordName(c);
    return "-";
}
} // namespace

// The root every pitched ambience voice is a multiple of, and the masks the chord pad plays from.
//   playing a track   its tonic, at the track's tuning-curve pitch where the music is now (so the
//                     ambience bends with gravity_wave's piano), its pitch set, the chord it is on
//   between tracks    a glide from where the last track ended to the next one's tonic, over the
//                     middle of the gap (the table's tonality.gap_glide), masks switching halfway
//   otherwise         the Sound tab's Tonal root and the table's fallback scale — follow-music off,
//                     no music, or the analysis not ready yet
// The octave is chosen when the SOURCE changes (nearest the root as it is, then kept within
// tonality.root_range_hz), so a new track is a glide of at most a tritone, never an octave; and the
// root never moves faster than tonality.slew_semitones_per_s — a skipped track glides too.
void SatelliteSim::updateTonality(float dt)
{
    FPROF_SCOPE("updateTonality");
    const Ambience::TonalityConfig &cfg = ambience_->tonality();
    if (musicLib_)
        for (const std::string &m : musicLib_->takeMessages())
            Log::line(m);

    std::string key = "manual";
    double hz = std::max(ambRootHz, 1.0f);
    uint16_t scale = cfg.fallbackScale, chord = 0x81;
    std::string source = ambRootFollowMusic ? "Tonal root (no analysis yet)" : "Tonal root (manual)";
    std::string keyName = "-", chordName = "-";
    float cents = 0.0f;
    if (ambRootFollowMusic && audio_ && musicLib_ && audio_->trackCount() > 0)
    {
        const int n = audio_->trackCount();
        const int ti = audio_->trackIndex();
        const auto A = musicLib_->get(audio_->trackPath(ti));
        const auto B = musicLib_->get(audio_->trackPath((ti + 1) % n));
        auto tonicAt = [&](const music::Analysis &x, float t)
        { return x.tonicHz(cfg.rootLowHz, cfg.followTuningCurve ? x.tuningAt(t) : x.tuningCents); };
        const bool gap = audio_->musicOn() && !audio_->trackPlaying() && audio_->gapRemaining() > 0.0f;
        if (gap && A && B)
        {
            const double hzA = tonicAt(*A, A->durationS);
            double hzB = tonicAt(*B, 0.0f);
            while (hzB > hzA * 1.41421356)
                hzB *= 0.5;
            while (hzB < hzA / 1.41421356)
                hzB *= 2.0;
            const float ph = 1.0f - audio_->gapRemaining() / std::max(audio_->musicGap(), 1e-3f);
            const float s = smoothstepf(cfg.gapGlide[0], cfg.gapGlide[1], ph);
            hz = std::exp2(std::log2(hzA) + (std::log2(hzB) - std::log2(hzA)) * s);
            const music::Analysis &M = s < 0.5f ? *A : *B;
            scale = music::toRelative(M.pitchSet, M.tonicPc);
            chord = music::toRelative(M.homeChord, M.tonicPc);
            key = "g:" + std::to_string(ti);
            source = "gap: " + audio_->trackName(ti) + " -> " + audio_->trackName((ti + 1) % n);
            keyName = M.keyName();
            chordName = chordLabel(M, M.homeChord);
        }
        else if (A)
        {
            const float pos = audio_->trackPlaying() ? audio_->trackPosition() : 0.0f;
            cents = cfg.followTuningCurve ? A->tuningAt(pos) : A->tuningCents;
            hz = A->tonicHz(cfg.rootLowHz, cents);
            scale = music::toRelative(A->pitchSet, A->tonicPc);
            const music::Chord *c = A->chordAt(pos);
            chord = music::toRelative(c ? c->mask : A->homeChord, A->tonicPc);
            key = "t:" + std::to_string(ti);
            source = audio_->trackName(ti);
            keyName = A->keyName();
            chordName = c ? music::chordName(*c) : chordLabel(*A, A->homeChord);
        }
    }

    if (key != tonal_.sourceKey)
    {
        tonal_.sourceKey = key;
        tonal_.octave = 0;
        if (key != "manual")
        {
            if (tonal_.rootLog2 >= 0.0)
                tonal_.octave = -(int)std::lround(std::log2(hz) - tonal_.rootLog2);
            double h = hz * std::exp2(tonal_.octave);
            for (int k = 0; k < 4 && h < cfg.rootLowHz; ++k, h *= 2.0)
                ++tonal_.octave;
            for (int k = 0; k < 4 && h > cfg.rootHighHz; ++k, h *= 0.5)
                --tonal_.octave;
        }
    }
    tonal_.targetLog2 = std::log2(hz) + tonal_.octave;
    if (tonal_.rootLog2 < 0.0)
        tonal_.rootLog2 = tonal_.targetLog2;
    else if (dt > 0.0f)
    {
        const double d = tonal_.targetLog2 - tonal_.rootLog2;
        const double maxStep = cfg.slewSemitonesPerS / 12.0 * dt;
        tonal_.rootLog2 += std::clamp(d * (1.0 - std::exp(-dt / 0.35)), -maxStep, maxStep);
    }
    tonal_.scale = (uint16_t)(scale | 1u);
    tonal_.chord = chord ? chord : (uint16_t)0x81;
    tonal_.source = source;
    tonal_.key = keyName;
    tonal_.chordName = chordName;
    tonal_.scaleText = music::intervalList(tonal_.scale);
    tonal_.tuningCents = cents;
}

nlohmann::json SatelliteSim::tonalityJson() const
{
    nlohmann::json j;
    j["follow_music"] = ambRootFollowMusic;
    j["root_hz"] = std::round(std::exp2(tonal_.rootLog2) * 100.0) / 100.0;
    j["target_hz"] = std::round(std::exp2(tonal_.targetLog2) * 100.0) / 100.0;
    j["source"] = tonal_.source;
    j["key"] = tonal_.key;
    j["chord"] = tonal_.chordName;
    j["scale"] = tonal_.scaleText;
    j["chord_intervals"] = music::intervalList(tonal_.chord);
    j["tension"] = ambience_ ? music::intervalList(ambience_->tonality().tension) : "";
    j["tuning_cents"] = std::round(tonal_.tuningCents * 10.0f) / 10.0f;
    j["analysis_pending"] = musicLib_ ? musicLib_->pending() : 0;
    return j;
}

std::string SatelliteSim::tonalityLine() const
{
    if (tonal_.rootLog2 < 0.0)
        return "Key: -";
    char buf[192];
    snprintf(buf, sizeof(buf), "Key: %s, %s  |  root %.1f Hz  |  %s", tonal_.key.c_str(), tonal_.chordName.c_str(),
             std::exp2(tonal_.rootLog2), tonal_.source.c_str());
    return buf;
}

std::string SatelliteSim::ambienceNowPlaying() const
{
    if (!ambience_ || !ambience_->loaded())
        return "ambience off (no ambience.json)";
    const nlohmann::json s = ambience_->stateJson();
    std::string out;
    for (const auto &l : s["layers"])
    {
        const float g = l["gain"].get<float>();
        if (g < 0.01f)
            continue;
        char buf[64];
        snprintf(buf, sizeof(buf), "%s%s %.0f%%", out.empty() ? "" : ", ", l["id"].get<std::string>().c_str(), g * 100.0f);
        out += buf;
    }
    return out.empty() ? "(silence)" : out;
}
