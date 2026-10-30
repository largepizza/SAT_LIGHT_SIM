// SatelliteSimInertial.cpp — the inertial (sidereal) camera lock (2026-10-28; CLAUDE.md "Inertial camera lock").
//
// The view is held fixed relative to the stars (the geocentric ECI frame: the J2000 equator, the Earth's axis): every
// frame, after the sim time advanced (recordCompute, right after updateFollow, before updatePositions), the stored
// ECI view direction is turned into this moment's ECEF / ENU and set as the camera's az / el. With `hold` the eye is
// held fixed in ECI as well (its ECEF position turns backward with the Earth): from orbit the Earth turns beneath.
// Without it the eye stays on the ground (rotating with the Earth) and only the view direction tracks the sky (an
// alt-az mount: the field turns about the view axis over hours). Mouse look and WASD still work: a change the player
// made since the last frame is adopted (in the previous frame's ECI). Ended by follow mode, Track or a cinematic.
// Built for watching the Great Comet over weeks at days per second (the comet's particles are a pure function of
// sim time, so any warp shows the same tail).
#include "../Harness.h"
#include "SatelliteSim.h"
#include "SatPhotometry.h"

#include <nlohmann/json.hpp>
#include <cmath>

namespace
{
glm::dvec3 ecefToEci(const glm::dvec3 &v, double th)
{
    const double c = std::cos(th), s = std::sin(th);
    return glm::dvec3(c * v.x - s * v.y, s * v.x + c * v.y, v.z);
}
glm::dvec3 eciToEcef(const glm::dvec3 &v, double th)
{
    const double c = std::cos(th), s = std::sin(th);
    return glm::dvec3(c * v.x + s * v.y, -s * v.x + c * v.y, v.z);
}
void enuBasis(const glm::dvec3 &up, glm::dvec3 &E, glm::dvec3 &N)
{
    const double cLH = std::sqrt(up.x * up.x + up.y * up.y);
    const double cLn = cLH > 1e-9 ? up.x / cLH : 1.0, sLn = cLH > 1e-9 ? up.y / cLH : 0.0;
    E = glm::dvec3(-sLn, cLn, 0.0);
    N = glm::dvec3(-up.z * cLn, -up.z * sLn, cLH);
}
double obliquityR(double t) { return (23.439 - 0.0000004 * (t / 86400.0)) * (3.14159265358979323846 / 180.0); }
} // namespace

glm::dvec3 SatelliteSim::inertialCamDirEcef() const
{
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    glm::dvec3 E, N;
    enuBasis(up, E, N);
    const double az = glm::radians((double)camera.azDeg), el = glm::radians((double)camera.elDeg);
    return std::sin(az) * std::cos(el) * E + std::cos(az) * std::cos(el) * N + std::sin(el) * up;
}

void SatelliteSim::inertialStart(bool hold)
{
    if (followActive) stopFollow();
    stopTrack();
    skAimA = skAimB = -1; // (a sky event's kept aim would fight it)
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double th = earthRotationAngle(t);
    inertialActive = true;
    inertialHold = hold;
    inertialEyeEci = ecefToEci(glm::normalize(glm::dvec3(obsDir)) * (satphot::kEarthRadiusM + (double)obsHeightOffset), th);
    inertialViewEci = ecefToEci(inertialCamDirEcef(), th);
    inertialLastTheta = th;
    inertialLastObsDir = obsDir;
    inertialLastHeight = obsHeightOffset;
    inertialLastAz = camera.azDeg;
    inertialLastEl = camera.elDeg;
}

// WASD while the eye is HELD in ECI (an orbit-height eye in the Stars frame). The ground-style move (a step along the
// facing tangent, `obsFacing`, on the Earth-fixed sphere) is the wrong motion there, and was the issue 20261029_180413
// "singularity": the facing is the view's azimuth, which at a nadir view (el -88.8 in the report) is degenerate and
// swings with every degree of look, and a step along it at constant radius is an orbit about the Earth's centre while
// the view stays on the stars. Here the eye flies in the camera's own frame: forward along the view direction (toward
// what you see: the Earth if you look down), sideways along the screen's right, at the usual height-scaled speed.
void SatelliteSim::inertialFly(float fwd, float right, float speedRadPerS, float dt)
{
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    glm::dvec3 E, N;
    enuBasis(up, E, N);
    const double az = glm::radians((double)camera.azDeg);
    const glm::dvec3 f = inertialCamDirEcef();
    const glm::dvec3 r = std::cos(az) * E - std::sin(az) * N; // the screen's right (horizontal), defined at any elevation
    const double R = satphot::kEarthRadiusM + (double)obsHeightOffset;
    const glm::dvec3 np = up * R + (f * (double)fwd + r * (double)right) * ((double)speedRadPerS * R * (double)dt);
    const double nr = std::clamp(glm::length(np), satphot::kEarthRadiusM + std::max(0.0, (double)obsTerrainH) + 2.0,
                                 satphot::kEarthRadiusM + (double)kMaxObsHeightM);
    const glm::dvec3 nu = glm::normalize(np);
    obsDir = glm::vec3(nu);
    obsHeightOffset = (float)(nr - satphot::kEarthRadiusM);
    obsLatDeg = glm::degrees(std::asin(glm::clamp(obsDir.z, -1.0f, 1.0f)));
    obsLonDeg = glm::degrees(std::atan2(obsDir.y, obsDir.x));
}

void SatelliteSim::inertialStop() { inertialActive = false; }

// recordCompute, after the time advance: hold the eye (with hold) and the view in ECI at this frame's time.
void SatelliteSim::inertialTick()
{
    if (!inertialActive) return;
    if (followActive || trackActive || cineActive())
    {
        inertialActive = false;
        return;
    }
    const double th0 = inertialLastTheta;
    // what the player changed since the last frame (in the previous frame's ECI)
    // Both the direction AND the height: Q / E, the altitude field and its scroll change only obsHeightOffset, and a hold
    // that adopted only obsDir overwrote it with the held eye every frame (issue 20261029_180413: "can't change altitude").
    // Nothing else writes either between ticks, so an exact compare is safe (and keeps moves under 1e-7).
    if (obsDir != inertialLastObsDir || obsHeightOffset != inertialLastHeight)
        inertialEyeEci = ecefToEci(glm::normalize(glm::dvec3(obsDir)) * (satphot::kEarthRadiusM + (double)obsHeightOffset), th0);
    if (std::abs(std::remainder(camera.azDeg - inertialLastAz, 360.0f)) > 0.01f || std::abs(camera.elDeg - inertialLastEl) > 0.01f)
        inertialViewEci = ecefToEci(inertialCamDirEcef(), th0);
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double th = earthRotationAngle(t);
    if (inertialHold)
    {
        const glm::dvec3 p = eciToEcef(inertialEyeEci, th);
        const double r = glm::length(p);
        obsDir = glm::vec3(p / r);
        obsHeightOffset = (float)std::max(r - satphot::kEarthRadiusM, 0.0);
        obsLatDeg = glm::degrees(std::asin(glm::clamp(obsDir.z, -1.0f, 1.0f)));
        obsLonDeg = glm::degrees(std::atan2(obsDir.y, obsDir.x));
    }
    const glm::dvec3 up = glm::normalize(glm::dvec3(obsDir));
    glm::dvec3 E, N;
    enuBasis(up, E, N);
    const glm::dvec3 v = eciToEcef(inertialViewEci, th);
    const float az = (float)glm::degrees(std::atan2(glm::dot(v, E), glm::dot(v, N)));
    const float el = (float)glm::degrees(std::asin(std::clamp(glm::dot(v, up), -1.0, 1.0)));
    aimCameraAzEl(az, el);
    inertialLastTheta = th;
    inertialLastObsDir = obsDir;
    inertialLastHeight = obsHeightOffset;
    inertialLastAz = camera.azDeg;
    inertialLastEl = camera.elDeg;
}

// ── Harness: inertial [state] | on [hold=1] | off | look ecl_lon= ecl_lat= [fov=] | place ecl_lon= ecl_lat= alt= ──
harness::Status SatelliteSim::harnessInertial(harness::Active &a)
{
    const harness::Command &c = a.cmd;
    nlohmann::json &r = a.result;
    const std::string sub = c.pos.empty() ? std::string("state") : c.pos[0];
    const double t = (double)simDayJ2000 * 86400.0 + simSecInDay;
    const double eps = obliquityR(t);
    auto eclDir = [&](double lonDeg, double latDeg) {
        const double lo = glm::radians(lonDeg), la = glm::radians(latDeg);
        const glm::dvec3 e(std::cos(la) * std::cos(lo), std::cos(la) * std::sin(lo), std::sin(la));
        return glm::dvec3(e.x, e.y * std::cos(eps) - e.z * std::sin(eps), e.y * std::sin(eps) + e.z * std::cos(eps)); // -> ECI
    };
    if (c.has("occult")) inertialOcculter = c.num("occult", 0.0) > 0.5;
    if (sub == "move")
    {
        // held movement input through the real WASD / Q-E code (sticks until the next `inertial move`; no args = stop)
        harnessMove_ = glm::vec3((float)c.num("fwd", 0.0), (float)c.num("right", 0.0), (float)c.num("up", 0.0));
        r["move"] = {harnessMove_.x, harnessMove_.y, harnessMove_.z};
        r["observer"] = {{"lat_deg", obsLatDeg}, {"lon_deg", obsLonDeg}, {"alt_m", obsHeightOffset}};
        return harness::Status::Done;
    }
    if (sub == "on")
    {
        inertialStart(c.num("hold", 1.0) > 0.5);
    }
    else if (sub == "off")
    {
        inertialStop();
    }
    else if (sub == "look")
    {
        if (!inertialActive) inertialStart(c.num("hold", 1.0) > 0.5);
        inertialViewEci = eclDir(c.num("ecl_lon", 0.0), c.num("ecl_lat", 0.0));
        if (c.has("fov")) camera.fovYDeg = std::clamp((float)c.num("fov", 60.0), 0.5f, 120.0f);
        inertialTickApplyNow_ = true;
    }
    else if (sub == "place")
    {
        // the eye at a geocentric ecliptic direction, alt metres above sea level (held in ECI)
        if (!inertialActive) inertialStart(true);
        inertialHold = true;
        const double alt = std::clamp(c.num("alt", 50.0e6), 0.0, (double)kMaxObsHeightM);
        inertialEyeEci = eclDir(c.num("ecl_lon", 0.0), c.num("ecl_lat", 0.0)) * (satphot::kEarthRadiusM + alt);
        inertialTickApplyNow_ = true;
    }
    else if (sub != "state")
    {
        a.error = "inertial [state] | move [fwd= right= up=] | on [hold=1] | off | look ecl_lon= ecl_lat= [fov=] | place ecl_lon= ecl_lat= alt= (any: occult=0|1)";
        return harness::Status::Error;
    }
    if (inertialTickApplyNow_)
    {
        // apply at once (the script's next capture sees it): set the camera from the stored ECI state
        inertialTickApplyNow_ = false;
        inertialLastObsDir = obsDir;
        inertialLastHeight = obsHeightOffset;
        inertialLastAz = camera.azDeg;
        inertialLastEl = camera.elDeg;
        inertialLastTheta = earthRotationAngle(t);
        inertialTick();
    }
    r["active"] = inertialActive;
    r["hold"] = inertialHold;
    r["occulter"] = inertialOcculter;
    {
        const glm::dvec3 v = inertialViewEci;
        const glm::dvec3 ve(v.x, v.y * std::cos(eps) + v.z * std::sin(eps), -v.y * std::sin(eps) + v.z * std::cos(eps));
        r["view_ecl_lon_deg"] = std::fmod(glm::degrees(std::atan2(ve.y, ve.x)) + 360.0, 360.0);
        r["view_ecl_lat_deg"] = glm::degrees(std::asin(std::clamp(ve.z, -1.0, 1.0)));
        const glm::dvec3 p = inertialEyeEci;
        const glm::dvec3 pe(p.x, p.y * std::cos(eps) + p.z * std::sin(eps), -p.y * std::sin(eps) + p.z * std::cos(eps));
        r["eye_ecl_lon_deg"] = std::fmod(glm::degrees(std::atan2(pe.y, pe.x)) + 360.0, 360.0);
        r["eye_ecl_lat_deg"] = glm::degrees(std::asin(std::clamp(pe.z / std::max(glm::length(pe), 1.0), -1.0, 1.0)));
        r["eye_alt_m"] = glm::length(p) - satphot::kEarthRadiusM;
    }
    r["camera"] = {{"az_deg", camera.azDeg}, {"el_deg", camera.elDeg}, {"fov_deg", camera.fovYDeg}};
    r["observer"] = {{"lat_deg", obsLatDeg}, {"lon_deg", obsLonDeg}, {"alt_m", obsHeightOffset}};
    return harness::Status::Done;
}
