// SatelliteSimReportsWindow.cpp — the outbox (2026-10-12, step 2 of the issue reports): the Issue reports
// window. A grid of cards, one per issue under <user data>/reports/ (the first shot's thumbnail, title, shots,
// category, status); a card opens the issue: title, category, status, a multi-line description, its shots (note,
// Go, delete) and the selected shot's image, on which a click drops the PIN — where in the frame the problem is.
//
// The window works from reportIssues_, a light copy of the report.json files (SatelliteSimReports.cpp writes
// them; every edit here goes through reportSetField / the functions below, which rewrite the file and patch the
// copy). Images: one atlas registered once with the UI — a 1024 x 576 preview of the selected shot plus 96
// thumbnail cells handed out on demand (least recently drawn first). A shot's thumbnail file is written with its
// PNG (reportWriteThumb, from the same frame); an older shot's is made from the PNG the first time it is shown.
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "../Harness.h"
#include "clay.h"
#include "UIPalette.h"
#include "stb_image.h"
#include "stb_image_write.h" // the implementation lives in UIRenderer.cpp

#include <nlohmann/json.hpp>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

namespace fs = std::filesystem;
using nlohmann::json;

const char *keyDisplayName(int key); // SatelliteSimUI.cpp
namespace satreport
{
    bool read(const fs::path &dir, json &j); // SatelliteSimReports.cpp
    bool write(const fs::path &dir, const json &j);
}

#ifdef _WIN32
extern "C" __declspec(dllimport) void *__stdcall ShellExecuteA(void *, const char *, const char *, const char *, const char *, int);
#endif

namespace
{
    constexpr const char *kCategories[] = {"clouds", "terrain", "sea", "sky", "lighting", "satellites", "performance", "ui", "other"};
    constexpr const char *kCategoryLabels[] = {"Clouds", "Terrain", "Sea", "Sky", "Lighting", "Satellites", "Perf", "UI", "Other"};
    constexpr const char *kStatuses[] = {"open", "sent", "fixed", "closed"};
    constexpr const char *kStatusLabels[] = {"Open", "Sent", "Fixed", "Closed"};

    void openFolder(const std::string &path)
    {
#ifdef _WIN32
        ShellExecuteA(nullptr, "open", path.c_str(), nullptr, nullptr, 1);
#elif defined(__APPLE__)
        const std::string cmd = "open \"" + path + "\" &";
        (void)std::system(cmd.c_str());
#else
        const std::string cmd = "xdg-open \"" + path + "\" >/dev/null 2>&1 &";
        (void)std::system(cmd.c_str());
#endif
    }

    // A box filter of the source rect (sx0, sy0, cw, ch) into dw x dh (nearest where it magnifies). RGBA8.
    void resampleBox(const uint8_t *src, int w, int h, int sx0, int sy0, int cw, int ch, uint8_t *dst, int dw, int dh)
    {
        for (int y = 0; y < dh; ++y)
            for (int x = 0; x < dw; ++x)
            {
                const int x0 = sx0 + (int)((int64_t)x * cw / dw), x1 = std::max(x0 + 1, sx0 + (int)((int64_t)(x + 1) * cw / dw));
                const int y0 = sy0 + (int)((int64_t)y * ch / dh), y1 = std::max(y0 + 1, sy0 + (int)((int64_t)(y + 1) * ch / dh));
                uint32_t acc[3] = {0, 0, 0}, n = 0;
                for (int yy = y0; yy < y1 && yy < h; ++yy)
                    for (int xx = x0; xx < x1 && xx < w; ++xx, ++n)
                        for (int c = 0; c < 3; ++c)
                            acc[c] += src[((size_t)yy * w + xx) * 4 + c];
                uint8_t *d = &dst[((size_t)y * dw + x) * 4];
                for (int c = 0; c < 3; ++c)
                    d[c] = (uint8_t)(n ? (acc[c] + n / 2) / n : 0);
                d[3] = 255;
            }
    }

    // The centred 16:9 crop of a frame, as a thumbnail.
    void makeThumb(const uint8_t *src, int w, int h, std::vector<uint8_t> &out, int tw, int th)
    {
        int cw = w, ch = h;
        if ((double)w / h > (double)tw / th)
            cw = (int)std::lround(h * (double)tw / th);
        else
            ch = (int)std::lround(w * (double)th / tw);
        out.assign((size_t)tw * th * 4, 0);
        resampleBox(src, w, h, (w - cw) / 2, (h - ch) / 2, cw, ch, out.data(), tw, th);
    }

    std::string cropName(const std::string &image)
    {
        const size_t dot = image.rfind('.');
        return (dot == std::string::npos ? image : image.substr(0, dot)) + "_crop.png";
    }

    std::string thumbName(const std::string &image)
    {
        const size_t dot = image.rfind('.');
        return (dot == std::string::npos ? image : image.substr(0, dot)) + "_thumb.png";
    }

    void cellOrigin(int c, int &x, int &y, int thumbW, int thumbH, int prevW, int prevH)
    {
        if (c < 16)
        {
            x = prevW + (c % 4) * thumbW;
            y = (c / 4) * thumbH;
        }
        else
        {
            c -= 16;
            x = (c % 8) * thumbW;
            y = prevH + (c / 8) * thumbH;
        }
    }

    // Whether an element's box (last frame's) is inside its scroll view's: only thumbnails on screen hold atlas
    // cells. Every card is laid out whether scrolled to or not, so with more issues than cells "drawn last frame"
    // covered them all, and the cards past the cells' count were marked "No image" for good (2026-10-27).
    bool rpBoxVisible(Clay_ElementId el, Clay_ElementId view)
    {
        const Clay_ElementData e = Clay_GetElementData(el), v = Clay_GetElementData(view);
        if (!e.found || !v.found)
            return true; // its first frame: not known yet
        const Clay_BoundingBox &a = e.boundingBox, &b = v.boundingBox;
        return a.x < b.x + b.width && a.x + a.width > b.x && a.y < b.y + b.height && a.y + a.height > b.y;
    }

    UIImage atlasRect(uint32_t uiId, int x, int y, int w, int h, int atlas)
    {
        UIImage im;
        im.imageId = uiId;
        im.u0 = (x + 0.5f) / atlas; // half a texel in: linear filtering never reads the neighbour
        im.v0 = (y + 0.5f) / atlas;
        im.u1 = (x + w - 0.5f) / atlas;
        im.v1 = (y + h - 0.5f) / atlas;
        return im;
    }
}

SatelliteSim::ReportIssueInfo *SatelliteSim::reportFind(const std::string &id)
{
    for (ReportIssueInfo &is : reportIssues_)
        if (is.id == id)
            return &is;
    return nullptr;
}

// ─── The list ────────────────────────────────────────────────────────────────
void SatelliteSim::reportsLoad()
{
    reportsLoaded_ = true;
    reportsDirty_ = false;
    reportIssues_.clear();
    std::error_code ec;
    std::vector<fs::path> dirs;
    for (const auto &e : fs::directory_iterator(reportDir(), ec))
        if (e.is_directory(ec))
            dirs.push_back(e.path());
    std::sort(dirs.begin(), dirs.end(), [](const fs::path &a, const fs::path &b) { return a.filename() > b.filename(); }); // newest first
    for (const fs::path &d : dirs)
    {
        json rep;
        if (!satreport::read(d, rep))
            continue;
        ReportIssueInfo is;
        is.id = d.filename().string();
        // (A report may come from anyone: its texts are cut to what the app itself would keep.)
        is.title = rep.value("title", std::string()).substr(0, kReportTitleMax);
        is.description = rep.value("description", std::string()).substr(0, kReportDescMax);
        is.category = rep.value("category", std::string());
        is.status = rep.value("status", std::string("open"));
        is.resolution = rep.contains("resolution") && rep["resolution"].is_string() ? rep["resolution"].get<std::string>().substr(0, kReportDescMax) : std::string();
        {
            auto str = [&](const char *k) { return rep.contains(k) && rep[k].is_string() ? rep[k].get<std::string>() : std::string(); };
            const std::string commit = str("resolution_commit"), by = str("resolved_by"), when = str("resolved_utc"), claim = str("claimed_by");
            if (!is.resolution.empty())
                snprintf(is.resLine, sizeof(is.resLine), "%s%s%s%s%s%s", commit.empty() ? "Resolved" : "Resolved in ", commit.c_str(),
                         by.empty() ? "" : " by ", by.c_str(), when.empty() ? "" : ", ", when.c_str());
            else if (!claim.empty())
                snprintf(is.resLine, sizeof(is.resLine), "Being looked at by %s", claim.c_str());
        }
        for (const json &s : rep["shots"])
        {
            ReportShotInfo sh;
            sh.n = s.value("n", 0);
            sh.note = s.value("note", std::string()).substr(0, kReportNoteMax);
            if (s.contains("image") && s["image"].is_string())
                sh.image = s["image"].get<std::string>();
            if (s.contains("capture"))
            {
                sh.imgW = s["capture"].value("image_width", 0);
                sh.imgH = s["capture"].value("image_height", 0);
            }
            if (s.contains("pin") && s["pin"].is_object())
            {
                sh.hasPin = true;
                sh.pinX = s["pin"].value("x", 0.5f);
                sh.pinY = s["pin"].value("y", 0.5f);
            }
            if (s.contains("rect") && s["rect"].is_object())
            {
                sh.hasRect = true;
                sh.rectX = std::clamp(s["rect"].value("x", 0.0f), 0.0f, 1.0f);
                sh.rectY = std::clamp(s["rect"].value("y", 0.0f), 0.0f, 1.0f);
                sh.rectW = std::clamp(s["rect"].value("w", 0.0f), 0.0f, 1.0f - sh.rectX);
                sh.rectH = std::clamp(s["rect"].value("h", 0.0f), 0.0f, 1.0f - sh.rectY);
            }
            if (s.contains("record"))
            {
                const json &rec = s["record"];
                if (rec.contains("sim_time"))
                    snprintf(sh.meta[0], sizeof(sh.meta[0]), "%.16s UTC", rec["sim_time"].value("utc", std::string("-")).c_str());
                if (rec.contains("observer"))
                {
                    const double lat = rec["observer"].value("lat_deg", 0.0), lon = rec["observer"].value("lon_deg", 0.0);
                    const double alt = rec["observer"].value("height_offset_m", 0.0);
                    char a[24];
                    if (alt >= 1000.0)
                        snprintf(a, sizeof(a), "%.1f km", alt / 1000.0);
                    else
                        snprintf(a, sizeof(a), "%.0f m", alt);
                    snprintf(sh.meta[1], sizeof(sh.meta[1]), "%.2f %c  %.2f %c  %s", std::abs(lat), lat >= 0.0 ? 'N' : 'S', std::abs(lon),
                             lon >= 0.0 ? 'E' : 'W', a);
                }
            }
            // A thumbnail this shot already has in the atlas.
            const std::string key = is.id + "/" + std::to_string(sh.n);
            for (int c = 0; c < kRpCells; ++c)
                if (reportCells_[c].key == key)
                {
                    int x, y;
                    cellOrigin(c, x, y, kRpThumbW, kRpThumbH, kRpPrevW, kRpPrevH);
                    sh.cell = c;
                    sh.img = atlasRect(rpAtlasUiId, x, y, kRpThumbW, kRpThumbH, kRpAtlas);
                }
            is.shots.push_back(std::move(sh));
        }
        snprintf(is.line, sizeof(is.line), "%d shot%s   %s   %s", (int)is.shots.size(), is.shots.size() == 1 ? "" : "s",
                 is.category.empty() ? "uncategorised" : is.category.c_str(), is.status.c_str());
        reportIssues_.push_back(std::move(is));
    }
    if (!reportViewId_.empty() && !reportFind(reportViewId_))
        reportViewId_.clear();
}

// ─── The field being edited ─────────────────────────────────────────────────────
// A text box applies its commit when it is next drawn (textEdit_.doneId). A click on Back, another issue's card,
// Export or the window's close commits the field, but the next frame does not draw it, so the commit was dropped
// (textEditEndFrame) and the typed description / note was lost; an export taken the same frame missed it too
// (issue 20261027_184917). Each report field names itself while drawn; this saves what it holds.
void SatelliteSim::reportNoteEditTarget(uint32_t id, const std::string &issue, int shot, const char *field, size_t maxLen, bool trim)
{
    if (textEdit_.id != id && textEdit_.doneId != id)
        return;
    reportEditTarget_ = ReportEditTarget{id, issue, field, shot, maxLen, trim};
}

void SatelliteSim::reportFlushEdit()
{
    ReportEditTarget &t = reportEditTarget_;
    if (t.id == 0 || t.issue.empty())
        return;
    std::string v;
    if (textEdit_.id == t.id)
    {
        v = textEdit_.buf;
        textEdit_.id = 0;
        textEdit_.selAll = false;
        textEdit_.anchor = -1;
        taDragging_ = false;
    }
    else if (textEdit_.doneId == t.id)
    {
        v = textEdit_.doneBuf;
        textEdit_.doneId = 0;
    }
    else
        return;
    const ReportEditTarget tt = t;
    t = ReportEditTarget{};
    v = v.substr(0, tt.maxLen);
    if (tt.trim)
        while (!v.empty() && (v.back() == '\n' || v.back() == ' '))
            v.pop_back();
    // Unchanged text writes nothing (as the field itself would not).
    if (const ReportIssueInfo *is = reportFind(tt.issue))
    {
        const std::string *cur = nullptr;
        if (tt.field == "title")
            cur = &is->title;
        else if (tt.field == "description")
            cur = &is->description;
        else
            for (const ReportShotInfo &s : is->shots)
                if (s.n == tt.shot)
                    cur = &s.note;
        if (cur && *cur == v)
            return;
    }
    reportSetField(tt.issue, tt.shot, tt.field.c_str(), v);
}

// ─── Images ──────────────────────────────────────────────────────────────────
// finalizeScreenshot: the frame a shot was taken from, as its card's thumbnail (written here, on the main
// thread, so it is on disk before the window can ask for it; the full PNG is still encoding).
void SatelliteSim::reportWriteThumb(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
{
    const std::string path = reportThumbPath_;
    reportThumbPath_.clear();
    if (path.empty() || w == 0 || h == 0 || rgba.size() < (size_t)w * h * 4)
        return;
    std::vector<uint8_t> t;
    makeThumb(rgba.data(), (int)w, (int)h, t, kRpThumbW, kRpThumbH);
    stbi_write_png(path.c_str(), kRpThumbW, kRpThumbH, 4, t.data(), kRpThumbW * 4);
}

bool SatelliteSim::reportEnsureAtlas(UIRenderer &ui)
{
    if (rpAtlasImg != VK_NULL_HANDLE)
        return rpAtlasUiId != 0;
    if (!ctx_)
        return false;
    VulkanContext &ctx = *ctx_;
    ctx.createImage(kRpAtlas, kRpAtlas, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, rpAtlasImg,
                    rpAtlasMem);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = rpAtlasImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vi, nullptr, &rpAtlasView);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &si, nullptr, &rpAtlasSampler);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, rpAtlasImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue clear{{0.01f, 0.01f, 0.012f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, rpAtlasImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ctx.imageBarrier(cmd, rpAtlasImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    rpAtlasUiId = ui.registerImage(ctx.device, rpAtlasView, rpAtlasSampler);
    if (rpAtlasUiId == 0)
        Log::line("reports: no free UI image slot for the atlas; the window shows no images");
    reportPreviewImg_ = atlasRect(rpAtlasUiId, 0, 0, kRpPrevW, kRpPrevH, kRpAtlas);
    return rpAtlasUiId != 0;
}

void SatelliteSim::reportUpload(int x, int y, int w, int h, const uint8_t *rgba)
{
    if (!ctx_ || rpAtlasImg == VK_NULL_HANDLE)
        return;
    VulkanContext &ctx = *ctx_;
    const VkDeviceSize bytes = (VkDeviceSize)w * h * 4;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stage, stageMem);
    void *mapped = nullptr;
    vkMapMemory(ctx.device, stageMem, 0, bytes, 0, &mapped);
    memcpy(mapped, rgba, (size_t)bytes);
    vkUnmapMemory(ctx.device, stageMem);
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, rpAtlasImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {x, y, 0};
    region.imageExtent = {(uint32_t)w, (uint32_t)h, 1};
    vkCmdCopyBufferToImage(cmd, stage, rpAtlasImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ctx.imageBarrier(cmd, rpAtlasImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, stage, nullptr);
    vkFreeMemory(ctx.device, stageMem, nullptr);
}

// buildUI (window open), before the window is laid out: at most ONE image file is read per frame — the open
// issue's preview first, then a thumbnail the window wants and does not have.
void SatelliteSim::reportThumbTick(UIRenderer &ui)
{
    if (!rpChrome.open || !reportEnsureAtlas(ui))
        return;
    ++reportCellClock_;
    // The full PNGs are written by the screenshot thread: never read one while a capture is in flight.
    const bool pngBusy = screenshotEncoding.load() || screenshotCopyPending || screenshotRequested;
    ReportIssueInfo *view = reportViewId_.empty() ? nullptr : reportFind(reportViewId_);

    if (view)
    {
        ReportShotInfo *sel = nullptr;
        for (ReportShotInfo &s : view->shots)
            if (s.n == reportViewShot_)
                sel = &s;
        if (!sel && !view->shots.empty())
        {
            sel = &view->shots.front();
            reportViewShot_ = sel->n;
        }
        const std::string key = sel ? view->id + "/" + std::to_string(sel->n) : std::string();
        if (key != reportPreviewKey_ && !pngBusy)
        {
            reportPreviewKey_ = key;
            reportPreviewOk_ = false;
            if (sel && !sel->image.empty())
            {
                int w = 0, h = 0, n = 0;
                stbi_uc *px = stbi_load((fs::path(reportDir()) / view->id / sel->image).string().c_str(), &w, &h, &n, 4);
                if (px)
                {
                    // Stretched to the region; the window draws it back at the frame's own aspect.
                    std::vector<uint8_t> p((size_t)kRpPrevW * kRpPrevH * 4);
                    resampleBox(px, w, h, 0, 0, w, h, p.data(), kRpPrevW, kRpPrevH);
                    stbi_image_free(px);
                    reportUpload(0, 0, kRpPrevW, kRpPrevH, p.data());
                    sel->imgW = w;
                    sel->imgH = h;
                    reportPreviewOk_ = true;
                }
            }
            return;
        }
    }

    auto want = [&](ReportIssueInfo &is, ReportShotInfo &s) -> bool
    {
        if (s.cell >= 0 || s.thumbTried || s.image.empty() || !s.onScreen)
            return false;
        // A cell first: this shot's own, a free one, else the least recently drawn (never one drawn last frame).
        // With none free the shot waits (it used to be marked tried: "No image" until the next reload).
        int cell = -1;
        for (int c = 0; c < kRpCells && cell < 0; ++c)
            if (reportCells_[c].key.empty())
                cell = c;
        if (cell < 0)
        {
            uint64_t oldest = reportCellClock_ - 1;
            for (int c = 0; c < kRpCells; ++c)
                if (reportCells_[c].used < oldest)
                {
                    oldest = reportCells_[c].used;
                    cell = c;
                }
            if (cell < 0)
                return false; // every cell is on screen
        }
        const fs::path dir = fs::path(reportDir()) / is.id;
        const fs::path thumb = dir / thumbName(s.image), full = dir / s.image;
        std::error_code ec;
        std::vector<uint8_t> cellPx;
        if (fs::exists(thumb, ec))
        {
            int w = 0, h = 0, n = 0;
            stbi_uc *px = stbi_load(thumb.string().c_str(), &w, &h, &n, 4);
            if (px)
            {
                cellPx.resize((size_t)kRpThumbW * kRpThumbH * 4);
                resampleBox(px, w, h, 0, 0, w, h, cellPx.data(), kRpThumbW, kRpThumbH);
                stbi_image_free(px);
            }
        }
        else if (pngBusy)
            return true; // its PNG may be the one being written: look again next frame
        else if (fs::exists(full, ec))
        {
            // A shot from before thumbnails (or a copied report): make it from the PNG, once.
            int w = 0, h = 0, n = 0;
            stbi_uc *px = stbi_load(full.string().c_str(), &w, &h, &n, 4);
            if (px)
            {
                makeThumb(px, w, h, cellPx, kRpThumbW, kRpThumbH);
                stbi_image_free(px);
                stbi_write_png(thumb.string().c_str(), kRpThumbW, kRpThumbH, 4, cellPx.data(), kRpThumbW * 4);
            }
        }
        s.thumbTried = true;
        if (cellPx.empty())
            return true;
        const std::string key = is.id + "/" + std::to_string(s.n);
        if (!reportCells_[cell].key.empty())
        {
            for (ReportIssueInfo &o : reportIssues_)
                for (ReportShotInfo &os : o.shots)
                    if (os.cell == cell)
                    {
                        os.cell = -1;
                        os.thumbTried = false;
                    }
        }
        int x, y;
        cellOrigin(cell, x, y, kRpThumbW, kRpThumbH, kRpPrevW, kRpPrevH);
        reportUpload(x, y, kRpThumbW, kRpThumbH, cellPx.data());
        reportCells_[cell].key = key;
        reportCells_[cell].used = reportCellClock_;
        s.cell = cell;
        s.img = atlasRect(rpAtlasUiId, x, y, kRpThumbW, kRpThumbH, kRpAtlas);
        return true;
    };
    if (view)
    {
        for (ReportShotInfo &s : view->shots)
            if (want(*view, s))
                return;
    }
    else
        for (ReportIssueInfo &is : reportIssues_)
            if (!is.shots.empty() && want(is, is.shots.front()))
                return;
}

void SatelliteSim::reportsDestroy(VkDevice device)
{
    if (rpAtlasSampler)
        vkDestroySampler(device, rpAtlasSampler, nullptr);
    if (rpAtlasView)
        vkDestroyImageView(device, rpAtlasView, nullptr);
    if (rpAtlasImg)
        vkDestroyImage(device, rpAtlasImg, nullptr);
    if (rpAtlasMem)
        vkFreeMemory(device, rpAtlasMem, nullptr);
    rpAtlasSampler = VK_NULL_HANDLE;
    rpAtlasView = VK_NULL_HANDLE;
    rpAtlasImg = VK_NULL_HANDLE;
    rpAtlasMem = VK_NULL_HANDLE;
}

// ─── Actions ─────────────────────────────────────────────────────────────────
// Stand in a shot's view: the place, the camera, the moment (paused) and the cloud map's drift — and, asked
// for, the settings it was taken with, exactly as saved (they replace the current ones, and are what the app
// saves on exit).
void SatelliteSim::reportGo(const std::string &id, int shot, bool withSettings)
{
    json rep;
    if (!satreport::read(fs::path(reportDir()) / id, rep))
        return;
    for (const json &s : rep["shots"])
        if (s.value("n", 0) == shot && s.contains("record"))
        {
            try
            {
                stopTrack();
                harnessApplyViewRecord(s["record"], withSettings, true, false);
                skyTaaHistValid = false; // a cut
                cv2HistoryValid = false;
                reportStatus_ = withSettings ? "At the shot's view, with its settings (time paused)" : "At the shot's view (time paused)";
            }
            catch (const std::exception &e)
            {
                reportStatus_ = std::string("Could not go there: ") + e.what();
            }
            return;
        }
}

void SatelliteSim::reportSetPin(const std::string &id, int shot, bool set, float x, float y)
{
    const fs::path dir = fs::path(reportDir()) / id;
    json rep;
    if (!satreport::read(dir, rep))
        return;
    for (json &s : rep["shots"])
        if (s.value("n", 0) == shot)
        {
            if (set)
                s["pin"] = {{"x", x}, {"y", y}};
            else
                s.erase("pin");
        }
    satreport::write(dir, rep);
    if (ReportIssueInfo *is = reportFind(id))
        for (ReportShotInfo &s : is->shots)
            if (s.n == shot)
            {
                s.hasPin = set;
                s.pinX = x;
                s.pinY = y;
            }
}

// The area of the frame the problem is in, and that area cut out of the shot's PNG at full resolution
// (shot_NN_crop.png, named by the shot's "crop"): what a reader of the report, or an agent, looks at first.
void SatelliteSim::reportSetRect(const std::string &id, int shot, bool set, float x, float y, float w, float h)
{
    const fs::path dir = fs::path(reportDir()) / id;
    json rep;
    if (!satreport::read(dir, rep))
        return;
    std::string img;
    for (const json &s : rep["shots"])
        if (s.value("n", 0) == shot && s.contains("image") && s["image"].is_string())
            img = s["image"].get<std::string>();
    x = std::clamp(x, 0.0f, 1.0f);
    y = std::clamp(y, 0.0f, 1.0f);
    w = std::clamp(w, 0.0f, 1.0f - x);
    h = std::clamp(h, 0.0f, 1.0f - y);
    std::string crop;
    if (!img.empty() && img.find_first_of("/\\") == std::string::npos)
    {
        std::error_code ec;
        fs::remove(dir / cropName(img), ec);
        // The PNG is written by the screenshot thread: never read one while a capture is in flight.
        if (set && (screenshotEncoding.load() || screenshotCopyPending || screenshotRequested))
            reportStatus_ = "Area marked. Its crop was not written (a shot is still being saved): mark it again in a moment";
        else if (set)
        {
            int iw = 0, ih = 0, n = 0;
            if (stbi_uc *px = stbi_load((dir / img).string().c_str(), &iw, &ih, &n, 4))
            {
                const int x0 = std::clamp((int)(x * (float)iw), 0, iw - 1), y0 = std::clamp((int)(y * (float)ih), 0, ih - 1);
                const int cw = std::clamp((int)(w * (float)iw + 0.5f), 1, iw - x0), ch = std::clamp((int)(h * (float)ih + 0.5f), 1, ih - y0);
                if (stbi_write_png((dir / cropName(img)).string().c_str(), cw, ch, 4, px + ((size_t)y0 * iw + x0) * 4, iw * 4))
                    crop = cropName(img);
                stbi_image_free(px);
            }
        }
    }
    for (json &s : rep["shots"])
        if (s.value("n", 0) == shot)
        {
            s.erase("rect");
            s.erase("crop");
            if (set)
            {
                s["rect"] = {{"x", x}, {"y", y}, {"w", w}, {"h", h}};
                if (!crop.empty())
                    s["crop"] = crop;
            }
        }
    satreport::write(dir, rep);
    if (ReportIssueInfo *is = reportFind(id))
        for (ReportShotInfo &s : is->shots)
            if (s.n == shot)
            {
                s.hasRect = set;
                s.rectX = x;
                s.rectY = y;
                s.rectW = w;
                s.rectH = h;
            }
}

void SatelliteSim::reportDeleteIssue(const std::string &id)
{
    const fs::path dir = fs::path(reportDir()) / id;
    std::error_code ec;
    // Only ever a folder directly under reports/ that holds a report.
    if (id.empty() || id.find_first_of("/\\") != std::string::npos || id == "." || id == ".." || !fs::exists(dir / "report.json", ec))
        return;
    fs::remove_all(dir, ec);
    for (ReportCell &c : reportCells_)
        if (c.key.rfind(id + "/", 0) == 0)
            c = ReportCell{};
    if (reportActiveId_ == id)
        reportActiveId_.clear();
    if (reportViewId_ == id)
        reportViewId_.clear();
    reportPreviewKey_.clear();
    reportsDirty_ = true;
    if (rpChrome.open)
        reportsLoad();
}

void SatelliteSim::reportDeleteShot(const std::string &id, int shot)
{
    const fs::path dir = fs::path(reportDir()) / id;
    json rep;
    if (!satreport::read(dir, rep))
        return;
    json kept = json::array();
    std::error_code ec;
    for (json &s : rep["shots"])
    {
        if (s.value("n", 0) != shot)
        {
            kept.push_back(std::move(s));
            continue;
        }
        if (s.contains("image") && s["image"].is_string())
        {
            const std::string img = s["image"].get<std::string>();
            if (img.find_first_of("/\\") == std::string::npos)
            {
                fs::remove(dir / img, ec);
                fs::remove(dir / thumbName(img), ec);
                fs::remove(dir / cropName(img), ec);
            }
        }
    }
    rep["shots"] = kept;
    satreport::write(dir, rep);
    const std::string key = id + "/" + std::to_string(shot);
    for (ReportCell &c : reportCells_)
        if (c.key == key)
            c = ReportCell{};
    reportPreviewKey_.clear();
    reportsDirty_ = true;
    if (rpChrome.open)
        reportsLoad();
}

// ─── Export ──────────────────────────────────────────────────────────────────
namespace
{
    uint32_t crc32Of(const std::vector<uint8_t> &d)
    {
        static uint32_t table[256];
        static bool init = false;
        if (!init)
        {
            for (uint32_t i = 0; i < 256; ++i)
            {
                uint32_t c = i;
                for (int k = 0; k < 8; ++k)
                    c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
                table[i] = c;
            }
            init = true;
        }
        uint32_t c = 0xFFFFFFFFu;
        for (uint8_t b : d)
            c = table[(c ^ b) & 0xFFu] ^ (c >> 8);
        return c ^ 0xFFFFFFFFu;
    }

    // A ZIP archive of stored (uncompressed) entries: enough for any unzip tool, no dependency.
    struct ZipWriter
    {
        std::ofstream f;
        std::vector<uint8_t> central;
        uint32_t count = 0;
        static void u16(std::vector<uint8_t> &v, uint32_t x)
        {
            v.push_back((uint8_t)(x & 0xFF));
            v.push_back((uint8_t)((x >> 8) & 0xFF));
        }
        static void u32(std::vector<uint8_t> &v, uint32_t x)
        {
            u16(v, x & 0xFFFF);
            u16(v, x >> 16);
        }
        void add(const std::string &name, const std::vector<uint8_t> &data)
        {
            const uint32_t offset = (uint32_t)f.tellp(), crc = crc32Of(data), size = (uint32_t)data.size();
            std::vector<uint8_t> h;
            u32(h, 0x04034b50u);
            u16(h, 20); u16(h, 0); u16(h, 0);          // version, flags, method 0 = stored
            u16(h, 0); u16(h, 0x21);                   // time, date (1980-01-01)
            u32(h, crc); u32(h, size); u32(h, size);
            u16(h, (uint32_t)name.size()); u16(h, 0);
            f.write((const char *)h.data(), (std::streamsize)h.size());
            f.write(name.data(), (std::streamsize)name.size());
            f.write((const char *)data.data(), (std::streamsize)data.size());
            u32(central, 0x02014b50u);
            u16(central, 20); u16(central, 20); u16(central, 0); u16(central, 0);
            u16(central, 0); u16(central, 0x21);
            u32(central, crc); u32(central, size); u32(central, size);
            u16(central, (uint32_t)name.size()); u16(central, 0); u16(central, 0);
            u16(central, 0); u16(central, 0); u32(central, 0);
            u32(central, offset);
            central.insert(central.end(), name.begin(), name.end());
            ++count;
        }
        bool finish()
        {
            const uint32_t start = (uint32_t)f.tellp();
            f.write((const char *)central.data(), (std::streamsize)central.size());
            std::vector<uint8_t> e;
            u32(e, 0x06054b50u);
            u16(e, 0); u16(e, 0); u16(e, count); u16(e, count);
            u32(e, (uint32_t)central.size()); u32(e, start); u16(e, 0);
            f.write((const char *)e.data(), (std::streamsize)e.size());
            f.close();
            return !f.fail();
        }
    };

    bool readAll(const fs::path &p, std::vector<uint8_t> &out)
    {
        std::ifstream f(p, std::ios::binary);
        if (!f)
            return false;
        out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
        return true;
    }

    void replaceAll(std::string &s, const std::string &from, const std::string &to)
    {
        if (from.empty())
            return;
        for (size_t i = 0; (i = s.find(from, i)) != std::string::npos; i += to.size())
            s.replace(i, from.size(), to);
    }
}

std::string SatelliteSim::reportExportZip(const std::string &id)
{
    const fs::path dir = fs::path(reportDir()) / id;
    json rep;
    if (id.empty() || !satreport::read(dir, rep))
    {
        reportStatus_ = "Export: the issue could not be read";
        return {};
    }
    if (screenshotEncoding.load() || screenshotCopyPending || screenshotRequested)
    {
        reportStatus_ = "Export: a shot is still being written, try again in a moment";
        return {};
    }
    // Sent, once it leaves (an issue already fixed / closed keeps its status).
    if (rep.value("status", std::string("open")) == "open")
        reportSetField(id, 0, "status", "sent");
    std::error_code ec;
    const fs::path outDir = fs::path(userDataDir_) / "report_exports";
    fs::create_directories(outDir, ec);
    const fs::path out = outDir / (id + ".zip");
    ZipWriter z;
    z.f.open(out, std::ios::binary | std::ios::trunc);
    if (!z.f)
    {
        reportStatus_ = "Export: could not write " + out.string();
        return {};
    }
    // The folder's own files (flat: report.json, the shots, their thumbnails, anything a resolution added).
    std::vector<fs::path> files;
    for (const auto &e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec) && e.path().extension() != ".tmp")
            files.push_back(e.path());
    std::sort(files.begin(), files.end());
    std::vector<uint8_t> data;
    for (const fs::path &p : files)
        if (readAll(p, data))
            z.add(id + "/" + p.filename().string(), data);
    // The end of this session's log: what the app said about the machine and the run. The folders it names
    // carry the reporter's user name: redacted.
    if (readAll(Log::path(), data))
    {
        std::string log(data.begin(), data.end());
        if (log.size() > 96 * 1024)
            log = log.substr(log.find('\n', log.size() - 96 * 1024) + 1);
        std::string a = exeDir_, b = userDataDir_;
        replaceAll(log, a, "<exe dir>");
        replaceAll(log, b, "<user data>");
        std::replace(a.begin(), a.end(), '\\', '/');
        std::replace(b.begin(), b.end(), '\\', '/');
        replaceAll(log, a, "<exe dir>");
        replaceAll(log, b, "<user data>");
        z.add(id + "/log_tail.txt", std::vector<uint8_t>(log.begin(), log.end()));
    }
    if (!z.finish())
    {
        reportStatus_ = "Export: writing " + out.string() + " failed";
        return {};
    }
    reportStatus_ = "Exported " + out.filename().string() + " (in report_exports): send that file";
    Log::line("report: exported " + out.string());
    return out.string();
}

// ─── The window ──────────────────────────────────────────────────────────────
void SatelliteSim::buildReportsWindow(const UIInput &inp, UIRenderer &ui)
{
    if (!inp.lmbDown && !inp.lmbReleased)
        rpMarkDrag_ = false; // (a mark's drag that ended while the window was not drawn)
    if (!rpChrome.open)
    {
        reportFlushEdit(); // closed with a field in edit (its commit has no field left to take it)
        return;
    }
    if (rpChrome.w <= 0.0f)
    {
        // 720 x 700 where it fits; on a small screen ~60% of the width, ~72% of the height (2026-10-21)
        rpChrome.w = winDefault(720.0f, 0.60f, inp.screenW, 560.0f);
        rpChrome.h = winDefault(std::min(700.0f, inp.screenH - 200.0f), 0.72f, inp.screenH, 380.0f);
    }
    reportThumbTick(ui);
    auto text = [&](const char *s, Clay_Color c, float size, bool wrap = false)
    {
        CLAY_TEXT((Clay_String{false, (int32_t)strlen(s), s}),
                  CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs((int)size), .wrapMode = wrap ? CLAY_TEXT_WRAP_WORDS : CLAY_TEXT_WRAP_NONE}));
    };
    const bool thumbs = rpAtlasUiId != 0;
    ReportIssueInfo *view = reportViewId_.empty() ? nullptr : reportFind(reportViewId_);

    // What the layout asks for; applied after it (or queued for the next tick when it reshapes the list).
    std::string openId, activeId, delIssue, folder, exportId;
    bool refresh = false;
    int goShot = 0, delShot = 0, selShot = 0;
    bool goSettings = false, back = false, newIssue = false, clearPin = false, setPin = false, delClicked = false;
    float pinX = 0.0f, pinY = 0.0f;
    bool setRect = false, clearRect = false;
    float rectX = 0.0f, rectY = 0.0f, rectW = 0.0f, rectH = 0.0f;

    static char titleBuf[64];
    snprintf(titleBuf, sizeof(titleBuf), view ? "Issue report" : "Issue reports (%d)", (int)reportIssues_.size());

    buildResizableWindow(
        inp, ui, rpChrome, 7, titleBuf, true, hovRpClose, (inp.screenW - rpChrome.w) * 0.5f, 90.0f, 560.0f, 320.0f, 1600.0f, 1400.0f,
        [&]()
        {
            CLAY(CLAY_ID("RpBody"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                .padding = {12, 12, 8, 10},
                                                .childGap = 6,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                if (!view)
                {
                    // ── The list ─────────────────────────────────────────────────────────────────────
                    CLAY(CLAY_ID("RpTopRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                          .childGap = 6,
                                                          .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                          .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        if (uiButton(inp, ui, "RpNew", 0, "New issue from this view", "Take a snapshot of the view as it is and start an issue with it"))
                            newIssue = true;
                        if (uiButton(inp, ui, "RpFolder", 0, "Open folder", "The reports folder: one folder per issue"))
                            folder = reportDir();
                        if (uiButton(inp, ui, "RpRefresh", 0, "Refresh", "Read the reports again (an agent or a tool may have resolved one)"))
                            refresh = true;
                    }
                    {
                        char k1[24], k2[24];
                        snprintf(k1, sizeof(k1), "%s", keyDisplayName(keybindings[KB_SAVE_SNAPSHOT].key));
                        snprintf(k2, sizeof(k2), "%s", keyDisplayName(keybindings[KB_REPORT_NEW].key));
                        text(uiKitStr("%s adds a shot to the active issue, %s starts a new one.", k1, k2), Pal::textHint, 11);
                    }
                    // The Display tab's setting, here too: shots with the UI show the windows you had open (a trace,
                    // the info window); this window and the note bar are always left out of them.
                    uiToggleRow(inp, ui, "RpShotUi", 0, "Shots include the UI", reportShotUiSetting,
                                "Keep the windows and panels you had open in the shot (this window and the note bar are left out)");
                    if (!reportStatus_.empty())
                        text(reportStatus_.c_str(), {230, 190, 120, 255}, 11);
                    CLAY(CLAY_ID("RpList"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                        .padding = {0, 8, 4, 4},
                                                        .childGap = 8,
                                                        .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                             .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                    {
                        if (reportIssues_.empty())
                            text("No issues yet", Pal::textDim, 12);
                        const Clay_ElementData ld = Clay_GetElementData(CLAY_ID("RpList"));
                        const float listW = std::max(200.0f, (ld.found ? ld.boundingBox.width : rpChrome.w - 24.0f) - 8.0f);
                        const float gap = 8.0f, wantW = (float)fs(11) * 18.0f;
                        const int cols = std::max(1, (int)((listW + gap) / (wantW + gap)));
                        const float cardW = std::floor((listW - gap * (cols - 1)) / cols);
                        const float imgW = cardW - 12.0f, imgH = std::floor(imgW * kRpThumbH / (float)kRpThumbW);
                        for (int r0 = 0; r0 < (int)reportIssues_.size(); r0 += cols)
                        {
                            CLAY(CLAY_IDI("RpRow", r0), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                    .childGap = (uint16_t)gap,
                                                                    .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                            {
                                for (int i = r0; i < r0 + cols && i < (int)reportIssues_.size(); ++i)
                                {
                                    ReportIssueInfo &is = reportIssues_[i];
                                    const bool active = is.id == reportActiveId_;
                                    CLAY(CLAY_IDI("RpCard", i), {.layout = {.sizing = {CLAY_SIZING_FIXED(cardW), CLAY_SIZING_FIT(0)},
                                                                            .padding = {6, 6, 6, 6},
                                                                            .childGap = 4,
                                                                            .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                                                 .backgroundColor = Pal::cardBg,
                                                                 .cornerRadius = CLAY_CORNER_RADIUS(4),
                                                                 .border = {.color = active ? Pal::btnAccent : Clay_Color{0, 0, 0, 0}, .width = CLAY_BORDER_ALL(1)}})
                                    {
                                        const Clay_ElementId tid = CLAY_IDI("RpThumb", i);
                                        const bool hovT = Clay_PointerOver(tid);
                                        CLAY(tid, {.layout = {.sizing = {CLAY_SIZING_FIXED(imgW), CLAY_SIZING_FIXED(imgH)},
                                                              .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                   .backgroundColor = Pal::insetBg,
                                                   .border = {.color = hovT ? Pal::btnAccent : Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
                                        {
                                            const bool n = Clay_Hovered();
                                            uiHovRoll(tid.id, n);
                                            ui.tooltip(inp, n, "Open this issue", fs(11));
                                            if (n && inp.lmbPressed)
                                                openId = is.id;
                                            ReportShotInfo *first = is.shots.empty() ? nullptr : &is.shots.front();
                                            if (first)
                                                first->onScreen = rpBoxVisible(tid, CLAY_ID("RpList"));
                                            if (thumbs && first && first->cell >= 0)
                                            {
                                                if (first->onScreen)
                                                    reportCells_[first->cell].used = reportCellClock_;
                                                CLAY(CLAY_IDI("RpThumbImg", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                                                 .custom = {.customData = &first->img}}) {}
                                            }
                                            else
                                                text(first ? (first->thumbTried ? "No image" : "...") : "No shots", Pal::textHint, 11);
                                        }
                                        CLAY(CLAY_IDI("RpCardTitle", i), {.layout = {.sizing = {CLAY_SIZING_FIXED(imgW), CLAY_SIZING_FIT(0)}},
                                                                          .clip = {.horizontal = true}})
                                        {
                                            text(is.title.empty() ? "(untitled)" : is.title.c_str(), Pal::textPrimary, 12);
                                        }
                                        // "fixed" = resolved and waiting for the reporter to verify: stands out.
                                        text(is.line, is.status == "fixed" ? Pal::textOk : (active ? Pal::volValue : Pal::textDim), 11);
                                        CLAY(CLAY_IDI("RpCardBtns", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                                    .childGap = 4,
                                                                                    .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                                        {
                                            if (uiButton(inp, ui, "RpOpen", i, "Open", "Its description and shots", false, true, true))
                                                openId = is.id;
                                            if (uiButton(inp, ui, "RpAct", i, active ? "Active" : "Make active",
                                                         active ? "New shots (the snapshot key) go to this issue" : "Send new shots (the snapshot key) to this issue",
                                                         active, true, true))
                                                activeId = is.id;
                                            const bool armed = reportDeleteArmed_ == is.id;
                                            if (uiButton(inp, ui, "RpDel", i, armed ? "Confirm" : "Delete",
                                                         armed ? "Click again to delete the issue and its images" : "Delete (asks once more)", armed, true, true))
                                            {
                                                delClicked = true;
                                                if (armed)
                                                    delIssue = is.id;
                                                else
                                                    reportDeleteArmed_ = is.id;
                                            }
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ui.scrollbar(CLAY_ID("RpList"));
                }
                else
                {
                    // ── One issue ────────────────────────────────────────────────────────────────────
                    ReportIssueInfo &is = *view;
                    const bool active = is.id == reportActiveId_;
                    const Clay_ElementData vd = Clay_GetElementData(CLAY_ID("RpView"));
                    const float bodyW = std::max(300.0f, (vd.found ? vd.boundingBox.width : rpChrome.w - 24.0f) - 12.0f);
                    CLAY(CLAY_ID("RpHead"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                        .childGap = 6,
                                                        .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                        .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                    {
                        if (uiButton(inp, ui, "RpBack", 0, "< Issues", "Back to the list"))
                            back = true;
                        if (uiButton(inp, ui, "RpAct", 1000, active ? "Active" : "Make active",
                                     active ? "New shots (the snapshot key) go to this issue" : "Send new shots (the snapshot key) to this issue", active))
                            activeId = is.id;
                        if (uiButton(inp, ui, "RpFolder", 1, "Open folder", "This issue's folder"))
                            folder = (fs::path(reportDir()) / is.id).string();
                        if (uiButton(inp, ui, "RpExport", 0, "Export zip",
                                     "One file to send: the report, its images and the end of the session log (marks an open issue Sent)"))
                            exportId = is.id;
                        const bool armed = reportDeleteArmed_ == is.id;
                        if (uiButton(inp, ui, "RpDel", 1000, armed ? "Confirm delete" : "Delete issue",
                                     armed ? "Click again to delete the issue and its images" : "Delete (asks once more)", armed))
                        {
                            delClicked = true;
                            if (armed)
                                delIssue = is.id;
                            else
                                reportDeleteArmed_ = is.id;
                        }
                    }
                    if (!reportStatus_.empty())
                        text(reportStatus_.c_str(), {230, 190, 120, 255}, 11);
                    CLAY(CLAY_ID("RpView"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                        .padding = {0, 12, 2, 6},
                                                        .childGap = 5,
                                                        .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                             .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                    {
                        CLAY(CLAY_ID("RpTitleRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                .childGap = 8,
                                                                .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                        {
                            text("Title", Pal::textDim, 11);
                            std::string t = is.title;
                            if (textField(inp, ui, CLAY_ID("RpTitle"), t, bodyW - (float)fs(11) * 4.0f, 13, "The issue in a few words", kReportTitleMax))
                                reportSetField(is.id, 0, "title", t);
                            reportNoteEditTarget(CLAY_ID("RpTitle").id, is.id, 0, "title", kReportTitleMax, false);
                        }
                        int cur = -1;
                        for (int k = 0; k < 9; ++k)
                            if (is.category == kCategories[k])
                                cur = k;
                        const int pc = uiChoiceRow(inp, ui, "RpCat", 0, "Category", kCategoryLabels, 9, cur,
                                                   "Which part of the sim it is about (it decides who looks at it)");
                        if (pc >= 0)
                            reportSetField(is.id, 0, "category", kCategories[pc]);
                        cur = -1;
                        for (int k = 0; k < 4; ++k)
                            if (is.status == kStatuses[k])
                                cur = k;
                        const int ps = uiChoiceRow(inp, ui, "RpStat", 0, "Status", kStatusLabels, 4, cur);
                        if (ps >= 0)
                            reportSetField(is.id, 0, "status", kStatuses[ps]);
                        if (!is.resolution.empty() || is.resLine[0])
                        {
                            // What whoever worked on it wrote back (tools/harness/reports.py resolve): go and look
                            // (Go on a shot), then set Closed, or Open again if it is not fixed.
                            uiSection("RpSec", 3, "RESOLUTION");
                            if (is.resLine[0])
                                text(is.resLine, Pal::textDim, 11);
                            if (!is.resolution.empty())
                            {
                                // (Read-only, through the text box: it wraps, scrolls past 14 lines, and a long
                                // one costs Clay nothing.)
                                std::string r = is.resolution;
                                textArea(inp, ui, CLAY_ID("RpResText"), r, bodyW, 1, 12, nullptr, kReportDescMax, 14, false, &Pal::volValue);
                            }
                        }

                        uiSection("RpSec", 0, "DESCRIPTION");
                        {
                            std::string d = is.description;
                            if (textArea(inp, ui, CLAY_ID("RpDesc"), d, bodyW, 5, 12,
                                         "What is wrong, what you expected to see, and how to see it (click to type; Ctrl+Enter or a click elsewhere saves)",
                                         kReportDescMax, 16))
                                reportSetField(is.id, 0, "description", d);
                            reportNoteEditTarget(CLAY_ID("RpDesc").id, is.id, 0, "description", kReportDescMax, true);
                        }

                        // ── The selected shot's frame, and its pin ────────────────────────────────────
                        ReportShotInfo *selS = nullptr;
                        for (ReportShotInfo &s : is.shots)
                            if (s.n == reportViewShot_)
                                selS = &s;
                        if (selS)
                        {
                            uiSection("RpSec", 2, uiKitStr("SHOT %d", selS->n));
                            const bool have = thumbs && reportPreviewOk_ && reportPreviewKey_ == is.id + "/" + std::to_string(selS->n);
                            const float aspect = selS->imgW > 0 && selS->imgH > 0 ? (float)selS->imgH / (float)selS->imgW : 9.0f / 16.0f;
                            const float pw = bodyW, ph = std::floor(pw * aspect);
                            // The black backing is on the parent; the image is the inner element's own custom draw,
                            // and the marks (pin, area, the rubber band of a drag) one custom element over it, so
                            // they scroll and clip with it.
                            const Clay_ElementId pid = CLAY_ID("RpPrev");
                            const Clay_ElementData pd = Clay_GetElementData(pid);
                            const bool pdOk = pd.found && pd.boundingBox.width > 1.0f && pd.boundingBox.height > 1.0f;
                            const float mx = pdOk ? std::clamp((inp.mouseX - pd.boundingBox.x) / pd.boundingBox.width, 0.0f, 1.0f) : 0.0f;
                            const float my = pdOk ? std::clamp((inp.mouseY - pd.boundingBox.y) / pd.boundingBox.height, 0.0f, 1.0f) : 0.0f;
                            rpMarks_.quads.clear();
                            auto outline = [&](float x, float y, float w, float h, float t, glm::vec4 c)
                            {
                                rpMarks_.quads.push_back({x, y, w, t, c});
                                rpMarks_.quads.push_back({x, y + h - t, w, t, c});
                                rpMarks_.quads.push_back({x, y + t, t, h - 2.0f * t, c});
                                rpMarks_.quads.push_back({x + w - t, y + t, t, h - 2.0f * t, c});
                            };
                            const glm::vec4 markCol{1.0f, 0.24f, 0.2f, 1.0f};
                            if (selS->hasRect)
                                outline(selS->rectX * pw, selS->rectY * ph, std::max(6.0f, selS->rectW * pw), std::max(6.0f, selS->rectH * ph), 2.0f, markCol);
                            if (selS->hasPin)
                            {
                                const float px = selS->pinX * pw, py = selS->pinY * ph;
                                outline(px - 12.0f, py - 12.0f, 24.0f, 24.0f, 3.0f, markCol);
                                rpMarks_.quads.push_back({px - 1.0f, py - 1.0f, 2.0f, 2.0f, glm::vec4(1.0f)});
                            }
                            if (rpMarkDrag_ && inp.lmbDown)
                            {
                                const float x0 = std::min(rpMarkX0_, mx) * pw, y0 = std::min(rpMarkY0_, my) * ph;
                                const float bw = std::abs(mx - rpMarkX0_) * pw, bh = std::abs(my - rpMarkY0_) * ph;
                                if (bw >= 6.0f && bh >= 6.0f)
                                    outline(x0, y0, bw, bh, 1.5f, glm::vec4(1.0f));
                            }
                            rpMarks_.block.quads = rpMarks_.quads.data();
                            rpMarks_.block.quadCount = (int)rpMarks_.quads.size();
                            CLAY(CLAY_ID("RpPrevBack"), {.layout = {.sizing = {CLAY_SIZING_FIXED(pw), CLAY_SIZING_FIXED(ph)},
                                                                    .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                                         .backgroundColor = Pal::insetBg})
                            {
                                if (!have)
                                    text(selS->image.empty() ? "This shot has no image" : (reportPreviewKey_.empty() ? "..." : "The image could not be read"),
                                         Pal::textHint, 11);
                                else
                                {
                                    CLAY(pid, {.layout = {.sizing = {CLAY_SIZING_FIXED(pw), CLAY_SIZING_FIXED(ph)}},
                                               .custom = {.customData = &reportPreviewImg_}})
                                    {
                                        // A press starts a mark: released where it began it is the pin, dragged it
                                        // is the area.
                                        if (Clay_Hovered() && inp.lmbPressed && pdOk)
                                        {
                                            rpMarkDrag_ = true;
                                            rpMarkX0_ = mx;
                                            rpMarkY0_ = my;
                                        }
                                        CLAY(CLAY_ID("RpMarks"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                                  .custom = {.customData = &rpMarks_.block}}) {}
                                    }
                                }
                            }
                            if (rpMarkDrag_ && inp.lmbDown)
                                ui.holdDragScroll(); // (the view must not be dragged along)
                            else if (rpMarkDrag_)
                            {
                                rpMarkDrag_ = false;
                                if (pdOk && have)
                                {
                                    const float dx = std::abs(mx - rpMarkX0_) * pd.boundingBox.width, dy = std::abs(my - rpMarkY0_) * pd.boundingBox.height;
                                    if (dx < 6.0f && dy < 6.0f)
                                    {
                                        setPin = true;
                                        pinX = rpMarkX0_;
                                        pinY = rpMarkY0_;
                                    }
                                    else if (dx >= 6.0f && dy >= 6.0f)
                                    {
                                        setRect = true;
                                        rectX = std::min(rpMarkX0_, mx);
                                        rectY = std::min(rpMarkY0_, my);
                                        rectW = std::abs(mx - rpMarkX0_);
                                        rectH = std::abs(my - rpMarkY0_);
                                    }
                                }
                            }
                            CLAY(CLAY_ID("RpPinRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                  .childGap = 8,
                                                                  .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                                  .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                            {
                                text("Click the image to pin a point, drag to mark an area.", Pal::textHint, 11);
                                if (selS->hasPin && uiButton(inp, ui, "RpPinClear", 0, "Clear pin", "Remove the point"))
                                    clearPin = true;
                                if (selS->hasRect && uiButton(inp, ui, "RpRectClear", 0, "Clear area", "Remove the area and its crop"))
                                    clearRect = true;
                            }
                            if (selS->hasPin || selS->hasRect)
                            {
                                const char *a = selS->hasPin ? uiKitStr("Pin at %.0f%%, %.0f%%.", selS->pinX * 100.0f, selS->pinY * 100.0f) : "";
                                const char *b = selS->hasRect ? uiKitStr("Area %.0f%% x %.0f%% of the frame at %.0f%%, %.0f%% (saved as its own crop image).",
                                                                         selS->rectW * 100.0f, selS->rectH * 100.0f, selS->rectX * 100.0f, selS->rectY * 100.0f)
                                                              : "";
                                text(uiKitStr("%s%s%s", a, *a && *b ? "  " : "", b), Pal::textHint, 11);
                            }
                        }

                        uiSection("RpSec", 1, uiKitStr("SHOTS (%d)", (int)is.shots.size()));
                        const float tW = (float)fs(11) * 12.0f, tH = std::floor(tW * kRpThumbH / (float)kRpThumbW);
                        for (int i = 0; i < (int)is.shots.size(); ++i)
                        {
                            ReportShotInfo &s = is.shots[i];
                            const bool sel = s.n == reportViewShot_;
                            CLAY(CLAY_IDI("RpShot", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                    .padding = {6, 6, 6, 6},
                                                                    .childGap = 8,
                                                                    .layoutDirection = CLAY_LEFT_TO_RIGHT},
                                                         .backgroundColor = sel ? Pal::cardBgSel : Pal::cardBg,
                                                         .cornerRadius = CLAY_CORNER_RADIUS(4)})
                            {
                                const Clay_ElementId tid = CLAY_IDI("RpShotThumb", i);
                                const bool hovT = Clay_PointerOver(tid);
                                CLAY(tid, {.layout = {.sizing = {CLAY_SIZING_FIXED(tW), CLAY_SIZING_FIXED(tH)},
                                                      .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                           .backgroundColor = Pal::insetBg,
                                           .border = {.color = (sel || hovT) ? Pal::btnAccent : Pal::cardBorder, .width = CLAY_BORDER_ALL(1)}})
                                {
                                    const bool n = Clay_Hovered();
                                    uiHovRoll(tid.id, n);
                                    ui.tooltip(inp, n, "Show this shot below", fs(11));
                                    if (n && inp.lmbPressed)
                                        selShot = s.n;
                                    s.onScreen = rpBoxVisible(tid, CLAY_ID("RpView"));
                                    if (thumbs && s.cell >= 0)
                                    {
                                        if (s.onScreen)
                                            reportCells_[s.cell].used = reportCellClock_;
                                        CLAY(CLAY_IDI("RpShotImg", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                                        .custom = {.customData = &s.img}}) {}
                                    }
                                    else
                                        text(s.thumbTried || s.image.empty() ? "No image" : "...", Pal::textHint, 11);
                                }
                                CLAY(CLAY_IDI("RpShotCol", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                           .childGap = 4,
                                                                           .layoutDirection = CLAY_TOP_TO_BOTTOM}})
                                {
                                    text(uiKitStr("Shot %d    %s    %s%s", s.n, s.meta[0], s.meta[1], s.hasPin || s.hasRect ? "    marked" : ""), Pal::volValue, 11);
                                    std::string note = s.note;
                                    // Three lines that grow to nine, then scroll.
                                    if (textArea(inp, ui, CLAY_IDI("RpShotNote", i), note, std::max(120.0f, bodyW - tW - 36.0f), 3, 12,
                                                 "What this shot shows (click to type)", kReportNoteMax, 9))
                                        reportSetField(is.id, s.n, "note", note);
                                    reportNoteEditTarget(CLAY_IDI("RpShotNote", i).id, is.id, s.n, "note", kReportNoteMax, true);
                                    CLAY(CLAY_IDI("RpShotBtns", i), {.layout = {.sizing = {CLAY_SIZING_FIT(0), CLAY_SIZING_FIT(0)},
                                                                                .childGap = 4,
                                                                                .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                                    {
                                        if (uiButton(inp, ui, "RpGo", i, "Go", "Stand in this shot's view: place, camera, moment (paused) and clouds"))
                                            goShot = s.n;
                                        if (uiButton(inp, ui, "RpGoSet", i, "Go + settings",
                                                     "The same, and switch to the settings the shot was taken with (they replace yours)"))
                                        {
                                            goShot = s.n;
                                            goSettings = true;
                                        }
                                        const std::string armKey = is.id + "#" + std::to_string(s.n);
                                        const bool armed = reportDeleteArmed_ == armKey;
                                        if (uiButton(inp, ui, "RpShotDel", i, armed ? "Confirm" : "Delete",
                                                     armed ? "Click again to delete this shot and its image" : "Delete this shot (asks once more)", armed))
                                        {
                                            delClicked = true;
                                            if (armed)
                                                delShot = s.n;
                                            else
                                                reportDeleteArmed_ = armKey;
                                        }
                                    }
                                }
                            }
                        }
                    }
                    ui.scrollbar(CLAY_ID("RpView"));
                }
            }
        });

    // After the layout. What reshapes the list (New, Delete) waits for the next reportTick.
    if (inp.lmbPressed && !delClicked)
        reportDeleteArmed_.clear(); // any click that is not on a Delete button disarms it
    // Anything that takes the open issue's fields off screen (or reads its file) saves the one being edited
    // first: its commit would otherwise wait for a field that is never drawn again, and be dropped.
    if (back || !openId.empty() || !exportId.empty() || goShot > 0 || refresh || newIssue || !delIssue.empty() || delShot > 0 ||
        !rpChrome.open)
        reportFlushEdit();
    if (refresh)
        reportsDirty_ = true;
    if (!exportId.empty())
    {
        const std::string zip = reportExportZip(exportId);
        if (!zip.empty())
            folder = fs::path(zip).parent_path().string();
    }
    if (!folder.empty())
    {
        std::error_code ec;
        fs::create_directories(folder, ec);
        if (!harness::options().enabled) // (a scripted run must not pop file-manager windows)
            openFolder(folder);
    }
    if (!activeId.empty())
        reportActiveId_ = activeId;
    if (!openId.empty())
    {
        reportViewId_ = openId;
        reportViewShot_ = 0; // reportThumbTick picks the first
        reportStatus_.clear();
    }
    if (back)
    {
        reportViewId_.clear();
        reportStatus_.clear();
    }
    if (selShot > 0)
        reportViewShot_ = selShot;
    if (view && goShot > 0)
        reportGo(view->id, goShot, goSettings);
    if (view && setPin)
        reportSetPin(view->id, reportViewShot_, true, pinX, pinY);
    if (view && clearPin)
        reportSetPin(view->id, reportViewShot_, false, 0.0f, 0.0f);
    if (view && setRect)
        reportSetRect(view->id, reportViewShot_, true, rectX, rectY, rectW, rectH);
    if (view && clearRect)
        reportSetRect(view->id, reportViewShot_, false, 0.0f, 0.0f, 0.0f, 0.0f);
    if (!delIssue.empty())
    {
        reportDeleteArmed_.clear();
        reportOp_ = ReportOp{1, delIssue, 0};
    }
    else if (view && delShot > 0)
    {
        reportDeleteArmed_.clear();
        reportOp_ = ReportOp{2, view->id, delShot};
    }
    else if (newIssue)
        reportOp_ = ReportOp{3, std::string(), 0};
}
