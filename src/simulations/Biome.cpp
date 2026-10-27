// Biome.cpp — see Biome.h.
#include "Biome.h"
#include "stb_image_write.h" // the implementation lives in UIRenderer.cpp
#include <algorithm>
#include <cmath>
#include <thread>

namespace wx {

namespace {
float ss(float a, float b, float x)
{
    const float t = std::clamp((x - a) / (b - a), 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}
} // namespace

const char *biomeName(Biome b)
{
    static const char *n[] = {"water", "ice", "tundra", "boreal forest", "temperate forest", "tropical forest",
                              "grassland", "savanna", "shrubland", "desert"};
    const int i = (int)b;
    return (i >= 0 && i < (int)Biome::Count) ? n[i] : "?";
}

void BiomeMap::build(const std::vector<uint8_t> &dayRgb, int w, int h, const std::function<float(float, float)> &elevM,
                     const std::function<bool(float, float)> &ocean, int threads,
                     const std::function<float(float, float)> &nightLum)
{
    if (w <= 0 || h <= 0 || dayRgb.size() < (size_t)w * h * 3)
        return;
    cls_.assign((size_t)w * h, 0);
    fuel_.assign((size_t)w * h, 0);
    dust_.assign((size_t)w * h, 0);
    urban_.assign((size_t)w * h, 0);
    auto rows = [&](int y0, int y1) {
        for (int y = y0; y < y1; ++y)
        {
            const float lat = 90.0f - (y + 0.5f) * 180.0f / h, alat = std::fabs(lat);
            for (int x = 0; x < w; ++x)
            {
                const float lon = -180.0f + (x + 0.5f) * 360.0f / w;
                const size_t i = (size_t)y * w + x;
                // Built-up land from the night map's lights (an ~18-km box mean of sRGB luminance: the land's blue base
                // ~14, farmland and roads 15-25, a town's texel ~30, a city's 60-255).
                // (the brightest of nine reads over the texel and its edges: the night copy's grid is not this one)
                if (nightLum)
                {
                    const float dLa = 0.5f * 180.0f / h, dLo = 0.5f * 360.0f / w;
                    float L = 0.0f;
                    for (int a = -1; a <= 1; ++a)
                        for (int b = -1; b <= 1; ++b)
                            L = std::max(L, nightLum(lat + a * dLa, lon + b * dLo));
                    urban_[i] = (uint8_t)std::lround(255.0f * ss(20.0f, 34.0f, L));
                }
                if (ocean(lat, lon))
                {
                    cls_[i] = (uint8_t)Biome::Water;
                    continue;
                }
                // The colour, a 3x3 average (~60 km: the day map is a dry-season mosaic, its texels mottled).
                float R = 0, G = 0, B = 0;
                for (int dy = -1; dy <= 1; ++dy)
                    for (int dx = -1; dx <= 1; ++dx)
                    {
                        const int xx = ((x + dx) % w + w) % w, yy = std::clamp(y + dy, 0, h - 1);
                        const uint8_t *p = &dayRgb[((size_t)yy * w + xx) * 3];
                        R += p[0] / 9.0f; G += p[1] / 9.0f; B += p[2] / 9.0f;
                    }
                const float L = (R + G + B) / 3.0f;
                const float ice = ss(165.0f, 205.0f, L) * ss(15.0f, -5.0f, R - B);
                const float desert = ss(95.0f, 150.0f, L) * ss(10.0f, 25.0f, R - B) * ss(5.0f, -15.0f, G - R);
                const float forest = ss(100.0f, 55.0f, L) * ss(0.0f, 10.0f, G - R) * ss(10.0f, 22.0f, G - B);
                const float veg = ss(-12.0f, 5.0f, G - R) * ss(8.0f, 25.0f, G - B) * (1.0f - ice);
                const float elev = elevM(lat, lon);
                Biome b;
                if (ice > 0.5f || (alat > 64.0f && L > 160.0f) || lat < -62.0f)
                    b = Biome::Ice;
                else if (desert > 0.5f)
                    b = Biome::Desert;
                else if ((alat > 58.0f || elev > 3800.0f) && forest < 0.4f)
                    b = Biome::Tundra;
                else if (forest > 0.4f || (veg > 0.6f && L < 85.0f))
                    b = alat < 20.0f ? Biome::TropicalForest
                      : (alat > 50.0f || elev > 2200.0f) ? Biome::Boreal : Biome::TemperateForest;
                else if (veg > 0.4f)
                    b = alat < 25.0f ? Biome::Savanna : Biome::Grassland;
                else if (L > 135.0f && R - B > 18.0f)
                    b = Biome::Desert;   // bright, tan but not past the desert test: semi-desert
                else
                    b = alat < 40.0f ? (alat < 20.0f ? Biome::Savanna : Biome::Shrubland) : Biome::Grassland;
                cls_[i] = (uint8_t)b;
                float fuel = 0.0f, dust = 0.0f;
                switch (b)
                {
                case Biome::Tundra: fuel = 0.15f; break;
                case Biome::Boreal: fuel = 0.95f; break;
                case Biome::TemperateForest: fuel = 0.7f; break;
                case Biome::TropicalForest: fuel = 0.35f; break;   // humid: it burns where cleared, in the dry season
                case Biome::Grassland: fuel = 0.5f; dust = alat < 45.0f ? 0.12f * ss(95.0f, 130.0f, L) : 0.0f; break;
                case Biome::Savanna: fuel = 0.85f; dust = 0.08f; break;
                case Biome::Shrubland: fuel = 0.75f; dust = 0.35f * ss(90.0f, 130.0f, L); break;
                case Biome::Desert: dust = 0.55f + 0.45f * ss(110.0f, 175.0f, L); break;
                default: break;
                }
                fuel_[i] = (uint8_t)std::lround(255.0f * std::clamp(fuel, 0.0f, 1.0f));
                dust_[i] = (uint8_t)std::lround(255.0f * std::clamp(dust, 0.0f, 1.0f));
            }
        }
    };
    const int n = std::clamp(threads, 1, 32);
    std::vector<std::thread> pool;
    for (int k = 0; k < n; ++k)
        pool.emplace_back(rows, k * h / n, (k + 1) * h / n);
    for (std::thread &t : pool)
        t.join();
    w_ = w;
    h_ = h;
}

int BiomeMap::idx(double latDeg, double lonDeg) const
{
    double lon = std::fmod(lonDeg + 180.0, 360.0);
    if (lon < 0.0) lon += 360.0;
    const int x = std::clamp((int)(lon / 360.0 * w_), 0, w_ - 1);
    const int y = std::clamp((int)((90.0 - latDeg) / 180.0 * h_), 0, h_ - 1);
    return y * w_ + x;
}

Biome BiomeMap::at(double latDeg, double lonDeg) const
{
    return ready() ? (Biome)cls_[(size_t)idx(latDeg, lonDeg)] : Biome::Water;
}
float BiomeMap::fuelAt(double latDeg, double lonDeg) const
{
    return ready() ? fuel_[(size_t)idx(latDeg, lonDeg)] / 255.0f : 0.0f;
}
float BiomeMap::dustAt(double latDeg, double lonDeg) const
{
    return ready() ? dust_[(size_t)idx(latDeg, lonDeg)] / 255.0f : 0.0f;
}
float BiomeMap::urbanAt(double latDeg, double lonDeg) const
{
    return ready() ? urban_[(size_t)idx(latDeg, lonDeg)] / 255.0f : 0.0f;
}
float BiomeMap::burnableAt(double latDeg, double lonDeg) const
{
    if (!ready()) return 0.0f;
    const size_t i = (size_t)idx(latDeg, lonDeg);
    return fuel_[i] / 255.0f * (1.0f - urban_[i] / 255.0f);
}

// The fire season: the extratropics dry out in their summer (peak ~day 210 north, ~day 25 south), the tropics in their
// winter dry season (north of the equator ~Dec-Mar, south ~Jun-Sep); the equatorial band stays wetter year round.
float BiomeMap::seasonalDryness(double latDeg, double dayOfYear)
{
    const double kTwoPi = 6.283185307179586;
    const double alat = std::fabs(latDeg);
    const double north = latDeg >= 0.0 ? 1.0 : -1.0;
    // Extratropical: a summer season that NARROWS poleward (2026-10-27, issue 20261027_000126: a fire on an Aleutian
    // island in late November). It was the cosine itself, 0.29 at 53 N on 22 November (0.09 after the rate's square):
    // the boreal and subarctic fire season is a few months of summer (June-August), the mid-latitudes' a longer one
    // (May-October), and in the winter half nothing burns. The season is the part of the year whose cosine passes c0,
    // c0 rising from -0.45 at 30 deg to 0.35 by 62 deg.
    const double cS = std::cos(kTwoPi * (dayOfYear - (north > 0 ? 210.0 : 25.0)) / 365.25);
    const double c0 = -0.45 + 0.8 * ss(30.0f, 62.0f, (float)alat);
    const double summer = ss((float)c0, 1.0f, (float)cS);
    // Tropical: the dry season half a year from the summer monsoon (the Sahel's peaks Dec-Jan, southern Africa's Aug).
    const double drySeason = 0.5 + 0.5 * std::cos(kTwoPi * (dayOfYear - (north > 0 ? 15.0 : 220.0)) / 365.25);
    const double wTrop = 1.0 - ss(18.0f, 30.0f, (float)alat);
    const double equatorWet = 1.0 - 0.6 * (1.0 - ss(0.0f, 8.0f, (float)alat));
    const double d = (wTrop * drySeason + (1.0 - wTrop) * summer) * equatorWet;
    return (float)std::clamp(d, 0.0, 1.0);
}

bool BiomeMap::exportPng(const std::string &path) const
{
    if (!ready())
        return false;
    static const uint8_t pal[(int)Biome::Count][3] = {
        {20, 40, 90},    // water
        {235, 240, 250}, // ice
        {150, 160, 140}, // tundra
        {30, 80, 60},    // boreal
        {50, 130, 50},   // temperate forest
        {10, 90, 20},    // tropical forest
        {170, 200, 90},  // grassland
        {200, 180, 70},  // savanna
        {170, 130, 80},  // shrubland
        {240, 210, 150}, // desert
    };
    std::vector<uint8_t> px((size_t)w_ * h_ * 4);
    for (size_t i = 0; i < cls_.size(); ++i)
    {
        const int c = std::min<int>(cls_[i], (int)Biome::Count - 1);
        px[i * 4 + 0] = pal[c][0];
        px[i * 4 + 1] = pal[c][1];
        px[i * 4 + 2] = pal[c][2];
        px[i * 4 + 3] = 255;
    }
    return stbi_write_png(path.c_str(), w_, h_, 4, px.data(), w_ * 4) != 0;
}

} // namespace wx
