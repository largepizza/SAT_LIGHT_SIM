// SatModelTool — offline bake / validate / OBJ export of satellite geometry models.
//
//   SatModelTool <model.json> [<model.json> ...] [--out <dir>] [--budget <lobes>]
//
// Runs exactly the pipeline the app runs at startup (SatModel.cpp): load → tessellate → bake the
// facet lobes → check them against a brute-force per-triangle evaluation → write the rest and
// sunlit OBJ poses. Prints the lobe table so a model's reflectance can be inspected without
// launching the simulator. Exit code 1 if any model fails to load.
#include "SatModel.h"

#include <cstdio>
#include <cmath>
#include <cstdlib>
#include <filesystem>
#include <string>
#include <vector>

int main(int argc, char **argv)
{
    std::vector<std::string> paths;
    std::string outDir = "satellite_models_debug";
    int budget = 48; // SatelliteSim::kSatLobeBudget (the app uses 256 for types flown by <= 10k satellites)
    int shadowSamples = 0; // --shadow-study N
    for (int i = 1; i < argc; ++i)
    {
        std::string a = argv[i];
        if (a == "--out" && i + 1 < argc)
            outDir = argv[++i];
        else if (a == "--budget" && i + 1 < argc)
            budget = std::atoi(argv[++i]);
        else if (a == "--shadow-study" && i + 1 < argc)
            shadowSamples = std::atoi(argv[++i]);
        else
            paths.push_back(a);
    }
    if (paths.empty())
    {
        std::printf("usage: SatModelTool <model.json> [...] [--out <dir>] [--budget <lobes>] [--shadow-study <N>]\n");
        return 2;
    }

    int failures = 0;
    for (const std::string &path : paths)
    {
        const std::string id = std::filesystem::path(path).stem().string();
        SatModel m;
        std::string err;
        std::vector<std::string> warn;
        bool ok = loadSatModel(path, id, m, err, warn);
        for (const std::string &w : warn)
            std::printf("[%s] warning: %s\n", id.c_str(), w.c_str());
        if (!ok)
        {
            std::printf("[%s] FAILED: %s\n", id.c_str(), err.c_str());
            ++failures;
            continue;
        }

        std::vector<SatTri> tris = tessellateSatModel(m);
        SatLobeBakeStats stats;
        std::vector<GpuSatLobe> lobes = bakeSatLobes(m, tris, budget, stats);
        validateSatLobes(m, tris, lobes, stats);
        bool objOk = writeSatModelObj(m, tris, outDir);

        double totalArea = 0.0;
        for (const SatTri &t : tris)
            totalArea += t.area;
        std::printf("[%s] %s\n", id.c_str(), m.name.c_str());
        std::printf("  %zu groups, %zu components, %zu materials, %d triangles, %.1f m2 surface\n",
                    m.groups.size(), m.components.size(), m.materials.size(), stats.triangles, totalArea);
        for (size_t gi = 0; gi < m.groups.size(); ++gi)
        {
            const AttitudeGroup &g = m.groups[gi];
            if (g.parent < 0)
                std::printf("  group %zu '%s': root, %s  +axis->%s, constraint->%s, joint %s\n", gi, g.name.c_str(),
                            attLawName(g.law), attTargetName(g.primaryTarget), attTargetName(g.secondaryTarget),
                            jointModeName(g.jointMode));
            else
                std::printf("  group %zu '%s': child of '%s', hinge (%.2f %.2f %.2f) axis (%.2f %.2f %.2f), joint %s->%s\n",
                            gi, g.name.c_str(), m.groups[g.parent].name.c_str(), g.hingePos.x, g.hingePos.y, g.hingePos.z,
                            g.jointAxis.x, g.jointAxis.y, g.jointAxis.z, jointModeName(g.jointMode),
                            attTargetName(g.jointTarget));
        }
        std::printf("  lobes: %d exact -> %d after budget %d\n", stats.exactLobes, stats.lobes, budget);
        std::printf("  %-4s %-5s %-26s %8s %8s %6s %6s %8s\n", "#", "group", "normal (root triad)", "area",
                    "diffA", "albedo", "F0", "alpha");
        for (size_t li = 0; li < lobes.size(); ++li)
        {
            const GpuSatLobe &L = lobes[li];
            std::printf("  %-4zu %-5u (%7.3f %7.3f %7.3f) %8.2f %8.2f %6.3f %6.3f %8.4f\n", li, L.group, L.normalT.x,
                        L.normalT.y, L.normalT.z, L.area, L.diffArea, L.albedoD, L.f0, std::sqrt(L.alpha2Mat));
        }
        std::printf("  validator vs brute force: |dmag| p95 %.3f, max %.3f\n", stats.p95ErrMag, stats.maxErrMag);
        if (shadowSamples > 0)
        {
            SatShadowStudy ss = studySatShadowing(m, tris, shadowSamples);
            std::printf("  shadowing (all groups, posed, %d lit configs): dimming median %.3f, p90 %.3f, p99 %.3f, "
                        "max %.2f mag; >0.1 mag in %.1f%%, >0.5 mag in %.1f%%\n",
                        ss.samples, ss.medianDmag, ss.p90Dmag, ss.p99Dmag, ss.maxDmag, 100.0 * ss.fracOver01,
                        100.0 * ss.fracOver05);
            std::printf("    brighter half only: p90 %.3f mag; >0.1 mag in %.1f%%, >0.5 mag in %.1f%%\n",
                        ss.brightP90Dmag, 100.0 * ss.brightFracOver01, 100.0 * ss.brightFracOver05);
        }
        std::printf("  OBJ: %s\n\n", objOk ? (std::filesystem::path(outDir) / (id + "_rest.obj / _sunlit.obj")).string().c_str()
                                           : "export FAILED");
    }
    return failures ? 1 : 0;
}
