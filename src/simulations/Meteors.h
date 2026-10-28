#pragma once
// Meteors.h — meteor showers and the sporadic background (2026-10-27; CLAUDE.md "Meteors").
//
// App-independent: the shower table (the IMO working list's major annual showers: peak solar longitude, ZHR, the
// activity profile's slopes, population index, radiant and its drift, entry speed), the flux a ZHR stands for, and a
// GENERATOR that enumerates the meteors whose start falls in a time range near an observer. Every meteor is a pure
// function of (shower, magnitude band, Earth-fixed cell, time bin): hashed counts (Poisson) and hashed properties, so
// time warp, reverse playback and bookmarks see the same sky. SatelliteSimMeteors.cpp draws them and runs the sky
// events / harness side.
#include <glm/glm.hpp>
#include <cstdint>
#include <functional>

namespace meteors
{
struct Shower
{
    const char *id;    // harness / json name ("perseids")
    const char *name;  // "Perseids"
    const char *code;  // IAU code ("PER")
    double lam0;       // peak solar longitude (deg, J2000)
    double zhr;        // the main peak's ZHR
    double bMinus, bPlus; // activity slopes before / after the peak: ZHR x 10^(-B |dLam|)
    double zhr2, b2;   // a broad component (0 = none)
    double r;          // population index (faint end, mag 2 .. 6.5)
    double rb;         // population index of the bright end (brighter than mag 2): < r = fireball-rich
    double vKms;       // atmospheric entry speed
    double ra, dec;    // radiant at the peak (deg, J2000)
    double dRa, dDec;  // drift (deg per deg of solar longitude ~ per day)
    double radiantSpreadDeg; // the radiant's spread (sporadic: its sources' widths)
};

constexpr int kSporadic = 12;   // the last entry: the sporadic background (its radiants drawn per meteor)
int showerCount();               // the showers + the sporadic entry
const Shower &shower(int i);
int showerByName(const char *idOrName); // -1 = none

// The Sun's ecliptic longitude (deg, 0..360) from its ECI (J2000 equator) direction.
double solarLongitudeDeg(const glm::dvec3 &sunDirEci);
// The shower's ZHR at solar longitude lam (deg); the sporadic background's is its constant ZHR.
double zhrAt(int s, double lamDeg);
// The radiant (ECI unit) at solar longitude lam.
glm::dvec3 radiantEci(int s, double lamDeg);
// Meteoroids per km^2 per hour brighter than absolute magnitude +6.5 for a ZHR and population index (Koschack &
// Rendtel 1990; the IMO quotes it x 1000, per 1000 km^2).
double fluxPerKm2h(double zhr, double r);
// The cumulative magnitude distribution N(<= M) / N(<= 6.5): r below mag 2, rb above (brighter).
double magFrac(double M, double r, double rb);
// The time of the shower's peak nearest after t (s since J2000), and the activity window (ZHR >= half) round it.
double peakAfter(int s, double t, double *t0 = nullptr, double *t1 = nullptr);

// Magnitude bands (absolute magnitude, i.e. at 100 km): each with its own time bin and the longest it can be shown
// after its start (its flight + train).
constexpr int kBands = 6;
constexpr double kBandEdge[kBands + 1] = {-14.0, -8.0, -4.0, -1.0, 2.0, 4.5, 6.5};
constexpr double kBandBinS[kBands] = {120.0, 30.0, 8.0, 2.0, 2.0, 2.0};
constexpr double kBandMaxAgeS[kBands] = {240.0, 60.0, 16.0, 8.0, 8.0, 8.0};

struct Meteor
{
    int shower = 0, band = 0;
    uint64_t seed = 0;
    double tStart = 0.0;     // s since J2000
    double dur = 0.0;        // its flight (s)
    double vMs = 0.0;
    float M = 0.0f;          // peak absolute magnitude (at 100 km)
    glm::dvec3 B{0.0}, E{0.0}; // begin / end points (ECEF, m)
    glm::dvec3 radiant{0.0};   // ECEF unit toward the radiant
    double hB = 0.0, hE = 0.0; // heights (m)
    float trainLife = 0.0f;  // persistent train's decay time (s; 0 = none)
    int forced = 0;          // a harness-forced meteor (not generated)
};

struct GenParams
{
    double tA = 0.0, tB = 0.0;      // meteors STARTING in [tA, tB)
    double maxAgeOnly = -1.0;       // > 0: per band, only bins covering [tB - maxAge_b, tB) (the frame's draw)
    glm::dvec3 eyeEcef{0.0, 0.0, 1.0}; // the observer (ECEF, m)
    double cullMag = 9.0;           // generate only what could be brighter than this from the eye (with 1 mag margin)
    double rateGain = 1.0, fireballGain = 1.0;
    int onlyShower = -1;            // >= 0: that shower alone
    int onlyBand = -1;              // >= 0: that magnitude band alone
};
// Calls cb for every meteor (deterministic order). Returns the number of Poisson draws made (the cost).
int generate(const GenParams &p, const std::function<void(const Meteor &)> &cb);

// The light curve: relative intensity (peak 1 at the main maximum, flares above it) at fraction x of the flight.
double lightCurve(const Meteor &m, double x);
// Fragments of a fireball (M <= -4): count, and fragment k's offset (ECEF, m) from the main body and its intensity
// relative to the main body's, tau seconds after the start. Returns false when the fragment is not glowing.
int fragmentCount(const Meteor &m);
bool fragmentAt(const Meteor &m, int k, double tau, glm::dvec3 &off, double &rel);
// A point on the path at fraction x (ECEF).
inline glm::dvec3 pathAt(const Meteor &m, double x) { return m.B + (m.E - m.B) * x; }
// The persistent train's upper-atmosphere wind at height h (east / north m/s) for this meteor (twists with height).
glm::dvec2 trainWind(const Meteor &m, double hM);
// Emission colour (linear rgb, max 1) for a meteor at relative intensity rel of its peak.
glm::vec3 color(const Meteor &m, double rel);
// Fills a Meteor's geometry from its end point (ECEF unit), radiant (ECEF unit), speed and magnitude. False if the
// radiant is below that horizon.
bool buildPath(Meteor &m, const glm::dvec3 &endUpEcef, const glm::dvec3 &radiantEcef, double vMs, double M);
} // namespace meteors
