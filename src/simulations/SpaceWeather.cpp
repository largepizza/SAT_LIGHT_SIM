#include "SpaceWeather.h"

#include <algorithm>
#include <cmath>
#include <cstdint>

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kDay = 86400.0;
constexpr double kRotationDays = 27.27;      // synodic solar rotation (Carrington)
constexpr double kCycleMinYear = 2019.96;    // solar cycle 25 minimum (Dec 2019)
constexpr double kCycleYears = 11.0;

// splitmix64 of (a, salt) -> three uniforms in [0, 1).
uint64_t mix64(uint64_t x) {
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
struct H3 { double x, y, z; };
H3 hash3(int64_t a, uint64_t salt) {
    uint64_t h = mix64((uint64_t)a * 0x2545F4914F6CDD1Dull ^ mix64(salt));
    uint64_t h2 = mix64(h ^ 0xA5A5A5A5u), h3 = mix64(h2 ^ 0x5A5A5A5Au);
    const double k = 1.0 / 9007199254740992.0; // 2^-53
    return {(double)(h >> 11) * k, (double)(h2 >> 11) * k, (double)(h3 >> 11) * k};
}
// Smooth 1D value noise in [0, 1].
double vnoise(double x, uint64_t salt) {
    double f = std::floor(x), u = x - f;
    u = u * u * (3.0 - 2.0 * u);
    double a = hash3((int64_t)f, salt).x, b = hash3((int64_t)f + 1, salt).x;
    return a + (b - a) * u;
}
double smooth01(double e0, double e1, double x) {
    double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

// Fractional year and day of year from seconds since J2000 (2000-01-01 12:00 TT ~ UTC here).
double yearOf(double t) { return 2000.0 + (t / kDay + 0.5) / 365.2422; }
double dayOfYear(double t) {
    double y = (t / kDay + 0.5) / 365.2422;
    return (y - std::floor(y)) * 365.2422;
}

double cycleEnvelope(double t) {
    double p = (yearOf(t) - kCycleMinYear) / kCycleYears;
    p -= std::floor(p);
    // Rises to maximum ~40% into the cycle, declines more slowly.
    double s = std::sin(kPi * std::pow(p, 0.75));
    return 0.15 + 0.85 * s * s;
}

// Russell-McPherron: the solar wind's southward field couples best near the equinoxes.
double equinoxFactor(double t) {
    return 1.0 + 0.3 * std::cos(4.0 * kPi * (dayOfYear(t) - 80.0) / 365.2422);
}

// A CME's peak Kp from a uniform u: the survival function through (Kp, P(peak >= Kp)) knots, log-linear
// between them. Calibrated with the solar-cycle rate above against NOAA's per-cycle counts of 3-h intervals
// (G1 1700, G2 600, G3 200, G4 100, G5 4): most CMEs give a minor storm, G4 a few a year near maximum,
// G5 one or two per cycle, a Carrington-class (Kp-equivalent 11) once in many cycles.
double cmePeakKp(double u) {
    static const double kp[] = {4.3, 6.0, 7.0, 8.0, 9.0, 10.0, 11.3};
    static const double sv[] = {1.0, 0.40, 0.17, 0.075, 0.004, 0.0006, 0.00006};
    const double s = std::max(1.0 - u, 1e-6); // survival
    for (int i = 0; i < 6; ++i) {
        if (s >= sv[i + 1]) {
            const double f = std::log(s / sv[i]) / std::log(sv[i + 1] / sv[i]);
            return kp[i] + (kp[i + 1] - kp[i]) * f;
        }
    }
    return kp[6];
}

struct Parts { double quiet, hss, cme, cmePeak, cmeHours, cycle; };

Parts activityParts(double t, double rateScale) {
    Parts P{};
    const double d = t / kDay;
    const double sc = cycleEnvelope(t);
    const double rm = equinoxFactor(t);
    P.cycle = sc;
    rateScale = std::max(rateScale, 0.0);

    // Background: ~Kp 0.5-2.5, wandering over half a day, plus 3-hourly jitter.
    P.quiet = 0.5 + 1.6 * sc * vnoise(d * 2.0, 11) + 0.6 * (vnoise(d * 8.0, 12) - 0.5);

    // Recurrent high-speed streams: two hole slots, each at a fixed Carrington phase for ~6 rotations.
    for (int k = 0; k < 2; ++k) {
        const double off = (k == 0) ? 0.13 : 0.61;
        const double rf = d / kRotationDays - off;
        const int64_t r0 = (int64_t)std::floor(rf);
        for (int64_t r = r0 - 1; r <= r0; ++r) {
            const int64_t epoch = (int64_t)std::floor((double)r / 6.0);
            const H3 he = hash3(epoch, 100 + k);
            const double exist = std::min(0.9, (0.4 + 0.4 * sc) * std::sqrt(rateScale));
            if (he.x >= exist) continue;
            const H3 hr = hash3(r, 200 + k);
            const double arrive = ((double)r + off + 0.3 * he.y) * kRotationDays + 0.6 * hr.x;
            const double x = d - arrive;
            if (x < 0.0 || x > 8.0) continue;
            const double peak = (3.3 + 3.0 * he.z) * (0.85 + 0.3 * hr.y) * std::sqrt(rm);
            const double v = peak * smooth01(0.0, 0.6, x) * std::exp(-std::max(x - 0.6, 0.0) / 2.2);
            P.hss = std::max(P.hss, v);
        }
    }

    // CME storms: one chance per day; ~1.7 a month at maximum (x the equinox bias).
    const int64_t b0 = (int64_t)std::floor(d);
    const double pDay = 0.15 * sc * rm * rateScale;
    for (int64_t b = b0 - 5; b <= b0; ++b) {
        const H3 h = hash3(b, 300);
        if (h.x >= pDay) continue;
        const H3 h2 = hash3(b, 301);
        const double arrive = (double)b + h.y;
        const double x = d - arrive;
        if (x < 0.0) continue;
        const double peak = cmePeakKp(h.z);
        const double rise = 0.12 + 0.25 * h2.x;              // main phase 3-9 h
        const double hold = 0.1 + 0.45 * h2.y;               // 2.5-13 h near peak
        const double tau = (0.5 + 0.8 * h2.z) * (1.0 + 0.08 * (peak - 5.0)); // recovery
        // Sudden commencement: a jump to ~half the peak within ~40 min of arrival, then the main phase.
        double ramp = std::max(0.5 * smooth01(0.0, 0.03, x), smooth01(0.0, rise, x));
        double decay = (x > rise + hold) ? std::exp(-(x - rise - hold) / tau) : 1.0;
        // Storm-time intermittency on a ~1.5-h scale.
        const double flick = 0.7 * (vnoise(d * 16.0, 400 + (uint64_t)(b & 0xFFFF)) - 0.5);
        const double v = std::max(peak * ramp * decay + flick * ramp, 0.0);
        if (v > P.cme) {
            P.cme = v;
            P.cmePeak = peak;
            P.cmeHours = x * 24.0;
        }
    }
    return P;
}

double combine(const Parts &P) {
    // A smooth max: the strongest driver dominates, a second one adds a little.
    double a = std::max({P.quiet, P.hss, P.cme});
    double s = std::exp(P.quiet - a) + std::exp(P.hss - a) + std::exp(P.cme - a);
    double kp = a + 0.2 * std::log(s);
    return std::clamp(kp, 0.0, 11.5);
}

} // namespace

double spaceWeatherKp(double t, double rateScale) { return combine(activityParts(t, rateScale)); }

SpaceWeather spaceWeatherAt(double t, double rateScale, double kpOverride) {
    SpaceWeather sw;
    const Parts P = activityParts(t, rateScale);
    sw.kp = combine(P);
    sw.kpQuiet = P.quiet;
    sw.hss = P.hss;
    sw.cme = P.cme;
    sw.cmePeakKp = P.cme > 0.5 ? P.cmePeak : 0.0;
    sw.cmeHoursSince = P.cmeHours;
    sw.cycle = P.cycle;
    if (kpOverride >= 0.0) sw.kp = std::min(kpOverride, 11.5);

    // Substorms: one chance per 2.5-h bin, likelier and stronger with activity; ~2.5 h each.
    const double binS = 2.5 * 3600.0;
    const int64_t j0 = (int64_t)std::floor(t / binS);
    for (int64_t j = j0 - 2; j <= j0; ++j) {
        const H3 h = hash3(j, 500);
        const double kpBin = (kpOverride >= 0.0) ? sw.kp : spaceWeatherKp((double)j * binS, rateScale);
        const double pOcc = 0.35 + 0.6 * smooth01(1.0, 4.0, kpBin);
        if (h.x >= pOcc) continue;
        const H3 h2 = hash3(j, 501);
        const double onset = ((double)j + 0.9 * h.y) * binS;
        const double xm = (t - onset) / 60.0; // minutes
        if (xm < 0.0 || xm > 240.0) continue;
        const double strength = (0.35 + 0.65 * h.z) * (0.5 + 0.12 * std::min(kpBin, 9.0));
        // Expansion: explosive brightening (~5 min), the bulge grows over ~30 min; recovery ~70 min.
        const double bright = 1.0 - std::exp(-xm / 5.0);
        const double grow = 1.0 - std::exp(-xm / 18.0);
        const double rec = (xm > 30.0) ? std::exp(-(xm - 30.0) / (55.0 + 30.0 * h2.x)) : 1.0;
        const double I = strength * bright * rec;
        if (I <= sw.subI) continue;
        sw.subI = I;
        sw.subMinutes = xm;
        const double onsetMlt = (22.8 + 1.4 * (h2.y - 0.5)) - 24.0; // hours from midnight (negative = dusk)
        // The westward travelling surge carries the bulge's dusk end; its centre drifts ~0.7 h west.
        const double mltH = onsetMlt - 0.7 * grow;
        sw.subMltRad = mltH * kPi / 12.0;
        sw.subHalfWRad = (0.8 + 2.4 * grow) * kPi / 12.0;
        sw.subPoleDeg = (2.0 + 4.5 * strength) * grow * rec;
    }
    return sw;
}

AuroraOvalBounds auroraOvalBounds(double kp) {
    const double a = std::clamp(kp, 0.0, 11.5);
    AuroraOvalBounds b;
    b.eqMid = (float)(90.0 - (66.5 - 2.1 * a));
    b.eqNoon = (float)(90.0 - (76.5 - 1.0 * a));
    b.polMid = (float)(90.0 - (72.5 - 0.8 * a));
    b.polNoon = (float)(90.0 - (79.0 - 0.6 * a));
    return b;
}

glm::dvec3 geomagPoleEcef() { return glm::normalize(glm::dvec3(0.0481, -0.1543, 0.9868)); }

AuroraGpu auroraGpuParams(const SpaceWeather &sw, const glm::dvec3 &sunEcef) {
    AuroraGpu g;
    const glm::dvec3 ax = geomagPoleEcef();
    glm::dvec3 m = -sunEcef - ax * glm::dot(-sunEcef, ax);
    m = (glm::length(m) > 1e-9) ? glm::normalize(m) : glm::dvec3(1, 0, 0);
    g.midnight = glm::vec4(glm::vec3(m), (float)sw.kp);
    const AuroraOvalBounds b = auroraOvalBounds(sw.kp);
    g.oval = glm::vec4(b.eqMid, b.eqNoon, b.polMid, b.polNoon);
    const double hw = std::max(sw.subHalfWRad, 0.05);
    g.sub = glm::vec4((float)sw.subI, (float)std::cos(sw.subMltRad), (float)std::sin(sw.subMltRad),
                      (float)(1.0 / (hw * hw)));
    // Brightness from activity: 1 at Kp 3 (the look the curtain was tuned at), dim when quiet.
    const double bright = std::clamp(0.4 + 0.2 * sw.kp, 0.25, 2.6);
    const double ripple = 1.5;
    const double maxColat = b.eqMid + 0.8 * std::max(sw.subI, 0.0) + 3.5 + ripple + 1.0;
    g.oval2 = glm::vec4((float)bright, (float)maxColat, (float)ripple, (float)sw.subPoleDeg);
    return g;
}

int geomagStormScale(double kp) {
    if (kp < 5.0) return 0;
    return std::min(5, (int)std::floor(kp) - 4);
}

// Mirror of shaders/include/aurora_oval.glsl (auroraOvalGeom + auroraOvalBand), without the ripple.
float auroraOvalWeightCpu(const AuroraGpu &g, const glm::dvec3 &dir) {
    const glm::dvec3 ax = geomagPoleEcef();
    const glm::dvec3 pole = glm::dot(dir, ax) > 0.0 ? ax : -ax;
    const double colat = glm::degrees(std::acos(std::clamp(glm::dot(dir, pole), -1.0, 1.0)));
    if (colat > g.oval2.y) return 0.0f;
    const glm::dvec3 m(g.midnight), e = glm::cross(ax, m);
    double c = glm::dot(dir, m), s = glm::dot(dir, e);
    const double r = std::max(std::hypot(c, s), 1e-4);
    c /= r; s /= r;
    const double cosMlt = c * 0.9664 - s * 0.2571;
    const double w = 0.5 + 0.5 * cosMlt;
    double eq = g.oval.y + (g.oval.x - g.oval.y) * w;
    double pol = g.oval.w + (g.oval.z - g.oval.w) * w;
    const double cd = c * g.sub.y + s * g.sub.z;
    const double bb = std::exp((cd - 1.0) * g.sub.w);
    const double boost = g.sub.x * bb;
    pol = std::max(pol - g.oval2.w * bb, 2.0);
    eq += 0.8 * boost;
    const double inner = smooth01(pol - 0.8, pol + 1.2, colat);
    const double outer = 1.0 - smooth01(eq - 2.0, eq + 3.5, colat);
    const double mltB = 0.3 + 0.7 * smooth01(-0.2, 0.7, cosMlt);
    return (float)(inner * outer * mltB * g.oval2.x * (1.0 + 1.5 * boost));
}
