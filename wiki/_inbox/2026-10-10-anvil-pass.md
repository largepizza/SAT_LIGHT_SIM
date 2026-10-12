---
pages: [rendering/clouds/march.md, rendering/clouds/field.md, rendering/atmosphere-and-sky.md]
code: [shaders/sat_sky.frag, shaders/cloud_v2_march.comp, shaders/cloud_v2_lightning.comp, shaders/include/cloud_lightning.glsl, shaders/include/clouds_v2.glsl, src/simulations/SatelliteSimCloudsV2.cpp]
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

**Sun shadow cascade.** Clouds anywhere within about 205 km of the eye are shadowed by clouds toward the Sun,
not only those within 3.9 km of the eye's line to the Sun. The cascade is a lattice of 128 x 32 parallel rays
across the Sun's direction (3.2 km apart horizontally, 0.8 km apart at sunset in the other axis, about 10 km at a
45-degree Sun) with 49 knots along the Sun (about 10 km apart at sunset); each knot stores the Sun's transmittance
beyond it. The lattice moves with the clouds and is stored toroidally, so a ray keeps its place while the eye flies.
Half of it is baked on every other frame, alternating with the fine grid, so the bake costs what the fine grid
alone did. It fills in 33 frames after a teleport, a time jump or a settings change and then fades in. A sample
reads it one knot sunward of the end of its own light march. Setting: "Far cloud shadows (0/1)",
`clouds_v2.sun_cascade`, default 1. It is off with the Sun more than 6 degrees below the horizon and with the eye
above 60 km. Limits: blockers 5-15 km sunward of a sample are missed; above a ~15-degree Sun it catches little of
an anvil's shadow on the cloud under it; nothing beyond 205 km.

**Both Sun shadow grids are running means of jittered bakes.** Each bake places a ray's sub-rays at a new
point of its cell and steps at a new phase; the stored transmittance is the mean of the bakes (a new bake weighs
1 / the number so far, at least 1/8). The fine grid's lattice also moves with the clouds and is stored
toroidally; with the cascade on it is baked on even frames and used only within 3-6 km of the eye. Thin ice (the
high layer) casts no shadow in either grid, and a thin-ice sample reads the cascade at the middle of its layer.

**God rays: the clouds' shadows on the air.** The sky pass multiplies each air sample's sunlight by the Sun
shadow cascade's transmittance (the mean of three reads along the step) and lights the shadowed share by the sky,
as it does air in the Earth's shadow. The cloud march reads the same lattice for its key light, so clouds and the
air between them share one Sun shadow. Setting: "God rays" (`clouds_v2.godrays`), 0-1 = how much of the shadow the
air takes, default 1; in the Clouds tab's "Cloud shadows & god rays" section with "Far cloud shadows". Not in the
Planetarium / Potato tiers or in mirror reflections. Cost about 0.7-1.0 ms of the sky pass at 1920x1009. The old
light volume is no longer baked.

## History
Before the refinement a step at 40 km was ~460 m against a lit skin of tens of metres: each ray was sunlit or
self-shadowed by chance (grain at the sparse grid's 4-px scale, the user's "grid" on anvils) and ~17% too dark
against a 25-m-step reference. Snap 4 high-pass noise 8.3 -> 2.6 levels, still flicker std 1.25 -> 0.78, tstab
2.03 -> 1.79. Dropped: no distance gate (+3.4 ms over near cumulus), 2 bisections for short steps (a hard-edged
darker oval), shading at view optical depth 0.7 from the density slope (stripes, 222 registers).
The lobes used the wind anchor: one direction in the drifted ECEF frame through towers that stand still in it.
At 25 N 134 W that direction is 56% downward, so the lobes sank down the columns.
Before the cascade, a tower 20 km away stood sunlit behind a storm that shaded it, and the shadow appeared on
flying to within 4 km (the fine grid's reach). With it on, the cloud march measures 0.3-0.8 ms lower in still
views and the same while moving; tstab pitch 1.14 -> 0.86, rise 1.27 -> 1.19, walk 2.94 -> 2.58.
With fixed sub-rays a ray knew a cloud top's height to a quarter or half cell; at a 0.5-degree Sun that
is 7-40 km along the ground, and shadow edges came out as stairs. On the second set of snapshots: cloud march
13.0 ms with the cascade vs 14.9 without; tstab pitch 0.81 -> 0.49, rise 1.52 -> 1.32, walk 1.73 -> 1.56.
The first god rays (an experimental option) scaled a pixel's cloud transmittance by the shadowed share of its
airlight, read from a 6-km light volume. The sky pass dims only the air behind a pixel's cloud distance, so clear
pixels were never dimmed and any pixel with a faint veil was dimmed in full: a darker sheet over the high layer's
haze with a stair-stepped outline.
