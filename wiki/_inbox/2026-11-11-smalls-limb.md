---
pages: [rendering/atmosphere-and-sky.md]
code: [shaders/include/sky_ap.glsl]
---
## Now
In the limb volume (the aerial-perspective grid around the Earth's limb, `clouds_v2.sky_ap_limb` 1), a pixel reads
only the rows on its own side of the limb. A ground pixel holds the last ground row and a sky pixel holds the first
sky row. Neither side extrapolates or blends across the limb.

## History
2026-11-11, issue_20261111_170539 "Line on horizon 2": the sky side extrapolated its first two rows, chosen on
2026-11-10 to remove a ~4-level step at 111 km. From orbit toward a low Sun, the cloud tops at the limb sit in that
last row, and the extrapolation drew a 1-px dark line there (-160 levels, -35 at a second shot). Holding the first row
removes the line and brings back the +4-level step at the 111-km view.
