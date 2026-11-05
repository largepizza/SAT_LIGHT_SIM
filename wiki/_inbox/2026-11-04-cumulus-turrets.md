---
pages: [rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSimUI.cpp]
---
## Now
Cumulus turrets: every low cloud with a convective share (cumulus fully, stratocumulus by half; not stratus, not the
Cb storm objects) adds a 3D tower field to its margin: the storm shape volume read at 2x (octaves 2.9 km, 1.4 km,
710 m, 360 m), read slower upward (`cumulus_turret_stretch` 0.5: columns taller than wide), leaned downwind with
height (`cumulus_turret_lean` 2.0 x the cells' "Lean"), its positive excursions grown (`cumulus_turret_skew` 1.0:
few tall turrets, many low). Strength `cumulus_turrets` 1.0 ("Cumulus turrets", Settings > Clouds; 0 = off). On a deck
type the turrets rise out of the sheet up to ~1.2 km over the type's top. The small billow lobes, the flat bases and
the lighting are unchanged. Cost within the noise (march at 128 registers).
## History
Issue 20261104_181014 ("every cloud a gumdrop", 4.7 km over a shower field): the field was a 2D strength under one
vertical profile per cell plus lobes <= 475 m, and the shower field was typed between stratocumulus and cumulus, so
it was shaped mostly as a lumpy deck. A deck-top term written into the deck profile put the march at 250 registers;
placed after the top height it is 128. A baked voxel / SDF shape volume (Nubis^3-style) was evaluated and not built:
see the resolution note.
