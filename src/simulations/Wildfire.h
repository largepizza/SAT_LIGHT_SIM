#pragma once
// Wildfires (2026-10-23, .plans/WEATHER_HAZARDS_PLAN.md section 1). Like the storms, a pure function of sim time:
// fires are hashed per (1-degree cell, 1-day bin) at a rate = fuel (the biome map) x the season's dryness there x a
// climatology factor, each with its whole life fixed at birth (ignition point, life, size, the wind it spreads with).
// Earth-fixed (they burn on the ground; the cloud map's drift does not move them). App-independent: the sim draws
// the smoke (plume Gaussians in the dust list), the front (point sprites) and its light (an engine-style point light).
#include "Biome.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <vector>

namespace wx {

struct Fire {
    uint64_t id = 0;
    double latDeg = 0, lonDeg = 0;   // ignition point
    double t0 = 0, life = 0;         // ignition (s since J2000), lifetime (s)
    double windAz = 0;               // the wind it spreads with (rad from north, toward): windAt() near the ground over its
                                     // life (its smoke follows the wind of each moment and height, windAt())
    double spreadMps = 0.05;         // the head's day-averaged spread rate
    float size = 0.5f;               // 0..1: how big it gets (life, plume height, smoke)
    float fuel = 0.5f;               // the biome fuel at the ignition point
    float pyro = 0.0f;               // 0 or 1: a pyrocumulonimbus fire (rare, big): smoke to 5-8 km
    uint32_t seed = 0;
};

// A fire at one moment.
struct FireState {
    double activity = 0;     // 0..1: the lifecycle x the diurnal cycle (afternoon peak, quiet nights)
    double headM = 0;        // head distance downwind of the ignition point (m)
    double backM = 0;        // backing distance upwind
    double halfWidthM = 0;   // the burnt ellipse's half width
    double windAz = 0;       // the direction it spreads (rad from north, toward): the burnt ellipse's axis (fixed: a
                             // burnt area does not turn with the wind; the smoke follows windAt())
    double plumeTopM = 0;    // smoke injection height above the ground (m)
    double ageS = 0;
};

FireState fireStateAt(const Fire &f, double t);
// The point on the burnt ellipse's edge at angle a (0 = the head, downwind), metres east / north of the ignition point.
glm::dvec2 fireEdgeAt(const FireState &s, double a);
// How hot the front is at edge angle a: the head 1, the flanks ~0.35, the back ~0.1.
double fireFrontIntensity(double a);
// The real front (2026-10-23 b): the ellipse with FINGERS (a radial perturbation from its centre, ~10% at the head,
// growing slowly with the fire) at edge angle a, metres east / north of the ignition point; and ds/da there (m/rad).
glm::dvec2 fireFrontAt(const Fire &f, const FireState &s, double a, double *dsda = nullptr);
// The fingers' radial factor at edge angle a (fireFrontAt = the ellipse's point at a scaled by it from the centre).
double fireFrontRadiusK(const Fire &f, const FireState &s, double a);
// How much of the front at angle a is burning now (2026-10-24 rework, the user: "fire generates as rings, at best they
// should be arcs with scattered flames here and there"): an ACTIVE ARC round the head (the whole ring only while the
// fire is young, narrowing to the head and the upper flanks, one flank often stronger), broken into runs of flame and
// long gaps (re-drawn over ~1.5 h, nothing between runs), with a few isolated flare-ups off the arc; 0 where the
// ground there cannot burn (bio: the biome map's fuel less its built-up mask: lakes, towns, bare desert). 0..~1.5.
double fireFrontHeat(const Fire &f, const FireState &s, double a, double t, const BiomeMap *bio = nullptr);
// The share of the front's heat on ground that can burn now (0..1; the smoke and the flames are x it): a fire whose
// front has run into a town, the sea or bare ground is out though its state still burns (2026-10-27: the Weather
// window and the harness ranked such fires first and framed a fire with no smoke or flames).
double fireBurningShare(const Fire &f, const FireState &s, double t, const BiomeMap &bio);
// The lat / lon of a point x, y metres east / north of the ignition point.
void fireOffsetLatLon(const Fire &f, const glm::dvec2 &q, double &latDeg, double &lonDeg);
// The flames: their height (m) and the depth of the flaming zone (m) at a front heat of 1.
double fireFlameHeightM(const Fire &f, const FireState &s);
double fireFlameDepthM(const Fire &f);

// The wind (2026-10-23 b): a pure function of place, height above the ground and time, coherent in space, so
// neighbouring fires' smoke agrees. m/s east / north. A zonal climatology (trades, westerlies, polar easterlies; the
// trades weaken and the westerlies strengthen with height) + synoptic eddies (~3000 km, days) + mesoscale (~400 km,
// hours) + small eddies (~30 km, an hour: the plumes' wiggle), each the curl of a sum of travelling waves on the
// sphere (divergence free). Near the ground it is slower and backed by friction (turned ~25 deg toward low pressure),
// and above it turns with height by a slowly varying thermal-wind angle (up to ~+-35 deg by 3 km), so a plume's layers
// go different ways.
glm::dvec2 windAt(const glm::dvec3 &dirEcef, double hM, double t);

// The smoke: its injection height now (m above the ground: the fire's plume top, lower in a strong wind, the column
// bending over) and the updraft that lifts it (m/s).
double fireInjectionHeightM(const Fire &f, const FireState &s, double t);
double fireUpdraftMps(const Fire &f);
// The height (m above the ground) of smoke emitted ageS ago, rising from its source toward its layer Hb.
double fireSmokeRiseM(double ageS, double Hb, double w0);
// Where the column starts (m east / north of the ignition point): a little downwind of the head, so the flames stand
// upwind of their smoke and are seen from the side (the near-ground smoke is the flames' own, carried off downwind).
glm::dvec2 fireSmokeSource(const Fire &f, const FireState &s, double t);
// Smoke TRAJECTORIES (2026-10-26, issue 20261026_204239 "the smoke wavers around randomly and sometimes changes direction
// entirely ... changing wind direction at the fire's source should have no effect on smoke farther down the trail"):
// every parcel of smoke is advected from where and when it left the fire, through windAt at ITS OWN position, height and
// time, step by step (Euler on a fixed schedule of ages, so a trajectory is a pure function of its emission time: the
// sim caches them and only ever extends them). fireSmokeStepS = the step from age a to the next knot of the schedule.
double fireSmokeStepS(double ageS);
// One step of a parcel: xy (m east / north of the ignition point) at age a, emitted at tE, rising to Hb with updraft w0.
glm::dvec2 fireSmokeAdvect(const Fire &f, const glm::dvec2 &xy, double ageS, double h, double tE, double Hb, double w0);
// Where the smoke emitted ageS ago (at t - ageS, from the column's source then) into the layer Hb is now (x, y metres
// east / north of the ignition point, z its height): its trajectory integrated on the schedule (the harness; the sim
// caches the same trajectories). s is unused (kept for the callers: the state at the emission is computed).
glm::dvec3 fireSmokeAt(const Fire &f, const FireState &s, double t, double ageS, double Hb, int steps = 16);

// Every fire born in the 1-degree cells within radiusKm of (lat, lon) over the last kMaxLife, alive at time t.
// rateScale x the calibrated rate (0 = none).
constexpr double kFireMaxLifeS = 30.0 * 86400.0;
void firesNear(double t, double latDeg, double lonDeg, double radiusKm, const BiomeMap &bio, float rateScale,
               std::vector<Fire> &out);
// Every fire born in day bins [binFrom, binTo] (bin = floor(J2000 s / 86400)) in the cells within radiusKm, alive or
// not (the sim caches them and adds each new day's bin). Appends.
void firesBorn(int64_t binFrom, int64_t binTo, double latDeg, double lonDeg, double radiusKm, const BiomeMap &bio,
               float rateScale, std::vector<Fire> &out);
// firesBorn over the 1-degree latitude rows [rowFrom, rowTo] only (the rows firesBorn scans are
// floor(latDeg -+ (radiusKm / 111.2 + 1))): a caller can split the rows over threads and append the parts in row order —
// the same fires in the same order as one firesBorn call.
void firesBornRows(int64_t binFrom, int64_t binTo, int rowFrom, int rowTo, double latDeg, double lonDeg, double radiusKm,
                   const BiomeMap &bio, float rateScale, std::vector<Fire> &out);
// Every fire alive worldwide at t (for the harness: ~2M hashes, tens of ms).
void firesWorld(double t, const BiomeMap &bio, float rateScale, std::vector<Fire> &out);

} // namespace wx
