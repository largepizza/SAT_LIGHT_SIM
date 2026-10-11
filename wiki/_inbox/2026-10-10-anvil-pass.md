---
pages: [rendering/clouds/march.md, rendering/clouds/field.md]
code: [shaders/cloud_v2_march.comp, shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
**Surface refinement (view march).** When a fine sample is opaque over its own step (extinction x step >= 1), the
sample before it was not, the ray still has transmittance > 0.1 and the step is longer than 2-3.5 base steps
(about 7-14 km from the eye at the defaults, dithered per ray), the march bisects the crossing three times with
unlit field reads and shades the bracket's inner end. Distant towers and anvils are shaded at their surface, not
at a random depth inside it. Cost +0.4-1.1 ms still, +1.0-1.3 ms moving (RTX 3070 Ti, 1920x1009).

**Cb tower lobes flow.** The towers read the storm shape volume at a wind-free anchor, displaced against a flow
that runs up the stalk and, in an anvil-reaching tower's head, outward from its axis. Two phases half a cycle
apart are cross-faded, so the displacement never exceeds 0.35 of the volume's period. Speed 1.5 x "Wind (m/s)";
one cycle is about 11 minutes at an 8 m/s wind and a 22.8 km storm period. The towers' erosion detail is
wind-free. The low layer's lobes still slide along the global wind vector.

**Limit: long-range cloud shadows.** A cloud sample's own light march reaches about 2 x "light length" (5 km).
Blockers beyond that are seen only by the Sun shadow grid, within 3-3.9 km of the eye's line to the Sun. A cloud
farther from that line is not shadowed by a distant storm, so the shadow appears as the eye approaches.

## History
Before the refinement a step at 40 km was ~460 m against a lit skin of tens of metres: each ray was sunlit or
self-shadowed by chance (grain at the sparse grid's 4-px scale, the user's "grid" on anvils) and ~17% too dark
against a 25-m-step reference. Snap 4 high-pass noise 8.3 -> 2.6 levels, still flicker std 1.25 -> 0.78, tstab
2.03 -> 1.79. Dropped: no distance gate (+3.4 ms over near cumulus), 2 bisections for short steps (a hard-edged
darker oval), shading at view optical depth 0.7 from the density slope (stripes, 222 registers).
The lobes used the wind anchor: one direction in the drifted ECEF frame through towers that stand still in it.
At 25 N 134 W that direction is 56% downward, so the lobes sank down the columns.
