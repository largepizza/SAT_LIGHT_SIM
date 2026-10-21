// ── Automation harness: SatelliteSim's commands ─────────────────────────────────────────────────
// The command language itself (parsing, queue, run folder, live inbox) is src/Harness.h/.cpp; this
// file is what each command DOES to the sim. Reference with examples: docs/HARNESS.md. Keep that
// document's command table in step with harnessExec() below — it is what other agents read.
//
// Everything here runs at the top of buildUI (harnessTick), i.e. before this frame's camera
// derivation, recordCompute and draws: a command's effect is in the very frame it ran in, and a
// `capture` issued after it records that frame.
#include "UIPalette.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "Launches.h"
#include "Ambience.h"
#include "../AudioSystem.h"
#include "../Harness.h"
#include "../Log.h"
#include "../MusicAnalysis.h"
#include "version.h"
#include "clay.h" // ui click: element lookup

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <stdexcept>

namespace fs = std::filesystem;
using harness::Status;
using nlohmann::json;

void formatSimClock(double tJ2000, char *buf, size_t n, bool withDate); // SatelliteSim.cpp

namespace
{
[[noreturn]] void fail(const std::string &msg) { throw std::runtime_error(msg); }

std::string lower(std::string s)
{
    for (char &c : s)
        c = (char)tolower((unsigned char)c);
    return s;
}

bool ieq(const std::string &a, const char *b) { return lower(a) == lower(std::string(b)); }

double parseNum(const std::string &s, const char *what)
{
    char *end = nullptr;
    double v = strtod(s.c_str(), &end);
    if (s.empty() || end == s.c_str() || *end != '\0')
        fail(std::string(what) + ": expected a number, got '" + s + "'");
    return v;
}

// Civil date <-> days since 1970-01-01 (proleptic Gregorian; H. Hinnant's algorithms).
int64_t daysFromCivil(int64_t y, unsigned m, unsigned d)
{
    y -= m <= 2;
    const int64_t era = (y >= 0 ? y : y - 399) / 400;
    const unsigned yoe = (unsigned)(y - era * 400);
    const unsigned doy = (153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (int64_t)doe - 719468;
}
void civilFromDays(int64_t z, int64_t &y, unsigned &m, unsigned &d)
{
    z += 719468;
    const int64_t era = (z >= 0 ? z : z - 146096) / 146097;
    const unsigned doe = (unsigned)(z - era * 146097);
    const unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365;
    y = (int64_t)yoe + era * 400;
    const unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100);
    const unsigned mp = (5 * doy + 2) / 153;
    d = doy - (153 * mp + 2) / 5 + 1;
    m = mp < 10 ? mp + 3 : mp - 9;
    y += m <= 2;
}

// The sim's clock is seconds since J2000 = 2000-01-01T12:00:00 UTC, and its Earth rotation angle is GMST
// (satphot::earthRotationAngle), so a `time set` UTC is the real one.
constexpr int64_t kJ2000Unix = 946728000;

double j2000FromIso(const std::string &iso)
{
    int Y = 0, M = 0, D = 0, h = 0, mi = 0;
    double sec = 0.0;
    int n = sscanf(iso.c_str(), "%d-%d-%dT%d:%d:%lf", &Y, &M, &D, &h, &mi, &sec);
    if (n < 3)
        n = sscanf(iso.c_str(), "%d-%d-%d %d:%d:%lf", &Y, &M, &D, &h, &mi, &sec);
    if (n < 3 || M < 1 || M > 12 || D < 1 || D > 31)
        fail("time: expected an ISO time like 2036-06-21T02:10:00Z, got '" + iso + "'");
    const int64_t days = daysFromCivil(Y, (unsigned)M, (unsigned)D);
    return (double)(days * 86400 - kJ2000Unix) + h * 3600.0 + mi * 60.0 + sec;
}

std::string isoFromJ2000(double t)
{
    const double unixS = t + (double)kJ2000Unix;
    int64_t days = (int64_t)std::floor(unixS / 86400.0);
    double sod = unixS - (double)days * 86400.0;
    int64_t y;
    unsigned m, d;
    civilFromDays(days, y, m, d);
    int hh = (int)(sod / 3600.0);
    int mm = (int)((sod - hh * 3600.0) / 60.0);
    double ss = sod - hh * 3600.0 - mm * 60.0;
    char buf[48];
    snprintf(buf, sizeof(buf), "%04lld-%02u-%02uT%02d:%02d:%06.3fZ", (long long)y, m, d, hh, mm, ss);
    return buf;
}

float azDegOf(const glm::vec3 &enu) { return glm::degrees(atan2f(enu.x, enu.y)); }
float elDegOf(const glm::vec3 &enu) { return glm::degrees(asinf(glm::clamp(enu.z, -1.0f, 1.0f))); }

// Output names become file names.
std::string safeName(const std::string &s)
{
    if (s.empty())
        fail("a name is required");
    std::string o;
    for (char c : s)
        o += (isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
    return o;
}

bool pngSize(const std::string &path, uint32_t &w, uint32_t &h)
{
    std::ifstream f(path, std::ios::binary);
    unsigned char b[24];
    if (!f.read((char *)b, 24))
        return false;
    w = (b[16] << 24) | (b[17] << 16) | (b[18] << 8) | b[19];
    h = (b[20] << 24) | (b[21] << 16) | (b[22] << 8) | b[23];
    return true;
}

// `set a.b.c v`: value as JSON when it parses (numbers, true/false, arrays, quoted strings), else a
// bare string.
json parseValue(const std::string &v)
{
    try
    {
        return json::parse(v);
    }
    catch (...)
    {
        return json(v);
    }
}

const json *lookupPath(const json &root, const std::string &dotted)
{
    const json *cur = &root;
    size_t start = 0;
    while (start <= dotted.size())
    {
        size_t dot = dotted.find('.', start);
        std::string part = dotted.substr(start, dot == std::string::npos ? std::string::npos : dot - start);
        if (cur->is_array() && !part.empty() && part.find_first_not_of("0123456789") == std::string::npos)
        {
            const size_t i = (size_t)std::stoul(part); // an index into an array (cine.shots.0.keys.1.t)
            if (i >= cur->size())
                return nullptr;
            cur = &(*cur)[i];
        }
        else if (!cur->is_object() || !cur->contains(part))
            return nullptr;
        else
            cur = &(*cur)[part];
        if (dot == std::string::npos)
            break;
        start = dot + 1;
    }
    return cur;
}

json patchFor(const std::string &dotted, const json &value)
{
    json root = json::object();
    json *cur = &root;
    size_t start = 0;
    while (true)
    {
        size_t dot = dotted.find('.', start);
        if (dot == std::string::npos)
        {
            (*cur)[dotted.substr(start)] = value;
            break;
        }
        cur = &(*cur)[dotted.substr(start, dot - start)];
        start = dot + 1;
    }
    return root;
}

const char *kHelp =
    "wait <frames> | wait seconds <s> | wait settle [frames]; "
    "time [set <iso>|add <s>|sun <el deg|noon|midnight> [rising|setting]|pause|play|scale <label>|reverse on/off]; "
    "observer lat= lon= [agl=|alt=]; beams [list] | beams go [rank=] [dist_km=] [bearing=] [agl=|alt=] [look=site|up|none]; snapshot <profile_log.jsonl> [index=-1] [settings=on|off] [clamp=on] [drift=intro|default]; report list [dir] | report info <issue dir> | report load <issue dir> [shot=1] [settings=on|off] [clamp=on] [trail=on|off] | report shot [note] [new=1] [title=] [bar=1] | report mark [issue] shot= [pin=x,y|off] [rect=x,y,w,h|off] | report export [issue]; drift [default] [phase=] [rate=]; camera [az= el= fov=] | camera look|track <sun|moon|sel|planet name|off>; "
    "select sat <i> | select const <name> [n=<k>] | select planet <name> | select none; follow [off] [offset=x,y,z]; track [on|off]; viewer [aim=free|observer|toward|sun] [light=live|studio] [glare=on|off] [shadows=on|off] [dist=<radii>]; "
    "const <name|all> on|off [highlight=on|off] | const list; set <section.key> <value>; get [section[.key]]; expect <key|cine.path> <value> [tol=]; "
    "preset <name>; knockout <none|mask|+key|-key ...> | knockout list; capture <name> [ui=on] [crop=x,y,w,h] [scale=s]; photo <name> [scale=1-4] [frames=N]; "
    "state [name]; probe <x> <y>; perf [frames=N] [name=]; sweep; shaders reload [march=<spv>] [wg=<X>x<Y>]; lightning; eclipse solar|lunar; aurora [next <kp>|substorm|kp <v>|auto]; tutorial [start [step]|next|back|skip|state]; missions [state]|go <i|twilight|crew|fury|fury_night> [watch=1]|tab <name>; launch [state]|list [hours=]|next [when=any|twilight|day|night]|look [stage=]|select [kind=]|reentry|vent|ship|check|padstats [hours=]; deploy [state]|stats [from=] [to=] [step=]; bookmark add [name] [id=] [scale=] [paused=]|go <n>|update <n>|rename <n> <name>|delete <n>|restore|list; pad <a|b|x|y|start|view|up|down|left|right|...>; debugview <off|normals|detail|steps|albedo|shadow|rough|elevzebra|distzebra|oceanrefl..|cloudairsplit..>; ui show|hide|scale <x>|open <win> [tab=]|close <win|all>|click <Id>[:i]|type <text>|key <name>; cine new|name|shot|key|play|export|save|load|state|stop; "
    "window <W>x<H>; path clear|key <t> ...|goto <t>|play [fps=] [record=]; overlay text|label|clear ...; "
    "audio [state [name]] | audio record <name> [seconds=] [bus=] [solo=] | audio expect <layers> [absent=] | "
    "audio force <layer> <gain|off> | audio music [next|prev|pause|play|end] | audio tonality [wait]; log <text>; quit";
} // namespace

// ─── Lifecycle ────────────────────────────────────────────────────────────────────────────────────
// The aurora's activity this frame (SpaceWeather.h) for `state` and the `aurora` command.
nlohmann::json SatelliteSim::auroraStateJson() const
{
    const SpaceWeather &w = spaceWeather_;
    const AuroraOvalBounds b = auroraOvalBounds(w.kp);
    nlohmann::json j = {{"auto", auroraActivityAuto >= 0.5f}, {"kp", w.kp}, {"g_scale", geomagStormScale(w.kp)},
                        {"quiet", w.kpQuiet}, {"hss", w.hss}, {"cme", w.cme}, {"cme_peak_kp", w.cmePeakKp},
                        {"cme_hours", w.cmeHoursSince}, {"solar_cycle", w.cycle}, {"storm01", stormStrength}};
    j["oval_mlat_deg"] = {{"eq_midnight", 90.0 - b.eqMid}, {"eq_noon", 90.0 - b.eqNoon},
                          {"pol_midnight", 90.0 - b.polMid}, {"pol_noon", 90.0 - b.polNoon}};
    j["substorm"] = {{"intensity", w.subI}, {"minutes", w.subMinutes}, {"mlt_h", 24.0 + w.subMltRad * 12.0 / 3.14159265},
                     {"halfwidth_h", w.subHalfWRad * 12.0 / 3.14159265}, {"poleward_deg", w.subPoleDeg}};
    // The observer under the oval: magnetic latitude and MLT, and the oval's brightness overhead.
    {
        const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir)), ax = geomagPoleEcef();
        const glm::dvec3 m(auroraGpu_.midnight), e = glm::cross(ax, m);
        const double mlat = glm::degrees(std::asin(std::clamp(glm::dot(up, ax), -1.0, 1.0)));
        double mlt = std::atan2(glm::dot(up, e), glm::dot(up, m)) * 12.0 / 3.14159265;
        if (mlt < 0.0) mlt += 24.0;
        j["observer"] = {{"mlat_deg", mlat}, {"mlt_h", mlt}, {"oval_overhead", auroraOvalWeightCpu(auroraGpu_, up)}};
    }
    return j;
}

void SatelliteSim::harnessInit()
{
    if (!harness::active())
        return;
    // A harness run starts in the scene, not the cinematic, and without first-run notices that would
    // otherwise sit in every UI capture for their first eight seconds.
    showIntro = false;
    tutAutoChecked = true; // nor the first-run tutorial (`tutorial start` shows it)
    graphicsAutoNoticeTimer = 0.0f;
    crashRecoveryNoticeTimer = 0.0f;
    harnessRunner_ = new harness::Runner();
    std::string err;
    if (!harnessRunner_->begin(err))
    {
        fprintf(stderr, "[harness] %s\n", err.c_str());
        Log::line("harness: " + err);
        harnessRunner_->writeSummary("script_error", err);
        harnessQuit_ = true;
        return;
    }
    harness::startWatchdog(harnessRunner_);
}

float SatelliteSim::frameDt(float realDt)
{
    if (cineFixedDt_ > 0.0f)
        return cineFixedDt_;   // a cinematic export owns the clock (review 17)
    if (!harnessRunner_)
        return realDt;
    if (harnessFixedDtOverride_ > 0.0f)
        return harnessFixedDtOverride_;
    if (harnessSettleFrames_ > 0)
        return kHarnessSettleDt;
    const float f = harness::options().fixedDt;
    return f > 0.0f ? f : realDt;
}

void SatelliteSim::harnessTick()
{
    if (!harnessRunner_ || harnessQuit_)
        return;
    if (harnessTrack_ != 0)
    {
        glm::vec3 d;
        if (harnessLookDir(harnessTrack_, harnessTrackPlanet_, d))
            aimCameraAzEl(azDegOf(d), elDegOf(d));
    }
    harnessRunner_->tick([this](harness::Active &a) { return harnessExec(a); });
    const bool done = harnessRunner_->quitRequested() ||
                      (harness::options().exitWhenDone && harnessRunner_->scriptFinished());
    if (done)
    {
        harnessRunner_->writeSummary(harnessRunner_->errorCount() ? "errors" : "ok");
        harnessQuit_ = true;
    }
}

// Aiming the camera from the harness goes through SatelliteSim::aimCameraAzEl() (it is shared with the
// selection panel's Track lock, so it lives next to updateTrack() in SatelliteSim.cpp): obsFacing is
// the authority, and buildUI derives camera.azDeg from it every frame, so both are set there.

bool SatelliteSim::harnessLookDir(int track, int planet, glm::vec3 &d)
{
    switch (track)
    {
    case 1:
        if (selectedPlanetIndex >= 0)
            return harnessLookDir(4, selectedPlanetIndex, d);
        if (selectedSatIndex < 0)
            return false;
        updateSelectedSkyDir();
        d = selSkyDirCpu;
        return true;
    case 2:
        d = glm::vec3(sunDirENU);
        return true;
    case 3:
        d = glm::vec3(moonDirENU);
        return true;
    case 4:
    {
        if (planet < 0 || planet >= kPlanetCount)
            return false;
        const glm::dvec3 e = planetStates[planet].eciDir;
        d = glm::normalize(glm::vec3((float)glm::dot(e, glm::dvec3(eci2enuX)), (float)glm::dot(e, glm::dvec3(eci2enuY)),
                                     (float)glm::dot(e, glm::dvec3(eci2enuZ))));
        return true;
    }
    }
    return false;
}

// ─── Camera paths ─────────────────────────────────────────────────────────────────────────────────
SatelliteSim::HarnessCamKey SatelliteSim::harnessEvalPath(double t) const
{
    return cineEval(const_cast<SatelliteSim *>(this)->cineKeys(), t);
}
void SatelliteSim::harnessApplyCam(const HarnessCamKey &k)
{
    if (followActive)
        stopFollow();
    stopTrack(); // a scripted camera key is an explicit aim, so it releases the selection's Track lock
    const float lat = (float)glm::clamp(k.lat, -89.999, 89.999), lon = (float)k.lon;
    const float la = glm::radians(lat), lo = glm::radians(lon);
    obsDir = {cosf(la) * cosf(lo), cosf(la) * sinf(lo), sinf(la)};
    obsLatDeg = lat;
    obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
    obsHeightOffset = (float)std::max(0.0, k.alt);
    aimCameraAzEl((float)k.az, (float)k.el);
    camera.fovYDeg = glm::clamp((float)k.fov, SkyCamera::kMinFovDeg, SkyCamera::kMaxFovDeg);
    if (k.hasSim)
    {
        const double days = std::floor(k.simT / 86400.0);
        simDayJ2000 = (int64_t)days;
        simSecInDay = k.simT - days * 86400.0;
    }
}

// ─── State snapshot ───────────────────────────────────────────────────────────────────────────────
// Everything needed to reproduce (and interpret) a frame. It is written beside every capture.
json SatelliteSim::harnessStateJson()
{
    json j;
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    j["time"] = {{"utc", isoFromJ2000(t)},
                 {"j2000_s", t},
                 {"paused", timePaused},
                 {"scale", kTimeLabels[std::clamp(timeScaleIdx, 0, kNumTimeScales - 1)]},
                 {"reverse", timeDir < 0.0f}};
    // obsHeightOffset is an altitude above sea level floored at the ground (see `observer`), so the
    // eye is at max(ground, offset) (+2 m). The ground is the GPU's (DEM + detail, last completed
    // frame) unless the depth pass is knocked out; then the CPU's coarse DEM copy.
    const bool gpuGround = harnessGpuGroundValid();
    const float ground = gpuGround ? terrainFrameMapped[1] : obsTerrainH;
    const float eyeAsl = std::max(ground, obsHeightOffset);
    j["observer"] = {{"lat_deg", obsLatDeg},
                     {"lon_deg", obsLonDeg},
                     {"alt_m", eyeAsl},
                     {"agl_m", eyeAsl - ground},
                     {"ground_m", ground},
                     {"ground_source", gpuGround ? "gpu" : "cpu"},
                     {"terrain_cpu_m", obsTerrainH},
                     {"height_offset_m", obsHeightOffset},
                     {"following", followActive}, {"follow_flight", followFlight},
                     {"follow_last", std::string(followLastLabel)}};
    if (followActive)
    {
        j["observer"]["follow"] = {{"sat", followSatIndex},
                                   {"offset_m", {followOffset.x, followOffset.y, followOffset.z}},
                                   {"label", std::string(followLabel)},
                                   {"handover_m", glm::length(followHandD)},
                                   {"camera_asl_m", glm::length(followObsEcef) - satphot::kEarthRadiusM},
                                   {"dist_to_target_m", glm::length(followObsEcef - followLastP)},
                                   {"flight_rel", followFlightRel}};
        if (followIsTransient())
            j["observer"]["follow"]["transient"] = {{"kind", (int)followTrCur.kind}, {"stage", (int)followTrCur.stage},
                                                    {"key", followTrKey}, {"waiting_for_kind", (int)followWantKind},
                                                    {"alt_km", (glm::length(followTrCur.pos) - satphot::kEarthRadiusM) / 1000.0},
                                                    {"tau_s", followTrCur.tau}};
    }
    j["camera"] = {{"az_deg", camera.azDeg}, {"el_deg", camera.elDeg}, {"fov_y_deg", camera.fovYDeg},
                   {"tracking", trackActive}};
    const glm::vec3 sun(sunDirENU), moon(moonDirENU);
    j["sun"] = {{"az_deg", azDegOf(sun)}, {"el_deg", elDegOf(sun)}};
    // The global exposure (SatelliteSim.h): the user's EV, the metered auto offset and the meter's
    // last reading of the displayed frame (linear mean, fraction clipped white).
    j["exposure"] = {{"ev", cv2ExposureEV}, {"auto_ev", autoExposureEV}, {"global_ev", globalExposureEV()},
                     {"sat_adapt_mag", satAdaptMag}, {"sat_adapt_lum", satAdaptLum}, {"sat_adapt_frame_mag", satAdaptFrameMag}, {"sat_gain_mag", satDisplayGainMag()},
                     {"meter_mean", meterMeanLum}, {"meter_clip", meterClipFrac}};
    j["moon"] = {{"az_deg", azDegOf(moon)}, {"el_deg", elDegOf(moon)}, {"illum", moonIllumFrac}, {"brightness_vs_full", moonDirENU.w},
                 {"dist_km", moonDistM * 1e-3},
                 {"sep_from_sun_deg", glm::degrees(std::acos(std::clamp(glm::dot(glm::normalize(moon), glm::normalize(sun)), -1.0f, 1.0f)))},
                 {"drawn_radius_deg", glm::degrees(moonAngR)}, {"sun_radius_deg", glm::degrees(sunAngRTrue)},
                 {"sun_seen_frac", moonEclipseSolarObs}, {"sky_light_frac", moonEclipseSkyObs}, {"sky_exposure", skyExposure()}, {"solar_possible", moonEclipseSolarPossible},
                 {"lunar_possible", moonEclipseLunarPossible}};

    j["aurora"] = auroraStateJson();
    {
        // The light-pollution dome, 16 sectors of 22.5 deg clockwise from north (sector i centred on
        // (i + 0.5) x 22.5): `gained` the linear dome satellites and stars dim by, `dark_sky` the eased
        // 0..1 pristine -> city value the Milky Way, zodiacal light and aurora gates read (darksky.glsl).
        json g = json::array(), e = json::array();
        for (int i = 0; i < kNumLightSectors; ++i) { g.push_back(lightDomeAz[i]); e.push_back(lightDomeEasedAz[i]); }
        j["light_dome"] = {{"gained", g}, {"dark_sky", e}};
    }

    json ko = json::array();
    for (int i = 0; i < debugToggleTableSize(); ++i)
    {
        uint32_t bit;
        const char *label, *key;
        if (debugToggleAt(i, bit, label, key) && (debugDisableMask & bit))
            ko.push_back(key);
    }
    if (debugDisableMask & 262144u)
        ko.push_back("potato_sky");
    if (debugDisableMask & 524288u)
        ko.push_back("lite_sky");
    // The compact visible list's length (last frame's header): satellites + stars' share + city light
    // sprites (city_sprites.comp appends after sat_flare.comp).
    j["kessler"] = kesslerStateJson();
    j["launches"] = launchStateJson();
    j["satellites"] = {{"visible_count", visibleCount}, {"active_count", activeSatCount},
                       // the mesh hand-off: the fade-in size (raised past the kMaxMeshInstances largest) and how many
                       // satellites were mesh-sized candidates last frame
                       {"mesh_fade_px", meshFadePx}, {"mesh_candidates", meshCandTotal},
                       {"peak_magnitude", peakMagnitude}};
    if (glowMapped)
    {
        // The sky glow: the strongest bin's summed flux (displayed effectFlare; the glow starts at 300).
        const GpuGlowBuf *gb = static_cast<const GpuGlowBuf *>(glowMapped);
        uint32_t mx = 0, lit = 0;
        for (int i = 0; i < kGlowBins; ++i)
        {
            mx = std::max(mx, gb->bins[i]);
            lit += gb->bins[i] > 300u;
        }
        j["satellites"]["sky_glow_max_bin_flux"] = mx;
        j["satellites"]["sky_glow_bins_lit"] = lit;
    }
    j["render"] = {{"preset", kGraphicsPresetNames[(int)graphicsPreset]},
                   {"knockout_mask", debugDisableMask},
                   {"ui_theme", kUiThemeKeys[(int)Pal::current]},
                   {"ui_night_filter", UIRenderer::colorFilter()},
                   {"knockouts", ko},
                   {"render_scale", renderScale},
                   {"dynamic_resolution", dynResEnabled},
                   {"sky_taa_used", skyTaaUsedThisFrame},
                   {"full_sky_deferred", fullSkyDeferred},
                   {"full_sky_pipeline", skyBgPipeline != VK_NULL_HANDLE},
                   {"boot_choice", bootChoiceMode},
                   {"ask_graphics_mode", askGraphicsModeOnStartup},
                   {"width", ctx_ ? ctx_->swapExtent.width : 0},
                   {"height", ctx_ ? ctx_->swapExtent.height : 0},
                   {"ui_visible", uiVisible},
                   {"ui_scale", uiScale}};

    json sel = json::object();
    if (selectedSatIndex >= 0 && selectedSatIndex < (int)satOrbits.size())
    {
        const SatOrbit &o = satOrbits[selectedSatIndex];
        sel["sat"] = selectedSatIndex;
        sel["type"] = o.typeIdx < satTypes.size() ? satTypes[o.typeIdx].name : std::string("transient");
        // The selection panel's two text lines as drawn (formatSelectedSatInfo), and the info window's subject.
        sel["panel_lines"] = {std::string(selInfoLine[0]), std::string(selInfoLine[1])};
        sel["viewer_sat"] = viewerSatIndex;
        if (selIsTransient())
            sel["transient"] = {{"kind", (int)selTransientCur.kind}, {"label", transientLabel()},
                                {"alt_km", (glm::length(selTransientCur.pos) - satphot::kEarthRadiusM) / 1000.0},
                                {"speed_kms", glm::length(selTransientCur.vel) / 1000.0}};
        if (o.constIdx < constellations.size())
        {
            sel["constellation"] = constellations[o.constIdx].name;
            sel["n"] = selectedSatIndex - (int)constellations[o.constIdx].orbitStart;
        }
        if (const ConstMember *m = rosterMember((uint32_t)selectedSatIndex)) // a List member (the GEO belt)
        {
            sel["member"] = {{"name", m->name}, {"operator", m->operatorName}, {"class", m->cls}, {"purpose", m->purpose}};
            if (m->geo)
            {
                // Its sub-satellite longitude now (the slot it is drawn at), from the CPU evaluator.
                const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
                const glm::dvec3 p = satOrbitStateAt(orbitElemsOf(o), t).posEci;
                double lon = glm::degrees(std::atan2(p.y, p.x) - earthRotationAngle(t));
                lon = std::fmod(lon + 540.0, 360.0) - 180.0;
                sel["member"]["slot_lon_deg"] = m->lonDeg;
                sel["member"]["lon_now_deg"] = lon;
                sel["member"]["lat_now_deg"] = glm::degrees(std::asin(p.z / glm::length(p)));
            }
        }
        updateSelectedSkyDir();
        sel["az_deg"] = azDegOf(selSkyDirCpu);
        sel["el_deg"] = elDegOf(selSkyDirCpu);
        sel["above_earth"] = selAboveEarth;
        sel["track"] = trackActive; // the camera lock below `camera.tracking`, on the satellite it follows
        if (o.formation) // formation ring member: where it is relative to its cluster centre, now
        {
            const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
            SatOrbitElems ce = orbitElemsOf(o);
            ce.formation = false;
            const SatOrbitState m = satOrbitStateAt(orbitElemsOf(o), t), c = satOrbitStateAt(ce, t);
            const glm::dvec3 d = m.posEci - c.posEci, rH = -c.nadir, vH = c.velocity;
            const glm::dvec3 hH = glm::normalize(glm::cross(rH, vH));
            sel["formation"] = {{"offset_m", {glm::dot(d, vH), glm::dot(d, hH), glm::dot(d, rH)}}, // along, cross, radial
                                {"dist_m", glm::length(d)}};
        }
        // The CPU photometry NOW from the camera (the eye, in follow mode too): magnitude, phase, and each lit lobe's
        // share — which surface makes the flare.
        if (o.typeIdx < satTypes.size() && satTypes[o.typeIdx].isModel())
        {
            const SatelliteType &type = satTypes[o.typeIdx];
            const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
            const double th = earthRotationAngle(t), ct = std::cos(th), st = std::sin(th);
            const glm::dvec3 obsEcef = followActive ? followObsEcef : glm::normalize(glm::dvec3(obsDir)) * (double)obsEyeRadiusM();
            SatPhotInputs in;
            in.sunDirEci = glm::dvec3(sunDirECI);
            in.obsEci = glm::dvec3(ct * obsEcef.x - st * obsEcef.y, st * obsEcef.x + ct * obsEcef.y, obsEcef.z);
            in.flareTiltRad = glm::radians((double)flareMitigationTiltDeg);
            const SatOrbitElems e = orbitElemsOf(o);
            if (attUsesGroundSite(type.groups))
            {
                in.hasSiteIdeal = true;
                in.siteIdeal = satGroundSiteIdeal(groundSiteAim(), e, (uint32_t)selectedSatIndex, t, in.sunDirEci).ideal;
            }
            const SatPhotResult pr = evalSatPhotometry(type.groups, type.lobes, e, t, in);
            json p = {{"mag", std::isfinite(pr.magnitude) ? pr.magnitude : 99.0},
                      {"range_m", pr.rangeM},
                      {"phase_deg", glm::degrees(pr.phaseAngleRad)},
                      {"lit", pr.litFactor},
                      {"i_sun", pr.intensitySun},
                      {"i_earth", pr.intensityEarth},
                      {"dominant_lobe", pr.dominantLobe},
                      {"flare_units", pr.flareUnits * brightnessScale},
                      {"unfold", satUnfoldAt(e, t)},
                      {"rest_joints", e.restJoints}};
            AttGeometry geo;
            geo.nadir = pr.orbit.nadir;
            geo.velocity = pr.orbit.velocity;
            geo.sun = in.sunDirEci;
            geo.siteIdeal = in.hasSiteIdeal ? in.siteIdeal : geo.nadir;
            geo.tumbleAngle = pr.orbit.tumbleAngle;
            geo.tumbleAxis = e.tumbleAxis;
            geo.forceTumble = e.forceTumble;
        geo.jointsRest = e.restJoints;
            geo.flareTiltRad = in.flareTiltRad;
            geo.fixW = satAttOverride(e, t, geo.fixR);
            const std::vector<GroupPose> poses = evalGroupPoses(type.groups, geo, false);
            const glm::dvec3 s = in.sunDirEci, ob = glm::normalize(in.obsEci - pr.orbit.posEci);
            const double sh = glm::dot(s, glm::normalize(s + ob));
            p["s_dot_h"] = sh;
            json lobes = json::array();
            for (int li = 0; li < (int)type.lobes.size(); ++li)
            {
                const GpuSatLobe &L = type.lobes[li];
                const glm::dvec3 n = satLobeNormalPosed(type.lobes, type.groups, poses, li);
                const double a2 = L.alpha2Mat + (double)kSunAlpha * kSunAlpha;
                const double I = satLobeIntensity(n, L.area, L.diffArea, L.albedoD, L.f0, a2, L.distribution != 0, s, ob, L.transmission);
                const double ns = glm::dot(n, s), no = glm::dot(n, ob);
                const double Id = no <= 0.0 ? 0.0
                                  : ns > 0.0 ? L.albedoD / glm::pi<double>() * L.diffArea * ns * no
                                             : L.transmission / glm::pi<double>() * L.diffArea * (-ns) * no;
                if (I <= 0.0)
                    continue;
                lobes.push_back({{"i", li}, {"I", I}, {"I_diffuse", Id}, {"area", L.area}, {"f0", L.f0},
                                 {"alpha", std::sqrt(L.alpha2Mat)}, {"group", L.group},
                                 {"n_s", glm::dot(n, s)}, {"n_o", glm::dot(n, ob)}});
            }
            p["lobes"] = lobes;
            if (o.formation)
            {
                int fn = 0;
                const double fm = formationMagnitude(selectedSatIndex, in, t, fn);
                p["formation"] = {{"mag", std::isfinite(fm) ? fm : 99.0}, {"members", fn},
                                  {"merge_factor", formationMergeFactor(o, selectedSatIndex, pr.rangeM)}};
            }
            sel["photometry"] = p;
        }
        if (selParityValid) // the GPU-vs-CPU check (updateSelectedPhotometry): magnitude gap and both ranges
            sel["parity"] = {{"dmag", selParityDmag},
                             {"range_gpu_m", selParityRangeGpuM},
                             {"range_cpu_m", selParityRangeCpuM}};
        // Its screen position (px, top-left origin) and, when its mesh is drawn, the hand-off: the mesh fade, the
        // sprite's keep (1 - spriteGone), the glare keep, its size in px, and where the sprite (point, bloom,
        // point-like glare) is placed — the Sun's image in its dominant lobe, weighted by the lobe's specular share.
        if (ctx_)
        {
            const float W = (float)ctx_->swapExtent.width, H = (float)ctx_->swapExtent.height;
            float sx = 0.0f, sy = 0.0f;
            if (projectSkyDirToScreen(selSkyDirCpu, W, H, sx, sy))
                sel["screen_px"] = {sx, sy};
            if (selMeshDbg.drawn)
            {
                json m = {{"fade", selMeshDbg.fade}, {"keep", selMeshDbg.keep}, {"glare_keep", selMeshDbg.glareKeep},
                          {"px", selMeshDbg.px}, {"glint_w", selMeshDbg.glintW},
                          {"glint_share", selMeshDbg.glintShare}};
                if (projectSkyDirToScreen(selMeshDbg.centreEnu, W, H, sx, sy))
                    m["centre_px"] = {sx, sy};
                if (projectSkyDirToScreen(selMeshDbg.spriteEnu, W, H, sx, sy))
                    m["sprite_px"] = {sx, sy};
                sel["mesh"] = m;
            }
        }
        // The main view's mesh glints (glare_find.comp's list, last frame's copy): where each mesh glare is drawn,
        // in px, with its light (effectFlare). The release cinematic's mirror shot checks the glare against
        // sprite_px (the Sun's image) above (tools/harness/glare_track.py).
        if (ctx_ && glintReadValid && glintReadMapped)
        {
            const float W = (float)ctx_->swapExtent.width, H = (float)ctx_->swapExtent.height;
            const GpuGlintList &gl = *static_cast<const GpuGlintList *>(glintReadMapped);
            json arr = json::array();
            for (uint32_t i = 0; i < std::min(gl.count, kMaxGlints); ++i)
                arr.push_back({{"px", {gl.pos[i].x * W, gl.pos[i].y * H}}, {"flare", gl.pos[i].z}});
            sel["glints"] = arr;
        }
    }
    if (selectedPlanetIndex >= 0)
        sel["planet"] = kPlanetNames[selectedPlanetIndex];
    j["selection"] = sel;
    // The v2 march's rate this frame: sparse (1 pixel in 4), full, half (checkerboard) or adaptive.
    j["clouds_v2"] = {{"rate", cv2AdaptiveNow ? "adaptive" : cv2HalfRateNow ? "half" : cv2FullRateNow ? "full" : "sparse"},
                      {"still_frames", cv2StillFrames}, {"history_valid", cv2HistoryValid},
                      {"sun_cloud_t", sunCloudTEased}, {"fast_lod", cv2FastLodNow},
                      {"shape_frame", cv2ShapeFrame}};   // review 22: the Sun disc's cloud transmittance
    // Rain at the eye (2026-10-04, the particles): what was drawn last frame, and the light and rain map the lightning
    // pass wrote (the previous frame's).
    {
        json rn = {{"mode", rainModeLastFrame == 1 ? "rain" : rainModeLastFrame == 2 ? "diamond_dust" : "none"},
                   {"instances", rainInstancesLastFrame}, {"levels", rainLevelsLastFrame},
                   // the temperature model (SatelliteSimCloudsV2.cpp): at the eye (the particles) and 500 m over the
                   // ground under it (the shafts seen at a distance); the beams' light at the eye (Suns)
                   {"eye_temp_c", cv2EyeTempC}, {"shaft_temp_c", cv2ShaftTempC},
                   {"beam_eye_irr_suns", cv2BeamEyeIrr},
                   {"beam_eye_dir_enu", {cv2BeamEyeDir.x, cv2BeamEyeDir.y, cv2BeamEyeDir.z}}};
        if (cv2FlashMapped)
        {
            const char *base = (const char *)cv2FlashMapped;
            const float *wg = (const float *)(base + kCv2RainMapOffset - 16 - 80);
            const float *map = (const float *)(base + kCv2RainMapOffset);
            const float *lt = (const float *)(base + kCv2RainLightOffset);
            const int n = 32, c = n / 2;
            rn["rate_eye"] = 0.25f * (map[(c - 1) * n + c - 1] + map[(c - 1) * n + c] + map[c * n + c - 1] + map[c * n + c]);
            rn["rate_near_max"] = std::max(std::max(wg[0], wg[1]), std::max(wg[2], wg[3]));
            int best = 0;
            for (int i = 1; i < n * n; ++i)
                if (map[i] > map[best]) best = i;
            rn["max_at_en_m"] = {((best % n) - c + 0.5f) * 40.0f, ((best / n) - c + 0.5f) * 40.0f};   // kCv2RainCellM
            rn["key"] = {lt[0], lt[1], lt[2]};
            rn["key_is_moon"] = lt[3] > 0.5f;
            rn["key_dir_enu"] = {lt[4], lt[5], lt[6]};
            rn["liquid_share"] = lt[7];
            rn["ambient"] = {lt[8], lt[9], lt[10]};
            rn["tau_up"] = lt[11];
            rn["wind_mps"] = {rainWindNow.x, rainWindNow.y};
            rn["fall_speed_ref"] = rainFallRefNow;
            rn["rate_eased"] = rainRateEased;
        }
        j["clouds_v2"]["rain"] = rn;
    }
    // The layer shadow map (cv2_layer_map.glsl), probed by cloud_v2_layers.comp at 1.5 km above sea level under the eye
    // (the previous frame's): the lookup there, the columns over it, and the slant cells where its Sun ray crosses each
    // layer's reference sphere (T, the mid layer's slant optical depth with its shadow share).
    if (cv2FlashMapped)
    {
        const float *pr = (const float *)((const char *)cv2FlashMapped + kCv2RainMapOffset - 80) + 8;
        j["clouds_v2"]["layer_map"] = {{"frames", cv2LayFrames}, {"epoch", cv2LayEpoch}, {"T_below_eye", pr[0]},
                                       {"mid_tau", pr[1]}, {"mid_tau_var", pr[2]}, {"mid_band_km", {pr[3], pr[4]}},
                                       {"high_tau", pr[5]}, {"high_band_km", {pr[6], pr[7]}},
                                       {"mid_slant_T", pr[8]}, {"mid_slant_tau", pr[9]}, {"high_slant_T", pr[10]},
                                       {"sun_sin_at_X", pr[11]}};
    }

    static const char *kBucketKeys[8] = {"scene_depth", "beam_cloud_block", "orbit_compute", "cloud_march",
                                         "flare_compute", "sky_background_draw", "satellite_star_draw", "ui_overlay"};
    json g;
    for (int b = 0; b < 8; ++b)
        g[kBucketKeys[b]] = gpuMsRaw[b];
    g["total"] = gpuMsRawTotal;
    j["gpu_ms_last_frame"] = g;
    j["gpu_ms_smoothed_total"] = gpuMsTotalSmoothed;

    std::string gpu;
    if (ctx_)
    {
        VkPhysicalDeviceProperties props{};
        vkGetPhysicalDeviceProperties(ctx_->physicalDevice, &props);
        gpu = props.deviceName;
    }
#ifdef NDEBUG
    const char *cfg = "Release";
#else
    const char *cfg = "Debug";
#endif
    if (ambience_ && ambience_->loaded())
    {
        j["ambience"] = ambience_->stateJson();
        j["ambience"]["tonality"] = tonalityJson();
        if (audio_)
        {
            j["ambience"]["volumes"] = {{"master", audio_->getMasterVolume()},
                                        {"music", audio_->getMusicVolume()},
                                        {"sfx", audio_->getSfxVolume()},
                                        {"ambience", audio_->getAmbienceVolume()}};
            j["ambience"]["music"] = {{"track", audio_->trackIndex()},
                                      {"name", audio_->trackName(audio_->trackIndex())},
                                      {"tracks", audio_->trackCount()},
                                      {"paused", audio_->musicPaused()},
                                      {"gap_remaining_s", audio_->gapRemaining()},
                                      {"gap_s", audio_->musicGap()}};
        }
    }
    j["build"] = {{"version", APP_VERSION}, {"commit", APP_GIT_COMMIT}, {"config", cfg}, {"gpu", gpu}};
    j["harness_frame"] = harnessRunner_ ? harnessRunner_->frame() : 0;
    return j;
}

// ─── A snapshot record's view ─────────────────────────────────────────────────────────────────────
std::string SatelliteSim::harnessApplyViewRecord(const json &rec, bool applySettings, bool raw, bool driftDefault)
{
    if (applySettings && rec.contains("settings"))
    {
        // raw: as saved, past applySettingsJson's range guards (a value typed in dev mode: the reporter's
        // machine ran with it, so the replay does too).
        struct RawGuard
        {
            bool &f;
            ~RawGuard() { f = false; }
        } rawGuard{settingsRawImport_};
        settingsRawImport_ = raw;
        applySettingsJson(rec["settings"], true);
        // A patch that names display.play_intro_on_startup re-arms the intro, which then flies the
        // camera off (and resets the drift offset): a harness run never plays it.
        showIntro = false;
    }

    if (followActive)
        stopFollow();
    const json &st = rec.at("sim_time");
    simDayJ2000 = st.at("day_j2000").get<int64_t>();
    simSecInDay = st.at("sec_in_day").get<double>();
    timePaused = true;
    trailClearPending = true;

    const bool hasView = rec.contains("view");
    const json &ob = rec.at("observer");
    if (hasView)
    {
        const json &d = rec["view"]["obs_dir"];
        obsDir = glm::normalize(glm::vec3(d[0].get<float>(), d[1].get<float>(), d[2].get<float>()));
    }
    else
    {
        const float la = glm::radians(ob.at("lat_deg").get<float>());
        const float lo = glm::radians(ob.at("lon_deg").get<float>());
        obsDir = {cosf(la) * cosf(lo), cosf(la) * sinf(lo), sinf(la)};
    }
    obsLatDeg = glm::degrees(asinf(glm::clamp(obsDir.z, -1.0f, 1.0f)));
    obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
    obsHeightOffset = ob.at("height_offset_m").get<float>();
    const json &cam = rec.at("camera");
    camera.fovYDeg = cam.at("fov_y_deg").get<float>();
    aimCameraAzEl(cam.at("az_deg").get<float>(), cam.at("el_deg").get<float>());

    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    double phase;
    std::string driftSrc;
    if (hasView && rec["view"].contains("cloud_drift_phase"))
    {
        cloudDriftRate = rec["view"].value("cloud_drift_rate", cloudDriftRate);
        phase = rec["view"]["cloud_drift_phase"].get<double>();
        driftSrc = "recorded";
    }
    else if (driftDefault)
    {
        phase = (double)cloudDriftRate * (t - kCloudDriftEpochS) + kCloudDriftPhaseDefault;
        driftSrc = "default offset (reconstructed)";
    }
    else
    {
        // The intro's offset, as if it ran at the snapshot's own time (see SatelliteSim.cpp's intro).
        phase = (double)kIntroCloudDriftRate * t;
        driftSrc = "intro offset (reconstructed)";
    }
    cloudDriftPhaseOffset = std::fmod(phase - (double)cloudDriftRate * (t - kCloudDriftEpochS),
                                      glm::two_pi<double>());

    obsTerrainH = cpuTerrainHeightM(obsLatDeg, obsLonDeg);
    updatePositions(t, 0.0f);
    return driftSrc;
}

// ─── Commands ─────────────────────────────────────────────────────────────────────────────────────
Status SatelliteSim::harnessExec(harness::Active &a)
{
    // Time/observer/follow commands change what this frame's recordCompute will derive (Sun, Moon,
    // the ENU frame, terrain height). Derive it now as well, so a `state`, `camera look sun` or
    // capture sidecar in the same frame reports the new values rather than last frame's.
    // updatePositions is O(1) and has no per-call state, so a second call per frame is harmless.
    struct Refresh
    {
        SatelliteSim *s;
        const std::string &name;
        ~Refresh()
        {
            if (name == "time" || name == "observer" || name == "follow" || name == "beams")
            {
                s->obsTerrainH = s->cpuTerrainHeightM(s->obsLatDeg, s->obsLonDeg);
                s->updatePositions((double)s->simDayJ2000 * 86400.0 + s->simSecInDay, 0.0f);
            }
        }
    } refresh{this, a.cmd.name};
    const harness::Command &c = a.cmd;
    const std::string &n = c.name;
    json &r = a.result;
    auto pos = [&](size_t i) -> std::string { return i < c.pos.size() ? c.pos[i] : std::string(); };

    // ── flow ────────────────────────────────────────────────────────────────────
    if (n == "help")
    {
        r["message"] = kHelp;
        return Status::Done;
    }
    if (n == "log" || n == "echo")
    {
        std::string msg;
        for (size_t i = 0; i < c.pos.size(); ++i)
            msg += (i ? " " : "") + c.pos[i];
        Log::line("harness: " + msg);
        r["message"] = msg;
        return Status::Done;
    }
    if (n == "quit" || n == "exit")
        return Status::Done; // the Runner sees the name and stops after recording it
    if (n == "wait" || n == "settle")
    {
        const std::string mode = n == "settle" ? "settle" : lower(pos(0));
        if (mode == "settle")
        {
            const std::string arg = n == "settle" ? pos(0) : pos(1);
            if (a.frame == 0)
            {
                const int frames = arg.empty() ? 40 : (int)parseNum(arg, "wait settle");
                harnessSettleSavedPaused_ = timePaused;
                timePaused = true;
                // The sky TAA restarts with the settle (history and jitter phase): a settled capture then
                // depends on the settle alone, not on how many frames ran before it (boot length varies).
                skyTaaHistValid = false;
                skyTaaFrame = 0;
                harnessSettleFrames_ = std::max(1, frames);
                a.scratch["frames"] = harnessSettleFrames_;
                return Status::Pending;
            }
            if (--harnessSettleFrames_ > 0)
                return Status::Pending;
            harnessSettleFrames_ = 0;
            timePaused = harnessSettleSavedPaused_;
            r["message"] = "settled " + std::to_string(a.scratch["frames"].get<int>()) + " frames";
            return Status::Done;
        }
        if (mode == "seconds" || mode == "s")
        {
            if (a.frame == 0)
                a.scratch["until"] = harness::nowS() + parseNum(pos(1), "wait seconds");
            return harness::nowS() >= a.scratch["until"].get<double>() ? Status::Done : Status::Pending;
        }
        const std::string arg = (mode == "frames") ? pos(1) : pos(0);
        const int frames = arg.empty() ? 1 : (int)parseNum(arg, "wait");
        return a.frame >= frames ? Status::Done : Status::Pending;
    }

    // ── time ────────────────────────────────────────────────────────────────────
    if (n == "time")
    {
        const std::string sub = lower(pos(0));
        auto setAbs = [&](double t)
        {
            const double days = std::floor(t / 86400.0);
            simDayJ2000 = (int64_t)days;
            simSecInDay = t - days * 86400.0;
            trailClearPending = true;
        };
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        if (sub == "set")
            setAbs(j2000FromIso(pos(1)));
        else if (sub == "j2000")
            setAbs(parseNum(pos(1), "time j2000"));
        else if (sub == "add")
            setAbs(now + parseNum(pos(1), "time add"));
        else if (sub == "sun")
        {
            // A UTC hour gives a local time of day only through the observer's longitude and the
            // equation of time. This finds the moment directly: the time nearest the current one (within +-12 h) at which
            // the Sun stands at the requested elevation for THIS observer, from the same Sun and
            // observer formulas updatePositions uses.
            const glm::dvec3 od = glm::dvec3(obsDir);
            const double rObs = (double)obsEyeRadiusM();
            auto sunEl = [&](double tt)
            {
                const glm::dvec3 up = glm::normalize(observerEciAt(od, rObs, tt));
                return glm::degrees(std::asin(glm::clamp(glm::dot(sunDirEciAt(tt), up), -1.0, 1.0)));
            };
            const std::string what = lower(pos(1));
            const std::string dir = lower(pos(2));
            const double step = 60.0;
            double best = now, bestCost = 1e30;
            if (what == "noon" || what == "midnight")
            {
                for (double dt = -43200.0; dt <= 43200.0; dt += step)
                {
                    const double e = sunEl(now + dt);
                    const double cost = what == "noon" ? -e : e;
                    if (cost < bestCost)
                        bestCost = cost, best = now + dt;
                }
            }
            else
            {
                const double target = parseNum(what, "time sun <elevation deg>");
                if (!dir.empty() && dir != "rising" && dir != "setting")
                    fail("time sun: rising | setting");
                bool found = false;
                for (double dt = -43200.0; dt < 43200.0; dt += step)
                {
                    const double e0 = sunEl(now + dt) - target, e1 = sunEl(now + dt + step) - target;
                    if ((e0 <= 0.0) == (e1 <= 0.0))
                        continue;
                    const bool rising = e1 > e0;
                    if ((dir == "rising" && !rising) || (dir == "setting" && rising))
                        continue;
                    double lo = now + dt, hi = lo + step; // bisect to a second
                    for (int k = 0; k < 20; ++k)
                    {
                        const double mid = 0.5 * (lo + hi);
                        if (((sunEl(mid) - target) <= 0.0) == (e0 <= 0.0))
                            lo = mid;
                        else
                            hi = mid;
                    }
                    const double cand = 0.5 * (lo + hi);
                    if (std::fabs(cand - now) < bestCost)
                        bestCost = std::fabs(cand - now), best = cand, found = true;
                }
                if (!found)
                    fail("time sun: the Sun never reaches " + what + " deg here within 12 h (polar day/night?)");
            }
            setAbs(best);
            r["sun_el_deg"] = sunEl(best);
        }
        else if (sub == "pause")
            timePaused = true;
        else if (sub == "play")
            timePaused = false;
        else if (sub == "scale")
        {
            int idx = -1;
            for (int i = 0; i < kNumTimeScales; ++i)
                if (ieq(pos(1), kTimeLabels[i]))
                    idx = i;
            if (idx < 0)
            {
                std::string opts;
                for (int i = 0; i < kNumTimeScales; ++i)
                    opts += std::string(i ? ", " : "") + kTimeLabels[i];
                fail("time scale: one of " + opts);
            }
            timeScaleIdx = idx;
        }
        else if (sub == "reverse")
            timeDir = c.pos.size() > 1 && (pos(1) == "off" || pos(1) == "0") ? 1.0f : -1.0f;
        else if (!sub.empty())
            fail("time: unknown subcommand '" + sub + "' (set, add, j2000, sun, pause, play, scale, reverse)");
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        r["utc"] = isoFromJ2000(t);
        r["j2000_s"] = t;
        r["paused"] = timePaused;
        r["message"] = isoFromJ2000(t) + (timePaused ? " (paused)" : "");
        return Status::Done;
    }

    // ── observer ────────────────────────────────────────────────────────────────
    if (n == "observer")
    {
        // agl > 0 finishes a frame later: the GPU's ground at the new position (DEM + detail, what
        // the renderer stands on) replaces the CPU's coarse estimate.
        if (a.frame > 0)
        {
            if (a.frame < 2)
                return Status::Pending;
            const float agl = a.scratch["agl"].get<float>();
            const float ground = terrainFrameMapped[1];
            obsHeightOffset = ground + agl;
            r["lat_deg"] = obsLatDeg;
            r["lon_deg"] = obsLonDeg;
            r["alt_m"] = obsHeightOffset;
            r["agl_m"] = agl;
            r["ground_m"] = ground;
            char buf[128];
            snprintf(buf, sizeof(buf), "%.4f, %.4f  alt %.0f m, agl %.0f m  (ground %.0f m)", obsLatDeg, obsLonDeg,
                     obsHeightOffset, agl, ground);
            r["message"] = buf;
            return Status::Done;
        }
        if (c.has("lat") || c.has("lon"))
        {
            if (followActive)
                stopFollow();
            const float lat = (float)c.num("lat", obsLatDeg), lon = (float)c.num("lon", obsLonDeg);
            if (lat < -90.0f || lat > 90.0f)
                fail("observer: lat must be in [-90, 90]");
            const float la = glm::radians(lat), lo = glm::radians(lon);
            obsDir = {cosf(la) * cosf(lo), cosf(la) * sinf(lo), sinf(la)};
            obsLatDeg = lat;
            obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
            aimCameraAzEl(camera.azDeg, camera.elDeg); // keep the view direction in the new local frame
            trailClearPending = true;
        }
        // obsHeightOffset is read by the sky shaders as an altitude ABOVE SEA LEVEL floored at the
        // ground (eye = max(ground, offset) + 2 m; terrain.glsl observerEffHeight). `alt` sets it
        // directly. agl=0 is 0 (on the ground, exactly). agl > 0 needs the ground height: the CPU's
        // (an 18 km/px copy of the DEM, off by hundreds of metres on a coast) for one frame, then
        // the GPU's, read back above. With the depth pass knocked out only the CPU's exists.
        if (c.has("alt"))
            obsHeightOffset = std::max(0.0f, (float)c.num("alt", 0.0));
        if (c.has("agl"))
        {
            const float agl = (float)c.num("agl", 0.0);
            obsHeightOffset = agl <= 0.0f ? 0.0f : cpuTerrainHeightM(obsLatDeg, obsLonDeg) + agl;
            if (agl > 0.0f && harnessGpuGroundValid())
            {
                a.scratch["agl"] = agl;
                return Status::Pending;
            }
        }
        obsTerrainH = cpuTerrainHeightM(obsLatDeg, obsLonDeg);
        r["lat_deg"] = obsLatDeg;
        r["lon_deg"] = obsLonDeg;
        const float eyeAsl = std::max(obsTerrainH, obsHeightOffset); // CPU ground: approximate
        r["alt_m"] = eyeAsl;
        r["agl_m"] = eyeAsl - obsTerrainH;
        r["terrain_m"] = obsTerrainH;
        char buf[128];
        snprintf(buf, sizeof(buf), "%.4f, %.4f  alt %.0f m, agl %.0f m  (terrain %.0f m)", obsLatDeg, obsLonDeg,
                 eyeAsl, eyeAsl - obsTerrainH, obsTerrainH);
        r["message"] = buf;
        return Status::Done;
    }

    // ── drift ───────────────────────────────────────────────────────────────────
    // `drift [default] [phase=<rad>] [rate=<rad/s>]`: the cloud map's drift (session state, what a bookmark or snapshot
    // stores). phase= sets the offset that gives that phase at the CURRENT sim time (set the time first); bare,
    // it reports the phase and rate. tools/harness/scripts/default_bookmarks.satcmd frames its storms with it.
    if (n == "drift")
    {
        if (c.has("rate"))
            cloudDriftRate = (float)c.num("rate", (double)cloudDriftRate);
        if (lower(pos(0)) == "default")
            cloudDriftPhaseOffset = kCloudDriftPhaseDefault; // the compiled-in offset (a fresh session's map)
        else if (c.has("phase"))
        {
            const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
            cloudDriftPhaseOffset = std::fmod(c.num("phase", 0.0) - (double)cloudDriftRate * (t - kCloudDriftEpochS),
                                              glm::two_pi<double>());
        }
        r["cloud_drift_phase"] = cloudDriftPhase();
        r["cloud_drift_rate"] = cloudDriftRate;
        char buf[96];
        snprintf(buf, sizeof(buf), "cloud drift phase %.6f rad, rate %.3g rad/s", cloudDriftPhase(), (double)cloudDriftRate);
        r["message"] = buf;
        return Status::Done;
    }

    // ── snapshot ────────────────────────────────────────────────────────────────
    // `snapshot <profile_log.jsonl> [index=-1] [settings=on|off] [clamp=on] [drift=intro|default]`: reproduce the
    // view of a perf snapshot (Settings -> Display -> Save Snapshot) — the user's way to hand a location
    // over with parity. Applies the record's settings (unless settings=off), sim time (paused), the
    // exact observer direction and height, the camera, and the cloud map's drift PHASE, which is session
    // state (the intro sets it) and was why lat/lon + time alone framed different clouds. index counts
    // from the end when negative (-1 = the last record). Records written before 2026-09-28 carry no
    // settings or phase: pass the user's settings.json to run.py, and the phase is reconstructed as a
    // session that played the intro (drift=intro, the default: its offset depends on when the intro ran
    // only at 5e-8 rad/s, ~300 m per 10 minutes) or the compiled default (drift=default).
    if (n == "snapshot")
    {
        std::ifstream in(pos(0));
        if (pos(0).empty() || !in)
            fail("snapshot: cannot open '" + pos(0) + "' (a perf_profiles/profile_log.jsonl)");
        std::vector<json> recs;
        for (std::string line; std::getline(in, line);)
        {
            if (line.find_first_not_of(" \t\r") == std::string::npos)
                continue;
            json rec = json::parse(line, nullptr, false);
            if (!rec.is_discarded() && rec.value("record_kind", std::string("snapshot")) == "snapshot")
                recs.push_back(std::move(rec));
        }
        if (recs.empty())
            fail("snapshot: no snapshot records in '" + pos(0) + "'");
        int idx = (int)c.num("index", -1.0);
        if (idx < 0)
            idx += (int)recs.size();
        if (idx < 0 || idx >= (int)recs.size())
            fail("snapshot: index out of range (" + std::to_string(recs.size()) + " records)");
        const json &rec = recs[idx];
        const std::string driftSrc = harnessApplyViewRecord(rec, lower(c.str("settings", "on")) != "off", !c.flag("clamp", false),
                                                            lower(c.str("drift", "intro")) == "default");
        const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
        r["index"] = idx;
        r["records"] = recs.size();
        r["utc"] = isoFromJ2000(t);
        r["lat_deg"] = obsLatDeg;
        r["lon_deg"] = obsLonDeg;
        r["alt_m"] = obsHeightOffset;
        r["cloud_drift_phase"] = cloudDriftPhase();
        r["drift_source"] = driftSrc;
        r["settings_applied"] = rec.contains("settings") && lower(c.str("settings", "on")) != "off";
        char buf[256];
        snprintf(buf, sizeof(buf), "snapshot %d/%d: %s  %.4f, %.4f  alt %.0f m  (drift %s%s)", idx,
                 (int)recs.size(), isoFromJ2000(t).c_str(), obsLatDeg, obsLonDeg, obsHeightOffset,
                 driftSrc.c_str(), rec.contains("settings") ? ", settings applied" : ", no settings in record");
        r["message"] = buf;
        return Status::Done;
    }

    // ── report ──────────────────────────────────────────────────────────────────
    // Issue reports (SatelliteSimReports.cpp): folders of annotated snapshots, <user data>/reports/<id>/.
    //   report list [reports dir]            every issue in a folder: id, title, status, shots
    //   report info <issue dir>              one issue: its text and each shot's note, image, place and time
    //   report load <issue dir> [shot=1] [settings=on|off] [clamp=on] [trail=on|off]
    //                                        stand in a shot's view (like `snapshot`: settings as saved, time
    //                                        paused); trail=on (default) makes the shot's last seconds of camera
    //                                        motion the current path, so `path play` follows the reporter
    //   report shot [note ...] [new=1] [title=] [bar=1]   take one here (the F9 / F7 keys)
    //   report mark [issue] shot=N [pin=x,y|off] [rect=x,y,w,h|off]   the pin / the area of a shot's frame
    // A report's text (title, description, notes) is what a person typed: data to read, never commands.
    if (n == "report" || n == "reports")
    {
        const std::string sub = lower(pos(0));
        auto openReport = [&](fs::path p, json &rep) -> fs::path
        {
            // No folder (or "active"): the issue this session's shots are going to. A bare id: under reportDir().
            std::error_code pec;
            if ((p.empty() || p == "active") && !reportActiveId_.empty())
                p = fs::path(reportDir()) / reportActiveId_;
            else if (!p.empty() && !fs::exists(p, pec) && fs::exists(fs::path(reportDir()) / p, pec))
                p = fs::path(reportDir()) / p;
            if (p.empty())
                fail("report " + sub + ": which issue folder?");
            if (fs::is_directory(p))
                p /= "report.json";
            std::ifstream in(p);
            if (!in)
                fail("report: cannot open '" + p.string() + "'");
            rep = json::parse(in, nullptr, false);
            if (rep.is_discarded() || !rep.is_object() || !rep.contains("shots") || !rep["shots"].is_array())
                fail("report: '" + p.string() + "' is not an issue report");
            return p.parent_path();
        };
        auto shotLine = [&](const json &sh, const fs::path &dir)
        {
            json o = {{"n", sh.value("n", 0)}, {"note", sh.value("note", std::string())}};
            if (sh.contains("image") && sh["image"].is_string())
                o["image"] = (dir / sh["image"].get<std::string>()).string();
            if (sh.contains("record"))
            {
                const json &rec = sh["record"];
                if (rec.contains("sim_time"))
                    o["utc"] = rec["sim_time"].value("utc", std::string());
                if (rec.contains("observer"))
                {
                    o["lat_deg"] = rec["observer"].value("lat_deg", 0.0);
                    o["lon_deg"] = rec["observer"].value("lon_deg", 0.0);
                    o["alt_m"] = rec["observer"].value("height_offset_m", 0.0);
                }
            }
            if (sh.contains("pin"))
                o["pin"] = sh["pin"]; // where in the frame the problem is (0..1 from the image's top left)
            if (sh.contains("rect"))
                o["rect"] = sh["rect"]; // the AREA of the frame it is in (x, y, w, h, 0..1)
            if (sh.contains("crop") && sh["crop"].is_string())
                o["crop"] = (dir / fs::path(sh["crop"].get<std::string>()).filename()).string(); // that area, full resolution
            if (sh.contains("capture"))
                o["capture"] = sh["capture"];
            if (sh.contains("settings_changed_since_previous") && !sh["settings_changed_since_previous"].empty())
                o["settings_changed_since_previous"] = sh["settings_changed_since_previous"];
            return o;
        };
        if (sub == "list")
        {
            const fs::path root = pos(1).empty() ? fs::path(reportDir()) : fs::path(pos(1));
            json list = json::array();
            std::error_code ec;
            std::vector<fs::path> dirs;
            for (const auto &e : fs::directory_iterator(root, ec))
                if (e.is_directory(ec))
                    dirs.push_back(e.path());
            std::sort(dirs.begin(), dirs.end());
            for (const fs::path &d : dirs)
            {
                std::ifstream in(d / "report.json");
                const json rep = in ? json::parse(in, nullptr, false) : json();
                if (!rep.is_object() || !rep.contains("shots"))
                    continue;
                list.push_back({{"id", rep.value("id", d.filename().string())},
                                {"dir", d.string()},
                                {"title", rep.value("title", std::string())},
                                {"category", rep.value("category", std::string())},
                                {"status", rep.value("status", std::string())},
                                {"shots", rep["shots"].size()}});
            }
            r["issues"] = list;
            r["message"] = std::to_string(list.size()) + " issues in " + root.string();
            return Status::Done;
        }
        if (sub == "info")
        {
            json rep;
            const fs::path dir = openReport(pos(1), rep);
            json shots = json::array();
            for (const json &sh : rep["shots"])
                shots.push_back(shotLine(sh, dir));
            for (const char *k : {"id", "title", "description", "category", "status", "created_utc", "app_version", "git_commit", "gpu_device"})
                if (rep.contains(k))
                    r[k] = rep[k];
            r["shots"] = shots;
            r["message"] = rep.value("title", std::string()) + ": " + std::to_string(shots.size()) + " shots";
            return Status::Done;
        }
        if (sub == "load")
        {
            json rep;
            const fs::path dir = openReport(pos(1), rep);
            const int count = (int)rep["shots"].size();
            int idx = (int)c.num("shot", 1.0);
            idx = idx < 0 ? idx + count : idx - 1;
            if (idx < 0 || idx >= count)
                fail("report load: no such shot (" + std::to_string(count) + " in this issue)");
            const json &sh = rep["shots"][(size_t)idx];
            if (!sh.contains("record"))
                fail("report load: the shot has no record");
            const bool withSettings = lower(c.str("settings", "on")) != "off";
            const std::string driftSrc = harnessApplyViewRecord(sh["record"], withSettings, !c.flag("clamp", false), false);
            int keys = 0;
            if (c.flag("trail", true) && sh.contains("trail") && sh["trail"].contains("rows") && sh["trail"]["rows"].size() >= 2)
            {
                // The reporter's last seconds as the current camera path (t from 0; the shot is its last key).
                std::vector<CineKey> &K = cineKeys();
                K.clear();
                const json &rows = sh["trail"]["rows"];
                const double t0 = rows[0][0].get<double>();
                for (const json &row : rows)
                {
                    if (!row.is_array() || row.size() < 8)
                        continue;
                    CineKey k;
                    k.t = row[0].get<double>() - t0;
                    k.lat = row[1].get<double>();
                    k.lon = row[2].get<double>();
                    k.alt = row[3].get<double>();
                    k.az = row[4].get<double>();
                    k.el = row[5].get<double>();
                    k.fov = row[6].get<double>();
                    k.hasSim = true;
                    k.simT = row[7].get<double>();
                    if (!K.empty())
                    {
                        if (k.t <= K.back().t)
                            continue;
                        while (k.lon - K.back().lon > 180.0)
                            k.lon -= 360.0;
                        while (k.lon - K.back().lon < -180.0)
                            k.lon += 360.0;
                    }
                    K.push_back(k);
                }
                keys = (int)K.size();
            }
            r = shotLine(sh, dir);
            r["title"] = rep.value("title", std::string());
            r["description"] = rep.value("description", std::string());
            r["status"] = rep.value("status", std::string());
            r["shots"] = count;
            r["drift_source"] = driftSrc;
            r["settings_applied"] = withSettings && sh["record"].contains("settings");
            r["trail_keys"] = keys;
            if (keys >= 2)
                r["trail_seconds"] = cineKeys().back().t;
            r["message"] = "report " + rep.value("id", std::string()) + " shot " + std::to_string(idx + 1) + "/" + std::to_string(count) +
                           ": " + sh.value("note", std::string()) + (keys >= 2 ? "  (trail loaded: path play)" : "");
            return Status::Done;
        }
        if (sub == "export")
        {
            // report export [issue dir|id]: the zip the window's "Export zip" writes (step 3).
            json rep;
            const fs::path dir = openReport(pos(1), rep);
            const std::string zip = reportExportZip(dir.filename().string());
            if (zip.empty())
                fail("report export: " + reportStatus_);
            r["zip"] = zip;
            r["message"] = zip;
            return Status::Done;
        }
        if (sub == "mark")
        {
            // report mark [issue] shot=N [pin=x,y|off] [rect=x,y,w,h|off]: what a click / a drag on the shot's
            // frame in the window sets (fractions of the image from its top left). The issue must be under the
            // reports folder.
            json rep;
            const fs::path dir = openReport(pos(1), rep);
            const std::string id = dir.filename().string();
            const int shot = (int)c.num("shot", 1.0);
            if (c.has("pin"))
            {
                float x = 0.0f, y = 0.0f;
                const std::string v = c.str("pin", "");
                if (v == "off")
                    reportSetPin(id, shot, false, 0.0f, 0.0f);
                else if (sscanf(v.c_str(), "%f,%f", &x, &y) == 2)
                    reportSetPin(id, shot, true, std::clamp(x, 0.0f, 1.0f), std::clamp(y, 0.0f, 1.0f));
                else
                    fail("report mark: pin=x,y (0..1) or pin=off");
            }
            if (c.has("rect"))
            {
                float x = 0.0f, y = 0.0f, w = 0.0f, h = 0.0f;
                const std::string v = c.str("rect", "");
                if (v == "off")
                    reportSetRect(id, shot, false, 0.0f, 0.0f, 0.0f, 0.0f);
                else if (sscanf(v.c_str(), "%f,%f,%f,%f", &x, &y, &w, &h) == 4)
                    reportSetRect(id, shot, true, x, y, w, h);
                else
                    fail("report mark: rect=x,y,w,h (0..1) or rect=off");
            }
            const fs::path dir2 = openReport(dir, rep);
            for (const json &sh : rep["shots"])
                if (sh.value("n", 0) == shot)
                    r = shotLine(sh, dir2);
            r["message"] = "report " + id + ": shot " + std::to_string(shot) + " marked";
            return Status::Done;
        }
        if (sub == "shot")
        {
            const bool busy = screenshotRequested || screenshotCopyPending || screenshotEncoding.load();
            if (!a.scratch.contains("taken"))
            {
                std::string note;
                for (size_t k = 1; !pos(k).empty(); ++k)
                    note += (note.empty() ? "" : " ") + pos(k);
                reportPendingNote_ = note;
                reportPendingTitle_ = c.str("title", "");
                if (!reportTakeShot(c.flag("new", false), c.flag("bar", false)))
                    return Status::Pending; // an earlier capture is still encoding
                a.scratch["taken"] = true;
                return Status::Pending;
            }
            if (busy)
                return Status::Pending; // until the image is on disk
            json rep;
            const fs::path dir = openReport(fs::path(reportDir()) / reportActiveId_, rep);
            r = shotLine(rep["shots"].back(), dir);
            r["id"] = reportActiveId_;
            r["dir"] = dir.string();
            r["shots"] = rep["shots"].size();
            r["message"] = "report " + reportActiveId_ + ": shot " + std::to_string(rep["shots"].size());
            return Status::Done;
        }
        fail("report: list [dir] | info <issue dir> | load <issue dir> [shot=] [settings=on|off] [clamp=on] [trail=on|off] | shot [note] [new=1] [title=] [bar=1] | mark [issue] shot= [pin=x,y|off] [rect=x,y,w,h|off] | export [issue]");
    }

    // ── beams ───────────────────────────────────────────────────────────────────
    // Where the Reflect-Orbital beams are landing NOW (converged beams per target site, from the last
    // beam readback; sat_orbit.comp builds beams only for satellites above the OBSERVER's horizon, so stand
    // within ~2000 km of the region first). `beams` lists the busiest sites; `beams go [rank=1]
    // [dist_km=0] [bearing=270] [alt=|agl=2] [look=site|up|none]` puts the observer dist_km from the
    // rank-th busiest site along `bearing` (degrees from north, measured FROM the site) and aims at the
    // site's ground point (site) or up the converged beams toward their satellites (up). Guessing a time
    // and a site framed no beams in two passes: which site the mirrors serve depends on the hour.
    if (n == "beams")
    {
        std::vector<int> order;
        for (int i = 0; i < reflectorTargetCount; ++i)
            if (beamSiteConverged[i] > 0)
                order.push_back(i);
        std::sort(order.begin(), order.end(),
                  [&](int x, int y) { return beamSiteConverged[x] > beamSiteConverged[y]; });
        auto latLonOf = [&](const glm::dvec3 &u, double &la, double &lo)
        {
            la = glm::degrees(std::asin(std::clamp(u.z, -1.0, 1.0)));
            lo = glm::degrees(std::atan2(u.y, u.x));
        };
        nlohmann::json list = nlohmann::json::array();
        for (size_t k = 0; k < order.size() && k < 10; ++k)
        {
            double la, lo;
            latLonOf(glm::dvec3(reflectorTargetsECEF[order[k]]), la, lo);
            list.push_back({{"site", order[k]}, {"converged", beamSiteConverged[order[k]]}, {"lat", la}, {"lon", lo}});
        }
        r["sites"] = list;
        const std::string sub = lower(pos(0));
        if (sub.empty() || sub == "list")
        {
            r["message"] = std::to_string(order.size()) + " sites with converged beams" +
                           (order.empty() ? std::string() : ", busiest " + list[0].dump());
            return Status::Done;
        }
        if (sub != "go")
            fail("beams: unknown subcommand '" + sub + "' (list, go)");
        const int rank = std::max(1, (int)c.num("rank", 1.0));
        if ((int)order.size() < rank)
            fail("beams go: only " + std::to_string(order.size()) + " sites have converged beams now");
        const int ti = order[rank - 1];
        const glm::dvec3 site = glm::normalize(glm::dvec3(reflectorTargetsECEF[ti]));
        double sLat, sLon;
        latLonOf(site, sLat, sLon);
        // Destination dist_km from the site along the bearing (great circle).
        const double dAng = c.num("dist_km", 0.0) * 1000.0 / 6371000.0, brg = glm::radians(c.num("bearing", 270.0));
        const double p1 = glm::radians(sLat), l1 = glm::radians(sLon);
        const double p2 = std::asin(std::sin(p1) * std::cos(dAng) + std::cos(p1) * std::sin(dAng) * std::cos(brg));
        const double l2 = l1 + std::atan2(std::sin(brg) * std::sin(dAng) * std::cos(p1),
                                          std::cos(dAng) - std::sin(p1) * std::sin(p2));
        if (followActive)
            stopFollow();
        obsDir = {(float)(std::cos(p2) * std::cos(l2)), (float)(std::cos(p2) * std::sin(l2)), (float)std::sin(p2)};
        obsLatDeg = (float)glm::degrees(p2);
        obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
        obsTerrainH = cpuTerrainHeightM(obsLatDeg, obsLonDeg);
        if (c.has("alt"))
            obsHeightOffset = std::max(0.0f, (float)c.num("alt", 0.0));
        else
            obsHeightOffset = obsTerrainH + (float)c.num("agl", 2.0); // the CPU's ground: approximate
        trailClearPending = true;
        // Aim, in the new observer's ENU frame.
        const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
        const glm::dvec3 east = glm::normalize(glm::cross(glm::dvec3(0, 0, 1), up));
        const glm::dvec3 north = glm::cross(up, east);
        const std::string look = lower(c.str("look", "site"));
        glm::dvec3 dE(0.0);
        if (look == "site")
        {
            const double eyeR = 6371000.0 + std::max((double)obsHeightOffset, (double)obsTerrainH) + 2.0;
            dE = site * (double)reflectorTargetsRadiusM[ti] - up * eyeR;
        }
        else if (look == "up")
            dE = beamSiteDirEcef[ti];
        else if (look != "none")
            fail("beams go: look must be site, up or none");
        if (look != "none")
        {
            if (glm::length(dE) < 1e-9)
                fail("beams go: no beam direction for that site");
            dE = glm::normalize(dE);
            const glm::vec3 d((float)glm::dot(dE, east), (float)glm::dot(dE, north), (float)glm::dot(dE, up));
            harnessTrack_ = 0;
            stopTrack();
            aimCameraAzEl(azDegOf(d), elDegOf(d));
        }
        else
            aimCameraAzEl(camera.azDeg, camera.elDeg);
        r["site"] = ti;
        r["converged"] = beamSiteConverged[ti];
        r["site_lat"] = sLat;
        r["site_lon"] = sLon;
        r["lat_deg"] = obsLatDeg;
        r["lon_deg"] = obsLonDeg;
        r["alt_m"] = obsHeightOffset;
        r["az_deg"] = camera.azDeg;
        r["el_deg"] = camera.elDeg;
        char buf[200];
        snprintf(buf, sizeof(buf), "site %d (%.3f, %.3f), %d converged beams; observer %.4f, %.4f alt %.0f m; az %.1f el %.1f",
                 ti, sLat, sLon, beamSiteConverged[ti], obsLatDeg, obsLonDeg, obsHeightOffset, camera.azDeg,
                 camera.elDeg);
        r["message"] = buf;
        return Status::Done;
    }

    // ── camera ──────────────────────────────────────────────────────────────────
    // ── eclipse solar|lunar: the sim's next eclipse — time, the observer under the Moon, aimed at it ──
    if (n == "eclipse")
    {
        const std::string kind = lower(pos(0));
        if (kind != "solar" && kind != "lunar")
            fail("eclipse: solar | lunar");
        const bool solar = kind == "solar";
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const double t = findEclipse(solar, now + 3600.0);
        if (t < 0.0)
            fail("eclipse: none found within 3 years");
        const double days = std::floor(t / 86400.0);
        simDayJ2000 = (int64_t)days;
        simSecInDay = t - days * 86400.0;
        trailClearPending = true;
        // Lunar: the sub-lunar point. Solar: where the Sun -> Moon axis meets the Earth (the centre of the
        // shadow), else under the axis' nearest point. ECI -> ECEF by the Earth rotation angle (GMST).
        glm::dvec3 m = glm::normalize(moonGeoEciM(t));
        if (solar)
        {
            const glm::dvec3 M = moonGeoEciM(t), d = -glm::normalize(sunDirEciAt(t));
            const double R = 6371000.0, b = glm::dot(M, d), c = glm::dot(M, M) - R * R, disc = b * b - c;
            if (disc > 0.0)
                m = glm::normalize(M + d * (-b - std::sqrt(disc)));
            else    // a partial eclipse only: under the point of the shadow axis nearest the Earth
                m = glm::normalize(M - d * b);
        }
        const double th = earthRotationAngle(t), c = std::cos(th), s = std::sin(th);
        const glm::dvec3 e(c * m.x + s * m.y, -s * m.x + c * m.y, m.z);
        obsDir = glm::vec3(glm::normalize(e));
        obsLatDeg = glm::degrees(std::asin((float)obsDir.z));
        obsLonDeg = glm::degrees(std::atan2(obsDir.y, obsDir.x));
        obsHeightOffset = 2.0f;
        updatePositions(t, 0.0f);
        const glm::vec3 tgt = glm::vec3(moonDirENU);
        aimCameraAzEl(glm::degrees(std::atan2(tgt.x, tgt.y)), glm::degrees(std::asin(std::clamp(tgt.z, -1.0f, 1.0f))));
        r["t_j2000"] = t;
        r["lat_deg"] = obsLatDeg;
        r["lon_deg"] = obsLonDeg;
        r["sun_seen_fraction"] = moonEclipseSolarObs;
        char buf[160];
        snprintf(buf, sizeof(buf), "%s eclipse at j2000 %.0f s, observer %.2f, %.2f (Sun seen %.3f)", kind.c_str(), t,
                 obsLatDeg, obsLonDeg, moonEclipseSolarObs);
        r["message"] = buf;
        return Status::Done;
    }

    // ── aurora: the activity now | next <kp> (jump to the peak of the next storm reaching it) | substorm
    //    (jump to ~10 min after the next substorm onset) | kp <v> (manual) | auto ──
    if (n == "aurora")
    {
        const std::string sub = lower(pos(0));
        const double now = (double)simDayJ2000 * 86400.0 + simSecInDay;
        auto jumpTo = [&](double t)
        {
            const double days = std::floor(t / 86400.0);
            simDayJ2000 = (int64_t)days;
            simSecInDay = t - days * 86400.0;
            trailClearPending = true;
            updatePositions(t, 0.0f);
        };
        if (sub == "kp")
        {
            auroraActivityAuto = 0.0f;
            auroraKpManual = (float)std::clamp(std::stod(pos(1)), 0.0, 11.0);
        }
        else if (sub == "auto")
            auroraActivityAuto = 1.0f;
        else if (sub == "next")
        {
            const double want = pos(1).empty() ? 7.0 : std::stod(pos(1));
            const double maxDays = c.num("days", 1100.0);
            double t = now + 3600.0, found = -1.0;
            for (; t < now + maxDays * 86400.0; t += 900.0)
                if (spaceWeatherKp(t, auroraStormRate) >= want) { found = t; break; }
            if (found < 0.0)
                fail("aurora next: no Kp >= " + pos(1) + " within the search window");
            // Climb to the storm's peak (within a day).
            double best = found, bestKp = spaceWeatherKp(found, auroraStormRate);
            for (double u = found; u < found + 86400.0; u += 600.0)
            {
                const double k = spaceWeatherKp(u, auroraStormRate);
                if (k > bestKp) { bestKp = k; best = u; }
            }
            auroraActivityAuto = 1.0f;
            jumpTo(best);
        }
        else if (sub == "substorm")
        {
            const double kpNow = auroraActivityAuto >= 0.5f ? -1.0 : (double)auroraKpManual;
            double found = -1.0;
            bool wasOn = spaceWeatherAt(now, auroraStormRate, kpNow).subMinutes >= 0.0;
            for (double t = now + 60.0; t < now + 3.0 * 86400.0; t += 60.0)
            {
                const SpaceWeather w = spaceWeatherAt(t, auroraStormRate, kpNow);
                const bool on = w.subMinutes >= 0.0 && w.subMinutes < 2.0;
                if (on && !wasOn && w.subI >= 0.0)
                {
                    const SpaceWeather w10 = spaceWeatherAt(t + 600.0, auroraStormRate, kpNow);
                    if (w10.subI >= c.num("min", 0.4)) { found = t + 600.0; break; }
                }
                wasOn = on;
            }
            if (found < 0.0)
                fail("aurora substorm: none found within 3 days");
            jumpTo(found);
        }
        else if (!sub.empty())
            fail("aurora: [next <kp> [days=]] | substorm [min=] | kp <v> | auto");
        const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
        spaceWeather_ = spaceWeatherAt(tNow, auroraStormRate, auroraActivityAuto >= 0.5f ? -1.0 : (double)auroraKpManual);
        r["aurora"] = auroraStateJson();
        r["t_j2000"] = tNow;
        char buf[200];
        snprintf(buf, sizeof(buf), "Kp %.2f (G%d)%s, substorm %.2f at %.0f min", spaceWeather_.kp,
                 geomagStormScale(spaceWeather_.kp), auroraActivityAuto >= 0.5f ? "" : " manual", spaceWeather_.subI,
                 spaceWeather_.subMinutes);
        r["message"] = buf;
        return Status::Done;
    }

    if (n == "camera")
    {
        const std::string sub = lower(pos(0));
        auto targetOf = [&](const std::string &name, int &track, int &planet)
        {
            const std::string t = lower(name);
            planet = -1;
            if (t == "off" || t == "none")
                track = 0;
            else if (t == "sel" || t == "selection" || t == "sat")
                track = 1;
            else if (t == "sun")
                track = 2;
            else if (t == "moon")
                track = 3;
            else
            {
                track = 4;
                for (int i = 0; i < kPlanetCount; ++i)
                    if (ieq(t, kPlanetNames[i]))
                        planet = i;
                if (planet < 0)
                    fail("camera: unknown target '" + name + "' (sun, moon, sel, a planet name, off)");
            }
        };
        if (sub == "look" || sub == "track")
        {
            int track, planet;
            targetOf(pos(1), track, planet);
            glm::vec3 d;
            if (track != 0 && !harnessLookDir(track, planet, d))
                fail("camera " + sub + ": nothing selected");
            if (track != 0)
                aimCameraAzEl(azDegOf(d), elDegOf(d));
            harnessTrack_ = sub == "track" ? track : 0;
            harnessTrackPlanet_ = planet;
            stopTrack(); // the player's Track lock and this explicit aim would fight for the camera
        }
        else if (!sub.empty())
            fail("camera: unknown subcommand '" + sub + "' (look, track, or az= el= fov=)");
        if (c.has("az") || c.has("el"))
        {
            harnessTrack_ = 0;
            stopTrack(); // an explicit aim outranks the Track lock (and would be overwritten by it)
            aimCameraAzEl((float)c.num("az", camera.azDeg), (float)c.num("el", camera.elDeg));
        }
        if (c.has("fov"))
            camera.fovYDeg = glm::clamp((float)c.num("fov", camera.fovYDeg), SkyCamera::kMinFovDeg, SkyCamera::kMaxFovDeg);
        r["az_deg"] = camera.azDeg;
        r["el_deg"] = camera.elDeg;
        r["fov_y_deg"] = camera.fovYDeg;
        char buf[96];
        snprintf(buf, sizeof(buf), "az %.2f el %.2f fov %.2f%s", camera.azDeg, camera.elDeg, camera.fovYDeg,
                 harnessTrack_ ? " (tracking)" : "");
        r["message"] = buf;
        return Status::Done;
    }

    // ── selection / follow ──────────────────────────────────────────────────────
    if (n == "select")
    {
        const std::string sub = lower(pos(0));
        // A roster satellite destroyed or re-entered by now cannot be selected (selectSatellite ignores it): an error,
        // not "selected" with the previous selection left standing (2026-10-20).
        auto selectOrFail = [&](int idx, const char *what) {
            const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
            double tGone = 0.0;
            const int fate = idx >= 0 && idx < (int)activeSatCount ? rosterFateAt((uint32_t)idx, tNow, &tGone) : 0;
            if (fate >= 2)
            {
                char b[64];
                const double h = (tNow - tGone) / 3600.0;
                snprintf(b, sizeof(b), "%.1f h ago", h);
                fail(std::string(what) + ": satellite " + std::to_string(idx) + " is " + rosterFateName(fate) + " (" + b + ")");
            }
            selectSatellite(idx);
            if (selectedSatIndex != idx)
                fail(std::string(what) + ": satellite " + std::to_string(idx) + " could not be selected");
        };
        if (sub == "none")
        {
            selectedSatIndex = -1;
            selectedPlanetIndex = -1;
        }
        else if (sub == "sat")
        {
            const int idx = (int)parseNum(pos(1), "select sat");
            if (idx < 0 || idx >= (int)satOrbits.size())
                fail("select sat: index out of range (0.." + std::to_string(satOrbits.size() - 1) + ")");
            selectOrFail(idx, "select sat");
        }
        else if (sub == "at")
        {
            // select at <x> <y>: the click pick (meshes on their bounding circle, else the nearest sprite), px from
            // the top-left of the frame.
            if (!ctx_)
                fail("select at: no device");
            const float W = (float)ctx_->swapExtent.width, H = (float)ctx_->swapExtent.height;
            const int idx = pickSatelliteAt((float)parseNum(pos(1), "select at x"), (float)parseNum(pos(2), "select at y"), W, H);
            if (idx < 0)
                fail("select at: no satellite there");
            selectSatellite(idx);
        }
        else if (sub == "const" || sub == "constellation")
        {
            int ci = -1;
            for (int i = 0; i < (int)constellations.size(); ++i)
                if (ieq(constellations[i].name, pos(1).c_str()))
                    ci = i;
            if (ci < 0)
                fail("select const: no constellation '" + pos(1) + "' (see `const list`)");
            int idx;
            if (c.has("n"))
            {
                const int k = (int)c.num("n", 0);
                if (k < 0 || k >= (int)constellations[ci].orbitCount)
                    fail("select const: n out of range");
                idx = (int)constellations[ci].orbitStart + k;
            }
            else
                idx = pickViewerSatellite(ci); // the member highest in the observer's sky
            if (idx < 0)
                fail("select const: constellation has no satellites");
            selectOrFail(idx, "select const");
        }
        else if (sub == "planet")
        {
            int pi = -1;
            for (int i = 0; i < kPlanetCount; ++i)
                if (ieq(pos(1), kPlanetNames[i]))
                    pi = i;
            if (pi < 0)
                fail("select planet: unknown planet '" + pos(1) + "'");
            selectedPlanetIndex = pi;
            selectedSatIndex = -1;
            formatSelectedPlanetInfo();
        }
        else
            fail("select: sat <i> | const <name> [n=<k>] | planet <name> | none");
        if (selectedSatIndex < 0)
            stopTrack(); // deselected, or switched to a planet: there is no orbit left to lock onto
        r = harnessStateJson()["selection"];
        std::string msg = "selected";
        if (r.contains("constellation"))
            msg += " " + r["constellation"].get<std::string>() + " #" + std::to_string(r["n"].get<int>()) +
                   " (sat " + std::to_string(r["sat"].get<int>()) + ")" +
                   (rosterMemberName((uint32_t)r["sat"].get<int>()) ? std::string(": ") + rosterMemberName((uint32_t)r["sat"].get<int>())
                                                                       : std::string());
        if (r.contains("planet"))
            msg += " " + r["planet"].get<std::string>();
        if (r.empty())
            msg = "selection cleared";
        r["message"] = msg;
        return Status::Done;
    }
    if (n == "follow")
    {
        // fly=on (review 22): the UI's Go to / Exit flight instead of the jump; `wait seconds` / frames to watch it.
        const bool fly = c.has("fly") && lower(c.str("fly")) == "on";
        if (lower(pos(0)) == "off")
        {
            stopFollow(fly);
            r["message"] = fly ? "follow off (flying home)" : "follow off";
            return Status::Done;
        }
        if (lower(pos(0)) == "stay")
        {
            // Leave Go to where the camera is: a free observer there (the HUD's "Stay here", KB_FOLLOW_STAY).
            if (!followActive)
                fail("follow stay: not following");
            stopFollowHere();
            r["message"] = "follow off (staying here)";
            r["alt_km"] = obsHeightOffset / 1000.0;
            return Status::Done;
        }
        if (lower(pos(0)) == "return")
        {
            // Following: fly back to the default offset; else follow the last object again (the HUD's Return /
            // "Go to ... again", KB_FOLLOW_RETURN).
            std::string m;
            if (!followReturn(&m))
                fail("follow return: " + (m.empty() ? std::string("nothing to return to") : m));
            r["message"] = m;
            return Status::Done;
        }
        const int idx = c.has("sat") ? (int)c.num("sat", -1) : selectedSatIndex;
        if (idx < 0 || idx >= (int)satOrbits.size())
            fail("follow: select a satellite first (or sat=<i>)");
        if (!isFollowing(idx))
            startFollow(idx, fly);
        if (!followActive)
            fail("follow: this satellite's type has no geometry model");
        if (c.has("offset"))
        {
            double x, y, z;
            if (sscanf(c.str("offset").c_str(), "%lf,%lf,%lf", &x, &y, &z) != 3)
                fail("follow: offset=<along>,<cross>,<radial> in metres");
            followOffset = glm::dvec3(x, y, z);
            updateFollow(0.0f);
        }
        // toward=observer|sun [dist=<m>] [side=<deg>] [lift=<deg>]: put the camera `dist` metres from the satellite
        // on its line to the parked ground observer (the side, and the flare, the observer sees) or to the Sun,
        // rotated `side` degrees about the radial and `lift` degrees toward the radial, and aim at the satellite.
        if (c.has("toward"))
        {
            const std::string w = lower(c.str("toward"));
            const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
            const double th = earthRotationAngle(tNow), ct = std::cos(th), st = std::sin(th);
            auto toEcef = [&](glm::dvec3 v) { return glm::dvec3(ct * v.x + st * v.y, -st * v.x + ct * v.y, v.z); };
            const SatOrbitState so = satOrbitStateAt(orbitElemsOf(satOrbits[followSatIndex]), tNow);
            const glm::dvec3 P = toEcef(so.posEci), Rh = glm::normalize(P);
            glm::dvec3 Th = toEcef(so.velocity);
            const glm::dvec3 Nh = glm::normalize(glm::cross(Rh, Th));
            Th = glm::cross(Nh, Rh);
            glm::dvec3 d;
            if (w == "observer")
                d = glm::normalize(followHomeEcef() - P);
            else if (w == "sun")
                d = glm::normalize(toEcef(glm::dvec3(sunDirECI)));
            else
                fail("follow: toward=observer|sun");
            glm::dvec3 o(glm::dot(d, Th), glm::dot(d, Nh), glm::dot(d, Rh));
            const double side = glm::radians(c.num("side", 0.0)), lift = glm::radians(c.num("lift", 0.0));
            o = glm::dvec3(std::cos(side) * o.x - std::sin(side) * o.y, std::sin(side) * o.x + std::cos(side) * o.y, o.z);
            const double hl = std::sqrt(o.x * o.x + o.y * o.y);
            const double el = std::atan2(o.z, hl) + lift;
            const glm::dvec3 hd = hl > 1e-9 ? glm::dvec3(o.x / hl, o.y / hl, 0.0) : glm::dvec3(1.0, 0.0, 0.0);
            o = std::cos(el) * hd + glm::dvec3(0.0, 0.0, std::sin(el));
            followOffset = o * c.num("dist", 300.0);
            followAimLock = true;
            updateFollow(0.0f);
            followAimLock = false;
            if (lower(c.str("look")) == "observer")
            {
                // Look away from the satellite, at the parked ground observer (down a Reflect beam to its site).
                const glm::dvec3 d = glm::normalize(followHomeEcef() - followObsEcef);
                const glm::dvec3 up = glm::dvec3(obsDir);
                const glm::dvec3 h = d - glm::dot(d, up) * up;
                if (glm::length(h) > 1e-9)
                    obsFacing = glm::vec3(glm::normalize(h));
                camera.elDeg = (float)glm::degrees(std::asin(std::clamp(glm::dot(d, up), -1.0, 1.0)));
                camera.elDeg = std::clamp(camera.elDeg, -89.0f, 89.0f);
                updateFollow(0.0f);
            }
        }
        else if (c.has("aimat"))
        {
            // aimat=<along>,<cross>,<radial>: look at that point of the satellite's frame (m from the satellite),
            // e.g. a formation's centroid from `cluster`.
            double x, y, z;
            if (sscanf(c.str("aimat").c_str(), "%lf,%lf,%lf", &x, &y, &z) != 3)
                fail("follow: aimat=<along>,<cross>,<radial> in metres");
            updateFollow(0.0f);
            const glm::dvec3 tgt = followLastP + x * followBasisT + y * followBasisN + z * followBasisR;
            const glm::dvec3 d = glm::normalize(tgt - followObsEcef);
            const glm::dvec3 up = glm::dvec3(obsDir);
            const glm::dvec3 h = d - glm::dot(d, up) * up;
            if (glm::length(h) > 1e-9)
                obsFacing = glm::vec3(glm::normalize(h));
            camera.elDeg = std::clamp((float)glm::degrees(std::asin(std::clamp(glm::dot(d, up), -1.0, 1.0))), -89.0f, 89.0f);
            updateFollow(0.0f);
        }
        else if (c.has("aim") && lower(c.str("aim")) == "sat")
        {
            followAimLock = true; // look at the satellite once, then free (the view holds in its frame)
            updateFollow(0.0f);
            followAimLock = false;
        }
        r["sat"] = followSatIndex;
        r["offset_m"] = {followOffset.x, followOffset.y, followOffset.z};
        r["message"] = followLabel;
        return Status::Done;
    }
    // ── track (the selection panel's Track button) ────────────────────────────────────────────────
    // `track on|off` is the PLAYER's camera lock (startTrack/stopTrack/updateTrack): the observer stays
    // put and only the aim follows the selected satellite, so the wheel still zooms. Not to be confused
    // with `camera track <target>`, which is this harness's own aim-every-frame. No argument = toggle.
    // Needs a satellite selection; `select none`, `select planet`, `camera az=|el=|look|track`, `follow`
    // and any scripted camera key all release it.
    // `viewer [aim=free|observer|toward|sun] [light=live|studio] [glare=on|off] [shadows=on|off] [dist=<radii>]`
    // — the 3D view's own controls (its chips and RENDER section) for a `ui open viewer` capture.
    // aim=observer is the Observer chip: the satellite from the ground observer's direction, where the
    // glare (2026-09-26) shows the glints of the flare the observer sees. aim=sun (harness only) looks from
    // the Sun's side, where a Sun-facing array glints. dist is in model radii.
    // ── flares (release cinematic scouting) ─────────────────────────────────────────────────────────
    // `flares "<constellation>" [window=600] [step=5] [minel=10] [top=10] [maxmag=]`: the CPU photometry
    // (evalSatTraceRow, the trace window's evaluator) over every member of a constellation that is above
    // `minel` at some step in the next `window` seconds of sim time, for the ground observer; returns the
    // `top` brightest peaks (refined to 0.1 s) with time, magnitude after extinction, az/el, phase and the
    // dominant lobe. Model types only. For finding a satellite flare to frame.
    // `cluster [sat=<i>]`: the formation cluster of a satellite (default: the selection) at the current sim time —
    // each member's offset from the given satellite in its along-track / cross-track / radial frame (m, CPU double)
    // and their centroid. For framing follow shots on a formation's centre (the release cinematic).
    if (n == "cluster")
    {
        const int idx = c.has("sat") ? (int)c.num("sat", -1) : selectedSatIndex;
        if (idx < 0 || idx >= (int)satOrbits.size())
            fail("cluster: select a satellite first (or sat=<i>)");
        const ConstellationConfig &cc = constellations[satOrbits[idx].constIdx];
        const int C = std::max(1, cc.clusterSize);
        const int first = (int)cc.orbitStart + ((idx - (int)cc.orbitStart) / C) * C;
        const double tNow = (double)simDayJ2000 * 86400.0 + simSecInDay;
        const SatOrbitState s0 = satOrbitStateAt(orbitElemsOf(satOrbits[idx]), tNow);
        const glm::dvec3 Rh = glm::normalize(s0.posEci);
        const glm::dvec3 Nh = glm::normalize(glm::cross(Rh, s0.velocity));
        const glm::dvec3 Th = glm::cross(Nh, Rh);
        json mem = json::array();
        glm::dvec3 sum(0.0);
        int cnt = 0;
        for (int k = first; k < first + C && k < (int)cc.orbitStart + (int)cc.orbitCount; ++k)
        {
            const glm::dvec3 d = satOrbitStateAt(orbitElemsOf(satOrbits[k]), tNow).posEci - s0.posEci;
            const glm::dvec3 o(glm::dot(d, Th), glm::dot(d, Nh), glm::dot(d, Rh));
            mem.push_back({{"sat", k}, {"offset_m", {o.x, o.y, o.z}}});
            sum += o;
            ++cnt;
        }
        const glm::dvec3 cen = sum / (double)std::max(1, cnt);
        r["members"] = mem;
        r["centroid_m"] = {cen.x, cen.y, cen.z};
        r["first"] = first;
        r["message"] = "cluster of " + std::to_string(cnt) + " from sat " + std::to_string(first);
        return Status::Done;
    }
    if (n == "flares")
    {
        int ci = -1;
        for (int i = 0; i < (int)constellations.size(); ++i)
            if (ieq(constellations[i].name, pos(0).c_str()))
                ci = i;
        if (ci < 0)
            fail("flares: no constellation '" + pos(0) + "' (see `const list`)");
        if (followActive)
            fail("flares: not in follow mode (the observer must be on the ground)");
        const ConstellationConfig &cc = constellations[ci];
        if (cc.typeIdx >= satTypes.size())
            fail("flares: bad type");
        const SatelliteType &type = satTypes[cc.typeIdx];
        if (const char *why = photometryUnsupportedReason(type))
            fail(std::string("flares: ") + why);
        const double window = c.num("window", 600.0), step = std::max(0.2, c.num("step", 5.0));
        const double minEl = c.num("minel", 10.0);
        const int top = (int)c.num("top", 10.0);
        const double t0 = (double)simDayJ2000 * 86400.0 + simSecInDay;
        SatTraceSetup s;
        s.lobeBudget = type.lobeBudget;
        s.occlusion = false;
        s.obsDirEcef = glm::dvec3(obsDir);
        s.obsRadiusM = (double)obsEyeRadiusM();
        s.flareTiltRad = (double)glm::radians(flareMitigationTiltDeg);
        s.extinctionK = extinctionCoeff;
        s.groundSite = attUsesGroundSite(type.groups);
        if (s.groundSite)
            s.groundAim = groundSiteAim();
        struct Peak { int sat; double t, mag, el, az, phase, range; int lobe; double contrast = 0.0; };
        std::vector<Peak> peaks;
        const int nSteps = (int)std::ceil(window / step);
        const bool azFilter = c.has("az");
        const double azC = c.num("az", 0.0), azW = c.num("azw", 30.0);
        auto azOk = [&](double az) { return !azFilter || std::abs(std::remainder(az - azC, 360.0)) <= azW; };
        const double sinMin = std::sin(glm::radians(minEl));
        for (uint32_t k = 0; k < cc.orbitCount; ++k)
        {
            const int si = (int)cc.orbitStart + (int)k;
            s.orbit = orbitElemsOf(satOrbits[si]);
            s.satelliteIndex = si;
            // Cheap elevation prefilter on the orbit alone.
            bool any = false;
            for (int i = 0; i <= nSteps && !any; i += 4)
            {
                const double t = t0 + std::min(window, i * step);
                const glm::dvec3 o = observerEciAt(s.obsDirEcef, s.obsRadiusM, t);
                const glm::dvec3 d = satOrbitStateAt(s.orbit, t).posEci - o;
                any = glm::dot(d, glm::normalize(o)) > (sinMin - 0.15) * glm::length(d);
            }
            if (!any)
                continue;
            Peak best{si, 0.0, INFINITY, 0, 0, 0, 0, -1, 0.0};
            for (int i = 0; i <= nSteps; ++i)
            {
                const SatTraceRow rw = evalSatTraceRow(s, type.groups, type.lobes, nullptr, t0 + std::min(window, i * step));
                if (rw.elevationDeg >= minEl && azOk(rw.azimuthDeg) && std::isfinite(rw.magApparent) && rw.magApparent < best.mag)
                    best = {si, rw.tJ2000, rw.magApparent, rw.elevationDeg, rw.azimuthDeg, rw.phaseDeg, rw.rangeM, rw.dominantLobe, 0.0};
            }
            if (!std::isfinite(best.mag))
                continue;
            for (double t = best.t - step; t <= best.t + step; t += 0.1) // refine
            {
                if (t < t0 || t > t0 + window)
                    continue;
                const SatTraceRow rw = evalSatTraceRow(s, type.groups, type.lobes, nullptr, t);
                if (rw.elevationDeg >= minEl && azOk(rw.azimuthDeg) && std::isfinite(rw.magApparent) && rw.magApparent < best.mag)
                    best = {si, rw.tJ2000, rw.magApparent, rw.elevationDeg, rw.azimuthDeg, rw.phaseDeg, rw.rangeM, rw.dominantLobe, 0.0};
            }
            // Contrast: how much fainter it is `half` seconds either side (a flare's rise and fall).
            const double half = c.num("half", 15.0);
            double side = INFINITY;
            for (double dt : {-half, half})
            {
                const SatTraceRow rw = evalSatTraceRow(s, type.groups, type.lobes, nullptr, best.t + dt);
                side = std::min(side, std::isfinite(rw.magApparent) ? rw.magApparent : 99.0);
            }
            best.contrast = side - best.mag;
            if (best.t - t0 < half || t0 + window - best.t < half)
                best.contrast = 0.0; // at the window's edge: not a peak
            peaks.push_back(best);
        }
        const bool byContrast = lower(c.str("sort")) == "contrast";
        std::sort(peaks.begin(), peaks.end(), [&](const Peak &a, const Peak &b) {
            return byContrast ? a.mag - a.contrast < b.mag - b.contrast : a.mag < b.mag;
        });
        nlohmann::json arr = nlohmann::json::array();
        for (int i = 0; i < (int)peaks.size() && i < top; ++i)
        {
            const Peak &p = peaks[i];
            char tb[32];
            formatSimClock(p.t, tb, sizeof(tb), false);
            arr.push_back({{"sat", p.sat}, {"n", p.sat - (int)cc.orbitStart}, {"t_from_now_s", p.t - t0}, {"utc_hms", tb},
                           {"j2000_s", p.t}, {"mag", p.mag}, {"el_deg", p.el}, {"az_deg", p.az}, {"phase_deg", p.phase},
                           {"range_km", p.range / 1000.0}, {"lobe", p.lobe}, {"contrast_mag", p.contrast}});
        }
        r["peaks"] = arr;
        r["candidates"] = (int)peaks.size();
        r["message"] = std::to_string(peaks.size()) + " satellites above " + std::to_string((int)minEl) + " deg; brightest " +
                       (peaks.empty() ? std::string("-") : std::to_string(peaks[0].mag));
        return Status::Done;
    }
    if (n == "viewer")
    {
        auto onOff =[&](const char *key, bool &v) {
            if (!c.has(key))
                return;
            const std::string s = lower(c.str(key));
            if (s != "on" && s != "off")
                fail(std::string("viewer: ") + key + "=on|off");
            v = s == "on";
        };
        if (c.has("aim"))
        {
            const std::string a = lower(c.str("aim"));
            if (a == "free")
                viewerAim = 0;
            else if (a == "observer" || a == "from")
                viewerAim = 1;
            else if (a == "toward")
                viewerAim = 2;
            else if (a == "sun")
            {
                // Harness only: a free camera on the Sun's side of the satellite (the backscatter
                // direction) — where a Sun-facing array's own glint is, whatever the observer sees. The
                // same yaw/pitch frame recordModelViewer's presets use (ECEF, the satellite's east/north/up).
                if (viewerSatIndex < 0 || viewerSatIndex >= (int)satOrbits.size())
                    fail("viewer aim=sun: open the viewer on a satellite first (`ui open viewer`)");
                const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
                const double th = earthRotationAngle(t), ct = std::cos(th), st = std::sin(th);
                auto toEcef = [&](glm::dvec3 v) { return glm::dvec3(ct * v.x + st * v.y, -st * v.x + ct * v.y, v.z); };
                const glm::dvec3 P = toEcef(satOrbitStateAt(orbitElemsOf(satOrbits[viewerSatIndex]), t).posEci);
                const glm::dvec3 up = glm::normalize(P);
                glm::dvec3 east = glm::cross(glm::dvec3(0.0, 0.0, 1.0), up);
                east = glm::length(east) > 1e-9 ? glm::normalize(east) : glm::dvec3(1.0, 0.0, 0.0);
                const glm::dvec3 north = glm::cross(up, east);
                const glm::dvec3 d = glm::normalize(toEcef(glm::dvec3(sunDirECI)));
                viewerAim = 0;
                viewerPitchDeg = (float)glm::degrees(std::asin(glm::clamp(glm::dot(d, up), -1.0, 1.0)));
                viewerYawDeg = (float)glm::degrees(std::atan2(glm::dot(d, north), glm::dot(d, east)));
            }
            else
                fail("viewer: aim=free|observer|toward|sun");
        }
        if (c.has("light"))
        {
            const std::string l = lower(c.str("light"));
            if (l != "live" && l != "studio")
                fail("viewer: light=live|studio");
            viewerStudioLight = l == "studio";
        }
        onOff("glare", viewerGlare);
        onOff("shadows", viewerShadows);
        if (c.has("dist"))
        {
            const SatMeshRenderer::TypeMesh *tm = meshRenderer.typeMesh(viewerType);
            if (!tm)
                fail("viewer dist=: open the viewer on a model satellite first (`ui open viewer`)");
            viewerDist = (float)(parseNum(c.str("dist"), "viewer dist") * std::max(0.1f, tm->boundsRadius));
        }
        updateViewerObserverInfo();
        r["aim"] = viewerAim;
        r["studio_light"] = viewerStudioLight;
        r["glare"] = viewerGlare;
        r["flare_per_i"] = viewerFlarePerI;
        r["glints_last_frame"] = meshRenderer.viewerGlintCount(); // the previous frame's list
        r["sat"] = viewerSatIndex;
        nlohmann::json phot = nlohmann::json::array();
        for (int i = 0; i < kViewerPhotRows; ++i)
            phot.push_back(std::string(kViewerPhotLabels[i]) + ": " + viewerPhotValue[i]);
        r["photometry"] = phot;
        r["message"] = std::string("viewer aim=") + (viewerAim == 1 ? "observer" : viewerAim == 2 ? "toward" : "free") +
                       (viewerGlare ? " glare=on" : " glare=off");
        return Status::Done;
    }
    if (n == "track")
    {
        const std::string arg = lower(pos(0));
        bool want = trackActive;
        if (arg == "on" || arg == "true" || arg == "1")
            want = true;
        else if (arg == "off" || arg == "false" || arg == "0")
            want = false;
        else if (!arg.empty())
            fail("track: on|off");
        if (want)
        {
            if (selectedSatIndex < 0 || selectedSatIndex >= (int)satOrbits.size())
                fail("track: select a satellite first (`select sat <i>` or `select const <name>`)");
            startTrack();
        }
        else
            stopTrack();
        // The camera's own aim beside the satellite's (selection.az_deg/el_deg): while tracking they
        // converge, which is exactly what a test asserts.
        r["track"] = trackActive;
        r["sat"] = selectedSatIndex;
        r["camera_az_deg"] = camera.azDeg;
        r["camera_el_deg"] = camera.elDeg;
        r["selection"] = harnessStateJson()["selection"];
        r["message"] = trackActive ? "track on" : "track off";
        return Status::Done;
    }
    if (n == "pad")
    {
        // A gamepad button press as pollGamepad handles it (no controller needed): the context meaning
        // first (tutorial card, selection focus / A / B), else the button's binding. Marks the pad as
        // the last input, so the HUD shows its focus ring and hints.
        static const std::pair<const char *, int> kButtons[] = {
            {"a", GLFW_GAMEPAD_BUTTON_A}, {"b", GLFW_GAMEPAD_BUTTON_B}, {"x", GLFW_GAMEPAD_BUTTON_X},
            {"y", GLFW_GAMEPAD_BUTTON_Y}, {"lb", GLFW_GAMEPAD_BUTTON_LEFT_BUMPER}, {"rb", GLFW_GAMEPAD_BUTTON_RIGHT_BUMPER},
            {"view", GLFW_GAMEPAD_BUTTON_BACK}, {"back", GLFW_GAMEPAD_BUTTON_BACK}, {"start", GLFW_GAMEPAD_BUTTON_START},
            {"ls", GLFW_GAMEPAD_BUTTON_LEFT_THUMB}, {"rs", GLFW_GAMEPAD_BUTTON_RIGHT_THUMB},
            {"up", GLFW_GAMEPAD_BUTTON_DPAD_UP}, {"down", GLFW_GAMEPAD_BUTTON_DPAD_DOWN},
            {"left", GLFW_GAMEPAD_BUTTON_DPAD_LEFT}, {"right", GLFW_GAMEPAD_BUTTON_DPAD_RIGHT}};
        const std::string name = lower(pos(0));
        int b = -1;
        for (const auto &e : kButtons)
            if (name == e.first)
                b = e.second;
        if (b < 0)
            fail("pad: a|b|x|y|lb|rb|view|start|ls|rs|up|down|left|right");
        lastInputWasGamepad = true;
        bool used = padContextButton(b, vCursorToggled && uiVisible);
        std::string fired = used ? "context" : "";
        if (!used)
            for (size_t i = 0; i < keybindings.size(); ++i)
                if (!keybindings[i].held && keybindings[i].gpButton == b)
                {
                    dispatchKeyAction((int)i);
                    fired = keybindings[i].action;
                }
        r["button"] = name;
        r["action"] = fired;
        r["pad_focus"] = padFocus;
        r["selection"] = selectedSatIndex;
        r["message"] = "pad " + name + " -> " + (fired.empty() ? std::string("nothing") : fired);
        return Status::Done;
    }
    if (n == "bookmark" || n == "bookmarks")
    {
        // bookmark add [name] [id=] [scale=] [paused=] | go <n> | update <n> | rename <n> <name> | delete <n> | restore | list
        // The Bookmarks window's actions (SatelliteSimBookmarks.cpp). A thumbnail is captured on a later frame:
        // `wait 3` after add / update before looking at the window.
        if (!bookmarksLoaded_)
            bookmarksLoad();
        const std::string sub = lower(pos(0));
        auto idxArg = [&]() -> int
        {
            const int i = (int)parseNum(pos(1), "bookmark index") - 1;
            if (i < 0 || i >= (int)bookmarks_.size())
                fail("bookmark: no bookmark " + pos(1) + " (" + std::to_string(bookmarks_.size()) + " saved)");
            return i;
        };
        std::string msg;
        if (sub == "add")
        {
            std::string name;
            for (size_t k = 1; !pos(k).empty(); ++k)
                name += (name.empty() ? "" : " ") + pos(k);
            // id= replaces a bookmark with that id (a rerun of the defaults recipe over seeded defaults)
            if (!c.str("id", "").empty())
                for (int k = 0; k < (int)bookmarks_.size(); ++k)
                    if (bookmarks_[k].id == c.str("id", ""))
                    {
                        bookmarkDelete(k);
                        break;
                    }
            const int i = bookmarkAdd(name);
            if (i < 0)
                fail("bookmark add: " + bmStatus_);
            // id= fixes the id (the shipped defaults: tools/harness/scripts/default_bookmarks.satcmd); the
            // thumbnail request follows it. scale= / paused= store the optional look (the clock a Go sets).
            const std::string want = c.str("id", "");
            if (!want.empty())
            {
                if (bmPendingThumb_ == bookmarks_[i].id)
                    bmPendingThumb_ = want;
                bookmarks_[i].id = want;
            }
            if (c.has("scale"))
                bookmarks_[i].lookTimeScaleIdx = (int)c.num("scale", 0.0);
            if (c.has("paused"))
                bookmarks_[i].lookPaused = c.str("paused", "0") != "0" ? 1 : 0;
            bookmarksSave();
            msg = "added " + std::to_string(i + 1) + ": " + bookmarks_[i].name;
        }
        else if (sub == "go")
        {
            const int i = idxArg();
            bookmarkGo(i);
            msg = "went to " + std::to_string(i + 1) + ": " + bookmarks_[i].name;
        }
        else if (sub == "update")
        {
            const int i = idxArg();
            bookmarkUpdate(i);
            msg = "updated " + std::to_string(i + 1);
        }
        else if (sub == "rename")
        {
            const int i = idxArg();
            std::string name;
            for (size_t k = 2; !pos(k).empty(); ++k)
                name += (name.empty() ? "" : " ") + pos(k);
            bookmarks_[i].name = name;
            bookmarksSave();
            msg = "renamed " + std::to_string(i + 1) + ": " + name;
        }
        else if (sub == "delete")
        {
            const int i = idxArg();
            bookmarkDelete(i);
            msg = "deleted " + std::to_string(i + 1);
        }
        else if (sub == "restore")
        {
            const int k = bookmarkRestoreDefaults();
            msg = "restored " + std::to_string(k) + " default bookmarks";
        }
        else if (!sub.empty() && sub != "list")
            fail("bookmark: add [name] [id=] [scale=] [paused=] | go <n> | update <n> | rename <n> <name> | delete <n> | restore | list");
        json list = json::array();
        for (const Bookmark &b : bookmarks_)
            list.push_back({{"name", b.name}, {"id", b.id}, {"when", b.meta[0]}, {"where", b.meta[1]}, {"thumbnail", b.slot >= 0}});
        r["bookmarks"] = list;
        r["thumbnail_pending"] = !bmPendingThumb_.empty() || !bmCaptureFor_.empty();
        r["message"] = msg.empty() ? std::to_string(bookmarks_.size()) + " bookmarks" : msg;
        return Status::Done;
    }
    if (n == "tutorial")
    {
        // The first-run tutorial (SatelliteSimTutorial.cpp): start it (at a step: a number or a name),
        // move through it, end it, or report where it is. Its steps finish on real input, which a
        // harness run cannot press — `next` is the way past an action step.
        static const char *kNames[TUT_COUNT] = {"look", "move", "altitude", "boost", "select",
                                                "actions", "time", "capture", "settings", "graphics"};
        const std::string sub = lower(pos(0));
        auto stepArg = [&](const std::string &a) -> int
        {
            for (int i = 0; i < TUT_COUNT; ++i)
                if (a == kNames[i])
                    return i;
            if (!a.empty() && std::isdigit((unsigned char)a[0]))
                return std::clamp(std::atoi(a.c_str()) - 1, 0, TUT_COUNT - 1);
            fail("tutorial: unknown step '" + a + "' (1-9 or look|move|altitude|boost|select|actions|time|capture|settings)");
            return 0;
        };
        // pad=1 shows the gamepad graphics (the card follows the last input used; a real pad is not needed)
        if (!c.str("pad", "").empty())
            lastInputWasGamepad = c.str("pad", "0") != "0";
        if (sub == "start")
            startTutorial(pos(1).empty() ? 0 : stepArg(lower(pos(1))));
        else if (sub == "step")
            startTutorial(stepArg(lower(pos(1))));
        else if (sub == "next")
            setTutorialStep(tutStep + 1);
        else if (sub == "back")
            setTutorialStep(std::max(0, tutStep - 1));
        else if (sub == "skip" || sub == "end")
            endTutorial(false);
        else if (!sub.empty() && sub != "state")
            fail("tutorial: start [step] | step <n> | next | back | skip | state  [pad=0|1]");
        r["active"] = tutActive;
        r["step"] = tutActive ? tutStep + 1 : 0;
        r["step_name"] = tutActive ? kNames[tutStep] : "";
        r["progress"] = tutProgress;
        r["done_showing"] = tutDoneT >= 0.0f;
        r["tutorial_done"] = tutorialDone;
        r["gamepad_view"] = tutGamepadView();
        r["message"] = tutActive ? std::string("tutorial step ") + std::to_string(tutStep + 1) + " (" + kNames[tutStep] + ")"
                                 : std::string("tutorial off");
        return Status::Done;
    }
    if (n == "kessler")
    {
        // kessler [state] | breakup <sat> [kind=explosion|asat] | collide <a> <b> | wait [days=<d>] [done]
        // (2026-10-16, "The Sun's Fury"; SatelliteSimKessler.cpp).
        const std::string sub = lower(pos(0));
        if (sub == "breakup" || sub == "collide")
        {
            // "sel" = the selected satellite (2026-10-20, QA: with nothing selected it broke up satellite 0).
            if (lower(pos(1)) == "sel" && (selectedSatIndex < 0 || selectedSatIndex >= (int)satOrbits.size()))
                fail("kessler " + sub + " sel: nothing selected");
            const uint32_t a0 = lower(pos(1)) == "sel" ? (uint32_t)std::max(selectedSatIndex, 0)
                                                        : (uint32_t)parseNum(pos(1), "kessler sat");
            uint8_t kind = kessler::EV_EXPLOSION;
            uint32_t b0 = 0xFFFFFFFFu;
            if (sub == "collide")
            {
                kind = kessler::EV_COLLISION;
                b0 = (uint32_t)parseNum(pos(2), "kessler collide b");
            }
            else if (lower(c.str("kind", "explosion")) == "asat")
                kind = kessler::EV_ASAT;
            std::string why;
            kForceRebuildOnce = c.num("rebuild", 0.0) > 0.5; // the old path: rebuild the history (comparison)
            if (!kesslerUserEvent(kind, a0, b0, &why))
                fail("kessler " + sub + ": " + why);
            r["breakup_ms"] = kLastBreakupMs;
            r["in_place"] = kLastBreakupFast;
            r["frontier_days"] = (kFrontier - kesslerEpoch()) / 86400.0;
        }
        else if (sub == "wait")
        {
            // Until the worker has simulated `days` past the CME (or finished, with `done`).
            const bool all = lower(pos(1)) == "done" || c.has("done");
            const double days = c.num("days", 0.0);
            kesslerWant(all ? 1e18 : kesslerEpoch() + days * 86400.0);
            const bool ok = kWorkerDone || (!all && kFrontier >= kesslerEpoch() + days * 86400.0);
            if (!ok)
                return Status::Pending;
        }
        else if (sub == "reentry")
        {
            // kessler reentry: the next re-entry over a dark sky (the settings' "Watch the next re-entry").
            std::string msg;
            if (!kesslerGoToNextReentry(msg))
                fail("kessler reentry: " + msg);
            r["reentry"] = msg;
        }
        else if (sub == "select" && lower(pos(1)) == "dead")
        {
            // kessler select dead [k=<n>] [formation=1]: the n-th roster satellite dead (tumbling) at the current time
            // (formation=1: only formation members, which drift along-track once dead).
            const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
            const int32_t sec = (int32_t)std::floor(t - kesslerEpoch());
            uint32_t want = (uint32_t)c.num("k", 0.0), seen = 0;
            const bool formOnly = c.num("formation", 0.0) > 0.5;
            int found = -1;
            for (uint32_t i = 0; i < activeSatCount && 2ull * i + 1 < kRosterWords.size(); ++i)
                if (sec >= kRosterWords[2ull * i] && sec < kRosterWords[2ull * i + 1] &&
                    (!formOnly || satOrbits[i].formation) && seen++ == want)
                {
                    found = (int)i;
                    break;
                }
            if (found < 0)
                fail("kessler select dead: only " + std::to_string(seen) + " dead satellites now");
            selectSatellite(found);
        }
        else if (sub == "select")
        {
            // kessler select [k=<n>]: the n-th record (default 0) of the newest player breakup, once the
            // worker has replayed it (Pending until then).
            const kessler::Event *ev = nullptr;
            for (size_t e = kEvents.size(); e-- > 0;)
                if (kEvents[e].origin == kessler::ORIGIN_USER)
                {
                    ev = &kEvents[e];
                    break;
                }
            if (!ev)
            {
                if (kUserEvents.empty())
                    fail("kessler select: no player breakup yet");
                return Status::Pending;
            }
            const uint32_t k = (uint32_t)c.num("k", 0.0);
            if (k >= ev->nRec)
                fail("kessler select: that breakup made " + std::to_string(ev->nRec) + " records");
            const uint32_t evIdx = (uint32_t)(ev - kEvents.data());
            uint32_t seen = 0;
            int found = -1;
            for (uint32_t j = 0; j < (uint32_t)kPool.size(); ++j)
                if (kPool[j].event == evIdx && seen++ == k)
                {
                    found = (int)j;
                    break;
                }
            if (found < 0)
                return Status::Pending; // not mirrored yet
            selectSatellite((int)poolIdx0() + found);
        }
        else if (!sub.empty() && sub != "state")
            fail("kessler: state | breakup <sat> [kind=explosion|asat] | collide <a> <b> | wait [days=] [done] | select [k=]");
        r["kessler"] = kesslerStateJson();
        const nlohmann::json &kj = r["kessler"];
        char msg[200];
        snprintf(msg, sizeof(msg), "frontier day %.1f%s, %zu events, %zu records; now: %d dead, %d fragments",
                 kj["frontier_days"].get<double>(), kWorkerDone ? " (done)" : "", kEvents.size(), kPool.size(),
                 kj["now"]["dead_satellites"].get<int>(), kj["now"]["fragments"].get<int>());
        r["message"] = msg;
        return Status::Done;
    }

    if (n == "missions")
    {
        // missions [state] | go <index|twilight|crew|fury|fury_night> [watch=1] | tab upcoming|ongoing|controls
        // (2026-10-19, SatelliteSimMissions.cpp): the Missions window's rows and actions.
        const std::string sub = lower(pos(0));
        if (sub.empty() || sub == "state")
        {
            r["missions"] = missionsStateJson();
            return Status::Done;
        }
        if (sub == "go")
        {
            std::string msg;
            if (!missionsGo(lower(pos(1)), c.num("watch", 0.0) > 0.5, msg))
                fail("missions go: " + msg);
            msStatus = msg;
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "ongoing")
        {
            // missions ongoing <row> [go=1]: an Ongoing row's Select (go=1: its Go to); rows are state's `ongoing`.
            std::string msg;
            missionsRefresh(true);
            if (!missionsOngoingAct((int)parseNum(pos(1), "missions ongoing"), c.num("go", 0.0) > 0.5, msg))
                fail("missions ongoing: " + msg);
            msStatus = msg;
            r["message"] = msg;
            r["selected"] = selectedSatIndex;
            return Status::Done;
        }
        if (sub == "milestone" || sub == "milestones")
        {
            // missions milestone [list|next|prev|<index>] [row=<ongoing row>] (2026-10-20): the mission timeline of
            // Go to's object, else the selection's (row=: an Ongoing launch row's); next / prev / an index skip to just
            // before that milestone at 1x, keeping the selection / Go to on the object.
            const std::string what = lower(pos(1).empty() ? std::string("list") : pos(1));
            const bool isList = what == "list" || what == "state";
            int dir = 0, index = -1;
            if (what == "next")
                dir = 1;
            else if (what == "prev" || what == "previous")
                dir = -1;
            else if (!isList)
                index = (int)parseNum(what, "missions milestone");
            std::string msg;
            MissionSubject s;
            if (c.has("row"))
            {
                missionsRefresh(true);
                const int row = (int)c.num("row", 0);
                if (row < 0 || row >= (int)msOngoing.size() || (msOngoing[(size_t)row].cat != 0 && msOngoing[(size_t)row].cat != 2))
                    fail("missions milestone: row " + std::to_string(row) + " is not a launch in flight (see `missions` ongoing)");
                if (!isList && !missionsMilestone(row, dir, index, msg))
                    fail("missions milestone: " + msg);
                missionSubjectOfBatch(msOngoing[(size_t)row].key, s, kMsPayload);
            }
            else
            {
                if (!missionSubjectNow(s))
                    fail("missions milestone: the selection / Go to's object has no mission timeline");
                if (!isList)
                {
                    if (!missionJump(s, dir, index, msg))
                        fail("missions milestone: " + msg);
                    missionSubjectNow(s);
                }
            }
            r["mission"] = missionSubjectJson(s);
            r["message"] = isList ? s.title : msg;
            return Status::Done;
        }
        if (sub == "tab")
        {
            const std::string t = lower(pos(1));
            msTab = t == "ongoing" ? 1 : t == "controls" ? 2 : 0;
            msChrome.open = true;
            uiVisible = true;
            return Status::Done;
        }
        fail("missions: state | go <index|twilight|crew|fury|fury_night> [watch=1] | ongoing <row> [go=1] | "
             "milestone [list|next|prev|<i>] [row=<ongoing row>] | tab upcoming|ongoing|controls");
    }
    if (n == "deploy")
    {
        // deploy [state] | stats [from=YYYY-MM-DD] [to=YYYY-MM-DD] [step=30] (2026-10-20, the deployment era).
        const std::string sub = lower(pos(0));
        if (sub == "stats")
        {
            const double from = launch::dateToJ2000(c.str("from", "2026-10-20"));
            const double to = launch::dateToJ2000(c.str("to", "2036-11-22"));
            if (std::isnan(from) || std::isnan(to) || to <= from)
                fail("deploy stats: from= / to= as YYYY-MM-DD, from < to");
            r["stats"] = deployStatsJson(from, to, c.num("step", 30.0));
            r["message"] = "peak " + r["stats"].value("peak_launches_per_day", nlohmann::json(0)).dump() + " launches a day";
            return Status::Done;
        }
        r["deploy"] = deployStateJson();
        r["message"] = std::to_string(r["deploy"].value("roster_there", 0ull)) + " of " +
                       std::to_string(r["deploy"].value("roster_total", 0ull)) + " roster satellites there";
        return Status::Done;
    }
    if (n == "launch")
    {
        // launch [state] | list [hours=48] | next [twilight=0|1] (2026-10-18, SatelliteSimLaunches.cpp).
        const std::string sub = lower(pos(0));
        if (sub == "padstats")
        {
            r["padstats"] = launchPadStatsJson(c.num("hours", 168.0));
            r["message"] = std::to_string(r["padstats"].value("same_pad_pairs_within_90min", 0)) +
                           " same-pad pairs within 90 min";
            return Status::Done;
        }
        if (sub == "list")
        {
            const nlohmann::json l = launchListJson(c.num("hours", 48.0));
            r["launches"] = l;
            r["message"] = std::to_string(l.size()) + " launches";
            return Status::Done;
        }
        if (sub == "why")
        {
            r["why"] = launchWhyJson(c.num("hours", 720.0));
            return Status::Done;
        }
        if (sub == "next")
        {
            std::string msg;
            const std::string when = lower(c.str("when", c.num("twilight", 0.0) > 0.5 ? "twilight" : "any"));
            if (!launchGoToNextWhen(msg, when == "twilight" ? 1 : when == "day" ? 2 : when == "night" ? 3 : 0,
                                    c.str("vehicle", ""), c.num("pad_dist_m", 0.0), c.num("lead", 30.0),
                                    c.num("side", 1.0)))
                fail("launch next: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "ship")
        {
            std::string msg;
            if (!launchGoToShipEntry(msg))
                fail("launch ship: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "ships")
        {
            r["ships"] = launchShipsJson();
            r["message"] = std::to_string(r["ships"].size()) + " landing ships at sea";
            return Status::Done;
        }
        if (sub == "droneship")
        {
            // launch droneship [lead=60] [dist_m=1500] [az=60] [h=5] [day=1|night=1] [vehicle=falcon9|new_glenn]: the next
            // booster landing on a ship (2026-10-20): the clock lead s before touchdown at 1x, the observer dist_m from
            // the ship toward az (clockwise from its bow) h m above the sea, looking just above the deck.
            std::string msg;
            const int dn = c.num("day", 0.0) > 0.5 ? 1 : c.num("night", 0.0) > 0.5 ? -1 : 0;
            if (!launchGoToShipLanding(c.num("lead", 60.0), c.num("dist_m", 1500.0), c.num("az", 60.0), c.num("h", 5.0),
                                       dn, c.str("vehicle", ""), msg))
                fail("launch droneship: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "pads")
        {
            r["pads"] = launchPadsJson();
            r["message"] = std::to_string(r["pads"].size()) + " pads";
            return Status::Done;
        }
        if (sub == "pad")
        {
            // launch pad <site id> [dist_m=600] [az=200] [h=2]: the observer at a pad, looking at its tower
            // (az clockwise from the pad's heading).
            std::string msg;
            if (!launchGoToPad(pos(1), c.num("dist_m", 600.0), c.num("az", 200.0), c.num("h", 2.0), msg))
                fail("launch pad: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "landing")
        {
            // launch landing [booster=1] [lead=90] [dist_m=2500] [day=1]: the next tower catch (a ship, or a booster;
            // day=1: with the Sun above 10 deg at the pad).
            std::string msg;
            const bool sea = c.num("droneship", 0.0) > 0.5; // (2026-10-20) a booster's landing at sea
            const bool bst = c.num("booster", 0.0) > 0.5 || sea;
            if (!launchGoToLanding(sea ? 2 : bst ? 1 : 0, c.num("lead", bst ? 60.0 : 90.0), c.num("dist_m", 2500.0), msg,
                                   c.num("day", 0.0) > 0.5))
                fail("launch landing: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "vent")
        {
            std::string msg;
            if (!launchGoToVent(msg))
                fail("launch vent: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "deorbit" || sub == "deploy" || sub == "fairing")
        {
            // (2026-10-19) the next sunlit upper stage as it turns for its deorbit burn / 10 s before it deploys /
            // (2026-10-20) 10 s before its fairing separates, the observer 250 km aside (then `launch select
            // kind=upper` and `follow`).
            std::string msg;
            if (!launchGoToStageEvent(msg, sub == "deorbit" ? 2 : sub == "deploy" ? 3 : 4, c.str("filter", ""),
                                      (int)c.num("min_members", 0.0)))
                fail("launch " + sub + ": " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "reentry")
        {
            std::string msg;
            if (!launchGoToNextReentry(msg))
                fail("launch reentry: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "select")
        {
            std::string msg;
            const std::string kind = lower(c.str("kind", "train"));
            // Drawn objects (this frame's heads): rocket stages, the ship coming home, re-entry fireballs.
            static const std::pair<const char *, int> kTr[] = {{"rocket", 1}, {"booster", 11}, {"upper", 12},
                                                               {"ship", 2}, {"fireball", 0},
                                                               {"retiree_reentry", 3}, {"stage_reentry", 4},
                                                               {"kessler_reentry", 5}, {"fragment", 6}, {"fairing", 7}};
            int trKind = -1;
            for (const auto &k : kTr)
                if (kind == k.first)
                    trKind = k.second;
            if (trKind >= 0)
            {
                // (2026-10-20) Right after a jump (`launch next`, a Missions Go) the framed launch's stages are listed
                // a frame or more later: wait up to 30 frames for them rather than take another launch's rocket.
                if ((trKind == 1 || trKind > 10) && launchFocusKey != 0 && !launchFocusListed() && a.frame < 30)
                    return Status::Pending;
                if (!selectTransient(trKind > 10 ? 1 : trKind, msg, trKind > 10 ? trKind - 11 : -1,
                                     c.num("nearest", 0.0) > 0.5))
                    fail("launch select: " + msg);
            }
            else if (!launchSelect(kind == "retiree" ? 1 : 0, msg, (int)c.num("member", -1.0)))
                fail("launch select: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "legacy")
        {
            // (2026-10-19) write the launch records the pre-fix way (diagnostics: an in-app before / after of the jitter)
            launchLegacyRecords(lower(pos(1)) == "on");
            r["message"] = std::string("legacy launch records ") + (lower(pos(1)) == "on" ? "on" : "off");
            return Status::Done;
        }
        if (sub == "jitter")
        {
            r["jitter"] = launchJitterJson((int)c.num("batches", 6.0));
            r["message"] = r["jitter"].dump();
            return Status::Done;
        }
        if (sub == "trace")
        {
            // launch trace [from=YYYY-MM-DD[THH:MM]] [days=400] [vehicle=<id>] [program=<name part>] [key=<batch>]
            // (2026-10-20, tools/harness/mission_ops.py): the next launch of every (vehicle, program) pair after `from`
            // (default: now), traced through its phases from the launch model; key= traces one batch.
            if (c.has("key"))
            {
                r["trace"] = launchTraceBatchJson(std::stoull(c.str("key", "0")));
                r["message"] = r["trace"].value("vehicle", std::string("?"));
                return Status::Done;
            }
            double from = (double)simDayJ2000 * 86400.0 + simSecInDay;
            if (c.has("from"))
            {
                from = launch::dateToJ2000(c.str("from", ""));
                if (std::isnan(from))
                    fail("launch trace: from= as YYYY-MM-DD[THH:MM]");
            }
            r["trace"] = launchTraceJson(from, c.num("days", 400.0), c.str("vehicle", ""), c.str("program", ""),
                                         c.num("phases", 1.0) > 0.5);
            int nf = 0;
            for (const auto &x : r["trace"].value("combos", nlohmann::json::array()))
                nf += x.value("found", false);
            r["message"] = std::to_string(nf) + " of " + std::to_string(r["trace"].value("combos", nlohmann::json::array()).size()) +
                           " vehicle / program pairs found";
            return Status::Done;
        }
        if (sub == "formation")
        {
            // launch formation key=<batch> [samples=8]: a formation train's arrangement from release to arrival.
            r["formation"] = launchFormationJson(std::stoull(c.str("key", "0")), (int)c.num("samples", 8.0), c.num("before", 0.0) > 0.5);
            r["message"] = r["formation"].value("constellation", std::string("?"));
            return Status::Done;
        }
        if (sub == "goto")
        {
            // launch goto key=<batch> [t=<s after lift-off>] [ground=0|1]: the clock at that moment of that launch (1x,
            // running); ground=1 also puts the observer beside the pad. Then `launch select kind=...` takes its stages.
            std::string msg;
            if (!launchGoToKey(std::stoull(c.str("key", "0")), c.num("ground", 0.0) > 0.5, -c.num("t", 0.0), msg))
                fail("launch goto: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (sub == "check")
        {
            r["check"] = launchCheckJson();
            r["message"] = r["check"].dump();
            return Status::Done;
        }
        if (sub == "look")
        {
            std::string msg;
            if (!launchLook((int)c.num("stage", 1.0), msg))
                fail("launch look: " + msg);
            r["message"] = msg;
            return Status::Done;
        }
        if (!sub.empty() && sub != "state")
            fail("launch: state | list [hours=48] | next [twilight=0|1] | look [stage=0|1] | pads | pad <site> | landing [booster=1]");
        r["launches"] = launchStateJson();
        r["message"] = r["launches"].dump();
        return Status::Done;
    }

    if (n == "roster")
    {
        // roster list | remove "<name>" | add "<name>" (2026-10-15): change the roster at runtime. Removed
        // constellations stay available to add back (same satellites: each keeps its random stream).
        const std::string sub = lower(pos(0));
        if (sub == "remove" || sub == "add")
        {
            if (!ctx_)
                fail("roster: no device");
            const std::string name = pos(1);
            if (sub == "remove" && !rosterRemove(*ctx_, name))
                fail("roster remove: no constellation '" + name + "' in the roster (see `roster list`)");
            std::string why;
            if (sub == "add" && !rosterAdd(*ctx_, name, &why))
                fail("roster add: " + why);
        }
        else if (!sub.empty() && sub != "list")
            fail("roster: list | remove <name> | add <name>");
        json in = json::array(), out = json::array();
        for (const auto &cc : constellations)
            in.push_back({{"name", cc.name}, {"count", cc.orbitCount}, {"first", cc.orbitStart}});
        for (const auto &d : constellationDefs)
        {
            bool present = false;
            for (const auto &cc : constellations)
                present = present || cc.name == d.name;
            if (!present)
                out.push_back(d.name);
        }
        r["roster"] = in;
        r["available"] = out;
        r["satellites"] = activeSatCount;
        r["message"] = std::to_string(constellations.size()) + " constellations, " + std::to_string(activeSatCount) +
                       " satellites, " + std::to_string(out.size()) + " removed";
        return Status::Done;
    }

    if (n == "const")
    {
        const std::string name = pos(0);
        if (lower(name) == "list" || name.empty())
        {
            json list = json::array();
            for (const auto &cc : constellations)
                list.push_back({{"name", cc.name}, {"category", constCategoryOf(cc)}, {"enabled", cc.enabled}, {"highlight", cc.highlight},
                                {"count", cc.orbitCount}, {"type", satTypes[cc.typeIdx].name}});
            r["constellations"] = list;
            r["message"] = std::to_string(list.size()) + " constellations";
            return Status::Done;
        }
        const std::string state = lower(pos(1));
        int changed = 0;
        for (auto &cc : constellations)
            if (lower(name) == "all" || ieq(cc.name, name.c_str()))
            {
                if (state == "on" || state == "off")
                    cc.enabled = state == "on";
                else if (!state.empty())
                    fail("const: on|off");
                if (c.has("highlight"))
                    cc.highlight = c.flag("highlight", cc.highlight);
                ++changed;
            }
        if (!changed)
            fail("const: no constellation '" + name + "' (see `const list`)");
        r["message"] = std::to_string(changed) + " constellation(s) updated";
        return Status::Done;
    }

    // ── settings ────────────────────────────────────────────────────────────────
    if (n == "get")
    {
        const json all = buildSettingsJson();
        if (c.pos.empty())
        {
            r["settings"] = all;
            r["message"] = "all settings";
            return Status::Done;
        }
        const json *v = lookupPath(all, pos(0));
        if (!v)
            fail("get: no setting '" + pos(0) + "' (use `get` or `get <section>` to list)");
        r["key"] = pos(0);
        r["value"] = *v;
        r["message"] = pos(0) + " = " + v->dump();
        return Status::Done;
    }
    if (n == "expect")
    {
        // expect <setting path | cine.<path> | state.<path>> <value> [tol=1e-4]: fails the run unless it matches
        // (numbers within tol, anything else as text). cine.<path> reads the open cinematic's JSON (cine.name,
        // cine.shots.0.keys.1.t); state.<path> the `state` command's JSON (state.observer.lat_deg, state.time.utc).
        const std::string path = pos(0);
        const bool isCine = path.rfind("cine.", 0) == 0, isState = path.rfind("state.", 0) == 0;
        const json src = isCine ? cineToJson(cine_) : isState ? harnessStateJson() : buildSettingsJson();
        const json *v = lookupPath(src, isCine ? path.substr(5) : isState ? path.substr(6) : path);
        if (!v)
            fail("expect: no value '" + path + "'");
        const std::string want = pos(1);
        bool ok;
        if (v->is_number())
        {
            char *end = nullptr;
            const double w = std::strtod(want.c_str(), &end);
            ok = end != want.c_str() && std::abs(v->get<double>() - w) <= c.num("tol", 1e-4) * std::max(1.0, std::abs(w));
        }
        else if (v->is_boolean())
            ok = (want == "true" || want == "1" || want == "on") == v->get<bool>();
        else
            ok = (v->is_string() ? v->get<std::string>() : v->dump()) == want;
        r["key"] = path;
        r["value"] = *v;
        if (!ok)
            fail("expect " + path + " = " + want + ", got " + v->dump());
        r["message"] = path + " = " + v->dump() + " (as expected)";
        return Status::Done;
    }
    if (n == "set")
    {
        std::vector<std::pair<std::string, std::string>> pairs;
        for (size_t i = 0; i + 1 < c.pos.size(); i += 2)
            pairs.push_back({c.pos[i], c.pos[i + 1]});
        if (c.pos.size() % 2)
            fail("set: <section.key> <value>");
        for (const auto &kv : c.kv)
            pairs.push_back(kv);
        if (pairs.empty())
            fail("set: <section.key> <value>");
        const json before = buildSettingsJson();
        const float oldRenderScale = renderScale;
        const FpsCapMode oldCap = fpsCapMode;
        std::string msg;
        for (const auto &kv : pairs)
        {
            const json *cur = lookupPath(before, kv.first);
            if (!cur)
                fail("set: no setting '" + kv.first + "' (use `get <section>` to list)");
            json v = parseValue(kv.second);
            if (cur->is_number() && !v.is_number())
                fail("set: " + kv.first + " is a number, got '" + kv.second + "'");
            if (cur->is_boolean() && v.is_string())
            {
                const std::string s = lower(v.get<std::string>());
                if (s == "on" || s == "off")
                    v = s == "on";
            }
            applySettingsJson(patchFor(kv.first, v), true);
            const json after = buildSettingsJson(); // held: lookupPath returns a pointer into it
            const json *now = lookupPath(after, kv.first);
            r[kv.first] = now ? *now : json();
            msg += (msg.empty() ? "" : ", ") + kv.first + " = " + (now ? now->dump() : "?");
        }
        if (ctx_ && fabsf(renderScale - oldRenderScale) > 1e-6f)
        {
            vkDeviceWaitIdle(ctx_->device);
            destroySkyLowResResources(ctx_->device);
            createSkyLowResResources(*ctx_);
        }
        if (fpsCapMode != oldCap)
            applyFpsCapMode();
        r["message"] = msg;
        return Status::Done;
    }
    if (n == "preset")
    {
        for (int i = 0; i <= (int)GraphicsPreset::Potato; ++i)
            if (ieq(pos(0), kGraphicsPresetNames[i]))
            {
                applyGraphicsPreset((GraphicsPreset)i);
                r["preset"] = kGraphicsPresetNames[i];
                r["knockout_mask"] = debugDisableMask;
                r["message"] = std::string("preset ") + kGraphicsPresetNames[i];
                return Status::Done;
            }
        fail("preset: Planetarium, Low, Medium, High, Ultra, Potato (or Custom)");
    }
    if (n == "knockout" || n == "ko")
    {
        auto bitOf = [&](const std::string &key) -> uint32_t
        {
            if (key == "potato_sky")
                return 262144u;
            if (key == "lite_sky")
                return 524288u;
            for (int i = 0; i < debugToggleTableSize(); ++i)
            {
                uint32_t bit;
                const char *label, *jk;
                if (debugToggleAt(i, bit, label, jk) && (key == jk || key == std::to_string(bit)))
                    return bit;
            }
            fail("knockout: unknown key '" + key + "' (see `knockout list`)");
        };
        if (lower(pos(0)) == "list")
        {
            json list = json::array();
            for (int i = 0; i < debugToggleTableSize(); ++i)
            {
                uint32_t bit;
                const char *label, *jk;
                if (debugToggleAt(i, bit, label, jk))
                    list.push_back({{"key", jk}, {"bit", bit}, {"label", label}, {"on", (debugDisableMask & bit) != 0}});
            }
            list.push_back({{"key", "potato_sky"}, {"bit", 262144}, {"label", "Potato sky shader"}, {"on", (debugDisableMask & 262144u) != 0}});
            list.push_back({{"key", "lite_sky"}, {"bit", 524288}, {"label", "SKY_LITE sky shader"}, {"on", (debugDisableMask & 524288u) != 0}});
            r["knockouts"] = list;
            r["message"] = std::to_string(list.size()) + " knockout bits";
            return Status::Done;
        }
        for (const std::string &tok : c.pos)
        {
            if (tok == "none" || tok == "0")
                debugDisableMask = 0;
            else if (isdigit((unsigned char)tok[0]))
                debugDisableMask = (uint32_t)strtoul(tok.c_str(), nullptr, 0);
            else if (tok[0] == '+')
                debugDisableMask |= bitOf(tok.substr(1));
            else if (tok[0] == '-')
                debugDisableMask &= ~bitOf(tok.substr(1));
            else
                debugDisableMask |= bitOf(tok);
        }
        graphicsPreset = GraphicsPreset::Custom; // what the Display tab does when a box is ticked
        // A knockout changes what is drawn: no temporal history carries the other mask's image (the clouds' history
        // kept cloud drawn into sky that the restored terrain covers: the knockout selftest's ridge outlines).
        skyTaaHistValid = false;
        cv2HistoryValid = false;
        r = harnessStateJson()["render"];
        r["message"] = "mask " + std::to_string(debugDisableMask);
        return Status::Done;
    }

    // ── UI ──────────────────────────────────────────────────────────────────────
    if (n == "ui")
    {
        const std::string sub = lower(pos(0));
        auto chromeOf = [&](const std::string &w) -> WindowChrome *
        {
            const std::string l = lower(w);
            if (l == "settings")
                return &settingsChrome;
            if (l == "viewcontrols" || l == "controls")
                return &viewControlsChrome;
            if (l == "trace")
                return &traceChrome;
            if (l == "info")
                return &infoChrome;
            if (l == "viewer" || l == "view")
                return &viewerChrome;
            if (l == "bookmarks" || l == "bookmark")
                return &bmChrome;
            if (l == "reports" || l == "report" || l == "issues")
                return &rpChrome;
            if (l == "cine" || l == "cinematics")
                return &cineChrome;
            if (l == "missions" || l == "mission")
                return &msChrome;
            return nullptr;
        };
        if (sub == "dump")
        {
            // Every drawn rect/text/image with its box, id and text, plus three automatic checks:
            // text cut by its scissor (a clipped label, or a scroll view's edge row), text off the
            // window, and text boxes overlapping each other (two labels drawn on top of each other).
            if (!harnessUi_)
                fail("ui dump: no UI renderer yet");
            if (a.frame == 0)
            {
                harnessUi_->requestLayoutDump();
                return Status::Pending;
            }
            if (!harnessUi_->layoutDumpReady())
            {
                if (a.frame > 30)
                    fail("ui dump: the UI did not record (is a clean capture pending every frame?)");
                return Status::Pending;
            }
            const auto &items = harnessUi_->layoutDump();
            const float W = ctx_ ? (float)ctx_->swapExtent.width : 0.0f, H = ctx_ ? (float)ctx_->swapExtent.height : 0.0f;
            json list = json::array(), clipped = json::array(), offscreen = json::array(), overlaps = json::array();
            std::vector<size_t> texts;
            for (size_t i = 0; i < items.size(); ++i)
            {
                const auto &it = items[i];
                json e = {{"i", i}, {"kind", it.kind}, {"box", {it.x, it.y, it.w, it.h}}};
                if (!it.id.empty())
                    e["id"] = it.id;
                if (it.kind == std::string("text"))
                {
                    e["text"] = it.text;
                    e["font"] = it.fontSize;
                    texts.push_back(i);
                    const float tol = 0.5f;
                    const bool inClip = it.x >= it.clip[0] - tol && it.y >= it.clip[1] - tol &&
                                        it.x + it.w <= it.clip[0] + it.clip[2] + tol &&
                                        it.y + it.h <= it.clip[1] + it.clip[3] + tol;
                    if (!inClip)
                        clipped.push_back({{"i", i}, {"text", it.text}, {"box", e["box"]},
                                           {"clip", {it.clip[0], it.clip[1], it.clip[2], it.clip[3]}}});
                    if (it.x < -tol || it.y < -tol || it.x + it.w > W + tol || it.y + it.h > H + tol)
                        offscreen.push_back({{"i", i}, {"text", it.text}, {"box", e["box"]}});
                }
                list.push_back(e);
            }
            for (size_t p = 0; p < texts.size(); ++p)
                for (size_t q = p + 1; q < texts.size(); ++q)
                {
                    const auto &A = items[texts[p]], &B = items[texts[q]];
                    // Only text under the same scissor: a window floating over the HUD is layering,
                    // not a layout bug.
                    if (A.clip[0] != B.clip[0] || A.clip[1] != B.clip[1] || A.clip[2] != B.clip[2] || A.clip[3] != B.clip[3])
                        continue;
                    const float ox = std::min(A.x + A.w, B.x + B.w) - std::max(A.x, B.x);
                    const float oy = std::min(A.y + A.h, B.y + B.h) - std::max(A.y, B.y);
                    if (ox > 1.0f && oy > 1.0f)
                        overlaps.push_back({{"a", A.text}, {"b", B.text}, {"a_box", {A.x, A.y, A.w, A.h}},
                                            {"b_box", {B.x, B.y, B.w, B.h}}});
                }
            json out = {{"width", W}, {"height", H}, {"ui_scale", uiScale}, {"items", list},
                        {"text_clipped", clipped}, {"text_offscreen", offscreen}, {"text_overlaps", overlaps}};
            const std::string name = safeName(pos(1).empty() ? "layout" : pos(1));
            const std::string path = harnessRunner_->capturePath(name, ".layout.json");
            std::ofstream(path) << out.dump(1) << '\n';
            r["file"] = path;
            r["items"] = list.size();
            r["text_clipped"] = clipped.size();
            r["text_offscreen"] = offscreen.size();
            r["text_overlaps"] = overlaps.size();
            if (!overlaps.empty())
                r["overlaps"] = overlaps;
            r["message"] = name + ".layout.json: " + std::to_string(list.size()) + " items, " +
                           std::to_string(clipped.size()) + " clipped, " + std::to_string(offscreen.size()) +
                           " off-screen, " + std::to_string(overlaps.size()) + " overlapping text";
            return Status::Done;
        }
        if (sub == "click")
        {
            // ui click <ElementId>[:index] [fx=] [fy=] [to_fx= to_fy=] [hold=frames]: the pointer moves onto the element's centre (last frame's
            // layout), presses for a frame, releases. Drives the real input path (hover, click, text-field focus).
            if (a.frame == 0)
            {
                std::string idStr = pos(1);
                int index = -1;
                const size_t colon = idStr.rfind(':');
                if (colon != std::string::npos)
                {
                    index = (int)parseNum(idStr.substr(colon + 1), "ui click index");
                    idStr = idStr.substr(0, colon);
                }
                if (idStr.empty())
                    fail("ui click <ElementId>[:index] (ids as in `ui dump`)");
                const Clay_String cs{false, (int32_t)idStr.size(), idStr.c_str()};
                const Clay_ElementId eid = index >= 0 ? Clay_GetElementIdWithIndex(cs, (uint32_t)index) : Clay_GetElementId(cs);
                const Clay_ElementData d = Clay_GetElementData(eid);
                if (!d.found)
                    fail("ui click: no element '" + pos(1) + "' in the last frame's layout");
                harnessPtrX_ = d.boundingBox.x + d.boundingBox.width * (float)c.num("fx", 0.5);
                harnessPtrY_ = d.boundingBox.y + d.boundingBox.height * (float)c.num("fy", 0.5);
                // to_fx= / to_fy=: a DRAG to that point of the element (the press, then the pointer moves there
                // and holds a few frames before the release).
                a.scratch["tx"] = d.boundingBox.x + d.boundingBox.width * (float)c.num("to_fx", c.num("fx", 0.5));
                a.scratch["ty"] = d.boundingBox.y + d.boundingBox.height * (float)c.num("to_fy", c.num("fy", 0.5));
                if (harnessUi_ && (harnessPtrX_ < 0.0f || harnessPtrY_ < 0.0f || harnessPtrX_ >= harnessUi_->input().screenW ||
                                   harnessPtrY_ >= harnessUi_->input().screenH))
                    fail("ui click: '" + pos(1) + "' is off screen at " + std::to_string((int)harnessPtrX_) + ", " +
                         std::to_string((int)harnessPtrY_) + " (scrolled out of its window?)");
                harnessPtrActive_ = true;
                harnessPtrDown_ = false;
                a.scratch["x"] = harnessPtrX_;
                a.scratch["y"] = harnessPtrY_;
                return Status::Pending; // a frame with the pointer over it (hover), then the press
            }
            if (a.frame == 1)
            {
                harnessPtrDown_ = true;
                return Status::Pending;
            }
            const bool drag = c.has("to_fx") || c.has("to_fy");
            if (drag && harnessPtrDown_)
            {
                harnessPtrX_ = a.scratch["tx"].get<float>();
                harnessPtrY_ = a.scratch["ty"].get<float>();
            }
            if (a.frame < 2 + (int)c.num("hold", drag ? 3.0 : 0.0))
                return Status::Pending;
            if (harnessPtrDown_)
            {
                harnessPtrDown_ = false;
                return Status::Pending; // the release frame
            }
            harnessPtrActive_ = false;
            r["x"] = a.scratch["x"];
            r["y"] = a.scratch["y"];
            r["text_focus"] = textEdit_.id != 0;
            r["message"] = "clicked " + pos(1);
            return Status::Done;
        }
        if (sub == "type")
        {
            // ui type <text>: each character as typed (onChar), into the focused text field or the console.
            std::string t;
            for (size_t i = 1; i < c.pos.size(); ++i)
                t += (i > 1 ? " " : "") + c.pos[i];
            for (char ch : t)
                onChar(win, (unsigned char)ch);
            r["text_focus"] = textEdit_.id != 0;
            r["buffer"] = textEdit_.buf;
            r["message"] = "typed " + std::to_string(t.size()) + " chars";
            return Status::Done;
        }
        if (sub == "key")
        {
            // ui key <enter|esc|tab|backspace|delete|left|right|home|end|ctrl+a|ctrl+v|...>: a key press (onKey).
            const std::string k = lower(pos(1));
            static const std::pair<const char *, int> keys[] = {
                {"enter", GLFW_KEY_ENTER}, {"esc", GLFW_KEY_ESCAPE}, {"escape", GLFW_KEY_ESCAPE}, {"tab", GLFW_KEY_TAB},
                {"backspace", GLFW_KEY_BACKSPACE}, {"delete", GLFW_KEY_DELETE}, {"left", GLFW_KEY_LEFT},
                {"right", GLFW_KEY_RIGHT}, {"home", GLFW_KEY_HOME}, {"end", GLFW_KEY_END}, {"w", GLFW_KEY_W},
                {"up", GLFW_KEY_UP}, {"down", GLFW_KEY_DOWN},
                // the screenshot / report keys (2026-10-19): f7 new issue shot, f9 shot, f10 screenshot with UI, f12 screenshot
                {"f7", GLFW_KEY_F7}, {"f8", GLFW_KEY_F8}, {"f9", GLFW_KEY_F9}, {"f10", GLFW_KEY_F10}, {"f12", GLFW_KEY_F12},
                // the mission milestone keys (2026-10-20)
                {"]", GLFW_KEY_RIGHT_BRACKET}, {"[", GLFW_KEY_LEFT_BRACKET}};
            int code = -1;
            for (const auto &e : keys)
                if (k == e.first)
                    code = e.second;
            if (code < 0)
                fail("ui key: enter | esc | tab | backspace | delete | left | right | up | down | home | end | w | f7 | f8 | f9 | f10 | f12 | ] | [");
            onKey(win, code, GLFW_PRESS);
            onKey(win, code, GLFW_RELEASE);
            r["text_focus"] = textEdit_.id != 0;
            r["buffer"] = textEdit_.buf;
            r["message"] = "key " + k;
            return Status::Done;
        }
        if (sub == "show" || sub == "hide")
            uiVisible = sub == "show";
        else if (sub == "scale")
            uiScale = glm::clamp((float)parseNum(pos(1), "ui scale"), 0.75f, 2.0f);
        else if (sub == "open")
        {
            const std::string w = lower(pos(1));
            if (w == "info" || w == "viewer" || w == "view")
            {
                if (selectedSatIndex < 0)
                    fail("ui open " + w + ": select a satellite first");
                const SatOrbit &o = satOrbits[selectedSatIndex];
                if (!meshRenderer.typeMesh((int)o.typeIdx))
                    fail("ui open " + w + ": this satellite's type has no geometry model");
                openModelViewer((int)o.typeIdx, satTypes[o.typeIdx].name.c_str(), o.altM, selectedSatIndex, w != "info");
            }
            else if (w == "console")
                consoleOpen_ = true;
            else if (w == "trace")
            {
                if (selectedSatIndex < 0)
                    fail("ui open trace: select a satellite first");
                computeSelectedTrace();
            }
            else
            {
                WindowChrome *ch = chromeOf(w);
                if (!ch)
                    fail("ui open: settings, viewcontrols, trace, info, viewer, cine, bookmarks, reports, missions, console");
                ch->open = true;
                if (ch == &msChrome)
                {
                    msNextWall = msPinWall = -1e300;
                    if (c.has("tab"))
                    {
                        const std::string t = lower(c.str("tab"));
                        msTab = t == "ongoing" ? 1 : t == "controls" ? 2 : 0;
                    }
                }
            }
            if (c.has("tab") && lower(pos(1)) != "missions" && lower(pos(1)) != "mission")
            {
                const int t = settingsTabIndexByName(c.str("tab"));
                if (t < 0)
                    fail("ui open: unknown settings tab '" + c.str("tab") + "'");
                settingsActiveTab = t;
                if (settingsTabIsAdvanced(t)) // (buildSettingsTabbedBody)
                    showAdvancedSettings = true;
            }
            uiVisible = true;
        }
        else if (sub == "close")
        {
            if (lower(pos(1)) == "all")
                consoleOpen_ = settingsChrome.open = viewControlsChrome.open = traceChrome.open = infoChrome.open =
                    viewerChrome.open = cineChrome.open = bmChrome.open = msChrome.open = rpChrome.open = false;
            else if (lower(pos(1)) == "console")
                consoleOpen_ = false;
            else if (WindowChrome *ch = chromeOf(pos(1)))
                ch->open = false;
            else
                fail("ui close: settings, viewcontrols, trace, info, viewer, cine, bookmarks, reports, missions, console, all");
        }
        else
            fail("ui: show | hide | scale <x> | open <window> [tab=<name>] | close <window|all> | hint | dump [name] | "
                 "click <ElementId>[:index] | type <text> | key <name>");
        r["message"] = "ui " + sub + (pos(1).empty() ? "" : " " + pos(1));
        return Status::Done;
    }
    if (n == "window")
    {
        if (a.frame == 0)
        {
            int w = 0, h = 0;
            if (sscanf(pos(0).c_str(), "%dx%d", &w, &h) != 2 || w < 64 || h < 64)
                fail("window: <W>x<H>, e.g. 1280x720");
            a.scratch["w"] = w;
            a.scratch["h"] = h;
            glfwSetWindowSize(win, w, h);
            return Status::Pending;
        }
        const int w = a.scratch["w"], h = a.scratch["h"];
        if (ctx_ && (int)ctx_->swapExtent.width == w && (int)ctx_->swapExtent.height == h)
        {
            r["message"] = "window " + std::to_string(w) + "x" + std::to_string(h);
            return Status::Done;
        }
        if (a.frame > 120)
            fail("window: the swapchain did not reach the requested size (is it larger than the screen?)");
        return Status::Pending;
    }

    // ── camera paths ────────────────────────────────────────────────────────────
    if (n == "path")
    {
        const std::string sub = lower(pos(0));
        if (sub == "clear")
        {
            cineKeys().clear();
            r["message"] = "path cleared";
            return Status::Done;
        }
        if (sub == "key")
        {
            // Unspecified channels inherit from the previous key (the current view for the first).
            HarnessCamKey k;
            if (!cineKeys().empty())
                k = cineKeys().back();
            else
            {
                k.lat = obsLatDeg;
                k.lon = obsLonDeg;
                k.alt = obsHeightOffset;
                k.az = camera.azDeg;
                k.el = camera.elDeg;
                k.fov = camera.fovYDeg;
            }
            k.t = parseNum(pos(1), "path key <t>");
            if (!cineKeys().empty() && k.t <= cineKeys().back().t)
                fail("path key: times must increase");
            k.hasSim = false;
            if (c.has("lat"))
                k.lat = c.num("lat", 0.0);
            if (c.has("lon"))
            {
                // Unwrap against the previous key so a path crossing the antimeridian goes the short way.
                double lon = c.num("lon", 0.0);
                if (!cineKeys().empty())
                    while (lon - cineKeys().back().lon > 180.0)
                        lon -= 360.0;
                if (!cineKeys().empty())
                    while (lon - cineKeys().back().lon < -180.0)
                        lon += 360.0;
                k.lon = lon;
            }
            if (c.has("alt"))
                k.alt = c.num("alt", 0.0);
            if (c.has("az"))
            {
                double az = c.num("az", 0.0);
                const double prev = cineKeys().empty() ? camera.azDeg : cineKeys().back().az;
                while (az - prev > 180.0)
                    az -= 360.0;
                while (az - prev < -180.0)
                    az += 360.0;
                k.az = az;
            }
            if (c.has("el"))
                k.el = c.num("el", 0.0);
            if (c.has("fov"))
                k.fov = c.num("fov", 60.0);
            if (c.has("sim"))
            {
                k.hasSim = true;
                k.simT = j2000FromIso(c.str("sim"));
            }
            if (c.has("simadd"))
            {
                // Relative to the current sim time at the moment the key is added.
                k.hasSim = true;
                k.simT = (double)simDayJ2000 * 86400.0 + simSecInDay + c.num("simadd", 0.0);
            }
            cineKeys().push_back(k);
            r["keys"] = cineKeys().size();
            r["message"] = "key " + std::to_string(cineKeys().size()) + " at t=" + pos(1);
            return Status::Done;
        }
        if (sub == "goto")
        {
            const double t = parseNum(pos(1), "path goto <t>");
            cineKeys();
            if (cine_.shots[cineShot_].followSat >= 0)
            {
                // A follow shot (2026-10-03): its keys are in the satellite's frame, so it goes through the
                // cinematic's own evaluation (a plain camera key would end follow mode).
                const bool one = cinePlayOneShot_;
                cinePlayOneShot_ = true;
                cineApplyAt(t - cine_.shots[cineShot_].keys.front().t, true);
                cinePlayOneShot_ = one;
            }
            else
                harnessApplyCam(harnessEvalPath(t));
            r["message"] = "path at t=" + pos(1);
            return Status::Done;
        }
        if (sub == "play")
        {
            // Plays the path at a fixed frame rate: every frame is exactly 1/fps of path time and of
            // frame time (the sim's eased quantities see a real 1/fps step). With record=<name> each
            // frame is captured (captures/<name>_00000.png ...) before the next is shown, so a slow
            // encode never drops or doubles a frame; sim time then comes from the path (or, without
            // sim keys, the time scale) rather than from however many frames the encode waited.
            if (a.frame == 0)
            {
                if (cineKeys().size() < 2)
                    fail("path play: add at least two keys (path key <t> ...)");
                const double fps = c.num("fps", 30.0);
                if (fps < 1.0 || fps > 240.0)
                    fail("path play: fps in [1, 240]");
                a.scratch["fps"] = fps;
                a.scratch["i"] = 0;
                a.scratch["n"] = (int)std::floor((cineKeys().back().t - cineKeys().front().t) * fps + 1e-6) + 1;
                a.scratch["record"] = c.str("record");
                a.scratch["simStart"] = (double)simDayJ2000 * 86400.0 + simSecInDay;
                a.scratch["simRate"] = timePaused ? 0.0 : (double)kTimeScales[timeScaleIdx] * timeDir;
                a.scratch["pausedBefore"] = timePaused;
                a.scratch["captured"] = 0;
                a.scratch["cmSum"] = 0.0;
                a.scratch["totSum"] = 0.0;
                a.scratch["nT"] = 0;
                a.scratch["tots"] = json::array();   // review 22: per-frame GPU totals (spikes, not just the mean)
                harnessFixedDtOverride_ = (float)(1.0 / fps);
                timePaused = true; // the path owns the clock
                harnessTrack_ = 0;
            }
            const std::string rec = a.scratch["record"];
            // Wait for the last frame's COPY only, never its encode: every frame rendered while waiting runs
            // the temporal passes (clouds' history, sky TAA) at a still pose, so a recording took ~6 settle
            // frames between its frames and showed far less ghosting than a player sees (review 15). The
            // copy is read back at the start of the next frame (App::drawFrame, before buildUI); there
            // finalizeScreenshot joins the previous encode on the main thread, so the wait stalls the app
            // instead of rendering.
            if (!rec.empty() && (screenshotRequested || screenshotCopyPending))
                return Status::Pending;
            const int i = a.scratch["i"], nF = a.scratch["n"];
            if (i >= nF)
            {
                harnessFixedDtOverride_ = 0.0f;
                timePaused = a.scratch["pausedBefore"].get<bool>();
                r["frames"] = nF;
                // The GPU cost while moving (perf measures a still view): the mean over the frames after
                // the first three (the timings lag a frame; the first frames still run the still view's rate).
                if (a.scratch["nT"].get<int>() > 0)
                    r["gpu_ms_mean"] = {{"cloud_march", a.scratch["cmSum"].get<double>() / a.scratch["nT"].get<int>()},
                                        {"total", a.scratch["totSum"].get<double>() / a.scratch["nT"].get<int>()}};
                if (!a.scratch["tots"].empty())
                {
                    std::vector<double> t = a.scratch["tots"].get<std::vector<double>>();
                    std::sort(t.begin(), t.end());
                    r["gpu_total_ms_p90"] = t[std::min(t.size() - 1, (size_t)(0.9 * (double)t.size()))];
                    r["gpu_total_ms_max"] = t.back();
                    if (a.scratch.contains("worstFrame"))
                        r["gpu_worst_frame_ms"] = a.scratch["worstFrame"];
                }
                if (!rec.empty())
                    r["pattern"] = harnessRunner_->capturePath(rec + "_%05d", ".png");
                r["message"] = "played " + std::to_string(nF) + " frames" +
                               (rec.empty() ? std::string() : " -> captures/" + rec + "_#####.png");
                return Status::Done;
            }
            if (i >= 3)
            {
                a.scratch["cmSum"] = a.scratch["cmSum"].get<double>() + gpuMsRaw[3];
                a.scratch["totSum"] = a.scratch["totSum"].get<double>() + gpuMsRawTotal;
                a.scratch["tots"].push_back(gpuMsRawTotal);
                if (gpuMsRawTotal > a.scratch.value("worst", 0.0))
                {
                    static const char *kB[8] = {"scene_depth", "beam_cloud_block", "orbit_compute", "cloud_march",
                                                "flare_compute", "sky_background_draw", "satellite_star_draw", "ui_overlay"};
                    json w;
                    for (int b = 0; b < 8; ++b)
                        w[kB[b]] = gpuMsRaw[b];
                    w["frame"] = i;
                    a.scratch["worst"] = (double)gpuMsRawTotal;
                    a.scratch["worstFrame"] = w;
                }
                a.scratch["nT"] = a.scratch["nT"].get<int>() + 1;
            }
            const double fps = a.scratch["fps"];
            const double tp = cineKeys().front().t + i / fps;
            HarnessCamKey k = harnessEvalPath(tp);
            if (!k.hasSim)
            {
                k.hasSim = true;
                k.simT = a.scratch["simStart"].get<double>() + a.scratch["simRate"].get<double>() * (tp - cineKeys().front().t);
            }
            harnessApplyCam(k);
            obsTerrainH = cpuTerrainHeightM(obsLatDeg, obsLonDeg);
            updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay, 0.0f);
            if (!rec.empty())
            {
                char nm[64];
                snprintf(nm, sizeof(nm), "_%05d", i);
                screenshotPath = harnessRunner_->capturePath(safeName(rec) + nm, ".png");
                screenshotIncludeUI = c.flag("ui", false);
                screenshotScale = (float)c.num("scale", 1.0);
                screenshotRequested = true;
            }
            a.scratch["i"] = i + 1;
            return Status::Pending;
        }
        fail("path: clear | key <t> [lat= lon= alt= az= el= fov= sim=<iso>|simadd=<s>] | goto <t> | play [fps=30] [record=name] [ui=on] [scale=s]");
    }

    // ── overlays ────────────────────────────────────────────────────────────────
    if (n == "overlay")
    {
        const std::string sub = lower(pos(0));
        if (sub == "clear")
        {
            const std::string id = pos(1);
            harnessOverlays_.erase(std::remove_if(harnessOverlays_.begin(), harnessOverlays_.end(),
                                                  [&](const HarnessOverlay &o) { return id.empty() || o.id == id; }),
                                   harnessOverlays_.end());
            r["message"] = id.empty() ? "overlays cleared" : "overlay " + id + " cleared";
            return Status::Done;
        }
        if (sub != "text" && sub != "label")
            fail("overlay: text <id> \"<text>\" [x= y= size= align=left|center color=RRGGBB] | label <id> \"<text>\" target=<sel|sun|moon|planet> [size=] | clear [id]");
        HarnessOverlay o;
        o.id = pos(1);
        o.text = pos(2);
        if (o.id.empty())
            fail("overlay: an id is required");
        o.size = (float)c.num("size", sub == "text" ? 28.0 : 18.0);
        o.center = lower(c.str("align", "center")) != "left";
        if (c.has("color"))
        {
            unsigned rgb = (unsigned)strtoul(c.str("color").c_str(), nullptr, 16);
            o.color = glm::vec4((float)((rgb >> 16) & 255), (float)((rgb >> 8) & 255), (float)(rgb & 255), 255.0f);
        }
        if (sub == "text")
        {
            o.x = (float)c.num("x", 0.5);
            o.y = (float)c.num("y", 0.1);
        }
        else
        {
            const std::string tg = lower(c.str("target", "sel"));
            o.x = (float)c.num("dx", 14.0);
            o.y = (float)c.num("dy", -10.0);
            if (tg == "sel" || tg == "selection")
                o.target = 1;
            else if (tg == "sun")
                o.target = 2;
            else if (tg == "moon")
                o.target = 3;
            else
            {
                o.target = 4;
                for (int i = 0; i < kPlanetCount; ++i)
                    if (ieq(tg, kPlanetNames[i]))
                        o.planet = i;
                if (o.planet < 0)
                    fail("overlay label: target sel | sun | moon | <planet>");
            }
        }
        harnessOverlays_.erase(std::remove_if(harnessOverlays_.begin(), harnessOverlays_.end(),
                                              [&](const HarnessOverlay &e) { return e.id == o.id; }),
                               harnessOverlays_.end());
        harnessOverlays_.push_back(o);
        r["message"] = "overlay " + o.id;
        return Status::Done;
    }

    // ── probe ───────────────────────────────────────────────────────────────────
    if (n == "probe")
    {
        // What the terrain algorithm computes for one pixel's ray (terrain_probe.comp): the seed
        // from the shared depth, the seeded and unseeded march (distance, steps), the DEM/detail at
        // the hit, and a 64-sample height profile along the ray. Pixel in full-resolution
        // coordinates, top-left origin (the same pixels a capture's PNG has).
        if (a.frame == 0)
        {
            if (!probePipeline)
                fail("probe: the probe pipeline failed to build");
            probePx = glm::vec2((float)parseNum(pos(0), "probe x"), (float)parseNum(pos(1), "probe y"));
            probeRequested = true;
            return Status::Pending;
        }
        if (a.frame < 2)
            return Status::Pending;
        const glm::vec4 *v = static_cast<const glm::vec4 *>(probeMapped);
        auto v4 = [](const glm::vec4 &x) { return json::array({x.x, x.y, x.z, x.w}); };
        r["pixel"] = {probePx.x, probePx.y};
        r["obs_eff_h"] = v[0].x;
        r["t_exit"] = v[0].y;
        r["seed_depth"] = v[0].z >= 1e29f ? json("sky") : json(v[0].z);
        r["seed_sky"] = v[0].w > 0.5f;
        r["t_seed"] = v[6].y;
        r["march_gate"] = v[6].x > 0.5f;
        r["t_hit_seeded"] = v[1].x;
        r["steps_seeded"] = v[1].y;
        r["t_hit_from_eye"] = v[1].z;
        r["steps_from_eye"] = v[1].w;
        r["dir_enu"] = {v[2].x, v[2].y, v[2].z};
        r["pix_angle"] = v[2].w;
        r["t_base_sphere"] = {v[5].x, v[5].y};
        r["t_shell"] = {v[5].z, v[5].w};
        r["hit"] = {{"dem_h", v[3].x}, {"dem_mip3", v[3].y}, {"detail_h", v[3].z}, {"amp0", v[3].w},
                    {"rough", v[4].x}, {"ray_alt", v[4].y}, {"lat", v[4].z}, {"lon", v[4].w}};
        json prof = json::array();
        for (int i = 0; i < 64; ++i)
            prof.push_back(v4(v[8 + i]));
        r["profile_t_rayalt_dem_H"] = prof;
        char buf[200];
        snprintf(buf, sizeof(buf), "seed %s, hit %.0f m (%d steps) / from eye %.0f m (%d steps)",
                 v[0].w > 0.5f ? "SKY" : std::to_string((int)v[0].z).c_str(), v[1].x, (int)v[1].y, v[1].z, (int)v[1].w);
        r["message"] = buf;
        return Status::Done;
    }

    // ── lightning: the flashes in progress (the previous frame's list, cloud_lightning.glsl) ──
    if (n == "lightning")
    {
        // `lightning spawn kind=cg|ic|spider|sprite dist_km= az= [seed=] [base_m=1500] [top_m=10000] [ground_km=4]
        // [ground_az=<az + 90>] [dur=]`: a flash placed by hand (SatelliteSimLightning.cpp). base_m / top_m are above
        // the eye's ground.
        if (lower(pos(0)) == "spawn")
        {
            const std::string k = lower(c.str("kind").empty() ? std::string("cg") : c.str("kind"));
            const int kind = k == "ic" ? 0 : k == "sprite" ? 2 : k == "spider" ? 3 : 1;
            const double dist = c.num("dist_km", 10.0) * 1000.0, az = glm::radians(c.num("az", (double)camera.azDeg));
            const float gnd0 = terrainFrameMapped ? terrainFrameMapped[1] : obsTerrainH;
            const float base = gnd0 + (float)c.num("base_m", 1500.0), top = gnd0 + (float)c.num("top_m", 10000.0);
            const double gOff = c.num("ground_km", 4.0) * 1000.0;
            const double gAz = glm::radians(c.num("ground_az", glm::degrees(az) + 90.0));
            const float ground = terrainFrameMapped ? terrainFrameMapped[1] : obsTerrainH;
            const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
            const glm::dvec3 E = glm::normalize(glm::cross(glm::dvec3(0.0, 0.0, 1.0), up)), N = glm::cross(up, E);
            auto at = [&](double e, double nn, double h) {
                return glm::normalize(up * 6371000.0 + E * e + N * nn) * (6371000.0 + h);
            };
            const double oe = dist * std::sin(az), on = dist * std::cos(az);
            const double oAlt = kind == 2 ? 75000.0 : kind == 3 ? base - 150.0
                              : kind == 0 ? base + 0.5 * (top - base) : base + 0.35 * (top - base);
            InjectedFlash q;
            q.id = 0xF0000000u | (++boltInjectSerial_);
            q.seed = (uint32_t)c.num("seed", (double)(boltInjectSerial_ * 2654435761u % 100000u));
            q.kind = kind;
            q.origin = at(oe, on, oAlt);
            q.ground = kind == 1 ? at(oe + gOff * std::sin(gAz), on + gOff * std::cos(gAz), ground) : q.origin;
            q.base = base;
            q.top = top;
            q.strength = 0.8f;
            q.dur = (float)c.num("dur", kind == 1 ? 0.6 : 0.8);
            q.startS = (double)simDayJ2000 * 86400.0 + simSecInDay;
            boltInjected_.push_back(q);
            r["id"] = q.id;
            r["message"] = "spawned kind " + std::to_string(kind) + " at " + std::to_string(dist / 1000.0) + " km";
            return Status::Done;
        }
        json list = json::array();
        uint32_t cnt = 0;
        if (cv2FlashMapped)
        {
            const uint32_t *hdr = (const uint32_t *)cv2FlashMapped;
            const float *f = (const float *)((const char *)cv2FlashMapped + 16);
            cnt = std::min(hdr[0], kCv2FlashMax);
            for (uint32_t i = 0; i < cnt; ++i)
            {
                const float *e = f + i * 16;
                uint32_t id;
                std::memcpy(&id, &e[8], 4);
                const int kind = (int)(e[7] + 0.5f);
                static const char *kKinds[4] = {"in_cloud", "cloud_to_ground", "sprite", "spider"};
                auto tr = boltTrees_.find(id);
                list.push_back({{"id", id}, {"intensity", e[3]}, {"kind", kKinds[std::clamp(kind, 0, 3)]},
                                {"cloud_to_ground", kind == 1}, {"sprite", kind == 2},
                                {"age_s", e[10]}, {"dist_km", e[11] / 1000.0f}, {"base_m", e[12]}, {"top_m", e[13]},
                                {"strength", e[14]}, {"segments", tr == boltTrees_.end() ? 0 : (int)tr->second.segs.size()},
                                {"enu_m", {e[0], e[1], e[2] - (float)(6371000.0)}}});
            }
        }
        r["count"] = cnt;
        r["flashes"] = list;
        {
            // The sound sources (thunderStep): each flash's shell against the listener now.
            const double nowS = (double)simDayJ2000 * 86400.0 + simSecInDay;
            const glm::dvec3 eye = thunderPrevValid_ ? thunderPrevEye_ : glm::dvec3(0.0);
            json src = json::array();
            size_t pending = 0;
            for (const ThunderSource &s : thunderSources_)
            {
                pending += s.heard ? 0 : 1;
                const double d = glm::length(s.origin - eye);
                src.push_back({{"id", s.id}, {"age_s", nowS - s.t0}, {"dist_km", d / 1000.0}, {"extent_km", s.extentM / 1000.0f},
                               {"arrives_in_s", (d - s.extentM) / 343.0 - (nowS - s.t0)}, {"pieces", s.pts.size()}, {"heard", s.heard}});
            }
            r["thunder_sources"] = src;
            r["thunder_pending"] = pending;
            r["thunder_level"] = thunderLevelNow_;
        }
        r["thunder_rolls"] = thunderRolls_;
        json trees = json::array();
        for (const auto &kv : boltTrees_)
        {
            const BoltTree &t = kv.second;
            int ord[4] = {0, 0, 0, 0};
            for (const BoltSegC &sg : t.segs)
                ++ord[(sg.packed >> 7) & 3u];
            trees.push_back({{"id", t.id}, {"kind", t.kind}, {"ground_local_m", {t.groundLocal.x, t.groundLocal.y, t.groundLocal.z}},
                             {"base_local_z_m", t.baseLocalZ}, {"segments_by_order", {ord[0], ord[1], ord[2], ord[3]}},
                             {"main_arc_m", t.mainArc}, {"max_arc_m", t.maxArc}});
        }
        r["trees"] = trees;
        r["drawn_flashes"] = boltFlashCount;
        r["drawn_segments"] = boltSegCount;
        r["message"] = std::to_string(cnt) + " flash(es) in progress";
        return Status::Done;
    }

    // ── shader hot reload (clouds v2 march + resolve) ───────────────────────────
    if (n == "shaders")
    {
        if (lower(pos(0)) != "reload" || !ctx_)
            fail("shaders reload [march=<spv path>]");
        std::string stats;
        uint32_t wgX = 0, wgY = 0;
        const std::string wg = c.str("wg");
        if (!wg.empty() && sscanf(wg.c_str(), "%ux%u", &wgX, &wgY) != 2)
            fail("shaders reload: wg=<X>x<Y>");
        ctx_->capturePipelineStats = true;   // the statistics below need it; startup does not ask for it
        const std::string err = reloadCloudsV2Shaders(*ctx_, c.str("march"), stats, wgX, wgY, c.str("resolve"), c.str("lightning"),
                                                        c.str("composite"), c.str("layers"));
        ctx_->capturePipelineStats = false;
        if (!err.empty())
            fail("shaders reload: " + err);
        r["stats"] = stats;
        r["message"] = "reloaded cloud_v2_march (wg " + std::to_string(cv2MarchWg[0]) + "x" + std::to_string(cv2MarchWg[1])
                     + ") / cloud_v2_resolve / cloud_v2_lightning / cloud_march" + (stats.empty() ? std::string() : ": " + stats);
        return Status::Done;
    }

    // ── debug views ─────────────────────────────────────────────────────────────
    if (n == "debugview")
    {
        // Terrain debug views (sat_sky.frag, cloud.terrainDebugView). Not persisted.
        static const char *kViews[] = {"off", "normals", "detail", "steps", "albedo", "shadow", "rough", "elevzebra", "distzebra",
                                       "erosion",
                                       // 10+: the terrain LIGHTING term by term (linear radiance x100, see sat_sky.frag)
                                       "terms", "direct", "skyamb", "night", "moon", "aurora", "gates", "factors",
                                       "skyambraw", "suntint", "aofactors", "day", "nightmap", "geodot", "sunvis",
                                       // terrain v2: the night sky + moonlit sky on the albedo; the night map with its base removed
                                       "nightsky", "citylights",
                                       // solar parks (R panel coverage, G graded ground, B panel normal east)
                                       "solar",
                                       // launch pads (R the pad ground's cover, G the city exclusion, B lamps + floodlit share)
                                       "pads"};
        const int viewCount = (int)(sizeof(kViews) / sizeof(kViews[0]));
        const std::string v = lower(pos(0) == "terrain" ? pos(1) : pos(0));
        int idx = -1;
        for (int i = 0; i < viewCount; ++i)
            if (v == kViews[i])
                idx = i;
        // Ocean views (sat_sky.frag, 2026-09-30) sit at 40..45.
        static const char *kOceanViews[] = {"oceanrefl", "oceanfresnel", "oceanstate", "oceansurf", "oceannormal", "oceanshore",
                                            "cloudairsplit", "cloudtrans", "cloudrad", "cloudalpha", "oceanshadow"};
        for (int i = 0; i < 11; ++i)
            if (v == kOceanViews[i])
                idx = 40 + i;
        if (idx < 0 && !v.empty() && isdigit((unsigned char)v[0]))
            idx = (int)parseNum(v, "debugview");
        if (idx < 0 || (idx >= viewCount && (idx < 40 || idx > 50)))
            fail("debugview: off | normals | detail | steps | albedo | shadow | rough | elevzebra | distzebra | erosion | "
                 "terms | direct | skyamb | night | moon | aurora | gates | factors | skyambraw | suntint | aofactors | "
                 "day | nightmap | geodot | sunvis | nightsky | citylights | solar "
                 "(steps: blue = few march steps .. red = the budget; rough: R roughness, G rock, B snow; "
                 "zebras: stripes every 25 m of elevation / 100 m of distance; terms/direct/skyamb/night/moon/aurora: "
                 "the terrain light term by term as linear radiance x100, so a capture pixel reads the number; "
                 "sunvis: R = the Sun's disc clears this point's own horizon (0 = no direct sun reaches it), "
                 "G = its margin over that horizon, B = the horizon dip at this altitude)");
        terrainDebugView = idx;
        r["message"] = std::string("debug view ") + (idx < viewCount ? kViews[idx] : kOceanViews[idx - 40]);
        return Status::Done;
    }

    // ── photo: the HQ photo (requestPhoto), saved as a capture ──────────────────
    // ── cine: the cinematic (review 17, Cinematic.h; `path key` edits the current shot's keys) ──────────
    if (n == "cine")
    {
        const std::string sub = lower(pos(0));
        auto shotJson = [&]()
        {
            json js = json::array();
            for (int i = 0; i < (int)cine_.shots.size(); ++i)
                js.push_back({{"index", i}, {"name", cine_.shots[i].name}, {"keys", cine_.shots[i].keys.size()},
                              {"duration_s", cine_.shots[i].duration()}, {"sim_rate", cine_.shots[i].simRate},
                              {"has_look", !cine_.shots[i].settings.is_null()}});
            return js;
        };
        if (sub == "new")
        {
            cineStop("new");
            cine_ = Cinematic{};
            if (!pos(1).empty())
                cine_.name = safeName(pos(1));
            cineShot_ = 0;
            cineKeys();
            r["message"] = "new cinematic " + cine_.name;
            return Status::Done;
        }
        if (sub == "name")
        {
            cine_.name = safeName(pos(1));
            r["message"] = "named " + cine_.name;
            return Status::Done;
        }
        if (sub == "shot")
        {
            const std::string what = lower(pos(1));
            if (what == "add")
            {
                CineShot ns;
                ns.name = pos(2).empty() ? "shot " + std::to_string(cine_.shots.size() + 1) : pos(2);
                ns.simStartValid = true;
                ns.simStart = (double)simDayJ2000 * 86400.0 + simSecInDay;
                cine_.shots.push_back(ns);
                cineShot_ = (int)cine_.shots.size() - 1;
            }
            else if (what == "del")
            {
                if (!cine_.shots.empty())
                    cine_.shots.erase(cine_.shots.begin() + std::clamp(cineShot_, 0, (int)cine_.shots.size() - 1));
                cineShot_ = std::max(0, cineShot_ - 1);
                cineKeys();
            }
            else if (what == "look")
            {
                cineKeys();
                cine_.shots[cineShot_].settings = lower(pos(2)) == "clear" ? json() : cineLookSettings();
                cine_.shots[cineShot_].hasDrift = lower(pos(2)) != "clear";
                cine_.shots[cineShot_].driftOffset = cloudDriftPhaseOffset;
                cine_.shots[cineShot_].driftRate = cloudDriftRate;
            }
            else if (what == "simrate")
            {
                cineKeys();
                cine_.shots[cineShot_].simRate = parseNum(pos(2), "cine shot simrate");
            }
            else if (what == "follow")
            {
                // The current shot rides with a satellite: `sel` = the selection, `off`, or a roster index.
                cineKeys();
                const std::string v = lower(pos(2));
                cine_.shots[cineShot_].followSat = v == "off" ? -1 : v == "sel" ? selectedSatIndex : (int)parseNum(v, "cine shot follow");
                if (cine_.shots[cineShot_].followSat >= 0)
                    startFollow(cine_.shots[cineShot_].followSat);
                if (cine_.shots[cineShot_].followSat >= 0 && !followActive)
                    fail("cine shot follow: that satellite has no geometry model");
            }
            else if (what == "ease")
            {
                cineKeys();
                cine_.shots[cineShot_].ease = std::clamp(parseNum(pos(2), "cine shot ease"), 0.0, 1.0);
            }
            else if (what == "simnow")
            {
                cineKeys();
                cine_.shots[cineShot_].simStartValid = true;
                cine_.shots[cineShot_].simStart = (double)simDayJ2000 * 86400.0 + simSecInDay;
            }
            else if (!what.empty() && isdigit((unsigned char)what[0]))
                cineShot_ = std::clamp((int)parseNum(what, "cine shot") - 1, 0, std::max(0, (int)cine_.shots.size() - 1));
            else
                fail("cine shot: add [name] | del | <n> (1-based) | look [clear] | simrate <x> | simnow | follow <sel|off|index> | ease <0-1>");
            cineKeys();
            r["shot"] = cineShot_ + 1;
            r["shots"] = shotJson();
            r["message"] = "shot " + std::to_string(cineShot_ + 1) + " of " + std::to_string(cine_.shots.size());
            return Status::Done;
        }
        if (sub == "key")
        {
            // The current view as a key (`path key` sets channels explicitly): at t= or 2 s after the last.
            std::vector<CineKey> &K = cineKeys();
            CineKey k = cineCurrentPose();
            k.t = c.has("t") ? c.num("t", 0.0) : (K.empty() ? 0.0 : K.back().t + 2.0);
            if (!K.empty() && k.t <= K.back().t)
                fail("cine key: times must increase");
            if (K.empty() && !cine_.shots[cineShot_].simStartValid)
            {
                cine_.shots[cineShot_].simStartValid = true;
                cine_.shots[cineShot_].simStart = k.simT;
            }
            K.push_back(k);
            r["keys"] = K.size();
            r["message"] = "key " + std::to_string(K.size()) + " at t=" + std::to_string(k.t);
            return Status::Done;
        }
        if (sub == "save" || sub == "load")
        {
            std::string err;
            if (sub == "save" ? !cineSave(pos(1), err) : !cineLoad(pos(1), err))
                fail("cine " + sub + ": " + err);
            r["shots"] = shotJson();
            r["message"] = cineStatus_;
            return Status::Done;
        }
        if (sub == "state")
        {
            r["name"] = cine_.name;
            r["shot"] = cineShot_ + 1;
            r["shots"] = shotJson();
            r["duration_s"] = cine_.duration();
            r["json"] = cineToJson(cine_);
            r["message"] = cine_.name + ": " + std::to_string(cine_.shots.size()) + " shots, " + std::to_string(cine_.duration()) + " s";
            return Status::Done;
        }
        if (sub == "play" || sub == "export")
        {
            // Pending until the playback / export ends. play [shot]; export preview|hq [fps=] [scale=] [frames=] [shot].
            if (a.frame == 0)
            {
                const std::string mode = lower(pos(1));
                const bool oneShot = c.flag("shot", false) || mode == "shot" || lower(pos(2)) == "shot";
                if (c.has("fps"))
                    cineExportFps_ = (float)std::clamp(c.num("fps", 30.0), 1.0, 240.0);
                if (c.has("scale"))
                    photoScaleSetting = std::clamp((float)c.num("scale", 2.0), 1.0f, 4.0f);
                if (c.has("frames"))
                    photoSettleFrames = std::clamp((float)c.num("frames", 48.0), 4.0f, 240.0f);
                if (c.has("blur"))
                    cineBlurSubs_ = (float)std::clamp(c.num("blur", 1.0), 1.0, 32.0);
                if (sub == "play")
                    cineStart(CineRun::Play, oneShot);
                else if (mode == "hq")
                    cineStart(CineRun::ExportHQ, oneShot);
                else if (mode == "preview")
                    cineStart(CineRun::ExportPreview, oneShot);
                else
                    fail("cine export: preview | hq [fps=] [scale=] [frames=] [blur=] [shot]");
                if (!cineActive())
                    fail("cine " + sub + ": " + cineStatus_);
                a.scratch["t0"] = harness::nowS();
                return Status::Pending;
            }
            if (cineActive() || screenshotRequested || screenshotCopyPending || screenshotEncoding.load())
                return Status::Pending;
            r["frames"] = cineFrame_;
            r["dir"] = cineOutDir_;
            r["wall_s"] = harness::nowS() - a.scratch["t0"].get<double>();
            r["message"] = cineStatus_;
            return Status::Done;
        }
        if (sub == "stop")
        {
            cineStop("Stopped");
            r["message"] = "stopped";
            return Status::Done;
        }
        fail("cine: new [name] | name <n> | shot add|del|<n>|look [clear]|simrate <x>|simnow | key [t=] | play [shot] | "
             "export preview|hq [fps=] [scale=] [frames=] [blur=] [shot] | save [file] | load <file> | state | stop");
    }

    if (n == "photo")
    {
        const bool busy = photoState != 0 || screenshotRequested || screenshotCopyPending || screenshotEncoding.load();
        if (!a.scratch.contains("requested"))
        {
            if (busy)
                return Status::Pending;
            const std::string name = safeName(pos(0));
            if (c.has("scale"))
                photoScaleSetting = std::clamp((float)c.num("scale", 2.0), 1.0f, 4.0f);
            if (c.has("frames"))
                photoSettleFrames = std::clamp((float)c.num("frames", 48.0), 4.0f, 240.0f);
            photoPathOverride = harnessRunner_->capturePath(name, ".png");
            requestPhoto();
            if (photoState == 0)
                fail("photo: could not start (screenshots unsupported?)");
            a.scratch["requested"] = true;
            a.scratch["name"] = name;
            return Status::Pending;
        }
        if (busy)
            return Status::Pending;
        const std::string png = harnessRunner_->capturePath(a.scratch["name"], ".png");
        uint32_t w = 0, h = 0;
        if (!fs::exists(png) || !pngSize(png, w, h))
            fail("photo: no image was written");
        r["path"] = png;
        r["width"] = w;
        r["height"] = h;
        return Status::Done;
    }

    // ── capture / state ─────────────────────────────────────────────────────────
    if (n == "capture" || n == "screenshot")
    {
        const bool busy = screenshotRequested || screenshotCopyPending || screenshotEncoding.load();
        if (!a.scratch.contains("requested"))
        {
            if (busy)
                return Status::Pending; // a previous capture is still encoding
            if (ctx_ && !ctx_->screenshotSupported)
                fail("capture: this GPU/driver can't copy from the swapchain (no TRANSFER_SRC)");
            const std::string name = safeName(pos(0));
            screenshotPath = harnessRunner_->capturePath(name, ".png");
            screenshotIncludeUI = c.flag("ui", false);
            screenshotScale = (float)c.num("scale", 1.0);
            if (screenshotScale <= 0.0f || screenshotScale > 16.0f)
                fail("capture: scale must be in (0, 16]");
            if (c.has("crop"))
            {
                int x, y, w, h;
                if (sscanf(c.str("crop").c_str(), "%d,%d,%d,%d", &x, &y, &w, &h) != 4 || w <= 0 || h <= 0)
                    fail("capture: crop=x,y,w,h in frame pixels");
                screenshotCrop[0] = x;
                screenshotCrop[1] = y;
                screenshotCrop[2] = w;
                screenshotCrop[3] = h;
            }
            a.scratch["requested"] = true;
            a.scratch["name"] = name;
            a.scratch["state"] = harnessStateJson();
            screenshotRequested = true;
            return Status::Pending;
        }
        if (busy)
            return Status::Pending;
        const std::string name = a.scratch["name"];
        const std::string png = harnessRunner_->capturePath(name, ".png");
        uint32_t w = 0, h = 0;
        if (!fs::exists(png) || !pngSize(png, w, h))
            fail("capture: no image was written (see the log)");
        json side = {{"name", name},
                     {"png", png},
                     {"width", w},
                     {"height", h},
                     {"ui", c.flag("ui", false)},
                     {"crop", c.str("crop")},
                     {"scale", c.num("scale", 1.0)},
                     {"command", c.text},
                     {"state", a.scratch["state"]}};
        const std::string sidecar = harnessRunner_->capturePath(name, ".json");
        std::ofstream(sidecar) << side.dump(2) << '\n';
        r["png"] = png;
        r["sidecar"] = sidecar;
        r["width"] = w;
        r["height"] = h;
        r["message"] = name + ".png " + std::to_string(w) + "x" + std::to_string(h);
        return Status::Done;
    }
    if (n == "state")
    {
        r = harnessStateJson();
        if (!c.pos.empty())
        {
            const std::string path = harnessRunner_->capturePath(safeName(pos(0)), ".state.json");
            std::ofstream(path) << r.dump(2) << '\n';
            r["file"] = path;
        }
        r["message"] = r["time"]["utc"].get<std::string>() + "  " + std::to_string(obsLatDeg) + ", " +
                       std::to_string(obsLonDeg);
        return Status::Done;
    }

    // ── audio ───────────────────────────────────────────────────────────────────
    // `audio [state [name]]` — the ambience context and layer gains. `audio record <name> [seconds=8]
    // [bus=ambience|music|sfx|all|music+ambience] [solo=<layer>]` — renders the mix offline into
    // captures/<name>.wav (+ <name>.json: the state and the levels). `audio force <layer> <gain>|off`
    // pins a layer's gain (calibrating one voice anywhere); `audio force off` releases them all.
    if (n == "audio")
    {
        const std::string sub = lower(pos(0));
        if (!ambience_)
            updateAmbience(0.0f); // loads the table (it otherwise loads on the first recordCompute)
        if (!ambience_ || !ambience_->loaded())
        {
            a.error = "ambience is not loaded (assets/sound/ambience/ambience.json - see satlight_log.txt)";
            return Status::Error;
        }
        if (sub.empty() || sub == "state")
        {
            r = ambience_->stateJson();
            r["tonality"] = tonalityJson();
            if (!pos(1).empty())
            {
                const std::string path = harnessRunner_->capturePath(safeName(pos(1)), ".audio.json");
                std::ofstream(path) << r.dump(2) << '\n';
                r["file"] = path;
            }
            std::string msg;
            for (const auto &id : r["audible"])
                msg += (msg.empty() ? "" : ", ") + id.get<std::string>();
            r["message"] = msg.empty() ? "(silence)" : msg;
            return Status::Done;
        }
        // `audio expect a,b [absent=c,d] [min=0.05]`: fails unless every listed layer is at least
        // `min` gain and every `absent` one below it — a location tour checks itself.
        if (sub == "expect")
        {
            const json st = ambience_->stateJson();
            const float minGain = (float)c.num("min", 0.05);
            auto gainOf = [&](const std::string &id) -> float
            {
                for (const auto &l : st["layers"])
                    if (l["id"] == id)
                        return l["gain"].get<float>();
                return -1.0f;
            };
            auto split = [](const std::string &list)
            {
                std::vector<std::string> out;
                size_t b = 0;
                while (b <= list.size())
                {
                    const size_t e = list.find(',', b);
                    const std::string t = list.substr(b, e == std::string::npos ? std::string::npos : e - b);
                    if (!t.empty())
                        out.push_back(t);
                    if (e == std::string::npos)
                        break;
                    b = e + 1;
                }
                return out;
            };
            std::string fails;
            for (const std::string &id : split(pos(1)))
            {
                const float g = gainOf(id);
                if (g < 0.0f)
                    fails += " no layer '" + id + "';";
                else if (g < minGain)
                    fails += " " + id + " is " + std::to_string(g) + ";";
            }
            for (const std::string &id : split(c.str("absent")))
            {
                const float g = gainOf(id);
                if (g < 0.0f)
                    fails += " no layer '" + id + "';";
                else if (g >= minGain)
                    fails += " " + id + " should be silent but is " + std::to_string(g) + ";";
            }
            r["audible"] = st["audible"];
            r["drivers"] = st["drivers"];
            if (!fails.empty())
            {
                std::string audible;
                for (const auto &id : st["audible"])
                    audible += (audible.empty() ? "" : ", ") + id.get<std::string>();
                a.error = "ambience expectation failed:" + fails + " (audible: " + audible + ")";
                return Status::Error;
            }
            std::string msg;
            for (const auto &id : st["audible"])
                msg += (msg.empty() ? "" : ", ") + id.get<std::string>();
            r["message"] = "ok: " + msg;
            return Status::Done;
        }
        if (sub == "music")
        {
            if (!audio_)
            {
                a.error = "no audio system";
                return Status::Error;
            }
            const std::string op = lower(pos(1));
            if (op == "next")
                audio_->nextTrack();
            else if (op == "prev")
                audio_->prevTrack();
            else if (op == "pause")
                audio_->setMusicPaused(true);
            else if (op == "play")
                audio_->setMusicPaused(false);
            else if (op == "end")
                audio_->endTrack(); // offline a track never ends by itself: this reaches the gap
            else if (!op.empty() && op != "state")
            {
                a.error = "audio music [next|prev|pause|play|end|state]";
                return Status::Error;
            }
            r = {{"track", audio_->trackIndex()},       {"name", audio_->trackName(audio_->trackIndex())},
                 {"tracks", audio_->trackCount()},      {"paused", audio_->musicPaused()},
                 {"gap_remaining_s", audio_->gapRemaining()}, {"gap_s", audio_->musicGap()},
                 {"altitude_fade", audio_->musicFade()}, {"has_upwell", audio_->trackHasUpwell()},
                 {"upwell_gain", audio_->upwellGain()}};
            r["message"] = audio_->trackName(audio_->trackIndex()) + (audio_->musicPaused() ? " (paused)" : "") +
                           " - track " + std::to_string(audio_->trackIndex() + 1) + "/" +
                           std::to_string(audio_->trackCount());
            return Status::Done;
        }
        // `audio tonality [wait]`: the key the ambience is in (the playing track's, from its analysis)
        // — root, source, key, chord, scale. `wait` holds the script until every track is analysed.
        if (sub == "tonality")
        {
            if (lower(pos(1)) == "wait" && musicLib_ && musicLib_->pending() > 0)
            {
                if (harness::nowS() - a.startWall > 60.0)
                {
                    a.error = "music analysis still pending after 60 s";
                    return Status::Error;
                }
                return Status::Pending;
            }
            r = tonalityJson();
            r["message"] = tonalityLine();
            return Status::Done;
        }
        if (sub == "force")
        {
            const std::string id = pos(1);
            if (id == "off" || id.empty())
            {
                ambience_->releaseForced();
                r["message"] = "released all forced layers";
                return Status::Done;
            }
            const std::string v = lower(pos(2));
            const float g = (v == "off" || v.empty()) ? -1.0f : (float)parseNum(v, "audio force");
            if (!ambience_->force(id, g))
            {
                a.error = "no ambience layer '" + id + "'";
                return Status::Error;
            }
            r["message"] = id + (g < 0.0f ? " released" : " forced to " + v);
            return Status::Done;
        }
        if (sub == "record")
        {
            if (!audio_)
            {
                a.error = "no audio system";
                return Status::Error;
            }
            const std::string name = safeName(pos(1).empty() ? "audio" : pos(1));
            const double seconds = c.num("seconds", 8.0);
            uint32_t mask = 0;
            const std::string bus = lower(c.str("bus", "ambience"));
            if (bus == "all")
                mask = AudioSystem::BusAll;
            if (bus.find("music") != std::string::npos)
                mask |= AudioSystem::BusMusic;
            if (bus.find("sfx") != std::string::npos)
                mask |= AudioSystem::BusSfx;
            if (bus.find("ambience") != std::string::npos)
                mask |= AudioSystem::BusAmbience;
            if (mask == 0)
            {
                a.error = "bus= must name music, sfx, ambience (joined by +) or all";
                return Status::Error;
            }
            int solo = -1;
            if (c.has("solo"))
            {
                const int li = ambience_->layerIndex(c.str("solo"));
                if (li < 0)
                {
                    a.error = "no ambience layer '" + c.str("solo") + "'";
                    return Status::Error;
                }
                solo = ambience_->layerVoice(li);
                if (solo < 0)
                {
                    a.error = "layer '" + c.str("solo") + "' has no voice here (silent, events-only, or failed); "
                              "`audio force " + c.str("solo") + " 1` first";
                    return Status::Error;
                }
            }
            const std::string path = harnessRunner_->capturePath(name, ".wav");
            AudioSystem::RenderStats st;
            std::string err;
            // Thunder is streamed per frame from the sound shells: the render's own clock carries them on (sim time
            // itself stands still during a render), so a flash spawned before the record is heard arriving in it.
            thunderRecording_ = false;
            const bool ok = audio_->renderWav(path, seconds, mask, solo,
                                              [this](float dt) { ambience_->tickEvents(dt, audio_); thunderRecordTick(dt); }, st, err);
            thunderRecording_ = false;
            thunderPrevValid_ = false;   // the next real frame starts from its own time: no reversed replay
            if (!ok)
            {
                a.error = err;
                return Status::Error;
            }
            r["file"] = path;
            r["seconds"] = st.seconds;
            r["rms_db"] = st.rmsDb;
            r["peak_db"] = st.peakDb;
            r["rms_db_per_second"] = st.rmsDbPerSecond;
            r["bus"] = bus;
            if (c.has("solo"))
                r["solo"] = c.str("solo");
            r["audible"] = ambience_->stateJson()["audible"];
            json side = harnessStateJson();
            side["audio_record"] = r;
            std::ofstream(harnessRunner_->capturePath(name, ".json")) << side.dump(2) << '\n';
            char buf[512];
            snprintf(buf, sizeof(buf), "%s: %.1f s, RMS %.1f dBFS, peak %.1f dBFS", path.c_str(), st.seconds, st.rmsDb,
                     st.peakDb);
            r["message"] = buf;
            return Status::Done;
        }
        a.error = "audio [state [name]] | record <name> [seconds=] [bus=] [solo=] | expect <layers> [absent=<layers>] "
                  "[min=] | force <layer> <gain|off> | force off | music [...] | tonality [wait]";
        return Status::Error;
    }

    // ── perf ────────────────────────────────────────────────────────────────────
    if (n == "perf")
    {
        static const char *kBucketKeys[8] = {"scene_depth", "beam_cloud_block", "orbit_compute", "cloud_march",
                                             "flare_compute", "sky_background_draw", "satellite_star_draw", "ui_overlay"};
        static const char *kCpuKeys[CPU_COUNT] = {"build_ui", "update_positions", "beam_readback", "update_stars",
                                                  "light_pollution_dome", "update_planets", "orbit_rebake", "kessler", "launches"};
        constexpr int kWarmup = 3; // gpuMsRaw lags a frame; skip the frames around the command itself
        const int frames = (int)c.num("frames", pos(0).empty() ? 60.0 : parseNum(pos(0), "perf"));
        if (a.frame == 0)
        {
            if (!ctx_ || ctx_->timestampPeriodNs <= 0.0)
                fail("perf: GPU timestamp queries are not supported on this device");
            a.scratch["gpu"] = std::vector<double>(8, 0.0);
            a.scratch["cpu"] = std::vector<double>(CPU_COUNT, 0.0);
            a.scratch["totals"] = json::array();
            a.scratch["walls"] = json::array();
            a.scratch["last"] = harness::nowS();
            return Status::Pending;
        }
        const double now = harness::nowS();
        const double wall = (now - a.scratch["last"].get<double>()) * 1000.0;
        a.scratch["last"] = now;
        if (a.frame > kWarmup)
        {
            for (int b = 0; b < 8; ++b)
                a.scratch["gpu"][b] = a.scratch["gpu"][b].get<double>() + gpuMsRaw[b];
            for (int k = 0; k < CPU_COUNT; ++k)
                a.scratch["cpu"][k] = a.scratch["cpu"][k].get<double>() + cpuMsRaw[k];
            a.scratch["totals"].push_back(gpuMsRawTotal);
            a.scratch["walls"].push_back(wall);
        }
        if (a.frame < kWarmup + frames)
            return Status::Pending;
        std::vector<double> tot = a.scratch["totals"].get<std::vector<double>>();
        std::vector<double> walls = a.scratch["walls"].get<std::vector<double>>();
        auto stats = [](std::vector<double> v)
        {
            std::sort(v.begin(), v.end());
            double sum = 0;
            for (double x : v)
                sum += x;
            auto q = [&](double p) { return v[std::min(v.size() - 1, (size_t)(p * (v.size() - 1) + 0.5))]; };
            return json{{"mean", sum / v.size()}, {"min", v.front()}, {"p50", q(0.5)}, {"p90", q(0.9)}, {"max", v.back()}};
        };
        json g;
        for (int b = 0; b < 8; ++b)
            g[kBucketKeys[b]] = a.scratch["gpu"][b].get<double>() / frames;
        json cpu;
        for (int k = 0; k < CPU_COUNT; ++k)
            cpu[kCpuKeys[k]] = a.scratch["cpu"][k].get<double>() / frames;
        r["frames"] = frames;
        r["gpu_ms"] = g;
        r["gpu_total_ms"] = stats(tot);
        r["cpu_ms"] = cpu;
        r["wall_frame_ms"] = stats(walls);
        r["fixed_dt"] = harness::options().fixedDt;
        r["state"] = harnessStateJson();
        r["state"].erase("gpu_ms_last_frame");
        if (c.has("name"))
        {
            json rec = r;
            rec["record_kind"] = "harness_sample";
            rec["name"] = c.str("name");
            appendPerfRecord(rec);
        }
        char buf[128];
        snprintf(buf, sizeof(buf), "GPU %.2f ms (p90 %.2f), frame %.2f ms over %d frames", r["gpu_total_ms"]["mean"].get<double>(),
                 r["gpu_total_ms"]["p90"].get<double>(), r["wall_frame_ms"]["mean"].get<double>(), frames);
        r["message"] = buf;
        return Status::Done;
    }
    if (n == "sweep")
    {
        if (a.frame == 0)
        {
            if (sweepActive)
                fail("sweep: a sweep is already running");
            a.scratch["before"] = sweepsCompleted;
            startKnockoutSweep();
            if (!sweepActive)
                fail("sweep: unavailable (no GPU timestamps?)");
            return Status::Pending;
        }
        if (sweepsCompleted == a.scratch["before"].get<int>())
            return Status::Pending;
        r = json::parse(lastSweepRecordJson);
        r["message"] = "sweep: " + std::to_string(r["knockout_sweep"]["steps"].size()) + " steps, baseline " +
                       std::to_string(r["knockout_sweep"]["baseline"]["total"].get<double>()) + " ms";
        return Status::Done;
    }

    fail("unknown command '" + n + "' (try `help`)");
}

// ─── The ~ console ────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::ensureConsoleRunner()
{
    if (harnessRunner_)
        return;
    // A normal session: a runner of its own, in real time, never exiting on its own.
    harness::Options &o = harness::options();
    o.exitWhenDone = false;
    o.fixedDt = 0.0f;
    char stamp[32];
    std::time_t tt = std::time(nullptr);
    std::strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", std::localtime(&tt));
    o.outDir = (fs::path(exeDir_) / "harness_runs" / (std::string("console_") + stamp)).string();
    harnessRunner_ = new harness::Runner();
    std::string err;
    if (!harnessRunner_->begin(err))
        harnessRunner_->consoleEcho("  ERROR: " + err);
    else
        harnessRunner_->consoleEcho("run folder: " + harnessRunner_->runDir());
}

bool SatelliteSim::consoleKey(int key, int action)
{
    if (!consoleOpen_)
    {
        if (key == GLFW_KEY_GRAVE_ACCENT && action == GLFW_PRESS && !showIntro)
        {
            consoleOpen_ = true;
            ensureConsoleRunner();
            return true;
        }
        return false;
    }
    if (action != GLFW_PRESS && action != GLFW_REPEAT)
        return true; // releases of keys pressed while typing must not reach the game either
    switch (key)
    {
    case GLFW_KEY_ESCAPE:
    case GLFW_KEY_GRAVE_ACCENT:
        if (action == GLFW_PRESS)
            consoleOpen_ = false;
        break;
    case GLFW_KEY_BACKSPACE:
        if (!consoleInput_.empty())
            consoleInput_.pop_back();
        break;
    case GLFW_KEY_ENTER:
    case GLFW_KEY_KP_ENTER:
        if (!consoleInput_.empty())
        {
            std::string err;
            if (!harnessRunner_->enqueueText(consoleInput_, "console", err))
                harnessRunner_->consoleEcho("  ERROR: " + err);
            if (consoleHistory_.empty() || consoleHistory_.back() != consoleInput_)
                consoleHistory_.push_back(consoleInput_);
            consoleInput_.clear();
            consoleHistIdx_ = -1;
        }
        break;
    case GLFW_KEY_UP:
        if (!consoleHistory_.empty())
        {
            consoleHistIdx_ = consoleHistIdx_ < 0 ? (int)consoleHistory_.size() - 1 : std::max(0, consoleHistIdx_ - 1);
            consoleInput_ = consoleHistory_[consoleHistIdx_];
        }
        break;
    case GLFW_KEY_DOWN:
        if (consoleHistIdx_ >= 0)
        {
            ++consoleHistIdx_;
            if (consoleHistIdx_ >= (int)consoleHistory_.size())
            {
                consoleHistIdx_ = -1;
                consoleInput_.clear();
            }
            else
                consoleInput_ = consoleHistory_[consoleHistIdx_];
        }
        break;
    default:
        break;
    }
    return true;
}

void SatelliteSim::onChar(GLFWwindow *, unsigned int cp)
{
    if (textEdit_.id != 0)
    {
        // A focused text field (SatelliteSimUI.cpp "Text fields"): printable ASCII only (the font is ASCII).
        if (cp >= 32 && cp < 127)
        {
            textEditDropSel(); // typing replaces the selection
            textEdit_.ensureCaret = true;
            if (textEdit_.buf.size() < textEdit_.maxLen)
            {
                textEdit_.buf.insert(textEdit_.buf.begin() + std::clamp(textEdit_.caret, 0, (int)textEdit_.buf.size()), (char)cp);
                ++textEdit_.caret;
            }
            textEdit_.t0 = glfwGetTime();
        }
        return;
    }
    if (!consoleOpen_ || cp == '`' || cp == '~')
        return; // the toggle key's own character
    if (cp >= 32 && cp < 127 && consoleInput_.size() < 400)
        consoleInput_ += (char)cp;
}

// ─── Terrain probe (harness `probe`) ──────────────────────────────────────────────────────────────
struct TerrainProbePC
{
    glm::mat4 skyView;
    glm::vec4 cam;    // fovY, aspect, pixel x, pixel y
    glm::vec4 screen; // full-res size
    glm::vec4 obsECEFDir;
};
static_assert(sizeof(TerrainProbePC) <= 128, "push constant floor");
static constexpr VkDeviceSize kProbeBytes = (8 + 64) * sizeof(glm::vec4);

void SatelliteSim::createTerrainProbe(VulkanContext &ctx)
{
    VkDescriptorSetLayoutBinding b[5] = {};
    b[0] = {0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[1] = {1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[2] = {2, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[3] = {3, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    b[4] = {4, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr};
    VkDescriptorSetLayoutCreateInfo li{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
    li.bindingCount = 5;
    li.pBindings = b;
    vkCreateDescriptorSetLayout(ctx.device, &li, nullptr, &probeDescLayout);
    VkDescriptorPoolSize ps[3] = {{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 3},
                                  {VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, 1},
                                  {VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1}};
    VkDescriptorPoolCreateInfo pi{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
    pi.poolSizeCount = 3;
    pi.pPoolSizes = ps;
    pi.maxSets = 1;
    vkCreateDescriptorPool(ctx.device, &pi, nullptr, &probeDescPool);
    VkDescriptorSetAllocateInfo ai{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
    ai.descriptorPool = probeDescPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &probeDescLayout;
    vkAllocateDescriptorSets(ctx.device, &ai, &probeDescSet);

    ctx.createBuffer(kProbeBytes, VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
                     VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT, probeBuf, probeMem);
    vkMapMemory(ctx.device, probeMem, 0, kProbeBytes, 0, &probeMapped);
    memset(probeMapped, 0, (size_t)kProbeBytes);

    VkPushConstantRange pr{VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(TerrainProbePC)};
    VkPipelineLayoutCreateInfo pli{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
    pli.setLayoutCount = 1;
    pli.pSetLayouts = &probeDescLayout;
    pli.pushConstantRangeCount = 1;
    pli.pPushConstantRanges = &pr;
    vkCreatePipelineLayout(ctx.device, &pli, nullptr, &probePipeLayout);
    VkShaderModule mod = ctx.loadShader("shaders/terrain_probe.comp.spv");
    VkComputePipelineCreateInfo ci{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
    ci.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO, nullptr, 0, VK_SHADER_STAGE_COMPUTE_BIT, mod, "main",
                nullptr};
    ci.layout = probePipeLayout;
    if (vkCreateComputePipelines(ctx.device, ctx.pipelineCache, 1, &ci, nullptr, &probePipeline) != VK_SUCCESS)
        probePipeline = VK_NULL_HANDLE;
    vkDestroyShaderModule(ctx.device, mod, nullptr);
}

void SatelliteSim::recordTerrainProbe(VkCommandBuffer cmd, VulkanContext &ctx)
{
    if (!probeRequested || !probePipeline)
        return;
    probeRequested = false;
    // Descriptors every time: sceneDepthView is recreated on resize. The previous frame's use of
    // this set has completed (single frame in flight, fence waited at the top of the frame).
    VkSampler elevS = earthElevSampler ? earthElevSampler : noiseSampler;
    VkImageView elevV = earthElevView ? earthElevView : noiseTexView;
    VkSampler specS = earthSpecSampler ? earthSpecSampler : noiseSampler;
    VkImageView specV = earthSpecView ? earthSpecView : noiseTexView;
    VkDescriptorImageInfo elev{elevS, elevV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo spec{specS, specV, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorImageInfo depth{sceneDepthSampler, sceneDepthView, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL};
    VkDescriptorBufferInfo ubo{cloudParamsBuf, 0, sizeof(GpuCloudParams)};
    VkDescriptorBufferInfo out{probeBuf, 0, kProbeBytes};
    VkWriteDescriptorSet w[5] = {};
    w[0] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, probeDescSet, 0, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &elev, nullptr, nullptr};
    w[1] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, probeDescSet, 1, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &spec, nullptr, nullptr};
    w[2] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, probeDescSet, 2, 0, 1, VK_DESCRIPTOR_TYPE_UNIFORM_BUFFER, nullptr, &ubo, nullptr};
    w[3] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, probeDescSet, 3, 0, 1, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, &depth, nullptr, nullptr};
    w[4] = {VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET, nullptr, probeDescSet, 4, 0, 1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, nullptr, &out, nullptr};
    vkUpdateDescriptorSets(ctx.device, 5, w, 0, nullptr);

    TerrainProbePC pc{};
    pc.skyView = camera.viewMatrix();
    pc.cam = glm::vec4(glm::radians(camera.fovYDeg), (float)ctx.swapExtent.width / (float)ctx.swapExtent.height,
                       probePx.x, probePx.y);
    pc.screen = glm::vec4((float)ctx.swapExtent.width, (float)ctx.swapExtent.height, 0.0f, 0.0f);
    pc.obsECEFDir = glm::vec4(obsDir, obsHeightOffset);
    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, probePipeLayout, 0, 1, &probeDescSet, 0, nullptr);
    vkCmdPushConstants(cmd, probePipeLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
    vkCmdDispatch(cmd, 1, 1, 1);
    VkMemoryBarrier mb{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
    mb.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
    mb.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &mb, 0, nullptr, 0,
                         nullptr);
}

void SatelliteSim::destroyTerrainProbe(VkDevice device)
{
    if (probePipeline)
        vkDestroyPipeline(device, probePipeline, nullptr);
    if (probePipeLayout)
        vkDestroyPipelineLayout(device, probePipeLayout, nullptr);
    if (probeDescPool)
        vkDestroyDescriptorPool(device, probeDescPool, nullptr);
    if (probeDescLayout)
        vkDestroyDescriptorSetLayout(device, probeDescLayout, nullptr);
    if (probeBuf)
    {
        vkUnmapMemory(device, probeMem);
        vkDestroyBuffer(device, probeBuf, nullptr);
        vkFreeMemory(device, probeMem, nullptr);
    }
    probePipeline = VK_NULL_HANDLE;
    probePipeLayout = VK_NULL_HANDLE;
    probeDescPool = VK_NULL_HANDLE;
    probeDescLayout = VK_NULL_HANDLE;
    probeBuf = VK_NULL_HANDLE;
}
