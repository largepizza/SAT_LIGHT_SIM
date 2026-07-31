#include "Paths.h"

#include <cstdlib>
#include <filesystem>

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

namespace Paths
{
    std::string exeDir()
    {
#if defined(_WIN32)
        char buf[MAX_PATH] = {};
        GetModuleFileNameA(nullptr, buf, MAX_PATH);
        return std::filesystem::path(buf).parent_path().string();

#elif defined(__linux__)
        char buf[PATH_MAX] = {};
        ssize_t len = ::readlink("/proc/self/exe", buf, sizeof(buf) - 1);
        if (len != -1)
            return std::filesystem::path(std::string(buf, len)).parent_path().string();

#elif defined(__APPLE__)
        char buf[PATH_MAX] = {};
        uint32_t size = sizeof(buf);
        if (_NSGetExecutablePath(buf, &size) == 0)
            return std::filesystem::path(buf).parent_path().string();
#endif

        // Universal fallback: use current working directory
        return std::filesystem::current_path().string();
    }

    std::string userDataDir()
    {
        std::filesystem::path dir;
        std::error_code ec;

        // Portable mode: a "portable.txt" sentinel next to the exe keeps every
        // write (settings, logs, perf snapshots) alongside the executable
        // instead of the OS per-user directory — useful for a zip-and-run
        // distribution where nothing should touch the user's profile.
        std::filesystem::path exe = exeDir();
        if (std::filesystem::exists(exe / "portable.txt", ec))
            return exe.string();

#if defined(_WIN32)
        const char *appData = std::getenv("APPDATA");
        dir = appData ? std::filesystem::path(appData) / "SatLightSim" : exe;
#elif defined(__linux__)
        const char *xdg = std::getenv("XDG_DATA_HOME");
        const char *home = std::getenv("HOME");
        if (xdg)
            dir = std::filesystem::path(xdg) / "SatLightSim";
        else if (home)
            dir = std::filesystem::path(home) / ".local" / "share" / "SatLightSim";
        else
            dir = exe;
#elif defined(__APPLE__)
        const char *home = std::getenv("HOME");
        dir = home ? std::filesystem::path(home) / "Library" / "Application Support" / "SatLightSim" : exe;
#else
        dir = exe;
#endif

        std::filesystem::create_directories(dir, ec); // best-effort; callers handle write failures
        return dir.string();
    }
}
