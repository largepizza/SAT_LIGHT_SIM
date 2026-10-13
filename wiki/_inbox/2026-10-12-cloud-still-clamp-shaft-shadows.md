---
pages: [rendering/clouds.md, rendering/cloud-lighting.md, using/graphics-settings.md, development/harness.md]
code: [shaders/cloud_v2_resolve.comp, shaders/cloud_v2_march.comp, shaders/cloud_v2_lightning.comp, shaders/include/cloud_lightning.glsl, src/simulations/SatelliteSimCloudsV2.cpp, tools/harness/reports.py]
---
## Now
**Resolve.** In a still view (less than 0.05-0.5 half-res px of parallax) the cloud resolve clamps the history
against the range of the 7x7 new samples around the pixel; in motion it is the 3x3 box with variance clipping, as
before. A still, half-shadowed cloud surface under a grazing Sun converges to the mean of its lit and shadowed
samples (0.4 levels of grain at the benchmark view).

**Far Sun shadow on cloud.** A cloud sample's Sun shadow beyond its own light march comes from three lattices, the
finest that covers it: the fine grid (32 x 32 rays at 250 m, within 3-6 km of the eye), the shaft lattice's two
rings (128 x 128 rays each, read where the sample's light march ends), and the cascade (rays 3-12 km apart) beyond.
A ring reaches 56 cells from the eye across the Sun's direction: ring 1 at 250 m cells about 14 km, ring 2 at
1000 m cells about 56 km. The rings' rays march the cloud field themselves all the way out: through their 32 knots,
then 16 quadratically spaced steps to the fine grid's ray length (at most 400 km). They are baked whenever "Far
cloud shadows" (`clouds_v2.sun_cascade`) is on, 512 rays of each per frame (a ray every 32 frames); the sky's god
rays read ring 1.

Settings (Weather tab, "Cloud shadows & god rays"): "Cloud shadow ring 1 cell (m)" (`clouds_v2.shadow_ring1_cell_m`,
250, 100-1000), "Cloud shadow ring 2 cell (m, 0 = off)" (`shadow_ring2_cell_m`, 1000, 0-4000), "Cloud shadow ring 2
depth (x)" (`shadow_ring2_depth`, 0.5: ring 2's knot spacing along the Sun as a multiple of ring 1's x the cells'
ratio). Larger cells reach further and soften the shadows' edges; the cost does not change with the cell.

"Accumulation" (`clouds_v2.sampler_accumulation`, default 32) matters for how fast a view cleans up after a reset
or a stop: at 0 with "History weight" 0.02 the grain is 15 levels after 40 frames and 3 after 240; at 32 it is 0.8
after 40.

Harness: a `report load` path in a script uses forward slashes (`reports.py show` prints it that way).

## History
User issues 20261012_202341 and 20261012_195811 (Sun 0.1-0.9 deg up, "Light march footprint" 0, "Accumulation" 0).
- The still-view box was 3x3. With all-or-nothing samples, a frame whose nine samples agreed had a zero-width box
  and the history's mean was clamped to that one value. Which frames agree depends on the 64-texel blue-noise tile
  around the pixel, so the tile was drawn over the surface: the same glyph every 128 px (autocorrelation 0.85 at
  that lag), 9.3 levels of grain, 16 levels darker than the true mean. Tried: a slack of 0.75 x the box mean (4.8
  levels), a 5x5 box (1.8), no clamp when still (0.4, but a moving beam shaft would trail). 7x7: 0.4. Cost within
  noise.
- The shaft lattice was read only by the sky (god rays), baked only with "God rays" > 0, and each ray started
  from the cascade's value at the window's sunward end. The cascade's cells are 0.8-1.4 km in the direction that
  resolves height at a grazing Sun, where a far cloud top's shadow is a layer a few hundred metres deep: it read
  lit where the fine grid read full shadow. So a cloud shaded by a distant one was shadowed only inside the fine
  grid, within 3-6 km of the eye, and the shadow appeared on flying up to it. Isolated with three march variants
  (cascade only; + shaft lattice; the fine grid without its distance fade). Cost within noise (43-45 ms GPU either
  way at the test view); the march stays at 128 registers.
- Ring 2 was added the same day, on the user's request for more reach with tunability: GPU frame 29.9-30.0 ms with
  it off, at 1000 m and at 2000 m; shadows hold from 15 and 30 km back at the test view. Beyond ring 2 the
  cascade's limit remains.
