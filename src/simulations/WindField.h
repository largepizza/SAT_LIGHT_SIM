#pragma once
// The one wind (Weather v2, 2026-11-06; .plans/WEATHER_V2_PLAN.md): the CPU twin of shaders/include/wind_field.glsl.
// Keep the two line for line. Storm tracks, the eye's surface wind and the parity probe (harness `wind probe`) read it.
#include <glm/glm.hpp>

namespace wx {

constexpr double kWfPeriod = 2592000.0;   // 30 days: every frequency in time is a whole multiple of 1 / kWfPeriod
constexpr double kWfLowH  = 2000.0;       // the cloud layers' steering heights (m), as the GLSL
constexpr double kWfMidH  = 5000.0;
constexpr double kWfHighH = 10000.0;

// The wind's state at one moment: what the GLSL gets as a vec4 (ws).
struct WindState {
    double clock = 0.0;   // sim time mod kWfPeriod (s)
    double decl  = 0.0;   // the Sun's declination (rad)
    double waves = 1.0;   // Rossby waves' gain
    double eddies = 1.0;  // mesoscale eddies' gain
    glm::vec4 gpu() const { return glm::vec4((float)clock, (float)decl, (float)waves, (float)eddies); }
};

// simT: seconds since J2000; decl: the Sun's declination (rad).
WindState windState(double simT, double decl, double waves, double eddies);
// The state dt seconds later (the clock moved; the declination held).
WindState windStateAt(const WindState &ws, double dt);

// d: unit ECEF direction (z north), h: metres above sea level. ECEF wind vector (m/s), tangential.
glm::dvec3 windAt(const glm::dvec3 &d, double h, const WindState &ws);

// East / north components of windAt.
glm::dvec2 windEN(const glm::dvec3 &d, double h, const WindState &ws);

// Where the air now at d was dt seconds ago (dt > 0), at height h: RK2 back along windAt.
glm::dvec3 backtrace(const glm::dvec3 &d, double h, const WindState &ws, double dt, int steps);

} // namespace wx
