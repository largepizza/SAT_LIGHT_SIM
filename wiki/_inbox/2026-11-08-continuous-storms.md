# Continuous storms (storm_continuous)

- Storms no longer have column/anvil-plate geometry by default. A catalog storm steers the weather map (type toward
  cumulonimbus, more coverage) over a wide envelope (~3.5x its cells' radius), and the cumulus field draws it as its own
  deep cloud. Towers come from a thresholded noise (cluster Perlin at 16 and 64 km + coarse cells), so they rise and
  fade continuously and ride the wind with the rest of the texture.
- The map's own dense deep convection (ITCZ, monsoon, afternoon land) can now reach the storm range, so stormy regions
  last as long as the map's systems (hours), not a catalog cell's life.
- Anvils are the high-cloud layer: a dense, flat-topped ice sheet under the tropopause wherever the convection cube's
  anvil field (or strong cores) says so; shaped by the layer's noise, lumpy underside, fibrous edge.
- Setting: clouds_v2.storm_continuous (1 = on, default; 0 = the previous column + plate storms).
- Source: docs/subsystems/weather.md "Continuous storms"; shaders cloud_v2_weather.comp, include/clouds_v2.glsl.

## Later the same night (2026-11-08)

- Each storm grows its own anvil: the bake writes the storm's wide envelope (and a hurricane's CDO) into the convection
  cube's anvil channel; the high-cloud layer draws a dense ice sheet there (optical depth ~20-30 at its core), its top at
  the tropopause (a little higher where it is thick), a lumpy underside (mammatus-like), fibrous cirrus at its edge.
  The catalog's anvil integral still spreads it downwind and lets it linger.
- Storm systems stick around: each storm's envelope fades over its anvil's linger after it stops feeding, so a rained-out
  storm leaves a decaying cloud mass instead of vanishing with its last tower.
- Hurricanes: the CDO is the anvil (bands are not); eyewall and core are typed cumulonimbus, bands towering congestus;
  towers under the CDO are capped below the shield except in the eyewall; the eye is cut from every layer.
- The storms' mid-level outflow is the mid layer's altostratus (a squall line's trailing shield, a hurricane's decks).
- Rendering fixes found on the way: the convection cube's read-back lag uses the rectangle bake's time (anvils ticked every
  ~40 sim s in time-lapse); the Sun shadow volume's lattice moves with the low wind (shadows no longer lag their clouds in
  time-lapse: tower view at 5 min/s ghost tiles 5.2 -> 0.7%); the march's far-side optical depth blends into its path
  estimate over a footprint range (no lighting ring round the nadir from altitude).
- Life cycle: a storm's towers rise as the catalog's cells grow (their drawn tops, over the tower growth time), and its
  anvil forms only once they reach the tropopause, then spreads; the catalog's lingering outflow thins faster, and the
  anvil's thickness follows Settings > Weather > Storms "Anvil thickness". Settings toggle: "Continuous storms (0 = columns)".
- Correction: the map's own deep convection is NOT raised into storm cloud (tried; whole tropical fields turned into a grey
  blocky sheet from orbit). Storm persistence comes from each storm's lingering system envelope.
