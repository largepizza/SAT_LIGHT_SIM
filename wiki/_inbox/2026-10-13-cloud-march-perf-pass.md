---
pages: [rendering/clouds/march.md, rendering/clouds/lighting.md, development/profiling.md, development/gpu-conventions.md]
code: [shaders/cloud_v2_march.comp, shaders/cloud_v2_sunlut.comp, shaders/include/cv2_sunlut.glsl, shaders/cloud_march.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
The cloud march works in two loops per batch of 32 samples: the first walks the ray on the density field alone
(coarse steps, the fine lattice, the surface refinement, the transmittance and depth statistics) and keeps the
samples that hold cloud; the second lights the kept samples (a light march of seven steps, a far sample and a sky
probe each: nine more field evaluations). A GPU runs a group of 32 rays in lockstep, so an iteration costs what
its most expensive ray costs; with the two loops an empty step costs one field evaluation for the whole group,
and the light march runs only for samples that need it.

The Sun's colour and the zenith sky's radiance at a cloud sample come from a table (`cloud_v2_sunlut.comp`,
`include/cv2_sunlut.glsl`): a 256 x 76 two-layer RGBA16F array indexed by the cube root of the Sun's zenith cosine
and by height in 800 m levels (0-60 km). It is baked when "Cloud sunlight Rayleigh"
(`clouds_v2.cloud_sun_rayleigh`) or the sky's Rayleigh / Mie gains change. It occupies march set bindings 13
(storage) and 14 (sampled).

Where a view sample's pixel footprint is more than twice the first light step (from about 100 km of eye altitude
at the defaults), the light steps merge in pairs: four samples cover the lengths of seven. The ground's cloud
shadow is not marched where a half-res texel and its eight neighbours lie behind cloud that transmits under 1%.

Cloud bucket at 1920x1009 on an RTX 3070 Ti (430 iterations, 7 light steps, full rate above 30 km): orbit storm
from 388 km 38 ms, 58 km 22 ms, anvils at sunset from 8.6 km 9.2 ms, cumulus from 6 km 9.2 ms. Near the ground
about 5 ms of the bucket is not the march (composite, resolve, shadow lattice bakes, ground shadow), and the sky
pass (10-18 ms) is the larger cost.

## History
2026-10-13, commits c622aa2 and 73cf44b. Before: orbit storm 78 ms, 58 km 38-48, anvils 17, cumulus 16.3. In the
single loop a ray in empty air waited out its neighbours' light marches, so the time was the longest ray's step
count times a lit sample's cost. The table replaced a per-ray cache recomputed at every 800 m level a ray crossed
(a 12-step zenith integral; one ray refreshing stalled its group): 16-18% of orbit views. Images are within
run-to-run noise of the old ones at ten views, except anvil lids under a grazing Sun, which read about 2 levels
lighter with the merged light steps (a 25-m view step reads the same; twelve light steps read darker).

Tried and dropped: one sample per batch (slower on overcast), early termination of opaque rays (1-4%), a view
step floor at 0.2 x the pixel footprint (-17% from orbit but contour rings on anvil lids), three rewrites of the
Cb tower search (each moved the driver from 128 to 215-255 registers and ran slower), and `DontInline` patched
into the SPIR-V for the field functions (3-4x slower).

The light volume (`cloud_v2_lightvol.comp`, 2026-09-29, unbaked since the god rays moved to the Sun shadow
cascade) is deleted; the table took its two bindings.
