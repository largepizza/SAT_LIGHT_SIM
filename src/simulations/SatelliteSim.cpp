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
    createCloudNoisePipeline(ctx);
    createCloudWarpNoisePipeline(ctx); // must run before createCloudMarchDescriptors (binding 9)
    createAuroraNoisePipeline(ctx); // must run before createGlowResources' writes (binding 16)
    createCloudMarchResources(ctx); // images must exist before createGlowResources' writes (bindings 10/11)
    createSceneDepthResources(ctx); // image must exist before createGlowResources' (binding 19),
                                     // createDescriptors' (binding 7) and initStars' (binding 7) writes
    createGlowResources(ctx);
    createDescriptors(ctx);
    createComputePipeline(ctx);
    createOrbitDescriptors(ctx);
    createOrbitPipeline(ctx);
    createCloudMarchDescriptors(ctx); // needs cloudParamsBuf from createGlowResources above
    createCloudMarchPipeline(ctx);
    createSceneDepthDescriptors(ctx); // needs earthElev/earthSpec from createGlowResources above
    createSceneDepthPipeline(ctx);
    createBeamCloudBlockDescriptors(ctx); // needs same cloudParamsBuf/earthClouds/cloudNoise/cloudWarpNoise
    createBeamCloudBlockPipeline(ctx);
    createSkyBgPipeline(ctx);
    createSkyLowResResources(ctx); // resolution scaling — needs skyBgPipeLayout from just above
    createDrawPipeline(ctx);
    updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay); // must run first — initConstellation reads sunDirECI
    initConstellation();
    // C12 follow-up #33: one-time upload of reflectorTargetsECEF[]/RadiusM[] (fixed for the
    // simulation's lifetime once initConstellation() generates them) into their GPU-visible
    // companion buffer — beam_cloud_block.comp reads this every frame, but it never needs
    // refreshing since the CPU arrays themselves never change after this point.
    {
        std::vector<glm::vec4> targetsECEF(kNumReflectorTargets);
        for (int ti = 0; ti < kNumReflectorTargets; ++ti)
            targetsECEF[ti] = glm::vec4(reflectorTargetsECEF[ti], reflectorTargetsRadiusM[ti]);
        memcpy(reflectorTargetsECEFMapped, targetsECEF.data(), sizeof(glm::vec4) * kNumReflectorTargets);
    }
    uploadSatOrbits(ctx); // bake + upload GpuSatOrbit data after orbits are built
    initStars(ctx);

    // Default window chrome sizes — must be set before the first updateWindowChrome()
    // call (buildUI); loadSettings() below may override x/y/w/h with persisted values.
    // settingsChrome defaults above its own 660x420 min (buildSettingsWindow) so it
    // never opens already-clamped-smaller-than-its-own-content on a fresh install.
    settingsChrome.w = 700.0f;
    settingsChrome.h = 480.0f;
    viewControlsChrome.w = 300.0f;
    viewControlsChrome.h = 340.0f;

    loadSettings(); // override defaults with any previously saved values

    // renderScale (just loaded above) may differ from the 1.0 default createSkyLowResResources
    // used a few lines earlier in this function — recreate at the correct persisted size now.
    // Cheap and harmless when unchanged (same one-time startup cost either way).
    destroySkyLowResResources(ctx.device);
    createSkyLowResResources(ctx);

    // Shown by default on first run per showControlsOnStartup (itself persisted); applied
    // after loadSettings() so a saved false sticks. viewControlsChrome.open is intentionally
    // NOT persisted — closing it only lasts for the current run (see buildViewControlsWindow).
    viewControlsChrome.open = showControlsOnStartup;
}

// ─── onResize ─────────────────────────────────────────────────────────────────
void SatelliteSim::onResize(VulkanContext &ctx)
{
    vkDestroyPipeline(ctx.device, skyBgPipeline, nullptr);
    skyBgPipeline = VK_NULL_HANDLE;
    createSkyBgPipeline(ctx);

    // Resolution scaling: low-res target is sized off ctx.swapExtent too, so it needs the same
    // destroy+recreate treatment as skyBgPipeline just above.
    destroySkyLowResResources(ctx.device);
    createSkyLowResResources(ctx);

    vkDestroyPipeline(ctx.device, drawPipeline, nullptr);
    drawPipeline = VK_NULL_HANDLE;
    createDrawPipeline(ctx);

    vkDestroyPipeline(ctx.device, starPipeline, nullptr);
    starPipeline = VK_NULL_HANDLE;
    createStarPipeline(ctx);

    // ── Half-res cloud march targets (C15-perf) — the only swapchain-size-dependent images this
    // class owns; recreate at the new half-extent, then patch the two descriptor sets that point
    // at their views (the sampler is resolution-independent and kept as-is). Safe with no extra
    // synchronization: this app has exactly one frame in flight (single fence, waited on at the
    // top of every drawFrame), matching the unsynchronized pipeline recreation just above.
    vkDestroyImageView(ctx.device, cloudMarchTargetAView, nullptr);
    vkDestroyImage(ctx.device, cloudMarchTargetAImg, nullptr);
    vkFreeMemory(ctx.device, cloudMarchTargetAMem, nullptr);
    vkDestroyImageView(ctx.device, cloudMarchTargetBView, nullptr);
    vkDestroyImage(ctx.device, cloudMarchTargetBImg, nullptr);
    vkFreeMemory(ctx.device, cloudMarchTargetBMem, nullptr);
    cloudMarchTargetAImg = cloudMarchTargetBImg = VK_NULL_HANDLE;
    cloudMarchTargetAMem = cloudMarchTargetBMem = VK_NULL_HANDLE;
    cloudMarchTargetAView = cloudMarchTargetBView = VK_NULL_HANDLE;
    createCloudMarchResources(ctx); // recreates images/views; leaves both in SHADER_READ_ONLY_OPTIMAL

    VkDescriptorImageInfo skyAInfo{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo skyBInfo{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet skyWrites[2] = {};
    skyWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, skyDescSet, 10, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &skyAInfo, nullptr, nullptr};
    skyWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, skyDescSet, 11, 0, 1,
                    VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &skyBInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 2, skyWrites, 0, nullptr);

    VkDescriptorImageInfo storageAInfo{VK_NULL_HANDLE, cloudMarchTargetAView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo storageBInfo{VK_NULL_HANDLE, cloudMarchTargetBView, VK_IMAGE_LAYOUT_GENERAL};
    VkWriteDescriptorSet computeWrites[2] = {};
    computeWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 5, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storageAInfo, nullptr, nullptr};
    computeWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 6, 0, 1,
                        VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &storageBInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 2, computeWrites, 0, nullptr);

    // C12 follow-up #33: descSet (satellite draw pipeline) also holds bindings 5/6 pointing at
    // these same views (sat_point.frag's cloud occlusion) — needs the same refresh as skyDescSet
    // above, or it would keep pointing at the image views just destroyed.
    VkDescriptorImageInfo satCloudAInfo{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo satCloudBInfo{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet satCloudWrites[2] = {};
    satCloudWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, 5, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &satCloudAInfo, nullptr, nullptr};
    satCloudWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, 6, 0, 1,
                         VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &satCloudBInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 2, satCloudWrites, 0, nullptr);

    // ── Shared scene depth — same swapchain-size dependency, same destroy/recreate/patch dance.
    // Two sets reference it: its own (as a storage image, for writing) and skyDescSet binding 20
    // (as a sampled image, for reading). Miss either and the next frame samples a destroyed view.
    vkDestroyImageView(ctx.device, sceneDepthView, nullptr);
    vkDestroyImage(ctx.device, sceneDepthImg, nullptr);
    vkFreeMemory(ctx.device, sceneDepthMem, nullptr);
    sceneDepthImg = VK_NULL_HANDLE;
    sceneDepthMem = VK_NULL_HANDLE;
    sceneDepthView = VK_NULL_HANDLE;
    createSceneDepthResources(ctx); // recreates image/view; leaves it in SHADER_READ_ONLY_OPTIMAL

    VkDescriptorImageInfo depthStorageInfo{VK_NULL_HANDLE, sceneDepthView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo depthSampledInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet depthWrites[5] = {};
    depthWrites[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sceneDepthDescSet, 2, 0, 1,
                      VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &depthStorageInfo, nullptr, nullptr};
    depthWrites[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 13, 0, 1,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthSampledInfo, nullptr, nullptr};
    depthWrites[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, skyDescSet, 19, 0, 1,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthSampledInfo, nullptr, nullptr};
    // The two point-draw sets read it as well (terrain occlusion at renderScale < 1.0). Easy to
    // forget — the same omission was caught late once before, for the cloud targets' bindings 5/6.
    depthWrites[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, descSet, 7, 0, 1,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthSampledInfo, nullptr, nullptr};
    depthWrites[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, starDescSet, 7, 0, 1,
                      VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depthSampledInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 5, depthWrites, 0, nullptr);
}

// ─── recordCompute ────────────────────────────────────────────────────────────
// Reads ctx.timestampMs (resolved by App::drawFrame right after this frame's fence wait,
// i.e. before this call — see VulkanContext::resolveTimestamps) and EMA-smooths the eight
// pass-duration buckets into gpuMsSmoothed[].  VulkanContext::kTimestampCount carries the
// authoritative slot table; this function, kPerfLabels[] in SatelliteSimUI.cpp, and the JSON
// keys in savePerfSnapshot() must all stay in sync with it and with each other.
void SatelliteSim::updateGpuTimingStats(VulkanContext &ctx)
{
    if (!ctx.timestampsReady)
        return;
    const double *t = ctx.timestampMs;
    // The pipeline-unification pass added two buckets and removed one: beam cloud block (whose
    // cost used to be folded silently into orbit compute, which is why that bucket always read a
    // suspiciously flat 0.37-0.59 ms) and scene depth, the new shared terrain-depth pass; cloud
    // shadow map went away when its dispatch was folded into cloud_march.comp.
    float raw[8] = {
        (float)(t[1] - t[0]), // scene depth compute (shared terrain/ocean depth)
        (float)(t[2] - t[1]), // beam cloud block compute (C12 follow-up #33)
        (float)(t[3] - t[2]), // orbit compute
        (float)(t[4] - t[3]), // cloud march compute (incl. the per-pixel cloud shadow)
        (float)(t[5] - t[4]), // flare compute
        (float)(t[6] - t[5]), // sky/terrain/ocean bg + cloud composite fragment shader
        (float)(t[7] - t[6]), // satellite points + star draw
        (float)(t[8] - t[7]), // UI overlay
    };
    const float kAlpha = 0.1f; // low-pass so the HUD numbers don't flicker frame to frame
    for (int i = 0; i < 8; ++i)
        gpuMsSmoothed[i] = glm::mix(gpuMsSmoothed[i], raw[i], kAlpha);
    gpuMsTotalSmoothed = glm::mix(gpuMsTotalSmoothed, (float)(t[8] - t[0]), kAlpha);
}

void SatelliteSim::recordCompute(VkCommandBuffer cmd, VulkanContext &ctx, float dt)
{
    updateGpuTimingStats(ctx);

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

    // ── Sky-background sun-glare gate (stars / Milky Way, space only) ──────────
    // updateStars()'s atmFrac fade (see that function) lets the day/night sky-brightness gate
    // relax toward "always visible" once truly clear of the atmosphere — correct in principle
    // (no air left to scatter sunlight into a blue daytime sky) but previously relaxed all the
    // way to a flat 1.0 regardless of the sun's position, so stars/Milky Way stayed fully visible
    // in space even staring straight at the sun, or in full unshielded sunlight. Real glare still
    // applies: this computes a single per-frame, whole-screen target — not per-pixel, since it's
    // meant to blank the ENTIRE sky background, not just fade near the sun the way the existing
    // localized sunGlareSuppress halo in sat_sky.frag's Milky Way section does — and eases toward
    // it so a quick look-away doesn't snap the sky instantly back.
    {
        glm::vec3 sunCam = glm::mat3(camera.viewMatrix()) * glm::vec3(sunDirENU);
        float tanHalfFov = tanf(glm::radians(camera.fovYDeg) * 0.5f);
        float aspect = (float)ctx.swapExtent.width / (float)ctx.swapExtent.height;
        bool sunOnScreen = false;
        if (sunCam.z < -0.001f)
        {
            float ndcX = sunCam.x / (-sunCam.z) / (tanHalfFov * aspect);
            float ndcY = -sunCam.y / (-sunCam.z) / tanHalfFov;
            sunOnScreen = (fabsf(ndcX) <= 1.0f && fabsf(ndcY) <= 1.0f);
        }

        // Observer's own local sun elevation — same day/night test updateStars()'s nightFactor
        // already uses below, valid at any altitude this sim reaches (ENU is defined at the
        // observer's actual position, so this tracks a real eclipse/shadow crossing reasonably
        // well without a separate Earth-shadow ray test).
        bool sunlit = sunDirENU.w > 0.0f;
        float glareTarget = sunOnScreen ? 0.0f : (sunlit ? sunlitBgVisibility : 1.0f);

        // Asymmetric hysteresis: glare hits fast (sensor/eyes overwhelmed almost immediately),
        // recovery is slow (night-vision-style readaptation) — avoids an instant on/off pop
        // either direction while still feeling responsive when the sun swings into view.
        const float kSkyGlareOnRate = 3.0f;  // ~0.3s to mostly reach target when dimming
        const float kSkyGlareOffRate = 0.4f; // ~2.5s to mostly reach target when recovering
        float rate = (glareTarget < skyGlareEased) ? kSkyGlareOnRate : kSkyGlareOffRate;
        skyGlareEased = glm::mix(skyGlareEased, glareTarget, 1.0f - expf(-dt * rate));
    }

    updateLightPollutionDome();
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

    // Read previous frame's reflectBeamsBuf — diagnostic for C12 (is anything actually being
    // written, and how far is the nearest one) — AND (C12 follow-up #41) the source signal for
    // the beam-proximity sky-glow wash below. Same one-frame-stale, HOST_COHERENT idiom as
    // peakMagnitude above.
    {
        const GpuReflectBeams *rb = static_cast<const GpuReflectBeams *>(reflectBeamsMapped);
        int count = std::min((int)rb->beamCount, kMaxActiveBeams);
        float nearest = -1.0f;
        for (int s = 0; s < count; ++s)
        {
            // C12 follow-up #41: point-to-segment distance to the beam's actual 3D LINE (target
            // to satellite), not just its ground endpoint (`length(targetENU)`, the old formula)
            // — climbing up alongside a long beam away from the ground previously read as
            // "getting farther from the beam" even while staying right next to its line. Same
            // formula as cloud_march.comp's own obsToBeamDist, simplified: these vectors are
            // already observer-relative (origin = observer), so no obsPos subtraction is needed.
            const glm::vec3 &tE = rb->entries[s].targetENU;
            const glm::vec3 &sE = rb->entries[s].satENU;
            float slantRangeM = glm::length(sE - tE);
            glm::vec3 dirUp = (slantRangeM > 1.0f) ? (sE - tE) / slantRangeM : glm::vec3(0, 0, 1);
            float t = glm::clamp(-glm::dot(tE, dirUp), 0.0f, slantRangeM);
            float d = glm::length(tE + dirUp * t);
            if (nearest < 0.0f || d < nearest)
                nearest = d;
        }
        lastActiveBeamCount = count;
        lastNearestBeamDistM = nearest;

        // C12 follow-up #41: ready-to-use [0,1] sky-glow wash value — smoothstepped from the
        // corrected nearest-beam-line distance above, using the SAME radius slider
        // (beamNearFieldFadeM) that already controls the tube's own near-field crossfade in
        // cloud_march.comp, so both fades share one tunable. Hand-rolled smoothstep (no
        // glm::smoothstep used elsewhere in this file).
        float x = glm::clamp(lastNearestBeamDistM / std::max(beamNearFieldFadeM, 1.0f), 0.0f, 1.0f);
        float sstep = x * x * (3.0f - 2.0f * x);
        beamProximityGlow = (lastNearestBeamDistM >= 0.0f) ? (1.0f - sstep) : 0.0f;
    }

    // Read previous frame's beamGlowDomeBuf (C12 follow-up #31) — one-frame-stale, same idiom as
    // glowBuf/reflectBeamsBuf above. sat_orbit.comp stores raw atomicMax'd uint bit-patterns
    // (floatBitsToUint on the GPU side), so reinterpret via memcpy rather than a direct float
    // cast, matching glowBuf's own pattern.
    {
        const uint32_t *bgd = static_cast<const uint32_t *>(beamGlowDomeMapped);
        for (int i = 0; i < kNumBeamGlowSectors; ++i)
        {
            memcpy(&beamGlowDomeAz[i], &bgd[i], sizeof(float));
        }
    }

    // Read previous frame's tracked-selection position, same one-frame-stale idiom as
    // peakMagnitude above. On the frame a selection is first made (or changed), this still
    // holds the prior (possibly default/zero) value — the copy in the dispatch section below
    // captures the freshly-selected satellite's real data for the FIRST time this frame, so the
    // panel settles onto the correct tracked position within ~2 frames of the click, not instantly.
    if (selectedSatIndex >= 0)
    {
        const GpuSatVisible *pv = static_cast<const GpuSatVisible *>(pickedVisibleMapped);
        lastPickedSkyDir = pv->skyDir;
        lastPickedFlare = pv->flareIntensity;
    }

    // ── Observer terrain height + cloud params UBO fill (relocated from recordDraw) ─────────
    // cloud_march.comp's dispatch below needs fresh CloudParams/obsEffH data, but it must run
    // before the render pass begins (recordCompute), which is BEFORE recordDraw used to compute
    // either of these. Both are pure CPU/mapped-memory state with no dependency on anything else
    // in recordDraw, so relocating them here is a straightforward move — and both must run before
    // the `activeSatCount == 0` early-out above's return would otherwise skip clouds entirely
    // whenever no satellites are active. Note: pc.obsECEFDir.w (used by both this dispatch and
    // recordDraw's SatDrawPC) is obsHeightOffset ONLY, not obsTerrainH+obsHeightOffset — obsEffH
    // below computes the max explicitly rather than trusting that combination.
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

    // ── City-detail world-fixed offset ────────────────────────────────────────────────────────
    // sat_sky.frag's "City detail texture blend" adds this (cityOffsetEastM/NorthM, packed into
    // CloudParams pad1/pad2) straight onto hitPt.xy to cancel that coordinate's observer-relative
    // drift with a plain translation. hitPt.xy's drift for any point near the observer is, to
    // leading order, just a uniform shift equal to the observer's own north/east motion — moving
    // the reference frame doesn't rotate nearby points relative to each other, it shifts them all
    // together — so tracking the observer's own cumulative displacement is sufficient; no basis
    // reconstruction or grid-snapping needed (an earlier version tried exactly-fixed local ENU
    // bases snapped to a grid, but re-deriving the basis at each snap silently rotated the axes a
    // little, not just translated them, causing a visible pop at every snap instead of the
    // intended seamless tile-period jump).
    {
        double latRad = (double)glm::radians(obsLatDeg);
        double lonRad = (double)glm::radians(obsLonDeg);
        if (!cityOffsetInit)
        {
            cityPrevObsLatRad = latRad;
            cityPrevObsLonRad = lonRad;
            cityOffsetInit = true;
        }
        double dLat = latRad - cityPrevObsLatRad;
        double dLon = lonRad - cityPrevObsLonRad;
        if (dLon > glm::pi<double>())  dLon -= glm::two_pi<double>(); // antimeridian wrap guard
        if (dLon < -glm::pi<double>()) dLon += glm::two_pi<double>();
        double cosLat = std::max(0.05, cos(latRad)); // guards the /cosLat below near the poles
        cityOffsetNorthM += dLat * (double)kEarthRadius;
        cityOffsetEastM  += dLon * (double)kEarthRadius * cosLat;
        cityPrevObsLatRad = latRad;
        cityPrevObsLonRad = lonRad;
    }

    if (cloudParamsMapped)
    {
        GpuCloudParams cp{};
        cp.coverage = cloudCoverage;
        cp.density = cloudDensity;
        cp.driftRate = cloudDriftRate;
        cp.sunGain = cloudSunGain;
        cp.sunGainZenith = cloudSunGainZenith;
        cp.ambientGain = cloudAmbientGain;
        cp.hgG = cloudHgG;
        cp.marchSteps = cloudMarchSteps;
        cp.lightSteps = cloudLightSteps;
        cp.extinctionCoeff = extinctionCoeff;
        cp.cirrusWindAngle = glm::radians(cloudCirrusWindDeg);
        cp.cirrusStretch = cloudCirrusStretch;
        cp.airglowGain = airglowGain;
        cp.airglowGreenGain = airglowGreenGain;
        cp.airglowRedGain = airglowRedGain;
        cp.airglowSodiumGain = airglowSodiumGain;
        cp.shadowMaxDistM = cloudShadowMaxDistM;
        cp.maxRenderDistM = cloudMaxRenderDistM;
        cp.viewSamplesMin = viewSamplesMin;
        cp.viewSamplesMax = viewSamplesMax;
        cp.lightSamples = lightSamples;
        cp.oceanSeaOctaves = oceanSeaOctaves;
        cp.oceanDetailOctaves = oceanDetailOctaves;
        cp.oceanReflSamples = oceanReflSamples;
        cp.moonGain = moonGain;
        cp.pad1 = (float)cityOffsetEastM;  // repurposed: city-detail world-fixed east offset (m)
        cp.pad2 = (float)cityOffsetNorthM; // repurposed: city-detail world-fixed north offset (m)
        cp.cloudNightAmbientGain = cloudNightAmbientGain;
        cp.cloudBaseVariance = cloudBaseVariance;
        cp.cloudErosionEdge = cloudErosionEdge;
        cp.cloudErosionCore = cloudErosionCore;
        cp.stormStrength = stormStrength;
        cp.auroraGain = auroraGain;
        cp.auroraCloudGain = auroraCloudGain;
        cp.auroraGroundGain = auroraGroundGain;
        cp.auroraCoverageFreq = auroraCoverageFreq;
        cp.auroraCoverageAzFreq = auroraCoverageAzFreq;
        cp.auroraCoverageDriftRate = auroraCoverageDriftRate;
        cp.auroraShimmerRate = auroraShimmerRate;
        cp.mwBasisRow0 = glm::vec4(mwRow0, 1.0f); // .w = milky way gain (fixed; no longer user-tunable)
        cp.mwBasisRow1 = glm::vec4(mwRow1, 0.0f);
        cp.mwBasisRow2 = glm::vec4(mwRow2, 0.0f);
        cp.cloudPhase = (float)fmod((double)cloudDriftRate * (simDayJ2000 * 86400.0 + simSecInDay),
                                    glm::two_pi<double>());
        // Layer 0: low cloud / stratus shell
        cp.layers[0] = {cloudBaseAltM, 1.0f, 0.80f, 0.0f, 1.0f, 1.0f, 1.0f, 0.0f};
        // Layer 1: high cirrus shell
        cp.layers[1] = {cloudTopAltM, 2.0f, 0.15f, 2.0f, 0.5f, 0.4f, 1.0f, 0.0f};
        // Layers 2-3: unused
        cp.layers[2] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        cp.layers[3] = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        memcpy(cloudParamsMapped, &cp, sizeof(cp));
    }

    // ── Dispatch: sat_orbit.comp — orbital mechanics + attitude ───────────────────────────────
    // Moved to run BEFORE cloud_march.comp (C12 follow-up #22) — it used to run after cloud_march/
    // cloud_shadow, which meant cloud_march.comp's Reflect-Orbital beam sky glow always read
    // ReflectBeamsBuf as written by the PREVIOUS frame's sat_orbit.comp (one frame stale). Harmless
    // for observer-independent data, but satENU/targetENU are METERS offsets in the observer's ENU
    // basis AT WRITE TIME — when the observer moves, a stale offset no longer matches this frame's
    // fresh obsPos/basis when cloud_march.comp uses it, producing a visible lag proportional to how
    // far the observer moved that frame (imperceptible at walking speed, clearly visible at "boost"
    // movement). Running unconditionally here (even when activeSatCount==0, a legal 0-workgroup
    // no-op dispatch) — before the `if (activeSatCount==0) return` check below — means
    // cloud_march.comp always reads THIS frame's fresh beam data instead.
    // Build enabled / highlight masks from constellation config (one bit per constellation).
    uint32_t enabledMask = 0, highlightMask = 0;
    for (uint32_t ci = 0; ci < (uint32_t)constellations.size() && ci < 32; ++ci)
    {
        if (constellations[ci].enabled)
            enabledMask |= (1u << ci);
        if (constellations[ci].highlight)
            highlightMask |= (1u << ci);
    }

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
    orbitPc.beamGain = beamGain;
    orbitPc.mirrorSlewDegPerSec = mirrorSlewDegPerSec; // C12 follow-up #20

    // ── Dispatch: scene_depth.comp — shared terrain/ocean depth (pipeline unification) ──────────
    // Runs FIRST. Everything downstream that needs to know "is this pixel's view blocked by the
    // ground" reads the result instead of re-deriving it: cloud_march.comp's beam occlusion (which
    // used to march the DEM per beam per pixel), and — from the next step — every volumetric
    // layer's own far bound. Depends on nothing else this frame, only the camera.
    {
        SceneDepthPC dpc{};
        dpc.skyView = camera.viewMatrix();
        dpc.fovYRad = glm::radians(camera.fovYDeg);
        // ALWAYS the true swapchain aspect, never a render-scaled one — this buffer is consumed
        // at several different resolutions and must be the same function of normalized screen UV
        // at all of them.
        dpc.aspect = (float)ctx.swapExtent.width / (float)ctx.swapExtent.height;
        dpc.debugDisableMask = debugDisableMask;
        dpc.obsECEFDir = glm::vec4(obsDir, obsHeightOffset);

        uint32_t halfW = (ctx.swapExtent.width + 1) / 2;
        uint32_t halfH = (ctx.swapExtent.height + 1) / 2;

        // Pre-dispatch: SHADER_READ_ONLY_OPTIMAL → GENERAL.
        // srcStage includes COMPUTE as well as FRAGMENT — unlike cloudMarchTargetA/B (read only by
        // fragment shaders), this image is also read by cloud_march.comp, so the write-after-read
        // hazard against the PREVIOUS frame's compute read has to be covered. Benign in practice
        // with one frame in flight plus the fence wait, but sync-validation flags its absence.
        ctx.imageBarrier(cmd, sceneDepthImg,
                         VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, sceneDepthPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                sceneDepthPipeLayout, 0, 1, &sceneDepthDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, sceneDepthPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(dpc), &dpc);
        vkCmdDispatch(cmd, (halfW + 15) / 16, (halfH + 15) / 16, 1);

        // Post-dispatch: GENERAL → SHADER_READ_ONLY_OPTIMAL. dstStage covers BOTH consumer kinds —
        // cloud_march.comp (compute, later this call) and sat_sky.frag / the point draws
        // (fragment, later this frame in the render pass).
        ctx.imageBarrier(cmd, sceneDepthImg,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 1);

    // Zero reflectBeamsBuf so this frame's orbit dispatch starts with an empty sector
    // selection (same rationale as the glowBuf fill below — atomicMax needs a known-zero start).
    vkCmdFillBuffer(cmd, reflectBeamsBuf, 0, sizeof(GpuReflectBeams), 0);
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = reflectBeamsBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // Zero beamGlowDomeBuf (C12 follow-up #31) — same rationale as reflectBeamsBuf above,
    // atomicMax needs a known-zero start each frame.
    vkCmdFillBuffer(cmd, beamGlowDomeBuf, 0, sizeof(float) * kNumBeamGlowSectors, 0);
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_SHADER_WRITE_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = beamGlowDomeBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_TRANSFER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // ── Dispatch: beam_cloud_block.comp — per-target cloud occlusion (C12 follow-up #33) ────────
    // Must run BEFORE sat_orbit.comp below, which reads beamCloudBlockBuf while writing beams.
    // 201 targets, no per-frame zero-fill needed (every thread owns and fully overwrites its own
    // index, no atomics — see the buffer's own member comment).
    //
    // Knockout bit 512 skips the dispatch itself (not just its consumers — bit 128 does that).
    // Skipping leaves beamCloudBlockBuf holding the previous frame's values rather than garbage,
    // since every thread fully overwrites its own slot; same convention bit 1024 uses for the
    // scene depth pass.
    if ((debugDisableMask & 512u) == 0u)
    {
        BeamCloudBlockPC bpc{};
        bpc.waveTime = (float)(simSecInDay * 1.0);
        bpc.cloudPhase = (float)fmod((double)cloudDriftRate * (simDayJ2000 * 86400.0 + simSecInDay),
                                     glm::two_pi<double>());

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, beamCloudBlockPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                beamCloudBlockPipeLayout, 0, 1, &beamCloudBlockDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, beamCloudBlockPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(bpc), &bpc);
        vkCmdDispatch(cmd, (kNumReflectorTargets + 63) / 64, 1, 1);

        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = beamCloudBlockBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }
    // Written unconditionally, including on the knockout-skipped path — the bucket then reads ~0,
    // which is exactly the measurement the knockout is there to produce.
    ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 2);

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
    // Barrier: sat_orbit.comp writes reflectBeamsBuf → read THIS frame by cloud_march.comp
    // (compute, right below) and sat_sky.frag (fragment, later in the render pass) — include both
    // stages now so downstream consumers don't need to revisit this barrier.
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = reflectBeamsBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }
    // Barrier: sat_orbit.comp writes beamGlowDomeBuf (C12 follow-up #31) → read THIS frame by
    // sat_flare.comp (compute) and sat_sky.frag's Milky Way section (fragment) — same scope as
    // reflectBeamsBuf's barrier above, same two consumer stage types.
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = beamGlowDomeBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }
    ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 3);

    // ── Dispatch: cloud_march.comp — half-resolution cloud/cirrus march (C15-perf) ──────────
    // Runs at half ctx.swapExtent, writing cloudMarchTargetA/B; sat_sky.frag samples them
    // (skyDescSet bindings 10/11) in place of the old inline cirrusMarch()/cloudMarch() calls.
    {
        CloudMarchPC cpc{};
        cpc.skyView = camera.viewMatrix();
        cpc.fovYRad = glm::radians(camera.fovYDeg);
        cpc.aspect = (float)ctx.swapExtent.width / (float)ctx.swapExtent.height;
        cpc.waveTime = (float)(simSecInDay * 1.0);
        cpc.obsEffH = std::max(obsTerrainH, obsHeightOffset);
        cpc.sunDirENU = sunDirENU;
        cpc.moonDirENU = moonDirENU;
        cpc.obsECEFDir = glm::vec4(obsDir, obsHeightOffset);
        cpc.debugDisableMask = debugDisableMask; // aurora knockout toggle now lives here too
        cpc.beamMaxRangeM = beamMaxRangeM; // C12 follow-up #6
        cpc.showBeamDebugRays = showBeamDebugRays ? 1u : 0u; // C12 follow-up #12
        cpc.beamSkyGlowGain = beamSkyGlowGain; // C12 follow-up #17
        cpc.daySuppression = daySuppression; // C12 follow-up #28 — same ratio sat_flare.comp uses for satellites/stars
        cpc.beamExtinctionMult = beamExtinctionMult; // C12 follow-up #29
        cpc.beamNearFieldFadeM = beamNearFieldFadeM; // C12 follow-up #40
        // C12 follow-up #39: cpc.beamGlowBleedGain removed — the near-field bleed/march it drove
        // in this shader was removed entirely; see buildSatDrawPC() for its new home.

        uint32_t halfW = (ctx.swapExtent.width + 1) / 2;
        uint32_t halfH = (ctx.swapExtent.height + 1) / 2;

        // Pre-dispatch: both targets are left in SHADER_READ_ONLY_OPTIMAL after the previous
        // frame's post-dispatch barrier below (or by createCloudMarchResources on the first frame
        // / after an onResize recreation) — transition back to GENERAL, required for storage-image
        // writes (imageStore).
        ctx.imageBarrier(cmd, cloudMarchTargetAImg,
                         VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);
        ctx.imageBarrier(cmd, cloudMarchTargetBImg,
                         VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, cloudMarchPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                cloudMarchPipeLayout, 0, 1, &cloudMarchDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, cloudMarchPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                           0, sizeof(cpc), &cpc);
        vkCmdDispatch(cmd, (halfW + 15) / 16, (halfH + 15) / 16, 1);

        // Post-dispatch: transition both targets back to SHADER_READ_ONLY_OPTIMAL for
        // sat_sky.frag to sample. Explicit layout-transition barriers — the render pass's
        // existing VK_SUBPASS_EXTERNAL dependency (used for glowBuf) has no layout fields and
        // cannot perform a layout transition on its own.
        ctx.imageBarrier(cmd, cloudMarchTargetAImg,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
        ctx.imageBarrier(cmd, cloudMarchTargetBImg,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    }
    ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 4);

    // (cloud_shadow.comp dispatched here — a fixed 128x128 observer-centred tangent-plane grid,
    //  plus the texel-snapping machinery it needed to stop shadows swimming as the observer
    //  moved. Deleted in the pipeline-unification pass: cloud_march.comp now marches the shadow
    //  per pixel from the terrain hit point the scene-depth pass already found, which is sharper
    //  near the camera, correct from any altitude, unbounded in range, and has nothing to snap.
    //  Its timestamp bucket went away with it.)

    if (activeSatCount == 0)
    {
        // sat_flare.comp below is skipped this frame — write the same timestamp into its slot so
        // updateGpuTimingStats() sees a zero-duration bucket next frame instead of stale or
        // unavailable query data. scene_depth/beam_cloud_block/sat_orbit/cloud_march above already
        // ran unconditionally this frame (sat_orbit.comp with 0 satellite workgroups when
        // applicable — a legal no-op dispatch) and got their own real timestamps, so only the
        // flare slot needs a placeholder here. See C12 follow-up #22 for why sat_orbit.comp now
        // runs before this check at all (it used to run after it, alongside flare).
        ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 5);
        return;
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

    // ── Dispatch: sat_flare.comp — lighting + visibility ──────────────────────
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
    pc.moonSuppression = moonSuppression;
    pc.moonDirECI = moonDirECI; // computed in updatePositions(), called earlier this frame
    pc.extinctionCoeff = extinctionCoeff;

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, compPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                            compPipeLayout, 0, 1, &descSet, 0, nullptr);
    vkCmdPushConstants(cmd, compPipeLayout, VK_SHADER_STAGE_COMPUTE_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, (activeSatCount + 63) / 64, 1, 1);

    // Barrier: sat_flare.comp writes satVisibleBuf → vertex shader reads it (and, when a satellite
    // is selected, the tiny per-frame pick-tracking copy just below also reads it via transfer).
    {
        VkBufferMemoryBarrier bmb{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
        bmb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
        bmb.dstAccessMask = VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT;
        bmb.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        bmb.buffer = satVisibleBuf;
        bmb.offset = 0;
        bmb.size = VK_WHOLE_SIZE;
        vkCmdPipelineBarrier(cmd,
                             VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                             VK_PIPELINE_STAGE_VERTEX_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
                             0, 0, nullptr, 1, &bmb, 0, nullptr);
    }

    // Selected-satellite tracking: mirror just that one 32-byte entry into pickedVisibleBuf so
    // next frame's buildUI can reproject it (see the one-frame-stale read near peakMagnitude
    // above). No-op — no command recorded at all — when nothing is selected.
    if (selectedSatIndex >= 0 && selectedSatIndex < (int)activeSatCount)
    {
        VkBufferCopy pickRegion{};
        pickRegion.srcOffset = (VkDeviceSize)selectedSatIndex * sizeof(GpuSatVisible);
        pickRegion.dstOffset = 0;
        pickRegion.size = sizeof(GpuSatVisible);
        vkCmdCopyBuffer(cmd, satVisibleBuf, pickedVisibleBuf, 1, &pickRegion);
    }
    ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 5);
}

// ─── projectSkyDirToScreen ────────────────────────────────────────────────────
// Pure camera geometry — mirrors sat_point.vert's projection exactly
// (shaders/sat_point.vert:34,47,60-62) so CPU-side picking/tracking agrees pixel-for-pixel
// with what's actually rendered. No orbital mechanics here, so unlike the GPU orbit/attitude
// math this carries little drift risk from being hand-duplicated in C++.
bool SatelliteSim::projectSkyDirToScreen(const glm::vec3 &skyDir, float screenW, float screenH,
                                         float &outX, float &outY) const
{
    glm::vec3 cam = glm::vec3(camera.viewMatrix() * glm::vec4(skyDir, 0.0f));
    if (cam.z >= -0.001f)
        return false; // behind camera — same threshold sat_point.vert uses

    float tanHalfFov = tanf(glm::radians(camera.fovYDeg) * 0.5f);
    float aspect = screenW / screenH;
    float ndcX = cam.x / -cam.z / (tanHalfFov * aspect);
    float ndcY = -cam.y / -cam.z / tanHalfFov;

    outX = (ndcX * 0.5f + 0.5f) * screenW;
    outY = (ndcY * 0.5f + 0.5f) * screenH;
    return true;
}

// ─── pickSatelliteAt ───────────────────────────────────────────────────────────
// One-shot click hit-test. Copies satVisibleBuf (device-local) back to a transient
// host-visible staging buffer sized to activeSatCount (not MAX_SATELLITES, so cost scales
// with what's actually simulated — a few MB at the current constellation roster), then scans
// it on the CPU for the nearest currently-visible satellite within its own hit radius. The
// synchronous stall from ctx.beginOneTimeCommands()/endOneTimeCommands() is fine here — this
// only runs once per user click, never per frame (contrast the tiny per-frame tracking copy
// in recordCompute above, which deliberately avoids any such stall).
int SatelliteSim::pickSatelliteAt(float clickX, float clickY, float screenW, float screenH)
{
    if (activeSatCount == 0 || !ctx_)
        return -1;

    VulkanContext &ctx = *ctx_;
    VkDeviceSize copySize = (VkDeviceSize)activeSatCount * sizeof(GpuSatVisible);

    VkBuffer stagingBuf = VK_NULL_HANDLE;
    VkDeviceMemory stagingMem = VK_NULL_HANDLE;
    ctx.createBuffer(copySize, VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stagingBuf, stagingMem);

    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    VkBufferCopy region{};
    region.srcOffset = 0;
    region.dstOffset = 0;
    region.size = copySize;
    vkCmdCopyBuffer(cmd, satVisibleBuf, stagingBuf, 1, &region);
    ctx.endOneTimeCommands(cmd);

    void *mapped = nullptr;
    vkMapMemory(ctx.device, stagingMem, 0, copySize, 0, &mapped);
    const GpuSatVisible *entries = static_cast<const GpuSatVisible *>(mapped);

    constexpr float kMinHitRadiusPx = 8.0f; // dim/tiny points stay clickable

    int best = -1;
    float bestDist = 0.0f;
    for (uint32_t i = 0; i < activeSatCount; ++i)
    {
        const GpuSatVisible &v = entries[i];
        if (v.flareIntensity <= 0.0f)
            continue;
        float sx, sy;
        if (!projectSkyDirToScreen(v.skyDir, screenW, screenH, sx, sy))
            continue;
        float dx = sx - clickX, dy = sy - clickY;
        float dist = sqrtf(dx * dx + dy * dy);
        float hitRadius = std::max(v.angularSize * 0.5f, kMinHitRadiusPx);
        if (dist <= hitRadius && (best < 0 || dist < bestDist))
        {
            best = (int)i;
            bestDist = dist;
        }
    }

    vkUnmapMemory(ctx.device, stagingMem);
    vkDestroyBuffer(ctx.device, stagingBuf, nullptr);
    vkFreeMemory(ctx.device, stagingMem, nullptr);

    return best;
}

// ─── formatSelectedSatInfo ─────────────────────────────────────────────────────
// Fills selInfoLine[] from static, CPU-resident orbital-element data (satOrbits/constellations/
// satTypes) — call once when selectedSatIndex changes, not every frame; nothing here is
// per-frame dynamic (only screen position is, handled separately via lastPickedSkyDir).
void SatelliteSim::formatSelectedSatInfo()
{
    if (selectedSatIndex < 0 || selectedSatIndex >= (int)satOrbits.size())
    {
        for (auto &line : selInfoLine)
            line[0] = '\0';
        return;
    }

    const SatOrbit &orb = satOrbits[selectedSatIndex];
    const char *constName = "?";
    const char *typeName = "?";
    int localId = selectedSatIndex;
    if (orb.constIdx < constellations.size())
    {
        const ConstellationConfig &c = constellations[orb.constIdx];
        constName = c.name.c_str();
        localId = selectedSatIndex - (int)c.orbitStart;
        if (c.typeIdx < satTypes.size())
            typeName = satTypes[c.typeIdx].name.c_str();
    }

    float altKm = orb.altM / 1000.0f;
    float inclDeg = glm::degrees(orb.incl);
    float periodMin = (orb.meanMot > 0.0f) ? (2.0f * glm::pi<float>() / orb.meanMot) / 60.0f : 0.0f;

    snprintf(selInfoLine[0], sizeof(selInfoLine[0]), "%s #%d", constName, localId);
    snprintf(selInfoLine[1], sizeof(selInfoLine[1]), "%s", typeName);
    snprintf(selInfoLine[2], sizeof(selInfoLine[2]), "Alt: %.0f km", altKm);
    snprintf(selInfoLine[3], sizeof(selInfoLine[3]), "Incl: %.1f deg", inclDeg);
    if (orb.alignTerminator)
        snprintf(selInfoLine[4], sizeof(selInfoLine[4]), "RAAN: sun-sync (precessing)");
    else
        snprintf(selInfoLine[4], sizeof(selInfoLine[4]), "RAAN: %.1f deg", glm::degrees(orb.raan));
    snprintf(selInfoLine[5], sizeof(selInfoLine[5]), "Period: %.1f min", periodMin);
}

// ─── recordDraw ───────────────────────────────────────────────────────────────
// Every field the sky/satellite/star pipelines' push constant needs — shared by recordPrePass
// (low-res sky background, when scaled) and recordDraw (satellites/stars always; sky background
// too when renderScale==1.0), so the two never drift out of sync on what they push. targetExtent
// is THIS draw's own actual framebuffer size (skyLowResExtent when rendering the scaled
// background, ctx.swapExtent for everything else) — aspect always uses the true swap extent
// (the camera's real aspect ratio never changes just because the sky pass rendered smaller), but
// screenSizePx must reflect the actual target so gl_FragCoord-based UV math in the shader stays
// correct (see that field's comment in sat_sky.frag for why).
SatDrawPC SatelliteSim::buildSatDrawPC(VulkanContext &ctx, VkExtent2D targetExtent)
{
    SatDrawPC pc{};
    pc.skyView = camera.viewMatrix();
    pc.fovYRad = glm::radians(camera.fovYDeg);
    pc.aspect = (float)ctx.swapExtent.width / (float)ctx.swapExtent.height;
    pc.screenSizePx = glm::vec2((float)targetExtent.width, (float)targetExtent.height);
    pc.gmst = (float)fmod(kOmegaEarth * (simDayJ2000 * 86400.0 + simSecInDay), glm::two_pi<double>());
    // Wave time relative to sim epoch: pauses when paused, scales with time warp.
    // Sim sec works great as it resets before any crazy floating point issues happen. Great for any animations that need a time variable.
    // There is probably a looping artifact when it rolls over but who cares it's a tiny blip that most won't notice
    pc.waveTime = simSecInDay * 1.0;
    pc.sunDirENU = sunDirENU;
    pc.moonDirENU = moonDirENU; // xyz = moon dir in ENU, w = illuminated fraction
    // obsTerrainH and the CloudParams UBO fill both moved to recordCompute() — the new
    // cloud_march.comp dispatch there needs them before this function even runs. See the
    // comment at that relocation site for why.
    pc.obsECEFDir = glm::vec4(obsDir, obsHeightOffset); // w = user altitude offset above terrain (m); GPU computes ground height
    pc.debugDisableMask = debugDisableMask; // perf knockout toggles — see SatelliteSim.h member comment
    // Only tell the point draws to depth-test against sceneDepthTex when the hardware depth
    // buffer isn't being written — i.e. on the render-scaled path, which blits an offscreen
    // background in and never runs a depth-writing draw. At full resolution hardware depth is
    // per-fragment exact, so leave it alone.
    pc.sceneDepthMode = (renderScale < 0.999f) ? 1.0f : 0.0f;
    pc.skyGlareVisibility = skyGlareEased; // sun-glare gate for the Milky Way — see skyGlareEased member comment
    pc.beamMaxRangeM = beamMaxRangeM; // C12 follow-up #6
    pc.beamSkyGlowGain = beamSkyGlowGain; // C12 follow-up #18 — shared with cloud_march.comp's copy
    pc.beamGlowBleedGain = beamGlowBleedGain; // C12 follow-up #39 — moved here from CloudMarchPC;
                                               // now drives this shader's own beam sky-glow wash
    pc.beamProximityGlow = beamProximityGlow; // C12 follow-up #41
    return pc;
}

void SatelliteSim::recordPrePass(VkCommandBuffer cmd, VulkanContext &ctx, float /*dt*/, uint32_t imgIdx)
{
    if (renderScale >= 0.999f)
        return; // full-res: Pass 1 draws inline in recordDraw as before, nothing to pre-render here

    SatDrawPC pc = buildSatDrawPC(ctx, skyLowResExtent);

    // ── Low-res sky/ground background, into its own offscreen target ─────────────────────────
    VkClearValue clear = clearColor();
    VkRenderPassBeginInfo rbi{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
    rbi.renderPass = skyLowResRenderPass;
    rbi.framebuffer = skyLowResFramebuffer;
    rbi.renderArea = {{0, 0}, skyLowResExtent};
    rbi.clearValueCount = 1;
    rbi.pClearValues = &clear;
    vkCmdBeginRenderPass(cmd, &rbi, VK_SUBPASS_CONTENTS_INLINE);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyLowResPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                            skyBgPipeLayout, 0, 1, &skyDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, skyBgPipeLayout,
                       VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                       0, sizeof(pc), &pc);
    vkCmdDraw(cmd, 3, 1, 0, 0);
    vkCmdEndRenderPass(cmd); // finalLayout=TRANSFER_SRC_OPTIMAL — ready for the blit below, no extra barrier
    // Moved here from recordDraw's Pass 1 (same meaning: sky/terrain/ocean/cloud-composite
    // shader's own cost) — this is now the only place that timestamp gets written when scaled.
    ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 6);

    // ── Blit (linear-filtered upscale) into the swapchain image ──────────────────────────────
    // UNDEFINED oldLayout: we're about to overwrite the whole image via blit, so previous
    // contents (whatever they were — PRESENT_SRC_KHR from a prior present, or truly undefined on
    // this image's very first use) don't need to be preserved. The wait on ctx.semImageAvailable
    // (App.cpp's submit, now gated on TRANSFER too — see that comment) already guarantees the
    // presentation engine is done reading this image before this write can start.
    ctx.imageBarrier(cmd, ctx.swapImages[imgIdx],
                     0, VK_ACCESS_TRANSFER_WRITE_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);

    VkImageBlit blit{};
    blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.srcOffsets[1] = {(int32_t)skyLowResExtent.width, (int32_t)skyLowResExtent.height, 1};
    blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    blit.dstOffsets[1] = {(int32_t)ctx.swapExtent.width, (int32_t)ctx.swapExtent.height, 1};
    vkCmdBlitImage(cmd, skyLowResColorImg, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                   ctx.swapImages[imgIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                   1, &blit, VK_FILTER_LINEAR);
    // No further barrier here — activeRenderPass() returns ctx.renderPassLoad when scaled, whose
    // color attachment initialLayout is TRANSFER_DST_OPTIMAL (exactly what the blit just left it
    // in); the render pass's own automatic transition takes it to COLOR_ATTACHMENT_OPTIMAL.
}

void SatelliteSim::recordDraw(VkCommandBuffer cmd, VulkanContext &ctx, float /*dt*/)
{
    SatDrawPC pc = buildSatDrawPC(ctx, ctx.swapExtent);

    // ── Pass 1: sky/ground background (fullscreen triangle, opaque) ──────────
    // Skipped when renderScale < 1.0 — already rendered (at low res) and blitted into this
    // frame's swapchain image by recordPrePass, before this render pass even began. See
    // SatelliteSim.h's resolution-scaling member comment for the full design and the accepted
    // depth-occlusion tradeoff.
    if (renderScale >= 0.999f)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, skyBgPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                skyBgPipeLayout, 0, 1, &skyDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, skyBgPipeLayout,
                           VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, 3, 1, 0, 0);
        // Isolates the sky/terrain/ocean/cloud-composite fragment shader's own cost from the
        // satellite + star point draws that follow (previously all three were lumped into one
        // timestamp bucket in App.cpp — see VulkanContext::kTimestampCount).
        ctx.writeTimestamp(cmd, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 6);
    }

    // ── Pass 2: satellite points (additive blending) ──────────────────────────
    if (activeSatCount > 0)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, drawPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                drawPipeLayout, 0, 1, &descSet, 0, nullptr);
        // C12 follow-up #33: FRAGMENT added (sat_point.frag now reads screenSizePx for cloud
        // occlusion) — must match drawPipeLayout's push constant range exactly.
        vkCmdPushConstants(cmd, drawPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, activeSatCount, 1, 0, 0);
    }

    // ── Pass 3: background stars (additive blending) ──────────────────────────
    if (starCount > 0 && starPipeline != VK_NULL_HANDLE)
    {
        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS, starPipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_GRAPHICS,
                                starPipeLayout, 0, 1, &starDescSet, 0, nullptr);
        vkCmdPushConstants(cmd, starPipeLayout, VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT,
                           0, sizeof(pc), &pc);
        vkCmdDraw(cmd, starCount, 1, 0, 0);
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
    // ── Cloud march pipeline (C15-perf) ────────────────────────────────────────
    vkDestroyPipeline(device, cloudMarchPipeline, nullptr);
    vkDestroyPipelineLayout(device, cloudMarchPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, cloudMarchDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, cloudMarchDescLayout, nullptr);
    // ── Scene depth pipeline (pipeline unification) ────────────────────────────
    vkDestroyPipeline(device, sceneDepthPipeline, nullptr);
    vkDestroyPipelineLayout(device, sceneDepthPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, sceneDepthDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, sceneDepthDescLayout, nullptr);
    vkDestroySampler(device, sceneDepthSampler, nullptr);
    vkDestroyImageView(device, sceneDepthView, nullptr);
    vkDestroyImage(device, sceneDepthImg, nullptr);
    vkFreeMemory(device, sceneDepthMem, nullptr);
    // ── Beam cloud block pipeline (C12 follow-up #33) ──────────────────────────
    vkDestroyPipeline(device, beamCloudBlockPipeline, nullptr);
    vkDestroyPipelineLayout(device, beamCloudBlockPipeLayout, nullptr);
    vkDestroyDescriptorPool(device, beamCloudBlockDescPool, nullptr);
    vkDestroyDescriptorSetLayout(device, beamCloudBlockDescLayout, nullptr);
    // ── Flare + draw + sky pipelines ───────────────────────────────────────────
    vkDestroyPipeline(device, compPipeline, nullptr);
    vkDestroyPipeline(device, skyBgPipeline, nullptr);
    destroySkyLowResResources(device);
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
    if (milkyWaySampler)
    {
        vkDestroySampler(device, milkyWaySampler, nullptr);
        milkyWaySampler = VK_NULL_HANDLE;
    }
    if (milkyWayView)
    {
        vkDestroyImageView(device, milkyWayView, nullptr);
        milkyWayView = VK_NULL_HANDLE;
    }
    if (milkyWayImg)
    {
        vkDestroyImage(device, milkyWayImg, nullptr);
        milkyWayImg = VK_NULL_HANDLE;
    }
    if (milkyWayMem)
    {
        vkFreeMemory(device, milkyWayMem, nullptr);
        milkyWayMem = VK_NULL_HANDLE;
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
    if (cityDayDetailSampler)
    {
        vkDestroySampler(device, cityDayDetailSampler, nullptr);
        cityDayDetailSampler = VK_NULL_HANDLE;
    }
    if (cityDayDetailView)
    {
        vkDestroyImageView(device, cityDayDetailView, nullptr);
        cityDayDetailView = VK_NULL_HANDLE;
    }
    if (cityDayDetailImg)
    {
        vkDestroyImage(device, cityDayDetailImg, nullptr);
        cityDayDetailImg = VK_NULL_HANDLE;
    }
    if (cityDayDetailMem)
    {
        vkFreeMemory(device, cityDayDetailMem, nullptr);
        cityDayDetailMem = VK_NULL_HANDLE;
    }
    if (cityNightDetailSampler)
    {
        vkDestroySampler(device, cityNightDetailSampler, nullptr);
        cityNightDetailSampler = VK_NULL_HANDLE;
    }
    if (cityNightDetailView)
    {
        vkDestroyImageView(device, cityNightDetailView, nullptr);
        cityNightDetailView = VK_NULL_HANDLE;
    }
    if (cityNightDetailImg)
    {
        vkDestroyImage(device, cityNightDetailImg, nullptr);
        cityNightDetailImg = VK_NULL_HANDLE;
    }
    if (cityNightDetailMem)
    {
        vkFreeMemory(device, cityNightDetailMem, nullptr);
        cityNightDetailMem = VK_NULL_HANDLE;
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
    if (cloudNoiseSampler)
    {
        vkDestroySampler(device, cloudNoiseSampler, nullptr);
        cloudNoiseSampler = VK_NULL_HANDLE;
    }
    if (cloudNoiseView)
    {
        vkDestroyImageView(device, cloudNoiseView, nullptr);
        cloudNoiseView = VK_NULL_HANDLE;
    }
    if (cloudNoiseImg)
    {
        vkDestroyImage(device, cloudNoiseImg, nullptr);
        cloudNoiseImg = VK_NULL_HANDLE;
    }
    if (cloudNoiseMem)
    {
        vkFreeMemory(device, cloudNoiseMem, nullptr);
        cloudNoiseMem = VK_NULL_HANDLE;
    }
    if (cloudWarpNoiseSampler)
    {
        vkDestroySampler(device, cloudWarpNoiseSampler, nullptr);
        cloudWarpNoiseSampler = VK_NULL_HANDLE;
    }
    if (cloudWarpNoiseView)
    {
        vkDestroyImageView(device, cloudWarpNoiseView, nullptr);
        cloudWarpNoiseView = VK_NULL_HANDLE;
    }
    if (cloudWarpNoiseImg)
    {
        vkDestroyImage(device, cloudWarpNoiseImg, nullptr);
        cloudWarpNoiseImg = VK_NULL_HANDLE;
    }
    if (cloudWarpNoiseMem)
    {
        vkFreeMemory(device, cloudWarpNoiseMem, nullptr);
        cloudWarpNoiseMem = VK_NULL_HANDLE;
    }
    if (auroraNoiseSampler)
    {
        vkDestroySampler(device, auroraNoiseSampler, nullptr);
        auroraNoiseSampler = VK_NULL_HANDLE;
    }
    if (auroraNoiseView)
    {
        vkDestroyImageView(device, auroraNoiseView, nullptr);
        auroraNoiseView = VK_NULL_HANDLE;
    }
    if (auroraNoiseImg)
    {
        vkDestroyImage(device, auroraNoiseImg, nullptr);
        auroraNoiseImg = VK_NULL_HANDLE;
    }
    if (auroraNoiseMem)
    {
        vkFreeMemory(device, auroraNoiseMem, nullptr);
        auroraNoiseMem = VK_NULL_HANDLE;
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
    if (cloudMarchSampler)
    {
        vkDestroySampler(device, cloudMarchSampler, nullptr);
        cloudMarchSampler = VK_NULL_HANDLE;
    }
    if (cloudMarchTargetAView)
    {
        vkDestroyImageView(device, cloudMarchTargetAView, nullptr);
        cloudMarchTargetAView = VK_NULL_HANDLE;
    }
    if (cloudMarchTargetAImg)
    {
        vkDestroyImage(device, cloudMarchTargetAImg, nullptr);
        cloudMarchTargetAImg = VK_NULL_HANDLE;
    }
    if (cloudMarchTargetAMem)
    {
        vkFreeMemory(device, cloudMarchTargetAMem, nullptr);
        cloudMarchTargetAMem = VK_NULL_HANDLE;
    }
    if (cloudMarchTargetBView)
    {
        vkDestroyImageView(device, cloudMarchTargetBView, nullptr);
        cloudMarchTargetBView = VK_NULL_HANDLE;
    }
    if (cloudMarchTargetBImg)
    {
        vkDestroyImage(device, cloudMarchTargetBImg, nullptr);
        cloudMarchTargetBImg = VK_NULL_HANDLE;
    }
    if (cloudMarchTargetBMem)
    {
        vkFreeMemory(device, cloudMarchTargetBMem, nullptr);
        cloudMarchTargetBMem = VK_NULL_HANDLE;
    }
    vkDestroyBuffer(device, glowBuf, nullptr);
    vkFreeMemory(device, glowMem, nullptr);
    if (pickedVisibleMapped)
        vkUnmapMemory(device, pickedVisibleMem);
    vkDestroyBuffer(device, pickedVisibleBuf, nullptr);
    vkFreeMemory(device, pickedVisibleMem, nullptr);
    // satInputBuf is now device-local (no host mapping to release).
    vkDestroyBuffer(device, satInputBuf, nullptr);
    vkFreeMemory(device, satInputMem, nullptr);
    vkDestroyBuffer(device, satVisibleBuf, nullptr);
    vkFreeMemory(device, satVisibleMem, nullptr);
    if (lightDomeMapped)
        vkUnmapMemory(device, lightDomeMem);
    vkDestroyBuffer(device, lightDomeBuf, nullptr);
    vkFreeMemory(device, lightDomeMem, nullptr);
    vkDestroyBuffer(device, satOrbitBuf, nullptr);
    vkFreeMemory(device, satOrbitMem, nullptr);
    vkDestroyBuffer(device, mirrorNormalsBuf, nullptr);
    vkFreeMemory(device, mirrorNormalsMem, nullptr);
    if (reflectorTargetsMapped)
        vkUnmapMemory(device, reflectorTargetsMem);
    vkDestroyBuffer(device, reflectorTargetsBuf, nullptr);
    vkFreeMemory(device, reflectorTargetsMem, nullptr);
    if (reflectorTargetsECEFMapped)
        vkUnmapMemory(device, reflectorTargetsECEFMem);
    vkDestroyBuffer(device, reflectorTargetsECEFBuf, nullptr);
    vkFreeMemory(device, reflectorTargetsECEFMem, nullptr);
    vkDestroyBuffer(device, beamCloudBlockBuf, nullptr);
    vkFreeMemory(device, beamCloudBlockMem, nullptr);
    if (reflectBeamsMapped)
        vkUnmapMemory(device, reflectBeamsMem);
    vkDestroyBuffer(device, reflectBeamsBuf, nullptr);
    vkFreeMemory(device, reflectBeamsMem, nullptr);
    if (beamGlowDomeMapped)
        vkUnmapMemory(device, beamGlowDomeMem);
    vkDestroyBuffer(device, beamGlowDomeBuf, nullptr);
    vkFreeMemory(device, beamGlowDomeMem, nullptr);

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
        toggleTimeDirection();
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

    // lightDomeBuf: host-visible, updated each frame by updateLightPollutionDome().
    ctx.createBuffer(sizeof(float) * kNumLightSectors,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     lightDomeBuf, lightDomeMem);
    vkMapMemory(ctx.device, lightDomeMem, 0, sizeof(float) * kNumLightSectors, 0, &lightDomeMapped);
    memset(lightDomeMapped, 0, sizeof(float) * kNumLightSectors);

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

    // reflectorTargetsECEFBuf: host-visible + coherent, but written ONCE (right after
    // initConstellation() in init(), not every frame — see the member comment). Sized as
    // vec4 per target (xyz=unit ECEF dir, w=radius); consumed by beam_cloud_block.comp.
    VkDeviceSize reflECEFSize = sizeof(glm::vec4) * kNumReflectorTargets;
    ctx.createBuffer(reflECEFSize,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     reflectorTargetsECEFBuf, reflectorTargetsECEFMem);
    vkMapMemory(ctx.device, reflectorTargetsECEFMem, 0, reflECEFSize, 0, &reflectorTargetsECEFMapped);
    memset(reflectorTargetsECEFMapped, 0, reflECEFSize);

    // beamCloudBlockBuf: device-local. Written every frame by beam_cloud_block.comp (each of the
    // 201 threads owns one index, no atomics), read the same frame by sat_orbit.comp. No CPU
    // involvement, no per-frame zero-fill needed (full overwrite every dispatch).
    ctx.createBuffer(sizeof(glm::vec2) * kNumReflectorTargets,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT,
                     beamCloudBlockBuf, beamCloudBlockMem);

    // reflectBeamsBuf: HOST_VISIBLE|HOST_COHERENT (same reasoning as glowBuf: single frame in
    // flight, so the previous frame's atomicMax writes from sat_orbit.comp are safely readable
    // by the CPU at the top of recordCompute, used for the active-beam-count/nearest-distance
    // diagnostic). Zeroed every frame via vkCmdFillBuffer, same as before this change.
    ctx.createBuffer(sizeof(GpuReflectBeams),
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     reflectBeamsBuf, reflectBeamsMem);
    vkMapMemory(ctx.device, reflectBeamsMem, 0, sizeof(GpuReflectBeams), 0, &reflectBeamsMapped);
    memset(reflectBeamsMapped, 0, sizeof(GpuReflectBeams));

    // beamGlowDomeBuf (C12 follow-up #31): HOST_VISIBLE|HOST_COHERENT, same reasoning as
    // reflectBeamsBuf — written by sat_orbit.comp (atomicMax per sector), zeroed every frame via
    // vkCmdFillBuffer, and safely readable by the CPU (one-frame-stale) for updateStars().
    ctx.createBuffer(sizeof(float) * kNumBeamGlowSectors,
                     VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     beamGlowDomeBuf, beamGlowDomeMem);
    vkMapMemory(ctx.device, beamGlowDomeMem, 0, sizeof(float) * kNumBeamGlowSectors, 0, &beamGlowDomeMapped);
    memset(beamGlowDomeMapped, 0, sizeof(float) * kNumBeamGlowSectors);
}

// ─── createDescriptors ────────────────────────────────────────────────────────
void SatelliteSim::createDescriptors(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding bindings[8] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT | VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // glowBuf: atomic writes from flare shader
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // lightDomeBuf: host-visible, CPU-written
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1,
                   VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // beamGlowDomeBuf: C12 follow-up #31
    // C12 follow-up #33: cloud occlusion for satellite/flare points — sat_point.frag reads these,
    // same underlying views/samplers already bound into skyDescSet (bindings 10/11 there).
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // cloudTargetA
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // cloudTargetB
    // sceneDepthTex — terrain occlusion for the point sprites when renderScale < 1.0, where the
    // hardware depth buffer is never written. Same image skyDescSet binding 19 samples.
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1,
                   VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // sceneDepthTex

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 8;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &descLayout);

    VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 5},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
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
    VkDescriptorBufferInfo domeInfo{lightDomeBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo beamDomeInfo{beamGlowDomeBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo cloudAInfo{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cloudBInfo{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo satDepthInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[8] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &inpInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &visInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &glowInfo, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &domeInfo, nullptr};
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &beamDomeInfo, nullptr};
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 5, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudAInfo, nullptr, nullptr};
    writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 6, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudBInfo, nullptr, nullptr};
    writes[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr,
                 descSet, 7, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &satDepthInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 8, writes, 0, nullptr);
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
//   binding 4  reflectBeamsBuf   (readwrite SSBO — capped atomic-append beam list, C12)
//   binding 5  beamGlowDomeBuf  (readwrite SSBO — 16-sector beam sky-glow dome, C12 follow-up #31)
//   binding 6  beamCloudBlockBuf (readonly SSBO — per-target cloud occlusion, C12 follow-up #33)
void SatelliteSim::createOrbitDescriptors(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding bindings[7] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // beamCloudBlockBuf, C12 follow-up #33

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 7;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &orbitDescLayout);

    VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 7};
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
    VkDescriptorBufferInfo beamInfo{reflectBeamsBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo beamDomeInfo{beamGlowDomeBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo beamCloudBlockInfo{beamCloudBlockBuf, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[7] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &orbitInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &inputInfo, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &mirrorInfo, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &reflInfo, nullptr};
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 4, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &beamInfo, nullptr};
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 5, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &beamDomeInfo, nullptr};
    writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, orbitDescSet, 6, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &beamCloudBlockInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 7, writes, 0, nullptr);
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

// ─── createCloudNoisePipeline ────────────────────────────────────────────────
// Allocates the 128³ RGBA cloud noise volume, dispatches cloud_noise.comp to bake
// Perlin-Worley + Worley channels into it in one shot, transitions to
// SHADER_READ_ONLY_OPTIMAL, then destroys the bake pipeline/descriptor set.
// Must be called before createGlowResources() so cloudNoiseView+Sampler exist
// when the sky descriptor writes are assembled.
void SatelliteSim::createCloudNoisePipeline(VulkanContext &ctx)
{
    static constexpr uint32_t kSz = 192;

    // ── Create 192³ RGBA8 3D image (storage + sampled) ───────────────────────
    ctx.createImage(kSz, kSz, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    cloudNoiseImg, cloudNoiseMem,
                    1,    // mipLevels
                    kSz); // depth > 1 → createImage produces VK_IMAGE_TYPE_3D

    // 3D image view (layerCount=1; depth lives in extent, not array layers)
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = cloudNoiseImg;
        vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &cloudNoiseView);
    }

    // Trilinear REPEAT sampler — noise must tile seamlessly across UVW [0,1)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = 1.0f;
        vkCreateSampler(ctx.device, &sci, nullptr, &cloudNoiseSampler);
    }

    // ── Bake descriptor set layout: single STORAGE_IMAGE binding 0 ───────────
    VkDescriptorSetLayout bakeDescLayout = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &bakeDescLayout);
    }

    VkDescriptorPool bakePool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        pi.maxSets = 1;
        vkCreateDescriptorPool(ctx.device, &pi, nullptr, &bakePool);
    }

    VkDescriptorSet bakeSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = bakePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &bakeDescLayout;
        vkAllocateDescriptorSets(ctx.device, &ai, &bakeSet);
    }

    // ── Pipeline layout + compute pipeline ────────────────────────────────────
    VkPipelineLayout bakePipeLayout = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &bakeDescLayout;
        vkCreatePipelineLayout(ctx.device, &li, nullptr, &bakePipeLayout);
    }

    VkPipeline bakePipeline = VK_NULL_HANDLE;
    {
        VkShaderModule mod = ctx.loadShader("shaders/cloud_noise.comp.spv");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = stage;
        ci.layout = bakePipeLayout;
        if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &bakePipeline) != VK_SUCCESS)
            throw std::runtime_error("SatelliteSim: failed to create cloud_noise bake pipeline");
        vkDestroyShaderModule(ctx.device, mod, nullptr);
    }

    // ── One-shot bake: barrier → bind → dispatch → barrier ───────────────────
    {
        auto cmd = ctx.beginOneTimeCommands();

        // Transition UNDEFINED → GENERAL so the compute shader can write
        ctx.imageBarrier(cmd, cloudNoiseImg,
                         0, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // Descriptor write: STORAGE_IMAGE pointing at cloudNoiseView in GENERAL layout
        VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, cloudNoiseView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = bakeSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                bakePipeLayout, 0, 1, &bakeSet, 0, nullptr);
        vkCmdDispatch(cmd, 24, 24, 24); // 24×8=192 threads per axis

        // Transition GENERAL → SHADER_READ_ONLY_OPTIMAL for use by sat_sky.frag
        ctx.imageBarrier(cmd, cloudNoiseImg,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        ctx.endOneTimeCommands(cmd);
    }

    // ── Destroy bake-only Vulkan objects (view+sampler are kept as members) ───
    vkDestroyPipeline(ctx.device, bakePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, bakePipeLayout, nullptr);
    vkDestroyDescriptorPool(ctx.device, bakePool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, bakeDescLayout, nullptr);
}

// ─── createCloudWarpNoisePipeline ───────────────────────────────────────────────
// Allocates the 192³ RGB cloud/cirrus domain-warp noise volume, dispatches cloud_warp_noise.comp
// to bake it in one shot, transitions to SHADER_READ_ONLY_OPTIMAL, then destroys the bake
// pipeline/descriptor set. Structurally identical to createCloudNoisePipeline above (same
// one-shot-bake pattern) — see cloud_warp_noise.comp for what's baked, why, and the tiling/
// repetition trade-off it deliberately accepts. Resolution matches createCloudNoisePipeline's
// own 192³ exactly — see that file's header comment for why (fixing visible interpolation
// faceting at an earlier, smaller 128³/single-octave attempt). Must run before
// createCloudMarchDescriptors() so cloudWarpNoiseView+Sampler exist when that descriptor set's
// writes are assembled.
void SatelliteSim::createCloudWarpNoisePipeline(VulkanContext &ctx)
{
    static constexpr uint32_t kSz = 192;

    // ── Create 192³ RGBA8 3D image (storage + sampled) ───────────────────────
    ctx.createImage(kSz, kSz, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    cloudWarpNoiseImg, cloudWarpNoiseMem,
                    1,    // mipLevels
                    kSz); // depth > 1 → createImage produces VK_IMAGE_TYPE_3D

    // 3D image view (layerCount=1; depth lives in extent, not array layers)
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = cloudWarpNoiseImg;
        vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &cloudWarpNoiseView);
    }

    // Trilinear REPEAT sampler — the bake tiles seamlessly across UVW [0,1), and the continuous
    // wind-drift term in cloudWarpOffset relies on hardware wrap to scroll through it smoothly.
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.maxLod = 1.0f;
        vkCreateSampler(ctx.device, &sci, nullptr, &cloudWarpNoiseSampler);
    }

    // ── Bake descriptor set layout: single STORAGE_IMAGE binding 0 ───────────
    VkDescriptorSetLayout bakeDescLayout = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &bakeDescLayout);
    }

    VkDescriptorPool bakePool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        pi.maxSets = 1;
        vkCreateDescriptorPool(ctx.device, &pi, nullptr, &bakePool);
    }

    VkDescriptorSet bakeSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = bakePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &bakeDescLayout;
        vkAllocateDescriptorSets(ctx.device, &ai, &bakeSet);
    }

    // ── Pipeline layout + compute pipeline ────────────────────────────────────
    VkPipelineLayout bakePipeLayout = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &bakeDescLayout;
        vkCreatePipelineLayout(ctx.device, &li, nullptr, &bakePipeLayout);
    }

    VkPipeline bakePipeline = VK_NULL_HANDLE;
    {
        VkShaderModule mod = ctx.loadShader("shaders/cloud_warp_noise.comp.spv");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = stage;
        ci.layout = bakePipeLayout;
        if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &bakePipeline) != VK_SUCCESS)
            throw std::runtime_error("SatelliteSim: failed to create cloud_warp_noise bake pipeline");
        vkDestroyShaderModule(ctx.device, mod, nullptr);
    }

    // ── One-shot bake: barrier → bind → dispatch → barrier ───────────────────
    {
        auto cmd = ctx.beginOneTimeCommands();

        // Transition UNDEFINED → GENERAL so the compute shader can write
        ctx.imageBarrier(cmd, cloudWarpNoiseImg,
                         0, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // Descriptor write: STORAGE_IMAGE pointing at cloudWarpNoiseView in GENERAL layout
        VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, cloudWarpNoiseView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = bakeSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                bakePipeLayout, 0, 1, &bakeSet, 0, nullptr);
        vkCmdDispatch(cmd, kSz / 8, kSz / 8, kSz / 8); // local_size (8,8,8)

        // Transition GENERAL → SHADER_READ_ONLY_OPTIMAL for use by cloud_march.comp
        ctx.imageBarrier(cmd, cloudWarpNoiseImg,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        ctx.endOneTimeCommands(cmd);
    }

    // ── Destroy bake-only Vulkan objects (view+sampler are kept as members) ───
    vkDestroyPipeline(ctx.device, bakePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, bakePipeLayout, nullptr);
    vkDestroyDescriptorPool(ctx.device, bakePool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, bakeDescLayout, nullptr);
}

// ─── createAuroraNoisePipeline ─────────────────────────────────────────────────
// Allocates the 1024x16x256 RGBA aurora noise volume, dispatches aurora_noise.comp to bake the
// curtain fold base (R) + column-window colA/colB (G/B) into it in one shot, transitions to
// SHADER_READ_ONLY_OPTIMAL, then destroys the bake pipeline/descriptor set. Structurally identical
// to createCloudNoisePipeline above (same one-shot-bake pattern) — see aurora_noise.comp for what's
// baked and why. Must run before createGlowResources() so auroraNoiseView+Sampler exist when the
// sky descriptor writes are assembled.
void SatelliteSim::createAuroraNoisePipeline(VulkanContext &ctx)
{
    static constexpr uint32_t kSzU = 1024, kSzV = 16, kSzW = 256;

    // ── Create 1024x16x256 RGBA8 3D image (storage + sampled) ────────────────
    ctx.createImage(kSzU, kSzV, VK_FORMAT_R8G8B8A8_UNORM,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    auroraNoiseImg, auroraNoiseMem,
                    1,     // mipLevels
                    kSzW); // depth > 1 → createImage produces VK_IMAGE_TYPE_3D

    // 3D image view (layerCount=1; depth lives in extent, not array layers)
    {
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = auroraNoiseImg;
        vci.viewType = VK_IMAGE_VIEW_TYPE_3D;
        vci.format = VK_FORMAT_R8G8B8A8_UNORM;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &auroraNoiseView);
    }

    // U (azimuth) wraps — REPEAT. V (altitude) and W (colatitude) never wrap at runtime — sat_sky.
    // frag always clamps both to their baked ranges — so CLAMP_TO_EDGE there matches how they're
    // actually sampled and avoids any bake-edge value leaking in from the opposite side.
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 1.0f;
        vkCreateSampler(ctx.device, &sci, nullptr, &auroraNoiseSampler);
    }

    // ── Bake descriptor set layout: single STORAGE_IMAGE binding 0 ───────────
    VkDescriptorSetLayout bakeDescLayout = VK_NULL_HANDLE;
    {
        VkDescriptorSetLayoutBinding b{};
        b.binding = 0;
        b.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        b.descriptorCount = 1;
        b.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
        li.bindingCount = 1;
        li.pBindings = &b;
        vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &bakeDescLayout);
    }

    VkDescriptorPool bakePool = VK_NULL_HANDLE;
    {
        VkDescriptorPoolSize ps{VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1};
        VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
        pi.poolSizeCount = 1;
        pi.pPoolSizes = &ps;
        pi.maxSets = 1;
        vkCreateDescriptorPool(ctx.device, &pi, nullptr, &bakePool);
    }

    VkDescriptorSet bakeSet = VK_NULL_HANDLE;
    {
        VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
        ai.descriptorPool = bakePool;
        ai.descriptorSetCount = 1;
        ai.pSetLayouts = &bakeDescLayout;
        vkAllocateDescriptorSets(ctx.device, &ai, &bakeSet);
    }

    // ── Pipeline layout + compute pipeline ────────────────────────────────────
    VkPipelineLayout bakePipeLayout = VK_NULL_HANDLE;
    {
        VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
        li.setLayoutCount = 1;
        li.pSetLayouts = &bakeDescLayout;
        vkCreatePipelineLayout(ctx.device, &li, nullptr, &bakePipeLayout);
    }

    VkPipeline bakePipeline = VK_NULL_HANDLE;
    {
        VkShaderModule mod = ctx.loadShader("shaders/aurora_noise.comp.spv");
        VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
        stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        stage.module = mod;
        stage.pName = "main";
        VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
        ci.stage = stage;
        ci.layout = bakePipeLayout;
        if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &bakePipeline) != VK_SUCCESS)
            throw std::runtime_error("SatelliteSim: failed to create aurora_noise bake pipeline");
        vkDestroyShaderModule(ctx.device, mod, nullptr);
    }

    // ── One-shot bake: barrier → bind → dispatch → barrier ───────────────────
    {
        auto cmd = ctx.beginOneTimeCommands();

        // Transition UNDEFINED → GENERAL so the compute shader can write
        ctx.imageBarrier(cmd, auroraNoiseImg,
                         0, VK_ACCESS_SHADER_WRITE_BIT,
                         VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL,
                         VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT);

        // Descriptor write: STORAGE_IMAGE pointing at auroraNoiseView in GENERAL layout
        VkDescriptorImageInfo imgInfo{VK_NULL_HANDLE, auroraNoiseView, VK_IMAGE_LAYOUT_GENERAL};
        VkWriteDescriptorSet w{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
        w.dstSet = bakeSet;
        w.dstBinding = 0;
        w.descriptorCount = 1;
        w.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        w.pImageInfo = &imgInfo;
        vkUpdateDescriptorSets(ctx.device, 1, &w, 0, nullptr);

        vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, bakePipeline);
        vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE,
                                bakePipeLayout, 0, 1, &bakeSet, 0, nullptr);
        vkCmdDispatch(cmd, kSzU / 8, kSzV / 8, kSzW / 8); // local_size (8,8,8)

        // Transition GENERAL → SHADER_READ_ONLY_OPTIMAL for use by sat_sky.frag
        ctx.imageBarrier(cmd, auroraNoiseImg,
                         VK_ACCESS_SHADER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT,
                         VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                         VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);

        ctx.endOneTimeCommands(cmd);
    }

    // ── Destroy bake-only Vulkan objects (view+sampler are kept as members) ───
    vkDestroyPipeline(ctx.device, bakePipeline, nullptr);
    vkDestroyPipelineLayout(ctx.device, bakePipeLayout, nullptr);
    vkDestroyDescriptorPool(ctx.device, bakePool, nullptr);
    vkDestroyDescriptorSetLayout(ctx.device, bakeDescLayout, nullptr);
}

// ─── createCloudMarchResources ────────────────────────────────────────────────
// Two half-resolution RGBA16F storage+sampled images written by cloud_march.comp each frame.
// Unlike cloudNoiseImg's bake-once volume, these are swapchain-size-dependent — recreated in
// onResize (see there for the matching descriptor-set patch).
void SatelliteSim::createCloudMarchResources(VulkanContext &ctx)
{
    uint32_t w = (ctx.swapExtent.width + 1) / 2;
    uint32_t h = (ctx.swapExtent.height + 1) / 2;

    auto createTarget = [&](VkImage &img, VkDeviceMemory &mem, VkImageView &view)
    {
        ctx.createImage(w, h, VK_FORMAT_R16G16B16A16_SFLOAT,
                        VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                        img, mem);
        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = img;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R16G16B16A16_SFLOAT;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &view);
    };
    createTarget(cloudMarchTargetAImg, cloudMarchTargetAMem, cloudMarchTargetAView);
    createTarget(cloudMarchTargetBImg, cloudMarchTargetBMem, cloudMarchTargetBView);

    // Bilinear, clamp-to-edge — resolution-independent, created once and reused across resizes.
    if (!cloudMarchSampler)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        vkCreateSampler(ctx.device, &sci, nullptr, &cloudMarchSampler);
    }

    // Leave both images in SHADER_READ_ONLY_OPTIMAL — the layout createGlowResources' descriptor
    // write (bindings 10/11) declares and the layout recordCompute's per-frame pre-dispatch
    // barrier expects to transition FROM (see recordCompute: SHADER_READ_ONLY_OPTIMAL → GENERAL
    // before each dispatch, back to SHADER_READ_ONLY_OPTIMAL after — this call only establishes
    // that starting state once, for both init and after an onResize recreation).
    auto cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, cloudMarchTargetAImg, 0, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.imageBarrier(cmd, cloudMarchTargetBImg, 0, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
}

// ─── createCloudMarchDescriptors ──────────────────────────────────────────────
// Descriptor set for cloud_march.comp:
//   binding 0  earthCloudsTex  (sampler2D)
//   binding 1  cloudNoiseTex   (sampler3D)
//   binding 2  earthNightTex   (sampler2D)
//   binding 3  noiseTex        (sampler2D)
//   binding 4  CloudParams UBO (same underlying buffer as skyDescSet binding 9)
//   binding 5  targetA (storage image, rgba16f)
//   binding 6  targetB (storage image, rgba16f)
//   binding 9  cloudWarpNoiseTex (sampler3D) — baked domain-warp field, see cloud_warp_noise.comp
//   binding 10 reflectBeamsBuf  (readonly SSBO) — Reflect-Orbital beams, C12, volumetric in-scatter
//   binding 11 earthElevTex    (sampler2D) — C12 follow-up #28: per-beam terrain occlusion march
//   binding 12 earthSpecTex    (sampler2D) — land/ocean mask, same pair sat_sky.frag already binds
// Requires createGlowResources() to already have run (needs cloudParamsBuf, earthClouds/Night
// textures) — see init() ordering.
void SatelliteSim::createCloudMarchDescriptors(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding bindings[14] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    // lightDomeBuf: same buffer as sat_flare.comp/sat_sky.frag's own read — needed now that the
    // aurora sky curtain march moved here (perf: folded into this half-res pass alongside clouds).
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // aurora noise sampler3D
    bindings[9] = {9, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // cloud warp noise sampler3D
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // reflectBeamsBuf
    bindings[11] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // earthElevTex
    bindings[12] = {12, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // earthSpecTex
    // sceneDepthTex: written by scene_depth.comp earlier in the same recordCompute. Read 1:1 by
    // texelFetch — this set's dispatch grid and that image are the same half-swapExtent size.
    bindings[13] = {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}; // sceneDepthTex

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 14;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &cloudMarchDescLayout);

    VkDescriptorPoolSize ps[4] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 9},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 4;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &cloudMarchDescPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = cloudMarchDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &cloudMarchDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &cloudMarchDescSet);

    VkDescriptorImageInfo cloudsInfo{earthCloudsSampler, earthCloudsView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo noise3DInfo{cloudNoiseSampler, cloudNoiseView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo nightInfo{earthNightSampler, earthNightView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo noiseInfo{noiseSampler, noiseTexView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo cloudParamsInfo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
    VkDescriptorImageInfo targetAInfo{VK_NULL_HANDLE, cloudMarchTargetAView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorImageInfo targetBInfo{VK_NULL_HANDLE, cloudMarchTargetBView, VK_IMAGE_LAYOUT_GENERAL};
    VkDescriptorBufferInfo lightDomeInfo{lightDomeBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo auroraNoiseInfo{auroraNoiseSampler, auroraNoiseView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo warpNoiseInfo{cloudWarpNoiseSampler, cloudWarpNoiseView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo beamInfo{reflectBeamsBuf, 0, VK_WHOLE_SIZE};
    // Same fallback pattern as the sky descriptor set (SatelliteSim.cpp:3432-3435) — elevation
    // texture may have failed to load, fall back to the always-valid noise sampler so this
    // descriptor set is never left pointing at a null image.
    VkSampler   elevSamplerFinal2 = earthElevSampler ? earthElevSampler : noiseSampler;
    VkImageView elevViewFinal2    = earthElevView ? earthElevView : noiseTexView;
    VkSampler   specSamplerFinal2 = earthSpecSampler ? earthSpecSampler : noiseSampler;
    VkImageView specViewFinal2    = earthSpecView ? earthSpecView : noiseTexView;
    VkDescriptorImageInfo elevInfo{elevSamplerFinal2, elevViewFinal2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo specInfo{specSamplerFinal2, specViewFinal2, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo sceneDepthInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};

    VkWriteDescriptorSet writes[14] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudsInfo, nullptr, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &noise3DInfo, nullptr, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &nightInfo, nullptr, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &noiseInfo, nullptr, nullptr};
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 4, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cloudParamsInfo, nullptr};
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 5, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &targetAInfo, nullptr, nullptr};
    writes[6] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 6, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &targetBInfo, nullptr, nullptr};
    writes[7] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 7, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &lightDomeInfo, nullptr};
    writes[8] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 8, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &auroraNoiseInfo, nullptr, nullptr};
    writes[9] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 9, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &warpNoiseInfo, nullptr, nullptr};
    writes[10] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 10, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &beamInfo, nullptr};
    writes[11] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 11, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &elevInfo, nullptr, nullptr};
    writes[12] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 12, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &specInfo, nullptr, nullptr};
    writes[13] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, cloudMarchDescSet, 13, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &sceneDepthInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 14, writes, 0, nullptr);
}

// ─── createCloudMarchPipeline ──────────────────────────────────────────────────
void SatelliteSim::createCloudMarchPipeline(VulkanContext &ctx)
{
    VkShaderModule mod = ctx.loadShader("shaders/cloud_march.comp.spv");

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(CloudMarchPC)};

    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &cloudMarchDescLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &li, nullptr, &cloudMarchPipeLayout);

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = cloudMarchPipeLayout;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &cloudMarchPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create cloud_march compute pipeline");

    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

// ─── createSceneDepthResources ────────────────────────────────────────────────
// One half-resolution R32_SFLOAT storage+sampled image written by scene_depth.comp each frame,
// holding the linear distance to the first terrain/ocean surface along each view ray.
//
// Sized identically to cloudMarchTargetA/B — half of the SWAP extent, NOT of the render-scaled
// extent — so cloud_march.comp (which dispatches on exactly this grid) can read it with a plain
// texelFetch at its own gl_GlobalInvocationID, with no UV math to get wrong. Swapchain-size
// dependent, so recreated in onResize alongside those targets.
//
// A colour-aspect image rather than a real depth attachment, deliberately: that keeps
// ctx.imageBarrier (which hardcodes COLOR aspect / 1 mip / 1 layer) usable unchanged, and avoids
// the depth-format blit portability problem that made the render-scale path skip depth entirely.
void SatelliteSim::createSceneDepthResources(VulkanContext &ctx)
{
    uint32_t w = (ctx.swapExtent.width + 1) / 2;
    uint32_t h = (ctx.swapExtent.height + 1) / 2;

    ctx.createImage(w, h, VK_FORMAT_R32_SFLOAT,
                    VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT,
                    sceneDepthImg, sceneDepthMem);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = sceneDepthImg;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = VK_FORMAT_R32_SFLOAT;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vci, nullptr, &sceneDepthView);

    // NEAREST, not LINEAR. Filtering a distance field across a terrain silhouette interpolates
    // between "ridge at 8 km" and "sky at 1e30", producing meaningless intermediate distances
    // along every skyline. Point sampling keeps every fetched value one the depth pass actually
    // wrote. Created once and reused across resizes (resolution-independent).
    if (!sceneDepthSampler)
    {
        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_NEAREST;
        sci.minFilter = VK_FILTER_NEAREST;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = 0.0f;
        vkCreateSampler(ctx.device, &sci, nullptr, &sceneDepthSampler);
    }

    // Establish SHADER_READ_ONLY_OPTIMAL as the starting layout — what the descriptor writes
    // declare and what recordCompute's per-frame pre-dispatch barrier transitions FROM. Same
    // one-time-setup role createCloudMarchResources' matching barriers play, for both first init
    // and after an onResize recreation.
    auto cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, sceneDepthImg, 0, VK_ACCESS_SHADER_READ_BIT,
                     VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
}

// ─── createSceneDepthDescriptors ──────────────────────────────────────────────
// Descriptor set for scene_depth.comp:
//   binding 0  earthElevTex (sampler2D)
//   binding 1  earthSpecTex (sampler2D)
//   binding 2  sceneDepth   (storage image, r32f)
void SatelliteSim::createSceneDepthDescriptors(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding bindings[3] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 3;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &sceneDepthDescLayout);

    VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 2},
        {VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &sceneDepthDescPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = sceneDepthDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &sceneDepthDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &sceneDepthDescSet);

    // Same fallback pattern the sky and cloud-march sets use — the elevation/spec textures may
    // have failed to load, so fall back to the always-valid noise sampler rather than leaving a
    // descriptor pointing at a null image. With that fallback the depth pass reads noise as
    // "terrain", which is wrong but bounded; a null view is a device loss.
    VkSampler   elevSamplerFinal = earthElevSampler ? earthElevSampler : noiseSampler;
    VkImageView elevViewFinal    = earthElevView ? earthElevView : noiseTexView;
    VkSampler   specSamplerFinal = earthSpecSampler ? earthSpecSampler : noiseSampler;
    VkImageView specViewFinal    = earthSpecView ? earthSpecView : noiseTexView;
    VkDescriptorImageInfo elevInfo{elevSamplerFinal, elevViewFinal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo specInfo{specSamplerFinal, specViewFinal, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo depthInfo{VK_NULL_HANDLE, sceneDepthView, VK_IMAGE_LAYOUT_GENERAL};

    VkWriteDescriptorSet writes[3] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sceneDepthDescSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &elevInfo, nullptr, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sceneDepthDescSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &specInfo, nullptr, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, sceneDepthDescSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, &depthInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 3, writes, 0, nullptr);
}

// ─── createSceneDepthPipeline ─────────────────────────────────────────────────
void SatelliteSim::createSceneDepthPipeline(VulkanContext &ctx)
{
    VkShaderModule mod = ctx.loadShader("shaders/scene_depth.comp.spv");

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(SceneDepthPC)};

    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &sceneDepthDescLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &li, nullptr, &sceneDepthPipeLayout);

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = sceneDepthPipeLayout;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &sceneDepthPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create scene_depth compute pipeline");

    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

// (createCloudShadowResources / Descriptors / Pipeline lived here — the 128x128 R16_SFLOAT grid,
//  its own 5-binding descriptor set, sampler, and compute pipeline. All deleted in the
//  pipeline-unification pass; cloud_march.comp::cloudGroundShadow now produces the same value
//  per pixel from the terrain hit point the scene-depth pass supplies, using bindings that pass
//  already had.)

// ─── createBeamCloudBlockDescriptors ──────────────────────────────────────────
// Descriptor set for beam_cloud_block.comp (C12 follow-up #33):
//   binding 0  reflectorTargetsECEFBuf (readonly SSBO) — static, uploaded once
//   binding 1  earthCloudsTex  (sampler2D)  — same texture as cloud_shadow.comp binding 0
//   binding 2  cloudNoiseTex   (sampler3D)  — same texture as cloud_shadow.comp binding 1
//   binding 3  cloudWarpNoiseTex (sampler3D) — same texture as cloud_shadow.comp binding 2
//   binding 4  CloudParams UBO (same underlying buffer as skyDescSet binding 9)
//   binding 5  beamCloudBlockBuf (SSBO, write)
// Deliberately its own small descriptor set — different buffer shapes
// (no storage image here) and the two passes are conceptually independent (see
// beam_cloud_block.comp's header for why this isn't built on cloud_shadow.comp's grid).
void SatelliteSim::createBeamCloudBlockDescriptors(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding bindings[6] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 6;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &beamCloudBlockDescLayout);

    VkDescriptorPoolSize ps[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 2},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
        {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &beamCloudBlockDescPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = beamCloudBlockDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &beamCloudBlockDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &beamCloudBlockDescSet);

    VkDescriptorBufferInfo targetEcefInfo{reflectorTargetsECEFBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo cloudsInfo{earthCloudsSampler, earthCloudsView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo noise3DInfo{cloudNoiseSampler, cloudNoiseView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo warpNoiseInfo{cloudWarpNoiseSampler, cloudWarpNoiseView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo cloudParamsInfo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
    VkDescriptorBufferInfo blockOutInfo{beamCloudBlockBuf, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[6] = {};
    writes[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, beamCloudBlockDescSet, 0, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &targetEcefInfo, nullptr};
    writes[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, beamCloudBlockDescSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &cloudsInfo, nullptr, nullptr};
    writes[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, beamCloudBlockDescSet, 2, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &noise3DInfo, nullptr, nullptr};
    writes[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, beamCloudBlockDescSet, 3, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &warpNoiseInfo, nullptr, nullptr};
    writes[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, beamCloudBlockDescSet, 4, 0, 1,
                 VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &cloudParamsInfo, nullptr};
    writes[5] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, beamCloudBlockDescSet, 5, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &blockOutInfo, nullptr};
    vkUpdateDescriptorSets(ctx.device, 6, writes, 0, nullptr);
}

// ─── createBeamCloudBlockPipeline ─────────────────────────────────────────────
void SatelliteSim::createBeamCloudBlockPipeline(VulkanContext &ctx)
{
    VkShaderModule mod = ctx.loadShader("shaders/beam_cloud_block.comp.spv");

    VkPipelineShaderStageCreateInfo stage{VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
    stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    stage.module = mod;
    stage.pName = "main";

    VkPushConstantRange pcr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(BeamCloudBlockPC)};

    VkPipelineLayoutCreateInfo li{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    li.setLayoutCount = 1;
    li.pSetLayouts = &beamCloudBlockDescLayout;
    li.pushConstantRangeCount = 1;
    li.pPushConstantRanges = &pcr;
    vkCreatePipelineLayout(ctx.device, &li, nullptr, &beamCloudBlockPipeLayout);

    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = stage;
    ci.layout = beamCloudBlockPipeLayout;
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &beamCloudBlockPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create beam_cloud_block compute pipeline");

    vkDestroyShaderModule(ctx.device, mod, nullptr);
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

    // ── Picked-satellite tracking buffer ───────────────────────────────────────
    // 32-byte host-visible mirror of the selected satellite's GpuSatVisible entry, written by a
    // tiny vkCmdCopyBuffer in recordCompute (only while a selection is active) and read back
    // one-frame-stale at the top of recordCompute — same idiom as glowBuf/peakMagnitude above.
    // Never bound as an SSBO, so TRANSFER_DST is the only usage it needs.
    ctx.createBuffer(sizeof(GpuSatVisible),
                     VK_BUFFER_USAGE_TRANSFER_DST_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     pickedVisibleBuf, pickedVisibleMem);
    vkMapMemory(ctx.device, pickedVisibleMem, 0, sizeof(GpuSatVisible), 0, &pickedVisibleMapped);
    memset(pickedVisibleMapped, 0, sizeof(GpuSatVisible));

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

    // ── Milky Way skybox texture (binding 13): 8K equirectangular galactic panorama ──
    // Same load pattern as earthDay (SRGB, mipmapped). Sampled in sat_sky.frag against a
    // CPU-computed ENU->galactic direction; see the "Milky Way skybox basis" block in
    // updatePositions().
    {
        int w = 0, h = 0, ch = 0;
        stbi_uc *pixels = stbi_load("assets/textures/8k_stars_milky_way.jpg", &w, &h, &ch, 4);
        if (!pixels)
            throw std::runtime_error("SatelliteSim: failed to load assets/textures/8k_stars_milky_way.jpg");

        milkyWayMips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1;
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
                        milkyWayImg, milkyWayMem, milkyWayMips);

        {
            auto cmd = ctx.beginOneTimeCommands();
            VkImageMemoryBarrier allMips{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
            allMips.srcAccessMask = 0;
            allMips.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            allMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            allMips.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            allMips.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
            allMips.image = milkyWayImg;
            allMips.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, milkyWayMips, 0, 1};
            vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &allMips);
            VkBufferImageCopy region{};
            region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
            region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
            vkCmdCopyBufferToImage(cmd, stageBuf, milkyWayImg,
                                   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
            ctx.generateMipmaps(cmd, milkyWayImg, VK_FORMAT_R8G8B8A8_SRGB,
                                (uint32_t)w, (uint32_t)h, milkyWayMips);
            ctx.endOneTimeCommands(cmd);
        }
        vkDestroyBuffer(ctx.device, stageBuf, nullptr);
        vkFreeMemory(ctx.device, stageMem, nullptr);

        VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
        vci.image = milkyWayImg;
        vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        vci.format = VK_FORMAT_R8G8B8A8_SRGB;
        vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, milkyWayMips, 0, 1};
        vkCreateImageView(ctx.device, &vci, nullptr, &milkyWayView);

        VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
        sci.magFilter = VK_FILTER_LINEAR;
        sci.minFilter = VK_FILTER_LINEAR;
        sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
        sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
        sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
        sci.maxLod = (float)milkyWayMips;
        vkCreateSampler(ctx.device, &sci, nullptr, &milkyWaySampler);
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

        // CPU-side downsample to 2160×1080 (~18km/px, matches earthElevCpu) for the observer
        // light-pollution lookup — stores precomputed Rec.709 luminance, one byte per texel.
        // Box-filtered (average every source pixel in each cell), not nearest-neighbor picking
        // one corner pixel — the latter throws away ~93% of the source data per cell and bakes
        // real aliasing/moiré into the array before updateLightPollutionDome() ever samples it.
        earthNightCpuW = 2160;
        earthNightCpuH = 1080;
        earthNightCpu.resize((size_t)earthNightCpuW * earthNightCpuH);
        for (int cy = 0; cy < earthNightCpuH; ++cy)
        {
            int sy0 = cy * h / earthNightCpuH;
            int sy1 = std::max(sy0 + 1, (cy + 1) * h / earthNightCpuH);
            for (int cx = 0; cx < earthNightCpuW; ++cx)
            {
                int sx0 = cx * w / earthNightCpuW;
                int sx1 = std::max(sx0 + 1, (cx + 1) * w / earthNightCpuW);
                float sum = 0.0f;
                int count = 0;
                for (int sy = sy0; sy < sy1; ++sy)
                {
                    for (int sx = sx0; sx < sx1; ++sx)
                    {
                        const stbi_uc *px = &pixels[((size_t)sy * w + sx) * 4];
                        sum += 0.2126f * px[0] + 0.7152f * px[1] + 0.0722f * px[2];
                        ++count;
                    }
                }
                earthNightCpu[cy * earthNightCpuW + cx] = (uint8_t)std::clamp(sum / (float)count, 0.0f, 255.0f);
            }
        }

        // Half-resolution box-blur (~37km/px) — see the member comment in SatelliteSim.h. One 2×2
        // averaging pass over the already-box-filtered earthNightCpu above.
        earthNightCpuBlurW = earthNightCpuW / 2;
        earthNightCpuBlurH = earthNightCpuH / 2;
        earthNightCpuBlur.resize((size_t)earthNightCpuBlurW * earthNightCpuBlurH);
        for (int by = 0; by < earthNightCpuBlurH; ++by)
        {
            for (int bx = 0; bx < earthNightCpuBlurW; ++bx)
            {
                int x0 = bx * 2, y0 = by * 2;
                int sum = earthNightCpu[y0 * earthNightCpuW + x0]
                        + earthNightCpu[y0 * earthNightCpuW + x0 + 1]
                        + earthNightCpu[(y0 + 1) * earthNightCpuW + x0]
                        + earthNightCpu[(y0 + 1) * earthNightCpuW + x0 + 1];
                earthNightCpuBlur[by * earthNightCpuBlurW + bx] = (uint8_t)(sum / 4);
            }
        }
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

    // ── City day/night detail textures (bindings 14/15): small tileable maps blended onto
    // dayColor/nightColor near cities (see terrain block in sat_sky.frag).
    {
        struct { const char *path; VkImage *img; VkDeviceMemory *mem; VkImageView *view;
                  VkSampler *sampler; uint32_t *mips; } detailTexes[2] = {
            {"assets/textures/city_day_detail.png", &cityDayDetailImg, &cityDayDetailMem,
             &cityDayDetailView, &cityDayDetailSampler, &cityDayDetailMips},
            {"assets/textures/city_night_detail.png", &cityNightDetailImg, &cityNightDetailMem,
             &cityNightDetailView, &cityNightDetailSampler, &cityNightDetailMips},
        };
        for (auto &t : detailTexes)
        {
            int w = 0, h = 0, ch = 0;
            stbi_uc *pixels = stbi_load(t.path, &w, &h, &ch, 4);
            if (!pixels)
                throw std::runtime_error(std::string("SatelliteSim: failed to load ") + t.path);

            *t.mips = (uint32_t)std::floor(std::log2((float)std::max(w, h))) + 1;
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
                            *t.img, *t.mem, *t.mips);

            {
                auto cmd = ctx.beginOneTimeCommands();
                VkImageMemoryBarrier allMips{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
                allMips.srcAccessMask = 0;
                allMips.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
                allMips.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                allMips.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
                allMips.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                allMips.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
                allMips.image = *t.img;
                allMips.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, *t.mips, 0, 1};
                vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
                                     VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &allMips);
                VkBufferImageCopy region{};
                region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
                region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
                vkCmdCopyBufferToImage(cmd, stageBuf, *t.img,
                                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
                ctx.generateMipmaps(cmd, *t.img, VK_FORMAT_R8G8B8A8_SRGB,
                                    (uint32_t)w, (uint32_t)h, *t.mips);
                ctx.endOneTimeCommands(cmd);
            }
            vkDestroyBuffer(ctx.device, stageBuf, nullptr);
            vkFreeMemory(ctx.device, stageMem, nullptr);

            VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
            vci.image = *t.img;
            vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            vci.format = VK_FORMAT_R8G8B8A8_SRGB;
            vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, *t.mips, 0, 1};
            vkCreateImageView(ctx.device, &vci, nullptr, t.view);

            // Tileable in both U and V (unlike the equirect Earth maps, which only repeat U).
            VkSamplerCreateInfo sci{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
            sci.magFilter = VK_FILTER_LINEAR;
            sci.minFilter = VK_FILTER_LINEAR;
            sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_LINEAR;
            sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_REPEAT;
            sci.maxLod = (float)*t.mips;
            vkCreateSampler(ctx.device, &sci, nullptr, t.sampler);
        }
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

    // ── Descriptor set layout: 0=GlowBuf, 1=noise, 2=moon, 3=earthDay, 4=earthNight, 5=earthElev, 6=earthSpec, 7=earthClouds, 8=cloudNoise3D, 9=CloudParams UBO, 10/11=half-res cloud march targets A/B, 12=lightDomeBuf, 13=milkyWayTex, 14=cityDayDetail, 15=cityNightDetail, 16=auroraNoise3D, 17=reflectBeamsBuf, 18=beamGlowDomeBuf, 19=sceneDepthTex
    VkDescriptorSetLayoutBinding bindings[20] = {};
    bindings[0] = {0, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[2] = {2, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[4] = {4, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[5] = {5, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[6] = {6, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[7] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[8] = {8, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // cloudNoise sampler3D
    bindings[9] = {9, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[10] = {10, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // half-res cloud march target A
    bindings[11] = {11, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // half-res cloud march target B
    // lightDomeBuf: same buffer as sat_flare.comp's binding 3 — sat_sky.frag needs its own read of
    // it to dim the Milky Way directionally, matching how satellites/stars are already dimmed.
    bindings[12] = {12, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    bindings[13] = {13, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // milky way skybox
    bindings[14] = {14, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // city day detail
    bindings[15] = {15, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // city night detail
    bindings[16] = {16, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr}; // aurora noise sampler3D
    // reflectBeamsBuf: same buffer as sat_orbit.comp's binding 4 — ground-spot direct lighting (C12).
    bindings[17] = {17, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // beamGlowDomeBuf: same buffer as sat_orbit.comp's binding 5 / sat_flare.comp's binding 4 —
    // dims the Milky Way near an active beam the same way the light pollution dome already does
    // (C12 follow-up #31). Was binding 19; compacted down when cloudShadowTex (18) was deleted.
    bindings[18] = {18, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    // sceneDepthTex: same image scene_depth.comp writes at the top of recordCompute — the shared
    // terrain/ocean distance every occlusion test now reads instead of re-deriving.
    bindings[19] = {19, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 20;
    li.pBindings = bindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &skyDescLayout);

    VkDescriptorPoolSize ps[3] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 4},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 15},
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
    VkDescriptorImageInfo cloudNoiseImgInfo{cloudNoiseSampler, cloudNoiseView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo cloudParamsInfo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
    VkDescriptorImageInfo cloudMarchAImgInfo{cloudMarchSampler, cloudMarchTargetAView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cloudMarchBImgInfo{cloudMarchSampler, cloudMarchTargetBView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo lightDomeInfo{lightDomeBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo milkyWayImgInfo{milkyWaySampler, milkyWayView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cityDayDetailImgInfo{cityDayDetailSampler, cityDayDetailView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo cityNightDetailImgInfo{cityNightDetailSampler, cityNightDetailView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo auroraNoiseImgInfo{auroraNoiseSampler, auroraNoiseView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo reflectBeamsInfo{reflectBeamsBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorBufferInfo beamGlowDomeInfo{beamGlowDomeBuf, 0, VK_WHOLE_SIZE};

    VkWriteDescriptorSet writes[20] = {};
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
    writes[8].dstBinding = 8;
    writes[8].descriptorCount = 1;
    writes[8].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[8].pImageInfo = &cloudNoiseImgInfo;
    writes[9].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[9].dstSet = skyDescSet;
    writes[9].dstBinding = 9;
    writes[9].descriptorCount = 1;
    writes[9].descriptorType = VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER;
    writes[9].pBufferInfo = &cloudParamsInfo;
    writes[10].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[10].dstSet = skyDescSet;
    writes[10].dstBinding = 10;
    writes[10].descriptorCount = 1;
    writes[10].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[10].pImageInfo = &cloudMarchAImgInfo;
    writes[11].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[11].dstSet = skyDescSet;
    writes[11].dstBinding = 11;
    writes[11].descriptorCount = 1;
    writes[11].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[11].pImageInfo = &cloudMarchBImgInfo;
    writes[12].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[12].dstSet = skyDescSet;
    writes[12].dstBinding = 12;
    writes[12].descriptorCount = 1;
    writes[12].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[12].pBufferInfo = &lightDomeInfo;
    writes[13].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[13].dstSet = skyDescSet;
    writes[13].dstBinding = 13;
    writes[13].descriptorCount = 1;
    writes[13].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[13].pImageInfo = &milkyWayImgInfo;
    writes[14].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[14].dstSet = skyDescSet;
    writes[14].dstBinding = 14;
    writes[14].descriptorCount = 1;
    writes[14].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[14].pImageInfo = &cityDayDetailImgInfo;
    writes[15].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[15].dstSet = skyDescSet;
    writes[15].dstBinding = 15;
    writes[15].descriptorCount = 1;
    writes[15].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[15].pImageInfo = &cityNightDetailImgInfo;
    writes[16].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[16].dstSet = skyDescSet;
    writes[16].dstBinding = 16;
    writes[16].descriptorCount = 1;
    writes[16].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[16].pImageInfo = &auroraNoiseImgInfo;
    writes[17].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[17].dstSet = skyDescSet;
    writes[17].dstBinding = 17;
    writes[17].descriptorCount = 1;
    writes[17].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[17].pBufferInfo = &reflectBeamsInfo;
    writes[18].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[18].dstSet = skyDescSet;
    writes[18].dstBinding = 18;
    writes[18].descriptorCount = 1;
    writes[18].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
    writes[18].pBufferInfo = &beamGlowDomeInfo;
    VkDescriptorImageInfo sceneDepthImgInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    writes[19].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[19].dstSet = skyDescSet;
    writes[19].dstBinding = 19;
    writes[19].descriptorCount = 1;
    writes[19].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[19].pImageInfo = &sceneDepthImgInfo;
    vkUpdateDescriptorSets(ctx.device, 20, writes, 0, nullptr);
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

// ─── createSkyLowResResources (resolution scaling, session 29) ────────────────────────────────
// Swapchain-size-AND-renderScale-dependent, so this recreates on both resize and any renderScale
// change (see buildSettingsDisplayTab). A single color-only render pass (no depth attachment —
// nothing else draws in this pass to depth-test against, and depth is deliberately not blitted
// downstream — see the member comments in SatelliteSim.h), CLEARed fresh each frame, with
// finalLayout TRANSFER_SRC_OPTIMAL so recordPrePass's blit needs no extra barrier on this side.
void SatelliteSim::createSkyLowResResources(VulkanContext &ctx)
{
    skyLowResExtent.width = std::max(1u, (uint32_t)(ctx.swapExtent.width * renderScale));
    skyLowResExtent.height = std::max(1u, (uint32_t)(ctx.swapExtent.height * renderScale));

    // ── Render pass: single color attachment, CLEAR -> TRANSFER_SRC_OPTIMAL ──────────────────
    VkAttachmentDescription color{};
    color.format = ctx.swapFormat;
    color.samples = VK_SAMPLE_COUNT_1_BIT;
    color.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
    color.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
    color.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
    color.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
    color.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    color.finalLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    VkAttachmentReference colorRef{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
    VkSubpassDescription sub{};
    sub.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
    sub.colorAttachmentCount = 1;
    sub.pColorAttachments = &colorRef;
    VkRenderPassCreateInfo rpci{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
    rpci.attachmentCount = 1;
    rpci.pAttachments = &color;
    rpci.subpassCount = 1;
    rpci.pSubpasses = &sub;
    if (vkCreateRenderPass(ctx.device, &rpci, nullptr, &skyLowResRenderPass) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create skyLowRes render pass");

    // ── Color image + view ────────────────────────────────────────────────────────────────────
    ctx.createImage(skyLowResExtent.width, skyLowResExtent.height, ctx.swapFormat,
                    VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
                    skyLowResColorImg, skyLowResColorMem);
    VkImageViewCreateInfo vci{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vci.image = skyLowResColorImg;
    vci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vci.format = ctx.swapFormat;
    vci.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vci, nullptr, &skyLowResColorView);

    // ── Framebuffer ────────────────────────────────────────────────────────────────────────────
    VkFramebufferCreateInfo fci{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
    fci.renderPass = skyLowResRenderPass;
    fci.attachmentCount = 1;
    fci.pAttachments = &skyLowResColorView;
    fci.width = skyLowResExtent.width;
    fci.height = skyLowResExtent.height;
    fci.layers = 1;
    if (vkCreateFramebuffer(ctx.device, &fci, nullptr, &skyLowResFramebuffer) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create skyLowRes framebuffer");

    // ── Pipeline: same shaders/layout as skyBgPipeline, low-res viewport, no depth ────────────
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

    VkViewport vp{0, 0, (float)skyLowResExtent.width, (float)skyLowResExtent.height, 0, 1};
    VkRect2D sc{{0, 0}, skyLowResExtent};
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

    // No depth attachment in this render pass at all — test/write both off.
    VkPipelineDepthStencilStateCreateInfo ds{VK_STRUCTURE_TYPE_PIPELINE_DEPTH_STENCIL_STATE_CREATE_INFO};
    ds.depthTestEnable = VK_FALSE;
    ds.depthWriteEnable = VK_FALSE;

    VkPipelineColorBlendAttachmentState cba{};
    cba.blendEnable = VK_FALSE;
    cba.colorWriteMask = VK_COLOR_COMPONENT_R_BIT | VK_COLOR_COMPONENT_G_BIT | VK_COLOR_COMPONENT_B_BIT | VK_COLOR_COMPONENT_A_BIT;
    VkPipelineColorBlendStateCreateInfo cb{VK_STRUCTURE_TYPE_PIPELINE_COLOR_BLEND_STATE_CREATE_INFO};
    cb.attachmentCount = 1;
    cb.pAttachments = &cba;

    // skyBgPipeLayout already exists by the time this is first called (createSkyBgPipeline runs
    // first in init() — see there) and is reused as-is: identical push constants/descriptor set.
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
    ci.renderPass = skyLowResRenderPass;
    ci.subpass = 0;

    if (vkCreateGraphicsPipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &skyLowResPipeline) != VK_SUCCESS)
        throw std::runtime_error("SatelliteSim: failed to create skyLowRes pipeline");

    vkDestroyShaderModule(ctx.device, vert, nullptr);
    vkDestroyShaderModule(ctx.device, frag, nullptr);
}

void SatelliteSim::destroySkyLowResResources(VkDevice device)
{
    if (skyLowResPipeline) vkDestroyPipeline(device, skyLowResPipeline, nullptr);
    if (skyLowResFramebuffer) vkDestroyFramebuffer(device, skyLowResFramebuffer, nullptr);
    if (skyLowResColorView) vkDestroyImageView(device, skyLowResColorView, nullptr);
    if (skyLowResColorImg) vkDestroyImage(device, skyLowResColorImg, nullptr);
    if (skyLowResColorMem) vkFreeMemory(device, skyLowResColorMem, nullptr);
    if (skyLowResRenderPass) vkDestroyRenderPass(device, skyLowResRenderPass, nullptr);
    skyLowResPipeline = VK_NULL_HANDLE;
    skyLowResFramebuffer = VK_NULL_HANDLE;
    skyLowResColorView = VK_NULL_HANDLE;
    skyLowResColorImg = VK_NULL_HANDLE;
    skyLowResColorMem = VK_NULL_HANDLE;
    skyLowResRenderPass = VK_NULL_HANDLE;
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
        // C12 follow-up #33: FRAGMENT added so sat_point.frag can read screenSizePx for its new
        // cloud-occlusion sampling (previously vertex-only, since the fragment shader used no
        // push constants at all before this).
        VkPushConstantRange drawPcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SatDrawPC)};
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

    // Descriptor layout: binding 1 (vertex shader reads GpuSatVisible) + binding 7
    // (fragment shader reads sceneDepthTex for terrain occlusion at renderScale < 1.0).
    VkDescriptorSetLayoutBinding starBindings[2] = {};
    starBindings[0] = {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_VERTEX_BIT, nullptr};
    starBindings[1] = {7, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_FRAGMENT_BIT, nullptr};

    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 2;
    li.pBindings = starBindings;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &starDescLayout);

    VkDescriptorPoolSize ps[2] = {
        {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1},
        {VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 2;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &starDescPool);

    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = starDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &starDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &starDescSet);

    VkDescriptorBufferInfo bufInfo{starBuf, 0, VK_WHOLE_SIZE};
    VkDescriptorImageInfo starDepthInfo{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkWriteDescriptorSet starWr[2] = {};
    starWr[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, starDescSet, 1, 0, 1,
                 VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &bufInfo, nullptr};
    starWr[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, starDescSet, 7, 0, 1,
                 VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &starDepthInfo, nullptr, nullptr};
    vkUpdateDescriptorSets(ctx.device, 2, starWr, 0, nullptr);

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
        // VERTEX|FRAGMENT: star_point.frag now reads screenSizePx/sceneDepthMode for the depth
        // test. The vkCmdPushConstants call at the draw site must use these exact same flags.
        VkPushConstantRange pcr{VK_SHADER_STAGE_VERTEX_BIT | VK_SHADER_STAGE_FRAGMENT_BIT, 0, sizeof(SatDrawPC)};
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

// ─── updateLightPollutionDome ────────────────────────────────────────────────
// Builds an 8-azimuth-sector "how much city glow is in this compass direction" dome around
// the observer, replacing the old single-scalar "brightness at the observer's own position"
// approximation that dimmed stars/satellites uniformly regardless of which way they appeared
// in the sky (session 25 follow-up per user feedback). Each sector samples earthNightCpuBlur
// (bilinearly) at a few radii along that bearing — a flat-Earth tangent-plane lat/lon offset,
// adequate at the tens-of-km scale light pollution actually reaches — and combines them with a
// weighted max
// (one nearby bright city should dominate that direction's glow, not get averaged away by
// darker samples at other radii in the same sector). Sector convention matches GlowBuf's
// existing 8-sector azBin in sat_flare.comp exactly (bearing clockwise from North, 45° each)
// so both consumers read consistent geometry. Uploaded to lightDomeBuf for sat_flare.comp;
// updateStars() (called right after this) reads lightDomeAz[] directly, no upload needed there.
void SatelliteSim::updateLightPollutionDome()
{
    float obsR = glm::length(obsECI);
    float obsHeight = obsR - kEarthRadius;
    // Altitude falloff: light pollution's visible skyglow washes out within a few km — a much
    // tighter scale than the atmosphere's own 80km Rayleigh height. Effectively zero by aircraft
    // cruise altitude, let alone orbit. Same for every sector (observer's own altitude).
    float altFalloff = glm::clamp(glm::exp(-obsHeight / 3000.0f), 0.0f, 1.0f);

    if (earthNightCpuBlur.empty() || altFalloff <= 0.0f)
    {
        for (int i = 0; i < kNumLightSectors; ++i)
            lightDomeAz[i] = 0.0f;
        memcpy(lightDomeMapped, lightDomeAz, sizeof(lightDomeAz));
        return;
    }

    // Bilinear sample of earthNightCpuBlur (the coarser, box-blurred level — see the member
    // comment in SatelliteSim.h) — previously a nearest-pixel lookup against the sharp array,
    // which made each of the 4 per-sector radius samples snap between ~18km cells as the
    // observer moved or as neighboring sectors sampled nearby bearings, reading as sharp,
    // blocky transitions. Longitude wraps; latitude clamps at the poles.
    auto sampleDomeLum = [this](float u, float v) -> float
    {
        float fx = u * (float)earthNightCpuBlurW - 0.5f;
        float fy = v * (float)earthNightCpuBlurH - 0.5f;
        int x0 = (int)floorf(fx), y0 = (int)floorf(fy);
        float tx = fx - (float)x0, ty = fy - (float)y0;
        auto wrapX = [this](int x) { return ((x % earthNightCpuBlurW) + earthNightCpuBlurW) % earthNightCpuBlurW; };
        int x0w = wrapX(x0), x1w = wrapX(x0 + 1);
        int y0c = std::clamp(y0, 0, earthNightCpuBlurH - 1);
        int y1c = std::clamp(y0 + 1, 0, earthNightCpuBlurH - 1);
        float v00 = earthNightCpuBlur[y0c * earthNightCpuBlurW + x0w] / 255.0f;
        float v10 = earthNightCpuBlur[y0c * earthNightCpuBlurW + x1w] / 255.0f;
        float v01 = earthNightCpuBlur[y1c * earthNightCpuBlurW + x0w] / 255.0f;
        float v11 = earthNightCpuBlur[y1c * earthNightCpuBlurW + x1w] / 255.0f;
        return glm::mix(glm::mix(v00, v10, tx), glm::mix(v01, v11, tx), ty);
    };

    // Same brightness-response curve (kNightFloor/kCityCompressK) as the sky-glow/cloud
    // city-light effects in sat_sky.frag, so all of these read one consistent "how bright is
    // this city" signal instead of drifting out of tune with each other.
    const float kNightFloor = 0.06f, kCityCompressK = 0.08f;
    // 2 km near sample added (session 26 follow-up): the observer's *own* position can sit inside
    // a bright pixel while every 8+ km ring around it is already dark countryside (small/isolated
    // towns) — without a near sample the dome can miss the pollution source entirely and read as
    // "no effect" even directly under city lights. This is the direct analog of the old scalar's
    // distance-0 sample, which this replaced.
    const float kSampleRadiiM[4] = {2000.0f, 8000.0f, 20000.0f, 45000.0f};
    const float kRadiusFalloffM = 20000.0f;
    float obsLatRad = glm::radians(obsLatDeg);
    float obsLonRad = glm::radians(obsLonDeg);
    float cosObsLat = std::max(0.05f, cosf(obsLatRad)); // guard near the poles

    for (int sec = 0; sec < kNumLightSectors; ++sec)
    {
        float bearing = (float(sec) + 0.5f) * (2.0f * glm::pi<float>() / float(kNumLightSectors));
        float domeRaw = 0.0f;
        for (float D : kSampleRadiiM)
        {
            float dLat = (D / kEarthRadius) * cosf(bearing);
            float dLon = (D / kEarthRadius) * sinf(bearing) / cosObsLat;
            float sampleLatRad = glm::clamp(obsLatRad + dLat, -glm::half_pi<float>(), glm::half_pi<float>());
            float sampleLonRad = obsLonRad + dLon;
            while (sampleLonRad > glm::pi<float>())
                sampleLonRad -= 2.0f * glm::pi<float>();
            while (sampleLonRad < -glm::pi<float>())
                sampleLonRad += 2.0f * glm::pi<float>();

            float u = (sampleLonRad + glm::pi<float>()) / (2.0f * glm::pi<float>());
            float v = (0.5f * glm::pi<float>() - sampleLatRad) / glm::pi<float>();
            float lum = sampleDomeLum(u, v);
            float raw = std::max(0.0f, lum - kNightFloor);
            float weight = expf(-D / kRadiusFalloffM);
            domeRaw = std::max(domeRaw, raw * weight);
        }
        float cityBrightness = domeRaw / (domeRaw + kCityCompressK);
        // lightPollutionGain applied once at the source so satellites (via lightDomeBuf) and
        // stars (reading lightDomeAz[] directly) stay coherently scaled by construction — same
        // array, not two separately-tuned gains that could drift apart like daySuppression vs.
        // the stars' fixed kStarPollutionMaxDim did.
        // NOT clamped to 1.0 here (session 26 follow-up — this was a real bug): elevFalloff
        // (applied downstream by each consumer, ≤1 for anything off the horizon) was multiplying
        // an already-saturated value, so no gain above the point where cityBrightness*altFalloff*
        // gain first hit 1.0 (around gain≈5) could push non-horizon directions any brighter —
        // gain=500 read identically to gain=5. Leaving this unclamped lets high gain compensate
        // for elevFalloff's reduction; the final domeVal clamp downstream still bounds the result.
        lightDomeAz[sec] = cityBrightness * altFalloff * lightPollutionGain;
    }

    // Circular smoothing pass (session 26 follow-up): each sector is a single bearing ray, so a
    // real city's edge — which doesn't line up with 22.5° sector boundaries — can put a bright
    // sector directly next to a dark one. Center-to-center interpolation (in the consumers) only
    // smooths *within* a sector's span; it doesn't reduce how different two neighboring raw
    // values are, so sharp swings still read as fast, unsubtle pops when panning across a sector
    // boundary — worst exactly at the horizon, where elevFalloff is largest and fully exposes any
    // sampling noise. 5-tap blur (~±45°) trades a little directional sharpness for removing that
    // noise while keeping the broad "city here, dark ocean there" structure intact.
    float smoothed[kNumLightSectors];
    const float kBlurWeights[5] = {0.1f, 0.2f, 0.4f, 0.2f, 0.1f};
    for (int i = 0; i < kNumLightSectors; ++i)
    {
        float acc = 0.0f;
        for (int k = -2; k <= 2; ++k)
        {
            int idx = ((i + k) % kNumLightSectors + kNumLightSectors) % kNumLightSectors;
            acc += lightDomeAz[idx] * kBlurWeights[k + 2];
        }
        smoothed[i] = acc;
    }
    memcpy(lightDomeAz, smoothed, sizeof(smoothed));

    memcpy(lightDomeMapped, lightDomeAz, sizeof(lightDomeAz));
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
    // atmFrac used to decay with the same 80 km scale height used for satellites' orbital day
    // suppression (correct for them — satellites fly hundreds of km up) — but reused here that
    // decayed too fast for a still-in-atmosphere observer: even a modest few-km cloud-deck
    // altitude already left atmFrac ~0.9, leaking ~10% of full night brightness into a clear
    // daytime sky, visible on the brightest stars. Real daytime sky glow doesn't meaningfully
    // thin until far above any cloud deck, so hold atmFrac at 1.0 (fully day/night gated, no
    // leak) through the whole flyable atmosphere and only fade toward "space, nothing to hide
    // behind" over the last stretch of the simulated atmosphere shell (R_ATMOS - R_EARTH = 100 km).
    const float kStarSpaceFadeStartM = 40000.0f;
    const float kStarSpaceFadeEndM   = 100000.0f;
    float obsR = glm::length(obsECI);
    float obsHeight = obsR - kEarthRadius;
    float atmFrac = 1.0f - glm::clamp((obsHeight - kStarSpaceFadeStartM)
                                       / (kStarSpaceFadeEndM - kStarSpaceFadeStartM), 0.0f, 1.0f);
    // skyGlareEased (computed once per frame in recordCompute(), right before this call) replaces
    // the old flat 1.0 space target — sun-on-screen or unshielded sunlight still gates visibility
    // even with no atmosphere left to explain it away. At atmFrac==1 (fully in-atmosphere) this
    // has no effect, since it's weighted out by (1-atmFrac)==0 and nightFactor alone governs.
    float nightFactorEff = glm::mix(skyGlareEased, nightFactor, atmFrac);

    // Earth-limb elevation cutoff: from altitude, stars are visible below the 0° horizon.
    float r = kEarthRadius / obsR;
    float limbSin = (obsHeight > 1.0f) ? -sqrtf(glm::max(0.0f, 1.0f - r * r)) : 0.0f;

    // Light pollution dome caps: kStarPollutionMaxDim caps how dark the directional dome
    // (lightDomeAz[], filled by updateLightPollutionDome() just above) can push a star, so the
    // brightest stars/planets still peek through even in a maximally lit city, like reality.
    const float kStarPollutionMaxDim = 0.99f;

    // Moonlight sky-brightness dimming: same physical ramp (elevation × phase illumination) as
    // sat_flare.comp's moonBright term, computed here CPU-side since stars are drawn from this
    // same per-frame pass. Reuses moonDirENU.w (illuminated fraction, already computed this frame
    // in updatePositions()) instead of re-deriving it from sunDirECI/moonDirECI. Unlike satellites'
    // user-tunable moonSuppression gain, this uses a fixed response cap — mirrors how nightFactor
    // above and kStarPollutionMaxDim are both fixed formulas with no settings-window knob; stars
    // have never exposed per-suppression-source sliders, only the geometry-driven inputs.
    float moonElevStar = glm::dot(moonDirECI, glm::normalize(obsECI));
    float tmStar = glm::clamp(moonElevStar / 0.5f, 0.0f, 1.0f);
    float moonBrightStar = tmStar * tmStar * moonDirENU.w;
    const float kStarMoonMaxDim = 0.9f; // full moon dims naked-eye stars but never Venus/Jupiter/Sirius

    auto *dst = static_cast<GpuSatVisible *>(starMapped);
    for (uint32_t i = 0; i < starCount; ++i)
    {
        const auto &rec = starRecords[i];

        // Rotate from inertial ECI into the observer's local ENU frame.
        glm::vec3 enu{glm::dot(rec.eciDir, glm::vec3(eci2enuX)),
                      glm::dot(rec.eciDir, glm::vec3(eci2enuY)),
                      glm::dot(rec.eciDir, glm::vec3(eci2enuZ))};

        // Directional light pollution: same interpolated-dome/elevFalloff formula as
        // sat_flare.comp, so stars and satellites read the same dome consistently in a given
        // direction. Interpolated between the two nearest sector CENTERS (not hard-binned) —
        // 16 discrete wedges still showed visible blocky transitions over wide, fairly uniform
        // bright regions (e.g. flying over Europe).
        float bearing = atan2f(enu.x, enu.y); // matches GPU's atan(skyDir.x, skyDir.y) convention
        if (bearing < 0.0f)
            bearing += 2.0f * glm::pi<float>();
        float secF = bearing * (float(kNumLightSectors) / (2.0f * glm::pi<float>())) - 0.5f;
        int sec0 = (int)floorf(secF);
        float secFrac = secF - float(sec0);
        int sec0w = ((sec0 % kNumLightSectors) + kNumLightSectors) % kNumLightSectors;
        int sec1w = (sec0w + 1) % kNumLightSectors;
        float domeAz = glm::mix(lightDomeAz[sec0w], lightDomeAz[sec1w], secFrac);
        // 0.35 (not 0.15): matches the sat_flare.comp softening — the steeper curve crushed the
        // effect to near-zero above ~20° elevation, where most visible stars actually sit.
        float elevFalloff = 0.35f / (std::max(enu.z, 0.0f) + 0.35f); // 1.0 at horizon, ~0.26 at zenith
        // domeAz is intentionally unclamped upstream — the clamp only happens here, after
        // elevFalloff, so high lightPollutionGain can compensate for elevFalloff's reduction at
        // non-horizon angles instead of saturating uselessly before elevFalloff is even applied.
        float domeVal = glm::clamp(domeAz * elevFalloff, 0.0f, 1.0f);

        // C12 follow-up #31: same suppression shape, second independent source — a nearby
        // Reflect-Orbital beam should wash out this star the same way real light pollution does.
        // beamGlowDomeAz[] is the one-frame-stale CPU readback of sat_orbit.comp's atomicMax'd
        // dome (see recordCompute()'s top-of-frame read), same interpolation convention as
        // lightDomeAz above.
        float beamDomeAz = glm::mix(beamGlowDomeAz[sec0w], beamGlowDomeAz[sec1w], secFrac);
        float beamDomeVal = glm::clamp(beamDomeAz * elevFalloff, 0.0f, 1.0f);
        const float kStarBeamPollutionMaxDim = 0.99f;

        // Atmospheric extinction (airmass) — same Kasten & Young 1989 approximation as
        // sat_flare.comp, applied identically so a star and a satellite at the same elevation
        // dim by the same amount. Independent of light pollution/moon; this is what gives the
        // pollution dome's directional variation a smooth baseline to sit on top of instead of
        // being the only source of horizon-vs-zenith brightness difference.
        float sinElClamped = glm::clamp(enu.z, 0.0f, 1.0f);
        float elDeg = glm::degrees(asinf(sinElClamped));
        float airmass = 1.0f / (sinElClamped + 0.50572f * powf(elDeg + 6.07995f, -1.6364f));
        float extinctMag = extinctionCoeff * (airmass - 1.0f) * atmFrac;
        float extinction = powf(10.0f, -0.4f * extinctMag);

        // Above the Earth limb: visible. Below: culled.
        float intensity = (enu.z >= limbSin)
                              ? rec.rawIntensity * nightFactorEff * extinction
                                * (1.0f - domeVal * kStarPollutionMaxDim)
                                * (1.0f - beamDomeVal * kStarBeamPollutionMaxDim)
                                * (1.0f - moonBrightStar * kStarMoonMaxDim)
                              : 0.0f;

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
    // Default every slot to sea level first (covers indices 0 and kNumReflectorTargets-1, which
    // the loop below doesn't touch) before the loop overrides 1..kNumReflectorTargets-2 with real
    // per-target terrain elevation.
    for (int ti = 0; ti < kNumReflectorTargets; ++ti)
        reflectorTargetsRadiusM[ti] = kEarthRadius;
    for (int ti = 1; ti < kNumReflectorTargets - 1; ++ti)
    {
        // Uniform sampling on sphere: latitude from arcsin of uniform[-1,1],
        // longitude uniform [0, 2π).
        float sinLat = (float)rand() / RAND_MAX * 2.0f - 1.0f;
        float cosLat = sqrtf(std::max(0.0f, 1.0f - sinLat * sinLat));
        float lon = (float)rand() / RAND_MAX * glm::two_pi<float>();
        reflectorTargetsECEF[ti] = glm::vec3(cosLat * cosf(lon), cosLat * sinf(lon), sinLat);

        // Real terrain elevation at this target's lat/lon (C12 follow-up #18) — without this,
        // every target was placed on the sea-level sphere regardless of actual ground height,
        // putting the "ground" endpoint of any beam-related ray for an elevated target (mountains,
        // plateaus) underground relative to the terrain actually rendered there. Same CPU-side
        // earthElevCpu lookup/formula as the observer's own terrain height above.
        if (!earthElevCpu.empty())
        {
            // Derive lon/lat back from the ECEF vector just built (atan2 gives [-π, π] regardless
            // of how `lon` above was originally sampled) — same convention the observer's own
            // lookup and updatePositions() both use, avoiding any wrap-convention mismatch.
            float lonRad = atan2f(reflectorTargetsECEF[ti].y, reflectorTargetsECEF[ti].x);
            float latRad = asinf(sinLat);
            float u = (lonRad + glm::pi<float>()) / (2.0f * glm::pi<float>());
            float v = (0.5f * glm::pi<float>() - latRad) / glm::pi<float>();
            int px = std::clamp((int)(u * (float)earthElevCpuW), 0, earthElevCpuW - 1);
            int py = std::clamp((int)(v * (float)earthElevCpuH), 0, earthElevCpuH - 1);
            // MAX over a small neighborhood, not a single point sample (C12 follow-up #23) — user
            // reported beams still converging visibly below the rendered terrain surface. earthElevCpu
            // is itself a 10x point-sampled downsample of the real 21600x10800 elevation texture
            // (see createGlowResources()), so a single lookup can land on a texel a full ~9km away
            // (10x the ~0.9km-per-texel source resolution) from the target's TRUE lat/lon, missing a
            // nearby peak entirely and underestimating height — the direct cause of "converge below
            // the surface." Taking the max of the surrounding 3x3 texels is a cheap, conservative
            // fix: it can only raise the estimate toward a real nearby peak, never lower it, so at
            // worst a target ends up slightly ABOVE ground (reads as "beam floats a little," far
            // less objectionable than "beam sinks into the hillside"). Combined with a small fixed
            // margin below for the same reason.
            const float kSeaLevel = 15.0f / 255.0f;
            uint8_t maxPix = 0;
            for (int dy = -1; dy <= 1; ++dy)
            {
                int py2 = std::clamp(py + dy, 0, earthElevCpuH - 1);
                for (int dx = -1; dx <= 1; ++dx)
                {
                    int px2 = ((px + dx) % earthElevCpuW + earthElevCpuW) % earthElevCpuW; // wrap longitude
                    maxPix = std::max(maxPix, earthElevCpu[py2 * earthElevCpuW + px2]);
                }
            }
            float pixVal = maxPix / 255.0f;
            float terrainH = (pixVal <= kSeaLevel) ? 0.0f : std::max(0.0f, (pixVal - kSeaLevel) * 8848.0f);
            const float kElevSafetyMarginM = 75.0f; // small fixed bias toward "above ground, not below"
            reflectorTargetsRadiusM[ti] = kEarthRadius + terrainH + kElevSafetyMarginM;
        }
    }
    // Last slot: fixed target at the observer spawn point (67°S, 67°W).
    // Guarantees at least one mirror always aims here when the site is on the night side.
    // (A duplicate assignment used to immediately follow this, overwriting this same slot with
    // glm::vec3(0,0,-1) — the geographic South Pole, ~2555 km from the actual 67°S/67°W spawn
    // point — under a "Zeroth slot / Antarctic Station" comment that didn't match the index it
    // wrote to (index 0 is left unset/reserved by the loop above starting at ti=1, but the
    // erroneous line wrote to kNumReflectorTargets-1 again instead). Found 2026-07-20 while
    // debugging why Reflect-Orbital beams never appeared near the observer: this was silently
    // breaking the one guaranteed-nearby target the horizon-visibility gate depends on. Removed;
    // if a real fixed Antarctic Station target is wanted, it belongs at index 0, not here.)

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

    // ── Milky Way skybox basis: ENU -> galactic, recomputed each frame ───────────────────────
    // The base ECI->Galactic rotation only depends on fixed IAU constants (galX/Y/Z below are
    // static in ECI, which is itself inertial), so this could be cached — recomputing is cheap
    // (a handful of dot/cross products) and keeps it self-contained alongside eci2enu above,
    // which is rebuilt every frame for the same reason despite most of its own inputs (lat/lon)
    // changing slowly too.
    // Alignment was confirmed by eye against assets/textures/8k_stars_milky_way.jpg: the only
    // correction needed versus the raw IAU rotation was a longitude mirror (galY negated) — no
    // yaw/pitch/roll tweak or V-flip. That was previously exposed as runtime sliders/toggles;
    // removed once confirmed correct, this is just the fixed result.
    {
        auto raDecToVec = [](float raDeg, float decDeg) {
            float ra = glm::radians(raDeg);
            float dec = glm::radians(decDeg);
            return glm::vec3{cosf(dec) * cosf(ra), cosf(dec) * sinf(ra), sinf(dec)};
        };
        // IAU 1958 galactic coordinate system constants (J2000 equatorial).
        glm::vec3 galZ = raDecToVec(192.859508f, 27.128336f);   // North Galactic Pole
        glm::vec3 gcDir = raDecToVec(266.405100f, -28.936175f); // Galactic center direction
        glm::vec3 galX = glm::normalize(gcDir - glm::dot(gcDir, galZ) * galZ);
        glm::vec3 galY = -glm::cross(galZ, galX); // negated: confirmed longitude mirror (was "Flip U")

        // dirGal = M * dirECI where M's rows are (galX,galY,galZ) expressed in ECI coords, and
        // dirECI = east*enu.x + north*enu.y + up*enu.z, so each row dotted against enuDir in the
        // shader needs to already be expressed in the ENU basis — project each gal axis onto
        // east/north/up here so the shader can do a single dot(enuDir, mwRowN).
        mwRow0 = glm::vec3(glm::dot(galX, east), glm::dot(galX, north), glm::dot(galX, up));
        mwRow1 = glm::vec3(glm::dot(galY, east), glm::dot(galY, north), glm::dot(galY, up));
        mwRow2 = glm::vec3(glm::dot(galZ, east), glm::dot(galZ, north), glm::dot(galZ, up));
    }

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
            // reflectorTargetsRadiusM (C12 follow-up #18), not the bare kEarthRadius — accounts
            // for real terrain elevation at this target so beams aim at the actual ground surface
            // instead of the sea-level sphere (which sat underneath any elevated terrain).
            glm::vec3 eci = reflectorTargetsRadiusM[ti] * glm::vec3(
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
