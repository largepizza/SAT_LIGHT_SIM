---
pages: [using/graphics-settings.md]
code: [src/simulations/SatelliteSimUI.cpp, src/simulations/SatelliteSim.h]
---
## Now
Settings > Display > SETTINGS has "Dev mode: unbounded values" (`display.dev_unbounded_values`, default off,
shown only while "Show advanced settings" is on). With it on, a number typed into a settings slider's value
box is applied as typed instead of being clamped to the slider's range. It covers the shared slider rows of
every advanced tab (Clouds, Weather, Atmosphere, Terrain, Night lights, Ocean, Beams, Sound's advanced rows)
and the Photometry rows. A value outside its range shows in amber; the -/+ buttons step it without snapping
back while it is outside; dragging the slider returns it to the range. Nothing downstream is guaranteed to
handle such a value.

Not covered: the Display tab's steppers (render scale etc.), the volumes, the Cinematics fields, and the keys
that are clamped when settings.json is loaded (`clouds_v2.history_weight`, `history_weight_moving`, `godrays`,
`haze_aloft`, `imagery_share`, `drop_reach_m`; `clouds.ocean_wave_range_km`, `ocean_wave_sharpness`,
`ocean_wave_range_fade`, `ocean_storm_trails`, `aurora_kp_manual`, `aurora_storm_rate`; `display.sky_taa_weight*`):
those take an out-of-range value for the session and are clamped again on the next launch.

## History
Added 2026-10-11 for tuning: the user often needs a value past a slider's range. `numberField(..., devUnbound)`
and `SatelliteSim::sliderStep()`.
