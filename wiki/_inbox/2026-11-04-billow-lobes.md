---
pages: [rendering/clouds/field.md]
code: [shaders/cloud_v2_noise.comp, src/simulations/SatelliteSimCloudsV2.cpp, src/simulations/SatelliteSim.h]
---
## Now
The shape volume's lobe channels (G, B) come from one of three bakes, chosen by "Lobe noise (0 cones, 1 caps,
2 billows)" (`clouds_v2.shape_lobes`, default 2, Weather tab "Rebuild experiments", slot 307) by a descriptor swap:
0 = inverted-Worley cones 1 - F1, 1 = rounded caps, 2 = **billows**. A billow octave is a union of flattened
hemispheres, max_i (R_i^2 - d_i^2)^0.3 with R 0.7-1.0 cells; G holds 4/8/16/32 cells, B 8/16/32, amplitude x0.6 per
octave, each channel remapped to the caps' mean / spread (so the field's calibration and the clouds' placement hold).
Every level set is round-topped lobes meeting in sharp V creases. The march code and its cost are unchanged; the
third bake is +9 MB of video memory.

The billows were fitted to the Disney WDAS cumulus (`.refs/wdas_cloud`, CC BY-SA 3.0, used as a reference only):
on one envelope at matched amplitude, the surface's mean-curvature skew is -0.48 (WDAS -0.32, caps +0.78, cones
+1.53), its median lobe radius 0.036 H (WDAS 0.018 H, caps 0.061 H), its surface per volume 0.073 (WDAS 0.096, caps
0.062). The WDAS cloud itself: flat interior density 0.5 under a thin denser shell, an edge that reaches the
interior density within ~0.006 H, a flat base, lobe sizes with about equal volume per octave of radius. Lobes finer
than ~60 m (on a 1 km cloud) are below the 128^3 shape volume's 14.8 m texel: the detail volume's erosion has them.

## History
Replaces "Cap lobes (0/1)" (`shape_caps`, 2026-10-22) as the default; the key changed so saved settings with caps on
move to billows. Tried and dropped: overlapping hemispheres R 0.9-1.25 (few creases, skew +1.78), a cascade putting
finer caps on the coarser crowns (+0.57..+1.36). Perf: cloud_march A/B in one live app (RTX 3070 Ti, 1600x900, the user's settings, 3 interleaved reps, two other agents' apps on the GPU, two contended reps dropped): caps -> billows 8.30 -> 8.42 ms (level flight at 1.5 km), 9.24 -> 9.35 (3 km looking down), 10.63 -> 10.89 (ground), 8.67 -> 8.66 (6 km over Darwin); caps-vs-caps spread 0.3-0.8 ms: neutral within the noise, the march SPV is unchanged.
