---
pages: [rendering/clouds/storms.md, rendering/points-bloom-glare.md, simulation/kessler.md, using/features.md, using/weather.md]
code: [src/simulations/SatelliteSim.h, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
The built-in defaults (SatelliteSim.h, = the High preset) carry the author's 2026-11-25 tuned look, 77 fields at 2
significant figures (full table: qa/waves/20261125/evidence/s1/BAKE.md). The ones a page quotes:
`kessler.dust_gain` x1000 (was x100); `clouds_v2.beam_light` 280 (was 29), `beam_haze` 1.0 (was 1.6);
`photometry.glare_gain` 0.11 (was 0.684), `glare_size_px` 57 (was 29.79), `glare_threshold` 1.9, `glare_falloff` 3.9,
`glare_spikes` 8, `glare_near_range_km` 1 (was 21.86), `glare_cluster_merge` 0.72; `clouds.moon_gain` 0.013 (was
0.0053); `clouds_v2.rain_amount` 1 (was 3); storms: `storm_life` 5, `storm_tower_height` 0.28, `storm_tower_rise_min`
120 min, `storm_moat` 0, `storm_fullness` 0.28, `anvil_thick_km` 0.05 (below the slider's 0.3 floor), `anvil_hang_km`
2.8, `cb_lobes` 0.068, `cb_overshoot_km` 0, `small_cumulus` 0, `silver_lining` 0.2; hurricanes `storm_cyc_arms` 0 (by
intensity), `hurricane_flow_turns` 0.93; sea `ocean_far_ripple` 0, `ocean_motion_filter` 0, `ocean_wave_range_km` 45;
launches `plume_gain` 0.23, `plume_growth` 0.43, `plume_dispersal` 2.9, `jelly_streak_length_s` 26; `clouds.drift_rate`
6.5e-6 (the rate the default drift phase was derived at).

## History
Earlier bakes: 2026-11-10 (cloud look), 2026-11-15 (coverage / lighting). This one also rounded the non-round
photometry and launch defaults it touched (glare 0.684 / 29.79 / 1.765 / 4.678 / 21.86; streak 54.77 / 0.9095).
