#pragma once
// Weather v2 (2026-11-06, .plans/WEATHER_V2_PLAN.md): weather SYSTEMS. A system is an object with a lifecycle and a
// track along the steering wind (WindField.h) that writes SDF-shaped fields (moisture, stratiform / convective share)
// into the weather cube (cloud_v2_weather.comp wxSystems). It travels for days without the resets a flow map needs,
// which is what carries the large-scale weather across the globe.
//
// Kinds so far: extratropical cyclones (the comma cloud: comma head, warm front, long trailing cold front, dry slot,
// cold-air cumulus), spawned along the mid-latitude storm tracks. Every system is a pure function of absolute sim
// time (seconds since J2000): spawned by hashes per (hemisphere, longitude cell, 12-h bin), its track integrated from
// birth through the deterministic wind, so warp, reverse, bookmarks and replays see the same systems.
#include "WindField.h"
#include <glm/glm.hpp>
#include <cstdint>
#include <unordered_map>
#include <vector>

namespace wx {

enum class SystemKind : uint8_t { Extratropical = 0 };

struct SystemSettings {
    double etcPerDay  = 4.0;    // extratropical cyclones born per day per hemisphere (mean)
    double sizeScale  = 1.0;    // x the comma's scale (its head radius ~550-800 km)
    double lifeScale  = 1.0;    // x the lifetimes (3-5.5 days)
    double waves = 1.0, eddies = 1.0;   // the wind's (WindState)
};

// One system at a moment.
struct SystemState {
    uint64_t id = 0;
    SystemKind kind = SystemKind::Extratropical;
    glm::dvec3 centre{0.0};    // unit ECEF
    glm::dvec3 heading{0.0};   // unit tangent: the direction it moves
    double hemi = 1.0;         // +1 north, -1 south
    double radiusM = 6.0e5;    // the comma head's radius
    double intensity = 0.0;    // 0..1 (the lifecycle)
    double age01 = 0.0;        // 0 at birth .. 1 at death (the occlusion's progress)
    double seed = 0.0;         // 0..1
    double speedMps = 0.0;
};

class SystemCatalog {
public:
    // The systems alive at t (seconds since J2000). decl: the Sun's declination (rad) at t.
    void systemsAt(double t, double decl, const SystemSettings &s, std::vector<SystemState> &out);
    void clear() { tracks_.clear(); }
    size_t cachedTracks() const { return tracks_.size(); }

private:
    struct Track {
        double birthT = 0.0, lifeS = 0.0, radiusM = 0.0, seed = 0.0, hemi = 1.0;
        std::vector<glm::dvec3> pos;   // every kTrackStepS from birth
        uint64_t settingsKey = 0;
        double lastUsedT = 0.0;
    };
    std::unordered_map<uint64_t, Track> tracks_;
};

} // namespace wx
