// ─── Startup graphics chooser (2026-10-06) ──────────────────────────────────────────────────────────
// The user: "before even loading in shaders, sat shapes, a window instructing the user to choose between
// Planetarium and Full Graphics modes ... If a machine cannot handle planetarium, then we can revert down to
// potato ... toggleable like the intro ... so we do not just freeze up someone's PC on bootup."
//
// App (App::runBootChooser) draws the chooser on the loading screen BEFORE init(): only the UI pipeline
// exists then. This file is the sim's half:
//   prepareBootChooser  — what to offer and pre-select (a cheap look at settings.json + the crash sentinel)
//   setBootChoice       — the pick, applied in init() after loadSettings (applyBootChoice)
//   fullSkyDeferred     — on a light tier init() never builds the three FULL sat_sky.frag pipelines;
//                         ensureFullSkyPipelines() builds them the first frame anything needs them
//   updateBootSafetyNet — the first seconds after loading at > 250 ms a frame step one tier down
#include "SatelliteSim.h"
#include "../Harness.h"
#include "../Log.h"
#include "../Paths.h"
#include <nlohmann/json.hpp>
#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>

namespace
{
constexpr uint32_t kBitMinimalSkyB = 262144u, kBitLiteSkyB = 524288u;
// The chooser's options (App shows them in this order).
enum BootMode : int
{
    kBootFull = 0,
    kBootPlanetarium = 1,
    kBootPotato = 2
};
int bootModeOf(int preset, uint32_t mask)
{
    if (preset == (int)GraphicsPreset::Potato) return kBootPotato;
    if (preset == (int)GraphicsPreset::Planetarium) return kBootPlanetarium;
    if (preset == (int)GraphicsPreset::Custom || preset < 0)
    { // a hand-tuned Custom: whichever sky its knockout mask draws
        if (mask & kBitMinimalSkyB) return kBootPotato;
        if (mask & kBitLiteSkyB) return kBootPlanetarium;
    }
    return kBootFull;
}
double steadyNowS()
{
    return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
} // namespace

// settings.json and the crash sentinel, read before init(): nothing here touches the GPU, and it must run
// before init() recreates the sentinel (session.lock) for this run.
void SatelliteSim::peekBootSettings()
{
    if (bootPeekDone) return;
    bootPeekDone = true;
    const std::filesystem::path dir = Paths::userDataDir();
    std::error_code ec;
    bootPeekCrash = std::filesystem::exists(dir / "session.lock", ec);
#ifdef SAT_FRESH_SETTINGS
    return; // every launch is a first run
#endif
    std::ifstream f(dir / "settings.json");
    if (!f.is_open()) return;
    try
    {
        nlohmann::json j;
        f >> j;
        bootPeekHasFile = true;
        if (j.contains("display") && j["display"].is_object())
        {
            const auto &d = j["display"];
            bootPeekAsk = d.value("ask_graphics_mode", true);
            bootPeekPreset = d.value("graphics_preset", -1);
            bootPeekMask = (uint32_t)d.value("debug_disable_mask", (int64_t)0);
        }
    }
    catch (...)
    {
        bootPeekHasFile = false; // a malformed file: loadSettings will log it; ask as on a first run
    }
}

// The device's recommendation among the chooser's modes: the first-run seed (recommendedPresetForDevice:
// MoltenVK on a non-Apple GPU -> Potato, integrated / CPU / virtual or <= 2.25 GiB -> Planetarium, else Medium).
int SatelliteSim::deviceRecommendedBootMode(VulkanContext &ctx) const
{
    const GraphicsPreset p = seedGraphicsPresetFromDevice(ctx);
    return p == GraphicsPreset::Potato ? kBootPotato : p == GraphicsPreset::Planetarium ? kBootPlanetarium : kBootFull;
}

// "Full graphics" when the saved preset is a light one (or there is none): the device seed if it is a full
// tier, else Low (Medium's effects at a 50% render scale) — an integrated GPU's user who asked for clouds.
GraphicsPreset SatelliteSim::recommendedFullPreset(VulkanContext &ctx) const
{
    const GraphicsPreset seed = seedGraphicsPresetFromDevice(ctx);
    if (seed == GraphicsPreset::Planetarium || seed == GraphicsPreset::Potato) return GraphicsPreset::Low;
    return seed;
}

bool SatelliteSim::prepareBootChooser(VulkanContext &ctx, BootChooserSpec &spec)
{
    peekBootSettings();
    spec.title = "Choose a graphics mode";
    spec.subtitle = "Nothing heavy has loaded yet. If this computer is old or has integrated graphics, pick a lighter mode.";
    spec.options = {
        {"Full graphics", "Volumetric clouds and weather, detailed terrain, aurora and the full sky. For a dedicated graphics card.", false},
        {"Planetarium", "The Earth and its atmosphere, the stars and every satellite, without clouds. For laptops and integrated graphics.", false},
        {"Potato (very old hardware)", "The simplest sky, for machines that struggle with Planetarium.", true},
    };
    const int rec = deviceRecommendedBootMode(ctx);
    spec.recommendedIdx = rec;
    int def = rec;
    if (bootPeekHasFile && bootPeekPreset >= 0) // a returning player: what they ran last time
        def = bootModeOf(bootPeekPreset, bootPeekMask);
    if (bootPeekCrash)
    { // offer the step down: the last session never reached its clean exit
        def = std::min(def + 1, (int)kBootPotato);
        spec.note = "The last session did not close properly, so a lighter mode is selected.";
    }
    spec.defaultIdx = def;
    spec.askAgain = bootPeekAsk;
    return bootPeekAsk || bootPeekCrash;
}

void SatelliteSim::setBootChoice(int option, bool askAgain)
{
    bootChoiceMode = std::clamp(option, (int)kBootFull, (int)kBootPotato);
    bootChoiceAsk = askAgain;
}

// After loadSettings (and in place of the crash path's forced Planetarium: the player has just chosen).
void SatelliteSim::applyBootChoice(VulkanContext &ctx, bool crashDetected)
{
    if (bootChoiceMode < 0) return;
    askGraphicsModeOnStartup = bootChoiceAsk;
    if (crashDetected) crashRecoveryMode = true; // still no intro benchmark this launch
    const int now = bootModeOf((int)graphicsPreset, debugDisableMask);
    if (bootChoiceMode == kBootPlanetarium && graphicsPreset != GraphicsPreset::Planetarium)
        applyGraphicsPreset(GraphicsPreset::Planetarium);
    else if (bootChoiceMode == kBootPotato && graphicsPreset != GraphicsPreset::Potato)
        applyGraphicsPreset(GraphicsPreset::Potato);
    else if (bootChoiceMode == kBootFull && now != kBootFull)
        applyGraphicsPreset(recommendedFullPreset(ctx)); // a full tier already loaded (or Custom) is kept
    Log::line(std::string("boot chooser: running ") + kGraphicsPresetNames[(int)graphicsPreset]);
}

// The FULL sat_sky.frag is drawn by: the inline sky (no light-tier bit), the low-res prepass (any tier
// below render scale 100%: it has no lite / minimal variant) and the sky TAA (full tier only).
bool SatelliteSim::fullSkyNeeded() const
{
    return (debugDisableMask & (kBitMinimalSkyB | kBitLiteSkyB)) == 0u || renderScale < 0.999f;
}

// Called at the top of recordCompute, before anything is recorded: the previous frame is finished (single
// frame in flight, fence waited), so the pipelines can be rebuilt the way onResize does.
void SatelliteSim::ensureFullSkyPipelines(VulkanContext &ctx)
{
    if (!fullSkyDeferred || !fullSkyNeeded()) return;
    fullSkyDeferred = false;
    const double t0 = steadyNowS();
    vkDestroyPipeline(ctx.device, skyBgPipeline, nullptr);
    skyBgPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx.device, skyBgMinimalPipeline, nullptr);
    skyBgMinimalPipeline = VK_NULL_HANDLE;
    vkDestroyPipeline(ctx.device, skyBgLitePipeline, nullptr);
    skyBgLitePipeline = VK_NULL_HANDLE;
    createSkyBgPipeline(ctx);
    destroySkyLowResResources(ctx.device);
    createSkyLowResResources(ctx);
    destroySkyTaaResources(ctx.device);
    createSkyTaaResources(ctx);
    char msg[128];
    std::snprintf(msg, sizeof(msg), "full sky pipelines created on demand (%.0f ms)", (steadyNowS() - t0) * 1000.0);
    Log::line(msg);
}

// The automatic step down: if the first frames after loading run slower than 250 ms each (median of 10,
// after ~2 s of warm-up for the first-frame pipeline compiles), drop one tier — a full tier to Planetarium,
// Planetarium to Potato — and say so. Once per launch, wall clock. Off in a harness run unless
// SATLIGHTSIM_BOOT_SAFETY_MS sets the threshold (to test it).
void SatelliteSim::updateBootSafetyNet()
{
    if (bootSafetyDone) return;
    float thresholdMs = 250.0f;
    if (const char *e = std::getenv("SATLIGHTSIM_BOOT_SAFETY_MS"))
        thresholdMs = (float)std::atof(e);
    else if (harness::active())
    {
        bootSafetyDone = true;
        return;
    }
    if (thresholdMs <= 0.0f)
    {
        bootSafetyDone = true;
        return;
    }
    const double now = steadyNowS();
    if (bootSafetyPrevT == 0.0)
    {
        bootSafetyT0 = bootSafetyPrevT = now;
        return;
    }
    const float ms = (float)((now - bootSafetyPrevT) * 1000.0);
    bootSafetyPrevT = now;
    // Warm-up: the first frames compile pipelines and fill caches. Then 10 frames — or, on a machine so slow
    // that 10 frames would take long, as many as fit in 5 s (at least 3).
    if (++bootSafetyWarm <= 5 || now - bootSafetyT0 < 2.0 || photoScaleActive)
        return;
    bootSafetyMs.push_back(ms);
    float sampledMs = 0.0f;
    for (float x : bootSafetyMs) sampledMs += x;
    if (bootSafetyMs.size() < 10 && !(bootSafetyMs.size() >= 3 && sampledMs > 5000.0f)) return;
    bootSafetyDone = true;
    std::vector<float> v = bootSafetyMs;
    std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
    const float median = v[v.size() / 2];
    char msg[160];
    std::snprintf(msg, sizeof(msg), "boot safety net: median %.0f ms over %zu frames on %s (threshold %.0f)", median,
                  v.size(), kGraphicsPresetNames[(int)graphicsPreset], thresholdMs);
    Log::line(msg);
    if (median <= thresholdMs || graphicsPreset == GraphicsPreset::Potato) return;
    const GraphicsPreset to = bootModeOf((int)graphicsPreset, debugDisableMask) == kBootFull ? GraphicsPreset::Planetarium
                                                                                            : GraphicsPreset::Potato;
    applyGraphicsPreset(to);
    snprintf(graphicsAutoNoticeText, sizeof(graphicsAutoNoticeText),
             "Frames took %.0f ms: switched to %s to keep this computer responsive. Change it in Settings > Display.",
             median, kGraphicsPresetNames[(int)graphicsPreset]);
    graphicsAutoNoticeTimer = 12.0f;
    Log::line(std::string("boot safety net: switched to ") + kGraphicsPresetNames[(int)graphicsPreset]);
}
