---
pages: [rendering/atmosphere-and-sky.md, "new: rendering/rocket-exhaust.md", development/controls-and-settings.md]
code: [src/simulations/SatelliteSimPlumeBricks.cpp, shaders/plume_brick.comp, src/simulations/SatelliteSimLaunches.cpp, shaders/cloud_march.comp]
---
## Now
A launch's staging cloud (the hot-staging lobes and the exhaust from ~10 s before staging to ~90 s after it, weighted
smoothly by emission time) is drawn as a small volume ("plume brick") instead of a sum of Gaussians: its Gaussians are
splatted into a 64^3 grid per brick, then each pixel marches the grid with the clouds' billow lobes (the same
WDAS-matched cauliflower noise the cumulus use, anchored at the staging point), a light march toward the Sun and the
plumes' multiple scattering. Near a twilight Sun (within ~35 deg) its thin ice shows iridescence: each wavelength's
diffraction (Airy) pattern for ice 0.25-0.75 um, in pastel patches. The result joins the plume composite as one more
plume. Settings (settings.json only, no slider yet): `launches.plume_bricks` (true), `launches.plume_brick_lobes` (1,
0 = smooth), `launches.plume_iridescence` (1). Up to 4 bricks; one wider than ~150-250 km hands back to the Gaussians.
Cost at the report's view: +0.4 ms GPU.

## History
2026-11-15, issue_20261115_165031 "Staging billow cloud" (the reporter asked for cumulus-style lobes and iridescence).
The hot-staging lobes alone turned out optically thin (tau ~3e-4 two minutes on); the visible "billow" is the upper
stage's early exhaust. Bugs found on the way: one mean colour for a brick lit the booster's reddened trail like sunlit
ice (fixed with a per-voxel colour); a per-frame dither shimmered 2.6x the Gaussians' noise (fixed per texel); the
brick's descriptor named a destroyed scene-depth view after a resize, so sky rays missed every brick.
