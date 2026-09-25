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
bool attUsesGroundSite(const std::vector<AttitudeGroup> &groups)
{
    for (const AttitudeGroup &g : groups)
        if (g.primaryTarget == AttTarget::SunReflectGroundSite || g.secondaryTarget == AttTarget::SunReflectGroundSite ||
            (g.jointMode != JointMode::None && g.jointTarget == AttTarget::SunReflectGroundSite))
            return true;
    return false;
}

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

glm::dvec3 satPivotOffset(const std::vector<AttitudeGroup> &groups, const std::vector<GroupPose> &poses, int group,
                          const glm::dvec3 &pivot)
{
    if (group < 0 || group >= (int)groups.size() || groups[group].parent < 0 ||
        (pivot.x == 0.0 && pivot.y == 0.0 && pivot.z == 0.0))
        return glm::dvec3(0.0);
    // The group's pose turns about its hinge; turning about hinge + pivot instead differs by
    // (I − Rj)·R_parent·pivot = (R_parent − R_group)·pivot (see evalGroupPoses' child branch).
    return (poses[groups[group].parent].R - poses[group].R) * pivot;
}

// ── Materials ─────────────────────────────────────────────────────────────────────────────────
const std::vector<SatMaterial> &satMaterialPresetsBase();

int satMaterialPattern(const SatMaterial &m)
{
    if (m.pattern >= 0)
        return m.pattern;
    const std::string &p = m.preset.empty() ? m.name : m.preset;
    if (p == "solar_cell" || p == "solar_cell_flex" || p == "solar_array_flex_back")
        return kPatternSolarCells;
    if (p == "mli_foil")
        return kPatternMli;
    if (p == "truss")
        return kPatternTruss;
    return kPatternNone;
}

const std::vector<SatMaterial> &satMaterialPresets()
{
    static const std::vector<SatMaterial> withTransmission = [] {
        std::vector<SatMaterial> v = satMaterialPresetsBase();
        for (SatMaterial &m : v)
        {
            if (m.name == "solar_cell_flex" || m.name == "solar_array_flex_back")
                m.transmission = 0.05f;
            if (m.name == "truss")
                m.coverage = 0.3f;
        }
        return v;
    }();
    return withTransmission;
}

bool satComponentOccludes(const SatModel &m, int ci)
{
    if (ci < 0 || ci >= (int)m.components.size())
        return false;
    const SatComponent &c = m.components[ci];
    return !c.renderOnly && m.materials[c.material].coverage >= 1.0f;
}

const std::vector<SatMaterial> &satMaterialPresetsBase()
{
    // INITIAL ESTIMATES — calibrated against reference satellites in Phase 3c. Roughness is GGX α.
    static const std::vector<SatMaterial> presets = {
        // Solar cells under cover glass: dark cells, dielectric glass specular, panel-scale waviness.
        // Diffuse albedo CALIBRATED 2026-09-23 (M8): 0.06 -> 0.02 from Mallama 2021's low-phase bins
        // (40-60 deg, where the VisorSat array face dominates) — consistent with AR-coated cells,
        // whose few-percent reflectance is mostly the cover glass's specular (F0 below).
        {"solar_cell", 0.02f, 0.04f, 0.05f, {0.20f, 0.25f, 0.55f}},
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
        // Bare 304L stainless (Starship-class tanks): a metal - the specular is steel-tinted, F0 ~0.55
        // across V - with a broad lobe from the ring welds and the wavy, dented tank walls, and a little
        // diffuse from the brushed/oxidised finish. INITIAL ESTIMATE.
        {"stainless_steel", 0.05f, 0.55f, 0.12f, {0.76f, 0.76f, 0.75f}},
        // SpaceX's RF-transparent dielectric (Bragg) mirror film on Starlink nadir faces ("Brightness
        // Mitigation Best Practices", SpaceX 2022): it "specularly scatters the vast majority of sunlight
        // away from the Earth". Gen 2 (V2 Mini) is "10x better at reducing observed brightness than the
        // first-generation film" by SpaceX's BRDF metric - here a 10x smaller diffuse floor. INITIAL
        // ESTIMATES: specular reflectance ~0.9, a flat-panel-scale waviness.
        // Beckmann (Gaussian tails): a smooth dielectric stack has no long power-law tail. With GGX's tail
        // the film forward-scattered the grazing terminator sunlight and the V2 Mini benchmark read 7.43
        // vs 7.87; Beckmann gives 7.90 (chosen with that benchmark in view - KNOWN_RESIDUALS.md).
        {"dielectric_mirror_gen1", 0.030f, 0.90f, 0.03f, {0.70f, 0.72f, 0.78f}, true},
        {"dielectric_mirror_gen2", 0.003f, 0.90f, 0.03f, {0.70f, 0.72f, 0.78f}, true},
        // An opaque dark-pigmented array backsheet (V2 Mini: "an opaque pigment for the solar backsheet").
        {"array_backsheet_dark", 0.08f, 0.04f, 0.30f, {0.20f, 0.18f, 0.17f}},
        // Phase 4f — flexible arrays on a translucent Kapton blanket (ISS-style): the cell face and the
        // blanket's back. Light on either side leaks through the gaps between the cells as an amber
        // glow on the other side (transmission). INITIAL ESTIMATES: ~9% open area x ~0.6 Kapton
        // transmittance in V.
        {"solar_cell_flex", 0.02f, 0.04f, 0.05f, {0.20f, 0.25f, 0.55f}},
        {"solar_array_flex_back", 0.30f, 0.04f, 0.35f, {0.80f, 0.62f, 0.32f}},
        // Open lattice truss of anodised / bare aluminium members (coverage 0.3: set in
        // satMaterialPresets). INITIAL ESTIMATE; members read as a lightly brushed metal.
        {"truss", 0.30f, 0.60f, 0.30f, {0.72f, 0.72f, 0.74f}},
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
                {
                    m = matLib[base];
                    m.preset = m.preset.empty() ? base : m.preset;
                }
                else
                    warn.push_back("material preset '" + base + "' not found");
            }
            m.name = jm.at("name").get<std::string>();
            m.diffuseAlbedo = jm.value("diffuse_albedo", m.diffuseAlbedo);
            m.specularF0 = jm.value("specular_f0", m.specularF0);
            m.roughness = jm.value("roughness", m.roughness);
            if (jm.contains("distribution"))
            {
                const std::string d = jm.value("distribution", std::string());
                if (d == "beckmann" || d == "ggx")
                    m.beckmann = d == "beckmann";
                else
                    warn.push_back("material '" + m.name + "': unknown distribution '" + d + "' (ggx|beckmann)");
            }
            m.color = jsonVec3(jm, "color", m.color);
            m.transmission = jm.value("transmission", m.transmission);
            m.transmissionColor = jsonVec3(jm, "transmission_color", m.transmissionColor);
            m.coverage = jm.value("coverage", m.coverage);
            if (!(m.coverage > 0.0f && m.coverage <= 1.0f))
            {
                warn.push_back("material '" + m.name + "': coverage must be in (0, 1]; using 1");
                m.coverage = 1.0f;
            }
            m.trussPitch = std::max(0.05f, jm.value("truss_pitch", m.trussPitch));
            if (jm.contains("pattern"))
            {
                const std::string pat = jm.value("pattern", std::string());
                if (pat == "none")
                    m.pattern = kPatternNone;
                else if (pat == "solar_cells")
                    m.pattern = kPatternSolarCells;
                else if (pat == "mli")
                    m.pattern = kPatternMli;
                else if (pat == "panel_seams")
                    m.pattern = kPatternPanelSeams;
                else if (pat == "truss")
                    m.pattern = kPatternTruss;
                else
                    warn.push_back("material '" + m.name + "': unknown pattern '" + pat +
                                   "' (none|solar_cells|mli|panel_seams|truss)");
            }
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
            if (s.status != "sourced" && s.status != "derived" && s.status != "estimate" && s.status != "calibrated")
                warn.push_back("source for '" + subjects.front() + "': status must be sourced/derived/estimate/calibrated");
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
            c.pivot = jsonVec3(jc, "pivot", c.pivot);
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
            c.renderOnly = jc.value("render_only", false);
            out.components.push_back(std::move(c));
        }
        // Render-only detail goes last (stable): the photometric components keep indices 0..n-1, so
        // occluder i is component i for every consumer (sat_orbit.comp, sat_mesh.frag, the tool).
        std::stable_partition(out.components.begin(), out.components.end(),
                              [](const SatComponent &c) { return !c.renderOnly; });
        if (std::all_of(out.components.begin(), out.components.end(),
                        [](const SatComponent &c) { return c.renderOnly; }) &&
            !out.components.empty())
            throw std::runtime_error("every component is render_only: the model has nothing to light");
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
    for (const SatComponent &c : out.components)
    {
        if (c.pivot == glm::vec3(0.0f))
            continue;
        if (out.groups[c.group].parent < 0)
        {
            err = "component '" + c.name + "': a pivot needs a child group (a root has no joint to turn it)";
            return false;
        }
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
        if (!c.renderOnly && !covered(c.name)) // render-only detail has no photometric numbers to source
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
        if (c.renderOnly)
            continue; // visual detail: the render mesh draws it, the photometry never sees it
        const size_t firstTri = tris.size();
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
        // Open lattice (SatMaterial::coverage < 1): each facet counts `coverage` of its area; a closed
        // primitive's far side shows through the near side's gaps, as inner faces at
        // coverage·(1 − coverage).
        const bool closed = c.prim != PrimitiveKind::Plane;
        const size_t endTri = tris.size();
        for (size_t i = firstTri; i < endTri; ++i)
        {
            const double cov = m.materials[tris[i].material].coverage;
            if (cov >= 1.0)
                continue;
            const double full = tris[i].area;
            tris[i].area = full * cov;
            if (!closed)
                continue;
            SatTri in = tris[i];
            std::swap(in.p[1], in.p[2]);
            in.n = -in.n;
            in.area = full * cov * (1.0 - cov);
            tris.push_back(in);
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
    double sumT = 0.0;                              // area-weighted transmission (Phase 4f)
    double sumBeck = 0.0;                          // area of Beckmann faces
    int group = 0;
    int material = -1; // -1 once lobes of different materials have been merged
    bool alive = true;
    std::vector<int> tris; // source triangles (occlusion sample points)

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
        sumT += o.sumT;
        sumF0 += o.sumF0;
        sumA2 += o.sumA2;
        sumBeck += o.sumBeck;
        if (material != o.material)
            material = -1;
        tris.insert(tris.end(), o.tris.begin(), o.tris.end());
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
                     glm::dvec3 s, glm::dvec3 o, bool beckmann, double transmission = 0.0)
{
    double ns = glm::dot(n, s), no = glm::dot(n, o);
    if (no <= 0.0)
        return 0.0;
    // Phase 4f: light from BEHIND the face, transmitted diffusely through it.
    if (ns <= 0.0)
        return transmission > 0.0 ? transmission / kPi * diffArea * (-ns) * no : 0.0;
    double diffuse = albedo / kPi * diffArea * ns * no;
    glm::dvec3 hv = s + o;
    double hl = glm::length(hv);
    if (hl < 1e-12)
        return diffuse;
    glm::dvec3 h = hv / hl;
    double nh = glm::dot(n, h);
    double sin2 = glm::dot(glm::cross(n, h), glm::cross(n, h));
    double sh = std::max(0.0, glm::dot(s, h));
    double F = f0 + (1.0 - f0) * std::pow(1.0 - sh, 5.0);
    if (beckmann)
    {
        // Beckmann D and Walter et al. 2007's rational fit to its Smith G1.
        if (nh <= 0.0)
            return diffuse;
        double nh2 = nh * nh;
        double D = std::exp(-sin2 / (nh2 * a2)) / (kPi * a2 * nh2 * nh2);
        double alpha = std::sqrt(a2);
        auto G1 = [alpha](double x) {
            double c = x / (alpha * std::sqrt(std::max(1e-30, 1.0 - x * x)));
            return c >= 1.6 ? 1.0 : (3.535 * c + 2.181 * c * c) / (1.0 + 2.276 * c + 2.577 * c * c);
        };
        return diffuse + area * D * F * G1(ns) * G1(no) / 4.0;
    }
    double den = nh * nh * a2 + sin2;
    double D = a2 / (kPi * den * den);
    auto G1 = [a2](double x) { return 2.0 * x / (x + std::sqrt(a2 + (1.0 - a2) * x * x)); };
    return diffuse + area * D * F * G1(ns) * G1(no) / 4.0;
}
} // namespace

std::vector<GpuSatLobe> bakeSatLobes(const SatModel &m, const std::vector<SatTri> &tris, int budget,
                                     SatLobeBakeStats &stats, std::vector<std::vector<int>> *lobeTris)
{
    stats.triangles = (int)tris.size();

    // 1) Exact merge: identical (group, material, normal to within ~0.5°) — every flat face.
    std::vector<LobeAcc> acc;
    for (size_t ti = 0; ti < tris.size(); ++ti)
    {
        const SatTri &t = tris[ti];
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
        one.sumT = mat.transmission * t.area;
        one.sumF0 = mat.specularF0 * t.area;
        one.sumA2 = ((double)mat.roughness * mat.roughness + t.spread2) * t.area;
        one.sumBeck = mat.beckmann ? t.area : 0.0;
        one.group = t.group;
        one.material = t.material;
        one.tris.push_back((int)ti);
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
    if (lobeTris)
        lobeTris->clear();
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
        L.sampleFirst = 0;
        L.sampleCount = 0;
        L.occluderMask = 0;
        L.distribution = 2.0 * a.sumBeck > a.sumA ? 1u : 0u;
        L.transmission = (float)(a.sumT / a.sumA);
        lobes.push_back(L);
        if (lobeTris)
            lobeTris->push_back(a.tris);
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
        sum += lobeIntensity(glm::normalize(n), L.area, L.diffArea, L.albedoD, L.f0, L.alpha2Mat + sourceAlpha2, s, o,
                              L.distribution != 0, L.transmission);
    }
    return sum;
}

double evalSatLobesPosed(const std::vector<GpuSatLobe> &lobes, const std::vector<AttitudeGroup> &groups,
                         const std::vector<GroupPose> &poses, glm::dvec3 s, glm::dvec3 o, double sourceAlpha2,
                         int *dominant, const SatOcclusion *occ, bool occludeSource)
{
    double sum = 0.0, best = 0.0;
    int bestIdx = -1;
    for (size_t li = 0; li < lobes.size(); ++li)
    {
        const GpuSatLobe &L = lobes[li];
        glm::dvec3 nBody = bodyTriad(groups[attRootOf(groups, (int)L.group)]) * glm::dvec3(L.normalT);
        glm::dvec3 n = glm::normalize(poses[L.group].R * nBody);
        double I = lobeIntensity(n, L.area, L.diffArea, L.albedoD, L.f0, L.alpha2Mat + sourceAlpha2, s, o,
                              L.distribution != 0, L.transmission);
        if (I > 0.0 && occ && li < occ->lobes.size())
            I *= satLobeVisibility(*occ, (int)li, (int)L.group, poses, s, occludeSource, o);
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

double satLobeEarthIntensity(glm::dvec3 n, double area, double diffArea, double albedo, double f0, double a2,
                             bool beckmann, const SatEarthLight &earth, glm::dvec3 o, double transmission)
{
    const double no = glm::dot(n, o);
    if (no <= 0.0 || earth.E <= 0.0)
        return 0.0;
    // Diffuse (and transmitted, from the far side) under the whole cap; the specular glint of the
    // tilted source with the Earth's size folded into α — lobeIntensity() with no diffuse part.
    double I = (albedo * earth.diffuse(n) + transmission * earth.diffuse(-n)) / kPi * diffArea * no;
    if (glm::dot(n, earth.dir) > 0.0)
        I += earth.E * lobeIntensity(n, area, diffArea, 0.0, f0, a2 + earth.a2, earth.dir, o, beckmann, 0.0);
    return I;
}

double evalSatLobesEarthPosed(const std::vector<GpuSatLobe> &lobes, const std::vector<AttitudeGroup> &groups,
                              const std::vector<GroupPose> &poses, const SatEarthLight &earth, glm::dvec3 o,
                              const SatOcclusion *occ)
{
    double sum = 0.0;
    for (size_t li = 0; li < lobes.size(); ++li)
    {
        const GpuSatLobe &L = lobes[li];
        glm::dvec3 nBody = bodyTriad(groups[attRootOf(groups, (int)L.group)]) * glm::dvec3(L.normalT);
        glm::dvec3 n = glm::normalize(poses[L.group].R * nBody);
        double I = satLobeEarthIntensity(n, L.area, L.diffArea, L.albedoD, L.f0, L.alpha2Mat, L.distribution != 0,
                                         earth, o, L.transmission);
        if (I > 0.0 && occ && li < occ->lobes.size())
            I *= satLobeVisibility(*occ, (int)li, (int)L.group, poses, earth.dir, false, o);
        sum += I;
    }
    return sum;
}

double satLobeIntensity(glm::dvec3 n, double area, double diffArea, double albedo, double f0, double a2, bool beckmann,
                        glm::dvec3 s, glm::dvec3 o, double transmission)
{
    return lobeIntensity(n, area, diffArea, albedo, f0, a2, s, o, beckmann, transmission);
}

// ── Phase 3b: occlusion between parts ─────────────────────────────────────────────────────────
namespace
{
// Corner / bounding points of an occluder in the rest frame (for the bake-time candidate test).
std::vector<glm::dvec3> occluderPoints(const SatOccluder &o)
{
    std::vector<glm::dvec3> pts;
    const double hz = o.kind == PrimitiveKind::Plane ? 0.0 : o.half.z;
    for (int sx = -1; sx <= 1; sx += 2)
        for (int sy = -1; sy <= 1; sy += 2)
            for (int sz = -1; sz <= 1; sz += 2)
                pts.push_back(o.center + o.axes * glm::dvec3(sx * o.half.x, sy * o.half.y, sz * hz));
    return pts;
}

// True if the ray org + t·dir (t > tMin) hits the occluder. Both in the occluder's LOCAL frame.
bool rayHitsOccluder(const SatOccluder &o, glm::dvec3 org, glm::dvec3 dir)
{
    constexpr double tMin = 1e-6;
    switch (o.kind)
    {
    case PrimitiveKind::Plane:
    {
        if (std::abs(dir.z) < 1e-12)
            return false;
        double t = -org.z / dir.z;
        if (t <= tMin)
            return false;
        glm::dvec3 p = org + t * dir;
        return std::abs(p.x) <= o.half.x && std::abs(p.y) <= o.half.y;
    }
    case PrimitiveKind::Box:
    {
        double tNear = -1e300, tFar = 1e300;
        for (int a = 0; a < 3; ++a)
        {
            if (std::abs(dir[a]) < 1e-15)
            {
                if (std::abs(org[a]) > o.half[a])
                    return false;
                continue;
            }
            double t0 = (-o.half[a] - org[a]) / dir[a], t1 = (o.half[a] - org[a]) / dir[a];
            if (t0 > t1)
                std::swap(t0, t1);
            tNear = std::max(tNear, t0);
            tFar = std::min(tFar, t1);
            if (tNear > tFar)
                return false;
        }
        return tFar > tMin;
    }
    case PrimitiveKind::Cylinder:
    case PrimitiveKind::Cone: // bounded by the cylinder of its larger radius (conservative)
    {
        const double r = o.half.x, hh = o.half.z;
        // Side: |xy(org + t dir)| = r with |z| <= hh.
        double a = dir.x * dir.x + dir.y * dir.y;
        double b = 2.0 * (org.x * dir.x + org.y * dir.y);
        double c = org.x * org.x + org.y * org.y - r * r;
        if (a > 1e-15)
        {
            double disc = b * b - 4.0 * a * c;
            if (disc >= 0.0)
            {
                double sq = std::sqrt(disc);
                for (double t : {(-b - sq) / (2.0 * a), (-b + sq) / (2.0 * a)})
                    if (t > tMin && std::abs(org.z + t * dir.z) <= hh)
                        return true;
            }
        }
        // Caps.
        if (std::abs(dir.z) > 1e-15)
            for (double zc : {-hh, hh})
            {
                double t = (zc - org.z) / dir.z;
                if (t > tMin)
                {
                    glm::dvec3 p = org + t * dir;
                    if (p.x * p.x + p.y * p.y <= r * r)
                        return true;
                }
            }
        return false;
    }
    case PrimitiveKind::Sphere:
    {
        const double r = o.half.x;
        double b = glm::dot(org, dir), c = glm::dot(org, org) - r * r;
        double disc = b * b - c;
        if (disc < 0.0)
            return false;
        double sq = std::sqrt(disc);
        return (-b - sq) > tMin || (-b + sq) > tMin;
    }
    }
    return false;
}
} // namespace

std::vector<glm::dvec3> satSubTriangleCentroids(int m)
{
    std::vector<glm::dvec3> out;
    m = std::max(1, m);
    for (int i = 0; i < m; ++i)
        for (int j = 0; j < m - i; ++j)
        {
            // The "up" sub-triangle with corner (i, j), and the "down" one beside it when it exists.
            double a = (i + 1.0 / 3.0) / m, b = (j + 1.0 / 3.0) / m;
            out.push_back({a, b, 1.0 - a - b});
            if (i + j < m - 1)
            {
                a = (i + 2.0 / 3.0) / m;
                b = (j + 2.0 / 3.0) / m;
                out.push_back({a, b, 1.0 - a - b});
            }
        }
    return out;
}

SatOcclusion buildSatOcclusion(const SatModel &m, const std::vector<SatTri> &tris,
                               const std::vector<GpuSatLobe> &lobes, const std::vector<std::vector<int>> &lobeTris,
                               int samplesPerLobe)
{
    samplesPerLobe = std::clamp(samplesPerLobe, 1, kMaxLobeSamples);
    SatOcclusion occ;
    occ.groups = m.groups;
    for (const SatComponent &c : m.components)
        occ.compPivot.push_back(glm::dvec3(c.pivot));
    // Occluders: one per component, in the rest frame (same transform the tessellator uses).
    std::vector<glm::dvec3> origin(m.groups.size(), glm::dvec3(0.0));
    for (size_t gi = 0; gi < m.groups.size(); ++gi)
        if (m.groups[gi].parent >= 0)
            origin[gi] = origin[m.groups[gi].parent] + glm::dvec3(m.groups[gi].hingePos);
    for (size_t ci = 0; ci < m.components.size(); ++ci)
    {
        const SatComponent &c = m.components[ci];
        if (c.renderOnly)
            break; // render-only detail sits after every photometric component (loadSatModel)
        if ((int)occ.occluders.size() >= kMaxOccluders)
        {
            ++occ.droppedOccluders;
            continue;
        }
        SatOccluder o;
        o.kind = c.prim;
        o.blocks = satComponentOccludes(m, (int)ci);
        o.group = c.group;
        o.pivot = glm::dvec3(c.pivot);
        o.center = origin[c.group] + glm::dvec3(c.position);
        o.axes = glm::dmat3(glm::mat3_cast(c.rotation));
        switch (c.prim)
        {
        case PrimitiveKind::Plane: o.half = {0.5 * c.size.x, 0.5 * c.size.y, 0.0}; break;
        case PrimitiveKind::Box: o.half = 0.5 * glm::dvec3(c.size); break;
        case PrimitiveKind::Cylinder: o.half = {c.radius, c.radius, 0.5 * c.height}; break;
        case PrimitiveKind::Cone:
        {
            double r = std::max(c.radius, c.radiusTop);
            o.half = {r, r, 0.5 * c.height};
            break;
        }
        case PrimitiveKind::Sphere: o.half = glm::dvec3(c.radius); break;
        }
        occ.occluders.push_back(o);
    }

    // Per lobe: candidate points (16 evenly spread sub-triangle centroids per source triangle, equal
    // area shares — enough for 16 representatives per component even on one that is a single
    // rectangle; more on a triangle over 32 m², ~one per 2 m², so the representatives of a large
    // array blanket are not picked from a coarse grid), clustered into area-weighted representatives by a few rounds of weighted k-means
    // (seeded by farthest-point selection — deterministic). Each representative is an actual
    // candidate, so it lies on the lobe's surface and keeps that triangle's normal.
    std::vector<std::vector<glm::dvec3>> barySets(13);
    auto baryFor = [&](double area) -> const std::vector<glm::dvec3> & {
        const int k = std::clamp((int)std::ceil(std::sqrt(area / 2.0)), 4, 12);
        if (barySets[k].empty())
            barySets[k] = satSubTriangleCentroids(k);
        return barySets[k];
    };
    occ.lobes.resize(lobes.size());
    for (size_t li = 0; li < lobes.size() && li < lobeTris.size(); ++li)
    {
        struct Cand
        {
            glm::dvec3 p, n;
            double w;
            int comp;
        };
        std::vector<Cand> cand;
        double wTot = 0.0;
        for (int ti : lobeTris[li])
        {
            const SatTri &t = tris[ti];
            const std::vector<glm::dvec3> &bary = baryFor(t.area);
            for (const glm::dvec3 &w : bary)
                cand.push_back({w.x * t.p[0] + w.y * t.p[1] + w.z * t.p[2], t.n, t.area / bary.size(), t.component});
            wTot += t.area;
        }
        SatLobeSamples &S = occ.lobes[li];
        if (cand.empty() || wTot <= 0.0)
            continue;
        // Weighted k-means over a subset of the candidates, appending k representatives to S.
        auto cluster = [&](const std::vector<int> &idx, int k) {
            k = std::min<int>(k, (int)idx.size());
            if (k <= 0)
                return;
            double wSub = 0.0;
            glm::dvec3 cen(0.0);
            for (int i : idx)
            {
                cen += cand[i].w * cand[i].p;
                wSub += cand[i].w;
            }
            cen /= wSub;
            // Seeds: the candidate nearest the weighted centroid, then farthest-point.
            std::vector<glm::dvec3> centers;
            size_t first = 0;
            for (size_t i = 1; i < idx.size(); ++i)
                if (glm::length(cand[idx[i]].p - cen) < glm::length(cand[idx[first]].p - cen))
                    first = i;
            centers.push_back(cand[idx[first]].p);
            while ((int)centers.size() < k)
            {
                size_t far = 0;
                double farD = -1.0;
                for (size_t i = 0; i < idx.size(); ++i)
                {
                    double d = 1e300;
                    for (const glm::dvec3 &c : centers)
                        d = std::min(d, glm::length(cand[idx[i]].p - c));
                    if (d > farD)
                    {
                        farD = d;
                        far = i;
                    }
                }
                centers.push_back(cand[idx[far]].p);
            }
            std::vector<int> assign(idx.size(), 0);
            for (int iter = 0; iter < 8; ++iter)
            {
                for (size_t i = 0; i < idx.size(); ++i)
                {
                    int bestC = 0;
                    for (int c = 1; c < k; ++c)
                        if (glm::length(cand[idx[i]].p - centers[c]) < glm::length(cand[idx[i]].p - centers[bestC]))
                            bestC = c;
                    assign[i] = bestC;
                }
                for (int c = 0; c < k; ++c)
                {
                    glm::dvec3 sum(0.0);
                    double ws = 0.0;
                    for (size_t i = 0; i < idx.size(); ++i)
                        if (assign[i] == c)
                        {
                            sum += cand[idx[i]].w * cand[idx[i]].p;
                            ws += cand[idx[i]].w;
                        }
                    if (ws > 0.0)
                        centers[c] = sum / ws;
                }
            }
            for (int c = 0; c < k && S.count < kMaxLobeSamples; ++c)
            {
                double ws = 0.0;
                size_t rep = idx.size();
                for (size_t i = 0; i < idx.size(); ++i)
                    if (assign[i] == c)
                    {
                        ws += cand[idx[i]].w;
                        if (rep == idx.size() ||
                            glm::length(cand[idx[i]].p - centers[c]) < glm::length(cand[idx[rep]].p - centers[c]))
                            rep = i;
                    }
                if (ws <= 0.0 || rep == idx.size())
                    continue;
                const Cand &r = cand[idx[rep]];
                S.p[S.count] = r.p;
                S.n[S.count] = r.n;
                S.w[S.count] = ws / wTot;
                S.comp[S.count] = r.comp;
                ++S.count;
            }
        };
        // A lobe that merges several components (the ISS's 16 array blankets share one normal and
        // material) gets `samplesPerLobe` per component, up to kMaxLobeSamples, shared out by area and
        // clustered within each component: one pooled k-means left ~1 sample per blanket, so a shadow
        // across part of a blanket (an iROSA's, the truss's) was all or nothing. A single-component
        // lobe takes exactly the path it always did.
        std::vector<int> compOrder;
        std::vector<std::vector<int>> compIdx;
        std::vector<double> compW;
        for (int i = 0; i < (int)cand.size(); ++i)
        {
            size_t ci = 0;
            while (ci < compOrder.size() && compOrder[ci] != cand[i].comp)
                ++ci;
            if (ci == compOrder.size())
            {
                compOrder.push_back(cand[i].comp);
                compIdx.emplace_back();
                compW.push_back(0.0);
            }
            compIdx[ci].push_back(i);
            compW[ci] += cand[i].w;
        }
        const int nComp = (int)compOrder.size();
        const int kTot = std::min(kMaxLobeSamples, samplesPerLobe * nComp);
        std::vector<int> all(cand.size());
        for (int i = 0; i < (int)cand.size(); ++i)
            all[i] = i;
        if (nComp == 1 || nComp > kTot)
            cluster(all, nComp == 1 ? samplesPerLobe : kTot);
        else
        {
            // Largest-remainder share of kTot by area, at least one sample per component.
            std::vector<int> kc(nComp, 1);
            int left = kTot - nComp;
            std::vector<double> want(nComp);
            for (int c = 0; c < nComp; ++c)
                want[c] = std::max(0.0, kTot * compW[c] / wTot - 1.0);
            for (int c = 0; c < nComp && left > 0; ++c)
            {
                const int add = std::min(left, (int)std::floor(want[c]));
                kc[c] += add;
                want[c] -= add;
                left -= add;
            }
            while (left > 0)
            {
                int best = 0;
                for (int c = 1; c < nComp; ++c)
                    if (want[c] > want[best])
                        best = c;
                ++kc[best];
                want[best] -= 1.0;
                --left;
            }
            for (int c = 0; c < nComp; ++c)
                cluster(compIdx[c], kc[c]);
        }

        // Candidate occluders: one in ANOTHER group may move in front at some joint angle, so it
        // always counts; one in the SAME group is rigidly placed and counts only if some part of it
        // lies in front of some sample's surface.
        // A translucent lobe (Phase 4f transmission) is lit from BEHIND as well, so a same-group
        // occluder on either side of it can shade it: the ISS's iROSAs sit in front of the legacy
        // blankets and shadow the light those blankets pass to their backs. Front-only masks made
        // that the model's largest occlusion error (self-test p95 0.26 mag; 0.10 with opaque arrays).
        const int lobeGroup = (int)lobes[li].group;
        const bool bothSides = lobes[li].transmission > 0.0f;
        for (size_t oi = 0; oi < occ.occluders.size(); ++oi)
        {
            const SatOccluder &o = occ.occluders[oi];
            if (!o.blocks)
                continue; // an open lattice: light passes (its facets' coverage already counts it)
            bool candidate = o.group != lobeGroup;
            if (!candidate)
                for (const glm::dvec3 &q : occluderPoints(o))
                    for (int s = 0; s < S.count && !candidate; ++s)
                    {
                        const double h = glm::dot(q - S.p[s], S.n[s]);
                        if (h > 1e-6 || (bothSides && h < -1e-6))
                            candidate = true;
                    }
            if (candidate)
                S.occluderMask |= uint64_t(1) << oi;
        }
    }
    return occ;
}

double satLobeVisibility(const SatOcclusion &occ, int li, int lobeGroup, const std::vector<GroupPose> &poses,
                         glm::dvec3 src, bool testSource, glm::dvec3 obs)
{
    const SatLobeSamples &S = occ.lobes[li];
    if (S.count == 0 || S.occluderMask == 0)
        return 1.0;
    const GroupPose &P = poses[lobeGroup];
    double vis = 0.0;
    for (int s = 0; s < S.count; ++s)
    {
        // World position of the sample, nudged off its own surface.
        const glm::dvec3 nw = P.R * S.n[s];
        const int sc = S.comp[s];
        const glm::dvec3 pw = P.R * S.p[s] + P.t + 1e-4 * nw +
                              (sc >= 0 && sc < (int)occ.compPivot.size()
                                   ? satPivotOffset(occ.groups, poses, lobeGroup, occ.compPivot[sc])
                                   : glm::dvec3(0.0));
        bool blocked = false;
        for (size_t oi = 0; oi < occ.occluders.size() && !blocked; ++oi)
        {
            if (!(S.occluderMask & (uint64_t(1) << oi)) || (int)oi == S.comp[s])
                continue;
            const SatOccluder &o = occ.occluders[oi];
            const GroupPose &Q = poses[o.group];
            const glm::dvec3 Qt = Q.t + satPivotOffset(occ.groups, poses, o.group, o.pivot);
            // World → occluder group rest frame → occluder local frame.
            const glm::dmat3 toLocal = glm::transpose(o.axes) * glm::transpose(Q.R);
            const glm::dvec3 org = glm::transpose(o.axes) * (glm::transpose(Q.R) * (pw - Qt) - o.center);
            if (rayHitsOccluder(o, org, toLocal * obs) || (testSource && rayHitsOccluder(o, org, toLocal * src)))
                blocked = true;
        }
        if (!blocked)
            vis += S.w[s];
    }
    return vis;
}

GpuSatOcclusionPack packSatOcclusionGpu(const std::vector<AttitudeGroup> &groups, const SatOcclusion &occ,
                                        std::vector<GpuSatLobe> &lobes)
{
    GpuSatOcclusionPack pack;
    // Rest → root triad coordinates of a group's tree: c = Bᵀ v (as attTriadCoords, for points too).
    auto toTriad = [&](int gi, glm::dvec3 v) {
        return glm::vec3(glm::transpose(bodyTriad(groups[attRootOf(groups, gi)])) * v);
    };
    std::vector<glm::dvec3> origin(groups.size(), glm::dvec3(0.0));
    for (size_t gi = 0; gi < groups.size() && gi < (size_t)kMaxAttitudeGroups; ++gi)
    {
        if (groups[gi].parent >= 0)
            origin[gi] = origin[groups[gi].parent] + glm::dvec3(groups[gi].hingePos);
        pack.originT[gi] = glm::vec4(toTriad((int)gi, origin[gi]), 0.0f);
    }
    for (const SatOccluder &o : occ.occluders)
    {
        GpuSatOccluder g{};
        g.centerT = toTriad(o.group, o.center);
        g.kind = (uint32_t)o.kind;
        g.half = glm::vec3(o.half);
        g.group = (uint32_t)o.group;
        g.axisXT = toTriad(o.group, o.axes[0]);
        g.axisYT = toTriad(o.group, o.axes[1]);
        g.axisZT = toTriad(o.group, o.axes[2]);
        const glm::vec3 pv = toTriad(o.group, o.pivot); // a vector: toTriad is linear
        g.pivotXT = pv.x;
        g.pivotYT = pv.y;
        g.pivotZT = pv.z;
        pack.occluders.push_back(g);
    }
    for (size_t li = 0; li < lobes.size(); ++li)
    {
        GpuSatLobe &L = lobes[li];
        L.sampleFirst = (uint32_t)pack.samples.size();
        L.sampleCount = 0;
        L.occluderMask = 0;
        L.occluderMaskHi = 0;
        if (li >= occ.lobes.size() || occ.lobes[li].count == 0 || occ.lobes[li].occluderMask == 0)
            continue;
        const SatLobeSamples &S = occ.lobes[li];
        for (int s = 0; s < S.count; ++s)
        {
            GpuSatLobeSample g{};
            // The nudge satLobeVisibility applies after posing; rigid, so it can be baked in.
            g.pT = toTriad((int)L.group, S.p[s] + 1e-4 * S.n[s]);
            g.weight = (float)S.w[s];
            g.comp = (uint32_t)S.comp[s];
            pack.samples.push_back(g);
        }
        L.sampleCount = (uint32_t)S.count;
        L.occluderMask = (uint32_t)(S.occluderMask & 0xFFFFFFFFu);
        L.occluderMaskHi = (uint32_t)(S.occluderMask >> 32);
    }
    return pack;
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
            sum += lobeIntensity(t.n, t.area, t.area, mat.diffuseAlbedo, mat.specularF0, a2, s, o, mat.beckmann,
                                 mat.transmission);
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
            const glm::dvec3 dp = satPivotOffset(m.groups, gp, tris[i].group,
                                                 glm::dvec3(m.components[tris[i].component].pivot));
            for (int k = 0; k < 3; ++k)
                posed[i].p[k] = P.R * tris[i].p[k] + P.t + dp;
            posed[i].n = P.R * tris[i].n;
        }

        double open = 0.0, shadowed = 0.0;
        for (size_t i = 0; i < posed.size(); ++i)
        {
            const SatTri &t = posed[i];
            const SatMaterial &mat = m.materials[t.material];
            double a2 = (double)mat.roughness * mat.roughness + t.spread2 + a2Sun;
            double I = lobeIntensity(t.n, t.area, t.area, mat.diffuseAlbedo, mat.specularF0, a2, geo.sun, o, mat.beckmann,
                                    mat.transmission);
            if (I <= 0.0)
                continue;
            // Visible fraction: sample points that see BOTH the sun and the observer.
            int both = 0;
            for (const glm::dvec3 &w : bary)
            {
                glm::dvec3 pt = w.x * t.p[0] + w.y * t.p[1] + w.z * t.p[2] + t.n * 1e-6;
                bool blocked = false;
                for (size_t j = 0; j < posed.size() && !blocked; ++j)
                    if (j != i && satComponentOccludes(m, posed[j].component) && (rayHitsTri(pt, geo.sun, posed[j].p[0], posed[j].p[1], posed[j].p[2]) ||
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
            const glm::dvec3 dp = satPivotOffset(m.groups, gp, t.group, glm::dvec3(m.components[t.component].pivot));
            for (int k = 0; k < 3; ++k)
            {
                glm::dvec3 w = out(P.R * t.p[k] + P.t + dp);
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
