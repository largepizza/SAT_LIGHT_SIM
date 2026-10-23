// Biome.h — a coarse land-cover map for the weather hazards (2026-10-23, .plans/WEATHER_HAZARDS_PLAN.md): wildfires
// burn where there is fuel in a dry season, haboobs lift dust where the ground is bare and dry. Built once at load from
// what the app already has: the day map's colour (the ground as drawn: the ambience's desert / forest / vegetation / ice
// tests), latitude, the elevation and the ocean mask. Equirect, 2048 x 1024 (~20 km a texel), Earth-fixed.
#pragma once
#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace wx {

enum class Biome : uint8_t {
    Water = 0,
    Ice,
    Tundra,
    Boreal,           // boreal / montane conifer forest
    TemperateForest,
    TropicalForest,
    Grassland,        // steppe, prairie, pasture (and cropland: the day map does not separate it)
    Savanna,          // tropical grass with scattered trees
    Shrubland,        // dry shrub, chaparral, Mediterranean scrub, semi-desert
    Desert,
    Count
};
const char *biomeName(Biome b);

class BiomeMap {
public:
    // dayRgb: w x h x 3 bytes (equirect, north up, lon -180 at x 0); elevM(lat, lon deg) metres above sea level;
    // ocean(lat, lon deg). Built in parallel on `threads` threads.
    void build(const std::vector<uint8_t> &dayRgb, int w, int h, const std::function<float(float, float)> &elevM,
               const std::function<bool(float, float)> &ocean, int threads);
    bool ready() const { return w_ > 0; }
    Biome at(double latDeg, double lonDeg) const;
    float fuelAt(double latDeg, double lonDeg) const;         // 0..1: what can burn (forest, savanna, scrub, grass)
    float dustAt(double latDeg, double lonDeg) const;         // 0..1: bare dry ground a gust front can lift
    // 0..1: how dry the season is there (the fire season): the extratropics' summer, the tropics' winter dry season.
    static float seasonalDryness(double latDeg, double dayOfYear);
    // A colour-coded PNG (rgba) of the classes, for checking against a land-cover map.
    bool exportPng(const std::string &path) const;
    int width() const { return w_; }
    int height() const { return h_; }

private:
    int idx(double latDeg, double lonDeg) const;
    int w_ = 0, h_ = 0;
    std::vector<uint8_t> cls_, fuel_, dust_;
};

} // namespace wx
