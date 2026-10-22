// --pass <spec.json> [--pass-out <file>]: one satellite pass evaluated sample by sample with the CPU photometric
// evaluator (evalSatPhotometry — the double-precision mirror of sat_orbit.comp). The satellite's state comes from
// the spec, not the sim's circular orbits, so a real satellite (a TLE propagated by SGP4) can be replayed against a
// published light curve. Used by tools/video; nothing in the app or the gate depends on it.
//
// spec (JSON):
//   model          path of a satellite model JSON
//   budget         lobe budget (default 256: what the app uses for a type flown by few satellites)
//   occlusion      part occlusion (default true)
//   flare_tilt_deg the global flare-mitigation tilt (default 0)
//   extinction_k   sea-level zenith extinction for the apparent magnitude (default 0.18, the app's default), from the
//                  observer's geodetic height (WGS84) above the extinction's sphere
//   samples        [{t_j2000, pos_eci_m[3], vel_eci_mps[3], obs_eci_m[3]}]  — the sim's ECI (GMST-rotated, = TEME)
// output (JSON, "sat-light-sim-pass/1"): per sample the magnitude above the atmosphere and apparent, range,
// elevation, phase, Earth-shadow factor, sun / earthshine split, the dominant lobe, each lobe's posed normal and
// sunlit intensity (no occlusion), the root group's rotation (rest body -> ECI) and the Sun / observer directions
// in the rest body frame (for plotting the observer on a model's brightness map).
#include "pass_eval.h"

#include "SatModel.h"
#include "SatPhotometry.h"

#include <nlohmann/json.hpp>

#include <cmath>
#include <cstdio>
#include <fstream>
#include <limits>

using nlohmann::json;

namespace
{
glm::dvec3 v3(const json &a) { return {a.at(0).get<double>(), a.at(1).get<double>(), a.at(2).get<double>()}; }
json j3(const glm::dvec3 &v) { return json::array({v.x, v.y, v.z}); }
json fin(double x) { return std::isfinite(x) ? json(x) : json(nullptr); }

// Height above the WGS84 ellipsoid of an ECI (or ECEF) position: a turn about z changes neither the distance from
// the axis nor z, so the frame does not matter. Bowring's start + three fixed-point steps (sub-mm in LEO / on the
// ground).
double geodeticHeightM(const glm::dvec3 &p)
{
    const double a = 6378137.0, f = 1.0 / 298.257223563, e2 = f * (2.0 - f);
    const double rho = std::hypot(p.x, p.y);
    double lat = std::atan2(p.z, rho * (1.0 - e2)), h = 0.0;
    for (int i = 0; i < 4; ++i)
    {
        const double sl = std::sin(lat), N = a / std::sqrt(1.0 - e2 * sl * sl);
        h = std::abs(std::cos(lat)) > 1e-9 ? rho / std::cos(lat) - N : std::abs(p.z) - N * (1.0 - e2);
        lat = std::atan2(p.z, rho * (1.0 - e2 * N / (N + h)));
    }
    return h;
}
} // namespace

bool runPassCommand(const std::string &specPath, const std::string &outPath)
{
    std::ifstream f(specPath);
    if (!f)
    {
        std::printf("[pass] cannot read %s\n", specPath.c_str());
        return false;
    }
    json spec;
    try
    {
        spec = json::parse(f);
    }
    catch (const std::exception &e)
    {
        std::printf("[pass] %s: %s\n", specPath.c_str(), e.what());
        return false;
    }
    const std::string modelPath = spec.at("model").get<std::string>();
    const int budget = spec.value("budget", 256);
    const bool occOn = spec.value("occlusion", true);
    const double tilt = spec.value("flare_tilt_deg", 0.0) * 3.14159265358979323846 / 180.0;
    const double k = spec.value("extinction_k", 0.18);

    SatModel m;
    std::string err;
    std::vector<std::string> warn;
    const std::string id = std::filesystem::path(modelPath).stem().string();
    if (!loadSatModel(modelPath, id, m, err, warn))
    {
        std::printf("[pass] %s: %s\n", modelPath.c_str(), err.c_str());
        return false;
    }
    std::vector<SatTri> tris = tessellateSatModel(m);
    SatLobeBakeStats st;
    std::vector<std::vector<int>> lobeTris;
    std::vector<GpuSatLobe> lobes = bakeSatLobes(m, tris, budget, st, &lobeTris);
    SatOcclusion occ = buildSatOcclusion(m, tris, lobes, lobeTris);
    const int root = attRootOf(m.groups, 0);

    json out;
    out["format"] = "sat-light-sim-pass/1";
    out["model"] = {{"id", id}, {"path", modelPath}, {"lobes", lobes.size()}, {"exact_lobes", st.exactLobes},
                    {"budget", budget}, {"occlusion", occOn}};
    out["extinction_k"] = k;
    json rows = json::array();
    for (const json &s : spec.at("samples"))
    {
        const double t = s.at("t_j2000").get<double>();
        const glm::dvec3 pos = v3(s.at("pos_eci_m")), vel = v3(s.at("vel_eci_mps")), obs = v3(s.at("obs_eci_m"));
        SatOrbitElems e;
        e.fixedState = true;
        e.fixPos = pos;
        e.fixVel = vel;
        e.fixT0 = t;
        e.rSatM = glm::length(pos);
        SatPhotInputs in;
        in.sunDirEci = sunDirEciAt(t);
        in.obsEci = obs;
        in.flareTiltRad = tilt;
        const SatPhotResult r = evalSatPhotometry(m.groups, lobes, e, t, in, nullptr, occOn ? &occ : nullptr);

        // The same poses the evaluator used, for the body-frame directions and the per-lobe terms.
        AttGeometry geo;
        geo.nadir = r.orbit.nadir;
        geo.velocity = r.orbit.velocity;
        geo.sun = glm::normalize(in.sunDirEci);
        geo.flareTiltRad = tilt;
        std::vector<GroupPose> poses = evalGroupPoses(m.groups, geo, false);
        const glm::dmat3 R = poses[root].R;
        const glm::dvec3 o = glm::normalize(obs - pos);
        const glm::dvec3 sunB = glm::transpose(R) * geo.sun, obsB = glm::transpose(R) * o;
        json perLobe = json::array(), normals = json::array();
        for (size_t li = 0; li < lobes.size(); ++li)
        {
            const GpuSatLobe &L = lobes[li];
            const glm::dvec3 n = satLobeNormalPosed(lobes, m.groups, poses, (int)li);
            normals.push_back(j3(glm::transpose(R) * n)); // in the rest body frame of the root
            perLobe.push_back(satLobeIntensitySun(n, L.area, L.diffArea, L.albedoD, L.f0, L.alpha2Mat,
                                                  satLobeBeckmann(L), geo.sun, o, L.transmission, satLobeMinnaert(L)));
        }
        // The extinction's atmosphere is a sphere of satphot::kEarthRadiusM (atmosphere.glsl), but the observer stands
        // on WGS84: |obs| - R is the ellipsoid's radius there less the sphere's (-7 km at 54 N, +7 at the equator) plus
        // the site's height, i.e. tens of magnitudes of air at a high latitude (2026-10-21; it was evaluated at obs).
        // Evaluate it from the observer's real geodetic height above the sphere.
        const glm::dvec3 rel = pos - obs;
        const double obsH = geodeticHeightM(obs);
        const glm::dvec3 obsAtm = glm::normalize(obs) * (satphot::kEarthRadiusM + std::max(0.0, obsH));
        const double ext = std::isfinite(r.magnitude) && k > 0.0 ? atmExtinctionMag(obsAtm, glm::normalize(rel),
                                                                                             r.rangeM, k) : 0.0;
        json R9 = json::array();
        for (int c = 0; c < 3; ++c)
            R9.push_back(j3(glm::dvec3(R[c][0], R[c][1], R[c][2]))); // columns: body axes in ECI
        rows.push_back({{"t_j2000", t},
                        {"mag", fin(r.magnitude)},
                        {"mag_apparent", fin(r.magnitude + ext)},
                        {"extinction_mag", ext},
                        {"obs_height_m", obsH},
                        {"m1000", fin(r.magnitude1000)},
                        {"range_m", r.rangeM},
                        {"elevation_deg", std::asin(std::clamp(r.sinElevation, -1.0, 1.0)) * 57.29577951308232},
                        {"phase_deg", r.phaseAngleRad * 57.29577951308232},
                        {"lit", r.litFactor},
                        {"intensity_sun", r.intensitySun},
                        {"intensity_earth", r.intensityEarth},
                        {"dominant_lobe", r.dominantLobe},
                        {"sun_body", j3(sunB)},
                        {"obs_body", j3(obsB)},
                        {"root_axes_eci", R9},
                        {"lobe_normals_body", normals},
                        {"lobe_intensity_sun", perLobe}});
    }
    out["samples"] = rows;
    const std::string dst = outPath.empty() ? std::filesystem::path(specPath).replace_extension(".out.json").string()
                                            : outPath;
    std::ofstream o(dst);
    if (!o)
    {
        std::printf("[pass] cannot write %s\n", dst.c_str());
        return false;
    }
    o << out.dump();
    std::printf("[pass] %s: %zu samples, %zu lobes -> %s\n", id.c_str(), rows.size(), lobes.size(), dst.c_str());
    return true;
}
