# Cirrus fibres ride the wind (flow map)

Date: 2026-11-08 (branch weather-salvage, ported from weather-v2). Pages: clouds (cirrus / high layer), settings.

- What changed: the cirrus fibres are read through a two-phase flow map driven by an analytic jet-level wind
  (shaders/include/wind_field.glsl: zonal belts, Rossby waves, mesoscale eddies), relative to the fibres' existing even
  eastward slide. Each window (default 2 h, "Cirrus flow window", 0 = off) the texture renews; the two phases cross-fade.
- Cost: +0.25 ms GPU from 400 km, nothing measurable from the ground; cloud march 249 -> 252 registers.
- The rest of the weather (map, storms, low cloud noise) is not advected on this branch: that was weather-v2's main cost.
