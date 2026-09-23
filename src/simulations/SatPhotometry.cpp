// SatPhotometry — see SatPhotometry.h for the overview.
#include "SatPhotometry.h"

#include <algorithm>
#include <cmath>

using namespace satphot;

// ── Time and frame helpers ────────────────────────────────────────────────────────────────────
double earthRotationAngle(double tJ2000)
{
    return std::fmod(kOmegaEarth * tJ2000, 2.0 * kPi);
}

glm::dvec3 sunDirEciAt(double tJ2000)
{
    // Low-accuracy Astronomical Almanac solar position (~0.01°), ecliptic → equatorial.
    const double deg = kPi / 180.0;
    double d = tJ2000 / 86400.0;
    double L = std::fmod(280.46 + 0.9856474 * d, 360.0);
    double g = std::fmod(357.528 + 0.9856003 * d, 360.0) * deg;
    double lambda = (L + 1.915 * std::sin(g) + 0.020 * std::sin(2.0 * g)) * deg;
    double eps = (23.439 - 0.0000004 * d) * deg;
    return glm::normalize(glm::dvec3(std::cos(lambda), std::sin(lambda) * std::cos(eps),
                                     std::sin(lambda) * std::sin(eps)));
}

glm::dvec3 observerEciAt(glm::dvec3 obsDirEcef, double radiusM, double tJ2000)
{
    // Earth-fixed → ECI is a rotation about Z by the Earth rotation angle.
    glm::dvec3 d = glm::normalize(obsDirEcef);
    double th = earthRotationAngle(tJ2000);
    double c = std::cos(th), s = std::sin(th);
    return radiusM * glm::dvec3(c * d.x - s * d.y, s * d.x + c * d.y, d.z);
}

// ── Orbit (mirror of sat_orbit.comp's satEciAt / processSatellite orbit block) ────────────────
SatOrbitState satOrbitStateAt(const SatOrbitElems &e, double tJ2000)
{
    const double twoPi = 2.0 * kPi;
    double n = e.meanMot > 0.0 ? e.meanMot : std::sqrt(kGM / (e.rSatM * e.rSatM * e.rSatM));
    double u = std::fmod(e.u0 + n * tJ2000, twoPi);
    double raan = e.sso ? std::fmod(e.raan + kSSOPrecRate * (tJ2000 - e.raanAnchorT), twoPi) : e.raan;
    double cosU = std::cos(u), sinU = std::sin(u);
    double cosR = std::cos(raan), sinR = std::sin(raan);
    double cosI = std::cos(e.incl), sinI = std::sin(e.incl);

    SatOrbitState st;
    st.posEci = e.rSatM * glm::dvec3(cosR * cosU - sinR * sinU * cosI,
                                     sinR * cosU + cosR * sinU * cosI,
                                     sinU * sinI);
    st.nadir = -glm::normalize(st.posEci);
    // d(position)/du — unit length for a circular orbit.
    st.velocity = glm::dvec3(-sinU * cosR - cosU * cosI * sinR,
                             -sinU * sinR + cosU * cosI * cosR,
                             cosU * sinI);
    st.tumbleAngle = e.tumblePhase + e.tumbleRate * tJ2000;
    return st;
}

// ── Photometry ────────────────────────────────────────────────────────────────────────────────
namespace
{
double smoothstep(double e0, double e1, double x)
{
    double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double angleBetween(glm::dvec3 a, glm::dvec3 b)
{
    return std::acos(std::clamp(glm::dot(a, b), -1.0, 1.0));
}

// Mirror of sat_orbit.comp's typeUsesGroundSite().
bool usesGroundSite(const std::vector<AttitudeGroup> &groups)
{
    for (const AttitudeGroup &g : groups)
    {
        if (g.law != AttLaw::TwoVector)
            continue;
        if (g.primaryTarget == AttTarget::SunReflectGroundSite || g.secondaryTarget == AttTarget::SunReflectGroundSite)
            return true;
        if ((g.jointMode == JointMode::Track || g.jointMode == JointMode::EdgeOn) &&
            g.jointTarget == AttTarget::SunReflectGroundSite)
            return true;
    }
    return false;
}

// Mirror of sat_orbit.comp's legacyFlux(), verbatim in structure, before brightnessScale.
double legacyFlareUnits(const LegacyReflectance &L, glm::dvec3 n0, glm::dvec3 n1, glm::dvec3 sun, glm::dvec3 nadir,
                        glm::dvec3 o, double litFactor, double earthIrr, double distFactor, double mirrorBoost)
{
    auto lobe = [&](glm::dvec3 n, double specExp, double &irr, glm::dvec3 &refl) {
        irr = std::abs(glm::dot(sun, n));
        glm::dvec3 nf = glm::dot(sun, n) >= 0.0 ? n : -n;
        refl = glm::reflect(-sun, nf);
        return irr * (specExp < 0.01 ? std::max(0.0, glm::dot(sun, o))
                                     : std::pow(std::clamp(glm::dot(refl, o), 0.0, 1.0), specExp));
    };
    double irr0, irr1;
    glm::dvec3 refl0, refl1;
    double spec0 = lobe(n0, L.specExp0, irr0, refl0);
    double spec1 = lobe(n1, L.specExp1, irr1, refl1);

    // Mirror spike on the primary surface.
    double mirrorExp = std::max(L.specExp0 * mirrorBoost, 8000.0);
    double mirrorDot = std::clamp(glm::dot(refl0, o), 0.0, 1.0);
    spec0 += irr0 * std::pow(mirrorDot, mirrorExp) * mirrorBoost * L.mirrorFrac;

    // Earthshine from nadir (not eclipse-gated).
    double irrE0 = std::max(0.0, glm::dot(nadir, n0));
    glm::dvec3 nE0 = glm::dot(nadir, n0) >= 0.0 ? n0 : -n0;
    glm::dvec3 reflE0 = glm::reflect(-nadir, nE0);
    double specE0 = irrE0 * (L.specExp0 < 0.01 ? std::max(0.0, glm::dot(nadir, o))
                                                : std::pow(std::max(0.0, glm::dot(reflE0, o)), L.specExp0));
    double specE0b = std::max(0.0, glm::dot(nadir, -n0)) * L.diffuse;
    double specE1 = std::max(0.0, glm::dot(nadir, n1)) * L.w1 * L.diffuse;
    double earthFlare = (specE0 + specE0b + specE1) * earthIrr * distFactor * L.crossSection;

    double solarFlare = (spec0 + spec1 * L.w1 + L.diffuse) * litFactor * distFactor * L.crossSection;
    return solarFlare + earthFlare;
}
} // namespace

double satMagnitudeFromIntensity(double intensity, double rangeM)
{
    if (!(intensity > 0.0) || !(rangeM > 0.0))
        return std::numeric_limits<double>::infinity();
    return kSunMagV - 2.5 * std::log10(intensity / (rangeM * rangeM));
}

double satMagnitudeTo1000km(double mag, double rangeM)
{
    return mag - 5.0 * std::log10(rangeM / 1.0e6);
}

SatPhotResult evalSatPhotometry(const std::vector<AttitudeGroup> &groups, const std::vector<GpuSatLobe> &lobes,
                                const SatOrbitElems &orbit, double tJ2000, const SatPhotInputs &in,
                                const LegacyReflectance *legacy)
{
    SatPhotResult r;
    r.orbit = satOrbitStateAt(orbit, tJ2000);
    const glm::dvec3 sat = r.orbit.posEci;
    const glm::dvec3 sun = glm::normalize(in.sunDirEci);
    const glm::dvec3 nadir = r.orbit.nadir;

    // ── Viewing geometry ────────────────────────────────────────────────────────────────────
    glm::dvec3 rel = sat - in.obsEci;
    r.rangeM = glm::length(rel);
    glm::dvec3 o = -rel / r.rangeM; // satellite → observer
    r.sinElevation = glm::dot(rel, glm::normalize(in.obsEci)) / r.rangeM;
    r.phaseAngleRad = angleBetween(sun, o);
    // Sun's mirror image in a plate facing nadir: reflect the incoming ray (−sun) about nadir.
    r.offSpecularRad = angleBetween(glm::reflect(-sun, nadir), o);

    // ── Soft Earth shadow (umbra/penumbra cones) — as processSatellite() ─────────────────────
    const double tanUmbra = (kSunRadiusM - kEarthRadiusM) / kAuM;
    const double tanPenumbra = (kSunRadiusM + kEarthRadiusM) / kAuM;
    double proj = glm::dot(sat, -sun);
    double perpLen = glm::length(sat - proj * -sun);
    double dShadow = std::max(proj, 0.0);
    double rUmbra = std::max(0.0, kEarthRadiusM - dShadow * tanUmbra);
    double rPenumbra = kEarthRadiusM + dShadow * tanPenumbra;
    r.litFactor = proj > 0.0 ? smoothstep(rUmbra, rPenumbra, perpLen) : 1.0;

    // ── Earthshine: lit Earth as a Lambertian disc — as modelFlux() ──────────────────────────
    double rOverD = kEarthRadiusM / orbit.rSatM; // sin of Earth's angular radius
    double sinEta = std::sqrt(std::max(0.0, 1.0 - rOverD * rOverD));
    double illumFrac = std::clamp((glm::dot(-nadir, sun) + sinEta) / (2.0 * sinEta), 0.0, 1.0);
    r.earthIrradiance = kEarthAlbedo * rOverD * rOverD * illumFrac;
    double alphaE = rOverD / (1.0 + sinEta); // tan(ρ/2); sinEta = cos ρ

    // ── Attitude ────────────────────────────────────────────────────────────────────────────
    if (usesGroundSite(groups) && !in.hasSiteIdeal)
    {
        r.supported = false;
        return r;
    }
    AttGeometry geo;
    geo.nadir = nadir;
    geo.velocity = r.orbit.velocity;
    geo.sun = sun;
    geo.siteIdeal = in.hasSiteIdeal ? in.siteIdeal : nadir;
    geo.tumbleAngle = r.orbit.tumbleAngle;
    geo.tumbleAxis = orbit.tumbleAxis;
    geo.flareTiltRad = in.flareTiltRad;
    std::vector<GroupPose> poses = evalGroupPoses(groups, geo, false);

    // ── Legacy two-surface model (GPU parity only) ──────────────────────────────────────────
    if (lobes.empty() && legacy)
    {
        auto surfaceNormal = [&](int g, glm::dvec3 n) {
            g = std::clamp(g, 0, (int)poses.size() - 1);
            return glm::normalize(poses[g].R * glm::normalize(n));
        };
        const double refRange = 500000.0; // REF_RANGE
        double distFactor = refRange / std::max(r.rangeM, refRange);
        distFactor *= distFactor;
        // Legacy earthshine: albedo × Earth's share of the upward hemisphere × lit fraction.
        double legacyEarthIrr = kEarthAlbedo * (1.0 - rOverD) * 0.5 * illumFrac;
        r.legacy = true;
        r.flareUnits = legacyFlareUnits(*legacy, surfaceNormal(legacy->surfGroup0, legacy->surfNormal0),
                                        surfaceNormal(legacy->surfGroup1, legacy->surfNormal1), sun, nadir, o,
                                        r.litFactor, legacyEarthIrr, distFactor, in.mirrorBoost);
        return r;
    }

    // ── Lobes ───────────────────────────────────────────────────────────────────────────────
    const double a2Sun = (double)kSunAlpha * kSunAlpha;
    r.intensitySun = evalSatLobesPosed(lobes, groups, poses, sun, o, a2Sun, &r.dominantLobe);
    r.intensityEarth = r.earthIrradiance > 0.0
                           ? evalSatLobesPosed(lobes, groups, poses, nadir, o, alphaE * alphaE)
                           : 0.0;
    r.intensity = r.intensitySun * r.litFactor + r.intensityEarth * r.earthIrradiance;
    if (r.litFactor <= 0.0)
        r.dominantLobe = -1;

    r.magnitude = satMagnitudeFromIntensity(r.intensity, r.rangeM);
    r.magnitude1000 = std::isfinite(r.magnitude) ? satMagnitudeTo1000km(r.magnitude, r.rangeM) : r.magnitude;
    r.flareUnits = kFluxToFlare * r.intensity / (r.rangeM * r.rangeM);
    return r;
}
