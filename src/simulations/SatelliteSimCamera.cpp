// SatelliteSimCamera.cpp — the camera rig (2026-11-14, camera revamp phase 2; docs/subsystems/ui.md "Camera rig").
//
// The camera's state is two DOUBLE unit vectors in the Earth-fixed frame (ECEF): camEyeDir, the eye's direction from
// the Earth's centre, and camFwd, the look direction (the eye's height stays obsHeightOffset). Everything the rest of
// the sim reads — obsDir, obsFacing, obsLatDeg / obsLonDeg, camera.azDeg / elDeg — is a float MIRROR written here by
// camPublish(), and every write goes through the setters below, which publish at once (a reader later in the same
// frame sees the change). SkyCamera::viewMatrix() still builds skyView from the published az / el, so the image is
// what it was.
//
// Why: obsDir was the canonical position and a float. A unit vector's ulp is ~6e-8 (~0.4 m on the Earth's surface),
// and WASD added speed x dt to it a frame: under ~10 m/s the step rounded away (2 m above the sea, holding W for 3 s
// moved 0.2 m of 9) and up to ~30 m/s the rounding, not the heading, chose where it went (10 m: 34 m on bearing 2.7
// deg whether facing 0 or 45). The phase 1 audit is in docs/subsystems/ui.md "Camera model and its limits".
//
// camSync() (once before the look input, once before the movement) is the net under the setter rule: a mirror
// changed since the last publish — a write this conversion missed — is adopted, with a log line, instead of being
// silently overwritten.
#include "SatelliteSim.h"
#include "../Log.h"

#include <cmath>
#include <cstdio>

namespace
{
constexpr double kDeg = 3.14159265358979323846 / 180.0;
// East and North at a unit up (Earth-fixed), as the float code everywhere builds them; North at the poles' fallback.
void enuAt(const glm::dvec3 &up, glm::dvec3 &E, glm::dvec3 &N)
{
    const double cLH = std::sqrt(up.x * up.x + up.y * up.y);
    const double cLn = cLH > 1e-9 ? up.x / cLH : 1.0, sLn = cLH > 1e-9 ? up.y / cLH : 0.0;
    E = glm::dvec3(-sLn, cLn, 0.0);
    N = glm::dvec3(-up.z * cLn, -up.z * sLn, cLH);
}
// v on the horizontal plane at up, unit; false when v is (nearly) vertical.
bool horizontal(const glm::dvec3 &v, const glm::dvec3 &up, glm::dvec3 &out)
{
    const glm::dvec3 h = v - glm::dot(v, up) * up;
    const double l = glm::length(h);
    if (!(l > 1e-12))
        return false;
    out = h / l;
    return true;
}
} // namespace

glm::dvec3 SatelliteSim::camFacing() const
{
    glm::dvec3 f;
    if (horizontal(camFwd, camEyeDir, f))
        return f;
    glm::dvec3 E, N;
    enuAt(camEyeDir, E, N);
    return N;
}

double SatelliteSim::camElDeg() const
{
    return std::asin(std::clamp(glm::dot(camFwd, camEyeDir), -1.0, 1.0)) / kDeg;
}

// (A setter before the first camSync() runs it first: the rig starts from the startup mirrors.)
void SatelliteSim::camSetEyeDir(const glm::dvec3 &dir)
{
    const double l = glm::length(dir);
    if (!(l > 0.0) || !std::isfinite(l))
        return;
    if (!camRigInit)
        camSync();
    const double el = camElDeg();
    const glm::dvec3 f0 = camFacing();
    camEyeDir = dir / l;
    glm::dvec3 f;
    if (!horizontal(f0, camEyeDir, f))
    {
        glm::dvec3 E;
        enuAt(camEyeDir, E, f);
    }
    camFwd = glm::normalize(std::cos(el * kDeg) * f + std::sin(el * kDeg) * camEyeDir);
    camPublish();
}

void SatelliteSim::camSetLook(const glm::dvec3 &facing, double elDeg, double elClampDeg)
{
    if (!camRigInit)
        camSync();
    glm::dvec3 f;
    if (!horizontal(facing, camEyeDir, f))
        f = camFacing();
    const double el = std::clamp(std::isfinite(elDeg) ? elDeg : camElDeg(), -elClampDeg, elClampDeg) * kDeg;
    camFwd = glm::normalize(std::cos(el) * f + std::sin(el) * camEyeDir);
    camPublish();
}

void SatelliteSim::camSetLookDir(const glm::dvec3 &dir, double elClampDeg)
{
    const double l = glm::length(dir);
    if (!(l > 0.0) || !std::isfinite(l))
        return;
    if (!camRigInit)
        camSync();
    const glm::dvec3 d = dir / l;
    camSetLook(d, std::asin(std::clamp(glm::dot(d, camEyeDir), -1.0, 1.0)) / kDeg, elClampDeg);
}

void SatelliteSim::camAimAzEl(double azDeg, double elDeg, double elClampDeg)
{
    if (!camRigInit)
        camSync();
    glm::dvec3 E, N;
    enuAt(camEyeDir, E, N);
    const double az = azDeg * kDeg;
    camSetLook(std::cos(az) * N + std::sin(az) * E, elDeg, elClampDeg);
    // The az as asked (200 stays 200, not -160) until the next publish derives it again, as aimCameraAzEl always did.
    if (std::abs(std::remainder((double)camera.azDeg - azDeg, 360.0)) < 1e-3)
        camera.azDeg = camPubAzDeg = (float)azDeg;
}

void SatelliteSim::camYaw(double rad)
{
    if (rad == 0.0)
        return;
    if (!camRigInit)
        camSync();
    // As the float code did: facing' = cos(a) facing + sin(a) (up x facing), up x facing being the LEFT tangent.
    const glm::dvec3 f = camFacing();
    const glm::dvec3 left = glm::cross(camEyeDir, f);
    camSetLook(std::cos(rad) * f + std::sin(rad) * left, camElDeg(), 90.0);
}

void SatelliteSim::camPitch(double deg, double elClampDeg)
{
    if (deg == 0.0)
        return;
    if (!camRigInit)
        camSync();
    // Never pulls an elevation already past the clamp (an aim at 89.9) back to it: only a move toward the horizon does.
    const double el0 = camElDeg();
    double el = el0 + deg;
    if (el > elClampDeg)
        el = std::max(std::min(el, el0), elClampDeg);
    if (el < -elClampDeg)
        el = std::min(std::max(el, el0), -elClampDeg);
    camSetLook(camFacing(), el, 90.0);
}

void SatelliteSim::camPublish()
{
    obsDir = glm::vec3(camEyeDir);
    obsLatDeg = (float)(std::asin(std::clamp(camEyeDir.z, -1.0, 1.0)) / kDeg);
    obsLonDeg = (float)(std::atan2(camEyeDir.y, camEyeDir.x) / kDeg);
    const glm::dvec3 f = camFacing();
    obsFacing = glm::vec3(f);
    glm::dvec3 E, N;
    enuAt(camEyeDir, E, N);
    camera.elDeg = (float)camElDeg();
    camera.azDeg = (float)(std::atan2(glm::dot(f, E), glm::dot(f, N)) / kDeg);
    camPubObsDir = obsDir;
    camPubFacing = obsFacing;
    camPubElDeg = camera.elDeg;
    camPubAzDeg = camera.azDeg;
}

void SatelliteSim::camSync()
{
    if (!camRigInit)
    {
        // The startup mirrors (constructor, loadSettings, a bookmark or the intro's first key before any setter).
        camRigInit = true;
        const double l = glm::length(glm::dvec3(obsDir));
        camEyeDir = l > 0.0 ? glm::dvec3(obsDir) / l : glm::dvec3(0.0, 0.0, 1.0);
        glm::dvec3 E, N;
        enuAt(camEyeDir, E, N);
        camFwd = N; // (the heading camSetLook keeps if obsFacing is vertical)
        camSetLook(glm::dvec3(obsFacing), camera.elDeg);
        return;
    }
    // A mirror written since the last publish: adopt it (eye first, then the heading, then the elevation).
    const bool eye = obsDir != camPubObsDir, fac = obsFacing != camPubFacing, el = camera.elDeg != camPubElDeg,
               az = camera.azDeg != camPubAzDeg;
    if (eye || fac || el || az)
    {
        if (camAdoptLogs < 20)
        {
            ++camAdoptLogs;
            char m[200];
            snprintf(m, sizeof(m), "camera rig: adopted an outside write to%s%s%s%s (a write that bypasses the cam* setters)",
                     eye ? " obsDir" : "", fac ? " obsFacing" : "", el ? " elDeg" : "", az ? " azDeg" : "");
            Log::line(m);
        }
        const glm::vec3 facing = obsFacing;
        const float elDeg = camera.elDeg, azDeg = camera.azDeg;
        if (eye)
            camSetEyeDir(glm::dvec3(obsDir));
        if (fac)
            camSetLook(glm::dvec3(facing), el ? (double)elDeg : camElDeg());
        else if (az)
            camAimAzEl(azDeg, el ? (double)elDeg : camElDeg());
        else if (el)
            camSetLook(camFacing(), elDeg);
        return;
    }
    camPublish();
}
