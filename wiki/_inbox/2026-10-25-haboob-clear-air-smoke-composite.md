---
pages: [rendering/clouds/weather.md, rendering/clouds/field.md, rendering/launch-visuals.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimHaboob.cpp, shaders/cloud_march.comp, shaders/cloud_v2_combine.comp]
---
## Now
The clear air ahead of a haboob's gust front removes the low cloud up to the dust walls' highest top and fades out
over the next 400 m (`kCv2DustClearFadeM`), that height displaced +-720 m by the wall's own lobe volume; the clearing's
lateral ramp is displaced by the same lobes, so its edges read as raised, lumpy cloud bases rather than cut planes. The
grid's clear air is held back within 3.5-6.5 km of any tornado on the ground (or within 5 min of it), so the funnel
keeps the storm's base to hang from. Wildfire smoke and haboob dust Gaussians (the aerosol range) take their density at
the ray's closest approach by height above the ground instead of above their centre's tangent plane: a flat pall
hundreds of km wide follows the Earth's curve instead of rising off the limb. Where smoke dominates the plumes in front
of a cloud, the cloud alpha's distance (the sky pass's air split) is the opacity-weighted mean of the smoke's distance
and the cloud's, at half and full resolution.

## History
Issues 20261025_184550 and _185309. The clear air was read only below the walls' top (~3.5 km) and cleared everything
under it: a horizontal slice through every cloud there ("odd horizon band"), and the tornado probe hung the funnel from
the slice (3.5 km base, in open sky). A pall's flat tangent plane stood ~80 km up 1000 km out: a dark band over the limb
from 119 km. The air split at a far cloud's distance added the air between smoke and cloud undimmed: light-blue blocky
holes in the pall. Letting the curved-sheet correction also raise density drew solid columns (rays grazing a flat sheet
for hundreds of km): it only thins. Cost within noise in the report views; the haboob Watch view +0.8-1.0 ms of cloud
march (the deck behind the old slice is drawn). Commits 8ab4658, 7246ba5.
