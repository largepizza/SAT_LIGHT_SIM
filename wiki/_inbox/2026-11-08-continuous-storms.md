# Continuous storms (storm_continuous)

- Storms no longer have column/anvil-plate geometry by default. A catalog storm steers the weather map (type toward
  cumulonimbus, more coverage) over a wide envelope (~3.5x its cells' radius), and the cumulus field draws it as its own
  deep cloud. Towers come from a thresholded noise (cluster Perlin at 16 and 64 km + coarse cells), so they rise and
  fade continuously and ride the wind with the rest of the texture.
- The map's own dense deep convection (ITCZ, monsoon, afternoon land) can now reach the storm range, so stormy regions
  last as long as the map's systems (hours), not a catalog cell's life.
- Anvils are the high-cloud layer: a dense, flat-topped ice sheet under the tropopause wherever the convection cube's
  anvil field (or strong cores) says so; shaped by the layer's noise, lumpy underside, fibrous edge.
- Setting: clouds_v2.storm_continuous (1 = on, default; 0 = the previous column + plate storms).
- Source: docs/subsystems/weather.md "Continuous storms"; shaders cloud_v2_weather.comp, include/clouds_v2.glsl.
