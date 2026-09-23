#include "Log.h"
#include "Paths.h"
#include "version.h"

#include <cstdio>
#include <ctime>
#include <filesystem>
#include <fstream>

namespace
{
    std::string g_path;
}

namespace Log
{
    void init()
    {
        const std::filesystem::path dir = Paths::userDataDir();
        g_path = (dir / "satlight_log.txt").string();
        // Keep the previous run's log as satlight_log.prev.txt before truncating: after a hard
        // lockup the user relaunches to check, and truncating here destroyed the only record of
        // what the crashed run was doing (2026-09-22 10M-satellite freeze). Best-effort — a
        // failure just means no .prev copy, never a failed startup.
        {
            std::error_code ec;
            if (std::filesystem::exists(g_path, ec))
                std::filesystem::rename(g_path, dir / "satlight_log.prev.txt", ec);
        }
        std::ofstream f(g_path, std::ios::trunc);
        if (!f.is_open())
        {
            g_path.clear(); // subsequent line() calls become no-ops rather than retrying every time
            return;
        }
        f << "SAT LIGHT SIM " << APP_VERSION << " (" << APP_GIT_COMMIT << ", built " << APP_BUILD_DATE << ")\n";
        f << "----------------------------------------\n";
    }

    void line(const std::string &msg)
    {
        if (g_path.empty())
            return;
        std::ofstream f(g_path, std::ios::app);
        if (!f.is_open())
            return;
        time_t now = time(nullptr);
        struct tm *utc = gmtime(&now);
        char buf[16] = {};
        if (utc)
            snprintf(buf, sizeof(buf), "[%02d:%02d:%02d] ", utc->tm_hour, utc->tm_min, utc->tm_sec);
        f << buf << msg << '\n';
    }

    const std::string &path()
    {
        return g_path;
    }
}
