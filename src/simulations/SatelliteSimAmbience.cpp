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

#include <algorithm>
#include <cctype>
#include <cmath>

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
    ambD_.timeScale = a.registerDriver("time_scale");
    ambD_.following = a.registerDriver("following");
    ambD_.intro = a.registerDriver("intro");
    ambD_.veg = a.registerDriver("veg");
    ambD_.forest = a.registerDriver("forest");
    ambD_.desert = a.registerDriver("desert");
    ambD_.ice = a.registerDriver("ice");
    ambD_.speed = a.registerDriver("speed_mps");
    ambD_.eas = a.registerDriver("eas");

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
    ambience_->setFadeScales(ambFadeInScale, ambFadeOutScale);
    ambience_->setRootHz(ambRootHz);
    for (size_t g = 0; g < Ambience::groupNames().size() && g < 6; ++g)
        ambience_->setGroupGain(Ambience::groupNames()[g], ambGroupGain[g]);
    ambience_->update(dt, audio_);

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
