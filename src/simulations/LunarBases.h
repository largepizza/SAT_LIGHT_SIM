#pragma once
// LunarBases.h — the lunar south-pole bases' solar arrays and their GLINTS seen from Earth (2026-10-27;
// CLAUDE.md "Moon-base glints"). App-independent: the data (data/lunar_bases.json), the Moon's IAU body frame as a
// pure function of time, and the photometry of a sun-tracking array of vertical panels as a mirror.
//
// The physics: at the pole the Sun stands within ~1.5 deg of the horizon, so the arrays are vertical panels on
// vertical-axis trackers that follow the Sun's azimuth. A panel facing the Sun mirrors it back toward the Sun's
// AZIMUTH at elevation 2 lean - e_sun: the beam reaches the Earth only when the Earth stands at about the Sun's azimuth
// (the Moon near FULL, seen from the Earth) and at that elevation. Each row's own tilt (and azimuth) error spreads the
// beam; the glint is the sum over rows of a flat mirror's flux, each a lobe = the Sun's disc blurred by the panel's
// flatness: per row R(i) A_row cos(i) p(Delta) / D^2 in units of the Sun's irradiance, p the lobe's angular density.
// For one perfect panel aimed at the observer that is the hand formula m = -26.74 - 2.5 log10(R A cos i / (Omega_sun D^2)).
#include <glm/glm.hpp>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace lunar
{
constexpr double kMoonRadiusM = 1737400.0;

struct Base
{
    std::string name;      // "ILRS (China)"
    std::string shortName; // "ILRS"
    double latDeg = -89.5, lonDeg = 0.0; // selenographic (mean Earth / polar axis, east longitude)
    double elevM = 1800.0;               // above the 1737.4-km datum
    double horizonDipDeg = 1.0;          // the local horizon's depression (an elevated rim sees below the horizontal)
    double areaM2 = 1.0e6;               // the array's panel area at full build-out
    int rows = 400;                      // rows (trackers) it is made of at full build-out
    double rowTiltSigmaDeg = 0.6;        // each row's tilt error (hashed, Gaussian)
    double rowAzSigmaDeg = 0.25;         // each row's tracking azimuth error
    double panelFlatnessDeg = 0.05;      // a panel's slope error within the row (blurs the mirrored Sun)
    double leanDeg = 0.0;                // the panels' lean back from vertical (normal raised)
    double f0 = 0.04;                    // the cover glass' reflectance at normal incidence (Schlick)
    double onlineT = 0.0, fullT = 0.0;   // J2000 s: the first rows stand; the array is complete
    uint32_t seed = 1;
    glm::vec3 tint{1.0f, 0.97f, 0.92f};  // the glint's colour (the Sun's, through the glass)
};

// The Moon's body frame in ECI at t (the IAU WGCCRE 2009 rotation model, as updatePositions draws the disc):
// P the north pole, X 0 deg longitude, Y 90 deg east.
void moonBodyFrame(double tJ2000, glm::dvec3 &P, glm::dvec3 &X, glm::dvec3 &Y);
// A selenographic place as a unit vector of the body frame (ECI).
glm::dvec3 bodyDir(const Base &b, const glm::dvec3 &P, const glm::dvec3 &X, const glm::dvec3 &Y);

// Loads the bases (false + why on failure). Dates "YYYY-MM-DD[THH:MM]" UTC.
bool loadBases(const std::string &path, std::vector<Base> &out, std::string &err);

// The array's share built at t (0 before online, 1 from fullT) and the rows standing.
double builtShare(const Base &b, double t);

struct Geometry
{
    glm::dvec3 baseEci{0.0};   // the base (m, ECI)
    glm::dvec3 up{0.0, 0.0, 1.0};
    glm::dvec3 sunDir{0.0};    // from the base
    double sunDistM = 1.496e11;
    double sunElevDeg = 0.0;
    double sunVisible = 1.0;   // the share of the Sun's disc over the local horizon
    double sunAngR = 0.00465;  // rad
};
Geometry geometryAt(const Base &b, double t, const glm::dvec3 &moonGeoEci, const glm::dvec3 &P, const glm::dvec3 &X,
                    const glm::dvec3 &Y);

struct Glint
{
    double ratio = 0.0;        // irradiance at the observer / the Sun's (above the Earth's air)
    double mag = 99.0;         // apparent magnitude above the air (99 = none)
    double rowsLit = 0.0;      // rows contributing > 1% of the brightest
    double obsElevDeg = 0.0;   // the observer's elevation in the base's sky
    double phaseDeg = 0.0;     // the angle Sun - base - observer
    double beamOffDeg = 0.0;   // the mean beam's angle from the observer
    bool visible = false;      // the observer is above the base's horizon and the Sun is up there
};
// The glint seen from obsEci by summing the rows (jitterScale x the rows' tilt / azimuth errors, lean added).
Glint glintRows(const Base &b, const Geometry &g, double t, const glm::dvec3 &obsEci, double jitterScale = 1.0,
                double leanAddDeg = 0.0);
// The same with the rows' scatter taken as one Gaussian beam (the search's fast form).
Glint glintAggregate(const Base &b, const Geometry &g, double t, const glm::dvec3 &obsEci, double jitterScale = 1.0,
                     double leanAddDeg = 0.0);
// The mean beam's direction (the reflection of the Sun about the rows' mean normal).
glm::dvec3 meanBeam(const Base &b, const Geometry &g, double leanAddDeg = 0.0);

// The best place on Earth at t for a base's glint: a grid of Earth-fixed places with the Moon >= minMoonAltDeg up and
// the Sun <= maxSunAltDeg, the brightest by the aggregate beam. False when no such place sees it at all.
struct Site
{
    glm::dvec3 ecef{0.0, 0.0, 1.0}; // unit, Earth-fixed
    double mag = 99.0;              // the glint there (aggregate, above the air)
    double moonAltDeg = 0.0, sunAltDeg = 0.0;
};
// useRows: the refinement measures the rows (the real sum, with its hashed scatter) instead of the aggregate.
bool bestSite(const Base &b, double t, double jitterScale, double leanAddDeg, Site &out, double minMoonAltDeg = 10.0,
              double maxSunAltDeg = -6.0, bool useRows = false);
// The glint's windows in [t0, t1]: stretches where the best place on Earth sees it brighter than magLimit (aggregate),
// found in 30-min steps (a pre-test on the beam's angle from the Earth) refined to 5 min; the peak re-measured by rows.
struct Window
{
    int base = 0;
    double t0 = 0.0, t1 = 0.0, tPeak = 0.0;
    double peakMag = 99.0;      // at the peak, from its best place (rows, above the air)
    Site site;                  // the best place at the peak
    double moonPhaseDeg = 0.0;  // the Moon's phase angle (geocentric) at the peak: 0 = full
};
// cancelled(ctx) is polled between steps (a worker's restart).
void findWindows(const std::vector<Base> &bases, double t0, double t1, double magLimit, double jitterScale,
                 double leanAddDeg, bool (*cancelled)(const void *), const void *cancelCtx, std::vector<Window> &out);

inline double magFromRatio(double ratio, double sunDistM)
{
    if (ratio <= 0.0) return 99.0;
    return -26.74 + 5.0 * std::log10(sunDistM / 1.495978707e11) - 2.5 * std::log10(ratio);
}
} // namespace lunar
