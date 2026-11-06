// CPU twin of shaders/include/wind_field.glsl (see WindField.h). Same constants, same order of terms.
#include "WindField.h"
#include <algorithm>
#include <cmath>

namespace wx {

namespace {
constexpr double kR   = 6371000.0;
constexpr double kD2R = 0.017453293;
constexpr double kTau = 6.2831853071795864;
double G(double x, double c, double s) { const double u = (x - c) / s; return std::exp(-u * u); }
double sstep(double e0, double e1, double x) { const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0); return t * t * (3.0 - 2.0 * t); }
}

WindState windState(double simT, double decl, double waves, double eddies)
{
    WindState s;
    s.clock = std::fmod(simT, kWfPeriod);
    if (s.clock < 0.0) s.clock += kWfPeriod;
    s.decl = decl; s.waves = waves; s.eddies = eddies;
    return s;
}

WindState windStateAt(const WindState &ws, double dt)
{
    WindState s = ws;
    s.clock = std::fmod(ws.clock + dt, kWfPeriod);
    if (s.clock < 0.0) s.clock += kWfPeriod;
    return s;
}

glm::dvec3 windAt(const glm::dvec3 &d, double h, const WindState &ws)
{
    const double lat   = std::asin(std::clamp(d.z, -1.0, 1.0));
    const double lon   = std::atan2(d.y, d.x);
    const double cl    = std::max(std::sqrt(d.x * d.x + d.y * d.y), 0.02);
    const glm::dvec3 east(-d.y / cl, d.x / cl, 0.0);
    const glm::dvec3 north = glm::cross(d, east);
    const double hemi  = lat >= 0.0 ? 1.0 : -1.0;
    const double hk    = h * 1e-3;
    const double tw    = ws.clock * (kTau / kWfPeriod);

    const double latS   = lat - 0.35 * ws.decl;
    const double a      = std::abs(latS) / kD2R;
    const double winter = std::clamp(-hemi * ws.decl / 0.41, -1.0, 1.0);
    const double lowK   = 1.0 - sstep(0.5, 7.0, hk);
    const double jetH   = sstep(0.0, 11.0, hk) * (1.0 - 0.45 * sstep(12.0, 20.0, hk));
    double u = -7.0 * G(a, 14.0, 10.0) * (0.25 + 0.75 * lowK)
             +  8.0 * G(a, 47.0, 11.0)
             -  3.5 * G(a, 72.0, 8.0) * lowK
             + (27.0 + 9.0 * winter) * G(a, 33.0 - 3.0 * winter, 10.0) * jetH
             -  7.0 * G(a, 0.0, 14.0) * sstep(8.0, 14.0, hk);
    double v = -2.5 * (latS >= 0.0 ? 1.0 : -1.0) * G(a, 12.0, 9.0) * (1.0 - 2.0 * sstep(1.5, 11.0, hk));

    const double c0 = 47.0 - 3.0 * winter;
    const double la = std::abs(lat) / kD2R;
    const double e0 = G(la, c0, 13.0);
    const double dE = e0 * (-2.0 * (la - c0) / (13.0 * 13.0)) * hemi / kD2R;
    const double A  = ws.waves * 6.0e6 * (0.45 + 0.75 * std::clamp(hk / 10.0, 0.0, 1.0)) * (1.0 + 0.3 * winter);
    static const double wm[5] = {4.0, 5.0, 6.0, 7.0, 9.0};
    static const double wl[5] = {3.0, -4.0, 6.0, -2.0, 5.0};
    static const double wn[5] = {3.0, 4.0, 4.0, 5.0, 7.0};
    static const double wq[5] = {5.0, 7.0, 3.0, 8.0, 6.0};
    static const double wp[5] = {0.4, 2.3, 4.1, 1.2, 5.5};
    static const double wa[5] = {1.0, 0.8, 0.65, 0.5, 0.4};
    double S = 0.0, Sl = 0.0, Sm = 0.0;
    const double hp = hemi > 0.0 ? 0.0 : 1.9;
    for (int k = 0; k < 5; ++k)
    {
        const double amp = wa[k] * (0.6 + 0.4 * std::cos(wq[k] * tw + wp[k] * 1.7 + hp));
        const double th  = wm[k] * lon + wl[k] * lat - wn[k] * tw + wp[k] + hp * double(k + 1);
        const double cs = std::cos(th), sn = std::sin(th);
        S  += amp * cs;
        Sl -= amp * wl[k] * sn;
        Sm -= amp * wm[k] * sn;
    }
    u += -(A / kR) * (dE * S + e0 * Sl);
    v +=  (A / (kR * cl)) * e0 * Sm;

    static const glm::dvec3 kd[6] = {{0.62, 0.48, 0.62}, {-0.71, 0.30, 0.64}, {0.18, -0.93, 0.32},
                                     {0.85, 0.21, -0.48}, {-0.34, -0.55, -0.76}, {0.05, 0.77, -0.64}};
    static const double ks[6] = {1.0, 1.13, 0.87, 1.31, 0.74, 1.21};
    static const double kn[6] = {17.0, 23.0, 19.0, 29.0, 13.0, 21.0};
    const double k0 = kTau * kR / 1.2e6;
    glm::dvec3 g(0.0);
    for (int i = 0; i < 6; ++i)
    {
        const glm::dvec3 kv = kd[i] * (k0 * ks[i] / glm::length(kd[i]));
        g += kv * (std::cos(glm::dot(kv, d) + double(i) * 1.37 + kn[i] * tw) / ks[i]);
    }
    const glm::dvec3 eddy = glm::cross(d, g) * (ws.eddies * 3.5 / (k0 * 1.73));

    return east * u + north * v + eddy;
}

glm::dvec2 windEN(const glm::dvec3 &d, double h, const WindState &ws)
{
    const glm::dvec3 w = windAt(d, h, ws);
    const double cl = std::max(std::sqrt(d.x * d.x + d.y * d.y), 0.02);
    const glm::dvec3 east(-d.y / cl, d.x / cl, 0.0);
    const glm::dvec3 north = glm::cross(d, east);
    return {glm::dot(w, east), glm::dot(w, north)};
}

glm::dvec3 backtrace(const glm::dvec3 &d0, double h, const WindState &ws, double dt, int steps)
{
    glm::dvec3 d = d0;
    const double s = dt / std::max(steps, 1);
    for (int i = 0; i < steps; ++i)
    {
        const WindState w0 = windStateAt(ws, -s * i);
        const glm::dvec3 v0 = windAt(d, h, w0);
        const glm::dvec3 mid = glm::normalize(d - v0 * (0.5 * s / kR));
        const glm::dvec3 v1 = windAt(mid, h, windStateAt(ws, -s * (i + 0.5)));
        d = glm::normalize(d - v1 * (s / kR));
    }
    return d;
}

} // namespace wx
