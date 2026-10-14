---
pages: [rendering/clouds.md, using/settings.md]
code: [shaders/include/cv2_layer_map.glsl, shaders/cloud_v2_layers.comp, shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The mid layer (altocumulus / altostratus) and the high layer (cirrus) cast shadows on the clouds below them at any distance,
through a layer shadow map. It is a 2D map fixed to the clouds (a gnomonic plane anchored in the drifting cloud frame), two
levels of 256 x 256 cells (1 km near; 6 km, growing to 48 km from orbit). A first pass stores each cell's vertical optical depth
of each layer (mean and variance, plus the layer's height and thickness), read finely so altocumulus and cirrus keep their true
optical depth. A second pass integrates each layer along the Sun ray through every cell, with a gamma model so broken
altocumulus lets the right amount of light through. A cloud sample looks the map up where its own light march ends: the light
march counts the nearby layer, the map the rest. At a high Sun the map adds nothing. Settings: "Layer shadows (mid / high)"
(`clouds_v2.layer_shadows`, 1) and "Cirrus self-shadow" (`clouds_v2.cirrus_self_shadow`, 0), section "Cloud shadows & god
rays". Cost about 0.2 ms of GPU time.

## History
Until 2026-10-14 the layers' shadows lived only in the light march (~8 km) and the far cascade; when the near shadow lattices
carried the mid layer, they drew a grey disc around the observer that followed them (issue 20261013_192258), so they were
removed from there, and a sheet then shaded nothing beyond the light march at a low Sun. With the map on, the cascade drops the
mid layer as well, which also removed fake sunbeam wedges along the horizon (the cascade's mid-layer shadow on the air).
Validated against a brute-force march of the cloud field along the Sun ray (map softens the sheet's edge over a few km).
