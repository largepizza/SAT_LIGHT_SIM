#pragma once
// ── SatModel — satellite geometry, materials, attitude and baked reflectance ──────────────────
// Lighting overhaul (.plans/SAT_LIGHTING_PLAN.md, .plans/SAT_LIGHTING_PHASE3.md).
//
// Phase 2 introduced data-driven attitude (rigid groups with two-vector laws and 1-DOF joints).
// Phase 3 adds geometry: a satellite MODEL is a kinematic tree of those groups (a root with an
// attitude law, children hinged to their parent) carrying primitive components with physical
// materials. At load the model is tessellated once and baked into a short list of facet LOBES —
// each exact for a flat face — that sat_orbit.comp evaluates per visible satellite. The same
// triangle mesh feeds the brute-force validator and the OBJ shape export, and will feed the
// Phase 4 in-sim renderer, so lighting and visuals cannot disagree about where a part is.

#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <nlohmann/json_fwd.hpp>

#include <cstdint>
#include <string>
#include <vector>

// ── Attitude ──────────────────────────────────────────────────────────────────────────────────
// A satellite is one or more RIGID GROUPS. A ROOT group's orientation is a two-vector law: a
// primary body axis points exactly at a target direction, and a secondary body axis points as
// close as it can to a second target (TRIAD — the align/constrain scheme STK and GMAT use). A
// CHILD group (parent >= 0) is hinged to its parent: at joint angle 0 its body frame IS the
// parent's, and its joint rotates it about the hinge axis. Any group may carry a 1-DOF joint
// (for a root it rotates the root about one of its own body axes — the legacy knife-edge roll).
// Everything is a closed-form function of the current sim time — no integrated state — so
// sat_orbit.comp's forward/reverse reproducibility is unaffected.

// Directions an attitude law or joint can aim at. Must match the ATT_T_* constants in
// sat_orbit.comp.
enum class AttTarget : uint32_t
{
    Nadir = 0,            // toward Earth's centre
    Zenith,               // away from Earth's centre
    Sun,
    AntiSun,
    Velocity,             // along-track (circular orbits)
    AntiVelocity,
    OrbitNormal,          // r × v
    AntiOrbitNormal,
    SunReflectNadir,      // the normal that reflects sunlight straight down: normalize(sun + nadir)
    SunReflectGroundSite, // the normal that reflects sunlight onto the satellite's chosen ground
                          // site (TargetedReflector's lock-window + rate-limited-ease machinery)
    Count
};

enum class AttLaw : uint32_t
{
    TwoVector = 0, // primary/secondary targets (TRIAD)
    Tumble,        // uncontrolled spin about the satellite's own random axis (GpuSatOrbit tumble*)
    Count
};

enum class JointMode : uint32_t
{
    None = 0,
    Track,               // rotate about the joint axis so body `vector` points as close to `target`
                         // as possible (a solar-array gimbal)
    EdgeOn,              // rotate so body `vector` is perpendicular to `target` (smaller solution)
    FixedAngle,          // constant rotation of `angleDeg`
    FlareMitigationTilt, // rotation by the global flareMitigationTiltDeg slider
    Count
};

// Upper bound on groups per type the GPU record carries (root + children, parents first).
// Must match SatType::groups in sat_orbit.comp.
static constexpr int kMaxAttitudeGroups = 4;

struct AttitudeGroup
{
    std::string name;
    int parent = -1;                       // index of the parent group (must be lower); -1 = root
    glm::vec3 hingePos{0.0f};              // child only: hinge point in the parent's body frame (m)
    AttLaw law = AttLaw::TwoVector;        // root only
    glm::vec3 primaryAxis{0.0f, 0.0f, 1.0f}; // root only: body axis that points exactly at primaryTarget
    AttTarget primaryTarget = AttTarget::Nadir;
    glm::vec3 secondaryAxis{1.0f, 0.0f, 0.0f}; // root only: body axis pointed as close as possible at secondaryTarget
    AttTarget secondaryTarget = AttTarget::Velocity;
    JointMode jointMode = JointMode::None;
    glm::vec3 jointAxis{1.0f, 0.0f, 0.0f};   // rotation axis (body frame; a child's hinge axis)
    glm::vec3 jointVector{0.0f, 0.0f, 1.0f}; // body vector the Track/EdgeOn objective applies to
    AttTarget jointTarget = AttTarget::Sun;
    float jointLimitDeg = 180.0f; // |rotation| clamp for Track/EdgeOn
    float jointAngleDeg = 0.0f;   // FixedAngle rotation

    bool operator==(const AttitudeGroup &o) const
    {
        return parent == o.parent && hingePos == o.hingePos && law == o.law &&
               primaryAxis == o.primaryAxis && primaryTarget == o.primaryTarget &&
               secondaryAxis == o.secondaryAxis && secondaryTarget == o.secondaryTarget &&
               jointMode == o.jointMode && jointAxis == o.jointAxis && jointVector == o.jointVector &&
               jointTarget == o.jointTarget && jointLimitDeg == o.jointLimitDeg &&
               jointAngleDeg == o.jointAngleDeg;
    }
};

// One rigid attitude group, GPU form (64 bytes, std430). Every body-frame vector is stored in the
// ROOT group's TRIAD coordinates — (t1b·v, t2b·v, t3b·v), with t1b = primaryAxis, t2b =
// normalize(primaryAxis × secondaryAxis), t3b = t1b × t2b; identity for the Tumble law — so the
// shader only builds the root's WORLD triad and rotates it down the tree. The body-frame half of
// the TRIAD construction is per-type constant and is done once on the CPU, in double precision.
// Must match AttGroup in sat_orbit.comp exactly.
struct GpuAttGroup
{
    uint32_t law;             // AttLaw (roots only)
    uint32_t primaryTarget;   // AttTarget (roots only)
    uint32_t secondaryTarget; // AttTarget (roots only)
    uint32_t jointMode;       // JointMode

    glm::vec3 jointAxisT;     // joint axis, root triad coordinates
    uint32_t jointTarget;     // AttTarget

    glm::vec3 jointVectorT;   // joint objective vector, root triad coordinates
    float jointLimitRad;

    float jointAngleRad;
    uint32_t parent; // parent group index, 0xFFFFFFFF = root
    float pad0, pad1;
};
static_assert(sizeof(GpuAttGroup) == 64, "GpuAttGroup layout mismatch");

// Names used in JSON (constellations.json "attitude_groups", model files, and the resolved dump).
const char *attTargetName(AttTarget t);
const char *attLawName(AttLaw l);
const char *jointModeName(JointMode m);
bool parseAttTargetName(const std::string &s, AttTarget &out);
bool parseAttLawName(const std::string &s, AttLaw &out);
bool parseJointModeName(const std::string &s, JointMode &out);

// Parses one attitude group. Parent NAMES are resolved to indices by the caller once every group
// of the list has been read (see resolveGroupParents). `warn` receives human-readable problems.
AttitudeGroup parseAttitudeGroupJson(const nlohmann::json &jg, size_t idx, std::string &parentName,
                                     std::vector<std::string> &warn);
// Resolves parent names collected by parseAttitudeGroupJson; false (with `problem`) on a
// reference to an unknown or later group.
bool resolveGroupParents(std::vector<AttitudeGroup> &groups, const std::vector<std::string> &parentNames,
                         std::string &problem);
// Structural checks shared by every attitude source (constellations.json, model files, legacy
// conversion). Empty return = valid.
std::string validateAttitudeGroups(const std::vector<AttitudeGroup> &groups);
nlohmann::json attitudeGroupToJson(const AttitudeGroup &g, const std::vector<AttitudeGroup> &all);

// Root group index of group `gi` (follows parents).
int attRootOf(const std::vector<AttitudeGroup> &groups, int gi);
// Body vector (of group gi's tree) → the root's TRIAD coordinates, double precision.
glm::vec3 attTriadCoords(const std::vector<AttitudeGroup> &groups, int gi, glm::vec3 v);
GpuAttGroup toGpuAttGroup(const std::vector<AttitudeGroup> &groups, int gi);

// ── CPU attitude evaluation (mirror of sat_orbit.comp's group frames) ───────────────────────
// Used for posed OBJ export now and the Phase 4 follow camera later. Hand-mirrored from GLSL:
// keep evalGroupFrames() and sat_orbit.comp's groupFrame()/type frame loop in step.
struct AttGeometry
{
    glm::dvec3 nadir{0.0, 0.0, -1.0};
    glm::dvec3 velocity{1.0, 0.0, 0.0};
    glm::dvec3 sun{0.0, 0.0, 1.0};
    glm::dvec3 siteIdeal{0.0, 0.0, -1.0}; // SunReflectGroundSite stand-in
    double tumbleAngle = 0.0;
    glm::dvec3 tumbleAxis{0.0, 0.0, 1.0};
    double flareTiltRad = 0.0;
};
// Rigid transform per group: posed = R * rest + t (rest = the model's body frame with every
// joint at 0; world = the frame AttGeometry's directions are given in).
struct GroupPose
{
    glm::dmat3 R{1.0};
    glm::dvec3 t{0.0};
};
std::vector<GroupPose> evalGroupPoses(const std::vector<AttitudeGroup> &groups, const AttGeometry &geo,
                                      bool jointsAtZero);

// ── Geometry + materials ──────────────────────────────────────────────────────────────────────
struct SatMaterial
{
    std::string name;
    float diffuseAlbedo = 0.1f; // Lambertian albedo ρd
    float specularF0 = 0.04f;   // Fresnel reflectance at normal incidence (Schlick)
    float roughness = 0.2f;     // GGX α
    glm::vec3 color{1.0f};      // visual tint (sprite colour, OBJ export)
};
// Built-in presets — INITIAL ESTIMATES, calibrated against reference satellites in Phase 3c.
const std::vector<SatMaterial> &satMaterialPresets();

enum class PrimitiveKind : uint32_t
{
    Plane,    // size [w, h] in local XY, front normal +Z
    Box,      // size [x, y, z]
    Cylinder, // radius, height along local Z (centred), optional caps
    Cone,     // radius_bottom (z = -h/2), radius_top (z = +h/2), height; optional caps
    Sphere,   // radius
};

struct SatComponent
{
    std::string name;
    PrimitiveKind prim = PrimitiveKind::Plane;
    glm::vec3 size{1.0f};       // plane: (w, h, -); box: (x, y, z)
    float radius = 1.0f;        // cylinder, sphere; cone: bottom radius
    float radiusTop = 0.0f;     // cone
    float height = 1.0f;        // cylinder, cone
    bool caps = true;           // cylinder/cone end caps
    bool singleSided = false;   // plane: omit the back face
    glm::vec3 position{0.0f};   // relative to the group's hinge (root: the body origin), metres
    glm::quat rotation{1.0f, 0.0f, 0.0f, 0.0f};
    int material = 0;           // index into SatModel::materials
    int backMaterial = -1;      // plane back face; -1 = same as `material`
    int group = 0;              // index into SatModel::groups
};

struct SatModel
{
    std::string id;   // file stem
    std::string name; // display name
    std::vector<AttitudeGroup> groups;
    std::vector<SatMaterial> materials;
    std::vector<SatComponent> components;
};

// Loads data/satellite_models/<id>.json. Returns false with `err` on any structural problem;
// non-fatal issues go to `warn`.
bool loadSatModel(const std::string &path, const std::string &id, SatModel &out, std::string &err,
                  std::vector<std::string> &warn);

// ── Tessellation ──────────────────────────────────────────────────────────────────────────────
// One triangle of the model in REST pose (every joint at 0), body frame of its tree, metres.
struct SatTri
{
    glm::dvec3 p[3];
    glm::dvec3 n; // unit face normal (outward)
    double area;
    // Intrinsic normal spread (GGX α² units) of the SMOOTH surface this flat facet stands in for:
    // 0 for planes/boxes, (Δφ)²/12 for a revolved side facet spanning Δφ, Ω/2π for a sphere facet
    // of solid angle Ω. Without it a faceted cylinder would glint as N tiny flat mirrors instead of
    // one continuous band. Isotropic — slightly over-blurs a cylinder along its axis.
    double spread2 = 0.0;
    int group;
    int material;
    int component;
};
std::vector<SatTri> tessellateSatModel(const SatModel &m);

// ── Baked reflectance: facet lobes ────────────────────────────────────────────────────────────
// Radiant intensity per unit solar irradiance (m²/sr-ish; I/r² is the flux ratio to the sun's):
//   I = Σ_k  ρd/π·diffArea·(n·s)₊(n·o)₊  +  area·D_GGX(n·h; α_eff)·F(s·h)·G/4
// GPU form, 48 bytes std430. Must match SatLobe in sat_orbit.comp.
struct GpuSatLobe
{
    glm::vec3 normalT; // lobe normal, root triad coordinates of its group's tree
    uint32_t group;

    float area;     // m² — specular term
    float diffArea; // m² — diffuse term (area × mean resultant length of the merged normals)
    float albedoD;  // ρd
    float f0;       // Schlick F0

    float alpha2Mat; // α_material² + α_spread² (sun / Earth source size added in the shader)
    uint32_t visLayer; // Phase 3b self-shadowing map layer; 0xFFFFFFFF = unoccluded
    float pad0, pad1;
};
static_assert(sizeof(GpuSatLobe) == 48, "GpuSatLobe layout mismatch");

// Sun's angular radius mapped to a GGX half-vector α (the solar disk convolved into every lobe —
// this is what gives a flat mirror its physically correct peak, replacing mirrorBoost).
static constexpr float kSunAlpha = 0.0023f;

struct SatLobeBakeStats
{
    int triangles = 0;
    int exactLobes = 0;      // lobes after merging identical (group, material, normal) faces
    int lobes = 0;           // final count after the budget merge
    // Validator |Δmag| over configurations within 5 mag of the model's TYPICAL brightness (median
    // over random sun/observer pairs) — dimmer, grazing configurations are where a merged lobe's
    // cosine clipping dominates, and they are far below anything visible.
    double maxErrMag = 0.0;
    double p95ErrMag = 0.0;
};

// Merges triangles into ≤ budget lobes (exact merge first, then angular agglomeration within the
// same group + material), expressed in root triad coordinates.
std::vector<GpuSatLobe> bakeSatLobes(const SatModel &m, const std::vector<SatTri> &tris, int budget,
                                     SatLobeBakeStats &stats);

// Brute-force check of the baked lobes against per-triangle evaluation over random (sun,
// observer) body-frame direction pairs. Fills stats.maxErrMag / p95ErrMag.
void validateSatLobes(const SatModel &m, const std::vector<SatTri> &tris,
                      const std::vector<GpuSatLobe> &lobes, SatLobeBakeStats &stats);

// Radiant intensity (per unit irradiance) of the lobes, with the model in its REST pose: `s` (toward
// the light) and `o` (toward the observer) are rest-body-frame unit vectors. sourceAlpha2 is the
// light source's size term (kSunAlpha² for the sun). Used by the validator and, in 3c, the
// standard-magnitude report.
double evalSatLobes(const std::vector<GpuSatLobe> &lobes, const std::vector<AttitudeGroup> &groups,
                    glm::dvec3 s, glm::dvec3 o, double sourceAlpha2);

// Writes <dir>/<id>_rest.obj and <id>_sunlit.obj (+ one shared .mtl), components as OBJ groups,
// coloured by material. The sunlit pose uses a fixed illustrative geometry (see the .cpp).
bool writeSatModelObj(const SatModel &m, const std::vector<SatTri> &tris, const std::string &dir);
