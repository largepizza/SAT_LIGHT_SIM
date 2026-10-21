#pragma once
// Frame profile: named, scoped wall-clock sections of the MAIN thread's frame, kept per frame so that a
// frame that hitches can say where its time went (2026-10-20, the "hangs at a high time rate" pass).
//
//   FPROF_SCOPE("kessler.sync");          // adds this scope's wall time to the section
//   fprof::beginFrame(nowSeconds);        // App::drawFrame, first thing: publishes the previous frame
//
// Sections are INCLUSIVE (a nested section is also inside its parent): read them as a tree by name.
// Calls from any thread other than the one that first called beginFrame are ignored, so a section
// inside code that also runs on a worker is harmless. A frame longer than the threshold (default 50 ms,
// env SATLIGHTSIM_HITCH_MS, 0 = off) is logged to satlight_log.txt with its largest sections and kept in a
// ring of the last 256 hitches; the last 4096 frames' wall times are kept for statistics (harness
// `hitches`). Cost: a steady_clock read per scope and a short linear search of the frame's sections.

#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <deque>
#include <string>
#include <thread>
#include <vector>

namespace fprof
{
    struct Entry
    {
        const char *name;
        float ms;
        int calls;
    };
    struct Frame
    {
        uint64_t index = 0;
        double wallMs = 0.0;
        double simT = 0.0;  // sim time (s since J2000) as tagged by the sim, for the log
        float timeScale = 0; // the sim's warp that frame
        std::vector<Entry> e;
    };
    struct State
    {
        std::thread::id mainId{};
        bool haveMain = false;
        std::vector<Entry> cur;
        double lastBegin = 0.0;
        uint64_t frame = 0;
        float thresholdMs = 50.0f;
        double curSimT = 0.0;
        float curScale = 0.0f;
        std::deque<Frame> hitches; // the last 256 frames over the threshold
        std::deque<float> walls;   // the last 4096 frames' wall times
        uint64_t logged = 0;
        uint64_t warmupFrames = 1; // the very first frame is not reported (its interval includes init)
    };
    inline State &st()
    {
        static State s;
        return s;
    }
    inline bool onMain()
    {
        State &s = st();
        return s.haveMain && std::this_thread::get_id() == s.mainId;
    }
    inline void add(const char *name, float ms)
    {
        if (!onMain())
            return;
        State &s = st();
        for (Entry &e : s.cur)
            if (e.name == name || std::strcmp(e.name, name) == 0)
            {
                e.ms += ms;
                ++e.calls;
                return;
            }
        s.cur.push_back({name, ms, 1});
    }
    inline void tag(double simT, float timeScale)
    {
        st().curSimT = simT;
        st().curScale = timeScale;
    }
    struct Scope
    {
        const char *name;
        std::chrono::steady_clock::time_point t0;
        explicit Scope(const char *n) : name(n), t0(std::chrono::steady_clock::now()) {}
        ~Scope()
        {
            add(name, std::chrono::duration<float, std::milli>(std::chrono::steady_clock::now() - t0).count());
        }
        Scope(const Scope &) = delete;
        Scope &operator=(const Scope &) = delete;
    };
    // Sorted copy of a frame's sections, largest first.
    inline std::vector<Entry> sorted(const std::vector<Entry> &e)
    {
        std::vector<Entry> v = e;
        std::sort(v.begin(), v.end(), [](const Entry &a, const Entry &b) { return a.ms > b.ms; });
        return v;
    }
    // Returns true (and fills `out`) when the frame just finished was a hitch to log.
    inline bool beginFrame(double nowS, Frame *out)
    {
        State &s = st();
        if (!s.haveMain)
        {
            s.mainId = std::this_thread::get_id();
            s.haveMain = true;
            if (const char *ev = std::getenv("SATLIGHTSIM_HITCH_MS"))
                s.thresholdMs = (float)std::atof(ev);
        }
        bool hitch = false;
        if (s.lastBegin > 0.0)
        {
            Frame f;
            f.index = s.frame;
            f.wallMs = (nowS - s.lastBegin) * 1000.0;
            f.simT = s.curSimT;
            f.timeScale = s.curScale;
            s.walls.push_back((float)f.wallMs);
            if (s.walls.size() > 4096)
                s.walls.pop_front();
            if (s.thresholdMs > 0.0f && f.wallMs > s.thresholdMs && s.frame >= s.warmupFrames)
            {
                f.e = sorted(s.cur);
                s.hitches.push_back(f);
                if (s.hitches.size() > 256)
                    s.hitches.pop_front();
                if (out)
                    *out = f;
                hitch = true;
            }
        }
        s.cur.clear();
        s.lastBegin = nowS;
        ++s.frame;
        return hitch;
    }
    // "wall 123.4 ms (frame 812, x3600): kessler.sync 80.1, launch.window 20.3, ..."
    inline std::string describe(const Frame &f, int top = 10)
    {
        char buf[256];
        std::snprintf(buf, sizeof(buf), "wall %.1f ms (frame %llu, warp x%g, sim t %.0f):", f.wallMs,
                      (unsigned long long)f.index, (double)f.timeScale, f.simT);
        std::string out = buf;
        int n = 0;
        for (const Entry &e : f.e)
        {
            if (n++ >= top || e.ms < 0.5f)
                break;
            std::snprintf(buf, sizeof(buf), " %s %.1f%s", e.name, e.ms, n < top ? "," : "");
            out += buf;
        }
        return out;
    }
}

#define FPROF_CAT2(a, b) a##b
#define FPROF_CAT(a, b) FPROF_CAT2(a, b)
#define FPROF_SCOPE(name) ::fprof::Scope FPROF_CAT(fprofScope_, __LINE__)(name)
