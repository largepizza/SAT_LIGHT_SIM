---
pages: [using/weather.md, simulation/weather.md, development/harness.md]
code: [shaders/weather_map_view.comp, src/simulations/SatelliteSimWeatherCyclones.cpp, src/simulations/SatelliteSimHarness.cpp]
---
## Now
The Weather window's Cyclones map can overlay the weather map the volumetric clouds are drawn from: the weather cube's
stored channels, reprojected from its six faces onto the map's equirect box, one at a time, under the cyclone tracks.
Rows under the map: "Weather map" Off / Cover / Type / Rain / Ground, "Convection" Cells / Tops / Anvil / Outflow,
"Opacity" 25% / 50% / 75% / 100% / Map only, "Mip" Auto / 0-4 (0 = the 2048-px faces, ~5 km; Auto matches the map's
pixels, ~3 for the world). It reads through the same direction mapping and cube lookup as the cloud march (no
recomputation), so what it shows is what the clouds draw: Cover is the low-cloud coverage remapped as the clouds remap it,
Type the stratus .. cumulonimbus classification, Rain the precipitation, Ground the terrain height cloud bases follow, and
the Convection layers the storm cube (cell intensity, column top, anvil field, mid-level outflow). Mid and high cloud have
no layer: they are derived from Cover in the march. A GPU pass (~512 x 256 pixels) runs only while a layer is shown, at
most 4 times a second. Harness: `weather map <off|cover|type|rain|ground|cells|tops|anvil|outflow> [opacity=] [only=]
[lod=] [zoom=]`.

## History
Added 2026-11-06 for in-app checks of the Weather v2 synthesised map and its layers. First check (2036-09-10 16:00 UTC):
the Ground layer's coasts land on the base map's, no cube-face seams, the ITCZ at ~0-10 N, hurricane eyes on their
catalog markers.
