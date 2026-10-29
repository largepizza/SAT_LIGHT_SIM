---
pages: [rendering/hardware-tiers.md]
code: [src/VulkanContext.cpp, src/simulations/SatelliteSimCloudsV2.cpp, THIRD_PARTY_NOTICES.txt]
---
## Now
The per-stage storage-image limit counts every binding of a pipeline layout that the stage can see, not only what a
shader reads. The clouds v2 march set (shared by the march, tiles, lightning, froxel, far-layer and clipmap passes)
has six storage images: the march's colour and depth at sparse and full rate, the far cloud layer and the Sun / sky
colour table's bake target. The app needs `maxPerStageDescriptorStorageImages` >= 6, two past the guaranteed 4, and
the device check names it when a GPU reports fewer. Desktop GPUs report thousands and MoltenVK at least 8. The sky
set has four.

THIRD_PARTY_NOTICES.txt also credits the derived files: the skyglow sources (from the night map), the Milky Way glow
(from the ESO panorama), the launch-site coasts (Natural Earth) and the GEO belt and lunar base sites (public sources
listed in each data file).

## History
2026-10-29 (v1.2 release audit). The device check asked for 4 storage images and the floor table said the march's
four outputs; the march set's layout had grown to six without the check following.
