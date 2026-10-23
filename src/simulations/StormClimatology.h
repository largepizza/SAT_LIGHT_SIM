#pragma once
// Thunderstorm climatology for the weather events (.plans/WEATHER_EVENTS_PLAN.md, 2026-10-22): where and when
// deep convection happens, as a function of place, season and local solar time. The storm catalog (Weather.h)
// spawns storms at this rate, thinned by the cloud map's moisture.
//
// Source: a baked table (tools/make_storm_climatology.py -> assets/textures/storm_climatology.bin), from the
// NASA LIS/OTD gridded lightning climatology where available; an analytic model (ITCZ following the Sun with
// a lag, land/sea contrast, the afternoon land maximum, the nocturnal maxima of the Great Plains / La Plata /
// the Sahel's MCSs, the maritime early-morning maximum) when the file is missing. App-independent: no Vulkan,
// no SatelliteSim.
#include <string>
#include <vector>
#include <cstdint>

namespace wx {

class StormClimatology {
public:
    // Loads the baked table. false = missing or malformed: the analytic model answers (logged by the caller).
    bool load(const std::string &path);
    bool loaded() const { return !table_.empty(); }

    // Lightning flash density at this point and moment, flashes per km^2 per year (the LIS/OTD unit: the
    // annual mean of the month, x the diurnal cycle's factor at that local solar hour, whose mean over a day
    // is 1). latDeg -90..90, lonDeg -180..180, dayOfYear 0..365.25 (fractional), localSolarHour 0..24.
    // isLand: 0 sea .. 1 land (the analytic model's only use of it; the table carries its own).
    float flashDensity(double latDeg, double lonDeg, double dayOfYear, double localSolarHour, float isLand) const;

    // The same at the table's resolution with no diurnal factor (diagnostics, the WeatherTool's maps).
    float monthlyMean(double latDeg, double lonDeg, double dayOfYear, float isLand) const;

private:
    float analytic(double latDeg, double lonDeg, double dayOfYear, double localSolarHour, float isLand) const;
    bool loadTable(const std::string &path);
    float tableMonthly(double latDeg, double lonDeg, double dayOfYear) const;
    float tableDiurnal(double latDeg, double lonDeg, double localSolarHour) const;
    // Baked table: see tools/make_storm_climatology.py for the layout (written there, read in load()).
    std::vector<float> table_;     // [month][lat][lon], flashes per km^2 per year
    int nLon_ = 0, nLat_ = 0, nMonth_ = 0, nHour_ = 0;
    float lon0_ = 0, lat0_ = 0, stepLon_ = 1, stepLat_ = 1;     // centre of cell [0][0], lat ascending
    std::vector<float> diurnal_;   // [lat][lon][hour] factors (mean 1), may be coarser than table_
    int dLon_ = 0, dLat_ = 0;
    float dLon0_ = 0, dLat0_ = 0, dStepLon_ = 1, dStepLat_ = 1;
};

} // namespace wx
