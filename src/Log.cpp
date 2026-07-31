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
        g_path = (std::filesystem::path(Paths::userDataDir()) / "satlight_log.txt").string();
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
