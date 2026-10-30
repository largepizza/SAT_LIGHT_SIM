// The solar corona's structure (2026-10-28, issue 20261028_220320 "Sun corona work": "mirrored symmetric ... make it
// more dynamic, evolution that responds to solar Kp; the coronagraph (cover Sun) should show it too").
//
// A pure function of sim time (time warp, reversal and bookmarks see the same corona), sent each frame in the refraction
// block (GpuRefraction::cor*) and drawn by sat_sky.frag solarCoronaDyn (full variants only: SKY_LITE keeps the old
// symmetric corona, its size budget):
//   - the SOLAR CYCLE (SpaceWeather's envelope) sets the shape: near minimum a flattened corona, its streamers in a belt
//     along a nearly flat current sheet, polar coronal holes full of plumes; near maximum a round corona, the sheet
//     warped up to ~70 deg, streamers at every latitude;
//   - the current sheet (the neutral line) is a warped ring in Carrington longitude; its latitude where it crosses
//     the east and west limbs is where the stalks of that limb converge, and it moves as the Sun turns (27.27 days);
//   - helmet streamers are slots living 2-5 rotations each, on the sheet near minimum, anywhere near maximum, plus the
//     largest sunspot groups near the limbs (fillSunspots): each is projected at its heliographic place, so it rises
//     at the east limb, fades as it turns toward us and comes back on the west limb half a rotation later;
//   - CMEs: every Earth-directed CME behind a storm of the space-weather model (spaceWeatherCmeArrivals: the same
//     hashes as the Kp) leaves the Sun as a HALO a transit time before it arrives (the Sun's Fury: at its X-class
//     flare, 17.5 h before); limb CMEs (the three-part bubble: a bright loop, a dark cavity, a bright core) come at a
//     rate following the cycle and the geomagnetic activity (of their own launch bin), and cross the 24-radius field in
//     ~3-18 h, fading in over their first hour and out over the field's outer half; they push the corona aside (the
//     shader warps it) and blow out the streamers they cross, which reform over a day (2026-10-30, issue
//     20261030_141422). Every list is capped softly (softCap): an entry crossing the cut fades rather than pops.
#include "SatelliteSim.h"
#include "SpaceWeather.h"
#include "../FrameProfile.h"
#include <algorithm>
#include <cmath>
#include <nlohmann/json.hpp>

namespace
{
constexpr double kPiC = 3.14159265358979323846;
constexpr double kTwoPiC = 2.0 * kPiC;
constexpr double kDayC = 86400.0;
constexpr double kRotSynDays = 27.2753;     // synodic Carrington rotation
constexpr double kRsunKm = 696000.0;
constexpr int kStrMax = 12, kCmeMax = 6;

uint64_t cmix(uint64_t x)
{
    x += 0x9E3779B97F4A7C15ull;
    x = (x ^ (x >> 30)) * 0xBF58476D1CE4E5B9ull;
    x = (x ^ (x >> 27)) * 0x94D049BB133111EBull;
    return x ^ (x >> 31);
}
double ch(int64_t a, int64_t b, uint32_t salt)
{
    const uint64_t h = cmix((uint64_t)a * 0x2545F4914F6CDD1Dull ^ cmix((uint64_t)b * 0x9E3779B97F4A7C15ull + salt));
    return (double)(h >> 11) * (1.0 / 9007199254740992.0);
}
double cnoise(double x, uint32_t salt)
{
    const double f = std::floor(x);
    double u = x - f;
    u = u * u * (3.0 - 2.0 * u);
    const double a = ch((int64_t)f, 0, salt), b = ch((int64_t)f + 1, 0, salt);
    return a + (b - a) * u;
}
double sm01(double e0, double e1, double x)
{
    const double t = std::clamp((x - e0) / (e1 - e0), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}
double wrapPiC(double a) { return a - kTwoPiC * std::floor((a + kPiC) / kTwoPiC); }

struct Str { glm::vec4 a, b; double rank; };
struct Cme { glm::vec4 a, b; double rank; };

// Activity 0..1 from the cycle's envelope (0.15 minimum .. 1 maximum).
double activityOf(double t) { return std::clamp((solarCycleEnvelope(t) - 0.15) / 0.85, 0.0, 1.0); }

// The current sheet's heliographic latitude at Carrington longitude L (rad): a tilted ring, warped near maximum.
double sheetLat(double L, double days, double act)
{
    const double tilt = glm::radians(5.0 + 62.0 * act) * (0.75 + 0.5 * cnoise(days / 90.0, 41u));
    const double L0 = kTwoPiC * cnoise(days / 160.0, 42u) * 2.0;
    const double L1 = kTwoPiC * cnoise(days / 70.0, 43u) * 2.0;
    double b = tilt * std::sin(L - L0) + 0.45 * act * tilt * std::sin(2.0 * L - L1);
    return std::clamp(b, -glm::radians(75.0), glm::radians(75.0));
}

// The sky place of a heliographic point (lat, longitude from the central meridian, west +): apparent latitude on its
// limb, side, plane-of-sky factor.
void projectHelio(double lat, double lonCM, double &ell, double &side, double &p)
{
    const double x = std::cos(lat) * std::sin(lonCM), y = std::sin(lat);
    p = std::sqrt(x * x + y * y);
    side = x >= 0.0 ? 1.0 : -1.0;
    ell = std::atan2(y, std::abs(x));
}

// CME speed (km/s) from its storm's peak Kp: ~600 km/s for a minor storm to ~2400 for a Carrington-class one.
double cmeSpeedKms(double peakKp) { return 600.0 * std::pow(4.0, std::clamp((peakKp - 4.3) / 6.7, 0.0, 1.0)); }
} // namespace

// Fade the entries ranked past `cap` in smoothly: an entry's amplitude goes to 0 as its rank value falls to the first
// one left out, so a candidate crossing the cut fades instead of popping (2026-10-30).
template <class T> int softCap(T *v, int n, int cap)
{
    std::sort(v, v + n, [](const T &p, const T &q) { return p.rank > q.rank; });
    if (n <= cap)
        return n;
    const double cut = v[cap].rank;
    for (int i = 0; i < cap; ++i)
        v[i].a.w *= (float)sm01(cut, cut * 1.5 + 1e-6, v[i].rank);
    return cap;
}

void SatelliteSim::fillCorona(double simT)
{
    FPROF_SCOPE("corona");
    GpuRefraction &g = refrGpu;
    const double days = simT / kDayC;
    const double act = activityOf(simT);
    // The Sun's rotation: a feature at Carrington longitude L is at L + phi from the central meridian (west +).
    const double phi = kTwoPiC * (days / kRotSynDays - std::floor(days / kRotSynDays));
    const double betaE = sheetLat(-0.5 * kPiC - phi, days, act), betaW = sheetLat(0.5 * kPiC - phi, days, act);

    // ── CMEs ──
    // Every candidate is a pure function of its own launch time (the Kp of ITS bin, not the Kp now: a CME in flight
    // read the current Kp and popped in and out as it changed). Their amplitude fades in over the first hour after
    // launch and out between 12 and 24 solar radii; `push` (corCme[2c+1].w) is how hard it sweeps the corona aside.
    // Limb CMEs up to 36 h old are kept in `blow` for the streamers they cross (blown out, reforming over a day).
    Cme cs[48];
    int m = 0;
    struct Blow { double psi, hw, dt, R; };
    Blow blow[24];
    int nb = 0;
    const double kRmax = 24.0;
    // Earth-directed: the storms arriving within the next 4 days, leaving the Sun a transit time before.
    {
        CmeArrival arr[16];
        const int na = spaceWeatherCmeArrivals(simT - 2.0 * kDayC, simT + 4.0 * kDayC, auroraStormRate, arr, 16);
        for (int i = 0; i < na && m < 48; ++i)
        {
            const double v = cmeSpeedKms(arr[i].peakKp);
            const double transit = arr[i].scripted ? 17.5 * 3600.0 : 1.496e8 / v * 1.15;   // (deceleration)
            const double dt = simT - (arr[i].arriveT - transit);
            if (dt < 0.0) continue;
            const double dR = v * dt / kRsunKm;                    // distance travelled toward us (solar radii)
            const int64_t sd = (int64_t)std::floor(arr[i].arriveT);
            const double hw = glm::radians(45.0 + 20.0 * ch(sd, 0, 511u));
            const double ring = (1.0 + dR) * std::sin(hw);        // the halo's edge on the sky
            if (ring > kRmax) continue;
            // the source on the disc: the Sun's Fury's flare region (14 N near the central meridian), else near centre
            double sLat = arr[i].scripted ? glm::radians(14.0) : glm::radians(30.0 * (ch(sd, 1, 511u) - 0.5));
            double sLon = arr[i].scripted ? 0.0 : glm::radians(40.0 * (ch(sd, 2, 511u) - 0.5));
            const double x = std::cos(sLat) * std::sin(sLon), y = std::sin(sLat);
            const double psi = std::atan2(x, y);
            const double out = 1.0 - sm01(0.5 * kRmax, kRmax, ring);
            const double in = sm01(0.0, 3600.0, dt) * sm01(0.8, 1.6, ring);
            const double amp = (0.5 + 0.3 * (arr[i].peakKp - 4.3)) * in * out;
            if (amp < 0.005) continue;
            cs[m++] = {glm::vec4((float)psi, (float)hw, (float)ring, (float)amp),
                       glm::vec4(1.0f, (float)(sd % 997), (float)std::min(0.3 + 0.7 * std::hypot(x, y), 1.0),
                                 (float)(in * out)),
                       10.0 + amp};
        }
    }
    // Limb CMEs: one chance per 3-h bin, more near maximum and in geomagnetically active times (that bin's Kp).
    {
        const double binS = 3.0 * 3600.0;
        const int64_t b0 = (int64_t)std::floor(simT / binS);
        for (int64_t b = b0 - 12; b <= b0; ++b)
        {
            const double kpB = spaceWeatherKp((double)b * binS, auroraStormRate);
            const double ratePerDay = 0.5 + 4.0 * act + 3.0 * sm01(4.0, 7.0, kpB);
            const double pBin = std::min(ratePerDay * binS / kDayC, 0.9);
            if (ch(b, 0, 521u) >= pBin) continue;
            const double tL = ((double)b + ch(b, 1, 521u)) * binS;
            const double dt = simT - tL;
            if (dt < 0.0) continue;
            const double u = ch(b, 2, 521u);
            const double v = 250.0 + 1250.0 * u * u;
            const double R = 1.0 + v * dt / kRsunKm;
            const double lat = glm::radians((2.0 * ch(b, 3, 521u) - 1.0) * (25.0 + 40.0 * act));
            const double defl = 0.4 * (1.0 - act);              // polar fields deflect them toward the equator
            const double latS = lat * (1.0 - defl);
            const double side = ch(b, 4, 521u) < 0.5 ? -1.0 : 1.0;
            const double psi = std::atan2(side * std::cos(latS), std::sin(latS));
            const double hw = glm::radians(15.0 + 35.0 * ch(b, 5, 521u));
            if (nb < 24 && dt < 36.0 * 3600.0)
                blow[nb++] = {psi, hw, dt, R};
            if (R > kRmax) continue;
            // in: over the first hour (and the loop's rise off the limb); out: over the outer half of the field
            const double in = sm01(0.0, 3600.0, dt) * sm01(1.0, 1.5, R);
            const double out = 1.0 - sm01(0.5 * kRmax, kRmax, R);
            const double amp = (0.5 + 0.9 * ch(b, 6, 521u)) * std::sqrt(hw / glm::radians(30.0)) * in * out;
            if (amp < 0.005) continue;
            cs[m++] = {glm::vec4((float)psi, (float)hw, (float)R, (float)amp),
                       glm::vec4(0.0f, (float)(b % 997), (float)(0.4 + 0.6 * ch(b, 7, 521u)), (float)(in * out)), amp};
        }
    }
    m = softCap(cs, m, kCmeMax);
    for (int i = 0; i < m; ++i)
    {
        g.corCme[2 * i] = cs[i].a;
        g.corCme[2 * i + 1] = cs[i].b;
    }
    coronaCmes = m;

    // ── Streamers ──
    // A streamer a limb CME crossed is blown out (to 25%) as the front passes its base and reforms over the next day.
    auto blownOut = [&](double ell, double side) {
        const double psiS = std::atan2(side * std::cos(ell), std::sin(ell));
        double keep = 1.0;
        for (int i = 0; i < nb; ++i)
        {
            const double d = wrapPiC(psiS - blow[i].psi) / blow[i].hw;
            const double hit = std::exp(-0.5 * d * d) * sm01(1.2, 3.0, blow[i].R) * (1.0 - sm01(8.0 * 3600.0, 30.0 * 3600.0, blow[i].dt));
            keep *= 1.0 - 0.75 * hit;
        }
        return keep;
    };
    // The latitude a streamer's stalk bends toward: its limb's neutral line, blended across the central meridian
    // (a streamer turning through it switched limbs, and its stalk jumped to the other limb's sheet).
    // Sent as POSITION ANGLES (base, target), so the shader's loop has no trig; near the central meridian the stalk does
    // not bend (bendK), since a streamer turning through it changes limb and its target would jump to the other side.
    auto sheetTarget = [&](double lat, double lonCM, double side) {
        const double x = std::cos(lat) * std::sin(lonCM);
        const double tgt = betaE + (betaW - betaE) * sm01(-0.3, 0.3, x);
        return std::atan2(side * std::cos(tgt), std::sin(tgt));
    };
    auto basePsi = [](double ell, double side) { return std::atan2(side * std::cos(ell), std::sin(ell)); };
    auto bendK = [](double lat, double lonCM) { return sm01(0.05, 0.3, std::abs(std::cos(lat) * std::sin(lonCM))); };
    Str ss[32];
    int n = 0;
    constexpr int kSlots = 11;
    for (int k = 0; k < kSlots; ++k)
    {
        const double E = (2.0 + 3.0 * ch(k, 0, 501u)) * kRotSynDays;   // epoch length (days)
        const double x = days / E + ch(k, 1, 501u);
        const int64_t e = (int64_t)std::floor(x);
        const double f = x - (double)e;
        const double env = sm01(0.0, 0.15, f) * (1.0 - sm01(0.85, 1.0, f));
        if (env < 0.02) continue;
        if (ch(k, e, 502u) > 0.5 + 0.45 * act) continue;
        const double L = kTwoPiC * ch(k, e, 503u);
        const bool free = ch(k, e, 504u) < act;          // near maximum most are not on the sheet
        double lat;
        if (free)
            lat = glm::radians(70.0 * (2.0 * ch(k, e, 505u) - 1.0) * (0.3 + 0.7 * act));
        else
            lat = sheetLat(L, days, act) + glm::radians(10.0 * (ch(k, e, 505u) - 0.5));
        double ell, side, p;
        const double lonCM = wrapPiC(L + phi);
        projectHelio(lat, lonCM, ell, side, p);
        const double w = sm01(0.3, 0.95, p);
        if (w < 0.02) continue;
        const double amp = (0.45 + 1.0 * ch(k, e, 506u)) * env * w * blownOut(ell, side);
        const double wb = 0.16 + 0.2 * ch(k, e, 507u) + 0.18 * (1.0 - act) * (free ? 0.0 : 1.0);
        const double wsk = 0.05 + 0.06 * ch(k, e, 508u);
        const double cusp = (2.6 - 0.8 * act) + 0.6 * (ch(k, e, 509u) - 0.5);
        const double bend = free ? 0.15 + 0.35 * (1.0 - act) : 0.85 - 0.5 * act;
        ss[n++] = {glm::vec4((float)basePsi(ell, side), (float)side, (float)sheetTarget(lat, lonCM, side), (float)amp),
                   glm::vec4((float)wb, (float)wsk, (float)cusp, (float)(bend * bendK(lat, lonCM))), amp};
    }
    // Over the largest active regions near the limbs (the sunspot groups fillSunspots sent this frame), faded in by
    // size x envelope (a hard cut at 0.15 popped them).
    for (int i = 0; i < sunspotGroups && n < 32; ++i)
    {
        const glm::vec4 a = g.spots[2 * i], b = g.spots[2 * i + 1];
        const double size = b.w, env = b.y;
        const double sk = sm01(0.08, 0.25, size * env);
        if (sk <= 0.0) continue;
        double ell, side, p;
        projectHelio(a.x, a.y, ell, side, p);
        const double w = sm01(0.45, 0.95, p);
        if (w < 0.02) continue;
        const double amp = (0.5 + 1.3 * size) * env * w * sk * blownOut(ell, side);
        ss[n++] = {glm::vec4((float)basePsi(ell, side), (float)side, (float)sheetTarget(a.x, a.y, side), (float)amp),
                   glm::vec4((float)(0.1 + 0.14 * size), 0.035f, (float)(1.6 + 0.4 * size), (float)(0.3 * bendK(a.x, a.y))), amp};
    }
    n = softCap(ss, n, kStrMax);
    for (int i = 0; i < n; ++i)
    {
        g.corStr[2 * i] = ss[i].a;
        g.corStr[2 * i + 1] = ss[i].b;
    }
    coronaStreamers = n;

    const bool occ = inertialOcculter;
    // The fine structure's phase mod 4: every rate the shader multiplies it by is a multiple of 1/4, so the wrap is
    // seamless (as a plain fract the 0.25x and 0.5x reads jumped every 14 days).
    const double ph4 = days * 0.07 / 4.0;
    g.corInfo = glm::vec4((float)act, (float)(4.0 * (ph4 - std::floor(ph4))), (float)n, occ ? 1.0f : 0.0f);
    g.corInfo2 = glm::vec4((float)m, (float)betaE, (float)betaW, coronaDisplayLift);
}

nlohmann::json SatelliteSim::coronaJson() const
{
    const GpuRefraction &g = refrGpu;
    nlohmann::json j;
    j["activity"] = g.corInfo.x;
    j["occulter"] = g.corInfo.w > 0.5f;
    j["neutral_line_lat_deg_east_west"] = {glm::degrees(g.corInfo2.y), glm::degrees(g.corInfo2.z)};
    nlohmann::json st = nlohmann::json::array();
    for (int i = 0; i < coronaStreamers; ++i)
    {
        const glm::vec4 a = g.corStr[2 * i], b = g.corStr[2 * i + 1];
        st.push_back({{"pa_deg", glm::degrees(a.x)}, {"side", a.y > 0.0f ? "W" : "E"}, {"stalk_pa_deg", glm::degrees(a.z)}, {"amp", a.w},
                      {"base_hw_deg", glm::degrees(b.x)}, {"cusp_r", b.z}, {"bend", b.w}});
    }
    j["streamers"] = st;
    nlohmann::json cm = nlohmann::json::array();
    for (int i = 0; i < coronaCmes; ++i)
    {
        const glm::vec4 a = g.corCme[2 * i], b = g.corCme[2 * i + 1];
        cm.push_back({{"pa_deg", glm::degrees(a.x)}, {"half_width_deg", glm::degrees(a.y)}, {"front_r", a.z},
                      {"amp", a.w}, {"halo", b.x > 0.5f}});
    }
    j["cmes"] = cm;
    j["kp"] = spaceWeather_.kp;
    j["display_lift"] = coronaDisplayLift;
    return j;
}
