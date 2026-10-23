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
    double windAz = 0;               // the mean wind it spreads and blows its smoke toward (rad from north, toward)
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
    double windAz = 0;       // the wind now (rad from north, toward): the mean +- a slow wander
    double plumeTopM = 0;    // smoke injection height above the ground (m)
    double ageS = 0;
};

FireState fireStateAt(const Fire &f, double t);
// The point on the burnt ellipse's edge at angle a (0 = the head, downwind), metres east / north of the ignition point.
glm::dvec2 fireEdgeAt(const FireState &s, double a);
// How hot the front is at edge angle a: the head 1, the flanks ~0.35, the back ~0.1.
double fireFrontIntensity(double a);

// Every fire born in the 1-degree cells within radiusKm of (lat, lon) over the last kMaxLife, alive at time t.
// rateScale x the calibrated rate (0 = none).
constexpr double kFireMaxLifeS = 30.0 * 86400.0;
void firesNear(double t, double latDeg, double lonDeg, double radiusKm, const BiomeMap &bio, float rateScale,
               std::vector<Fire> &out);
// Every fire born in day bins [binFrom, binTo] (bin = floor(J2000 s / 86400)) in the cells within radiusKm, alive or
// not (the sim caches them and adds each new day's bin). Appends.
void firesBorn(int64_t binFrom, int64_t binTo, double latDeg, double lonDeg, double radiusKm, const BiomeMap &bio,
               float rateScale, std::vector<Fire> &out);
// Every fire alive worldwide at t (for the harness: ~2M hashes, tens of ms).
void firesWorld(double t, const BiomeMap &bio, float rateScale, std::vector<Fire> &out);

} // namespace wx
