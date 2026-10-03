// Cinematics (review 17): playback and export of the camera paths in Cinematic.h. The window is
// buildCinematicWindow (SatelliteSimUI.cpp); the harness drives the same state through `path` (the current
// shot's keys) and `cine` (SatelliteSimHarness.cpp).
//
// Play: real time, the cinematic owns the camera and the clock (time paused, sim time from the shot).
// Export preview: a fixed 1/fps step, every frame captured as it renders (the temporal passes see the real
//   motion, like a recording of play).
// Export HQ: every frame is a settled HQ photo — the offscreen photo target at "HQ photo resolution" for the
//   whole export, the clouds at full rate, "HQ photo settle frames" at each pose (time held), then the copy.
//   Converged and supersampled; motion leaves no trace in a frame (no motion blur).
#include "SatelliteSim.h"
#include "../Log.h"
#include "../Harness.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <ctime>
#include <filesystem>
#include <fstream>

std::vector<CineKey> &SatelliteSim::cineKeys()
{
    if (cine_.shots.empty())
        cine_.shots.push_back(CineShot{"shot 1"});
    cineShot_ = std::clamp(cineShot_, 0, (int)cine_.shots.size() - 1);
    return cine_.shots[cineShot_].keys;
}

CineKey SatelliteSim::cineCurrentPose() const
{
    CineKey k;
    k.lat = glm::degrees(std::asin(std::clamp((double)obsDir.z, -1.0, 1.0)));
    k.lon = glm::degrees(std::atan2((double)obsDir.y, (double)obsDir.x));
    k.alt = obsHeightOffset;
    k.az = camera.azDeg;
    k.el = camera.elDeg;
    k.fov = camera.fovYDeg;
    k.hasSim = false;
    k.simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    if (followActive)
    {
        k.ox = followOffset.x;
        k.oy = followOffset.y;
        k.oz = followOffset.z;
        k.hasOffset = true;
        // The view in the satellite's along / cross / radial frame (follow mode's aim is free on arrival, and a
        // follow key that only kept the offset played back aimed at the satellite: a different framing).
        if (followBasisValid && !followAimLock)
        {
            const double el = glm::radians((double)camera.elDeg);
            const glm::dvec3 v = std::cos(el) * glm::dvec3(obsFacing) + std::sin(el) * glm::dvec3(obsDir);
            k.hasView = true;
            k.vx = glm::dot(v, followBasisT);
            k.vy = glm::dot(v, followBasisN);
            k.vz = glm::dot(v, followBasisR);
        }
    }
    return k;
}

// A follow shot's keys are offsets from ITS satellite: one taken while not following it (or following another)
// would put the camera somewhere unrelated, so the window refuses it and says why.
bool SatelliteSim::cineKeyFollowMismatch(std::string &why) const
{
    if (cine_.shots.empty())
        return false;
    const CineShot &S = cine_.shots[std::clamp(cineShot_, 0, (int)cine_.shots.size() - 1)];
    if (S.followSat < 0)
        return false;
    if (!followActive || followSatIndex != S.followSat)
    {
        why = "This shot follows satellite #" + std::to_string(S.followSat) +
              ": select it and press Go to (or Follow) before taking a key";
        return true;
    }
    return false;
}

std::string SatelliteSim::cineDir() const
{
    return (std::filesystem::path(userDataDir_) / "cinematics").string();
}

void SatelliteSim::cineRefreshFiles()
{
    cineFiles_.clear();
    std::error_code ec;
    for (const auto &e : std::filesystem::directory_iterator(cineDir(), ec))
        if (e.path().extension() == ".json")
            cineFiles_.push_back(e.path().string());
    std::sort(cineFiles_.begin(), cineFiles_.end());
}

// Letters, digits, - _ . kept; anything else (spaces included) becomes _. Never empty.
std::string SatelliteSim::cineFileStem(const std::string &name)
{
    std::string o;
    for (char c : name)
        o += (std::isalnum((unsigned char)c) || c == '-' || c == '_' || c == '.') ? c : '_';
    while (!o.empty() && (o.back() == '.' || o.back() == ' '))
        o.pop_back();
    return o.empty() ? "untitled" : o;
}

bool SatelliteSim::cineSave(const std::string &file, std::string &err)
{
    // A bare name goes to <user data>/cinematics/<name>.json; a path is used as given. No name: the cinematic's
    // own (the window's Name field), as a file name.
    std::filesystem::path p = file.empty() ? std::filesystem::path(cineDir()) / (cineFileStem(cine_.name) + ".json")
                                           : std::filesystem::path(file);
    if (!file.empty() && !p.has_parent_path())
        p = std::filesystem::path(cineDir()) / (p.extension() == ".json" ? p : std::filesystem::path(file + ".json"));
    std::error_code ec;
    std::filesystem::create_directories(p.parent_path(), ec);
    std::ofstream out(p);
    if (!out)
    {
        err = "cannot write " + p.string();
        return false;
    }
    out << cineToJson(cine_).dump(1);
    cineStatus_ = "Saved " + p.filename().string();
    cineFile_ = p.string();
    cineRefreshFiles();
    return true;
}

bool SatelliteSim::cineLoad(const std::string &file, std::string &err)
{
    std::filesystem::path p(file);
    if (!p.has_parent_path() && p.extension() != ".json")
        p = std::filesystem::path(cineDir()) / (file + ".json");
    else if (!p.has_parent_path())
        p = std::filesystem::path(cineDir()) / file;
    std::ifstream in(p);
    if (!in)
    {
        err = "cannot open " + p.string();
        return false;
    }
    nlohmann::json j = nlohmann::json::parse(in, nullptr, false);
    if (j.is_discarded())
    {
        err = "not JSON: " + p.string();
        return false;
    }
    Cinematic c;
    if (!cineFromJson(j, c, err))
        return false;
    cineStop("Loaded another cinematic");
    cine_ = std::move(c);
    cineShot_ = 0;
    cineScrub_ = 0.0f;
    cineStatus_ = "Loaded " + p.filename().string();
    cineFile_ = p.string();
    return true;
}

// The pose at global time t (or the current shot's local time while playing one shot). A cut — a new shot
// — applies that shot's settings patch and fixes its sim time at the shot's start.
void SatelliteSim::cineApplyAt(double t, bool force)
{
    int si = cineShot_;
    double local = t;
    if (!cinePlayOneShot_)
        si = cine_.shotAt(t, local);
    else if (!cine_.shots.empty() && !cine_.shots[si].keys.empty())
        local = cine_.shots[si].keys.front().t + std::clamp(t, 0.0, cine_.shots[si].duration());
    if (si < 0 || si >= (int)cine_.shots.size() || cine_.shots[si].keys.empty())
        return;
    const CineShot &shot = cine_.shots[si];
    if (si != cineLastShot_ || force)
    {
        if (!shot.settings.is_null() && si != cineLastShot_)
        {
            applySettingsJson(shot.settings, true);
            timePaused = true;   // the cinematic keeps the clock whatever the patch says
        }
        if (shot.hasDrift && si != cineLastShot_)
        {
            cloudDriftPhaseOffset = shot.driftOffset;
            cloudDriftRate = (float)shot.driftRate;
        }
        cineShotSim_ = shot.simStartValid ? shot.simStart : (double)simDayJ2000 * 86400.0 + simSecInDay;
        cineLastShot_ = si;
        trailClearPending = true;
        // A cut: nothing temporal may carry the last shot into this one (sky TAA, the clouds' history).
        skyTaaHistValid = false;
        cv2HistoryValid = false;
    }
    // Ease in / out over the shot (its time remapped, so the camera starts and stops gently; sim time follows
    // the shot's real time, not the eased one).
    const double t0 = shot.keys.front().t, dur = shot.duration();
    double camLocal = local;
    if (dur > 0.0 && shot.ease > 0.0)
    {
        const double u = std::clamp((local - t0) / dur, 0.0, 1.0);
        camLocal = t0 + dur * (u + shot.ease * (u * u * (3.0 - 2.0 * u) - u));
    }
    CineKey k = cineEval(shot.keys, camLocal);
    {
        const CineKey ks = cineEval(shot.keys, local);   // sim time on the un-eased clock
        k.hasSim = ks.hasSim;
        k.simT = ks.simT;
    }
    if (!k.hasSim)
    {
        k.hasSim = true;
        k.simT = cineShotSim_ + shot.simRate * (local - shot.keys.front().t);
    }
    if (shot.followSat >= 0 && shot.followSat < (int)satOrbits.size())
    {
        // A follow shot: the camera rides with the satellite (follow mode, aimed at it), at the key's offset.
        const double days = std::floor(k.simT / 86400.0);
        simDayJ2000 = (int64_t)days;
        simSecInDay = k.simT - days * 86400.0;
        if (!followActive || followSatIndex != shot.followSat)
            startFollow(shot.followSat);
        if (followActive)
        {
            // 2026-10-03: no flight in progress (a Go to started before playing kept flying under the shot), the
            // key's own offset (a key without one — taken before Follow was switched on — sits behind the satellite
            // where Go to would put it, not at its centre), and the key's own framing.
            followFlight = 0;
            followFlightAlign = false;
            glm::dvec3 off(k.ox, k.oy, k.oz);
            if (!k.hasOffset || glm::length(off) < 1e-3)
            {
                const SatMeshRenderer::TypeMesh *tm = meshRenderer.typeMesh((int)satOrbits[shot.followSat].typeIdx);
                const double r = tm ? std::max(1.0, (double)tm->boundsRadius) : 10.0;
                off = glm::dvec3(-4.0 * r, 0.0, 1.5 * r);
            }
            followOffset = off;
            followAimLock = !k.hasView;
            camera.fovYDeg = glm::clamp((float)k.fov, SkyCamera::kMinFovDeg, SkyCamera::kMaxFovDeg);
            updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay, 0.0f);
            updateFollow(0.0f);
            if (k.hasView && followBasisValid)
            {
                // The view from the satellite's frame to the observer's local az / el (updateFollow keeps it in the
                // satellite's frame from here on, like a free aim).
                const glm::dvec3 d = glm::normalize(k.vx * followBasisT + k.vy * followBasisN + k.vz * followBasisR);
                const glm::dvec3 up = glm::dvec3(obsDir);
                const double cLH = std::sqrt(up.x * up.x + up.y * up.y);
                const glm::dvec3 east = cLH > 1e-9 ? glm::dvec3(-up.y / cLH, up.x / cLH, 0.0) : glm::dvec3(0.0, 1.0, 0.0);
                const glm::dvec3 north = glm::cross(up, east);
                aimCameraAzEl((float)glm::degrees(std::atan2(glm::dot(d, east), glm::dot(d, north))),
                              (float)glm::degrees(std::asin(std::clamp(glm::dot(d, up), -1.0, 1.0))));
            }
            return;
        }
    }
    harnessApplyCam(k);
    obsTerrainH = cpuTerrainHeightM(obsLatDeg, obsLonDeg);
    updatePositions((double)simDayJ2000 * 86400.0 + simSecInDay, 0.0f);
}

void SatelliteSim::cineStart(CineRun mode, bool oneShot)
{
    if (cineActive())
        cineStop("restarted");
    cinePlayOneShot_ = oneShot;
    const double dur = oneShot ? (cine_.shots.empty() ? 0.0 : cine_.shots[std::clamp(cineShot_, 0, (int)cine_.shots.size() - 1)].duration())
                               : cine_.duration();
    if (cine_.shots.empty() || dur <= 0.0)
    {
        cineStatus_ = "Nothing to play: a shot needs two keys";
        return;
    }
    if (mode != CineRun::Play && (!ctx_ || !ctx_->screenshotSupported || photoState != 0))
    {
        cineStatus_ = photoState != 0 ? "An HQ photo is being taken" : "Export needs screenshot support (GPU/driver)";
        return;
    }
    cineSavedPaused_ = timePaused;
    timePaused = true; // the cinematic owns the clock
    cineRun_ = mode;
    cineT_ = 0.0;
    cineFrame_ = 0;
    cineSettle_ = 0;
    cineLastShot_ = -1;
    cinePrerollShot_ = -1;
    cineSub_ = 0;
    cineAccumSubs_ = mode == CineRun::Play ? 0 : std::clamp((int)std::lround(cineBlurSubs_), 1, 32);
    cineAccumCount_ = 0;
    cineAccum_.clear();
    const double fps = std::clamp((double)cineExportFps_, 1.0, 240.0);
    cine_.fps = fps;
    cineFrames_ = (int)std::floor(dur * fps + 1e-6) + 1;
    if (mode == CineRun::ExportPreview || mode == CineRun::ExportHQ)
    {
        // screenshots/cinematics/<name>_<stamp>/frame_00000.png (screenshots/ is a link into the source tree).
        char stamp[32];
        const time_t now = time(nullptr);
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        strftime(stamp, sizeof(stamp), "%Y%m%d_%H%M%S", &lt);
        std::filesystem::path dir = std::filesystem::path(exeDir_) / "screenshots" / "cinematics" /
                                    (cine_.name + "_" + stamp + (mode == CineRun::ExportHQ ? "_hq" : ""));
        if (harnessRunner_)
            dir = std::filesystem::path(harnessRunner_->capturePath("cine_" + cine_.name + (mode == CineRun::ExportHQ ? "_hq" : ""), "")).parent_path() /
                  ("cine_" + cine_.name + (mode == CineRun::ExportHQ ? "_hq" : ""));
        std::error_code ec;
        std::filesystem::create_directories(dir, ec);
        cineOutDir_ = dir.string();
        cineFixedDt_ = (float)(1.0 / (fps * std::max(cineAccumSubs_, 1)));   // a subframe's step with motion blur
        {
            std::ofstream note(dir / "README.txt");
            note << "Frames of the cinematic '" << cine_.name << "' at " << fps << " fps ("
                 << (mode == CineRun::ExportHQ ? "HQ: each frame a settled HQ photo" : "preview: rendered in motion") << ").\n"
                 << "Video: python tools/harness/frames2video.py \"" << dir.string() << "/frame\" -o " << cine_.name
                 << ".mp4 --fps " << fps << "\n";
            std::ofstream cj(dir / "cinematic.json");
            cj << cineToJson(cine_).dump(1);
        }
    }
    if (mode == CineRun::ExportHQ)
    {
        // The HQ photo's state for the whole export (requestPhoto does the same for one frame).
        const float s = std::clamp(std::round(photoScaleSetting), 1.0f, 4.0f);
        cineSavedFullRateKm_ = cv2FullRateAboveKm;
        cineSavedSparse_ = cv2SparseWhenStill;
        cineSavedPointSigma_ = pointSigmaPx;
        cineSavedPointMax_ = pointSigmaMaxPx;
        cineSavedGlare_ = glareSizePx;
        cineHqSettingsSaved_ = true;
        cv2FullRateAboveKm = 0.0f;
        cv2SparseWhenStill = 0.0f;
        pointSigmaPx *= s;
        pointSigmaMaxPx *= s;
        glareSizePx *= s;
        photoScaleActive = (uint32_t)s;
    }
    cineApplyAt(0.0, true);
    cineExportT0_ = glfwGetTime();
    cineStatus_ = mode == CineRun::Play ? "Playing" : "Exporting";
    Log::line("cinematic: start " + cine_.name + " (" + std::to_string(cineFrames_) + " frames)");
}

void SatelliteSim::cineStop(const char *why)
{
    if (!cineActive())
        return;
    if (cineHqSettingsSaved_)
    {
        cv2FullRateAboveKm = cineSavedFullRateKm_;
        cv2SparseWhenStill = cineSavedSparse_;
        pointSigmaPx = cineSavedPointSigma_;
        pointSigmaMaxPx = cineSavedPointMax_;
        glareSizePx = cineSavedGlare_;
        cineHqSettingsSaved_ = false;
        photoScaleActive = 0;
    }
    const bool exported = cineRun_ == CineRun::ExportPreview || cineRun_ == CineRun::ExportHQ;
    cineRun_ = CineRun::Idle;
    cineAccumSubs_ = 0;   // finalizeScreenshot back to plain screenshots
    cineAccum_.clear();
    cineFixedDt_ = 0.0f;
    timePaused = cineSavedPaused_;
    if (exported)
        cineStatus_ = std::string(why) + ": " + std::to_string(cineFrame_) + " frames -> " + cineOutDir_;
    else
        cineStatus_ = why;
    Log::line(std::string("cinematic: stop (") + why + ")");
}

void SatelliteSim::cineTick(float dt)
{
    if (!cineActive())
        return;
    const double dur = cinePlayOneShot_ ? cine_.shots[std::clamp(cineShot_, 0, (int)cine_.shots.size() - 1)].duration()
                                        : cine_.duration();
    if (cineRun_ == CineRun::Play)
    {
        cineT_ += dt;
        if (cineT_ > dur)
        {
            if (!cineLoop_)
            {
                cineApplyAt(dur, false);
                cineStop("Finished");
                return;
            }
            cineT_ = std::fmod(cineT_, std::max(dur, 1e-3));
            cineLastShot_ = -1;
        }
        cineApplyAt(cineT_, false);
        cineScrub_ = (float)cineT_;
        return;
    }
    // Export: one frame per path frame; the copy of the last one must be done before the next pose.
    if (screenshotRequested || screenshotCopyPending)
        return;
    if (cineRun_ == CineRun::ExportHQ && (!ctx_ || !ctx_->photoActive))
        return; // the photo target is not up yet (App switches it after the frame)
    if (cineFrame_ >= cineFrames_)
    {
        cineStop("Exported");
        return;
    }
    // Motion blur: subframe s of N sits at (s + 0.5) / N of a 180-degree shutter centred on the frame's time.
    const int subs = std::max(cineAccumSubs_, 1);
    const double t = (cineFrame_ + (subs > 1 ? 0.5 * ((cineSub_ + 0.5) / subs - 0.5) : 0.0)) / cine_.fps;
    cineApplyAt(std::clamp(t, 0.0, dur), false);
    cineScrub_ = (float)t;
    if (cineRun_ == CineRun::ExportHQ &&
        ++cineSettle_ < std::max(subs > 1 ? 4 : 1, (int)std::lround(photoSettleFrames / (float)subs)))
        return; // settle at this pose (its sim time held: timePaused); the subframes share the settle budget
    // Preview: a shot's first frame has no history (the cut reset it) — pre-roll 16 frames at that pose so
    // it is not a grain of single samples.
    if (cineRun_ == CineRun::ExportPreview && cineLastShot_ != cinePrerollShot_ && ++cineSettle_ < 16)
        return;
    cinePrerollShot_ = cineLastShot_;
    cineSettle_ = 0;
    char nm[32];
    snprintf(nm, sizeof(nm), "frame_%05d.png", cineFrame_);
    screenshotPath = (std::filesystem::path(cineOutDir_) / nm).string();
    screenshotIncludeUI = false;
    screenshotRequested = true;
    if (++cineSub_ < subs)
        return;   // the next subframe of this frame
    cineSub_ = 0;
    ++cineFrame_;
    char buf[96];
    snprintf(buf, sizeof(buf), "Exporting %d / %d", cineFrame_, cineFrames_);
    cineStatus_ = buf;
}

nlohmann::json SatelliteSim::cineLookSettings()
{
    // The look of a shot, not the session: no observer, camera, time, window geometry, keys or audio.
    nlohmann::json all = buildSettingsJson(), out;
    for (const char *k : {"clouds", "clouds_v2", "photometry", "constellations", "planets"})
        if (all.contains(k))
            out[k] = all[k];
    if (all.contains("display"))
        for (const char *k : {"render_scale", "sky_taa", "sky_taa_weight", "sky_taa_weight_moving", "graphics_preset",
                              "debug_disable_mask", "trail_enabled"})
            if (all["display"].contains(k))
                out["display"][k] = all["display"][k];
    return out;
}

// The HQ export's expected wall time: every frame settles "HQ photo settle frames" (+ the copy) at the photo's
// pixel count, so it is about frames x settle x scale^2 window frames at the current frame rate. Rough (the photo
// target's cost is not quite proportional to its pixels), but the right order: the export that looked like a
// freeze was 451 x 48 frames at 4x the pixels, ~20 minutes.
double SatelliteSim::cineEstimateHqS() const
{
    const double fps = std::max((double)fpsBadgeEma, 1.0);
    const double dur = cinePlayOneShot_ && !cine_.shots.empty() ? cine_.shots[std::clamp(cineShot_, 0, (int)cine_.shots.size() - 1)].duration()
                                                                 : cine_.duration();
    const double frames = std::floor(dur * std::clamp((double)cineExportFps_, 1.0, 240.0) + 1e-6) + 1.0;
    const double subs = std::max(1.0, std::round((double)cineBlurSubs_));
    const double perFrame = subs * std::max(subs > 1.0 ? 4.0 : 1.0, std::round(photoSettleFrames / subs)) + 2.0;
    const double s = std::clamp(std::round((double)photoScaleSetting), 1.0, 4.0);
    return frames * perFrame * s * s / fps;
}
