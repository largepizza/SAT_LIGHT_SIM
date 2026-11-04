---
pages: [rendering/clouds/storms.md, rendering/clouds/field.md]
code: [shaders/include/clouds_v2.glsl, shaders/include/cv2_storms.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
---
## Now
With "storms through the cumulus field" on (`clouds_v2.storm_unified` 1, no UI; UBO `cv2.stormU`), a storm object's
towers and anvil shield are not density functions of their own. `cv2StormGeom` walks the point's tower bin (the same
radius profile: base, waist, flared head under the lid, overshooting dome, lean, head drift, turrets) and the strongest
shield (a lens under the lid: its outline where the shield's presence passes 0.5, thickness "Anvil thickness"
x (presence - 0.5) / 0.5 + "Anvil hang" under a head) for a MARGIN in metres, with no noise fetches. `cv2FieldLow` (split into
`cv2LowA`, the cumulus' carve scalars, and the carve itself) divides it by `storm_unified_scale_m` (4000 m per margin
unit), joins it to the cumulus margin as a smooth max and blends the carve scalars toward the storm's where it
dominates (the Cb type's density and erosion, storm-scale lobes, a surface ramp of `storm_unified_ramp_m` 250 m or
1.5 footprints): ONE set of billow-lobe and billow-erosion fetches shapes cumulus, towers, heads and the anvil plate,
so a storm is the same material as the cumulus around it. The plate's lid keeps 40% of the lobes. The switch is the march's specialization constant 4 (a change rebuilds the
cloud pipelines); the other shaders read the uniform. The shield's thin
veil beyond the plate (frayed outline, a cyclone's outflow cirrus) is still `cv2AnvilSigmaP`'s, added as thin ice
(coarse steps, no light march). Storm objects, cyclones, squall lines, the weather stamp, rain cores, lightning and
the anvil formation / decay timing are unchanged. 0 = the old towers (`cv2ColumnSigma`) and shields.

## History
Issue 20261104_111947 "Anvil performance" (11 km near an anvil at sunset, cloud march 94.5 ms): 36 of ~38 shaded
samples a ray were the old shield's translucent haze (density x presence^2 at "Anvils" 0.16), each a fine step and a
full field read; knocking the shields out took 74 -> 31 ms. Towers and cumulus overlapped at 0.007 samples a ray
there: the "double dip" was not the cause. Cloud march (RTX 3070 Ti, 1920x1009, user settings): the report 49.0 -> 28.9 ms
still, 57.8 -> 36.7 on its trail; supercell from orbit 37.2 -> 34.6; a plate seen from 19 km 23.6 -> 26.7; hurricane from
orbit and the 020257 storm unchanged. An outline at presence 0.35 put the reporter's eye inside a plate of fog.
