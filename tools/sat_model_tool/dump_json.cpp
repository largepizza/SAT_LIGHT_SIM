// --dump-json: everything a visualisation needs to show how a model becomes lobes, as one JSON file.
// Read by tools/video (the technical-video toolkit); nothing in the app or the gate depends on it.
//
// All vectors are in the model's REST body frame (every joint at 0), the frame the triangles are
// tessellated in. Lobe normals are converted from root-triad coordinates with identity poses.
//   model      id, name, groups, components (name, group, material), materials (name, colour, BRDF)
//   triangles  p0 p1 p2, n, area, group, material, component
//   levels     one per budget, finest first: lobes (n, area, diffArea, albedo, f0, alpha, beckmann,
//              group, tris = member triangle indices) — the same bakeSatLobes the app runs
//   validation random (sun, observer) pairs: phase angle, brute-force per-triangle intensity, and the
//              intensity of each level's lobes (same order as `levels`)
//   sphere     intensity seen from every direction of a lat/lon grid with the sun fixed, brute force
//              and each level's lobes (same order as `levels`)
#include "dump_json.h"

#include "SatModel.h"

#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <random>

using nlohmann::json;

namespace
{
json vec(const glm::dvec3 &v)
{
    auto r = [](double x) { return std::round(x * 1e5) / 1e5; };
    return json::array({r(v.x), r(v.y), r(v.z)});
}

double brute(const SatModel &m, const std::vector<SatTri> &tris, glm::dvec3 s, glm::dvec3 o)
{
    double sum = 0.0;
    for (const SatTri &t : tris)
        sum += satMaterialIntensitySun(m.materials[t.material], t.n, t.area, t.spread2, s, o);
    return sum;
}

struct Level
{
    int budget;
    std::vector<GpuSatLobe> lobes;
    std::vector<std::vector<int>> tris;
    std::vector<glm::dvec3> nBody;
};

double lobesAt(const Level &L, glm::dvec3 s, glm::dvec3 o)
{
    double sum = 0.0;
    for (size_t i = 0; i < L.lobes.size(); ++i)
    {
        const GpuSatLobe &b = L.lobes[i];
        sum += satLobeIntensitySun(L.nBody[i], b.area, b.diffArea, b.albedoD, b.f0, b.alpha2Mat, satLobeBeckmann(b), s,
                                   o, b.transmission, satLobeMinnaert(b));
    }
    return sum;
}
} // namespace

bool dumpModelJson(const SatModel &m, const std::string &id, const std::vector<SatTri> &tris,
                   const std::vector<int> &budgets, const std::string &outPath)
{
    json j;
    j["format"] = "sat-light-sim-lobe-dump/1";
    j["model"] = {{"id", id}, {"name", m.name}};
    json groups = json::array();
    for (const AttitudeGroup &g : m.groups)
        groups.push_back({{"name", g.name}, {"parent", g.parent}});
    j["model"]["groups"] = groups;
    json comps = json::array();
    for (const SatComponent &c : m.components)
        comps.push_back({{"name", c.name}, {"group", c.group}, {"material", c.material}, {"render_only", c.renderOnly}});
    j["model"]["components"] = comps;
    json mats = json::array();
    for (const SatMaterial &mt : m.materials)
        mats.push_back({{"name", mt.name},
                        {"color", json::array({mt.color.r, mt.color.g, mt.color.b})},
                        {"albedo", mt.diffuseAlbedo},
                        {"f0", mt.specularF0},
                        {"alpha", mt.roughness},
                        {"beckmann", mt.beckmann},
                        {"minnaert", mt.minnaertK},
                        {"transmission", mt.transmission}});
    j["model"]["materials"] = mats;

    json jt = json::array();
    for (const SatTri &t : tris)
        jt.push_back({{"p", json::array({vec(t.p[0]), vec(t.p[1]), vec(t.p[2])})},
                      {"n", vec(t.n)},
                      {"area", t.area},
                      {"group", t.group},
                      {"material", t.material},
                      {"component", t.component}});
    j["triangles"] = jt;

    // Levels: finest (no budget limit = the exact merge) first, then the requested budgets descending.
    std::vector<int> bs = budgets;
    bs.push_back(1 << 20);
    std::sort(bs.begin(), bs.end(), std::greater<int>());
    bs.erase(std::unique(bs.begin(), bs.end()), bs.end());
    const std::vector<GroupPose> rest(m.groups.size());
    std::vector<Level> levels;
    for (int b : bs)
    {
        Level L;
        L.budget = b;
        SatLobeBakeStats st;
        L.lobes = bakeSatLobes(m, tris, b, st, &L.tris);
        if (!levels.empty() && levels.back().lobes.size() == L.lobes.size())
            continue; // the budget did not bind
        for (size_t i = 0; i < L.lobes.size(); ++i)
            L.nBody.push_back(satLobeNormalPosed(L.lobes, m.groups, rest, (int)i));
        levels.push_back(std::move(L));
    }
    json jl = json::array();
    for (const Level &L : levels)
    {
        json lobes = json::array();
        for (size_t i = 0; i < L.lobes.size(); ++i)
        {
            const GpuSatLobe &b = L.lobes[i];
            lobes.push_back({{"n", vec(L.nBody[i])},
                             {"area", b.area},
                             {"diff_area", b.diffArea},
                             {"albedo", b.albedoD},
                             {"f0", b.f0},
                             {"alpha", std::sqrt(b.alpha2Mat)},
                             {"beckmann", satLobeBeckmann(b)},
                             {"minnaert", satLobeMinnaert(b)},
                             {"transmission", b.transmission},
                             {"group", b.group},
                             {"tris", L.tris[i]}});
        }
        jl.push_back({{"budget", L.budget >= (1 << 20) ? -1 : L.budget}, {"lobes", lobes}});
    }
    j["levels"] = jl;

    std::mt19937 rng(20261021);
    std::normal_distribution<double> nd;
    auto randDir = [&]() { return glm::normalize(glm::dvec3(nd(rng), nd(rng), nd(rng))); };
    json jv = json::array();
    for (int i = 0; i < 4000; ++i)
    {
        const glm::dvec3 s = randDir(), o = randDir();
        json row = {{"phase_deg", std::acos(std::clamp(glm::dot(s, o), -1.0, 1.0)) * 57.29577951308232},
                    {"brute", brute(m, tris, s, o)}};
        json per = json::array();
        for (const Level &L : levels)
            per.push_back(lobesAt(L, s, o));
        row["lobes"] = per;
        jv.push_back(row);
    }
    j["validation"] = jv;

    // Intensity sphere: sun from a fixed oblique direction, observer over a 36 x 72 lat/lon grid.
    const glm::dvec3 sun = glm::normalize(glm::dvec3(0.45, -0.3, 0.84));
    json js = {{"sun", vec(sun)}, {"n_lat", 36}, {"n_lon", 72}};
    json sb = json::array(), sl = json::array();
    for (size_t li = 0; li < levels.size(); ++li)
        sl.push_back(json::array());
    for (int a = 0; a < 36; ++a)
        for (int b = 0; b < 72; ++b)
        {
            const double lat = (-90.0 + (a + 0.5) * 5.0) * 0.017453292519943295;
            const double lon = (b + 0.5) * 5.0 * 0.017453292519943295;
            const glm::dvec3 o(std::cos(lat) * std::cos(lon), std::cos(lat) * std::sin(lon), std::sin(lat));
            sb.push_back(brute(m, tris, sun, o));
            for (size_t li = 0; li < levels.size(); ++li)
                sl[li].push_back(lobesAt(levels[li], sun, o));
        }
    js["brute"] = sb;
    js["lobes"] = sl;
    j["sphere"] = js;

    std::error_code ec;
    if (std::filesystem::path(outPath).has_parent_path())
        std::filesystem::create_directories(std::filesystem::path(outPath).parent_path(), ec);
    std::ofstream f(outPath);
    if (!f)
        return false;
    f << j.dump();
    std::printf("  dump: %s (%zu triangles, %zu levels: %zu -> %zu lobes)\n", outPath.c_str(), tris.size(),
                levels.size(), levels.front().lobes.size(), levels.back().lobes.size());
    return true;
}
