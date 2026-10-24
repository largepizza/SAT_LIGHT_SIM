---
pages: [rendering/clouds/march.md, rendering/clouds/field.md, development/profiling.md]
code: [shaders/include/clouds_v2.glsl (cv2FieldLow, cv2ColumnSigma), shaders/include/cloud_lightning.glsl (cv2StvT), shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
The low cloud field returns before any texture read wherever a sample lies above the highest top any weather type can
have there: an exact bound computed on the CPU each frame (the largest top over every type mix at the tropopause
factor's two ends, interpolated), plus the ground, plus the storm cumulus target only within reach of a Cb tower (and
there with its real value once the cluster field is read). The Sun transmittance volume is read from its finest level
out and stops once the finer levels cover the point. Under an anvil at 12.4 km (1920x1009, RTX 3070 Ti) the cloud march
takes ~35 ms (was ~39), from 6.6 km ~13.3 ms (14.0), at Medium from 12.4 km ~18.6 ms (21.1); the image is unchanged.

"Near cirrus stride" (Weather tab, Rebuild experiments; `clouds_v2.near_cirrus_stride`, 1 = off, the default) lets a pure
thin-ice sample within a 12-m pixel footprint stand for 2-4 view steps: with the eye inside the high layer its near haze
is most of the march (12.4 km: 34 -> 27 ms at 2, 25.5 at 4). Opt-in, because a dense cloud starting inside a stride is
entered up to ~2 steps late.

## History
2026-10-24 (issue 20261023_230322, "Storm performance"): the old low-field test took the storm cumulus target's largest
value (Storm cumulus top x (1 + variation), 21.6 km at the reporter's 12 km x 1.8), so every sample in the cirrus /
anvil band read the cell, smooth-weather and ground textures before returning nothing. Tried and dropped the same day:
finer bins and bounding spheres for the anvil shields' index, unrolled shield loops, lazy record loads, height pre-tests
in the high and anvil layers, a slice cache for the sky's god-ray froxels, `glslc -O` (none measurable, or slower).
