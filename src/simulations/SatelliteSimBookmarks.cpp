// SatelliteSimBookmarks.cpp — bookmarks (2026-10-03): a place at a moment, with a thumbnail of the view.
//
// A bookmark is the observer (its ECEF direction and height), the camera (az / el / fov), the sim time and the
// cloud map's drift — the drift is session state, so without it a storm bookmark would come back to clear sky
// (the same reason harness snapshots carry it). Stored in <user data>/bookmarks/bookmarks.json, the thumbnails as
// <id>.png beside it.
//
// The thumbnail is the next CLEAN frame after Add / Update (the screenshot path with the UI left out: the copy is
// recorded by recordScreenshotCopy, and finalizeScreenshot hands its pixels here instead of writing a screenshot),
// centre-cropped to 16:9 and box-filtered down to kBmThumbW x kBmThumbH. All thumbnails live in one SRGB atlas
// registered once with the UI (UIRenderer has only four external image slots), each card drawing its cell through
// UIImage's sub-rect.
#include "SatelliteSim.h"
#include "../UIRenderer.h"
#include "../VulkanContext.h"
#include "../Log.h"
#include "clay.h"
#include "UIPalette.h"
#include "stb_image.h"
#include "stb_image_write.h" // the implementation lives in UIRenderer.cpp

#include <nlohmann/json.hpp>
#include <cstdio>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

std::string SatelliteSim::bookmarkDir() const
{
    return (fs::path(userDataDir_) / "bookmarks").string();
}

// ─── Persistence ─────────────────────────────────────────────────────────────
void SatelliteSim::bookmarksLoad()
{
    bookmarksLoaded_ = true;
    bookmarks_.clear();
    std::ifstream f(fs::path(bookmarkDir()) / "bookmarks.json");
    if (!f)
        return;
    const nlohmann::json j = nlohmann::json::parse(f, nullptr, false);
    if (j.is_discarded() || !j.contains("bookmarks") || !j["bookmarks"].is_array())
    {
        Log::line("bookmarks: bookmarks.json is not readable; starting empty");
        return;
    }
    for (const auto &e : j["bookmarks"])
    {
        if ((int)bookmarks_.size() >= kBmMax)
            break;
        Bookmark b;
        b.id = e.value("id", std::string());
        b.name = e.value("name", std::string("Bookmark"));
        if (b.id.empty() || !e.contains("obs_dir") || !e["obs_dir"].is_array() || e["obs_dir"].size() != 3)
            continue;
        b.simT = e.value("sim_t_j2000", 0.0);
        b.obsDir = glm::normalize(glm::dvec3(e["obs_dir"][0].get<double>(), e["obs_dir"][1].get<double>(), e["obs_dir"][2].get<double>()));
        b.heightM = e.value("height_m", 0.0f);
        b.az = e.value("az_deg", 0.0f);
        b.el = e.value("el_deg", 10.0f);
        b.fov = e.value("fov_y_deg", 60.0f);
        b.hasDrift = e.contains("cloud_drift_phase");
        b.driftPhase = e.value("cloud_drift_phase", 0.0);
        b.driftRate = e.value("cloud_drift_rate", cloudDriftRate);
        bookmarkFormatMeta(b);
        bookmarks_.push_back(std::move(b));
    }
}

void SatelliteSim::bookmarksSave()
{
    nlohmann::json arr = nlohmann::json::array();
    for (const Bookmark &b : bookmarks_)
    {
        nlohmann::json e = {{"id", b.id},
                            {"name", b.name},
                            {"sim_t_j2000", b.simT},
                            {"obs_dir", {b.obsDir.x, b.obsDir.y, b.obsDir.z}},
                            {"height_m", b.heightM},
                            {"az_deg", b.az},
                            {"el_deg", b.el},
                            {"fov_y_deg", b.fov}};
        if (b.hasDrift)
        {
            e["cloud_drift_phase"] = b.driftPhase;
            e["cloud_drift_rate"] = b.driftRate;
        }
        arr.push_back(e);
    }
    std::error_code ec;
    fs::create_directories(bookmarkDir(), ec);
    std::ofstream f(fs::path(bookmarkDir()) / "bookmarks.json");
    if (!f)
    {
        bmStatus_ = "Could not write bookmarks.json";
        return;
    }
    f << nlohmann::json({{"format", "sat-light-sim-bookmarks/1"}, {"bookmarks", arr}}).dump(2);
}

void SatelliteSim::bookmarkFormatMeta(Bookmark &b) const
{
    const time_t unixT = (time_t)std::floor(b.simT) + 946728000;
    const struct tm *utc = gmtime(&unixT);
    if (utc)
        snprintf(b.meta[0], sizeof(b.meta[0]), "%04d-%02d-%02d %02d:%02d UTC", utc->tm_year + 1900, utc->tm_mon + 1,
                 utc->tm_mday, utc->tm_hour, utc->tm_min);
    else
        snprintf(b.meta[0], sizeof(b.meta[0]), "-");
    const double lat = glm::degrees(std::asin(glm::clamp(b.obsDir.z, -1.0, 1.0)));
    const double lon = glm::degrees(std::atan2(b.obsDir.y, b.obsDir.x));
    char alt[24];
    if (b.heightM >= 1000.0f)
        snprintf(alt, sizeof(alt), "%.1f km", b.heightM / 1000.0f);
    else
        snprintf(alt, sizeof(alt), "%.0f m", b.heightM);
    snprintf(b.meta[1], sizeof(b.meta[1]), "%.2f %c  %.2f %c  %s", std::abs(lat), lat >= 0.0 ? 'N' : 'S', std::abs(lon),
             lon >= 0.0 ? 'E' : 'W', b.heightM > 0.0f ? alt : "ground");
}

// ─── Add / update / go / delete ──────────────────────────────────────────────
namespace
{
    // The current view into a bookmark (everything but its id, name and thumbnail).
    struct BmState
    {
        double simT;
        glm::dvec3 obsDir;
        float h, az, el, fov;
        double drift;
        float rate;
    };
}

int SatelliteSim::bookmarkAdd(const std::string &name)
{
    if (!bookmarksLoaded_)
        bookmarksLoad();
    if ((int)bookmarks_.size() >= kBmMax)
    {
        bmStatus_ = "The list is full (" + std::to_string(kBmMax) + "): delete one first";
        return -1;
    }
    Bookmark b;
    // An id that is unique and sorts by creation: the wall-clock second plus a counter.
    {
        const time_t now = time(nullptr);
        struct tm lt;
#ifdef _WIN32
        localtime_s(&lt, &now);
#else
        localtime_r(&now, &lt);
#endif
        char buf[48];
        strftime(buf, sizeof(buf), "bm_%Y%m%d_%H%M%S", &lt);
        b.id = buf;
        for (int k = 2; std::any_of(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark &o) { return o.id == b.id; }); ++k)
            b.id = std::string(buf) + "_" + std::to_string(k);
    }
    b.name = name.empty() ? "Bookmark " + std::to_string(bookmarks_.size() + 1) : name;
    bookmarks_.push_back(b);
    bookmarkUpdate((int)bookmarks_.size() - 1);
    return (int)bookmarks_.size() - 1;
}

void SatelliteSim::bookmarkUpdate(int i)
{
    if (i < 0 || i >= (int)bookmarks_.size())
        return;
    Bookmark &b = bookmarks_[i];
    b.simT = (double)simDayJ2000 * 86400.0 + simSecInDay;
    // In follow mode obsDir / obsHeightOffset are the camera beside the satellite (updateFollow), so the
    // bookmark keeps the camera's place; going back to it is a free camera there (the satellite has moved on).
    b.obsDir = glm::normalize(glm::dvec3(obsDir));
    b.heightM = obsHeightOffset;
    b.az = camera.azDeg;
    b.el = camera.elDeg;
    b.fov = camera.fovYDeg;
    b.hasDrift = true;
    b.driftPhase = cloudDriftPhase();
    b.driftRate = cloudDriftRate;
    bookmarkFormatMeta(b);
    bookmarksSave();
    bookmarkRequestThumb(b.id);
}

void SatelliteSim::bookmarkGo(int i)
{
    if (i < 0 || i >= (int)bookmarks_.size())
        return;
    const Bookmark &b = bookmarks_[i];
    if (followActive)
        stopFollow();
    stopTrack(); // an explicit aim, like any camera key
    const double days = std::floor(b.simT / 86400.0);
    simDayJ2000 = (int64_t)days;
    simSecInDay = b.simT - days * 86400.0;
    obsDir = glm::normalize(glm::vec3(b.obsDir));
    obsLatDeg = glm::degrees(asinf(glm::clamp(obsDir.z, -1.0f, 1.0f)));
    obsLonDeg = glm::degrees(atan2f(obsDir.y, obsDir.x));
    obsHeightOffset = b.heightM;
    camera.fovYDeg = glm::clamp(b.fov, SkyCamera::kMinFovDeg, SkyCamera::kMaxFovDeg);
    aimCameraAzEl(b.az, b.el);
    if (b.hasDrift)
    {
        // The offset that reproduces the stored phase at the stored time (the harness snapshot's formula).
        cloudDriftRate = b.driftRate;
        cloudDriftPhaseOffset = std::fmod(b.driftPhase - (double)cloudDriftRate * (b.simT - kCloudDriftEpochS), glm::two_pi<double>());
    }
    obsTerrainH = cpuTerrainHeightM(obsLatDeg, obsLonDeg);
    updatePositions(b.simT, 0.0f);
    trailClearPending = true;
    skyTaaHistValid = false; // a cut: nothing temporal may carry the last view into this one
    cv2HistoryValid = false;
    // A bookmark whose thumbnail is missing (deleted, never captured) gets one once the view has settled.
    if (b.slot < 0 && b.thumbTried)
        bookmarkRequestThumb(b.id, 45);
}

void SatelliteSim::bookmarkDelete(int i)
{
    if (i < 0 || i >= (int)bookmarks_.size())
        return;
    std::error_code ec;
    fs::remove(fs::path(bookmarkDir()) / (bookmarks_[i].id + ".png"), ec);
    if (bmPendingThumb_ == bookmarks_[i].id)
        bmPendingThumb_.clear();
    bookmarks_.erase(bookmarks_.begin() + i);
    bookmarksSave();
}

// ─── Thumbnails ──────────────────────────────────────────────────────────────
void SatelliteSim::bookmarkRequestThumb(const std::string &id, int delayFrames)
{
    bmPendingThumb_ = id;
    bmPendingDelay_ = delayFrames;
}

// finalizeScreenshot: the clean frame the request asked for, as RGBA (sRGB-encoded bytes).
void SatelliteSim::bookmarkCaptureThumb(const std::vector<uint8_t> &rgba, uint32_t w, uint32_t h)
{
    const std::string id = bmCaptureFor_;
    bmCaptureFor_.clear();
    if (w == 0 || h == 0 || rgba.size() < (size_t)w * h * 4)
        return;
    // Centre crop to the thumbnail's 16:9, then a box filter (in the encoded values: a thumbnail).
    const double aspect = (double)kBmThumbW / kBmThumbH;
    uint32_t cw = w, ch = h;
    if ((double)w / h > aspect)
        cw = (uint32_t)std::lround(h * aspect);
    else
        ch = (uint32_t)std::lround(w / aspect);
    const uint32_t x0 = (w - cw) / 2, y0 = (h - ch) / 2;
    bmThumbPixels_.assign((size_t)kBmThumbW * kBmThumbH * 4, 0);
    for (int y = 0; y < kBmThumbH; ++y)
        for (int x = 0; x < kBmThumbW; ++x)
        {
            const uint32_t sx0 = x0 + (uint32_t)((uint64_t)x * cw / kBmThumbW), sx1 = std::max(sx0 + 1, x0 + (uint32_t)((uint64_t)(x + 1) * cw / kBmThumbW));
            const uint32_t sy0 = y0 + (uint32_t)((uint64_t)y * ch / kBmThumbH), sy1 = std::max(sy0 + 1, y0 + (uint32_t)((uint64_t)(y + 1) * ch / kBmThumbH));
            uint32_t acc[3] = {0, 0, 0}, n = 0;
            for (uint32_t yy = sy0; yy < sy1 && yy < h; ++yy)
                for (uint32_t xx = sx0; xx < sx1 && xx < w; ++xx, ++n)
                    for (int c = 0; c < 3; ++c)
                        acc[c] += rgba[((size_t)yy * w + xx) * 4 + c];
            uint8_t *d = &bmThumbPixels_[((size_t)y * kBmThumbW + x) * 4];
            for (int c = 0; c < 3; ++c)
                d[c] = (uint8_t)(n ? (acc[c] + n / 2) / n : 0);
            d[3] = 255;
        }
    bmThumbPixelsFor_ = id;
}

int SatelliteSim::bookmarkFreeSlot() const
{
    for (int s = 0; s < kBmMax; ++s)
        if (std::none_of(bookmarks_.begin(), bookmarks_.end(), [&](const Bookmark &b) { return b.slot == s; }))
            return s;
    return -1;
}

bool SatelliteSim::bookmarkEnsureAtlas(UIRenderer &ui)
{
    if (bmAtlasImg != VK_NULL_HANDLE)
        return bmAtlasUiId != 0;
    if (!ctx_)
        return false;
    VulkanContext &ctx = *ctx_;
    ctx.createImage(kBmAtlasW, kBmAtlasH, VK_FORMAT_R8G8B8A8_SRGB, VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
                    bmAtlasImg, bmAtlasMem);
    VkImageViewCreateInfo vi{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
    vi.image = bmAtlasImg;
    vi.viewType = VK_IMAGE_VIEW_TYPE_2D;
    vi.format = VK_FORMAT_R8G8B8A8_SRGB;
    vi.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCreateImageView(ctx.device, &vi, nullptr, &bmAtlasView);
    VkSamplerCreateInfo si{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
    si.magFilter = si.minFilter = VK_FILTER_LINEAR;
    si.addressModeU = si.addressModeV = si.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(ctx.device, &si, nullptr, &bmAtlasSampler);
    // Start dark (a cell with no thumbnail is never drawn, but filtering at a cell's edge reads its neighbour).
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, bmAtlasImg, 0, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_UNDEFINED,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    const VkClearColorValue clear{{0.01f, 0.01f, 0.012f, 1.0f}};
    const VkImageSubresourceRange range{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    vkCmdClearColorImage(cmd, bmAtlasImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, &clear, 1, &range);
    ctx.imageBarrier(cmd, bmAtlasImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    bmAtlasUiId = ui.registerImage(ctx.device, bmAtlasView, bmAtlasSampler);
    if (bmAtlasUiId == 0)
        Log::line("bookmarks: no free UI image slot for the thumbnail atlas; thumbnails are off");
    return bmAtlasUiId != 0;
}

void SatelliteSim::bookmarkUploadThumb(int slot, const uint8_t *rgba)
{
    if (!ctx_ || bmAtlasImg == VK_NULL_HANDLE || slot < 0 || slot >= kBmMax)
        return;
    VulkanContext &ctx = *ctx_;
    const VkDeviceSize bytes = (VkDeviceSize)kBmThumbW * kBmThumbH * 4;
    VkBuffer stage = VK_NULL_HANDLE;
    VkDeviceMemory stageMem = VK_NULL_HANDLE;
    ctx.createBuffer(bytes, VK_BUFFER_USAGE_TRANSFER_SRC_BIT, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT,
                     stage, stageMem);
    void *mapped = nullptr;
    vkMapMemory(ctx.device, stageMem, 0, bytes, 0, &mapped);
    memcpy(mapped, rgba, (size_t)bytes);
    vkUnmapMemory(ctx.device, stageMem);
    const int cols = kBmAtlasW / kBmThumbW;
    VkCommandBuffer cmd = ctx.beginOneTimeCommands();
    ctx.imageBarrier(cmd, bmAtlasImg, VK_ACCESS_SHADER_READ_BIT, VK_ACCESS_TRANSFER_WRITE_BIT, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                     VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageOffset = {(slot % cols) * kBmThumbW, (slot / cols) * kBmThumbH, 0};
    region.imageExtent = {(uint32_t)kBmThumbW, (uint32_t)kBmThumbH, 1};
    vkCmdCopyBufferToImage(cmd, stage, bmAtlasImg, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    ctx.imageBarrier(cmd, bmAtlasImg, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                     VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT);
    ctx.endOneTimeCommands(cmd);
    vkDestroyBuffer(ctx.device, stage, nullptr);
    vkFreeMemory(ctx.device, stageMem, nullptr);
}

namespace
{
    // A cell's sub-rect, inset half a texel so linear filtering never reads the neighbouring cell.
    UIImage bmCellImage(uint32_t uiId, int slot, int atlasW, int atlasH, int cellW, int cellH)
    {
        const int cols = atlasW / cellW;
        const float x = (float)((slot % cols) * cellW), y = (float)((slot / cols) * cellH);
        UIImage im;
        im.imageId = uiId;
        im.u0 = (x + 0.5f) / atlasW;
        im.v0 = (y + 0.5f) / atlasH;
        im.u1 = (x + cellW - 0.5f) / atlasW;
        im.v1 = (y + cellH - 0.5f) / atlasH;
        return im;
    }
}

void SatelliteSim::bookmarkLoadThumb(Bookmark &b)
{
    b.thumbTried = true;
    const std::string path = (fs::path(bookmarkDir()) / (b.id + ".png")).string();
    std::error_code ec;
    if (!fs::exists(path, ec))
        return;
    int w = 0, h = 0, n = 0;
    stbi_uc *px = stbi_load(path.c_str(), &w, &h, &n, 4);
    if (!px)
        return;
    std::vector<uint8_t> cell((size_t)kBmThumbW * kBmThumbH * 4);
    for (int y = 0; y < kBmThumbH; ++y) // nearest: a thumbnail written by this file is already the cell's size
        for (int x = 0; x < kBmThumbW; ++x)
            memcpy(&cell[((size_t)y * kBmThumbW + x) * 4], &px[((size_t)(y * h / kBmThumbH) * w + (size_t)(x * w / kBmThumbW)) * 4], 4);
    stbi_image_free(px);
    const int slot = b.slot >= 0 ? b.slot : bookmarkFreeSlot();
    if (slot < 0)
        return;
    bookmarkUploadThumb(slot, cell.data());
    b.slot = slot;
    b.img = bmCellImage(bmAtlasUiId, slot, kBmAtlasW, kBmAtlasH, kBmThumbW, kBmThumbH);
}

// buildUI, every frame (the window may be closed): load the list, ask for a pending thumbnail's frame, upload a
// captured one, and load the saved thumbnails a few per frame once the window is open.
void SatelliteSim::bookmarkTick(UIRenderer &ui)
{
    if (!bookmarksLoaded_)
        bookmarksLoad();
    // The window's Add / Delete from last frame (deferred: see the end of buildBookmarksWindow).
    if (bmDeletePending_ >= 0)
    {
        bookmarkDelete(bmDeletePending_);
        bmDeletePending_ = -1;
    }
    if (bmAddPending_)
    {
        bmAddPending_ = false;
        bmStatus_.clear();
        bookmarkAdd(bmNewName_);
        bmNewName_.clear();
    }
    if (!bmThumbPixels_.empty())
    {
        for (Bookmark &b : bookmarks_)
            if (b.id == bmThumbPixelsFor_)
            {
                std::error_code ec;
                fs::create_directories(bookmarkDir(), ec);
                const std::string path = (fs::path(bookmarkDir()) / (b.id + ".png")).string();
                if (!stbi_write_png(path.c_str(), kBmThumbW, kBmThumbH, 4, bmThumbPixels_.data(), kBmThumbW * 4))
                    bmStatus_ = "Could not write the thumbnail";
                b.thumbTried = true;
                if (bookmarkEnsureAtlas(ui))
                {
                    const int slot = b.slot >= 0 ? b.slot : bookmarkFreeSlot();
                    if (slot >= 0)
                    {
                        bookmarkUploadThumb(slot, bmThumbPixels_.data());
                        b.slot = slot;
                        b.img = bmCellImage(bmAtlasUiId, slot, kBmAtlasW, kBmAtlasH, kBmThumbW, kBmThumbH);
                    }
                }
            }
        bmThumbPixels_.clear();
        bmThumbPixelsFor_.clear();
    }
    if (!bmPendingThumb_.empty() && bmCaptureFor_.empty())
    {
        if (bmPendingDelay_ > 0)
            --bmPendingDelay_;
        else if (ctx_ && ctx_->screenshotSupported && !screenshotEncoding.load() && !screenshotCopyPending && !screenshotRequested &&
                 photoState == 0 && !cineActive())
        {
            // The screenshot path's clean frame (wantsCleanScreenshot leaves the UI out); finalizeScreenshot
            // passes its pixels to bookmarkCaptureThumb instead of writing a file.
            bmCaptureFor_ = bmPendingThumb_;
            bmPendingThumb_.clear();
            screenshotIncludeUI = false;
            screenshotRequested = true;
        }
    }
    if (bmChrome.open)
    {
        int loads = 0;
        for (Bookmark &b : bookmarks_)
            if (b.slot < 0 && !b.thumbTried && loads < 4 && bookmarkEnsureAtlas(ui))
            {
                bookmarkLoadThumb(b);
                ++loads;
            }
    }
}

void SatelliteSim::bookmarksDestroy(VkDevice device)
{
    if (bmAtlasSampler)
        vkDestroySampler(device, bmAtlasSampler, nullptr);
    if (bmAtlasView)
        vkDestroyImageView(device, bmAtlasView, nullptr);
    if (bmAtlasImg)
        vkDestroyImage(device, bmAtlasImg, nullptr);
    if (bmAtlasMem)
        vkFreeMemory(device, bmAtlasMem, nullptr);
    bmAtlasSampler = VK_NULL_HANDLE;
    bmAtlasView = VK_NULL_HANDLE;
    bmAtlasImg = VK_NULL_HANDLE;
    bmAtlasMem = VK_NULL_HANDLE;
}

// ─── The window ──────────────────────────────────────────────────────────────
// A name field and Add at the top, then the bookmarks as a grid of cards: the thumbnail (click: go there), the
// name (click to rename), when and where, and Go / Update / Delete (Delete asks once more).
void SatelliteSim::buildBookmarksWindow(const UIInput &inp, UIRenderer &ui)
{
    if (!bmChrome.open)
        return;
    if (bmChrome.w <= 0.0f)
    {
        bmChrome.w = std::min(640.0f, inp.screenW - 40.0f);
        bmChrome.h = std::max(360.0f, std::min(620.0f, inp.screenH - 240.0f));
    }
    auto text = [&](const char *s, Clay_Color c, float size)
    {
        CLAY_TEXT((Clay_String{false, (int32_t)strlen(s), s}), CLAY_TEXT_CONFIG({.textColor = c, .fontSize = fs((int)size), .wrapMode = CLAY_TEXT_WRAP_NONE}));
    };
    int goIdx = -1, updIdx = -1, delIdx = -1, renamed = -1;
    bool add = false;
    static char titleBuf[48];
    snprintf(titleBuf, sizeof(titleBuf), "Bookmarks (%d)", (int)bookmarks_.size());

    buildResizableWindow(
        inp, ui, bmChrome, 6, titleBuf, true, hovBmClose, (inp.screenW - bmChrome.w) * 0.5f, 110.0f, 340.0f, 300.0f, 1600.0f,
        1400.0f,
        [&]()
        {
            CLAY(CLAY_ID("BmBody"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                .padding = {12, 12, 8, 10},
                                                .childGap = 6,
                                                .layoutDirection = CLAY_TOP_TO_BOTTOM}})
            {
                // ── Add the current view ───────────────────────────────────────────────────────────────
                CLAY(CLAY_ID("BmAddRow"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                      .childGap = 6,
                                                      .childAlignment = {.y = CLAY_ALIGN_Y_CENTER},
                                                      .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                {
                    text("Name", Pal::textDim, 11);
                    const std::string placeholder = "Bookmark " + std::to_string(bookmarks_.size() + 1);
                    std::string nm = bmNewName_.empty() ? placeholder : bmNewName_;
                    if (textField(inp, ui, CLAY_ID("BmNewName"), nm, (float)fs(11) * 14.0f, 12, "The new bookmark's name"))
                        bmNewName_ = nm == placeholder ? std::string() : nm;
                    if (uiButton(inp, ui, "BmAdd", 0, "Add current view", "Save this place, camera and moment, with a thumbnail",
                                 false, (int)bookmarks_.size() < kBmMax))
                        add = true;
                }
                if (!bmStatus_.empty())
                    text(bmStatus_.c_str(), {230, 190, 120, 255}, 11);

                // ── The cards ──────────────────────────────────────────────────────────────────────────
                CLAY(CLAY_ID("BmList"), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)},
                                                    .padding = {0, 8, 4, 4},
                                                    .childGap = 8,
                                                    .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                         .clip = {.vertical = true, .childOffset = Clay_GetScrollOffset()}})
                {
                    if (bookmarks_.empty())
                        text("No bookmarks yet", Pal::textDim, 12);
                    // Columns from the list's width in the last layout (Clay has no wrapping rows).
                    const Clay_ElementData ld = Clay_GetElementData(CLAY_ID("BmList"));
                    const float listW = std::max(200.0f, (ld.found ? ld.boundingBox.width : bmChrome.w - 24.0f) - 8.0f);
                    const float gap = 8.0f, want = (float)fs(11) * 16.0f;
                    const int cols = std::max(1, (int)((listW + gap) / (want + gap)));
                    const float cardW = std::floor((listW - gap * (cols - 1)) / cols);
                    const float imgW = cardW - 12.0f, imgH = std::floor(imgW * kBmThumbH / (float)kBmThumbW);
                    const bool thumbs = bmAtlasUiId != 0;
                    for (int r0 = 0; r0 < (int)bookmarks_.size(); r0 += cols)
                    {
                        CLAY(CLAY_IDI("BmRow", r0), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                .childGap = (uint16_t)gap,
                                                                .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                        {
                            for (int i = r0; i < r0 + cols && i < (int)bookmarks_.size(); ++i)
                            {
                                Bookmark &b = bookmarks_[i];
                                CLAY(CLAY_IDI("BmCard", i), {.layout = {.sizing = {CLAY_SIZING_FIXED(cardW), CLAY_SIZING_FIT(0)},
                                                                        .padding = {6, 6, 6, 6},
                                                                        .childGap = 4,
                                                                        .layoutDirection = CLAY_TOP_TO_BOTTOM},
                                                             .backgroundColor = {14, 14, 16, 220},
                                                             .cornerRadius = CLAY_CORNER_RADIUS(4)})
                                {
                                    // The thumbnail: click to go there. The black backing is on this parent (a
                                    // UIImage element must not have its own background — see UIImage).
                                    const Clay_ElementId tid = CLAY_IDI("BmThumb", i);
                                    const bool hovT = Clay_PointerOver(tid);
                                    CLAY(tid, {.layout = {.sizing = {CLAY_SIZING_FIXED(imgW), CLAY_SIZING_FIXED(imgH)},
                                                          .childAlignment = {.x = CLAY_ALIGN_X_CENTER, .y = CLAY_ALIGN_Y_CENTER}},
                                               .backgroundColor = {4, 4, 5, 255},
                                               .border = {.color = hovT ? Pal::btnAccent : Clay_Color{40, 40, 44, 255}, .width = CLAY_BORDER_ALL(1)}})
                                    {
                                        const bool n = Clay_Hovered();
                                        uiHovRoll(tid.id, n);
                                        ui.tooltip(inp, n, "Go here", fs(11));
                                        if (n && inp.lmbPressed)
                                            goIdx = i;
                                        if (thumbs && b.slot >= 0)
                                        {
                                            CLAY(CLAY_IDI("BmThumbImg", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_GROW(0)}},
                                                                             .custom = {.customData = &b.img}}) {}
                                        }
                                        else
                                            text(bmPendingThumb_ == b.id || bmCaptureFor_ == b.id ? "..." : "No image", Pal::textHint, 11);
                                    }
                                    if (textField(inp, ui, CLAY_IDI("BmName", i), b.name, imgW, 12, "Click to rename", 48))
                                        renamed = i;
                                    text(b.meta[0], Pal::volValue, 11);
                                    text(b.meta[1], Pal::textDim, 11);
                                    CLAY(CLAY_IDI("BmBtns", i), {.layout = {.sizing = {CLAY_SIZING_GROW(0), CLAY_SIZING_FIT(0)},
                                                                            .childGap = 4,
                                                                            .layoutDirection = CLAY_LEFT_TO_RIGHT}})
                                    {
                                        if (uiButton(inp, ui, "BmGo", i, "Go", "Go to this place and moment", false, true, true))
                                            goIdx = i;
                                        if (uiButton(inp, ui, "BmUpdate", i, "Update", "Replace it with the current view (and its thumbnail)",
                                                     false, true, true))
                                            updIdx = i;
                                        const bool armed = bmDeleteArmed_ == b.id;
                                        if (uiButton(inp, ui, "BmDelete", i, armed ? "Confirm" : "Delete",
                                                     armed ? "Click again to delete it" : "Delete (asks once more)", armed, true, true))
                                        {
                                            if (armed)
                                                delIdx = i;
                                            else
                                                bmDeleteArmed_ = b.id;
                                        }
                                    }
                                }
                            }
                        }
                    }
                }
                ui.scrollbar(CLAY_ID("BmList"));
            }
        });

    // After the layout. Clay keeps pointers into bookmarks_ (the thumbnails' UIImage, the meta strings) until the
    // frame is recorded, so Add and Delete — which grow or shrink the vector — wait for the next bookmarkTick,
    // which runs before any layout. Go / Update / a rename change no element's address.
    if (inp.lmbPressed && delIdx < 0 && !bmDeleteArmed_.empty())
    {
        // Any click other than the armed Delete disarms it.
        bool onArmed = false;
        for (int i = 0; i < (int)bookmarks_.size(); ++i)
            if (bookmarks_[i].id == bmDeleteArmed_ && uiHov_[CLAY_SIDI(CLAY_STRING("BmDelete"), (uint32_t)i).id])
                onArmed = true;
        if (!onArmed)
            bmDeleteArmed_.clear();
    }
    if (renamed >= 0)
        bookmarksSave();
    if (delIdx >= 0)
    {
        bmDeleteArmed_.clear();
        bmDeletePending_ = delIdx;
    }
    if (goIdx >= 0)
        bookmarkGo(goIdx);
    if (updIdx >= 0)
        bookmarkUpdate(updIdx);
    if (add)
        bmAddPending_ = true;
}
