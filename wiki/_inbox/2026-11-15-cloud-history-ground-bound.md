---
pages: [rendering/clouds/temporal.md]
code: [shaders/cloud_v2_resolve.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The cloud resolve reads the scene depth (binding 11, the march's half-res terrain/mesh distance). On a ground pixel
nothing can be farther than the surface, so the pixel reprojects at min(its cloud distance, the ground's distance) and
the eye-parallax floor is capped at the ground's distance instead of 50 km. Moving near a hillside, its history is
therefore clamped to its own new samples like any near pixel's, and clouds seen above a ridge cannot be carried onto the
hill as it rises over them. Sky pixels are unchanged; cost within noise (+0.1 +-0.16 ms GPU at the report's view).

## History
issue_20261115_133245 ("Odd clipping on terrain", 2026-11-15): dropping into a valley at walking pace, the clouds above
the ridge stayed on the hillside as blobs and stair-stepped bands (one per frame of the descent) for ~120 still frames.
The ground pixels reprojected at infinity (or at a far cloud's distance from the sky beside them), so the resolve took
them as still and kept their history unclamped. Terrain crop luminance at the trail's last frame 58.3 -> 38.7 (no
cloud history at all: 38.2).
