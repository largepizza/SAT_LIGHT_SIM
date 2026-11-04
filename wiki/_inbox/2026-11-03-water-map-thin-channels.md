---
pages: [rendering/terrain.md]
code: [tools/make_water_map.py, assets/textures/earth_water_sdf.r8, shaders/include/terrain.glsl]
---
## Now
The water map bake keeps sea channels narrower than one 8K texel. Natural Earth is rasterised at 16K and its
signed distance field averaged 2x2 down to 8K; water that a 3x3 opening removes at 16K (channels 1-2 px wide:
fjords, sounds, estuaries) is found separately, and where the DEM's footprint reaches the sea byte (15/255) and
the nearest water body is the sea, the 8K field is raised by a Gaussian bump (K_CHANNEL 0.12 texel, sigma 0.8
texel, gain 2) so the channel stays water. A smooth bump rather than a hard texel value, because the shore ramp
is 0.25 x this field and a single raised texel creases it along texel edges. 21,420 texels flip land -> water
worldwide, none the other way; lake levels are untouched.

## History
2026-11-03 (issue_20261030_204632, southern Andes "glitchy blue terrain"): the 2x2 mean dropped Canal
Puyuhuapi's southern arm (~2-3 km wide, present in Natural Earth and in the DEM as sea). It was drawn as flat
0-m land coloured by the day map's sea blue, and the field's shallow negative ridge along the lost channel
(~-0.3 texel, ~1 km) read as a nearby shore, so the shore cap shaved the wall beside it into a texel-stepped
shelf (probe: DEM 835 m drawn at 280 m). Sognefjord was mostly missing for the same reason. An earlier fix
(ce17d62, an early-out from the cap above 375 m) was reverted by the user; it left the lost channel in place and made the height
function step where the DEM crossed 375 m.
