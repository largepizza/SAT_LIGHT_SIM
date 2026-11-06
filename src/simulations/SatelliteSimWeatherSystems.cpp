// Weather v2 (2026-11-06, .plans/WEATHER_V2_PLAN.md): the weather systems' upload. WeatherSystems.h is the model; this
// fills the bake's list (cv2_storms.glsl cv2SysHead / cv2Systems, read by cloud_v2_weather.comp wxSystems).
#include "SatelliteSim.h"
#include "../FrameProfile.h"
#include <algorithm>
#include <cmath>

void SatelliteSim::updateWeatherSystems(double t)
{
    FPROF_SCOPE("updateWeatherSystems");
    if (!stormMapped)
        return;
    wxSystemSettings.waves = wxWindWaves;
    wxSystemSettings.eddies = wxWindEddies;
    wxSystems.systemsAt(t, 0.0, wxSystemSettings, wxSystemsNow);
    // The strongest first (the list is capped).
    std::sort(wxSystemsNow.begin(), wxSystemsNow.end(),
              [](const wx::SystemState &a, const wx::SystemState &b) { return a.intensity > b.intensity; });
    const uint32_t n = (uint32_t)std::min<size_t>(wxSystemsNow.size(), kWxSystemMax);
    char *base = static_cast<char *>(stormMapped);
    auto *sys = reinterpret_cast<GpuWxSystem *>(base + kSystemsOffset);
    for (uint32_t i = 0; i < n; ++i)
    {
        const wx::SystemState &S = wxSystemsNow[i];
        // The reach: the ownership ellipse reaches 3.2 / 0.55 = 5.8 head radii down the cold front's trail.
        const double reach = 6.2 * S.radiusM;
        sys[i].a = glm::vec4(glm::vec3(S.centre), (float)S.radiusM);
        sys[i].b = glm::vec4(glm::vec3(S.heading), (float)S.intensity);
        sys[i].c = glm::vec4((float)S.age01, (float)S.hemi, (float)S.kind, (float)S.seed);
        sys[i].d = glm::vec4((float)S.speedMps, 0.0f, 0.0f, (float)std::cos(std::min(reach / 6371000.0, 3.0)));
    }
    auto *head = reinterpret_cast<uint32_t *>(base + kSysHeadOffset);
    head[0] = n; head[1] = head[2] = head[3] = 0u;
    ctx_->mirrorDirty(stormMapped, kSysHeadOffset, 16 + 64ull * n);
}
