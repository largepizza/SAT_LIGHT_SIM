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
// Extra translation of a component of `group` whose joint turns it about its own pivot (rest offset
// from the group hinge — SatComponent::pivot): (R_parent − R_group)·pivot. Zero for a root group, a
// zero pivot, or joints at rest. Every consumer that poses a POSITION adds it (occlusion samples and
// occluders, the reference ray-casts, OBJ export, the mesh renderer; sat_orbit.comp mirrors it).
glm::dvec3 satPivotOffset(const std::vector<AttitudeGroup> &groups, const std::vector<GroupPose> &poses, int group,
                          const glm::dvec3 &pivot);

// ── Geometry + materials ──────────────────────────────────────────────────────────────────────
struct SatMaterial
{
    std::string name;
    float diffuseAlbedo = 0.1f; // Lambertian albedo ρd
    float specularF0 = 0.04f;   // Fresnel reflectance at normal incidence (Schlick)
    float roughness = 0.2f;     // microfacet α
    glm::vec3 color{1.0f};      // visual tint (sprite colour, OBJ export)
    // Microfacet distribution. GGX (default) has long power-law tails; Beckmann's are Gaussian —
    // the one to use when α comes from a Phong cos^n fit (α = sqrt(2/(n+2)) is the Beckmann
    // equivalence), since GGX at the same α is ~10x brighter 30 deg off the peak. After `color` so
    // the presets' positional initializers still fit.
    bool beckmann = false;
    // Procedural surface detail for the mesh renderer (Phase 4b; sat_mesh.frag). Visual only: every
    // pattern modulates albedo with an area-weighted MEAN of 1, so the photometry's scalar values stay
    // right. -1 = automatic (from the preset the material came from), else a SatSurfacePattern.
    int pattern = -1;
    std::string preset; // the preset it was built from ("" = none); drives the automatic pattern
    // Diffuse TRANSMISSION (Phase 4f — flexible arrays on a Kapton blanket, as on the ISS): the
    // area-mean fraction of the light falling on the OTHER side of this face that leaves this side
    // diffusely, V band. Photometry: T/π · diffArea · (−n·s)₊(n·o)₊ in every lobe evaluator; the
    // renderer tints it with transmissionColor (luminance-normalised on packing) and, with the solar
    // cell pattern, sends it through the gaps between cells (the cells themselves are opaque).
    float transmission = 0.0f;
    glm::vec3 transmissionColor{1.0f, 0.55f, 0.15f}; // Kapton: amber (blue absorbed)
};
enum SatSurfacePattern : int
{
    kPatternNone = 0,
    kPatternSolarCells = 1, // cell grid with lighter substrate gaps, per-cell facet jitter
    kPatternMli = 2,        // crinkled film: micro-facet normals instead of a blurred lobe
    kPatternPanelSeams = 3, // panel joints every 0.5 m
};
// The pattern a material draws with (resolving -1).
int satMaterialPattern(const SatMaterial &m);
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
    // Per-component joint pivot (JSON "pivot", same frame as `position`: relative to the group's
    // hinge). The group's joint then turns this component about the joint axis through hinge+pivot
    // instead of through the hinge: one joint, several parallel axes — the ISS's four beta gimbals
    // in one group, all at the same angle. Normals (every lobe, every magnitude) are unaffected; a
    // posed POSITION gains satPivotOffset(). Child groups only, and only groups with no children.
    glm::vec3 pivot{0.0f};
};

// Provenance of one part of a model (benchmarking M4): every group, component and model-local
// material should be covered by a `sources` entry naming where its numbers come from.
//   status "sourced"  — taken from a cited publication
//          "derived"  — computed from sourced values plus a stated constraint
//          "estimate" — no source; the reason and the plausible range belong in `value`/`note`
//          "calibrated" — fitted to a benchmark (name it, the metric and the commit in `source`);
//                         that benchmark then checks the fit, not the model — say which ones stay
//                         held out
struct SatModelSource
{
    std::string subject; // group, component or material name ("attitude" for the pointing law)
    std::string status;
    std::string value;   // the number(s) as used
    std::string source;  // citation / URL / derivation
};

struct SatModel
{
    std::string id;   // file stem
    std::string name; // display name
    std::vector<AttitudeGroup> groups;
    std::vector<SatMaterial> materials;
    std::vector<SatComponent> components;
    std::vector<SatModelSource> sources;
    std::vector<std::string> localMaterials; // materials defined in the model file (need sources)
};

// Subjects of a model that no `sources` entry covers (groups, components, model-local materials).
// Empty = every part is explained, though some may be explained as estimates.
std::vector<std::string> unexplainedModelParts(const SatModel &m);

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
    // Phase 3b occlusion (GPU form, filled by packSatOcclusionGpu; the CPU evaluators ignore them).
    uint32_t sampleFirst;  // first GpuSatLobeSample — relative to the type until upload rebases it
    uint32_t sampleCount;  // 0 = never occluded
    uint32_t occluderMask; // bit i: the type's occluder i can block this lobe (occluders 0-31)

    uint32_t distribution; // 0 = GGX, 1 = Beckmann (the majority by area of the merged faces)
    float transmission;    // Phase 4f: diffuse transmission, V band (SatMaterial::transmission), 0 = opaque
    uint32_t occluderMaskHi; // occluders 32-63 (kMaxOccluders = 64 since the ISS model)
    uint32_t pad2;
};
static_assert(sizeof(GpuSatLobe) == 64, "GpuSatLobe layout mismatch");

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
// same group + material), expressed in root triad coordinates. `lobeTris`, when given, receives the
// triangle indices each emitted lobe was built from (for the occlusion bake).
std::vector<GpuSatLobe> bakeSatLobes(const SatModel &m, const std::vector<SatTri> &tris, int budget,
                                     SatLobeBakeStats &stats, std::vector<std::vector<int>> *lobeTris = nullptr);

// ── Phase 3b: occlusion between parts (benchmarking M7) ───────────────────────────────────────
// The shadowing study showed the shadowing that matters is BETWEEN parts that move relative to each
// other (wings over a bus, a visor under antennas), so it is evaluated at runtime against the
// model's own primitives posed at the live joint angles. Each lobe keeps a few area-weighted sample
// points on its own surface; a sample counts only if its rays toward the light source and toward the
// observer both leave the satellite unblocked. Primitives stand in for their tessellation exactly
// (planes, boxes, spheres) or as a smooth surface (a capped cylinder for a cylinder's facets; a cone
// is bounded by the cylinder of its larger radius — conservative).
// Storage bound. The count used is a bake parameter PER COMPONENT: a lobe merging several components
// gets that many for each of them, up to this bound (buildSatOcclusion).
static constexpr int kMaxLobeSamples = 64;
static constexpr int kDefaultLobeSamples = 16; // per component. Self-test p95: VisorSat 0.14, ISS 0.17
static constexpr int kMaxOccluders = 64; // per model (a 64-bit mask per lobe selects the candidates)

struct SatOccluder
{
    PrimitiveKind kind = PrimitiveKind::Plane;
    int group = 0;
    glm::dvec3 pivot{0.0}; // the component's joint pivot (SatComponent::pivot)
    glm::dvec3 center{0.0};  // rest pose, root body frame
    glm::dmat3 axes{1.0};    // columns: the component's local X, Y, Z in the rest frame
    glm::dvec3 half{0.0};    // plane (w/2, h/2, 0); box half extents; cylinder/cone (r, r, h/2); sphere (r, r, r)
};

struct SatLobeSamples
{
    int count = 0;
    glm::dvec3 p[kMaxLobeSamples];      // rest pose, root body frame, on the lobe's surface
    glm::dvec3 n[kMaxLobeSamples];      // that surface's outward normal
    double w[kMaxLobeSamples] = {};     // area fractions (sum 1)
    int comp[kMaxLobeSamples] = {};     // component the sample lies on (never occludes itself)
    uint64_t occluderMask = 0;          // occluders that can ever block this lobe
};

struct SatOcclusion
{
    std::vector<SatOccluder> occluders;
    std::vector<AttitudeGroup> groups;    // the model's groups (parents, for satPivotOffset)
    std::vector<glm::dvec3> compPivot;    // SatComponent::pivot per component (sample points)
    std::vector<SatLobeSamples> lobes; // parallel to the baked lobes
    int droppedOccluders = 0;          // components beyond kMaxOccluders (reported, not modelled)
};

// Builds occluders from the model's components and `samplesPerLobe` sample points per component of
// each baked lobe (a lobe merging several components gets that many for each, up to
// kMaxLobeSamples in all, clustered within each component). A translucent lobe's candidate
// occluders include same-group ones on BOTH sides of it (it is lit from behind). A primitive never occludes its own surface: every primitive
// here is convex or flat, so that is exact — and it keeps a faceted cylinder's samples, which sit
// just inside the smooth cylinder standing in for it, from shadowing themselves.
SatOcclusion buildSatOcclusion(const SatModel &m, const std::vector<SatTri> &tris,
                               const std::vector<GpuSatLobe> &lobes, const std::vector<std::vector<int>> &lobeTris,
                               int samplesPerLobe = kDefaultLobeSamples);

// Barycentric centroids of the m² equal sub-triangles of a triangle split m ways per edge — evenly
// spread, equal-area sample points (the occlusion bake and its self-test reference use them).
std::vector<glm::dvec3> satSubTriangleCentroids(int m);

// Fraction of lobe `li` (weighted by its samples) whose rays toward `src` (if `testSource`) and
// toward `obs` are both unblocked, with every group posed by `poses`. World directions, unit.
double satLobeVisibility(const SatOcclusion &occ, int li, int lobeGroup, const std::vector<GroupPose> &poses,
                         glm::dvec3 src, bool testSource, glm::dvec3 obs);

// GPU form of SatOcclusion (sat_orbit.comp's SatOccluder / SatLobeSample; std430). Every position
// and axis is in the ROOT TRIAD coordinates of its group's tree — the same convention as
// GpuSatLobe::normalT — so the shader maps it to world with the group frame it already builds:
// world = F_group · pT + t_group.
struct GpuSatOccluder
{
    glm::vec3 centerT;
    uint32_t kind;  // PrimitiveKind
    glm::vec3 half; // as SatOccluder::half
    uint32_t group;
    glm::vec3 axisXT; // the component's local axes
    float pivotXT;    // the component's joint pivot (SatComponent::pivot), root triad coordinates —
    glm::vec3 axisYT; // spread over the three pad slots; 0 = turns about the group hinge
    float pivotYT;
    glm::vec3 axisZT;
    float pivotZT;
};
static_assert(sizeof(GpuSatOccluder) == 80, "GpuSatOccluder layout mismatch");

struct GpuSatLobeSample
{
    glm::vec3 pT;  // sample point, already nudged 1e-4 m off its own surface
    float weight;  // area fraction of the lobe
    uint32_t comp; // occluder index of the component it lies on (skipped — never self-occludes)
    uint32_t pad0, pad1, pad2;
};
static_assert(sizeof(GpuSatLobeSample) == 32, "GpuSatLobeSample layout mismatch");

struct GpuSatOcclusionPack
{
    std::vector<GpuSatOccluder> occluders;
    std::vector<GpuSatLobeSample> samples;
    glm::vec4 originT[kMaxAttitudeGroups] = {}; // each group's rest hinge point (root: 0), triad coords
};
// Converts `occ` to GPU form and writes each lobe's sample range and occluder mask into `lobes`
// (sampleFirst relative to pack.samples). With `occ` empty, every lobe is left unoccluded.
GpuSatOcclusionPack packSatOcclusionGpu(const std::vector<AttitudeGroup> &groups, const SatOcclusion &occ,
                                        std::vector<GpuSatLobe> &lobes);

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

// Same, with every group POSED (evalGroupPoses): `s` and `o` are in the poses' world frame. This is
// the lobe loop of sat_orbit.comp's modelIntensity() — lobe normal = R_group · B_root · normalT. When
// `dominant` is given it receives the index of the brightest lobe (-1 if none is lit).
double evalSatLobesPosed(const std::vector<GpuSatLobe> &lobes, const std::vector<AttitudeGroup> &groups,
                         const std::vector<GroupPose> &poses, glm::dvec3 s, glm::dvec3 o, double sourceAlpha2,
                         int *dominant = nullptr, const SatOcclusion *occ = nullptr, bool occludeSource = true);

// Intensity of one flat facet or lobe per unit irradiance — the formula every evaluator above (and
// sat_orbit.comp's lobeIntensity()) uses. `a2` includes the source-size term.
double satLobeIntensity(glm::dvec3 n, double area, double diffArea, double albedo, double f0, double a2, bool beckmann,
                        glm::dvec3 s, glm::dvec3 o, double transmission = 0.0);

// How much does self-shadowing (all groups, posed) change the model's brightness? Poses the model
// at `samples` random geometries — sun anywhere that lights the satellite, observer within the
// Earth-facing cap as seen from the ground — and compares the brute-force intensity with and
// without ray-cast occlusion toward the sun and the observer. Decision data for Phase 3b's scope.
struct SatShadowStudy
{
    int samples = 0;
    double medianDmag = 0.0, p90Dmag = 0.0, p99Dmag = 0.0, maxDmag = 0.0; // dimming from shadowing, mag
    double fracOver01 = 0.0;  // fraction of lit configurations dimmed by > 0.1 mag
    double fracOver05 = 0.0;  // ... by > 0.5 mag
    // Same, restricted to the BRIGHTER half of configurations (unshadowed intensity above the
    // median) — relative dimming of an already-faint configuration matters less than of a bright one.
    double brightP90Dmag = 0.0;
    double brightFracOver01 = 0.0, brightFracOver05 = 0.0;
};
SatShadowStudy studySatShadowing(const SatModel &m, const std::vector<SatTri> &tris, int samples);

// Writes <dir>/<id>_rest.obj and <id>_sunlit.obj (+ one shared .mtl), components as OBJ groups,
// coloured by material. The sunlit pose uses a fixed illustrative geometry (see the .cpp).
bool writeSatModelObj(const SatModel &m, const std::vector<SatTri> &tris, const std::string &dir);
