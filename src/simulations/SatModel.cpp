// SatModel — see SatModel.h for the overview.
#include "SatModel.h"

#include <glm/gtc/matrix_transform.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <cmath>
#include <filesystem>
#include <fstream>
#include <map>
#include <random>

namespace
{
constexpr double kPi = 3.14159265358979323846;

const char *const kAttTargetNames[] = {
    "nadir", "zenith", "sun", "anti_sun", "velocity", "anti_velocity",
    "orbit_normal", "anti_orbit_normal", "sun_reflect_nadir", "sun_reflect_ground_site"};
const char *const kJointModeNames[] = {"none", "track", "edge_on", "fixed", "flare_mitigation_tilt"};
const char *const kAttLawNames[] = {"two_vector", "tumble"};
static_assert(sizeof(kAttTargetNames) / sizeof(kAttTargetNames[0]) == (size_t)AttTarget::Count);
static_assert(sizeof(kJointModeNames) / sizeof(kJointModeNames[0]) == (size_t)JointMode::Count);
static_assert(sizeof(kAttLawNames) / sizeof(kAttLawNames[0]) == (size_t)AttLaw::Count);

template <size_t N>
int lookupName(const char *const (&names)[N], const std::string &s)
{
    for (size_t i = 0; i < N; ++i)
        if (s == names[i])
            return (int)i;
    return -1;
}

glm::vec3 jsonVec3(const nlohmann::json &j, const char *key, glm::vec3 def)
{
    if (!j.contains(key) || !j[key].is_array() || j[key].size() != 3)
        return def;
    return {j[key][0].get<float>(), j[key][1].get<float>(), j[key][2].get<float>()};
}

nlohmann::json vecJson(glm::vec3 v) { return nlohmann::json::array({v.x, v.y, v.z}); }

AttTarget jsonTarget(const nlohmann::json &j, const char *key, AttTarget def, const std::string &where,
                     std::vector<std::string> &warn)
{
    if (!j.contains(key))
        return def;
    AttTarget t;
    if (!parseAttTargetName(j[key].get<std::string>(), t))
    {
        warn.push_back(where + ": unknown target '" + j[key].get<std::string>() + "'");
        return def;
    }
    return t;
}

// Rotation matrix about unit axis k by theta (Rodrigues), double precision.
glm::dmat3 axisAngle(glm::dvec3 k, double theta)
{
    return glm::dmat3(glm::rotate(glm::dmat4(1.0), theta, k));
}

// Body-frame triad of a root group (columns t1b, t2b, t3b); identity for the tumble law.
glm::dmat3 bodyTriad(const AttitudeGroup &root)
{
    if (root.law == AttLaw::Tumble)
        return glm::dmat3(1.0);
    glm::dvec3 t1 = glm::normalize(glm::dvec3(root.primaryAxis));
    glm::dvec3 t2 = glm::normalize(glm::cross(t1, glm::dvec3(root.secondaryAxis)));
    glm::dvec3 t3 = glm::cross(t1, t2);
    return glm::dmat3(t1, t2, t3);
}
} // namespace

// ── Names ─────────────────────────────────────────────────────────────────────────────────────
const char *attTargetName(AttTarget t) { return kAttTargetNames[(int)t]; }
const char *attLawName(AttLaw l) { return kAttLawNames[(int)l]; }
const char *jointModeName(JointMode m) { return kJointModeNames[(int)m]; }
bool parseAttTargetName(const std::string &s, AttTarget &out)
{
    int i = lookupName(kAttTargetNames, s);
    if (i >= 0)
        out = (AttTarget)i;
    return i >= 0;
}
bool parseAttLawName(const std::string &s, AttLaw &out)
{
    int i = lookupName(kAttLawNames, s);
    if (i >= 0)
        out = (AttLaw)i;
    return i >= 0;
}
bool parseJointModeName(const std::string &s, JointMode &out)
{
    int i = lookupName(kJointModeNames, s);
    if (i >= 0)
        out = (JointMode)i;
    return i >= 0;
}

// ── Attitude group JSON ───────────────────────────────────────────────────────────────────────
AttitudeGroup parseAttitudeGroupJson(const nlohmann::json &jg, size_t idx, std::string &parentName,
                                     std::vector<std::string> &warn)
{
    AttitudeGroup g;
    g.name = jg.value("name", "group" + std::to_string(idx));
    const std::string where = "group '" + g.name + "'";
    parentName = jg.value("parent", std::string());

    const std::string law = jg.value("law", std::string("two_vector"));
    if (!parseAttLawName(law, g.law))
        warn.push_back(where + ": unknown law '" + law + "', using two_vector");
    if (jg.contains("primary"))
    {
        g.primaryAxis = jsonVec3(jg["primary"], "axis", g.primaryAxis);
        g.primaryTarget = jsonTarget(jg["primary"], "target", g.primaryTarget, where, warn);
    }
    if (jg.contains("secondary"))
    {
        g.secondaryAxis = jsonVec3(jg["secondary"], "axis", g.secondaryAxis);
        g.secondaryTarget = jsonTarget(jg["secondary"], "target", g.secondaryTarget, where, warn);
    }
    if (jg.contains("joint"))
    {
        const nlohmann::json &jj = jg["joint"];
        const std::string mode = jj.value("mode", std::string("none"));
        if (!parseJointModeName(mode, g.jointMode))
            warn.push_back(where + ": unknown joint mode '" + mode + "'");
        g.jointAxis = jsonVec3(jj, "axis", g.jointAxis);
        g.jointVector = jsonVec3(jj, "vector", g.jointVector);
        g.jointTarget = jsonTarget(jj, "target", g.jointTarget, where, warn);
        g.jointLimitDeg = jj.value("limit_deg", g.jointLimitDeg);
        g.jointAngleDeg = jj.value("angle_deg", g.jointAngleDeg);
    }
    // A child's hinge axis IS its joint axis — the hinge block wins over joint.axis.
    if (jg.contains("hinge"))
    {
        g.hingePos = jsonVec3(jg["hinge"], "position", g.hingePos);
        if (jg["hinge"].contains("axis"))
            g.jointAxis = jsonVec3(jg["hinge"], "axis", g.jointAxis);
    }
    return g;
}

bool resolveGroupParents(std::vector<AttitudeGroup> &groups, const std::vector<std::string> &parentNames,
                         std::string &problem)
{
    for (size_t i = 0; i < groups.size(); ++i)
    {
        groups[i].parent = -1;
        if (i >= parentNames.size() || parentNames[i].empty())
            continue;
        for (size_t p = 0; p < i; ++p)
            if (groups[p].name == parentNames[i])
                groups[i].parent = (int)p;
        if (groups[i].parent < 0)
        {
            problem = "group '" + groups[i].name + "': parent '" + parentNames[i] +
                      "' must name an EARLIER group";
            return false;
        }
    }
    return true;
}

std::string validateAttitudeGroups(const std::vector<AttitudeGroup> &groups)
{
    if (groups.empty() || (int)groups.size() > kMaxAttitudeGroups)
        return std::to_string(groups.size()) + " attitude groups (1-" + std::to_string(kMaxAttitudeGroups) +
               " supported)";
    for (size_t i = 0; i < groups.size(); ++i)
    {
        const AttitudeGroup &g = groups[i];
        if (g.parent >= (int)i)
            return "group '" + g.name + "' has a parent that is not an earlier group";
        if (g.parent < 0 && g.law == AttLaw::TwoVector &&
            glm::length(glm::cross(glm::normalize(g.primaryAxis), glm::normalize(g.secondaryAxis))) < 1e-3f)
            return "group '" + g.name + "' has parallel primary/secondary axes";
        if (g.jointMode != JointMode::None && glm::length(g.jointAxis) < 1e-6f)
            return "group '" + g.name + "' has a zero joint axis";
        if ((g.jointMode == JointMode::Track || g.jointMode == JointMode::EdgeOn) &&
            glm::length(glm::cross(glm::normalize(g.jointAxis), glm::normalize(g.jointVector))) < 1e-3f)
            return "group '" + g.name + "' has its joint vector parallel to the joint axis";
    }
    return {};
}

nlohmann::json attitudeGroupToJson(const AttitudeGroup &g, const std::vector<AttitudeGroup> &all)
{
    nlohmann::json jg = {{"name", g.name}};
    if (g.parent >= 0)
    {
        jg["parent"] = all[g.parent].name;
        jg["hinge"] = {{"position", vecJson(g.hingePos)}, {"axis", vecJson(g.jointAxis)}};
    }
    else
    {
        jg["law"] = attLawName(g.law);
        if (g.law == AttLaw::TwoVector)
        {
            jg["primary"] = {{"axis", vecJson(g.primaryAxis)}, {"target", attTargetName(g.primaryTarget)}};
            jg["secondary"] = {{"axis", vecJson(g.secondaryAxis)}, {"target", attTargetName(g.secondaryTarget)}};
        }
    }
    if (g.jointMode != JointMode::None)
    {
        nlohmann::json jj = {{"mode", jointModeName(g.jointMode)}};
        if (g.parent < 0)
            jj["axis"] = vecJson(g.jointAxis);
        if (g.jointMode == JointMode::Track || g.jointMode == JointMode::EdgeOn)
        {
            jj["vector"] = vecJson(g.jointVector);
            jj["target"] = attTargetName(g.jointTarget);
            jj["limit_deg"] = g.jointLimitDeg;
        }
        if (g.jointMode == JointMode::FixedAngle)
            jj["angle_deg"] = g.jointAngleDeg;
        jg["joint"] = jj;
    }
    return jg;
}

// ── Triad coordinates / GPU form ──────────────────────────────────────────────────────────────
int attRootOf(const std::vector<AttitudeGroup> &groups, int gi)
{
    int r = gi;
    for (int guard = 0; guard < kMaxAttitudeGroups + 1 && r >= 0 && groups[r].parent >= 0; ++guard)
        r = groups[r].parent;
    return r;
}

glm::vec3 attTriadCoords(const std::vector<AttitudeGroup> &groups, int gi, glm::vec3 v)
{
    // Coordinates in the ROOT's body triad: c = Bᵀ v (B's columns are orthonormal).
    return glm::vec3(glm::transpose(bodyTriad(groups[attRootOf(groups, gi)])) * glm::dvec3(v));
}

GpuAttGroup toGpuAttGroup(const std::vector<AttitudeGroup> &groups, int gi)
{
    const AttitudeGroup &g = groups[gi];
    GpuAttGroup o{};
    o.law = (uint32_t)g.law;
    o.primaryTarget = (uint32_t)g.primaryTarget;
    o.secondaryTarget = (uint32_t)g.secondaryTarget;
    o.jointMode = (uint32_t)g.jointMode;
    glm::vec3 axis = glm::normalize(g.jointAxis);
    // Track/EdgeOn's closed form assumes the objective vector is perpendicular to the joint axis
    // (true for every legacy mapping) — project it so a slightly-off authored vector still works.
    glm::vec3 vec = g.jointVector - glm::dot(g.jointVector, axis) * axis;
    vec = glm::length(vec) > 1e-6f ? glm::normalize(vec) : glm::vec3(0.0f, 0.0f, 1.0f);
    o.jointAxisT = attTriadCoords(groups, gi, axis);
    o.jointTarget = (uint32_t)g.jointTarget;
    o.jointVectorT = attTriadCoords(groups, gi, vec);
    o.jointLimitRad = glm::radians(g.jointLimitDeg);
    o.jointAngleRad = glm::radians(g.jointAngleDeg);
    o.parent = g.parent >= 0 ? (uint32_t)g.parent : 0xFFFFFFFFu;
    return o;
}

// ── CPU attitude evaluation (mirror of sat_orbit.comp) ────────────────────────────────────────
static glm::dvec3 cpuTarget(AttTarget t, const AttGeometry &geo)
{
    switch (t)
    {
    case AttTarget::Nadir: return geo.nadir;
    case AttTarget::Zenith: return -geo.nadir;
    case AttTarget::Sun: return geo.sun;
    case AttTarget::AntiSun: return -geo.sun;
    case AttTarget::Velocity: return geo.velocity;
    case AttTarget::AntiVelocity: return -geo.velocity;
    case AttTarget::OrbitNormal: return glm::normalize(glm::cross(-geo.nadir, geo.velocity));
    case AttTarget::AntiOrbitNormal: return -glm::normalize(glm::cross(-geo.nadir, geo.velocity));
    case AttTarget::SunReflectNadir:
    {
        glm::dvec3 n = geo.sun + geo.nadir;
        double len = glm::length(n);
        return len > 1e-5 ? n / len : geo.nadir;
    }
    case AttTarget::SunReflectGroundSite: return geo.siteIdeal;
    default: return geo.nadir;
    }
}

std::vector<GroupPose> evalGroupPoses(const std::vector<AttitudeGroup> &groups, const AttGeometry &geo,
                                      bool jointsAtZero)
{
    std::vector<GroupPose> poses(groups.size());
    std::vector<glm::dvec3> origin(groups.size(), glm::dvec3(0.0)); // hinge point, rest coordinates
    for (size_t gi = 0; gi < groups.size(); ++gi)
    {
        const AttitudeGroup &g = groups[gi];
        GroupPose pre;
        if (g.parent < 0)
        {
            glm::dmat3 W;
            if (g.law == AttLaw::Tumble)
            {
                glm::dvec3 ax = glm::normalize(geo.tumbleAxis);
                glm::dvec3 ref = std::abs(ax.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0);
                glm::dvec3 axA = glm::normalize(glm::cross(ax, ref));
                glm::dvec3 axB = glm::cross(ax, axA);
                glm::dvec3 t1 = std::cos(geo.tumbleAngle) * axA + std::sin(geo.tumbleAngle) * axB;
                W = glm::dmat3(t1, glm::cross(ax, t1), ax);
            }
            else
            {
                glm::dvec3 t1 = cpuTarget(g.primaryTarget, geo);
                glm::dvec3 c = glm::cross(t1, cpuTarget(g.secondaryTarget, geo));
                if (glm::length(c) < 1e-5)
                    c = glm::cross(t1, std::abs(t1.z) < 0.9 ? glm::dvec3(0, 0, 1) : glm::dvec3(1, 0, 0));
                glm::dvec3 t2 = glm::normalize(c);
                W = glm::dmat3(t1, t2, glm::cross(t1, t2));
            }
            pre.R = W * glm::transpose(bodyTriad(g)); // body → world
        }
        else
        {
            pre = poses[g.parent];
            origin[gi] = origin[g.parent] + glm::dvec3(g.hingePos);
        }

        GroupPose pose = pre;
        if (!jointsAtZero && g.jointMode != JointMode::None)
        {
            glm::dvec3 k = glm::normalize(pre.R * glm::dvec3(g.jointAxis));
            double theta = 0.0;
            if (g.jointMode == JointMode::FixedAngle)
                theta = glm::radians((double)g.jointAngleDeg);
            else if (g.jointMode == JointMode::FlareMitigationTilt)
                theta = geo.flareTiltRad;
            else
            {
                glm::dvec3 axisB = glm::normalize(glm::dvec3(g.jointAxis));
                glm::dvec3 vB = glm::dvec3(g.jointVector) - glm::dot(glm::dvec3(g.jointVector), axisB) * axisB;
                glm::dvec3 jv = pre.R * glm::normalize(vB);
                glm::dvec3 tgt = cpuTarget(g.jointTarget, geo);
                double a = glm::dot(tgt, jv), b = glm::dot(tgt, glm::cross(k, jv));
                if (g.jointMode == JointMode::Track)
                    theta = std::atan2(b, a);
                else
                {
                    double th1 = std::atan2(-a, b);
                    double th2 = th1 + (th1 < 0.0 ? kPi : -kPi);
                    theta = std::abs(th1) <= std::abs(th2) ? th1 : th2;
                }
                double lim = glm::radians((double)g.jointLimitDeg);
                theta = std::clamp(theta, -lim, lim);
            }
            glm::dmat3 Rj = axisAngle(k, theta);
            glm::dvec3 pivot = pre.R * origin[gi] + pre.t; // hinge point (root: body origin)
            pose.R = Rj * pre.R;
            pose.t = Rj * (pre.t - pivot) + pivot;
        }
        poses[gi] = pose;
    }
    return poses;
}

// ── Materials ─────────────────────────────────────────────────────────────────────────────────
const std::vector<SatMaterial> &satMaterialPresets()
{
    // INITIAL ESTIMATES — calibrated against reference satellites in Phase 3c. Roughness is GGX α.
    static const std::vector<SatMaterial> presets = {
        // Solar cells under cover glass: dark cells, dielectric glass specular, panel-scale waviness.
        {"solar_cell", 0.06f, 0.04f, 0.05f, {0.20f, 0.25f, 0.55f}},
        // Back of a solar array (white/Kapton substrate).
        {"solar_array_back", 0.50f, 0.04f, 0.30f, {0.85f, 0.82f, 0.70f}},
        // Multi-layer insulation: crinkled metallised film — high F0, very rough.
        {"mli_foil", 0.20f, 0.60f, 0.35f, {0.95f, 0.78f, 0.40f}},
        {"white_paint", 0.80f, 0.04f, 0.50f, {0.95f, 0.95f, 0.95f}},
        {"black_paint", 0.05f, 0.04f, 0.40f, {0.08f, 0.08f, 0.08f}},
        {"aluminum", 0.10f, 0.90f, 0.20f, {0.80f, 0.80f, 0.82f}},
        // Phased-array antenna face (Starlink-style dielectric panel).
        {"antenna_panel", 0.05f, 0.04f, 0.10f, {0.25f, 0.25f, 0.28f}},
        // Optical solar reflector: silvered quartz tiles — nearly a mirror.
        {"osr_radiator", 0.05f, 0.90f, 0.01f, {0.85f, 0.88f, 0.92f}},
        // Large flat mirror (Reflect Orbital-class): near-perfect specular.
        {"mirror", 0.00f, 0.95f, 0.0005f, {0.90f, 0.92f, 0.95f}},
    };
    return presets;
}

// ── Model loader ──────────────────────────────────────────────────────────────────────────────
bool loadSatModel(const std::string &path, const std::string &id, SatModel &out, std::string &err,
                  std::vector<std::string> &warn)
{
    std::ifstream f(path);
    if (!f.is_open())
    {
        err = "cannot open " + path;
        return false;
    }
    nlohmann::json j;
    try
    {
        f >> j;
    }
    catch (const nlohmann::json::exception &e)
    {
        err = std::string("parse error: ") + e.what();
        return false;
    }

    out = SatModel{};
    out.id = id;
    out.name = j.value("name", id);

    try
    {
        // Materials: presets, then model-local definitions (which may extend a preset).
        std::map<std::string, SatMaterial> matLib;
        for (const SatMaterial &p : satMaterialPresets())
            matLib[p.name] = p;
        for (const auto &jm : j.value("materials", nlohmann::json::array()))
        {
            SatMaterial m;
            const std::string base = jm.value("preset", std::string());
            if (!base.empty())
            {
                if (matLib.count(base))
                    m = matLib[base];
                else
                    warn.push_back("material preset '" + base + "' not found");
            }
            m.name = jm.at("name").get<std::string>();
            m.diffuseAlbedo = jm.value("diffuse_albedo", m.diffuseAlbedo);
            m.specularF0 = jm.value("specular_f0", m.specularF0);
            m.roughness = jm.value("roughness", m.roughness);
            m.color = jsonVec3(jm, "color", m.color);
            matLib[m.name] = m;
            out.localMaterials.push_back(m.name);
        }
        // Provenance (benchmarking M4) — informational; SatModelTool reports coverage.
        // An entry names one `subject` or several `subjects` sharing the same provenance.
        for (const auto &js : j.value("sources", nlohmann::json::array()))
        {
            SatModelSource s;
            s.status = js.value("status", std::string());
            s.value = js.value("value", std::string());
            s.source = js.value("source", std::string());
            std::vector<std::string> subjects;
            if (js.contains("subjects"))
                subjects = js["subjects"].get<std::vector<std::string>>();
            else
                subjects.push_back(js.value("subject", std::string()));
            if (s.status != "sourced" && s.status != "derived" && s.status != "estimate")
                warn.push_back("source for '" + subjects.front() + "': status must be sourced/derived/estimate");
            for (const std::string &sub : subjects)
            {
                s.subject = sub;
                out.sources.push_back(s);
            }
        }
        std::map<std::string, int> matIndex;
        auto materialRef = [&](const std::string &name) -> int {
            auto it = matIndex.find(name);
            if (it != matIndex.end())
                return it->second;
            if (!matLib.count(name))
                throw std::runtime_error("unknown material '" + name + "'");
            out.materials.push_back(matLib[name]);
            return matIndex[name] = (int)out.materials.size() - 1;
        };

        // Attitude groups (kinematic tree).
        std::vector<std::string> parentNames;
        for (const auto &jg : j.at("attitude_groups"))
        {
            std::string pn;
            out.groups.push_back(parseAttitudeGroupJson(jg, out.groups.size(), pn, warn));
            parentNames.push_back(pn);
        }
        if (!resolveGroupParents(out.groups, parentNames, err))
            return false;
        err = validateAttitudeGroups(out.groups);
        if (!err.empty())
            return false;
        auto groupRef = [&](const nlohmann::json &jc) -> int {
            if (!jc.contains("group"))
                return 0;
            if (jc["group"].is_number_integer())
            {
                int gi = jc["group"].get<int>();
                if (gi < 0 || gi >= (int)out.groups.size())
                    throw std::runtime_error("group index " + std::to_string(gi) + " out of range");
                return gi;
            }
            const std::string name = jc["group"].get<std::string>();
            for (size_t gi = 0; gi < out.groups.size(); ++gi)
                if (out.groups[gi].name == name)
                    return (int)gi;
            throw std::runtime_error("unknown group '" + name + "'");
        };

        // Components.
        for (const auto &jc : j.at("components"))
        {
            SatComponent c;
            c.name = jc.value("name", "component" + std::to_string(out.components.size()));
            const std::string type = jc.at("type").get<std::string>();
            if (type == "plane")
            {
                c.prim = PrimitiveKind::Plane;
                const auto &s = jc.at("size");
                c.size = {s.at(0).get<float>(), s.at(1).get<float>(), 0.0f};
                c.singleSided = jc.value("single_sided", false);
            }
            else if (type == "box")
            {
                c.prim = PrimitiveKind::Box;
                c.size = jsonVec3(jc, "size", c.size);
            }
            else if (type == "cylinder")
            {
                c.prim = PrimitiveKind::Cylinder;
                c.radius = jc.at("radius").get<float>();
                c.height = jc.at("height").get<float>();
                c.caps = jc.value("caps", true);
            }
            else if (type == "cone")
            {
                c.prim = PrimitiveKind::Cone;
                c.radius = jc.at("radius_bottom").get<float>();
                c.radiusTop = jc.value("radius_top", 0.0f);
                c.height = jc.at("height").get<float>();
                c.caps = jc.value("caps", true);
            }
            else if (type == "sphere")
            {
                c.prim = PrimitiveKind::Sphere;
                c.radius = jc.at("radius").get<float>();
            }
            else
                throw std::runtime_error("component '" + c.name + "': unknown type '" + type + "'");

            c.position = jsonVec3(jc, "position", c.position);
            if (jc.contains("rotation_quat"))
            {
                const auto &q = jc["rotation_quat"];
                c.rotation = glm::normalize(glm::quat(q.at(0).get<float>(), q.at(1).get<float>(),
                                                      q.at(2).get<float>(), q.at(3).get<float>()));
            }
            else if (jc.contains("rotation_deg"))
            {
                // Intrinsic X, then Y, then Z: R = Rx · Ry · Rz.
                glm::vec3 e = glm::radians(jsonVec3(jc, "rotation_deg", glm::vec3(0.0f)));
                c.rotation = glm::angleAxis(e.x, glm::vec3(1, 0, 0)) * glm::angleAxis(e.y, glm::vec3(0, 1, 0)) *
                             glm::angleAxis(e.z, glm::vec3(0, 0, 1));
            }
            c.material = materialRef(jc.at("material").get<std::string>());
            c.backMaterial = jc.contains("back_material") ? materialRef(jc["back_material"].get<std::string>()) : -1;
            c.group = groupRef(jc);
            out.components.push_back(std::move(c));
        }
    }
    catch (const std::exception &e)
    {
        err = e.what();
        return false;
    }
    if (out.components.empty())
    {
        err = "model has no components";
        return false;
    }
    return true;
}

std::vector<std::string> unexplainedModelParts(const SatModel &m)
{
    auto covered = [&](const std::string &name) {
        for (const SatModelSource &s : m.sources)
            if (s.subject == name)
                return true;
        return false;
    };
    std::vector<std::string> missing;
    for (const AttitudeGroup &g : m.groups)
        if (!covered(g.name))
            missing.push_back("group " + g.name);
    for (const SatComponent &c : m.components)
        if (!covered(c.name))
            missing.push_back("component " + c.name);
    for (const std::string &mat : m.localMaterials)
        if (!covered(mat))
            missing.push_back("material " + mat);
    return missing;
}

// ── Tessellation ──────────────────────────────────────────────────────────────────────────────
namespace
{
struct TriSink
{
    std::vector<SatTri> &out;
    glm::dmat3 R;
    glm::dvec3 offset;
    int group, component;
    double spread2 = 0.0; // applied to every triangle added until changed

    // Adds a triangle given in component-local coordinates, oriented so its normal agrees with
    // `outward` (also local). Degenerate triangles (cone apex quads) are dropped.
    void tri(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 outward, int material)
    {
        glm::dvec3 n = glm::cross(b - a, c - a);
        double len = glm::length(n);
        if (len < 1e-12)
            return;
        if (glm::dot(n, outward) < 0.0)
        {
            std::swap(b, c);
            n = -n;
        }
        SatTri t;
        t.p[0] = R * a + offset;
        t.p[1] = R * b + offset;
        t.p[2] = R * c + offset;
        t.n = glm::normalize(R * (n / len));
        t.area = 0.5 * len;
        t.spread2 = spread2;
        t.group = group;
        t.material = material;
        t.component = component;
        out.push_back(t);
    }
    void quad(glm::dvec3 a, glm::dvec3 b, glm::dvec3 c, glm::dvec3 d, glm::dvec3 outward, int material)
    {
        tri(a, b, c, outward, material);
        tri(a, c, d, outward, material);
    }
};

void tessRevolved(TriSink &s, double r0, double r1, double h, bool caps, int mat)
{
    // Frustum along Z: radius r0 at z = -h/2, r1 at z = +h/2. Flat-faceted, 32 segments.
    constexpr int N = 32;
    const double z0 = -0.5 * h, z1 = 0.5 * h;
    const double dphi = 2.0 * kPi / N;
    const double slope = (r0 - r1) / h; // outward normal tilts toward +Z when the top is narrower
    for (int i = 0; i < N; ++i)
    {
        double a0 = 2.0 * kPi * i / N, a1 = 2.0 * kPi * (i + 1) / N, am = 0.5 * (a0 + a1);
        glm::dvec3 d0(std::cos(a0), std::sin(a0), 0), d1(std::cos(a1), std::sin(a1), 0);
        glm::dvec3 out = glm::dvec3(std::cos(am), std::sin(am), slope);
        s.spread2 = dphi * dphi / 12.0; // smooth side: normals span Δφ within this facet
        s.quad(d0 * r0 + glm::dvec3(0, 0, z0), d1 * r0 + glm::dvec3(0, 0, z0), d1 * r1 + glm::dvec3(0, 0, z1),
               d0 * r1 + glm::dvec3(0, 0, z1), out, mat);
        s.spread2 = 0.0; // caps are flat
        if (caps && r1 > 0.0)
            s.tri(glm::dvec3(0, 0, z1), d0 * r1 + glm::dvec3(0, 0, z1), d1 * r1 + glm::dvec3(0, 0, z1),
                  glm::dvec3(0, 0, 1), mat);
        if (caps && r0 > 0.0)
            s.tri(glm::dvec3(0, 0, z0), d1 * r0 + glm::dvec3(0, 0, z0), d0 * r0 + glm::dvec3(0, 0, z0),
                  glm::dvec3(0, 0, -1), mat);
    }
}

void tessSphere(TriSink &s, double r, int mat)
{
    // Icosphere, 2 subdivisions (320 faces).
    const double t = (1.0 + std::sqrt(5.0)) / 2.0;
    std::vector<glm::dvec3> v = {{-1, t, 0}, {1, t, 0}, {-1, -t, 0}, {1, -t, 0}, {0, -1, t}, {0, 1, t},
                                 {0, -1, -t}, {0, 1, -t}, {t, 0, -1}, {t, 0, 1}, {-t, 0, -1}, {-t, 0, 1}};
    for (auto &p : v)
        p = glm::normalize(p);
    std::vector<glm::ivec3> f = {{0, 11, 5}, {0, 5, 1}, {0, 1, 7}, {0, 7, 10}, {0, 10, 11}, {1, 5, 9}, {5, 11, 4},
                                 {11, 10, 2}, {10, 7, 6}, {7, 1, 8}, {3, 9, 4}, {3, 4, 2}, {3, 2, 6}, {3, 6, 8},
                                 {3, 8, 9}, {4, 9, 5}, {2, 4, 11}, {6, 2, 10}, {8, 6, 7}, {9, 8, 1}};
    for (int level = 0; level < 2; ++level)
    {
        std::vector<glm::ivec3> nf;
        std::map<std::pair<int, int>, int> mid;
        auto midpoint = [&](int a, int b) {
            auto key = std::minmax(a, b);
            auto it = mid.find(key);
            if (it != mid.end())
                return it->second;
            v.push_back(glm::normalize(v[a] + v[b]));
            return mid[key] = (int)v.size() - 1;
        };
        for (auto &tri : f)
        {
            int ab = midpoint(tri.x, tri.y), bc = midpoint(tri.y, tri.z), ca = midpoint(tri.z, tri.x);
            nf.push_back({tri.x, ab, ca});
            nf.push_back({tri.y, bc, ab});
            nf.push_back({tri.z, ca, bc});
            nf.push_back({ab, bc, ca});
        }
        f.swap(nf);
    }
    s.spread2 = 2.0 / (double)f.size(); // Ω/2π with Ω = 4π/faces
    for (auto &tri : f)
    {
        glm::dvec3 a = v[tri.x] * r, b = v[tri.y] * r, c = v[tri.z] * r;
        s.tri(a, b, c, a + b + c, mat);
    }
    s.spread2 = 0.0;
}
} // namespace

std::vector<SatTri> tessellateSatModel(const SatModel &m)
{
    // Group origins in REST coordinates: a child's hinge point accumulates down the tree.
    std::vector<glm::dvec3> origin(m.groups.size(), glm::dvec3(0.0));
    for (size_t gi = 0; gi < m.groups.size(); ++gi)
        if (m.groups[gi].parent >= 0)
            origin[gi] = origin[m.groups[gi].parent] + glm::dvec3(m.groups[gi].hingePos);

    std::vector<SatTri> tris;
    for (size_t ci = 0; ci < m.components.size(); ++ci)
    {
        const SatComponent &c = m.components[ci];
        TriSink s{tris, glm::dmat3(glm::mat3_cast(c.rotation)), origin[c.group] + glm::dvec3(c.position),
                  c.group, (int)ci};
        switch (c.prim)
        {
        case PrimitiveKind::Plane:
        {
            double w = 0.5 * c.size.x, h = 0.5 * c.size.y;
            glm::dvec3 a(-w, -h, 0), b(w, -h, 0), cc(w, h, 0), d(-w, h, 0);
            s.quad(a, b, cc, d, glm::dvec3(0, 0, 1), c.material);
            if (!c.singleSided)
                s.quad(a, b, cc, d, glm::dvec3(0, 0, -1), c.backMaterial >= 0 ? c.backMaterial : c.material);
            break;
        }
        case PrimitiveKind::Box:
        {
            glm::dvec3 e = 0.5 * glm::dvec3(c.size);
            for (int axis = 0; axis < 3; ++axis)
                for (int sign = -1; sign <= 1; sign += 2)
                {
                    glm::dvec3 n(0.0);
                    n[axis] = sign;
                    int u = (axis + 1) % 3, v = (axis + 2) % 3;
                    glm::dvec3 p[4];
                    const double su[4] = {-1, 1, 1, -1}, sv[4] = {-1, -1, 1, 1};
                    for (int k = 0; k < 4; ++k)
                    {
                        p[k][axis] = sign * e[axis];
                        p[k][u] = su[k] * e[u];
                        p[k][v] = sv[k] * e[v];
                    }
                    s.quad(p[0], p[1], p[2], p[3], n, c.material);
                }
            break;
        }
        case PrimitiveKind::Cylinder:
            tessRevolved(s, c.radius, c.radius, c.height, c.caps, c.material);
            break;
        case PrimitiveKind::Cone:
            tessRevolved(s, c.radius, c.radiusTop, c.height, c.caps, c.material);
            break;
        case PrimitiveKind::Sphere:
            tessSphere(s, c.radius, c.material);
            break;
        }
    }
    return tris;
}

// ── Lobe bake ─────────────────────────────────────────────────────────────────────────────────
namespace
{
struct LobeAcc
{
    glm::dvec3 sumAN{0.0}; // Σ area · normal
    double sumA = 0.0;
    double sumAlb = 0.0, sumF0 = 0.0, sumA2 = 0.0; // area-weighted material parameters
    int group = 0;
    int material = -1; // -1 once lobes of different materials have been merged
    bool alive = true;

    glm::dvec3 dir() const
    {
        double l = glm::length(sumAN);
        return l > 1e-12 ? sumAN / l : glm::dvec3(0, 0, 1);
    }
    void absorb(const LobeAcc &o)
    {
        sumAN += o.sumAN;
        sumA += o.sumA;
        sumAlb += o.sumAlb;
        sumF0 += o.sumF0;
        sumA2 += o.sumA2;
        if (material != o.material)
            material = -1;
    }
};

// Merge preference between two lobes: same group only; same material strongly preferred; then the
// closest normals.
double mergeScore(const LobeAcc &a, const LobeAcc &b)
{
    if (a.group != b.group)
        return -1e9;
    return glm::dot(a.dir(), b.dir()) - (a.material != b.material || a.material < 0 ? 2.5 : 0.0);
}

// Intensity of one lobe (per unit irradiance) — the exact formula sat_orbit.comp evaluates.
double lobeIntensity(glm::dvec3 n, double area, double diffArea, double albedo, double f0, double a2,
                     glm::dvec3 s, glm::dvec3 o)
{
    double ns = glm::dot(n, s), no = glm::dot(n, o);
    if (ns <= 0.0 || no <= 0.0)
        return 0.0;
    double diffuse = albedo / kPi * diffArea * ns * no;
    glm::dvec3 hv = s + o;
    double hl = glm::length(hv);
    if (hl < 1e-12)
        return diffuse;
    glm::dvec3 h = hv / hl;
    double nh = glm::dot(n, h);
    double sin2 = glm::dot(glm::cross(n, h), glm::cross(n, h));
    double den = nh * nh * a2 + sin2;
    double D = a2 / (kPi * den * den);
    double sh = std::max(0.0, glm::dot(s, h));
    double F = f0 + (1.0 - f0) * std::pow(1.0 - sh, 5.0);
    auto G1 = [a2](double x) { return 2.0 * x / (x + std::sqrt(a2 + (1.0 - a2) * x * x)); };
    return diffuse + area * D * F * G1(ns) * G1(no) / 4.0;
}
} // namespace

std::vector<GpuSatLobe> bakeSatLobes(const SatModel &m, const std::vector<SatTri> &tris, int budget,
                                     SatLobeBakeStats &stats)
{
    stats.triangles = (int)tris.size();

    // 1) Exact merge: identical (group, material, normal to within ~0.5°) — every flat face.
    std::vector<LobeAcc> acc;
    for (const SatTri &t : tris)
    {
        const SatMaterial &mat = m.materials[t.material];
        LobeAcc *hit = nullptr;
        for (LobeAcc &a : acc)
            if (a.group == t.group && a.material == t.material && glm::dot(a.dir(), t.n) > 0.99996)
            {
                hit = &a;
                break;
            }
        LobeAcc one;
        one.sumAN = t.n * t.area;
        one.sumA = t.area;
        one.sumAlb = mat.diffuseAlbedo * t.area;
        one.sumF0 = mat.specularF0 * t.area;
        one.sumA2 = ((double)mat.roughness * mat.roughness + t.spread2) * t.area;
        one.group = t.group;
        one.material = t.material;
        if (hit)
            hit->absorb(one);
        else
            acc.push_back(one);
    }
    stats.exactLobes = (int)acc.size();

    // 2) Budget merge: repeatedly fuse the best-scoring pair, with a cached best partner per lobe.
    const int n = (int)acc.size();
    std::vector<int> best(n, -1);
    std::vector<double> bestScore(n, -1e18);
    auto refresh = [&](int i) {
        best[i] = -1;
        bestScore[i] = -1e18;
        for (int j = 0; j < n; ++j)
            if (j != i && acc[j].alive)
            {
                double sc = mergeScore(acc[i], acc[j]);
                if (sc > bestScore[i])
                {
                    bestScore[i] = sc;
                    best[i] = j;
                }
            }
    };
    int alive = n;
    if (alive > budget)
        for (int i = 0; i < n; ++i)
            refresh(i);
    while (alive > budget)
    {
        int bi = -1;
        for (int i = 0; i < n; ++i)
            if (acc[i].alive && best[i] >= 0 && (bi < 0 || bestScore[i] > bestScore[bi]))
                bi = i;
        if (bi < 0 || bestScore[bi] < -1e8)
            break; // only cross-group pairs left — cannot merge further
        int bj = best[bi];
        acc[bi].absorb(acc[bj]);
        acc[bj].alive = false;
        --alive;
        for (int i = 0; i < n; ++i)
            if (acc[i].alive && (i == bi || best[i] == bi || best[i] == bj))
                refresh(i);
    }

    // 3) Emit, in root triad coordinates.
    std::vector<GpuSatLobe> lobes;
    for (const LobeAcc &a : acc)
    {
        if (!a.alive || a.sumA <= 0.0)
            continue;
        double rbar = glm::length(a.sumAN) / a.sumA; // mean resultant length: 1 = flat, → 0 = spread
        GpuSatLobe L{};
        L.normalT = attTriadCoords(m.groups, a.group, glm::vec3(a.dir()));
        L.group = (uint32_t)a.group;
        L.area = (float)a.sumA;
        L.diffArea = (float)(a.sumA * rbar);
        L.albedoD = (float)(a.sumAlb / a.sumA);
        L.f0 = (float)(a.sumF0 / a.sumA);
        L.alpha2Mat = (float)(a.sumA2 / a.sumA + std::max(0.0, 2.0 * (1.0 - rbar)));
        L.visLayer = 0xFFFFFFFFu;
        lobes.push_back(L);
    }
    stats.lobes = (int)lobes.size();
    return lobes;
}

double evalSatLobes(const std::vector<GpuSatLobe> &lobes, const std::vector<AttitudeGroup> &groups,
                    glm::dvec3 s, glm::dvec3 o, double sourceAlpha2)
{
    // s, o in the REST body frame. Lobe normals are root-triad coordinates: n_body = B_root · nT.
    double sum = 0.0;
    for (const GpuSatLobe &L : lobes)
    {
        glm::dvec3 n = bodyTriad(groups[attRootOf(groups, (int)L.group)]) * glm::dvec3(L.normalT);
        sum += lobeIntensity(glm::normalize(n), L.area, L.diffArea, L.albedoD, L.f0, L.alpha2Mat + sourceAlpha2, s, o);
    }
    return sum;
}

double evalSatLobesPosed(const std::vector<GpuSatLobe> &lobes, const std::vector<AttitudeGroup> &groups,
                         const std::vector<GroupPose> &poses, glm::dvec3 s, glm::dvec3 o, double sourceAlpha2,
                         int *dominant)
{
    double sum = 0.0, best = 0.0;
    int bestIdx = -1;
    for (size_t li = 0; li < lobes.size(); ++li)
    {
        const GpuSatLobe &L = lobes[li];
        glm::dvec3 nBody = bodyTriad(groups[attRootOf(groups, (int)L.group)]) * glm::dvec3(L.normalT);
        glm::dvec3 n = glm::normalize(poses[L.group].R * nBody);
        double I = lobeIntensity(n, L.area, L.diffArea, L.albedoD, L.f0, L.alpha2Mat + sourceAlpha2, s, o);
        sum += I;
        if (I > best)
        {
            best = I;
            bestIdx = (int)li;
        }
    }
    if (dominant)
        *dominant = bestIdx;
    return sum;
}

double satLobeIntensity(glm::dvec3 n, double area, double diffArea, double albedo, double f0, double a2,
                        glm::dvec3 s, glm::dvec3 o)
{
    return lobeIntensity(n, area, diffArea, albedo, f0, a2, s, o);
}

void validateSatLobes(const SatModel &m, const std::vector<SatTri> &tris, const std::vector<GpuSatLobe> &lobes,
                      SatLobeBakeStats &stats)
{
    const double a2Sun = (double)kSunAlpha * kSunAlpha;
    std::mt19937 rng(12345);
    std::normal_distribution<double> nd;
    auto randDir = [&]() { return glm::normalize(glm::dvec3(nd(rng), nd(rng), nd(rng))); };

    auto brute = [&](glm::dvec3 s, glm::dvec3 o) {
        double sum = 0.0;
        for (const SatTri &t : tris)
        {
            const SatMaterial &mat = m.materials[t.material];
            double a2 = (double)mat.roughness * mat.roughness + t.spread2 + a2Sun;
            sum += lobeIntensity(t.n, t.area, t.area, mat.diffuseAlbedo, mat.specularF0, a2, s, o);
        }
        return sum;
    };

    // Random pairs, plus exact specular configurations for every lobe (random pairs almost never
    // land on a narrow glint, which is where a bad merge would show).
    std::vector<std::pair<glm::dvec3, glm::dvec3>> pairs;
    const size_t nRandom = 1500;
    for (size_t i = 0; i < nRandom; ++i)
        pairs.emplace_back(randDir(), randDir());
    for (const GpuSatLobe &L : lobes)
    {
        glm::dvec3 n = glm::normalize(bodyTriad(m.groups[attRootOf(m.groups, (int)L.group)]) * glm::dvec3(L.normalT));
        for (int i = 0; i < 40; ++i)
        {
            glm::dvec3 s = randDir();
            if (glm::dot(s, n) < 0.0)
                s = -s;
            pairs.emplace_back(s, glm::reflect(-s, n));
        }
    }

    std::vector<double> ref(pairs.size()), got(pairs.size());
    for (size_t i = 0; i < pairs.size(); ++i)
    {
        ref[i] = brute(pairs[i].first, pairs[i].second);
        got[i] = evalSatLobes(lobes, m.groups, pairs[i].first, pairs[i].second, a2Sun);
    }
    // Significance floor: 5 magnitudes below the median brightness of the random (non-glint)
    // configurations that see any light at all.
    std::vector<double> lit;
    for (size_t i = 0; i < nRandom; ++i)
        if (ref[i] > 0.0)
            lit.push_back(ref[i]);
    std::sort(lit.begin(), lit.end());
    const double floorI = lit.empty() ? 0.0 : 0.01 * lit[lit.size() / 2];
    std::vector<double> err;
    for (size_t i = 0; i < pairs.size(); ++i)
        if (ref[i] > floorI && ref[i] > 0.0)
            err.push_back(std::abs(2.5 * std::log10(std::max(got[i], 1e-30) / ref[i])));
    std::sort(err.begin(), err.end());
    stats.maxErrMag = err.empty() ? 0.0 : err.back();
    stats.p95ErrMag = err.empty() ? 0.0 : err[(size_t)(0.95 * (err.size() - 1))];
}

// ── Shadowing study ───────────────────────────────────────────────────────────────────────────
namespace
{
// Möller–Trumbore; true if the ray origin + t·dir (t > tMin) hits the triangle.
bool rayHitsTri(glm::dvec3 org, glm::dvec3 dir, const glm::dvec3 &a, const glm::dvec3 &b, const glm::dvec3 &c)
{
    glm::dvec3 e1 = b - a, e2 = c - a;
    glm::dvec3 pv = glm::cross(dir, e2);
    double det = glm::dot(e1, pv);
    if (std::abs(det) < 1e-14)
        return false;
    double inv = 1.0 / det;
    glm::dvec3 tv = org - a;
    double u = glm::dot(tv, pv) * inv;
    if (u < 0.0 || u > 1.0)
        return false;
    glm::dvec3 qv = glm::cross(tv, e1);
    double v = glm::dot(dir, qv) * inv;
    if (v < 0.0 || u + v > 1.0)
        return false;
    return glm::dot(e2, qv) * inv > 1e-6;
}
} // namespace

SatShadowStudy studySatShadowing(const SatModel &m, const std::vector<SatTri> &tris, int samples)
{
    const double a2Sun = (double)kSunAlpha * kSunAlpha;
    std::mt19937 rng(777);
    std::normal_distribution<double> nd;
    std::uniform_real_distribution<double> ud(0.0, 1.0);
    auto randDir = [&]() { return glm::normalize(glm::dvec3(nd(rng), nd(rng), nd(rng))); };
    // Stratified sample points (barycentric) per triangle for the visible-fraction estimate.
    const glm::dvec3 bary[4] = {{1.0 / 3, 1.0 / 3, 1.0 / 3}, {2.0 / 3, 1.0 / 6, 1.0 / 6},
                                {1.0 / 6, 2.0 / 3, 1.0 / 6}, {1.0 / 6, 1.0 / 6, 2.0 / 3}};

    std::vector<double> dmag, openI; // per lit configuration: dimming (mag) and unshadowed intensity
    int over01 = 0, over05 = 0;
    std::vector<SatTri> posed(tris.size());
    for (int iter = 0; iter < samples * 20 && (int)dmag.size() < samples; ++iter)
    {
        AttGeometry geo; // nadir -Z, velocity +X (zenith-up local frame)
        geo.sun = randDir();
        if (glm::dot(geo.sun, -geo.nadir) < -0.4)
            continue; // deep behind Earth from this satellite — unlit, irrelevant
        geo.siteIdeal = glm::normalize(geo.sun + geo.nadir);
        geo.tumbleAngle = ud(rng) * 2.0 * kPi;
        geo.tumbleAxis = randDir();
        // Observer on the ground: within ~60° of nadir as seen from LEO.
        glm::dvec3 o = randDir();
        if (glm::dot(o, geo.nadir) < 0.5)
            continue;

        std::vector<GroupPose> gp = evalGroupPoses(m.groups, geo, false);
        for (size_t i = 0; i < tris.size(); ++i)
        {
            const GroupPose &P = gp[tris[i].group];
            posed[i] = tris[i];
            for (int k = 0; k < 3; ++k)
                posed[i].p[k] = P.R * tris[i].p[k] + P.t;
            posed[i].n = P.R * tris[i].n;
        }

        double open = 0.0, shadowed = 0.0;
        for (size_t i = 0; i < posed.size(); ++i)
        {
            const SatTri &t = posed[i];
            const SatMaterial &mat = m.materials[t.material];
            double a2 = (double)mat.roughness * mat.roughness + t.spread2 + a2Sun;
            double I = lobeIntensity(t.n, t.area, t.area, mat.diffuseAlbedo, mat.specularF0, a2, geo.sun, o);
            if (I <= 0.0)
                continue;
            // Visible fraction: sample points that see BOTH the sun and the observer.
            int both = 0;
            for (const glm::dvec3 &w : bary)
            {
                glm::dvec3 pt = w.x * t.p[0] + w.y * t.p[1] + w.z * t.p[2] + t.n * 1e-6;
                bool blocked = false;
                for (size_t j = 0; j < posed.size() && !blocked; ++j)
                    if (j != i && (rayHitsTri(pt, geo.sun, posed[j].p[0], posed[j].p[1], posed[j].p[2]) ||
                                   rayHitsTri(pt, o, posed[j].p[0], posed[j].p[1], posed[j].p[2])))
                        blocked = true;
                both += blocked ? 0 : 1;
            }
            open += I;
            shadowed += I * both / 4.0;
        }
        if (open <= 0.0)
            continue;
        double d = shadowed > 0.0 ? 2.5 * std::log10(open / shadowed) : 10.0;
        dmag.push_back(d);
        openI.push_back(open);
        over01 += d > 0.1;
        over05 += d > 0.5;
    }

    SatShadowStudy s;
    s.samples = (int)dmag.size();
    if (dmag.empty())
        return s;
    {
        std::vector<double> sortedI = openI;
        std::sort(sortedI.begin(), sortedI.end());
        const double medI = sortedI[sortedI.size() / 2];
        std::vector<double> bright;
        for (size_t i = 0; i < dmag.size(); ++i)
            if (openI[i] >= medI)
                bright.push_back(dmag[i]);
        std::sort(bright.begin(), bright.end());
        s.brightP90Dmag = bright[(size_t)(0.9 * (bright.size() - 1))];
        for (double d : bright)
        {
            s.brightFracOver01 += d > 0.1;
            s.brightFracOver05 += d > 0.5;
        }
        s.brightFracOver01 /= bright.size();
        s.brightFracOver05 /= bright.size();
    }
    std::sort(dmag.begin(), dmag.end());
    auto pct = [&](double q) { return dmag[(size_t)(q * (dmag.size() - 1))]; };
    s.medianDmag = pct(0.5);
    s.p90Dmag = pct(0.9);
    s.p99Dmag = pct(0.99);
    s.maxDmag = dmag.back();
    s.fracOver01 = (double)over01 / dmag.size();
    s.fracOver05 = (double)over05 / dmag.size();
    return s;
}

// ── OBJ export ────────────────────────────────────────────────────────────────────────────────
bool writeSatModelObj(const SatModel &m, const std::vector<SatTri> &tris, const std::string &dir)
{
    std::error_code ec;
    std::filesystem::create_directories(dir, ec);
    const std::filesystem::path base = std::filesystem::path(dir) / m.id;

    {
        std::ofstream mtl(base.string() + ".mtl");
        if (!mtl.is_open())
            return false;
        for (const SatMaterial &mat : m.materials)
        {
            // Visual approximation only: Kd from the tint, Ks from F0, shininess from roughness.
            mtl << "newmtl " << mat.name << "\n"
                << "Kd " << mat.color.r << " " << mat.color.g << " " << mat.color.b << "\n"
                << "Ks " << mat.specularF0 << " " << mat.specularF0 << " " << mat.specularF0 << "\n"
                << "Ns " << std::clamp(2.0f / std::max(mat.roughness * mat.roughness, 1e-4f) - 2.0f, 1.0f, 1000.0f)
                << "\n\n";
        }
    }

    // Illustrative sunlit geometry, expressed in a local frame with +Z = zenith: satellite over
    // the observer, flying +X, sun 30° above the local horizontal and off the orbit plane so
    // out-of-plane gimbals visibly work. SunReflectGroundSite aims straight down for the export.
    AttGeometry sunlit;
    sunlit.sun = glm::normalize(glm::dvec3(0.75, 0.35, 0.5));
    sunlit.siteIdeal = glm::normalize(sunlit.sun + sunlit.nadir);
    struct Pose
    {
        const char *suffix;
        bool jointsAtZero;
    };
    // _rest: the model's own BODY frame (OBJ axes = body axes, every joint at 0) — for checking
    // dimensions and hinge placement independent of any attitude law.
    // _sunlit: the real attitude and joint solutions under the illustrative geometry above, in a
    // zenith-up local frame (converted to OBJ Y-up).
    for (const Pose &pose : {Pose{"_rest", true}, Pose{"_sunlit", false}})
    {
        const bool bodyFrame = pose.jointsAtZero;
        std::vector<GroupPose> gp = bodyFrame ? std::vector<GroupPose>(m.groups.size())
                                              : evalGroupPoses(m.groups, sunlit, false);
        std::ofstream obj(base.string() + pose.suffix + ".obj");
        if (!obj.is_open())
            return false;
        if (bodyFrame)
            obj << "# " << m.name << " (" << m.id << ") — rest pose: BODY frame, joints at 0\n"
                << "# OBJ axes = body axes (x, y, z) — no attitude applied.\n";
        else
            obj << "# " << m.name << " (" << m.id << ") — sunlit pose: real attitude + joint solutions\n"
                << "# Local frame (OBJ Y-up): +Y = zenith (Earth below), satellite flying +X.\n"
                << "# Sun direction (OBJ Y-up): " << sunlit.sun.x << " " << sunlit.sun.z << " " << -sunlit.sun.y << "\n";
        obj << "mtllib " << m.id << ".mtl\n";
        // Zenith-up local → OBJ Y-up is (x, y, z) → (x, z, −y); the body frame is written as-is.
        auto out = [bodyFrame](glm::dvec3 w) {
            return bodyFrame ? w : glm::dvec3(w.x, w.z, -w.y);
        };
        int vcount = 0, lastComponent = -1, lastMaterial = -1;
        for (const SatTri &t : tris)
        {
            if (t.component != lastComponent)
            {
                obj << "g " << m.components[t.component].name << "\n";
                lastComponent = t.component;
                lastMaterial = -1;
            }
            if (t.material != lastMaterial)
            {
                obj << "usemtl " << m.materials[t.material].name << "\n";
                lastMaterial = t.material;
            }
            const GroupPose &P = gp[t.group];
            for (int k = 0; k < 3; ++k)
            {
                glm::dvec3 w = out(P.R * t.p[k] + P.t);
                obj << "v " << w.x << " " << w.y << " " << w.z << "\n";
            }
            glm::dvec3 nw = out(P.R * t.n);
            obj << "vn " << nw.x << " " << nw.y << " " << nw.z << "\n";
            int ni = vcount / 3 + 1;
            obj << "f " << vcount + 1 << "//" << ni << " " << vcount + 2 << "//" << ni << " " << vcount + 3 << "//"
                << ni << "\n";
            vcount += 3;
        }
    }
    return true;
}
