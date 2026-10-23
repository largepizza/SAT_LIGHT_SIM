#include "StormClimatology.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <thread>

namespace wx {

namespace {

constexpr double kPi = 3.14159265358979323846;
constexpr double kYearDays = 365.25;

double sstep(double a, double b, double x) {
    double t = std::clamp((x - a) / (b - a), 0.0, 1.0);
    return t * t * (3.0 - 2.0 * t);
}

double lonDiff(double a, double b) {
    double d = std::fmod(a - b + 540.0, 360.0);
    if (d < 0.0) d += 360.0;
    return d - 180.0;
}

// Elliptic Gaussian in degrees: every regional feature is one of these, so nothing has a hard edge.
double blob(double lat, double lon, double lat0, double lon0, double sLat, double sLon) {
    double a = (lat - lat0) / sLat, b = lonDiff(lon, lon0) / sLon;
    return std::exp(-0.5 * (a * a + b * b));
}

// +1 at the June solstice + lag, -1 at the December one + lag.
double seasonSin(double doy, double lagDays) {
    return std::sin(2.0 * kPi * (doy - 79.5 - lagDays) / kYearDays);
}

// 1 at northern midsummer + lag, 0 at midwinter.
double nhSummer(double doy, double lagDays) { return 0.5 * (1.0 + seasonSin(doy, lagDays)); }

// Peak at a day of year, cos^n shaped (n even-ish via the 0.5(1+cos) form), period one year.
double seasonPeak(double doy, double peakDoy, double sharp) {
    double c = 0.5 * (1.0 + std::cos(2.0 * kPi * (doy - peakDoy) / kYearDays));
    return std::pow(c, sharp);
}

double besselI0(double x) {
    double sum = 1.0, term = 1.0, q = 0.25 * x * x;
    for (int k = 1; k < 30; ++k) {
        term *= q / double(k * k);
        sum += term;
        if (term < 1e-12 * sum) break;
    }
    return sum;
}

// Mean 1 over 24 h.
double vonMises24(double hour, double peakHour, double k) {
    return std::exp(k * std::cos(2.0 * kPi * (hour - peakHour) / 24.0)) / besselI0(k);
}

// Regions whose storms organise into nocturnal mesoscale systems (or, over the two lakes, the land breeze
// converging over the water at night): 0..1.
double nocturnalWeight(double lat, double lon) {
    double w = 0.0;
    w = std::max(w, 0.85 * blob(lat, lon, 38.0, -97.5, 6.0, 6.5));    // US Great Plains
    w = std::max(w, 0.80 * blob(lat, lon, -28.5, -60.0, 5.5, 6.0));   // La Plata basin
    w = std::max(w, 0.55 * blob(lat, lon, 13.0, 3.0, 3.5, 16.0));     // Sahel squall lines
    w = std::max(w, 0.75 * blob(lat, lon, 24.5, 90.5, 2.5, 3.5));     // Bangladesh / NE India
    w = std::max(w, 0.90 * blob(lat, lon, 9.7, -71.6, 0.9, 0.9));     // Lake Maracaibo (Catatumbo)
    w = std::max(w, 0.80 * blob(lat, lon, -1.0, 32.9, 1.0, 1.1));     // Lake Victoria
    return w;
}

// Mean 1 over a day each: land (afternoon), nocturnal-MCS land, sea (weak early-morning maximum).
double landCurve(double hour) { return 0.25 + 0.75 * vonMises24(hour, 15.8, 2.0); }
double nightCurve(double hour) { return 0.30 + 0.70 * vonMises24(hour, 1.5, 1.6); }
double seaCurve(double hour) { return 1.0 + 0.22 * std::cos(2.0 * kPi * (hour - 5.0) / 24.0); }

double combineDiurnal(double landC, double nightC, double seaC, double noc, double land) {
    // The two lakes are water but behave as their region does; elsewhere the night peak needs land.
    double landish = std::max(land, noc > 0.5 ? noc : 0.0);
    double overLand = landC + (nightC - landC) * noc;
    return seaC + (overLand - seaC) * landish;
}

double diurnalFactor(double lat, double lon, double hour, double land) {
    return combineDiurnal(landCurve(hour), nightCurve(hour), seaCurve(hour), nocturnalWeight(lat, lon), land);
}

double landDensity(double lat, double lon, double doy) {
    double decL = 23.44 * seasonSin(doy, 30.0);
    double nhS = nhSummer(doy, 25.0);

    // South Asia's monsoon reaches 25-30 N; the generic belt stops near 18.
    double asia = blob(lat, lon, 24.0, 82.0, 10.0, 14.0);
    double center = 2.0 + 0.72 * decL + 9.0 * asia * nhS;
    double dy = (lat - center) / 10.0;
    double trop = 30.0 * std::exp(-0.5 * dy * dy) + 5.0 * std::exp(-0.5 * (lat / 15.0) * (lat / 15.0));

    // Summer convection over mid-latitude land.
    double nhW = std::pow(nhS, 2.2), shW = std::pow(1.0 - nhS, 2.2);
    double nProf = std::exp(-0.5 * std::pow((lat - 36.0) / 10.0, 2.0));
    double sProf = std::exp(-0.5 * std::pow((lat + 31.0) / 9.0, 2.0));
    double mid = 24.0 * nhW * nProf * sstep(5.0, 18.0, lat) + 26.0 * shW * sProf * sstep(5.0, 18.0, -lat);

    double v = 0.9 * (trop + mid);

    // Regional character of the convection (the shape of the LIS/OTD maps), as smooth multipliers.
    double m = 1.0;
    m += 1.8 * blob(lat, lon, -1.5, 23.0, 6.0, 8.0);      // Congo basin
    m -= 0.45 * blob(lat, lon, -5.0, -62.0, 7.0, 10.0);   // central Amazon (much less than Africa)
    m += 0.6 * blob(lat, lon, 6.0, -75.0, 5.0, 4.0);      // Colombia, Andes foothills
    m += 0.5 * blob(lat, lon, 12.0, -86.0, 4.0, 6.0);     // Central America
    m += 0.4 * blob(lat, lon, 33.0, -88.0, 6.0, 10.0);    // US Southeast
    m += 0.3 * blob(lat, lon, 38.0, -98.0, 5.0, 7.0);     // Great Plains
    m -= 0.45 * blob(lat, lon, 49.0, 15.0, 6.0, 20.0);    // Europe (few, weak storms)
    m += 0.4 * blob(lat, lon, 27.0, 113.0, 7.0, 9.0);     // south China
    m += 0.5 * blob(lat, lon, -1.0, 112.0, 6.0, 14.0);    // Maritime Continent
    m += 0.4 * blob(lat, lon, 8.0, 38.0, 5.0, 5.0);       // Ethiopian highlands
    m += 0.3 * blob(lat, lon, -19.0, 47.0, 4.0, 3.0);     // Madagascar
    m -= 0.4 * blob(lat, lon, 60.0, 90.0, 8.0, 50.0);     // Siberia
    m -= 0.4 * blob(lat, lon, 58.0, -100.0, 7.0, 30.0);   // Canada
    v *= std::max(m, 0.05);

    // Deserts: the belt passes, the moisture does not.
    double dry = 1.0;
    dry *= 1.0 - 0.97 * blob(lat, lon, 24.0, 12.0, 7.0, 24.0);   // Sahara
    dry *= 1.0 - 0.95 * blob(lat, lon, 23.0, 47.0, 7.0, 10.0);   // Arabia
    dry *= 1.0 - 0.75 * blob(lat, lon, 31.0, 60.0, 5.0, 9.0);    // Iran / Afghanistan
    dry *= 1.0 - 0.85 * blob(lat, lon, 42.0, 92.0, 5.0, 18.0);   // Taklamakan / Gobi
    dry *= 1.0 - 0.65 * blob(lat, lon, 33.5, 88.0, 3.5, 10.0);   // Tibetan plateau
    dry *= 1.0 - 0.55 * blob(lat, lon, 45.0, 62.0, 6.0, 15.0);   // Central Asian steppe
    dry *= 1.0 - 0.88 * blob(lat, lon, -26.0, 131.0, 6.0, 12.0); // Australian interior
    dry *= 1.0 - 0.90 * blob(lat, lon, -20.0, -70.5, 8.0, 2.0);  // Atacama / Peruvian coast
    dry *= 1.0 - 0.75 * blob(lat, lon, -24.0, 16.0, 5.0, 4.0);   // Namib / Kalahari
    dry *= 1.0 - 0.55 * blob(lat, lon, 33.0, -113.0, 5.0, 6.0);  // US Southwest
    v *= dry;

    // Point features that dominate the real maps.
    v += 70.0 * blob(lat, lon, 33.0, 74.0, 2.5, 3.5) * (0.25 + seasonPeak(doy, 160.0, 1.5)); // Kashmir
    v += 40.0 * blob(lat, lon, 27.3, 86.0, 1.8, 8.0) * (0.15 + seasonPeak(doy, 150.0, 2.0)); // Himalaya
    v += 55.0 * blob(lat, lon, 25.0, 91.0, 2.0, 3.0) * (0.20 + seasonPeak(doy, 125.0, 2.0)); // Meghalaya
    v += 55.0 * blob(lat, lon, 28.0, -81.5, 2.0, 2.2) * (0.08 + seasonPeak(doy, 205.0, 3.0)); // Florida
    v += 45.0 * blob(lat, lon, -31.0, -63.0, 4.5, 5.0) * (0.10 + seasonPeak(doy, 15.0, 2.0)); // Cordoba
    v += 25.0 * blob(lat, lon, -15.0, 132.0, 4.0, 10.0) * seasonPeak(doy, 20.0, 2.5);       // Top End

    v *= 1.0 - sstep(55.0, 72.0, std::fabs(lat));
    return std::max(v, 0.0);
}

double seaDensity(double lat, double lon, double doy) {
    double decS = 23.44 * seasonSin(doy, 50.0);
    double nhS = nhSummer(doy, 35.0);
    double nhWinter = 1.0 - nhS, shWinter = nhS;

    double cS = 5.0 + 0.35 * decS;
    double dy = (lat - cS) / 7.0;
    double v = 1.7 * std::exp(-0.5 * dy * dy) + 0.45 * std::exp(-0.5 * (lat / 22.0) * (lat / 22.0));

    // Winter storm tracks (cold air over warm water) and the western boundary currents.
    v += 0.35 * nhWinter * std::exp(-0.5 * std::pow((lat - 40.0) / 7.0, 2.0));
    v += 0.25 * shWinter * std::exp(-0.5 * std::pow((lat + 42.0) / 7.0, 2.0));
    v += (1.2 + 2.4 * nhWinter) * blob(lat, lon, 35.5, -70.0, 4.0, 10.0);   // Gulf Stream
    v += (0.6 + 1.6 * nhWinter) * blob(lat, lon, 32.0, 138.0, 4.0, 10.0);  // Kuroshio
    v += 0.8 * blob(lat, lon, -33.0, 30.0, 3.0, 6.0);                       // Agulhas
    v += 2.2 * blob(lat, lon, 1.0, 115.0, 9.0, 22.0);                       // Maritime Continent seas
    v += 1.4 * nhS * blob(lat, lon, 25.0, -88.0, 5.0, 8.0);                 // Gulf of Mexico
    v += 1.0 * nhS * blob(lat, lon, 15.0, -75.0, 5.0, 10.0);                // Caribbean
    v += 1.2 * blob(lat, lon, 16.0, 89.0, 5.0, 5.0);                        // Bay of Bengal
    v += 1.8 * seasonPeak(doy, 290.0, 2.0) * blob(lat, lon, 38.0, 15.0, 4.0, 13.0); // Mediterranean autumn
    v += 1.0 * blob(lat, lon, 8.0, -80.0, 4.0, 6.0);                        // Panama Bight
    v += 0.8 * blob(lat, lon, 3.0, 5.0, 4.0, 6.0);                          // Gulf of Guinea

    // Trade-wind / subsidence oceans.
    double dry = 1.0;
    dry *= 1.0 - 0.85 * blob(lat, lon, -18.0, -100.0, 10.0, 25.0);
    dry *= 1.0 - 0.75 * blob(lat, lon, 22.0, -135.0, 9.0, 20.0);
    dry *= 1.0 - 0.80 * blob(lat, lon, -17.0, -5.0, 9.0, 14.0);
    dry *= 1.0 - 0.70 * blob(lat, lon, 21.0, -32.0, 8.0, 15.0);
    dry *= 1.0 - 0.60 * blob(lat, lon, -25.0, 85.0, 8.0, 20.0);
    v *= dry;

    v *= 1.0 - sstep(52.0, 68.0, std::fabs(lat));
    return std::max(v, 0.0);
}

// Over the lakes the water is the hotspot: added whatever isLand says.
double lakeDensity(double lat, double lon, double doy) {
    double v = 0.0;
    v += 240.0 * blob(lat, lon, 9.6, -71.7, 0.75, 0.8) * (0.8 + 0.4 * seasonPeak(doy, 270.0, 1.0));
    v += 60.0 * blob(lat, lon, -1.0, 32.9, 0.9, 1.0);
    return v;
}

// The lakes' features are narrower than the tabulated grid: near them flashDensity evaluates the model.
bool nearLake(double lat, double lon) {
    return (std::fabs(lat - 9.6) < 4.0 && std::fabs(lonDiff(lon, -71.7)) < 4.0) ||
           (std::fabs(lat + 1.0) < 4.0 && std::fabs(lonDiff(lon, 32.9)) < 4.0);
}

// One axis of a cell-centred grid: the two cells around x and the weight of the second.
void gridAxis(double x, double x0, double step, int n, bool wrap, int &i0, int &i1, double &f) {
    double u = (x - x0) / step;
    double fl = std::floor(u);
    f = u - fl;
    i0 = int(fl);
    i1 = i0 + 1;
    if (wrap) {
        i0 = ((i0 % n) + n) % n;
        i1 = ((i1 % n) + n) % n;
    } else if (i0 < 0) {
        i0 = i1 = 0;
        f = 0.0;
    } else if (i1 > n - 1) {
        i0 = i1 = n - 1;
        f = 0.0;
    }
}

// The analytic model tabulated (evaluated it costs ~1.4 us a call, and the storm catalog calls it per cell):
// sea and land monthly means on 1-degree nodes at 12 mid-months, the nocturnal weight on the same nodes, the
// three diurnal curves every 0.1 h. Lakes excluded (nearLake). Built once per process on first use (a
// function-local static: safe under concurrent const calls).
struct AnalyticTables {
    static constexpr int kNLon = 360, kNLat = 181, kNMonth = 12, kNHour = 240;
    std::vector<float> month;   // [m][lat][lon][2]: sea, land
    std::vector<float> noc;     // [lat][lon]
    float curves[3][kNHour + 1] = {};
};

const AnalyticTables &analyticTables() {
    static const AnalyticTables t = [] {
        AnalyticTables r;
        const int nx = AnalyticTables::kNLon, ny = AnalyticTables::kNLat, nm = AnalyticTables::kNMonth;
        r.month.resize(size_t(nm) * ny * nx * 2);
        r.noc.resize(size_t(ny) * nx);
        auto rows = [&r](int y0, int y1) {
            for (int y = y0; y < y1; ++y) {
                double lat = -90.0 + y;
                for (int x = 0; x < nx; ++x) {
                    double lon = -180.0 + x;
                    r.noc[size_t(y) * nx + x] = float(nocturnalWeight(lat, lon));
                    for (int m = 0; m < nm; ++m) {
                        double doy = (m + 0.5) * kYearDays / nm;
                        size_t i = ((size_t(m) * ny + y) * nx + x) * 2;
                        r.month[i] = float(seaDensity(lat, lon, doy));
                        r.month[i + 1] = float(landDensity(lat, lon, doy));
                    }
                }
            }
        };
        unsigned nt = std::clamp(std::thread::hardware_concurrency(), 1u, 16u);
        std::vector<std::thread> pool;
        for (unsigned k = 0; k < nt; ++k) pool.emplace_back(rows, int(ny * k / nt), int(ny * (k + 1) / nt));
        for (auto &th : pool) th.join();
        for (int h = 0; h <= AnalyticTables::kNHour; ++h) {
            double hour = 24.0 * h / AnalyticTables::kNHour;
            r.curves[0][h] = float(landCurve(hour));
            r.curves[1][h] = float(nightCurve(hour));
            r.curves[2][h] = float(seaCurve(hour));
        }
        return r;
    }();
    return t;
}

// hour < 0: the monthly mean. land already clamped to 0..1.
float analyticFast(double lat, double lon, double doy, double hour, double land) {
    const AnalyticTables &t = analyticTables();
    const int nx = AnalyticTables::kNLon, ny = AnalyticTables::kNLat, nm = AnalyticTables::kNMonth;
    int x0, x1, y0, y1, m0, m1;
    double fx, fy, fm;
    gridAxis(lon, -180.0, 1.0, nx, true, x0, x1, fx);
    gridAxis(lat, -90.0, 1.0, ny, false, y0, y1, fy);
    gridAxis(doy, 0.5 * kYearDays / nm, kYearDays / nm, nm, true, m0, m1, fm);
    const float *a = t.month.data();
    auto at = [&](int m, int y, int x) {
        const float *p = a + ((size_t(m) * ny + y) * nx + x) * 2;
        return p[0] + (p[1] - p[0]) * land;
    };
    auto plane = [&](int m) {
        double u = at(m, y0, x0) + (at(m, y0, x1) - at(m, y0, x0)) * fx;
        double v = at(m, y1, x0) + (at(m, y1, x1) - at(m, y1, x0)) * fx;
        return u + (v - u) * fy;
    };
    double p0 = plane(m0);
    double mean = std::max(p0 + (plane(m1) - p0) * fm, 0.0);
    if (hour < 0.0) return float(mean);
    const float *n = t.noc.data();
    double nu = n[y0 * nx + x0] + (n[y0 * nx + x1] - n[y0 * nx + x0]) * fx;
    double nv = n[y1 * nx + x0] + (n[y1 * nx + x1] - n[y1 * nx + x0]) * fx;
    double noc = nu + (nv - nu) * fy;
    double hu = hour * (AnalyticTables::kNHour / 24.0);
    int h0 = std::clamp(int(hu), 0, AnalyticTables::kNHour - 1);
    double fh = hu - h0;
    double c[3];
    for (int k = 0; k < 3; ++k) c[k] = t.curves[k][h0] + (t.curves[k][h0 + 1] - t.curves[k][h0]) * fh;
    return float(mean * combineDiurnal(c[0], c[1], c[2], noc, land));
}

} // namespace

float StormClimatology::analytic(double latDeg, double lonDeg, double dayOfYear, double localSolarHour,
                                 float isLand) const {
    double land = std::clamp(double(isLand), 0.0, 1.0);
    double m = seaDensity(latDeg, lonDeg, dayOfYear) +
               (landDensity(latDeg, lonDeg, dayOfYear) - seaDensity(latDeg, lonDeg, dayOfYear)) * land +
               lakeDensity(latDeg, lonDeg, dayOfYear);
    if (localSolarHour < 0.0) return float(m);
    return float(m * diurnalFactor(latDeg, lonDeg, localSolarHour, land));
}

// Without the file every call is a table lookup of the analytic model, built here so that no concurrent
// caller pays for it.
bool StormClimatology::load(const std::string &path) {
    table_.clear();
    diurnal_.clear();
    bool ok = loadTable(path);
    if (!ok) {
        table_.clear();
        diurnal_.clear();
        analyticTables();
    }
    return ok;
}

bool StormClimatology::loadTable(const std::string &path) {
    FILE *f = std::fopen(path.c_str(), "rb");
    if (!f) return false;
    char magic[4] = {};
    uint32_t hdr[7] = {};    // version, nLon, nLat, nMonth, nHour, dLon, dLat
    float grid[8] = {};      // lon0, lat0, stepLon, stepLat, then the same for the diurnal grid
    bool ok = std::fread(magic, 1, 4, f) == 4 && std::memcmp(magic, "SLWX", 4) == 0 &&
              std::fread(hdr, 4, 7, f) == 7 && hdr[0] == 1 && std::fread(grid, 4, 8, f) == 8;
    ok = ok && hdr[1] > 1 && hdr[1] <= 4096 && hdr[2] > 1 && hdr[2] <= 2048 && hdr[3] >= 1 && hdr[3] <= 366 &&
         hdr[4] >= 1 && hdr[4] <= 96 && hdr[5] > 1 && hdr[5] <= 4096 && hdr[6] > 1 && hdr[6] <= 2048 &&
         grid[2] > 0.0f && grid[3] > 0.0f && grid[6] > 0.0f && grid[7] > 0.0f;
    std::vector<float> tab, di;
    if (ok) {
        tab.resize(size_t(hdr[3]) * hdr[2] * hdr[1]);
        di.resize(size_t(hdr[6]) * hdr[5] * hdr[4]);
        ok = std::fread(tab.data(), 4, tab.size(), f) == tab.size() &&
             std::fread(di.data(), 4, di.size(), f) == di.size();
    }
    std::fclose(f);
    if (!ok) return false;
    for (float v : tab)
        if (!(v >= 0.0f && v < 1e5f)) return false;
    for (float v : di)
        if (!(v >= 0.0f && v < 100.0f)) return false;
    nLon_ = int(hdr[1]); nLat_ = int(hdr[2]); nMonth_ = int(hdr[3]); nHour_ = int(hdr[4]);
    dLon_ = int(hdr[5]); dLat_ = int(hdr[6]);
    lon0_ = grid[0]; lat0_ = grid[1]; stepLon_ = grid[2]; stepLat_ = grid[3];
    dLon0_ = grid[4]; dLat0_ = grid[5]; dStepLon_ = grid[6]; dStepLat_ = grid[7];
    table_ = std::move(tab);
    diurnal_ = std::move(di);
    return true;
}

// Bilinear in space, linear between mid-months (cyclic).
float StormClimatology::tableMonthly(double latDeg, double lonDeg, double dayOfYear) const {
    int x0, x1, y0, y1, m0, m1;
    double fx, fy, fm;
    gridAxis(lonDeg, lon0_, stepLon_, nLon_, true, x0, x1, fx);
    gridAxis(latDeg, lat0_, stepLat_, nLat_, false, y0, y1, fy);
    double monthLen = kYearDays / nMonth_;
    gridAxis(dayOfYear, 0.5 * monthLen, monthLen, nMonth_, true, m0, m1, fm);
    auto at = [&](int m, int y, int x) { return double(table_[(size_t(m) * nLat_ + y) * nLon_ + x]); };
    auto plane = [&](int m) {
        double a = at(m, y0, x0) + (at(m, y0, x1) - at(m, y0, x0)) * fx;
        double b = at(m, y1, x0) + (at(m, y1, x1) - at(m, y1, x0)) * fx;
        return a + (b - a) * fy;
    };
    double p0 = plane(m0), p1 = plane(m1);
    return float(std::max(p0 + (p1 - p0) * fm, 0.0));
}

// Bilinear in space, linear between hour bins (cyclic); bin i is centred on local solar hour (i + 0.5) 24 / n.
// Bilinear and linear blends of factors whose mean is 1 keep a mean of 1.
float StormClimatology::tableDiurnal(double latDeg, double lonDeg, double localSolarHour) const {
    int x0, x1, y0, y1, h0, h1;
    double fx, fy, fh;
    gridAxis(lonDeg, dLon0_, dStepLon_, dLon_, true, x0, x1, fx);
    gridAxis(latDeg, dLat0_, dStepLat_, dLat_, false, y0, y1, fy);
    double binLen = 24.0 / nHour_;
    gridAxis(localSolarHour, 0.5 * binLen, binLen, nHour_, true, h0, h1, fh);
    auto at = [&](int y, int x, int h) { return double(diurnal_[(size_t(y) * dLon_ + x) * nHour_ + h]); };
    auto plane = [&](int h) {
        double a = at(y0, x0, h) + (at(y0, x1, h) - at(y0, x0, h)) * fx;
        double b = at(y1, x0, h) + (at(y1, x1, h) - at(y1, x0, h)) * fx;
        return a + (b - a) * fy;
    };
    double p0 = plane(h0), p1 = plane(h1);
    return float(std::max(p0 + (p1 - p0) * fh, 0.0));
}

float StormClimatology::flashDensity(double latDeg, double lonDeg, double dayOfYear, double localSolarHour,
                                     float isLand) const {
    double h = std::fmod(localSolarHour, 24.0);
    if (h < 0.0) h += 24.0;
    if (loaded()) return tableMonthly(latDeg, lonDeg, dayOfYear) * tableDiurnal(latDeg, lonDeg, h);
    if (nearLake(latDeg, lonDeg)) return analytic(latDeg, lonDeg, dayOfYear, h, isLand);
    return analyticFast(latDeg, lonDeg, dayOfYear, h, std::clamp(double(isLand), 0.0, 1.0));
}

float StormClimatology::monthlyMean(double latDeg, double lonDeg, double dayOfYear, float isLand) const {
    if (loaded()) return tableMonthly(latDeg, lonDeg, dayOfYear);
    if (nearLake(latDeg, lonDeg)) return analytic(latDeg, lonDeg, dayOfYear, -1.0, isLand);
    return analyticFast(latDeg, lonDeg, dayOfYear, -1.0, std::clamp(double(isLand), 0.0, 1.0));
}

} // namespace wx
