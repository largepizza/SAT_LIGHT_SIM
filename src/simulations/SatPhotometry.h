#pragma once
// ── SatPhotometry — CPU double-precision photometric evaluator ────────────────────────────────
// Benchmarking milestone M1 (design page: "Satellite Brightness Benchmarking"). One satellite at one
// instant: orbit → attitude → lobes → physical magnitude. It is the CPU mirror of sat_orbit.comp's
// per-satellite chain for geometry-model types (processSatellite() → modelFlux()), and the single
// source of truth for everything that must be MEASURED rather than rendered: SatBench, the in-app
// photometry readout and exports. The GPU-parity readout (M2) checks the two agree.
//
// Hand-mirrored from GLSL, like evalGroupPoses(): keep satOrbitStateAt() in step with satEciAt()/
// processSatellite()'s orbit block, and evalSatPhotometry() with modelFlux() and the shadow /
// earthshine terms above it.
//
// Scope: geometry-model types, plus — for the GPU-parity check (M2) only — a mirror of the legacy
// two-surface model's raw flux (legacyFlux()), whose output is in tuned display units, not a
// physical magnitude. Types whose attitude aims at a ground site (TargetedReflector) need the
// lock-window search's siteIdeal, which the caller must supply.

#include "SatModel.h"

#include <glm/glm.hpp>
#include <array>
#include <limits>
#include <vector>

namespace satphot
{
// Must equal the constants SatelliteSim.cpp and sat_orbit.comp use (SatelliteSim.cpp static_asserts
// its own copies against these).
constexpr double kPi = 3.14159265358979323846;
constexpr double kEarthRadiusM = 6371000.0;
constexpr double kGM = 3.986004418e14;          // m³/s²
constexpr double kOmegaEarth = 7.2921150e-5;    // rad/s, sidereal
constexpr double kYearSec = 365.25 * 86400.0;
constexpr double kSSOPrecRate = 2.0 * kPi / kYearSec; // rad/s
constexpr double kSunRadiusM = 6.96e8;
constexpr double kAuM = 1.496e11;
constexpr double kEarthAlbedo = 0.3;
constexpr double kSunMagV = -26.74;             // apparent V magnitude of the Sun at 1 AU
// effectFlare per unit (I/r²): the display anchor effectFlare 0.008 ↔ mag 6 (K_FLUX in sat_orbit.comp).
constexpr double kFluxToFlare = 9.979e10;

// Earthshine table axes — must equal EARTH_LUT_* in sat_orbit.comp. Rows: the visible cap's
// half-angle λ0 = acos(R/r) (10° ≈ 70 km … 85° ≈ 67,000 km; clamped outside); columns: cos of the
// Sun's zenith angle at the sub-satellite point, -1..1.
constexpr int kEarthLutLambda = 64;
constexpr int kEarthLutCos = 128;
constexpr double kEarthLutLambdaMinDeg = 10.0;
constexpr double kEarthLutLambdaMaxDeg = 85.0;
constexpr float kEarthLutLnFloor = -60.0f; // ln irradiance stored where no lit ground is visible
} // namespace satphot

// ── Earthshine ────────────────────────────────────────────────────────────────────────────────
// Sunlight reflected by the visible cap of a Lambertian Earth (albedo kEarthAlbedo), as seen from a
// satellite at r = R/rOverD: the vector irradiance Σ L·dΩ·(direction), per unit solar irradiance.
// Each ground point's radiance carries its own Sun cosine, so near the terminator — where every
// twilight observation is made — the lit crescent is dim and off to the Sun's side. `irradiance` is
// what a plate facing `tiltRad` from nadir toward the Sun receives. The integral around each ring of
// the cap is closed-form; the one over the cap's radius is numerical (n steps).
// `sh`, when given (kEarthShTerms values), receives the order-4 spherical-harmonic fit of the cap's
// IRRADIANCE function (SatEarthLight::sh: the plane irradiance for any normal, local frame nadir /
// Sun side), from the same ring integrals (∫μ0·cos kφ dφ, k = 0..4, closed-form).
void earthshineExact(double rOverD, double cosSunZenith, double &irradiance, double &tiltRad, int n = 512,
                     double *sh = nullptr);
// The table sat_orbit.comp reads (float, [λ row * kEarthLutCos + cos column] = (ln irradiance, tilt)),
// and its bilinear lookup exactly as the shader does it.
const std::vector<glm::vec2> &earthshineLut();
// Its companion (earthShA/B/C in sat_orbit.comp): the SH coefficients per entry as RATIOS to the
// vector irradiance, so they interpolate across the table's many decades like the ln column does.
const std::vector<std::array<float, kEarthShTerms>> &earthshineShLut();
void earthshineLookup(double rOverD, double cosSunZenith, double &irradiance, double &tiltRad, double *sh = nullptr);
// Effective source direction: nadir turned `tiltRad` toward the Sun (nadir if the Sun is on the axis).
glm::dvec3 earthshineDirection(glm::dvec3 nadir, glm::dvec3 sun, double tiltRad);
// Everything a lit surface needs from the Earth at a satellite (table lookup): vector irradiance,
// direction, the SH irradiance fit and its frame, and the Earth's angular size as α². rOverD = R/r.
SatEarthLight earthshineLight(glm::dvec3 nadir, glm::dvec3 sun, double rOverD);

// ── Atmospheric extinction along a line of sight ──────────────────────────────────────────────
// Mirror of shaders/include/atmosphere.glsl (see it for the model): molecular (8 km) + aerosol
// (1.2 km) exponential columns via the Chapman function, sharing the sea-level zenith extinction k.
// p: Earth-centred position (m), d: unit direction, L: distance to the target (<= 0: infinity).
double atmColumn(glm::dvec3 p, glm::dvec3 d, double L, double scaleHeightM);
double atmExtinctionMag(glm::dvec3 p, glm::dvec3 d, double L, double k);

// ── Time and frame helpers (shared with SatelliteSim::updatePositions) ────────────────────────
// Time is seconds since J2000 (the sim's simDayJ2000·86400 + simSecInDay).
//
// NOTE — the sim's Earth rotation angle is kOmegaEarth·t, with NO Greenwich sidereal angle at J2000
// (≈ 280.46°). Everything in the sim uses the same angle, so it is self-consistent, but it means sim
// clock time and real UTC differ by a fixed rotation (the Sun's local hour angle is off by ≈ 5.3 h).
// Statistical benchmarks sample by Sun geometry and are unaffected; replaying real timestamped
// observations must correct for it. Recorded as an open question on the design page.
double earthRotationAngle(double tJ2000);
// Low-accuracy almanac Sun direction in ECI (the formula updatePositions() has always used).
glm::dvec3 sunDirEciAt(double tJ2000);
// Observer position in ECI (m) for an Earth-fixed unit direction and distance from Earth's centre.
glm::dvec3 observerEciAt(glm::dvec3 obsDirEcef, double radiusM, double tJ2000);

// ── Orbit ─────────────────────────────────────────────────────────────────────────────────────
// A circular orbit in the sim's convention, all angles at t = 0 (J2000). Built from the sim's own
// SatOrbit record (floats promoted, so the numbers are the app's) or directly for a benchmark.
struct SatOrbitElems
{
    double raan = 0.0;       // rad; for SSO (alignTerminator) the value at raanAnchorT
    double incl = 0.0;       // rad
    double u0 = 0.0;         // argument of latitude at t = 0, rad
    double rSatM = satphot::kEarthRadiusM + 550000.0;
    double meanMot = 0.0;    // rad/s; 0 = derive from rSatM
    bool sso = false;        // RAAN precesses at kSSOPrecRate from raanAnchorT
    double raanAnchorT = 0.0;
    double tumbleRate = 0.0; // rad/s
    double tumblePhase = 0.0;
    glm::dvec3 tumbleAxis{0.0, 0.0, 1.0};
};

struct SatOrbitState
{
    glm::dvec3 posEci{0.0};  // m
    glm::dvec3 nadir{0.0, 0.0, -1.0};
    glm::dvec3 velocity{1.0, 0.0, 0.0}; // unit along-track
    double tumbleAngle = 0.0;
};
SatOrbitState satOrbitStateAt(const SatOrbitElems &e, double tJ2000);

// ── Ground-site mirror aim (TargetedReflector / SunReflectGroundSite) ─────────────────────────
// The CPU mirror of sat_orbit.comp's ground-site block, in double: each satellite picks a target
// per fixed sim-time lock window (offset per satellite by a hash of its index), the argmax of an
// integer hash over the night-side sites it sees above minElevSin at that window's START; the mirror
// eases from the previous window's aim to the live ideal at maxRateDegPerSec. A pure function of
// (sim time, satellite index), like the GPU. Keep it in step with that block, findWinner(),
// nearFallbackIdeal() and idealTowards().
struct SatGroundSiteAim
{
    std::vector<glm::dvec4> targetsEcef; // xyz = unit ECEF direction, w = ground radius (m)
    double lockWindowS = 90.0;           // reflectorLockWindowS
    double maxRateDegPerSec = 1.0;       // mirrorMaxRateDegPerSec
    double minElevSin = 0.0;             // sin(reflectorMinElevDeg)
};
struct SatGroundSiteResult
{
    glm::dvec3 ideal{0.0, 0.0, -1.0}; // mirror normal, ECI (unit)
    int target = -1;                  // this window's site, -1 = none beam-worthy (fallback aim)
};
SatGroundSiteResult satGroundSiteIdeal(const SatGroundSiteAim &aim, const SatOrbitElems &orbit, uint32_t satIndex,
                                       double tJ2000, const glm::dvec3 &sunDirEci);

// ── Photometry ────────────────────────────────────────────────────────────────────────────────
struct SatPhotInputs
{
    glm::dvec3 sunDirEci{1.0, 0.0, 0.0};
    glm::dvec3 obsEci{satphot::kEarthRadiusM, 0.0, 0.0};
    double flareTiltRad = 0.0;          // global flare-mitigation tilt (FlareMitigationTilt joints)
    bool hasSiteIdeal = false;          // SunReflectGroundSite aim, if the caller computed it
    glm::dvec3 siteIdeal{0.0};
    double mirrorBoost = 300.0;         // legacy model only (the Photometry slider)
};

// The legacy two-surface model's per-type parameters (SatelliteType's primary/secondary surfaces,
// diffuse floor, mirror fraction and cross-section). Normals are in their group's body frame.
struct LegacyReflectance
{
    int surfGroup0 = 0, surfGroup1 = 0;
    glm::dvec3 surfNormal0{0.0, 0.0, 1.0}, surfNormal1{0.0, 0.0, 1.0};
    double specExp0 = 0.0, specExp1 = 0.0, w1 = 0.0, diffuse = 0.0, mirrorFrac = 0.0;
    double crossSection = 1.0; // sqrt(area_m² / 10)
};

struct SatPhotResult
{
    bool supported = true;      // false: the type aims at a ground site and no siteIdeal was given
    SatOrbitState orbit;
    double rangeM = 0.0;
    double sinElevation = 0.0;  // above the observer's geocentric horizon (up = obsEci direction)
    double phaseAngleRad = 0.0; // angle at the satellite between Sun and observer (0 = fully lit)
    double offSpecularRad = 0.0; // angle between the observer and the Sun's mirror image in a nadir-
                                 // facing plate — provisional flare-curve definition (design page)
    double litFactor = 0.0;     // Earth-shadow factor, 1 = full sunlight
    double earthIrradiance = 0.0; // earthshine (vector) irradiance as a fraction of sunlight (earthshineLookup)
    glm::dvec3 earthDir{0.0};     // its effective source direction (earthshineDirection)
    SatEarthLight earth;          // the whole earthshine light (SH diffuse + specular source)
    double intensitySun = 0.0;  // I from the Sun per unit irradiance, before litFactor (m² sr⁻¹)
    double intensityEarth = 0.0; // I from the lit Earth, per unit SOLAR irradiance (already × earthshine)
    double intensity = 0.0;     // intensitySun·litFactor + intensityEarth
    int dominantLobe = -1;      // brightest lobe under sunlight (-1: none lit)
    // Above-atmosphere apparent magnitude at the actual range, and reduced to 1000 km. +inf when dark.
    double magnitude = std::numeric_limits<double>::infinity();
    double magnitude1000 = std::numeric_limits<double>::infinity();
    // sat_orbit.comp's raw flux before brightnessScale: K_FLUX·I/r² for a model, or the legacy
    // model's value in its own tuned units (magnitudes then stay +inf — they are not physical).
    double flareUnits = 0.0;
    bool legacy = false;
};

// Evaluates one satellite. Geometry model: `lobes` non-empty (SatelliteType::lobes, or a SatModel's
// bakeSatLobes()). Otherwise `legacy` must be given and the legacy two-surface flux is mirrored.
// `occ` (Phase 3b, buildSatOcclusion) adds occlusion between parts: sunlight needs both rays clear,
// earthshine only the observer ray (Earth is too broad a source for one shadow ray).
SatPhotResult evalSatPhotometry(const std::vector<AttitudeGroup> &groups, const std::vector<GpuSatLobe> &lobes,
                                const SatOrbitElems &orbit, double tJ2000, const SatPhotInputs &in,
                                const LegacyReflectance *legacy = nullptr, const SatOcclusion *occ = nullptr);

// Magnitude conversions (design page, "Photometric conventions").
double satMagnitudeFromIntensity(double intensity, double rangeM); // +inf when intensity <= 0
double satMagnitudeTo1000km(double mag, double rangeM);
