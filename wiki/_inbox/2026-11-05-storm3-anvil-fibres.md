---
pages: [rendering/clouds/storms.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
With storms through the cumulus field the anvil plate is fibrous spreading ice: its storm lobes only bulge (the lid
keeps 0.25 of them, `clouds_v2.anvil_lid_lobes`; the underside 0.7, `anvil_under_lobes`: gentle domes and pouches),
it keeps 0.2 of the Cb erosion ("Anvil erosion", `anvil_erosion`), its skin ramps in over 3x the storm surface's ramp,
and its density is combed by `cv2AnvilFibre`: the shape volume's Perlin + Worley read in absolute drifted longitude /
latitude, stretched along the wind (`anvil_fibre_along_km` 15, the period divides the equator; `anvil_fibre_across_m`
1500), meandered, 0.55-1.1 inside and opening into wisps toward the skin ("Anvil fibres", `anvil_fibres` 1, 0 = the
old eroded plate). The veil beyond the plate reads its fray from one fetch of the same fibre volume (4x wider across
the wind) instead of three. Towers take the cumulus bubbles and the turrets' skew ("Storm bubbles", `storm_bubbles` 1),
off the plate and the overshooting domes.

## History
The plate was carved by the cumulus' lobes (+-1.5 km) and Cb erosion, which holed a sheet a few hundred metres thick:
"very eroded swiss cheese slabs" (the user, 2026-11-05). Audit at storm views (knockouts, cloud march ms): the veil
was -7.7 of 26 ms at issue 111947's eye (11 km) and -4.7 of 32 from 30 km; the tower/shield walk -4..-10; the storm
detail fetch ~0. Measured before/after (RTX 3070 Ti, contended, cloud march ms): issue 111947 at 11 km 22.2 -> 16.4; a storm from 30 km at
sunset 29.2 -> 31.6; an anvil edge-on from 10 km 15.1 -> 16.1; congestus 11.1 -> 11.2. Towers: the bubbles carve and
bulge at most ~400 m past the storm surface (added freely they stood as a debris of detached puffs round the towers).
