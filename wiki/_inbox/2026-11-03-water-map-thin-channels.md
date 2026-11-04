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
is 0.25 x this field and a single raised texel creases it along texel edges.

The bake also dunks sea-level land the day map paints as sea: land texels (by the coastline) where the DEM byte is
the sea's, the nearest body is the sea (within 3 texels) and the 8K day map is sea blue ((b - (r+g)/2) / luminance
> 0.22) get the same bump (K_DUNK 0.25). Green polders and deltas at DEM 0 stay land. Together 51,056 texels flip
land -> water worldwide, none the other way; lake levels are untouched.

The shore ramp (land within 1.5 km of the shore capped to 0.25 m per m) no longer flattens steep shores: within
800 m of the shore the land is never lowered by more than hS (the DEM at this point's shore, above the water level,
found down the distance field's gradient), faded to 0 at 800 m; past that the DEM stands. A flat coast never
reaches this (the ramp is already above it), so beaches and polders are drawn as before.

The sky shader finishes what the 8K bake cannot: ground at the sea's level that the day map paints sea blue is drawn
as the sea, and land above it whose day-map texel is sea blue takes the least-blue of eight texels around it (2 and
4 texels away). Both act only amid relief (the DEM's ~20 km mean above 100-200 m), so flat coasts keep their map.

## History
2026-11-03 (issue_20261030_204632, southern Andes "glitchy blue terrain"): the 2x2 mean dropped Canal
Puyuhuapi's southern arm (~2-3 km wide, present in Natural Earth and in the DEM as sea). It was drawn as flat
0-m land coloured by the day map's sea blue, and the field's shallow negative ridge along the lost channel
(~-0.3 texel, ~1 km) read as a nearby shore, so the shore cap shaved the wall beside it into a texel-stepped
shelf (probe: DEM 835 m drawn at 280 m). Sognefjord was mostly missing for the same reason. With the channel back,
the plain cap flattened the restored fjord's real walls (937 m drawn at 338 m), hence the shore-cliff rule. At the
report's shot: ground samples > 100 m below the DEM 43.5% -> 1.7%.
An earlier fix (ce17d62, an early-out from the cap above 375 m) was reverted by the user; it left the lost
channel in place and made the height function step where the DEM crossed 375 m. At the shot, sea-level ground drawn as land went 18.1% -> 0.0% of the terrain and sea-blue land above
the sea 82% -> 0.2%; a first, unguarded shader dunk flooded the Dutch dunes at Zandvoort and was gated by relief.
