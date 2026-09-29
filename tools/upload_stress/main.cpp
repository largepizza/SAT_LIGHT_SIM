// UploadStress: the machine-freeze reproducer for the GPU side of the launch (docs/FREEZES.md).
//
// Every boot freeze on record stops in the texture step, most often on earth_elevation.png.
// tools/harness/stress_decode.py showed the CPU/RAM half of that step (the PNG decode) survives
// ~950 cycles on its own, so this tool repeats the OTHER half with no app, no window and no
// shaders: exactly SatelliteSim's upload path for each texture —
//   host-visible staging buffer -> memcpy -> device-local image with a full mip chain ->
//   one command buffer (UNDEFINED->TRANSFER_DST barrier, vkCmdCopyBufferToImage, the
//   generateMipmaps blit chain) -> vkQueueSubmit + vkQueueWaitIdle -> destroy + free.
// By default every cycle also creates and destroys the Vulkan instance and device, as each app
// launch does (--device once keeps one device for the whole run instead).
//
// Textures are decoded ONCE at startup with the same stb_image calls and channel counts the app
// uses, so the cycles are pure GPU work. Each step appends one fsynced line to
// <out>/<local start time>.log, so after a freeze its last line names the cycle and the step.
//
//   cmake --build build --config Release --target UploadStress
//   build/Release/UploadStress.exe                        # DEM only, fresh device per cycle, 2 s idle
//   build/Release/UploadStress.exe --set launch           # all nine textures in init order
//   build/Release/UploadStress.exe --device once --gap 0  # one device, back-to-back uploads
//   build/Release/UploadStress.exe --decode-only          # stbi_load in a loop, no Vulkan calls at all
// Run from the repository root (texture paths are relative), or pass --root.
#define STB_IMAGE_IMPLEMENTATION
#include "stb_image.h"

#include <vulkan/vulkan.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#include <io.h>
#define SYNC_FILE(f) _commit(_fileno(f))
#else
#include <unistd.h>
#define SYNC_FILE(f) fsync(fileno(f))
#endif

namespace fs = std::filesystem;

// ── fsynced log ───────────────────────────────────────────────────────────────
static FILE *gLog = nullptr;

static void logLine(const char *fmt, ...)
{
    using namespace std::chrono;
    auto now = system_clock::now();
    std::time_t t = system_clock::to_time_t(now);
    int ms = (int)(duration_cast<milliseconds>(now.time_since_epoch()).count() % 1000);
    std::tm tmv{};
#ifdef _WIN32
    gmtime_s(&tmv, &t);
#else
    gmtime_r(&t, &tmv);
#endif
    char stamp[40];
    std::snprintf(stamp, sizeof stamp, "%04d-%02d-%02dT%02d:%02d:%02d.%03dZ", tmv.tm_year + 1900,
                  tmv.tm_mon + 1, tmv.tm_mday, tmv.tm_hour, tmv.tm_min, tmv.tm_sec, ms);
    char msg[512];
    va_list ap;
    va_start(ap, fmt);
    std::vsnprintf(msg, sizeof msg, fmt, ap);
    va_end(ap);
    std::printf("[%s] %s\n", stamp, msg);
    std::fflush(stdout);
    if (gLog)
    {
        std::fprintf(gLog, "[%s] %s\n", stamp, msg);
        std::fflush(gLog);
        SYNC_FILE(gLog);
    }
}

static void check(VkResult r, const char *what)
{
    if (r != VK_SUCCESS)
    {
        logLine("FATAL: %s failed (VkResult %d)", what, (int)r);
        throw std::runtime_error(what);
    }
}

// ── textures ─────────────────────────────────────────────────────────────────
struct Tex
{
    const char *path;
    int comp;          // channels the app asks stb_image for
    VkFormat format;   // the image format the app creates
    bool mips;         // the app builds a mip chain for it
    int w = 0, h = 0;
    std::vector<unsigned char> pixels;
};

// SatelliteSim::init's order and formats.
static std::vector<Tex> launchSet()
{
    return {
        {"assets/noise/rgba_noise.png", 4, VK_FORMAT_R8G8B8A8_UNORM, false},
        {"assets/textures/full_moon.png", 4, VK_FORMAT_R8G8B8A8_SRGB, true},
        {"assets/textures/8k_earth_daymap.jpg", 4, VK_FORMAT_R8G8B8A8_SRGB, true},
        {"assets/textures/8k_stars_milky_way.jpg", 4, VK_FORMAT_R8G8B8A8_SRGB, true},
        {"assets/textures/8k_earth_nightmap.jpg", 4, VK_FORMAT_R8G8B8A8_SRGB, true},
        {"assets/textures/city_day_detail.jpg", 4, VK_FORMAT_R8G8B8A8_SRGB, true},
        {"assets/textures/city_night_detail.jpg", 4, VK_FORMAT_R8G8B8A8_SRGB, true},
        {"assets/textures/earth_elevation.png", 1, VK_FORMAT_R8_UNORM, true},
        {"assets/textures/8k_earth_specular_map.png", 1, VK_FORMAT_R8_UNORM, true},
        {"assets/textures/8k_earth_clouds.jpg", 1, VK_FORMAT_R8_UNORM, true},
    };
}

// ── the Vulkan context (instance + device + queue + pool) ────────────────────
struct Gpu
{
    VkInstance instance = VK_NULL_HANDLE;
    VkPhysicalDevice phys = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue queue = VK_NULL_HANDLE;
    uint32_t queueFamily = 0;
    VkCommandPool pool = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties memProps{};

    void create()
    {
        VkApplicationInfo app{VK_STRUCTURE_TYPE_APPLICATION_INFO};
        app.pApplicationName = "UploadStress";
        app.apiVersion = VK_API_VERSION_1_2;
        VkInstanceCreateInfo ici{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
        ici.pApplicationInfo = &app;
        check(vkCreateInstance(&ici, nullptr, &instance), "vkCreateInstance");

        uint32_t n = 0;
        vkEnumeratePhysicalDevices(instance, &n, nullptr);
        std::vector<VkPhysicalDevice> devs(n);
        vkEnumeratePhysicalDevices(instance, &n, devs.data());
        if (devs.empty())
            throw std::runtime_error("no Vulkan device");
        phys = devs[0];
        for (VkPhysicalDevice d : devs)
        {
            VkPhysicalDeviceProperties p;
            vkGetPhysicalDeviceProperties(d, &p);
            if (p.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
            {
                phys = d;
                break;
            }
        }
        vkGetPhysicalDeviceMemoryProperties(phys, &memProps);

        uint32_t qn = 0;
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, nullptr);
        std::vector<VkQueueFamilyProperties> qf(qn);
        vkGetPhysicalDeviceQueueFamilyProperties(phys, &qn, qf.data());
        queueFamily = UINT32_MAX;
        for (uint32_t i = 0; i < qn; ++i)
            if (qf[i].queueFlags & VK_QUEUE_GRAPHICS_BIT)
            {
                queueFamily = i;
                break;
            }
        if (queueFamily == UINT32_MAX)
            throw std::runtime_error("no graphics queue");

        float prio = 1.0f;
        VkDeviceQueueCreateInfo qci{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
        qci.queueFamilyIndex = queueFamily;
        qci.queueCount = 1;
        qci.pQueuePriorities = &prio;
        VkDeviceCreateInfo dci{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
        dci.queueCreateInfoCount = 1;
        dci.pQueueCreateInfos = &qci;
        check(vkCreateDevice(phys, &dci, nullptr, &device), "vkCreateDevice");
        vkGetDeviceQueue(device, queueFamily, 0, &queue);

        VkCommandPoolCreateInfo pci{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
        pci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pci.queueFamilyIndex = queueFamily;
        check(vkCreateCommandPool(device, &pci, nullptr, &pool), "vkCreateCommandPool");
    }

    void destroy()
    {
        if (device)
        {
            vkDeviceWaitIdle(device);
            vkDestroyCommandPool(device, pool, nullptr);
            vkDestroyDevice(device, nullptr);
        }
        if (instance)
            vkDestroyInstance(instance, nullptr);
        *this = Gpu{};
    }

    uint32_t memType(uint32_t filter, VkMemoryPropertyFlags props) const
    {
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i)
            if ((filter & (1u << i)) && (memProps.memoryTypes[i].propertyFlags & props) == props)
                return i;
        throw std::runtime_error("no memory type");
    }

    VkFilter blitFilter(VkFormat fmt) const
    {
        VkFormatProperties p{};
        vkGetPhysicalDeviceFormatProperties(phys, fmt, &p);
        return (p.optimalTilingFeatures & VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT)
                   ? VK_FILTER_LINEAR
                   : VK_FILTER_NEAREST;
    }
};

// VulkanContext::generateMipmaps, verbatim in behaviour.
static void generateMipmaps(const Gpu &g, VkCommandBuffer cmd, VkImage img, VkFormat fmt, uint32_t w,
                            uint32_t h, uint32_t mipLevels)
{
    const VkFilter filter = g.blitFilter(fmt);
    VkImageMemoryBarrier b{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    b.image = img;
    b.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    b.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
    int32_t mw = (int32_t)w, mh = (int32_t)h;
    for (uint32_t i = 1; i < mipLevels; ++i)
    {
        b.subresourceRange.baseMipLevel = i - 1;
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        b.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
                             nullptr, 0, nullptr, 1, &b);
        int32_t nw = mw > 1 ? mw / 2 : 1, nh = mh > 1 ? mh / 2 : 1;
        VkImageBlit blit{};
        blit.srcOffsets[1] = {mw, mh, 1};
        blit.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i - 1, 0, 1};
        blit.dstOffsets[1] = {nw, nh, 1};
        blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, i, 0, 1};
        vkCmdBlitImage(cmd, img, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, img,
                       VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, filter);
        b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
        b.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
        vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0,
                             0, nullptr, 0, nullptr, 1, &b);
        mw = nw;
        mh = nh;
    }
    b.subresourceRange.baseMipLevel = mipLevels - 1;
    b.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    b.newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    b.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    b.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT, 0, 0,
                         nullptr, 0, nullptr, 1, &b);
}

// One texture through the app's upload path; everything is freed before returning.
static void uploadOnce(const Gpu &g, const Tex &t, int cycle)
{
    const char *name = std::strrchr(t.path, '/') ? std::strrchr(t.path, '/') + 1 : t.path;
    const VkDeviceSize bytes = (VkDeviceSize)t.w * t.h * t.comp;
    const uint32_t mips = t.mips ? (uint32_t)std::floor(std::log2((float)std::max(t.w, t.h))) + 1 : 1;
    auto t0 = std::chrono::steady_clock::now();
    logLine("cycle %d upload %s: staging %.0f MB", cycle, name, bytes / 1e6);

    // Staging buffer (HOST_VISIBLE | HOST_COHERENT), memcpy, unmap.
    VkBuffer stage;
    VkDeviceMemory stageMem;
    VkBufferCreateInfo bci{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
    bci.size = bytes;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    check(vkCreateBuffer(g.device, &bci, nullptr, &stage), "vkCreateBuffer");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(g.device, stage, &req);
    VkMemoryAllocateInfo ai{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g.memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT |
                                                           VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    check(vkAllocateMemory(g.device, &ai, nullptr, &stageMem), "vkAllocateMemory(staging)");
    vkBindBufferMemory(g.device, stage, stageMem, 0);
    void *mapped;
    check(vkMapMemory(g.device, stageMem, 0, bytes, 0, &mapped), "vkMapMemory");
    std::memcpy(mapped, t.pixels.data(), (size_t)bytes);
    vkUnmapMemory(g.device, stageMem);

    // Device-local image with the mip chain.
    logLine("cycle %d upload %s: image %dx%d, %u mips", cycle, name, t.w, t.h, mips);
    VkImage img;
    VkDeviceMemory imgMem;
    VkImageCreateInfo ici{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = t.format;
    ici.extent = {(uint32_t)t.w, (uint32_t)t.h, 1};
    ici.mipLevels = mips;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    check(vkCreateImage(g.device, &ici, nullptr, &img), "vkCreateImage");
    vkGetImageMemoryRequirements(g.device, img, &req);
    ai.allocationSize = req.size;
    ai.memoryTypeIndex = g.memType(req.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    check(vkAllocateMemory(g.device, &ai, nullptr, &imgMem), "vkAllocateMemory(image)");
    vkBindImageMemory(g.device, img, imgMem, 0);

    // One command buffer: barrier, copy, mips; submit + wait idle (endOneTimeCommands).
    VkCommandBufferAllocateInfo cai{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
    cai.commandPool = g.pool;
    cai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
    cai.commandBufferCount = 1;
    VkCommandBuffer cmd;
    check(vkAllocateCommandBuffers(g.device, &cai, &cmd), "vkAllocateCommandBuffers");
    VkCommandBufferBeginInfo cbi{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
    cbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
    vkBeginCommandBuffer(cmd, &cbi);
    VkImageMemoryBarrier all{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
    all.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    all.oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    all.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    all.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    all.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    all.image = img;
    all.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, mips, 0, 1};
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr,
                         0, nullptr, 1, &all);
    VkBufferImageCopy region{};
    region.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
    region.imageExtent = {(uint32_t)t.w, (uint32_t)t.h, 1};
    vkCmdCopyBufferToImage(cmd, stage, img, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &region);
    if (mips > 1)
        generateMipmaps(g, cmd, img, t.format, (uint32_t)t.w, (uint32_t)t.h, mips);
    vkEndCommandBuffer(cmd);

    logLine("cycle %d upload %s: submit", cycle, name);
    VkSubmitInfo si{VK_STRUCTURE_TYPE_SUBMIT_INFO};
    si.commandBufferCount = 1;
    si.pCommandBuffers = &cmd;
    check(vkQueueSubmit(g.queue, 1, &si, VK_NULL_HANDLE), "vkQueueSubmit");
    check(vkQueueWaitIdle(g.queue), "vkQueueWaitIdle");
    vkFreeCommandBuffers(g.device, g.pool, 1, &cmd);

    vkDestroyBuffer(g.device, stage, nullptr);
    vkFreeMemory(g.device, stageMem, nullptr);
    vkDestroyImage(g.device, img, nullptr);
    vkFreeMemory(g.device, imgMem, nullptr);
    double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    logLine("cycle %d upload %s: done %.0f ms", cycle, name, ms);
}

int main(int argc, char **argv)
{
    std::string set = "dem", device = "cycle", root = ".", out = "harness_runs/upload_stress";
    double gap = 2.0, minutes = 30.0;
    long cycles = 0;
    bool decodeOnly = false;
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        auto next = [&]() -> std::string {
            if (i + 1 >= argc)
            {
                std::fprintf(stderr, "%s needs a value\n", a.c_str());
                std::exit(2);
            }
            return argv[++i];
        };
        if (a == "--set") set = next();
        else if (a == "--device") device = next();
        else if (a == "--gap") gap = std::atof(next().c_str());
        else if (a == "--minutes") minutes = std::atof(next().c_str());
        else if (a == "--cycles") cycles = std::atol(next().c_str());
        else if (a == "--root") root = next();
        else if (a == "--out") out = next();
        else if (a == "--decode-only") decodeOnly = true;
        else
        {
            std::printf("UploadStress [--set dem|launch] [--device cycle|once] [--gap S] [--minutes M]\n"
                        "             [--cycles N] [--root <repo>] [--out <dir>]\n");
            return a == "--help" || a == "-h" ? 0 : 2;
        }
    }
    if ((set != "dem" && set != "launch") || (device != "cycle" && device != "once"))
    {
        std::fprintf(stderr, "bad --set or --device\n");
        return 2;
    }
    fs::current_path(root);

    fs::create_directories(out);
    {
        std::time_t t = std::time(nullptr);
        std::tm lt{};
#ifdef _WIN32
        localtime_s(&lt, &t);
#else
        localtime_r(&t, &lt);
#endif
        char name[64];
        std::strftime(name, sizeof name, "%Y%m%d_%H%M%S.log", &lt);
        gLog = std::fopen((fs::path(out) / name).string().c_str(), "a");
        std::printf("log: %s\n", (fs::path(out) / name).string().c_str());
    }

    std::vector<Tex> texs = launchSet();
    if (set == "dem")
        texs.erase(std::remove_if(texs.begin(), texs.end(),
                                  [](const Tex &t) { return std::strstr(t.path, "earth_elevation") == nullptr; }),
                   texs.end());

    logLine("UploadStress start: set=%s device=%s gap=%gs minutes=%g cycles=%ld%s", set.c_str(), device.c_str(),
            gap, minutes, cycles, decodeOnly ? " DECODE-ONLY (no Vulkan)" : "");

    // --decode-only: the app's stbi_load calls in a loop, nothing else. The first run of this
    // tool froze the machine inside its startup stbi_load of earth_elevation.png, before any
    // Vulkan call (2026-09-29 07:49Z) — this is that step on its own.
    if (decodeOnly)
    {
        auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(minutes * 60.0);
        long cycle = 0;
        while (std::chrono::steady_clock::now() < deadline && (cycles == 0 || cycle < cycles))
        {
            ++cycle;
            for (const Tex &t : texs)
            {
                logLine("cycle %ld stbi_load %s: start", cycle, t.path);
                auto t0 = std::chrono::steady_clock::now();
                int w, h, ch;
                unsigned char *p = stbi_load(t.path, &w, &h, &ch, t.comp);
                if (!p)
                {
                    logLine("FATAL: could not decode %s", t.path);
                    return 1;
                }
                stbi_image_free(p);
                double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
                logLine("cycle %ld stbi_load %s: done %dx%d x%d %.0f ms", cycle, t.path, w, h, t.comp, ms);
            }
            if (gap > 0)
                std::this_thread::sleep_for(std::chrono::duration<double>(gap));
        }
        logLine("UploadStress decode-only finished cleanly: %ld cycles", cycle);
        return 0;
    }

    for (Tex &t : texs)
    {
        logLine("stbi_load %s: start", t.path);
        int ch;
        unsigned char *p = stbi_load(t.path, &t.w, &t.h, &ch, t.comp);
        if (!p)
        {
            logLine("FATAL: could not decode %s", t.path);
            return 1;
        }
        t.pixels.assign(p, p + (size_t)t.w * t.h * t.comp);
        stbi_image_free(p);
        logLine("decoded %s %dx%d x%d", t.path, t.w, t.h, t.comp);
    }

    Gpu gpu;
    auto deadline = std::chrono::steady_clock::now() + std::chrono::duration<double>(minutes * 60.0);
    long cycle = 0;
    try
    {
        if (device == "once")
        {
            logLine("device: create");
            gpu.create();
        }
        while (std::chrono::steady_clock::now() < deadline && (cycles == 0 || cycle < cycles))
        {
            ++cycle;
            auto c0 = std::chrono::steady_clock::now();
            if (device == "cycle")
            {
                logLine("cycle %ld device: create", cycle);
                gpu.create();
            }
            for (const Tex &t : texs)
                uploadOnce(gpu, t, (int)cycle);
            if (device == "cycle")
            {
                logLine("cycle %ld device: destroy", cycle);
                gpu.destroy();
            }
            double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - c0).count();
            logLine("cycle %ld complete %.0f ms; idle %g s", cycle, ms, gap);
            if (gap > 0)
                std::this_thread::sleep_for(std::chrono::duration<double>(gap));
        }
        gpu.destroy();
    }
    catch (const std::exception &e)
    {
        logLine("stopped on error after %ld cycles: %s", cycle, e.what());
        return 1;
    }
    logLine("UploadStress finished cleanly: %ld cycles", cycle);
    return 0;
}
