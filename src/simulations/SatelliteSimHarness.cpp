// ── Automation harness: SatelliteSim's commands ─────────────────────────────────────────────────
// The command language itself (parsing, queue, run folder, live inbox) is src/Harness.h/.cpp; this
// file is what each command DOES to the sim. Reference with examples: docs/HARNESS.md. Keep that
// document's command table in step with harnessExec() below — it is what other agents read.
//
// Everything here runs at the top of buildUI (harnessTick), i.e. before this frame's camera
// derivation, recordCompute and draws: a command's effect is in the very frame it ran in, and a
// `capture` issued after it records that frame.
#include "SatelliteSim.h"
#include "SatPhotometry.h"
#include "Ambience.h"
#include "../AudioSystem.h"
#include "../Harness.h"
#include "../Log.h"
#include "version.h"

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

// The sim's clock is seconds since J2000 = 2000-01-01T12:00:00 (see SatelliteSim::init; the sim
// treats that as UTC, and its Earth rotation has no GMST offset — CLAUDE.md "sim clock != real UTC").
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
    const double unix = t + (double)kJ2000Unix;
    int64_t days = (int64_t)std::floor(unix / 86400.0);
    double sod = unix - (double)days * 86400.0;
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
        if (!cur->is_object() || !cur->contains(part))
            return nullptr;
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
    "observer lat= lon= [agl=|alt=]; camera [az= el= fov=] | camera look|track <sun|moon|sel|planet name|off>; "
    "select sat <i> | select const <name> [n=<k>] | select planet <name> | select none; follow [off] [offset=x,y,z]; track [on|off]; "
    "const <name|all> on|off [highlight=on|off] | const list; set <section.key> <value>; get [section[.key]]; "
    "preset <name>; knockout <none|mask|+key|-key ...> | knockout list; capture <name> [ui=on] [crop=x,y,w,h] [scale=s]; "
    "state [name]; probe <x> <y>; perf [frames=N] [name=]; sweep; debugview <off|normals|detail|steps|albedo|shadow|rough|elevzebra|distzebra>; ui show|hide|scale <x>|open <win> [tab=]|close <win|all>; "
    "window <W>x<H>; path clear|key <t> ...|goto <t>|play [fps=] [record=]; overlay text|label|clear ...; "
    "audio [state [name]] | audio record <name> [seconds=] [bus=] [solo=] | audio expect <layers> [absent=] | "
    "audio force <layer> <gain|off> | audio music [next|prev|pause|play]; log <text>; quit";
} // namespace

// ─── Lifecycle ────────────────────────────────────────────────────────────────────────────────────
void SatelliteSim::harnessInit()
{
    if (!harness::active())
        return;
    // A harness run starts in the scene, not the cinematic, and without first-run notices that would
    // otherwise sit in every UI capture for their first eight seconds.
    showIntro = false;
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
    const auto &K = harnessPath_;
    if (K.empty())
        return HarnessCamKey{};
    if (t <= K.front().t)
        return K.front();
    if (t >= K.back().t)
        return K.back();
    size_t i = 0;
    while (i + 1 < K.size() && K[i + 1].t < t)
        ++i;
    const HarnessCamKey &a = K[i], &b = K[i + 1];
    const double h = b.t - a.t, s = (t - a.t) / h;
    const double h00 = 2 * s * s * s - 3 * s * s + 1, h10 = s * s * s - 2 * s * s + s;
    const double h01 = -2 * s * s * s + 3 * s * s, h11 = s * s * s - s * s;
    // Catmull-Rom tangent of channel f at key j (one-sided at the ends), per path second.
    auto tangent = [&](size_t j, double (*f)(const HarnessCamKey &)) -> double
    {
        const size_t j0 = j > 0 ? j - 1 : j, j1 = j + 1 < K.size() ? j + 1 : j;
        const double dt = K[j1].t - K[j0].t;
        return dt > 0.0 ? (f(K[j1]) - f(K[j0])) / dt : 0.0;
    };
    auto herm = [&](double (*f)(const HarnessCamKey &))
    { return h00 * f(a) + h10 * h * tangent(i, f) + h01 * f(b) + h11 * h * tangent(i + 1, f); };
    HarnessCamKey r;
    r.t = t;
    r.lat = herm([](const HarnessCamKey &k) { return k.lat; });
    r.lon = herm([](const HarnessCamKey &k) { return k.lon; });
    r.alt = std::exp(herm([](const HarnessCamKey &k) { return std::log(std::max(k.alt, 0.0) + 10.0); })) - 10.0;
    r.az = herm([](const HarnessCamKey &k) { return k.az; });
    r.el = herm([](const HarnessCamKey &k) { return k.el; });
    r.fov = std::exp(herm([](const HarnessCamKey &k) { return std::log(k.fov); }));
    // Sim time: linear between the keys that set it (hold outside them).
    const HarnessCamKey *sa = nullptr, *sb = nullptr;
    for (const auto &k : K)
    {
        if (!k.hasSim)
            continue;
        if (k.t <= t)
            sa = &k;
        if (k.t >= t && !sb)
            sb = &k;
    }
    if (sa || sb)
    {
        r.hasSim = true;
        if (sa && sb && sb->t > sa->t)
            r.simT = sa->simT + (sb->simT - sa->simT) * (t - sa->t) / (sb->t - sa->t);
        else
            r.simT = sa ? sa->simT : sb->simT;
    }
    return r;
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
                     {"following", followActive}};
    if (followActive)
        j["observer"]["follow"] = {{"sat", followSatIndex},
                                   {"offset_m", {followOffset.x, followOffset.y, followOffset.z}}};
    j["camera"] = {{"az_deg", camera.azDeg}, {"el_deg", camera.elDeg}, {"fov_y_deg", camera.fovYDeg},
                   {"tracking", trackActive}};
    const glm::vec3 sun(sunDirENU), moon(moonDirENU);
    j["sun"] = {{"az_deg", azDegOf(sun)}, {"el_deg", elDegOf(sun)}};
    j["moon"] = {{"az_deg", azDegOf(moon)}, {"el_deg", elDegOf(moon)}, {"illum", moonDirENU.w}};

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
    j["render"] = {{"preset", kGraphicsPresetNames[(int)graphicsPreset]},
                   {"knockout_mask", debugDisableMask},
                   {"knockouts", ko},
                   {"render_scale", renderScale},
                   {"width", ctx_ ? ctx_->swapExtent.width : 0},
                   {"height", ctx_ ? ctx_->swapExtent.height : 0},
                   {"ui_visible", uiVisible},
                   {"ui_scale", uiScale}};

    json sel = json::object();
    if (selectedSatIndex >= 0 && selectedSatIndex < (int)satOrbits.size())
    {
        const SatOrbit &o = satOrbits[selectedSatIndex];
        sel["sat"] = selectedSatIndex;
        sel["type"] = satTypes[o.typeIdx].name;
        if (o.constIdx < constellations.size())
        {
            sel["constellation"] = constellations[o.constIdx].name;
            sel["n"] = selectedSatIndex - (int)constellations[o.constIdx].orbitStart;
        }
        updateSelectedSkyDir();
        sel["az_deg"] = azDegOf(selSkyDirCpu);
        sel["el_deg"] = elDegOf(selSkyDirCpu);
        sel["above_earth"] = selAboveEarth;
        sel["track"] = trackActive; // the camera lock below `camera.tracking`, on the satellite it follows
    }
    if (selectedPlanetIndex >= 0)
        sel["planet"] = kPlanetNames[selectedPlanetIndex];
    j["selection"] = sel;

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
            if (name == "time" || name == "observer" || name == "follow")
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
            // The sim clock is not real UTC (no GMST offset), so a UTC hour does not give a local
            // time of day. This finds one: the time nearest the current one (within +-12 h) at which
            // the Sun stands at the requested elevation for THIS observer, from the same Sun and
            // observer formulas updatePositions uses.
            const glm::dvec3 od = glm::dvec3(obsDir);
            const double rObs = satphot::kEarthRadiusM + obsTerrainH + obsHeightOffset;
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

    // ── camera ──────────────────────────────────────────────────────────────────
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
            selectSatellite(idx);
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
                   " (sat " + std::to_string(r["sat"].get<int>()) + ")";
        if (r.contains("planet"))
            msg += " " + r["planet"].get<std::string>();
        if (r.empty())
            msg = "selection cleared";
        r["message"] = msg;
        return Status::Done;
    }
    if (n == "follow")
    {
        if (lower(pos(0)) == "off")
        {
            stopFollow();
            r["message"] = "follow off";
            return Status::Done;
        }
        const int idx = c.has("sat") ? (int)c.num("sat", -1) : selectedSatIndex;
        if (idx < 0 || idx >= (int)satOrbits.size())
            fail("follow: select a satellite first (or sat=<i>)");
        if (!followActive || followSatIndex != idx)
            startFollow(idx);
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
    if (n == "const")
    {
        const std::string name = pos(0);
        if (lower(name) == "list" || name.empty())
        {
            json list = json::array();
            for (const auto &cc : constellations)
                list.push_back({{"name", cc.name}, {"enabled", cc.enabled}, {"highlight", cc.highlight},
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
                    fail("ui open: settings, viewcontrols, trace, info, viewer, console");
                ch->open = true;
            }
            if (c.has("tab"))
            {
                const int t = settingsTabIndexByName(c.str("tab"));
                if (t < 0)
                    fail("ui open: unknown settings tab '" + c.str("tab") + "'");
                settingsActiveTab = t;
                if (t >= 6 && t <= 10) // Clouds..Beams are "advanced" (buildSettingsTabbedBody)
                    showAdvancedSettings = true;
            }
            uiVisible = true;
        }
        else if (sub == "close")
        {
            if (lower(pos(1)) == "all")
                consoleOpen_ = settingsChrome.open = viewControlsChrome.open = traceChrome.open = infoChrome.open =
                    viewerChrome.open = false;
            else if (lower(pos(1)) == "console")
                consoleOpen_ = false;
            else if (WindowChrome *ch = chromeOf(pos(1)))
                ch->open = false;
            else
                fail("ui close: settings, viewcontrols, trace, info, viewer, all");
        }
        else
            fail("ui: show | hide | scale <x> | open <window> [tab=<name>] | close <window|all> | dump [name]");
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
            harnessPath_.clear();
            r["message"] = "path cleared";
            return Status::Done;
        }
        if (sub == "key")
        {
            // Unspecified channels inherit from the previous key (the current view for the first).
            HarnessCamKey k;
            if (!harnessPath_.empty())
                k = harnessPath_.back();
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
            if (!harnessPath_.empty() && k.t <= harnessPath_.back().t)
                fail("path key: times must increase");
            k.hasSim = false;
            if (c.has("lat"))
                k.lat = c.num("lat", 0.0);
            if (c.has("lon"))
            {
                // Unwrap against the previous key so a path crossing the antimeridian goes the short way.
                double lon = c.num("lon", 0.0);
                if (!harnessPath_.empty())
                    while (lon - harnessPath_.back().lon > 180.0)
                        lon -= 360.0;
                if (!harnessPath_.empty())
                    while (lon - harnessPath_.back().lon < -180.0)
                        lon += 360.0;
                k.lon = lon;
            }
            if (c.has("alt"))
                k.alt = c.num("alt", 0.0);
            if (c.has("az"))
            {
                double az = c.num("az", 0.0);
                const double prev = harnessPath_.empty() ? camera.azDeg : harnessPath_.back().az;
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
            harnessPath_.push_back(k);
            r["keys"] = harnessPath_.size();
            r["message"] = "key " + std::to_string(harnessPath_.size()) + " at t=" + pos(1);
            return Status::Done;
        }
        if (sub == "goto")
        {
            harnessApplyCam(harnessEvalPath(parseNum(pos(1), "path goto <t>")));
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
                if (harnessPath_.size() < 2)
                    fail("path play: add at least two keys (path key <t> ...)");
                const double fps = c.num("fps", 30.0);
                if (fps < 1.0 || fps > 240.0)
                    fail("path play: fps in [1, 240]");
                a.scratch["fps"] = fps;
                a.scratch["i"] = 0;
                a.scratch["n"] = (int)std::floor((harnessPath_.back().t - harnessPath_.front().t) * fps + 1e-6) + 1;
                a.scratch["record"] = c.str("record");
                a.scratch["simStart"] = (double)simDayJ2000 * 86400.0 + simSecInDay;
                a.scratch["simRate"] = timePaused ? 0.0 : (double)kTimeScales[timeScaleIdx] * timeDir;
                a.scratch["pausedBefore"] = timePaused;
                a.scratch["captured"] = 0;
                harnessFixedDtOverride_ = (float)(1.0 / fps);
                timePaused = true; // the path owns the clock
                harnessTrack_ = 0;
            }
            const std::string rec = a.scratch["record"];
            if (!rec.empty() && (screenshotRequested || screenshotCopyPending || screenshotEncoding.load()))
                return Status::Pending; // last frame still encoding
            const int i = a.scratch["i"], nF = a.scratch["n"];
            if (i >= nF)
            {
                harnessFixedDtOverride_ = 0.0f;
                timePaused = a.scratch["pausedBefore"].get<bool>();
                r["frames"] = nF;
                if (!rec.empty())
                    r["pattern"] = harnessRunner_->capturePath(rec + "_%05d", ".png");
                r["message"] = "played " + std::to_string(nF) + " frames" +
                               (rec.empty() ? std::string() : " -> captures/" + rec + "_#####.png");
                return Status::Done;
            }
            const double fps = a.scratch["fps"];
            const double tp = harnessPath_.front().t + i / fps;
            HarnessCamKey k = harnessEvalPath(tp);
            if (!k.hasSim)
            {
                k.hasSim = true;
                k.simT = a.scratch["simStart"].get<double>() + a.scratch["simRate"].get<double>() * (tp - harnessPath_.front().t);
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

    // ── debug views ─────────────────────────────────────────────────────────────
    if (n == "debugview")
    {
        // Terrain debug views (sat_sky.frag, cloud.terrainDebugView). Not persisted.
        static const char *kViews[] = {"off", "normals", "detail", "steps", "albedo", "shadow", "rough", "elevzebra", "distzebra",
                                       "erosion",
                                       // 10+: the terrain LIGHTING term by term (linear radiance x100, see sat_sky.frag)
                                       "terms", "direct", "skyamb", "night", "moon", "aurora", "gates", "factors",
                                       "skyambraw", "suntint", "aofactors", "day", "nightmap", "geodot", "sunvis"};
        const int viewCount = (int)(sizeof(kViews) / sizeof(kViews[0]));
        const std::string v = lower(pos(0) == "terrain" ? pos(1) : pos(0));
        int idx = -1;
        for (int i = 0; i < viewCount; ++i)
            if (v == kViews[i])
                idx = i;
        if (idx < 0 && !v.empty() && isdigit((unsigned char)v[0]))
            idx = (int)parseNum(v, "debugview");
        if (idx < 0 || idx >= viewCount)
            fail("debugview: off | normals | detail | steps | albedo | shadow | rough | elevzebra | distzebra | erosion | "
                 "terms | direct | skyamb | night | moon | aurora | gates | factors | skyambraw | suntint | aofactors | "
                 "day | nightmap | geodot | sunvis "
                 "(steps: blue = few march steps .. red = the budget; rough: R roughness, G rock, B snow; "
                 "zebras: stripes every 25 m of elevation / 100 m of distance; terms/direct/skyamb/night/moon/aurora: "
                 "the terrain light term by term as linear radiance x100, so a capture pixel reads the number; "
                 "sunvis: R = the Sun's disc clears this point's own horizon (0 = no direct sun reaches it), "
                 "G = its margin over that horizon, B = the horizon dip at this altitude)");
        terrainDebugView = idx;
        r["message"] = std::string("terrain debug view ") + kViews[idx];
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
            else if (!op.empty() && op != "state")
            {
                a.error = "audio music [next|prev|pause|play|state]";
                return Status::Error;
            }
            r = {{"track", audio_->trackIndex()},       {"name", audio_->trackName(audio_->trackIndex())},
                 {"tracks", audio_->trackCount()},      {"paused", audio_->musicPaused()},
                 {"gap_remaining_s", audio_->gapRemaining()}, {"gap_s", audio_->musicGap()},
                 {"altitude_fade", audio_->musicFade()}};
            r["message"] = audio_->trackName(audio_->trackIndex()) + (audio_->musicPaused() ? " (paused)" : "") +
                           " - track " + std::to_string(audio_->trackIndex() + 1) + "/" +
                           std::to_string(audio_->trackCount());
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
            if (!audio_->renderWav(path, seconds, mask, solo, [this](float dt) { ambience_->tickEvents(dt, audio_); },
                                   st, err))
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
                  "[min=] | force <layer> <gain|off> | force off";
        return Status::Error;
    }

    // ── perf ────────────────────────────────────────────────────────────────────
    if (n == "perf")
    {
        static const char *kBucketKeys[8] = {"scene_depth", "beam_cloud_block", "orbit_compute", "cloud_march",
                                             "flare_compute", "sky_background_draw", "satellite_star_draw", "ui_overlay"};
        static const char *kCpuKeys[CPU_COUNT] = {"build_ui", "update_positions", "beam_readback", "update_stars",
                                                  "light_pollution_dome", "update_planets"};
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
    if (vkCreateComputePipelines(ctx.device, VK_NULL_HANDLE, 1, &ci, nullptr, &probePipeline) != VK_SUCCESS)
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
