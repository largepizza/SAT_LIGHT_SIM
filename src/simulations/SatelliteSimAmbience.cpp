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
#include "SatelliteSim.h"

#include "Ambience.h"
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
    int hits = oceanAt(latRad, lonRad) ? 1 : 0, n = 1;
    const double cosLat = std::max(0.05, std::cos(latRad));
    for (int r = 1; r <= rings; ++r)
    {
        const double d = radiusM * r / rings / kReM;
        for (int k = 0; k < 8; ++k)
        {
            const double b = (k + 0.5 * (r & 1)) * (glm::pi<double>() / 4.0);
            hits += oceanAt(latRad + d * std::cos(b), lonRad + d * std::sin(b) / cosLat) ? 1 : 0;
            ++n;
        }
    }
    return (float)hits / (float)n;
}

void SatelliteSim::computeAmbienceContext(float dt)
{
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

    // Auroral oval under the camera: sat_sky's band (kGeomagPoleECEF, colatitude 20 deg + storm
    // expansion), without the curtain/coverage noise — a hum wants a smooth region, not patches.
    {
        const glm::dvec3 pole = glm::normalize(glm::dvec3(0.0481, -0.1543, 0.9868));
        const glm::dvec3 pd = glm::dot(up, pole) > 0.0 ? pole : -pole;
        const float colat = (float)glm::degrees(std::acos(std::clamp(glm::dot(up, pd), -1.0, 1.0)));
        const float centre = 20.0f + stormStrength * 8.0f;
        const float width = 6.0f * (1.0f + stormStrength * 1.5f);
        const float band = smoothstepf(width * 2.0f, width * 0.5f, std::fabs(colat - centre));
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

    // ── Rain: the cloud field's rain rate at the eye (0 drizzle-free .. 1 a Cb core), written by
    // cloud_v2_march.comp into terrainFrame.w and read back a frame later. The same value drives the
    // streaks, so what you hear is what falls on screen. ──
    {
        const float raw = terrainFrameMapped ? std::clamp(terrainFrameMapped[3], 0.0f, 1.0f) : 0.0f;
        ambRainEased += (raw - ambRainEased) * (dt > 0.0f ? 1.0f - expf(-dt / 2.0f) : 1.0f);
        a.set(ambD_.rain, ambRainEased);
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
        for (int ci : ambGroupConsts_[g])
        {
            const ConstellationConfig &c = constellations[ci];
            if (!c.enabled || c.orbitCount == 0)
                continue;
            if (followActive && followSatIndex >= (int)c.orbitStart && followSatIndex < (int)(c.orbitStart + c.orbitCount))
                followedInGroup = true;
            const double N = (double)c.orbitCount;
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
                const double z = glm::dot(cam, nrm);
                const double rho = glm::length(cam - nrm * z);
                const double half = 0.5 * (nr - 1) * spacingM + c.altJitterM;
                const double R0 = Rc - half - 0.5 * spacingM, R1 = Rc + half + 0.5 * spacingM;
                const double sigma = N / (glm::pi<double>() * (R1 * R1 - R0 * R0));
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
            }
            else
            {
                const double R = kReM + c.altM;
                double iEff = c.incl;
                if (iEff > glm::half_pi<double>())
                    iEff = glm::pi<double>() - iEff;
                const double absLat = std::fabs(camLat);
                const double reach = std::clamp((iEff + 0.05 - absLat) / 0.1, 0.0, 1.0);
                double sigma = 0.0;
                if (reach > 0.0)
                {
                    if (c.distribution == OrbitDistribution::RandomShell)
                        sigma = N / (4.0 * glm::pi<double>() * R * R * std::max(std::sin(iEff), 0.05));
                    else
                    {
                        const double si = std::sin(iEff), sp = std::sin(absLat);
                        const double eps = std::max(2.0 * si * std::cos(iEff) * 0.035, 0.004);
                        sigma = N / (2.0 * glm::pi<double>() * glm::pi<double>() * R * R * std::sqrt(std::max(si * si - sp * sp, eps)));
                    }
                    sigma *= reach * reach * (3.0 - 2.0 * reach);
                }
                const double dr = std::max(0.0, std::fabs(camR - R) - (double)c.altJitterM);
                if (sigma > 0.0)
                {
                    const double a2 = H * H - dr * dr;
                    if (a2 > 0.0)
                        count += sigma * glm::pi<double>() * a2;
                    const double spacing = 0.5 / std::sqrt(sigma);
                    nearest = std::min(nearest, std::sqrt(dr * dr + spacing * spacing));
                }
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
    if (!ambience_)
        initAmbience();
    if (!ambience_->loaded())
        return;
    computeAmbienceContext(dt);
    updateTonality(dt);
    ambience_->setFadeScales(ambFadeInScale, ambFadeOutScale);
    ambience_->setRootHz((float)std::exp2(tonal_.rootLog2));
    ambience_->setTonal(tonal_.scale, tonal_.chord, ambience_->tonality().tension);
    for (size_t g = 0; g < Ambience::groupNames().size() && g < 6; ++g)
        ambience_->setGroupGain(Ambience::groupNames()[g], ambGroupGain[g]);
    ambience_->update(dt, audio_);
    updateThunder();

    // The playing track's upwell stem: its gain is the table's "music_upwell" map of the glare on
    // screen, slewed linearly (up over fade_in_s, down over fade_out_s, full scale). Moving between
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
// The lightning flash list (cloud_v2_lightning.comp, host-visible; this is the previous frame's) is
// read for flashes not seen before; each within 30 km is heard at its distance / 343 m/s after it
// started, in SIM time (paused, nothing arrives; at 1 min/s it rolls in almost at once), as one roll
// of the "thunder" layer's synth. One roll starts per frame (the synth takes its parameters once per
// block); a crowd of arrivals queues. Reversed or jumped time drops the queue.
void SatelliteSim::updateThunder()
{
    if (!ambience_ || !cv2FlashMapped)
        return;
    const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (now < thunderLastSimS_ - 1.0 || now > thunderLastSimS_ + 120.0)
    {
        thunderPending_.clear();
        thunderSeen_.clear();
    }
    thunderLastSimS_ = now;

    const uint32_t *hdr = (const uint32_t *)cv2FlashMapped;
    const float *f = (const float *)((const char *)cv2FlashMapped + 16);
    const uint32_t n = std::min(hdr[0], kCv2FlashMax);
    for (uint32_t i = 0; i < n; ++i)
    {
        const float *e = f + i * 12;
        uint32_t id;
        std::memcpy(&id, &e[8], 4);
        bool seen = false;
        for (auto &s : thunderSeen_)
            if (s.first == id)
                seen = true;
        if (seen)
            continue;
        thunderSeen_.push_back({id, now});
        const float distM = e[11];
        if (distM > 30000.0f)
            continue;
        // Bearing from the listener (x east, y north in the observer's ENU frame) against the
        // camera's heading: +1 = right.
        const float bearing = atan2f(e[0], e[1]);
        const float rel = bearing - glm::radians(camera.azDeg);
        const bool cg = e[7] > 0.5f;
        thunderPending_.push_back({now - (double)e[10] + distM / 343.0, distM / 1000.0f, cg ? 1.0f : 0.55f, sinf(rel)});
    }
    // Forget flashes long over (their ids recur only in another time slot).
    thunderSeen_.erase(std::remove_if(thunderSeen_.begin(), thunderSeen_.end(),
                                      [&](const std::pair<uint32_t, double> &s) { return now - s.second > 10.0; }),
                       thunderSeen_.end());

    const int li = ambience_->layerIndex("thunder");
    const int v = (li >= 0) ? ambience_->layerVoice(li) : -1;
    AmbientSynth *syn = (audio_ && v >= 0) ? audio_->voiceSynth(v) : nullptr;
    for (size_t i = 0; i < thunderPending_.size(); ++i)
    {
        if (thunderPending_[i].arriveS > now)
            continue;
        const ThunderEvent ev = thunderPending_[i];
        thunderPending_.erase(thunderPending_.begin() + (long)i);
        if (syn)
        {
            syn->setParam(syn->paramIndex("distance_km"), ev.distKm);
            syn->setParam(syn->paramIndex("energy"), ev.energy);
            syn->setParam(syn->paramIndex("pan"), ev.pan);
            thunderTrigger_ += 1.0f;
            syn->setParam(syn->paramIndex("trigger"), thunderTrigger_);
            ++thunderRolls_;
        }
        break;   // one per frame
    }
    if (thunderPending_.size() > 64)
        thunderPending_.erase(thunderPending_.begin(), thunderPending_.end() - 64);
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
