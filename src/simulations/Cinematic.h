#pragma once
// Cinematic camera paths (review 17). A Cinematic is a list of SHOTS; a shot is a spline through camera
// keyframes (observer lat/lon/altitude, camera azimuth/elevation/FOV, optionally sim time) plus an optional
// settings patch applied at its first frame (a cut). The harness's `path` commands edit the current shot,
// the Cinematics window edits the same data, and both play and export it the same way
// (SatelliteSimCinematic.cpp). Saved as JSON under <user data>/cinematics/.
#include <nlohmann/json.hpp>
#include <string>
#include <vector>

struct CineKey
{
    double t = 0.0;                                             // seconds into the shot
    double lat = 0.0, lon = 0.0, alt = 0.0, az = 0.0, el = 0.0, fov = 60.0;   // alt: m above sea level
    bool hasSim = false;
    double simT = 0.0;                                          // sim time, seconds since J2000
};

struct CineShot
{
    std::string name;
    std::vector<CineKey> keys;
    // Sim time when no key sets it: the time at the shot's start (simStartValid) advancing at simRate x real
    // time (0 = frozen). Keys with sim time override it (linear between them).
    bool simStartValid = false;
    double simStart = 0.0;
    double simRate = 1.0;
    nlohmann::json settings;                                    // null = leave the settings alone
    double duration() const { return keys.empty() ? 0.0 : keys.back().t - keys.front().t; }
};

struct Cinematic
{
    std::string name = "untitled";
    std::vector<CineShot> shots;
    double fps = 30.0;
    double duration() const;
    // The shot and its local time at global time t (shots play back to back).
    int shotAt(double t, double &local) const;
};

// Cubic Hermite with Catmull-Rom tangents per channel (log altitude, log FOV, unwrapped azimuth); sim
// time linear between the keys that set it.
CineKey cineEval(const std::vector<CineKey> &keys, double t);

nlohmann::json cineToJson(const Cinematic &c);
bool cineFromJson(const nlohmann::json &j, Cinematic &c, std::string &err);
