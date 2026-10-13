---
pages: [rendering/clouds/field.md, rendering/clouds/march.md, rendering/clouds/temporal.md, rendering/sea.md, using/graphics-settings.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_march.comp, shaders/sat_sky.frag, shaders/sky_taa.comp, src/simulations/SatelliteSimUI.cpp]
---
## Now
A convective cell's top varies across a field ("Cumulus height variation", `clouds_v2.cumulus_height_variation`, 0.6:
by region and by fine cell, floor 0.12 of the type's span). A stratiform type other than stratus is shaped as cells
where coverage is low (0.25-0.65), so a broken deck is shallow cumulus, not a lens. The lobes are read at a fixed height
in a cloud's lowest part (flat-bottomed puffs) and carve a shallow cloud in proportion to its strength ("Cumulus
break-up", `cumulus_breakup`, 0.6). The strength needed at a cell's top is 1 + 0.5 x "Cumulus top headroom"
(`cumulus_top_headroom`, 0.5), so tops are not cut flat. The lobes lift a convective cloud's upper surface by up to
900 m x their strength, capped at 0.7 of the cell's thickness. Deep convection and the Cb towers carry the cumulus-scale
lobes on the storm-scale ones ("Storm cumulus fine lobes" `storm_cumulus_fine_lobes` 0.5; "Cb tower fine lobes"
`cb_fine_lobes` 0.5, a 500 m displacement at 1). The density ramp steepens with 1 / the cell's strength about the
erosion's cut level, and the edge sharpening is up to x3 below an 80 m footprint. "Cumulus wisps" (`cumulus_wisps`,
0.5) frays weak cells and parts of formed clouds. "Silver lining" (`silver_lining`, 0.5) adds a narrow forward lobe
(g 0.92, weight 0.12 x the setting) to single scattering in water cloud. "Tower top" ranges 0.3-0.95.
The lobe occlusion dims multiple-scattered sunlight as well as sky light; sky light under thick cloud loses its hue with
the diffuse transmission above it. The sky TAA's "Storm sea trails" apply only where the weather map rains and fade
out over 1-4 px of eye parallax.

The view march's coarse strides are 1.5-2.5 steps each (a per-step golden sequence off the ray's jitter), which removes
the contour rings on smooth anvil heads and tower walls; the four per-visit jitters step by the R4 constants. Convective
clouds thicker than ~1.2-3.5 km take the big lobes (the storm volume at 2x its coordinates) at 0.7.

## History
User issues 20261012_235712 (pancake clouds, lobeless storm cumulus, smooth towers, muddy near clouds, clouds dragged
over a stormy sea in fast flight) and 20261012_235159 (blue lobes / warm creases under an overcast at a low Sun).
The lobes used to be damped to 5-30% at the base and full only 700 m up; deep convection read only the 12x storm-scale
noise. The "old cones" (Tower top 0) and the low layer's deep cores (Cb columns 0) were removed: with them the march
compiled to 222 registers instead of 128. GPU total at the nine shots 19.8-22.5 ms (21.2-24.8 before); tstab at shot 5
1.45 -> 1.88 (more relief while moving). Details: CLAUDE.md "Cumulus shape pass (2026-10-13".
