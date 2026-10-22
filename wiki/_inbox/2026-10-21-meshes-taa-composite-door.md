---
pages: [rendering/satellite-meshes.md, rendering/launch-visuals.md, simulation/rockets.md]
code: [shaders/sky_taa.comp, shaders/sat_sky.frag, shaders/taa_depth_restore.frag, shaders/scene_depth.comp, shaders/lightning.frag, shaders/cloud_march.comp, tools/make_rocket_models.py, src/simulations/SatelliteSimLaunches.cpp]
---
## Now
- Sky TAA and things that move on their own: meshes (satellites, rockets, pads) and texels lit only by the plume /
  flame Gaussians are marked in the TAA input (alpha 0.875); the resolve stores their depth negated, and wherever the
  3x3 or the history's 2x2 holds one it clips the history and depth-tests it even when the view is still. A moving
  rocket leaves no trail or translucent ghost.
- Meshes and the half-res cloud / fog composite: a half-res texel is clamped to a mesh when any pixel of it or the
  one-pixel ring round it is mesh; a mesh pixel tests each of its four texels' composite distance against its own
  (sat_sky.frag's joint-bilateral upsample), the engine-light pass lights cloud / fog on a mesh pixel only in front of
  it, and a resolved v2 cloud whose distance is past the texel's surface is dropped.
- Starship's payload door is a steel panel over a black slot: closed, and sliding up open only from 90 s before the
  first satellite's release to 60 s after the last (20 s each way).

## History
Issue 20261021_174219 (a Starship landing at its tower at night, render scale 0.67): with TAA on the descending ship
was a dotted translucent ghost (its history, the sky, passed the still view's widened depth test and was kept
unclipped); TAA off, catch-tower trusses had a dark half-res staircase along their edges and odd squares in the
lattice (one filtered composite distance averaged near fog with a far cloud bank; texels wholly inside lattice holes
were marched past the tower), and a hull flown past at 86 m/s carried the cumulus behind it as blocky dark patches
(cloud v2 resolve history). The door slot used the bells' "soot" material, which glows white-hot while the engines
burn: a bright band on every landing ship, read as "the hatch is always open". A first attempt that clamped only
texels the mesh covers whole made the trusses worse (members lost the fog in front of them) and was reverted.
