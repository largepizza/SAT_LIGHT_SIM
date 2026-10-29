// SatelliteSimReports.cpp — issue reports (2026-10-12): annotated snapshots, grouped by the issue they show.
//
// An ISSUE is one folder, <user data>/reports/<id>/, that stands on its own (zip it, send it):
//   report.json   format "sat-light-sim-report/1": title, description, category, status, and the SHOTS
//   shot_NN.png   each shot's frame, at the window's resolution
// A SHOT is everything needed to stand where the reporter stood and to read what they saw:
//   record    the perf snapshot record (buildPerfSnapshotJson: view, cloud drift, full settings, timings)
//   state     the harness state sidecar (harnessStateJson: selection, exposure, cloud rate, the Moon, ...)
//   capture   what the temporal passes were doing (histories valid, live render scale, the eye's speed)
//   trail     the camera's last ~12 s at 30 Hz: a motion bug comes with the path that showed it
//   settings_changed_since_previous   what the reporter changed between two shots of one issue
//   note      one line, typed in the bar that opens after the shot
//
// The image is the frame presented right after the key press, through the screenshot path with the UI left
// out. Nothing is paused, settled, reset or re-rendered for it: a capture that waited for the clouds' history
// or the sky TAA to converge would hide the grain, ghosting or pop the shot was taken to show.
//
// Save Snapshot (F9) adds a shot to the active issue (and still appends its record to
// perf_profiles/profile_log.jsonl); New Issue Snapshot (F7) starts another issue. Harness: `report`
// (SatelliteSimHarness.cpp, docs/HARNESS.md).
#include "../FrameProfile.h"
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "clay.h"
#include "UIPalette.h"
#include <GLFW/glfw3.h>

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;
using nlohmann::json;

const char *keyDisplayName(int key); // SatelliteSimUI.cpp

std::string SatelliteSim::reportDir() const
{
    return (fs::path(userDataDir_) / "reports").string();
}

namespace
{
    constexpr const char *kReportFormat = "sat-light-sim-report/1";

    bool readReport(const fs::path &dir, json &j)
    {
        std::ifstream f(dir / "report.json");
        if (!f)
            return false;
        j = json::parse(f, nullptr, false);
        return !j.is_discarded() && j.is_object() && j.contains("shots") && j["shots"].is_array();
    }

    // Through a temporary file: a crash mid-write (they happen) must not cost the shots already in the issue.
    bool writeReport(const fs::path &dir, const json &j)
    {
        std::error_code ec;
        fs::create_directories(dir, ec);
        const fs::path tmp = dir / "report.json.tmp";
        {
            std::ofstream f(tmp);
            if (!f)
                return false;
            f << j.dump(1) << '\n';
            if (!f)
                return false;
        }
        fs::rename(tmp, dir / "report.json", ec);
        return !ec;
    }

    std::string localStamp(const char *fmt)
    {
        const time_t now = time(nullptr);
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        char buf[64];
        strftime(buf, sizeof(buf), fmt, &lt);
        return buf;
    }

    // Every leaf that differs between two settings objects, as "a.b.c": [before, after].
    void diffSettings(const json &a, const json &b, const std::string &path, json &out)
    {
        if (a.is_object() && b.is_object())
        {
            for (auto it = a.begin(); it != a.end(); ++it)
            {
                const std::string p = path.empty() ? it.key() : path + "." + it.key();
                if (b.contains(it.key()))
                    diffSettings(it.value(), b[it.key()], p, out);
                else
                    out[p] = json::array({it.value(), nullptr});
            }
            for (auto it = b.begin(); it != b.end(); ++it)
                if (!a.contains(it.key()))
                    out[path.empty() ? it.key() : path + "." + it.key()] = json::array({nullptr, it.value()});
        }
        else if (a != b)
            out[path] = json::array({a, b});
    }
}

// The camera as it stands: one trail sample.
SatelliteSim::ReportPose SatelliteSim::reportPoseNow() const
{
    ReportPose p;
    p.t = reportClock_;
    p.lat = glm::degrees(std::asin(glm::clamp((double)obsDir.z, -1.0, 1.0)));
    p.lon = glm::degrees(std::atan2((double)obsDir.y, (double)obsDir.x));
    p.alt = obsHeightOffset; // above sea level (in follow mode: the camera's)
    p.az = camera.azDeg;
    p.el = camera.elDeg;
    p.fov = camera.fovYDeg;
    p.simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    return p;
}

// buildUI, every frame, right after the harness and the cinematic have had the camera: the trail, the two
// keys, and a pending shot (which waits here while an earlier screenshot is still encoding).
void SatelliteSim::reportTick(float dt)
{
    FPROF_SCOPE("reportTick");
    // The window's queued New / Delete, then the list itself: all before this frame's layout.
    if (reportOp_.kind == 1)
        reportDeleteIssue(reportOp_.id);
    else if (reportOp_.kind == 2)
        reportDeleteShot(reportOp_.id, reportOp_.shot);
    else if (reportOp_.kind == 3 && reportShotPending_ == 0)
    {
        reportShotPending_ = 2;
        reportPendingBar_ = true;
        reportPendingNote_.clear();
        reportPendingTitle_.clear();
    }
    reportOp_ = ReportOp{};
    if (rpChrome.open && (!reportsLoaded_ || reportsDirty_))
        reportsLoad();

    reportClock_ += (double)dt;
    reportFrameMs_[reportFrameN_++ % 240] = dt * 1000.0f;
    if (reportFrameN_ >= 480)
        reportFrameN_ -= 240;

    if (reportTrail_.empty() || reportClock_ - reportTrail_.back().t >= kReportTrailStepS)
    {
        reportTrail_.push_back(reportPoseNow());
        size_t drop = 0;
        while (drop < reportTrail_.size() && reportTrail_[drop].t < reportClock_ - kReportTrailSeconds)
            ++drop;
        if (drop > 0)
            reportTrail_.erase(reportTrail_.begin(), reportTrail_.begin() + (std::ptrdiff_t)drop);
    }

    if (snapshotKeyPending || reportNewKeyPending)
    {
        if (reportShotPending_ == 0)
        {
            reportShotPending_ = reportNewKeyPending ? 2 : 1;
            reportPendingBar_ = true;
            reportPendingNote_.clear();
            reportPendingTitle_.clear();
        }
        snapshotKeyPending = reportNewKeyPending = false;
    }
    if (reportShotPending_ != 0 && reportTakeShot(reportShotPending_ == 2, reportPendingBar_))
        reportShotPending_ = 0;
}

bool SatelliteSim::reportTakeShot(bool newIssue, bool openBar)
{
    if (userDataDir_.empty())
        return true; // nowhere to write: drop it
    const bool canShoot = ctx_ && ctx_->screenshotSupported;
    if (canShoot && (screenshotEncoding.load() || screenshotCopyPending || screenshotRequested || photoState != 0))
        return false;

    // The note bar's unsaved text belongs to the shot it was typed for, not to this one.
    if (reportBarOpen_ && textEdit_.id != 0)
    {
        if (textEdit_.id == CLAY_ID("ReportBarNote").id)
            reportSetField(reportActiveId_, reportBarShot_, "note", textEdit_.buf);
        else if (textEdit_.id == CLAY_ID("ReportBarTitle").id)
            reportSetField(reportActiveId_, 0, "title", textEdit_.buf);
        if (textEdit_.id == CLAY_ID("ReportBarNote").id || textEdit_.id == CLAY_ID("ReportBarTitle").id)
        {
            textEdit_.id = 0;
            textEdit_.selAll = false;
        }
    }

    // The trail ends on the shot's own pose (its samples are 1/30 s apart, and a harness command may have moved
    // the camera this frame).
    if (!reportTrail_.empty() && reportTrail_.back().t >= reportClock_ - 1e-9)
        reportTrail_.back() = reportPoseNow();
    else
        reportTrail_.push_back(reportPoseNow());

    const float cpuDt = reportFrameMs_[(reportFrameN_ + 239) % 240] / 1000.0f;
    json rec = buildPerfSnapshotJson(cpuDt);

    json rep;
    fs::path dir;
    bool isNew = newIssue || reportActiveId_.empty();
    if (!isNew)
    {
        dir = fs::path(reportDir()) / reportActiveId_;
        if (!readReport(dir, rep))
            isNew = true; // its folder was moved or deleted: start another
    }
    if (isNew)
    {
        std::string id = localStamp("issue_%Y%m%d_%H%M%S");
        std::error_code ec;
        for (int k = 2; fs::exists(fs::path(reportDir()) / id, ec); ++k)
            id = localStamp("issue_%Y%m%d_%H%M%S") + "_" + std::to_string(k);
        dir = fs::path(reportDir()) / id;
        rep = {{"format", kReportFormat},
               {"id", id},
               {"title", reportPendingTitle_.empty() ? localStamp("Issue %Y-%m-%d %H:%M") : reportPendingTitle_},
               {"description", ""},
               {"category", ""},
               {"status", "open"},
               {"created_utc", rec.value("captured_at_utc", std::string())},
               {"app_version", rec.value("app_version", std::string())},
               {"git_commit", rec.value("git_commit", std::string())},
               {"gpu_device", rec.value("gpu_device", std::string())},
               {"shots", json::array()}};
        reportActiveId_ = id;
    }
    else if (!reportPendingTitle_.empty())
        rep["title"] = reportPendingTitle_;

    // A shot's number is its identity (its files, the note bar, the window): never reused after a delete.
    int n = 1;
    for (const json &s : rep["shots"])
        n = std::max(n, s.value("n", 0) + 1);
    char img[32];
    snprintf(img, sizeof(img), "shot_%02d.png", n);

    json shot = {{"n", n},
                 {"note", reportPendingNote_},
                 {"image", canShoot ? json(img) : json(nullptr)},
                 {"captured_at_utc", rec.value("captured_at_utc", std::string())}};

    // What the temporal passes were doing in the captured frame.
    {
        double speed = 0.0, turn = 0.0;
        if (reportTrail_.size() >= 2)
        {
            // Over the last ~0.25 s of the trail.
            const ReportPose &b = reportTrail_.back();
            size_t ia = reportTrail_.size() - 1;
            while (ia > 0 && b.t - reportTrail_[ia].t < 0.25)
                --ia;
            const ReportPose &a = reportTrail_[ia];
            const double span = b.t - a.t;
            if (span > 1e-4)
            {
                auto ecef = [](const ReportPose &p)
                {
                    const double la = glm::radians(p.lat), lo = glm::radians(p.lon), r = 6371000.0 + p.alt;
                    return glm::dvec3(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la)) * r;
                };
                speed = glm::length(ecef(b) - ecef(a)) / span;
                double daz = std::fmod(b.az - a.az + 540.0, 360.0) - 180.0;
                daz *= std::cos(glm::radians(b.el));
                turn = std::sqrt(daz * daz + (b.el - a.el) * (b.el - a.el)) / span;
            }
        }
        shot["capture"] = {{"frame", "the frame presented after the key press; nothing paused, settled or reset; UI left out"},
                           {"image_width", ctx_ ? ctx_->swapExtent.width : 0u},
                           {"image_height", ctx_ ? ctx_->swapExtent.height : 0u},
                           {"cloud_history_valid", cv2HistoryValid},
                           {"sky_taa_history_valid", skyTaaHistValid},
                           {"auto_exposure_ev", autoExposureEV},
                           {"render_scale_now", renderScale},
                           {"ui_visible", uiVisible},
                           {"eye_speed_mps", speed},
                           {"view_turn_deg_per_s", turn}};
    }
    // Frame times over the last 240 frames (wall clock, ms).
    {
        const int cnt = std::min(reportFrameN_, 240);
        std::vector<float> v(reportFrameMs_, reportFrameMs_ + cnt);
        if (!v.empty())
        {
            std::sort(v.begin(), v.end());
            double sum = 0.0;
            for (float x : v)
                sum += x;
            shot["frame_ms"] = {{"frames", cnt},
                                {"mean", sum / cnt},
                                {"min", v.front()},
                                {"median", v[(size_t)cnt / 2]},
                                {"p95", v[std::min((size_t)cnt - 1, (size_t)(cnt * 0.95))]},
                                {"max", v.back()}};
        }
    }
    // The camera's path up to the shot (t <= 0 s, the shot at 0): harness `report load ... trail=on` turns it
    // into the current camera path.
    {
        json rows = json::array();
        for (const ReportPose &p : reportTrail_)
            rows.push_back({p.t - reportClock_, p.lat, p.lon, p.alt, p.az, p.el, p.fov, p.simT});
        shot["trail"] = {{"columns", {"t_s", "lat_deg", "lon_deg", "alt_m", "az_deg", "el_deg", "fov_y_deg", "sim_t_j2000_s"}},
                         {"rows", rows}};
    }
    if (!rep["shots"].empty())
    {
        const json &prev = rep["shots"].back();
        if (prev.contains("record") && prev["record"].contains("settings") && rec.contains("settings"))
        {
            json d = json::object();
            for (auto it = rec["settings"].begin(); it != rec["settings"].end(); ++it)
            {
                // The view and the clock are the shot itself, not a setting the reporter changed.
                if (it.key() == "camera" || it.key() == "observer" || it.key() == "time")
                    continue;
                if (prev["record"]["settings"].contains(it.key()))
                    diffSettings(prev["record"]["settings"][it.key()], it.value(), it.key(), d);
            }
            shot["settings_changed_since_previous"] = d;
        }
    }
    shot["state"] = harnessStateJson();
    shot["record"] = rec;
    rep["shots"].push_back(std::move(shot));
    rep["updated_utc"] = rec.value("captured_at_utc", std::string());

    const std::string title = rep.value("title", std::string());
    if (!writeReport(dir, rep))
    {
        snprintf(screenshotToastText, sizeof(screenshotToastText), tx(Txt::ToastCouldNotWriteTheIssueReport), dir.string().c_str());
        screenshotToastTimer = 4.0f;
        screenshotToastWarn = true;
        return true;
    }
    // The old log keeps growing too (tools/perf_analysis reads it), each line naming its issue and shot.
    rec["record_kind"] = "snapshot";
    rec["report"] = {{"id", reportActiveId_}, {"shot", n}};
    appendPerfRecord(rec);

    if (canShoot)
    {
        screenshotPath = (dir / img).string();
        char th[40];
        snprintf(th, sizeof(th), "shot_%02d_thumb.png", n);
        reportThumbPath_ = (dir / th).string(); // the window's card image, from the same frame
        screenshotIncludeUI = reportShotUiSetting;    // the Display tab's "Issue report shots include the UI"
        screenshotHideOverlays_ = reportShotUiSetting; // ...but never the note bar / reports window / toast
        screenshotRequested = true; // THIS frame, as it is (recordScreenshotCopy after the render pass)
    }
    snprintf(screenshotToastText, sizeof(screenshotToastText), tx(Txt::ToastShotSaved), title.c_str(), n);
    screenshotToastTimer = 2.5f;
    screenshotToastWarn = false;
    Log::line("report: " + reportActiveId_ + " shot " + std::to_string(n) + (isNew ? " (new issue)" : ""));
    reportsDirty_ = true;

    // A NEW issue opens the Issue reports window on it: its title, description and the shot's note are all
    // there (nothing is focused, so movement keys keep working). With the HUD hidden the bar stands in.
    const bool openWindow = isNew && openBar && !showIntro && uiVisible;
    if (openWindow)
    {
        rpChrome.open = true;
        reportViewId_ = reportActiveId_;
        reportViewShot_ = n;
        reportStatus_.clear();
        reportBarOpen_ = false;
        reportsLoad(); // (reportTick runs before any layout)
    }
    if (openBar && !showIntro && !openWindow)
    {
        // Moving or looking when the key was pressed: the bar opens WITHOUT the keyboard (the held W would
        // type into it and movement would stop). Enter or a click focuses the note.
        bool busyHands = camera.captured;
        if (win)
        {
            for (int k : {GLFW_KEY_W, GLFW_KEY_A, GLFW_KEY_S, GLFW_KEY_D, keybindings[KB_RAISE_ELEV].key,
                          keybindings[KB_LOWER_ELEV].key, keybindings[KB_MOVE_BOOST].key})
                busyHands = busyHands || (k > 0 && glfwGetKey(win, k) == GLFW_PRESS);
            busyHands = busyHands || glfwGetMouseButton(win, GLFW_MOUSE_BUTTON_RIGHT) == GLFW_PRESS;
        }
        reportBarOpen_ = true;
        reportBarFocusPending_ = !busyHands;
        reportBarCloseOnCommit_ = false;
        reportBarTimer_ = 10.0f;
        reportBarShot_ = n;
        reportBarTitle_ = title;
        reportBarNote_ = reportPendingNote_;
    }
    reportPendingNote_.clear();
    reportPendingTitle_.clear();
    return true;
}

// One text field of an issue (shot 0) or of one of its shots (1-based), rewritten in report.json.
void SatelliteSim::reportSetField(const std::string &id, int shot, const char *key, const std::string &valueIn)
{
    if (id.empty())
        return;
    // Whatever the caller holds, a field never grows past its limit.
    const std::string k0 = key;
    const size_t cap = k0 == "title" ? kReportTitleMax : k0 == "note" ? kReportNoteMax : k0 == "description" ? kReportDescMax : 64;
    const std::string value = valueIn.substr(0, cap);
    const fs::path dir = fs::path(reportDir()) / id;
    json rep;
    if (!readReport(dir, rep))
        return;
    if (shot <= 0)
        rep[key] = value;
    else
    {
        bool found = false;
        for (json &s : rep["shots"])
            if (s.value("n", 0) == shot)
            {
                s[key] = value;
                found = true;
            }
        if (!found)
            return;
    }
    writeReport(dir, rep);
    // The window's copy (same strings; no element moves).
    if (ReportIssueInfo *is = reportFind(id))
    {
        const std::string k = key;
        if (shot <= 0)
        {
            if (k == "title") is->title = value;
            else if (k == "description") is->description = value;
            else if (k == "category") is->category = value;
            else if (k == "status") is->status = value;
            snprintf(is->line, sizeof(is->line), tx(Txt::FmtShot3), (int)is->shots.size(), is->shots.size() == 1 ? "" : "s",
                     is->category.empty() ? "uncategorised" : is->category.c_str(), is->status.c_str());
        }
        else
            for (ReportShotInfo &s : is->shots)
                if (s.n == shot && k == "note")
                    s.note = value;
    }
}

// onKey, ahead of the text field: with the bar up, the two snapshot keys take their shot even while a field of
// it has the keyboard (the unsaved text goes to the shot it was typed for: reportTakeShot), Enter / Esc in a
// field close the bar, and Enter with the bar unfocused focuses the note.
bool SatelliteSim::reportBarKey(int key, int action)
{
    if (!reportBarOpen_ || action != GLFW_PRESS)
        return false;
    const bool barFocus = textEdit_.id != 0 && (textEdit_.id == CLAY_ID("ReportBarNote").id || textEdit_.id == CLAY_ID("ReportBarTitle").id);
    if (barFocus)
    {
        if (key == keybindings[KB_SAVE_SNAPSHOT].key)
        {
            snapshotKeyPending = true;
            return true;
        }
        if (key == keybindings[KB_REPORT_NEW].key)
        {
            reportNewKeyPending = true;
            return true;
        }
        // (Shift+Enter in the note is a new line, not the end of the edit.)
        const bool shift = win && (glfwGetKey(win, GLFW_KEY_LEFT_SHIFT) == GLFW_PRESS || glfwGetKey(win, GLFW_KEY_RIGHT_SHIFT) == GLFW_PRESS);
        const bool newLine = shift && textEdit_.id == CLAY_ID("ReportBarNote").id && key != GLFW_KEY_ESCAPE;
        if ((key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER || key == GLFW_KEY_ESCAPE) && !newLine)
            reportBarCloseOnCommit_ = true; // textEditKey commits / cancels next
        return false;
    }
    if (textEdit_.id == 0 && !consoleOpen_ && (key == GLFW_KEY_ENTER || key == GLFW_KEY_KP_ENTER))
    {
        reportBarFocusPending_ = true;
        return true;
    }
    if (textEdit_.id == 0 && !consoleOpen_ && key == GLFW_KEY_ESCAPE)
    {
        reportBarOpen_ = false; // (capturesKeyboard keeps this Esc from quitting the app)
        return true;
    }
    return false;
}

// The bar that opens after a shot: the issue's title and this shot's note. The note has the keyboard unless the
// player was moving or looking; a right-click (look) or a click elsewhere gives the keyboard back and leaves the
// bar up for a few seconds (Enter or a click returns to the note). Enter saves and closes, Esc closes.
void SatelliteSim::buildReportBar(const UIInput &inp, UIRenderer &ui)
{
    if (!reportBarOpen_)
        return;
    const Clay_ElementId noteId = CLAY_ID("ReportBarNote"), titleId = CLAY_ID("ReportBarTitle");
    if (reportBarFocusPending_)
    {
        reportBarFocusPending_ = false;
        textEditFocus(noteId.id, reportBarNote_);
        textEdit_.selAll = false; // (the caret at the end: a first key must not replace a note already there)
    }
    auto text = [&](const char *s, Clay_Color c, float size)
    {
        CLAY_TEXT((Clay_String{false, (int32_t)strlen(s), s}), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs((int)size), .wrapMode = CLAY_TEXT_WRAP_NONE}));
    };
    const float w = std::min(640.0f * std::max(1.0f, uiScale), inp.screenW - 40.0f);
    const float labelW = (float)fs(11) * 3.2f, fieldW = w - 24.0f - labelW - 8.0f;
    static char shotBuf[32], hintBuf[224];
    snprintf(shotBuf, sizeof(shotBuf), tx(Txt::FmtShot4), reportBarShot_);
    {
        char k1[24], k2[24]; // keyDisplayName's letters rotate through a small pool: copy
        snprintf(k1, sizeof(k1), "%s", keyDisplayName(keybindings[KB_SAVE_SNAPSHOT].key));
        snprintf(k2, sizeof(k2), "%s", keyDisplayName(keybindings[KB_REPORT_NEW].key));
        const bool typing = textEdit_.id == noteId.id || textEdit_.id == titleId.id || reportBarFocusPending_;
        snprintf(hintBuf, sizeof(hintBuf), tx(Txt::FmtAnotherShotOfThisIssueA),
                 typing ? "Enter saves, Shift+Enter adds a line, Esc closes, right-click to keep moving." : "Enter or click to type a note.", k1, k2);
    }
    CLAY(CLAY_ID("ReportBar"), {.layout = {.sizing = {CLAY_SIZING_FIXED(w), CLAY_SIZING_FIT(0)},
                                           .padding = {12, 12, 8, 8},
                                           .childGap = 5,
                                           .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                .backgroundColor = Pal::popupBg,
                                .cornerRadius = CLAY_CORNER_RADIUS(6),
                                .floating = {.offset = {0, 56}, .zIndex = 44, .attachPoints = {.element = CLAY_ATTACH_POINT_CENTER_TOP, .parent = CLAY_ATTACH_POINT_CENTER_TOP}, .attachTo = CLAY_ATTACH_TO_ROOT},
                                .border = {.color = Pal::popupBorder, .width = CLAY_BORDER_ALL(1)}})
    {
        CLAY(CLAY_ID("ReportBarRow1"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                   .childGap = 8,
                                                   .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                   .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_ID("ReportBarL1"), {.layout = {.sizing = {CLAY_SIZING_FIXED(labelW), CLAY_SIZING_FIT(0)}}}) { text(tx(Txt::NoteIssue), Pal::textDim, 11); }
            if (textField(inp, ui, titleId, reportBarTitle_, fieldW - (float)fs(11) * 5.0f, 12, "The issue's title (shared by its shots)", kReportTitleMax))
                reportSetField(reportActiveId_, 0, "title", reportBarTitle_);
            text(shotBuf, Pal::volValue, 11);
        }
        CLAY(CLAY_ID("ReportBarRow2"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                   .childGap = 8,
                                                   .layoutDirection = CLAY_LEFT_TO_RIGHT}})
        {
            CLAY(CLAY_ID("ReportBarL2"), {.layout = {.sizing = {CLAY_SIZING_FIXED(labelW), CLAY_SIZING_FIT(0)}, .padding = {0, 0, 5, 0}}}) { text(tx(Txt::NoteNote), Pal::textDim, 11); }
            // Two lines that grow to six, then scroll; Enter saves (Shift+Enter: a new line).
            if (textArea(inp, ui, noteId, reportBarNote_, fieldW, 2, 12, "What this shot shows", kReportNoteMax, 6, true, nullptr, 44))
                reportSetField(reportActiveId_, reportBarShot_, "note", reportBarNote_);
        }
        text(hintBuf, Pal::textHint, 11);
    }
    if (const Clay_ElementData d = Clay_GetElementData(CLAY_ID("ReportBar")); d.found)
        ui.addMouseCaptureRect(d.boundingBox.x, d.boundingBox.y, d.boundingBox.width, d.boundingBox.height);

    // Looking around (right mouse button) takes the keyboard back, keeping what was typed.
    bool focus = textEdit_.id == noteId.id || textEdit_.id == titleId.id;
    if (focus && (inp.rmbPressed || camera.captured))
    {
        textEditCommit();
        focus = false;
    }
    const bool pending = textEdit_.doneId == noteId.id || textEdit_.doneId == titleId.id;
    if (focus)
        reportBarTimer_ = 10.0f;
    else if (!pending && !reportBarFocusPending_)
    {
        // Unfocused: gone at once after Enter / Esc, else after a few seconds (a commit the field has yet to
        // pick up keeps it one more frame).
        reportBarTimer_ -= inp.dt;
        if (reportBarCloseOnCommit_ || reportBarTimer_ <= 0.0f)
            reportBarOpen_ = false;
    }
}

// For SatelliteSimReportsWindow.cpp (the outbox).
namespace satreport
{
    bool read(const fs::path &dir, json &j) { return readReport(dir, j); }
    bool write(const fs::path &dir, const json &j) { return writeReport(dir, j); }
}
