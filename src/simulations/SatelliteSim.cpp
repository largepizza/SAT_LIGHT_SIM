#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../AudioSystem.h"
#include "clay.h"
#include "star_catalog.h"
#include "stb_image.h"

#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <algorithm>
#include <stdexcept>
#include <fstream>
#include <unordered_map>

#include <nlohmann/json.hpp>

#include <filesystem>
#include <string>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#elif defined(__linux__)
#include <unistd.h> // readlink
#include <limits.h> // PATH_MAX
#elif defined(__APPLE__)
#include <mach-o/dyld.h> // _NSGetExecutablePath
#include <limits.h>
#endif

// ── Earth + observer constants ─────────────────────────────────────────────────
static constexpr float kEarthRadius = 6'371'000.0f; // mean Earth radius (m)
static constexpr double kOmegaEarth = 7.2921150e-5; // sidereal rotation rate (rad/s)
static constexpr float kObsLatDefault = 37.0f;      // default observer latitude (°N, ~Bay Area)

// ── Orbital mechanics ─────────────────────────────────────────────────────────
static constexpr double kGM = 3.986004418e14;        // Earth gravitational parameter (m³/s²)
static constexpr double kJ2 = 1.08263e-3;            // Earth oblateness (J2) coefficient
static constexpr double kYearSec = 365.25 * 86400.0; // seconds per tropical year
// SSO nodal precession rate = Earth's mean orbital motion ≈ 0.9856°/day eastward.
// J2 causes a retrograde circular orbit (i > 90°) to precess its RAAN eastward at
// exactly this rate, keeping the nodal plane fixed relative to the sun.
static constexpr double kSSOPrecRate = 2.0 * 3.14159265358979323846 / kYearSec; // rad/s

// ── Photometry (must mirror sat_flare.comp constants) ────────────────────────
// kBrightnessScale MUST stay in sync with BRIGHTNESS_SCALE in sat_flare.comp.
// kMagRef and kMagRefFlare define the calibration anchor for the magnitude readout.
// kBrightnessScale / kDaySuppression removed — now runtime members on SatelliteSim,
// synced to SatFlarePC each frame so CPU magnitude readout matches GPU render.
static constexpr float kRefRange = 500'000.0f; // 500 km normalisation range (m)
static constexpr float kMagRef = 6.0f;         // apparent magnitude at kMagRefFlare
static constexpr float kMagRefFlare = 0.008f;  // effectFlare corresponding to kMagRef
// Virtual diffuse floor for the magnitude readout only (not sent to GPU).
// Zero-diffuse satellites (Starlink) are only visible via transient specular flares;
// this floor lets them appear in the readout as a meaningful steady-state estimate.
static constexpr float kMagDiffuseFloor = 0.003f;

static inline float computeMeanMotion(float altM)
{
    double a = (double)kEarthRadius + (double)altM;
    return (float)sqrt(kGM / (a * a * a)); // rad/s
}

// SSO inclination from J2 nodal precession: solves dΩ/dt = kSSOPrecRate.
// dΩ/dt = -1.5 * n * J2 * (Re/a)² * cos(i)   →   cos(i) = -kSSOPrecRate / (1.5*n*J2*(Re/a)²)
// Result is in the retrograde range (~97–107° for typical LEO/MEO SSO altitudes).
static inline float computeSSOInclination(float altM)
{
    double a = (double)kEarthRadius + (double)altM;
    double n = sqrt(kGM / (a * a * a));
    double rat = (double)kEarthRadius / a;
    double cosI = -kSSOPrecRate / (1.5 * n * kJ2 * rat * rat);
    return (float)acos(glm::clamp(cosI, -1.0, 1.0));
}

static std::filesystem::path resolveExeDir()
{
#if defined(_WIN32)
    char buf[MAX_PATH] = {};
    GetModuleFileNameA(nullptr, buf, MAX_PATH);
    return std::filesystem::path(buf).parent_path();

#elif defined(__linux__)
    char buf[PATH_MAX] = {};
    ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
    if (len != -1)
        return std::filesystem::path(std::string(buf, len)).parent_path();

#elif defined(__APPLE__)
    char buf[PATH_MAX] = {};
    uint32_t size = sizeof(buf);
    if (_NSGetExecutablePath(buf, &size) == 0)
        return std::filesystem::path(buf).parent_path();
#endif

    // Universal fallback: use current working directory
    return std::filesystem::current_path();
}

// ─── init ─────────────────────────────────────────────────────────────────────
void SatelliteSim::init(VulkanContext &ctx)
{
    // Resolve directory that contains the executable so we can find constellations.json.
    exeDir_ = resolveExeDir().string();

    // Fixed start time: 2036-06-21 00:00:00 UTC
    // J2000.0 = 2000-01-01 12:00:00 UTC = Unix 946728000
    // 2036-06-21 00:00:00 UTC = Unix 2097619200
    // Fixed start time: 1150891200 seconds from J2000 = 13320 days + 43200 s.
    // Stored split so float deltaT stays small regardless of time-warp distance.
    constexpr int64_t kInitWholeSec = 1150891200LL - 3 * 30 * 24 * 60 * 60 + 17.5 * 60 * 60;
    simDayJ2000 = kInitWholeSec / 86400LL;           // 13320
    simSecInDay = (double)(kInitWholeSec % 86400LL); // 43200.0
    simInitDayJ2000 = simDayJ2000;
    simInitSecInDay = simSecInDay;

    ctx_ = &ctx;

    // Order must match the KB_* enum in SatelliteSim.h.
    // held=true  → polled every frame in recordCompute (modifier/held keys)
    // held=false → fired once in onKey (toggle/event keys)
    keybindings = {
        {"Toggle UI", GLFW_KEY_TAB, false, false},          // KB_TOGGLE_UI
        {"Pause/Resume", GLFW_KEY_SPACE, false, false},     // KB_PAUSE
        {"Slow Down", GLFW_KEY_COMMA, false, false},        // KB_SLOWER
        {"Speed Up", GLFW_KEY_PERIOD, false, false},        // KB_FASTER
        {"Reverse Time", GLFW_KEY_R, false, false},         // KB_REVERSE
        {"Move Fast", GLFW_KEY_LEFT_SHIFT, true, false},    // KB_MOVE_BOOST (held)
        {"Move Fine", GLFW_KEY_LEFT_CONTROL, true, false},  // KB_MOVE_FINE  (held)
        {"Cinematic Pan", GLFW_KEY_LEFT_ALT, false, false}, // KB_CINEMATIC  (event, toggle)
        {"Raise Elevation", GLFW_KEY_Q, true, false},       // KB_RAISE_ELEV (held)
        {"Lower Elevation", GLFW_KEY_E, true, false},       // KB_LOWER_ELEV (held)
        {"Reset Elevation", GLFW_KEY_Z, false, false},      // KB_RESET_ELEV (event)
    };
    static_assert(KB_COUNT == 11, "KB enum and keybindings initializer are out of sync");

    createBuffers(ctx);
    createGlowResources(ctx);
    createDescriptors(ctx);
    createComputePipeline(ctx);
    createOrbitDescriptors(ctx);
    createOrbitPipeline(ctx);
    createSkyBgPipeline(ctx);
    createDrawPipeline(ctx);
    updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay); // must run first — initConstellation reads sunDirECI
    initConstellation();
    uploadSatOrbits(ctx); // bake + upload GpuSatOrbit data after orbits are built
    initStars(ctx);
    loadSettings(); // override defaults with any previously saved values
}

// ─── onResize ─────────────────────────────────────────────────────────────────
void SatelliteSim::onResize(VulkanContext &ctx)
{
    vkDestroyPipeline(ctx.device, skyBgPipeline, nullptr);
    skyBgPipeline = VK_NULL_HANDLE;
    createSkyBgPipeline(ctx);

    vkDestroyPipeline(ctx.device, drawPipeline, nullptr);
    drawPipeline = VK_NULL_HANDLE;
    createDrawPipeline(ctx);

    vkDestroyPipeline(ctx.device, starPipeline, nullptr);
    starPipeline = VK_NULL_HANDLE;
    createStarPipeline(ctx);
}

// ─── recordCompute ────────────────────────────────────────────────────────────
void SatelliteSim::recordCompute(VkCommandBuffer cmd, VulkanContext &ctx, float dt)
{
    // ── WASD surface navigation ───────────────────────────────────────────────
    // Pure 3D ECEF — no lat/lon arithmetic, no gimbal lock, works at any latitude.
    //
    // obsDir    : unit position vector on the Earth-fixed sphere.
    // obsFacing : unit tangent vector (forward), always ⊥ obsDir.
    //
    // W/S move along obsFacing; A/D move along cross(obsFacing, obsDir) (right).
    // After each step obsFacing is parallel-transported to stay tangent at newPos.
    if (win)
    {
        bool boost = win && glfwGetKey(win, keybindings[KB_MOVE_BOOST].key) == GLFW_PRESS;
        bool fine = win && glfwGetKey(win, keybindings[KB_MOVE_FINE].key) == GLFW_PRESS;
        float speed = boost ? 0.5f : fine ? 0.005f
                                          : 0.08f; // boost = fast, fine = slow, default = normal

        float fwd = (glfwGetKey(win, GLFW_KEY_W) == GLFW_PRESS ? 1.0f : 0.0f) - (glfwGetKey(win, GLFW_KEY_S) == GLFW_PRESS ? 1.0f : 0.0f);
        float right = (glfwGetKey(win, GLFW_KEY_D) == GLFW_PRESS ? 1.0f : 0.0f) - (glfwGetKey(win, GLFW_KEY_A) == GLFW_PRESS ? 1.0f : 0.0f);

        if (fwd != 0.0f || right != 0.0f)
        {
            // right tangent = cross(obsFacing, obsDir)  (right-hand rule: forward × up = right)
            glm::vec3 rightDir = glm::normalize(glm::cross(obsFacing, obsDir));
            glm::vec3 newPos = glm::normalize(
                obsDir + speed * dt * (fwd * obsFacing + right * rightDir));

            // Parallel-transport obsFacing: project out any radial component at newPos.
            obsFacing = glm::normalize(obsFacing - glm::dot(obsFacing, newPos) * newPos);
            obsDir = newPos;

            // Refresh display caches (atan2(0,0)==0 at poles — fine for display only)
            obsLatDeg = glm::degrees(asinf(glm::clamp(obsDir.z, -1.0f, 1.0f)));
            obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
        }

        // Q/E: raise/lower observer relative to terrain; rate scales with height offset
        // (faster when high up, 10m/s minimum near the surface).
        bool raise = glfwGetKey(win, keybindings[KB_RAISE_ELEV].key) == GLFW_PRESS;
        bool lower = glfwGetKey(win, keybindings[KB_LOWER_ELEV].key) == GLFW_PRESS;
        if (raise || lower)
        {
            float rate = std::max(10.0f, obsHeightOffset * 0.5f);
            if (boost)
                rate *= 10.0f;
            if (fine)
                rate *= 0.1f;
            obsHeightOffset += (raise ? 1.0f : -1.0f) * rate * dt;
            // Clamp so observer never sinks below the terrain surface (only reset via Z)
            obsHeightOffset = std::max(0.0f, obsHeightOffset);
        }
    }

    float simDt = timePaused ? 0.0f : fabsf(dt * kTimeScales[timeScaleIdx]);
    if (!timePaused)
    {
        simSecInDay += (double)dt * kTimeScales[timeScaleIdx] * timeDir;
        // Re-base to [0, 86400) and carry whole-day overflow into simDayJ2000.
        // Using a loop (not fmod) so timeDir reversal is handled cleanly.
        while (simSecInDay >= 86400.0)
        {
            simSecInDay -= 86400.0;
            ++simDayJ2000;
        }
        while (simSecInDay < 0.0)
        {
            simSecInDay += 86400.0;
            --simDayJ2000;
        }
    }

    // Auto-rebake orbit buffer if the epoch has drifted more than kOrbitRebakeDays.
    // Keeps float deltaT < kOrbitRebakeDays*86400 s (float ULP ≈ 0.07 s at 7 days).
    if (std::abs(simDayJ2000 - orbitEpochDay) >= kOrbitRebakeDays)
        uploadSatOrbits(ctx);

    updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay, simDt);
    updateStars();

    // Read previous frame's GPU glow results for the magnitude UI.
    // glowBuf is HOST_COHERENT; by the time recordCompute is called the previous
    // frame's queue work is complete, so the atomicMax writes from sat_flare.comp are visible.
    {
        const GpuGlowBuf *gb = static_cast<const GpuGlowBuf *>(glowMapped);
        float maxFlare = 0.0f;
        for (int i = 0; i < kGlowBins; ++i)
        {
            if (gb->bins[i] != 0u)
            {
                float f;
                memcpy(&f, &gb->bins[i], sizeof(float));
                maxFlare = std::max(maxFlare, f);
            }
        }
        peakMagnitude = (maxFlare > 0.0f)
                            ? kMagRef - 2.5f * std::log10f(maxFlare / kMagRefFlare)
                            : 99.0f;
    }

    if (activeSatCount == 0)
        return;

    // Build enabled / highlight masks from constellation config (one bit per constellation).
    uint32_t enabledMask = 0, highlightMask = 0;
    for (uint32_t ci = 0; ci < (uint32_t)constellations.size() && ci < 32; ++ci)
    {
        if (constellations[ci].enabled)
            enabledMask |= (1u << ci);
        if (constellations[ci].highlight)
            highlightMask |= (1u << ci);
    }

    // ── Dispatch 1: sat_orbit.comp — orbital mechanics + attitude ─────────────
    SatOrbitPC orbitPc{};
    orbitPc.enuX = eci2enuX;
    orbitPc.enuY = eci2enuY;
    orbitPc.enuZ = eci2enuZ;
    orbitPc.sunDirECI = sunDirECI;
    // Two-part subtraction: integer day difference (exact) + double seconds (precise).
    // After auto-rebake, dDays < kOrbitRebakeDays so the float cast loses < 0.07 s.
    int64_t dDays = simDayJ2000 - orbitEpochDay;
    double dSec = simSecInDay - orbitEpochSec;
    if (dSec < 0.0)
    {
        --dDays;
        dSec += 86400.0;
    } // borrow from day if frac is negative
    orbitPc.deltaT = (float)((double)dDays * 86400.0 + dSec);
    orbitPc.obsECI = obsECI;
    orbitPc.satCount = activeSatCount;
    orbitPc.highlightMask = highlightMask;
    orbitPc.enabledMask = enabledMask;
    orbitPc.simDt = simDt;
    // Horizon cull threshold: open up to Earth limb for elevated observers.
    // limbSin = -sqrt(1 - (R_EARTH/obsR)²); always clamped to at most -0.01.
    {
        float obsR = glm::length(obsECI);
        float r = kEarthRadius / obsR;
        float limbSin = -sqrtf(std::max(0.0f, 1.0f - r * r));
        orbitPc.elevCutoff = std::min(-0.01f, limbSin);
    }

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, orbitPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            orbitPipeLayout, 0, 1, &orbitDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, orbitPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(orbitPc), &orbitPc);
    vkCmdDispatch(cmd, (activeSatCount + 63) / 64, 1, 1);

    // Barrier: sat_orbit.comp writes satInputBuf → sat_flare.comp reads it.
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = satInputBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // Zero all glow bins so this frame's flare shader starts with an empty histogram.
    // floatBitsToUint(0.0) == 0u, so filling with 0 correctly marks every bin empty.
    vkCmdFillBuffer(cmd, glowBuf, 0, sizeof(GpuGlowBuf), 0);
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = glowBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // ── Dispatch 2: sat_flare.comp — lighting + visibility ────────────────────
    SatFlarePC pc{};
    pc.enuX = eci2enuX;
    pc.enuY = eci2enuY;
    pc.enuZ = eci2enuZ;
    pc.sunDirECI = sunDirECI;
    pc.satCount = activeSatCount;
    pc.obsECI = obsECI;
    pc.elevCutoff = orbitPc.elevCutoff; // same threshold computed above
    pc.brightnessScale = brightnessScale;
    pc.daySuppression = daySuppression;
    pc.mirrorBoost = mirrorBoost;
    pc.visThresh = visThresh;
    pc.highlightFlare = highlightFlare;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            compPipeLayout, 0, 1, &descSet, 0, nullptr);
    vkCmdPushConstants(cmd, compPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (activeSatCount + 63) / 64, 1, 1);

    // Barrier: sat_flare.comp writes satVisibleBuf → vertex shader reads it.
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = satVisibleBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }
}

// ─── recordDraw ───────────────────────────────────────────────────────────────
void SatelliteSim::recordDraw(VkCommandBuffer cmd, VulkanContext &ctx, float /*dt*/)
{
    // Camera push constants shared by all sky passes.
    SatDrawPC pc{};
    pc.skyView = camera.viewMatrix();
    pc.fovYRad = glm::radians(camera.fovYDeg);
    pc.aspect = (float)ctx.swapExtent.width / (float)ctx.swapExtent.height;
    pc.gmst = (float)fmod(kOmegaEarth * (simDayJ2000 * 86400.0 + simSecInDay), glm::two_pi<double>());
    // Wave time relative to sim epoch: pauses when paused, scales with time warp.
    // Sim sec works great as it resets before any crazy floating point issues happen. Great for any animations that need a time variable.
    // There is probably a looping artifact when it rolls over but who cares it's a tiny blip that most won't notice
    pc.waveTime = simSecInDay * 1.0;
    pc.sunDirENU = sunDirENU;
    pc.moonDirENU = moonDirENU; // xyz = moon dir in ENU, w = illuminated fraction
    // Sample terrain elevation at observer lat/lon from the CPU downsampled map.
    if (!earthElevCpu.empty())
    {
        float latRad = glm::radians(obsLatDeg);
        float lonRad = glm::radians(obsLonDeg);
        float u = (lonRad + glm::pi<float>()) / (2.0f * glm::pi<float>());
        float v = (0.5f * glm::pi<float>() - latRad) / glm::pi<float>();
        int px = (int)(u * (float)earthElevCpuW) % earthElevCpuW;
        int py = std::min((int)(v * (float)earthElevCpuH), earthElevCpuH - 1);
        float pixVal = earthElevCpu[py * earthElevCpuW + px] / 255.0f;
        // DEM ocean baseline is 15/255; subtract it so sea-level land maps to 0 m.
        const float kSeaLevel = 15.0f / 255.0f;
        obsTerrainH = (pixVal <= kSeaLevel) ? 0.0f : std::max(0.0f, (pixVal - kSeaLevel) * 8848.0f);
    }
    pc.obsECEFDir = glm::vec4(obsDir, obsHeightOffset); // w = user altitude offset above terrain (m); GPU computes ground height

    // ── Update cloud params UBO (binding 9) ──────────────────────────────────
    if (cloudParamsMapped)
    {
        GpuCloudParams cp{};
        cp.coverage    = cloudCoverage;
        cp.density     = cloudDensity;
        cp.driftRate   = cloudDriftRate;
        cp.sunGain     = cloudSunGain;
        cp.ambientGain = cloudAmbientGain;
        cp.hgG         = cloudHgG;
        cp.marchSteps  = cloudMarchSteps;
        cp.lightSteps  = cloudLightSteps;
        cp.cloudPhase  = (float)fmod((double)cloudDriftRate * (simDayJ2000 * 86400.0 + simSecInDay),
                                     glm::two_pi<double>());
        // Layer 0: low cloud / stratus shell
        cp.layers[0] = { cloudBaseAltM, 1.0f, 0.80f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f };
        // Layer 1: high cirrus shell
        cp.layers[1] = { cloudTopAltM,  2.0f, 0.15f, 2.0f, 0.5f, 0.4f, 1.0f, 0.0f };
        // Layers 2-3: unused
        cp.layers[2] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        cp.layers[3] = { 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f };
        memcpy(cloudParamsMapped, &cp, sizeof(cp));
    }

    // ── Pass 1: sky/ground background (fullscreen triangle, opaque) ──────────
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyBgPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            skyBgPipeLayout, 0, 1, &skyDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, skyBgPipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);

    // ── Pass 2: satellite points (additive blending) ──────────────────────────
    if (activeSatCount > 0)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                drawPipeLayout, 0, 1, &descSet, 0, nullptr);
        vkCmdPushConstants(cmd, drawPipeLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, activeSatCount, 1, 0, 0);
    }

    // ── Pass 3: background stars (additive blending) ──────────────────────────
    if (starCount > 0 && starPipeline != VK_NULL_HANDLE)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, starPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                starPipeLayout, 0, 1, &starDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, starPipeLayout, VK_SHADER_STAGE_VERTEX_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, starCount, 1, 0, 0);
    }
}

// Icon index constants (match order passed to ui.loadIcons)
static constexpr int kIconAngleLeft = 0;  // pixel--angle-left.png  → slow down
static constexpr int kIconAngleRight = 1; // pixel--angle-right.png → speed up
// 2 = controller (unused in time controls)
static constexpr int kIconPause = 3;    // pixel--pause.png
static constexpr int kIconPlay = 4;     // pixel--play.png
static constexpr int kIconSettings = 5; // pixel--settings.png

// Helper: short display name for a GLFW key code (used in settings window).
static const char *keyDisplayName(int key)
{
    switch (key)
    {
    case GLFW_KEY_SPACE:
        return "Space";
    case GLFW_KEY_TAB:
        return "Tab";
    case GLFW_KEY_COMMA:
        return ",";
    case GLFW_KEY_PERIOD:
        return ".";
    case GLFW_KEY_ESCAPE:
        return "Esc";
    case GLFW_KEY_ENTER:
        return "Enter";
    case GLFW_KEY_LEFT_SHIFT:
        return "LShift";
    case GLFW_KEY_RIGHT_SHIFT:
        return "RShift";
    case GLFW_KEY_LEFT_CONTROL:
        return "LCtrl";
    case GLFW_KEY_RIGHT_CONTROL:
        return "RCtrl";
    case GLFW_KEY_LEFT_ALT:
        return "LAlt";
    case GLFW_KEY_RIGHT_ALT:
        return "RAlt";
    case GLFW_KEY_LEFT_SUPER:
        return "LSuper";
    case GLFW_KEY_RIGHT_SUPER:
        return "RSuper";
    case GLFW_KEY_F11:
        return "F11";
    case GLFW_KEY_F1:
        return "F1";
    case GLFW_KEY_F2:
        return "F2";
    case GLFW_KEY_F3:
        return "F3";
    case GLFW_KEY_F4:
        return "F4";
    case GLFW_KEY_F5:
        return "F5";
    case GLFW_KEY_F6:
        return "F6";
    case GLFW_KEY_F7:
        return "F7";
    case GLFW_KEY_F8:
        return "F8";
    case GLFW_KEY_F9:
        return "F9";
    case GLFW_KEY_F10:
        return "F10";
    case GLFW_KEY_F12:
        return "F12";
    case GLFW_KEY_UP:
        return "Up";
    case GLFW_KEY_DOWN:
        return "Down";
    case GLFW_KEY_LEFT:
        return "Left";
    case GLFW_KEY_RIGHT:
        return "Right";
    case GLFW_KEY_PAGE_UP:
        return "PgUp";
    case GLFW_KEY_PAGE_DOWN:
        return "PgDn";
    case GLFW_KEY_HOME:
        return "Home";
    case GLFW_KEY_END:
        return "End";
    case GLFW_KEY_INSERT:
        return "Ins";
    case GLFW_KEY_DELETE:
        return "Del";
    case GLFW_KEY_BACKSPACE:
        return "Bksp";
    case GLFW_KEY_SLASH:
        return "/";
    case GLFW_KEY_BACKSLASH:
        return "\\";
    case GLFW_KEY_SEMICOLON:
        return ";";
    case GLFW_KEY_APOSTROPHE:
        return "'";
    case GLFW_KEY_LEFT_BRACKET:
        return "[";
    case GLFW_KEY_RIGHT_BRACKET:
        return "]";
    case GLFW_KEY_MINUS:
        return "-";
    case GLFW_KEY_EQUAL:
        return "=";
    default:
        if (key >= GLFW_KEY_A && key <= GLFW_KEY_Z)
        {
            static char buf[2] = {};
            buf[0] = (char)('A' + (key - GLFW_KEY_A));
            return buf;
        }
        if (key >= GLFW_KEY_0 && key <= GLFW_KEY_9)
        {
            static char buf[2] = {};
            buf[0] = (char)('0' + (key - GLFW_KEY_0));
            return buf;
        }
        return "?";
    }
}

// ── UI color palette ──────────────────────────────────────────────────────────
// Edit here to restyle the entire UI. All buildUI colors reference these names.
namespace Pal
{
    // Backgrounds
    constexpr Clay_Color panelBg = {8, 8, 9, 210};            // floating panel
    constexpr Clay_Color panelBgFade = {8, 8, 9, 180};        // panel, slightly transparent
    constexpr Clay_Color panelSolid = {12, 12, 13, 245};      // settings window
    constexpr Clay_Color titleBar = {18, 18, 19, 255};        // title / header strip
    constexpr Clay_Color sectionHdr = {22, 22, 23, 130};      // section divider strip
    constexpr Clay_Color rowEnabled = {45, 10, 10, 180};      // enabled constellation row
    constexpr Clay_Color rowDisabled = {16, 16, 17, 160};     // disabled constellation row
    constexpr Clay_Color rowHighlight = {35, 30, 8, 180};     // highlighted constellation row
    constexpr Clay_Color btnHighlight = {160, 120, 15, 240};  // HLT active (amber)
    constexpr Clay_Color btnHighlightHv = {110, 85, 10, 230}; // HLT hovered
    constexpr Clay_Color listenRow = {50, 10, 10, 185};       // keybind capture row
    // Buttons
    constexpr Clay_Color btnIdle = {30, 30, 31, 210};      // default button
    constexpr Clay_Color btnHover = {52, 52, 54, 230};     // hovered button
    constexpr Clay_Color btnAccent = {150, 20, 20, 240};   // ON / active (red)
    constexpr Clay_Color btnAccentHv = {100, 15, 15, 230}; // accent hovered
    constexpr Clay_Color closeBgIdle = {50, 16, 16, 180};  // [X] idle
    constexpr Clay_Color closeBgHov = {170, 30, 30, 220};  // [X] hovered
    constexpr Clay_Color pauseActive = {140, 25, 25, 230}; // pause btn when paused
    constexpr Clay_Color listenBtn = {120, 18, 18, 220};   // rebind btn while listening
    // Chrome
    constexpr Clay_Color divider = {48, 48, 50, 120}; // separator line
    // Text
    constexpr Clay_Color textPrimary = {205, 205, 210, 255}; // main readable text
    constexpr Clay_Color textDim = {130, 130, 135, 200};     // secondary / dim
    constexpr Clay_Color textHint = {72, 72, 76, 160};       // hint / footer
    constexpr Clay_Color textSection = {155, 155, 165, 200}; // section header labels
    constexpr Clay_Color textCamera = {110, 110, 115, 180};  // dim descriptive text
    constexpr Clay_Color volLabel = {185, 185, 195, 220};    // vol/scale label
    constexpr Clay_Color volValue = {210, 210, 215, 255};    // vol/scale value readout
    constexpr Clay_Color btnLabel = {210, 210, 215, 255};    // text inside +/- buttons
    constexpr Clay_Color listenKey = {255, 85, 85, 255};     // key label while listening
    constexpr Clay_Color keyText = {140, 140, 145, 200};     // normal key label
    // Speed indicator
    constexpr Clay_Color speedFwd = {200, 55, 55, 220};    // forward (red)
    constexpr Clay_Color speedRev = {155, 155, 165, 220};  // reverse (grey)
    constexpr Clay_Color speedPaused = {95, 95, 100, 220}; // paused (dark grey)
}

// ─── buildUI ──────────────────────────────────────────────────────────────────
void SatelliteSim::buildUI(float dt, UIRenderer &ui)
{
    // Apply camera mouse look.
    // Yaw  (dmx): rotate obsFacing around obsDir via Rodrigues — no ENU frame, no pole issue.
    // Pitch (dmy): handled by camera.update → camera.elDeg as usual.
    //
    // Cinematic mode (RMB + ALT held): mouse input adds force to a velocity that
    // drifts and decays, so the camera coasts smoothly after the mouse stops.
    // Releasing ALT instantly zeroes the velocity and returns to direct control.
    if (win)
    {
        // Clear cinematic mode as soon as RMB is released — the toggle only lives
        // while a pan is active, so it resets automatically for the next drag.
        if (!camera.captured && cinematicMode)
        {
            cinematicMode = false;
            cinematicYawVel = 0.0f;
            cinematicPitchVel = 0.0f;
        }

        bool cinematic = camera.captured && cinematicMode;

        if (cinematic)
        {
            // Let camera.update handle RMB capture/release without applying any rotation.
            camera.update(win, 0.0f, 0.0f);

            // Mouse input adds to velocity as an impulse (kForce fraction of raw delta).
            // Velocity units: pixels-equivalent — same as dmx/dmy — so it slots straight
            // into the Rodrigues and elDeg formulas below without any unit conversion.
            const float kForce = 0.06f;
            cinematicYawVel += dmx * kForce;
            cinematicPitchVel += dmy * kForce;

            // Apply velocity this frame (identical math to the direct-control path).
            if (fabsf(cinematicYawVel) > 0.0001f)
            {
                float angle = glm::radians(-cinematicYawVel * camera.sens);
                glm::vec3 leftDir = glm::cross(obsDir, obsFacing);
                obsFacing = glm::normalize(cosf(angle) * obsFacing + sinf(angle) * leftDir);
            }
            camera.elDeg -= cinematicPitchVel * camera.sens;
            camera.elDeg = glm::clamp(camera.elDeg, -89.0f, 89.0f);

            // Exponential decay — half-life ≈ 0.87 s (kDamp = 0.8 → e^-0.8 per second).
            // float decay = expf(-0.999f * dt);
            // cinematicYawVel *= decay;
            // cinematicPitchVel *= decay;

            cinematicActive = true;
        }
        else
        {
            // Normal direct control: pitch via camera.update, yaw via Rodrigues.
            camera.update(win, 0.0f, dmy);

            if (camera.captured && dmx != 0.0f)
            {
                // cross(obsDir, obsFacing) is the LEFT tangent, so negate angle for look-right.
                float angle = glm::radians(-dmx * camera.sens);
                glm::vec3 leftDir = glm::cross(obsDir, obsFacing);
                obsFacing = glm::normalize(cosf(angle) * obsFacing + sinf(angle) * leftDir);
            }

            // Kill any residual drift immediately when leaving cinematic mode.
            if (cinematicActive)
            {
                cinematicYawVel = 0.0f;
                cinematicPitchVel = 0.0f;
                cinematicActive = false;
            }
        }

        // Derive camera.azDeg from obsFacing projected into the local Earth-fixed ENU.
        // Only used for the view matrix — never fed back into movement math.
        {
            float sL = obsDir.z;
            float cLH = sqrtf(obsDir.x * obsDir.x + obsDir.y * obsDir.y);
            float inv = (cLH > 1e-7f) ? 1.0f / cLH : 0.0f;
            float cLn = obsDir.x * inv, sLn = obsDir.y * inv;
            glm::vec3 eastEF = {-sLn, cLn, 0.0f};
            glm::vec3 northEF = {-sL * cLn, -sL * sLn, cLH};
            camera.azDeg = glm::degrees(atan2f(
                glm::dot(obsFacing, eastEF),
                glm::dot(obsFacing, northEF)));
        }
    }
    dmx = dmy = 0.0f;

    const UIInput &inp = ui.input();

    // ── Font-size helper: scales base pixel size by uiScale ───────────────────
    auto fs = [&](int base) -> uint16_t
    {
        return (uint16_t)std::max(8, (int)(base * uiScale + 0.5f));
    };

    // ── Audio helpers: rollover fires on hover transition, click on LMB press ─
    // Both are no-ops when audio_ is null (audio init failed or not yet set).
    auto sndRollover = [&](bool nowHov, bool prevHov)
    {
        if (audio_ && nowHov && !prevHov)
            audio_->playSfx("assets/sound/ui/buttonrollover.wav");
    };
    auto sndClick = [&](bool nowHov)
    {
        if (audio_ && nowHov && inp.lmbPressed)
            audio_->playSfx("assets/sound/ui/buttonclick.wav");
    };

    // ── Lazy icon loading (first buildUI call after init) ─────────────────────
    if (!iconsLoaded && ctx_)
    {
        const char *iconPaths[] = {
            "assets/icons/ui/pixel--angle-left.png",
            "assets/icons/ui/pixel--angle-right.png",
            "assets/icons/ui/pixel--controller.png",
            "assets/icons/ui/pixel--pause.png",
            "assets/icons/ui/pixel--play.png",
            "assets/icons/ui/pixel--settings.png",
        };
        ui.loadIcons(*ctx_, iconPaths, 6);
        iconsLoaded = true;
    }

    // ── Scroll wheel → FOV zoom (when not hovering over UI panels) ───────────
    if (inp.scrollY != 0.0f && !ui.mouseOverUI())
    {
        camera.fovYDeg = glm::clamp(camera.fovYDeg - inp.scrollY * 3.0f, 10.0f, 120.0f);
    }

    // ── Tab: skip all UI when hidden ─────────────────────────────────────────
    if (!uiVisible)
        return;

    // ── Simulated UTC time string ─────────────────────────────────────────────
    static char timeBuf[32];
    {
        time_t unixSim = (time_t)(simDayJ2000 * 86400LL + (int64_t)simSecInDay) + 946728000;
        struct tm *utc = gmtime(&unixSim);
        if (utc)
            snprintf(timeBuf, sizeof(timeBuf), "UTC %04d-%02d-%02d %02d:%02d:%02d",
                     utc->tm_year + 1900, utc->tm_mon + 1, utc->tm_mday,
                     utc->tm_hour, utc->tm_min, utc->tm_sec);
        else
            snprintf(timeBuf, sizeof(timeBuf), "UTC --");
    }

    // ── Status bar data (bottom-right toolbar) ────────────────────────────────
    // setLat: moves the observer to a new latitude while preserving longitude direction
    // and parallel-transporting obsFacing so it stays tangent after the position jump.
    auto setLat = [this](float newLatDeg)
    {
        newLatDeg = glm::clamp(newLatDeg, -90.0f, 90.0f);
        float sinL = sinf(glm::radians(newLatDeg));
        float cosL = cosf(glm::radians(newLatDeg));
        glm::vec2 xy = glm::vec2(obsDir.x, obsDir.y);
        float xyMag = glm::length(xy);
        if (xyMag > 1e-6f)
            xy /= xyMag;
        else
            xy = {1.0f, 0.0f};
        obsDir = {xy.x * cosL, xy.y * cosL, sinL};
        obsFacing = glm::normalize(obsFacing - glm::dot(obsFacing, obsDir) * obsDir);
        obsLatDeg = newLatDeg;
        obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
    };

    static char latBuf[20], lonBuf[20], statFpsBuf[24], statVisBuf[32], statLoopBuf[24];
    {
        float absLat = fabsf(obsLatDeg);
        float absLon = fabsf(obsLonDeg);
        snprintf(latBuf, sizeof(latBuf), "%.1f\xc2\xb0 %c", absLat, obsLatDeg >= 0.0f ? 'N' : 'S');
        snprintf(lonBuf, sizeof(lonBuf), "%.1f\xc2\xb0 %c", absLon, obsLonDeg >= 0.0f ? 'E' : 'W');
        snprintf(statFpsBuf, sizeof(statFpsBuf), "%.0f fps", dt > 0.0f ? 1.0f / dt : 0.0f);
        snprintf(statVisBuf, sizeof(statVisBuf), "%u vis", gpuSatCount);
        snprintf(statLoopBuf, sizeof(statLoopBuf), "%.2f ms", loopMs);
    }
    Clay_String latStr{false, (int32_t)strlen(latBuf), latBuf};
    Clay_String lonStr{false, (int32_t)strlen(lonBuf), lonBuf};
    Clay_String statFpsStr{false, (int32_t)strlen(statFpsBuf), statFpsBuf};
    Clay_String statVisStr{false, (int32_t)strlen(statVisBuf), statVisBuf};
    Clay_String statLoopStr{false, (int32_t)strlen(statLoopBuf), statLoopBuf};

    // ── Time controls (bottom-left) ───────────────────────────────────────────
    // Shows UTC time, a slow/pause|play/fast icon button row, speed label, and reverse indicator.
    static char speedBuf[24];
    snprintf(speedBuf, sizeof(speedBuf), "%s%s",
             timeDir < 0.0f ? "REV " : "", kTimeLabels[timeScaleIdx]);

    CLAY(CLAY_ID("TimePanel"), {.layout = {
                                    .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                    .padding = {10, 10, 8, 8},
                                    .childGap = 6,
                                    .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                .backgroundColor = Pal::panelBg,
                                .cornerRadius = CLAY_CORNER_RADIUS(6),
                                .floating = {.offset = {12, inp.screenH - 110.0f}, .zIndex = 5, .attachTo = CLAY_ATTACH_TO_ROOT}})
    {
        // UTC time + speed indicator in one row
        CLAY(CLAY_ID("TimeHeaderRow"), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .childGap = 8,
                                            .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            Clay_String timeStr{false, (int32_t)strlen(timeBuf), timeBuf};
            CLAY_TEXT(timeStr,
                      CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));

            // Speed / direction label
            Clay_Color speedCol = timePaused       ? Pal::speedPaused
                                  : timeDir < 0.0f ? Pal::speedRev
                                                   : Pal::speedFwd;
            Clay_String speedStr{false, (int32_t)strlen(speedBuf), speedBuf};
            CLAY_TEXT(speedStr, CLAY_TEXT_CONFIG({.textColor = speedCol, .fontSize = fs(12)}));
        }

        // Icon button row: [◀] [⏸/▶] [▶]
        const int kBtnSize = 28;
        const int kIconSize = 18;
        CLAY(CLAY_ID("TimeBtnRow"), {.layout = {
                                         .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                         .childGap = 5,
                                         .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                         .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            // ── Slow down ─────────────────────────────────────────────────────
            Clay_Color slowBg = hovTimeSlower ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_ID("TimeSlowerBtn"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = slowBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovTimeSlower);
                    sndClick(n);
                    hovTimeSlower = n;
                }
                if (hovTimeSlower && inp.lmbPressed)
                    timeScaleIdx = std::max(0, timeScaleIdx - 1);
                CLAY(CLAY_ID("TimeSlowerIcon"), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                                 .image = {.imageData = (void *)(intptr_t)(kIconAngleLeft + 1)}}) {}
            }

            // ── Pause / Play ──────────────────────────────────────────────────
            Clay_Color pauseBg = timePaused
                                     ? Pal::pauseActive
                                     : (hovTimePause ? Pal::btnHover : Pal::btnIdle);
            CLAY(CLAY_ID("TimePauseBtn"), {.layout = {
                                               .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                           .backgroundColor = pauseBg,
                                           .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovTimePause);
                    sndClick(n);
                    hovTimePause = n;
                }
                if (hovTimePause && inp.lmbPressed)
                    timePaused = !timePaused;
                int pauseIcon = timePaused ? kIconPlay : kIconPause;
                CLAY(CLAY_ID("TimePauseIcon"), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                                .image = {.imageData = (void *)(intptr_t)(pauseIcon + 1)}}) {}
            }

            // ── Speed up ──────────────────────────────────────────────────────
            Clay_Color fastBg = hovTimeFaster ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_ID("TimeFasterBtn"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(kBtnSize), CLAY_SIZING_FIXED(kBtnSize)},
                                                .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                            .backgroundColor = fastBg,
                                            .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovTimeFaster);
                    sndClick(n);
                    hovTimeFaster = n;
                }
                if (hovTimeFaster && inp.lmbPressed)
                    timeScaleIdx = std::min(kNumTimeScales - 1, timeScaleIdx + 1);
                CLAY(CLAY_ID("TimeFasterIcon"), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(kIconSize), CLAY_SIZING_FIXED(kIconSize)}},
                                                 .image = {.imageData = (void *)(intptr_t)(kIconAngleRight + 1)}}) {}
            }
        }

        // Hint text
        CLAY_TEXT(CLAY_STRING(",/. = speed  Space = pause  R = reverse  Tab = hide UI"),
                  CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(10)}));
    }

    // ── Status bar (bottom-right): lat/lon, fps, vis count, settings gear ────
    {
        const int kSBBtnSz = 22;
        const int kSBIconSz = 14;
        const int kGearSz = 28;
        Clay_Color settingsBg = hovSettings ? Pal::btnHover : Pal::panelBgFade;

        CLAY(CLAY_ID("StatusBar"), {.layout = {
                                        .sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIXED(38)},
                                        .padding = {10, 10, 6, 6},
                                        .childGap = 7,
                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                    .backgroundColor = Pal::panelBg,
                                    .cornerRadius = CLAY_CORNER_RADIUS(6),
                                    .floating = {.offset = {-12.0f, -12.0f}, .zIndex = 5, .attachPoints = {.element = CLAY_ATTACH_POINT_RIGHT_BOTTOM, .parent = CLAY_ATTACH_POINT_RIGHT_BOTTOM}, .attachTo = CLAY_ATTACH_TO_ROOT}})
        {
            // ── Lat south button ──────────────────────────────────────────────
            Clay_Color sbSBg = hovLatSouth ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_ID("SBLatSBtn"), {.layout = {
                                            .sizing = {CLAY_SIZING_FIXED(kSBBtnSz), CLAY_SIZING_FIXED(kSBBtnSz)},
                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                        .backgroundColor = sbSBg,
                                        .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovLatSouth);
                    sndClick(n);
                    hovLatSouth = n;
                }
                if (hovLatSouth && inp.lmbPressed)
                    setLat(obsLatDeg - 5.0f);
                CLAY(CLAY_ID("SBLatSIcon"), {.layout = {.sizing = {CLAY_SIZING_FIXED(kSBIconSz), CLAY_SIZING_FIXED(kSBIconSz)}},
                                             .image = {.imageData = (void *)(intptr_t)(kIconAngleLeft + 1)}}) {}
            }

            // ── Lat display (scroll to adjust) ────────────────────────────────
            CLAY(CLAY_ID("SBLatDisplay"), {.layout = {
                                               .sizing = {CLAY_SIZING_FIXED(62), CLAY_SIZING_FIT(0)},
                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                if (Clay_Hovered() && inp.scrollY != 0.0f)
                    setLat(obsLatDeg + inp.scrollY * 5.0f);
                CLAY_TEXT(latStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
            }

            // ── Lat north button ──────────────────────────────────────────────
            Clay_Color sbNBg = hovLatNorth ? Pal::btnHover : Pal::btnIdle;
            CLAY(CLAY_ID("SBLatNBtn"), {.layout = {
                                            .sizing = {CLAY_SIZING_FIXED(kSBBtnSz), CLAY_SIZING_FIXED(kSBBtnSz)},
                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                        .backgroundColor = sbNBg,
                                        .cornerRadius = CLAY_CORNER_RADIUS(3)})
            {
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovLatNorth);
                    sndClick(n);
                    hovLatNorth = n;
                }
                if (hovLatNorth && inp.lmbPressed)
                    setLat(obsLatDeg + 5.0f);
                CLAY(CLAY_ID("SBLatNIcon"), {.layout = {.sizing = {CLAY_SIZING_FIXED(kSBIconSz), CLAY_SIZING_FIXED(kSBIconSz)}},
                                             .image = {.imageData = (void *)(intptr_t)(kIconAngleRight + 1)}}) {}
            }

            CLAY(CLAY_ID("SBDiv1"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                     .backgroundColor = Pal::divider}) {}

            // ── Lon display ───────────────────────────────────────────────────
            CLAY(CLAY_ID("SBLonDisplay"), {.layout = {
                                               .sizing = {CLAY_SIZING_FIXED(62), CLAY_SIZING_FIT(0)},
                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(lonStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
            }

            CLAY(CLAY_ID("SBDiv2"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                     .backgroundColor = Pal::divider}) {}

            // ── FPS ───────────────────────────────────────────────────────────
            CLAY(CLAY_ID("SBFps"), {.layout = {
                                        .sizing = {CLAY_SIZING_FIXED(50), CLAY_SIZING_FIT(0)},
                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(statFpsStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));
            }

            CLAY(CLAY_ID("SBDiv3"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                     .backgroundColor = Pal::divider}) {}

            // ── Visible sat count ─────────────────────────────────────────────
            CLAY(CLAY_ID("SBVis"), {.layout = {
                                        .sizing = {CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT(0)},
                                        .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(statVisStr, CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(12)}));
            }

            CLAY(CLAY_ID("SBDiv4"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                     .backgroundColor = Pal::divider}) {}

            CLAY(CLAY_ID("SBLoop"), {.layout = {
                                         .sizing = {CLAY_SIZING_FIXED(62), CLAY_SIZING_FIT(0)},
                                         .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
            {
                CLAY_TEXT(statLoopStr, CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(12)}));
            }

            CLAY(CLAY_ID("SBDiv5"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1), CLAY_SIZING_FIXED(20)}},
                                     .backgroundColor = Pal::divider}) {}

            // ── Settings gear button ──────────────────────────────────────────
            CLAY(CLAY_ID("SettingsBtn"), {.layout = {
                                              .sizing = {CLAY_SIZING_FIXED(kGearSz), CLAY_SIZING_FIXED(kGearSz)},
                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = settingsBg,
                                          .cornerRadius = CLAY_CORNER_RADIUS(4)})
            {
                {
                    bool n = Clay_Hovered();
                    sndRollover(n, hovSettings);
                    sndClick(n);
                    if (n && inp.lmbPressed)
                        settingsOpen = !settingsOpen;
                    hovSettings = n;
                }
                CLAY(CLAY_ID("SettingsIcon"), {.layout = {.sizing = {CLAY_SIZING_FIXED(18), CLAY_SIZING_FIXED(18)}},
                                               .image = {.imageData = (void *)(intptr_t)(kIconSettings + 1)}}) {}
            }
        }
    }

    // ── Settings window ───────────────────────────────────────────────────────
    if (settingsOpen)
    {
        const float kWinW = 500.0f;
        const float kWinH = 500.0f;

        // Centre on first open this session; restore saved position afterward.
        if (settingsWinX < 0.0f)
        {
            settingsWinX = (inp.screenW - kWinW) * 0.5f;
            settingsWinY = (inp.screenH - kWinH) * 0.5f;
        }

        // Apply drag delta before Clay layout so position is correct this frame.
        if (!inp.lmbDown)
            settingsDragging = false;
        if (settingsDragging)
        {
            settingsWinX += inp.dMouseX;
            settingsWinY += inp.dMouseY;
        }
        settingsWinX = glm::clamp(settingsWinX, 0.0f, inp.screenW - kWinW);
        settingsWinY = glm::clamp(settingsWinY, 0.0f, inp.screenH - kWinH);

        CLAY(CLAY_ID("SettingsWin"), {.layout = {
                                          .sizing = {CLAY_SIZING_FIXED(kWinW), CLAY_SIZING_FIXED(kWinH)},
                                          .padding = {0, 0, 0, 0},
                                          .childGap = 0,
                                          .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                      .backgroundColor = Pal::panelSolid,
                                      .cornerRadius = CLAY_CORNER_RADIUS(8),
                                      .floating = {.offset = {settingsWinX, settingsWinY}, .zIndex = 10, .attachTo = CLAY_ATTACH_TO_ROOT}})
        {
            // Title bar
            CLAY(CLAY_ID("SettingsTitleBar"), {.layout = {
                                                   .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(36)},
                                                   .padding = {14, 14, 0, 0},
                                                   .childGap = 0,
                                                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                   .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                               .backgroundColor = Pal::titleBar,
                                               .cornerRadius = {8, 8, 0, 0}})
            {
                // Start drag when the title bar is clicked outside the close button.
                {
                    bool n = Clay_Hovered();
                    if (n && inp.lmbPressed && !hovSettingsClose)
                        settingsDragging = true;
                }

                CLAY_TEXT(CLAY_STRING("Settings"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(16)}));

                // Spacer
                CLAY(CLAY_ID("SettingsTitleSpacer"), {.layout = {
                                                          .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

                // Close button [X]
                Clay_Color closeBg = hovSettingsClose ? Pal::closeBgHov : Pal::closeBgIdle;
                CLAY(CLAY_ID("SettingsCloseBtn"), {.layout = {
                                                       .sizing = {CLAY_SIZING_FIXED(24), CLAY_SIZING_FIXED(24)},
                                                       .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                   .backgroundColor = closeBg,
                                                   .cornerRadius = CLAY_CORNER_RADIUS(4)})
                {
                    {
                        bool n = Clay_Hovered();
                        sndRollover(n, hovSettingsClose);
                        sndClick(n);
                        hovSettingsClose = n;
                    }
                    if (hovSettingsClose && inp.lmbPressed)
                    {
                        settingsOpen = false;
                        saveSettings();
                    }
                    CLAY_TEXT(CLAY_STRING("X"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
                }
            }

            // Scrollable controls list
            CLAY(CLAY_ID("SettingsScroll"), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                 .padding = {14, 14, 10, 10},
                                                 .childGap = 4,
                                                 .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                             .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
            {
                // ── Constellations ────────────────────────────────────────────
                CLAY_TEXT(CLAY_STRING("Constellations"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));
                CLAY(CLAY_ID("ConstSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                      .padding = {0, 0, 4, 4}},
                                           .backgroundColor = Pal::sectionHdr}) {}

                static char constCntBuf[256][16]; // one slot per constellation; 256 > any realistic mod
                for (int ci = 0; ci < (int)constellations.size() && ci < 256; ++ci)
                {
                    ConstellationConfig &c = constellations[ci];
                    snprintf(constCntBuf[ci], sizeof(constCntBuf[ci]), "%u", c.orbitCount);

                    bool hov = ci < (int)hovConst.size() && hovConst[ci];
                    bool hovHlt = ci < (int)hovHighlightConst.size() && hovHighlightConst[ci];
                    Clay_Color rowBg = c.highlight ? Pal::rowHighlight
                                                   : (c.enabled ? Pal::rowEnabled : Pal::rowDisabled);
                    CLAY(CLAY_IDI("ConstRow", ci), {.layout = {
                                                        .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(24)},
                                                        .padding = {4, 4, 3, 3},
                                                        .childGap = 6,
                                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                    .backgroundColor = rowBg,
                                                    .cornerRadius = CLAY_CORNER_RADIUS(3)})
                    {
                        // ── ON/OFF toggle ────────────────────────────────────
                        Clay_Color btnBg = c.enabled
                                               ? Pal::btnAccent
                                               : (hov ? Pal::btnAccentHv : Pal::btnIdle);
                        CLAY(CLAY_IDI("ConstBtn", ci), {.layout = {
                                                            .sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(18)},
                                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                        .backgroundColor = btnBg,
                                                        .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            {
                                bool n = Clay_Hovered();
                                sndRollover(n, hov);
                                sndClick(n);
                                if (ci < (int)hovConst.size())
                                    hovConst[ci] = n;
                            }
                            if (hov && inp.lmbPressed)
                                c.enabled = !c.enabled;
                            CLAY_TEXT(c.enabled ? CLAY_STRING("ON") : CLAY_STRING("OFF"),
                                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(10)}));
                        }
                        // ── Highlight toggle ─────────────────────────────────
                        Clay_Color hltBg = c.highlight
                                               ? Pal::btnHighlight
                                               : (hovHlt ? Pal::btnHighlightHv : Pal::btnIdle);
                        CLAY(CLAY_IDI("ConstHltBtn", ci), {.layout = {
                                                               .sizing = {CLAY_SIZING_FIXED(30), CLAY_SIZING_FIXED(18)},
                                                               .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                           .backgroundColor = hltBg,
                                                           .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            {
                                bool n = Clay_Hovered();
                                sndRollover(n, hovHlt);
                                sndClick(n);
                                if (ci < (int)hovHighlightConst.size())
                                    hovHighlightConst[ci] = n;
                            }
                            if (hovHlt && inp.lmbPressed)
                                c.highlight = !c.highlight;
                            CLAY_TEXT(CLAY_STRING("HLT"),
                                      CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(10)}));
                        }
                        CLAY(CLAY_IDI("ConstName", ci), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)}}})
                        {
                            Clay_String nameStr{false, (int32_t)c.name.size(), c.name.data()};
                            CLAY_TEXT(nameStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
                        }
                        CLAY(CLAY_IDI("ConstCnt", ci), {.layout = {
                                                            .sizing = {CLAY_SIZING_FIXED(52), CLAY_SIZING_FIT(0)},
                                                            .childAlignment = {.x = CLAY_ALIGN_X_RIGHT}}})
                        {
                            Clay_String cntStr{false, (int32_t)strlen(constCntBuf[ci]), constCntBuf[ci]};
                            CLAY_TEXT(cntStr, CLAY_TEXT_CONFIG({.textColor = Pal::textCamera, .fontSize = fs(11)}));
                        }
                    }
                }

                // ── Sound ────────────────────────────────────────────────────
                CLAY(CLAY_ID("SndTopSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                       .padding = {0, 0, 6, 4}},
                                            .backgroundColor = Pal::sectionHdr}) {}
                CLAY_TEXT(CLAY_STRING("Sound"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));
                CLAY(CLAY_ID("SndSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                    .padding = {0, 0, 4, 4}},
                                         .backgroundColor = Pal::sectionHdr}) {}

                // Helper: one volume row (label, spacer, −, value, +)
                // We use a macro-like lambda to avoid copy-pasting Clay layout 3 times.
                static char volBufs[3][8];
                struct VolRow
                {
                    const char *label;
                    float vol;
                    bool &hMinus;
                    bool &hPlus;
                    const char *idMinus;
                    const char *idPlus;
                    const char *idVal;
                    int bufIdx;
                };
                VolRow volRows[] = {
                    {"Master vol", audio_ ? audio_->getMasterVolume() : masterVol_, hovMasterVolMinus, hovMasterVolPlus, "MasterVolMinus", "MasterVolPlus", "MasterVolVal", 0},
                    {"Music vol", audio_ ? audio_->getMusicVolume() : musicVol_, hovMusicVolMinus, hovMusicVolPlus, "MusicVolMinus", "MusicVolPlus", "MusicVolVal", 1},
                    {"SFX vol", audio_ ? audio_->getSfxVolume() : sfxVol_, hovSfxVolMinus, hovSfxVolPlus, "SfxVolMinus", "SfxVolPlus", "SfxVolVal", 2},
                };
                for (auto &vr : volRows)
                {
                    snprintf(volBufs[vr.bufIdx], sizeof(volBufs[0]), "%3.0f%%", vr.vol * 100.0f);
                    Clay_String volStr{false, (int32_t)strlen(volBufs[vr.bufIdx]), volBufs[vr.bufIdx]};
                    CLAY(CLAY_IDI("VolRow", vr.bufIdx), {.layout = {
                                                             .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(26)},
                                                             .padding = {4, 4, 2, 2},
                                                             .childGap = 6,
                                                             .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                             .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY(CLAY_IDI("VolLabel", vr.bufIdx), {.layout = {.sizing = {CLAY_SIZING_FIXED(76), CLAY_SIZING_FIT(0)}}})
                        {
                            Clay_String lblStr{false, (int32_t)strlen(vr.label), vr.label};
                            CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
                        }
                        CLAY(CLAY_IDI("VolSpc", vr.bufIdx), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}
                        // − button
                        Clay_Color cMinus = vr.hMinus ? Pal::btnHover : Pal::btnIdle;
                        CLAY(CLAY_IDI("VolMinus", vr.bufIdx), {.layout = {
                                                                   .sizing = {CLAY_SIZING_FIXED(20), CLAY_SIZING_FIXED(20)},
                                                                   .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                               .backgroundColor = cMinus,
                                                               .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, vr.hMinus);
                            sndClick(n);
                            vr.hMinus = n;
                            if (vr.hMinus && inp.lmbPressed)
                            {
                                if (vr.bufIdx == 0 && audio_)
                                    audio_->setMasterVolume(audio_->getMasterVolume() - 0.05f);
                                else if (vr.bufIdx == 1 && audio_)
                                    audio_->setMusicVolume(audio_->getMusicVolume() - 0.05f);
                                else if (vr.bufIdx == 2 && audio_)
                                    audio_->setSfxVolume(audio_->getSfxVolume() - 0.05f);
                            }
                            CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
                        }
                        // value
                        CLAY(CLAY_IDI("VolVal", vr.bufIdx), {.layout = {
                                                                 .sizing = {CLAY_SIZING_FIXED(38), CLAY_SIZING_FIT(0)},
                                                                 .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
                        {
                            CLAY_TEXT(volStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
                        }
                        // + button
                        Clay_Color cPlus = vr.hPlus ? Pal::btnHover : Pal::btnIdle;
                        CLAY(CLAY_IDI("VolPlus", vr.bufIdx), {.layout = {
                                                                  .sizing = {CLAY_SIZING_FIXED(20), CLAY_SIZING_FIXED(20)},
                                                                  .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                              .backgroundColor = cPlus,
                                                              .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, vr.hPlus);
                            sndClick(n);
                            vr.hPlus = n;
                            if (vr.hPlus && inp.lmbPressed)
                            {
                                if (vr.bufIdx == 0 && audio_)
                                    audio_->setMasterVolume(audio_->getMasterVolume() + 0.05f);
                                else if (vr.bufIdx == 1 && audio_)
                                    audio_->setMusicVolume(audio_->getMusicVolume() + 0.05f);
                                else if (vr.bufIdx == 2 && audio_)
                                    audio_->setSfxVolume(audio_->getSfxVolume() + 0.05f);
                            }
                            CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
                        }
                    }
                }

                // ── Controls ──────────────────────────────────────────────────
                CLAY(CLAY_ID("SndCtrlSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                        .padding = {0, 0, 6, 4}},
                                             .backgroundColor = Pal::sectionHdr}) {}
                CLAY_TEXT(CLAY_STRING("Controls"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));

                // Thin separator
                CLAY(CLAY_ID("CtrlSep"), {.layout = {
                                              .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                              .padding = {0, 0, 4, 4}},
                                          .backgroundColor = Pal::sectionHdr}) {}

                // One row per keybinding — driven entirely from the keybindings vector.
                // Adding a new KB_* entry and a keybindings[] initializer line is all
                // that's needed; this loop and the rebind logic require no changes.
                static char kbKeyBuf[KB_COUNT][16];
                for (int ki = 0; ki < (int)keybindings.size() && ki < KB_COUNT; ++ki)
                {
                    KeyBinding &kb = keybindings[ki];
                    // Held keys show "(hold)" suffix so users know not to press-and-release.
                    // if (kb.held)
                    //     snprintf(kbKeyBuf[ki], sizeof(kbKeyBuf[ki]), "[%s] hold", keyDisplayName(kb.key));
                    // else
                    snprintf(kbKeyBuf[ki], sizeof(kbKeyBuf[ki]), "[%s]", keyDisplayName(kb.key));

                    Clay_Color rowBg = kb.listening
                                           ? Pal::listenRow
                                           : Clay_Color{0, 0, 0, 0};
                    CLAY(CLAY_IDI("KbRow", ki), {.layout = {
                                                     .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                                     .padding = {4, 4, 4, 4},
                                                     .childGap = 6,
                                                     .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                     .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                 .backgroundColor = rowBg,
                                                 .cornerRadius = CLAY_CORNER_RADIUS(3)})
                    {
                        // Action name (fixed width)
                        CLAY(CLAY_IDI("KbAction", ki), {.layout = {
                                                            .sizing = {CLAY_SIZING_FIXED(130), CLAY_SIZING_FIT(0)}}})
                        {
                            Clay_String actStr{false, (int32_t)strlen(kb.action), kb.action};
                            CLAY_TEXT(actStr,
                                      CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
                        }

                        // Current key label (fixed width)
                        CLAY(CLAY_IDI("KbKey", ki), {.layout = {
                                                         .sizing = {CLAY_SIZING_FIXED(60), CLAY_SIZING_FIT(0)}}})
                        {
                            Clay_String keyStr{false, (int32_t)strlen(kbKeyBuf[ki]), kbKeyBuf[ki]};
                            Clay_Color keyCol = kb.listening
                                                    ? Pal::listenKey
                                                    : Pal::keyText;
                            CLAY_TEXT(keyStr,
                                      CLAY_TEXT_CONFIG({.textColor = keyCol, .fontSize = fs(13)}));
                        }

                        // Rebind button
                        Clay_Color rebindBg = kb.listening
                                                  ? Pal::listenBtn
                                                  : (hovRebind[ki] ? Pal::btnHover : Pal::btnIdle);
                        CLAY(CLAY_IDI("KbRebind", ki), {.layout = {
                                                            .sizing = {CLAY_SIZING_FIXED(120), CLAY_SIZING_FIXED(20)},
                                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                        .backgroundColor = rebindBg,
                                                        .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            {
                                bool n = Clay_Hovered();
                                sndRollover(n, hovRebind[ki]);
                                sndClick(n);
                                hovRebind[ki] = n;
                            }
                            if (hovRebind[ki] && inp.lmbPressed)
                            {
                                // Cancel any other listening binding
                                for (auto &other : keybindings)
                                    other.listening = false;
                                kb.listening = true;
                            }
                            CLAY_TEXT(kb.listening ? CLAY_STRING("PRESS KEY") : CLAY_STRING("Rebind"),
                                      CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(10)}));
                        }
                    }
                }

                // Camera controls section
                CLAY(CLAY_ID("CamCtrlSep"), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                 .padding = {0, 0, 6, 4}},
                                             .backgroundColor = Pal::sectionHdr}) {}
                CLAY_TEXT(CLAY_STRING("Camera"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));
                CLAY_TEXT(CLAY_STRING("Right-click drag   Look around"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textCamera, .fontSize = fs(12)}));
                CLAY_TEXT(CLAY_STRING("Scroll wheel        Zoom (FOV)"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textCamera, .fontSize = fs(12)}));

                // ── UI Scale ──────────────────────────────────────────────────
                CLAY(CLAY_ID("UiScaleSep"), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                 .padding = {0, 0, 6, 4}},
                                             .backgroundColor = Pal::sectionHdr}) {}
                CLAY_TEXT(CLAY_STRING("Display"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));

                CLAY(CLAY_ID("UiScaleRow"), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                                 .padding = {4, 4, 4, 4},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    CLAY_TEXT(CLAY_STRING("Text scale"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));

                    // Spacer
                    CLAY(CLAY_ID("UiScaleSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

                    // − button
                    Clay_Color scaleMinusBg = hovScaleMinus ? Pal::btnHover : Pal::btnIdle;
                    CLAY(CLAY_ID("UiScaleMinus"), {.layout = {
                                                       .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                       .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                   .backgroundColor = scaleMinusBg,
                                                   .cornerRadius = CLAY_CORNER_RADIUS(3)})
                    {
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, hovScaleMinus);
                            sndClick(n);
                            hovScaleMinus = n;
                        }
                        if (hovScaleMinus && inp.lmbPressed)
                            uiScale = std::max(0.75f, uiScale - 0.125f);
                        CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(13)}));
                    }

                    // Scale readout
                    static char scaleBuf[8];
                    snprintf(scaleBuf, sizeof(scaleBuf), "%.2fx", uiScale);
                    Clay_String scaleStr{false, (int32_t)strlen(scaleBuf), scaleBuf};
                    CLAY(CLAY_ID("UiScaleVal"), {.layout = {
                                                     .sizing = {CLAY_SIZING_FIXED(44), CLAY_SIZING_FIT(0)},
                                                     .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
                    {
                        CLAY_TEXT(scaleStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(13)}));
                    }

                    // + button
                    Clay_Color scalePlusBg = hovScalePlus ? Pal::btnHover : Pal::btnIdle;
                    CLAY(CLAY_ID("UiScalePlus"), {.layout = {
                                                      .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                      .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                  .backgroundColor = scalePlusBg,
                                                  .cornerRadius = CLAY_CORNER_RADIUS(3)})
                    {
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, hovScalePlus);
                            sndClick(n);
                            hovScalePlus = n;
                        }
                        if (hovScalePlus && inp.lmbPressed)
                            uiScale = std::min(2.0f, uiScale + 0.125f);
                        CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(13)}));
                    }
                }

                // ── Fullscreen toggle ─────────────────────────────────────────
                bool isFs = win && glfwGetWindowMonitor(win) != nullptr;
                CLAY(CLAY_ID("WinModeRow"), {.layout = {
                                                 .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                                 .padding = {4, 4, 4, 4},
                                                 .childGap = 8,
                                                 .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                 .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    CLAY_TEXT(CLAY_STRING("Window mode"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(13)}));
                    CLAY(CLAY_ID("WinModeSpacer"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)}}}) {}

                    Clay_Color fsBg = isFs ? (hovFullscreen ? Pal::btnAccentHv : Pal::btnAccent)
                                           : (hovFullscreen ? Pal::btnHover : Pal::btnIdle);
                    CLAY(CLAY_ID("FsToggleBtn"), {.layout = {
                                                      .sizing = {CLAY_SIZING_FIXED(92), CLAY_SIZING_FIXED(22)},
                                                      .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                  .backgroundColor = fsBg,
                                                  .cornerRadius = CLAY_CORNER_RADIUS(3)})
                    {
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, hovFullscreen);
                            sndClick(n);
                            hovFullscreen = n;
                        }
                        if (hovFullscreen && inp.lmbPressed && win)
                        {
                            if (!isFs)
                            {
                                glfwGetWindowPos(win, &windowedX, &windowedY);
                                glfwGetWindowSize(win, &windowedW, &windowedH);
                                GLFWmonitor *mon = glfwGetPrimaryMonitor();
                                const GLFWvidmode *mode = glfwGetVideoMode(mon);
                                glfwSetWindowMonitor(win, mon, 0, 0,
                                                     mode->width, mode->height, mode->refreshRate);
                            }
                            else
                            {
                                glfwSetWindowMonitor(win, nullptr,
                                                     windowedX, windowedY, windowedW, windowedH, 0);
                            }
                        }
                        CLAY_TEXT(isFs ? CLAY_STRING("Windowed") : CLAY_STRING("Fullscreen"),
                                  CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(11)}));
                    }
                }

                // ── Photometry ────────────────────────────────────────────────
                CLAY(CLAY_ID("PhotoTopSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                         .padding = {0, 0, 6, 4}},
                                              .backgroundColor = Pal::sectionHdr}) {}
                CLAY_TEXT(CLAY_STRING("Photometry"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));
                CLAY(CLAY_ID("PhotoSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                      .padding = {0, 0, 4, 4}},
                                           .backgroundColor = Pal::sectionHdr}) {}

                // Layout constants — must match Clay sizing declarations exactly for slider hit-test.
                // Row: [Label(110)] [Slider(228)] [Value(58)] [-(22)] [+(22)]  childGap=6 (×4=24)
                // Row padding left=4; scroll padding left=14 → slider abs x = settingsWinX + 14 + 4 + 110 + 6
                const float kPhotoSliderAbsX = settingsWinX + 134.0f;
                const float kPhotoSliderW = 228.0f;

                struct PhotoParam
                {
                    const char *label;
                    float *val;
                    float vmin, vmax, step;
                    const char *fmt;
                    int idx;
                };
                static char photoBufs[5][12];
                PhotoParam photoParams[] = {
                    {"Brightness", &brightnessScale, 0.05f, 20.0f, 0.25f, "%.2f", 0},
                    {"Day suppress", &daySuppression, 5.0f, 5000.0f, 5.0f, "%.0f", 1},
                    {"Mirror boost", &mirrorBoost, 50.0f, 1000.0f, 25.0f, "%.0f", 2},
                    {"Vis threshold", &visThresh, 0.0001f, 0.1f, 0.0001f, "%.3f", 3},
                    {"Hlgt flare", &highlightFlare, 0.01f, 1.0f, 0.01f, "%.2f", 4},
                };
                for (auto &pp : photoParams)
                {
                    int pi = pp.idx;
                    snprintf(photoBufs[pi], sizeof(photoBufs[pi]), pp.fmt, *pp.val);
                    Clay_String valStr{false, (int32_t)strlen(photoBufs[pi]), photoBufs[pi]};
                    float t = glm::clamp((*pp.val - pp.vmin) / (pp.vmax - pp.vmin), 0.0f, 1.0f);

                    CLAY(CLAY_IDI("PhotoRow", pi), {.layout = {
                                                        .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                                        .padding = {4, 4, 4, 4},
                                                        .childGap = 6,
                                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        // Label
                        CLAY(CLAY_IDI("PhotoLbl", pi), {.layout = {.sizing = {CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0)}}})
                        {
                            Clay_String lblStr{false, (int32_t)strlen(pp.label), pp.label};
                            CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
                        }

                        // Slider track — draggable, filled bar shows current value
                        CLAY(CLAY_IDI("PhotoSlider", pi), {.layout = {
                                                               .sizing = {CLAY_SIZING_FIXED(kPhotoSliderW), CLAY_SIZING_FIXED(16)},
                                                               .childGap = 0,
                                                               .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                           .backgroundColor = {22, 22, 24, 255},
                                                           .cornerRadius = CLAY_CORNER_RADIUS(4)})
                        {
                            {
                                bool hov = Clay_Hovered();
                                if (hov && inp.lmbPressed)
                                    draggingPhoto[pi] = true;
                                if (!inp.lmbDown)
                                    draggingPhoto[pi] = false;
                                if (draggingPhoto[pi])
                                {
                                    float nt = (inp.mouseX - kPhotoSliderAbsX) / kPhotoSliderW;
                                    *pp.val = glm::clamp(pp.vmin + nt * (pp.vmax - pp.vmin), pp.vmin, pp.vmax);
                                }
                            }
                            float fillW = t * kPhotoSliderW;
                            if (fillW >= 1.0f)
                            {
                                CLAY(CLAY_IDI("PhotoFill", pi), {.layout = {.sizing = {CLAY_SIZING_FIXED(fillW), CLAY_SIZING_GROW(0)}},
                                                                 .backgroundColor = Pal::btnAccent,
                                                                 .cornerRadius = CLAY_CORNER_RADIUS(3)}) {}
                            }
                        }

                        // Value readout
                        CLAY(CLAY_IDI("PhotoVal", pi), {.layout = {
                                                            .sizing = {CLAY_SIZING_FIXED(58), CLAY_SIZING_FIT(0)},
                                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
                        {
                            CLAY_TEXT(valStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
                        }

                        // − button
                        Clay_Color cMinus = hovPhotoMinus[pi] ? Pal::btnHover : Pal::btnIdle;
                        CLAY(CLAY_IDI("PhotoMinus", pi), {.layout = {
                                                              .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                          .backgroundColor = cMinus,
                                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, hovPhotoMinus[pi]);
                            sndClick(n);
                            hovPhotoMinus[pi] = n;
                            if (hovPhotoMinus[pi] && inp.lmbPressed)
                                *pp.val = glm::clamp(*pp.val - pp.step, pp.vmin, pp.vmax);
                            CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
                        }

                        // + button
                        Clay_Color cPlus = hovPhotoPlus[pi] ? Pal::btnHover : Pal::btnIdle;
                        CLAY(CLAY_IDI("PhotoPlus", pi), {.layout = {
                                                             .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                         .backgroundColor = cPlus,
                                                         .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, hovPhotoPlus[pi]);
                            sndClick(n);
                            hovPhotoPlus[pi] = n;
                            if (hovPhotoPlus[pi] && inp.lmbPressed)
                                *pp.val = glm::clamp(*pp.val + pp.step, pp.vmin, pp.vmax);
                            CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
                        }
                    }
                }

                // ── Clouds ────────────────────────────────────────────────────
                CLAY(CLAY_ID("CloudTopSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                          .padding = {0, 0, 6, 4}},
                                              .backgroundColor = Pal::sectionHdr}) {}
                CLAY_TEXT(CLAY_STRING("Clouds"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));
                CLAY(CLAY_ID("CloudSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                       .padding = {0, 0, 4, 4}},
                                           .backgroundColor = Pal::sectionHdr}) {}

                struct CloudSlider
                {
                    const char *label;
                    float *val;
                    float vmin, vmax, step;
                    const char *fmt;
                    int idx;
                };
                static char cloudBufs[10][16];
                CloudSlider cloudSliders[] = {
                    {"Coverage",    &cloudCoverage,    0.0f,  1.0f,    0.05f,  "%.2f", 0},
                    {"Density",     &cloudDensity,     0.1f,  10.0f,   0.1f,   "%.1f", 1},
                    {"L0 alt (m)",  &cloudBaseAltM,    100.0f, 6000.0f, 100.0f, "%.0f", 2},
                    {"L1 alt (m)",  &cloudTopAltM,     4000.0f,15000.0f,250.0f, "%.0f", 3},
                    {"Drift (1e-6)",&cloudDriftRate,   0.0f,  20e-6f,  0.5e-6f,"%.1e", 4},
                    {"Sun gain",    &cloudSunGain,     0.0f,  5.0f,    0.1f,   "%.2f", 5},
                    {"Ambient",     &cloudAmbientGain, 0.0f,  2.0f,    0.05f,  "%.2f", 6},
                    {"HG g",        &cloudHgG,         0.0f,  0.99f,   0.05f,  "%.2f", 7},
                    {"March steps", &cloudMarchSteps,  4.0f,  128.0f,  4.0f,   "%.0f", 8},
                    {"Light steps", &cloudLightSteps,  1.0f,  16.0f,   1.0f,   "%.0f", 9},
                };
                for (auto &cs : cloudSliders)
                {
                    int ci = cs.idx;
                    snprintf(cloudBufs[ci], sizeof(cloudBufs[ci]), cs.fmt, *cs.val);
                    Clay_String valStr{false, (int32_t)strlen(cloudBufs[ci]), cloudBufs[ci]};
                    float t = glm::clamp((*cs.val - cs.vmin) / (cs.vmax - cs.vmin), 0.0f, 1.0f);

                    CLAY(CLAY_IDI("CloudRow", ci), {.layout = {
                                                        .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(28)},
                                                        .padding = {4, 4, 4, 4},
                                                        .childGap = 6,
                                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        CLAY(CLAY_IDI("CloudLbl", ci), {.layout = {.sizing = {CLAY_SIZING_FIXED(110), CLAY_SIZING_FIT(0)}}})
                        {
                            Clay_String lblStr{false, (int32_t)strlen(cs.label), cs.label};
                            CLAY_TEXT(lblStr, CLAY_TEXT_CONFIG({.textColor = Pal::volLabel, .fontSize = fs(12)}));
                        }

                        CLAY(CLAY_IDI("CloudSlider", ci), {.layout = {
                                                               .sizing = {CLAY_SIZING_FIXED(kPhotoSliderW), CLAY_SIZING_FIXED(16)},
                                                               .childGap = 0,
                                                               .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                           .backgroundColor = {22, 22, 24, 255},
                                                           .cornerRadius = CLAY_CORNER_RADIUS(4)})
                        {
                            {
                                bool hov = Clay_Hovered();
                                if (hov && inp.lmbPressed)
                                    draggingCloud[ci] = true;
                                if (!inp.lmbDown)
                                    draggingCloud[ci] = false;
                                if (draggingCloud[ci])
                                {
                                    float nt = (inp.mouseX - kPhotoSliderAbsX) / kPhotoSliderW;
                                    *cs.val = glm::clamp(cs.vmin + nt * (cs.vmax - cs.vmin), cs.vmin, cs.vmax);
                                }
                            }
                            float fillW = t * kPhotoSliderW;
                            if (fillW >= 1.0f)
                            {
                                CLAY(CLAY_IDI("CloudFill", ci), {.layout = {.sizing = {CLAY_SIZING_FIXED(fillW), CLAY_SIZING_GROW(0)}},
                                                                  .backgroundColor = Pal::btnAccent,
                                                                  .cornerRadius = CLAY_CORNER_RADIUS(3)}) {}
                            }
                        }

                        CLAY(CLAY_IDI("CloudVal", ci), {.layout = {
                                                            .sizing = {CLAY_SIZING_FIXED(58), CLAY_SIZING_FIT(0)},
                                                            .childAlignment = {.x = CLAY_ALIGN_X_CENTER}}})
                        {
                            CLAY_TEXT(valStr, CLAY_TEXT_CONFIG({.textColor = Pal::volValue, .fontSize = fs(12)}));
                        }

                        Clay_Color cMinus = hovCloudMinus[ci] ? Pal::btnHover : Pal::btnIdle;
                        CLAY(CLAY_IDI("CloudMinus", ci), {.layout = {
                                                              .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                          .backgroundColor = cMinus,
                                                          .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, hovCloudMinus[ci]);
                            sndClick(n);
                            hovCloudMinus[ci] = n;
                            if (hovCloudMinus[ci] && inp.lmbPressed)
                                *cs.val = glm::clamp(*cs.val - cs.step, cs.vmin, cs.vmax);
                            CLAY_TEXT(CLAY_STRING("-"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
                        }

                        Clay_Color cPlus = hovCloudPlus[ci] ? Pal::btnHover : Pal::btnIdle;
                        CLAY(CLAY_IDI("CloudPlus", ci), {.layout = {
                                                             .sizing = {CLAY_SIZING_FIXED(22), CLAY_SIZING_FIXED(22)},
                                                             .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                         .backgroundColor = cPlus,
                                                         .cornerRadius = CLAY_CORNER_RADIUS(3)})
                        {
                            bool n = Clay_Hovered();
                            sndRollover(n, hovCloudPlus[ci]);
                            sndClick(n);
                            hovCloudPlus[ci] = n;
                            if (hovCloudPlus[ci] && inp.lmbPressed)
                                *cs.val = glm::clamp(*cs.val + cs.step, cs.vmin, cs.vmax);
                            CLAY_TEXT(CLAY_STRING("+"), CLAY_TEXT_CONFIG({.textColor = Pal::btnLabel, .fontSize = fs(12)}));
                        }
                    }
                }

                // ── Attributions ──────────────────────────────────────────────
                CLAY(CLAY_ID("AttrGap"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(8)}}}) {}
                CLAY(CLAY_ID("AttrHdr"), {.layout = {
                                              .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(22)},
                                              .padding = {6, 6, 4, 4},
                                              .childAlignment = {.y = CLAY_ALIGN_Y_CENTER}},
                                          .backgroundColor = Pal::sectionHdr})
                {
                    CLAY_TEXT(CLAY_STRING("Attributions"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textSection, .fontSize = fs(14)}));
                }
                CLAY(CLAY_ID("AttrSep"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                     .padding = {0, 0, 4, 4}},
                                          .backgroundColor = Pal::sectionHdr}) {}

                // Row: constellation data source
                CLAY(CLAY_ID("Attr0"), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .padding = {6, 6, 5, 5},
                                            .childGap = 4,
                                            .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    CLAY_TEXT(CLAY_STRING("Satellite constellation data"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
                    CLAY_TEXT(CLAY_STRING("planet4589.org/space/con/conlist.html"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
                }

                CLAY(CLAY_ID("AttrDiv0"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                      .padding = {0, 0, 2, 2}},
                                           .backgroundColor = {30, 30, 32, 255}}) {}

                // Row: lens flare shader
                CLAY(CLAY_ID("Attr1"), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .padding = {6, 6, 5, 5},
                                            .childGap = 4,
                                            .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    CLAY_TEXT(CLAY_STRING("Lens Flare shader (modified)"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
                    CLAY_TEXT(CLAY_STRING("\"Lens Flare Example\" by peterekepeter"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
                    CLAY_TEXT(CLAY_STRING("shadertoy.com/view/4sX3Rs"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
                }

                CLAY(CLAY_ID("AttrDiv1"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIXED(1)},
                                                      .padding = {0, 0, 2, 2}},
                                           .backgroundColor = {30, 30, 32, 255}}) {}

                CLAY(CLAY_ID("Attr2"), {.layout = {
                                            .sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                            .padding = {6, 6, 5, 5},
                                            .childGap = 4,
                                            .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    CLAY_TEXT(CLAY_STRING("Icons"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));
                    CLAY_TEXT(CLAY_STRING("\"HackerNoon's Pixel Icon Library"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textDim, .fontSize = fs(11)}));
                    CLAY_TEXT(CLAY_STRING("https://github.com/hackernoon/pixel-icon-library"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textHint, .fontSize = fs(11)}));
                }
            }
        }
    }

    // ── Mouse capture rects ───────────────────────────────────────────────────
    ui.addMouseCaptureRect(12, inp.screenH - 110.0f, 340, 100); // time panel (bottom-left)
    ui.addMouseCaptureRect(inp.screenW - 460.0f, inp.screenH - 52.0f,
                           450.0f, 44.0f); // status bar (bottom-right)
    if (settingsOpen)
        ui.addMouseCaptureRect(settingsWinX, settingsWinY, 500.0f, 500.0f); // settings window

    // ── Cinematic intro overlay ───────────────────────────────────────────────
    if (showIntro)
    {
        if (inp.lmbPressed || inp.rmbPressed)
            showIntro = false;
        ui.addMouseCaptureRect(0, 0, inp.screenW, inp.screenH);

        CLAY(CLAY_ID("IntroOverlay"), {.layout = {
                                           .sizing = {CLAY_SIZING_FIXED((float)inp.screenW),
                                                      CLAY_SIZING_FIXED((float)inp.screenH)},
                                           .childAlignment = {.x = CLAY_ALIGN_X_CENTER,
                                                              .y = CLAY_ALIGN_Y_CENTER},
                                           .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                       .backgroundColor = {0, 0, 0, 185},
                                       .floating = {.zIndex = 30, .attachTo = CLAY_ATTACH_TO_ROOT}})
        {
            CLAY(CLAY_ID("IntroPanel"), {.layout = {
                                             .sizing = {CLAY_SIZING_FIXED(660),
                                                        CLAY_SIZING_FIT(0)},
                                             .childGap = 0,
                                             .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                // ── Title ─────────────────────────────────────────────────────
                CLAY_TEXT(CLAY_STRING("SAT LIGHT SIM"),
                          CLAY_TEXT_CONFIG({.textColor = {255, 255, 255, 255},
                                            .fontSize = fs(34)}));
                CLAY_TEXT(CLAY_STRING("by papereater"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(12)}));

                CLAY(CLAY_ID("IP1"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1),
                                                            CLAY_SIZING_FIXED(22)}}}) {}

                // ── Body ──────────────────────────────────────────────────────
                CLAY(CLAY_ID("IntroBody"), {.layout = {
                                                .sizing = {CLAY_SIZING_FIXED(660), CLAY_SIZING_FIT(0)},
                                                .childGap = 14,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {

                    CLAY_TEXT(CLAY_STRING("Welcome to the near future! Every planned major space constellation has been constructed. This simulation aims to realistically model what these will look like from the ground."),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
                    CLAY_TEXT(CLAY_STRING("Perpetual sunshine lies in sun synchronous orbit, following the terminator line of the Earth. This has become competitive real estate for football field-sized space datacenters and mirror reflectors"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
                    CLAY_TEXT(CLAY_STRING(
                                  "Whether or not they are profitable, useful, or even still functional, "
                                  "they are going to be up there for a very long time."),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
                    CLAY_TEXT(CLAY_STRING("We will come to miss the quiet sky."),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textPrimary, .fontSize = fs(18)}));
                }

                CLAY(CLAY_ID("IP2"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1),
                                                            CLAY_SIZING_FIXED(48)}}}) {}

                // ── Controls ──────────────────────────────────────────────────
                CLAY(CLAY_ID("IntroControls"), {.layout = {
                                                    .sizing = {CLAY_SIZING_FIXED(660),
                                                               CLAY_SIZING_FIT(0)},
                                                    .childGap = 7,
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                {
                    CLAY_TEXT(CLAY_STRING("WASD  =  Move"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                                .fontSize = fs(13)}));
                    CLAY_TEXT(CLAY_STRING("Right click  =  Look"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                                .fontSize = fs(13)}));
                    CLAY_TEXT(CLAY_STRING("Shift  =  Boost"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                                .fontSize = fs(13)}));
                    CLAY_TEXT(CLAY_STRING("Space  =  Play / Pause"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                                .fontSize = fs(13)}));
                    CLAY_TEXT(CLAY_STRING("Comma  =  Slow down time"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                                .fontSize = fs(13)}));
                    CLAY_TEXT(CLAY_STRING("Period  =  Speed up time"),
                              CLAY_TEXT_CONFIG({.textColor = Pal::textSection,
                                                .fontSize = fs(13)}));
                }

                CLAY(CLAY_ID("IP3"), {.layout = {.sizing = {CLAY_SIZING_FIXED(1),
                                                            CLAY_SIZING_FIXED(32)}}}) {}

                // ── Dismiss hint ──────────────────────────────────────────────
                CLAY_TEXT(CLAY_STRING("Click or press any key to continue"),
                          CLAY_TEXT_CONFIG({.textColor = Pal::textHint,
                                            .fontSize = fs(11)}));
            }
        }
    }
}

// ─── setAudio ─────────────────────────────────────────────────────────────────
// Called by App after both sim and audio are initialised.
// Configures the music playlist and stores the pointer for buildUI UI sounds.
void SatelliteSim::setAudio(AudioSystem *audio)
{
    audio_ = audio;
    if (!audio_)
        return;

    audio_->addTrack("assets/sound/music/gravity_wave.mp3");
    audio_->addTrack("assets/sound/music/fuse.mp3");
    audio_->startMusic();
    // Apply any volumes loaded from settings.json before the audio system was ready.
    audio_->setMasterVolume(masterVol_);
    audio_->setMusicVolume(musicVol_);
    audio_->setSfxVolume(sfxVol_);
}

// ─── cleanup ──────────────────────────────────────────────────────────────────
void SatelliteSim::cleanup(VkDevice device)
{
    saveSettings();
    // ── Orbit pipeline ─────────────────────────────────────────────────────────
    vkDestroyPipeline(device, orbitPipeline, nullptr);
    vkDestroyPipelineLayout(device, orbitPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, orbitDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, orbitDescLayout, nullptr);
    // ── Flare + draw + sky pipelines ───────────────────────────────────────────
    vkDestroyPipeline(device, compPipeline, nullptr);
    vkDestroyPipeline(device, skyBgPipeline, nullptr);
    vkDestroyPipeline(device, drawPipeline, nullptr);
    vkDestroyPipelineLayout(device, compPipeLayout, nullptr);
    vkDestroyPipelineLayout(device, skyBgPipeLayout, nullptr);
    vkDestroyPipelineLayout(device, drawPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, descPool, nullptr);
    vkDestroyDescriptorSetLayout(device, descLayout, nullptr);
    vkDestroyDescriptorPool(device, skyDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, skyDescLayout, nullptr);
    vkUnmapMemory(device, glowMem);
    if (noiseSampler)
    {
        vkDestroySampler(device, noiseSampler, nullptr);
        noiseSampler = VK_NULL_HANDLE;
    }
    if (noiseTexView)
    {
        vkDestroyImageView(device, noiseTexView, nullptr);
        noiseTexView = VK_NULL_HANDLE;
    }
    if (noiseTex)
    {
        vkDestroyImage(device, noiseTex, nullptr);
        noiseTex = VK_NULL_HANDLE;
    }
    if (noiseTexMem)
    {
        vkFreeMemory(device, noiseTexMem, nullptr);
        noiseTexMem = VK_NULL_HANDLE;
    }
    if (moonSampler)
    {
        vkDestroySampler(device, moonSampler, nullptr);
        moonSampler = VK_NULL_HANDLE;
    }
    if (moonTexView)
    {
        vkDestroyImageView(device, moonTexView, nullptr);
        moonTexView = VK_NULL_HANDLE;
    }
    if (moonTex)
    {
        vkDestroyImage(device, moonTex, nullptr);
        moonTex = VK_NULL_HANDLE;
    }
    if (moonTexMem)
    {
        vkFreeMemory(device, moonTexMem, nullptr);
        moonTexMem = VK_NULL_HANDLE;
    }
    if (earthDaySampler)
    {
        vkDestroySampler(device, earthDaySampler, nullptr);
        earthDaySampler = VK_NULL_HANDLE;
    }
    if (earthDayView)
    {
        vkDestroyImageView(device, earthDayView, nullptr);
        earthDayView = VK_NULL_HANDLE;
    }
    if (earthDayImg)
    {
        vkDestroyImage(device, earthDayImg, nullptr);
        earthDayImg = VK_NULL_HANDLE;
    }
    if (earthDayMem)
    {
        vkFreeMemory(device, earthDayMem, nullptr);
        earthDayMem = VK_NULL_HANDLE;
    }
    if (earthNightSampler)
    {
        vkDestroySampler(device, earthNightSampler, nullptr);
        earthNightSampler = VK_NULL_HANDLE;
    }
    if (earthNightView)
    {
        vkDestroyImageView(device, earthNightView, nullptr);
        earthNightView = VK_NULL_HANDLE;
    }
    if (earthNightImg)
    {
        vkDestroyImage(device, earthNightImg, nullptr);
        earthNightImg = VK_NULL_HANDLE;
    }
    if (earthNightMem)
    {
        vkFreeMemory(device, earthNightMem, nullptr);
        earthNightMem = VK_NULL_HANDLE;
    }
    if (earthElevSampler)
    {
        vkDestroySampler(device, earthElevSampler, nullptr);
        earthElevSampler = VK_NULL_HANDLE;
    }
    if (earthElevView)
    {
        vkDestroyImageView(device, earthElevView, nullptr);
        earthElevView = VK_NULL_HANDLE;
    }
    if (earthElevImg)
    {
        vkDestroyImage(device, earthElevImg, nullptr);
        earthElevImg = VK_NULL_HANDLE;
    }
    if (earthElevMem)
    {
        vkFreeMemory(device, earthElevMem, nullptr);
        earthElevMem = VK_NULL_HANDLE;
    }
    if (earthSpecSampler)
    {
        vkDestroySampler(device, earthSpecSampler, nullptr);
        earthSpecSampler = VK_NULL_HANDLE;
    }
    if (earthSpecView)
    {
        vkDestroyImageView(device, earthSpecView, nullptr);
        earthSpecView = VK_NULL_HANDLE;
    }
    if (earthSpecImg)
    {
        vkDestroyImage(device, earthSpecImg, nullptr);
        earthSpecImg = VK_NULL_HANDLE;
    }
    if (earthSpecMem)
    {
        vkFreeMemory(device, earthSpecMem, nullptr);
        earthSpecMem = VK_NULL_HANDLE;
    }
    if (earthCloudsSampler)
    {
        vkDestroySampler(device, earthCloudsSampler, nullptr);
        earthCloudsSampler = VK_NULL_HANDLE;
    }
    if (earthCloudsView)
    {
        vkDestroyImageView(device, earthCloudsView, nullptr);
        earthCloudsView = VK_NULL_HANDLE;
    }
    if (earthCloudsImg)
    {
        vkDestroyImage(device, earthCloudsImg, nullptr);
        earthCloudsImg = VK_NULL_HANDLE;
    }
    if (earthCloudsMem)
    {
        vkFreeMemory(device, earthCloudsMem, nullptr);
        earthCloudsMem = VK_NULL_HANDLE;
    }
    if (cloudParamsBuf)
    {
        vkDestroyBuffer(device, cloudParamsBuf, nullptr);
        cloudParamsBuf = VK_NULL_HANDLE;
    }
    if (cloudParamsMem)
    {
        vkFreeMemory(device, cloudParamsMem, nullptr);
        cloudParamsMem = VK_NULL_HANDLE;
    }
    vkDestroyBuffer(device, glowBuf, nullptr);
    vkFreeMemory(device, glowMem, nullptr);
    // satInputBuf is now device-local (no host mapping to release).
    vkDestroyBuffer(device, satInputBuf, nullptr);
    vkFreeMemory(device, satInputMem, nullptr);
    vkDestroyBuffer(device, satVisibleBuf, nullptr);
    vkFreeMemory(device, satVisibleMem, nullptr);
    vkDestroyBuffer(device, satOrbitBuf, nullptr);
    vkFreeMemory(device, satOrbitMem, nullptr);
    vkDestroyBuffer(device, mirrorNormalsBuf, nullptr);
    vkFreeMemory(device, mirrorNormalsMem, nullptr);
    if (reflectorTargetsMapped)
        vkUnmapMemory(device, reflectorTargetsMem);
    vkDestroyBuffer(device, reflectorTargetsBuf, nullptr);
    vkFreeMemory(device, reflectorTargetsMem, nullptr);

    vkDestroyPipeline(device, starPipeline, nullptr);
    vkDestroyPipelineLayout(device, starPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, starDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, starDescLayout, nullptr);
    if (starMapped)
        vkUnmapMemory(device, starMem);
    vkDestroyBuffer(device, starBuf, nullptr);
    vkFreeMemory(device, starMem, nullptr);

    if (win)
        glfwSetInputMode(win, GLFW_CURSOR, GLFW_CURSOR_NORMAL);
}

// ─── onKey ────────────────────────────────────────────────────────────────────
void SatelliteSim::onKey(GLFWwindow *w, int key, int action)
{
    win = w;
    if (action != GLFW_PRESS)
        return;

    if (showIntro)
    {
        showIntro = false;
        return;
    }

    // If any binding is listening, capture this key press and assign it.
    for (auto &kb : keybindings)
    {
        if (!kb.listening)
            continue;
        if (key == GLFW_KEY_ESCAPE)
        {
            kb.listening = false; // cancel rebind
        }
        else
        {
            kb.key = key;
            kb.listening = false;
        }
        return; // consume the key event
    }

    // Dispatch via keybindings array
    auto pressed = [&](int bindIdx)
    {
        return bindIdx < (int)keybindings.size() && key == keybindings[bindIdx].key;
    };

    if (pressed(KB_TOGGLE_UI))
        uiVisible = !uiVisible;
    if (pressed(KB_PAUSE))
        timePaused = !timePaused;
    if (pressed(KB_SLOWER))
        timeScaleIdx = std::max(0, timeScaleIdx - 1);
    if (pressed(KB_FASTER))
        timeScaleIdx = std::min(kNumTimeScales - 1, timeScaleIdx + 1);
    if (pressed(KB_REVERSE))
        timeDir = -timeDir;
    if (pressed(KB_CINEMATIC) && camera.captured)
        cinematicMode = !cinematicMode;
    if (pressed(KB_RESET_ELEV))
        obsHeightOffset = 0.0f;
    // KB_MOVE_BOOST, KB_MOVE_FINE, KB_RAISE_ELEV, KB_LOWER_ELEV are held keys — polled in recordCompute.

    // F11: toggle fullscreen
    if (key == GLFW_KEY_F11)
    {
        bool isFs = glfwGetWindowMonitor(win) != nullptr;
        if (!isFs)
        {
            glfwGetWindowPos(win, &windowedX, &windowedY);
            glfwGetWindowSize(win, &windowedW, &windowedH);
            GLFWmonitor *mon = glfwGetPrimaryMonitor();
            const GLFWvidmode *mode = glfwGetVideoMode(mon);
            glfwSetWindowMonitor(win, mon, 0, 0, mode->width, mode->height, mode->refreshRate);
        }
        else
        {
            glfwSetWindowMonitor(win, nullptr, windowedX, windowedY, windowedW, windowedH, 0);
        }
    }
}

// ─── onCursorPos ──────────────────────────────────────────────────────────────
void SatelliteSim::onCursorPos(GLFWwindow *w, double x, double y)
{
    win = w;
    if (firstMouse)
    {
        prevX = x;
        prevY = y;
        firstMouse = false;
    }
    dmx += (float)(x - prevX);
    dmy += (float)(y - prevY);
    prevX = x;
    prevY = y;
}

// ─── createBuffers ────────────────────────────────────────────────────────────
void SatelliteSim::createBuffers(VulkanContext &ctx)
{
    // satInputBuf: device-local. sat_orbit.comp writes each frame; sat_flare.comp reads.
    ctx.createBuffer(sizeof(GpuSatInput) * MAX_SATELLITES,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     satInputBuf, satInputMem);

    // satVisibleBuf: device-local. sat_flare.comp writes, vertex reads.
    ctx.createBuffer(sizeof(GpuSatVisible) * MAX_SATELLITES,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     satVisibleBuf, satVisibleMem);

    // satOrbitBuf: device-local, uploaded once. sat_orbit.comp reads every frame.
    ctx.createBuffer(sizeof(GpuSatOrbit) * MAX_SATELLITES,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     satOrbitBuf, satOrbitMem);

    // mirrorNormalsBuf: device-local, read-write each frame by sat_orbit.comp.
    // Zero-initialised so w=0 triggers the snap-to-target path on first invocation.
    ctx.createBuffer(sizeof(glm::vec4) * MAX_SATELLITES,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     mirrorNormalsBuf, mirrorNormalsMem);
    {
        VkCommandBuffer cmd = ctx.beginOneTimeCommands();
        vkCmdFillBuffer(cmd, mirrorNormalsBuf, 0, VK_WHOLE_SIZE, 0);
        ctx.endOneTimeCommands(cmd);
    }

    // reflectorTargetsBuf: host-visible + coherent, updated every frame by CPU.
    VkDeviceSize reflSize = sizeof(GpuReflectorTarget) * kNumReflectorTargets;
    ctx.createBuffer(reflSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     reflectorTargetsBuf, reflectorTargetsMem);
    vkMapMemory(ctx.device, reflectorTargetsMem, 0, reflSize, 0, &reflectorTargetsMapped);
    memset(reflectorTargetsMapped, 0, reflSize);
}

// ─── createDescriptors ────────────────────────────────────────────────────────
void SatelliteSim::createDescriptors(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // glowBuf: atomic writes from flare shader

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &descLayout);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 3};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &descPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = descPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &descLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &descSet);

    VkDescriptorBufferInfo inpInfo{satInputBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo visInfo{satVisibleBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo glowInfo{glowBuf, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[3] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &inpInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &visInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &glowInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 3, writes, 0, nullptr);
}

// ─── createComputePipeline ────────────────────────────────────────────────────
void SatelliteSim::createComputePipeline(VulkanContext &ctx)
{
    VkShaderModule mod = ctx.loadShader("shaders/sat_flare.comp.spv");

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SatFlarePC)};

    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &descLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &li, nullptr, &compPipeLayout);

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = compPipeLayout;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &compPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create compute pipeline");

    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

// ─── createOrbitDescriptors ───────────────────────────────────────────────────
// Descriptor set for sat_orbit.comp:
//   binding 0  satOrbitBuf       (readonly  SSBO)
//   binding 1  satInputBuf       (write     SSBO — same buffer that sat_flare.comp reads)
//   binding 2  mirrorNormalsBuf  (readwrite SSBO)
//   binding 3  reflectorTargetsBuf (readonly SSBO)
void SatelliteSim::createOrbitDescriptors(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding bindings[4] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 4;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &orbitDescLayout);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &orbitDescPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = orbitDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &orbitDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &orbitDescSet);

    VkDescriptorBufferInfo orbitInfo{satOrbitBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo inputInfo{satInputBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo mirrorInfo{mirrorNormalsBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo reflInfo{reflectorTargetsBuf, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[4] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &orbitInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &inputInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &mirrorInfo, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &reflInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 4, writes, 0, nullptr);
}

// ─── createOrbitPipeline ──────────────────────────────────────────────────────
void SatelliteSim::createOrbitPipeline(VulkanContext &ctx)
{
    VkShaderModule mod = ctx.loadShader("shaders/sat_orbit.comp.spv");

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SatOrbitPC)};

    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &orbitDescLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &li, nullptr, &orbitPipeLayout);

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = orbitPipeLayout;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &orbitPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create orbit compute pipeline");

    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

// ─── uploadSatOrbits ─────────────────────────────────────────────────────────
// Bakes GpuSatOrbit data from satOrbits+satTypes and uploads to satOrbitBuf.
// Stores orbitEpochDay/Sec = current simTime so deltaT resets to 0.
// Auto-called from recordCompute when |simDayJ2000-orbitEpochDay| >= kOrbitRebakeDays.
void SatelliteSim::uploadSatOrbits(VulkanContext &ctx)
{
    if (satOrbits.empty())
        return;

    orbitEpochDay = simDayJ2000;
    orbitEpochSec = simSecInDay;
    const double orbitEpochT0 = (double)orbitEpochDay * 86400.0 + orbitEpochSec;
    // SSO RAAN is anchored at sim-start, so bake only the precession since then.
    const double t_start = (double)simInitDayJ2000 * 86400.0 + simInitSecInDay;

    std::vector<GpuSatOrbit> gpuOrbits(activeSatCount);
    for (uint32_t ci = 0; ci < (uint32_t)constellations.size(); ++ci)
    {
        const ConstellationConfig &c = constellations[ci];
        for (uint32_t i = c.orbitStart; i < c.orbitStart + c.orbitCount; ++i)
        {
            if (i >= activeSatCount)
                break;
            const SatOrbit &src = satOrbits[i];
            const SatelliteType &type = satTypes[src.typeIdx];
            GpuSatOrbit &dst = gpuOrbits[i];
            dst.raan = src.alignTerminator
                           ? (float)fmod((double)src.raan + kSSOPrecRate * (orbitEpochT0 - t_start),
                                         glm::two_pi<double>())
                           : src.raan;
            dst.u0 = (float)fmod((double)src.u0 + (double)src.meanMot * orbitEpochT0,
                                 glm::two_pi<double>());
            dst.R_sat = src.R_sat;
            dst.meanMot = src.meanMot;
            dst.cosI = src.cosI;
            dst.sinI = src.sinI;
            dst.cosRaan = src.cosRaan;
            dst.sinRaan = src.sinRaan;

            dst.tumbleRate = src.tumbleRate;
            dst.tumblePhase = (float)fmod((double)src.tumblePhase +
                                              (double)src.tumbleRate * orbitEpochT0,
                                          glm::two_pi<double>());
            dst.alignTerminator = src.alignTerminator ? 1.0f : 0.0f;
            dst.tumbleAxisX = src.tumbleAxis.x;
            dst.tumbleAxisY = src.tumbleAxis.y;
            dst.tumbleAxisZ = src.tumbleAxis.z;

            dst.primaryAttitude = (uint32_t)type.primary.attitude;
            dst.secondaryAttitude = (uint32_t)type.secondary.attitude;

            dst.baseColorR = type.baseColor.r;
            dst.baseColorG = type.baseColor.g;
            dst.baseColorB = type.baseColor.b;
            dst.crossSection = sqrtf(type.crossSectionM2 / 10.0f);
            dst.specExp0 = type.primary.specExp;
            dst.specExp1 = type.secondary.specExp;
            dst.w1 = type.secondary.weight;
            dst.diffuse = type.diffuse;
            dst.mirrorFrac = type.mirrorFrac;
            dst.constIdx = src.constIdx;
            dst.pad0 = dst.pad1 = 0;
        }
    }

    VkDeviceSize bufSize = activeSatCount * sizeof(GpuSatOrbit);
    VkBuffer staging;
    VkDeviceMemory stagingMem;
    ctx.createBuffer(bufSize,
                     VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     staging, stagingMem);
    void *mapped;
    vkMapMemory(ctx.device, stagingMem, 0, bufSize, 0, &mapped);
    memcpy(mapped, gpuOrbits.data(), bufSize);
    vkUnmapMemory(ctx.device, stagingMem);

    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    VkBufferCopy region{0, 0, bufSize};
    vkCmdCopyBuffer(cmd, staging, satOrbitBuf, 1, &region);
    ctx.endOneTimeCommands(cmd);

    vkDestroyBuffer(ctx.device, staging, nullptr);
    vkFreeMemory(ctx.device, stagingMem, nullptr);
}

// ─── createSkyBgPipeline ──────────────────────────────────────────────────────
// ─── createGlowResources ──────────────────────────────────────────────────────
// Allocates the host-visible SSBO that holds up to kMaxGlows bright-flare entries,
// and creates the descriptor set used by the sky background pipeline to read it.
void SatelliteSim::createGlowResources(VulkanContext &ctx)
{
    // ── SSBO: top-N glow entries written every frame ──────────────────────────
    VkDeviceSize bufSize = sizeof(GpuGlowBuf);
    ctx.createBuffer(bufSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     glowBuf, glowMem);
    vkMapMemory(ctx.device, glowMem, 0, bufSize, 0, &glowMapped);
    memset(glowMapped, 0, bufSize);

    // ── Noise texture: RGBA PNG for lens-flare angular corona variation ────────
    // Loaded from assets/noise/rgba_noise.png (tiled REPEAT sampler).
    // The sky shader samples it at angular coordinates around each flare source
    // to produce the irregular spiky corona shape (see lensFlare() in sat_sky.frag).
    {
        int w = 0, h = 0, ch = 0;
        stbi_uc *pixels = stbi_load("assets/noise/rgba_noise.png", &w, &h, &ch, 4);
        if (!pixels)
            throw std::runtime_error("SatelliteSim: failed to load assets/noise/rgba_noise.png");

        VkDeviceSize imgBytes = (VkDeviceSize)w * h * 4;

        // Staging buffer
        VkBuffer stageBuf;
        VkDeviceMemory stageMem;
        ctx.createBuffer(imgBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stageBuf, stageMem);
        void *mapped;
        vkMapMemory(ctx.device, stageMem, 0, imgBytes, 0, &mapped);
        memcpy(mapped, pixels, (size_t)imgBytes);
        vkUnmapMemory(ctx.device, stageMem);
        stbi_image_free(pixels);

        // Device image
        ctx.createImage((uint32_t)w, (uint32_t)h,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        noiseTex, noiseTexMem);

        // Upload via one-time command
        {
            auto cmd = ctx.beginOneTimeCommands();
            ctx.imageBarrier(cmd, noiseTex,
                             0, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, noiseTex,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.imageBarrier(cmd, noiseTex,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ctx.endOneTimeCommands(cmd);
        }
        vkDestroyBuffer(ctx.device, stageBuf, nullptr);
        vkFreeMemory(ctx.device, stageMem, nullptr);

        // Image view
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = noiseTex;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &noiseTexView);

        // Sampler: REPEAT so the noise tiles seamlessly around the full angular range
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        vkCreateSampler(ctx.device, &sci, nullptr, &noiseSampler);
    }

    // ── Moon texture: near-side face disc image (binding 2) ──────────────────
    {
        int w = 0, h = 0, ch = 0;
        stbi_uc *pixels = stbi_load("assets/textures/full_moon.png", &w, &h, &ch, 4);
        if (!pixels)
            throw std::runtime_error("SatelliteSim: failed to load assets/textures/full_moon.png");

        VkDeviceSize imgBytes = (VkDeviceSize)w * h * 4;

        VkBuffer stageBuf;
        VkDeviceMemory stageMem;
        ctx.createBuffer(imgBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stageBuf, stageMem);
        void *mapped;
        vkMapMemory(ctx.device, stageMem, 0, imgBytes, 0, &mapped);
        memcpy(mapped, pixels, (size_t)imgBytes);
        vkUnmapMemory(ctx.device, stageMem);
        stbi_image_free(pixels);

        ctx.createImage((uint32_t)w, (uint32_t)h,
                        VK_FORMAT_R8G8B8A8_UNORM,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                        moonTex, moonTexMem);

        {
            auto cmd = ctx.beginOneTimeCommands();
            ctx.imageBarrier(cmd, moonTex,
                             0, VK_ACCESS_TRANSFER_WRITE_BIT,
                             VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                             VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, moonTex,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.imageBarrier(cmd, moonTex,
                             VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                             VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                             VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
            ctx.endOneTimeCommands(cmd);
        }
        vkDestroyBuffer(ctx.device, stageBuf, nullptr);
        vkFreeMemory(ctx.device, stageMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = moonTex;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &moonTexView);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        vkCreateSampler(ctx.device, &sci, nullptr, &moonSampler);
    }

    // ── Earth day texture (binding 3): 8K equirectangular colour map ─────────
    {
        int w = 0, h = 0, ch = 0;
        stbi_uc *pixels = stbi_load("assets/textures/8k_earth_daymap.jpg", &w, &h, &ch, 4);
        if (!pixels)
            throw std::runtime_error("SatelliteSim: failed to load assets/textures/8k_earth_daymap.jpg");

        earthDayMips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1;
        VkDeviceSize imgBytes = (VkDeviceSize)w * h * 4;

        VkBuffer stageBuf;
        VkDeviceMemory stageMem;
        ctx.createBuffer(imgBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stageBuf, stageMem);
        void *mapped;
        vkMapMemory(ctx.device, stageMem, 0, imgBytes, 0, &mapped);
        memcpy(mapped, pixels, (size_t)imgBytes);
        vkUnmapMemory(ctx.device, stageMem);
        stbi_image_free(pixels);

        ctx.createImage((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_SRGB,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        earthDayImg, earthDayMem, earthDayMips);

        {
            auto cmd = ctx.beginOneTimeCommands();
            // Transition ALL mips to TRANSFER_DST_OPTIMAL for upload + blit
            VkImageMemoryBarrier allMips{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            allMips.srcAccessMask = 0;
            allMips.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            allMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            allMips.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            allMips.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.image = earthDayImg;
            allMips.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthDayMips, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &allMips);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, earthDayImg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.generateMipmaps(cmd, earthDayImg, VK_FORMAT_R8G8B8A8_SRGB,
                                (uint32_t)w, (uint32_t)h, earthDayMips);
            ctx.endOneTimeCommands(cmd);
        }
        vkDestroyBuffer(ctx.device, stageBuf, nullptr);
        vkFreeMemory(ctx.device, stageMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = earthDayImg;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_SRGB;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthDayMips, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &earthDayView);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = (float)earthDayMips;
        vkCreateSampler(ctx.device, &sci, nullptr, &earthDaySampler);
    }

    // ── Earth night texture (binding 4): 8K equirectangular night-lights map ─
    {
        int w = 0, h = 0, ch = 0;
        stbi_uc *pixels = stbi_load("assets/textures/8k_earth_nightmap.jpg", &w, &h, &ch, 4);
        if (!pixels)
            throw std::runtime_error("SatelliteSim: failed to load assets/textures/8k_earth_nightmap.jpg");

        earthNightMips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1;
        VkDeviceSize imgBytes = (VkDeviceSize)w * h * 4;

        VkBuffer stageBuf;
        VkDeviceMemory stageMem;
        ctx.createBuffer(imgBytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         stageBuf, stageMem);
        void *mapped;
        vkMapMemory(ctx.device, stageMem, 0, imgBytes, 0, &mapped);
        memcpy(mapped, pixels, (size_t)imgBytes);
        vkUnmapMemory(ctx.device, stageMem);
        stbi_image_free(pixels);

        ctx.createImage((uint32_t)w, (uint32_t)h, VK_FORMAT_R8G8B8A8_SRGB,
                        VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                            VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                        earthNightImg, earthNightMem, earthNightMips);

        {
            auto cmd = ctx.beginOneTimeCommands();
            VkImageMemoryBarrier allMips{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            allMips.srcAccessMask = 0;
            allMips.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            allMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            allMips.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            allMips.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.image = earthNightImg;
            allMips.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthNightMips, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &allMips);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, earthNightImg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.generateMipmaps(cmd, earthNightImg, VK_FORMAT_R8G8B8A8_SRGB,
                                (uint32_t)w, (uint32_t)h, earthNightMips);
            ctx.endOneTimeCommands(cmd);
        }
        vkDestroyBuffer(ctx.device, stageBuf, nullptr);
        vkFreeMemory(ctx.device, stageMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = earthNightImg;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_SRGB;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthNightMips, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &earthNightView);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = (float)earthNightMips;
        vkCreateSampler(ctx.device, &sci, nullptr, &earthNightSampler);
    }

    // ── Load earth elevation map (binding 5): 21600×10800 R8_UNORM , 0 = sea level
    {
        int w, h, ch;
        unsigned char *pixels = stbi_load("assets/textures/earth_elevation.png", &w, &h, &ch, 1);
        if (pixels)
        {
            earthElevMips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1;
            VkDeviceSize imgBytes = (VkDeviceSize)w * h * 1;

            VkBuffer stageBuf;
            VkDeviceMemory stageMem;
            ctx.createBuffer(imgBytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stageBuf, stageMem);
            void *mapped;
            vkMapMemory(ctx.device, stageMem, 0, imgBytes, 0, &mapped);
            memcpy(mapped, pixels, imgBytes);
            vkUnmapMemory(ctx.device, stageMem);

            // Downsample to 2160×1080 (10:1 each axis, ~18 km/pixel) for CPU observer height
            earthElevCpuW = 2160;
            earthElevCpuH = 1080;
            earthElevCpu.resize((size_t)earthElevCpuW * earthElevCpuH);
            for (int cy = 0; cy < earthElevCpuH; ++cy)
            {
                for (int cx = 0; cx < earthElevCpuW; ++cx)
                {
                    int sx = std::min(cx * w / earthElevCpuW, w - 1);
                    int sy = std::min(cy * h / earthElevCpuH, h - 1);
                    earthElevCpu[cy * earthElevCpuW + cx] = pixels[sy * w + sx];
                }
            }
            stbi_image_free(pixels);

            ctx.createImage((uint32_t)w, (uint32_t)h,
                            VK_FORMAT_R8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            earthElevImg, earthElevMem, earthElevMips);

            VkCommandBuffer cmd = ctx.beginOneTimeCommands();
            VkImageMemoryBarrier allMips{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            allMips.srcAccessMask = 0;
            allMips.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            allMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            allMips.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            allMips.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.image = earthElevImg;
            allMips.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthElevMips, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &allMips);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, earthElevImg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.generateMipmaps(cmd, earthElevImg, VK_FORMAT_R8_UNORM,
                                (uint32_t)w, (uint32_t)h, earthElevMips);
            ctx.endOneTimeCommands(cmd);
            vkDestroyBuffer(ctx.device, stageBuf, nullptr);
            vkFreeMemory(ctx.device, stageMem, nullptr);

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = earthElevImg;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthElevMips, 0, 1};
            vkCreateImageView(ctx.device, &vci, nullptr, &earthElevView);

            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = (float)earthElevMips;
            vkCreateSampler(ctx.device, &sci, nullptr, &earthElevSampler);
        }
        else
        {
            fprintf(stderr, "Warning: could not load earth_elevation terrain march disabled\n");
        }
    }

    // ── Load earth specular map (binding 6): 8K R8_UNORM ocean mask ──────────────
    {
        int w, h, ch;
        unsigned char *pixels = stbi_load("assets/textures/8k_earth_specular_map.png", &w, &h, &ch, 1);
        if (pixels)
        {
            earthSpecMips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1;
            VkDeviceSize imgBytes = (VkDeviceSize)w * h;

            VkBuffer stageBuf;
            VkDeviceMemory stageMem;
            ctx.createBuffer(imgBytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stageBuf, stageMem);
            void *mapped;
            vkMapMemory(ctx.device, stageMem, 0, imgBytes, 0, &mapped);
            memcpy(mapped, pixels, (size_t)imgBytes);
            vkUnmapMemory(ctx.device, stageMem);
            stbi_image_free(pixels);

            ctx.createImage((uint32_t)w, (uint32_t)h,
                            VK_FORMAT_R8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            earthSpecImg, earthSpecMem, earthSpecMips);

            VkCommandBuffer cmd = ctx.beginOneTimeCommands();
            VkImageMemoryBarrier allMips{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            allMips.srcAccessMask = 0;
            allMips.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            allMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            allMips.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            allMips.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.image = earthSpecImg;
            allMips.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthSpecMips, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &allMips);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, earthSpecImg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.generateMipmaps(cmd, earthSpecImg, VK_FORMAT_R8_UNORM,
                                (uint32_t)w, (uint32_t)h, earthSpecMips);
            ctx.endOneTimeCommands(cmd);
            vkDestroyBuffer(ctx.device, stageBuf, nullptr);
            vkFreeMemory(ctx.device, stageMem, nullptr);

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = earthSpecImg;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthSpecMips, 0, 1};
            vkCreateImageView(ctx.device, &vci, nullptr, &earthSpecView);

            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = (float)earthSpecMips;
            vkCreateSampler(ctx.device, &sci, nullptr, &earthSpecSampler);
        }
        else
        {
            fprintf(stderr, "Warning: could not load 8k_earth_specular_map.png; ocean shader disabled\n");
        }
    }

    // ── Load earth cloud map (binding 7): 8K R8_UNORM grayscale coverage ─────────
    {
        int w, h, ch;
        unsigned char *pixels = stbi_load("assets/textures/8k_earth_clouds.jpg", &w, &h, &ch, 1);
        if (pixels)
        {
            earthCloudsMips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1;
            VkDeviceSize imgBytes = (VkDeviceSize)w * h;

            VkBuffer stageBuf;
            VkDeviceMemory stageMem;
            ctx.createBuffer(imgBytes,
                             VK_BUFFER_USAGE_TRANSFER_SRC_BIT,
                             VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                             stageBuf, stageMem);
            void *mapped;
            vkMapMemory(ctx.device, stageMem, 0, imgBytes, 0, &mapped);
            memcpy(mapped, pixels, (size_t)imgBytes);
            vkUnmapMemory(ctx.device, stageMem);
            stbi_image_free(pixels);

            ctx.createImage((uint32_t)w, (uint32_t)h,
                            VK_FORMAT_R8_UNORM,
                            VK_IMAGE_USAGE_SAMPLED_BIT |
                                VK_IMAGE_USAGE_TRANSFER_DST_BIT |
                                VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                            earthCloudsImg, earthCloudsMem, earthCloudsMips);

            VkCommandBuffer cmd = ctx.beginOneTimeCommands();
            VkImageMemoryBarrier allMips{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            allMips.srcAccessMask = 0;
            allMips.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            allMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            allMips.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            allMips.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.image = earthCloudsImg;
            allMips.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthCloudsMips, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &allMips);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, earthCloudsImg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.generateMipmaps(cmd, earthCloudsImg, VK_FORMAT_R8_UNORM,
                                (uint32_t)w, (uint32_t)h, earthCloudsMips);
            ctx.endOneTimeCommands(cmd);
            vkDestroyBuffer(ctx.device, stageBuf, nullptr);
            vkFreeMemory(ctx.device, stageMem, nullptr);

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = earthCloudsImg;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8_UNORM;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, earthCloudsMips, 0, 1};
            vkCreateImageView(ctx.device, &vci, nullptr, &earthCloudsView);

            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
            sci.maxLod = (float)earthCloudsMips;
            vkCreateSampler(ctx.device, &sci, nullptr, &earthCloudsSampler);
        }
        else
        {
            fprintf(stderr, "Warning: could not load 8k_earth_clouds.jpg; cloud map disabled\n");
        }
    }

    // ── Cloud params UBO (binding 9): host-visible, persistently mapped ──────────
    {
        VkDeviceSize sz = sizeof(GpuCloudParams);
        ctx.createBuffer(sz,
                         VK_BUFFER_USAGE_UNIFORM_BUFFER_BIT,
                         VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                         cloudParamsBuf, cloudParamsMem);
        vkMapMemory(ctx.device, cloudParamsMem, 0, sz, 0, &cloudParamsMapped);
        memset(cloudParamsMapped, 0, sz);
    }

    // ── Descriptor set layout: 0=GlowBuf, 1=noise, 2=moon, 3=earthDay, 4=earthNight, 5=earthElev, 6=earthSpec, 7=earthClouds, 9=CloudParams UBO
    VkDescriptorSetLayoutBinding bindings[9] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[8] = {9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER,         1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 9;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &skyDescLayout);

    VkDescriptorPoolSize ps[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 7},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
    };
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &skyDescPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = skyDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &skyDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &skyDescSet);

    // Use noise texture as 1×1 placeholder if optional textures failed to load
    VkSampler elevSamplerFinal = earthElevSampler ? earthElevSampler : noiseSampler;
    VkImageView elevViewFinal = earthElevView ? earthElevView : noiseTexView;
    VkSampler specSamplerFinal = earthSpecSampler ? earthSpecSampler : noiseSampler;
    VkImageView specViewFinal = earthSpecView ? earthSpecView : noiseTexView;
    VkSampler cloudsSamplerFinal = earthCloudsSampler ? earthCloudsSampler : noiseSampler;
    VkImageView cloudsViewFinal = earthCloudsView ? earthCloudsView : noiseTexView;

    VkDescriptorBufferInfo bufInfo{glowBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo noiseImgInfo{noiseSampler, noiseTexView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo moonImgInfo{moonSampler, moonTexView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo dayImgInfo{earthDaySampler, earthDayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo nightImgInfo{earthNightSampler, earthNightView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo elevImgInfo{elevSamplerFinal, elevViewFinal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo specImgInfo{specSamplerFinal, specViewFinal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cloudsImgInfo{cloudsSamplerFinal, cloudsViewFinal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo cloudParamsInfo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};

    VkWriteDescriptorSet writes[9] = {};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = skyDescSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[0].pBufferInfo = &bufInfo;
    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = skyDescSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[1].pImageInfo = &noiseImgInfo;
    writes[2].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[2].dstSet = skyDescSet;
    writes[2].dstBinding = 2;
    writes[2].descriptorCount = 1;
    writes[2].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[2].pImageInfo = &moonImgInfo;
    writes[3].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[3].dstSet = skyDescSet;
    writes[3].dstBinding = 3;
    writes[3].descriptorCount = 1;
    writes[3].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[3].pImageInfo = &dayImgInfo;
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = skyDescSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[4].pImageInfo = &nightImgInfo;
    writes[5].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[5].dstSet = skyDescSet;
    writes[5].dstBinding = 5;
    writes[5].descriptorCount = 1;
    writes[5].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[5].pImageInfo = &elevImgInfo;
    writes[6].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[6].dstSet = skyDescSet;
    writes[6].dstBinding = 6;
    writes[6].descriptorCount = 1;
    writes[6].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[6].pImageInfo = &specImgInfo;
    writes[7].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[7].dstSet = skyDescSet;
    writes[7].dstBinding = 7;
    writes[7].descriptorCount = 1;
    writes[7].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[7].pImageInfo = &cloudsImgInfo;
    writes[8].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[8].dstSet = skyDescSet;
    writes[8].dstBinding = 9;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[8].pBufferInfo = &cloudParamsInfo;
    vkUpdateDescriptorSets(ctx.device, 9, writes, 0, nullptr);
}

// Fullscreen triangle that colors pixels sky or ground based on camera elevation.
// Uses same push constant layout as the satellite draw pass (SatDrawPC).
void SatelliteSim::createSkyBgPipeline(VulkanContext &ctx)
{
    VkShaderModule vert = ctx.loadShader("shaders/sat_sky.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/sat_sky.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_TRIANGLE_LIST;

    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Write terrain depth so satellite/star passes can test against it with LESS.
    // ALWAYS compare op so the sky background always wins (it's the first pass).
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_TRUE;
    ds.depthCompareOp = VK_COMPARE_OP_ALWAYS;

    // Opaque: simply overwrite what the clear left.
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    if (skyBgPipeLayout == VK_NULL_HANDLE)
    {
        // Fragment stage needs push constants too (sun disc reads sunDirENU).
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                                0, sizeof(SatDrawPC)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &skyDescLayout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(ctx.device, &li, nullptr, &skyBgPipeLayout);
    }

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rast;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.layout = skyBgPipeLayout;
    ci.renderPass = ctx.renderPass;
    ci.subpass = 0;

    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &skyBgPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create sky background pipeline");

    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

// ─── createDrawPipeline ───────────────────────────────────────────────────────
void SatelliteSim::createDrawPipeline(VulkanContext &ctx)
{
    VkShaderModule vert = ctx.loadShader("shaders/sat_point.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/sat_point.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Depth test against terrain written by the sky background pass (gl_FragDepth).
    // Satellites at fixed depth 0.5 fail LESS where terrain depth < 0.5 (close terrain hits).
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    // Additive blending.
    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    if (drawPipeLayout == VK_NULL_HANDLE)
    {
        VkPushConstantRange drawPcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SatDrawPC)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &descLayout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &drawPcr;
        vkCreatePipelineLayout(ctx.device, &li, nullptr, &drawPipeLayout);
    }

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rast;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.layout = drawPipeLayout;
    ci.renderPass = ctx.renderPass;
    ci.subpass = 0;

    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &drawPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create draw pipeline");

    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

// ─── initStars ────────────────────────────────────────────────────────────────
// Parses the embedded Yale BSC catalog, builds star records with ECI vectors,
// creates a host-visible GPU buffer, and sets up the star descriptor set + pipeline.
void SatelliteSim::initStars(VulkanContext &ctx)
{
    starRecords.clear();
    const int kCatSize = sizeof(kStarCatalog) / sizeof(kStarCatalog[0]);
    starRecords.reserve(kCatSize);

    for (int i = 0; i < kCatSize; ++i)
    {
        const auto &s = kStarCatalog[i];

        // RA/Dec (degrees, J2000) → ECI unit vector.
        // ECI is the J2000 equatorial frame: X toward vernal equinox, Z toward north pole.
        float ra = glm::radians(s.ra_deg);
        float dec = glm::radians(s.dec_deg);
        glm::vec3 eciDir{cosf(dec) * cosf(ra),
                         cosf(dec) * sinf(ra),
                         sinf(dec)};

        // Visual magnitude → intensity: mag 0 → 1.0; Sirius (−1.46) → ~3.84.
        float rawInt = glm::clamp(powf(10.0f, -s.vmag / 2.5f), 0.0f, 8.0f);

        // B-V colour index → approximate RGB (hot blue at low B-V, red at high B-V).
        float bv = s.bv;
        glm::vec3 col{glm::clamp(0.90f + 0.10f * bv, 0.60f, 1.0f),  // R
                      glm::clamp(1.00f - 0.15f * bv, 0.50f, 1.0f),  // G
                      glm::clamp(1.00f - 0.90f * bv, 0.10f, 1.0f)}; // B

        // Point sprite size: 1.5 px for faint stars, up to ~5.5 px for Sirius.
        float starScale = 4.0f; // tweak this to make stars bigger/smaller overall
        float angSize = 1.5f + glm::min(rawInt, 4.0f) * 1.0f;
        angSize *= starScale;

        starRecords.push_back({eciDir, rawInt, col, angSize});
    }
    starCount = (uint32_t)starRecords.size();

    // Host-visible buffer (tiny: ~287 × 32 bytes = ~9 KB).
    VkDeviceSize bufSize = starCount * sizeof(GpuSatVisible);
    ctx.createBuffer(bufSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     starBuf, starMem);
    vkMapMemory(ctx.device, starMem, 0, bufSize, 0, &starMapped);

    // Descriptor layout: only binding=1 (vertex shader reads GpuSatVisible).
    VkDescriptorSetLayoutBinding binding{};
    binding.binding = 1;
    binding.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    binding.descriptorCount = 1;
    binding.stageFlags = VK_SHADER_STAGE_VERTEX_BIT;

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 1;
    li.pBindings = &binding;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &starDescLayout);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 1;
    pi.pPoolSizes = &ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &starDescPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = starDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &starDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &starDescSet);

    VkDescriptorBufferInfo bufInfo{starBuf, 0, VK_WHOLE_SIZE};
    VkWriteDescriptorSet wr{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
    wr.dstSet = starDescSet;
    wr.dstBinding = 1;
    wr.descriptorCount = 1;
    wr.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    wr.pBufferInfo = &bufInfo;
    vkUpdateDescriptorSets(ctx.device, 1, &wr, 0, nullptr);

    createStarPipeline(ctx);

    // Do an initial upload so stars are visible from frame 1.
    updateStars();
}

// ─── createStarPipeline ───────────────────────────────────────────────────────
// Uses star_point.vert (twinkling + shared layout) + star_point.frag (tight core only,
// no satellite-style outer glow — prevents bright stars from becoming blobs).
void SatelliteSim::createStarPipeline(VulkanContext &ctx)
{
    VkShaderModule vert = ctx.loadShader("shaders/star_point.vert.spv");
    VkShaderModule frag = ctx.loadShader("shaders/star_point.frag.spv");

    VkPipelineShaderStageCreateInfo stages[2] = {};
    stages[0] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_VERTEX_BIT, vert, "main", nullptr};
    stages[1] = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0,
                 VK_SHADER_STAGE_FRAGMENT_BIT, frag, "main", nullptr};

    VkPipelineVertexInputStateCreateInfo vi{VK_STRUCTURE_TYPE_PIPELINE_VERTEX_INPUT_STATE_CREATE_INFO};
    VkPipelineInputAssemblyStateCreateInfo ia{VK_STRUCTURE_TYPE_PIPELINE_INPUT_ASSEMBLY_STATE_CREATE_INFO};
    ia.topology = VK_PRIMITIVE_TOPOLOGY_POINT_LIST;

    VkViewport vp{0, 0, (float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, ctx.swapExtent};
    VkPipelineViewportStateCreateInfo vps{VK_STRUCTURE_TYPE_PIPELINE_VIEWPORT_STATE_CREATE_INFO};
    vps.viewportCount = 1;
    vps.pViewports = &vp;
    vps.scissorCount = 1;
    vps.pScissors = &sc;

    VkPipelineRasterizationStateCreateInfo rast{VK_STRUCTURE_TYPE_PIPELINE_RASTERIZATION_STATE_CREATE_INFO};
    rast.polygonMode = VK_POLYGON_MODE_FILL;
    rast.cullMode = VK_CULL_MODE_NONE;
    rast.frontFace = VK_FRONT_FACE_COUNTER_CLOCKWISE;
    rast.lineWidth = 1.0f;

    VkPipelineMultisampleStateCreateInfo ms{VK_STRUCTURE_TYPE_PIPELINE_MULTISAMPLE_STATE_CREATE_INFO};
    ms.rasterizationSamples = VK_SAMPLE_COUNT_1_BIT;

    // Same depth test as satellites: stars at fixed depth 0.5 are culled by close terrain.
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_TRUE;
    ds.depthWriteEnable = VK_FALSE;
    ds.depthCompareOp = VK_COMPARE_OP_LESS;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_TRUE;
    cba.srcColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstColorBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.colorBlendOp = VK_BLEND_OP_ADD;
    cba.srcAlphaBlendFactor = VK_BLEND_FACTOR_ONE;
    cba.dstAlphaBlendFactor = VK_BLEND_FACTOR_ZERO;
    cba.alphaBlendOp = VK_BLEND_OP_ADD;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT |
                         VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    if (starPipeLayout == VK_NULL_HANDLE)
    {
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT, 0, sizeof(SatDrawPC)};
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &starDescLayout;
        li.pushConstantRangeCount = 1;
        li.pPushConstantRanges = &pcr;
        vkCreatePipelineLayout(ctx.device, &li, nullptr, &starPipeLayout);
    }

    VkGraphicsPipelineCreateInfo ci{VK_STRUCTURE_TYPE_GRAPHICS_PIPELINE_CREATE_INFO};
    ci.stageCount = 2;
    ci.pStages = stages;
    ci.pVertexInputState = &vi;
    ci.pInputAssemblyState = &ia;
    ci.pViewportState = &vps;
    ci.pRasterizationState = &rast;
    ci.pMultisampleState = &ms;
    ci.pDepthStencilState = &ds;
    ci.pColorBlendState = &cb;
    ci.layout = starPipeLayout;
    ci.renderPass = ctx.renderPass;
    ci.subpass = 0;

    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &starPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create star pipeline");

    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

// ─── updateStars ──────────────────────────────────────────────────────────────
// Transforms star ECI unit vectors into ENU each frame (Earth rotates under stars).
// Stars fade out during civil/nautical twilight — invisible in full daylight.
void SatelliteSim::updateStars()
{
    if (!starMapped || starCount == 0)
        return;

    // Stars become visible as the sun sinks below the horizon.
    // sin(elevation) = sunDirENU.w: 0 at horizon, -0.2 at ~11.5° below.
    float nightFactor = glm::clamp(-sunDirENU.w * 5.0f, 0.0f, 1.0f);

    // In space the sky is dark regardless of sun angle — no atmosphere to scatter.
    // atmFrac decays with the same 80 km scale height used for sat daytime suppression.
    float obsR = glm::length(obsECI);
    float obsHeight = obsR - kEarthRadius;
    float atmFrac = glm::clamp(glm::exp(-obsHeight / 80000.0f), 0.0f, 1.0f);
    float nightFactorEff = glm::mix(1.0f, nightFactor, atmFrac); // 1.0 in space

    // Earth-limb elevation cutoff: from altitude, stars are visible below the 0° horizon.
    float r = kEarthRadius / obsR;
    float limbSin = (obsHeight > 1.0f) ? -sqrtf(glm::max(0.0f, 1.0f - r * r)) : 0.0f;

    auto *dst = static_cast<GpuSatVisible *>(starMapped);
    for (uint32_t i = 0; i < starCount; ++i)
    {
        const auto &rec = starRecords[i];

        // Rotate from inertial ECI into the observer's local ENU frame.
        glm::vec3 enu{glm::dot(rec.eciDir, glm::vec3(eci2enuX)),
                      glm::dot(rec.eciDir, glm::vec3(eci2enuY)),
                      glm::dot(rec.eciDir, glm::vec3(eci2enuZ))};

        // Above the Earth limb: visible. Below: culled.
        float intensity = (enu.z >= limbSin) ? rec.rawIntensity * nightFactorEff : 0.0f;

        dst[i].skyDir = enu;
        dst[i].flareIntensity = intensity;
        dst[i].baseColor = rec.color;
        dst[i].angularSize = rec.angSize;
    }
}

// ─── initConstellation ────────────────────────────────────────────────────────
// Entry point called once from init().  Loads satellite type and constellation
// definitions (from constellations.json or hardcoded fallback), then builds the
// flat satOrbits array that drives per-frame position updates.
void SatelliteSim::initConstellation()
{
    loadDefinitions();
    buildOrbits();
}

// ─── JSON helpers (file-local) ────────────────────────────────────────────────
static AttitudeMode parseAttitudeMode(const std::string &s)
{
    if (s == "NadirPointing")
        return AttitudeMode::NadirPointing;
    if (s == "SunTracking")
        return AttitudeMode::SunTracking;
    if (s == "Tumbling")
        return AttitudeMode::Tumbling;
    if (s == "Perpendicular")
        return AttitudeMode::Perpendicular;
    if (s == "AntiNadir")
        return AttitudeMode::AntiNadir;
    if (s == "FlatMirror45")
        return AttitudeMode::FlatMirror45;
    if (s == "TargetedReflector")
        return AttitudeMode::TargetedReflector;
    if (s == "KnifeEdge")
        return AttitudeMode::KnifeEdge;
    if (s == "SunPerp")
        return AttitudeMode::SunPerp;
    fprintf(stderr, "[SatelliteSim] Unknown AttitudeMode '%s'; using NadirPointing.\n", s.c_str());
    return AttitudeMode::NadirPointing;
}

static SurfaceSpec parseSurfaceSpec(const nlohmann::json &j)
{
    return {
        parseAttitudeMode(j.value("attitude", std::string("NadirPointing"))),
        j.value("spec_exp", 0.0f),
        j.value("weight", 0.0f),
    };
}

// ─── loadDefinitions ─────────────────────────────────────────────────────────
// Reads constellations.json from the exe directory.  If the file is missing or
// malformed, falls back to loadHardcoded() and logs the reason to stderr.
void SatelliteSim::loadDefinitions()
{
    auto jsonPath = (std::filesystem::path(exeDir_) / "constellations.json").string();
    std::ifstream f(jsonPath);
    if (!f.is_open())
    {
        fprintf(stderr, "[SatelliteSim] constellations.json not found at '%s';"
                        " using hardcoded defaults.\n",
                jsonPath.c_str());
        loadHardcoded();
        return;
    }

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception &e)
    {
        fprintf(stderr, "[SatelliteSim] Failed to parse constellations.json: %s\n"
                        "              Using hardcoded defaults.\n",
                e.what());
        loadHardcoded();
        return;
    }

    // ── Satellite types ───────────────────────────────────────────────────────
    satTypes.clear();
    for (const auto &jt : j.value("satellite_types", nlohmann::json::array()))
    {
        SatelliteType t;
        t.name = jt["name"].get<std::string>();
        auto col = jt["base_color"];
        t.baseColor = {col[0].get<float>(), col[1].get<float>(), col[2].get<float>()};
        t.crossSectionM2 = jt["cross_section_m2"].get<float>();
        t.primary = parseSurfaceSpec(jt["primary"]);
        t.secondary = jt.contains("secondary")
                          ? parseSurfaceSpec(jt["secondary"])
                          : SurfaceSpec{AttitudeMode::Perpendicular, 0.0f, 0.0f};
        t.diffuse = jt.value("diffuse", 0.02f);
        t.mirrorFrac = jt.value("mirror_frac", 0.0f);
        satTypes.push_back(std::move(t));
    }

    // Build name → index map so constellations can reference types by name.
    std::unordered_map<std::string, uint32_t> typeMap;
    for (uint32_t i = 0; i < (uint32_t)satTypes.size(); ++i)
        typeMap[satTypes[i].name] = i;

    // ── Constellations ────────────────────────────────────────────────────────
    constellations.clear();
    for (const auto &jc : j.value("constellations", nlohmann::json::array()))
    {
        ConstellationConfig c;
        c.name = jc["name"].get<std::string>();
        c.numPlanes = jc["num_planes"].get<int>();
        c.perPlane = jc["per_plane"].get<int>();
        c.altM = jc["alt_km"].get<float>() * 1000.0f;
        c.incl = glm::radians(jc.value("incl_deg", 0.0f));
        c.enabled = jc.value("enabled", true);

        std::string typeName = jc["type"].get<std::string>();
        auto it = typeMap.find(typeName);
        if (it == typeMap.end())
        {
            fprintf(stderr, "[SatelliteSim] Constellation '%s' references unknown type '%s';"
                            " skipping.\n",
                    c.name.c_str(), typeName.c_str());
            continue;
        }
        c.typeIdx = it->second;

        std::string dist = jc.value("distribution", std::string("Walker"));
        if (dist == "RandomShell")
            c.distribution = OrbitDistribution::RandomShell;
        else if (dist == "Disk")
            c.distribution = OrbitDistribution::Disk;
        else
            c.distribution = OrbitDistribution::Walker;

        c.altJitterM = jc.value("alt_jitter_km", 0.0f) * 1000.0f;
        c.raan = glm::radians(jc.value("raan_deg", 0.0f));
        c.alignTerminator = jc.value("align_terminator", false);
        c.numRings = jc.value("num_rings", 1);
        c.ringSpacingM = jc.value("ring_spacing_km", 0.0f) * 1000.0f;

        constellations.push_back(std::move(c));
    }

    hovConst.assign(constellations.size(), false);
    hovHighlightConst.assign(constellations.size(), false);
    fprintf(stderr, "[SatelliteSim] Loaded %zu satellite type(s) and %zu constellation(s)"
                    " from %s\n",
            satTypes.size(), constellations.size(), jsonPath.c_str());
}

// ─── loadHardcoded ────────────────────────────────────────────────────────────
// Hardcoded satellite type catalogue and constellation shells — used as fallback
// when constellations.json is absent or malformed.
void SatelliteSim::loadHardcoded()
{
    // AI SAT
    // 175 m estimate, 1820 x 72
    // scale 1820px / 175 m
    // 870 * 72 px panels
    // 200 x 55 px radiator panels

    float px_scale = 1820.0f / 175.0f;                                                      // ~10.4 px/meter for the bus/antenna face
    float ai_sat_panel_area = (870.0f * 72.0f * 2.0f) / (px_scale * px_scale);              // ~1158 m² total panel area
    [[maybe_unused]] float ai_sat_radiator_area = (200.0f * 55.0f) / (px_scale * px_scale); // ~102 m² total radiator area

    satTypes = {
        {// 0 — Starlink: flat phased-array face toward Earth, brief intense flares
         "Starlink",
         {0.80f, 0.87f, 1.00f},                      // cool blue-white
         10.0f,                                      // ~10 m² bus + visor
         {AttitudeMode::NadirPointing, 18.0f, 1.0f}, // very sharp specular (flat mirror-like face)
         {AttitudeMode::Perpendicular, 0.0f, 0.0f},  // no significant secondary surface
         0.01f,                                      // no diffuse floor (visor-darkened)
         0.05f},                                     // mirrorFrac: polished phased-array glass → mag ~-2.7 at perfect alignment
        {                                            // 1 — LEO broadband (OneWeb/Kuiper/Xingwang/Telesat): sun-tracking panels
         "LEO Broadband",
         {1.00f, 0.92f, 0.75f}, // warm white
         5.0f,                  // ~12 m² typical LEO broadband bus + panels
         {AttitudeMode::SunTracking, 18.0f, 1.0f},
         {AttitudeMode::Perpendicular, 0.0f, 0.0f},
         0.01f,  // no diffuse floor
         0.02f}, // mirrorFrac: moderate — sun-tracking panels occasionally flash
        {        // 2 — GEO Comsat: large sun-tracking panels + body radiators facing away from Earth
         "GEO Comsat",
         {0.95f, 0.95f, 1.00f},                    // near-white
         50.0f,                                    // ~50 m² (large GEO body + wings)
         {AttitudeMode::SunTracking, 3.0f, 1.00f}, // broad lobe solar wings
         {AttitudeMode::AntiNadir, 2.0f, 0.10f},   // body radiators face deep space
         0.05f,                                    // slight structural glow
         0.10f},                                   // mirrorFrac: large polished antenna dishes, well-aligned
        {                                          // 3 — ISS: enormous truss-mounted solar arrays AND large radiator panels.
         // The PVTCS and EATCS radiators (~900 m² NH3 panels on the ITS) face away from
         // Earth for maximum view factor to cold space. From the ground: ISS at zenith shows
         // the back of the radiators (dim); ISS near the horizon shows the radiator face.
         "ISS",
         {1.00f, 0.85f, 0.70f}, // warm golden (solar array color)
         250.0f,
         {AttitudeMode::SunTracking, 12.0f, 1.00f}, // truss-mounted solar arrays
         {AttitudeMode::AntiNadir, 4.0f, 0.35f},    // large radiator panels (ITS) face deep space
         0.04f,                                     // complex truss/module body
         0.05f},                                    // mirrorFrac: highly polished solar panel glass → mag ~-7.5 at peak
        {                                           // 4 — SpaceX AI1 datacenter satellite (revealed 2026).
         // Bus: nadir-pointing (phased-array antenna faces Earth). Modeled via diffuse.
         // Solar arrays: ~600 m², two-axis tracked (bus yaw + panel gimbal) → always face sun.
         // Radiators: 110 m² deployable liquid panels, hard-mounted perpendicular to bus.
         //   The bus yaws to let solar wings track the sun, which constrains the radiators
         //   to normal = cross(sunDir, satNadir) — always edge-on to the sun by design.
         //   irr = 0 always (correct: radiator must never see the sun to reject heat).
         //   Visual contribution is through the diffuse parameter (large structure scatter).
         // crossSection = sqrt(600/10) ≈ 7.75 for the dominant solar wing area.

         "SpaceX AI Sats",
         {1.00f, 1.00f, 0.92f},                    // cyan-teal (distinct from Starlink blue-white)
         600.0f,                                   // 600 m² solar array area (150 kW / 250 W/m²)
         {AttitudeMode::SunTracking, 25.0f, 1.0f}, // solar wings — always face sun, sharp specular
         {AttitudeMode::SunPerp, 3.0f, 0.18f},     // radiators 110 m² — edge-on to sun, irr=0
         0.06f,                                    // bus body + radiator structure bulk scatter
         0.01f},                                   // mirrorFrac: polished solar panel glass
        {                                          // 5 — Reflect Orbital mirror (speculative, 55 m diameter flat mirror).
         // FlatMirror45: normal = normalize(sunDir + satNadir).
         // By construction reflect(-sunDir, n) = satNadir — reflected sunlight
         // hits the ground directly below the satellite.  In SSO the mirror spends
         // most of its time near the terminator, so ground observers see a brilliant
         // slow-moving point of light just before dawn or just after dusk.
         //
         // To switch to targeted multi-beam focusing, change AttitudeMode to
         // TargetedReflector and call the orbit post-processing loop below.
         //
         // Area: 55 m diameter circular mirror → π × 27.5² ≈ 2,376 m²
         // crossSection = sqrt(2376/10) ≈ 15.4  (vs. Starlink ~1.0)
         // At 500 km, perfect alignment: peak effectFlare ≈ 10,000+ → mag ≈ −11
         // (comparable to a quarter moon; visible in daylight sky)
         "Reflect Mirror",
         {1.00f, 0.97f, 0.94f},                           // warm silver-white
         2376.0f,                                         // 55 m diameter mirror area (m²)
         {AttitudeMode::TargetedReflector, 200.0f, 1.0f}, // near-perfect flat mirror; tight but not laser-narrow lobe
         {AttitudeMode::Perpendicular, 0.0f, 0.0f},       // no secondary surface
         0.02f,                                           // no diffuse scatter (mirror absorbs nothing)
         0.97f},                                          // mirrorFrac: near-perfect specular mirror
        {                                                 // 6 — Space debris: defunct satellites, rocket bodies, fragments.
         // Tumbling attitude — chaotic rotation around a random body axis.
         // Rate varies per object from near-stationary to ~1 Hz flicker.
         // Small area, rough surfaces, no attitude control.
         "Debris",
         {0.78f, 0.74f, 0.68f},                     // dull grey-tan (aged thermal blanket / oxidised metal)
         1.0f,                                      // ~3 m² effective cross-section (fragment to small bus)
         {AttitudeMode::Tumbling, 6.0f, 1.0f},      // rough diffuse tumble; occasional glint
         {AttitudeMode::Perpendicular, 6.0f, 0.0f}, // no secondary surface
         0.001f,                                    // high diffuse floor — structural clutter scatters everywhere
         0.03f},                                    // rare metallic glints from exposed foil or polished surfaces
        {                                           // 7 — Starlink (knife-edge): SpaceX roll-angle policy adopted 2020.
         // Body rolls around the along-track axis so the phased-array face is
         // edge-on to the sun; solar panel gimbals counter-rotate to compensate.
         // Roll is clamped to ±kKnifeMaxRollDeg (80°) — solar power constraint.
         // Measured effect: ~90% brightness reduction vs. NadirPointing at standard
         // distance (Mallama & Respler 2023, 2303.01431).  Residual brightness at
         // the clamp limit: specular ∝ cos(80°) ≈ 0.17 of fully-lit nadir face.
         "Starlink KE",
         {0.80f, 0.87f, 1.00f},                     // same cool blue-white as Starlink
         10.0f,                                     // same bus area
         {AttitudeMode::KnifeEdge, 18.0f, 1.0f},    // sharp specular; normal set by roll solver
         {AttitudeMode::Perpendicular, 0.0f, 0.0f}, // no secondary surface
         0.01f,                                     // visor-darkened diffuse floor
         0.05f},                                    // same polished array face as baseline Starlink
    };

    // ── Constellation shells ───────────────────────────────────────────────────
    // Walker:  numPlanes × perPlane satellites, regular spacing.
    // Random:  numPlanes satellites total, random orbital params.
    // Disk:    numPlanes satellites in one or more concentric rings.
    //   .altJitterM   = per-satellite altitude scatter (random)
    //   .raan         = orbital plane RAAN (unless alignTerminator=true)
    //   .alignTerminator = derive incl+raan from sunDirECI at init
    //   .numRings     = concentric rings (1 = single ring)
    //   .ringSpacingM = altitude step between rings
    // ── Real mega-constellation data (source: planet4589.org/space/con/conlist.html) ──
    // Walker field order: name, altM, incl, numPlanes, perPlane, typeIdx, enabled, distribution
    //   total sats = numPlanes × perPlane
    // Disk field order (extra trailing args): ..., altJitterM, raan, alignTerminator, numRings, ringSpacingM
    //   total sats = numPlanes × perPlane, spread evenly across numRings concentric rings
    //   alignTerminator=true: overrides incl+raan to track sunDirECI (orbital plane = terminator plane)
    // All totals fit within MAX_SATELLITES=100,000 when all enabled simultaneously (~98,907).

    constellations = {
        // SpaceX Starlink Gen1 — FCC filing: 4,408 sats; 72 planes × 61 = 4,392
        {"Starlink Gen1",
         550'000.0f,          // altM:      550 km
         glm::radians(53.0f), // incl:      53° — primary mid-inclination shell
         72,                  // numPlanes: orbital planes
         61,                  // perPlane:  sats per plane (72×61 = 4,392)
         0u,                  // typeIdx:   Starlink (NadirPointing)
         true,                // enabled
         OrbitDistribution::Walker},

        // SpaceX Starlink Gen2 — FCC filing: 30,456 sats; 120 planes × 254 = 30,480
        // Uses knife-edge roll (type 7) to model SpaceX's 2020 roll-angle policy.
        {"Starlink Gen2",
         525'000.0f,          // altM:      525 km (slightly lower than Gen1)
         glm::radians(53.2f), // incl:      53.2°
         120,                 // numPlanes: orbital planes
         254,                 // perPlane:  sats per plane (120×254 = 30,480)
         7u,                  // typeIdx:   Starlink KE (KnifeEdge roll)
         true,                // enabled
         OrbitDistribution::Walker},

        // OneWeb (UK/Eutelsat) — planned: 648 sats; 18 planes × 36 = 648
        {"OneWeb",
         1'200'000.0f,        // altM:      1,200 km
         glm::radians(87.9f), // incl:      87.9° — near-polar
         18,                  // numPlanes: orbital planes
         36,                  // perPlane:  sats per plane (18×36 = 648)
         1u,                  // typeIdx:   LEO Broadband (SunTracking)
         true,                // enabled
         OrbitDistribution::Walker},

        // Amazon Kuiper — FCC filing: 7,774 sats; 98 planes × 79 = 7,742
        {"Amazon LEO",
         630'000.0f,          // altM:      630 km
         glm::radians(51.9f), // incl:      51.9°
         98,                  // numPlanes: orbital planes
         79,                  // perPlane:  sats per plane (98×79 = 7,742)
         1u,                  // typeIdx:   LEO Broadband (SunTracking)
         true,                // enabled
         OrbitDistribution::Walker},

        // China Xingwang/GW (CASC/CASIC) — planned: ~13,952 sats; 80 planes × 174 = 13,920
        {"Guowang",
         508'000.0f,          // altM:      508 km
         glm::radians(85.0f), // incl:      85° — near-polar
         80,                  // numPlanes: orbital planes
         174,                 // perPlane:  sats per plane (80×174 = 13,920)
         1u,                  // typeIdx:   LEO Broadband (SunTracking)
         true,                // enabled
         OrbitDistribution::Walker},

        // International Space Station — single object for visual reference
        {"ISS",
         408'000.0f,          // altM:      408 km
         glm::radians(51.6f), // incl:      51.6°
         1,                   // numPlanes: 1 plane
         1,                   // perPlane:  1 satellite
         3u,                  // typeIdx:   ISS (SunTracking + large radiators)
         true,                // enabled
         OrbitDistribution::Walker},

        // SpaceX Orbital Data Center — sun-synchronous Disk shell (FCC filing Jan 2026)
        //   Disk+alignTerminator places the ring in the Earth-Sun terminator plane, visually
        //   representing where SSO satellites dwell relative to the day/night boundary.
        //   200 × 100 = 20,000 sats spread across 10 rings from ~575 km to ~1,925 km.
        {"SpaceX AI Sat",
         1'250'000.0f, // altM:      1,250 km — ring centre altitude
         0.0f,         // incl:      ignored (alignTerminator=true overrides)
         200,          // numPlanes: × perPlane = total sats (200×100 = 20,000)
         100,          // perPlane:  × numPlanes = total sats
         4u,           // typeIdx:   SpaceX ODC (NadirPointing + AntiNadir radiators)
         true,         // enabled
         OrbitDistribution::Disk,
         5000.0f,         // altJitterM:      no per-satellite altitude scatter
         0.0f,            // raan:            ignored (alignTerminator=true)
         true,            // alignTerminator: orbital plane = terminator plane (tracks Sun)
         10 * 2,          // numRings:        10 concentric rings
         150'000.0f / 2}, // ringSpacingM:    150 km between rings (575–1,925 km range)

        // Reflect Orbital — speculative 55 m flat mirror constellation (SSO Walker).
        // FlatMirror45 attitude keeps mirror normal = normalize(sunDir+satNadir) each frame,
        // reflecting sunlight straight down to the ground below.
        // Disabled by default: enabling while all other constellations are on exceeds
        // MAX_SATELLITES=100,000.  Toggle others off first, or raise the cap.
        //
        // To enable focused multi-beam targeting (10 ground spots, ~500 mirrors each):
        //   1. Change typeIdx to 5 — switch sat type primary to TargetedReflector.
        //      (Add a type-6 TargetedReflector variant and reference it here, or edit type-5.)
        //   2. Ensure the post-processing loop below runs (it already does for typeIdx==5).
        {"Reflect Orbital",
         500'000.0f, // altM:   500 km — low LEO for maximum ground flux
         0,          // incl:   SSO retrograde (~97.4°) from J2 formula
         10,         // numPlanes: orbital planes
         100,        // perPlane:  50×100 = 5,000 satellites
         5u,         // typeIdx:   Reflect Mirror (FlatMirror45)
         true,       // enabled:   OFF — enabling pushes total > MAX_SATELLITES
         OrbitDistribution::Disk,
         1000.0f, // altJitterM:      no per-satellite altitude scatter
         0.0f,    // raan:            ignored (alignTerminator=true)
         true,    // alignTerminator: orbital plane = terminator plane (tracks Sun)
         10,      // numRings:        10 concentric rings
         10000},  // ringSpacingM:    150 km between rings (575–1,925 km range)

        // Space Junk — LEO debris shell modelling defunct satellites, rocket bodies,
        // and large fragments.  Random inclinations (0–180°) give isotropic coverage.
        // Each object gets an independent tumble axis + rate (0–1 Hz) so flickers
        // are desynchronised across the shell.
        {"Space Junk",
         1'000'000.0f,     // altM:       600 km — centre of dense LEO debris band
         glm::pi<float>(), // incl: random 0..180° → full spherical coverage
         100,              // numPlanes:  }
         30,               // perPlane:   } 1 × 3,000 = 3,000 debris objects
         6u,               // typeIdx:    Debris (typeIdx 6)
         true,             // enabled
         OrbitDistribution::RandomShell,
         500'000.0f}, // altJitterM: ±200 km → 400–800 km altitude band
    };

    hovConst.assign(constellations.size(), false);
    hovHighlightConst.assign(constellations.size(), false);
}

// ─── loadSettings ─────────────────────────────────────────────────────────────
// Reads settings.json from the exe directory.  Silently skips if the file is
// missing (first run).  Logs a warning and returns on parse error.
// Must be called after initConstellation() so constellations[] is populated.
void SatelliteSim::loadSettings()
{
    auto path = (std::filesystem::path(exeDir_) / "settings.json").string();
    std::ifstream f(path);
    if (!f.is_open())
        return; // first run — silently use defaults

    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception &e)
    {
        fprintf(stderr, "[SatelliteSim] Failed to parse settings.json: %s\n", e.what());
        return;
    }

    if (j.contains("photometry"))
    {
        auto &p = j["photometry"];
        brightnessScale = p.value("brightness_scale", brightnessScale);
        daySuppression = p.value("day_suppression", daySuppression);
        mirrorBoost = p.value("mirror_boost", mirrorBoost);
        visThresh = p.value("vis_thresh", visThresh);
        highlightFlare = p.value("highlight_flare", highlightFlare);
    }

    if (j.contains("display"))
    {
        uiScale = j["display"].value("ui_scale", uiScale);
        settingsWinX = j["display"].value("win_x", settingsWinX);
        settingsWinY = j["display"].value("win_y", settingsWinY);
    }

    if (j.contains("audio"))
    {
        auto &a = j["audio"];
        masterVol_ = a.value("master_vol", masterVol_);
        musicVol_ = a.value("music_vol", musicVol_);
        sfxVol_ = a.value("sfx_vol", sfxVol_);
        // audio_ is null here (setAudio not called yet); volumes are applied there.
    }

    if (j.contains("camera"))
    {
        auto &c = j["camera"];
        camera.azDeg = c.value("az_deg", camera.azDeg);
        camera.elDeg = c.value("el_deg", camera.elDeg);
        camera.fovYDeg = c.value("fov_y_deg", camera.fovYDeg);
    }

    if (j.contains("observer"))
    {
        float latDeg = j["observer"].value("lat_deg", obsLatDeg);
        float lonDeg = j["observer"].value("lon_deg", obsLonDeg);
        float lat = glm::radians(latDeg);
        float lon = glm::radians(lonDeg);
        obsDir = {cosf(lat) * cosf(lon), cosf(lat) * sinf(lon), sinf(lat)};
        obsFacing = {-sinf(lat) * cosf(lon), -sinf(lat) * sinf(lon), cosf(lat)};
        obsLatDeg = latDeg;
        obsLonDeg = lonDeg;
    }

    if (j.contains("time"))
    {
        timeScaleIdx = j["time"].value("scale_idx", timeScaleIdx);
        timeScaleIdx = std::clamp(timeScaleIdx, 0, kNumTimeScales - 1);
    }

    if (j.contains("controls") && j["controls"].contains("keybindings"))
    {
        std::unordered_map<std::string, int> actionKey;
        for (const auto &kb : j["controls"]["keybindings"])
            if (kb.contains("action") && kb.contains("key"))
                actionKey[kb["action"].get<std::string>()] = kb["key"].get<int>();
        for (auto &kb : keybindings)
        {
            auto it = actionKey.find(kb.action);
            if (it != actionKey.end())
                kb.key = it->second;
        }
    }

    if (j.contains("constellations"))
    {
        std::unordered_map<std::string, const nlohmann::json *> byName;
        for (const auto &jc : j["constellations"])
            if (jc.contains("name"))
                byName[jc["name"].get<std::string>()] = &jc;
        for (auto &c : constellations)
        {
            auto it = byName.find(c.name);
            if (it != byName.end())
            {
                c.enabled = it->second->value("enabled", c.enabled);
                c.highlight = it->second->value("highlight", c.highlight);
            }
        }
    }

    if (j.contains("clouds"))
    {
        auto &c = j["clouds"];
        cloudCoverage    = c.value("coverage",     cloudCoverage);
        cloudDensity     = c.value("density",      cloudDensity);
        cloudBaseAltM    = c.value("base_alt_m",   cloudBaseAltM);
        cloudTopAltM     = c.value("top_alt_m",    cloudTopAltM);
        cloudDriftRate   = c.value("drift_rate",   cloudDriftRate);
        cloudSunGain     = c.value("sun_gain",     cloudSunGain);
        cloudAmbientGain = c.value("ambient_gain", cloudAmbientGain);
        cloudHgG         = c.value("hg_g",         cloudHgG);
        cloudMarchSteps  = c.value("march_steps",  cloudMarchSteps);
        cloudLightSteps  = c.value("light_steps",  cloudLightSteps);
    }

    fprintf(stderr, "[SatelliteSim] Loaded settings from %s\n", path.c_str());
}

// ─── saveSettings ─────────────────────────────────────────────────────────────
// Writes the current runtime state to settings.json next to the exe.
// Called on cleanup() and when the settings window is closed.
void SatelliteSim::saveSettings()
{
    if (exeDir_.empty())
        return;

    nlohmann::json j;

    j["photometry"] = {
        {"brightness_scale", brightnessScale},
        {"day_suppression", daySuppression},
        {"mirror_boost", mirrorBoost},
        {"vis_thresh", visThresh},
        {"highlight_flare", highlightFlare}};

    j["display"] = {{"ui_scale", uiScale}};
    if (settingsWinX >= 0.0f)
    {
        j["display"]["win_x"] = settingsWinX;
        j["display"]["win_y"] = settingsWinY;
    }

    j["audio"] = {
        {"master_vol", audio_ ? audio_->getMasterVolume() : masterVol_},
        {"music_vol", audio_ ? audio_->getMusicVolume() : musicVol_},
        {"sfx_vol", audio_ ? audio_->getSfxVolume() : sfxVol_}};

    j["camera"] = {
        {"az_deg", camera.azDeg},
        {"el_deg", camera.elDeg},
        {"fov_y_deg", camera.fovYDeg}};

    j["observer"] = {{"lat_deg", obsLatDeg}, {"lon_deg", obsLonDeg}};

    j["time"] = {{"scale_idx", timeScaleIdx}};

    j["clouds"] = {
        {"coverage",     cloudCoverage},
        {"density",      cloudDensity},
        {"base_alt_m",   cloudBaseAltM},
        {"top_alt_m",    cloudTopAltM},
        {"drift_rate",   cloudDriftRate},
        {"sun_gain",     cloudSunGain},
        {"ambient_gain", cloudAmbientGain},
        {"hg_g",         cloudHgG},
        {"march_steps",  cloudMarchSteps},
        {"light_steps",  cloudLightSteps}};

    nlohmann::json kbArr = nlohmann::json::array();
    for (const auto &kb : keybindings)
        kbArr.push_back({{"action", kb.action}, {"key", kb.key}});
    j["controls"]["keybindings"] = kbArr;

    nlohmann::json constArr = nlohmann::json::array();
    for (const auto &c : constellations)
        constArr.push_back({{"name", c.name}, {"enabled", c.enabled}, {"highlight", c.highlight}});
    j["constellations"] = constArr;

    auto path = (std::filesystem::path(exeDir_) / "settings.json").string();
    try
    {
        std::ofstream f(path);
        f << j.dump(4) << '\n';
    }
    catch (const std::exception &e)
    {
        fprintf(stderr, "[SatelliteSim] Failed to save settings.json: %s\n", e.what());
    }
}

// ─── buildOrbits ──────────────────────────────────────────────────────────────
// Generates the flat satOrbits array from whatever is currently in satTypes and
// constellations (loaded by loadDefinitions or loadHardcoded).
// Also generates reflector ground targets and applies the MAX_SATELLITES cap.
void SatelliteSim::buildOrbits()
{
    // ── Populate satOrbits ────────────────────────────────────────────────────
    satOrbits.clear();
    for (ConstellationConfig &c : constellations)
    {
        c.orbitStart = (uint32_t)satOrbits.size();

        if (c.distribution == OrbitDistribution::Walker)
        {
            for (int p = 0; p < c.numPlanes; ++p)
            {
                float raan = (float)p / c.numPlanes * glm::two_pi<float>();
                for (int s = 0; s < c.perPlane; ++s)
                {
                    float u0 = (float)rand() / (float)RAND_MAX * glm::two_pi<float>();
                    satOrbits.push_back({raan, c.incl, u0, c.typeIdx, c.altM, 0.0f, 0.0f, {0.0f, 0.0f, 1.0f}, false});
                }
            }
        }
        else if (c.distribution == OrbitDistribution::RandomShell)
        {
            int total = c.numPlanes * c.perPlane;
            for (int i = 0; i < total; ++i)
            {
                float raan = (float)rand() / RAND_MAX * glm::two_pi<float>();
                float incl = (float)rand() / RAND_MAX * c.incl;
                float u0 = (float)rand() / RAND_MAX * glm::two_pi<float>();
                float jitter = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * c.altJitterM;
                float altM = c.altM + jitter;

                float phi = (float)rand() / RAND_MAX * glm::two_pi<float>();
                float cosTheta = (float)rand() / RAND_MAX * 2.0f - 1.0f;
                float sinTheta = sqrtf(1.0f - cosTheta * cosTheta);
                glm::vec3 axis{sinTheta * cosf(phi), sinTheta * sinf(phi), cosTheta};

                // Randomise rotation rate 0..2π rad/s (0..1 Hz) so debris
                // objects tumble independently rather than blinking in unison.
                float tumbleRate = (float)rand() / (float)RAND_MAX * glm::two_pi<float>() * 0.001;
                float tumblePhase = (float)rand() / RAND_MAX * glm::two_pi<float>();

                satOrbits.push_back({raan, incl, u0, c.typeIdx, altM,
                                     tumbleRate, tumblePhase, axis, false});
            }
        }
        else if (c.distribution == OrbitDistribution::Disk)
        {
            // Determine orbital plane.  For alignTerminator, RAAN is shared across all rings
            // but inclination is computed per-ring from each ring's actual altitude.
            float incl_d = c.incl;
            float raan_d = c.raan;
            if (c.alignTerminator)
            {
                // Anchor RAAN at the simulation start time using the sun direction already
                // computed by updatePositions().  uploadSatOrbits() then precesses only
                // (orbitEpochT0 - t_start) seconds forward, so liveRaan = raan_start +
                // kSSOPrecRate*(simTime - t_start).  Anchoring here rather than at J2000
                // eliminates the ~3° obliquity-driven phase error that accumulates when
                // extrapolating 36+ years with a constant precession rate.
                raan_d = atan2f(sunDirECI.x, -sunDirECI.y);
            }

            // Distribute satellites across numRings concentric rings.
            // The rings are centred on c.altM and spaced by c.ringSpacingM.
            int totalSats = c.numPlanes * c.perPlane;
            int nr = glm::max(1, c.numRings);
            int perRing = (totalSats + nr - 1) / nr; // ceiling division

            for (int r = 0; r < nr; ++r)
            {
                // Altitude: centre-offset each ring around c.altM.
                float ringAlt = c.altM + (r - (nr - 1) * 0.5f) * c.ringSpacingM;
                // For SSO constellations compute the exact J2 inclination for each ring's
                // altitude rather than using the centre altitude for all rings.  A 1500 km
                // span (e.g. 500–2000 km) otherwise biases every ring by up to ±3.7°.
                float ringIncl = c.alignTerminator ? computeSSOInclination(ringAlt) : incl_d;

                // Model incomplete constellation, vary number of sats per ring to fill totalSats without exceeding it.
                int satsInThisRing = glm::min(perRing, totalSats - r * perRing);

                for (int s = 0; s < satsInThisRing; ++s)
                {
                    // Evenly spaced around the ring + optional small jitter.
                    float u0 = (float)s / satsInThisRing * glm::two_pi<float>();
                    float jitter = ((float)rand() / RAND_MAX * 2.0f - 1.0f) * c.altJitterM;
                    satOrbits.push_back({raan_d, ringIncl, u0, c.typeIdx, ringAlt + jitter, 0.0f, 0.0f, {0.0f, 0.0f, 1.0f}, c.alignTerminator});
                }
            }
        }

        c.orbitCount = (uint32_t)satOrbits.size() - c.orbitStart;

        // Stamp constIdx on every orbit that belongs to this constellation so
        // updatePositions() can look up highlight/enabled state by orbit index.
        uint32_t ci = (uint32_t)(&c - constellations.data());
        for (uint32_t oi = c.orbitStart; oi < c.orbitStart + c.orbitCount; ++oi)
            satOrbits[oi].constIdx = ci;
    }
    // 67 Right here

    // ── Generate random ground targets for TargetedReflector mode ───────────
    // kNumReflectorTargets random lat/lon points stored as unit ECEF vectors.
    // updatePositions() rotates them to ECI each frame and filters for the
    // night-side terminator zone so mirrors only aim at dark-but-reachable spots.
    for (int ti = 1; ti < kNumReflectorTargets - 1; ++ti)
    {
        // Uniform sampling on sphere: latitude from arcsin of uniform[-1,1],
        // longitude uniform [0, 2π).
        float sinLat = (float)rand() / RAND_MAX * 2.0f - 1.0f;
        float cosLat = sqrtf(std::max(0.0f, 1.0f - sinLat * sinLat));
        float lon = (float)rand() / RAND_MAX * glm::two_pi<float>();
        reflectorTargetsECEF[ti] = glm::vec3(cosLat * cosf(lon), cosLat * sinf(lon), sinLat);
    }
    // Last slot: fixed target at the observer spawn point (67°S, 67°W).
    // Guarantees at least one mirror always aims here when the site is on the night side.
    reflectorTargetsECEF[kNumReflectorTargets - 1] = glm::normalize(glm::vec3(0.2527f, -0.4596f, -0.8205f));

    // Zeroth slot
    // Antartic Station
    reflectorTargetsECEF[kNumReflectorTargets - 1] = glm::normalize(glm::vec3(0, 0, -1.0));

    // ── Safety cap ────────────────────────────────────────────────────────────
    // satInputBuf and satVisibleBuf are allocated for exactly MAX_SATELLITES
    // entries.  Exceeding this causes a buffer overflow in recordCompute()'s
    // memcpy, corrupting heap memory or triggering a GPU fault.  Satellites
    // beyond the cap are silently dropped.
    //
    // Common overflow source: Starlink G1 at 7200 planes × 22 sats = 158,400 —
    // already 58% over the 100,000 limit.  Raise MAX_SATELLITES and resize the
    // GPU buffers (createBuffers) if more capacity is needed.  Alternatively,
    // move orbit computation to a second compute shader so the CPU loop and
    // the host-visible upload buffer are no longer the bottleneck.
    if ((uint32_t)satOrbits.size() > MAX_SATELLITES)
    {
        fprintf(stderr, "[SatelliteSim] Warning: %zu total satellites exceeds "
                        "MAX_SATELLITES=%u; truncating.\n",
                satOrbits.size(), MAX_SATELLITES);
        satOrbits.resize(MAX_SATELLITES);
    }
    // Precompute frame-invariant constants into each SatOrbit so updatePositions()
    // doesn't recompute them every frame (saves sqrt + 4 trig calls per satellite).
    for (SatOrbit &orb : satOrbits)
    {
        orb.R_sat = kEarthRadius + orb.altM;
        orb.meanMot = (float)sqrt(kGM / ((double)orb.R_sat * orb.R_sat * orb.R_sat));
        orb.cosI = cosf(orb.incl);
        orb.sinI = sinf(orb.incl);
        if (!orb.alignTerminator)
        {
            orb.cosRaan = cosf(orb.raan);
            orb.sinRaan = sinf(orb.raan);
        }
    }

    activeSatCount = (uint32_t)satOrbits.size();
    // Mirror normals are now stored in mirrorNormalsBuf (device-local, zeroed at init).
    // satMirrorNormals is kept as an empty placeholder; GPU handles slew state.
}

// ─── updatePositions ──────────────────────────────────────────────────────────
// Recomputes: observer ECI position + ECI→ENU matrix, sun direction,
// and per-satellite geometry + panel attitude (nested per-constellation).
//
// Performance characteristics:
//   This function runs on the CPU main thread every frame, O(N) in satellite
//   count.  Per satellite it executes ~15–20 floating-point operations including
//   double-precision fmod, cosf/sinf, asinf, length, and conditionally cross
//   products for tumbling/sun-tracking attitude modes.
//
//   Approximate wall time on a modern desktop CPU:
//     1,000  sats  →  ~0.1 ms
//    10,000  sats  →  ~1   ms
//   100,000  sats  →  ~10  ms (hits frame budget at 60 Hz)
//
//   For larger constellations the orbit computation should be moved to a
//   dedicated GPU compute pass.  The CPU would then only upload simTime + the
//   ECI→ENU matrix (~120 bytes) rather than the full GpuSatInput array
//   (~6.4 MB at 100k sats).
void SatelliteSim::updatePositions(double t, float dt)
{
    // ── Observer ECI position (rotates with Earth) ────────────────────────────
    // fmod keeps the angle small so float trig precision is maintained at large t.
    // Add the Earth-fixed longitude offset to the GMST angle.
    // kOmegaEarth * t  = Greenwich Meridian Sidereal Time (Earth's rotation since epoch).
    // obsLonRad        = observer's geodetic longitude in the Earth-fixed frame.
    // Together: the observer sits at geodetic (obsLatDeg, obsLonDeg) rotating with Earth.
    // Derive lat/lon from obsDir (canonical state) — stable at all latitudes.
    float sinLat = obsDir.z;
    float cosLat = sqrtf(obsDir.x * obsDir.x + obsDir.y * obsDir.y);
    float obsLonRad = atan2f(obsDir.y, obsDir.x); // safe: cosLat >= 0 always
    float theta = (float)fmod(kOmegaEarth * t + (double)obsLonRad, glm::two_pi<double>());
    // Refresh display caches each frame so UI stays in sync regardless of who moved obsDir.
    obsLatDeg = glm::degrees(asinf(glm::clamp(sinLat, -1.0f, 1.0f)));
    obsLonDeg = glm::degrees(obsLonRad);
    float cosLon = cosf(theta), sinLon = sinf(theta);

    float obsRadius = kEarthRadius + obsTerrainH + obsHeightOffset;
    obsECI = glm::vec3{obsRadius * cosLat * cosLon,
                       obsRadius * cosLat * sinLon,
                       obsRadius * sinLat};

    // ── ECI → ENU basis vectors ───────────────────────────────────────────────
    glm::vec3 east{-sinLon, cosLon, 0.0f};
    glm::vec3 north{-sinLat * cosLon, -sinLat * sinLon, cosLat};
    glm::vec3 up{cosLat * cosLon, cosLat * sinLon, sinLat};

    eci2enuX = glm::vec4(east, 0.0f);
    eci2enuY = glm::vec4(north, 0.0f);
    eci2enuZ = glm::vec4(up, 0.0f);

    // ── Sun direction in ECI (low-accuracy Astronomical Almanac) ─────────────
    double dJ2000 = t / 86400.0;
    double L = fmod(280.46 + 0.9856474 * dJ2000, 360.0);
    double g = fmod(357.528 + 0.9856003 * dJ2000, 360.0);
    double gR = g * (glm::pi<double>() / 180.0);
    double lambdaR = (L + 1.915 * sin(gR) + 0.020 * sin(2.0 * gR)) * (glm::pi<double>() / 180.0);
    double epsR = (23.439 - 0.0000004 * dJ2000) * (glm::pi<double>() / 180.0);

    sunDirECI = glm::normalize(glm::vec3{
        (float)cos(lambdaR),
        (float)(sin(lambdaR) * cos(epsR)),
        (float)(sin(lambdaR) * sin(epsR))});

    glm::vec3 sunENU{
        glm::dot(sunDirECI, east),
        glm::dot(sunDirECI, north),
        glm::dot(sunDirECI, up)};
    sunDirENU = glm::vec4(glm::normalize(sunENU), sunENU.z); // w = sin(elevation)

    // ── Moon direction in ECI (simple circular equatorial orbit) ─────────────
    // Period: 27.3217 days. The moon orbits in the ecliptic plane (~5° tilt);
    // for rendering purposes an equatorial approximation is sufficient.
    static constexpr double kMoonPeriodSec = 27.3217 * 86400.0;
    // Phase offset originally calibrated for 2026-03-30 epoch; at 2036-06-21 the moon
    // will be at a different phase (recalibrate if accurate phase is needed).
    static constexpr double kMoonPhaseOffsetRad = 3.916;
    double moonAngle = fmod(2.0 * glm::pi<double>() * (t) / kMoonPeriodSec + kMoonPhaseOffsetRad,
                            glm::two_pi<double>());
    moonDirECI = glm::vec3{(float)cos(moonAngle), (float)sin(moonAngle), 0.0f};

    // Moon in ENU
    glm::vec3 moonENU_local{
        glm::dot(moonDirECI, east),
        glm::dot(moonDirECI, north),
        glm::dot(moonDirECI, up)};
    // Illuminated fraction = (1 − dot(sunDir, moonDir)) / 2
    // Full moon when moon is opposite the sun; new moon when aligned.
    float moonIllum = (1.0f - glm::dot(sunDirECI, moonDirECI)) * 0.5f;
    moonDirENU = glm::vec4(moonENU_local, moonIllum);

    // ── TargetedReflector: rotate target points to ECI, flag valid ones ─────
    // ECEF unit vectors rotate to ECI by the GMST angle (Earth's sidereal rotation).
    // Same rotation formula used for obsECI — consistent frame.
    //
    // Validity: the whole night side (sunDot < 0).  Why not restrict to the terminator
    // zone?  With targets spread across the full night hemisphere, valid targets appear
    // at DIFFERENT azimuth/elevation positions in the observer's sky as Earth rotates.
    // Restricting to just the terminator (sunDot > -0.5) puts all valid targets in the
    // same narrow azimuth band, making the rotation indistinguishable — they all look
    // "fixed" even though the ECEF rotation is working correctly.
    //
    // With 200 targets across the full night side, ~100 are valid at any moment,
    // spread all the way from the dusk terminator to the dawn terminator.  As Earth
    // rotates under the constellation, different geographic points enter/exit the
    // night side and the flare directions visibly sweep across the sky.
    // ── TargetedReflector targets: ECEF→ECI + validity → upload to GPU ─────────
    // Written to reflectorTargetsMapped (host-coherent); read by sat_orbit.comp.
    {
        float gmst = (float)fmod(kOmegaEarth * t, glm::two_pi<double>());
        float cosG = cosf(gmst), sinG = sinf(gmst);
        GpuReflectorTarget *targets = static_cast<GpuReflectorTarget *>(reflectorTargetsMapped);
        for (int ti = 0; ti < kNumReflectorTargets; ++ti)
        {
            const glm::vec3 &ef = reflectorTargetsECEF[ti];
            glm::vec3 eci = kEarthRadius * glm::vec3(
                                               cosG * ef.x - sinG * ef.y,
                                               sinG * ef.x + cosG * ef.y,
                                               ef.z);
            float sunDot = glm::dot(glm::normalize(eci), sunDirECI);
            targets[ti].posECI = eci;
            targets[ti].valid = (sunDot < 0.0f) ? 1.0f : 0.0f;
        }
    }

    // ── Satellite loop runs on GPU (sat_orbit.comp + sat_flare.comp) ─────────────
    // peakMagnitude is computed in recordCompute() from the previous frame's glowBuf.
    visibleCount = activeSatCount;
    gpuSatCount = activeSatCount;
    loopMs = 0.0f;
}
