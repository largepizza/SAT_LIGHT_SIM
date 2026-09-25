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

// ── Earthshine ────────────────────────────────────────────────────────────────────────────────
void earthshineExact(double rOverD, double cosSunZenith, double &irradiance, double &tiltRad, int n, double *sh)
{
    // Units of Earth radii; satellite on +z at r, Sun in the x-z plane at zenith angle Z (as seen from
    // the sub-satellite point). A ground point at colatitude λ (from the sub-satellite point) and
    // azimuth φ: Sun cosine μ0 = a + b·cos φ with a = cos λ cos Z, b = sin λ sin Z; distance d and
    // emission cosine μ depend on λ only. Integrating over φ where μ0 > 0 (|φ| < φ0):
    //   ∫ μ0 dφ = 2(a φ0 + b sin φ0),  ∫ μ0 cos φ dφ = 2a sin φ0 + b(φ0 + sin φ0 cos φ0),
    //   ∫ μ0 cos kφ dφ = 2a sin kφ0 / k + b(sin (k−1)φ0 / (k−1) + sin (k+1)φ0 / (k+1)),  k ≥ 2.
    // As seen from the satellite a ring point's direction has nadir cosine cz = (r − cos λ)/d and
    // sine sz = sin λ / d, at azimuth φ from the Sun's side; the radiance moments over the cap then
    // give the order-4 SH fit of the irradiance function (cosine-convolved: A0 = π, A1 = 2π/3,
    // A2 = π/4, A3 = 0, A4 = −π/24), in the polynomial form SatEarthLight::diffuse() evaluates.
    irradiance = 0.0;
    tiltRad = 0.0;
    if (sh)
        for (int k = 0; k < kEarthShTerms; ++k)
            sh[k] = 0.0;
    rOverD = std::clamp(rOverD, 1e-6, 1.0 - 1e-9);
    const double r = 1.0 / rOverD, lam0 = std::acos(rOverD);
    const double cosZ = std::clamp(cosSunZenith, -1.0, 1.0), sinZ = std::sqrt(1.0 - cosZ * cosZ);
    const double dl = lam0 / n;
    double ez = 0.0, ex = 0.0; // toward Earth's centre; toward the Sun's side
    double m[kEarthShTerms] = {}; // ∫L·P_k over the cap (the polynomials of SatEarthLight::sh)
    for (int i = 0; i < n; ++i)
    {
        const double lam = (i + 0.5) * dl, cl = std::cos(lam), sl = std::sin(lam);
        const double a = cl * cosZ, b = sl * sinZ;
        double phi0;
        if (b < 1e-15)
        {
            if (a <= 0.0)
                continue;
            phi0 = kPi;
        }
        else
        {
            const double c = -a / b;
            if (c >= 1.0)
                continue;
            phi0 = c <= -1.0 ? kPi : std::acos(c);
        }
        const double i0 = 2.0 * (a * phi0 + b * std::sin(phi0));
        const double i1 = 2.0 * a * std::sin(phi0) + b * (phi0 + std::sin(phi0) * std::cos(phi0));
        const double d2 = 1.0 + r * r - 2.0 * r * cl, d = std::sqrt(d2);
        const double mu = (r * cl - 1.0) / d; // emission cosine at the ground
        if (mu <= 0.0)
            continue;
        // (albedo/π)·μ0 radiance × dΩ = sin λ dλ dφ · μ / d², times the unit direction to the point.
        const double w = kEarthAlbedo / kPi * sl * mu / d2 * dl;
        const double cz = (r - cl) / d, sz = sl / d;
        ez += w * i0 * cz;
        ex += w * i1 * sz;
        if (sh)
        {
            auto ik = [&](int k) { // ∫ μ0 cos kφ dφ, k ≥ 2
                return 2.0 * a * std::sin(k * phi0) / k +
                       b * (std::sin((k - 1) * phi0) / (k - 1) + std::sin((k + 1) * phi0) / (k + 1));
            };
            const double i2 = ik(2), i3 = ik(3), i4 = ik(4);
            const double c2 = cz * cz, s2 = sz * sz;
            m[0] += w * i0;
            m[3] += w * i0 * (3.0 * c2 - 1.0);
            m[4] += w * i1 * sz * cz;
            m[5] += w * i2 * s2;
            m[6] += w * i0 * (35.0 * c2 * c2 - 30.0 * c2 + 3.0);
            m[7] += w * i1 * sz * cz * (7.0 * c2 - 3.0);
            m[8] += w * i2 * s2 * (7.0 * c2 - 1.0);
            m[9] += w * i3 * s2 * sz * cz;
            m[10] += w * i4 * s2 * s2;
        }
    }
    irradiance = std::hypot(ez, ex);
    tiltRad = irradiance > 0.0 ? std::atan2(ex, ez) : 0.0;
    if (sh)
    {
        // E(n) = Σ A_l L_lm Y_lm(n) with the real SH constants folded in (A_l·c_lm²): π/(4π) = 1/4,
        // (2π/3)·3/(4π) = 1/2, then for l = 2 5/64, 15/16, 15/64 and for l = 4 (A4 = −π/24)
        // −9/6144, −45/768, −45/1536, −315/768, −315/6144.
        sh[0] = 0.25 * m[0];
        sh[1] = 0.5 * ez;
        sh[2] = 0.5 * ex;
        sh[3] = 5.0 / 64.0 * m[3];
        sh[4] = 15.0 / 16.0 * m[4];
        sh[5] = 15.0 / 64.0 * m[5];
        sh[6] = -9.0 / 6144.0 * m[6];
        sh[7] = -45.0 / 768.0 * m[7];
        sh[8] = -45.0 / 1536.0 * m[8];
        sh[9] = -315.0 / 768.0 * m[9];
        sh[10] = -315.0 / 6144.0 * m[10];
    }
}

namespace
{
struct EarthLutTables
{
    std::vector<glm::vec2> vec;              // (ln irradiance, tilt)
    std::vector<std::array<float, kEarthShTerms>> sh; // SH coefficients / irradiance
};
const EarthLutTables &earthLutTables()
{
    static const EarthLutTables tables = [] {
        EarthLutTables t;
        t.vec.resize((size_t)kEarthLutLambda * kEarthLutCos);
        t.sh.resize(t.vec.size());
        for (int li = 0; li < kEarthLutLambda; ++li)
        {
            const double lamDeg =
                kEarthLutLambdaMinDeg + (kEarthLutLambdaMaxDeg - kEarthLutLambdaMinDeg) * li / (kEarthLutLambda - 1);
            const double rOverD = std::cos(lamDeg * kPi / 180.0);
            for (int ci = 0; ci < kEarthLutCos; ++ci)
            {
                double e, tilt, sh[kEarthShTerms];
                earthshineExact(rOverD, -1.0 + 2.0 * ci / (kEarthLutCos - 1), e, tilt, 512, sh);
                const double ln = e > 0.0 ? std::log(e) : (double)kEarthLutLnFloor;
                const size_t i = (size_t)li * kEarthLutCos + ci;
                t.vec[i] = glm::vec2((float)std::max(ln, (double)kEarthLutLnFloor), (float)tilt);
                for (int k = 0; k < kEarthShTerms; ++k)
                    t.sh[i][k] = e > 0.0 ? (float)(sh[k] / e) : 0.0f;
            }
        }
        return t;
    }();
    return tables;
}
} // namespace

const std::vector<glm::vec2> &earthshineLut()
{
    return earthLutTables().vec;
}

const std::vector<std::array<float, kEarthShTerms>> &earthshineShLut()
{
    return earthLutTables().sh;
}

void earthshineLookup(double rOverD, double cosSunZenith, double &irradiance, double &tiltRad, double *sh)
{
    // Mirror of earthshineAt() in sat_orbit.comp.
    const std::vector<glm::vec2> &t = earthshineLut();
    const double lamDeg = std::acos(std::clamp(rOverD, 0.0, 1.0)) * 180.0 / kPi;
    const double fl = std::clamp((lamDeg - kEarthLutLambdaMinDeg) / (kEarthLutLambdaMaxDeg - kEarthLutLambdaMinDeg) *
                                     (kEarthLutLambda - 1),
                                 0.0, (double)(kEarthLutLambda - 1));
    const double fc = std::clamp((cosSunZenith + 1.0) * 0.5 * (kEarthLutCos - 1), 0.0, (double)(kEarthLutCos - 1));
    const int l0 = std::min((int)fl, kEarthLutLambda - 2), c0 = std::min((int)fc, kEarthLutCos - 2);
    const double tl = fl - l0, tc = fc - c0;
    auto at = [&](int l, int c) { return glm::dvec2(t[(size_t)l * kEarthLutCos + c]); };
    const glm::dvec2 v = glm::mix(glm::mix(at(l0, c0), at(l0, c0 + 1), tc), glm::mix(at(l0 + 1, c0), at(l0 + 1, c0 + 1), tc), tl);
    irradiance = v.x <= kEarthLutLnFloor + 1.0 ? 0.0 : std::exp(v.x);
    tiltRad = v.y;
    if (sh)
    {
        const std::vector<std::array<float, kEarthShTerms>> &s = earthshineShLut();
        auto sa = [&](int l, int c, int k) { return (double)s[(size_t)l * kEarthLutCos + c][k]; };
        for (int k = 0; k < kEarthShTerms; ++k)
            sh[k] = irradiance * ((sa(l0, c0, k) * (1.0 - tc) + sa(l0, c0 + 1, k) * tc) * (1.0 - tl) +
                                   (sa(l0 + 1, c0, k) * (1.0 - tc) + sa(l0 + 1, c0 + 1, k) * tc) * tl);
    }
}

SatEarthLight earthshineLight(glm::dvec3 nadir, glm::dvec3 sun, double rOverD)
{
    SatEarthLight L;
    double tilt = 0.0;
    earthshineLookup(rOverD, glm::dot(-nadir, sun), L.E, tilt, L.sh);
    L.dir = earthshineDirection(nadir, sun, tilt);
    L.ez = nadir;
    const glm::dvec3 perp = sun - glm::dot(sun, nadir) * nadir;
    const double pl = glm::length(perp);
    // With the Sun on the axis the field is symmetric about it and any perpendicular will do.
    L.ex = pl > 1e-9 ? perp / pl : glm::normalize(glm::cross(nadir, std::abs(nadir.z) < 0.9 ? glm::dvec3(0, 0, 1)
                                                                                          : glm::dvec3(1, 0, 0)));
    const double cosRho = std::sqrt(std::max(0.0, 1.0 - rOverD * rOverD));
    const double alphaE = rOverD / (1.0 + cosRho); // tan(ρ/2)
    L.a2 = alphaE * alphaE;
    return L;
}

glm::dvec3 earthshineDirection(glm::dvec3 nadir, glm::dvec3 sun, double tiltRad)
{
    glm::dvec3 perp = sun - glm::dot(sun, nadir) * nadir;
    const double l = glm::length(perp);
    if (l < 1e-9)
        return nadir;
    return std::cos(tiltRad) * nadir + std::sin(tiltRad) * (perp / l);
}

// ── Atmospheric extinction (mirror of atmosphere.glsl) ────────────────────────────────────────
namespace
{
constexpr double kAtmHRay = 8000.0, kAtmHAer = 1200.0, kAtmFRay = 0.6;

double atmErfcx(double y)
{
    return 1.0 / (1.7724539 * (0.6564 * y + 0.3436 * std::sqrt(y * y + 2.6961)));
}

double atmColumnInf(double r, double cosChi, double H)
{
    const double x = r / H;
    const double up = std::exp(-(r - kEarthRadiusM) / H) * std::sqrt(1.5707963 * x) *
                      atmErfcx(std::sqrt(0.5 * x) * std::abs(cosChi));
    if (cosChi >= 0.0)
        return up;
    const double rt = std::max(r * std::sqrt(std::max(0.0, 1.0 - cosChi * cosChi)), kEarthRadiusM);
    return 2.0 * std::exp(-(rt - kEarthRadiusM) / H) * std::sqrt(1.5707963 * rt / H) - up;
}
} // namespace

double atmColumn(glm::dvec3 p, glm::dvec3 d, double L, double H)
{
    const double r = glm::length(p);
    const double cosP = glm::dot(p, d) / r;
    if (L <= 0.0)
        return atmColumnInf(r, cosP, H);
    const glm::dvec3 q = p + d * L;
    const double rq = glm::length(q);
    const double cosQ = glm::dot(q, d) / rq;
    const double c = (cosP >= 0.0 || cosQ > 0.0) ? atmColumnInf(r, cosP, H) - atmColumnInf(rq, cosQ, H)
                                                 : atmColumnInf(rq, -cosQ, H) - atmColumnInf(r, -cosP, H);
    return std::max(c, 0.0);
}

double atmExtinctionMag(glm::dvec3 p, glm::dvec3 d, double L, double k)
{
    return k * (kAtmFRay * atmColumn(p, d, L, kAtmHRay) + (1.0 - kAtmFRay) * atmColumn(p, d, L, kAtmHAer));
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

// ── Ground-site aim ───────────────────────────────────────────────────────────────────────────
namespace
{
uint32_t groundHashU(uint32_t x) // sat_orbit.comp hashU()
{
    x ^= x >> 16u;
    x *= 0x7feb352du;
    x ^= x >> 15u;
    x *= 0x846ca68bu;
    x ^= x >> 16u;
    return x;
}
float groundPairScore(uint32_t a, uint32_t b) // pairScore(): the GPU's float, so ties and order agree
{
    return float(groundHashU(a * 0x9E3779B9u ^ groundHashU(b))) * (1.0f / 4294967296.0f);
}
glm::dvec3 ecefToEci(const glm::dvec3 &d, double gmst)
{
    const double c = std::cos(gmst), s = std::sin(gmst);
    return {c * d.x - s * d.y, s * d.x + c * d.y, d.z};
}
glm::dvec3 idealTowards(const glm::dvec3 &sat, const glm::dvec3 &target, const glm::dvec3 &sun,
                        const glm::dvec3 &fallback)
{
    const glm::dvec3 n = sun + glm::normalize(target - sat);
    const double len = glm::length(n);
    return len > 1e-5 ? n / len : fallback;
}
int findWinner(const SatGroundSiteAim &aim, uint32_t satIndex, const glm::dvec3 &sat, double gmst,
               const glm::dvec3 &sun)
{
    int best = -1;
    float bestScore = -1.0f;
    for (size_t k = 0; k < aim.targetsEcef.size(); ++k)
    {
        const glm::dvec3 dir = ecefToEci(glm::dvec3(aim.targetsEcef[k]), gmst);
        if (glm::dot(dir, sun) >= 0.0)
            continue; // day side at this instant
        const glm::dvec3 pos = aim.targetsEcef[k].w * dir;
        if (glm::dot(dir, glm::normalize(sat - pos)) < aim.minElevSin)
            continue;
        const float score = groundPairScore(satIndex, (uint32_t)k);
        if (score > bestScore)
        {
            bestScore = score;
            best = (int)k;
        }
    }
    return best;
}
glm::dvec3 nearFallbackIdeal(const SatGroundSiteAim &aim, const glm::dvec3 &sat, double gmst, const glm::dvec3 &sun)
{
    const glm::dvec3 nadir = -glm::normalize(sat);
    int nearIdx = -1;
    double nearCos = -2.0;
    for (size_t k = 0; k < aim.targetsEcef.size(); ++k)
    {
        const glm::dvec3 dir = ecefToEci(glm::dvec3(aim.targetsEcef[k]), gmst);
        if (glm::dot(dir, sun) >= 0.0)
            continue;
        const double c = glm::dot(-nadir, dir);
        if (c > nearCos)
        {
            nearCos = c;
            nearIdx = (int)k;
        }
    }
    if (nearIdx >= 0)
        return idealTowards(sat, aim.targetsEcef[nearIdx].w * ecefToEci(glm::dvec3(aim.targetsEcef[nearIdx]), gmst),
                            sun, nadir);
    const glm::dvec3 n = sun + nadir;
    const double len = glm::length(n);
    return len > 1e-5 ? n / len : nadir;
}
} // namespace

SatGroundSiteResult satGroundSiteIdeal(const SatGroundSiteAim &aim, const SatOrbitElems &orbit, uint32_t satIndex,
                                       double tJ2000, const glm::dvec3 &sunDirEci)
{
    SatGroundSiteResult r;
    const double W = aim.lockWindowS;
    const double Wfrac = std::max(1.0, W); // the CPU's windowFrac uses max(1, W); the shader's offsets use W
    const double ratio = tJ2000 / Wfrac;
    const double windowFrac = ratio - std::floor(ratio);
    const double offset = double(groundHashU(satIndex * 0x2545F491u)) * (1.0 / 4294967296.0);
    double fracI = windowFrac + offset;
    fracI -= std::floor(fracI);
    const double toStart = -fracI * W, toPrevStart = toStart - W;
    const glm::dvec3 satCur = satOrbitStateAt(orbit, tJ2000 + toStart).posEci;
    const glm::dvec3 satPrev = satOrbitStateAt(orbit, tJ2000 + toPrevStart).posEci;
    const double gmstCur = earthRotationAngle(tJ2000 + toStart);
    const double gmstPrev = earthRotationAngle(tJ2000 + toPrevStart);
    const glm::dvec3 satNow = satOrbitStateAt(orbit, tJ2000).posEci;
    const double gmstNow = earthRotationAngle(tJ2000);
    const glm::dvec3 &sun = sunDirEci;

    const int best = findWinner(aim, satIndex, satCur, gmstCur, sun);
    const int bestPrev = findWinner(aim, satIndex, satPrev, gmstPrev, sun);
    auto site = [&](int k, double gmst) { return aim.targetsEcef[k].w * ecefToEci(glm::dvec3(aim.targetsEcef[k]), gmst); };
    const glm::dvec3 nadirCur = -glm::normalize(satCur);
    const glm::dvec3 startAim = bestPrev >= 0 ? idealTowards(satCur, site(bestPrev, gmstCur), sun, nadirCur)
                                              : nearFallbackIdeal(aim, satCur, gmstCur, sun);
    const glm::dvec3 destAtStart = best >= 0 ? idealTowards(satCur, site(best, gmstCur), sun, nadirCur)
                                             : nearFallbackIdeal(aim, satCur, gmstCur, sun);
    const glm::dvec3 live = best >= 0 ? idealTowards(satNow, site(best, gmstNow), sun, -glm::normalize(satNow))
                                      : nearFallbackIdeal(aim, satNow, gmstNow, sun);
    const double angle0 = std::acos(std::clamp(glm::dot(startAim, destAtStart), -1.0, 1.0));
    const double rate = std::max(0.01, aim.maxRateDegPerSec) * kPi / 180.0;
    const double slew = std::clamp(angle0 / rate, 0.001, W);
    const double tIn = std::clamp(fracI * W / slew, 0.0, 1.0);
    const double blend = tIn * tIn * (3.0 - 2.0 * tIn); // smoothstep(0, slew, fracI·W)
    const glm::dvec3 mixed = startAim + (live - startAim) * blend;
    const double len = glm::length(mixed);
    r.ideal = len > 1e-5 ? mixed / len : live;
    r.target = best;
    return r;
}

double satMagnitudeTo1000km(double mag, double rangeM)
{
    return mag - 5.0 * std::log10(rangeM / 1.0e6);
}

SatPhotResult evalSatPhotometry(const std::vector<AttitudeGroup> &groups, const std::vector<GpuSatLobe> &lobes,
                                const SatOrbitElems &orbit, double tJ2000, const SatPhotInputs &in,
                                const LegacyReflectance *legacy, const SatOcclusion *occ)
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

    // ── Earthshine: the lit cap of a Lambertian Earth — as modelFlux() ───────────────────────
    double rOverD = kEarthRadiusM / orbit.rSatM; // sin of Earth's angular radius
    double sinEta = std::sqrt(std::max(0.0, 1.0 - rOverD * rOverD));
    double illumFrac = std::clamp((glm::dot(-nadir, sun) + sinEta) / (2.0 * sinEta), 0.0, 1.0); // legacy only
    r.earth = earthshineLight(nadir, sun, rOverD);
    r.earthIrradiance = r.earth.E;
    r.earthDir = r.earth.dir;

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
    r.intensitySun = evalSatLobesPosed(lobes, groups, poses, sun, o, a2Sun, &r.dominantLobe, occ, true);
    r.intensityEarth = r.earth.E > 0.0 ? evalSatLobesEarthPosed(lobes, groups, poses, r.earth, o, occ) : 0.0;
    r.intensity = r.intensitySun * r.litFactor + r.intensityEarth;
    if (r.litFactor <= 0.0)
        r.dominantLobe = -1;

    r.magnitude = satMagnitudeFromIntensity(r.intensity, r.rangeM);
    r.magnitude1000 = std::isfinite(r.magnitude) ? satMagnitudeTo1000km(r.magnitude, r.rangeM) : r.magnitude;
    r.flareUnits = kFluxToFlare * r.intensity / (r.rangeM * r.rangeM);
    return r;
}
