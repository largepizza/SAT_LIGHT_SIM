---
pages: [rendering/clouds/march.md, rendering/atmosphere-and-sky.md]
code: [shaders/sat_sky.frag]
---
## Now
A background sample that hits no surface but has opaque cloud in front writes that cloud's distance as its depth
(`tCloudOcclude`, sat_sky.frag; the sky TAA marks it alpha 0.8). The distance is taken PER TEXEL of the half-res
cloud target, never from the filtered alpha (a signed distance in km: opaque >= 0, else minus the mean distance or
-60000 for none): each texel's gathered alpha counts as opaque when it is over 1 m, the sample is opaque when those
texels hold at least half of the bilinear weight, at their weighted distance. Where a surface edge crosses the
half-res grid, the texels are the joint-bilateral's (those whose scene depth matches this sample's surface), so a
sky sample beside a ridge or the horizon takes the sky-marched texels' answer, not the ground-marched ones'.
Everything the main pass depth-tests after the sky (rain and snow particles, stars, satellites) sees that depth.

## History
2026-11-23 (wave 20261122 c28): standing in heavy rain with the sky TAA on, 1-px dark dashes ran along the far
horizon (Madagascar supercell, from inside its rain). A jittered sample that missed the ground on a horizon pixel
read the ground-marched texels' opaque distance (the rain light's mean distance, a few hundred m) through the
filtered alpha, wrote it as depth, and the rain particles (the near rain's veil) were depth-rejected there. Dark
pixels in a 1920x300 horizon crop 860-1600 -> 0-19 per frame; GPU within noise (+0.08 ms), tstab not worse.
