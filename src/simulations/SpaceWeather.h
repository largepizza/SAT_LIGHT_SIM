#pragma once
// Space weather for the aurora (2026-10-03): a deterministic geomagnetic activity index over sim time,
// and the auroral oval it implies. Everything is a PURE FUNCTION of absolute sim time (seconds since
// J2000), like the orbits and the Reflect lock windows, so time reversal, time warp and a bookmark all
// see the same storm at the same instant.
//
// The index is a Kp equivalent (0..9, up to ~11 for a Carrington-class event), built from the drivers
// real activity comes from:
//   - the 11-year solar cycle (cycle 25 minimum Dec 2019; cycle 26 peaks ~2035-2036),
//   - recurrent high-speed streams from coronal holes (every 27.27-day solar rotation, a hole living
//     ~6 rotations: moderate Kp 3-5 for a few days, recurring),
//   - CME-driven storms (Poisson arrivals scaled by the cycle and the Russell-McPherron equinox bias;
//     sudden commencement, a main phase of hours, a recovery of a day or two; peak Kp 5 + an
//     exponential tail: Kp >= 8 a few times a year near maximum, Kp 9+ about yearly, Carrington rare),
//   - substorms (every few hours once Kp >= ~2: onset near 23 MLT, explosive brightening, a poleward
//     bulge that widens and drifts westward, recovery over an hour or two).
#include <glm/glm.hpp>

struct SpaceWeather {
    double kp = 0.0;          // the activity index (Kp equivalent, 0..11)
    double kpQuiet = 0.0;     // the background part (cycle + noise), for readouts
    double hss = 0.0;         // the recurrent-stream part
    double cme = 0.0;         // the strongest CME storm's part
    double cmePeakKp = 0.0;   // that storm's peak, 0 = none in progress
    double cmeHoursSince = 0.0; // hours since that storm's arrival
    double cycle = 0.0;       // solar cycle envelope 0..1
    // The strongest substorm in progress (intensity 0 = none).
    double subI = 0.0;        // 0..~1.5
    double subMltRad = 0.0;   // centre of the bulge, radians of MLT from midnight (east positive)
    double subHalfWRad = 0.0; // half-width of the bulge (radians of MLT)
    double subPoleDeg = 0.0;  // how far the bulge pushes the poleward edge toward the pole
    double subMinutes = -1.0; // minutes since onset, < 0 = none
};

// rateScale multiplies the CME arrival rate and the coronal holes' odds (1 = the model's calibration).
// kpOverride >= 0 replaces the index with a fixed value (manual mode); substorms still come and go, at
// that activity.
SpaceWeather spaceWeatherAt(double tJ2000s, double rateScale, double kpOverride = -1.0);
// Just the index (no substorm), cheap enough to call in a search loop.
double spaceWeatherKp(double tJ2000s, double rateScale);

// The oval's boundaries for an activity index, as COLATITUDES from the geomagnetic pole (degrees), at
// magnetic midnight and noon (Feldstein-Starkov-like: quiet midnight 66.5-72.5 MLAT, noon 76.5-79;
// the equatorward edge at midnight moves ~2.1 deg per Kp — Kp 9 overhead near 47 MLAT).
struct AuroraOvalBounds {
    float eqMid, eqNoon, polMid, polNoon; // colatitudes, degrees
};
AuroraOvalBounds auroraOvalBounds(double kp);

// The UBO block the shaders read (cloud_params.glsl auroraMidnight / auroraOval / auroraSub / auroraOval2).
struct AuroraGpu {
    glm::vec4 midnight; // xyz unit vector toward magnetic midnight in the magnetic equatorial plane (ECEF), w kp
    glm::vec4 oval;     // AuroraOvalBounds
    glm::vec4 sub;      // substorm: x intensity, y cos / z sin of its MLT angle, w 1 / half-width^2 (rad^-2)
    glm::vec4 oval2;    // x brightness from activity, y max lit colatitude (deg), z ripple (deg), w the substorm's
                        // poleward push (deg)
};
AuroraGpu auroraGpuParams(const SpaceWeather &sw, const glm::dvec3 &sunEcef);

// CPU mirror of the shader's band (shaders/include/aurora_oval.glsl), without the noise: 0..1 brightness
// of the oval at a ground direction (ECEF unit vector). The ambience driver uses it.
float auroraOvalWeightCpu(const AuroraGpu &g, const glm::dvec3 &dirEcef);

// The geomagnetic north pole (dipole axis), ECEF unit vector — kGeomagPoleECEF in the shaders.
glm::dvec3 geomagPoleEcef();

// Geomagnetic storm scale for a Kp (NOAA G1..G5 from Kp 5..9), or 0.
int geomagStormScale(double kp);
