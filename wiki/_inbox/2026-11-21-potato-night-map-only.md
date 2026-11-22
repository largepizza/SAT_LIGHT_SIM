---
pages: [rendering/cities.md, development/controls-and-settings.md]
code: [shaders/sat_sky_minimal.frag]
---
## Now
The Potato preset (`sat_sky_minimal.frag`) draws cities as the night-lights map alone at every altitude: the day map
by day, the night map x 1.3 at night, blended over the local terminator. It binds and reads no city detail texture.
Planetarium (SKY_LITE) still tiles the city detail textures over the night map, as in v1.1.

## History
Until 2026-11-21 Potato blended the tiled day / night city detail textures (20-km tiles) over every night-map texel
brighter than 0.01 within 300 km of the eye. From the air each lit 5-km texel of the map read as a square of street
pattern with a soft square halo (issue_20261121_160405, shot 10: "Just remove the city detail texture in Potato
mode"). SPIR-V 42,688 -> 41,060 bytes (budget 47,104).
