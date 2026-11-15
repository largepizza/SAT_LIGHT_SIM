// SatelliteSimCamera.cpp — the camera rig (2026-11-14, camera revamp; docs/subsystems/ui.md "Camera rig").
//
// The camera's state is three DOUBLE unit vectors in the Earth-fixed frame (ECEF): camEyeDir, the eye's direction from
// the Earth's centre, camFwd, the look direction, and camUp, the camera's up (normal to camFwd); the eye's height stays
// obsHeightOffset. Everything the rest of the sim reads — obsDir, obsFacing, obsLatDeg / obsLonDeg, camera.azDeg /
// elDeg / rollDeg and the view (camera.viewEnu, ENU -> camera) — is a MIRROR written here by camPublish(), and every
// write goes through the setters below, which publish at once (a reader later in the same frame sees the change).
//
// Phase 2 (double): obsDir was the canonical position and a float. A unit vector's ulp is ~6e-8 (~0.4 m on the
// Earth's surface), and WASD added speed x dt to it a frame: under ~10 m/s the step rounded away (2 m above the sea,
// holding W for 3 s moved 0.2 m of 9) and up to ~30 m/s the rounding, not the heading, chose where it went.
//
// Phase 3 (up vector, auto-level, roll): the view was glm::lookAt(fwd from az / el, up = Up, or North past |el| 88):
// from orbit, -87.9 -> -88.1 deg turned the whole image by the azimuth, and the mouse's yaw about the local up spun
// the image about its centre when looking down. Now the view is the rig's own basis. HORIZON AUTO-LEVEL (below
// camLevelAltKm, default 100 km, or switched off in Settings > Controls) keeps the first-person feel: yaw about the
// local up, pitch about the camera's right, stopping at +-89 deg, any roll eased back to level (the easing fades
// within ~10 deg of the nadir / zenith, where the roll is the heading). FREE (above it): yaw / pitch about the
// camera's own axes (looking down, a sideways drag slides the view instead of spinning it), no pitch limit, Z / C
// roll. Every input is a
// ROTATION of the camera (camYaw / camPitch / camRoll), never a rebuild from angles, so nothing snaps.
//
// camSync() (once before the look input, once before the movement) is the net under the setter rule: a mirror
// changed since the last publish — a write the conversion missed — is adopted, with a log line, instead of being
// silently overwritten.
#include "SatelliteSim.h"
#include "../Log.h"
#include "SatPhotometry.h"

#include <cmath>
#include <cstdio>

namespace
{
constexpr double kPi = 3.14159265358979323846;
constexpr double kDeg = kPi / 180.0;
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
// v turned by a (rad) about the unit axis k (right hand).
glm::dvec3 rotAbout(const glm::dvec3 &v, const glm::dvec3 &k, double a)
{
    const double c = std::cos(a), s = std::sin(a);
    return v * c + glm::cross(k, v) * s + k * (glm::dot(k, v) * (1.0 - c));
}
// v turned by the shortest rotation taking unit a to unit b (opposite: half a turn about an axis normal to a).
glm::dvec3 rotTo(const glm::dvec3 &v, const glm::dvec3 &a, const glm::dvec3 &b)
{
    const glm::dvec3 ax = glm::cross(a, b);
    const double s = glm::length(ax), c = glm::dot(a, b);
    if (s < 1e-15)
    {
        if (c > 0.0)
            return v;
        glm::dvec3 k = glm::cross(a, std::abs(a.x) < 0.9 ? glm::dvec3(1, 0, 0) : glm::dvec3(0, 1, 0));
        return rotAbout(v, glm::normalize(k), kPi);
    }
    const glm::dvec3 k = ax / s;
    return v * c + glm::cross(k, v) * s + k * (glm::dot(k, v) * (1.0 - c));
}
double smooth01(double e0, double e1, double x)
{
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
// How far the look is from vertical, as a 0..1 weight: 1 below ~78 deg of elevation, 0 within ~2 deg of the nadir /
// zenith. Auto-level's yaw axis and its pull to level fade with it: at the vertical, "level" has no meaning.
double levelWeight(const glm::dvec3 &fwd, const glm::dvec3 &up)
{
    const double s = glm::dot(fwd, up);
    return smooth01(0.035, 0.21, std::sqrt(std::max(0.0, 1.0 - s * s)));
}
} // namespace

glm::dvec3 SatelliteSim::camFacing() const
{
    // The look's horizontal part; near the nadir / zenith (where it shrinks to nothing and its direction is noise) the
    // screen's top (looking down) / bottom (looking up) takes over, so the heading never flips. For a level camera the
    // two point the same way, so the heading is the look's own everywhere a level camera can look.
    const glm::dvec3 U = camEyeDir;
    const double s = glm::dot(camFwd, U), c = std::sqrt(std::max(0.0, 1.0 - s * s));
    glm::dvec3 g = camFwd - s * U;
    g += (-s) * (1.0 - smooth01(0.02, 0.1, c)) * (camUp - glm::dot(camUp, U) * U);
    const double l = glm::length(g);
    if (l > 1e-12)
        return g / l;
    glm::dvec3 E, N;
    enuAt(U, E, N);
    return N;
}

double SatelliteSim::camElDeg() const
{
    return std::asin(std::clamp(glm::dot(camFwd, camEyeDir), -1.0, 1.0)) / kDeg;
}

double SatelliteSim::camRollDeg() const
{
    // From level: the local up on the plane normal to the look (straight down / up: the up's own horizontal part, so
    // the roll there reads 0 — it is the heading).
    const glm::dvec3 U = camEyeDir;
    glm::dvec3 lvl = U - glm::dot(U, camFwd) * camFwd;
    double l = glm::length(lvl);
    if (l < 1e-9)
    {
        lvl = camUp - glm::dot(camUp, U) * U;
        l = glm::length(lvl);
        if (l < 1e-12)
            return 0.0;
    }
    lvl /= l;
    return std::atan2(glm::dot(glm::cross(lvl, camUp), camFwd), glm::dot(lvl, camUp)) / kDeg;
}

double SatelliteSim::camEyeAltM() const
{
    return followActive ? followRadiusM - satphot::kEarthRadiusM : (double)obsEyeRadiusM() - satphot::kEarthRadiusM - 2.0;
}

bool SatelliteSim::camLevelActive() const
{
    return camLevelEnabled && camEyeAltM() < (double)camLevelAltKm * 1000.0;
}

double SatelliteSim::camVertFreeWeight() const
{
    if (!camLevelEnabled)
        return 1.0;
    const double L = (double)camLevelAltKm * 1000.0;
    return smooth01(L, L * kCamVertBlend, camEyeAltM());
}

// (A setter before the first camSync() runs it first: the rig starts from the startup mirrors.)
void SatelliteSim::camSetEyeDir(const glm::dvec3 &dir)
{
    const double l = glm::length(dir);
    if (!(l > 0.0) || !std::isfinite(l))
        return;
    if (!camRigInit)
        camSync();
    const glm::dvec3 u0 = camEyeDir, u1 = dir / l;
    camEyeDir = u1;
    camFwd = glm::normalize(rotTo(camFwd, u0, u1));
    camUp = glm::normalize(rotTo(camUp, u0, u1));
    camPublish();
}

void SatelliteSim::camSetLook(const glm::dvec3 &facing, double elDeg, double elClampDeg, bool keepRoll)
{
    if (!camRigInit)
        camSync();
    const glm::dvec3 U = camEyeDir;
    glm::dvec3 f;
    if (!horizontal(facing, U, f))
        f = camFacing();
    const double el = std::clamp(std::isfinite(elDeg) ? elDeg : camElDeg(), -elClampDeg, elClampDeg) * kDeg;
    const glm::dvec3 nf = glm::normalize(std::cos(el) * f + std::sin(el) * U);
    // Level for this look (defined straight up / down too: from the heading), then the roll it had: the roll ANGLE is
    // kept, so a level camera stays level through any re-aim. (Turning the up the shortest way with the look kept it
    // only for small turns: Go to's jump from the ground to a satellite arrived banked 5.7 deg.)
    const double roll0 = keepRoll ? camRollDeg() * kDeg : 0.0;
    camUp = rotAbout(-std::sin(el) * f + std::cos(el) * U, nf, roll0);
    camFwd = nf;
    camUp = glm::normalize(camUp - glm::dot(camUp, camFwd) * camFwd);
    camPublish();
}

void SatelliteSim::camSetLookDir(const glm::dvec3 &dir, double elClampDeg, bool keepRoll)
{
    const double l = glm::length(dir);
    if (!(l > 0.0) || !std::isfinite(l))
        return;
    if (!camRigInit)
        camSync();
    const glm::dvec3 d = dir / l;
    camSetLook(d, std::asin(std::clamp(glm::dot(d, camEyeDir), -1.0, 1.0)) / kDeg, elClampDeg, keepRoll);
}

void SatelliteSim::camAimAzEl(double azDeg, double elDeg, double elClampDeg, bool keepRoll)
{
    if (!camRigInit)
        camSync();
    glm::dvec3 E, N;
    enuAt(camEyeDir, E, N);
    const double az = azDeg * kDeg;
    camSetLook(std::cos(az) * N + std::sin(az) * E, elDeg, elClampDeg, keepRoll);
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
    // Auto-level: about the local up, as on the ground (a level camera stays level; looking straight down, the ground
    // turns under you). Free: about the camera's own up. (+ = left, right-handed about an up.) Blending to the
    // camera's up near the nadir under auto-level was tried: a sideways drag at -89 slid the view to -79 and swung the
    // heading 144 deg — a horizontal drag changing the elevation.
    const glm::dvec3 ax = camLevelActive() ? camEyeDir : camUp;
    camFwd = glm::normalize(rotAbout(camFwd, ax, rad));
    camUp = rotAbout(camUp, ax, rad);
    camUp = glm::normalize(camUp - glm::dot(camUp, camFwd) * camFwd);
    camPublish();
}

void SatelliteSim::camPitch(double deg)
{
    if (deg == 0.0)
        return;
    if (!camRigInit)
        camSync();
    const glm::dvec3 r = glm::normalize(camRightEcef());
    if (camLevelActive())
    {
        // Stops at +-89 deg; never pulls an elevation already past it (an aim at 90, a camera that came down from the
        // free zone upside down) back to it: only a move toward the horizon goes through.
        constexpr double kLim = 89.0;
        const double el0 = camElDeg();
        const double el1 = std::asin(std::clamp(glm::dot(rotAbout(camFwd, r, deg * kDeg), camEyeDir), -1.0, 1.0)) / kDeg;
        if (std::abs(el1) > kLim && std::abs(el1) > std::abs(el0))
        {
            if (std::abs(el0) >= kLim)
                return;
            deg *= (kLim - std::abs(el0)) / std::max(std::abs(el1) - std::abs(el0), 1e-9);
        }
    }
    camFwd = glm::normalize(rotAbout(camFwd, r, deg * kDeg)); // (+ about the right: the look turns toward the up)
    camUp = rotAbout(camUp, r, deg * kDeg);
    camUp = glm::normalize(camUp - glm::dot(camUp, camFwd) * camFwd);
    camPublish();
}

void SatelliteSim::camRoll(double deg)
{
    if (deg == 0.0)
        return;
    if (!camRigInit)
        camSync();
    camUp = rotAbout(camUp, camFwd, deg * kDeg); // (+ about the look: the up turns toward the right = banks right)
    camUp = glm::normalize(camUp - glm::dot(camUp, camFwd) * camFwd);
    camPublish();
}

void SatelliteSim::camSetRoll(double deg)
{
    if (!camRigInit)
        camSync();
    camRoll(std::remainder(deg - camRollDeg(), 360.0));
}

void SatelliteSim::camLevelTick(float dt)
{
    if (!camRigInit)
        camSync();
    // (No toast when the eye crosses the height: the user turned it down, 2026-11-14.)
    if (!camLevelActive())
        return;
    const double k = levelWeight(camFwd, camEyeDir);
    if (k <= 0.0)
        return;
    const double roll = camRollDeg();
    if (std::abs(roll) < 1e-6)
        return;
    double next = roll * std::exp(-(double)std::min(dt, 0.25f) * k / kCamLevelTauS);
    if (std::abs(next) < 0.01)
        next = 0.0;
    camRoll(next - roll);
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
    const glm::dvec3 &U = camEyeDir;
    camera.elDeg = (float)camElDeg();
    camera.azDeg = (float)(std::atan2(glm::dot(f, E), glm::dot(f, N)) / kDeg);
    camera.rollDeg = (float)camRollDeg();
    // ENU -> camera: rows right, up, back (glm::lookAt's), each in East / North / Up components.
    const glm::dvec3 r = glm::normalize(camRightEcef()), b = -camFwd;
    const glm::dvec3 rE(glm::dot(r, E), glm::dot(r, N), glm::dot(r, U));
    const glm::dvec3 uE(glm::dot(camUp, E), glm::dot(camUp, N), glm::dot(camUp, U));
    const glm::dvec3 bE(glm::dot(b, E), glm::dot(b, N), glm::dot(b, U));
    for (int j = 0; j < 3; ++j)
        camera.viewEnu[j] = glm::vec3((float)rE[j], (float)uE[j], (float)bE[j]);
    camera.viewSet = true;
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
        camUp = camEyeDir;
        camSetLook(glm::dvec3(obsFacing), camera.elDeg, 90.0, false);
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
            camAimAzEl(azDeg, el ? (double)elDeg : camElDeg(), 90.0, true);
        else if (el)
            camSetLook(camFacing(), elDeg);
        return;
    }
    camPublish();
}
