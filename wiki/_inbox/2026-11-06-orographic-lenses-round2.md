---
pages: [rendering/clouds/layers.md, rendering/clouds/field.md, rendering/clouds/lighting.md]
code: [shaders/include/clouds_v2.glsl, shaders/cloud_v2_march.comp]
---
## Now
Orographic lenses are occasional, finite and lit like the cloud around them (issues 20261105_201331, 20261105_201521):
- Wave-cloud days: the condensation lift a lens needs is lowered by "lens moisture" (`oro_moist`) only where a regional
  gate is open: two slow waves (~1500 km, drifting west, turning over in ~3 days) open it over roughly a fifth to a third
  of the land at a time. Elsewhere the dry air's 1400 m is past what any relief lifts the layer (the lift saturates near
  1.5 km): no lenses. A place sees lenses on some days and none on most.
- Along a crest the lens is broken by the moisture the wind carries through the standing wave: the cluster Perlin
  (8-64 km, +-225 m of the condensation lift), the mid layer's shape volume (km scale, +-220 m) and the 4-32 km Perlin of
  the closed-cell volume, which both shifts the threshold (+-300 m) and cuts the lens outright where it is low. Each lens
  is a set of separate almonds 10-20 km long with ragged ends, whose thickness swells and thins with the cloudlets' lobes
  (x0.75-1.25); its texture is the same shape fetch at x2.2 contrast (floor 0.4), its density x3.5 the altocumulus'.
- The lift reads the relief at DEM mip 1.2 over the mip-2.8 massif (~6 km over ~19 km, was 5 km over 13 km); the low
  cloud parts round an envelope of the mip-1 relief + 1.5 x its excess over that massif (was the 2.7 km DEM + 0.8 x):
  the gap's roof over a ridge follows the range's smooth shape, not an inverted V along the crest line.
- Lighting: the cloud march's light march and its march away from the light do not see the terrain terms (they are
  computed once, at the main sample: 128 registers; kept live across the light march, 254), so a lens had ~0 optical
  depth on both sides and the diffuse field shaded it as a thin wisp: flat, grey-blue, darker than the cumulus beside
  it (median luminance 160 against the cumulus' 210, blue/red 1.21 against 0.99). A lens sample now flags itself (a
  negative deck share) and takes its own path to its top along the light and to its base away from it: median 215,
  blue/red 1.00.

## History
2026-11-06 (lens-a) the lenses were the mid layer condensing wherever the lift passed a moisture threshold that a single
regional wave moved by +-50%: with the user's lens moisture 0.75 every range had smooth, even ribbons tens of km long,
all the time, lit flat blue-grey (issue 20261105_201331), and the low cloud's gap over a ridge had a sharp V roof
(issue 20261105_201521).
