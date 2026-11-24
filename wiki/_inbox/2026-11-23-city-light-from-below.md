---
pages: [rendering/clouds/march.md, rendering/cities.md]
code: [shaders/cloud_v2_march.comp, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
City light reaches the volumetric clouds from below: the night map (mip 3, ~39 km, the light as the air spreads it)
under each cloud sample enters the cloud's base and climbs through it, as direct light falling off with the depth
down to the base plus the slab's diffuse field (that depth toward the light, the depth up to the top beyond it, the
view's angle to the light below). Bases are lit most, a lit cloud glows through when seen from above, and the look
changes with the viewing angle. The level is set on the host (`cv2.grain.y`) so the clouds' mean level matches the
old term's at a side-on ground view; no field reads, same cost.

## History
Until 2026-11-23 city light was a flat term on the lower 45% of each column (issue_20261123_192207, "looks very
basic"). A height-dependent blur of the night map was tried and dropped: offshore decks lost the coast's light.
