// BoltTool: the lightning channel generator (src/simulations/LightningBolt.cpp) without the app.
//   cmake --build build --config Release --target BoltTool
//   build/Release/BoltTool.exe stats [n=200]             branch counts, vertices, straightness, determinism, per kind
//   build/Release/BoltTool.exe dump kind=cg|ic|spider|sprite seed=7 [target=20] [ground=2800,-2800,-4476] [base=-2975]
//                              [top=5500] [tendrils=1.23] [strength=0.8] [fit=<relief m>]    the pieces as JSON (stdout)
// tools/bolt_tool/sheet.py draws dumps as a contact sheet. fit= runs the terrain fit against a synthetic relief
// (ridges of that height) and reports the least clearance left.
#include "LightningBolt.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

static const char *arg(int argc, char **argv, const char *key, const char *def)
{
    const size_t n = std::strlen(key);
    for (int i = 2; i < argc; ++i)
        if (!std::strncmp(argv[i], key, n) && argv[i][n] == '=')
            return argv[i] + n + 1;
    return def;
}

static bolt::Params paramsFor(int kind, uint32_t seed, float tendrils)
{
    bolt::Params p;
    p.seed = seed;
    p.id = 0xF0000000u | seed;
    p.kind = kind;
    p.tendrils = tendrils;
    p.strength = 0.8f;
    if (kind == 1)
    {
        // As the harness command `lightning spawn kind=cg` (base 1500 m, top 10 km, the strike 4 km aside).
        p.ground = glm::vec3(2828.0f, -2830.0f, -4476.0f);
        p.baseZ = -2975.0f;
        p.topZ = 5525.0f;
    }
    else if (kind == 3)
    {
        p.baseZ = 150.0f;
        p.topZ = 8650.0f;
    }
    else if (kind == 0)
    {
        p.baseZ = -4250.0f;
        p.topZ = 4250.0f;
    }
    return p;
}

// A synthetic relief in the tree's frame: ridges `relief` m high over the plain at zGround.
static float reliefAt(glm::vec3 p, float zGround, float relief)
{
    return zGround + relief * (0.5f + 0.5f * std::sin(p.x / 700.0f) * std::cos(p.y / 900.0f + 1.3f))
         + 0.15f * relief * std::sin(p.x / 90.0f + p.y / 130.0f);
}

int main(int argc, char **argv)
{
    const std::string mode = argc > 1 ? argv[1] : "stats";
    const float tendrils = (float)std::atof(arg(argc, argv, "tendrils", "1.23"));
    if (mode == "dump")
    {
        const std::string k = arg(argc, argv, "kind", "cg");
        const int kind = k == "ic" ? 0 : k == "sprite" ? 2 : k == "spider" ? 3 : 1;
        bolt::Params p = paramsFor(kind, (uint32_t)std::atoi(arg(argc, argv, "seed", "7")), tendrils);
        if (const char *g = arg(argc, argv, "ground", nullptr))
            std::sscanf(g, "%f,%f,%f", &p.ground.x, &p.ground.y, &p.ground.z);
        if (const char *b = arg(argc, argv, "base", nullptr))
            p.baseZ = (float)std::atof(b);
        if (const char *b = arg(argc, argv, "top", nullptr))
            p.topZ = (float)std::atof(b);
        p.strength = (float)std::atof(arg(argc, argv, "strength", "0.8"));
        bolt::Tree t;
        bolt::build(p, t);
        float minClear = -1.0f;
        if (const char *f = arg(argc, argv, "fit", nullptr))
        {
            const float relief = (float)std::atof(f);
            const float zG = kind == 1 ? p.ground.z - 0.5f * relief : p.baseZ - 1200.0f;
            std::vector<float> terr(t.verts.size());
            for (size_t i = 0; i < t.verts.size(); ++i)
                terr[i] = reliefAt(t.verts[i].p, zG, relief);
            minClear = bolt::fitTerrain(t, terr);
        }
        std::vector<bolt::Seg> segs;
        bolt::segments(t, (float)std::atof(arg(argc, argv, "target", "20")), segs);
        const bolt::Stats s = bolt::stats(t);
        std::printf("{\"kind\":%d,\"seed\":%u,\"hash\":\"%016llx\",\"verts\":%d,\"paths\":[%d,%d,%d,%d],\"side_branches\":%d,"
                    "\"main_len\":%.1f,\"main_chord\":%.1f,\"max_straight\":%.1f,\"min_clear\":%.2f,\"segs\":[",
                    kind, p.seed, (unsigned long long)t.hash, s.verts, s.paths[0], s.paths[1], s.paths[2], s.paths[3], s.sideBranches,
                    s.mainLen, s.mainChord, s.maxStraight, minClear);
        for (size_t i = 0; i < segs.size(); ++i)
            std::printf("%s[%.1f,%.1f,%.1f,%.1f,%.1f,%.1f,%.3f,%u]", i ? "," : "", segs[i].p0.x, segs[i].p0.y, segs[i].p0.z,
                        segs[i].p1.x, segs[i].p1.y, segs[i].p1.z, segs[i].w, segs[i].order);
        std::printf("]}\n");
        return 0;
    }
    // stats: n seeds per kind.
    const int n = std::atoi(arg(argc, argv, "n", "200"));
    static const char *kNames[4] = {"in_cloud", "cloud_to_ground", "sprite", "spider"};
    int bad = 0;
    for (int kind : {1, 0, 3, 2})
    {
        double verts = 0.0, side = 0.0, straight = 0.0, len = 0.0, s20 = 0.0, s80 = 0.0, s2000 = 0.0, clear = 1e30;
        int vMax = 0, sideMin = 1 << 30, same = 0;
        float straightMax = 0.0f;
        for (int i = 0; i < n; ++i)
        {
            const bolt::Params p = paramsFor(kind, 1000u + (uint32_t)i * 7919u, tendrils);
            bolt::Tree t, t2;
            bolt::build(p, t);
            bolt::build(p, t2);
            same += t.hash == t2.hash ? 1 : 0;
            const bolt::Stats s = bolt::stats(t);
            verts += s.verts;
            vMax = std::max(vMax, s.verts);
            side += s.sideBranches;
            sideMin = std::min(sideMin, s.sideBranches);
            len += s.mainLen;
            straight += s.maxStraight / std::max(s.mainLen, 1.0f);
            straightMax = std::max(straightMax, s.maxStraight / std::max(s.mainLen, 1.0f));
            std::vector<bolt::Seg> segs;
            s20 += (double)bolt::segments(t, 19.0f, segs);     // 6 km at 60 deg / 900 px
            s80 += (double)bolt::segments(t, 80.0f, segs);     // 25 km
            s2000 += (double)bolt::segments(t, 2000.0f, segs); // orbit
            if (kind != 2)
            {
                const float relief = 600.0f;
                const float zG = kind == 1 ? p.ground.z - 0.5f * relief : p.baseZ - 500.0f;
                std::vector<float> terr(t.verts.size());
                for (size_t j = 0; j < t.verts.size(); ++j)
                    terr[j] = reliefAt(t.verts[j].p, zG, relief);
                clear = std::min(clear, (double)bolt::fitTerrain(t, terr));
                // (the fit moves vertices in z only: the relief under each is unchanged)
                for (const bolt::Path &pp : t.paths)
                    for (uint32_t j = 0; j < pp.count; ++j)
                        if (t.verts[pp.first + j].p.z < terr[pp.first + j] - 1e-3f)
                            ++bad;
            }
        }
        std::printf("%-16s n=%d  verts mean %.0f max %d  side branches mean %.1f min %d  main %.0f m  longest straight run "
                    "mean %.2f%% max %.2f%% of the channel  pieces at 19 m %.0f, 80 m %.0f, 2 km %.0f  rebuilt identical %d/%d"
                    "  least clearance after the fit %.2f m\n",
                    kNames[kind], n, verts / n, vMax, side / n, sideMin, len / n, 100.0 * straight / n, 100.0 * straightMax, s20 / n,
                    s80 / n, s2000 / n, same, n, kind == 2 ? 0.0 : clear);
    }
    std::printf("vertices below the relief after the fit: %d\n", bad);
    return bad ? 1 : 0;
}
