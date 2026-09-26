#include "Log.h"
#include "Paths.h"
#include "version.h"

#include <chrono>
#include <cstdio>
#include <ctime>
#include <filesystem>
#include <mutex>
#include <sstream>

#ifdef _WIN32
#include <windows.h>
#endif

namespace
{
    std::string g_path;
    std::mutex g_mutex; // the harness watchdog logs too (Harness.cpp), from its own thread

#ifdef _WIN32
    // The log is the only witness when the machine dies mid-run, so it stays open and every line
    // is pushed through FlushFileBuffers: an unflushed tail commits with its size but reads
    // all-NUL, which destroys the single record of where a run stopped (docs/FREEZES.md,
    // "Machine-level resets"). The call sites are launch / breadcrumb events (~40 of them, never
    // per frame), so one fsync per line is affordable. FILE_SHARE_READ so a driver can tail it.
    HANDLE g_file = INVALID_HANDLE_VALUE;
#endif

    bool writeAll(const char *bytes, size_t n)
    {
#ifdef _WIN32
        if (g_file == INVALID_HANDLE_VALUE)
            return false;
        DWORD done = 0;
        if (!::WriteFile(g_file, bytes, (DWORD)n, &done, nullptr) || done != n)
            return false;
        ::FlushFileBuffers(g_file); // best effort: a failure here must not drop the line
        return true;
#else
        if (g_path.empty())
            return false;
        std::FILE *f = std::fopen(g_path.c_str(), "ab");
        if (!f)
            return false;
        const bool ok = std::fwrite(bytes, 1, n, f) == n;
        std::fflush(f);
        std::fclose(f);
        return ok;
#endif
    }
}

namespace Log
{
    void init()
    {
        const std::filesystem::path dir = Paths::userDataDir();
        const std::filesystem::path file = dir / "satlight_log.txt";
        g_path = file.string();
        // Keep the previous run's log as satlight_log.prev.txt before truncating: after a hard
        // lockup the user relaunches to check, and truncating here destroyed the only record of
        // what the crashed run was doing (2026-09-22 10M-satellite freeze). Best-effort — a
        // failure just means no .prev copy, never a failed startup.
        {
            std::error_code ec;
            if (std::filesystem::exists(file, ec))
                std::filesystem::rename(file, dir / "satlight_log.prev.txt", ec);
        }
        std::ostringstream header;
        header << "SAT LIGHT SIM " << APP_VERSION << " (" << APP_GIT_COMMIT << ", built " << APP_BUILD_DATE
               << ")\n----------------------------------------\n";
        const std::string text = header.str();
#ifdef _WIN32
        g_file = ::CreateFileW(file.wstring().c_str(), FILE_APPEND_DATA, FILE_SHARE_READ | FILE_SHARE_WRITE,
                               nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (g_file == INVALID_HANDLE_VALUE)
        {
            g_path.clear(); // subsequent line() calls become no-ops rather than retrying every time
            return;
        }
        std::lock_guard<std::mutex> lock(g_mutex);
        writeAll(text.data(), text.size());
#else
        std::FILE *f = std::fopen(g_path.c_str(), "wb");
        if (!f)
        {
            g_path.clear();
            return;
        }
        std::fwrite(text.data(), 1, text.size(), f);
        std::fflush(f);
        std::fclose(f);
#endif
    }

    void line(const std::string &msg)
    {
        if (g_path.empty())
            return;
        // Milliseconds, UTC: tools/harness/blackbox.py samples the GPU/CPU every 100 ms in UTC, and a
        // whole-second stamp could not say which side of a launch step a hardware spike fell on.
        const auto sinceEpoch = std::chrono::system_clock::now().time_since_epoch();
        const time_t now = (time_t)std::chrono::duration_cast<std::chrono::seconds>(sinceEpoch).count();
        const int ms = (int)(std::chrono::duration_cast<std::chrono::milliseconds>(sinceEpoch).count() % 1000);
        struct tm *utc = gmtime(&now);
        char buf[24] = {};
        if (utc)
            snprintf(buf, sizeof(buf), "[%02d:%02d:%02d.%03d] ", utc->tm_hour, utc->tm_min, utc->tm_sec, ms);
        const std::string out = std::string(buf) + msg + '\n';
        std::lock_guard<std::mutex> lock(g_mutex);
        writeAll(out.data(), out.size());
    }

    const std::string &path()
    {
        return g_path;
    }
}

